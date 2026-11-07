#include "compiled_mods.h"

#include <string.h>

#include "files.h"
#include "game.h"
#include "host.h"
#include "list.h"
#include "modules.h"
#include "mods.h"
#include "registry.h"

namespace tsuchi {

const char* const kCompiledEntry = "mod.nro";
const char* const kCompiledStart = "tsuchi_mod_start";
const char* const kCompiledBuiltFor = "tsuchi_mod_built_for";

namespace {

namespace t = game::table;
using game::kPage;

// The system's loader. The game is built with only some of its functions, so all of them are found by name in
// the system's library, which the game's own pointer to a file function leads to.
struct Loader {
    uint32_t (*initialize)();
    uint32_t (*register_list)(void* registration, const void* list);
    void (*unregister_list)(void* registration);
    uint32_t (*buffer_size)(uint64_t* out, const void* image);
    uint32_t (*load)(void* module, const void* image, void* buffer, uint64_t buffer_size, int32_t flags);
    uint32_t (*lookup)(uint64_t* out, const void* module, const char* name);
};

bool find_loader(Loader& loader) {
    const uint64_t library = game::module_containing(game::global<uint64_t>(t::data::system_library_anchor));
    const uint64_t found[] = {game::module_symbol(library, "_ZN2nn2ro10InitializeEv"),
                              game::module_symbol(library, "_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv"),
                              game::module_symbol(library, "_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE"),
                              game::module_symbol(library, "_ZN2nn2ro13GetBufferSizeEPmPKv"),
                              game::module_symbol(library, "_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi"),
                              game::module_symbol(library, "_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc")};
    for (uint64_t address : found)
        if (!address) return false;
    loader.initialize = reinterpret_cast<uint32_t (*)()>(found[0]);
    loader.register_list = reinterpret_cast<uint32_t (*)(void*, const void*)>(found[1]);
    loader.unregister_list = reinterpret_cast<void (*)(void*)>(found[2]);
    loader.buffer_size = reinterpret_cast<uint32_t (*)(uint64_t*, const void*)>(found[3]);
    loader.load = reinterpret_cast<uint32_t (*)(void*, const void*, void*, uint64_t, int32_t)>(found[4]);
    loader.lookup = reinterpret_cast<uint32_t (*)(uint64_t*, const void*, const char*)>(found[5]);
    return true;
}

// Whole pages of memory, zeroed, which is what the loader takes.
struct Pages {
    void* block = nullptr;
    uint8_t* at = nullptr;

    bool take(uint64_t bytes) {
        block = allocate(bytes + kPage);
        if (!block) return false;
        at = reinterpret_cast<uint8_t*>((reinterpret_cast<uint64_t>(block) + kPage - 1) & ~(kPage - 1));
        memset(at, 0, bytes);
        return true;
    }
    void give() {
        release(block);
        block = nullptr, at = nullptr;
    }
};

// One compiled mod's code, loaded, and everything the loader was given for it. All of it stays for as long as the
// game runs.
// A mod that does not say which version of the framework it was built for.
constexpr uint32_t kNotSaid = 0xFFFFFFFF;

struct Compiled {
    char id[kModIdBytes];
    uint8_t hash[kSha256Bytes]; // of the file it was loaded from
    Pages image, buffer, list;
    void* handles = nullptr; // the loader's own record of the module, then its record of the list
    TSUCHI_Result (*start)(const TSUCHI_Host*, TSUCHI_Mod*) = nullptr; // null when the code has none
    uint32_t built_for = kNotSaid;                             // the version of the headers it was built against

    void* module() const { return handles; }
    void* registration() const { return static_cast<uint8_t*>(handles) + t::constant::loader_module_bytes; }
    void give() {
        image.give(), buffer.give(), list.give();
        release(handles);
        handles = nullptr;
    }
};
List<Compiled> loaded;
CompiledCounts counts;

Compiled* code_of(const char* id) {
    for (uint32_t i = 0; i < loaded.count; ++i)
        if (!strcmp(loaded[i].id, id)) return &loaded[i];
    return nullptr;
}

TSUCHI_Result refuse(Text& problem, TSUCHI_Result result, const char* why, uint32_t answer = 0) {
    if (answer) counts.refused = answer;
    problem.set(why);
    return result;
}

const char* const kNoMemory = "there is no memory to load it";

// Reads the mod's file whole into pages of its own, and checks that it is a module. The pages are the caller's to
// give back.
TSUCHI_Result read_file(TSUCHI_Mod* mod, Pages& image, uint64_t& bytes, Text& problem) {
    Texts where;
    if (!mod_file_path(where, mod, kCompiledEntry)) return refuse(problem, TSUCHI_NO_MEMORY, kNoMemory);
    uint64_t got = 0;
    TSUCHI_Result read = game::file_size(where.data, bytes);
    if (read == TSUCHI_OK) read = image.take(bytes) ? game::read_file_part(where.data, 0, image.at, bytes, got) : TSUCHI_NO_MEMORY;
    where.clear();
    if (read == TSUCHI_NO_MEMORY) return refuse(problem, TSUCHI_NO_MEMORY, kNoMemory);
    if (read != TSUCHI_OK || got != bytes) return refuse(problem, TSUCHI_IO, "it could not be read");
    if (!game::module_file_valid(image.at, bytes)) return refuse(problem, TSUCHI_INVALID, "it is not a module built for the game");
    return TSUCHI_OK;
}

// Gives the file in `code.image` to the loader. Whatever the loader took before it refused is taken back, and the
// memory is the caller's to give back; once the loader has the module, nothing is.
TSUCHI_Result load(Compiled& code, Text& problem) {
    const char* const not_taken = "the system's loader would not take it";
    Loader loader;
    if (!find_loader(loader)) return refuse(problem, TSUCHI_UNSUPPORTED, "the system's loader was not found");
    // The engine starts the loader the first time it wants it, and notes that it has. The framework does the same.
    if (!game::global<uint8_t>(t::data::loader_state)) {
        if (const uint32_t answer = loader.initialize()) return refuse(problem, TSUCHI_UNSUPPORTED, "the system's loader would not start", answer);
        game::write(game::main_base() + t::data::loader_state, uint8_t(t::constant::loader_started));
    }

    const uint64_t handle_bytes = t::constant::loader_module_bytes + t::constant::loader_registration_bytes;
    code.handles = allocate(handle_bytes);
    if (!code.handles || !code.list.take(kPage)) return refuse(problem, TSUCHI_NO_MEMORY, kNoMemory);
    memset(code.handles, 0, handle_bytes);
    game::write_module_list(code.list.at, t::constant::program_id, code.hash, t::constant::loader_list_wide != 0);
    if (const uint32_t answer = loader.register_list(code.registration(), code.list.at)) return refuse(problem, TSUCHI_UNSUPPORTED, not_taken, answer);

    uint64_t room = 0; // for what the module keeps that starts as zeros
    uint32_t answer = loader.buffer_size(&room, code.image.at);
    const bool memory = !room || code.buffer.take(room);
    if (!answer && memory) answer = loader.load(code.module(), code.image.at, code.buffer.at, room, int32_t(t::constant::loader_bind_now));
    if (answer || !memory) {
        loader.unregister_list(code.registration());
        return memory ? refuse(problem, TSUCHI_UNSUPPORTED, not_taken, answer) : refuse(problem, TSUCHI_NO_MEMORY, kNoMemory);
    }
    uint64_t entry = 0;
    if (loader.lookup(&entry, code.module(), kCompiledStart) == 0) code.start = reinterpret_cast<TSUCHI_Result (*)(const TSUCHI_Host*, TSUCHI_Mod*)>(entry);
    uint64_t mark = 0;
    if (loader.lookup(&mark, code.module(), kCompiledBuiltFor) == 0 && game::readable(mark, sizeof(uint32_t))) code.built_for = *reinterpret_cast<const uint32_t*>(mark);
    return TSUCHI_OK;
}

TSUCHI_Result start(TSUCHI_Mod* mod, Text& problem) {
    Compiled fresh{};
    uint64_t bytes = 0;
    TSUCHI_Result result = read_file(mod, fresh.image, bytes, problem);
    if (result == TSUCHI_OK) sha256(fresh.image.at, bytes, fresh.hash);
    Compiled* code = code_of(mod_id(mod));
    if (result == TSUCHI_OK && code && memcmp(code->hash, fresh.hash, sizeof fresh.hash))
        result = refuse(problem, TSUCHI_BUSY, "its file is not the one its code was loaded from; the game has to be started again");
    if (result == TSUCHI_OK && !code) {
        // Loaded now, and kept from here on. Its place among those that are kept comes first: once the loader has the
        // module there is no giving it back.
        memcpy(fresh.id, mod_id(mod), strlen(mod_id(mod)) + 1);
        Compiled* const place = loaded.add(fresh);
        result = place ? load(*place, problem) : refuse(problem, TSUCHI_NO_MEMORY, kNoMemory);
        if (result == TSUCHI_OK) {
            code = place;
            fresh = Compiled{};
            ++counts.loaded;
        } else if (place) {
            fresh = *place; // with whatever the loader was to be given besides the file
            loaded.remove(place);
        }
    }
    fresh.give();
    if (result != TSUCHI_OK) return result;
    if (!code->start) return refuse(problem, TSUCHI_INVALID, "it has no tsuchi_mod_start");
    // The tables a mod is handed only ever grow at their ends, so a mod built for this version or an older one
    // finds everything where it expects it. One built for a newer version may call what is not there.
    if (code->built_for == kNotSaid) return refuse(problem, TSUCHI_INVALID, "it does not say which version of the framework it was built for; build it with the SDK");
    // Unless its manifest names the lowest version it runs on, which the framework has checked already: then its
    // author has it ask what is there before it uses what a later version added.
    const ModRecord* const record = find_mod(mod_id(mod));
    const bool says_it_runs_here = record && record->manifest && record->manifest->find("framework");
    if (code->built_for > TSUCHI_API_VERSION && !says_it_runs_here)
        return refuse(problem, TSUCHI_UNSUPPORTED, "it was built for a newer version of the framework than this one, and its manifest does not say it runs on this one");
    ++counts.started;
    result = code->start(&host, mod);
    if (result == TSUCHI_OK) return TSUCHI_OK;
    // What the mod's own start answered, by name.
    static const char said[] = "its start answered ";
    const char* const name = result_name(result);
    if (problem.reserve(sizeof said + strlen(name))) {
        memcpy(problem.data, said, sizeof said - 1);
        memcpy(problem.data + sizeof said - 1, name, strlen(name) + 1);
    }
    return result;
}

} // namespace

bool register_compiled_mods() { return add_mod_runtime({kCompiledEntry, start}); }

const CompiledCounts& compiled_counts() { return counts; }

} // namespace tsuchi
