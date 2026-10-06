#include "GLGizmoPaintedModifier.hpp"

#include "libslic3r/Color.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "GLGizmoUtils.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glad/gl.h>
#include <imgui.h>
#include <wx/busycursor.h>

namespace Slic3r::GUI {

GLGizmoPaintedModifier::GLGizmoPaintedModifier(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id), m_current_tool(ImGui::CircleButtonIcon)
{
}

std::string GLGizmoPaintedModifier::on_get_name() const
{
    return _u8L("Painted modifier");
}

bool GLGizmoPaintedModifier::on_init()
{
    const wxString ctrl  = GUI::shortkey_ctrl_prefix();
    const wxString shift = GUI::shortkey_shift_prefix();

    m_desc["target"]           = _L("Modifier");
    m_desc["new"]              = _L("New painted modifier");
    m_desc["depth"]            = _L("Depth");
    m_desc["depth_tooltip"]    = _L("How far the painted area reaches into the part, measured from the painted surface.\n"
                                    "0 reaches across the whole part, up to the nearest unpainted surface.");
    m_desc["no_host"]          = _L("The part this modifier was painted on has been removed.");
    m_desc["remove_all"]       = _L("Erase all");
    m_desc["tool_type"]        = _L("Tool type");
    m_desc["cursor_size"]      = _L("Brush size");
    m_desc["smart_fill_angle"] = _L("Smart fill angle");
    m_desc["add_area"]         = _L("Paint the modifier area");
    m_desc["remove_area"]      = _L("Erase the modifier area");

    const std::pair<wxString, wxString> add_shortcut    = {_L("Left mouse button"), m_desc["add_area"]};
    const std::pair<wxString, wxString> remove_shortcut = {shift + _L("Left mouse button"), m_desc["remove_area"]};
    m_shortcuts_brush      = {add_shortcut, remove_shortcut, {ctrl + _L("Mouse wheel"), m_desc["cursor_size"]}};
    m_shortcuts_triangle   = {add_shortcut, remove_shortcut};
    m_shortcuts_smart_fill = {add_shortcut, remove_shortcut, {ctrl + _L("Mouse wheel"), m_desc["smart_fill_angle"]}};
    return true;
}

void GLGizmoPaintedModifier::on_shutdown()
{
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

void GLGizmoPaintedModifier::set_target(const ModelVolume *painted_modifier)
{
    const ObjectID id = painted_modifier ? painted_modifier->id() : ObjectID();
    if (id == m_target_id)
        return;
    m_target_id = id;
    if (m_state == On)
        update_from_model_object(false);
}

ModelVolume *GLGizmoPaintedModifier::target(const ModelObject &mo) const
{
    if (m_target_id.invalid())
        return nullptr;
    auto it = std::find_if(mo.volumes.begin(), mo.volumes.end(), [this](const ModelVolume *v) { return v->id() == m_target_id; });
    return it != mo.volumes.end() && (*it)->is_painted_modifier() ? *it : nullptr;
}

bool GLGizmoPaintedModifier::is_mesh_paintable(int mesh_id) const
{
    return m_target_id.invalid() || mesh_id == m_target_mesh_id;
}

void GLGizmoPaintedModifier::render_painter_gizmo()
{
    const Selection &selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);
    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

void GLGizmoPaintedModifier::render_tooltip_button(float x, float y)
{
    auto get_shortcuts = [this]() -> std::vector<std::pair<wxString, wxString>> {
        switch (m_tool_type) {
        case ToolType::BRUSH: return m_cursor_type == TriangleSelector::POINTER ? m_shortcuts_triangle : m_shortcuts_brush;
        case ToolType::SMART_FILL: return m_shortcuts_smart_fill;
        default: return {};
        }
    };
    GLGizmoUtils::render_tooltip_button(m_imgui, m_parent, get_shortcuts(), x, y);
}

void GLGizmoPaintedModifier::on_render_input_window(float x, float y, float bottom_limit)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (!mo)
        return;
    ModelVolume *painted_modifier = target(*mo);

    float scale = m_parent.get_scale();
#ifdef WIN32
    const int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
    scale *= float(dpi) / float(DPI_DEFAULT);
#endif // WIN32

    y = std::min(y, bottom_limit - m_imgui->scaled(22.f));
#if BBS_TOOLBAR_ON_TOP
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
#else
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    const float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const float space_size         = m_imgui->get_style_scaling() * 8;
    const float sliders_left_width = std::max({m_imgui->calc_text_size(m_desc["cursor_size"]).x, m_imgui->calc_text_size(m_desc["smart_fill_angle"]).x,
                                               m_imgui->calc_text_size(m_desc["depth"]).x, m_imgui->calc_text_size(m_desc["target"]).x}) +
                                     m_imgui->scaled(1.5f);
    const float sliders_width      = m_imgui->scaled(7.0f);
    const float drag_left_width    = ImGui::GetStyle().WindowPadding.x + sliders_width - space_size;
    const float slider_icon_width  = m_imgui->get_slider_icon_size().x;
    const float empty_button_width = m_imgui->calc_button_size("").x;
    const float max_tooltip_width  = ImGui::GetFontSize() * 20.0f;

    // The painted modifier the strokes go to, or a new one created by the next stroke.
    std::vector<ModelVolume *> painted_modifiers;
    std::vector<std::string>   options;
    int                        selection = -1;
    for (ModelVolume *mv : mo->volumes)
        if (mv->is_painted_modifier()) {
            if (mv == painted_modifier)
                selection = int(painted_modifiers.size());
            options.push_back(mv->name + "##" + std::to_string(painted_modifiers.size()));
            painted_modifiers.push_back(mv);
        }
    options.push_back(into_u8(m_desc["new"]) + "##new");
    if (selection < 0)
        selection = int(options.size()) - 1;
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    if (m_imgui->combo(m_desc["target"], options, selection, 0, sliders_left_width, sliders_width + slider_icon_width * 1.5f + space_size)) {
        ModelVolume *selected = selection < int(painted_modifiers.size()) ? painted_modifiers[selection] : nullptr;
        set_target(selected);
        if (selected)
            wxGetApp().obj_list()->select_item(ObjectVolumeID{mo, selected});
    }
    ImGuiWrapper::pop_combo_style();

    m_imgui->disabled_begin(painted_modifier == nullptr);
    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc["depth"]);
    ImGui::SameLine(sliders_left_width);
    ImGui::PushItemWidth(sliders_width);
    ImGui::BBLDragFloat("##painted_modifier_depth", &m_depth, 0.1f, 0.f, 1000.f, "%.2f mm");
    if (painted_modifier && ImGui::IsItemDeactivatedAfterEdit() && std::max(0.f, m_depth) != painted_modifier->painted_modifier_depth) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Change painted modifier depth"), UndoRedo::SnapshotType::GizmoAction);
        painted_modifier->painted_modifier_depth = std::max(0.f, m_depth);
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(m_desc["depth_tooltip"], max_tooltip_width);
    m_imgui->disabled_end();

    ImGui::Separator();

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc["tool_type"]);

    const std::array<wchar_t, 4> tool_ids = {ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::FillButtonIcon};
    const std::array<wchar_t, 4> icons    = m_is_dark_mode ?
        std::array<wchar_t, 4>{ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon, ImGui::TriangleButtonDarkIcon, ImGui::FillButtonDarkIcon} :
        tool_ids;
    const std::array<wxString, 4> tool_tips = {_L("Circle"), _L("Sphere"), _L("Triangle"), _L("Fill")};
    for (int i = 0; i < int(tool_ids.size()); i++) {
        if (i != 0)
            ImGui::SameLine((empty_button_width + m_imgui->scaled(1.75f)) * i + m_imgui->scaled(1.5f));

        const bool is_active = m_current_tool == tool_ids[i];
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f * scale, 4.f * scale));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        ImGui::PushStyleColor(ImGuiCol_Button, is_active ? ImVec4(0.f, .59f, .53f, .25f) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, is_active ? ImVec4(0.f, .59f, .53f, .25f) : ImVec4(.6f, .6f, .6f, .2f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, is_active ? ImVec4(0.f, .59f, .53f, .30f) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, is_active ? ImGuiWrapper::COL_ORCA : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_BorderActive, is_active ? ImGuiWrapper::COL_ORCA : ImVec4(0, 0, 0, 0));
        const bool btn_clicked = m_imgui->glyph_button(icons[i], ImVec2(16.f * scale, 16.f * scale));
        ImGui::PopStyleColor(6);
        ImGui::PopStyleVar(3);

        if (btn_clicked && m_current_tool != tool_ids[i]) {
            m_current_tool = tool_ids[i];
            for (auto &triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(tool_tips[i], max_tooltip_width);
    }

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    if (m_current_tool == ImGui::CircleButtonIcon || m_current_tool == ImGui::SphereButtonIcon) {
        m_cursor_type = m_current_tool == ImGui::CircleButtonIcon ? TriangleSelector::CursorType::CIRCLE : TriangleSelector::CursorType::SPHERE;
        m_tool_type   = ToolType::BRUSH;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["cursor_size"]);
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");
    } else if (m_current_tool == ImGui::TriangleButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::BRUSH;
    } else {
        assert(m_current_tool == ImGui::FillButtonIcon);
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::SMART_FILL;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["smart_fill_angle"]);
        const std::string format_str = std::string("%.f") + I18N::translate_utf8("°", "Face angle threshold,"
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
    }

    ImGui::Separator();

    render_tooltip_button(x, y);

    ImGui::SameLine();
    m_imgui->disabled_begin(painted_modifier == nullptr || m_target_mesh_id < 0 || painted_modifier->painted_modifier_facets.empty());
    if (m_imgui->button(m_desc["remove_all"])) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reset selection"), UndoRedo::SnapshotType::GizmoAction);
        m_triangle_selectors[m_target_mesh_id]->reset();
        m_triangle_selectors[m_target_mesh_id]->request_update_render_data(true);
        update_model_object();
        m_parent.set_as_dirty();
    }
    m_imgui->disabled_end();

    ImGui::SameLine();
    GLGizmoUtils::begin_right_aligned_buttons({_L("Done")});
    if (m_imgui->button(_L("Done")))
        m_parent.reset_all_gizmos();

    if (painted_modifier != nullptr && m_target_mesh_id < 0) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_WARNING);
        m_imgui->text_wrapped(m_desc["no_host"], ImGui::GetContentRegionAvail().x);
        ImGui::PopStyleColor();
    }

    ImGui::PopStyleVar(1); // ImGuiStyleVar_FramePadding
    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoPaintedModifier::update_model_object()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;

    ModelVolume *painted_modifier = target(*mo);
    bool         created          = false;
    if (painted_modifier == nullptr) {
        // The first stroke creates the painted modifier on the part it painted.
        std::vector<const ModelVolume *> parts;
        for (const ModelVolume *mv : mo->volumes)
            if (mv->is_model_part())
                parts.push_back(mv);
        for (int mesh_id = 0; mesh_id < int(std::min(parts.size(), m_triangle_selectors.size())); ++mesh_id)
            if (m_triangle_selectors[mesh_id]->has_facets(EnforcerBlockerType::ENFORCER)) {
                const ModelObjectPtrs &objects = wxGetApp().model().objects;
                const int              obj_idx = int(std::find(objects.begin(), objects.end(), mo) - objects.begin());
                painted_modifier               = wxGetApp().obj_list()->add_painted_modifier(obj_idx, *parts[mesh_id]);
                m_target_id                    = painted_modifier->id();
                m_target_mesh_id               = mesh_id;
                created                        = true;
                break;
            }
        if (painted_modifier == nullptr)
            return;
    }

    if (m_target_mesh_id >= 0 && painted_modifier->painted_modifier_facets.set(*m_triangle_selectors[m_target_mesh_id]))
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    if (created)
        // Show its settings, after its paint is stored.
        wxGetApp().obj_list()->select_item(ObjectVolumeID{mo, painted_modifier});
}

void GLGizmoPaintedModifier::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    m_triangle_selectors.clear();
    m_target_mesh_id = -1;
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    mo->sync_painted_modifiers();

    const ModelVolume *painted_modifier = target(*mo);
    if (painted_modifier == nullptr && first_update) {
        // Opened from the toolbar: continue with the last painted modifier of the object.
        auto it = std::find_if(mo->volumes.rbegin(), mo->volumes.rend(), [](const ModelVolume *v) { return v->is_painted_modifier(); });
        if (it != mo->volumes.rend())
            painted_modifier = *it;
    }
    m_target_id = painted_modifier ? painted_modifier->id() : ObjectID();
    if (painted_modifier != nullptr)
        m_depth = painted_modifier->painted_modifier_depth;
    const ModelVolume *host = painted_modifier ? mo->painted_modifier_host(*painted_modifier) : nullptr;

    const std::vector<ColorRGBA> ebt_colors = {GLVolume::NEUTRAL_COLOR, TriangleSelectorGUI::enforcers_color, TriangleSelectorGUI::blockers_color};
    int mesh_id = -1;
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        ++mesh_id;
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(mv->mesh(), ebt_colors));
        if (mv == host) {
            m_target_mesh_id = mesh_id;
            // Reset of TriangleSelector is done inside TriangleSelectorGUI's constructor, so we don't need it to perform it again in deserialize().
            m_triangle_selectors.back()->deserialize(painted_modifier->painted_modifier_facets.get_data(), false);
        }
        m_triangle_selectors.back()->request_update_render_data();
    }
}

wxString GLGizmoPaintedModifier::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    return shift_down ? _L("Erase the modifier area") : _L("Paint the modifier area");
}

} // namespace Slic3r::GUI
