// Orca: This suite links libslic3r_gui; the camera math needs no wx application or GL context.
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
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <string>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "slic3r/GUI/Camera.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

const BoundingBoxf3 bed_box{Vec3d(-128.0, -128.0, 0.0), Vec3d(128.0, 128.0, 256.0)};
const Vec3d         scene_center(0.0, 0.0, 128.0);
constexpr int       viewport_size = 1000;

Camera dolly_camera(double zoom)
{
    Camera camera;
    camera.set_dolly_enabled(true);
    camera.set_type(Camera::EType::Perspective);
    camera.set_viewport(0, 0, viewport_size, viewport_size);
    camera.set_target(scene_center);
    camera.set_zoom(zoom);
    camera.apply_projection(bed_box);
    return camera;
}

// Pixels from the viewport center where a world point is drawn.
Vec2d to_screen(const Camera& camera, const Vec3d& point)
{
    const Eigen::Vector4d clip = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix() *
                                 Eigen::Vector4d(point.x(), point.y(), point.z(), 1.0);
    return 0.5 * viewport_size * Vec2d(clip.x(), clip.y()) / clip.w();
}

} // namespace

TEST_CASE("Zooming without dolly changes the field of view at a fixed distance", "[Camera]")
{
    Camera camera;
    camera.set_type(Camera::EType::Perspective);
    camera.set_viewport(0, 0, viewport_size, viewport_size);
    camera.set_target(scene_center);

    camera.set_zoom(1.0);
    camera.apply_projection(bed_box);
    const double far_distance = camera.get_distance();
    const double far_fov      = camera.get_fov();

    camera.set_zoom(10.0);
    camera.apply_projection(bed_box);
    CHECK_THAT(camera.get_distance(), WithinRel(far_distance, 1e-9));
    CHECK(camera.get_fov() < 0.2 * far_fov);
}

TEST_CASE("Zooming with dolly moves the camera and keeps the field of view", "[Camera]")
{
    const double zoom   = GENERATE(0.76, 3.8, 20.0, 250.0);
    Camera       camera = dolly_camera(1.0);
    const double start_distance = camera.get_distance();

    camera.set_zoom(zoom);
    camera.apply_projection(bed_box);

    CHECK_THAT(camera.get_fov(), WithinRel(Camera::DollyFovDeg, 1e-9));
    CHECK_THAT(camera.get_distance(), WithinRel(start_distance / zoom, 1e-9));
    CHECK_THAT((camera.get_target() - scene_center).norm(), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Dolly keeps the zoom scale on the target plane", "[Camera]")
{
    const double zoom   = GENERATE(0.76, 3.8, 20.0, 250.0);
    Camera       camera = dolly_camera(zoom);
    // Small enough to stay in the view at the maximum zoom.
    const double offset = 1.0;

    const Vec2d screen = to_screen(camera, camera.get_target() + offset * camera.get_dir_right());

    CHECK_THAT(screen.x(), WithinRel(offset * zoom, 1e-6));
    CHECK_THAT(screen.y(), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Dolly gets closer than the legacy near plane limit without being pushed back", "[Camera]")
{
    // The camera is inside the scene box here.
    Camera camera = dolly_camera(100.0);

    CHECK(camera.get_distance() < Camera::FrustrumMinNearZ);
    CHECK(camera.get_near_z() > 0.0);
    CHECK(camera.get_near_z() < camera.get_distance());
    CHECK(camera.get_far_z() > camera.get_near_z());
}

TEST_CASE("Dolly keeps a valid frustum when the scene is behind the camera", "[Camera]")
{
    Camera camera = dolly_camera(100.0);
    const Vec3d behind = camera.get_position() - 500.0 * camera.get_dir_forward();
    const BoundingBoxf3 box{behind - Vec3d::Ones(), behind + Vec3d::Ones()};

    camera.apply_projection(box);

    CHECK(camera.get_near_z() > 0.0);
    CHECK(camera.get_far_z() > camera.get_near_z());
}

TEST_CASE("Switching between dolly and orthographic keeps the view", "[Camera]")
{
    Camera camera = dolly_camera(20.0);
    const Vec3d  forward        = camera.get_dir_forward();
    const double dolly_distance = camera.get_distance();
    const Vec3d  point          = camera.get_target() + 3.0 * camera.get_dir_right() - 2.0 * camera.get_dir_up();
    const Vec2d  dolly_screen   = to_screen(camera, point);

    camera.set_type(Camera::EType::Ortho);
    camera.apply_projection(bed_box);

    CHECK_THAT((camera.get_dir_forward() - forward).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT((camera.get_target() - scene_center).norm(), WithinAbs(0.0, 1e-6));
    CHECK_THAT((to_screen(camera, point) - dolly_screen).norm(), WithinAbs(0.0, 1e-6));

    camera.set_type(Camera::EType::Perspective);
    camera.apply_projection(bed_box);

    CHECK_THAT(camera.get_distance(), WithinRel(dolly_distance, 1e-9));
    CHECK_THAT((camera.get_target() - scene_center).norm(), WithinAbs(0.0, 1e-6));
    CHECK_THAT((to_screen(camera, point) - dolly_screen).norm(), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Zooming to the mouse with dolly keeps the target plane point under the mouse", "[Camera]")
{
    Camera camera = dolly_camera(5.0);
    const Vec3d point    = camera.get_target() + 20.0 * camera.get_dir_right() + 10.0 * camera.get_dir_up();
    const Vec2d start    = to_screen(camera, point);
    const Vec3d offset   = camera.get_inv_zoom() * (start.x() * camera.get_dir_right() + start.y() * camera.get_dir_up());
    const double zoom    = camera.get_zoom();

    camera.translate(offset);
    camera.set_zoom(zoom * 2.0);
    camera.translate(-offset / 2.0);
    camera.apply_projection(bed_box);

    CHECK_THAT((to_screen(camera, point) - start).norm(), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Zooming to a box with dolly keeps every corner in the view", "[Camera]")
{
    const std::string   view = GENERATE(as<std::string>{}, "iso", "topfront", "top");
    const int           width = GENERATE(1000, 1600);
    const BoundingBoxf3 box{Vec3d(0.0, 0.0, 0.0), Vec3d(200.0, 200.0, 100.0)};

    Camera camera;
    camera.set_dolly_enabled(true);
    camera.set_type(Camera::EType::Perspective);
    camera.set_viewport(0, 0, width, viewport_size);
    camera.select_view(view);
    camera.zoom_to_box(box);
    camera.apply_projection(box);

    for (int i = 0; i < 8; ++i) {
        const Vec3d corner((i & 1) ? box.max.x() : box.min.x(), (i & 2) ? box.max.y() : box.min.y(), (i & 4) ? box.max.z() : box.min.z());
        const Vec2d screen = to_screen(camera, corner);
        // to_screen scales both axes by the viewport height.
        CHECK(std::abs(screen.x()) <= 0.5 * width);
        CHECK(std::abs(screen.y()) <= 0.5 * viewport_size);
    }
}

TEST_CASE("Turning dolly off restores the legacy camera distance", "[Camera]")
{
    Camera camera = dolly_camera(20.0);
    const Vec3d target = camera.get_target();
    REQUIRE(camera.get_distance() < Camera::DefaultDistance);

    camera.set_dolly_enabled(false);
    camera.apply_projection(bed_box);

    CHECK_THAT(camera.get_distance(), WithinRel(Camera::DefaultDistance, 1e-9));
    CHECK_THAT((camera.get_target() - target).norm(), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Pixel size keeps a fixed screen size at every depth in dolly mode", "[Camera]")
{
    const double depth_offset = GENERATE(-3.0, 0.0, 50.0, 400.0);
    Camera       camera       = dolly_camera(100.0);
    const Vec3d  point        = camera.get_target() + depth_offset * camera.get_dir_forward();
    const double pixels       = 16.0;

    const Vec3d offset = pixels * camera.get_pixel_size_at(point) * camera.get_dir_right();
    const Vec2d screen = to_screen(camera, point + offset) - to_screen(camera, point);

    CHECK_THAT(screen.x(), WithinRel(pixels, 1e-6));
}

TEST_CASE("Pixel size is the inverse zoom without dolly", "[Camera]")
{
    const bool dolly_enabled = GENERATE(false, true);
    Camera     camera;
    camera.set_dolly_enabled(dolly_enabled);
    camera.set_type(dolly_enabled ? Camera::EType::Ortho : Camera::EType::Perspective);
    camera.set_viewport(0, 0, viewport_size, viewport_size);
    camera.set_zoom(5.0);
    camera.apply_projection(bed_box);

    CHECK_THAT(camera.get_pixel_size_at(camera.get_position() + 123.0 * camera.get_dir_forward()), WithinRel(camera.get_inv_zoom(), 1e-12));
}

TEST_CASE("Moving the dolly target keeps the camera and the picture", "[Camera]")
{
    const double depth    = GENERATE(0.0, 2.5, 40.0, 900.0);
    Camera       camera   = dolly_camera(5.0);
    const Vec3d  position = camera.get_position();
    const Vec3d  point    = camera.get_target() + 20.0 * camera.get_dir_right() + 10.0 * camera.get_dir_up();
    const Vec2d  screen   = to_screen(camera, point);

    camera.set_dolly_target_depth(depth);
    camera.apply_projection(bed_box);

    CHECK_THAT((camera.get_position() - position).norm(), WithinAbs(0.0, 1e-6));
    CHECK_THAT((to_screen(camera, point) - screen).norm(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(camera.get_distance(), WithinRel(std::max(depth, Camera::DollyMinTargetDepth), 1e-9));
}

TEST_CASE("Moving the dolly target respects the zoom out limit without moving the camera", "[Camera]")
{
    Camera camera = dolly_camera(5.0);
    camera.set_scene_box(bed_box);
    const Vec3d position = camera.get_position();

    camera.set_dolly_target_depth(1.0e6);
    camera.apply_projection(bed_box);

    CHECK_THAT(camera.get_zoom(), WithinRel(camera.min_zoom(), 1e-9));
    CHECK_THAT((camera.get_position() - position).norm(), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Dolly speed follows the depth of the surface and never stops", "[Camera]")
{
    const double delta   = GENERATE(0.1, 1.0, 4.0);
    Camera       camera  = dolly_camera(1.0);
    const Vec3d  forward = camera.get_dir_forward();
    // A wall far in front of the old target, as when flying up to a part after zooming in elsewhere.
    const Vec3d  wall    = camera.get_position() + 300.0 * forward;

    bool passed_wall = false;
    for (int i = 0; i < 20000 && !passed_wall; ++i) {
        const Vec3d  position = camera.get_position();
        const double depth    = (wall - position).dot(forward);

        camera.set_dolly_target_depth(depth);
        camera.update_zoom(delta);
        camera.apply_projection(bed_box);

        const double step   = (camera.get_position() - position).dot(forward);
        const double target = std::max(depth, Camera::DollyMinTargetDepth);
        REQUIRE_THAT(step, WithinRel(target * delta * Camera::ZoomUnit, 1e-6));
        passed_wall = depth <= 0.0;
    }

    CHECK(passed_wall);
}

TEST_CASE("Dolly can move back from the smallest target depth", "[Camera]")
{
    Camera      camera   = dolly_camera(5.0);
    camera.set_dolly_target_depth(0.0);
    camera.apply_projection(bed_box);
    const Vec3d position = camera.get_position();

    camera.update_zoom(-1.0);
    camera.apply_projection(bed_box);

    CHECK((camera.get_position() - position).dot(camera.get_dir_forward()) < 0.0);
}

TEST_CASE("Leaving dolly zoomed in beyond the legacy limit does not jump on the next zoom step", "[Camera]")
{
    Camera camera = dolly_camera(5.0);
    camera.set_dolly_target_depth(0.0);
    const double zoom = camera.get_zoom();
    REQUIRE(zoom > 250.0);

    SECTION("Orthographic zooms out smoothly") {
        camera.set_type(Camera::EType::Ortho);
        camera.update_zoom(-1.0);
        CHECK_THAT(camera.get_zoom(), WithinRel(zoom / 1.1, 1e-9));
    }
    SECTION("Turning dolly off returns to the legacy limit") {
        camera.set_dolly_enabled(false);
        CHECK_THAT(camera.get_zoom(), WithinRel(250.0, 1e-12));
        camera.set_zoom(1.0e6);
        CHECK_THAT(camera.get_zoom(), WithinRel(250.0, 1e-12));
    }
}

TEST_CASE("Loading a camera view keeps the target when the distances differ", "[Camera]")
{
    Camera source = dolly_camera(20.0);
    Camera stored;
    stored.set_viewport(0, 0, viewport_size, viewport_size);

    stored.load_camera_view(source);

    CHECK_THAT((stored.get_target() - scene_center).norm(), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Changing the viewport height with dolly keeps the camera in place", "[Camera]")
{
    const unsigned int height   = GENERATE(10u, 500u, 2000u);
    Camera             camera   = dolly_camera(5.0);
    const Vec3d        position = camera.get_position();

    camera.set_viewport(0, 0, viewport_size, height);
    camera.apply_projection(bed_box);

    CHECK_THAT((camera.get_position() - position).norm(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(camera.get_zoom(), WithinRel(5.0 * height / viewport_size, 1e-9));
}

TEST_CASE("Restoring the viewport height with dolly restores the zoom", "[Camera]")
{
    Camera camera = dolly_camera(5.0);

    camera.set_viewport(0, 0, viewport_size, 10);
    camera.set_viewport(0, 0, viewport_size, viewport_size);

    CHECK_THAT(camera.get_zoom(), WithinRel(5.0, 1e-9));
}

TEST_CASE("Changing the viewport width with dolly keeps the zoom", "[Camera]")
{
    Camera camera = dolly_camera(5.0);

    camera.set_viewport(0, 0, 2 * viewport_size, viewport_size);

    CHECK_THAT(camera.get_zoom(), WithinRel(5.0, 1e-9));
}

TEST_CASE("Changing the viewport height keeps the zoom in orthographic view", "[Camera]")
{
    Camera camera = dolly_camera(5.0);
    camera.set_type(Camera::EType::Ortho);

    camera.set_viewport(0, 0, viewport_size, viewport_size / 2);

    CHECK_THAT(camera.get_zoom(), WithinRel(5.0, 1e-9));
}

TEST_CASE("Changing the viewport height keeps the zoom without dolly", "[Camera]")
{
    Camera camera;
    camera.set_type(Camera::EType::Perspective);
    camera.set_viewport(0, 0, viewport_size, viewport_size);
    camera.set_zoom(5.0);

    camera.set_viewport(0, 0, viewport_size, viewport_size / 2);

    CHECK_THAT(camera.get_zoom(), WithinRel(5.0, 1e-9));
}

TEST_CASE("Syncing the dolly distance applies a zoom that has not been rendered", "[Camera]")
{
    Camera camera = dolly_camera(5.0);
    camera.set_zoom(10.0);
    Camera rendered = dolly_camera(10.0);

    camera.sync_dolly_distance();

    CHECK_THAT((camera.get_position() - rendered.get_position()).norm(), WithinAbs(0.0, 1e-6));
}

TEST_CASE("A camera that never used dolly keeps the legacy zoom limit", "[Camera]")
{
    Camera camera;
    camera.set_type(Camera::EType::Perspective);
    camera.set_viewport(0, 0, viewport_size, viewport_size);

    camera.set_zoom(10000.0);

    CHECK_THAT(camera.get_zoom(), WithinRel(250.0, 1e-9));
}
