#pragma once

#include <algorithm>
#include <cstddef>

namespace Slic3r { namespace Gpu {

// Whole boxes are conservative for filled polygon clipping. Retain hole and
// degenerate boxes as candidates too; an empty contour contributes nothing.
// On false the caller discards all emitted boxes. Disabled mode emits nothing.
template<class Contours, class Emit>
bool visit_filled_contour_boxes(bool enabled, const Contours& contours, Emit&& emit)
{
    if (!enabled)
        return false;
    constexpr size_t maximum_boxes = 128 * 1024;
    size_t count = 0;
    for (const auto& contour : contours) {
        if (contour.points.empty())
            continue;
        if (count == maximum_boxes)
            return false;
        auto min_x = contour.points.front().x();
        auto min_y = contour.points.front().y();
        auto max_x = min_x;
        auto max_y = min_y;
        for (const auto& point : contour.points) {
            min_x = std::min(min_x, point.x());
            min_y = std::min(min_y, point.y());
            max_x = std::max(max_x, point.x());
            max_y = std::max(max_y, point.y());
        }
        emit(min_x, min_y, max_x, max_y);
        ++count;
    }
    return true;
}

// Only call with the shared backend's fully validated batch. Its zero flags
// prove disjointness from every WHOLE contour box. Positives stay on Clipper.
// Preserve correspondence with the exact request-line vector, and seed both
// directed cache keys. Reject malformed metadata before writing any key.
template<class Batch, class Lines, class Emit>
bool consume_tree_negatives(const Batch& batch, const Lines& lines, Emit&& emit)
{
    if (!batch.dispatched || batch.may_intersect.size() != lines.size() ||
        std::any_of(batch.may_intersect.begin(), batch.may_intersect.end(),
                    [](auto value) { return value > 1; }))
        return false;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (batch.may_intersect[i] != 0)
            continue;
        emit(lines[i].a, lines[i].b);
        emit(lines[i].b, lines[i].a);
    }
    return true;
}

}} // namespace Slic3r::Gpu
