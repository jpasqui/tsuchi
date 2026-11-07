#pragma once

namespace tsuchi {
namespace ui_label_fit_detail {
inline bool finite(float value) {
    return value == value && value <= 3.402823466e38f && value >= -3.402823466e38f;
}
// The cached FGeometry prefix has 13 floats: local size [0,1], accumulated
// render matrix [7..10], and absolute render translation [11,12]. Only an
// upright, non-reflected layout is supported. Wait for a nonempty arrangement.
inline bool geometry(const float* value) {
    if (!value) return false;
    for (unsigned i = 0; i < 13; ++i) if (!finite(value[i])) return false;
    return value[0] > 0 && value[0] < 100000 && value[1] > 0 && value[1] < 100000 &&
        value[7] > 0 && value[7] < 100 && value[10] > 0 && value[10] < 100 &&
        value[8] == 0 && value[9] == 0;
}
inline bool changed(float next, float current, float tolerance) {
    return next < current - tolerance || next > current + tolerance;
}
} // namespace ui_label_fit_detail

struct UiLabelFitStamp {
    float x[10]{};
    bool known = false;
    bool waiting = false;
};

// Remember the arrangement used by a successful fit BEFORE calling native
// setters, which can reenter while cached geometry still has the old transform.
// Vertical scrolling must not make that stale arrangement eligible again.
// A clip/window change alone cannot release the gate: wait until the label's
// own cached X arrangement changes, including the transform written by the fit.
// Do not remember unsuccessful calculations.
// Keep one stamp per widget generation; clear it if its transform is reset.
inline bool ui_label_fit_seen(UiLabelFitStamp& stamp, const float* clip, const float* label) {
    if (!stamp.known || !clip || !label) return false;
    constexpr unsigned at[]{0, 7, 8, 9, 11};
    if (stamp.waiting) {
        bool same_label = true;
        for (unsigned i = 0; i < 5; ++i)
            if (stamp.x[i + 5] != label[at[i]]) same_label = false;
        if (same_label) return true;
        stamp.waiting = false;
    }
    for (unsigned i = 0; i < 5; ++i)
        if (stamp.x[i] != clip[at[i]] || stamp.x[i + 5] != label[at[i]]) return false;
    return true;
}
inline void ui_label_fit_remember(UiLabelFitStamp& stamp, const float* clip, const float* label) {
    if (!clip || !label) return;
    constexpr unsigned at[]{0, 7, 8, 9, 11};
    for (unsigned i = 0; i < 5; ++i)
        if (!ui_label_fit_detail::finite(clip[at[i]]) || !ui_label_fit_detail::finite(label[at[i]])) return;
    for (unsigned i = 0; i < 5; ++i) {
        stamp.x[i] = clip[at[i]];
        stamp.x[i + 5] = label[at[i]];
    }
    stamp.known = true;
    stamp.waiting = true;
}

// Adapted from the original manager's mod_label_fit.h. A caption is aligned to
// its right edge (X pivot 1, or an initial unit scale). Recover its unscaled
// rendered width so repeated fits do not shrink it again. The caller keeps the
// right pivot and vertical transform; neither desired size nor row size changes.
// Cached geometry must include the supplied current render transform. After
// applying a fit, wait for Slate to arrange that transform before fitting again.
// Both helpers leave their output arguments unchanged when returning false.
inline bool ui_label_fit(const float* clip, const float* label, float current, float& result) {
    using namespace ui_label_fit_detail;
    if (!geometry(clip) || !geometry(label) || !(current > 0 && current <= 2)) return false;
    const float left = clip[11] + 24.0f * clip[7];
    const float width = label[0] * label[7] / current;
    const float available = label[11] + label[0] * label[7] - left;
    if (!(width > 0 && width < 100000 && available > 0 && available < 100000)) return false;
    const float scale = available < width ? available / width : 1.0f;
    if (!changed(scale, current, 0.001f)) return false;
    result = scale;
    return true;
}

// A heading moves to the clip's left inset and uses X pivot 0. Compensate for
// its old pivot before changing it; otherwise an existing scale shifts the
// heading on every fit. The returned X is the widget's render translation.
inline bool ui_heading_fit(const float* clip, const float* label, float current,
                           float translation, float pivot, float& scale, float& x) {
    using namespace ui_label_fit_detail;
    if (!geometry(clip) || !geometry(label) || !(clip[0] > 48 && current > 0 && current <= 2) ||
        !(translation > -100000 && translation < 100000 && pivot >= 0 && pivot <= 1)) return false;
    const float parent = label[7] / current, width = label[0] * parent;
    const float available = (clip[0] - 48.0f) * clip[7];
    const float left = clip[11] + 24.0f * clip[7];
    if (!(parent > 0 && parent < 100 && width > 0 && width < 100000 &&
          available > 0 && available < 100000)) return false;
    const float next_scale = available < width ? available / width : 1.0f;
    const float next_x = translation + (left - label[11]) / parent +
        (1.0f - current) * pivot * label[0];
    if (!(next_x > -100000 && next_x < 100000) ||
        !(changed(next_scale, current, 0.001f) || changed(next_x, translation, 0.01f) || pivot != 0)) return false;
    scale = next_scale;
    x = next_x;
    return true;
}
} // namespace tsuchi
