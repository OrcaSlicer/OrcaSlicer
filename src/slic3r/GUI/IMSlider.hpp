#ifndef slic3r_GUI_IMSlider_hpp_
#define slic3r_GUI_IMSlider_hpp_

#include "TickCode.hpp"
#include "PreviewOverlayLayout.hpp"
#include "CanvasUi/Types.hpp"
#include <cstddef>
#include <functional>
#include <array>
#include <imgui/imgui.h>
#include <vector>
#include "libslic3r/CustomGCode.hpp"
#include <string>
#include <wx/event.h>
#include <wx/slider.h>

#include <set>

class wxMouseEvent;

struct IMGUI_API ImRect;

namespace Slic3r {

class PrintObject;
class Layer;

namespace GUI {

/* For exporting GCode in GCodeWriter is used XYZF_NUM(val) = PRECISION(val, 3) for XYZ values. 
 * So, let use same value as a permissible error for layer height.
 */
constexpr double epsilon() { return 0.0011; }

bool equivalent_areas(const double &bottom_area, const double &top_area);

// return true if color change was detected
bool check_color_change(PrintObject* object, size_t frst_layer_id, size_t layers_cnt, bool check_overhangs,
                        // what to do with detected color change
                        // and return true when detection have to be desturbed
                        std::function<bool(Layer*)> break_condition);

// Fills the inner corner where a docked tab meets a band, so the tab flows into it. dx and dy
// (+1 or -1) point from the corner into the gap the fillet covers.
void draw_concave_corner(ImDrawList* draw_list, const ImVec2& corner, float dx, float dy, float radius, ImU32 color);

enum SelectedSlider {
    ssUndef = 0,
    ssLower = 1,
    ssHigher = 2
};

enum DrawMode
{
    dmRegular,
    dmSlaPrint,
    dmSequentialFffPrint,
    dmSequentialGCodeView,
};

enum LabelType
{
    ltHeightWithLayer,
    ltHeight,
    ltLayer,
    ltEstimatedTime,
};

class IMSlider
{
public:
    IMSlider(int lowerValue, int higherValue, int minValue, int maxValue, long style = wxSL_VERTICAL);


    ~IMSlider() {}

    int    GetMinValue() const { return m_min_value; }
    int    GetMaxValue() const { return m_max_value; }
    double GetMinValueD() { return m_values.empty() ? 0. : m_values[m_min_value]; }
    double GetMaxValueD() { return m_values.empty() ? 0. : m_values[m_max_value]; }
    int    GetLowerValue() const { return m_lower_value; }
    int    GetHigherValue() const { return m_higher_value; }
    int    GetActiveValue() const;
    double GetLowerValueD() { return get_double_value(ssLower); }
    double GetHigherValueD() { return get_double_value(ssHigher); }
    SelectedSlider GetSelection() { return m_selection; }

    // Set low and high slider position. If the span is non-empty, disable the "one layer" mode.
    void SetLowerValue(const int lower_val);
    void SetHigherValue(const int higher_val);
    void SetSelectionSpan(const int lower_val, const int higher_val);
    void SetMaxValue(const int max_value);
    void SetKoefForLabels(const double koef) { m_label_koef = koef; }
    void SetSliderValues(const std::vector<double> &values);
    void SetSliderAlternateValues(const std::vector<double> &values) { m_alternate_values = values; }

    CustomGCode::Info GetTicksValues() const;
    void SetTicksValues(const CustomGCode::Info &custom_gcode_per_print_z);
    void SetLayersTimes(const std::vector<float> &layers_times, float total_time);
    void SetLayersTimes(const std::vector<double> &layers_times);

    void SetDrawMode(bool is_sequential_print);
    void SetDrawMode(DrawMode mode) { m_draw_mode = mode; }
    //BBS
    void SetExtraStyle(long style) { m_extra_style = style; }
    void SetManipulationMode(CustomGCode::Mode mode) { m_mode = mode; }
    CustomGCode::Mode GetManipulationMode() const { return m_mode; }
    void SetModeAndOnlyExtruder(const bool is_one_extruder_printed_model, const int only_extruder, bool can_change_color);
    void SetExtruderColors(const std::vector<std::string> &extruder_colors);

    bool IsNewPrint();

    void set_render_as_disabled(bool value) { m_render_as_disabled = value; }
    bool is_rendering_as_disabled() const { return m_render_as_disabled; }

    bool is_horizontal() const { return m_style == wxSL_HORIZONTAL; }
    bool is_one_layer() const { return m_is_one_layer; }
    bool is_lower_at_min() const { return m_lower_value == m_min_value; }
    bool is_higher_at_max() const { return m_higher_value == m_max_value; }
    bool is_full_span() const { return this->is_lower_at_min() && this->is_higher_at_max(); }

    void UseDefaultColors(bool def_colors_on) { m_ticks.set_default_colors(def_colors_on); }

    // Whether the wheel was over one of the wheel areas and moved the slider.
    bool on_mouse_wheel(wxMouseEvent& evt);
    // An area outside the slider's own windows where the wheel moves it too, for this frame.
    void add_wheel_area(const CanvasUi::Rect& area) { m_wheel_areas.push_back(area); }
    void post_ticks_changed_event(CustomGCode::Type type = CustomGCode::Unknown);
    bool check_ticks_changed_event(CustomGCode::Type type);
    bool switch_one_layer_mode();
    void show_go_to_layer(bool show) { m_show_go_to_layer_dialog = show; }

    bool render(const PreviewOverlayLayout& layout);
    // Size of a layer pill holding the longest label of this print, in canvas pixels.
    ImVec2 pill_size(float scale);

    //BBS update scroll value changed
    bool is_dirty() { return m_dirty; }
    // whether the mouse is holding this slider's handle, read from ImGui's active id rather than
    // from the dirty flag, which is raised and consumed inside a single frame
    bool is_dragging() const;
    void set_as_dirty(bool dirty = true) { m_dirty = dirty; }
    bool is_need_post_tick_event() { return m_is_need_post_tick_changed_event; }
    void reset_post_tick_event(bool val = false) {
        m_is_need_post_tick_changed_event = val;
        m_tick_change_event_type = CustomGCode::Type::Unknown;
    }
    CustomGCode::Type get_post_tick_event_type() { return m_tick_change_event_type; }

    float m_scale = 1.0;
    void set_scale(float scale = 1.0);
    void on_change_color_mode(bool is_dark);
    void set_menu_enable(bool enable = true) { m_menu_enable = enable; }

protected:
    void add_custom_gcode(std::string custom_gcode);
    void add_code_as_tick(CustomGCode::Type type, int selected_extruder = -1);
    void delete_tick(const TickCode& tick);
    void do_go_to_layer(size_t layer_number); //menu
    void correct_lower_value();
    void correct_higher_value();
    bool horizontal_slider(const char* str_id, int* v, int v_min, int v_max);
    void render_go_to_layer_dialog(); //menu
    void render_input_custom_gcode(std::string custom_gcode = ""); //menu
    void render_menu();
    void render_add_menu(); //menu
    void render_edit_menu(const TickCode& tick); //menu
    void draw_colored_band(const ImRect& groove, const ImRect& slideable_region);
    void draw_ticks(const ImRect& slideable_region);
    void draw_tick_on_mouse_position(const ImRect& slideable_region);
    void show_tooltip(const std::string tooltip); //menu
    bool vertical_slider(const char* str_id, int* higher_value, int* lower_value, int v_min, int v_max,
        SelectedSlider& selection, bool one_layer_flag);
    bool  render_pill(const char* str_id, int tick, const CanvasUi::Rect& rect, int corners);
    void  render_single_layer_button(float size);
    void render_marks_popup();
    void move_active_handle_to(int tick);
    bool is_wipe_tower_layer(int tick) const;

private:
    struct PillText
    {
        std::string prefix;
        std::string number;
        std::string suffix;
        std::string z;
    };
    // Widths of the pill's parts for the last layer, so the pill does not change size with the handle.
    struct PillSlots
    {
        float prefix{0.f};
        float number{0.f};
        float suffix{0.f};
        float z{0.f};
        float width{0.f};
    };
    PillText  pill_text(int tick);
    PillSlots pill_slots();

    std::string get_label(int tick, LabelType label_type = ltHeightWithLayer);
    ImU32       tick_color(std::set<TickCode>::const_iterator it) const;
    double get_double_value(const SelectedSlider& selection);
    int    get_tick_from_value(double value, bool force_lower_bound = false);
    float get_pos_from_value(int v_min, int v_max, int value, const ImRect& rect);
    int    get_tick_near_point(int v_min, int v_max, const ImVec2& pt, const ImRect& rect);

    std::string get_color_for_tool_change_tick(std::set<TickCode>::const_iterator it) const;
    // Get active extruders for tick.
    // Means one current extruder for not existing tick OR
    // 2 extruders - for existing tick (extruder before ToolChangeCode and extruder of current existing tick)
    // Use those values to disable selection of active extruders
    std::array<int, 2> get_active_extruders_for_tick(int tick) const;

    // Use those values to disable selection of active extruders
    bool m_is_dark = false;

    int  m_min_value;
    int  m_max_value;
    int  m_lower_value;
    int  m_higher_value;
    int  m_one_layer_value; // ORCA
    bool m_dirty = false;
    // the ImGui id of the slider widget, as of its last render
    unsigned int m_imgui_id = 0;

    bool m_render_as_disabled{ false };

    SelectedSlider m_selection;
    bool m_is_one_layer       = false;
    bool m_menu_enable        = true; //menu
    bool m_show_menu          = false; //menu
    bool m_show_custom_gcode_window = false; //menu
    bool m_show_go_to_layer_dialog = false; //menu
    bool m_force_mode_apply   = true;
    bool m_is_wipe_tower      = false; // This flag indicates that there is multiple extruder print with wipe tower
    bool m_is_spiral_vase     = false;

    PreviewOverlayLayout     m_layout;
    std::vector<CanvasUi::Rect> m_wheel_areas;

    struct MarksPopup
    {
        bool             open{false};
        std::vector<int> ticks;
        ImVec2           anchor;
        ImVec2           mark_min;
        ImVec2           mark_max;
        ImVec2           window_min;
        ImVec2           window_max;
        bool             submenu_open{false};
        int              filament_tick{-1};
    };
    MarksPopup m_marks_popup;
    bool       m_mark_hovered{false};

    DrawMode            m_draw_mode = dmRegular;
    CustomGCode::Mode   m_mode          = CustomGCode::SingleExtruder;
    int                 m_only_extruder = -1;

    long                m_style;
    long                m_extra_style;
    float               m_label_koef{1.0};

    std::vector<double>      m_values;
    TickCodeInfo             m_ticks;
    std::vector<double>      m_layers_times;
    std::vector<double>      m_layers_values;
    std::vector<std::string> m_extruder_colors;
    bool                     m_can_change_color;
    std::string              m_print_obj_idxs;
    bool                     m_is_need_post_tick_changed_event { false };
    CustomGCode::Type        m_tick_change_event_type;

    std::vector<double> m_alternate_values;

    char m_custom_gcode[1024] = { 0 }; //menu
    char m_layer_number[64] = { 0 }; //menu
};

}

} // Slic3r


#endif // slic3r_GUI_IMSlider_hpp_
