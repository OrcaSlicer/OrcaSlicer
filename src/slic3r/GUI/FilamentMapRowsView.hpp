#ifndef slic3r_FilamentMapRowsView_hpp_
#define slic3r_FilamentMapRowsView_hpp_

#include <functional>
#include <string>
#include <vector>

#include <optional>

#include <wx/dialog.h>
#include <wx/panel.h>

#include "libslic3r/FilamentCompaction.hpp"
#include "libslic3r/FilamentInventory.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include <cstddef>
#include <wx/string.h>

class Button;
class CheckBox;

namespace Slic3r {
namespace GUI {

class FilamentMapRowsPanel;
class ThumbnailPanel;

// Orca: plate-filament -> physical-filament mapping rows with an Automatic button, the "remember as
// loaded filaments" offer, the mismatch warning and a recolored plate preview. Separate from
// FilamentMapDialog, which is BBL's dual-nozzle grouping UI. The host owns the OK button.
class FilamentMapRowsView : public wxPanel
{
public:
    // plate_filaments: 1-based ids. on_state_changed(all rows assigned) fires on every change; the
    // best size changes with it.
    FilamentMapRowsView(wxWindow*                 parent,
                        const std::vector<int>&   plate_filaments,
                        const FilamentInventory&  inventory,
                        size_t                    tool_count,
                        std::function<void(bool)> on_state_changed);

    // Call once, on OK; also records the picks as loaded filaments if the offer was accepted.
    void Commit();

    // Full-length, 1-based tools; 0 for filaments not on the plate. Valid after Commit().
    const std::vector<int>& GetFilamentMaps() const { return m_filament_map; }

    bool AllRowsAssigned() const;

private:
    void apply_automatic();
    void repin_rows_min_height();
    void seed_status();
    void update_mismatch_warning();
    void update_preview();

    FilamentMapRowsPanel* m_rows_panel{nullptr};
    CheckBox*             m_record_checkbox{nullptr};
    wxWindow*             m_record_row{nullptr};
    ThumbnailPanel*       m_preview_panel{nullptr};

    // Copies taken at construction: rendering from a modal dialog causes a GLX BadMatch, and the
    // plate's thumbnails are invalidated while a dialog is up.
    ThumbnailData m_preview_lit;
    ThumbnailData m_preview_no_light;

    bool m_inventory_was_all_unset{false};

    std::vector<int>         m_plate_filaments;
    FilamentInventory        m_inventory;
    size_t                   m_tool_count{0};

    std::vector<int>         m_filament_map;
    std::vector<std::string> m_filament_color;
    std::vector<std::string> m_filament_type;
    std::vector<std::string> m_filament_names;
    std::vector<std::string> m_filament_vendor;
    std::vector<std::string> m_filament_preset;

    std::function<void(bool)> m_on_state_changed;
};

// Orca: FilamentMapRowsView in a modal.
class FilamentMapRowsDialog : public wxDialog
{
public:
    FilamentMapRowsDialog(wxWindow*                parent,
                          const std::vector<int>&  plate_filaments,
                          const FilamentInventory& inventory,
                          size_t                   tool_count,
                          const wxString&          title);

    // Valid after wxID_OK.
    const std::vector<int>& GetFilamentMaps() const { return m_view->GetFilamentMaps(); }

private:
    FilamentMapRowsView* m_view{nullptr};
    Button*              m_ok_btn{nullptr};
};

// Orca: send-time mapping proposed from the inventory alone, ignoring stored maps.
// std::nullopt means the user cancelled and the send must abort.
std::optional<std::vector<int>> collect_device_map_table_for_send(wxWindow* parent, const std::vector<int>& plate_filaments, const wxString& title);

// Orca: re-keys per-project-slot picks onto the g-code's emitted tool numbers, using the
// compaction the plate was sliced with. Only tools up to the highest one the plate prints are keyed.
std::vector<int> device_wire_filament_map(const FilamentCompaction& compaction, const std::vector<int>& filaments_1based,
                                          const std::vector<int>& map_1based);

}} // namespace Slic3r::GUI

#endif /* slic3r_FilamentMapRowsView_hpp_ */
