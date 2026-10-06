#ifndef slic3r_PreciseSeam_hpp_
#define slic3r_PreciseSeam_hpp_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <optional>
#include <vector>
#include <unordered_map>
#include <utility>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Model.hpp"
#include "SeamPlacer.hpp"

// Precise Seam: helper volumes that decide where the seam goes on external perimeters.
// Design: docs/HLSD/precise-seam.md

namespace Slic3r {
namespace PreciseSeam {

// Import EnforcedBlockedSeamPoint from SeamPlacerImpl namespace for convenience
using SeamPlacerImpl::EnforcedBlockedSeamPoint;

// Geometry and its exterior bounds are prepared together, then treated as read-only.
struct ModifierRegion {
    ExPolygon polygon;
    BoundingBox bounds;

    explicit ModifierRegion(ExPolygon region)
        : polygon(std::move(region)), bounds(polygon.contour.points) {}
};

using ModifierRegions = std::vector<ModifierRegion>;
using ModifierSlices = std::vector<ModifierRegions>;
// Per-volume slices with cached bounds, shared read-only by both modifier kinds.
using ModifierRegionsCache = std::unordered_map<const ModelVolume*, ModifierSlices>;

// Move sliced geometry into the cache without detaching holes or changing layer indices.
ModifierRegions prepare_modifier_regions(ExPolygons regions);
ModifierSlices prepare_modifier_slices(std::vector<ExPolygons> slices);

// Bound diagnostic volume only; every failed or recovered fragment is still counted and handled.
// The same limit applies separately to failure and recovery markers.
inline constexpr size_t failed_fragment_log_limit = 10;

// Shared by all layers and objects in one SeamPlacer::init(); a new pass starts fresh.
struct PreciseSeamWarnings {
    // Masks of the Precise Seam types that caused each warning reason, one bit per type (type_bit()).
    // The user warning lists the types instead of naming modifiers.
    std::atomic<unsigned> multiple_intersections{0}; // Center/Left/Right with several segments on a perimeter.
    std::atomic<unsigned> full_containment{0};       // Skipped for a perimeter fully inside: Center/Left/Right, Blocked.
    std::atomic<unsigned> failed_types{0};           // Types with at least one discarded fragment.
    std::atomic<size_t> failed_fragments{0}; // Total discarded fragments, for the log summary.
    // Fragments saved by the rare-case fallback or accepted as contacts; log only, no user warning.
    // Clipper is deterministic, so a prismatic model can repeat the same case on every layer.
    std::atomic<size_t> recovered_fragments{0};

    // Per-modifier flags for the "had no effect" warning. Modifiers are registered before the parallel
    // phase, so workers only set flags; unregistered ones (e.g. in tests) are not tracked.
    struct ModifierUsage {
        std::atomic<bool> checked{false}; // Extracted on at least one perimeter.
        std::atomic<bool> reached{false}; // Gave a segment, full containment or a discarded fragment.
    };
    std::unordered_map<const ModelVolume*, ModifierUsage> modifier_usage;

    // Bit of a Precise Seam type in the masks above, in menu order (Center is bit 0).
    static unsigned type_bit(ModelVolumeType type)
    {
        assert(is_precise_seam(type));
        return 1u << (int(type) - int(ModelVolumeType::PRECISE_SEAM_CENTER));
    }
    // Load before fetch_or: most calls find the bit already set, so shared cache lines stay clean.
    static void mark(std::atomic<unsigned> &mask, ModelVolumeType type)
    {
        const unsigned bit = type_bit(type);
        if ((mask.load(std::memory_order_relaxed) & bit) == 0)
            mask.fetch_or(bit, std::memory_order_relaxed);
    }
};

// Optional caller identity for concise diagnostics when an intersection is discarded.
struct ExtractionContext {
    const Layer *layer = nullptr;
    const ModelVolume *modifier = nullptr;
    PreciseSeamWarnings *warnings = nullptr;
};

// Borrows the source polygon; use only until insertion/refinement changes that polygon.
struct PreparedPerimeter {
    const Polygon &polygon;
    BoundingBox bounds;
    Polyline line;
    bool valid = false;

    explicit PreparedPerimeter(const Polygon &perimeter);
};

// A vertex is represented by its outgoing edge and parameter zero, including vertex 0.
struct PerimeterPosition {
    size_t edge_index;
    double parameter;
};

// Prepared against the immutable perimeter, before insertion shifts its edge indices.
struct StrongSeamTarget {
    Point point;
    size_t edge_index;
};

struct PerimeterSegment {
    Polyline polyline;
    // One bound source edge per polyline interval.
    std::vector<size_t> edge_indices;
    PerimeterPosition begin;
    PerimeterPosition end;
    std::optional<StrongSeamTarget> strong_target; // Absent for weak modifiers and full containment.
    // Scaled arc length, calculated only for Center or comparison of multiple strong segments.
    double length = 0.; // Zero means unmeasured for weak, full containment, and a single Left/Right segment.
};

struct SegmentExtraction {
    std::vector<PerimeterSegment> segments;
    bool full_containment = false;
    bool valid = true; // Invalid perimeter input; discarded fragments do not invalidate other segments.
    // Clipped fragments whose binding failed; one segment may consist of several fragments.
    size_t discarded_fragments = 0; // Failed bindings are ignored, with a warning and diagnostic marker.
};

// Clips a prepared, unchanged perimeter (>= 3 vertices, no consecutive duplicates, either direction)
// against a modifier's regions and returns its segments; strong targets are prepared only where needed.
SegmentExtraction extract_perimeter_segments(const PreparedPerimeter &prepared, const ModifierRegions &modifier,
                                             ModelVolumeType mode, const ExtractionContext &context = {});

// Result of weak modifier segment processing
struct WeakModifierSegment {
    EnforcedBlockedSeamPoint type;  // Enforced/Blocked/Neutral
    Point left_point;               // Coordinates of left (first) point of segment
    PerimeterPosition left_position; // Position on the source perimeter before insertion/refinement.
    Point right_point;              // Coordinates of right (last) point of segment
    PerimeterPosition right_position; // Retained provenance, not an index into the modified polygon.
    // Full containment of an Enforced or Neutral modifier: the zone is the whole perimeter, without
    // boundaries (the points and positions above are unused and nothing is inserted for it).
    bool whole_perimeter = false;
};

// Collects the object's Precise Seam volumes: strong ones in priority order, weak ones in application
// order. Call once per object in SeamPlacer::init() before gathering candidates.
void init_precise_seam_data(
    std::vector<const ModelVolume*>& strong_volumes_out,
    std::vector<const ModelVolume*>& weak_volumes_out,
    bool& has_strong_out,
    const ModelObject* model_object);

// Inserts the seam point of the first strong modifier with a usable segment on this perimeter and returns
// it, or nullopt. `prepared` must describe `polygon` before any change.
std::optional<Point> insert_strong_seam_point(
    const std::vector<const ModelVolume*> &strong_volumes,
    Polygon &polygon,
    const PreparedPerimeter &prepared,
    const Layer *layer,
    const ModifierRegionsCache &slices_cache,
    PreciseSeamWarnings* warnings = nullptr);

// Collects weak zones, inserts their boundaries into `polygon` and subdivides enforced edges. Pass
// modifiers lowest priority first; `prepared` must describe the unchanged `polygon`.
std::vector<WeakModifierSegment> collect_weak_modifier_segments(
    const std::vector<const ModelVolume*> &weak_volumes,
    Polygon &polygon,
    const PreparedPerimeter &prepared,
    const Layer *layer,
    const ModifierRegionsCache &slices_cache,
    PreciseSeamWarnings* warnings = nullptr);

// Retypes the candidates inside each zone in the given order (pass zones lowest priority first);
// sets some_point_enforced when an Enforced zone applies.
void apply_weak_modifiers_to_perimeter(
    const std::vector<WeakModifierSegment> &weak_segments,
    PrintObjectSeamData::LayerSeams &result,
    const SeamPlacerImpl::Perimeter &perimeter,
    bool &some_point_enforced);

// Restore precise seam positions that may have been modified by alignment
// Iterates through all perimeters and restores precise_seam_point positions
void restore_precise_seam_positions(std::vector<PrintObjectSeamData::LayerSeams> &layers);

} // namespace PreciseSeam
} // namespace Slic3r

#endif // slic3r_PreciseSeam_hpp_
