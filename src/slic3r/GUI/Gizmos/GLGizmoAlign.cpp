#include "GLGizmoAlign.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "libslic3r/Color.hpp"

namespace Slic3r::GUI {

GLGizmoAlign::GLGizmoAlign(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{
}

std::string GLGizmoAlign::on_get_name() const
{
    return _u8L("Align and Distribute");
}

bool GLGizmoAlign::on_is_activable() const
{
    return m_parent.get_selection().get_volume_idxs().size() > 1;
}

CommonGizmosDataID GLGizmoAlign::on_get_requirements() const
{
    return CommonGizmosDataID::None;
}

void GLGizmoAlign::on_render_input_window(float x, float y, float bottom_limit)
{
    if (m_parent.get_selection().get_volume_idxs().size() < 2) {
        // Close the gizmo if selection falls below 2 items
        m_parent.get_gizmos_manager().open_gizmo(GLGizmosManager::EType::Undefined);
        return;
    }

    m_imgui->push_common_window_style(m_parent.get_scale());

    // Position panel relative to coordinates passed
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);

    int flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
    m_imgui->begin(on_get_name(), flags);

    if (ImGui::BeginTable("AlignTable", 4, ImGuiTableFlags_None)) {
        bool is_dark = wxGetApp().app_config->get("dark_color_mode") == "1";
        if (is_dark) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(70.0f / 255.f, 70.0f / 255.f, 75.0f / 255.f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(230.0f / 255.f, 230.0f / 255.f, 235.0f / 255.f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(38.0f / 255.f, 46.0f / 255.f, 48.0f / 255.f, 1.00f));
        }

        // Calculate a uniform width for all buttons
        float btn_width = std::max({
            m_imgui->calc_button_size(_u8L("Min")).x,
            m_imgui->calc_button_size(_u8L("Mid")).x,
            m_imgui->calc_button_size(_u8L("Max")).x
        }) + 12.0f * m_parent.get_scale();

        auto render_centered_header = [&](const ImVec4& color, const std::string& text) {
            float cell_width = ImGui::GetContentRegionAvail().x;
            float text_width = ImGui::CalcTextSize(text.c_str()).x;
            if (cell_width > text_width) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cell_width - text_width) * 0.5f);
            }
            ImGui::TextColored(color, "%s", text.c_str());
        };

        auto render_centered_button = [&](const std::string& label, std::function<void()> onClick) {
            float cell_width = ImGui::GetContentRegionAvail().x;
            if (cell_width > btn_width) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cell_width - btn_width) * 0.5f);
            }
            if (m_imgui->button(label, ImVec2(btn_width, 0.f), true)) {
                onClick();
            }
        };

        // Custom Header Row
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("");
        
        ImGui::TableSetColumnIndex(1);
        render_centered_header(ImGuiWrapper::to_ImVec4(ColorRGBA::X()), "X");
        
        ImGui::TableSetColumnIndex(2);
        render_centered_header(ImGuiWrapper::to_ImVec4(ColorRGBA::Y()), "Y");
        
        ImGui::TableSetColumnIndex(3);
        render_centered_header(ImGuiWrapper::to_ImVec4(ColorRGBA::Z()), "Z");

        // Align rows
        // Row 1: Min
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%s", _u8L("Align").c_str());
        
        ImGui::TableSetColumnIndex(1);
        render_centered_button((_u8L("Min") + "##a_0").c_str(), [&]() {
            m_parent.get_selection().align(0, 0, false);
        });
        ImGui::TableSetColumnIndex(2);
        render_centered_button((_u8L("Min") + "##a_1").c_str(), [&]() {
            m_parent.get_selection().align(1, 0, false);
        });
        ImGui::TableSetColumnIndex(3);
        render_centered_button((_u8L("Min") + "##a_2").c_str(), [&]() {
            m_parent.get_selection().align(2, 0, false);
        });

        // Row 2: Mid
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("");
        
        ImGui::TableSetColumnIndex(1);
        render_centered_button((_u8L("Mid") + "##a_0").c_str(), [&]() {
            m_parent.get_selection().align(0, 1, false);
        });
        ImGui::TableSetColumnIndex(2);
        render_centered_button((_u8L("Mid") + "##a_1").c_str(), [&]() {
            m_parent.get_selection().align(1, 1, false);
        });
        ImGui::TableSetColumnIndex(3);
        render_centered_button((_u8L("Mid") + "##a_2").c_str(), [&]() {
            m_parent.get_selection().align(2, 1, false);
        });

        // Row 3: Max
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("");
        
        ImGui::TableSetColumnIndex(1);
        render_centered_button((_u8L("Max") + "##a_0").c_str(), [&]() {
            m_parent.get_selection().align(0, 2, false);
        });
        ImGui::TableSetColumnIndex(2);
        render_centered_button((_u8L("Max") + "##a_1").c_str(), [&]() {
            m_parent.get_selection().align(1, 2, false);
        });
        ImGui::TableSetColumnIndex(3);
        render_centered_button((_u8L("Max") + "##a_2").c_str(), [&]() {
            m_parent.get_selection().align(2, 2, false);
        });

        // Distribute rows
        // Row 4: Min
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%s", _u8L("Distribute").c_str());
        
        ImGui::TableSetColumnIndex(1);
        render_centered_button((_u8L("Min") + "##d_0").c_str(), [&]() {
            m_parent.get_selection().align(0, 0, true);
        });
        ImGui::TableSetColumnIndex(2);
        render_centered_button((_u8L("Min") + "##d_1").c_str(), [&]() {
            m_parent.get_selection().align(1, 0, true);
        });
        ImGui::TableSetColumnIndex(3);
        render_centered_button((_u8L("Min") + "##d_2").c_str(), [&]() {
            m_parent.get_selection().align(2, 0, true);
        });

        // Row 5: Mid
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("");
        
        ImGui::TableSetColumnIndex(1);
        render_centered_button((_u8L("Mid") + "##d_0").c_str(), [&]() {
            m_parent.get_selection().align(0, 1, true);
        });
        ImGui::TableSetColumnIndex(2);
        render_centered_button((_u8L("Mid") + "##d_1").c_str(), [&]() {
            m_parent.get_selection().align(1, 1, true);
        });
        ImGui::TableSetColumnIndex(3);
        render_centered_button((_u8L("Mid") + "##d_2").c_str(), [&]() {
            m_parent.get_selection().align(2, 1, true);
        });

        // Row 6: Max
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("");
        
        ImGui::TableSetColumnIndex(1);
        render_centered_button((_u8L("Max") + "##d_0").c_str(), [&]() {
            m_parent.get_selection().align(0, 2, true);
        });
        ImGui::TableSetColumnIndex(2);
        render_centered_button((_u8L("Max") + "##d_1").c_str(), [&]() {
            m_parent.get_selection().align(1, 2, true);
        });
        ImGui::TableSetColumnIndex(3);
        render_centered_button((_u8L("Max") + "##d_2").c_str(), [&]() {
            m_parent.get_selection().align(2, 2, true);
        });

        ImGui::PopStyleColor(4);
        ImGui::EndTable();
    }

    m_imgui->end();
    m_imgui->pop_common_window_style();
}

} // namespace Slic3r::GUI
