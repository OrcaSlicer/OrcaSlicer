#include "GLGizmoUtils.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "GLGizmosManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <vector>
#include <utility>
#include <imgui.h>
#include <algorithm>
#include <string_view>
#include <cstddef>
#include <wx/app.h>
#include <wx/utils.h>
#include <boost/algorithm/string.hpp>
#include "slic3r/GUI/GUI_Utils.hpp"

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
        - [Confirm] buttons should use the orca_button_style to differentiate them from other buttons
        - Multiple warnings can show, but should only have one ImGui::Separator above
        - If no warnings is shown, dont render the ImGui::Separator

*/

using namespace std::string_view_literals;

namespace Slic3r::GUI::GLGizmoUtils {

    void render_tooltip_button(
        ImGuiWrapper* imgui_wrapper, const GLCanvas3D& canvas, const std::vector<std::pair<wxString, wxString>>& shortcuts, float x, float y)
    {
        float caption_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
        float caption_x_max = 0.f;
        for (const auto& item : shortcuts) {
            caption_x_max = std::max(caption_x_max, imgui_wrapper->calc_text_size(item.first).x);
        }
        caption_x_max += imgui_wrapper->calc_text_size(": "sv).x + 35.f;

        auto& gizmos_manager = canvas.get_gizmos_manager();
        ImTextureID normal_id = gizmos_manager.get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);

        float scale = canvas.get_scale();
#ifdef WIN32
        int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        scale *= (float)dpi / (float)DPI_DEFAULT;
#endif

        toolbar_circular_button(normal_id, "##tooltip_btn", scale);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip2(ImVec2(x, caption_y));
            for (const auto& item : shortcuts) {
                imgui_wrapper->text_colored(ImGuiWrapper::COL_ACTIVE, item.first + ": ");
                ImGui::SameLine(caption_x_max);
                imgui_wrapper->text_colored(ImGuiWrapper::COL_WINDOW_BG, item.second);
            }
            ImGui::EndTooltip();
        }
    }

    bool render_wiki_guide_button(const GLCanvas3D& canvas, float scale, const char* url)
    {
        auto& gm      = canvas.get_gizmos_manager();
        auto  icon    = gm.get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_WIKI_GUIDE);
        bool  clicked = toolbar_circular_button(icon, "##wiki_guide_btn", scale);
        if (url && *url && clicked)
            wxLaunchDefaultBrowser(wxString::FromUTF8(url));
        if (ImGui::IsItemHovered()){
            if(url && *url)
                ImGui::SetTooltip("%s\n%s", _u8L("Wiki Guide").c_str(), url);
            else
                ImGui::SetTooltip("%s", _u8L("Wiki Guide").c_str());
        }
        return clicked; // for dynamically generated links
    }

    bool render_video_guide_button(const GLCanvas3D& canvas, float scale, const char* url)
    {
        auto& gm      = canvas.get_gizmos_manager();
        auto  icon    = gm.get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_VIDEO_GUIDE);
        bool  clicked = toolbar_circular_button(icon, "##video_guide_btn", scale);
        if (url && *url && clicked)
            wxLaunchDefaultBrowser(wxString::FromUTF8(url));
        if (ImGui::IsItemHovered()){
            if(url && *url)
                ImGui::SetTooltip("%s\n%s", _u8L("Video Guide").c_str(), url);
            else
                ImGui::SetTooltip("%s", _u8L("Video Guide").c_str());
        }
        return clicked; // for dynamically generated links
    }

    bool toolbar_circular_button(ImTextureID textureID, const char* id, float scale)
    {
        ImVec2 btn_sz  = ImVec2(21 * scale, 21 * scale);
        ImVec2 icon_sz = ImVec2(15 * scale, 15 * scale);
        float  btn_pad = (btn_sz.x - icon_sz.x) * .5f;

        ImVec2      p       = ImGui::GetCursorScreenPos();
        bool        clicked = ImGui::InvisibleButton(id, btn_sz);
        ImDrawList* dl      = ImGui::GetWindowDrawList();
        bool        is_dark = ImGuiWrapper::COL_WINDOW_BG.x != ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).x;
        ImVec4      col     = is_dark ? ImGuiWrapper::COL_ORCA_DARK       : ImGuiWrapper::COL_ORCA;
        ImVec4      col_hvr = is_dark ? ImGuiWrapper::COL_ORCA_HOVER_DARK : ImGuiWrapper::COL_ORCA_HOVER;
        dl->AddCircleFilled(
            ImVec2(p.x + btn_sz.x * .5f, p.y + btn_sz.y * .5f),
            btn_sz.x * .5f,
            ImGui::ColorConvertFloat4ToU32((ImGui::IsItemActive()||ImGui::IsItemHovered()) ? col_hvr : col)
        );
        dl->AddImage(textureID, ImVec2(p.x + btn_pad, p.y + btn_pad), ImVec2(p.x + btn_sz.x - btn_pad, p.y + btn_sz.y - btn_pad));

        return clicked;
    }

    void begin_right_aligned_buttons(const std::vector<wxString>& labels)
    {
        float       total_width = 0.0f;
        ImGuiStyle& style = ImGui::GetStyle();
        float       spacing = style.ItemSpacing.x;
        float       padding = style.FramePadding.x * 2.0f;

        // Calculate width
        for (size_t i = 0; i < labels.size(); ++i) {
            total_width += ImGuiWrapper::calc_text_size(labels[i]).x + padding;
            if (i < labels.size() - 1)
                total_width += spacing;
        }

        float avail = ImGui::GetContentRegionAvail().x;

        // Handle Overlap: If the total width of the buttons exceeds available space, move to a new line
        if (total_width > avail) {
            ImGui::NewLine();
            avail = ImGui::GetContentRegionAvail().x; // Reset to full window width
        }

        float posX = ImGui::GetCursorPosX() + std::max(0.0f, avail - total_width);
        ImGui::SetCursorPosX(posX);
    }

    void push_orca_button_style()
    {
        ImVec4 base_orca = ImGuiWrapper::COL_ORCA;

        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(base_orca.x, base_orca.y, base_orca.z, h, s, v);

        ImVec4 hover, active;

        // Lighter variant for Hover (Increase Value by ~12%)
        ImGui::ColorConvertHSVtoRGB(h, s, std::min(v + 0.12f, 1.0f), hover.x, hover.y, hover.z);
        hover.w = base_orca.w;

        // Darker variant for Active (Decrease Value by ~12%)
        ImGui::ColorConvertHSVtoRGB(h, s, std::max(v - 0.12f, 0.0f), active.x, active.y, active.z);
        active.w = base_orca.w;

        ImGui::PushStyleColor(ImGuiCol_Button, base_orca);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);

        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_WINDOW_BG);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    }

    void pop_orca_button_style()
    {
        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(4);
    }

} // namespace Slic3r::GUI::GLGizmoUtils