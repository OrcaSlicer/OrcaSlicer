#ifndef slic3r_GUI_FilamentInventoryEditor_hpp_
#define slic3r_GUI_FilamentInventoryEditor_hpp_

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <wx/colour.h>
#include <wx/dialog.h>
#include <wx/event.h>

#include "libslic3r/FilamentInventory.hpp"

class wxColourPickerCtrl;
class wxBoxSizer;
class wxSizer;
class wxStaticText;
class Button;

namespace Slic3r {
class Preset;
namespace GUI {

// "Printer Material Settings" editor for one printer preset's FilamentInventory.
// Load-modify-save: ids of untouched rows never change, since plate mappings reference
// physical filaments by id across sessions.
class PhysicalFilamentComboBox;
class SlotGridPanel;

class FilamentInventoryEditor : public wxDialog
{
public:
    FilamentInventoryEditor(wxWindow* parent, const std::string& printer_preset_name, size_t tool_count);

private:
    // color_touched/type_touched are separate so picking only a type doesn't persist the
    // picker's placeholder color, and vice versa.
    // The widget pointers are non-null only while the row's editor dialog is open.
    struct Row
    {
        bool                  color_touched{false};
        bool                  type_touched{false};
        wxColour              last_color{0xD9, 0xD9, 0xD9};
        std::string           picked_preset; // canonical filament preset name ("" = none)
        // Fallback when a picked preset's filament_type can't be resolved.
        std::string           loaded_type;
        wxColourPickerCtrl*   color_picker{nullptr};
        PhysicalFilamentComboBox* type_choice{nullptr};
        wxWindow*             action_btn{nullptr}; // Clear
    };

    // device() with the rows' color, type and preset; ids and slot topology stay the record's.
    FilamentInventory edited_inventory();
    void on_ok(wxCommandEvent& event);
    void on_sync_from_printer(wxCommandEvent& event);
    void update_clear_enabled(Row& row);
    void refresh_grid();
    void add_row_widgets(size_t slot, wxWindow* parent, wxSizer* target_sizer);
    // Reverts the row to its pre-edit snapshot on Cancel; the widgets write into it live.
    void open_row_editor(size_t slot);
    // nullptr when the row has no resolvable picked preset.
    const Preset* resolved_preset_of(const Row& row) const;

    void reload_rows_from_device();
    // Pads the inventory to m_tool_count on every call.
    FilamentInventory& device();

    std::string               m_printer_preset_name;
    FilamentInventories        m_store;
    size_t                    m_tool_count{0};
    // The bound agent cannot push filament edits; every card is edit-disabled.
    bool                      m_read_only{false};
    wxStaticText*             m_read_only_note{nullptr}; // shown while m_read_only
    // True when m_read_only changed. Rerun after every sync: the agent only knows the
    // changer dialect after a fetch.
    bool                      refresh_read_only();
    bool                      m_sync_available{false};

    // OK pushes only tools whose row differs from the post-sync baseline; no baseline, no push.
    // Tag-locked (NFC) tools are never written.
    struct SlotSnapshot
    {
        std::string picked_preset;
        std::string loaded_type;
        std::string color; // "#RRGGBB" when set, "" when unset
        bool operator==(const SlotSnapshot& o) const
        { return picked_preset == o.picked_preset && loaded_type == o.loaded_type && color == o.color; }
    };
    std::map<size_t, SlotSnapshot> m_synced_baseline;   // tool index -> post-sync row snapshot
    std::set<size_t>               m_tag_locked_tools;  // tool indexes backed by an NFC tag
    // Tools the last sync reported empty; their card is not editable.
    std::set<size_t>               m_empty_on_printer;

    SlotSnapshot snapshot_of(const Row& row) const;
    // interactive=false (auto-sync on open) suppresses the informational dialogs.
    void do_sync_from_printer(bool interactive);
    void push_changes_to_printer();
    std::vector<Row>          m_rows; // one per slot
    SlotGridPanel*            m_grid{nullptr};
    size_t                    m_extruder_count{1}; // nozzles; the grid's extruder row on a MEMM printer
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FilamentInventoryEditor_hpp_
