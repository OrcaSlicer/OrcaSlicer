//**********************************************************/
/* File: wgtDeviceNozzleRackUpdate.h
*  Description: The panel for updating hotends
*
*  \n class wgtDeviceNozzleRackUpdate
//**********************************************************/

#pragma once
#include "slic3r/GUI/DeviceCore/DevNozzleRack.h"

#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/Widgets/StaticBox.hpp"

#include <wx/panel.h>
#include <memory>

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
    class wgtDeviceNozzleRackUprade;
    class wgtDeviceNozzleRackHotendUpdate;
}
};

namespace Slic3r::GUI
{
class wgtDeviceNozzleRackUpgradeDlg : public DPIDialog
{
public:
    wgtDeviceNozzleRackUpgradeDlg(wxWindow* parent, const std::shared_ptr<DevNozzleRack> rack);

public:
    void UpdateRackInfo(const std::shared_ptr<DevNozzleRack> rack);;

public:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    wgtDeviceNozzleRackUprade* m_rack_upgrade_panel;
};


class wgtDeviceNozzleRackUprade : public wxPanel
{
public:
    wgtDeviceNozzleRackUprade(wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);
    ~wgtDeviceNozzleRackUprade() = default;

public:
    void UpdateRackInfo(const std::shared_ptr<DevNozzleRack> rack);
    void Rescale();

private:
    void CreateGui();

    void OnBtnReadAll(wxCommandEvent& e);

private:
    std::weak_ptr<DevNozzleRack> m_nozzle_rack;

    // GUI
    wgtDeviceNozzleRackHotendUpdate* m_extruder_nozzle_item;
    std::unordered_map<int, wgtDeviceNozzleRackHotendUpdate*> m_nozzle_items;
};

// Using for rack nozzle id or extruder nozzle id
class wgtDeviceNozzleRackHotendUpdate : public StaticBox
{
public:
    wgtDeviceNozzleRackHotendUpdate(wxWindow* parent, const wxString& idx_text);

public:
    // Color
    void UpdateColourStyle(const wxColour& clr);

    // extruder nozzle
    int GetExtruderNozzleId() const { return m_ext_nozzle_id; }
    void SetExtruderNozzleId(int ext_nozzle_id) { m_ext_nozzle_id = ext_nozzle_id; }
    void UpdateExtruderNozzleInfo(const std::shared_ptr<DevNozzleRack> rack);

    // rack nozzle
    void SetRackNozzleId(int rack_nozzle_id) { m_rack_nozzle_id = rack_nozzle_id; }
    int GetRackNozzleId() const { return m_rack_nozzle_id; }
    void UpdateRackNozzleInfo(const std::shared_ptr<DevNozzleRack> rack);

    // gui
    void Rescale();

private:
    void CreateGui();

    void UpdateInfo(const DevNozzle& nozzle);

private:
    enum NozzleStatus : int
    {
        NOZZLE_STATUS_DC = -1,
        NOZZLE_STATUS_EMPTY,
        NOZZLE_STATUS_NORMAL,
        NOZZLE_STATUS_ABNORMAL,
        NOZZLE_STATUS_UNKNOWN,
    };

private:
    int m_ext_nozzle_id = -1;
    int m_rack_nozzle_id = -1;

    NozzleStatus m_nozzle_status = NOZZLE_STATUS_DC;
    std::weak_ptr<DevNozzleRack> m_nozzle_rack;

    // GUI
    ScalableBitmap* m_nozzle_image = nullptr;
    ScalableBitmap* m_nozzle_empty_image = nullptr;

    Label* m_idx_label;
    wxStaticBitmap* m_icon_bitmap{ nullptr };

    Label* m_diameter_label;
    Label* m_flowtype_label;
    Label* m_type_label;
    ScalableButton* m_error_button{ nullptr };

    Label* m_sn_label;
    Label* m_version_label;
    Label* m_version_new_label;
};



};// end of namespace Slic3r::GUI