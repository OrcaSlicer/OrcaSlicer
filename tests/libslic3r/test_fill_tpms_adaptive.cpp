#include <catch2/catch_all.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Fill/FillTpmsAdaptive.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

ExPolygon rectangle(double x0, double y0, double x1, double y1)
{
    return ExPolygon(Points{Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1)});
}

// The expolygons stacked in 0.2 mm layers from z = 0 to height.
TpmsRadialField radial_field(const ExPolygons &expolygons, double height, TpmsAdaptiveMode mode = TpmsAdaptiveMode::Lobes)
{
    std::vector<TpmsRadialField::Slice> slices;
    for (int i = 0; 0.2 * (i + 1) < height + EPSILON; ++i)
        slices.push_back({0.2 * i, 0.2 * (i + 1), &expolygons});
    return TpmsRadialField(slices, get_extents(expolygons), mode, [] {});
}

double radial(const TpmsRadialField &field, const Vec3d &pt)
{
    TpmsRadialField::Radials radials;
    field.radial(pt, radials);
    return radials[0].t;
}

Vec3d center(const TpmsRadialField &field, const Vec3d &pt)
{
    TpmsRadialField::Radials radials;
    field.radial(pt, radials);
    return radials[0].center;
}

// The grid cells are 0.5 mm, so a radial coordinate over 10 mm is accurate to about a twentieth.
constexpr double Tolerance = 0.075;

} // namespace

TEST_CASE("TPMS radial field is zero at the center of a cube and one at its faces", "[FillTpmsAdaptive]")
{
    // A 20 mm cube, 10 mm from its center to every face.
    const ExPolygons      square{rectangle(0., 0., 20., 20.)};
    const TpmsRadialField field = radial_field(square, 20.);

    const Vec3d c = center(field, {10., 10., 10.});
    CHECK_THAT(c.x(), WithinAbs(10., 0.5));
    CHECK_THAT(c.y(), WithinAbs(10., 0.5));
    CHECK_THAT(c.z(), WithinAbs(10., 0.5));
    CHECK_THAT(radial(field, {10., 10., 10.}), WithinAbs(0., Tolerance));
    for (const Vec3d &face : {Vec3d(0., 10., 10.), Vec3d(20., 10., 10.), Vec3d(10., 0., 10.), Vec3d(10., 10., 0.), Vec3d(10., 10., 20.)}) {
        CAPTURE(face.x(), face.y(), face.z());
        CHECK_THAT(radial(field, face), WithinAbs(1., Tolerance));
    }
}

TEST_CASE("TPMS radial field grows linearly from the center of a cube to its faces", "[FillTpmsAdaptive]")
{
    const ExPolygons      square{rectangle(0., 0., 20., 20.)};
    const TpmsRadialField field = radial_field(square, 20.);
    for (double d = 1.; d < 10.; d += 1.) {
        CAPTURE(d);
        CHECK_THAT(radial(field, {10. - d, 10., 10.}), WithinAbs(d / 10., Tolerance));
        CHECK_THAT(radial(field, {10., 10., 10. + d}), WithinAbs(d / 10., Tolerance));
    }
}

TEST_CASE("TPMS radial field of a tall box is centered at its middle height", "[FillTpmsAdaptive]")
{
    // 20 x 20 x 60 mm: 10 mm from the center to the sides, 30 mm to the top and the bottom.
    const ExPolygons      square{rectangle(0., 0., 20., 20.)};
    const TpmsRadialField field = radial_field(square, 60.);

    CHECK_THAT(center(field, {10., 10., 45.}).z(), WithinAbs(30., 0.5));
    CHECK_THAT(radial(field, {10., 10., 15.}), WithinAbs(0.5, Tolerance));
    CHECK_THAT(radial(field, {10., 10., 45.}), WithinAbs(0.5, Tolerance));
    CHECK_THAT(radial(field, {15., 10., 30.}), WithinAbs(0.5, Tolerance));
}

TEST_CASE("TPMS radial field grades every body towards its own center", "[FillTpmsAdaptive]")
{
    const ExPolygons      squares{rectangle(0., 0., 20., 20.), rectangle(30., 0., 50., 20.)};
    const TpmsRadialField field = radial_field(squares, 20.);

    CHECK_THAT(center(field, {5., 10., 10.}).x(), WithinAbs(10., 0.5));
    CHECK_THAT(center(field, {45., 10., 10.}).x(), WithinAbs(40., 0.5));
    CHECK_THAT(radial(field, {40., 10., 10.}), WithinAbs(0., Tolerance));
    CHECK_THAT(radial(field, {30., 10., 10.}), WithinAbs(1., Tolerance));
}

TEST_CASE("TPMS radial field is beyond one outside of the object", "[FillTpmsAdaptive]")
{
    const ExPolygons      square{rectangle(0., 0., 20., 20.)};
    const TpmsRadialField field = radial_field(square, 20.);
    CHECK(radial(field, {-5., 10., 10.}) > 1.);
    CHECK(radial(field, {10., 10., 30.}) > 1.);
}

TEST_CASE("TPMS radial field grades every lobe of a body towards its own center", "[FillTpmsAdaptive]")
{
    // Two spheres of 10 mm united, their centers 16 mm apart: the neck between them is 6 mm deep.
    const Vec3d                        c1(10., 10., 10.), c2(26., 10., 10.);
    std::vector<ExPolygons>            layers;
    std::vector<TpmsRadialField::Slice> slices;
    for (int i = 0; i < 100; ++i) {
        const double z = 0.2 * i + 0.1, r = std::sqrt(std::max(0., 100. - sqr(z - 10.)));
        Polygons     circles;
        for (const Vec3d &c : {c1, c2}) {
            Polygon &circle = circles.emplace_back();
            for (int k = 0; k < 90; ++k)
                circle.points.push_back(Point::new_scale(c.x() + r * std::cos(k * 2. * PI / 90.), c.y() + r * std::sin(k * 2. * PI / 90.)));
        }
        layers.push_back(union_ex(circles));
    }
    for (int i = 0; i < 100; ++i)
        slices.push_back({0.2 * i, 0.2 * (i + 1), &layers[i]});
    const TpmsRadialField field(slices, get_extents(layers[50]), TpmsAdaptiveMode::Lobes, [] {});

    for (const Vec3d &c : {c1, c2}) {
        CAPTURE(c.x());
        CHECK_THAT(center(field, c).x(), WithinAbs(c.x(), 0.5));
        CHECK_THAT(radial(field, c), WithinAbs(0., Tolerance));
        CHECK_THAT(radial(field, c + Vec3d(0., 0., 9.5)), WithinAbs(1., 2. * Tolerance));
    }
    // The side between the lobes is half way to the surface, where both patterns morph into each other.
    TpmsRadialField::Radials radials;
    REQUIRE(field.radial(0.5 * (c1 + c2), radials) == 2);
    for (size_t i = 0; i < 2; ++i) {
        CHECK_THAT(radials[i].t, WithinAbs(0.5, 2. * Tolerance));
        CHECK_THAT(radials[i].weight, WithinAbs(0.5, 0.05));
    }
}

TEST_CASE("TPMS radial field blends the lobes meeting at a junction continuously", "[FillTpmsAdaptive]")
{
    // Three spheres of 10 mm united, their centers on a triangle of 16 mm sides: the necks meet at its middle.
    const std::array<Vec3d, 3>          centers{Vec3d(10., 10., 10.), Vec3d(26., 10., 10.), Vec3d(18., 10. + 8. * std::sqrt(3.), 10.)};
    std::vector<ExPolygons>             layers;
    std::vector<TpmsRadialField::Slice> slices;
    for (int i = 0; i < 100; ++i) {
        const double z = 0.2 * i + 0.1, r = std::sqrt(std::max(0., 100. - sqr(z - 10.)));
        Polygons     circles;
        for (const Vec3d &c : centers) {
            Polygon &circle = circles.emplace_back();
            for (int k = 0; k < 90; ++k)
                circle.points.push_back(Point::new_scale(c.x() + r * std::cos(k * 2. * PI / 90.), c.y() + r * std::sin(k * 2. * PI / 90.)));
        }
        layers.push_back(union_ex(circles));
    }
    for (int i = 0; i < 100; ++i)
        slices.push_back({0.2 * i, 0.2 * (i + 1), &layers[i]});
    const TpmsRadialField field(slices, get_extents(layers[50]), TpmsAdaptiveMode::Lobes, [] {});

    // Around the junction the nearest lobes swap, but the weight of every lobe changes smoothly.
    const Vec3d junction  = (centers[0] + centers[1] + centers[2]) / 3.;
    size_t      max_count = 0;
    double      max_jump  = 0.;
    double      max_error = 0.;
    for (int row = 0; row <= 100; ++row) {
        std::array<float, 3> previous{};
        for (int step = 0; step <= 200; ++step) {
            TpmsRadialField::Radials radials;
            const size_t count = field.radial(junction + Vec3d(0.01 * step - 1., 0.02 * row - 1., 0.), radials);
            max_count          = std::max(max_count, count);
            std::array<float, 3> weights{};
            for (size_t i = 0; i < count; ++i) {
                auto nearest = std::min_element(centers.begin(), centers.end(), [&](const Vec3d &a, const Vec3d &b) {
                    return (a - radials[i].center).norm() < (b - radials[i].center).norm();
                });
                weights[nearest - centers.begin()] += radials[i].weight;
            }
            max_error = std::max(max_error, std::abs(weights[0] + weights[1] + weights[2] - 1.));
            if (step > 0)
                for (size_t k = 0; k < 3; ++k)
                    max_jump = std::max(max_jump, double(std::abs(weights[k] - previous[k])));
            previous = weights;
        }
    }
    CHECK(max_count == 3);
    CHECK(max_error < 1e-5);
    CHECK(max_jump < 0.05);
}

TEST_CASE("TPMS radial field is empty when the object is thinner than the grid cells", "[FillTpmsAdaptive]")
{
    // A 0.3 mm square bar between the nodes of a grid sized by a 200 mm bounding box, with cells of 0.5 mm or more.
    const ExPolygons                    bar{rectangle(0.1, 0.1, 0.4, 0.4)};
    std::vector<TpmsRadialField::Slice> slices;
    for (int i = 0; i < 1000; ++i)
        slices.push_back({0.2 * i, 0.2 * (i + 1), &bar});
    const TpmsRadialField field(slices, BoundingBox(Point::new_scale(0., 0.), Point::new_scale(200., 200.)),
                                TpmsAdaptiveMode::Lobes, [] {});
    CHECK(field.empty());
}

TEST_CASE("TPMS depth follows the distance to the surface relative to the deepest point", "[FillTpmsAdaptive]")
{
    // 20 x 20 x 60 mm: from 10 to 50 mm high the axis is 10 mm deep, as deep as the center.
    const ExPolygons      square{rectangle(0., 0., 20., 20.)};
    const TpmsRadialField field = radial_field(square, 60., TpmsAdaptiveMode::SmoothBlend);
    for (double z : {15., 30., 45.}) {
        CAPTURE(z);
        CHECK_THAT(field.depth({10., 10., z}), WithinAbs(1., Tolerance));
    }
    CHECK_THAT(field.depth({5., 10., 30.}), WithinAbs(0.5, Tolerance));
    CHECK_THAT(field.depth({10., 10., 55.}), WithinAbs(0.5, Tolerance));
    CHECK_THAT(field.depth({0., 10., 30.}), WithinAbs(0., Tolerance));
}

TEST_CASE("TPMS radial field in Distance warp mode follows the distance to the surface", "[FillTpmsAdaptive]")
{
    // In a cube the depth falls linearly along every ray from the center, so Distance warp matches Lobes.
    const ExPolygons      square{rectangle(0., 0., 20., 20.)};
    const TpmsRadialField cube = radial_field(square, 20., TpmsAdaptiveMode::DistanceWarp);
    CHECK_THAT(radial(cube, {10., 10., 10.}), WithinAbs(0., Tolerance));
    CHECK_THAT(radial(cube, {15., 10., 10.}), WithinAbs(0.5, Tolerance));
    CHECK_THAT(radial(cube, {10., 10., 20.}), WithinAbs(1., Tolerance));

    // 20 x 20 x 60 mm: Lobes grades the axis towards the top and the bottom, Distance warp keeps it deep.
    const TpmsRadialField lobes = radial_field(square, 60.);
    const TpmsRadialField warp  = radial_field(square, 60., TpmsAdaptiveMode::DistanceWarp);
    for (double z : {15., 45.}) {
        CAPTURE(z);
        CHECK_THAT(radial(lobes, {10., 10., z}), WithinAbs(0.5, Tolerance));
        CHECK(radial(warp, {10., 10., z}) < 0.25);
    }
    CHECK_THAT(radial(warp, {0., 10., 30.}), WithinAbs(1., Tolerance));
}

TEST_CASE("TPMS stepped shells split a layer by depth from the surface inwards", "[FillTpmsAdaptive]")
{
    // The middle layer of a 40 mm cube, 20 mm from its center to every face, 20% at the surface to 5% inside.
    const ExPolygons             square{rectangle(0., 0., 40., 40.)};
    const TpmsRadialField        field  = radial_field(square, 40., TpmsAdaptiveMode::SteppedShells);
    const std::vector<TpmsShell> shells = make_tpms_shells(field, square.front(), 20., 0.2f, 0.05f, TpmsAdaptiveGradient::Linear);
    REQUIRE(shells.size() == 5);
    CHECK_THAT(shells.front().density, WithinAbs(0.2, 1e-6));
    CHECK_THAT(shells.back().density, WithinAbs(0.05, 1e-6));
    double area = 0.;
    for (size_t i = 0; i < shells.size(); ++i) {
        CAPTURE(i);
        if (i > 0)
            CHECK(shells[i].density < shells[i - 1].density);
        for (const ExPolygon &expolygon : shells[i].expolygons)
            area += expolygon.area();
    }
    CHECK_THAT(area / square.front().area(), WithinAbs(1., 0.01));
    const Point center = Point::new_scale(20., 20.);
    CHECK(std::any_of(shells.back().expolygons.begin(), shells.back().expolygons.end(),
                      [&center](const ExPolygon &expolygon) { return expolygon.contains(center); }));
}

TEST_CASE("TPMS radial field in 2D grades every section normal to the axis on its own", "[FillTpmsAdaptive]")
{
    // 20 x 20 x 60 mm: every section normal to Z is 10 mm from its center to the sides, whatever its height.
    const ExPolygons      square{rectangle(0., 0., 20., 20.)};
    const TpmsRadialField normal_z = radial_field(square, 60., TpmsAdaptiveMode::NormalZ);
    for (double z : {5., 30., 55.}) {
        CAPTURE(z);
        CHECK_THAT(radial(normal_z, {10., 10., z}), WithinAbs(0., Tolerance));
        CHECK_THAT(radial(normal_z, {15., 10., z}), WithinAbs(0.5, Tolerance));
        CHECK_THAT(radial(normal_z, {10., 0., z}), WithinAbs(1., Tolerance));
    }

    // Normal to X, the sections are 20 x 60 mm: 10 mm from the center to the sides, 30 mm to the top and the bottom.
    const TpmsRadialField normal_x = radial_field(square, 60., TpmsAdaptiveMode::NormalX);
    for (double x : {3., 10., 17.}) {
        CAPTURE(x);
        CHECK_THAT(radial(normal_x, {x, 15., 30.}), WithinAbs(0.5, Tolerance));
        CHECK_THAT(radial(normal_x, {x, 10., 45.}), WithinAbs(0.5, Tolerance));
    }
}
