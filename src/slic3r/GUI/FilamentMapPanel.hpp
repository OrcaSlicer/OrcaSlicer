
#ifndef FILAMENT_MAP_PANEL_HPP
#define FILAMENT_MAP_PANEL_HPP

#include "GUI.hpp"
#include "DragDropPanel.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Label.hpp"
#include <wx/event.h>
#include <wx/panel.h>
#include <map>
#include <optional>
#include <vector>
#include <string>
#include <wx/timer.h>
#include <wx/string.h>
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
class PresetBundle;

namespace GUI {

// The nozzles a printer's extruders carry, per extruder: the most nozzles it can hold (the printer's
// extruder_max_nozzle_count), its nozzle volume type (the project's nozzle_volume_type) and how many nozzles of each
// volume type it has (the printer's extruder_nozzle_stats, none of a type it lacks).
struct NozzleInventory
{
    std::vector<int>                             max_nozzle_counts;
    std::vector<int>                             volume_types;
    std::vector<std::map<NozzleVolumeType, int>> nozzle_counts;
};

// The inventory of the printer being edited and the project.
NozzleInventory nozzle_inventory(const PresetBundle& preset_bundle);

// Whether one of the extruders can hold several nozzles. Only such an extruder keeps an inventory worth checking: a
// plain multi-extruder printer has none, and checking it would refuse every grouping whenever its counts are stale.
bool has_multi_nozzle_extruder(const std::vector<int>& max_nozzle_counts);

// An extruder (0-based) that a manual filament grouping gives filaments and that has no nozzle of the volume type
// they need.
struct NozzleShortage
{
    int              extruder    = -1;
    NozzleVolumeType volume_type = nvtStandard;
};

// The check the filament grouping dialog's Custom page makes before its OK can be pressed: the first extruder that
// `filament_map` (the 1-based extruder of each filament) gives one of `filaments` (the 1-based filaments the plate
// prints) and that has no nozzle of the volume type it needs. That type is the extruder's own, or on a Hybrid extruder
// each of the types `filament_volume_map` gives its filaments (Standard checked before High Flow). None on a printer
// with no multi-nozzle extruder.
std::optional<NozzleShortage> manual_map_nozzle_shortage(const NozzleInventory&  inventory,
                                                         const std::vector<int>& filaments,
                                                         const std::vector<int>& filament_map,
                                                         const std::vector<int>& filament_volume_map);

// Fired by the manual panel's validation timer: the event int is 1 when the current manual
// grouping is printable with the installed nozzles, 0 when some zone has no matching nozzle.
wxDECLARE_EVENT(wxEVT_INVALID_MANUAL_MAP, wxCommandEvent);

class FilamentMapManualPanel : public wxPanel
{
public:
    FilamentMapManualPanel(wxWindow                       *parent,
                           const std::vector<std::string> &color,
                           const std::vector<std::string> &type,
                           const std::vector<int>         &filament_list,
                           const std::vector<int>         &filament_map,
                           const std::vector<int>         &filament_volume_map);
    ~FilamentMapManualPanel();

    std::vector<int> GetFilamentMaps() const;
    std::vector<int> GetFilamentVolumeMaps() const;
    std::vector<int> GetLeftFilaments() const { return m_left_panel->GetAllFilaments(); }
    std::vector<int> GetRightFilaments() const { return m_right_panel->GetAllFilaments(); }

    std::vector<int> GetRightHighFlowFilaments() const { return m_right_panel->GetHighFlowFilaments(); }
    std::vector<int> GetRightStandardFilaments() const { return m_right_panel->GetStandardFilaments(); }
    std::vector<int> GetRightTPUHighFlowFilaments() const { return m_right_panel->GetTPUHighFlowFilaments(); }
    void UpdateNozzleVolumeType();
    void UpdateNozzleCountDisplay();

    bool Show(bool show = true) override;

private:
    void OnTimer(wxTimerEvent &evt);
    void OnSwitchFilament(wxCommandEvent &);
    void SyncPanelHeights();
    void OnDragDropCompleted(wxCommandEvent &evt);
    void OnSuggestionClicked(wxCommandEvent &event);

    DragDropPanel          *m_left_panel;
    SeparatedDragDropPanel *m_right_panel;

    Label   *m_description;
    Label   *m_tips;
    Label   *m_errors;
    wxPanel *m_suggestion_panel;

    ScalableButton *m_switch_btn;

    std::vector<int>         m_filament_map;
    std::vector<int>         m_filament_volume_map;
    std::vector<int>         m_filament_list;
    std::vector<std::string> m_filament_color;
    std::vector<std::string> m_filament_type;

    wxTimer *m_timer;
    int      m_invalid_id{-1};
    bool     m_force_validation{false};
};

class FilamentMapBtnPanel : public wxPanel
{
public:
    FilamentMapBtnPanel(wxWindow *parent, const wxString &label, const wxString &detail, const std::string &icon_path);
    bool Show(bool show = true) override;
    void Select(bool selected);
    bool Enable(bool enable) override;
    bool IsEnabled() const { return m_enabled; }
protected:
    void OnPaint(wxPaintEvent &event);
private:
    void OnEnterWindow(wxMouseEvent &event);
    void OnLeaveWindow(wxMouseEvent &evnet);

    void UpdateStatus();

    wxBitmap icon_enabled;
    wxBitmap icon_disabled;

    wxBitmapButton *m_btn;
    wxStaticText   *m_label;
    Label          *m_disable_tip;
    Label          *m_detail;
    std::string     m_icon_path;
    bool            m_enabled{ true };
    bool            m_hover{false};
    bool            m_selected{false};
};

class FilamentMapAutoPanel : public wxPanel
{
public:
    FilamentMapAutoPanel(wxWindow *parent, FilamentMapMode mode, bool machine_synced);
    bool Show(bool show = true) override;
    FilamentMapMode GetMode() const { return m_mode; }

private:
    void OnModeSwitch(FilamentMapMode mode);
    void UpdateStatus();

    FilamentMapBtnPanel *m_flush_panel;
    FilamentMapBtnPanel *m_match_panel;
    FilamentMapMode      m_mode;
};

class FilamentMapDefaultPanel : public wxPanel
{
public:
    FilamentMapDefaultPanel(wxWindow *parent);
    bool Show(bool show = true) override;

private:
    Label *m_label;
};
} // namespace GUI
} // namespace Slic3r

#endif