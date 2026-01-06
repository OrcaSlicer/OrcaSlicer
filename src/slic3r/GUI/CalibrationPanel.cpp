#include <wx/dcgraph.h>
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "CalibrationPanel.hpp"
#include "I18N.hpp"
#include "SelectMachine.hpp"
#include "SelectMachinePop.hpp"

#include "DeviceCore/DevManager.h"

namespace Slic3r { namespace GUI {

#define REFRESH_INTERVAL       1000

#define INITIAL_NUMBER_OF_MACHINES 0
#define LIST_REFRESH_INTERVAL 200
#define MACHINE_LIST_REFRESH_INTERVAL 2000
wxDEFINE_EVENT(EVT_FINISHED_UPDATE_MLIST, wxCommandEvent);
wxDEFINE_EVENT(EVT_UPDATE_USER_MLIST, wxCommandEvent);

wxString get_calibration_type_name(CalibMode cali_mode)
{
    switch (cali_mode) {
    case CalibMode::Calib_PA_Line:
        return _L("Flow Dynamics");
    case CalibMode::Calib_Flow_Rate:
        return _L("Flow Rate");
    case CalibMode::Calib_Vol_speed_Tower:
        return _L("Max Volumetric Speed");
    case CalibMode::Calib_Temp_Tower:
        return _L("Temperature");
    case CalibMode::Calib_Retraction_tower:
        return _L("Retraction");
    default:
        return "";
    }
}

MObjectPanel::MObjectPanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name)
{
    wxPanel::Create(parent, id, pos, SELECT_MACHINE_ITEM_SIZE, style, name);
    Bind(wxEVT_PAINT, &MObjectPanel::OnPaint, this);
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));


    m_printer_status_offline = ScalableBitmap(this, "printer_status_offline", 12);
    m_printer_status_busy = ScalableBitmap(this, "printer_status_busy", 12);
    m_printer_status_idle = ScalableBitmap(this, "printer_status_idle", 12);
    m_printer_status_lock = ScalableBitmap(this, "printer_status_lock", 16);
    m_printer_in_lan = ScalableBitmap(this, "printer_in_lan", 16);

    Bind(wxEVT_ENTER_WINDOW, &MObjectPanel::on_mouse_enter, this);
    Bind(wxEVT_LEAVE_WINDOW, &MObjectPanel::on_mouse_leave, this);
    Bind(wxEVT_LEFT_UP, &MObjectPanel::on_mouse_left_up, this);
}


MObjectPanel::~MObjectPanel() {}


void MObjectPanel::set_printer_state(PrinterState state)
{
    m_state = state;
    Refresh();
}

void MObjectPanel::OnPaint(wxPaintEvent & event)
{
    wxPaintDC dc(this);
    doRender(dc);
}

void MObjectPanel::render(wxDC& dc)
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

void MObjectPanel::doRender(wxDC& dc)
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
    auto        sizet = dc.GetTextExtent(dev_name);
    auto        text_end = size.x - FromDIP(15);

    wxString finally_name = dev_name;
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


    if (m_hover) {
        dc.SetPen(SELECT_MACHINE_BRAND);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, size.x, size.y);
    }

}

void MObjectPanel::update_machine_info(MachineObject* info, bool is_my_devices)
{
    m_info = info;
    m_is_my_devices = is_my_devices;
    Refresh();
}

void MObjectPanel::on_mouse_enter(wxMouseEvent& evt)
{
    m_hover = true;
    Refresh();
}

void MObjectPanel::on_mouse_leave(wxMouseEvent& evt)
{
    m_hover = false;
    Refresh();
}

void MObjectPanel::on_mouse_left_up(wxMouseEvent& evt)
{
    if (m_is_my_devices) {
        if (m_info && m_info->is_lan_mode_printer()) {
            if (m_info->has_access_right() && m_info->is_avaliable()) {
                Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
                if (!dev) return;
                dev->set_selected_machine(m_info->get_dev_id());
            }
        }
        else {
            Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
            if (!dev) return;
            dev->set_selected_machine(m_info->get_dev_id());
        }
        wxCommandEvent event(EVT_DISSMISS_MACHINE_LIST);
        event.SetEventObject(this->GetParent()->GetParent());
        wxPostEvent(this, event);
    }
}

SelectMObjectPopup::SelectMObjectPopup(wxWindow* parent)
    :PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS), m_dismiss(false)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__


    SetSize(SELECT_MACHINE_POPUP_SIZE);
    SetMinSize(SELECT_MACHINE_POPUP_SIZE);
    SetMaxSize(SELECT_MACHINE_POPUP_SIZE);

    Freeze();
    wxBoxSizer* m_sizer_main = new wxBoxSizer(wxVERTICAL);
    SetBackgroundColour(SELECT_MACHINE_GREY400);



    m_scrolledWindow = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, SELECT_MACHINE_LIST_SIZE, wxHSCROLL | wxVSCROLL);
    m_scrolledWindow->SetBackgroundColour(*wxWHITE);
    m_scrolledWindow->SetMinSize(SELECT_MACHINE_LIST_SIZE);
    m_scrolledWindow->SetScrollRate(0, 5);
    auto m_sizxer_scrolledWindow = new wxBoxSizer(wxVERTICAL);
    m_scrolledWindow->SetSizer(m_sizxer_scrolledWindow);
    m_scrolledWindow->Layout();
    m_sizxer_scrolledWindow->Fit(m_scrolledWindow);

    m_sizer_my_devices = new wxBoxSizer(wxVERTICAL);
    m_sizxer_scrolledWindow->Add(m_sizer_my_devices, 0, wxEXPAND, 0);


    m_sizer_main->Add(m_scrolledWindow, 0, wxALL | wxEXPAND, FromDIP(2));

    SetSizer(m_sizer_main);
    Layout();
    Thaw();

#ifdef __APPLE__
    m_scrolledWindow->Bind(wxEVT_LEFT_UP, &SelectMObjectPopup::OnLeftUp, this);
#endif // __APPLE__

    m_refresh_timer = new wxTimer();
    m_refresh_timer->SetOwner(this);
    Bind(EVT_UPDATE_USER_MLIST, &SelectMObjectPopup::update_machine_list, this);
    Bind(wxEVT_TIMER, &SelectMObjectPopup::on_timer, this);
    Bind(EVT_DISSMISS_MACHINE_LIST, &SelectMObjectPopup::on_dissmiss_win, this);
}

SelectMObjectPopup::~SelectMObjectPopup() { delete m_refresh_timer; }

void SelectMObjectPopup::Popup(wxWindow* WXUNUSED(focus))
{
    BOOST_LOG_TRIVIAL(trace) << "get_print_info: start";
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

                wxGetApp().CallAfter([token, this, result, body]() {
                    if (token.expired()) {return;}
                    if (result == 0) {
                        m_print_info = body;
                    }
                    else {
                        m_print_info = "";
                    }

                    wxCommandEvent event(EVT_UPDATE_USER_MLIST);
                    event.SetEventObject(this);
                    wxPostEvent(this, event);
                });
            }));
        }
    }

    wxPostEvent(this, wxTimerEvent());
    PopupWindow::Popup();
}

void SelectMObjectPopup::OnDismiss()
{
    BOOST_LOG_TRIVIAL(trace) << "get_print_info: dismiss";
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

    wxCommandEvent event(EVT_FINISHED_UPDATE_MLIST);
    event.SetEventObject(this);
    wxPostEvent(this, event);
}

bool SelectMObjectPopup::ProcessLeftDown(wxMouseEvent& event) {
    return PopupWindow::ProcessLeftDown(event);
}

bool SelectMObjectPopup::Show(bool show) {
    if (show) {
        for (int i = 0; i < m_user_list_machine_panel.size(); i++) {
            m_user_list_machine_panel[i]->mPanel->update_machine_info(nullptr);
            m_user_list_machine_panel[i]->mPanel->Hide();
        }
    }
    return PopupWindow::Show(show);
}

void SelectMObjectPopup::on_timer(wxTimerEvent& event)
{
    BOOST_LOG_TRIVIAL(trace) << "SelectMObjectPopup on_timer";
    wxCommandEvent user_event(EVT_UPDATE_USER_MLIST);
    user_event.SetEventObject(this);
    wxPostEvent(this, user_event);
}

void SelectMObjectPopup::update_user_devices()
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
    for (auto& it : m_bind_machine_list) {
        user_machine_list.push_back(it);
    }

    std::sort(user_machine_list.begin(), user_machine_list.end(), [&](auto& a, auto& b) {
        if (a.second && b.second) {
            return a.second->get_dev_name().compare(b.second->get_dev_name()) < 0;
        }
        return false;
        });

    BOOST_LOG_TRIVIAL(trace) << "SelectMObjectPopup update_machine_list start";
    this->Freeze();
    m_scrolledWindow->Freeze();
    int i = 0;

    for (auto& elem : user_machine_list) {
        MachineObject* mobj = elem.second;
        MObjectPanel* op = nullptr;
        if (i < m_user_list_machine_panel.size()) {
            op = m_user_list_machine_panel[i]->mPanel;
            op->Show();
        }
        else {
            op = new MObjectPanel(m_scrolledWindow, wxID_ANY);
            MPanel* mpanel = new MPanel();
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
                if (mobj->has_access_right() && mobj->is_avaliable()) {
                    op->set_printer_state(PrinterState::IN_LAN);
                    op->SetToolTip(_L("Online"));
                }
                else {
                    op->set_printer_state(PrinterState::LOCK);
                }
            }
        }
        else {
            if (!mobj->is_online()) {
                op->SetToolTip(_L("Offline"));
                op->set_printer_state(PrinterState::OFFLINE);
            }
            else {
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

void SelectMObjectPopup::on_dissmiss_win(wxCommandEvent& event)
{
    Dismiss();
}

void SelectMObjectPopup::update_machine_list(wxCommandEvent& event)
{
    update_user_devices();
    BOOST_LOG_TRIVIAL(trace) << "SelectMObjectPopup update_machine_list end";
}

void SelectMObjectPopup::OnLeftUp(wxMouseEvent& event)
{
    auto mouse_pos = ClientToScreen(event.GetPosition());
    auto wxscroll_win_pos = m_scrolledWindow->ClientToScreen(wxPoint(0, 0));

    if (mouse_pos.x > wxscroll_win_pos.x && mouse_pos.y > wxscroll_win_pos.y && mouse_pos.x < (wxscroll_win_pos.x + m_scrolledWindow->GetSize().x) &&
        mouse_pos.y < (wxscroll_win_pos.y + m_scrolledWindow->GetSize().y)) {

        for (MPanel* p : m_user_list_machine_panel) {
            auto p_rect = p->mPanel->ClientToScreen(wxPoint(0, 0));
            if (mouse_pos.x > p_rect.x && mouse_pos.y > p_rect.y && mouse_pos.x < (p_rect.x + p->mPanel->GetSize().x) && mouse_pos.y < (p_rect.y + p->mPanel->GetSize().y)) {
                wxMouseEvent event(wxEVT_LEFT_UP);
                auto         tag_pos = p->mPanel->ScreenToClient(mouse_pos);
                event.SetPosition(tag_pos);
                event.SetEventObject(p->mPanel);
                wxPostEvent(p->mPanel, event);
            }
        }
    }
}


CalibrationPanel::CalibrationPanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : wxPanel(parent, id, pos, size, style),
    m_mobjectlist_popup(SelectMObjectPopup(this))
{
    SetBackgroundColour(*wxWHITE);

    init_tabpanel();

    wxBoxSizer* sizer_main = new wxBoxSizer(wxVERTICAL);
    sizer_main->Add(m_tabpanel, 1, wxEXPAND, 0);

    SetSizerAndFit(sizer_main);
    Layout();

    init_timer();
    Bind(wxEVT_TIMER, &CalibrationPanel::on_timer, this);
}

void CalibrationPanel::init_tabpanel() {
    m_side_tools = new SideTools(this, wxID_ANY);
    m_side_tools->get_panel()->Connect(wxEVT_LEFT_DOWN, wxMouseEventHandler(CalibrationPanel::on_printer_clicked), NULL, this);


    wxBoxSizer* sizer_side_tools = new wxBoxSizer(wxVERTICAL);
    sizer_side_tools->Add(m_side_tools, 1, wxEXPAND, 0);

    m_tabpanel = new Tabbook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, sizer_side_tools, wxNB_LEFT | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME);
    m_side_tools->set_table_panel(m_tabpanel);
    m_tabpanel->SetBackgroundColour(*wxWHITE);

    m_cali_panels[0] = new PressureAdvanceWizard(m_tabpanel);
    m_cali_panels[1] = new FlowRateWizard(m_tabpanel);
    //m_cali_panels[2] = new MaxVolumetricSpeedWizard(m_tabpanel);

    for (int i = 0; i < (int)CALI_MODE_COUNT; i++) {
        bool selected = false;
        if (i == 0)
            selected = true;
        m_tabpanel->AddPage(m_cali_panels[i],
            get_calibration_type_name(m_cali_panels[i]->get_calibration_mode()),
            "",
            selected);
    }

    for (int i = 0; i < (int)CALI_MODE_COUNT; i++)
        m_tabpanel->SetPageImage(i, "");

    auto padding_size = m_tabpanel->GetBtnsListCtrl()->GetPaddingSize(0);
    m_tabpanel->GetBtnsListCtrl()->SetPaddingSize({ FromDIP(15), padding_size.y });

    m_initialized = true;
}

void CalibrationPanel::init_timer()
{
    m_refresh_timer = new wxTimer();
    m_refresh_timer->SetOwner(this);
    m_refresh_timer->Start(REFRESH_INTERVAL);
    wxPostEvent(this, wxTimerEvent());
}

void CalibrationPanel::on_timer(wxTimerEvent& event) {
    update_all();
}

void CalibrationPanel::update_print_error_info(int code, std::string msg, std::string extra) {
    // update current wizard only
    int curr_selected = m_tabpanel->GetSelection();
    if (curr_selected >= 0 && curr_selected < CALI_MODE_COUNT) {
        if (m_cali_panels[curr_selected]) {
            auto page = m_cali_panels[curr_selected]->get_curr_step()->page;
            if (page) {
                if (page->get_page_type() == CaliPageType::CALI_PAGE_PRESET) {
                    auto preset_page = static_cast<CalibrationPresetPage*>(page);
                    preset_page->update_print_error_info(code, msg, extra);
                }
                if (page->get_page_type() == CaliPageType::CALI_PAGE_COARSE_SAVE) {
                    auto corase_page = static_cast<CalibrationFlowCoarseSavePage*>(page);
                    corase_page->update_print_error_info(code, msg, extra);
                }
            }
        }
    }
}

void CalibrationPanel::update_all() {

    NetworkAgent* m_agent = wxGetApp().getAgent();
    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;
    obj = dev->get_selected_machine();

    // check valid machine
    if (obj && dev->get_my_machine(obj->get_dev_id()) == nullptr) {
        dev->set_selected_machine("");
        if (m_agent) m_agent->set_user_selected_machine("");
        show_status((int) MONITOR_NO_PRINTER);
        return;
    }

    // update current wizard only
    int curr_selected = m_tabpanel->GetSelection();

    if (curr_selected >= 0 && curr_selected < CALI_MODE_COUNT) {
        if (m_cali_panels[curr_selected])
            m_cali_panels[curr_selected]->update(obj);
    }

    if (obj) {
        if (last_obj != obj && obj->is_info_ready()) {
            for (int i = 0; i < CALI_MODE_COUNT; i++) {
                m_cali_panels[i]->on_device_connected(obj);
            }
            last_obj = obj;
        }
    }

    if (obj && obj->connection_type() != last_conn_type) {
        last_conn_type = obj->connection_type();
    }

    m_side_tools->update_status(obj);

    if (!obj) {
        show_status((int)MONITOR_NO_PRINTER);
        return;
    }

    if (obj->is_connecting()) {
        show_status(MONITOR_CONNECTING);
        return;
    }
    else if (!obj->is_connected()) {
        int server_status = 0;
        // only disconnected server in cloud mode
        if (obj->connection_type() != "lan") {
            if (m_agent) {
                server_status = m_agent->is_server_connected() ? 0 : (int)MONITOR_DISCONNECTED_SERVER;
            }
        }
        show_status((int)MONITOR_DISCONNECTED + server_status);
        return;
    }

    show_status(MONITOR_NORMAL);
}

void CalibrationPanel::show_status(int status)
{
    if (!m_initialized) return;
    if (last_status == status)return;
    last_status = status;

    BOOST_LOG_TRIVIAL(info) << "monitor: show_status = " << status;


    Freeze();
    // update panels
    if (m_side_tools) { m_side_tools->show_status(status); };

    if ((status & (int)MonitorStatus::MONITOR_NO_PRINTER) != 0) {
        set_default();
        m_tabpanel->Layout();
    }
    else if (((status & (int)MonitorStatus::MONITOR_NORMAL) != 0)
        || ((status & (int)MonitorStatus::MONITOR_DISCONNECTED) != 0)
        || ((status & (int)MonitorStatus::MONITOR_DISCONNECTED_SERVER) != 0)
        || ((status & (int)MonitorStatus::MONITOR_CONNECTING) != 0))
    {

        if (((status & (int)MonitorStatus::MONITOR_DISCONNECTED) != 0)
            || ((status & (int)MonitorStatus::MONITOR_DISCONNECTED_SERVER) != 0)
            || ((status & (int)MonitorStatus::MONITOR_CONNECTING) != 0))
        {
            set_default();
        }
        m_tabpanel->Layout();
    }
    Layout();
    Thaw();
}

bool CalibrationPanel::Show(bool show) {
    if (show) {
        m_refresh_timer->Stop();
        m_refresh_timer->SetOwner(this);
        m_refresh_timer->Start(REFRESH_INTERVAL);
        wxPostEvent(this, wxTimerEvent());

        DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
        if (dev) {
            //set a default machine when obj is null
            obj = dev->get_selected_machine();
            if (obj == nullptr) {
                dev->load_last_machine();
            }
            else {
                obj->reset_update_time();
            }
        }

    }
    else {
        m_refresh_timer->Stop();
    }
    return wxPanel::Show(show);
}

void CalibrationPanel::on_printer_clicked(wxMouseEvent& event)
{
    auto mouse_pos = ClientToScreen(event.GetPosition());
    wxPoint rect = m_side_tools->ClientToScreen(wxPoint(0, 0));

    if (!m_side_tools->is_in_interval()) {
        wxPoint pos = m_side_tools->ClientToScreen(wxPoint(0, 0));
        pos.y += m_side_tools->GetRect().height;
        m_mobjectlist_popup.Move(pos);

#ifdef __linux__
        m_mobjectlist_popup.SetSize(wxSize(m_side_tools->GetSize().x, -1));
        m_mobjectlist_popup.SetMaxSize(wxSize(m_side_tools->GetSize().x, -1));
        m_mobjectlist_popup.SetMinSize(wxSize(m_side_tools->GetSize().x, -1));
#endif

        m_mobjectlist_popup.Popup();
    }
}

void CalibrationPanel::set_default()
{
    obj = nullptr;
    last_conn_type = "undefined";
}

void CalibrationPanel::msw_rescale()
{
    for (int i = 0; i < (int)CALI_MODE_COUNT; i++) {
        m_cali_panels[i]->msw_rescale();
    }
}

void CalibrationPanel::on_sys_color_changed()
{
    for (int i = 0; i < (int)CALI_MODE_COUNT; i++) {
        m_cali_panels[i]->on_sys_color_changed();
    }
}

CalibrationPanel::~CalibrationPanel() {
    m_side_tools->get_panel()->Disconnect(wxEVT_LEFT_DOWN, wxMouseEventHandler(CalibrationPanel::on_printer_clicked), NULL, this);
    if (m_refresh_timer)
        m_refresh_timer->Stop();
    delete m_refresh_timer;
}

}}
