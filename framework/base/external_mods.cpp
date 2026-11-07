#include "external_mods.h"
#include "json.h"
#include <string.h>

namespace tsuchi {
namespace {
void clear_entries(List<ExternalModEntry>& entries) {
    for (uint32_t i = 0; i < entries.count; ++i) {
        entries[i].name.clear(); entries[i].version.clear(); entries[i].problem.clear();
    }
    entries.clear();
}
bool keys(const Json* object, const char* const* names, uint32_t count) {
    if (!object || object->kind != Json::Kind::object || object->count != count) return false;
    for (uint32_t i = 0; i < count; ++i) if (!object->find(names[i])) return false;
    return true;
}
bool text(const Json* value, size_t limit, bool empty = true) {
    if (!value || value->kind != Json::Kind::text || value->bytes > limit ||
        (!empty && !value->bytes)) return false;
    for (size_t i = 0; i < value->bytes; ++i) {
        const uint8_t c = uint8_t(value->text[i]);
        if (c < 0x20 || c == 0x7F ||
            (c == 0xC2 && i + 1 < value->bytes &&
             uint8_t(value->text[i + 1]) >= 0x80 && uint8_t(value->text[i + 1]) <= 0x9F))
            return false;
    }
    return true;
}
bool revision(const Json* value) {
    if (!text(value, 64, false) || value->bytes != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        const char c = value->text[i];
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
    }
    return true;
}
bool format(const Json* value) {
    return value && value->kind == Json::Kind::number && value->whole && value->integer == 1;
}
TSUCHI_Result document(const char* source, uint64_t bytes, const Json** out) {
    JsonError error{};
    *out = read_json(source, size_t(bytes), &error);
    if (*out) return TSUCHI_OK;
    return error.what && !strcmp(error.what, "there is no memory to read it into")
               ? TSUCHI_NO_MEMORY : TSUCHI_INVALID;
}
TSUCHI_Result entry(const Json* source, ExternalModEntry& out) {
    static const char* const fields[] = {"id", "name", "version", "kind", "state", "problem", "manageable"};
    if (!keys(source, fields, 7)) return TSUCHI_INVALID;
    const Json* id = source->find("id");
    const Json* name = source->find("name");
    const Json* version = source->find("version");
    const Json* kind = source->find("kind");
    const Json* state = source->find("state");
    const Json* problem = source->find("problem");
    const Json* manageable = source->find("manageable");
    if (!text(id, kModIdBytes - 1, false) || !valid_mod_id(id->text) ||
        id->bytes < 9 || memcmp(id->text, "external.", 9) || !text(name, 128, false) ||
        !text(version, 64) || !text(problem, 512) || !text(kind, 8, false) ||
        !text(state, 8, false) || manageable->kind != Json::Kind::boolean) return TSUCHI_INVALID;
    if (!strcmp(kind->text, "romfs")) out.kind = ExternalModKind::romfs;
    else if (!strcmp(kind->text, "exefs")) out.kind = ExternalModKind::exefs;
    else if (!strcmp(kind->text, "mixed")) out.kind = ExternalModKind::mixed;
    else if (!strcmp(kind->text, "cheats")) out.kind = ExternalModKind::cheats;
    else return TSUCHI_INVALID;
    if (!strcmp(state->text, "enabled")) out.observed = ExternalModState::enabled;
    else if (!strcmp(state->text, "disabled")) out.observed = ExternalModState::disabled;
    else if (!strcmp(state->text, "conflict")) out.observed = ExternalModState::conflict;
    else if (!strcmp(state->text, "recovery")) out.observed = ExternalModState::recovery;
    else return TSUCHI_INVALID;
    memcpy(out.id, id->text, id->bytes + 1);
    out.manageable = manageable->boolean &&
                     (out.observed == ExternalModState::enabled || out.observed == ExternalModState::disabled);
    out.requested = out.saved = out.observed == ExternalModState::enabled;
    return out.name.set(name->text) && out.version.set(version->text) && out.problem.set(problem->text)
               ? TSUCHI_OK : TSUCHI_NO_MEMORY;
}
TSUCHI_Result catalog(const char* source, uint64_t bytes, List<ExternalModEntry>& staged, char* rev) {
    const Json* root = nullptr;
    TSUCHI_Result result = document(source, bytes, &root);
    if (result != TSUCHI_OK) return result;
    static const char* const fields[] = {"format", "revision", "backend", "mods"};
    const Json* mods = root->find("mods");
    const Json* backend = root->find("backend");
    if (!keys(root, fields, 4) || !format(root->find("format")) ||
        !revision(root->find("revision")) || !text(backend, 9, false) ||
        strcmp(backend->text, "companion") || !mods || mods->kind != Json::Kind::list ||
        mods->count > kExternalModsLimit) result = TSUCHI_INVALID;
    if (result == TSUCHI_OK) {
        memcpy(rev, root->find("revision")->text, 65);
        for (const Json* item = mods->first; item && result == TSUCHI_OK; item = item->next) {
            ExternalModEntry fresh{};
            result = entry(item, fresh);
            if (result == TSUCHI_OK) {
                for (uint32_t i = 0; i < staged.count; ++i)
                    if (!strcmp(staged[i].id, fresh.id)) { result = TSUCHI_INVALID; break; }
                if (result == TSUCHI_OK && !staged.add(fresh)) result = TSUCHI_NO_MEMORY;
            }
            if (result != TSUCHI_OK) {
                fresh.name.clear(); fresh.version.clear(); fresh.problem.clear();
            }
        }
    }
    release_json(root);
    return result;
}
TSUCHI_Result requests(const char* source, uint64_t bytes, ExternalMods& mods) {
    const Json* root = nullptr;
    TSUCHI_Result result = document(source, bytes, &root);
    if (result != TSUCHI_OK) return result;
    static const char* const fields[] = {"format", "revision", "enabled"};
    const Json* enabled = root->find("enabled");
    if (!keys(root, fields, 3) || !format(root->find("format")) ||
        !revision(root->find("revision")) ||
        !enabled || enabled->kind != Json::Kind::object || enabled->count > kExternalModsLimit)
        result = TSUCHI_INVALID;
    const bool stale = result == TSUCHI_OK && strcmp(root->find("revision")->text, mods.revision());
    if (stale) {
        uint32_t manageable = 0;
        for (uint32_t i = 0; i < mods.count(); ++i) manageable += mods.at(i)->manageable;
        if (enabled->count != manageable) result = TSUCHI_INVALID;
    }
    // Validate every request before publishing any request.
    if (result == TSUCHI_OK) for (const Json* value = enabled->first; value; value = value->next) {
        const ExternalModEntry* item = mods.find(value->name);
        if (!item || !item->manageable || value->kind != Json::Kind::boolean ||
            (stale && ((item->observed != ExternalModState::enabled &&
                        item->observed != ExternalModState::disabled) ||
                       value->boolean != (item->observed == ExternalModState::enabled)))) {
            result = TSUCHI_INVALID; break;
        }
    }
    // A complete stale request that agrees with the observed files has no switch
    // differences left. Keep observed defaults; no request is rebased or published.
    if (result == TSUCHI_OK && !stale) for (const Json* value = enabled->first; value; value = value->next) {
        ExternalModEntry* item = mods.find(value->name);
        item->saved = item->requested = value->boolean;
    }
    release_json(root);
    return result;
}
} // namespace
ExternalModEntry* ExternalMods::find(const char* id) {
    if (!id) return nullptr;
    for (uint32_t i = 0; i < count(); ++i) if (!strcmp(entries_[i].id, id)) return &entries_[i];
    return nullptr;
}
const ExternalModEntry* ExternalMods::find(const char* id) const {
    return const_cast<ExternalMods*>(this)->find(id);
}
TSUCHI_Result ExternalMods::load(const char* source, uint64_t bytes, const char* saved, uint64_t saved_bytes) {
    if (!source && !bytes && !saved && !saved_bytes) { clear(); return TSUCHI_OK; }
    if (!source || !bytes || bytes > kExternalCatalogBytes) return TSUCHI_INVALID;
    List<ExternalModEntry> staged;
    char rev[65]{};
    TSUCHI_Result result = catalog(source, bytes, staged, rev);
    if (result != TSUCHI_OK) { clear_entries(staged); return result; }
    clear(); entries_ = staged; memcpy(revision_, rev, sizeof revision_); unread_ = false;
    if (!saved && !saved_bytes) return TSUCHI_OK;
    unread_ = true;
    if (!saved || !saved_bytes || saved_bytes > kExternalRequestsBytes) return TSUCHI_INVALID;
    result = requests(saved, saved_bytes, *this);
    unread_ = result != TSUCHI_OK;
    return result;
}
TSUCHI_Result ExternalMods::request(const char* id, bool enabled) {
    ExternalModEntry* item = find(id);
    if (!item) return TSUCHI_NOT_FOUND;
    if (!item->manageable) return TSUCHI_INVALID;
    if (unread_) return TSUCHI_NOT_READY;
    item->requested = enabled;
    return TSUCHI_OK;
}
bool ExternalMods::changed() const {
    for (uint32_t i = 0; i < count(); ++i) if (entries_[i].requested != entries_[i].saved) return true;
    return false;
}
bool ExternalMods::queued() const {
    for (uint32_t i = 0; i < count(); ++i)
        if (entries_[i].manageable && entries_[i].saved != (entries_[i].observed == ExternalModState::enabled)) return true;
    return false;
}
void ExternalMods::discard() {
    for (uint32_t i = 0; i < count(); ++i) entries_[i].requested = entries_[i].saved;
}
bool ExternalMods::requests_json(Text& out) const {
    if (!revision_[0]) return false;
    // IDs and revisions contain no JSON characters that need escaping.
    size_t bytes = 128;
    for (uint32_t i = 0; i < count(); ++i) if (entries_[i].manageable) bytes += strlen(entries_[i].id) + 10;
    Text staged;
    if (!staged.reserve(bytes)) return false;
    char* put = staged.data;
    const auto append = [&](const char* value) { const size_t n = strlen(value); memcpy(put, value, n); put += n; };
    append("{\"format\":1,\"revision\":\""); append(revision_); append("\",\"enabled\":{");
    bool first = true;
    for (uint32_t i = 0; i < count(); ++i) {
        const ExternalModEntry& item = entries_[i];
        if (!item.manageable) continue;
        if (!first) append(",");
        first = false;
        append("\""); append(item.id); append(item.requested ? "\":true" : "\":false");
    }
    append("}}"); *put = 0;
    out.clear(); out = staged;
    return true;
}
TSUCHI_Result ExternalMods::save(TSUCHI_Result (*write)(const char*, uint64_t)) {
    if (!write || !revision_[0]) return TSUCHI_INVALID;
    if (unread_) return TSUCHI_NOT_READY;
    if (!changed()) return TSUCHI_OK;
    Text encoded;
    if (!requests_json(encoded)) return TSUCHI_NO_MEMORY;
    const TSUCHI_Result result = write(encoded.get(), uint64_t(strlen(encoded.get())));
    encoded.clear();
    if (result == TSUCHI_OK) for (uint32_t i = 0; i < count(); ++i) entries_[i].saved = entries_[i].requested;
    return result;
}
void ExternalMods::clear() {
    clear_entries(entries_);
    revision_[0] = 0;
    unread_ = false;
}
} // namespace tsuchi
