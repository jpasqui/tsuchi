// What moves in the world, between steps. The renderer is given each part's place when UpdatePrimitiveTransform
// publishes it, and each skinned part's pose when its mesh object builds the bones; both are taken as the step's
// own, and what is published is the place or pose at the picture's place between the last two steps. A part still
// between steps that the game does not send this frame is sent again before the drawing, through the game's own
// calls. Nothing of the game's own state is changed: the next send replaces what was shown. (On a build whose mesh
// object has no bone kernel, the pose array lends its data to the blend for the one call; see lent_pose.)
//
// The scene's sends and the bone builds run on the game's thread and on workers, so what is kept is behind one
// short lock.
#include "presenter_scene.h"

#include <string.h>

#include "game.h"
#include "hooks.h"
#include "native_routes.h"
#include "objects.h"
#include "presenter_logic.h"
#include "presenter_markers.h"
#include "registry.h"

namespace tsuchi {
namespace {

namespace t = game::table;
using namespace presenter;

constexpr uint32_t kGap = 4;           // steps apart that two places or poses are still shown between
constexpr uint64_t kDrawnWithin = 3;   // frames since the renderer last drew a part, for it to be sent between steps
constexpr float kSmallPose = 0.1f;     // a pose this small on the screen (its bounds' radius over the view's half width) waits for its step
constexpr uint32_t kMostBones = 1024;  // a pose with more is left as it is
constexpr float kSmallRoot = 0;        // a part this small on the screen is not sent between steps (0: every part is)
constexpr float kLeastPixels = 0;      // nor one that would move less than this many pixels from where it was sent (0: off)
constexpr float kHalfWidth = 960;      // pixels across half the picture

PresentedMoment moment{};
// The first player's view of the last picture: where it is, and the tangent of half its field of view; 0 while
// none. The parts are sent before the drawing asks for this picture's view.
float view_at[3] = {}, view_tangent = 0;
uint64_t view_frame = 0;

struct Lock {
    int held = 0;
    void take() {
        uint32_t tries = 0;
        while (__atomic_exchange_n(&held, 1, __ATOMIC_ACQUIRE))
            while (__atomic_load_n(&held, __ATOMIC_RELAXED)) lock_wait(tries);
    }
    void give() { __atomic_store_n(&held, 0, __ATOMIC_RELEASE); }
};
Lock lock;
struct Held {
    Held() { lock.take(); }
    ~Held() { lock.give(); }
};

SceneCounts counts{};
void count(uint32_t& counter) { __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED); }

// What each thread is doing inside the game's calls: the part whose place or pose is being sent, and a buffer
// for the pose it is shown with.
struct ThreadPart {
    uint64_t thread, place_part, pose_part;
    int32_t lod;
    BonePose* scratch;
    uint32_t room;
};
constexpr unsigned kThreads = 32;
ThreadPart threads[kThreads] = {};
ThreadPart* this_thread() {
    const uint64_t me = current_thread_token();
    for (ThreadPart& slot : threads)
        if (__atomic_load_n(&slot.thread, __ATOMIC_ACQUIRE) == me) return &slot;
    for (ThreadPart& slot : threads) {
        uint64_t none = 0;
        if (__atomic_compare_exchange_n(&slot.thread, &none, me, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return &slot;
    }
    return nullptr;
}

// The place of a part as the renderer is given it.
struct Place {
    float matrix[16], bounds[7], attachment[3];
    Trs trs;
};
// A part whose place is kept: the last two steps it moved at.
struct Root {
    uint64_t part, weak, proxy, scene, from, to, sent_frame, drawn_frame;
    uint32_t drawn;
    float move, sent, turn, size; // turn, size: a step's change of each across the last two steps
    bool used, pair, cut, measured; // measured: turn and size were
    Place a, b;
};
constexpr unsigned kRoots = 512;
Root roots[kRoots] = {};

// A skinned part's pose: the last two qualified native updates (or byte changes on the fallback path).
struct Pose {
    uint64_t part, weak, from, to, sent_frame, drawn_frame;
    uint64_t owner_weak;
    uint32_t revision;
    uint32_t drawn;
    int32_t lod;
    uint32_t count, room;
    float sent;
    bool used, pair, cut, native_known, endpoint_due, before_known; // before_known: `a` holds the step before `to`
    uint64_t submission;
    BonePose *a, *b; // two halves of one block of 2 * room bones
    BoneMotion* motion; // room bones' changes across their last two steps
};
constexpr unsigned kPoses = 256;
Pose poses[kPoses] = {};
bool smooth_poses = false;
uint64_t pose_epoch = 1;

void reuse(Root&) {}
void reuse(Pose& pose) {
    pose.native_known = pose.endpoint_due = false;
    ++pose.submission;
}

uint64_t hash(uint64_t part) { return (part >> 4) * 0x9E3779B97F4A7C15ull; }

template <class T, unsigned N>
T* find(T (&table)[N], uint64_t part, bool add) {
    T* empty = nullptr;
    for (unsigned probe = 0; probe < N; ++probe) {
        T& entry = table[(hash(part) + probe) % N];
        if (entry.used && entry.part == part) return &entry;
        if (!entry.used && !empty) empty = &entry;
    }
    if (!add || !empty) return nullptr;
    empty->used = true, empty->part = part, empty->weak = game::to_weak_value(part), empty->pair = empty->cut = false;
    empty->from = empty->to = empty->sent_frame = empty->drawn_frame = 0, empty->sent = 1, empty->drawn = 0;
    reuse(*empty);
    return empty;
}

// Whether the renderer drew the part in the last few frames. It notes the view's time in the part each time it
// draws it, in a view or a shadow, so a change since the last look is a drawing. A part not drawn is not sent
// between steps: nobody sees it, and sending it costs the game's thread and the renderer.
template <class T>
bool drawn_lately(T& entry) {
    const uint32_t now = game::read_or<uint32_t>(entry.part + t::primitive::last_drawn);
    if (now != entry.drawn) entry.drawn = now, entry.drawn_frame = moment.frame;
    return entry.drawn_frame && moment.frame - entry.drawn_frame < kDrawnWithin;
}

// The part is still the object it was when it was first kept.
template <class T>
bool same_part(const T& entry) {
    return entry.weak && game::from_weak_value(entry.weak) == entry.part;
}

double at() { return double(moment.steps) - 1 + moment.fraction; }

// ---- Places -----------------------------------------------------------------------------------------

using SceneUpdate = decltype(t::slot::scene_update)::Function;
using SceneRemove = decltype(t::slot::scene_remove)::Function;
SceneUpdate next_update = nullptr;
SceneRemove next_remove = nullptr;

void scene_update(void* scene, void* part) {
    ThreadPart* slot = moment.active ? this_thread() : nullptr;
    const uint64_t outer = slot ? slot->place_part : 0;
    if (slot) slot->place_part = reinterpret_cast<uint64_t>(part);
    next_update(scene, part);
    if (slot) slot->place_part = outer;
}

void forget_part(uint64_t part) {
    if (Root* root = find(roots, part, false)) root->used = false;
    if (Pose* pose = find(poses, part, false)) pose->used = false; // its buffers are kept for the next
}

void scene_remove(void* scene, void* part) {
    if (part) {
        Held held;
        forget_part(reinterpret_cast<uint64_t>(part));
    }
    next_remove(scene, part);
}

// The task UpdatePrimitiveTransform has filled in, just before it publishes it: x20.
void on_publish(NativeFrame& frame) {
    if (!moment.active) return;
    ThreadPart* slot = this_thread();
    const uint64_t task = frame.x[t::route_frame::scene_publish::task], part = slot ? slot->place_part : 0;
    if (!part) return;
    namespace k = t::transform_task;
    const uint64_t scene = game::read_or<uint64_t>(task + k::scene), proxy = game::read_or<uint64_t>(task + k::proxy);
    if (!scene || !proxy || game::read_or<uint64_t>(part + t::primitive::proxy) != proxy ||
        game::read_or<uint64_t>(scene + t::scene_world::world) != game::global<uint64_t>(t::data::world))
        return;
    Place now;
    if (!game::read(task + k::matrix, now.matrix) || !game::read(task + k::world_bounds, now.bounds) ||
        !game::read(task + k::attachment, now.attachment) || !to_trs(now.matrix, now.trs))
        return;
    Held held;
    Root* root = find(roots, part, true);
    if (!root) return;
    if (root->proxy != proxy || root->scene != scene) root->proxy = proxy, root->scene = scene, root->pair = false, root->to = 0;
    const uint64_t step = moment.steps;
    const bool same = root->to && !memcmp(&root->b, &now, offsetof(Place, trs));
    if (!root->to) {
        root->b = now, root->to = step, root->move = root->turn = root->size = 0, root->measured = false;
    } else if (!same) {
        if (step == root->to) {
            root->b = now; // moved again in the same step: the newest is the step's
        } else {
            float move = 0;
            const bool jump = jumped(root->b.trs.t, now.trs.t, root->move, move);
            if (jump) count(counts.jumps);
            // A turn or a size that snapped is shown at once too (presenter_logic.h, "Snaps").
            const Change change = change_between(root->b.trs.q, root->b.trs.t, root->b.trs.s, now.trs.q, now.trs.t, now.trs.s, step - root->to);
            const bool snap = root->measured && (beyond(change.turn, root->turn, kSnapTurn) || beyond(change.size, root->size, kSnapSize));
            if (snap && !jump) count(counts.snaps);
            root->turn = change.turn, root->size = change.size, root->measured = true;
            root->pair = !root->cut && !jump && !snap && step - root->to <= kGap && (root->b.trs.s[0] < 0) == (now.trs.s[0] < 0);
            root->a = root->b, root->from = root->to, root->b = now, root->to = step, root->move = root->pair ? move : 0;
        }
        root->cut = false;
    }
    // A part's place is kept with roots left out too: a pose small on the screen is told by it.
    const float f = root->pair && (moment.parts & kPartRoots) ? place_between(root->from, root->to, at()) : 1;
    if (f < 1) {
        Place shown;
        blend(root->a.trs, root->b.trs, f, shown.trs);
        to_matrix(shown.trs, shown.matrix);
        hold_both(root->a.bounds, root->b.bounds, shown.bounds);
        for (unsigned i = 0; i < 3; ++i) shown.attachment[i] = root->a.attachment[i] + (root->b.attachment[i] - root->a.attachment[i]) * f;
        game::write(task + k::matrix, shown.matrix);
        game::write(task + k::world_bounds, shown.bounds);
        game::write(task + k::attachment, shown.attachment);
        count(counts.root_blends);
    }
    root->sent = f, root->sent_frame = moment.frame;
}

// ---- Poses ---------------------------------------------------------------------------------------

using MeshUpdate = decltype(t::slot::mesh_update)::Function;
using BoneKernel = decltype(t::code_site::bone_kernel)::Function;
MeshUpdate next_mesh = nullptr;
BoneKernel next_kernel = nullptr;

bool kernel_present() { return t::present(t::code_sites[t::code_site::bone_kernel.number].address); }
bool lent_pose(ThreadPart* slot, uint64_t part, void* mesh, int32_t lod, void* morphs, void* weights, uint32_t previous);

void mesh_update(void* mesh, int32_t lod, void* part, void* morphs, void* weights, uint32_t previous) {
    ThreadPart* slot = moment.active ? this_thread() : nullptr;
    const uint64_t outer = slot ? slot->pose_part : 0;
    const int32_t outer_lod = slot ? slot->lod : 0;
    if (slot) slot->pose_part = reinterpret_cast<uint64_t>(part), slot->lod = lod;
    if (!lent_pose(slot, reinterpret_cast<uint64_t>(part), mesh, lod, morphs, weights, previous))
        next_mesh(mesh, lod, part, morphs, weights, previous);
    if (slot) slot->pose_part = outer, slot->lod = outer_lod;
}

bool make_room(BonePose*& buffer, uint32_t& room, uint32_t count) {
    if (count <= room) return true;
    auto* grown = static_cast<BonePose*>(allocate(sizeof(BonePose) * count));
    if (!grown) return false;
    release(buffer);
    buffer = grown, room = count;
    return true;
}
// A pose's two halves, with room for `count` bones each. What they held is lost when they grow.
bool make_halves(Pose& pose, uint32_t count) {
    if (count <= pose.room) return true;
    auto* motion = static_cast<BoneMotion*>(allocate(sizeof(BoneMotion) * count));
    if (!motion) return false;
    BonePose* low = pose.a < pose.b ? pose.a : pose.b;
    uint32_t room = pose.room * 2;
    if (!make_room(low, room, count * 2)) return release(motion), false;
    release(pose.motion);
    pose.a = low, pose.b = low + count, pose.motion = motion, pose.room = count, pose.to = 0, pose.pair = false;
    return true;
}

enum class PoseCadence { interpolated, reduced, small };

struct PoseArray {
    uint64_t data;
    int32_t count, room;
};
struct NativePose {
    uint64_t part_weak = 0, owner_weak = 0, master = 0;
    uint32_t revision = 0;
    int32_t read_index = 0;
    PoseArray array{};
};

// Pose arrays lent now. An array is its owner's, and the owner may be the master pose of several parts whose
// updates run at once on workers: each array is lent once, to a copy of the first blend, which every update that
// reads it meanwhile shares; the last of them gives the array its own data back.
struct Lend {
    uint64_t array, own;
    BonePose* blend;
    uint32_t users;
};
constexpr unsigned kLends = 64;
Lend lends[kLends] = {};

// The data an array holds of its own: while it is lent, what it held before.
uint64_t own_data(uint64_t array) {
    Held held;
    for (const Lend& entry : lends)
        if (entry.users && entry.array == array) return entry.own;
    return game::read_or<uint64_t>(array);
}

// The original mesh update reads the component/master's finalized revision. Its matrix helper may reject
// a master with an incompatible bone map, so accept only when this exact supplied array belongs to that
// same owner. No inferred update from equal bytes, a dirty flag, or our own deferred request is needed.
bool native_pose(uint64_t part, uint64_t given, NativePose& out) {
    out.part_weak = game::to_weak_value(part);
    if (!out.part_weak || game::from_weak_value(out.part_weak) != part ||
        !game::read(part + t::skinned_pose::master, out.master)) return false;
    const uint64_t master = game::from_weak_value(out.master);
    const uint64_t owner = master ? master : part;
    out.owner_weak = master ? out.master : out.part_weak;
    if (!game::read(owner + t::skinned_pose::read_index, out.read_index) || out.read_index < 0 || out.read_index > 1 ||
        given != owner + t::skinned_pose::arrays + uint64_t(out.read_index) * sizeof(PoseArray) ||
        !game::read(given, out.array) || !(out.array.data = own_data(given)) || out.array.count <= 0 || out.array.room < out.array.count ||
        !game::read(owner + t::skinned_pose::revision, out.revision)) return false;
    return game::from_weak_value(out.part_weak) == part && game::from_weak_value(out.owner_weak) == owner;
}

bool same_native(const NativePose& a, const NativePose& b) {
    return a.part_weak == b.part_weak && a.owner_weak == b.owner_weak && a.master == b.master &&
           a.revision == b.revision && a.read_index == b.read_index && a.array.data == b.array.data &&
           a.array.count == b.array.count && a.array.room == b.array.room;
}

// The game's update and the intermediate sends must agree: an interpolated pose must be advanced by later
// pictures. A distant pose that is not sent between updates is published at its newest native value instead.
PoseCadence pose_cadence(const Pose& pose) {
    if (smooth_poses && pose.native_known && pose.to > pose.from && pose.to - pose.from <= kGap)
        return PoseCadence::interpolated;
    if (pose.to - pose.from > 1) return PoseCadence::reduced;
    if (const Root* root = view_tangent > 0 ? find(roots, pose.part, false) : nullptr) {
        const float* bounds = root->b.bounds;
        const float d[3] = {bounds[0] - view_at[0], bounds[1] - view_at[1], bounds[2] - view_at[2]};
        const float distance = math::square_root(dot3(d, d));
        if (root->to && distance > 1 && bounds[6] < kSmallPose * distance * view_tangent) return PoseCadence::small;
    }
    return PoseCadence::interpolated;
}

// What a pose's bones are built from at a picture: the component's own array (data, count, room), or between steps
// a blend of its last two in the thread's scratch.
struct PoseChoice {
    float f = 1;
    uint64_t epoch = 0, submission = 0, weak = 0;
    bool qualified = false;
    NativePose before;
};
struct ShownArray {
    uint64_t data;
    int32_t count, room;
};

// False for an array this leaves as it is.
bool choose_pose(ThreadPart* slot, uint64_t part, uint64_t array, PoseChoice& c, ShownArray& shown) {
    const uint64_t data = part ? own_data(array) : 0;
    const uint32_t bones = data ? game::read_or<uint32_t>(array + 8) : 0;
    if (!bones || bones > kMostBones || !game::readable(data, uint64_t(bones) * sizeof(BonePose))) return false;
    const BonePose* now = reinterpret_cast<const BonePose*>(data);
    NativePose& before = c.before;
    c.qualified = native_pose(part, array, before) && before.array.data == data && uint32_t(before.array.count) == bones;
    const bool qualified = c.qualified;
    float f = 1;
    {
        Held held;
        c.epoch = pose_epoch;
        Pose* pose = find(poses, part, true);
        if (pose) {
            c.submission = ++pose->submission;
            if (pose->count != bones || pose->lod != slot->lod ||
                (qualified && (!pose->native_known || pose->weak != before.part_weak || pose->owner_weak != before.owner_weak)))
                pose->to = 0, pose->pair = false;
            if (make_halves(*pose, bones)) {
                const uint64_t step = moment.steps;
                const bool fresh = qualified && (!pose->native_known || pose->owner_weak != before.owner_weak || pose->revision != before.revision);
                const bool changed = pose->to && memcmp(pose->b, now, sizeof(BonePose) * bones);
                const bool advancing = pose->to && step != pose->to;
                if (!pose->to) {
                    memcpy(pose->b, now, sizeof(BonePose) * bones), pose->to = step, pose->count = bones, pose->lod = slot->lod;
                    memset(pose->motion, 0, sizeof(BoneMotion) * bones), pose->before_known = false; // no bone seen moving yet
                } else if (fresh || (!qualified && changed)) {
                    const bool advanced = step != pose->to;
                    if (advanced) {
                        pose->pair = !pose->cut && step - pose->to <= kGap;
                        BonePose* swap = pose->a;
                        pose->a = pose->b, pose->b = swap, pose->from = pose->to, pose->to = step, pose->before_known = true;
                    }
                    memcpy(pose->b, now, sizeof(BonePose) * bones);
                    // Each bone's change across this step against the step before's: a bone that snapped is shown
                    // at this step at once (presenter_logic.h, "Snaps").
                    if (pose->before_known && pose->to > pose->from &&
                        mark_snaps(pose->a, pose->b, bones, pose->to - pose->from, advanced, pose->motion) && advanced)
                        count(counts.snaps);
                    pose->cut = false;
                } else if (qualified && changed) {
                    // Unfinalized edits under an unchanged revision cannot establish an evaluation cadence.
                    // Show their native endpoint and wait for a fresh finalized revision to restart history.
                    memcpy(pose->b, now, sizeof(BonePose) * bones);
                    pose->pair = false, pose->cut = true;
                }
                pose->native_known = qualified;
                if (qualified) pose->weak = before.part_weak, pose->owner_weak = before.owner_weak, pose->revision = before.revision;
                // Keep cadence history across a held pose without scheduling redundant intermediate kernels.
                if (fresh && advancing && !changed) pose->pair = false;
                if (pose_cadence(*pose) != PoseCadence::interpolated) pose->pair = false;
                f = pose->pair && !pose->endpoint_due ? place_between(pose->from, pose->to, at()) : 1;
                if (f < 1 && make_room(slot->scratch, slot->room, bones)) blend_bones(pose->a, pose->b, bones, f, slot->scratch, pose->motion);
                else f = 1;
                pose->sent = f, pose->sent_frame = moment.frame;
            }
            c.weak = pose->weak;
        }
    }
    c.f = f;
    shown = {reinterpret_cast<uint64_t>(slot->scratch), int32_t(bones), int32_t(bones)};
    if (f < 1) count(counts.bone_blends);
    return true;
}

// After the bones were built from the choice: whether the game's own pose or the policy changed meanwhile, so that
// a blend must not stay visible and the bones are built again from the game's array.
bool pose_redo(uint64_t part, uint64_t array, const PoseChoice& c, bool& native_valid) {
    NativePose after;
    native_valid = !c.qualified || (native_pose(part, array, after) && same_native(c.before, after));
    bool policy_changed = false;
    {
        Held held;
        policy_changed = c.epoch != pose_epoch;
    }
    return c.f < 1 && (!native_valid || policy_changed);
}

void pose_done(uint64_t part, const PoseChoice& c, bool native_valid, bool redo) {
    Held held;
    Pose* pose = find(poses, part, false);
    // Reentry can already have established a newer pose or policy history.
    if (pose && pose->submission == c.submission && pose->weak == c.weak && same_part(*pose)) {
        if (!native_valid) pose->to = 0, pose->pair = false, pose->native_known = false;
        if (native_valid && (c.f >= 1 || redo)) {
            pose->endpoint_due = false;
            pose->sent = 1;
        }
    }
}

// The pose the bones are built from is given to the kernel as an array; between steps the kernel is given another
// array with the pose at the picture's place.
void bone_kernel(void* out, const void* given, void* visible, void* mapping, void* inverse, void* extra, void* lods, int32_t lod,
                 uint64_t required) {
    ThreadPart* slot = moment.active && (moment.parts & kPartBones) ? this_thread() : nullptr;
    const uint64_t part = slot ? slot->pose_part : 0;
    const uint64_t array = reinterpret_cast<uint64_t>(given);
    PoseChoice choice;
    ShownArray shown{};
    if (!choose_pose(slot, part, array, choice, shown))
        return next_kernel(out, given, visible, mapping, inverse, extra, lods, lod, required);
    next_kernel(out, choice.f < 1 ? &shown : given, visible, mapping, inverse, extra, lods, lod, required);
    bool native_valid = true;
    // A policy or native change while the worker builds bones cannot leave an
    // old-policy blend visible. The retry always uses the game's input array.
    const bool redo = pose_redo(part, array, choice, native_valid);
    if (redo) {
        next_kernel(out, given, visible, mapping, inverse, extra, lods, lod, required);
        NativePose after;
        if (choice.qualified) native_valid = native_pose(part, array, after) && same_native(choice.before, after);
    }
    pose_done(part, choice, native_valid, redo);
}

bool lend(uint64_t array, const ShownArray& shown) {
    Held held;
    for (Lend& entry : lends)
        if (entry.users && entry.array == array) return ++entry.users, true;
    for (Lend& entry : lends) {
        if (entry.users) continue;
        const uint64_t bytes = uint64_t(shown.count) * sizeof(BonePose);
        auto* blend = static_cast<BonePose*>(allocate(bytes));
        if (!blend) return false;
        memcpy(blend, reinterpret_cast<const void*>(shown.data), bytes);
        entry = {array, game::read_or<uint64_t>(array), blend, 1};
        game::write(array, reinterpret_cast<uint64_t>(blend));
        return true;
    }
    return false;
}
// A blend given back may still be read by an update the hooks do not see, which took the array while it was lent:
// it is freed two frames later, when every update of its frame is over.
struct Retired {
    BonePose* blend;
    uint64_t frame;
};
constexpr unsigned kRetired = 1024;
constexpr uint64_t kRetiredFrames = 2;
Retired retired[kRetired] = {};

void give_back(uint64_t array) {
    BonePose* done[kRetired];
    unsigned freeing = 0;
    {
        Held held;
        BonePose* given = nullptr;
        for (Lend& entry : lends) {
            if (!entry.users || entry.array != array || --entry.users) continue;
            if (game::read_or<uint64_t>(array) == reinterpret_cast<uint64_t>(entry.blend)) game::write(array, entry.own);
            given = entry.blend;
            entry = {};
        }
        Retired* oldest = nullptr;
        for (Retired& entry : retired) {
            if (entry.blend && entry.frame + kRetiredFrames < moment.frame) done[freeing++] = entry.blend, entry = {};
            if (!entry.blend) oldest = &entry;
            else if (oldest && oldest->blend && entry.frame < oldest->frame) oldest = &entry;
            else if (!oldest) oldest = &entry;
        }
        if (given) {
            if (oldest->blend) done[freeing++] = oldest->blend; // more given back within two frames than there is room for
            *oldest = {given, moment.frame};
        }
    }
    for (unsigned i = 0; i < freeing; ++i) release(done[i]);
}

// A build whose mesh object builds the bones inline, with no kernel of its own to give another array to: for the
// update, the component's array lends its data to the blend, and gets its own back right after. False when the
// update was not made here.
bool lent_pose(ThreadPart* slot, uint64_t part, void* mesh, int32_t lod, void* morphs, void* weights, uint32_t previous) {
    if (kernel_present() || !slot || !part || !(moment.parts & kPartBones)) return false;
    uint64_t master = 0, owner = part;
    int32_t index = -1;
    if (!game::read(part + t::skinned_pose::master, master)) return false;
    if (const uint64_t kept = game::from_weak_value(master)) owner = kept;
    if (!game::read(owner + t::skinned_pose::read_index, index) || index < 0 || index > 1) return false;
    const uint64_t array = owner + t::skinned_pose::arrays + uint64_t(index) * sizeof(PoseArray);
    PoseChoice choice;
    ShownArray shown{};
    if (!choose_pose(slot, part, array, choice, shown)) return false;
    const bool lent = choice.f < 1 && lend(array, shown);
    if (choice.f < 1 && !lent) choice.f = 1; // nowhere to lend it: the game's own pose, as an endpoint
    next_mesh(mesh, lod, reinterpret_cast<void*>(part), morphs, weights, previous);
    if (lent) give_back(array);
    bool native_valid = true;
    const bool redo = pose_redo(part, array, choice, native_valid);
    if (redo) {
        next_mesh(mesh, lod, reinterpret_cast<void*>(part), morphs, weights, previous);
        NativePose after;
        if (choice.qualified) native_valid = native_pose(part, array, after) && same_native(choice.before, after);
    }
    pose_done(part, choice, native_valid, redo);
    return true;
}

// ---- Jumps -------------------------------------------------------------------------------------------

using ActorWarp = decltype(t::code_site::actor_warp)::Function;
ActorWarp next_warp = nullptr;

void actor_warp(void* actor, void* place, void* angles, bool turn, bool ground, bool height) {
    next_warp(actor, place, angles, turn, ground, height);
    presenter_cut(reinterpret_cast<uint64_t>(actor));
}

} // namespace

void presenter_cut(uint64_t actor) {
    if (!actor || !moment.active) return;
    count(counts.cuts);
    presenter_cut_camera(actor);
    markers_cut(actor);
    uint64_t waiting[256];
    unsigned left = 0, seen = 0;
    const uint64_t root = game::read_or<uint64_t>(actor + t::engine_actor::root);
    if (root) waiting[left++] = root;
    Held held;
    while (left && seen < 1024) {
        const uint64_t part = waiting[--left];
        ++seen;
        if (Root* entry = find(roots, part, false)) entry->pair = false, entry->cut = true;
        if (Pose* entry = find(poses, part, false)) entry->pair = false, entry->cut = true;
        const uint64_t children = game::read_or<uint64_t>(part + t::scene_component::attach_children);
        const int32_t number = game::read_or<int32_t>(part + t::scene_component::attach_children + t::array::count);
        for (int32_t i = 0; children && i < number && left < 256; ++i)
            if (const uint64_t child = game::read_or<uint64_t>(children + 8 * uint64_t(i))) waiting[left++] = child;
    }
}

bool scene_start(TSUCHI_Mod* self) {
    if (!self) return false;
    uint64_t id = 0;
    if (!next_update) hook_slot(self, t::slot::scene_update, &scene_update, &next_update, 0, &id);
    if (!next_remove) hook_slot(self, t::slot::scene_remove, &scene_remove, &next_remove, 0, &id);
    if (!next_mesh) hook_slot(self, t::slot::mesh_update, &mesh_update, &next_mesh, 0, &id);
    // Without a kernel of its own the mesh update lends its array the blend (lent_pose).
    const bool kernel = kernel_present();
    if (kernel && !next_kernel) hook_site(self, t::code_site::bone_kernel, &bone_kernel, &next_kernel, &id);
    if (!next_warp) hook_site(self, t::code_site::actor_warp, &actor_warp, &next_warp, &id);
    // Without the scene's own call to send a part's place, the publish has no part to go by.
    static bool publish_ready = false;
    if (next_update && !publish_ready) publish_ready = native_route(t::native_route::scene_publish, &on_publish);
    return next_update && next_remove && next_mesh && (next_kernel || !kernel) && next_warp && publish_ready;
}

void scene_pose_policy(bool smooth) {
    Held held;
    if (smooth == smooth_poses) return;
    smooth_poses = smooth;
    ++pose_epoch;
    for (Pose& pose : poses) {
        if (!pose.used) continue;
        pose.endpoint_due = pose.endpoint_due || pose.sent < 1;
        pose.from = pose.to = 0;
        pose.pair = pose.cut = pose.native_known = false;
    }
}

void scene_moment(const PresentedMoment& now) {
    moment = now;
}

void scene_view(const float location[3], float field_of_view) {
    if (view_frame == moment.frame || !(field_of_view > 1 && field_of_view < 170)) return; // the first view of the picture
    view_frame = moment.frame;
    memcpy(view_at, location, sizeof view_at);
    const float half = field_of_view * 0.5f * math::kPi / 180;
    view_tangent = math::sine(half) / math::cosine(half);
}

constexpr unsigned kDue = 256;

// Each is marked to be sent, as the game marks a part it moves, and the end of the frame sends it in the
// drawing, on the game's thread or its workers. A part already marked is left as it is.
void send(const uint64_t* send_places, unsigned places, const uint64_t* send_poses, unsigned poses_due) {
    for (unsigned i = 0; i < places; ++i) {
        const uint64_t part = send_places[i];
        const uint32_t flags = game::read_or<uint32_t>(part + t::primitive::flags);
        if ((flags & t::constant::render_ready) != t::constant::render_ready || (flags & t::constant::render_transform_dirty) ||
            !game::read_or<uint64_t>(part + t::primitive::proxy) || !t::bound(t::fn::mark_end_of_frame))
            continue;
        game::write<uint32_t>(part + t::primitive::flags, flags | uint32_t(t::constant::render_transform_dirty));
        t::fn::mark_end_of_frame(reinterpret_cast<void*>(part));
        count(counts.root_sends);
    }
    for (unsigned i = 0; i < poses_due; ++i) {
        const uint64_t part = send_poses[i];
        const uint32_t flags = game::read_or<uint32_t>(part + t::primitive::flags);
        if ((flags & t::constant::render_dynamic_dirty) || !t::bound(t::fn::mark_pose_dirty)) continue;
        t::fn::mark_pose_dirty(reinterpret_cast<void*>(part));
        count(counts.bone_sends);
    }
}

void scene_present() {
    if (!moment.active) return;
    const double place = at();
    uint64_t send_places[kDue], send_poses[kDue];
    unsigned places = 0, poses_due = 0;
    {
        Held held;
        for (Root& root : roots) {
            if (!root.used || !(moment.parts & kPartRoots)) continue;
            const float f = root.pair ? place_between(root.from, root.to, place) : 1;
            if ((f >= 1 && root.sent >= 1) || root.sent_frame == moment.frame) {
                if (root.to + 300 < moment.steps) root.used = false; // still for ten seconds: forgotten
                continue;
            }
            if (!same_part(root)) {
                root.used = false;
                continue;
            }
            if (!drawn_lately(root)) {
                count(counts.unseen);
                continue;
            }
            if ((kSmallRoot > 0 || kLeastPixels > 0) && view_tangent > 0 && root.pair) {
                const float* bounds = root.b.bounds;
                const float d[3] = {bounds[0] - view_at[0], bounds[1] - view_at[1], bounds[2] - view_at[2]};
                const float across = math::square_root(dot3(d, d)) * view_tangent; // half the view's width there
                if (across > 1 && kSmallRoot > 0 && bounds[6] < kSmallRoot * across) {
                    count(counts.small_roots);
                    continue;
                }
                if (across > 1 && kLeastPixels > 0) {
                    // How far it would move on the screen from the place last sent: its own move, and its turn
                    // at its edge.
                    const float* a = root.a.trs.t;
                    const float* b = root.b.trs.t;
                    const float step[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                    const float cosine = dot3(root.a.trs.q, root.b.trs.q) + root.a.trs.q[3] * root.b.trs.q[3];
                    const float turn = 1 - (cosine < 0 ? -cosine : cosine);
                    const float share = f > root.sent ? f - root.sent : root.sent - f;
                    const float moved = (math::square_root(dot3(step, step)) + 2 * math::square_root(2 * turn) * bounds[6]) * share;
                    if (moved / across * kHalfWidth < kLeastPixels) {
                        count(counts.still_roots);
                        continue;
                    }
                }
            }
            if (places < kDue) send_places[places++] = root.part;
        }
        for (Pose& pose : poses) {
            if (!pose.used || !(moment.parts & kPartBones)) continue;
            const float f = pose.pair ? place_between(pose.from, pose.to, place) : 1;
            if ((!pose.endpoint_due && f >= 1 && pose.sent >= 1) || pose.sent_frame == moment.frame) {
                if (!pose.endpoint_due && pose.to + 300 < moment.steps) pose.used = false;
                continue;
            }
            if (!same_part(pose)) {
                pose.used = false;
                continue;
            }
            // A pose the game makes less often than every step is a far one's, small on the screen: it is shown
            // at its steps, as making it again between them costs as much as a near one's.
            const PoseCadence cadence = pose_cadence(pose);
            if (!pose.endpoint_due && cadence == PoseCadence::reduced) {
                count(counts.reduced);
                continue;
            }
            // So is one small on the screen, such as a Pikmin's a few meters away: its limbs are a few pixels.
            if (!pose.endpoint_due && cadence == PoseCadence::small) {
                count(counts.small);
                continue;
            }
            if (!drawn_lately(pose)) {
                count(counts.unseen);
                continue;
            }
            if (poses_due < kDue) send_poses[poses_due++] = pose.part;
        }
    }
    send(send_places, places, send_poses, poses_due);
}

void scene_forget() {
    // A part last shown between steps keeps that place until the game sends it again, which a part at rest
    // never does. Each one is sent again first, and the game's own place or pose is what it sends.
    uint64_t send_places[kDue], send_poses[kDue];
    unsigned places = 0, poses_due = 0;
    {
        Held held;
        view_tangent = 0;
        for (Root& root : roots) {
            if (root.used && root.sent < 1 && places < kDue && same_part(root)) send_places[places++] = root.part;
            root.used = false;
        }
        for (Pose& pose : poses) {
            if (pose.used && (pose.sent < 1 || pose.endpoint_due) && poses_due < kDue && same_part(pose)) send_poses[poses_due++] = pose.part;
            pose.used = false;
        }
    }
    send(send_places, places, send_poses, poses_due);
}

SceneCounts scene_counts(bool clear) {
    SceneCounts out = counts;
    {
        Held held;
        for (const Root& root : roots) out.roots += root.used;
        for (const Pose& pose : poses) out.bones += pose.used;
    }
    if (clear) counts = {};
    return out;
}

} // namespace tsuchi
