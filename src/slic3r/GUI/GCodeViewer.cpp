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

static std::string get_view_type_string(GCodeViewer::EViewType view_type)
{
    if (view_type == GCodeViewer::EViewType::Summary)
        return _u8L("Summary");
    else if (view_type == GCodeViewer::EViewType::FeatureType)
        return _u8L("Line Type");
    else if (view_type == GCodeViewer::EViewType::Height)
        return _u8L("Layer Height");
    else if (view_type == GCodeViewer::EViewType::Width)
        return _u8L("Line Width");
    else if (view_type == GCodeViewer::EViewType::Feedrate)
        return _u8L("Speed");
    else if (view_type == GCodeViewer::EViewType::FanSpeed)
        return _u8L("Fan Speed");
    else if (view_type == GCodeViewer::EViewType::Temperature)
        return _u8L("Temperature");
    else if (view_type == GCodeViewer::EViewType::VolumetricRate)
        return _u8L("Flow");
    else if (view_type == GCodeViewer::EViewType::Tool)
        return _u8L("Tool");
    else if (view_type == GCodeViewer::EViewType::ColorPrint)
        return _u8L("Filament");
    else if (view_type == GCodeViewer::EViewType::LayerTime)
        return _u8L("Layer Time");
else if (view_type == GCodeViewer::EViewType::LayerTimeLog)
        return _u8L("Layer Time (log)");
    return "";
}

static unsigned char buffer_id(EMoveType type) {
    return static_cast<unsigned char>(type) - static_cast<unsigned char>(EMoveType::Retract);
}

static EMoveType buffer_type(unsigned char id) {
    return static_cast<EMoveType>(static_cast<unsigned char>(EMoveType::Retract) + id);
}

// Round to a bin with minimum two digits resolution.
// Equivalent to conversion to string with sprintf(buf, "%.2g", value) and conversion back to float, but faster.
static float round_to_bin(const float value)
{
//    assert(value > 0);
    constexpr float const scale    [5] = { 100.f,  1000.f,  10000.f,  100000.f,  1000000.f };
    constexpr float const invscale [5] = { 0.01f,  0.001f,  0.0001f,  0.00001f,  0.000001f };
    constexpr float const threshold[5] = { 0.095f, 0.0095f, 0.00095f, 0.000095f, 0.0000095f };
    // Scaling factor, pointer to the tables above.
    int                   i            = 0;
    // While the scaling factor is not yet large enough to get two integer digits after scaling and rounding:
    for (; value < threshold[i] && i < 4; ++ i) ;
    return std::round(value * scale[i]) * invscale[i];
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

void GCodeViewer::VBuffer::reset()
{
    // release gpu memory
    if (!vbos.empty()) {
        glsafe(::glDeleteBuffers(static_cast<GLsizei>(vbos.size()), static_cast<const GLuint*>(vbos.data())));
        vbos.clear();
    }
    sizes.clear();
    count = 0;
}

void GCodeViewer::InstanceVBuffer::Ranges::reset()
{
    for (Range& range : ranges) {
        // release gpu memory
        if (range.vbo > 0)
            glsafe(::glDeleteBuffers(1, &range.vbo));
    }

    ranges.clear();
}

void GCodeViewer::InstanceVBuffer::reset()
{
    s_ids.clear();
    s_ids.shrink_to_fit();
    buffer.clear();
    buffer.shrink_to_fit();
    render_ranges.reset();
}

void GCodeViewer::IBuffer::reset()
{
    // release gpu memory
    if (ibo > 0) {
        glsafe(::glDeleteBuffers(1, &ibo));
        ibo = 0;
    }

    vbo = 0;
    count = 0;
}

bool GCodeViewer::Path::matches(const GCodeProcessorResult::MoveVertex& move) const
{
    auto matches_percent = [](float value1, float value2, float max_percent) {
        return std::abs(value2 - value1) / value1 <= max_percent;
    };

    switch (move.type)
    {
    case EMoveType::Tool_change:
    case EMoveType::Color_change:
    case EMoveType::Pause_Print:
    case EMoveType::Custom_GCode:
    case EMoveType::Retract:
    case EMoveType::Unretract:
    case EMoveType::Seam:
    case EMoveType::Extrude: {
        // use rounding to reduce the number of generated paths
        return type == move.type && extruder_id == move.extruder_id && cp_color_id == move.cp_color_id && role == move.extrusion_role &&
            move.position.z() <= sub_paths.front().first.position.z() && feedrate == move.feedrate && fan_speed == move.fan_speed &&
            height == round_to_bin(move.height) && width == round_to_bin(move.width) &&
            matches_percent(volumetric_rate, move.volumetric_rate(), 0.05f) && layer_time == move.layer_duration;
    }
    case EMoveType::Travel: {
        return type == move.type && feedrate == move.feedrate && extruder_id == move.extruder_id && cp_color_id == move.cp_color_id;
    }
    default: { return false; }
    }
}

void GCodeViewer::TBuffer::Model::reset()
{
    instances.reset();
}

void GCodeViewer::TBuffer::reset()
{
    vertices.reset();
    for (IBuffer& buffer : indices) {
        buffer.reset();
    }

    indices.clear();
    paths.clear();
    render_paths.clear();
    model.reset();
}

void GCodeViewer::TBuffer::add_path(const GCodeProcessorResult::MoveVertex& move, unsigned int b_id, size_t i_id, size_t s_id)
{
    Path::Endpoint endpoint = { b_id, i_id, s_id, move.position };
    // use rounding to reduce the number of generated paths
    paths.push_back({ move.type, move.extrusion_role, move.delta_extruder,
        round_to_bin(move.height), round_to_bin(move.width),
        move.feedrate, move.fan_speed, move.temperature,
        move.volumetric_rate(), move.layer_duration, move.extruder_id, move.cp_color_id, { { endpoint, endpoint } } });
}

ColorRGBA GCodeViewer::Extrusions::Range::get_color_at(float value) const
{
    // Input value scaled to the colors range
    const float step = step_size();
    float _min = min;
    if(log_scale) {
        value = std::log(value);
        _min = std::log(min);
    }
    const float global_t = (step != 0.0f) ? std::max(0.0f, value - _min) / step : 0.0f; // lower limit of 0.0f

    const size_t color_max_idx = Range_Colors.size() - 1;

    // Compute the two colors just below (low) and above (high) the input value
    const size_t color_low_idx = std::clamp<size_t>(static_cast<size_t>(global_t), 0, color_max_idx);
    const size_t color_high_idx = std::clamp<size_t>(color_low_idx + 1, 0, color_max_idx);

    // Interpolate between the low and high colors to find exactly which color the input value should get
    return lerp(Range_Colors[color_low_idx], Range_Colors[color_high_idx], global_t - static_cast<float>(color_low_idx));
}

float GCodeViewer::Extrusions::Range::step_size() const {
if (log_scale)
    {
        float min_range = min;
        if (min_range == 0)
            min_range = 0.001f;
        return (std::log(max / min_range) / (static_cast<float>(Range_Colors.size()) - 1.0f));
    } else
    return (max - min) / (static_cast<float>(Range_Colors.size()) - 1.0f);
}

float GCodeViewer::Extrusions::Range::get_value_at_step(int step) const {
    if (!log_scale)
        return min + static_cast<float>(step) * step_size();
    else
    return std::exp(std::log(min) + static_cast<float>(step) * step_size());
    
}
GCodeViewer::SequentialRangeCap::~SequentialRangeCap() {
    if (ibo > 0)
        glsafe(::glDeleteBuffers(1, &ibo));
}

void GCodeViewer::SequentialRangeCap::reset() {
    if (ibo > 0)
        glsafe(::glDeleteBuffers(1, &ibo));

    buffer = nullptr;
    ibo = 0;
    vbo = 0;
    color = { 0.0f, 0.0f, 0.0f, 1.0f };
}

void GCodeViewer::SequentialView::Marker::init(std::string filename)
{
    if (filename.empty()) {
        m_model.init_from(stilized_arrow(16, 1.5f, 3.0f, 0.8f, 3.0f));
    } else {
        m_model.init_from_file(filename);
    }
    m_model.set_color({ 1.0f, 1.0f, 1.0f, 0.5f });
}

void GCodeViewer::SequentialView::Marker::set_world_position(const Vec3f& position)
{
    m_world_position = position;
    m_world_transform = (Geometry::assemble_transform((position + m_z_offset * Vec3f::UnitZ()).cast<double>()) * Geometry::assemble_transform(m_model.get_bounding_box().size().z() * Vec3d::UnitZ(), { M_PI, 0.0, 0.0 })).cast<float>();
}

void GCodeViewer::SequentialView::Marker::update_curr_move(const GCodeProcessorResult::MoveVertex move) {
    m_curr_move = move;
}

//BBS: GUI refactor: add canvas size from parameters
void GCodeViewer::SequentialView::Marker::render(int canvas_width, int canvas_height, const EViewType& view_type)
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
    const Transform3d model_matrix = m_world_transform.cast<double>();
    shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
    shader->set_uniform("view_normal_matrix", view_normal_matrix);

    m_model.render();

    shader->stop_using();

    glsafe(::glDisable(GL_BLEND));

    static float last_window_width = 0.0f;
    size_t text_line = 0;
    static size_t last_text_line = 0;
    const ImU32 text_name_clr = m_is_dark ? IM_COL32(255, 255, 255, 0.88 * 255) : IM_COL32(38, 46, 48, 255);
    const ImU32 text_value_clr = m_is_dark ? IM_COL32(255, 255, 255, 0.4 * 255) : IM_COL32(144, 144, 144, 255);

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    //BBS: GUI refactor: add canvas size from parameters
    imgui.set_next_window_pos(0.5f * static_cast<float>(canvas_width), static_cast<float>(canvas_height), ImGuiCond_Always, 0.5f, 1.0f);
    imgui.push_toolbar_style(m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0, 4.0 * m_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0 * m_scale, 6.0 * m_scale));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, text_name_clr);
    ImGui::PushStyleColor(ImGuiCol_Text, text_value_clr);
    imgui.begin(std::string("ExtruderPosition"), ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
    ImGui::AlignTextToFramePadding();
    //BBS: minus the plate offset when show tool position
    PartPlateList& partplate_list = wxGetApp().plater()->get_partplate_list();
    PartPlate* plate = partplate_list.get_curr_plate();
    const Vec3f position = m_world_position + m_world_offset;
    std::string x = ImGui::ColorMarkerStart + std::string("X: ") + ImGui::ColorMarkerEnd;
    std::string y = ImGui::ColorMarkerStart + std::string("Y: ") + ImGui::ColorMarkerEnd;
    std::string z = ImGui::ColorMarkerStart + std::string("Z: ") + ImGui::ColorMarkerEnd;
    std::string height = ImGui::ColorMarkerStart + _u8L("Height: ") + ImGui::ColorMarkerEnd;
    std::string width = ImGui::ColorMarkerStart + _u8L("Width: ") + ImGui::ColorMarkerEnd;
    std::string speed = ImGui::ColorMarkerStart + _u8L("Speed: ") + ImGui::ColorMarkerEnd;
    std::string flow = ImGui::ColorMarkerStart + _u8L("Flow: ") + ImGui::ColorMarkerEnd;
    std::string layer_time = ImGui::ColorMarkerStart + _u8L("Layer Time: ") + ImGui::ColorMarkerEnd;
    std::string fanspeed = ImGui::ColorMarkerStart + _u8L("Fan: ") + ImGui::ColorMarkerEnd;
    std::string temperature = ImGui::ColorMarkerStart + _u8L("Temperature: ") + ImGui::ColorMarkerEnd;
    const float item_size = imgui.calc_text_size(std::string_view{"X: 000.000       "}).x;
    const float item_spacing = imgui.get_item_spacing().x;
    const float window_padding = ImGui::GetStyle().WindowPadding.x;

    char buf[1024];
     if (true)
    {
        float startx2 = window_padding + item_size + item_spacing;
        float startx3 = window_padding + 2*(item_size + item_spacing);
        sprintf(buf, "%s%.3f", x.c_str(), position.x() - plate->get_origin().x());
        ImGui::PushItemWidth(item_size);
        imgui.text(buf);

        ImGui::SameLine(startx2);
        sprintf(buf, "%s%.3f", y.c_str(), position.y() - plate->get_origin().y());
        ImGui::PushItemWidth(item_size);
        imgui.text(buf);

        ImGui::SameLine(startx3);
        sprintf(buf, "%s%.3f", z.c_str(), position.z());
        ImGui::PushItemWidth(item_size);
        imgui.text(buf);

        sprintf(buf, "%s%.0f", speed.c_str(), m_curr_move.feedrate);
        ImGui::PushItemWidth(item_size);
        imgui.text(buf);

        switch (view_type) {
        case EViewType::Height: {
            ImGui::SameLine(startx2);
            sprintf(buf, "%s%.2f", height.c_str(), m_curr_move.height);
            ImGui::PushItemWidth(item_size);
            imgui.text(buf);
            break;
        }
        case EViewType::Width: {
            ImGui::SameLine(startx2);
            sprintf(buf, "%s%.2f", width.c_str(), m_curr_move.width);
            ImGui::PushItemWidth(item_size);
            imgui.text(buf);
            break;
        }
        // case EViewType::Feedrate: {
        //     ImGui::SameLine(startx2);
        //     sprintf(buf, "%s%.0f", speed.c_str(), m_curr_move.feedrate);
        //     ImGui::PushItemWidth(item_size);
        //     imgui.text(buf);
        //     break;
        // }
        case EViewType::VolumetricRate: {
            if (m_curr_move.type != EMoveType::Extrude) break;
            ImGui::SameLine(startx2);
            sprintf(buf, "%s%.2f", flow.c_str(), m_curr_move.volumetric_rate());
            ImGui::PushItemWidth(item_size);
            imgui.text(buf);
            break;
        }
        case EViewType::FanSpeed: {
            ImGui::SameLine(startx2);
            sprintf(buf, "%s%.0f", fanspeed.c_str(), m_curr_move.fan_speed);
            ImGui::PushItemWidth(item_size);
            imgui.text(buf);
            break;
        }
        case EViewType::Temperature: {
            ImGui::SameLine(startx2);
            sprintf(buf, "%s%.0f", temperature.c_str(), m_curr_move.temperature);
            ImGui::PushItemWidth(item_size);
            imgui.text(buf);
            break;
        }
        case EViewType::LayerTime:
        case EViewType::LayerTimeLog: {
            ImGui::SameLine(startx2);
            sprintf(buf, "%s%.1f", layer_time.c_str(), m_curr_move.layer_duration);
            ImGui::PushItemWidth(item_size);
            imgui.text(buf);
            break;
        }
        default:
            break;
        }
        text_line = 2;
    }
    // else {
    //     sprintf(buf, "%s%.3f", x.c_str(), position.x() - plate->get_origin().x());
    //     imgui.text(buf);

    //     ImGui::SameLine();
    //     sprintf(buf, "%s%.3f", y.c_str(), position.y() - plate->get_origin().y());
    //     imgui.text(buf);

    //     ImGui::SameLine();
    //     sprintf(buf, "%s%.3f", z.c_str(), position.z());
    //     imgui.text(buf);

    //     text_line = 1;
    // }

    // force extra frame to automatically update window size
    float window_width = ImGui::GetWindowWidth();
    if (window_width != last_window_width || text_line != last_text_line) {
        last_window_width = window_width;
        last_text_line = text_line;
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
        imgui.set_requires_extra_frame();
#else
        wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
        wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    }

    imgui.end();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    imgui.pop_toolbar_style();
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
//void GCodeViewer::SequentialView::GCodeWindow::render(float top, float bottom, uint64_t curr_line_id) const
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

    ImGuiWrapper& imgui = *wxGetApp().imgui();

    //BBS: GUI refactor: move to right
    //imgui.set_next_window_pos(0.0f, top, ImGuiCond_Always, 0.0f, 0.0f);
    imgui.set_next_window_pos(right, top + 6 * m_scale, ImGuiCond_Always, 1.0f, 0.0f); // ORCA add a small gap between legend and code viewer
    imgui.set_next_window_size(0.0f, wnd_height, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * m_scale); // ORCA add window rounding to modernize / match style
    ImGui::SetNextWindowBgAlpha(0.8f);
    imgui.begin(std::string("G-code"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);

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
void GCodeViewer::SequentialView::render(const bool has_render_path, float legend_height, int canvas_width, int canvas_height, int right_margin, const EViewType& view_type)
{
    if (has_render_path && m_show_marker) {
        marker.set_world_position(current_position);
        marker.set_world_offset(current_offset);

        marker.render(canvas_width, canvas_height, view_type);
    }

    //float bottom = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size().get_height();
    // BBS
#if 0
    if (wxGetApp().is_editor())
        bottom -= wxGetApp().plater()->get_view_toolbar().get_height();
#endif
    if (has_render_path)
        gcode_window.render(legend_height + 2, std::max(10.f, (float)canvas_height - 40), (float)canvas_width - (float)right_margin, static_cast<uint64_t>(gcode_ids[current.last]));
}

const std::vector<ColorRGBA> GCodeViewer::Extrusion_Role_Colors{ {
    { 0.90f, 0.70f, 0.70f, 1.0f },   // erNone
    { 1.00f, 0.90f, 0.30f, 1.0f },   // erPerimeter
    { 1.00f, 0.49f, 0.22f, 1.0f },   // erExternalPerimeter
    { 0.12f, 0.12f, 1.00f, 1.0f },   // erOverhangPerimeter
    { 0.69f, 0.19f, 0.16f, 1.0f },   // erInternalInfill
    { 0.59f, 0.33f, 0.80f, 1.0f },   // erSolidInfill
    { 0.94f, 0.25f, 0.25f, 1.0f },   // erTopSolidInfill
    { 0.40f, 0.36f, 0.78f, 1.0f },   // erBottomSurface
    { 1.00f, 0.55f, 0.41f, 1.0f },   // erIroning
    { 0.30f, 0.40f, 0.63f, 1.0f },   // erBridgeInfill
    { 0.30f, 0.50f, 0.73f, 1.0f },   // erInternalBridgeInfill
    { 1.00f, 1.00f, 1.00f, 1.0f },   // erGapFill
    { 0.00f, 0.53f, 0.43f, 1.0f },   // erSkirt
    { 0.00f, 0.23f, 0.43f, 1.0f },   // erBrim
    { 0.00f, 1.00f, 0.00f, 1.0f },   // erSupportMaterial
    { 0.00f, 0.50f, 0.00f, 1.0f },   // erSupportMaterialInterface
    { 0.00f, 0.25f, 0.00f, 1.0f },   // erSupportTransition
    { 0.70f, 0.89f, 0.67f, 1.0f },   // erWipeTower
    { 0.37f, 0.82f, 0.58f, 1.0f }    // erCustom
}};

const std::vector<ColorRGBA> GCodeViewer::Options_Colors{ {
    { 0.803f, 0.135f, 0.839f, 1.0f },   // Retractions
    { 0.287f, 0.679f, 0.810f, 1.0f },   // Unretractions
    { 0.900f, 0.900f, 0.900f, 1.0f },   // Seams
    { 0.758f, 0.744f, 0.389f, 1.0f },   // ToolChanges
    { 0.856f, 0.582f, 0.546f, 1.0f },   // ColorChanges
    { 0.322f, 0.942f, 0.512f, 1.0f },   // PausePrints
    { 0.886f, 0.825f, 0.262f, 1.0f }    // CustomGCodes
}};

const std::vector<ColorRGBA> GCodeViewer::Travel_Colors{ {
    { 0.219f, 0.282f, 0.609f, 1.0f }, // Move
    { 0.112f, 0.422f, 0.103f, 1.0f }, // Extrude
    { 0.505f, 0.064f, 0.028f, 1.0f }  // Retract
}};

// Normal ranges
// blue to red
const std::vector<ColorRGBA> GCodeViewer::Range_Colors{ {
    decode_color_to_float_array("#0b2c7a"),  // bluish
    decode_color_to_float_array("#135985"),
    decode_color_to_float_array("#1c8891"),
    decode_color_to_float_array("#04d60f"),
    decode_color_to_float_array("#aaf200"),
    decode_color_to_float_array("#fcf903"),
    decode_color_to_float_array("#f5ce0a"),
    //decode_color_to_float_array("#e38820"),
    decode_color_to_float_array("#d16830"),
    decode_color_to_float_array("#c2523c"),
    decode_color_to_float_array("#942616")    // reddish
}};

const ColorRGBA GCodeViewer::Wipe_Color    = ColorRGBA::YELLOW();
const ColorRGBA GCodeViewer::Neutral_Color = ColorRGBA::DARK_GRAY();

GCodeViewer::GCodeViewer()
{
    m_moves_slider  = new IMSlider(0, 0, 0, 100, wxSL_HORIZONTAL);
    m_layers_slider = new IMSlider(0, 0, 0, 100, wxSL_VERTICAL);
    m_extrusions.reset_role_visibility_flags();

//    m_sequential_view.skip_invisible_moves = true;
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

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": enter, m_buffers.size=%1%")
        %m_buffers.size();
    // initializes opengl data of TBuffers
    for (size_t i = 0; i < m_buffers.size(); ++i) {
        TBuffer& buffer = m_buffers[i];
        EMoveType type = buffer_type(i);
        switch (type)
        {
        default: { break; }
        case EMoveType::Tool_change:
        case EMoveType::Color_change:
        case EMoveType::Pause_Print:
        case EMoveType::Custom_GCode:
        case EMoveType::Retract:
        case EMoveType::Unretract:
        case EMoveType::Seam: {
//            if (wxGetApp().is_gl_version_greater_or_equal_to(3, 3)) {
//                buffer.render_primitive_type = TBuffer::ERenderPrimitiveType::InstancedModel;
//                buffer.shader = "gouraud_light_instanced";
//                buffer.model.model.init_from(diamond(16));
//                buffer.model.color = option_color(type);
//                buffer.model.instances.format = InstanceVBuffer::EFormat::InstancedModel;
//            }
//            else {
            if(type == EMoveType::Seam)
                buffer.visible = true;

                buffer.render_primitive_type = TBuffer::ERenderPrimitiveType::BatchedModel;
                buffer.vertices.format = VBuffer::EFormat::PositionNormal3;
                buffer.shader = "gouraud_light";

                buffer.model.data = diamond(16);
                buffer.model.color = option_color(type);
                buffer.model.instances.format = InstanceVBuffer::EFormat::BatchedModel;
//            }
            break;
        }
        case EMoveType::Wipe:
        case EMoveType::Extrude: {
            buffer.render_primitive_type = TBuffer::ERenderPrimitiveType::Triangle;
            buffer.vertices.format = VBuffer::EFormat::PositionNormal3;
            buffer.shader = "gouraud_light";
            break;
        }
        case EMoveType::Travel: {
            buffer.render_primitive_type = TBuffer::ERenderPrimitiveType::Line;
            buffer.vertices.format = VBuffer::EFormat::Position;
            buffer.shader = "flat";
            break;
        }
        }

        set_toolpath_move_type_visible(EMoveType::Extrude, true);
    }

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

    // Orca:
    // Default view type at first slice.
    // May be overridden in load() once we know how many tools are actually used in the G-code.
    m_nozzle_nums = preset_bundle ? preset_bundle->get_printer_extruder_count() : 1;
    auto it = std::find(view_type_items.begin(), view_type_items.end(), EViewType::FeatureType);
    m_view_type_sel = (it != view_type_items.end()) ? std::distance(view_type_items.begin(), it) : 0;
    set_view_type(EViewType::FeatureType);

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
    view_type_items.push_back(EViewType::Summary);
    view_type_items.push_back(EViewType::FeatureType);
    view_type_items.push_back(EViewType::ColorPrint);
    view_type_items.push_back(EViewType::Feedrate);
    view_type_items.push_back(EViewType::Height);
    view_type_items.push_back(EViewType::Width);
    view_type_items.push_back(EViewType::VolumetricRate);
    view_type_items.push_back(EViewType::LayerTime);
view_type_items.push_back(EViewType::LayerTimeLog);
    view_type_items.push_back(EViewType::FanSpeed);
    view_type_items.push_back(EViewType::Temperature);
    //if (mode == ConfigOptionMode::comDevelop) {
    //    view_type_items.push_back(EViewType::Tool);
    //}

    for (int i = 0; i < view_type_items.size(); i++) {
        view_type_items_str.push_back(get_view_type_string(view_type_items[i]));
    }

    // BBS for first layer inspection
    view_type_items.push_back(EViewType::FilamentId);

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
void GCodeViewer::load(const GCodeProcessorResult& gcode_result, const Print& print, const BuildVolume& build_volume,
                const std::vector<BoundingBoxf3>& exclude_bounding_box, ConfigOptionMode mode, bool only_gcode)
{
    // avoid processing if called with the same gcode_result
    if (m_last_result_id == gcode_result.id) {
        //BBS: add logs
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": the same id %1%, return directly, result %2% ") % m_last_result_id % (&gcode_result);
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

    load_toolpaths(gcode_result, build_volume, exclude_bounding_box);
    
    // ORCA: Only show filament/color print preview if more than one tool/extruder is actually used in the toolpaths.
    // Only reset back to Toolpaths (FeatureType) if we are currently in ColorPrint and this load is single-tool.
    if (m_extruder_ids.size() > 1) {
        auto it = std::find(view_type_items.begin(), view_type_items.end(), EViewType::ColorPrint);
        if (it != view_type_items.end())
            m_view_type_sel = std::distance(view_type_items.begin(), it);
        set_view_type(EViewType::ColorPrint);
    } else if (m_view_type == EViewType::ColorPrint) {
        auto it = std::find(view_type_items.begin(), view_type_items.end(), EViewType::FeatureType);
        if (it != view_type_items.end())
            m_view_type_sel = std::distance(view_type_items.begin(), it);
        set_view_type(EViewType::FeatureType);
    }

    // BBS: data for rendering color arrangement recommendation
    m_nozzle_nums = print.config().option<ConfigOptionFloats>("nozzle_diameter")->values.size();
    // Orca hack: Hide filament group for non-bbl printers
    if (!print.is_BBL_printer()) m_nozzle_nums = 1;
    std::vector<int>         filament_maps = print.get_filament_maps();
    std::vector<std::string> color_opt     = print.config().option<ConfigOptionStrings>("filament_colour")->values;
    std::vector<std::string> type_opt      = print.config().option<ConfigOptionStrings>("filament_type")->values;
    std::vector<unsigned char> support_filament_opt = print.config().option<ConfigOptionBools>("filament_is_support")->values;
    for (auto extruder_id : m_extruder_ids) {
        if (filament_maps[extruder_id] == 1) {
            m_left_extruder_filament.push_back({type_opt[extruder_id], color_opt[extruder_id], extruder_id, (bool)(support_filament_opt[extruder_id])});
        } else {
            m_right_extruder_filament.push_back({type_opt[extruder_id], color_opt[extruder_id], extruder_id, (bool)(support_filament_opt[extruder_id])});
        }
    }
    //BBS: add mutex for protection of gcode result
    if (m_layers.empty()) {
        gcode_result.unlock();
        wxGetApp().plater()->schedule_background_process();
        return;
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

    if (m_time_estimate_mode != PrintEstimatedStatistics::ETimeMode::Normal) {
        const float time = m_print_statistics.modes[static_cast<size_t>(m_time_estimate_mode)].time;
        if (time == 0.0f ||
            short_time(get_time_dhms(time)) == short_time(get_time_dhms(m_print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time)))
            m_time_estimate_mode = PrintEstimatedStatistics::ETimeMode::Normal;
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
    if (m_conflict_result) { m_conflict_result.value().layer = m_layers.get_l_at(m_conflict_result.value()._height); }

    m_gcode_check_result = gcode_result.gcode_check_result;

    filament_printable_reuslt = gcode_result.filament_printable_reuslt;
    //BBS: add mutex for protection of gcode result
    gcode_result.unlock();
    wxGetApp().plater()->schedule_background_process();
    //BBS: add logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": finished, m_buffers size %1%!")%m_buffers.size();
}

void GCodeViewer::refresh(const GCodeProcessorResult& gcode_result, const std::vector<std::string>& str_tool_colors)
{
#if ENABLE_GCODE_VIEWER_STATISTICS
    auto start_time = std::chrono::high_resolution_clock::now();
#endif // ENABLE_GCODE_VIEWER_STATISTICS

    //BBS: add mutex for protection of gcode result
    gcode_result.lock();

    //BBS: add safe check
    if (gcode_result.moves.size() == 0) {
        //result cleaned before slicing ,should return here
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": gcode result reset before, return directly!");
        gcode_result.unlock();
        return;
    }

    //BBS: add mutex for protection of gcode result
    if (m_moves_count == 0) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": gcode result m_moves_count is 0, return directly!");
        gcode_result.unlock();
        return;
    }

    wxBusyCursor busy;

    if (m_view_type == EViewType::Tool && !gcode_result.extruder_colors.empty()) {
        // update tool colors from config stored in the gcode
        decode_colors(gcode_result.extruder_colors, m_tools.m_tool_colors);
        m_tools.m_tool_visibles = std::vector<bool>(m_tools.m_tool_colors.size());
        for (auto item: m_tools.m_tool_visibles) item = true;
    }
    else {
        // update tool colors
        decode_colors(str_tool_colors, m_tools.m_tool_colors);
        m_tools.m_tool_visibles = std::vector<bool>(m_tools.m_tool_colors.size());
        for (auto item : m_tools.m_tool_visibles) item = true;
    }

    for (int i = 0; i < m_tools.m_tool_colors.size(); i++) {
        m_tools.m_tool_colors[i] = adjust_color_for_rendering(m_tools.m_tool_colors[i]);
    }
    ColorRGBA default_color;
    decode_color("#FF8000", default_color);
	// ensure there are enough colors defined
    while (m_tools.m_tool_colors.size() < std::max(size_t(1), gcode_result.filaments_count)) {
        m_tools.m_tool_colors.push_back(default_color);
        m_tools.m_tool_visibles.push_back(true);
    }

    // update ranges for coloring / legend
    m_extrusions.reset_ranges();
    for (size_t i = 0; i < m_moves_count; ++i) {
        // skip first vertex
        if (i == 0)
            continue;

        const GCodeProcessorResult::MoveVertex& curr = gcode_result.moves[i];

        switch (curr.type)
        {
        case EMoveType::Extrude:
        {
            if (curr.extrusion_role != ExtrusionRole::erCustom) {
                m_extrusions.ranges.height.update_from(round_to_bin(curr.height));
                m_extrusions.ranges.width.update_from(round_to_bin(curr.width));
            } // prevent the start code extrude extreme height/width and make the range deviate from the normal range
            m_extrusions.ranges.fan_speed.update_from(curr.fan_speed);
            m_extrusions.ranges.temperature.update_from(curr.temperature);
            if (curr.delta_extruder > 0.005 && curr.travel_dist > 0.01) {
                // Ignore very tiny extrusions from flow rate calculation, because
                // it could give very imprecise result due to rounding in gcode generation
                if (curr.extrusion_role != erCustom || is_visible(erCustom))
                    m_extrusions.ranges.volumetric_rate.update_from(round_to_bin(curr.volumetric_rate()));
            }

            if (curr.layer_duration > 0.f) {
                m_extrusions.ranges.layer_duration.update_from(curr.layer_duration);
m_extrusions.ranges.layer_duration_log.update_from(curr.layer_duration);
            }
            [[fallthrough]];
        }
        case EMoveType::Travel:
        {
            if (m_buffers[buffer_id(curr.type)].visible)
                m_extrusions.ranges.feedrate.update_from(curr.feedrate);

            break;
        }
        default: { break; }
        }
    }

#if ENABLE_GCODE_VIEWER_STATISTICS
    m_statistics.refresh_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time).count();
#endif // ENABLE_GCODE_VIEWER_STATISTICS

    //BBS: add mutex for protection of gcode result
    gcode_result.unlock();

    // update buffers' render paths
    refresh_render_paths();
    log_memory_used("Refreshed G-code extrusion paths, ");
}

void GCodeViewer::refresh_render_paths()
{
    refresh_render_paths(false, false);
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

    m_moves_count = 0;
    m_ssid_to_moveid_map.clear();
    m_ssid_to_moveid_map.shrink_to_fit();
    for (TBuffer& buffer : m_buffers) {
        buffer.reset();
    }
    m_paths_bounding_box = BoundingBoxf3();
    m_max_bounding_box = BoundingBoxf3();
    m_max_print_height = 0.0f;
    m_tools.m_tool_colors = std::vector<ColorRGBA>();
    m_tools.m_tool_visibles = std::vector<bool>();
    m_extruders_count = 0;
    m_extruder_ids = std::vector<unsigned char>();
    m_filament_diameters = std::vector<float>();
    m_filament_densities = std::vector<float>();
    m_extrusions.reset_ranges();
    //BBS: always load shell at preview
    //m_shells.volumes.clear();
    m_layers.reset();
    m_layers_z_range = { 0, 0 };
    m_roles = std::vector<ExtrusionRole>();
    m_print_statistics.reset();
    m_custom_gcode_per_print_z = std::vector<CustomGCode::Item>();
    m_sequential_view.gcode_window.reset();
    m_left_extruder_filament.clear();
    m_right_extruder_filament.clear();
#if ENABLE_GCODE_VIEWER_STATISTICS
    m_statistics.reset_all();
#endif // ENABLE_GCODE_VIEWER_STATISTICS
    m_contained_in_bed = true;
}

//BBS: GUI refactor: add canvas width and height
void GCodeViewer::render(int canvas_width, int canvas_height, int right_margin)
{
#if ENABLE_GCODE_VIEWER_STATISTICS
    m_statistics.reset_opengl();
    m_statistics.total_instances_gpu_size = 0;
#endif // ENABLE_GCODE_VIEWER_STATISTICS

    glsafe(::glEnable(GL_DEPTH_TEST));
    render_shells(canvas_width, canvas_height);

    if (m_roles.empty())
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
    m_sequential_view.m_show_marker = m_sequential_view.m_show_marker || (m_sequential_view.current.last != m_sequential_view.endpoints.last && !m_no_render_path);
    // BBS fixed buttom margin. m_moves_slider.pos_y
    m_sequential_view.render(!m_no_render_path, legend_height, canvas_width, canvas_height - bottom_margin * m_scale, right_margin * m_scale, m_view_type);
#if ENABLE_GCODE_VIEWER_STATISTICS
    render_statistics();
#endif // ENABLE_GCODE_VIEWER_STATISTICS

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

void GCodeViewer::_render_calibration_thumbnail_internal(ThumbnailData& thumbnail_data, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager)
{
    int plate_idx = thumbnail_params.plate_id;
    PartPlate* plate = partplate_list.get_plate(plate_idx);
    BoundingBoxf3 plate_box = plate->get_bounding_box(false);
    plate_box.min.z() = 0.0;
    plate_box.max.z() = 0.0;
    Vec3d center = plate_box.center();

#if 1
    Camera camera;
    camera.set_viewport(0, 0, thumbnail_data.width, thumbnail_data.height);
    camera.apply_viewport();
    camera.set_scene_box(plate_box);
    camera.set_type(Camera::EType::Ortho);
    camera.set_target(center);
    camera.select_view("top");
    camera.zoom_to_box(plate_box, 1.0f);
    camera.apply_projection(plate_box);

    auto render_as_triangles = [
#if ENABLE_GCODE_VIEWER_STATISTICS
        this
#endif // ENABLE_GCODE_VIEWER_STATISTICS
    ](TBuffer &buffer, std::vector<RenderPath>::iterator it_path, std::vector<RenderPath>::iterator it_end, GLShaderProgram& shader, int uniform_color) {
        for (auto it = it_path; it != it_end && it_path->ibuffer_id == it->ibuffer_id; ++it) {
            const RenderPath& path = *it;
            // Some OpenGL drivers crash on empty glMultiDrawElements, see GH #7415.
            assert(!path.sizes.empty());
            assert(!path.offsets.empty());
            shader.set_uniform(uniform_color, path.color);
            glsafe(::glMultiDrawElements(GL_TRIANGLES, (const GLsizei*)path.sizes.data(), GL_UNSIGNED_SHORT, (const void* const*)path.offsets.data(), (GLsizei)path.sizes.size()));
#if ENABLE_GCODE_VIEWER_STATISTICS
            ++m_statistics.gl_multi_triangles_calls_count;
#endif // ENABLE_GCODE_VIEWER_STATISTICS
        }
    };

    auto render_as_instanced_model = [
#if ENABLE_GCODE_VIEWER_STATISTICS
        this
#endif // ENABLE_GCODE_VIEWER_STATISTICS
    ](TBuffer& buffer, GLShaderProgram& shader) {
        for (auto& range : buffer.model.instances.render_ranges.ranges) {
            if (range.vbo == 0 && range.count > 0) {
                glsafe(::glGenBuffers(1, &range.vbo));
                glsafe(::glBindBuffer(GL_ARRAY_BUFFER, range.vbo));
                glsafe(::glBufferData(GL_ARRAY_BUFFER, range.count * buffer.model.instances.instance_size_bytes(), (const void*)&buffer.model.instances.buffer[range.offset * buffer.model.instances.instance_size_floats()], GL_STATIC_DRAW));
                glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
            }

            if (range.vbo > 0) {
                buffer.model.model.set_color(range.color);
                buffer.model.model.render_instanced(range.vbo, range.count);
#if ENABLE_GCODE_VIEWER_STATISTICS
                ++m_statistics.gl_instanced_models_calls_count;
                m_statistics.total_instances_gpu_size += static_cast<int64_t>(range.count * buffer.model.instances.instance_size_bytes());
#endif // ENABLE_GCODE_VIEWER_STATISTICS
            }
        }
    };

#if ENABLE_GCODE_VIEWER_STATISTICS
    auto render_as_batched_model = [this](TBuffer& buffer, GLShaderProgram& shader) {
#else
    auto render_as_batched_model = [](TBuffer& buffer, GLShaderProgram& shader, int position_id, int normal_id) {
#endif // ENABLE_GCODE_VIEWER_STATISTICS

        struct Range
        {
            unsigned int first;
            unsigned int last;
            bool intersects(const Range& other) const { return (other.last < first || other.first > last) ? false : true; }
        };
        Range buffer_range = { 0, 0 };
        size_t indices_per_instance = buffer.model.data.indices_count();

        for (size_t j = 0; j < buffer.indices.size(); ++j) {
            const IBuffer& i_buffer = buffer.indices[j];
            buffer_range.last = buffer_range.first + i_buffer.count / indices_per_instance;
            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, i_buffer.vbo));
            if (position_id != -1) {
                glsafe(::glVertexAttribPointer(position_id, buffer.vertices.position_size_floats(), GL_FLOAT, GL_FALSE, buffer.vertices.vertex_size_bytes(), (const void*)buffer.vertices.position_offset_bytes()));
                glsafe(::glEnableVertexAttribArray(position_id));
            }
            bool has_normals = buffer.vertices.normal_size_floats() > 0;
            if (has_normals) {
                if (normal_id != -1) {
                    glsafe(::glVertexAttribPointer(normal_id, buffer.vertices.normal_size_floats(), GL_FLOAT, GL_FALSE, buffer.vertices.vertex_size_bytes(), (const void*)buffer.vertices.normal_offset_bytes()));
                    glsafe(::glEnableVertexAttribArray(normal_id));
                }
            }

            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, i_buffer.ibo));

            for (auto& range : buffer.model.instances.render_ranges.ranges) {
                Range range_range = { range.offset, range.offset + range.count };
                if (range_range.intersects(buffer_range)) {
                    shader.set_uniform("uniform_color", range.color);
                    unsigned int offset = (range_range.first > buffer_range.first) ? range_range.first - buffer_range.first : 0;
                    size_t offset_bytes = static_cast<size_t>(offset) * indices_per_instance * sizeof(IBufferType);
                    Range render_range = { std::max(range_range.first, buffer_range.first), std::min(range_range.last, buffer_range.last) };
                    size_t count = static_cast<size_t>(render_range.last - render_range.first) * indices_per_instance;
                    if (count > 0) {
                        glsafe(::glDrawElements(GL_TRIANGLES, (GLsizei)count, GL_UNSIGNED_SHORT, (const void*)offset_bytes));
#if ENABLE_GCODE_VIEWER_STATISTICS
                        ++m_statistics.gl_batched_models_calls_count;
#endif // ENABLE_GCODE_VIEWER_STATISTICS
                    }
                }
            }

            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
            
            if (normal_id != -1)
                glsafe(::glDisableVertexAttribArray(normal_id));
            if (position_id != -1)
                glsafe(::glDisableVertexAttribArray(position_id));
            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));

            buffer_range.first = buffer_range.last;
        }
    };

    unsigned char begin_id = buffer_id(EMoveType::Retract);
    unsigned char end_id = buffer_id(EMoveType::Count);

    BOOST_LOG_TRIVIAL(info) << boost::format("render_calibration_thumbnail: begin_id %1%, end_id %2%")%begin_id %end_id;
    for (unsigned char i = begin_id; i < end_id; ++i) {
        TBuffer& buffer = m_buffers[i];
        if (!buffer.visible || !buffer.has_data())
            continue;

        GLShaderProgram* shader = opengl_manager.get_shader("flat");
        if (shader != nullptr) {
            shader->start_using();

            shader->set_uniform("view_model_matrix", camera.get_view_matrix());
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            int position_id = shader->get_attrib_location("v_position");
            int normal_id   = shader->get_attrib_location("v_normal");

            if (buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::InstancedModel) {
                //shader->set_uniform("emission_factor", 0.25f);
                render_as_instanced_model(buffer, *shader);
                //shader->set_uniform("emission_factor", 0.0f);
            }
            else if (buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::BatchedModel) {
                //shader->set_uniform("emission_factor", 0.25f);
                render_as_batched_model(buffer, *shader, position_id, normal_id);
                //shader->set_uniform("emission_factor", 0.0f);
            }
            else {
                int uniform_color = shader->get_uniform_location("uniform_color");
                auto it_path = buffer.render_paths.begin();
                for (unsigned int ibuffer_id = 0; ibuffer_id < static_cast<unsigned int>(buffer.indices.size()); ++ibuffer_id) {
                    const IBuffer& i_buffer = buffer.indices[ibuffer_id];
                    // Skip all paths with ibuffer_id < ibuffer_id.
                    for (; it_path != buffer.render_paths.end() && it_path->ibuffer_id < ibuffer_id; ++it_path);
                    if (it_path == buffer.render_paths.end() || it_path->ibuffer_id > ibuffer_id)
                        // Not found. This shall not happen.
                        continue;

                    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, i_buffer.vbo));
                    if (position_id != -1) {
                        glsafe(::glVertexAttribPointer(position_id, buffer.vertices.position_size_floats(), GL_FLOAT, GL_FALSE, buffer.vertices.vertex_size_bytes(), (const void*)buffer.vertices.position_offset_bytes()));
                        glsafe(::glEnableVertexAttribArray(position_id));
                    }
                    bool has_normals = false;// buffer.vertices.normal_size_floats() > 0;
                    if (has_normals) {
                        if (normal_id != -1) {
                            glsafe(::glVertexAttribPointer(normal_id, buffer.vertices.normal_size_floats(), GL_FLOAT, GL_FALSE, buffer.vertices.vertex_size_bytes(), (const void*)buffer.vertices.normal_offset_bytes()));
                            glsafe(::glEnableVertexAttribArray(normal_id));
                        }
                    }

                    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, i_buffer.ibo));

                    // Render all elements with it_path->ibuffer_id == ibuffer_id, possible with varying colors.
                    switch (buffer.render_primitive_type)
                    {
                    case TBuffer::ERenderPrimitiveType::Triangle: {
                        render_as_triangles(buffer, it_path, buffer.render_paths.end(), *shader, uniform_color);
                        break;
                    }
                    default: { break; }
                    }

                    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
                    
                    if (normal_id != -1)
                        glsafe(::glDisableVertexAttribArray(normal_id));
                    if (position_id != -1)
                        glsafe(::glDisableVertexAttribArray(position_id));

                    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
                }
            }

            shader->stop_using();
        }
        else {
            BOOST_LOG_TRIVIAL(info) << boost::format("render_calibration_thumbnail: can not find shader");
        }
    }
#endif
    BOOST_LOG_TRIVIAL(info) << boost::format("render_calibration_thumbnail: exit");

}

void GCodeViewer::_render_calibration_thumbnail_framebuffer(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager)
{
    BOOST_LOG_TRIVIAL(info) << boost::format("render_calibration_thumbnail prepare: width %1%, height %2%")%w %h;
    thumbnail_data.set(w, h);
    if (!thumbnail_data.is_valid())
        return;

    //TODO bool multisample = m_multisample_allowed;
    bool multisample = OpenGLManager::can_multisample();
    //if (!multisample)
    //    glsafe(::glEnable(GL_MULTISAMPLE));

    GLint max_samples;
    glsafe(::glGetIntegerv(GL_MAX_SAMPLES, &max_samples));
    GLsizei num_samples = max_samples / 2;

    GLuint render_fbo;
    glsafe(::glGenFramebuffers(1, &render_fbo));
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, render_fbo));
    BOOST_LOG_TRIVIAL(info) << boost::format("render_calibration_thumbnail prepare: max_samples %1%, multisample %2%, render_fbo %3%")%max_samples %multisample %render_fbo;

    GLuint render_tex = 0;
    GLuint render_tex_buffer = 0;
    if (multisample) {
        // use renderbuffer instead of texture to avoid the need to use glTexImage2DMultisample which is available only since OpenGL 3.2
        glsafe(::glGenRenderbuffers(1, &render_tex_buffer));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, render_tex_buffer));
        glsafe(::glRenderbufferStorageMultisample(GL_RENDERBUFFER, num_samples, GL_RGBA8, w, h));
        glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, render_tex_buffer));
    }
    else {
        glsafe(::glGenTextures(1, &render_tex));
        glsafe(::glBindTexture(GL_TEXTURE_2D, render_tex));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, render_tex, 0));
    }

    GLuint render_depth;
    glsafe(::glGenRenderbuffers(1, &render_depth));
    glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, render_depth));
    if (multisample)
        glsafe(::glRenderbufferStorageMultisample(GL_RENDERBUFFER, num_samples, GL_DEPTH_COMPONENT24, w, h));
    else
        glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, w, h));

    glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, render_depth));

    GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
    glsafe(::glDrawBuffers(1, drawBufs));


    if (::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        _render_calibration_thumbnail_internal(thumbnail_data, thumbnail_params, partplate_list, opengl_manager);

        if (multisample) {
            GLuint resolve_fbo;
            glsafe(::glGenFramebuffers(1, &resolve_fbo));
            glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo));

            GLuint resolve_tex;
            glsafe(::glGenTextures(1, &resolve_tex));
            glsafe(::glBindTexture(GL_TEXTURE_2D, resolve_tex));
            glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
            glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolve_tex, 0));

            glsafe(::glDrawBuffers(1, drawBufs));

            if (::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo));
                glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo));
                glsafe(::glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR));

                glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve_fbo));
                glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*)thumbnail_data.pixels.data()));
            }

            glsafe(::glDeleteTextures(1, &resolve_tex));
            glsafe(::glDeleteFramebuffers(1, &resolve_fbo));
        }
        else
            glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*)thumbnail_data.pixels.data()));
    }
#if ENABLE_CALIBRATION_THUMBNAIL_OUTPUT
     debug_calibration_output_thumbnail(thumbnail_data);
#endif

     glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
     glsafe(::glDeleteRenderbuffers(1, &render_depth));
     if (render_tex_buffer != 0)
         glsafe(::glDeleteRenderbuffers(1, &render_tex_buffer));
     if (render_tex != 0)
         glsafe(::glDeleteTextures(1, &render_tex));
     glsafe(::glDeleteFramebuffers(1, &render_fbo));

    //if (!multisample)
    //    glsafe(::glDisable(GL_MULTISAMPLE));
    BOOST_LOG_TRIVIAL(info) << boost::format("render_calibration_thumbnail prepare: exit");
}

//BBS
void GCodeViewer::render_calibration_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager)
{
    // reset values and refresh render
    int       last_view_type_sel = m_view_type_sel;
    EViewType last_view_type     = m_view_type;
    unsigned int last_role_visibility_flags = m_extrusions.role_visibility_flags;
    // set color scheme to FilamentId
    for (int i = 0; i < view_type_items.size(); i++) {
        if (view_type_items[i] == EViewType::FilamentId) {
            m_view_type_sel = i;
            break;
        }
    }
    set_view_type(EViewType::FilamentId, false);
    // set m_layers_z_range to 0, 1;
    // To be safe, we include both layers here although layer 1 seems enough
    // layer 0: custom extrusions such as flow calibration etc.
    // layer 1: the real first layer of object
    std::array<unsigned int, 2> tmp_layers_z_range = m_layers_z_range;
    m_layers_z_range = {0, 1};
    // BBS exclude feature types
    m_extrusions.role_visibility_flags = m_extrusions.role_visibility_flags & ~(1 << erSkirt);
    m_extrusions.role_visibility_flags = m_extrusions.role_visibility_flags & ~(1 << erCustom);
    // BBS include feature types
    m_extrusions.role_visibility_flags = m_extrusions.role_visibility_flags | (1 << erWipeTower);
    m_extrusions.role_visibility_flags = m_extrusions.role_visibility_flags | (1 << erPerimeter);
    m_extrusions.role_visibility_flags = m_extrusions.role_visibility_flags | (1 << erExternalPerimeter);
    m_extrusions.role_visibility_flags = m_extrusions.role_visibility_flags | (1 << erOverhangPerimeter);
    m_extrusions.role_visibility_flags = m_extrusions.role_visibility_flags | (1 << erSolidInfill);
    m_extrusions.role_visibility_flags = m_extrusions.role_visibility_flags | (1 << erTopSolidInfill);
    m_extrusions.role_visibility_flags = m_extrusions.role_visibility_flags | (1 << erInternalInfill);
    m_extrusions.role_visibility_flags = m_extrusions.role_visibility_flags | (1 << erBottomSurface);

    refresh_render_paths(false, false);

    _render_calibration_thumbnail_framebuffer(thumbnail_data, w, h, thumbnail_params, partplate_list, opengl_manager);

    // restore values and refresh render
    // reset m_layers_z_range and view type
    m_view_type_sel = last_view_type_sel;
    set_view_type(last_view_type, false);
    m_layers_z_range = tmp_layers_z_range;
    m_extrusions.role_visibility_flags = last_role_visibility_flags;
    refresh_render_paths(false, false);
}

bool GCodeViewer::can_export_toolpaths() const
{
    return has_data() && m_buffers[buffer_id(EMoveType::Extrude)].render_primitive_type == TBuffer::ERenderPrimitiveType::Triangle;
}

void GCodeViewer::update_sequential_view_current(unsigned int first, unsigned int last)
{
    auto is_visible = [this](unsigned int id) {
        for (const TBuffer &buffer : m_buffers) {
            if (buffer.visible) {
                for (const Path &path : buffer.paths) {
                    if (path.sub_paths.front().first.s_id <= id && id <= path.sub_paths.back().last.s_id) return true;
                }
            }
        }
        return false;
    };

    const int first_diff = static_cast<int>(first) - static_cast<int>(m_sequential_view.last_current.first);
    const int last_diff  = static_cast<int>(last) - static_cast<int>(m_sequential_view.last_current.last);

    unsigned int new_first = first;
    unsigned int new_last  = last;

    if (m_sequential_view.skip_invisible_moves) {
        while (!is_visible(new_first)) {
            if (first_diff > 0)
                ++new_first;
            else
                --new_first;
        }

        while (!is_visible(new_last)) {
            if (last_diff > 0)
                ++new_last;
            else
                --new_last;
        }
    }

    m_sequential_view.current.first = new_first;
    m_sequential_view.current.last  = new_last;
    m_sequential_view.last_current  = m_sequential_view.current;

    refresh_render_paths(true, true);

    if (new_first != first || new_last != last) {
        update_moves_slider();
    }
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
    const GCodeViewer::SequentialView &view = get_sequential_view();
    // this should not be needed, but it is here to try to prevent rambling crashes on Mac Asan
    if (view.endpoints.last < view.endpoints.first) return;

    std::vector<double> values(view.endpoints.last - view.endpoints.first + 1);
    std::vector<double> alternate_values(view.endpoints.last - view.endpoints.first + 1);
    unsigned int        count = 0;
    for (unsigned int i = view.endpoints.first; i <= view.endpoints.last; ++i) {
        values[count] = static_cast<double>(i + 1);
        if (view.gcode_ids[i] > 0) alternate_values[count] = static_cast<double>(view.gcode_ids[i]);
        ++count;
    }

    bool keep_min = m_moves_slider->GetActiveValue() == m_moves_slider->GetMinValue();

    m_moves_slider->SetSliderValues(values);
    m_moves_slider->SetSliderAlternateValues(alternate_values);
    m_moves_slider->SetMaxValue(view.endpoints.last - view.endpoints.first);
    m_moves_slider->SetSelectionSpan(view.current.first - view.endpoints.first, view.current.last - view.endpoints.first);
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

void GCodeViewer::update_marker_curr_move() {
    if ((int)m_last_result_id != -1) {
        auto it = std::find_if(m_gcode_result->moves.begin(), m_gcode_result->moves.end(), [this](auto move) {
                if (m_sequential_view.current.last < m_sequential_view.gcode_ids.size() && m_sequential_view.current.last >= 0) {
                    return move.gcode_id == static_cast<uint64_t>(m_sequential_view.gcode_ids[m_sequential_view.current.last]);
                }
                return false;
            });
        if (it != m_gcode_result->moves.end())
            m_sequential_view.marker.update_curr_move(*it);
    }
}

bool GCodeViewer::is_toolpath_move_type_visible(EMoveType type) const
{
    size_t id = static_cast<size_t>(buffer_id(type));
    return (id < m_buffers.size()) ? m_buffers[id].visible : false;
}

void GCodeViewer::set_toolpath_move_type_visible(EMoveType type, bool visible)
{
    size_t id = static_cast<size_t>(buffer_id(type));
    if (id < m_buffers.size())
        m_buffers[id].visible = visible;
}

unsigned int GCodeViewer::get_options_visibility_flags() const
{
    auto set_flag = [](unsigned int flags, unsigned int flag, bool active) {
        return active ? (flags | (1 << flag)) : flags;
    };

    unsigned int flags = 0;
    flags = set_flag(flags, static_cast<unsigned int>(Preview::OptionType::Travel), is_toolpath_move_type_visible(EMoveType::Travel));
    flags = set_flag(flags, static_cast<unsigned int>(Preview::OptionType::Wipe), is_toolpath_move_type_visible(EMoveType::Wipe));
    flags = set_flag(flags, static_cast<unsigned int>(Preview::OptionType::Retractions), is_toolpath_move_type_visible(EMoveType::Retract));
    flags = set_flag(flags, static_cast<unsigned int>(Preview::OptionType::Unretractions), is_toolpath_move_type_visible(EMoveType::Unretract));
    flags = set_flag(flags, static_cast<unsigned int>(Preview::OptionType::Seams), is_toolpath_move_type_visible(EMoveType::Seam));
    flags = set_flag(flags, static_cast<unsigned int>(Preview::OptionType::ToolChanges), is_toolpath_move_type_visible(EMoveType::Tool_change));
    flags = set_flag(flags, static_cast<unsigned int>(Preview::OptionType::ColorChanges), is_toolpath_move_type_visible(EMoveType::Color_change));
    flags = set_flag(flags, static_cast<unsigned int>(Preview::OptionType::PausePrints), is_toolpath_move_type_visible(EMoveType::Pause_Print));
    flags = set_flag(flags, static_cast<unsigned int>(Preview::OptionType::CustomGCodes), is_toolpath_move_type_visible(EMoveType::Custom_GCode));
    flags = set_flag(flags, static_cast<unsigned int>(Preview::OptionType::Shells), m_shells.visible);
    flags = set_flag(flags, static_cast<unsigned int>(Preview::OptionType::ToolMarker), m_sequential_view.marker.is_visible());
    flags = set_flag(flags, static_cast<unsigned int>(Preview::OptionType::Legend), is_legend_enabled());
    return flags;
}

void GCodeViewer::set_options_visibility_from_flags(unsigned int flags)
{
    auto is_flag_set = [flags](unsigned int flag) {
        return (flags & (1 << flag)) != 0;
    };

    set_toolpath_move_type_visible(EMoveType::Travel, is_flag_set(static_cast<unsigned int>(Preview::OptionType::Travel)));
    set_toolpath_move_type_visible(EMoveType::Wipe, is_flag_set(static_cast<unsigned int>(Preview::OptionType::Wipe)));
    set_toolpath_move_type_visible(EMoveType::Retract, is_flag_set(static_cast<unsigned int>(Preview::OptionType::Retractions)));
    set_toolpath_move_type_visible(EMoveType::Unretract, is_flag_set(static_cast<unsigned int>(Preview::OptionType::Unretractions)));
    set_toolpath_move_type_visible(EMoveType::Seam, is_flag_set(static_cast<unsigned int>(Preview::OptionType::Seams)));
    set_toolpath_move_type_visible(EMoveType::Tool_change, is_flag_set(static_cast<unsigned int>(Preview::OptionType::ToolChanges)));
    set_toolpath_move_type_visible(EMoveType::Color_change, is_flag_set(static_cast<unsigned int>(Preview::OptionType::ColorChanges)));
    set_toolpath_move_type_visible(EMoveType::Pause_Print, is_flag_set(static_cast<unsigned int>(Preview::OptionType::PausePrints)));
    set_toolpath_move_type_visible(EMoveType::Custom_GCode, is_flag_set(static_cast<unsigned int>(Preview::OptionType::CustomGCodes)));
    m_shells.visible = is_flag_set(static_cast<unsigned int>(Preview::OptionType::Shells));
    m_sequential_view.marker.set_visible(is_flag_set(static_cast<unsigned int>(Preview::OptionType::ToolMarker)));
    enable_legend(is_flag_set(static_cast<unsigned int>(Preview::OptionType::Legend)));
}

void GCodeViewer::set_layers_z_range(const std::array<unsigned int, 2>& layers_z_range)
{
    bool keep_sequential_current_first = layers_z_range[0] >= m_layers_z_range[0];
    bool keep_sequential_current_last = layers_z_range[1] <= m_layers_z_range[1];
    m_layers_z_range = layers_z_range;
    refresh_render_paths(keep_sequential_current_first, keep_sequential_current_last);
    update_moves_slider(true);
}

void GCodeViewer::export_toolpaths_to_obj(const char* filename) const
{
    if (filename == nullptr)
        return;

    if (!has_data())
        return;

    wxBusyCursor busy;

    // the data needed is contained into the Extrude TBuffer
    const TBuffer& t_buffer = m_buffers[buffer_id(EMoveType::Extrude)];
    if (!t_buffer.has_data())
        return;

    if (t_buffer.render_primitive_type != TBuffer::ERenderPrimitiveType::Triangle)
        return;

    // collect color information to generate materials
    std::vector<ColorRGBA> colors;
    for (const RenderPath& path : t_buffer.render_paths) {
        colors.push_back(path.color);
    }
    sort_remove_duplicates(colors);

    // save materials file
    boost::filesystem::path mat_filename(filename);
    mat_filename.replace_extension("mtl");

    CNumericLocalesSetter locales_setter;

    FILE* fp = boost::nowide::fopen(mat_filename.string().c_str(), "w");
    if (fp == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "GCodeViewer::export_toolpaths_to_obj: Couldn't open " << mat_filename.string().c_str() << " for writing";
        return;
    }

    fprintf(fp, "# G-Code Toolpaths Materials\n");
    fprintf(fp, "# Generated by %s-%s based on Slic3r\n", SLIC3R_APP_NAME, SoftFever_VERSION);

    unsigned int colors_count = 1;
    for (const ColorRGBA& color : colors) {
        fprintf(fp, "\nnewmtl material_%d\n", colors_count++);
        fprintf(fp, "Ka 1 1 1\n");
        fprintf(fp, "Kd %g %g %g\n", color.r(), color.g(), color.b());
        fprintf(fp, "Ks 0 0 0\n");
    }

    fclose(fp);

    // save geometry file
    fp = boost::nowide::fopen(filename, "w");
    if (fp == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "GCodeViewer::export_toolpaths_to_obj: Couldn't open " << filename << " for writing";
        return;
    }

    fprintf(fp, "# G-Code Toolpaths\n");
    fprintf(fp, "# Generated by %s-%s based on Slic3r\n", SLIC3R_APP_NAME, SoftFever_VERSION);
    fprintf(fp, "\nmtllib ./%s\n", mat_filename.filename().string().c_str());

    const size_t floats_per_vertex = t_buffer.vertices.vertex_size_floats();

    std::vector<Vec3f> out_vertices;
    std::vector<Vec3f> out_normals;

    struct VerticesOffset
    {
        unsigned int vbo;
        size_t offset;
    };
    std::vector<VerticesOffset> vertices_offsets;
    vertices_offsets.push_back({ t_buffer.vertices.vbos.front(), 0 });

    // get vertices/normals data from vertex buffers on gpu
    for (size_t i = 0; i < t_buffer.vertices.vbos.size(); ++i) {
        const size_t floats_count = t_buffer.vertices.sizes[i] / sizeof(float);
        VertexBuffer vertices(floats_count);
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, t_buffer.vertices.vbos[i]));
        glsafe(::glGetBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(t_buffer.vertices.sizes[i]), static_cast<void*>(vertices.data())));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
        const size_t vertices_count = floats_count / floats_per_vertex;
        for (size_t j = 0; j < vertices_count; ++j) {
            const size_t base = j * floats_per_vertex;
            out_vertices.push_back({ vertices[base + 0], vertices[base + 1], vertices[base + 2] });
            out_normals.push_back({ vertices[base + 3], vertices[base + 4], vertices[base + 5] });
        }

        if (i < t_buffer.vertices.vbos.size() - 1)
            vertices_offsets.push_back({ t_buffer.vertices.vbos[i + 1], vertices_offsets.back().offset + vertices_count });
    }

    // save vertices to file
    fprintf(fp, "\n# vertices\n");
    for (const Vec3f& v : out_vertices) {
        fprintf(fp, "v %g %g %g\n", v.x(), v.y(), v.z());
    }

    // save normals to file
    fprintf(fp, "\n# normals\n");
    for (const Vec3f& n : out_normals) {
        fprintf(fp, "vn %g %g %g\n", n.x(), n.y(), n.z());
    }

    size_t i = 0;
    for (const ColorRGBA& color : colors) {
        // save material triangles to file
        fprintf(fp, "\nusemtl material_%zu\n", i + 1);
        fprintf(fp, "# triangles material %zu\n", i + 1);

        for (const RenderPath& render_path : t_buffer.render_paths) {
            if (render_path.color != color)
                continue;

            const IBuffer& ibuffer = t_buffer.indices[render_path.ibuffer_id];
            size_t vertices_offset = 0;
            for (size_t j = 0; j < vertices_offsets.size(); ++j) {
                const VerticesOffset& offset = vertices_offsets[j];
                if (offset.vbo == ibuffer.vbo) {
                    vertices_offset = offset.offset;
                    break;
                }
            }

            // get indices data from index buffer on gpu
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibuffer.ibo));
            for (size_t j = 0; j < render_path.sizes.size(); ++j) {
                IndexBuffer indices(render_path.sizes[j]);
                glsafe(::glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>(render_path.offsets[j]),
                    static_cast<GLsizeiptr>(render_path.sizes[j] * sizeof(IBufferType)), static_cast<void*>(indices.data())));

                const size_t triangles_count = render_path.sizes[j] / 3;
                for (size_t k = 0; k < triangles_count; ++k) {
                    const size_t base = k * 3;
                    const size_t v1 = 1 + static_cast<size_t>(indices[base + 0]) + vertices_offset;
                    const size_t v2 = 1 + static_cast<size_t>(indices[base + 1]) + vertices_offset;
                    const size_t v3 = 1 + static_cast<size_t>(indices[base + 2]) + vertices_offset;
                    if (v1 != v2)
                        // do not export dummy triangles
                        fprintf(fp, "f %zu//%zu %zu//%zu %zu//%zu\n", v1, v1, v2, v2, v3, v3);
                }
            }
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
        }
        ++i;
    }

    fclose(fp);
}

void GCodeViewer::load_toolpaths(const GCodeProcessorResult& gcode_result, const BuildVolume& build_volume, const std::vector<BoundingBoxf3>& exclude_bounding_box)
{
    // max index buffer size, in bytes
    static const size_t IBUFFER_THRESHOLD_BYTES = 64 * 1024 * 1024;

    //BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(",build_volume center{%1%, %2%}, moves count %3%\n")%build_volume.bed_center().x() % build_volume.bed_center().y() %gcode_result.moves.size();
    auto log_memory_usage = [this](const std::string& label, const std::vector<MultiVertexBuffer>& vertices, const std::vector<MultiIndexBuffer>& indices) {
        int64_t vertices_size = 0;
        for (const MultiVertexBuffer& buffers : vertices) {
            for (const VertexBuffer& buffer : buffers) {
                vertices_size += SLIC3R_STDVEC_MEMSIZE(buffer, float);
            }
            //BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format("vertices count %1%\n")%buffers.size();
        }
        int64_t indices_size = 0;
        for (const MultiIndexBuffer& buffers : indices) {
            for (const IndexBuffer& buffer : buffers) {
                indices_size += SLIC3R_STDVEC_MEMSIZE(buffer, IBufferType);
            }
            //BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format("indices count %1%\n")%buffers.size();
        }
        log_memory_used(label, vertices_size + indices_size);
    };

    // format data into the buffers to be rendered as lines
    auto add_vertices_as_line = [](const GCodeProcessorResult::MoveVertex& prev, const GCodeProcessorResult::MoveVertex& curr, VertexBuffer& vertices) {
        auto add_vertex = [&vertices](const Vec3f& position) {
            // add position
            vertices.push_back(position.x());
            vertices.push_back(position.y());
            vertices.push_back(position.z());
        };
        // x component of the normal to the current segment (the normal is parallel to the XY plane)
        //BBS: Has modified a lot for this function to support arc move
        size_t loop_num = curr.is_arc_move_with_interpolation_points() ? curr.interpolation_points.size() : 0;
        for (size_t i = 0; i < loop_num + 1; i++) {
            const Vec3f &previous = (i == 0? prev.position : curr.interpolation_points[i-1]);
            const Vec3f &current = (i == loop_num? curr.position : curr.interpolation_points[i]);
            // add previous vertex
            add_vertex(previous);
            // add current vertex
            add_vertex(current);
        }
    };
    //BBS: modify a lot to support arc travel
    auto add_indices_as_line = [](const GCodeProcessorResult::MoveVertex& prev, const GCodeProcessorResult::MoveVertex& curr, TBuffer& buffer,
        size_t& vbuffer_size, unsigned int ibuffer_id, IndexBuffer& indices, size_t move_id) {

            if (buffer.paths.empty() || prev.type != curr.type || !buffer.paths.back().matches(curr)) {
                buffer.add_path(curr, ibuffer_id, indices.size(), move_id - 1);
                buffer.paths.back().sub_paths.front().first.position = prev.position;
            }

            Path& last_path = buffer.paths.back();
            size_t loop_num = curr.is_arc_move_with_interpolation_points() ? curr.interpolation_points.size() : 0;
            for (size_t i = 0; i < loop_num + 1; i++) {
                //BBS: add previous index
                indices.push_back(static_cast<IBufferType>(indices.size()));
                //BBS: add current index
                indices.push_back(static_cast<IBufferType>(indices.size()));
                vbuffer_size += buffer.max_vertices_per_segment();
            }
            last_path.sub_paths.back().last = { ibuffer_id, indices.size() - 1, move_id, curr.position };
    };

    // format data into the buffers to be rendered as solid.
    auto add_vertices_as_solid = [](const GCodeProcessorResult::MoveVertex& prev, const GCodeProcessorResult::MoveVertex& curr, TBuffer& buffer, unsigned int vbuffer_id, VertexBuffer& vertices, size_t move_id) {
        auto store_vertex = [](VertexBuffer& vertices, const Vec3f& position, const Vec3f& normal) {
            // append position
            vertices.push_back(position.x());
            vertices.push_back(position.y());
            vertices.push_back(position.z());
            // append normal
            vertices.push_back(normal.x());
            vertices.push_back(normal.y());
            vertices.push_back(normal.z());
        };

        if (buffer.paths.empty() || prev.type != curr.type || !buffer.paths.back().matches(curr)) {
            buffer.add_path(curr, vbuffer_id, vertices.size(), move_id - 1);
            buffer.paths.back().sub_paths.back().first.position = prev.position;
        }

        Path& last_path = buffer.paths.back();
        //BBS: Has modified a lot for this function to support arc move
        size_t loop_num = curr.is_arc_move_with_interpolation_points() ? curr.interpolation_points.size() : 0;
        for (size_t i = 0; i < loop_num + 1; i++) {
            const Vec3f &prev_position = (i == 0? prev.position : curr.interpolation_points[i-1]);
            const Vec3f &curr_position = (i == loop_num? curr.position : curr.interpolation_points[i]);

            const Vec3f dir = (curr_position - prev_position).normalized();
            const Vec3f right = Vec3f(dir.y(), -dir.x(), 0.0f).normalized();
            const Vec3f left = -right;
            const Vec3f up = right.cross(dir);
            const Vec3f down = -up;
            const float half_width = 0.5f * last_path.width;
            const float half_height = 0.5f * last_path.height;
            const Vec3f prev_pos = prev_position - half_height * up;
            const Vec3f curr_pos = curr_position - half_height * up;
            const Vec3f d_up = half_height * up;
            const Vec3f d_down = -half_height * up;
            const Vec3f d_right = half_width * right;
            const Vec3f d_left = -half_width * right;

            if ((last_path.vertices_count() == 1 || vertices.empty()) && i == 0) {
                store_vertex(vertices, prev_pos + d_up, up);
                store_vertex(vertices, prev_pos + d_right, right);
                store_vertex(vertices, prev_pos + d_down, down);
                store_vertex(vertices, prev_pos + d_left, left);
            } else {
                store_vertex(vertices, prev_pos + d_right, right);
                store_vertex(vertices, prev_pos + d_left, left);
            }

            store_vertex(vertices, curr_pos + d_up, up);
            store_vertex(vertices, curr_pos + d_right, right);
            store_vertex(vertices, curr_pos + d_down, down);
            store_vertex(vertices, curr_pos + d_left, left);
        }

        last_path.sub_paths.back().last = { vbuffer_id, vertices.size(), move_id, curr.position };
    };
    auto add_indices_as_solid = [&](const GCodeProcessorResult::MoveVertex& prev, const GCodeProcessorResult::MoveVertex& curr, const GCodeProcessorResult::MoveVertex* next,
        TBuffer& buffer, size_t& vbuffer_size, unsigned int ibuffer_id, IndexBuffer& indices, size_t move_id) {
            static Vec3f prev_dir;
            static Vec3f prev_up;
            static float sq_prev_length;
            auto store_triangle = [](IndexBuffer& indices, IBufferType i1, IBufferType i2, IBufferType i3) {
                indices.push_back(i1);
                indices.push_back(i2);
                indices.push_back(i3);
            };
            auto append_dummy_cap = [store_triangle](IndexBuffer& indices, IBufferType id) {
                store_triangle(indices, id, id, id);
                store_triangle(indices, id, id, id);
            };
            auto convert_vertices_offset = [](size_t vbuffer_size, const std::array<int, 8>& v_offsets) {
                std::array<IBufferType, 8> ret = {
                    static_cast<IBufferType>(static_cast<int>(vbuffer_size) + v_offsets[0]),
                    static_cast<IBufferType>(static_cast<int>(vbuffer_size) + v_offsets[1]),
                    static_cast<IBufferType>(static_cast<int>(vbuffer_size) + v_offsets[2]),
                    static_cast<IBufferType>(static_cast<int>(vbuffer_size) + v_offsets[3]),
                    static_cast<IBufferType>(static_cast<int>(vbuffer_size) + v_offsets[4]),
                    static_cast<IBufferType>(static_cast<int>(vbuffer_size) + v_offsets[5]),
                    static_cast<IBufferType>(static_cast<int>(vbuffer_size) + v_offsets[6]),
                    static_cast<IBufferType>(static_cast<int>(vbuffer_size) + v_offsets[7])
                };
                return ret;
            };
            auto append_starting_cap_triangles = [&](IndexBuffer& indices, const std::array<IBufferType, 8>& v_offsets) {
                store_triangle(indices, v_offsets[0], v_offsets[2], v_offsets[1]);
                store_triangle(indices, v_offsets[0], v_offsets[3], v_offsets[2]);
            };
            auto append_stem_triangles = [&](IndexBuffer& indices, const std::array<IBufferType, 8>& v_offsets) {
                store_triangle(indices, v_offsets[0], v_offsets[1], v_offsets[4]);
                store_triangle(indices, v_offsets[1], v_offsets[5], v_offsets[4]);
                store_triangle(indices, v_offsets[1], v_offsets[2], v_offsets[5]);
                store_triangle(indices, v_offsets[2], v_offsets[6], v_offsets[5]);
                store_triangle(indices, v_offsets[2], v_offsets[3], v_offsets[6]);
                store_triangle(indices, v_offsets[3], v_offsets[7], v_offsets[6]);
                store_triangle(indices, v_offsets[3], v_offsets[0], v_offsets[7]);
                store_triangle(indices, v_offsets[0], v_offsets[4], v_offsets[7]);
            };
            auto append_ending_cap_triangles = [&](IndexBuffer& indices, const std::array<IBufferType, 8>& v_offsets) {
                store_triangle(indices, v_offsets[4], v_offsets[6], v_offsets[7]);
                store_triangle(indices, v_offsets[4], v_offsets[5], v_offsets[6]);
            };

            if (buffer.paths.empty() || prev.type != curr.type || !buffer.paths.back().matches(curr)) {
                buffer.add_path(curr, ibuffer_id, indices.size(), move_id - 1);
                buffer.paths.back().sub_paths.back().first.position = prev.position;
            }

            Path& last_path = buffer.paths.back();
            bool is_first_segment = (last_path.vertices_count() == 1);
            //BBS: has modified a lot for this function to support arc move
            std::array<IBufferType, 8> first_seg_v_offsets = convert_vertices_offset(vbuffer_size, { 0, 1, 2, 3, 4, 5, 6, 7 });
            std::array<IBufferType, 8> non_first_seg_v_offsets = convert_vertices_offset(vbuffer_size, { -4, 0, -2, 1, 2, 3, 4, 5 });

            size_t loop_num = curr.is_arc_move_with_interpolation_points() ? curr.interpolation_points.size() : 0;
            for (size_t i = 0; i < loop_num + 1; i++) {
                const Vec3f &prev_position = (i == 0? prev.position : curr.interpolation_points[i-1]);
                const Vec3f &curr_position = (i == loop_num? curr.position : curr.interpolation_points[i]);

                const Vec3f dir = (curr_position - prev_position).normalized();
                const Vec3f right = Vec3f(dir.y(), -dir.x(), 0.0f).normalized();
                const Vec3f up = right.cross(dir);
                const float sq_length = (curr_position - prev_position).squaredNorm();

                if ((is_first_segment || vbuffer_size == 0) && i == 0) {
                    if (is_first_segment && i == 0)
                        // starting cap triangles
                        append_starting_cap_triangles(indices, first_seg_v_offsets);
                    // dummy triangles outer corner cap
                    append_dummy_cap(indices, vbuffer_size);
                    // stem triangles
                    append_stem_triangles(indices, first_seg_v_offsets);

                    vbuffer_size += 8;
                } else {
                    float displacement = 0.0f;
                    float cos_dir = prev_dir.dot(dir);
                    if (cos_dir > -0.9998477f) {
                        // if the angle between adjacent segments is smaller than 179 degrees
                        const Vec3f med_dir = (prev_dir + dir).normalized();
                        const float half_width = 0.5f * last_path.width;
                        displacement = half_width * ::tan(::acos(std::clamp(dir.dot(med_dir), -1.0f, 1.0f)));
                    }

                    float sq_displacement = sqr(displacement);
                    bool can_displace = displacement > 0.0f && sq_displacement < sq_prev_length&& sq_displacement < sq_length;

                    bool is_right_turn = prev_up.dot(prev_dir.cross(dir)) <= 0.0f;
                    // whether the angle between adjacent segments is greater than 45 degrees
                    bool is_sharp = cos_dir < 0.7071068f;

                    bool right_displaced = false;
                    bool left_displaced = false;

                    if (!is_sharp && can_displace) {
                        if (is_right_turn)
                            left_displaced = true;
                        else
                            right_displaced = true;
                    }

                    // triangles outer corner cap
                    if (is_right_turn) {
                        if (left_displaced)
                            // dummy triangles
                            append_dummy_cap(indices, vbuffer_size);
                        else {
                            store_triangle(indices, vbuffer_size - 4, vbuffer_size + 1, vbuffer_size - 1);
                            store_triangle(indices, vbuffer_size + 1, vbuffer_size - 2, vbuffer_size - 1);
                        }
                    }
                    else {
                        if (right_displaced)
                            // dummy triangles
                            append_dummy_cap(indices, vbuffer_size);
                        else {
                            store_triangle(indices, vbuffer_size - 4, vbuffer_size - 3, vbuffer_size + 0);
                            store_triangle(indices, vbuffer_size - 3, vbuffer_size - 2, vbuffer_size + 0);
                        }
                    }
                    // stem triangles
                    non_first_seg_v_offsets = convert_vertices_offset(vbuffer_size, { -4, 0, -2, 1, 2, 3, 4, 5 });
                    append_stem_triangles(indices, non_first_seg_v_offsets);
                    vbuffer_size += 6;
                }
                prev_dir = dir;
                prev_up = up;
                sq_prev_length = sq_length;
            }

            if (next != nullptr && (curr.type != next->type || !last_path.matches(*next)))
                // ending cap triangles
                append_ending_cap_triangles(indices, (is_first_segment && !curr.is_arc_move_with_interpolation_points()) ? first_seg_v_offsets : non_first_seg_v_offsets);

            last_path.sub_paths.back().last = { ibuffer_id, indices.size() - 1, move_id, curr.position };
    };

    // format data into the buffers to be rendered as instanced model
    auto add_model_instance = [](const GCodeProcessorResult::MoveVertex& curr, InstanceBuffer& instances, InstanceIdBuffer& instances_ids, size_t move_id) {
        // append position
        instances.push_back(curr.position.x());
        instances.push_back(curr.position.y());
        instances.push_back(curr.position.z());
        // append width
        instances.push_back(curr.width);
        // append height
        instances.push_back(curr.height);

        // append id
        instances_ids.push_back(move_id);
    };

    // format data into the buffers to be rendered as batched model
    auto add_vertices_as_model_batch = [](const GCodeProcessorResult::MoveVertex& curr, const GLModel::Geometry& data, VertexBuffer& vertices, InstanceBuffer& instances, InstanceIdBuffer& instances_ids, size_t move_id) {
        const double width = static_cast<double>(1.5f * curr.width);
        const double height = static_cast<double>(1.5f * curr.height);

        const Transform3d trafo = Geometry::assemble_transform((curr.position - 0.5f * curr.height * Vec3f::UnitZ()).cast<double>(), Vec3d::Zero(), { width, width, height });
        const Eigen::Matrix<double, 3, 3, Eigen::DontAlign> normal_matrix = trafo.matrix().template block<3, 3>(0, 0).inverse().transpose();

        // append vertices
        const size_t vertices_count = data.vertices_count();
        for (size_t i = 0; i < vertices_count; ++i) {
            // append position
            const Vec3d position = trafo * data.extract_position_3(i).cast<double>();
            vertices.push_back(float(position.x()));
            vertices.push_back(float(position.y()));
            vertices.push_back(float(position.z()));

            // append normal
            const Vec3d normal = normal_matrix * data.extract_normal_3(i).cast<double>();
            vertices.push_back(float(normal.x()));
            vertices.push_back(float(normal.y()));
            vertices.push_back(float(normal.z()));
        }

        // append instance position
        instances.push_back(curr.position.x());
        instances.push_back(curr.position.y());
        instances.push_back(curr.position.z());
        // append instance id
        instances_ids.push_back(move_id);
    };

    auto add_indices_as_model_batch = [](const GLModel::Geometry& data, IndexBuffer& indices, IBufferType base_index) {
        const size_t indices_count = data.indices_count();
        for (size_t i = 0; i < indices_count; ++i) {
            indices.push_back(static_cast<IBufferType>(data.extract_index(i) + base_index));
        }
    };

#if ENABLE_GCODE_VIEWER_STATISTICS
    auto start_time = std::chrono::high_resolution_clock::now();
    m_statistics.results_size = SLIC3R_STDVEC_MEMSIZE(gcode_result.moves, GCodeProcessorResult::MoveVertex);
    m_statistics.results_time = gcode_result.time;
#endif // ENABLE_GCODE_VIEWER_STATISTICS

    m_moves_count = gcode_result.moves.size();
    if (m_moves_count == 0)
        return;

    m_extruders_count = gcode_result.filaments_count;

    unsigned int progress_count = 0;
    static const unsigned int progress_threshold = 1000;
    //BBS: add only gcode mode
    ProgressDialog *          progress_dialog    = m_only_gcode_in_preview ?
        new ProgressDialog(_L("Loading G-code"), "...",
            100, wxGetApp().mainframe, wxPD_AUTO_HIDE | wxPD_APP_MODAL) : nullptr;

    wxBusyCursor busy;

    //BBS: use convex_hull for toolpath outside check
    Points pts;

    // extract approximate paths bounding box from result
    //BBS: add only gcode mode
    for (const GCodeProcessorResult::MoveVertex& move : gcode_result.moves) {
        //if (wxGetApp().is_gcode_viewer()) {
        //if (m_only_gcode_in_preview) {
            // for the gcode viewer we need to take in account all moves to correctly size the printbed
        //    m_paths_bounding_box.merge(move.position.cast<double>());
        //}
        //else {
            if (move.type == EMoveType::Extrude && move.extrusion_role != erCustom && move.width != 0.0f && move.height != 0.0f) {
                m_paths_bounding_box.merge(move.position.cast<double>());
                //BBS: use convex_hull for toolpath outside check
                pts.emplace_back(Point(scale_(move.position.x()), scale_(move.position.y())));
            }
        //}
    }

    // BBS: also merge the point on arc to bounding box
    for (const GCodeProcessorResult::MoveVertex& move : gcode_result.moves) {
        // continue if not arc path
        if (!move.is_arc_move_with_interpolation_points())
            continue;

        //if (wxGetApp().is_gcode_viewer())
        //if (m_only_gcode_in_preview)
        //    for (int i = 0; i < move.interpolation_points.size(); i++)
        //        m_paths_bounding_box.merge(move.interpolation_points[i].cast<double>());
        //else {
            if (move.type == EMoveType::Extrude && move.width != 0.0f && move.height != 0.0f)
                for (int i = 0; i < move.interpolation_points.size(); i++) {
                    m_paths_bounding_box.merge(move.interpolation_points[i].cast<double>());
                    //BBS: use convex_hull for toolpath outside check
                    pts.emplace_back(Point(scale_(move.interpolation_points[i].x()), scale_(move.interpolation_points[i].y())));
                }
        //}
    }

    // set approximate max bounding box (take in account also the tool marker)
    m_max_bounding_box = m_paths_bounding_box;
    m_max_bounding_box.merge(m_paths_bounding_box.max + m_sequential_view.marker.get_bounding_box().size().z() * Vec3d::UnitZ());

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(",m_paths_bounding_box {%1%, %2%}-{%3%, %4%}\n")
        %m_paths_bounding_box.min.x() %m_paths_bounding_box.min.y() %m_paths_bounding_box.max.x() %m_paths_bounding_box.max.y();

    //if (wxGetApp().is_editor())
    {
        //BBS: use convex_hull for toolpath outside check
        m_contained_in_bed = build_volume.all_paths_inside(gcode_result, m_paths_bounding_box);
        if (m_contained_in_bed) {
            //PartPlateList& partplate_list = wxGetApp().plater()->get_partplate_list();
            //PartPlate* plate = partplate_list.get_curr_plate();
            //const std::vector<BoundingBoxf3>& exclude_bounding_box = plate->get_exclude_areas();
            if (exclude_bounding_box.size() > 0)
            {
                int index;
                Slic3r::Polygon convex_hull_2d = Slic3r::Geometry::convex_hull(std::move(pts));
                for (index = 0; index < exclude_bounding_box.size(); index ++)
                {
                    Slic3r::Polygon p = exclude_bounding_box[index].polygon(true);  // instance convex hull is scaled, so we need to scale here
                    if (intersection({ p }, { convex_hull_2d }).empty() == false)
                    {
                        m_contained_in_bed = false;
                        break;
                    }
                }
            }
        }
        (const_cast<GCodeProcessorResult&>(gcode_result)).toolpath_outside = !m_contained_in_bed;
    }

    m_sequential_view.gcode_ids.clear();
    for (size_t i = 0; i < gcode_result.moves.size(); ++i) {
        const GCodeProcessorResult::MoveVertex& move = gcode_result.moves[i];
        if (move.type != EMoveType::Seam)
            m_sequential_view.gcode_ids.push_back(move.gcode_id);
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(",m_contained_in_bed %1%\n")%m_contained_in_bed;

    std::vector<MultiVertexBuffer> vertices(m_buffers.size());
    std::vector<MultiIndexBuffer> indices(m_buffers.size());
    std::vector<InstanceBuffer> instances(m_buffers.size());
    std::vector<InstanceIdBuffer> instances_ids(m_buffers.size());
    std::vector<InstancesOffsets> instances_offsets(m_buffers.size());
    std::vector<float> options_zs;

    size_t seams_count = 0;
    std::vector<size_t> biased_seams_ids;

    // toolpaths data -> extract vertices from result
    for (size_t i = 0; i < m_moves_count; ++i) {
        const GCodeProcessorResult::MoveVertex& curr = gcode_result.moves[i];
        if (curr.type == EMoveType::Seam) {
            ++seams_count;
            biased_seams_ids.push_back(i - biased_seams_ids.size() - 1);
        }

        size_t move_id = i - seams_count;

        // skip first vertex
        if (i == 0)
            continue;

        const GCodeProcessorResult::MoveVertex& prev = gcode_result.moves[i - 1];

        // update progress dialog
        ++progress_count;
        if (progress_dialog != nullptr && progress_count % progress_threshold == 0) {
            progress_dialog->Update(int(100.0f * float(i) / (2.0f * float(m_moves_count))),
                _L("Generating geometry vertex data") + ": " + wxNumberFormatter::ToString(100.0 * double(i) / double(m_moves_count), 0, wxNumberFormatter::Style_None) + "%");
            progress_dialog->Fit();
            progress_count = 0;
        }

        const unsigned char id = buffer_id(curr.type);
        TBuffer& t_buffer = m_buffers[id];
        MultiVertexBuffer& v_multibuffer = vertices[id];
        InstanceBuffer& inst_buffer = instances[id];
        InstanceIdBuffer& inst_id_buffer = instances_ids[id];
        InstancesOffsets& inst_offsets = instances_offsets[id];

        /*if (i%1000 == 1) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":i=%1%, buffer_id %2% render_type %3%, gcode_id %4%\n")
                %i %(int)id %(int)t_buffer.render_primitive_type %curr.gcode_id;
        }*/

        // ensure there is at least one vertex buffer
        if (v_multibuffer.empty())
            v_multibuffer.push_back(VertexBuffer());

        // if adding the vertices for the current segment exceeds the threshold size of the current vertex buffer
        // add another vertex buffer
        // BBS: get the point number and then judge whether the remaining buffer is enough
        size_t points_num = curr.is_arc_move_with_interpolation_points() ? curr.interpolation_points.size() + 1 : 1;
        size_t vertices_size_to_add = (t_buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::BatchedModel) ? t_buffer.model.data.vertices_size_bytes() : points_num * t_buffer.max_vertices_per_segment_size_bytes();
        if (v_multibuffer.back().size() * sizeof(float) > t_buffer.vertices.max_size_bytes() - vertices_size_to_add) {
            v_multibuffer.push_back(VertexBuffer());
            if (t_buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::Triangle) {
                Path& last_path = t_buffer.paths.back();
                if (prev.type == curr.type && last_path.matches(curr))
                    last_path.add_sub_path(prev, static_cast<unsigned int>(v_multibuffer.size()) - 1, 0, move_id - 1);
            }
        }

        VertexBuffer& v_buffer = v_multibuffer.back();

        switch (t_buffer.render_primitive_type)
        {
        case TBuffer::ERenderPrimitiveType::Line:     { add_vertices_as_line(prev, curr, v_buffer); break; }
        case TBuffer::ERenderPrimitiveType::Triangle: { add_vertices_as_solid(prev, curr, t_buffer, static_cast<unsigned int>(v_multibuffer.size()) - 1, v_buffer, move_id); break; }
        case TBuffer::ERenderPrimitiveType::InstancedModel:
        {
            add_model_instance(curr, inst_buffer, inst_id_buffer, move_id);
            inst_offsets.push_back(prev.position - curr.position);
#if ENABLE_GCODE_VIEWER_STATISTICS
            ++m_statistics.instances_count;
#endif // ENABLE_GCODE_VIEWER_STATISTICS
            break;
        }
        case TBuffer::ERenderPrimitiveType::BatchedModel:
        {
            add_vertices_as_model_batch(curr, t_buffer.model.data, v_buffer, inst_buffer, inst_id_buffer, move_id);
            inst_offsets.push_back(prev.position - curr.position);
#if ENABLE_GCODE_VIEWER_STATISTICS
            ++m_statistics.batched_count;
#endif // ENABLE_GCODE_VIEWER_STATISTICS
            break;
        }
        }

        // collect options zs for later use
        if (curr.type == EMoveType::Pause_Print || curr.type == EMoveType::Custom_GCode) {
            const float* const last_z = options_zs.empty() ? nullptr : &options_zs.back();
            if (last_z == nullptr || curr.position[2] < *last_z - EPSILON || *last_z + EPSILON < curr.position[2])
                options_zs.emplace_back(curr.position[2]);
        }
    }

    /*for (size_t b = 0; b < vertices.size(); ++b) {
        MultiVertexBuffer& v_multibuffer = vertices[b];
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":b=%1%, vertex buffer count %2%\n")
            %b %v_multibuffer.size();
    }*/
    auto extract_move_id = [&biased_seams_ids](size_t id) {
        size_t new_id = size_t(-1);
        auto it = std::lower_bound(biased_seams_ids.begin(), biased_seams_ids.end(), id);
        if (it == biased_seams_ids.end())
            new_id = id + biased_seams_ids.size();
        else {
            if (it == biased_seams_ids.begin() && *it < id)
                new_id = id;
            else if (it != biased_seams_ids.begin())
                new_id = id + std::distance(biased_seams_ids.begin(), it);
        }
        return (new_id == size_t(-1)) ? id : new_id;
    };
    //BBS: generate map from ssid to move id in advance to reduce computation
    m_ssid_to_moveid_map.clear();
    m_ssid_to_moveid_map.reserve( m_moves_count - biased_seams_ids.size());
    for (size_t i = 0; i < m_moves_count - biased_seams_ids.size(); i++)
        m_ssid_to_moveid_map.push_back(extract_move_id(i));

    //BBS: smooth toolpaths corners for the given TBuffer using triangles
    auto smooth_triangle_toolpaths_corners = [&gcode_result, this](const TBuffer& t_buffer, MultiVertexBuffer& v_multibuffer) {
        auto extract_position_at = [](const VertexBuffer& vertices, size_t offset) {
            return Vec3f(vertices[offset + 0], vertices[offset + 1], vertices[offset + 2]);
        };
        auto update_position_at = [](VertexBuffer& vertices, size_t offset, const Vec3f& position) {
            vertices[offset + 0] = position.x();
            vertices[offset + 1] = position.y();
            vertices[offset + 2] = position.z();
        };
        auto match_right_vertices_with_internal_point = [&](const Path::Sub_Path& prev_sub_path, const Path::Sub_Path& next_sub_path,
            size_t curr_s_id, bool is_internal_point, size_t interpolation_point_id, size_t vertex_size_floats, const Vec3f& displacement_vec) {
            if (&prev_sub_path == &next_sub_path || is_internal_point) { // previous and next segment are both contained into to the same vertex buffer
                VertexBuffer& vbuffer = v_multibuffer[prev_sub_path.first.b_id];
                // offset into the vertex buffer of the next segment 1st vertex
                size_t temp_offset = prev_sub_path.last.s_id - curr_s_id;
                for (size_t i = prev_sub_path.last.s_id; i > curr_s_id; i--) {
                    size_t move_id = m_ssid_to_moveid_map[i];
                    temp_offset += (gcode_result.moves[move_id].is_arc_move() ? gcode_result.moves[move_id].interpolation_points.size() : 0);
                }
                if (is_internal_point) {
                    size_t move_id = m_ssid_to_moveid_map[curr_s_id];
                    temp_offset += (gcode_result.moves[move_id].interpolation_points.size() - interpolation_point_id);
                }
                const size_t next_1st_offset = temp_offset * 6 * vertex_size_floats;
                // offset into the vertex buffer of the right vertex of the previous segment
                const size_t prev_right_offset = prev_sub_path.last.i_id - next_1st_offset - 3 * vertex_size_floats;
                // new position of the right vertices
                const Vec3f shared_vertex = extract_position_at(vbuffer, prev_right_offset) + displacement_vec;
                // update previous segment
                update_position_at(vbuffer, prev_right_offset, shared_vertex);
                // offset into the vertex buffer of the right vertex of the next segment
                const size_t next_right_offset = prev_sub_path.last.i_id - next_1st_offset;
                // update next segment
                update_position_at(vbuffer, next_right_offset, shared_vertex);
            }
            else { // previous and next segment are contained into different vertex buffers
                VertexBuffer& prev_vbuffer = v_multibuffer[prev_sub_path.first.b_id];
                VertexBuffer& next_vbuffer = v_multibuffer[next_sub_path.first.b_id];
                // offset into the previous vertex buffer of the right vertex of the previous segment
                const size_t prev_right_offset = prev_sub_path.last.i_id - 3 * vertex_size_floats;
                // new position of the right vertices
                const Vec3f shared_vertex = extract_position_at(prev_vbuffer, prev_right_offset) + displacement_vec;
                // update previous segment
                update_position_at(prev_vbuffer, prev_right_offset, shared_vertex);
                // offset into the next vertex buffer of the right vertex of the next segment
                const size_t next_right_offset = next_sub_path.first.i_id + 1 * vertex_size_floats;
                // update next segment
                update_position_at(next_vbuffer, next_right_offset, shared_vertex);
            }
        };
        //BBS: modify a lot of this function to support arc move
        auto match_left_vertices_with_internal_point = [&](const Path::Sub_Path& prev_sub_path, const Path::Sub_Path& next_sub_path,
            size_t curr_s_id, bool is_internal_point, size_t interpolation_point_id, size_t vertex_size_floats, const Vec3f& displacement_vec) {
            if (&prev_sub_path == &next_sub_path || is_internal_point) { // previous and next segment are both contained into to the same vertex buffer
                VertexBuffer& vbuffer = v_multibuffer[prev_sub_path.first.b_id];
                // offset into the vertex buffer of the next segment 1st vertex
                size_t temp_offset = prev_sub_path.last.s_id - curr_s_id;
                for (size_t i = prev_sub_path.last.s_id; i > curr_s_id; i--) {
                    size_t move_id = m_ssid_to_moveid_map[i];
                    temp_offset += (gcode_result.moves[move_id].is_arc_move() ? gcode_result.moves[move_id].interpolation_points.size() : 0);
                }
                if (is_internal_point) {
                    size_t move_id = m_ssid_to_moveid_map[curr_s_id];
                    temp_offset += (gcode_result.moves[move_id].interpolation_points.size() - interpolation_point_id);
                }
                const size_t next_1st_offset = temp_offset * 6 * vertex_size_floats;
                // offset into the vertex buffer of the left vertex of the previous segment
                const size_t prev_left_offset = prev_sub_path.last.i_id - next_1st_offset - 1 * vertex_size_floats;
                // new position of the left vertices
                const Vec3f shared_vertex = extract_position_at(vbuffer, prev_left_offset) + displacement_vec;
                // update previous segment
                update_position_at(vbuffer, prev_left_offset, shared_vertex);
                // offset into the vertex buffer of the left vertex of the next segment
                const size_t next_left_offset = prev_sub_path.last.i_id - next_1st_offset + 1 * vertex_size_floats;
                // update next segment
                update_position_at(vbuffer, next_left_offset, shared_vertex);
            }
            else { // previous and next segment are contained into different vertex buffers
                VertexBuffer& prev_vbuffer = v_multibuffer[prev_sub_path.first.b_id];
                VertexBuffer& next_vbuffer = v_multibuffer[next_sub_path.first.b_id];
                // offset into the previous vertex buffer of the left vertex of the previous segment
                const size_t prev_left_offset = prev_sub_path.last.i_id - 1 * vertex_size_floats;
                // new position of the left vertices
                const Vec3f shared_vertex = extract_position_at(prev_vbuffer, prev_left_offset) + displacement_vec;
                // update previous segment
                update_position_at(prev_vbuffer, prev_left_offset, shared_vertex);
                // offset into the next vertex buffer of the left vertex of the next segment
                const size_t next_left_offset = next_sub_path.first.i_id + 3 * vertex_size_floats;
                // update next segment
                update_position_at(next_vbuffer, next_left_offset, shared_vertex);
            }
        };

        size_t vertex_size_floats = t_buffer.vertices.vertex_size_floats();
        for (const Path& path : t_buffer.paths) {
            //BBS: the two segments of the path sharing the current vertex may belong
            //to two different vertex buffers
            size_t prev_sub_path_id = 0;
            size_t next_sub_path_id = 0;
            const size_t path_vertices_count = path.vertices_count();
            const float half_width = 0.5f * path.width;
            // BBS: modify a lot to support arc move which has internal points
            for (size_t j = 1; j < path_vertices_count; ++j) {
                size_t curr_s_id = path.sub_paths.front().first.s_id + j;
                size_t move_id = m_ssid_to_moveid_map[curr_s_id];
                int interpolation_points_num = gcode_result.moves[move_id].is_arc_move_with_interpolation_points()?
                                                    gcode_result.moves[move_id].interpolation_points.size() : 0;
                int loop_num = interpolation_points_num;
                //BBS: select the subpaths which contains the previous/next segments
                if (!path.sub_paths[prev_sub_path_id].contains(curr_s_id))
                    ++prev_sub_path_id;
                if (j == path_vertices_count - 1) {
                    if (!gcode_result.moves[move_id].is_arc_move_with_interpolation_points())
                        break;   // BBS: the last move has no internal point.
                    loop_num--;  //BBS: don't need to handle the endpoint of the last arc move of path
                    next_sub_path_id = prev_sub_path_id;
                } else {
                    if (!path.sub_paths[next_sub_path_id].contains(curr_s_id + 1))
                        ++next_sub_path_id;
                }
                const Path::Sub_Path& prev_sub_path = path.sub_paths[prev_sub_path_id];
                const Path::Sub_Path& next_sub_path = path.sub_paths[next_sub_path_id];

                // BBS: smooth triangle toolpaths corners including arc move which has internal interpolation point
                for (int k = 0; k <= loop_num; k++) {
                    const Vec3f& prev = k==0?
                                        gcode_result.moves[move_id - 1].position :
                                        gcode_result.moves[move_id].interpolation_points[k-1];
                    const Vec3f& curr = k==interpolation_points_num?
                                        gcode_result.moves[move_id].position :
                                        gcode_result.moves[move_id].interpolation_points[k];
                    const Vec3f& next = k < interpolation_points_num - 1?
                                        gcode_result.moves[move_id].interpolation_points[k+1]:
                                        (k == interpolation_points_num - 1? gcode_result.moves[move_id].position :
                                        (gcode_result.moves[move_id + 1].is_arc_move_with_interpolation_points()?
                                        gcode_result.moves[move_id + 1].interpolation_points[0] :
                                        gcode_result.moves[move_id + 1].position));

                    const Vec3f prev_dir = (curr - prev).normalized();
                    const Vec3f prev_right = Vec3f(prev_dir.y(), -prev_dir.x(), 0.0f).normalized();
                    const Vec3f prev_up = prev_right.cross(prev_dir);

                    const Vec3f next_dir = (next - curr).normalized();

                    const bool is_right_turn = prev_up.dot(prev_dir.cross(next_dir)) <= 0.0f;
                    const float cos_dir = prev_dir.dot(next_dir);
                    // whether the angle between adjacent segments is greater than 45 degrees
                    const bool is_sharp = cos_dir < 0.7071068f;

                    float displacement = 0.0f;
                    if (cos_dir > -0.9998477f) {
                        // if the angle between adjacent segments is smaller than 179 degrees
                        Vec3f med_dir = (prev_dir + next_dir).normalized();
                        displacement = half_width * ::tan(::acos(std::clamp(next_dir.dot(med_dir), -1.0f, 1.0f)));
                    }

                    const float sq_prev_length = (curr - prev).squaredNorm();
                    const float sq_next_length = (next - curr).squaredNorm();
                    const float sq_displacement = sqr(displacement);
                    const bool can_displace = displacement > 0.0f && sq_displacement < sq_prev_length&& sq_displacement < sq_next_length;
                    bool is_internal_point = interpolation_points_num > k;

                    if (can_displace) {
                        // displacement to apply to the vertices to match
                        Vec3f displacement_vec = displacement * prev_dir;
                        // matches inner corner vertices
                        if (is_right_turn)
                            match_right_vertices_with_internal_point(prev_sub_path, next_sub_path, curr_s_id, is_internal_point, k, vertex_size_floats, -displacement_vec);
                        else
                            match_left_vertices_with_internal_point(prev_sub_path, next_sub_path, curr_s_id, is_internal_point, k, vertex_size_floats, -displacement_vec);

                        if (!is_sharp) {
                            //BBS: matches outer corner vertices
                            if (is_right_turn)
                                match_left_vertices_with_internal_point(prev_sub_path, next_sub_path, curr_s_id, is_internal_point, k, vertex_size_floats, displacement_vec);
                            else
                                match_right_vertices_with_internal_point(prev_sub_path, next_sub_path, curr_s_id, is_internal_point, k, vertex_size_floats, displacement_vec);
                        }
                    }
                }
            }
        }
    };

#if ENABLE_GCODE_VIEWER_STATISTICS
    auto load_vertices_time = std::chrono::high_resolution_clock::now();
    m_statistics.load_vertices = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time).count();
#endif // ENABLE_GCODE_VIEWER_STATISTICS

    // smooth toolpaths corners for TBuffers using triangles
    for (size_t i = 0; i < m_buffers.size(); ++i) {
        const TBuffer& t_buffer = m_buffers[i];
        if (t_buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::Triangle) {
            smooth_triangle_toolpaths_corners(t_buffer, vertices[i]);
        }
    }

    // dismiss, no more needed
    std::vector<size_t>().swap(biased_seams_ids);

    for (MultiVertexBuffer& v_multibuffer : vertices) {
        for (VertexBuffer& v_buffer : v_multibuffer) {
            v_buffer.shrink_to_fit();
        }
    }

    // move the wipe toolpaths half height up to render them on proper position
    MultiVertexBuffer& wipe_vertices = vertices[buffer_id(EMoveType::Wipe)];
    for (VertexBuffer& v_buffer : wipe_vertices) {
        for (size_t i = 2; i < v_buffer.size(); i += 3) {
            v_buffer[i] += 0.5f * GCodeProcessor::Wipe_Height;
        }
    }

    // send vertices data to gpu, where needed
    for (size_t i = 0; i < m_buffers.size(); ++i) {
        TBuffer& t_buffer = m_buffers[i];
        if (t_buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::InstancedModel) {
            const InstanceBuffer& inst_buffer = instances[i];
            if (!inst_buffer.empty()) {
                t_buffer.model.instances.buffer = inst_buffer;
                t_buffer.model.instances.s_ids = instances_ids[i];
                t_buffer.model.instances.offsets = instances_offsets[i];
            }
        }
        else {
            if (t_buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::BatchedModel) {
                const InstanceBuffer& inst_buffer = instances[i];
                if (!inst_buffer.empty()) {
                    t_buffer.model.instances.buffer = inst_buffer;
                    t_buffer.model.instances.s_ids = instances_ids[i];
                    t_buffer.model.instances.offsets = instances_offsets[i];
                }
            }
            const MultiVertexBuffer& v_multibuffer = vertices[i];
            for (const VertexBuffer& v_buffer : v_multibuffer) {
                const size_t size_elements = v_buffer.size();
                const size_t size_bytes = size_elements * sizeof(float);
                const size_t vertices_count = size_elements / t_buffer.vertices.vertex_size_floats();
                t_buffer.vertices.count += vertices_count;

#if ENABLE_GCODE_VIEWER_STATISTICS
                m_statistics.total_vertices_gpu_size += static_cast<int64_t>(size_bytes);
                m_statistics.max_vbuffer_gpu_size = std::max(m_statistics.max_vbuffer_gpu_size, static_cast<int64_t>(size_bytes));
                ++m_statistics.vbuffers_count;
#endif // ENABLE_GCODE_VIEWER_STATISTICS

                GLuint id = 0;
                glsafe(::glGenBuffers(1, &id));
                glsafe(::glBindBuffer(GL_ARRAY_BUFFER, id));
                glsafe(::glBufferData(GL_ARRAY_BUFFER, size_bytes, v_buffer.data(), GL_STATIC_DRAW));
                glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));

                t_buffer.vertices.vbos.push_back(static_cast<unsigned int>(id));
                t_buffer.vertices.sizes.push_back(size_bytes);
            }
        }
    }

#if ENABLE_GCODE_VIEWER_STATISTICS
    auto smooth_vertices_time = std::chrono::high_resolution_clock::now();
    m_statistics.smooth_vertices = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - load_vertices_time).count();
#endif // ENABLE_GCODE_VIEWER_STATISTICS
    log_memory_usage("Loaded G-code generated vertex buffers ", vertices, indices);

    // dismiss vertices data, no more needed
    std::vector<MultiVertexBuffer>().swap(vertices);
    std::vector<InstanceBuffer>().swap(instances);
    std::vector<InstanceIdBuffer>().swap(instances_ids);

    // toolpaths data -> extract indices from result
    // paths may have been filled while extracting vertices,
    // so reset them, they will be filled again while extracting indices
    for (TBuffer& buffer : m_buffers) {
        buffer.paths.clear();
    }

    // variable used to keep track of the current vertex buffers index and size
    using CurrVertexBuffer = std::pair<unsigned int, size_t>;
    std::vector<CurrVertexBuffer> curr_vertex_buffers(m_buffers.size(), { 0, 0 });

    // variable used to keep track of the vertex buffers ids
    using VboIndexList = std::vector<unsigned int>;
    std::vector<VboIndexList> vbo_indices(m_buffers.size());

    seams_count = 0;

    for (size_t i = 0; i < m_moves_count; ++i) {
        const GCodeProcessorResult::MoveVertex& curr = gcode_result.moves[i];
        if (curr.type == EMoveType::Seam)
            ++seams_count;

        size_t move_id = i - seams_count;

        // skip first vertex
        if (i == 0)
            continue;

        const GCodeProcessorResult::MoveVertex& prev = gcode_result.moves[i - 1];
        const GCodeProcessorResult::MoveVertex* next = nullptr;
        if (i < m_moves_count - 1)
            next = &gcode_result.moves[i + 1];

        ++progress_count;
        if (progress_dialog != nullptr && progress_count % progress_threshold == 0) {
            progress_dialog->Update(int(100.0f * float(m_moves_count + i) / (2.0f * float(m_moves_count))),
                _L("Generating geometry index data") + ": " + wxNumberFormatter::ToString(100.0 * double(i) / double(m_moves_count), 0, wxNumberFormatter::Style_None) + "%");
            progress_dialog->Fit();
            progress_count = 0;
        }

        const unsigned char id = buffer_id(curr.type);
        TBuffer& t_buffer = m_buffers[id];
        MultiIndexBuffer& i_multibuffer = indices[id];
        CurrVertexBuffer& curr_vertex_buffer = curr_vertex_buffers[id];
        VboIndexList& vbo_index_list = vbo_indices[id];

        // ensure there is at least one index buffer
        if (i_multibuffer.empty()) {
            i_multibuffer.push_back(IndexBuffer());
            if (!t_buffer.vertices.vbos.empty())
                vbo_index_list.push_back(t_buffer.vertices.vbos[curr_vertex_buffer.first]);
        }

        // if adding the indices for the current segment exceeds the threshold size of the current index buffer
        // create another index buffer
        // BBS: get the point number and then judge whether the remaining buffer is enough
        size_t points_num = curr.is_arc_move_with_interpolation_points() ? curr.interpolation_points.size() + 1 : 1;
        size_t indiced_size_to_add = (t_buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::BatchedModel) ? t_buffer.model.data.indices_size_bytes() : points_num * t_buffer.max_indices_per_segment_size_bytes();
        if (i_multibuffer.back().size() * sizeof(IBufferType) >= IBUFFER_THRESHOLD_BYTES - indiced_size_to_add) {
            i_multibuffer.push_back(IndexBuffer());
            vbo_index_list.push_back(t_buffer.vertices.vbos[curr_vertex_buffer.first]);
            if (t_buffer.render_primitive_type != TBuffer::ERenderPrimitiveType::BatchedModel) {
                Path& last_path = t_buffer.paths.back();
                last_path.add_sub_path(prev, static_cast<unsigned int>(i_multibuffer.size()) - 1, 0, move_id - 1);
            }
        }

        // if adding the vertices for the current segment exceeds the threshold size of the current vertex buffer
        // create another index buffer
        // BBS: support multi points in one MoveVertice, should multiply point number
        size_t vertices_size_to_add = (t_buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::BatchedModel) ? t_buffer.model.data.vertices_size_bytes() : points_num * t_buffer.max_vertices_per_segment_size_bytes();
        if (curr_vertex_buffer.second * t_buffer.vertices.vertex_size_bytes() > t_buffer.vertices.max_size_bytes() - vertices_size_to_add) {
            i_multibuffer.push_back(IndexBuffer());

            ++curr_vertex_buffer.first;
            curr_vertex_buffer.second = 0;
            vbo_index_list.push_back(t_buffer.vertices.vbos[curr_vertex_buffer.first]);

            if (t_buffer.render_primitive_type != TBuffer::ERenderPrimitiveType::BatchedModel) {
                Path& last_path = t_buffer.paths.back();
                last_path.add_sub_path(prev, static_cast<unsigned int>(i_multibuffer.size()) - 1, 0, move_id - 1);
            }
        }

        IndexBuffer& i_buffer = i_multibuffer.back();

        switch (t_buffer.render_primitive_type)
        {
        case TBuffer::ERenderPrimitiveType::Line: {
            add_indices_as_line(prev, curr, t_buffer, curr_vertex_buffer.second, static_cast<unsigned int>(i_multibuffer.size()) - 1, i_buffer, move_id);
            break;
        }
        case TBuffer::ERenderPrimitiveType::Triangle: {
            add_indices_as_solid(prev, curr, next, t_buffer, curr_vertex_buffer.second, static_cast<unsigned int>(i_multibuffer.size()) - 1, i_buffer, move_id);
            break;
        }
        case TBuffer::ERenderPrimitiveType::BatchedModel: {
            add_indices_as_model_batch(t_buffer.model.data, i_buffer, curr_vertex_buffer.second);
            curr_vertex_buffer.second += t_buffer.model.data.vertices_count();
            break;
        }
        default: { break; }
        }
    }

    for (MultiIndexBuffer& i_multibuffer : indices) {
        for (IndexBuffer& i_buffer : i_multibuffer) {
            i_buffer.shrink_to_fit();
        }
    }

    // toolpaths data -> send indices data to gpu
    for (size_t i = 0; i < m_buffers.size(); ++i) {
        TBuffer& t_buffer = m_buffers[i];
        if (t_buffer.render_primitive_type != TBuffer::ERenderPrimitiveType::InstancedModel) {
            const MultiIndexBuffer& i_multibuffer = indices[i];
            for (const IndexBuffer& i_buffer : i_multibuffer) {
                const size_t size_elements = i_buffer.size();
                const size_t size_bytes = size_elements * sizeof(IBufferType);

                // stores index buffer informations into TBuffer
                t_buffer.indices.push_back(IBuffer());
                IBuffer& ibuf = t_buffer.indices.back();
                ibuf.count = size_elements;
                ibuf.vbo = vbo_indices[i][t_buffer.indices.size() - 1];

#if ENABLE_GCODE_VIEWER_STATISTICS
                m_statistics.total_indices_gpu_size += static_cast<int64_t>(size_bytes);
                m_statistics.max_ibuffer_gpu_size = std::max(m_statistics.max_ibuffer_gpu_size, static_cast<int64_t>(size_bytes));
                ++m_statistics.ibuffers_count;
#endif // ENABLE_GCODE_VIEWER_STATISTICS

                glsafe(::glGenBuffers(1, &ibuf.ibo));
                glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibuf.ibo));
                glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, size_bytes, i_buffer.data(), GL_STATIC_DRAW));
                glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
            }
        }
    }

    if (progress_dialog != nullptr) {
        progress_dialog->Update(100, "");
        progress_dialog->Fit();
    }

#if ENABLE_GCODE_VIEWER_STATISTICS
    for (const TBuffer& buffer : m_buffers) {
        m_statistics.paths_size += SLIC3R_STDVEC_MEMSIZE(buffer.paths, Path);
    }

    auto update_segments_count = [&](EMoveType type, int64_t& count) {
        unsigned int id = buffer_id(type);
        const MultiIndexBuffer& buffers = indices[id];
        int64_t indices_count = 0;
        for (const IndexBuffer& buffer : buffers) {
            indices_count += buffer.size();
        }
        const TBuffer& t_buffer = m_buffers[id];
        if (t_buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::Triangle)
            indices_count -= static_cast<int64_t>(12 * t_buffer.paths.size()); // remove the starting + ending caps = 4 triangles

        count += indices_count / t_buffer.indices_per_segment();
    };

    update_segments_count(EMoveType::Travel, m_statistics.travel_segments_count);
    update_segments_count(EMoveType::Wipe, m_statistics.wipe_segments_count);
    update_segments_count(EMoveType::Extrude, m_statistics.extrude_segments_count);

    m_statistics.load_indices = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - smooth_vertices_time).count();
#endif // ENABLE_GCODE_VIEWER_STATISTICS

    log_memory_usage("Loaded G-code generated indices buffers ", vertices, indices);

    // dismiss indices data, no more needed
    std::vector<MultiIndexBuffer>().swap(indices);

    // layers zs / roles / extruder ids -> extract from result
    size_t last_travel_s_id = 0;
    seams_count = 0;
    m_extruder_ids.clear();
    for (size_t i = 0; i < m_moves_count; ++i) {
        const GCodeProcessorResult::MoveVertex& move = gcode_result.moves[i];
        if (move.type == EMoveType::Seam)
            ++seams_count;

        size_t move_id = i - seams_count;

        if (move.type == EMoveType::Extrude) {
            // layers zs
            const double* const last_z = m_layers.empty() ? nullptr : &m_layers.get_zs().back();
            const double z = static_cast<double>(move.position.z());
            if (last_z == nullptr || z < *last_z - EPSILON || *last_z + EPSILON < z)
                m_layers.append(z, { last_travel_s_id, move_id });
            else
                m_layers.get_endpoints().back().last = move_id;
            // extruder ids
            m_extruder_ids.emplace_back(move.extruder_id);
            // roles
            if (i > 0)
                m_roles.emplace_back(move.extrusion_role);
        }
        else if (move.type == EMoveType::Travel) {
            if (move_id - last_travel_s_id > 1 && !m_layers.empty())
                m_layers.get_endpoints().back().last = move_id;

            last_travel_s_id = move_id;
        }
    }

    // roles -> remove duplicates
    sort_remove_duplicates(m_roles);
    m_roles.shrink_to_fit();

    // extruder ids -> remove duplicates
    sort_remove_duplicates(m_extruder_ids);
    m_extruder_ids.shrink_to_fit();

    std::vector<int> plater_extruder;
	for (auto mid : m_extruder_ids){
        int eid = mid;
        plater_extruder.push_back(++eid);
	}
    m_plater_extruder = plater_extruder;

    // replace layers for spiral vase mode
    if (!gcode_result.spiral_vase_layers.empty()) {
        m_layers.reset();
        for (const auto& layer : gcode_result.spiral_vase_layers) {
            m_layers.append(layer.first, { layer.second.first, layer.second.second });
        }
    }

    // set layers z range
    if (!m_layers.empty())
        m_layers_z_range = { 0, static_cast<unsigned int>(m_layers.size() - 1) };

    // change color of paths whose layer contains option points
    if (!options_zs.empty()) {
        TBuffer& extrude_buffer = m_buffers[buffer_id(EMoveType::Extrude)];
        for (Path& path : extrude_buffer.paths) {
            const float z = path.sub_paths.front().first.position.z();
            if (std::find_if(options_zs.begin(), options_zs.end(), [z](float f) { return f - EPSILON <= z && z <= f + EPSILON; }) != options_zs.end())
                path.cp_color_id = 255 - path.cp_color_id;
        }
    }

#if ENABLE_GCODE_VIEWER_STATISTICS
    m_statistics.load_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time).count();
#endif // ENABLE_GCODE_VIEWER_STATISTICS

    if (progress_dialog != nullptr)
        progress_dialog->Destroy();
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

void GCodeViewer::refresh_render_paths(bool keep_sequential_current_first, bool keep_sequential_current_last) const
{
#if ENABLE_GCODE_VIEWER_STATISTICS
    auto start_time = std::chrono::high_resolution_clock::now();
#endif // ENABLE_GCODE_VIEWER_STATISTICS

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": enter, m_buffers size %1%!")%m_buffers.size();
    auto extrusion_color = [this](const Path& path) {
        ColorRGBA color;
        switch (m_view_type)
        {
        case EViewType::FeatureType:    { color = Extrusion_Role_Colors[static_cast<unsigned int>(path.role)]; break; }
        case EViewType::Height:         { color = m_extrusions.ranges.height.get_color_at(path.height); break; }
        case EViewType::Width:          { color = m_extrusions.ranges.width.get_color_at(path.width); break; }
        case EViewType::Feedrate:       { color = m_extrusions.ranges.feedrate.get_color_at(path.feedrate); break; }
        case EViewType::FanSpeed:       { color = m_extrusions.ranges.fan_speed.get_color_at(path.fan_speed); break; }
        case EViewType::Temperature:    { color = m_extrusions.ranges.temperature.get_color_at(path.temperature); break; }
        case EViewType::LayerTime:      { color = m_extrusions.ranges.layer_duration.get_color_at(path.layer_time); break; }
        case EViewType::LayerTimeLog:   { color = m_extrusions.ranges.layer_duration_log.get_color_at(path.layer_time); break; }
        case EViewType::VolumetricRate: { color = m_extrusions.ranges.volumetric_rate.get_color_at(path.volumetric_rate); break; }
        case EViewType::Tool:           { color = m_tools.m_tool_colors[path.extruder_id]; break; }
        case EViewType::Summary:
        case EViewType::ColorPrint:     {
            if (path.cp_color_id >= static_cast<unsigned char>(m_tools.m_tool_colors.size()))
                color = ColorRGBA::GRAY();
            else {
                color = m_tools.m_tool_colors[path.cp_color_id];
                color = adjust_color_for_rendering(color);
            }
            break;
        }
        case EViewType::FilamentId: {
            float id = float(path.extruder_id)/256;
            float role = float(path.role) / 256;
            color      = {id, role, id, 1.0f};
            break;
        }
        default: { color = ColorRGBA::WHITE(); break; }
        }

        return color;
    };

    auto travel_color = [](const Path& path) {
        return (path.delta_extruder < 0.0f) ? Travel_Colors[2] /* Retract */ :
            ((path.delta_extruder > 0.0f) ? Travel_Colors[1] /* Extrude */ :
                Travel_Colors[0] /* Move */);
    };

    auto is_in_layers_range = [this](const Path& path, size_t min_id, size_t max_id) {
        auto in_layers_range = [this, min_id, max_id](size_t id) {
            return m_layers.get_endpoints_at(min_id).first <= id && id <= m_layers.get_endpoints_at(max_id).last;
        };

        return in_layers_range(path.sub_paths.front().first.s_id) && in_layers_range(path.sub_paths.back().last.s_id);
    };

    //BBS
    auto is_extruder_in_layer_range = [this](const Path& path, size_t extruder_id) {
        return path.extruder_id == extruder_id;
    };


    auto is_travel_in_layers_range = [this](size_t path_id, size_t min_id, size_t max_id) {
        const TBuffer& buffer = m_buffers[buffer_id(EMoveType::Travel)];
        if (path_id >= buffer.paths.size())
            return false;

        Path path = buffer.paths[path_id];
        size_t first = path_id;
        size_t last = path_id;

        // check adjacent paths
        while (first > 0 && path.sub_paths.front().first.position.isApprox(buffer.paths[first - 1].sub_paths.back().last.position)) {
            --first;
            path.sub_paths.front().first = buffer.paths[first].sub_paths.front().first;
        }
        while (last < buffer.paths.size() - 1 && path.sub_paths.back().last.position.isApprox(buffer.paths[last + 1].sub_paths.front().first.position)) {
            ++last;
            path.sub_paths.back().last = buffer.paths[last].sub_paths.back().last;
        }

        const size_t min_s_id = m_layers.get_endpoints_at(min_id).first;
        const size_t max_s_id = m_layers.get_endpoints_at(max_id).last;

        return (min_s_id <= path.sub_paths.front().first.s_id && path.sub_paths.front().first.s_id <= max_s_id) ||
            (min_s_id <= path.sub_paths.back().last.s_id && path.sub_paths.back().last.s_id <= max_s_id);
    };

#if ENABLE_GCODE_VIEWER_STATISTICS
    Statistics* statistics = const_cast<Statistics*>(&m_statistics);
    statistics->render_paths_size = 0;
    statistics->models_instances_size = 0;
#endif // ENABLE_GCODE_VIEWER_STATISTICS

    const bool top_layer_only = true;

    //BBS
    SequentialView::Endpoints global_endpoints = { m_sequential_view.gcode_ids.size() , 0 };
    SequentialView::Endpoints top_layer_endpoints = global_endpoints;
    SequentialView* sequential_view = const_cast<SequentialView*>(&m_sequential_view);
    if (top_layer_only || !keep_sequential_current_first) sequential_view->current.first = 0;
    //BBS
    if (!keep_sequential_current_last) sequential_view->current.last = m_sequential_view.gcode_ids.size();

    // first pass: collect visible paths and update sequential view data
    std::vector<std::tuple<unsigned char, unsigned int, unsigned int, unsigned int>> paths;

    for (size_t b = 0; b < m_buffers.size(); ++b) {
        TBuffer& buffer = const_cast<TBuffer&>(m_buffers[b]);
        // reset render paths
        buffer.render_paths.clear();

        if (!buffer.visible)
            continue;

        if (buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::InstancedModel ||
            buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::BatchedModel) {
            for (size_t id : buffer.model.instances.s_ids) {
                if (id < m_layers.get_endpoints_at(m_layers_z_range[0]).first || m_layers.get_endpoints_at(m_layers_z_range[1]).last < id)
                    continue;

                global_endpoints.first = std::min(global_endpoints.first, id);
                global_endpoints.last = std::max(global_endpoints.last, id);

                if (top_layer_only) {
                    if (id < m_layers.get_endpoints_at(m_layers_z_range[1]).first || m_layers.get_endpoints_at(m_layers_z_range[1]).last < id)
                        continue;

                    top_layer_endpoints.first = std::min(top_layer_endpoints.first, id);
                    top_layer_endpoints.last = std::max(top_layer_endpoints.last, id);
                }
            }
        }
        else {
            for (size_t i = 0; i < buffer.paths.size(); ++i) {
                const Path& path = buffer.paths[i];
                if (path.type == EMoveType::Travel) {
                    if (!is_travel_in_layers_range(i, m_layers_z_range[0], m_layers_z_range[1]))
                        continue;
                }
                else if (!is_in_layers_range(path, m_layers_z_range[0], m_layers_z_range[1]))
                    continue;

                if (top_layer_only) {
                    if (path.type == EMoveType::Travel) {
                        if (is_travel_in_layers_range(i, m_layers_z_range[1], m_layers_z_range[1])) {
                            top_layer_endpoints.first = std::min(top_layer_endpoints.first, path.sub_paths.front().first.s_id);
                            top_layer_endpoints.last = std::max(top_layer_endpoints.last, path.sub_paths.back().last.s_id);
                        }
                    }
                    else if (is_in_layers_range(path, m_layers_z_range[1], m_layers_z_range[1])) {
                        top_layer_endpoints.first = std::min(top_layer_endpoints.first, path.sub_paths.front().first.s_id);
                        top_layer_endpoints.last = std::max(top_layer_endpoints.last, path.sub_paths.back().last.s_id);
                    }
                }

                if (path.type == EMoveType::Extrude && !is_visible(path))
                    continue;

                if (m_view_type == EViewType::ColorPrint && !m_tools.m_tool_visibles[path.extruder_id])
                    continue;

                // store valid path
                for (size_t j = 0; j < path.sub_paths.size(); ++j) {
                    paths.push_back({ static_cast<unsigned char>(b), path.sub_paths[j].first.b_id, static_cast<unsigned int>(i), static_cast<unsigned int>(j) });
                }

                global_endpoints.first = std::min(global_endpoints.first, path.sub_paths.front().first.s_id);
                global_endpoints.last = std::max(global_endpoints.last, path.sub_paths.back().last.s_id);
            }
        }
    }

    // update current sequential position
    sequential_view->current.first = !top_layer_only && keep_sequential_current_first ? std::clamp(sequential_view->current.first, global_endpoints.first, global_endpoints.last) : global_endpoints.first;
    if (global_endpoints.last == 0) {
m_no_render_path = true;
        sequential_view->current.last = global_endpoints.last;
    } else {
m_no_render_path = false;
        sequential_view->current.last = keep_sequential_current_last ? std::clamp(sequential_view->current.last, global_endpoints.first, global_endpoints.last) : global_endpoints.last;
    }

    // get the world position from the vertex buffer
    bool found = false;
    for (const TBuffer& buffer : m_buffers) {
        if (buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::InstancedModel ||
            buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::BatchedModel) {
            for (size_t i = 0; i < buffer.model.instances.s_ids.size(); ++i) {
                if (buffer.model.instances.s_ids[i] == m_sequential_view.current.last) {
                    size_t offset = i * buffer.model.instances.instance_size_floats();
                    sequential_view->current_position.x() = buffer.model.instances.buffer[offset + 0];
                    sequential_view->current_position.y() = buffer.model.instances.buffer[offset + 1];
                    sequential_view->current_position.z() = buffer.model.instances.buffer[offset + 2];
                    sequential_view->current_offset = buffer.model.instances.offsets[i];
                    found = true;
                    break;
                }
            }
        }
        else {
            // searches the path containing the current position
            for (const Path& path : buffer.paths) {
                if (path.contains(m_sequential_view.current.last)) {
                    const int sub_path_id = path.get_id_of_sub_path_containing(m_sequential_view.current.last);
                    if (sub_path_id != -1) {
                        const Path::Sub_Path& sub_path = path.sub_paths[sub_path_id];
                        unsigned int offset = static_cast<unsigned int>(m_sequential_view.current.last - sub_path.first.s_id);
                        if (offset > 0) {
                            if (buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::Line) {
                                for (size_t i = sub_path.first.s_id + 1; i < m_sequential_view.current.last + 1; i++) {
                                    size_t move_id = m_ssid_to_moveid_map[i];
                                    const GCodeProcessorResult::MoveVertex& curr = m_gcode_result->moves[move_id];
                                    if (curr.is_arc_move()) {
                                        offset += curr.interpolation_points.size();
                                    }
                                }
                                offset = 2 * offset - 1;
                            }
                            else if (buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::Triangle) {
                                unsigned int indices_count = buffer.indices_per_segment();
                                // BBS: modify to support moves which has internal point
                                for (size_t i = sub_path.first.s_id + 1; i < m_sequential_view.current.last + 1; i++) {
                                    size_t move_id = m_ssid_to_moveid_map[i];
                                    const GCodeProcessorResult::MoveVertex& curr = m_gcode_result->moves[move_id];
                                    if (curr.is_arc_move()) {
                                        offset += curr.interpolation_points.size();
                                    }
                                }
                                offset = indices_count * (offset - 1) + (indices_count - 2);
                                if (sub_path_id == 0)
                                    offset += 6; // add 2 triangles for starting cap
                            }
                        }
                        offset += static_cast<unsigned int>(sub_path.first.i_id);

                        // gets the vertex index from the index buffer on gpu
                        if (sub_path.first.b_id >= 0 && sub_path.first.b_id < buffer.indices.size()) {
                            const IBuffer &i_buffer = buffer.indices[sub_path.first.b_id];
                            unsigned int   index    = 0;
                            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, i_buffer.ibo));
                            glsafe(::glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>(offset * sizeof(IBufferType)),
                                                        static_cast<GLsizeiptr>(sizeof(IBufferType)), static_cast<void *>(&index)));
                            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));

                            // gets the position from the vertices buffer on gpu
                            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, i_buffer.vbo));
                            glsafe(::glGetBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(index * buffer.vertices.vertex_size_bytes()),
                                                        static_cast<GLsizeiptr>(3 * sizeof(float)), static_cast<void *>(sequential_view->current_position.data())));
                            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
                        }
                        sequential_view->current_offset = Vec3f::Zero();
                        found = true;
                        break;
                    }
                }
            }
        }

        if (found)
            break;
    }

    // second pass: filter paths by sequential data and collect them by color
    RenderPath* render_path = nullptr;
    for (const auto& [tbuffer_id, ibuffer_id, path_id, sub_path_id] : paths) {
        TBuffer& buffer = const_cast<TBuffer&>(m_buffers[tbuffer_id]);
        const Path& path = buffer.paths[path_id];
        const Path::Sub_Path& sub_path = path.sub_paths[sub_path_id];
        if (m_sequential_view.current.last < sub_path.first.s_id || sub_path.last.s_id < m_sequential_view.current.first)
            continue;

        ColorRGBA color;
        switch (path.type)
        {
        case EMoveType::Tool_change:
        case EMoveType::Color_change:
        case EMoveType::Pause_Print:
        case EMoveType::Custom_GCode:
        case EMoveType::Retract:
        case EMoveType::Unretract:
        case EMoveType::Seam: { color = option_color(path.type); break; }
        case EMoveType::Extrude: {
            if (!top_layer_only ||
                m_sequential_view.current.last == global_endpoints.last ||
                is_in_layers_range(path, m_layers_z_range[1], m_layers_z_range[1]))
                color = extrusion_color(path);
            else
                color = Neutral_Color;

            break;
        }
        case EMoveType::Travel: {
            if (!top_layer_only || m_sequential_view.current.last == global_endpoints.last || is_travel_in_layers_range(path_id, m_layers_z_range[1], m_layers_z_range[1]))
                color = (m_view_type == EViewType::Feedrate || m_view_type == EViewType::Tool) ? extrusion_color(path) : travel_color(path);
            else
                color = Neutral_Color;

            break;
        }
        case EMoveType::Wipe: { color = Wipe_Color; break; }
        default: { color = { 0.0f, 0.0f, 0.0f, 1.0f }; break; }
        }

        RenderPath key{ tbuffer_id, color, static_cast<unsigned int>(ibuffer_id), path_id };
        if (render_path == nullptr || !RenderPathPropertyEqual()(*render_path, key)) {
            buffer.render_paths.emplace_back(key);
            render_path = const_cast<RenderPath*>(&buffer.render_paths.back());
        }

        unsigned int delta_1st = 0;
        if (sub_path.first.s_id < m_sequential_view.current.first && m_sequential_view.current.first <= sub_path.last.s_id)
            delta_1st = static_cast<unsigned int>(m_sequential_view.current.first - sub_path.first.s_id);

        unsigned int size_in_indices = 0;
        switch (buffer.render_primitive_type)
        {
        case TBuffer::ERenderPrimitiveType::Line:
        case TBuffer::ERenderPrimitiveType::Triangle: {
            // BBS: modify to support moves which has internal point
            size_t max_s_id = std::min(m_sequential_view.current.last, sub_path.last.s_id);
            size_t min_s_id = std::max(m_sequential_view.current.first, sub_path.first.s_id);
            unsigned int segments_count = max_s_id - min_s_id;
            for (size_t i = min_s_id + 1; i < max_s_id + 1; i++) {
                size_t move_id = m_ssid_to_moveid_map[i];
                const GCodeProcessorResult::MoveVertex& curr = m_gcode_result->moves[move_id];
                if (curr.is_arc_move()) {
                    segments_count += curr.interpolation_points.size();
                }
            }
            size_in_indices = buffer.indices_per_segment() * segments_count;
            break;
        }
        default: { break; }
        }

        if (size_in_indices == 0)
            continue;

        if (buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::Triangle) {
            if (sub_path_id == 0 && delta_1st == 0)
                size_in_indices += 6; // add 2 triangles for starting cap
            if (sub_path_id == path.sub_paths.size() - 1 && path.sub_paths.back().last.s_id <= m_sequential_view.current.last)
                size_in_indices += 6; // add 2 triangles for ending cap
            if (delta_1st > 0)
                size_in_indices -= 6; // remove 2 triangles for corner cap
        }

        render_path->sizes.push_back(size_in_indices);

        if (buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::Triangle) {
            delta_1st *= buffer.indices_per_segment();
            if (delta_1st > 0) {
                delta_1st += 6; // skip 2 triangles for corner cap
                if (sub_path_id == 0)
                    delta_1st += 6; // skip 2 triangles for starting cap
            }
        }

        render_path->offsets.push_back(static_cast<size_t>((sub_path.first.i_id + delta_1st) * sizeof(IBufferType)));

#if 0
        // check sizes and offsets against index buffer size on gpu
        GLint buffer_size;
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer->indices[render_path->ibuffer_id].ibo));
        glsafe(::glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &buffer_size));
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
        if (render_path->offsets.back() + render_path->sizes.back() * sizeof(IBufferType) > buffer_size)
            BOOST_LOG_TRIVIAL(error) << "GCodeViewer::refresh_render_paths: Invalid render path data";
#endif
    }

    // Removes empty render paths and sort.
    for (size_t b = 0; b < m_buffers.size(); ++b) {
        TBuffer* buffer = const_cast<TBuffer*>(&m_buffers[b]);
        buffer->render_paths.erase(std::remove_if(buffer->render_paths.begin(), buffer->render_paths.end(),
            [](const auto &path){ return path.sizes.empty() || path.offsets.empty(); }),
            buffer->render_paths.end());
    }

    // second pass: for buffers using instanced and batched models, update the instances render ranges
    for (size_t b = 0; b < m_buffers.size(); ++b) {
        TBuffer& buffer = const_cast<TBuffer&>(m_buffers[b]);
        if (buffer.render_primitive_type != TBuffer::ERenderPrimitiveType::InstancedModel &&
            buffer.render_primitive_type != TBuffer::ERenderPrimitiveType::BatchedModel)
            continue;

        buffer.model.instances.render_ranges.reset();

        if (!buffer.visible || buffer.model.instances.s_ids.empty())
            continue;

        buffer.model.instances.render_ranges.ranges.push_back({ 0, 0, 0, buffer.model.color });
        bool has_second_range = top_layer_only && m_sequential_view.current.last != m_sequential_view.global.last;
        if (has_second_range)
            buffer.model.instances.render_ranges.ranges.push_back({ 0, 0, 0, Neutral_Color });

        if (m_sequential_view.current.first <= buffer.model.instances.s_ids.back() && buffer.model.instances.s_ids.front() <= m_sequential_view.current.last) {
            for (size_t id : buffer.model.instances.s_ids) {
                if (has_second_range) {
                    if (id < m_sequential_view.endpoints.first) {
                        ++buffer.model.instances.render_ranges.ranges.front().offset;
                        if (id <= m_sequential_view.current.first)
                            ++buffer.model.instances.render_ranges.ranges.back().offset;
                        else
                            ++buffer.model.instances.render_ranges.ranges.back().count;
                    }
                    else if (id <= m_sequential_view.current.last)
                        ++buffer.model.instances.render_ranges.ranges.front().count;
                    else
                        break;
                }
                else {
                    if (id <= m_sequential_view.current.first)
                        ++buffer.model.instances.render_ranges.ranges.front().offset;
                    else if (id <= m_sequential_view.current.last)
                        ++buffer.model.instances.render_ranges.ranges.front().count;
                    else
                        break;
                }
            }
        }
    }

    // set sequential data to their final value
    sequential_view->endpoints = top_layer_only ? top_layer_endpoints : global_endpoints;
    sequential_view->current.first = !top_layer_only && keep_sequential_current_first ? std::clamp(sequential_view->current.first, sequential_view->endpoints.first, sequential_view->endpoints.last) : sequential_view->endpoints.first;
    sequential_view->global = global_endpoints;

    // updates sequential range caps
    std::array<SequentialRangeCap, 2>* sequential_range_caps = const_cast<std::array<SequentialRangeCap, 2>*>(&m_sequential_range_caps);
    (*sequential_range_caps)[0].reset();
    (*sequential_range_caps)[1].reset();

    if (m_sequential_view.current.first != m_sequential_view.current.last) {
        for (const auto& [tbuffer_id, ibuffer_id, path_id, sub_path_id] : paths) {
            TBuffer& buffer = const_cast<TBuffer&>(m_buffers[tbuffer_id]);
            if (buffer.render_primitive_type != TBuffer::ERenderPrimitiveType::Triangle)
                continue;

            const Path& path = buffer.paths[path_id];
            const Path::Sub_Path& sub_path = path.sub_paths[sub_path_id];
            if (m_sequential_view.current.last <= sub_path.first.s_id || sub_path.last.s_id <= m_sequential_view.current.first)
                continue;

            // update cap for first endpoint of current range
            if (m_sequential_view.current.first > sub_path.first.s_id) {
                SequentialRangeCap& cap = (*sequential_range_caps)[0];
                const IBuffer& i_buffer = buffer.indices[ibuffer_id];
                cap.buffer = &buffer;
                cap.vbo = i_buffer.vbo;

                // calculate offset into the index buffer
                unsigned int offset = sub_path.first.i_id;
                offset += 6; // add 2 triangles for corner cap
                offset += static_cast<unsigned int>(m_sequential_view.current.first - sub_path.first.s_id) * buffer.indices_per_segment();
                if (sub_path_id == 0)
                    offset += 6; // add 2 triangles for starting cap

                // extract indices from index buffer
                std::array<IBufferType, 6> indices{ 0, 0, 0, 0, 0, 0 };
                glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, i_buffer.ibo));
                glsafe(::glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>((offset + 0) * sizeof(IBufferType)), static_cast<GLsizeiptr>(sizeof(IBufferType)), static_cast<void*>(&indices[0])));
                glsafe(::glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>((offset + 7) * sizeof(IBufferType)), static_cast<GLsizeiptr>(sizeof(IBufferType)), static_cast<void*>(&indices[1])));
                glsafe(::glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>((offset + 1) * sizeof(IBufferType)), static_cast<GLsizeiptr>(sizeof(IBufferType)), static_cast<void*>(&indices[2])));
                glsafe(::glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>((offset + 13) * sizeof(IBufferType)), static_cast<GLsizeiptr>(sizeof(IBufferType)), static_cast<void*>(&indices[4])));
                glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
                indices[3] = indices[0];
                indices[5] = indices[1];

                // send indices to gpu
                glsafe(::glGenBuffers(1, &cap.ibo));
                glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cap.ibo));
                glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(IBufferType), indices.data(), GL_STATIC_DRAW));
                glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));

                // extract color from render path
                size_t offset_bytes = offset * sizeof(IBufferType);
                for (const RenderPath& render_path : buffer.render_paths) {
                    if (render_path.ibuffer_id == ibuffer_id) {
                        for (size_t j = 0; j < render_path.offsets.size(); ++j) {
                            if (render_path.contains(offset_bytes)) {
                                cap.color = render_path.color;
                                break;
                            }
                        }
                    }
                }
            }

            // update cap for last endpoint of current range
            if (m_sequential_view.current.last < sub_path.last.s_id) {
                SequentialRangeCap& cap = (*sequential_range_caps)[1];
                const IBuffer& i_buffer = buffer.indices[ibuffer_id];
                cap.buffer = &buffer;
                cap.vbo = i_buffer.vbo;

                // calculate offset into the index buffer
                unsigned int offset = sub_path.first.i_id;
                offset += 6; // add 2 triangles for corner cap
                offset += static_cast<unsigned int>(m_sequential_view.current.last - 1 - sub_path.first.s_id) * buffer.indices_per_segment();
                if (sub_path_id == 0)
                    offset += 6; // add 2 triangles for starting cap

                // extract indices from index buffer
                std::array<IBufferType, 6> indices{ 0, 0, 0, 0, 0, 0 };
                glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, i_buffer.ibo));
                glsafe(::glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>((offset + 2) * sizeof(IBufferType)), static_cast<GLsizeiptr>(sizeof(IBufferType)), static_cast<void*>(&indices[0])));
                glsafe(::glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>((offset + 4) * sizeof(IBufferType)), static_cast<GLsizeiptr>(sizeof(IBufferType)), static_cast<void*>(&indices[1])));
                glsafe(::glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>((offset + 10) * sizeof(IBufferType)), static_cast<GLsizeiptr>(sizeof(IBufferType)), static_cast<void*>(&indices[2])));
                glsafe(::glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>((offset + 16) * sizeof(IBufferType)), static_cast<GLsizeiptr>(sizeof(IBufferType)), static_cast<void*>(&indices[5])));
                glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
                indices[3] = indices[0];
                indices[4] = indices[2];

                // send indices to gpu
                glsafe(::glGenBuffers(1, &cap.ibo));
                glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cap.ibo));
                glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, 6 * sizeof(IBufferType), indices.data(), GL_STATIC_DRAW));
                glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));

                // extract color from render path
                size_t offset_bytes = offset * sizeof(IBufferType);
                for (const RenderPath& render_path : buffer.render_paths) {
                    if (render_path.ibuffer_id == ibuffer_id) {
                        for (size_t j = 0; j < render_path.offsets.size(); ++j) {
                            if (render_path.contains(offset_bytes)) {
                                cap.color = render_path.color;
                                break;
                            }
                        }
                    }
                }
            }

            if ((*sequential_range_caps)[0].is_renderable() && (*sequential_range_caps)[1].is_renderable())
                break;
        }
    }

    //BBS
    enable_moves_slider(!paths.empty());

#if ENABLE_GCODE_VIEWER_STATISTICS
    for (const TBuffer& buffer : m_buffers) {
        statistics->render_paths_size += SLIC3R_STDUNORDEREDSET_MEMSIZE(buffer.render_paths, RenderPath);
        for (const RenderPath& path : buffer.render_paths) {
            statistics->render_paths_size += SLIC3R_STDVEC_MEMSIZE(path.sizes, unsigned int);
            statistics->render_paths_size += SLIC3R_STDVEC_MEMSIZE(path.offsets, size_t);
        }
        statistics->models_instances_size += SLIC3R_STDVEC_MEMSIZE(buffer.model.instances.buffer, float);
        statistics->models_instances_size += SLIC3R_STDVEC_MEMSIZE(buffer.model.instances.s_ids, size_t);
        statistics->models_instances_size += SLIC3R_STDVEC_MEMSIZE(buffer.model.instances.render_ranges.ranges, InstanceVBuffer::Ranges::Range);
    }
    statistics->refresh_paths_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time).count();
#endif // ENABLE_GCODE_VIEWER_STATISTICS
}

void GCodeViewer::render_toolpaths()
{
    const Camera& camera = wxGetApp().plater()->get_camera();
    const double zoom = camera.get_zoom();

    auto render_as_lines = [
#if ENABLE_GCODE_VIEWER_STATISTICS
        this
#endif // ENABLE_GCODE_VIEWER_STATISTICS
    ](std::vector<RenderPath>::iterator it_path, std::vector<RenderPath>::iterator it_end, GLShaderProgram& shader, int uniform_color) {
        for (auto it = it_path; it != it_end && it_path->ibuffer_id == it->ibuffer_id; ++it) {
            const RenderPath& path = *it;
            // Some OpenGL drivers crash on empty glMultiDrawElements, see GH #7415.
            assert(! path.sizes.empty());
            assert(! path.offsets.empty());
            shader.set_uniform(uniform_color, path.color);
            glsafe(::glMultiDrawElements(GL_LINES, (const GLsizei*)path.sizes.data(), GL_UNSIGNED_SHORT, (const void* const*)path.offsets.data(), (GLsizei)path.sizes.size()));
#if ENABLE_GCODE_VIEWER_STATISTICS
            ++m_statistics.gl_multi_lines_calls_count;
#endif // ENABLE_GCODE_VIEWER_STATISTICS
        }
    };

    auto render_as_triangles = [
#if ENABLE_GCODE_VIEWER_STATISTICS
        this
#endif // ENABLE_GCODE_VIEWER_STATISTICS
    ](std::vector<RenderPath>::iterator it_path, std::vector<RenderPath>::iterator it_end, GLShaderProgram& shader, int uniform_color) {
        for (auto it = it_path; it != it_end && it_path->ibuffer_id == it->ibuffer_id; ++it) {
            const RenderPath& path = *it;
            // Some OpenGL drivers crash on empty glMultiDrawElements, see GH #7415.
            assert(! path.sizes.empty());
            assert(! path.offsets.empty());
            shader.set_uniform(uniform_color, path.color);
            glsafe(::glMultiDrawElements(GL_TRIANGLES, (const GLsizei*)path.sizes.data(), GL_UNSIGNED_SHORT, (const void* const*)path.offsets.data(), (GLsizei)path.sizes.size()));
#if ENABLE_GCODE_VIEWER_STATISTICS
            ++m_statistics.gl_multi_triangles_calls_count;
#endif // ENABLE_GCODE_VIEWER_STATISTICS
        }
    };

    auto render_as_instanced_model = [
#if ENABLE_GCODE_VIEWER_STATISTICS
        this
#endif // ENABLE_GCODE_VIEWER_STATISTICS
        ](TBuffer& buffer, GLShaderProgram & shader) {
        for (auto& range : buffer.model.instances.render_ranges.ranges) {
            if (range.vbo == 0 && range.count > 0) {
                glsafe(::glGenBuffers(1, &range.vbo));
                glsafe(::glBindBuffer(GL_ARRAY_BUFFER, range.vbo));
                glsafe(::glBufferData(GL_ARRAY_BUFFER, range.count * buffer.model.instances.instance_size_bytes(), (const void*)&buffer.model.instances.buffer[range.offset * buffer.model.instances.instance_size_floats()], GL_STATIC_DRAW));
                glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
            }

            if (range.vbo > 0) {
                buffer.model.model.set_color(range.color);
                buffer.model.model.render_instanced(range.vbo, range.count);
#if ENABLE_GCODE_VIEWER_STATISTICS
                ++m_statistics.gl_instanced_models_calls_count;
                m_statistics.total_instances_gpu_size += static_cast<int64_t>(range.count * buffer.model.instances.instance_size_bytes());
#endif // ENABLE_GCODE_VIEWER_STATISTICS
            }
        }
    };

#if ENABLE_GCODE_VIEWER_STATISTICS
        auto render_as_batched_model = [this](TBuffer& buffer, GLShaderProgram& shader, int position_id, int normal_id) {
#else
        auto render_as_batched_model = [](TBuffer& buffer, GLShaderProgram& shader, int position_id, int normal_id) {
#endif // ENABLE_GCODE_VIEWER_STATISTICS

        struct Range
        {
            unsigned int first;
            unsigned int last;
            bool intersects(const Range& other) const { return (other.last < first || other.first > last) ? false : true; }
        };
        Range buffer_range = { 0, 0 };
        const size_t indices_per_instance = buffer.model.data.indices_count();

        for (size_t j = 0; j < buffer.indices.size(); ++j) {
            const IBuffer& i_buffer = buffer.indices[j];
            buffer_range.last = buffer_range.first + i_buffer.count / indices_per_instance;
            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, i_buffer.vbo));
            if (position_id != -1) {
                glsafe(::glVertexAttribPointer(position_id, buffer.vertices.position_size_floats(), GL_FLOAT, GL_FALSE, buffer.vertices.vertex_size_bytes(), (const void*)buffer.vertices.position_offset_bytes()));
                glsafe(::glEnableVertexAttribArray(position_id));
            }
            const bool has_normals = buffer.vertices.normal_size_floats() > 0;
            if (has_normals) {
                if (normal_id != -1) {
                    glsafe(::glVertexAttribPointer(normal_id, buffer.vertices.normal_size_floats(), GL_FLOAT, GL_FALSE, buffer.vertices.vertex_size_bytes(), (const void*)buffer.vertices.normal_offset_bytes()));
                    glsafe(::glEnableVertexAttribArray(normal_id));
                }
            }

            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, i_buffer.ibo));

            for (auto& range : buffer.model.instances.render_ranges.ranges) {
                const Range range_range = { range.offset, range.offset + range.count };
                if (range_range.intersects(buffer_range)) {
                    shader.set_uniform("uniform_color", range.color);
                    const unsigned int offset = (range_range.first > buffer_range.first) ? range_range.first - buffer_range.first : 0;
                    const size_t offset_bytes = static_cast<size_t>(offset) * indices_per_instance * sizeof(IBufferType);
                    const Range render_range = { std::max(range_range.first, buffer_range.first), std::min(range_range.last, buffer_range.last) };
                    const size_t count = static_cast<size_t>(render_range.last - render_range.first) * indices_per_instance;
                    if (count > 0) {
                        glsafe(::glDrawElements(GL_TRIANGLES, (GLsizei)count, GL_UNSIGNED_SHORT, (const void*)offset_bytes));
#if ENABLE_GCODE_VIEWER_STATISTICS
                        ++m_statistics.gl_batched_models_calls_count;
#endif // ENABLE_GCODE_VIEWER_STATISTICS
                    }
                }
            }

            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));

            if (normal_id != -1)
                glsafe(::glDisableVertexAttribArray(normal_id));
            if (position_id != -1)
                glsafe(::glDisableVertexAttribArray(position_id));
            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));

            buffer_range.first = buffer_range.last;
        }
    };

    auto line_width = [](double zoom) {
        return (zoom < 5.0) ? 1.0 : (1.0 + 5.0 * (zoom - 5.0) / (100.0 - 5.0));
    };

    const unsigned char begin_id = buffer_id(EMoveType::Retract);
    const unsigned char end_id   = buffer_id(EMoveType::Count);

    for (unsigned char i = begin_id; i < end_id; ++i) {
        TBuffer& buffer = m_buffers[i];
        if (!buffer.visible || !buffer.has_data())
            continue;

        GLShaderProgram* shader = wxGetApp().get_shader(buffer.shader.c_str());
        if (shader == nullptr)
            continue;

        shader->start_using();

        shader->set_uniform("view_model_matrix", camera.get_view_matrix());
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("view_normal_matrix", (Matrix3d)Matrix3d::Identity());

        if (buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::InstancedModel) {
            shader->set_uniform("emission_factor", 0.25f);
            render_as_instanced_model(buffer, *shader);
            shader->set_uniform("emission_factor", 0.0f);
        }
        else if (buffer.render_primitive_type == TBuffer::ERenderPrimitiveType::BatchedModel) {
            shader->set_uniform("emission_factor", 0.25f);
            const int position_id = shader->get_attrib_location("v_position");
            const int normal_id   = shader->get_attrib_location("v_normal");
            render_as_batched_model(buffer, *shader, position_id, normal_id);
            shader->set_uniform("emission_factor", 0.0f);
        }
        else {
            const int position_id = shader->get_attrib_location("v_position");
            const int normal_id   = shader->get_attrib_location("v_normal");
            const int uniform_color = shader->get_uniform_location("uniform_color");

            auto it_path = buffer.render_paths.begin();
            for (unsigned int ibuffer_id = 0; ibuffer_id < static_cast<unsigned int>(buffer.indices.size()); ++ibuffer_id) {
                const IBuffer& i_buffer = buffer.indices[ibuffer_id];
                // Skip all paths with ibuffer_id < ibuffer_id.
                for (; it_path != buffer.render_paths.end() && it_path->ibuffer_id < ibuffer_id; ++it_path);
                if (it_path == buffer.render_paths.end() || it_path->ibuffer_id > ibuffer_id)
                    // Not found. This shall not happen.
                    continue;

                glsafe(::glBindBuffer(GL_ARRAY_BUFFER, i_buffer.vbo));
                if (position_id != -1) {
                    glsafe(::glVertexAttribPointer(position_id, buffer.vertices.position_size_floats(), GL_FLOAT, GL_FALSE, buffer.vertices.vertex_size_bytes(), (const void*)buffer.vertices.position_offset_bytes()));
                    glsafe(::glEnableVertexAttribArray(position_id));
                }
                const bool has_normals = buffer.vertices.normal_size_floats() > 0;
                if (has_normals) {
                    if (normal_id != -1) {
                        glsafe(::glVertexAttribPointer(normal_id, buffer.vertices.normal_size_floats(), GL_FLOAT, GL_FALSE, buffer.vertices.vertex_size_bytes(), (const void*)buffer.vertices.normal_offset_bytes()));
                        glsafe(::glEnableVertexAttribArray(normal_id));
                    }
                }

                glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, i_buffer.ibo));

                // Render all elements with it_path->ibuffer_id == ibuffer_id, possible with varying colors.
                switch (buffer.render_primitive_type)
                {
                case TBuffer::ERenderPrimitiveType::Line: {
                    glsafe(::glLineWidth(static_cast<GLfloat>(line_width(zoom))));
                    render_as_lines(it_path, buffer.render_paths.end(), *shader, uniform_color);
                    break;
                }
                case TBuffer::ERenderPrimitiveType::Triangle: {
                    render_as_triangles(it_path, buffer.render_paths.end(), *shader, uniform_color);
                    break;
                }
                default: { break; }
                }

                glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));

                if (normal_id != -1)
                    glsafe(::glDisableVertexAttribArray(normal_id));
                if (position_id != -1)
                    glsafe(::glDisableVertexAttribArray(position_id));
                glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
            }
        }

        shader->stop_using();
    }

#if ENABLE_GCODE_VIEWER_STATISTICS
    auto render_sequential_range_cap = [this, &camera]
#else
    auto render_sequential_range_cap = [&camera]
#endif // ENABLE_GCODE_VIEWER_STATISTICS
    (const SequentialRangeCap& cap) {
        const TBuffer* buffer = cap.buffer;
        GLShaderProgram* shader = wxGetApp().get_shader(buffer->shader.c_str());
        if (shader == nullptr)
            return;

        shader->start_using();

        shader->set_uniform("view_model_matrix", camera.get_view_matrix());
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("view_normal_matrix", (Matrix3d)Matrix3d::Identity());

        const int position_id = shader->get_attrib_location("v_position");
        const int normal_id   = shader->get_attrib_location("v_normal");

        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, cap.vbo));
        if (position_id != -1) {
            glsafe(::glVertexAttribPointer(position_id, buffer->vertices.position_size_floats(), GL_FLOAT, GL_FALSE, buffer->vertices.vertex_size_bytes(), (const void*)buffer->vertices.position_offset_bytes()));
            glsafe(::glEnableVertexAttribArray(position_id));
        }
        const bool has_normals = buffer->vertices.normal_size_floats() > 0;
        if (has_normals) {
            if (normal_id != -1) {
                glsafe(::glVertexAttribPointer(normal_id, buffer->vertices.normal_size_floats(), GL_FLOAT, GL_FALSE, buffer->vertices.vertex_size_bytes(), (const void*)buffer->vertices.normal_offset_bytes()));
                glsafe(::glEnableVertexAttribArray(normal_id));
            }
        }

        shader->set_uniform("uniform_color", cap.color);

        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cap.ibo));
        glsafe(::glDrawElements(GL_TRIANGLES, (GLsizei)cap.indices_count(), GL_UNSIGNED_SHORT, nullptr));
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));

#if ENABLE_GCODE_VIEWER_STATISTICS
            ++m_statistics.gl_triangles_calls_count;
#endif // ENABLE_GCODE_VIEWER_STATISTICS

        if (normal_id != -1)
            glsafe(::glDisableVertexAttribArray(normal_id));
        if (position_id != -1)
            glsafe(::glDisableVertexAttribArray(position_id));

        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));

        shader->stop_using();
    };

    for (unsigned int i = 0; i < 2; ++i) {
        if (m_sequential_range_caps[i].is_renderable())
            render_sequential_range_cap(m_sequential_range_caps[i]);
    }
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
            const PrintEstimatedStatistics::Mode& plate_time_mode = plate_print_statistics.modes[static_cast<size_t>(m_time_estimate_mode)];
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
    if (!m_legend_enabled)
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

    const PrintEstimatedStatistics::Mode& time_mode = m_print_statistics.modes[static_cast<size_t>(m_time_estimate_mode)];
    //BBS
    /*bool show_estimated_time = time_mode.time > 0.0f && (m_view_type == EViewType::FeatureType ||
        (m_view_type == EViewType::ColorPrint && !time_mode.custom_gcode_times.empty()));*/
    bool show_estimated = time_mode.time > 0.0f && (m_view_type == EViewType::FeatureType || m_view_type == EViewType::ColorPrint);

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

    auto append_range = [append_item](const Extrusions::Range& range, unsigned int decimals) {
        auto append_range_item = [append_item](int i, float value, unsigned int decimals) {
            char buf[1024];
            ::sprintf(buf, "%.*f", decimals, value);
            append_item(EItemType::Rect, Range_Colors[i], { { buf , 0} });
        };

        if (range.count == 1)
            // single item use case
            append_range_item(0, range.min, decimals);
        else if (range.count == 2) {
            append_range_item(static_cast<int>(Range_Colors.size()) - 1, range.max, decimals);
            append_range_item(0, range.min, decimals);
        }
        else {
            const float step_size = range.step_size();
            for (int i = static_cast<int>(Range_Colors.size()) - 1; i >= 0; --i) {
                append_range_item(i, range.get_value_at_step(i), decimals);
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

            const std::vector<double> zs = m_layers.get_zs();
            auto lower_b = std::lower_bound(zs.begin(), zs.end(), item.print_z - epsilon());
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

    auto role_time_and_percent = [time_mode](ExtrusionRole role) {
        auto it = std::find_if(time_mode.roles_times.begin(), time_mode.roles_times.end(), [role](const std::pair<ExtrusionRole, float>& item) { return role == item.first; });
        return (it != time_mode.roles_times.end()) ? std::make_pair(it->second, it->second / time_mode.time) : std::make_pair(0.0f, 0.0f);
    };

    auto move_time_and_percent = [time_mode](EMoveType move_type) {
        auto it = std::find_if(time_mode.moves_times.begin(), time_mode.moves_times.end(), [move_type](const std::pair<EMoveType, float>& item) { return move_type == item.first; });
        return (it != time_mode.moves_times.end()) ? std::make_pair(it->second, it->second / time_mode.time) : std::make_pair(0.0f, 0.0f);
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
                // update buffers' render paths
                refresh_render_paths(false, false);
                update_moves_slider();
                wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
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


    // used filament statistics
    for (size_t extruder_id : m_extruder_ids) {
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

    for (size_t extruder_id : m_extruder_ids) {
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

    for (size_t extruder_id : m_extruder_ids) {
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

    for (size_t extruder_id : m_extruder_ids) {
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
    switch (m_view_type)
    {
    case EViewType::FeatureType:
    {
        // calculate offsets to align time/percentage data
        char buffer[64];
        for (size_t i = 0; i < m_roles.size(); ++i) {
            ExtrusionRole role = m_roles[i];
            if (role < erCount) {
                labels.push_back(_u8L(ExtrusionEntity::role_to_string(role)));
                auto [time, percent] = role_time_and_percent(role);
                times.push_back((time > 0.0f) ? short_time(get_time_dhms(time)) : "");
                if (percent == 0) // ORCA remove % symbol from rows
                    ::sprintf(buffer, "0");
                else
                    percent > 0.001 ? ::sprintf(buffer, "%.1f", percent * 100) : ::sprintf(buffer, "<0.1");
                percents.push_back(buffer);

                auto [model_used_filament_m, model_used_filament_g] = used_filament_per_role(role);
                ::sprintf(buffer, imperial_units ? "%.2fin" : "%.2fm", model_used_filament_m); // ORCA dont use spacing between value and unit
                used_filaments_length.push_back(buffer);
                ::sprintf(buffer, imperial_units ? "%.2foz" : "%.2fg", model_used_filament_g); // ORCA dont use spacing between value and unit
                used_filaments_weight.push_back(buffer);
            }
        }

        //BBS: get travel time and percent
        {
            auto [time, percent] = move_time_and_percent(EMoveType::Travel);
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
    case EViewType::Height:         { imgui.title(_u8L("Layer Height (mm)")); break; }
    case EViewType::Width:          { imgui.title(_u8L("Line Width (mm)")); break; }
    case EViewType::Feedrate:
    {
        imgui.title(_u8L("Speed (mm/s)"));
        break;
    }

    case EViewType::FanSpeed:       { imgui.title(_u8L("Fan Speed (%)")); break; }
    case EViewType::Temperature:    { imgui.title(_u8L("Temperature (°C)")); break; }
    case EViewType::VolumetricRate: { imgui.title(_u8L("Volumetric flow rate (mm³/s)")); break; }
    case EViewType::LayerTime:      { imgui.title(_u8L("Layer Time")); break; }
    case EViewType::LayerTimeLog:   { imgui.title(_u8L("Layer Time (log)")); break; }

    case EViewType::Tool:
    {
        // calculate used filaments data
        for (size_t extruder_id : m_extruder_ids) {
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
    case EViewType::ColorPrint:
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

    auto append_option_item = [this, append_item](EMoveType type, std::vector<float> offsets) {
        auto append_option_item_with_type = [this, offsets, append_item](EMoveType type, const ColorRGBA& color, const std::string& label, bool visible) {
            append_item(EItemType::Rect, color, {{ label , offsets[0] }}, true, offsets.back()/*ORCA checkbox_pos*/, visible, [this, type, visible]() {
                m_buffers[buffer_id(type)].visible = !m_buffers[buffer_id(type)].visible;
                // update buffers' render paths
                refresh_render_paths(false, false);
                update_moves_slider();
                wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
                });
        };
        const bool visible = m_buffers[buffer_id(type)].visible;
        if (type == EMoveType::Travel) {
            //BBS: only display travel time in FeatureType view
            append_option_item_with_type(type, Travel_Colors[0], _u8L("Travel"), visible);
        }
        else if (type == EMoveType::Seam)
            append_option_item_with_type(type, Options_Colors[(int)EOptionsColors::Seams], _u8L("Seams"), visible);
        else if (type == EMoveType::Retract)
            append_option_item_with_type(type, Options_Colors[(int)EOptionsColors::Retractions], _u8L("Retract"), visible);
        else if (type == EMoveType::Unretract)
            append_option_item_with_type(type, Options_Colors[(int)EOptionsColors::Unretractions], _u8L("Unretract"), visible);
        else if (type == EMoveType::Tool_change)
            append_option_item_with_type(type, Options_Colors[(int)EOptionsColors::ToolChanges], _u8L("Filament Changes"), visible);
        else if (type == EMoveType::Wipe)
            append_option_item_with_type(type, Wipe_Color, _u8L("Wipe"), visible);
    };

    // extrusion paths section -> items
    switch (m_view_type)
    {
    case EViewType::FeatureType:
    {
        for (size_t i = 0; i < m_roles.size(); ++i) {
            ExtrusionRole role = m_roles[i];
            if (role >= erCount)
                continue;
            const bool visible = is_visible(role);
            std::vector<std::pair<std::string, float>> columns_offsets;
            columns_offsets.push_back({ labels[i], offsets[0] });
            columns_offsets.push_back({ times[i], offsets[1] });
            columns_offsets.push_back({percents[i], offsets[2]});
            columns_offsets.push_back({used_filaments_length[i], offsets[3]});
            columns_offsets.push_back({used_filaments_weight[i], offsets[4]});
            append_item(EItemType::Rect, Extrusion_Role_Colors[static_cast<unsigned int>(role)], columns_offsets,
                true, offsets.back(), visible, [this, role, visible]() {
                    m_extrusions.role_visibility_flags = visible ? m_extrusions.role_visibility_flags & ~(1 << role) : m_extrusions.role_visibility_flags | (1 << role);
                    // update buffers' render paths
                    refresh_render_paths(false, false);
                    update_moves_slider();
                    wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
                });
        }

        for(auto item : options_items) {
            if (item != EMoveType::Travel) {
                append_option_item(item, offsets);
            } else {
                //BBS: show travel time in FeatureType view
                const bool visible = m_buffers[buffer_id(item)].visible;
                std::vector<std::pair<std::string, float>> columns_offsets;
                columns_offsets.push_back({ _u8L("Travel"), offsets[0] });
                columns_offsets.push_back({ travel_time, offsets[1] });
                columns_offsets.push_back({ travel_percent, offsets[2] });
                append_item(EItemType::Rect, Travel_Colors[0], columns_offsets, true, offsets.back()/*ORCA checkbox_pos*/, visible, [this, item, visible]() {
                        m_buffers[buffer_id(item)].visible = !m_buffers[buffer_id(item)].visible;
                        // update buffers' render paths
                        refresh_render_paths(false, false);
                        update_moves_slider();
                        wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
                    });
            }
        }
        break;
    }
    case EViewType::Height:         { append_range(m_extrusions.ranges.height, 2); break; }
    case EViewType::Width:          { append_range(m_extrusions.ranges.width, 2); break; }
    case EViewType::Feedrate:       {
        append_range(m_extrusions.ranges.feedrate, 0);
        ImGui::Spacing();
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        offsets = calculate_offsets({ { _u8L("Options"), { _u8L("Travel")}}, { _u8L("Display"), {""}} }, icon_size);
        append_headers({ {_u8L("Options"), offsets[0] }, { _u8L("Display"), offsets[1]} });
        const bool travel_visible = m_buffers[buffer_id(EMoveType::Travel)].visible;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 3.0f));
        append_item(EItemType::None, Travel_Colors[0], { {_u8L("travel"), offsets[0] }}, true, predictable_icon_pos/*ORCA checkbox_pos*/, travel_visible, [this, travel_visible]() {
            m_buffers[buffer_id(EMoveType::Travel)].visible = !m_buffers[buffer_id(EMoveType::Travel)].visible;
            // update buffers' render paths, and update m_tools.m_tool_colors and m_extrusions.ranges
            refresh(*m_gcode_result, wxGetApp().plater()->get_extruder_colors_from_plater_config(m_gcode_result));
            update_moves_slider();
            wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
            });
        ImGui::PopStyleVar(1);
        break;
    }
    case EViewType::FanSpeed:       { append_range(m_extrusions.ranges.fan_speed, 0); break; }
    case EViewType::Temperature:    { append_range(m_extrusions.ranges.temperature, 0); break; }
    case EViewType::LayerTime:      { append_range(m_extrusions.ranges.layer_duration, true); break; }
    case EViewType::LayerTimeLog:   { append_range(m_extrusions.ranges.layer_duration_log, true); break; }
    case EViewType::VolumetricRate: { append_range(m_extrusions.ranges.volumetric_rate, 2); break; }
    case EViewType::Tool:
    {
        // shows only extruders actually used
        char buf[64];
        size_t i = 0;
        for (unsigned char extruder_id : m_extruder_ids) {
            ::sprintf(buf, imperial_units ? "%.2f in    %.2f g" : "%.2f m    %.2f g", model_used_filaments_m[i], model_used_filaments_g[i]);
            append_item(EItemType::Rect, m_tools.m_tool_colors[extruder_id], { { _u8L("Extruder") + " " + std::to_string(extruder_id + 1), offsets[0]}, {buf, offsets[1]} });
            i++;
        }
        break;
    }
    case EViewType::Summary:
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
    case EViewType::ColorPrint:
    {
        //BBS: replace model custom gcode with current plate custom gcode
        const std::vector<CustomGCode::Item>& custom_gcode_per_print_z = wxGetApp().is_editor() ? wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes : m_custom_gcode_per_print_z;
        size_t total_items = 1;
        // BBS: no ColorChange type, use ToolChange
        //for (size_t extruder_id : m_extruder_ids) {
        //    total_items += color_print_ranges(extruder_id, custom_gcode_per_print_z).size();
        //}

        const bool need_scrollable = static_cast<float>(total_items) * (icon_size + ImGui::GetStyle().ItemSpacing.y) > child_height;

        // add scrollable region, if needed
        if (need_scrollable)
            ImGui::BeginChild("color_prints", { -1.0f, child_height }, false);

        // shows only extruders actually used
        size_t i = 0;
        for (auto extruder_idx : m_extruder_ids) {
            const bool filament_visible = m_tools.m_tool_visibles[extruder_idx];
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
                append_item(EItemType::Rect, m_tools.m_tool_colors[extruder_idx], columns_offsets, true, checkbox_pos/*ORCA*/, filament_visible, [this, extruder_idx]() {
                        m_tools.m_tool_visibles[extruder_idx] = !m_tools.m_tool_visibles[extruder_idx];
                        // update buffers' render paths
                        refresh_render_paths(false, false);
                        update_moves_slider();
                        wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
                    });
            }
            i++;
        }
        
        if (need_scrollable)
            ImGui::EndChild();

        // Sum of all rows
        char buf[64];
        if (m_extruder_ids.size() > 1) {
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
            append_item(EItemType::None, m_tools.m_tool_colors[0], columns_offsets);
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
    if (m_view_type == EViewType::ColorPrint) {
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
            std::vector<ColorRGBA> last_color(m_extruders_count);
            for (size_t i = 0; i < m_extruders_count; ++i) {
                last_color[i] = m_tools.m_tool_colors[i];
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
    if (m_buffers[buffer_id(EMoveType::Travel)].visible) {
        switch (m_view_type)
        {
        case EViewType::Feedrate:
        case EViewType::Tool:
        case EViewType::ColorPrint: {
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

    auto any_option_available = [this]() {
        auto available = [this](EMoveType type) {
            const TBuffer& buffer = m_buffers[buffer_id(type)];
            return buffer.visible && buffer.has_data();
        };

        return available(EMoveType::Color_change) ||
            available(EMoveType::Custom_GCode) ||
            available(EMoveType::Pause_Print) ||
            available(EMoveType::Retract) ||
            available(EMoveType::Tool_change) ||
            available(EMoveType::Unretract) ||
            available(EMoveType::Seam);
    };

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
    show_settings &= (m_view_type == EViewType::FeatureType || m_view_type == EViewType::Tool);
    show_settings &= has_settings;
    if (show_settings) {
        auto calc_offset = [this]() {
            float ret = 0.0f;
            if (!m_settings_ids.printer.empty())
                ret = std::max(ret, ImGui::CalcTextSize((_u8L("Printer") + std::string(":")).c_str()).x);
            if (!m_settings_ids.print.empty())
                ret = std::max(ret, ImGui::CalcTextSize((_u8L("Print settings") + std::string(":")).c_str()).x);
            if (!m_settings_ids.filament.empty()) {
                for (unsigned char i : m_extruder_ids) {
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
            for (unsigned char i : m_extruder_ids) {
                if (i < static_cast<unsigned char>(m_settings_ids.filament.size()) && !m_settings_ids.filament[i].empty()) {
                    std::string txt = _u8L("Filament");
                    txt += (m_extruder_ids.size() == 1) ? ":" : " " + std::to_string(i + 1);
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
            int  layer = m_layers.get_l_at(custom_gcode.print_z);
            ::sprintf(buf, "%d",layer );
            imgui.text(buf);
            ImGui::SameLine(max_len * 1.5);
            
            std::vector<float> layer_times = m_print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].layers_times;
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
    std::string time_title = m_view_type == EViewType::FeatureType ? _u8L("Total Estimation") : _u8L("Time Estimation");
    auto can_show_mode_button = [this](PrintEstimatedStatistics::ETimeMode mode) {
        bool show = false;
        if (m_print_statistics.modes.size() > 1 && m_print_statistics.modes[static_cast<size_t>(mode)].roles_times.size() > 0) {
            for (size_t i = 0; i < m_print_statistics.modes.size(); ++i) {
                if (i != static_cast<size_t>(mode) &&
                    m_print_statistics.modes[i].time > 0.0f &&
                    short_time(get_time_dhms(m_print_statistics.modes[static_cast<size_t>(mode)].time)) != short_time(get_time_dhms(m_print_statistics.modes[i].time))) {
                    show = true;
                    break;
                }
            }
        }
        return show;
    };
 if (can_show_mode_button(m_time_estimate_mode)) {
        switch (m_time_estimate_mode)
        {
        case PrintEstimatedStatistics::ETimeMode::Normal: { time_title += " [" + _u8L("Normal mode") + "]"; break; }
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
    if (time_mode.layers_times.empty())
        max_len += ImGui::CalcTextSize(total_str.c_str()).x;
    else {
        if (m_view_type == EViewType::FeatureType)
            max_len += std::max(ImGui::CalcTextSize(cost_str.c_str()).x,
                std::max(ImGui::CalcTextSize(print_str.c_str()).x,
                    std::max(std::max(ImGui::CalcTextSize(prepare_str.c_str()).x, ImGui::CalcTextSize(total_str.c_str()).x),
                        std::max(ImGui::CalcTextSize(total_filament_str.c_str()).x, ImGui::CalcTextSize(model_filament_str.c_str()).x))));
        else
            max_len += std::max(ImGui::CalcTextSize(print_str.c_str()).x,
                (std::max(ImGui::CalcTextSize(prepare_str.c_str()).x, ImGui::CalcTextSize(total_str.c_str()).x)));
    }
    if (m_view_type == EViewType::FeatureType) {
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
     auto role_time = [time_mode](ExtrusionRole role) {
        auto it = std::find_if(time_mode.roles_times.begin(), time_mode.roles_times.end(), [role](const std::pair<ExtrusionRole, float>& item) { return role == item.first; });
            return (it != time_mode.roles_times.end()) ? it->second : 0.0f;
        };
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

    auto show_mode_button = [this, &imgui, can_show_mode_button](const wxString& label, PrintEstimatedStatistics::ETimeMode mode) {
        if (can_show_mode_button(mode)) {
            if (imgui.button(label)) {
                m_time_estimate_mode = mode;
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
                imgui.set_requires_extra_frame();
#else
                wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
            wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
            }
        }
    };

    switch (m_time_estimate_mode) {
    case PrintEstimatedStatistics::ETimeMode::Normal: {
        show_mode_button(_L("Switch to silent mode"), PrintEstimatedStatistics::ETimeMode::Stealth);
        break;
    }
    case PrintEstimatedStatistics::ETimeMode::Stealth: {
        show_mode_button(_L("Switch to normal mode"), PrintEstimatedStatistics::ETimeMode::Normal);
        break;
    }
    default : { assert(false); break; }
    }

    if (m_view_type == EViewType::ColorPrint) {
        ImGui::Spacing();
        ImGui::Dummy({ window_padding, window_padding });
        ImGui::SameLine();
        offsets = calculate_offsets({ { _u8L("Options"), { ""}}, { _u8L("Display"), {""}} }, icon_size);
        offsets[1] = std::max(predictable_icon_pos, color_print_offsets[_u8L("Display")]); // ORCA prefer predictable_icon_pos when header not reacing end
        append_headers({ {_u8L("Options"), offsets[0] }, { _u8L("Display"), offsets[1]} });
        for (auto item : options_items)
            append_option_item(item, offsets);
    }
    ImGui::Dummy({ window_padding, window_padding });
    if (m_nozzle_nums > 1 && (m_view_type == EViewType::Summary || m_view_type == EViewType::ColorPrint)) // ORCA show only on summary and filament tab
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

#if ENABLE_GCODE_VIEWER_STATISTICS
void GCodeViewer::render_statistics()
{
    static const float offset = 275.0f;

    ImGuiWrapper& imgui = *wxGetApp().imgui();

    auto add_time = [&imgui](const std::string& label, int64_t time) {
        imgui.text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, label);
        ImGui::SameLine(offset);
        imgui.text(std::to_string(time) + " ms (" + get_time_dhms(static_cast<float>(time) * 0.001f) + ")");
    };

    auto add_memory = [&imgui](const std::string& label, int64_t memory) {
        auto format_string = [memory](const std::string& units, float value) {
            return std::to_string(memory) + " bytes (" +
                   Slic3r::float_to_string_decimal_point(float(memory) * value, 3)
                    + " " + units + ")";
        };

        static const float kb = 1024.0f;
        static const float inv_kb = 1.0f / kb;
        static const float mb = 1024.0f * kb;
        static const float inv_mb = 1.0f / mb;
        static const float gb = 1024.0f * mb;
        static const float inv_gb = 1.0f / gb;
        imgui.text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, label);
        ImGui::SameLine(offset);
        if (static_cast<float>(memory) < mb)
            imgui.text(format_string("KB", inv_kb));
        else if (static_cast<float>(memory) < gb)
            imgui.text(format_string("MB", inv_mb));
        else
            imgui.text(format_string("GB", inv_gb));
    };

    auto add_counter = [&imgui](const std::string& label, int64_t counter) {
        imgui.text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, label);
        ImGui::SameLine(offset);
        imgui.text(std::to_string(counter));
    };

    imgui.set_next_window_pos(0.5f * wxGetApp().plater()->get_current_canvas3D()->get_canvas_size().get_width(), 0.0f, ImGuiCond_Once, 0.5f, 0.0f);
    ImGui::SetNextWindowSizeConstraints({ 300.0f, 100.0f }, { 600.0f, 900.0f });
    imgui.begin(std::string("GCodeViewer Statistics"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    if (ImGui::CollapsingHeader("Time")) {
        add_time(std::string("GCodeProcessor:"), m_statistics.results_time);

        ImGui::Separator();
        add_time(std::string("Load:"), m_statistics.load_time);
        add_time(std::string("  Load vertices:"), m_statistics.load_vertices);
        add_time(std::string("  Smooth vertices:"), m_statistics.smooth_vertices);
        add_time(std::string("  Load indices:"), m_statistics.load_indices);
        add_time(std::string("Refresh:"), m_statistics.refresh_time);
        add_time(std::string("Refresh paths:"), m_statistics.refresh_paths_time);
    }

    if (ImGui::CollapsingHeader("OpenGL calls")) {
        add_counter(std::string("Multi GL_LINES:"), m_statistics.gl_multi_lines_calls_count);
        add_counter(std::string("Multi GL_TRIANGLES:"), m_statistics.gl_multi_triangles_calls_count);
        add_counter(std::string("GL_TRIANGLES:"), m_statistics.gl_triangles_calls_count);
        ImGui::Separator();
        add_counter(std::string("Instanced models:"), m_statistics.gl_instanced_models_calls_count);
        add_counter(std::string("Batched models:"), m_statistics.gl_batched_models_calls_count);
    }

    if (ImGui::CollapsingHeader("CPU memory")) {
        add_memory(std::string("GCodeProcessor results:"), m_statistics.results_size);

        ImGui::Separator();
        add_memory(std::string("Paths:"), m_statistics.paths_size);
        add_memory(std::string("Render paths:"), m_statistics.render_paths_size);
        add_memory(std::string("Models instances:"), m_statistics.models_instances_size);
    }

    if (ImGui::CollapsingHeader("GPU memory")) {
        add_memory(std::string("Vertices:"), m_statistics.total_vertices_gpu_size);
        add_memory(std::string("Indices:"), m_statistics.total_indices_gpu_size);
        add_memory(std::string("Instances:"), m_statistics.total_instances_gpu_size);
        ImGui::Separator();
        add_memory(std::string("Max VBuffer:"), m_statistics.max_vbuffer_gpu_size);
        add_memory(std::string("Max IBuffer:"), m_statistics.max_ibuffer_gpu_size);
    }

    if (ImGui::CollapsingHeader("Other")) {
        add_counter(std::string("Travel segments count:"), m_statistics.travel_segments_count);
        add_counter(std::string("Wipe segments count:"), m_statistics.wipe_segments_count);
        add_counter(std::string("Extrude segments count:"), m_statistics.extrude_segments_count);
        add_counter(std::string("Instances count:"), m_statistics.instances_count);
        add_counter(std::string("Batched count:"), m_statistics.batched_count);
        ImGui::Separator();
        add_counter(std::string("VBuffers count:"), m_statistics.vbuffers_count);
        add_counter(std::string("IBuffers count:"), m_statistics.ibuffers_count);
    }

    imgui.end();
}
#endif // ENABLE_GCODE_VIEWER_STATISTICS

void GCodeViewer::log_memory_used(const std::string& label, int64_t additional) const
{
    if (Slic3r::get_logging_level() >= 5) {
        int64_t paths_size = 0;
        int64_t render_paths_size = 0;
        for (const TBuffer& buffer : m_buffers) {
            paths_size += SLIC3R_STDVEC_MEMSIZE(buffer.paths, Path);
            render_paths_size += SLIC3R_STDUNORDEREDSET_MEMSIZE(buffer.render_paths, RenderPath);
            for (const RenderPath& path : buffer.render_paths) {
                render_paths_size += SLIC3R_STDVEC_MEMSIZE(path.sizes, unsigned int);
                render_paths_size += SLIC3R_STDVEC_MEMSIZE(path.offsets, size_t);
            }
        }
        int64_t layers_size = SLIC3R_STDVEC_MEMSIZE(m_layers.get_zs(), double);
        layers_size += SLIC3R_STDVEC_MEMSIZE(m_layers.get_endpoints(), Layers::Endpoints);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format("paths_size %1%, render_paths_size %2%,layers_size %3%, additional %4%\n")
            %paths_size %render_paths_size %layers_size %additional;
        BOOST_LOG_TRIVIAL(trace) << label
            << "(" << format_memsize_MB(additional + paths_size + render_paths_size + layers_size) << ");"
            << log_memory_info();
    }
}

ColorRGBA GCodeViewer::option_color(EMoveType move_type) const
{
    switch (move_type)
    {
    case EMoveType::Tool_change:  { return Options_Colors[static_cast<unsigned int>(EOptionsColors::ToolChanges)]; }
    case EMoveType::Color_change: { return Options_Colors[static_cast<unsigned int>(EOptionsColors::ColorChanges)]; }
    case EMoveType::Pause_Print:  { return Options_Colors[static_cast<unsigned int>(EOptionsColors::PausePrints)]; }
    case EMoveType::Custom_GCode: { return Options_Colors[static_cast<unsigned int>(EOptionsColors::CustomGCodes)]; }
    case EMoveType::Retract:      { return Options_Colors[static_cast<unsigned int>(EOptionsColors::Retractions)]; }
    case EMoveType::Unretract:    { return Options_Colors[static_cast<unsigned int>(EOptionsColors::Unretractions)]; }
    case EMoveType::Seam:         { return Options_Colors[static_cast<unsigned int>(EOptionsColors::Seams)]; }
    default:                      { return { 0.0f, 0.0f, 0.0f, 1.0f }; }
    }
}

} // namespace GUI
} // namespace Slic3r

