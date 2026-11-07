#include "ui_label_fit.h"
#include <array>
#include <cassert>
#include <cmath>
#include <limits>

using Geometry = std::array<float, 13>;
namespace {
Geometry geometry(float width, float scale, float x) {
    Geometry value{};
    value[0] = width; value[1] = 80;
    value[6] = 1; value[7] = scale; value[10] = scale; value[11] = x;
    return value;
}
bool near(float a, float b) { return std::fabs(a - b) < 0.005f; }

void captions() {
    // Stock list measurements at 1920x1080, repeated at different window sizes.
    // Apply a right-pivot render scale while leaving local width and Y unchanged.
    for (float window : {0.5f, 0.75f, 1.0f, 1.5f, 2.0f}) {
        auto clip = geometry(1468, 0.75f * window, 409.5f * window);
        auto label = geometry(902, 0.75f * window, 207.75f * window);
        const auto clip_before = clip, label_before = label;
        const float right = label[11] + label[0] * label[7];
        const float inset = clip[11] + 24 * clip[7];
        float next = -7;
        assert(tsuchi::ui_label_fit(clip.data(), label.data(), 1, next));
        assert(next > 0.67f && next < 0.68f);
        assert(clip == clip_before && label == label_before);
        const float current = next;
        label[7] = 0.75f * window * current;
        label[11] = right - label[0] * label[7];
        assert(near(label[11], inset));
        assert(near(label[11] + label[0] * label[7], right));
        assert(label[0] == 902 && label[1] == 80 && label[10] == label_before[10]);
        for (unsigned frame = 0; frame < 120; ++frame) {
            label[12] -= 3;
            next = -7;
            assert(!tsuchi::ui_label_fit(clip.data(), label.data(), current, next));
            assert(next == -7);
        }
        // Shorter text can return to unit scale while retaining its right edge.
        label[0] = 274;
        label[11] = right - label[0] * label[7];
        assert(tsuchi::ui_label_fit(clip.data(), label.data(), current, next) && next == 1);
        label[7] = 0.75f * window;
        label[11] = right - label[0] * label[7];
        assert(near(label[11] + label[0] * label[7], right));
        assert(!tsuchi::ui_label_fit(clip.data(), label.data(), 1, next));
    }
}

void headings() {
    for (float window : {0.5f, 0.75f, 1.0f, 1.5f, 2.0f}) {
        const float parent = 0.75f * window, base = 362.25f * window;
        const auto clip = geometry(1468, parent, 409.5f * window);
        for (float width : {292.0f, 2400.0f}) for (float old_pivot : {0.0f, 0.5f, 1.0f}) {
            float current = 0.7f, translation = 13, pivot = old_pivot;
            auto label = geometry(width, parent * current,
                base + (translation + (1 - current) * pivot * width) * parent);
            label[10] = parent;
            const auto before = label;
            float next = -7, x = -9;
            assert(tsuchi::ui_heading_fit(clip.data(), label.data(), current, translation, pivot, next, x));
            assert(label == before);
            current = next; translation = x; pivot = 0;
            label[7] = parent * current;
            label[11] = base + translation * parent;
            assert(near(label[11], clip[11] + 24 * parent));
            assert(label[11] + width * label[7] <= clip[11] + (clip[0] - 24) * parent + 0.005f);
            assert(label[0] == width && label[1] == 80 && label[10] == parent);
            if (width == 292) assert(current == 1);
            else assert(current < 1);
            for (unsigned frame = 0; frame < 120; ++frame) {
                label[12] -= 3;
                next = -7; x = -9;
                assert(!tsuchi::ui_heading_fit(clip.data(), label.data(), current, translation, pivot, next, x));
                assert(next == -7 && x == -9);
            }
            // New text also retains the left edge when restored to unit scale.
            label[0] = 100;
            if (current < 1) {
                assert(tsuchi::ui_heading_fit(clip.data(), label.data(), current, translation, pivot, next, x));
                assert(next == 1 && near(x, translation));
            }
        }
    }
}

void arrangement_gate() {
    auto clip = geometry(1468, 0.75f, 409.5f), label = geometry(902, 0.75f, 207.75f);
    tsuchi::UiLabelFitStamp stamp;
    float current = 1;
    unsigned applications = 0;
    auto fit = [&] {
        if (tsuchi::ui_label_fit_seen(stamp, clip.data(), label.data())) return false;
        float next = -7;
        if (!tsuchi::ui_label_fit(clip.data(), label.data(), current, next)) return false;
        tsuchi::ui_label_fit_remember(stamp, clip.data(), label.data());
        // Native setters update current immediately, before cached geometry.
        current = next; ++applications;
        return true;
    };
    assert(!tsuchi::ui_label_fit_seen(stamp, clip.data(), label.data()));
    assert(fit() && applications == 1 && current > 0.67f && current < 0.68f);
    const float first = current;
    assert(stamp.waiting);
    assert(!fit() && applications == 1 && current == first);
    clip[12] += 10; label[12] -= 30;
    assert(!fit() && applications == 1 && current == first);
    // A viewport/window update can arrive before this label is arranged. It
    // cannot make stale label geometry eligible for a second scale write.
    for (unsigned index : {0u, 7u, 8u, 9u, 11u}) {
        auto changed_clip = clip; changed_clip[index] += 0.5f;
        assert(tsuchi::ui_label_fit_seen(stamp, changed_clip.data(), label.data()));
        assert(stamp.waiting);
    }
    clip[0] += 32;
    assert(!fit() && applications == 1 && current == first);

    // Slate eventually paints the new transform. No further fit is needed;
    // unsuccessful calculations must leave the previous stamp alone.
    constexpr float right = 884.25f;
    label[7] = 0.75f * current;
    label[11] = right - label[0] * label[7];
    assert(!tsuchi::ui_label_fit_seen(stamp, clip.data(), label.data()));
    assert(!stamp.waiting);
    assert(!fit() && applications == 1);
    assert(!tsuchi::ui_label_fit_seen(stamp, clip.data(), label.data()));

    label[0] = 274;
    label[11] = right - label[0] * label[7];
    assert(fit() && applications == 2 && current == 1);
    assert(!fit() && applications == 2);

    // A new window arrangement permits the long caption to fit again.
    clip = geometry(1468, 1.5f, 819.0f);
    label = geometry(902, 1.5f, 415.5f);
    assert(!tsuchi::ui_label_fit_seen(stamp, clip.data(), label.data()));
    assert(fit() && applications == 3 && near(current, first));

    // Every requested X component participates; Y/height and layout-only data
    // do not defeat the gate while that horizontal arrangement is unchanged.
    tsuchi::ui_label_fit_remember(stamp, clip.data(), label.data());
    auto arranged = label; arranged[7] += 0.1f;
    assert(!tsuchi::ui_label_fit_seen(stamp, clip.data(), arranged.data()));
    assert(!stamp.waiting);
    for (unsigned index : {0u, 7u, 8u, 9u, 11u}) {
        auto value = clip; value[index] += 0.5f;
        assert(!tsuchi::ui_label_fit_seen(stamp, value.data(), label.data()));
        value = label; value[index] += 0.5f;
        assert(!tsuchi::ui_label_fit_seen(stamp, clip.data(), value.data()));
    }
    for (unsigned index : {1u, 2u, 3u, 4u, 5u, 6u, 10u, 12u}) {
        auto value = clip; value[index] += 0.5f;
        assert(tsuchi::ui_label_fit_seen(stamp, value.data(), label.data()));
        value = label; value[index] += 0.5f;
        assert(tsuchi::ui_label_fit_seen(stamp, clip.data(), value.data()));
    }
    assert(!tsuchi::ui_label_fit_seen(stamp, nullptr, label.data()));
    assert(!tsuchi::ui_label_fit_seen(stamp, clip.data(), nullptr));

    // Headings need the same gate across pivot and translation setters.
    stamp = {};
    clip = geometry(1468, 0.75f, 409.5f);
    current = 0.7f;
    float translation = 13, pivot = 0.5f, next = -7, x = -9;
    constexpr float width = 2400, base = 362.25f, parent = 0.75f;
    label = geometry(width, parent * current,
        base + (translation + (1 - current) * pivot * width) * parent);
    assert(tsuchi::ui_heading_fit(clip.data(), label.data(), current, translation, pivot, next, x));
    tsuchi::ui_label_fit_remember(stamp, clip.data(), label.data());
    current = next; translation = x; pivot = 0;
    assert(tsuchi::ui_label_fit_seen(stamp, clip.data(), label.data()));
    label[12] -= 20;
    assert(tsuchi::ui_label_fit_seen(stamp, clip.data(), label.data()));
    clip[0] += 32;
    assert(tsuchi::ui_label_fit_seen(stamp, clip.data(), label.data()));
    clip[0] -= 32;
    label[7] = parent * current; label[11] = base + translation * parent;
    assert(!tsuchi::ui_label_fit_seen(stamp, clip.data(), label.data()));
    assert(!stamp.waiting);
    assert(!tsuchi::ui_heading_fit(clip.data(), label.data(), current, translation, pivot, next, x));
}

void rejected(const float* clip, const float* label, float current = 1) {
    float result = -7, scale = -9, x = -11;
    assert(!tsuchi::ui_label_fit(clip, label, current, result));
    assert(!tsuchi::ui_heading_fit(clip, label, current, 0, 0, scale, x));
    assert(result == -7 && scale == -9 && x == -11);
}
void invalid() {
    const auto clip = geometry(1468, 0.75f, 409.5f), label = geometry(902, 0.75f, 207.75f);
    rejected(nullptr, label.data()); rejected(clip.data(), nullptr);
    for (float current : {0.0f, -1.0f, 2.01f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) rejected(clip.data(), label.data(), current);
    for (unsigned index : {0u, 1u, 7u, 10u}) for (float bad : {0.0f, -1.0f, 100000.0f}) {
        auto value = clip; value[index] = bad; rejected(value.data(), label.data());
        value = label; value[index] = bad; rejected(clip.data(), value.data());
    }
    // Rotation, shear, and reflected axes are unsupported even for captions.
    for (unsigned index : {8u, 9u}) for (float bad : {-0.5f, 0.5f}) {
        auto value = clip; value[index] = bad; rejected(value.data(), label.data());
        value = label; value[index] = bad; rejected(clip.data(), value.data());
    }
    for (unsigned index = 0; index < 13; ++index) for (float bad : {
            std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto value = clip; value[index] = bad; rejected(value.data(), label.data());
        value = label; value[index] = bad; rejected(clip.data(), value.data());
    }
    for (float bad : {-1.0f, 1.01f, std::numeric_limits<float>::quiet_NaN()}) {
        float scale = -7, x = -9;
        assert(!tsuchi::ui_heading_fit(clip.data(), label.data(), 1, 0, bad, scale, x));
        assert(scale == -7 && x == -9);
    }
    float scale = -7, x = -9;
    assert(!tsuchi::ui_heading_fit(clip.data(), label.data(), 1,
        std::numeric_limits<float>::infinity(), 0, scale, x));
    assert(scale == -7 && x == -9);
    auto narrow = clip; narrow[0] = 48;
    assert(!tsuchi::ui_heading_fit(narrow.data(), label.data(), 1, 0, 0, scale, x));
    auto unavailable = label; unavailable[11] = -1000;
    float result = -11;
    assert(!tsuchi::ui_label_fit(clip.data(), unavailable.data(), 1, result) && result == -11);
}
} // namespace

int main() { captions(); headings(); arrangement_gate(); invalid(); }
