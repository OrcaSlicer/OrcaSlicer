#include <catch2/catch_all.hpp>

#include <vector>

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

double radial(const TpmsRadialField &field, const Vec3d &pt) { return field.radial(pt).second; }

// The grid cells are 0.5 mm, so a radial coordinate over 10 mm is accurate to about a twentieth.
constexpr double Tolerance = 0.075;

} // namespace

TEST_CASE("TPMS radial field is zero at the center of a cube and one at its faces", "[FillTpmsAdaptive]")
{
    // A 20 mm cube, 10 mm from its center to every face.
    const ExPolygons      square{rectangle(0., 0., 20., 20.)};
    const TpmsRadialField field = radial_field(square, 20.);

    const auto [center, t] = field.radial({10., 10., 10.});
    CHECK_THAT(center.x(), WithinAbs(10., 0.5));
    CHECK_THAT(center.y(), WithinAbs(10., 0.5));
    CHECK_THAT(center.z(), WithinAbs(10., 0.5));
    CHECK_THAT(t, WithinAbs(0., Tolerance));
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

    CHECK_THAT(field.radial({10., 10., 45.}).first.z(), WithinAbs(30., 0.5));
    CHECK_THAT(radial(field, {10., 10., 15.}), WithinAbs(0.5, Tolerance));
    CHECK_THAT(radial(field, {10., 10., 45.}), WithinAbs(0.5, Tolerance));
    CHECK_THAT(radial(field, {15., 10., 30.}), WithinAbs(0.5, Tolerance));
}

TEST_CASE("TPMS radial field grades every body towards its own center", "[FillTpmsAdaptive]")
{
    const ExPolygons      squares{rectangle(0., 0., 20., 20.), rectangle(30., 0., 50., 20.)};
    const TpmsRadialField field = radial_field(squares, 20.);

    CHECK_THAT(field.radial({5., 10., 10.}).first.x(), WithinAbs(10., 0.5));
    CHECK_THAT(field.radial({45., 10., 10.}).first.x(), WithinAbs(40., 0.5));
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
