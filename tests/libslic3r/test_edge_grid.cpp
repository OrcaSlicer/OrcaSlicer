#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "catch2/matchers/catch_matchers.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/EdgeGrid.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

TEST_CASE("EdgeGrid propagates signed distances across the interior and exterior", "[EdgeGrid][Regression]")
{
    const Polygon rectangle {Point::new_scale(-10., -12.), Point::new_scale(10., -12.),
                             Point::new_scale(10., 12.), Point::new_scale(-10., 12.)};
    const Polygons outlines {rectangle};
    const BoundingBox bounds(Point::new_scale(-30., -32.), Point::new_scale(30., 32.));
    EdgeGrid::Grid grid(bounds);
    grid.create(outlines, scale_(1.));
    // Distances beyond the initialized edge band require backward propagation.
    // With -fsanitize=shift, this also checks negative neighbour offsets.
    grid.calculate_sdf();
    const auto distance = [&](double x, double y) {
        return grid.signed_distance_bilinear(Point::new_scale(x, y)) * SCALING_FACTOR;
    };
    REQUIRE_THAT(distance(0., 0.), Catch::Matchers::WithinAbs(-10., 0.01));
    for (double sign : {-1., 1.}) {
        REQUIRE_THAT(distance(sign * 5., 0.), Catch::Matchers::WithinAbs(-5., 0.01));
        REQUIRE_THAT(distance(0., sign * 7.), Catch::Matchers::WithinAbs(-5., 0.01));
        REQUIRE_THAT(distance(sign * 20., 0.), Catch::Matchers::WithinAbs(10., 0.01));
        REQUIRE_THAT(distance(0., sign * 22.), Catch::Matchers::WithinAbs(10., 0.01));
    }
}
