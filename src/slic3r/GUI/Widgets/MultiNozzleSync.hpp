#ifndef MULTI_NOZZLE_SYNC_HPP
#define MULTI_NOZZLE_SYNC_HPP

#include "../wxExtensions.hpp"
#include "../MsgDialog.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "slic3r/GUI/DeviceCore/DevNozzleRack.h"
#include "slic3r/GUI/DeviceTab/wgtDeviceNozzleRack.h"
#include "slic3r/GUI/Widgets/RadioBox.hpp"
#include <wx/webview.h>

#define ENABLE_MIX_FLOW_PRINT 1

namespace Slic3r {
    class PresetBundle;
}

namespace Slic3r::GUI {

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

class ManualNozzleCountDialog : public DPIDialog
{
public:
    ManualNozzleCountDialog(wxWindow *parent, NozzleVolumeType volume_type, int standard_count, int highflow_count,int max_nozzle_count, bool force_no_zero);
    ~ManualNozzleCountDialog() {};
    virtual void on_dpi_changed(const wxRect& suggested_rect) {};
    int GetNozzleCount(NozzleVolumeType volume_type) const;
private:
    wxChoice* m_standard_choice { nullptr };
    wxChoice* m_highflow_choice { nullptr };
    Button* m_confirm_btn{ nullptr };
    Label* m_error_label{ nullptr };
    NozzleVolumeType m_volume_type;
};


class ExtruderBadge :public wxPanel
{
public:
    ExtruderBadge(wxWindow* parent);
    void SetExtruderInfo(int extruder_id, const string& label, const NozzleVolumeType& flow);
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

class HotEndTable :public wxPanel
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


std::optional<NozzleOption> tryPopUpMultiNozzleDialog(MachineObject* obj);

void setExtruderNozzleCount(PresetBundle* preset_bundle, int extruder_id, NozzleVolumeType type, int nozzle_count, bool clear_before_set);

void updateNozzleCountDisplay(PresetBundle* preset_bundle, int extruder_id, NozzleVolumeType volume_type);

void manuallySetNozzleCount(int extruder_id);

} // namespace Slic3r

#endif