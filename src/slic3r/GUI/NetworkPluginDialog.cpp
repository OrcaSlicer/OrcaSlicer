#include "NetworkPluginDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/DialogButtons.hpp"
#include "BitmapCache.hpp"
#include "wxExtensions.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/collpane.h>

#define BORDER_W     FromDIP(20)
#define TEXT_WRAP    FromDIP(400)
#define DIALOG_WIDTH FromDIP(440)

namespace Slic3r {
namespace GUI {

NetworkPluginDownloadDialog::NetworkPluginDownloadDialog(wxWindow* parent, Mode mode,
    const std::string& current_version,
    const std::string& error_message,
    const std::string& error_details)
    : DPIDialog(parent, wxID_ANY, mode == Mode::UpdateAvailable ?
        _L("Network Plug-in Update Available") : _L("Bambu Network Plug-in Required"),
        wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_mode(mode)
    , m_error_message(error_message)
    , m_error_details(error_details)
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(DIALOG_WIDTH, 1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
    main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    main_sizer->AddSpacer(BORDER_W);

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

    auto* desc = new Label(this,
        m_mode == Mode::CorruptedPlugin ?
            _L("The Bambu Network Plug-in is corrupted or incompatible. Please reinstall it.") :
            _L("The Bambu Network Plug-in is required for cloud features, printer discovery, and remote printing."));
    desc->Wrap(TEXT_WRAP);
    desc->SetMaxSize(wxSize(TEXT_WRAP, -1));
    main_sizer->Add(desc, 0, wxLEFT | wxRIGHT, BORDER_W);
    main_sizer->AddSpacer(FromDIP(15));

    if (!m_error_message.empty()) {
        auto* error_label = new wxStaticText(this, wxID_ANY,
            wxString::Format(_L("Error: %s"), wxString::FromUTF8(m_error_message)));
        error_label->SetFont(::Label::Body_13);
        error_label->SetForegroundColour(wxColour(208, 93, 93));
        error_label->Wrap(TEXT_WRAP);
        error_label->SetMaxSize(wxSize(TEXT_WRAP, -1));
        main_sizer->Add(error_label, 0, wxLEFT | wxRIGHT, BORDER_W);
        main_sizer->AddSpacer(FromDIP(5));

        if (!m_error_details.empty()) {
            auto expand_btn = new Button(this, _L("Show details"));
            expand_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
            main_sizer->Add(expand_btn, 0, wxLEFT, BORDER_W);
            main_sizer->AddSpacer(FromDIP(5));

            auto details_text = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(m_error_details),
                wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxNO_BORDER);

            details_text->SetFont(wxGetApp().code_font());
            details_text->SetBackgroundColour(wxColour("#F1F1F1"));
            details_text->SetMaxSize(wxSize(TEXT_WRAP, -1));
            main_sizer->Add(details_text, 0, wxLEFT | wxRIGHT | wxEXPAND, BORDER_W);

            details_text->Hide();

            expand_btn->Bind(wxEVT_BUTTON, [this, details_text, expand_btn](wxCommandEvent&){
                Freeze();
                details_text->Show(!details_text->IsShown());
                expand_btn->SetLabel(details_text->IsShown() ? _L("Hide details") : _L("Show details"));
                Layout();
                Fit();
                Refresh();
                Thaw();
            });

            main_sizer->AddSpacer(FromDIP(15));
        }
    }

    auto* version_label = new Label(this, _L("Version to install:"));
    main_sizer->Add(version_label, 0, wxLEFT | wxRIGHT, BORDER_W);
    main_sizer->AddSpacer(FromDIP(3));

    setup_version_selector();

    main_sizer->Add(m_version_combo, 0, wxLEFT | wxRIGHT | wxEXPAND, BORDER_W);
    main_sizer->AddSpacer(15);

    auto dlg_btns = new DialogButtons(this,
        {"Download and Install", "Skip for Now"},
        _L("Download and Install")  // Primary button
    );

    dlg_btns->GetButtonFromIndex(0)->Bind(wxEVT_BUTTON, &NetworkPluginDownloadDialog::on_download, this);
    dlg_btns->GetButtonFromIndex(1)->Bind(wxEVT_BUTTON, &NetworkPluginDownloadDialog::on_skip, this);

    main_sizer->Add(dlg_btns, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(10));
}

void NetworkPluginDownloadDialog::create_update_available_ui(const std::string& current_version)
{
    wxBoxSizer* main_sizer = static_cast<wxBoxSizer*>(GetSizer());

    auto* desc = new Label(this,
        _L("A new version of the Bambu Network Plug-in is available."));
    desc->Wrap(TEXT_WRAP);
    desc->SetMaxSize(wxSize(TEXT_WRAP, -1));
    main_sizer->Add(desc, 0, wxLEFT | wxRIGHT, BORDER_W);
    main_sizer->AddSpacer(FromDIP(15));

    auto* version_text = new Label(this,
        wxString::Format(_L("Current version: %s"), wxString::FromUTF8(current_version)));
    main_sizer->Add(version_text, 0, wxLEFT | wxRIGHT, BORDER_W);
    main_sizer->AddSpacer(FromDIP(15));

    auto* update_label = new Label(this, _L("Update to version:"));
    main_sizer->Add(update_label, 0, wxLEFT | wxRIGHT, BORDER_W);
    main_sizer->AddSpacer(FromDIP(3));

    setup_version_selector();
    main_sizer->Add(m_version_combo, 0, wxLEFT | wxRIGHT | wxEXPAND, BORDER_W);
    main_sizer->AddSpacer(20);

    auto daa_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto cfg = wxGetApp().app_config;

    auto daa_chk = new CheckBox(this);
    daa_chk->SetValue(cfg->is_network_update_prompt_disabled());
    daa_chk->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e){
        auto cfg = wxGetApp().app_config;
        cfg->set_network_update_prompt_disabled(e.IsChecked());
        cfg->save();
    });

    auto daa_str = new Label(this, _L("Don't Ask Again"));
    auto on_toggle = [this, daa_chk]() {
        daa_chk->SetValue(!daa_chk->GetValue());
        wxCommandEvent evt(wxEVT_TOGGLEBUTTON, daa_chk->GetId());
        evt.SetEventObject(daa_chk);
        daa_chk->GetEventHandler()->ProcessEvent(evt);
    };
    daa_str->Bind(wxEVT_LEFT_DOWN,   [on_toggle](wxMouseEvent& e) {if(!e.LeftDClick()) on_toggle();});
    daa_str->Bind(wxEVT_LEFT_DCLICK, [on_toggle](wxMouseEvent& e) {on_toggle();});

    daa_sizer->Add(daa_chk, 0, wxALIGN_CENTER_VERTICAL);
    daa_sizer->Add(daa_str, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));

    main_sizer->Add(daa_sizer, 0, wxLEFT | wxRIGHT | wxEXPAND, BORDER_W);
    main_sizer->AddSpacer(10);

    auto dlg_btns = new DialogButtons(this,
        {"Update Now", "Remind Later", "Skip Version"},
        _L("Update Now")
    );

    dlg_btns->GetButtonFromIndex(0)->Bind(wxEVT_BUTTON, &NetworkPluginDownloadDialog::on_download, this);
    dlg_btns->GetButtonFromIndex(1)->Bind(wxEVT_BUTTON, &NetworkPluginDownloadDialog::on_remind_later, this);
    dlg_btns->GetButtonFromIndex(2)->Bind(wxEVT_BUTTON, &NetworkPluginDownloadDialog::on_skip_version, this);

    main_sizer->Add(dlg_btns, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(10));
}

wxString network_version_label(const NetworkLibraryVersionInfo& ver)
{
    wxString label = wxString::FromUTF8(ver.display_name);
    if (!ver.suffix.empty())
        label = wxString::FromUTF8("\xE2\x94\x94 ") + label;
    // Both can apply: the loaded build may also be the highest listed one.
    if (ver.is_latest)
        label += wxString(" ") + _L("(Latest)");
    if (ver.is_loaded)
        label += wxString(" ") + _L("(installed)");
    return label;
}

void NetworkPluginDownloadDialog::setup_version_selector()
{
    m_version_combo = new ComboBox(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(FromDIP(380), FromDIP(28)), 0, nullptr, wxCB_READONLY);

    m_available_versions = get_all_available_versions();
    for (const auto& ver : m_available_versions)
        m_version_combo->Append(network_version_label(ver));

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

    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(DIALOG_WIDTH, 1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
    main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    main_sizer->AddSpacer(BORDER_W);

    auto* icon_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* icon_bitmap = new wxStaticBitmap(this, wxID_ANY,
        create_scaled_bitmap("info", nullptr, 64));
    icon_sizer->Add(icon_bitmap, 0, wxALL, FromDIP(10));

    auto* text_sizer = new wxBoxSizer(wxVERTICAL);

    auto* desc = new Label(this, 
        _L("The Bambu Network Plug-in has been installed successfully."));
    desc->Wrap(TEXT_WRAP);
    desc->SetMaxSize(wxSize(TEXT_WRAP, -1));
    text_sizer->Add(desc, 0, wxTOP, FromDIP(10));
    text_sizer->AddSpacer(FromDIP(10));

    auto* restart_msg = new Label(this,
        _L("A restart is required to load the new plug-in. Would you like to restart now?"));
    restart_msg->Wrap(TEXT_WRAP);
    restart_msg->SetMaxSize(wxSize(TEXT_WRAP, -1));
    text_sizer->Add(restart_msg, 0, wxBOTTOM, FromDIP(10));

    icon_sizer->Add(text_sizer, 1, wxEXPAND | wxRIGHT, BORDER_W);
    main_sizer->Add(icon_sizer, 0, wxLEFT | wxRIGHT | wxEXPAND, BORDER_W);
    main_sizer->AddSpacer(15);

    auto dlg_btns = new DialogButtons(this,
        {"Restart Now", "Restart Later"},
        _L("Restart Now") // Primary button
    );

    dlg_btns->GetButtonFromIndex(0)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_restart_now = true;
        EndModal(wxID_OK);
    });

    dlg_btns->GetButtonFromIndex(1)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_restart_now = false;
        EndModal(wxID_CANCEL);
    });
    
    main_sizer->Add(dlg_btns, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(10));

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
