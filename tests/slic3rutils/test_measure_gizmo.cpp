// Which gripper the Measure gizmo treats as hovered, and which clicks it turns into a selection.
// wx brings in <windows.h>, which needs these defines first.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/Gizmos/GLGizmoMeasure.hpp"

using namespace Slic3r::GUI;
using PickState  = GLGizmoMeasure::PickState;
using GripperHit = GLGizmoMeasure::GripperHit;

namespace {
PickState hovering(int hover_id)
{
    PickState state;
    state.hover_id = hover_id;
    return state;
}
} // namespace

TEST_CASE("A feature gripper id has nothing to pick without a hovered feature", "[MeasureGizmo]")
{
    PickState state      = hovering(GENERATE(POINT_ID, EDGE_ID, CIRCLE_ID, PLANE_ID));
    state.sphere_1_live  = true;
    state.sphere_2_live  = true;
    CHECK_FALSE(GLGizmoMeasure::has_pick_target(state));
}

TEST_CASE("A hovered feature can be picked with any feature hover id", "[MeasureGizmo]")
{
    PickState state       = hovering(GENERATE(-1, POINT_ID, EDGE_ID, CIRCLE_ID, PLANE_ID));
    state.feature_hovered = true;
    CHECK(GLGizmoMeasure::has_pick_target(state));
}

TEST_CASE("Point selection needs the point on the hovered feature", "[MeasureGizmo]")
{
    PickState state       = hovering(EDGE_ID);
    state.feature_hovered = true;
    state.point_selection = true;
    CHECK_FALSE(GLGizmoMeasure::has_pick_target(state));

    state.has_point_on_feature = true;
    CHECK(GLGizmoMeasure::has_pick_target(state));
}

TEST_CASE("A selection sphere can be picked only while it is live", "[MeasureGizmo]")
{
    PickState first = hovering(SEL_SPHERE_1_ID);
    first.sphere_1_live = true;
    CHECK(GLGizmoMeasure::has_pick_target(first));

    PickState first_gone       = hovering(SEL_SPHERE_1_ID);
    first_gone.sphere_2_live   = true;
    first_gone.feature_hovered = true;
    CHECK_FALSE(GLGizmoMeasure::has_pick_target(first_gone));

    PickState second = hovering(SEL_SPHERE_2_ID);
    second.sphere_2_live = true;
    CHECK(GLGizmoMeasure::has_pick_target(second));

    PickState second_gone       = hovering(SEL_SPHERE_2_ID);
    second_gone.sphere_1_live   = true;
    second_gone.feature_hovered = true;
    CHECK_FALSE(GLGizmoMeasure::has_pick_target(second_gone));
}

TEST_CASE("No gripper hit leaves nothing hovered", "[MeasureGizmo]")
{
    CHECK(GLGizmoMeasure::pick_hover_id({}) == -1);
}

TEST_CASE("The closest gripper hit is hovered", "[MeasureGizmo]")
{
    CHECK(GLGizmoMeasure::pick_hover_id({{PLANE_ID, 9.0}, {EDGE_ID, 4.0}, {CIRCLE_ID, 6.0}}) == EDGE_ID);
}

TEST_CASE("The first selection sphere wins a tie with the second", "[MeasureGizmo]")
{
    CHECK(GLGizmoMeasure::pick_hover_id({{SEL_SPHERE_1_ID, 4.0}, {SEL_SPHERE_2_ID, 4.0}}) == SEL_SPHERE_1_ID);
}
