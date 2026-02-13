#include "DeviceErrorDialog.hpp"
#include "HMS.hpp"

#include "Widgets/Button.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "ReleaseNote.hpp"

namespace Slic3r {
namespace GUI
{

static std::unordered_set<std::string> message_containing_retry{
    "0701-8004",
    "0701-8005",
    "0701-8007",
    "0701-8012",
    "0702-8012",
    "0703-8012",
    "07FF-8012",
    "07FF-8013",
};


DeviceErrorDialog::DeviceErrorDialog(MachineObject* obj, wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style)
    :DPIDialog(parent, id, title, pos, size, style), m_obj(obj)
{
    std::string icon_path = (boost::format("%1%/images/OrcaSlicerTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));
    SetBackgroundColour(*wxWHITE);

    SetTitle(_L("Error"));

    auto        m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(350), 1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));

    m_scroll_area = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scroll_area->SetScrollRate(0, 5);
    m_scroll_area->SetBackgroundColour(*wxWHITE);
    m_scroll_area->SetMinSize(wxSize(FromDIP(320), FromDIP(250)));

    wxBoxSizer* text_sizer = new wxBoxSizer(wxVERTICAL);

    m_error_msg_label = new Label(m_scroll_area, wxEmptyString, LB_AUTO_WRAP);
    m_error_picture = new wxStaticBitmap(m_scroll_area, wxID_ANY, wxBitmap(), wxDefaultPosition, wxSize(FromDIP(300), FromDIP(180)));

    //Label* dev_name = new Label(m_scroll_area, wxString::FromUTF8(obj->dev_name) + ":", LB_AUTO_WRAP);
    //dev_name->SetMaxSize(wxSize(FromDIP(300), -1));
    //dev_name->SetMinSize(wxSize(FromDIP(300), -1));
    //dev_name->Wrap(FromDIP(300));
    //text_sizer->Add(dev_name, 0, wxALIGN_CENTER, FromDIP(5));
    //text_sizer->AddSpacer(5);
    text_sizer->Add(m_error_picture, 0, wxALIGN_CENTER, FromDIP(5));
    text_sizer->AddSpacer(10);
    text_sizer->Add(m_error_msg_label, 0, wxALIGN_CENTER, FromDIP(5));

    m_error_code_label = new Label(m_scroll_area, wxEmptyString, LB_AUTO_WRAP);
    text_sizer->AddSpacer(5);
    text_sizer->Add(m_error_code_label, 0, wxALIGN_CENTER, FromDIP(5));
    m_scroll_area->SetSizer(text_sizer);

    auto bottom_sizer = new wxBoxSizer(wxVERTICAL);
    m_sizer_button = new wxBoxSizer(wxVERTICAL);
    bottom_sizer->Add(m_sizer_button, 0, wxEXPAND | wxRIGHT | wxLEFT, 0);

    wxBoxSizer* m_center_sizer = new wxBoxSizer(wxVERTICAL);
    m_center_sizer->Add(0, 0, 1, wxTOP, FromDIP(5));
    m_center_sizer->Add(m_scroll_area, 0, wxEXPAND | wxRIGHT | wxLEFT, FromDIP(15));
    m_center_sizer->Add(bottom_sizer, 0, wxEXPAND | wxRIGHT | wxLEFT, FromDIP(20));
    m_center_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));

    m_sizer_main = new wxBoxSizer(wxVERTICAL);
    m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);
    m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(5));
    m_sizer_main->Add(m_center_sizer, 0, wxBOTTOM | wxEXPAND, FromDIP(5));

    SetSizer(m_sizer_main);
    Layout();
    m_sizer_main->Fit(this);

    init_button_list();

    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    Bind(wxEVT_WEBREQUEST_STATE, &DeviceErrorDialog::on_webrequest_state, this);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &e){
        if (m_obj) { m_obj->command_clean_print_error_uiop(m_obj->print_error); }
        e.Skip();
    });
}

DeviceErrorDialog::~DeviceErrorDialog()
{
    if (web_request.IsOk() && web_request.GetState() == wxWebRequest::State_Active)
    {
        BOOST_LOG_TRIVIAL(info) << "web_request: cancelled";
        web_request.Cancel();
    }
    m_error_picture->SetBitmap(wxBitmap());
}

void DeviceErrorDialog::on_webrequest_state(wxWebRequestEvent& evt)
{
    BOOST_LOG_TRIVIAL(trace) << "monitor: monitor_panel web request state = " << evt.GetState();
    switch (evt.GetState())
    {
    case wxWebRequest::State_Completed:
    {
        wxImage img(*evt.GetResponse().GetStream());
        wxImage resize_img = img.Scale(FromDIP(320), FromDIP(180), wxIMAGE_QUALITY_HIGH);
        wxBitmap error_prompt_pic = resize_img;
        m_error_picture->SetBitmap(error_prompt_pic);
        Layout();
        Fit();

        break;
    }
    case wxWebRequest::State_Failed:
    case wxWebRequest::State_Cancelled:
    case wxWebRequest::State_Unauthorized:
    {
        m_error_picture->SetBitmap(wxBitmap());
        break;
    }
    case wxWebRequest::State_Active:
    case wxWebRequest::State_Idle: break;
    default: break;
    }
}

void DeviceErrorDialog::init_button(ActionButton style, wxString buton_text)
{
    if (btn_bg_white.count() == 0)
    {
        btn_bg_white = StateColor(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
                                  std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                                  std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
    }

    Button* print_error_button = new Button(this, buton_text);
    print_error_button->SetBackgroundColor(btn_bg_white);
    print_error_button->SetBorderColor(wxColour(38, 46, 48));
    print_error_button->SetFont(Label::Body_14);
    print_error_button->SetSize(wxSize(FromDIP(300), FromDIP(30)));
    print_error_button->SetMinSize(wxSize(FromDIP(300), FromDIP(30)));
    print_error_button->SetMaxSize(wxSize(-1, FromDIP(30)));
    print_error_button->SetCornerRadius(FromDIP(5));
    print_error_button->Hide();
    m_button_list[style] = print_error_button;
    m_button_list[style]->Bind(wxEVT_LEFT_DOWN, [this, style](wxMouseEvent& e)
        {
            this->on_button_click(style);
            e.Skip();
        });
}

void DeviceErrorDialog::init_button_list()
{
    init_button(RESUME_PRINTING, _L("Resume Printing"));
    init_button(RESUME_PRINTING_DEFECTS, _L("Resume (defects acceptable)"));
    init_button(RESUME_PRINTING_PROBELM_SOLVED, _L("Resume (problem solved)"));
    init_button(STOP_PRINTING, _L("Stop Printing"));// pop up recheck dialog?
    init_button(CHECK_ASSISTANT, _L("Check Assistant"));
    init_button(FILAMENT_EXTRUDED, _L("Filament Extruded, Continue"));
    init_button(RETRY_FILAMENT_EXTRUDED, _L("Not Extruded Yet, Retry"));
    init_button(CONTINUE, _L("Finished, Continue"));
    init_button(LOAD_VIRTUAL_TRAY, _L("Load Filament"));
    init_button(OK_BUTTON, _L("OK"));
    init_button(FILAMENT_LOAD_RESUME, _L("Filament Loaded, Resume"));
    init_button(JUMP_TO_LIVEVIEW, _L("View Liveview"));
    init_button(NO_REMINDER_NEXT_TIME, _L("No Reminder Next Time"));
    init_button(IGNORE_NO_REMINDER_NEXT_TIME, _L("Ignore. Don't Remind Next Time"));
    init_button(IGNORE_RESUME, _L("Ignore this and Resume"));
    init_button(PROBLEM_SOLVED_RESUME, _L("Problem Solved and Resume"));
    init_button(TURN_OFF_FIRE_ALARM, _L("Got it, Turn off the Fire Alarm."));
    init_button(RETRY_PROBLEM_SOLVED, _L("Retry (problem solved)"));
    init_button(CANCLE, _L("Cancel"));
    init_button(STOP_DRYING, _L("Stop Drying"));
    init_button(PROCEED, _L("Proceed"));
    init_button(DBL_CHECK_CANCEL, _L("Cancel"));
    init_button(DBL_CHECK_DONE, _L("Done"));
    init_button(DBL_CHECK_RETRY, _L("Retry"));
    init_button(DBL_CHECK_RESUME, _L("Resume"));
    init_button(DBL_CHECK_OK, _L("Confirm"));
}

void DeviceErrorDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    for (auto used_button : m_used_button) { used_button->Rescale();}
    wxGetApp().UpdateDlgDarkUI(this);
    Refresh();
}

wxString DeviceErrorDialog::parse_error_level(int error_code)
{
    int level = (error_code & 0x0000F000) >> 12;
    switch (level) {
    case 0x4: return _L("Error");
    case 0x8: return _L("Warning");
    case 0xC: return _L("Info");
    default: return _L("Unknown");
    }
}

static const std::unordered_set<string> s_jump_liveview_error_codes = { "0300-8003", "0300-8002", "0300-800A"};
wxString DeviceErrorDialog::show_error_code(int error_code)
{
    if (m_error_code == error_code) { return wxEmptyString;}
    if (wxGetApp().get_hms_query()->is_internal_error(m_obj, error_code)) { return wxEmptyString;}

    /* error code str*/
    std::string error_str = m_obj->get_error_code_str(error_code);

    /* error code message*/
    wxString error_msg = wxGetApp().get_hms_query()->query_print_error_msg(m_obj, error_code);
    if (error_msg.IsEmpty()) { error_msg = _L("Unknown error.");}

    /* parse error level */
    wxString error_level = parse_error_level(error_code);

    /* error_str is old error code*/
    if (message_containing_retry.count(error_str)) {
        /* convert old error code to pseudo buttons*/
        std::vector<int> pseudo_button = convert_to_pseudo_buttons(error_str);

        /* do update*/
        update_contents(error_level, error_msg, error_str, wxEmptyString, pseudo_button);
    } else {
        /* action buttons*/
        std::vector<int> used_button;
        wxString         error_image_url = wxGetApp().get_hms_query()->query_print_image_action(m_obj, error_code, used_button);
        if (s_jump_liveview_error_codes.count(error_str)) { used_button.emplace_back(DeviceErrorDialog::JUMP_TO_LIVEVIEW); } // special case

        /* do update*/
        update_contents(error_level, error_msg, error_str, error_image_url, used_button);
    }

    wxGetApp().UpdateDlgDarkUI(this);
    Show();
    Raise();

    this->RequestUserAttention(wxUSER_ATTENTION_ERROR);

    return error_msg;
}

std::vector<int> DeviceErrorDialog::convert_to_pseudo_buttons(std::string error_str)
{
    std::vector<int> pseudo_button;

    pseudo_button.emplace_back(DBL_CHECK_RETRY);
    pseudo_button.emplace_back(DBL_CHECK_OK);

    return pseudo_button;
}

void DeviceErrorDialog::update_contents(const wxString& title, const wxString& text, const wxString& error_code, const wxString& image_url, const std::vector<int>& btns)
{
    if (error_code.empty()) { return; }

    /* buttons*/
    {
        m_sizer_button->Clear();
        m_used_button.clear();

        // Show the used buttons
        bool need_remove_close_btn = false;
        std::unordered_set<int> shown_btns;
        for (int button_id : btns)
        {
            need_remove_close_btn |= (button_id == REMOVE_CLOSE_BTN); // special case, do not show close button

            auto iter = m_button_list.find(button_id);
            if (iter != m_button_list.end())
            {
                m_sizer_button->Add(iter->second, 0, wxALL, FromDIP(5));
                iter->second->Show();
                m_used_button.insert(iter->second);
            }
        }

        // Special case, do not show close button
        if (need_remove_close_btn)
        {
            SetWindowStyle(GetWindowStyle() & ~wxCLOSE_BOX);
        }
        else
        {
            SetWindowStyle(GetWindowStyle() | wxCLOSE_BOX);
        }

        // Hide unused buttons
        for (const auto& pair : m_button_list)
        {
            if (m_used_button.count(pair.second) == 0) { pair.second->Hide(); }
        }
    }

    /* image */
    if (!image_url.empty())
    {
        const wxImage& img = wxGetApp().get_hms_query()->query_image_from_local(image_url);
        if (!img.IsOk() && image_url.Contains("http"))
        {
            web_request = wxWebSession::GetDefault().CreateRequest(this, image_url);
            BOOST_LOG_TRIVIAL(trace) << "monitor: create new webrequest, state = " << web_request.GetState();
            if (web_request.GetState() == wxWebRequest::State_Idle) web_request.Start();
            BOOST_LOG_TRIVIAL(trace) << "monitor: start new webrequest, state = " << web_request.GetState();
        }
        else
        {
            const wxImage& resize_img = img.Scale(FromDIP(320), FromDIP(180), wxIMAGE_QUALITY_HIGH);
            m_error_picture->SetBitmap(wxBitmap(resize_img));
        }

        m_error_picture->Show();
    }
    else
    {
        m_error_picture->Hide();
    }

    /* error code*/
    const wxString& show_time = wxDateTime::Now().Format("%H%M%d");
    const wxString& error_code_msg = wxString::Format("[%S %S]", error_code, show_time);
    m_error_code_label->SetMaxSize(wxSize(FromDIP(300), -1));
    m_error_code_label->SetMinSize(wxSize(FromDIP(300), -1));
    m_error_code_label->SetLabelText(error_code_msg);

    /* error message*/
    m_error_msg_label->SetMaxSize(wxSize(FromDIP(300), -1));
    m_error_msg_label->SetMinSize(wxSize(FromDIP(300), -1));
    m_error_msg_label->SetLabelText(text);

    /* dialog title*/
    SetTitle(title);

    /* update layout*/
    {
        m_scroll_area->Layout();
        auto text_size = m_error_msg_label->GetBestSize();
        if (text_size.y < FromDIP(360))
        {
            if (!image_url.empty())
            {
                m_scroll_area->SetMinSize(wxSize(FromDIP(320), text_size.y + FromDIP(220)));
            }
            else
            {
                m_scroll_area->SetMinSize(wxSize(FromDIP(320), text_size.y + FromDIP(50)));
            }
        }
        else
        {
            m_scroll_area->SetMinSize(wxSize(FromDIP(320), FromDIP(340)));
        }

        Layout();
        Fit();
    }
};

void DeviceErrorDialog::on_button_click(ActionButton btn_id)
{
    switch (btn_id) {
    case DeviceErrorDialog::RESUME_PRINTING: {
        m_obj->command_hms_resume(std::to_string(m_error_code), m_obj->job_id_);
        break;
    }
    case DeviceErrorDialog::RESUME_PRINTING_DEFECTS: {
        m_obj->command_hms_resume(std::to_string(m_error_code), m_obj->job_id_);
        break;
    }
    case DeviceErrorDialog::RESUME_PRINTING_PROBELM_SOLVED: {
        m_obj->command_hms_resume(std::to_string(m_error_code), m_obj->job_id_);
        break;
    }
    case DeviceErrorDialog::STOP_PRINTING: {
        m_obj->command_hms_stop(std::to_string(m_error_code), m_obj->job_id_);
        break;
    }
    case DeviceErrorDialog::CHECK_ASSISTANT: {
        wxGetApp().mainframe->m_monitor->jump_to_HMS(); // go to assistant page
        break;
    }
    case DeviceErrorDialog::FILAMENT_EXTRUDED: {
        m_obj->command_ams_control("done");
        break;
    }
    case DeviceErrorDialog::RETRY_FILAMENT_EXTRUDED: {
        m_obj->command_ams_control("resume");
        return;// do not hide the dialogs
    }
    case DeviceErrorDialog::CONTINUE: {
        m_obj->command_ams_control("resume");
        break;
    }
    case DeviceErrorDialog::LOAD_VIRTUAL_TRAY: {
        //m_ams_control->SwitchAms(std::to_string(VIRTUAL_TRAY_MAIN_ID));
        //on_ams_load_curr();
        break;/*AP, unknown what it is*/
    }
    case DeviceErrorDialog::OK_BUTTON: {
        m_obj->command_clean_print_error(m_obj->subtask_id_, m_error_code);
        break;/*do nothing*/
    }
    case DeviceErrorDialog::FILAMENT_LOAD_RESUME: {
        m_obj->command_hms_resume(std::to_string(m_error_code), m_obj->job_id_);
        break;
    }
    case DeviceErrorDialog::JUMP_TO_LIVEVIEW: {
        Slic3r::GUI::wxGetApp().mainframe->jump_to_monitor();
        Slic3r::GUI::wxGetApp().mainframe->m_monitor->jump_to_LiveView();
        break;
    }
    case DeviceErrorDialog::NO_REMINDER_NEXT_TIME: {
        m_obj->command_hms_idle_ignore(std::to_string(m_error_code), 0); /*the type is 0, supported by AP*/
        break;
    }
    case DeviceErrorDialog::IGNORE_NO_REMINDER_NEXT_TIME: {
        m_obj->command_hms_ignore(std::to_string(m_error_code), m_obj->job_id_);
        break;
    }
    case DeviceErrorDialog::IGNORE_RESUME: {
        m_obj->command_hms_ignore(std::to_string(m_error_code), m_obj->job_id_);
        break;
    }
    case DeviceErrorDialog::PROBLEM_SOLVED_RESUME: {
        m_obj->command_hms_resume(std::to_string(m_error_code), m_obj->job_id_);
        break;
    }
    case DeviceErrorDialog::TURN_OFF_FIRE_ALARM: {
        m_obj->command_stop_buzzer();
        break;
    }
    case DeviceErrorDialog::RETRY_PROBLEM_SOLVED: {
        m_obj->command_ams_control("resume");
        break;
    }
    case DeviceErrorDialog::CANCLE: {
        break;
    }
    case DeviceErrorDialog::STOP_DRYING: {
        m_obj->command_ams_drying_stop();
        break;
    }
    case DeviceErrorDialog::PROCEED: {
        if(!m_action_json.is_null()){
            try{
                m_obj->command_ack_proceed(m_action_json);
            } catch(...){
                BOOST_LOG_TRIVIAL(error) << "DeviceErrorDialog: Action Proceed missing params.";
            }
        }
        break;
    }
    case DeviceErrorDialog::ERROR_BUTTON_COUNT: break;

    case DeviceErrorDialog::DBL_CHECK_CANCEL: {
        // post EVT_SECONDARY_CHECK_CANCEL
        // no event
        break;
    }
    case DeviceErrorDialog::DBL_CHECK_DONE: {
        // post EVT_SECONDARY_CHECK_DONE
        m_obj->command_ams_control("done");
        break;
    }
    case DeviceErrorDialog::DBL_CHECK_RETRY: {
        // post EVT_SECONDARY_CHECK_RETRY
        wxCommandEvent event(EVT_SECONDARY_CHECK_RETRY);
        wxPostEvent(GetParent(), event);
        break;
    }
    case DeviceErrorDialog::DBL_CHECK_RESUME: {
        // post EVT_SECONDARY_CHECK_RESUME
        wxCommandEvent event(EVT_SECONDARY_CHECK_RESUME);
        wxPostEvent(GetParent(), event);
        break;
    }
    case DeviceErrorDialog::DBL_CHECK_OK: {
        // post EVT_SECONDARY_CHECK_CONFIRM
        m_obj->command_clean_print_error(m_obj->subtask_id_, m_error_code);
        m_obj->command_clean_print_error_uiop(m_error_code);
        break;
    }

    default: break;
    }

    Hide();
}

}
} // namespace Slic3r::GUI
