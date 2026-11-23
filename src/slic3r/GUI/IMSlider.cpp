#include "IMSlider.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "NotificationManager.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/libslic3r.h"
#include <imgui.h>
#include <cmath>
#include <cstddef>
#include <functional>
#include "libslic3r/ExPolygon.hpp"
#include <string>
#include "libslic3r/CustomGCode.hpp"
#include <cstdio>
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "libslic3r/Utils.hpp"
#include <algorithm>
#include <vector>
#include "slic3r/GUI/TickCode.hpp"
#include "slic3r/GUI/PreviewOverlayLayout.hpp"
#include "slic3r/GUI/PreviewMarks.hpp"
#include <boost/log/trivial.hpp>
#include <wx/colour.h>
#include "libslic3r/Color.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include <set>
#include <wx/string.h>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <wx/event.h>
#include <wx/utils.h>
#include <wx/slider.h>
#include <array>
#include <initializer_list>
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>
#include "libslic3r/Config.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r {

using namespace CustomGCode;

namespace GUI {

// equal to 25 mm2
inline double min_delta_area() { return scale_(scale_(25)); }
// equal to 1 mm2
inline double miscalculation() { return scale_(scale_(1)); }

static const ImU32 BACKGROUND_COLOR_DARK  = IM_COL32(65, 65, 71, 255);
static const ImU32 BACKGROUND_COLOR_LIGHT = IM_COL32(255, 255, 255, 255);
static const ImU32 GROOVE_COLOR_DARK      = IM_COL32(45, 45, 49, 255);
static const ImU32 GROOVE_COLOR_LIGHT     = IM_COL32(206, 206, 206, 255);
static const ImU32 BRAND_COLOR            = IM_COL32(0, 150, 136, 255);
static const ImU32 BRAND_HOVER_COLOR      = IM_COL32(38, 166, 154, 255);
static const ImU32 PAUSE_COLOR            = IM_COL32(255, 111, 0, 255);
static const ImU32 CUSTOM_GCODE_COLOR     = IM_COL32(178, 156, 232, 255);
static const ImU32 HANDLE_RING_COLOR      = IM_COL32(255, 255, 255, 255);
static const ImU32 TRACK_COLOR            = IM_COL32(150, 150, 150, 255);
static const ImU32 BADGE_TEXT_COLOR       = IM_COL32(40, 40, 44, 255);

static int m_tick_value = -1;
static ImVec4 m_tick_rect;

bool equivalent_areas(const double& bottom_area, const double& top_area)
{
    return fabs(bottom_area - top_area) <= miscalculation();
}

bool check_color_change(PrintObject *object, size_t frst_layer_id, size_t layers_cnt, bool check_overhangs, std::function<bool(Layer *)> break_condition)
{
    double prev_area = area(object->get_layer(frst_layer_id)->lslices);

    bool detected = false;
    for (size_t i = frst_layer_id + 1; i < layers_cnt; i++) {
        Layer *layer    = object->get_layer(i);
        double cur_area = area(layer->lslices);

        // check for overhangs
        if (check_overhangs && cur_area > prev_area && !equivalent_areas(prev_area, cur_area)) break;

        // Check percent of the area decrease.
        // This value have to be more than min_delta_area and more then 10%
        if ((prev_area - cur_area > min_delta_area()) && (cur_area / prev_area < 0.9)) {
            detected = true;
            if (break_condition(layer)) break;
        }

        prev_area = cur_area;
    }
    return detected;
}


static std::string gcode(Type type)
{
    Slic3r::DynamicPrintConfig config = wxGetApp().preset_bundle->full_config();
    switch (type) {
    //BBS
    case Template:    return config.opt_string("template_custom_gcode");
    case PausePrint:  return config.opt_string("machine_pause_gcode");

    default:          return "";
    }

    //const PrintConfig& config = GUI::wxGetApp().plater()->fff_print().config();
    //switch (type) {
    ////BBS
    ////case ColorChange: return config.color_change_gcode;
    //case PausePrint:  return config.machine_pause_gcode;
    //case Template:    return config.template_custom_gcode;
    //default:          return "";
    //}
}

static std::string short_and_splitted_time(const std::string &time)
{
    // Parse the dhms time format.
    int days    = 0;
    int hours   = 0;
    int minutes = 0;
    int seconds = 0;
    if (time.find('d') != std::string::npos)
        ::sscanf(time.c_str(), "%dd %dh %dm %ds", &days, &hours, &minutes, &seconds);
    else if (time.find('h') != std::string::npos)
        ::sscanf(time.c_str(), "%dh %dm %ds", &hours, &minutes, &seconds);
    else if (time.find('m') != std::string::npos)
        ::sscanf(time.c_str(), "%dm %ds", &minutes, &seconds);
    else if (time.find('s') != std::string::npos)
        ::sscanf(time.c_str(), "%ds", &seconds);

    // Format the dhm time.
    char buffer[64];
    if (days > 0)
        ::sprintf(buffer, "%dd%dh%dm", days, hours, minutes);
    else if (hours > 0) {
        if (hours < 10 && minutes < 10 && seconds < 10)
            ::sprintf(buffer, "%dh%dm%ds", hours, minutes, seconds);
        else if (hours > 10 && minutes > 10 && seconds > 10)
            ::sprintf(buffer, "%dh%dm%ds", hours, minutes, seconds);
        else if ((minutes < 10 && seconds > 10) || (minutes > 10 && seconds < 10))
            ::sprintf(buffer, "%dh%dm%ds", hours, minutes, seconds);
        else
            ::sprintf(buffer, "%dh%dm%ds", hours, minutes, seconds);
    } else if (minutes > 0) {
        if (minutes > 10 && seconds > 10)
            ::sprintf(buffer, "%dm%ds", minutes, seconds);
        else
            ::sprintf(buffer, "%dm%ds", minutes, seconds);
    } else
        ::sprintf(buffer, "%ds", seconds);
    return std::string(buffer);
}


IMSlider::IMSlider(int lowerValue, int higherValue, int minValue, int maxValue, long style)
{
    m_lower_value  = lowerValue;
    m_higher_value = higherValue;
    m_min_value    = minValue;
    m_max_value    = maxValue;
    m_style        = style == wxSL_HORIZONTAL || style == wxSL_VERTICAL ? style : wxSL_HORIZONTAL;
    // BBS set to none style by default
    m_extra_style = style == wxSL_VERTICAL ? 0 : 0;
    m_selection   = ssHigher;
    m_is_need_post_tick_changed_event = false;
    m_tick_change_event_type = Type::Unknown;

    m_ticks.set_extruder_colors(&m_extruder_colors);
}

int IMSlider::GetActiveValue() const
{
    return m_selection == ssLower ?
    m_lower_value : m_selection == ssHigher ?
                m_higher_value : -1;
}


void IMSlider::SetLowerValue(const int lower_val)
{
    m_lower_value = lower_val;
    correct_lower_value();
    set_as_dirty();
}

void IMSlider::SetHigherValue(const int higher_val)
{
    m_higher_value = higher_val;
    correct_higher_value();
    set_as_dirty();
}

void IMSlider::SetSelectionSpan(const int lower_val, const int higher_val)
{
    m_lower_value  = std::max(lower_val, m_min_value);
    m_higher_value = std::max(std::min(higher_val, m_max_value), m_lower_value);
    if (m_lower_value < m_higher_value) m_is_one_layer = false;

    // ORCA reset single layer position when min max values changed
    // This will trigger when print height changed. but stays same on reslicing if layer count is same 
    m_one_layer_value = int((m_higher_value - m_lower_value)/2); 

    set_as_dirty();
}

void IMSlider::SetMaxValue(const int max_value)
{
    m_max_value = max_value;
    set_as_dirty();
}

void IMSlider::SetSliderValues(const std::vector<double> &values)
{
    m_values = values;
}

Info IMSlider::GetTicksValues() const
{
    Info                            custom_gcode_per_print_z;
    std::vector<CustomGCode::Item> &values = custom_gcode_per_print_z.gcodes;

    const int val_size = m_values.size();
    if (!m_values.empty())
        for (const TickCode &tick : m_ticks.ticks) {
            if (tick.tick > val_size) break;
            values.emplace_back(CustomGCode::Item{m_values[tick.tick], tick.type, tick.extruder, tick.color, tick.extra});
        }

    if (m_force_mode_apply) custom_gcode_per_print_z.mode = m_mode;

    return custom_gcode_per_print_z;
}

void IMSlider::SetTicksValues(const Info &custom_gcode_per_print_z)
{
    if (m_values.empty()) {
        m_ticks.mode = m_mode;
        return;
    }

    static bool last_spiral_vase_status = false;

    const bool was_empty = m_ticks.empty();

    m_ticks.ticks.clear();
    const std::vector<CustomGCode::Item> &heights = custom_gcode_per_print_z.gcodes;
    for (auto h : heights) {
        int tick = get_tick_from_value(h.print_z, true);
        if (tick >= 0) m_ticks.ticks.emplace(TickCode{tick, h.type, h.extruder, h.color, h.extra});
    }

    if (m_ticks.has_tick_with_code(ToolChange) && !m_can_change_color) {
        if (!wxGetApp().plater()->only_gcode_mode() && !wxGetApp().plater()->using_exported_file())
        {
            m_ticks.erase_all_ticks_with_code(ToolChange);
            post_ticks_changed_event();
        }
    }

    if (last_spiral_vase_status != m_is_spiral_vase) {
        last_spiral_vase_status = m_is_spiral_vase;
        if (!m_ticks.empty()) {
            m_ticks.ticks.clear();
            post_ticks_changed_event();
        }
    }

    //auto has_tick_execpt = [this](CustomGCode::Type type) {
    //    for (const TickCode& tick : m_ticks.ticks)
    //        if (tick.type != type) return true;

    //    return false;
    //};
    if ((!m_ticks.empty() /*&& has_tick_execpt(PausePrint)*/) && m_draw_mode == dmSequentialFffPrint) {
        for (auto it{ m_ticks.ticks.begin() }, end{ m_ticks.ticks.end() }; it != end;) {
            if (true/*it->type != PausePrint*/)
                it = m_ticks.ticks.erase(it);
            else
                ++it;
        }
        post_ticks_changed_event();
    }

    if (custom_gcode_per_print_z.mode && !custom_gcode_per_print_z.gcodes.empty()) m_ticks.mode = custom_gcode_per_print_z.mode;

    set_as_dirty();
}

void IMSlider::SetLayersTimes(const std::vector<float> &layers_times, float total_time)
{
    m_layers_times.clear();
    if (layers_times.empty()) return;
    m_layers_times.resize(layers_times.size(), 0.0);
    m_layers_times[0] = layers_times[0];
    for (size_t i = 1; i < layers_times.size(); i++) m_layers_times[i] = m_layers_times[i - 1] + layers_times[i];

    // Erase duplicates values from m_values and save it to the m_layers_values
    // They will be used for show the correct estimated time for MM print, when "No sparce layer" is enabled
    if (m_is_wipe_tower && m_values.size() != m_layers_times.size()) {
        m_layers_values = m_values;
        sort(m_layers_values.begin(), m_layers_values.end());
        m_layers_values.erase(unique(m_layers_values.begin(), m_layers_values.end()), m_layers_values.end());

        // When whipe tower is used to the end of print, there is one layer which is not marked in layers_times
        // So, add this value from the total print time value
        if (m_layers_values.size() != m_layers_times.size())
            for (size_t i = m_layers_times.size(); i < m_layers_values.size(); i++) m_layers_times.push_back(total_time);
        set_as_dirty();
        set_as_dirty();
    }
}

void IMSlider::SetLayersTimes(const std::vector<double> &layers_times)
{
    m_is_wipe_tower = false;
    m_layers_times  = layers_times;
    for (size_t i = 1; i < m_layers_times.size(); i++) m_layers_times[i] += m_layers_times[i - 1];
}

void IMSlider::SetDrawMode(bool is_sequential_print)
{
    m_draw_mode = is_sequential_print   ? dmSequentialFffPrint  :
                                          dmRegular;
    m_can_change_color = m_can_change_color && !(m_draw_mode == dmSequentialFffPrint);
}

void IMSlider::SetModeAndOnlyExtruder(const bool is_one_extruder_printed_model, const int only_extruder, bool can_change_color)
{
    m_mode = !is_one_extruder_printed_model ? MultiExtruder : only_extruder < 0 ? SingleExtruder : MultiAsSingle;
    if (!m_ticks.mode || (m_ticks.empty() && m_ticks.mode != m_mode)) m_ticks.mode = m_mode;
    m_only_extruder = only_extruder;

    UseDefaultColors(m_mode == SingleExtruder);

    m_is_wipe_tower = m_mode != SingleExtruder;

    auto config = wxGetApp().preset_bundle->full_config();
    m_is_spiral_vase = config.option<ConfigOptionBool>("spiral_mode")->value;

    m_can_change_color = can_change_color && !m_is_spiral_vase;

    // close opened menu window after reslice
    m_show_menu = false;
    m_show_custom_gcode_window = false;
    m_show_go_to_layer_dialog = false;
}

void IMSlider::SetExtruderColors( const std::vector<std::string>& extruder_colors)
{
    m_extruder_colors = extruder_colors;
}

bool IMSlider::IsNewPrint()
{
    const Print &print = GUI::wxGetApp().plater()->fff_print();
    std::string  idxs;
    for (auto object : print.objects()) idxs += std::to_string(object->id().id) + "_";

    if (idxs == m_print_obj_idxs) return false;

    m_print_obj_idxs = idxs;
    return true;
}

void IMSlider::post_ticks_changed_event(Type type)
{
    m_tick_change_event_type = type;
    m_is_need_post_tick_changed_event = true;
}

void IMSlider::add_custom_gcode(std::string custom_gcode)
{
    if (m_selection == ssUndef) return;
    const int tick = m_selection == ssLower ? m_lower_value : m_higher_value;

    const auto it = m_ticks.ticks.find(TickCode{ tick });

    if (it != m_ticks.ticks.end()) {
        m_ticks.ticks.erase(it);
    }
    m_ticks.ticks.emplace(TickCode{ tick, Custom, std::max<int>(1, m_only_extruder), "", custom_gcode });

    post_ticks_changed_event(Custom);
}

void IMSlider::add_code_as_tick(Type type, int selected_extruder)
{
    if (m_selection == ssUndef) return;
    const int tick = m_selection == ssLower ? m_lower_value : m_higher_value;

    if (!check_ticks_changed_event(type)) {
        BOOST_LOG_TRIVIAL(trace) << "check ticks change event false";
        return;
    }

    if (type == ColorChange && gcode(ColorChange).empty()) GUI::wxGetApp().plater()->get_notification_manager()->push_notification(GUI::NotificationType::EmptyColorChangeCode);

    const int  extruder = selected_extruder > 0 ? selected_extruder : std::max<int>(1, m_only_extruder);
    const auto it       = m_ticks.ticks.find(TickCode{tick});

    if (it == m_ticks.ticks.end()) {
        // try to add tick
        if (!m_ticks.add_tick(tick, type, extruder, m_values[tick])) return;
    } else if (type == ToolChange || type == ColorChange) {
        // try to switch tick code to ToolChange or ColorChange accordingly
        if (!m_ticks.switch_code_for_tick(it, type, extruder)) return;
    } else
        return;

    post_ticks_changed_event(type);
}

void IMSlider::delete_tick(const TickCode& tick) {
    Type t = tick.type; // Avoid Use-After-Free issue, which resets the tick.type to 0
    m_ticks.ticks.erase(tick);
    post_ticks_changed_event(t);
}

bool IMSlider::check_ticks_changed_event(Type type)
{
    //BBL only support MultiExtruder
    if (m_ticks.mode == m_mode || (type != ColorChange && type != ToolChange) ||
        (m_ticks.mode == SingleExtruder && m_mode == MultiAsSingle) || // All ColorChanges will be applied for 1st extruder
        (m_ticks.mode == MultiExtruder && m_mode == MultiAsSingle))    // Just mark ColorChanges for all unused extruders
        return true;

    if ((m_ticks.mode == SingleExtruder && m_mode == MultiExtruder) || (m_ticks.mode == MultiExtruder && m_mode == SingleExtruder)) {
        if (!m_ticks.has_tick_with_code(ColorChange)) return true;
        /*
        wxString message = (m_ticks.mode == SingleExtruder ? _L("The last color change data was saved for a single extruder printing.") :
                                                             _L("The last color change data was saved for a multi extruder printing.")) +
                           "\n" + _L("Your current changes will delete all saved color changes.") + "\n\n\t" + _L("Are you sure you want to continue?");


        GUI::MessageDialog msg(this, message, _L("Notice"), wxYES_NO);
        if (msg.ShowModal() == wxID_YES) {
            m_ticks.erase_all_ticks_with_code(ColorChange);
            post_ticks_changed_event(ColorChange);
        }
        */
        return false;
    }

    return true;
}


// switch on/off one layer mode
bool IMSlider::switch_one_layer_mode()
{
    if (m_show_custom_gcode_window)
        return false;

    m_is_one_layer = !m_is_one_layer;
    if (!m_is_one_layer) {                       // DEACTIVATE
        m_one_layer_value = GetHigherValue();    // ORCA Backup value on deactivate
        SetLowerValue(m_min_value);
        SetHigherValue(m_max_value);             // Higher value resets on toggling off one layer mode to show whole model
    }else{                                       // ACTIVATE
                                                 // ORCA Ensure value fits range. value set in IMSlider::SetSelectionSpan but added this just in case
        if(!m_one_layer_value || m_one_layer_value > m_max_value || m_one_layer_value < m_min_value){
            m_one_layer_value = int((m_max_value - m_min_value)/2);
            SetHigherValue(m_one_layer_value);  
        }
        else if(GetHigherValue() == m_max_value) // ORCA Prefer backup value if higher value reseted
            SetHigherValue(m_one_layer_value);   // ORCA Restore value
        else                                     // ORCA Prefer higher value if user changed higher value. so it will show section on same view
            SetHigherValue(GetHigherValue());    // ORCA use same position with higher value if user changed its position. visible section stays same when switching one layer mode with this
    }
    if (m_selection == ssUndef) m_selection = ssHigher;
    set_as_dirty();
    return true;
}

bool IMSlider::is_dragging() const
{
    return GImGui != nullptr && m_imgui_id != 0 && GImGui->ActiveId == m_imgui_id && GImGui->IO.MouseDown[0];
}

void draw_concave_corner(ImDrawList* draw_list, const ImVec2& corner, float dx, float dy, float radius, ImU32 color)
{
    const ImVec2 center(corner.x + dx * radius, corner.y + dy * radius);
    const float  from = std::atan2(0.f, -dx);
    float        to   = std::atan2(-dy, 0.f);
    if (to - from > IM_PI)
        to -= 2.f * IM_PI;
    else if (from - to > IM_PI)
        to += 2.f * IM_PI;
    const float outer = radius * 1.4143f + 1.f;
    draw_list->PushClipRect(ImVec2(std::min(corner.x, center.x), std::min(corner.y, center.y)),
                            ImVec2(std::max(corner.x, center.x), std::max(corner.y, center.y)), true);
    draw_list->PathArcTo(center, 0.5f * (radius + outer), from - 0.1f * (to - from), to + 0.1f * (to - from), 16);
    draw_list->PathStroke(color, 0, outer - radius);
    draw_list->PopClipRect();
}

static void draw_rail_handle(ImDrawList* draw_list, const ImVec2& center, float radius, float scale, bool active)
{
    draw_list->AddCircleFilled(center, radius, HANDLE_RING_COLOR);
    draw_list->AddCircleFilled(center, radius - 2.f * scale, BRAND_COLOR);
    if (active) {
        const float half = 0.5f * radius;
        draw_list->AddLine(center - ImVec2(half, 0.f), center + ImVec2(half, 0.f), HANDLE_RING_COLOR, 1.5f * scale);
        draw_list->AddLine(center - ImVec2(0.f, half), center + ImVec2(0.f, half), HANDLE_RING_COLOR, 1.5f * scale);
    }
}

bool IMSlider::horizontal_slider(const char* str_id, int* value, int v_min, int v_max)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& context = *GImGui;
    const ImGuiID id = window->GetID(str_id);
    m_imgui_id = id;

    const ImRect draw_region(window->Pos, window->Pos + window->Size);
    ImGui::ItemSize(draw_region);

    const float  radius = m_layout.handle_radius;
    const float  mid_y  = m_layout.bottom_rail_y;
    const ImRect region(ImVec2(m_layout.bottom_rail_left, mid_y - radius), ImVec2(m_layout.bottom_rail_right, mid_y + radius));
    const ImRect track(ImVec2(region.Min.x, mid_y - 0.5f * m_layout.track_thickness),
                       ImVec2(region.Max.x, mid_y + 0.5f * m_layout.track_thickness));

    if (ImGui::ItemHoverable(draw_region, id) && context.IO.MouseDown[0]) {
        ImGui::SetActiveID(id, window);
        ImGui::SetFocusID(id, window);
        ImGui::FocusWindow(window);
    }

    const float handle_pos = get_pos_from_value(v_min, v_max, *value, region);
    ImRect      handle(handle_pos - radius, mid_y - radius, handle_pos + radius, mid_y + radius);
    const bool  value_changed = slider_behavior(id, region, (const ImS32) v_min, (const ImS32) v_max, (ImS32 *) value, &handle);
    const ImVec2 center = handle.GetCenter();

    ImDrawList* draw_list = window->DrawList;
    const float rounding  = 0.5f * track.GetHeight();
    draw_list->AddRectFilled(track.Min, track.Max, TRACK_COLOR, rounding);
    draw_list->AddRectFilled(track.Min, ImVec2(center.x, track.Max.y), BRAND_COLOR, rounding);
    draw_rail_handle(draw_list, center, radius, m_layout.scale, false);

    return value_changed;
}

void IMSlider::draw_colored_band(const ImRect& groove, const ImRect& slideable_region) {
    if (!m_ticks.has_tick_with_code(ToolChange))
        return;

    ImRect main_band = groove;

    auto draw_band = [this](const ImU32& clr, const ImRect& band_rc)
    {
        if (clr == (m_is_dark ? BACKGROUND_COLOR_DARK : BACKGROUND_COLOR_LIGHT)) {
            ImRect rc = band_rc;
            rc.Min += ImVec2(1, 1) * m_scale;
            rc.Max -= ImVec2(1, 1) * m_scale;
            ImGui::RenderFrame(band_rc.Min, band_rc.Max, m_is_dark ? GROOVE_COLOR_DARK : GROOVE_COLOR_LIGHT, false, band_rc.GetWidth() * 0.5);
            //cover round corner
            ImGui::RenderFrame(ImVec2(band_rc.Min.x, band_rc.Max.y - band_rc.GetWidth() * 0.5), band_rc.Max, m_is_dark ? GROOVE_COLOR_DARK : GROOVE_COLOR_LIGHT, false);

            ImGui::RenderFrame(rc.Min, rc.Max, clr, false, rc.GetWidth() * 0.5);
            //cover round corner
            ImGui::RenderFrame(ImVec2(rc.Min.x, rc.Max.y - rc.GetWidth() * 0.5), rc.Max, clr, false);
        }
        else {
            ImGui::RenderFrame(band_rc.Min, band_rc.Max, clr, false, band_rc.GetWidth() * 0.5);
            //cover round corner
            ImGui::RenderFrame(ImVec2(band_rc.Min.x, band_rc.Max.y - band_rc.GetWidth() * 0.5), band_rc.Max, clr, false);
        }
    };
    auto draw_main_band = [&main_band, this](const ImU32& clr) {
        if (clr == (m_is_dark ? BACKGROUND_COLOR_DARK : BACKGROUND_COLOR_LIGHT)) {
            ImRect rc = main_band;
            rc.Min += ImVec2(1, 1) * m_scale;
            rc.Max -= ImVec2(1, 1) * m_scale;
            ImGui::RenderFrame(main_band.Min, main_band.Max, m_is_dark ? GROOVE_COLOR_DARK : GROOVE_COLOR_LIGHT, false, main_band.GetWidth() * 0.5);
            ImGui::RenderFrame(rc.Min, rc.Max, clr, false, rc.GetWidth() * 0.5);
        }
        else {
            ImGui::RenderFrame(main_band.Min, main_band.Max, clr, false, main_band.GetWidth() * 0.5);
        }
    };
    //draw main colored band
    const int default_color_idx = m_mode == MultiAsSingle ? std::max<int>(m_only_extruder - 1, 0) : 0;
    ColorRGBA rgba = decode_color_to_float_array(m_extruder_colors[default_color_idx]);
    ImU32     band_clr          = ImGuiWrapper::to_ImU32(rgba);
    draw_main_band(band_clr);

    static float tick_pos;
    std::set<TickCode>::const_iterator tick_it = m_ticks.ticks.begin();
    while (tick_it != m_ticks.ticks.end())
    {
        //get position from tick
        tick_pos = get_pos_from_value(GetMinValue(), GetMaxValue(), tick_it->tick, slideable_region);
        //draw colored band
        if (tick_it->type == ToolChange) {
            if ((m_mode == SingleExtruder) || (m_mode == MultiAsSingle))
            {
                ImRect band_rect = ImRect(main_band.Min, ImVec2(main_band.Max.x, tick_pos));

                const std::string clr_str = m_mode == SingleExtruder ? tick_it->color : get_color_for_tool_change_tick(tick_it);

                if (!clr_str.empty()) {
                    ColorRGBA rgba = decode_color_to_float_array(clr_str);
                    ImU32     band_clr = ImGuiWrapper::to_ImU32(rgba);
                    if (tick_it->tick == 0)
                        draw_main_band(band_clr);
                    else
                        draw_band(band_clr, band_rect);
                }
            }
        }
        tick_it++;
    }
}

ImU32 IMSlider::tick_color(std::set<TickCode>::const_iterator it) const
{
    if (it->type == Custom || it->type == Template)
        return CUSTOM_GCODE_COLOR;
    if (it->type == PausePrint)
        return PAUSE_COLOR;
    const std::string color = it->type == ToolChange && m_mode != SingleExtruder ? get_color_for_tool_change_tick(it) : it->color;
    return color.empty() ? IM_COL32(144, 144, 144, 255) : ImGuiWrapper::to_ImU32(decode_color_to_float_array(color));
}

static void draw_mark_icon(ImDrawList* draw_list, const ImVec2& center, Type type, ImU32 color, float radius, float scale)
{
    switch (type) {
    case PausePrint: {
        const ImVec2 bar(0.3f * radius, radius);
        draw_list->AddRectFilled(center + ImVec2(-1.6f * bar.x, -bar.y), center + ImVec2(-0.6f * bar.x, bar.y), color, scale);
        draw_list->AddRectFilled(center + ImVec2(0.6f * bar.x, -bar.y), center + ImVec2(1.6f * bar.x, bar.y), color, scale);
        break;
    }
    case Custom:
    case Template:
        draw_list->AddQuadFilled(center + ImVec2(0.f, -radius), center + ImVec2(radius, 0.f), center + ImVec2(0.f, radius),
                                 center + ImVec2(-radius, 0.f), color);
        break;
    default: draw_list->AddCircleFilled(center, radius, color); break;
    }
}

void IMSlider::draw_ticks(const ImRect& slideable_region)
{
    m_mark_hovered = false;
    m_tick_value   = -1;
    m_tick_rect    = ImVec4();
    if (m_ticks.empty())
        return;

    ImDrawList* draw_list  = ImGui::GetWindowDrawList();
    const float scale      = m_layout.scale;
    const float mid_x      = m_layout.side_rail_x;
    const float half_hit   = 0.5f * m_layout.band;
    const float dot_radius = 4.5f * scale;
    const float badge_h    = ImGui::GetTextLineHeight();

    std::vector<std::set<TickCode>::const_iterator> ticks;
    std::vector<float>                              positions;
    for (auto it = m_ticks.ticks.begin(); it != m_ticks.ticks.end(); ++it) {
        ticks.push_back(it);
        positions.push_back(get_pos_from_value(GetMinValue(), GetMaxValue(), it->tick, slideable_region));
    }

    for (const TickGroup& group : group_ticks_by_position(positions, badge_h + 2.f * scale)) {
        const float  y      = group.pos;
        const float  half_h = group.count > 1 ? 0.5f * badge_h : dot_radius + 2.f * scale;
        const ImVec2 hover_min(mid_x - half_hit, y - half_h - 2.f * scale);
        const ImVec2 hover_max(mid_x + half_hit, y + half_h + 2.f * scale);

        if (group.count > 1) {
            const std::string label  = std::to_string(group.count);
            const ImVec2      text   = ImGui::CalcTextSize(label.c_str());
            const float       half_w = 0.5f * (text.x + 8.f * scale);
            const float       half_h = 0.5f * (text.y + 2.f * scale);
            draw_list->AddRectFilled(ImVec2(mid_x - half_w, y - half_h), ImVec2(mid_x + half_w, y + half_h), HANDLE_RING_COLOR, 3.f * scale);
            draw_list->AddText(ImVec2(mid_x - 0.5f * text.x, y - 0.5f * text.y), BADGE_TEXT_COLOR, label.c_str());
        } else {
            const auto it = ticks[group.first];
            if (it->type == PausePrint) {
                const float half_bar = half_hit - 2.f * scale;
                draw_list->AddRectFilled(ImVec2(mid_x - half_bar, y - 2.f * scale), ImVec2(mid_x + half_bar, y + 2.f * scale), PAUSE_COLOR,
                                         2.f * scale);
            } else {
                draw_list->AddCircleFilled(ImVec2(mid_x, y), dot_radius + 1.5f * scale, BADGE_TEXT_COLOR);
                draw_mark_icon(draw_list, ImVec2(mid_x, y), it->type, tick_color(it), dot_radius, scale);
            }
        }

        if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(hover_min, hover_max, false)) {
            m_mark_hovered = true;
            m_marks_popup.open     = true;
            m_marks_popup.anchor   = ImVec2(hover_min.x, y);
            m_marks_popup.mark_min = hover_min;
            m_marks_popup.mark_max = hover_max;
            m_marks_popup.ticks.clear();
            for (size_t i = group.first + group.count; i > group.first; --i)
                m_marks_popup.ticks.push_back(ticks[i - 1]->tick);
            if (group.count == 1) {
                m_tick_value = ticks[group.first]->tick;
                m_tick_rect  = ImVec4(hover_min.x, hover_min.y, hover_max.x, hover_max.y);
            }
        }
    }
}

void IMSlider::show_tooltip(const std::string tooltip) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 6 * m_scale, 3 * m_scale });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3 * m_scale);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGuiWrapper::COL_WINDOW_BACKGROUND);
    ImGui::PushStyleColor(ImGuiCol_Border, { 0,0,0,0 });
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(tooltip.c_str());
    ImGui::EndTooltip();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

int IMSlider::get_tick_near_point(int v_min, int v_max, const ImVec2& pt, const ImRect& rect) {
    ImS32 v_range = (v_min < v_max ? v_max - v_min : v_min - v_max);
    
    const ImGuiAxis axis = is_horizontal() ? ImGuiAxis_X : ImGuiAxis_Y;
    const float region_usable_sz = (rect.Max[axis] - rect.Min[axis]);
    const float region_usable_pos_min = rect.Min[axis];
    
    const float abs_pos = pt[axis];
    
    float pos_ratio = (region_usable_sz > 0.0f) ? ImClamp((abs_pos - region_usable_pos_min) / region_usable_sz, 0.0f, 1.0f) : 0.0f;
    if (axis == ImGuiAxis_Y)
        pos_ratio = 1.0f - pos_ratio;
    
    return v_min + (ImS32)(v_range * pos_ratio + 0.5f);
}

void IMSlider::draw_tick_on_mouse_position(const ImRect& slideable_region) {
    int v_min = GetMinValue();
    int v_max = GetMaxValue();
    ImGuiContext& context = *GImGui;

    const int   tick     = get_tick_near_point(v_min, v_max, context.IO.MousePos, slideable_region);
    const float tick_pos = get_pos_from_value(v_min, v_max, tick, slideable_region);
    const float half     = 0.35f * m_layout.band;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(m_layout.side_rail_x - half, tick_pos), ImVec2(m_layout.side_rail_x + half, tick_pos),
                                        IM_COL32(144, 144, 144, 255), m_layout.scale);

    const std::string label = get_label(tick, ltEstimatedTime);
    if (!label.empty())
        show_tooltip(label);
}

bool IMSlider::vertical_slider(const char* str_id, int* higher_value, int* lower_value, int v_min, int v_max, SelectedSlider& selection, bool one_layer_flag)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& context = *GImGui;
    const ImGuiID id = window->GetID(str_id);
    m_imgui_id = id;

    const ImRect draw_region(window->Pos, window->Pos + window->Size);
    ImGui::ItemSize(draw_region);

    const float  radius = m_layout.handle_radius;
    const float  mid_x  = m_layout.side_rail_x;
    const float  top    = m_layout.side_rail_top;
    const float  bottom = m_layout.side_rail_bottom;
    const ImRect track(ImVec2(mid_x - 0.5f * m_layout.track_thickness, top), ImVec2(mid_x + 0.5f * m_layout.track_thickness, bottom));
    const ImRect higher_region(ImVec2(mid_x - radius, top), ImVec2(mid_x + radius, bottom - radius));
    const ImRect lower_region(ImVec2(mid_x - radius, top + radius), ImVec2(mid_x + radius, bottom));
    const ImRect one_region(ImVec2(mid_x - radius, top), ImVec2(mid_x + radius, bottom));

    auto handle_at = [&](int value, const ImRect& region) {
        const float pos = get_pos_from_value(v_min, v_max, value, region);
        return ImRect(mid_x - radius, pos - radius, mid_x + radius, pos + radius);
    };

    const bool menu_open      = ImGui::IsPopupOpen("slider_add_menu_popup") || ImGui::IsPopupOpen("slider_edit_menu_popup");
    const ImRect grab_region(ImVec2(draw_region.Min.x, m_layout.top_toggle_square.bottom), ImVec2(draw_region.Max.x, m_layout.bottom_toggle_square.top));
    const bool   slider_hovered = !menu_open && ImGui::ItemHoverable(grab_region, id);
    if (slider_hovered && context.IO.MouseDown[0]) {
        ImGui::SetActiveID(id, window);
        ImGui::SetFocusID(id, window);
        ImGui::FocusWindow(window);
    }

    ImDrawList* draw_list = window->DrawList;
    draw_list->AddRectFilled(track.Min, track.Max, TRACK_COLOR, 0.5f * track.GetWidth());

    auto update_menu = [&](const ImRect& handle) {
        if (!menu_open && ImGui::ItemHoverable(handle, id) && context.IO.MouseClicked[1])
            m_show_menu = true;
        if (!menu_open && ((!ImGui::ItemHoverable(handle, id) && context.IO.MouseClicked[1]) || context.IO.MouseClicked[0]))
            m_show_menu = false;
    };

    bool value_changed = false;
    if (!one_layer_flag) {
        ImRect higher_handle = handle_at(*higher_value, higher_region);
        ImRect lower_handle  = handle_at(*lower_value, lower_region);
        if (!menu_open) {
            if (ImGui::ItemHoverable(higher_handle, id) && context.IO.MouseClicked[0])
                selection = ssHigher;
            if (ImGui::ItemHoverable(lower_handle, id) && context.IO.MouseClicked[0])
                selection = ssLower;
        }
        const bool h_selected = selection != ssLower;
        if (h_selected)
            value_changed = slider_behavior(id, higher_region, v_min, v_max, higher_value, &higher_handle, ImGuiSliderFlags_Vertical,
                                            m_tick_value, m_tick_rect);
        else
            value_changed = slider_behavior(id, lower_region, v_min, v_max, lower_value, &lower_handle, ImGuiSliderFlags_Vertical,
                                            m_tick_value, m_tick_rect);

        ImVec2 higher_center = higher_handle.GetCenter();
        ImVec2 lower_center  = lower_handle.GetCenter();
        if (higher_center.y + radius > lower_center.y && h_selected) {
            lower_handle = higher_handle;
            lower_handle.TranslateY(radius);
            lower_center.y = higher_center.y + radius;
            *lower_value   = *higher_value;
        }
        if (higher_center.y + radius > lower_center.y && !h_selected) {
            higher_handle = lower_handle;
            higher_handle.TranslateY(-radius);
            higher_center.y = lower_center.y - radius;
            *higher_value   = *lower_value;
        }

        update_menu(h_selected ? higher_handle : lower_handle);

        const ImRect& region = h_selected ? higher_region : lower_region;
        draw_colored_band(track, region);
        if (!m_ticks.has_tick_with_code(ToolChange))
            draw_list->AddRectFilled(ImVec2(track.Min.x, higher_center.y), ImVec2(track.Max.x, lower_center.y), BRAND_COLOR,
                                     0.5f * track.GetWidth());
        draw_ticks(region);

        draw_rail_handle(draw_list, higher_center, radius, m_layout.scale, h_selected);
        draw_rail_handle(draw_list, lower_center, radius, m_layout.scale, !h_selected);

        if (slider_hovered && !context.IO.MouseDown[0] && !m_mark_hovered && !m_marks_popup.open)
            draw_tick_on_mouse_position(region);
    } else {
        ImRect one_handle = handle_at(*higher_value, one_region);
        value_changed     = slider_behavior(id, one_region, v_min, v_max, higher_value, &one_handle, ImGuiSliderFlags_Vertical, m_tick_value,
                                            m_tick_rect);
        update_menu(one_handle);

        draw_colored_band(track, one_region);
        draw_ticks(one_region);
        draw_rail_handle(draw_list, one_handle.GetCenter(), radius, m_layout.scale, true);

        if (slider_hovered && !context.IO.MouseDown[0] && !m_mark_hovered && !m_marks_popup.open)
            draw_tick_on_mouse_position(one_region);
    }

    return value_changed;
}

IMSlider::PillText IMSlider::pill_text(int tick)
{
    static constexpr const char* NUMBER_MARK = "\x1f";
    const std::string layer = format(_u8L("Layer %1%"), NUMBER_MARK);
    const size_t      split = layer.find(NUMBER_MARK);

    PillText text;
    text.prefix = layer.substr(0, split);
    text.suffix = split == std::string::npos ? "" : layer.substr(split + std::strlen(NUMBER_MARK));
    text.number = get_label(tick, ltLayer);
    text.z      = format(_u8L("%1% mm"), get_label(tick, ltHeight));
    return text;
}

IMSlider::PillSlots IMSlider::pill_slots()
{
    ImGuiWrapper&  imgui = *wxGetApp().imgui();
    const PillText last  = pill_text(GetMaxValue());
    PillSlots      slots;
    slots.prefix = ImGui::CalcTextSize(last.prefix.c_str()).x;
    slots.suffix = ImGui::CalcTextSize(last.suffix.c_str()).x;
    slots.z      = ImGui::CalcTextSize(last.z.c_str()).x;
    const bool bold = imgui.push_bold_font();
    slots.number    = ImGui::CalcTextSize(last.number.c_str()).x;
    if (bold)
        imgui.pop_bold_font();
    const float space = ImGui::CalcTextSize(" ").x;
    slots.width       = slots.prefix + slots.number + slots.suffix + 3.f * space + ImGui::CalcTextSize("Z").x + space + slots.z;
    return slots;
}

ImVec2 IMSlider::pill_size(float scale)
{
    return ImVec2(std::ceil(pill_slots().width + 20.f * scale), std::ceil(ImGui::GetTextLineHeight() + 12.f * scale));
}

static void draw_single_layer_icon(ImDrawList* draw_list, const ImVec2& center, float scale, ImU32 color, bool filled)
{
    const float w     = 7.f * scale;
    const float h     = 3.5f * scale;
    const float step  = 3.5f * scale;
    const float top   = center.y - 3.5f * scale;
    const float thick = 1.4f * scale;

    for (int i = 2; i >= 1; --i) {
        const float  y       = top + step * float(i);
        const ImVec2 edge[3] = {ImVec2(center.x - w, y), ImVec2(center.x, y + h), ImVec2(center.x + w, y)};
        draw_list->AddPolyline(edge, 3, color, 0, thick);
    }
    const ImVec2 plate[4] = {ImVec2(center.x, top - h), ImVec2(center.x + w, top), ImVec2(center.x, top + h), ImVec2(center.x - w, top)};
    if (filled)
        draw_list->AddConvexPolyFilled(plate, 4, color);
    else
        draw_list->AddPolyline(plate, 4, color, ImDrawFlags_Closed, thick);
}

void IMSlider::render_single_layer_button(float size)
{
    const float scale = m_layout.scale;
    const bool  on    = is_one_layer();
    if (ImGui::InvisibleButton("##single_layer", ImVec2(size, size)))
        switch_one_layer_mode();
    const bool   hovered  = ImGui::IsItemHovered();
    const ImVec2 min      = ImGui::GetItemRectMin();
    const ImVec2 max      = ImGui::GetItemRectMax();
    const float  rounding = 4.f * scale;
    const ImVec4 text     = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImDrawList*  draw_list = ImGui::GetWindowDrawList();
    if (on)
        draw_list->AddRectFilled(min, max, hovered ? BRAND_HOVER_COLOR : BRAND_COLOR, rounding);
    else if (hovered)
        draw_list->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBgHovered), rounding);
    draw_single_layer_icon(draw_list, (min + max) * 0.5f, scale,
                           on ? HANDLE_RING_COLOR : ImGui::GetColorU32(ImVec4(text.x, text.y, text.z, 0.85f)), on);
    if (hovered)
        show_tooltip(_u8L("Single layer"));
}

bool IMSlider::render_pill(const char* str_id, int tick, const CanvasUi::Rect& rect, int corners)
{
    ImGuiWrapper&   imgui = *wxGetApp().imgui();
    const float     scale = m_layout.scale;
    const float     space = ImGui::CalcTextSize(" ").x;
    const PillText  text  = pill_text(tick);
    const PillSlots slots = pill_slots();
    const ImVec2    size(rect.width(), rect.height());

    ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    background.w      = 0.8f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    imgui.set_next_window_pos(rect.left, rect.top, ImGuiCond_Always, 0.f, 0.f);
    ImGui::SetNextWindowSize(size);
    imgui.begin(std::string(str_id), ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList*  draw_list = ImGui::GetWindowDrawList();
    const ImVec2 origin    = ImGui::GetCursorScreenPos();
    draw_list->AddRectFilled(origin, origin + size, ImGui::GetColorU32(background), 10.f * scale, corners);

    const bool clicked = ImGui::InvisibleButton("##pill", size);
    if (ImGui::IsItemHovered())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const ImVec4 text_color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const ImU32  muted      = ImGui::GetColorU32(ImVec4(text_color.x, text_color.y, text_color.z, 0.65f));
    const ImU32  normal     = ImGui::GetColorU32(ImGuiCol_Text);
    ImVec2       cursor(origin.x + std::floor(0.5f * (size.x - slots.width)), origin.y + std::floor(0.5f * (size.y - ImGui::GetTextLineHeight())));
    draw_list->AddText(ImVec2(cursor.x + slots.prefix - ImGui::CalcTextSize(text.prefix.c_str()).x, cursor.y), muted, text.prefix.c_str());
    cursor.x += slots.prefix;
    const bool  bold        = imgui.push_bold_font();
    const float number_text = ImGui::CalcTextSize(text.number.c_str()).x;
    draw_list->AddText(ImVec2(cursor.x + slots.number - number_text, cursor.y), normal, text.number.c_str());
    if (bold)
        imgui.pop_bold_font();
    cursor.x += slots.number;
    draw_list->AddText(cursor, muted, text.suffix.c_str());
    cursor.x += slots.suffix + 3.f * space;
    draw_list->AddText(cursor, ImGuiWrapper::to_ImU32(ColorRGBA::Z()), "Z");
    cursor.x += ImGui::CalcTextSize("Z").x + space;
    draw_list->AddText(ImVec2(cursor.x + slots.z - ImGui::CalcTextSize(text.z.c_str()).x, cursor.y), normal, text.z.c_str());

    const ImVec2 pos = ImGui::GetWindowPos();
    m_wheel_areas.push_back({pos.x, pos.y, pos.x + size.x, pos.y + size.y});
    imgui.end();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    return clicked;
}

void IMSlider::move_active_handle_to(int tick)
{
    if (m_is_one_layer) {
        SetHigherValue(tick);
        m_one_layer_value = tick;
    } else if (m_selection == ssLower)
        SetLowerValue(tick);
    else {
        m_selection = ssHigher;
        SetHigherValue(tick);
    }
}

static std::string mark_type_name(Type type)
{
    switch (type) {
    case PausePrint: return _u8L("Pause");
    case ColorChange: return _u8L("Color change");
    case ToolChange: return _u8L("Filament change");
    case Custom: return _u8L("Custom G-code");
    case Template: return _u8L("Template");
    default: return {};
    }
}

void IMSlider::render_marks_popup()
{
    MarksPopup& popup = m_marks_popup;
    if (!popup.open)
        return;

    popup.ticks.erase(std::remove_if(popup.ticks.begin(), popup.ticks.end(),
                                     [this](int tick) { return m_ticks.ticks.find(TickCode{tick}) == m_ticks.ticks.end(); }),
                      popup.ticks.end());

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (!m_mark_hovered && !popup.submenu_open) {
        const ImRect window_rect(popup.window_min, popup.window_max);
        const ImRect bridge(ImVec2(popup.window_max.x - 1.f, std::min(popup.mark_min.y, popup.window_min.y)),
                            ImVec2(popup.mark_max.x, std::max(popup.mark_max.y, popup.window_max.y)));
        if (popup.ticks.empty() || (!window_rect.Contains(mouse) && !bridge.Contains(mouse))) {
            popup = MarksPopup();
            return;
        }
    }

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    const float   scale = m_layout.scale;
    const float   line  = ImGui::GetTextLineHeight();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f, 6.f) * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 4.f) * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.f, 2.f) * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
    ImGui::SetNextWindowBgAlpha(0.9f);
    imgui.set_next_window_pos(popup.anchor.x - m_layout.gap, popup.anchor.y, ImGuiCond_Always, 1.f, 0.5f);
    imgui.begin(std::string("layers_slider_marks"), ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    int      jump_to = -1;
    int      edit_custom = -1;
    bool     open_filaments = false;
    TickCode to_delete{-1};
    const ImU32       icon_hover = ImGui::GetColorU32(ImGuiCol_FrameBgHovered);
    const ImU32       text_color = ImGui::GetColorU32(ImGuiCol_Text);

    std::vector<std::string> times;
    float                    time_w = 0.f;
    for (int tick : popup.ticks) {
        times.push_back(tick > 0 ? get_label(tick - 1, ltEstimatedTime) : "");
        time_w = std::max(time_w, ImGui::CalcTextSize(times.back().c_str()).x);
    }
    const int actions_column = time_w > 0.f ? 4 : 3;

    if (ImGui::BeginTable("##marks", actions_column + 1, ImGuiTableFlags_SizingFixedFit)) {
        for (size_t row = 0; row < popup.ticks.size(); ++row) {
            const int  tick = popup.ticks[row];
            const auto it   = m_ticks.ticks.find(TickCode{tick});
            ImGui::PushID(tick);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            const float  row_top     = ImGui::GetCursorScreenPos().y;
            const ImVec2 icon_center = ImGui::GetCursorScreenPos() + ImVec2(0.5f * line, 0.5f * line);
            ImGui::Dummy(ImVec2(line, line));
            draw_mark_icon(ImGui::GetWindowDrawList(), icon_center, it->type, tick_color(it), 0.32f * line, scale);

            ImGui::TableSetColumnIndex(1);
            const bool bold = imgui.push_bold_font();
            ImGui::TextUnformatted(mark_type_name(it->type).c_str());
            if (bold)
                imgui.pop_bold_font();

            ImGui::TableSetColumnIndex(2);
            // TRN Where a mark on the layer slider stands: its layer number and its height in millimeters.
            const std::string where = format(_u8L("%1% · %2% mm"), get_label(tick, ltLayer), get_label(tick, ltHeight));
            ImGui::TextDisabled("%s", where.c_str());

            if (time_w > 0.f) {
                ImGui::TableSetColumnIndex(3);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + time_w - ImGui::CalcTextSize(times[row].c_str()).x);
                ImGui::TextDisabled("%s", times[row].c_str());
            }

            ImGui::TableSetColumnIndex(actions_column);
            const bool can_edit = m_menu_enable && (it->type == Custom ||
                                                    (it->type == ToolChange && m_extruder_colors.size() > 1 && m_can_change_color));
            if (can_edit) {
                if (ImGui::InvisibleButton("##edit", ImVec2(line, line))) {
                    if (it->type == Custom)
                        edit_custom = tick;
                    else {
                        popup.filament_tick = tick;
                        open_filaments      = true;
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), icon_hover, 3.f * scale);
                const std::string pencil = into_u8(ImGui::SliderFloatEditBtnIcon);
                const ImVec2      size   = ImGui::CalcTextSize(pencil.c_str());
                ImGui::GetWindowDrawList()->AddText((ImGui::GetItemRectMin() + ImGui::GetItemRectMax() - size) * 0.5f, text_color,
                                                    pencil.c_str());
            } else if (m_menu_enable)
                ImGui::Dummy(ImVec2(line, line));
            if (m_menu_enable) {
                ImGui::SameLine();
                if (ImGui::InvisibleButton("##delete", ImVec2(line, line)))
                    to_delete = *it;
                const ImVec2 min = ImGui::GetItemRectMin();
                const ImVec2 max = ImGui::GetItemRectMax();
                if (ImGui::IsItemHovered())
                    ImGui::GetWindowDrawList()->AddRectFilled(min, max, icon_hover, 3.f * scale);
                const ImVec2 c    = (min + max) * 0.5f;
                const float  half = 0.22f * line;
                ImGui::GetWindowDrawList()->AddLine(c - ImVec2(half, half), c + ImVec2(half, half), text_color, 1.5f * scale);
                ImGui::GetWindowDrawList()->AddLine(c + ImVec2(-half, half), c + ImVec2(half, -half), text_color, 1.5f * scale);
                if (ImGui::IsItemHovered())
                    show_tooltip(_u8L("Delete"));
            }

            const ImVec2 row_min(ImGui::GetWindowPos().x, row_top);
            const ImVec2 row_max(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), row_top + line);
            if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseHoveringRect(row_min, row_max, false)) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, icon_hover);
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    jump_to = tick;
                const std::string code = it->type == Custom ? it->extra : it->type == PausePrint || it->type == Template ? gcode(it->type) : "";
                const size_t      end  = code.find_last_not_of(" \t\r\n");
                if (end != std::string::npos)
                    show_tooltip(code.substr(0, end + 1));
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (open_filaments)
        ImGui::OpenPopup("##marks_filaments");
    if (ImGui::BeginPopup("##marks_filaments")) {
        for (size_t i = 0; i < m_extruder_colors.size(); ++i) {
            const ImU32 icon_clr = ImGuiWrapper::to_ImU32(decode_color_to_float_array(m_extruder_colors[i]));
            if (menu_item_with_icon(into_u8(wxString::Format(_L("Filament %d"), int(i + 1))).c_str(), "", ImVec2(14, 14) * m_scale, icon_clr)) {
                move_active_handle_to(popup.filament_tick);
                add_code_as_tick(ToolChange, int(i + 1));
            }
        }
        ImGui::EndPopup();
    }
    popup.submenu_open = ImGui::IsPopupOpen("##marks_filaments");

    popup.window_min = ImGui::GetWindowPos();
    popup.window_max = popup.window_min + ImGui::GetWindowSize();
    imgui.end();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(5);

    if (jump_to >= 0)
        move_active_handle_to(jump_to);
    if (edit_custom >= 0) {
        move_active_handle_to(edit_custom);
        m_show_custom_gcode_window = true;
        imgui.set_requires_extra_frame();
    }
    if (to_delete.tick >= 0)
        delete_tick(to_delete);
}

bool IMSlider::render(const PreviewOverlayLayout& layout)
{
    m_layout = layout;
    m_wheel_areas.assign(1, is_horizontal() ? layout.bottom_band : layout.right_band);
    bool result = false;
    ImGuiWrapper &imgui = *wxGetApp().imgui();
    ImGui::PushStyleVar(ImGuiStyleVar_::ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_::ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_::ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImVec4 rail_background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    rail_background.w      = 0.8f;
    const ImU32 rail_bg    = ImGui::GetColorU32(rail_background);
    auto begin_band = [&imgui, rail_bg, &layout](const char* name, const CanvasUi::Rect& band, int flags, int corners) {
        imgui.set_next_window_pos(band.left, band.top, ImGuiCond_Always, 0.f, 0.f);
        ImGui::SetNextWindowSize(ImVec2(band.width(), band.height()));
        imgui.begin(std::string(name), flags);
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetWindowPos(), ImGui::GetWindowPos() + ImGui::GetWindowSize(), rail_bg,
                                                  corners == ImDrawFlags_RoundCornersNone ? 0.f : 0.5f * layout.band, corners);
    };
    ImGui::PushStyleColor(ImGuiCol_::ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

    const int windows_flag = ImGuiWindowFlags_NoTitleBar
                       | ImGuiWindowFlags_NoCollapse
                       | ImGuiWindowFlags_NoMove
                       | ImGuiWindowFlags_NoResize
                       | ImGuiWindowFlags_NoScrollbar
                       | ImGuiWindowFlags_NoScrollWithMouse;
    if (is_horizontal()) {
        int corners = ImDrawFlags_RoundCornersNone;
        if (layout.bottom_band.left > 0.f)
            corners = ImDrawFlags_RoundCornersTopLeft;
        if (layout.bottom_band.right < layout.bottom_pill.left)
            corners = corners == ImDrawFlags_RoundCornersNone ? ImDrawFlags_RoundCornersTopRight : ImDrawFlags_RoundCornersTop;
        begin_band("moves_slider", layout.bottom_band, windows_flag, corners);
        if (layout.bottom_band.right >= layout.bottom_pill.left) {
            const ImVec4 text  = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            const float  x     = layout.bottom_band.right - 0.5f * layout.scale;
            const float  inset = 6.f * layout.scale;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(x, layout.bottom_band.top + inset), ImVec2(x, layout.bottom_band.bottom - inset),
                                                ImGui::GetColorU32(ImVec4(text.x, text.y, text.z, 0.3f)), layout.scale);
        }
        int value = GetHigherValue();
        if (horizontal_slider("moves_slider", &value, GetMinValue(), GetMaxValue())) {
            result = true;
            SetHigherValue(value);
        }
        imgui.end();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
        return result;
    }

    begin_band("laysers_slider", layout.right_band, windows_flag, ImDrawFlags_RoundCornersNone);

    const int old_higher   = GetHigherValue();
    const int old_lower    = GetLowerValue();
    int       higher_value = old_higher;
    int       lower_value  = old_lower;
    if (vertical_slider("laysers_slider", &higher_value, &lower_value, GetMinValue(), GetMaxValue(), m_selection, is_one_layer())) {
        if (higher_value != old_higher)
            SetHigherValue(higher_value);
        if (lower_value != old_lower)
            SetLowerValue(lower_value);
        result = true;
    }
    const float inset = 3.f * layout.scale;
    for (const CanvasUi::Rect& square : {layout.top_toggle_square, layout.bottom_toggle_square}) {
        ImGui::PushID(square.top > 0.f ? 1 : 0);
        ImGui::SetCursorScreenPos(ImVec2(square.left + inset, square.top + inset));
        render_single_layer_button(square.width() - 2.f * inset);
        ImGui::PopID();
    }
    render_menu();
    imgui.end();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    if (render_pill("##layers_top_pill", m_higher_value, layout.top_pill, ImDrawFlags_RoundCornersBottomLeft)) {
        m_selection               = ssHigher;
        m_show_go_to_layer_dialog = true;
        imgui.set_requires_extra_frame();
    }
    if (render_pill("##layers_bottom_pill", is_one_layer() ? m_higher_value : m_lower_value,
                    layout.bottom_pill,
                    layout.bottom_band.right >= layout.bottom_pill.left ? ImDrawFlags_RoundCornersNone : ImDrawFlags_RoundCornersTopLeft)) {
        m_selection               = is_one_layer() ? ssHigher : ssLower;
        m_show_go_to_layer_dialog = true;
        imgui.set_requires_extra_frame();
    }
    ImDrawList* background = ImGui::GetBackgroundDrawList();
    const float legend_rounding = 8.f * layout.scale;
    draw_concave_corner(background, ImVec2(layout.right_band.left, layout.top_pill.bottom), -1.f, 1.f,
                        layout.column_top - layout.top_pill.bottom + legend_rounding, rail_bg);
    draw_concave_corner(background, ImVec2(layout.right_band.left, layout.bottom_pill.top), -1.f, -1.f,
                        layout.bottom_pill.top - layout.overlays_bottom + legend_rounding, rail_bg);

    render_marks_popup();

    return result;
}

void IMSlider::render_input_custom_gcode(std::string custom_gcode)
{
    if (m_show_custom_gcode_window)
        ImGui::OpenPopup((_u8L("Custom G-code")).c_str());

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    static bool set_focus = true;

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    imgui.push_menu_style(m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 10) * m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.f * m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 3) * m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 7) * m_scale);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, m_is_dark ? ImVec4(54 / 255.0f, 54 / 255.0f, 60 / 255.0f, 1.00f) : ImVec4(245 / 255.0f, 245 / 255.0f, 245 / 255.0f, 1.00f));
    ImGui::GetCurrentContext()->DimBgRatio = 1.0f;
    int windows_flag =
        ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginPopupModal((_u8L("Custom G-code")).c_str(), NULL, windows_flag))
    {
        imgui.text(_u8L("Enter Custom G-code used on current layer:"));
        if (ImGui::IsMouseClicked(0)) {
            set_focus = false;
        }
        if (set_focus && !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked(0)) {
            wxGetApp().plater()->get_current_canvas3D()->force_set_focus();
            ImGui::SetKeyboardFocusHere(0);
            strcpy(m_custom_gcode, custom_gcode.c_str());
        }
        const int text_height = 6;

        ImGui::InputTextMultiline("##text", m_custom_gcode, sizeof(m_custom_gcode), ImVec2(-1, ImGui::GetTextLineHeight() * text_height));

        ImGui::NewLine();
        ImGui::SameLine(ImGui::GetStyle().WindowPadding.x * 14);
        imgui.push_confirm_button_style();

        bool disable_button = false;
        if (strlen(m_custom_gcode) == 0)
            disable_button = true;
        if (disable_button) {
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            imgui.push_button_disable_style();
        }
        if (imgui.bbl_button(_L("OK"))) {
            add_custom_gcode(m_custom_gcode);
            m_show_custom_gcode_window = false;
            ImGui::CloseCurrentPopup();
            set_focus = true;
        }
        if (disable_button) {
            ImGui::PopItemFlag();
            imgui.pop_button_disable_style();
        }
        imgui.pop_confirm_button_style();

        ImGui::SameLine();
        imgui.push_cancel_button_style();
        if (imgui.bbl_button(_L("Cancel")) || ImGui::IsKeyDown(ImGui::GetKeyIndex(ImGuiKey_Escape))) {
            m_show_custom_gcode_window = false;
            ImGui::CloseCurrentPopup();
            set_focus = true;
        }
        imgui.pop_cancel_button_style();

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor();
    imgui.pop_menu_style();
}

void IMSlider::do_go_to_layer(size_t layer_number) {
    layer_number = std::clamp((int)layer_number, m_min_value, m_max_value);
    GetSelection() == ssLower ? SetLowerValue(layer_number) : SetHigherValue(layer_number);
}

void IMSlider::render_go_to_layer_dialog()
{
    if (m_show_go_to_layer_dialog)
        ImGui::OpenPopup((_u8L("Jump to layer")).c_str());

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    static bool set_focus = true;

    imgui.push_menu_style(m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 10) * m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.f * m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 3) * m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 7) * m_scale);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, m_is_dark ? ImVec4(54 / 255.0f, 54 / 255.0f, 60 / 255.0f, 1.00f) : ImVec4(245 / 255.0f, 245 / 255.0f, 245 / 255.0f, 1.00f));
    ImGui::GetCurrentContext()->DimBgRatio = 1.0f;
    int windows_flag =
        ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginPopupModal((_u8L("Jump to layer")).c_str(), NULL, windows_flag))
    {
        imgui.text(_u8L("Please enter the layer number.") + " (" + std::to_string(m_min_value + 1) + " - " + std::to_string(m_max_value + 1) + "):");
        if (ImGui::IsMouseClicked(0)) {
            set_focus = false;
        }
        if (set_focus && !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked(0)) {
            wxGetApp().plater()->get_current_canvas3D()->force_set_focus();
            ImGui::SetKeyboardFocusHere(0);
        }
        ImGui::InputText("##input_layer_number", m_layer_number, sizeof(m_layer_number));

        ImGui::NewLine();
        ImGui::SameLine(GImGui->Style.WindowPadding.x * 8);
        imgui.push_confirm_button_style();
        bool disable_button = false;
        if (strlen(m_layer_number) == 0)
            disable_button = true;
        else {
            for (size_t i = 0; i < strlen(m_layer_number); i++)
                if (!isdigit(m_layer_number[i]))
                    disable_button = true;
            if (!disable_button && (m_min_value > atoi(m_layer_number) - 1 || atoi(m_layer_number) - 1 > m_max_value))
                disable_button = true;
        }
        if (disable_button) {
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            imgui.push_button_disable_style();
        }
        if (imgui.bbl_button(_L("OK")) || (!disable_button && ImGui::IsKeyDown(ImGui::GetKeyIndex(ImGuiKey_Enter)))) {
            do_go_to_layer(atoi(m_layer_number) - 1);
            m_show_go_to_layer_dialog = false;
            ImGui::CloseCurrentPopup();
            set_focus = true;
        }
        if (disable_button) {
            ImGui::PopItemFlag();
            imgui.pop_button_disable_style();
        }
        imgui.pop_confirm_button_style();

        ImGui::SameLine();
        imgui.push_cancel_button_style();
        if (imgui.bbl_button(_L("Cancel")) || ImGui::IsKeyDown(ImGui::GetKeyIndex(ImGuiKey_Escape))) {
            m_show_go_to_layer_dialog = false;
            ImGui::CloseCurrentPopup();
            set_focus = true;
        }
        imgui.pop_cancel_button_style();

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor();
    imgui.pop_menu_style();
}

void IMSlider::render_menu() {
    if (!m_menu_enable) {
        render_go_to_layer_dialog();
        return;
    }

    ImGuiWrapper::push_menu_style(m_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_::ImGuiStyleVar_ChildRounding, 4.0f * m_scale);

    auto tick_it = GetSelection() == ssHigher ? m_ticks.ticks.find(TickCode{ GetHigherValue() }) :
        GetSelection() == ssLower ? m_ticks.ticks.find(TickCode{ GetLowerValue() }) :
        m_ticks.ticks.end();
    std::string custom_code;
    if (tick_it != m_ticks.ticks.end()) {
        if (tick_it->type == CustomGCode::Custom)
            custom_code = tick_it->extra;
        render_edit_menu(*tick_it);
    }
    else {
        render_add_menu();
    }

    ImGui::PopStyleVar(1);
    ImGuiWrapper::pop_menu_style();

    render_input_custom_gcode(custom_code);
    render_go_to_layer_dialog();
}

void IMSlider::render_add_menu()
{
    int extruder_num = m_extruder_colors.size();

    if (m_show_menu) {
        ImGui::OpenPopup("slider_add_menu_popup");
        m_show_menu = false;
    }
    if (ImGui::BeginPopup("slider_add_menu_popup", ImGuiWindowFlags_NoMove)) {
        bool menu_item_enable = m_draw_mode != dmSequentialFffPrint;
        bool hovered = false;
        {
            if (menu_item_with_icon(_u8L("Add Pause").c_str(), "", ImVec2(0, 0), 0, false, menu_item_enable, &hovered)) {
                add_code_as_tick(PausePrint);
            }
            if (hovered) { show_tooltip(_u8L("Insert a pause command at the beginning of this layer.")); }


            if (menu_item_with_icon(_u8L("Add Custom G-code").c_str(), "", ImVec2(0, 0), 0, false, menu_item_enable, &hovered)) {
                m_show_custom_gcode_window = true;
            }
            if (hovered) { show_tooltip(_u8L("Insert custom G-code at the beginning of this layer.")); }

            if (!gcode(Template).empty()) {
                if (menu_item_with_icon(_u8L("Add Custom Template").c_str(), "", ImVec2(0, 0), 0, false, menu_item_enable, &hovered)) {
                    add_code_as_tick(Template);
                }
                if (hovered) { show_tooltip(_u8L("Insert template custom G-code at the beginning of this layer.")); }
            }

            if (menu_item_with_icon(_u8L("Jump to layer").c_str(), "")) {
                m_show_go_to_layer_dialog = true;
            }
        }

        //BBS render this menu item only when extruder_num > 1
        if (extruder_num > 1) {
            if (!m_can_change_color) {
                begin_menu(_u8L("Change Filament").c_str(), false);
            }
            else if (begin_menu(_u8L("Change Filament").c_str())) {
                for (int i = 0; i < extruder_num; i++) {
                    ColorRGBA rgba     = decode_color_to_float_array(m_extruder_colors[i]);
                    ImU32                icon_clr = ImGuiWrapper::to_ImU32(rgba);
                    if (rgba.a() == 0)
                        icon_clr = 0;
                    if (menu_item_with_icon(into_u8(wxString::Format(_L("Filament %d"), i + 1)).c_str(), "", ImVec2(14, 14) * m_scale, icon_clr, false, true, &hovered)) add_code_as_tick(ToolChange, i + 1);
                    if (hovered) { show_tooltip(_u8L("Change filament at the beginning of this layer.")); }
                }
                end_menu();
            }
        }

        ImGui::EndPopup();
    }
}

void IMSlider::render_edit_menu(const TickCode& tick)
{
    if (m_show_menu) {
        ImGui::OpenPopup("slider_edit_menu_popup");
        m_show_menu = false;
    }
    if (ImGui::BeginPopup("slider_edit_menu_popup", ImGuiWindowFlags_NoMove)) {
        switch (tick.type)
        {
        case CustomGCode::PausePrint:
            if (menu_item_with_icon(_u8L("Delete Pause").c_str(), "")) {
                delete_tick(tick);
            }
            break;
        case CustomGCode::Template:
            if (!gcode(Template).empty()) {
                if (menu_item_with_icon(_u8L("Delete Custom Template").c_str(), "")) {
                    delete_tick(tick);
                }
            }
            break;
        case CustomGCode::Custom:
            if (menu_item_with_icon(_u8L("Edit Custom G-code").c_str(), "")) {
                m_show_custom_gcode_window = true;
            }
            if (menu_item_with_icon(_u8L("Delete Custom G-code").c_str(), "")) {
                delete_tick(tick);
            }
            break;
        case CustomGCode::ToolChange: {
            int extruder_num = m_extruder_colors.size();
            if (extruder_num > 1) {
                if (!m_can_change_color) {
                    begin_menu(_u8L("Change Filament").c_str(), false);
                }
                else if (begin_menu(_u8L("Change Filament").c_str())) {
                    for (int i = 0; i < extruder_num; i++) {
                        ColorRGBA rgba = decode_color_to_float_array(m_extruder_colors[i]);
                        ImU32     icon_clr = ImGuiWrapper::to_ImU32(rgba);
                        if (menu_item_with_icon(into_u8(wxString::Format(_L("Filament %d"), i + 1)).c_str(), "", ImVec2(14, 14) * m_scale, icon_clr)) add_code_as_tick(ToolChange, i + 1);
                    }
                    end_menu();
                }
                if (menu_item_with_icon(_u8L("Delete Filament Change").c_str(), "")) {
                    delete_tick(tick);
                }
            }
            break;
        }
        case CustomGCode::ColorChange:
        case CustomGCode::Unknown:
        default:
            break;
        }
        ImGui::EndPopup();
    }
}

void IMSlider::on_change_color_mode(bool is_dark) {
    m_is_dark = is_dark;
}

void IMSlider::set_scale(float scale)
{
    if(m_scale != scale) m_scale = scale;
}

bool IMSlider::on_mouse_wheel(wxMouseEvent& evt) {
    const float x = float(evt.GetPosition().x);
    const float y = float(evt.GetPosition().y);
    if (std::none_of(m_wheel_areas.begin(), m_wheel_areas.end(), [x, y](const CanvasUi::Rect& area) { return area.contains(CanvasUi::Point(x, y)); }))
        return false;

    ImGuiWrapper& imgui = *wxGetApp().imgui();

    float wheel = evt.GetWheelRotation();
    // mac trackpads trigger this event with value 0 when right-clicking with two fingers
    if (wheel == 0.0f)
        return false;
    wheel = wheel > 0 ? 1.0f : -1.0f;

#ifdef __WXOSX__
    if (wxGetKeyState(WXK_SHIFT)) {
        wheel *= -5;
    }
    else if (wxGetKeyState(WXK_RAW_CONTROL)) {
        wheel *= 5;
    }
#else
    if (wxGetKeyState(WXK_COMMAND) || wxGetKeyState(WXK_SHIFT))
        wheel *= 5;
#endif
    if (is_horizontal())
        SetHigherValue(GetHigherValue() + wheel);
    else if (is_one_layer()) {
        const int new_pos = GetHigherValue() + wheel;
        SetHigherValue(new_pos);
        m_one_layer_value = new_pos; // ORCA backup value for single layer mode
    } else {
        const int new_pos = m_selection == ssLower ? GetLowerValue() + wheel : GetHigherValue() + wheel;
        m_selection == ssLower ? SetLowerValue(new_pos) : SetHigherValue(new_pos);
    }
    set_as_dirty();
    imgui.set_requires_extra_frame();
    return true;
}

void IMSlider::correct_lower_value()
{
    if (m_lower_value < m_min_value)
        m_lower_value = m_min_value;
    else if (m_lower_value > m_max_value)
        m_lower_value = m_max_value;

    if ((m_lower_value >= m_higher_value && m_lower_value <= m_max_value) || m_is_one_layer) m_higher_value = m_lower_value;
}

void IMSlider::correct_higher_value()
{
    if (m_higher_value > m_max_value)
        m_higher_value = m_max_value;
    else if (m_higher_value < m_min_value)
        m_higher_value = m_min_value;

    if ((m_higher_value <= m_lower_value && m_higher_value >= m_min_value) || m_is_one_layer) m_lower_value = m_higher_value;
}

bool IMSlider::is_wipe_tower_layer(int tick) const
{
    // BBS: This function is useless in BBS
    return false;

    if (!m_is_wipe_tower || tick >= (int) m_values.size()) return false;
    if (tick == 0 || (tick == (int) m_values.size() - 1 && m_values[tick] > m_values[tick - 1])) return false;
    if ((m_values[tick - 1] == m_values[tick + 1] && m_values[tick] < m_values[tick + 1]) ||
        (tick > 0 && m_values[tick] < m_values[tick - 1])) // if there is just one wiping on the layer
        return true;

    return false;
}

std::string IMSlider::get_label(int tick, LabelType label_type)
{
    const size_t value = tick;

    if (m_label_koef == 1.0 && m_values.empty()) {
        return std::to_string(value);
    }
    if (value >= m_values.size()) return "error";

    auto get_layer_number = [this](int value, LabelType label_type) {
        if (label_type == ltEstimatedTime && m_layers_times.empty()) return size_t(-1);
        double layer_print_z = m_values[is_wipe_tower_layer(value) ? std::max<int>(value - 1, 0) : value];
        auto   it            = std::lower_bound(m_layers_values.begin(), m_layers_values.end(), layer_print_z - epsilon());
        if (it == m_layers_values.end()) {
            it = std::lower_bound(m_values.begin(), m_values.end(), layer_print_z - epsilon());
            if (it == m_values.end()) return size_t(-1);
            return size_t(value);
        }
        return size_t(it - m_layers_values.begin());
    };

    if (m_draw_mode == dmSequentialGCodeView) {
        return std::to_string(tick);

    } else {
        if (label_type == ltEstimatedTime) {
            if (m_is_wipe_tower) {
                size_t layer_number = get_layer_number(value, label_type);
                return (layer_number == size_t(-1) || layer_number == m_layers_times.size()) ? "" : short_and_splitted_time(get_time_dhms(m_layers_times[layer_number]));
            }
            return value < m_layers_times.size() ? short_and_splitted_time(get_time_dhms(m_layers_times[value])) : "";
        }

        char layer_height[64];
        ::sprintf(layer_height, "%.2f", m_values.empty() ? m_label_koef * value : m_values[value]);
        if (label_type == ltHeight) return std::string(layer_height);
        const size_t layer_number = m_draw_mode == dmSequentialFffPrint ? (m_values.empty() ? value : value + 1) :
                                    m_is_wipe_tower                    ? get_layer_number(value, label_type) + 1 :
                                                                         (m_values.empty() ? value : value + 1);
        if (label_type == ltLayer) return std::to_string(layer_number);
        if (label_type == ltHeightWithLayer) {
            char buffer[90];
            ::sprintf(buffer, "%5s\n%5s", std::to_string(layer_number).c_str(), layer_height);
            return std::string(buffer);
        }
    }

    return "";
}

double IMSlider::get_double_value(const SelectedSlider &selection)
{
    if (m_values.empty() || m_lower_value < 0) return 0.0;
    if (m_values.size() <= size_t(m_higher_value)) {
        correct_higher_value();
        return m_values.back();
    }
    return m_values[selection == ssLower ? m_lower_value : m_higher_value];
}

int IMSlider::get_tick_from_value(double value, bool force_lower_bound /* = false*/)
{
    std::vector<double>::iterator it;
    if (m_is_wipe_tower && !force_lower_bound)
        it = std::find_if(m_values.begin(), m_values.end(), [value](const double &val) { return fabs(value - val) <= epsilon(); });
    else
        it = std::lower_bound(m_values.begin(), m_values.end(), value - epsilon());

    if (it == m_values.end()) return -1;
    return int(it - m_values.begin());
}

float IMSlider::get_pos_from_value(int v_min, int v_max, int value, const ImRect& rect) {
    float pos_ratio = (v_max - v_min) != 0 ? ((float)(value - v_min) / (float)(v_max - v_min)) : 0.0f;
    float handle_pos;
    if (is_horizontal()) {
        handle_pos = rect.Min.x + (rect.Max.x - rect.Min.x) * pos_ratio;
    }
    else {
        pos_ratio = 1.0f - pos_ratio;
        handle_pos = rect.Min.y + (rect.Max.y - rect.Min.y) * pos_ratio;
    }
    return handle_pos;
}

std::string IMSlider::get_color_for_tool_change_tick(std::set<TickCode>::const_iterator it) const
{
    const int current_extruder = it->extruder == 0 ? std::max<int>(m_only_extruder, 1) : it->extruder;

    auto it_n = it;
    while (it_n != m_ticks.ticks.begin()) {
        --it_n;
        if (it_n->type == ColorChange && it_n->extruder == current_extruder)
            return it_n->color;
    }

    if ((current_extruder > 0 && (current_extruder - 1) < m_extruder_colors.size()))
    {
        return m_extruder_colors[current_extruder - 1]; // return a color for a specific extruder from the colors list
    }
    return "";
}

// Get active extruders for tick.
// Means one current extruder for not existing tick OR
// 2 extruders - for existing tick (extruder before ToolChange and extruder of current existing tick)
// Use those values to disable selection of active extruders
std::array<int, 2> IMSlider::get_active_extruders_for_tick(int tick) const
{
    int                default_initial_extruder = m_mode == MultiAsSingle ? std::max<int>(1, m_only_extruder) : 1;
    std::array<int, 2> extruders                = {default_initial_extruder, -1};
    if (m_ticks.empty()) return extruders;

    auto it = m_ticks.ticks.lower_bound(TickCode{tick});

    if (it != m_ticks.ticks.end() && it->tick == tick) // current tick exists
        extruders[1] = it->extruder;

    while (it != m_ticks.ticks.begin()) {
        --it;
        if (it->type == ToolChange) {
            extruders[0] = it->extruder;
            break;
        }
    }

    return extruders;
}

}

} // Slic3r
