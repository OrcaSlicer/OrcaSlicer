#include <catch2/catch_all.hpp>

#include <cmath>
#include <vector>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Fill/FillTpmsAdaptive.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

ExPolygon rectangle(double x0, double y0, double x1, double y1)
{
    return ExPolygon(Points{Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1)});
}

// The expolygons stacked in 0.2 mm layers from z = 0 to height.
TpmsRadialField radial_field(const ExPolygons &expolygons, double height)
{
    std::vector<TpmsRadialField::Slice> slices;
    for (int i = 0; 0.2 * (i + 1) < height + EPSILON; ++i)
        slices.push_back({0.2 * i, 0.2 * (i + 1), &expolygons});
    return TpmsRadialField(slices, get_extents(expolygons), [] {});
}

double radial(const TpmsRadialField &field, const Vec3d &pt)
{
    std::array<TpmsRadialField::Radial, 2> radials;
    field.radial(pt, radials);
    return radials[0].t;
}

Vec3d center(const TpmsRadialField &field, const Vec3d &pt)
{
    std::array<TpmsRadialField::Radial, 2> radials;
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
    const TpmsRadialField field(slices, get_extents(layers[50]), [] {});

    for (const Vec3d &c : {c1, c2}) {
        CAPTURE(c.x());
        CHECK_THAT(center(field, c).x(), WithinAbs(c.x(), 0.5));
        CHECK_THAT(radial(field, c), WithinAbs(0., Tolerance));
        CHECK_THAT(radial(field, c + Vec3d(0., 0., 9.5)), WithinAbs(1., 2. * Tolerance));
    }
    // The side between the lobes is half way to the surface, where both patterns morph into each other.
    std::array<TpmsRadialField::Radial, 2> radials;
    REQUIRE(field.radial(0.5 * (c1 + c2), radials) == 2);
    for (const TpmsRadialField::Radial &r : radials) {
        CHECK_THAT(r.t, WithinAbs(0.5, 2. * Tolerance));
        CHECK_THAT(r.weight, WithinAbs(0.5, 0.05));
    }
}
