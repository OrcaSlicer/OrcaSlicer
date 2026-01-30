#include "NetworkPluginDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Label.hpp"
#include "BitmapCache.hpp"
#include "wxExtensions.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/collpane.h>

namespace Slic3r {
namespace GUI {

NetworkPluginDownloadDialog::NetworkPluginDownloadDialog(wxWindow* parent, Mode mode,
    const std::string& current_version,
    const std::string& error_message,
    const std::string& error_details)
    : DPIDialog(parent, wxID_ANY, mode == Mode::UpdateAvailable ?
        _L("Network Plugin Update Available") : _L("Bambu Network Plugin Required"),
        wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_mode(mode)
    , m_error_message(error_message)
    , m_error_details(error_details)
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
    main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(20));

    SetSizer(main_sizer);

    if (mode == Mode::UpdateAvailable) {
        create_update_available_ui(current_version);
    } else {
        create_missing_plugin_ui();
    }
    Layout();
    Fit();
    CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void NetworkPluginDownloadDialog::create_missing_plugin_ui()
{
    wxBoxSizer* main_sizer = static_cast<wxBoxSizer*>(GetSizer());

    auto* desc = new wxStaticText(this, wxID_ANY,
        m_mode == Mode::CorruptedPlugin ?
            _L("The Bambu Network Plugin is corrupted or incompatible. Please reinstall it.") :
            _L("The Bambu Network Plugin is required for cloud features, printer discovery, and remote printing."));
    desc->SetFont(::Label::Body_13);
    desc->Wrap(FromDIP(400));
    main_sizer->Add(desc, 0, wxLEFT | wxRIGHT, FromDIP(25));
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(15));

    if (!m_error_message.empty()) {
        auto* error_label = new wxStaticText(this, wxID_ANY,
            wxString::Format(_L("Error: %s"), wxString::FromUTF8(m_error_message)));
        error_label->SetFont(::Label::Body_13);
        error_label->SetForegroundColour(wxColour(208, 93, 93));
        error_label->Wrap(FromDIP(400));
        main_sizer->Add(error_label, 0, wxLEFT | wxRIGHT, FromDIP(25));
        main_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));

        if (!m_error_details.empty()) {
            m_details_pane = new wxCollapsiblePane(this, wxID_ANY, _L("Show details"));
            auto* pane = m_details_pane->GetPane();
            auto* pane_sizer = new wxBoxSizer(wxVERTICAL);

            auto* details_text = new wxStaticText(pane, wxID_ANY, wxString::FromUTF8(m_error_details));
            details_text->SetFont(wxGetApp().code_font());
            details_text->Wrap(FromDIP(380));
            pane_sizer->Add(details_text, 0, wxALL, FromDIP(10));

            pane->SetSizer(pane_sizer);
            main_sizer->Add(m_details_pane, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(25));
            main_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));
        }
    }

    auto* version_label = new wxStaticText(this, wxID_ANY, _L("Version to install:"));
    version_label->SetFont(::Label::Body_13);
    main_sizer->Add(version_label, 0, wxLEFT | wxRIGHT, FromDIP(25));
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(5));

    setup_version_selector();
    main_sizer->Add(m_version_combo, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(25));
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(20));

    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->Add(0, 0, 1, wxEXPAND, 0);

    StateColor btn_bg_green(
        std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));

    StateColor btn_bg_white(
        std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
        std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));

    auto* btn_download = new Button(this, _L("Download and Install"));
    btn_download->SetBackgroundColor(btn_bg_green);
    btn_download->SetBorderColor(*wxWHITE);
    btn_download->SetTextColor(*wxWHITE);
    btn_download->SetFont(::Label::Body_12);
    btn_download->SetMinSize(wxSize(FromDIP(120), FromDIP(24)));
    btn_download->Bind(wxEVT_BUTTON, &NetworkPluginDownloadDialog::on_download, this);
    btn_sizer->Add(btn_download, 0, wxRIGHT, FromDIP(10));

    auto* btn_skip = new Button(this, _L("Skip for Now"));
    btn_skip->SetBackgroundColor(btn_bg_white);
    btn_skip->SetBorderColor(wxColour(38, 46, 48));
    btn_skip->SetFont(::Label::Body_12);
    btn_skip->SetMinSize(wxSize(FromDIP(100), FromDIP(24)));
    btn_skip->Bind(wxEVT_BUTTON, &NetworkPluginDownloadDialog::on_skip, this);
    btn_sizer->Add(btn_skip, 0, wxRIGHT, FromDIP(10));

    main_sizer->Add(btn_sizer, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(20));
    main_sizer->Add(0, 0, 0, wxBOTTOM, FromDIP(20));
}

void NetworkPluginDownloadDialog::create_update_available_ui(const std::string& current_version)
{
    wxBoxSizer* main_sizer = static_cast<wxBoxSizer*>(GetSizer());

    auto* desc = new wxStaticText(this, wxID_ANY,
        _L("A new version of the Bambu Network Plugin is available."));
    desc->SetFont(::Label::Body_13);
    desc->Wrap(FromDIP(400));
    main_sizer->Add(desc, 0, wxLEFT | wxRIGHT, FromDIP(25));
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(15));

    auto* version_text = new wxStaticText(this, wxID_ANY,
        wxString::Format(_L("Current version: %s"), wxString::FromUTF8(current_version)));
    version_text->SetFont(::Label::Body_13);
    main_sizer->Add(version_text, 0, wxLEFT | wxRIGHT, FromDIP(25));
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));

    auto* update_label = new wxStaticText(this, wxID_ANY, _L("Update to version:"));
    update_label->SetFont(::Label::Body_13);
    main_sizer->Add(update_label, 0, wxLEFT | wxRIGHT, FromDIP(25));
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(5));

    setup_version_selector();
    main_sizer->Add(m_version_combo, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(25));
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(20));

    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->Add(0, 0, 1, wxEXPAND, 0);

    StateColor btn_bg_green(
        std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));

    StateColor btn_bg_white(
        std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
        std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));

    auto* btn_download = new Button(this, _L("Update Now"));
    btn_download->SetBackgroundColor(btn_bg_green);
    btn_download->SetBorderColor(*wxWHITE);
    btn_download->SetTextColor(*wxWHITE);
    btn_download->SetFont(::Label::Body_12);
    btn_download->SetMinSize(wxSize(FromDIP(100), FromDIP(24)));
    btn_download->Bind(wxEVT_BUTTON, &NetworkPluginDownloadDialog::on_download, this);
    btn_sizer->Add(btn_download, 0, wxRIGHT, FromDIP(10));

    auto* btn_remind = new Button(this, _L("Remind Later"));
    btn_remind->SetBackgroundColor(btn_bg_white);
    btn_remind->SetBorderColor(wxColour(38, 46, 48));
    btn_remind->SetFont(::Label::Body_12);
    btn_remind->SetMinSize(wxSize(FromDIP(100), FromDIP(24)));
    btn_remind->Bind(wxEVT_BUTTON, &NetworkPluginDownloadDialog::on_remind_later, this);
    btn_sizer->Add(btn_remind, 0, wxRIGHT, FromDIP(10));

    auto* btn_skip = new Button(this, _L("Skip Version"));
    btn_skip->SetBackgroundColor(btn_bg_white);
    btn_skip->SetBorderColor(wxColour(38, 46, 48));
    btn_skip->SetFont(::Label::Body_12);
    btn_skip->SetMinSize(wxSize(FromDIP(100), FromDIP(24)));
    btn_skip->Bind(wxEVT_BUTTON, &NetworkPluginDownloadDialog::on_skip_version, this);
    btn_sizer->Add(btn_skip, 0, wxRIGHT, FromDIP(10));

    auto* btn_dont_ask = new Button(this, _L("Don't Ask Again"));
    btn_dont_ask->SetBackgroundColor(btn_bg_white);
    btn_dont_ask->SetBorderColor(wxColour(38, 46, 48));
    btn_dont_ask->SetFont(::Label::Body_12);
    btn_dont_ask->SetMinSize(wxSize(FromDIP(110), FromDIP(24)));
    btn_dont_ask->Bind(wxEVT_BUTTON, &NetworkPluginDownloadDialog::on_dont_ask, this);
    btn_sizer->Add(btn_dont_ask, 0, wxRIGHT, FromDIP(10));

    main_sizer->Add(btn_sizer, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(20));
    main_sizer->Add(0, 0, 0, wxBOTTOM, FromDIP(20));
}

void NetworkPluginDownloadDialog::setup_version_selector()
{
    m_version_combo = new ComboBox(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(FromDIP(380), FromDIP(28)), 0, nullptr, wxCB_READONLY);
    m_version_combo->SetFont(::Label::Body_13);

    m_available_versions = BBL::get_all_available_versions();
    for (size_t i = 0; i < m_available_versions.size(); ++i) {
        const auto& ver = m_available_versions[i];
        wxString label;
        if (!ver.suffix.empty()) {
            label = wxString::FromUTF8("\xE2\x94\x94 ") + wxString::FromUTF8(ver.display_name);
        } else {
            label = wxString::FromUTF8(ver.display_name);
            if (ver.is_latest) {
                label += wxString(" ") + _L("(Latest)");
            }
        }
        m_version_combo->Append(label);
    }

    m_version_combo->SetSelection(0);
}

std::string NetworkPluginDownloadDialog::get_selected_version() const
{
    if (!m_version_combo) {
        return "";
    }

    int selection = m_version_combo->GetSelection();
    if (selection < 0 || selection >= static_cast<int>(m_available_versions.size())) {
        return "";
    }

    return m_available_versions[selection].version;
}

void NetworkPluginDownloadDialog::on_download(wxCommandEvent& evt)
{
    int selection = m_version_combo ? m_version_combo->GetSelection() : 0;
    if (selection >= 0 && selection < static_cast<int>(m_available_versions.size())) {
        const std::string& warning = m_available_versions[selection].warning;
        if (!warning.empty()) {
            MessageDialog warn_dlg(this, wxString::FromUTF8(warning), _L("Warning"), wxOK | wxCANCEL | wxICON_WARNING);
            if (warn_dlg.ShowModal() != wxID_OK) {
                return;
            }
        }
    }
    EndModal(RESULT_DOWNLOAD);
}

void NetworkPluginDownloadDialog::on_skip(wxCommandEvent& evt)
{
    EndModal(RESULT_SKIP);
}

void NetworkPluginDownloadDialog::on_remind_later(wxCommandEvent& evt)
{
    EndModal(RESULT_REMIND_LATER);
}

void NetworkPluginDownloadDialog::on_skip_version(wxCommandEvent& evt)
{
    EndModal(RESULT_SKIP_VERSION);
}

void NetworkPluginDownloadDialog::on_dont_ask(wxCommandEvent& evt)
{
    EndModal(RESULT_DONT_ASK);
}

void NetworkPluginDownloadDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    Layout();
    Fit();
}

NetworkPluginRestartDialog::NetworkPluginRestartDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Restart Required"),
        wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
    main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(20));

    auto* icon_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* icon_bitmap = new wxStaticBitmap(this, wxID_ANY,
        create_scaled_bitmap("info", nullptr, 64));
    icon_sizer->Add(icon_bitmap, 0, wxALL, FromDIP(10));

    auto* text_sizer = new wxBoxSizer(wxVERTICAL);

    auto* desc = new wxStaticText(this, wxID_ANY,
        _L("The Bambu Network Plugin has been installed successfully."));
    desc->SetFont(::Label::Body_14);
    desc->Wrap(FromDIP(350));
    text_sizer->Add(desc, 0, wxTOP, FromDIP(10));
    text_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));

    auto* restart_msg = new wxStaticText(this, wxID_ANY,
        _L("A restart is required to load the new plugin. Would you like to restart now?"));
    restart_msg->SetFont(::Label::Body_13);
    restart_msg->Wrap(FromDIP(350));
    text_sizer->Add(restart_msg, 0, wxBOTTOM, FromDIP(10));

    icon_sizer->Add(text_sizer, 1, wxEXPAND | wxRIGHT, FromDIP(20));
    main_sizer->Add(icon_sizer, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(15));
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(20));

    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->Add(0, 0, 1, wxEXPAND, 0);

    StateColor btn_bg_green(
        std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));

    StateColor btn_bg_white(
        std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
        std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));

    auto* btn_restart = new Button(this, _L("Restart Now"));
    btn_restart->SetBackgroundColor(btn_bg_green);
    btn_restart->SetBorderColor(*wxWHITE);
    btn_restart->SetTextColor(*wxWHITE);
    btn_restart->SetFont(::Label::Body_12);
    btn_restart->SetMinSize(wxSize(FromDIP(100), FromDIP(24)));
    btn_restart->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_restart_now = true;
        EndModal(wxID_OK);
    });
    btn_sizer->Add(btn_restart, 0, wxRIGHT, FromDIP(10));

    auto* btn_later = new Button(this, _L("Restart Later"));
    btn_later->SetBackgroundColor(btn_bg_white);
    btn_later->SetBorderColor(wxColour(38, 46, 48));
    btn_later->SetFont(::Label::Body_12);
    btn_later->SetMinSize(wxSize(FromDIP(100), FromDIP(24)));
    btn_later->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_restart_now = false;
        EndModal(wxID_CANCEL);
    });
    btn_sizer->Add(btn_later, 0, wxRIGHT, FromDIP(10));

    main_sizer->Add(btn_sizer, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(20));
    main_sizer->Add(0, 0, 0, wxBOTTOM, FromDIP(20));

    SetSizer(main_sizer);
    Layout();
    Fit();
    CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void NetworkPluginRestartDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    Layout();
    Fit();
}

}
}
