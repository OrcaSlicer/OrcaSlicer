#ifndef slic3r_PreciseSeam_hpp_
#define slic3r_PreciseSeam_hpp_

#include <atomic>
#include <optional>
#include <vector>
#include <unordered_map>
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "SeamPlacer.hpp"

// CURRENT STATUS:
// Strong modifiers (Center/Left/Right): only one intersection per perimeter is supported,
// since there can be only one seam. Additional intersections are ignored.
//
// Weak modifiers (Enforced/Blocked/Neutral): multiple intersections are supported,
// but none of them should pass through the model entirely. A through-body intersection
// produces multiple segments, of which only one will be processed.
//
// In both cases, a pop-up warning is shown when unsupported intersections are detected.
//
// If any modifier has a multiply-connected cross-section (e.g. a hollow shape),
// it is skipped and a corresponding notification is shown.
//
// Full containment of the perimeter within modifier is not handled.
//
// FUTURE DIRECTION:
// A lightweight algorithm is needed to detect and handle through-body intersections
// for Weak modifiers. The algorithm must not slow down the 99.9% common case.
// Possible approach: if intersection passes the diff check (no through-body),
// use the current fast algorithm. If diff check fails, fall back to a heavier
// method: compute midpoints of intersection polygon edges, then check which
// midpoints lie strictly inside the modifier (not on boundary) using
// point_in_polygon. Those edges originate from the perimeter; the rest
// originate from the modifier boundary. Collect perimeter edges into a polyline.
// Additionally, multiply-connected cross-sections could be supported instead of
// being skipped entirely (e.g. by decomposing them into simple polygons).
// For Enforced and Neutral weak modifiers, full containment of the perimeter
// within modifier could be handled (currently ignored).

namespace Slic3r {
namespace PreciseSeam {

// Import EnforcedBlockedSeamPoint from SeamPlacerImpl namespace for convenience
using SeamPlacerImpl::EnforcedBlockedSeamPoint;

// Pre-sliced modifier cache: ModelVolume pointer → per-layer Polygons.
// Built once in SeamPlacer::init(), then passed read-only into per-perimeter functions.
using ModifierSlicesCache = std::unordered_map<const ModelVolume*, std::vector<Polygons>>;

// Warning flags set during Precise Seam processing (thread-safe)
struct PreciseSeamWarnings {
    std::atomic<bool> multiple_intersections{false};  // modifier intersects perimeter in multiple separate places (strong only)
    std::atomic<bool> through_body{false};            // modifier passes through the model body entirely
    std::atomic<bool> multiply_connected{false};      // modifier has holes (multiply-connected cross-section)
    std::atomic<bool> full_containment{false};        // modifier fully contains perimeter, no intersection edges
};

// Result of finding common segment between perimeter and intersection
struct SegmentData {
    Polyline segment;                         // Points from intersection_polygon forming the segment
    std::vector<size_t> perimeter_edge_indices; // edge_index for each point in segment
};

// Result of weak modifier segment processing
struct WeakModifierSegment {
    EnforcedBlockedSeamPoint type;  // Enforced/Blocked/Neutral
    Point left_point;               // Coordinates of left (first) point of segment
    size_t left_idx;                // Perimeter vertex index for left_point
    Point right_point;              // Coordinates of right (last) point of segment
    size_t right_idx;               // Perimeter vertex index for right_point
};

// Initialize Precise Seam data by populating provided vectors and flag
// Collects precise seam modifiers and fills output parameters
// Call once during SeamPlacer::init() before gather_seam_candidates()
// Parameters:
//   strong_volumes_out - output vector for strong modifiers (CENTER/LEFT/RIGHT)
//   weak_volumes_out   - output vector for weak modifiers (ENFORCED/BLOCKED/NEUTRAL)
//   has_strong_out     - output flag indicating presence of strong modifiers
//   model_object       - model object containing volumes
void init_precise_seam_data(
    std::vector<const ModelVolume*>& strong_volumes_out,
    std::vector<const ModelVolume*>& weak_volumes_out,
    bool& has_strong_out,
    const ModelObject* model_object);

// Insert strong seam point into perimeter polygon
// Processes strong modifiers (CENTER/LEFT/RIGHT) and inserts seam point into polygon
// Parameters:
//   strong_volumes    - list of strong precise seam modifiers
//   polygon           - perimeter polygon (will be modified if point inserted)
//   layer             - current layer
//   slices_cache      - pre-sliced modifier polygons (built once in SeamPlacer::init)
// Returns:
//   Coordinates of inserted point (internal units) or std::nullopt if nothing inserted
std::optional<Point> insert_strong_seam_point(
    const std::vector<const ModelVolume*> &strong_volumes,
    Polygon &polygon,
    const Layer *layer,
    const ModifierSlicesCache &slices_cache,
    PreciseSeamWarnings* warnings = nullptr);

// Collect all weak modifier segments for a perimeter polygon
// Processes weak modifiers (ENFORCED/BLOCKED/NEUTRAL) and collects segment boundaries
// Also inserts boundary points into the perimeter polygon (sorted by descending arc length)
// Refines enforced edges by subdividing them into segments ≤ enforcer_oversampling_distance
// Parameters:
//   strong_volumes    - list of strong precise seam modifiers (for debug output header)
//   weak_volumes      - list of weak precise seam modifiers
//   polygon           - perimeter polygon (will be modified with inserted points and refined edges)
//   layer             - current layer
//   slices_cache      - pre-sliced modifier polygons (built once in SeamPlacer::init)
// Returns:
//   Ordered vector of segments with updated coordinates (same order as weak_volumes list)
std::vector<WeakModifierSegment> collect_weak_modifier_segments(
    const std::vector<const ModelVolume*> &strong_volumes,
    const std::vector<const ModelVolume*> &weak_volumes,
    Polygon &polygon,
    const Layer *layer,
    const ModifierSlicesCache &slices_cache,
    PreciseSeamWarnings* warnings = nullptr);

// Apply weak modifier types to perimeter points based on segment boundaries
// Finds boundary points in refined polygon and sets types for points within segments
// Parameters:
//   weak_segments       - segments with boundary coordinates and types
//   polygon             - refined polygon (after point insertion)
//   result              - layer seams data to modify
//   perimeter           - perimeter info (start/end indices)
//   some_point_enforced - flag to update if Enforced points are set
//   weak_volumes        - list of weak modifiers (for debug output, optional)
//   layer_id            - layer ID (for debug output, optional)
void apply_weak_modifiers_to_perimeter(
    const std::vector<WeakModifierSegment> &weak_segments,
    const Polygon &polygon,
    PrintObjectSeamData::LayerSeams &result,
    const SeamPlacerImpl::Perimeter &perimeter,
    bool &some_point_enforced,
    const std::vector<const ModelVolume*> *weak_volumes = nullptr,
    size_t layer_id = 0);

// Nudge duplicate last vertex toward previous vertex to avoid zero-length edge
// Modifies polygon in-place if first and last vertices coincide
void nudge_duplicate_vertex(Polygon &polygon);

// Restore precise seam positions that may have been modified by alignment
// Iterates through all perimeters and restores precise_seam_point positions
void restore_precise_seam_positions(std::vector<PrintObjectSeamData::LayerSeams> &layers);

} // namespace PreciseSeam
} // namespace Slic3r

#endif // slic3r_PreciseSeam_hpp_
