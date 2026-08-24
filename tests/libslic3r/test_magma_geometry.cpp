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

TEST_CASE("Magma: the press is depth past first contact, whatever the tube", "[Magma][seal]")
{
    // The whole point of parameterising it this way: the depth travelled past first contact IS
    // the setting, for every tube size and every nozzle, with no conversion and no regime
    // switch. The previous radial form needed a division by tan(theta) here and a separate
    // branch for tubes the flat already spanned.
    const double cone = 30.0;
    for (double press : { 0.0, 0.05, 0.0866, 0.3 })
        for (double flat : { 1.2, 1.7, 2.4 })
            for (double opening : { 1.0, 2.0, 2.9, 3.6 }) {
                const double d_seal    = auto_seal_depth(opening, flat, cone, press);
                const double d_contact = seal_depth_for_opening(opening, flat, cone);
                INFO("flat=" << flat << " opening=" << opening << " press=" << press);
                CHECK(d_seal - d_contact == Catch::Approx(press).epsilon(1e-9));
            }
}
TEST_CASE("Magma: effective pattern resolution follows dual-infill state", "[Magma][sizing]")
{
    CHECK(magma_effective_pattern(false, ipMagmaHoneycomb, ipMagmaRectilinear)
          == ipMagmaRectilinear);                                   // dual off -> sparse wins
    CHECK(magma_effective_pattern(true, ipMagmaHoneycomb, ipMagmaRectilinear)
          == ipMagmaHoneycomb);                                     // dual on  -> outer wins
    // No longer clamped: a non-Magma outer pattern is a legitimate choice -- a zone with no
    // injectable channels -- so it comes back as chosen rather than substituted.
    CHECK(magma_effective_pattern(true, ipGrid, ipMagmaRectilinear)
          == ipGrid);                                               // non-Magma outer -> honoured
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

TEST_CASE("Magma: seal depth is solved for the cell's furthest corner", "[Magma][seal]")
{
    // Tube width and seal depth are the same number in two units. The cone must widen to cover
    // the CIRCUMSCRIBED circle -- the corners are the last thing it reaches -- plus the corner
    // press. Solve one way and back again for every pattern; if these drift, a tube is
    // sized against one depth and sealed at another.
    const double flat = 1.70, cone = 30.0, lw = 0.60;
    for (const PatternCase &pc : all_patterns()) {
        const MagmaGeometry &geom = magma_geometry_for(pc.pattern);
        for (double interior : { 1.2, 1.6, 2.0 }) {
            for (double press : { 0.0, 0.05, 0.2 }) {
                const double spacing = cell_spacing_from_geometry(interior, lw);
                const double opening = geom.opening_diameter(spacing, lw);
                const double d       = auto_seal_depth(opening, flat, cone, press);
                INFO(pc.name << " interior=" << interior << " press=" << press);
                // The cone covers the cell's furthest corner and is then pressed further, so
                // coverage is never short. Equality only when the flat did not already span it.
                CHECK(cone_diameter_at(d, flat, cone) + 1e-9 >= opening);
                CHECK(d - seal_depth_for_opening(opening, flat, cone)
                      == Catch::Approx(press).epsilon(1e-9));
                // And the bore/opening ratio is the pattern's cos(pi/n), independently computed.
                CHECK(2.0 * geom.inscribed_radius(interior, lw)
                      == Catch::Approx(opening * pc.bore_over_opening).epsilon(1e-9));
            }
        }
    }
}

TEST_CASE("Magma: corner grip depends on press and plunge, nothing else", "[Magma][seal]")
{
    // The corner is the least-pressed contact in the cell and the one that lets go first, so
    // what holds it is the number worth pinning. It is deliberately independent of tube size
    // and of seal depth -- a wider tube does NOT grip its corners any harder.
    // Press and plunge are the same quantity in the same units -- depth past first contact,
    // one before the injection and one during it -- so they enter the grip identically.
    const double cone = 30.0, T = std::tan(cone * MAGMA_DEG2RAD);
    for (double press : { 0.0, 0.05, 0.2 })
        for (double pl : { 0.0, 0.1, 0.3 }) {
            INFO("press=" << press << " plunge=" << pl);
            CHECK(corner_grip(press, pl, cone) == Catch::Approx((press + pl) * T).epsilon(1e-9));
        }
    // No press and no plunge means the corners are covered and ungripped -- the geometry
    // seals and the mechanics do not.
    CHECK(corner_grip(0.0, 0.0, cone) == Catch::Approx(0.0).margin(1e-12));
}

TEST_CASE("Magma: the plunge is additive, not carved out of the seal", "[Magma][seal]")
{
    // Regression: the plunge used to be subtracted before the tube was sized while the emitter
    // added it back at print time, so raising the plunge silently shrank the tube. Sizing no
    // longer sees it at all, and only the absolute sanity clamp bounds the pair.
    for (double interior : { 1.2, 1.6, 2.0 })
        CHECK(effective_interior_width(interior) == Catch::Approx(interior));

    for (double seal : { 0.2, 0.6, 1.0 })
        for (double pl : { 0.0, 0.05, 0.3, 2.0 }) {
            const double got = clamp_plunge_depth(seal, pl);
            INFO("seal=" << seal << " plunge=" << pl);
            CHECK(got <= pl + 1e-9);
            CHECK(seal + got <= MAGMA_SLAM_CLAMP + 1e-9);
        }
}
