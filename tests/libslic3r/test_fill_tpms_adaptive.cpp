#include <catch2/catch_all.hpp>

#include <vector>

#include "libslic3r/Fill/FillTpmsAdaptive.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// A 20 mm cube sliced in 0.2 mm layers. Its deepest point is its center, 10 mm from every face.
struct Cube
{
    ExPolygons                         square{ExPolygon(Points{Point::new_scale(0., 0.), Point::new_scale(20., 0.),
                                                               Point::new_scale(20., 20.), Point::new_scale(0., 20.)})};
    std::vector<TpmsDepthField::Slice> slices;

    Cube()
    {
        for (int i = 0; i < 100; ++i)
            slices.push_back({0.2 * i, 0.2 * (i + 1), &square});
    }
    TpmsDepthField field() const { return TpmsDepthField(slices, get_extents(square), [] {}); }
};

// The grid cells are 0.5 mm, so the depth is accurate to about a cell.
constexpr double Tolerance = 0.5 / 10. * 1.5;

} // namespace

TEST_CASE("TPMS depth field is one at the deepest point and zero at the surface", "[FillTpmsAdaptive]")
{
    const TpmsDepthField field = Cube().field();
    CHECK_THAT(field.depth({10., 10., 10.}), WithinAbs(1., Tolerance));
    CHECK_THAT(field.depth({0., 10., 10.}), WithinAbs(0., Tolerance));
    CHECK_THAT(field.depth({10., 20., 10.}), WithinAbs(0., Tolerance));
    CHECK_THAT(field.depth({10., 10., 0.}), WithinAbs(0., Tolerance));
    CHECK_THAT(field.depth({10., 10., 20.}), WithinAbs(0., Tolerance));
}

TEST_CASE("TPMS depth field grows with the distance to the nearest face", "[FillTpmsAdaptive]")
{
    const TpmsDepthField field = Cube().field();
    for (double d = 1.; d < 10.; d += 1.) {
        CAPTURE(d);
        CHECK_THAT(field.depth({d, 10., 10.}), WithinAbs(d / 10., Tolerance));
        CHECK_THAT(field.depth({10., 10., d}), WithinAbs(d / 10., Tolerance));
        // Nearer to another face than to the one it moves away from.
        CHECK_THAT(field.depth({d, 2., 10.}), WithinAbs(std::min(d, 2.) / 10., Tolerance));
    }
}

TEST_CASE("TPMS depth field is zero outside of the object", "[FillTpmsAdaptive]")
{
    const TpmsDepthField field = Cube().field();
    CHECK_THAT(field.depth({-5., 10., 10.}), WithinAbs(0., 1e-6));
    CHECK_THAT(field.depth({10., 10., 30.}), WithinAbs(0., 1e-6));
}

TEST_CASE("TPMS depth field measures the depth from the walls of a hole", "[FillTpmsAdaptive]")
{
    // A 20 mm square with a 10 mm square hole: a 5 mm wide ring, deepest halfway between the hole and the sides.
    ExPolygon ring(Points{Point::new_scale(0., 0.), Point::new_scale(20., 0.), Point::new_scale(20., 20.), Point::new_scale(0., 20.)});
    ring.holes.emplace_back(Points{Point::new_scale(5., 5.), Point::new_scale(5., 15.), Point::new_scale(15., 15.), Point::new_scale(15., 5.)});
    const ExPolygons                   expolygons{ring};
    std::vector<TpmsDepthField::Slice> slices;
    for (int i = 0; i < 100; ++i)
        slices.push_back({0.2 * i, 0.2 * (i + 1), &expolygons});
    const TpmsDepthField field(slices, get_extents(expolygons), [] {});

    CHECK_THAT(field.depth({10., 10., 10.}), WithinAbs(0., 1e-6));
    const float middle = field.depth({2.5, 10., 10.});
    CHECK(middle > field.depth({1., 10., 10.}));
    CHECK(middle > field.depth({4., 10., 10.}));
    CHECK_THAT(field.depth({17.5, 10., 10.}), WithinAbs(middle, Tolerance));
    CHECK_THAT(field.depth({10., 2.5, 10.}), WithinAbs(middle, Tolerance));
}
