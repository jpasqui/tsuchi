// The game's unpooled particle emitters. The effects service owns their holds.
#include "effects_native.h"
#include "actors.h"
#include "flow.h"
#include "game.h"
#include "hooks.h"
#include "object_retention.h"
#include "objects.h"
#include "reflect.h"
#include "registry.h"

namespace tsuchi {
namespace {
namespace t = game::table;
game::ClassRef particles{t::name::effect_asset_class};
game::ClassRef components{t::name::effect_component_class};
game::ClassRef actors{t::name::engine_actor_class};
game::ClassRef roots{t::name::scene_component_class};
game::ClassRef worlds{t::name::effect_world_class};
using Finished = void (*)(void*, void*);
Finished next_finished = nullptr;
bool finish_ready = false;

struct Retained {
    uint64_t handles[4]{};
    uint32_t count = 0;
    bool add(uint64_t handle) {
        if (!handle || count == 4 || !retain_game_object(handle)) return false;
        handles[count++] = handle;
        return true;
    }
    ~Retained() { while (count) release_game_object(handles[--count]); }
};

TSUCHI_Result supported(uint64_t asset) {
    uint32_t flags = 0;
    if (!game::live(asset) || !game::is_a(asset, particles) ||
        !game::read(asset + t::object::flags, flags) || (flags & t::constant::template_flags)) return TSUCHI_NOT_FOUND;
    uint8_t single = 0, water = 0;
    if (!game::read(asset + t::effect_asset::single_emitter, single) ||
        !game::read(asset + t::effect_asset::water_adjustment, water)) return TSUCHI_NOT_READY;
    return single || water ? TSUCHI_UNSUPPORTED : TSUCHI_OK;
}

TSUCHI_Result owner_gate(uint64_t owner) {
    const uint64_t framework = game::game_framework();
    uint64_t manager = 0;
    if (!framework || !game::read(framework + t::effect_owner::manager, manager) || !manager) return TSUCHI_NOT_READY;
    uint8_t enabled = 0;
    if (!game::read(manager + t::effect_manager::invalid_enabled, enabled)) return TSUCHI_NOT_READY;
    if (!enabled) return TSUCHI_OK;
    int32_t count = 0, capacity = 0;
    uint64_t data = 0;
    if (!game::read(manager + t::effect_manager::invalid_count, count) ||
        !game::read(manager + t::effect_manager::invalid_capacity, capacity) ||
        count < 0 || capacity < count || uint32_t(capacity) > t::constant::max_objects) return TSUCHI_NOT_READY;
    if (!count) return TSUCHI_OK;
    if (!game::read(manager + t::effect_manager::invalid_owners, data) ||
        !data || !game::readable(data, sizeof(uint64_t) * uint64_t(count))) return TSUCHI_NOT_READY;
    for (int32_t i = 0; i < count; ++i)
        if (game::read_or<uint64_t>(data + sizeof(uint64_t) * uint64_t(i)) == owner) return TSUCHI_NOT_READY;
    return TSUCHI_OK;
}

void finished(void* emitter, void* component) {
    // The service queues the event. Native teardown continues through the chain.
    if (on_game_thread()) effects_finished_native(game::to_handle(reinterpret_cast<uint64_t>(emitter)));
    if (next_finished) next_finished(emitter, component);
}

bool unpooled(uint64_t emitter) {
    uint64_t pool = 0;
    return game::live(emitter) && game::read(emitter + t::effect_emitter::pool, pool) &&
        pool == t::constant::effect_unpooled;
}

bool playing(uint64_t emitter) {
    uint8_t released = 0;
    return unpooled(emitter) && game::read(emitter + t::effect_emitter::released, released) && !released;
}
} // namespace

bool effects_native_ready() {
    if (!on_game_thread() || !finish_ready || !t::bound(t::fn::effect_create) || !t::bound(t::fn::effect_release)) return false;
    const uint64_t world = game::loaded_world(), owner = game::game_framework();
    uint64_t manager = 0;
    return world && game::live(world) && owner && game::live(owner) &&
        game::read(owner + t::effect_owner::manager, manager) && manager &&
        game::readable(manager + t::effect_manager::invalid_enabled, sizeof(uint8_t));
}

uint64_t effects_native_world() {
    return on_game_thread() ? game::to_handle(game::loaded_world()) : 0;
}

uint64_t effects_native_revision() {
    const auto base = game::main_base();
    return (uint64_t(game::read_or<uint32_t>(base + t::data::object_count)) << 32) |
        game::read_or<uint32_t>(base + t::data::object_serial_counter);
}

TSUCHI_Result effects_native_catalog(EffectVisit visit, void* context) {
    if (!on_game_thread()) return TSUCHI_NOT_READY;
    if (!visit) return TSUCHI_INVALID;
    // Resolve every native class during catalog construction. A first play or
    // parameter write must not discover a class by scanning the object table.
    if (!game::find_class(particles) || !game::find_class(components) || !game::find_class(actors) ||
        !game::find_class(roots) || !game::find_class(worlds)) return TSUCHI_NOT_READY;
    const uint64_t base = game::main_base();
    int32_t count = 0;
    if (!game::read(base + t::data::object_count, count) || count < 0 || uint32_t(count) > t::constant::max_objects ||
        !game::find_class(particles)) return TSUCHI_NOT_READY;
    const uint32_t chunks = (uint32_t(count) + uint32_t(t::constant::objects_per_chunk) - 1) / uint32_t(t::constant::objects_per_chunk);
    uint64_t blocks = 0;
    if (chunks && (!game::read(base + t::data::object_chunks, blocks) || !blocks ||
        !game::readable(blocks, sizeof(uint64_t) * uint64_t(chunks)))) return TSUCHI_NOT_READY;
    for (uint32_t i = 0; i < chunks; ++i) {
        const uint64_t block = game::read_or<uint64_t>(blocks + sizeof(uint64_t) * i);
        const uint32_t left = uint32_t(count) - i * uint32_t(t::constant::objects_per_chunk);
        const uint32_t entries = left < uint32_t(t::constant::objects_per_chunk) ? left : uint32_t(t::constant::objects_per_chunk);
        if (!block || !game::readable(block, uint64_t(t::object_item::size) * entries)) return TSUCHI_NOT_READY;
    }
    struct Catalog { EffectVisit visit; void* context; } catalog{visit, context};
    game::each_object([](void* data, uint64_t asset) {
        auto& catalog = *static_cast<Catalog*>(data);
        const TSUCHI_Result status = supported(asset);
        if (status != TSUCHI_OK && status != TSUCHI_UNSUPPORTED) return true;
        EffectAsset entry{};
        entry.handle = game::to_handle(asset);
        if (!entry.handle || !game::name_text(game::name_of(asset), entry.id, sizeof entry.id) || !entry.id[0]) return true;
        return catalog.visit(catalog.context, entry);
    }, &catalog);
    return TSUCHI_OK;
}

TSUCHI_Result effects_native_create(uint64_t asset_handle, uint64_t parent_handle, const TSUCHI_Vec3& place,
                                    uint64_t& emitter_handle, uint64_t& root_handle) {
    emitter_handle = root_handle = 0;
    if (!on_game_thread()) return TSUCHI_NOT_READY;
    if (!__builtin_isfinite(place.x) || !__builtin_isfinite(place.y) || !__builtin_isfinite(place.z)) return TSUCHI_INVALID;
    if (!effects_native_ready()) return TSUCHI_NOT_READY;
    const uint64_t world = game::loaded_world(), world_handle = game::to_handle(world);
    const uint64_t asset = game::from_handle(asset_handle);
    const uint64_t owner = parent_handle ? game::from_handle(parent_handle) : game::game_framework();
    if (!asset || !owner) return TSUCHI_NOT_FOUND;
    if (!game::is_a(owner, actors)) return TSUCHI_INVALID;
    TSUCHI_Result status = supported(asset);
    if (status != TSUCHI_OK) return status;
    uint32_t spawn_flags = 0;
    if (!world_handle || !game::is_a(world, worlds) || !game::read(world + t::effect_world::spawn_flags, spawn_flags) ||
        (spawn_flags & t::constant::effect_spawn_blocked)) return TSUCHI_NOT_READY;
    const uint64_t owner_handle = game::to_handle(owner);
    uint64_t root = 0;
    if (parent_handle && (!game::read(owner + t::engine_actor::root, root) || !root || !game::live(root) ||
        !game::is_a(root, roots))) return TSUCHI_NOT_READY;
    const uint64_t held_root = root ? game::to_handle(root) : 0;
    Retained inputs;
    if (!inputs.add(world_handle) || !inputs.add(owner_handle) || !inputs.add(asset_handle) ||
        (root && !inputs.add(held_root))) return TSUCHI_NO_MEMORY;
    const auto get_world = game::method(owner, t::effect_owner::get_world);
    if (!get_world || reinterpret_cast<uint64_t>(get_world(reinterpret_cast<void*>(owner))) != world) return TSUCHI_NOT_READY;
    if (game::from_handle(world_handle) != world || game::from_handle(owner_handle) != owner ||
        game::from_handle(asset_handle) != asset || (root && (game::from_handle(held_root) != root ||
        game::read_or<uint64_t>(owner + t::engine_actor::root) != root))) return TSUCHI_NOT_FOUND;
    status = supported(asset);
    if (status != TSUCHI_OK) return status;
    if (game::loaded_world() != world || !game::read(world + t::effect_world::spawn_flags, spawn_flags) ||
        (spawn_flags & t::constant::effect_spawn_blocked) || owner_gate(owner) != TSUCHI_OK) return TSUCHI_NOT_READY;
    const float transform[10]{0, 0, 0, 1, place.x, place.y, place.z, 1, 1, 1};
    const game::Name socket{};
    const uint64_t emitter = reinterpret_cast<uint64_t>(t::fn::effect_create(reinterpret_cast<void*>(owner), nullptr,
        reinterpret_cast<void*>(asset), transform, nullptr, reinterpret_cast<void*>(root), &socket));
    emitter_handle = game::to_handle(emitter);
    if (!emitter_handle || !playing(emitter)) {
        // This exact creation has not crossed another native call. Release it
        // even if the native result does not have the requested unpooled marker.
        uint8_t released = 0;
        if (emitter_handle && game::from_handle(emitter_handle) == emitter &&
            game::read(emitter + t::effect_emitter::released, released) && !released)
            t::fn::effect_release(reinterpret_cast<void*>(emitter), true);
        emitter_handle = 0;
        return TSUCHI_NOT_READY;
    }
    if (game::from_handle(world_handle) != world || game::loaded_world() != world ||
        game::from_handle(asset_handle) != asset || game::from_handle(owner_handle) != owner ||
        (root && (game::from_handle(held_root) != root || game::read_or<uint64_t>(owner + t::engine_actor::root) != root))) {
        effects_native_stop(emitter_handle);
        emitter_handle = 0;
        return TSUCHI_NOT_FOUND;
    }
    root_handle = held_root;
    return TSUCHI_OK;
}

bool effects_native_alive(uint64_t emitter_handle, uint64_t parent_handle, uint64_t root_handle) {
    if (!on_game_thread() || !playing(game::from_handle(emitter_handle))) return false;
    if (!parent_handle) return true;
    const uint64_t owner = game::from_handle(parent_handle), root = game::from_handle(root_handle);
    return owner && root && game::read_or<uint64_t>(owner + t::engine_actor::root) == root;
}

void effects_native_stop(uint64_t emitter_handle) {
    if (!on_game_thread() || !t::bound(t::fn::effect_release)) return;
    const uint64_t emitter = game::from_handle(emitter_handle);
    uint8_t released = 0;
    if (!unpooled(emitter) || !game::read(emitter + t::effect_emitter::released, released) || released) return;
    t::fn::effect_release(reinterpret_cast<void*>(emitter), true);
}

TSUCHI_Result effects_native_parameter(uint64_t emitter_handle, const char* text, TSUCHI_ParameterKind kind,
                                       const TSUCHI_ParticleValue& value) {
    if (!on_game_thread()) return TSUCHI_NOT_READY;
    if (!text || !*text || kind > TSUCHI_PARAMETER_KIND_COLOR ||
        !__builtin_isfinite(value.x) || (kind != TSUCHI_PARAMETER_KIND_SCALAR &&
        (!__builtin_isfinite(value.y) || !__builtin_isfinite(value.z))) ||
        (kind == TSUCHI_PARAMETER_KIND_COLOR && !__builtin_isfinite(value.w))) return TSUCHI_INVALID;
    const uint64_t emitter = game::from_handle(emitter_handle);
    if (!playing(emitter)) return TSUCHI_NOT_FOUND;
    Retained retained;
    if (!retained.add(emitter_handle)) return TSUCHI_NO_MEMORY;
    game::Name name{};
    const TSUCHI_Result named = game::name_from_utf8(text, name);
    if (named != TSUCHI_OK) return named;
    if (!name.index) return TSUCHI_NOT_FOUND;
    if (game::from_handle(emitter_handle) != emitter || !playing(emitter)) return TSUCHI_NOT_FOUND;
    uint64_t component = 0;
    if (!game::read(emitter + t::effect_emitter::component, component) || !component ||
        !game::live(component) || !game::is_a(component, components)) return TSUCHI_NOT_READY;
    const uint64_t component_handle = game::to_handle(component);
    if (!retained.add(component_handle)) return TSUCHI_NO_MEMORY;
    uint64_t packed = 0;
    memcpy(&packed, &name, sizeof packed);
    if (kind == TSUCHI_PARAMETER_KIND_SCALAR) {
        const auto set = game::method(component, t::effect_parameters::scalar);
        if (!set) return TSUCHI_NOT_READY;
        set(reinterpret_cast<void*>(component), packed, value.x);
    } else if (kind == TSUCHI_PARAMETER_KIND_VECTOR) {
        const auto set = game::method(component, t::effect_parameters::vector);
        if (!set) return TSUCHI_NOT_READY;
        set(reinterpret_cast<void*>(component), packed, value.x, value.y, value.z);
    } else {
        const auto set = game::method(component, t::effect_parameters::color);
        if (!set) return TSUCHI_NOT_READY;
        set(reinterpret_cast<void*>(component), packed, value.x, value.y, value.z, value.w);
    }
    return game::from_handle(emitter_handle) == emitter && playing(emitter) &&
        game::from_handle(component_handle) == component &&
        game::read_or<uint64_t>(emitter + t::effect_emitter::component) == component ? TSUCHI_OK : TSUCHI_NOT_FOUND;
}

void effects_native_start(TSUCHI_Mod* self) {
    uint64_t hook = 0;
    finish_ready = hook_slot(self, t::slot::effect_finished, finished, &next_finished, 0, &hook) == TSUCHI_OK;
}
} // namespace tsuchi
