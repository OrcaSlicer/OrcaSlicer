#include "SideTools.hpp"
#include "bambu_networking.hpp"
#include <wx/dcmemory.h>
#include <wx/dcgraph.h>
#include "Label.hpp"
#include "StateColor.hpp"
#include "../GUI_App.hpp"
#include "../wxExtensions.hpp"
#include "../I18N.hpp"
#include "../GUI.hpp"

namespace Slic3r { namespace GUI {
	SideToolsPanel::SideToolsPanel(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size)
{
    wxPanel::Create(parent, id, pos, size);

    SetMinSize(wxSize(-1, FromDIP(50)));
    SetMaxSize(wxSize(-1, FromDIP(50)));

    Bind(wxEVT_PAINT, &SideToolsPanel::OnPaint, this);
    SetBackgroundColour(wxColour("#FEFFFF"));

    m_printing_img = ScalableBitmap(this, "printer", 16);
    m_arrow_img    = ScalableBitmap(this, "monitor_arrow", 14);

    m_none_printing_img = ScalableBitmap(this, "tab_monitor_active", 20); // ORCA match icon size with exact resolution to fix blurry icon
    m_none_arrow_img    = ScalableBitmap(this, "monitor_none_arrow", 14);
    m_none_add_img      = ScalableBitmap(this, "monitor_none_add", 14);

    m_wifi_none_img     = ScalableBitmap(this, "monitor_signal_no", 18);
    m_wifi_weak_img     = ScalableBitmap(this, "monitor_signal_weak", 18);
    m_wifi_middle_img   = ScalableBitmap(this, "monitor_signal_middle", 18);
    m_wifi_strong_img   = ScalableBitmap(this, "monitor_signal_strong", 18);
    m_network_wired_img = ScalableBitmap(this, "monitor_network_wired", 18);

    m_intetval_timer = new wxTimer();
    m_intetval_timer->SetOwner(this);

    this->Bind(wxEVT_TIMER, &SideToolsPanel::stop_interval, this);
    this->Bind(wxEVT_ENTER_WINDOW, &SideToolsPanel::on_mouse_enter, this);
    this->Bind(wxEVT_LEAVE_WINDOW, &SideToolsPanel::on_mouse_leave, this);
    this->Bind(wxEVT_LEFT_DOWN, &SideToolsPanel::on_mouse_left_down, this);
    this->Bind(wxEVT_LEFT_UP, &SideToolsPanel::on_mouse_left_up, this);
}

SideToolsPanel::~SideToolsPanel() { delete m_intetval_timer; }

void SideToolsPanel::set_none_printer_mode()
{
    m_none_printer = true;
    Refresh();
}

void SideToolsPanel::on_timer(wxTimerEvent &event)
{
}

void SideToolsPanel::set_current_printer_name(std::string dev_name)
{
     if (m_dev_name == from_u8(dev_name) && !m_none_printer) return;

     m_none_printer = false;
     m_dev_name     = from_u8(dev_name);
     Refresh();
}

void SideToolsPanel::set_current_printer_signal(WifiSignal sign)
{
     if (last_printer_signal == sign && !m_none_printer) return;

     last_printer_signal = sign;
     m_none_printer = false;
     m_wifi_type    = sign;
     Refresh();
}

void SideToolsPanel::start_interval()
{
    m_intetval_timer->Start(SIDE_TOOL_CLICK_INTERVAL);
    m_is_in_interval = true;
}

void SideToolsPanel::stop_interval(wxTimerEvent& event)
{
    m_is_in_interval = false;
    m_intetval_timer->Stop();
}


bool SideToolsPanel::is_in_interval()
{
    return m_is_in_interval;
}

void SideToolsPanel::msw_rescale()
{
    m_printing_img.msw_rescale();
    m_arrow_img.msw_rescale();

    m_none_printing_img.msw_rescale();
    m_none_arrow_img.msw_rescale();
    m_none_add_img.msw_rescale();

    m_wifi_none_img.msw_rescale();
    m_wifi_weak_img.msw_rescale();
    m_wifi_middle_img.msw_rescale();
    m_wifi_strong_img.msw_rescale();

    Refresh();
}

void SideToolsPanel::OnPaint(wxPaintEvent &event)
{
    wxPaintDC dc(this);
    doRender(dc);
}

void SideToolsPanel::render(wxDC &dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({0, 0}, size, &dc, {0, 0});

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif
}

void SideToolsPanel::doRender(wxDC &dc)
{
    auto   left = FromDIP(15);
    wxSize size = GetSize();

    //if (m_none_printer) {
    //    dc.SetPen(SIDE_TOOLS_LIGHT_GREEN);
    //    dc.SetBrush(SIDE_TOOLS_LIGHT_GREEN);
    //    dc.DrawRectangle(0, 0, size.x, size.y);
    //}

    if (m_none_printer) {
        dc.SetPen(StateColor::darkModeColorFor(SIDE_TOOLS_BRAND));   // ORCA: Sidebar header background color - Fix for dark mode compability
        dc.SetBrush(StateColor::darkModeColorFor(SIDE_TOOLS_BRAND)); // ORCA: Sidebar header background color - Fix for dark mode compability
        dc.DrawRectangle(0, 0, size.x, size.y);

        dc.DrawBitmap(m_none_printing_img.bmp(), left, (size.y - m_none_printing_img.GetBmpSize().y) / 2);

        left += (m_none_printing_img.GetBmpSize().x + FromDIP(15));
        dc.DrawBitmap(m_none_arrow_img.bmp(), left, (size.y - m_none_arrow_img.GetBmpSize().y) / 2);

        left += (m_none_arrow_img.GetBmpSize().x + FromDIP(6));
        dc.SetFont(::Label::Body_14);
        dc.SetBackgroundMode(wxTRANSPARENT);
        dc.SetTextForeground(*wxWHITE);

        wxString no_printer_str = _L("No printer");
        auto sizet = dc.GetTextExtent(no_printer_str);
        auto left_add_bitmap = size.x - FromDIP(30) - m_wifi_none_img.GetBmpSize().x - m_none_add_img.GetBmpSize().x;
        auto size_width = left_add_bitmap - left;

        if (sizet.x > size_width) {
            wxString temp_str = wxEmptyString;
            for (auto i = 0; i < no_printer_str.Len(); i++) {
                if (dc.GetTextExtent(L("...") + temp_str).x < size_width) {
                    temp_str += no_printer_str[i];
                }
                else {
                    break;
                }
            }

            no_printer_str = temp_str + L("...");
        }

        dc.DrawText(no_printer_str, wxPoint(left, (size.y - sizet.y) / 2));

        left = size.x - FromDIP(30) - m_wifi_none_img.GetBmpSize().x;
        dc.DrawBitmap(m_none_add_img.bmp(), left, (size.y - m_none_add_img.GetBmpSize().y) / 2);
    } else {
        dc.DrawBitmap(m_printing_img.bmp(), left, (size.y - m_printing_img.GetBmpSize().y) / 2);

        left += (m_printing_img.GetBmpSize().x + FromDIP(5));
        dc.DrawBitmap(m_arrow_img.bmp(), left, (size.y - m_arrow_img.GetBmpSize().y) / 2);

        left += (m_arrow_img.GetBmpSize().x + FromDIP(6));
        dc.SetFont(::Label::Body_14);
        dc.SetBackgroundMode(wxTRANSPARENT);
        dc.SetTextForeground(StateColor::darkModeColorFor(SIDE_TOOLS_GREY900));

        auto sizet = dc.GetTextExtent(m_dev_name);
        auto text_end = size.x - m_wifi_none_img.GetBmpSize().x - 20;

        wxString finally_name = m_dev_name;
        if (sizet.x > (text_end - left)) {
            auto limit_width = text_end - left - dc.GetTextExtent("...").x - 20;
            for (auto i = 0; i < m_dev_name.length(); i++) {
                auto curr_width = dc.GetTextExtent(m_dev_name.substr(0, i));
                if (curr_width.x >= limit_width) {
                    finally_name = m_dev_name.substr(0, i) + "...";
                    break;
                }
            }
        }


        dc.DrawText(finally_name, wxPoint(left, (size.y - sizet.y) / 2));

        left = size.x - FromDIP(18) - m_wifi_none_img.GetBmpSize().x;
        if (m_wifi_type == WifiSignal::NONE) dc.DrawBitmap(m_wifi_none_img.bmp(), left, (size.y - m_wifi_none_img.GetBmpSize().y) / 2);
        if (m_wifi_type == WifiSignal::WEAK) dc.DrawBitmap(m_wifi_weak_img.bmp(), left, (size.y - m_wifi_weak_img.GetBmpSize().y) / 2);
        if (m_wifi_type == WifiSignal::MIDDLE) dc.DrawBitmap(m_wifi_middle_img.bmp(), left, (size.y - m_wifi_middle_img.GetBmpSize().y) / 2);
        if (m_wifi_type == WifiSignal::STRONG) dc.DrawBitmap(m_wifi_strong_img.bmp(), left, (size.y - m_wifi_strong_img.GetBmpSize().y) / 2);
        if (m_wifi_type == WifiSignal::WIRED)  dc.DrawBitmap(m_network_wired_img.bmp(), left, (size.y - m_network_wired_img.GetBmpSize().y) / 2);
    }

    if (m_hover) {
        dc.SetPen(SIDE_TOOLS_BRAND);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, size.x, size.y);
    }
}

void SideToolsPanel::on_mouse_left_down(wxMouseEvent &evt)
{
    m_click = true;
    Refresh();
}

void SideToolsPanel::on_mouse_left_up(wxMouseEvent &evt)
{
     m_click = false;
     Refresh();
}

void SideToolsPanel::on_mouse_enter(wxMouseEvent &evt)
{
    m_hover = true;
    Refresh();
}

void SideToolsPanel::on_mouse_leave(wxMouseEvent &evt)
{
    m_hover = false;
    Refresh();
}

SideTools::SideTools(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size)
{
    wxPanel::Create(parent, id, pos, size);
    SetBackgroundColour(wxColour("#FEFFFF"));

    m_side_tools = new SideToolsPanel(this, wxID_ANY);

    m_connection_info = new Button(this, wxEmptyString);
    m_connection_info->SetBackgroundColor(wxColour(255, 111, 0));
    m_connection_info->SetBorderColor(wxColour(255, 111, 0));
    m_connection_info->SetTextColor(*wxWHITE);
    m_connection_info->SetFont(::Label::Body_13);
    m_connection_info->SetCornerRadius(0);
    m_connection_info->SetSize(wxSize(FromDIP(-1), FromDIP(25)));
    m_connection_info->SetMinSize(wxSize(FromDIP(-1), FromDIP(25)));
    m_connection_info->SetMaxSize(wxSize(FromDIP(-1), FromDIP(25)));


    wxBoxSizer* connection_sizer_V = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* connection_sizer_H = new wxBoxSizer(wxHORIZONTAL);

    // ORCA standardized HyperLink
    m_hyperlink = new HyperLink(m_connection_info, _L("Failed to connect to the server"), wxT("https://wiki.bambulab.com/en/software/bambu-studio/failed-to-connect-printer"));

    m_more_err_open = ScalableBitmap(this, "monitir_err_open", 16);
    m_more_err_close = ScalableBitmap(this, "monitir_err_close", 16);
    m_more_button = new ScalableButton(m_connection_info, wxID_ANY, "monitir_err_open");
    m_more_button->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_HAND); });
    m_more_button->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_ARROW); });
    m_more_button->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        if (!m_more_err_state) {
            m_more_button->SetBitmap(m_more_err_close.bmp());
            Freeze();
            m_side_error_panel->Show();
            m_more_err_state = true;
            m_tabpanel->Refresh();
            m_tabpanel->Layout();
            Thaw();
        }
        else {
            m_more_button->SetBitmap(m_more_err_open.bmp());
            Freeze();
            m_side_error_panel->Hide();
            m_more_err_state = false;
            m_tabpanel->Refresh();
            m_tabpanel->Layout();
            Thaw();
        }

        });

    connection_sizer_H->Add(m_hyperlink, 0, wxALIGN_CENTER | wxALL, 5);
    connection_sizer_H->Add(m_more_button, 0, wxALIGN_CENTER | wxALL, 3);
    connection_sizer_V->Add(connection_sizer_H, 0, wxALIGN_CENTER, 0);

    m_connection_info->SetSizer(connection_sizer_V);
    m_connection_info->Layout();
    connection_sizer_V->Fit(m_connection_info);

    m_side_error_panel = new wxWindow(this, wxID_ANY);
    m_side_error_panel->SetBackgroundColour(wxColour(255, 232, 214));
    m_side_error_panel->SetMinSize(wxSize(-1, -1));
    m_side_error_panel->SetMaxSize(wxSize(-1, -1));

    m_side_error_panel->Hide();
    m_more_button->Hide();
    m_connection_info->Hide();

    wxBoxSizer* sizer_print_failed_info = new wxBoxSizer(wxVERTICAL);
    m_side_error_panel->SetSizer(sizer_print_failed_info);

    wxBoxSizer* sizer_error_code = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* sizer_error_desc = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* sizer_extra_info = new wxBoxSizer(wxHORIZONTAL);

    // ORCA standardized HyperLink
    m_link_network_state = new HyperLink(m_side_error_panel, _L("Check the status of current system services"), wxGetApp().link_to_network_check(), wxST_ELLIPSIZE_END);
    m_link_network_state->SetMaxSize(wxSize(FromDIP(220), -1));
    m_link_network_state->SetFont(::Label::Body_12);

    auto st_title_error_code = new wxStaticText(m_side_error_panel, wxID_ANY, _L("code"), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    auto st_title_error_code_doc = new wxStaticText(m_side_error_panel, wxID_ANY, ": ");
    m_st_txt_error_code = new Label(m_side_error_panel, wxEmptyString, LB_AUTO_WRAP);
    st_title_error_code->SetForegroundColour(0x909090);
    st_title_error_code_doc->SetForegroundColour(0x909090);
    m_st_txt_error_code->SetForegroundColour(0x909090);
    st_title_error_code->SetFont(::Label::Body_12);
    st_title_error_code_doc->SetFont(::Label::Body_12);
    m_st_txt_error_code->SetFont(::Label::Body_12);
    st_title_error_code->SetMinSize(wxSize(FromDIP(32), -1));
    st_title_error_code->SetMaxSize(wxSize(FromDIP(32), -1));
    m_st_txt_error_code->SetMinSize(wxSize(FromDIP(175), -1));
    m_st_txt_error_code->SetMaxSize(wxSize(FromDIP(175), -1));
    sizer_error_code->Add(st_title_error_code, 0, wxALL, 0);
    sizer_error_code->Add(st_title_error_code_doc, 0, wxALL, 0);
    sizer_error_code->Add(m_st_txt_error_code, 0, wxALL, 0);


    auto st_title_error_desc = new wxStaticText(m_side_error_panel, wxID_ANY, wxT("desc"), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    auto st_title_error_desc_doc = new wxStaticText(m_side_error_panel, wxID_ANY, ": ");
    m_st_txt_error_desc = new Label(m_side_error_panel, wxEmptyString, LB_AUTO_WRAP);
    st_title_error_desc->SetForegroundColour(0x909090);
    st_title_error_desc_doc->SetForegroundColour(0x909090);
    m_st_txt_error_desc->SetForegroundColour(0x909090);
    st_title_error_desc->SetFont(::Label::Body_12);
    st_title_error_desc_doc->SetFont(::Label::Body_12);
    m_st_txt_error_desc->SetFont(::Label::Body_12);
    st_title_error_desc->SetMinSize(wxSize(FromDIP(32), -1));
    st_title_error_desc->SetMaxSize(wxSize(FromDIP(32), -1));
    m_st_txt_error_desc->SetMinSize(wxSize(FromDIP(175), -1));
    m_st_txt_error_desc->SetMaxSize(wxSize(FromDIP(175), -1));
    sizer_error_desc->Add(st_title_error_desc, 0, wxALL, 0);
    sizer_error_desc->Add(st_title_error_desc_doc, 0, wxALL, 0);
    sizer_error_desc->Add(m_st_txt_error_desc, 0, wxALL, 0);

    auto st_title_extra_info = new wxStaticText(m_side_error_panel, wxID_ANY, wxT("info"), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    auto st_title_extra_info_doc = new wxStaticText(m_side_error_panel, wxID_ANY, ": ");
    m_st_txt_extra_info = new Label(m_side_error_panel, wxEmptyString, LB_AUTO_WRAP);
    st_title_extra_info->SetForegroundColour(0x909090);
    st_title_extra_info_doc->SetForegroundColour(0x909090);
    m_st_txt_extra_info->SetForegroundColour(0x909090);
    st_title_extra_info->SetFont(::Label::Body_12);
    st_title_extra_info_doc->SetFont(::Label::Body_12);
    m_st_txt_extra_info->SetFont(::Label::Body_12);
    st_title_extra_info->SetMinSize(wxSize(FromDIP(32), -1));
    st_title_extra_info->SetMaxSize(wxSize(FromDIP(32), -1));
    m_st_txt_extra_info->SetMinSize(wxSize(FromDIP(175), -1));
    m_st_txt_extra_info->SetMaxSize(wxSize(FromDIP(175), -1));
    sizer_extra_info->Add(st_title_extra_info, 0, wxALL, 0);
    sizer_extra_info->Add(st_title_extra_info_doc, 0, wxALL, 0);
    sizer_extra_info->Add(m_st_txt_extra_info, 0, wxALL, 0);

    sizer_print_failed_info->Add(m_link_network_state, 0, wxALIGN_CENTER, 3);
    sizer_print_failed_info->Add(sizer_error_code, 0, wxLEFT, 5);
    sizer_print_failed_info->Add(0, 0, 0, wxTOP, FromDIP(3));
    sizer_print_failed_info->Add(sizer_error_desc, 0, wxLEFT, 5);
    sizer_print_failed_info->Add(0, 0, 0, wxTOP, FromDIP(3));
    sizer_print_failed_info->Add(sizer_extra_info, 0, wxLEFT, 5);

    m_st_txt_error_desc->SetLabel("");

    wxBoxSizer* m_main_sizer = new wxBoxSizer(wxVERTICAL);
    m_main_sizer->Add(m_connection_info, 0, wxEXPAND, 0);
    m_main_sizer->Add(m_side_error_panel, 0, wxEXPAND, 0);
    m_main_sizer->Add(m_side_tools, 1, wxEXPAND, 0);
    SetSizer(m_main_sizer);
    Layout();
    Fit();
}

void SideTools::msw_rescale()
{
    m_side_tools->msw_rescale();
    m_connection_info->SetCornerRadius(0);
    m_connection_info->SetSize(wxSize(FromDIP(220), FromDIP(25)));
    m_connection_info->SetMinSize(wxSize(FromDIP(220), FromDIP(25)));
    m_connection_info->SetMaxSize(wxSize(FromDIP(220), FromDIP(25)));
}

bool SideTools::is_in_interval()
{
    return m_side_tools->is_in_interval();
}

void SideTools::set_current_printer_name(std::string dev_name)
{
    m_side_tools->set_current_printer_name(dev_name);
}

void SideTools::set_current_printer_signal(WifiSignal sign)
{
    m_side_tools->set_current_printer_signal(sign);
}

void SideTools::set_none_printer_mode()
{
    m_side_tools->set_none_printer_mode();
}

void SideTools::start_interval()
{
    m_side_tools->start_interval();
}

void SideTools::update_connect_err_info(int code, wxString desc, wxString info)
{
    m_st_txt_error_code->SetLabelText(wxString::Format("%d", code));
    m_st_txt_error_desc->SetLabelText(desc);
    m_st_txt_extra_info->SetLabelText(info);

    if (code == BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED) {
        m_link_network_state->Hide();
    }
    else if (code == BAMBU_NETWORK_ERR_CONNECTION_TO_SERVER_FAILED) {
        m_link_network_state->Show();
    }
}

void SideTools::update_status(MachineObject* obj)
{
    if (!obj) return;

    /* Update Device Info */
    m_side_tools->set_current_printer_name(obj->get_dev_name());

    // update wifi signal image
    int wifi_signal_val = 0;
    if (!obj->is_connected() || obj->is_connecting()) {
        m_side_tools->set_current_printer_signal(WifiSignal::NONE);
    }
    else if (!obj->is_lan_mode_printer() && !obj->is_online()) {
        m_side_tools->set_current_printer_signal(WifiSignal::NONE);/*STUDIO-10185*/
    }
    else {
        if (obj->network_wired) {
            m_side_tools->set_current_printer_signal(WifiSignal::WIRED);
        }
        else if (!obj->wifi_signal.empty() && boost::ends_with(obj->wifi_signal, "dBm")) {
            try {
                wifi_signal_val = std::stoi(obj->wifi_signal.substr(0, obj->wifi_signal.size() - 3));
            }
            catch (...) {
                ;
            }
            if (wifi_signal_val > -45) {
                m_side_tools->set_current_printer_signal(WifiSignal::STRONG);
            }
            else if (wifi_signal_val <= -45 && wifi_signal_val >= -60) {
                m_side_tools->set_current_printer_signal(WifiSignal::MIDDLE);
            }
            else {
                m_side_tools->set_current_printer_signal(WifiSignal::WEAK);
            }
        }
        else {
            m_side_tools->set_current_printer_signal(WifiSignal::MIDDLE);
        }
    }
}

void SideTools::show_status(int status)
{
    if (((status & (int)MonitorStatus::MONITOR_DISCONNECTED) != 0) || ((status & (int)MonitorStatus::MONITOR_DISCONNECTED_SERVER) != 0)) {
        if ((status & (int)MonitorStatus::MONITOR_DISCONNECTED_SERVER)) {
            m_hyperlink->SetLabel(_L("Failed to connect to the server"));
            update_connect_err_info(BAMBU_NETWORK_ERR_CONNECTION_TO_SERVER_FAILED,
                _L("Failed to connect to cloud service"),
                _L("Please click on the hyperlink above to view the cloud service status"));
        }
        else {
            m_hyperlink->SetLabel(_L("Failed to connect to the printer"));
            update_connect_err_info(BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED,
                _L("Connection to printer failed"),
                _L("Please check the network connection of the printer and Orca."));
        }

        m_hyperlink->Show();
        m_connection_info->SetLabel(wxEmptyString);
        m_connection_info->SetBackgroundColor(0xFF6F00);
        m_connection_info->SetBorderColor(0xFF6F00);
        m_connection_info->Show();
        m_more_button->Show();

    }
    else if ((status & (int)MonitorStatus::MONITOR_NORMAL) != 0) {
        m_connection_info->Hide();
        m_more_button->Hide();
        m_side_error_panel->Hide();
    }
    else if ((status & (int)MonitorStatus::MONITOR_CONNECTING) != 0) {
        m_hyperlink->Hide();
        m_connection_info->SetLabel(_L("Connecting..."));
        m_connection_info->SetBackgroundColor(0x009688);
        m_connection_info->SetBorderColor(0x009688);
        m_connection_info->Show();
        m_more_button->Hide();
        m_side_error_panel->Hide();
    }

    if ((status & (int)MonitorStatus::MONITOR_NO_PRINTER) != 0) {
        m_side_tools->set_none_printer_mode();
        m_connection_info->Hide();
        m_side_error_panel->Hide();
        m_more_button->Hide();
    }
    else if (((status & (int)MonitorStatus::MONITOR_NORMAL) != 0)
        || ((status & (int)MonitorStatus::MONITOR_DISCONNECTED) != 0)
        || ((status & (int)MonitorStatus::MONITOR_DISCONNECTED_SERVER) != 0)
        || ((status & (int)MonitorStatus::MONITOR_CONNECTING) != 0)
        ) {
        if (((status & (int)MonitorStatus::MONITOR_DISCONNECTED) != 0)
            || ((status & (int)MonitorStatus::MONITOR_DISCONNECTED_SERVER) != 0)
            || ((status & (int)MonitorStatus::MONITOR_CONNECTING) != 0)) {
            m_side_tools->set_current_printer_signal(WifiSignal::NONE);
        }
    }
    Layout();
    Fit();
}

SideTools::~SideTools()
{
}

}}
