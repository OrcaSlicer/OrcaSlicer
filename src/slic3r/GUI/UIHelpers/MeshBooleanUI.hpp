#ifndef slic3r_MeshBooleanUI_hpp_
#define slic3r_MeshBooleanUI_hpp_

#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoMeshBoolean.hpp"
#include "libslic3r/Model.hpp"
#include "imgui/imgui.h"
#include <string>
#include <vector>
#include <set>
#include <functional>

namespace Slic3r {
namespace GUI {

// Forward declarations
class GLCanvas3D;
class Selection;
class VolumeListManager;
class BooleanWarningManager;
struct WarningItem;
enum class MeshBooleanOperation;
// enum class DifferenceType;
enum class BooleanTargetMode;

// Structure to hold list item information
struct ListItemInfo {
    std::string name;
    ModelVolumeType type;
    bool is_object_mode;
    int object_idx; // For object mode, this stores the ModelObject index

    ListItemInfo(const std::string& n, ModelVolumeType t, bool obj_mode = false, int obj_idx = -1)
        : name(n), type(t), is_object_mode(obj_mode), object_idx(obj_idx) {}
};

// ========================== MESH BOOLEAN UI CLASS ==========================
// Handles all ImGui rendering and UI interaction for the mesh boolean gizmo

class MeshBooleanUI {
public:
    MeshBooleanUI();
    ~MeshBooleanUI();

    // ========================== MAIN RENDERING INTERFACE ==========================

    // Main UI content rendering method - called from within Gizmo's ImGui window
    void render_content(GLCanvas3D& parent, ImGuiWrapper* imgui, bool is_dark_mode);

    // ========================== UI STATE ACCESS ==========================

    // Get current UI state
    MeshBooleanOperation get_selected_operation() const { return m_operation_mode; }
    // DifferenceType get_difference_type() const { return m_difference_type; }
    BooleanTargetMode get_target_mode() const { return m_target_mode; }
    bool get_keep_original_models() const { return m_keep_original_models; }
    bool get_entity_only() const { return m_entity_only; }

    // Set UI state
    void set_operation_mode(MeshBooleanOperation mode) { m_operation_mode = mode; }
    // void set_difference_type(DifferenceType type) { m_difference_type = type; }
    void set_target_mode(BooleanTargetMode mode) { m_target_mode = mode; }
    void set_keep_original_models(bool keep) { m_keep_original_models = keep; }
    void set_entity_only(bool entity_only) { m_entity_only = entity_only; }

    // External components access (injected dependencies)
    void set_volume_manager(VolumeListManager* manager) { m_volume_manager = manager; }
    void set_warning_manager(BooleanWarningManager* manager) { m_warning_manager = manager; }

    // Callback interface for UI events
    std::function<void()> on_execute_mesh_boolean;
    std::function<void()> on_reset_operation;
    std::function<void(MeshBooleanOperation)> on_apply_color_overrides;
    std::function<bool()> on_delete_selected; // Returns true if delete was successful

    // Async status callbacks
    std::function<bool()> is_async_enabled;
    std::function<bool()> is_async_busy;
    std::function<float()> get_async_progress;
    std::function<void()> on_cancel_async_operation;

    // UI state queries
    bool is_ok_button_enabled() const;
    bool has_non_entity_in_current_lists() const;

    // ========================== ICON MANAGEMENT ==========================

    // Load UI icons
    bool load_icons();

private:
    // ========================== UI STATE ==========================

    MeshBooleanOperation m_operation_mode;
    // DifferenceType m_difference_type;
    BooleanTargetMode m_target_mode;
    bool m_keep_original_models;
    bool m_entity_only;
    bool m_is_dark_mode;

    // External dependencies (injected)
    VolumeListManager* m_volume_manager{nullptr};
    BooleanWarningManager* m_warning_manager{nullptr};
    GLCanvas3D* m_parent{nullptr};
    ImGuiWrapper* m_imgui{nullptr};

    // Dynamic layout dimensions (computed based on content and font size)
    mutable float m_computed_list_width{MeshBooleanConfig::LIST_WIDTH};
    mutable float m_computed_control_width{MeshBooleanConfig::CONTROL_WIDTH};
    mutable float m_computed_tab_height{MeshBooleanConfig::TAB_HEIGHT};
    mutable float m_computed_list_title_height{MeshBooleanConfig::LIST_TITLE_HEIGHT};
    mutable float m_computed_list_item_height{MeshBooleanConfig::LIST_ITEM_HEIGHT};
    mutable float m_computed_icon_size_display{MeshBooleanConfig::ICON_SIZE_DISPLAY};
    mutable float m_computed_icon_size_button{MeshBooleanConfig::ICON_SIZE_BUTTON};

    // ========================== ICON IDs ==========================

    // Operation icons
    ImTextureID m_union_icon_light_id{0};
    ImTextureID m_union_icon_dark_id{0};
    ImTextureID m_intersection_icon_light_id{0};
    ImTextureID m_intersection_icon_dark_id{0};
    ImTextureID m_difference_icon_light_id{0};
    ImTextureID m_difference_icon_dark_id{0};

    // Warning icons
    ImTextureID m_warning_icon_id{0};
    ImTextureID m_error_icon_id{0};

    // Info icons
    ImTextureID m_info_icon_light_id{0};
    ImTextureID m_info_icon_dark_id{0};

    // List item type icons
    ImTextureID m_object_icon_light_id{0};
    ImTextureID m_object_icon_dark_id{0};
    ImTextureID m_part_icon_light_id{0};
    ImTextureID m_part_icon_dark_id{0};
    ImTextureID m_negative_icon_id{0};
    ImTextureID m_modifier_icon_id{0};
    ImTextureID m_support_enforcer_icon_id{0};
    ImTextureID m_support_blocker_icon_id{0};

    // Button icons
    ImTextureID m_delete_light_icon_id{0};
    ImTextureID m_delete_dark_icon_id{0};
    ImTextureID m_delete_hover_icon_id{0};
    ImTextureID m_delete_inactive_icon_id{0};

    ImTextureID m_to_left_light_icon_id{0};
    ImTextureID m_to_left_dark_icon_id{0};
    ImTextureID m_to_left_hover_icon_id{0};
    ImTextureID m_to_left_inactive_icon_id{0};
    ImTextureID m_to_left_clicked_icon_id{0};

    ImTextureID m_to_right_light_icon_id{0};
    ImTextureID m_to_right_dark_icon_id{0};
    ImTextureID m_to_right_hover_icon_id{0};
    ImTextureID m_to_right_inactive_icon_id{0};
    ImTextureID m_to_right_clicked_icon_id{0};

    ImTextureID m_swap_light_icon_id{0};
    ImTextureID m_swap_dark_icon_id{0};
    ImTextureID m_swap_hover_icon_id{0};
    ImTextureID m_swap_inactive_icon_id{0};
    ImTextureID m_swap_clicked_icon_id{0};

    ImTextureID m_check_list_icon_id{0};

    bool m_icons_loaded{false};

    // ========================== UI RENDERING METHODS ==========================

    // Main UI components
    bool draw_operation_mode_tabs();
    void draw_volume_lists();
    void draw_control_buttons();
    void draw_action_buttons();
    void draw_only_entity_checkbox();
    void draw_warnings(); // Warnings and error messages
    void draw_separator(); // Separator line between content and controls
    void draw_progress_bar(); // Async progress display (progress bar + status text)

    // Tab and button helpers
    bool draw_tab_button(const char* icon_name, const char* text, bool selected,
                        const ImVec2& size, bool enabled, int border_type);
    bool operation_button(const char* id, ImTextureID light_id, ImTextureID dark_id,
                         ImTextureID hover_id, ImTextureID inactive_id, ImTextureID clicked_id,
                         bool enabled);

    // List rendering
    void draw_object_list(const std::string& table_name, ImVec2 size,
                         const std::vector<ListItemInfo>& items,
                         const std::set<size_t>& selected_indices,
                         std::function<void(size_t, bool)> on_item_click,
                         ImU32 title_bg_color);
    bool draw_selectable(const ListItemInfo& item_info, bool selected, ImVec2 size);
    void render_group_list(const std::string& table_name, ImVec2 size,
                          const std::vector<ListItemInfo>& items,
                          const std::vector<std::vector<unsigned int>>& groups,
                          ImU32 title_color);

    // UI helpers
    void render_checkbox(const wxString& label, bool& value, bool enabled);
    void set_centered_cursor_x(float width) const;
    bool compute_ok_enabled();

    // List building helpers
    void build_list(const std::vector<unsigned int>& list,
                   std::vector<ListItemInfo>& out_items,
                   std::vector<std::vector<unsigned int>>& out_groups);
    std::set<size_t> groups_to_selected_indices(
        const std::vector<std::vector<unsigned int>>& groups,
        const std::set<unsigned int>& selected);

    // Icon helpers
    ImTextureID get_icon_for_volume_type(ModelVolumeType type) const;
    ImTextureID get_icon_for_object_mode() const;

    // Non-entity detection helper
    bool object_has_non_entity_volumes(int obj_idx) const;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_MeshBooleanUI_hpp_
