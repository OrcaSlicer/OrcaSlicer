#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/SlicedDistanceField.hpp"
#include "libslic3r/EdgeGrid.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/libslic3r.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <stdexcept>

using namespace Slic3r;
namespace {
ExPolygon rectangle(double x0, double y0, double x1, double y1)
{
    return ExPolygon{Point::new_scale(x0, y0), Point::new_scale(x1, y0),
                     Point::new_scale(x1, y1), Point::new_scale(x0, y1)};
}
}

TEST_CASE("Sliced sliced distances include holes and use each point's height", "[SlicedDistanceField]")
{
    ExPolygon lower = rectangle(-20., -20., 20., 20.);
    lower.holes.push_back(rectangle(-3., -3., 3., 3.).contour);
    lower.holes.back().reverse();
    const ExPolygons low {lower}, high {rectangle(-10., -10., 10., 10.)};
    SlicedDistanceField field({{-5., 5., &low}, {5., 15., &high}});
    const auto values = field.sample({Vec3d(19., 0., 0.), Vec3d(0., 0., 0.),
                                      Vec3d(4., 0., 0.), Vec3d(9., 0., 10.), Vec3d(11., 0., 10.)});
    REQUIRE_THAT(values[0], Catch::Matchers::WithinAbs(-1., 0.2));
    REQUIRE(values[1] > 0.);
    REQUIRE_THAT(values[2], Catch::Matchers::WithinAbs(-1., 0.2));
    REQUIRE_THAT(values[3], Catch::Matchers::WithinAbs(-1., 0.2));
    REQUIRE_THAT(values[4], Catch::Matchers::WithinAbs(1., 0.2));
    const auto again = field.sample({Vec3d(11., 0., 10.), Vec3d(19., 0., 0.)});
    REQUIRE_THAT(again[0], Catch::Matchers::WithinAbs(values[4], EPSILON));
    REQUIRE_THAT(again[1], Catch::Matchers::WithinAbs(values[0], EPSILON));
}

TEST_CASE("Sliced wall distances include floors and roofs without internal layer caps", "[SlicedDistanceField]")
{
    const ExPolygons outlines {rectangle(-20., -20., 20., 20.)};
    SlicedDistanceField field({{0., 10., &outlines}, {10., 20., &outlines}});
    const auto values = field.sample({Vec3d(0., 0., 1.), Vec3d(0., 0., 19.),
                                      Vec3d(0., 0., 10.), Vec3d(0., 0., -2.),
                                      Vec3d(0., 0., 22.), Vec3d(21., 0., 22.)});
    REQUIRE_THAT(values[0], Catch::Matchers::WithinAbs(-1., 0.2));
    REQUIRE_THAT(values[1], Catch::Matchers::WithinAbs(-1., 0.2));
    REQUIRE_THAT(values[2], Catch::Matchers::WithinAbs(-10., 0.2));
    REQUIRE_THAT(values[3], Catch::Matchers::WithinAbs(2., 0.2));
    REQUIRE_THAT(values[4], Catch::Matchers::WithinAbs(2., 0.2));
    REQUIRE_THAT(values[5], Catch::Matchers::WithinAbs(std::sqrt(5.), 0.2));
    SlicedDistanceField rounded({{0., 0.3, &outlines}, {0.1 + 0.2, 0.6, &outlines}});
    REQUIRE(std::abs(rounded.sample({Vec3d(0., 0., 0.3)}).front()) <= 2.);
}

TEST_CASE("Sliced wall distances include local ledges and cavity floors and ceilings", "[SlicedDistanceField]")
{
    const ExPolygons wide {rectangle(-20., -20., 20., 20.)};
    const ExPolygons narrow {rectangle(-10., -10., 10., 10.)};
    SlicedDistanceField ledge({{0., 10., &wide}, {10., 20., &narrow}});
    const auto values = ledge.sample({Vec3d(15., 0., 9.), Vec3d(15., 0., 11.), Vec3d(0., 0., 10.)});
    REQUIRE_THAT(values[0], Catch::Matchers::WithinAbs(-1., 0.2));
    REQUIRE_THAT(values[1], Catch::Matchers::WithinAbs(1., 0.2));
    REQUIRE_THAT(values[2], Catch::Matchers::WithinAbs(-10., 0.2));

    ExPolygon hollow = wide.front();
    hollow.holes.push_back(narrow.front().contour);
    hollow.holes.back().reverse();
    const ExPolygons middle {hollow};
    SlicedDistanceField cavity({{0., 10., &wide}, {10., 20., &middle}, {20., 30., &wide}});
    const auto cap = cavity.sample({Vec3d(0., 0., 9.), Vec3d(0., 0., 11.),
                                   Vec3d(0., 0., 19.), Vec3d(0., 0., 21.)});
    REQUIRE_THAT(cap[0], Catch::Matchers::WithinAbs(-1., 0.2));
    REQUIRE_THAT(cap[1], Catch::Matchers::WithinAbs(1., 0.2));
    REQUIRE_THAT(cap[2], Catch::Matchers::WithinAbs(1., 0.2));
    REQUIRE_THAT(cap[3], Catch::Matchers::WithinAbs(-1., 0.2));
}

TEST_CASE("Sliced cubic distances preserve a fixed component beside changing profiles", "[SlicedDistanceField]")
{
    std::vector<ExPolygons> profiles(20);
    std::vector<SlicedDistanceField::Slice> slices;
    for (size_t i = 0; i < profiles.size(); ++i) {
        profiles[i].push_back(rectangle(-100., -50., 0., 50.));
        const double z = (i + 0.5) * 5.;
        const double radius = std::sqrt(2500. - (z - 50.) * (z - 50.));
        Polygon circle;
        for (int n = 0; n < 32; ++n) {
            const double angle = 2. * M_PI * n / 32.;
            circle.points.push_back(Point::new_scale(50. + radius * std::cos(angle), radius * std::sin(angle)));
        }
        profiles[i].push_back(ExPolygon(circle));
        slices.push_back({i * 5., (i + 1) * 5., &profiles[i]});
    }
    SlicedDistanceField field(slices);
    std::vector<Vec3d> points;
    for (double x : {-99., -50., -1.})
        for (double y : {-49., 0., 49.})
            for (double z : {1., 50., 99.}) points.emplace_back(x, y, z);
    const auto distances = field.sample(points);
    for (size_t i = 0; i < points.size(); ++i) {
        const Vec3d q = (points[i] - Vec3d(-50., 0., 50.)).cwiseAbs() - Vec3d(50., 50., 50.);
        REQUIRE_THAT(distances[i], Catch::Matchers::WithinAbs(q.maxCoeff(), 0.7));
    }
    const ExPolygons lower {rectangle(-20., -20., 20., 20.)}, upper {rectangle(-10., -10., 10., 10.)};
    SlicedDistanceField ledge({{0., 10., &lower}, {10., 20., &upper}});
    REQUIRE_THAT(ledge.sample({Vec3d(9., 0., 9.)}).front(), Catch::Matchers::WithinAbs(-std::sqrt(2.), 0.2));
}

TEST_CASE("Sliced distances preserve long diagonal walls and caps", "[SlicedDistanceField]")
{
    const ExPolygons outlines {ExPolygon{Point::new_scale(0., -100.), Point::new_scale(100., 0.),
                                         Point::new_scale(0., 100.), Point::new_scale(-100., 0.)}};
    SlicedDistanceField field({{0., 100., &outlines}});
    const auto values = field.sample({Vec3d(30., 20., 50.), Vec3d(40., 30., 1.),
                                      Vec3d(40., 30., 99.), Vec3d(70., 60., 50.),
                                      Vec3d(103., 0., 50.), Vec3d(0., 0., -2.)});
    REQUIRE_THAT(values[0], Catch::Matchers::WithinAbs(-50. / std::sqrt(2.), 0.5));
    REQUIRE_THAT(values[1], Catch::Matchers::WithinAbs(-1., 0.3));
    REQUIRE_THAT(values[2], Catch::Matchers::WithinAbs(-1., 0.3));
    REQUIRE_THAT(values[3], Catch::Matchers::WithinAbs(30. / std::sqrt(2.), 0.3));
    REQUIRE_THAT(values[4], Catch::Matchers::WithinAbs(3., 0.3));
    REQUIRE_THAT(values[5], Catch::Matchers::WithinAbs(2., 0.3));
}

TEST_CASE("Sliced cubic distances include walls floors holes and deep interiors", "[SlicedDistanceField]")
{
    const ExPolygons outlines {rectangle(-40., -40., 40., 40.)};
    SlicedDistanceField table({{0., 40., &outlines}, {40., 80., &outlines}});
    const auto values = table.sample({Vec3d(0., 0., 40.), Vec3d(0., 0., 1.),
                                      Vec3d(0., 0., 79.), Vec3d(42., 0., 40.), Vec3d(100., 0., 40.)});
    REQUIRE_THAT(values[0], Catch::Matchers::WithinAbs(-40., 0.5));
    REQUIRE_THAT(values[1], Catch::Matchers::WithinAbs(-1., 0.15));
    REQUIRE_THAT(values[2], Catch::Matchers::WithinAbs(-1., 0.15));
    REQUIRE(values[3] > 0.);
    REQUIRE(values[4] > 32.);
    ExPolygon hollow = outlines.front();
    hollow.holes.push_back(rectangle(-10., -10., 10., 10.).contour);
    hollow.holes.back().reverse();
    const ExPolygons middle {hollow};
    SlicedDistanceField cavity({{0., 10., &outlines}, {10., 20., &middle}, {20., 30., &outlines}});
    REQUIRE_THAT(cavity.sample({Vec3d(0., 0., 15.)}).front(), Catch::Matchers::WithinAbs(5., 0.2));
    REQUIRE_THAT(cavity.sample({Vec3d(0., 0., 5.)}).front(), Catch::Matchers::WithinAbs(-5., 0.2));
}

TEST_CASE("Sliced logarithmic distances retain zero crossings before density saturation", "[SlicedDistanceField]")
{
    const ExPolygons outlines {rectangle(-20., -20., 20., 20.)};
    SlicedDistanceField table({{0., 20., &outlines}});
    const auto values = table.sample({Vec3d(0., 0., -0.25), Vec3d(0., 0., 0.),
                                      Vec3d(0., 0., 0.25), Vec3d(19.5, 0., 10.)});
    REQUIRE_THAT(values[0], Catch::Matchers::WithinAbs(0.25, 0.03));
    REQUIRE_THAT(values[1], Catch::Matchers::WithinAbs(0., EPSILON));
    REQUIRE_THAT(values[2], Catch::Matchers::WithinAbs(-0.25, 0.03));
    REQUIRE_THAT(values[3], Catch::Matchers::WithinAbs(-0.5, 0.06));
}

TEST_CASE("Sliced logarithmic distances retain distant slices and saturate at one metre", "[SlicedDistanceField]")
{
    const ExPolygons outlines {rectangle(-4., -4., 4., 4.)};
    SlicedDistanceField table({{0., 10., &outlines}, {2210., 2220., &outlines}});
    const auto values = table.sample({Vec3d(0., 0., 50.), Vec3d(0., 0., 1000.),
                                      Vec3d(0., 0., 1110.), Vec3d(0., 0., 2170.)});
    REQUIRE_THAT(values[0], Catch::Matchers::WithinAbs(40., 0.5));
    REQUIRE_THAT(values[1], Catch::Matchers::WithinAbs(990., 11.));
    REQUIRE_THAT(values[2], Catch::Matchers::WithinAbs(1000., EPSILON));
    REQUIRE_THAT(values[3], Catch::Matchers::WithinAbs(40., 0.5));
}

TEST_CASE("Sliced cubic distances retain slices between output planes", "[SlicedDistanceField]")
{
    const ExPolygons solid {rectangle(-20., -20., 20., 20.)};
    ExPolygon hollow = solid.front();
    hollow.holes.push_back(rectangle(-10., -10., 10., 10.).contour);
    hollow.holes.back().reverse();
    const ExPolygons cavity {hollow};
    SlicedDistanceField table({{0., 10.2, &solid}, {10.2, 10.3, &cavity}, {10.3, 20., &solid}});
    const auto values = table.sample({Vec3d(0., 0., 9.), Vec3d(0., 0., 12.), Vec3d(15., 0., 9.)});
    REQUIRE(std::abs(values[0]) <= 2.2);
    REQUIRE(std::abs(values[1]) <= 2.2);
    REQUIRE(std::abs(values[2]) > 4.);
    const ExPolygons empty;
    SlicedDistanceField gap({{0., 10.2, &solid}, {10.2, 10.3, &empty}, {10.3, 20., &solid}});
    REQUIRE(std::abs(gap.sample({Vec3d(15., 0., 9.)}).front()) <= 2.2);
    SlicedDistanceField missing({{0., 10.2, &solid}, {10.3, 20., &solid}});
    REQUIRE(gap.sample({Vec3d(15., 0., 9.)}) == missing.sample({Vec3d(15., 0., 9.)}));
}

TEST_CASE("Sliced distance sweeps match all slab minima across changing thin profiles", "[SlicedDistanceField]")
{
    std::vector<ExPolygons> profiles(40);
    std::vector<SlicedDistanceField::Slice> slices;
    double height = 0.;
    for (size_t i = 0; i < profiles.size(); ++i) {
        const double radius = 5. + (i * 7 % 13);
        profiles[i].push_back(rectangle(-radius, -18., radius, 18.));
        if (i % 3 == 0) {
            profiles[i].front().holes.push_back(rectangle(-2., -3., 2., 3.).contour);
            profiles[i].front().holes.back().reverse();
        }
        const double thickness = i % 2 == 0 ? 0.17 : 1.31;
        slices.push_back({height, height + thickness, &profiles[i]});
        height += thickness;
        if (i == 20) height += 0.23;
    }
    SlicedDistanceField field(slices);
    const BoundingBox bounds(Point::new_scale(-21., -22.), Point::new_scale(21., 22.));
    std::vector<Vec3d> points;
    for (double z = -2.; z <= std::ceil(height) + 2.; z += 1.)
        for (double x = -19.; x <= 19.; x += 1.) points.emplace_back(x, 0., z);
    std::vector<double> solid(points.size(), 1e6), air(points.size());
    std::vector<bool> inside(points.size(), false);
    for (size_t i = 0; i < points.size(); ++i) {
        const double dz = std::max(0., std::min(points[i].z(), height - points[i].z()));
        air[i] = dz * dz;
    }
    double previous_top = 0.;
    for (const auto &slice : slices) {
        EdgeGrid::Grid grid(bounds);
        grid.create(*slice.contours, scale_(1.));
        grid.calculate_sdf();
        for (size_t i = 0; i < points.size(); ++i) {
            const double z = points[i].z();
            if (slice.bottom_z > previous_top) {
                const double gap = std::max({0., previous_top - z, z - slice.bottom_z});
                air[i] = std::min(air[i], gap * gap);
            }
            const float sdf = float(grid.signed_distance_bilinear(Point::new_scale(points[i].x(), points[i].y())) * SCALING_FACTOR);
            const double dz = std::max({0., slice.bottom_z - z, z - slice.top_z});
            solid[i] = std::min(solid[i], dz * dz + std::pow(std::max(0.f, sdf), 2));
            air[i] = std::min(air[i], dz * dz + std::pow(std::max(0.f, -sdf), 2));
            if (dz == 0. && sdf < 0.) inside[i] = true;
        }
        previous_top = slice.top_z;
    }
    const auto actual = field.sample(points);
    for (size_t i = 0; i < points.size(); ++i) {
        const double distance = std::sqrt(inside[i] ? air[i] : solid[i]);
        const double code = std::round(255. * std::log1p(distance / 6.) / std::log1p(1000. / 6.));
        const double expected = (inside[i] ? -6. : 6.) * std::expm1(code * std::log1p(1000. / 6.) / 255.);
        REQUIRE_THAT(actual[i], Catch::Matchers::WithinAbs(expected, 1e-6));
    }
}

TEST_CASE("Sliced distance construction reports progress and propagates cancellation", "[SlicedDistanceField]")
{
    const ExPolygons model {rectangle(-10., -10., 10., 10.)};
    auto previous = SlicedDistanceField::Stage::Distance;
    double fraction = 0.;
    SlicedDistanceField field({{0., 20., &model}}, [&](auto stage, double value) {
        REQUIRE(int(stage) >= int(previous));
        if (stage == previous) REQUIRE(value >= fraction);
        REQUIRE(value >= 0.);
        REQUIRE(value <= 1.);
        previous = stage;
        fraction = value;
    });
    REQUIRE(previous == SlicedDistanceField::Stage::Complete);
    REQUIRE_THAT(fraction, Catch::Matchers::WithinAbs(1., 1e-12));
    REQUIRE_THROWS_AS(SlicedDistanceField({{0., 20., &model}}, [](auto, double) {
        throw std::runtime_error("Canceled");
    }), std::runtime_error);
    REQUIRE_THAT(SlicedDistanceField().sample({Vec3d::Zero()}).front(), Catch::Matchers::WithinAbs(1000., 1e-12));
}
