// Entities shown along fed pose tracks, against a made-up game. Host evidence, not a game run.
#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>

#include "actor_events.h"
#include "actors.h"
#include "entity_tracks.h"
#include "frame.h"
#include "ground_contact.h"
#include "objects.h"
#include "ordinary_lifecycle.h"
#include "presenter_scene.h"
#include "registry.h"

namespace {
struct Body {
    bool alive = true, entity = true;
    float location[3]{}, angles[3]{};
    int placed = 0;
    bool awake = true;      // it has an awake body of its own that a velocity request moves
    float requested[3]{};   // the velocity across the ground last asked of its body
    int requests = 0;
    uint64_t at() const { return reinterpret_cast<uint64_t>(this); }
};
Body first, second, scenery;
std::vector<void (*)()> steps;
void (*left)(uint64_t, uint64_t, tsuchi::ActorLeft) = nullptr;
Body* body(uint64_t at) {
    for (Body* b : {&first, &second, &scenery})
        if (b->alive && at == b->at()) return b;
    return nullptr;
}

// A block of the documented format.
struct Block {
    std::vector<uint8_t> bytes;
    uint16_t count = 0;
    explicit Block(float time) {
        bytes.push_back(1);
        put(time);
        put(uint16_t(0));
    }
    template <typename T> void put(T value) {
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof value);
    }
    Block& add(uint32_t key, const float* location, const float* yaw_pitch_roll, const float* velocity) {
        put(key);
        put(uint8_t((location ? 1 : 0) | (yaw_pitch_roll ? 2 : 0) | (velocity ? 4 : 0)));
        if (location) for (int i = 0; i < 3; ++i) put(location[i]);
        if (yaw_pitch_roll) for (int i = 0; i < 3; ++i) put(int16_t(lroundf(yaw_pitch_roll[i] * 100)));
        if (velocity) for (int i = 0; i < 3; ++i) put(velocity[i]);
        ++count;
        memcpy(bytes.data() + 5, &count, 2);
        return *this;
    }
};
uint32_t feed(TSUCHI_Mod* mod, const Block& block, TSUCHI_Result expect = TSUCHI_OK) {
    uint32_t taken = 0;
    assert(tsuchi::track_feed(mod, block.bytes.data(), block.bytes.size(), &taken) == expect);
    return taken;
}
uint32_t show(TSUCHI_Mod* mod, float now) {
    uint32_t moved = 0;
    assert(tsuchi::track_show(mod, now, &moved) == TSUCHI_OK);
    return moved;
}
void step() { for (auto call : steps) call(); }
bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }
} // namespace

namespace tsuchi::game {
uint64_t from_handle(uint64_t handle) { return body(handle) ? handle : 0; }
uint64_t to_handle(uint64_t object) { return body(object) ? object : 0; }
bool live(uint64_t object) { return body(object) != nullptr; }
bool is_game_actor(uint64_t object) { return body(object) && body(object)->entity; }
uint64_t part_owner(uint64_t part) { return part; }
bool get_transform(uint64_t actor, Transform& out) {
    Body* b = body(actor);
    if (!b) return false;
    memcpy(out.location, b->location, sizeof out.location);
    memcpy(out.angles, b->angles, sizeof out.angles);
    return true;
}
float velocity_of_first[3] = {5, -6, 7};
bool get_velocity(uint64_t actor, float out[3]) {
    if (actor != first.at()) return false;   // the second one has no movement of its own
    memcpy(out, velocity_of_first, 12);
    return true;
}
bool request_ground_velocity(uint64_t actor, const float velocity[3]) {
    Body* b = body(actor);
    if (!b || !b->awake) return false;
    b->requested[0] = velocity[0], b->requested[1] = velocity[1], b->requested[2] = 0;
    ++b->requests;
    return true;
}
TSUCHI_Result set_transform(uint64_t actor, const Transform& to) {
    Body* b = body(actor);
    if (!b) return TSUCHI_NOT_FOUND;
    memcpy(b->location, to.location, sizeof b->location);
    memcpy(b->angles, to.angles, sizeof b->angles);
    ++b->placed;
    return TSUCHI_OK;
}
} // namespace tsuchi::game

namespace tsuchi {
bool each_frame(void (*call)()) { steps.push_back(call); return true; }
bool on_actor_left(void (*call)(uint64_t, uint64_t, ActorLeft)) { left = call; return true; }
void ordinary_lifecycle_listener(void (*)(uint64_t, uint64_t, bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_OK; }
uint64_t ordinary_lifecycle_restore_part(uint64_t handle, uint64_t) { return game::from_handle(handle); }
void ground_contact_forget(uint64_t) {}
int cuts = 0;
void presenter_cut(uint64_t) { ++cuts; }
} // namespace tsuchi

int main() {
    tsuchi::note_game_thread();
    auto* mod = tsuchi::mod_create("test.tracks");
    auto* other = tsuchi::mod_create("test.other");
    const TSUCHI_Actor a{first.at()}, b{second.at()}, plain{scenery.at()};
    scenery.entity = false;

    // Holding: a key from 1, a game entity, one actor per key, one mod per actor.
    assert(tsuchi::track_hold(mod, a, 0) == TSUCHI_INVALID);
    assert(tsuchi::track_hold(mod, plain, 3) == TSUCHI_INVALID);
    assert(tsuchi::track_hold(mod, a, 7) == TSUCHI_OK);
    assert(tsuchi::track_hold(mod, b, 7) == TSUCHI_BUSY);
    assert(tsuchi::track_hold(other, a, 2) == TSUCHI_BUSY);
    assert(tsuchi::track_hold(mod, b, 8) == TSUCHI_OK);

    // Nothing fed: nothing moves, and there is nothing to read.
    assert(show(mod, 1.0f) == 0);
    TSUCHI_Track read{};
    assert(tsuchi::track_read(mod, 7, &read) == TSUCHI_NOT_FOUND);

    // A malformed block is refused whole.
    Block bad(1.0f);
    const float far[3] = {100, 0, 0};
    bad.add(7, far, nullptr, nullptr);
    bad.bytes.push_back(0);
    feed(mod, bad, TSUCHI_INVALID);
    Block zero_key(1.0f);
    zero_key.add(0, far, nullptr, nullptr);
    feed(mod, zero_key, TSUCHI_INVALID);
    assert(tsuchi::track_read(mod, 7, &read) == TSUCHI_NOT_FOUND);

    // Fed at time 1: read back as fed, rotation in hundredths of a degree.
    const float yaw90[3] = {0, 90, 0}, still[3] = {0, 0, 0}, moving[3] = {100, 0, 0};
    Block one(1.0f);
    one.add(7, far, yaw90, still).add(8, far, nullptr, moving).add(9, far, nullptr, nullptr);
    assert(feed(mod, one) == 3);
    assert(tsuchi::track_read(mod, 7, &read) == TSUCHI_OK && near(read.location.x, 100) && near(read.rotation.yaw, 90));
    assert(tsuchi::track_read(mod, 9, &read) == TSUCHI_OK);   // a key needs no actor
    assert(tsuchi::track_read(other, 7, &read) == TSUCHI_NOT_FOUND);   // a mod's keys are its own

    // The first showings ease from where the actor stood: a sixth of the way, then a fifth of what is left...
    assert(show(mod, 1.0f) == 2);
    assert(near(first.location[0], 100.0f / 6) && near(first.angles[1], 90));
    for (int i = 0; i < 5; ++i) show(mod, 1.0f);
    assert(near(first.location[0], 100));

    // A showing is a movement, not a move at once: the pictures between steps are never cut for it.
    assert(tsuchi::cuts == 0);

    // Still: not moved again. Carried on by its velocity, at most 0.2 s, after a 1/60 s buffer.
    const int placed = first.placed;
    assert(show(mod, 1.0f) == 0 && first.placed == placed);
    show(mod, 1.1f);
    assert(near(second.location[0], 100 + 100 * (0.1f - 1.0f / 60)));
    show(mod, 5.0f);
    assert(near(second.location[0], 120));

    // An older part than the track has is left out; a newer one is taken.
    Block older(0.5f);
    const float back[3] = {0, 0, 0};
    older.add(7, back, nullptr, nullptr);
    feed(mod, older);
    assert(tsuchi::track_read(mod, 7, &read) == TSUCHI_OK && near(read.location.x, 100));
    Block newer(2.0f);
    const float on[3] = {150, 0, 0};
    newer.add(7, on, nullptr, nullptr);
    feed(mod, newer);
    assert(show(mod, 2.0f) >= 1 && near(first.location[0], 150) && near(first.angles[1], 90));

    // Pushed: put back at the next showing, each time. Under a millimeter: left.
    first.location[1] = 0.05f;
    show(mod, 2.0f);
    assert(near(first.location[1], 0.05f));
    for (int i = 0; i < 3; ++i) {
        first.location[1] = 0.5f;
        assert(show(mod, 2.0f) == 1 && near(first.location[1], 0));
    }
    assert(tsuchi::cuts == 0);

    // Its body is given the velocity of its motion along the track since the last showing, across the ground. It
    // walks 10 cm a step (300 cm/s), fed places that stand (velocity 0), and its body moves at its track's speed.
    {
        const int before = first.requests;
        float x = first.location[0];
        for (int k = 1; k <= 6; ++k) {
            const float time = 2.0f + k / 30.0f;
            x += 10;
            const float place[3] = {x, 0, 0};
            Block walk(time);
            walk.add(7, place, nullptr, still);
            feed(mod, walk);
            show(mod, time);
            if (k > 1) assert(near(first.requested[0], 300) && near(first.requested[1], 0) && first.requested[2] == 0);
        }
        assert(first.requests == before + 6 && near(first.location[0], x));
        // It stops: shown standing, its body is brought to rest at once, though nothing moves it again.
        for (int k = 7; k <= 10; ++k) {
            const float time = 2.0f + k / 30.0f;
            const float place[3] = {x, 0, 0};
            Block stand(time);
            stand.add(7, place, nullptr, still);
            feed(mod, stand);
            const int placed_before = first.placed;
            show(mod, time);
            assert(first.requested[0] == 0 && first.requested[1] == 0 && first.placed == placed_before);
        }
        assert(first.requests == before + 10);
        // Shown again at the same time, nothing more is asked; with no awake body, nothing is asked at all.
        show(mod, 2.0f + 10 / 30.0f);
        assert(first.requests == before + 10);
        first.awake = false;
        const float place[3] = {x + 10, 0, 0};
        Block on_again(2.0f + 11 / 30.0f);
        on_again.add(7, place, nullptr, still);
        feed(mod, on_again);
        show(mod, 2.0f + 11 / 30.0f);
        assert(first.requests == before + 10 && near(first.location[0], x + 10));
        first.awake = true;
    }

    // Released: no longer moved. Forgotten: not found.
    assert(tsuchi::track_release(mod, a) == TSUCHI_OK);
    assert(tsuchi::track_release(mod, a) == TSUCHI_NOT_FOUND);
    Block later(3.0f);
    const float away[3] = {400, 0, 0};
    later.add(7, away, nullptr, nullptr);
    feed(mod, later);
    show(mod, 3.0f);
    assert(near(first.location[0], 220));
    assert(tsuchi::track_forget(mod, 7) == TSUCHI_OK && tsuchi::track_read(mod, 7, &read) == TSUCHI_NOT_FOUND);
    assert(tsuchi::track_forget(mod, 7) == TSUCHI_NOT_FOUND);

    // Many poses at once: a record each, in order, the track layout without its key.
    {
        first.location[0] = 10; first.location[1] = 20; first.location[2] = 30;
        first.angles[0] = 0; first.angles[1] = 270; first.angles[2] = 0;
        second.location[0] = -1; second.location[1] = -2; second.location[2] = -3;
        second.angles[1] = 45;
        const uint64_t handles[4] = {first.at(), second.at(), scenery.at(), 0};
        uint8_t records[4 * tsuchi::kPoseRecordBytes];
        uint64_t got = 0;
        assert(tsuchi::track_read_poses(handles, sizeof handles, records, sizeof records, &got) == TSUCHI_OK);
        assert(got == 4 * tsuchi::kPoseRecordBytes);
        float f[3];
        int16_t yaw = 0;
        assert(records[0] == 7);
        memcpy(f, records + 1, 12);
        assert(near(f[0], 10) && near(f[2], 30));
        memcpy(&yaw, records + 15, 2);
        assert(yaw == -9000);   // 270 degrees, wrapped
        memcpy(f, records + 19, 12);
        assert(near(f[0], 5) && near(f[2], 7));
        const uint8_t* s = records + tsuchi::kPoseRecordBytes;
        assert(s[0] == 3);      // no velocity
        memcpy(f, s + 19, 12);
        assert(f[0] == 0 && f[1] == 0);
        assert(records[2 * tsuchi::kPoseRecordBytes] == 0 && records[3 * tsuchi::kPoseRecordBytes] == 0);
        assert(tsuchi::track_read_poses(handles, 7, records, sizeof records, &got) == TSUCHI_INVALID);
        assert(tsuchi::track_read_poses(handles, sizeof handles, records, sizeof records - 1, &got) == TSUCHI_INVALID);
        assert(tsuchi::track_read_poses(handles, 0, records, 0, &got) == TSUCHI_OK && got == 0);
    }

    // An actor that leaves ends its hold at the next frame; a mod that stops forgets its tracks.
    second.alive = false;
    step();
    second.alive = true;
    assert(tsuchi::track_release(mod, b) == TSUCHI_NOT_FOUND);
    tsuchi::mod_stop(mod);
    auto* again = tsuchi::mod_create("test.tracks");
    assert(tsuchi::track_read(again, 8, &read) == TSUCHI_NOT_FOUND);
    tsuchi::mod_stop(again);
    tsuchi::mod_stop(other);
    return 0;
}
