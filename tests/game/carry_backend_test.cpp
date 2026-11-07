#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "actors.h"
#include "carry_service.h"
#include "content.h"
#include "game.h"
#include "hooks.h"
#include "objects.h"
#include "registry.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
namespace tsuchi { void carry_start(TSUCHI_Mod* self); }
namespace {
namespace t = tsuchi::game::table;
struct Object { uint64_t address, handle; bool entity, component, live, stick; };
struct Range { uint64_t address, bytes; };
std::vector<Object> objects;
std::vector<Range> ranges;
std::vector<uint64_t> actors;
bool world = true;
uint64_t thread = 1;
using Credit = decltype(t::code_site::carry_credit)::Function;
Credit credit_hook = nullptr;
void (*delivery_step)() = nullptr;
bool pool_on_credit = false;
unsigned native_calls = 0, deliveries = 0;
uint64_t delivered_actor = 0, delivered_destination = 0;
int32_t delivered_count = 0;
char delivered_kind[96]{};
uint64_t thread_probe() { return thread; }
Object* object(uint64_t address) { for (auto& entry : objects) if (entry.address == address) return &entry; return nullptr; }
void remember(void* memory, size_t bytes) { ranges.push_back({reinterpret_cast<uint64_t>(memory), bytes}); }
uint64_t add(void* memory, size_t bytes, uint64_t handle, bool entity, bool component = false, bool stick = false) {
    remember(memory, bytes);
    uint64_t address = reinterpret_cast<uint64_t>(memory);
    objects.push_back({address, handle, entity, component, true, stick});
    if (entity) actors.push_back(address);
    return address;
}
template<class T> void put(void* memory, uint64_t offset, T value) { memcpy(static_cast<unsigned char*>(memory) + offset, &value, sizeof value); }
int32_t native_credit(bool story, void* actor, int32_t count, uint32_t color, void* special, void* notify) {
    CHECK(story && count == 3 && color == 5);
    CHECK(special && notify);
    *static_cast<unsigned char*>(special) = 7;
    *static_cast<unsigned char*>(notify) = 9;
    ++native_calls;
    if (pool_on_credit) object(reinterpret_cast<uint64_t>(actor))->live = false;
    return 11;
}
void on_delivered(void*, const TSUCHI_CarryDelivered* event) {
    ++deliveries;
    delivered_actor = event->actor.id;
    delivered_destination = event->destination.id;
    delivered_count = event->count;
    CHECK(strlen(event->kind) < sizeof delivered_kind);
    memcpy(delivered_kind, event->kind, strlen(event->kind) + 1);
}
}
namespace tsuchi {
bool each_frame(void (*step)()) { delivery_step = step; return true; }
TSUCHI_Result hook_code_site(TSUCHI_Mod*, uint32_t site, void* function, uint64_t* id, void** original) {
    CHECK(site == t::code_site::carry_credit.number);
    credit_hook = reinterpret_cast<Credit>(function);
    *id = 1; *original = reinterpret_cast<void*>(native_credit);
    return TSUCHI_OK;
}
}
namespace tsuchi::game {
bool readable(uint64_t address, uint64_t bytes) {
    for (const auto& r : ranges) if (address >= r.address && address - r.address <= r.bytes && bytes <= r.bytes - (address - r.address)) return true;
    return false;
}
bool live(uint64_t address) { auto* entry = object(address); return entry && entry->live; }
uint64_t from_handle(uint64_t handle) { for (auto entry : objects) if (entry.handle == handle && entry.live) return entry.address; return 0; }
uint64_t to_handle(uint64_t address) { auto* entry = object(address); return entry && entry->live ? entry->handle : 0; }
bool is_game_actor(uint64_t address) { auto* entry = object(address); return entry && entry->live && entry->entity; }
bool is_a(uint64_t address, ClassRef& ref) {
    auto* entry = object(address);
    if (!entry || !entry->live) return false;
    if (!strcmp(ref.name, t::name::carry_component_class)) return entry->component;
    CHECK(!strcmp(ref.name, t::name::carry_stick_class));
    return entry->stick;
}
uint64_t ai_component(uint64_t actor) { return read_or<uint64_t>(actor + t::game_actor::ai); }
bool name_is(Name name, const char* text) {
    if (name.number) return false;
    return name.index == 1 ? !strcmp(text, "Carry") : name.index == 2 ? !strcmp(text, "CarryAI") : false;
}
Name actor_kind(uint64_t) { return Name{1, 0}; }
TSUCHI_Result kind_into(Name, char* buffer, uint64_t capacity, uint64_t* out) {
    const char kind[] = "carry.check:treasure";
    *out = sizeof kind;
    if (capacity < sizeof kind) return TSUCHI_CAPACITY;
    memcpy(buffer, kind, sizeof kind);
    return TSUCHI_OK;
}
uint64_t game_framework() { return world ? 1 : 0; }
void each_actor(bool (*visit)(void*, uint64_t), void* context) { for (uint64_t actor : actors) if (live(actor) && !visit(context, actor)) return; }
}
int main() {
    alignas(8) unsigned char treasure[0x400]{}, receiver[0x400]{}, pikmin[0x400]{}, dog[0x400]{}, ordinary[0x400]{}, component[0x650]{}, wrong[0x650]{}, routing[0x100]{};
    uint64_t treasure_address = add(treasure, sizeof treasure, 101, true);
    uint64_t receiver_address = add(receiver, sizeof receiver, 102, true);
    uint64_t pikmin_address = add(pikmin, sizeof pikmin, 103, true);
    uint64_t dog_address = add(dog, sizeof dog, 104, true);
    uint64_t ordinary_address = add(ordinary, sizeof ordinary, 105, false);
    uint64_t component_address = add(component, sizeof component, 106, false, true);
    uint64_t wrong_address = add(wrong, sizeof wrong, 107, false);
    uint64_t raw[] = {pikmin_address, pikmin_address, dog_address, ordinary_address, 0xBAD};
    remember(raw, sizeof raw); remember(routing, sizeof routing);
    put(treasure, t::carry_actor::component, component_address);
    put(receiver, t::carry_actor::component, wrong_address);
    put(component, t::carry_component::weight, int32_t(20));
    put(component, t::carry_component::maximum, int32_t(30));
    put(component, t::carry_component::carriers + t::array::data, reinterpret_cast<uint64_t>(raw));
    put(component, t::carry_component::carriers + t::array::count, int32_t(5));
    put(component, t::carry_component::carriers + t::array::max, int32_t(5));
    put(component, t::carry_component::info, reinterpret_cast<uint64_t>(routing));
    put(routing, t::carry_info::destination, receiver_address);
    tsuchi::set_thread_probe(thread_probe); tsuchi::note_game_thread();
    TSUCHI_Mod* mod = tsuchi::mod_create("carry.check"); CHECK(mod);
    tsuchi::carry_start(mod); CHECK(credit_hook && delivery_step);
    const auto& service = tsuchi::carry::table;
    uint64_t count = 99; TSUCHI_Actor found[2]{{777}, {888}};
    world = false;
    CHECK(service.list(mod, found, 2, &count) == TSUCHI_NOT_READY && count == 99);
    world = true;
    CHECK(service.list(mod, nullptr, 0, &count) == TSUCHI_CAPACITY && count == 1);
    CHECK(service.list(mod, found, 2, &count) == TSUCHI_OK && count == 1 && found[0].id == 101);
    TSUCHI_CarryInfo info{}; info.weight = 77;
    CHECK(service.info(mod, {999}, &info) == TSUCHI_NOT_FOUND && info.weight == 77);
    CHECK(service.info(mod, {105}, &info) == TSUCHI_INVALID && info.weight == 77);
    CHECK(service.info(mod, {102}, &info) == TSUCHI_UNSUPPORTED && info.weight == 77);
    CHECK(service.info(mod, {101}, &info) == TSUCHI_OK && info.weight == 20 && info.maximum == 30 && info.count == 2 && info.destination.id == 102);
    found[0].id = 777;
    CHECK(service.carriers(mod, {101}, found, 1, &count) == TSUCHI_CAPACITY && count == 2 && found[0].id == 777);
    CHECK(service.carriers(mod, {101}, found, 2, &count) == TSUCHI_OK && count == 2 && found[0].id == 103 && found[1].id == 104);
    CHECK(service.carriers(mod, {999}, found, 2, &count) == TSUCHI_NOT_FOUND);
    CHECK(service.carriers(mod, {105}, found, 2, &count) == TSUCHI_INVALID);
    CHECK(service.carriers(mod, {102}, found, 2, &count) == TSUCHI_UNSUPPORTED);
    TSUCHI_Actor carried{777};
    CHECK(service.carried_by(mod, {103}, &carried) == TSUCHI_OK && carried.id == 101);
    CHECK(service.carried_by(mod, {104}, &carried) == TSUCHI_OK && carried.id == 101);
    CHECK(service.carried_by(mod, {102}, &carried) == TSUCHI_OK && carried.id == 0);
    CHECK(service.carried_by(mod, {999}, &carried) == TSUCHI_NOT_FOUND);
    CHECK(service.carried_by(mod, {105}, &carried) == TSUCHI_INVALID);
    world = false;
    CHECK(service.carried_by(mod, {103}, &carried) == TSUCHI_NOT_READY);
    world = true;
    object(receiver_address)->live = false;
    CHECK(service.info(mod, {101}, &info) == TSUCHI_OK && info.destination.id == 0);
    object(dog_address)->live = false;
    CHECK(service.carriers(mod, {101}, found, 2, &count) == TSUCHI_OK && count == 1 && found[0].id == 103);
    put(component, t::carry_component::info, uint64_t(0));
    CHECK(service.info(mod, {101}, &info) == TSUCHI_OK && info.destination.id == 0 && info.count == 1);
    put(component, t::carry_component::carriers + t::array::count, int32_t(-1));
    info.weight = 77; found[0].id = 777; carried.id = 777;
    CHECK(service.info(mod, {101}, &info) == TSUCHI_NOT_READY && info.weight == 77);
    CHECK(service.carriers(mod, {101}, found, 2, &count) == TSUCHI_NOT_READY && found[0].id == 777);
    CHECK(service.carried_by(mod, {103}, &carried) == TSUCHI_NOT_READY && carried.id == 777);
    put(component, t::carry_component::carriers + t::array::count, int32_t(6));
    CHECK(service.carriers(mod, {101}, found, 2, &count) == TSUCHI_NOT_READY);
    put(component, t::carry_component::carriers + t::array::count, int32_t(0));
    put(component, t::carry_component::carriers + t::array::data, uint64_t(0));
    CHECK(service.carriers(mod, {101}, nullptr, 0, &count) == TSUCHI_OK && count == 0);
    CHECK(service.carried_by(mod, {103}, &carried) == TSUCHI_OK && carried.id == 0);
    put(component, t::carry_component::weight, int32_t(-1));
    CHECK(service.info(mod, {101}, &info) == TSUCHI_NOT_READY);
    thread = 2;
    CHECK(service.list(mod, found, 2, &count) == TSUCHI_INVALID);
    CHECK(service.info(mod, {101}, &info) == TSUCHI_INVALID);
    CHECK(service.carriers(mod, {101}, found, 2, &count) == TSUCHI_INVALID);
    CHECK(service.carried_by(mod, {103}, &carried) == TSUCHI_INVALID);
    thread = 1;
    CHECK(service.info(mod, {}, &info) == TSUCHI_INVALID);
    CHECK(service.info(mod, {101}, nullptr) == TSUCHI_INVALID);
    CHECK(service.carriers(mod, {101}, nullptr, 1, &count) == TSUCHI_INVALID);

    // The native attachment source carries ordinary Pikmin while +0x600 can
    // stay empty. Non-carry attachments and stale relationships do not qualify.
    alignas(8) unsigned char target_stick[0x520]{}, pikmin_stick[0x520]{}, dog_stick[0x520]{};
    alignas(8) unsigned char pikmin_ai[0x240]{}, dog_ai[0x240]{}, pikmin_tree[0x80]{}, dog_tree[0x80]{};
    alignas(8) unsigned char pikmin_action[0x90]{}, dog_action[0x90]{};
    const uint64_t target_stick_address = add(target_stick, sizeof target_stick, 108, false, false, true);
    const uint64_t pikmin_stick_address = add(pikmin_stick, sizeof pikmin_stick, 109, false, false, true);
    const uint64_t dog_stick_address = add(dog_stick, sizeof dog_stick, 110, false, false, true);
    remember(pikmin_ai, sizeof pikmin_ai); remember(dog_ai, sizeof dog_ai);
    remember(pikmin_tree, sizeof pikmin_tree); remember(dog_tree, sizeof dog_tree);
    remember(pikmin_action, sizeof pikmin_action); remember(dog_action, sizeof dog_action);
    const uint64_t head = target_stick_address + t::carry_stick::attachments;
    const uint64_t pikmin_node = pikmin_stick_address + t::carry_stick::member;
    const uint64_t dog_node = dog_stick_address + t::carry_stick::member;
    put(treasure, t::game_actor::stick, target_stick_address);
    put(pikmin, t::game_actor::stick, pikmin_stick_address);
    put(dog, t::game_actor::stick, dog_stick_address);
    put(pikmin_stick, t::carry_stick::owner, pikmin_address);
    put(dog_stick, t::carry_stick::owner, dog_address);
    put(pikmin_stick, t::stick_component::target, treasure_address);
    put(dog_stick, t::stick_component::target, treasure_address);
    put(pikmin_stick, t::carry_stick::target_stick, target_stick_address);
    put(dog_stick, t::carry_stick::target_stick, target_stick_address);
    put(pikmin, t::game_actor::ai, reinterpret_cast<uint64_t>(pikmin_ai));
    put(dog, t::game_actor::ai, reinterpret_cast<uint64_t>(dog_ai));
    put(pikmin_ai, t::ai_component::actions, reinterpret_cast<uint64_t>(pikmin_tree));
    put(dog_ai, t::ai_component::actions, reinterpret_cast<uint64_t>(dog_tree));
    put(pikmin_tree, t::action::child, reinterpret_cast<uint64_t>(pikmin_action));
    put(dog_tree, t::action::child, reinterpret_cast<uint64_t>(dog_action));
    put(pikmin_action, t::action::name, tsuchi::game::Name{1, 0});
    put(dog_action, t::action::name, tsuchi::game::Name{2, 0});
    put(pikmin_stick, t::carry_stick::member + t::carry_attachment_node::actor, pikmin_address);
    put(dog_stick, t::carry_stick::member + t::carry_attachment_node::actor, dog_address);
    const auto link = [&](bool with_dog) {
        put(target_stick, t::carry_stick::attachments + t::carry_attachment_list::next, pikmin_node);
        put(target_stick, t::carry_stick::attachments + t::carry_attachment_list::previous, with_dog ? dog_node : pikmin_node);
        put(target_stick, t::carry_stick::attachments + t::carry_attachment_list::count, int32_t(with_dog ? 2 : 1));
        put(pikmin_stick, t::carry_stick::member + t::carry_attachment_node::previous, head);
        put(pikmin_stick, t::carry_stick::member + t::carry_attachment_node::next, with_dog ? dog_node : head);
        put(pikmin_stick, t::carry_stick::member + t::carry_attachment_node::owner, head);
        put(dog_stick, t::carry_stick::member + t::carry_attachment_node::previous, pikmin_node);
        put(dog_stick, t::carry_stick::member + t::carry_attachment_node::next, head);
        put(dog_stick, t::carry_stick::member + t::carry_attachment_node::owner, head);
    };
    put(component, t::carry_component::weight, int32_t(20));
    put(component, t::carry_component::carriers + t::array::count, int32_t(0));
    object(dog_address)->live = true;
    link(false);
    CHECK(service.info(mod, {101}, &info) == TSUCHI_OK && info.count == 1);
    CHECK(service.carriers(mod, {101}, found, 2, &count) == TSUCHI_OK && count == 1 && found[0].id == 103);
    CHECK(service.carried_by(mod, {103}, &carried) == TSUCHI_OK && carried.id == 101);
    link(true);
    CHECK(service.info(mod, {101}, &info) == TSUCHI_OK && info.count == 2);
    CHECK(service.carriers(mod, {101}, found, 2, &count) == TSUCHI_OK && count == 2 && found[0].id == 103 && found[1].id == 104);
    CHECK(service.carried_by(mod, {104}, &carried) == TSUCHI_OK && carried.id == 101);
    put(component, t::carry_component::carriers + t::array::data, reinterpret_cast<uint64_t>(raw));
    put(component, t::carry_component::carriers + t::array::count, int32_t(5));
    CHECK(service.info(mod, {101}, &info) == TSUCHI_OK && info.count == 2);
    CHECK(service.carriers(mod, {101}, found, 2, &count) == TSUCHI_OK && count == 2 && found[0].id == 103 && found[1].id == 104);
    found[0].id = 777;
    CHECK(service.carriers(mod, {101}, found, 1, &count) == TSUCHI_CAPACITY && count == 2 && found[0].id == 777);
    put(component, t::carry_component::carriers + t::array::count, int32_t(0));
    link(false);
    const auto empty = [&]() {
        CHECK(service.info(mod, {101}, &info) == TSUCHI_OK && info.count == 0);
        CHECK(service.carriers(mod, {101}, nullptr, 0, &count) == TSUCHI_OK && count == 0);
        const bool still_live = object(pikmin_address)->live;
        CHECK(service.carried_by(mod, {103}, &carried) == (still_live ? TSUCHI_OK : TSUCHI_NOT_FOUND));
        if (still_live) CHECK(carried.id == 0);
    };
    put(pikmin_action, t::action::name, tsuchi::game::Name{3, 0}); empty();
    put(pikmin_action, t::action::name, tsuchi::game::Name{1, 1}); empty();
    put(pikmin_action, t::action::name, tsuchi::game::Name{1, 0});
    put(pikmin_tree, t::action::child, uint64_t(0)); empty();
    put(pikmin_tree, t::action::child, reinterpret_cast<uint64_t>(pikmin_action));
    put(pikmin_stick, t::stick_component::target, receiver_address); empty();
    put(pikmin_stick, t::stick_component::target, treasure_address);
    put(pikmin_stick, t::carry_stick::target_stick, dog_stick_address); empty();
    put(pikmin_stick, t::carry_stick::target_stick, target_stick_address);
    put(pikmin_stick, t::carry_stick::owner, dog_address); empty();
    put(pikmin_stick, t::carry_stick::owner, pikmin_address);
    object(pikmin_stick_address)->stick = false; empty();
    object(pikmin_stick_address)->stick = true;
    object(pikmin_address)->live = false; empty();
    object(pikmin_address)->live = true;
    object(pikmin_stick_address)->live = false; empty();
    object(pikmin_stick_address)->live = true;
    put(pikmin_stick, t::carry_stick::member + t::carry_attachment_node::actor, ordinary_address); empty();
    put(pikmin_stick, t::carry_stick::member + t::carry_attachment_node::actor, pikmin_address);
    const auto corrupt = [&]() {
        info.count = 777; found[0].id = 777; count = 777; carried.id = 777;
        CHECK(service.info(mod, {101}, &info) == TSUCHI_NOT_READY && info.count == 777);
        CHECK(service.carriers(mod, {101}, found, 2, &count) == TSUCHI_NOT_READY && found[0].id == 777 && count == 777);
        CHECK(service.carried_by(mod, {103}, &carried) == TSUCHI_NOT_READY && carried.id == 777);
    };
    put(pikmin_stick, t::carry_stick::member + t::carry_attachment_node::owner, uint64_t(0)); corrupt(); link(false);
    put(pikmin_stick, t::carry_stick::member + t::carry_attachment_node::previous, pikmin_node); corrupt(); link(false);
    put(pikmin_stick, t::carry_stick::member + t::carry_attachment_node::next, uint64_t(0)); corrupt(); link(false);
    put(pikmin_stick, t::carry_stick::member + t::carry_attachment_node::next, pikmin_node); corrupt(); link(false);
    put(target_stick, t::carry_stick::attachments + t::carry_attachment_list::previous, dog_node); corrupt(); link(false);
    put(target_stick, t::carry_stick::attachments + t::carry_attachment_list::next, uint64_t(0xBAD)); corrupt(); link(false);
    put(target_stick, t::carry_stick::attachments + t::carry_attachment_list::count, int32_t(-1)); corrupt(); link(false);
    put(target_stick, t::carry_stick::attachments + t::carry_attachment_list::count, int32_t(0)); corrupt(); link(false);
    put(target_stick, t::carry_stick::attachments + t::carry_attachment_list::count, int32_t(2)); corrupt(); link(false);
    put(target_stick, t::carry_stick::attachments + t::carry_attachment_list::count, int32_t(t::constant::max_objects + 1)); corrupt(); link(false);
    object(target_stick_address)->stick = false; corrupt();
    object(target_stick_address)->stick = true;
    object(target_stick_address)->live = false; corrupt();
    object(target_stick_address)->live = true;
    put(target_stick, t::carry_stick::attachments + t::carry_attachment_list::next, head);
    put(target_stick, t::carry_stick::attachments + t::carry_attachment_list::previous, head);
    put(target_stick, t::carry_stick::attachments + t::carry_attachment_list::count, int32_t(0));
    empty();
    put(treasure, t::game_actor::stick, uint64_t(0));

    // Collection preserves the native ABI and records pre-pool identity. The
    // preferred arrival receiver differs from the current routing receiver.
    object(receiver_address)->live = true;
    object(dog_address)->live = true;
    put(component, t::carry_component::info, reinterpret_cast<uint64_t>(routing));
    put(routing, t::carry_info::destination, receiver_address);
    put(routing, t::carry_info::arrival_destination, dog_address);
    TSUCHI_Subscription subscription{};
    CHECK(service.on_delivered(mod, on_delivered, nullptr, &subscription) == TSUCHI_OK);
    unsigned char special = 0, notify = 0;
    pool_on_credit = true;
    CHECK(credit_hook(true, treasure, 3, 5, &special, &notify) == 11);
    CHECK(native_calls == 1 && special == 7 && notify == 9 && deliveries == 0);
    CHECK(tsuchi::game::from_handle(101) == 0);
    delivery_step();
    CHECK(deliveries == 1 && delivered_actor == 101 && delivered_destination == 104 && delivered_count == 11);
    CHECK(!strcmp(delivered_kind, "carry.check:treasure"));
    delivery_step(); CHECK(deliveries == 1);
    object(treasure_address)->live = true; pool_on_credit = false;
    put(routing, t::carry_info::arrival_destination, uint64_t(0));
    CHECK(credit_hook(true, treasure, 3, 5, &special, &notify) == 11);
    delivery_step(); CHECK(deliveries == 2 && delivered_destination == 102);
    thread = 2;
    CHECK(credit_hook(true, treasure, 3, 5, &special, &notify) == 11);
    thread = 1;
    delivery_step(); CHECK(deliveries == 2 && native_calls == 3);
    CHECK(credit_hook(true, treasure, 3, 5, &special, &notify) == 11);
    CHECK(tsuchi::cancel(mod, subscription) == TSUCHI_OK);
    delivery_step(); CHECK(deliveries == 2);
    CHECK(credit_hook(true, treasure, 3, 5, &special, &notify) == 11);
    delivery_step(); CHECK(deliveries == 2 && native_calls == 5);
    tsuchi::mod_stop(mod);
    CHECK(service.list(mod, found, 2, &count) == TSUCHI_INVALID);
    CHECK(service.info(mod, {101}, &info) == TSUCHI_INVALID);
    CHECK(service.carriers(mod, {101}, found, 2, &count) == TSUCHI_INVALID);
    CHECK(service.carried_by(mod, {103}, &carried) == TSUCHI_INVALID);
    CHECK(treasure_address != 0);
    puts("carry reads passed");
}
