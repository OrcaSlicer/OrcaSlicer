#include "PhysicalPrinterDialog.hpp"
#include "PresetComboBoxes.hpp"
#include "PrinterCloudAuthDialog.hpp"

#include <cstddef>
#include <vector>
#include <string>
#include <boost/algorithm/string.hpp>
#include <boost/regex.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/statbox.h>
#include <wx/wupdlock.h>

#include "libslic3r/libslic3r.h"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "Widgets/DialogButtons.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "Tab.hpp"
#include "wxExtensions.hpp"
#include "PrintHostDialogs.hpp"
#include "../Utils/ASCIIFolding.hpp"
#include "../Utils/PrintHost.hpp"
#include "../Utils/FixModelByWin10.hpp"
#include "../Utils/UndoRedo.hpp"
#include "RemovableDriveManager.hpp"
#include "BitmapCache.hpp"
#include "BonjourDialog.hpp"
#include "MsgDialog.hpp"
#include "OAuthDialog.hpp"
#include "SimplyPrint.hpp"

namespace Slic3r {
namespace GUI {

#define BORDER_W FromDIP(10)

//------------------------------------------
//          PhysicalPrinterDialog
//------------------------------------------

PhysicalPrinterDialog::PhysicalPrinterDialog(wxWindow* parent) :
    DPIDialog(parent, wxID_ANY, _L("Physical Printer"), wxDefaultPosition, wxSize(45 * wxGetApp().em_unit(), -1), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetFont(wxGetApp().normal_font());
    SetBackgroundColour(*wxWHITE);

    // input the preset name
    Tab *tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
    m_presets = tab->get_presets();
    const Preset &sel_preset  = m_presets->get_selected_preset();
    std::string suffix = _CTX_utf8(L_CONTEXT("Copy", "PresetName"), "PresetName");
    std::string   preset_name = sel_preset.is_default ? "Untitled" : sel_preset.is_system ? (boost::format(("%1% - %2%")) % sel_preset.name % suffix).str() : sel_preset.name;

    auto input_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText *label_top = new wxStaticText(this, wxID_ANY, from_u8((boost::format(_utf8(L("Save %s as"))) % into_u8(tab->title())).str()));
    label_top->SetFont(::Label::Body_14);
    label_top->SetForegroundColour(wxColour(38,46,48));

    m_input_area = new RoundedRectangle(this, StateColor::darkModeColorFor(wxColour("#DBDBDB")), wxDefaultPosition, wxSize(-1,-1), 3, 1);
    m_input_area->SetMinSize(wxSize(FromDIP(360), FromDIP(32)));

    wxBoxSizer *input_sizer_h = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *input_sizer_v  = new wxBoxSizer(wxVERTICAL);

    m_input_ctrl = new wxTextCtrl(m_input_area, -1, from_u8(preset_name), wxDefaultPosition, wxSize(wxSize(FromDIP(360), FromDIP(32)).x, -1), 0 | wxBORDER_NONE);
    m_input_ctrl->SetBackgroundColour(wxColour(255, 255, 255));
    m_input_ctrl->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { update(); });


    input_sizer_v->Add(m_input_ctrl, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, 12);
    input_sizer_h->Add(input_sizer_v, 0, wxALIGN_CENTER, 0);

    m_input_area->SetSizer(input_sizer_h);
    m_input_area->Layout();

    m_valid_label = new wxStaticText(this, wxID_ANY, "");
    m_valid_label->SetForegroundColour(wxColor(255, 111, 0));

    input_sizer->Add(label_top, 0, wxEXPAND | wxLEFT, BORDER_W);
    input_sizer->Add(m_input_area, 0, wxEXPAND | wxTOP, BORDER_W);
    input_sizer->Add(m_valid_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, BORDER_W);


    m_config = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
    m_optgroup = new ConfigOptionsGroup(this, _L("Print Host upload"), m_config);
    check_host_key_valid();
    build_printhost_settings(m_optgroup);

    auto dlg_btns = new DialogButtons(this, {"OK"});

    btnOK = dlg_btns->GetOK();
    btnOK->Bind(wxEVT_BUTTON, &PhysicalPrinterDialog::OnOK, this);

    wxBoxSizer* topSizer = new wxBoxSizer(wxVERTICAL);

    // topSizer->Add(label_top           , 0, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, BORDER_W);
    topSizer->Add(input_sizer         , 0, wxEXPAND | wxALL, BORDER_W);
    topSizer->Add(m_optgroup->sizer   , 1, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, BORDER_W);
    topSizer->Add(dlg_btns, 0, wxEXPAND);

    Bind(wxEVT_CLOSE_WINDOW, [this](auto& e) {this->EndModal(wxID_NO);});

    SetSizer(topSizer);
    topSizer->SetSizeHints(this);
    this->CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

PhysicalPrinterDialog::~PhysicalPrinterDialog()
{
}

void PhysicalPrinterDialog::build_printhost_settings(ConfigOptionsGroup* m_optgroup)
{
    m_optgroup->m_on_change = [this](t_config_option_key opt_key, boost::any value) {
        if (opt_key == "host_type" || opt_key == "printhost_authorization_type")
            this->update();
        if (opt_key == "print_host")
            this->update_printhost_buttons();
        if (opt_key == "printhost_port")
            this->update_ports();
        if (opt_key == "bbl_use_print_host_webui")
            this->update_webui();
    };

    m_optgroup->append_single_option_line("host_type");

    auto create_sizer_with_btn = [](wxWindow* parent, Button** btn, const std::string& icon_name, const wxString& label) {
        *btn = new Button(parent, label);
        (*btn)->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);

        auto sizer = new wxBoxSizer(wxHORIZONTAL);
        sizer->Add(*btn);
        return sizer;
    };

    auto printhost_browse = [=](wxWindow* parent) 
    {
        auto sizer = create_sizer_with_btn(parent, &m_printhost_browse_btn, "printer_host_browser", _L("Browse") + " " + dots);
        m_printhost_browse_btn->Bind(wxEVT_BUTTON, [=](wxCommandEvent& e) {
            BonjourDialog dialog(this, Preset::printer_technology(*m_config));
            if (dialog.show_and_lookup()) {
                m_optgroup->set_value("print_host", dialog.get_selected(), true);
                m_optgroup->get_field("print_host")->field_changed();
            }
        });

        return sizer;
    };

    auto print_host_test = [=](wxWindow* parent) {
        auto sizer = create_sizer_with_btn(parent, &m_printhost_test_btn, "printer_host_test", _L("Test"));

        m_printhost_test_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
            std::unique_ptr<PrintHost> host(PrintHost::get_print_host(m_config));
            if (!host) {
                const wxString text = _L("Could not get a valid Printer Host reference");
                show_error(this, text);
                return;
            }

            wxString msg;
            bool result;
            {
                // Show a wait cursor during the connection test, as it is blocking UI.
                wxBusyCursor wait;
                result = host->test(msg);

                if (!result && host->is_cloud()) {
                    if (const auto h = dynamic_cast<SimplyPrint*>(host.get()); h) {
                        OAuthDialog dlg(this, h->get_oauth_params());
                        dlg.ShowModal();

                        const auto& r = dlg.get_result();
                        result = r.success;
                        if (r.success) {
                            h->save_oauth_credential(r);
                        } else {
                            msg = r.error_message;
                        }
                    } else {
                        PrinterCloudAuthDialog dlg(this->GetParent(), host.get());
                        dlg.ShowModal();
                        
                        const auto api_key = dlg.GetApiKey();
                        m_config->opt_string("printhost_apikey") = api_key;
                        result       = !api_key.empty();
                    }
                }
            }
            if (result)
                show_info(this, host->get_test_ok_msg(), _L("Success!"));
            else
                show_error(this, host->get_test_failed_msg(msg));

            update();
            });

        return sizer;
    };

    auto print_host_logout = [&](wxWindow* parent) {
        auto sizer = create_sizer_with_btn(parent, &m_printhost_logout_btn, "", _L("Log Out"));

        m_printhost_logout_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
            std::unique_ptr<PrintHost> host(PrintHost::get_print_host(m_config));
            if (!host) {
                const wxString text = _L("Could not get a valid Printer Host reference");
                show_error(this, text);
                return;
            }

            wxString msg_text = _L("Are you sure to log out?");
            MessageDialog dialog(this, msg_text, "", wxICON_QUESTION | wxYES_NO);

            if (dialog.ShowModal() == wxID_YES) {
                host->log_out();
                update();
            }
        });

        return sizer;
    };

    auto print_host_printers = [this, create_sizer_with_btn](wxWindow* parent) {
        //add_scaled_button(parent, &m_printhost_port_browse_btn, "browse", _(L("Refresh Printers")), wxBU_LEFT | wxBU_EXACTFIT);
        auto sizer = create_sizer_with_btn(parent, &m_printhost_port_browse_btn, "monitor_signal_strong", _L("Refresh") + " " + dots);
        Button* btn = m_printhost_port_browse_btn;
        btn->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
        btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent e) { update_printers(); });
        return sizer;
    };

    // Set a wider width for a better alignment
    Option option = m_optgroup->get_option("print_host");
    option.opt.width = Field::def_width_wider();
    Line host_line = m_optgroup->create_single_option_line(option);
    host_line.append_widget(printhost_browse);
    host_line.append_widget(print_host_test);
    host_line.append_widget(print_host_logout);
    m_optgroup->append_line(host_line);

    option = m_optgroup->get_option("print_host_webui");
    option.opt.width = Field::def_width_wider();
    m_optgroup->append_single_option_line(option);

    {
        // For bbl printers, we build a fake option to control whether the original device tab should be used
        ConfigOptionDef def;
        def.type     = coBool;
        def.width    = Field::def_width();
        def.label    = L("View print host webui in Device tab");
        def.tooltip  = L("Replace the BambuLab's device tab with print host webui");
        def.set_default_value(new ConfigOptionBool(false));

        auto option = Option(def, "bbl_use_print_host_webui");
        m_optgroup->append_single_option_line(option);
    }

    m_optgroup->append_single_option_line("printhost_authorization_type");

    option = m_optgroup->get_option("printhost_apikey");
    option.opt.width = Field::def_width_wider();
    m_optgroup->append_single_option_line(option);

    option = m_optgroup->get_option("printhost_port");
    option.opt.width = Field::def_width_wider();
    Line port_line = m_optgroup->create_single_option_line(option);
    port_line.append_widget(print_host_printers);
    m_optgroup->append_line(port_line);

    const auto ca_file_hint = _u8L("HTTPS CA file is optional. It is only needed if you use HTTPS with a self-signed certificate.");

    if (Http::ca_file_supported()) {
        option = m_optgroup->get_option("printhost_cafile");
        option.opt.width = Field::def_width_wider();
        Line cafile_line = m_optgroup->create_single_option_line(option);

        auto printhost_cafile_browse = [=](wxWindow* parent) {
            auto sizer = create_sizer_with_btn(parent, &m_printhost_cafile_browse_btn, "monitor_signal_strong", _L("Browse") + " " + dots);
            m_printhost_cafile_browse_btn->Bind(wxEVT_BUTTON, [this, m_optgroup](wxCommandEvent e) {
                static const auto filemasks = _L("Certificate files (*.crt, *.pem)|*.crt;*.pem|All files|*.*");
                wxFileDialog openFileDialog(this, _L("Open CA certificate file"), "", "", filemasks, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
                if (openFileDialog.ShowModal() != wxID_CANCEL) {
                    m_optgroup->set_value("printhost_cafile", openFileDialog.GetPath(), true);
                    m_optgroup->get_field("printhost_cafile")->field_changed();
                }
                });

            return sizer;
        };

        cafile_line.append_widget(printhost_cafile_browse);
        m_optgroup->append_line(cafile_line);

        Line cafile_hint{ "", "" };
        cafile_hint.full_width = 1;
        cafile_hint.widget = [ca_file_hint](wxWindow* parent) {
            auto txt = new wxStaticText(parent, wxID_ANY, from_u8(ca_file_hint));
            auto sizer = new wxBoxSizer(wxHORIZONTAL);
            sizer->Add(txt);
            return sizer;
        };
        m_optgroup->append_line(cafile_hint);
    }
    else {
        
        Line line{ "", "" };
        line.full_width = 1;

        line.widget = [ca_file_hint](wxWindow* parent) {
            std::string info = _u8L("HTTPS CA File") + ":\n\t" +
                (boost::format(_u8L("On this system, %s uses HTTPS certificates from the system Certificate Store or Keychain.")) % SLIC3R_APP_NAME).str() +
                "\n\t" + _u8L("To use a custom CA file, please import your CA file into Certificate Store / Keychain.");

            //auto txt = new wxStaticText(parent, wxID_ANY, from_u8((boost::format("%1%\n\n\t%2%") % info % ca_file_hint).str()));
            auto txt = new wxStaticText(parent, wxID_ANY, from_u8((boost::format("%1%\n\t%2%") % info % ca_file_hint).str()));
            txt->SetFont(wxGetApp().normal_font());
            auto sizer = new wxBoxSizer(wxHORIZONTAL);
            sizer->Add(txt, 1, wxEXPAND|wxALIGN_LEFT);
            return sizer;
        };
        m_optgroup->append_line(line);
    }

    for (const std::string& opt_key : std::vector<std::string>{ "printhost_user", "printhost_password" }) {        
        option = m_optgroup->get_option(opt_key);
        option.opt.width = Field::def_width_wider();
        m_optgroup->append_single_option_line(option);
    }

#ifdef WIN32
    option = m_optgroup->get_option("printhost_ssl_ignore_revoke");
    option.opt.width = Field::def_width_wider();
    m_optgroup->append_single_option_line(option);
#endif

    m_optgroup->activate();

    Field* printhost_field = m_optgroup->get_field("print_host");
    if (printhost_field)
    {
        wxTextCtrl* temp = dynamic_cast<wxTextCtrl*>(printhost_field->getWindow());
        if (temp)
            temp->Bind(wxEVT_TEXT, ([printhost_field, temp](wxEvent& e)
            {
#ifndef __WXGTK__
                e.Skip();
                temp->GetToolTip()->Enable(true);
#endif // __WXGTK__
                // Remove all leading and trailing spaces from the input
                std::string trimed_str, str = trimed_str = temp->GetValue().ToStdString();
                boost::trim(trimed_str);
                if (trimed_str != str)
                    temp->SetValue(trimed_str);

                TextCtrl* field = dynamic_cast<TextCtrl*>(printhost_field);
                if (field)
                    field->propagate_value();
            }), temp->GetId());
    }

    // Always fill in the "printhost_port" combo box from the config and select it.
    {
        Choice* choice = dynamic_cast<Choice*>(m_optgroup->get_field("printhost_port"));
        choice->set_values({ m_config->opt_string("printhost_port") });
        choice->set_selection();
    }

    update();
}

void PhysicalPrinterDialog::update_ports() {
    const PrinterTechnology tech = Preset::printer_technology(*m_config);
    if (tech == ptFFF) {
        const auto opt = m_config->option<ConfigOptionEnum<PrintHostType>>("host_type");
        if (opt->value == htObico) {
            auto build_web_ui = [](DynamicPrintConfig* config) {
                auto host = config->opt_string("print_host");
                auto port = config->opt_string("printhost_port");
                auto api_key = config->opt_string("printhost_apikey");
                if (host.empty() || port.empty()) {
                    return std::string();
                }
                boost::regex  re("\\[(\\d+)\\]");
                boost::smatch match;
                if (!boost::regex_search(port, match, re))
                    return std::string();
                if (match.size() <= 1) {
                    return std::string();
                }
                boost::format urlFormat("%1%/printers/%2%/control");
                urlFormat % host % match[1];
                return urlFormat.str();
            };
            auto url = build_web_ui(m_config);
            if (Field* print_host_webui_field = m_optgroup->get_field("print_host_webui"); print_host_webui_field) {
                if (TextInput* temp_input = dynamic_cast<TextInput*>(print_host_webui_field->getWindow()); temp_input) {
                    if (wxTextCtrl* temp = temp_input->GetTextCtrl()) {
                        temp->SetValue(wxString(url));
                        m_config->opt_string("print_host_webui") = url;
                    }
                }
            }
        }
    }
}

void PhysicalPrinterDialog::update_webui()
{
    const PrinterTechnology tech = Preset::printer_technology(*m_config);
    if (tech == ptFFF) {
        const auto opt = m_config->option<ConfigOptionEnum<PrintHostType>>("host_type");
        if (opt->value == htSimplyPrint) {
            bool bbl_use_print_host_webui = false;
            if (Field* printhost_webui_field = m_optgroup->get_field("bbl_use_print_host_webui"); printhost_webui_field) {
                if (CheckBox* temp = dynamic_cast<CheckBox*>(printhost_webui_field); temp) {
                    bbl_use_print_host_webui = boost::any_cast<bool>(temp->get_value());
                }
            }

            const std::string v = bbl_use_print_host_webui ? "https://simplyprint.io/panel" : "";
            if (Field* printhost_webui_field = m_optgroup->get_field("print_host_webui"); printhost_webui_field) {
                if (wxTextCtrl* temp = dynamic_cast<TextCtrl*>(printhost_webui_field)->text_ctrl(); temp) {
                    temp->SetValue(v);
                }
            }
            m_config->opt_string("print_host_webui") = v;
        }
    }
}

void PhysicalPrinterDialog::update_printhost_buttons()
{
    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(m_config));
    if (host) {
        m_printhost_test_btn->Enable(!m_config->opt_string("print_host").empty() && host->can_test());
        m_printhost_browse_btn->Show(host->has_auto_discovery());
        m_printhost_logout_btn->Show(host->is_logged_in());
        m_printhost_test_btn->SetLabel(host->is_cloud() ? _L("Login/Test") : _L("Test"));
    }
}

void PhysicalPrinterDialog::update_preset_input() {
    m_preset_name = into_u8(m_input_ctrl->GetValue());

    m_valid_type = Valid;
    wxString info_line;

    const char *unusable_symbols = "<>[]:/\\|?*\"";

    const std::string unusable_suffix = PresetCollection::get_suffix_modified(); //"(modified)";
    for (size_t i = 0; i < std::strlen(unusable_symbols); i++) {
        if (m_preset_name.find_first_of(unusable_symbols[i]) != std::string::npos) {
            info_line    = _L("Name is invalid;") + "\n" + _L("illegal characters:") + " " + unusable_symbols;
            m_valid_type = NoValid;
            break;
        }
    }


    if (m_valid_type == Valid && m_preset_name.find(unusable_suffix) != std::string::npos) {
        info_line    = _L("Name is invalid;") + "\n" + _L("illegal suffix:") + "\n\t" + from_u8(PresetCollection::get_suffix_modified());
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid &&
        (m_preset_name == "Default Setting" || m_preset_name == "Default Filament" || m_preset_name == "Default Printer")) {
        info_line    = _L("Name is unavailable.");
        m_valid_type = NoValid;
    }

    const Preset *existing = m_presets->find_preset(m_preset_name, false);
    if (m_valid_type == Valid && existing && (existing->is_default || existing->is_system)) {
        info_line = _L("Overwriting a system profile is not allowed.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && existing && m_preset_name != m_presets->get_selected_preset_name()) {
        if (existing->is_compatible)
            info_line = from_u8((boost::format(_u8L("Preset \"%1%\" already exists.")) % m_preset_name).str());
        else
            info_line = from_u8((boost::format(_u8L("Preset \"%1%\" already exists and is incompatible with the current printer.")) % m_preset_name).str());
        info_line += "\n" + _L("Please note that saving will overwrite this preset.");
        m_valid_type = Warning;
    }

    if (m_valid_type == Valid && m_preset_name.empty()) {
        info_line    = _L("The name is not allowed to be empty.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && m_preset_name.find_first_of(' ') == 0) {
        info_line    = _L("The name is not allowed to start with space character.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && m_preset_name.find_last_of(' ') == m_preset_name.length() - 1) {
        info_line    = _L("The name is not allowed to end with space character.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && m_presets->get_preset_name_by_alias(m_preset_name) != m_preset_name) {
        info_line    = _L("The name cannot be the same as a preset alias name.");
        m_valid_type = NoValid;
    }

    m_valid_label->SetLabel(info_line);
    m_input_area->Refresh();
    m_valid_label->Show(!info_line.IsEmpty());

    if (m_valid_type == NoValid) {
        if (btnOK)
            btnOK->Disable();
    }
    else {
        if (btnOK)
            btnOK->Enable();
    }
}

void PhysicalPrinterDialog::update(bool printer_change)
{
    m_optgroup->reload_config();

    const PrinterTechnology tech = Preset::printer_technology(*m_config);
    // Only offer the host type selection for FFF, for SLA it's always the SL1 printer (at the moment)
    bool supports_multiple_printers = false;
    if (tech == ptFFF) {
        update_host_type(printer_change);
        const auto opt = m_config->option<ConfigOptionEnum<PrintHostType>>("host_type");
        m_optgroup->show_field("host_type");

        m_optgroup->enable_field("print_host");
        m_optgroup->show_field("print_host_webui");
        m_optgroup->hide_field("bbl_use_print_host_webui");
        m_optgroup->enable_field("printhost_cafile");
        m_optgroup->enable_field("printhost_ssl_ignore_revoke");
        if (m_printhost_cafile_browse_btn)
            m_printhost_cafile_browse_btn->Enable();

        // hide pre-configured address, in case user switched to a different host type
        if (Field* printhost_field = m_optgroup->get_field("print_host"); printhost_field) {
            if (wxTextCtrl* temp = dynamic_cast<TextCtrl*>(printhost_field)->text_ctrl(); temp) {
                const auto current_host = temp->GetValue();
                if (current_host == L"https://connect.prusa3d.com" ||
                    current_host == L"https://app.obico.io" ||
                    current_host == "https://simplyprint.io" || current_host == "https://simplyprint.io/panel") {
                    temp->SetValue(wxString());
                    m_config->opt_string("print_host") = "";
                }
            }
        }
        if (opt->value == htPrusaLink) { // PrusaConnect does NOT allow http digest
            m_optgroup->show_field("printhost_authorization_type");
            AuthorizationType auth_type = m_config->option<ConfigOptionEnum<AuthorizationType>>("printhost_authorization_type")->value;
            m_optgroup->show_field("printhost_apikey", auth_type == AuthorizationType::atKeyPassword);
            for (const char* opt_key : { "printhost_user", "printhost_password" })
                m_optgroup->show_field(opt_key, auth_type == AuthorizationType::atUserPassword); 
        } else {
            m_optgroup->hide_field("printhost_authorization_type");
            m_optgroup->show_field("printhost_apikey", true);
            for (const std::string& opt_key : std::vector<std::string>{ "printhost_user", "printhost_password" })
                m_optgroup->hide_field(opt_key);
            supports_multiple_printers = opt->value == htRepetier || opt->value == htObico;

            if (opt->value == htPrusaConnect) { // automatically show default prusaconnect address
                if (Field* printhost_field = m_optgroup->get_field("print_host"); printhost_field) {
                    if (wxTextCtrl* temp = dynamic_cast<TextCtrl*>(printhost_field)->text_ctrl(); temp && temp->GetValue().IsEmpty()) {
                        temp->SetValue(L"https://connect.prusa3d.com");
                        m_config->opt_string("print_host") = "https://connect.prusa3d.com";
                    }
                }
            } else if (opt->value == htObico) { // automatically show default obico address
                if (Field* printhost_field = m_optgroup->get_field("print_host"); printhost_field) {
                    if (wxTextCtrl* temp = dynamic_cast<TextCtrl*>(printhost_field)->text_ctrl(); temp && temp->GetValue().IsEmpty()) {
                        temp->SetValue(L"https://app.obico.io");
                        m_config->opt_string("print_host") = "https://app.obico.io";
                    }
                }
            } else if (opt->value == htSimplyPrint) {
                // Set the host url
                if (Field* printhost_field = m_optgroup->get_field("print_host"); printhost_field) {
                    printhost_field->disable();
                    if (wxTextCtrl* temp = dynamic_cast<TextCtrl*>(printhost_field)->text_ctrl(); temp && temp->GetValue().IsEmpty()) {
                        temp->SetValue("https://simplyprint.io/panel");
                    }
                    m_config->opt_string("print_host") = "https://simplyprint.io/panel";
                }

                const auto current_webui = m_config->opt_string("print_host_webui");
                if (!current_webui.empty()) {
                    if (Field* printhost_webui_field = m_optgroup->get_field("print_host_webui"); printhost_webui_field) {
                        if (wxTextCtrl* temp = dynamic_cast<TextCtrl*>(printhost_webui_field)->text_ctrl(); temp) {
                            temp->SetValue("https://simplyprint.io/panel");
                        }
                    }
                    m_config->opt_string("print_host_webui") = "https://simplyprint.io/panel";
                }

                // For bbl printers, show option to control the device tab
                if (wxGetApp().preset_bundle->is_bbl_vendor()) {
                    m_optgroup->show_field("bbl_use_print_host_webui");
                    const bool use_print_host_webui = !current_webui.empty();
                    if (Field* printhost_webui_field = m_optgroup->get_field("bbl_use_print_host_webui"); printhost_webui_field) {
                        if (CheckBox* temp = dynamic_cast<CheckBox*>(printhost_webui_field); temp) {
                            temp->set_value(use_print_host_webui);
                        }
                    }
                }

                m_optgroup->hide_field("print_host_webui");
                m_optgroup->hide_field("printhost_apikey");
                m_optgroup->disable_field("printhost_cafile");
                m_optgroup->disable_field("printhost_ssl_ignore_revoke");
                if (m_printhost_cafile_browse_btn)
                    m_printhost_cafile_browse_btn->Disable();
            }
        }
        
        if (opt->value == htFlashforge) {
                m_optgroup->hide_field("printhost_apikey");
                m_optgroup->hide_field("printhost_authorization_type");
            }
    }
    else {
        m_optgroup->set_value("host_type", int(PrintHostType::htOctoPrint), false);
        m_optgroup->hide_field("host_type");

        m_optgroup->show_field("printhost_authorization_type");

        AuthorizationType auth_type = m_config->option<ConfigOptionEnum<AuthorizationType>>("printhost_authorization_type")->value;
        m_optgroup->show_field("printhost_apikey", auth_type == AuthorizationType::atKeyPassword);

        for (const char *opt_key : { "printhost_user", "printhost_password" })
            m_optgroup->show_field(opt_key, auth_type == AuthorizationType::atUserPassword);
    }

    m_optgroup->show_field("printhost_port", supports_multiple_printers);
    m_printhost_port_browse_btn->Show(supports_multiple_printers);

    update_preset_input();

    update_printhost_buttons();

    this->SetSize(this->GetBestSize());
    this->Layout();
}

void PhysicalPrinterDialog::update_host_type(bool printer_change)
{
    if (m_config == nullptr)
        return;
    Field* ht = m_optgroup->get_field("host_type");
    wxArrayString types;
    int last_in_conf = m_config->option("host_type")->getInt(); //  this is real position in last choice

    // Append localized enum_labels
    assert(ht->m_opt.enum_labels.size() == ht->m_opt.enum_values.size());
    for (size_t i = 0; i < ht->m_opt.enum_labels.size(); ++ i) {
        wxString label = _(ht->m_opt.enum_labels[i]);
        types.Add(label);
    }

    Choice* choice = dynamic_cast<Choice*>(ht);
    choice->set_values(types);
    int index_in_choice = (printer_change ? std::clamp(last_in_conf - ((int)ht->m_opt.enum_values.size() - (int)types.size()), 0, (int)ht->m_opt.enum_values.size() - 1) : last_in_conf);
    choice->set_value(index_in_choice);
    if ("prusalink" == ht->m_opt.enum_values.at(index_in_choice))
        m_config->set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(htPrusaLink));
    else if ("prusaconnect" == ht->m_opt.enum_values.at(index_in_choice))
        m_config->set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(htPrusaConnect));
    else {
        int host_type = std::clamp(index_in_choice + ((int)ht->m_opt.enum_values.size() - (int)types.size()), 0, (int)ht->m_opt.enum_values.size() - 1);
        PrintHostType type = static_cast<PrintHostType>(host_type);
        m_config->set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(type));
    }
}

void PhysicalPrinterDialog::update_printers()
{
    wxBusyCursor wait;

    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(m_config));

    wxArrayString printers;
    Field *rs = m_optgroup->get_field("printhost_port");
    try {
        if (! host->get_printers(printers))
            printers.clear();
    } catch (const HostNetworkError &err) {
        printers.clear();
        show_error(this, _L("Connection to printers connected via the print host failed.") + "\n\n" + from_u8(err.what()));
    }
    Choice *choice = dynamic_cast<Choice*>(rs);
    choice->set_values(printers);
    printers.empty() ? rs->disable() : rs->enable();
}

void PhysicalPrinterDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    const int& em = em_unit();

    m_printhost_browse_btn->Rescale();
    m_printhost_test_btn->Rescale();
    m_printhost_logout_btn->Rescale();
    if (m_printhost_cafile_browse_btn)
        m_printhost_cafile_browse_btn->Rescale();

    m_optgroup->msw_rescale();

    const wxSize& size = wxSize(45 * em, 35 * em);
    SetMinSize(size);

    Fit();
    Refresh();
}

void PhysicalPrinterDialog::check_host_key_valid()
{
    std::vector<std::string> keys = {"print_host", "print_host_webui", "printhost_apikey", "printhost_cafile", "printhost_user", "printhost_password", "printhost_port"};
    for (auto &key : keys) {
        auto it = m_config->option<ConfigOptionString>(key);
        if (!it) m_config->set_key_value(key, new ConfigOptionString(""));
    }
    return;
}

void PhysicalPrinterDialog::OnOK(wxEvent& event)
{
    wxGetApp().get_tab(Preset::TYPE_PRINTER)->save_preset("", false, false, true, m_preset_name );
    event.Skip();
}

}}    // namespace Slic3r::GUI
