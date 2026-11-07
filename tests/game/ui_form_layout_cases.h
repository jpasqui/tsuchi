#pragma once
#include "ui_form_layout.h"
#include <array>
#include <cassert>
#include <limits>

namespace ui_form_layout_test_detail {
using Geometry = std::array<float, 13>;
using Heights = std::array<float, tsuchi::kUiFormLayoutRows + 1>;

inline Geometry geometry(float height, float x, float y, float scale) {
    Geometry result{};
    result[0] = 1468;
    result[1] = height;
    result[7] = scale;
    result[10] = scale;
    result[11] = x;
    result[12] = y;
    return result;
}

struct Layout {
    Geometry category{}, heading{}, panel{};
    std::array<float, tsuchi::kUiFormLayoutRows * 13> rows{};
    unsigned count;
    float spacing;

    explicit Layout(unsigned number = 12, float gap = 0, float last_growth = 0,
                    float scale = 1, float x = 0, float y = 0)
        : count(number), spacing(gap) {
        const float height = 85 + number * (120 + gap) + last_growth;
        category = panel = geometry(height, x, y, scale);
        heading = geometry(85, x, y, scale);
        for (unsigned i = 0; i < number; ++i) {
            const float top = 85 + gap + i * (120 + gap);
            const Geometry arranged = geometry(120 + (i + 1 == number ? last_growth : 0),
                                               x, y + top * scale, scale);
            for (unsigned j = 0; j < 13; ++j) row(i)[j] = arranged[j];
        }
    }

    float* row(unsigned i) { return rows.data() + i * 13; }
    bool calculate(float* heights, float& total) const {
        return tsuchi::ui_form_layout(category.data(), heading.data(), panel.data(),
                                      rows.data(), count, spacing, heights, total);
    }
};

inline Heights sentinels() {
    Heights result{};
    for (unsigned i = 0; i < result.size(); ++i) result[i] = -700 - float(i);
    return result;
}

inline void expect_layout(const Layout& layout, float last_height, float expected_total) {
    Heights heights = sentinels();
    const Heights before = heights;
    // The observed native values undershoot the arranged 85/120 boundaries.
    heights[0] = 63;
    for (unsigned i = 1; i <= layout.count; ++i) heights[i] = 112;
    float total = 63 + float(layout.count) * 112;
    assert(layout.calculate(heights.data(), total));
    assert(heights[0] == 85 && total == expected_total);
    for (unsigned i = 1; i < layout.count; ++i) assert(heights[i] == 120);
    assert(heights[layout.count] == last_height);
    for (unsigned i = layout.count + 1; i < heights.size(); ++i)
        assert(heights[i] == before[i]);
}

inline void expect_rejected(const Layout& layout) {
    Heights heights = sentinels();
    const Heights before = heights;
    float total = -900;
    assert(!layout.calculate(heights.data(), total));
    assert(heights == before && total == -900);
}
} // namespace ui_form_layout_test_detail

inline void test_ui_form_layout() {
    using namespace ui_form_layout_test_detail;
    expect_layout(Layout{}, 120, 1525); // Captured 85 + twelve 120-unit rows.
    expect_layout(Layout{3, 8}, 120, 469); // Three native gaps, removed from entries.
    expect_layout(Layout{12, 0, 48}, 168, 1573); // Final row grows after wrapping.
    expect_layout(Layout{4, 4, 12, 1.25f, 321.25f, -137.5f}, 132, 593);
    expect_layout(Layout{tsuchi::kUiFormLayoutRows}, 120, 15445);
    Layout trailing_space{3};
    trailing_space.category[1] += 32;
    trailing_space.panel[1] += 32;
    expect_layout(trailing_space, 152, 477); // Final extent includes arranged tail.

    const auto reject = [](auto mutate) {
        Layout layout;
        mutate(layout);
        expect_rejected(layout);
    };
    reject([](Layout& layout) { layout.count = 0; });
    reject([](Layout& layout) { layout.count = tsuchi::kUiFormLayoutRows + 1; });
    reject([](Layout& layout) { layout.category[0] = std::numeric_limits<float>::quiet_NaN(); });
    reject([](Layout& layout) { layout.heading[6] = std::numeric_limits<float>::infinity(); });
    reject([](Layout& layout) { layout.row(11)[12] = std::numeric_limits<float>::infinity(); });
    reject([](Layout& layout) { layout.row(1)[8] = .25f; });
    reject([](Layout& layout) { layout.panel[9] = .25f; });
    reject([](Layout& layout) { layout.row(1)[10] = -1; });
    reject([](Layout& layout) { layout.heading[7] = -1; });
    reject([](Layout& layout) { layout.row(1)[10] = 1.25f; });
    reject([](Layout& layout) { layout.row(1)[12] -= 1; }); // Overlapping rows.
    reject([](Layout& layout) { layout.heading[1] += 1; }); // Heading overlaps first row.
    reject([](Layout& layout) { // Category clips the last row, with panel still coherent.
        layout.category[1] -= 1;
        layout.panel[1] -= 1;
    });
    reject([](Layout& layout) { layout.panel[12] += 1; });
    reject([](Layout& layout) { layout.panel[1] += 1; });
    reject([](Layout& layout) { layout.panel[10] = 1.25f; });
    reject([](Layout& layout) { layout.spacing = -1; });
    reject([](Layout& layout) { layout.spacing = std::numeric_limits<float>::quiet_NaN(); });
    reject([](Layout& layout) { layout.spacing = std::numeric_limits<float>::infinity(); });
    reject([](Layout& layout) { layout.spacing = 86; }); // No positive heading extent.
}
