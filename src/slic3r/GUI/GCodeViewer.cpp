#include "libslic3r/libslic3r.h"
#include "GCodeViewer.hpp"

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/PresetBundle.hpp"
//BBS: add convex hull logic for toolpath check
#include "libslic3r/Geometry/ConvexHull.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "Camera.hpp"
#include "I18N.hpp"
#include "GUI_Utils.hpp"
#include "GUI.hpp"
#include "GLCanvas3D.hpp"
#include "GLToolbar.hpp"
#include "GUI_Preview.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "MsgDialog.hpp"

#if ENABLE_ACTUAL_SPEED_DEBUG
#define IMGUI_DEFINE_MATH_OPERATORS
#endif // ENABLE_ACTUAL_SPEED_DEBUG
#include <imgui/imgui_internal.h>

#include <GL/glew.h>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <wx/progdlg.h>
#include <wx/numformatter.h>

#include <array>
#include <algorithm>
#include <chrono>


namespace Slic3r {
namespace GUI {

//BBS translation of EViewType
//const std::string EViewType_Map[(int) GCodeViewer::EViewType::Count] = {
//        _u8L("Line Type"),
//        _u8L("Layer Height"),
//        _u8L("Line Width"),
//        _u8L("Speed"),
//        _u8L("Fan Speed"),
//        _u8L("Temperature"),
//        _u8L("Flow"),
//        _u8L("Tool"),
//        _u8L("Filament")
//    };

static std::string get_view_type_string(libvgcode::EViewType view_type)
{
    if (view_type == libvgcode::EViewType::Summary)
        return _u8L("Summary");
    else if (view_type == libvgcode::EViewType::FeatureType)
        return _u8L("Line Type");
    else if (view_type == libvgcode::EViewType::Height)
        return _u8L("Layer Height");
    else if (view_type == libvgcode::EViewType::Width)
        return _u8L("Line Width");
    else if (view_type == libvgcode::EViewType::Speed)
        return _u8L("Speed");
    else if (view_type == libvgcode::EViewType::ActualSpeed)
        return _u8L("Actual Speed");
    else if (view_type == libvgcode::EViewType::FanSpeed)
        return _u8L("Fan Speed");
    else if (view_type == libvgcode::EViewType::Temperature)
        return _u8L("Temperature");
    else if (view_type == libvgcode::EViewType::VolumetricFlowRate)
        return _u8L("Flow");
    else if (view_type == libvgcode::EViewType::ActualVolumetricFlowRate)
        return _u8L("Actual Flow");
    else if (view_type == libvgcode::EViewType::Tool)
        return _u8L("Tool");
    else if (view_type == libvgcode::EViewType::ColorPrint)
        return _u8L("Filament");
    else if (view_type == libvgcode::EViewType::LayerTimeLinear)
        return _u8L("Layer Time");
else if (view_type == libvgcode::EViewType::LayerTimeLogarithmic)
        return _u8L("Layer Time (log)");
    return "";
}

// Find an index of a value in a sorted vector, which is in <z-eps, z+eps>.
// Returns -1 if there is no such member.
static int find_close_layer_idx(const std::vector<double> &zs, double &z, double eps)
{
    if (zs.empty()) return -1;
    auto it_h = std::lower_bound(zs.begin(), zs.end(), z);
    if (it_h == zs.end()) {
        auto it_l = it_h;
        --it_l;
        if (z - *it_l < eps) return int(zs.size() - 1);
    } else if (it_h == zs.begin()) {
        if (*it_h - z < eps) return 0;
    } else {
        auto it_l = it_h;
        --it_l;
        double dist_l = z - *it_l;
        double dist_h = *it_h - z;
        if (std::min(dist_l, dist_h) < eps) { return (dist_l < dist_h) ? int(it_l - zs.begin()) : int(it_h - zs.begin()); }
    }
    return -1;
}

#if ENABLE_ACTUAL_SPEED_DEBUG
int GCodeViewer::SequentialView::ActualSpeedImguiWidget::plot(const char* label, const std::array<float, 2>& frame_size)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return -1;

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiID id = window->GetID(label);

    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    ImVec2 internal_frame_size(frame_size[0], frame_size[1]);
    internal_frame_size = ImGui::CalcItemSize(internal_frame_size, ImGui::CalcItemWidth(), label_size.y + style.FramePadding.y * 2.0f);

    const ImRect frame_bb(window->DC.CursorPos, window->DC.CursorPos + internal_frame_size);
    const ImRect inner_bb(frame_bb.Min + style.FramePadding, frame_bb.Max - style.FramePadding);
    const ImRect total_bb(frame_bb.Min, frame_bb.Max + ImVec2(label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f, 0));
    ImGui::ItemSize(total_bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(total_bb, 0, &frame_bb))
        return -1;

    const bool hovered = ImGui::ItemHoverable(frame_bb, id);

    ImGui::RenderFrame(frame_bb.Min, frame_bb.Max, ImGui::GetColorU32(ImGuiCol_FrameBg), true, style.FrameRounding);

    static const int values_count_min = 2;
    const int values_count = static_cast<int>(data.size());
    int idx_hovered = -1;

    const ImVec2 offset(10.0f, 0.0f);

    const float size_y = y_range.second - y_range.first;
    const float size_x = data.back().pos - data.front().pos;
    if (size_x > 0.0f && values_count >= values_count_min) {
        const float inv_scale_y = (size_y == 0.0f) ? 0.0f : 1.0f / size_y;
        const float inv_scale_x = 1.0f / size_x;
        const float x0 = data.front().pos;
        const float y0 = y_range.first;

        const ImU32 grid_main_color = ImGui::GetColorU32(ImVec4(0.5f, 0.5f, 0.5f, 0.5f));
        const ImU32 grid_secondary_color = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.5f, 0.5f));

        // horizontal levels
        for (const auto& [level, color] : levels) {
            const float y = 1.0f - ImSaturate((level - y_range.first) * inv_scale_y);
            window->DrawList->AddLine(ImLerp(inner_bb.Min, ImVec2(inner_bb.Min.x + offset.x, inner_bb.Max.y), ImVec2(0.1f, y)),
                ImLerp(inner_bb.Min, ImVec2(inner_bb.Min.x + offset.x, inner_bb.Max.y), ImVec2(0.9f, y)), ImGuiWrapper::to_ImU32(color), 3.0f);
        }

        // vertical positions
        for (int n = 0; n < values_count - 1; ++n) {
            const float x = ImSaturate((data[n].pos - x0) * inv_scale_x);
            window->DrawList->AddLine(ImLerp(inner_bb.Min + offset, inner_bb.Max, ImVec2(x, 0.0f)),
                ImLerp(inner_bb.Min + offset, inner_bb.Max, ImVec2(x, 1.0f)), data[n].internal ? grid_secondary_color : grid_main_color);
        }
        window->DrawList->AddLine(ImLerp(inner_bb.Min + offset, inner_bb.Max, ImVec2(1.0f, 0.0f)),
            ImLerp(inner_bb.Min + offset, inner_bb.Max, ImVec2(1.0f, 1.0f)), grid_main_color);

        // profiile
        const ImU32 col_base = ImGui::GetColorU32(ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
        const ImU32 col_hovered = ImGui::GetColorU32(ImGuiCol_PlotLinesHovered);
        for (int n = 0; n < values_count - 1; ++n) {
            const ImVec2 tp1(ImSaturate((data[n].pos - x0) * inv_scale_x), 1.0f - ImSaturate((data[n].speed - y0) * inv_scale_y));
            const ImVec2 tp2(ImSaturate((data[n + 1].pos - x0) * inv_scale_x), 1.0f - ImSaturate((data[n + 1].speed - y0) * inv_scale_y));
            // Tooltip on hover
            if (hovered && inner_bb.Contains(io.MousePos)) {
                const float t = ImClamp((io.MousePos.x - inner_bb.Min.x - offset.x) / (inner_bb.Max.x - inner_bb.Min.x - offset.x), 0.0f, 0.9999f);
                if (tp1.x < t && t < tp2.x)
                    idx_hovered = n;
            }
            window->DrawList->AddLine(ImLerp(inner_bb.Min + offset, inner_bb.Max, tp1), ImLerp(inner_bb.Min + offset, inner_bb.Max, tp2),
                idx_hovered == n ? col_hovered : col_base, 2.0f);
        }
    }

    if (label_size.x > 0.0f)
        ImGui::RenderText(ImVec2(frame_bb.Max.x + style.ItemInnerSpacing.x, inner_bb.Min.y), label);
    return idx_hovered;
}
#endif // ENABLE_ACTUAL_SPEED_DEBUG

void GCodeViewer::SequentialView::Marker::init(std::string filename)
{
    if (filename.empty()) {
        m_model.init_from(stilized_arrow(16, 1.5f, 3.0f, 0.8f, 3.0f));
    } else {
        m_model.init_from_file(filename);
    }
    m_model.set_color({ 1.0f, 1.0f, 1.0f, 0.5f });
}

//BBS: GUI refactor: add canvas size from parameters
void GCodeViewer::SequentialView::Marker::render(int canvas_width, int canvas_height, const libvgcode::EViewType& view_type)
{
    if (!m_visible)
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    shader->start_using();
    shader->set_uniform("emission_factor", 0.0f);

    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d& view_matrix = camera.get_view_matrix();
    float scale_factor = m_scale_factor;
    const Transform3d model_matrix = (Geometry::translation_transform((m_world_position + m_model_z_offset * Vec3f::UnitZ()).cast<double>()) *
        Geometry::translation_transform(scale_factor * m_model.get_bounding_box().size().z() * Vec3d::UnitZ()) * Geometry::rotation_transform({ M_PI, 0.0, 0.0 })) *
        Geometry::scale_transform(scale_factor);
    shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
    shader->set_uniform("view_normal_matrix", view_normal_matrix);

    m_model.render();

    shader->stop_using();

    glsafe(::glDisable(GL_BLEND));
}

static std::string to_string(libvgcode::EMoveType type)
{
    switch (type)
    {
    case libvgcode::EMoveType::Noop:        { return _u8L("Noop"); }
    case libvgcode::EMoveType::Retract:     { return _u8L("Retract"); }
    case libvgcode::EMoveType::Unretract:   { return _u8L("Unretract"); }
    case libvgcode::EMoveType::Seam:        { return _u8L("Seam"); }
    case libvgcode::EMoveType::ToolChange:  { return _u8L("Tool Change"); }
    case libvgcode::EMoveType::ColorChange: { return _u8L("Color Change"); }
    case libvgcode::EMoveType::PausePrint:  { return _u8L("Pause Print"); }
    case libvgcode::EMoveType::CustomGCode: { return _u8L("Custom GCode"); }
    case libvgcode::EMoveType::Travel:      { return _u8L("Travel"); }
    case libvgcode::EMoveType::Wipe:        { return _u8L("Wipe"); }
    case libvgcode::EMoveType::Extrude:     { return _u8L("Extrude"); }
    default:                                { return _u8L("Unknown"); }
    }
}

static std::string to_string(libvgcode::EGCodeExtrusionRole role)
{
    switch (role)
    {
    case libvgcode::EGCodeExtrusionRole::None:                     { return _u8L("Unknown"); }
    case libvgcode::EGCodeExtrusionRole::Perimeter:                { return _u8L("Perimeter"); }
    case libvgcode::EGCodeExtrusionRole::ExternalPerimeter:        { return _u8L("External perimeter"); }
    case libvgcode::EGCodeExtrusionRole::OverhangPerimeter:        { return _u8L("Overhang perimeter"); }
    case libvgcode::EGCodeExtrusionRole::InternalInfill:           { return _u8L("Internal infill"); }
    case libvgcode::EGCodeExtrusionRole::SolidInfill:              { return _u8L("Solid infill"); }
    case libvgcode::EGCodeExtrusionRole::TopSolidInfill:           { return _u8L("Top solid infill"); }
    case libvgcode::EGCodeExtrusionRole::Ironing:                  { return _u8L("Ironing"); }
    case libvgcode::EGCodeExtrusionRole::BridgeInfill:             { return _u8L("Bridge infill"); }
    case libvgcode::EGCodeExtrusionRole::GapFill:                  { return _u8L("Gap fill"); }
    case libvgcode::EGCodeExtrusionRole::Skirt:                    { return _u8L("Skirt"); } // ORCA
    case libvgcode::EGCodeExtrusionRole::SupportMaterial:          { return _u8L("Support material"); }
    case libvgcode::EGCodeExtrusionRole::SupportMaterialInterface: { return _u8L("Support material interface"); }
    case libvgcode::EGCodeExtrusionRole::WipeTower:                { return _u8L("Wipe tower"); }
    case libvgcode::EGCodeExtrusionRole::Custom:                   { return _u8L("Custom"); }
    // ORCA
    case libvgcode::EGCodeExtrusionRole::BottomSurface:            { return _u8L("Bottom surface"); }
    case libvgcode::EGCodeExtrusionRole::InternalBridgeInfill:     { return _u8L("Internal bridge infill"); }
    case libvgcode::EGCodeExtrusionRole::Brim:                     { return _u8L("Brim"); }
    case libvgcode::EGCodeExtrusionRole::SupportTransition:        { return _u8L("Support transition"); }
    case libvgcode::EGCodeExtrusionRole::Mixed:                    { return _u8L("Mixed"); }
    default:                                                       { return _u8L("Unknown"); }
    }
}

void GCodeViewer::SequentialView::Marker::render_position_window(const libvgcode::Viewer* viewer, int canvas_width, int canvas_height, const libvgcode::EViewType& view_type)
{
    static float last_window_width = 0.0f;
    static size_t last_text_length = 0;
    static bool properties_shown = false;

    if (viewer != nullptr) {
        ImGuiWrapper& imgui = *wxGetApp().imgui();
        // const Size cnv_size = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size();
        imgui.set_next_window_pos(0.5f * static_cast<float>(canvas_width), static_cast<float>(canvas_height), ImGuiCond_Always, 0.5f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImGuiWrapper::COL_BUTTON_BACKGROUND);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGuiWrapper::COL_BUTTON_ACTIVE);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGuiWrapper::COL_BUTTON_HOVERED);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::SetNextWindowBgAlpha(0.8f);
        imgui.begin(std::string("ToolPosition"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::AlignTextToFramePadding();
        // ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORCA, _u8L("Position") + ":");
        // ImGui::SameLine();
        libvgcode::PathVertex vertex = viewer->get_current_vertex();
        size_t vertex_id = viewer->get_current_vertex_id();
        if (vertex.type == libvgcode::EMoveType::Seam) {
            vertex_id = static_cast<size_t>(viewer->get_view_visible_range()[1]) - 1;
            vertex = viewer->get_vertex_at(vertex_id);
        }

        char buf[1024];
        sprintf(buf, "X: %.3f, Y: %.3f, Z: %.3f Speed: %.0f ", vertex.position[0], vertex.position[1], vertex.position[2], vertex.feedrate);
        switch (view_type) {
            case libvgcode::EViewType::Height: {
                sprintf(buf, "%s %s%.2f", buf, _u8L("Height: ").c_str(), vertex.height);
                break;
            }
            case libvgcode::EViewType::Width: {
                sprintf(buf, "%s %s%.2f", buf, _u8L("Width: ").c_str(), vertex.width);
                break;
            }
            case libvgcode::EViewType::VolumetricFlowRate: {
                sprintf(buf, "%s %s%.2f", buf, _u8L("Flow: ").c_str(), vertex.volumetric_rate());
                break;
            }
            case libvgcode::EViewType::FanSpeed: {
                sprintf(buf, "%s %s%.0f", buf, _u8L("Fan: ").c_str(), vertex.fan_speed);
                break;
            }
            case libvgcode::EViewType::Temperature: {
                sprintf(buf, "%s %s%.0f", buf, _u8L("Temperature: ").c_str(), vertex.temperature);
                break;
            }
            case libvgcode::EViewType::LayerTimeLinear:
            case libvgcode::EViewType::LayerTimeLogarithmic: {
                sprintf(buf, "%s %s%.1f", buf, _u8L("Layer Time: ").c_str(), vertex.layer_duration);
                break;
            }
            case libvgcode::EViewType::Tool: {
                sprintf(buf, "%s %s%d", buf, _u8L("Tool: ").c_str(), vertex.extruder_id + 1);
                break;
            }
            case libvgcode::EViewType::ColorPrint: {
                sprintf(buf, "%s %s%d", buf, _u8L("Color: ").c_str(), vertex.color_id + 1);
                break;
            }
            case libvgcode::EViewType::ActualVolumetricFlowRate: {
                sprintf(buf, "%s %s%.2f", buf, _u8L("Actual Flow: ").c_str(), vertex.actual_volumetric_rate());
                break;
            }
            case libvgcode::EViewType::ActualSpeed: {
                sprintf(buf, "%s %s%.1f", buf, _u8L("Actual Speed: ").c_str(), vertex.actual_feedrate);
                break;
            }

            default:
                break;
            }
        ImGuiWrapper::text(std::string(buf));
        if (view_type == libvgcode::EViewType::FeatureType) {
            ImGui::SameLine();
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, to_string(vertex.role).c_str());
        }
        ImGui::SameLine();
        if (imgui.image_button(properties_shown ? ImGui::HorizontalHide : ImGui::HorizontalShow, properties_shown ? _u8L("Hide properties") : _u8L("Show properties"))) {
            properties_shown = !properties_shown;
        }

        if (properties_shown) {
            auto append_table_row = [](const std::string& label, std::function<void(void)> value_callback) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORCA, label);
                ImGui::TableSetColumnIndex(1);
                value_callback();
            };

            ImGui::Separator();
            if (ImGui::BeginTable("Properties", 2)) {
                char buff[1024];

                append_table_row(_u8L("Type"), [&vertex]() {
                    ImGuiWrapper::text(_u8L(to_string(vertex.type)));
                });
                append_table_row(_u8L("Feature type"), [&vertex]() {
                    std::string text;
                    if (vertex.is_extrusion())
                        text = _u8L(to_string(vertex.role));
                    else
                        text = _u8L("N/A");
                    ImGuiWrapper::text(text);
                });
                append_table_row(_u8L("Width") + " (" + _u8L("mm") + ")", [&vertex, &buff]() {
                    std::string text;
                    if (vertex.is_extrusion()) {
                        sprintf(buff, "%.3f", vertex.width);
                        text = std::string(buff);
                    }
                    else
                        text = _u8L("N/A");
                    ImGuiWrapper::text(text);
                });
                append_table_row(_u8L("Height") + " (" + _u8L("mm") + ")", [&vertex, &buff]() {
                    std::string text;
                    if (vertex.is_extrusion()) {
                        sprintf(buff, "%.3f", vertex.height);
                        text = std::string(buff);
                    }
                    else
                        text = _u8L("N/A");
                    ImGuiWrapper::text(text);
                });
                append_table_row(_u8L("Layer"), [&vertex, &buff]() {
                    sprintf(buff, "%d", vertex.layer_id + 1);
                    const std::string text = std::string(buff);
                    ImGuiWrapper::text(text);
                });
                append_table_row(_u8L("Speed") + " (" + _u8L("mm/s") + ")", [&vertex, &buff]() {
                    sprintf(buff, "%.1f", vertex.feedrate);
                    const std::string text = std::string(buff);
                    ImGuiWrapper::text(text);
                });
                  append_table_row(_u8L("Volumetric flow rate") + " (" + _u8L("mm³/s") + ")", [&vertex, &buff]() {
                    std::string text;
                    if (vertex.is_extrusion()) {
                        sprintf(buff, "%.3f", vertex.volumetric_rate());
                        text = std::string(buff);
                    }
                    else
                        text = _u8L("N/A");
                    ImGuiWrapper::text(text);
                  });
                append_table_row(_u8L("Fan speed") + " (" + _u8L("%") + ")", [&vertex, &buff]() {
                    sprintf(buff, "%.0f", vertex.fan_speed);
                    const std::string text = std::string(buff);
                    ImGuiWrapper::text(text);
                });
                append_table_row(_u8L("Temperature") + " (" + _u8L("°C") + ")", [&vertex, &buff]() {
                    sprintf(buff, "%.0f", vertex.temperature);
                    ImGuiWrapper::text(std::string(buff));
                });
                append_table_row(_u8L("Time"), [viewer, &vertex, &buff, vertex_id]() {
                    const float estimated_time = viewer->get_estimated_time_at(vertex_id);
                    sprintf(buff, "%s (%.3fs)", get_time_dhms(estimated_time).c_str(), vertex.times[static_cast<size_t>(viewer->get_time_mode())]);
                    const std::string text = std::string(buff);
                    ImGuiWrapper::text(text);
                });

                ImGui::EndTable();
            }

#if ENABLE_ACTUAL_SPEED_DEBUG
            if (vertex.is_extrusion() || vertex.is_travel() || vertex.is_wipe()) {
                ImGui::Spacing();
                ImGuiWrapper::text(_u8L("Actual speed profile"));
                ImGui::SameLine();
                static bool table_shown = false;
                if (imgui.button(table_shown ? _u8L("Hide table") : _u8L("Show table")))
                    table_shown = !table_shown;
                ImGui::Separator();
                const int hover_id = m_actual_speed_imgui_widget.plot("##ActualSpeedProfile", { -1.0f, 150.0f });
                if (table_shown) {
                    static float table_wnd_height = 0.0f;
                    const ImVec2 wnd_size = ImGui::GetWindowSize();
                    imgui.set_next_window_pos(ImGui::GetWindowPos().x + wnd_size.x, static_cast<float>(canvas_height), ImGuiCond_Always, 0.0f, 1.0f);
                    ImGui::SetNextWindowSizeConstraints({ 0.0f, 0.0f }, { -1.0f, wnd_size.y });
                    imgui.begin(std::string("ToolPositionTableWnd"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
                    if (ImGui::BeginTable("ToolPositionTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
                        char buff[1024];
                        ImGui::TableSetupScrollFreeze(0, 1); // Make top row always visible
                        ImGui::TableSetupColumn("Position (mm)");
                        ImGui::TableSetupColumn("Speed (mm/s)");
                        ImGui::TableHeadersRow();
                        int counter = 0;
                        for (const ActualSpeedImguiWidget::Item& item : m_actual_speed_imgui_widget.data) {
                            const bool highlight = hover_id >= 0 && (counter == hover_id || counter == hover_id + 1);
                            if (highlight && counter == hover_id)
                                ImGui::SetScrollHereY();
                            ImGui::TableNextRow();
                            const ImU32 row_bg_color = ImGui::GetColorU32(item.internal ? ImVec4(0.0f, 0.0f, 0.5f, 0.25f) : ImVec4(0.5f, 0.5f, 0.5f, 0.25f));
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, row_bg_color);
                            ImGui::TableSetColumnIndex(0);
                            sprintf(buff, "%.3f", item.pos);
                            imgui.text_colored(highlight ? ImGuiWrapper::COL_ORCA : ImGuiWrapper::to_ImVec4(ColorRGBA::WHITE()), buff);
                            ImGui::TableSetColumnIndex(1);
                            sprintf(buff, "%.1f", item.speed);
                            imgui.text_colored(highlight ? ImGuiWrapper::COL_ORCA : ImGuiWrapper::to_ImVec4(ColorRGBA::WHITE()), buff);
                            ++counter;
                        }
                        ImGui::EndTable();
                    }
                    const float curr_table_wnd_height = ImGui::GetWindowHeight();
                    if (table_wnd_height != curr_table_wnd_height) {
                        table_wnd_height = curr_table_wnd_height;
                        // require extra frame to hide the table scroll bar (bug in imgui)
                        imgui.set_requires_extra_frame();
                    }
                    imgui.end();
                }
            }
#endif // ENABLE_ACTUAL_SPEED_DEBUG
        }

        // force extra frame to automatically update window size
        const float width = ImGui::GetWindowWidth();
        const size_t length = strlen(buf);
        if (width != last_window_width || length != last_text_length) {
            last_window_width = width;
            last_text_length = length;
            imgui.set_requires_extra_frame();
        }

        imgui.end();
        ImGui::PopStyleVar();
    }
    else {
        ImGuiWrapper& imgui = *wxGetApp().imgui();
        const Size cnv_size = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size();
        imgui.set_next_window_pos(0.5f * static_cast<float>(cnv_size.get_width()), static_cast<float>(cnv_size.get_height()), ImGuiCond_Always, 0.5f, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::SetNextWindowBgAlpha(0.8f);
        imgui.begin(std::string("ToolPosition"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        imgui.text_colored(ImGuiWrapper::COL_ORCA, _u8L("Tool position") + ":");
        ImGui::SameLine();
        char buf[1024];
        const Vec3f position = m_world_position + m_world_offset + m_z_offset * Vec3f::UnitZ();
        sprintf(buf, "X: %.3f, Y: %.3f, Z: %.3f", position.x(), position.y(), position.z());
        ImGuiWrapper::text(std::string(buf));

        // force extra frame to automatically update window size
        const float width = ImGui::GetWindowWidth();
        const size_t length = strlen(buf);
        if (width != last_window_width || length != last_text_length) {
            last_window_width = width;
            last_text_length = length;
            imgui.set_requires_extra_frame();
        }
        imgui.end();
        ImGui::PopStyleVar();
    }
}

void GCodeViewer::SequentialView::GCodeWindow::load_gcode(const std::string& filename, const std::vector<size_t> &lines_ends)
{
    assert(! m_file.is_open());
    if (m_file.is_open())
        return;

    m_filename   = filename;
    m_lines_ends = lines_ends;

    m_selected_line_id = 0;
    m_last_lines_size = 0;

    try
    {
        m_file.open(boost::filesystem::path(m_filename));
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": mapping file " << m_filename;
    }
    catch (...)
    {
        BOOST_LOG_TRIVIAL(error) << "Unable to map file " << m_filename << ". Cannot show G-code window.";
        reset();
    }
}

//BBS: GUI refactor: move to right
void GCodeViewer::SequentialView::GCodeWindow::render(float top, float bottom, float right, uint64_t curr_line_id) const
{
    // Orca: truncate long lines(>55 characters), add "..." at the end
    auto update_lines = [this](uint64_t start_id, uint64_t end_id) {
        std::vector<Line> ret;
        ret.reserve(end_id - start_id + 1);
        for (uint64_t id = start_id; id <= end_id; ++id) {
            // read line from file
            const size_t start        = id == 1 ? 0 : m_lines_ends[id - 2];
            const size_t original_len = m_lines_ends[id - 1] - start;
            const size_t len          = std::min(original_len, (size_t) 55);
            std::string  gline(m_file.data() + start, len);

            // If original line is longer than 55 characters, truncate and append "..."
            if (original_len > 55)
                gline = gline.substr(0, 52) + "...";

            std::string command, parameters, comment;
            // extract comment
            std::vector<std::string> tokens;
            boost::split(tokens, gline, boost::is_any_of(";"), boost::token_compress_on);
            command = tokens.front();
            if (tokens.size() > 1)
                comment = ";" + tokens.back();

            // extract gcode command and parameters
            if (!command.empty()) {
                boost::split(tokens, command, boost::is_any_of(" "), boost::token_compress_on);
                command = tokens.front();
                if (tokens.size() > 1) {
                    for (size_t i = 1; i < tokens.size(); ++i) {
                        parameters += " " + tokens[i];
                    }
                }
            }
            ret.push_back({command, parameters, comment});
        }
        return ret;
    };

    static const ImVec4 LINE_NUMBER_COLOR    = ImGuiWrapper::COL_ORANGE_LIGHT;
    static const ImVec4 SELECTION_RECT_COLOR = ImGuiWrapper::COL_ORANGE_DARK;
    static const ImVec4 COMMAND_COLOR        = {0.8f, 0.8f, 0.0f, 1.0f};
    static const ImVec4 PARAMETERS_COLOR     = { 1.0f, 1.0f, 1.0f, 1.0f };
    static const ImVec4 COMMENT_COLOR        = { 0.7f, 0.7f, 0.7f, 1.0f };

    if (!wxGetApp().show_gcode_window() || m_filename.empty() || m_lines_ends.empty() || curr_line_id == 0)
        return;

    // window height
    const float wnd_height = bottom - top;

    // number of visible lines
    const float text_height = ImGui::CalcTextSize("0").y;
    const ImGuiStyle& style = ImGui::GetStyle();
    const uint64_t lines_count = static_cast<uint64_t>((wnd_height - 2.0f * style.WindowPadding.y + style.ItemSpacing.y) / (text_height + style.ItemSpacing.y));

    if (lines_count == 0)
        return;

    // visible range
    const uint64_t half_lines_count = lines_count / 2;
    uint64_t start_id = (curr_line_id >= half_lines_count) ? curr_line_id - half_lines_count : 0;
    uint64_t end_id = start_id + lines_count - 1;
    if (end_id >= static_cast<uint64_t>(m_lines_ends.size())) {
        end_id = static_cast<uint64_t>(m_lines_ends.size()) - 1;
        start_id = end_id - lines_count + 1;
    }

    // updates list of lines to show, if needed
    if (m_selected_line_id != curr_line_id || m_last_lines_size != end_id - start_id + 1) {
        try
        {
            *const_cast<std::vector<Line>*>(&m_lines) = update_lines(start_id, end_id);
        }
        catch (...)
        {
            BOOST_LOG_TRIVIAL(error) << "Error while loading from file " << m_filename << ". Cannot show G-code window.";
            return;
        }
        *const_cast<uint64_t*>(&m_selected_line_id) = curr_line_id;
        *const_cast<size_t*>(&m_last_lines_size) = m_lines.size();
    }

    // line number's column width
    const float id_width = ImGui::CalcTextSize(std::to_string(end_id).c_str()).x;

    float required_width = 0.0f;
    { // --- Compute required G-code window width ---
        float max_text_width = id_width;
        for (const Line& line : m_lines) {
            float w = 0.0f;

            if (!line.command.empty())
                w += ImGui::CalcTextSize(line.command.c_str()).x;

            if (!line.parameters.empty())
                w += ImGui::CalcTextSize(line.parameters.c_str()).x;

            if (!line.comment.empty())
                w += ImGui::CalcTextSize(line.comment.c_str()).x;

            // spacing between segments
            w += 3 * ImGui::GetStyle().ItemSpacing.x;

            if (w > max_text_width)
                max_text_width = w;
        }

        required_width = id_width + max_text_width;
    }

    ImGuiWrapper& imgui = *wxGetApp().imgui();

    //BBS: GUI refactor: move to right
    imgui.set_next_window_pos(right, top + 6 * m_scale, ImGuiCond_Always, 1.0f, 0.0f); // ORCA add a small gap between legend and code viewer
    ImGui::SetNextWindowSize(ImVec2(required_width, wnd_height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * m_scale); // ORCA add window rounding to modernize / match style
    ImGui::SetNextWindowBgAlpha(0.8f);
    imgui.begin(std::string("G-code"), ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);

    // center the text in the window by pushing down the first line
    const float f_lines_count = static_cast<float>(lines_count);
    ImGui::SetCursorPosY(0.5f * (wnd_height - f_lines_count * text_height - (f_lines_count - 1.0f) * style.ItemSpacing.y));

    // render text lines
    for (uint64_t id = start_id; id <= end_id; ++id) {
        const Line& line = m_lines[id - start_id];

        // rect around the current selected line
        if (id == curr_line_id) {
            //BBS: GUI refactor: move to right
            const float pos_y = ImGui::GetCursorScreenPos().y;
            const float pos_x = ImGui::GetCursorScreenPos().x;
            const float half_ItemSpacing_y = 0.5f * style.ItemSpacing.y;
            const float half_ItemSpacing_x = 0.5f * style.ItemSpacing.x;
            //ImGui::GetWindowDrawList()->AddRect({ half_padding_x, pos_y - half_ItemSpacing_y },
            //    { ImGui::GetCurrentWindow()->Size.x - half_padding_x, pos_y + text_height + half_ItemSpacing_y },
            //    ImGui::GetColorU32(SELECTION_RECT_COLOR));
            ImGui::GetWindowDrawList()->AddRect({ pos_x - half_ItemSpacing_x, pos_y - half_ItemSpacing_y },
                { right - half_ItemSpacing_x, pos_y + text_height + half_ItemSpacing_y },
                ImGui::GetColorU32(SELECTION_RECT_COLOR));
        }

        // render line number
        const std::string id_str = std::to_string(id);
        // spacer to right align text
        ImGui::Dummy({ id_width - ImGui::CalcTextSize(id_str.c_str()).x, text_height });
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, LINE_NUMBER_COLOR);
        imgui.text(id_str);
        ImGui::PopStyleColor();

        if (!line.command.empty() || !line.comment.empty())
            ImGui::SameLine();

        // render command
        if (!line.command.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, COMMAND_COLOR);
            imgui.text(line.command);
            ImGui::PopStyleColor();
        }

        // render parameters
        if (!line.parameters.empty()) {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, PARAMETERS_COLOR);
            imgui.text(line.parameters);
            ImGui::PopStyleColor();
        }

        // render comment
        if (!line.comment.empty()) {
            if (!line.command.empty())
                ImGui::SameLine(0.0f, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, COMMENT_COLOR);
            imgui.text(line.comment);
            ImGui::PopStyleColor();
        }
    }

    imgui.end();
    ImGui::PopStyleVar();
}

void GCodeViewer::SequentialView::GCodeWindow::stop_mapping_file()
{
    //BBS: add log to trace the gcode file issue
    if (m_file.is_open()) {
        m_file.close();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": finished mapping file " << m_filename;
    }
}
void GCodeViewer::SequentialView::render(const bool has_render_path, float legend_height, const libvgcode::Viewer* viewer, uint32_t gcode_id, int canvas_width, int canvas_height, int right_margin, const libvgcode::EViewType& view_type)
{
    if (has_render_path && m_show_marker) {
        // marker.set_world_offset(current_offset);
        marker.render(canvas_width, canvas_height, view_type);
        marker.render_position_window(viewer, canvas_width, canvas_height, view_type);
    }

    //float bottom = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size().get_height();
    // BBS
#if 0
    if (wxGetApp().is_editor())
        bottom -= wxGetApp().plater()->get_view_toolbar().get_height();
#endif
    if (has_render_path)
        gcode_window.render(legend_height + 2, std::max(10.f, (float)canvas_height - 40), (float)canvas_width - (float)right_margin, gcode_id);
}

GCodeViewer::GCodeViewer()
{
    m_moves_slider  = new IMSlider(0, 0, 0, 100, wxSL_HORIZONTAL);
    m_layers_slider = new IMSlider(0, 0, 0, 100, wxSL_VERTICAL);
}

GCodeViewer::~GCodeViewer()
{
    reset();
    if (m_moves_slider) {
        delete m_moves_slider;
        m_moves_slider = nullptr;
    }
    if (m_layers_slider) {
        delete m_layers_slider;
        m_layers_slider = nullptr;
    }
}

void GCodeViewer::init(ConfigOptionMode mode, PresetBundle* preset_bundle)
{
    if (m_gl_data_initialized)
        return;

    // initializes tool marker
    std::string filename;
    if (preset_bundle != nullptr) {
        const Preset* curr = &preset_bundle->printers.get_selected_preset();
        if (curr->is_system)
            filename = PresetUtils::system_printer_hotend_model(*curr);
        else {
            auto *printer_model = curr->config.opt<ConfigOptionString>("printer_model");
            if (printer_model != nullptr && ! printer_model->value.empty()) {
                filename = preset_bundle->get_hotend_model_for_printer_model(printer_model->value);
            }

            if (filename.empty()) {
                filename = preset_bundle->get_hotend_model_for_printer_model(PresetBundle::ORCA_DEFAULT_PRINTER_MODEL);
            }
        }
    }

    m_sequential_view.marker.init(filename);

    // initializes point sizes
    std::array<int, 2> point_sizes;
    ::glGetIntegerv(GL_ALIASED_POINT_SIZE_RANGE, point_sizes.data());
    m_detected_point_sizes = { static_cast<float>(point_sizes[0]), static_cast<float>(point_sizes[1]) };

    // BBS initialzed view_type items
    m_user_mode = mode;
    update_by_mode(m_user_mode);

    m_layers_slider->init_texture();

    m_gl_data_initialized = true;

    try
    {
        m_viewer.init(reinterpret_cast<const char*>(glGetString(GL_VERSION)));
        glcheck();
    }
    catch (const std::exception& e)
    {
        MessageDialog msg_dlg(wxGetApp().plater(), e.what(), _L("Error"), wxICON_ERROR | wxOK);
        msg_dlg.ShowModal();
    }

    // Orca:
    // Default view type at first slice.
    // May be overridden in load() once we know how many tools are actually used in the G-code.
    m_nozzle_nums = preset_bundle ? preset_bundle->get_printer_extruder_count() : 1;
    auto it = std::find(view_type_items.begin(), view_type_items.end(), libvgcode::EViewType::FeatureType);
    m_view_type_sel = (it != view_type_items.end()) ? std::distance(view_type_items.begin(), it) : 0;
    set_view_type(libvgcode::EViewType::FeatureType);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": finished");
}

void GCodeViewer::on_change_color_mode(bool is_dark) {
    m_is_dark = is_dark;
    m_sequential_view.marker.on_change_color_mode(m_is_dark);
    m_sequential_view.gcode_window.on_change_color_mode(m_is_dark);
}

void GCodeViewer::set_scale(float scale)
{
    if(m_scale != scale)m_scale = scale;
    if (m_sequential_view.m_scale != scale) {
        m_sequential_view.m_scale = scale;
        m_sequential_view.marker.m_scale = scale;
        m_sequential_view.gcode_window.m_scale = scale; // ORCA
    }
}

void GCodeViewer::update_by_mode(ConfigOptionMode mode)
{
    view_type_items.clear();
    view_type_items_str.clear();
    options_items.clear();

    // BBS initialzed view_type items
    view_type_items.push_back(libvgcode::EViewType::Summary);
    view_type_items.push_back(libvgcode::EViewType::FeatureType);
    view_type_items.push_back(libvgcode::EViewType::ColorPrint);
    view_type_items.push_back(libvgcode::EViewType::Speed);
    view_type_items.push_back(libvgcode::EViewType::ActualSpeed);
    view_type_items.push_back(libvgcode::EViewType::Height);
    view_type_items.push_back(libvgcode::EViewType::Width);
    view_type_items.push_back(libvgcode::EViewType::VolumetricFlowRate);
    view_type_items.push_back(libvgcode::EViewType::ActualVolumetricFlowRate);
    view_type_items.push_back(libvgcode::EViewType::LayerTimeLinear);
    view_type_items.push_back(libvgcode::EViewType::LayerTimeLogarithmic);
    view_type_items.push_back(libvgcode::EViewType::FanSpeed);
    view_type_items.push_back(libvgcode::EViewType::Temperature);
    //if (mode == ConfigOptionMode::comDevelop) {
    //    view_type_items.push_back(EViewType::Tool);
    //}

    for (int i = 0; i < view_type_items.size(); i++) {
        view_type_items_str.push_back(get_view_type_string(view_type_items[i]));
    }

    // BBS for first layer inspection
    view_type_items.push_back(libvgcode::EViewType::Tool);

    options_items.push_back(EMoveType::Travel);
    options_items.push_back(EMoveType::Retract);
    options_items.push_back(EMoveType::Unretract);
    options_items.push_back(EMoveType::Wipe);
    //if (mode == ConfigOptionMode::comDevelop) {
    //    options_items.push_back(EMoveType::Tool_change);
    //}
    //BBS: seam is not real move and extrusion, put at last line
    options_items.push_back(EMoveType::Seam);
}

std::vector<int> GCodeViewer::get_plater_extruder()
{
    return m_plater_extruder;
}

//BBS: always load shell at preview
void GCodeViewer::load_as_gcode(const GCodeProcessorResult& gcode_result, const Print& print, const std::vector<std::string>& str_tool_colors,
                const std::vector<std::string>& str_color_print_colors, const BuildVolume& build_volume,
                const std::vector<BoundingBoxf3>& exclude_bounding_box, ConfigOptionMode mode, bool only_gcode)
{
    m_loaded_as_preview = false;

    const bool current_top_layer_only = m_viewer.is_top_layer_only_view_range();
    const bool required_top_layer_only = get_app_config()->get_bool("seq_top_layer_only");
    if (current_top_layer_only != required_top_layer_only)
        m_viewer.toggle_top_layer_only_view_range();

    // avoid processing if called with the same gcode_result
    if (m_last_result_id == gcode_result.id && wxGetApp().is_editor()) {
        //BBS: add logs
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": the same id %1%, return directly, result %2% ") % m_last_result_id % (&gcode_result);

        // collect tool colors
        libvgcode::Palette tools_colors;
        tools_colors.reserve(str_tool_colors.size());
        for (const std::string& color : str_tool_colors) {
            tools_colors.emplace_back(libvgcode::convert(color));
        }
        m_viewer.set_tool_colors(tools_colors);

        // collect color print colors
        libvgcode::Palette color_print_colors;
        const std::vector<std::string>& str_colors = str_color_print_colors.empty() ? str_tool_colors : str_color_print_colors;
        color_print_colors.reserve(str_colors.size());
        for (const std::string& color : str_colors) {
            color_print_colors.emplace_back(libvgcode::convert(color));
        }
        m_viewer.set_color_print_colors(color_print_colors);
        return;
    }

    //BBS: add logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": gcode result %1%, new id %2%, gcode file %3% ") % (&gcode_result) % m_last_result_id % gcode_result.filename;

    // release gpu memory, if used
    reset();

    //BBS: add mutex for protection of gcode result
    wxGetApp().plater()->suppress_background_process(true);
    gcode_result.lock();
    //BBS: add safe check
    if (gcode_result.moves.size() == 0) {
        //result cleaned before slicing ,should return here
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": gcode result reset before, return directly!");
        gcode_result.unlock();
        wxGetApp().plater()->schedule_background_process();
        return;
    }

    // convert data from PrusaSlicer format to libvgcode format
    libvgcode::GCodeInputData data = libvgcode::convert(gcode_result, str_tool_colors, str_color_print_colors, m_viewer);

//#define ENABLE_DATA_EXPORT 1
//#if ENABLE_DATA_EXPORT
//    auto extrusion_role_to_string = [](libvgcode::EGCodeExtrusionRole role) {
//        switch (role) {
//        case libvgcode::EGCodeExtrusionRole::None:                     { return "EGCodeExtrusionRole::None"; }
//        case libvgcode::EGCodeExtrusionRole::Perimeter:                { return "EGCodeExtrusionRole::Perimeter"; }
//        case libvgcode::EGCodeExtrusionRole::ExternalPerimeter:        { return "EGCodeExtrusionRole::ExternalPerimeter"; }
//        case libvgcode::EGCodeExtrusionRole::OverhangPerimeter:        { return "EGCodeExtrusionRole::OverhangPerimeter"; }
//        case libvgcode::EGCodeExtrusionRole::InternalInfill:           { return "EGCodeExtrusionRole::InternalInfill"; }
//        case libvgcode::EGCodeExtrusionRole::SolidInfill:              { return "EGCodeExtrusionRole::SolidInfill"; }
//        case libvgcode::EGCodeExtrusionRole::TopSolidInfill:           { return "EGCodeExtrusionRole::TopSolidInfill"; }
//        case libvgcode::EGCodeExtrusionRole::Ironing:                  { return "EGCodeExtrusionRole::Ironing"; }
//        case libvgcode::EGCodeExtrusionRole::BridgeInfill:             { return "EGCodeExtrusionRole::BridgeInfill"; }
//        case libvgcode::EGCodeExtrusionRole::GapFill:                  { return "EGCodeExtrusionRole::GapFill"; }
//        case libvgcode::EGCodeExtrusionRole::Skirt:                    { return "EGCodeExtrusionRole::Skirt"; }
//        case libvgcode::EGCodeExtrusionRole::SupportMaterial:          { return "EGCodeExtrusionRole::SupportMaterial"; }
//        case libvgcode::EGCodeExtrusionRole::SupportMaterialInterface: { return "EGCodeExtrusionRole::SupportMaterialInterface"; }
//        case libvgcode::EGCodeExtrusionRole::WipeTower:                { return "EGCodeExtrusionRole::WipeTower"; }
//        case libvgcode::EGCodeExtrusionRole::Custom:                   { return "EGCodeExtrusionRole::Custom"; }
//        case libvgcode::EGCodeExtrusionRole::COUNT:                    { return "EGCodeExtrusionRole::COUNT"; }
//        }
//    };
//
//    auto move_type_to_string = [](libvgcode::EMoveType type) {
//        switch (type) {
//        case libvgcode::EMoveType::Noop:        { return "EMoveType::Noop"; }
//        case libvgcode::EMoveType::Retract:     { return "EMoveType::Retract"; }
//        case libvgcode::EMoveType::Unretract:   { return "EMoveType::Unretract"; }
//        case libvgcode::EMoveType::Seam:        { return "EMoveType::Seam"; }
//        case libvgcode::EMoveType::ToolChange:  { return "EMoveType::ToolChange"; }
//        case libvgcode::EMoveType::ColorChange: { return "EMoveType::ColorChange"; }
//        case libvgcode::EMoveType::PausePrint:  { return "EMoveType::PausePrint"; }
//        case libvgcode::EMoveType::CustomGCode: { return "EMoveType::CustomGCode"; }
//        case libvgcode::EMoveType::Travel:      { return "EMoveType::Travel"; }
//        case libvgcode::EMoveType::Wipe:        { return "EMoveType::Wipe"; }
//        case libvgcode::EMoveType::Extrude:     { return "EMoveType::Extrude"; }
//        case libvgcode::EMoveType::COUNT:       { return "EMoveType::COUNT"; }
//        }
//    };
//
//    FilePtr out{ boost::nowide::fopen("C:/prusa/slicer/test_output/spe1872/test.data", "wb") };
//    if (out.f != nullptr) {
//        const uint32_t vertices_count = static_cast<uint32_t>(data.vertices.size());
//        fwrite((void*)&vertices_count, 1, sizeof(uint32_t), out.f);
//        for (const libvgcode::PathVertex& v : data.vertices) {
//            fwrite((void*)&v.position[0], 1, sizeof(float), out.f);
//            fwrite((void*)&v.position[1], 1, sizeof(float), out.f);
//            fwrite((void*)&v.position[2], 1, sizeof(float), out.f);
//            fwrite((void*)&v.height, 1, sizeof(float), out.f);
//            fwrite((void*)&v.width, 1, sizeof(float), out.f);
//            fwrite((void*)&v.feedrate, 1, sizeof(float), out.f);
//            fwrite((void*)&v.actual_feedrate, 1, sizeof(float), out.f);
//            fwrite((void*)&v.mm3_per_mm, 1, sizeof(float), out.f);
//            fwrite((void*)&v.fan_speed, 1, sizeof(float), out.f);
//            fwrite((void*)&v.temperature, 1, sizeof(float), out.f);
//            fwrite((void*)&v.role, 1, sizeof(uint8_t), out.f);
//            fwrite((void*)&v.type, 1, sizeof(uint8_t), out.f);
//            fwrite((void*)&v.gcode_id, 1, sizeof(uint32_t), out.f);
//            fwrite((void*)&v.layer_id, 1, sizeof(uint32_t), out.f);
//            fwrite((void*)&v.extruder_id, 1, sizeof(uint32_t), out.f);
//            fwrite((void*)&v.color_id, 1, sizeof(uint32_t), out.f);
//            fwrite((void*)&v.times[0], 1, sizeof(float), out.f);
//            fwrite((void*)&v.times[1], 1, sizeof(float), out.f);
//#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
//            const float weight = v.weight;
//#else
//            const float weight = 0.0f;
//#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
//            fwrite((void*)&weight, 1, sizeof(float), out.f);
//        }
//
//        const uint8_t spiral_vase_mode = data.spiral_vase_mode ? 1 : 0;
//        fwrite((void*)&spiral_vase_mode, 1, sizeof(uint8_t), out.f);
//
//        const uint32_t tool_colors_count = static_cast<uint32_t>(data.tools_colors.size());
//        fwrite((void*)&tool_colors_count, 1, sizeof(uint32_t), out.f);
//        for (const libvgcode::Color& c : data.tools_colors) {
//            fwrite((void*)&c[0], 1, sizeof(uint8_t), out.f);
//            fwrite((void*)&c[1], 1, sizeof(uint8_t), out.f);
//            fwrite((void*)&c[2], 1, sizeof(uint8_t), out.f);
//        }
//
//        const uint32_t color_print_colors_count = static_cast<uint32_t>(data.color_print_colors.size());
//        fwrite((void*)&color_print_colors_count, 1, sizeof(uint32_t), out.f);
//        for (const libvgcode::Color& c : data.color_print_colors) {
//            fwrite((void*)&c[0], 1, sizeof(uint8_t), out.f);
//            fwrite((void*)&c[1], 1, sizeof(uint8_t), out.f);
//            fwrite((void*)&c[2], 1, sizeof(uint8_t), out.f);
//        }
//    }
//#endif // ENABLE_DATA_EXPORT

    // send data to the viewer
    m_viewer.reset_default_extrusion_roles_colors();
    m_viewer.load(std::move(data));

// #if !VGCODE_ENABLE_COG_AND_TOOL_MARKERS
//     const size_t vertices_count = m_viewer.get_vertices_count();
//     m_cog.reset();
//     for (size_t i = 1; i < vertices_count; ++i) {
//         const libvgcode::PathVertex& curr = m_viewer.get_vertex_at(i);
//         if (curr.type == libvgcode::EMoveType::Extrude &&
//             curr.role != libvgcode::EGCodeExtrusionRole::Skirt &&
//             curr.role != libvgcode::EGCodeExtrusionRole::SupportMaterial &&
//             curr.role != libvgcode::EGCodeExtrusionRole::SupportMaterialInterface &&
//             curr.role != libvgcode::EGCodeExtrusionRole::WipeTower &&
//             curr.role != libvgcode::EGCodeExtrusionRole::Custom) {
//             const Vec3d curr_pos = libvgcode::convert(curr.position).cast<double>();
//             const Vec3d prev_pos = libvgcode::convert(m_viewer.get_vertex_at(i - 1).position).cast<double>();
//             m_cog.add_segment(curr_pos, prev_pos, gcode_result.filament_densities[curr.extruder_id] * curr.mm3_per_mm * (curr_pos - prev_pos).norm());
//         }
//     }
// #endif // !VGCODE_ENABLE_COG_AND_TOOL_MARKERS

    const libvgcode::AABox bbox = wxGetApp().is_gcode_viewer() ?
        m_viewer.get_bounding_box() :
        m_viewer.get_extrusion_bounding_box({
            libvgcode::EGCodeExtrusionRole::Perimeter, libvgcode::EGCodeExtrusionRole::ExternalPerimeter, libvgcode::EGCodeExtrusionRole::OverhangPerimeter,
            libvgcode::EGCodeExtrusionRole::InternalInfill, libvgcode::EGCodeExtrusionRole::SolidInfill, libvgcode::EGCodeExtrusionRole::TopSolidInfill,
            libvgcode::EGCodeExtrusionRole::Ironing, libvgcode::EGCodeExtrusionRole::BridgeInfill, libvgcode::EGCodeExtrusionRole::GapFill,
            libvgcode::EGCodeExtrusionRole::Skirt, libvgcode::EGCodeExtrusionRole::SupportMaterial, libvgcode::EGCodeExtrusionRole::SupportMaterialInterface,
            libvgcode::EGCodeExtrusionRole::WipeTower,
            // ORCA
            libvgcode::EGCodeExtrusionRole::BottomSurface, libvgcode::EGCodeExtrusionRole::InternalBridgeInfill, libvgcode::EGCodeExtrusionRole::Brim,
            libvgcode::EGCodeExtrusionRole::SupportTransition, libvgcode::EGCodeExtrusionRole::Mixed
            });
    m_paths_bounding_box = BoundingBoxf3(libvgcode::convert(bbox[0]).cast<double>(), libvgcode::convert(bbox[1]).cast<double>());

    if (wxGetApp().is_editor())
        m_contained_in_bed = wxGetApp().plater()->build_volume().all_paths_inside(gcode_result, m_paths_bounding_box);

    m_extruders_count = gcode_result.filaments_count;

    //BBS: move the id to the end of reset
    m_last_result_id = gcode_result.id;
    m_gcode_result = &gcode_result;
    m_only_gcode_in_preview = only_gcode;

    m_sequential_view.gcode_window.load_gcode(gcode_result.filename, gcode_result.lines_ends);

    //BBS: add only gcode mode
    //if (wxGetApp().is_gcode_viewer())
    if (m_only_gcode_in_preview)
        m_custom_gcode_per_print_z = gcode_result.custom_gcode_per_print_z;

    m_max_print_height = gcode_result.printable_height;
    m_z_offset = gcode_result.z_offset;

    // load_toolpaths(gcode_result, build_volume, exclude_bounding_box);
    
    // ORCA: Only show filament/color print preview if more than one tool/extruder is actually used in the toolpaths.
    // Only reset back to Toolpaths (FeatureType) if we are currently in ColorPrint and this load is single-tool.
    if (m_viewer.get_used_extruders_count() > 1) {
        auto it = std::find(view_type_items.begin(), view_type_items.end(), libvgcode::EViewType::ColorPrint);
        if (it != view_type_items.end())
            m_view_type_sel = std::distance(view_type_items.begin(), it);
        set_view_type(libvgcode::EViewType::ColorPrint);
    } else if (get_view_type() == libvgcode::EViewType::ColorPrint) {
        auto it = std::find(view_type_items.begin(), view_type_items.end(), libvgcode::EViewType::FeatureType);
        if (it != view_type_items.end())
            m_view_type_sel = std::distance(view_type_items.begin(), it);
        set_view_type(libvgcode::EViewType::FeatureType);
    }

    // BBS: data for rendering color arrangement recommendation
    m_nozzle_nums = print.config().option<ConfigOptionFloats>("nozzle_diameter")->values.size();
    // Orca hack: Hide filament group for non-bbl printers
    if (!print.is_BBL_printer()) m_nozzle_nums = 1;
    std::vector<int>         filament_maps = print.get_filament_maps();
    std::vector<std::string> color_opt     = print.config().option<ConfigOptionStrings>("filament_colour")->values;
    std::vector<std::string> type_opt      = print.config().option<ConfigOptionStrings>("filament_type")->values;
    std::vector<unsigned char> support_filament_opt = print.config().option<ConfigOptionBools>("filament_is_support")->values;
    for (auto extruder_id : m_viewer.get_used_extruders_ids()) {
        if (filament_maps[extruder_id] == 1) {
            m_left_extruder_filament.push_back({type_opt[extruder_id], color_opt[extruder_id], extruder_id, (bool)(support_filament_opt[extruder_id])});
        } else {
            m_right_extruder_filament.push_back({type_opt[extruder_id], color_opt[extruder_id], extruder_id, (bool)(support_filament_opt[extruder_id])});
        }
    }

    m_settings_ids = gcode_result.settings_ids;
    m_filament_diameters = gcode_result.filament_diameters;
    m_filament_densities = gcode_result.filament_densities;
    m_sequential_view.m_show_marker = false;

    //BBS: always load shell at preview
    /*if (wxGetApp().is_editor())
    {
        load_shells(print);
    }
    else {*/
    //BBS: add only gcode mode
    if (m_only_gcode_in_preview) {
        Pointfs printable_area;
        //BBS: add bed exclude area
        Pointfs bed_exclude_area = Pointfs();
        Pointfs wrapping_exclude_area = Pointfs();
        std::vector<Pointfs> extruder_areas;
        std::vector<double> extruder_heights;
        std::string texture;
        std::string model;

        if (!gcode_result.printable_area.empty()) {
            // bed shape detected in the gcode
            printable_area = gcode_result.printable_area;
            const auto bundle = wxGetApp().preset_bundle;
            if (bundle != nullptr && !m_settings_ids.printer.empty()) {
                const Preset* preset = bundle->printers.find_preset(m_settings_ids.printer);
                if (preset != nullptr) {
                    model = PresetUtils::system_printer_bed_model(*preset);
                    texture = PresetUtils::system_printer_bed_texture(*preset);
                }
            }

            //BBS: add bed exclude area
            if (!gcode_result.bed_exclude_area.empty())
                bed_exclude_area = gcode_result.bed_exclude_area;

            if (!gcode_result.wrapping_exclude_area.empty())
                wrapping_exclude_area = gcode_result.wrapping_exclude_area;

            if (!gcode_result.extruder_areas.empty())
                extruder_areas = gcode_result.extruder_areas;
            if (!gcode_result.extruder_heights.empty())
                extruder_heights = gcode_result.extruder_heights;

            wxGetApp().plater()->set_bed_shape(printable_area, bed_exclude_area, wrapping_exclude_area, gcode_result.printable_height, extruder_areas, extruder_heights, texture, model, gcode_result.printable_area.empty());
        }
        /*else {
            // adjust printbed size in dependence of toolpaths bbox
            const double margin = 10.0;
            const Vec2d min(m_paths_bounding_box.min.x() - margin, m_paths_bounding_box.min.y() - margin);
            const Vec2d max(m_paths_bounding_box.max.x() + margin, m_paths_bounding_box.max.y() + margin);

            const Vec2d size = max - min;
            printable_area = {
                { min.x(), min.y() },
                { max.x(), min.y() },
                { max.x(), min.y() + 0.442265 * size.y()},
                { max.x() - 10.0, min.y() + 0.4711325 * size.y()},
                { max.x() + 10.0, min.y() + 0.5288675 * size.y()},
                { max.x(), min.y() + 0.557735 * size.y()},
                { max.x(), max.y() },
                { min.x() + 0.557735 * size.x(), max.y()},
                { min.x() + 0.5288675 * size.x(), max.y() - 10.0},
                { min.x() + 0.4711325 * size.x(), max.y() + 10.0},
                { min.x() + 0.442265 * size.x(), max.y()},
                { min.x(), max.y() } };
        }*/
    }

    m_print_statistics = gcode_result.print_statistics;

    PrintEstimatedStatistics::ETimeMode time_mode = convert(m_viewer.get_time_mode());
    if (m_viewer.get_time_mode() != libvgcode::ETimeMode::Normal) {
        const float time = m_print_statistics.modes[static_cast<size_t>(time_mode)].time;
        if (time == 0.0f ||
            short_time(get_time_dhms(time)) == short_time(get_time_dhms(m_print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time)))
            m_viewer.set_time_mode(libvgcode::convert(PrintEstimatedStatistics::ETimeMode::Normal));
    }

    // set to color print by default if use multi extruders
    if (m_viewer.get_used_extruders_count() > 1) {
        for (int i = 0; i < view_type_items.size(); i++) {
            if (view_type_items[i] == libvgcode::EViewType::ColorPrint) {
                m_view_type_sel = i;
                break;
            }
        }

        set_view_type(libvgcode::EViewType::ColorPrint);
    }

    bool only_gcode_3mf = false;
    PartPlate* current_plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    bool current_has_print_instances = current_plate->has_printable_instances();
    if (current_plate->is_slice_result_valid() && wxGetApp().model().objects.empty() && !current_has_print_instances)
        only_gcode_3mf = true;
    m_layers_slider->set_menu_enable(!(only_gcode || only_gcode_3mf));
    m_layers_slider->set_as_dirty();
    m_moves_slider->set_as_dirty();

    //BBS
    m_conflict_result = gcode_result.conflict_result;
    if (m_conflict_result) { m_conflict_result.value().layer = m_viewer.get_layer_id_at(static_cast<float>(m_conflict_result.value()._height)); }

    m_gcode_check_result = gcode_result.gcode_check_result;

    filament_printable_reuslt = gcode_result.filament_printable_reuslt;
    //BBS: add mutex for protection of gcode result
    gcode_result.unlock();
    wxGetApp().plater()->schedule_background_process();
}

void GCodeViewer::load_as_preview(libvgcode::GCodeInputData&& data)
{
    m_loaded_as_preview = true;

    m_viewer.set_extrusion_role_color(libvgcode::EGCodeExtrusionRole::Skirt,                    { 127, 255, 127 });
    m_viewer.set_extrusion_role_color(libvgcode::EGCodeExtrusionRole::ExternalPerimeter,        { 255, 255, 0 });
    m_viewer.set_extrusion_role_color(libvgcode::EGCodeExtrusionRole::SupportMaterial,          { 127, 255, 127 });
    m_viewer.set_extrusion_role_color(libvgcode::EGCodeExtrusionRole::SupportMaterialInterface, { 127, 255, 127 });
    m_viewer.set_extrusion_role_color(libvgcode::EGCodeExtrusionRole::InternalInfill,           { 255, 127, 127 });
    m_viewer.set_extrusion_role_color(libvgcode::EGCodeExtrusionRole::SolidInfill,              { 255, 127, 127 });
    m_viewer.set_extrusion_role_color(libvgcode::EGCodeExtrusionRole::WipeTower,                { 127, 255, 127 });
    m_viewer.load(std::move(data));

    const libvgcode::AABox bbox = m_viewer.get_extrusion_bounding_box();
    const BoundingBoxf3 paths_bounding_box(libvgcode::convert(bbox[0]).cast<double>(), libvgcode::convert(bbox[1]).cast<double>());
    m_contained_in_bed = wxGetApp().plater()->build_volume().all_paths_inside(GCodeProcessorResult(), paths_bounding_box);
}

void GCodeViewer::update_shells_color_by_extruder(const DynamicPrintConfig *config)
{
    if (config != nullptr)
        m_shells.volumes.update_colors_by_extruder(config, false);
}

void GCodeViewer::set_shell_transparency(float alpha) { m_shells.volumes.set_transparency(alpha); }

//BBS: always load shell at preview
void GCodeViewer::reset_shell()
{
    m_shells.volumes.clear();
    m_shells.print_id = -1;
    m_shell_bounding_box = BoundingBoxf3();
}

void GCodeViewer::reset()
{
    //BBS: should also reset the result id
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": current result id %1% ")%m_last_result_id;
    m_last_result_id = -1;
    //BBS: add only gcode mode
    m_only_gcode_in_preview = false;

    m_viewer.reset();

    m_paths_bounding_box = BoundingBoxf3();
    m_max_bounding_box = BoundingBoxf3();
    m_max_print_height = 0.0f;
    m_z_offset = 0.0f;
    m_extruders_count = 0;
    m_filament_diameters = std::vector<float>();
    m_filament_densities = std::vector<float>();
    m_print_statistics.reset();
    m_custom_gcode_per_print_z = std::vector<CustomGCode::Item>();
    m_left_extruder_filament.clear();
    m_right_extruder_filament.clear();
    m_sequential_view.gcode_window.reset();
    m_contained_in_bed = true;
}

//BBS: GUI refactor: add canvas width and height
void GCodeViewer::render(int canvas_width, int canvas_height, int right_margin)
{
    glsafe(::glEnable(GL_DEPTH_TEST));
    render_shells(canvas_width, canvas_height);

    if (m_viewer.get_extrusion_roles().empty())
        return;

    render_toolpaths();

    float legend_height = 0.0f;
    render_legend(legend_height, canvas_width, canvas_height, right_margin);

    if (m_user_mode != wxGetApp().get_mode()) {
        update_by_mode(wxGetApp().get_mode());
        m_user_mode = wxGetApp().get_mode();
    }

    //BBS fixed bottom_margin for space to render horiz slider
    int bottom_margin = SLIDER_BOTTOM_MARGIN * GCODE_VIEWER_SLIDER_SCALE;
    auto current = m_viewer.get_view_visible_range();
    auto endpoints = m_viewer.get_view_full_range();
    m_sequential_view.m_show_marker = m_sequential_view.m_show_marker || (current.back() != endpoints.back() && !m_no_render_path);
    const libvgcode::PathVertex& curr_vertex = m_viewer.get_current_vertex();
    m_sequential_view.marker.set_world_position(libvgcode::convert(curr_vertex.position));
    m_sequential_view.marker.set_z_offset(m_z_offset + 0.5f);
    // BBS fixed buttom margin. m_moves_slider.pos_y
    m_sequential_view.render(!m_no_render_path, legend_height, &m_viewer, m_viewer.get_current_vertex().gcode_id, canvas_width, canvas_height - bottom_margin * m_scale, right_margin * m_scale, m_viewer.get_view_type());

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    if (is_legend_shown()) {
        ImGuiWrapper& imgui = *Slic3r::GUI::wxGetApp().imgui();
        const Size cnv_size = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size();
        ImGuiPureWrap::set_next_window_pos(static_cast<float>(cnv_size.get_width()), static_cast<float>(cnv_size.get_height()), ImGuiCond_Always, 1.0f, 1.0f);
        ImGuiPureWrap::begin(std::string("LibVGCode Viewer Controller"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize);

        ImGuiPureWrap::checkbox("Cog marker fixed screen size", m_cog_marker_fixed_screen_size);
        if (ImGui::BeginTable("Cog", 2)) {

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Cog marker size");
            ImGui::TableSetColumnIndex(1);
            imgui.slider_float("##CogSize", &m_cog_marker_size, 1.0f, 5.0f);

            ImGui::EndTable();
        }

        ImGuiPureWrap::checkbox("Tool marker fixed screen size", m_tool_marker_fixed_screen_size);
        if (ImGui::BeginTable("Tool", 2)) {

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Tool marker size");
            ImGui::TableSetColumnIndex(1);
            imgui.slider_float("##ToolSize", &m_tool_marker_size, 1.0f, 5.0f);

            ImGui::EndTable();
        }

        ImGuiPureWrap::end();
    }
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS

    //BBS render slider
    render_slider(canvas_width, canvas_height);
}

#define ENABLE_CALIBRATION_THUMBNAIL_OUTPUT 0
#if ENABLE_CALIBRATION_THUMBNAIL_OUTPUT
static void debug_calibration_output_thumbnail(const ThumbnailData& thumbnail_data)
{
    // debug export of generated image
    wxImage image(thumbnail_data.width, thumbnail_data.height);
    image.InitAlpha();

    for (unsigned int r = 0; r < thumbnail_data.height; ++r)
    {
        unsigned int rr = (thumbnail_data.height - 1 - r) * thumbnail_data.width;
        for (unsigned int c = 0; c < thumbnail_data.width; ++c)
        {
            unsigned char* px = (unsigned char*)thumbnail_data.pixels.data() + 4 * (rr + c);
            image.SetRGB((int)c, (int)r, px[0], px[1], px[2]);
            image.SetAlpha((int)c, (int)r, px[3]);
        }
    }

    image.SaveFile("D:/calibrate.png", wxBITMAP_TYPE_PNG);
}
#endif

bool GCodeViewer::can_export_toolpaths() const
{
    const libvgcode::Interval& visible_range = m_viewer.get_view_visible_range();
    for (size_t i = visible_range[0]; i <= visible_range[1]; ++i) {
        if (m_viewer.get_vertex_at(i).is_extrusion())
            return true;
    }
    return false;
}

void GCodeViewer::update_sequential_view_current(unsigned int first, unsigned int last)
{
    m_viewer.set_view_visible_range(static_cast<uint32_t>(first), static_cast<uint32_t>(last));
    const libvgcode::Interval& enabled_range = m_viewer.get_view_enabled_range();
    enable_moves_slider(enabled_range[1] > enabled_range[0]);

#if ENABLE_ACTUAL_SPEED_DEBUG
    if (enabled_range[1] != m_viewer.get_view_visible_range()[1]) {
        const libvgcode::PathVertex& curr_vertex = m_viewer.get_current_vertex();
        if (curr_vertex.is_extrusion() || curr_vertex.is_travel() || curr_vertex.is_wipe() ||
            curr_vertex.type == libvgcode::EMoveType::Seam) {
            const libvgcode::ColorRange& color_range = m_viewer.get_color_range(libvgcode::EViewType::ActualSpeed);
            const std::array<float, 2>& interval = color_range.get_range();
            const size_t vertices_count = m_viewer.get_vertices_count();
            std::vector<SequentialView::ActualSpeedImguiWidget::Item> actual_speed_data;
            // collect vertices sharing the same gcode_id
            const size_t curr_id = m_viewer.get_current_vertex_id();
            size_t start_id = curr_id;
            while (start_id > 0) {
                --start_id;
                if (curr_vertex.gcode_id != m_viewer.get_vertex_at(start_id).gcode_id)
                    break;
            }
            size_t end_id = curr_id;
            while (end_id < vertices_count - 1) {
                ++end_id;
                if (curr_vertex.gcode_id != m_viewer.get_vertex_at(end_id).gcode_id)
                    break;
            }

            if (m_viewer.get_vertex_at(end_id - 1).type == libvgcode::convert(EMoveType::Seam))
                --end_id;

            assert(end_id - start_id >= 2);

            float total_len = 0.0f;
            for (size_t i = start_id; i < end_id; ++i) {
                const libvgcode::PathVertex& v = m_viewer.get_vertex_at(i);
                const float len = (i > start_id) ?
                    (libvgcode::convert(v.position) - libvgcode::convert(m_viewer.get_vertex_at(i - 1).position)).norm() : 0.0f;
                total_len += len;
                if (i == start_id || len > EPSILON)
                    actual_speed_data.push_back({ total_len, v.actual_feedrate, v.times[0] == 0.0f });
            }

            std::vector<std::pair<float, ColorRGBA>> levels;
            const std::vector<float> values = color_range.get_values();
            for (float value : values) {
                levels.push_back(std::make_pair(value, libvgcode::convert(color_range.get_color_at(value))));
                levels.back().second.a(0.5f);
            }
            m_sequential_view.marker.set_actual_speed_data(actual_speed_data);
            m_sequential_view.marker.set_actual_speed_y_range(std::make_pair(interval[0], interval[1]));
            m_sequential_view.marker.set_actual_speed_levels(levels);
        }
    }
#endif // ENABLE_ACTUAL_SPEED_DEBUG
}

void GCodeViewer::enable_moves_slider(bool enable) const
{
    bool render_as_disabled = !enable;
    if (m_moves_slider != nullptr && m_moves_slider->is_rendering_as_disabled() != render_as_disabled) {
        m_moves_slider->set_render_as_disabled(render_as_disabled);
        m_moves_slider->set_as_dirty();
    }
}

void GCodeViewer::update_moves_slider(bool set_to_max)
{
    if (m_gcode_result->moves.empty())
        return;

    const libvgcode::Interval& range = m_viewer.get_view_enabled_range();
    const libvgcode::Interval& visible_range = m_viewer.get_view_visible_range();
    uint32_t last_gcode_id = get_gcode_vertex_at(range[0]).gcode_id;
    uint32_t gcode_id_min = get_gcode_vertex_at(visible_range[0]).gcode_id;
    uint32_t gcode_id_max = get_gcode_vertex_at(visible_range[1]).gcode_id;

    const size_t range_size = range[1] - range[0] + 1;
    std::vector<double> values;
    values.reserve(range_size);
    std::vector<double> alternate_values;
    alternate_values.reserve(range_size);

    std::optional<uint32_t> visible_range_min_id;
    std::optional<uint32_t> visible_range_max_id;
    uint32_t counter = 0;

    for (size_t i = range[0]; i <= range[1]; ++i) {
        const uint32_t gcode_id = get_gcode_vertex_at(i).gcode_id;
        bool skip = false;
        if (i > range[0]) {
            // skip consecutive moves with same gcode id (resulting from processing G2 and G3 lines)
            if (last_gcode_id == gcode_id) {
                values.back() = i + 1;
                skip = true;
            }
            else
                last_gcode_id = gcode_id;
        }

        if (!skip) {
            values.emplace_back(i + 1);
            alternate_values.emplace_back(gcode_id);
            if (alternate_values.back() == gcode_id_min)
                visible_range_min_id = counter;
            else if (alternate_values.back() == gcode_id_max)
                visible_range_max_id = counter;
            ++counter;
        }
    }

    const int span_min_id = visible_range_min_id.has_value() ? *visible_range_min_id : 0;
    const int span_max_id = visible_range_max_id.has_value() ? *visible_range_max_id : static_cast<int>(values.size()) - 1;

    bool keep_min = m_moves_slider->GetActiveValue() == m_moves_slider->GetMinValue();

    m_moves_slider->SetSliderValues(values);
    m_moves_slider->SetSliderAlternateValues(alternate_values);
    m_moves_slider->SetMaxValue(static_cast<int>(values.size()) - 1);
    m_moves_slider->SetSelectionSpan(span_min_id, span_max_id);
    if (set_to_max)
        m_moves_slider->SetHigherValue(keep_min ? m_moves_slider->GetMinValue() : m_moves_slider->GetMaxValue());
}

void GCodeViewer::update_layers_slider_mode()
{
    //    true  -> single-extruder printer profile OR
    //             multi-extruder printer profile , but whole model is printed by only one extruder
    //    false -> multi-extruder printer profile , and model is printed by several extruders
    bool one_extruder_printed_model = true;

    // extruder used for whole model for multi-extruder printer profile
    int only_extruder = -1;

    // BBS
    if (wxGetApp().filaments_cnt() > 1) {
        const ModelObjectPtrs &objects = wxGetApp().plater()->model().objects;

        // check if whole model uses just only one extruder
        if (!objects.empty()) {
            const int extruder = objects[0]->config.has("extruder") ? objects[0]->config.option("extruder")->getInt() : 0;

            auto is_one_extruder_printed_model = [objects, extruder]() {
                for (ModelObject *object : objects) {
                    if (object->config.has("extruder") && object->config.option("extruder")->getInt() != extruder) return false;

                    for (ModelVolume *volume : object->volumes)
                        if ((volume->config.has("extruder") && volume->config.option("extruder")->getInt() != extruder) || !volume->mmu_segmentation_facets.empty()) return false;

                    for (const auto &range : object->layer_config_ranges)
                        if (range.second.has("extruder") && range.second.option("extruder")->getInt() != extruder) return false;
                }
                return true;
            };

            if (is_one_extruder_printed_model())
                only_extruder = extruder;
            else
                one_extruder_printed_model = false;
        }
    }

    // TODO m_layers_slider->SetModeAndOnlyExtruder(one_extruder_printed_model, only_extruder);
}

void GCodeViewer::set_layers_z_range(const std::array<unsigned int, 2>& layers_z_range)
{
    m_viewer.set_layers_view_range(static_cast<uint32_t>(layers_z_range[0]), static_cast<uint32_t>(layers_z_range[1]));
    update_moves_slider(true);
}

class ToolpathsObjExporter
{
public:
    explicit ToolpathsObjExporter(const libvgcode::Viewer& viewer)
    : m_viewer(viewer) {
    }

    void export_to(const std::string& filename) {
        CNumericLocalesSetter locales_setter;

        // open geometry file
        FilePtr f_geo = boost::nowide::fopen(filename.c_str(), "w");
        if (f_geo.f == nullptr) {
            BOOST_LOG_TRIVIAL(error) << "ToolpathsObjExporter: Couldn't open " << filename << " for writing";
            return;
        }

        boost::filesystem::path materials_filename(filename);
        materials_filename.replace_extension("mtl");

        // write header to geometry file
        fprintf(f_geo.f, "# G-Code Toolpaths\n");
        fprintf(f_geo.f, "# Generated by %s-%s based on Slic3r\n", SLIC3R_APP_NAME, SoftFever_VERSION);
        fprintf(f_geo.f, "\nmtllib ./%s\n", materials_filename.filename().string().c_str());

        // open material file
        FilePtr f_mat = boost::nowide::fopen(materials_filename.string().c_str(), "w");
        if (f_mat.f == nullptr) {
            BOOST_LOG_TRIVIAL(error) << "ToolpathsObjExporter: Couldn't open " << materials_filename.string() << " for writing";
            return;
        }

        // write header to material file
        fprintf(f_mat.f, "# G-Code Toolpaths Materials\n");
        fprintf(f_mat.f, "# Generated by %s-%s based on Slic3r\n", SLIC3R_APP_NAME, SoftFever_VERSION);

        libvgcode::Interval visible_range = m_viewer.get_view_visible_range();
        if (m_viewer.is_top_layer_only_view_range())
            visible_range[0] = m_viewer.get_view_full_range()[0];
        for (size_t i = visible_range[0]; i <= visible_range[1]; ++i) {
            const libvgcode::PathVertex& curr = m_viewer.get_vertex_at(i);
            const libvgcode::PathVertex& next = m_viewer.get_vertex_at(i + 1);
            if (!curr.is_extrusion() || !next.is_extrusion())
                continue;

            const libvgcode::PathVertex& nextnext = m_viewer.get_vertex_at(i + 2);
            unsigned char flags = 0;
            if (curr.gcode_id == next.gcode_id)
                flags |= Flag_First;
            if (i + 1 == visible_range[1] || !nextnext.is_extrusion())
                flags |= Flag_Last;
            else
                flags |= Flag_Internal;
            export_segment(*f_geo.f, flags, i, curr, next, nextnext);
        }
        export_materials(*f_mat.f);
    }

private:
    const libvgcode::Viewer& m_viewer;
    size_t m_vertices_count{ 0 };
    std::vector<libvgcode::Color> m_colors;
    static const unsigned char Flag_First    = 0x01;
    static const unsigned char Flag_Last     = 0x02;
    static const unsigned char Flag_Internal = 0x04;
    static const float Cap_Rounding_Factor;

    struct SegmentLocalAxes
    {
        Vec3f forward;
        Vec3f right;
        Vec3f up;
    };

    SegmentLocalAxes segment_local_axes(const Vec3f& v1, const Vec3f& v2) {
        SegmentLocalAxes ret;
        ret.forward = (v2 - v1).normalized();
        ret.right   = ret.forward.cross(Vec3f::UnitZ()).normalized();
        ret.up      = ret.right.cross(ret.forward);
        return ret;
    }

    struct Vertex
    {
        Vec3f position;
        Vec3f normal;
    };

    struct CrossSection
    {
        Vertex right;
        Vertex top;
        Vertex left;
        Vertex bottom;
    };

    CrossSection cross_section(const Vec3f& v, const Vec3f& right, const Vec3f& up, float width, float height) {
        CrossSection ret;
        const Vec3f w_shift = 0.5f * width * right;
        const Vec3f h_shift = 0.5f * height * up;
        ret.right.position  = v + w_shift;
        ret.right.normal    = right;
        ret.top.position    = v + h_shift;
        ret.top.normal      = up;
        ret.left.position   = v - w_shift;
        ret.left.normal     = -right;
        ret.bottom.position = v - h_shift;
        ret.bottom.normal   = -up;
        return ret;
    }

    CrossSection normal_cross_section(const Vec3f& v, const SegmentLocalAxes& axes, float width, float height) {
        return cross_section(v, axes.right, axes.up, width, height);
    }

    enum CornerType : unsigned char
    {
        RightTurn,
        LeftTurn,
        Straight
    };

    CrossSection corner_cross_section(const Vec3f& v, const SegmentLocalAxes& axes1, const SegmentLocalAxes& axes2,
        float width, float height, CornerType& corner_type) {
        if (std::abs(std::abs(axes1.forward.dot(axes2.forward)) - 1.0f) < EPSILON)
            corner_type = CornerType::Straight;
        else if (axes1.up.dot(axes1.forward.cross(axes2.forward)) < 0.0f)
            corner_type = CornerType::RightTurn;
        else
            corner_type = CornerType::LeftTurn;
        const Vec3f right = (0.5f * (axes1.right + axes2.right)).normalized();
        return cross_section(v, right, axes1.up, width, height);
    }

    void export_segment(FILE& f, unsigned char flags, size_t v1_id, const libvgcode::PathVertex& v1, const libvgcode::PathVertex& v2, const libvgcode::PathVertex& v3) {
        const Vec3f v1_pos = libvgcode::convert(v1.position);
        const Vec3f v2_pos = libvgcode::convert(v2.position);
        const Vec3f v3_pos = libvgcode::convert(v3.position);
        const SegmentLocalAxes v1_v2 = segment_local_axes(v1_pos, v2_pos);
        const SegmentLocalAxes v2_v3 = segment_local_axes(v2_pos, v3_pos);

        // starting cap
        if ((flags & Flag_First) > 0) {
            const Vertex v0 = { v1_pos - Cap_Rounding_Factor * v1.width * v1_v2.forward, -v1_v2.forward };
            const CrossSection ncs = normal_cross_section(v1_pos, v1_v2, v1.width, v1.height);
            export_vertex(f, v0);         // 0
            export_vertex(f, ncs.right);  // 1
            export_vertex(f, ncs.top);    // 2
            export_vertex(f, ncs.left);   // 3
            export_vertex(f, ncs.bottom); // 4
            export_material(f, color_id(v1_id));
            export_triangle(f, vertex_id(0), vertex_id(1), vertex_id(2));
            export_triangle(f, vertex_id(0), vertex_id(2), vertex_id(3));
            export_triangle(f, vertex_id(0), vertex_id(3), vertex_id(4));
            export_triangle(f, vertex_id(0), vertex_id(4), vertex_id(1));
            m_vertices_count += 5;
        }
        // segment body + ending cap
        if ((flags & Flag_Last) > 0) {
            const Vertex v0 = { v2_pos + Cap_Rounding_Factor * v2.width * v1_v2.forward, v1_v2.forward };
            const CrossSection ncs = normal_cross_section(v2_pos, v1_v2, v2.width, v2.height);
            export_vertex(f, v0);         // 0
            export_vertex(f, ncs.right);  // 1
            export_vertex(f, ncs.top);    // 2
            export_vertex(f, ncs.left);   // 3
            export_vertex(f, ncs.bottom); // 4
            export_material(f, color_id(v1_id + 1));
            // segment body
            export_triangle(f, vertex_id(-4), vertex_id(1), vertex_id(2));
            export_triangle(f, vertex_id(-4), vertex_id(2), vertex_id(-3));
            export_triangle(f, vertex_id(-3), vertex_id(2), vertex_id(3));
            export_triangle(f, vertex_id(-3), vertex_id(3), vertex_id(-2));
            export_triangle(f, vertex_id(-2), vertex_id(3), vertex_id(4));
            export_triangle(f, vertex_id(-2), vertex_id(4), vertex_id(-1));
            export_triangle(f, vertex_id(-1), vertex_id(4), vertex_id(1));
            export_triangle(f, vertex_id(-1), vertex_id(1), vertex_id(-4));
            // ending cap
            export_triangle(f, vertex_id(0), vertex_id(3), vertex_id(2));
            export_triangle(f, vertex_id(0), vertex_id(2), vertex_id(1));
            export_triangle(f, vertex_id(0), vertex_id(1), vertex_id(4));
            export_triangle(f, vertex_id(0), vertex_id(4), vertex_id(3));
            m_vertices_count += 5;
        }
        else {
            CornerType corner_type;
            const CrossSection ccs   = corner_cross_section(v2_pos, v1_v2, v2_v3, v2.width, v2.height, corner_type);
            const CrossSection ncs12 = normal_cross_section(v2_pos, v1_v2, v2.width, v2.height);
            const CrossSection ncs23 = normal_cross_section(v2_pos, v2_v3, v2.width, v2.height);
            if (corner_type == CornerType::Straight) {
                export_vertex(f, ncs12.right);  // 0
                export_vertex(f, ncs12.top);    // 1
                export_vertex(f, ncs12.left);   // 2
                export_vertex(f, ncs12.bottom); // 3
                export_material(f, color_id(v1_id + 1));
                // segment body
                export_triangle(f, vertex_id(-4), vertex_id(0), vertex_id(1));
                export_triangle(f, vertex_id(-4), vertex_id(1), vertex_id(-3));
                export_triangle(f, vertex_id(-3), vertex_id(1), vertex_id(2));
                export_triangle(f, vertex_id(-3), vertex_id(2), vertex_id(-2));
                export_triangle(f, vertex_id(-2), vertex_id(2), vertex_id(3));
                export_triangle(f, vertex_id(-2), vertex_id(3), vertex_id(-1));
                export_triangle(f, vertex_id(-1), vertex_id(3), vertex_id(0));
                export_triangle(f, vertex_id(-1), vertex_id(0), vertex_id(-4));
                m_vertices_count += 4;
            }
            else if (corner_type == CornerType::RightTurn) {
                export_vertex(f, ncs12.left);   // 0
                export_vertex(f, ccs.left);     // 1
                export_vertex(f, ccs.right);    // 2
                export_vertex(f, ncs12.top);    // 3
                export_vertex(f, ncs23.left);   // 4
                export_vertex(f, ncs12.bottom); // 5
                export_material(f, color_id(v1_id + 1));
                // segment body
                export_triangle(f, vertex_id(-4), vertex_id(2), vertex_id(3));
                export_triangle(f, vertex_id(-4), vertex_id(3), vertex_id(-3));
                export_triangle(f, vertex_id(-3), vertex_id(3), vertex_id(0));
                export_triangle(f, vertex_id(-3), vertex_id(0), vertex_id(-2));
                export_triangle(f, vertex_id(-2), vertex_id(0), vertex_id(5));
                export_triangle(f, vertex_id(-2), vertex_id(5), vertex_id(-1));
                export_triangle(f, vertex_id(-1), vertex_id(5), vertex_id(2));
                export_triangle(f, vertex_id(-1), vertex_id(2), vertex_id(-4));
                // corner
                export_triangle(f, vertex_id(1), vertex_id(0), vertex_id(3));
                export_triangle(f, vertex_id(1), vertex_id(3), vertex_id(4));
                export_triangle(f, vertex_id(1), vertex_id(4), vertex_id(5));
                export_triangle(f, vertex_id(1), vertex_id(5), vertex_id(0));
                m_vertices_count += 6;
            }
            else {
                export_vertex(f, ncs12.right);  // 0
                export_vertex(f, ccs.right);    // 1
                export_vertex(f, ncs23.right);  // 2
                export_vertex(f, ncs12.top);    // 3
                export_vertex(f, ccs.left);     // 4
                export_vertex(f, ncs12.bottom); // 5
                export_material(f, color_id(v1_id + 1));
                // segment body
                export_triangle(f, vertex_id(-4), vertex_id(0), vertex_id(3));
                export_triangle(f, vertex_id(-4), vertex_id(3), vertex_id(-3));
                export_triangle(f, vertex_id(-3), vertex_id(3), vertex_id(4));
                export_triangle(f, vertex_id(-3), vertex_id(4), vertex_id(-2));
                export_triangle(f, vertex_id(-2), vertex_id(4), vertex_id(5));
                export_triangle(f, vertex_id(-2), vertex_id(5), vertex_id(-1));
                export_triangle(f, vertex_id(-1), vertex_id(5), vertex_id(0));
                export_triangle(f, vertex_id(-1), vertex_id(0), vertex_id(-4));
                // corner
                export_triangle(f, vertex_id(1), vertex_id(2), vertex_id(3));
                export_triangle(f, vertex_id(1), vertex_id(3), vertex_id(0));
                export_triangle(f, vertex_id(1), vertex_id(0), vertex_id(5));
                export_triangle(f, vertex_id(1), vertex_id(5), vertex_id(2));
                m_vertices_count += 6;
            }
        }
    }

    size_t vertex_id(int id) { return static_cast<size_t>(1 + static_cast<int>(m_vertices_count) + id); }

    void export_vertex(FILE& f, const Vertex& v) {
        fprintf(&f, "v %g %g %g\n", v.position.x(), v.position.y(), v.position.z());
        fprintf(&f, "vn %g %g %g\n", v.normal.x(), v.normal.y(), v.normal.z());
    }

    void export_material(FILE& f, size_t material_id) {
        fprintf(&f, "\nusemtl material_%zu\n", material_id + 1);
    }

    void export_triangle(FILE& f, size_t v1, size_t v2, size_t v3) {
        fprintf(&f, "f %zu//%zu %zu//%zu %zu//%zu\n", v1, v1, v2, v2, v3, v3);
    }

    void export_materials(FILE& f) {
        static const float inv_255 = 1.0f / 255.0f;
        size_t materials_counter = 0;
        for (const auto& color : m_colors) {
            fprintf(&f, "\nnewmtl material_%zu\n", ++materials_counter);
            fprintf(&f, "Ka 1 1 1\n");
            fprintf(&f, "Kd %g %g %g\n", static_cast<float>(color[0]) * inv_255,
                                         static_cast<float>(color[1]) * inv_255,
                                         static_cast<float>(color[2]) * inv_255);
            fprintf(&f, "Ks 0 0 0\n");
        }
    }

    size_t color_id(size_t vertex_id) {
        const libvgcode::PathVertex& v = m_viewer.get_vertex_at(vertex_id);
        const size_t top_layer_id = m_viewer.is_top_layer_only_view_range() ? m_viewer.get_layers_view_range()[1] : 0;
        const bool color_top_layer_only = m_viewer.get_view_full_range()[1] != m_viewer.get_view_visible_range()[1];
        const libvgcode::Color color = (color_top_layer_only && v.layer_id < top_layer_id &&
              (!m_viewer.is_spiral_vase_mode() || vertex_id != m_viewer.get_view_enabled_range()[0])) ?
              libvgcode::DUMMY_COLOR : m_viewer.get_vertex_color(v);
        auto color_it = std::find_if(m_colors.begin(), m_colors.end(), [&color](const libvgcode::Color& m) { return m == color; });
        if (color_it == m_colors.end()) {
            m_colors.emplace_back(color);
            color_it = std::prev(m_colors.end());
        }
        return std::distance(m_colors.begin(), color_it);
    }
};

const float ToolpathsObjExporter::Cap_Rounding_Factor = 0.25f;

void GCodeViewer::export_toolpaths_to_obj(const char* filename) const
{
    if (filename == nullptr)
        return;

    if (!has_data())
        return;

    wxBusyCursor busy;
    ToolpathsObjExporter exporter(m_viewer);
    exporter.export_to(filename);
}

void GCodeViewer::load_shells(const Print& print, bool initialized, bool force_previewing)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": initialized=%1%, force_previewing=%2%")%initialized %force_previewing;
    if ((print.id().id == m_shells.print_id)&&(print.get_modified_count() == m_shells.print_modify_count)) {
        //BBS: update force previewing logic
        if (force_previewing)
            m_shells.previewing = force_previewing;
        //already loaded
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": already loaded, print=%1% print_id=%2%, print_modify_count=%3%, force_previewing %4%")%(&print) %m_shells.print_id %m_shells.print_modify_count %force_previewing;
        return;
    }

    //reset shell firstly
    reset_shell();

    //BBS: move behind of reset_shell, to clear previous shell for empty plate
    if (print.objects().empty()) {
        // no shells, return
        return;
    }
    // adds objects' volumes
    // BBS: fix the issue that object_idx is not assigned as index of Model.objects array
    int object_count = 0;
    const ModelObjectPtrs& model_objs = wxGetApp().model().objects;
    for (const PrintObject* obj : print.objects()) {
        const ModelObject* model_obj = obj->model_object();

        int object_idx = -1;
        for (int idx = 0; idx < model_objs.size(); idx++) {
            if (model_objs[idx]->id() == model_obj->id()) {
                object_idx = idx;
                break;
            }
        }

        // BBS: object may be deleted when this method is called when deleting an object
        if (object_idx == -1)
            continue;

        std::vector<int> instance_ids(model_obj->instances.size());
        //BBS: only add the printable instance
        int instance_index = 0;
        for (int i = 0; i < (int)model_obj->instances.size(); ++i) {
            //BBS: only add the printable instance
            if (model_obj->instances[i]->is_printable())
                instance_ids[instance_index++] = i;
        }
        instance_ids.resize(instance_index);

        size_t current_volumes_count = m_shells.volumes.volumes.size();
        m_shells.volumes.load_object(model_obj, object_idx, instance_ids, "object", initialized, false);

        // adjust shells' z if raft is present
        const SlicingParameters& slicing_parameters = obj->slicing_parameters();
        if (slicing_parameters.object_print_z_min != 0.0) {
            const Vec3d z_offset = slicing_parameters.object_print_z_min * Vec3d::UnitZ();
            for (size_t i = current_volumes_count; i < m_shells.volumes.volumes.size(); ++i) {
                GLVolume* v = m_shells.volumes.volumes[i];
                auto offset  = v->get_instance_transformation().get_matrix_no_offset().inverse() * z_offset;
                v->set_volume_offset(v->get_volume_offset() + offset);
            }
        }

        object_count++;
    }

    // Orca: disable wipe tower shell
    // if (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF) {
        //     // BBS: adds wipe tower's volume
        //     std::vector<unsigned int> print_extruders = print.extruders(true);
        //     int extruders_count = print_extruders.size();

        //     const double max_z = print.objects()[0]->model_object()->get_model()->bounding_box().max(2);
        //     const PrintConfig& config = print.config();
        //     if (config.enable_prime_tower &&
            //         (print.enable_timelapse_print() || (extruders_count > 1 && (config.print_sequence == PrintSequence::ByLayer)))) {
            //         const float depth = print.wipe_tower_data(extruders_count).depth;
            //         const float brim_width = print.wipe_tower_data(extruders_count).brim_width;

            //         int plate_idx = print.get_plate_index();
            //         Vec3d plate_origin = print.get_plate_origin();
            //         double wipe_tower_x = config.wipe_tower_x.get_at(plate_idx) + plate_origin(0);
            //         double wipe_tower_y = config.wipe_tower_y.get_at(plate_idx) + plate_origin(1);
            //         m_shells.volumes.load_wipe_tower_preview(1000, wipe_tower_x, wipe_tower_y, config.prime_tower_width, depth, max_z, config.wipe_tower_rotation_angle,
                //             !print.is_step_done(psWipeTower), brim_width, initialized);
        //     }
    // }

    // remove modifiers
    while (true) {
        GLVolumePtrs::iterator it = std::find_if(m_shells.volumes.volumes.begin(), m_shells.volumes.volumes.end(), [](GLVolume* volume) { return volume->is_modifier; });
        if (it != m_shells.volumes.volumes.end()) {
            delete (*it);
            m_shells.volumes.volumes.erase(it);
        }
        else
            break;
    }

    for (GLVolume* volume : m_shells.volumes.volumes) {
        volume->zoom_to_volumes = false;
        volume->color.a(0.5f);
        volume->force_native_color = true;
        volume->set_render_color();
        //BBS: add shell bounding box logic
        m_shell_bounding_box.merge(volume->transformed_bounding_box());
    }

    //BBS: always load shell when preview
    m_shells.print_id = print.id().id;
    m_shells.print_modify_count = print.get_modified_count();
    m_shells.previewing = true;
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": shell loaded, id change to %1%, modify_count %2%, object count %3%, glvolume count %4%")
        % m_shells.print_id % m_shells.print_modify_count % object_count %m_shells.volumes.volumes.size();
}

void GCodeViewer::render_toolpaths()
{
    const Camera& camera = wxGetApp().plater()->get_camera();
    const libvgcode::Mat4x4 converted_view_matrix = libvgcode::convert(static_cast<Matrix4f>(camera.get_view_matrix().matrix().cast<float>()));
    const libvgcode::Mat4x4 converted_projetion_matrix = libvgcode::convert(static_cast<Matrix4f>(camera.get_projection_matrix().matrix().cast<float>()));
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    m_viewer.set_cog_marker_scale_factor(m_cog_marker_fixed_screen_size ? 10.0f * m_cog_marker_size * camera.get_inv_zoom() : m_cog_marker_size);
    m_viewer.set_tool_marker_scale_factor(m_tool_marker_fixed_screen_size ? 10.0f * m_tool_marker_size * camera.get_inv_zoom() : m_tool_marker_size);
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    m_viewer.render(converted_view_matrix, converted_projetion_matrix);

#if ENABLE_NEW_GCODE_VIEWER_DEBUG
    if (is_legend_shown()) {
        ImGuiWrapper& imgui = *wxGetApp().imgui();
        const Size cnv_size = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size();
        imgui.set_next_window_pos(static_cast<float>(cnv_size.get_width()), 0.0f, ImGuiCond_Always, 1.0f, 0.0f);
        imgui.begin(std::string("LibVGCode Viewer Debug"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize);

        if (ImGui::BeginTable("Data", 2)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "# vertices");
            ImGui::TableSetColumnIndex(1);
            ImGuiWrapper::text(std::to_string(m_viewer.get_vertices_count()));

            ImGui::Separator();

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "cpu memory");
            ImGui::TableSetColumnIndex(1);
            ImGuiWrapper::text(format_memsize(m_viewer.get_used_cpu_memory()));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "gpu memory");
            ImGui::TableSetColumnIndex(1);
            ImGuiWrapper::text(format_memsize(m_viewer.get_used_gpu_memory()));

            ImGui::Separator();

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "layers range");
            ImGui::TableSetColumnIndex(1);
            const libvgcode::Interval& layers_range = m_viewer.get_layers_view_range();
            ImGuiWrapper::text(std::to_string(layers_range[0]) + " - " + std::to_string(layers_range[1]));

            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "view range (enabled)");
            ImGui::TableSetColumnIndex(1);
            const libvgcode::Interval& enabled_view_range = m_viewer.get_view_enabled_range();
            ImGuiWrapper::text(std::to_string(enabled_view_range[0]) + " - " + std::to_string(enabled_view_range[1]) + " | " +
                std::to_string(m_viewer.get_vertex_at(enabled_view_range[0]).gcode_id) + " - " +
                std::to_string(m_viewer.get_vertex_at(enabled_view_range[1]).gcode_id));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "view range (visible)");
            ImGui::TableSetColumnIndex(1);
            const libvgcode::Interval& visible_view_range = m_viewer.get_view_visible_range();
            ImGuiWrapper::text(std::to_string(visible_view_range[0]) + " - " + std::to_string(visible_view_range[1]) + " | " +
                std::to_string(m_viewer.get_vertex_at(visible_view_range[0]).gcode_id) + " - " +
                std::to_string(m_viewer.get_vertex_at(visible_view_range[1]).gcode_id));

            auto add_range_property_row = [&imgui](const std::string& label, const std::array<float, 2>& range) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, label);
                ImGui::TableSetColumnIndex(1);
                char buf[128];
                sprintf(buf, "%.3f - %.3f", range[0], range[1]);
                ImGuiWrapper::text(buf);
            };

            add_range_property_row("height range", m_viewer.get_color_range(libvgcode::EViewType::Height).get_range());
            add_range_property_row("width range", m_viewer.get_color_range(libvgcode::EViewType::Width).get_range());
            add_range_property_row("speed range", m_viewer.get_color_range(libvgcode::EViewType::Speed).get_range());
            add_range_property_row("fan speed range", m_viewer.get_color_range(libvgcode::EViewType::FanSpeed).get_range());
            add_range_property_row("temperature range", m_viewer.get_color_range(libvgcode::EViewType::Temperature).get_range());
            add_range_property_row("volumetric rate range", m_viewer.get_color_range(libvgcode::EViewType::VolumetricFlowRate).get_range());
            add_range_property_row("layer time linear range", m_viewer.get_color_range(libvgcode::EViewType::LayerTimeLinear).get_range());
            add_range_property_row("layer time logarithmic range", m_viewer.get_color_range(libvgcode::EViewType::LayerTimeLogarithmic).get_range());

            ImGui::EndTable();
        }

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
        ImGui::Separator();

        if (ImGui::BeginTable("Cog", 2)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Cog marker scale factor");
            ImGui::TableSetColumnIndex(1);
            ImGuiPureWrap::text(std::to_string(get_cog_marker_scale_factor()));

            ImGui::EndTable();
        }

        ImGui::Separator();

        if (ImGui::BeginTable("Tool", 2)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Tool marker scale factor");
            ImGui::TableSetColumnIndex(1);
            ImGuiPureWrap::text(std::to_string(m_viewer.get_tool_marker_scale_factor()));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Tool marker z offset");
            ImGui::TableSetColumnIndex(1);
            float tool_z_offset = m_viewer.get_tool_marker_offset_z();
            if (imgui.slider_float("##ToolZOffset", &tool_z_offset, 0.0f, 1.0f))
                m_viewer.set_tool_marker_offset_z(tool_z_offset);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Tool marker color");
            ImGui::TableSetColumnIndex(1);
            const libvgcode::Color& color = m_viewer.get_tool_marker_color();
            std::array<float, 3> c = { static_cast<float>(color[0]) / 255.0f, static_cast<float>(color[1]) / 255.0f, static_cast<float>(color[2]) / 255.0f };
            if (ImGui::ColorPicker3("##ToolColor", c.data())) {
                m_viewer.set_tool_marker_color({ static_cast<uint8_t>(c[0] * 255.0f),
                                                  static_cast<uint8_t>(c[1] * 255.0f),
                                                  static_cast<uint8_t>(c[2] * 255.0f) });
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Tool marker alpha");
            ImGui::TableSetColumnIndex(1);
            float tool_alpha = m_viewer.get_tool_marker_alpha();
            if (imgui.slider_float("##ToolAlpha", &tool_alpha, 0.25f, 0.75f))
                m_viewer.set_tool_marker_alpha(tool_alpha);

            ImGui::EndTable();
        }
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS

        ImGui::Separator();
        if (ImGui::BeginTable("Radii", 2)) {

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "Travels radius");
            ImGui::TableSetColumnIndex(1);
            float travels_radius = m_viewer.get_travels_radius();
            ImGui::SetNextItemWidth(200.0f);
            if (imgui.slider_float("##TravelRadius", &travels_radius, 0.05f, 0.5f))
                m_viewer.set_travels_radius(travels_radius);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, "Wipes radius");
            ImGui::TableSetColumnIndex(1);
            float wipes_radius = m_viewer.get_wipes_radius();
            ImGui::SetNextItemWidth(200.0f);
            if (imgui.slider_float("##WipesRadius", &wipes_radius, 0.05f, 0.5f))
                m_viewer.set_wipes_radius(wipes_radius);

            ImGui::EndTable();
        }
        imgui.end();
    }
#endif // ENABLE_NEW_GCODE_VIEWER_DEBUG
}

void GCodeViewer::render_shells(int canvas_width, int canvas_height)
{
    //BBS: add shell previewing logic
    if ((!m_shells.previewing && !m_shells.visible) || m_shells.volumes.empty())
        //if (!m_shells.visible || m_shells.volumes.empty())
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    glsafe(::glDepthMask(GL_FALSE));

    shader->start_using();
    shader->set_uniform("emission_factor", 0.1f);
    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("z_far", camera.get_far_z());
    shader->set_uniform("z_near", camera.get_near_z());
    m_shells.volumes.render(GLVolumeCollection::ERenderType::Transparent, false, camera.get_view_matrix(), camera.get_projection_matrix(), {canvas_width, canvas_height});
    shader->set_uniform("emission_factor", 0.0f);
    shader->stop_using();

    glsafe(::glDepthMask(GL_TRUE));
}

//BBS
void GCodeViewer::render_all_plates_stats(const std::vector<const GCodeProcessorResult*>& gcode_result_list, bool show /*= true*/) const {
    if (!show)
        return;
    for (auto gcode_result : gcode_result_list) {
        if (gcode_result->moves.size() == 0)
            return;
    }
    ImGuiWrapper& imgui = *wxGetApp().imgui();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * m_scale); // ORCA add window rounding to modernize / match style
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0, 10.0 * m_scale));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.00f, 0.68f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.68f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.42f, 0.42f, 0.42f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(340.f * m_scale * imgui.scaled(1.0f / 15.0f), 0));

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), 0, ImVec2(0.5f, 0.5f));
    ImGui::Begin(_L("Statistics of All Plates").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    std::vector<float> filament_diameters = gcode_result_list.front()->filament_diameters;
    std::vector<float> filament_densities = gcode_result_list.front()->filament_densities;
    std::vector<ColorRGBA> filament_colors;
    decode_colors(wxGetApp().plater()->get_extruder_colors_from_plater_config(gcode_result_list.back()), filament_colors);

    for (int i = 0; i < filament_colors.size(); i++) {
        filament_colors[i] = adjust_color_for_rendering(filament_colors[i]);
    }

    bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
    float window_padding = 4.0f * m_scale;
    const float icon_size = ImGui::GetTextLineHeight() * 0.7;
    std::map<std::string, float> offsets;
    std::map<int, double> model_volume_of_extruders_all_plates; // map<extruder_idx, volume>
    std::map<int, double> flushed_volume_of_extruders_all_plates; // map<extruder_idx, flushed volume>
    std::map<int, double> wipe_tower_volume_of_extruders_all_plates; // map<extruder_idx, flushed volume>
    std::map<int, double> support_volume_of_extruders_all_plates; // map<extruder_idx, flushed volume>
    std::vector<double> model_used_filaments_m_all_plates;
    std::vector<double> model_used_filaments_g_all_plates;
    std::vector<double> flushed_filaments_m_all_plates;
    std::vector<double> flushed_filaments_g_all_plates;
    std::vector<double> wipe_tower_used_filaments_m_all_plates;
    std::vector<double> wipe_tower_used_filaments_g_all_plates;
    std::vector<double> support_used_filaments_m_all_plates;
    std::vector<double> support_used_filaments_g_all_plates;
    float total_time_all_plates = 0.0f;
    float total_cost_all_plates = 0.0f;
    bool show_detailed_statistics_page = false;
    struct ColumnData {
        enum {
            Model = 1,
            Flushed = 2,
            WipeTower = 4,
            Support = 1 << 3,
        };
    };
    int displayed_columns = 0;
    auto max_width = [](const std::vector<std::string>& items, const std::string& title, float extra_size = 0.0f) {
        float ret = ImGui::CalcTextSize(title.c_str()).x;
        for (const std::string& item : items) {
            ret = std::max(ret, extra_size + ImGui::CalcTextSize(item.c_str()).x);
        }
        return ret;
    };
    auto calculate_offsets = [max_width, window_padding](const std::vector<std::pair<std::string, std::vector<::string>>>& title_columns, float extra_size = 0.0f) {
        const ImGuiStyle& style = ImGui::GetStyle();
        std::vector<float> offsets;
        offsets.push_back(max_width(title_columns[0].second, title_columns[0].first, extra_size) + 3.0f * style.ItemSpacing.x + style.WindowPadding.x);
        for (size_t i = 1; i < title_columns.size() - 1; i++)
            offsets.push_back(offsets.back() + max_width(title_columns[i].second, title_columns[i].first) + style.ItemSpacing.x);
        if (title_columns.back().first == _u8L("Display"))
            offsets.back() = ImGui::GetWindowWidth() - ImGui::CalcTextSize(_u8L("Display").c_str()).x - ImGui::GetFrameHeight() / 2 - 2 * window_padding;

        float average_col_width = ImGui::GetWindowWidth() / static_cast<float>(title_columns.size());
        std::vector<float> ret;
        ret.push_back(0);
        for (size_t i = 1; i < title_columns.size(); i++) {
            ret.push_back(std::max(offsets[i - 1], i * average_col_width));
        }

        return ret;
    };
    auto append_item = [icon_size, &imgui, imperial_units, &window_padding, &draw_list, this](const ColorRGBA& color, const std::vector<std::pair<std::string, float>>& columns_offsets)
    {
        // render icon
        ImVec2 pos = ImVec2(ImGui::GetCursorScreenPos().x + window_padding * 3, ImGui::GetCursorScreenPos().y);

        draw_list->AddRectFilled({ pos.x + 1.0f * m_scale, pos.y + 3.0f * m_scale }, { pos.x + icon_size - 1.0f * m_scale, pos.y + icon_size + 1.0f * m_scale },
            ImGuiWrapper::to_ImU32(color));

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(20.0 * m_scale, 6.0 * m_scale));

        // render selectable
        ImGui::Dummy({ 0.0, 0.0 });
        ImGui::SameLine();

        // render column item
        {
            float dummy_size = ImGui::GetStyle().ItemSpacing.x + icon_size;
            ImGui::SameLine(dummy_size);
            imgui.text(columns_offsets[0].first);

            for (auto i = 1; i < columns_offsets.size(); i++) {
                ImGui::SameLine(columns_offsets[i].second);
                imgui.text(columns_offsets[i].first);
            }
        }

        ImGui::PopStyleVar(1);
    };
    auto append_headers = [&imgui](const std::vector<std::pair<std::string, float>>& title_offsets) {
        for (size_t i = 0; i < title_offsets.size(); i++) {
            ImGui::SameLine(title_offsets[i].second);
            imgui.bold_text(title_offsets[i].first);
        }
        ImGui::Separator();
    };
    auto get_used_filament_from_volume = [this, imperial_units, &filament_diameters, &filament_densities](double volume, int extruder_id) {
        double koef = imperial_units ? 1.0 / GizmoObjectManipulation::in_to_mm : 0.001;
        std::pair<double, double> ret = { koef * volume / (PI * sqr(0.5 * filament_diameters[extruder_id])),
                                            volume * filament_densities[extruder_id] * 0.001 };
        return ret;
    };

    ImGui::Dummy({ window_padding, window_padding });
    ImGui::SameLine();
    // title and item data
    {
        PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();
        for (auto plate : plate_list.get_nonempty_plate_list())
        {
            auto plate_print_statistics = plate->get_slice_result()->print_statistics;
            auto plate_extruders = plate->get_extruders(true);
            for (size_t extruder_id : plate_extruders) {
                extruder_id -= 1;
                if (plate_print_statistics.model_volumes_per_extruder.find(extruder_id) == plate_print_statistics.model_volumes_per_extruder.end())
                    model_volume_of_extruders_all_plates[extruder_id] += 0;
                else {
                    double model_volume = plate_print_statistics.model_volumes_per_extruder.at(extruder_id);
                    model_volume_of_extruders_all_plates[extruder_id] += model_volume;
                }
                if (plate_print_statistics.flush_per_filament.find(extruder_id) == plate_print_statistics.flush_per_filament.end())
                    flushed_volume_of_extruders_all_plates[extruder_id] += 0;
                else {
                    double flushed_volume = plate_print_statistics.flush_per_filament.at(extruder_id);
                    flushed_volume_of_extruders_all_plates[extruder_id] += flushed_volume;
                }
                if (plate_print_statistics.wipe_tower_volumes_per_extruder.find(extruder_id) == plate_print_statistics.wipe_tower_volumes_per_extruder.end())
                    wipe_tower_volume_of_extruders_all_plates[extruder_id] += 0;
                else {
                    double wipe_tower_volume = plate_print_statistics.wipe_tower_volumes_per_extruder.at(extruder_id);
                    wipe_tower_volume_of_extruders_all_plates[extruder_id] += wipe_tower_volume;
                }
                if (plate_print_statistics.support_volumes_per_extruder.find(extruder_id) == plate_print_statistics.support_volumes_per_extruder.end())
                    support_volume_of_extruders_all_plates[extruder_id] += 0;
                else {
                    double support_volume = plate_print_statistics.support_volumes_per_extruder.at(extruder_id);
                    support_volume_of_extruders_all_plates[extruder_id] += support_volume;
                }
            }
            const PrintEstimatedStatistics::Mode& plate_time_mode = plate_print_statistics.modes[static_cast<size_t>(m_viewer.get_time_mode())];
            total_time_all_plates += plate_time_mode.time;

            Print     *print;
            plate->get_print((PrintBase **) &print, nullptr, nullptr);
            total_cost_all_plates += print->print_statistics().total_cost;
        }

        for (auto it = model_volume_of_extruders_all_plates.begin(); it != model_volume_of_extruders_all_plates.end(); it++) {
            auto [model_used_filament_m, model_used_filament_g] = get_used_filament_from_volume(it->second, it->first);
            if (model_used_filament_m != 0.0 || model_used_filament_g != 0.0)
                displayed_columns |= ColumnData::Model;
            model_used_filaments_m_all_plates.push_back(model_used_filament_m);
            model_used_filaments_g_all_plates.push_back(model_used_filament_g);
        }
        for (auto it = flushed_volume_of_extruders_all_plates.begin(); it != flushed_volume_of_extruders_all_plates.end(); it++) {
            auto [flushed_filament_m, flushed_filament_g] = get_used_filament_from_volume(it->second, it->first);
            if (flushed_filament_m != 0.0 || flushed_filament_g != 0.0)
                displayed_columns |= ColumnData::Flushed;
            flushed_filaments_m_all_plates.push_back(flushed_filament_m);
            flushed_filaments_g_all_plates.push_back(flushed_filament_g);
        }
        for (auto it = wipe_tower_volume_of_extruders_all_plates.begin(); it != wipe_tower_volume_of_extruders_all_plates.end(); it++) {
            auto [wipe_tower_filament_m, wipe_tower_filament_g] = get_used_filament_from_volume(it->second, it->first);
            if (wipe_tower_filament_m != 0.0 || wipe_tower_filament_g != 0.0)
                displayed_columns |= ColumnData::WipeTower;
            wipe_tower_used_filaments_m_all_plates.push_back(wipe_tower_filament_m);
            wipe_tower_used_filaments_g_all_plates.push_back(wipe_tower_filament_g);
        }
        for (auto it = support_volume_of_extruders_all_plates.begin(); it != support_volume_of_extruders_all_plates.end(); it++) {
            auto [support_filament_m, support_filament_g] = get_used_filament_from_volume(it->second, it->first);
            if (support_filament_m != 0.0 || support_filament_g != 0.0)
                displayed_columns |= ColumnData::Support;
            support_used_filaments_m_all_plates.push_back(support_filament_m);
            support_used_filaments_g_all_plates.push_back(support_filament_g);
        }

        char buff[64];
        double longest_str = 0.0;
        for (auto i : model_used_filaments_g_all_plates) {
            if (i > longest_str)
                longest_str = i;
        }
        ::sprintf(buff, "%.2f", longest_str);

        std::vector<std::pair<std::string, std::vector<::string>>> title_columns;
        if (displayed_columns & ColumnData::Model) {
            title_columns.push_back({ _u8L("Filament"), {""} });
            title_columns.push_back({ _u8L("Model"), {buff} });
        }
        if (displayed_columns & ColumnData::Support) {
            title_columns.push_back({ _u8L("Support"), {buff} });
        }
        if (displayed_columns & ColumnData::Flushed) {
            title_columns.push_back({ _u8L("Flushed"), {buff} });
        }
        if (displayed_columns & ColumnData::WipeTower) {
            title_columns.push_back({ _u8L("Tower"), {buff} });
        }
        if ((displayed_columns & ~ColumnData::Model) > 0) {
            title_columns.push_back({ _u8L("Total"), {buff} });
        }
        auto offsets_ = calculate_offsets(title_columns, icon_size);
        std::vector<std::pair<std::string, float>> title_offsets;
        for (int i = 0; i < offsets_.size(); i++) {
            title_offsets.push_back({ title_columns[i].first, offsets_[i] });
            offsets[title_columns[i].first] = offsets_[i];
        }
        append_headers(title_offsets);
    }

    // item
    {
        size_t i = 0;
        for (auto it = model_volume_of_extruders_all_plates.begin(); it != model_volume_of_extruders_all_plates.end(); it++) {
            if (i < model_used_filaments_m_all_plates.size() && i < model_used_filaments_g_all_plates.size()) {
                std::vector<std::pair<std::string, float>> columns_offsets;
                columns_offsets.push_back({ std::to_string(it->first + 1), offsets[_u8L("Filament")]});

                char buf[64];
                double unit_conver = imperial_units ? GizmoObjectManipulation::oz_to_g : 1.0;

                float column_sum_m = 0.0f;
                float column_sum_g = 0.0f;
                if (displayed_columns & ColumnData::Model) {
                    if ((displayed_columns & ~ColumnData::Model) > 0)
                        ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", model_used_filaments_m_all_plates[i], model_used_filaments_g_all_plates[i] / unit_conver);
                    else
                        ::sprintf(buf, imperial_units ? "%.2f in    %.2f oz" : "%.2f m    %.2f g", model_used_filaments_m_all_plates[i], model_used_filaments_g_all_plates[i] / unit_conver);
                    columns_offsets.push_back({ buf, offsets[_u8L("Model")] });
                    column_sum_m += model_used_filaments_m_all_plates[i];
                    column_sum_g += model_used_filaments_g_all_plates[i];
                }
                if (displayed_columns & ColumnData::Support) {
                    ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", support_used_filaments_m_all_plates[i], support_used_filaments_g_all_plates[i] / unit_conver);
                    columns_offsets.push_back({ buf, offsets[_u8L("Support")] });
                    column_sum_m += support_used_filaments_m_all_plates[i];
                    column_sum_g += support_used_filaments_g_all_plates[i];
                }
                if (displayed_columns & ColumnData::Flushed) {
                    ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", flushed_filaments_m_all_plates[i], flushed_filaments_g_all_plates[i] / unit_conver);
                    columns_offsets.push_back({ buf, offsets[_u8L("Flushed")] });
                    column_sum_m += flushed_filaments_m_all_plates[i];
                    column_sum_g += flushed_filaments_g_all_plates[i];
                }
                if (displayed_columns & ColumnData::WipeTower) {
                    ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", wipe_tower_used_filaments_m_all_plates[i], wipe_tower_used_filaments_g_all_plates[i] / unit_conver);
                    columns_offsets.push_back({ buf, offsets[_u8L("Tower")] });
                    column_sum_m += wipe_tower_used_filaments_m_all_plates[i];
                    column_sum_g += wipe_tower_used_filaments_g_all_plates[i];
                }
                if ((displayed_columns & ~ColumnData::Model) > 0) {
                    ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", column_sum_m, column_sum_g / unit_conver);
                    columns_offsets.push_back({ buf, offsets[_u8L("Total")] });
                }

                append_item(filament_colors[it->first], columns_offsets);
            }
            i++;
        }

        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.title(_u8L("Total Estimation"));

        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.text(_u8L("Total time") + ":");
        ImGui::SameLine();
        imgui.text(short_time(get_time_dhms(total_time_all_plates)));

        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.text(_u8L("Total cost") + ":");
        ImGui::SameLine();
        char buf[64];
        ::sprintf(buf, "%.2f", total_cost_all_plates);
        imgui.text(buf);
    }
    ImGui::End();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(3);
    return;
}

void GCodeViewer::render_legend_color_arr_recommen(float window_padding)
{
    ImGuiWrapper &imgui = *wxGetApp().imgui();

    auto link_text = [&](const std::string &label) {
        ImVec2 wiki_part_size = ImGui::CalcTextSize(label.c_str());

        ImColor HyperColor = ImColor(0, 150, 136, 255).Value;
        ImGui::PushStyleColor(ImGuiCol_Text, HyperColor.Value);
        imgui.text(label.c_str());
        ImGui::PopStyleColor();

        // underline
        ImVec2 lineEnd = ImGui::GetItemRectMax();
        lineEnd.y -= 2.0f;
        ImVec2 lineStart = lineEnd;
        lineStart.x      = ImGui::GetItemRectMin().x;
        ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, HyperColor);
        // click behavior
        if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true)) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                Plater *plater = wxGetApp().plater();
                wxCommandEvent evt(EVT_OPEN_FILAMENT_MAP_SETTINGS_DIALOG);
                evt.SetEventObject(plater);
                evt.SetInt(1); // 1 means from gcode viewer
                wxPostEvent(plater, evt);
            }
        }
    };

    auto link_text_set_to_optional = [&](const std::string &label) {
        ImVec2 wiki_part_size = ImGui::CalcTextSize(label.c_str());

        ImColor HyperColor = ImColor(0, 150, 136, 255).Value;
        ImGui::PushStyleColor(ImGuiCol_Text, HyperColor.Value);
        imgui.text(label.c_str());
        ImGui::PopStyleColor();
        // underline
        ImVec2 lineEnd = ImGui::GetItemRectMax();
        lineEnd.y -= 2.0f;
        ImVec2 lineStart = lineEnd;
        lineStart.x      = ImGui::GetItemRectMin().x;
        ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, HyperColor);
        // click behavior
        if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true)) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                MessageDialog msg_dlg(nullptr, _L("Automatically re-slice according to the optimal filament grouping, and the grouping results will be displayed after slicing."), wxEmptyString, wxOK | wxCANCEL);
                if (msg_dlg.ShowModal() == wxID_OK) {
                    PartPlateList &partplate_list = wxGetApp().plater()->get_partplate_list();
                    PartPlate     *plate          = partplate_list.get_curr_plate();
                    plate->set_filament_map_mode(FilamentMapMode::fmmAutoForFlush);
                    Plater        *plater = wxGetApp().plater();
                    wxPostEvent(plater, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
                }
            }
        }
    };

    auto link_filament_group_wiki = [&](const std::string& label) {
        ImVec2 wiki_part_size = ImGui::CalcTextSize(label.c_str());

        ImColor HyperColor = ImColor(0, 150, 136, 255).Value;
        ImGui::PushStyleColor(ImGuiCol_Text, HyperColor.Value);
        imgui.text(label.c_str());
        ImGui::PopStyleColor();

        // underline
        ImVec2 lineEnd = ImGui::GetItemRectMax();
        lineEnd.y -= 2.0f;
        ImVec2 lineStart = lineEnd;
        lineStart.x = ImGui::GetItemRectMin().x;
        ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, HyperColor);
        // click behavior
        if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true)) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                std::string wiki_path = Slic3r::resources_dir() + "/wiki/filament_group_wiki_zh.html";
                wxLaunchDefaultBrowser(wxString(wiki_path.c_str()));
            }
        }
    };

    auto draw_dash_line = [&](ImDrawList* draw_list, int dash_length = 5, int gap_length = 3) {
        ImVec2 p1 = ImGui::GetCursorScreenPos();
        ImVec2 p2 = ImVec2(p1.x + ImGui::GetContentRegionAvail().x, p1.y);
        for (float i = p1.x; i < p2.x; i += (dash_length + gap_length)) {
            draw_list->AddLine(ImVec2(i, p1.y), ImVec2(i + dash_length, p1.y), ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.6f))); // ORCA match color
        }
    };

    ////BBS Color Arrangement Recommendation

    auto config            = wxGetApp().plater()->get_partplate_list().get_current_fff_print().config();
    auto stats_by_extruder = wxGetApp().plater()->get_partplate_list().get_current_fff_print().statistics_by_extruder();

    float delta_weight_to_single_ext = stats_by_extruder.stats_by_single_extruder.filament_flush_weight - stats_by_extruder.stats_by_multi_extruder_curr.filament_flush_weight;
    float delta_weight_to_best = stats_by_extruder.stats_by_multi_extruder_curr.filament_flush_weight - stats_by_extruder.stats_by_multi_extruder_best.filament_flush_weight;
    int   delta_change_to_single_ext = stats_by_extruder.stats_by_single_extruder.filament_change_count - stats_by_extruder.stats_by_multi_extruder_curr.filament_change_count;
    int   delta_change_to_best = stats_by_extruder.stats_by_multi_extruder_curr.filament_change_count - stats_by_extruder.stats_by_multi_extruder_best.filament_change_count;

    bool any_less_to_single_ext = delta_weight_to_single_ext > EPSILON || delta_change_to_single_ext > 0;
    bool any_more_to_best = delta_weight_to_best > EPSILON || delta_change_to_best > 0;
    bool all_less_to_single_ext = delta_weight_to_single_ext > EPSILON && delta_change_to_single_ext > 0;
    bool all_more_to_best = delta_weight_to_best > EPSILON && delta_change_to_best > 0;

    auto get_filament_display_type = [](const ExtruderFilament& filament) {
        if (filament.is_support_filament && (filament.type == "PLA" || filament.type == "PA" || filament.type == "ABS"))
            return "Sup." + filament.type;
        return filament.type;
        };


    // BBS AMS containers
    float line_height          = ImGui::GetFrameHeight();
    float ams_item_height = 0;
    float filament_group_item_align_width = 0;
    {
        float three_words_width    = imgui.calc_text_size("ABC"sv).x;
        const int line_capacity = 4;

        for (const auto& extruder_filaments : {m_left_extruder_filament,m_right_extruder_filament })
        {
            float container_height = 0.f;
            for (size_t idx = 0; idx < extruder_filaments.size(); idx += line_capacity) {
                float text_line_height = 0;
                for (int j = idx; j < extruder_filaments.size() && j < idx + line_capacity; ++j) {
                    auto text_info = imgui.calculate_filament_group_text_size(get_filament_display_type(extruder_filaments[j]));
                    auto text_size = std::get<0>(text_info);
                    filament_group_item_align_width = max(filament_group_item_align_width, text_size.x);
                    text_line_height = max(text_line_height, text_size.y);
                }
                container_height += (three_words_width * 1.3f + text_line_height );
            }
            container_height += 2 * line_height;
            ams_item_height = std::max(ams_item_height, container_height);
        }
    }

    int tips_count = 8;
    if (any_more_to_best) {
        tips_count = 8;
        if (wxGetApp().app_config->get("language") != "zh_CN")
            tips_count += 1;
    }
    else if (any_less_to_single_ext) {
        tips_count = 6;
        if (wxGetApp().app_config->get("language") != "zh_CN")
            tips_count += 1;
    }
    else
        tips_count = 5;

    float AMS_container_height = ams_item_height + line_height * tips_count + line_height;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0)); // this shold be 0 since its child of gcodeviewer
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(window_padding * 3, 0));

    // ImGui::Dummy({window_padding, window_padding});
    ImGui::BeginChild("#AMS", ImVec2(0, AMS_container_height), true, ImGuiWindowFlags_AlwaysUseWindowPadding);
    {
        float available_width   = ImGui::GetContentRegionAvail().x;
        float half_width       = available_width * 0.49f;
        float spacing           = 18.0f * m_scale;

        ImGui::Dummy({window_padding, window_padding});
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f,1.0f,1.0f,0.6f));
        imgui.bold_text(_u8L("Filament Grouping"));
        ImGui::SameLine();
        std::string tip_str = _u8L("Why this grouping");
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionWidth() - window_padding - ImGui::CalcTextSize(tip_str.c_str()).x);
        link_filament_group_wiki(tip_str);
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Dummy({window_padding, window_padding});

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 0.05f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(window_padding * 2, window_padding));

        ImDrawList *child_begin_draw_list = ImGui::GetWindowDrawList();
        ImVec2      cursor_pos            = ImGui::GetCursorScreenPos();
        child_begin_draw_list->AddRectFilled(cursor_pos, ImVec2(cursor_pos.x + half_width, cursor_pos.y + line_height), IM_COL32(255, 255, 255, 10));
        ImGui::BeginChild("#LeftAMS", ImVec2(half_width, ams_item_height), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
        {
            imgui.text(_u8L("Left nozzle"));
            ImGui::Dummy({window_padding, window_padding});
            int index = 1;
            for (const auto &extruder_filament : m_left_extruder_filament) {
                imgui.filament_group(get_filament_display_type(extruder_filament), extruder_filament.hex_color.c_str(), extruder_filament.filament_id, filament_group_item_align_width);
                if (index % 4 != 0) { ImGui::SameLine(0, spacing); }
                index++;
            }
            ImGui::EndChild();
        }
        ImGui::SameLine();
        cursor_pos = ImGui::GetCursorScreenPos();
        child_begin_draw_list->AddRectFilled(cursor_pos, ImVec2(cursor_pos.x + half_width, cursor_pos.y + line_height), IM_COL32(255, 255, 255, 10));
        ImGui::BeginChild("#RightAMS", ImVec2(half_width, ams_item_height), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
        {
            imgui.text(_u8L("Right nozzle"));
            ImGui::Dummy({window_padding, window_padding});
            int index = 1;
            for (const auto &extruder_filament : m_right_extruder_filament) {
                imgui.filament_group(get_filament_display_type(extruder_filament), extruder_filament.hex_color.c_str(), extruder_filament.filament_id, filament_group_item_align_width);
                if (index % 4 != 0) { ImGui::SameLine(0, spacing); }
                index++;
            }
            ImGui::EndChild();
        }
        ImGui::PopStyleColor(1);
        ImGui::PopStyleVar(1);

        ImGui::Dummy({window_padding, window_padding});
        imgui.text_wrapped(from_u8(_u8L("Please place filaments on the printer based on grouping result.")), ImGui::GetContentRegionAvail().x);
        ImGui::Dummy({window_padding, window_padding});

        {
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_dash_line(draw_list);
        }

        bool is_optimal_group = true;
        float parent_width = ImGui::GetContentRegionAvail().x;
        auto number_format = [](float num) {
            if (num > 1000) {
                std::string number_str = std::to_string(num);
                std::string first_three_digits = number_str.substr(0, 3);
                return std::stoi(first_three_digits);
            }
            return static_cast<int>(num);
        };

        if (any_more_to_best) {
            ImGui::Dummy({window_padding, window_padding});
            is_optimal_group = false;
            ImVec4 orangeColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, orangeColor);
            imgui.text(_u8L("Tips:"));
            imgui.text(_u8L("Current grouping of slice result is not optimal."));
            wxString tip;
            if (delta_weight_to_best >= 0 && delta_change_to_best >= 0)
                tip = from_u8((boost::format(_u8L("Increase %1%g filament and %2% changes compared to optimal grouping."))
                    % number_format(delta_weight_to_best)
                    % delta_change_to_best).str());
            else if (delta_weight_to_best >= 0 && delta_change_to_best < 0)
                tip = from_u8((boost::format(_u8L("Increase %1%g filament and save %2% changes compared to optimal grouping."))
                    % number_format(delta_weight_to_best)
                    % std::abs(delta_change_to_best)).str());
            else if (delta_weight_to_best < 0 && delta_change_to_best >= 0)
                tip = from_u8((boost::format(_u8L("Save %1%g filament and increase %2% changes compared to optimal grouping."))
                    % number_format(std::abs(delta_weight_to_best))
                    % delta_change_to_best).str());

            imgui.text_wrapped(tip, parent_width);
            ImGui::PopStyleColor(1);
        }
        else if (any_less_to_single_ext) {
            ImGui::Dummy({window_padding, window_padding});
            wxString tip;
            if (delta_weight_to_single_ext >= 0 && delta_change_to_single_ext >= 0)
                tip = from_u8((boost::format(_u8L("Save %1%g filament and %2% changes compared to a printer with one nozzle."))
                    % number_format(delta_weight_to_single_ext)
                    % delta_change_to_single_ext).str());
            else if (delta_weight_to_single_ext >= 0 && delta_change_to_single_ext < 0)
                tip = from_u8((boost::format(_u8L("Save %1%g filament and increase %2% changes compared to a printer with one nozzle."))
                    % number_format(delta_weight_to_single_ext)
                    % std::abs(delta_change_to_single_ext)).str());
            else if (delta_weight_to_single_ext < 0 && delta_change_to_single_ext >= 0)
                tip = from_u8((boost::format(_u8L("Increase %1%g filament and save %2% changes compared to a printer with one nozzle."))
                    % number_format(std::abs(delta_weight_to_single_ext))
                    % delta_change_to_single_ext).str());

            imgui.text_wrapped(tip, parent_width);
        }

        ImGui::Dummy({window_padding, window_padding});
        if (!is_optimal_group) {
            link_text_set_to_optional(_u8L("Set to Optimal"));
            ImGui::SameLine();
            ImGui::Dummy({window_padding, window_padding});
            ImGui::SameLine();
        }
        link_text(_u8L("Regroup filament"));

        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionWidth() - window_padding - ImGui::CalcTextSize("Tips").x);
        link_filament_group_wiki(_u8L("Tips"));

        ImGui::EndChild();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(1);
}

void GCodeViewer::render_legend(float &legend_height, int canvas_width, int canvas_height, int right_margin)
{
    if (!is_legend_shown())
        return;

    const Size cnv_size = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size();

    ImGuiWrapper& imgui = *wxGetApp().imgui();

    //BBS: GUI refactor: move to the right
    imgui.set_next_window_pos(float(canvas_width - right_margin * m_scale), 4.0f * m_scale, ImGuiCond_Always, 1.0f, 0.0f); // ORCA add a small gap to top to create seperation with main toolbar
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * m_scale); // ORCA add window rounding to modernize / match style
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0,0.0));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f,1.0f,1.0f,0.6f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.00f, 0.59f, 0.53f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.59f, 0.53f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.42f, 0.42f, 0.42f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
    //ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.f, 1.f, 1.f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, {1, 0, 0, 0});
    ImGui::SetNextWindowBgAlpha(0.8f);
    const float max_height = 0.75f * static_cast<float>(cnv_size.get_height());
    const float child_height = 0.3333f * max_height;
    ImGui::SetNextWindowSizeConstraints({ 0.0f, 0.0f }, { -1.0f, max_height });
    imgui.begin(std::string("Legend"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

    enum class EItemType : unsigned char
    {
        Rect,
        Circle,
        Hexagon,
        Line,
        None
    };

    const PrintEstimatedStatistics::Mode& time_mode = m_print_statistics.modes[static_cast<size_t>(m_viewer.get_time_mode())];
    const libvgcode::EViewType curr_view_type = m_viewer.get_view_type();
    const int curr_view_type_i = static_cast<int>(curr_view_type);
    bool show_estimated_time = time_mode.time > 0.0f && (curr_view_type == libvgcode::EViewType::FeatureType ||
        curr_view_type == libvgcode::EViewType::LayerTimeLinear || curr_view_type == libvgcode::EViewType::LayerTimeLogarithmic ||
        (curr_view_type == libvgcode::EViewType::ColorPrint && !time_mode.custom_gcode_times.empty()));

    const float icon_size = ImGui::GetTextLineHeight() * 0.7;
    //BBS GUI refactor
    //const float percent_bar_size = 2.0f * ImGui::GetTextLineHeight();
    const float percent_bar_size = 0;

    bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos_rect = ImGui::GetCursorScreenPos();
    float window_padding = 4.0f * m_scale;

    // ORCA dont use background on top bar to give modern look
    //draw_list->AddRectFilled(ImVec2(pos_rect.x,pos_rect.y - ImGui::GetStyle().WindowPadding.y),
    //ImVec2(pos_rect.x + ImGui::GetWindowWidth() + ImGui::GetFrameHeight(),pos_rect.y + ImGui::GetFrameHeight() + window_padding * 2.5),
    //ImGui::GetColorU32(ImVec4(0,0,0,0.3)));

    auto append_item = [icon_size, &imgui, imperial_units, &window_padding, &draw_list, this](
        EItemType type,
        const ColorRGBA& color,
        const std::vector<std::pair<std::string, float>>& columns_offsets,
        bool checkbox = true,
        float checkbox_pos = 0.f, // ORCA use calculated value for eye icon. Aligned to "Display" header or end of combo box
        bool visible = true,
        std::function<void()> callback = nullptr)
    {
        // render icon
        ImVec2 pos = ImVec2(ImGui::GetCursorScreenPos().x + window_padding * 3, ImGui::GetCursorScreenPos().y);
        switch (type) {
        default:
        case EItemType::Rect: {
            draw_list->AddRectFilled({ pos.x + 1.0f * m_scale, pos.y + 3.0f * m_scale }, { pos.x + icon_size - 1.0f * m_scale, pos.y + icon_size + 1.0f * m_scale },
                                     ImGuiWrapper::to_ImU32(color));
            break;
        }
        case EItemType::Circle: {
            ImVec2 center(0.5f * (pos.x + pos.x + icon_size), 0.5f * (pos.y + pos.y + icon_size + 5.0f));
            draw_list->AddCircleFilled(center, 0.5f * icon_size, ImGuiWrapper::to_ImU32(color), 16);
            break;
        }
        case EItemType::Hexagon: {
            ImVec2 center(0.5f * (pos.x + pos.x + icon_size), 0.5f * (pos.y + pos.y + icon_size + 5.0f));
            draw_list->AddNgonFilled(center, 0.5f * icon_size, ImGuiWrapper::to_ImU32(color), 6);
            break;
        }
        case EItemType::Line: {
            draw_list->AddLine({ pos.x + 1, pos.y + icon_size + 2 }, { pos.x + icon_size - 1, pos.y + 4 }, ImGuiWrapper::to_ImU32(color), 3.0f);
            break;
        case EItemType::None:
            break;
        }
        }

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(20.0 * m_scale, 6.0 * m_scale));

        // BBS render selectable
        ImGui::Dummy({ 0.0, 0.0 });
        ImGui::SameLine();
        if (callback) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * m_scale);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0 * m_scale, 0.0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.00f, 0.68f, 0.26f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1.00f, 0.68f, 0.26f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
            float max_height = 0.f;
            for (auto column_offset : columns_offsets) {
                if (ImGui::CalcTextSize(column_offset.first.c_str()).y > max_height)
                    max_height = ImGui::CalcTextSize(column_offset.first.c_str()).y;
            }
            bool b_menu_item = ImGui::BBLMenuItem(("##" + columns_offsets[0].first).c_str(), nullptr, false, true, max_height);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);
            if (b_menu_item)
                callback();
            if (checkbox) {
                // ORCA replace checkboxes with eye icon
                // Use calculated position from argument. this method has predictable result compared to alingning button using window width
                // fixes slowly resizing window and endlessly expanding window when there is a miscalculation on position
                ImGui::SameLine(checkbox_pos);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0, 0.0)); // ensure no padding active
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0, 0.0)); // ensure no item spacing active
                ImGui::Text("%s", into_u8(visible ? ImGui::VisibleIcon : ImGui::HiddenIcon).c_str());
                ImGui::PopStyleVar(2);
            }
        }

        // BBS render column item
        {
            if(callback && !checkbox && !visible)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(172 / 255.0f, 172 / 255.0f, 172 / 255.0f, 1.00f));
            float dummy_size = type == EItemType::None ? window_padding * 3 : ImGui::GetStyle().ItemSpacing.x + icon_size;
            ImGui::SameLine(dummy_size);
            imgui.text(columns_offsets[0].first);

            for (auto i = 1; i < columns_offsets.size(); i++) {
                ImGui::SameLine(columns_offsets[i].second);
                imgui.text(columns_offsets[i].first);
            }
            if (callback && !checkbox && !visible)
                ImGui::PopStyleColor(1);
        }

        ImGui::PopStyleVar(1);

    };

    auto append_range = [append_item](const libvgcode::ColorRange& range, unsigned int decimals) {
        auto append_range_item = [append_item, &range](int i, float value, unsigned int decimals) {
            char buf[1024];
            ::sprintf(buf, "%.*f", decimals, value);
            append_item(EItemType::Rect, libvgcode::convert(range.get_palette()[i]), { { buf , 0} });
        };

        std::vector<float> values = range.get_values();
        if (values.size() == 1)
            // single item use case
            append_range_item(0, values.front(), decimals);
        else if (values.size() == 2) {
            append_range_item(static_cast<int>(range.get_palette().size()) - 1, values.back(), decimals);
            append_range_item(0, values.front(), decimals);
        }
        else {
            for (int i = static_cast<int>(range.get_palette().size()) - 1; i >= 0; --i) {
                append_range_item(i, values[i], decimals);
            }
        }
    };

    auto append_headers = [&imgui, window_padding, this](const std::vector<std::pair<std::string, float>>& title_offsets) {
        for (size_t i = 0; i < title_offsets.size(); i++) {
            if (title_offsets[i].first == _u8L("Display")) { // ORCA Hide Display header
                ImGui::SameLine(title_offsets[i].second);
                ImGui::Dummy({16.f * m_scale, 1}); // 16(icon_size)
                continue;
            }
            ImGui::SameLine(title_offsets[i].second);
            imgui.bold_text(title_offsets[i].first);
        }
        // Ensure right padding
        ImGui::SameLine();
        ImGui::Dummy({window_padding, 1});
        ImGui::Separator();
    };

    auto max_width = [](const std::vector<std::string>& items, const std::string& title, float extra_size = 0.0f) {
        float ret = ImGui::CalcTextSize(title.c_str()).x;
        for (const std::string& item : items) {
            ret = std::max(ret, extra_size + ImGui::CalcTextSize(item.c_str()).x);
        }
        return ret;
    };

    auto calculate_offsets = [&imgui, max_width, window_padding, this](const std::vector<std::pair<std::string, std::vector<::string>>>& title_columns, float extra_size = 0.0f) {
            const ImGuiStyle& style = ImGui::GetStyle();
            std::vector<float> offsets;
            // ORCA increase spacing for more readable format. Using direct number requires much less code change in here. GetTextLineHeight for additional spacing for icon_size
            offsets.push_back(max_width(title_columns[0].second, title_columns[0].first, extra_size) + 12.f * m_scale + ImGui::GetTextLineHeight());
            for (size_t i = 1; i < title_columns.size() - 1; i++) // ORCA dont add extra spacing after icon / "Display" header
                offsets.push_back(offsets.back() + max_width(title_columns[i].second, title_columns[i].first) + ((title_columns[i].first == _u8L("Display") ? 0 : 12.f) * m_scale));
            if (title_columns.back().first == _u8L("Display") && title_columns.size() > 2)
                offsets[title_columns.size() - 2] -= 3.f; // ORCA reduce spacing after previous header

            float average_col_width = ImGui::GetWindowWidth() / static_cast<float>(title_columns.size());
            std::vector<float> ret;
            ret.push_back(0);
            for (size_t i = 1; i < title_columns.size(); i++) {
                ret.push_back(std::max(offsets[i - 1], i * average_col_width));
            }
            return ret;
    };

    auto color_print_ranges = [this](unsigned char extruder_id, const std::vector<CustomGCode::Item>& custom_gcode_per_print_z) {
        std::vector<std::pair<ColorRGBA, std::pair<double, double>>> ret;
        ret.reserve(custom_gcode_per_print_z.size());

        for (const auto& item : custom_gcode_per_print_z) {
            if (extruder_id + 1 != static_cast<unsigned char>(item.extruder))
                continue;

            if (item.type != ColorChange)
                continue;

            const std::vector<float> zs = m_viewer.get_layers_zs();
            auto lower_b = std::lower_bound(zs.begin(), zs.end(),
                static_cast<float>(item.print_z - epsilon()));
            if (lower_b == zs.end())
                continue;

            const double current_z = *lower_b;
            const double previous_z = (lower_b == zs.begin()) ? 0.0 : *(--lower_b);

            // to avoid duplicate values, check adding values
            if (ret.empty() || !(ret.back().second.first == previous_z && ret.back().second.second == current_z))
            {
                ColorRGBA color;
                decode_color(item.color, color);
                ret.push_back({ color, { previous_z, current_z } });
            }
        }

        return ret;
    };

    auto upto_label = [](double z) {
        char buf[64];
        ::sprintf(buf, "%.2f", z);
        return _u8L("up to") + " " + std::string(buf) + " " + _u8L("mm");
    };

    auto above_label = [](double z) {
        char buf[64];
        ::sprintf(buf, "%.2f", z);
        return _u8L("above") + " " + std::string(buf) + " " + _u8L("mm");
    };

    auto fromto_label = [](double z1, double z2) {
        char buf1[64];
        ::sprintf(buf1, "%.2f", z1);
        char buf2[64];
        ::sprintf(buf2, "%.2f", z2);
        return _u8L("from") + " " + std::string(buf1) + " " + _u8L("to") + " " + std::string(buf2) + " " + _u8L("mm");
    };

    auto role_time_and_percent = [this, time_mode](libvgcode::EGCodeExtrusionRole role) {
        const float time = m_viewer.get_extrusion_role_estimated_time(role);
        return std::make_pair(time, time / time_mode.time);
    };

    auto travel_time_and_percent = [this, time_mode]() {
        const float time = m_viewer.get_travels_estimated_time();
        return std::make_pair(time, time / time_mode.time);
    };

    auto used_filament_per_role = [this, imperial_units](ExtrusionRole role) {
        auto it = m_print_statistics.used_filaments_per_role.find(role);
        if (it == m_print_statistics.used_filaments_per_role.end())
            return std::make_pair(0.0, 0.0);

        double koef        = imperial_units ? GizmoObjectManipulation::in_to_mm / 1000.0 : 1.0;
        double unit_conver = imperial_units ? GizmoObjectManipulation::oz_to_g : 1;
        return std::make_pair(it->second.first / koef, it->second.second / unit_conver);
    };

    // get used filament (meters and grams) from used volume in respect to the active extruder
    auto get_used_filament_from_volume = [this, imperial_units](double volume, int extruder_id) {
        double koef = imperial_units ? 1.0 / GizmoObjectManipulation::in_to_mm : 0.001;
        std::pair<double, double> ret = { koef * volume / (PI * sqr(0.5 * m_filament_diameters[extruder_id])),
                                          volume * m_filament_densities[extruder_id] * 0.001 };
        return ret;
    };

    //BBS display Color Scheme
    ImGui::Dummy({ window_padding, window_padding });
    ImGui::Dummy({ window_padding, window_padding });
    ImGui::SameLine(window_padding * 2); // ORCA Ignores item spacing to get perfect window margins since since this part uses dummies for window padding
    std::wstring btn_name;
    if (m_fold)
        btn_name = ImGui::UnfoldButtonIcon;
    else
        btn_name = ImGui::FoldButtonIcon;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(84 / 255.f, 84 / 255.f, 90 / 255.f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(84 / 255.f, 84 / 255.f, 90 / 255.f, 1.f));
    float calc_padding = (ImGui::GetFrameHeight() - 16 * m_scale) / 2;                      // ORCA calculated padding for 16x16 icon
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(calc_padding, calc_padding));    // ORCA Center icon with frame padding
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * m_scale);                       // ORCA Match button style with combo box

    float button_width = 16 * m_scale + calc_padding * 2;                                   // ORCA match buttons height with combo box
    if (ImGui::Button(into_u8(btn_name).c_str(), ImVec2(button_width, button_width))) {
        m_fold = !m_fold;
    }

    ImGui::SameLine();
    const wchar_t gCodeToggle = ImGui::gCodeButtonIcon;
    if (ImGui::Button(into_u8(gCodeToggle).c_str(), ImVec2(button_width, button_width))) {
        wxGetApp().toggle_show_gcode_window();
        wxGetApp().plater()->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT));
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    //imgui.bold_text(_u8L("Color Scheme"));
    push_combo_style();

    ImGui::SameLine();
    const char* view_type_value = view_type_items_str[m_view_type_sel].c_str();
    ImGuiComboFlags flags = ImGuiComboFlags_HeightLargest; // ORCA allow to fit all items to prevent scrolling on reaching last elements
    if (ImGui::BBLBeginCombo("", view_type_value, flags)) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        for (int i = 0; i < view_type_items_str.size(); i++) {
            const bool is_selected = (m_view_type_sel == i);
            if (ImGui::BBLSelectable(view_type_items_str[i].c_str(), is_selected)) {
                m_fold = false;
                m_view_type_sel = i;
                set_view_type(view_type_items[m_view_type_sel]);
                reset_visible(view_type_items[m_view_type_sel]);
                update_moves_slider();
            #if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
                imgui.set_requires_extra_frame();
            #else
                wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
                wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
            #endif
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::PopStyleVar(1);
        ImGui::EndCombo();
    }
    pop_combo_style();
    ImGui::SameLine(0, window_padding);               // ORCA Without (0,window_padding) it adds unnecessary item spacing after combo box
                                                      // ORCA predictable_icon_pos helpful when window size determined by combo box.
    float predictable_icon_pos = ImGui::GetCursorPosX() - icon_size - window_padding - ImGui::GetStyle().ItemSpacing.x - 1.f * m_scale; // 1 for border
    ImGui::Dummy({ window_padding, window_padding });
    ImGui::Dummy({ window_padding, window_padding }); // ORCA Matches top-bottom window paddings
    float window_width = ImGui::GetWindowWidth();     // ORCA Store window width

    if (m_fold) {
        legend_height = ImGui::GetFrameHeight() + window_padding * 4; // ORCA using 4 instead 2 gives correct toolbar margins while its folded
        ImGui::SameLine(window_width);                // ORCA use stored window width while folded. This prevents annoying position change on fold/expand button
        ImGui::Dummy({ 0, 0 });
        imgui.end();
        ImGui::PopStyleColor(7);
        ImGui::PopStyleVar(2);
        return;
    }

    // data used to properly align items in columns when showing time
    std::vector<float> offsets;
    std::vector<std::string> labels;
    std::vector<std::string> times;
    std::string travel_time;
    std::vector<std::string> percents;
    std::vector<std::string> used_filaments_length;
    std::vector<std::string> used_filaments_weight;
    std::string travel_percent;
    std::vector<double> model_used_filaments_m;
    std::vector<double> model_used_filaments_g;
    double total_model_used_filament_m = 0, total_model_used_filament_g = 0;
    std::vector<double> flushed_filaments_m;
    std::vector<double> flushed_filaments_g;
    double total_flushed_filament_m = 0, total_flushed_filament_g = 0;
    std::vector<double> wipe_tower_used_filaments_m;
    std::vector<double> wipe_tower_used_filaments_g;
    double total_wipe_tower_used_filament_m = 0, total_wipe_tower_used_filament_g = 0;
    std::vector<double> support_used_filaments_m;
    std::vector<double> support_used_filaments_g;
    double total_support_used_filament_m = 0, total_support_used_filament_g = 0;
    struct ColumnData {
        enum {
            Model = 1,
            Flushed = 2,
            WipeTower = 4,
            Support = 1 << 3,
        };
    };
    int displayed_columns = 0;
    std::map<std::string, float> color_print_offsets;
    const PrintStatistics& ps = wxGetApp().plater()->get_partplate_list().get_current_fff_print().print_statistics();
    double koef = imperial_units ? GizmoObjectManipulation::in_to_mm : 1000.0;
    double unit_conver = imperial_units ? GizmoObjectManipulation::oz_to_g : 1;
    std::vector<double> used_filaments_m;
    std::vector<double> used_filaments_g;

    // used filament statistics
    for (size_t extruder_id : m_viewer.get_used_extruders_ids()) {
        if (m_print_statistics.model_volumes_per_extruder.find(extruder_id) == m_print_statistics.model_volumes_per_extruder.end()) {
            model_used_filaments_m.push_back(0.0);
            model_used_filaments_g.push_back(0.0);
        }
        else {
            double volume = m_print_statistics.model_volumes_per_extruder.at(extruder_id);
            auto [model_used_filament_m, model_used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
            model_used_filaments_m.push_back(model_used_filament_m);
            model_used_filaments_g.push_back(model_used_filament_g);
            total_model_used_filament_m += model_used_filament_m;
            total_model_used_filament_g += model_used_filament_g;
            displayed_columns |= ColumnData::Model;
        }
    }

    for (size_t extruder_id : m_viewer.get_used_extruders_ids()) {
        if (m_print_statistics.wipe_tower_volumes_per_extruder.find(extruder_id) == m_print_statistics.wipe_tower_volumes_per_extruder.end()) {
            wipe_tower_used_filaments_m.push_back(0.0);
            wipe_tower_used_filaments_g.push_back(0.0);
        }
        else {
            double volume = m_print_statistics.wipe_tower_volumes_per_extruder.at(extruder_id);
            auto [wipe_tower_used_filament_m, wipe_tower_used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
            wipe_tower_used_filaments_m.push_back(wipe_tower_used_filament_m);
            wipe_tower_used_filaments_g.push_back(wipe_tower_used_filament_g);
            total_wipe_tower_used_filament_m += wipe_tower_used_filament_m;
            total_wipe_tower_used_filament_g += wipe_tower_used_filament_g;
            displayed_columns |= ColumnData::WipeTower;
        }
    }

    for (size_t extruder_id : m_viewer.get_used_extruders_ids()) {
        if (m_print_statistics.flush_per_filament.find(extruder_id) == m_print_statistics.flush_per_filament.end()) {
            flushed_filaments_m.push_back(0.0);
            flushed_filaments_g.push_back(0.0);
        }
        else {
            double volume = m_print_statistics.flush_per_filament.at(extruder_id);
            auto [flushed_filament_m, flushed_filament_g] = get_used_filament_from_volume(volume, extruder_id);
            flushed_filaments_m.push_back(flushed_filament_m);
            flushed_filaments_g.push_back(flushed_filament_g);
            total_flushed_filament_m += flushed_filament_m;
            total_flushed_filament_g += flushed_filament_g;
            displayed_columns |= ColumnData::Flushed;
        }
    }

    for (size_t extruder_id : m_viewer.get_used_extruders_ids()) {
        if (m_print_statistics.support_volumes_per_extruder.find(extruder_id) == m_print_statistics.support_volumes_per_extruder.end()) {
            support_used_filaments_m.push_back(0.0);
            support_used_filaments_g.push_back(0.0);
        }
        else {
            double volume = m_print_statistics.support_volumes_per_extruder.at(extruder_id);
            auto [used_filament_m, used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
            support_used_filaments_m.push_back(used_filament_m);
            support_used_filaments_g.push_back(used_filament_g);
            total_support_used_filament_m += used_filament_m;
            total_support_used_filament_g += used_filament_g;
            displayed_columns |= ColumnData::Support;
        }
    }


    // extrusion paths section -> title
    ImGui::Dummy({ window_padding, window_padding });
    ImGui::SameLine();
    switch (curr_view_type)
    {
    case libvgcode::EViewType::FeatureType:
    {
        // calculate offsets to align time/percentage data
        char buffer[64];
        const std::vector<libvgcode::EGCodeExtrusionRole>& roles = m_viewer.get_extrusion_roles();
        for (libvgcode::EGCodeExtrusionRole role : roles) {
            assert(static_cast<size_t>(role) < libvgcode::GCODE_EXTRUSION_ROLES_COUNT);
            if (static_cast<size_t>(role) < libvgcode::GCODE_EXTRUSION_ROLES_COUNT) {
                labels.push_back(_u8L(ExtrusionEntity::role_to_string(convert(role))));
                auto [time, percent] = role_time_and_percent(role);
                times.push_back((time > 0.0f) ? short_time(get_time_dhms(time)) : "");
                if (percent == 0) // ORCA remove % symbol from rows
                    ::sprintf(buffer, "0");
                else
                    percent > 0.001 ? ::sprintf(buffer, "%.1f", percent * 100) : ::sprintf(buffer, "<0.1");
                percents.push_back(buffer);

                auto [model_used_filament_m, model_used_filament_g] = used_filament_per_role(convert(role));
                ::sprintf(buffer, imperial_units ? "%.2fin" : "%.2fm", model_used_filament_m); // ORCA dont use spacing between value and unit
                used_filaments_length.push_back(buffer);
                ::sprintf(buffer, imperial_units ? "%.2foz" : "%.2fg", model_used_filament_g); // ORCA dont use spacing between value and unit
                used_filaments_weight.push_back(buffer);
            }
        }

        //BBS: get travel time and percent
        {
            auto [time, percent] = travel_time_and_percent();
            travel_time = (time > 0.0f) ? short_time(get_time_dhms(time)) : "";
            if (percent == 0) // ORCA remove % symbol from rows
                ::sprintf(buffer, "0");
            else
                percent > 0.001 ? ::sprintf(buffer, "%.1f", percent * 100) : ::sprintf(buffer, "<0.1");
            travel_percent = buffer;
        }

        // ORCA use % symbol for percentage and use "Usage" for "Used filaments"
        offsets = calculate_offsets({ {_u8L("Line Type"), labels}, {_u8L("Time"), times}, {"%", percents}, {"", used_filaments_length}, {"", used_filaments_weight}, {_u8L("Display"), {""}}}, icon_size);
        append_headers({{_u8L("Line Type"), offsets[0]}, {_u8L("Time"), offsets[1]}, {"%", offsets[2]}, {_u8L("Usage"), offsets[3]}, {_u8L("Display"), offsets[5]}});
        break;
    }
    case libvgcode::EViewType::Height:         { imgui.title(_u8L("Layer Height (mm)")); break; }
    case libvgcode::EViewType::Width:          { imgui.title(_u8L("Line Width (mm)")); break; }
    case libvgcode::EViewType::Speed:
    {
        imgui.title(_u8L("Speed (mm/s)"));
        break;
    }
    case libvgcode::EViewType::ActualSpeed:
    {
        imgui.title(_u8L("Actual Speed (mm/s)"));
        break;
    }
    case libvgcode::EViewType::FanSpeed:       { imgui.title(_u8L("Fan Speed (%)")); break; }
    case libvgcode::EViewType::Temperature:    { imgui.title(_u8L("Temperature (°C)")); break; }
    case libvgcode::EViewType::VolumetricFlowRate:
        { imgui.title(_u8L("Volumetric flow rate (mm³/s)")); break; }
    case libvgcode::EViewType::ActualVolumetricFlowRate:
        { imgui.title(_u8L("Actual volumetric flow rate (mm³/s)")); break; }
    case libvgcode::EViewType::LayerTimeLinear:
        { imgui.title(_u8L("Layer Time")); break; }
    case libvgcode::EViewType::LayerTimeLogarithmic:
        { imgui.title(_u8L("Layer Time (log)")); break; }

    case libvgcode::EViewType::Tool:
    {
        // calculate used filaments data
        const size_t extruders_count = get_extruders_count();
        used_filaments_m = std::vector<double>(extruders_count, 0.0);
        used_filaments_g = std::vector<double>(extruders_count, 0.0);
        const std::vector<uint8_t>& used_extruders_ids = m_viewer.get_used_extruders_ids();
        for (uint8_t extruder_id : used_extruders_ids) {
            if (m_print_statistics.model_volumes_per_extruder.find(extruder_id) == m_print_statistics.model_volumes_per_extruder.end())
                continue;
            double volume = m_print_statistics.model_volumes_per_extruder.at(extruder_id);

            auto [model_used_filament_m, model_used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
            model_used_filaments_m.push_back(model_used_filament_m);
            model_used_filaments_g.push_back(model_used_filament_g);
        }

        offsets = calculate_offsets({ { "Extruder NNN", {""}}}, icon_size);
        append_headers({ {_u8L("Filament"), offsets[0]}, {_u8L("Usage"), offsets[1]} });
        break;
    }
    case libvgcode::EViewType::ColorPrint:
    {
        std::vector<std::string> total_filaments;
        char buffer[64];
        ::sprintf(buffer, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", ps.total_used_filament / /*1000*/koef, ps.total_weight / unit_conver);
        total_filaments.push_back(buffer);


        std::vector<std::pair<std::string, std::vector<::string>>> title_columns;
        if (displayed_columns & ColumnData::Model) {
            title_columns.push_back({ _u8L("Filament"), {""} });
            title_columns.push_back({ _u8L("Model"), total_filaments });
        }
        if (displayed_columns & ColumnData::Support) {
            title_columns.push_back({ _u8L("Support"), total_filaments });
        }
        if (displayed_columns & ColumnData::Flushed) {
            title_columns.push_back({ _u8L("Flushed"), total_filaments });
        }
        if (displayed_columns & ColumnData::WipeTower) {
            title_columns.push_back({ _u8L("Tower"), total_filaments });
        }
        if ((displayed_columns & ~ColumnData::Model) > 0) {
            title_columns.push_back({ _u8L("Total"), total_filaments });
        }
        title_columns.push_back({ _u8L("Display"), {""}}); // ORCA Add spacing for eye icon. used as color_print_offsets[_u8L("Display")]
        auto offsets_ = calculate_offsets(title_columns, icon_size);
        std::vector<std::pair<std::string, float>> title_offsets;
        for (int i = 0; i < offsets_.size(); i++) {
            title_offsets.push_back({ title_columns[i].first, offsets_[i] });
            color_print_offsets[title_columns[i].first] = offsets_[i];
        }
        append_headers(title_offsets);

        break;
    }
    default: { break; }
    }

    auto append_option_item = [this, append_item](libvgcode::EOptionType type, std::vector<float> offsets) {
        auto append_option_item_with_type = [this, offsets, append_item](libvgcode::EOptionType type, const ColorRGBA& color, const std::string& label, bool visible) {
            append_item(EItemType::Rect, color, {{ label , offsets[0] }}, true, offsets.back()/*ORCA checkbox_pos*/, visible, [this, type, visible]() {
                m_viewer.toggle_option_visibility(type);
                update_moves_slider();
                });
        };
        const bool visible = m_viewer.is_option_visible(type);
        if (type == libvgcode::EOptionType::Travels) {
            //BBS: only display travel time in FeatureType view
            append_option_item_with_type(type, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Travels)), _u8L("Travel"), visible);
        }
        else if (type == libvgcode::EOptionType::Seams)
            append_option_item_with_type(type, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Seams)), _u8L("Seams"), visible);
        else if (type == libvgcode::EOptionType::Retractions)
            append_option_item_with_type(type, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Retractions)), _u8L("Retract"), visible);
        else if (type == libvgcode::EOptionType::Unretractions)
            append_option_item_with_type(type, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Unretractions)), _u8L("Unretract"), visible);
        else if (type == libvgcode::EOptionType::ToolChanges)
            append_option_item_with_type(type, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::ToolChanges)), _u8L("Filament Changes"), visible);
        else if (type == libvgcode::EOptionType::Wipes)
            append_option_item_with_type(type, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Wipes)), _u8L("Wipe"), visible);
    };

    const libvgcode::EViewType new_view_type = curr_view_type;

    // extrusion paths section -> items
    switch (new_view_type)
    {
    case libvgcode::EViewType::FeatureType:
    {
        auto roles = m_viewer.get_extrusion_roles();
        for (size_t i = 0; i < roles.size(); ++i) {
            libvgcode::EGCodeExtrusionRole role = roles[i];
            if (role >= libvgcode::EGCodeExtrusionRole::COUNT)
                continue;
            const bool visible = m_viewer.is_extrusion_role_visible(role);
            std::vector<std::pair<std::string, float>> columns_offsets;
            columns_offsets.push_back({ labels[i], offsets[0] });
            columns_offsets.push_back({ times[i], offsets[1] });
            columns_offsets.push_back({percents[i], offsets[2]});
            columns_offsets.push_back({used_filaments_length[i], offsets[3]});
            columns_offsets.push_back({used_filaments_weight[i], offsets[4]});
            append_item(EItemType::Rect, libvgcode::convert(m_viewer.get_extrusion_role_color(role)), columns_offsets,
                true, offsets.back(), visible, [this, role, visible]() {
                    m_viewer.toggle_extrusion_role_visibility(role);
                    update_moves_slider();
                });
        }

        auto options = m_viewer.get_options();
        for(auto item : options) {
            if (item != libvgcode::EOptionType::Travels) {
                append_option_item(item, offsets);
            } else {
                //BBS: show travel time in FeatureType view
                const bool visible = m_viewer.is_option_visible(item);
                std::vector<std::pair<std::string, float>> columns_offsets;
                columns_offsets.push_back({ _u8L("Travel"), offsets[0] });
                columns_offsets.push_back({ travel_time, offsets[1] });
                columns_offsets.push_back({ travel_percent, offsets[2] });
                append_item(EItemType::Rect, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Travels)), columns_offsets, true, offsets.back()/*ORCA checkbox_pos*/, visible, [this, item, visible]() {
                        m_viewer.toggle_option_visibility(item);
                        update_moves_slider();
                    });
            }
        }
        break;
    }
    case libvgcode::EViewType::Height:                   { append_range(m_viewer.get_color_range(libvgcode::EViewType::Height), 2); break; }
    case libvgcode::EViewType::Width:                    { append_range(m_viewer.get_color_range(libvgcode::EViewType::Width), 2); break; }
    case libvgcode::EViewType::Speed:       {
        append_range(m_viewer.get_color_range(libvgcode::EViewType::Speed), 0);
        ImGui::Spacing();
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        offsets = calculate_offsets({ { _u8L("Options"), { _u8L("Travel")}}, { _u8L("Display"), {""}} }, icon_size);
        append_headers({ {_u8L("Options"), offsets[0] }, { _u8L("Display"), offsets[1]} });
        const bool travel_visible = m_viewer.is_option_visible(libvgcode::EOptionType::Travels);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 3.0f));
        append_item(EItemType::None, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Travels)), { {_u8L("travel"), offsets[0] }}, true, predictable_icon_pos/*ORCA checkbox_pos*/, travel_visible, [this, travel_visible]() {
            m_viewer.toggle_option_visibility(libvgcode::EOptionType::Travels);
            // refresh(*m_gcode_result, wxGetApp().plater()->get_extruder_colors_from_plater_config(m_gcode_result));
            update_moves_slider();
            });
        ImGui::PopStyleVar(1);
        break;
    }
    case libvgcode::EViewType::ActualSpeed: {
        append_range(m_viewer.get_color_range(libvgcode::EViewType::ActualSpeed), 0);
        ImGui::Spacing();
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        offsets = calculate_offsets({ { _u8L("Options"), { _u8L("Travel")}}, { _u8L("Display"), {""}} }, icon_size);
        append_headers({ {_u8L("Options"), offsets[0] }, { _u8L("Display"), offsets[1]} });
        const bool travel_visible = m_viewer.is_option_visible(libvgcode::EOptionType::Travels);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 3.0f));
        append_item(EItemType::None, libvgcode::convert(m_viewer.get_option_color(libvgcode::EOptionType::Travels)), { {_u8L("travel"), offsets[0] }}, true, predictable_icon_pos/*ORCA checkbox_pos*/, travel_visible, [this, travel_visible]() {
            m_viewer.toggle_option_visibility(libvgcode::EOptionType::Travels);
            // refresh(*m_gcode_result, wxGetApp().plater()->get_extruder_colors_from_plater_config(m_gcode_result));
            update_moves_slider();
            });
        ImGui::PopStyleVar(1);
        break;
    }
    case libvgcode::EViewType::FanSpeed:                 { append_range(m_viewer.get_color_range(libvgcode::EViewType::FanSpeed), 0); break; }
    case libvgcode::EViewType::Temperature:              { append_range(m_viewer.get_color_range(libvgcode::EViewType::Temperature), 0); break; }
    case libvgcode::EViewType::LayerTimeLinear:          { append_range(m_viewer.get_color_range(libvgcode::EViewType::LayerTimeLinear), true); break; }
    case libvgcode::EViewType::LayerTimeLogarithmic:     { append_range(m_viewer.get_color_range(libvgcode::EViewType::LayerTimeLogarithmic), true); break; }
    case libvgcode::EViewType::VolumetricFlowRate:       { append_range(m_viewer.get_color_range(libvgcode::EViewType::VolumetricFlowRate), 2); break; }
    case libvgcode::EViewType::ActualVolumetricFlowRate: { append_range(m_viewer.get_color_range(libvgcode::EViewType::ActualVolumetricFlowRate), 2); break; }
    case libvgcode::EViewType::Tool:
    {
        // shows only extruders actually used
        char buf[64];
        size_t i = 0;
        const std::vector<uint8_t>& used_extruders_ids = m_viewer.get_used_extruders_ids();
        for (uint8_t extruder_id : used_extruders_ids) {
            ::sprintf(buf, imperial_units ? "%.2f in    %.2f g" : "%.2f m    %.2f g", model_used_filaments_m[i], model_used_filaments_g[i]);
            append_item(EItemType::Rect, libvgcode::convert(m_viewer.get_tool_colors()[extruder_id]), { { _u8L("Extruder") + " " + std::to_string(extruder_id + 1), offsets[0]}, {buf, offsets[1]} });
            // append_item(EItemType::Rect, libvgcode::convert(m_viewer.get_tool_colors()[extruder_id]), _u8L("Extruder") + " " + std::to_string(extruder_id + 1),
            // true, "", 0.0f, 0.0f, offsets, used_filaments_m[extruder_id], used_filaments_g[extruder_id]);
            i++;
        }
        break;
    }
    case libvgcode::EViewType::Summary:
    {
        char buf[64];
        imgui.text(_u8L("Total") + ":");
        ImGui::SameLine();
        ::sprintf(buf, imperial_units ? "%.2f in / %.2f oz" : "%.2f m / %.2f g", ps.total_used_filament / koef, ps.total_weight / unit_conver);
        imgui.text(buf);

        ImGui::Dummy({window_padding, window_padding});
        ImGui::SameLine();
        imgui.text(_u8L("Cost") + ":");
        ImGui::SameLine();
        ::sprintf(buf, "%.2f", ps.total_cost);
        imgui.text(buf);

        ImGui::Dummy({window_padding, window_padding});
        ImGui::SameLine();
        imgui.text(_u8L("Total time") + ":");
        ImGui::SameLine();
        imgui.text(short_time(get_time_dhms(time_mode.time)));
        break;
    }
    case libvgcode::EViewType::ColorPrint: {
        //BBS: replace model custom gcode with current plate custom gcode
        const std::vector<CustomGCode::Item>& custom_gcode_per_print_z = wxGetApp().is_editor() ? wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes : m_custom_gcode_per_print_z;
        size_t total_items = 1;
        // BBS: no ColorChange type, use ToolChange
        //const std::vector<uint8_t>& used_extruders_ids = m_viewer.get_used_extruders_ids();
        //for (uint8_t extruder_id : used_extruders_ids) {
        //    total_items += color_print_ranges(extruder_id, custom_gcode_per_print_z).size();
        //}

        const bool need_scrollable = static_cast<float>(total_items) * (icon_size + ImGui::GetStyle().ItemSpacing.y) > child_height;

        // add scrollable region, if needed
        if (need_scrollable)
            ImGui::BeginChild("color_prints", { -1.0f, child_height }, false);

        // shows only extruders actually used
        size_t i = 0;
        const std::vector<uint8_t>& used_extruders_ids = m_viewer.get_used_extruders_ids();
        auto tool_colors = m_viewer.get_tool_colors();
        for (auto extruder_idx : used_extruders_ids) {
            if (i < model_used_filaments_m.size() && i < model_used_filaments_g.size()) {
                std::vector<std::pair<std::string, float>> columns_offsets;
                columns_offsets.push_back({ std::to_string(extruder_idx + 1), color_print_offsets[_u8L("Filament")]});

                char buf[64];
                float column_sum_m = 0.0f;
                float column_sum_g = 0.0f;
                if (displayed_columns & ColumnData::Model) {
                    if ((displayed_columns & ~ColumnData::Model) > 0)
                        ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", model_used_filaments_m[i], model_used_filaments_g[i] / unit_conver);
                    else
                        ::sprintf(buf, imperial_units ? "%.2f in    %.2f oz" : "%.2f m    %.2f g", model_used_filaments_m[i], model_used_filaments_g[i] / unit_conver);
                    columns_offsets.push_back({ buf, color_print_offsets[_u8L("Model")] });
                    column_sum_m += model_used_filaments_m[i];
                    column_sum_g += model_used_filaments_g[i];
                }
                if (displayed_columns & ColumnData::Support) {
                    ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", support_used_filaments_m[i], support_used_filaments_g[i] / unit_conver);
                    columns_offsets.push_back({ buf, color_print_offsets[_u8L("Support")] });
                    column_sum_m += support_used_filaments_m[i];
                    column_sum_g += support_used_filaments_g[i];
                }
                if (displayed_columns & ColumnData::Flushed) {
                    ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", flushed_filaments_m[i], flushed_filaments_g[i] / unit_conver);
                    columns_offsets.push_back({ buf, color_print_offsets[_u8L("Flushed")]});
                    column_sum_m += flushed_filaments_m[i];
                    column_sum_g += flushed_filaments_g[i];
                }
                if (displayed_columns & ColumnData::WipeTower) {
                    ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", wipe_tower_used_filaments_m[i], wipe_tower_used_filaments_g[i] / unit_conver);
                    columns_offsets.push_back({ buf, color_print_offsets[_u8L("Tower")] });
                    column_sum_m += wipe_tower_used_filaments_m[i];
                    column_sum_g += wipe_tower_used_filaments_g[i];
                }
                if ((displayed_columns & ~ColumnData::Model) > 0) {
                    ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", column_sum_m, column_sum_g / unit_conver);
                    columns_offsets.push_back({ buf, color_print_offsets[_u8L("Total")] });
                }

                float checkbox_pos = std::max(predictable_icon_pos, color_print_offsets[_u8L("Display")]); // ORCA prefer predictable_icon_pos when header not reacing end
                append_item(EItemType::Rect, libvgcode::convert(tool_colors[extruder_idx]), columns_offsets, false, checkbox_pos/*ORCA*/, true, [this, extruder_idx]() {});
            }
            i++;
        }

        if (need_scrollable)
            ImGui::EndChild();

        // Sum of all rows
        char buf[64];
        if (used_extruders_ids.size() > 1) {
            // Separator
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            const ImRect separator(ImVec2(window->Pos.x + window_padding * 3, window->DC.CursorPos.y), ImVec2(window->Pos.x + window->Size.x - window_padding * 3, window->DC.CursorPos.y + 1.0f));
            ImGui::ItemSize(ImVec2(0.0f, 0.0f));
            const bool item_visible = ImGui::ItemAdd(separator, 0);
            window->DrawList->AddLine(separator.Min, ImVec2(separator.Max.x, separator.Min.y), ImGui::GetColorU32(ImGuiCol_Separator));

            std::vector<std::pair<std::string, float>> columns_offsets;
            columns_offsets.push_back({ _u8L("Total"), color_print_offsets[_u8L("Filament")]});
            if (displayed_columns & ColumnData::Model) {
                if ((displayed_columns & ~ColumnData::Model) > 0)
                    ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_model_used_filament_m, total_model_used_filament_g / unit_conver);
                else
                    ::sprintf(buf, imperial_units ? "%.2f in    %.2f oz" : "%.2f m    %.2f g", total_model_used_filament_m, total_model_used_filament_g / unit_conver);
                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Model")] });
            }
            if (displayed_columns & ColumnData::Support) {
                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_support_used_filament_m, total_support_used_filament_g / unit_conver);
                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Support")] });
            }
            if (displayed_columns & ColumnData::Flushed) {
                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_flushed_filament_m, total_flushed_filament_g / unit_conver);
                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Flushed")] });
            }
            if (displayed_columns & ColumnData::WipeTower) {
                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_wipe_tower_used_filament_m, total_wipe_tower_used_filament_g / unit_conver);
                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Tower")] });
            }
            if ((displayed_columns & ~ColumnData::Model) > 0) {
                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_model_used_filament_m + total_support_used_filament_m + total_flushed_filament_m + total_wipe_tower_used_filament_m,
                    (total_model_used_filament_g + total_support_used_filament_g + total_flushed_filament_g + total_wipe_tower_used_filament_g) / unit_conver);
                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Total")] });
            }
            append_item(EItemType::None, libvgcode::convert(tool_colors[0]), columns_offsets);
        }

        //BBS display filament change times
        ImGui::Dummy({window_padding, window_padding});
        ImGui::SameLine();
        imgui.text(_u8L("Filament change times") + ":");
        ImGui::SameLine();
        ::sprintf(buf, "%d", m_print_statistics.total_filament_changes);
        imgui.text(buf);

        //BBS display cost
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.text(_u8L("Cost")+":");
        ImGui::SameLine();
        ::sprintf(buf, "%.2f", ps.total_cost);
        imgui.text(buf);

        break;
    }
    default: { break; }
    }

    // partial estimated printing time section
    if (new_view_type == libvgcode::EViewType::ColorPrint) {
        using Times = std::pair<float, float>;
        using TimesList = std::vector<std::pair<CustomGCode::Type, Times>>;

        // helper structure containig the data needed to render the time items
        struct PartialTime
        {
            enum class EType : unsigned char
            {
                Print,
                ColorChange,
                Pause
            };
            EType type;
            int extruder_id;
            ColorRGBA color1;
            ColorRGBA color2;
            Times times;
            std::pair<double, double> used_filament {0.0f, 0.0f};
        };
        using PartialTimes = std::vector<PartialTime>;

        auto generate_partial_times = [this, get_used_filament_from_volume](const TimesList& times, const std::vector<double>& used_filaments) {
            PartialTimes items;

            //BBS: replace model custom gcode with current plate custom gcode
            std::vector<CustomGCode::Item> custom_gcode_per_print_z = wxGetApp().is_editor() ? wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes : m_custom_gcode_per_print_z;
            const size_t extruders_count = get_extruders_count();
            std::vector<ColorRGBA> last_color(extruders_count);
            for (size_t i = 0; i < extruders_count; ++i) {
                last_color[i] = libvgcode::convert(m_viewer.get_tool_colors()[i]);
            }
            int last_extruder_id = 1;
            int color_change_idx = 0;
            for (const auto& time_rec : times) {
                switch (time_rec.first)
                {
                case CustomGCode::PausePrint: {
                    auto it = std::find_if(custom_gcode_per_print_z.begin(), custom_gcode_per_print_z.end(), [time_rec](const CustomGCode::Item& item) { return item.type == time_rec.first; });
                    if (it != custom_gcode_per_print_z.end()) {
                        items.push_back({ PartialTime::EType::Print, it->extruder, last_color[it->extruder - 1], ColorRGBA::BLACK(), time_rec.second });
                        items.push_back({ PartialTime::EType::Pause, it->extruder, ColorRGBA::BLACK(), ColorRGBA::BLACK(), time_rec.second });
                        custom_gcode_per_print_z.erase(it);
                    }
                    break;
                }
                case CustomGCode::ColorChange: {
                    auto it = std::find_if(custom_gcode_per_print_z.begin(), custom_gcode_per_print_z.end(), [time_rec](const CustomGCode::Item& item) { return item.type == time_rec.first; });
                    if (it != custom_gcode_per_print_z.end()) {
                        items.push_back({ PartialTime::EType::Print, it->extruder, last_color[it->extruder - 1], ColorRGBA::BLACK(), time_rec.second, get_used_filament_from_volume(used_filaments[color_change_idx++], it->extruder - 1) });
                        ColorRGBA color;
                        decode_color(it->color, color);
                        items.push_back({ PartialTime::EType::ColorChange, it->extruder, last_color[it->extruder - 1], color, time_rec.second });
                        last_color[it->extruder - 1] = color;
                        last_extruder_id = it->extruder;
                        custom_gcode_per_print_z.erase(it);
                    }
                    else
                        items.push_back({ PartialTime::EType::Print, last_extruder_id, last_color[last_extruder_id - 1], ColorRGBA::BLACK(), time_rec.second, get_used_filament_from_volume(used_filaments[color_change_idx++], last_extruder_id - 1) });

                    break;
                }
                default: { break; }
                }
            }

            return items;
        };

        auto append_color_change = [&imgui](const ColorRGBA& color1, const ColorRGBA& color2, const std::array<float, 4>& offsets, const Times& times) {
            imgui.text(_u8L("Color change"));
            ImGui::SameLine();

            float icon_size = ImGui::GetTextLineHeight();
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 pos = ImGui::GetCursorScreenPos();
            pos.x -= 0.5f * ImGui::GetStyle().ItemSpacing.x;

            draw_list->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { pos.x + icon_size - 1.0f, pos.y + icon_size - 1.0f },
                ImGuiWrapper::to_ImU32(color1));
            pos.x += icon_size;
            draw_list->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { pos.x + icon_size - 1.0f, pos.y + icon_size - 1.0f },
                ImGuiWrapper::to_ImU32(color2));

            ImGui::SameLine(offsets[0]);
            imgui.text(short_time(get_time_dhms(times.second - times.first)));
        };

        auto append_print = [&imgui, imperial_units](const ColorRGBA& color, const std::array<float, 4>& offsets, const Times& times, std::pair<double, double> used_filament) {
            imgui.text(_u8L("Print"));
            ImGui::SameLine();

            float icon_size = ImGui::GetTextLineHeight();
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 pos = ImGui::GetCursorScreenPos();
            pos.x -= 0.5f * ImGui::GetStyle().ItemSpacing.x;

            draw_list->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { pos.x + icon_size - 1.0f, pos.y + icon_size - 1.0f },
                ImGuiWrapper::to_ImU32(color));

            ImGui::SameLine(offsets[0]);
            imgui.text(short_time(get_time_dhms(times.second)));
            ImGui::SameLine(offsets[1]);
            imgui.text(short_time(get_time_dhms(times.first)));
            if (used_filament.first > 0.0f) {
                char buffer[64];
                ImGui::SameLine(offsets[2]);
                ::sprintf(buffer, imperial_units ? "%.2f in" : "%.2f m", used_filament.first);
                imgui.text(buffer);

                ImGui::SameLine(offsets[3]);
                ::sprintf(buffer, "%.2f g", used_filament.second);
                imgui.text(buffer);
            }
        };

        PartialTimes partial_times = generate_partial_times(time_mode.custom_gcode_times, m_print_statistics.volumes_per_color_change);
        if (!partial_times.empty()) {
            labels.clear();
            times.clear();

            for (const PartialTime& item : partial_times) {
                switch (item.type)
                {
                case PartialTime::EType::Print:       { labels.push_back(_u8L("Print")); break; }
                case PartialTime::EType::Pause:       { labels.push_back(_u8L("Pause")); break; }
                case PartialTime::EType::ColorChange: { labels.push_back(_u8L("Color change")); break; }
                }
                times.push_back(short_time(get_time_dhms(item.times.second)));
            }

            std::string longest_used_filament_string;
            for (const PartialTime& item : partial_times) {
                if (item.used_filament.first > 0.0f) {
                    char buffer[64];
                    ::sprintf(buffer, imperial_units ? "%.2f in" : "%.2f m", item.used_filament.first);
                    if (::strlen(buffer) > longest_used_filament_string.length())
                        longest_used_filament_string = buffer;
                }
            }

            //offsets = calculate_offsets(labels, times, { _u8L("Event"), _u8L("Remaining time"), _u8L("Duration"), longest_used_filament_string }, 2.0f * icon_size);

            //ImGui::Spacing();
            //append_headers({ _u8L("Event"), _u8L("Remaining time"), _u8L("Duration"), _u8L("Used filament") }, offsets);
            //const bool need_scrollable = static_cast<float>(partial_times.size()) * (icon_size + ImGui::GetStyle().ItemSpacing.y) > child_height;
            //if (need_scrollable)
            //    // add scrollable region
            //    ImGui::BeginChild("events", { -1.0f, child_height }, false);

            //for (const PartialTime& item : partial_times) {
            //    switch (item.type)
            //    {
            //    case PartialTime::EType::Print: {
            //        append_print(item.color1, offsets, item.times, item.used_filament);
            //        break;
            //    }
            //    case PartialTime::EType::Pause: {
            //        imgui.text(_u8L("Pause"));
            //        ImGui::SameLine(offsets[0]);
            //        imgui.text(short_time(get_time_dhms(item.times.second - item.times.first)));
            //        break;
            //    }
            //    case PartialTime::EType::ColorChange: {
            //        append_color_change(item.color1, item.color2, offsets, item.times);
            //        break;
            //    }
            //    }
            //}

            //if (need_scrollable)
            //    ImGui::EndChild();
        }
    }

    // travel paths section
    if (m_viewer.is_option_visible(libvgcode::EOptionType::Travels)) {
        switch (m_viewer.get_view_type())
        {
        case libvgcode::EViewType::Speed:
        case libvgcode::EViewType::ActualSpeed:
        case libvgcode::EViewType::Tool:
        case libvgcode::EViewType::ColorPrint: {
            break;
        }
        default: {
            // BBS GUI:refactor
            // title
            //ImGui::Spacing();
            //imgui.title(_u8L("Travel"));
            //// items
            //append_item(EItemType::Line, Travel_Colors[0], _u8L("Movement"));
            //append_item(EItemType::Line, Travel_Colors[1], _u8L("Extrusion"));
            //append_item(EItemType::Line, Travel_Colors[2], _u8L("Retraction"));

            break;
        }
        }
    }

    // wipe paths section
    //if (m_buffers[buffer_id(EMoveType::Wipe)].visible) {
    //    switch (m_view_type)
    //    {
    //    case EViewType::Feedrate:
    //    case EViewType::Tool:
    //    case EViewType::ColorPrint: { break; }
    //    default: {
    //        // title
    //        ImGui::Spacing();
    //        ImGui::Dummy({ window_padding, window_padding });
    //        ImGui::SameLine();
    //        imgui.title(_u8L("Wipe"));

    //        // items
    //        append_item(EItemType::Line, Wipe_Color, { {_u8L("Wipe"), 0} });

    //        break;
    //    }
    //    }
    //}

    //auto add_option = [this, append_item](EMoveType move_type, EOptionsColors color, const std::string& text) {
    //    const TBuffer& buffer = m_buffers[buffer_id(move_type)];
    //    if (buffer.visible && buffer.has_data())
    //        append_item(EItemType::Circle, Options_Colors[static_cast<unsigned int>(color)], text);
    //};

    /* BBS GUI refactor */
    // options section
    //if (any_option_available()) {
    //    // title
    //    ImGui::Spacing();
    //    imgui.title(_u8L("Options"));

    //    // items
    //    add_option(EMoveType::Retract, EOptionsColors::Retractions, _u8L("Retractions"));
    //    add_option(EMoveType::Unretract, EOptionsColors::Unretractions, _u8L("Deretractions"));
    //    add_option(EMoveType::Seam, EOptionsColors::Seams, _u8L("Seams"));
    //    add_option(EMoveType::Tool_change, EOptionsColors::ToolChanges, _u8L("Tool changes"));
    //    add_option(EMoveType::Color_change, EOptionsColors::ColorChanges, _u8L("Color changes"));
    //    add_option(EMoveType::Pause_Print, EOptionsColors::PausePrints, _u8L("Print pauses"));
    //    add_option(EMoveType::Custom_GCode, EOptionsColors::CustomGCodes, _u8L("Custom G-codes"));
    //}


    // settings section
    bool has_settings = false;
    has_settings |= !m_settings_ids.print.empty();
    has_settings |= !m_settings_ids.printer.empty();
    bool has_filament_settings = true;
    has_filament_settings &= !m_settings_ids.filament.empty();
    for (const std::string& fs : m_settings_ids.filament) {
        has_filament_settings &= !fs.empty();
    }
    has_settings |= has_filament_settings;
    //BBS: add only gcode mode
    bool show_settings = m_only_gcode_in_preview; //wxGetApp().is_gcode_viewer();
    show_settings &= (new_view_type == libvgcode::EViewType::FeatureType || new_view_type == libvgcode::EViewType::Tool);
    show_settings &= has_settings;
    if (show_settings) {
        auto calc_offset = [this]() {
            float ret = 0.0f;
            if (!m_settings_ids.printer.empty())
                ret = std::max(ret, ImGui::CalcTextSize((_u8L("Printer") + std::string(":")).c_str()).x);
            if (!m_settings_ids.print.empty())
                ret = std::max(ret, ImGui::CalcTextSize((_u8L("Print settings") + std::string(":")).c_str()).x);
            if (!m_settings_ids.filament.empty()) {
                for (unsigned char i : m_viewer.get_used_extruders_ids()) {
                    ret = std::max(ret, ImGui::CalcTextSize((_u8L("Filament") + " " + std::to_string(i + 1) + ":").c_str()).x);
                }
            }
            if (ret > 0.0f)
                ret += 2.0f * ImGui::GetStyle().ItemSpacing.x;
            return ret;
        };

        ImGui::Spacing();
        imgui.title(_u8L("Settings"));

        float offset = calc_offset();

        if (!m_settings_ids.printer.empty()) {
            imgui.text(_u8L("Printer") + ":");
            ImGui::SameLine(offset);
            imgui.text(m_settings_ids.printer);
        }
        if (!m_settings_ids.print.empty()) {
            imgui.text(_u8L("Print settings") + ":");
            ImGui::SameLine(offset);
            imgui.text(m_settings_ids.print);
        }
        if (!m_settings_ids.filament.empty()) {
            for (unsigned char i : m_viewer.get_used_extruders_ids()) {
                if (i < static_cast<unsigned char>(m_settings_ids.filament.size()) && !m_settings_ids.filament[i].empty()) {
                    std::string txt = _u8L("Filament");
                    txt += (m_viewer.get_used_extruders_count() == 1) ? ":" : " " + std::to_string(i + 1);
                    imgui.text(txt);
                    ImGui::SameLine(offset);
                    imgui.text(m_settings_ids.filament[i]);
                }
            }
        }
    }
    // Custom G-code overview
    std::vector<CustomGCode::Item> custom_gcode_per_print_z = wxGetApp().is_editor() ?
                                                                  wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes :
                                                                  m_custom_gcode_per_print_z;
    if (custom_gcode_per_print_z.size() != 0) {
        float max_len = window_padding + 2 * ImGui::GetStyle().ItemSpacing.x;
        ImGui::Spacing();
        // Title Line
        std::string cgcode_title_str       = _u8L("Custom G-code");
        std::string cgcode_layer_str       = _u8L("Layer");
        std::string cgcode_time_str        =  _u8L("Time");
        // Types of custom gcode
        std::string cgcode_pause_str = _u8L("Pause");
        std::string cgcode_template_str= _u8L("Template");
        std::string cgcode_toolchange_str = _u8L("Tool Change");
        std::string cgcode_custom_str = _u8L("Custom");
        std::string cgcode_unknown_str = _u8L("Unknown");

        // Get longest String
        max_len += std::max(ImGui::CalcTextSize(cgcode_title_str.c_str()).x,
                                              std::max(ImGui::CalcTextSize(cgcode_pause_str.c_str()).x,
                                                       std::max(ImGui::CalcTextSize(cgcode_template_str.c_str()).x,
                                                                std::max(ImGui::CalcTextSize(cgcode_toolchange_str.c_str()).x,
                                                                         std::max(ImGui::CalcTextSize(cgcode_custom_str.c_str()).x,
                                                                                  ImGui::CalcTextSize(cgcode_unknown_str.c_str()).x))))

        );

        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
        ImGui::Dummy({window_padding, window_padding});
        ImGui::SameLine();
        imgui.title(cgcode_title_str,true);
        ImGui::SameLine(max_len);
        imgui.title(cgcode_layer_str, true);
        ImGui::SameLine(max_len*1.5);
        imgui.title(cgcode_time_str, false);

        for (Slic3r::CustomGCode::Item custom_gcode : custom_gcode_per_print_z) {
            ImGui::Dummy({window_padding, window_padding});
            ImGui::SameLine();

            switch (custom_gcode.type) {
            case PausePrint: imgui.text(cgcode_pause_str); break;
            case Template: imgui.text(cgcode_template_str); break;
            case ToolChange: imgui.text(cgcode_toolchange_str); break;
            case Custom: imgui.text(cgcode_custom_str); break;
            default: imgui.text(cgcode_unknown_str); break;
            }
            ImGui::SameLine(max_len);
            char buf[64];
            int layer = m_viewer.get_layer_id_at(custom_gcode.print_z);
            ::sprintf(buf, "%d",layer );
            imgui.text(buf);
            ImGui::SameLine(max_len * 1.5);

            std::vector<float> layer_times = m_viewer.get_layers_estimated_times();
            float custom_gcode_time = 0;
            if (layer > 0)
            {
                for (int i = 0; i < layer-1; i++) {
                    custom_gcode_time += layer_times[i];
                }
            }
            imgui.text(short_time(get_time_dhms(custom_gcode_time)));

        }
    }


    // total estimated printing time section
    ImGui::Spacing();
    std::string time_title = m_viewer.get_view_type() == libvgcode::EViewType::FeatureType ? _u8L("Total Estimation") : _u8L("Time Estimation");
    auto can_show_mode_button = [this](libvgcode::ETimeMode mode) {
        std::vector<std::string> time_strs;
        for (size_t i = 0; i < m_print_statistics.modes.size(); ++i) {
            if (m_print_statistics.modes[i].time > 0.0f) {
                const std::string time_str = short_time(get_time_dhms(m_print_statistics.modes[i].time));
                const auto it = std::find(time_strs.begin(), time_strs.end(), time_str);
                if (it == time_strs.end())
                    time_strs.emplace_back(time_str);
            }
        }
        return time_strs.size() > 1;
    };

    const libvgcode::ETimeMode time_mode_id = m_viewer.get_time_mode();
    if (can_show_mode_button(time_mode_id)) {
        switch (time_mode_id)
        {
        case libvgcode::ETimeMode::Normal: { time_title += " [" + _u8L("Normal mode") + "]"; break; }
        default: { assert(false); break; }
        }
    }
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
    ImGui::Dummy({ window_padding, window_padding });
    ImGui::SameLine();
    imgui.title(time_title);
    std::string total_filament_str = _u8L("Total Filament");
    std::string model_filament_str = _u8L("Model Filament");
    std::string cost_str = _u8L("Cost");
    std::string prepare_str = _u8L("Prepare time");
    std::string print_str = _u8L("Model printing time");
    std::string total_str = _u8L("Total time");
    float max_len = window_padding + 2 * ImGui::GetStyle().ItemSpacing.x;
    if (m_viewer.get_layers_estimated_times().empty())
        max_len += ImGui::CalcTextSize(total_str.c_str()).x;
    else {
        if (m_viewer.get_view_type() == libvgcode::EViewType::FeatureType)
            max_len += std::max(ImGui::CalcTextSize(cost_str.c_str()).x,
                std::max(ImGui::CalcTextSize(print_str.c_str()).x,
                    std::max(std::max(ImGui::CalcTextSize(prepare_str.c_str()).x, ImGui::CalcTextSize(total_str.c_str()).x),
                        std::max(ImGui::CalcTextSize(total_filament_str.c_str()).x, ImGui::CalcTextSize(model_filament_str.c_str()).x))));
        else
            max_len += std::max(ImGui::CalcTextSize(print_str.c_str()).x,
                (std::max(ImGui::CalcTextSize(prepare_str.c_str()).x, ImGui::CalcTextSize(total_str.c_str()).x)));
    }
    if (m_viewer.get_view_type() == libvgcode::EViewType::FeatureType) {
        //BBS display filament cost
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.text(total_filament_str + ":");
        ImGui::SameLine(max_len);
        //BBS: use current plater's print statistics
        bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
        char buf[64];
        ::sprintf(buf, imperial_units ? "%.2f in" : "%.2f m", ps.total_used_filament / koef);
        imgui.text(buf);
        ImGui::SameLine();
        ::sprintf(buf, imperial_units ? "  %.2f oz" : "  %.2f g", ps.total_weight / unit_conver);
        imgui.text(buf);
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.text(model_filament_str + ":");
        ImGui::SameLine(max_len);
        auto exlude_m = total_support_used_filament_m + total_flushed_filament_m + total_wipe_tower_used_filament_m;
        auto exlude_g = total_support_used_filament_g + total_flushed_filament_g + total_wipe_tower_used_filament_g;
        ::sprintf(buf, imperial_units ? "%.2f in" : "%.2f m", ps.total_used_filament / koef - exlude_m);
        imgui.text(buf);
        ImGui::SameLine();
        ::sprintf(buf, imperial_units ? "  %.2f oz" : "  %.2f g", (ps.total_weight - exlude_g) / unit_conver);
        imgui.text(buf);
        //BBS: display cost of filaments
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.text(cost_str + ":");
        ImGui::SameLine(max_len);
        ::sprintf(buf, "%.2f", ps.total_cost);
        imgui.text(buf);
    }
    //BBS: start gcode is mostly same with prepeare time
    if (time_mode.prepare_time != 0.0f) {
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        imgui.text(prepare_str + ":");
        ImGui::SameLine(max_len);
        imgui.text(short_time(get_time_dhms(time_mode.prepare_time)));
    }
    ImGui::Dummy({ window_padding, window_padding });
    ImGui::SameLine();
    imgui.text(print_str + ":");
    ImGui::SameLine(max_len);
    imgui.text(short_time(get_time_dhms(time_mode.time - time_mode.prepare_time)));
    ImGui::Dummy({ window_padding, window_padding });
    ImGui::SameLine();
    imgui.text(total_str + ":");
    ImGui::SameLine(max_len);
    imgui.text(short_time(get_time_dhms(time_mode.time)));

    auto show_mode_button = [this, &imgui, can_show_mode_button](const std::string& label, libvgcode::ETimeMode mode) {
        if (can_show_mode_button(mode)) {
            if (imgui.button(label)) {
                m_viewer.set_time_mode(mode);
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
                imgui.set_requires_extra_frame();
#else
                wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
                wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
            }
        }
    };

    switch (time_mode_id) {
    case libvgcode::ETimeMode::Normal: {
        show_mode_button(_u8L("Show stealth mode"), libvgcode::ETimeMode::Stealth);
        break;
    }
    case libvgcode::ETimeMode::Stealth: {
        show_mode_button(_u8L("Show normal mode"), libvgcode::ETimeMode::Normal);
        break;
    }
    default : { assert(false); break; }
    }

    if (m_viewer.get_view_type() == libvgcode::EViewType::ColorPrint) {
        ImGui::Spacing();
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        offsets = calculate_offsets({ { _u8L("Options"), { ""}}, { _u8L("Display"), {""}} }, icon_size);
        offsets[1] = std::max(predictable_icon_pos, color_print_offsets[_u8L("Display")]); // ORCA prefer predictable_icon_pos when header not reacing end
        append_headers({ {_u8L("Options"), offsets[0] }, { _u8L("Display"), offsets[1]} });
        for (auto item : m_viewer.get_options())
            append_option_item(item, offsets);
    }
    ImGui::Dummy({ window_padding, window_padding });
    if (m_nozzle_nums > 1 && (m_viewer.get_view_type() == libvgcode::EViewType::Summary || m_viewer.get_view_type() == libvgcode::EViewType::ColorPrint)) // ORCA show only on summary and filament tab
        render_legend_color_arr_recommen(window_padding);

    legend_height = ImGui::GetCurrentWindow()->Size.y;
    imgui.end();
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(2);
}

void GCodeViewer::push_combo_style()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * m_scale); // ORCA scale rounding
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * m_scale); // ORCA scale frame size
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0,8.0));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.0f, 0.0f, 0.0f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.59f, 0.53f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.00f, 0.59f, 0.53f, 1.0f));
}
void GCodeViewer::pop_combo_style()
{
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(8);
}

void GCodeViewer::render_slider(int canvas_width, int canvas_height) {
    m_moves_slider->render(canvas_width, canvas_height);
    m_layers_slider->render(canvas_width, canvas_height);
}

} // namespace GUI
} // namespace Slic3r

