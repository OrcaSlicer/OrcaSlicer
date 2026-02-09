#include "MeshBooleanUI.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoMeshBoolean.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/GLTexture.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/UIHelpers/TextEllipsis.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include <algorithm>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

// ========================== IMGUI RAII HELPERS ==========================
// Local RAII helpers for ImGui state management

namespace {
    // Push/Pop one style color in a scope
    struct ImScopedStyleColor {
        ImScopedStyleColor(ImGuiCol idx, const ImVec4 &color) { ImGui::PushStyleColor(idx, color); pushed = true; }
        ~ImScopedStyleColor() { if (pushed) ImGui::PopStyleColor(); }
        bool pushed {false};
    };

    // Push/Pop item flag in a scope
    struct ImScopedItemFlag {
        ImScopedItemFlag(ImGuiItemFlags flag, bool enable) {
            if (!enable) { ImGui::PushItemFlag(flag, true); pushed = true; }
        }
        ~ImScopedItemFlag() { if (pushed) ImGui::PopItemFlag(); }
        bool pushed {false};
    };

    // Push/Pop ID in a scope
    struct ImScopedID {
        explicit ImScopedID(const char* id) { ImGui::PushID(id); }
        ~ImScopedID() { ImGui::PopID(); }
    };
}

namespace Slic3r {
namespace GUI {

// ========================== CONSTRUCTOR/DESTRUCTOR ==========================

MeshBooleanUI::MeshBooleanUI()
    : m_operation_mode(MeshBooleanOperation::Union)
    // , m_difference_type(DifferenceType::A_MINUS_B)
    , m_target_mode(BooleanTargetMode::Part)
    , m_keep_original_models(false)
    , m_entity_only(true)
    , m_is_dark_mode(false)
{
}

MeshBooleanUI::~MeshBooleanUI() = default;

// ========================== MAIN RENDERING INTERFACE ==========================

void MeshBooleanUI::render_content(GLCanvas3D& parent, ImGuiWrapper* imgui, bool is_dark_mode)
{
    // Store context for this render cycle
    m_parent = &parent;
    m_imgui = imgui;
    m_is_dark_mode = is_dark_mode;

    // Guard: if selection is empty, don't render
    const Selection& sel = parent.get_selection();
    if (sel.get_content().empty()) {
        return;
    }

    // Additional guard: check if we have any items in lists
    if (!m_volume_manager) {
        return;
    }

    bool has_any_items = !m_volume_manager->get_working_list().empty()
                       || !m_volume_manager->get_list_a().empty()
                       || !m_volume_manager->get_list_b().empty();
    if (!has_any_items) {
        return;
    }

    // Calculate adaptive heights based on font size at render start
    float font_height = ImGui::GetFontSize();
    float frame_padding_y = ImGui::GetStyle().FramePadding.y;

    m_computed_icon_size_button = std::max(MeshBooleanConfig::ICON_SIZE_BUTTON, font_height + frame_padding_y);
    m_computed_icon_size_display = std::max(MeshBooleanConfig::ICON_SIZE_DISPLAY, font_height - 1.0f * frame_padding_y);

    // Tab button height: max of font and icon, plus padding
    m_computed_tab_height = std::max(font_height, m_computed_icon_size_display) + 4.0f * frame_padding_y;

    // List title height: font size plus padding
    m_computed_list_title_height = font_height + 4.0f * frame_padding_y;

    // List item height: max of font and icon (NO extra padding - Selectable adds it automatically)
    m_computed_list_item_height = std::max(font_height, m_computed_icon_size_display) + 2.0f * frame_padding_y;

    // Reduce overall spacing throughout the interface
    ImVec2 original_item_spacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(original_item_spacing.x, original_item_spacing.y * MeshBooleanConfig::SPACING_VERTICAL_REDUCTION));

    // Draw main UI components
    draw_operation_mode_tabs();
    ImGui::Spacing();

    draw_volume_lists();

    ImGui::Spacing();
    draw_control_buttons();

    // Reduced spacing between control buttons and entity checkbox
    ImVec2 cursor_pos = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(cursor_pos.x, cursor_pos.y + ImGui::GetStyle().ItemSpacing.y * 0.5f));

    draw_only_entity_checkbox();

    // Warnings and errors
    draw_warnings();

    // Separator line
    draw_separator();

    // Progress bar (if async operation is running)
    draw_progress_bar();

    // Action buttons (Execute / Cancel / Reset)
    draw_action_buttons();

    // Request frame updates while async operations are running
    if (is_async_busy && is_async_busy() && m_parent) {
        m_parent->set_as_dirty();
        m_parent->request_extra_frame();
    }

    ImGui::PopStyleVar(1); // Restore original ItemSpacing
}

// ========================== UI COMPONENT STUBS ==========================
// These are temporary stubs - we'll move the actual implementation from GLGizmoMeshBoolean

bool MeshBooleanUI::draw_operation_mode_tabs()
{
    bool clicked_any = false;

    // Compute max tab length to keep three tabs visually aligned
    const int max_tab_length = 4 * ImGui::GetStyle().FramePadding.x + m_computed_icon_size_display + MeshBooleanConfig::ICON_SPACING + std::max(ImGui::CalcTextSize(_u8L("Union").c_str()).x,
        std::max(ImGui::CalcTextSize(_u8L("Difference").c_str()).x, ImGui::CalcTextSize(_u8L("Intersection").c_str()).x)) + MeshBooleanConfig::ICON_PADDING;

    // Calculate tab strip total width and store dynamic widths
    float total_width = 3 * max_tab_length;

    // Compare with LIST_WIDTH and take the larger value for all UI elements
    m_computed_list_width = std::max(total_width + 80.0f, m_computed_list_width);
    m_computed_control_width = m_computed_list_width - 20.0f;

    // Center tab strip
    set_centered_cursor_x(total_width);

    ImGui::BeginGroup();
    if (draw_tab_button("union", _u8L("Union").c_str(), m_operation_mode == MeshBooleanOperation::Union, ImVec2(max_tab_length, m_computed_tab_height), true, 0)) {
        m_operation_mode = MeshBooleanOperation::Union;
        clicked_any = true;
        if (on_apply_color_overrides) on_apply_color_overrides(m_operation_mode);
    }
    ImGui::SameLine(0, 0);
    if (draw_tab_button("intersection", _u8L("Intersection").c_str(), m_operation_mode == MeshBooleanOperation::Intersection, ImVec2(max_tab_length, m_computed_tab_height), true, 1)) {
        m_operation_mode = MeshBooleanOperation::Intersection;
        clicked_any = true;
        if (on_apply_color_overrides) on_apply_color_overrides(m_operation_mode);
    }
    ImGui::SameLine(0, 0);
    if (draw_tab_button("difference", _u8L("Difference").c_str(), m_operation_mode == MeshBooleanOperation::Difference, ImVec2(max_tab_length, m_computed_tab_height), true, 2)) {
        m_operation_mode = MeshBooleanOperation::Difference;
        clicked_any = true;
        if (on_apply_color_overrides) on_apply_color_overrides(m_operation_mode);
    }
    ImGui::EndGroup();

    return clicked_any;
}

void MeshBooleanUI::draw_volume_lists()
{
    if (!m_parent || !m_volume_manager) return;

    const Selection &selection = m_parent->get_selection();
    std::string mode_text = m_target_mode == BooleanTargetMode::Object ? "Object" : "Part";
    std::string title_text = m_target_mode == BooleanTargetMode::Object ? _u8L("Selected Objects") : _u8L("Selected Parts");

    if (m_operation_mode == MeshBooleanOperation::Difference) {
        // Center the AB lists as a group
        set_centered_cursor_x(m_computed_list_width);

        // Header line: Selected Objects [A-B] : (with [A-B] in bold)
        ImGui::BeginGroup();
        ImGui::TextUnformatted(title_text.c_str());
        ImGui::SameLine(0, 0);
        if (m_imgui) m_imgui->push_bold_font();
        ImGui::TextUnformatted(" [A - B]");
        if (m_imgui) m_imgui->pop_bold_font();
        ImGui::SameLine(0, 0);
        ImGui::TextUnformatted(" :");
        ImGui::EndGroup();
        ImGui::Spacing();

        // Center the two lists as a combined group
        set_centered_cursor_x(m_computed_list_width);

        // Wrap both lists in a group for proper centering
        ImGui::BeginGroup();

        // Build A
        std::vector<ListItemInfo> list_a_items;
        std::vector<std::vector<unsigned int>> groups_a;
        build_list(m_volume_manager->get_list_a(), list_a_items, groups_a);
        ImGui::BeginGroup();
        render_group_list(mode_text + "_SUB_A",
                          ImVec2((m_computed_list_width - MeshBooleanConfig::LIST_PADDING) / 2, m_computed_list_title_height * 5.0f),
                          list_a_items, groups_a, MeshBooleanConfig::COLOR_LIST_A);
        ImGui::EndGroup();

        ImGui::SameLine(0, MeshBooleanConfig::LIST_PADDING);

        // Build B
        std::vector<ListItemInfo> list_b_items;
        std::vector<std::vector<unsigned int>> groups_b;
        build_list(m_volume_manager->get_list_b(), list_b_items, groups_b);
        ImGui::BeginGroup();
        render_group_list(mode_text + "_SUB_B",
                          ImVec2((m_computed_list_width - MeshBooleanConfig::LIST_PADDING) / 2, m_computed_list_title_height * 5.0f),
                          list_b_items, groups_b, MeshBooleanConfig::COLOR_LIST_B);
        ImGui::EndGroup();

        ImGui::EndGroup(); // End wrapper group for both lists
    } else {
        // Center the single list
        set_centered_cursor_x(m_computed_list_width);

        std::vector<ListItemInfo> items;
        std::vector<std::vector<unsigned int>> groups;
        build_list(m_volume_manager->get_working_list(), items, groups);
        ImGui::BeginGroup();
        std::string table_text = m_operation_mode == MeshBooleanOperation::Union ? "UNI" : "INT";
        render_group_list(mode_text + "_" + table_text,
                          ImVec2(m_computed_list_width, m_computed_list_title_height * 5.0f), items, groups,
                          MeshBooleanConfig::COLOR_LIST_A);
        ImGui::EndGroup();
    }

    // Update object lists after any changes
    m_volume_manager->update_obj_lists(selection);
}

void MeshBooleanUI::draw_control_buttons()
{
    if (!m_parent || !m_volume_manager || !m_warning_manager) return;

    bool b_async_working_or_cancelling = is_async_busy ? is_async_busy() : false;
    float window_content_width = ImGui::GetWindowWidth();

    // Calculate layout dimensions
    float checkbox_text_width = ImGui::CalcTextSize(_L("Keep original models").c_str()).x;
    float checkbox_icon_width = ImGui::GetFrameHeight() * 0.78f;
    float checkbox_spacing = ImGui::GetStyle().ItemInnerSpacing.x + 4.0f;
    float checkbox_total_width = checkbox_icon_width + checkbox_spacing + checkbox_text_width;
    float button_group_width = (m_operation_mode == MeshBooleanOperation::Difference) ? (m_computed_icon_size_button * 4 + MeshBooleanConfig::ICON_SPACING * 3) : m_computed_icon_size_button;

    // Calculate positions for left-aligned checkbox and right-aligned buttons
    float ctrl_start_x = (ImGui::GetWindowWidth() - m_computed_control_width) * 0.5f;
    float checkbox_pos_x = ctrl_start_x;
    float buttons_pos_x = ctrl_start_x + m_computed_control_width - button_group_width;

    // Position and render checkbox
    ImGui::SetCursorPosX(checkbox_pos_x);
    render_checkbox(_L("Keep original models"), m_keep_original_models, !b_async_working_or_cancelling);

    // Position button group (right-aligned within control width)
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(buttons_pos_x);

    // Check selection state
    bool has_selection = false;
    if (m_operation_mode == MeshBooleanOperation::Difference) {
        for (unsigned int volume_idx : m_volume_manager->get_selected_objects()) {
            if (std::find(m_volume_manager->get_list_a().begin(), m_volume_manager->get_list_a().end(), volume_idx) != m_volume_manager->get_list_a().end() ||
                std::find(m_volume_manager->get_list_b().begin(), m_volume_manager->get_list_b().end(), volume_idx) != m_volume_manager->get_list_b().end()) {
                has_selection = true;
                break;
            }
        }
    } else {
        has_selection = !m_volume_manager->get_selected_objects().empty();
    }

    // Render operation buttons
    if (m_operation_mode == MeshBooleanOperation::Difference) {
        bool has_a_selection = false;
        bool has_b_selection = false;
        bool not_processing = !b_async_working_or_cancelling;

        for (unsigned int volume_idx : m_volume_manager->get_selected_objects()) {
            if (std::find(m_volume_manager->get_list_a().begin(), m_volume_manager->get_list_a().end(), volume_idx) != m_volume_manager->get_list_a().end()) has_a_selection = true;
            if (std::find(m_volume_manager->get_list_b().begin(), m_volume_manager->get_list_b().end(), volume_idx) != m_volume_manager->get_list_b().end()) has_b_selection = true;
        }

        if (operation_button("to_left", m_to_left_light_icon_id, m_to_left_dark_icon_id, m_to_left_hover_icon_id, m_to_left_inactive_icon_id, m_to_left_clicked_icon_id, has_b_selection && not_processing)) {
            m_volume_manager->move_selected_to_left();
            m_volume_manager->update_obj_lists(m_parent->get_selection());
            if (on_apply_color_overrides) on_apply_color_overrides(m_operation_mode);
            m_warning_manager->clear_warnings();
        }
        ImGui::SameLine(0, MeshBooleanConfig::ICON_SPACING);

        if (operation_button("to_right", m_to_right_light_icon_id, m_to_right_dark_icon_id, m_to_right_hover_icon_id, m_to_right_inactive_icon_id, m_to_right_clicked_icon_id, has_a_selection && not_processing)) {
            m_volume_manager->move_selected_to_right();
            m_volume_manager->update_obj_lists(m_parent->get_selection());
            if (on_apply_color_overrides) on_apply_color_overrides(m_operation_mode);
            m_warning_manager->clear_warnings();
        }
        ImGui::SameLine(0, MeshBooleanConfig::ICON_SPACING);

        if (operation_button("swap", m_swap_light_icon_id, m_swap_dark_icon_id, m_swap_hover_icon_id, m_swap_inactive_icon_id, m_swap_clicked_icon_id, not_processing)) {
            m_volume_manager->swap_lists();
            m_volume_manager->update_obj_lists(m_parent->get_selection());
            if (on_apply_color_overrides) on_apply_color_overrides(m_operation_mode);
            m_warning_manager->clear_warnings();
        }
        ImGui::SameLine(0, MeshBooleanConfig::ICON_SPACING);
    }

    if (operation_button("delete", m_delete_light_icon_id, m_delete_dark_icon_id, m_delete_hover_icon_id, m_delete_inactive_icon_id, m_delete_hover_icon_id, has_selection && !b_async_working_or_cancelling)) {
        if (on_delete_selected) {
            bool delete_successful = on_delete_selected();
            if (delete_successful && on_apply_color_overrides) {
                on_apply_color_overrides(m_operation_mode);
            }
        }
    }
}

void MeshBooleanUI::draw_action_buttons()
{
    if (!m_parent || !m_volume_manager || !m_warning_manager || !m_imgui) return;

    ImGui::Spacing();

    // Compute base enable state
    bool enable_button = false;
    if (m_operation_mode == MeshBooleanOperation::Difference) enable_button = m_volume_manager->validate_for_difference();
    else enable_button = (m_operation_mode == MeshBooleanOperation::Union) ? m_volume_manager->validate_for_union() : m_volume_manager->validate_for_intersection();

    // Calculate adaptive button widths based on text
    float button_spacing = 15.0f;
    float button_padding = 2.0f * ImGui::GetStyle().FramePadding.x;
    float min_button_width = 50.0f;

    // Calculate Execute Boolean button width
    std::string execute_text = _L("Execute").ToStdString();
    float execute_text_width = ImGui::CalcTextSize(execute_text.c_str()).x;
    float execute_button_width = std::max(execute_text_width + button_padding, min_button_width);

    // Calculate Reset/Cancel button width
    bool async_is_busy = is_async_busy ? is_async_busy() : false;
    std::string reset_cancel_text = async_is_busy ? _L("Cancel").ToStdString() : _L("Reset").ToStdString();
    float reset_cancel_text_width = ImGui::CalcTextSize(reset_cancel_text.c_str()).x;
    float reset_cancel_button_width = std::max(reset_cancel_text_width + button_padding, min_button_width);

    // Calculate total width for buttons
    float buttons_total_width = execute_button_width + reset_cancel_button_width + button_spacing;

    // Position buttons (right-aligned)
    float line_width = m_computed_list_width - 4.0f;
    float ctrl_start_x = (ImGui::GetWindowWidth() - line_width) * 0.5f;
    float buttons_start_x = ctrl_start_x + line_width - buttons_total_width;
    ImGui::SetCursorPosX(buttons_start_x);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

    // Recompute enabled using helper
    enable_button = is_ok_button_enabled();

    // OK button (green)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.00f, 0.68f, 0.26f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.75f, 0.30f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.60f, 0.22f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

    bool ok_clicked = false;
    if (!enable_button) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        if (m_is_dark_mode) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(39.0f / 255.0f, 39.0f / 255.0f, 39.0f / 255.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(108.0f / 255.0f, 108.0f / 255.0f, 108.0f / 255.0f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(230.0f / 255.0f, 230.0f / 255.0f, 230.0f / 255.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(163.0f / 255.0f, 163.0f / 255.0f, 163.0f / 255.0f, 1.0f));
        }
    }

    ok_clicked = m_imgui->button((_L("Execute") + "##btn").c_str(), execute_button_width, 0.0f);

    if (!enable_button) {
        ImGui::PopItemFlag();
        ImGui::PopStyleColor(2);
    }

    ImGui::PopStyleColor(4); // remove green

    if (ok_clicked && enable_button) {
        m_warning_manager->clear_warnings();
        if (on_execute_mesh_boolean) on_execute_mesh_boolean();
    }

    ImGui::SameLine(0, button_spacing);

    // Cancel/Reset button - context sensitive
    bool cancel_enabled = true; // Always enabled

    // Determine button text based on async state (reuse already calculated text)
    auto button_text = async_is_busy ? _L("Cancel") : _L("Reset");
    bool is_cancel = async_is_busy;

    if (!cancel_enabled) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        if (m_is_dark_mode) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(39.0f / 255.0f, 39.0f / 255.0f, 39.0f / 255.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(108.0f / 255.0f, 108.0f / 255.0f, 108.0f / 255.0f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(230.0f / 255.0f, 230.0f / 255.0f, 230.0f / 255.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(163.0f / 255.0f, 163.0f / 255.0f, 163.0f / 255.0f, 1.0f));
        }
    }
    // Note: Cancel and Reset buttons now use default button colors (no red highlighting)

    if (m_imgui->button((button_text + "##btn").c_str(), reset_cancel_button_width, 0.0f)) {
        if (is_cancel) {
            if (on_cancel_async_operation) on_cancel_async_operation();
        } else {
            m_volume_manager->clear_all();
            m_warning_manager->clear_warnings();
            if (on_reset_operation) on_reset_operation();
        }
    }

    if (!cancel_enabled) {
        ImGui::PopItemFlag();
        ImGui::PopStyleColor(2);
    }

    ImGui::PopStyleVar();
    ImGui::Spacing();
}

void MeshBooleanUI::draw_only_entity_checkbox()
{
    bool b_async_working_or_cancelling = is_async_busy ? is_async_busy() : false;

    // Check if there is non-entity in the current lists
    bool has_non_entity = has_non_entity_in_current_lists();

    // If there is no non-entity, do not show this checkbox
    if (!has_non_entity) return;

    // Match width with control row above (Keep original models row)
    const float info_icon_spacing = 4.0f;

    set_centered_cursor_x(m_computed_control_width);

    // Begin group for horizontal layout
    ImGui::BeginGroup();

    bool previous_entity_only = m_entity_only;
    render_checkbox(_L("Entity Only"), m_entity_only, !b_async_working_or_cancelling);

    // Get checkbox bounding box for proper alignment
    ImVec2  checkbox_size = ImGui::GetItemRectSize();

    // Add info icon
    ImGui::SameLine(0, info_icon_spacing);

    // Get appropriate info icon based on dark mode
    ImTextureID info_icon = m_is_dark_mode ? m_info_icon_dark_id : m_info_icon_light_id;

    if (info_icon) {
        ImVec2 icon_pos = ImGui::GetCursorScreenPos();
        icon_pos.y += (checkbox_size.y - m_computed_icon_size_display) * 0.5f;

        // Draw icon
        ImGui::GetWindowDrawList()->AddImage(
            info_icon,
            icon_pos,
            ImVec2(icon_pos.x + m_computed_icon_size_display, icon_pos.y + m_computed_icon_size_display)
        );

        // Create invisible button for hover detection
        ImGui::SetCursorScreenPos(icon_pos);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::Button("##entity_info", ImVec2(m_computed_icon_size_display, m_computed_icon_size_display));
        ImGui::PopStyleColor(4);

        // Show tooltip on hover
        if (ImGui::IsItemHovered()) {
            if (m_imgui) {
                m_imgui->tooltip(_u8L("Perform Boolean operations on entities only."), ImGui::GetFontSize() * 20.0f);
            } else {
                ImGui::SetTooltip("%s", _u8L("Perform Boolean operations on entities only.").c_str());
            }
        }
    }

    ImGui::EndGroup();

    if (previous_entity_only != m_entity_only) {
        if (on_apply_color_overrides) {
            on_apply_color_overrides(m_operation_mode);
        }
    }
}

// ========================== UI STATE QUERIES ==========================

bool MeshBooleanUI::is_ok_button_enabled() const
{
    return const_cast<MeshBooleanUI*>(this)->compute_ok_enabled();
}

bool MeshBooleanUI::has_non_entity_in_current_lists() const
{
    if (!m_parent || !m_volume_manager) return false;

    const Selection& sel = m_parent->get_selection();
    auto check_list = [&](const std::vector<unsigned int>& vec){
        for (unsigned int idx : vec) {
            const GLVolume* glv = sel.get_volume(idx);
            if (!glv) continue;
            ModelVolume* mv = get_model_volume(*glv, sel.get_model()->objects);
            if (!mv) continue;
            if (!mv->is_model_part()) return true; // non-entity
        }
        return false;
    };
    if (m_operation_mode == MeshBooleanOperation::Difference)
        return check_list(m_volume_manager->get_list_a()) || check_list(m_volume_manager->get_list_b());
    return check_list(m_volume_manager->get_working_list());
}

// ========================== ICON MANAGEMENT ==========================

bool MeshBooleanUI::load_icons()
{
    if (m_icons_loaded) return true;

    std::string resources_path = Slic3r::resources_dir() + "/images/";

    // Structure for icon loading info
    struct IconInfo {
        const char* filename;
        ImTextureID* texture_id;
        bool is_button;
    };

    // Display icons for boolean operations
    std::vector<IconInfo> display_icons = {
        {"bool_union_dark.svg", &m_union_icon_dark_id, false},
        {"bool_intersection_dark.svg", &m_intersection_icon_dark_id, false},
        {"bool_difference_dark.svg", &m_difference_icon_dark_id, false},
        {"bool_union_light.svg", &m_union_icon_light_id, false},
        {"bool_intersection_light.svg", &m_intersection_icon_light_id, false},
        {"bool_difference_light.svg", &m_difference_icon_light_id, false},
        {"warning.svg", &m_warning_icon_id, false},  // Warning icon
        {"error.svg", &m_error_icon_id, false},      // Error icon

        // Info icons
        {"more_info.svg", &m_info_icon_light_id, false},
        {"more_info_dark.svg", &m_info_icon_dark_id, false},

        // List item type icons
        {"bool_object_light.svg", &m_object_icon_light_id, false},
        {"bool_object_dark.svg",  &m_object_icon_dark_id,  false},
        {"bool_part_light.svg", &m_part_icon_light_id, false},
        {"bool_part_dark.svg",  &m_part_icon_dark_id,  false},
        {"menu_add_negative.svg", &m_negative_icon_id, false},
        {"menu_add_modifier.svg", &m_modifier_icon_id, false},
        {"menu_support_enforcer.svg", &m_support_enforcer_icon_id, false},
        {"menu_support_blocker.svg", &m_support_blocker_icon_id, false}
    };

    // Functional button icons with all states (light/default, dark, hover, inactive, clicked)
    std::vector<IconInfo> button_icons = {
        // Delete button states
        {"bool_delete_active_light.svg", &m_delete_light_icon_id, true},
        {"bool_delete_active_dark.svg", &m_delete_dark_icon_id, true},
        {"bool_delete_hover.svg", &m_delete_hover_icon_id, true},
        {"bool_delete_inactive.svg", &m_delete_inactive_icon_id, true},

        // Left arrow button states
        {"bool_to_left_active_light.svg", &m_to_left_light_icon_id, true},
        {"bool_to_left_active_dark.svg", &m_to_left_dark_icon_id, true},
        {"bool_to_left_hover.svg", &m_to_left_hover_icon_id, true},
        {"bool_to_left_inactive.svg", &m_to_left_inactive_icon_id, true},
        {"bool_to_left_clicked.svg", &m_to_left_clicked_icon_id, true},

        // Right arrow button states
        {"bool_to_right_active_light.svg", &m_to_right_light_icon_id, true},
        {"bool_to_right_active_dark.svg", &m_to_right_dark_icon_id, true},
        {"bool_to_right_hover.svg", &m_to_right_hover_icon_id, true},
        {"bool_to_right_inactive.svg", &m_to_right_inactive_icon_id, true},
        {"bool_to_right_clicked.svg", &m_to_right_clicked_icon_id, true},

        // Swap button states
        {"bool_swap_active_light.svg", &m_swap_light_icon_id, true},
        {"bool_swap_active_dark.svg", &m_swap_dark_icon_id, true},
        {"bool_swap_hover.svg", &m_swap_hover_icon_id, true},
        {"bool_swap_inactive.svg", &m_swap_inactive_icon_id, true},
        {"bool_swap_clicked.svg", &m_swap_clicked_icon_id, true},

        // Check list icon (select all)
        {"bool_check_list.svg", &m_check_list_icon_id, true}
    };

    auto load_icons_helper = [this, &resources_path](const std::vector<IconInfo>& icons) -> bool {
        bool all_loaded = true;
        for (const auto& icon : icons) {
            const int size = 64.0f;
            if (IMTexture::load_from_svg_file(resources_path + icon.filename, size, size, *icon.texture_id)) {
                // Successfully loaded
            } else {
                all_loaded = false;
            }
        }
        return all_loaded;
    };

    // Load both sets of icons
    bool display_loaded = load_icons_helper(display_icons);
    bool buttons_loaded = load_icons_helper(button_icons);

    m_icons_loaded = display_loaded && buttons_loaded;
    return m_icons_loaded;
}

// ========================== UI HELPER STUBS ==========================
// These will be moved from GLGizmoMeshBoolean in the next steps

bool MeshBooleanUI::draw_tab_button(const char* icon_name, const char* text, bool selected,
                                   const ImVec2& size, bool enabled, int border_type)
{
    ImScopedID id_scope(text);

    ImTextureID icon_id = 0;
    if (strcmp(icon_name, "union") == 0) {
        icon_id = (selected || m_is_dark_mode) ? (m_union_icon_dark_id ? m_union_icon_dark_id : m_union_icon_light_id)
                                               : (m_union_icon_light_id ? m_union_icon_light_id : m_union_icon_dark_id);
    } else if (strcmp(icon_name, "intersection") == 0) {
        icon_id = (selected || m_is_dark_mode) ? (m_intersection_icon_dark_id ? m_intersection_icon_dark_id : m_intersection_icon_light_id)
                                               : (m_intersection_icon_light_id ? m_intersection_icon_light_id : m_intersection_icon_dark_id);
    } else if (strcmp(icon_name, "difference") == 0) {
        icon_id = (selected || m_is_dark_mode) ? (m_difference_icon_dark_id ? m_difference_icon_dark_id : m_difference_icon_light_id)
                                               : (m_difference_icon_light_id ? m_difference_icon_light_id : m_difference_icon_dark_id);
    }

    ImScopedItemFlag disabled(ImGuiItemFlags_Disabled, enabled);
    ImScopedStyleColor c0(ImGuiCol_Button, ImVec4(0,0,0,0));
    ImScopedStyleColor c1(ImGuiCol_ButtonHovered, ImVec4(0,0,0,0));
    ImScopedStyleColor c2(ImGuiCol_ButtonActive, ImVec4(0,0,0,0));
    ImScopedStyleColor c3(ImGuiCol_Border, ImVec4(0,0,0,0));
    bool clicked = ImGui::Button("##invisible", size);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 button_min = ImGui::GetItemRectMin();
    ImVec2 button_max = ImGui::GetItemRectMax();

    ImU32 bg_color = selected
        ? ImGui::GetColorU32(ImVec4(0, 174.0f/255.0f, 66.0f/255.0f, 1.0f))
        : (ImGui::IsItemHovered()
            ? ImGui::GetColorU32(m_is_dark_mode ? ImVec4(0.3f,0.3f,0.3f,1.0f) : ImVec4(0.9f,0.9f,0.9f,1.0f))
            : ImGui::GetColorU32(m_is_dark_mode ? ImVec4(0.2f,0.2f,0.2f,1.0f) : ImVec4(1.0f,1.0f,1.0f,1.0f)));
    ImU32 border_color = ImGui::GetColorU32(m_is_dark_mode ? ImVec4(0.5f,0.5f,0.5f,1.0f) : ImVec4(0.7f,0.7f,0.7f,1.0f));
    float rounding = MeshBooleanConfig::ROUNDING_BUTTON;

    if (border_type == 0) {
        draw_list->AddRectFilled(button_min, button_max, bg_color, rounding, ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomLeft);
        draw_list->AddRect(button_min, button_max, border_color, rounding, ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomLeft);
    } else if (border_type == 1) {
        draw_list->AddRectFilled(button_min, button_max, bg_color);
        draw_list->AddRectFilled(ImVec2(button_min.x, button_min.y), ImVec2(button_max.x, button_min.y + 1), border_color);
        draw_list->AddRectFilled(ImVec2(button_min.x, button_max.y - 1), ImVec2(button_max.x, button_max.y), border_color);
    } else if (border_type == 2) {
        draw_list->AddRectFilled(button_min, button_max, bg_color, rounding, ImDrawFlags_RoundCornersTopRight | ImDrawFlags_RoundCornersBottomRight);
        draw_list->AddRect(button_min, button_max, border_color, rounding, ImDrawFlags_RoundCornersTopRight | ImDrawFlags_RoundCornersBottomRight);
    }

    const float icon_size = m_computed_icon_size_display;
    const float spacing   = MeshBooleanConfig::ICON_SPACING;
    const float padding   = MeshBooleanConfig::ICON_PADDING;
    ImVec2 text_size = ImGui::CalcTextSize(text);
    float total_width = icon_size + spacing + text_size.x;
    float available_width = (button_max.x - button_min.x) - 2 * padding;
    float start_x = button_min.x + padding + (available_width - total_width) * 0.5f;
    float center_y = button_min.y + (button_max.y - button_min.y) * 0.5f;

    ImVec2 icon_pos = ImVec2(start_x, center_y - icon_size * 0.5f);
    if (icon_id) draw_list->AddImage(icon_id, icon_pos, ImVec2(icon_pos.x + icon_size, icon_pos.y + icon_size));

    ImVec2 text_pos = ImVec2(start_x + icon_size + spacing, center_y - text_size.y * 0.5f);
    ImU32 text_color = selected ? IM_COL32(255,255,255,255)
                                : (m_is_dark_mode ? IM_COL32(200,200,200,255) : IM_COL32(50,50,50,255));
    draw_list->AddText(text_pos, text_color, text);

    return clicked && enabled;
}

bool MeshBooleanUI::operation_button(const char* id, ImTextureID light_id, ImTextureID dark_id,
                                    ImTextureID hover_id, ImTextureID inactive_id, ImTextureID clicked_id,
                                    bool enabled)
{
    if (!enabled) {
        ImGui::Image(m_is_dark_mode ? hover_id : inactive_id, ImVec2(m_computed_icon_size_button, m_computed_icon_size_button));
        return false;
    }

    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));

    bool clicked = ImGui::Button("##op", ImVec2(m_computed_icon_size_button, m_computed_icon_size_button));

    ImTextureID icon_id = nullptr;
    if (ImGui::IsItemActive()) {
        icon_id = clicked_id;
    } else if (ImGui::IsItemHovered()) {
        icon_id = m_is_dark_mode ? inactive_id : hover_id;
    } else {
        icon_id = m_is_dark_mode ? dark_id : light_id;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 button_min = ImGui::GetItemRectMin();
    ImVec2 button_max = ImGui::GetItemRectMax();
    draw_list->AddImage(icon_id, button_min, button_max);

    ImGui::PopStyleColor(4);
    ImGui::PopID();
    return clicked;
}

void MeshBooleanUI::draw_object_list(const std::string& table_name, ImVec2 size,
                                    const std::vector<ListItemInfo>& items,
                                    const std::set<size_t>& selected_indices,
                                    std::function<void(size_t, bool)> on_item_click,
                                    ImU32 title_bg_color)
{
    // Set corner rounding
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, MeshBooleanConfig::ROUNDING_LIST);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, MeshBooleanConfig::ROUNDING_LIST);

    // Main list background
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImU32 bg_color = ImGui::GetColorU32(ImGuiCol_FrameBg);
    ImU32 border_color = ImGui::GetColorU32(ImGuiCol_Border);

    draw_list->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg_color, MeshBooleanConfig::ROUNDING_LIST);
    draw_list->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), border_color, MeshBooleanConfig::ROUNDING_LIST);

    // Title row
    ImVec2 title_min = ImVec2(pos.x, pos.y);
    ImVec2 title_max = ImVec2(pos.x + size.x, pos.y + m_computed_list_title_height);
    draw_list->AddRectFilled(title_min, title_max, title_bg_color, MeshBooleanConfig::ROUNDING_LIST, ImDrawFlags_RoundCornersTop);

    // Select all button on the left side of title bar
    bool all_items_selected = !items.empty() && selected_indices.size() == items.size();
    if (m_check_list_icon_id && !items.empty()) {
        float button_size = m_computed_icon_size_display;
        float button_margin_y = (m_computed_list_title_height - button_size) * 0.5f;
        float button_margin_x = 6.0f;
        ImVec2 button_pos = ImVec2(title_min.x + button_margin_x, title_min.y + button_margin_y);
        ImVec2 button_max = ImVec2(button_pos.x + button_size, button_pos.y + button_size);

        // Create invisible button for click detection with unique ID
        ImGui::PushID(table_name.c_str());
        ImGui::SetCursorPos(ImVec2(button_pos.x - ImGui::GetWindowPos().x, button_pos.y - ImGui::GetWindowPos().y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        bool select_all_clicked = ImGui::Button("##select_all", ImVec2(button_size, button_size));
        ImGui::PopStyleColor(4);

        // Draw the check list icon
        ImU32 icon_color = ImGui::IsItemHovered() ? IM_COL32(0, 0, 0, 255) : IM_COL32(0, 0, 0, 180);
        draw_list->AddImage(m_check_list_icon_id, button_pos, button_max, ImVec2(0,0), ImVec2(1,1), icon_color);

        ImGui::PopID();

        // Handle click: toggle select all/none
        if (select_all_clicked) {
            if (all_items_selected) {
                // Deselect all
                for (size_t i = 0; i < items.size(); ++i) {
                    if (selected_indices.find(i) != selected_indices.end()) {
                        on_item_click(i, true);  // was_selected = true
                    }
                }
            } else {
                // Select all
                for (size_t i = 0; i < items.size(); ++i) {
                    if (selected_indices.find(i) == selected_indices.end()) {
                        on_item_click(i, false);  // was_selected = false
                    }
                }
            }
        }
    }

    // Make title text center with item count
    std::string title;
    if (table_name.find("SUB_A") != std::string::npos)
        title = "A";
    else if (table_name.find("SUB_B") != std::string::npos)
        title = "B";
    else
        title = m_target_mode == BooleanTargetMode::Object ? _u8L("Selected Objects") : _u8L("Selected Parts");

    // Add count to title
    std::string count_text = "(" + std::to_string(items.size()) + ")";
    ImVec2 title_text_size = ImGui::CalcTextSize(title.c_str());
    ImFont* font = ImGui::GetFont();
    float smaller_font_size = font->FontSize * 0.85f;
    ImVec2 count_text_size_small = font->CalcTextSizeA(smaller_font_size, FLT_MAX, 0.0f, count_text.c_str());

    // Calculate total width (title + spacing + count with smaller font)
    float total_width = title_text_size.x + MeshBooleanConfig::SPACING_LIST_TITLE + count_text_size_small.x;

    // Position for centered text
    ImVec2 start_pos = ImVec2(
        title_min.x + (title_max.x - title_min.x - total_width) * 0.5f,
        title_min.y + (m_computed_list_title_height - title_text_size.y) * 0.5f
    );

    // Draw title text (normal size)
    ImGui::SetCursorPos(ImVec2(start_pos.x - ImGui::GetWindowPos().x,
                               start_pos.y - ImGui::GetWindowPos().y));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));
    ImGui::Text(title.c_str());

    // Draw count text (smaller size) with some spacing
    ImGui::SameLine(0, MeshBooleanConfig::SPACING_LIST_TITLE); // Add 4 pixels spacing between title and count

    // Count text with proper vertical alignment
    ImDrawList* draw_list_text = ImGui::GetWindowDrawList();
    ImVec2 count_pos = ImGui::GetCursorScreenPos();

    // Calculate vertical adjustment to align with main title baseline
    float normal_font_size = font->FontSize;
    float baseline_offset = (normal_font_size - smaller_font_size) * 0.5f;
    count_pos.y += 2 * baseline_offset;
    draw_list_text->AddText(font, smaller_font_size, count_pos, IM_COL32(0, 0, 0, 255), count_text.c_str());

    ImGui::PopStyleColor();

    // Set list start position - add spacing below the title
    float title_to_list_spacing = MeshBooleanConfig::SPACING_LIST_TITLE;
    float bottom_margin = MeshBooleanConfig::MARGIN_BOTTOM;
    ImGui::SetCursorPos(ImVec2(pos.x - ImGui::GetWindowPos().x, title_max.y + title_to_list_spacing - ImGui::GetWindowPos().y));

    // Set list item common style
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, MeshBooleanConfig::SPACING_LIST_ITEM));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(MeshBooleanConfig::PADDING_LIST_FRAME_X, MeshBooleanConfig::PADDING_LIST_FRAME_Y));

    // Restore corner setting to avoid affecting the controls in the child window
    ImGui::PopStyleVar(2); // restore FrameRounding and ChildRounding

    // Set scrollbar color
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(220, 220, 220, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(200, 200, 200, 200));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, IM_COL32(180, 180, 180, 220));

    // Create child window for list content - don't reduce height for bottom margin to avoid clipping
    float available_height = size.y - m_computed_list_title_height - title_to_list_spacing;
    ImGui::BeginChild((std::string("list_") + table_name).c_str(), ImVec2(size.x, available_height), false);

    // Draw list items with spacing between them
    for (size_t i = 0; i < items.size(); i++) {
        ImGui::PushID((int)i);
        bool is_selected = selected_indices.find(i) != selected_indices.end();
        ImVec2 item_size = ImVec2(size.x - 16, 0);
        if (draw_selectable(items[i], is_selected, item_size)) {
            on_item_click(i, is_selected);
        }
        ImGui::PopID();
    }

    // Add bottom padding to ensure last item is fully visible
    ImGui::Dummy(ImVec2(0, bottom_margin));

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

bool MeshBooleanUI::draw_selectable(const ListItemInfo& item_info, bool selected, ImVec2 size)
{
    ImGui::Indent(8);

    // Manually calculate position to ensure left padding and vertical center
    ImVec2 item_min = ImGui::GetCursorScreenPos();

    // Force set square corners, override any corner setting
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, MeshBooleanConfig::ROUNDING_NONE);

    // Make Selectable fully transparent, only for click detection
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0, 0, 0, 0));
    bool clicked = ImGui::Selectable("##selectable", selected, ImGuiSelectableFlags_AllowItemOverlap, ImVec2(size.x, m_computed_list_item_height));
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(1);

    // Get actual item area
    ImVec2 item_max = ImGui::GetItemRectMax();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Get child window clipping area, avoid drawing out of bounds
    ImVec2 child_min = ImGui::GetWindowPos();
    ImVec2 child_max = ImVec2(child_min.x + ImGui::GetWindowSize().x, child_min.y + ImGui::GetWindowSize().y);

    // Check if there is a scrollbar, only adjust the right boundary when there is a scrollbar
    bool has_scrollbar = ImGui::GetScrollMaxY() > 0;
    float content_right = child_max.x;
    if (has_scrollbar) {
        float scrollbar_width = ImGui::GetStyle().ScrollbarSize;
        content_right = child_max.x - scrollbar_width;
    }

    // Add margin to avoid covering the outer frame, adjust right margin to match left margin
    float margin = MeshBooleanConfig::MARGIN_LAYOUT;
    float right_margin = margin + MeshBooleanConfig::MARGIN_RIGHT_OFFSET;
    ImVec2 bg_min = ImVec2(
        std::max(item_min.x + margin, child_min.x + margin),
        std::max(item_min.y, child_min.y + margin)
    );
    ImVec2 bg_max = ImVec2(
        std::min(item_max.x - right_margin, content_right - right_margin),
        std::min(item_max.y, child_max.y - margin)
    );

    // The right boundary of the border also uses the same right margin
    ImVec2 border_max = ImVec2(std::min(item_max.x - right_margin, content_right - right_margin), item_max.y);

    // Draw the background of the selected state - use clipping margin to avoid covering the outer frame
    if (selected && bg_min.x < bg_max.x && bg_min.y < bg_max.y) {
        if (m_is_dark_mode) {
            // #244135 for dark mode selected background
            draw_list->AddRectFilled(bg_min, bg_max, MeshBooleanConfig::COLOR_SELECTED_BG_DARK);
        } else {
            draw_list->AddRectFilled(bg_min, bg_max, MeshBooleanConfig::COLOR_SELECTED_BG);
        }
    }

    // Draw the hover border - use the adjusted right boundary, end at the left side of the scrollbar
     if (ImGui::IsItemHovered()) {
        draw_list->AddRect(item_min, border_max, MeshBooleanConfig::COLOR_HOVER_BORDER, 0.0f, 0, MeshBooleanConfig::BORDER_WIDTH_HOVER);
    }

    // Get appropriate icon for the item
    ImTextureID icon_id = nullptr;
    if (item_info.is_object_mode) {
        icon_id = get_icon_for_object_mode();
    } else {
        icon_id = get_icon_for_volume_type(item_info.type);
    }

    // Calculate positions for icon and text
    const float icon_size = m_computed_icon_size_display;
    const float icon_spacing = 4.0f; // spacing between icon and text

    float content_start_x = item_min.x + MeshBooleanConfig::SPACING_TEXT;
    float actual_height = item_max.y - item_min.y;

    // Determine if this is a non-entity item for graying out
    bool is_non_entity = !item_info.is_object_mode && item_info.type != ModelVolumeType::MODEL_PART;
    bool should_gray_out = m_entity_only && is_non_entity;

    // Draw icon if available (prefer nearest sampling to avoid blur on integer pixel grid)
    if (icon_id) {
        ImVec2 icon_pos = ImVec2(
            content_start_x,
            item_min.y + (actual_height - icon_size) * 0.5f
        );
        // Snap to pixel to avoid blurriness on fractional coordinates
        icon_pos.x = IM_ROUND(icon_pos.x);
        icon_pos.y = IM_ROUND(icon_pos.y);
        ImVec2 icon_max = ImVec2(icon_pos.x + icon_size, icon_pos.y + icon_size);
        icon_max.x = IM_ROUND(icon_max.x);
        icon_max.y = IM_ROUND(icon_max.y);
        draw_list->AddImage(icon_id, icon_pos, icon_max, ImVec2(0,0), ImVec2(1,1), IM_COL32_WHITE);
        content_start_x += icon_size + icon_spacing;
    }

    // Compute available width for text, reserving space for a possible right-side warning icon in object mode
    bool will_show_warning_icon = item_info.is_object_mode && m_warning_icon_id && object_has_non_entity_volumes(item_info.object_idx);
    const float warning_icon_size = m_computed_icon_size_display;
    const float warning_icon_margin = 8.0f;
    float right_reserved = will_show_warning_icon ? (warning_icon_size + warning_icon_margin) : 0.0f;

    float text_available_width = std::max(0.0f, (bg_max.x - right_reserved) - content_start_x);

    // Prepare display text with ellipsis if needed
    std::string display_text = ellipsize_text_imgui(item_info.name, text_available_width);
    ImVec2 text_size = ImGui::CalcTextSize(display_text.c_str());
    ImVec2 text_pos = ImVec2(
        content_start_x,
        item_min.y + (actual_height - text_size.y) * 0.5f
    );
    text_pos.x = IM_ROUND(text_pos.x);
    text_pos.y = IM_ROUND(text_pos.y);

    // Determine text color: gray out non-entity items when Entity Only mode is enabled
    ImU32 text_color;
    if (should_gray_out) {
        // Gray color for non-entity items when Entity Only is enabled
        text_color = m_is_dark_mode ? IM_COL32(108, 108, 108, 255) : IM_COL32(163, 163, 163, 255);
    } else {
        // Normal text color
        text_color = m_is_dark_mode ? MeshBooleanConfig::COLOR_TEXT_DARK : MeshBooleanConfig::COLOR_TEXT;
    }

    // Use AddText with same rounding for start position to avoid sub-pixel blur
    draw_list->AddText(text_pos, text_color, display_text.c_str());

    // Draw warning icon at the right end for object mode if object contains non-entity volumes
    if (will_show_warning_icon) {
        ImVec2 warning_pos = ImVec2(
            bg_max.x - warning_icon_size - warning_icon_margin,
            item_min.y + (actual_height - warning_icon_size) * 0.5f
        );
        warning_pos.x = IM_ROUND(warning_pos.x);
        warning_pos.y = IM_ROUND(warning_pos.y);

        ImVec2 warning_max = ImVec2(warning_pos.x + warning_icon_size, warning_pos.y + warning_icon_size);
        warning_max.x = IM_ROUND(warning_max.x);
        warning_max.y = IM_ROUND(warning_max.y);

        // draw image first
        draw_list->AddImage(m_warning_icon_id, warning_pos, warning_max, ImVec2(0,0), ImVec2(1,1), IM_COL32_WHITE);

        // hover hitbox: add invisible button over the icon rect
        ImGui::SetCursorScreenPos(warning_pos);
        ImGui::PushID("warn_icon");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0,0,0,0));
        ImGui::Button("##warn", ImVec2(warning_icon_size, warning_icon_size));
        ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered()) {
            if (m_imgui) m_imgui->tooltip(_L("This object contains non-part components."), ImGui::GetFontSize() * 20.0f);
            else ImGui::SetTooltip("%s", "This object contains non-part components.");
        }
        ImGui::PopID();
        // restore cursor to avoid affecting subsequent layout
        ImGui::SetCursorScreenPos(ImVec2(item_max.x, ImGui::GetCursorScreenPos().y));
    }

    ImGui::Unindent(8);
    return clicked;
}

void MeshBooleanUI::render_group_list(const std::string& table_name, ImVec2 size,
                                     const std::vector<ListItemInfo>& items,
                                     const std::vector<std::vector<unsigned int>>& groups,
                                     ImU32 title_color)
{
    if (!m_volume_manager) return;

    auto &selected = m_volume_manager->get_selected_objects();
    std::set<size_t> selected_indices = groups_to_selected_indices(groups, selected);

    draw_object_list(table_name, size, items, selected_indices,
        [&](size_t index, bool was_selected){
            if (index >= groups.size()) return;
            const auto &vols = groups[index];
            if (was_selected) for (unsigned int v : vols) m_volume_manager->remove_from_selection(v);
            else              for (unsigned int v : vols) m_volume_manager->add_to_selection(v);
        }, title_color);
}

void MeshBooleanUI::render_checkbox(const wxString& label, bool& value, bool enabled)
{
    ImGui::PushItemFlag(ImGuiItemFlags_Disabled, !enabled);
    ImVec2 original_spacing = ImGui::GetStyle().ItemInnerSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(original_spacing.x + 4.0f, original_spacing.y));
    if (m_imgui) {
        m_imgui->bbl_checkbox(label, value);
    } else {
        // Fallback if m_imgui is not available
        std::string label_str = label.ToStdString();
        ImGui::Checkbox(label_str.c_str(), &value);
    }
    ImGui::PopStyleVar(1);
    ImGui::PopItemFlag();
}

void MeshBooleanUI::set_centered_cursor_x(float width) const
{
    float window_width = ImGui::GetWindowWidth();

    // Use a minimum window width to prevent layout jumping on first render
    float min_expected_width = m_computed_list_width + 40.0f;
    if (window_width < min_expected_width) {
        window_width = min_expected_width;
    }

    float start_x = (window_width - width) * 0.5f;
    ImGui::SetCursorPosX(start_x);
}

bool MeshBooleanUI::compute_ok_enabled()
{
    if (!m_volume_manager || !m_warning_manager) return false;

    // Base validation by mode using minimal strategy
    bool enabled = false;
    switch (m_operation_mode) {
        case MeshBooleanOperation::Union:
            enabled = m_volume_manager->validate_for_union();
            break;
        case MeshBooleanOperation::Intersection:
            enabled = m_volume_manager->validate_for_intersection();
            break;
        case MeshBooleanOperation::Difference:
            enabled = m_volume_manager->validate_for_difference();
            break;
        default: enabled = false; break;
    }

    // Async state - disable if busy
    if (is_async_enabled && is_async_enabled() && is_async_busy) {
        enabled = enabled && !is_async_busy();
    }

    // Error warnings present → disable OK
    if (m_warning_manager->has_errors_for_mode(m_operation_mode)) {
        enabled = false;
    }

    return enabled;
}

void MeshBooleanUI::build_list(const std::vector<unsigned int>& list,
                              std::vector<ListItemInfo>& out_items,
                              std::vector<std::vector<unsigned int>>& out_groups)
{
    if (!m_parent || !m_volume_manager) return;

    const Selection &selection = m_parent->get_selection();
    out_items.clear();
    out_groups.clear();

    if (m_target_mode == BooleanTargetMode::Object) {
        const std::vector<int> &obj_list = &list == &m_volume_manager->get_list_a() ?
                                               m_volume_manager->get_object_list_a() :
                                               (&list == &m_volume_manager->get_list_b() ? m_volume_manager->get_object_list_b() : m_volume_manager->get_object_working_list());
        for (int obj_idx : obj_list) {
            ModelObject *mo = selection.get_model()->objects[obj_idx];
            std::string obj_name = mo ? mo->name : std::string("Object");
            out_items.emplace_back(obj_name, ModelVolumeType::MODEL_PART, true, obj_idx); // Object mode uses MODEL_PART as type, is_object_mode = true, pass obj_idx
            out_groups.emplace_back(selection.get_volume_idxs_from_object((unsigned int) obj_idx));
        }
    } else {
        for (unsigned int idx : list) {
            const GLVolume *glv = selection.get_volume(idx);
            if (!glv) continue;
            ModelVolume *mv = get_model_volume(*glv, selection.get_model()->objects);
            if (!mv) continue;
            out_items.emplace_back(mv->name, mv->type(), false); // Part mode uses actual volume type, is_object_mode = false
            out_groups.push_back({idx});
        }
    }
}

std::set<size_t> MeshBooleanUI::groups_to_selected_indices(
    const std::vector<std::vector<unsigned int>>& groups,
    const std::set<unsigned int>& selected)
{
    std::set<size_t> out;
    for (size_t i = 0; i < groups.size(); ++i) {
        const auto &vols = groups[i];
        if (std::any_of(vols.begin(), vols.end(), [&](unsigned int v){ return selected.count(v) > 0; }))
            out.insert(i);
    }
    return out;
}

void MeshBooleanUI::draw_warnings()
{
    if (!m_warning_manager || !m_volume_manager || !m_imgui) return;

    auto current_warnings = m_warning_manager->get_warnings_for_current_mode(m_operation_mode);
    if (!current_warnings.empty()) {
        m_warning_manager->render_warnings_list(current_warnings, m_computed_control_width,
            m_warning_icon_id, m_error_icon_id, m_imgui, m_computed_icon_size_display);
    } else {
        auto hints = m_warning_manager->get_inline_hints_for_state(m_operation_mode, *m_volume_manager);
        if (!hints.empty()) {
            m_warning_manager->render_warnings_list(hints, m_computed_control_width,
                m_warning_icon_id, m_error_icon_id, m_imgui, m_computed_icon_size_display);
        }
    }
}

void MeshBooleanUI::draw_separator()
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    float separator_start_x = (ImGui::GetWindowWidth() - m_computed_list_width) * 0.5f;
    ImVec2 separator_pos = ImGui::GetCursorScreenPos();
    separator_pos.x = ImGui::GetWindowPos().x + separator_start_x;
    ImU32 separator_color = m_is_dark_mode ? MeshBooleanConfig::COLOR_SEPARATOR_DARK : MeshBooleanConfig::COLOR_SEPARATOR;
    draw_list->AddLine(ImVec2(separator_pos.x, separator_pos.y),
                       ImVec2(separator_pos.x + m_computed_list_width, separator_pos.y),
                       separator_color, 1.0f);

    ImGui::Spacing();
    ImGui::Spacing();
}

void MeshBooleanUI::draw_progress_bar()
{
    // Check if we need to draw progress bar
    bool async_enabled = is_async_enabled ? is_async_enabled() : false;
    bool async_is_busy = is_async_busy ? is_async_busy() : false;

    // Only show progress bar if async is enabled and busy
    if (!async_enabled || !async_is_busy) {
        return;
    }

    float progress = get_async_progress ? get_async_progress() : 0.0f;

    // Calculate layout
    float line_width = m_computed_list_width - 4.0f;
    float ctrl_start_x = (ImGui::GetWindowWidth() - line_width) * 0.5f;

    // === First line: Progress bar with percentage ===
    ImGui::SetCursorPosX(ctrl_start_x);
    ImGui::BeginGroup();

    // Calculate percentage text width
    std::string progress_text = GUI::format(_L("%1%"), int(progress + 0.5f)) + "%%";
    float progress_text_width = ImGui::CalcTextSize(progress_text.c_str()).x;
    float text_padding = 8.0f;

    // Progress bar width
    float progress_bar_width = line_width - progress_text_width - text_padding;

    // Draw progress bar
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.00f, 0.68f, 0.26f, 1.00f));
    ImVec2 progress_size(progress_bar_width, 0.0f);
    ImGui::BBLProgressBar2(progress / 100.0f, progress_size);
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar();

    // Draw percentage text on the same line
    ImGui::SameLine(0, text_padding);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.42f, 0.42f, 0.42f, 1.00f), progress_text.c_str());

    ImGui::EndGroup();

    // === Second line: Status text ===
    ImGui::SetCursorPosX(ctrl_start_x);

    // Determine status text and color based on progress
    std::string status_text;
    ImVec4 text_color = ImVec4(0.42f, 0.42f, 0.42f, 1.0f);

    if (progress < 10.0f) {
        status_text = _u8L("Preparing data...");
    } else if (progress < 90.0f) {
        status_text = _u8L("Computing...");
    } else {
        status_text = _u8L("Applying result..");
    }

    ImGui::TextColored(text_color, status_text.c_str());

    ImGui::Spacing();
}

// ========================== ICON HELPER FUNCTIONS ==========================

ImTextureID MeshBooleanUI::get_icon_for_volume_type(ModelVolumeType type) const
{
    switch (type) {
        case ModelVolumeType::MODEL_PART:
            return m_is_dark_mode && this->m_part_icon_dark_id ? this->m_part_icon_dark_id : this->m_part_icon_light_id;
        case ModelVolumeType::NEGATIVE_VOLUME:
            return this->m_negative_icon_id;
        case ModelVolumeType::PARAMETER_MODIFIER:
            return this->m_modifier_icon_id;
        case ModelVolumeType::SUPPORT_ENFORCER:
            return this->m_support_enforcer_icon_id;
        case ModelVolumeType::SUPPORT_BLOCKER:
            return this->m_support_blocker_icon_id;
        default:
            return this->m_part_icon_light_id; // fallback
    }
}

ImTextureID MeshBooleanUI::get_icon_for_object_mode() const
{
    return m_is_dark_mode && this->m_object_icon_dark_id ? this->m_object_icon_dark_id : this->m_object_icon_light_id;
}

bool MeshBooleanUI::object_has_non_entity_volumes(int obj_idx) const
{
    if (!m_parent) return false;

    const Selection& sel = m_parent->get_selection();
    const Model* model = sel.get_model();
    if (!model || obj_idx < 0 || obj_idx >= static_cast<int>(model->objects.size())) return false;

    const ModelObject* mo = model->objects[obj_idx];
    if (!mo) return false;

    // Check all volumes in this object
    for (const ModelVolume* mv : mo->volumes) {
        if (mv && !mv->is_model_part()) {
            return true; // Found a non-entity volume
        }
    }

    return false; // All volumes are entities (MODEL_PART)
}

} // namespace GUI
} // namespace Slic3r
