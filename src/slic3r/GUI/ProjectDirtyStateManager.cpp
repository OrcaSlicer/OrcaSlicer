#include "libslic3r/libslic3r.h"

#include "ProjectDirtyStateManager.hpp"
#include "ImGuiWrapper.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "I18N.hpp"
#include "Plater.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include <algorithm>
#include <assert.h>

namespace Slic3r {
namespace GUI {

void ProjectDirtyStateManager::update_from_undo_redo_stack(bool dirty)
{
    m_plater_dirty = dirty;
    if (const Plater *plater = wxGetApp().plater(); plater && wxGetApp().initialized())
        wxGetApp().mainframe->update_title();
}

void ProjectDirtyStateManager::update_from_presets()
{
    m_presets_dirty = false;
    // check switching of the presets only for exist/loaded project, but not for new
    GUI_App &app = wxGetApp();
    if (!app.plater()->get_project_filename().IsEmpty()) {
        for (const auto &[type, name] : app.get_selected_presets()) { 
            if (type == Preset::Type::TYPE_FILAMENT) { 
                m_presets_dirty |= m_initial_filament_presets_names != wxGetApp().preset_bundle->filament_presets;
                if (ConfigOption *color_option = wxGetApp().preset_bundle->project_config.option("filament_colour")) {
                    auto colors = static_cast<ConfigOptionStrings *>(color_option->clone());
                    m_presets_dirty |= m_initial_filament_presets_colors != colors->values;
                    delete colors;
                }
            } else {
                m_presets_dirty |= !m_initial_presets[type].empty() && m_initial_presets[type] != name;
            }
        }
    } else {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "project file name is empty";
    }
    m_presets_dirty |= app.has_unsaved_preset_changes();
    m_project_config_dirty = m_initial_project_config != app.preset_bundle->project_config;
    app.mainframe->update_title();
}

void ProjectDirtyStateManager::reset_after_save()
{
    this->reset_initial_presets();
    m_plater_dirty  = false;
    m_presets_dirty = false;
    m_project_config_dirty = false;
    wxGetApp().mainframe->update_title();
}

void ProjectDirtyStateManager::reset_initial_presets()
{
    m_initial_presets.fill(std::string{});
    GUI_App &app = wxGetApp();
    for (const auto &[type, name] : app.get_selected_presets()) { 
        if (type == Preset::Type::TYPE_FILAMENT) {
            m_initial_filament_presets_names = wxGetApp().preset_bundle->filament_presets;
            if (ConfigOption *color_option = wxGetApp().preset_bundle->project_config.option("filament_colour")) {
                auto colors = static_cast<ConfigOptionStrings *>(color_option->clone());
                m_initial_filament_presets_colors = colors->values;
                delete colors;
            }
        } else {
            m_initial_presets[type] = name;
        }
    }
    m_initial_project_config = app.preset_bundle->project_config;
}

#if ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW
void ProjectDirtyStateManager::render_debug_window() const
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();

    auto color = [](bool value) {
        return value ? ImVec4(1.0f, 0.49f, 0.216f, 1.0f) /* orange */: ImVec4(1.0f, 1.0f, 1.0f, 1.0f) /* white */;
    };
    auto bool_to_text = [](bool value) {
        return value ? "true" : "false";
    };
    auto append_bool_item = [color, bool_to_text, &imgui](const std::string& name, bool value) {
        imgui.text_colored(color(value), name);
        ImGui::SameLine();
        imgui.text_colored(color(value), bool_to_text(value));
    };
    auto append_int_item = [&imgui](const std::string& name, int value) {
        imgui.text(name);
        ImGui::SameLine();
        imgui.text(std::to_string(value));
    };
    auto append_snapshot_item = [&imgui](const std::string& label, const UndoRedo::Snapshot* snapshot) {
        imgui.text(label);
        ImGui::SameLine(100);
        if (snapshot != nullptr)
            imgui.text(snapshot->name + " (" + std::to_string(snapshot->timestamp) + ")");
        else
            imgui.text("-");
    };

    imgui.begin(std::string("Project dirty state statistics"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    if (ImGui::CollapsingHeader("Dirty state", ImGuiTreeNodeFlags_DefaultOpen)) {
        append_bool_item("Overall:", is_dirty());
        ImGui::Separator();
        append_bool_item("Plater:", m_state.plater);
        append_bool_item("Presets:", m_state.presets);
        append_bool_item("Current gizmo:", m_state.gizmos.current);
    }

    if (ImGui::CollapsingHeader("Last save timestamps", ImGuiTreeNodeFlags_DefaultOpen)) {
        append_int_item("Main:", m_last_save.main);
        append_int_item("Current gizmo:", m_last_save.gizmo);
    }

    const UndoRedo::Stack& main_stack = wxGetApp().plater()->undo_redo_stack_main();
    const UndoRedo::Snapshot* main_active_snapshot = get_active_snapshot(main_stack);
    const UndoRedo::Snapshot* main_last_saveable_snapshot = get_last_saveable_snapshot(EStackType::Main, main_stack, m_state.gizmos, m_last_save.main);
    const std::vector<UndoRedo::Snapshot>& main_snapshots = main_stack.snapshots();

    if (ImGui::CollapsingHeader("Main snapshots", ImGuiTreeNodeFlags_DefaultOpen)) {
        append_snapshot_item("Active:", main_active_snapshot);
        append_snapshot_item("Last saveable:", main_last_saveable_snapshot);
    }

    if (ImGui::CollapsingHeader("Main undo/redo stack", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const UndoRedo::Snapshot& snapshot : main_snapshots) {
            bool active = main_active_snapshot->timestamp == snapshot.timestamp;
            imgui.text_colored(color(active), snapshot.name);
            ImGui::SameLine(150);
            imgui.text_colored(color(active), " (" + std::to_string(snapshot.timestamp) + ")");
            if (&snapshot == main_last_saveable_snapshot) {
                ImGui::SameLine();
                imgui.text_colored(color(active), " (S)");
            }
            if (m_last_save.main > 0 && m_last_save.main == snapshot.timestamp) {
                ImGui::SameLine();
                imgui.text_colored(color(active), " (LS)");
            }
        }
    }

    const UndoRedo::Stack& active_stack = wxGetApp().plater()->undo_redo_stack_active();
    if (&active_stack != &main_stack) {
        if (ImGui::CollapsingHeader("Gizmo undo/redo stack", ImGuiTreeNodeFlags_DefaultOpen)) {
            const UndoRedo::Snapshot* active_active_snapshot = get_active_snapshot(active_stack);
            const std::vector<UndoRedo::Snapshot>& active_snapshots = active_stack.snapshots();
            for (const UndoRedo::Snapshot& snapshot : active_snapshots) {
                bool active = active_active_snapshot->timestamp == snapshot.timestamp;
                imgui.text_colored(color(active), snapshot.name);
                ImGui::SameLine(150);
                imgui.text_colored(color(active), " (" + std::to_string(snapshot.timestamp) + ")");
            }
        }
    }

    imgui.end();
}
#endif // ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW

} // namespace GUI
} // namespace Slic3r

