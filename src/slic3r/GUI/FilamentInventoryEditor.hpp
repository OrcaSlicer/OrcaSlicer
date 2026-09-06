#ifndef slic3r_GUI_FilamentInventoryEditor_hpp_
#define slic3r_GUI_FilamentInventoryEditor_hpp_

#include <map>
#include <set>
#include <string>
#include <vector>

#include <wx/colour.h>
#include <wx/dialog.h>

#include "libslic3r/FilamentInventory.hpp"

class wxColourPickerCtrl;
class wxBoxSizer;
class wxSizer;
class Button;

namespace Slic3r {
class Preset;
namespace GUI {

// "Printer Material Settings" editor: per tool, the currently loaded physical filament (slot 0)
// plus zero or more swappable ones (Slic3r::FilamentInventory, libslic3r/FilamentInventory.hpp),
// so the mapping dialog (FilamentMapDialog) can target a specific physical filament -- not just a
// tool -- when auto-suggesting or letting the user pick a plate-filament assignment.
// Persistence goes through GUI::load_filament_inventories / save_filament_inventories /
// current_inventory_for_preset (FilamentInventoryStore.hpp), which route to the inventory keyed
// by THIS printer preset (see current_inventory_for_preset's doc), and follows load-modify-save: the
// inventory loaded from disk is kept as the working model for the whole session, edited in place,
// and written back unchanged except for the edits the user actually made -- ids of rows the user
// didn't touch never change, since plate mappings reference physical filaments by id across
// sessions.
class PhysicalFilamentComboBox;
class FilamentCard;

class FilamentInventoryEditor : public wxDialog
{
public:
    FilamentInventoryEditor(wxWindow* parent, const std::string& printer_preset_name, size_t tool_count);

private:
    // One physical filament row: the tool's loaded slot (slot 0, always present) or a swappable
    // slot. color_touched/type_touched are tracked independently -- picking only a type must not
    // also persist the untouched picker's neutral placeholder color as if it were a real color
    // pick, and vice versa.
    //
    // is_new is true only for rows created via "Add filament" in this session; such a row's `id`
    // stays 0 until OK, when it is minted from the inventory's next_id allocator. Every row
    // loaded from disk has is_new == false and keeps whatever `id` it already had (including 0
    // for a never-set loaded slot) for the rest of the session, regardless of edits -- ids are
    // never reassigned to a pre-existing row.
    //
    // last_color/picked_preset/loaded_type/color_touched/type_touched are the authoritative,
    // always-current data for the row; color_picker/type_choice/action_btn are non-null only
    // while this row's small editor dialog (opened from its card's pencil icon, see
    // open_row_editor) is on screen -- the card strip itself never keeps row widgets alive, it
    // paints straight from the plain data.
    struct Row
    {
        int                   id{0};
        bool                  is_new{false};
        Slic3r::PhysicalFilament::Kind kind{Slic3r::PhysicalFilament::Kind::Manual};
        bool                  color_touched{false};
        bool                  type_touched{false};
        wxColour              last_color{0xD9, 0xD9, 0xD9};
        std::string           picked_preset; // canonical filament preset name ("" = none)
        // The filament_type as loaded (or as last derived from a resolved preset), used by on_ok
        // to keep the previous type when a newly picked preset's filament_type can't be resolved.
        std::string           loaded_type;
        wxColourPickerCtrl*   color_picker{nullptr};
        PhysicalFilamentComboBox* type_choice{nullptr};
        wxWindow*             action_btn{nullptr}; // Clear (slot 0 only); swap rows have none here
    };

    // A tool's rows (rows[0] = loaded, rows[1..] = swappable) plus the card widgets that render
    // them: main_card shows rows[0] (the editor renders one card per tool; rows beyond 0 are
    // carried as data only and round-trip through save untouched).
    // torn down/rebuilt whenever a row is added or removed (rebuild_tool_rows).
    struct ToolGroup
    {
        std::vector<Row> rows;
        FilamentCard*     main_card{nullptr};
    };

    // Converts one row's live plain data into the PhysicalFilament shape saved to the registry
    // (kind/id passthrough; color only if color_touched; preset+type resolved from picked_preset
    // when type_touched and it still resolves, else the row's loaded_type). Shared by on_ok
    // (every row, including swaps) and do_sync_from_printer's immediate write-through of a synced
    // tool's loaded slot (row 0 only) -- both must agree on exactly what "this row's current
    // state" means as a PhysicalFilament.
    Slic3r::PhysicalFilament slot_from_row(const Row& row) const;
    void on_ok(wxCommandEvent& event);
    void on_sync_from_printer(wxCommandEvent& event);
    void update_clear_enabled(Row& row);
    void rebuild_tool_rows(size_t tool_idx);
    // Builds the color-picker/PhysicalFilamentComboBox/Clear-button row-edit machinery for one
    // row into `parent`/`target_sizer` -- used only by open_row_editor to host it inside that
    // row's small modal editor; the card strip itself never calls this.
    void add_row_widgets(size_t tool_idx, size_t row_idx, wxWindow* parent, wxSizer* target_sizer);
    // Opens the small modal editor (color picker + PhysicalFilamentComboBox + Clear for slot 0)
    // for one row, hosted via add_row_widgets; reverts the row's plain data to its pre-edit
    // snapshot on Cancel, since the row-edit machinery writes live into it as the user interacts.
    void open_row_editor(size_t tool_idx, size_t row_idx);
    // Pushes one row's current plain data onto its card (color/label/lock state) and (re)binds
    // the card's pencil/remove callbacks to this tool/row index.
    void update_card(size_t tool_idx, size_t row_idx, FilamentCard* card);
    // Orca: the row's picked preset, resolved once -- shared by material_type_of, vendor_of,
    // slot_from_row and push_changes_to_printer instead of each independently re-running
    // type_touched/picked_preset/find_preset. nullptr for an unresolved/legacy row (bare
    // loaded_type only, or nothing touched at all).
    const Preset* resolved_preset_of(const Row& row) const;
    // Orca: the row's current color -- the live picker's value while one is open (so an
    // uncommitted pick is seen immediately, matching what the user sees on screen), else the
    // row's last committed last_color. Shared by snapshot_of, slot_from_row and
    // push_changes_to_printer so a save mid-pick can never persist a different color than what
    // was last compared against. Callers still gate on row.color_touched themselves for "has a
    // color actually been set".
    wxColour effective_color_of(const Row& row) const;
    // Card-content helpers for the two-line material cards: material type ("PLA") and
    // vendor/brand ("PolyTerra", the leading token of the resolved preset's display name --
    // same convention as libslic3r's preset_vendor_token). Both return "" for an unresolved/
    // legacy row (bare loaded_type only, or nothing touched at all).
    std::string material_type_of(const Row& row) const;
    std::string vendor_of(const Row& row) const;

    // Reseeds every tool group's row data from m_store's inventory for m_printer_preset_name and
    // rebuilds their widgets. Shared by the ctor and the device-switch/sync rebuild path
    // (via rebuild_tool_rows, not this -- this one is ctor-only now that the printer preset
    // itself can never change mid-dialog), so the seeding logic lives in exactly one place.
    void reload_rows_from_device();
    // The inventory this dialog edits, in m_store, keyed by m_printer_preset_name -- resolved once
    // in the ctor via current_inventory_for_preset, never re-resolved since the printer preset
    // can't change mid-dialog. Pads the inventory to m_tool_count on every call, same as
    // current_inventory_for_preset itself.
    FilamentInventory& device();

    std::string               m_printer_preset_name;
    // Orca: inventory-store routing (see FilamentInventoryStore.hpp). m_store is loaded in the
    // ctor (current_inventory_for_preset) and saved back to by on_ok, keyed throughout by
    // m_printer_preset_name.
    FilamentInventories        m_store;
    size_t                    m_tool_count{0};
    // True when the bound agent cannot deliver edits to the printer (no supports_filament_push)
    // -- every card renders edit-disabled with a tooltip naming why.
    bool                      m_read_only{false};
    bool                      m_sync_available{false};

    // Bidirectional sync state. After a successful read from the printer, each visited tool's
    // slot-0 row snapshot becomes its baseline; OK pushes back only tools whose row DIFFERS
    // from that baseline (no baseline = no connection-known state = no push). Swappable rows
    // (slot 1+) are never pushed. Tag-locked tools (NFC-backed, DevAmsTray::tag_uid) are read
    // but never written -- the tag is authoritative; their card's pencil is disabled.
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
    // Tool indexes whose last sync reported the loaded slot as not present -- there's no
    // filament to set a material/color on, so update_card disables that tool's slot-0 pencil
    // (no editing an empty tool). Repopulated from scratch on every
    // do_sync_from_printer call, so it naturally resets once a later sync reports filament; a
    // tool never covered by any sync never enters this set and stays fully editable (in-session
    // only -- not persisted across dialog restarts).
    std::set<size_t>               m_empty_on_printer;

    SlotSnapshot snapshot_of(const Row& row) const;
    // interactive=false (auto-sync on open) suppresses the informational dialogs.
    void do_sync_from_printer(bool interactive);
    void push_changes_to_printer();
    std::vector<ToolGroup>    m_tools;
    // Compatible filament preset names for the selected printer, in ComboBox order. Combo layout
    int                       m_next_id{1};   // FilamentInventory::next_id at load; advances only at save
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FilamentInventoryEditor_hpp_
