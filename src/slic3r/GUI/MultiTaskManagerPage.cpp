#include "MultiTaskManagerPage.hpp"
#include "I18N.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Widgets/RadioBox.hpp"
#include <wx/listimpl.cpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "DeviceCore/DevManager.h"

namespace Slic3r {
namespace GUI {

MultiTaskItem::MultiTaskItem(wxWindow* parent, MachineObject* obj, int type)
    : DeviceItem(parent, obj),
    m_task_type(type)
{
    SetBackgroundColour(*wxWHITE);
    SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));

    Bind(wxEVT_PAINT, &MultiTaskItem::paintEvent, this);
    Bind(wxEVT_ENTER_WINDOW, &MultiTaskItem::OnEnterWindow, this);
    Bind(wxEVT_LEAVE_WINDOW, &MultiTaskItem::OnLeaveWindow, this);
    Bind(wxEVT_LEFT_DOWN, &MultiTaskItem::OnLeftDown, this);
    Bind(wxEVT_MOTION, &MultiTaskItem::OnMove, this);
    Bind(EVT_MULTI_DEVICE_SELECTED, &MultiTaskItem::OnSelectedDevice, this);

    m_bitmap_check_disable = ScalableBitmap(this, "check_off_disabled", 18);
    m_bitmap_check_off = ScalableBitmap(this, "check_off_focused", 18);
    m_bitmap_check_on = ScalableBitmap(this, "check_on", 18);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* item_sizer = new wxBoxSizer(wxHORIZONTAL);


    auto m_btn_bg_enable = StateColor(
        std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal)
    );

    m_button_resume = new Button(this, _L("Resume"));
    m_button_resume->SetBackgroundColor(m_btn_bg_enable);
    m_button_resume->SetBorderColor(m_btn_bg_enable);
    m_button_resume->SetFont(Label::Body_12);
    m_button_resume->SetTextColor(StateColor::darkModeColorFor("#FFFFFE"));
    m_button_resume->SetMinSize(wxSize(FromDIP(70), FromDIP(35)));
    m_button_resume->SetCornerRadius(6);


    StateColor clean_bg(std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Disabled), std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered), std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Enabled),
        std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Normal));
    StateColor clean_bd(std::pair<wxColour, int>(wxColour(144, 144, 144), StateColor::Disabled), std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));
    StateColor clean_text(std::pair<wxColour, int>(wxColour(144, 144, 144), StateColor::Disabled), std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));

    m_button_cancel = new Button(this, _L("Cancel"));
    m_button_cancel->SetBackgroundColor(clean_bg);
    m_button_cancel->SetBorderColor(clean_bd);
    m_button_cancel->SetTextColor(clean_text);
    m_button_cancel->SetFont(Label::Body_12);
    m_button_cancel->SetCornerRadius(6);
    m_button_cancel->SetMinSize(wxSize(FromDIP(70), FromDIP(35)));

    m_button_pause = new Button(this, _L("Pause"));
    m_button_pause->SetBackgroundColor(clean_bg);
    m_button_pause->SetBorderColor(clean_bd);
    m_button_pause->SetTextColor(clean_text);
    m_button_pause->SetFont(Label::Body_12);
    m_button_pause->SetCornerRadius(6);
    m_button_pause->SetMinSize(wxSize(FromDIP(70), FromDIP(35)));

    m_button_stop = new Button(this, _L("Stop"));
    m_button_stop->SetBackgroundColor(clean_bg);
    m_button_stop->SetBorderColor(clean_bd);
    m_button_stop->SetTextColor(clean_text);
    m_button_stop->SetFont(Label::Body_12);
    m_button_stop->SetCornerRadius(6);
    m_button_stop->SetMinSize(wxSize(FromDIP(70), FromDIP(35)));


    item_sizer->Add(0, 0, 1, wxEXPAND, 0);
    item_sizer->Add(m_button_cancel, 0, wxALIGN_CENTER, 0);
    item_sizer->Add(m_button_resume, 0, wxALIGN_CENTER, 0);
    item_sizer->Add(m_button_pause, 0, wxALIGN_CENTER, 0);
    item_sizer->Add(m_button_stop, 0, wxALIGN_CENTER, 0);

    m_button_cancel->Hide();
    m_button_pause->Hide();
    m_button_resume->Hide();
    m_button_stop->Hide();

    main_sizer->Add(item_sizer, 1, wxEXPAND, 0);
    SetSizer(main_sizer);
    Layout();

    m_button_cancel->Bind(wxEVT_BUTTON, [this](auto& e) {
        onCancel();
    });

    m_button_pause->Bind(wxEVT_BUTTON, [this](auto& e) {
        onPause();
    });

    m_button_resume->Bind(wxEVT_BUTTON, [this](auto& e) {
        onResume();
    });

    m_button_stop->Bind(wxEVT_BUTTON, [this](auto& e) {
        onStop();
    });

    wxGetApp().UpdateDarkUIWin(this);
}

void MultiTaskItem::update_info()
{
    //local
    if (m_task_type == 0) {
        m_button_stop->Hide();
        m_button_pause->Hide();
        m_button_resume->Hide();
        if (state_local_task  == 0 || state_local_task == 1) {
            m_button_cancel->Show();
            Layout();
        }
        else {
            m_button_cancel->Hide();
            Layout();
        }
    }
    //cloud
    else if (m_task_type == 1 && get_obj() && (m_job_id == get_obj()->profile_id_)) {
        m_button_cancel->Hide();

        if (obj_ && obj_->is_in_printing() && state_cloud_task == 0 ) {
            if (obj_->can_abort()) {
                m_button_stop->Show();
            }
            else {
                m_button_stop->Hide();
            }

            if (obj_->can_pause()) {
                m_button_pause->Show();
            }
            else {
                m_button_pause->Hide();
            }

            if (obj_->can_resume()) {
                m_button_resume->Show();
            }
            else {
                m_button_resume->Hide();
            }

            Layout();
        }
        else {
            m_button_stop->Hide();
            m_button_pause->Hide();
            m_button_resume->Hide();
            Layout();
        }
    }
    else {
        m_button_cancel->Hide();
        m_button_stop->Hide();
        m_button_pause->Hide();
        m_button_resume->Hide();
        Layout();
    }
}

void MultiTaskItem::onPause()
{
    if (get_obj() && !get_obj()->can_resume()) {
        BOOST_LOG_TRIVIAL(info) << "MultiTask: pause current print task dev_id =" << get_obj()->get_dev_id();
        get_obj()->command_task_pause();
        m_button_pause->Hide();
        m_button_resume->Show();
        Layout();
    }
}

void MultiTaskItem::onResume()
{
    if (get_obj() && get_obj()->can_resume()) {
        BOOST_LOG_TRIVIAL(info) << "MultiTask: resume current print task dev_id =" << get_obj()->get_dev_id();
        get_obj()->command_task_resume();
        m_button_pause->Show();
        m_button_resume->Hide();
        Layout();
    }
}

void MultiTaskItem::onStop()
{
    if (get_obj()) {
        BOOST_LOG_TRIVIAL(info) << "MultiTask: abort current print task dev_id =" << get_obj()->get_dev_id();
        get_obj()->command_task_abort();
        m_button_pause->Hide();
        m_button_resume->Hide();
        m_button_stop->Hide();
        state_cloud_task = 2;
        Layout();
        Refresh();
    }
}


void MultiTaskItem::onCancel()
{
    if (task_obj) {
        task_obj->cancel();
        if (!task_obj->get_job_id().empty()) {
            get_obj()->command_task_cancel(task_obj->get_job_id());
        }
    }
}

void MultiTaskItem::OnEnterWindow(wxMouseEvent& evt)
{
    m_hover = true;
    Refresh();
}

void MultiTaskItem::OnLeaveWindow(wxMouseEvent& evt)
{
    m_hover = false;
    Refresh();
}

void MultiTaskItem::OnSelectedDevice(wxCommandEvent& evt)
{
    auto dev_id = evt.GetString();
    auto state = evt.GetInt();
    if (state == 0) {
        state_selected = 1;
    }
    else if (state == 1) {
        state_selected = 0;
    }
    Refresh();
}

void MultiTaskItem::OnLeftDown(wxMouseEvent& evt)
{
    int left = FromDIP(15);
    auto mouse_pos = ClientToScreen(evt.GetPosition());
    auto item = this->ClientToScreen(wxPoint(0, 0));

    if (mouse_pos.x > (item.x + left) &&
        mouse_pos.x < (item.x + left + m_bitmap_check_disable.GetBmpWidth()) &&
        mouse_pos.y > item.y &&
        mouse_pos.y < (item.y + DEVICE_ITEM_MAX_HEIGHT)) {

        if (m_task_type == 0 && state_local_task <= 1) {
            post_event(wxCommandEvent(EVT_MULTI_DEVICE_SELECTED));
        }
        else if (m_task_type == 1 && state_cloud_task == 0) {
            post_event(wxCommandEvent(EVT_MULTI_DEVICE_SELECTED));
        }
    }
}

void MultiTaskItem::OnMove(wxMouseEvent& evt)
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

void MultiTaskItem::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    render(dc);
}

void MultiTaskItem::render(wxDC& dc)
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

void MultiTaskItem::doRender(wxDC& dc)
{
    wxSize size = GetSize();
    dc.SetPen(wxPen(*wxBLACK));

    int left = FromDIP(TASK_LEFT_PADDING_LEFT);


    //checkbox
    if (m_task_type == 0) {
        if (state_local_task >= 2) {
            dc.DrawBitmap(m_bitmap_check_disable.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2));
        }
        else {
            if (state_selected == 0) {
                dc.DrawBitmap(m_bitmap_check_off.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2));
            }
            else if (state_selected == 1) {
                dc.DrawBitmap(m_bitmap_check_on.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2));
            }
        }
    }
    else if(m_task_type == 1){
        if (state_cloud_task != 0) {
            dc.DrawBitmap(m_bitmap_check_disable.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2));
        }
        else {
            if (state_selected == 0) {
                dc.DrawBitmap(m_bitmap_check_off.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2));
            }
            else if (state_selected == 1) {
                dc.DrawBitmap(m_bitmap_check_on.bmp(), wxPoint(left, (size.y - m_bitmap_check_disable.GetBmpSize().y) / 2));
            }
        }
    }


    left += FromDIP(TASK_LEFT_PRINTABLE);

    //project name
    DrawTextWithEllipsis(dc, m_project_name, FromDIP(TASK_LEFT_PRO_NAME), left);
    left += FromDIP(TASK_LEFT_PRO_NAME);

    //dev name
    DrawTextWithEllipsis(dc, m_dev_name, FromDIP(TASK_LEFT_DEV_NAME), left);
    left += FromDIP(TASK_LEFT_DEV_NAME);

    //local task state
    if (m_task_type == 0) {
        DrawTextWithEllipsis(dc, get_local_state_task(), FromDIP(TASK_LEFT_PRO_STATE), left);
    }
    else {
        DrawTextWithEllipsis(dc, get_cloud_state_task(), FromDIP(TASK_LEFT_PRO_STATE), left);
    }

    left += FromDIP(TASK_LEFT_PRO_STATE);

    //cloud task info
    if (m_task_type == 1) {
        if (get_obj()) {
            if (state_cloud_task == 0 && m_job_id == get_obj()->profile_id_) {
                dc.SetFont(Label::Body_13);
                if (state_device == 0) {
                    dc.SetTextForeground(*wxBLACK);
                    DrawTextWithEllipsis(dc, get_state_device(), FromDIP(DEVICE_LEFT_PRO_INFO), left);
                }
                else if (state_device == 1) {
                    dc.SetTextForeground(wxColour(0, 150, 136));
                    DrawTextWithEllipsis(dc, get_state_device(), FromDIP(DEVICE_LEFT_PRO_INFO), left);
                }
                else if (state_device == 2)
                {
                    dc.SetTextForeground(wxColour(208, 27, 27));
                    DrawTextWithEllipsis(dc, get_state_device(), FromDIP(DEVICE_LEFT_PRO_INFO), left);
                }
                else if (state_device > 2 && state_device < 7) {
                    dc.SetFont(Label::Body_12);
                    dc.SetTextForeground(wxColour(0, 150, 136));
                    if (obj_->get_curr_stage() == _L("Printing") && obj_->subtask_) {
                        //wxString layer_info = wxString::Format(_L("Layer: %d/%d"), obj_->curr_layer, obj_->total_layers);
                        wxString progress_info = wxString::Format("%d", obj_->subtask_->task_progress);
                        wxString left_time = wxString::Format("%s", get_left_time(obj_->mc_left_time));

                        DrawTextWithEllipsis(dc, progress_info + "%  |  " + left_time, FromDIP(TASK_LEFT_PRO_INFO), left, FromDIP(10));

                        dc.SetPen(wxPen(wxColour(233, 233, 233)));
                        dc.SetBrush(wxBrush(wxColour(233, 233, 233)));
                        dc.DrawRoundedRectangle(left, FromDIP(30), FromDIP(TASK_LEFT_PRO_INFO), FromDIP(10), 2);

                        dc.SetPen(wxPen(wxColour(0, 150, 136)));
                        dc.SetBrush(wxBrush(wxColour(0, 150, 136)));
                        dc.DrawRoundedRectangle(left, FromDIP(30), FromDIP(TASK_LEFT_PRO_INFO) * (static_cast<float>(obj_->subtask_->task_progress) / 100.0f), FromDIP(10), 2);
                    }
                    else {
                        DrawTextWithEllipsis(dc, obj_->get_curr_stage(), FromDIP(TASK_LEFT_PRO_INFO), left);
                    }
                }
                else {
                    dc.SetTextForeground(*wxBLACK);
                    DrawTextWithEllipsis(dc, get_state_device(), FromDIP(TASK_LEFT_PRO_INFO), left);
                }
            }
        }
    }
    else {
        if (state_local_task == 1) {
            wxString progress_info = wxString::Format("%d", m_sending_percent);
            DrawTextWithEllipsis(dc, progress_info + "% " , FromDIP(TASK_LEFT_PRO_INFO), left, FromDIP(10));

            dc.SetPen(wxPen(wxColour(233, 233, 233)));
            dc.SetBrush(wxBrush(wxColour(233, 233, 233)));
            dc.DrawRoundedRectangle(left, FromDIP(30), FromDIP(TASK_LEFT_PRO_INFO), FromDIP(10), 2);

            dc.SetPen(wxPen(wxColour(0, 150, 136)));
            dc.SetBrush(wxBrush(wxColour(0, 150, 136)));
            dc.DrawRoundedRectangle(left, FromDIP(30), FromDIP(TASK_LEFT_PRO_INFO) * (static_cast<float>(m_sending_percent) / 100.0f), FromDIP(10), 2);
        }
        /*else {
            if () {

            }
            if (m_button_cancel->IsShown()) {
                m_button_cancel->Hide();
                Layout();
            }
        }*/
    }
    left += FromDIP(TASK_LEFT_PRO_INFO);

    //send time
    dc.SetFont(Label::Body_13);
    dc.SetTextForeground(*wxBLACK);

    if (!boost::algorithm::contains(m_send_time, "1970")) {
        DrawTextWithEllipsis(dc, m_send_time, FromDIP(TASK_LEFT_SEND_TIME), left);
    }

    left += FromDIP(TASK_LEFT_SEND_TIME);

    if (m_hover) {
        dc.SetPen(wxPen(wxColour(0, 150, 136)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(0, 0, size.x, size.y, 3);
    }
}

void MultiTaskItem::DrawTextWithEllipsis(wxDC& dc, const wxString& text, int maxWidth, int left, int top) {
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

void MultiTaskItem::post_event(wxCommandEvent&& event)
{
    event.SetEventObject(this);
    event.SetString(m_dev_id);
    event.SetInt(state_selected);
    wxPostEvent(this, event);
}

void MultiTaskItem::DoSetSize(int x, int y, int width, int height, int sizeFlags /*= wxSIZE_AUTO*/)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
}

wxString MultiTaskItem::get_left_time(int mc_left_time)
{
    // update gcode progress
    std::string left_time;
    wxString    left_time_text = _L("N/A");

    try {
        left_time = get_bbl_monitor_time_dhm(mc_left_time);
    }
    catch (...) {
        ;
    }

    if (!left_time.empty()) left_time_text = wxString::Format("-%s", left_time);
    return left_time_text;
}


LocalTaskManagerPage::LocalTaskManagerPage(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__
    SetBackgroundColour(wxColour(0xEEEEEE));
    m_main_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_main_panel->SetBackgroundColour(*wxWHITE);
    m_main_sizer = new wxBoxSizer(wxVERTICAL);

    StateColor head_bg(
        std::pair<wxColour, int>(TABLE_HEAD_PRESSED_COLOUR, StateColor::Pressed),
        std::pair<wxColour, int>(TABLE_HEAR_NORMAL_COLOUR, StateColor::Normal)
    );

    m_table_head_panel = new wxPanel(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_table_head_panel->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_table_head_panel->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_table_head_panel->SetBackgroundColour(TABLE_HEAR_NORMAL_COLOUR);
    m_table_head_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_select_checkbox = new CheckBox(m_table_head_panel, wxID_ANY);
    m_select_checkbox->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_select_checkbox->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_table_head_sizer->Add(m_select_checkbox, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_select_checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        if (m_select_checkbox->GetValue()) {
            for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {

                if (it->second->state_local_task <= 1) {
                    it->second->selected();
                }
            }
        }
        else {
            for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
                it->second->unselected();
            }
        }
        Refresh(false);
        e.Skip();
        });


    m_task_name = new Button(m_table_head_panel, _L("Task Name"), "", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_task_name->SetBackgroundColor(TABLE_HEAR_NORMAL_COLOUR);
    m_task_name->SetFont(TABLE_HEAD_FONT);
    m_task_name->SetCornerRadius(0);
    m_task_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->SetCenter(false);
    m_table_head_sizer->Add(m_task_name, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_printer_name = new Button(m_table_head_panel, _L("Device Name"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_printer_name->SetBackgroundColor(head_bg);
    m_printer_name->SetFont(TABLE_HEAD_FONT);
    m_printer_name->SetCornerRadius(0);
    m_printer_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->SetCenter(false);
    m_printer_name->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_printer_name->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_printer_name->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_name_big = !device_name_big;
        this->m_sort.set_role(SortItem::SortRule::SR_DEV_NAME, device_name_big);
        this->refresh_user_device();
    });
    m_table_head_sizer->Add(m_printer_name, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_status = new Button(m_table_head_panel, _L("Task Status"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_status->SetBackgroundColor(head_bg);
    m_status->SetFont(TABLE_HEAD_FONT);
    m_status->SetCornerRadius(0);
    m_status->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->SetCenter(false);
    m_status->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_status->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_status->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_state_big = !device_state_big;
        this->m_sort.set_role(SortItem::SortRule::SR_LOCAL_TASK_STATE, device_state_big);
        this->refresh_user_device();
    });
    m_table_head_sizer->Add(m_status, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_info = new Button(m_table_head_panel, _L("Info"), "", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_info->SetBackgroundColor(TABLE_HEAR_NORMAL_COLOUR);
    m_info->SetFont(TABLE_HEAD_FONT);
    m_info->SetCornerRadius(0);
    m_info->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->SetCenter(false);
    m_table_head_sizer->Add(m_info, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_send_time = new Button(m_table_head_panel, _L("Sent Time"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE, false);
    m_send_time->SetBackgroundColor(head_bg);
    m_send_time->SetFont(TABLE_HEAD_FONT);
    m_send_time->SetCornerRadius(0);
    m_send_time->SetMinSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->SetMaxSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->SetCenter(false);
    m_send_time->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_send_time->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_send_time->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_send_time = !device_send_time;
        this->m_sort.set_role(SortItem::SortRule::SR_SEND_TIME, device_send_time);
        this->refresh_user_device();
    });
    m_table_head_sizer->Add(m_send_time, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_action = new Button(m_table_head_panel, _L("Actions"), "", wxNO_BORDER, ICON_SINGLE_SIZE, false);
    m_action->SetBackgroundColor(TABLE_HEAR_NORMAL_COLOUR);
    m_action->SetFont(TABLE_HEAD_FONT);
    m_action->SetCornerRadius(0);
    /* m_action->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
     m_action->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));*/
    m_action->SetCenter(false);
    m_table_head_sizer->Add(m_action, 0, wxALIGN_CENTER_VERTICAL, 0);
    m_table_head_panel->SetSizer(m_table_head_sizer);
    m_table_head_panel->Layout();

    m_tip_text = new wxStaticText(m_main_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_tip_text->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_tip_text->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_tip_text->SetLabel(_L("There are no tasks to be sent!"));
    m_tip_text->SetForegroundColour(wxColour(50, 58, 61));
    m_tip_text->SetFont(::Label::Head_24);
    m_tip_text->Wrap(-1);

    m_task_list = new wxScrolledWindow(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_task_list->SetBackgroundColour(*wxWHITE);
    m_task_list->SetScrollRate(0, 5);
    m_task_list->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_list->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), 10 * FromDIP(DEVICE_ITEM_MAX_HEIGHT)));

    m_sizer_task_list = new wxBoxSizer(wxVERTICAL);
    m_task_list->SetSizer(m_sizer_task_list);
    m_task_list->Layout();

    m_main_sizer->AddSpacer(FromDIP(50));
    m_main_sizer->Add(m_table_head_panel, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    m_main_sizer->Add(m_tip_text, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(50));
    m_main_sizer->Add(m_task_list, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    m_main_sizer->AddSpacer(FromDIP(5));

    // ctrl panel
    StateColor ctrl_bg(
        std::pair<wxColour, int>(CTRL_BUTTON_PRESSEN_COLOUR, StateColor::Pressed),
        std::pair<wxColour, int>(CTRL_BUTTON_NORMAL_COLOUR, StateColor::Normal)
    );

    m_ctrl_btn_panel = new wxPanel(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_ctrl_btn_panel->SetBackgroundColour(*wxWHITE);
    m_ctrl_btn_panel->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_ctrl_btn_panel->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_stop_all = new Button(m_ctrl_btn_panel, _L("Stop"));
    btn_stop_all->SetBackgroundColor(ctrl_bg);
    btn_stop_all->SetCornerRadius(FromDIP(5));
    m_sel_text = new wxStaticText(m_ctrl_btn_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize);

    m_btn_sizer->Add(m_sel_text, 0, wxLEFT, FromDIP(15));;
    m_btn_sizer->Add(btn_stop_all, 0, wxLEFT, FromDIP(10));
    m_ctrl_btn_panel->SetSizer(m_btn_sizer);
    m_ctrl_btn_panel->Layout();

    m_main_sizer->AddSpacer(FromDIP(10));
    m_main_sizer->Add(m_ctrl_btn_panel, 0, wxALIGN_CENTER_HORIZONTAL, 0);

    btn_stop_all->Bind(wxEVT_BUTTON, &LocalTaskManagerPage::cancel_all, this);
    m_main_panel->SetSizer(m_main_sizer);
    m_main_panel->Layout();

    page_sizer = new wxBoxSizer(wxVERTICAL);
    page_sizer->Add(m_main_panel, 1, wxALL | wxEXPAND, FromDIP(25));

    wxGetApp().UpdateDarkUIWin(this);

    SetSizer(page_sizer);
    Layout();
    Fit();
}

void LocalTaskManagerPage::update_page()
{
    for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
        it->second->update_info();
    }
}

void LocalTaskManagerPage::refresh_user_device(bool clear)
{
    m_sizer_task_list->Clear(false);

    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) {
        for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
            wxWindow* child = it->second;
            child->Destroy();
        }
        m_ctrl_btn_panel->Show(false);
        return;
    }

    if(clear)return;

    std::vector<std::string> subscribe_list;
    std::vector<MultiTaskItem*> task_temps;

    auto all_machine = dev->get_my_cloud_machine_list();
    auto user_machine = std::map<std::string, MachineObject*>();

    //selected machine
    for (int i = 0; i < PICK_DEVICE_MAX; i++) {
        auto dev_id = wxGetApp().app_config->get("multi_devices", std::to_string(i));

        if (all_machine.count(dev_id) > 0) {
            user_machine[dev_id] = all_machine[dev_id];
        }
    }

    auto task_manager = wxGetApp().getTaskManager();
    if (task_manager) {
        auto m_task_obj_list = task_manager->get_local_task_list();

        for (auto it = m_task_obj_list.rbegin(); it != m_task_obj_list.rend(); ++it) {

            TaskStateInfo* task_state_info = it->second;

            if(!task_state_info) continue;

            MultiTaskItem* mtitem = new MultiTaskItem(m_task_list, nullptr, 0);
            mtitem->task_obj = task_state_info;
            mtitem->m_project_name = wxString::FromUTF8(task_state_info->get_task_name());
            mtitem->m_dev_name = wxString::FromUTF8(task_state_info->get_device_name());
            mtitem->m_dev_id = task_state_info->params().dev_id;
            mtitem->m_send_time = task_state_info->get_sent_time();
            mtitem->state_local_task = task_state_info->state();

            task_state_info->set_state_changed_fn([this, mtitem](TaskState state, int percent) {
                mtitem->state_local_task = state;
                if (state == TaskState::TS_SEND_COMPLETED) {

                    mtitem->m_send_time = mtitem->task_obj->get_sent_time();
                    wxCommandEvent event(EVT_MULTI_REFRESH);
                    event.SetEventObject(mtitem);
                    wxPostEvent(mtitem, event);
                }
                mtitem->m_sending_percent = percent;
            });

            if (m_task_items.find(it->first) != m_task_items.end()) {
                MultiTaskItem* item = m_task_items[it->first];
                if (item->state_selected == 1 && mtitem->state_local_task < 2)
                    mtitem->state_selected = item->state_selected;
                item->Destroy();
            }

            m_task_items[it->first] = mtitem;
            task_temps.push_back(mtitem);
        }

        if (m_sort.rule != SortItem::SortRule::SR_None && m_sort.rule != SortItem::SortRule::SR_SEND_TIME) {
            std::sort(task_temps.begin(), task_temps.end(), m_sort.get_call_back());
        }

        for (const auto& item : task_temps)
            m_sizer_task_list->Add(item, 0, wxALL | wxEXPAND, 0);

        // maintenance
        auto it = m_task_items.begin();
        while (it != m_task_items.end()) {
            if (m_task_obj_list.find(it->first) != m_task_obj_list.end())
                ++it;
            else {
                it->second->Destroy();
                it = m_task_items.erase(it);
            }
        }

        dev->subscribe_device_list(subscribe_list);
        int num = m_task_items.size() > 10 ? 10 : m_task_items.size();
        m_task_list->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), num * FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
        m_task_list->Layout();
    }
    m_tip_text->Show(m_task_items.empty());
    m_ctrl_btn_panel->Show(!m_task_items.empty());
    Layout();
}

bool LocalTaskManagerPage::Show(bool show)
{
    if (show) {
        refresh_user_device();
    }
    else {
        Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
        if (dev) {
            dev->subscribe_device_list(std::vector<std::string>());
        }
    }
    return wxPanel::Show(show);
}

void LocalTaskManagerPage::cancel_all(wxCommandEvent& evt)
{
    for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
        if (it->second->m_button_cancel->IsShown() && (it->second->get_state_selected()  == 1) && it->second->state_local_task < 2) {
            it->second->onCancel();
        }
    }
}

void LocalTaskManagerPage::msw_rescale()
{
    m_select_checkbox->Rescale();
    m_select_checkbox->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_select_checkbox->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->Rescale();
    m_task_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->Rescale();
    m_printer_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->Rescale();
    m_status->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->Rescale();
    m_info->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->Rescale();
    m_send_time->SetMinSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->SetMaxSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_action->Rescale();
    m_action->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_action->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));

    btn_stop_all->Rescale();

    for (auto it = m_task_items.begin(); it != m_task_items.end(); ++it) {
        it->second->Refresh();
    }

    Fit();
    Layout();
    Refresh();
}

CloudTaskManagerPage::CloudTaskManagerPage(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__
    SetBackgroundColour(wxColour(0xEEEEEE));
    m_sort.set_role(SortItem::SR_SEND_TIME, true);

    SetBackgroundColour(wxColour(0xEEEEEE));
    m_main_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_main_panel->SetBackgroundColour(*wxWHITE);
    m_main_sizer = new wxBoxSizer(wxVERTICAL);

    StateColor head_bg(
        std::pair<wxColour, int>(TABLE_HEAD_PRESSED_COLOUR, StateColor::Pressed),
        std::pair<wxColour, int>(TABLE_HEAR_NORMAL_COLOUR, StateColor::Normal)
    );

    StateColor ctrl_bg(
        std::pair<wxColour, int>(CTRL_BUTTON_PRESSEN_COLOUR, StateColor::Pressed),
        std::pair<wxColour, int>(CTRL_BUTTON_NORMAL_COLOUR, StateColor::Normal)
    );

    m_table_head_panel = new wxPanel(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_table_head_panel->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_table_head_panel->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_table_head_panel->SetBackgroundColour(TABLE_HEAR_NORMAL_COLOUR);
    m_table_head_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_select_checkbox = new CheckBox(m_table_head_panel, wxID_ANY);
    m_select_checkbox->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_select_checkbox->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    //m_table_head_sizer->AddSpacer(FromDIP(TASK_LEFT_PADDING_LEFT));
    m_table_head_sizer->Add(m_select_checkbox, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_select_checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        if (m_select_checkbox->GetValue()) {
            for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {

                if (it->second->state_cloud_task == 0) {
                    it->second->selected();
                }
            }
        }
        else {
            for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
                it->second->unselected();
            }
        }
        Refresh(false);
        e.Skip();
    });



    m_task_name = new Button(m_table_head_panel, _L("Task Name"), "", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_task_name->SetBackgroundColor(TABLE_HEAR_NORMAL_COLOUR);
    m_task_name->SetFont(TABLE_HEAD_FONT);
    m_task_name->SetCornerRadius(0);
    m_task_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->SetCenter(false);
    m_table_head_sizer->Add(m_task_name, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_printer_name = new Button(m_table_head_panel, _L("Device Name"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_printer_name->SetBackgroundColor(head_bg);
    m_printer_name->SetFont(TABLE_HEAD_FONT);
    m_printer_name->SetCornerRadius(0);
    m_printer_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->SetCenter(false);
    m_printer_name->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_printer_name->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_printer_name->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_name_big = !device_name_big;
        this->m_sort.set_role(SortItem::SortRule::SR_DEV_NAME, device_name_big);
        this->refresh_user_device();
    });
    m_table_head_sizer->Add(m_printer_name, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_status = new Button(m_table_head_panel, _L("Task Status"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_status->SetBackgroundColor(head_bg);
    m_status->SetFont(TABLE_HEAD_FONT);
    m_status->SetCornerRadius(0);
    m_status->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->SetCenter(false);
    m_status->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_status->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_status->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_state_big = !device_state_big;
        this->m_sort.set_role(SortItem::SortRule::SR_CLOUD_TASK_STATE, device_state_big);
        this->refresh_user_device();
    });
    m_table_head_sizer->Add(m_status, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_info = new Button(m_table_head_panel, _L("Info"), "", wxNO_BORDER, ICON_SINGLE_SIZE);
    m_info->SetBackgroundColor(TABLE_HEAR_NORMAL_COLOUR);
    m_info->SetFont(TABLE_HEAD_FONT);
    m_info->SetCornerRadius(0);
    m_info->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->SetCenter(false);
    m_table_head_sizer->Add(m_info, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_send_time = new Button(m_table_head_panel, _L("Sent Time"), "toolbar_double_directional_arrow", wxNO_BORDER, ICON_SINGLE_SIZE, false);
    m_send_time->SetBackgroundColor(head_bg);
    m_send_time->SetFont(TABLE_HEAD_FONT);
    m_send_time->SetCornerRadius(0);
    m_send_time->SetMinSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->SetMaxSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->SetCenter(false);
    m_send_time->Bind(wxEVT_ENTER_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_HAND);
    });
    m_send_time->Bind(wxEVT_LEAVE_WINDOW, [&](wxMouseEvent& evt) {
        SetCursor(wxCURSOR_ARROW);
    });
    m_send_time->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        device_send_time = !device_send_time;
        this->m_sort.set_role(SortItem::SortRule::SR_SEND_TIME, device_send_time);
        this->refresh_user_device();
    });
    m_table_head_sizer->Add(m_send_time, 0, wxALIGN_CENTER_VERTICAL, 0);

    m_action = new Button(m_table_head_panel, _L("Actions"), "", wxNO_BORDER, ICON_SINGLE_SIZE, false);
    m_action->SetBackgroundColor(TABLE_HEAR_NORMAL_COLOUR);
    m_action->SetFont(TABLE_HEAD_FONT);
    m_action->SetCornerRadius(0);
    m_action->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_action->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_action->SetCenter(false);
    m_table_head_sizer->Add(m_action, 0, wxALIGN_CENTER_VERTICAL, 0);
    m_table_head_panel->SetSizer(m_table_head_sizer);
    m_table_head_panel->Layout();

    m_tip_text = new wxStaticText(m_main_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_tip_text->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_tip_text->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_tip_text->SetLabel(_L("No historical tasks!"));
    m_tip_text->SetForegroundColour(wxColour(50, 58, 61));
    m_tip_text->SetFont(::Label::Head_24);
    m_tip_text->Wrap(-1);

    m_loading_text = new wxStaticText(m_main_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_loading_text->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_loading_text->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_loading_text->SetLabel(_L("Loading..."));
    m_loading_text->SetForegroundColour(wxColour(50, 58, 61));
    m_loading_text->SetFont(::Label::Head_24);
    m_loading_text->Wrap(-1);
    m_loading_text->Show(false);

    m_task_list = new wxScrolledWindow(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_task_list->SetBackgroundColour(*wxWHITE);
    m_task_list->SetScrollRate(0, 5);
    m_task_list->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_list->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), 10 * FromDIP(DEVICE_ITEM_MAX_HEIGHT)));

    m_sizer_task_list = new wxBoxSizer(wxVERTICAL);
    m_task_list->SetSizer(m_sizer_task_list);
    m_task_list->Layout();
    m_task_list->Fit();

    m_main_sizer->AddSpacer(FromDIP(50));
    m_main_sizer->Add(m_table_head_panel, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    m_main_sizer->Add(m_tip_text, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(50));
    m_main_sizer->Add(m_loading_text, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(50));
    m_main_sizer->Add(m_task_list, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    m_main_sizer->AddSpacer(FromDIP(5));

    // add flipping page
    m_flipping_panel = new wxPanel(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_flipping_panel->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_flipping_panel->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_flipping_panel->SetBackgroundColour(*wxWHITE);

    m_flipping_page_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_page_sizer = new wxBoxSizer(wxVERTICAL);
    btn_last_page = new Button(m_flipping_panel, "", "go_last_plate", wxBORDER_NONE, FromDIP(20));
    btn_last_page->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    btn_last_page->SetMaxSize(wxSize(FromDIP(20), FromDIP(20)));
    btn_last_page->SetBackgroundColor(head_bg);
    btn_last_page->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [&](auto& evt) {
        evt.Skip();
        if (m_current_page == 0)
            return;
        enable_buttons(false);
        start_timer();
        m_current_page--;
        if (m_current_page < 0)
            m_current_page = 0;
        refresh_user_device();
        update_page_number();
        /*m_sizer_task_list->Clear(false);
        m_loading_text->Show(true);
        Layout();*/
    });
    st_page_number = new wxStaticText(m_flipping_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize);
    btn_next_page = new Button(m_flipping_panel, "", "go_next_plate", wxBORDER_NONE, FromDIP(20));
    btn_next_page->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    btn_next_page->SetMaxSize(wxSize(FromDIP(20), FromDIP(20)));
    btn_next_page->SetBackgroundColor(head_bg);
    btn_next_page->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [&](auto& evt) {
        evt.Skip();
        if (m_current_page == m_total_page - 1)
            return;
        enable_buttons(false);
        start_timer();
        m_current_page++;
        if (m_current_page > m_total_page - 1)
            m_current_page = m_total_page - 1;
        refresh_user_device();
        update_page_number();
        /*m_sizer_task_list->Clear(false);
        m_loading_text->Show(true);
        Layout();*/
    });

    m_page_num_input = new ::TextInput(m_flipping_panel, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(50), -1), wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    m_page_num_input->SetBackgroundColor(input_bg);
    m_page_num_input->GetTextCtrl()->SetValue("1");
    wxTextValidator validator(wxFILTER_DIGITS);
    m_page_num_input->GetTextCtrl()->SetValidator(validator);
    m_page_num_input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [&](wxCommandEvent& e) {
        page_num_enter_evt();
    });

    m_page_num_enter = new Button(m_flipping_panel, _("Go"));
    m_page_num_enter->SetMinSize(wxSize(FromDIP(25), FromDIP(25)));
    m_page_num_enter->SetMaxSize(wxSize(FromDIP(25), FromDIP(25)));
    m_page_num_enter->SetBackgroundColor(ctrl_bg);
    m_page_num_enter->SetCornerRadius(FromDIP(5));
    m_page_num_enter->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [&](auto& evt) {
        page_num_enter_evt();
    });

    m_flipping_page_sizer->Add(0, 0, 1, wxEXPAND, 0);
    m_flipping_page_sizer->Add(btn_last_page, 0, wxALIGN_CENTER, 0);
    m_flipping_page_sizer->Add(st_page_number, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    m_flipping_page_sizer->Add(btn_next_page, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    m_flipping_page_sizer->Add(m_page_num_input, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(20));
    m_flipping_page_sizer->Add(m_page_num_enter, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    m_flipping_page_sizer->Add(0, 0, 1, wxEXPAND, 0);
    m_page_sizer->Add(m_flipping_page_sizer, 0, wxALIGN_CENTER_HORIZONTAL, FromDIP(5));
    m_flipping_panel->SetSizer(m_page_sizer);
    m_flipping_panel->Layout();
    m_main_sizer->Add(m_flipping_panel, 0, wxALIGN_CENTER_HORIZONTAL, 0);

    m_ctrl_btn_panel = new wxPanel(m_main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_ctrl_btn_panel->SetBackgroundColour(*wxWHITE);
    m_ctrl_btn_panel->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_ctrl_btn_panel->SetMaxSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), -1));
    m_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_pause_all = new Button(m_ctrl_btn_panel, _L("Pause"));
    btn_pause_all->SetBackgroundColor(ctrl_bg);
    btn_pause_all->SetCornerRadius(FromDIP(5));
    btn_continue_all = new Button(m_ctrl_btn_panel, _L("Resume"));
    btn_continue_all->SetBackgroundColor(ctrl_bg);
    btn_continue_all->SetCornerRadius(FromDIP(5));
    btn_stop_all = new Button(m_ctrl_btn_panel, _L("Stop"));
    btn_stop_all->SetBackgroundColor(ctrl_bg);
    btn_stop_all->SetCornerRadius(FromDIP(5));
    m_sel_text = new wxStaticText(m_ctrl_btn_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize);

    btn_pause_all->Bind(wxEVT_BUTTON, &CloudTaskManagerPage::pause_all, this);
    btn_continue_all->Bind(wxEVT_BUTTON, &CloudTaskManagerPage::resume_all, this);
    btn_stop_all->Bind(wxEVT_BUTTON, &CloudTaskManagerPage::stop_all, this);

    m_btn_sizer->Add(m_sel_text, 0, wxLEFT, FromDIP(15));
    m_btn_sizer->Add(btn_pause_all, 0, wxLEFT, FromDIP(10));
    m_btn_sizer->Add(btn_continue_all, 0, wxLEFT, FromDIP(10));
    m_btn_sizer->Add(btn_stop_all, 0, wxLEFT, FromDIP(10));
    m_ctrl_btn_panel->SetSizer(m_btn_sizer);
    m_ctrl_btn_panel->Layout();

    m_main_sizer->AddSpacer(FromDIP(10));
    m_main_sizer->Add(m_ctrl_btn_panel, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    m_main_panel->SetSizer(m_main_sizer);
    m_main_panel->Layout();

    page_sizer = new wxBoxSizer(wxVERTICAL);
    page_sizer->Add(m_main_panel, 1, wxALL | wxEXPAND, FromDIP(25));
    Bind(wxEVT_TIMER, &CloudTaskManagerPage::on_timer, this);

    wxGetApp().UpdateDarkUIWin(this);

    SetSizer(page_sizer);
    Layout();
    Fit();
}

CloudTaskManagerPage::~CloudTaskManagerPage()
{
    if (m_flipping_timer)
        m_flipping_timer->Stop();
    delete m_flipping_timer;
}


void CloudTaskManagerPage::refresh_user_device(bool clear)
{
    m_sizer_task_list->Clear(false);

    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) {
        for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
            wxWindow* child = it->second;
            child->Destroy();
        }
        m_flipping_panel->Show(false);
        m_ctrl_btn_panel->Show(false);
        return;
    }

    if (clear) return;

    std::vector<MultiTaskItem*> task_temps;
    std::vector<std::string> subscribe_list;

    auto all_machine = dev->get_my_cloud_machine_list();
    auto user_machine = std::map<std::string, MachineObject*>();

    //selected machine
    for (int i = 0; i < PICK_DEVICE_MAX; i++) {
        auto dev_id = wxGetApp().app_config->get("multi_devices", std::to_string(i));

        if (all_machine.count(dev_id) > 0) {
            user_machine[dev_id] = all_machine[dev_id];
        }
    }

    auto task_manager = wxGetApp().getTaskManager();
    if (task_manager) {
        auto m_task_obj_list = task_manager->get_task_list(m_current_page, m_count_page_item, m_total_count);

        for (auto it = m_task_obj_list.begin(); it != m_task_obj_list.end(); it++) {

            TaskStateInfo task_state_info = it->second;
            MachineObject* machine_obj  = nullptr;

            if (user_machine.count(task_state_info.params().dev_id)) {
                machine_obj = user_machine[task_state_info.params().dev_id];
            }

            MultiTaskItem* mtitem = new MultiTaskItem(m_task_list, machine_obj, 1);
            //mtitem->task_obj = task_state_info;
            mtitem->m_job_id = task_state_info.get_job_id();
            mtitem->m_project_name = wxString::FromUTF8(task_state_info.get_task_name());
            mtitem->m_dev_name = wxString::FromUTF8(task_state_info.get_device_name());
            mtitem->m_dev_id = task_state_info.params().dev_id;

            mtitem->m_send_time = utc_time_to_date(task_state_info.start_time);

            if (task_state_info.state() == TS_PRINTING) {
                mtitem->state_cloud_task = 0;
            }
            else if (task_state_info.state() == TS_PRINT_SUCCESS) {
                mtitem->state_cloud_task = 1;
            }
            else if (task_state_info.state() == TS_PRINT_FAILED) {
                mtitem->state_cloud_task = 2;
            }

            if (m_task_items.find(it->first) != m_task_items.end()) {
                MultiTaskItem* item = m_task_items[it->first];
                if (item->state_selected == 1 && mtitem->state_cloud_task == 0)
                    mtitem->state_selected = item->state_selected;
                item->Destroy();
            }

            m_task_items[it->first] = mtitem;
            mtitem->update_info();
            task_temps.push_back(mtitem);

            auto find_it = std::find(subscribe_list.begin(), subscribe_list.end(), mtitem->m_dev_id);
            if (find_it == subscribe_list.end()) {
                subscribe_list.push_back(mtitem->m_dev_id);
            }
        }

        dev->subscribe_device_list(subscribe_list);

        if (m_sort.rule == SortItem::SortRule::SR_None) {
            this->device_send_time = true;
            m_sort.set_role(SortItem::SortRule::SR_SEND_TIME, device_send_time);
        }
        std::sort(task_temps.begin(), task_temps.end(), m_sort.get_call_back());

        for (const auto& item : task_temps)
            m_sizer_task_list->Add(item, 0, wxALL | wxEXPAND, 0);

        // maintenance
        auto it = m_task_items.begin();
        while (it != m_task_items.end()) {
            if (m_task_obj_list.find(it->first) != m_task_obj_list.end()) {
                ++it;
            }
            else {
                it->second->Destroy();
                it = m_task_items.erase(it);
            }
        }
        m_sizer_task_list->Layout();
        int num = m_task_items.size() > 10 ? 10 : m_task_items.size();
        m_task_list->SetMinSize(wxSize(FromDIP(CLOUD_TASK_ITEM_MAX_WIDTH), num * FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
        m_task_list->Layout();
    }

    update_page_number();

    m_tip_text->Show(m_task_items.empty());
    m_flipping_panel->Show(m_total_page > 1);
    m_ctrl_btn_panel->Show(!m_task_items.empty());
    Layout();
}

std::string CloudTaskManagerPage::utc_time_to_date(std::string utc_time)
{
    /*std::tm timeInfo = {};
    std::istringstream iss(utc_time);
    iss >> std::get_time(&timeInfo, "%Y-%m-%dT%H:%M:%SZ");

    std::chrono::system_clock::time_point tp = std::chrono::system_clock::from_time_t(std::mktime(&timeInfo));
    std::time_t localTime = std::chrono::system_clock::to_time_t(tp);
    std::tm* localTimeInfo = std::localtime(&localTime);

    std::stringstream ss;
    ss << std::put_time(localTimeInfo, "%Y-%m-%d %H:%M:%S");
    return ss.str();*/
    std::string send_time;


    std::tm timeInfo = {};
    std::istringstream iss(utc_time);
    iss >> std::get_time(&timeInfo, "%Y-%m-%dT%H:%M:%SZ");

    std::chrono::system_clock::time_point tp = std::chrono::system_clock::from_time_t(std::mktime(&timeInfo));
    std::time_t utcTime = std::chrono::system_clock::to_time_t(tp);


    wxDateTime::TimeZone tz(wxDateTime::Local);
    long offset = tz.GetOffset();


    std::time_t localTime = utcTime + offset;

    std::tm* localTimeInfo = std::localtime(&localTime);
    std::stringstream ss;
    ss << std::put_time(localTimeInfo, "%Y-%m-%d %H:%M:%S");
    send_time =  ss.str();


    return send_time;
}


bool CloudTaskManagerPage::Show(bool show)
{
    if (show) {
        refresh_user_device();
    }
    else {
        Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
        if (dev) {
            dev->subscribe_device_list(std::vector<std::string>());
        }
    }

    return wxPanel::Show(show);
}

void CloudTaskManagerPage::update_page()
{
    for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
        it->second->sync_state();
        it->second->update_info();
    }
}

void CloudTaskManagerPage::update_page_number()
{
    double result = static_cast<double>(m_total_count) / m_count_page_item;
    m_total_page = std::ceil(result);

    wxString number = wxString(std::to_string(m_current_page + 1)) + " / " + wxString(std::to_string(m_total_page));
    st_page_number->SetLabel(number);
}

void CloudTaskManagerPage::start_timer()
{
    if (m_flipping_timer) {
        m_flipping_timer->Stop();
    }
    else {
        m_flipping_timer = new wxTimer();
    }

    m_flipping_timer->SetOwner(this);
    m_flipping_timer->Start(1000);
    wxPostEvent(this, wxTimerEvent(*m_flipping_timer));
}

void CloudTaskManagerPage::on_timer(wxTimerEvent& event)
{
    m_flipping_timer->Stop();
    enable_buttons(true);
    update_page_number();
}

void CloudTaskManagerPage::pause_all(wxCommandEvent& evt)
{
    for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
        if (it->second->m_button_pause->IsShown() && (it->second->get_state_selected()  == 1) && it->second->state_cloud_task == 0) {
            it->second->onPause();
        }
    }
}

void CloudTaskManagerPage::resume_all(wxCommandEvent& evt)
{
    for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
        if (it->second->m_button_resume->IsShown() && (it->second->get_state_selected()  == 1) && it->second->state_cloud_task == 0) {
            it->second->onResume();
        }
    }
}

void CloudTaskManagerPage::stop_all(wxCommandEvent& evt)
{
    for (auto it = m_task_items.begin(); it != m_task_items.end(); it++) {
        if (it->second->m_button_stop->IsShown() && (it->second->get_state_selected()  == 1) && it->second->state_cloud_task == 0) {
            it->second->onStop();
        }
    }
}

void CloudTaskManagerPage::enable_buttons(bool enable)
{
    btn_last_page->Enable(enable);
    btn_next_page->Enable(enable);
    btn_pause_all->Enable(enable);
    btn_continue_all->Enable(enable);
    btn_stop_all->Enable(enable);
}

void CloudTaskManagerPage::page_num_enter_evt()
{
    enable_buttons(false);
    start_timer();
    auto value = m_page_num_input->GetTextCtrl()->GetValue();
    long page_num = 0;
    if (value.ToLong(&page_num)) {
        if (page_num > m_total_page)
            m_current_page = m_total_page - 1;
        else if (page_num < 1)
            m_current_page = 0;
        else
            m_current_page = page_num - 1;
    }
    refresh_user_device();
    update_page_number();
    /*m_sizer_task_list->Clear(false);
    m_loading_text->Show(true);
    Layout();*/
}

void CloudTaskManagerPage::msw_rescale()
{
    btn_last_page->Rescale();
    btn_last_page->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    btn_last_page->SetMaxSize(wxSize(FromDIP(20), FromDIP(20)));
    btn_next_page->Rescale();
    btn_next_page->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    btn_next_page->SetMaxSize(wxSize(FromDIP(20), FromDIP(20)));
    m_page_num_enter->Rescale();
    m_page_num_enter->SetMinSize(wxSize(FromDIP(25), FromDIP(25)));
    m_page_num_enter->SetMaxSize(wxSize(FromDIP(25), FromDIP(25)));

    m_select_checkbox->Rescale();
    m_select_checkbox->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_select_checkbox->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRINTABLE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->Rescale();
    m_task_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_task_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->Rescale();
    m_printer_name->SetMinSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_printer_name->SetMaxSize(wxSize(FromDIP(TASK_LEFT_DEV_NAME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->Rescale();
    m_status->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_status->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_STATE), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->Rescale();
    m_info->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_info->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->Rescale();
    m_send_time->SetMinSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_send_time->SetMaxSize(wxSize(FromDIP(TASK_LEFT_SEND_TIME), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_action->Rescale();
    m_action->SetMinSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));
    m_action->SetMaxSize(wxSize(FromDIP(TASK_LEFT_PRO_INFO), FromDIP(DEVICE_ITEM_MAX_HEIGHT)));

    btn_pause_all->Rescale();
    btn_continue_all->Rescale();
    btn_stop_all->Rescale();

    for (auto it = m_task_items.begin(); it != m_task_items.end(); ++it) {
        it->second->Refresh();
    }

    Fit();
    Layout();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
