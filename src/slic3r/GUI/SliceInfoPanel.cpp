#include "SliceInfoPanel.hpp"

#include <boost/log/trivial.hpp>
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/Utils.hpp"
#include "GUI_App.hpp"//for  ICON_SIZE (wxSize(FromDIP(16), FromDIP(16)))

namespace Slic3r {
namespace GUI {

#define THUMBNAIL_SIZE  (wxSize(FromDIP(60), FromDIP(60)))
#define PRINT_ICON_SIZE (wxSize(FromDIP(18), FromDIP(18)))

wxIMPLEMENT_CLASS(SliceInfoPopup, PopupWindow);

wxBEGIN_EVENT_TABLE(SliceInfoPopup, PopupWindow)
    EVT_MOUSE_EVENTS( SliceInfoPopup::OnMouse )
    EVT_SIZE(SliceInfoPopup::OnSize)
    EVT_SET_FOCUS( SliceInfoPopup::OnSetFocus )
    EVT_KILL_FOCUS( SliceInfoPopup::OnKillFocus )
wxEND_EVENT_TABLE()

static wxColour BUTTON_BORDER_COL = wxColour(255, 255, 255);

inline float calc_gray(wxColour color)
{
    return 0.299 * (float) color.Red() + 0.587 * (float) color.Green() + 0.114 * (float) color.Blue();
}

static wxColour decode_color(const std::string &color)
{
    std::array<int, 3> ret = {0, 0, 0};
    const char *       c   = color.data() + 1;
    if (color.size() == 7 && color.front() == '#') {
        for (size_t j = 0; j < 3; ++j) {
            int digit1 = hex_digit_to_int(*c++);
            int digit2 = hex_digit_to_int(*c++);
            if (digit1 == -1 || digit2 == -1) break;

            ret[j] = float(digit1 * 16 + digit2);
        }
    }
    return wxColour(ret[0], ret[1], ret[2]);
}


SliceInfoPopup::SliceInfoPopup(wxWindow *parent, wxBitmap bmp, BBLSliceInfo *info)
   : PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif
    m_panel = new wxScrolledWindow(this, wxID_ANY);
    m_panel->SetBackgroundColour(*wxWHITE);

    m_panel->Bind(wxEVT_MOTION, &SliceInfoPopup::OnMouse, this);

    wxBoxSizer * main_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer * topSizer   = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer * caption_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer * caption_left_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer * caption_right_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto prediction_bitmap = new wxStaticBitmap(m_panel, wxID_ANY, create_scaled_bitmap("monitor_item_prediction", nullptr, 16));
    wxString predict_text;
    if (info)
        predict_text = get_bbl_monitor_time_dhm(info->prediction);
    auto prediction = new wxStaticText(m_panel, wxID_ANY, predict_text, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    caption_left_sizer->Add(prediction_bitmap, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    caption_left_sizer->Add(prediction, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    prediction->Wrap(-1);
    auto cost_bitmap = new wxStaticBitmap(m_panel, wxID_ANY, create_scaled_bitmap("monitor_item_cost", nullptr, 16));
    wxString cost_text;
    if (info) {
        if (info->weight > 0) {
            cost_text = wxString::Format("%.2fg", info->weight);
        } else {
            cost_text = "0g";
        }
    }
    auto used_g_text = new wxStaticText(m_panel, wxID_ANY, cost_text, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    caption_right_sizer->Add(cost_bitmap, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    caption_right_sizer->Add(used_g_text, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    caption_sizer->Add(caption_left_sizer, 1, 0, FromDIP(5));
    caption_sizer->Add(caption_right_sizer, 1, 0, FromDIP(5));

    topSizer->Add(caption_sizer, 0, wxEXPAND | wxALL, 0);
    auto static_line = new StaticLine(m_panel);
    topSizer->Add(static_line, 0, wxEXPAND | wxALL, 0);
    wxGridSizer *grid_sizer = new wxGridSizer(2, wxSize(FromDIP(10), 0));
    if (info) {
        for (auto f : info->filaments_info) {
            auto f_sizer = new wxBoxSizer(wxHORIZONTAL);
            auto f_type  = new Button(m_panel, f.type);
            f_type->SetBorderColor(BUTTON_BORDER_COL);
            wxColour color = decode_color(f.color);
            f_type->SetBackgroundColor(color);
            auto  textcolor = wxColour(0, 0, 0);
            if (calc_gray(color) <= 128)
                textcolor = wxColour(255, 255, 255);
            else
                textcolor = wxColour(0, 0, 0);

            f_type->SetTextColor(textcolor);
            f_type->SetSize(wxSize(FromDIP(40), FromDIP(20)));
            f_type->SetMinSize(wxSize(FromDIP(40), FromDIP(20)));
            f_type->SetMaxSize(wxSize(FromDIP(40), FromDIP(20)));
            f_type->SetCornerRadius(FromDIP(10));

            wxString used_g_text = wxString::Format("%.1fg", f.used_g);
            auto f_used_g = new wxStaticText(m_panel, wxID_ANY, used_g_text, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
            f_used_g->Wrap(-1);
            f_used_g->SetSize(wxSize(FromDIP(60), -1));
            f_sizer->Add(f_type, 0, wxEXPAND | wxALL, FromDIP(5));
            f_sizer->Add(f_used_g, 0, wxEXPAND | wxALL, FromDIP(5));
            grid_sizer->Add(f_sizer, 0, wxEXPAND, 0);
            f_type->Bind(wxEVT_LEFT_DOWN, [this](auto &e) {});
        }
    }
    topSizer->Add(grid_sizer, 0, wxALL, FromDIP(5));
    main_sizer->Add(FromDIP(13), 0, 0, 0);
    main_sizer->Add(topSizer, 0, wxEXPAND | wxALL, 0);
    main_sizer->Add(FromDIP(13), 0, 0, 0);
    main_sizer->SetMinSize(wxSize(FromDIP(200), -1));
    m_panel->SetSizer(main_sizer);
    m_panel->Layout();

    main_sizer->Fit(m_panel);

    SetClientSize(m_panel->GetSize());
}

void SliceInfoPopup::Popup(wxWindow *WXUNUSED(focus)) {
    PopupWindow::Popup();
}

void SliceInfoPopup::OnDismiss() {
    PopupWindow::OnDismiss();
}

bool SliceInfoPopup::ProcessLeftDown(wxMouseEvent &event)
{
    return PopupWindow::ProcessLeftDown(event);
}
bool SliceInfoPopup::Show(bool show)
{
    return PopupWindow::Show(show);
}

void SliceInfoPopup::OnSize(wxSizeEvent &event)
{
    event.Skip();
}

void SliceInfoPopup::OnSetFocus(wxFocusEvent &event)
{
    event.Skip();
}

void SliceInfoPopup::OnKillFocus(wxFocusEvent &event)
{
    event.Skip();
}

void SliceInfoPopup::OnMouse(wxMouseEvent &event)
{
    event.Skip();
}

SliceInfoPanel::SliceInfoPanel(wxWindow *parent, wxBitmap &prediction, wxBitmap &cost, wxBitmap &print,
    wxWindowID id, const wxPoint &pos, const wxSize &size, long style, const wxString &name)
    : wxPanel(parent, id, pos, size, style, name)
{
    this->SetBackgroundColour(*wxWHITE);

    m_item_top_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_bmp_item_thumbnail = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxDefaultSize, wxBU_AUTODRAW | 0);
    m_bmp_item_thumbnail->SetMinSize(THUMBNAIL_SIZE);
    m_bmp_item_thumbnail->SetSize(THUMBNAIL_SIZE);

    m_item_top_sizer->Add(m_bmp_item_thumbnail, 0, wxALL, 0);

    wxBoxSizer *m_item_content_sizer;
    m_item_content_sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer *m_item_info_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_bmp_item_prediction = new wxStaticBitmap(this, wxID_ANY, prediction);
    m_bmp_item_prediction->SetMinSize(ICON_SIZE);
    m_bmp_item_prediction->SetSize(ICON_SIZE);
    m_item_info_sizer->Add(m_bmp_item_prediction, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    m_text_item_prediction = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(60), -1));
    m_text_item_prediction->Wrap(-1);
    m_item_info_sizer->Add(m_text_item_prediction, 1, wxALIGN_CENTER_VERTICAL | wxALL, 0);

    m_bmp_item_cost = new wxStaticBitmap(this, wxID_ANY, cost);
    m_bmp_item_cost->SetMinSize(ICON_SIZE);
    m_bmp_item_cost->SetSize(ICON_SIZE);
    m_item_info_sizer->Add(m_bmp_item_cost, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    m_text_item_cost = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(35), -1));
    m_text_item_cost->Wrap(-1);
    m_item_info_sizer->Add(m_text_item_cost, 1, wxALIGN_CENTER_VERTICAL | wxALL, 0);

    m_item_content_sizer->Add(m_item_info_sizer, 0, wxEXPAND, 0);

    wxGridSizer *m_filament_info_sizer = new wxGridSizer(0, 3, 0, 8);

    m_item_content_sizer->Add(m_filament_info_sizer, 0, wxEXPAND, 0);

    m_item_top_sizer->Add(m_item_content_sizer, 0, wxEXPAND, 0);

    wxBoxSizer *m_item_right_sizer;
    m_item_right_sizer = new wxBoxSizer(wxVERTICAL);

    m_bmp_item_print = new wxStaticBitmap(this, wxID_ANY, print, wxDefaultPosition, wxDefaultSize, wxBU_AUTODRAW | 0);
    m_bmp_item_print->SetMinSize(PRINT_ICON_SIZE);
    m_bmp_item_print->SetSize(PRINT_ICON_SIZE);
    m_item_right_sizer->Add(m_bmp_item_print, 0, wxALL, FromDIP(5));

    m_item_right_sizer->Add(0, 0, 1, wxEXPAND, FromDIP(5));

    m_text_plate_index = new wxStaticText(this, wxID_ANY, "");
    m_text_plate_index->Wrap(-1);
    m_text_plate_index->SetForegroundColour(wxColour(107, 107, 107));
    m_item_right_sizer->Add(m_text_plate_index, 0, wxALIGN_RIGHT | wxALL, FromDIP(5));

    m_item_top_sizer->Add(m_item_right_sizer, 0, wxEXPAND, 0);

    this->SetSizer(m_item_top_sizer);
    this->Layout();

    Bind(wxEVT_WEBREQUEST_STATE, &SliceInfoPanel::on_webrequest_state, this);

    // Connect Events
    m_bmp_item_thumbnail->Connect(wxEVT_ENTER_WINDOW, wxMouseEventHandler(SliceInfoPanel::on_thumbnail_enter), NULL, this);
    m_bmp_item_thumbnail->Connect(wxEVT_LEAVE_WINDOW, wxMouseEventHandler(SliceInfoPanel::on_thumbnail_leave), NULL, this);
    m_bmp_item_print->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(SliceInfoPanel::on_subtask_print), NULL, this);
}

SliceInfoPanel::~SliceInfoPanel()
{
    // Disconnect Events
    m_bmp_item_thumbnail->Disconnect(wxEVT_ENTER_WINDOW, wxMouseEventHandler(SliceInfoPanel::on_thumbnail_enter), NULL, this);
    m_bmp_item_thumbnail->Disconnect(wxEVT_LEAVE_WINDOW, wxMouseEventHandler(SliceInfoPanel::on_thumbnail_leave), NULL, this);
    m_bmp_item_print->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(SliceInfoPanel::on_subtask_print), NULL, this);
}

void SliceInfoPanel::SetImages(wxBitmap &prediction, wxBitmap &cost, wxBitmap &printing)
{
    m_bmp_item_prediction->SetBitmap(prediction);
    m_bmp_item_cost->SetBitmap(cost);
    m_bmp_item_print->SetBitmap(printing);
}

void SliceInfoPanel::on_subtask_print(wxCommandEvent &evt)
{
    ;
}

void SliceInfoPanel::on_thumbnail_enter(wxMouseEvent &event)
{
    /*
    m_slice_info_popup = std::make_shared<SliceInfoPopup>(this);
    wxWindow *ctrl    = (wxWindow *) event.GetEventObject();
    wxPoint   pos     = ctrl->ClientToScreen(wxPoint(0, 0));
    wxSize    sz      = ctrl->GetSize();
    m_slice_info_popup->Position(pos, sz);
    m_slice_info_popup->Popup();
    */
}

void SliceInfoPanel::on_thumbnail_leave(wxMouseEvent &event)
{
    if (m_thumbnail_popup) { m_thumbnail_popup->Hide(); }
}

void SliceInfoPanel::on_mouse_enter(wxMouseEvent &event) { ; }

void SliceInfoPanel::on_mouse_leave(wxMouseEvent &event) { ; }

void SliceInfoPanel::on_webrequest_state(wxWebRequestEvent &evt)
{
    BOOST_LOG_TRIVIAL(trace) << "monitor: sub_task_panel web request state = " << evt.GetState();
    switch (evt.GetState()) {
    case wxWebRequest::State_Completed: {
        m_thumbnail_img    = *evt.GetResponse().GetStream();
        wxImage resize_img = m_thumbnail_img.Scale(m_bmp_item_thumbnail->GetSize().x, m_bmp_item_thumbnail->GetSize().y);
        m_bmp_item_thumbnail->SetBitmap(resize_img);
        break;
    }
    case wxWebRequest::State_Failed: {
        break;
    }
    case wxWebRequest::State_Cancelled:
    case wxWebRequest::State_Unauthorized:
    case wxWebRequest::State_Active:
    case wxWebRequest::State_Idle: break;
    default: break;
    }
}

void SliceInfoPanel::update(BBLSliceInfo *info)
{
    wxString prediction = wxString::Format("%s", get_bbl_time_dhms(info->prediction));
    m_text_item_prediction->SetLabelText(prediction);

    wxString weight = wxString::Format("%.2fg", info->weight);
    m_text_item_cost->SetLabelText(weight);

    m_text_plate_index->SetLabelText(info->index);

    if (web_request.IsOk()) web_request.Cancel();

    if (!info->thumbnail_url.empty()) {
        web_request = wxWebSession::GetDefault().CreateRequest(this, info->thumbnail_url);
        BOOST_LOG_TRIVIAL(trace) << "slice info: start reqeust thumbnail, url = " << info->thumbnail_url;
        web_request.Start();
    }

    this->Layout();
}

void SliceInfoPanel::msw_rescale()
{
    m_bmp_item_prediction->SetMinSize(ICON_SIZE);
    m_bmp_item_prediction->SetSize(ICON_SIZE);
    m_bmp_item_cost->SetMinSize(ICON_SIZE);
    m_bmp_item_cost->SetSize(ICON_SIZE);
    m_bmp_item_print->SetMinSize(PRINT_ICON_SIZE);
    m_bmp_item_print->SetSize(PRINT_ICON_SIZE);
    this->Layout();
}


}
}
