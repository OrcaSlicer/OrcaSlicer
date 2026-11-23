#include "StoragePanel.h"

#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "MediaFilePanel.h"
#include "OrcaFilesPanel.h"
#include "Widgets/StateColor.hpp"

#include <wx/colour.h>
#include <wx/panel.h>
#include <wx/event.h>
#include <wx/sizer.h>

namespace Slic3r {
namespace GUI {

StoragePanel::StoragePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    const wxColour light_background("#EEEEEE");
    SetBackgroundColour(wxGetApp().dark_mode() ? StateColor::darkModeColorFor(light_background) : light_background);
    SetSizer(new wxBoxSizer(wxVERTICAL));
}

MediaFilePanel* StoragePanel::ensureBambuPanel()
{
    if (!m_bambu_panel) {
        m_bambu_panel = new MediaFilePanel(this);
        m_bambu_panel->Hide();
        GetSizer()->Add(m_bambu_panel, 1, wxEXPAND);
        wxGetApp().UpdateDarkUIWin(m_bambu_panel);
    }
    return m_bambu_panel;
}

OrcaFilesPanel* StoragePanel::ensureOrcaPanel()
{
    if (!m_orca_panel) {
        m_orca_panel = new OrcaFilesPanel(this);
        m_orca_panel->Hide();
        GetSizer()->Add(m_orca_panel, 1, wxEXPAND);
        wxGetApp().UpdateDarkUIWin(m_orca_panel);
    }
    return m_orca_panel;
}

void StoragePanel::showChild(Child child)
{
    if (m_bambu_panel)
        m_bambu_panel->Show(child == Child::Bambu);
    if (m_orca_panel)
        m_orca_panel->Show(child == Child::Orca);
    m_active_child = child;
    Layout();
}

void StoragePanel::UpdateByObj(MachineObject* obj)
{
    // Agent capability controls the page; Bambu remains the fallback for other printers.
    Child target = m_active_child;
    if (obj)
        target = obj->printer_supports_feature("printer_files") ? Child::Orca : Child::Bambu;
    else if (target == Child::None)
        target = Child::Bambu;

    if (target != m_active_child) {
        if (m_active_child == Child::Bambu && m_bambu_panel)
            m_bambu_panel->UpdateByObj(nullptr);
        else if (m_active_child == Child::Orca && m_orca_panel)
            m_orca_panel->UpdateByObj(nullptr);

        if (target == Child::Bambu)
            ensureBambuPanel();
        else
            ensureOrcaPanel();
        showChild(target);
    }

    if (target == Child::Bambu)
        ensureBambuPanel()->UpdateByObj(obj);
    else if (target == Child::Orca)
        ensureOrcaPanel()->UpdateByObj(obj);
}

void StoragePanel::SwitchStorage(bool external)
{
    if (m_bambu_panel)
        m_bambu_panel->SwitchStorage(external);
}

void StoragePanel::Rescale()
{
    if (m_bambu_panel)
        m_bambu_panel->Rescale();
    if (m_orca_panel)
        m_orca_panel->Rescale();
}

void StoragePanel::on_sys_color_changed()
{
    const wxColour light_background("#EEEEEE");
    SetBackgroundColour(wxGetApp().dark_mode() ? StateColor::darkModeColorFor(light_background) : light_background);
    if (m_bambu_panel)
        wxGetApp().UpdateDarkUIWin(m_bambu_panel);
    if (m_orca_panel)
        wxGetApp().UpdateDarkUIWin(m_orca_panel);
    if (m_orca_panel)
        m_orca_panel->on_sys_color_changed();
    Refresh();
}

}}
