//**********************************************************/
/* File: wgtDeviceNozzleRackNozzleItem.h
*  Description: One nozzle cell of the H2C hotend rack view.
*
*  This header defines only the single-nozzle-cell widget (and the selection event it emits), which
*  is the one dependency MultiNozzleSync's HotEndTable needs. The rest of the Device-tab rack panel
*  (wgtDeviceNozzleRack / ...Area / ...ToolHead / ...Pos) is not provided here.
//**********************************************************/

#pragma once
#include "slic3r/GUI/DeviceCore/DevNozzleRack.h"

#include "slic3r/GUI/Widgets/StaticBox.hpp"
#include "slic3r/GUI/Widgets/Label.hpp" // complete type required by the inline SetDisplayIdText below

#include <wx/panel.h>
#include <memory>

// Previous definitions
class ScalableBitmap;
namespace Slic3r
{
    struct DevNozzle;
    class DevNozzleRack;
};

// Events
wxDECLARE_EVENT(EVT_NOZZLE_RACK_NOZZLE_ITEM_SELECTED, wxCommandEvent);

namespace Slic3r::GUI
{
class wgtDeviceNozzleRackNozzleItem : public StaticBox
{
public:
    enum NOZZLE_STATUS
    {
        NOZZLE_EMPTY,
        NOZZLE_NORMAL,
        NOZZLE_UNKNOWN,
        NOZZLE_ERROR
    };

public:
    wgtDeviceNozzleRackNozzleItem(wxWindow* parent, int nozzle_id);

public:
    void Update(const std::shared_ptr<DevNozzleRack> rack, bool on_rack = true); // on_rack is false means extruder nozzle

    int  GetNozzleId() const { return m_nozzle_id; }
    void SetDisplayIdText(const wxString& text) { m_nozzle_label_id->SetLabel(text);};

    void EnableSelect();;
    void SetSelected(bool selected);
    bool IsSelected() const { return m_is_selected; }

    bool IsDisabled() const { return m_is_disabled; }
    void SetDisable(bool disabled);

    void Rescale();

private:
    void CreateGui();

    void SetNozzleStatus(NOZZLE_STATUS status, const wxString& str1, const wxString& str2, const std::string& color);

    void OnBtnNozzleStatus(wxMouseEvent& evt);
    void OnItemSelected(wxMouseEvent& evt);

private:
    std::weak_ptr<DevNozzleRack> m_rack;

    int           m_nozzle_id; // internal id, from 0 to 5
    std::string   m_filament_color;
    NOZZLE_STATUS m_status      = NOZZLE_STATUS::NOZZLE_EMPTY;

    // select
    bool  m_is_selected = false;
    bool  m_enable_select = false;
    ScalableBitmap* m_nozzle_selected_image{ nullptr };
    wxStaticBitmap* m_nozzle_selected_bitmap{ nullptr };

    // enable or disable
    bool m_is_disabled = false;

    // Images
    ScalableBitmap* m_nozzle_normal_image{ nullptr };
    ScalableBitmap* m_nozzle_empty_image{ nullptr };
    ScalableBitmap* m_nozzle_unknown_image{ nullptr };
    ScalableBitmap* m_nozzle_error_image{ nullptr };

    // GUI
    wxStaticBitmap* m_nozzle_icon{ nullptr };
    Label* m_nozzle_label_id { nullptr };
    Label* m_nozzle_label_1{ nullptr };
    wxStaticBitmap* m_nozzle_status_icon = nullptr;
    Label* m_nozzle_label_2{ nullptr };
};

};// end of namespace Slic3r::GUI
