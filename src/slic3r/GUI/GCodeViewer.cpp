#include "libslic3r/libslic3r.h"
#include "GCodeViewer.hpp"
#include "LegendText.hpp"
#include "PreviewPanels.hpp"

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/calib.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/IMEXHelpers.hpp"
#include "libslic3r/IMEXZones.hpp"
//BBS: add convex hull logic for toolpath check

#include "GUI_App.hpp"
#include "Shortcuts.hpp"
#include "Plater.hpp"
#include "Camera.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "GUI.hpp"
#include "GLCanvas3D.hpp"
#include "FilamentGroupPopup.hpp"
#include "GLToolbar.hpp"
#include "libslic3r/BeltTransform.hpp"
#include "MsgDialog.hpp"
#include <memory>
#include <boost/container_hash/hash.hpp>
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/PreviewOverlayLayout.hpp"
#include "slic3r/GUI/MovePanelLayout.hpp"
#include "slic3r/GUI/PreviewPanelSlots.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"
#include <initializer_list>
#include <set>
#include <string>
#include "libvgcode/include/Types.hpp"
#include <vector>
#include <utility>
#include <cstdio>
#include "libslic3r/Technologies.hpp"
#include <imgui.h>
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoUtils.hpp"
#include "libslic3r/Point.hpp"
#include <math.h>
#include "libvgcode/include/Viewer.hpp"
#include "libvgcode/include/PathVertex.hpp"
#include <cstddef>
#include <cstring>
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"
#include <cassert>
#include <cstdint>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/constants.hpp>
#include "slic3r/GUI/IMSlider.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include <exception>
#include <iterator>
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libvgcode/include/GCodeInputData.hpp"
#include "libvgcode/include/ColorRange.hpp"
#include <cfloat>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include "libslic3r_version.h"
#include <wx/busycursor.h>
#include "libslic3r/Slicing.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "libslic3r/Color.hpp"
#include <map>
#include <wx/event.h>
#include <wx/string.h>
#include <wx/slider.h>
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/Event.hpp"
#include <string_view>
#include <functional>
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/ExtrusionEntity.hpp"

#define IMGUI_DEFINE_MATH_OPERATORS

#include <imgui/imgui_internal.h>

#include <glad/gl.h>
#include <boost/functional/hash.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <wx/progdlg.h>
#include <wx/numformatter.h>
#include <wx/utils.h>

#include <array>
#include <algorithm>
#include <limits>
#include <sstream>
#include <cmath>
#include <chrono>
#include <Eigen/Geometry>
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/ObjectID.hpp"
#include "slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp"
#include "slic3r/GUI/PartPlate.hpp"

using namespace std::string_view_literals;

namespace Slic3r { class PrintBase; }


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
    if (view_type == libvgcode::EViewType::FeatureType)
        return _u8L("Line Type");
    else if (view_type == libvgcode::EViewType::Height)
        return _u8L("Layer Height");
    else if (view_type == libvgcode::EViewType::Width)
        return _u8L("Line Width");
    else if (view_type == libvgcode::EViewType::Speed)
        return _u8L("Speed");
    else if (view_type == libvgcode::EViewType::ActualSpeed)
        return _u8L("Actual Speed");
    else if (view_type == libvgcode::EViewType::Acceleration)
        return _u8L("Acceleration");
    else if (view_type == libvgcode::EViewType::Jerk)
        return _u8L("Jerk");
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
// ORCA: Add Pressure Advance visualization support
    else if (view_type == libvgcode::EViewType::PressureAdvance)
        return _u8L("Pressure Advance");
    return "";
}

// ORCA: Stable, locale independent names used to persist a view type in the application config.
// Keep these in sync with the entries of libvgcode::EViewType exposed in the preview combo box.
static const std::vector<std::pair<std::string, libvgcode::EViewType>>& view_type_config_map()
{
    static const std::vector<std::pair<std::string, libvgcode::EViewType>> map = {
        { "summary",                      libvgcode::EViewType::Summary },
        { "feature_type",                 libvgcode::EViewType::FeatureType },
        { "color_print",                  libvgcode::EViewType::ColorPrint },
        { "speed",                        libvgcode::EViewType::Speed },
        { "actual_speed",                 libvgcode::EViewType::ActualSpeed },
        { "acceleration",                 libvgcode::EViewType::Acceleration },
        { "jerk",                         libvgcode::EViewType::Jerk },
        { "height",                       libvgcode::EViewType::Height },
        { "width",                        libvgcode::EViewType::Width },
        { "volumetric_flow_rate",         libvgcode::EViewType::VolumetricFlowRate },
        { "actual_volumetric_flow_rate",  libvgcode::EViewType::ActualVolumetricFlowRate },
        { "layer_time_linear",            libvgcode::EViewType::LayerTimeLinear },
        { "layer_time_logarithmic",       libvgcode::EViewType::LayerTimeLogarithmic },
        { "fan_speed",                    libvgcode::EViewType::FanSpeed },
        { "temperature",                  libvgcode::EViewType::Temperature },
        { "pressure_advance",             libvgcode::EViewType::PressureAdvance },
    };
    return map;
}

// ORCA: the view type showing the value a calibration varies, when the preview has one.
static std::optional<libvgcode::EViewType> calibration_view_type(CalibMode mode)
{
    switch (mode) {
    case CalibMode::Calib_PA_Line:
    case CalibMode::Calib_PA_Pattern:
    case CalibMode::Calib_PA_Tower:        return libvgcode::EViewType::PressureAdvance;
    case CalibMode::Calib_Temp_Tower:      return libvgcode::EViewType::Temperature;
    case CalibMode::Calib_Flow_Rate:
    case CalibMode::Calib_Vol_speed_Tower: return libvgcode::EViewType::VolumetricFlowRate;
    case CalibMode::Calib_VFA_Tower:       return libvgcode::EViewType::Speed;
    case CalibMode::Calib_Cornering:       return libvgcode::EViewType::Jerk;
    default:                               return std::nullopt;
    }
}

#if ENABLE_ACTUAL_SPEED_DEBUG
void GCodeViewer::SequentialView::ActualSpeedImguiWidget::plot(const ImVec2& size, float scale) const
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return;

    const ImRect bb(window->DC.CursorPos, window->DC.CursorPos + size);
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, 0) || data.size() < 2 || data.back().pos <= data.front().pos)
        return;

    ImDrawList* draw_list = window->DrawList;
    const float text_h    = ImGui::GetTextLineHeight();
    const float gap       = 4.f * scale;
    const float gutter    = ImGui::CalcTextSize("999.9").x + gap;
    const ImRect plot_bb(bb.Min + ImVec2(gutter, text_h), bb.Max - ImVec2(gutter, text_h));
    if (plot_bb.GetHeight() <= 0.f || plot_bb.GetWidth() <= 0.f)
        return;

    float max_speed = 0.f;
    for (const Item& item : data)
        max_speed = std::max(max_speed, item.speed);
    const float inv_y = max_speed > 0.f ? 1.f / max_speed : 0.f;
    const float x0    = data.front().pos;
    const float inv_x = 1.f / (data.back().pos - x0);
    auto point = [&](size_t i) {
        return ImVec2(ImLerp(plot_bb.Min.x, plot_bb.Max.x, ImSaturate((data[i].pos - x0) * inv_x)),
                      ImLerp(plot_bb.Max.y, plot_bb.Min.y, ImSaturate(data[i].speed * inv_y)));
    };

    const ImU32 grid = ImGui::GetColorU32(ImVec4(0.5f, 0.5f, 0.5f, 0.6f));
    draw_list->AddLine(ImVec2(plot_bb.Min.x, plot_bb.Max.y), plot_bb.Max, grid, scale);

    const ImU32 line_color = ImGui::GetColorU32(ImGuiCol_Text);
    for (size_t i = 0; i + 1 < data.size(); ++i)
        draw_list->AddLine(point(i), point(i + 1), line_color, 1.5f * scale);

    std::vector<float> speeds;
    speeds.reserve(data.size());
    for (const Item& item : data)
        speeds.push_back(item.speed);
    const SpeedKeyPoints keys = speed_profile_key_points(speeds);

    const ImU32 muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    char        buf[32];

    auto side_label = [&](size_t idx, bool left) {
        sprintf(buf, "%.1f", data[idx].speed);
        const float w = ImGui::CalcTextSize(buf).x;
        const float y = ImClamp(point(idx).y - 0.5f * text_h, bb.Min.y, plot_bb.Max.y - 0.5f * text_h);
        draw_list->AddText(ImVec2(left ? plot_bb.Min.x - gap - w : plot_bb.Max.x + gap, y), line_color, buf);
    };
    side_label(keys.first, true);
    side_label(keys.last, false);
    if (keys.plateau_begin != keys.first && keys.plateau_end != keys.last) {
        sprintf(buf, "%.1f", data[keys.plateau_begin].speed);
        const float w = ImGui::CalcTextSize(buf).x;
        const float x = 0.5f * (point(keys.plateau_begin).x + point(keys.plateau_end).x) - 0.5f * w;
        draw_list->AddText(ImVec2(ImClamp(x, plot_bb.Min.x, plot_bb.Max.x - w), point(keys.plateau_begin).y - text_h - scale), line_color, buf);
    }

    auto pos_text = [&](size_t idx) { return format("%.3f", data[idx].pos - x0); };
    const std::string length_text = pos_text(keys.last) + " " + _u8L("mm");
    const float       length_left = plot_bb.Max.x - ImGui::CalcTextSize(length_text.c_str()).x;
    draw_list->AddText(ImVec2(plot_bb.Min.x, plot_bb.Max.y), muted, "0");
    draw_list->AddText(ImVec2(length_left, plot_bb.Max.y), muted, length_text.c_str());

    float pos_right = plot_bb.Min.x + ImGui::CalcTextSize("0").x + gap;
    auto  pos_label = [&](size_t idx) {
        const std::string label = pos_text(idx);
        const float       w     = ImGui::CalcTextSize(label.c_str()).x;
        const float       px    = point(idx).x;
        const float       x     = std::min(std::max(px - 0.5f * w, pos_right), length_left - gap - w);
        if (px < x || px > x + w)
            return;
        draw_list->AddText(ImVec2(x, plot_bb.Max.y), muted, label.c_str());
        draw_list->AddLine(ImVec2(px, plot_bb.Max.y), point(idx), grid, scale);
        pos_right = x + w + gap;
    };
    if (keys.plateau_begin != keys.first)
        pos_label(keys.plateau_begin);
    if (keys.plateau_end != keys.plateau_begin && keys.plateau_end != keys.last)
        pos_label(keys.plateau_end);
}
#endif // ENABLE_ACTUAL_SPEED_DEBUG

void GCodeViewer::SequentialView::Marker::init(std::string filename)
{
    if (filename.empty()) {
        m_model.init_from(stilized_arrow(16, 1.5f, 3.0f, 0.8f, 3.0f));
    } else {
        m_model.init_from_file(filename);
    }
    m_model.set_color(default_color());
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
    // ORCA Tilt the model around the nozzle tip
    const Transform3d tilt(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), m_nozzle_axis));
    const Transform3d model_matrix = Geometry::translation_transform(m_world_position.cast<double>()) * tilt *
        Geometry::translation_transform((m_model_z_offset + scale_factor * m_model.get_bounding_box().size().z()) * Vec3d::UnitZ()) *
        Geometry::rotation_transform({ M_PI, 0.0, 0.0 }) * Geometry::scale_transform(scale_factor);
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
    case libvgcode::EMoveType::CustomGCode: { return _u8L("Custom G-code"); }
    case libvgcode::EMoveType::Travel:      { return _u8L("Travel"); }
    case libvgcode::EMoveType::Wipe:        { return _u8L("Wipe"); }
    case libvgcode::EMoveType::Extrude:     { return _u8L("Extrude"); }
    default:                                { return _u8L("Unknown"); }
    }
}

static std::string to_string(libvgcode::EGCodeExtrusionRole role)
{
    switch (role)
    {                                                              // ORCA matched terms
    case libvgcode::EGCodeExtrusionRole::None:                     { return _u8L("Unknown"); }
    case libvgcode::EGCodeExtrusionRole::Perimeter:                { return _u8L("Inner wall"); }
    case libvgcode::EGCodeExtrusionRole::ExternalPerimeter:        { return _u8L("Outer wall"); }
    case libvgcode::EGCodeExtrusionRole::OverhangPerimeter:        { return _u8L("Overhang wall"); }
    case libvgcode::EGCodeExtrusionRole::InternalInfill:           { return _u8L("Sparse infill"); }
    case libvgcode::EGCodeExtrusionRole::SolidInfill:              { return _u8L("Internal solid infill"); }
    case libvgcode::EGCodeExtrusionRole::TopSolidInfill:           { return _u8L("Top surface"); }
    case libvgcode::EGCodeExtrusionRole::Ironing:                  { return _u8L("Ironing"); }
    case libvgcode::EGCodeExtrusionRole::BridgeInfill:             { return _u8L("Bridge"); }
    case libvgcode::EGCodeExtrusionRole::GapFill:                  { return _u8L("Gap infill"); }
    case libvgcode::EGCodeExtrusionRole::Skirt:                    { return _u8L("Skirt"); }
    case libvgcode::EGCodeExtrusionRole::SupportMaterial:          { return _u8L("Support"); }
    case libvgcode::EGCodeExtrusionRole::SupportMaterialInterface: { return _u8L("Support interface"); }
    case libvgcode::EGCodeExtrusionRole::WipeTower:                { return _u8L("Prime tower"); }
    case libvgcode::EGCodeExtrusionRole::Custom:                   { return _u8L("Custom"); }
    // ORCA
    case libvgcode::EGCodeExtrusionRole::BottomSurface:            { return _u8L("Bottom surface"); }
    case libvgcode::EGCodeExtrusionRole::InternalBridgeInfill:     { return _u8L("Internal bridge"); } // ORCA
    case libvgcode::EGCodeExtrusionRole::Brim:                     { return _u8L("Brim"); }
    case libvgcode::EGCodeExtrusionRole::SupportTransition:        { return _u8L("Support transition"); }
    case libvgcode::EGCodeExtrusionRole::Mixed:                    { return _u8L("Mixed"); }
    default:                                                       { return _u8L("Unknown"); }
    }
}

static std::string color_scheme_value(const libvgcode::PathVertex& vertex, libvgcode::EViewType view_type, const std::string& na)
{
    const bool is_extrusion = vertex.is_extrusion();
    switch (view_type) {
    case libvgcode::EViewType::FeatureType:
        return is_extrusion ? to_string(vertex.role) : vertex.type != libvgcode::EMoveType::Noop ? to_string(vertex.type) : na;
    case libvgcode::EViewType::Height: return _u8L("Height: ") + (is_extrusion ? format("%.2f", vertex.height) : na);
    case libvgcode::EViewType::Width: return _u8L("Width: ") + (is_extrusion ? format("%.2f", vertex.width) : na);
    case libvgcode::EViewType::VolumetricFlowRate: return _u8L("Flow: ") + (is_extrusion ? format("%.2f", vertex.volumetric_rate()) : na);
    case libvgcode::EViewType::FanSpeed: return _u8L("Fan: ") + format("%.0f", vertex.fan_speed);
    case libvgcode::EViewType::Temperature: return _u8L("Temperature: ") + format("%.0f", vertex.temperature);
    case libvgcode::EViewType::LayerTimeLinear:
    case libvgcode::EViewType::LayerTimeLogarithmic: return _u8L("Layer Time: ") + format("%.1f", vertex.layer_duration);
    case libvgcode::EViewType::Tool: return _u8L("Tool: ") + std::to_string(vertex.extruder_id + 1);
    case libvgcode::EViewType::ColorPrint: return _u8L("Color: ") + std::to_string(vertex.color_id + 1);
    case libvgcode::EViewType::Acceleration: return _u8L("Acceleration: ") + format("%.0f", vertex.acceleration);
    case libvgcode::EViewType::Jerk: return _u8L("Jerk: ") + format("%.1f", vertex.jerk);
    case libvgcode::EViewType::PressureAdvance: return _u8L("PA: ") + format("%.4f", vertex.pressure_advance);
    default: return {};
    }
}

// ORCA: Length and duration of the move ending at the vertex, or a length of -1 for a move that
// does not travel. Arc moves (G2/G3) are discretized into several vertices sharing the same gcode
// line id, so the whole run is accumulated to report the arc instead of a single chord.
static std::pair<float, float> move_length_and_duration(const libvgcode::Viewer& viewer, const libvgcode::PathVertex& vertex, size_t vertex_id)
{
    const size_t time_mode = static_cast<size_t>(viewer.get_time_mode());
    if (vertex_id == 0 || !(vertex.is_extrusion() || vertex.is_travel() || vertex.is_wipe()))
        return {-1.f, vertex.times[time_mode]};

    const size_t vertices_count = viewer.get_vertices_count();
    size_t       first_id       = vertex_id;
    while (first_id > 0 && viewer.get_vertex_at(first_id - 1).gcode_id == vertex.gcode_id)
        --first_id;
    size_t last_id = vertex_id;
    while (last_id + 1 < vertices_count && viewer.get_vertex_at(last_id + 1).gcode_id == vertex.gcode_id)
        ++last_id;
    float length   = 0.f;
    float duration = 0.f;
    for (size_t i = std::max<size_t>(first_id, 1); i <= last_id; ++i) {
        const libvgcode::PathVertex& v = viewer.get_vertex_at(i);
        length += (libvgcode::convert(v.position) - libvgcode::convert(viewer.get_vertex_at(i - 1).position)).norm();
        duration += v.times[time_mode];
    }
    return {length, duration};
}

namespace {

struct MoveDetailRow
{
    std::string label;
    std::string value;
    std::string widest;
};

struct MoveDetailGroup
{
    std::string                title;
    std::vector<MoveDetailRow> rows;
    float                      label_w{0.f};
    float                      value_w{0.f};
    float                      width{0.f};
};

} // namespace

static std::vector<MoveDetailGroup> move_detail_groups(const libvgcode::PathVertex& vertex, float length, const std::string& na)
{
    const bool        is_extrusion = vertex.is_extrusion();
    const std::string mm           = _u8L("mm");
    const std::string mm_s         = _u8L("mm/s");
    auto value_or_na = [&na](bool defined, const std::string& value) { return defined ? value : na; };
    return {
        {_u8L_CONTEXT("LINE", "Move details"),
         {{_u8L("Type"), to_string(vertex.type), ""},
          // TRN Width × height of the extrusion line.
          {_u8L("W × H"), value_or_na(is_extrusion, format("%.3f × %.3f %s", vertex.width, vertex.height, mm)), "9.999 × 9.999 " + mm},
          {_u8L("Length"), value_or_na(length >= 0.f, format("%.3f %s", length, mm)), "9999.999 " + mm}}},
        {_u8L_CONTEXT("MOTION", "Move details"),
         {{_u8L("Speed"), format("%.1f %s", vertex.feedrate, mm_s), "9999.9 " + mm_s},
          {_u8L("Acceleration"), format("%.0f %s", vertex.acceleration, _u8L("mm/s²")), "99999 " + _u8L("mm/s²")},
          {_u8L("Jerk"), format("%.1f %s", vertex.jerk, mm_s), "999.9 " + mm_s}}},
        {_u8L_CONTEXT("EXTRUSION", "Move details"),
         {{_u8L("Flow rate"), value_or_na(is_extrusion, format("%.3f %s", vertex.volumetric_rate(), _u8L("mm³/s"))), "999.999 " + _u8L("mm³/s")},
          {_u8L("Pressure Advance"), format("%.4f", vertex.pressure_advance), "9.9999"}}},
        {_u8L_CONTEXT("THERMAL", "Move details"),
         {{_u8L("Temperature"), format("%.0f %s", vertex.temperature, _u8L("\u2103" /* °C */)), "999 " + _u8L("\u2103" /* °C */)},
          {_u8L("Fan speed"), format("%.0f %%", vertex.fan_speed), "100 %"}}},
    };
}

void GCodeViewer::SequentialView::Marker::render_position_window(const libvgcode::Viewer* viewer, const PreviewOverlayLayout& layout,
                                                                  int move_number, int move_count, const libvgcode::EViewType& view_type)
{
    static bool properties_shown = false;
    static bool profile_shown    = false;

    m_panel_rect = CanvasUi::Rect();
    if (viewer == nullptr)
        return;

    ImGuiWrapper&     imgui  = *wxGetApp().imgui();
    const float       s      = layout.scale;
    const std::string NA_TXT = _u8L("N/A");
    const std::string mm_s   = _u8L("mm/s");

    libvgcode::PathVertex vertex    = viewer->get_current_vertex();
    size_t                vertex_id = viewer->get_current_vertex_id();
    // Seam moves may contain non-deterministic vertex data, so use the
    // last visible non-seam vertex instead (except in FeatureType view)
    if (view_type != libvgcode::EViewType::FeatureType && vertex.type == libvgcode::EMoveType::Seam) {
        const libvgcode::Interval& visible_range = viewer->get_view_visible_range();
        if (visible_range[1] > 0) {
            vertex_id = static_cast<size_t>(visible_range[1]) - 1;
            vertex    = viewer->get_vertex_at(vertex_id);
        }
    }
    const std::string detail      = color_scheme_value(vertex, view_type, NA_TXT);
    const bool        show_swatch = view_type == libvgcode::EViewType::FeatureType && vertex.type != libvgcode::EMoveType::Noop;
    const std::pair<float, float> move_extent = properties_shown ? move_length_and_duration(*viewer, vertex, vertex_id) : std::pair(-1.f, 0.f);
    const float                   length      = move_extent.first;
    const float                   duration    = move_extent.second;
    std::vector<MoveDetailGroup>  groups;
    if (properties_shown)
        groups = move_detail_groups(vertex, length, NA_TXT);

    const float text_h      = ImGui::GetTextLineHeight();
    const float item_gap    = 18.f * s;
    const float label_gap   = 12.f * s;
    const float group_gap   = 28.f * s;
    const ImVec2 padding    = ImVec2(16.f, 4.f) * s;
    const float top_inset   = 6.f * s;
    const float rule_gap    = 6.f * s;
    auto bold_width = [&imgui](const std::string& text) {
        const bool  bold  = imgui.push_bold_font();
        const float width = ImGui::CalcTextSize(text.c_str()).x;
        if (bold)
            imgui.pop_bold_font();
        return width;
    };
    for (MoveDetailGroup& g : groups) {
        for (const MoveDetailRow& r : g.rows) {
            g.label_w = std::max(g.label_w, ImGui::CalcTextSize(r.label.c_str()).x);
            g.value_w = std::max({g.value_w, ImGui::CalcTextSize(r.value.c_str()).x, ImGui::CalcTextSize(r.widest.c_str()).x});
        }
        g.width = std::max(bold_width(g.title), g.label_w + label_gap + g.value_w);
    }

    static constexpr const char* MOVE_MARK = "\x1f";
    const std::string move_text    = format(_u8L("Move %1% of %2%"), MOVE_MARK, move_count);
    const size_t      move_split   = move_text.find(MOVE_MARK);
    const std::string move_prefix  = move_text.substr(0, move_split);
    const std::string move_suffix  = move_split == std::string::npos ? "" : move_text.substr(move_split + std::strlen(MOVE_MARK));
    const std::string time_label   = _u8L("Time");
    const std::string time_value   = get_time_dhms(viewer->get_estimated_time_at(vertex_id));
    const std::string speed_label  = _u8L("Speed");
    const std::string speed_value  = format("%.0f %s", vertex.feedrate, mm_s);
    const std::string profile_btn  = _u8L("Speed profile");
    const bool        show_filament  = viewer->get_used_extruders_count() > 1;
    const std::string filament_label = _u8L("Filament");
    const std::string filament_value = std::to_string(vertex.extruder_id + 1);

    const Vec3d     plate_origin = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_origin();
    libvgcode::AABox extent      = viewer->get_bounding_box();
    for (libvgcode::Vec3& corner : extent) {
        corner[0] -= static_cast<float>(plate_origin.x());
        corner[1] -= static_cast<float>(plate_origin.y());
    }
    const float extent_max = std::max({std::abs(extent[0][0]), std::abs(extent[1][0]), std::abs(extent[0][1]), std::abs(extent[1][1]),
                                       std::abs(extent[0][2]), std::abs(extent[1][2])});
    const char* precision  = m_high_precision_gcode ? (extent_max > 9999 ? "%.2f" : extent_max > 999 ? "%.3f" : "%.4f")
                                                    : (extent_max > 9999 ? "%.1f" : extent_max > 999 ? "%.2f" : "%.3f");
    const float max_speed  = viewer->get_color_range(libvgcode::EViewType::Speed).get_range()[1];

    auto text_width = [](const std::string& text) { return ImGui::CalcTextSize(text.c_str()).x; };

    const float space         = text_width(" ");
    const float chevron_w     = 16.f * s + 4.f * s;
    const float swatch_w      = show_swatch ? text_h * 0.7f + space : 0.f;
    float       detail_w      = text_width(detail);
    if (view_type == libvgcode::EViewType::FeatureType)
        for (size_t r = 0; r < libvgcode::GCODE_EXTRUSION_ROLES_COUNT; ++r)
            detail_w = std::max(detail_w, text_width(to_string(libvgcode::EGCodeExtrusionRole(r))));
    const float move_num_w    = std::max(bold_width(std::to_string(move_count)), bold_width(std::to_string(move_number)));
    const float move_w        = text_width(move_prefix) + move_num_w + text_width(move_suffix);
    const float time_value_w  = std::max(text_width(get_time_dhms(viewer->get_estimated_time())), text_width(time_value));
    const float time_w        = text_width(time_label) + space + time_value_w;
    std::array<float, 3> axis_value_w;
    for (size_t i = 0; i < 3; ++i)
        axis_value_w[i] = std::max(text_width(format(precision, extent[0][i])), text_width(format(precision, extent[1][i])));
    const float axis_label_w  = text_width("X ");
    const float position_w    = 3.f * axis_label_w + axis_value_w[0] + axis_value_w[1] + axis_value_w[2] + 4.f * space;
    const float speed_value_w = std::max(text_width(format("%.0f %s", max_speed > 0.f && max_speed < 1e6f ? max_speed : vertex.feedrate, mm_s)), text_width(speed_value));
    const float speed_w       = text_width(speed_label) + space + speed_value_w;
    float       filament_num_w = 0.f;
    for (const uint8_t id : viewer->get_used_extruders_ids())
        filament_num_w = std::max(filament_num_w, text_width(std::to_string(id + 1)));
    const float dot_d         = text_h * 0.7f;
    const float filament_w    = show_filament ? text_width(filament_label) + 2.f * space + dot_d + filament_num_w : 0.f;
    const float profile_btn_w = text_width(profile_btn) + 16.f * s;
    // TRN Title of the speed chart of a move: its length in millimeters and its duration in seconds.
    const std::string profile_title_fmt = _u8L("Actual speed profile · %1% mm · %2% s");
    const std::string profile_title     = format(profile_title_fmt, format("%.3f", std::max(length, 0.f)), format("%.3f", duration));
    const std::string no_profile    = _u8L("No speed profile for this move");
    const float profile_w     = std::max({240.f * s, text_width(format(profile_title_fmt, "9999.999", "999.999")), text_width(no_profile)});
    const float more_w        = text_width(format(_L_PLURAL("%1% more", "%1% more", 4), 4)) + 16.f * s;

    MovePanelWidths widths;
    widths.available      = layout.panel_width(FLT_MAX) - 2.f * padding.x;
    widths.fixed          = chevron_w + move_w + item_gap + speed_w + item_gap + swatch_w + detail_w + (show_filament ? item_gap + filament_w : 0.f);
    widths.time           = properties_shown ? time_w : 0.f;
    widths.position       = position_w;
    widths.spacing        = item_gap;
    widths.group_spacing  = group_gap;
    widths.profile_button = properties_shown ? profile_btn_w : 0.f;
    widths.profile_chart  = properties_shown ? profile_w : 0.f;
    widths.more_button    = more_w;
    if (properties_shown)
        for (const MoveDetailGroup& g : groups)
            widths.groups.push_back(g.width);
    const MovePanelVisibility visible             = fit_move_panel(widths);
    const bool                show_profile_button = properties_shown && visible.show_profile;
    const bool                show_chart          = show_profile_button && profile_shown;

    const bool show_time = properties_shown && visible.show_time;
    float row_w = widths.fixed + (show_time ? item_gap + time_w : 0.f) + (visible.show_position ? item_gap + position_w : 0.f);
    if (show_profile_button)
        row_w += item_gap + profile_btn_w;
    float body_w = 0.f;
    if (properties_shown) {
        for (size_t i = 0; i < visible.visible_groups; ++i)
            body_w += (i > 0 ? group_gap : 0.f) + groups[i].width;
        if (show_chart)
            body_w += group_gap + profile_w;
        if (visible.visible_groups < groups.size())
            body_w += group_gap + more_w;
    }
    const float panel_w = layout.panel_width(std::ceil(std::max(row_w, body_w) + 2.f * padding.x));
    const float panel_x = layout.panel_left(panel_w);

    imgui.set_next_window_pos(panel_x, layout.band_bottom, ImGuiCond_Always, 0.f, 1.f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(panel_w, 0.f), ImVec2(panel_w, FLT_MAX));
    ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    background.w      = 0.8f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(item_gap, 4.f * s));
    imgui.begin(std::string("ToolPosition"), ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);

    const ImVec4 text_color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const ImVec4 muted      = ImVec4(text_color.x, text_color.y, text_color.z, 0.6f);
    auto slot = [&](float width, auto&& draw) {
        const float x = ImGui::GetCursorPosX();
        draw();
        ImGui::SameLine(x + width + item_gap);
    };
    auto right_aligned = [](float slot_w, float text_w, auto&& draw) {
        ImGui::SameLine(0.f, 0.f);
        ImGui::Dummy(ImVec2(std::max(0.f, slot_w - text_w), 0.f));
        ImGui::SameLine(0.f, 0.f);
        draw();
    };
    const bool profile_available = vertex.is_extrusion() || vertex.is_travel() || vertex.is_wipe();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + top_inset);

    auto draw_group = [&](const MoveDetailGroup& g) {
        ImGui::BeginGroup();
        const bool bold = imgui.push_bold_font();
        ImGui::TextColored(muted, "%s", g.title.c_str());
        if (bold)
            imgui.pop_bold_font();
        for (const MoveDetailRow& r : g.rows) {
            ImGui::TextColored(muted, "%s", r.label.c_str());
            ImGui::SameLine(0.f, std::max(0.f, g.width - text_width(r.label) - text_width(r.value)));
            ImGuiWrapper::text(r.value);
        }
        ImGui::EndGroup();
    };

    if (properties_shown) {
        bool first = true;
        auto next  = [&] {
            if (!first)
                ImGui::SameLine(0.f, group_gap);
            first = false;
        };
        for (size_t i = 0; i < visible.visible_groups; ++i) {
            next();
            draw_group(groups[i]);
            if (i == 1 && show_chart) {
                next();
                ImGui::BeginGroup();
                const ImVec2 chart_size(profile_w, 3.f * text_h + 2.f * 4.f * s);
                if (profile_available) {
                    ImGui::TextColored(muted, "%s", profile_title.c_str());
#if ENABLE_ACTUAL_SPEED_DEBUG
                    m_actual_speed_imgui_widget.plot(chart_size, s);
#endif // ENABLE_ACTUAL_SPEED_DEBUG
                } else {
                    ImGui::TextColored(muted, "%s", profile_btn.c_str());
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(chart_size);
                    const ImVec2 text = ImGui::CalcTextSize(no_profile.c_str());
                    ImGui::GetWindowDrawList()->AddText(p + ImVec2(0.5f * (chart_size.x - text.x), 0.5f * (chart_size.y - text.y)),
                                                        ImGui::GetColorU32(muted), no_profile.c_str());
                }
                ImGui::EndGroup();
            }
        }
        const size_t hidden = groups.size() - visible.visible_groups;
        if (hidden > 0) {
            next();
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f * s);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
            ImGui::PushStyleColor(ImGuiCol_Border, muted);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + text_h);
            if (ImGui::Button(format(_L_PLURAL("%1% more", "%1% more", hidden), hidden).c_str(), ImVec2(more_w, 0.f)))
                ImGui::OpenPopup("##more_move_details");
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);

            ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetWindowPos().y - layout.gap), ImGuiCond_Always, ImVec2(0.f, 1.f));
            if (ImGui::BeginPopup("##more_move_details")) {
                for (size_t i = visible.visible_groups; i < groups.size(); ++i) {
                    if (i > visible.visible_groups)
                        ImGui::SameLine(0.f, group_gap);
                    draw_group(groups[i]);
                }
                ImGui::EndPopup();
            }
        }
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rule_gap);
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(text_color.x, text_color.y, text_color.z, 0.2f));
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rule_gap);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.f, 2.f) * s);
    ImGui::PushStyleColor(ImGuiCol_Button, properties_shown ? ImVec4(text_color.x, text_color.y, text_color.z, 0.15f) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
    const float row_y = ImGui::GetCursorPosY();
    if (imgui.glyph_button(properties_shown ? ImGui::UnfoldButtonIcon : ImGui::FoldButtonIcon, ImVec2(16.f, 16.f) * s)) {
        properties_shown = !properties_shown;
        imgui.set_requires_extra_frame();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    ImGui::SameLine(padding.x + chevron_w + 4.f * s);

    slot(move_w, [&] {
        ImGui::TextColored(muted, "%s", move_prefix.c_str());
        right_aligned(move_num_w, bold_width(std::to_string(move_number)), [&] {
            const bool bold = imgui.push_bold_font();
            ImGuiWrapper::text(std::to_string(move_number));
            if (bold)
                imgui.pop_bold_font();
        });
        ImGui::SameLine(0.f, 0.f);
        ImGui::TextColored(muted, "%s", move_suffix.c_str());
    });
    if (show_time)
        slot(time_w, [&] {
            ImGui::TextColored(muted, "%s", time_label.c_str());
            right_aligned(space + time_value_w, text_width(time_value), [&] { ImGuiWrapper::text(time_value); });
        });
    if (visible.show_position) {
        auto pos = vertex.position;
        pos[0] -= static_cast<float>(plate_origin.x());
        pos[1] -= static_cast<float>(plate_origin.y());
        slot(position_w, [&] {
            const std::array<std::pair<const char*, ColorRGBA>, 3> axes = {{{"X ", ColorRGBA::X()}, {"Y ", ColorRGBA::Y()}, {"Z ", ColorRGBA::Z()}}};
            for (size_t i = 0; i < 3; ++i) {
                if (i > 0)
                    ImGui::SameLine(0.f, 2.f * space);
                ImGui::TextColored(ImGuiWrapper::to_ImVec4(axes[i].second), "%s", axes[i].first);
                const std::string value = format(precision, pos[i]);
                right_aligned(axis_value_w[i], text_width(value), [&] { ImGuiWrapper::text(value); });
            }
        });
    }
    slot(speed_w, [&] {
        ImGui::TextColored(muted, "%s", speed_label.c_str());
        right_aligned(space + speed_value_w, text_width(speed_value), [&] { ImGuiWrapper::text(speed_value); });
    });
    if (show_filament)
        slot(filament_w, [&] {
            ImGui::TextColored(muted, "%s", filament_label.c_str());
            ImGui::SameLine(0.f, space);
            const libvgcode::PaletteWithAlpha& tools = viewer->get_tool_colors();
            ColorRGBA color = vertex.extruder_id < tools.size() ? libvgcode::convert(tools[vertex.extruder_id]) : ColorRGBA::GRAY();
            color.a(1.f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(p + ImVec2(0.5f * dot_d, 0.5f * text_h), 0.5f * dot_d, ImGuiWrapper::to_ImU32(color), 16);
            ImGui::Dummy(ImVec2(dot_d, text_h));
            right_aligned(space + filament_num_w, text_width(filament_value), [&] { ImGuiWrapper::text(filament_value); });
        });
    if (show_swatch) {
        const ImVec2 p  = ImGui::GetCursorScreenPos();
        const float  sw = text_h * 0.7f;
        const ColorRGBA c = libvgcode::convert(viewer->get_vertex_color(vertex));
        ImGui::GetWindowDrawList()->AddRectFilled(p + ImVec2(0.f, 0.5f * (text_h - sw)), p + ImVec2(sw, 0.5f * (text_h + sw)),
                                                  ImGuiWrapper::to_ImU32(c), 2.f * s);
        ImGui::Dummy(ImVec2(sw, text_h));
        ImGui::SameLine(0.f, space);
    }
    ImGuiWrapper::text(detail);

    if (show_profile_button) {
        const float btn_x = ImGui::GetWindowContentRegionMax().x - profile_btn_w;
        ImGui::SameLine(std::max(btn_x, ImGui::GetCursorPosX()));
        ImGui::SetCursorPosY(row_y);
        const bool active = profile_shown;
        if (active)
            imgui.push_confirm_button_style();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, active ? 0.f : 1.f);
        ImGui::PushStyleColor(ImGuiCol_Border, muted);
        if (!active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
        }
        if (imgui.button(from_u8(profile_btn), ImVec2(profile_btn_w, 0.f), true)) {
            profile_shown = !profile_shown;
            imgui.set_requires_extra_frame();
        }
        if (!active)
            ImGui::PopStyleColor(2);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (active)
            imgui.pop_confirm_button_style();
    }

    const ImVec2 wnd_pos  = ImGui::GetWindowPos();
    const ImVec2 wnd_size = ImGui::GetWindowSize();
    m_panel_rect          = {wnd_pos.x, wnd_pos.y, wnd_pos.x + wnd_size.x, wnd_pos.y + wnd_size.y};
    ImGui::GetBackgroundDrawList()->AddRectFilled(wnd_pos, ImVec2(wnd_pos.x + wnd_size.x, layout.band_bottom), ImGui::GetColorU32(background),
                                                  10.f * s, ImDrawFlags_RoundCornersTop);
    const float fillet = 10.f * s;
    const float right  = wnd_pos.x + wnd_size.x;
    if (layout.bottom_band.left < wnd_pos.x - fillet)
        draw_concave_corner(ImGui::GetBackgroundDrawList(), ImVec2(wnd_pos.x, layout.band_bottom), -1.f, -1.f, fillet, ImGui::GetColorU32(background));
    if (layout.bottom_band.right > right + fillet)
        draw_concave_corner(ImGui::GetBackgroundDrawList(), ImVec2(right, layout.band_bottom), 1.f, -1.f, fillet, ImGui::GetColorU32(background));
    imgui.end();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
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

// Byte offset just past the first count characters of str, or its length if it is shorter.
static size_t utf8_offset(const std::string& str, size_t count)
{
    const char* const begin = str.c_str();
    const char* const end   = begin + str.size();
    const char*       pos   = begin;
    for (size_t i = 0; i < count && pos < end; ++i) {
        unsigned int codepoint = 0;
        pos += ImTextCharFromUtf8(&codepoint, pos, end);
    }
    return pos - begin;
}

//BBS: GUI refactor: move to right
void GCodeViewer::SequentialView::GCodeWindow::render(float top, float bottom, float right, uint64_t curr_line_id) const
{
    // Orca: truncate long lines(>55 characters), add "..." at the end
    auto update_lines = [this](uint64_t start_id, uint64_t end_id) {
        std::vector<Line> ret;
        ret.reserve(end_id - start_id + 1);
        // Orca: m_lines_ends indexes into a memory mapping, so it must be clamped to the mapping. If the
        // file was modified behind our back (an in-place post-processing script that shrank it), an
        // unchecked read is an access violation, which the caller's try/catch cannot catch on Windows.
        const size_t file_size = m_file.size();
        for (uint64_t id = start_id; id <= end_id; ++id) {
            // read line from file
            // Keep one entry per id: render() indexes m_lines by (id - start_id).
            const size_t start = id == 1 ? 0 : std::min(m_lines_ends[id - 2], file_size);
            const size_t end   = std::min(m_lines_ends[id - 1], file_size);
            const size_t original_len = end > start ? end - start : 0;
            // A character is four bytes at most, so 55 of them always fit in 220.
            const size_t len          = std::min(original_len, (size_t) 55 * 4);
            std::string  gline(m_file.data() + start, len);

            // If original line is longer than 55 characters, truncate and append "...".
            // The cut must land on a character boundary or it leaves half a character behind.
            if (len < original_len || utf8_offset(gline, 55) < gline.size())
                gline = gline.substr(0, utf8_offset(gline, 52)) + "...";

            std::string command, parameters, comment;
            const size_t comment_start = gline.find(';');
            if (comment_start == std::string::npos)
                command = gline;
            else {
                command = gline.substr(0, comment_start);
                comment = gline.substr(comment_start);
            }

            // extract gcode command and parameters
            if (!command.empty()) {
                std::vector<std::string> tokens;
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
void GCodeViewer::SequentialView::render_marker(const bool has_render_path, int canvas_width, int canvas_height, const libvgcode::EViewType& view_type)
{
    if (has_render_path && m_show_marker) {
        // marker.set_world_offset(current_offset);
        marker.render(canvas_width, canvas_height, view_type);
        // IDEX/IQEX secondary carriage markers
        for (auto& sec : m_imex_secondary_markers)
            sec.render(canvas_width, canvas_height, view_type);
    }
}

void GCodeViewer::SequentialView::render_overlay(const bool has_render_path, const PreviewPanelPlacement& gcode_place, const libvgcode::Viewer* viewer, uint32_t gcode_id, const PreviewOverlayLayout& layout, int move_number, int move_count, const libvgcode::EViewType& view_type)
{
    marker.render_position_window(has_render_path ? viewer : nullptr, layout, move_number, move_count, view_type);

    if (has_render_path)
        gcode_window.render(gcode_place.top, std::max(gcode_place.top + 10.f, gcode_place.bottom), gcode_place.x, gcode_id);
}

GCodeViewer::GCodeViewer() : m_panels(std::make_unique<PreviewPanels>(*this))
{
    m_moves_slider  = new IMSlider(0, 0, 0, 100, wxSL_HORIZONTAL);
    m_layers_slider = new IMSlider(0, 0, 0, 100, wxSL_VERTICAL);
}

GCodeViewer::~GCodeViewer()
{
    reset();
    release_caster_depth();
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

    m_panels->bind();

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

    m_marker_filename = filename;
    m_sequential_view.marker.init(filename);

    // initializes point sizes
    std::array<int, 2> point_sizes;
    ::glGetIntegerv(GL_ALIASED_POINT_SIZE_RANGE, point_sizes.data());
    m_detected_point_sizes = { static_cast<float>(point_sizes[0]), static_cast<float>(point_sizes[1]) };

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
    apply_default_view_type();

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

// ORCA: Preview default view type preference, see "preview_default_view_type" in the application config.
std::string GCodeViewer::view_type_to_config_name(libvgcode::EViewType type)
{
    for (const auto& [name, value] : view_type_config_map()) {
        if (value == type)
            return name;
    }
    return std::string();
}

bool GCodeViewer::view_type_from_config_name(const std::string& name, libvgcode::EViewType& type)
{
    for (const auto& [config_name, value] : view_type_config_map()) {
        if (config_name == name) {
            type = legend_opened_view_type(value);
            return true;
        }
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> GCodeViewer::default_view_type_choices()
{
    std::vector<std::pair<std::string, std::string>> choices = {
        { "auto", _u8L("Automatic") },
        { "last", _u8L("Last used") },
    };
    // A saved summary reads as Filament, so it stands for that choice until another one is picked.
    const bool saved_summary = wxGetApp().app_config->get("preview_default_view_type") == "summary";
    for (const auto& [name, type] : view_type_config_map())
        if (type == libvgcode::EViewType::ColorPrint && saved_summary)
            choices.push_back({ "summary", get_view_type_string(type) });
        else if (type != libvgcode::EViewType::Summary)
            choices.push_back({ name, get_view_type_string(type) });
    return choices;
}

void GCodeViewer::set_view_type(libvgcode::EViewType type)
{
    if (type != m_viewer.get_view_type())
        m_panels->drop_range_filter();
    m_viewer.set_view_type(type);
}

void GCodeViewer::select_view_type(libvgcode::EViewType type) { set_view_type(type); }

// ORCA: Pick the view type the preview opens with, following the "preview_default_view_type" preference:
// a fixed view type, the one the user picked last ("last"), or the automatic choice ("auto", the default)
// which shows Filament for multi material prints and Line Type for single material ones.
// A calibration's view, when given, takes precedence over the preference.
// The default is only (re)applied when it actually changes, so a view type picked by hand survives a reslice.
void GCodeViewer::apply_default_view_type(std::optional<libvgcode::EViewType> calibration_view)
{
    const std::string preference = wxGetApp().app_config->get("preview_default_view_type");

    std::string key = preference;
    libvgcode::EViewType type = libvgcode::EViewType::FeatureType;
    if (calibration_view) {
        type = *calibration_view;
        key  = "calibration_" + view_type_to_config_name(type);
    }
    else if (preference == "last") {
        if (!view_type_from_config_name(wxGetApp().app_config->get("preview_last_view_type"), type))
            type = libvgcode::EViewType::FeatureType;
    }
    else if (!view_type_from_config_name(preference, type)) {
        // "auto", or an unknown value written by a newer version
        const bool multi_material = m_viewer.get_used_extruders_count() > 1;
        type = multi_material ? libvgcode::EViewType::ColorPrint : libvgcode::EViewType::FeatureType;
        key = multi_material ? "auto_multi_material" : "auto_single_material";
    }

    if (m_applied_default_view_type_key == key)
        return;

    m_applied_default_view_type_key = key;
    select_view_type(type);
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

    // Belt printers: drive the designed/raw view UI (legend checkbox, hotkey B, canvas-toolbar
    // menu item) from the G-code itself. Only BeltGCode writes the belt header, so its tilt
    // (gcode_result.belt_tilt_angle, abs of the slicing rotation) says whether this is belt
    // G-code; the selected printer does not, for a file opened from disk. The back-transform
    // below still reads print.config(), which Plater::load_gcode() fills from the file's own
    // config block (GCodeProcessor::export_config_for_render).
    m_belt_view_enabled = gcode_result.belt_tilt_angle > 0.f;
    m_belt_angle_deg    = gcode_result.belt_tilt_angle;

    // The tool position readout shows coordinates with the precision the G-code was exported with.
    // For a file opened from disk, print.config() holds the value from its config block as well.
    m_sequential_view.marker.set_high_precision_gcode(print.config().high_precision_gcode.value);

    const bool current_top_layer_only = m_viewer.is_top_layer_only_view_range();
    const bool required_top_layer_only = get_app_config()->get_bool("seq_top_layer_only");
    if (current_top_layer_only != required_top_layer_only)
        m_viewer.toggle_top_layer_only_view_range();

    read_reduced_detail_preferences();

    // ORCA: darken the layers the preview layer slider is not scrubbed to
    m_viewer.set_dim_previous_layers(get_app_config()->get_bool("preview_dim_previous_layers"));
    m_viewer.set_dim_previous_layers_brightness(0.01f * std::stoi(get_app_config()->get("preview_dim_previous_layers_brightness")));
    m_viewer.set_translucent_filaments(get_app_config()->get_bool("preview_translucent_filaments"));

    // avoid processing if called with the same gcode_result.
    // On a belt printer the toolpath geometry fed to libvgcode also depends on the
    // designed/raw view state (the back-transform is applied in convert), so the
    // same result is converted again only when that view has been toggled.
    const bool same_belt_view = !m_belt_view_enabled || m_last_belt_show_designed == m_belt_show_designed;
    if (m_last_result_id == gcode_result.id && wxGetApp().is_editor() && same_belt_view) {
        //BBS: add logs
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": the same id %1%, return directly, result %2% ") % m_last_result_id % (&gcode_result);

        m_viewer.set_tool_colors(libvgcode::convert_filament_colors(str_tool_colors));
        m_viewer.set_color_print_colors(libvgcode::convert_filament_colors(str_color_print_colors.empty() ? str_tool_colors : str_color_print_colors));
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
    m_plate_mass    = gcode_result.plate_mass;
    m_object_masses = gcode_result.object_masses;
    m_body_masses   = gcode_result.body_masses;
    m_support_masses = gcode_result.support_masses;

    // convert data from PrusaSlicer format to libvgcode format.
    // Belt printers: when the designed (upright) view is active, back-transform
    // the toolpath geometry into model/Cartesian space using the general belt
    // inverse (handles the mesh rotation, shear and axis remap). When off, the
    // raw machine-frame G-code is shown (useful for checking the transform itself).
    const bool is_belt = m_belt_view_enabled && print.config().belt_printer.value;
    Transform3d belt_inv = (is_belt && m_belt_show_designed)
        ? BeltTransformPipeline::build_gcode_back_transform(print.config()) : Transform3d::Identity();
    // Belt: move positions are stored as gcode_Z + belt_z_origin (the start G-code's
    // purge-blob advance baked into the machine-Z origin by its G92 Z0 resets). Subtract
    // that constant before the linear back-transform so every toolpath maps to the model's
    // belt coordinate. Without it the back-transform mixes the offset with the gantry-Y
    // term, leaving a per-move designed-Y error that min-corner anchoring cannot remove
    // when a bridge/keel move happens to cancel it at the bbox minimum.
    if (is_belt && m_belt_show_designed && gcode_result.belt_z_origin != 0.0f)
        belt_inv = belt_inv * Transform3d(Eigen::Translation3d(Vec3d(0.0, 0.0, -double(gcode_result.belt_z_origin))));
    const bool apply_belt = is_belt && m_belt_show_designed
        && !belt_inv.matrix().isApprox(Transform3d::Identity().matrix());
    if (apply_belt) {
        // The linear belt back-transform recovers the print's shape and orientation but not
        // the per-object placement/lift translation: the object's position on the belt, the
        // BeltSliceStrategy min-Z lift, and the centering pre-translate are applied OUTSIDE
        // build_forward_transform() (see PrintObjectSlice.cpp), so its linear inverse leaves
        // them un-undone. Uncorrected, the upright toolpaths float a fixed offset from the
        // model shell. Recover the translation generally — independent of the offset's exact
        // source or the axis remap — by anchoring the back-transformed object body onto the
        // upright model bounding box (the same space the shells render in).
        BoundingBoxf3 model_bb;
        for (const PrintObject* po : print.objects())
            for (const ModelInstance* mi : po->model_object()->instances)
                model_bb.merge(po->model_object()->instance_bounding_box(*mi));
        // Build the anchor bbox from surface toolpaths only. After the belt_z_origin
        // correction the surface back-transforms onto the model, but a few elevated
        // features (bridges/overhangs over the chevron gap) are mis-mapped by the linear
        // inverse to well outside the model body; if one becomes the bbox minimum it
        // drags the min-corner anchor by ~20mm. Drop moves that land clearly outside the
        // (correct) model bbox — a geometric filter, not a role guess.
        // Tolerance around the model bbox for outlier rejection, and the minimum fraction
        // of MOVES (by count) the clip must retain before we trust it over the full bbox.
        // Count, not bbox extent: a single far-flung outlier move (e.g. a belt-entry purge
        // that maps to y~0) inflates the full bbox so much that a size-fraction test would
        // wrongly reject the clip and re-include the outlier, dragging the min-corner anchor
        // by ~the offset. By count the clip keeps ~100% of moves here and ~0% only when the
        // object is genuinely offset from the belt entry (the real fall-back-to-full case).
        constexpr double ANCHOR_CLIP_MARGIN_MM   = 10.0;
        constexpr double ANCHOR_CLIP_MIN_KEEP    = 0.5;
        BoundingBoxf3 tp_bb_clip, tp_bb_full;
        size_t n_filtered = 0, n_clip = 0;
        const double y_lo = model_bb.defined ? model_bb.min.y() - ANCHOR_CLIP_MARGIN_MM : -1e30;
        const double y_hi = model_bb.defined ? model_bb.max.y() + ANCHOR_CLIP_MARGIN_MM :  1e30;
        // The anchor aligns the toolpath body onto the object-only model bbox, so it must be
        // built from OBJECT-body toolpaths only. Support, skirt, brim and wipe-tower extrusions
        // are not part of the model mesh and extend past it (support especially reaches well
        // beyond the object on a belt), so including them drags tp_bb.min and shifts the whole
        // back-transformed g-code off the mesh — but only when those features are present. That
        // is the "g-code shifts vs mesh as soon as supports are enabled" bug. Filter them out.
        auto is_object_body = [](ExtrusionRole r) {
            return r != erSupportMaterial && r != erSupportMaterialInterface
                && r != erSupportTransition
                && r != erSkirt && r != erBrim && r != erWipeTower;
        };
        auto accumulate = [&](bool body_only) {
            tp_bb_clip = BoundingBoxf3();
            tp_bb_full = BoundingBoxf3();
            n_filtered = 0;
            n_clip     = 0;
            for (const GCodeProcessorResult::MoveVertex& mv : gcode_result.moves)
                if (mv.type == EMoveType::Extrude && mv.layer_id >= 1 // skip layer-0 prime/skirt
                    && (!body_only || is_object_body(mv.extrusion_role))) {
                    ++n_filtered;
                    const Vec3d p = belt_inv * mv.position.cast<double>();
                    tp_bb_full.merge(p);
                    if (p.y() >= y_lo && p.y() <= y_hi) {
                        tp_bb_clip.merge(p);
                        ++n_clip;
                    }
                }
        };
        accumulate(/*body_only=*/true);
        // Fallback: a plate with no object-body extrusions above layer 0 (e.g. a
        // support-only object) would leave tp_bb undefined and silently skip the
        // anchor. Re-accumulate over ALL extrude moves so the min-corner anchor is
        // still computed rather than letting the preview float by the placement offset.
        if (n_filtered == 0)
            accumulate(/*body_only=*/false);
        // Use the clipped bbox when it still holds the bulk of the moves (outliers removed).
        // If the object sits far from the belt entry the toolpaths are grossly offset and the
        // clip drops most of them — fall back to the full bbox so the min-corner anchor still
        // recovers that gross translation rather than breaking.
        const BoundingBoxf3& tp_bb =
            (tp_bb_clip.defined && n_filtered > 0 &&
             double(n_clip) >= ANCHOR_CLIP_MIN_KEEP * double(n_filtered)) ? tp_bb_clip : tp_bb_full;
        if (model_bb.defined && tp_bb.defined) {
            // Anchor the back-transformed toolpath body onto the upright model bbox by
            // its MIN corner. (Center anchoring was tried and regressed when the toolpath
            // and model bounding boxes differ in extent.) With the belt_z_origin
            // correction above and the outlier-robust bbox below, the surface overlaps the
            // shell to well under a millimetre when the object is at the belt entry; an
            // object placed elsewhere in global-rotation mode still carries the placement
            // translation, which this min-corner step recovers.
            const Vec3d d = model_bb.min - tp_bb.min;
            belt_inv = Transform3d(Eigen::Translation3d(d)) * belt_inv;
        }
        // The masses into the toolpaths' frame.
        m_plate_mass.transform(belt_inv);
        for (std::vector<GCodeProcessorResult::ObjectMass>* masses : { &m_object_masses, &m_body_masses, &m_support_masses })
            for (GCodeProcessorResult::ObjectMass& mass : *masses)
                mass.transform(belt_inv);
    }
    libvgcode::GCodeInputData data = libvgcode::convert(gcode_result, str_tool_colors, str_color_print_colors, m_viewer,
        apply_belt ? &belt_inv : nullptr);
    // ORCA Tilt the marker to the layer normal of the designed belt view
    Vec3d nozzle_axis = Vec3d::UnitZ();
    if (apply_belt)
        nozzle_axis = BeltTransformPipeline::build_rotation_matrix(print.config()).row(2).transpose();
    m_sequential_view.marker.set_nozzle_axis(nozzle_axis);

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

    // Belt printers: libvgcode labels a layer with the height of its toolpaths, which
    // on a tilted layer is wherever its last extrusion happened to end, and the layer
    // slider looks its labels and the colour-change ticks up in that list assuming it
    // increases.  Give it the layers' print Z instead (the slicer's layer Z, which
    // increases along the belt), numbered the way libvgcode::convert() numbers the
    // layers: consecutively over the moves that exist.
    m_belt_layer_zs.clear();
    if (is_belt) {
        unsigned int src_layer_id = std::numeric_limits<unsigned int>::max();
        for (size_t i = 1; i < gcode_result.moves.size(); ++ i) {
            const GCodeProcessorResult::MoveVertex &mv = gcode_result.moves[i];
            if (mv.layer_id != src_layer_id) {
                src_layer_id = mv.layer_id;
                m_belt_layer_zs.emplace_back(double(mv.print_z));
            }
        }
        if (m_belt_layer_zs.size() != m_viewer.get_layers_count())
            m_belt_layer_zs.clear();
    }

    // The solid model is cut at the heights of the layer range's ends, which bound the range only
    // if the layers never drop: printed object by object, the heights start over with each object.
    // Two layers may share a height, as layers of objects with different layer heights can.
    m_layers_z_rising = true;
    float prev_z = 0.0f;
    for (size_t i = 0; i < m_viewer.get_layers_count() && m_layers_z_rising; ++i) {
        const float z = m_viewer.get_layer_z(i);
        // a layer with no extrusion has no height
        if (z == 0.0f)
            continue;
        m_layers_z_rising = z >= prev_z;
        prev_z = z;
    }

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
    m_max_bounding_box = m_paths_bounding_box;

    if (wxGetApp().is_editor()) {
        if (is_belt) {
            // The moves are machine-frame coordinates (Z is belt travel), so the per-move
            // test inside all_paths_inside() can never pass on a belt. Judge the
            // back-transformed box instead, with room for the designed view's min-corner
            // anchor, which is only accurate to a fraction of a millimetre.
            BoundingBoxf3 bed = wxGetApp().plater()->build_volume().bounding_volume();
            bed.offset(1.);
            m_contained_in_bed = !m_paths_bounding_box.defined || (bed.contains(m_paths_bounding_box.min) && bed.contains(m_paths_bounding_box.max));
        } else
            m_contained_in_bed = wxGetApp().plater()->build_volume().all_paths_inside(gcode_result, m_paths_bounding_box);
    }

    m_extruders_count = gcode_result.filaments_count;

    //BBS: move the id to the end of reset
    m_last_result_id = gcode_result.id;
    m_panels->drop_path_focus();
    m_last_belt_show_designed = m_belt_show_designed;
    m_gcode_result = &gcode_result;
    m_move_type_counts.fill(0);
    for (auto& move_type_times : m_move_type_times)
        move_type_times.fill(0.0f);
    m_move_type_distances.fill(0.0f);
    for (const GCodeProcessorResult::MoveVertex& move : gcode_result.moves) {
        if (move.internal_only)
            continue;

        const size_t move_type = static_cast<size_t>(move.type);
        if (move_type < m_move_type_counts.size()) {
            ++m_move_type_counts[move_type];
            for (size_t mode = 0; mode < move.time.size(); ++mode)
                m_move_type_times[move_type][mode] += move.time[mode];
            if (move.type == EMoveType::Retract || move.type == EMoveType::Unretract)
                m_move_type_distances[move_type] += std::fabs(move.delta_extruder);
            else
                m_move_type_distances[move_type] += move.travel_dist;
        }
    }
    m_only_gcode_in_preview = only_gcode;

    m_sequential_view.gcode_window.load_gcode(gcode_result.filename, gcode_result.lines_ends);

    //BBS: add only gcode mode
    //if (wxGetApp().is_gcode_viewer())
    if (m_only_gcode_in_preview)
        m_custom_gcode_per_print_z = gcode_result.custom_gcode_per_print_z;

    m_max_print_height = gcode_result.printable_height;
    m_machine_frame_transform_active = gcode_result.machine_frame_transform_active;
    m_z_offset = gcode_result.z_offset;


    // load_toolpaths(gcode_result, build_volume, exclude_bounding_box);
    
    // ORCA: Apply the default view type now that we know how many tools the G-code actually uses.
    const bool flowrate_calib = print.calib_mode() == CalibMode::Calib_None && wxGetApp().plater()->is_flowrate_calib();
    apply_default_view_type(calibration_view_type(flowrate_calib ? CalibMode::Calib_Flow_Rate : print.calib_mode()));

    // BBS: data for rendering color arrangement recommendation
    m_nozzle_nums = print.config().option<ConfigOptionFloats>("nozzle_diameter")->values.size();
    // the shell drag mode hides the infill only where the profile covers it: with no top or bottom
    // shell, or no walls, the infill is the surface. Per-object settings are not consulted.
    const PrintRegionConfig& region = print.default_region_config();
    m_viewer.set_reduced_detail_hide_infill(region.top_shell_layers.value > 0 && region.bottom_shell_layers.value > 0 && region.wall_loops.value > 0);
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
    // When the file's own settings were loaded into the presets, the bed already is the file's, with the
    // model and texture of a user or custom printer too, which the lookup below cannot resolve.
    if (m_only_gcode_in_preview && !wxGetApp().plater()->gcode_settings_loaded()) {
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
    m_plate_mass = {};
    m_object_masses.clear();
    m_body_masses.clear();
    m_support_masses.clear();

    m_move_type_counts.fill(0);
    for (auto& move_type_times : m_move_type_times)
        move_type_times.fill(0.0f);
    m_move_type_distances.fill(0.0f);
    const size_t normal_time_mode_idx = static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal);
    for (size_t i = 0; i < data.vertices.size(); ++i) {
        const libvgcode::PathVertex& vertex = data.vertices[i];
        const size_t move_type = static_cast<size_t>(vertex.type);
        if (move_type < m_move_type_counts.size()) {
            ++m_move_type_counts[move_type];
            for (size_t mode = 0; mode < vertex.times.size(); ++mode)
                m_move_type_times[move_type][mode] += vertex.times[mode];
            if (vertex.type == libvgcode::EMoveType::Retract || vertex.type == libvgcode::EMoveType::Unretract) {
                m_move_type_distances[move_type] += std::fabs(vertex.feedrate) * vertex.times[normal_time_mode_idx];
            } else if (i > 0) {
                const float dx = vertex.position[0] - data.vertices[i - 1].position[0];
                const float dy = vertex.position[1] - data.vertices[i - 1].position[1];
                const float dz = vertex.position[2] - data.vertices[i - 1].position[2];
                m_move_type_distances[move_type] += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
        }
    }

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
    m_shells.with_wipe_tower = false;
    m_shell_bounding_box = BoundingBoxf3();
}

void GCodeViewer::reset()
{
    //BBS: should also reset the result id
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": current result id %1% ")%m_last_result_id;
    m_last_result_id = -1;
    m_panels->drop_path_focus();
    m_belt_layer_zs.clear();
    //BBS: add only gcode mode
    m_only_gcode_in_preview = false;

    m_viewer.reset();

    m_paths_bounding_box = BoundingBoxf3();
    m_max_bounding_box = BoundingBoxf3();
    m_max_print_height = 0.0f;
    m_machine_frame_transform_active = false;
    m_z_offset = 0.0f;
    m_extruders_count = 0;
    m_filament_diameters = std::vector<float>();
    m_filament_densities = std::vector<float>();
    m_move_type_counts.fill(0);
    for (auto& move_type_times : m_move_type_times)
        move_type_times.fill(0.0f);
    m_move_type_distances.fill(0.0f);
    m_print_statistics.reset();
    m_custom_gcode_per_print_z = std::vector<CustomGCode::Item>();
    m_plate_mass = {};
    m_object_masses.clear();
    m_body_masses.clear();
    m_support_masses.clear();
    m_left_extruder_filament.clear();
    m_right_extruder_filament.clear();
    m_sequential_view.gcode_window.reset();
    m_contained_in_bed = true;
}

// IDEX/IQEX: resolve where every secondary carriage marker sits, relative to the primary,
// for one (printer preset, active mode, plate bed) triple. This parses the mode string
// several times over and rebuilds the zone grid, and every one of its inputs is preset or
// plate state, so render_scene() runs it only when GCodeViewer::ImexMarkerKey changes and replays
// the returned plan on all the frames in between.
//
// `mode` is already resolved (the plate's mode beats the process preset). An empty
// `carriages` list means "no secondary markers for this configuration" — IMEX off in this
// mode, firmware-managed zones, or a mode whose roster has no zone-owning secondary.
GCodeViewer::ImexMarkerPlan GCodeViewer::resolve_imex_marker_plan(const DynamicPrintConfig& printer_cfg,
                                                                  const std::string&        mode,
                                                                  const BoundingBoxf&       bed_extents)
{
    ImexMarkerPlan plan;

    // Firmware-managed-zones centers the slice at bed origin, so prim_pos
    // and the preview toolpaths sit in a shifted frame relative to the
    // plate-local bed bounds the zone math uses. Computing secondaries here
    // would place them off-bed. The firmware physically fans the centered
    // toolpath out into the zones, so the centered preview with no
    // secondaries is the honest representation.
    if (imex_cfg_bool(printer_cfg, "imex_firmware_managed_zones"))
        return plan;
    if (mode.empty() || mode == kImexPrimaryMode)
        return plan;

    // Only the gantry grouping and the mirror axis are decided here; the zone grid itself comes
    // from compute_imex_zone_layout() below, which reads the same keys through the same accessor.
    const int tools_per_gantry = std::max(1, imex_cfg_int(printer_cfg, "imex_tools_per_gantry"));
    plan.box_wx = (float)imex_cfg_float(printer_cfg, "imex_nozzle_clearance_x");
    plan.box_wy = (float)imex_cfg_float(printer_cfg, "imex_nozzle_clearance_y");

    // Parse "idx:P/C/M" via shared helpers — matches PartPlate::calc_imex_zones.
    // The role travels as an ImexRole all the way to the marker placement
    // below; there is no int encoding in between, so a role added to the enum
    // cannot silently fall into the "not Mirror, therefore Copy" branch.
    int pri_tool = -1;
    std::vector<int> sec_tool_ids;
    std::map<int, ImexRole> sec_tool_roles; // tool_id -> Copy or Mirror
    std::set<int>     sec_aggregated;  // representatives standing in for a whole gantry
    {
        // Same lookup rule as PartPlate's zones and ghosts, and as the slicer:
        // an unresolved mode, and a mode the tools array is too short to cover,
        // both come back empty and leave the marker roster empty.
        const std::string entry = find_imex_mode(printer_cfg, mode).active_tools;
        pri_tool = imex_primary_tool_for_mode(entry);
        // Aggregate per gantry via the same source of truth as PartPlate's
        // zone/ghost aggregation: in Span modes only one tool prints per
        // zone at a time, so a non-primary gantry collapses to one
        // representative marker. Non-Span gantries keep per-tool markers.
        const ImexGantryGrouping grouping =
            group_imex_active_tools_by_gantry(entry, tools_per_gantry);
        auto add_tool = [&](int phys_idx, ImexRole role, bool aggregated) {
            if (phys_idx < 0 || phys_idx == pri_tool) return;
            // Only Copy and Mirror print in a zone of their own, so only they
            // get a marker. Exhaustive with no default so -Wswitch makes a new
            // role answer "does this carriage get its own marker?" rather than
            // defaulting into a Copy-shaped one.
            switch (role) {
            case ImexRole::Copy:
            case ImexRole::Mirror:
                break;
            case ImexRole::Primary:  // not a secondary carriage
            case ImexRole::Span:     // rides in the primary's zone
                return;
            }
            sec_tool_roles[phys_idx] = role;
            sec_tool_ids.push_back(phys_idx);
            if (aggregated) sec_aggregated.insert(phys_idx);
        };
        // Out-of-grid tool indices (a stale mode string carried over from a
        // printer with more tools) are handled inconsistently by the code we
        // must agree with: compute_imex_zone_layout drops them, so they own no
        // zone, while calc_imex_ghosts still bakes a ghost for them. No single
        // policy matches both, so the marker roster is left exactly as it was
        // and such a tool simply falls back to the first zone below. Clamping
        // pri_tool instead is a trap: the -1 sentinel truncates to gantry 0 in
        // imex_mirror_axis_for and flips the marker to the opposite mirror
        // axis from the ghost. It is also why the layout's own `primary_head`
        // cannot stand in for `pri_tool` here — the layout reports -1 for an
        // off-grid primary, this roster has to keep the authored index.
        for (const auto& grp : grouping.groups) {
            // Markers: an aggregated non-primary gantry shows only its rep.
            if (grp.aggregate && grp.gantry_index != grouping.primary_gantry)
                add_tool(grp.representative_phys, grp.representative_role, true);
            else
                for (const auto& [phys_idx, role] : grp.tools)
                    add_tool(phys_idx, role, false);
        }
    }
    const int sec_count = (int)sec_tool_ids.size();
    if (sec_count == 0)
        return plan;

    // The zone grid is not re-derived here: this is the same call
    // PartPlate::calc_imex_zones() makes, with the same printer config and the
    // same bed extents, so the preview and the plate cannot place a tool
    // differently for the same mode. The T0-corner flips, the active
    // column/row sets and the zone pitch all live inside the library; the
    // markers need nothing from it but `head_zone_centers`.
    // `mode` is already resolved (per-plate mode beats the process preset),
    // so it goes in as the plate mode with no process fallback.
    const ImexZoneLayout zone_layout =
        compute_imex_zone_layout(printer_cfg, mode, std::string(), bed_extents);

    // Centre of zone (0,0), the fallback for any head the layout gave no zone —
    // a mode string naming a tool outside this printer's grid. Zone indices are
    // dense, so the lowest centre on each axis IS zone 0's. An empty layout
    // means one undivided zone, whose centre is the bed's.
    Vec2d zone0_center = bed_extents.center();
    if (!zone_layout.head_zone_centers.empty()) {
        zone0_center = zone_layout.head_zone_centers.begin()->second;
        for (const auto& head_center : zone_layout.head_zone_centers) {
            zone0_center.x() = std::min(zone0_center.x(), head_center.second.x());
            zone0_center.y() = std::min(zone0_center.y(), head_center.second.y());
        }
    }
    auto zone_center_of = [&](int tid) -> Vec2d {
        auto it = zone_layout.head_zone_centers.find(tid);
        return (it == zone_layout.head_zone_centers.end()) ? zone0_center : it->second;
    };
    const Vec2d pri_center = zone_center_of(pri_tool);

    // An aggregated gantry's zone is a full-X row strip with no column of its
    // own, so its marker tracks primary's X. calc_imex_ghosts() reaches the same
    // zero X offset by putting both frames on the bed centreline; for a single
    // marker, pinning the centre says it directly.
    auto sec_center_of = [&](int tid) -> Vec2d {
        Vec2d c = zone_center_of(tid);
        if (sec_aggregated.count(tid))
            c.x() = pri_center.x();
        return c;
    };

    // Primary carriage box: seeded from whether primary holds the leftmost zone
    // column, then turned by any secondary sitting to one side of it.
    plan.pri_box_offset_x = (pri_center.x() <= zone0_center.x()) ? 0.0f : -plan.box_wx;
    plan.pri_box_offset_y = -plan.box_wy;
    for (int i = 0; i < sec_count; ++i) {
        const Vec2d sc = sec_center_of(sec_tool_ids[i]);
        if      (sc.x() > pri_center.x()) { plan.pri_box_offset_x = 0.0f;         }
        else if (sc.x() < pri_center.x()) { plan.pri_box_offset_x = -plan.box_wx; }
        if      (sc.y() > pri_center.y()) { plan.pri_box_offset_y = 0.0f;         }
        else if (sc.y() < pri_center.y()) { plan.pri_box_offset_y = -plan.box_wy; }
    }

    plan.pri_head = pri_tool;
    plan.carriages.reserve(sec_count);
    for (int i = 0; i < sec_count; ++i) {
        const int   sec_tool   = sec_tool_ids[i];
        const Vec2d sec_center = sec_center_of(sec_tool);
        const ImexRole sec_role = sec_tool_roles[sec_tool];

        // A carriage stays inside its own zone; a Mirror reflects its
        // zone-relative offset about that zone's centerline, on the axis of the
        // boundary it shares with primary — the same rule the ghosts use:
        //   Copy                  → tracks primary on both axes.
        //   Mirror, same gantry   → reflect X (zones sit side by side).
        //   Mirror, other gantry  → reflect Y (zones sit front-to-back); X
        //                           tracks primary, since the part off that
        //                           gantry is a Y-reflection of the tool
        //                           directly behind it.
        // The axis comes from the shared helper, so the markers can never drift
        // out of step with the plate ghosts.
        const bool is_mirror    = (sec_role == ImexRole::Mirror);
        const bool cross_gantry = imex_mirror_axis_for(pri_tool, sec_tool,
                                                       tools_per_gantry) == ImexMirrorAxis::Y;

        // Tracking an axis translates by the gap between the two zone centres.
        // Mirroring it reflects primary about the midpoint of those centres —
        // the zones being equal-sized, that midpoint is the boundary between
        // them, so the carriage still lands inside its own zone. Both are a
        // single term applied to the live primary position each frame:
        // `term - pos` when mirrored, `pos + term` when tracked.
        ImexMarkerPlan::Carriage carriage;
        carriage.phys_head = sec_tool;
        carriage.mirror_x = is_mirror && !cross_gantry;
        carriage.mirror_y = is_mirror &&  cross_gantry;
        carriage.x_term   = carriage.mirror_x ? (float)(sec_center.x() + pri_center.x())
                                              : (float)(sec_center.x() - pri_center.x());
        carriage.y_term   = carriage.mirror_y ? (float)(sec_center.y() + pri_center.y())
                                              : (float)(sec_center.y() - pri_center.y());

        // The box shows the side a toolhead could COLLIDE from, not merely which
        // way its body hangs. Tools sharing a gantry share an X rail and can
        // actually run into each other, and only in a same-gantry (X-axis) mirror
        // do they converge — so that is the one case where the secondary's box
        // flips to face the primary. Tools on different gantries cannot collide
        // in X at all, so a cross-gantry mirror keeps the primary's facing, the
        // same as a Copy. Do not "fix" this to follow carriage geometry: a box on
        // the far side would point away from the only tool it can hit.
        if (is_mirror && !cross_gantry) {
            if      (sec_center.x() > pri_center.x()) carriage.box_offset_x = -plan.box_wx;
            else if (sec_center.x() < pri_center.x()) carriage.box_offset_x = 0.0f;
            else                                      carriage.box_offset_x = plan.pri_box_offset_x;
        } else {
            carriage.box_offset_x = plan.pri_box_offset_x;
        }
        // Y follows the same collision rule: face the gantry you could hit. A tool
        // on ANOTHER row faces the primary's row; a tool on the primary's OWN row
        // shares its gantry and can only hit the same other gantry, so it faces
        // wherever the primary faces. The old `else` hardcoded -imex_box_wy, which
        // happens to equal pri_box_offset_y on the rear-* layouts (primary's row
        // sits above the others, so the loop above settles on -box_wy anyway)
        // — hence a no-op there. On the front-* layouts the primary flips to 0.0f
        // and the hardcoded value pointed the same-gantry secondary away from the
        // only tools it could run into.
        if      (sec_center.y() > pri_center.y()) carriage.box_offset_y = -plan.box_wy;
        else if (sec_center.y() < pri_center.y()) carriage.box_offset_y = 0.0f;
        else                                      carriage.box_offset_y = plan.pri_box_offset_y;

        plan.carriages.push_back(carriage);
    }

    return plan;
}

//BBS: GUI refactor: add canvas width and height
void GCodeViewer::render_scene(int canvas_width, int canvas_height)
{
    glsafe(::glEnable(GL_DEPTH_TEST));
    render_shells();
    // while dragging in the solid model mode, the objects stand in for their toolpaths, cut to the
    // visible layer range; the toolpath set then holds only the range's bottom and top layers
    if (m_viewer.is_reduced_detail() && solid_model_enabled())
        render_solid_model(canvas_width, canvas_height);

    if (m_viewer.get_extrusion_roles_count() == 0)
        return;

    render_toolpaths();
    // ORCA: the markers write depth, which would hide the translucent toolpaths behind them
    if (!m_viewer.has_translucent_toolpaths())
        render_markers(canvas_width, canvas_height);
}

void GCodeViewer::render_markers(int canvas_width, int canvas_height)
{
    auto current = m_viewer.get_view_visible_range();
    auto endpoints = m_viewer.get_view_full_range();
    m_sequential_view.m_show_marker = m_sequential_view.m_show_marker || (current.back() != endpoints.back() && !m_no_render_path);
    const libvgcode::PathVertex& curr_vertex = m_viewer.get_current_vertex();
    m_sequential_view.marker.set_world_position(libvgcode::convert(curr_vertex.position));
    m_sequential_view.marker.set_z_offset(m_z_offset + 0.5f);

    // IDEX/IQEX: compute all carriage positions; update secondary nozzle markers;
    // carriage_box_draws is populated for toolhead footprint rendering after render_marker().
    // Okabe-Ito colorblind-safe palette — excludes orange (#E69F00) and sky blue (#56B4E9)
    // which are used by the bed zone fills, ensuring the markers contrast against the background.
    // It identifies the carriages in the views that colour by anything but filament; in Filament
    // view a carriage takes its own head's filament colour instead, so a marker, the ghost of
    // what it prints and the toolpaths under it all agree on what is loaded there.
    static const std::array<ColorRGBA, 4> s_carriage_colors = {{
        { 1.000f, 1.000f, 1.000f, 0.65f },   // primary      — white
        { 0.941f, 0.894f, 0.259f, 0.65f },   // secondary 1  — yellow       (#F0E442)
        { 0.835f, 0.369f, 0.000f, 0.65f },   // secondary 2  — vermilion    (#D55E00)
        { 0.800f, 0.475f, 0.655f, 0.65f },   // secondary 3  — reddish purple (#CC79A7)
    }};
    struct CarriageDraw { Vec3f pos; ColorRGBA color; float box_offset_x = 0.0f; float box_offset_y = 0.0f; };
    std::vector<CarriageDraw> carriage_box_draws;
    float imex_box_wx = 0.0f, imex_box_wy = 0.0f;
    {
        PresetBundle* preset_bundle = wxGetApp().preset_bundle;
        bool imex_active = false;
        if (preset_bundle && m_sequential_view.m_show_marker) {
            const DynamicPrintConfig& printer_cfg = preset_bundle->printers.get_edited_preset().config;
            auto* is_imex_opt = printer_cfg.opt<ConfigOptionBool>("is_imex");
            if (is_imex_opt && is_imex_opt->value) {
                const DynamicPrintConfig& process_cfg = preset_bundle->prints.get_edited_preset().config;
                auto* mode_opt = process_cfg.opt<ConfigOptionString>("imex_parallel_mode");
                std::string mode = mode_opt ? mode_opt->value : kImexPrimaryMode;
                // Per-plate mode overrides the process preset.
                PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();
                PartPlate*     curr_plate = plate_list.get_curr_plate();
                if (curr_plate) {
                    // Same resolution the plate itself uses, rather than a fourth copy of it.
                    const std::string plate_mode = curr_plate->get_effective_imex_mode();
                    if (plate_mode != kImexPrimaryMode)
                        mode = plate_mode;
                }

                // Bed X and Y bounds — read from the current plate's shape, which is
                // in world/GL coordinates (same space as curr_vertex.position), and is
                // the exact same source PartPlate::calc_imex_zones() hands to the library.
                BoundingBoxf bed_extents;
                {
                    const Pointfs& plate_shape = curr_plate ? curr_plate->get_shape() : Pointfs{};
                    if (!plate_shape.empty()) {
                        bed_extents = get_extents(plate_shape);
                    } else {
                        // Fallback: use toolpath bounding box extent
                        bed_extents = BoundingBoxf(Vec2d(m_paths_bounding_box.min.x(), m_paths_bounding_box.min.y()),
                                                   Vec2d(m_paths_bounding_box.max.x(), m_paths_bounding_box.max.y()));
                    }
                }

                // Cache guard. The marker plan below is resolved from preset/plate state only,
                // so it is rebuilt when that state moves and reused otherwise — the sequential
                // slider is sticky, and re-resolving the zone layout on every frame of the rest
                // of the session showed up as a measurable per-frame cost.
                ImexMarkerKey::Plate plate;
                plate.plate_index = plate_list.get_curr_plate_index();
                plate.bed_min_x   = bed_extents.min.x();
                plate.bed_min_y   = bed_extents.min.y();
                plate.bed_max_x   = bed_extents.max.x();
                plate.bed_max_y   = bed_extents.max.y();

                const bool layout_changed = update_imex_layout_key(m_imex_marker_key.layout, &printer_cfg, mode);
                if (layout_changed || !(m_imex_marker_key.plate == plate)) {
                    m_imex_marker_key.plate = plate;
                    m_imex_marker_plan = resolve_imex_marker_plan(printer_cfg, mode, bed_extents);
                    // The carriage roster — and with it the marker count — can change with any
                    // of those inputs, so drop the markers and let the lazy init below rebuild
                    // them. Their colours are restamped every frame regardless.
                    m_sequential_view.m_imex_secondary_markers.clear();
                }

                const ImexMarkerPlan& plan      = m_imex_marker_plan;
                const int             sec_count = (int)plan.carriages.size();

                // Filament view colours the toolpaths by the filament printing them, so a
                // carriage takes its own head's filament colour there and the palette
                // everywhere else. get_imex_head_filament_color() answers UNPRINTABLE_COLOR
                // for a head it cannot resolve, which is alpha 0.5 where a filament colour
                // decodes opaque - that difference, not the black, is what distinguishes it,
                // and a head with no filament keeps the palette rather than borrowing a
                // colour that would read as a real assignment - the one point where a marker
                // and the ghost of what it prints differ, the ghost having no palette to fall
                // back to.
                //
                // The map and the pem are hoisted and each head resolved once, as
                // PartPlate::update_imex_ghost_colors() does for the ghosts: both the marker
                // and the toolhead box of a carriage want the same answer, and rebuilding it
                // per draw parses the plate's map string and rebuilds the pem vector.
                const bool color_by_filament = m_viewer.get_view_type() == libvgcode::EViewType::ColorPrint;
                std::vector<ColorRGBA> carriage_colors(sec_count + 1);
                if (sec_count > 0) {
                    const ConfigOptionInts pem       = effective_physical_extruder_map(*preset_bundle);
                    const auto             plate_map = curr_plate ? curr_plate->get_imex_head_filament_map()
                                                                  : std::map<int, int>{};
                    std::map<int, ColorRGBA> head_colors;
                    auto color_for = [&](int phys_head, size_t palette_index) {
                        const ColorRGBA fallback = s_carriage_colors[palette_index % s_carriage_colors.size()];
                        if (!color_by_filament || curr_plate == nullptr || phys_head < 0)
                            return fallback;
                        auto hc = head_colors.find(phys_head);
                        if (hc == head_colors.end())
                            hc = head_colors.emplace(phys_head,
                                     curr_plate->get_imex_head_filament_color(phys_head, pem, plate_map)).first;
                        if (hc->second == GLVolume::UNPRINTABLE_COLOR)
                            return fallback;
                        ColorRGBA c = hc->second;
                        c.a(fallback.a());   // as translucent as the palette makes them
                        return c;
                    };
                    carriage_colors[0] = color_for(plan.pri_head, 0);
                    for (int i = 0; i < sec_count; ++i)
                        carriage_colors[i + 1] = color_for(plan.carriages[i].phys_head, i + 1);
                }
                if (sec_count > 0) {
                    imex_active = true;
                    imex_box_wx = plan.box_wx;
                    imex_box_wy = plan.box_wy;

                    // Lazy init secondary nozzle markers
                    if ((int)m_sequential_view.m_imex_secondary_markers.size() != sec_count) {
                        m_sequential_view.m_imex_secondary_markers.resize(sec_count);
                        for (int i = 0; i < sec_count; ++i) {
                            m_sequential_view.m_imex_secondary_markers[i].init(m_marker_filename);
                        }
                    }
                    // Set every frame, not once at init: the view type and the filament palette
                    // both change without the marker list being rebuilt. The primary marker is
                    // one of the carriages here, so it is coloured alongside them - and restored
                    // below when no mode is active, since it is the same marker every non-IMEX
                    // preview draws.
                    m_sequential_view.marker.set_color(carriage_colors[0]);
                    for (int i = 0; i < sec_count; ++i)
                        m_sequential_view.m_imex_secondary_markers[i].set_color(carriage_colors[i + 1]);

                    const Vec3f prim_pos = libvgcode::convert(curr_vertex.position);
                    carriage_box_draws.push_back({ prim_pos, carriage_colors[0],
                                                   plan.pri_box_offset_x, plan.pri_box_offset_y });

                    for (int i = 0; i < sec_count; ++i) {
                        const ImexMarkerPlan::Carriage& carriage = plan.carriages[i];
                        // Tracking an axis translates the primary by the gap between the two
                        // zone centres; mirroring reflects it about the midpoint of those
                        // centres — the zones being equal-sized, that midpoint is the boundary
                        // between them, so the carriage still lands inside its own zone. Which
                        // axis does which, and both terms, come from resolve_imex_marker_plan().
                        const Vec3f sec_pos{ carriage.mirror_x ? carriage.x_term - prim_pos.x()
                                                               : prim_pos.x() + carriage.x_term,
                                             carriage.mirror_y ? carriage.y_term - prim_pos.y()
                                                               : prim_pos.y() + carriage.y_term,
                                             prim_pos.z() };
                        m_sequential_view.m_imex_secondary_markers[i].set_world_position(sec_pos);
                        m_sequential_view.m_imex_secondary_markers[i].set_z_offset(m_z_offset + 0.5f);
                        m_sequential_view.m_imex_secondary_markers[i].set_nozzle_axis(m_sequential_view.marker.get_nozzle_axis());
                        carriage_box_draws.push_back({
                            sec_pos, carriage_colors[i + 1],
                            carriage.box_offset_x, carriage.box_offset_y });
                    }
                }
            }
        }
        if (!imex_active) {
            m_sequential_view.m_imex_secondary_markers.clear();
            // The primary marker is the one every preview draws, so the carriage colour it may
            // have been given has to come back off when no parallel mode is active.
            m_sequential_view.marker.set_color(SequentialView::Marker::default_color());
        }
    }
    m_sequential_view.render_marker(!m_no_render_path, canvas_width, canvas_height, m_viewer.get_view_type());

    // IDEX/IQEX: render toolhead footprint boxes for each active carriage.
    // Each box is imex_nozzle_clearance_x × imex_nozzle_clearance_y, sitting above the nozzle tip.
    // Hidden when the user toggles off "Show IDEX/IQEX Toolhead Boxes" in the View menu — gives
    // an unobstructed view of the toolpaths during sequential playback. Default-on; the absence
    // of an app_config entry is also treated as on.
    const bool show_toolhead_boxes = wxGetApp().app_config->get("show_imex_toolhead_boxes") != "false";
    if (show_toolhead_boxes && !carriage_box_draws.empty() && imex_box_wx > 0.0f && imex_box_wy > 0.0f) {
        // The mesh depends only on the two clearance dimensions, which a config edit can
        // change without a G-code reload — so rebuild it when they move (or when it has not
        // been built yet) rather than on every frame.
        // Mesh origin: nozzle at x=0, centered in Y, Z starts at nozzle tip level.
        // Per-carriage box_offset_x shifts the mesh left or right so the nozzle lands
        // at the correct (collision-side) edge.
        if (!m_imex_toolhead_box.is_initialized() ||
            m_imex_box_mesh_wx != imex_box_wx || m_imex_box_mesh_wy != imex_box_wy) {
            const float box_h = std::max(imex_box_wx, imex_box_wy);
            indexed_triangle_set its = its_make_cube((double)imex_box_wx, (double)imex_box_wy, (double)box_h);
            // No vertex pre-shifting — box_offset_x/y in the per-carriage transform
            // positions the nozzle at the correct collision-side edge.
            m_imex_toolhead_box.reset();
            m_imex_toolhead_box.init_from(its);
            m_imex_box_mesh_wx = imex_box_wx;
            m_imex_box_mesh_wy = imex_box_wy;
        }

        GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
        if (shader) {
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            shader->start_using();
            shader->set_uniform("emission_factor", 0.0f);
            const Camera& camera = wxGetApp().plater()->get_camera();
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            const Transform3d& view_matrix = camera.get_view_matrix();
            for (const auto& draw : carriage_box_draws) {
                const Transform3d model_matrix = Geometry::translation_transform(
                    (draw.pos + Vec3f(draw.box_offset_x, draw.box_offset_y, m_z_offset)).cast<double>());
                shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
                const Matrix3d view_normal_matrix =
                    view_matrix.matrix().block(0, 0, 3, 3) *
                    model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
                shader->set_uniform("view_normal_matrix", view_normal_matrix);
                m_imex_toolhead_box.set_color(draw.color);
                m_imex_toolhead_box.render();
            }
            shader->stop_using();
            glsafe(::glDisable(GL_BLEND));
        }
    }
}

size_t GCodeViewer::shadow_casters_signature() const
{
    size_t hash = m_viewer.get_vertices_count();
    const libvgcode::Interval& visible = m_viewer.get_view_visible_range();
    const libvgcode::Interval& layers  = m_viewer.get_layers_view_range();
    for (size_t value : { size_t(visible[0]), size_t(visible[1]), size_t(layers[0]), size_t(layers[1]) })
        boost::hash_combine(hash, value);
    for (double value : { m_paths_bounding_box.min.x(), m_paths_bounding_box.min.y(), m_paths_bounding_box.min.z(),
                          m_paths_bounding_box.max.x(), m_paths_bounding_box.max.y(), m_paths_bounding_box.max.z() })
        boost::hash_combine(hash, value);
    boost::hash_combine(hash, m_viewer.is_top_layer_only_view_range());
    // the casters are drawn from the reduced set while dragging, with the solid model in its mode
    boost::hash_combine(hash, m_viewer.is_reduced_detail());
    boost::hash_combine(hash, static_cast<int>(m_reduced_detail_mode));
    for (size_t i = 0; i < libvgcode::GCODE_EXTRUSION_ROLES_COUNT; ++i)
        boost::hash_combine(hash, m_viewer.is_extrusion_role_visible(libvgcode::EGCodeExtrusionRole(i)));
    boost::hash_combine(hash, m_viewer.is_option_visible(libvgcode::EOptionType::Travels));
    boost::hash_combine(hash, m_viewer.is_option_visible(libvgcode::EOptionType::Wipes));
    for (float value : m_clipping_plane)
        boost::hash_combine(hash, value);
    return hash;
}

void GCodeViewer::render_shadow_casters(const Transform3d& light_view_matrix, const Transform3d& light_projection_matrix, const Vec3d& light_position)
{
    if (!has_data())
        return;

    m_viewer.render_shadow_casters(
        libvgcode::convert(static_cast<Matrix4f>(light_view_matrix.matrix().cast<float>())),
        libvgcode::convert(static_cast<Matrix4f>(light_projection_matrix.matrix().cast<float>())),
        libvgcode::convert(static_cast<Vec3f>(light_position.cast<float>())));
    if (m_viewer.is_reduced_detail() && solid_model_enabled())
        render_solid_model_casters(light_view_matrix, light_projection_matrix);
}

// The stencil count that tells where a body cut at one cap alone is open: with the depth test off,
// its back faces add one and its front faces take one away, so a pixel where the ray leaves the
// body through the opening ends nonzero. The caller draws the body between the two. Depth clamping
// keeps the faces past the near plane in the count: the light's frustum ends at the top visible
// layer, so without it a part above the range keeps only its far faces and counts as open.
static void begin_cap_count()
{
    glsafe(::glEnable(GL_DEPTH_CLAMP));
    glsafe(::glStencilMask(0xFF));
    glsafe(::glClear(GL_STENCIL_BUFFER_BIT));
    glsafe(::glStencilFunc(GL_ALWAYS, 0, 0xFF));
    glsafe(::glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_DECR_WRAP));
    glsafe(::glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_INCR_WRAP));
    glsafe(::glDepthMask(GL_FALSE));
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
}

// After the count, what is drawn next lands only where the body was open.
static void end_cap_count()
{
    glsafe(::glDisable(GL_DEPTH_CLAMP));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glStencilFunc(GL_NOTEQUAL, 0, 0xFF));
    glsafe(::glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP));
    glsafe(::glStencilMask(0x00));
}

// The solid casts as the toolpaths it stands in for would: the shells, cut at the same heights,
// drawn depth-only into the shadow map the canvas has bound, and the cut faces with them. Without
// its cut faces an open body lets the light through: down onto the bed through a band cut at both
// ends, or onto its own far wall, with the end band's lines throwing their shadow through the
// hollow. The cut faces are counted and drawn as render_solid_model() draws them, which takes a
// stencil the map has not got, so they go into a depth target of the viewer's first, the map's
// size, and are copied into the map from there.
void GCodeViewer::render_solid_model_casters(const Transform3d& light_view_matrix, const Transform3d& light_projection_matrix)
{
    const std::optional<std::array<float, 2>> z_range = solid_model_z_range();
    GLShaderProgram* shader = wxGetApp().get_shader("flat_clip");
    if (m_shells.volumes.empty() || !z_range.has_value() || shader == nullptr)
        return;

    // the walls, cut by the section view as the camera sees it, so what it cuts away casts nothing
    shader->start_using();
    shader->set_uniform("projection_matrix", light_projection_matrix);
    shader->set_uniform("clipping_plane", m_clipping_plane);
    shader->set_uniform("z_range", *z_range);
    for (GLVolume* volume : m_shells.volumes.volumes) {
        const Transform3d world_matrix = volume->world_matrix();
        shader->set_uniform("volume_world_matrix", world_matrix);
        shader->set_uniform("view_model_matrix", light_view_matrix * world_matrix);
        volume->render();
    }
    shader->stop_using();

    GLShaderProgram* copy_shader = wxGetApp().get_shader("depth_copy");
    GLint viewport[4] = { 0, 0, 0, 0 };
    glsafe(::glGetIntegerv(GL_VIEWPORT, viewport));
    // without a target of its own the body casts open
    if (copy_shader == nullptr || !init_caster_depth(viewport[2], viewport[3]))
        return;
    std::array<SolidModelCap, 3> caps;
    init_solid_model_caps(*z_range, caps);

    GLint map_fbo = 0;
    glsafe(::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &map_fbo));
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, m_caster_fbo));
    glsafe(::glStencilMask(0xFF));
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
    glsafe(::glEnable(GL_STENCIL_TEST));
    shader->start_using();
    shader->set_uniform("projection_matrix", light_projection_matrix);
    for (GLVolume* volume : m_shells.volumes.volumes) {
        const Transform3d world_matrix = volume->world_matrix();
        for (SolidModelCap& cap : caps) {
            if (!cap.quad.is_initialized())
                continue;
            begin_cap_count();
            shader->set_uniform("z_range", cap.count_z_range);
            shader->set_uniform("clipping_plane", cap.count_plane);
            shader->set_uniform("volume_world_matrix", world_matrix);
            shader->set_uniform("view_model_matrix", light_view_matrix * world_matrix);
            volume->render();
            end_cap_count();
            shader->set_uniform("z_range", cap.quad_z_range);
            shader->set_uniform("clipping_plane", cap.quad_plane);
            shader->set_uniform("volume_world_matrix", Transform3d::Identity());
            shader->set_uniform("view_model_matrix", light_view_matrix);
            cap.quad.render();
        }
    }
    shader->stop_using();
    glsafe(::glStencilMask(0xFF));
    glsafe(::glDisable(GL_STENCIL_TEST));

    // into the map, where nearer than what is there
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(map_fbo)));
    GLModel screen;
    GLModel::Geometry data;
    data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    data.add_vertex(Vec3f(-1.0f, -1.0f, 0.0f));
    data.add_vertex(Vec3f(1.0f, -1.0f, 0.0f));
    data.add_vertex(Vec3f(1.0f, 1.0f, 0.0f));
    data.add_vertex(Vec3f(-1.0f, 1.0f, 0.0f));
    data.add_triangle(0, 1, 2);
    data.add_triangle(0, 2, 3);
    screen.init_from(std::move(data));
    // bound on a unit of its own, with the volumes' own bindings left to unit 0
    glsafe(::glActiveTexture(GL_TEXTURE6));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_caster_depth));
    glsafe(::glActiveTexture(GL_TEXTURE0));
    copy_shader->start_using();
    copy_shader->set_uniform("depth_texture", 6);
    copy_shader->set_uniform("map_size", std::array<float, 2>{ static_cast<float>(viewport[2]), static_cast<float>(viewport[3]) });
    screen.render();
    copy_shader->stop_using();
    glsafe(::glActiveTexture(GL_TEXTURE6));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    glsafe(::glActiveTexture(GL_TEXTURE0));
}

// The depth and stencil target the caster pass draws the cut faces into, the size of the shadow
// map they are copied into.
bool GCodeViewer::init_caster_depth(int width, int height)
{
    if (OpenGLManager::get_framebuffers_type() != OpenGLManager::EFramebufferType::Arb || width <= 0 || height <= 0)
        return false;
    if (m_caster_depth != 0 && m_caster_width == width && m_caster_height == height)
        return true;
    if (m_caster_depth == 0) {
        glsafe(::glGenTextures(1, &m_caster_depth));
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_caster_depth));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    }
    else
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_caster_depth));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    m_caster_width = width;
    m_caster_height = height;

    GLint bound_fbo = 0;
    glsafe(::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound_fbo));
    if (m_caster_fbo == 0)
        glsafe(::glGenFramebuffers(1, &m_caster_fbo));
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, m_caster_fbo));
    glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, m_caster_depth, 0));
    glsafe(::glDrawBuffer(GL_NONE));
    glsafe(::glReadBuffer(GL_NONE));
    const bool complete = ::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(bound_fbo)));
    if (!complete)
        release_caster_depth();
    return complete;
}

void GCodeViewer::release_caster_depth()
{
    if (m_caster_fbo != 0) {
        glsafe(::glDeleteFramebuffers(1, &m_caster_fbo));
        m_caster_fbo = 0;
    }
    if (m_caster_depth != 0) {
        glsafe(::glDeleteTextures(1, &m_caster_depth));
        m_caster_depth = 0;
    }
    m_caster_width = 0;
    m_caster_height = 0;
}

void GCodeViewer::set_shadow_map(int texture_unit, const Transform3d& light_view_projection, float intensity, float texel_size)
{
    m_shadow_map = { texture_unit, light_view_projection, intensity, texel_size };
    m_viewer.set_shadow_map(texture_unit,
        libvgcode::convert(static_cast<Matrix4f>(light_view_projection.matrix().cast<float>())),
        intensity, texel_size);
}

void GCodeViewer::set_light_top_dir(const Vec3d& direction)
{
    m_light_top_dir = direction;
    m_viewer.set_light_top_dir(libvgcode::convert(static_cast<Vec3f>(direction.cast<float>())));
}

void GCodeViewer::set_tone(float exposure, float saturation)
{
    m_viewer.set_tone(exposure, saturation);
}

void GCodeViewer::set_clipping_plane(const ClippingPlane& plane)
{
    // Flipped to match ClippingPlane::distance().
    const Vec3f normal = -plane.get_normal().cast<float>();
    m_clipping_plane = plane.is_active() ?
        std::array<float, 4>{ normal.x(), normal.y(), normal.z(), float(plane.get_offset()) } :
        std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f };
    m_viewer.set_clipping_plane(m_clipping_plane);
}

void GCodeViewer::render_overlay(int canvas_width, int canvas_height, const PreviewOverlayLayout& layout)
{
    m_panels->render(layout);

    const int move_number = m_moves_slider->GetHigherValue() - m_moves_slider->GetMinValue() + 1;
    const int move_count  = m_moves_slider->GetMaxValue() - m_moves_slider->GetMinValue() + 1;
    m_sequential_view.render_overlay(!m_no_render_path, panel_place(PreviewPanel::GCode, layout), &m_viewer, m_viewer.get_current_vertex().gcode_id, layout, move_number, move_count, m_viewer.get_view_type());

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

    render_slider(layout);
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

            // ORCA Compress consecutive duplicate speeds with 0.1 precision
            auto sameSpeed = [](float a, float b) {
                return static_cast<int>(std::roundf(a * 10.0f)) == static_cast<int>(std::roundf(b * 10.0f));
            };
            std::vector<SequentialView::ActualSpeedImguiWidget::Item> compressed;
            if (!actual_speed_data.empty()) {
                compressed.push_back(actual_speed_data[0]);
                for (int i = 1; i < (int)actual_speed_data.size(); ++i) {
                    const bool same_as_prev = sameSpeed(actual_speed_data[i].speed, actual_speed_data[i - 1].speed);
                    const bool same_as_next = (i + 1 < (int)actual_speed_data.size()) && sameSpeed(actual_speed_data[i].speed, actual_speed_data[i + 1].speed);
                    if (!same_as_prev) {
                        if (!sameSpeed(compressed.back().speed, actual_speed_data[i - 1].speed))
                            compressed.push_back(actual_speed_data[i - 1]);
                        compressed.push_back(actual_speed_data[i]);
                    } else if (!same_as_next)
                        compressed.push_back(actual_speed_data[i]);
                }
                if (compressed.back().pos != actual_speed_data.back().pos)
                    compressed.push_back(actual_speed_data.back());
            }

            m_sequential_view.marker.set_actual_speed_data(compressed);
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

void GCodeViewer::set_interacting(bool interacting)
{
    // With the mode off nothing changes while dragging. The solid model needs shells to stand in
    // for the toolpaths and a single height to cut them at; a belt printer's layers are tilted
    // planes, which no height cuts.
    const bool usable = m_reduced_detail_mode != libvgcode::EReducedDetailMode::Off &&
                        (!solid_model_enabled() || (!m_shells.volumes.empty() && m_layers_z_rising && !m_belt_view_enabled));
    // sampled afresh as each drag begins: the legend may have recoloured the toolpaths, and the
    // sliders may have moved the span, since the last one
    if (interacting && !m_viewer.is_reduced_detail())
        m_solid_model_colors.valid = false;
    m_viewer.set_reduced_detail(interacting && usable);
}

void GCodeViewer::read_reduced_detail_preferences()
{
    m_reduced_detail_mode = reduced_detail_mode_from_string(get_app_config()->get("preview_reduced_detail_mode"));
    m_viewer.set_reduced_detail_mode(m_reduced_detail_mode);
}

void GCodeViewer::set_reduced_detail_mode(const std::string& mode)
{
    const bool was_solid = solid_model_enabled();
    m_reduced_detail_mode = reduced_detail_mode_from_string(mode);
    m_viewer.set_reduced_detail_mode(m_reduced_detail_mode);

    // Only the prime tower comes and goes with the solid model: a full reload would drop the shells
    // whenever the print has moved on since they were loaded, leaving the solid model nothing to draw.
    if (was_solid == solid_model_enabled() || m_shells.print_id == -1 || wxGetApp().plater() == nullptr)
        return;
    // the shells are loaded from the current plate's print, which is not the plater's own
    const Print& print = wxGetApp().plater()->get_partplate_list().get_current_fff_print();
    if (static_cast<int>(print.id().id) == m_shells.print_id)
        update_shell_wipe_tower(print, m_gl_data_initialized);
}

libvgcode::EReducedDetailMode GCodeViewer::reduced_detail_mode_from_string(const std::string& mode)
{
    if (mode == "solid")
        return libvgcode::EReducedDetailMode::EndLayersOnly;
    if (mode == "shell")
        return libvgcode::EReducedDetailMode::ShellOnly;
    return libvgcode::EReducedDetailMode::Off;
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
    // the shells can load before the first G-code does, so the preferences are read here as well
    read_reduced_detail_preferences();
    if ((print.id().id == m_shells.print_id)&&(print.get_modified_count() == m_shells.print_modify_count)) {
        // the prime tower comes and goes on its own, without reloading the objects
        update_shell_wipe_tower(print, initialized);
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
    update_shell_wipe_tower(print, initialized);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": shell loaded, id change to %1%, modify_count %2%, object count %3%, glvolume count %4%")
        % m_shells.print_id % m_shells.print_modify_count % object_count %m_shells.volumes.volumes.size();
}

ColorRGBA GCodeViewer::shown_filament_color(ColorRGBA color) const
{
    if (!m_viewer.is_translucent_filaments())
        color.a(1.0f);
    return color;
}

void GCodeViewer::render_translucent_toolpaths(int canvas_width, int canvas_height)
{
    if (!m_viewer.has_translucent_toolpaths())
        return;

    const Camera& camera = wxGetApp().plater()->get_camera();
    m_viewer.render_translucent(libvgcode::convert(static_cast<Matrix4f>(camera.get_view_matrix().matrix().cast<float>())),
                                libvgcode::convert(static_cast<Matrix4f>(camera.get_projection_matrix().matrix().cast<float>())));
    glsafe(::glEnable(GL_DEPTH_TEST));
    render_markers(canvas_width, canvas_height);
}

// The prime tower as it was sliced, so that the solid model shows what the print shows. It keeps its
// opaque colour, so it never appears among the translucent shells, and stays out of their bounding box.
void GCodeViewer::update_shell_wipe_tower(const Print& print, bool initialized)
{
    const bool with_wipe_tower = solid_model_enabled() && print.is_step_done(psWipeTower) && print.wipe_tower_data().wipe_tower_mesh_data;
    if (with_wipe_tower == m_shells.with_wipe_tower)
        return;
    m_shells.with_wipe_tower = with_wipe_tower;
    GLVolumePtrs& volumes = m_shells.volumes.volumes;
    if (!with_wipe_tower) {
        volumes.erase(std::remove_if(volumes.begin(), volumes.end(), [](GLVolume* volume) {
            if (!volume->is_wipe_tower)
                return false;
            delete volume;
            return true;
        }), volumes.end());
        return;
    }
    const PrintConfig& config = print.config();
    const int plate_idx = print.get_plate_index();
    const Vec3d plate_origin = print.get_plate_origin();
    const float x = static_cast<float>(config.wipe_tower_x.get_at(plate_idx) + plate_origin.x());
    const float y = static_cast<float>(config.wipe_tower_y.get_at(plate_idx) + plate_origin.y());
    const size_t first_new = volumes.size();
    m_shells.volumes.load_real_wipe_tower_preview(1000 + plate_idx, x, y, print.wipe_tower_data().wipe_tower_mesh_data->real_wipe_tower_mesh,
                                                  print.wipe_tower_data().wipe_tower_mesh_data->real_brim_mesh, true,
                                                  static_cast<float>(config.wipe_tower_rotation_angle), false, initialized);
    for (size_t i = first_new; i < volumes.size(); ++i) {
        volumes[i]->zoom_to_volumes = false;
        volumes[i]->force_native_color = true;
        volumes[i]->set_render_color();
    }
}

void GCodeViewer::render_toolpaths()
{
    const Camera& camera = wxGetApp().plater()->get_camera();
    Matrix4f view = camera.get_view_matrix().matrix().cast<float>();
    // Belt "designed" (upright) view is produced by back-transforming the toolpath
    // GEOMETRY into model space at load time (see load_as_gcode ->
    // BeltTransformPipeline::build_gcode_back_transform -> libvgcode::convert). The camera is left untouched
    // here; transforming the view as well would double-apply the inverse. Baking the
    // transform into the geometry (not the camera) also keeps the bed and toolpaths in
    // the same frame so they stay aligned.
    const libvgcode::Mat4x4 converted_view_matrix = libvgcode::convert(view);
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
            add_range_property_row("acceleration range", m_viewer.get_color_range(libvgcode::EViewType::Acceleration).get_range());
            add_range_property_row("jerk range", m_viewer.get_color_range(libvgcode::EViewType::Jerk).get_range());
            add_range_property_row("fan speed range", m_viewer.get_color_range(libvgcode::EViewType::FanSpeed).get_range());
            add_range_property_row("temperature range", m_viewer.get_color_range(libvgcode::EViewType::Temperature).get_range());
// ORCA: Add Pressure Advance visualization support
            add_range_property_row("pressure advance range", m_viewer.get_color_range(libvgcode::EViewType::PressureAdvance).get_range());
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

void GCodeViewer::render_shells()
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
    m_shells.volumes.render(GLVolumeCollection::ERenderType::Transparent, false, camera.get_view_matrix(), camera.get_projection_matrix());
    shader->set_uniform("emission_factor", 0.0f);
    shader->stop_using();

    glsafe(::glDepthMask(GL_TRUE));
}

// The sliced objects and the prime tower drawn opaque, in their filament colours, cut to the
// visible layer range by the shader's z range. The toolpaths of the range's bottom and top layers
// are drawn afterwards and cap the cut.
// The layers at the range's ends are drawn as toolpaths, so the solid runs from the top of the
// bottom ones to the bottom of the top ones. A face of the model in either plane would otherwise
// fight the toolpaths for the same pixels and flicker while dragging.
// The layer heights are the toolpaths', which the printer's z offset is already taken out of, so
// they are in the shells' frame as they are.
// A layer with no extrusion has no height of its own; the nearest one below stands in for it.
std::optional<std::array<float, 2>> GCodeViewer::solid_model_z_range() const
{
    auto layer_top = [this](size_t id) {
        float z = m_viewer.get_layer_z(id);
        while (z == 0.0f && id > 0)
            z = m_viewer.get_layer_z(--id);
        return z;
    };
    const libvgcode::Interval whole = m_viewer.get_reduced_detail_whole_layers();
    const float z_top = (whole[1] > 0) ? layer_top(whole[1] - 1) - 0.001f : -FLT_MAX;
    const float z_bottom = layer_top(whole[0]) + 0.001f;
    // the toolpaths cover the whole range
    if (z_top <= z_bottom)
        return std::nullopt;
    return std::array<float, 2>{ z_bottom, z_top };
}

void GCodeViewer::init_solid_model_caps(const std::array<float, 2>& z_range, std::array<SolidModelCap, 3>& caps) const
{
    const auto [z_bottom, z_top] = z_range;
    auto init_quad = [](SolidModelCap& cap, const Vec3f& center, const Vec3f& u, const Vec3f& v) {
        GLModel::Geometry data;
        data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        data.reserve_vertices(4);
        data.reserve_indices(6);
        data.add_vertex(Vec3f(center - u - v));
        data.add_vertex(Vec3f(center + u - v));
        data.add_vertex(Vec3f(center + u + v));
        data.add_vertex(Vec3f(center - u + v));
        data.add_triangle(0, 1, 2);
        data.add_triangle(0, 2, 3);
        cap.quad.init_from(std::move(data));
    };
    const Vec3f box_min = m_paths_bounding_box.min.cast<float>();
    const Vec3f box_max = m_paths_bounding_box.max.cast<float>();
    const Vec3f box_center = 0.5f * (box_min + box_max);
    const float margin = 1.0f;
    const Vec3f half_x(0.5f * (box_max.x() - box_min.x()) + margin, 0.0f, 0.0f);
    const Vec3f half_y(0.0f, 0.5f * (box_max.y() - box_min.y()) + margin, 0.0f);
    if (m_viewer.get_layers_view_range()[0] > 0) {
        SolidModelCap& cap = caps[0];
        init_quad(cap, Vec3f(box_center.x(), box_center.y(), z_bottom), half_x, half_y);
        cap.count_z_range = { z_bottom, FLT_MAX };
        cap.quad_plane = m_clipping_plane;
        cap.color_index = 0;
    }
    if (z_top > -FLT_MAX) {
        SolidModelCap& cap = caps[1];
        init_quad(cap, Vec3f(box_center.x(), box_center.y(), z_top), half_x, half_y);
        cap.count_z_range = { -FLT_MAX, z_top };
        cap.quad_plane = m_clipping_plane;
        cap.color_index = 1;
    }
    // the section view cuts the solid as it cuts the toolpaths; nothing else sets a plane on the shells
    if (m_clipping_plane[0] != 0.0f || m_clipping_plane[1] != 0.0f || m_clipping_plane[2] != 0.0f) {
        SolidModelCap& cap = caps[2];
        const Vec3f normal(m_clipping_plane[0], m_clipping_plane[1], m_clipping_plane[2]);
        const Vec3f on_plane = box_center - (normal.dot(box_center) + m_clipping_plane[3]) * normal;
        // the in-plane axes, turned from the unit axis the normal leans on least
        const Vec3f leaning = normal.cwiseAbs();
        const Vec3f axis = (leaning.x() <= leaning.y() && leaning.x() <= leaning.z()) ? Vec3f::UnitX() :
                           (leaning.y() <= leaning.z()) ? Vec3f::UnitY() : Vec3f::UnitZ();
        const Vec3f u = normal.cross(axis).normalized();
        const Vec3f v = normal.cross(u);
        const float reach = 0.5f * (box_max - box_min).norm() + margin;
        init_quad(cap, on_plane, reach * u, reach * v);
        cap.count_plane = m_clipping_plane;
        cap.quad_z_range = { z_bottom, z_top };
        cap.color_index = 2;
    }
}

void GCodeViewer::render_solid_model(int canvas_width, int canvas_height)
{
    if (m_shells.volumes.empty())
        return;
    // gouraud_light has no z range, so it could not cut the model
    GLShaderProgram* shader = wxGetApp().get_shader("gouraud");
    if (shader == nullptr)
        return;
    const std::optional<std::array<float, 2>> z_range = solid_model_z_range();
    if (!z_range.has_value())
        return;
    const auto [z_bottom, z_top] = *z_range;
    const libvgcode::Interval& layers = m_viewer.get_layers_view_range();
    const libvgcode::Interval whole = m_viewer.get_reduced_detail_whole_layers();

    // the volumes wear the solid's colours for the frame, over their own filament colours
    update_solid_model_colors(whole);
    const SolidModelColors& colors = m_solid_model_colors;
    // The layers the solid stands for are never the ones a slider thumb lights, so the solid
    // wears what the toolpath shader gives the rest of the print: dimmed to the preference's
    // brightness while the viewer dims the layers around the inspected ones, dark grey while the
    // moves slider is short of the top layer's end; see libvgcode::ViewerImpl::set_layer_colors().
    const bool top_layer_only = m_viewer.is_top_layer_only_view_range();
    const bool moves_short = m_viewer.get_view_full_range()[1] != m_viewer.get_view_visible_range()[1];
    const bool inspecting = layers[0] > 0 || layers[1] + 1 < m_viewer.get_layers_count() || moves_short;
    const bool dimmed = top_layer_only && inspecting && m_viewer.is_dim_previous_layers();
    const bool greyed = !dimmed && top_layer_only && moves_short;
    auto shade = [dimmed, greyed, brightness = m_viewer.get_dim_previous_layers_brightness()](const ColorRGBA& color) {
        if (greyed)
            return ColorRGBA(64.0f / 255.0f, 64.0f / 255.0f, 64.0f / 255.0f, 1.0f);
        if (dimmed)
            return ColorRGBA(brightness * color.r(), brightness * color.g(), brightness * color.b(), 1.0f);
        return ColorRGBA(color.r(), color.g(), color.b(), 1.0f);
    };
    std::vector<ColorRGBA> own_colors;
    own_colors.reserve(m_shells.volumes.volumes.size());
    // the tower draws itself in its own colours and leaves the volume's at the default; one list per volume
    std::vector<std::vector<ColorRGBA>> own_tower_colors(m_shells.volumes.volumes.size());
    for (size_t k = 0; k < m_shells.volumes.volumes.size(); ++k) {
        GLVolume* volume = m_shells.volumes.volumes[k];
        own_colors.push_back(volume->color);
        const std::optional<ColorRGBA>& body = colors.bodies[k].body;
        if (auto* tower = dynamic_cast<GLWipeTowerVolume*>(volume); tower != nullptr) {
            own_tower_colors[k] = tower->colors();
            for (ColorRGBA& color : tower->colors())
                color = shade(body.value_or(color));
        }
        volume->color = shade(body.value_or(volume->color));
        volume->set_render_color();
    }
    m_shells.volumes.set_z_range(z_bottom, z_top);
    // the section view cuts the solid as it cuts the toolpaths
    m_shells.volumes.set_clipping_plane({ m_clipping_plane[0], m_clipping_plane[1], m_clipping_plane[2], m_clipping_plane[3] });

    shader->start_using();
    // The realistic view's shadow map, bound by the canvas for the toolpaths, shades the solid
    // too, under the same light; both are off when the map is not there. The 3D view leaves its
    // own settings on the shared program, so each is set here.
    shader->set_uniform("shadow_map", m_shadow_map.texture_unit);
    shader->set_uniform("shadow_light_vp", m_shadow_map.light_view_projection);
    shader->set_uniform("shadow_map_texel", m_shadow_map.texel_size);
    shader->set_uniform("shadow_intensity", m_shadow_map.intensity);
    shader->set_uniform("use_static_light", true);
    shader->set_uniform("static_light_dir", m_light_top_dir);
    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("z_far", camera.get_far_z());
    shader->set_uniform("z_near", camera.get_near_z());
    m_shells.volumes.render(GLVolumeCollection::ERenderType::Opaque, false, camera.get_view_matrix(), camera.get_projection_matrix());
    shader->stop_using();

    // The cut faces, one cap of one body at a time. A pixel looks into a body through a cut where,
    // with the depth test off and the body cut at that cap alone, its back faces outnumber its
    // front faces: zero where the ray misses the body or crosses it whole, one where it leaves
    // through the opening, which is where the cap plane lies inside the body. Counted with two
    // cuts at once the body is an open tube, and a ray through both openings would count as
    // closed, one in through a wall and out through the other opening as open. There a quad in
    // the cut plane, in the colour of the infill it is cut under darkened, or the body's own,
    // stands in for the material, so that a sparse layer on the cut shows a solid inside between
    // its lines rather than whatever lies further down. The section view's plane is one more
    // cut: its cap is counted on the body cut by it alone and drawn between the heights of the
    // other two, and theirs are drawn on its kept side.
    GLShaderProgram* cap_shader = wxGetApp().get_shader("flat_clip");
    if (cap_shader != nullptr) {
        std::array<SolidModelCap, 3> caps;
        init_solid_model_caps(*z_range, caps);
        glsafe(::glEnable(GL_STENCIL_TEST));
        for (size_t n = 0; n < m_shells.volumes.volumes.size(); ++n) {
            GLVolume* volume = m_shells.volumes.volumes[n];
            const auto* tower = dynamic_cast<const GLWipeTowerVolume*>(volume);
            const Transform3d world_matrix = volume->world_matrix();
            for (SolidModelCap& cap : caps) {
                if (!cap.quad.is_initialized())
                    continue;
                begin_cap_count();
                glsafe(::glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE));
                // The body is drawn directly, on the uniforms the pass above left on the program: the
                // collection's render would build the full preset config and sort every volume, per body.
                shader->start_using();
                shader->set_uniform("z_range", cap.count_z_range);
                shader->set_uniform("clipping_plane", cap.count_plane);
                shader->set_uniform("volume_world_matrix", world_matrix);
                shader->set_uniform("view_model_matrix", camera.get_view_matrix() * world_matrix);
                volume->render();
                shader->stop_using();
                glsafe(::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
                end_cap_count();

                ColorRGBA inside = volume->color;
                if (tower != nullptr && !tower->colors().empty())
                    inside = tower->colors().front();
                if (cap.color_index.has_value()) {
                    if (const std::optional<ColorRGBA>& sampled = colors.bodies[n].caps[*cap.color_index]; sampled.has_value())
                        inside = shade(*sampled);
                }
                inside = ColorRGBA(0.55f * inside.r(), 0.55f * inside.g(), 0.55f * inside.b(), 1.0f);
                cap_shader->start_using();
                cap_shader->set_uniform("view_model_matrix", camera.get_view_matrix());
                cap_shader->set_uniform("projection_matrix", camera.get_projection_matrix());
                cap_shader->set_uniform("volume_world_matrix", Transform3d::Identity());
                cap_shader->set_uniform("z_range", cap.quad_z_range);
                cap_shader->set_uniform("clipping_plane", cap.quad_plane);
                cap.quad.set_color(inside);
                cap.quad.render();
                cap_shader->stop_using();
            }
        }
        glsafe(::glEnable(GL_CULL_FACE));
        glsafe(::glStencilMask(0xFF));
        glsafe(::glDisable(GL_STENCIL_TEST));
    }

    m_shells.volumes.set_z_range(-FLT_MAX, FLT_MAX);
    for (size_t k = 0; k < m_shells.volumes.volumes.size(); ++k) {
        GLVolume* volume = m_shells.volumes.volumes[k];
        if (auto* tower = dynamic_cast<GLWipeTowerVolume*>(volume); tower != nullptr)
            tower->colors() = own_tower_colors[k];
        volume->color = own_colors[k];
        volume->set_render_color();
    }
}

// The solid stands in for the toolpaths, so it takes their colours in the view that colours them by
// something other than the filament: each body the outer wall's, each layer cap the infill's of the
// layer it is cut under, sparse if the layer has any, else solid, the section view's cap the
// infill's over all the layers, and the tower its own role's, each from the toolpaths within the
// body's own footprint, so that objects printed at different grades keep their own. The views that
// colour by filament leave every body its own colour. The bodies and the section view's cap are
// sampled over the layers the solid stands for and each layer cap in its own layer, each colour
// that of the vertex the viewer picks to represent the role there, so a view that grades the
// toolpaths, by speed or fan, shows the solid in the grade most of that role was printed at. The
// bodies are sampled once, as the drag begins, and held while a slider drag moves the span: sampled
// again at every step, the grade would flip with the layers the sample happens to fall on. The
// section view's cap is held with them; each layer cap follows its own layer.
void GCodeViewer::update_solid_model_colors(const libvgcode::Interval& whole_layers)
{
    SolidModelColors& colors = m_solid_model_colors;
    const libvgcode::EViewType view_type = m_viewer.get_view_type();
    const libvgcode::Focus& focus = m_viewer.get_focus();
    const std::vector<GLVolume*>& volumes = m_shells.volumes.volumes;
    const bool held = colors.valid && colors.view_type == view_type && colors.focus == focus && colors.bodies.size() == volumes.size();
    if (held && colors.whole_layers == whole_layers)
        return;
    if (!held) {
        colors = SolidModelColors();
        colors.valid = true;
        colors.view_type = view_type;
        colors.focus = focus;
        colors.bodies.resize(volumes.size());
    }
    colors.whole_layers = whole_layers;
    // ORCA: a body takes the colour of one of its paths, so it goes grey when that path is out of the legend's focus; coloured by
    // filament, it keeps its own colour otherwise.
    const bool by_filament = view_type == libvgcode::EViewType::Summary || view_type == libvgcode::EViewType::ColorPrint || view_type == libvgcode::EViewType::Tool;
    if (by_filament && !focus.any())
        return;

    using Role = libvgcode::EGCodeExtrusionRole;
    auto sample = [this, by_filament](const libvgcode::Interval& layers, std::initializer_list<Role> roles, const libvgcode::XYBox& box) -> std::optional<ColorRGBA> {
        static const ColorRGBA out_of_focus{ 150.0f / 255.0f, 150.0f / 255.0f, 150.0f / 255.0f, 1.0f };
        for (Role role : roles) {
            const size_t id = m_viewer.find_extrusion_vertex(layers, role, box);
            if (id < m_viewer.get_vertices_count()) {
                const libvgcode::PathVertex& vertex = m_viewer.get_vertex_at(id);
                if (!m_viewer.is_in_focus(vertex))
                    return out_of_focus;
                return by_filament ? std::nullopt : std::optional<ColorRGBA>(libvgcode::convert(m_viewer.get_vertex_color(vertex)));
            }
        }
        return std::nullopt;
    };
    const std::initializer_list<Role> walls = { Role::ExternalPerimeter, Role::Perimeter, Role::OverhangPerimeter };
    const std::initializer_list<Role> tower_role = { Role::WipeTower };
    const std::initializer_list<Role> infill = { Role::InternalInfill, Role::SolidInfill, Role::TopSolidInfill, Role::BottomSurface,
                                                 Role::BridgeInfill, Role::InternalBridgeInfill, Role::Ironing };
    for (size_t n = 0; n < volumes.size(); ++n) {
        const GLVolume* volume = volumes[n];
        const bool tower = dynamic_cast<const GLWipeTowerVolume*>(volume) != nullptr;
        // the walls' centrelines lie within the outline, pushed out by a compensation at most
        const BoundingBoxf3& footprint = volume->transformed_bounding_box();
        const float margin = 1.0f;
        const libvgcode::XYBox box{ { static_cast<float>(footprint.min.x()) - margin, static_cast<float>(footprint.min.y()) - margin },
                                    { static_cast<float>(footprint.max.x()) + margin, static_cast<float>(footprint.max.y()) + margin } };
        SolidModelColors::Body& body = colors.bodies[n];
        if (!held) {
            body.body = sample(whole_layers, tower ? tower_role : walls, box);
            // the section view's cut may cross any of the layers, so it is sampled over all of them, as the body is
            body.caps[2] = tower ? std::nullopt : sample(whole_layers, infill, box);
            if (!body.caps[2].has_value())
                body.caps[2] = body.body;
        }
        for (size_t k = 0; k < 2; ++k) {
            body.caps[k] = tower ? std::nullopt : sample({ whole_layers[k], whole_layers[k] }, infill, box);
            if (!body.caps[k].has_value())
                body.caps[k] = body.body;
        }
    }
}

void GCodeViewer::render_slider(const PreviewOverlayLayout& layout) {
    m_moves_slider->render(layout);
    m_moves_slider->add_wheel_area(m_sequential_view.marker.panel_rect());
    m_layers_slider->render(layout);
}

} // namespace GUI
} // namespace Slic3r

