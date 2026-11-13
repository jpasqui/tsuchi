#include "mods.h"
#include "mod_choices.h"

#include <string.h>

#include "files.h"
#include "file_service.h"
#include "host.h"
#include "list.h"
#include "mod_service.h"
#include "tsuchi/core.h"

namespace tsuchi {

const char* const kModsFolder = "rom:/tsuchi/mods";

namespace {

List<ModRecord> records; // by ID
// Parsed manifests stay private until saved activation and order are known.
// In particular, a Settings host must not see candidates that cannot run yet.
List<ModRecord> pending_records;
bool pending_scanned = false, finding = false;
uint64_t discovery_generation = 0;
// Searches answered busy because the saved choices could not be read yet. See DESIGN.md: at the bound, the mods
// run in the default order instead of never starting.
constexpr uint32_t kChoicesWaits = 300;
uint32_t choices_waits = 0;
bool lifecycle_active = false, forget_pending = false;
List<ModRuntime> runtimes;
List<ManifestSection> sections;
struct KeyOff { const char* key; const char* family; };
List<KeyOff> keys_off;
void (*before_start)(ModRecord&) = nullptr;
void (*log_sink)(const char*, const char*) = nullptr;
uint32_t lines_logged = 0;

// A native declaration or cleanup can reenter the loader. Keep records alive until its callback returns,
// refuse nested preparation/startup, and let a requested forget invalidate the remainder of this phase.
struct Lifecycle {
    uint64_t generation = discovery_generation;
    Lifecycle() { lifecycle_active = true; }
    ~Lifecycle() {
        lifecycle_active = false;
        if (forget_pending) { forget_pending = false; forget_mods(); }
    }
    bool current() const { return generation == discovery_generation && !forget_pending; }
};

void stop_context(ModRecord& record) {
    TSUCHI_Mod* context = record.context;
    record.context = nullptr;
    record.prepared = false;
    if (context) mod_stop(context);
}

// A prepared dependent can be blocked later, when its provider's code fails. Unwind in the reverse selected
// order, with the pointers cleared before callbacks so a nested refusal cannot stop the same context twice.
void stop_blocked_contexts() {
    // A release callback can refuse another context whose place was already visited in this pass.
    for (bool stopped = true; stopped;) {
        stopped = false;
        for (uint32_t place = records.count; place--;)
            for (uint32_t i = 0; i < records.count; ++i)
                if (records[i].context && records[i].order == place &&
                    records[i].state != ModState::ready && records[i].state != ModState::running) {
                    stop_context(records[i]);
                    stopped = true;
                }
    }
}

// ---- Small tools --------------------------------------------------------------------------------

// A version is three numbers with points between them. A requirement may be "*" instead, for any version.
bool read_version(const char* text, uint32_t parts[3]) {
    for (int i = 0; i < 3; ++i) {
        if (*text < '0' || *text > '9' || (*text == '0' && text[1] >= '0' && text[1] <= '9')) return false;
        uint32_t value = 0;
        for (; *text >= '0' && *text <= '9'; ++text) {
            if (value > 99999999) return false;
            value = value * 10 + uint32_t(*text - '0');
        }
        parts[i] = value;
        if (*text != (i < 2 ? '.' : '\0')) return false;
        ++text;
    }
    return true;
}

// Sets a record's problem from up to four parts, and gives false so a caller can return it.
bool blocked(ModRecord& record, const char* a, const char* b = "", const char* c = "", const char* d = "") {
    const char* parts[] = {a, b, c, d};
    size_t bytes = 1;
    for (const char* part : parts) bytes += strlen(part);
    record.state = ModState::blocked;
    if (!record.problem.reserve(bytes)) return false;
    char* at = record.problem.data;
    for (const char* part : parts) {
        const size_t length = strlen(part);
        memcpy(at, part, length);
        at += length;
    }
    *at = 0;
    return false;
}

// A whole number as text.
const char* number_text(uint32_t value, char buffer[12]) {
    char* at = buffer + 11;
    *at = 0;
    do *--at = char('0' + value % 10);
    while (value /= 10);
    return at;
}

void forget(ModRecord& record) {
    record.name.clear(), record.version.clear(), record.problem.clear();
    forget_settings(record.settings); // before the manifest they are declared in
    release_json(record.manifest);
    record = ModRecord{};
}

// The keys a manifest may have at its top. Anything else is a mistake its author should hear of.
bool known_key(const char* key) {
    const char* const every[] = {"id", "name", "version", "framework", "game", "author", "description", "url", "meta",
                                 "requires", "optional", "conflicts", "settings"};
    for (const char* known : every)
        if (!strcmp(key, known)) return true;
    for (uint32_t i = 0; i < sections.count; ++i)
        if (!strcmp(key, sections[i].key)) return true;
    return false;
}

// What a mod asks of another: the other's ID and the lowest version of it that will do.
struct Need {
    const char* id;
    const char* version;
};
// The requirements of one kind a manifest lists: an object of IDs to versions. Each is checked as it is read.
bool well_formed_needs(ModRecord& record, const char* key) {
    const Json* needs = record.manifest->find(key);
    if (!needs) return true;
    if (needs->kind != Json::Kind::object) return blocked(record, "mod.json: \"", key, "\" is an object of mod IDs, each with the lowest version that will do");
    for (const Json* need = needs->first; need; need = need->next) {
        uint32_t parts[3];
        if (!valid_mod_id(need->name) || !strcmp(need->name, record.id)) return blocked(record, "mod.json: \"", key, "\" names something that is no other mod's ID: ", need->name);
        if (need->kind != Json::Kind::text || (strcmp(need->text, "*") && !read_version(need->text, parts)))
            return blocked(record, "mod.json: the version asked of ", need->name, " is three numbers, such as 1.0.0, or * for any");
    }
    return true;
}

} // namespace

bool read_manifest(ModRecord& record, const char* text, size_t bytes) {
    JsonError error{};
    record.manifest = read_json(text, bytes, &error);
    if (!record.manifest) {
        if (!error.line) return blocked(record, "mod.json: ", error.what);
        char line[12], column[12], where[48] = "mod.json, line ";
        const char* const parts[] = {number_text(error.line, line), ", column ", number_text(error.column, column), ": "};
        char* at = where + strlen(where);
        for (const char* part : parts) {
            const size_t length = strlen(part);
            memcpy(at, part, length);
            at += length;
        }
        *at = 0;
        return blocked(record, where, error.what);
    }
    const Json& manifest = *record.manifest;
    if (manifest.kind != Json::Kind::object) return blocked(record, "mod.json is an object, in braces");
    const Json* id = manifest.find("id"), *name = manifest.find("name"), *version = manifest.find("version"), *framework = manifest.find("framework");
    // The version of the framework it needs comes first: a mod made for a later framework may well have keys this
    // one does not know, and what its author should hear is that the framework is too old.
    if (framework) {
        char needed[12], running[12];
        if (framework->kind != Json::Kind::number || !framework->whole || framework->integer < 0 || framework->integer > UINT32_MAX)
            return blocked(record, "mod.json: \"framework\" is the lowest version of the framework it runs on, a whole number");
        if (framework->integer > int64_t(TSUCHI_API_VERSION))
            return blocked(record, "it needs version ", number_text(uint32_t(framework->integer), needed), " of the framework, and this is version ", number_text(TSUCHI_API_VERSION, running));
    }
    for (const Json* member = manifest.first; member; member = member->next) {
        for (uint32_t i = 0; i < keys_off.count; ++i)
            if (!strcmp(member->name, keys_off[i].key))
                return blocked(record, "mod.json uses ", member->name, ", and this build of the game has that family disabled: ", keys_off[i].family);
        if (!known_key(member->name)) return blocked(record, "mod.json has a key the framework does not know: ", member->name);
    }
    // What the mod list shows of it. \"meta\" is for tools: the framework reads past it, whatever it holds.
    const char* const shown[] = {"author", "description", "url"};
    for (const char* key : shown) {
        const Json* said = manifest.find(key);
        if (said && (said->kind != Json::Kind::text || strlen(said->text) != said->bytes)) return blocked(record, "mod.json: \"", key, "\" is text");
    }
    // A mod that only works with some builds of the game says which, by key or by version.
    if (const Json* game = manifest.find("game")) {
        const auto names_this_build = [](const Json& said) {
            return !strcmp(said.text, game::table::build_key) || !strcmp(said.text, game::table::version);
        };
        const auto well_formed = [](const Json& said) { return said.kind == Json::Kind::text && said.bytes && strlen(said.text) == said.bytes; };
        const char* const shape = "mod.json: \"game\" is the build of the game it is for, such as 1.1.1, or a list of them";
        bool matched = false;
        const char* first = nullptr;
        if (game->kind == Json::Kind::list) {
            if (!game->first) return blocked(record, shape);
            for (const Json* each = game->first; each; each = each->next) {
                if (!well_formed(*each)) return blocked(record, shape);
                if (!first) first = each->text;
                matched = matched || names_this_build(*each);
            }
        } else {
            if (!well_formed(*game)) return blocked(record, shape);
            first = game->text;
            matched = names_this_build(*game);
        }
        if (!matched) return blocked(record, "it is for version ", first, " of the game, and this is version ", game::table::version);
    }
    if (!id || id->kind != Json::Kind::text || strcmp(id->text, record.id)) return blocked(record, "mod.json: \"id\" has to be the folder's name, ", record.id);
    if (!name || name->kind != Json::Kind::text || !name->bytes || strlen(name->text) != name->bytes) return blocked(record, "mod.json: \"name\" is missing: the name people see");
    if (!version || version->kind != Json::Kind::text || !read_version(version->text, record.parts))
        return blocked(record, "mod.json: \"version\" is three numbers with points between them, such as 1.0.0");
    if (!record.name.set(name->text) || !record.version.set(version->text)) return blocked(record, "there is no memory for it");
    if (!well_formed_needs(record, "requires") || !well_formed_needs(record, "optional")) return false;
    if (const Json* conflicts = manifest.find("conflicts")) {
        if (conflicts->kind != Json::Kind::list) return blocked(record, "mod.json: \"conflicts\" is a list of mod IDs");
        for (const Json* other = conflicts->first; other; other = other->next)
            if (other->kind != Json::Kind::text || !valid_mod_id(other->text) || !strcmp(other->text, record.id)) return blocked(record, "mod.json: \"conflicts\" is a list of other mods' IDs");
    }
    Text wrong;
    if (!declare_settings(record.settings, manifest.find("settings"), wrong)) {
        blocked(record, "mod.json: ", wrong.get()[0] ? wrong.get() : "there is no memory for its settings");
        wrong.clear();
        return false;
    }
    for (uint32_t i = 0; i < sections.count; ++i) {
        const Json* part = manifest.find(sections[i].key);
        if (!part || sections[i].well_formed(*part, wrong)) continue;
        blocked(record, "mod.json: ", wrong.get()[0] ? wrong.get() : "there is no memory to read it");
        wrong.clear();
        return false;
    }
    record.state = ModState::ready;
    record.manifest_valid = true;
    return true;
}

namespace {

// ---- Which can run, and in what order ---------------------------------------------------------

// Whether an installed mod's version will do for what is asked of it: any version, or the same first number and
// nothing lower.
bool satisfies(const ModRecord& installed, const char* asked) {
    uint32_t wanted[3];
    if (!strcmp(asked, "*")) return true;
    read_version(asked, wanted);
    if (installed.parts[0] != wanted[0]) return false;
    return installed.parts[1] != wanted[1] ? installed.parts[1] > wanted[1] : installed.parts[2] >= wanted[2];
}

// Blocks every mod that needs what is not there, cannot run, or is too old, and those that need them in turn.
void block_unmet() {
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t i = 0; i < records.count; ++i) {
            ModRecord& record = records[i];
            if (record.state != ModState::ready) continue;
            const Json* needs = record.manifest->find("requires");
            for (const Json* need = needs ? needs->first : nullptr; need && record.state == ModState::ready; need = need->next) {
                const ModRecord* other = find_mod(need->name);
                if (!other) blocked(record, "it needs ", need->name, ", which is not installed");
                else if (other->state != ModState::ready && other->state != ModState::running) blocked(record, "it needs ", need->name, ", which cannot run");
                else if (!satisfies(*other, need->text)) blocked(record, "it needs ", need->name, " ", need->text);
            }
            changed = changed || record.state != ModState::ready;
        }
    }
}

// Blocks every mod that cannot run beside one that can: one it says it conflicts with, or one it works with only
// from a later version. All of them are found before any is blocked, so the order they are looked at in changes nothing.
void block_clashes() {
    for (uint32_t i = 0; i < records.count; ++i) {
        ModRecord& record = records[i];
        record.clash = nullptr, record.clash_version = nullptr;
        if (record.state != ModState::ready) continue;
        const Json* conflicts = record.manifest->find("conflicts");
        for (const Json* other = conflicts ? conflicts->first : nullptr; other && !record.clash; other = other->next) {
            const ModRecord* there = find_mod(other->text);
            if (there && there->state == ModState::ready) record.clash = other->text;
        }
        const Json* optional = record.manifest->find("optional");
        for (const Json* need = optional ? optional->first : nullptr; need && !record.clash; need = need->next) {
            const ModRecord* other = find_mod(need->name);
            if (other && other->state == ModState::ready && !satisfies(*other, need->text)) record.clash = need->name, record.clash_version = need->text;
        }
    }
    for (uint32_t i = 0; i < records.count; ++i) {
        ModRecord& record = records[i];
        if (!record.clash) continue;
        if (record.clash_version) blocked(record, "it works with ", record.clash, " only from ", record.clash_version);
        else blocked(record, "it conflicts with ", record.clash, ", which is installed");
    }
}

// True when every mod this one starts after has its place already.
bool may_start(const ModRecord& record, uint32_t placed) {
    const char* const kinds[] = {"requires", "optional"};
    for (const char* key : kinds) {
        const Json* needs = record.manifest->find(key);
        for (const Json* need = needs ? needs->first : nullptr; need; need = need->next) {
            const ModRecord* other = find_mod(need->name);
            if (other && other->state == ModState::ready && other->order >= placed) return false;
        }
    }
    return true;
}

constexpr uint32_t kUnplaced = UINT32_MAX;
uint32_t stamp = 0;

// Whether following what a mod starts after leads to `target`.
bool leads_to(const ModRecord& from, const ModRecord& target) {
    const char* const kinds[] = {"requires", "optional"};
    for (const char* key : kinds) {
        const Json* needs = from.manifest->find(key);
        for (const Json* need = needs ? needs->first : nullptr; need; need = need->next) {
            ModRecord* other = find_mod(need->name);
            if (!other || other->state != ModState::ready) continue;
            if (other == &target) return true;
            if (other->seen == stamp) continue;
            other->seen = stamp;
            if (leads_to(*other, target)) return true;
        }
    }
    return false;
}

// The order mods start in: dependencies first, then player order, then ID. Mods that ask for each other in a
// circle have no place in any order, and are blocked. True when any were: what asked for them has to be looked at again.
bool order_mods() {
    uint32_t ready = 0, placed = 0;
    for (uint32_t i = 0; i < records.count; ++i) {
        records[i].order = kUnplaced;
        ready += records[i].state == ModState::ready;
    }
    while (placed < ready) {
        ModRecord* next = nullptr;
        for (uint32_t i = 0; i < records.count; ++i)
            if (records[i].state == ModState::ready && records[i].order == kUnplaced && may_start(records[i], placed) &&
                (!next || mod_choices().rank(records[i].id) < mod_choices().rank(next->id))) next = &records[i];
        if (!next) break;
        next->order = placed++;
    }
    if (placed == ready) return false;
    // Those left over are in a circle, or wait on one that is. Only the first kind is told so.
    for (uint32_t i = 0; i < records.count; ++i) {
        ModRecord& record = records[i];
        ++stamp;
        record.circular = record.state == ModState::ready && record.order == kUnplaced && leads_to(record, record);
    }
    for (uint32_t i = 0; i < records.count; ++i)
        if (records[i].circular) blocked(records[i], "it and another mod each need the other first");
    return true;
}

// ---- The services ------------------------------------------------------------------------------

TSUCHI_Result give_text(const char* text, char* buffer, uint64_t capacity, uint64_t* out) {
    *out = strlen(text) + 1;
    if (capacity < *out) return TSUCHI_CAPACITY;
    memcpy(buffer, text, *out);
    return TSUCHI_OK;
}

ModRecord* record_of(const TSUCHI_Mod* mod) {
    for (uint32_t i = 0; i < records.count; ++i)
        if (records[i].context == mod) return &records[i];
    return nullptr;
}

TSUCHI_Result id(TSUCHI_Mod* mod, char* buffer, uint64_t capacity, uint64_t* out) { return give_text(mod_id(mod), buffer, capacity, out); }

TSUCHI_Result name(TSUCHI_Mod* mod, char* buffer, uint64_t capacity, uint64_t* out) {
    const ModRecord* record = record_of(mod);
    return record ? give_text(record->name.get(), buffer, capacity, out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result version(TSUCHI_Mod* mod, char* buffer, uint64_t capacity, uint64_t* out) {
    const ModRecord* record = record_of(mod);
    return record ? give_text(record->version.get(), buffer, capacity, out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result version_of(const char* other, char* buffer, uint64_t capacity, uint64_t* out) {
    const ModRecord* record = find_mod(other);
    return record && record->state == ModState::running ? give_text(record->version.get(), buffer, capacity, out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result log(TSUCHI_Mod* mod, const char* text) {
    ++lines_logged;
    if (log_sink) log_sink(mod_id(mod), text);
    return TSUCHI_OK;
}

// The version a manifest's "framework" is judged against, and the one a compiled mod's mark is.
TSUCHI_Result framework(uint32_t* out) {
    *out = TSUCHI_API_VERSION;
    return TSUCHI_OK;
}

// ---- The development folder ------------------------------------------------------------------------

struct {
    const char* root = nullptr;
    bool (*begin)() = nullptr;
    void (*end)() = nullptr;
} dev;

// The development folders can be read for as long as one lives, when `open` is true.
struct DevScope {
    bool open;
    DevScope() : open(dev.root && (!dev.begin || dev.begin())) {}
    ~DevScope() {
        if (open && dev.end) dev.end();
    }
    DevScope(const DevScope&) = delete;
    DevScope& operator=(const DevScope&) = delete;
};

// Where a file is in a folder that has one folder for each mod: the mods folder, or the development folders. An
// empty path is the mod's folder itself. False when there is no memory.
bool path_in(Texts& out, const char* root, const char* id, const char* within) {
    if (!out.add(root, "/", id, *within ? "/" : "")) return false;
    if (!*within) return true;
    --out.used; // the terminator goes, and the rest follows
    return out.add(within);
}

// Whether the mod has a development folder now.
bool dev_folder_there(const char* id) {
    if (!dev.root || !game::files_known()) return false;
    Texts where, files, folders;
    bool there = false;
    if (path_in(where, dev.root, id, "")) {
        DevScope scope;
        there = scope.open && game::list_folder(where.data, files, folders) == TSUCHI_OK;
    }
    where.clear(), files.clear(), folders.clear();
    return there;
}

// Calls `use` with where one of a mod's files is: in its development folder first, when `from_dev`, and in its own
// folder when the development folder has nothing at that path.
template <typename Use>
TSUCHI_Result each_place(const char* id, bool from_dev, const char* path, Use&& use) {
    Texts where;
    if (from_dev) {
        if (!path_in(where, dev.root, id, path)) return TSUCHI_NO_MEMORY;
        TSUCHI_Result result = TSUCHI_NOT_FOUND;
        {
            DevScope scope;
            if (scope.open) result = use(where.data);
        }
        where.clear();
        if (result != TSUCHI_NOT_FOUND) return result;
    }
    if (!path_in(where, kModsFolder, id, path)) return TSUCHI_NO_MEMORY;
    const TSUCHI_Result result = use(where.data);
    where.clear();
    return result;
}

bool from_dev(const TSUCHI_Mod* mod) {
    const ModRecord* record = record_of(mod);
    return record && record->dev_folder;
}

// Whether the text names any of the texts.
bool among(const Texts& texts, const char* name) {
    for (const char* text = texts.first(); text; text = texts.after(text))
        if (!strcmp(text, name)) return true;
    return false;
}

// What stands in the way of reading one of a mod's files.
TSUCHI_Result readable(const char* path, bool folder) {
    if (!game::files_known()) return TSUCHI_NOT_READY;
    if (!(folder && !*path) && !game::path_stays_within(path)) return TSUCHI_INVALID;
    return TSUCHI_OK;
}

TSUCHI_Result size(TSUCHI_Mod* mod, const char* path, uint64_t* out) {
    const TSUCHI_Result result = readable(path, false);
    if (result != TSUCHI_OK) return result;
    return each_place(mod_id(mod), from_dev(mod), path, [&](const char* where) { return game::file_size(where, *out); });
}

TSUCHI_Result exists(TSUCHI_Mod* mod, const char* path, bool* out) {
    uint64_t bytes = 0;
    const TSUCHI_Result result = size(mod, path, &bytes);
    if (result != TSUCHI_OK && result != TSUCHI_NOT_FOUND) return result;
    *out = result == TSUCHI_OK;
    return TSUCHI_OK;
}

TSUCHI_Result read(TSUCHI_Mod* mod, const char* path, uint64_t offset, void* buffer, uint64_t capacity, uint64_t* out) {
    const TSUCHI_Result result = readable(path, false);
    if (result != TSUCHI_OK) return result;
    return each_place(mod_id(mod), from_dev(mod), path,
                      [&](const char* where) { return game::read_file_part(where, offset, buffer, capacity, *out); });
}

// The folder's names in the mod's folder, and those of the same folder in its development folder that are not
// among them.
TSUCHI_Result list(TSUCHI_Mod* mod, const char* folder, char* buffer, uint64_t capacity, uint64_t* out) {
    TSUCHI_Result result = readable(folder, true);
    if (result != TSUCHI_OK) return result;
    Texts where, names, folders, dev_names, dev_folders;
    result = path_in(where, kModsFolder, mod_id(mod), folder) ? game::list_folder(where.data, names, folders) : TSUCHI_NO_MEMORY;
    where.clear();
    bool found = result == TSUCHI_OK;
    if (result == TSUCHI_NOT_FOUND) result = TSUCHI_OK;
    if (result == TSUCHI_OK && from_dev(mod)) {
        TSUCHI_Result there = TSUCHI_NOT_FOUND;
        if (!path_in(where, dev.root, mod_id(mod), folder)) there = TSUCHI_NO_MEMORY;
        else {
            DevScope scope;
            if (scope.open) there = game::list_folder(where.data, dev_names, dev_folders);
        }
        where.clear();
        if (there == TSUCHI_OK) {
            found = true;
            for (const char* name = dev_names.first(); result == TSUCHI_OK && name; name = dev_names.after(name))
                if (!among(names, name) && !names.add(name)) result = TSUCHI_NO_MEMORY;
            for (const char* name = dev_folders.first(); result == TSUCHI_OK && name; name = dev_folders.after(name))
                if (!among(folders, name) && !folders.add(name)) result = TSUCHI_NO_MEMORY;
        } else if (there != TSUCHI_NOT_FOUND) result = there;
    }
    if (result == TSUCHI_OK && !found) result = TSUCHI_NOT_FOUND;
    // A folder is told from a file by the slash after its name.
    for (const char* inside = folders.first(); result == TSUCHI_OK && inside; inside = folders.after(inside))
        if (!names.add(inside, "/")) result = TSUCHI_NO_MEMORY;
    if (result == TSUCHI_OK) result = names.give(buffer, capacity, out);
    names.clear(), folders.clear(), dev_names.clear(), dev_folders.clear();
    return result;
}

const file::Backend files_backend{read, size, exists, list};

} // namespace

TSUCHI_Result find_mods() {
    if (finding || lifecycle_active) return TSUCHI_NOT_READY;
    if (!game::files_known()) return TSUCHI_NOT_READY;
    struct Finding { Finding() { finding = true; } ~Finding() { finding = false; } } in_progress;
    TSUCHI_Result result = TSUCHI_OK;
    if (!pending_scanned) {
        forget_mods();
        Texts files, folders;
        result = game::list_folder(kModsFolder, files, folders);
        files.clear();
        if (result == TSUCHI_NOT_FOUND) result = TSUCHI_OK; // no mods folder: no mods
        // Every folder is a mod, by ID order, whatever its manifest turns out to say.
        for (const char* folder = folders.first(); result == TSUCHI_OK && folder; folder = folders.after(folder)) {
            const char* const id = valid_mod_id(folder) ? folder : "?"; // what is no ID is kept under a mark, before all the rest
            uint32_t at = 0;
            while (at < pending_records.count && strcmp(pending_records[at].id, id) <= 0) ++at;
            if (!pending_records.add(ModRecord{})) {
                result = TSUCHI_NO_MEMORY;
                break;
            }
            for (uint32_t i = pending_records.count - 1; i > at; --i) pending_records[i] = pending_records[i - 1];
            ModRecord& record = pending_records[at];
            record = ModRecord{};
            memcpy(record.id, id, strlen(id) + 1);
            if (id != folder) {
                blocked(record, "its folder's name is no ID: ", folder);
                continue;
            }
            if ((strlen(id) >= 7 && !memcmp(id, "tsuchi.", 7)) || !strcmp(id, "tsuchi")) { // the framework's own context is tsuchi.self
                blocked(record, "its folder's name is one the framework keeps for itself: ", folder);
                continue;
            }
            Texts where;
            char* text = nullptr;
            uint64_t bytes = 0;
            const TSUCHI_Result read = where.add(kModsFolder, "/", folder, "/mod.json") ? game::read_whole_file(where.data, text, bytes) : TSUCHI_NO_MEMORY;
            where.clear();
            if (read == TSUCHI_OK) read_manifest(record, text, bytes);
            else blocked(record, read == TSUCHI_NOT_FOUND ? "it has no mod.json" : read == TSUCHI_NO_MEMORY ? "there is no memory to read its mod.json" : "its mod.json could not be read");
            release(text);
        }
        folders.clear();
        if (result != TSUCHI_OK) {
            forget_mods();
            return result;
        }
        pending_scanned = true;
    }
    const uint64_t generation = discovery_generation;
    result = load_mod_choices();
    // The replaceable loader can forget this search while its save session is open.
    if (generation != discovery_generation) return TSUCHI_NOT_READY;
    // A busy save is unresolved selection, however many frames it takes. Missing files are conclusive OK
    // defaults from the loader; damaged/unreadable files keep the existing explicit recovery below.
    if (result == TSUCHI_NO_MEMORY) return result;
    if (result == TSUCHI_NOT_READY && ++choices_waits < kChoicesWaits) return result;
    choices_waits = 0;
    records = pending_records;
    pending_records = {};
    pending_scanned = false;
    if (result != TSUCHI_OK)
        mod_note(kFrameworkLogId, result == TSUCHI_NOT_READY ?
            "the save storage did not open for the Mods menu's choices, so every mod runs in the default order" :
            "the Mods menu's choices in save:/tsuchi/mods.json could not be read, so every mod runs in the default order");
    for (uint32_t i = 0; i < records.count; ++i)
        if (records[i].state == ModState::ready && !mod_choices().enabled(records[i].id)) records[i].state = ModState::disabled;
    block_unmet();
    block_clashes();
    do block_unmet();
    while (order_mods());
    return TSUCHI_OK;
}

namespace {

// Runs a mod's code, if its folder has the file any kind of mod starts from. False, with the reason as the
// record's problem, when the code fails at once.
bool run_code(ModRecord& record) {
    const uint64_t generation = discovery_generation;
    bool lua = false, javascript = false;
    const char* const script_entries[] = {"main.lua", "main.js"};
    for (const char* entry : script_entries) {
        uint64_t size = 0;
        const TSUCHI_Result has = each_place(record.id, record.dev_folder, entry, [&](const char* where) { return game::file_size(where, size); });
        if (generation != discovery_generation || forget_pending) return false;
        if (has == TSUCHI_OK) { if (!strcmp(entry, "main.lua")) lua = true; else javascript = true; }
    }
    if (lua && javascript) return blocked(record, "it has both main.lua and main.js; choose one script language");
    for (uint32_t i = 0; i < runtimes.count; ++i) {
        const ModRuntime runtime = runtimes[i];
        uint64_t size = 0;
        const TSUCHI_Result has = each_place(record.id, record.dev_folder, runtime.entry, [&](const char* where) { return game::file_size(where, size); });
        if (generation != discovery_generation || forget_pending) return false;
        if (has == TSUCHI_NOT_FOUND) continue;
        if (has != TSUCHI_OK) return blocked(record, runtime.entry, has == TSUCHI_NO_MEMORY ? ": there is no memory to start it" : ": it could not be read");
        Text problem;
        const TSUCHI_Result result = runtime.start(record.context, problem);
        if (generation == discovery_generation && !forget_pending && result != TSUCHI_OK)
            blocked(record, runtime.entry, ": ", problem.get()[0] ? problem.get() : "it could not be started");
        problem.clear();
        if (generation != discovery_generation || forget_pending || result != TSUCHI_OK || record.state != ModState::running ||
            !mod_usable(record.context)) return false;
    }
    return true; // a mod may have code of every kind, each started in the order the kinds were added, or none: then it is its manifest and its files
}

// Does what the mod's manifest declares, before any code of the mod's.
bool begin_declared(ModRecord& record, bool before_world) {
    const uint64_t generation = discovery_generation;
    for (uint32_t i = 0; i < sections.count; ++i) {
        const ManifestSection section = sections[i];
        if (before_world ? !section.before_world : record.prepared && section.before_world) continue;
        const Json* part = record.manifest->find(section.key);
        if (!part) continue;
        Text problem;
        const TSUCHI_Result result = section.begin(record.context, *part, problem);
        // A part that gives no reason of its own is told by what it answered. Only no_memory means memory ran out.
        if (generation == discovery_generation && !forget_pending && result != TSUCHI_OK) {
            if (problem.get()[0]) blocked(record, "mod.json: ", problem.get());
            else if (result == TSUCHI_NO_MEMORY) blocked(record, "mod.json: there is no memory for what it declares");
            else blocked(record, "mod.json: \"", section.key, "\" answered ", result_name(result));
        }
        problem.clear();
        if (generation != discovery_generation || forget_pending || result != TSUCHI_OK ||
            record.state != (before_world ? ModState::ready : ModState::running) || !mod_usable(record.context)) return false;
    }
    return true;
}

// Whether everything a mod needs is running, now that it is its turn.
bool needs_running(ModRecord& record) {
    const Json* needs = record.manifest->find("requires");
    for (const Json* need = needs ? needs->first : nullptr; need; need = need->next) {
        const ModRecord* other = find_mod(need->name);
        if (!other || other->state != ModState::running || !mod_usable(other->context))
            return blocked(record, "it needs ", need->name, ", which cannot run");
    }
    return true;
}

extern uint32_t watch_every;
void take_dev_print(ModRecord& record);

// Gives a mod that has its turn its context, in its old place when it has had one, then does what its manifest
// declares and runs its code. A mod prepared before the world keeps its context and does only the rest. False,
// with the mod blocked and nothing of it left, when it cannot start.
bool start_one(ModRecord& record) {
    const uint64_t generation = discovery_generation;
    const auto current = [&] { return generation == discovery_generation && !forget_pending; };
    bool started = false;
    if (needs_running(record)) {
        if (!record.context) {
            record.prepared = false;
            record.context = mod_create(record.id, record.rank);
            if (record.context) {
                record.rank = mod_rank(record.context);
                record.dev_folder = dev_folder_there(record.id);
            }
        }
        if (!record.context) blocked(record, "it could not be started");
        else if (mod_usable(record.context)) {
            record.state = ModState::running;
            record.problem.clear();
            if (record.dev_folder)
                mod_note(record.id, "its files are read from its development folder on the SD card first, tsuchi/dev/ and its ID");
            if (watch_every) take_dev_print(record);
            if (before_start) before_start(record);
            if (!current()) return false;
            started = record.state == ModState::running && mod_usable(record.context) &&
                      begin_declared(record, false) && run_code(record);
            if (!current()) return false;
        }
    }
    // Something the mod asked for as it started may have been refused in a way it cannot run with. That is
    // the reason kept, whatever the mod's own code made of the refusal.
    const char* const refused = record.context ? mod_block_reason(record.context) : nullptr;
    if (refused) blocked(record, refused);
    if (started && !refused && record.state == ModState::running) return true;
    if (record.state == ModState::running || record.state == ModState::ready || record.state == ModState::stopped)
        blocked(record, "it stopped as it started");
    block_unmet();
    stop_blocked_contexts(); // whatever was made for it before it failed goes with it
    return false;
}

// ---- Restarts ----------------------------------------------------------------------------------------

bool (*restart_gate)() = nullptr;

// Whether the mod has had its turn to start in this run: it runs, or it was blocked as it started.
bool had_turn(const ModRecord& record) {
    return record.rank && (record.state == ModState::running || record.state == ModState::blocked || record.state == ModState::stopped);
}

// Whether the mod requires one of those marked with the current stamp.
bool requires_marked(const ModRecord& record) {
    const Json* needs = record.manifest_valid ? record.manifest->find("requires") : nullptr;
    for (const Json* need = needs ? needs->first : nullptr; need; need = need->next) {
        const ModRecord* other = find_mod(need->name);
        if (other && other->seen == stamp) return true;
    }
    return false;
}

// Compiles the entries of the mod's code that have a check, without running them. False, with `problem` set, at
// the first that fails.
bool compiles(const ModRecord& record, Text& problem) {
    for (uint32_t i = 0; i < runtimes.count; ++i) {
        if (!runtimes[i].check) continue;
        uint64_t size = 0;
        const TSUCHI_Result has = each_place(record.id, record.dev_folder, runtimes[i].entry, [&](const char* where) { return game::file_size(where, size); });
        if (has == TSUCHI_NOT_FOUND) continue;
        if (has == TSUCHI_OK && runtimes[i].check(record.id, problem) == TSUCHI_OK) continue;
        if (!problem.get()[0]) problem.set(has == TSUCHI_OK ? "it could not be compiled" : "it could not be read");
        return false;
    }
    return true;
}

void tell_restarted(const ModRecord& record, TSUCHI_RestartOutcome outcome, const char* problem) {
    if (outcome == TSUCHI_RESTART_OUTCOME_RESTARTED) mod_note(record.id, "restarted");
    else {
        Texts line;
        if (line.add(outcome == TSUCHI_RESTART_OUTCOME_KEPT ? "not restarted, and it runs as before: " : "restarted, and blocked as it started: ", problem))
            mod_note(record.id, line.data);
        line.clear();
    }
    mod::emit_restarted({record.id, outcome, problem});
}

// Restarts one mod with every mod that requires it and has had its turn. See DESIGN.md, "Restarting a mod".
// Marks the mod, and every mod that had its turn and requires it, with a new stamp.
void mark_with_dependents(ModRecord& target) {
    ++stamp;
    target.seen = stamp;
    for (bool grew = true; grew;) {
        grew = false;
        for (uint32_t i = 0; i < records.count; ++i) {
            ModRecord& record = records[i];
            if (record.seen == stamp || !had_turn(record) || !requires_marked(record)) continue;
            record.seen = stamp;
            grew = true;
        }
    }
}

// Restarts the mods marked with the current stamp together.
void restart_marked() {
    // Nothing stops unless the entry of every running one compiles, read from where its next start reads it.
    Text problem;
    bool fine = true;
    for (uint32_t i = 0; i < records.count; ++i) {
        ModRecord& record = records[i];
        if (record.seen != stamp) continue;
        record.restart_asked = false;
        const bool before = record.dev_folder;
        record.dev_folder = dev_folder_there(record.id);
        if (fine && record.state == ModState::running && !compiles(record, problem)) fine = false;
        record.dev_folder = before;
    }
    if (!fine) {
        for (uint32_t place = 0; place < records.count; ++place)
            for (uint32_t i = 0; i < records.count; ++i)
                if (records[i].seen == stamp && records[i].order == place) tell_restarted(records[i], TSUCHI_RESTART_OUTCOME_KEPT, problem.get());
        problem.clear();
        return;
    }
    problem.clear();
    // Last started, first stopped; then each starts again in its place.
    for (uint32_t place = records.count; place--;)
        for (uint32_t i = 0; i < records.count; ++i) {
            ModRecord& record = records[i];
            if (record.seen != stamp || record.order != place || record.state != ModState::running) continue;
            stop_context(record);
            record.state = ModState::stopped;
        }
    for (uint32_t place = 0; place < records.count; ++place)
        for (uint32_t i = 0; i < records.count; ++i)
            if (records[i].seen == stamp && records[i].order == place) start_one(records[i]);
    for (uint32_t place = 0; place < records.count; ++place)
        for (uint32_t i = 0; i < records.count; ++i) {
            const ModRecord& record = records[i];
            if (record.seen != stamp || record.order != place) continue;
            const bool running = record.state == ModState::running;
            tell_restarted(record, running ? TSUCHI_RESTART_OUTCOME_RESTARTED : TSUCHI_RESTART_OUTCOME_BLOCKED,
                           running ? "" : record.problem.get()[0] ? record.problem.get() : "there was no memory to keep the reason");
        }
}

TSUCHI_Result restart(const char* other) {
    ModRecord* record = find_mod(other);
    if (!record) return TSUCHI_NOT_FOUND;
    if (record->state == ModState::ready) return TSUCHI_NOT_READY;
    if (!had_turn(*record)) return TSUCHI_UNSUPPORTED;
    record->restart_asked = true;
    return TSUCHI_OK;
}

TSUCHI_Result state(const char* other, TSUCHI_ModState* out) {
    const ModRecord* record = find_mod(other);
    if (!record) return TSUCHI_NOT_FOUND;
    switch (record->state) {
    case ModState::ready: *out = TSUCHI_MOD_STATE_WAITING; break;
    case ModState::running: *out = TSUCHI_MOD_STATE_RUNNING; break;
    case ModState::disabled: *out = TSUCHI_MOD_STATE_DISABLED; break;
    default: *out = TSUCHI_MOD_STATE_BLOCKED; break;
    }
    return TSUCHI_OK;
}

TSUCHI_Result problem(const char* other, char* buffer, uint64_t capacity, uint64_t* out) {
    const ModRecord* record = find_mod(other);
    if (!record) return TSUCHI_NOT_FOUND;
    return give_text(record->state == ModState::blocked ? record->problem.get() : "", buffer, capacity, out);
}

// ---- Watching the development folders -----------------------------------------------------------------

uint32_t watch_every = 0, watch_steps = 0;

void mix(uint64_t& hash, const void* bytes, uint64_t size) {
    const auto* at = static_cast<const unsigned char*>(bytes);
    for (uint64_t i = 0; i < size; ++i) hash = (hash ^ at[i]) * 1099511628211ull;
}

constexpr uint64_t kHashStart = 14695981039346656037ull;
constexpr uint32_t kDevFiles = 256;           // files of a development folder looked at, at most
constexpr uint64_t kDevLargest = 1024 * 1024; // a larger file counts by its path and size alone
constexpr uint32_t kWatchFiles = 8;            // files whose bytes one look reads, at most
constexpr uint64_t kWatchBytes = 256 * 1024;   // and their bytes, at most, past the first file

// The files of a development folder, folders looked into four deep: each one's whole path and size, in the order
// the listings give them. Only the folders are opened.
struct DevFiles {
    Texts paths;
    List<uint64_t> sizes;
    uint32_t count = 0;
    size_t within = 0; // where a path within the development folder starts
    ~DevFiles() { paths.clear(), sizes.clear(); }
};

bool list_dev_files(const char* folder, uint32_t depth, DevFiles& out) {
    Texts files, folders;
    List<uint64_t> sizes;
    const TSUCHI_Result listed = game::list_folder(folder, files, folders, sizes);
    bool ok = listed == TSUCHI_OK;
    uint32_t i = 0;
    for (const char* name = ok ? files.first() : nullptr; name && out.count < kDevFiles; name = files.after(name), ++i) {
        Texts path;
        uint64_t size = i < sizes.count ? sizes[i] : game::kSizeUnknown;
        ok = path.add(folder, "/", name);
        if (ok && size == game::kSizeUnknown && game::file_size(path.data, size) != TSUCHI_OK) size = game::kSizeUnknown;
        ok = ok && out.paths.add(path.data) && out.sizes.add(size);
        path.clear();
        if (!ok) break;
        ++out.count;
    }
    for (const char* name = ok ? folders.first() : nullptr; name && depth; name = folders.after(name)) {
        Texts path;
        if (path.add(folder, "/", name)) list_dev_files(path.data, depth - 1, out);
        path.clear();
    }
    files.clear(), folders.clear(), sizes.clear();
    return listed == TSUCHI_OK;
}

// The mod's development folder now: false when there is none, or the card cannot be read.
bool look_at_dev_folder(const char* id, const DevScope& scope, DevFiles& out) {
    if (!dev.root || !game::files_known() || !scope.open) return false;
    Texts where;
    bool there = path_in(where, dev.root, id, "");
    if (there) {
        out.within = where.used;
        there = list_dev_files(where.data, 4, out);
    }
    where.clear();
    return there;
}

// Its files by path and size, as one number other than 0.
uint64_t shape_of(const DevFiles& files) {
    uint64_t hash = kHashStart;
    uint32_t i = 0;
    for (const char* path = files.paths.first(); path && i < files.count; path = files.paths.after(path), ++i) {
        mix(hash, path + files.within, strlen(path + files.within) + 1);
        mix(hash, &files.sizes[i], sizeof files.sizes[i]);
    }
    return hash ? hash : 1;
}

// Adds the bytes of its files from `from` on to `hash`, each after its path, while the budget lasts: at least one
// file, and no more than `most_files` or past `most_bytes`. Gives the file the next look goes on from.
uint32_t read_dev_files(const DevFiles& files, uint32_t from, uint32_t most_files, uint64_t most_bytes, uint64_t& hash) {
    uint32_t i = 0, read = 0;
    uint64_t bytes = 0;
    for (const char* path = files.paths.first(); path && i < files.count; path = files.paths.after(path), ++i) {
        if (i < from) continue;
        const uint64_t size = files.sizes[i];
        const bool counted = size != game::kSizeUnknown && size <= kDevLargest;
        if (read && (read >= most_files || (counted && bytes + size > most_bytes))) break;
        mix(hash, path + files.within, strlen(path + files.within) + 1);
        if (counted) {
            char* data = nullptr;
            uint64_t got = 0;
            if (game::read_whole_file(path, data, got) == TSUCHI_OK) mix(hash, data, got);
            release(data);
            bytes += size;
        }
        ++read;
    }
    return i;
}

// Notes what the mod's development folder holds now, the whole of it, as a start or a change it restarts for.
void take_dev_print(ModRecord& record) {
    DevScope scope;
    DevFiles files;
    record.dev_print = look_at_dev_folder(record.id, scope, files) ? shape_of(files) : 0;
    record.dev_content = kHashStart;
    if (record.dev_print) read_dev_files(files, 0, kDevFiles, UINT64_MAX, record.dev_content);
    record.dev_partial = kHashStart, record.dev_next = 0;
}

// One look at the mod's development folder: true when it changed since the last.
bool dev_folder_changed(ModRecord& record) {
    DevScope scope;
    DevFiles files;
    const uint64_t shape = look_at_dev_folder(record.id, scope, files) ? shape_of(files) : 0;
    if (shape != record.dev_print) return true;
    if (!shape) return false;
    record.dev_next = read_dev_files(files, record.dev_next, kWatchFiles, kWatchBytes, record.dev_partial);
    if (record.dev_next < files.count) return false;
    const bool changed = record.dev_partial != record.dev_content;
    record.dev_partial = kHashStart, record.dev_next = 0;
    return changed;
}

const mod::Backend mods_backend{id, name, version, version_of, log, framework, restart, state, problem};

} // namespace

void run_restarts() {
    if (lifecycle_active) return;
    Lifecycle phase;
    bool waiting = false;
    for (uint32_t i = 0; i < records.count && !waiting; ++i) waiting = records[i].restart_asked;
    if (!waiting || (restart_gate && !restart_gate())) return;
    // The requests there are now. One made while these run waits for the next step. Each request takes the
    // mods that require it along, and requests that take a mod in common restart together, so each mod
    // starts once, after what it requires.
    List<uint32_t> group; // for each record: 0, or the group it restarts with
    if (!group.reserve(records.count)) return; // tried again next step
    for (uint32_t i = 0; i < records.count; ++i) group.add(0);
    uint32_t groups = 0;
    for (uint32_t r = 0; r < records.count; ++r) {
        if (!records[r].restart_asked) continue;
        mark_with_dependents(records[r]);
        const uint32_t joined = ++groups;
        for (uint32_t i = 0; i < records.count; ++i) {
            if (records[i].seen != stamp) continue;
            const uint32_t earlier = group[i];
            for (uint32_t j = 0; earlier && j < records.count; ++j)
                if (group[j] == earlier) group[j] = joined;
            group[i] = joined;
        }
    }
    for (uint32_t g = 1; g <= groups && phase.current(); ++g) {
        ++stamp;
        bool any = false;
        for (uint32_t i = 0; i < records.count; ++i) {
            if (group[i] != g) continue;
            records[i].seen = stamp;
            any = true;
        }
        if (any) restart_marked();
    }
    group.clear();
}

void set_restart_gate(bool (*ready)()) { restart_gate = ready; }

void set_dev_folder(const char* root, bool (*begin)(), void (*end)()) { dev.root = root, dev.begin = begin, dev.end = end; }

void set_dev_watch(uint32_t steps) {
    watch_every = steps, watch_steps = 0;
    for (uint32_t i = 0; i < records.count; ++i) {
        if (steps && had_turn(records[i])) take_dev_print(records[i]);
        else records[i].dev_print = 0;
    }
}

void watch_dev_folders() {
    if (!watch_every || lifecycle_active || ++watch_steps < watch_every) return;
    watch_steps = 0;
    for (uint32_t i = 0; i < records.count; ++i) {
        ModRecord& record = records[i];
        if (!had_turn(record) || record.restart_asked) continue;
        if (!dev_folder_changed(record)) continue;
        take_dev_print(record); // what it restarts for: a restart that keeps the running code doesn't ask again
        mod_note(record.id, record.dev_print ? "its development folder changed, so it restarts" : "its development folder was taken away, so it restarts");
        record.restart_asked = true;
    }
}

TSUCHI_Result read_mod_file(const char* id, const char* path, char*& data, uint64_t& size) {
    data = nullptr, size = 0;
    if (!game::files_known()) return TSUCHI_NOT_READY;
    const ModRecord* record = find_mod(id);
    return each_place(id, record && record->dev_folder, path, [&](const char* where) { return game::read_whole_file(where, data, size); });
}

bool block_mod(ModRecord& record, const char* a, const char* b, const char* c, const char* d) {
    blocked(record, a, b, c, d);
    block_unmet();
    if (!lifecycle_active) { Lifecycle phase; stop_blocked_contexts(); }
    return false;
}

void prepare_mods() {
    if (lifecycle_active) return;
    Lifecycle phase;
    block_unmet();
    stop_blocked_contexts();
    if (!phase.current()) return;
    // Context ranks must retain the complete selected order, including mods with no early sections.
    for (uint32_t place = 0; place < records.count; ++place) {
        for (uint32_t i = 0; i < records.count; ++i) {
            ModRecord& record = records[i];
            if (record.state != ModState::ready || record.order != place || record.prepared) continue;
            if (!record.context) {
                record.context = mod_create(record.id, record.rank);
                if (record.context) {
                    record.rank = mod_rank(record.context);
                    record.dev_folder = dev_folder_there(record.id);
                }
            }
            if (!record.context) blocked(record, "it could not be prepared");
            else {
                const bool prepared = begin_declared(record, true);
                if (!phase.current()) return;
                const char* refused = mod_block_reason(record.context);
                if (refused) blocked(record, refused);
                if (prepared && !refused && record.state == ModState::ready && mod_usable(record.context)) {
                    record.prepared = true;
                    continue;
                }
                if (record.state == ModState::ready) blocked(record, "it stopped as its manifest was prepared");
            }
            block_unmet();
            stop_blocked_contexts();
            if (!phase.current()) return;
        }
    }
}

void start_mods() {
    if (lifecycle_active) return;
    prepare_mods();
    Lifecycle phase;
    // Places stay as they were given, with gaps where a mod was blocked since.
    uint32_t places = 0;
    for (uint32_t i = 0; i < records.count; ++i)
        if (records[i].state == ModState::ready && records[i].order + 1 > places) places = records[i].order + 1;
    for (uint32_t place = 0; place < places; ++place)
        for (uint32_t i = 0; i < records.count; ++i)
            if (records[i].state == ModState::ready && records[i].order == place) {
                start_one(records[i]);
                if (!phase.current()) return;
            }
}

bool add_mod_runtime(const ModRuntime& runtime) { return runtimes.add(runtime) != nullptr; }
bool add_manifest_section(const ManifestSection& section) { return sections.add(section) != nullptr; }
bool add_manifest_key_off(const char* key, const char* family) { return keys_off.add({key, family}) != nullptr; }
TSUCHI_Result manifest_part(const char* key, TSUCHI_Mod* mod, const Json& part, Text& problem, bool apply) {
    for (uint32_t i = 0; i < sections.count; ++i) {
        const ManifestSection section = sections[i];
        if (strcmp(key, section.key)) continue;
        if (!section.well_formed(part, problem)) return TSUCHI_INVALID;
        return apply ? section.begin(mod, part, problem) : TSUCHI_OK;
    }
    return TSUCHI_NOT_READY;
}
void set_before_start(void (*prepare)(ModRecord&)) { before_start = prepare; }

void forget_mods() {
    if (lifecycle_active) { ++discovery_generation; forget_pending = true; return; }
    Lifecycle phase;
    ++discovery_generation;
    choices_waits = 0;
    pending_scanned = false;
    for (uint32_t i = 0; i < pending_records.count; ++i) forget(pending_records[i]);
    pending_records.clear();
    // Last started, first stopped.
    for (uint32_t place = records.count; place--;)
        for (uint32_t i = 0; i < records.count; ++i)
            if (records[i].context && records[i].order == place) stop_context(records[i]);
    for (uint32_t i = 0; i < records.count; ++i) forget(records[i]);
    release(records.items);
    records = List<ModRecord>{};
}

uint32_t mod_count() { return records.count; }
ModRecord* mod_at(uint32_t index) { return index < records.count ? &records[index] : nullptr; }
ModRecord* find_mod(const char* id) {
    for (uint32_t i = 0; i < records.count; ++i)
        if (!strcmp(records[i].id, id)) return &records[i];
    return nullptr;
}

bool mod_file_path(Texts& out, const TSUCHI_Mod* mod, const char* within) {
    if (*within && !game::path_stays_within(within)) return false;
    if (!out.add(kModsFolder, "/", mod_id(mod), *within ? "/" : "")) return false;
    if (!*within) return true;
    --out.used; // the terminator goes, and the rest follows
    return out.add(within);
}

void set_log_sink(void (*sink)(const char*, const char*)) { log_sink = sink; }
void mod_note(const char* id, const char* text) {
    if (log_sink) log_sink(id, text);
}
uint32_t log_lines() { return lines_logged; }

namespace {

// Writes one line of the framework's own to the log, made of parts. With no memory for it the line is lost, and
// nothing more.
template <size_t count>
void say(const char* const (&parts)[count]) {
    Texts line;
    bool whole = true;
    for (const char* part : parts) {
        if (line.used) --line.used; // the terminator goes, and the rest follows
        whole = whole && line.add(part);
    }
    if (whole) log_sink(kFrameworkLogId, line.data);
    line.clear();
}

} // namespace

void log_mods() {
    if (!log_sink) return;
    uint32_t running = 0, stopped = 0;
    for (uint32_t i = 0; i < records.count; ++i) running += records[i].state == ModState::running, stopped += records[i].state == ModState::blocked;
    char api[12], found[12], run[12], held[12];
    const char* const summary[] = {"api ", number_text(TSUCHI_API_VERSION, api), ", game ", game::table::version, ": ", number_text(records.count, found),
                                   records.count == 1 ? " mod, " : " mods, ", number_text(running, run), " running, ", number_text(stopped, held), " blocked"};
    say(summary);
    for (uint32_t i = 0; i < records.count; ++i) {
        const ModRecord& record = records[i];
        const char* const runs[] = {"mod ", record.id, " ", record.version.get(), " running"};
        const char* const stands[] = {"mod ", record.id, " blocked: ", record.problem.get()[0] ? record.problem.get() : "there was no memory to keep the reason"};
        const char* const waits[] = {"mod ", record.id, " not running"};
        if (record.state == ModState::running) say(runs);
        else if (record.state == ModState::blocked) say(stands);
        else say(waits);
    }
}

bool mod_requirement_satisfied(const ModRecord& installed, const char* version) { return satisfies(installed, version); }

void mods_start() {
    mod::set_backend(&mods_backend);
    file::set_backend(&files_backend);
}

} // namespace tsuchi
