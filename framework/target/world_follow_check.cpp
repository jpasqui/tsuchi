// A finite check that a spawned creature walks the points returned by world.path.
// Native movement is used only here, in the framework's own check build.
#if TSUCHI_SELFCHECK
#include "world_follow_check.h"

#include <string.h>

#include "game.h"
#include "host.h"
#include "names.h"
#include "objects.h"
#include "reflect.h"
#include "registry.h"

extern "C" unsigned long long tsuchi_libc_ticks();

namespace tsuchi::target {
namespace {

namespace t = game::table;
constexpr uint32_t kSteps = 1200, kPoints = 128;
constexpr float kArrivalSquared = 125 * 125, kTravelSquared = 200 * 200;

struct Check {
    TSUCHI_Mod* owner = nullptr;
    TSUCHI_Mod* observer = nullptr;
    TSUCHI_Actor creature{};
    TSUCHI_Pose captain{}, start{}, last{};
    TSUCHI_Vec3 finish{}, points[kPoints]{};
    game::Name state_name{};
    uint64_t count = 0;
    uint32_t phase = 0, steps = 0, control_wait = 0, waited = 0, waypoint = 0;
    uint32_t queries = 0, requests = 0, arrivals = 0, cleanup_wait = 0, storage_tries = 0;
    TSUCHI_Result path_result = TSUCHI_NOT_READY, request_result = TSUCHI_NOT_READY, storage_result = TSUCHI_NOT_READY;
    float max_displacement_squared = 0, minimum_finish_squared = 0, final_finish_squared = 0;
    bool passed = false, cleanup_alive = false;
    const char* reason = "waiting_for_control";
};
Check check{};

// Bounded decimal observations without the target's missing formatted I/O.
struct Line {
    char text[1536]{};
    uint32_t used = 0;
    void add(const char* word) {
        for (uint32_t i = 0; word[i] && used + 1 < sizeof text; ++i) text[used++] = word[i];
        text[used] = 0;
    }
    void number(int64_t value) {
        char digits[20];
        uint32_t count = 0;
        uint64_t magnitude = value < 0 ? uint64_t(-(value + 1)) + 1 : uint64_t(value);
        if (value < 0) add("-");
        do { digits[count++] = char('0' + magnitude % 10); magnitude /= 10; } while (magnitude && count < sizeof digits);
        while (count && used + 1 < sizeof text) text[used++] = digits[--count];
        text[used] = 0;
    }
};

float squared(const TSUCHI_Vec3& a, const TSUCHI_Vec3& b) {
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return x * x + y * y + z * z;
}

void log(const char* event, uint32_t index, const TSUCHI_Vec3& point) {
    if (!check.observer) return;
    Line line;
    line.add("WORLD_FOLLOW "); line.add(event);
    line.add(" step="); line.number(check.steps);
    line.add(" waypoint="); line.number(index);
    line.add(" xyz_cm="); line.number(int64_t(point.x));
    line.add(","); line.number(int64_t(point.y));
    line.add(","); line.number(int64_t(point.z));
    host.mod->log(check.observer, line.text);
}

void finish(bool passed, const char* reason) {
    check.passed = passed;
    check.reason = reason;
    check.phase = 4;
    log(passed ? "walk_complete" : reason, check.waypoint, check.last.location);
    // This owner owns only the check's creature and loaded class reference.
    // Stopping it releases the existing shared spawn hold and removes the creature.
    if (check.owner) mod_stop(check.owner);
}

void store() {
    Line line;
    line.add("{\"passed\":"); line.add(check.passed ? "true" : "false");
    line.add(",\"reason\":\""); line.add(check.reason); line.add("\"");
    line.add(",\"steps\":"); line.number(check.steps);
    line.add(",\"path_queries\":"); line.number(check.queries);
    line.add(",\"path_result\":"); line.number(check.path_result);
    line.add(",\"path_points\":"); line.number(int64_t(check.count));
    line.add(",\"requests\":"); line.number(check.requests);
    line.add(",\"request_result\":"); line.number(check.request_result);
    line.add(",\"arrivals\":"); line.number(check.arrivals);
    line.add(",\"max_displacement_squared_cm\":"); line.number(int64_t(check.max_displacement_squared));
    line.add(",\"minimum_finish_squared_cm\":"); line.number(int64_t(check.minimum_finish_squared));
    line.add(",\"final_finish_squared_cm\":"); line.number(int64_t(check.final_finish_squared));
    line.add(",\"start_cm\":["); line.number(int64_t(check.start.location.x));
    line.add(","); line.number(int64_t(check.start.location.y)); line.add(","); line.number(int64_t(check.start.location.z));
    line.add("],\"last_cm\":["); line.number(int64_t(check.last.location.x));
    line.add(","); line.number(int64_t(check.last.location.y)); line.add(","); line.number(int64_t(check.last.location.z));
    line.add("],\"finish_cm\":["); line.number(int64_t(check.finish.x));
    line.add(","); line.number(int64_t(check.finish.y)); line.add(","); line.number(int64_t(check.finish.z));
    line.add("],\"cleanup_alive\":"); line.add(check.cleanup_alive ? "true" : "false");
    line.add(",\"warps\":0,\"coordinate_rounding_cm\":1}");
    check.storage_result = host.storage->write(check.observer, "observations.json", line.text, strlen(line.text));
    if (check.storage_result == TSUCHI_OK || ++check.storage_tries >= 30) {
        Line result;
        result.add("WORLD_FOLLOW observations_storage_result="); result.number(check.storage_result);
        result.add(" passed="); result.add(check.passed ? "true" : "false");
        result.add(" reason="); result.add(check.reason);
        host.mod->log(check.observer, result.text);
        mod_stop(check.observer);
        check.phase = 5;
    }
}

template <class T>
void argument(unsigned char* bytes, uint64_t offset, const T& value) {
    memcpy(bytes + offset, &value, sizeof value);
}

TSUCHI_Result request(const TSUCHI_Vec3& point) {
    const uint64_t actor = game::from_handle(check.creature.id);
    if (!actor || !mod_usable(check.owner) || !on_game_thread()) return TSUCHI_NOT_FOUND;
    const uint64_t ai = game::read_or<uint64_t>(actor + t::game_actor::ai);
    const uint64_t fsm = ai ? game::read_or<uint64_t>(ai + t::world_follow_ai::fsm) : 0;
    const uint64_t mover = game::read_or<uint64_t>(actor + t::game_actor::movement);
    const uint64_t root = mover ? game::read_or<uint64_t>(mover + t::world_follow_mover::root) : 0;
    if (!fsm || !root || !t::bound(t::fn::world_follow_request_move)) return TSUCHI_NOT_READY;
    if (game::read_or<uint8_t>(fsm + t::world_follow_fsm::locked, 1)) return TSUCHI_BUSY;
    const game::Name default_name = game::global<game::Name>(t::data::world_follow_default_name);
    const game::Name none_name = game::global<game::Name>(t::data::world_follow_none_name);
    const game::Name move_name = game::global<game::Name>(t::data::world_follow_move_name);
    if (!default_name.index || !move_name.index) return TSUCHI_NOT_READY;
    alignas(8) unsigned char args[t::world_follow_args::most]{};
    const uint64_t no_target = uint64_t(UINT32_MAX);
    const uint8_t stop = 1;
    argument(args, t::world_follow_args::position, point);
    argument(args, t::world_follow_args::target, no_target);
    argument(args, t::world_follow_args::default_name, default_name);
    argument(args, t::world_follow_args::start_motion, none_name);
    argument(args, t::world_follow_args::walk_motion, move_name);
    argument(args, t::world_follow_args::stop, stop);
    const uint64_t name = uint64_t(check.state_name.index) | (uint64_t(check.state_name.number) << 32);
    t::fn::world_follow_request_move(reinterpret_cast<void*>(fsm), name, args);
    // A request's return does not establish movement. Confirm the entered state name,
    // then later require measured displacement and every requested waypoint's arrival.
    const uint64_t child = game::read_or<uint64_t>(fsm + t::action::child);
    game::Name entered{};
    if (!child || !game::read(child + t::action::name, entered)) return TSUCHI_NOT_READY;
    return entered.index == check.state_name.index && entered.number == check.state_name.number ? TSUCHI_OK : TSUCHI_BUSY;
}

void start() {
    if (!host.world || !host.asset || !host.entity || !host.player || !host.game || !host.storage || !host.mod) return;
    if (!check.observer) check.observer = mod_create("tsuchi.world-follow-observe");
    if (!check.observer) { check.phase = 5; return; }
    // A build whose move request is not in the table (the Switch 2 demo's takes other arguments) cannot walk the path.
    if (!t::bound(t::fn::world_follow_request_move) || !t::present(t::world_follow_ai::fsm) || !t::present(t::world_follow_mover::root)) {
        finish(false, "native_move_request_unavailable"); return;
    }
    bool control = false;
    TSUCHI_Actor captain{};
    if (host.game->has_control(check.observer, &control) != TSUCHI_OK || !control ||
        host.player->captain(check.observer, 0, &captain) != TSUCHI_OK || host.entity->pose(check.observer, captain, &check.captain) != TSUCHI_OK)
        return;
    check.owner = mod_create("tsuchi.world-follow-check");
    if (!check.owner) { finish(false, "owner_unavailable"); return; }
    // The target's existing clock reports microseconds. Measure one play through
    // the public table, including its guards and shared hold admission.
    TSUCHI_Effect timed{};
    const uint64_t before_play = tsuchi_libc_ticks();
    const TSUCHI_Result played = host.effects->play(check.owner, "PS_eft_Demo_Pikmin_HitPurpleSmoke00", &check.captain.location, &timed);
    const uint64_t play_micros = tsuchi_libc_ticks() - before_play;
    Line timing;
    timing.add("WORLD_EFFECTS play_microseconds="); timing.number(int64_t(play_micros));
    timing.add(" result="); timing.number(played);
    host.mod->log(check.observer, timing.text);
    if (played == TSUCHI_OK) host.effects->stop(check.owner, timed);
    const TSUCHI_Result named = game::name_from_utf8("TsuchiWorldFollowCheck", check.state_name);
    if (named != TSUCHI_OK) { finish(false, "state_name_unavailable"); return; }
    const TSUCHI_Vec3 extent{150, 150, 800};
    TSUCHI_Vec3 spawn = check.captain.location;
    spawn.x -= 350; spawn.y += 250;
    check.finish = check.captain.location;
    check.finish.x += 350; check.finish.y += 250;
    TSUCHI_Vec3 ground{}, finish_ground{};
    if (host.world->ground(check.owner, &spawn, &extent, &ground) != TSUCHI_OK ||
        host.world->ground(check.owner, &check.finish, &extent, &finish_ground) != TSUCHI_OK) {
        finish(false, "ground_query_failed"); return;
    }
    check.finish = finish_ground;
    spawn = ground; spawn.z += 30;
    TSUCHI_Object type{};
    const TSUCHI_Result loaded = host.asset->load(check.owner, "/Game/Carrot4/Placeables/Teki/GKochappy.GKochappy_C", &type);
    if (loaded != TSUCHI_OK) { finish(false, "creature_class_unavailable"); return; }
    if (host.entity->spawn(check.owner, type, &spawn, &check.captain.rotation, -1, false, &check.creature) != TSUCHI_OK) {
        finish(false, "creature_spawn_failed"); return;
    }
    check.phase = 1; check.waited = 0;
    log("spawned", 0, spawn);
}

void get_path() {
    if (++check.waited < 30) return;
    if (host.entity->pose(check.owner, check.creature, &check.start) != TSUCHI_OK) { finish(false, "creature_pose_unavailable"); return; }
    check.last = check.start;
    check.path_result = host.world->path(check.owner, &check.start.location, &check.finish, check.points, kPoints, &check.count);
    ++check.queries;
    if (check.path_result != TSUCHI_OK) {
        if (check.queries < 2) { check.waited = 0; return; }
        finish(false, "complete_path_unavailable"); return;
    }
    if (check.count < 2 || check.count > kPoints) { finish(false, "path_point_count_invalid"); return; }
    check.finish = check.points[check.count - 1];
    check.minimum_finish_squared = check.final_finish_squared = squared(check.start.location, check.finish);
    if (check.final_finish_squared < 300 * 300) { finish(false, "route_too_short"); return; }
    check.phase = 2;
    log("complete_path", uint32_t(check.count), check.finish);
}

void follow() {
    if (host.entity->pose(check.owner, check.creature, &check.last) != TSUCHI_OK) { finish(false, "creature_disappeared"); return; }
    const float displacement = squared(check.last.location, check.start.location);
    if (displacement > check.max_displacement_squared) check.max_displacement_squared = displacement;
    check.final_finish_squared = squared(check.last.location, check.finish);
    if (check.final_finish_squared < check.minimum_finish_squared) check.minimum_finish_squared = check.final_finish_squared;
    if (check.phase == 3) {
        if (squared(check.last.location, check.points[check.waypoint]) > kArrivalSquared) return;
        ++check.arrivals;
        log("arrived", check.waypoint, check.last.location);
        ++check.waypoint;
        check.phase = 2;
    }
    // A returned corner already within reach needs no movement request.
    for (uint32_t skipped = 0; check.waypoint < check.count && skipped < kPoints; ++skipped) {
        if (squared(check.last.location, check.points[check.waypoint]) > kArrivalSquared) break;
        ++check.waypoint;
    }
    if (check.waypoint >= check.count) {
        const bool walked = check.requests && check.arrivals == check.requests && check.max_displacement_squared >= kTravelSquared &&
                            check.final_finish_squared <= kArrivalSquared;
        finish(walked, walked ? "walked_returned_path" : "insufficient_measured_walk");
        return;
    }
    ++check.requests;
    check.request_result = request(check.points[check.waypoint]);
    log("requested", check.waypoint, check.points[check.waypoint]);
    if (check.request_result != TSUCHI_OK) { finish(false, "native_move_request_refused"); return; }
    check.phase = 3;
}

} // namespace

void world_follow_check_step() {
    if (check.phase == 5) return;
    if (!on_game_thread()) return;
    if (check.phase == 4) {
        check.cleanup_alive = check.creature.id && game::from_handle(check.creature.id);
        if (check.cleanup_alive && ++check.cleanup_wait < 30) return;
        if (check.cleanup_alive) { check.passed = false; check.reason = "creature_cleanup_incomplete"; }
        store();
        return;
    }
    // The movement budget begins after a playable world appears. Title-screen
    // loading gets a separate finite four-minute wait at the game's usual rate.
    const bool timed_out = check.phase ? ++check.steps > kSteps : ++check.control_wait > 7200;
    if (timed_out) {
        if (!check.observer) { check.phase = 5; return; }
        finish(false, check.phase ? "walk_timed_out" : "control_timed_out");
        return;
    }
    if (check.phase == 0) start();
    else if (check.phase == 1) get_path();
    else follow();
}

} // namespace tsuchi::target
#endif
