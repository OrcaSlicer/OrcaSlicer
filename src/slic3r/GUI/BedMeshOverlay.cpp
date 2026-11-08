#include "BedMeshOverlay.hpp"

#include <algorithm>
#include <cmath>

#include <glad/gl.h>
#include <imgui/imgui.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/format.hpp"
#include "3DScene.hpp"
#include "GLShader.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "ImGuiWrapper.hpp"

namespace Slic3r {
namespace GUI {

namespace {

ColorRGBA rgb(unsigned char r, unsigned char g, unsigned char b) { return ColorRGBA(r, g, b, (unsigned char)255); }

// Diverging ramps: t = 0 is the lowest point, 0.5 on plane, 1 the highest. Each has a cool low
// pole, a neutral grey midpoint and a warm high pole, with the poles checked for separation
// under simulated colour-vision deficiencies (OKLab ΔE, see docs/HLSD/bed-mesh-overlay.md).
const ColorRGBA NEUTRAL = rgb(233, 233, 233);

// Normal vision: ColorBrewer RdBu, blue below, red above.
const std::vector<std::pair<float, ColorRGBA>> NORMAL_STOPS = {
    { 0.00f, rgb(5, 113, 176) }, { 0.25f, rgb(146, 197, 222) }, { 0.5f, NEUTRAL }, { 0.75f, rgb(244, 165, 130) }, { 1.0f, rgb(202, 0, 32) }
};
// Protanopia / deuteranopia: blue below, orange above (Okabe-Ito poles), separated along the
// blue-yellow axis these readers keep.
const std::vector<std::pair<float, ColorRGBA>> RED_GREEN_SAFE_STOPS = {
    { 0.00f, rgb(0, 114, 178) }, { 0.25f, rgb(143, 193, 230) }, { 0.5f, NEUTRAL }, { 0.75f, rgb(245, 206, 122) }, { 1.0f, rgb(230, 159, 0) }
};
// Tritanopia: green below, vermilion above, separated along the red-green axis these readers keep.
const std::vector<std::pair<float, ColorRGBA>> BLUE_YELLOW_SAFE_STOPS = {
    { 0.00f, rgb(0, 158, 115) }, { 0.25f, rgb(138, 212, 184) }, { 0.5f, NEUTRAL }, { 0.75f, rgb(245, 180, 138) }, { 1.0f, rgb(213, 94, 0) }
};

constexpr float SURFACE_ALPHA   = 0.85f;
constexpr int   RAMP_TEXELS     = 256;
constexpr float PROBE_MARK_HALF = 0.75f; // mm

} // namespace

void BedMeshOverlay::set_mesh(BedMesh mesh, const std::string& host)
{
    m_mesh         = std::move(mesh);
    m_host         = host;
    m_z_min        = m_mesh.z_min();
    m_z_max        = m_mesh.z_max();
    // Symmetric range so the neutral colour is always "on plane"; the floor keeps a flat bed
    // from being stretched into false relief.
    m_range        = std::max({ std::abs(m_z_min), std::abs(m_z_max), 0.005f });
    m_models_dirty = true;
}

void BedMeshOverlay::clear()
{
    m_mesh = BedMesh();
    m_host.clear();
    m_surface.reset();
    m_outline.reset();
    m_probe_marks.reset();
    m_z_min = m_z_max = m_range = 0.f;
    m_models_dirty = true;
}

void BedMeshOverlay::update_palette()
{
    const std::string mode = wxGetApp().app_config->get("preview_color_vision");
    if (mode == m_color_vision && !m_stops.empty())
        return;
    m_color_vision = mode;
    m_stops        = (mode == "protanopia" || mode == "deuteranopia") ? RED_GREEN_SAFE_STOPS :
                     mode == "tritanopia"                             ? BLUE_YELLOW_SAFE_STOPS :
                                                                        NORMAL_STOPS;
    m_ramp_dirty   = true;
}

ColorRGBA BedMeshOverlay::color_at(float t) const
{
    t = std::clamp(t, 0.f, 1.f);
    for (size_t i = 0; i + 1 < m_stops.size(); ++i) {
        const auto& [t0, c0] = m_stops[i];
        const auto& [t1, c1] = m_stops[i + 1];
        if (t <= t1) {
            const float f = t1 > t0 ? (t - t0) / (t1 - t0) : 0.f;
            return ColorRGBA(c0.r() + (c1.r() - c0.r()) * f, c0.g() + (c1.g() - c0.g()) * f, c0.b() + (c1.b() - c0.b()) * f, 1.f);
        }
    }
    return m_stops.empty() ? NEUTRAL : m_stops.back().second;
}

void BedMeshOverlay::init_ramp_texture()
{
    std::vector<unsigned char> data(RAMP_TEXELS * 4);
    for (int i = 0; i < RAMP_TEXELS; ++i) {
        const ColorRGBA c   = color_at((float(i) + 0.5f) / float(RAMP_TEXELS));
        data[4 * i + 0]     = (unsigned char)std::lround(c.r() * 255.f);
        data[4 * i + 1]     = (unsigned char)std::lround(c.g() * 255.f);
        data[4 * i + 2]     = (unsigned char)std::lround(c.b() * 255.f);
        data[4 * i + 3]     = (unsigned char)std::lround(SURFACE_ALPHA * 255.f);
    }
    m_ramp.reset();
    if (!m_ramp.load_from_raw_data(std::move(data), RAMP_TEXELS, 1))
        return;
    // load_from_raw_data re-uploads the level-0 bytes as every mip level, which reads as noise
    // once the strip is minified. Sample level 0 only, clamped, so each texel is one colour.
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_ramp.get_id()));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    m_ramp_dirty = false;
}

void BedMeshOverlay::init_models(float z)
{
    m_surface.reset();
    m_outline.reset();
    m_probe_marks.reset();
    m_models_z     = z;
    m_models_dirty = false;
    if (m_mesh.empty())
        return;

    // The surface: the firmware's interpolated grid, each node's colour looked up on the ramp
    // by its Z. Linear interpolation of the texture coordinate between nodes interpolates the
    // height, not the colour, so the neutral band stays where Z crosses zero. Cells with an
    // unprobed (NaN) corner are left out.
    const auto&  surf = m_mesh.surface();
    const size_t rows = surf.size();
    const size_t cols = surf.front().size();
    if (rows >= 2 && cols >= 2) {
        GLModel::Geometry geometry;
        geometry.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3T2 };
        geometry.reserve_vertices(rows * cols);
        geometry.reserve_indices((rows - 1) * (cols - 1) * 6);
        const float half_texel = 0.5f / float(RAMP_TEXELS);
        for (size_t r = 0; r < rows; ++r)
            for (size_t c = 0; c < cols; ++c) {
                const Vec2d p = m_mesh.node_position(r, c, rows, cols);
                const float v = BedMesh::is_probed(surf[r][c]) ? surf[r][c] : 0.f;
                const float u = std::clamp(0.5f + 0.5f * v / m_range, half_texel, 1.f - half_texel);
                geometry.add_vertex(Vec3f(float(p.x()), float(p.y()), z), Vec2f(u, 0.5f));
            }
        for (size_t r = 0; r + 1 < rows; ++r)
            for (size_t c = 0; c + 1 < cols; ++c) {
                if (!BedMesh::is_probed(surf[r][c]) || !BedMesh::is_probed(surf[r][c + 1]) || !BedMesh::is_probed(surf[r + 1][c]) ||
                    !BedMesh::is_probed(surf[r + 1][c + 1]))
                    continue;
                const unsigned a = unsigned(r * cols + c), b = a + 1, d = a + unsigned(cols), e = d + 1;
                geometry.add_triangle(a, b, e);
                geometry.add_triangle(a, e, d);
            }
        if (geometry.indices_count() > 0)
            m_surface.init_from(std::move(geometry));
    }

    // Outline of the probed rectangle: this is the edge of what the printer knows.
    const float z_marks = z + 0.001f;
    {
        GLModel::Geometry geometry;
        geometry.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        geometry.add_vertex(Vec3f(float(m_mesh.min.x()), float(m_mesh.min.y()), z_marks));
        geometry.add_vertex(Vec3f(float(m_mesh.max.x()), float(m_mesh.min.y()), z_marks));
        geometry.add_vertex(Vec3f(float(m_mesh.max.x()), float(m_mesh.max.y()), z_marks));
        geometry.add_vertex(Vec3f(float(m_mesh.min.x()), float(m_mesh.max.y()), z_marks));
        for (unsigned i = 0; i < 4; ++i)
            geometry.add_line(i, (i + 1) % 4);
        m_outline.init_from(std::move(geometry));
    }

    // One small square per probe point.
    {
        const size_t p_rows = m_mesh.probed_rows(), p_cols = m_mesh.probed_cols();
        GLModel::Geometry geometry;
        geometry.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        geometry.reserve_vertices(p_rows * p_cols * 4);
        geometry.reserve_indices(p_rows * p_cols * 6);
        for (size_t r = 0; r < p_rows; ++r)
            for (size_t c = 0; c < p_cols; ++c) {
                if (!BedMesh::is_probed(m_mesh.probed[r][c]))
                    continue;
                const Vec2d    p = m_mesh.node_position(r, c, p_rows, p_cols);
                const unsigned i = unsigned(geometry.vertices_count());
                geometry.add_vertex(Vec3f(float(p.x()) - PROBE_MARK_HALF, float(p.y()) - PROBE_MARK_HALF, z_marks));
                geometry.add_vertex(Vec3f(float(p.x()) + PROBE_MARK_HALF, float(p.y()) - PROBE_MARK_HALF, z_marks));
                geometry.add_vertex(Vec3f(float(p.x()) + PROBE_MARK_HALF, float(p.y()) + PROBE_MARK_HALF, z_marks));
                geometry.add_vertex(Vec3f(float(p.x()) - PROBE_MARK_HALF, float(p.y()) + PROBE_MARK_HALF, z_marks));
                geometry.add_triangle(i, i + 1, i + 2);
                geometry.add_triangle(i, i + 2, i + 3);
            }
        if (geometry.indices_count() > 0)
            m_probe_marks.init_from(std::move(geometry));
    }
}

void BedMeshOverlay::render(const Transform3d& view_matrix, const Transform3d& projection_matrix, const Vec3d& plate_origin, float z)
{
    if (!has_mesh())
        return;
    update_palette();
    if (m_models_dirty || m_models_z != z)
        init_models(z);
    if (m_ramp_dirty)
        init_ramp_texture();

    const Transform3d view_model_matrix = view_matrix * Geometry::translation_transform(plate_origin);

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    // Like the plate logo: drawn over the plate without claiming depth, so objects still occlude it.
    glsafe(::glDepthMask(GL_FALSE));

    if (m_surface.is_initialized() && m_ramp.get_id() != 0) {
        if (GLShaderProgram* shader = wxGetApp().get_shader("flat_texture"); shader != nullptr) {
            shader->start_using();
            shader->set_uniform("view_model_matrix", view_model_matrix);
            shader->set_uniform("projection_matrix", projection_matrix);
            glsafe(::glBindTexture(GL_TEXTURE_2D, m_ramp.get_id()));
            m_surface.render();
            glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
            shader->stop_using();
        }
    }

    if (GLShaderProgram* shader = wxGetApp().get_shader("flat"); shader != nullptr) {
        shader->start_using();
        shader->set_uniform("view_model_matrix", view_model_matrix);
        shader->set_uniform("projection_matrix", projection_matrix);
        const ColorRGBA ink = wxGetApp().dark_mode() ? ColorRGBA(0.95f, 0.95f, 0.95f, 0.9f) : ColorRGBA(0.15f, 0.15f, 0.15f, 0.9f);
        m_outline.set_color(ink);
        m_outline.render();
        m_probe_marks.set_color(ink);
        m_probe_marks.render();
        shader->stop_using();
    }

    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDisable(GL_BLEND));
}

void BedMeshOverlay::render_legend(const BoundingBoxf& bed, float right, float top, float scale)
{
    if (!has_mesh())
        return;
    update_palette();

    ImGuiWrapper* imgui  = wxGetApp().imgui();
    const float   margin = 10.0f * scale;
    imgui->set_next_window_pos(right - margin, top + margin, ImGuiCond_Always, 1.0f, 0.0f);
    ImGuiWrapper::push_common_window_style(scale);
    imgui->begin(_L("Bed Mesh"), ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);

    // The ramp, drawn segment by segment between its stops.
    const float bar_w = 220.0f * scale;
    const float bar_h = 14.0f * scale;
    ImDrawList*  draw = ImGui::GetWindowDrawList();
    const ImVec2 p    = ImGui::GetCursorScreenPos();
    auto to_u32 = [](const ColorRGBA& c) { return ImGui::GetColorU32(ImVec4(c.r(), c.g(), c.b(), 1.0f)); };
    for (size_t i = 0; i + 1 < m_stops.size(); ++i) {
        const float x0 = p.x + bar_w * m_stops[i].first;
        const float x1 = p.x + bar_w * m_stops[i + 1].first;
        const ImU32 c0 = to_u32(m_stops[i].second), c1 = to_u32(m_stops[i + 1].second);
        draw->AddRectFilledMultiColor(ImVec2(x0, p.y), ImVec2(x1, p.y + bar_h), c0, c1, c1, c0);
    }
    ImGui::Dummy(ImVec2(bar_w, bar_h));

    const std::string lo = Slic3r::format("%.3f mm", -m_range);
    const std::string hi = Slic3r::format("+%.3f mm", m_range);
    ImGui::Text("%s", lo.c_str());
    ImGui::SameLine(bar_w * 0.5f - ImGui::CalcTextSize("0").x * 0.5f);
    ImGui::Text("0");
    ImGui::SameLine(bar_w - ImGui::CalcTextSize(hi.c_str()).x);
    ImGui::Text("%s", hi.c_str());

    ImGui::Text("%s", Slic3r::format(_u8L("Profile: %1%"), m_mesh.profile_name.empty() ? _u8L("unsaved") : m_mesh.profile_name).c_str());
    const float  z_range = m_z_max - m_z_min;
    const size_t probed  = m_mesh.probed_count();
    std::string  grid    = Slic3r::format("%1% x %2%", m_mesh.probed_cols(), m_mesh.probed_rows());
    if (probed < m_mesh.probed_cols() * m_mesh.probed_rows())
        grid = Slic3r::format(_u8L("%1% of %2%"), probed, grid);
    ImGui::Text("%s", Slic3r::format(_u8L("%1% probe points, %2% to %3% mm, range %4% mm"), grid,
                                     Slic3r::format("%.3f", m_z_min), Slic3r::format("%+.3f", m_z_max), Slic3r::format("%.3f", z_range)).c_str());
    if (z_range > BedMesh::FLATNESS_WARNING_MM)
        // Amber, and worded, so the warning reads without the colour too.
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.15f, 1.0f), "%s",
                           Slic3r::format(_u8L("Warning: bed is %1% mm out of flat, more than the %2% mm threshold."),
                                          Slic3r::format("%.3f", z_range), Slic3r::format("%.1f", BedMesh::FLATNESS_WARNING_MM)).c_str());
    if (!m_mesh.source.empty())
        ImGui::Text("%s", (Slic3r::format(_u8L("Source: %1%"), m_mesh.source) + (m_mesh.note.empty() ? std::string() : " (" + m_mesh.note + ")")).c_str());
    if (!m_mesh.loaded)
        ImGui::Text("%s", _u8L("The printer is not confirmed to be compensating with this mesh.").c_str());

    // What the mesh covers. An adaptive mesh only knows the area of the last print, so say so
    // rather than letting a small patch read as the whole bed.
    const Vec2d size = m_mesh.max - m_mesh.min;
    if (bed.defined) {
        const Vec2d       bed_size = bed.size();
        const Vec2d       cov      = m_mesh.coverage(bed);
        const std::string percent  = Slic3r::format("%d%%", int(std::lround(100.0 * cov.x() * cov.y())));
        const std::string dims     = Slic3r::format("%1% x %2% mm", int(std::lround(size.x())), int(std::lround(size.y())));
        const std::string bed_dims = Slic3r::format("%1% x %2% mm", int(std::lround(bed_size.x())), int(std::lround(bed_size.y())));
        ImGui::Text("%s", (m_mesh.is_partial(bed) ?
                               Slic3r::format(_u8L("Partial (adaptive) mesh: %1%, %2% of the %3% bed. The rest was not probed."), dims, percent, bed_dims) :
                               Slic3r::format(_u8L("Covers %1%, %2% of the %3% bed."), dims, percent, bed_dims)).c_str());
    }
    else
        ImGui::Text("%s", Slic3r::format(_u8L("Covers %1% x %2% mm."), int(std::lround(size.x())), int(std::lround(size.y()))).c_str());

    imgui->end();
    ImGuiWrapper::pop_common_window_style();
}

} // namespace GUI
} // namespace Slic3r
