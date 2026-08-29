#include <catch2/catch_all.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <slic3r/GUI/ToolpathMeshBuilder.hpp>
#include <libvgcode/include/GCodeInputData.hpp>

// libvgcode::Viewer loading owns OpenGL resources. This focused test only needs its normalized
// CPU vertices and range state, so expose those internals without requiring a test GL context.
#define private public
#include <libvgcode/include/Viewer.hpp>
#include "../../src/libvgcode/src/ViewerImpl.hpp"
#undef private

namespace {

libvgcode::PathVertex extrusion(float x, uint32_t gcode_id)
{
    libvgcode::PathVertex vertex;
    vertex.position = {x, 0.0f, 0.2f};
    vertex.width = 0.45f;
    vertex.height = 0.2f;
    vertex.type = libvgcode::EMoveType::Extrude;
    vertex.role = libvgcode::EGCodeExtrusionRole::Perimeter;
    vertex.gcode_id = gcode_id;
    return vertex;
}

libvgcode::PathVertex travel(float x, uint32_t gcode_id)
{
    libvgcode::PathVertex vertex = extrusion(x, gcode_id);
    vertex.type = libvgcode::EMoveType::Travel;
    return vertex;
}

} // namespace

TEST_CASE("Viewer mesh scope separates complete and visible toolpaths", "[ToolpathMeshBuilder]")
{
    libvgcode::Viewer viewer;
    viewer.m_impl->m_vertices = {
        extrusion(0.0f, 1), extrusion(10.0f, 2), travel(20.0f, 3),
        travel(90.0f, 4), extrusion(100.0f, 5), extrusion(110.0f, 6), travel(120.0f, 7)
    };
    viewer.m_impl->m_view_range.set_full(0, 6);
    viewer.m_impl->m_view_range.set_enabled(0, 6);
    viewer.m_impl->m_view_range.set_visible(4, 6);

    const auto complete = Slic3r::GUI::build_preview_triangle_mesh(
        viewer, Slic3r::GUI::PreviewMeshScope::Complete);
    const auto visible = Slic3r::GUI::build_preview_triangle_mesh(
        viewer, Slic3r::GUI::PreviewMeshScope::Visible);

    CHECK(complete.vertices.size() == 20);
    CHECK(complete.indices.size() == 96);
    CHECK(complete.bounds.min.x() < 0.0);
    CHECK(complete.bounds.max.x() > 110.0);

    CHECK(visible.vertices.size() == 10);
    CHECK(visible.indices.size() == 48);
    CHECK(visible.bounds.min.x() > 90.0);
    CHECK(visible.bounds.max.x() > 110.0);
    CHECK(visible.bounds.max.x() < 120.0);
}

TEST_CASE("Visible mesh stops at the selected range and preserves empty-range fallback", "[ToolpathMeshBuilder]")
{
    libvgcode::Viewer viewer;
    viewer.m_impl->m_vertices = {
        extrusion(0.0f, 1), extrusion(10.0f, 2), extrusion(20.0f, 3),
        extrusion(30.0f, 4), extrusion(40.0f, 5), extrusion(50.0f, 6)
    };
    viewer.m_impl->m_view_range.set_full(0, 5);
    viewer.m_impl->m_view_range.set_enabled(0, 5);
    viewer.m_impl->m_view_range.set_visible(1, 3);

    const auto selected = Slic3r::GUI::build_preview_triangle_mesh(
        viewer, Slic3r::GUI::PreviewMeshScope::Visible);
    CHECK(selected.bounds.min.x() < 10.0);
    CHECK(selected.bounds.max.x() > 30.0);
    CHECK(selected.bounds.max.x() < 35.0);

    viewer.m_impl->m_view_range.set_visible(0, 0);
    const auto fallback = Slic3r::GUI::build_preview_triangle_mesh(
        viewer, Slic3r::GUI::PreviewMeshScope::Visible);
    CHECK_FALSE(fallback.vertices.empty());
    CHECK(fallback.bounds.max.x() > 50.0);
}

TEST_CASE("Complete mesh includes and caps a terminal extrusion segment", "[ToolpathMeshBuilder]")
{
    libvgcode::GCodeInputData data;
    data.tools_colors.push_back({255, 0, 0});
    data.vertices = {extrusion(0.0f, 1), extrusion(10.0f, 2), extrusion(20.0f, 3)};

    const auto mesh = Slic3r::GUI::build_preview_triangle_mesh(data);
    CHECK(mesh.vertices.size() == 14);
    CHECK(mesh.indices.size() == 72);
    CHECK(mesh.bounds.max.x() > 20.0);
}
