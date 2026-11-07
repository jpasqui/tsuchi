#include "parts.h"

#include "engine_call.h"
#include "game.h"
#include "objects.h"
#include "reflect.h"

namespace tsuchi::game {
namespace {

namespace t = table;

ClassRef actor_class{t::name::engine_actor_class}, part_class{t::name::actor_component_class},
    scene_class{t::name::scene_component_class}, class_class{t::name::class_class};
ClassRef mesh_class{t::name::mesh_class}, material_class{t::name::material_class};
ClassRef particle_class{"ParticleSystemComponent"}, decal_class{"DecalComponent"}, image_class{"Image"};
ClassRef skeletal_class{t::name::skeletal_class}, clip_class{t::name::clip_class};

// Optional renderer classes need not be loaded yet. Cache by the target's
// class life, and look only through its hierarchy on a miss. A newly loaded
// class has another handle, so earlier misses cannot hide it.
struct RendererKind { uint64_t type = 0; uint8_t kinds = 0; };
RendererKind renderer_types[128];
uint32_t next_renderer_type = 0;
uint8_t render_kinds(uint64_t object) {
    if (!live(object)) return false;
    const uint64_t type = class_of(object), handle = to_handle(type);
    if (!handle) return false;
    for (const RendererKind& cached : renderer_types)
        if (cached.type == handle) return cached.kinds;
    uint8_t kinds = 0;
    ClassRef* refs[] = {&particle_class, &decal_class, &image_class};
    for (uint64_t parent = type, depth = 0; parent && depth < 64;
         parent = read_or<uint64_t>(parent + t::struct_type::super), ++depth) {
        for (uint32_t i = 0; i < 3; ++i) {
            ClassRef& ref = *refs[i];
            const uint64_t known = from_handle(ref.handle);
            if (known ? parent == known : name_is(name_of(parent), ref.name)) {
                ref.found = parent;
                ref.handle = to_handle(parent);
                if (ref.handle) kinds |= uint8_t(1u << i);
            }
        }
    }
    renderer_types[next_renderer_type++ % 128] = RendererKind{handle, kinds};
    return kinds;
}
bool is_render_kind(uint64_t object, ClassRef& wanted) {
    const uint8_t bit = &wanted == &particle_class ? 1 : &wanted == &decal_class ? 2 : 4;
    return (render_kinds(object) & bit) != 0;
}

// Where the objects of a class keep one of their fields. Found once for the
// class that declares it and kept while that class is the same object.
struct KnownField {
    ClassRef& type;
    const char* name;
    uint64_t type_handle = 0, property = 0, offset = 0;
};
KnownField visible_field{scene_class, "bVisible"}, overrides_field{mesh_class, "OverrideMaterials"};
KnownField mode_field{skeletal_class, "AnimationMode"}, emitter_materials{particle_class, "EmitterMaterials"};
// What a mesh that plays one clip starts that clip from again whenever the engine registers the mesh anew,
// as a change of its mobility does. The clip calls below keep these the same as what they ask for.
KnownField saved_looping{skeletal_class, "AnimationData.bSavedLooping"}, saved_position{skeletal_class, "AnimationData.SavedPosition"},
    saved_rate{skeletal_class, "AnimationData.SavedPlayRate"};

bool place_of(KnownField& field, uint64_t object, Place& place) {
    const uint64_t type = find_class(field.type);
    if (!type) return false;
    if (field.type_handle != to_handle(type)) {
        Place found;
        if (find_place(type, object, field.name, found) != TSUCHI_OK) return false;
        field.type_handle = to_handle(type), field.property = found.property, field.offset = found.address - object;
    }
    place = Place{};
    place.address = object + field.offset;
    place.property = field.property;
    return true;
}

bool write_known(KnownField& field, uint64_t object, const Value& value) {
    Place place;
    return place_of(field, object, place) && write_value(place, value) == TSUCHI_OK;
}

} // namespace

bool is_engine_actor(uint64_t object) { return is_a(object, actor_class); }
bool is_part(uint64_t object) { return is_a(object, part_class); }
bool is_scene_part(uint64_t object) { return is_a(object, scene_class); }

uint64_t part_owner(uint64_t part) { return read_or<uint64_t>(part + t::scene_component::owner); }

bool each_part(uint64_t actor, void (*visit)(void*, uint64_t), void* context) {
    const uint64_t every = find_class(part_class);
    if (!every) return false;
    EngineCall call(actor, "K2_GetComponentsByClass");
    uint32_t count = 0;
    if (call.set("ComponentClass", object_value(to_handle(every))) != TSUCHI_OK || call.invoke() != TSUCHI_OK ||
        call.objects("ReturnValue", count) != TSUCHI_OK)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        if (const uint64_t part = call.object_at("ReturnValue", i)) visit(context, part);
    return true;
}

bool destroy_part(uint64_t part) {
    EngineCall destroy(part, "K2_DestroyComponent");
    // The engine lets a part be destroyed by itself or by its actor.
    return destroy.set("Object", object_value(to_handle(part))) == TSUCHI_OK && destroy.invoke() == TSUCHI_OK;
}

TSUCHI_Result add_part(uint64_t actor, uint64_t type, uint64_t parent, const char* socket, uint64_t& part) {
    part = 0;
    const uint64_t every = find_class(part_class), placed = find_class(scene_class);
    if (!every || !placed) return TSUCHI_NOT_READY;
    if (!is_a(type, class_class) || !derives_from(type, every)) return TSUCHI_INVALID;
    if (parent && (!derives_from(type, placed) || !is_scene_part(parent) || part_owner(parent) != actor)) return TSUCHI_INVALID;
    {
        EngineCall add(actor, "AddComponentByClass");
        if (add.ready() != TSUCHI_OK) return TSUCHI_NOT_READY;
        // Where it is told to go is where it is, with no turn and its own size.
        const bool given = add.set("Class", object_value(to_handle(type))) == TSUCHI_OK && add.set("bManualAttachment", boolean_value(parent != 0)) == TSUCHI_OK &&
                           add.set("RelativeTransform.Rotation.W", number_value(1)) == TSUCHI_OK && add.set("RelativeTransform.Scale3D.X", number_value(1)) == TSUCHI_OK &&
                           add.set("RelativeTransform.Scale3D.Y", number_value(1)) == TSUCHI_OK && add.set("RelativeTransform.Scale3D.Z", number_value(1)) == TSUCHI_OK &&
                           add.set("bDeferredFinish", boolean_value(false)) == TSUCHI_OK;
        Value made;
        if (!given || add.invoke() != TSUCHI_OK || add.get("ReturnValue", made) != TSUCHI_OK) return TSUCHI_NOT_READY;
        part = made.kind == ValueKind::object ? from_handle(made.object) : 0;
    }
    if (!part) return TSUCHI_UNSUPPORTED;
    if (!parent) return TSUCHI_OK;
    EngineCall attach(part, "K2_AttachToComponent");
    Value attached;
    if (attach.set("Parent", object_value(to_handle(parent))) == TSUCHI_OK && (!socket || !*socket || attach.set_text("SocketName", socket) == TSUCHI_OK) &&
        attach.invoke() == TSUCHI_OK && attach.get("ReturnValue", attached) == TSUCHI_OK && attached.kind == ValueKind::boolean && attached.boolean)
        return TSUCHI_OK;
    destroy_part(part);
    part = 0;
    return TSUCHI_UNSUPPORTED;
}

bool is_mesh_part(uint64_t object) { return is_a(object, mesh_class); }
bool is_material(uint64_t object) { return is_a(object, material_class); }
bool is_material_image(uint64_t object) { return !is_part(object) && is_render_kind(object, image_class); }
bool is_shader_target(uint64_t object) {
    if (is_mesh_part(object)) return true;
    const uint8_t kinds = render_kinds(object);
    return (kinds & 3) || ((kinds & 4) && !is_part(object));
}

bool material_overrides(uint64_t part, uint64_t& data, uint32_t& count) {
    Place place;
    KnownField* field = is_mesh_part(part) ? &overrides_field : is_render_kind(part, particle_class) ? &emitter_materials : nullptr;
    ArrayMemory array;
    if (!field || !place_of(*field, part, place) || array_of(place, array) != TSUCHI_OK || array.count < 0) return false;
    data = array.data;
    count = data ? uint32_t(array.count) : 0;
    return true;
}

bool material_slots(uint64_t part, uint32_t& count) {
    if (!is_mesh_part(part) && (is_render_kind(part, decal_class) || is_material_image(part))) { count = 1; return true; }
    EngineCall ask(part, "GetNumMaterials");
    Value value;
    if (ask.invoke() != TSUCHI_OK || ask.get("ReturnValue", value) != TSUCHI_OK || value.kind != ValueKind::integer || value.integer < 0) return false;
    count = uint32_t(value.integer);
    return true;
}

uint64_t image_resource(uint64_t image, uint32_t slot) {
    Place place; Value value;
    if (slot || find_place(class_of(image), image, "Brush.ResourceObject", place) != TSUCHI_OK ||
        read_value(place, value) != TSUCHI_OK || value.kind != ValueKind::object) return 0;
    return from_handle(value.object);
}
uint64_t slot_material(uint64_t part, uint32_t slot) {
    const bool mesh = is_mesh_part(part);
    if (!mesh && is_material_image(part)) {
        const uint64_t material = image_resource(part, slot);
        return is_material(material) ? material : 0;
    }
    const bool decal = !mesh && is_render_kind(part, decal_class);
    if (decal && slot) return 0;
    EngineCall ask(part, decal ? "GetDecalMaterial" : "GetMaterial");
    Value value;
    if ((!decal && ask.set("ElementIndex", integer_value(slot)) != TSUCHI_OK) || ask.invoke() != TSUCHI_OK || ask.get("ReturnValue", value) != TSUCHI_OK) return 0;
    return value.kind == ValueKind::object ? from_handle(value.object) : 0;
}

uint64_t slot_override(uint64_t part, uint32_t slot) {
    if (!is_mesh_part(part)) {
        if (is_material_image(part)) return image_resource(part, slot);
        if (is_render_kind(part, decal_class)) return slot_material(part, slot);
    }
    uint64_t data = 0; uint32_t count = 0;
    if (!material_overrides(part, data, count) || slot >= count) return 0;
    const uint64_t material = read_or<uint64_t>(data + 8 * uint64_t(slot));
    return material && live(material) ? material : 0;
}

static bool write_slot_material(uint64_t part, uint32_t slot, uint64_t material, CallGuard guard) {
    if (!guard.permits()) return false;
    if (!is_mesh_part(part) && (is_render_kind(part, decal_class) || is_material_image(part))) {
        if (slot) return false;
        const bool image = is_material_image(part);
        EngineCall set(part, image ? "SetBrushResourceObject" : "SetDecalMaterial", guard);
        return set.set(image ? "ResourceObject" : "NewDecalMaterial", material ? object_value(to_handle(material)) : Value{}) == TSUCHI_OK &&
               set.invoke() == TSUCHI_OK;
    }
    EngineCall set(part, "SetMaterial", guard);
    // An argument left alone is empty, which is how the engine is told to take the material away.
    return set.set("ElementIndex", integer_value(slot)) == TSUCHI_OK && (!material || set.set("Material", object_value(to_handle(material))) == TSUCHI_OK) &&
           set.invoke() == TSUCHI_OK;
}

bool set_slot_material(uint64_t part, uint32_t slot, uint64_t material) {
    return write_slot_material(part, slot, material, {});
}

bool part_visible(uint64_t part, bool& visible) {
    Place place;
    Value value;
    if (!is_scene_part(part) || !place_of(visible_field, part, place) || read_value(place, value) != TSUCHI_OK || value.kind != ValueKind::boolean) return false;
    visible = value.boolean;
    return true;
}

bool is_skeletal_part(uint64_t object) { return is_a(object, skeletal_class); }
bool is_clip(uint64_t object) { return is_a(object, clip_class); }

bool animation_mode(uint64_t part, uint32_t& mode) {
    Place place;
    Value value;
    if (!place_of(mode_field, part, place) || read_value(place, value) != TSUCHI_OK || value.kind != ValueKind::integer) return false;
    mode = uint32_t(value.integer);
    return true;
}

bool set_animation_mode(uint64_t part, uint32_t mode) {
    EngineCall call(part, "SetAnimationMode");
    return call.set("InAnimationMode", integer_value(mode)) == TSUCHI_OK && call.invoke() == TSUCHI_OK;
}

bool play_clip_on(uint64_t part, uint64_t clip, bool looping, float rate) {
    // Both calls have their memory and arguments before the clip begins, so a clip that begins also gets its rate.
    EngineCall play(part, "PlayAnimation"), speed(part, "SetPlayRate");
    if (play.set("NewAnimToPlay", object_value(to_handle(clip))) != TSUCHI_OK || play.set("bLooping", boolean_value(looping)) != TSUCHI_OK ||
        speed.set("Rate", number_value(rate)) != TSUCHI_OK)
        return false;
    return play.invoke() == TSUCHI_OK && speed.invoke() == TSUCHI_OK && write_known(saved_looping, part, boolean_value(looping)) &&
           write_known(saved_position, part, number_value(0)) && write_known(saved_rate, part, number_value(rate));
}

bool set_clip_rate_on(uint64_t part, float rate) {
    EngineCall call(part, "SetPlayRate");
    return call.set("Rate", number_value(rate)) == TSUCHI_OK && call.invoke() == TSUCHI_OK && write_known(saved_rate, part, number_value(rate));
}

bool seek_clip_on(uint64_t part, float seconds) {
    EngineCall call(part, "SetPosition");
    return call.set("InPos", number_value(seconds)) == TSUCHI_OK && call.set("bFireNotifies", boolean_value(false)) == TSUCHI_OK && call.invoke() == TSUCHI_OK &&
           write_known(saved_position, part, number_value(seconds));
}

bool clip_state(uint64_t part, float& position, float& rate, bool& playing) {
    EngineCall where(part, "GetPosition"), speed(part, "GetPlayRate"), going(part, "IsPlaying");
    Value at, fast, on;
    if (where.invoke() != TSUCHI_OK || where.get("ReturnValue", at) != TSUCHI_OK || at.kind != ValueKind::number) return false;
    if (speed.invoke() != TSUCHI_OK || speed.get("ReturnValue", fast) != TSUCHI_OK || fast.kind != ValueKind::number) return false;
    if (going.invoke() != TSUCHI_OK || going.get("ReturnValue", on) != TSUCHI_OK || on.kind != ValueKind::boolean) return false;
    position = float(at.number), rate = float(fast.number), playing = on.boolean;
    return true;
}

bool set_part_visible(uint64_t part, bool visible) {
    EngineCall call(part, "SetVisibility");
    return call.set("bNewVisibility", boolean_value(visible)) == TSUCHI_OK && call.set("bPropagateToChildren", boolean_value(false)) == TSUCHI_OK &&
           call.invoke() == TSUCHI_OK;
}

bool restore_destroy_part(uint64_t part, CallGuard guard) {
    EngineCall destroy(part, "K2_DestroyComponent", guard);
    return guard.permits() && destroy.set("Object", object_value(to_handle(part))) == TSUCHI_OK && destroy.invoke() == TSUCHI_OK;
}

bool restore_slot_material(uint64_t part, uint32_t slot, uint64_t material, CallGuard guard) {
    return write_slot_material(part, slot, material, guard);
}

bool restore_part_visible(uint64_t part, bool visible, CallGuard guard) {
    EngineCall set(part, "SetVisibility", guard);
    return set.set("bNewVisibility", boolean_value(visible)) == TSUCHI_OK &&
        set.set("bPropagateToChildren", boolean_value(false)) == TSUCHI_OK && set.invoke() == TSUCHI_OK;
}

bool restore_animation_mode(uint64_t part, uint32_t mode, CallGuard guard) {
    EngineCall set(part, "SetAnimationMode", guard);
    return set.set("InAnimationMode", integer_value(mode)) == TSUCHI_OK && set.invoke() == TSUCHI_OK;
}

} // namespace tsuchi::game
