#include "GLGizmoUtils.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "GLGizmosManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include <wx/app.h>
#include <boost/algorithm/string.hpp>

#ifdef WIN32
#include <wx/msw/winundef.h>
#endif

/*
    GizmoUI Footer Structure:

    ~ Content ~
    ----------------------------------------
    [Button1] [Button2]
    ----------------------------------------
    [?] [Reset]           [Confirm] [Cancel]
    ----------------------------------------
    ~ Warnings ~


    Additional details:
        - [Confirm], [Cancel], [Done], ... are buttons that close the Tool Dialog
        - [Reset], [Button1], ... are buttons that do not!
        - Non-consequential buttons like [Cancel] and [Done] are always the right-most buttons
        - Multiple warnings can show, but should only have one ImGui::Separator above
        - If no warnings is shown, dont render the ImGui::Separator

*/

namespace Slic3r::GUI::GLGizmoUtils {

void render_tooltip_button(
    ImGuiWrapper* imgui_wrapper, const GLCanvas3D& canvas, const std::vector<std::pair<wxString, wxString>>& shortcuts, float x, float y)
{
    float caption_y     = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    float caption_x_max = 0.f;
    for (const auto& item : shortcuts) {
        caption_x_max = std::max(caption_x_max, imgui_wrapper->calc_text_size(item.first).x);
    }
    caption_x_max += imgui_wrapper->calc_text_size(": "sv).x + 35.f;

    auto&       gizmos_manager = canvas.get_gizmos_manager();
    ImTextureID normal_id      = gizmos_manager.get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id       = gizmos_manager.get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    float scale = canvas.get_scale();
#ifdef WIN32
    int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
    scale *= (float) dpi / (float) DPI_DEFAULT;
#endif

    ImVec2 button_size = ImVec2(25 * scale, 25 * scale);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 0});

    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, caption_y));
        for (const auto& item : shortcuts) {
            imgui_wrapper->text_colored(ImGuiWrapper::COL_ACTIVE, item.first + ": ");
            ImGui::SameLine(caption_x_max);
            imgui_wrapper->text_colored(ImGuiWrapper::COL_WINDOW_BG, item.second);
        }
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

} // namespace Slic3r::GUI::GLGizmoUtils