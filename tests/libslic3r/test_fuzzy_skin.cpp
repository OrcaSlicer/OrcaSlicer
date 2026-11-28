#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <thread>
#include <vector>

#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Feature/FuzzySkin/FuzzySkin.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using namespace Slic3r::Feature::FuzzySkin;

namespace {

// Closed 20 mm square loop at a uniform width.
Arachne::ExtrusionJunctions square_loop(coord_t width)
{
    const coord_t s = scaled<coord_t>(20.);
    return {{Point(0, 0), width, 0}, {Point(s, 0), width, 0}, {Point(s, s), width, 0}, {Point(0, s), width, 0}, {Point(0, 0), width, 0}};
}

FuzzySkinConfig thick_fuzzy_config(FuzzySkinMode mode, NoiseType noise_type, double thickness_mm)
{
    FuzzySkinConfig cfg{};
    cfg.type              = FuzzySkinType::All;
    cfg.thickness         = scaled<coord_t>(thickness_mm);
    cfg.point_distance    = scaled<coord_t>(0.3);
    cfg.fuzzy_first_layer = true;
    cfg.noise_type        = noise_type;
    cfg.noise_scale       = 1.0;
    cfg.noise_octaves     = 4;
    cfg.noise_persistence = 0.5;
    cfg.mode              = mode;
    cfg.layer_id          = 5;
    return cfg;
}

// A 10 mm circle of 64 points around (cx, cy).
Points circle(double cx = 0., double cy = 0.)
{
    Points pts;
    for (int i = 0; i < 64; ++i) {
        const double a = 2. * PI * i / 64.;
        pts.emplace_back(scaled<coord_t>(cx + 5. * std::cos(a)), scaled<coord_t>(cy + 5. * std::sin(a)));
    }
    return pts;
}

FuzzySkinConfig plain_fuzzy_config(NoiseType noise_type)
{
    FuzzySkinConfig cfg  = thick_fuzzy_config(FuzzySkinMode::Displacement, noise_type, 0.3);
    cfg.point_distance   = scaled<coord_t>(0.8);
    return cfg;
}

Points fuzzed(Points poly, double slice_z, NoiseType noise_type = NoiseType::Classic, bool closed = true)
{
    fuzzy_polyline(poly, closed, slice_z, plain_fuzzy_config(noise_type));
    return poly;
}

Arachne::ExtrusionJunctions fuzzed_junctions(double slice_z, NoiseType noise_type, bool closed)
{
    Arachne::ExtrusionJunctions loop;
    for (const Point &p : circle())
        loop.emplace_back(p, scaled<coord_t>(0.42), 0);
    loop.emplace_back(loop.front());
    fuzzy_extrusion_line(loop, slice_z, /*layer_height*/ 0.2, plain_fuzzy_config(noise_type), closed);
    return loop;
}

// How many points two outputs of equal size differ at by more than rounding.
size_t points_differing(const Points &a, const Points &b)
{
    size_t n = 0;
    for (size_t i = 0; i < a.size(); ++i)
        n += (a[i] - b[i]).cast<double>().norm() > scaled<double>(0.01);
    return n;
}

} // namespace

// Extrusion and Combined mode add noise to each junction's width. A junction narrower than
// height * (1 - PI/4) makes Flow::rounded_rectangle_extrusion_spacing() throw and fails the slice.
// The fuzz thickness is 3x the line width so the clamp is hit on every run regardless of RNG seed.
// Ridged multifractal is covered because its output is not bounded to [-1, 1], so it scales past
// the configured thickness; the floor has to hold for any noise value, not just an in-range one.
TEST_CASE("Fuzzy skin extrusion width is floored at the minimum the flow accepts", "[Arachne][FuzzySkin]") {
    using namespace Slic3r::Feature::FuzzySkin;

    const double layer_height = GENERATE(0.08, 0.2, 0.28);
    const auto   mode         = GENERATE(FuzzySkinMode::Extrusion, FuzzySkinMode::Combined);
    const auto   noise_type   = GENERATE(NoiseType::Classic, NoiseType::Perlin, NoiseType::Billow, NoiseType::RidgedMulti, NoiseType::Voronoi);
    CAPTURE(layer_height, int(mode), int(noise_type));

    const double line_width_mm = 0.42;
    auto         loop          = square_loop(scaled<coord_t>(line_width_mm));
    fuzzy_extrusion_line(loop, /*slice_z*/ 1.0, layer_height, thick_fuzzy_config(mode, noise_type, 3 * line_width_mm));

    REQUIRE(loop.size() > 100);

    const auto   narrowest    = std::min_element(loop.begin(), loop.end(), [](const auto& a, const auto& b) { return a.w < b.w; });
    const double narrowest_mm = unscaled<double>(narrowest->w);
    const double floor_mm     = layer_height * (1. - 0.25 * PI);
    CAPTURE(narrowest_mm, floor_mm);

    CHECK(narrowest_mm < line_width_mm); // the clamp was exercised
    CHECK(narrowest_mm > floor_mm);
    CHECK_NOTHROW(Flow::rounded_rectangle_extrusion_spacing(float(narrowest_mm), float(layer_height)));
}

// Displacement mode only moves points; widths must pass through unchanged.
TEST_CASE("Fuzzy skin displacement mode leaves widths untouched", "[Arachne][FuzzySkin]") {
    using namespace Slic3r::Feature::FuzzySkin;

    const coord_t width = scaled<coord_t>(0.42);
    auto          loop  = square_loop(width);
    fuzzy_extrusion_line(loop, /*slice_z*/ 1.0, /*layer_height*/ 0.2, thick_fuzzy_config(FuzzySkinMode::Displacement, NoiseType::Classic, 1.26));

    REQUIRE(loop.size() > 100);
    CHECK(std::all_of(loop.begin(), loop.end(), [width](const auto& j) { return j.w == width; }));
}

TEST_CASE("Fuzzy skin gives the same points for the same outline twice", "[FuzzySkin]")
{
    const auto noise_type = GENERATE(NoiseType::Classic, NoiseType::Perlin);
    const bool closed     = GENERATE(true, false);
    CAPTURE(int(noise_type), closed);

    CHECK(fuzzed(circle(), 1.0, noise_type, closed) == fuzzed(circle(), 1.0, noise_type, closed));

    const auto a = fuzzed_junctions(1.0, noise_type, closed);
    const auto b = fuzzed_junctions(1.0, noise_type, closed);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].p == b[i].p);
        CHECK(a[i].w == b[i].w);
    }
}

TEST_CASE("Fuzzy skin gives the same points on another thread", "[FuzzySkin]")
{
    const Points here = fuzzed(circle(), 1.0);
    Points       there;
    std::thread([&there] { there = fuzzed(circle(), 1.0); }).join();
    CHECK(here == there);
}

// The same outline on neighbouring layers must not repeat its pattern, or the print shows
// aligned patches up the wall.
TEST_CASE("Fuzzy skin differs between neighbouring layers of the same outline", "[FuzzySkin]")
{
    std::set<Points> layers;
    for (int i = 0; i < 50; ++i)
        layers.insert(fuzzed(circle(), 0.2 * (i + 1)));
    CHECK(layers.size() == 50);

    // Not merely different somewhere: most points move, unless the point spacing already differs.
    const Points first = fuzzed(circle(), 0.2), second = fuzzed(circle(), 0.4);
    CHECK((first.size() != second.size() || points_differing(first, second) > first.size() / 2));
}

TEST_CASE("Fuzzy skin gives two outlines in one layer different patterns", "[FuzzySkin]")
{
    const Points here    = fuzzed(circle(), 1.0);
    Points       shifted = fuzzed(circle(30., 0.), 1.0);
    for (Point &p : shifted)
        p.x() -= scaled<coord_t>(30.);
    CHECK((here.size() != shifted.size() || points_differing(here, shifted) > here.size() / 2));
}
