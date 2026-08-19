#include <catch2/catch_all.hpp>

#include <cmath>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/SafeParkPosition.hpp"
#include "libslic3r/Magma/MagmaGeometry.hpp"
#include "libslic3r/Magma/MagmaPatterns.hpp"
#include "libslic3r/Magma/MagmaResolved.hpp"
#include "libslic3r/Magma/MagmaTriangleCell.hpp"
#include "libslic3r/Magma/MagmaTubeMap.hpp"
#include "libslic3r/Magma/MagmaTubeSolver.hpp"

using namespace Slic3r;
using namespace Slic3r::magma;

// Regression tests for the audit bugs whose failure mode is SILENT ARITHMETIC DRIFT -- a number
// that goes quietly wrong while every call site still compiles and every comment still claims
// it is correct. That was the audit's recurring shape, and it is what a test can catch that
// review demonstrably did not.
//
// Deliberately not covered here: the validate() checks for an out-of-range injection filament
// and for Klipper + plunge. Both are real and both were severe, but a test for them asserts an
// if-statement exists -- and an if-statement that gets deleted is a visible deletion, not drift.
// They would cost a Print and a mesh apiece to say so.

namespace {

struct PatternCase {
    const char   *name;
    InfillPattern pattern;
    // cos(pi/n) for a regular n-gon: how much of the circle the nozzle must cover is usable
    // tube. Triangle 1/2, square 1/sqrt2, hexagon sqrt3/2.
    double        bore_over_opening;
};

const std::vector<PatternCase> &all_patterns()
{
    static const std::vector<PatternCase> cases = {
        { "MagmaTriangle",    ipMagmaTriangle,    0.5                  },
        { "MagmaRectilinear", ipMagmaRectilinear, 1.0 / std::sqrt(2.0) },
        { "MagmaHoneycomb",   ipMagmaHoneycomb,   std::sqrt(3.0) / 2.0 },
        { "MagmaTriHex",      ipMagmaTriHex,      std::sqrt(3.0) / 2.0 },
    };
    return cases;
}

// A classic E3D 0.6mm nozzle: 1.70mm flat, 30 degree cone.
constexpr double REF_FLAT = 1.70, REF_CONE = 30.0, REF_NOZZLE = 0.60;
constexpr double REF_LINE_WIDTH = 0.42, REF_IMMERSION = 0.60;
constexpr double REF_MIN_SEAL = 0.10, REF_PLUNGE = 0.05;

// Resolve through the SAME entry point MagmaTubeMap::build, Print::validate and PresetHints
// use. A test against a reimplementation of the chain would pass while the shipped one drifted,
// which is exactly how the drift bugs survived review.
MagmaResolved resolve_ref(InfillPattern pattern)
{
    PrintRegionConfig region;
    PrintObjectConfig object;
    PrintConfig       printer;

    printer.nozzle_diameter.values = { REF_NOZZLE };

    region.sparse_infill_pattern.value      = pattern;
    region.sparse_infill_filament.value     = 1;
    region.dual_infill_enabled.value        = false;
    region.sparse_infill_line_width.value   = REF_LINE_WIDTH;
    region.sparse_infill_line_width.percent = false;

    object.magma_tube_width_mode.value        = MagmaTubeWidthMode::Auto;
    object.magma_nozzle_outer_diameter.value  = REF_FLAT;
    object.magma_nozzle_cone_half_angle.value = REF_CONE;
    object.magma_max_immersion.value          = REF_IMMERSION;
    object.magma_auto_slam_press.value        = REF_MIN_SEAL;
    object.magma_injection_plunge.value       = true;
    object.magma_injection_plunge_depth.value = REF_PLUNGE;

    MagmaResolved m;
    REQUIRE(resolve_magma(region, object, printer, m));
    return m;
}

} // namespace

TEST_CASE("Magma: auto sizing spends the immersion budget exactly, for every pattern",
          "[Magma][resolved]")
{
    // The audit's own summary of what it needed: "a single test asserting the opening/slam the
    // G-code uses equals what validate predicts, run per pattern, would have caught H2, C4, H6
    // and the three drift bugs found by hand."
    //
    // In Auto mode the opening is set by the NOZZLE and the budget, not by the lattice, so it is
    // identical across all four patterns -- only what fits inside it is pattern-specific. A
    // change that made the opening pattern-dependent would break the seal model silently.
    const double seal_budget = REF_IMMERSION - REF_PLUNGE;
    const double golden_opening =
        REF_FLAT + 2.0 * seal_budget * std::tan(REF_CONE * MAGMA_DEG2RAD) - MAGMA_SEAL_MARGIN;
    REQUIRE(golden_opening == Catch::Approx(2.2350853).epsilon(1e-6));

    for (const PatternCase &pc : all_patterns()) {
        const MagmaResolved m = resolve_ref(pc.pattern);
        INFO(pc.name);

        CHECK(m.line_width       == Catch::Approx(REF_LINE_WIDTH));
        CHECK(m.opening_diameter == Catch::Approx(golden_opening).epsilon(1e-9));
        CHECK(m.bore_diameter
              == Catch::Approx(m.opening_diameter * pc.bore_over_opening).epsilon(1e-9));

        // The seal spends the budget less the plunge reservation, the plunge spends the
        // reservation, and together they are the budget. Exactly -- not "within".
        CHECK(m.seal_depth    == Catch::Approx(seal_budget).epsilon(1e-9));
        CHECK(m.plunge_depth  == Catch::Approx(REF_PLUNGE).epsilon(1e-9));
        CHECK(m.total_depth() == Catch::Approx(REF_IMMERSION).epsilon(1e-9));

        // And the point of sizing it that way: this nozzle seals this tube.
        CHECK(m.seals());
    }
}

TEST_CASE("Magma: the park retract is an absolute target under absolute E", "[Magma][regression]")
{
    // C0: the park path emitted a raw "G1 E-2.0" delta regardless of E mode. Under ABSOLUTE E
    // that means "move the E axis TO -2", so at E=850 it is an ~852mm retraction that pulls the
    // filament clear of the hotend and past the drive gear. 14 shipped machine profiles run
    // absolute E, and this path is shared with ooze prevention, so it fires on prints that never
    // touch Magma.
    const double e_before = 850.0, extra = 2.0;

    CHECK(park_extra_retract_e  (true,  e_before, extra) == Catch::Approx(-2.0));
    CHECK(park_extra_unretract_e(true,  e_before, extra) == Catch::Approx( 2.0));
    // Absolute: a target near where the extruder already is, never a small negative number.
    CHECK(park_extra_retract_e  (false, e_before, extra) == Catch::Approx(848.0));
    CHECK(park_extra_unretract_e(false, e_before, extra) == Catch::Approx(850.0));
}

TEST_CASE("Magma: a raft's placeholder layers never become the minimum layer height",
          "[Magma][regression]")
{
    // C1: m_layer_data is indexed by ABSOLUTE Layer::id() and object ids start at raft_layers(),
    // so a raft leaves zero-filled rows below the first object layer. Seeding the minimum from
    // row 0 seeded it with one of those 0.0s, which a "> 0" guard can never displace. The
    // minimum stayed 0, max_tube_height / 0.0 was +inf, and (int)ceil(inf) is undefined
    // behaviour -- INT_MIN on x86-64 -- collapsing the Z window to one layer and silently
    // disabling CP-SAT on every raft print, while the user was told to raise the solver timeout.
    std::vector<LayerData> layers(8);
    for (auto &ld : layers) ld.height = 0.0;   // raft placeholders
    layers[3].height = 0.20;
    layers[4].height = 0.20;
    layers[5].height = 0.12;                   // the real minimum
    layers[6].height = 0.20;
    layers[7].height = 0.20;

    CHECK(min_positive_layer_height(layers, 3) == Catch::Approx(0.12));
    // Scanning from row 0 must skip the zeros rather than return one: a zero is an absence of
    // data, not a thin layer.
    CHECK(min_positive_layer_height(layers, 0) == Catch::Approx(0.12));

    // No positive height anywhere reports 0 so the caller can bail, rather than inventing a
    // height and producing a plausible answer from data it does not have.
    std::vector<LayerData> all_zero(4);
    for (auto &ld : all_zero) ld.height = 0.0;
    CHECK(min_positive_layer_height(all_zero, 0) == Catch::Approx(0.0));
}

TEST_CASE("Magma: honeycomb does not inherit tri-hex's vertex overlap", "[Magma][regression]")
{
    // C4: honeycomb's vertex_overlap_excess_area returned 0 (correct -- degree-3 vertices where
    // line ENDS meet, no crossing) while the neighbouring line_overlap_excess_fraction returned
    // tri-hex's value, its comment copied verbatim including "(Same as tri-hex.)". Walls were
    // thinned ~10% for an overlap the other method said was zero, opening a ~0.05mm void up
    // every vertical wall -- a leak path for injected melt.
    //
    // That second method is gone with the overlap-correction feature, so one source remains.
    // What must not come back is honeycomb being handed a crossing overlap it does not have.
    const double lw = 0.42;
    CHECK(magma_geometry_for(ipMagmaHoneycomb).vertex_overlap_excess_area(lw)
          == Catch::Approx(0.0).margin(1e-12));
    CHECK(magma_geometry_for(ipMagmaTriHex).vertex_overlap_excess_area(lw) > 0.0);
}
