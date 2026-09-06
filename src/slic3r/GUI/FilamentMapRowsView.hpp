#ifndef slic3r_FilamentMapRowsView_hpp_
#define slic3r_FilamentMapRowsView_hpp_

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <optional>

#include <wx/dialog.h>
#include <wx/panel.h>

#include "libslic3r/FilamentInventory.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"

class Button;
class CheckBox;
class wxBoxSizer;

namespace Slic3r {
namespace GUI {

class FilamentMapRowsPanel;
class ThumbnailPanel;

// Orca: the whole rows-mode mapping experience as an embeddable panel -- one row per plate
// filament targeting a physical filament, the "remember these as the loaded filaments" offer,
// the merged-profile mismatch warning, and the recolored plate preview.
//
// This is ours, not an extension of upstream's FilamentMapDialog: that dialog is BBL's dual-nozzle
// filament-grouping UI and stays exactly as upstream ships it. Everything device-owned mapping
// needs -- the proposal, bootstrap handling, inventory backfill, mismatch warning and recolored
// preview -- lives here, so the send dialog can embed it and a plain modal (FilamentMapRowsDialog,
// below) can host the same widget for callers that want it on its own.
//
// The host owns its own Reset / Automatic / OK buttons (so each keeps its native button row) and
// drives them through ResetToInitial(), ApplyAutomatic() and the on_state_changed callback.
class FilamentMapRowsView : public wxPanel
{
public:
    // filament_color/filament_type/filament_names: full project lists, 0-based by filament id.
    // filament_map: full-length 1-based stored filament->tool map; only the base-map passthrough
    //   for filaments not on this plate (and a bootstrap row's tool -- see FilamentMapRowsPanel).
    // physical_filament_map: full-length stored filament->physical-filament-id map (0 = unassigned),
    //   consulted only when filament_map_confirmed is true.
    // plate_filaments: 1-based used filament ids, in plate order.
    // on_state_changed: called whenever the rows change, with "every row has a target". Hosts gate
    //   their OK/Print button on it, and should re-layout: this panel's best size changes with it.
    FilamentMapRowsView(wxWindow*                       parent,
                        const std::vector<std::string>& filament_color,
                        const std::vector<std::string>& filament_type,
                        const std::vector<std::string>& filament_names,
                        const std::vector<int>&         filament_map,
                        const std::vector<int>&         physical_filament_map,
                        const std::vector<int>&         plate_filaments,
                        const FilamentInventory&        inventory,
                        size_t                          tool_count,
                        const std::string&              printer_preset_name,
                        bool                            filament_map_confirmed,
                        std::function<void(bool)>       on_state_changed);

    // Reads the rows into the maps below and, when the user accepted the offer, records the picks
    // as the printer's loaded filaments. Call once, when the host's OK is accepted.
    void Commit();

    const std::vector<int>& GetFilamentMaps() const { return m_filament_map; }
    const std::vector<int>& GetPhysicalMaps() const { return m_physical_filament_map; }

    bool AllRowsAssigned() const;

    // True while "Automatic" is armed with no manual pick since: a host that persists a mapping
    // onto a plate should save it in live (re-mapped every slice) mode rather than freezing it.

    // Restores the rows to the proposal this view was built with, badges included.
    void ResetToInitial();
    // Fills the rows with a freshly computed auto-match, as a never-confirmed plate would get.
    void ApplyAutomatic();

private:
    void rebuild_rows_panel(const std::vector<int>& proposal, bool auto_matched);
    void repin_rows_min_height();
    void seed_status();
    void update_mismatch_warning();
    void update_preview();

    std::map<int, std::string> build_slot_preset_names() const;
    FilamentInventory&         device();

    FilamentMapRowsPanel* m_rows_panel{nullptr};
    wxBoxSizer*           m_rows_holder_sizer{nullptr};
    CheckBox*             m_record_checkbox{nullptr};
    wxWindow*             m_record_row{nullptr};
    ThumbnailPanel*       m_preview_panel{nullptr};

    // Construction-time COPIES of the current plate's lit / no-light renders: this view never
    // triggers a render of its own (that caused a GLX BadMatch from a modal dialog) and the
    // plate's own thumbnails are invalidated while a dialog is up.
    ThumbnailData m_preview_lit;
    ThumbnailData m_preview_no_light;

    std::vector<int> m_initial_proposal;
    bool             m_initial_proposal_was_auto{false};
    bool             m_inventory_was_all_unset{false};
    bool             m_live_auto_pending{false};

    std::vector<std::string> m_filament_names;
    std::vector<int>         m_plate_filaments;
    FilamentInventory        m_inventory;
    size_t                   m_tool_count{0};
    std::string              m_printer_preset_name;
    FilamentInventories      m_store;

    std::vector<int>         m_filament_map;
    std::vector<int>         m_physical_filament_map;
    std::vector<std::string> m_filament_color;
    std::vector<std::string> m_filament_type;
    std::vector<std::string> m_filament_vendor;
    std::vector<std::string> m_filament_preset;

    std::function<void(bool)> m_on_state_changed;
};

// Orca: the modal wrapper around FilamentMapRowsView, for callers that want the mapping on its
// own rather than embedded in a larger dialog (the agent send path). The send dialog
// (DevicePrintOptionsDialog) embeds the view directly instead of opening this.
class FilamentMapRowsDialog : public wxDialog
{
public:
    FilamentMapRowsDialog(wxWindow*                       parent,
                          const std::vector<std::string>& filament_color,
                          const std::vector<std::string>& filament_type,
                          const std::vector<std::string>& filament_names,
                          const std::vector<int>&         filament_map,
                          const std::vector<int>&         physical_filament_map,
                          const std::vector<int>&         plate_filaments,
                          const FilamentInventory&        inventory,
                          size_t                          tool_count,
                          const std::string&              printer_preset_name,
                          bool                            filament_map_confirmed,
                          const wxString&                 title);

    // 1-based tool per project filament, valid after the dialog returns wxID_OK.
    const std::vector<int>& GetFilamentMaps() const { return m_view->GetFilamentMaps(); }

private:
    FilamentMapRowsView* m_view{nullptr};
    Button*              m_ok_btn{nullptr};
};

// Orca: send-time mapping for device-owned-protocol printers -- shows the rows with no PartPlate
// to read a stored map from or persist a result onto (stored maps ignored; every row proposed
// straight from the inventory auto-match). Callers keep their own protocol gate and their own
// destination for the result. Returns std::nullopt when the user cancelled, which callers must
// treat as "abort the send": their own protocol check already established a mapping IS needed.
std::optional<std::vector<int>> collect_device_map_table_for_send(wxWindow* parent, const std::vector<int>& plate_filaments, const wxString& title);

}} // namespace Slic3r::GUI

#endif /* slic3r_FilamentMapRowsView_hpp_ */
