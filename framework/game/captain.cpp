#include "captain.h"

#include "actors.h"
#include "game.h"
#include "objects.h"
#include "people.h"
#include "pikmin.h"

namespace tsuchi::game {
namespace {

namespace t = table;

ClassRef avatar_class{t::name::avatar_class};

void* pointer(uint64_t address) { return reinterpret_cast<void*>(address); }

uint64_t whistle_part(uint64_t part) {
    const uint64_t whistle = read_or<uint64_t>(part + t::avatar_component::whistle);
    return whistle && live(whistle) ? whistle : 0;
}

// The cursor in use, which the game picks: the captain's, or its dog's while it rides.
uint64_t cursor_of(uint64_t captain) {
    const uint64_t part = captain_part(captain);
    if (!part || !t::bound(t::fn::captain_cursor)) return 0;
    return reinterpret_cast<uint64_t>(t::fn::captain_cursor(pointer(part)));
}

} // namespace

uint64_t captain_part(uint64_t actor) {
    const uint64_t part = ai_component(actor);
    return part && is_a(part, avatar_class) ? part : 0;
}

bool read_whistle(uint64_t captain, Whistle& out) {
    const uint64_t part = captain_part(captain), whistle = part ? whistle_part(part) : 0;
    namespace w = t::whistle_part;
    uint8_t sounding = 0;
    if (!whistle || !read(part + t::avatar_component::whistling, sounding) || !read(whistle + w::radius, out.radius) ||
        !read(whistle + w::center, out.center) || !read(whistle + w::count, out.count))
        return false;
    out.active = sounding != 0;
    return true;
}

bool start_whistle(uint64_t captain) {
    const uint64_t part = captain_part(captain);
    if (!part || !whistle_part(part) || !t::bound(t::fn::start_whistle)) return false;
    t::fn::start_whistle(pointer(part), false);
    return true;
}

void end_whistle(uint64_t captain) {
    const uint64_t part = captain_part(captain);
    if (part && whistle_part(part) && t::fn::end_whistle) t::fn::end_whistle(pointer(part));
}

bool read_cursor(uint64_t captain, float out[3]) {
    const uint64_t cursor = cursor_of(captain);
    float point[3];
    if (!cursor || !read(cursor + t::cursor::position, point)) return false;
    for (int i = 0; i < 3; ++i) out[i] = point[i];
    return true;
}

TSUCHI_Result place_cursor(uint64_t captain, const float point[3]) {
    const uint64_t cursor = cursor_of(captain);
    Transform at;
    if (!cursor || !get_transform(captain, at)) return TSUCHI_NOT_READY;
    // The game keeps the cursor as a step across the ground from the captain, and works out the point from that.
    const float step[3] = {point[0] - at.location[0], point[1] - at.location[1], 0};
    const float shown[3] = {point[0], point[1], point[2]};
    return write(cursor + t::cursor::offset, step) && write(cursor + t::cursor::position, shown) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

TSUCHI_Result throw_pikmin(uint64_t captain, uint64_t pikmin) {
    const uint64_t part = captain_part(captain);
    if (!part || !is_pikmin(pikmin)) return TSUCHI_NOT_FOUND;
    if (!t::bound(t::fn::pikmin_leader) || !t::bound(t::fn::squad_selected) || !t::bound(t::fn::squad_select_of) || !t::bound(t::fn::start_throw)) return TSUCHI_NOT_READY;
    const uint64_t leader = pikmin_leader(pikmin);
    if (!leader || (leader != captain && leader != partner_dog(captain))) return TSUCHI_INVALID;
    // The squad it throws from is its own, or its dog's while it rides: the captain's own function says which.
    const auto ask = method(part, t::avatar_table::squad);
    const uint64_t squad = ask ? ask(pointer(part)) : 0;
    if (!squad) return TSUCHI_NOT_READY;
    // A captain lets go at once of a Pikmin that is not of the type it has picked.
    const int32_t color = read_or<uint8_t>(pikmin + t::game_actor::color);
    if (t::fn::squad_selected(pointer(squad)) != color) {
        t::fn::squad_select_of(pointer(squad), pointer(pikmin));
        if (t::fn::squad_selected(pointer(squad)) != color) return TSUCHI_BUSY;
    }
    t::fn::start_throw(pointer(part), pointer(pikmin), nullptr);
    return TSUCHI_OK;
}

uint64_t partner_dog(uint64_t captain) {
    const uint64_t steering = read_or<uint64_t>(captain + t::game_actor::player);
    const uint64_t dog = steering ? read_or<uint64_t>(steering + t::player_component::ridden_dog) : 0;
    return dog && is_game_actor(dog) ? dog : 0;
}

uint64_t mount_of(uint64_t captain) { return is_riding(captain) ? partner_dog(captain) : 0; }

bool ride_asked(uint64_t captain) {
    const uint64_t part = captain_part(captain);
    return part && read_or<uint8_t>(part + t::avatar_component::ride_asked);
}

TSUCHI_Result ride(uint64_t captain) {
    const uint64_t part = captain_part(captain), dog = partner_dog(captain);
    if (!dog || !t::bound(t::fn::ride_allowed) || !t::bound(t::fn::request_ride)) return TSUCHI_NOT_READY;
    if (is_riding(captain) || ride_asked(captain)) return TSUCHI_BUSY;
    Transform here, there;
    float reach = 0;
    if (!get_transform(captain, here) || !get_transform(dog, there) || !read(part + t::avatar_component::ride_reach, reach)) return TSUCHI_NOT_READY;
    const float dx = here.location[0] - there.location[0], dy = here.location[1] - there.location[1];
    if (dx * dx + dy * dy > reach * reach) return TSUCHI_NOT_READY;
    if (!t::fn::ride_allowed(pointer(dog), pointer(captain), true)) return TSUCHI_NOT_READY;
    t::fn::request_ride(pointer(part));
    return TSUCHI_OK;
}

bool settle_ride(uint64_t captain) {
    const uint64_t part = captain_part(captain);
    if (!part || !ride_asked(captain)) return true;
    if (!is_riding(captain)) return false;
    write(part + t::avatar_component::ride_asked, uint8_t(0));
    return true;
}

TSUCHI_Result dismount(uint64_t captain) {
    const uint64_t dog = mount_of(captain);
    const uint64_t steering = dog ? read_or<uint64_t>(dog + t::game_actor::player) : 0;
    if (!steering || !live(steering) || !t::bound(t::fn::request_get_off)) return TSUCHI_NOT_READY;
    t::fn::request_get_off(pointer(steering));
    return TSUCHI_OK;
}

} // namespace tsuchi::game
