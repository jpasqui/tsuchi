#pragma once
#include "ui_label_fit.h"

namespace tsuchi {
constexpr unsigned kUiFormLayoutRows = 128;

// One generated category owns this layout. Convert its arranged boundaries to
// the native scroller's heading/row heights, without choosing a row height.
inline bool ui_form_layout(const float* category, const float* heading,
                           const float* panel, const float* rows, unsigned count,
                           float spacing, float* heights, float& total) {
    using namespace ui_label_fit_detail;
    if (!geometry(category) || !geometry(heading) || !geometry(panel) || !rows || !heights ||
        !count || count > kUiFormLayoutRows || !ui_label_fit_detail::finite(spacing) || spacing < 0 || spacing >= 100000)
        return false;
    const auto close = [](float a, float b) { return a >= b - .01f && a <= b + .01f; };
    if (!close(panel[10], category[10]) || !close(panel[12], category[12]) ||
        !close(panel[1], category[1]) || !close(heading[10], category[10])) return false;
    float starts[kUiFormLayoutRows]{}, sizes[kUiFormLayoutRows]{};
    for (unsigned i = 0; i < count; ++i) {
        const float* row = rows + i * 13;
        if (!geometry(row) || !close(row[10], category[10])) return false;
        starts[i] = (row[12] - category[12]) / category[10];
        sizes[i] = row[1];
        if (!ui_label_fit_detail::finite(starts[i]) || starts[i] < 0 || starts[i] + sizes[i] > category[1] + .01f ||
            (i && starts[i] < starts[i - 1] + sizes[i - 1] - .01f)) return false;
    }
    const float heading_bottom = (heading[12] + heading[1] * heading[10] - category[12]) / category[10];
    if (!ui_label_fit_detail::finite(heading_bottom) || heading_bottom > starts[0] + .01f) return false;
    float candidate[kUiFormLayoutRows + 1]{};
    candidate[0] = starts[0] - spacing;
    if (candidate[0] <= 0) return false;
    for (unsigned i = 0; i < count; ++i) {
        candidate[i + 1] = i + 1 < count ? starts[i + 1] - starts[i] - spacing : category[1] - starts[i];
        if (!ui_label_fit_detail::finite(candidate[i + 1]) || candidate[i + 1] <= 0 || candidate[i + 1] < sizes[i] - .01f)
            return false;
    }
    for (unsigned i = 0; i <= count; ++i) heights[i] = candidate[i];
    total = category[1];
    return true;
}
} // namespace tsuchi
