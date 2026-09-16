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
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
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
    // Orca: closing the gizmo skips the panel's commit, so finish an unfinished pattern edit here, before the object
    // pointer is cleared: flush_periodic_patterns() writes only while the patterns still belong to the selected object.
    this->flush_periodic_patterns();
    m_periodic_patterns_object = nullptr;
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

    m_gradient_ramps = wxGetApp().plater()->get_filament_gradient_ramps();
    m_gradient_ramps.resize(m_extruders_colors.size());

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
    m_desc["perform"]          = _L("Apply");
    m_desc["remove_all"]       = _L("Erase all");
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
    std::pair<wxString, wxString> toggle_wireframe_shortcut = {alt + shift + _L_CONTEXT("Enter", "Keyboard Shortcut"),       m_desc["toggle_wireframe"]};

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
    // Orca: after the mesh, so the band tint blends over it.
    render_periodic_bands();

    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

void GLGizmoMmuSegmentation::data_changed(bool is_serializing)
{
    GLGizmoPainterBase::data_changed(is_serializing);

    // Undo and redo can change the patterns without replacing the object, which load_periodic_patterns() would not
    // notice, so force a reload.
    m_periodic_patterns_object = nullptr;
    // Also drop any unfinished edit: the model changed under it, and with nothing selected the reload returns early
    // without clearing it.
    m_periodic_patterns_dirty  = false;

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
bool GLGizmoMmuSegmentation::draw_color_button(int idx, const char* id_str, const ColorRGBA& color, ColorRGBA& map_color, bool active, float scale)
{
    // Inset of the frame stroked below, which is what trims the swatch down to its visible shape.
    const float frame_inset = 1.5f;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    std::string label_id  = std::to_string(idx) + id_str + std::to_string(idx);
    ImVec2      pos       = ImGui::GetCursorScreenPos();
    ImVec2      size      = ImVec2(27.f * scale, 27.f * scale);
    ImVec4      color_vec = ImGuiWrapper::to_ImVec4(color);
    ImU32       br_color  = ImGui::ColorConvertFloat4ToU32(active ? ImGuiWrapper::COL_ORCA : m_is_dark_mode ? ImVec4(.35f, .35f, .35f, 1) : ImVec4(.85f, .85f, .85f, 1));
    // Every caller labels the button with the 1 based slot number, so idx - 1 picks out the slot's fade.
    const std::vector<wxColour>* gradient = gradient_of(idx - 1);
    // The centered slot number sits at the swatch's mid height, so take its contrast from the colour
    // printed there rather than from the slot's blended color.
    bool dark_tone = gradient ? (*gradient)[gradient->size() / 2].GetLuminance() < 0.51 :
                                (0.299f * color.r() + 0.587f * color.g() + 0.114f * color.b()) < 0.51f; // matching values used by wxWidgets with clr.GetLuminance() < 0.51

    // Paint a gradient mixed filament's fade before the button and keep the button transparent, so
    // the slot number and the frame below stay on top of it. The bands cannot round their corners,
    // so the fade is inset to the frame, which masks it into the shape a plain color slot gets.
    if (gradient) {
        ImGuiWrapper::draw_gradient_ramp(draw_list, {pos.x + frame_inset * scale, pos.y + frame_inset * scale},
                                         {pos.x + size.x - frame_inset * scale, pos.y + size.y - frame_inset * scale}, *gradient);
        color_vec.w = 0.f; // let the fade show through
    }

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
    drawBorder(frame_inset, 3.f, 4.f, ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_WindowBg)));
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
    float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

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

    const float filter_btn_width = m_imgui->calc_text_size(m_desc.at("perform")).x + m_imgui->scaled(1.f);
    const float remap_btn_width = m_imgui->calc_text_size(m_desc.at("perform_remap")).x + m_imgui->scaled(1.f);
    const float buttons_width = filter_btn_width + remap_btn_width + m_imgui->scaled(2.f);
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

        if (extruder_idx < int(GLGizmoMmuSegmentation::EXTRUDERS_LIMIT) && ImGui::IsItemHovered()) m_imgui->tooltip(_L("Shortcut Key ") + std::to_string(extruder_idx + 1), max_tooltip_width);
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
        //std::string  str_label = std::string("");
        //std::wstring btn_name  = icons[i] + boost::nowide::widen(str_label);

        if (i != 0) ImGui::SameLine((empty_button_width + m_imgui->scaled(1.75f)) * i + m_imgui->scaled(1.5f));

        bool is_active = m_current_tool == tool_ids[i];
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding  , 3.f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding   , ImVec2(4.f * scale, 4.f * scale));
        ImGui::PushStyleColor(ImGuiCol_Text         , ImVec4(1,1,1,1)); // ORCA Fixes icon rendered without colors while using Light theme
        ImGui::PushStyleColor(ImGuiCol_Button       , is_active ? ImVec4(0.f, .59f, .53f, .25f) : ImVec4(0,0,0,0));         // ORCA
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, is_active ? ImVec4(0.f, .59f, .53f, .25f) : ImVec4(.6f,.6f,.6f,.2f)); // ORCA
        ImGui::PushStyleColor(ImGuiCol_ButtonActive , is_active ? ImVec4(0.f, .59f, .53f, .30f) : ImVec4(0,0,0,0));         // ORCA
        ImGui::PushStyleColor(ImGuiCol_Border       , is_active ? ImGuiWrapper::COL_ORCA        : ImVec4(0,0,0,0));         // ORCA
        ImGui::PushStyleColor(ImGuiCol_BorderActive , is_active ? ImGuiWrapper::COL_ORCA        : ImVec4(0,0,0,0));         // ORCA matched color for fixing flicker on click
        bool btn_clicked = m_imgui->glyph_button(icons[i], ImVec2(16.f  * scale, 16.f  * scale)); // ORCA glyph_button for fixing unequal paddings
        ImGui::PopStyleColor(6);
        ImGui::PopStyleVar(3);

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
                
        m_imgui->bbl_checkbox(m_desc["edge_detection"], m_detect_geometry_edge);
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

        // Apply Gap fill button
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
    }

    // Orca: periodic recolor patterns, in a section under the Height range tool. While the section is hidden, commit an
    // unfinished pattern edit, because its fields are no longer drawn to commit it.
    const bool periodic_ui_open = m_current_tool == ImGui::HeightRangeIcon &&
                                  ImGui::TreeNodeEx(_u8L("Periodic Height Range").c_str(), ImGuiTreeNodeFlags_SpanAvailWidth |
                                                    ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_NoTreePushOnOpen);
    if (periodic_ui_open)
        this->render_periodic_recolor_ui(window_width, sliders_left_width, sliders_width, drag_left_width, slider_icon_width, scale);
    else
        this->flush_periodic_patterns();

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

    render_tooltip_button(x, y);

    ImGui::SameLine();
    m_imgui->disabled_begin(m_c->selection_info()->model_object()->is_mm_painted() == false);
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
    m_imgui->disabled_end();

    ImGui::SameLine();
    GLGizmoUtils::begin_right_aligned_buttons({_L("Done")});
    if (m_imgui->button(_L("Done"))) {
        m_parent.reset_all_gizmos();
    }

    ImGui::PopStyleVar(1); // ImGuiStyleVar_FramePadding
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
        // A volume may be assigned to a mixed-color slot, whose index can sit past the
        // physical colour list; fall back to the first colour rather than reading OOB.
        if (extruder_idx >= (int)m_extruders_colors.size())
            extruder_idx = 0;
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
        // A mixed-color slot can index past the physical colour list; fall back to the first colour.
        if (extruder_color_idx >= (int)m_extruders_colors.size())
            extruder_color_idx = 0;
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


// ---------------------------------------------------------------------------------------
// Orca: periodic feature recoloring
// ---------------------------------------------------------------------------------------

// Clips a triangle to the Z range [lo, hi] and appends what is left to `out` as a fan, so the tint matches the band
// heights exactly (Sutherland-Hodgman against two horizontal planes). `poly` and `clipped` are scratch buffers the
// caller reuses across calls.
static void clip_triangle_to_slab(const Vec3f &a, const Vec3f &b, const Vec3f &c,
                                  float lo, float hi, GLModel::Geometry &out,
                                  std::vector<Vec3f> &poly, std::vector<Vec3f> &clipped)
{
    poly.assign({ a, b, c });
    for (int pass = 0; pass < 2; ++pass) {
        const bool  keep_above = (pass == 0);
        const float plane      = keep_above ? lo : hi;
        clipped.clear();
        clipped.reserve(poly.size() + 2);
        for (size_t i = 0; i < poly.size(); ++i) {
            const Vec3f &p0  = poly[i];
            const Vec3f &p1  = next_value_modulo(i, poly);
            const bool   in0 = keep_above ? (p0.z() >= plane) : (p0.z() <= plane);
            const bool   in1 = keep_above ? (p1.z() >= plane) : (p1.z() <= plane);
            if (in0)
                clipped.emplace_back(p0);
            if (in0 != in1) {
                const float dz = p1.z() - p0.z();
                clipped.emplace_back(p0 + (p1 - p0) * ((plane - p0.z()) / dz));
            }
        }
        poly.swap(clipped);
        if (poly.size() < 3)
            return;
    }
    const unsigned int base = (unsigned int) out.vertices_count();
    for (const Vec3f &v : poly)
        out.add_vertex(v);
    for (unsigned int i = 2; i < (unsigned int) poly.size(); ++i)
        out.add_triangle(base, base + i - 1, base + i);
}

// World Z range of one instance, for the band geometry and the height sliders. Uses the cached convex hulls, since
// this runs every frame, but falls back to the exact bounding box when a volume has no hull, because the hull
// bounding box leaves that volume out and would report too short a height.
static void periodic_instance_z(const ModelObject &mo, size_t instance_idx, double &min_z, double &size_z)
{
    for (const ModelVolume *mv : mo.volumes)
        if (mv->is_model_part() && mv->get_convex_hull().its.indices.empty()) {
            const BoundingBoxf3 bbox = mo.instance_bounding_box(instance_idx);
            min_z  = bbox.min.z();
            size_z = bbox.size().z();
            return;
        }
    const BoundingBoxf3 hull_bbox = mo.instance_convex_hull_bounding_box(instance_idx);
    min_z  = hull_bbox.min.z();
    size_z = hull_bbox.size().z();
}

void GLGizmoMmuSegmentation::update_periodic_band_models()
{
    // Most bands the preview builds per pattern; above it the preview stops partway up the object. The print is not
    // affected. Rebuilding runs on every slider move and grows with triangles x bands, so this trades a cut-off
    // preview for a smooth drag.
    static const size_t MAX_PREVIEW_BANDS = 512;

    const ModelObject *mo           = m_periodic_patterns_object;
    const int          instance_idx = m_parent.get_selection().get_instance_idx();
    if (mo == nullptr || instance_idx < 0 || instance_idx >= int(mo->instances.size()))
        return;

    const size_t      num_filaments = std::min((size_t) EnforcerBlockerType::ExtruderMax, m_extruders_colors.size());
    const Transform3d inst_matrix   = mo->instances[instance_idx]->get_transformation().get_matrix();

    // Z range of the whole object's model parts, the same volumes merged into world_its below. Not the selection's box:
    // selecting one volume must not move the bands.
    double object_min_z = 0., object_size_z = 0.;
    periodic_instance_z(*mo, size_t(instance_idx), object_min_z, object_size_z);

    // Rebuild only when these inputs change; slicing and clipping the mesh is too slow for every frame. The instance
    // matrix is included because a rotation can move the bands without changing the Z range.
    std::ostringstream key_stream;
    key_stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (double v : m_periodic_patterns.to_doubles())
        key_stream << v << ',';
    key_stream << ';' << object_min_z << ',' << object_size_z << ';' << num_filaments << ';' << instance_idx;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            key_stream << ',' << inst_matrix(r, c);
    const std::string key = key_stream.str();
    if (key == m_periodic_bands_key)
        return;
    m_periodic_bands_key = key;
    m_periodic_band_models.clear();
    // GLModel has a destructor and no move constructor, so an entry built outside the vector would be copied in.
    m_periodic_band_models.reserve(m_periodic_patterns.patterns.size());

    if (object_size_z <= 0.)
        return;

    indexed_triangle_set world_its;
    for (const ModelVolume *mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        indexed_triangle_set its = mv->mesh().its;
        // fix_left_handed: a mirrored instance or volume reverses the winding, and the tint renders with GL_CULL_FACE,
        // so it would disappear on mirrored parts.
        its_transform(its, inst_matrix * mv->get_matrix(), true);
        its_merge(world_its, its);
    }
    if (world_its.indices.empty())
        return;

    for (const PeriodicRecolorPattern &pattern : m_periodic_patterns.patterns) {
        if (! pattern.enabled || ! pattern.is_valid(num_filaments))
            continue;
        const std::vector<std::pair<double, double>> bands =
            periodic_recolor_ideal_bands(pattern, object_size_z, MAX_PREVIEW_BANDS);
        if (bands.empty())
            continue;

        // World Z of each band. Both edges ascend, which the merge and the binary search below rely on.
        std::vector<std::pair<float, float>> zbands;
        zbands.reserve(bands.size());
        for (const std::pair<double, double> &band : bands)
            zbands.emplace_back(float(object_min_z + band.first), float(object_min_z + band.second));

        // Merge overlapping bands for the tint, as build() does for the print. Clipping each separately would draw the
        // same surface many times, making the translucent tint opaque. The outlines keep every band, since each marks a
        // height the user asked for.
        std::vector<std::pair<float, float>> fill_bands;
        for (const std::pair<float, float> &zb : zbands)
            if (! fill_bands.empty() && zb.first <= fill_bands.back().second)
                fill_bands.back().second = std::max(fill_bands.back().second, zb.second);
            else
                fill_bands.emplace_back(zb);

        // The filament, not its color: the color is read at draw time.
        PeriodicBandModel &entry = m_periodic_band_models.emplace_back();
        entry.filament = pattern.filament;

        // ---- tinted surface, clipped to the band heights -------------------------------------
        GLModel::Geometry fill;
        fill.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        std::vector<Vec3f> clip_poly, clip_scratch;
        for (const stl_triangle_vertex_indices &face : world_its.indices) {
            const Vec3f &a = world_its.vertices[face(0)];
            const Vec3f &b = world_its.vertices[face(1)];
            const Vec3f &c = world_its.vertices[face(2)];
            const float  tri_lo = std::min({ a.z(), b.z(), c.z() });
            const float  tri_hi = std::max({ a.z(), b.z(), c.z() });
            auto zb = std::lower_bound(fill_bands.begin(), fill_bands.end(), tri_lo,
                                       [](const std::pair<float, float> &band, float z) { return band.second < z; });
            for (; zb != fill_bands.end() && zb->first <= tri_hi; ++zb)
                clip_triangle_to_slab(a, b, c, zb->first, zb->second, fill, clip_poly, clip_scratch);
        }
        if (fill.vertices_count() > 0)
            entry.fill.init_from(std::move(fill));

        // ---- outlines at the band edges ------------------------------------------------------
        // slice_mesh() needs sorted heights, and overlapping bands give unsorted pairs (thickness 4, period 2 gives
        // [0, 4, 2, 6]), which would lose the outline. Abutting bands share an edge, so duplicates are removed too.
        std::vector<float> zs;
        zs.reserve(zbands.size() * 2);
        for (const std::pair<float, float> &zb : zbands) {
            zs.emplace_back(zb.first);
            zs.emplace_back(zb.second);
        }
        sort_remove_duplicates(zs);
        // trafo defaults to identity, and the mesh is already in world space.
        const std::vector<Polygons> layers = slice_mesh(world_its, zs, MeshSlicingParams{});

        size_t segments = 0;
        for (const Polygons &polys : layers)
            for (const Polygon &poly : polys)
                segments += poly.points.size();
        if (segments > 0) {
            GLModel::Geometry outline;
            outline.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
            outline.reserve_vertices(2 * segments);
            outline.reserve_indices(2 * segments);
            unsigned int vertices_counter = 0;
            for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
                const float z = zs[layer_idx];
                for (const Polygon &poly : layers[layer_idx]) {
                    for (size_t i = 0; i < poly.points.size(); ++i) {
                        const Point &p0 = poly.points[i];
                        const Point &p1 = next_value_modulo(i, poly.points);
                        outline.add_vertex(Vec3f(unscale<float>(p0.x()), unscale<float>(p0.y()), z));
                        outline.add_vertex(Vec3f(unscale<float>(p1.x()), unscale<float>(p1.y()), z));
                        vertices_counter += 2;
                        outline.add_line(vertices_counter - 2, vertices_counter - 1);
                    }
                }
            }
            entry.outline.init_from(std::move(outline));
        }

        if (! entry.fill.is_initialized() && ! entry.outline.is_initialized())
            m_periodic_band_models.pop_back();
    }
}

void GLGizmoMmuSegmentation::render_periodic_bands()
{
    if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView)
        return;

    // Load here too: this renders before the panel, which would otherwise load the patterns a frame late.
    this->load_periodic_patterns();
    if (m_periodic_patterns_object == nullptr)
        return;

    // Nothing to draw or clear; skips building the cache key every frame.
    if (m_periodic_patterns.patterns.empty() && m_periodic_band_models.empty())
        return;

    this->update_periodic_band_models();
    if (m_periodic_band_models.empty())
        return;

    // Band geometry is in world space, so the volume matrix is identity. flat_clip, so the section view cuts the
    // bands along with the object.
    GLShaderProgram *shader = wxGetApp().get_shader("flat_clip");
    if (shader == nullptr)
        return;
    shader->start_using();
    ScopeGuard shader_guard([shader]() { shader->stop_using(); });

    const ClippingPlaneDataWrapper clp_data = this->get_clipping_plane_data();
    const Camera                  &camera   = wxGetApp().plater()->get_camera();
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("volume_world_matrix", Transform3d::Identity());

    // The tint needs GL_LEQUAL to draw on the surface it covers. Save the depth func and restore it afterward:
    // render_cursor() and the clipper renders rely on it.
    GLint prev_depth_func = GL_LESS;
    glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &prev_depth_func));
    glsafe(::glDepthFunc(GL_LEQUAL));
    glsafe(::glDepthMask(GL_FALSE));

    // Back faces would tint the far wall of the object through the near one.
    glsafe(::glEnable(GL_CULL_FACE));
    // Pull the tint toward the camera: clipped vertices differ slightly in depth from the surface, which speckles.
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-1.0f, -1.0f));
    for (PeriodicBandModel &entry : m_periodic_band_models)
        if (entry.fill.is_initialized() && entry.filament >= 1 &&
            size_t(entry.filament) <= m_extruders_colors.size()) {
            ColorRGBA c = m_extruders_colors[entry.filament - 1];
            c.a(0.55f);
            entry.fill.set_color(c);
            entry.fill.render();
        }
    glsafe(::glPolygonOffset(0.0f, 0.0f));
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glDisable(GL_CULL_FACE));

    for (PeriodicBandModel &entry : m_periodic_band_models)
        if (entry.outline.is_initialized() && entry.filament >= 1 &&
            size_t(entry.filament) <= m_extruders_colors.size()) {
            ColorRGBA c = m_extruders_colors[entry.filament - 1];
            c.a(1.f);   // opaque: a palette color may carry its own alpha
            entry.outline.set_color(c);
            entry.outline.render();
        }

    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDepthFunc(prev_depth_func));
}

void GLGizmoMmuSegmentation::load_periodic_patterns()
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == m_periodic_patterns_object)
        return;
    m_periodic_bands_key.clear();
    m_periodic_patterns_object      = mo;
    m_periodic_patterns             = PeriodicRecolorPatterns();
    m_periodic_patterns_before_edit.clear();
    m_periodic_patterns_dirty       = false;
    if (mo != nullptr && mo->config.has("periodic_recolor_patterns")) {
        // Start the undo baseline from the stored patterns, so the first undo does not erase patterns loaded from a project.
        m_periodic_patterns_before_edit = mo->config.get().option<ConfigOptionFloats>("periodic_recolor_patterns")->values;
        m_periodic_patterns             = PeriodicRecolorPatterns::from_doubles(m_periodic_patterns_before_edit);
    }
}

void GLGizmoMmuSegmentation::flush_periodic_patterns()
{
    if (! m_periodic_patterns_dirty)
        return;
    // Clear before committing: commit posts events that can call back here, which would commit twice.
    m_periodic_patterns_dirty = false;

    // Drop the edit if the selection moved on, or it would be written onto another object.
    const CommonGizmosDataObjects::SelectionInfo *sel = m_c != nullptr ? m_c->selection_info() : nullptr;
    const ModelObject *mo = sel != nullptr ? sel->model_object() : nullptr;
    if (mo == nullptr || mo != m_periodic_patterns_object)
        return;

    // Also drop it if the stored patterns changed since the edit began, e.g. an undo that data_changed() has not
    // reported yet; committing would put the undone value back.
    const std::vector<double> current = mo->config.has("periodic_recolor_patterns") ?
        mo->config.get().option<ConfigOptionFloats>("periodic_recolor_patterns")->values : std::vector<double>();
    if (current != m_periodic_patterns_before_edit) {
        // Force a reload, so the panel shows what is stored.
        m_periodic_patterns_object = nullptr;
        return;
    }

    this->commit_periodic_patterns();
}

void GLGizmoMmuSegmentation::commit_periodic_patterns()
{
    ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;

    const std::vector<double> values = m_periodic_patterns.to_doubles();
    const std::vector<double> current = mo->config.has("periodic_recolor_patterns") ?
        mo->config.get().option<ConfigOptionFloats>("periodic_recolor_patterns")->values : std::vector<double>();
    if (values == current)
        return;

    // The config still holds the pre-edit patterns, so the snapshot keeps them for undo. Then apply the new ones.
    {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Periodic recoloring", UndoRedo::SnapshotType::GizmoAction);
        if (values.empty())
            mo->config.erase("periodic_recolor_patterns");
        else
            mo->config.set_key_value("periodic_recolor_patterns", new ConfigOptionFloats(values));
    }
    m_periodic_patterns_before_edit = values;

    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    m_parent.set_as_dirty();
}

void GLGizmoMmuSegmentation::render_periodic_recolor_ui(float window_width, float sliders_left_width, float sliders_width,
                                                        float drag_left_width, float slider_icon_width,
                                                        float scale)
{
    // Swatches per line in a pattern row, matching the painting swatches at the top of the panel.
    constexpr size_t PERIODIC_SWATCHES_PER_LINE = 8;

    this->load_periodic_patterns();
    if (m_periodic_patterns_object == nullptr)
        return;

    const size_t num_filaments = std::min((size_t) EnforcerBlockerType::ExtruderMax, m_extruders_colors.size());
    const int    instance_idx  = m_parent.get_selection().get_instance_idx();
    double object_min_z = 0., object_height = 0.;
    if (instance_idx >= 0 && instance_idx < int(m_periodic_patterns_object->instances.size()))
        periodic_instance_z(*m_periodic_patterns_object, size_t(instance_idx), object_min_z, object_height);
    const float height_slider_max = float(object_height);
    // Round to the displayed precision to avoid float drift at layer boundaries.
    constexpr double FIELD_SCALE = 1000.;

    m_imgui->text_wrapped(_L("Recolor features by repeating height bands periodically. See the results in Preview."), window_width);
    m_imgui->text_wrapped(_L("Start and End elevations are measured from the bottom of the object."), window_width);
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    std::vector<std::string> role_labels;
    role_labels.reserve(PERIODIC_RECOLOR_ROLES.size());
    for (ExtrusionRole role : PERIODIC_RECOLOR_ROLES)
        role_labels.emplace_back(into_u8(_L(ExtrusionEntity::role_to_string(role))));

    float role_combo_width = sliders_width;
    for (const std::string &label : role_labels)
        role_combo_width = std::max(role_combo_width, ImGui::CalcTextSize(label.c_str()).x);
    role_combo_width += 3.f * ImGui::GetFontSize();   // dropdown arrow and frame padding

    // Slider and drag edits change every frame, so commit on release rather than taking an undo snapshot and re-slicing every frame.
    bool changed  = false;
    // Set when a slider or numeric field finishes its own edit this frame.
    bool finished = false;
    int  to_erase = -1;

    const PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
    // 0 when every filament is mixed, which disables Add pattern.
    int first_selectable = 0;
    for (size_t f = 0; f < num_filaments && first_selectable == 0; ++f)
        if (! preset_bundle.is_mixed_filament(f))
            first_selectable = int(f + 1);

    for (size_t idx = 0; idx < m_periodic_patterns.patterns.size(); ++idx) {
        PeriodicRecolorPattern &pattern = m_periodic_patterns.patterns[idx];
        ImGui::PushID(int(idx));

        // Shade every other pattern. Its height is known only after its controls are drawn, so the controls go on the front
        // draw channel and the shading is added to the back channel afterward.
        ImDrawList *pattern_dl = ImGui::GetWindowDrawList();
        pattern_dl->ChannelsSplit(2);
        pattern_dl->ChannelsSetCurrent(1);
        ImGui::BeginGroup();

        const ImVec4 pattern_head       = m_is_dark_mode ? ImGuiWrapper::COL_SEPARATOR_DARK : ImGuiWrapper::COL_SEPARATOR;
        const ImVec4 pattern_head_hover = m_is_dark_mode ? ImGuiWrapper::COL_GREY_LIGHT     : ImGuiWrapper::COL_TITLE_BG;
        ImGui::PushStyleColor(ImGuiCol_Header,        pattern_head);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, pattern_head_hover);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  pattern_head_hover);
        // A stable ID preserves the expanded state when the feature label changes.
        const std::string pattern_title = into_u8(GUI::format(_L("Pattern %1% - %2%"), idx + 1,
                                                              _L(ExtrusionEntity::role_to_string(pattern.role))));
        const bool pattern_open = ImGui::TreeNodeEx("##pattern", ImGuiTreeNodeFlags_Framed |
                                                          ImGuiTreeNodeFlags_SpanAvailWidth |
                                                          ImGuiTreeNodeFlags_FramePadding |
                                                          ImGuiTreeNodeFlags_DefaultOpen,
                                                   "%s", pattern_title.c_str());
        ImGui::PopStyleColor(3);
        // Use the heading's rect so the shading lines up with it.
        const float pattern_x0 = ImGui::GetItemRectMin().x;
        const float pattern_x1 = ImGui::GetItemRectMax().x;
        if (pattern_open) {
            changed |= m_imgui->bbl_checkbox("##pattern_enabled", pattern.enabled);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            m_imgui->text(_L("Enabled"));

            m_imgui->text(_L("Filament") + ":");
            for (size_t f = 0; f < num_filaments; ++f) {
                if (f == 0)
                    ImGui::SameLine(sliders_left_width);
                else if (f % PERIODIC_SWATCHES_PER_LINE != 0)
                    ImGui::SameLine();
                else
                    ImGui::SetCursorPosX(sliders_left_width);
                m_imgui->disabled_begin(preset_bundle.is_mixed_filament(f));
                if (draw_color_button(int(f + 1), "###periodic_filament_", m_extruders_colors[f], m_extruders_colors[f],
                                      pattern.filament == int(f + 1), scale)) {
                    pattern.filament = int(f + 1);
                    changed = true;
                }
                m_imgui->disabled_end();
            }

            const auto role_it = std::find(PERIODIC_RECOLOR_ROLES.begin(), PERIODIC_RECOLOR_ROLES.end(), pattern.role);
            int role_idx = int(std::distance(PERIODIC_RECOLOR_ROLES.begin(), role_it));
            if (render_combo(into_u8(_L("Feature") + ":"), role_labels, role_idx, sliders_left_width, role_combo_width)) {
                pattern.role = PERIODIC_RECOLOR_ROLES[size_t(role_idx)];
                changed = true;
            }

            auto value_row = [&](const wxString &label, const char *id, double &value, float min_v, float max_v) {
                ImGui::AlignTextToFramePadding();
                m_imgui->text(label + ":");
                ImGui::SameLine(sliders_left_width);
                ImGui::SetNextItemWidth(sliders_width);
                float v = float(value);
                const std::string fmt = std::string("%.3f ") + into_u8(_L("mm"));
                bool edited = m_imgui->bbl_slider_float_style((std::string("##slider_") + id).c_str(), &v, min_v, max_v,
                                                              fmt.c_str(), 1.0f, false);
                finished |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SameLine(drag_left_width + sliders_left_width);
                ImGui::SetNextItemWidth(1.5f * slider_icon_width);
                edited |= ImGui::BBLDragFloat((std::string("##input_") + id).c_str(), &v, 0.05f, 0.0f, 0.0f, "%.3f");
                finished |= ImGui::IsItemDeactivatedAfterEdit();
                if (edited) {
                    value = std::round(double(v) * FIELD_SCALE) / FIELD_SCALE;
                    m_periodic_patterns_dirty = true;
                }
            };

            value_row(_L("Start"), "start", pattern.start, 0.f, height_slider_max);
            value_row(_L("End"),   "end",   pattern.end,   0.f, height_slider_max);
            value_row(_L("Period"), "period", pattern.period, 0.f,  50.f);

            ImGui::AlignTextToFramePadding();
            m_imgui->text(_L("Alignment") + ":");
            ImGui::SameLine(sliders_left_width);
            {
                int alignment_idx = int(pattern.alignment);
                // Reuse the "Alignment" translations of Top, Middle and Bottom; camera views and layer surfaces translate them differently.
                // push_radio_style() is required: this panel does not set ImGuiCol_CheckMark, so the selected radio would look
                // unselected. Cut, Emboss and SVG do the same.
                ImGuiWrapper::push_radio_style(m_parent.get_scale());
                bool alignment_changed = ImGui::RadioButton(into_u8(_L_CONTEXT("Top", "Alignment")).c_str(),
                                                            &alignment_idx, int(PeriodicRecolorAlignment::Top));
                ImGui::SameLine();
                alignment_changed |= ImGui::RadioButton(into_u8(_L_CONTEXT("Middle", "Alignment")).c_str(),
                                                        &alignment_idx, int(PeriodicRecolorAlignment::Middle));
                ImGui::SameLine();
                alignment_changed |= ImGui::RadioButton(into_u8(_L_CONTEXT("Bottom", "Alignment")).c_str(),
                                                        &alignment_idx, int(PeriodicRecolorAlignment::Bottom));
                ImGuiWrapper::pop_radio_style();
                if (alignment_changed) {
                    pattern.alignment = PeriodicRecolorAlignment(alignment_idx);
                    changed = true;
                }
            }

            value_row(_L("Band thickness"), "band", pattern.band_vertical_height, 0.f, 20.f);

            if (! pattern.is_valid(num_filaments))
                m_imgui->warning_text(_L("This pattern is incomplete and will be ignored."));

            if (m_imgui->button(_L("Remove")))
                to_erase = int(idx);

            ImGui::TreePop();
        }

        ImGui::EndGroup();
        pattern_dl->ChannelsSetCurrent(0);
        if (idx % 2 == 1) {
            const float pad = ImGui::GetStyle().ItemSpacing.y * 0.5f;
            ImVec4 wash_col = m_is_dark_mode ? ImGuiWrapper::COL_SEPARATOR_DARK : ImGuiWrapper::COL_SEPARATOR;
            wash_col.w = 0.45f;
            const ImU32 wash = ImGui::GetColorU32(wash_col);
            pattern_dl->AddRectFilled(ImVec2(pattern_x0, ImGui::GetItemRectMin().y - pad),
                                      ImVec2(pattern_x1, ImGui::GetItemRectMax().y + pad), wash);
        }
        pattern_dl->ChannelsMerge();
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));

    m_imgui->disabled_begin(first_selectable == 0);
    if (m_imgui->button(_L("Add pattern"))) {
        PeriodicRecolorPattern pattern;
        const int previous = m_periodic_patterns.patterns.empty() ? 0 : m_periodic_patterns.patterns.back().filament;
        const bool previous_ok = previous >= 1 && size_t(previous) <= num_filaments &&
                                 ! preset_bundle.is_mixed_filament(size_t(previous - 1));
        pattern.filament             = previous_ok ? previous : first_selectable;
        // Rounded like a typed value, so the stored End matches the field.
        pattern.end                  = std::round(object_height * FIELD_SCALE) / FIELD_SCALE;
        pattern.band_vertical_height = 1.;
        pattern.period               = 5.;
        // Start at the band thickness: a mark at 0 gives Top and Middle no layer to place a band on. With the default Top
        // alignment the first band then starts at the object's bottom.
        pattern.start                = pattern.band_vertical_height;
        m_periodic_patterns.patterns.emplace_back(pattern);
        changed = true;
    }
    m_imgui->disabled_end();

    if (to_erase >= 0) {
        m_periodic_patterns.patterns.erase(m_periodic_patterns.patterns.begin() + to_erase);
        changed = true;
    }

    // Commit an unfinished edit once its widget is released or nothing is active. Both checks need the dirty flag,
    // so a finish that arrives after data_changed() dropped the edit does not restore it.
    if (changed || (m_periodic_patterns_dirty && (finished || ! ImGui::IsAnyItemActive()))) {
        this->commit_periodic_patterns();
        m_periodic_patterns_dirty = false;
    }
}

} // namespace Slic3r
