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
#include <cmath>

#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/Gizmos/GLGizmoRotate.hpp"

using namespace Slic3r;
using Slic3r::GUI::GLGizmoRotate;
using Catch::Matchers::WithinAbs;

TEST_CASE("Rotate gizmo measures the angle from the clicked point", "[GizmoRotate][Regression]")
{
    const double start_pointer = GENERATE(-0.2, 0.0, 0.2, 3.13, -3.13);
    const double start_angle = GENERATE(0.0, double(PI) / 2.0);
    const auto angle = [&](double delta) {
        const double pointer = start_pointer + delta;
        const Vec2d pos(12.0 * std::cos(pointer), 12.0 * std::sin(pointer));
        return GLGizmoRotate::calc_drag_angle(pos, start_pointer, start_angle, 3.0, 6.0, 10.0, 11.0);
    };
    const double full_turn = 2.0 * double(PI);
    const auto expected = [&](double delta) {
        double result = std::fmod(start_angle + delta, full_turn);
        return result < 0.0 ? result + full_turn : result;
    };
    CHECK_THAT(std::remainder(angle(0.0) - expected(0.0), full_turn), WithinAbs(0.0, 1e-10));
    CHECK_THAT(std::remainder(angle(0.01) - expected(0.01), full_turn), WithinAbs(0.0, 1e-10));
    CHECK_THAT(std::remainder(angle(-0.01) - expected(-0.01), full_turn), WithinAbs(0.0, 1e-10));
}

TEST_CASE("Rotate gizmo snaps corrected angles in the snap regions", "[GizmoRotate][Regression]")
{
    const double start_pointer = 0.2;
    const auto angle = [&](double radius, double delta) {
        const double pointer = start_pointer + delta;
        const Vec2d pos(radius * std::cos(pointer), radius * std::sin(pointer));
        return GLGizmoRotate::calc_drag_angle(pos, start_pointer, 0.0, 3.0, 6.0, 10.0, 11.0);
    };
    const double coarse_step = 2.0 * double(PI) / 8.0;
    const double fine_step = 2.0 * double(PI) / 72.0;
    CHECK_THAT(angle(4.0, 0.1), WithinAbs(0.0, 1e-10));
    CHECK_THAT(angle(4.0, coarse_step * 0.6), WithinAbs(coarse_step, 1e-10));
    CHECK_THAT(angle(10.5, fine_step * 0.6), WithinAbs(fine_step, 1e-10));
    CHECK_THAT(angle(10.5, -fine_step * 0.6), WithinAbs(2.0 * double(PI) - fine_step, 1e-10));
}
