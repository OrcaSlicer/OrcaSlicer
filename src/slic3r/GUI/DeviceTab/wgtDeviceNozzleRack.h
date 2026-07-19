//**********************************************************/
/* File: wgtDeviceNozzleRack.h
*  Description: The Device-tab panel with the toolhead nozzle and the H2C induction hotend rack.
*
*  \n class wgtDeviceNozzleRack;      // toolhead panel + rack area, side by side
*  \n class wgtDeviceNozzleRackToolHead;
*  \n class wgtDeviceNozzleRackArea;
*  \n class wgtDeviceNozzleRackPos;
*
*  The single-nozzle tile class wgtDeviceNozzleRackNozzleItem lives in its own translation unit
*  (wgtDeviceNozzleRackNozzleItem.{h,cpp}), so it is included here rather than redeclared.
*  The Hotends-Info upgrade dialog (wgtDeviceNozzleRackUpgradeDlg) is intentionally not wired yet
*  (see the TODO markers in the .cpp).
//**********************************************************/

#pragma once
#include "slic3r/GUI/DeviceCore/DevNozzleRack.h"

#include "wgtDeviceNozzleRackNozzleItem.h"

#include "slic3r/GUI/Widgets/StaticBox.hpp"
#include "slic3r/GUI/Widgets/AnimaController.hpp"

#include <wx/panel.h>
#include <wx/simplebook.h>
#include <memory>
#include <unordered_map>
#include <vector>

// Previous definitions
class Button;
class Label;
class ScalableBitmap;
class ScalableButton;
namespace Slic3r
{
    struct DevNozzle;
    class DevNozzleRack;
namespace GUI
{
    class wgtDeviceNozzleRackArea;
    class wgtDeviceNozzleRackNozzleItem;
    class wgtDeviceNozzleRackToolHead;
    class wgtDeviceNozzleRackPos;
    class wgtDeviceNozzleRackTitle;
    class wgtDeviceNozzleRackUpgradeDlg;
}
};

namespace Slic3r::GUI
{
class wgtDeviceNozzleRack : public wxPanel
{
public:
    wgtDeviceNozzleRack(wxWindow* parent,
                        wxWindowID id = wxID_ANY,
                        const wxPoint& pos = wxDefaultPosition,
                        const wxSize& size = wxDefaultSize,
                        long style = wxTAB_TRAVERSAL);
    ~wgtDeviceNozzleRack() = default;

public:
    void UpdateRackInfo(std::shared_ptr<DevNozzleRack> rack);
    void Rescale();

private:
    void CreateGui();

private:
    std::weak_ptr<DevNozzleRack> m_nozzle_rack;

    // GUI
    wgtDeviceNozzleRackToolHead* m_toolhead_panel{ nullptr };
    wgtDeviceNozzleRackArea* m_rack_area{ nullptr };
};


class wgtDeviceNozzleRackToolHead : public wxPanel
{
public:
    wgtDeviceNozzleRackToolHead(wxWindow* parent) : wxPanel(parent) { CreateGui();}

public:
    void UpdateToolHeadInfo(const DevNozzle& extruder_nozzle);
    void Rescale();

private:
    void CreateGui();

private:
    bool m_extruder_nozzle_exist = false;
    std::string m_filament_color;

    // GUI
    ScalableBitmap* m_extruder_nozzle_normal = nullptr;
    ScalableBitmap* m_extruder_nozzle_empty = nullptr;
    wxStaticBitmap* m_toolhead_icon;

    Label*  m_nozzle_diamenter_label;
    Label*  m_nozzle_flowtype_label;
};


class wgtDeviceNozzleRackArea : public wxPanel
{
public:
    wgtDeviceNozzleRackArea(wxWindow* parent) : wxPanel(parent) { CreateGui();}

public:
    void UpdateRackInfo(std::weak_ptr<DevNozzleRack> rack);
    void Rescale();

private:
    void CreateGui();
    StaticBox* CreateNozzleBox(const std::vector<int> nozzle_idxes);
    wxSizer* CreateRefreshBook(wxPanel* parent);

    // updates
    void UpdateNozzleItems(const std::unordered_map<int, wgtDeviceNozzleRackNozzleItem*>& nozzle_items,
        std::shared_ptr<DevNozzleRack> nozzle_rack);

    // events
    void OnBtnHotendsInfos(wxCommandEvent& evt);
    void OnBtnReadAll(wxCommandEvent& evt);

private:
    std::weak_ptr<DevNozzleRack> m_nozzle_rack;
    DevNozzleRack::RackPos m_rack_pos = DevNozzleRack::RACK_POS_UNKNOWN;
    DevNozzleRack::RackStatus m_rack_status = DevNozzleRack::RACK_STATUS_UNKNOWN;

    // GUI
    wxSimplebook* m_simple_book{ nullptr };
    wxPanel* m_panel_content{ nullptr };
    wxPanel* m_panel_refresh{ nullptr };

    wgtDeviceNozzleRackTitle* m_title_nozzle_rack;
    wxBoxSizer* m_hotends_sizer;
    StaticBox* m_arow_nozzles_box;
    StaticBox* m_brow_nozzles_box;
    std::unordered_map<int, wgtDeviceNozzleRackNozzleItem*> m_nozzle_items;

    wgtDeviceNozzleRackPos* m_rack_pos_panel;

    Button* m_btn_hotends_infos;
    Button* m_btn_read_all;

    /* refresh book */
    Label* m_progress_refresh{ nullptr };
    AnimaIcon* m_refresh_icon{ nullptr };

    // "Hotends Info" upgrade dialog. Owned for its ShowModal lifetime by
    // OnBtnHotendsInfos; UpdateRackInfo forwards live device pushes to it while it is shown.
    wgtDeviceNozzleRackUpgradeDlg* m_rack_upgrade_dlg = nullptr;
};

class wgtDeviceNozzleRackPos : public wxPanel
{
public:
    explicit wgtDeviceNozzleRackPos(wxWindow* parent) : wxPanel(parent) { CreateGui();}

public:
    void UpdateRackPos(const std::shared_ptr<DevNozzleRack>& rack);
    void Rescale();

private:
    void CreateGui();

    void UpdateRackPos(DevNozzleRack::RackPos new_pos,
                       DevNozzleRack::RackStatus new_status,
                       bool is_reading);

    // events
    void OnMoveRackUp(wxCommandEvent& evt);
    void OnMoveRackDown(wxCommandEvent& evt);
    void OnBtnHomingRack(wxCommandEvent& evt);

private:
    std::weak_ptr<DevNozzleRack> m_rack;
    DevNozzleRack::RackPos m_rack_pos = DevNozzleRack::RACK_POS_UNKNOWN;
    DevNozzleRack::RackStatus m_rack_status = DevNozzleRack::RACK_STATUS_UNKNOWN;

    // GUI
    StaticBox* m_rowup_panel;
    ScalableButton* m_btn_rowup;
    Label* m_label_rowup_status{ nullptr };
    Label* m_label_rowup{ nullptr };

    StaticBox* m_rowbottom_panel;
    ScalableButton* m_btn_rowbottom_up;
    Label* m_label_rowbottom_status{ nullptr };
    Label* m_label_rowbottom{ nullptr };

    ScalableButton* m_btn_homing{ nullptr };
};

};// end of namespace Slic3r::GUI
