// The carry service reads the game's own carrier records and selected receiver.
// It does not infer delivery from an arrival state or from an actor leaving play.
#include "actors.h"
#include "carry_service.h"
#include "content.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "objects.h"
#include "registry.h"

namespace tsuchi {
namespace {
namespace t = game::table;
game::ClassRef carry_class{t::name::carry_component_class};
game::ClassRef stick_class{t::name::carry_stick_class};

TSUCHI_Result entity_of(TSUCHI_Actor actor, uint64_t& out) {
    out = game::from_handle(actor.id);
    if (!out) return TSUCHI_NOT_FOUND;
    return game::is_game_actor(out) ? TSUCHI_OK : TSUCHI_INVALID;
}

uint64_t component_of(uint64_t actor) {
    const uint64_t part = game::read_or<uint64_t>(actor + t::carry_actor::component);
    return part && game::live(part) && game::is_a(part, carry_class) ? part : 0;
}

struct Carriers { uint64_t data; int32_t count, maximum; };
bool records(uint64_t component, Carriers& out) {
    const uint64_t at = component + t::carry_component::carriers;
    return game::read(at + t::array::data, out.data) && game::read(at + t::array::count, out.count) &&
           game::read(at + t::array::max, out.maximum) && out.count >= 0 && out.maximum >= out.count &&
           uint32_t(out.count) <= t::constant::max_objects &&
           (!out.count || (out.data && game::readable(out.data, uint64_t(out.count) * sizeof(uint64_t))));
}

uint64_t carrier(const Carriers& entries, int32_t at) {
    const uint64_t actor = game::read_or<uint64_t>(entries.data + uint64_t(at) * sizeof(uint64_t));
    return actor && game::live(actor) && game::is_game_actor(actor) ? game::to_handle(actor) : 0;
}

// Ordinary attached Pikmin are in the stick component's attachment list, not
// necessarily in the carry component's carrier array; the game's carrier
// resolver visits both. An attachment's inverse relationship and an active
// Carry or CarryAI action exclude other kinds of attachment, as the game does.
bool carrying_action(uint64_t actor) {
    const uint64_t brain = game::ai_component(actor);
    const uint64_t tree = brain ? game::read_or<uint64_t>(brain + t::ai_component::actions) : 0;
    const uint64_t action = tree ? game::read_or<uint64_t>(tree + t::action::child) : 0;
    game::Name name{};
    return action && game::read(action + t::action::name, name) &&
           (game::name_is(name, t::name::carry_action) || game::name_is(name, t::name::carry_ai_action));
}

struct CarrierView {
    Carriers registered{};
    uint64_t actor = 0, stick = 0, head = 0, first = 0, last = 0;
    int32_t attachments = 0;
};

bool carrier_view(uint64_t actor, uint64_t component, CarrierView& view) {
    view.actor = actor;
    if (!records(component, view.registered)) return false;
    if (!game::read(actor + t::game_actor::stick, view.stick)) return false;
    if (!view.stick) return true;
    if (!game::live(view.stick) || !game::is_a(view.stick, stick_class)) return false;
    view.head = view.stick + t::carry_stick::attachments;
    if (!game::read(view.head + t::carry_attachment_list::previous, view.last) ||
        !game::read(view.head + t::carry_attachment_list::next, view.first) ||
        !game::read(view.head + t::carry_attachment_list::count, view.attachments) ||
        view.attachments < 0 || uint32_t(view.attachments) > t::constant::max_objects ||
        !view.first || !view.last) return false;
    return view.attachments ? view.first != view.head && view.last != view.head
                            : view.first == view.head && view.last == view.head;
}

bool in_array(const Carriers& entries, uint64_t handle, int32_t before) {
    for (int32_t i = 0; i < before; ++i) if (carrier(entries, i) == handle) return true;
    return false;
}

template <class Visit>
bool each_carrier(const CarrierView& view, Visit visit) {
    for (int32_t i = 0; i < view.registered.count; ++i) {
        const uint64_t handle = carrier(view.registered, i);
        if (handle && !in_array(view.registered, handle, i)) visit(handle);
    }
    if (!view.stick) return true;
    uint64_t node = view.first, previous = view.head;
    // The native count and reciprocal links bound the walk. Corrupt or cyclic
    // lists fail before the caller publishes a count or writes its buffer.
    for (int32_t i = 0; i < view.attachments; ++i) {
        uint64_t before = 0, next = 0, actor = 0, owner = 0;
        if (node == view.head ||
            !game::read(node + t::carry_attachment_node::previous, before) || before != previous ||
            !game::read(node + t::carry_attachment_node::next, next) || !next ||
            !game::read(node + t::carry_attachment_node::actor, actor) ||
            !game::read(node + t::carry_attachment_node::owner, owner) || owner != view.head) return false;
        const uint64_t handle = actor && game::live(actor) && game::is_game_actor(actor) ? game::to_handle(actor) : 0;
        const uint64_t stick = handle ? game::read_or<uint64_t>(actor + t::game_actor::stick) : 0;
        if (stick && game::live(stick) && game::is_a(stick, stick_class) &&
            node == stick + t::carry_stick::member &&
            game::read_or<uint64_t>(stick + t::carry_stick::owner) == actor &&
            game::read_or<uint64_t>(stick + t::stick_component::target) == view.actor &&
            game::read_or<uint64_t>(stick + t::carry_stick::target_stick) == view.stick &&
            carrying_action(actor) && !in_array(view.registered, handle, view.registered.count)) visit(handle);
        previous = node;
        node = next;
    }
    return node == view.head && previous == view.last;
}

TSUCHI_Result carry_of(TSUCHI_Actor actor, uint64_t& component) {
    uint64_t object = 0;
    const TSUCHI_Result found = entity_of(actor, object);
    if (found != TSUCHI_OK) return found;
    component = component_of(object);
    return component ? TSUCHI_OK : TSUCHI_UNSUPPORTED;
}

struct Objects { TSUCHI_Actor* buffer; uint64_t capacity, count; };
bool collect(void* context, uint64_t actor) {
    auto& out = *static_cast<Objects*>(context);
    if (!component_of(actor)) return true;
    const uint64_t handle = game::to_handle(actor);
    if (!handle) return true;
    if (out.count < out.capacity) out.buffer[out.count].id = handle;
    ++out.count;
    return true;
}

TSUCHI_Result list(TSUCHI_Actor* buffer, uint64_t capacity, uint64_t* out) {
    if (!game::game_framework()) return TSUCHI_NOT_READY;
    Objects counter{nullptr, 0, 0};
    game::each_actor(collect, &counter);
    *out = counter.count;
    if (counter.count > capacity) return TSUCHI_CAPACITY;
    Objects writer{buffer, counter.count, 0};
    game::each_actor(collect, &writer);
    return TSUCHI_OK;
}

TSUCHI_Result info(TSUCHI_Actor actor, TSUCHI_CarryInfo* out) {
    uint64_t component = 0;
    const TSUCHI_Result found = carry_of(actor, component);
    if (found != TSUCHI_OK) return found;
    TSUCHI_CarryInfo value{};
    CarrierView view{};
    uint64_t routing = 0, destination = 0;
    if (!carrier_view(game::from_handle(actor.id), component, view) ||
        !game::read(component + t::carry_component::weight, value.weight) ||
        !game::read(component + t::carry_component::maximum, value.maximum) ||
        value.weight < 0 || value.maximum < 0 ||
        !game::read(component + t::carry_component::info, routing) ||
        (routing && !game::read(routing + t::carry_info::destination, destination))) return TSUCHI_NOT_READY;
    if (!each_carrier(view, [&](uint64_t) { ++value.count; })) return TSUCHI_NOT_READY;
    if (destination && game::live(destination) && game::is_game_actor(destination)) value.destination.id = game::to_handle(destination);
    *out = value;
    return TSUCHI_OK;
}

TSUCHI_Result carriers(TSUCHI_Actor actor, TSUCHI_Actor* buffer, uint64_t capacity, uint64_t* out) {
    uint64_t component = 0;
    const TSUCHI_Result found = carry_of(actor, component);
    if (found != TSUCHI_OK) return found;
    CarrierView view{};
    if (!carrier_view(game::from_handle(actor.id), component, view)) return TSUCHI_NOT_READY;
    uint32_t count = 0;
    if (!each_carrier(view, [&](uint64_t) { ++count; })) return TSUCHI_NOT_READY;
    *out = count;
    if (count > capacity) return TSUCHI_CAPACITY;
    uint32_t written = 0;
    return each_carrier(view, [&](uint64_t handle) { buffer[written++].id = handle; }) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

struct Search { uint64_t carrier, object; TSUCHI_Result result; };
bool find_carried(void* context, uint64_t actor) {
    auto& search = *static_cast<Search*>(context);
    const uint64_t component = component_of(actor);
    if (!component) return true;
    CarrierView view{};
    if (!carrier_view(actor, component, view) ||
        !each_carrier(view, [&](uint64_t handle) {
            if (handle == search.carrier) search.object = game::to_handle(actor);
        })) { search.result = TSUCHI_NOT_READY; return false; }
    return !search.object;
}

TSUCHI_Result carried_by(TSUCHI_Actor actor, TSUCHI_Actor* out) {
    uint64_t object = 0;
    const TSUCHI_Result found = entity_of(actor, object);
    if (found != TSUCHI_OK) return found;
    if (!game::game_framework()) return TSUCHI_NOT_READY;
    Search search{actor.id, 0, TSUCHI_OK};
    game::each_actor(find_carried, &search);
    if (search.result != TSUCHI_OK) return search.result;
    out->id = search.object;
    return TSUCHI_OK;
}

const carry::Backend service{list, info, carriers, carried_by};

// Collection credit is distinct from arrival. The native dispatcher is reached
// by the Onion and ship receiver message handlers. Preserve its return and both
// conditional output pointers, and capture identity before collection can pool.
struct Delivery { uint64_t actor, destination; char kind[game::kNameBytes]; int32_t count; };
Delivery* pending = nullptr;
uint32_t pending_count = 0, pending_capacity = 0;
using Credit = decltype(t::code_site::carry_credit)::Function;
Credit next_credit = nullptr;

void queue(const Delivery& delivery) {
    if (pending_count == pending_capacity) {
        const uint32_t next = pending_capacity ? pending_capacity * 2 : 16;
        if (next < pending_capacity) return;
        auto* grown = static_cast<Delivery*>(allocate(sizeof(Delivery) * next));
        if (!grown) return;
        if (pending) memcpy(grown, pending, sizeof(Delivery) * pending_count);
        release(pending);
        pending = grown;
        pending_capacity = next;
    }
    pending[pending_count++] = delivery;
}

int32_t credit(bool story, void* actor_pointer, int32_t count, uint32_t color, void* special, void* notify) {
    const uint64_t actor = reinterpret_cast<uint64_t>(actor_pointer);
    Delivery delivery{};
    if (on_game_thread() && carry::delivered_wanted() && actor && game::is_game_actor(actor)) {
        delivery.actor = game::to_handle(actor);
        const uint64_t component = component_of(actor);
        const uint64_t routing = component ? game::read_or<uint64_t>(component + t::carry_component::info) : 0;
        uint64_t destination = routing ? game::read_or<uint64_t>(routing + t::carry_info::arrival_destination) : 0;
        if (!destination && routing) destination = game::read_or<uint64_t>(routing + t::carry_info::destination);
        if (destination && game::is_game_actor(destination)) delivery.destination = game::to_handle(destination);
        uint64_t bytes = 0;
        if (game::kind_into(game::actor_kind(actor), delivery.kind, sizeof delivery.kind, &bytes) != TSUCHI_OK) delivery.actor = 0;
    }
    const int32_t result = next_credit(story, actor_pointer, count, color, special, notify);
    if (delivery.actor) { delivery.count = result; queue(delivery); }
    return result;
}

void deliver() {
    const uint32_t ready = pending_count;
    for (uint32_t i = 0; i < ready; ++i) {
        const Delivery delivery = pending[i];
        carry::emit_delivered({{delivery.actor}, {delivery.destination}, delivery.kind, delivery.count});
    }
    if (pending_count > ready) memmove(pending, pending + ready, sizeof(Delivery) * (pending_count - ready));
    pending_count -= ready;
    if (!pending_count && !carry::delivered_wanted()) {
        release(pending);
        pending = nullptr;
        pending_capacity = 0;
    }
}
} // namespace

void carry_start(TSUCHI_Mod* self) {
    carry::set_backend(&service);
    uint64_t id = 0;
    hook_site(self, t::code_site::carry_credit, credit, &next_credit, &id);
    each_frame(deliver);
}
} // namespace tsuchi
