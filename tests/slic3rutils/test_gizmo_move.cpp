#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    #include <CommCtrl.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Line.hpp"
#include "libslic3r/Point.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoMove.hpp"

using namespace Slic3r;
using Slic3r::GUI::GLGizmoMove3D;
using Catch::Matchers::WithinAbs;

TEST_CASE("Move gizmo measures displacement from the clicked point", "[GizmoMove][Regression]")
{
    const int axis = GENERATE(0, 1, 2, 3);
    const double offset = GENERATE(-2.0, 0.0, 2.0);
    Vec3d direction;
    if (axis == 3)
        direction = Vec3d(1.0, 1.0, 0.0).normalized();
    else
        direction = Vec3d::Unit(axis);
    const Vec3d view_direction = axis == 2 ? Vec3d::UnitY().eval() : Vec3d::UnitZ().eval();
    const Vec3d center(10.0, -20.0, 30.0);
    const Vec3d handle = center + 10.0 * direction;
    const auto ray_at = [&](double distance) {
        const Vec3d point = handle + distance * direction;
        return Linef3(point - 10.0 * view_direction, point + 10.0 * view_direction);
    };
    const Linef3 start = ray_at(offset);
    const auto displacement = [&](double distance, bool snap) {
        return GLGizmoMove3D::calc_drag_displacement(ray_at(offset + distance), start, handle, center, snap, 1.0);
    };

    CHECK_THAT(displacement(0.0, false), WithinAbs(0.0, 1e-10));
    CHECK_THAT(displacement(0.1, false), WithinAbs(0.1, 1e-10));
    CHECK_THAT(displacement(-0.1, false), WithinAbs(-0.1, 1e-10));
    CHECK_THAT(displacement(0.1, true), WithinAbs(0.0, 1e-10));
    CHECK_THAT(displacement(0.6, true), WithinAbs(1.0, 1e-10));
    CHECK_THAT(displacement(-0.6, true), WithinAbs(-1.0, 1e-10));
}

TEST_CASE("Move gizmo starts at zero with a perspective mouse ray", "[GizmoMove][Regression]")
{
    const Vec3d eye(0.0, -10.0, 10.0);
    const Vec3d handle(10.0, 0.0, 0.0);
    const Linef3 start(eye, Vec3d(12.0, 0.0, 0.0));
    const Linef3 moved(eye, Vec3d(12.1, 0.0, 0.0));

    const auto displacement = [&](const Linef3& ray) {
        return GLGizmoMove3D::calc_drag_displacement(ray, start, handle, Vec3d::Zero(), false, 1.0);
    };
    CHECK_THAT(displacement(start), WithinAbs(0.0, 1e-10));
    CHECK(displacement(moved) > 0.0);
    CHECK_THAT(displacement(start), WithinAbs(0.0, 1e-10));
}
