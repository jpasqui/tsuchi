// The components service: the parts an actor is made of, hiding them, and adding to them.
#include "added.h"
#include "families.h"
#include "game_table.h"
#include "component_service.h"
#include "hidden.h"
#include "instance_cull.h"
#include "materials.h"
#include "names.h"
#include "objects.h"
#include "parts.h"
#include "services.h"

namespace tsuchi {
namespace {

uint64_t actor_of(TSUCHI_Actor actor) {
    const uint64_t object = game::from_handle(actor.id);
    return object && game::is_engine_actor(object) ? object : 0;
}
uint64_t part_of(TSUCHI_Component component) {
    const uint64_t object = game::from_handle(component.id);
    return object && game::is_part(object) ? object : 0;
}

// Counts every part and writes those that fit.
struct Collector {
    TSUCHI_Component* buffer;
    uint64_t capacity, count;
};
void collect(void* context, uint64_t part) {
    Collector& c = *static_cast<Collector*>(context);
    if (c.count < c.capacity) c.buffer[c.count].id = game::to_handle(part);
    ++c.count;
}

TSUCHI_Result of(TSUCHI_Actor actor, TSUCHI_Component* buffer, uint64_t capacity, uint64_t* out) {
    const uint64_t address = actor_of(actor);
    if (!address) return TSUCHI_NOT_FOUND;
    // Counted first, so a buffer that is too small is left as it was.
    Collector counter{nullptr, 0, 0};
    if (!game::each_part(address, collect, &counter)) return TSUCHI_NOT_READY;
    *out = counter.count;
    if (counter.count > capacity) return TSUCHI_CAPACITY;
    Collector writer{buffer, capacity, 0};
    return game::each_part(address, collect, &writer) && writer.count == counter.count ? TSUCHI_OK : TSUCHI_NOT_READY;
}

struct Search {
    const char* name;
    uint64_t found;
};
bool same_name(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const char x = *a >= 'A' && *a <= 'Z' ? char(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? char(*b + 32) : *b;
        if (x != y) return false;
        if (!x) return true;
    }
}
void match(void* context, uint64_t part) {
    Search& search = *static_cast<Search*>(context);
    char name[game::kNameBytes];
    if (!search.found && game::name_text(game::name_of(part), name, sizeof name) && same_name(name, search.name)) search.found = part;
}

TSUCHI_Result find(TSUCHI_Actor actor, const char* name, TSUCHI_Component* out) {
    const uint64_t address = actor_of(actor);
    if (!address) return TSUCHI_NOT_FOUND;
    Search search{name, 0};
    if (!game::each_part(address, match, &search)) return TSUCHI_NOT_READY;
    if (!search.found) return TSUCHI_NOT_FOUND;
    out->id = game::to_handle(search.found);
    return TSUCHI_OK;
}

TSUCHI_Result owner(TSUCHI_Component component, TSUCHI_Actor* out) {
    const uint64_t part = part_of(component);
    const uint64_t actor = part ? game::part_owner(part) : 0;
    if (!actor || !game::is_engine_actor(actor)) return TSUCHI_NOT_FOUND;
    out->id = game::to_handle(actor);
    return TSUCHI_OK;
}

TSUCHI_Result hide(TSUCHI_Mod* mod, TSUCHI_Component component, bool children) {
    const uint64_t part = part_of(component);
    return part ? hide_part(mod, part, children) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result show(TSUCHI_Mod* mod, TSUCHI_Component component) {
    const uint64_t part = part_of(component);
    return part ? show_part(mod, part) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result hidden(TSUCHI_Component component, bool* out) {
    const uint64_t part = part_of(component);
    if (!part) return TSUCHI_NOT_FOUND;
    *out = part_hidden(part);
    return TSUCHI_OK;
}

TSUCHI_Result add(TSUCHI_Mod* mod, TSUCHI_Actor actor, TSUCHI_Object type, TSUCHI_Component parent, const char* socket, TSUCHI_Component* out) {
    const uint64_t address = actor_of(actor), kind = game::from_handle(type.id), under = parent.id ? part_of(parent) : 0;
    if (!address || !kind || (parent.id && !under)) return TSUCHI_NOT_FOUND;
    uint64_t part = 0;
    const TSUCHI_Result result = add_part_for(mod, address, kind, under, socket, part);
    if (result == TSUCHI_OK) out->id = game::to_handle(part);
    return result;
}

TSUCHI_Result remove(TSUCHI_Mod* mod, TSUCHI_Component component) {
    const uint64_t part = part_of(component);
    return part ? remove_part_for(mod, part) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result get_material(TSUCHI_Component component, uint32_t slot, TSUCHI_Object* out) {
    const uint64_t part = part_of(component);
    if (!part) return TSUCHI_NOT_FOUND;
    if (!game::is_mesh_part(part)) return TSUCHI_UNSUPPORTED;
    uint32_t slots = 0;
    if (!game::material_slots(part, slots)) return TSUCHI_NOT_READY;
    if (slot >= slots) return TSUCHI_INVALID;
    const uint64_t material = game::slot_material(part, slot);
    if (!material) return TSUCHI_NOT_FOUND;
    out->id = game::to_handle(material);
    return TSUCHI_OK;
}

TSUCHI_Result set_material(TSUCHI_Mod* mod, TSUCHI_Component component, uint32_t slot, TSUCHI_Object material) {
    const uint64_t part = part_of(component), with = game::from_handle(material.id);
    return part && with ? hold_material(mod, part, slot, with) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result restore_material(TSUCHI_Mod* mod, TSUCHI_Component component, uint32_t slot) {
    const uint64_t part = part_of(component);
    return part ? tsuchi::restore_material(mod, part, slot) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result hold_cull_scale(TSUCHI_Mod* mod, TSUCHI_Component component, float scale) {
    const uint64_t part = part_of(component);
    return part ? hold_instance_cull_scale(mod, part, scale) : TSUCHI_NOT_FOUND;
}
TSUCHI_Result release_cull_scale(TSUCHI_Mod* mod, TSUCHI_Component component) {
    const uint64_t part = part_of(component);
    return part ? release_instance_cull_scale(mod, part) : TSUCHI_NOT_FOUND;
}

const component::Backend service{of,     find,         owner,        hide,             show,              hidden,          add,
                                  remove, get_material, set_material, restore_material, hold_cull_scale, release_cull_scale};

} // namespace

void components_start() {
    component::set_backend(&service);
    hidden_start();
    added_start();
    materials_start();
    if (!family_absent(game::table::family::instance_cull)) instance_cull_start();
}

} // namespace tsuchi
