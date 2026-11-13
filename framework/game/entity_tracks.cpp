// Entities shown along timed pose tracks. A mod feeds timed poses under keys of its own (feed_tracks), holds
// actors to keys (hold_track), and once a step has each held actor put where its track is (show_tracks). It is the
// native form of what a mod would otherwise do with a pose, a velocity and set_pose for every actor, every step.
#include "entity_tracks.h"

#include <string.h>

#include "actors.h"
#include "ground_contact.h"
#include "holds.h"
#include "objects.h"
#include "registry.h"

namespace tsuchi {
namespace {

constexpr uint8_t kFormat = 1;
constexpr uint8_t kLocation = 1, kRotation = 2, kVelocity = 4;
constexpr uint64_t kMostBytes = 65536;
constexpr float kBuffer = 1.0f / 60;   // seconds behind its time a track is shown from, for the feed's jitter
constexpr float kAhead = 0.2f;         // seconds at most a track is carried on by its velocity past its time
constexpr uint8_t kEase = 6;           // showings that ease from where the actor stood when the hold began
constexpr float kMoved = 0.05f;        // cm the shown place has to move to be put again
constexpr float kTurned = 0.05f;       // degrees the shown yaw has to turn
// cm something may push an actor from where it was put before it is put back, looked at on every showing. Tighter
// than a picture shows: an actor whose own game keeps nudging it (a character held only in its movement, with its
// physics running) crept to a looser limit and jumped back, over and over.
constexpr float kDrift = 0.1f;
constexpr uint32_t kMods = 8;
constexpr float kLongestShowing = 1;   // seconds between showings past which the motion between them is not known

// One key's newest parts, each with its own time.
struct Entry {
    uint32_t key;
    uint8_t has;
    float location[3], rotation[3], velocity[3];
    float location_time, rotation_time, velocity_time;
};

// A mod's tracks, in key order.
struct Store {
    TSUCHI_Mod* mod;
    Entry* entries;
    uint32_t count, capacity;
};
Store stores[kMods]{};
const Kind kStore{"entity.track_data"};

void drop_store(void* context, uint64_t) {
    auto* store = static_cast<Store*>(context);
    release(store->entries);
    *store = Store{};
}

Store* store_of(TSUCHI_Mod* mod, bool make) {
    for (Store& store : stores)
        if (store.mod == mod) return &store;
    if (!make) return nullptr;
    for (Store& store : stores) {
        if (store.mod) continue;
        if (own(mod, kStore, 1, drop_store, &store) != TSUCHI_OK) return nullptr;
        store = Store{mod, nullptr, 0, 0};
        return &store;
    }
    return nullptr;
}

// The first entry with a key at or past `key`.
uint32_t lower_bound(const Store& store, uint32_t key) {
    uint32_t low = 0, high = store.count;
    while (low < high) {
        const uint32_t middle = (low + high) / 2;
        if (store.entries[middle].key < key) low = middle + 1;
        else high = middle;
    }
    return low;
}

Entry* find(const Store* store, uint32_t key) {
    if (!store) return nullptr;
    const uint32_t at = lower_bound(*store, key);
    return at < store->count && store->entries[at].key == key ? &store->entries[at] : nullptr;
}

Entry* find_or_add(Store& store, uint32_t key) {
    const uint32_t at = lower_bound(store, key);
    if (at < store.count && store.entries[at].key == key) return &store.entries[at];
    if (store.count == store.capacity) {
        const uint32_t room = store.capacity ? store.capacity * 2 : 64;
        auto* grown = static_cast<Entry*>(allocate(sizeof(Entry) * room));
        if (!grown) return nullptr;
        if (store.entries) memcpy(grown, store.entries, sizeof(Entry) * store.count);
        release(store.entries);
        store.entries = grown;
        store.capacity = room;
    }
    memmove(store.entries + at + 1, store.entries + at, sizeof(Entry) * (store.count - at));
    ++store.count;
    Entry& made = store.entries[at];
    memset(&made, 0, sizeof made);
    made.key = key;
    return &made;
}

// What a hold keeps: its key, where the actor was last put and shown, how far along its easing and its drift
// looks are, and where its track was at the last showing and when.
struct Kept {
    uint32_t key;
    float shown_location[3], shown_rotation[3];
    float placed[3];
    uint8_t shown, ease;
    float last_time, last_location[3];
    uint8_t timed;
};
static_assert(sizeof(Kept) <= kHoldKeptBytes, "a hold's bytes hold it");

TSUCHI_Result apply_track(const Hold& hold, Kept& kept, const uint32_t& key, bool fresh) {
    if (fresh) {
        memset(&kept, 0, sizeof kept);
        kept.ease = kEase;
    }
    kept.key = key;
    return game::from_handle(hold.object) ? TSUCHI_OK : TSUCHI_NOT_FOUND;
}

Holds<Kept, uint32_t, apply_track, nullptr, nullptr> tracked{"entity.track", HoldOn::actor, true};

uint64_t actor_of(TSUCHI_Actor actor, TSUCHI_Result& why) {
    const uint64_t object = game::from_handle(actor.id);
    why = !object ? TSUCHI_NOT_FOUND : game::is_game_actor(object) ? TSUCHI_OK : TSUCHI_INVALID;
    return why == TSUCHI_OK ? object : 0;
}

float wrap(float degrees) {
    float d = degrees;
    for (int turns = 0; d > 180 && turns < 8; ++turns) d -= 360;
    for (int turns = 0; d < -180 && turns < 8; ++turns) d += 360;
    return d;
}

float magnitude(float value) { return value < 0 ? -value : value; }

float distance2(const float a[3], const float b[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

bool finite_all(const float* values, int count) {
    for (int i = 0; i < count; ++i)
        if (!finite(values[i])) return false;
    return true;
}

// Little-endian reads that move along the data.
struct Reader {
    const uint8_t* at;
    const uint8_t* end;
    bool ok = true;
    void bytes(void* out, uint64_t count) {
        if (!ok || uint64_t(end - at) < count) {
            ok = false;
            memset(out, 0, count);
            return;
        }
        memcpy(out, at, count);
        at += count;
    }
    template <typename T> T get() {
        T value{};
        bytes(&value, sizeof value);
        return value;
    }
};

struct Record {
    uint32_t key;
    uint8_t parts;
    float location[3], rotation[3], velocity[3];
};

bool read_record(Reader& in, Record& out) {
    out.key = in.get<uint32_t>();
    out.parts = in.get<uint8_t>();
    if (out.parts & kLocation) in.bytes(out.location, sizeof out.location);
    if (out.parts & kRotation)
        for (float& angle : out.rotation) angle = float(in.get<int16_t>()) / 100;
    if (out.parts & kVelocity) in.bytes(out.velocity, sizeof out.velocity);
    return in.ok && out.key && !(out.parts & ~(kLocation | kRotation | kVelocity)) && finite_all(out.location, 3) &&
        finite_all(out.velocity, 3);
}

// Moves an actor along its track. Unlike set_pose, the pictures between steps are not cut: a track is a movement,
// and an actor put a little farther each step slides there as one the game moves does. Cut every step, it was
// shown at the steps' rate while everything around it was shown at the screen's. A track that jumps is told by the
// presenter itself, as for any actor (its jump rule).
bool place(uint64_t actor, const float location[3], const float rotation[3]) {
    ground_contact_forget(actor);
    game::Transform to{};
    memcpy(to.location, location, sizeof to.location);
    memcpy(to.angles, rotation, sizeof to.angles);
    return game::set_transform(actor, to) == TSUCHI_OK;
}

} // namespace

void track_ahead(const float location[3], const float velocity[3], float time, float now, float out[3]) {
    float ahead = now - time - kBuffer;
    ahead = ahead < 0 ? 0 : ahead > kAhead ? kAhead : ahead;
    for (int i = 0; i < 3; ++i) out[i] = location[i] + velocity[i] * ahead;
}

TSUCHI_Result track_hold(TSUCHI_Mod* mod, TSUCHI_Actor actor, uint32_t key) {
    if (!key) return TSUCHI_INVALID;
    TSUCHI_Result why = TSUCHI_OK;
    if (!actor_of(actor, why)) return why;
    for (uint32_t i = 0; i < tracked.count(); ++i) {
        const Hold& hold = tracked.hold_at(i);
        if (hold.mod == mod && hold.object != actor.id && tracked.kept_at(i).key == key) return TSUCHI_BUSY;
    }
    return tracked.take(mod, actor.id, 0, key);
}

TSUCHI_Result track_release(TSUCHI_Mod* mod, TSUCHI_Actor actor) {
    TSUCHI_Result why = TSUCHI_OK;
    if (!actor_of(actor, why) && why == TSUCHI_INVALID) return why;
    return tracked.release(mod, actor.id, 0);
}

TSUCHI_Result track_feed(TSUCHI_Mod* mod, const void* data, uint64_t size, uint32_t* out) {
    if (!data || size > kMostBytes) return TSUCHI_INVALID;
    Reader check{static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size};
    const uint8_t format = check.get<uint8_t>();
    const float time = check.get<float>();
    const uint16_t count = check.get<uint16_t>();
    if (!check.ok || format != kFormat || !finite(time)) return TSUCHI_INVALID;
    // The whole block is checked before any of it is taken.
    Reader body = check;
    for (uint16_t i = 0; i < count; ++i) {
        Record record{};
        if (!read_record(check, record)) return TSUCHI_INVALID;
    }
    if (check.at != check.end) return TSUCHI_INVALID;
    Store* store = store_of(mod, true);
    if (!store) return TSUCHI_NO_MEMORY;
    uint32_t taken = 0;
    for (uint16_t i = 0; i < count; ++i) {
        Record record{};
        read_record(body, record);
        Entry* entry = find_or_add(*store, record.key);
        if (!entry) return TSUCHI_NO_MEMORY;
        if ((record.parts & kLocation) && (!(entry->has & kLocation) || time > entry->location_time)) {
            memcpy(entry->location, record.location, sizeof entry->location);
            entry->location_time = time;
            entry->has |= kLocation;
        }
        if ((record.parts & kRotation) && (!(entry->has & kRotation) || time > entry->rotation_time)) {
            memcpy(entry->rotation, record.rotation, sizeof entry->rotation);
            entry->rotation_time = time;
            entry->has |= kRotation;
        }
        if ((record.parts & kVelocity) && (!(entry->has & kVelocity) || time > entry->velocity_time)) {
            memcpy(entry->velocity, record.velocity, sizeof entry->velocity);
            entry->velocity_time = time;
            entry->has |= kVelocity;
        }
        ++taken;
    }
    *out = taken;
    return TSUCHI_OK;
}

// The record layout feed_tracks reads, without its key: a byte of parts, then all three parts, zeros where missing.
TSUCHI_Result track_read_poses(const void* actors, uint64_t size, void* buffer, uint64_t capacity, uint64_t* out) {
    *out = 0;
    if (size % 8 || size / 8 > kPosesMost) return TSUCHI_INVALID;
    const uint64_t count = size / 8;
    if (capacity < count * kPoseRecordBytes) return TSUCHI_INVALID;
    const auto* handles = static_cast<const uint8_t*>(actors);
    auto* at = static_cast<uint8_t*>(buffer);
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t handle = 0;
        memcpy(&handle, handles + i * 8, 8);
        uint8_t* record = at + i * kPoseRecordBytes;
        memset(record, 0, kPoseRecordBytes);
        TSUCHI_Result why = TSUCHI_OK;
        const uint64_t actor = actor_of(TSUCHI_Actor{handle}, why);
        game::Transform transform{};
        if (!actor || !game::get_transform(actor, transform) || !finite_all(transform.location, 3)) continue;
        uint8_t parts = kLocation | kRotation;
        memcpy(record + 1, transform.location, 12);
        for (int k = 0; k < 3; ++k) {
            const float h = wrap(transform.angles[k]) * 100;
            const int16_t hundredths = int16_t(h < 0 ? h - 0.5f : h + 0.5f);
            memcpy(record + 13 + k * 2, &hundredths, 2);
        }
        float velocity[3];
        if (game::get_velocity(actor, velocity) && finite_all(velocity, 3)) {
            parts |= kVelocity;
            memcpy(record + 19, velocity, 12);
        }
        record[0] = parts;
    }
    *out = count * kPoseRecordBytes;
    return TSUCHI_OK;
}

TSUCHI_Result track_show(TSUCHI_Mod* mod, float now, uint32_t* out) {
    const Store* store = store_of(mod, false);
    uint32_t moved = 0;
    for (uint32_t i = 0; i < tracked.count(); ++i) {
        const Hold& hold = tracked.hold_at(i);
        if (hold.mod != mod) continue;
        const uint64_t actor = game::from_handle(hold.object);
        Kept& kept = tracked.kept_at(i);
        const Entry* entry = find(store, kept.key);
        if (!actor || !entry || !(entry->has & kLocation)) continue;
        float location[3], rotation[3] = {0, 0, 0};
        track_ahead(entry->location, entry->velocity, entry->location_time, now, location);
        if (entry->has & kRotation) memcpy(rotation, entry->rotation, sizeof rotation);
        else if (kept.shown) memcpy(rotation, kept.shown_rotation, sizeof rotation);
        if (kept.ease) {
            game::Transform at{};
            if (game::get_transform(actor, at)) {
                const float f = 1.0f / kept.ease;
                for (int k = 0; k < 3; ++k) location[k] = at.location[k] + (location[k] - at.location[k]) * f;
            }
            --kept.ease;
        }
        // Its body is given the velocity of its motion along the track since the last showing, across the ground: a
        // teleport leaves a body's velocity as it was, and a held actor whose logic and physics run walked on the
        // spot after every stop with the speed it had, and started late. The place is put again each step, so a
        // body that moves by this velocity in the world's update is where the next showing puts it, not past it.
        float velocity[3] = {0, 0, 0};
        const float since = now - kept.last_time;
        if (kept.timed && since > 0 && since <= kLongestShowing) {
            for (int k = 0; k < 2; ++k) velocity[k] = (location[k] - kept.last_location[k]) / since;
        } else if (!kept.timed && (entry->has & kVelocity)) {
            for (int k = 0; k < 2; ++k) velocity[k] = entry->velocity[k];
        }
        if (finite_all(velocity, 3) && (!kept.timed || since != 0)) game::request_ground_velocity(actor, velocity);
        if (!kept.timed || since != 0) {
            memcpy(kept.last_location, location, sizeof location);
            kept.last_time = now, kept.timed = 1;
        }
        bool stale = !kept.shown || distance2(location, kept.shown_location) > kMoved * kMoved ||
            magnitude(wrap(rotation[1] - kept.shown_rotation[1])) > kTurned;
        if (!stale) {
            game::Transform at{};
            if (game::get_transform(actor, at) && distance2(at.location, kept.placed) > kDrift * kDrift) stale = true;
        }
        if (!stale || !place(actor, location, rotation)) continue;
        memcpy(kept.shown_location, location, sizeof location);
        memcpy(kept.shown_rotation, rotation, sizeof rotation);
        memcpy(kept.placed, location, sizeof location);
        kept.shown = 1;
        ++moved;
    }
    *out = moved;
    return TSUCHI_OK;
}

TSUCHI_Result track_read(TSUCHI_Mod* mod, uint32_t key, TSUCHI_Track* out) {
    const Entry* entry = find(store_of(mod, false), key);
    if (!entry || !(entry->has & kLocation)) return TSUCHI_NOT_FOUND;
    out->location = {entry->location[0], entry->location[1], entry->location[2]};
    out->rotation = {entry->rotation[0], entry->rotation[1], entry->rotation[2]};
    out->velocity = {entry->velocity[0], entry->velocity[1], entry->velocity[2]};
    out->location_time = entry->location_time;
    out->rotation_time = entry->rotation_time;
    return TSUCHI_OK;
}

TSUCHI_Result track_forget(TSUCHI_Mod* mod, uint32_t key) {
    Store* store = store_of(mod, false);
    if (!store) return TSUCHI_NOT_FOUND;
    const uint32_t at = lower_bound(*store, key);
    if (at >= store->count || store->entries[at].key != key) return TSUCHI_NOT_FOUND;
    memmove(store->entries + at, store->entries + at + 1, sizeof(Entry) * (store->count - at - 1));
    --store->count;
    return TSUCHI_OK;
}

} // namespace tsuchi
