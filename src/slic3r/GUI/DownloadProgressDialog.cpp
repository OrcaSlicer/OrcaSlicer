#include "DownloadProgressDialog.hpp"

#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/statbmp.h>
#include <wx/scrolwin.h>
#include <wx/clipbrd.h>
#include <wx/checkbox.h>
#include <wx/html/htmlwin.h>

#include <boost/algorithm/string/replace.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
//#include "ConfigWizard.hpp"
#include "wxExtensions.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "GUI_App.hpp"
#include "Jobs/BoostThreadWorker.hpp"
#include "Jobs/PlaterWorker.hpp"

#include "Widgets/HyperLink.hpp" // ORCA

#define DESIGN_INPUT_SIZE wxSize(FromDIP(100), -1)

namespace Slic3r {
namespace GUI {



DownloadProgressDialog::DownloadProgressDialog(wxString title)
    : DPIDialog(static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    wxString download_failed_url = wxT("https://wiki.bambulab.com/en/software/bambu-studio/failed-to-get-network-plugin");
    wxString install_failed_url = wxT("https://wiki.bambulab.com/en/software/bambu-studio/failed-to-get-network-plugin");

    wxString download_failed_msg = _L("Failed to download the plug-in. Please check your firewall settings and vpn software, check and retry.");
    wxString install_failed_msg = _L("Failed to install the plug-in. Please check whether it is blocked or deleted by anti-virus software.");

    SetBackgroundColour(*wxWHITE);
    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);
    auto        m_line_top   = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
    m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);


    m_simplebook_status = new wxSimplebook(this);
    m_simplebook_status->SetSize(wxSize(FromDIP(400), FromDIP(70)));
    m_simplebook_status->SetMinSize(wxSize(FromDIP(400), FromDIP(70)));
    m_simplebook_status->SetMaxSize(wxSize(FromDIP(400), FromDIP(70)));

    //mode normal
    m_status_bar    = std::make_shared<BBLStatusBarSend>(m_simplebook_status);
    m_panel_download = m_status_bar->get_panel();
    m_panel_download->SetSize(wxSize(FromDIP(400), FromDIP(70)));
    m_panel_download->SetMinSize(wxSize(FromDIP(400), FromDIP(70)));
    m_panel_download->SetMaxSize(wxSize(FromDIP(400), FromDIP(70)));

    m_worker = std::make_unique<PlaterWorker<BoostThreadWorker>>(this, m_status_bar, "download_worker");

    //mode Download Failed 
    auto m_panel_download_failed = new wxPanel(m_simplebook_status, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);

    wxBoxSizer* sizer_download_failed = new wxBoxSizer(wxVERTICAL);

    auto m_statictext_download_failed = new wxStaticText(m_panel_download_failed, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
    m_statictext_download_failed->SetForegroundColour(*wxBLACK);
    m_statictext_download_failed->SetLabel(format_text(m_statictext_download_failed, download_failed_msg, FromDIP(360)));
    m_statictext_download_failed->Wrap(FromDIP(360));

    sizer_download_failed->Add(m_statictext_download_failed, 0, wxALIGN_CENTER | wxALL, 5);

    // ORCA standardized HyperLink
    auto m_download_hyperlink = new HyperLink(m_panel_download_failed, _L("click here to see more info"), download_failed_url);
    sizer_download_failed->Add(m_download_hyperlink, 0, wxALIGN_CENTER | wxALL, 5);


    m_panel_download_failed->SetSizer(sizer_download_failed);
    m_panel_download_failed->Layout();
    sizer_download_failed->Fit(m_panel_download_failed);


    //mode Installed failed
    auto m_panel_install_failed = new wxPanel(m_simplebook_status, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);

    wxBoxSizer* sizer_install_failed = new wxBoxSizer(wxVERTICAL);

    auto m_statictext_install_failed = new wxStaticText(m_panel_install_failed, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
    m_statictext_install_failed->SetForegroundColour(*wxBLACK);
    m_statictext_install_failed->SetLabel(format_text(m_statictext_install_failed, install_failed_msg,FromDIP(360)));
    m_statictext_install_failed->Wrap(FromDIP(360));

    sizer_install_failed->Add(m_statictext_install_failed, 0, wxALIGN_CENTER | wxALL, 5);

    // ORCA standardized HyperLink
    auto m_install_hyperlink = new HyperLink(m_panel_install_failed, _L("click here to see more info"), install_failed_url);
    sizer_install_failed->Add(m_install_hyperlink, 0, wxALIGN_CENTER | wxALL, 5);


    m_panel_install_failed->SetSizer(sizer_install_failed);
    m_panel_install_failed->Layout();
    sizer_install_failed->Fit(m_panel_install_failed);

    m_sizer_main->Add(m_simplebook_status, 0, wxALL, FromDIP(20));
    m_sizer_main->Add(0, 0, 1, wxBOTTOM, 10);


    m_simplebook_status->AddPage(m_status_bar->get_panel(), wxEmptyString, true);
    m_simplebook_status->AddPage(m_panel_download_failed, wxEmptyString, false);
    m_simplebook_status->AddPage(m_panel_install_failed, wxEmptyString, false);

    SetSizer(m_sizer_main);
    Layout();
    Fit();
    CentreOnParent();

    Bind(wxEVT_CLOSE_WINDOW, &DownloadProgressDialog::on_close, this);
    wxGetApp().UpdateDlgDarkUI(this);
}

wxString DownloadProgressDialog::format_text(wxStaticText* st, wxString str, int warp)
{
    if (wxGetApp().app_config->get("language") != "zh_CN") { return str; }

    wxString out_txt = str;
    wxString count_txt = "";
    int      new_line_pos = 0;

    for (int i = 0; i < str.length(); i++) {
        auto text_size = st->GetTextExtent(count_txt);
        if (text_size.x < warp) {
            count_txt += str[i];
        }
        else {
            out_txt.insert(i - 1, '\n');
            count_txt = "";
        }
    }
    return out_txt;
}

bool DownloadProgressDialog::Show(bool show)
{
    if (show) {
        m_simplebook_status->SetSelection(0);
        auto m_upgrade_job = make_job();
        m_upgrade_job->set_event_handle(this);
        m_status_bar->set_progress(0);
        Bind(EVT_UPGRADE_NETWORK_SUCCESS, [this](wxCommandEvent& evt) {
            m_status_bar->change_button_label(_L("Close"));
            on_finish();
            m_status_bar->set_cancel_callback_fina(
                [this]() {
                    this->Close();
                }
            );
        });

        //download failed
        Bind(EVT_DOWNLOAD_NETWORK_FAILED, [this](wxCommandEvent& evt) {
            m_status_bar->change_button_label(_L("Close"));
            m_status_bar->set_progress(0);
            this->m_simplebook_status->SetSelection(1);
            m_status_bar->set_cancel_callback_fina(
                [this]() {
                    this->Close();
                }
            );
        });

        //install failed
        Bind(EVT_INSTALL_NETWORK_FAILED, [this](wxCommandEvent& evt) {
            m_status_bar->change_button_label(_L("Close"));
            m_status_bar->set_progress(0);
            this->m_simplebook_status->SetSelection(2);
            m_status_bar->set_cancel_callback_fina(
                [this]() {
                    this->Close();
                }
            );
        });

        m_status_bar->set_cancel_callback_fina([this]() {
            m_worker->cancel_all();
        });

        replace_job(*m_worker, std::move(m_upgrade_job));
    }
    return DPIDialog::Show(show);
}

void DownloadProgressDialog::on_close(wxCloseEvent& event)
{
    m_worker.get()->cancel_all();
    event.Skip();
}

 DownloadProgressDialog::~DownloadProgressDialog() {}

void DownloadProgressDialog::on_dpi_changed(const wxRect &suggested_rect) {}

void DownloadProgressDialog::update_release_note(std::string release_note, std::string version) {}

std::unique_ptr<UpgradeNetworkJob> DownloadProgressDialog::make_job() { return std::make_unique<UpgradeNetworkJob>(); }

void DownloadProgressDialog::on_finish()
{
    if (wxGetApp().hot_reload_network_plugin()) {
        return;
    }

    MessageDialog dlg(nullptr,
        _L("The network plugin was installed but could not be loaded. Please restart the application."),
        _L("Restart Required"), wxOK | wxICON_INFORMATION);
    dlg.ShowModal();
}

}} // namespace Slic3r::GUI
