#include "ConnectPrinter.hpp"
#include "GUI_App.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/DeviceCore/DevConfigUtil.h"
#include <slic3r/GUI/I18N.hpp>
#include <slic3r/GUI/Widgets/Button.hpp>
#include <slic3r/GUI/Widgets/Label.hpp>
#include <wx/string.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "slic3r/GUI/Mcp/WindowNames.hpp"
#include <wx/event.h>
#include <cstddef>
#include <string>
#include "slic3r/GUI/wxExtensions.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "DeviceCore/DevManager.h"

namespace Slic3r { namespace GUI {

namespace {
// Trim ASCII whitespace from both ends.
std::string trim_copy(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Split "[scheme://]host[:port][/path]" into its parts (empty parts when absent).
void split_host_port(const std::string& input, std::string& scheme, std::string& host, std::string& port)
{
    scheme.clear();
    host.clear();
    port.clear();
    std::string rest = input;
    if (rest.rfind("http://", 0) == 0) {
        scheme = "http://";
        rest = rest.substr(7);
    } else if (rest.rfind("https://", 0) == 0) {
        scheme = "https://";
        rest = rest.substr(8);
    }
    size_t slash = rest.find('/');
    if (slash != std::string::npos)
        rest = rest.substr(0, slash);
    size_t colon = rest.find(':');
    if (colon != std::string::npos) {
        port = rest.substr(colon + 1);
        rest = rest.substr(0, colon);
    }
    host = rest;
}
} // namespace

ConnectPrinterDialog::ConnectPrinterDialog(wxWindow *parent, wxWindowID id, const wxString &title, const wxPoint &pos, const wxSize &size, long style)
    : DPIDialog(parent, id, _L("Connect Printer (LAN)"), pos, size, style)
{
    SetBackgroundColour(*wxWHITE);
    this->SetSizeHints(wxDefaultSize, wxDefaultSize);

    wxBoxSizer *main_sizer;
    main_sizer = new wxBoxSizer(wxHORIZONTAL);

    main_sizer->Add(FromDIP(40), 0);

    wxBoxSizer *sizer_top;
    sizer_top = new wxBoxSizer(wxVERTICAL);
    m_sizer_top = sizer_top;

    sizer_top->Add(0, FromDIP(40));

    m_staticText_connection_code = new wxStaticText(this, wxID_ANY, _L("Please input the printer access code:"), wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_connection_code->SetFont(Label::Body_15);
    m_staticText_connection_code->SetForegroundColour(wxColour(50, 58, 61));
    m_staticText_connection_code->Wrap(-1);
    sizer_top->Add(m_staticText_connection_code, 0, wxALL, 0);

    sizer_top->Add(0, FromDIP(10));
	
    wxBoxSizer *sizer_connect;
    sizer_connect = new wxBoxSizer(wxHORIZONTAL);

    m_textCtrl_code = new TextInput(this, wxEmptyString);
    // OrcaSonar uses a 12-character base32 access code. Keep this field long
    // enough for it while retaining the existing validation for LAN codes.
    m_textCtrl_code->GetTextCtrl()->SetMaxLength(12);
    m_textCtrl_code->SetName(Mcp::kSecretField);
    m_textCtrl_code->SetFont(Label::Body_14);
    m_textCtrl_code->SetCornerRadius(FromDIP(5));
    m_textCtrl_code->SetSize(wxSize(FromDIP(330), FromDIP(40)));
    m_textCtrl_code->SetMinSize(wxSize(FromDIP(330), FromDIP(40)));
    m_textCtrl_code->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(22)));
    m_textCtrl_code->GetTextCtrl()->SetMinSize(wxSize(-1, FromDIP(22)));
    m_textCtrl_code->SetBackgroundColour(*wxWHITE);
    m_textCtrl_code->GetTextCtrl()->SetForegroundColour(wxColour(107, 107, 107));
    sizer_connect->Add(m_textCtrl_code, 0, wxALL | wxALIGN_CENTER_VERTICAL, 0);

    sizer_connect->Add(FromDIP(20), 0);

    m_button_confirm = new Button(this, _L("Confirm"));
    m_button_confirm->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);

    sizer_connect->Add(m_button_confirm, 0, wxALL | wxALIGN_CENTER_VERTICAL, 0);
    
    sizer_connect->Add(FromDIP(60), 0);

    sizer_top->Add(sizer_connect);

    sizer_top->Add(0, FromDIP(35));

    m_staticText_hints = new wxStaticText(this, wxID_ANY, _L("You can find it in \"Settings > Network > Access code\"\non the printer, as shown in the figure:"), wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_hints->SetFont(Label::Body_15);
    m_staticText_hints->SetForegroundColour(wxColour(50, 58, 61));
    m_staticText_hints->Wrap(-1);
    sizer_top->Add(m_staticText_hints, 0, wxALL, 0);

    sizer_top->Add(0, FromDIP(25));

    wxBoxSizer *sizer_diagram;
    sizer_diagram = new wxBoxSizer(wxHORIZONTAL);

    m_bitmap_diagram = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(340), -1), 0);
    m_bitmap_diagram->SetBitmap(m_diagram_img);
    sizer_diagram->Add(m_bitmap_diagram);

    sizer_top->Add(sizer_diagram);

    sizer_top->Add(0, FromDIP(40), 0, wxEXPAND, 0);

    main_sizer->Add(sizer_top);

    this->SetSizerAndFit(main_sizer);
    this->Layout();
    CentreOnParent();

    m_textCtrl_code->Bind(wxEVT_TEXT, &ConnectPrinterDialog::on_input_enter, this);
    m_button_confirm->Bind(wxEVT_BUTTON, &ConnectPrinterDialog::on_button_confirm, this);
    wxGetApp().UpdateDlgDarkUI(this);
}

ConnectPrinterDialog::~ConnectPrinterDialog() {}

void ConnectPrinterDialog::end_modal(wxStandardID id)
{
    EndModal(id);
}

void ConnectPrinterDialog::init_bitmap()
{
    AppConfig *config = get_app_config();
    std::string language = config->get("language");

    if (m_obj) {
        std::string img_str = DevPrinterConfigUtil::get_printer_connect_help_img(m_obj->printer_type);
        if(img_str.empty()){img_str = "input_access_code_x1"; }

        if (language == "zh_CN") {
            m_diagram_bmp = create_scaled_bitmap(img_str+"_cn", nullptr, 190);
        }
        else {
            m_diagram_bmp = create_scaled_bitmap(img_str+"_en", nullptr, 190);
        }

        // traverse the guide text
        {
            // traverse the guide text
            if (m_obj->printer_type == "O1D")
            {
                m_staticText_hints->SetLabel(_L("You can find it in \"Setting > Setting > LAN only > Access Code\"\non the printer, as shown in the figure:"));
            }
            else
            {
                m_staticText_hints->SetLabel(_L("You can find it in \"Settings > Network > Access code\"\non the printer, as shown in the figure:"));
            }
        }
    }
    else{
        if (language == "zh_CN") {
            m_diagram_bmp = create_scaled_bitmap("input_access_code_x1_cn", nullptr, 190);
        }
        else {
            m_diagram_bmp = create_scaled_bitmap("input_access_code_x1_en", nullptr, 190);
        }
    }
    m_diagram_img = m_diagram_bmp.ConvertToImage();
    auto bmp_size = m_diagram_bmp.GetSize();
    float scale = (float)FromDIP(340) / (float)bmp_size.x;
    m_diagram_img.Rescale(FromDIP(340), bmp_size.y * scale);
    m_bitmap_diagram->SetBitmap(m_diagram_img);
    Fit();
}

void ConnectPrinterDialog::set_machine_object(MachineObject* obj)
{
    m_obj = obj;
    init_bitmap();
}

void ConnectPrinterDialog::enable_add_by_ip()
{
    if (m_add_by_ip || !m_sizer_top)
        return;
    m_add_by_ip = true;

    SetTitle(_L("Add printer by IP"));

    auto make_label = [this](const wxString& text) {
        wxStaticText* lbl = new wxStaticText(this, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, 0);
        lbl->SetFont(Label::Body_15);
        lbl->SetForegroundColour(wxColour(50, 58, 61));
        lbl->Wrap(-1);
        return lbl;
    };
    auto make_input = [this]() {
        TextInput* ti = new TextInput(this, wxEmptyString);
        ti->SetFont(Label::Body_14);
        ti->SetCornerRadius(FromDIP(5));
        ti->SetSize(wxSize(FromDIP(330), FromDIP(40)));
        ti->SetMinSize(wxSize(FromDIP(330), FromDIP(40)));
        ti->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(22)));
        ti->GetTextCtrl()->SetMinSize(wxSize(-1, FromDIP(22)));
        ti->SetBackgroundColour(*wxWHITE);
        ti->GetTextCtrl()->SetForegroundColour(wxColour(107, 107, 107));
        return ti;
    };

    // IP row (inserted above the access-code label).
    m_staticText_host = make_label(_L("Printer IP address (or IP:port):"));
    m_sizer_top->Insert(1, m_staticText_host, 0, wxALL, 0);
    m_sizer_top->Insert(2, FromDIP(10), 0);
    m_textCtrl_host = make_input();
    m_sizer_top->Insert(3, m_textCtrl_host, 0, wxALL, 0);
    m_sizer_top->Insert(4, FromDIP(10), 0);

    // Port + printer agent row.
    wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
    m_staticText_port = make_label(_L("Port:"));
    row->Add(m_staticText_port, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_textCtrl_port = make_input();
    m_textCtrl_port->SetSize(wxSize(FromDIP(90), FromDIP(40)));
    m_textCtrl_port->SetMinSize(wxSize(FromDIP(90), FromDIP(40)));
    row->Add(m_textCtrl_port, 0, wxALIGN_CENTER_VERTICAL);
    row->Add(FromDIP(30), 0);
    m_staticText_agent = make_label(_L("Printer agent:"));
    row->Add(m_staticText_agent, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_choice_agent = new wxChoice(this, wxID_ANY);
    row->Add(m_choice_agent, 0, wxALIGN_CENTER_VERTICAL);
    m_sizer_top->Insert(5, row, 0, wxALL | wxEXPAND, 0);

    // Prefill from the currently edited printer preset.
    std::string host_raw, port, code, stored_agent;
    if (wxGetApp().preset_bundle) {
        DynamicPrintConfig* cfg = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
        host_raw = cfg->opt_string("print_host");
        port     = cfg->opt_string("printhost_port");
        code     = cfg->opt_string("printhost_apikey");
        stored_agent = cfg->opt_string("printer_agent");
    }
    std::string scheme, host, inline_port;
    split_host_port(trim_copy(host_raw), scheme, host, inline_port);
    if (!inline_port.empty())
        port = inline_port;
    if (trim_copy(port).empty())
        port = "80";
    m_textCtrl_host->GetTextCtrl()->SetValue(wxString::FromUTF8((scheme + host).c_str()));
    m_textCtrl_port->GetTextCtrl()->SetValue(wxString::FromUTF8(port.c_str()));
    if (!code.empty() && code != "88888888")
        m_textCtrl_code->GetTextCtrl()->SetValue(wxString::FromUTF8(code.c_str()));

    // Agent combo: explicitly shows the preset's agent (resolved to a registered id).
    const std::string wanted = wxGetApp().resolve_printer_agent_id(stored_agent);
    int               sel    = -1;
    for (const PrinterAgentInfo& agent : NetworkAgentFactory::get_registered_printer_agents()) {
        m_choice_agent->Append(wxString::FromUTF8(agent.display_name.c_str()));
        m_agent_ids.push_back(agent.id);
        if (agent.id == wanted)
            sel = (int) m_agent_ids.size() - 1;
    }
    if (sel < 0 && !m_agent_ids.empty())
        sel = 0;
    if (sel >= 0)
        m_choice_agent->SetSelection(sel);

    // The access-code help text/diagram belongs to the discovery flow; keep this form compact.
    m_staticText_hints->Hide();
    m_bitmap_diagram->Hide();

    Layout();
    Fit();
    CentreOnParent();
}

bool ConnectPrinterDialog::apply_ip_settings(const std::string& effective_code, const std::string& agent_id)
{
    std::string input_host = trim_copy(m_textCtrl_host->GetTextCtrl()->GetValue().ToStdString());
    std::string port       = trim_copy(m_textCtrl_port->GetTextCtrl()->GetValue().ToStdString());

    if (input_host.empty()) {
        show_error(this, _L("Please input the printer IP address"));
        return false;
    }
    for (char c : input_host) {
        if (c == ' ' || c == '\t') {
            show_error(this, _L("Invalid input"));
            return false;
        }
    }

    std::string scheme, host, inline_port;
    split_host_port(input_host, scheme, host, inline_port);
    if (!inline_port.empty())
        port = inline_port;
    if (host.empty()) {
        show_error(this, _L("Please input the printer IP address"));
        return false;
    }
    if (port.empty())
        port = "80";
    long port_value = 0;
    for (char c : port) {
        if (c < '0' || c > '9') {
            port_value = -1;
            break;
        }
        port_value = port_value * 10 + (c - '0');
        if (port_value > 65535)
            break;
    }
    if (port_value < 1 || port_value > 65535) {
        show_error(this, _L("Invalid port number"));
        return false;
    }

    const std::string print_host = scheme + host;

    // Store the address in the edited printer preset, the same way PhysicalPrinterDialog
    // edits it (it points ConfigOptionsGroup at this very config).
    if (!wxGetApp().preset_bundle)
        return false;
    DynamicPrintConfig* cfg = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
    auto assign = [cfg](const std::string& key, const std::string& value) {
        if (cfg->has(key))
            cfg->opt_string(key) = value;
        else
            cfg->set_key_value(key, new ConfigOptionString(value));
    };
    assign("print_host", print_host);
    assign("printhost_port", port);
    assign("printhost_apikey", effective_code);
    assign("printer_agent", agent_id);

    // An earlier attempt may already have inserted this device with a wrong code;
    // select_machine() only refreshes the code for moonraker, so do it here for all agents.
    if (DeviceManager* dev = wxGetApp().getDeviceManager()) {
        const std::string dev_id = MachineObject::dev_id_from_address(print_host, port);
        if (MachineObject* existing = dev->get_local_machine(dev_id))
            existing->set_access_code(effective_code);
    }

    // Standard flow: installs the preset's agent if it changed, creates the device entry
    // (insert_local_device), selects it and starts the connection.
    wxGetApp().switch_printer_agent_if_pending();
    wxGetApp().select_machine(agent_id);
    return true;
}

void ConnectPrinterDialog::on_input_enter(wxCommandEvent& evt)
{
    m_input_access_code = evt.GetString();
}


void ConnectPrinterDialog::on_button_confirm(wxCommandEvent &event)
{
    wxString code = m_textCtrl_code->GetTextCtrl()->GetValue();

    // Agent context: the explicit combo selection in add-by-IP mode, the machine's own
    // agent in the classic access-code flow (empty when no machine is bound yet).
    std::string agent_id;
    if (m_add_by_ip) {
        if (!m_agent_ids.empty()) {
            int sel = m_choice_agent ? m_choice_agent->GetSelection() : (int) wxNOT_FOUND;
            if (sel == wxNOT_FOUND || sel < 0 || sel >= (int) m_agent_ids.size()) {
                show_error(this, _L("Invalid input"));
                return;
            }
            agent_id = m_agent_ids[sel];
        } else {
            agent_id = wxGetApp().resolve_printer_agent_id(std::string());
        }
    } else if (m_obj) {
        agent_id = m_obj->printer_agent_id;
    }

    if (code.empty() && agent_id != MOONRAKER_PRINTER_AGENT_ID)
        code = "88888888";
    for (char c : code) {
        if (!(('0' <= c && c <= '9') || ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z'))) {
            show_error(this, _L("Invalid input"));
            return;
        }
    }

    if (m_add_by_ip) {
        if (!apply_ip_settings(code.ToStdString(), agent_id))
            return;
    }

    if (m_obj) {
        m_obj->set_access_code(code.ToStdString());
    }
    EndModal(wxID_OK);
}

void ConnectPrinterDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    init_bitmap();
    m_bitmap_diagram->SetBitmap(m_diagram_img);
    m_textCtrl_code->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(22)));
    m_textCtrl_code->GetTextCtrl()->SetMinSize(wxSize(-1, FromDIP(22)));
    if (m_textCtrl_host) {
        m_textCtrl_host->SetSize(wxSize(FromDIP(330), FromDIP(40)));
        m_textCtrl_host->SetMinSize(wxSize(FromDIP(330), FromDIP(40)));
        m_textCtrl_host->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(22)));
        m_textCtrl_host->GetTextCtrl()->SetMinSize(wxSize(-1, FromDIP(22)));
    }
    if (m_textCtrl_port) {
        m_textCtrl_port->SetSize(wxSize(FromDIP(90), FromDIP(40)));
        m_textCtrl_port->SetMinSize(wxSize(FromDIP(90), FromDIP(40)));
        m_textCtrl_port->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(22)));
        m_textCtrl_port->GetTextCtrl()->SetMinSize(wxSize(-1, FromDIP(22)));
    }

    m_button_confirm->Rescale(); // ORCA No need to set style again
    
    Layout();
    this->Refresh();
}
}} // namespace Slic3r::GUI