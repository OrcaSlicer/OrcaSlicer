#include "CameraPopup.hpp"

#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/Utils.hpp"
#include "BitmapCache.hpp"
#include <wx/progdlg.h>
#include <wx/clipbrd.h>
#include <wx/dcgraph.h>
#include "GUI_App.hpp"
#include <slic3r/GUI/StatusPanel.hpp>

#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevStorage.h"

namespace Slic3r {
namespace GUI {

wxIMPLEMENT_CLASS(CameraPopup, PopupWindow);

wxBEGIN_EVENT_TABLE(CameraPopup, PopupWindow)
    EVT_MOUSE_EVENTS(CameraPopup::OnMouse )
    EVT_SIZE(CameraPopup::OnSize)
    EVT_SET_FOCUS(CameraPopup::OnSetFocus )
    EVT_KILL_FOCUS(CameraPopup::OnKillFocus )
wxEND_EVENT_TABLE()

wxDEFINE_EVENT(EVT_VCAMERA_SWITCH, wxMouseEvent);
wxDEFINE_EVENT(EVT_SDCARD_ABSENT_HINT, wxCommandEvent);
wxDEFINE_EVENT(EVT_CAM_SOURCE_CHANGE, wxCommandEvent);

#define CAMERAPOPUP_CLICK_INTERVAL 20

const wxColour TEXT_COL = wxColour(43, 52, 54);

CameraPopup::CameraPopup(wxWindow *parent)
   : PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif
    m_panel = new wxScrolledWindow(this, wxID_ANY);
    m_panel->SetBackgroundColour(*wxWHITE);
    m_panel->SetMinSize(wxSize(FromDIP(180),-1));
    m_panel->Bind(wxEVT_MOTION, &CameraPopup::OnMouse, this);

    main_sizer = new wxBoxSizer(wxVERTICAL);
    wxFlexGridSizer* top_sizer = new wxFlexGridSizer(0, 2, 0, FromDIP(50));
    top_sizer->AddGrowableCol(0);
    top_sizer->SetFlexibleDirection(wxBOTH);
    top_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

    //recording
    m_text_recording = new wxStaticText(m_panel, wxID_ANY, _L("Auto-record Monitoring"));
    m_text_recording->Wrap(-1);
    m_text_recording->SetFont(Label::Head_14);
    m_text_recording->SetForegroundColour(TEXT_COL);
    m_switch_recording = new SwitchButton(m_panel);

    //vcamera
    m_text_vcamera = new wxStaticText(m_panel, wxID_ANY, _L("Go Live"));
    m_text_vcamera->Wrap(-1);
    m_text_vcamera->SetFont(Label::Head_14);
    m_text_vcamera->SetForegroundColour(TEXT_COL);
    m_switch_vcamera = new SwitchButton(m_panel);

    top_sizer->Add(m_text_recording, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxALL, FromDIP(5));
    top_sizer->Add(m_switch_recording, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT | wxALL, FromDIP(5));
    top_sizer->Add(m_text_vcamera, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxALL, FromDIP(5));
    top_sizer->Add(m_switch_vcamera, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT | wxALL, FromDIP(5));

#if !BBL_RELEASE_TO_PUBLIC
    m_text_liveview_retry = new wxStaticText(m_panel, wxID_ANY, _L("Liveview Retry"));
    m_text_liveview_retry->Wrap(-1);
    m_text_liveview_retry->SetFont(Label::Head_14);
    m_text_liveview_retry->SetForegroundColour(TEXT_COL);
    m_switch_liveview_retry = new SwitchButton(m_panel);
    bool auto_retry         = wxGetApp().app_config->get("liveview", "auto_retry") != "false";
    m_switch_liveview_retry->SetValue(auto_retry);

    top_sizer->Add(m_text_liveview_retry, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxALL, FromDIP(5));
    top_sizer->Add(m_switch_liveview_retry, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT | wxALL, FromDIP(5));

    m_switch_liveview_retry->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        wxGetApp().app_config->set("liveview", "auto_retry", e.IsChecked());
        e.Skip();
    });
#endif

    //resolution
    m_text_resolution = new wxStaticText(m_panel, wxID_ANY, _L("Resolution"));
    m_text_resolution->Wrap(-1);
    m_text_resolution->SetFont(Label::Head_14);
    m_text_resolution->SetForegroundColour(TEXT_COL);
    top_sizer->Add(m_text_resolution, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxALL, FromDIP(5));
    top_sizer->Add(0, 0, wxALL, 0);
    for (int i = 0; i < (int)RESOLUTION_OPTIONS_NUM; ++i)
    {
        m_resolution_options[i] = create_item_radiobox(to_resolution_label_string(CameraResolution(i)), m_panel, wxEmptyString, FromDIP(10));
        top_sizer->Add(m_resolution_options[i], 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxALL, FromDIP(5));
        top_sizer->Add(0, 0, wxALL, 0);
    }

    // custom IP camera
    m_custom_camera_input_confirm = new Button(m_panel, _L("Enable"));
    m_custom_camera_input_confirm->SetBackgroundColor(wxColour(38, 166, 154));
    m_custom_camera_input_confirm->SetBorderColor(wxColour(38, 166, 154));
    m_custom_camera_input_confirm->SetTextColor(wxColour(0xFFFFFE));
    m_custom_camera_input_confirm->SetFont(Label::Body_14);
    m_custom_camera_input_confirm->SetMinSize(wxSize(FromDIP(90), FromDIP(30)));
    m_custom_camera_input_confirm->SetPosition(wxDefaultPosition);
    m_custom_camera_input_confirm->SetCornerRadius(FromDIP(12));
    m_custom_camera_input = new TextInput(m_panel, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxDefaultSize);
    m_custom_camera_input->GetTextCtrl()->SetHint(_L("Hostname or IP"));
    m_custom_camera_input->GetTextCtrl()->SetFont(Label::Body_14);
    m_custom_camera_hint = new wxStaticText(m_panel, wxID_ANY, _L("Custom camera source"));
    m_custom_camera_hint->Wrap(-1);
    m_custom_camera_hint->SetFont(Label::Head_14);
    m_custom_camera_hint->SetForegroundColour(TEXT_COL);

    m_custom_camera_input_confirm->Bind(wxEVT_BUTTON, &CameraPopup::on_camera_source_changed, this);

    if (!wxGetApp().app_config->get("camera", "custom_source").empty()) {
        m_custom_camera_input->GetTextCtrl()->SetValue(wxGetApp().app_config->get("camera", "custom_source"));
        set_custom_cam_button_state(wxGetApp().app_config->get("camera", "enable_custom_source") == "true");
    }

    top_sizer->Add(m_custom_camera_hint, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxALL, FromDIP(5));
    top_sizer->Add(0, 0, wxALL, 0);
    top_sizer->Add(m_custom_camera_input, 2, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxEXPAND | wxALL, FromDIP(5));
    top_sizer->Add(m_custom_camera_input_confirm, 1, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT | wxALL, FromDIP(5));
    main_sizer->Add(top_sizer, 0, wxALL, FromDIP(10));

    auto url = wxString::Format(L"https://wiki.bambulab.com/%s/software/bambu-studio/virtual-camera", L"en");
    auto text = _L("Show \"Live Video\" guide page.");

    wxBoxSizer* link_sizer = new wxBoxSizer(wxVERTICAL);
    vcamera_guide_link = new Label(m_panel, text);
    vcamera_guide_link->Wrap(-1);
    vcamera_guide_link->SetForegroundColour(wxColour(0x1F, 0x8E, 0xEA));
    auto text_size = vcamera_guide_link->GetTextExtent(text);
    vcamera_guide_link->Bind(wxEVT_LEFT_DOWN, [this, url](wxMouseEvent& e) {wxLaunchDefaultBrowser(url); });

    link_underline = new wxPanel(m_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    link_underline->SetBackgroundColour(wxColour(0x1F, 0x8E, 0xEA));
    link_underline->SetSize(wxSize(text_size.x, 1));
    link_underline->SetMinSize(wxSize(text_size.x, 1));

    vcamera_guide_link->Hide();
    link_underline->Hide();
    link_sizer->Add(vcamera_guide_link, 0, wxALL, 0);
    link_sizer->Add(link_underline, 0, wxALL, 0);

    main_sizer->Add(link_sizer, 0, wxALL, FromDIP(15));

    m_panel->SetSizer(main_sizer);
    m_panel->Layout();

    main_sizer->Fit(m_panel);

    SetClientSize(m_panel->GetSize());
    m_switch_recording->Connect(wxEVT_LEFT_DOWN, wxCommandEventHandler(CameraPopup::on_switch_recording), NULL, this);
    m_switch_vcamera->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        wxMouseEvent evt(EVT_VCAMERA_SWITCH);
        evt.SetEventObject(this);
        GetEventHandler()->ProcessEvent(evt);
        });
    #ifdef __APPLE__
    m_panel->Bind(wxEVT_LEFT_UP, &CameraPopup::OnLeftUp, this);
    #endif //APPLE

    this->Bind(wxEVT_TIMER, &CameraPopup::stop_interval, this);
    m_interval_timer = new wxTimer();
    m_interval_timer->SetOwner(this);

    wxGetApp().UpdateDarkUIWin(this);
}

void CameraPopup::sdcard_absent_hint()
{
    wxCommandEvent evt(EVT_SDCARD_ABSENT_HINT);
    evt.SetEventObject(this);
    GetEventHandler()->ProcessEvent(evt);
}

void CameraPopup::on_camera_source_changed(wxCommandEvent &event)
{
    if (m_obj && !m_custom_camera_input->GetTextCtrl()->IsEmpty()) {
        handle_camera_source_change();
    }
}

void CameraPopup::handle_camera_source_change()
{
    m_custom_camera_enabled = !m_custom_camera_enabled;

    set_custom_cam_button_state(m_custom_camera_enabled);

    wxGetApp().app_config->set("camera", "custom_source", m_custom_camera_input->GetTextCtrl()->GetValue().ToStdString());
    wxGetApp().app_config->set("camera", "enable_custom_source", m_custom_camera_enabled);

    wxCommandEvent evt(EVT_CAM_SOURCE_CHANGE);
    evt.SetEventObject(this);
    GetEventHandler()->ProcessEvent(evt);
}

void CameraPopup::set_custom_cam_button_state(bool state)
{
    m_custom_camera_enabled = state;
    auto stateColour = state ? wxColour(170, 0, 0) : wxColour(38, 166, 154);
    auto stateText = state ? "Disable" : "Enable";
    m_custom_camera_input_confirm->SetBackgroundColor(stateColour);
    m_custom_camera_input_confirm->SetBorderColor(stateColour);
    m_custom_camera_input_confirm->SetLabel(_L(stateText));
}

void CameraPopup::on_switch_recording(wxCommandEvent& event)
{
    if (!m_obj) return;
    if (m_obj->GetStorage()->get_sdcard_state()  != DevStorage::SdcardState::HAS_SDCARD_NORMAL) {
        sdcard_absent_hint();
        return;
    }
    bool value = m_switch_recording->GetValue();
    m_switch_recording->SetValue(!value);
    m_obj->command_ipcam_record(!value);
}

void CameraPopup::on_set_resolution()
{
    if (!m_obj) return;

    m_obj->command_ipcam_resolution_set(to_resolution_msg_string(curr_sel_resolution));
}

void CameraPopup::Popup(wxWindow *WXUNUSED(focus))
{
    wxPoint curr_position = this->GetPosition();
    wxSize win_size = this->GetSize();
    curr_position.x -= win_size.x;
    this->SetPosition(curr_position);

    if (!m_is_in_interval)
        PopupWindow::Popup();
}

wxWindow* CameraPopup::create_item_radiobox(wxString title, wxWindow* parent, wxString tooltip, int padding_left)
{
    wxWindow *item = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(20)));
    item->SetBackgroundColour(*wxWHITE);

    RadioBox *radiobox = new RadioBox(item);
    radiobox->SetPosition(wxPoint(padding_left, (item->GetSize().GetHeight() - radiobox->GetSize().GetHeight()) / 2));
    resolution_rbtns.push_back(radiobox);
    int btn_idx = resolution_rbtns.size() - 1;
    radiobox->Bind(wxEVT_LEFT_DOWN, [this, btn_idx](wxMouseEvent &e) {
        if (m_obj && allow_alter_resolution) {
            select_curr_radiobox(btn_idx);
            on_set_resolution();
        }
        });

    wxStaticText *text = new wxStaticText(item, wxID_ANY, title, wxDefaultPosition, wxDefaultSize);
    text->SetForegroundColour(*wxBLACK);
    resolution_texts.push_back(text);
    text->SetPosition(wxPoint(padding_left + radiobox->GetSize().GetWidth() + 10, (item->GetSize().GetHeight() - text->GetSize().GetHeight()) / 2));
    text->SetFont(Label::Body_13);
    text->SetForegroundColour(0x6B6B6B);
    text->Bind(wxEVT_LEFT_DOWN, [this, btn_idx](wxMouseEvent &e) {
        if (m_obj && allow_alter_resolution) {
            select_curr_radiobox(btn_idx);
            on_set_resolution();
        }
        });

    radiobox->SetToolTip(tooltip);
    text->SetToolTip(tooltip);
    return item;
}

void CameraPopup::select_curr_radiobox(int btn_idx)
{
    if (!m_obj) return;

    int len = resolution_rbtns.size();
    for (int i = 0; i < len; ++i) {
        if (i == btn_idx) {
            curr_sel_resolution = CameraResolution(i);
            resolution_rbtns[i]->SetValue(true);
        }
        else {
            resolution_rbtns[i]->SetValue(false);
        }
    }
}

void CameraPopup::sync_resolution_setting(std::string resolution)
{
    if (resolution == "") {
        reset_resolution_setting();
        return;
    }
    int res = 0;
    for (CameraResolution i = RESOLUTION_720P; i < RESOLUTION_OPTIONS_NUM; i = CameraResolution(i+1)){
        if (resolution == to_resolution_msg_string(i)) {
            res = int(i);
            break;
        }
    }
    select_curr_radiobox(res);
}

void CameraPopup::reset_resolution_setting()
{
    int len = resolution_rbtns.size();
    for (int i = 0; i < len; ++i) {
         resolution_rbtns[i]->SetValue(false);
    }
    curr_sel_resolution = RESOLUTION_OPTIONS_NUM;
}

void CameraPopup::sync_vcamera_state(bool show_vcamera)
{
    is_vcamera_show = show_vcamera;
    if (is_vcamera_show) {
        m_switch_vcamera->SetValue(true);
        vcamera_guide_link->Show();
        link_underline->Show();
    }
    else {
        m_switch_vcamera->SetValue(false);
        vcamera_guide_link->Hide();
        link_underline->Hide();
    }

    rescale();
}

void CameraPopup::check_func_supported(MachineObject *obj2)
{
    m_obj = obj2;
    if (m_obj == nullptr)
        return;
    // function supported
    if (m_obj->has_ipcam) {
        m_text_recording->Show();
        m_switch_recording->Show();
    } else {
        m_text_recording->Hide();
        m_switch_recording->Hide();
    }

    if (m_obj->virtual_camera && m_obj->has_ipcam) {
        m_text_vcamera->Show();
        m_switch_vcamera->Show();
        if (is_vcamera_show) {
            vcamera_guide_link->Show();
            link_underline->Show();
        }
    } else {
        m_text_vcamera->Hide();
        m_switch_vcamera->Hide();
        vcamera_guide_link->Hide();
        link_underline->Hide();
    }

    allow_alter_resolution = ( (m_obj->camera_resolution_supported.size() > 1?true:false) && m_obj->has_ipcam);

    //check u2 version
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;
    MachineObject* obj = dev->get_selected_machine();
    if (!obj) return;

    //resolution supported
    std::vector<std::string> resolution_supported = m_obj->get_resolution_supported();
    auto support_count = resolution_supported.size();
    for (int i = 0; i < (int)RESOLUTION_OPTIONS_NUM; ++i){
        auto curr_res = to_resolution_msg_string(CameraResolution(i));
        std::vector <std::string> ::iterator it = std::find(resolution_supported.begin(), resolution_supported.end(), curr_res);
        if ((it == resolution_supported.end())||(support_count <= 1) || !obj->is_support_1080dpi)
            m_resolution_options[i]->Hide();
        else {
            m_resolution_options[i]->Show();
            if (m_obj->camera_resolution == curr_res) {
                resolution_rbtns[i]->SetValue(true);
            }
        }
    }
    //hide resolution if there is only one choice
    if (support_count <= 1 || !obj->is_support_1080dpi) {
        m_text_resolution->Hide();
    }
    else {
        m_text_resolution->Show();
    }
}

void CameraPopup::update(bool vcamera_streaming)
{
    if (!m_obj) return;
    m_switch_recording->SetValue(m_obj->camera_recording_when_printing);
    sync_resolution_setting(m_obj->camera_resolution);
    sync_vcamera_state(vcamera_streaming);

    rescale();
}

wxString CameraPopup::to_resolution_label_string(CameraResolution resolution) {
    switch (resolution) {
    case RESOLUTION_720P:
        return "720p";
    case RESOLUTION_1080P:
        return "1080p";
    default:
        return "";
    }
    return "";
}

std::string CameraPopup::to_resolution_msg_string(CameraResolution resolution) {
    switch (resolution) {
    case RESOLUTION_720P:
        return std::string("720p");
    case RESOLUTION_1080P:
        return std::string("1080p");
    default:
        return "";
    }
    return "";
}

void CameraPopup::rescale()
{
    m_panel->Layout();
    main_sizer->Fit(m_panel);
    SetClientSize(m_panel->GetSize());
    PopupWindow::Update();
}

void CameraPopup::OnLeftUp(wxMouseEvent &event)
{
    auto mouse_pos = ClientToScreen(event.GetPosition());
    auto wxscroll_win_pos = m_panel->ClientToScreen(wxPoint(0, 0));

    if (mouse_pos.x > wxscroll_win_pos.x && mouse_pos.y > wxscroll_win_pos.y && mouse_pos.x < (wxscroll_win_pos.x + m_panel->GetSize().x) && mouse_pos.y < (wxscroll_win_pos.y + m_panel->GetSize().y)) {
        //recording
        auto recording_rect = m_switch_recording->ClientToScreen(wxPoint(0, 0));
        if (mouse_pos.x > recording_rect.x && mouse_pos.y > recording_rect.y && mouse_pos.x < (recording_rect.x + m_switch_recording->GetSize().x) && mouse_pos.y < (recording_rect.y + m_switch_recording->GetSize().y)) {
            wxMouseEvent recording_evt(wxEVT_LEFT_DOWN);
            m_switch_recording->GetEventHandler()->ProcessEvent(recording_evt);
            return;
        }
        //vcamera
        auto vcamera_rect = m_switch_vcamera->ClientToScreen(wxPoint(0, 0));
        if (mouse_pos.x > vcamera_rect.x && mouse_pos.y > vcamera_rect.y && mouse_pos.x < (vcamera_rect.x + m_switch_vcamera->GetSize().x) && mouse_pos.y < (vcamera_rect.y + m_switch_vcamera->GetSize().y)) {
            wxMouseEvent vcamera_evt(wxEVT_LEFT_DOWN);
            m_switch_vcamera->GetEventHandler()->ProcessEvent(vcamera_evt);
            return;
        }
        //resolution
        for (int i = 0; i < (int)RESOLUTION_OPTIONS_NUM; ++i){
            auto resolution_rbtn = resolution_rbtns[i];
            auto rbtn_rect = resolution_rbtn->ClientToScreen(wxPoint(0, 0));
            if (mouse_pos.x > rbtn_rect.x && mouse_pos.y > rbtn_rect.y && mouse_pos.x < (rbtn_rect.x + resolution_rbtn->GetSize().x) && mouse_pos.y < (rbtn_rect.y + resolution_rbtn->GetSize().y)) {
                wxMouseEvent resolution_evt(wxEVT_LEFT_DOWN);
                resolution_rbtn->GetEventHandler()->ProcessEvent(resolution_evt);
                return;
            }
            auto resolution_txt = resolution_texts[i];
            auto txt_rect = resolution_txt->ClientToScreen(wxPoint(0, 0));
            if (mouse_pos.x > txt_rect.x && mouse_pos.y > txt_rect.y && mouse_pos.x < (txt_rect.x + resolution_txt->GetSize().x) && mouse_pos.y < (txt_rect.y + resolution_txt->GetSize().y)) {
                wxMouseEvent resolution_evt(wxEVT_LEFT_DOWN);
                resolution_txt->GetEventHandler()->ProcessEvent(resolution_evt);
                return;
            }
        }
        //hyper link
        auto h_rect = vcamera_guide_link->ClientToScreen(wxPoint(0, 0));
        if (mouse_pos.x > h_rect.x && mouse_pos.y > h_rect.y && mouse_pos.x < (h_rect.x + vcamera_guide_link->GetSize().x) && mouse_pos.y < (h_rect.y + vcamera_guide_link->GetSize().y)) {
            auto url = wxString::Format(L"https://wiki.bambulab.com/%s/software/bambu-studio/virtual-camera", L"en");
            wxLaunchDefaultBrowser(url);
        }
    }
}

void CameraPopup::start_interval()
{
    m_interval_timer->Start(CAMERAPOPUP_CLICK_INTERVAL);
    m_is_in_interval = true;
}

void CameraPopup::stop_interval(wxTimerEvent& event)
{
    m_is_in_interval = false;
    m_interval_timer->Stop();
}

void CameraPopup::OnDismiss() {
    PopupWindow::OnDismiss();
    this->start_interval();
}

bool CameraPopup::ProcessLeftDown(wxMouseEvent &event)
{
    return PopupWindow::ProcessLeftDown(event);
}

bool CameraPopup::Show(bool show)
{
    return PopupWindow::Show(show);
}

void CameraPopup::OnSize(wxSizeEvent &event)
{
    event.Skip();
}

void CameraPopup::OnSetFocus(wxFocusEvent &event)
{
    event.Skip();
}

void CameraPopup::OnKillFocus(wxFocusEvent &event)
{
    event.Skip();
}

void CameraPopup::OnMouse(wxMouseEvent &event)
{
    event.Skip();
}

CameraItem::CameraItem(wxWindow *parent, std::string normal, std::string hover)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    m_bitmap_normal  = ScalableBitmap(this, normal, 20);
    m_bitmap_hover   = ScalableBitmap(this, hover, 20);

    SetSize(wxSize(FromDIP(20), FromDIP(20)));
    SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    SetMaxSize(wxSize(FromDIP(20), FromDIP(20)));
    Bind(wxEVT_PAINT, &CameraItem::paintEvent, this);
    Bind(wxEVT_ENTER_WINDOW, &CameraItem::on_enter_win, this);
    Bind(wxEVT_LEAVE_WINDOW, &CameraItem::on_level_win, this);
}

CameraItem::~CameraItem() {}

void CameraItem::msw_rescale() {
    m_bitmap_normal.msw_rescale();
    m_bitmap_hover.msw_rescale();
}

void CameraItem::on_enter_win(wxMouseEvent &evt)
{
    m_hover = true;
    Refresh();
}

void CameraItem::on_level_win(wxMouseEvent &evt)
{
    m_hover = false;
    Refresh();
}

void CameraItem::paintEvent(wxPaintEvent &evt)
{
    wxPaintDC dc(this);
    render(dc);

    // PrepareDC(buffdc);
    // PrepareDC(dc);
}

void CameraItem::render(wxDC &dc)
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

void CameraItem::doRender(wxDC &dc)
{
    if (m_hover) {
        dc.DrawBitmap(m_bitmap_hover.bmp(), wxPoint((GetSize().x - m_bitmap_hover.GetBmpSize().x) / 2, (GetSize().y - m_bitmap_hover.GetBmpSize().y) / 2));
    } else {
        dc.DrawBitmap(m_bitmap_normal.bmp(), wxPoint((GetSize().x - m_bitmap_normal.GetBmpSize().x) / 2, (GetSize().y - m_bitmap_normal.GetBmpSize().y) / 2));
    }
}

}
}