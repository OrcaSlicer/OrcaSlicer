#include "MultiMachinePage.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"

#include "DeviceCore/DevManager.h"

namespace Slic3r {
namespace GUI {


MultiMachinePage::MultiMachinePage(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : wxPanel(parent, id, pos, size, style)
{
    init_tabpanel();
    m_main_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_main_sizer->Add(m_tabpanel, 1, wxEXPAND | wxLEFT, 0);
    SetSizerAndFit(m_main_sizer);
    Layout();
    Fit();

    wxGetApp().UpdateDarkUIWin(this);

    init_timer();
    Bind(wxEVT_TIMER, &MultiMachinePage::on_timer, this);
}

MultiMachinePage::~MultiMachinePage()
{
    if (m_refresh_timer)
        m_refresh_timer->Stop();
    delete m_refresh_timer;
}

void MultiMachinePage::jump_to_send_page()
{
    m_tabpanel->SetSelection(1);
}

void MultiMachinePage::on_sys_color_changed()
{
}

void MultiMachinePage::msw_rescale()
{
    m_tabpanel->Rescale();
    if (m_local_task_manager)
        m_local_task_manager->msw_rescale();
    if (m_cloud_task_manager)
        m_cloud_task_manager->msw_rescale();
    if (m_machine_manager)
        m_machine_manager->msw_rescale();

    this->Fit();
    this->Layout();
    this->Refresh();
}

bool MultiMachinePage::Show(bool show)
{
    if (show) {
        m_refresh_timer->Stop();
        m_refresh_timer->SetOwner(this);
        m_refresh_timer->Start(2000);
        wxPostEvent(this, wxTimerEvent(*m_refresh_timer));
    }
    else {
        m_refresh_timer->Stop();
    }

    auto page = m_tabpanel->GetCurrentPage();
    if (page)
        page->Show(show);
    return wxPanel::Show(show);
}

void MultiMachinePage::init_tabpanel()
{
    auto m_side_tools = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(220), FromDIP(18)));
    wxBoxSizer* sizer_side_tools = new wxBoxSizer(wxHORIZONTAL);
    sizer_side_tools->Add(m_side_tools, 1, wxEXPAND, 0);
    m_tabpanel = new Tabbook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, sizer_side_tools, wxNB_LEFT | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME);
    m_tabpanel->SetBackgroundColour(wxColour("#FEFFFF"));
    m_tabpanel->Bind(wxEVT_BOOKCTRL_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {; });

    m_local_task_manager = new LocalTaskManagerPage(m_tabpanel);
    m_cloud_task_manager = new CloudTaskManagerPage(m_tabpanel);
    m_machine_manager = new MultiMachineManagerPage(m_tabpanel);

    m_tabpanel->AddPage(m_machine_manager, _L("Device"), "", true);
    m_tabpanel->AddPage(m_local_task_manager, _L("Task Sending"), "", false);
    m_tabpanel->AddPage(m_cloud_task_manager, _L("Task Sent"), "", false);
}

void MultiMachinePage::init_timer()
{
    m_refresh_timer = new wxTimer();
    //m_refresh_timer->SetOwner(this);
    //m_refresh_timer->Start(8000);
    //wxPostEvent(this, wxTimerEvent(*m_refresh_timer));
}

void MultiMachinePage::on_timer(wxTimerEvent& event)
{
    m_local_task_manager->update_page();
    m_cloud_task_manager->update_page();
    m_machine_manager->update_page();
}

void MultiMachinePage::clear_page()
{
    m_local_task_manager->refresh_user_device(true);
    m_cloud_task_manager->refresh_user_device(true);
    m_machine_manager->refresh_user_device(true);
}

DevicePickItem::DevicePickItem(wxWindow* parent, MachineObject* obj)
    : DeviceItem(parent, obj)
{
    SetBackgroundColour(*wxWHITE);
    m_bitmap_check_disable = ScalableBitmap(this, "check_off_disabled", 18);
    m_bitmap_check_off = ScalableBitmap(this, "check_off_focused", 18);
    m_bitmap_check_on = ScalableBitmap(this, "check_on", 18);


    SetMinSize(wxSize(FromDIP(400), FromDIP(30)));
    SetMaxSize(wxSize(FromDIP(400), FromDIP(30)));

    Bind(wxEVT_PAINT, &DevicePickItem::paintEvent, this);
    Bind(wxEVT_ENTER_WINDOW, &DevicePickItem::OnEnterWindow, this);
    Bind(wxEVT_LEAVE_WINDOW, &DevicePickItem::OnLeaveWindow, this);
    Bind(wxEVT_LEFT_DOWN, &DevicePickItem::OnLeftDown, this);
    Bind(wxEVT_MOTION, &DevicePickItem::OnMove, this);
    Bind(EVT_MULTI_DEVICE_SELECTED, &DevicePickItem::OnSelectedDevice, this);
    wxGetApp().UpdateDarkUIWin(this);
}

void DevicePickItem::DrawTextWithEllipsis(wxDC& dc, const wxString& text, int maxWidth, int left, int top /*= 0*/)
{
    wxSize size = GetSize();
    wxFont font = dc.GetFont();

    wxSize textSize = dc.GetTextExtent(text);
    dc.SetTextForeground(StateColor::darkModeColorFor(wxColour(50, 58, 61)));
    int textWidth = textSize.GetWidth();

    if (textWidth > maxWidth) {
        wxString truncatedText = text;
        int ellipsisWidth = dc.GetTextExtent("...").GetWidth();
        int numChars = text.length();

        for (int i = numChars - 1; i >= 0; --i) {
            truncatedText = text.substr(0, i) + "...";
            int truncatedWidth = dc.GetTextExtent(truncatedText).GetWidth();

            if (truncatedWidth <= maxWidth - ellipsisWidth) {
                break;
            }
        }

        if (top == 0) {
            dc.DrawText(truncatedText, left, (size.y - textSize.y) / 2);
        }
        else {
            dc.DrawText(truncatedText, left, (size.y - textSize.y) / 2 - top);
        }

    }
    else {
        if (top == 0) {
            dc.DrawText(text, left, (size.y - textSize.y) / 2);
        }
        else {
            dc.DrawText(text, left, (size.y - textSize.y) / 2 - top);
        }
    }
}

void DevicePickItem::OnEnterWindow(wxMouseEvent& evt)
{
    m_hover = true;
    Refresh(false);
}

void DevicePickItem::OnLeaveWindow(wxMouseEvent& evt)
{
    m_hover = false;
    Refresh(false);
}

void DevicePickItem::OnSelectedDevice(wxCommandEvent& evt)
{
    auto dev_id = evt.GetString();
    auto state = evt.GetInt();
    if (state == 0) {
        state_selected = 1;
    }
    else if (state == 1) {
        state_selected = 0;
    }
    Refresh(false);
    evt.Skip();

    post_event(wxCommandEvent(EVT_MULTI_DEVICE_SELECTED_FINHSH));
}

void DevicePickItem::OnLeftDown(wxMouseEvent& evt)
{
    int left = FromDIP(15);
    auto mouse_pos = ClientToScreen(evt.GetPosition());
    auto item = this->ClientToScreen(wxPoint(0, 0));

    if (mouse_pos.x > (item.x + left) &&
        mouse_pos.x < (item.x + left + m_bitmap_check_disable.GetBmpWidth()) &&
        mouse_pos.y > item.y &&
        mouse_pos.y < (item.y + DEVICE_ITEM_MAX_HEIGHT)) {

        post_event(wxCommandEvent(EVT_MULTI_DEVICE_SELECTED));
    }
}

void DevicePickItem::OnMove(wxMouseEvent& evt)
{
    int left = FromDIP(15);
    auto mouse_pos = ClientToScreen(evt.GetPosition());
    auto item = this->ClientToScreen(wxPoint(0, 0));

    if (mouse_pos.x > (item.x + left) &&
        mouse_pos.x < (item.x + left + m_bitmap_check_disable.GetBmpWidth()) &&
        mouse_pos.y > item.y &&
        mouse_pos.y < (item.y + DEVICE_ITEM_MAX_HEIGHT)) {
        SetCursor(wxCURSOR_HAND);
    }
    else {
        SetCursor(wxCURSOR_ARROW);
    }
}

void DevicePickItem::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    render(dc);
}

void DevicePickItem::render(wxDC& dc)
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

void DevicePickItem::doRender(wxDC& dc)
{
    wxSize size = GetSize();
    dc.SetPen(wxPen(*wxBLACK));

    int left = FromDIP(PICK_LEFT_PADDING_LEFT);


    //checkbox
    if (state_selected == 0) {
        dc.DrawBitmap(m_bitmap_check_off.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2));
    }
    else if (state_selected == 1) {
        dc.DrawBitmap(m_bitmap_check_on.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2));
    }

    left += FromDIP(PICK_LEFT_PRINTABLE);

    //dev names
    DrawTextWithEllipsis(dc, wxString::FromUTF8(get_obj()->get_dev_name()), FromDIP(PICK_LEFT_DEV_NAME), left);
    left += FromDIP(PICK_LEFT_DEV_NAME);
}
void DevicePickItem::post_event(wxCommandEvent&& event)
{
    event.SetEventObject(this);
    event.SetString(obj_->get_dev_id());
    event.SetInt(state_selected);
    wxPostEvent(this, event);
}

void DevicePickItem::DoSetSize(int x, int y, int width, int height, int sizeFlags /*= wxSIZE_AUTO*/)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
}

MultiMachinePickPage::MultiMachinePickPage(Plater* plater /*= nullptr*/)
    : DPIDialog(static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY,
        _L("Edit multiple printers"),
        wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    app_config = get_app_config();

    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    auto line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    line_top->SetBackgroundColour(wxColour(166, 169, 170));

    m_label = new Label(this, _L("Select connected printers (0/6)"));

    scroll_macine_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scroll_macine_list->SetSize(wxSize(FromDIP(400), FromDIP(10 * 30)));
    scroll_macine_list->SetMinSize(wxSize(FromDIP(400), FromDIP(10 * 30)));
    scroll_macine_list->SetMaxSize(wxSize(FromDIP(400), FromDIP(10 * 30)));
    scroll_macine_list->SetBackgroundColour(*wxWHITE);
    scroll_macine_list->SetScrollRate(0, 5);

    sizer_machine_list = new wxBoxSizer(wxVERTICAL);
    scroll_macine_list->SetSizer(sizer_machine_list);
    scroll_macine_list->Layout();

    main_sizer->Add(line_top, 0, wxEXPAND, 0);
    main_sizer->AddSpacer(FromDIP(10));
    main_sizer->Add(m_label, 0, wxLEFT, FromDIP(20));
    main_sizer->Add(scroll_macine_list, 0, wxLEFT|wxRIGHT, FromDIP(20));
    main_sizer->AddSpacer(FromDIP(10));

    SetSizer(main_sizer);
    Layout();
    Fit();
    Centre(wxBOTH);

    wxGetApp().UpdateDlgDarkUI(this);
}

MultiMachinePickPage::~MultiMachinePickPage()
{

}

int MultiMachinePickPage::get_selected_count()
{
    int count = 0;
    for (auto it = m_device_items.begin(); it != m_device_items.end(); it++) {
        if (it->second->state_selected == 1) {
            count++;
        }
    }
    return count;
}

void MultiMachinePickPage::update_selected_count()
{
    std::vector<std::string> selected_multi_devices;

    int count = 0;
    for (auto it = m_device_items.begin(); it != m_device_items.end(); it++) {
        if (it->second->state_selected == 1 ) {
            selected_multi_devices.push_back(it->second->obj_->get_dev_id());
            count++;
        }
    }

    m_selected_count = count;
    m_label->SetLabel(wxString::Format(_L("Select Connected Printers (%d/6)"), m_selected_count));

    if (m_selected_count > PICK_DEVICE_MAX) {
        MessageDialog msg_wingow(nullptr, wxString::Format(_L("The maximum number of printers that can be selected is %d"), PICK_DEVICE_MAX), "", wxAPPLY | wxOK);
        if (msg_wingow.ShowModal() == wxOK) {
            return;
        }
    }

    for (int i = 0; i < PICK_DEVICE_MAX; i++) {
        app_config->erase("multi_devices",std::to_string(i));
    }

    for (int j = 0; j < selected_multi_devices.size(); j++) {
        app_config->set_str("multi_devices",  std::to_string(j), selected_multi_devices[j]);
    }
    app_config->save();
}

void MultiMachinePickPage::on_dpi_changed(const wxRect& suggested_rect)
{

}

void MultiMachinePickPage::on_sys_color_changed()
{

}

void MultiMachinePickPage::refresh_user_device()
{
   std::vector<std::string> selected_multi_devices;

   for(int i = 0; i < PICK_DEVICE_MAX; i++){
       auto dev_id = app_config->get("multi_devices", std::to_string(i));
       selected_multi_devices.push_back(dev_id);
   }

    sizer_machine_list->Clear(false);
    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) {
        for (auto it = m_device_items.begin(); it != m_device_items.end(); it++) {
            wxWindow* child = it->second;
            child->Destroy();
        }
        return;
    }

    auto user_machine = dev->get_my_cloud_machine_list();
    auto task_manager = wxGetApp().getTaskManager();

    std::vector<std::string> subscribe_list;

    for (auto it = user_machine.begin(); it != user_machine.end(); ++it) {
        if (it->second->GetExtderSystem()->GetTotalExtderCount() > 1) { continue; }
        if (it->second->printer_type == "O1D") { continue;} /*maybe total_extder_count is not valid, hard codes here. to be moved to printers json*/

        DevicePickItem* di = new DevicePickItem(scroll_macine_list, it->second);

        di->Bind(EVT_MULTI_DEVICE_SELECTED_FINHSH, [this, di](auto& e) {
            int count = get_selected_count();
            if (count > PICK_DEVICE_MAX) {
                di->unselected();
                return;
            }
            update_selected_count();
        });

        /* if (m_device_items.find(it->first) != m_device_items.end()) {
             auto item = m_device_items[it->first];
             if (item->state_selected == 1 && di->state_printable <= 2)
                 di->state_selected = item->state_selected;
             item->Destroy();
         }*/
        m_device_items[it->first] = di;

        //update state
        if (task_manager) {
            m_device_items[it->first]->state_local_task = task_manager->query_task_state(it->first);
        }

        //update selected
        auto dev_it = std::find(selected_multi_devices.begin(), selected_multi_devices.end(), it->second->get_dev_id() );
        if (dev_it != selected_multi_devices.end()) {
            di->state_selected = 1;
        }

        sizer_machine_list->Add(di, 0, wxALL | wxEXPAND, 0);
        subscribe_list.push_back(it->first);
    }

    dev->subscribe_device_list(subscribe_list);

    sizer_machine_list->Layout();
    Layout();
    Fit();
}

void MultiMachinePickPage::on_confirm(wxCommandEvent& event)
{

}

bool MultiMachinePickPage::Show(bool show)
{
    if (show) {
        refresh_user_device();
        update_selected_count();
        //m_refresh_timer->Stop();
        //m_refresh_timer->SetOwner(this);
        //m_refresh_timer->Start(4000);
        //wxPostEvent(this, wxTimerEvent(*m_refresh_timer));
    }
    else {
        //m_refresh_timer->Stop();
        Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
        if (dev) {
            dev->subscribe_device_list(std::vector<std::string>());
        }
    }
    return wxDialog::Show(show);
}

} // namespace GUI
} // namespace Slic3r
