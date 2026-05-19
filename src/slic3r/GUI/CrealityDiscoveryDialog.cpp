#include "CrealityDiscoveryDialog.hpp"
#include "slic3r/Utils/CrealityHostDiscovery.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

#include <wx/sizer.h>
#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace GUI {

CrealityDiscoveryDialog::CrealityDiscoveryDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("Detect Creality K-series printer"),
               wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    const int em = wxGetApp().em_unit();

    m_status = new wxStaticText(this, wxID_ANY, _L("Click Scan to look for K-series printers on your network."));

    m_list = new wxListView(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                            wxLC_REPORT | wxSIMPLE_BORDER | wxLC_SINGLE_SEL);
    m_list->SetMinSize(wxSize(50 * em, 18 * em));
    m_list->AppendColumn(_L("Model"),    wxLIST_FORMAT_LEFT,  8 * em);
    m_list->AppendColumn(_L("Hostname"), wxLIST_FORMAT_LEFT, 14 * em);
    m_list->AppendColumn(_L("IP"),       wxLIST_FORMAT_LEFT, 14 * em);

    auto* scan_btn   = new wxButton(this, wxID_ANY, _L("Scan"));
    auto* ok_btn     = new wxButton(this, wxID_OK,     _L("Use Selected"));
    auto* cancel_btn = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    ok_btn->Disable();

    auto* button_sizer = new wxBoxSizer(wxHORIZONTAL);
    button_sizer->Add(scan_btn,   0, wxALL, em);
    button_sizer->AddStretchSpacer(1);
    button_sizer->Add(ok_btn,     0, wxALL, em);
    button_sizer->Add(cancel_btn, 0, wxALL, em);

    auto* vsizer = new wxBoxSizer(wxVERTICAL);
    vsizer->Add(m_status, 0, wxEXPAND | wxALL, em);
    vsizer->Add(m_list,   1, wxEXPAND | wxALL, em);
    vsizer->Add(button_sizer, 0, wxEXPAND);
    SetSizerAndFit(vsizer);

    scan_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { run_discovery(); });

    m_list->Bind(wxEVT_LIST_ITEM_SELECTED,   [ok_btn](wxListEvent&) { ok_btn->Enable(); });
    m_list->Bind(wxEVT_LIST_ITEM_DESELECTED, [ok_btn](wxListEvent&) { ok_btn->Disable(); });
    m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED,  [this](wxListEvent&)   { on_ok(); EndModal(wxID_OK); });

    ok_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_ok(); EndModal(wxID_OK); });

    wxGetApp().UpdateDlgDarkUI(this);

    // Run discovery synchronously from the ctor so results are ready by the
    // time the dialog is shown. Posting an async CallAfter from a ShowModal
    // override risked the event firing after the modal loop had exited -- the
    // captured `this` would then be a dangling stack pointer and subsequent
    // UI access could fault.
    run_discovery();
}

void CrealityDiscoveryDialog::run_discovery()
{
    m_status->SetLabel(_L("Scanning the LAN for K-series printers... this takes a few seconds."));
    m_list->DeleteAllItems();
    m_rows.clear();
    Layout();
    Update();

    std::vector<CrealityHost> hosts;
    {
        wxBusyCursor cursor;
        wxWindowDisabler disabler(this);
        hosts = CrealityHostDiscovery::scan(/*probe_info=*/true);
    }

    for (const auto& h : hosts) {
        Row row;
        row.ip       = h.ip;
        row.hostname = h.hostname;
        if (!h.model_name.empty())
            row.model = h.model_name;
        else if (h.cfs_capable)
            row.model = "(unknown K-series)";
        else
            row.model = "Creality";
        m_rows.push_back(std::move(row));
    }

    for (size_t i = 0; i < m_rows.size(); ++i) {
        long idx = m_list->InsertItem(i, wxString::FromUTF8(m_rows[i].model));
        m_list->SetItem(idx, 1, wxString::FromUTF8(m_rows[i].hostname));
        m_list->SetItem(idx, 2, wxString::FromUTF8(m_rows[i].ip));
    }

    if (m_rows.empty()) {
        m_status->SetLabel(_L(
            "No K-series printers found. Make sure the printer is on the same "
            "network and not blocked by Wi-Fi client isolation, then click Scan again."));
    } else {
        m_status->SetLabel(wxString::Format(
            _L("Found %zu Creality printer(s). Select one and click Use Selected."),
            m_rows.size()));
        m_list->Select(0);
    }
}

void CrealityDiscoveryDialog::on_ok()
{
    auto sel = m_list->GetFirstSelected();
    if (sel >= 0 && sel < int(m_rows.size())) {
        m_selected_ip = m_rows[sel].ip;
    }
}

} // namespace GUI
} // namespace Slic3r
