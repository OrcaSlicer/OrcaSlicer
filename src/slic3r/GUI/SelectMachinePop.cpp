#include "SelectMachinePop.hpp"
#include "I18N.hpp"

#include "libslic3r/Utils.hpp"
#include "libslic3r/Thread.hpp"

#include "slic3r/Utils/WxFontUtils.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Preview.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/StaticBox.hpp"
#include "ConnectPrinter.hpp"


#include <wx/progdlg.h>
#include <wx/clipbrd.h>
#include <wx/dcgraph.h>
#include <wx/mstream.h>
#include <miniz.h>
#include <algorithm>
#include "Plater.hpp"
#include "Notebook.hpp"
#include "BitmapCache.hpp"
#include "BindDialog.hpp"

#include "DeviceCore/DevManager.h"

namespace Slic3r { namespace GUI {

wxDEFINE_EVENT(EVT_UPDATE_WINDOWS_POSITION, wxCommandEvent);
wxDEFINE_EVENT(EVT_FINISHED_UPDATE_MACHINE_LIST, wxCommandEvent);
wxDEFINE_EVENT(EVT_UPDATE_USER_MACHINE_LIST, wxCommandEvent);
wxDEFINE_EVENT(EVT_BIND_MACHINE, wxCommandEvent);
wxDEFINE_EVENT(EVT_UNBIND_MACHINE, wxCommandEvent);
wxDEFINE_EVENT(EVT_DISSMISS_MACHINE_LIST, wxCommandEvent);
wxDEFINE_EVENT(EVT_CONNECT_LAN_PRINT, wxCommandEvent);
wxDEFINE_EVENT(EVT_EDIT_PRINT_NAME, wxCommandEvent);
wxDEFINE_EVENT(EVT_CLEAR_IPADDRESS, wxCommandEvent);


#define INITIAL_NUMBER_OF_MACHINES 0
#define LIST_REFRESH_INTERVAL 200
#define MACHINE_LIST_REFRESH_INTERVAL 2000

#define WRAP_GAP FromDIP(2)

MachineObjectPanel::MachineObjectPanel(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size, long style, const wxString &name)
{
    wxPanel::Create(parent, id, pos, wxDefaultSize, style, name);

    SetSize(SELECT_MACHINE_ITEM_SIZE);
    SetMinSize(SELECT_MACHINE_ITEM_SIZE);
    SetMaxSize(SELECT_MACHINE_ITEM_SIZE);

    Bind(wxEVT_PAINT, &MachineObjectPanel::OnPaint, this);

    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

    m_unbind_img        = ScalableBitmap(this, "unbind", 18);
    m_edit_name_img     = ScalableBitmap(this, "edit_button", 18);
    m_select_unbind_img = ScalableBitmap(this, "unbind_selected", 18);

    m_printer_status_offline = ScalableBitmap(this, "printer_status_offline", 12);
    m_printer_status_busy    = ScalableBitmap(this, "printer_status_busy", 12);
    m_printer_status_idle    = ScalableBitmap(this, "printer_status_idle", 12);
    m_printer_status_lock    = ScalableBitmap(this, "printer_status_lock", 16);
    m_printer_in_lan         = ScalableBitmap(this, "printer_in_lan", 16);

    this->Bind(wxEVT_ENTER_WINDOW, &MachineObjectPanel::on_mouse_enter, this);
    this->Bind(wxEVT_LEAVE_WINDOW, &MachineObjectPanel::on_mouse_leave, this);
    this->Bind(wxEVT_LEFT_UP, &MachineObjectPanel::on_mouse_left_up, this);

#ifdef __APPLE__
    wxPlatformInfo platformInfo;
    auto major = platformInfo.GetOSMajorVersion();
    auto minor = platformInfo.GetOSMinorVersion();
    auto micro = platformInfo.GetOSMicroVersion();

    //macos over 13.1.0
    if (major == 13 && minor >= 1) {
        m_is_macos_special_version = true;
    } else if (major > 13) {
        m_is_macos_special_version = true;
    }
#endif

}


MachineObjectPanel::~MachineObjectPanel() {}

void MachineObjectPanel::show_bind_dialog()
{
    if (wxGetApp().is_user_login()) {
        BindMachineDialog dlg;
        dlg.update_machine_info(m_info);
        dlg.ShowModal();
    }
}

void MachineObjectPanel::set_printer_state(PrinterState state)
{
    m_state = state;
    Refresh();
}

void MachineObjectPanel::show_edit_printer_name(bool show)
{
    m_show_edit = show;
    Refresh();
}

void MachineObjectPanel::show_printer_bind(bool show, PrinterBindState state)
{
    m_show_bind   = show;
    m_bind_state  = state;
    Refresh();
}

void MachineObjectPanel::OnPaint(wxPaintEvent &event)
{
    wxPaintDC dc(this);
    doRender(dc);
}

void MachineObjectPanel::render(wxDC &dc)
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

void MachineObjectPanel::doRender(wxDC &dc)
{
    auto   left = 10;
    wxSize size = GetSize();
    dc.SetPen(*wxTRANSPARENT_PEN);

    auto dwbitmap = m_printer_status_offline;
    if (m_state == PrinterState::IDLE) { dwbitmap = m_printer_status_idle; }
    if (m_state == PrinterState::BUSY) { dwbitmap = m_printer_status_busy; }
    if (m_state == PrinterState::OFFLINE) { dwbitmap = m_printer_status_offline; }
    if (m_state == PrinterState::LOCK) { dwbitmap = m_printer_status_lock; }
    if (m_state == PrinterState::IN_LAN) { dwbitmap = m_printer_in_lan; }

    // dc.DrawCircle(left, size.y / 2, 3);
    dc.DrawBitmap(dwbitmap.bmp(), wxPoint(left, (size.y - dwbitmap.GetBmpSize().y) / 2));

    left += dwbitmap.GetBmpSize().x + 8;
    dc.SetFont(Label::Body_13);
    dc.SetBackgroundMode(wxTRANSPARENT);
    dc.SetTextForeground(StateColor::darkModeColorFor(SELECT_MACHINE_GREY900));
    wxString dev_name = "";
    if (m_info) {
        dev_name = from_u8(m_info->get_dev_name());

         if (m_state == PrinterState::IN_LAN) {
             dev_name += _L("(LAN)");
         }
    }
    auto        sizet        = dc.GetTextExtent(dev_name);
    auto        text_end     = 0;

    if (m_show_edit) {
        text_end = size.x - m_unbind_img.GetBmpSize().x - 30;
    }
    else {
        text_end = size.x - m_unbind_img.GetBmpSize().x;
    }

    wxString finally_name =  dev_name;
    if (sizet.x > (text_end - left)) {
        auto limit_width = text_end - left - dc.GetTextExtent("...").x - 15;
        for (auto i = 0; i < dev_name.length(); i++) {
            auto curr_width = dc.GetTextExtent(dev_name.substr(0, i));
            if (curr_width.x >= limit_width) {
                finally_name = dev_name.substr(0, i) + "...";
                break;
            }
        }
    }

    dc.DrawText(finally_name, wxPoint(left, (size.y - sizet.y) / 2));


    if (m_hover || m_is_macos_special_version) {

        if (m_hover && !m_is_macos_special_version) {
            dc.SetPen(SELECT_MACHINE_BRAND);
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(0, 0, size.x, size.y);
        }

        if (m_show_bind) {
            if (m_bind_state == ALLOW_UNBIND) {
                left = size.x - m_unbind_img.GetBmpSize().x - 6;
                dc.DrawBitmap(m_unbind_img.bmp(), left, (size.y - m_unbind_img.GetBmpSize().y) / 2);
            }
        }

        if (m_show_edit) {
            left = size.x - m_unbind_img.GetBmpSize().x - 6 - m_edit_name_img.GetBmpSize().x - 6;
            dc.DrawBitmap(m_edit_name_img.bmp(), left, (size.y - m_edit_name_img.GetBmpSize().y) / 2);
        }
    }

}

void MachineObjectPanel::update_machine_info(MachineObject *info, bool is_my_devices)
{
    m_info = info;
    m_is_my_devices = is_my_devices;
    Refresh();
}

void MachineObjectPanel::on_mouse_enter(wxMouseEvent &evt)
{
    m_hover = true;
    Refresh();
}

void MachineObjectPanel::on_mouse_leave(wxMouseEvent &evt)
{
    m_hover = false;
    Refresh();
}

void MachineObjectPanel::on_mouse_left_up(wxMouseEvent &evt)
{
    if (m_is_my_devices) {
        // show edit
        if (m_show_edit) {
            auto edit_left   = GetSize().x - m_unbind_img.GetBmpSize().x - 6 - m_edit_name_img.GetBmpSize().x - 6;
            auto edit_right  = edit_left + m_edit_name_img.GetBmpSize().x;
            auto edit_top    = (GetSize().y - m_edit_name_img.GetBmpSize().y) / 2;
            auto edit_bottom = (GetSize().y - m_edit_name_img.GetBmpSize().y) / 2 + m_edit_name_img.GetBmpSize().y;
            if ((evt.GetPosition().x >= edit_left && evt.GetPosition().x <= edit_right) && evt.GetPosition().y >= edit_top && evt.GetPosition().y <= edit_bottom) {
                wxCommandEvent event(EVT_EDIT_PRINT_NAME);
                event.SetEventObject(this);
                wxPostEvent(this, event);
                return;
            }
        }
        if (m_show_bind) {
            auto left   = GetSize().x - m_unbind_img.GetBmpSize().x - 6;
            auto right  = left + m_unbind_img.GetBmpSize().x;
            auto top    = (GetSize().y - m_unbind_img.GetBmpSize().y) / 2;
            auto bottom = (GetSize().y - m_unbind_img.GetBmpSize().y) / 2 + m_unbind_img.GetBmpSize().y;

            if ((evt.GetPosition().x >= left && evt.GetPosition().x <= right) && evt.GetPosition().y >= top && evt.GetPosition().y <= bottom) {
                wxCommandEvent event(EVT_UNBIND_MACHINE, GetId());
                event.SetEventObject(this);
                GetEventHandler()->ProcessEvent(event);
            } else {
                if (m_info) {
                    wxGetApp().mainframe->jump_to_monitor(m_info->get_dev_id());
                }
                //wxGetApp().mainframe->SetFocus();
                wxCommandEvent event(EVT_DISSMISS_MACHINE_LIST);
                event.SetEventObject(this->GetParent());
                wxPostEvent(this->GetParent(), event);
            }
            return;
        }
        if (m_info && m_info->is_lan_mode_printer()) {
            if (m_info->has_access_right() && m_info->is_avaliable()) {
                wxGetApp().mainframe->jump_to_monitor(m_info->get_dev_id());
            } else {
                wxCommandEvent event(EVT_CONNECT_LAN_PRINT);
                event.SetEventObject(this);
                wxPostEvent(this, event);
            }
        } else {
            wxGetApp().mainframe->jump_to_monitor(m_info->get_dev_id());
        }
    } else {
        if (m_info && m_info->is_lan_mode_printer()) {
            wxCommandEvent event(EVT_CONNECT_LAN_PRINT);
            event.SetEventObject(this);
            wxPostEvent(this, event);
        } else {
            wxCommandEvent event(EVT_BIND_MACHINE);
            event.SetEventObject(this);
            wxPostEvent(this, event);
        }
    }

}

SelectMachinePopup::SelectMachinePopup(wxWindow *parent)
    : PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS), m_dismiss(false)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__


    SetSize(SELECT_MACHINE_POPUP_SIZE);
    SetMinSize(SELECT_MACHINE_POPUP_SIZE);
    SetMaxSize(SELECT_MACHINE_POPUP_SIZE);

    Freeze();
    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);
    SetBackgroundColour(SELECT_MACHINE_GREY400);



    m_scrolledWindow = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, SELECT_MACHINE_LIST_SIZE, wxHSCROLL | wxVSCROLL);
    m_scrolledWindow->SetBackgroundColour(*wxWHITE);
    m_scrolledWindow->SetMinSize(SELECT_MACHINE_LIST_SIZE);
    m_scrolledWindow->SetScrollRate(0, 5);
    auto m_sizxer_scrolledWindow = new wxBoxSizer(wxVERTICAL);
    m_scrolledWindow->SetSizer(m_sizxer_scrolledWindow);
    m_scrolledWindow->Layout();
    m_sizxer_scrolledWindow->Fit(m_scrolledWindow);

#if defined(__WINDOWS__)
	m_sizer_search_bar = new wxBoxSizer(wxVERTICAL);
	m_search_bar = new wxSearchCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0 );
	m_search_bar->SetDescriptiveText(_L("Search"));
	m_search_bar->ShowSearchButton( true );
	m_search_bar->ShowCancelButton( false );
	m_sizer_search_bar->Add( m_search_bar, 1, wxALL| wxEXPAND, 1 );
	m_sizer_main->Add(m_sizer_search_bar, 0, wxALL | wxEXPAND, FromDIP(2));
	m_search_bar->Bind( wxEVT_COMMAND_TEXT_UPDATED, &SelectMachinePopup::update_machine_list, this );
#endif
    auto own_title        = create_title_panel(_L("My Device"));
    m_sizer_my_devices    = new wxBoxSizer(wxVERTICAL);
    auto other_title      = create_title_panel(_L("Other Device"));
    m_sizer_other_devices = new wxBoxSizer(wxVERTICAL);


    m_panel_ping_code = new PinCodePanel(m_scrolledWindow, 0, wxID_ANY, wxDefaultPosition, SELECT_MACHINE_ITEM_SIZE);
    m_panel_direct_connection = new PinCodePanel(m_scrolledWindow, 1, wxID_ANY, wxDefaultPosition, SELECT_MACHINE_ITEM_SIZE);

    m_sizxer_scrolledWindow->Add(own_title, 0, wxEXPAND | wxLEFT, FromDIP(15));
    m_sizxer_scrolledWindow->Add(m_sizer_my_devices, 0, wxEXPAND, 0);
    m_sizxer_scrolledWindow->Add(m_panel_ping_code, 0, wxEXPAND, 0);
    m_sizxer_scrolledWindow->Add(m_panel_direct_connection, 0, wxEXPAND, 0);
    m_sizxer_scrolledWindow->Add(other_title, 0, wxEXPAND | wxLEFT, FromDIP(15));
    m_sizxer_scrolledWindow->Add(m_sizer_other_devices, 0, wxEXPAND, 0);

    m_sizer_main->Add(m_scrolledWindow, 0, wxALL | wxEXPAND, FromDIP(2));

    SetSizer(m_sizer_main);
    Layout();
    Thaw();

    #ifdef __APPLE__
    m_scrolledWindow->Bind(wxEVT_LEFT_UP, &SelectMachinePopup::OnLeftUp, this);
    #endif // __APPLE__

    m_refresh_timer = new wxTimer();
    m_refresh_timer->SetOwner(this);
    Bind(EVT_UPDATE_USER_MACHINE_LIST, &SelectMachinePopup::update_machine_list, this);
    Bind(wxEVT_TIMER, &SelectMachinePopup::on_timer, this);
    Bind(EVT_DISSMISS_MACHINE_LIST, &SelectMachinePopup::on_dissmiss_win, this);
}

SelectMachinePopup::~SelectMachinePopup() { delete m_refresh_timer;}

void SelectMachinePopup::Popup(wxWindow *WXUNUSED(focus))
{
    BOOST_LOG_TRIVIAL(trace) << "get_print_info: start";
    start_ssdp(true);
    if (m_refresh_timer) {
        m_refresh_timer->Stop();
        m_refresh_timer->Start(MACHINE_LIST_REFRESH_INTERVAL);
    }

    if (wxGetApp().is_user_login()) {
        if (!get_print_info_thread) {
            get_print_info_thread = new boost::thread(Slic3r::create_thread([this, token = std::weak_ptr<int>(m_token)] {
                NetworkAgent* agent = wxGetApp().getAgent();
                unsigned int http_code;
                std::string body;
                int result = agent->get_user_print_info(&http_code, &body);
                CallAfter([token, this, result, body]() {
                    if (token.expired()) {return;}
                    if (result == 0) {
                        m_print_info = body;
                    }
                    else {
                        m_print_info = "";
                    }
                    wxCommandEvent event(EVT_UPDATE_USER_MACHINE_LIST);
                    event.SetEventObject(this);
                    wxPostEvent(this, event);
                });
            }));
        }
    }

    {
        wxGetApp().reset_to_active();
        wxCommandEvent user_event(EVT_UPDATE_USER_MACHINE_LIST);
        user_event.SetEventObject(this);
        wxPostEvent(this, user_event);
    }

    PopupWindow::Popup();
}

void SelectMachinePopup::OnDismiss()
{
    BOOST_LOG_TRIVIAL(trace) << "get_print_info: dismiss";
    start_ssdp(false);
    m_dismiss = true;

    if (m_refresh_timer) {
        m_refresh_timer->Stop();
    }
    if (get_print_info_thread) {
        if (get_print_info_thread->joinable()) {
            get_print_info_thread->join();
            delete get_print_info_thread;
            get_print_info_thread = nullptr;
        }
    }

    wxCommandEvent event(EVT_FINISHED_UPDATE_MACHINE_LIST);
    event.SetEventObject(this);
    wxPostEvent(this, event);
}

bool SelectMachinePopup::ProcessLeftDown(wxMouseEvent &event) {
    return PopupWindow::ProcessLeftDown(event);
}

bool SelectMachinePopup::Show(bool show) {
    if (show) {
        for (int i = 0; i < m_user_list_machine_panel.size(); i++) {
            m_user_list_machine_panel[i]->mPanel->update_machine_info(nullptr);
            m_user_list_machine_panel[i]->mPanel->Hide();
        }

         for (int j = 0; j < m_other_list_machine_panel.size(); j++) {
            m_other_list_machine_panel[j]->mPanel->update_machine_info(nullptr);
            m_other_list_machine_panel[j]->mPanel->Hide();
        }
    }
    return PopupWindow::Show(show);
}

wxWindow *SelectMachinePopup::create_title_panel(wxString text)
{
    auto m_panel_title_own = new wxWindow(m_scrolledWindow, wxID_ANY, wxDefaultPosition, SELECT_MACHINE_ITEM_SIZE, wxTAB_TRAVERSAL);
    m_panel_title_own->SetBackgroundColour(*wxWHITE);

    wxBoxSizer *m_sizer_title_own = new wxBoxSizer(wxHORIZONTAL);

    auto m_title_own = new wxStaticText(m_panel_title_own, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, 0);
    m_title_own->Wrap(-1);
    m_sizer_title_own->Add(m_title_own, 0, wxALIGN_CENTER, 0);

    wxBoxSizer *m_sizer_line_own = new wxBoxSizer(wxHORIZONTAL);

    auto m_panel_line_own = new wxPanel(m_panel_title_own, wxID_ANY, wxDefaultPosition, wxSize(SELECT_MACHINE_ITEM_SIZE.x, FromDIP(1)), wxTAB_TRAVERSAL);
    m_panel_line_own->SetBackgroundColour(SELECT_MACHINE_GREY400);

    m_sizer_line_own->Add(m_panel_line_own, 0, wxALIGN_CENTER, 0);
    m_sizer_title_own->Add(0, 0, 0, wxLEFT, FromDIP(10));
    m_sizer_title_own->Add(m_sizer_line_own, 1, wxEXPAND | wxRIGHT, FromDIP(10));

    m_panel_title_own->SetSizer(m_sizer_title_own);
    m_panel_title_own->Layout();
    return m_panel_title_own;
}

void SelectMachinePopup::on_timer(wxTimerEvent &event)
{
    BOOST_LOG_TRIVIAL(trace) << "SelectMachinePopup on_timer";
    wxGetApp().reset_to_active();
    wxCommandEvent user_event(EVT_UPDATE_USER_MACHINE_LIST);
    user_event.SetEventObject(this);
    wxPostEvent(this, user_event);
}

void SelectMachinePopup::update_other_devices()
{
    DeviceManager* dev = wxGetApp().getDeviceManager();
    if (!dev) return;
    m_free_machine_list = dev->get_local_machinelist();

    BOOST_LOG_TRIVIAL(trace) << "SelectMachinePopup update_other_devices start";
    this->Freeze();
    m_scrolledWindow->Freeze();
    int i = 0;

    for (auto &elem : m_free_machine_list) {
        MachineObject *     mobj = elem.second;
        /* do not show printer bind state is empty */
        if (!mobj->is_avaliable()) continue;

        if (!wxGetApp().is_user_login() && !mobj->is_lan_mode_printer())
            continue;

        /* do not show printer in my list */
        auto it = m_bind_machine_list.find(mobj->get_dev_id());
        if (it != m_bind_machine_list.end())
            continue;

        MachineObjectPanel* op = nullptr;
        if (i < m_other_list_machine_panel.size()) {
            op = m_other_list_machine_panel[i]->mPanel;
        } else {
            op = new MachineObjectPanel(m_scrolledWindow, wxID_ANY);
            MachinePanel* mpanel = new MachinePanel();
            mpanel->mIndex = wxString::Format("%d", i);
            mpanel->mPanel = op;
            m_other_list_machine_panel.push_back(mpanel);
            m_sizer_other_devices->Add(op, 0, wxEXPAND, 0);
        }
#if defined(__WINDOWS__)
        if (!search_for_printer(mobj)) {
            op->Hide();
        }
        else {
            op->Show();
        }
#else
        op->Show();
#endif
        i++;

        op->update_machine_info(mobj);

        if (mobj->is_lan_mode_printer()) {
            if (mobj->has_access_right()) {
                op->set_printer_state(PrinterState::IN_LAN);
            } else {
                op->set_printer_state(PrinterState::LOCK);
            }
        } else {
            op->show_edit_printer_name(false);
            op->show_printer_bind(true, PrinterBindState::ALLOW_BIND);
            if (mobj->is_in_printing()) {
                op->set_printer_state(PrinterState::BUSY);
            } else {
                op->SetToolTip(_L("Online"));
                op->set_printer_state(IDLE);
            }
        }

        op->Bind(EVT_CONNECT_LAN_PRINT, [this, mobj](wxCommandEvent &e) {
            if (mobj) {
                if (mobj->is_lan_mode_printer()) {
                    ConnectPrinterDialog dlg(wxGetApp().mainframe, wxID_ANY, _L("Input access code"));
                    dlg.set_machine_object(mobj);
                    if (dlg.ShowModal() == wxID_OK) {
                        wxGetApp().mainframe->jump_to_monitor(mobj->get_dev_id());
                    }
                }
            }
        });

        op->Bind(EVT_BIND_MACHINE, [this, mobj](wxCommandEvent &e) {
            BindMachineDialog dlg;
            dlg.update_machine_info(mobj);
            int dlg_result = wxID_CANCEL;
            dlg_result     = dlg.ShowModal();
            if (dlg_result == wxID_OK) { wxGetApp().mainframe->jump_to_monitor(mobj->get_dev_id()); }
        });
    }

    for (int j = i; j < m_other_list_machine_panel.size(); j++) {
        m_other_list_machine_panel[j]->mPanel->update_machine_info(nullptr);
        m_other_list_machine_panel[j]->mPanel->Hide();
    }

    if (m_placeholder_panel != nullptr) {
        m_scrolledWindow->RemoveChild(m_placeholder_panel);
        m_placeholder_panel->Destroy();
        m_placeholder_panel = nullptr;
    }

    m_placeholder_panel = new wxWindow(m_scrolledWindow, wxID_ANY, wxDefaultPosition, wxSize(-1,FromDIP(26)));
    wxBoxSizer* placeholder_sizer = new wxBoxSizer(wxVERTICAL);

    m_hyperlink = new wxHyperlinkCtrl(m_placeholder_panel, wxID_ANY, _L("Can't find my devices?"), wxT("https://wiki.bambulab.com/en/software/bambu-studio/failed-to-connect-printer"), wxDefaultPosition, wxDefaultSize, wxHL_DEFAULT_STYLE);
    m_hyperlink->SetNormalColour(StateColor::darkModeColorFor("#009789"));
    placeholder_sizer->Add(m_hyperlink, 0, wxALIGN_CENTER | wxALL, 5);


    m_placeholder_panel->SetSizer(placeholder_sizer);
    m_placeholder_panel->Layout();
    placeholder_sizer->Fit(m_placeholder_panel);

    m_placeholder_panel->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    m_sizer_other_devices->Add(m_placeholder_panel, 0, wxEXPAND, 0);

    //m_sizer_other_devices->Layout();
    if(m_other_devices_count != i) {
		m_scrolledWindow->Fit();
    }
    m_scrolledWindow->Layout();
	m_scrolledWindow->Thaw();
	Layout();
	Fit();
	this->Thaw();
    m_other_devices_count = i;
    BOOST_LOG_TRIVIAL(trace) << "SelectMachinePopup update_other_devices end";
}

void SelectMachinePopup::update_user_devices()
{
    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;

    if (!m_print_info.empty()) {
        dev->parse_user_print_info(m_print_info);
        m_print_info = "";
    }

    m_bind_machine_list.clear();
    m_bind_machine_list = dev->get_my_machine_list();

    //sort list
    std::vector<std::pair<std::string, MachineObject*>> user_machine_list;
    for (auto& it: m_bind_machine_list) {
        user_machine_list.push_back(it);
    }

    std::sort(user_machine_list.begin(), user_machine_list.end(), [&](auto& a, auto&b) {
            if (a.second && b.second) {
                return a.second->get_dev_name().compare(b.second->get_dev_name()) < 0;
            }
            return false;
        });

    BOOST_LOG_TRIVIAL(trace) << "SelectMachinePopup update_machine_list start";
    this->Freeze();
    m_scrolledWindow->Freeze();
    int i = 0;

    for (auto& elem : user_machine_list) {
        MachineObject* mobj = elem.second;
        MachineObjectPanel* op = nullptr;
        if (i < m_user_list_machine_panel.size()) {
            op = m_user_list_machine_panel[i]->mPanel;
#if defined(__WINDOWS__)
			if (!search_for_printer(mobj)) {
				op->Hide();
			} else {
                op->Show();
            }
#else
            op->Show();
#endif
        } else {
            op = new MachineObjectPanel(m_scrolledWindow, wxID_ANY);
            MachinePanel* mpanel = new MachinePanel();
            mpanel->mIndex = wxString::Format("%d", i);
            mpanel->mPanel = op;
            m_user_list_machine_panel.push_back(mpanel);
            m_sizer_my_devices->Add(op, 0, wxEXPAND, 0);
        }
        i++;
        op->update_machine_info(mobj, true);
        //set in lan
        if (mobj->is_lan_mode_printer()) {
            if (!mobj->is_online()) {
                continue;
            }
            else {
                op->show_printer_bind(false, PrinterBindState::NONE);
                op->show_edit_printer_name(false);
                if (mobj->has_access_right() && mobj->is_avaliable()) {
                    op->set_printer_state(PrinterState::IN_LAN);
                    op->show_printer_bind(true, PrinterBindState::ALLOW_UNBIND);
                    op->SetToolTip(_L("Online"));
                }
                else {
                    op->set_printer_state(PrinterState::LOCK);
                }
            }
            op->Bind(EVT_UNBIND_MACHINE, [this, dev, mobj](wxCommandEvent& e) {
                dev->set_selected_machine("");
                if (mobj) {
                    AppConfig* config = wxGetApp().app_config;
                    if (config) {
                        config->erase_local_machine(mobj->get_dev_id());
                    }

                    mobj->set_access_code("");
                    mobj->erase_user_access_code();
                }

                if (GUI::wxGetApp().plater())
                    GUI::wxGetApp().plater()->update_machine_sync_status();

                MessageDialog msg_wingow(nullptr, _L("Log out successful."), "", wxAPPLY | wxOK);
                if (msg_wingow.ShowModal() == wxOK) { return; }
                });
        }
        else {
            op->show_printer_bind(true, PrinterBindState::ALLOW_UNBIND);
            op->Bind(EVT_UNBIND_MACHINE, [this, mobj, dev](wxCommandEvent& e) {
                // show_unbind_dialog
                UnBindMachineDialog dlg;
                dlg.update_machine_info(mobj);
                if (dlg.ShowModal() == wxID_OK) {
                    dev->set_selected_machine("");
                }
                });

            if (!mobj->is_online()) {
                op->SetToolTip(_L("Offline"));
                op->set_printer_state(PrinterState::OFFLINE);
            }
            else {
                op->show_edit_printer_name(true);
                op->show_printer_bind(true, PrinterBindState::ALLOW_UNBIND);
                if (mobj->is_in_printing()) {
                    op->SetToolTip(_L("Busy"));
                    op->set_printer_state(PrinterState::BUSY);
                }
                else {
                    op->SetToolTip(_L("Online"));
                    op->set_printer_state(PrinterState::IDLE);
                }
            }
        }

        op->Bind(EVT_CONNECT_LAN_PRINT, [this, mobj](wxCommandEvent &e) {
            if (mobj) {
                if (mobj->is_lan_mode_printer()) {
                    ConnectPrinterDialog dlg(wxGetApp().mainframe, wxID_ANY, _L("Input access code"));
                    dlg.set_machine_object(mobj);
                    if (dlg.ShowModal() == wxID_OK) {
                        wxGetApp().mainframe->jump_to_monitor(mobj->get_dev_id());
                    }
                }
            }
        });

         op->Bind(EVT_EDIT_PRINT_NAME, [this, mobj](wxCommandEvent &e) {
            EditDevNameDialog dlg;
            dlg.set_machine_obj(mobj);
            dlg.ShowModal();
         });
    }

    for (int j = i; j < m_user_list_machine_panel.size(); j++) {
        m_user_list_machine_panel[j]->mPanel->update_machine_info(nullptr);
        m_user_list_machine_panel[j]->mPanel->Hide();
    }
    //m_sizer_my_devices->Layout();

    if (m_my_devices_count != i) {
		m_scrolledWindow->Fit();
    }
    m_scrolledWindow->Layout();
    m_scrolledWindow->Thaw();
	Layout();
	Fit();
	this->Thaw();
    m_my_devices_count = i;
}

bool SelectMachinePopup::search_for_printer(MachineObject* obj)
{
	const std::string& search_text = m_search_bar->GetValue().ToStdString();
	if (search_text.empty()) {
		return true;
	}

	const auto& name = wxString::FromUTF8(obj->get_dev_name()).ToStdString();
    const auto& name_it = name.find(search_text);
    if (name_it != std::string::npos) {
        return true;
    }

#if !BBL_RELEASE_TO_PUBLIC
    const auto& ip_it = obj->get_dev_ip().find(search_text);
    if (ip_it != std::string::npos) {
        return true;
    }
#endif

    return false;
}

void SelectMachinePopup::on_dissmiss_win(wxCommandEvent &event)
{
    Dismiss();
}

void SelectMachinePopup::update_machine_list(wxCommandEvent &event)
{
    update_user_devices();
    update_other_devices();
    BOOST_LOG_TRIVIAL(trace) << "SelectMachinePopup update_machine_list end";
}

void SelectMachinePopup::start_ssdp(bool start)
{
    return;
    //if (wxGetApp().getAgent()) { wxGetApp().getAgent()->start_discovery(true, start); }
}

void SelectMachinePopup::OnLeftUp(wxMouseEvent &event)
{
    auto mouse_pos = ClientToScreen(event.GetPosition());
    auto wxscroll_win_pos = m_scrolledWindow->ClientToScreen(wxPoint(0, 0));

    if (mouse_pos.x > wxscroll_win_pos.x && mouse_pos.y > wxscroll_win_pos.y && mouse_pos.x < (wxscroll_win_pos.x + m_scrolledWindow->GetSize().x) &&
        mouse_pos.y < (wxscroll_win_pos.y + m_scrolledWindow->GetSize().y)) {

        for (MachinePanel* p : m_user_list_machine_panel) {
            auto p_rect = p->mPanel->ClientToScreen(wxPoint(0, 0));
            if (mouse_pos.x > p_rect.x && mouse_pos.y > p_rect.y && mouse_pos.x < (p_rect.x + p->mPanel->GetSize().x) && mouse_pos.y < (p_rect.y + p->mPanel->GetSize().y)) {
                wxMouseEvent event(wxEVT_LEFT_UP);
                auto         tag_pos = p->mPanel->ScreenToClient(mouse_pos);
                event.SetPosition(tag_pos);
                event.SetEventObject(p->mPanel);
                wxPostEvent(p->mPanel, event);
            }
        }

        for (MachinePanel* p : m_other_list_machine_panel) {
            auto p_rect = p->mPanel->ClientToScreen(wxPoint(0, 0));
            if (mouse_pos.x > p_rect.x && mouse_pos.y > p_rect.y && mouse_pos.x < (p_rect.x + p->mPanel->GetSize().x) && mouse_pos.y < (p_rect.y + p->mPanel->GetSize().y)) {
                wxMouseEvent event(wxEVT_LEFT_UP);
                auto         tag_pos = p->mPanel->ScreenToClient(mouse_pos);
                event.SetPosition(tag_pos);
                event.SetEventObject(p->mPanel);
                wxPostEvent(p->mPanel, event);
            }
        }

        //pin code
        auto pc_rect = m_panel_ping_code->ClientToScreen(wxPoint(0, 0));
        if (mouse_pos.x > pc_rect.x && mouse_pos.y > pc_rect.y && mouse_pos.x < (pc_rect.x + m_panel_ping_code->GetSize().x) && mouse_pos.y < (pc_rect.y + m_panel_ping_code->GetSize().y)) {
            wxGetApp().popup_ping_bind_dialog();
        }

        //bind with access code
        auto dc_rect = m_panel_direct_connection->ClientToScreen(wxPoint(0, 0));
        if (mouse_pos.x > dc_rect.x && mouse_pos.y > dc_rect.y && mouse_pos.x < (dc_rect.x + m_panel_direct_connection->GetSize().x) && mouse_pos.y < (dc_rect.y + m_panel_direct_connection->GetSize().y)) {
            InputIpAddressDialog dlgo;
            dlgo.ShowModal();
        }

        //hyper link
        auto h_rect = m_hyperlink->ClientToScreen(wxPoint(0, 0));
        if (mouse_pos.x > h_rect.x && mouse_pos.y > h_rect.y && mouse_pos.x < (h_rect.x + m_hyperlink->GetSize().x) && mouse_pos.y < (h_rect.y + m_hyperlink->GetSize().y)) {
          wxLaunchDefaultBrowser(wxT("https://wiki.bambulab.com/en/software/bambu-studio/failed-to-connect-printer"));
        }
    }
}

EditDevNameDialog::EditDevNameDialog(Plater *plater /*= nullptr*/)
    : DPIDialog(static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Modifying the device name"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);
    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);
    auto        m_line_top   = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
    m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);
    m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(38));
    m_textCtr = new ::TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(260), FromDIP(40)), wxTE_PROCESS_ENTER);
    m_textCtr->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(22)));
    m_textCtr->SetMinSize(wxSize(FromDIP(260), FromDIP(40)));
    m_sizer_main->Add(m_textCtr, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(40));

    m_static_valid = new wxStaticText(this, wxID_ANY, wxT(""), wxDefaultPosition, wxDefaultSize, 0);
    m_static_valid->Wrap(-1);
    m_static_valid->SetFont(::Label::Body_13);
    m_static_valid->SetForegroundColour(wxColour(255, 111, 0));
    m_sizer_main->Add(m_static_valid, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxLEFT | wxRIGHT, FromDIP(10));


    m_button_confirm = new Button(this, _L("Confirm"));
    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed), std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
    m_button_confirm->SetBackgroundColor(btn_bg_green);
    m_button_confirm->SetBorderColor(wxColour(0, 150, 136));
    m_button_confirm->SetTextColor(wxColour(255, 255, 255));
    m_button_confirm->SetSize(wxSize(FromDIP(72), FromDIP(24)));
    m_button_confirm->SetMinSize(wxSize(FromDIP(72), FromDIP(24)));
    m_button_confirm->SetCornerRadius(FromDIP(12));
    m_button_confirm->Bind(wxEVT_BUTTON, &EditDevNameDialog::on_edit_name, this);

    m_sizer_main->Add(m_button_confirm, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
    m_sizer_main->Add(0, 0, 0, wxBOTTOM, FromDIP(38));

    SetSizer(m_sizer_main);
    Layout();
    Fit();
    Centre(wxBOTH);
    wxGetApp().UpdateDlgDarkUI(this);
}

EditDevNameDialog::~EditDevNameDialog() {}

void EditDevNameDialog::set_machine_obj(MachineObject *obj)
{
    m_info = obj;
    if (m_info)
        m_textCtr->GetTextCtrl()->SetValue(from_u8(m_info->get_dev_name()));
}

void EditDevNameDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    m_button_confirm->SetSize(wxSize(FromDIP(72), FromDIP(24)));
    m_button_confirm->SetMinSize(wxSize(FromDIP(72), FromDIP(24)));
}

void EditDevNameDialog::on_edit_name(wxCommandEvent &e)
{
    m_static_valid->SetLabel(wxEmptyString);
    auto     m_valid_type = Valid;
    wxString info_line;
    auto     new_dev_name = m_textCtr->GetTextCtrl()->GetValue();

    const char *      unusable_symbols = "<>[]:/\\|?*\"";
    const std::string unusable_suffix  = PresetCollection::get_suffix_modified();

    for (size_t i = 0; i < std::strlen(unusable_symbols); i++) {
        if (new_dev_name.find_first_of(unusable_symbols[i]) != std::string::npos) {
            info_line    = _L("Name is invalid;") + _L("illegal characters:") + " " + unusable_symbols;
            m_valid_type = NoValid;
            break;
        }
    }

    if (m_valid_type == Valid && new_dev_name.find(unusable_suffix) != std::string::npos) {
        info_line    = _L("Name is invalid;") + _L("illegal suffix:") + "\n\t" + from_u8(PresetCollection::get_suffix_modified());
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_dev_name.empty()) {
        info_line    = _L("The name is not allowed to be empty.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_dev_name.find_first_of(' ') == 0) {
        info_line    = _L("The name is not allowed to start with space character.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_dev_name.find_last_of(' ') == new_dev_name.length() - 1) {
        info_line    = _L("The name is not allowed to end with space character.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_dev_name.length() > 32)
    {
        info_line    = _L("The name is not allowed to exceed 32 characters.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == NoValid) {
        m_static_valid->SetLabel(info_line);
        m_static_valid->Wrap(m_static_valid->GetSize().GetWidth());
        Layout();
    }

    if (m_valid_type == Valid) {
        m_static_valid->SetLabel(wxEmptyString);
        DeviceManager *dev      = Slic3r::GUI::wxGetApp().getDeviceManager();
        if (dev) {
            auto           utf8_str = new_dev_name.ToUTF8();
            auto           name     = std::string(utf8_str.data(), utf8_str.length());
            if (m_info)
                dev->modify_device_name(m_info->get_dev_id(), name);
        }
        DPIDialog::EndModal(wxID_CLOSE);
    }
}

PinCodePanel::PinCodePanel(wxWindow* parent, int type, wxWindowID winid /*= wxID_ANY*/, const wxPoint& pos /*= wxDefaultPosition*/, const wxSize& size /*= wxDefaultSize*/)
 {
     wxPanel::Create(parent, winid, pos);
     Bind(wxEVT_PAINT, &PinCodePanel::OnPaint, this);
     SetSize(SELECT_MACHINE_ITEM_SIZE);
     SetMaxSize(SELECT_MACHINE_ITEM_SIZE);
     SetMinSize(SELECT_MACHINE_ITEM_SIZE);

     m_type = type;
     m_bitmap = ScalableBitmap(this, "bind_device_ping_code",10);
     
     this->Bind(wxEVT_ENTER_WINDOW, &PinCodePanel::on_mouse_enter, this);
     this->Bind(wxEVT_LEAVE_WINDOW, &PinCodePanel::on_mouse_leave, this);
     this->Bind(wxEVT_LEFT_UP, &PinCodePanel::on_mouse_left_up, this);
 }

 void PinCodePanel::OnPaint(wxPaintEvent& event)
 {
     wxPaintDC dc(this);
     render(dc);
 }

 void PinCodePanel::render(wxDC& dc)
 {
#ifdef __WXMSW__
     wxSize     size = GetSize();
     wxMemoryDC memdc;
     wxBitmap   bmp(size.x, size.y);
     memdc.SelectObject(bmp);
     memdc.Blit({ 0, 0 }, size, &dc, { 0, 0 });

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

 void PinCodePanel::doRender(wxDC& dc)
 {
     auto size = GetSize();
     dc.DrawBitmap(m_bitmap.bmp(), wxPoint(FromDIP(12), (size.y - m_bitmap.GetBmpSize().y) / 2));
     dc.SetFont(::Label::Head_13);
     dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#262E30"))); // ORCA fix text not visible on dark theme
     wxString txt;
     if (m_type == 0) {txt = _L("Bind with Pin Code");}
     else if (m_type == 1) {txt = _L("Bind with Access Code");}

     WxFontUtils::get_suitable_font_size(0.5 * size.GetHeight(), dc);
     auto txt_size = dc.GetTextExtent(txt);
     dc.DrawText(txt, wxPoint(FromDIP(28), (size.y - txt_size.y) / 2));

     if (m_hover) {
         dc.SetPen(SELECT_MACHINE_BRAND);
         dc.SetBrush(*wxTRANSPARENT_BRUSH);
         dc.DrawRectangle(0, 0, size.x, size.y);
     }
 }

 void PinCodePanel::on_mouse_enter(wxMouseEvent& evt)
 {
     m_hover = true;
     Refresh();
 }

 void PinCodePanel::on_mouse_leave(wxMouseEvent& evt)
 {
     m_hover = false;
     Refresh();
 }

 void PinCodePanel::on_mouse_left_up(wxMouseEvent& evt)
 {
     if (m_type == 0) {
         wxGetApp().popup_ping_bind_dialog();
     }
     else if (m_type == 1) {
         InputIpAddressDialog dlgo;
         dlgo.ShowModal();
     }
 }

 }} // namespace Slic3r::GUI
