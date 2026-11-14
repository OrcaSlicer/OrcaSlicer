#ifndef slic3r_PreciseSeam_hpp_
#define slic3r_PreciseSeam_hpp_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>
#include <utility>
#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Model.hpp"
#include "SeamPlacer.hpp"

// Precise Seam: helper volumes that decide where the seam goes on external perimeters.
// Design: docs/HLSD/precise-seam.md

namespace Slic3r {
namespace PreciseSeam {

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
    std::atomic<unsigned> full_containment{0};       // Skipped for a perimeter fully inside: Center/Left/Right.
    std::atomic<unsigned> failed_types{0};           // Types with at least one discarded fragment.
    std::atomic<size_t> failed_fragments{0}; // Total discarded fragments, for the log summary.
    // Fragments saved by the rare-case fallback or accepted as contacts; log only, no user warning.
    // Clipper is deterministic, so a prismatic model can repeat the same case on every layer.
    std::atomic<size_t> recovered_fragments{0};
    // Fuzzy skin, log only: points carried onto the noisy loop without the line through their surface point, and weak
    // zones whose carried boundaries came out in the reverse order and were reduced to one point.
    std::atomic<size_t> approximate_transfers{0};
    std::atomic<size_t> reversed_zones{0};

    // Per-modifier flags for the "had no effect" and "covers an entire perimeter" warnings. Modifiers are
    // registered before the parallel phase, so workers only set flags; unregistered ones (e.g. in tests)
    // are not tracked.
    struct ModifierUsage {
        std::atomic<bool> checked{false};       // Extracted on at least one perimeter.
        std::atomic<bool> reached{false};       // Gave a segment, full containment or a discarded fragment.
        std::atomic<bool> fully_blocked{false}; // Seam Blocked whole zone in effect on a perimeter left Blocked.
    };
    std::unordered_map<const ModelVolume*, ModifierUsage> modifier_usage;

    // Bit of a Precise Seam type in the masks above, in menu order (Center is bit 0).
    static unsigned type_bit(ModelVolumeType type)
    {
        assert(is_precise_seam(type));
        return 1u << (int(type) - int(ModelVolumeType::PRECISE_SEAM_CENTER));
    }
    // Load before writing: most calls find the bit or flag already set, so shared cache lines stay clean.
    static void mark(std::atomic<unsigned> &mask, ModelVolumeType type)
    {
        const unsigned bit = type_bit(type);
        if ((mask.load(std::memory_order_relaxed) & bit) == 0)
            mask.fetch_or(bit, std::memory_order_relaxed);
    }
    static void set(std::atomic<bool> &flag)
    {
        if (!flag.load(std::memory_order_relaxed))
            flag.store(true, std::memory_order_relaxed);
    }
};

// Optional caller identity for concise diagnostics when an intersection is discarded.
struct ExtractionContext {
    const Layer *layer = nullptr;
    const ModelVolume *modifier = nullptr;
    PreciseSeamWarnings *warnings = nullptr;
};

namespace detail { struct SurfaceLine; }

// Scaled distance from the perimeter centerline to the model surface the user sees: half the line width,
// less the object's XY compensation for contours or holes (positive towards air) and a small margin. Zero when
// the surface practically coincides with the centerline; extraction then clips the centerline itself.
coord_t surface_shift(double line_width, double compensation);

// A shift that changes along the perimeter: one value per vertex and, on an edge where it does not change
// linearly between its vertices, values at interior points as (parameter, shift) in increasing parameter order.
struct ShiftProfile {
    std::vector<coord_t> vertices;
    std::vector<std::vector<std::pair<double, coord_t>>> interior; // One list per edge.
    bool empty() const { return vertices.empty(); }
};

// Borrows the source polygon; use only until insertion/refinement changes that polygon.
struct PreparedPerimeter {
    // The smooth centerline modifiers are evaluated on.
    const Polygon &polygon;
    // With fuzzy skin, the noisy loop the results are inserted into, made from `polygon`; null when they are
    // inserted into `polygon` itself. Points are carried onto it within `window` of the smooth line.
    const Polygon *target = nullptr;
    coord_t window = 0;
    BoundingBox bounds;
    Polyline line;
    bool valid = false;
    // Largest distance to the reconstructed surface and its side: air lies inside the polygon of a hole.
    coord_t shift = 0;
    bool hole = false;
    // The shift at every point when it changes along the perimeter; empty when it is `shift` everywhere.
    ShiftProfile profile;

    explicit PreparedPerimeter(const Polygon &perimeter, coord_t surface_shift = 0, bool is_hole = false);
    // A profile whose largest shift is within the snapping radius keeps the centerline, as a constant one does.
    PreparedPerimeter(const Polygon &perimeter, ShiftProfile shift_profile, bool is_hole);
    ~PreparedPerimeter();
    // Sets the noisy loop made from `polygon` by fuzzy skin and the window within which its points are looked for
    // around `polygon`: an estimate from the fuzzy skin settings, not a measured largest distance. An unusable loop
    // (as the constructor checks) makes the preparation invalid.
    void set_target(const Polygon &noisy, coord_t noise_window);
    // The polygon insertion changes.
    const Polygon &insertion() const { return target != nullptr ? *target : polygon; }
    // Built on the first request, so perimeters that no modifier approaches never pay for it. Internal to
    // extraction.
    const detail::SurfaceLine &surface() const;
    // The same for the edges of the noisy loop; requires a target.
    const AABBTreeLines::LinesDistancer<Line> &target_lines() const;

private:
    mutable std::unique_ptr<detail::SurfaceLine> m_surface;
    mutable std::unique_ptr<AABBTreeLines::LinesDistancer<Line>> m_target_lines;
};

// What measuring the shift along one perimeter needs. Lengths in mm.
struct ShiftMeasurement {
    // Boundary of the layer's islands as slicing stored them, before elephant foot compensation.
    const AABBTreeLines::LinesDistancer<Line> &outline;
    double line_width;           // Of the external perimeter; half of it is the nominal depth of the centerline.
    double compensation;         // XY compensation of this kind of wall, positive towards air.
    double outline_compensation; // The part of `compensation` the outline already contains.
    // Depths of the centerline below the outline that belong to the wall's own surface; elsewhere (a wall thinner
    // than expected, a boundary between regions) the nominal shift applies.
    double min_depth;
    double max_depth;
};

// The shift at every vertex of a perimeter, and at interior points of edges longer than the sampling step where it
// does not change linearly along them: the measured depth of the centerline below the outline, less the
// compensation the outline contains and the margin, never negative. The depth is the offset to the nearest point of
// the outline projected on the normal of the edge (the larger of the two at a vertex), so at an inner corner, whose
// nearest outline point is the corner, it is still the distance to each side. Returns an empty profile when every
// value equals the nominal shift of surface_shift() within the snapping radius, so that walls at their nominal depth
// keep the constant shift exactly.
ShiftProfile measure_shift_profile(const Polygon &perimeter, bool hole, const ShiftMeasurement &measurement);

// A vertex is represented by its outgoing edge and parameter zero, including vertex 0.
struct PerimeterPosition {
    size_t edge_index;
    double parameter;
};

// Prepared against the immutable perimeter, before insertion shifts its edge indices.
struct StrongSeamTarget {
    Point point;
    size_t edge_index;
    // The surface point paired with `point`: the segment end's for Left and Right; for Center, worked out only when
    // the point is carried onto a noisy loop.
    std::optional<Point> surface = std::nullopt;
};

struct PerimeterSegment {
    // A single point with no edges when the surface was reached only around a convex vertex.
    Polyline polyline;
    // One bound source edge per polyline interval.
    std::vector<size_t> edge_indices;
    PerimeterPosition begin;
    PerimeterPosition end;
    // The reconstructed surface points paired with the polyline's ends, kept through merging; the ends themselves on
    // the centerline path. A zero-length segment has the two ends of its piece of arc.
    Point surface_first;
    Point surface_last;
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
    SeamPlacerImpl::EnforcedBlockedSeamPoint type;  // Enforced/Blocked/Neutral
    Point left_point;               // Coordinates of left (first) point of segment
    PerimeterPosition left_position; // Position on the source perimeter before insertion/refinement.
    Point right_point;              // Coordinates of right (last) point of segment
    PerimeterPosition right_position; // Retained provenance, not an index into the modified polygon.
    // Full containment of a weak modifier: the zone is the whole perimeter, without boundaries
    // (the points and positions above are unused and nothing is inserted for it).
    bool whole_perimeter = false;
    const ModelVolume *modifier = nullptr; // Source modifier, for the warnings.
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
// sets some_point_enforced when an Enforced zone applies. When the whole perimeter ends up Blocked and
// the highest whole-perimeter zone is Blocked, flags that zone's modifier for the warning.
void apply_weak_modifiers_to_perimeter(
    const std::vector<WeakModifierSegment> &weak_segments,
    PrintObjectSeamData::LayerSeams &result,
    const SeamPlacerImpl::Perimeter &perimeter,
    bool &some_point_enforced,
    PreciseSeamWarnings *warnings = nullptr);

// Restore precise seam positions that may have been modified by alignment
// Iterates through all perimeters and restores precise_seam_point positions
void restore_precise_seam_positions(std::vector<PrintObjectSeamData::LayerSeams> &layers);

} // namespace PreciseSeam
} // namespace Slic3r

#endif // slic3r_PreciseSeam_hpp_
