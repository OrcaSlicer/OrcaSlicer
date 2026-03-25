#pragma once

#include "GUI_Utils.hpp"
#include "UnsavedChangesDialog.hpp"
#include "slic3r/Utils/SyncBackend.hpp"

namespace Slic3r { namespace GUI {

// Field-level merge dialog for self-hosted profile sync conflicts.
//
// Unlike DiffPresetDialog (which compares presets already loaded into
// PresetBundle by name), this dialog operates on raw JSON strings
// received from the sync backend.  It parses both versions into
// DynamicPrintConfig, diffs them, and lets the user cherry-pick
// individual fields.  The result is a rebuilt JSON string written
// back to disk — no temporary preset objects are needed.
//
// Reuses DiffViewCtrl for the checkbox tree, but owns its own button
// set (Apply Merge / Keep All Local / Keep All Remote / Skip) and
// returns SyncConflictResult instead of modifying presets in-place.
class SyncMergeDialog : public DPIDialog
{
public:
    SyncMergeDialog(wxWindow* parent, const SyncConflict& conflict);

    SyncConflictResult get_result() const { return m_result; }
    bool               has_visible_diffs() const { return m_has_visible_diffs; }

    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    SyncConflictResult     m_result;
    DiffViewCtrl*      m_tree{nullptr};
    Preset::Type       m_preset_type{Preset::Type::TYPE_INVALID};
    std::string        m_local_json_content;
    DynamicPrintConfig m_local_config;
    DynamicPrintConfig m_remote_config;

    bool               m_parse_ok{false};
    bool               m_has_visible_diffs{false};

    // Deletion conflict support
    bool               m_is_deletion_conflict{false};
    bool               m_local_deleted{false};
    bool               m_remote_deleted{false};

    void build_ui(const SyncConflict& conflict);
    bool parse_configs(const SyncConflict& conflict);
    void populate_tree();
    std::string build_merged_json(const std::vector<std::string>& selected_remote_keys,
                                  const std::string& base_json_content);

    static std::string format_time(long long timestamp);
};

}} // namespace Slic3r::GUI
