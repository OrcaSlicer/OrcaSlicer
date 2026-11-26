#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Eigen/Geometry>

#include "libslic3r/Point.hpp"
#include "slic3r/GUI/NavigatorGeometry.hpp"
#include "slic3r/GUI/PreviewOverlayLayout.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

NavProjection projection_from(const Vec3d& to_camera, const Vec3d& up_hint)
{
    const Vec3d     z = to_camera.normalized();
    const Vec3d     y = (up_hint - z * up_hint.dot(z)).normalized();
    const Vec3d     x = y.cross(z);
    NavProjection   p;
    p.rotation.row(0) = x;
    p.rotation.row(1) = y;
    p.rotation.row(2) = z;
    p.center          = Vec2d(100., 100.);
    p.scale           = 40.;
    return p;
}

double projected_area(const NavZone& zone, const NavProjection& p)
{
    double area = 0.;
    for (size_t i = 0, j = zone.polygon.size() - 1; i < zone.polygon.size(); j = i++) {
        const Vec2d a = p.project(zone.polygon[j]);
        const Vec2d b = p.project(zone.polygon[i]);
        area += a.x() * b.y() - b.x() * a.y();
    }
    return std::abs(0.5 * area);
}

size_t count(NavZoneKind kind)
{
    const std::vector<NavZone>& zones = navigator_cube_zones();
    return size_t(std::count_if(zones.begin(), zones.end(), [kind](const NavZone& z) { return z.kind == kind; }));
}

} // namespace

TEST_CASE("The navigator cube has 6 faces, 12 edges and 8 corners", "[NavigatorGeometry]")
{
    REQUIRE(navigator_cube_zones().size() == NAV_ZONE_COUNT);
    CHECK(count(NavZoneKind::Face) == 6);
    CHECK(count(NavZoneKind::Edge) == 12);
    CHECK(count(NavZoneKind::Corner) == 8);
}

TEST_CASE("Faces are octagons, edges rectangles and corners hexagons", "[NavigatorGeometry]")
{
    for (const NavZone& zone : navigator_cube_zones()) {
        const size_t expected = zone.kind == NavZoneKind::Face ? 8 : zone.kind == NavZoneKind::Edge ? 4 : 6;
        CHECK(zone.polygon.size() == expected);
    }
}

TEST_CASE("Zone polygons wind counterclockwise seen from outside", "[NavigatorGeometry]")
{
    for (const NavZone& zone : navigator_cube_zones()) {
        Vec3d normal = Vec3d::Zero();
        for (size_t i = 0, j = zone.polygon.size() - 1; i < zone.polygon.size(); j = i++)
            normal += zone.polygon[j].cross(zone.polygon[i]);
        CHECK(normal.dot(zone.normal) > 0.);
    }
}

TEST_CASE("Edge and corner targets are of a similar size in isometry", "[NavigatorGeometry]")
{
    const NavProjection         p     = projection_from(Vec3d(1., -1., 1.), Vec3d::UnitZ());
    const std::vector<NavZone>& zones = navigator_cube_zones();
    const NavZone&              face  = zones[3];
    const auto                  edge  = std::find_if(zones.begin(), zones.end(), [&p](const NavZone& z) {
        return z.kind == NavZoneKind::Edge && navigator_zone_visible(z, p) && z.normal.z() > 0.5 && z.normal.y() < -0.5;
    });
    const auto corner = std::find_if(zones.begin(), zones.end(), [](const NavZone& z) {
        return z.kind == NavZoneKind::Corner && z.normal.x() > 0. && z.normal.y() < 0. && z.normal.z() > 0.;
    });
    REQUIRE(edge != zones.end());
    REQUIRE(corner != zones.end());
    const double ratio = projected_area(*edge, p) / projected_area(*corner, p);
    CHECK(ratio > 0.6);
    CHECK(ratio < 1.6);
    CHECK(projected_area(face, p) > 3. * projected_area(*corner, p));
}

TEST_CASE("A point in a visible zone hits that zone, from above and below, flat and in perspective", "[NavigatorGeometry]")
{
    const Vec3d above = GENERATE(Vec3d(1., -1., 1.), Vec3d(-0.4, -1., -0.8));
    const bool  perspective = GENERATE(false, true);
    NavProjection p         = projection_from(above, Vec3d::UnitZ());
    p.eye_distance          = perspective ? NAV_EYE_DISTANCE : 0.;
    const std::vector<NavZone>& zones = navigator_cube_zones();
    size_t                      hit   = 0;
    for (size_t i = 0; i < zones.size(); ++i) {
        if (!navigator_zone_visible(zones[i], p))
            continue;
        Vec3d centroid = Vec3d::Zero();
        for (const Vec3d& v : zones[i].polygon)
            centroid += v;
        centroid /= double(zones[i].polygon.size());
        CHECK(navigator_zone_at(p, p.project(centroid)) == int(i));
        ++hit;
    }
    CHECK(hit >= 7);
    CHECK(navigator_zone_at(p, Vec2d(-500., -500.)) == -1);
}

TEST_CASE("The bed tab sticks out of the front edge and the outer corners are rounded", "[NavigatorGeometry]")
{
    const std::vector<Vec3d>& outline = navigator_bed_outline();
    const auto                front   = std::min_element(outline.begin(), outline.end(), [](const Vec3d& a, const Vec3d& b) { return a.y() < b.y(); });
    CHECK_THAT(front->y(), WithinAbs(-NAV_BED_HALF - 0.21, 1e-6));
    CHECK(std::abs(front->x()) < 0.35);
    for (const Vec3d& v : outline) {
        CHECK(std::abs(v.x()) <= NAV_BED_HALF + 1e-9);
        CHECK(std::abs(v.x()) + std::abs(v.y()) <= 2. * NAV_BED_HALF - 0.09);
        CHECK_THAT(v.z(), WithinAbs(NAV_BED_TOP, 1e-12));
    }
}

TEST_CASE("A label fades out as its face turns away", "[NavigatorGeometry]")
{
    CHECK_THAT(navigator_label_opacity(Vec3d::UnitZ(), Vec3d::UnitZ()), WithinAbs(1.0, 1e-6));
    CHECK_THAT(navigator_label_opacity(Vec3d::UnitZ(), Vec3d::UnitX()), WithinAbs(0.0, 1e-6));
    const auto at = [](double cos) { return navigator_label_opacity(Vec3d::UnitZ(), Vec3d(std::sqrt(1. - cos * cos), 0., cos)); };
    CHECK_THAT(at(0.3), WithinAbs(0.0, 1e-6));
    CHECK_THAT(at(0.6), WithinAbs(1.0, 1e-6));
    CHECK(at(0.4) > 0.f);
    CHECK(at(0.4) < at(0.5));
    CHECK(at(0.5) < 1.f);
}

TEST_CASE("In perspective the near side of the cube looks larger and a face seen at a glancing angle hides", "[NavigatorGeometry]")
{
    NavProjection p = projection_from(Vec3d(0., -1., 0.), Vec3d::UnitZ());
    p.eye_distance  = NAV_EYE_DISTANCE;
    CHECK_THAT((p.project(Vec3d::Zero()) - p.center).norm(), WithinAbs(0., 1e-9));
    const double near_side = (p.project(Vec3d(1., -1., 0.)) - p.center).norm();
    const double far_side  = (p.project(Vec3d(1., 1., 0.)) - p.center).norm();
    CHECK(near_side > far_side);

    NavProjection glancing = projection_from(Vec3d(0.1, -1., 0.), Vec3d::UnitZ());
    const NavZone& right   = navigator_cube_zones()[0];
    CHECK(navigator_zone_visible(right, glancing));
    glancing.eye_distance = NAV_EYE_DISTANCE;
    CHECK_FALSE(navigator_zone_visible(right, glancing));
}

TEST_CASE("The bed fades from opaque at the horizon to a quarter at 25 degrees below it", "[NavigatorGeometry]")
{
    auto at = [](double degrees) {
        const double a = degrees * 3.14159265358979 / 180.;
        return navigator_bed_opacity(Vec3d(std::cos(a), 0., std::sin(a)));
    };
    CHECK_THAT(at(30.), WithinAbs(1.0, 1e-6));
    CHECK_THAT(at(0.), WithinAbs(1.0, 1e-6));
    CHECK_THAT(at(-12.), WithinAbs(0.63, 0.01));
    CHECK_THAT(at(-25.), WithinAbs(0.25, 1e-6));
    CHECK_THAT(at(-90.), WithinAbs(0.25, 1e-6));
}

TEST_CASE("A zone view looks at the zone with Z up, and from the top with the back up", "[NavigatorGeometry]")
{
    const NavView top = navigator_zone_view(4);
    CHECK_THAT(top.to_camera.z(), WithinAbs(1.0, 1e-9));
    CHECK_THAT(top.up.y(), WithinAbs(1.0, 1e-9));
    const NavView front = navigator_zone_view(3);
    CHECK_THAT(front.to_camera.y(), WithinAbs(-1.0, 1e-9));
    CHECK_THAT(front.up.z(), WithinAbs(1.0, 1e-9));
}

TEST_CASE("Without a background the toolbar is a card of its own under the cube, centred on it", "[NavigatorGeometry]")
{
    const float     bottom = 900.f;
    const NavLayout l      = navigator_layout(false, 1.5f, bottom);
    CHECK_FALSE(l.background);
    CHECK(l.toolbar.bottom < bottom);
    CHECK(l.buttons.front().left > l.toolbar.left);
    CHECK(l.buttons.back().right < l.toolbar.right);
    CHECK_THAT(l.buttons.front().left - l.toolbar.left, WithinAbs(l.toolbar.right - l.buttons.back().right, 1.));
    CHECK(l.cube.bottom < l.toolbar.top);
    CHECK_THAT(l.cube.left + l.cube.right, WithinAbs(l.toolbar.left + l.toolbar.right, 1e-3));
    CHECK(l.section.left > l.toolbar.right);
}

TEST_CASE("The toolbar's buttons and separator start on a whole pixel at any scale, with a background or without", "[NavigatorGeometry]")
{
    for (const bool background : {false, true})
        for (const float scale : {1.f, 1.25f, 1.5f, 1.75f, 2.f}) {
            const NavLayout l = navigator_layout(background, scale, 900.f);
            CHECK_THAT(l.buttons.front().left, WithinAbs(std::round(l.buttons.front().left), 1e-6));
            CHECK_THAT(l.buttons.front().top, WithinAbs(std::round(l.buttons.front().top), 1e-6));
            CHECK_THAT(l.separator.left, WithinAbs(std::round(l.separator.left), 1e-6));
            CHECK_THAT(l.separator.top, WithinAbs(std::round(l.separator.top), 1e-6));
            CHECK_THAT(l.separator.bottom, WithinAbs(std::round(l.separator.bottom), 1e-6));
        }
}

TEST_CASE("With a background the cube and the toolbar share one card", "[NavigatorGeometry]")
{
    const NavLayout l = navigator_layout(true, 1.f, 800.f);
    CHECK(l.background);
    CHECK_THAT(l.toolbar.bottom, WithinAbs(l.card.bottom, 1e-3));
    CHECK_THAT(l.toolbar.left, WithinAbs(l.card.left, 1e-3));
    CHECK_THAT(l.toolbar.right, WithinAbs(l.card.right, 1e-3));
    CHECK(l.cube.top > l.card.top);
    CHECK(l.cube.bottom < l.toolbar.top);
    CHECK(l.cube.left > l.card.left);
    CHECK(l.cube.right < l.card.right);
    CHECK(l.section.left > l.card.right);
    CHECK_THAT(l.top(), WithinAbs(l.card.top, 1e-3));
}

TEST_CASE("The toolbar buttons fill the toolbar with a separator before the last", "[NavigatorGeometry]")
{
    const float     scale  = GENERATE(1.f, 1.5f);
    const NavLayout l      = navigator_layout(false, scale, 800.f);
    const float     inset  = l.buttons.front().left - l.toolbar.left;
    CHECK(inset > 0.f);
    CHECK_THAT(l.toolbar.right - l.buttons.back().right, WithinAbs(inset, 1.));
    CHECK_THAT(l.buttons[0].top - l.toolbar.top, WithinAbs(inset, 1.));
    CHECK_THAT(l.toolbar.bottom - l.buttons[0].bottom, WithinAbs(inset, 1.));
    CHECK(l.separator.left > l.buttons[4].right);
    CHECK(l.separator.right < l.buttons[5].left);
    CHECK(l.section.left > l.toolbar.right);
    CHECK_THAT(l.section.bottom, WithinAbs(l.toolbar.bottom, 1e-3));
}

TEST_CASE("The cube grows with the interface scale and keeps its size whatever the height of the scene", "[NavigatorGeometry]")
{
    const bool  background = GENERATE(false, true);
    const auto  half_edge  = [background](float scale, float scene_bottom) {
        return navigator_projection(navigator_layout(background, scale, scene_bottom).cube, Eigen::Matrix3d::Identity()).scale;
    };
    const double base = half_edge(1.f, 800.f);
    CHECK(base > 0.);
    CHECK_THAT(half_edge(1.f, 1200.f), WithinAbs(base, 1e-9));
    CHECK_THAT(half_edge(1.5f, 800.f), WithinRel(1.5 * base, 0.02));
    CHECK_THAT(half_edge(2.f, 800.f), WithinRel(2. * base, 0.02));
}

TEST_CASE("The cube sits above the center of its area when the bed is under it on screen", "[NavigatorGeometry]")
{
    const CanvasUi::Rect   area{0.f, 0.f, 200.f, 160.f};
    const NavProjection p = navigator_projection(area, Eigen::Matrix3d::Identity());
    CHECK_THAT(p.project(Vec3d::Zero()).y(), WithinAbs(80., 1e-6));
    Eigen::Matrix3d front;
    front << 1., 0., 0., 0., 0., 1., 0., -1., 0.;
    CHECK(navigator_projection(area, front).project(Vec3d::Zero()).y() < 80.);
    CHECK(navigator_bed_at(p, p.project(Vec3d(0., 0., NAV_BED_TOP))));
}

TEST_CASE("A navigator turn stops when something else turns the camera or other canvases drew meanwhile, not on a slow frame", "[NavigatorGeometry]")
{
    const Eigen::Quaterniond set(Eigen::AngleAxisd(0.3, Vec3d::UnitZ()));
    const Eigen::Quaterniond moved(Eigen::AngleAxisd(0.35, Vec3d::UnitZ()));
    CHECK_FALSE(navigator_turn_interrupted(set, set, 1));
    CHECK(navigator_turn_interrupted(moved, set, 1));
    CHECK(navigator_turn_interrupted(set, set, 2));
}

TEST_CASE("The home view reads back as it was saved, and anything but a rotation reads as none", "[NavigatorGeometry]")
{
    const Eigen::Quaterniond view(Eigen::AngleAxisd(0.7, Vec3d(1., 2., 3.).normalized()));
    const std::optional<Eigen::Quaterniond> read = navigator_view_from_string(navigator_view_to_string(view));
    REQUIRE(read.has_value());
    CHECK(read->angularDistance(view) < 1e-7);
    CHECK_FALSE(navigator_view_from_string("").has_value());
    CHECK_FALSE(navigator_view_from_string("1 0 0").has_value());
    CHECK_FALSE(navigator_view_from_string("2 0 0 0").has_value());
}

TEST_CASE("A press becomes a drag past the threshold, and its release turns to the zone pressed unless it dragged", "[NavigatorGeometry]")
{
    NavPress press{3, false, Vec2d(100., 100.), false};
    CHECK_FALSE(navigator_drag_starts(press, Vec2d(100., 100.), 3.));
    CHECK_FALSE(navigator_drag_starts(press, Vec2d(102., 102.), 3.));
    CHECK(navigator_drag_starts(press, Vec2d(104., 100.), 3.));
    CHECK(navigator_released_zone(press) == std::optional<size_t>(3));

    press.dragging = true;
    CHECK_FALSE(navigator_drag_starts(press, Vec2d(200., 100.), 3.));
    CHECK_FALSE(navigator_released_zone(press).has_value());

    const NavPress bed{-1, true, Vec2d(100., 100.), false};
    CHECK_FALSE(navigator_released_zone(bed).has_value());
}

TEST_CASE("The bed is hit on the plate and missed beside it", "[NavigatorGeometry]")
{
    const NavProjection p = navigator_projection(CanvasUi::Rect{0.f, 0.f, 200.f, 160.f}, projection_from(Vec3d(1., -1., 1.), Vec3d::UnitZ()).rotation);
    CHECK(navigator_bed_at(p, p.project(Vec3d(NAV_BED_HALF - 0.2, 0., NAV_BED_TOP))));
    CHECK_FALSE(navigator_bed_at(p, p.project(Vec3d(NAV_BED_HALF + 1., NAV_BED_HALF + 1., NAV_BED_TOP))));
    CHECK_FALSE(navigator_bed_at(p, Vec2d(-50., -50.)));
}

TEST_CASE("Turning to a zone looks at it with the view's up at the top, for every zone", "[NavigatorGeometry]")
{
    for (size_t zone = 0; zone < NAV_ZONE_COUNT; ++zone) {
        INFO("zone " << zone);
        const NavView       view   = navigator_zone_view(zone);
        const NavProjection p      = navigator_projection(CanvasUi::Rect{0.f, 0.f, 200.f, 160.f}, navigator_view_rotation(view).toRotationMatrix());
        const Vec2d         center = p.project(Vec3d::Zero());
        CHECK(std::abs(view.up.normalized().dot(view.to_camera.normalized())) < 1e-9);
        CHECK(view.to_camera.normalized().dot(navigator_cube_zones()[zone].normal) > 0.99);
        CHECK(navigator_zone_at(p, center) == int(zone));
        CHECK(p.project(view.up.normalized()).y() < center.y() - 1.);
        CHECK_THAT(p.project(view.up.normalized()).x(), WithinAbs(center.x(), 1e-6));
    }
}

TEST_CASE("Every zone has a name of its own", "[NavigatorGeometry]")
{
    std::vector<const NavZoneName*> names;
    for (size_t zone = 0; zone < NAV_ZONE_COUNT; ++zone) {
        const NavZoneName& name   = navigator_zone_name(zone);
        const Vec3d&       normal = navigator_cube_zones()[zone].normal;
        CHECK(Vec3d(name.x, name.y, name.z).normalized().dot(normal) > 0.99);
        names.push_back(&name);
    }
    std::sort(names.begin(), names.end());
    CHECK(std::unique(names.begin(), names.end()) == names.end());
}
