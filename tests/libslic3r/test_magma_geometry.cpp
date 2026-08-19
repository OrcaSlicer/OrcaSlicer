#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Magma/MagmaGeometry.hpp"
#include "libslic3r/Magma/MagmaPatterns.hpp"
#include "libslic3r/Magma/MagmaTriangleCell.hpp"
#include "libslic3r/Magma/MagmaTubeMap.hpp"

using namespace Slic3r;
using namespace Slic3r::magma;

// Pure-geometry contract tests for the Magma cell shapes.
//
// These exist because the same quantity was being derived independently in several files, each
// copy carrying a comment asserting it matched the canonical version, and every one of those
// comments turned out to be false. A comment cannot enforce an invariant; these can.
//
// Deliberately free of Print/Model/pipeline dependencies so they stay fast and can run on every
// build.

namespace {

struct PatternCase {
    const char   *name;
    InfillPattern pattern;
    // Inscribed-circle diameter as a fraction of the circumscribed-circle diameter, i.e. how
    // much of what the nozzle must cover is actually usable tube. cos(pi/n) for a regular
    // n-gon: triangle 1/2, square 1/sqrt2, hexagon sqrt3/2.
    double        bore_over_opening;
};

const std::vector<PatternCase> &all_patterns()
{
    static const std::vector<PatternCase> cases = {
        { "MagmaTriangle",    ipMagmaTriangle,    0.5              },
        { "MagmaRectilinear", ipMagmaRectilinear, 1.0 / std::sqrt(2.0) },
        { "MagmaHoneycomb",   ipMagmaHoneycomb,   std::sqrt(3.0) / 2.0 },
        { "MagmaTriHex",      ipMagmaTriHex,      std::sqrt(3.0) / 2.0 },
    };
    return cases;
}

// Representative geometries: a small tube on a fine nozzle, a typical one, a large one.
struct Sizing { double interior; double line_width; };
const std::vector<Sizing> &sizings()
{
    static const std::vector<Sizing> s = {
        { 1.20, 0.45 }, { 1.62, 0.60 }, { 2.50, 0.66 }, { 4.00, 0.80 },
    };
    return s;
}

} // namespace

TEST_CASE("Magma: interior_for_opening is the exact inverse of opening_diameter", "[Magma][geometry]")
{
    for (const PatternCase &pc : all_patterns()) {
        const MagmaGeometry &geom = magma_geometry_for(pc.pattern);
        for (const Sizing &sz : sizings()) {
            const double spacing = cell_spacing_from_geometry(sz.interior, sz.line_width);
            const double opening = geom.opening_diameter(spacing, sz.line_width);
            REQUIRE(opening > 0.0);
            INFO(pc.name << " interior=" << sz.interior << " lw=" << sz.line_width);
            CHECK(geom.interior_for_opening(opening, sz.line_width)
                  == Catch::Approx(sz.interior).epsilon(1e-9));
        }
    }
}

TEST_CASE("Magma: bore is the inscribed circle of the cell, not the interior width", "[Magma][geometry]")
{
    // Regression: every geometry returned interior_width * 0.5, which is correct for square,
    // hex and tri-hex and wrong for triangle -- a near-miss that is right 3 times in 4. The
    // GUI reported a triangle bore ~59% too large while the validator quoted the correct 50%.
    for (const PatternCase &pc : all_patterns()) {
        const MagmaGeometry &geom = magma_geometry_for(pc.pattern);
        for (const Sizing &sz : sizings()) {
            const double spacing = cell_spacing_from_geometry(sz.interior, sz.line_width);
            const double opening = geom.opening_diameter(spacing, sz.line_width);
            const double bore    = 2.0 * geom.inscribed_radius(sz.interior, sz.line_width);
            INFO(pc.name << " interior=" << sz.interior << " lw=" << sz.line_width
                         << " opening=" << opening << " bore=" << bore);
            CHECK(bore / opening == Catch::Approx(pc.bore_over_opening).epsilon(1e-6));
        }
    }
}

TEST_CASE("Magma: the immersion budget round-trips through sizing and slam", "[Magma][seal]")
{
    // Auto tube sizing inverts the budget into an opening; auto_seal_depth must then spend
    // exactly that budget. If these drift, the tube is sized against one number and sealed
    // against another -- which is how a seal warning ends up blind to the case it exists for.
    const double flat  = 1.70;
    const double cone  = 30.0;
    const double press = 0.10;

    for (const PatternCase &pc : all_patterns()) {
        const MagmaGeometry &geom = magma_geometry_for(pc.pattern);
        for (double immersion : { 0.0, 0.2, 0.6, 1.0 }) {
            const double opening_max = max_opening_for_immersion(flat, cone, immersion);
            const double interior    = geom.interior_for_opening(opening_max, 0.60);
            const double spacing     = cell_spacing_from_geometry(interior, 0.60);
            const double opening     = geom.opening_diameter(spacing, 0.60);

            INFO(pc.name << " immersion=" << immersion);
            // Sizing must reproduce the opening the budget allows.
            CHECK(opening == Catch::Approx(opening_max).epsilon(1e-9));

            const double seal = auto_seal_depth(opening, flat, cone,
                                                immersion_budget(immersion), press);
            // The seal spends exactly the budget: no more, no less. The minimum seal depth
            // cannot move this either way -- above the geometric need it is capped by the same
            // budget, below it the geometry already asks for more.
            CHECK(seal == Catch::Approx(immersion).epsilon(1e-9));
        }
    }
}

TEST_CASE("Magma: the minimum seal depth cannot outrank the immersion budget", "[Magma][seal]")
{
    // Regression: the budget used to be max(min_seal_depth, max_immersion), so a floor bounded
    // at 1.0mm raised a setting named for the maximum whenever immersion sat below it -- a
    // 0.3mm immersion with a 0.8mm floor drove 0.8mm into the part, past the depth measured as
    // visibly deforming. A floor under the seal is descent past the rim like any other.
    const double flat = 1.70, cone = 30.0;

    for (double immersion : { 0.0, 0.1, 0.3, 0.6, 1.0 }) {
        const double budget  = immersion_budget(immersion);
        const double opening = max_opening_for_immersion(flat, cone, immersion);
        for (double floor_depth : { 0.0, 0.05, 0.5, 1.0 }) {
            const double seal   = auto_seal_depth(opening, flat, cone, budget, floor_depth);
            const double plunge = clamp_plunge_depth(seal, 0.05, budget);
            INFO("immersion=" << immersion << " min_seal_depth=" << floor_depth);
            CHECK(seal >= 0.0);
            CHECK(seal + plunge <= immersion + 1e-9);   // the stated maximum, from every path
        }
    }

    // Seating mode: at a zero budget nothing enters the tube, the floor included. The tube is
    // covered by the flat outright, so the nozzle seats on the rim without descending.
    const double opening0 = max_opening_for_immersion(flat, cone, 0.0);
    CHECK(opening0 <= flat);
    CHECK(auto_seal_depth(opening0, flat, cone, immersion_budget(0.0), 0.10)
          == Catch::Approx(0.0).margin(1e-12));
}

TEST_CASE("Magma: mechanical interference past first contact is opening-independent", "[Magma][seal]")
{
    // The finding the immersion model is built on: because auto_seal_depth solves for
    // opening + MAGMA_SEAL_MARGIN while the rim is first touched at opening, the interference
    // past contact is MARGIN / (2 tan theta) -- the opening and the flat cancel. Two prints
    // differing only in immersion had identical interference; immersion is what deforms tubes.
    const double cone = 30.0;
    const double expected = MAGMA_SEAL_MARGIN / (2.0 * std::tan(cone * MAGMA_DEG2RAD));

    for (double flat : { 1.2, 1.7, 2.4 }) {
        for (double opening : { 2.0, 2.9, 3.6 }) {
            if (opening <= flat) continue;
            const double d_slam    = seal_depth_for_opening(opening + MAGMA_SEAL_MARGIN, flat, cone);
            const double d_contact = seal_depth_for_opening(opening, flat, cone);
            INFO("flat=" << flat << " opening=" << opening);
            CHECK(d_slam - d_contact == Catch::Approx(expected).epsilon(1e-9));
        }
    }
}

TEST_CASE("Magma: effective_interior_width honours the tube width mode", "[Magma][sizing]")
{
    // Regression: this was resolved three different ways -- once via a triangle-specific free
    // function that ignored the geometry, and twice in Print::validate ignoring the mode
    // entirely, so warnings were evaluated against tubes that were never printed.
    const double flat = 1.70, cone = 30.0, lw = 0.60, immersion = 0.6;

    for (const PatternCase &pc : all_patterns()) {
        const MagmaGeometry &geom = magma_geometry_for(pc.pattern);
        INFO(pc.name);

        // Manual: the user's value, verbatim, whatever the nozzle says.
        CHECK(effective_interior_width(geom, MagmaTubeWidthMode::Manual, 2.2,
                                       flat, lw, cone, immersion, 0.0)
              == Catch::Approx(2.2).epsilon(1e-12));

        // Auto: resolved through THIS pattern's geometry, ignoring the manual value.
        const double expect = geom.interior_for_opening(
            max_opening_for_immersion(flat, cone, immersion), lw);
        CHECK(effective_interior_width(geom, MagmaTubeWidthMode::Auto, 2.2,
                                       flat, lw, cone, immersion, 0.0)
              == Catch::Approx(expect).epsilon(1e-12));

        // ...and Auto must differ per pattern, or the geometry is not being consulted.
        if (pc.pattern != ipMagmaTriangle) {
            const double tri = magma_geometry_for(ipMagmaTriangle).interior_for_opening(
                max_opening_for_immersion(flat, cone, immersion), lw);
            CHECK(expect != Catch::Approx(tri).epsilon(1e-6));
        }
    }
}

TEST_CASE("Magma: effective pattern resolution follows dual-infill state", "[Magma][sizing]")
{
    CHECK(magma_effective_pattern(false, ipMagmaHoneycomb, ipMagmaRectilinear)
          == ipMagmaRectilinear);                                   // dual off -> sparse wins
    CHECK(magma_effective_pattern(true, ipMagmaHoneycomb, ipMagmaRectilinear)
          == ipMagmaHoneycomb);                                     // dual on  -> outer wins
    CHECK(magma_effective_pattern(true, ipGrid, ipMagmaRectilinear)
          == ipMagmaTriangle);                                      // non-Magma outer -> substituted
}

TEST_CASE("Magma: geometry and lattice agree on edge length", "[Magma][geometry]")
{
    // Regression: hex is the only pattern where MagmaGeometry::edge_length and the lattice's
    // own edge disagree, and the window height cap is derived from the former -- so the hex
    // auto window is permanently undersized.
    const double lw = 0.60;
    for (const PatternCase &pc : all_patterns()) {
        const MagmaGeometry &geom = magma_geometry_for(pc.pattern);
        for (const Sizing &sz : sizings()) {
            const double spacing = cell_spacing_from_geometry(sz.interior, sz.line_width);
            INFO(pc.name << " spacing=" << spacing);
            // The open (inset) edge must be shorter than the full edge but still positive for
            // any sizing we would actually print.
            const double edge = geom.edge_length(spacing);
            CHECK(edge > 0.0);
            CHECK(edge > lw * 0.5);
        }
    }
}

TEST_CASE("Magma: auto sizing leaves room for the plunge", "[Magma][seal]")
{
    // Regression: auto sizing spent the ENTIRE immersion budget on the seal depth, so
    // clamp_plunge_depth found zero headroom and the plunge was always exactly 0 in Auto mode
    // -- the default. The slam-melt never ran, with the setting switched on in the UI.
    const double flat = 1.75, cone = 30.0, lw = 0.60, immersion = 0.6, press = 0.1;
    for (const PatternCase &pc : all_patterns()) {
        const MagmaGeometry &geom = magma_geometry_for(pc.pattern);
        for (double plunge_cfg : { 0.05, 0.2 }) {
            const double interior = effective_interior_width(
                geom, MagmaTubeWidthMode::Auto, 2.2, flat, lw, cone, immersion, plunge_cfg);
            const double spacing  = cell_spacing_from_geometry(interior, lw);
            const double opening  = geom.opening_diameter(spacing, lw);
            const double budget   = immersion_budget(immersion);
            const double seal     = auto_seal_depth(opening, flat, cone, budget, press);
            const double plunge   = clamp_plunge_depth(seal, plunge_cfg, budget);
            INFO(pc.name << " plunge_cfg=" << plunge_cfg << " seal=" << seal);
            CHECK(plunge == Catch::Approx(plunge_cfg).epsilon(1e-6));
            CHECK(seal + plunge <= budget + 1e-9);
        }
    }
}

TEST_CASE("Magma: the plunge never spends past the immersion budget", "[Magma][seal]")
{
    // Regression: clamp_plunge_depth used to bound seal+plunge only by a separate 4.0mm
    // constant and ignore max_immersion entirely, so a user who lowered the immersion budget
    // still got the full plunge driven past it -- the budget bounded the seal but not the
    // plunge. The budget is now the only ceiling, which is why the 4.0mm constant is gone.
    for (double seal : { 0.1, 0.6, 1.5, 3.4 })
        for (double plunge : { 0.0, 0.05, 0.5, 2.0 })
            for (double budget : { 0.0, 0.1, 0.6, 3.5 }) {
                const double clamped = clamp_plunge_depth(seal, plunge, budget);
                INFO("seal=" << seal << " plunge=" << plunge << " budget=" << budget);
                CHECK(clamped >= 0.0);
                CHECK(clamped <= plunge);
                // A seal already past the budget leaves no room at all, rather than the
                // plunge topping up to some other ceiling.
                CHECK(clamped <= std::max(0.0, budget - seal) + 1e-9);
                CHECK(seal + clamped <= std::max(seal, budget) + 1e-9);
            }
}
