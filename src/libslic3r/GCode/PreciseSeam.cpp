#include "PreciseSeam.hpp"
#include "SeamPlacer.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <unordered_map>
#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>

namespace Slic3r {
namespace PreciseSeam {

// Import EnforcedBlockedSeamPoint from SeamPlacerImpl namespace for convenience
using SeamPlacerImpl::EnforcedBlockedSeamPoint;

// Machine precision for checking exact coordinate matching (squared distance)
// Ideally, intersection points should match perimeter vertices bitwise,
// but we account for possible machine rounding errors in Clipper calculations
// Actual deviations: maximum ~0.27, using 2.5 with margin (nanometers)
static constexpr double MACHINE_PRECISION_SQUARED = 2.5;

// Tolerance for checking proximity when inserting seam points into perimeter
static const coord_t TOLERANCE_LINEAR = scale_(0.001);  // 1.0 micrometers
static const coord_t TOLERANCE_SQUARED = TOLERANCE_LINEAR * TOLERANCE_LINEAR;

// Finds the closest point of a polygon to the query point.
// Returns the foot point, the squared distance to it, and the index of the edge
// (or vertex) that produced that closest point; {foot_point, distance_squared, edge_index}
// edge_index == std::numeric_limits<size_t>::max()
// marks "no valid edge/vertex found" (e.g. empty polygon), so callers
// must check for that sentinel before dereferencing the index.
static std::tuple<Point, double, size_t>
project_point_onto_polygon(const Polygon &poly, const Point &point)
{
    Point proj = point;
    double dmin = std::numeric_limits<double>::max();
    const auto invalid = std::numeric_limits<size_t>::max();
    size_t best_edge = invalid;

    const auto &pts = poly.points;
    if (pts.empty()) {
        return {proj, dmin, best_edge};
    }

    for (size_t i = 0; i < pts.size(); ++i) {
        const size_t j = (i + 1 == pts.size()) ? 0 : i + 1;
        const Point &pt0 = pts[i];
        const Point &pt1 = pts[j];

        double d = (point - pt0).cast<double>().squaredNorm();
        if (d < dmin) {
            dmin = d;
            proj = pt0;
            best_edge = i;                  // closest vertex - its index
        }

        d = (point - pt1).cast<double>().squaredNorm();
        if (d < dmin) {
            dmin = d;
            proj = pt1;
            best_edge = j;
        }

        Vec2d v1(coordf_t(pt1(0) - pt0(0)), coordf_t(pt1(1) - pt0(1)));
        coordf_t div = v1.squaredNorm();
        if (div <= 0.) {
            continue;
        }

        Vec2d v2(coordf_t(point(0) - pt0(0)), coordf_t(point(1) - pt0(1)));
        coordf_t t = v1.dot(v2) / div;
        if (t <= 0. || t >= 1.) {
            continue;
        }

        Point foot(
            coord_t(std::floor(coordf_t(pt0(0)) + t * v1(0) + 0.5)),
            coord_t(std::floor(coordf_t(pt0(1)) + t * v1(1) + 0.5))
        );

        d = (point - foot).cast<double>().squaredNorm();
        if (d < dmin) {
            dmin = d;
            proj = foot;
            best_edge = i;                  // projection lies on edge starting at pts[i]
        }
    }

    return {proj, dmin, best_edge};
}

// Find common segment between intersection polygon and object perimeter
//
// REQUIREMENTS:
//   - intersection_polygon must be converted to CCW (counter-clockwise) beforehand
//   - perimeter_polygon must be converted to CCW (counter-clockwise) beforehand
//
// Parameters:
//   intersection_polygon - intersection area polygon (result of intersection()), CCW orientation
//   perimeter_polygon - object outline (outer perimeter), CCW orientation
//   modifier_polygon - modifier that formed the intersection
//
// Returns:
//   SegmentData - longest continuous segment with point correspondence
//   std::nullopt - if segment not found (< 2 points) or error
//
static std::optional<SegmentData> common_segment_in_intersection(
    const Polygon &intersection_polygon,
    const Polygon &perimeter_polygon,
    PreciseSeamWarnings* warnings = nullptr)
{
    const size_t isect_n = intersection_polygon.points.size();
    const size_t perim_n = perimeter_polygon.points.size();

    // Check for empty polygons
    if (isect_n == 0 || perim_n == 0) {
        return std::nullopt;
    }

    // Minimum 2 points required to form segment
    if (isect_n < 2) {
        return std::nullopt;
    }

    // ============================================================
    // STEP 1: Project all intersection points onto perimeter
    // ============================================================

    struct PointProjection {
        double dist_squared;    // Squared distance to perimeter
        size_t edge_index;      // Edge/vertex index of perimeter
        bool on_perimeter;      // Whether point lies on perimeter (within machine precision)
    };

    std::vector<PointProjection> projections;
    projections.reserve(isect_n);

    // Project each intersection point onto perimeter
    for (size_t i = 0; i < isect_n; ++i) {
        auto [proj_point, dist_sq, edge_idx] = project_point_onto_polygon(
            perimeter_polygon,
            intersection_polygon.points[i]
        );

        const auto invalid = std::numeric_limits<size_t>::max();
        if (edge_idx == invalid) {
            // Projection error - point does not belong to perimeter
            projections.push_back({dist_sq, edge_idx, false});
        } else {
            // Check whether point lies on perimeter (within machine precision)
            bool on_perim = (dist_sq <= MACHINE_PRECISION_SQUARED);
            projections.push_back({dist_sq, edge_idx, on_perim});
        }
    }

    // ============================================================
    // STEP 2: Find all continuous segments of points on perimeter
    // ============================================================

    std::vector<bool> processed(isect_n, false);  // Processed point flags
    std::vector<std::vector<size_t>> segments;    // Found segments (point indices)

    for (size_t start_idx = 0; start_idx < isect_n; ++start_idx) {
        // Skip processed or non-perimeter points
        if (processed[start_idx] || !projections[start_idx].on_perimeter) {
            continue;
        }

        // Found point on perimeter - search for continuous segment
        std::vector<size_t> backward_indices;  // Point indices backward from start_idx
        std::vector<size_t> forward_indices;   // Point indices forward from start_idx (including start_idx)

        // Add start point to forward
        forward_indices.push_back(start_idx);
        processed[start_idx] = true;

        // Backward traversal (only if start_idx == 0, for wrap-around handling)
        if (start_idx == 0) {
            for (size_t offset = 1; offset < isect_n; ++offset) {
                size_t curr_idx = (start_idx + isect_n - offset) % isect_n;

                // Stop if point already processed or not on perimeter
                if (processed[curr_idx] || !projections[curr_idx].on_perimeter) {
                    break;
                }

                backward_indices.push_back(curr_idx);
                processed[curr_idx] = true;
            }
        }

        // Forward traversal from start_idx
        for (size_t offset = 1; offset < isect_n; ++offset) {
            size_t curr_idx = (start_idx + offset) % isect_n;

            // Stop if point already processed or not on perimeter
            if (processed[curr_idx] || !projections[curr_idx].on_perimeter) {
                break;
            }

            forward_indices.push_back(curr_idx);
            processed[curr_idx] = true;
        }

        // Merge backward (in reverse order) + forward into one segment
        std::vector<size_t> segment_indices;
        segment_indices.reserve(backward_indices.size() + forward_indices.size());

        // Add backward in reverse order
        segment_indices.insert(
            segment_indices.end(),
            backward_indices.rbegin(),
            backward_indices.rend()
        );

        // Add forward
        segment_indices.insert(
            segment_indices.end(),
            forward_indices.begin(),
            forward_indices.end()
        );

        // Save found segment
        segments.push_back(std::move(segment_indices));
    }

    // ============================================================
    // STEP 3: Select longest segment
    // ============================================================

    // If no segments found
    if (segments.empty()) {
        return std::nullopt;
    }

    // Search for segment with maximum point count
    auto it_longest = std::max_element(
        segments.begin(),
        segments.end(),
        [](const auto &a, const auto &b) { return a.size() < b.size(); }
    );

    const std::vector<size_t> &longest_segment = *it_longest;

    // Check minimum requirement: >= 2 points
    if (longest_segment.size() < 2) {
        return std::nullopt;
    }

    // Special case: all intersection vertices lie on perimeter
    if (longest_segment.size() == isect_n) {
        // Threshold for edge midpoint check: increased by 0.5 due to rounding error in integer coordinate division
        constexpr double EDGE_CENTER_THRESHOLD = MACHINE_PRECISION_SQUARED + 0.5;

        // Check each intersection edge - does its midpoint lie on perimeter
        std::vector<size_t> edges_not_on_perim_indices;

        for (size_t i = 0; i < isect_n; ++i) {
            size_t next_i = (i + 1) % isect_n;

            // Calculate edge midpoint i→next_i
            const Point &pt1 = intersection_polygon.points[i];
            const Point &pt2 = intersection_polygon.points[next_i];
            Point edge_center(
                (pt1.x() + pt2.x()) / 2,
                (pt1.y() + pt2.y()) / 2
            );

            // Project midpoint onto perimeter
            auto [proj, dist_sq, edge_idx] = project_point_onto_polygon(
                perimeter_polygon,
                edge_center
            );

            // Check if midpoint lies on perimeter (accounting for rounding error)
            if (dist_sq > EDGE_CENTER_THRESHOLD) {
                edges_not_on_perim_indices.push_back(i);
            }
        }

        // Analyze results
        if (edges_not_on_perim_indices.empty()) {
            // All edges on perimeter → modifier fully contains perimeter → not suitable for seam placement
            if (warnings)
                warnings->full_containment.store(true, std::memory_order_relaxed);
            return std::nullopt;
        }

        // Edges not on perimeter act as "cuts" that split the circular ring of vertices
        // into separate on-perimeter segments. For k cuts there are k segments.
        // We iterate over consecutive pairs of cuts and pick the longest segment.
        const size_t k = edges_not_on_perim_indices.size();
        size_t best_start = 0;
        size_t best_length = 0;

        for (size_t i = 0; i < k; ++i) {
            size_t gap_cur  = edges_not_on_perim_indices[i];
            size_t gap_next = edges_not_on_perim_indices[(i + 1) % k];

            // Segment starts at the vertex right after the current cut
            size_t start  = (gap_cur + 1) % isect_n;
            // Number of vertices from start up to and including the vertex before the next cut
            size_t length = (gap_next - gap_cur - 1 + isect_n) % isect_n + 1;

            if (length > best_length) {
                best_length = length;
                best_start  = start;
            }
        }

        if (best_length < 2) {
            return std::nullopt;
        }

        // Form SegmentData from the longest on-perimeter segment
        SegmentData result;
        result.segment.points.reserve(best_length);
        result.perimeter_edge_indices.reserve(best_length);

        for (size_t i = 0; i < best_length; ++i) {
            size_t idx = (best_start + i) % isect_n;
            result.segment.points.push_back(intersection_polygon.points[idx]);
            result.perimeter_edge_indices.push_back(projections[idx].edge_index);
        }

        return result;
    }

    // ============================================================
    // STEP 4: Form SegmentData result
    // ============================================================

    SegmentData result;
    result.segment.points.reserve(longest_segment.size());
    result.perimeter_edge_indices.reserve(longest_segment.size());

    // Fill points and edge_index for each segment point
    for (size_t idx : longest_segment) {
        result.segment.points.push_back(intersection_polygon.points[idx]);
        result.perimeter_edge_indices.push_back(projections[idx].edge_index);
    }

    return result;
}

// Fast search for common segment between intersection polygon and object perimeter
// Hybrid algorithm: first exact coordinate matching, then geometric check
//
// REQUIREMENTS:
//   - intersection_polygon must be converted to CCW (counter-clockwise) beforehand
//   - perimeter_polygon must be converted to CCW (counter-clockwise) beforehand
//
// Parameters:
//   intersection_polygon - intersection area polygon (result of intersection()), CCW orientation
//   perimeter_polygon - object outline (outer perimeter), CCW orientation
//
// Returns:
//   SegmentData - continuous segment with point correspondence
//   std::nullopt - if segment not found (< 2 points) or error
//
static std::optional<SegmentData> common_segment_in_intersection_fast(
    const Polygon &intersection_polygon,
    const Polygon &perimeter_polygon,
    PreciseSeamWarnings* warnings = nullptr)
{
    const size_t isect_n = intersection_polygon.points.size();
    const size_t perim_n = perimeter_polygon.points.size();

    // ============================================================
    // STEP 1: Input data validation
    // ============================================================

    if (isect_n == 0 || perim_n == 0) {
        return std::nullopt;
    }

    if (isect_n < 2) {
        return std::nullopt;
    }

    // ============================================================
    // STEP 2: Find first point (exact coordinate match)
    // ============================================================

    // Vectors for forward direction
    std::vector<size_t> forward_intersection_indices;
    std::vector<size_t> forward_edge_indices;
    forward_intersection_indices.reserve(isect_n);
    forward_edge_indices.reserve(isect_n);

    // Vectors for backward direction
    std::vector<size_t> backward_intersection_indices;
    std::vector<size_t> backward_edge_indices;
    backward_intersection_indices.reserve(isect_n);
    backward_edge_indices.reserve(isect_n);

    size_t first_isect_idx = 0;   // index of first matching point in intersection_polygon
    size_t first_perim_idx = 0;   // index of first matching point in perimeter_polygon (also edge_index)
    bool found_first = false;

    // Search for first exact match (not optimized — expected gain is negligible)
    for (size_t i = 0; i < isect_n; ++i) {
        const Point &isect_pt = intersection_polygon.points[i];

        auto it = std::find(perimeter_polygon.points.begin(),
                           perimeter_polygon.points.end(),
                           isect_pt);

        if (it != perimeter_polygon.points.end()) {
            first_isect_idx = i;
            first_perim_idx = std::distance(perimeter_polygon.points.begin(), it);
            found_first = true;

            // Add first point to forward vectors
            forward_intersection_indices.push_back(i);
            forward_edge_indices.push_back(first_perim_idx);
            break;
        }
    }

    // If no matching point found - use full geometric algorithm
    if (!found_first) {
        return common_segment_in_intersection(intersection_polygon, perimeter_polygon, warnings);
    }

    // Sentinel value for invalid edge_index (returned by project_point_onto_polygon on error)
    const auto invalid = std::numeric_limits<size_t>::max();

    // ============================================================
    // STEP 3: Forward pass (from first point forward)
    // ============================================================

    // Adaptive tracking of position in perimeter (instead of fixed prediction)
    size_t next_expected_perim_idx = (first_perim_idx + 1) % perim_n;

    for (size_t offset = 1; offset < isect_n; ++offset) {
        size_t curr_isect_idx = (first_isect_idx + offset) % isect_n;
        const Point &curr_isect_pt = intersection_polygon.points[curr_isect_idx];
        const Point &expected_pt = perimeter_polygon.points[next_expected_perim_idx];

        // First check exact match with expected position
        if (curr_isect_pt == expected_pt) {
            forward_intersection_indices.push_back(curr_isect_idx);
            forward_edge_indices.push_back(next_expected_perim_idx);
            next_expected_perim_idx = (next_expected_perim_idx + 1) % perim_n;
            continue;
        }

        // Exact match not found - check geometrically
        auto [proj_point, dist_sq, edge_idx] = project_point_onto_polygon(
            perimeter_polygon,
            curr_isect_pt
        );

        if (edge_idx == invalid || dist_sq > MACHINE_PRECISION_SQUARED) {
            // Point not on perimeter - break forward pass
            break;
        }

        // Point on perimeter - add and adjust expected position
        forward_intersection_indices.push_back(curr_isect_idx);
        forward_edge_indices.push_back(edge_idx);
        next_expected_perim_idx = (edge_idx + 1) % perim_n;
    }

    // ============================================================
    // STEP 4: Backward pass
    // ============================================================

    // Optimization: backward can find maximum (isect_n - forward_count) points
    size_t forward_count = forward_intersection_indices.size();
    size_t max_backward_iterations = isect_n - forward_count;

    // Adaptive tracking of position in perimeter for backward direction
    next_expected_perim_idx = (first_perim_idx + perim_n - 1) % perim_n;

    for (size_t offset = 1; offset <= max_backward_iterations; ++offset) {
        size_t curr_isect_idx = (first_isect_idx + isect_n - offset) % isect_n;
        const Point &curr_isect_pt = intersection_polygon.points[curr_isect_idx];
        const Point &expected_pt = perimeter_polygon.points[next_expected_perim_idx];

        // First check exact match with expected position
        if (curr_isect_pt == expected_pt) {
            backward_intersection_indices.push_back(curr_isect_idx);
            backward_edge_indices.push_back(next_expected_perim_idx);
            next_expected_perim_idx = (next_expected_perim_idx + perim_n - 1) % perim_n;
            continue;
        }

        // Exact match not found - check geometrically
        auto [proj_point, dist_sq, edge_idx] = project_point_onto_polygon(
            perimeter_polygon,
            curr_isect_pt
        );

        if (edge_idx == invalid || dist_sq > MACHINE_PRECISION_SQUARED) {
            // Point not on perimeter - break backward pass
            break;
        }

        // Point on perimeter - add and adjust expected position
        backward_intersection_indices.push_back(curr_isect_idx);
        backward_edge_indices.push_back(edge_idx);
        next_expected_perim_idx = (edge_idx + perim_n - 1) % perim_n;
    }

    // Check special case: all intersection vertices lie on perimeter
    // Use full geometric algorithm (rare case but requires special handling)
    size_t total_points = forward_intersection_indices.size() + backward_intersection_indices.size();
    if (total_points == isect_n) {
        return common_segment_in_intersection(intersection_polygon, perimeter_polygon, warnings);
    }

    // ============================================================
    // STEP 5: Merge backward (reversed) + forward
    // ============================================================

    std::vector<size_t> continuous_intersection_indices;
    std::vector<size_t> continuous_edge_indices;

    if (backward_intersection_indices.empty()) {
        // No backward - just move forward
        continuous_intersection_indices = std::move(forward_intersection_indices);
        continuous_edge_indices = std::move(forward_edge_indices);
    } else {
        // Merge: backward (reversed) + forward
        size_t total_size = backward_intersection_indices.size() + forward_intersection_indices.size();
        continuous_intersection_indices.reserve(total_size);
        continuous_edge_indices.reserve(total_size);

        // Add backward in reverse order
        continuous_intersection_indices.insert(
            continuous_intersection_indices.end(),
            backward_intersection_indices.rbegin(),
            backward_intersection_indices.rend()
        );
        continuous_edge_indices.insert(
            continuous_edge_indices.end(),
            backward_edge_indices.rbegin(),
            backward_edge_indices.rend()
        );

        // Add forward
        continuous_intersection_indices.insert(
            continuous_intersection_indices.end(),
            forward_intersection_indices.begin(),
            forward_intersection_indices.end()
        );
        continuous_edge_indices.insert(
            continuous_edge_indices.end(),
            forward_edge_indices.begin(),
            forward_edge_indices.end()
        );
    }

    // ============================================================
    // STEP 6: Check minimum size and special cases
    // ============================================================

    if (continuous_intersection_indices.size() < 2) {
        return std::nullopt;
    }

    // ============================================================
    // STEP 7: Form SegmentData result
    // ============================================================

    SegmentData result;
    result.segment.points.reserve(continuous_intersection_indices.size());
    result.perimeter_edge_indices.reserve(continuous_intersection_indices.size());

    // Fill with original coordinates from intersection_polygon + edge_index
    for (size_t i = 0; i < continuous_intersection_indices.size(); ++i) {
        size_t idx = continuous_intersection_indices[i];
        result.segment.points.push_back(intersection_polygon.points[idx]);
        result.perimeter_edge_indices.push_back(continuous_edge_indices[i]);
    }

    return result;
}

void init_precise_seam_data(
    std::vector<const ModelVolume*>& strong_volumes_out,
    std::vector<const ModelVolume*>& weak_volumes_out,
    bool& has_strong_out,
    const ModelObject* model_object)
{
    // Clear output vectors
    strong_volumes_out.clear();
    weak_volumes_out.clear();

    if (model_object == nullptr) {
        has_strong_out = false;
        return;
    }

    // Collect and categorize precise seam modifiers
    for (const ModelVolume* volume : model_object->volumes) {
        if (volume->is_precise_seam()) {
            ModelVolumeType type = volume->type();
            // Categorization: strong modifiers have priority
            if (type == ModelVolumeType::PRECISE_SEAM_CENTER ||
                type == ModelVolumeType::PRECISE_SEAM_LEFT ||
                type == ModelVolumeType::PRECISE_SEAM_RIGHT) {
                strong_volumes_out.push_back(volume);
            } else {
                // ENFORCED, BLOCKED, NEUTRAL - weak modifiers (processed later)
                weak_volumes_out.push_back(volume);
            }
        }
    }

    has_strong_out = !strong_volumes_out.empty();

    // Sort modifiers by their position in model_object->volumes[].
    // Drag & drop in GUI directly reorders volumes[], so this reflects user intent.
    // Works identically for GUI and CLI (3MF preserves volumes[] order).
    auto get_position = [&](const ModelVolume* vol) -> size_t {
        auto it = std::find(model_object->volumes.begin(),
                           model_object->volumes.end(), vol);
        return (it != model_object->volumes.end())
            ? std::distance(model_object->volumes.begin(), it)
            : SIZE_MAX;
    };

    // Strong modifiers: top-down (earlier in volumes[] = higher priority)
    if (!strong_volumes_out.empty()) {
        std::stable_sort(strong_volumes_out.begin(), strong_volumes_out.end(),
            [&](const ModelVolume* a, const ModelVolume* b) {
                return get_position(a) < get_position(b);
            });
    }

    // Weak modifiers: sorted low-priority-first (bottom of tree first).
    // Earlier in volumes[] = higher in object tree = higher priority.
    // Application uses "last write wins", so higher-priority modifiers
    // (earlier in volumes[], placed last in this sorted order) overwrite
    // lower-priority ones, producing the correct hierarchy.
    if (!weak_volumes_out.empty()) {
        std::stable_sort(weak_volumes_out.begin(), weak_volumes_out.end(),
            [&](const ModelVolume* a, const ModelVolume* b) {
                return get_position(a) > get_position(b);
            });
    }
}

// Calculate cumulative lengths for each Polyline point
// Analog of Polygon::parameter_by_length(), adapted for open line
static std::vector<float> polyline_parameter_by_length(const Polyline &polyline)
{
    // Parameterize polyline by its length
    std::vector<float> lengths(polyline.points.size(), 0.f);
    for (size_t i = 1; i < polyline.points.size(); ++i) {
        lengths[i] = lengths[i-1] + (polyline.points[i] - polyline.points[i-1]).cast<float>().norm();
    }
    return lengths;
}

// Find geometric center coordinates of segment
// Returns: {center coordinates, perimeter vertex index}
// Index is start vertex of edge containing center
static std::optional<std::pair<Point, size_t>> segment_center(
    const SegmentData &data,
    const Polygon &perimeter_polygon
)
{
    const Polyline &segment = data.segment;

    if (segment.points.size() < 2) {
        return std::nullopt; // Need at least a line to find middle
    }

    std::vector<float> lengths = polyline_parameter_by_length(segment);
    if (lengths.empty()) {
        return std::nullopt; // Polyline contains no points
    }

    float half_length = lengths.back() * 0.5f; // Take half of total length
    size_t mid_idx = segment.points.size() / 2;
    float mid_length = lengths[mid_idx];

    bool found = false;
    size_t start_idx = 0;
    size_t end_idx = 0;

    if (mid_length < half_length) {
        // Go right (to end)
        for (size_t i = mid_idx; i < segment.points.size() - 1; ++i) {
            if (lengths[i] <= half_length && half_length < lengths[i+1]) {
                start_idx = i;     // Fix left point of segment
                end_idx = i + 1;   // Fix right point of segment
                found = true;
                break;
            }
        }
    } else if (mid_length > half_length) {
        // Go left (to start)
        for (size_t i = mid_idx; i > 0; --i) {
            if (lengths[i-1] <= half_length && half_length < lengths[i]) {
                start_idx = i - 1; // Take neighboring point on left
                end_idx = i;       // And nearest on right
                found = true;
                break;
            }
        }
    } else {
        // Middle sits exactly at vertex
        start_idx = mid_idx;
        end_idx = (mid_idx + 1) % segment.points.size(); // use next point (wrap-around)
        found = true;
    }

    if (!found) {
        return std::nullopt; // Didn't find suitable segment
    }

    const Point &p1 = segment.points[start_idx];
    const Point &p2 = segment.points[end_idx];

    float local_mid_length = half_length - lengths[start_idx];
    float edge_length = lengths[end_idx] - lengths[start_idx];

    Point mid_point;
    if (edge_length <= 0.0f) {
        mid_point = p1; // Degenerate case, take start point
    } else {
        float k = local_mid_length / edge_length;
        mid_point = p1 + (k * (p2 - p1).cast<float>()).cast<coord_t>(); // Linear interpolation
    }

    // Take edge_index from start point of segment containing center
    size_t edge_idx = data.perimeter_edge_indices[start_idx];

    return std::make_pair(mid_point, edge_idx);
}

// Find coordinates of left (first) point of segment
// Returns: {first point coordinates, perimeter vertex index}
// Index is start vertex of edge containing first point
static std::optional<std::pair<Point, size_t>> segment_left(
    const SegmentData &data,
    const Polygon &perimeter_polygon
)
{
    if (data.segment.points.empty()) {
        return std::nullopt;
    }

    return std::make_pair(data.segment.points[0], data.perimeter_edge_indices[0]);
}

// Find coordinates of right (last) point of segment
// Returns: {last point coordinates, perimeter vertex index}
// Index is start vertex of edge containing last point
static std::optional<std::pair<Point, size_t>> segment_right(
    const SegmentData &data,
    const Polygon &perimeter_polygon
)
{
    if (data.segment.points.empty()) {
        return std::nullopt;
    }

    size_t last_idx = data.segment.points.size() - 1;
    return std::make_pair(data.segment.points[last_idx], data.perimeter_edge_indices[last_idx]);
}

// Insert point into perimeter with proximity check to existing vertices
// If point is close to vertex (< TOLERANCE_SQUARED) - use existing vertex
// Returns pair: {final coordinates, point index in polygon}
// edge_start_idx is start vertex of edge containing point
static std::optional<std::pair<Point, size_t>> insert_point_into_perimeter(
    const Point &point,
    size_t edge_start_idx,
    Polygon &perimeter_polygon
)
{
    // Check input data
    if (perimeter_polygon.points.size() < 3) {
        return std::nullopt;  // Polygon must be at least a triangle
    }

    size_t perim_max = perimeter_polygon.points.size();

    // Determine edge start and end
    size_t vtx_start = edge_start_idx;
    size_t vtx_end = (edge_start_idx + 1) % perim_max;

    const Point &perim_p_start = perimeter_polygon.points[vtx_start];
    const Point &perim_p_end = perimeter_polygon.points[vtx_end];

    // Check proximity to edge vertices
    coord_t dist_sq_start = (point - perim_p_start).squaredNorm();
    if (dist_sq_start < TOLERANCE_SQUARED) {
        return std::make_pair(perim_p_start, vtx_start);
    }

    coord_t dist_sq_end = (point - perim_p_end).squaredNorm();
    if (dist_sq_end < TOLERANCE_SQUARED) {
        return std::make_pair(perim_p_end, vtx_end);
    }

    // Insert point into perimeter
    // IMPORTANT: Special handling for the last edge to preserve indexing for subsequent insertions.
    // If this is the last edge (edge_start_idx == perim_max - 1), we append to the end instead of
    // inserting at position 0 (which would shift all indices). This allows sorting points by
    // descending arc length and inserting them without invalidating previously computed indices.
    size_t insert_pos;
    if (edge_start_idx == perim_max - 1) {
        // Last edge: add to end of vector
        perimeter_polygon.points.push_back(point);
        insert_pos = perimeter_polygon.points.size() - 1;
    } else {
        // Regular edge: insert before end vertex
        insert_pos = vtx_end;
        perimeter_polygon.points.insert(
            perimeter_polygon.points.begin() + insert_pos,
            point
        );
    }

    return std::make_pair(perimeter_polygon.points[insert_pos], insert_pos);
}

// Insert new point at distance TOLERANCE_LINEAR from specified perimeter vertex
// Insertion direction specified by direction parameter: +1 = after vertex, -1 = before vertex
// If target edge length < 2*TOLERANCE_LINEAR, insertion not performed (new point would be too close to edge end)
// Returns true if point was inserted, false otherwise
// point_idx is index of perimeter vertex from which insertion is performed
static bool refine_at_vertex(
    size_t point_idx,
    int direction,
    Polygon &perimeter_polygon
)
{
    // Check input data
    if (perimeter_polygon.points.size() < 3) {
        return false;  // Polygon must be at least a triangle
    }

    if (direction != 1 && direction != -1) {
        return false;  // Direction must be +1 or -1
    }

    size_t perim_max = perimeter_polygon.points.size();

    // Determine target edge based on direction
    size_t edge_start_idx, edge_end_idx;

    if (direction == 1) {
        // Direction +1: insertion AFTER point_idx (edge point_idx → point_idx+1)
        edge_start_idx = point_idx;
        edge_end_idx = (point_idx + 1) % perim_max;
    } else {
        // Direction -1: insertion BEFORE point_idx (edge point_idx-1 → point_idx)
        edge_start_idx = (point_idx + perim_max - 1) % perim_max;
        edge_end_idx = point_idx;
    }

    const Point &edge_start = perimeter_polygon.points[edge_start_idx];
    const Point &edge_end = perimeter_polygon.points[edge_end_idx];

    // Calculate edge length
    Vec2d edge_vector = (edge_end - edge_start).cast<double>();
    double edge_length = edge_vector.norm();

    // Check if edge is long enough for insertion
    // New point must be at distance TOLERANCE_LINEAR from start
    // and at distance >= TOLERANCE_LINEAR from end
    if (edge_length < 2.0 * TOLERANCE_LINEAR) {
        return false;  // Edge too short - new point would be too close to end
    }

    // Calculate new point coordinates: edge_start + TOLERANCE_LINEAR * direction_normalized
    Vec2d direction_normalized = edge_vector / edge_length;
    // Place helper point near point_idx: after it for +1, before it for -1
    auto offset = (TOLERANCE_LINEAR * direction_normalized).cast<coord_t>();
    Point new_point = (direction == 1)
        ? Point(edge_start + offset)
        : Point(edge_end   - offset);

    // Insert point into perimeter
    // IMPORTANT: Special handling of last edge to preserve indexing for subsequent insertions.
    // If this is last edge (edge_start_idx == perim_max - 1), add point to end of vector
    // instead of inserting at position 0 (which would shift all indices). This allows sorting points
    // by descending arc length and inserting them without invalidating previously computed indices.
    if (edge_start_idx == perim_max - 1) {
        // Last edge: add to end of vector
        perimeter_polygon.points.push_back(new_point);
    } else {
        // Regular edge: insert before end vertex
        perimeter_polygon.points.insert(
            perimeter_polygon.points.begin() + edge_end_idx,
            new_point
        );
    }

    return true;
}

// Insert strong seam point into perimeter polygon
std::optional<Point> insert_strong_seam_point(
    const std::vector<const ModelVolume*> &strong_volumes,
    Polygon &polygon,
    const Layer *layer,
    const ModifierSlicesCache &slices_cache,
    PreciseSeamWarnings* warnings)
{
    if (strong_volumes.empty() || layer == nullptr) {
        return std::nullopt;
    }

    // layer->id() is offset by raft layer count, but modifier_slices is 0-based
    // (built from PrintObject::layers() via slice_single_volume). Subtract raft
    // offset to get the correct index into the cache.
    const size_t raft_layers = layer->object()->slicing_parameters().raft_layers();
    size_t layer_id = layer->id() - raft_layers;

    // Iterate through strong modifiers in hierarchy order
    for (const ModelVolume* modifier_volume : strong_volumes) {
        // Look up pre-sliced polygons from cache (sliced once in SeamPlacer::init).
        // TODO: slice_single_volume() converts ExPolygons to flat Polygons, losing
        // the association between outer contours and their holes. This makes correct
        // handling of multiply-connected modifier regions (e.g. a torus cross-section)
        // impossible. Consider a variant returning std::vector<ExPolygons> and adapting
        // the algorithm to work with multiply-connected domains.
        auto it = slices_cache.find(modifier_volume);
        if (it == slices_cache.end())
            continue; // modifier not in cache (should not happen)
        const std::vector<Polygons> &modifier_slices = it->second;

        // Check if this layer has slices for this modifier
        if (layer_id >= modifier_slices.size()) {
            continue; // No slices for this layer
        }

        const Polygons &modifier_polygons = modifier_slices[layer_id];

        // Check for multiply-connected regions (holes = CW polygons).
        // slice_single_volume() flattens ExPolygons into Polygons, but preserves
        // orientation: CCW = outer contour, CW = hole. If any CW polygon is present,
        // the modifier is multiply-connected and cannot be processed correctly.
        bool has_holes = std::any_of(modifier_polygons.begin(), modifier_polygons.end(),
            [](const Polygon &p) { return p.is_clockwise(); });
        if (has_holes) {
            if (warnings)
                warnings->multiply_connected.store(true, std::memory_order_relaxed);
            continue;
        }

        // Iterate through all polygons of the modifier on this layer
        // After finding a match, check if remaining modifier polygons also intersect the perimeter.
        // Strong modifiers process only one intersection (one seam per perimeter), so any additional
        // intersections from unprocessed polygons indicate a multiple-intersection situation.
        auto check_remaining_polygons = [&](size_t current_idx) {
            if (warnings && !warnings->multiple_intersections.load(std::memory_order_relaxed)) {
                for (size_t j = current_idx + 1; j < modifier_polygons.size(); ++j) {
                    if (!intersection(Polygons{polygon}, Polygons{modifier_polygons[j]}).empty()) {
                        warnings->multiple_intersections.store(true, std::memory_order_relaxed);
                        break;  // one extra intersection is enough to trigger the warning
                    }
                }
            }
        };
        for (size_t modifier_polygon_idx = 0; modifier_polygon_idx < modifier_polygons.size(); ++modifier_polygon_idx) {
            const Polygon &modifier_polygon = modifier_polygons[modifier_polygon_idx];
            // Find intersection with perimeter
            Polygons intersection_polygons = intersection(Polygons{polygon}, Polygons{modifier_polygon});

            // Multiple intersection polygons = modifier crosses perimeter in several places
            if (warnings && intersection_polygons.size() > 1)
                warnings->multiple_intersections.store(true, std::memory_order_relaxed);
            // Diff check: modifier minus perimeter yields >1 polygon = through-body intersection.
            // However, if any diff polygon is CW, it is a hole left by full containment
            // (modifier fully covers perimeter), not a real through-body case.
            // Full containment is detected separately in common_segment_in_intersection().
            if (warnings && !warnings->through_body.load(std::memory_order_relaxed)) {
                Polygons diff_polygons = diff(Polygons{modifier_polygon}, Polygons{polygon});
                if (diff_polygons.size() > 1) {
                    bool has_cw = std::any_of(diff_polygons.begin(), diff_polygons.end(),
                        [](const Polygon &p) { return p.is_clockwise(); });
                    if (!has_cw)
                        warnings->through_body.store(true, std::memory_order_relaxed);
                }
            }

            // Process each intersection polygon
            for (Polygon &intersection_polygon : intersection_polygons) {
                // Convert intersection_polygon to CCW to guarantee same traversal direction as perimeter
                intersection_polygon.make_counter_clockwise();

                // Try to find perimeter segment in this intersection
                std::optional<SegmentData> segment = common_segment_in_intersection_fast(
                    intersection_polygon,
                    polygon,
                    warnings
                );

                if (!segment.has_value()) {
                    continue;
                }

                // Select target point finder based on modifier type
                std::optional<std::pair<Point, size_t>> target;
                switch (modifier_volume->type()) {
                    case ModelVolumeType::PRECISE_SEAM_CENTER: target = segment_center(segment.value(), polygon); break;
                    case ModelVolumeType::PRECISE_SEAM_LEFT:   target = segment_left(segment.value(), polygon);   break;
                    case ModelVolumeType::PRECISE_SEAM_RIGHT:  target = segment_right(segment.value(), polygon);  break;
                    default: continue;
                }

                if (!target.has_value())
                    continue;

                // Insert target point into perimeter with tolerance check
                std::optional<std::pair<Point, size_t>> result = insert_point_into_perimeter(
                    target->first,  // target_point
                    target->second, // insert_idx
                    polygon
                );

                if (!result.has_value())
                    continue;

                // Add additional points on both sides to create transition zone.
                // +1 must be called before -1: reverse order shifts result->second and breaks insertion.
                refine_at_vertex(result->second, +1, polygon);
                refine_at_vertex(result->second, -1, polygon);

                check_remaining_polygons(modifier_polygon_idx);
                return result->first;
            }
        }
    }

    // No matching segment found or insertion failed
    return std::nullopt;
}

// Convert ModelVolumeType of weak modifier to EnforcedBlockedSeamPoint.
// Precondition: called only with weak precise-seam types (filtered via is_precise_seam_weak()).
// Exhaustive switch (no default) so -Wswitch flags any future PRECISE_SEAM_* additions.
static EnforcedBlockedSeamPoint convert_weak_modifier_type(ModelVolumeType type) {
    switch (type) {
        case ModelVolumeType::PRECISE_SEAM_ENFORCED:
            return EnforcedBlockedSeamPoint::Enforced;
        case ModelVolumeType::PRECISE_SEAM_BLOCKED:
            return EnforcedBlockedSeamPoint::Blocked;
        case ModelVolumeType::PRECISE_SEAM_NEUTRAL:
            return EnforcedBlockedSeamPoint::Neutral;
        // Non-weak types are unreachable by precondition; listed to keep the switch exhaustive.
        case ModelVolumeType::INVALID:
        case ModelVolumeType::MODEL_PART:
        case ModelVolumeType::NEGATIVE_VOLUME:
        case ModelVolumeType::PARAMETER_MODIFIER:
        case ModelVolumeType::SUPPORT_BLOCKER:
        case ModelVolumeType::SUPPORT_ENFORCER:
        case ModelVolumeType::PRECISE_SEAM_CENTER:
        case ModelVolumeType::PRECISE_SEAM_LEFT:
        case ModelVolumeType::PRECISE_SEAM_RIGHT:
            break;
    }
    assert(false && "convert_weak_modifier_type called with non-weak type");
    return EnforcedBlockedSeamPoint::Neutral;
}

// Collect all weak modifier segments for given perimeter
// Process weak modifiers (ENFORCED/BLOCKED/NEUTRAL) and collect segment boundaries
// Also insert boundary points into perimeter polygon (sorted by descending arc length)
// Split enforced edges into small segments (≤ enforcer_oversampling_distance) for precise seam placement
// Return ordered vector of segments with updated coordinates (in same order as weak_volumes list)
std::vector<WeakModifierSegment> collect_weak_modifier_segments(
    const std::vector<const ModelVolume*> &strong_volumes,
    const std::vector<const ModelVolume*> &weak_volumes,
    Polygon &polygon,
    const Layer *layer,
    const ModifierSlicesCache &slices_cache,
    PreciseSeamWarnings* warnings)
{
    std::vector<WeakModifierSegment> result;

    // Check input parameters
    if (weak_volumes.empty() || layer == nullptr) {
        return result; // Empty vector
    }

    // layer->id() is offset by raft layer count, but modifier_slices is 0-based
    // (built from PrintObject::layers() via slice_single_volume). Subtract raft
    // offset to get the correct index into the cache.
    const size_t raft_layers = layer->object()->slicing_parameters().raft_layers();
    size_t layer_id = layer->id() - raft_layers;

    // Iterate through all weak modifiers in hierarchy order
    for (const ModelVolume* modifier_volume : weak_volumes) {
        // Look up pre-sliced polygons from cache (sliced once in SeamPlacer::init).
        // TODO: slice_single_volume() converts ExPolygons to flat Polygons, losing
        // the association between outer contours and their holes. This makes correct
        // handling of multiply-connected modifier regions (e.g. a torus cross-section)
        // impossible. Consider a variant returning std::vector<ExPolygons> and adapting
        // the algorithm to work with multiply-connected domains.
        auto it = slices_cache.find(modifier_volume);
        if (it == slices_cache.end())
            continue; // modifier not in cache (should not happen)
        const std::vector<Polygons> &modifier_slices = it->second;

        // Check if slices exist for given layer
        if (layer_id >= modifier_slices.size()) {
            continue; // No slices for this layer
        }

        const Polygons &modifier_polygons = modifier_slices[layer_id];

        // Check for multiply-connected regions (holes = CW polygons).
        // slice_single_volume() flattens ExPolygons into Polygons, but preserves
        // orientation: CCW = outer contour, CW = hole. If any CW polygon is present,
        // the modifier is multiply-connected and cannot be processed correctly.
        bool has_holes = std::any_of(modifier_polygons.begin(), modifier_polygons.end(),
            [](const Polygon &p) { return p.is_clockwise(); });
        if (has_holes) {
            if (warnings)
                warnings->multiply_connected.store(true, std::memory_order_relaxed);
            continue;
        }

        // Iterate through all modifier polygons on this layer
        for (const Polygon &modifier_polygon : modifier_polygons) {
            // Find intersection with perimeter
            Polygons intersection_polygons = intersection(Polygons{polygon}, Polygons{modifier_polygon});

            // Note: intersection_polygons.size() > 1 is NOT flagged as a warning here.
            // For weak modifiers, multiple intersection polygons are expected (the modifier
            // may legitimately cross the perimeter in several places).
            // Only through-body intersections (detected by diff below) are abnormal.

            // Diff check: if modifier minus perimeter yields >1 polygon, the modifier
            // passes through the model body, creating a through-body intersection.
            // CW polygon in diff = hole from full containment, not through-body.
            // Full containment is detected separately in common_segment_in_intersection().
            if (warnings && !warnings->through_body.load(std::memory_order_relaxed)) {
                Polygons diff_polygons = diff(Polygons{modifier_polygon}, Polygons{polygon});
                if (diff_polygons.size() > 1) {
                    bool has_cw = std::any_of(diff_polygons.begin(), diff_polygons.end(),
                        [](const Polygon &p) { return p.is_clockwise(); });
                    if (!has_cw)
                        warnings->through_body.store(true, std::memory_order_relaxed);
                }
            }

            // Process each intersection polygon
            for (Polygon &intersection_polygon : intersection_polygons) {
                // Convert intersection_polygon to CCW to guarantee same traversal direction as perimeter
                intersection_polygon.make_counter_clockwise();

                // Search for perimeter segment in this intersection
                std::optional<SegmentData> segment = common_segment_in_intersection_fast(
                    intersection_polygon,
                    polygon,
                    warnings
                );

                if (!segment.has_value()) {
                    continue; // Segment not found
                }

                // Get left (first) point of segment
                std::optional<std::pair<Point, size_t>> left = segment_left(segment.value(), polygon);

                // Get right (last) point of segment
                std::optional<std::pair<Point, size_t>> right = segment_right(segment.value(), polygon);

                // If both boundaries found, add segment to result
                if (left.has_value() && right.has_value()) {
                    result.push_back({
                        convert_weak_modifier_type(modifier_volume->type()),  // Type: Enforced/Blocked/Neutral
                        left->first,                                          // Left point coordinates
                        left->second,                                         // Perimeter vertex index for left point
                        right->first,                                         // Right point coordinates
                        right->second                                         // Perimeter vertex index for right point
                    });
                }
            }
        }
    }

    // If no segments, return empty vector
    if (result.empty()) {
        return result;
    }

    // Insert boundary points into perimeter polygon
    // Sort points by descending arc length to avoid breaking indexing

    // 1. Parameterize polygon: calculate cumulative lengths for each vertex
    std::vector<double> cumulative_lengths(polygon.points.size() + 1);
    cumulative_lengths[0] = 0.0;
    for (size_t i = 0; i < polygon.points.size(); ++i) {
        size_t next_i = (i + 1) % polygon.points.size();
        double edge_length = (polygon.points[next_i] - polygon.points[i]).cast<double>().norm();
        cumulative_lengths[i + 1] = cumulative_lengths[i] + edge_length;
    }

    // 2. Create helper vector for sorting: {segment index, left/right point, arc length}
    struct PointToInsert {
        size_t segment_idx;  // Index in result
        bool is_left;        // true = left point, false = right point
        double arc_length;   // Arc length from perimeter start
    };
    std::vector<PointToInsert> points_to_insert;
    points_to_insert.reserve(result.size() * 2);

    for (size_t seg_idx = 0; seg_idx < result.size(); ++seg_idx) {
        const WeakModifierSegment &seg = result[seg_idx];

        // Left point
        double left_base = cumulative_lengths[seg.left_idx];
        double left_offset = (seg.left_point - polygon.points[seg.left_idx]).cast<double>().norm();
        points_to_insert.push_back({seg_idx, true, left_base + left_offset});

        // Right point
        double right_base = cumulative_lengths[seg.right_idx];
        double right_offset = (seg.right_point - polygon.points[seg.right_idx]).cast<double>().norm();
        points_to_insert.push_back({seg_idx, false, right_base + right_offset});
    }

    // 3. Sort by descending arc length (insert distant points first)
    std::sort(points_to_insert.begin(), points_to_insert.end(),
              [](const PointToInsert &a, const PointToInsert &b) {
                  return a.arc_length > b.arc_length;
              });

    // 4. Insert points in descending arc length order
    // Preparation: reserve memory for boundary points (used for refine_at_vertex)
    std::vector<std::pair<Point, int>> boundary_points;
    boundary_points.reserve(points_to_insert.size());

    std::vector<bool> segment_valid(result.size(), true);
    bool any_insertion_failed = false;

    for (const PointToInsert &pt : points_to_insert) {
        WeakModifierSegment &seg = result[pt.segment_idx];
        Point &point_coords = pt.is_left ? seg.left_point : seg.right_point;
        size_t edge_idx = pt.is_left ? seg.left_idx : seg.right_idx;

        // Insert point with tolerance check
        std::optional<std::pair<Point, size_t>> insert_result =
            insert_point_into_perimeter(point_coords, edge_idx, polygon);

        if (insert_result.has_value()) {
            // Update coordinates in result (if point coincided with existing vertex, take its coordinates)
            point_coords = insert_result->first;

            boundary_points.emplace_back(point_coords, pt.is_left ? -1 : +1);
        } else {
            // Failed to insert point - segment becomes invalid
            segment_valid[pt.segment_idx] = false;
            any_insertion_failed = true;
        }
    }

    // 5. Process insertion result: compaction (if errors) or refinement (normal case)
    if (any_insertion_failed) {
        // Critical error: boundary point not inserted (shouldn't happen in normal conditions)
        BOOST_LOG_TRIVIAL(error) << "PreciseSeam: boundary point insertion failed, performing segment compaction";

        // Remove invalid segments (array compaction)
        size_t write_pos = 0;
        for (size_t read_pos = 0; read_pos < result.size(); ++read_pos) {
            if (segment_valid[read_pos]) {
                if (write_pos != read_pos) {
                    result[write_pos] = std::move(result[read_pos]);
                }
                ++write_pos;
            }
        }
        result.resize(write_pos);
    } else {
        // 6. Insert additional points near segment boundaries through refine_at_vertex
        // boundary_points ordered by descending arc length (from distant to near)
        // Iterate polygon from end to start - matches order of points in boundary_points
        // O(N) instead of O(N×M) searching each point through entire polygon
        for (size_t poly_idx = polygon.size(), bp_idx = 0;
             poly_idx-- > 0 && bp_idx < boundary_points.size(); ) {
            if (polygon[poly_idx] == boundary_points[bp_idx].first) {
                refine_at_vertex(poly_idx, boundary_points[bp_idx].second, polygon);
                ++bp_idx;
            }
        }
    }

    // 7. Split edges in enforced zones into segments ≤ enforcer_oversampling_distance
    // Determine type pattern for each polygon edge (sequential application of hierarchy)
    std::vector<EnforcedBlockedSeamPoint> edge_types(polygon.size(), EnforcedBlockedSeamPoint::Neutral);

    // Helper lambda: search for point index in modified polygon by coordinates.
    // Linear scan is intentional — O(N×M) is acceptable for typical M ≤ 5 weak segments.
    auto find_point_index = [&](const Point &pt) -> std::optional<size_t> {
        for (size_t i = 0; i < polygon.size(); ++i) {
            if (polygon[i] == pt) return i;
        }
        return std::nullopt;
    };

    // Apply types sequentially: segments are sorted low-priority-first
    // (bottom of object tree first), so higher-priority modifiers overwrite
    // lower-priority ones via last-write-wins.
    for (const auto &segment : result) {
        // Find boundary point indices in modified polygon
        std::optional<size_t> left_idx = find_point_index(segment.left_point);
        std::optional<size_t> right_idx = find_point_index(segment.right_point);

        if (!left_idx.has_value() || !right_idx.has_value()) {
            BOOST_LOG_TRIVIAL(error) << "PreciseSeam: boundary point not found in modified polygon, skipping segment";
            continue;
        }

        // Set type for edges [left_idx, right_idx]
        for (size_t idx = left_idx.value(); ; idx = (idx + 1) % polygon.size()) {
            edge_types[idx] = segment.type;
            if (idx == right_idx.value()) break;
        }
    }

    // Split enforced edges into small segments
    const double STEP_SCALED = scale_(SeamPlacer::enforcer_oversampling_distance);

    // Collect new list of polygon points with split enforced edges
    Points new_points;
    new_points.reserve(polygon.size() * 10); // Approximate estimate

    for (size_t i = 0; i < polygon.size(); ++i) {
        size_t next_i = (i + 1) % polygon.size();
        Point p_start = polygon[i];
        Point p_end = polygon[next_i];

        // Add current vertex
        new_points.push_back(p_start);

        // Check edge type [i, next_i]
        if (edge_types[i] != EnforcedBlockedSeamPoint::Enforced) {
            continue; // Not enforced - don't split
        }

        // Calculate subdivision parameters
        Vec2d edge_vec = (p_end - p_start).cast<double>();
        double edge_length = edge_vec.norm();

        if (edge_length <= STEP_SCALED) {
            continue; // Edge too short - don't split
        }

        // Number of segments: ceil(L / S)
        size_t num_segments = static_cast<size_t>(std::ceil(edge_length / STEP_SCALED));

        // Uniform step: L / num_segments (each segment ≤ S)
        double actual_step = edge_length / num_segments;
        Vec2d step_vec = edge_vec.normalized() * actual_step;

        // Add intermediate points incrementally
        Vec2d current_pos = p_start.cast<double>();
        for (size_t j = 1; j < num_segments; ++j) {
            current_pos += step_vec;
            new_points.push_back(current_pos.cast<coord_t>());
        }
    }

    // Replace polygon points with new ones (with split enforced edges)
    polygon.points = std::move(new_points);

    return result;
}

// Apply weak modifier types to perimeter points based on segment boundaries.
// Find boundary points in refined polygon by coordinates and set types
// for all points inside each segment.
void apply_weak_modifiers_to_perimeter(
    const std::vector<WeakModifierSegment> &weak_segments,
    const Polygon &polygon,
    PrintObjectSeamData::LayerSeams &result,
    const SeamPlacerImpl::Perimeter &perimeter,
    bool &some_point_enforced,
    const std::vector<const ModelVolume*> *weak_volumes,
    size_t layer_id)
{
    // Get z-coordinate for unscaling boundary points
    const float z_coord = result.points[perimeter.start_index].position.z();
    const size_t perimeter_size = perimeter.end_index - perimeter.start_index;

    // Helper lambda: search for point index in result.points by unscaled coordinates.
    // Linear scan is intentional — O(N×M) is acceptable for typical M ≤ 5 weak segments.
    auto find_point_index = [&](const Point &pt) -> std::optional<size_t> {
        Vec2f unscaled_pt = unscale(pt).cast<float>();
        Vec3f target(unscaled_pt.x(), unscaled_pt.y(), z_coord);
        for (size_t i = perimeter.start_index; i < perimeter.end_index; ++i) {
            if (result.points[i].position == target) return i - perimeter.start_index;
        }
        return std::nullopt;
    };

    // Apply weak modifiers sequentially: sorted low-priority-first,
    // so higher-priority modifiers (higher in object tree) overwrite via last-write-wins.
    for (size_t seg_idx = 0; seg_idx < weak_segments.size(); ++seg_idx) {
        const auto &segment = weak_segments[seg_idx];

        // Find boundary point indices in result.points
        std::optional<size_t> left_idx = find_point_index(segment.left_point);
        std::optional<size_t> right_idx = find_point_index(segment.right_point);

        if (!left_idx.has_value() || !right_idx.has_value()) {
            BOOST_LOG_TRIVIAL(error) << "PreciseSeam: boundary point not found in perimeter, skipping segment";
            continue;
        }

        // Apply modifier type to range [left_idx, right_idx] with wraparound
        for (size_t idx = left_idx.value(); ; idx = (idx + 1) % perimeter_size) {
            result.points[perimeter.start_index + idx].type = segment.type;
            if (idx == right_idx.value()) break;
        }

        if (segment.type == EnforcedBlockedSeamPoint::Enforced) {
            some_point_enforced = true;
        }
    }
}

// Nudge duplicate last vertex toward previous vertex to avoid zero-length edge.
// Modifies polygon in-place if first and last vertices coincide exactly (bitwise comparison).
// If last vertex coincides with first, nudges it toward the previous vertex by a small amount:
// min(0.5 * edge_length, 0.001 mm). Logs warnings if operation cannot be performed safely.
void nudge_duplicate_vertex(Polygon &polygon)
{
    // Check minimum vertex count
    if (polygon.points.size() < 2) {
        BOOST_LOG_TRIVIAL(warning) << "nudge_duplicate_vertex: polygon has < 2 points, skipping";
        return;
    }

    // Bitwise check if first and last vertices coincide
    const Point &first = polygon.points.front();
    const Point &last = polygon.points.back();
    if (first != last) {
        return;  // No duplicate - nothing to do
    }

    // Get second-to-last vertex
    const Point &prev = polygon.points[polygon.points.size() - 2];

    // Compute distance between last and previous vertices
    coord_t edge_length_sq = (last - prev).squaredNorm();
    if (edge_length_sq == 0) {
        BOOST_LOG_TRIVIAL(warning) << "nudge_duplicate_vertex: zero distance between last two vertices";
        return;
    }

    float edge_length = std::sqrt(float(edge_length_sq));

    // Compute nudge amount: min(0.5 * edge_length, 1 micron)
    coord_t max_nudge = scale_(0.001);  // 0.001 mm = 1 micron
    float nudge_amount = std::min(0.5f * edge_length, float(max_nudge));

    // Interpolate: new_last = last + nudge_amount * (prev - last).normalized()
    float k = nudge_amount / edge_length;
    Point new_last = last + (k * (prev - last).cast<float>()).cast<coord_t>();

    // Verify that nudged vertex doesn't coincide with other vertices
    if (new_last == first || new_last == prev) {
        BOOST_LOG_TRIVIAL(warning) << "nudge_duplicate_vertex: nudged vertex coincides with another vertex, skipping";
        return;
    }

    // Apply the change
    polygon.points.back() = new_last;
}


// Restore precise seam positions that may have been modified
void restore_precise_seam_positions(std::vector<PrintObjectSeamData::LayerSeams> &layers) {
  using SeamPlacerImpl::SeamCandidate;
  using SeamPlacerImpl::Perimeter;

  tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()),
    [&layers](tbb::blocked_range<size_t> r) {
      for (size_t layer_idx = r.begin(); layer_idx < r.end(); ++layer_idx) {
        std::vector<SeamCandidate> &layer_perimeter_points = layers[layer_idx].points;
        // Iterate over perimeters (jump by end_index)
        for (size_t current = 0; current < layer_perimeter_points.size();
             current = layer_perimeter_points[current].perimeter.end_index) {
          Perimeter &perimeter = layer_perimeter_points[current].perimeter;
          if (perimeter.precise_seam_point.has_value()) {
            perimeter.final_seam_position = perimeter.precise_seam_point.value();
            perimeter.seam_index = perimeter.precise_seam_index;
          }
        }
      }
    });
}

} // namespace PreciseSeam
} // namespace Slic3r
