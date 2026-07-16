#ifndef slic3r_GUI_MultiNozzleSync_hpp_
#define slic3r_GUI_MultiNozzleSync_hpp_

// Multi-nozzle GUI.
//
// Two surfaces live here:
//  - The DEVICE-INDEPENDENT "manual nozzle count" surface (ManualNozzleCountDialog + helpers): lets the
//    user declare, per extruder, how many physical nozzles of each volume type a multi-nozzle (H2C)
//    extruder carries. Persisted into the printer preset's `extruder_nozzle_stats` config key, which the
//    multi-nozzle slicer already consumes (ToolOrdering::build_multi_nozzle_group_result).
//  - The DEVICE-SYNC surface (MultiNozzleSyncDialog / HotEndTable / NozzleListTable /
//    tryPopUpMultiNozzleDialog): reads the live nozzle rack from the connected machine via
//    DevNozzleRack / wgtDeviceNozzleRackNozzleItem and lets the user pick a nozzle option to slice with.
//    Popped from the sidebar "sync machine" button when the selected printer is an H2C.
//
// Everything here is inert for existing printers: the manual entry points are gated on the printer having
// any extruder with `extruder_max_nozzle_count > 1` (no shipping single-nozzle/dual-extruder profile sets
// it), and the device-sync path only runs for a connected machine whose rack reports as supported.

#include "../GUI_Utils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "slic3r/GUI/DeviceCore/DevNozzleRack.h"
#include "slic3r/GUI/DeviceTab/wgtDeviceNozzleRackNozzleItem.h"

#include <wx/panel.h>
#include <wx/webview.h>

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class wxChoice;
class wxStaticText;
class wxStaticBitmap;
class Button; // global widget (src/slic3r/GUI/Widgets/Button.hpp), not in the Slic3r::GUI namespace
class Label;  // global widget (src/slic3r/GUI/Widgets/Label.hpp)
class StaticBox;

namespace Slic3r {
class PresetBundle;
class MachineObject;

namespace GUI {

#define ENABLE_MIX_FLOW_PRINT 1

#if ENABLE_MIX_FLOW_PRINT
struct NozzleOption
{
    std::string diameter;
    std::unordered_map<int, std::unordered_map<NozzleVolumeType, int>> extruder_nozzle_stats;
};
#else
struct NozzleOption
{
    std::string diameter;
    std::unordered_map<int, std::pair<NozzleVolumeType, int>> extruder_nozzle_stats;
};
#endif

// Dialog to manually set the per-volume-type physical nozzle count of one multi-nozzle extruder.
class ManualNozzleCountDialog : public DPIDialog
{
public:
    ManualNozzleCountDialog(wxWindow *parent, NozzleVolumeType volume_type, int standard_count, int highflow_count, int max_nozzle_count, bool force_no_zero);
    ~ManualNozzleCountDialog() override = default;
    void on_dpi_changed(const wxRect &suggested_rect) override {}
    int  GetNozzleCount(NozzleVolumeType volume_type) const;

private:
    wxChoice        *m_standard_choice{nullptr};
    wxChoice        *m_highflow_choice{nullptr};
    Button          *m_confirm_btn{nullptr};
    wxStaticText    *m_error_label{nullptr};
};

class ExtruderBadge : public wxPanel
{
public:
    ExtruderBadge(wxWindow* parent);
    void SetExtruderInfo(int extruder_id, const std::string& label, const NozzleVolumeType& flow);
    void UnMarkRelatedItems(const NozzleOption& option);
    void MarkRelatedItems(const NozzleOption& option);
    void SetExtruderValid(bool right_on);
private:
    void SetExtruderStatus(bool left_selected, bool right_selected);

    bool m_right_on{ true };
    wxStaticBitmap* badget;
    Label* left;
    Label* right;
    Label* left_diameter_desp;
    Label* right_diameter_desp;
    Label* left_flow_desp;
    Label* right_flow_desp;

    std::vector<std::string> m_diameter_list;
    std::vector<NozzleVolumeType> m_volume_type_list;
};

class HotEndTable : public wxPanel
{
public:
    HotEndTable(wxWindow* parent);
    void UpdateRackInfo(std::weak_ptr<DevNozzleRack> rack);
    void MarkRelatedItems(const NozzleOption& option);
    void UnMarkRelatedItems(const NozzleOption& option);
private:
    StaticBox* CreateNozzleBox(const std::vector<int>& nozzle_indices);
    void UpdateNozzleItems(const std::unordered_map<int, wgtDeviceNozzleRackNozzleItem*>& nozzle_items,
        std::shared_ptr<DevNozzleRack> nozzle_rack);

private:
    struct HotEndAttr {
        std::string diameter;
        int extruder_id;
        NozzleVolumeType volume_type;
    };

    std::vector<int> FilterHotEnds(const NozzleOption& option);

private:
    StaticBox* m_arow_nozzle_box{ nullptr };
    StaticBox* m_brow_nozzle_box{ nullptr };
    std::unordered_map<int, wgtDeviceNozzleRackNozzleItem*> m_nozzle_items;
    std::weak_ptr<DevNozzleRack> m_nozzle_rack;
    void OnPaint(wxPaintEvent& event);
};


wxDECLARE_EVENT(EVT_NOZZLE_SELECTED, wxCommandEvent);

class NozzleListTable : public wxPanel
{
public:
    NozzleListTable(wxWindow* parent);
    int GetSelectIdx();
    void SetOptions(const std::vector<NozzleOption>& options,int default_select);
private:
    wxString BuildTableObjStr();
    wxString BuildTextObjStr();
    std::vector<NozzleOption> m_nozzle_options;

    void SendSelectionChangedEvent();

    wxWebView* m_web_view;

    int m_selected_idx;
};

class MultiNozzleStatusTable : public wxPanel
{
public:
    MultiNozzleStatusTable(wxWindow* parent);
    void UpdateRackInfo(std::weak_ptr<DevNozzleRack> rack);
    void MarkRelatedItems(const NozzleOption& option);
    void UnMarkRelatedItems(const NozzleOption& option);
private:
    ExtruderBadge* m_badge;
    HotEndTable* m_table;
};


class MultiNozzleSyncDialog : public DPIDialog
{
public:
    MultiNozzleSyncDialog(wxWindow* parent, std::weak_ptr<DevNozzleRack> rack);
    virtual void on_dpi_changed(const wxRect& suggested_rect) {};
    std::vector<NozzleOption> GetNozzleOptions(const std::vector<MultiNozzleUtils::NozzleGroupInfo>& group_infos);

    std::optional<NozzleOption> GetSelectedOption() {
        if (m_nozzle_option_idx < 0 || m_nozzle_option_idx >= m_nozzle_option_values.size())
            return std::nullopt;
        return m_nozzle_option_values[m_nozzle_option_idx];
    }

    int ShowModal() override;
    ~MultiNozzleSyncDialog() override;
private:
    void UpdateRackInfo(std::weak_ptr<DevNozzleRack> rack);

    bool hasMultiDiameters(const std::vector<MultiNozzleUtils::NozzleGroupInfo>& group_infos);
    void OnSelectRadio(int select_idx);

    bool UpdateUi(std::weak_ptr<DevNozzleRack> rack, bool ignore_unknown=false, bool ignore_unreliable=false);

    bool UpdateOptionList(std::weak_ptr<DevNozzleRack> rack, bool ignore_unknown, bool ignore_unreliable);
    void UpdateTip(std::weak_ptr<DevNozzleRack> rack, bool ignore_unknown, bool ignore_unreliable);
    void UpdateButton(std::weak_ptr<DevNozzleRack> rack, bool ignore_unknown, bool ignore_unreliable);
    void OnRackStatusReadingFinished(wxEvent& evt);
    void OnRefreshTimer(wxTimerEvent& event);

private:
    MultiNozzleStatusTable* m_nozzle_table;
    NozzleListTable* m_list_table;
    std::vector<NozzleOption> m_nozzle_option_values;
    int m_nozzle_option_idx{ -1 };
    bool m_refreshing{ false };

    std::weak_ptr<DevNozzleRack> m_nozzle_rack;
    Label* m_tips;
    Label* m_caution;

    wxTimer* m_refresh_timer {nullptr};
    size_t m_rack_event_token;
    Button* m_cancel_btn;
    Button* m_confirm_btn;
};


// Entry point for the sidebar "sync machine" button: pops MultiNozzleSyncDialog for a connected H2C and
// returns the chosen nozzle option (nullopt if the machine has no supported rack or the user cancels).
std::optional<NozzleOption> tryPopUpMultiNozzleDialog(MachineObject* obj);

// Persist one extruder's per-volume-type nozzle count into the edited printer preset's `extruder_nozzle_stats`
// config key (the value the multi-nozzle slicer reads). When clear_all is true, the extruder's other volume-type
// counts are reset first.
void setExtruderNozzleCount(PresetBundle *preset_bundle, int extruder_id, NozzleVolumeType type, int nozzle_count, bool clear_all);

// Read one extruder's nozzle count for a volume type (or its total) from the edited printer preset's
// `extruder_nozzle_stats` config key. Returns 0 when the stats are absent or the extruder is unknown.
int getExtruderNozzleCount(PresetBundle *preset_bundle, int extruder_id, NozzleVolumeType volume_type);
int getExtruderNozzleCountTotal(PresetBundle *preset_bundle, int extruder_id);

// Refresh the sidebar nozzle-count badge for one extruder: shows the extruder's physical nozzle count on
// multi-nozzle printers, hides it (count -1) everywhere else.
void updateNozzleCountDisplay(PresetBundle *preset_bundle, int extruder_id, NozzleVolumeType volume_type);

// Reset `extruder_nozzle_stats` to its baseline for the selected printer: each extruder gets
// extruder_max_nozzle_count nozzles of its currently selected volume type. Called whenever the stats are
// absent (the key is session-only — preset switches rebuild the edited config without it). Clears the
// machine-provenance flag.
void seedExtruderNozzleStats(PresetBundle *preset_bundle);

// React to the user switching one extruder's nozzle volume type: carry the extruder's total nozzle count
// over to the new type. No-op for Hybrid (which is a mix, not a type all nozzles share) and for
// machine-synced stats (the device-reported per-type breakdown must survive a flow switch).
void onNozzleVolumeTypeSwitch(PresetBundle *preset_bundle, int extruder_id, NozzleVolumeType type);

// Mark the current `extruder_nozzle_stats` values as machine-reported (device sync) or manual/seeded.
void setNozzleStatsFromMachine(bool from_machine);

// True when a nozzle of this diameter can carry the "Direct Drive TPU High Flow" variant. That variant
// exists on the 0.4 and 0.6 nozzle H2D/H2D Pro leaves (not 0.2/0.8), so this is the single source of truth
// for the diameter set — the counterpart of the High-Flow-skip-for-0.2 guard.
bool nozzle_diameter_supports_tpu_high_flow(double nozzle_diameter);
// Same predicate resolved from the edited printer preset's nozzle_diameter for one extruder. False when unknown.
bool extruder_supports_tpu_high_flow(PresetBundle *preset_bundle, int extruder_id);

// Entry point: pop the ManualNozzleCountDialog for one extruder of a multi-nozzle printer and persist the result.
// No-op unless the selected printer has an extruder with extruder_max_nozzle_count > 1.
void manuallySetNozzleCount(int extruder_id);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_MultiNozzleSync_hpp_
