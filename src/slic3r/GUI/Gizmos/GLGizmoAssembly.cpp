#include "GLGizmoAssembly.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/MeasureUtils.hpp"

#include <imgui/imgui_internal.h>

#include <numeric>

#include <GL/glew.h>

#include <tbb/parallel_for.h>
#include <future>
#include <wx/clipbrd.h>

namespace Slic3r {
namespace GUI {

GLGizmoAssembly::GLGizmoAssembly(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id) :
    GLGizmoMeasure(parent, icon_filename, sprite_id)
{
    m_measure_mode       = EMeasureMode::ONLY_ASSEMBLY;
}

std::string GLGizmoAssembly::on_get_name() const
{
    if (!on_is_activable() && m_state == EState::Off) {
        if (wxGetApp().plater()->canvas3D()->get_canvas_type() == GLCanvas3D::ECanvasType::CanvasAssembleView) {
            return _u8L("Assemble") + ":\n" + _u8L("Please confirm explosion ratio = 1 and select at least two volumes.");
        }
        else {
            return _u8L("Assemble") + ":\n" + _u8L("Please select at least two volumes.");
        }
    } else {
        return _u8L("Assemble");
    }
}

bool GLGizmoAssembly::on_init()
{
    m_shortcut_key = WXK_CONTROL_Y;
    return true;
}

bool GLGizmoAssembly::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();
    if (selection.is_wipe_tower()) {
        return false;
    }
    const int    selection_volumes_count = 2;
    if (wxGetApp().plater()->canvas3D()->get_canvas_type() == GLCanvas3D::ECanvasType::CanvasAssembleView) {
        if (abs(m_parent.get_explosion_ratio() - 1.0f) < 1e-2 && selection.volumes_count() >= selection_volumes_count) {
            return true;
        }
        return false;
    } else {
        return selection.volumes_count() >= selection_volumes_count;
    }
}

void GLGizmoAssembly::on_render_input_window(float x, float y, float bottom_limit)
{
    static std::optional<Measure::SurfaceFeature> last_feature;
    static EMode last_mode = EMode::FeatureSelection;
    static SelectedFeatures last_selected_features;

    static float last_y = 0.0f;
    static float last_h = 0.0f;

    if (m_editing_distance)
        return;
    m_current_active_imgui_id      = ImGui::GetActiveID();
    // adjust window position to avoid overlap the view toolbar
    const float win_h = ImGui::GetWindowHeight();
    y = std::min(y, bottom_limit - win_h);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    if (last_h != win_h || last_y != y) {
        // ask canvas for another frame to render the window in the correct position
        m_imgui->set_requires_extra_frame();
        if (last_h != win_h)
            last_h = win_h;
        if (last_y != y)
            last_y = y;
    }
    // Orca
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
    init_render_input_window();

    float moving_size = m_imgui->calc_text_size(_L("(Moving)")).x;
    float combox_content_size = m_imgui->calc_text_size(_L("Point and point assembly")).x*1.1 + ImGui::GetStyle().FramePadding.x * 18.0f;
    float caption_size = moving_size + 2 * m_space_size;
    if (render_assembly_mode_combo(caption_size + 0.5 * m_space_size,  combox_content_size)) {
        ;
    }
    show_selection_ui();
    show_face_face_assembly_common();
    ImGui::Separator();
    show_face_face_assembly_senior();
    show_distance_xyz_ui();
    render_input_window_warning(m_same_model_object);
    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    float caption_max    = 0.f;
    float total_text_max = 0.f;
    for (const auto &t : std::array<std::string, 3>{"point_selection", "reset", "unselect"}) {
        caption_max    = std::max(caption_max, m_imgui->calc_text_size(m_desc[t + "_caption"]).x);
        total_text_max = std::max(total_text_max, m_imgui->calc_text_size(m_desc[t]).x);
    }
    show_tooltip_information(caption_max, x, get_cur_y);

    float f_scale =m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    ImGui::PopStyleVar(2);

    if (last_feature != m_curr_feature || last_mode != m_mode || last_selected_features != m_selected_features) {
        // the dialog may have changed its size, ask for an extra frame to render it properly
        last_feature = m_curr_feature;
        last_mode = m_mode;
        last_selected_features = m_selected_features;
        m_imgui->set_requires_extra_frame();
    }
    m_last_active_item_imgui = m_current_active_imgui_id;
    GizmoImguiEnd();
    // Orca
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoAssembly::render_input_window_warning(bool same_model_object)
{
    if (wxGetApp().plater()->canvas3D()->get_canvas_type() == GLCanvas3D::ECanvasType::CanvasView3D) {
        if (m_hit_different_volumes.size() == 2) {
            if (same_model_object == false) {
                m_imgui->warning_text(_L("Warning") + ": " +
               _L("It is recommended to assemble the objects first,\nbecause the objects is restriced to bed \nand only parts can be lifted."));
            }
        }
    }
}

bool GLGizmoAssembly::render_assembly_mode_combo(double label_width, float item_width)
{
    ImGui::AlignTextToFramePadding();
    int                      selection_idx = int(m_assembly_mode);
    std::vector<std::string> modes         = {_u8L("Face and face assembly"), _u8L("Point and point assembly")};
    bool                     is_changed    = false;

    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    if (render_combo(_u8L("Mode"), modes, selection_idx, label_width, item_width)) {
        is_changed = true;
        switch_to_mode((AssemblyMode) selection_idx);
    }
    ImGuiWrapper::pop_combo_style();
    return is_changed;
}

void GLGizmoAssembly::switch_to_mode(AssemblyMode new_mode)
{
    m_assembly_mode = new_mode;
    reset_all_feature();
}

} // namespace GUI
} // namespace Slic3r
