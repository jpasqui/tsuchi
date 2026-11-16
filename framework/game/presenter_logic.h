// The presenter's arithmetic, apart from the game: which frames step the world, where each picture is between
// the last two steps, and what the camera, the view's clocks, the widgets that count frames and the controllers
// are given. presenter.cpp connects it to the game; a test runs it alone.
#pragma once
#include <stdint.h>
#include <string.h>

#include "angles.h"

namespace tsuchi::presenter {

constexpr double kStep = 1.0 / 30.0;
constexpr float kStepSeconds = 1.0f / 30.0f; // what a step tells the world, the same float the fixed rate gives
constexpr double kQuantum = 1.0 / 120.0;     // a frame within a millisecond of a multiple of this is taken as that
constexpr double kRepay = 0.00025;           // and what that took off or added is given back this much a frame at most

inline bool finite(double value) { return value - value == 0; }

// What one frame does.
struct Frame {
    bool step = true;      // the world steps in this frame
    float fraction = 1;    // where its picture is between the last two steps
    double seconds = 0;    // the frame's length, as the clock took it
};

// The world's clock: frames come at any rate, steps at thirty a second. At most one step in a frame and at most one
// step owed, so the game slows down below thirty frames a second as it does without the presenter.
class Clock {
public:
    // Forgets where the pictures were: the next one is shown at the newest step. The steps go on being counted.
    void forget() { shown_ = false; }
    // `seconds` is the frame's length; `least` and `most` are the world's limits on it, or 0 and 0 for none.
    Frame advance(double seconds, double least = 0, double most = 0) {
        Frame out;
        if (!finite(seconds) || seconds < 0) seconds = kStep;
        // A steady rate counts as exactly that rate, so the pictures fall at the same places between steps. What
        // the rounding took is given back a little each frame, so the world keeps to the clock on the wall.
        const double quantized = double(int64_t(seconds / kQuantum + 0.5)) * kQuantum;
        if (quantized > 0 && seconds - quantized < 0.001 && quantized - seconds < 0.001) owed_ += seconds - quantized, seconds = quantized;
        if (owed_ > kStep || owed_ < -kStep) owed_ = 0;
        const double repaid = owed_ > kRepay ? kRepay : owed_ < -kRepay ? -kRepay : owed_;
        seconds += repaid, owed_ -= repaid;
        double admitted = seconds;
        if (most > 0 && least <= most) admitted = admitted < least ? least : admitted > most ? most : admitted;
        debt_ += admitted;
        out.seconds = seconds;
        out.step = debt_ + 1e-9 >= kStep;
        if (out.step) {
            debt_ -= kStep;
            ++steps_;
        }
        if (debt_ > kStep) debt_ = kStep;
        if (debt_ < 0) debt_ = 0;
        // Shown between the step before and the newest, as far as the time owed says; a frame of a whole step or
        // more, or a first picture, is shown at the newest step.
        double fraction = debt_ / kStep;
        if (!shown_ || seconds + 1e-9 >= kStep || debt_ + 1e-9 >= kStep) fraction = 1;
        if (fraction > 1) fraction = 1;
        // Pictures never go back in time.
        const double at = double(steps_) - 1 + fraction;
        if (shown_ && at < shown_at_) fraction = shown_at_ - (double(steps_) - 1);
        if (fraction > 1) fraction = 1;
        shown_ = true;
        shown_at_ = double(steps_) - 1 + fraction;
        out.fraction = float(fraction);
        return out;
    }
    uint64_t steps() const { return steps_; }
    double owed() const { return debt_; }

private:
    double debt_ = 0, shown_at_ = 0, owed_ = 0;
    uint64_t steps_ = 0;
    bool shown_ = false;
};

// A view as the engine describes one: where it is, which way it looks (pitch, yaw, roll), and how wide.
struct CameraPose {
    float location[3], rotation[3], fov, desired_fov;
};

// The newest two steps' views of one camera, and a picture between them.
class CameraHistory {
public:
    void forget() { seen_ = pair_ = false; }
    // The view the game gives at `step`. Asked again within the same step, it changes nothing. `cut` says the camera
    // jumped: the picture then shows the new view at once.
    bool capture(uint64_t step, const CameraPose& pose, bool cut) {
        const float* values = &pose.location[0];
        for (unsigned i = 0; i < 8; ++i)
            if (!finite(values[i])) return forget(), false;
        if (seen_ && step == step_ && !cut) return true;
        bool pair = seen_ && !cut && step == step_ + 1;
        float distance = 0;
        for (unsigned i = 0; i < 3; ++i) distance += (pose.location[i] - current_.location[i]) * (pose.location[i] - current_.location[i]);
        if (distance > 1000.0f * 1000.0f) pair = false; // a jump the camera did not call one is still not a slide
        previous_ = pair ? current_ : pose;
        current_ = pose;
        pair_ = pair;
        step_ = step;
        seen_ = true;
        return true;
    }
    // The view at `fraction` of the way from the step before to the newest. False with nothing captured.
    bool sample(float fraction, CameraPose& out) const {
        if (!seen_) return false;
        out = current_;
        if (!pair_ || !(fraction < 1)) return true;
        if (!(fraction > 0)) fraction = 0;
        for (unsigned i = 0; i < 3; ++i) out.location[i] = previous_.location[i] + (current_.location[i] - previous_.location[i]) * fraction;
        out.fov = previous_.fov + (current_.fov - previous_.fov) * fraction;
        out.desired_fov = previous_.desired_fov + (current_.desired_fov - previous_.desired_fov) * fraction;
        if (memcmp(previous_.rotation, current_.rotation, sizeof current_.rotation)) turn(previous_.rotation, current_.rotation, fraction, out.rotation);
        return true;
    }
    bool paired() const { return pair_; }

    // The shortest turn from one rotation to another, `fraction` of the way, through their quaternions.
    static void turn(const float from[3], const float to[3], float fraction, float out[3]) {
        float a[4], b[4];
        math::to_quaternion(from, a);
        math::to_quaternion(to, b);
        float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        if (dot < 0) {
            for (float& part : b) part = -part;
            dot = -dot;
        }
        float wa = 1 - fraction, wb = fraction;
        if (dot < 0.9995f) {
            const float angle = math::arctangent(math::square_root(1 - dot * dot), dot);
            const float sine = math::sine(angle);
            wa = math::sine((1 - fraction) * angle) / sine;
            wb = math::sine(fraction * angle) / sine;
        }
        float q[4], length = 0;
        for (unsigned i = 0; i < 4; ++i) q[i] = wa * a[i] + wb * b[i], length += q[i] * q[i];
        length = math::square_root(length);
        for (float& part : q) part /= length;
        math::to_angles(q, out);
    }

private:
    CameraPose previous_{}, current_{};
    uint64_t step_ = 0;
    bool seen_ = false, pair_ = false;
};

// A world's clocks as a view family is given them.
struct WorldTimes {
    float seconds = 0, delta = 0, real = 0;
};
inline bool same_bits(const WorldTimes& a, const WorldTimes& b) { return !memcmp(&a, &b, sizeof a); }

// The clocks a view family is given for a picture between steps: the world's and the real time at the same place
// as the camera, and as the frame's length the share of a step that the picture moved on. Without the presenter
// the world's clocks change only on a step, and every picture between would get the same time and then a step's
// worth at once, which moving materials show.
class ViewTime {
public:
    void forget() { seen_ = pair_ = shown_ = shown_pair_ = false; }
    // The world's clocks after each step.
    void capture(uint64_t step, const WorldTimes& times) {
        if (!finite(times.seconds) || !finite(times.delta) || !finite(times.real)) return forget();
        pair_ = seen_ && step == step_ + 1 && times.seconds >= current_.seconds && times.real >= current_.real;
        previous_ = pair_ ? current_ : times;
        current_ = times;
        step_ = step;
        seen_ = true;
        if (!pair_) shown_ = false;
    }
    // `given` is what the family was made with. False when that is not the newest step's clocks; the family then
    // keeps them. Every family of one frame gets the same.
    bool sample(uint64_t frame, uint64_t step, float fraction, const WorldTimes& given, WorldTimes& out) {
        if (!seen_ || step != step_ || !same_bits(given, current_) || !finite(fraction)) return false;
        if (shown_ && frame == frame_) return out = out_, true;
        if (fraction < 0) fraction = 0;
        if (fraction > 1) fraction = 1;
        const double progress = double(step) + fraction;
        WorldTimes times = current_;
        double world = current_.seconds;
        if (pair_) {
            world = double(previous_.seconds) + (double(current_.seconds) - previous_.seconds) * fraction;
            times.seconds = float(world);
            times.real = float(double(previous_.real) + (double(current_.real) - previous_.real) * fraction);
        }
        if (shown_) {
            const double moved = progress - progress_;
            // Between two pictures shown between steps, the delta is the world time they moved on, which spans
            // both steps' lengths when the picture crossed a step whose length changed.
            const bool along = pair_ && shown_pair_ && world >= world_;
            times.delta = moved < 0 || moved > 4 ? given.delta : along ? float(world - world_) : float(moved * given.delta);
            if (times.seconds < out_.seconds) times.seconds = out_.seconds;
            if (times.real < out_.real) times.real = out_.real;
        }
        shown_ = true;
        shown_pair_ = pair_;
        world_ = world;
        frame_ = frame;
        progress_ = progress;
        out_ = out = times;
        return true;
    }

private:
    WorldTimes previous_{}, current_{}, out_{};
    uint64_t step_ = 0, frame_ = 0;
    double progress_ = 0, world_ = 0; // the world time last shown, before it is kept as a float
    bool seen_ = false, pair_ = false, shown_ = false, shown_pair_ = false;
};

// The widgets that count frames: each runs once a step, given the length of every frame it let pass. One that
// has not been seen for a second gives its room to another.
class WidgetPaces {
public:
    static constexpr unsigned kRoom = 256;
    void forget() { memset(paces_, 0, sizeof paces_); }
    // Whether the widget `key` ticks now through its entry `entry`; when it does, `seconds` becomes what it let pass
    // and this frame together.
    bool admit(uint64_t key, uint32_t entry, uint64_t step, float& seconds) {
        Pace* room = nullptr;
        for (unsigned probe = 0; probe < kRoom; ++probe) {
            Pace& pace = paces_[(key * 0x9E3779B97F4A7C15ull + entry + probe) % kRoom];
            if (pace.key == key && pace.entry == entry) {
                if (pace.step == step) {
                    pace.waited += seconds;
                    return false;
                }
                seconds += pace.waited;
                pace.waited = 0;
                pace.step = step;
                return true;
            }
            if (!room && (!pace.key || pace.step + 30 < step)) room = &pace;
        }
        if (room) *room = {key, entry, step, 0};
        return true; // with no room it ticks as it would without the presenter
    }

private:
    struct Pace {
        uint64_t key;
        uint32_t entry;
        uint64_t step;
        float waited;
    };
    Pace paces_[kRoom]{};
};

// The visual base of a frame-counting widget. Only a widget whose own native tick reached this base is known,
// and the base is supplied at most once per picture, even when paced children call paced parents.
class WidgetBases {
public:
    static constexpr unsigned kRoom = 256;
    void forget() { memset(bases_, 0, sizeof bases_); }
    bool known(uint64_t key) const {
        for (const Base& base : bases_)
            if (base.key == key) return true;
        return false;
    }
    bool admit(uint64_t key, uint64_t frame) {
        Base* room = nullptr;
        for (Base& base : bases_) {
            if (base.key == key) {
                if (base.frame == frame) return false;
                base.frame = frame;
                return true;
            }
            if (!room && (!base.key || base.frame + 240 < frame)) room = &base;
        }
        if (room) *room = {key, frame};
        return true;
    }

private:
    struct Base { uint64_t key, frame; };
    Base bases_[kRoom]{};
};

// The buttons each controller had down at any picture since the last step, so a press shorter than a step still
// reaches the game, which reads its controllers only when it steps. Only a press the last step did not see is kept:
// one the game saw already is not made longer.
class InputLatch {
public:
    static constexpr unsigned kRoom = 8;
    void forget() { count_ = 0; }
    // A controller the game reads, by its number.
    void know(uint32_t id) {
        for (unsigned i = 0; i < count_; ++i)
            if (ids_[i] == id) return;
        if (count_ < kRoom) ids_[count_] = id, held_[count_] = seen_[count_] = 0, ++count_;
    }
    unsigned count() const { return count_; }
    uint32_t id(unsigned index) const { return ids_[index]; }
    // The buttons down at a picture between steps.
    void add(uint32_t id, uint64_t buttons) {
        for (unsigned i = 0; i < count_; ++i)
            if (ids_[i] == id) held_[i] |= buttons & ~seen_[i];
    }
    // What the game read at a step, with what was kept added.
    void read(uint32_t id, uint64_t buttons) {
        for (unsigned i = 0; i < count_; ++i)
            if (ids_[i] == id) seen_[i] = buttons;
    }
    uint64_t held(uint32_t id) const {
        for (unsigned i = 0; i < count_; ++i)
            if (ids_[i] == id) return held_[i];
        return 0;
    }
    void clear() {
        for (unsigned i = 0; i < count_; ++i) held_[i] = 0;
    }

private:
    uint32_t ids_[kRoom]{};
    uint64_t held_[kRoom]{}, seen_[kRoom]{};
    unsigned count_ = 0;
};


// ---- Places between steps of what moves in the world --------------------------------------------

// How far a picture at `at` (in steps: the step before the newest plus the picture's fraction) is between two
// steps `from` and `to` of something that moved, from 0 to 1. Something that moves only every few steps, as the
// game makes a far creature's pose, is shown that many steps behind, so its moves join up: a picture starts
// toward `to` once that step is the newest, and gets there as the next move could come.
inline float place_between(uint64_t from, uint64_t to, double at) {
    if (to <= from) return 1;
    const double f = (at - (double(to) - 1)) / double(to - from);
    return f < 0 ? 0.0f : f > 1 ? 1.0f : float(f);
}

// A place, a turn and a size: what a matrix the renderer is given is made of.
struct Trs {
    float t[3], q[4], s[3];
};

inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// A matrix as the engine keeps one: rows are the axes, each scaled, and the last row is the place. False for one
// that is no turn and size, such as one that is sheared or flat.
inline bool to_trs(const float m[16], Trs& out) {
    float rows[3][3];
    for (unsigned i = 0; i < 3; ++i) {
        const float length = math::square_root(dot3(m + 4 * i, m + 4 * i));
        if (!(length > 1e-6f) || !finite(length)) return false;
        out.s[i] = length;
        for (unsigned j = 0; j < 3; ++j) rows[i][j] = m[4 * i + j] / length;
    }
    if (dot3(rows[0], rows[1]) > 1e-3f || dot3(rows[0], rows[1]) < -1e-3f || dot3(rows[0], rows[2]) > 1e-3f ||
        dot3(rows[0], rows[2]) < -1e-3f || dot3(rows[1], rows[2]) > 1e-3f || dot3(rows[1], rows[2]) < -1e-3f)
        return false;
    const float cross[3] = {rows[0][1] * rows[1][2] - rows[0][2] * rows[1][1], rows[0][2] * rows[1][0] - rows[0][0] * rows[1][2],
                            rows[0][0] * rows[1][1] - rows[0][1] * rows[1][0]};
    if (dot3(cross, rows[2]) < 0) { // mirrored: the first axis carries the sign
        out.s[0] = -out.s[0];
        for (float& v : rows[0]) v = -v;
    }
    const float trace = rows[0][0] + rows[1][1] + rows[2][2];
    if (trace > 0) {
        const float root = math::square_root(trace + 1), half = 0.5f / root;
        out.q[3] = 0.5f * root;
        out.q[0] = (rows[1][2] - rows[2][1]) * half;
        out.q[1] = (rows[2][0] - rows[0][2]) * half;
        out.q[2] = (rows[0][1] - rows[1][0]) * half;
    } else {
        unsigned i = 0;
        if (rows[1][1] > rows[0][0]) i = 1;
        if (rows[2][2] > rows[i][i]) i = 2;
        const unsigned j = (i + 1) % 3, k = (j + 1) % 3;
        const float root = math::square_root(rows[i][i] - rows[j][j] - rows[k][k] + 1), half = 0.5f / root;
        out.q[i] = 0.5f * root;
        out.q[3] = (rows[j][k] - rows[k][j]) * half;
        out.q[j] = (rows[i][j] + rows[j][i]) * half;
        out.q[k] = (rows[i][k] + rows[k][i]) * half;
    }
    for (unsigned i = 0; i < 3; ++i) out.t[i] = m[12 + i];
    return true;
}

inline void to_matrix(const Trs& in, float m[16]) {
    const float x = in.q[0], y = in.q[1], z = in.q[2], w = in.q[3];
    const float x2 = x + x, y2 = y + y, z2 = z + z;
    const float xx = x * x2, yy = y * y2, zz = z * z2, yz = y * z2, wx = w * x2, xy = x * y2, wz = w * z2, xz = x * z2, wy = w * y2;
    const float out[16] = {(1 - (yy + zz)) * in.s[0], (xy + wz) * in.s[0], (xz - wy) * in.s[0], 0,
                           (xy - wz) * in.s[1], (1 - (xx + zz)) * in.s[1], (yz + wx) * in.s[1], 0,
                           (xz + wy) * in.s[2], (yz - wx) * in.s[2], (1 - (xx + yy)) * in.s[2], 0,
                           in.t[0], in.t[1], in.t[2], 1};
    memcpy(m, out, sizeof out);
}

// A turn `f` of the way from one to another, the short way round. Close turns are blended and made whole again,
// which is what a step's turn needs.
inline void blend_turn(const float a[4], const float b[4], float f, float out[4]) {
    const float sign = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0 ? -1.0f : 1.0f;
    float length = 0;
    for (unsigned i = 0; i < 4; ++i) out[i] = a[i] + (sign * b[i] - a[i]) * f, length += out[i] * out[i];
    length = math::square_root(length);
    if (length > 1e-6f)
        for (unsigned i = 0; i < 4; ++i) out[i] /= length;
}

inline void blend(const Trs& a, const Trs& b, float f, Trs& out) {
    for (unsigned i = 0; i < 3; ++i) out.t[i] = a.t[i] + (b.t[i] - a.t[i]) * f, out.s[i] = a.s[i] + (b.s[i] - a.s[i]) * f;
    blend_turn(a.q, b.q, f, out.q);
}

// The world bounds the engine keeps: a box's middle and half its size, and a sphere's radius around that middle.
// The bounds of a picture between two places hold both.
inline void hold_both(const float a[7], const float b[7], float out[7]) {
    float low[3], high[3];
    for (unsigned i = 0; i < 3; ++i) {
        low[i] = a[i] - a[3 + i] < b[i] - b[3 + i] ? a[i] - a[3 + i] : b[i] - b[3 + i];
        high[i] = a[i] + a[3 + i] > b[i] + b[3 + i] ? a[i] + a[3 + i] : b[i] + b[3 + i];
        out[i] = (low[i] + high[i]) * 0.5f;
        out[3 + i] = (high[i] - low[i]) * 0.5f;
    }
    float radius = 0;
    for (unsigned which = 0; which < 2; ++which) {
        const float* box = which ? b : a;
        float d[3] = {box[0] - out[0], box[1] - out[1], box[2] - out[2]};
        const float r = math::square_root(dot3(d, d)) + box[6];
        if (r > radius) radius = r;
    }
    out[6] = radius;
}

// ---- Snaps -------------------------------------------------------------------------------------------
//
// A turn, a place or a size that changed across a step far more than it changed across the step before snapped:
// a looping animation wrapped where its two ends are not the same pose (a flower whose petals look alike after a
// part of a turn, spun that part and back), an animation started again, a pose set outright. The pictures between
// the two steps would show it going the long way back through poses it never had, so it is shown at the newer step
// at once. Each change is taken a step at a time, so something moved every few steps compares like with like, and
// a steady motion, however fast, is never a snap.
constexpr float kSnapTurn = 0.7853982f; // 45 degrees: a step's turn below this is never a snap
constexpr float kSnapMove = 100;        // cm: nor a step's move below this
constexpr float kSnapSize = 0.5f;       // nor a step's change of size below half the larger size
constexpr float kSnapTimes = 4;         // and above them, a change is a snap only at more than this many times the step before's

// What changed across one step.
struct Change {
    float turn, move, size;
};

// The angle between two turns, in radians, whichever sign either quaternion has; 0 when either is no turn.
inline float turn_between(const float a[4], const float b[4]) {
    if (!(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3] > 0.5f) || !(b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3] > 0.5f))
        return 0;
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) d = -d;
    if (d > 1) d = 1;
    return 2 * math::arctangent(math::square_root(1 - d * d), d);
}

// The largest change of one axis's size, as a share of the larger of the two.
inline float size_between(const float a[3], const float b[3]) {
    float most = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const float x = a[i] < 0 ? -a[i] : a[i], y = b[i] < 0 ? -b[i] : b[i];
        const float larger = x > y ? x : y, d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        if (larger > 1e-6f && d / larger > most) most = d / larger;
    }
    return most;
}

// The change from one turn, place and size to another, `steps` steps apart, as a step's.
inline Change change_between(const float qa[4], const float ta[3], const float sa[3], const float qb[4], const float tb[3],
                             const float sb[3], uint64_t steps) {
    const float d[3] = {tb[0] - ta[0], tb[1] - ta[1], tb[2] - ta[2]};
    const float per = steps > 1 ? 1.0f / float(steps) : 1.0f;
    return {turn_between(qa, qb) * per, math::square_root(dot3(d, d)) * per, size_between(sa, sb) * per};
}

inline bool beyond(float now, float before, float least) {
    const float most = kSnapTimes * before > least ? kSnapTimes * before : least;
    return now > most;
}
// Whether the step's change is a snap after the step before's.
inline bool snapped(const Change& now, const Change& before) {
    return beyond(now.turn, before.turn, kSnapTurn) || beyond(now.move, before.move, kSnapMove) || beyond(now.size, before.size, kSnapSize);
}

// One bone of a pose as the renderer is given it: a turn, a place and a size, in 48 bytes.
struct BonePose {
    float q[4], t[3], s[3], unused[2];
};
static_assert(sizeof(BonePose) == 48, "a pose's bone is 48 bytes");

// What a pose's bone did across its last two steps, and whether the newest snapped. Zeroed, a bone not yet seen.
struct BoneMotion {
    Change before, now;
    uint8_t cut, known, measured, unused; // known: `before` was measured; measured: `now` was
};

// Takes each bone's change from `a` to `b`, `steps` steps apart. `advanced`: `b` is a newer step than the last
// taken, so the change last taken becomes the step before's; otherwise the newest step was made again. A bone's
// first change has nothing to be judged by and is never a snap. The count of bones that snapped.
inline uint32_t mark_snaps(const BonePose* a, const BonePose* b, uint32_t count, uint64_t steps, bool advanced, BoneMotion* motion) {
    uint32_t snaps = 0;
    for (uint32_t bone = 0; bone < count; ++bone) {
        BoneMotion& m = motion[bone];
        if (advanced) m.before = m.now, m.known = m.measured;
        m.now = change_between(a[bone].q, a[bone].t, a[bone].s, b[bone].q, b[bone].t, b[bone].s, steps);
        m.measured = 1;
        m.cut = m.known && snapped(m.now, m.before);
        snaps += m.cut;
    }
    return snaps;
}

// The bones `f` of the way from `a` to `b`; a bone `motion` says snapped is at `b`.
inline void blend_bones(const BonePose* a, const BonePose* b, uint32_t count, float f, BonePose* out, const BoneMotion* motion = nullptr) {
    for (uint32_t bone = 0; bone < count; ++bone) {
        out[bone] = b[bone];
        if (motion && motion[bone].cut) continue;
        for (unsigned i = 0; i < 3; ++i) {
            out[bone].t[i] = a[bone].t[i] + (b[bone].t[i] - a[bone].t[i]) * f;
            out[bone].s[i] = a[bone].s[i] + (b[bone].s[i] - a[bone].s[i]) * f;
        }
        blend_turn(a[bone].q, b[bone].q, f, out[bone].q);
    }
}

// A move from one step to the next too far to be one, shown at once: more than max(150 cm, four times the step
// before's).
inline bool jumped(const float from[3], const float to[3], float previous_move, float& move) {
    const float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    move = math::square_root(dot3(d, d));
    const float most = 4 * previous_move > 150 ? 4 * previous_move : 150;
    return move > most;
}

} // namespace tsuchi::presenter
