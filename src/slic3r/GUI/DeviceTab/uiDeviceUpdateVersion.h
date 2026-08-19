/**********************************************************
* File: uiDeviceUpdateVersion.h
*  Description: The panel with firmware info
* 
*  \n class uiDeviceUpdateVersion
**********************************************************/

#pragma once
#include <wx/panel.h>
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/DeviceManager.hpp"

// Previous defintions
class wxStaticText;
class wxStaticBitmap;

namespace Slic3r::GUI
{
// @Class uiDeviceUpdateVersion
// @Note  The panel with firmware info
class uiDeviceUpdateVersion : public wxPanel
{
public:
    uiDeviceUpdateVersion(wxWindow* parent,
                          wxWindowID id = wxID_ANY,
                          const wxPoint& pos = wxDefaultPosition,
                          const wxSize& size = wxDefaultSize,
                          long style = wxTAB_TRAVERSAL);
    ~uiDeviceUpdateVersion() = default;

public:
    void  UpdateInfo(const DevFirmwareVersionInfo& info);

private:
    void  CreateWidgets();

    void  SetName(const wxString& str) { m_dev_name->SetLabel(str); };
    void  SetSerial(const wxString& str) { m_dev_snl->SetLabel(str); };
    void  SetVersion(const wxString& cur_version, const wxString& latest_version);

private:
    wxStaticText*   m_dev_name;
    wxStaticText*   m_dev_snl;
    wxStaticText*   m_dev_version;
    wxStaticBitmap* m_dev_upgrade_indicator;
};
};// end of namespace Slic3r::GUI
