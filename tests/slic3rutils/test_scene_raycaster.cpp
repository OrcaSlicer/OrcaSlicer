// This suite links libslic3r_gui; navigation raycasts need no wx application or GL context.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/SceneRaycaster.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {

Camera horizontal_camera(Camera::EType type = Camera::EType::Perspective)
{
    Camera camera;
    camera.set_type(type);
    camera.look_at({0.0, 0.0, 10.0}, {0.0, 100.0, 10.0}, Vec3d::UnitZ());
    camera.set_viewport(0, 0, 600, 600);
    camera.apply_projection(-1.0, 1.0, -1.0, 1.0, 1.0, 1000.0);
    return camera;
}

SceneRaycaster::HitResult scene_hit(const SceneRaycaster& scene, const Camera& camera, const Vec3d& point)
{
    return scene.hit(CameraUtils::project(camera, point).cast<double>(), camera, nullptr,
        SceneRaycaster::EHitMode::SceneOnly);
}

} // namespace

TEST_CASE("Navigation hits the visible bed below a horizontal perspective view", "[SceneRaycaster][Regression]")
{
    const MeshRaycaster bed(TriangleMesh(
        {{-100.f, 20.f, 0.f}, {100.f, 20.f, 0.f}, {100.f, 200.f, 0.f}, {-100.f, 200.f, 0.f}},
        {{0, 1, 2}, {0, 2, 3}}));
    SceneRaycaster scene;
    scene.add_raycaster(SceneRaycaster::EType::Bed, 0, bed, Transform3d::Identity());
    const Camera camera = horizontal_camera();

    const auto hit = scene_hit(scene, camera, {0.0, 100.0, 0.0});
    REQUIRE(hit.is_valid());
    CHECK(hit.type == SceneRaycaster::EType::Bed);
    CHECK_THAT(hit.position.z(), Catch::Matchers::WithinAbs(0.0, 1e-4));
}

TEST_CASE("Navigation hits a visible side face away from the perspective view center", "[SceneRaycaster][Regression]")
{
    const MeshRaycaster side(TriangleMesh(
        {{10.f, 20.f, -100.f}, {10.f, 20.f, 100.f}, {10.f, 200.f, 100.f}, {10.f, 200.f, -100.f}},
        {{0, 1, 2}, {0, 2, 3}}));
    SceneRaycaster scene;
    scene.add_raycaster(SceneRaycaster::EType::Volume, 0, side, Transform3d::Identity());
    const Camera camera = horizontal_camera();

    const auto hit = scene_hit(scene, camera, {10.0, 100.0, 10.0});
    REQUIRE(hit.is_valid());
    CHECK(hit.type == SceneRaycaster::EType::Volume);
    CHECK_THAT(hit.position.x(), Catch::Matchers::WithinAbs(10.0, 1e-4));
}

TEST_CASE("Navigation ignores gizmos and inactive volumes and chooses the nearest scene surface", "[SceneRaycaster]")
{
    const bool gizmos_on_top = GENERATE(false, true);
    const auto type = GENERATE(Camera::EType::Perspective, Camera::EType::Ortho);
    const MeshRaycaster cube(make_cube(20.0, 20.0, 20.0));
    SceneRaycaster scene;
    scene.set_gizmos_on_top(gizmos_on_top);
    scene.add_raycaster(SceneRaycaster::EType::Gizmo, 0, cube, Geometry::translation_transform({-10.0, 20.0, 0.0}));
    scene.add_raycaster(SceneRaycaster::EType::FallbackGizmo, 0, cube, Geometry::translation_transform({-10.0, 30.0, 0.0}));
    scene.add_raycaster(SceneRaycaster::EType::Volume, 0, cube, Geometry::translation_transform({-10.0, 40.0, 0.0}))->set_active(false);
    scene.add_raycaster(SceneRaycaster::EType::Volume, 1, cube, Geometry::translation_transform({-10.0, 150.0, 0.0}));
    scene.add_raycaster(SceneRaycaster::EType::Volume, 2, cube, Geometry::translation_transform({-10.0, 100.0, 0.0}));
    const Camera camera = horizontal_camera(type);

    const auto hit = scene_hit(scene, camera, {0.0, 100.0, 10.0});
    REQUIRE(hit.is_valid());
    CHECK(hit.type == SceneRaycaster::EType::Volume);
    CHECK(hit.raycaster_id == 2);
    CHECK_THAT(hit.position.y(), Catch::Matchers::WithinAbs(100.0, 1e-4));
}

TEST_CASE("Navigation respects the back-face policy away from the perspective view center", "[SceneRaycaster]")
{
    const bool use_back_faces = GENERATE(false, true);
    const MeshRaycaster side(TriangleMesh(
        {{10.f, 20.f, -100.f}, {10.f, 20.f, 100.f}, {10.f, 200.f, 100.f}, {10.f, 200.f, -100.f}},
        {{0, 2, 1}, {0, 3, 2}}));
    SceneRaycaster scene;
    scene.add_raycaster(SceneRaycaster::EType::Volume, 0, side, Transform3d::Identity(), use_back_faces);

    const auto hit = scene_hit(scene, horizontal_camera(), {10.0, 100.0, 10.0});
    CHECK(hit.is_valid() == use_back_faces);
}

TEST_CASE("Navigation ignores volume surfaces removed by the clipping plane", "[SceneRaycaster]")
{
    const MeshRaycaster cube(make_cube(20.0, 20.0, 20.0));
    SceneRaycaster scene;
    scene.add_raycaster(SceneRaycaster::EType::Volume, 0, cube, Geometry::translation_transform({-10.0, 100.0, 0.0}));
    scene.add_raycaster(SceneRaycaster::EType::Volume, 1, cube, Geometry::translation_transform({-10.0, 150.0, 0.0}));
    const Camera camera = horizontal_camera();
    const ClippingPlane clipping_plane(-Vec3d::UnitY(), -130.0);

    const auto hit = scene.hit({300.0, 300.0}, camera, &clipping_plane, SceneRaycaster::EHitMode::SceneOnly);
    REQUIRE(hit.is_valid());
    CHECK(hit.raycaster_id == 1);
    CHECK_THAT(hit.position.y(), Catch::Matchers::WithinAbs(150.0, 1e-4));
}
