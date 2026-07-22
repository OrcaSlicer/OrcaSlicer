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
class Label;
namespace Slic3r
{
    struct DevNozzle;
    class DevNozzleRack;
namespace GUI
{
    class wgtDeviceNozzleRackNozzleItem;
    class wgtMsgBox;
}
};

wxDECLARE_EVENT(EVT_NOZZLE_SELECT_CHANGED, wxCommandEvent);
wxDECLARE_EVENT(EVT_NOZZLE_SELECT_CLICKED, wxCommandEvent);
namespace Slic3r::GUI
{
class wgtDeviceNozzleRackSelect : public wxPanel
{
public:
    wgtDeviceNozzleRackSelect(wxWindow* parent);
    ~wgtDeviceNozzleRackSelect() = default;

public:
    int  GetSelectedNozzlePosID() const { return m_selected_nozzle.GetNozzlePosId();}
    void UpdatSelectedNozzles(std::shared_ptr<DevNozzleRack> rack, std::vector<int> selected_nozzle_pos_vec, bool use_dynamic_switch, std::optional<PrintFromType> print_from_type);// for slicing with dynamic switch
    void Rescale();

private:
    void CreateGui();

    void ClearSelection();
    void SetSelectedNozzle(const DevNozzle &nozzle);
    void UpdatSelectedNozzle(std::shared_ptr<DevNozzleRack> rack, int selected_nozzle_pos_id = -1); // for slicing without dynamic switch

    void UpdateNozzleInfos(std::shared_ptr<DevNozzleRack> rack);

    void OnNozzleItemSelected(wxCommandEvent& evt);

private:
    DevNozzle                    m_selected_nozzle;
    std::weak_ptr<DevNozzleRack> m_nozzle_rack;

    // pick
    bool m_enable_manual_nozzle_pick = true;

    // Label
    wgtMsgBox* m_title_tips_dynamic;

    // GUI
    wgtDeviceNozzleRackNozzleItem * m_toolhead_nozzle_l{nullptr};
    wgtDeviceNozzleRackNozzleItem * m_toolhead_nozzle_r{nullptr};
    std::unordered_map<int, wgtDeviceNozzleRackNozzleItem *> m_nozzle_items; // from 16 to 21
};

};// end of namespace Slic3r::GUI