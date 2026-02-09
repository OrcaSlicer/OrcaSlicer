//**********************************************************/
/* File: wgtDeviceNozzleSelect.h
*  Description: The panel to select nozzle
*
*  \n class wgtDeviceNozzleSelect;
//**********************************************************/

#pragma once

#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"

#include <wx/panel.h>

#include <memory>

// Previous definitions
namespace Slic3r
{
    struct DevNozzle;
    class DevNozzleRack;
namespace GUI
{
    class wgtDeviceNozzleRackNozzleItem;
}
};

wxDECLARE_EVENT(EVT_NOZZLE_RACK_ITEM_CLICKED, wxCommandEvent);
namespace Slic3r::GUI
{
class wgtDeviceNozzleRackSelect : public wxPanel
{
public:
    wgtDeviceNozzleRackSelect(wxWindow* parent);
    ~wgtDeviceNozzleRackSelect() = default;

public:
    int  GetSelectedNozzlePosID() const { return m_selected_nozzle.GetNozzlePosId();}
    void UpdateRackSelect(std::shared_ptr<DevNozzleRack> rack, int selected_nozzle_pos_id = -1);
    void Rescale();

private:
    void CreateGui();

    void ClearSelection();
    void SetSelectedNozzle(const DevNozzle &nozzle);

    void OnNozzleItemSelected(wxCommandEvent& evt);

private:
    DevNozzle                    m_selected_nozzle;
    std::weak_ptr<DevNozzleRack> m_nozzle_rack;

    // GUI
    wgtDeviceNozzleRackNozzleItem * m_toolhead_nozzle{nullptr};
    std::unordered_map<int, wgtDeviceNozzleRackNozzleItem *> m_nozzle_items; // from 0 to 5
};

};// end of namespace Slic3r::GUI