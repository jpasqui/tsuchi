// Particle playbacks are counted holds in the shared hold tables.
#include <string.h>
#include "effects_native.h"
#include "effects_service.h"
#include "frame.h"
#include "holds.h"
#include "ids.h"
#include "list.h"
#include "object_retention.h"
#include "objects.h"

namespace tsuchi {
namespace {
List<EffectAsset> catalog;
List<TSUCHI_EffectsFinished> pending;
uint64_t catalog_world = 0, next_effect = 1, catalog_revision = 0;
TSUCHI_Result catalog_state = TSUCHI_NOT_READY;
struct Parameter { char name[kIdBytes]; TSUCHI_ParameterKind kind; TSUCHI_ParticleValue value; };
struct Kept {
    uint64_t emitter, root, world;
    List<Parameter>* parameters;
    bool natural, attached, retained, accepted;
};
struct Wanted { uint64_t asset, parent; TSUCHI_Vec3 place; const Parameter* parameter = nullptr; };

TSUCHI_Result apply(const Hold& hold, Kept& kept, const Wanted& wanted, bool fresh) {
    if (!fresh) {
        if (!wanted.parameter) return TSUCHI_BUSY;
        const Parameter entry = *wanted.parameter;
        auto& parameters = *kept.parameters;
        uint32_t index = parameters.count;
        for (uint32_t i = 0; i < parameters.count; ++i)
            if (id_same(parameters[i].name, entry.name) && parameters[i].kind == entry.kind) { index = i; break; }
        if (index == parameters.count && !parameters.reserve(index + 1)) return TSUCHI_NO_MEMORY;
        if (!hold_call_guard(hold).permits()) return TSUCHI_NOT_READY;
        if (kept.natural || effects_native_world() != kept.world ||
            !effects_native_alive(kept.emitter, kept.attached ? hold.object : 0, kept.root)) return TSUCHI_NOT_FOUND;
        const TSUCHI_Result changed = effects_native_parameter(kept.emitter, entry.name, entry.kind, entry.value);
        if (!hold_call_guard(hold).permits()) return TSUCHI_NOT_READY;
        if (kept.natural || !effects_native_alive(kept.emitter, kept.attached ? hold.object : 0, kept.root)) return TSUCHI_NOT_FOUND;
        if (changed != TSUCHI_OK) return changed;
        if (index == parameters.count) ++parameters.count;
        parameters[index] = entry;
        return TSUCHI_OK;
    }
    if (wanted.parameter) return TSUCHI_NOT_FOUND;
    kept.world = effects_native_world();
    kept.attached = wanted.parent != 0;
    kept.parameters = static_cast<List<Parameter>*>(allocate(sizeof(List<Parameter>)));
    if (!kept.parameters) return TSUCHI_NO_MEMORY;
    *kept.parameters = {};
    if (!hold_call_guard(hold).permits()) {
        release(kept.parameters); kept.parameters = nullptr;
        return TSUCHI_NOT_READY;
    }
    // Restore stack state if creation calls into a mod that stops this hold.
    hold_apply_checkpoint(hold);
    const TSUCHI_Result result = effects_native_create(wanted.asset, wanted.parent, wanted.place, kept.emitter, kept.root);
    if (result == TSUCHI_OK && kept.emitter)
        kept.retained = retain_game_object(kept.emitter);
    if (result != TSUCHI_OK || !kept.emitter || !kept.retained) {
        if (kept.emitter) effects_native_stop(kept.emitter);
        kept.emitter = 0;
        release(kept.parameters); kept.parameters = nullptr;
        return result != TSUCHI_OK ? result : TSUCHI_NO_MEMORY;
    }
    return TSUCHI_OK;
}
bool keep(const Hold& hold, Kept& kept) {
    return !kept.natural && effects_native_world() == kept.world &&
        effects_native_alive(kept.emitter, kept.attached ? hold.object : 0, kept.root);
}
void restore(const Hold& hold, Kept& kept, HoldEnd why) {
    const TSUCHI_EffectEnd reason = kept.natural ? TSUCHI_EFFECT_END_NATURAL :
        effects_native_world() != kept.world ? TSUCHI_EFFECT_END_WORLD_UNLOADED :
        (why == HoldEnd::released || why == HoldEnd::stopped) ? TSUCHI_EFFECT_END_STOPPED :
        kept.attached ? TSUCHI_EFFECT_END_PARENT_LOST : TSUCHI_EFFECT_END_LOST;
    if (kept.emitter && !kept.natural) effects_native_stop(kept.emitter);
    if (kept.retained) release_game_object(kept.emitter);
    if (kept.parameters) { kept.parameters->clear(); release(kept.parameters); }
    // Each accepted play reserved one terminal event before any native creation.
    if (kept.accepted) pending.add({{hold.what}, reason});
}
using Playbacks = Holds<Kept, Wanted, apply, keep, restore>;
Playbacks placed{"effects.play", HoldOn::object, false};
Playbacks attached{"effects.play_on", HoldOn::watched_actor, false};
Playbacks* tables[] = {&placed, &attached};

bool add_asset(void*, const EffectAsset& asset) {
    if (!catalog.add(asset)) { catalog_state = TSUCHI_NO_MEMORY; return false; }
    return true;
}
void refresh_catalog(bool requested) {
    const uint64_t world = effects_native_world();
    const uint64_t revision = effects_native_revision();
    if (world != catalog_world) {
        catalog.clear(); catalog_world = world; catalog_state = TSUCHI_NOT_READY; catalog_revision = 0;
    }
    // World changes invalidate the cache. An unused service does no discovery
    // in startup and scene-loading steps.
    if (!requested || (revision == catalog_revision && catalog_state == TSUCHI_OK)) return;
    catalog.clear(); catalog_state = TSUCHI_NOT_READY;
    if (!world || !effects_native_ready()) { catalog_world = 0; return; }
    catalog_state = TSUCHI_OK;
    const TSUCHI_Result result = effects_native_catalog(add_asset, nullptr);
    if (result != TSUCHI_OK) catalog_state = result;
    // Catalog construction can assign weak serials of its own.
    catalog_revision = effects_native_revision();
}
void catalog_step() { refresh_catalog(false); }
void deliver() {
    const uint32_t budget = pending.count;
    for (uint32_t i = 0; i < budget; ++i) {
        const TSUCHI_EffectsFinished event = pending[0];
        --pending.count;
        if (pending.count) memmove(pending.items, pending.items + 1, sizeof(event) * pending.count);
        effects::emit_finished(event);
    }
}
TSUCHI_Result listed(char* buffer, uint64_t capacity, uint64_t* out) {
    refresh_catalog(true);
    if (!effects_native_world() || effects_native_world() != catalog_world) return TSUCHI_NOT_READY;
    if (catalog_state != TSUCHI_OK) return catalog_state;
    uint64_t bytes = 1; // The packed text list ends with an empty string.
    for (uint32_t i = 0; i < catalog.count; ++i) bytes += strlen(catalog[i].id) + 1;
    *out = bytes;
    if (capacity < bytes) return TSUCHI_CAPACITY;
    for (uint32_t i = 0; i < catalog.count; ++i) {
        const size_t n = strlen(catalog[i].id) + 1;
        memcpy(buffer, catalog[i].id, n); buffer += n;
    }
    *buffer = 0;
    return TSUCHI_OK;
}
TSUCHI_Result play(TSUCHI_Mod* mod, const char* kind, uint64_t parent, const TSUCHI_Vec3& place, TSUCHI_Effect* out) {
    uint64_t asset = 0;
    for (uint32_t attempt = 0; attempt < 2 && !asset; ++attempt) {
        // Existing live assets need no global discovery when unrelated objects
        // or the previous playback change the native object inventory.
        refresh_catalog(attempt != 0);
        if (!effects_native_world() || effects_native_world() != catalog_world || !effects_native_ready()) return TSUCHI_NOT_READY;
        if (catalog_state != TSUCHI_OK) continue;
        for (uint32_t i = 0; i < catalog.count; ++i) {
            if (!id_same(kind, catalog[i].id) || !game::from_handle(catalog[i].handle)) continue;
            if (asset) return TSUCHI_BUSY;
            asset = catalog[i].handle;
        }
    }
    if (catalog_state != TSUCHI_OK) return catalog_state;
    if (!asset) return TSUCHI_NOT_FOUND;
    if (!next_effect) return TSUCHI_CAPACITY;
    if (!pending.reserve(pending.count + placed.count() + attached.count() + 1)) return TSUCHI_NO_MEMORY;
    const uint64_t token = next_effect++;
    Playbacks& table = parent ? attached : placed;
    const uint64_t object = parent ? parent : effects_native_world();
    const TSUCHI_Result result = table.take(mod, object, token, Wanted{asset, parent, place});
    if (result == TSUCHI_OK) {
        table.find(mod, object, token)->accepted = true;
        out->id = token;
    }
    return result;
}
TSUCHI_Result at(TSUCHI_Mod* mod, const char* kind, const TSUCHI_Vec3* place, TSUCHI_Effect* out) {
    return play(mod, kind, 0, *place, out);
}
TSUCHI_Result on(TSUCHI_Mod* mod, const char* kind, TSUCHI_Actor parent, const TSUCHI_Vec3* place, TSUCHI_Effect* out) {
    return play(mod, kind, parent.id, *place, out);
}
struct Found { Playbacks* table; Hold hold; Kept* kept; };
TSUCHI_Result find(TSUCHI_Mod* mod, TSUCHI_Effect effect, Found& out) {
    for (auto* table : tables) for (uint32_t i = 0; i < table->count(); ++i) {
        const Hold held = table->hold_at(i);
        if (held.what != effect.id) continue;
        if (held.mod != mod) return TSUCHI_DENIED;
        auto& kept = table->kept_at(i);
        if (!kept.accepted || kept.natural || !keep(held, kept)) return TSUCHI_NOT_FOUND;
        out = {table, held, &kept}; return TSUCHI_OK;
    }
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result parameter(TSUCHI_Mod* mod, TSUCHI_Effect effect, const char* name, TSUCHI_ParameterKind kind, TSUCHI_ParticleValue* out) {
    Found found{};
    const TSUCHI_Result result = find(mod, effect, found);
    if (result != TSUCHI_OK) return result;
    for (uint32_t i = 0; i < found.kept->parameters->count; ++i) {
        const auto& entry = (*found.kept->parameters)[i];
        if (id_same(entry.name, name) && entry.kind == kind) { *out = entry.value; return TSUCHI_OK; }
    }
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result set_parameter(TSUCHI_Mod* mod, TSUCHI_Effect effect, const char* name, TSUCHI_ParameterKind kind, const TSUCHI_ParticleValue* value) {
    Found found{};
    const TSUCHI_Result result = find(mod, effect, found);
    if (result != TSUCHI_OK) return result;
    Parameter entry{};
    memcpy(entry.name, name, strlen(name) + 1); entry.kind = kind; entry.value = *value;
    if (kind == TSUCHI_PARAMETER_KIND_SCALAR) entry.value.y = entry.value.z = entry.value.w = 0;
    if (kind == TSUCHI_PARAMETER_KIND_VECTOR) entry.value.w = 0;
    // Shared apply keeps the playback state alive through a native setter. A
    // reentrant stop defers emitter cleanup until that setter has returned.
    const TSUCHI_Result changed = found.table->take(mod, found.hold.object, found.hold.what, Wanted{0, 0, {}, &entry});
    return changed == TSUCHI_NOT_READY && !found.table->find(mod, found.hold.object, found.hold.what) ? TSUCHI_NOT_FOUND : changed;
}
TSUCHI_Result stop(TSUCHI_Mod* mod, TSUCHI_Effect effect) {
    Found found{};
    const TSUCHI_Result result = find(mod, effect, found);
    return result == TSUCHI_OK ? found.table->release(mod, found.hold.object, found.hold.what) : result;
}
const effects::Backend service{listed, at, on, parameter, set_parameter, stop};
} // namespace
void effects_finished_native(uint64_t emitter) {
    if (!on_game_thread()) return;
    for (auto* table : tables) for (uint32_t i = 0; i < table->count(); ++i)
        if (table->kept_at(i).emitter == emitter) table->kept_at(i).natural = true;
}
void effects_start(TSUCHI_Mod* self) {
    effects_native_start(self);
    each_frame(catalog_step);
    placed.start(); attached.start();
    each_frame(deliver);
    effects::set_backend(&service);
}
} // namespace tsuchi
