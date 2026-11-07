// Code declarations are admitted first, then applied through the manifest paths.
#include <string.h>
#include "content_service.h"
#include "content_sound.h"
#include "content_identity.h"
#include "content_placements.h"
#include "frame.h"
#include "json.h"
#include "list.h"
#include "mods.h"
#include "registry.h"
#include "text.h"

namespace tsuchi {
namespace {
const Kind declaration_kind{"content.declaration"};
struct Declaration {
    uint64_t id, first, last;
    TSUCHI_Mod* mod;
    const Json* tree;
    TSUCHI_DeclarationState state;
    bool active, ended;
};
List<Declaration*> declarations;
uint64_t next_id = 1;
bool stepping = false, started = false;
Declaration* find(uint64_t id) {
    for (uint32_t i = 0; i < declarations.count; ++i) if (declarations[i]->id == id) return declarations[i];
    return nullptr;
}
void discard(Declaration* declaration) {
    for (uint32_t i = 0; i < declarations.count; ++i) if (declarations[i] == declaration) { declarations.remove(&declarations[i]); break; }
    release_json(declaration->tree);
    release(declaration);
}
void end_declaration(void* context, uint64_t) {
    auto* declaration = static_cast<Declaration*>(context);
    declaration->ended = true;
    if (!declaration->active) discard(declaration);
}
bool allowed(const char* key) {
    return !strcmp(key, "content") || !strcmp(key, "new_maps") || !strcmp(key, "maps") ||
           !strcmp(key, "tables") || !strcmp(key, "text");
}
const char* reason(TSUCHI_Result result) {
    switch (result) {
    case TSUCHI_NOT_FOUND: return "not_found";
    case TSUCHI_INVALID: return "invalid";
    case TSUCHI_BUSY: return "busy";
    case TSUCHI_NOT_READY: return "not_ready";
    case TSUCHI_CAPACITY: return "capacity";
    case TSUCHI_UNSUPPORTED: return "unsupported";
    case TSUCHI_IO: return "io";
    case TSUCHI_DENIED: return "denied";
    case TSUCHI_NO_MEMORY: return "no_memory";
    default: return "ok";
    }
}
TSUCHI_Result add(TSUCHI_Mod* mod, const char* definition, TSUCHI_ContentDeclaration* out) {
    if (!started) return TSUCHI_NOT_READY;
    if (ownership_isolated(mod)) return TSUCHI_BUSY;
    JsonError error{};
    const Json* tree = read_json(definition, strlen(definition), &error);
    if (!tree) return error.what && !strcmp(error.what, "there is no memory to read it into") ? TSUCHI_NO_MEMORY : TSUCHI_INVALID;
    if (tree->kind != Json::Kind::object || !tree->count) { release_json(tree); return TSUCHI_INVALID; }
    Text problem;
    for (const Json* part = tree->first; part; part = part->next) {
        if (!allowed(part->name)) { problem.clear(); release_json(tree); return TSUCHI_INVALID; }
        const auto result = manifest_part(part->name, mod, *part, problem, false);
        problem.clear();
        if (result != TSUCHI_OK) { release_json(tree); return result; }
    }
    auto* declaration = static_cast<Declaration*>(allocate(sizeof(Declaration)));
    if (!declaration) { release_json(tree); return TSUCHI_NO_MEMORY; }
    *declaration = Declaration{next_id, 0, 0, mod, tree, TSUCHI_DECLARATION_STATE_QUEUED, false, false};
    if (!declarations.add(declaration)) { release_json(tree); release(declaration); return TSUCHI_NO_MEMORY; }
    const auto result = own(mod, declaration_kind, declaration->id, end_declaration, declaration);
    if (result != TSUCHI_OK) { discard(declaration); return result; }
    out->id = next_id++;
    return TSUCHI_OK;
}
void step() {
    if (stepping || !on_game_thread()) return;
    stepping = true;
    const uint64_t ceiling = next_id;
    uint64_t after = 0;
    for (;;) {
        Declaration* declaration = nullptr;
        for (uint32_t i = 0; i < declarations.count; ++i) {
            Declaration* candidate = declarations[i];
            if (!candidate->ended && candidate->state == TSUCHI_DECLARATION_STATE_QUEUED && candidate->id > after && candidate->id < ceiling) {
                if (!declaration || candidate->id < declaration->id) declaration = candidate;
            }
        }
        if (!declaration) break;
        after = declaration->id;
        declaration->active = true;
        TSUCHI_Mod* mod = declaration->mod;
        declaration->first = ownership_mark(mod);
        const uint64_t previous_floor = isolate_ownership(mod, declaration->first);
        Text problem;
        TSUCHI_Result result = TSUCHI_OK;
        for (const Json* part = declaration->tree->first; part; part = part->next) {
            if (!mod_usable(mod)) { result = TSUCHI_NOT_READY; break; }
            result = manifest_part(part->name, mod, *part, problem, true);
            if (result != TSUCHI_OK) break;
        }
        problem.clear();
        declaration->last = ownership_mark(mod);
        isolate_ownership(mod, previous_floor);
        if (result == TSUCHI_OK && !declaration->ended) content_identity_accept(mod, declaration->first, declaration->last);
        if (result != TSUCHI_OK && !declaration->ended) release_owned_between(mod, declaration->first, declaration->last);
        if (!declaration->ended && mod_usable(mod)) {
            declaration->state = result == TSUCHI_OK ? TSUCHI_DECLARATION_STATE_READY : TSUCHI_DECLARATION_STATE_FAILED;
            const TSUCHI_ContentDeclaration identity{declaration->id};
            if (result == TSUCHI_OK) content::emit_ready_to(mod, TSUCHI_ContentReady{identity});
            else content::emit_failed_to(mod, TSUCHI_ContentFailed{identity, reason(result)});
        }
        declaration->active = false;
        if (declaration->ended) discard(declaration);
    }
    stepping = false;
}
TSUCHI_Result state(TSUCHI_Mod* mod, TSUCHI_ContentDeclaration identity, TSUCHI_DeclarationState* out) {
    const auto* declaration = find(identity.id);
    if (!declaration || declaration->ended) return TSUCHI_NOT_FOUND;
    if (declaration->mod != mod) return TSUCHI_DENIED;
    *out = declaration->state;
    return TSUCHI_OK;
}
TSUCHI_Result retract(TSUCHI_Mod* mod, TSUCHI_ContentDeclaration identity) {
    auto* declaration = find(identity.id);
    if (!declaration || declaration->ended) return TSUCHI_NOT_FOUND;
    if (declaration->mod != mod) return TSUCHI_DENIED;
    if (declaration->active) return TSUCHI_BUSY;
    declaration->active = true;
    release_owned_between(mod, declaration->first, declaration->last);
    declaration->active = false;
    if (!declaration->ended) {
        disown(mod, declaration_kind, declaration->id);
        declaration->ended = true;
    }
    discard(declaration);
    return TSUCHI_OK;
}
TSUCHI_Result load_bank(TSUCHI_Mod* mod, const char* id, const void* bytes, uint64_t size, TSUCHI_SoundBank* out) {
    if (size > 16u * 1024u * 1024u) return TSUCHI_CAPACITY;
    return content_sound_load(mod, id, bytes, uint32_t(size), &out->id);
}
TSUCHI_Result bank(TSUCHI_Mod* mod, const char* id, TSUCHI_SoundBank* out) {
    return content_sound_find(mod, id, &out->id);
}
TSUCHI_Result bank_events(TSUCHI_Mod* mod, TSUCHI_SoundBank bank, char* buffer, uint64_t capacity, uint64_t* out) {
    return content_sound_events(mod, bank.id, buffer, capacity, out);
}
TSUCHI_Result play_event(TSUCHI_Mod* mod, TSUCHI_SoundBank bank, const char* event, const TSUCHI_Vec3* point, TSUCHI_SoundVoice* out) {
    return content_sound_play(mod, bank.id, event, *point, &out->id);
}
TSUCHI_Result stop_event(TSUCHI_Mod* mod, TSUCHI_SoundVoice voice) { return content_sound_stop(mod, voice.id); }
TSUCHI_Result unload_bank(TSUCHI_Mod* mod, TSUCHI_SoundBank bank) { return content_sound_unload(mod, bank.id); }
} // namespace
void content_service_start() {
    if (!started) started = each_frame(step);
    static const content::Backend backend{add, state, retract, load_bank, bank, bank_events, play_event, stop_event, unload_bank,
        content_placement_values, content_placement_kind, content_placement_toggle, content_placement_integer,
        content_placement_number, content_placement_text};
    content_sound_start();
    content::set_backend(&backend);
}
} // namespace tsuchi
