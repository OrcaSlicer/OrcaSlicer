// Test file for Arachne wall generation
// 
// Tests for duplicate/coinciding wall segment detection in Arachne output.
// 
// This test reproduces an issue where Arachne generates duplicate extrusion
// segments at certain min_bead_width settings. The test uses a polygon with
// an outer rectangle (0,0)-(20,20) and an inner cutout (0.5,0.5)-(19.5,19.5).
//
// With precise_outer_wall enabled and min_bead_width at 50% (0.20mm), Arachne
// generates two separate closed contours that share a coinciding edge at y=19.75.
// At 60% (0.24mm), Arachne handles this differently and avoids the duplicate.
//
// Parameters are based on "0.28mm Extra Draft @BBL X1C" profile with:
// - 0.4mm nozzle, 0.28mm layer height
// - outer_wall_line_width: 0.42mm, inner_wall_line_width: 0.45mm
// - wall_loops: 2, precise_outer_wall: enabled

#include <catch2/catch_all.hpp>

#include "libslic3r/Arachne/WallToolPaths.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/Arachne/BeadingStrategy/BeadingStrategyFactory.hpp"
#include "libslic3r/Arachne/BeadingStrategy/BeadingStrategy.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Point.hpp"

#include <algorithm>
#include <cmath>

using namespace Slic3r;
using namespace Slic3r::Arachne;

namespace {

// Represents a segment with direction-independent comparison
struct Segment {
    Point from;
    Point to;
    size_t inset_idx;
    
    // Normalize segment so that the "smaller" point comes first
    // This allows direction-independent comparison
    Segment normalized() const {
        if (from < to || (from.x() == to.x() && from.y() < to.y())) {
            return *this;
        }
        return {to, from, inset_idx};
    }
    
    bool operator<(const Segment& other) const {
        auto a = normalized();
        auto b = other.normalized();
        if (a.inset_idx != b.inset_idx) return a.inset_idx < b.inset_idx;
        if (a.from != b.from) return a.from < b.from;
        return a.to < b.to;
    }
    
    bool operator==(const Segment& other) const {
        auto a = normalized();
        auto b = other.normalized();
        return a.inset_idx == b.inset_idx && a.from == b.from && a.to == b.to;
    }
};

// Check if two points are approximately equal within tolerance
bool points_approx_equal(const Point& a, const Point& b, coord_t tolerance) {
    return std::abs(a.x() - b.x()) <= tolerance && std::abs(a.y() - b.y()) <= tolerance;
}

// Check if two segments are approximately equal (direction-independent)
bool segments_approx_equal(const Segment& a, const Segment& b, coord_t tolerance) {
    if (a.inset_idx != b.inset_idx) return false;
    
    // Check both directions
    bool same_dir = points_approx_equal(a.from, b.from, tolerance) && 
                    points_approx_equal(a.to, b.to, tolerance);
    bool reverse_dir = points_approx_equal(a.from, b.to, tolerance) && 
                       points_approx_equal(a.to, b.from, tolerance);
    return same_dir || reverse_dir;
}

// Extract all segments from toolpaths (all inset indices)
std::vector<Segment> extract_all_segments(const std::vector<VariableWidthLines>& toolpaths) {
    std::vector<Segment> segments;
    
    for (const auto& inset : toolpaths) {
        for (const auto& line : inset) {
            if (line.junctions.size() < 2) continue;
            
            for (size_t i = 0; i + 1 < line.junctions.size(); ++i) {
                segments.push_back({
                    line.junctions[i].p,
                    line.junctions[i + 1].p,
                    line.inset_idx
                });
            }
        }
    }
    
    return segments;
}

// Find duplicate segments within tolerance
std::vector<std::pair<Segment, Segment>> find_duplicate_segments(
    const std::vector<Segment>& segments, 
    coord_t tolerance) 
{
    std::vector<std::pair<Segment, Segment>> duplicates;
    
    for (size_t i = 0; i < segments.size(); ++i) {
        for (size_t j = i + 1; j < segments.size(); ++j) {
            if (segments_approx_equal(segments[i], segments[j], tolerance)) {
                duplicates.emplace_back(segments[i], segments[j]);
            }
        }
    }
    
    return duplicates;
}

// Create params matching "0.28mm Extra Draft @BBL X1C" profile
WallToolPathsParams make_bbl_x1c_028_params(int min_bead_width_percent) {
    constexpr double nozzle_diameter = 0.4;
    
    WallToolPathsParams params;
    params.min_bead_width = float(min_bead_width_percent / 100.0 * nozzle_diameter);
    params.min_feature_size = float(0.25 * nozzle_diameter);
    params.wall_transition_filter_deviation = float(0.25 * nozzle_diameter);
    params.wall_transition_length = float(1.0 * nozzle_diameter);
    params.wall_transition_angle = 10.0f;
    params.wall_distribution_count = 1;
    params.min_length_factor = 0.5f;
    params.is_top_or_bottom_layer = false;
    return params;
}

// Run Arachne wall generation test with specified min_bead_width percentage
// Returns the number of duplicate segments found
size_t run_arachne_test(int min_bead_width_percent) {
    constexpr double layer_height = 0.28;
    constexpr double ext_perimeter_width_mm = 0.42;
    constexpr double perimeter_width_mm = 0.45;
    
    // Spacing calculation: width - height * (1 - PI/4)
    constexpr double spacing_factor = 1.0 - 0.25 * M_PI;
    double ext_perimeter_spacing_mm = ext_perimeter_width_mm - layer_height * spacing_factor;
    double perimeter_spacing_mm = perimeter_width_mm - layer_height * spacing_factor;
    
    coord_t ext_perimeter_width = scaled<coord_t>(ext_perimeter_width_mm);
    coord_t ext_perimeter_spacing = scaled<coord_t>(ext_perimeter_spacing_mm);
    coord_t perimeter_spacing = scaled<coord_t>(perimeter_spacing_mm);
    
    coord_t bead_width_0 = ext_perimeter_spacing;
    coord_t bead_width_x = perimeter_spacing;
    size_t  inset_count  = 2;
    
    // precise_outer_wall enabled
    float precise_offset = -float(ext_perimeter_width - ext_perimeter_spacing);
    coord_t wall_0_inset = -coord_t(ext_perimeter_width / 2 - ext_perimeter_spacing / 2);
    
    auto params = make_bbl_x1c_028_params(min_bead_width_percent);
    
    // Test polygon: outer rectangle with inner cutout creating 0.5mm frame
    Polygon outer_raw;
    outer_raw.points.emplace_back(Point::new_scale(0.0, 0.0));
    outer_raw.points.emplace_back(Point::new_scale(20.0, 0.0));
    outer_raw.points.emplace_back(Point::new_scale(20.0, 20.0));
    outer_raw.points.emplace_back(Point::new_scale(0.0, 20.0));
    
    Polygon inner_raw;
    inner_raw.points.emplace_back(Point::new_scale(0.5, 0.5));
    inner_raw.points.emplace_back(Point::new_scale(0.5, 19.5));
    inner_raw.points.emplace_back(Point::new_scale(19.5, 19.5));
    inner_raw.points.emplace_back(Point::new_scale(19.5, 0.5));
    
    ExPolygon input_expolygon;
    input_expolygon.contour = outer_raw;
    input_expolygon.holes.push_back(inner_raw);
    
    ExPolygons offset_result = offset_ex(input_expolygon, precise_offset);
    Polygons outline;
    for (const auto& expoly : offset_result) {
        outline.push_back(expoly.contour);
        for (const auto& hole : expoly.holes) {
            outline.push_back(hole);
        }
    }
    
    WallToolPaths wallToolPaths(outline, bead_width_0, bead_width_x, 
                                 inset_count, wall_0_inset, 
                                 layer_height, params);
    auto toolpaths = wallToolPaths.getToolPaths();
    
    auto all_segments = extract_all_segments(toolpaths);
    auto duplicates = find_duplicate_segments(all_segments, scaled<coord_t>(0.1));
    
    return duplicates.size();
}

} // anonymous namespace

TEST_CASE("Arachne wall generation - 50% min_bead_width", "[Arachne]") {
    size_t duplicates = run_arachne_test(50);
    REQUIRE(duplicates == 0);
}

TEST_CASE("Arachne wall generation - 60% min_bead_width", "[Arachne]") {
    size_t duplicates = run_arachne_test(60);
    REQUIRE(duplicates == 0);
}

// Regression test for #14376 ("Fuzzy skin artifacting" — a surface bulge at a fixed height).
//
// PR #14031 changed WideningBeadingStrategy::compute() to take the thin-wall single-bead
// branch whenever thickness < getTransitionThickness(1). That branch emits a single bead at
// the full wall thickness and ignores the requested bead_count. When the skeletal graph asks
// for 2 beads at a thickness inside the 1<->2 transition band (between the inner wall width and
// getTransitionThickness(1)), the request was collapsed into one over-wide bead — an
// over-extruded line that shows up as a bulge on curved surfaces at a deterministic height.
//
// Profile mirrors the reporter's project ("0.20mm Standard @BBL X1C", 0.4mm nozzle):
//   outer 0.42mm / inner 0.45mm, min_bead_width 85% (0.34mm), 2 walls (max_bead_count 4).
// For these numbers wall_split_middle_threshold = 2*0.34/0.42 - 1 = 0.619, so
// getTransitionThickness(1) = (1 + 0.619) * 0.42 = 0.68mm. A 0.5mm-thick wall therefore sits
// in the transition band: alpha produced 2 beads here, beta collapses it to 1 fat bead.
TEST_CASE("Arachne widening keeps two beads in transition band (#14376)", "[Arachne]") {
    using namespace Slic3r::Arachne;

    // Widths in mm; the scaled coord_t values and the thresholds below are both derived from
    // these so a width change cannot silently desync the transition-band math.
    const double outer_mm = 0.42, inner_mm = 0.45, min_bead_mm = 0.34; // min_bead = 85% of 0.4mm nozzle

    const coord_t outer_width = scaled<coord_t>(outer_mm);
    const coord_t inner_width = scaled<coord_t>(inner_mm);
    const coord_t min_bead_width = scaled<coord_t>(min_bead_mm);
    const coord_t min_feature_size = scaled<coord_t>(0.10); // 25% of 0.4mm nozzle
    const coord_t transition_length = scaled<coord_t>(0.40);
    const coord_t max_bead_count = 4; // 2 * wall_loops

    // Same derivation as WallToolPaths.cpp.
    const double split_middle_threshold = std::clamp(2.0 * min_bead_mm / outer_mm - 1.0, 0.01, 0.99);
    const double add_middle_threshold = std::clamp(min_bead_mm / inner_mm, 0.01, 0.99);

    auto strategy = BeadingStrategyFactory::makeStrategy(
        outer_width, inner_width, transition_length,
        /*transitioning_angle*/ float(M_PI / 4.0), /*print_thin_walls*/ true,
        min_bead_width, min_feature_size,
        split_middle_threshold, add_middle_threshold,
        max_bead_count, /*outer_wall_offset*/ 0, /*inward_distributed_center_wall_count*/ 1);

    // A wall thickness inside the 1<->2 bead transition band (inner_width < t < transition).
    const coord_t thickness = scaled<coord_t>(0.50);
    REQUIRE(thickness > inner_width);
    REQUIRE(thickness < strategy->getTransitionThickness(1));

    // When the graph requests 2 beads, the strategy must produce 2 beads — not collapse them
    // into a single full-thickness (bulge) bead.
    const BeadingStrategy::Beading beading = strategy->compute(thickness, 2);
    REQUIRE(beading.bead_widths.size() == 2);

    // And neither bead may be over-wide: a single collapsed bead would be ~0.5mm (the full
    // thickness), well above the configured wall widths.
    for (const coord_t w : beading.bead_widths)
        CHECK(w <= inner_width);
}
