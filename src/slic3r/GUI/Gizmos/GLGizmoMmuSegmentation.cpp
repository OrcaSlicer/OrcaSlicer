#include "GLGizmoMmuSegmentation.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/BitmapCache.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "GLGizmoUtils.hpp"


#include <glad/gl.h>

namespace Slic3r::GUI {

static inline void show_notification_extruders_limit_exceeded()
{
    wxGetApp()
        .plater()
        ->get_notification_manager()
        ->push_notification(NotificationType::MmSegmentationExceededExtrudersLimit, NotificationManager::NotificationLevel::PrintInfoNotificationLevel,
                            GUI::format(_L("Filament count exceeds the maximum number that painting tool supports. Only the "
                                           "first %1% filaments will be available in painting tool."), GLGizmoMmuSegmentation::EXTRUDERS_LIMIT));
}

void GLGizmoMmuSegmentation::on_opening()
{
    if (wxGetApp().filaments_cnt() > int(GLGizmoMmuSegmentation::EXTRUDERS_LIMIT))
        show_notification_extruders_limit_exceeded();
}

void GLGizmoMmuSegmentation::on_shutdown()
{
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

std::string GLGizmoMmuSegmentation::on_get_name() const
{
    return _u8L("Color Painting");
}

bool GLGizmoMmuSegmentation::on_is_selectable() const
{
    return (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF
            && /*wxGetApp().get_mode() != comSimple && */wxGetApp().filaments_cnt() > 1);
}

bool GLGizmoMmuSegmentation::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();
    return !selection.is_empty() && (selection.is_single_full_instance() || selection.is_any_volume()) && wxGetApp().filaments_cnt() > 1;
}

static std::vector<int> get_extruder_id_for_volumes(const ModelObject &model_object)
{
    std::vector<int> extruders_idx;
    extruders_idx.reserve(model_object.volumes.size());
    for (const ModelVolume *model_volume : model_object.volumes) {
        if (!model_volume->is_model_part())
            continue;

        extruders_idx.emplace_back(model_volume->extruder_id());
    }

    return extruders_idx;
}

void GLGizmoMmuSegmentation::init_extruders_data()
{
    m_extruders_colors      = wxGetApp().plater()->get_extruders_colors();
    m_selected_extruder_idx = 0;

    // keep remap table consistent with current extruder count
    m_extruder_remap.resize(m_extruders_colors.size());
    for (size_t i = 0; i < m_extruder_remap.size(); ++i)
        m_extruder_remap[i] = i;
}

bool GLGizmoMmuSegmentation::on_init()
{
    // BBS
    m_shortcut_key = WXK_CONTROL_N;

    const wxString ctrl  = GUI::shortkey_ctrl_prefix();
    const wxString alt   = GUI::shortkey_alt_prefix();
    const wxString shift = GUI::shortkey_shift_prefix();

    m_desc["clipping_of_view"] = _L("Section view");
    m_desc["reset_direction"]  = _L("Reset direction");
    m_desc["cursor_size"]      = _L("Brush size");
    m_desc["cursor_type"]      = _L("Brush shape");
    m_desc["paint"]            = _L("Paint");
    m_desc["erase"]            = _L("Erase");
    m_desc["shortcut_key"]     = _L("Choose filament");
    m_desc["edge_detection"]   = _L("Edge detection");
    m_desc["gap_area"]         = _L("Gap area");
    m_desc["perform"]          = _L("Perform");
    m_desc["remove_all"]       = _L("Erase all painting");
    m_desc["circle"]           = _L("Circle");
    m_desc["sphere"]           = _L("Sphere");
    m_desc["pointer"]          = _L("Triangles");
    m_desc["filaments"]        = _L("Filaments");
    m_desc["tool_type"]        = _L("Tool type");
    m_desc["tool_brush"]       = _L("Brush");
    m_desc["tool_smart_fill"]  = _L("Smart fill");
    m_desc["tool_bucket_fill"] = _L("Bucket fill");
    m_desc["smart_fill_angle"] = _L("Smart fill angle");
    m_desc["height_range"]     = _L("Height range");
    m_desc["toggle_wireframe"] = _L("Toggle Wireframe");
    m_desc["perform_remap"]    = _u8L("Remap filaments");
    m_desc["remap"]            = _L("Remap");
    m_desc["remap_reset"]      = _L("Reset");

    std::pair<wxString, wxString> paint_shortcut            = {_L("Left mouse button"),         m_desc["paint"]};
    std::pair<wxString, wxString> erase_shortcut            = {shift + _L("Left mouse button"), m_desc["erase"]};
    std::pair<wxString, wxString> clipping_shortcut         = {alt + _L("Mouse wheel"),         m_desc["clipping_of_view"]};
    std::pair<wxString, wxString> toggle_wireframe_shortcut = {alt + shift + _L("Enter"),       m_desc["toggle_wireframe"]};

    m_shortcuts_brush = {
        paint_shortcut,
        erase_shortcut,
        {ctrl + _L("Mouse wheel"), m_desc["cursor_size"]},
        clipping_shortcut,
        toggle_wireframe_shortcut
    };

    m_shortcuts_bucket_fill = {
        paint_shortcut,
        erase_shortcut,
        {ctrl + _L("Mouse wheel"), m_desc["smart_fill_angle"]},
        clipping_shortcut,
        toggle_wireframe_shortcut
    };

    m_shortcuts_gap_fill = {
        {ctrl + _L("Mouse wheel"), m_desc["gap_area"]},
        toggle_wireframe_shortcut
    };

    init_extruders_data();

    return true;
}

GLGizmoMmuSegmentation::GLGizmoMmuSegmentation(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id), m_current_tool(ImGui::CircleButtonIcon)
{
}

void GLGizmoMmuSegmentation::render_painter_gizmo()
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);

    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

void GLGizmoMmuSegmentation::data_changed(bool is_serializing)
{
    GLGizmoPainterBase::data_changed(is_serializing);
    if (m_state != On || wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptFFF || wxGetApp().extruders_edited_cnt() <= 1)
        return;

    ModelObject* model_object = m_c->selection_info()->model_object();
    int prev_extruders_count = int(m_extruders_colors.size());
    if (prev_extruders_count != wxGetApp().filaments_cnt()) {
        if (wxGetApp().filaments_cnt() > int(GLGizmoMmuSegmentation::EXTRUDERS_LIMIT))
            show_notification_extruders_limit_exceeded();

        this->init_extruders_data();
        // Reinitialize triangle selectors because of change of extruder count need also change the size of GLIndexedVertexArray
        if (prev_extruders_count != wxGetApp().filaments_cnt())
            this->init_model_triangle_selectors();
    } else if (wxGetApp().plater()->get_extruders_colors() != m_extruders_colors) {
        this->init_extruders_data();
        this->update_triangle_selectors_colors();
    }
    else if (model_object != nullptr && get_extruder_id_for_volumes(*model_object) != m_volumes_extruder_idxs) {
        this->init_model_triangle_selectors();
    }
}

// BBS
bool GLGizmoMmuSegmentation::on_number_key_down(int number)
{
    int extruder_idx = number - 1;
    if (extruder_idx < m_extruders_colors.size() && extruder_idx >= 0)
        m_selected_extruder_idx = extruder_idx;

    return true;
}

bool GLGizmoMmuSegmentation::on_key_down_select_tool_type(int keyCode) {
    switch (keyCode)
    {
    case 'F':
        m_current_tool = ImGui::FillButtonIcon;
        break;
    case 'T':
        m_current_tool = ImGui::TriangleButtonIcon;
        break;
    case 'S':
        m_current_tool = ImGui::SphereButtonIcon;
        break;
    case 'C':
        m_current_tool = ImGui::CircleButtonIcon;
        break;
    case 'H':
        m_current_tool = ImGui::HeightRangeIcon;
        break;
    case 'G':
        m_current_tool = ImGui::GapFillIcon;
        break;
    default:
        return false;
        break;
    }
    return true;
}

static void render_extruders_combo(const std::string& label,
                                   const std::vector<std::string>& extruders,
                                   const std::vector<ColorRGBA>& extruders_colors,
                                   size_t& selection_idx)
{
    assert(!extruders_colors.empty());
    assert(extruders_colors.size() == extruders_colors.size());

    size_t selection_out = selection_idx;
    // It is necessary to use BeginGroup(). Otherwise, when using SameLine() is called, then other items will be drawn inside the combobox.
    ImGui::BeginGroup();
    ImVec2 combo_pos = ImGui::GetCursorScreenPos();
    if (ImGui::BeginCombo(label.c_str(), "")) {
        for (size_t extruder_idx = 0; extruder_idx < std::min(extruders.size(), GLGizmoMmuSegmentation::EXTRUDERS_LIMIT); ++extruder_idx) {
            ImGui::PushID(int(extruder_idx));
            ImVec2 start_position = ImGui::GetCursorScreenPos();

            if (ImGui::Selectable("", extruder_idx == selection_idx))
                selection_out = extruder_idx;

            ImGui::SameLine();
            ImGuiStyle &style  = ImGui::GetStyle();
            float       height = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(start_position, ImVec2(start_position.x + height + height / 2, start_position.y + height), ImGuiWrapper::to_ImU32(extruders_colors[extruder_idx]));
            ImGui::GetWindowDrawList()->AddRect(start_position, ImVec2(start_position.x + height + height / 2, start_position.y + height), IM_COL32_BLACK);

            ImGui::SetCursorScreenPos(ImVec2(start_position.x + height + height / 2 + style.FramePadding.x, start_position.y));
            ImGui::Text("%s", extruders[extruder_idx].c_str());
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    ImVec2      backup_pos = ImGui::GetCursorScreenPos();
    ImGuiStyle &style      = ImGui::GetStyle();

    ImGui::SetCursorScreenPos(ImVec2(combo_pos.x + style.FramePadding.x, combo_pos.y + style.FramePadding.y));
    ImVec2 p      = ImGui::GetCursorScreenPos();
    float  height = ImGui::GetTextLineHeight();

    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + height + height / 2, p.y + height), ImGuiWrapper::to_ImU32(extruders_colors[selection_idx]));
    ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + height + height / 2, p.y + height), IM_COL32_BLACK);

    ImGui::SetCursorScreenPos(ImVec2(p.x + height + height / 2 + style.FramePadding.x, p.y));
    ImGui::Text("%s", extruders[selection_out].c_str());
    ImGui::SetCursorScreenPos(backup_pos);
    ImGui::EndGroup();

    selection_idx = selection_out;
}

void GLGizmoMmuSegmentation::render_tooltip_button(float x, float y)
{
    auto get_shortcuts = [this]() -> std::vector<std::pair<wxString, wxString>> {
        switch (m_tool_type) {
        case ToolType::BRUSH: return m_shortcuts_brush;

        case ToolType::BUCKET_FILL:
        case ToolType::SMART_FILL: return m_shortcuts_bucket_fill;

        case ToolType::GAP_FILL: return m_shortcuts_gap_fill;

        default: return {};
        }
    };

    GLGizmoUtils::render_tooltip_button(m_imgui, m_parent, get_shortcuts(), x, y);
}

// ORCA
bool GLGizmoMmuSegmentation::draw_color_button(int idx, std::string id_str, const ColorRGBA& color, ColorRGBA& map_color, bool active, float scale)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    std::string label_id  = std::to_string(idx) + id_str + std::to_string(idx);
    ImVec2      pos       = ImGui::GetCursorScreenPos();
    ImVec2      size      = ImVec2(27.f * scale, 27.f * scale);
    ImVec4      color_vec = ImGuiWrapper::to_ImVec4(color);
    ImU32       br_color  = ImGui::ColorConvertFloat4ToU32(active ? ImGuiWrapper::COL_ORCA : m_is_dark_mode ? ImVec4(.35f, .35f, .35f, 1) : ImVec4(.85f, .85f, .85f, 1));
    bool        dark_tone = (0.299f * color.r() + 0.587f * color.g() + 0.114f * color.b()) < 0.51f; // matching values used by wxWidgets with clr.GetLuminance() < 0.51

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding  , 7.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding   , ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text         , dark_tone ? ImVec4(1,1,1,1) : ImVec4(0,0,0,1));
    ImGui::PushStyleColor(ImGuiCol_Button       , color_vec);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color_vec);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive , color_vec);
    bool clicked = ImGui::Button(label_id.c_str(), size);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);

    auto drawBorder = [&](float d, float r, float t, ImU32 col) {
        draw_list->AddRect({pos.x + d * scale, pos.y + d * scale}, {pos.x + size.x - d * scale , pos.y + size.y - d * scale}, col, r * scale, 0, t * scale);
    };
    drawBorder(1.5f, 3.f, 4.f, ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_WindowBg)));
    if(active)
        drawBorder(.5f, 4.f , 2.f, br_color);
    else
        drawBorder(3.f, 2.5f, 1.f, br_color);

    if (color != map_color){ // show mapped color as bubble if mapped
        ImVec2 center = {pos.x + size.x - 3.f * scale, pos.y + 3.f * scale};
        draw_list->AddCircleFilled(center, 6.f * scale, br_color, 16); // outer border for better visibility
        draw_list->AddCircleFilled(center, 5.f * scale, ImGuiWrapper::to_ImU32(map_color), 16);
    }

    return clicked;
};

void GLGizmoMmuSegmentation::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c->selection_info()->model_object()) return;

    float  scale       = m_parent.get_scale();
    #ifdef WIN32
        int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        scale *= (float) dpi / (float) DPI_DEFAULT;
    #endif // WIN32

    const float approx_height = m_imgui->scaled(22.0f);
    y = std::min(y, bottom_limit - approx_height);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always);

    wchar_t old_tool = m_current_tool;

    // BBS
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // First calculate width of all the texts that are could possibly be shown. We will decide set the dialog width based on that:
    const float space_size = m_imgui->get_style_scaling() * 8;
    const float clipping_slider_left  = std::max(m_imgui->calc_text_size(m_desc.at("clipping_of_view")).x + m_imgui->scaled(1.5f),
        m_imgui->calc_text_size(m_desc.at("reset_direction")).x + m_imgui->scaled(1.5f) + ImGui::GetStyle().FramePadding.x * 2);
    const float cursor_slider_left = m_imgui->calc_text_size(m_desc.at("cursor_size")).x + m_imgui->scaled(1.5f);
    const float smart_fill_slider_left = m_imgui->calc_text_size(m_desc.at("smart_fill_angle")).x + m_imgui->scaled(1.5f);
    const float edge_detect_slider_left = m_imgui->calc_text_size(m_desc.at("edge_detection")).x + m_imgui->scaled(1.f);
    const float gap_area_slider_left = m_imgui->calc_text_size(m_desc.at("gap_area")).x + m_imgui->scaled(1.5f) + space_size;
    const float height_range_slider_left = m_imgui->calc_text_size(m_desc.at("height_range")).x + m_imgui->scaled(2.f);

    const float remove_btn_width = m_imgui->calc_text_size(m_desc.at("remove_all")).x + m_imgui->scaled(1.f);
    const float filter_btn_width = m_imgui->calc_text_size(m_desc.at("perform")).x + m_imgui->scaled(1.f);
    const float remap_btn_width = m_imgui->calc_text_size(m_desc.at("perform_remap")).x + m_imgui->scaled(1.f);
    const float buttons_width = remove_btn_width + filter_btn_width + remap_btn_width + m_imgui->scaled(2.f);
    const float minimal_slider_width = m_imgui->scaled(4.f);
    const float color_button_width = m_imgui->calc_text_size(std::string_view{""}).x + m_imgui->scaled(1.75f);

    float caption_max = 0.f;
    float total_text_max = 0.f;
    for (const auto &t : std::array<std::string, 6>{"paint", "erase", "cursor_size", "smart_fill_angle", "height_range", "clipping_of_view"}) {
        caption_max = std::max(caption_max, m_imgui->calc_text_size(m_desc[t + "_caption"]).x);
        total_text_max = std::max(total_text_max, m_imgui->calc_text_size(m_desc[t]).x);
    }
    total_text_max += caption_max + m_imgui->scaled(1.f);
    caption_max += m_imgui->scaled(1.f);

    const float circle_max_width = std::max(clipping_slider_left,cursor_slider_left);
    const float height_max_width = std::max(clipping_slider_left,height_range_slider_left);
    const float sliders_left_width = std::max(smart_fill_slider_left,
                                         std::max(cursor_slider_left, std::max(edge_detect_slider_left, std::max(gap_area_slider_left, std::max(height_range_slider_left,
                                                                                                                                              clipping_slider_left))))) + space_size;
    const float slider_icon_width = m_imgui->get_slider_icon_size().x;
    float window_width = minimal_slider_width + sliders_left_width + slider_icon_width;
    const int max_filament_items_per_line = 8;
    const float empty_button_width = m_imgui->calc_button_size("").x;
    const float filament_item_width = empty_button_width + m_imgui->scaled(1.5f);

    window_width = std::max(window_width, total_text_max);
    window_width = std::max(window_width, buttons_width);
    window_width = std::max(window_width, max_filament_items_per_line * filament_item_width + +m_imgui->scaled(0.5f));

    const float sliders_width = m_imgui->scaled(7.0f);
    const float drag_left_width = ImGui::GetStyle().WindowPadding.x + sliders_width - space_size;

    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;

    m_imgui->text(m_desc.at("filaments"));

    size_t n_extruder_colors = std::min((size_t)EnforcerBlockerType::ExtruderMax, m_extruders_colors.size());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(7.f * scale, 7.f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 0); // removes extra space on tree node indentation
    for (int extruder_idx = 0; extruder_idx < n_extruder_colors; extruder_idx++) {

        if (extruder_idx % max_filament_items_per_line != 0)
            ImGui::SameLine();

        if (draw_color_button(
            extruder_idx + 1,                        // idx
            "###extruder_color_",                    // button_id
            m_extruders_colors[extruder_idx],        // color
            m_extruders_colors[extruder_idx],        // mapped_color (not used in here)
            m_selected_extruder_idx == extruder_idx, // is_active
            scale
        )){
            m_selected_extruder_idx = extruder_idx;
        }

        if (extruder_idx < 16 && ImGui::IsItemHovered()) m_imgui->tooltip(_L("Shortcut Key ") + std::to_string(extruder_idx + 1), max_tooltip_width);
    }
    // ORCA: Remap filaments section (Border only, Title in border). 
    // Styled as a panel for visual grouping.
    if (ImGui::TreeNodeEx(m_desc.at("perform_remap").c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding)){
        render_filament_remap_ui(window_width, max_tooltip_width, scale);

        bool has_mapping = false;
        for (size_t i = 0; i < m_extruder_remap.size(); ++i){
            if(m_extruder_remap[i] != i){
                has_mapping = true;
                break;
            }
        }

        ImGui::Dummy(ImVec2(0,0));

        // ORCA: Add Remap and Cancel buttons (outside the panel)
        m_imgui->disabled_begin(!has_mapping); // disable when no mapping
        if (m_imgui->button(m_desc.at("remap"))) {
            this->remap_filament_assignments();
            // Reset mapping to identity after apply
            for (size_t i = 0; i < m_extruder_remap.size(); ++i) m_extruder_remap[i] = i;
        }
        m_imgui->disabled_end(/*m_is_unknown_font*/);

        if (has_mapping){  // show only when it has mapping
            ImGui::SameLine();
            if (m_imgui->button(m_desc.at("remap_reset"))) {
                // Reset mapping to identity
                for (size_t i = 0; i < m_extruder_remap.size(); ++i) m_extruder_remap[i] = i;
            }
        }

        //ImGui::Dummy(ImVec2(0.0f, 3.f * scale));
        ImGui::TreePop();
    }
    ImGui::PopStyleVar(2); // IndentSpacing ItemSpacing

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    m_imgui->text(m_desc.at("tool_type"));

    std::array<wchar_t, 6> tool_ids;
    tool_ids = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::HeightRangeIcon, ImGui::FillButtonIcon, ImGui::GapFillIcon };
    std::array<wchar_t, 6> icons;
    if (m_is_dark_mode)
        icons = { ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon, ImGui::TriangleButtonDarkIcon, ImGui::HeightRangeDarkIcon, ImGui::FillButtonDarkIcon, ImGui::GapFillDarkIcon };
    else
        icons = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::HeightRangeIcon, ImGui::FillButtonIcon, ImGui::GapFillIcon };
    std::array<wxString, 6> tool_tips = { _L("Circle"), _L("Sphere"), _L("Triangle"), _L("Height Range"), _L("Fill"), _L("Gap Fill") };
    for (int i = 0; i < tool_ids.size(); i++) {
        std::string  str_label = std::string("");
        std::wstring btn_name  = icons[i] + boost::nowide::widen(str_label);

        if (i != 0) ImGui::SameLine((empty_button_width + m_imgui->scaled(1.75f)) * i + m_imgui->scaled(1.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));                     // ORCA Removes button background on dark mode
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));                       // ORCA Fixes icon rendered without colors while using Light theme
        if (m_current_tool == tool_ids[i]) {
            ImGui::PushStyleColor(ImGuiCol_Button,          ImVec4(0.f, 0.59f, 0.53f, 0.25f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,   ImVec4(0.f, 0.59f, 0.53f, 0.25f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,    ImVec4(0.f, 0.59f, 0.53f, 0.30f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_Border,          ImGuiWrapper::COL_ORCA);            // ORCA use orca color for border on selected tool / brush
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0);
        }
        bool btn_clicked = ImGui::Button(into_u8(btn_name).c_str());
        if (m_current_tool == tool_ids[i])
        {
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(1);

        if (btn_clicked && m_current_tool != tool_ids[i]) {
            m_current_tool = tool_ids[i];
            for (auto &triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        }

        if (ImGui::IsItemHovered()) {
            m_imgui->tooltip(tool_tips[i], max_tooltip_width);
        }
    }

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    if (m_current_tool != old_tool)
        this->tool_changed(old_tool, m_current_tool);

    if (m_current_tool == ImGui::CircleButtonIcon || m_current_tool == ImGui::SphereButtonIcon) {
        if (m_current_tool == ImGui::CircleButtonIcon)
            m_cursor_type = TriangleSelector::CursorType::CIRCLE;
        else
             m_cursor_type = TriangleSelector::CursorType::SPHERE;
        m_tool_type = ToolType::BRUSH;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");

        if (m_imgui->bbl_checkbox(_L("Vertical"), m_vertical_only)) {
            if (m_vertical_only) {
                m_horizontal_only = false;
            }
        }
        if (m_imgui->bbl_checkbox(_L("Horizontal"), m_horizontal_only)) {
            if (m_horizontal_only) {
                m_vertical_only = false;
            }
        }
    } 
    else if (m_current_tool == ImGui::TriangleButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::BRUSH;

        if (m_imgui->bbl_checkbox(_L("Vertical"), m_vertical_only)) {
            if (m_vertical_only) {
                m_horizontal_only = false;
            }
        }
        if (m_imgui->bbl_checkbox(_L("Horizontal"), m_horizontal_only)) {
            if (m_horizontal_only) {
                m_vertical_only = false;
            }
        }
    } 
    else if (m_current_tool == ImGui::FillButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type = ToolType::BUCKET_FILL;

        m_imgui->bbl_checkbox(m_desc["edge_detection"], m_detect_geometry_edge);

        if (m_detect_geometry_edge) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc["smart_fill_angle"]);
            std::string format_str = std::string("%.f") + I18N::translate_utf8("°", "Face angle threshold,"
                                                                                    "placed after the number with no whitespace in between.");
            ImGui::SameLine(sliders_left_width);
            ImGui::PushItemWidth(sliders_width);
            if (m_imgui->bbl_slider_float_style("##smart_fill_angle", &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, format_str.data(), 1.0f, true))
                for (auto &triangle_selector : m_triangle_selectors) {
                    triangle_selector->seed_fill_unselect_all_triangles();
                    triangle_selector->request_update_render_data();
                }
            ImGui::SameLine(drag_left_width + sliders_left_width);
            ImGui::PushItemWidth(1.5 * slider_icon_width);
            ImGui::BBLDragFloat("##smart_fill_angle_input", &m_smart_fill_angle, 0.05f, 0.0f, 0.0f, "%.2f");
        } else {
            // set to negative value to disable edge detection
            m_smart_fill_angle = -1.f;
        }
    } 
    else if (m_current_tool == ImGui::HeightRangeIcon) {
        m_tool_type   = ToolType::BRUSH;
        m_cursor_type = TriangleSelector::CursorType::HEIGHT_RANGE;
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["height_range"] + ":");
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        std::string format_str = std::string("%.2f") + I18N::translate_utf8("mm", "Height range," "Facet in [cursor z, cursor z + height] will be selected.");
        m_imgui->bbl_slider_float_style("##cursor_height", &m_cursor_height, CursorHeightMin, CursorHeightMax, format_str.data(), 1.0f, true);
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_height_input", &m_cursor_height, 0.05f, 0.0f, 0.0f, "%.2f");
    }
    else if (m_current_tool == ImGui::GapFillIcon) {
        m_tool_type = ToolType::GAP_FILL;
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["gap_area"] + ":");
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        std::string format_str = std::string("%.2f") + I18N::translate_utf8("", "Triangle patch area threshold,""triangle patch will be merged to neighbor if its area is less than threshold");
        m_imgui->bbl_slider_float_style("##gap_area", &TriangleSelectorPatch::gap_area, TriangleSelectorPatch::GapAreaMin, TriangleSelectorPatch::GapAreaMax, format_str.data(), 1.0f, true);
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##gap_area_input", &TriangleSelectorPatch::gap_area, 0.05f, 0.0f, 0.0f, "%.2f");
    }

    ImGui::Separator();
    if (m_c->object_clipper()->get_position() == 0.f) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("clipping_of_view"));
    } else {
        if (m_imgui->button(m_desc.at("reset_direction"))) {
            wxGetApp().CallAfter([this]() { m_c->object_clipper()->set_position_by_ratio(-1., false); });
        }
    }

    auto clp_dist = float(m_c->object_clipper()->get_position());
    ImGui::SameLine(sliders_left_width);
    ImGui::PushItemWidth(sliders_width);
    bool slider_clp_dist = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);
    ImGui::SameLine(drag_left_width + sliders_left_width);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    bool b_clp_dist_input = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");

    if (slider_clp_dist || b_clp_dist_input) {
        m_c->object_clipper()->set_position_by_ratio(clp_dist, true);
    }

    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    render_tooltip_button(x, y);

    float f_scale =m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    ImGui::SameLine();

    if (m_current_tool == ImGui::GapFillIcon) {
        if (m_imgui->button(m_desc.at("perform"))) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Gap fill", UndoRedo::SnapshotType::GizmoAction);

            for (int i = 0; i < m_triangle_selectors.size(); i++) {
                TriangleSelectorPatch* ts_mm = dynamic_cast<TriangleSelectorPatch*>(m_triangle_selectors[i].get());
                ts_mm->update_selector_triangles();
                ts_mm->request_update_render_data(true);
            }
            update_model_object();
            m_parent.set_as_dirty();
        }

        ImGui::SameLine();
    }

    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset selection", UndoRedo::SnapshotType::GizmoAction);
        ModelObject *        mo  = m_c->selection_info()->model_object();
        int                  idx = -1;
        for (ModelVolume *mv : mo->volumes)
            if (mv->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data(true);
            }

        update_model_object();
        m_parent.set_as_dirty();
    }
    ImGui::PopStyleVar(2);
    GizmoImguiEnd();

    // BBS
    ImGuiWrapper::pop_toolbar_style();
}


void GLGizmoMmuSegmentation::update_model_object()
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info()->model_object();
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->mmu_segmentation_facets.set(*m_triangle_selectors[idx].get());
    }

    if (updated) {
        const ModelObjectPtrs &mos = wxGetApp().model().objects;
        size_t obj_idx = std::find(mos.begin(), mos.end(), mo) - mos.begin();
        wxGetApp().obj_list()->update_info_items(obj_idx);
        wxGetApp().plater()->get_partplate_list().notify_instance_update(obj_idx, 0);
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));

        // ORCA: Refresh cache
        this->update_used_filaments();
    }
}

void GLGizmoMmuSegmentation::init_model_triangle_selectors()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();
    m_volumes_extruder_idxs.clear();

    // Don't continue when extruders colors are not initialized
    if(m_extruders_colors.empty())
        return;

    // BBS: Don't continue when model object is null
    if (mo == nullptr)
        return;

    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        int extruder_idx = (mv->extruder_id() > 0) ? mv->extruder_id() - 1 : 0;
        std::vector<ColorRGBA> ebt_colors;
        ebt_colors.push_back(m_extruders_colors[size_t(extruder_idx)]);
        ebt_colors.insert(ebt_colors.end(), m_extruders_colors.begin(), m_extruders_colors.end());

        // This mesh does not account for the possible Z up SLA offset.
        const TriangleMesh* mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors, 0.2));
        // Reset of TriangleSelector is done inside TriangleSelectorMmGUI's constructor, so we don't need it to perform it again in deserialize().
        EnforcerBlockerType max_ebt = (EnforcerBlockerType)std::min(m_extruders_colors.size(), (size_t)EnforcerBlockerType::ExtruderMax);
        m_triangle_selectors.back()->deserialize(mv->mmu_segmentation_facets.get_data(), false, max_ebt);
        m_triangle_selectors.back()->request_update_render_data();
        m_triangle_selectors.back()->set_wireframe_needed(true);
        m_volumes_extruder_idxs.push_back(mv->extruder_id());
    }
}

void GLGizmoMmuSegmentation::update_triangle_selectors_colors()
{
    for (int i = 0; i < m_triangle_selectors.size(); i++) {
        TriangleSelectorPatch* selector = dynamic_cast<TriangleSelectorPatch*>(m_triangle_selectors[i].get());
        int extruder_idx = m_volumes_extruder_idxs[i];
        int extruder_color_idx = std::max(0, extruder_idx - 1);
        std::vector<ColorRGBA> ebt_colors;
        ebt_colors.push_back(m_extruders_colors[extruder_color_idx]);
        ebt_colors.insert(ebt_colors.end(), m_extruders_colors.begin(), m_extruders_colors.end());
        selector->set_ebt_colors(ebt_colors);
    }
}

void GLGizmoMmuSegmentation::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    // Extruder colors need to be reloaded before calling init_model_triangle_selectors to render painted triangles
    // using colors from loaded 3MF and not from printer profile in Slicer.
    if (int prev_extruders_count = int(m_extruders_colors.size());
        prev_extruders_count != wxGetApp().filaments_cnt() || wxGetApp().plater()->get_extruders_colors() != m_extruders_colors)
        this->init_extruders_data();

    this->init_model_triangle_selectors();

    // ORCA: Refresh cache when model changes
    this->update_used_filaments();
}

void GLGizmoMmuSegmentation::tool_changed(wchar_t old_tool, wchar_t new_tool)
{
    if ((old_tool == ImGui::GapFillIcon && new_tool == ImGui::GapFillIcon) ||
        (old_tool != ImGui::GapFillIcon && new_tool != ImGui::GapFillIcon))
        return;

    for (auto& selector_ptr : m_triangle_selectors) {
        TriangleSelectorPatch* tsp = dynamic_cast<TriangleSelectorPatch*>(selector_ptr.get());
        tsp->set_filter_state(new_tool == ImGui::GapFillIcon);
    }
}

PainterGizmoType GLGizmoMmuSegmentation::get_painter_type() const
{
    return PainterGizmoType::MM_SEGMENTATION;
}

// BBS
ColorRGBA GLGizmoMmuSegmentation::get_cursor_hover_color() const
{
    if (m_selected_extruder_idx < m_extruders_colors.size())
        return m_extruders_colors[m_selected_extruder_idx];
    else
        return m_extruders_colors[0];
}

void GLGizmoMmuSegmentation::on_set_state()
{
    GLGizmoPainterBase::on_set_state();

    if (get_state() == Off) {
        ModelObject* mo = m_c->selection_info()->model_object();
        if (mo) Slic3r::save_object_mesh(*mo);
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_FORCE_UPDATE));
        if (m_current_tool == ImGui::GapFillIcon) {//exit gap fill
            m_current_tool = ImGui::CircleButtonIcon;
        }
    }
}

wxString GLGizmoMmuSegmentation::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    wxString action_name;
    if (shift_down)
        action_name = _L("Remove painted color");
    else {
        action_name        = GUI::format(_L("Painted using: Filament %1%"), m_selected_extruder_idx);
    }
    return action_name;
}

void GLMmSegmentationGizmo3DScene::release_geometry() {
    if (this->vertices_VBO_id) {
        glsafe(::glDeleteBuffers(1, &this->vertices_VBO_id));
        this->vertices_VBO_id = 0;
    }
    for(auto &triangle_indices_VBO_id : triangle_indices_VBO_ids) {
        glsafe(::glDeleteBuffers(1, &triangle_indices_VBO_id));
        triangle_indices_VBO_id = 0;
    }
#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        if (this->vertices_VAO_id > 0) {
            glsafe(::glDeleteVertexArrays(1, &this->vertices_VAO_id));
            this->vertices_VAO_id = 0;
        }
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES

    this->clear();
}

void GLMmSegmentationGizmo3DScene::render(size_t triangle_indices_idx) const
{
    assert(triangle_indices_idx < this->triangle_indices_VBO_ids.size());
    assert(this->triangle_patches.size() == this->triangle_indices_VBO_ids.size());
#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        assert(this->vertices_VAO_id != 0);
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES
    assert(this->vertices_VBO_id != 0);
    assert(this->triangle_indices_VBO_ids[triangle_indices_idx] != 0);

    GLShaderProgram* shader = wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        glsafe(::glBindVertexArray(this->vertices_VAO_id));
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES
    // the following binding is needed to set the vertex attributes
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, this->vertices_VBO_id));
    const GLint position_id = shader->get_attrib_location("v_position");
    if (position_id != -1) {
        glsafe(::glVertexAttribPointer(position_id, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (GLvoid*)0));
        glsafe(::glEnableVertexAttribArray(position_id));
    }

    // Render using the Vertex Buffer Objects.
    if (this->triangle_indices_VBO_ids[triangle_indices_idx] != 0 &&
        this->triangle_indices_sizes[triangle_indices_idx] > 0) {
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->triangle_indices_VBO_ids[triangle_indices_idx]));
        glsafe(::glDrawElements(GL_TRIANGLES, GLsizei(this->triangle_indices_sizes[triangle_indices_idx]), GL_UNSIGNED_INT, nullptr));
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
    }

    if (position_id != -1)
        glsafe(::glDisableVertexAttribArray(position_id));

    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        glsafe(::glBindVertexArray(0));
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES
}

void GLMmSegmentationGizmo3DScene::finalize_vertices()
{
#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        assert(this->vertices_VAO_id == 0);
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES
    assert(this->vertices_VBO_id == 0);
    if (!this->vertices.empty()) {
#if !SLIC3R_OPENGL_ES
        if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
            glsafe(::glGenVertexArrays(1, &this->vertices_VAO_id));
            glsafe(::glBindVertexArray(this->vertices_VAO_id));
#if !SLIC3R_OPENGL_ES
        }
#endif // !SLIC3R_OPENGL_ES

        glsafe(::glGenBuffers(1, &this->vertices_VBO_id));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, this->vertices_VBO_id));
        glsafe(::glBufferData(GL_ARRAY_BUFFER, this->vertices.size() * sizeof(float), this->vertices.data(), GL_STATIC_DRAW));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
        this->vertices.clear();

#if !SLIC3R_OPENGL_ES
        if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
            glsafe(::glBindVertexArray(0));
#if !SLIC3R_OPENGL_ES
        }
#endif // !SLIC3R_OPENGL_ES
    }
}

void GLMmSegmentationGizmo3DScene::finalize_triangle_indices()
{
    triangle_indices_VBO_ids.resize(this->triangle_patches.size());
    triangle_indices_sizes.resize(this->triangle_patches.size());
    assert(std::all_of(triangle_indices_VBO_ids.cbegin(), triangle_indices_VBO_ids.cend(), [](const auto &ti_VBO_id) { return ti_VBO_id == 0; }));

    for (size_t buffer_idx = 0; buffer_idx < this->triangle_patches.size(); ++buffer_idx) {
        std::vector<int>& triangle_indices = this->triangle_patches[buffer_idx].triangle_indices;
        triangle_indices_sizes[buffer_idx] = triangle_indices.size();
        if (!triangle_indices.empty()) {
            glsafe(::glGenBuffers(1, &this->triangle_indices_VBO_ids[buffer_idx]));
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->triangle_indices_VBO_ids[buffer_idx]));
            glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, triangle_indices.size() * sizeof(int), triangle_indices.data(), GL_STATIC_DRAW));
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
            triangle_indices.clear();
        }
    }
}

// ORCA: Update the cache of used filaments (both base volume extruders and painted triangles)
void GLGizmoMmuSegmentation::update_used_filaments()
{
    m_used_filaments.clear();

    // Add base extruder IDs from volumes (unpainted areas)
    for (int ext_id : m_volumes_extruder_idxs) {
        // ext_id is 1-based (1 = Extruder 1), 0 = Default (usually maps to first available or object default)
        // Here we assume 0 maps to index 0 (Extruder 1) for simplicity in display, 
        // or we should check logic in init_model_triangle_selectors where it does:
        // int extruder_idx = (mv->extruder_id() > 0) ? mv->extruder_id() - 1 : 0;
        int idx = (ext_id > 0) ? ext_id - 1 : 0;
        if (idx >= 0 && idx < m_extruders_colors.size())
             m_used_filaments.insert((size_t)idx);
    }

    // Add painted states
    for (const auto& selector : m_triangle_selectors) {
        if (!selector) continue;
        TriangleSelector::TriangleSplittingData data = selector->serialize();
        std::vector<EnforcerBlockerType> states = TriangleSelector::extract_used_facet_states(data);
        for (EnforcerBlockerType s : states) {
             int idx = (int)s - (int)EnforcerBlockerType::Extruder1;
             if (idx >= 0 && idx < m_extruders_colors.size())
                 m_used_filaments.insert((size_t)idx);
        }
    }
}

void GLGizmoMmuSegmentation::render_filament_remap_ui(float window_width, float max_tooltip_width, float scale)
{
    size_t n_extr = std::min((size_t)EnforcerBlockerType::ExtruderMax, m_extruders_colors.size());

    int displayed_count = 0;
    const int max_per_line = 8;

    // ORCA: Use m_used_filaments to show only relevant source filaments
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(7.f * scale, 7.f * scale));
    for (size_t src : m_used_filaments) {
        if (src >= n_extr) continue;

        if (displayed_count > 0 && (displayed_count % max_per_line != 0))
            ImGui::SameLine();
        
        std::string pop_id = "popup_" + std::to_string(src);

        bool src_clicked = draw_color_button(
            (int)src + 1,                              // idx
            "###remap_src_",                           // button_id
            m_extruders_colors[src],                   // color
            m_extruders_colors[m_extruder_remap[src]], // mapped_color (shows bubble if not matches with Color)
            ImGui::IsPopupOpen(pop_id.c_str()),        // is_active
            scale
        );

        if (src_clicked) {
            // Calculate popup position centered below the current button
            ImVec2 button_pos = ImGui::GetItemRectMin();
            ImVec2 button_size = ImGui::GetItemRectSize();

            // Ensure popup is within the main viewport bounds
            int   dst_count   = (int)std::min(n_extr, (size_t)max_per_line);
            float est_popup_w = button_size.x * dst_count
                              + ImGui::GetStyle().ItemSpacing.x * (dst_count - 1)
                              + ImGui::GetStyle().WindowPadding.x * 2.f;

            ImGuiViewport* vp = ImGui::GetMainViewport();
            float right_limit = vp->WorkPos.x + vp->WorkSize.x - est_popup_w * 0.5f; // pivot is 0.5 so subtract half
            float centered_x  = button_pos.x + button_size.x * 0.5f;                 // pivot 0.5 just needs center x

            ImVec2 popup_pos(std::min(centered_x, right_limit), button_pos.y + button_size.y);

            ImGui::SetNextWindowPos(popup_pos, ImGuiCond_Appearing, ImVec2(0.5f, -0.1f));
            ImGui::SetNextWindowBgAlpha(1.0f); // Ensure full opacity
            ImGui::OpenPopup(pop_id.c_str());
        }

        if (ImGui::IsItemHovered() && src != m_extruder_remap[src]) // show tooltip if it has mapping info
            m_imgui->tooltip(std::to_string(src + 1) + " >> " + std::to_string(m_extruder_remap[src] + 1), max_tooltip_width);
        
        // Apply popup styling before BeginPopup using standard Orca colors
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding  , 8.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 2.0f * scale); // thicker & colored border to prevent mixing with main window. Current ImGui version not supports shadows
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        ImGui::PushStyleColor(ImGuiCol_Border , ImGui::ColorConvertFloat4ToU32(ImGuiWrapper::COL_ORCA));
        
        if (ImGui::BeginPopup(pop_id.c_str())) {
            
            m_imgui->text(_L("To:"));

            for (int dst = 0; dst < (int)n_extr; ++dst) {
                if (dst > 0 && (dst % max_per_line != 0))
                     ImGui::SameLine();
                bool dst_clicked = draw_color_button(
                    dst + 1,                      // idx
                    "###remap_dst_",              // button_id
                    m_extruders_colors[dst],      // color
                    m_extruders_colors[dst],      // mapped_color (non fuctional in here)
                    m_extruder_remap[src] == dst, // is_active
                    scale
                );
                if (dst_clicked) {
                    m_extruder_remap[src] = dst;
                    // update the source button color immediately
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::Dummy(ImVec2(0.0f, 2.f * scale));
            ImGui::EndPopup();
        }
        
        // Clean up popup styling (always pop, whether popup was open or not)
        ImGui::PopStyleColor(2); // PopupBg and Border
        ImGui::PopStyleVar(2);   // PopupRounding and PopupBorderSize
        
        displayed_count++;
    }
    ImGui::PopStyleVar(1); // ItemSpacing
}

void GLGizmoMmuSegmentation::remap_filament_assignments()
{
    if (m_extruder_remap.empty())
        return;

    constexpr size_t MAX_EBT = (size_t)EnforcerBlockerType::ExtruderMax;
    EnforcerBlockerStateMap state_map;

    // identity mapping by default
    for (size_t i = 0; i <= MAX_EBT; ++i)
        state_map[i] = static_cast<EnforcerBlockerType>(i);

    size_t n_extr = std::min(m_extruder_remap.size(), MAX_EBT);
    const int start_extruder = (int) EnforcerBlockerType::Extruder1;
    bool   any_change = false;
    for (size_t src = 0; src < n_extr; ++src) {
        size_t dst = m_extruder_remap[src];
        if (dst != src) {
            state_map[src+start_extruder] = static_cast<EnforcerBlockerType>(dst+start_extruder);
            any_change     = true;
        }
    }
    if (!any_change)
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(),
                                  "Remap filament assignments",
                                  UndoRedo::SnapshotType::GizmoAction);

    bool updated = false;
    int idx = -1;
    ModelObject* mo = m_c->selection_info()->model_object();
    if (!mo) return;

    bool volume_extruder_changed = false;

    for (ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        ++idx;
        TriangleSelectorGUI* ts = m_triangle_selectors[idx].get();
        if (!ts) continue;

        // Remap painted triangles
        ts->remap_triangle_state(state_map);
        ts->request_update_render_data(true);

        // ORCA: Remap base volume extruder as well if selected
        int current_ext_id = mv->extruder_id();
        int current_idx = (current_ext_id > 0) ? current_ext_id - 1 : 0;

        if (current_idx >= 0 && current_idx < m_extruder_remap.size()) {
            size_t dest_idx = m_extruder_remap[current_idx];
            if (dest_idx != current_idx) {
                // Check if volume has its own extruder config or uses object's fallback                                                                                                                                            
                const ConfigOption *vol_opt = mv->config.option("extruder");                                                                                                                                                        
                if (vol_opt != nullptr && vol_opt->getInt() != 0) {                                                                                                                                                                 
                    // Volume has its own extruder setting, update it                                                                                                                                                               
                    mv->config.set("extruder", (int)dest_idx + 1);                                                                                                                                                                  
                } else {                                                                                                                                                                                                            
                    // Volume uses object's extruder setting, update the object                                                                                                                                                     
                    mo->config.set("extruder", (int)dest_idx + 1);                                                                                                                                                                  
                }      
                if (idx < m_volumes_extruder_idxs.size())
                    m_volumes_extruder_idxs[idx] = (int)dest_idx + 1;
                volume_extruder_changed = true;
            }
        }

        updated = true;
    }

    if (updated) {
        // ORCA: Update renderer colors if base volume extruder changed
        if (volume_extruder_changed) {
            this->update_triangle_selectors_colors();
            // ORCA: Update GUI_ObjectList extruder column to reflect the new extruder value
            wxGetApp().obj_list()->update_objects_list_filament_column(wxGetApp().filaments_cnt());
        }

        // ORCA: Removed "Filament remapping finished" notification to reduce UI noise.
        update_model_object();
        m_parent.set_as_dirty();
        
        // ORCA: Refresh used filaments cache
        this->update_used_filaments();
    }
}

} // namespace Slic3r
