// Tests for VariableWidth: turning variable width lines, as Arachne makes them, into extrusion paths.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/VariableWidth.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using Catch::Approx;

namespace {

// A straight 12 mm wall: 10 mm whose width creeps from 0.450 to 0.459 mm (2%), then 2 mm at 0.55 mm.
ThickPolyline creeping_wall()
{
    ThickPolyline wall;
    for (int i = 0; i <= 10; ++ i)
        wall.points.emplace_back(scaled<coord_t>(double(i)), 0);
    wall.points.emplace_back(scaled<coord_t>(12.), 0);
    // Two widths per segment, at its start and at its end.
    for (int i = 0; i < 10; ++ i) {
        wall.width.emplace_back(scaled<double>(0.45 + 0.0009 * i));
        wall.width.emplace_back(scaled<double>(0.45 + 0.0009 * (i + 1)));
    }
    wall.width.emplace_back(scaled<double>(0.55));
    wall.width.emplace_back(scaled<double>(0.55));
    return wall;
}

double volume(const ExtrusionMultiPath &multi_path)
{
    double v = 0.;
    for (const ExtrusionPath &path : multi_path.paths)
        v += path.mm3_per_mm * unscaled(path.polyline.length());
    return v;
}

} // namespace

TEST_CASE("Wall width tolerance merges near-equal widths into one path", "[VariableWidth]")
{
    const ThickPolyline wall = creeping_wall();
    const Flow          flow(0.45f, 0.2f, 0.4f);
    const float         band = float(0.05 * flow.scaled_width()); // a wall width tolerance of 5%

    const ExtrusionMultiPath banded = thick_polyline_to_multi_path_banded(wall, erPerimeter, flow, scaled<float>(0.05), band);
    const ExtrusionMultiPath exact  = thick_polyline_to_multi_path(wall, erPerimeter, flow, scaled<float>(0.05), 0.f);

    // The 2% creep shares one path, at its mean width; the step to 0.55 mm starts another. The widths of the lines are
    // spacings: a path is wider by the rounded sides of the extrusion.
    const double spacing_offset = 0.2 * (1. - 0.25 * PI);
    REQUIRE(banded.paths.size() == 2);
    CHECK(banded.paths.front().width == Approx(0.4545 + spacing_offset).epsilon(0.001));
    CHECK(banded.paths.back().width == Approx(0.55 + spacing_offset).epsilon(0.001));
    CHECK(exact.paths.size() > banded.paths.size());

    // The same material as the wall: the mean width carries the volume of the widths it replaces.
    const double expected = (10. * 0.4545 + 2. * 0.55) * 0.2;
    CHECK(volume(banded) == Approx(expected).epsilon(0.0001));
}
