// Orca: This suite links libslic3r_gui; the Design tab's sketch tool needs no wx application or GL
// context until it renders.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    // Match the GUI precompiled header: wx/msw/wrapcctl.h needs HDITEM from CommCtrl.h.
    #include <CommCtrl.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/CAD/CadDocument.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/CAD/DesignSketchTool.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

SketchEntity circle(const Vec2d& c, double r)
{
    SketchEntity e;
    e.type   = SketchEntity::Type::Circle;
    e.center = e.p0 = c;
    e.radius = r;
    return e;
}

// Two committed sketches on XY: feature 1 a 10 mm circle at (50, 20), feature 2 a 5 mm one at (-60, 0).
void show_two_sketches(DesignSketchTool& tool)
{
    tool.set_display_sketches({ { { circle({ 50., 20. }, 10.) }, SketchPlane::XY(), 1 },
                                { { circle({ -60., 0. }, 5.) }, SketchPlane::XY(), 2 } });
}

// The Design tab's base planes, all through one origin, as DesignPanel shows them.
std::vector<SketchPlane> base_planes() { return { SketchPlane::XY(), SketchPlane::XZ(), SketchPlane::YZ() }; }

// The squares the base planes are drawn as, through a modeling origin at `origin`.
std::vector<SketchPlane> reference_squares(const Vec3d& origin, double half)
{
    std::vector<SketchPlane> squares;
    for (SketchPlane plane : base_planes()) {
        plane.origin += origin;
        squares.push_back(reference_square(plane, int(squares.size()), half));
    }
    return squares;
}

struct View
{
    Vec3d eye;
    Vec3d forward;
    bool  perspective;
};

// Eyes in four octants, off every plane, plus two orthographic directions.
const View kViews[] = {
    { Vec3d(300., -400., 250.), Vec3d(-300., 400., -250.).normalized(), true },
    { Vec3d(-350., -200., 300.), Vec3d(350., 200., -300.).normalized(), true },
    { Vec3d(250., 300., -200.), Vec3d(-250., -300., 200.).normalized(), true },
    { Vec3d(-300., 350., -250.), Vec3d(300., -350., 250.).normalized(), true },
    { Vec3d::Zero(), Vec3d(-0.5, 0.7, -0.5).normalized(), false },
    { Vec3d::Zero(), Vec3d(0.3, 0.4, 0.85).normalized(), false },
};

double area(const PlanePiece& piece, const SketchPlane& plane)
{
    Vec3d sum = Vec3d::Zero();
    for (size_t i = 0; i < piece.corners.size(); ++i)
        sum += piece.corners[i].cross(piece.corners[(i + 1) % piece.corners.size()]);
    return 0.5 * std::abs(sum.dot(plane.x_axis.cross(plane.y_axis)));
}

// How far along the ray (from, unit dir) it crosses `piece`, or nothing if it misses.
std::optional<double> hit_distance(const PlanePiece& piece, const SketchPlane& plane, const Vec3d& from, const Vec3d& dir)
{
    const Vec3d  n  = plane.x_axis.cross(plane.y_axis);
    const double dn = n.dot(dir);
    if (std::abs(dn) < 1e-9)
        return std::nullopt;
    const double t = n.dot(piece.corners.front() - from) / dn;
    if (t <= 0.)
        return std::nullopt;
    const Vec3d x  = from + dir * t;
    double      lo = 0., hi = 0.;
    for (size_t i = 0; i < piece.corners.size(); ++i) {
        const Vec3d& a = piece.corners[i];
        const Vec3d& b = piece.corners[(i + 1) % piece.corners.size()];
        const double s = (b - a).cross(x - a).dot(n);
        lo = std::min(lo, s);
        hi = std::max(hi, s);
    }
    if (lo < 0. && hi > 0.)
        return std::nullopt;   // outside one of the edges
    return t;
}

struct SightLines
{
    int overlapping  = 0;   // sight lines through two or more pieces, where draw order matters
    int out_of_order = 0;   // ...of which meet a nearer piece before a farther one
};

// Translucent pieces blend correctly only if, along every line of sight, each piece is drawn after
// every piece behind it. The lines of sight aim at points of the box [lo, hi].
SightLines sight_lines(const std::vector<SketchPlane>& planes, double half, const View& view, const Vec3d& lo, const Vec3d& hi)
{
    const std::vector<PlanePiece> pieces = planes_back_to_front(planes, half, view.eye, view.forward, view.perspective);
    std::mt19937                           rng(7);
    std::uniform_real_distribution<double> unit(0., 1.);
    SightLines                             seen;
    for (int r = 0; r < 500; ++r) {
        const Vec3d target = lo + (hi - lo).cwiseProduct(Vec3d(unit(rng), unit(rng), unit(rng)));
        const Vec3d from = view.perspective ? view.eye : Vec3d(target - view.forward * (10. * half));
        const Vec3d dir  = (target - from).normalized();
        double      last = std::numeric_limits<double>::max();
        int         hits = 0;
        bool        ok   = true;
        for (const PlanePiece& piece : pieces)
            if (const std::optional<double> t = hit_distance(piece, planes[piece.plane], from, dir)) {
                ok   = ok && *t <= last + 1e-6 * half;
                last = *t;
                ++hits;
            }
        if (hits >= 2) {
            ++seen.overlapping;
            seen.out_of_order += ok ? 0 : 1;
        }
    }
    return seen;
}

} // namespace

TEST_CASE("Fit frames the picked sketch region, not the other sketches", "[DesignSketchTool]")
{
    DesignSketchTool   tool;
    GLVolumeCollection no_bodies;
    show_two_sketches(tool);
    tool.set_display_pick(1, 0);

    const BoundingBoxf3 box = tool.fit_box(no_bodies);
    REQUIRE(box.defined);
    CHECK_THAT(box.min.x(), WithinAbs(40., 0.5));
    CHECK_THAT(box.max.x(), WithinAbs(60., 0.5));
    CHECK_THAT(box.min.y(), WithinAbs(10., 0.5));
    CHECK_THAT(box.max.y(), WithinAbs(30., 0.5));
    // Zoom to selection frames the same region, the padding left to the canvas.
    CHECK_THAT(tool.selection_box().min.x(), WithinAbs(40., 0.5));
    CHECK_THAT(tool.selection_box().size().z(), WithinAbs(0., 1e-9));
}

TEST_CASE("Fit frames every sketch when nothing is picked", "[DesignSketchTool]")
{
    DesignSketchTool   tool;
    GLVolumeCollection no_bodies;
    show_two_sketches(tool);

    const BoundingBoxf3 box = tool.fit_box(no_bodies);
    REQUIRE(box.defined);
    CHECK_THAT(box.min.x(), WithinAbs(-65., 0.5));
    CHECK_THAT(box.max.x(), WithinAbs(60., 0.5));
    // Zoom to selection has no such fallback.
    CHECK_FALSE(tool.selection_box().defined);
}

TEST_CASE("Fit frames the sketch being drawn along with the committed ones", "[DesignSketchTool]")
{
    DesignSketchTool   tool;
    GLVolumeCollection no_bodies;
    show_two_sketches(tool);
    // A 100 mm line up the XZ plane, whose y axis is world Z.
    SketchEntity line;
    line.p0 = { 0., 0. };
    line.p1 = { 0., 100. };
    tool.begin_edit({ line }, {}, SketchPlane::XZ());

    const BoundingBoxf3 box = tool.fit_box(no_bodies);
    REQUIRE(box.defined);
    CHECK_THAT(box.max.z(), WithinAbs(100., 1e-6));
    CHECK_THAT(box.min.x(), WithinAbs(-65., 0.5));
}

TEST_CASE("Fit gives a flat sketch depth, so it can be framed edge-on", "[DesignSketchTool]")
{
    DesignSketchTool   tool;
    GLVolumeCollection no_bodies;
    show_two_sketches(tool);
    tool.set_display_pick(2, 0);

    const BoundingBoxf3 box = tool.fit_box(no_bodies);
    REQUIRE(box.defined);
    CHECK(box.size().z() > 0.);
    CHECK_THAT(box.center().z(), WithinAbs(0., 1e-9));
}

TEST_CASE("Fit frames nothing when the Design tab shows nothing", "[DesignSketchTool]")
{
    DesignSketchTool   tool;
    GLVolumeCollection no_bodies;
    CHECK_FALSE(tool.fit_box(no_bodies).defined);
}

TEST_CASE("A sketch's box lies on its own plane", "[DesignSketchTool]")
{
    // A 5 mm circle at (10, 20) on XZ, whose y axis is world Z.
    const BoundingBoxf3 box = DesignSketchTool::sketch_box({ circle({ 10., 20. }, 5.) }, SketchPlane::XZ());
    REQUIRE(box.defined);
    CHECK_THAT(box.min.x(), WithinAbs(5., 0.05));
    CHECK_THAT(box.max.x(), WithinAbs(15., 0.05));
    CHECK_THAT(box.min.z(), WithinAbs(15., 0.05));
    CHECK_THAT(box.max.z(), WithinAbs(25., 0.05));
    CHECK_THAT(box.size().y(), WithinAbs(0., 1e-9));
    CHECK_FALSE(DesignSketchTool::sketch_box({}, SketchPlane::XY()).defined);

    SketchEntity point;
    point.type = SketchEntity::Type::Point;
    point.p0   = { 3., 4. };
    const BoundingBoxf3 dot = DesignSketchTool::sketch_box({ point }, SketchPlane::XY());
    REQUIRE(dot.defined);
    CHECK_THAT(dot.min.x(), WithinAbs(3., 1e-9));
    CHECK_THAT(dot.min.y(), WithinAbs(4., 1e-9));
}

TEST_CASE("A face's box comes from the pick mesh, on a hidden body too", "[DesignSketchTool]")
{
    // One triangle per body: body 0 near the origin, body 1, hidden, 100 mm along X.
    indexed_triangle_set its;
    its.vertices = { { 0.f, 0.f, 0.f }, { 10.f, 0.f, 0.f }, { 0.f, 10.f, 5.f },
                     { 100.f, 0.f, 0.f }, { 110.f, 0.f, 0.f }, { 100.f, 10.f, 5.f } };
    its.indices  = { { 0, 1, 2 }, { 3, 4, 5 } };
    const TriangleMesh          mesh(its);
    const std::vector<CadBody>  bodies(2);
    const std::vector<int>      tri_face{ 0, 0 };
    const std::vector<int>      tri_body{ 0, 1 };
    const std::vector<bool>     visible{ true, false };
    DesignSketchTool            tool;
    tool.set_solid_pick(&bodies, &mesh, &tri_face, &tri_body, &visible);

    const BoundingBoxf3 box = tool.faces_box({ { 1, 0 } });
    REQUIRE(box.defined);
    CHECK_THAT(box.min.x(), WithinAbs(100., 1e-6));
    CHECK_THAT(box.max.x(), WithinAbs(110., 1e-6));
    CHECK_THAT(box.max.z(), WithinAbs(5., 1e-6));
    CHECK_FALSE(tool.faces_box({}).defined);

    // CadDocument::faces_made_by may list a body's faces after a later body's.
    const BoundingBoxf3 both = tool.faces_box({ { 1, 0 }, { 0, 0 } });
    REQUIRE(both.defined);
    CHECK_THAT(both.min.x(), WithinAbs(0., 1e-6));
    CHECK_THAT(both.max.x(), WithinAbs(110., 1e-6));
}

TEST_CASE("Padding a box to frame gives every axis a small extent and leaves a long one alone", "[DesignSketchTool]")
{
    BoundingBoxf3 box;
    box.merge(Vec3d(0., 0., 3.));
    box.merge(Vec3d(100., 4., 3.));
    DesignSketchTool::pad_box(box);
    CHECK_THAT(box.size().x(), WithinAbs(100., 1e-9));
    CHECK_THAT(box.size().y(), WithinAbs(10., 1e-9));
    CHECK_THAT(box.size().z(), WithinAbs(10., 1e-9));
    CHECK_THAT(box.center().z(), WithinAbs(3., 1e-9));

    BoundingBoxf3 none;
    DesignSketchTool::pad_box(none);
    CHECK_FALSE(none.defined);
}

TEST_CASE("Crossing base planes are drawn back to front from any viewpoint", "[DesignSketchTool]")
{
    const View&      view = kViews[GENERATE(range(0, int(std::size(kViews))))];
    const SightLines seen = sight_lines(base_planes(), 75., view, Vec3d::Constant(-0.95 * 75.), Vec3d::Constant(0.95 * 75.));
    CHECK(seen.overlapping >= 200);   // of the 500: most lines of sight into the planes cross two
    CHECK(seen.out_of_order == 0);
}

TEST_CASE("Datum planes are drawn back to front among the base planes", "[DesignSketchTool]")
{
    // A datum parallel to XY 30 mm up, and one tilted 30 degrees about X through (0, 0, 10).
    std::vector<SketchPlane> planes = base_planes();
    SketchPlane              raised = SketchPlane::XY();
    raised.origin                   = Vec3d(0., 0., 30.);
    SketchPlane tilted;
    tilted.origin = Vec3d(0., 0., 10.);
    tilted.y_axis = Vec3d(0., std::cos(M_PI / 6.), std::sin(M_PI / 6.));
    tilted.normal = tilted.x_axis.cross(tilted.y_axis);
    planes.push_back(raised);
    planes.push_back(tilted);

    const View&      view = kViews[GENERATE(range(0, int(std::size(kViews))))];
    const SightLines seen = sight_lines(planes, 75., view, Vec3d::Constant(-0.95 * 75.), Vec3d::Constant(0.95 * 75.));
    CHECK(seen.overlapping >= 200);
    CHECK(seen.out_of_order == 0);
}

TEST_CASE("Cutting the base planes along each other keeps every plane whole", "[DesignSketchTool]")
{
    const std::vector<SketchPlane> planes = base_planes();
    const double                   half   = 75.;
    const View&                    view   = kViews[0];
    std::vector<double>            covered(planes.size(), 0.);
    for (const PlanePiece& piece : planes_back_to_front(planes, half, view.eye, view.forward, view.perspective))
        covered[piece.plane] += area(piece, planes[piece.plane]);
    for (double a : covered)
        CHECK_THAT(a, WithinRel(4. * half * half, 1e-9));
}

TEST_CASE("Base reference planes sit in the octant their names face, clear of the axes", "[DesignSketchTool]")
{
    // Each square is flat in one coordinate and at least `gap` out along the other two, toward
    // (+X, -Y, +Z), so no two of them meet: a point of XY has z = 0, every point of XZ and YZ has
    // z >= gap.
    const Vec3d                    origin(128., 128., 0.), octant(1., -1., 1.);
    const double                   half = 32., gap = reference_square_gap(half);
    const std::vector<SketchPlane> squares = reference_squares(origin, half);
    const int                      flat[] = { 2, 1, 0 };   // XY in z, XZ in y, YZ in x
    for (int i = 0; i < 3; ++i) {
        // The side a name reads from faces into the same octant.
        CHECK(squares[i].x_axis.cross(squares[i].y_axis).dot(octant) > 0.);
        for (const Vec2d& corner : { Vec2d(-half, -half), Vec2d(half, -half), Vec2d(half, half), Vec2d(-half, half) }) {
            const Vec3d x = squares[i].to_world(corner) - origin;
            for (int k = 0; k < 3; ++k)
                if (k == flat[i])
                    CHECK_THAT(x[k], WithinAbs(0., 1e-9));
                else {
                    CHECK(octant[k] * x[k] >= gap - 1e-9);
                    CHECK(octant[k] * x[k] <= gap + 2. * half + 1e-9);
                }
        }
    }
}

TEST_CASE("From the front no base reference plane stands behind another", "[DesignSketchTool]")
{
    // Looking from the front and above, as the Design tab opens: XY lies below the X axis on screen
    // and XZ above it, and YZ is seen edge-on, so no line of sight crosses two squares.
    const double                   half = 32., e = reference_square_gap(half) + 2. * half;
    const std::vector<SketchPlane> squares = reference_squares(Vec3d::Zero(), half);
    const double                   elevation = M_PI / 180. * GENERATE(20., 45., 70.);
    const View                     front{ Vec3d::Zero(), Vec3d(0., std::cos(elevation), -std::sin(elevation)), false };
    CHECK(sight_lines(squares, half, front, Vec3d(0., -e, 0.), Vec3d(e, 0., e)).overlapping == 0);
}

TEST_CASE("A datum's reference square stays centred on its origin", "[DesignSketchTool]")
{
    SketchPlane datum = SketchPlane::XY();
    datum.origin      = Vec3d(10., 20., 30.);
    CHECK((reference_square(datum, 3, 32.).origin - datum.origin).norm() < 1e-12);
}

TEST_CASE("Base reference planes are drawn back to front without being cut", "[DesignSketchTool]")
{
    const double                   half    = 32.;
    const std::vector<SketchPlane> squares = reference_squares(Vec3d::Zero(), half);
    int                            overlapping = 0;
    for (const View& view : kViews) {
        const std::vector<PlanePiece> pieces = planes_back_to_front(squares, half, view.eye, view.forward, view.perspective);
        CHECK(pieces.size() == 3);
        for (const PlanePiece& piece : pieces)
            CHECK(piece.corners.size() == 4);
        const double     e    = reference_square_gap(half) + 2. * half;
        const SightLines seen = sight_lines(squares, half, view, Vec3d(0., -e, 0.), Vec3d(e, 0., e));
        CHECK(seen.out_of_order == 0);
        overlapping += seen.overlapping;
    }
    // These views do see squares behind one another, so the order is actually put to the test.
    CHECK(overlapping >= 50);
}

TEST_CASE("A click takes the nearest reference square along the ray", "[DesignSketchTool]")
{
    const double                   half    = 32.;   // the squares span 8..72 out along both their axes
    const std::vector<SketchPlane> squares = reference_squares(Vec3d::Zero(), half);
    // From the front left through YZ (x = 0) at (0, -20, 40), then on through XZ (y = 0) at (20, 0, 40).
    const Vec3d along(1., 1., 0.);
    CHECK(pick_reference_square(squares, half, Vec3d(-40., -60., 40.), along) == 2);
    // The same line walked the other way meets XZ first.
    CHECK(pick_reference_square(squares, half, Vec3d(60., 40., 40.), -along) == 1);
    // Straight down onto XY, and down the gap beside it, which holds nothing.
    CHECK(pick_reference_square(squares, half, Vec3d(40., -40., 100.), Vec3d(0., 0., -1.)) == 0);
    CHECK(pick_reference_square(squares, half, Vec3d(4., -40., 100.), Vec3d(0., 0., -1.)) == -1);
    // Looking away from the squares.
    CHECK(pick_reference_square(squares, half, Vec3d(40., -40., 100.), Vec3d(0., 0., 1.)) == -1);
}

TEST_CASE("A base plane's name lies inside its own square", "[DesignSketchTool]")
{
    const double                   half    = 32.;
    const std::vector<SketchPlane> squares = reference_squares(Vec3d(128., 128., 0.), half);
    const char*                    names[] = { "XY", "XZ", "YZ" };
    for (int i = 0; i < 3; ++i)
        for (const Vec3d& corner : reference_label_box(squares[i], half, names[i])) {
            const Vec3d d = corner - squares[i].origin;
            CHECK_THAT(d.dot(squares[i].x_axis.cross(squares[i].y_axis)), WithinAbs(0., 1e-9));
            CHECK(std::abs(d.dot(squares[i].x_axis)) < half);
            CHECK(std::abs(d.dot(squares[i].y_axis)) < half);
        }
}

TEST_CASE("A move arrow drag moves the body by the cursor's travel, wherever the arrow is grabbed", "[DesignSketchTool]")
{
    DesignSketchTool tool;
    Transform3d      moved = Transform3d::Identity();
    tool.on_body_move_changed = [&moved](int, const Transform3d& xform) { moved = xform; };
    tool.set_move_gizmo(0, Vec3d::Zero(), Transform3d::Identity(), 10.);
    // Looking straight down onto the X arrow, the cursor over x = `x` on it.
    const auto ray_at = [](double x) { return Linef3(Vec3d(x, 0., 100.), Vec3d(x, 0., 0.)); };

    // Grabbed 12 mm out from the body centre: a 1 mm move moves the body 1 mm, not 13.
    tool.grab_move_arrow(0, ray_at(12.));
    tool.drag_move_arrow(ray_at(13.));
    CHECK_THAT(moved.translation().x(), WithinAbs(1., 1e-9));
    tool.drag_move_arrow(ray_at(17.));
    CHECK_THAT(moved.translation().x(), WithinAbs(5., 1e-9));

    // The next drag carries on from where the body was left: grabbed 3 mm past it, moved 2 mm.
    tool.grab_move_arrow(0, ray_at(8.));
    tool.drag_move_arrow(ray_at(10.));
    CHECK_THAT(moved.translation().x(), WithinAbs(7., 1e-9));
}

TEST_CASE("A move arrow pressed while looking down its axis does not jump on the first move", "[DesignSketchTool]")
{
    DesignSketchTool tool;
    Transform3d      moved = Transform3d::Identity();
    tool.on_body_move_changed = [&moved](int, const Transform3d& xform) { moved = xform; };
    tool.set_move_gizmo(0, Vec3d::Zero(), Transform3d::Identity(), 10.);
    const auto ray_at = [](double x) { return Linef3(Vec3d(x, 0., 100.), Vec3d(x, 0., 0.)); };

    // The press projects nowhere on the X axis, so the first move only finds where it was grabbed.
    tool.grab_move_arrow(0, Linef3(Vec3d(100., 0., 0.), Vec3d::Zero()));
    tool.drag_move_arrow(ray_at(13.));
    CHECK_THAT(moved.translation().x(), WithinAbs(0., 1e-9));
    tool.drag_move_arrow(ray_at(15.));
    CHECK_THAT(moved.translation().x(), WithinAbs(2., 1e-9));
}
