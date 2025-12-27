#include "StatusPanel.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StepCtrl.hpp"
#include "Widgets/SideTools.hpp"
#include "Widgets/WebView.hpp"

#include "BitmapCache.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"

#include "MsgDialog.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/Thread.hpp"
#include "DeviceErrorDialog.hpp"

#include "RecenterDialog.hpp"
#include "CalibUtils.hpp"
#include <slic3r/GUI/Widgets/ProgressDialog.hpp>
#include <wx/display.h>
#include <wx/mstream.h>
#include <wx/sstream.h>
#include <wx/zstream.h>

#include "DeviceCore/DevBed.h"
#include "DeviceCore/DevCtrl.h"
#include "DeviceCore/DevFan.h"
#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevLamp.h"
#include "DeviceCore/DevStorage.h"

#include "DeviceCore/DevConfig.h"
#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevPrintTaskInfo.h"



#include "PrintOptionsDialog.hpp"
#include "SafetyOptionsDialog.hpp"

#include "ThermalPreconditioningDialog.hpp"


namespace Slic3r { namespace GUI {

#define TEMP_THRESHOLD_VAL 2
#define TEMP_THRESHOLD_ALLOW_E_CTRL 170.0f

/* const strings */
static const wxString NA_STR         = _L("N/A");
static const wxString TEMP_BLANK_STR = wxString("_");
static const wxFont   SWITCH_FONT    = Label::Body_10;

/* const values */
static const int bed_temp_range[2]    = {20, 120};
static const int default_champer_temp_min = 20;
static const int default_champer_temp_max = 60;

/* colors */
static const wxColour STATUS_PANEL_BG     = wxColour(238, 238, 238);
static const wxColour STATUS_TITLE_BG     = wxColour(248, 248, 248);
static const wxColour STATIC_BOX_LINE_COL = wxColour(238, 238, 238);

static const wxColour BUTTON_NORMAL1_COL = wxColour(238, 238, 238);
static const wxColour BUTTON_NORMAL2_COL = wxColour(206, 206, 206);
static const wxColour BUTTON_PRESS_COL   = wxColour(172, 172, 172);
static const wxColour BUTTON_HOVER_COL   = wxColour(0, 150, 136);

static const wxColour DISCONNECT_TEXT_COL = wxColour(171, 172, 172);
static const wxColour NORMAL_TEXT_COL     = wxColour(48,58,60);
static const wxColour NORMAL_FAN_TEXT_COL = wxColour(107, 107, 107);
static const wxColour WARNING_INFO_BG_COL = wxColour(255, 111, 0);
static const wxColour STAGE_TEXT_COL      = wxColour(0, 150, 136);

static const wxColour GROUP_STATIC_LINE_COL = wxColour(206, 206, 206);

/* font and foreground colors */
static const wxFont PAGE_TITLE_FONT  = Label::Body_14;
//static const wxFont GROUP_TITLE_FONT = Label::sysFont(17);

static wxColour PAGE_TITLE_FONT_COL  = wxColour(107, 107, 107);
static wxColour GROUP_TITLE_FONT_COL = wxColour(172, 172, 172);
static wxColour TEXT_LIGHT_FONT_COL  = wxColour(107, 107, 107);

static wxImage fail_image;


/* size */
#define PAGE_TITLE_HEIGHT FromDIP(36)
#define PAGE_TITLE_TEXT_WIDTH FromDIP(200)
#define PAGE_TITLE_LEFT_MARGIN FromDIP(17)
#define GROUP_TITLE_LEFT_MARGIN FromDIP(15)
#define GROUP_TITLE_LINE_MARGIN FromDIP(11)
#define GROUP_TITLE_RIGHT_MARGIN FromDIP(15)

#define NORMAL_SPACING FromDIP(5)
#define PAGE_SPACING FromDIP(10)
#define PAGE_MIN_WIDTH FromDIP(574)
#define PROGRESSBAR_HEIGHT FromDIP(8)

#define SWITCH_BUTTON_SIZE (wxSize(FromDIP(40), -1))
#define TASK_THUMBNAIL_SIZE (wxSize(FromDIP(120), FromDIP(120)))
#define TASK_BUTTON_SIZE (wxSize(FromDIP(48), FromDIP(24)))
#define TASK_BUTTON_SIZE2 (wxSize(-1, FromDIP(24)))
#define Z_BUTTON_SIZE (wxSize(FromDIP(44), FromDIP(40)))
#define MISC_BUTTON_PANEL_SIZE (wxSize(FromDIP(136), FromDIP(55)))
#define MISC_BUTTON_1FAN_SIZE (wxSize(FromDIP(132), FromDIP(51)))
#define MISC_BUTTON_2FAN_SIZE (wxSize(FromDIP(66), FromDIP(51)))
#define MISC_BUTTON_3FAN_SIZE (wxSize(FromDIP(44), FromDIP(51)))
#define TEMP_CTRL_MIN_SIZE_ALIGN_ONE_ICON (wxSize(FromDIP(125), FromDIP(52)))
#define TEMP_CTRL_MIN_SIZE_ALIGN_TWO_ICON (wxSize(FromDIP(145), FromDIP(48)))
#define AXIS_MIN_SIZE (wxSize(FromDIP(258), FromDIP(258)))
#define EXTRUDER_IMAGE_SIZE (wxSize(FromDIP(48), FromDIP(76)))

static void market_model_scoring_page(int design_id)
{
    std::string url;
    std::string country_code   = GUI::wxGetApp().app_config->get_country_code();
    std::string model_http_url = GUI::wxGetApp().get_model_http_url(country_code);
    if (GUI::wxGetApp().getAgent()->get_model_mall_detail_url(&url, std::to_string(design_id)) == 0) {
        std::string user_id = GUI::wxGetApp().getAgent()->get_user_id();
        boost::algorithm::replace_first(url, "models", "u/" + user_id + "/rating");
        // Prevent user_id from containing design_id
        size_t      sign_in = url.find("/rating");
        std::string sub_url = url.substr(0, sign_in + 7);
        url.erase(0, sign_in + 7);
        boost::algorithm::replace_first(url, std::to_string(design_id), "");
        url = sub_url + url;
        try {
            if (!url.empty()) { wxLaunchDefaultBrowser(url); }
        } catch (...) {
            return;
        }
    }
}

/*************************************************
Description:Extruder
**************************************************/

ExtruderImage::ExtruderImage(wxWindow* parent, wxWindowID id, int nozzle_num, const wxPoint& pos, const wxSize& size)
{
    wxWindow::Create(parent, id, pos, wxSize(FromDIP(45), FromDIP(112)));
    SetBackgroundColour(*wxWHITE);
    m_nozzle_num = nozzle_num;
    SetSize(wxSize(FromDIP(45), FromDIP(112)));
    SetMinSize(wxSize(FromDIP(45), FromDIP(112)));
    SetMaxSize(wxSize(FromDIP(45), FromDIP(112)));

    m_pipe_filled_load = new ScalableBitmap(this, "pipe_of_loading_selected", 50);
    m_pipe_filled_unload = new ScalableBitmap(this, "pipe_of_unloading_selected", 50);
    m_pipe_empty_load = new ScalableBitmap(this, "pipe_of_empty", 50);
    m_pipe_empty_unload = new ScalableBitmap(this, "pipe_of_empty", 50);
    m_pipe_filled_load_unselected = new ScalableBitmap(this, "pipe_of_loading_unselected", 50);
    m_pipe_filled_unload_unselected = new ScalableBitmap(this, "pipe_of_unloading_unselected", 50);
    m_pipe_empty_load_unselected = new ScalableBitmap(this, "pipe_of_empty", 50);
    m_pipe_empty_unload_unselected = new ScalableBitmap(this, "pipe_of_empty", 50);

    m_left_extruder_active_filled = new ScalableBitmap(this, "left_extruder_active_filled", 62);
    m_left_extruder_active_empty = new ScalableBitmap(this, "left_extruder_active_empty", 62);
    m_left_extruder_unactive_filled = new ScalableBitmap(this, "left_extruder_unactive_filled", 62);
    m_left_extruder_unactive_empty = new ScalableBitmap(this, "left_extruder_unactive_empty", 62);
    m_right_extruder_active_filled = new ScalableBitmap(this, "right_extruder_active_filled", 62);
    m_right_extruder_active_empty = new ScalableBitmap(this, "right_extruder_active_empty", 62);
    m_right_extruder_unactive_filled = new ScalableBitmap(this, "right_extruder_unactive_filled", 62);
    m_right_extruder_unactive_empty = new ScalableBitmap(this, "right_extruder_unactive_empty", 62);

    m_extruder_single_nozzle_empty_load = new ScalableBitmap(this, "monitor_extruder_empty_load", 106);
    m_extruder_single_nozzle_empty_unload = new ScalableBitmap(this, "monitor_extruder_empty_unload", 106);
    m_extruder_single_nozzle_filled_load = new ScalableBitmap(this, "monitor_extruder_filled_load", 106);
    m_extruder_single_nozzle_filled_unload = new ScalableBitmap(this, "monitor_extruder_filled_unload", 106);

    Bind(wxEVT_PAINT, &ExtruderImage::paintEvent, this);
}

ExtruderImage::~ExtruderImage() {}

void ExtruderImage::msw_rescale()
{
    //m_ams_extruder.SetSize(AMS_EXTRUDER_BITMAP_SIZE);
    //auto image     = m_ams_extruder.ConvertToImage();
    //m_extruder_pipe = ScalableBitmap(this, "pipe_of_extruder_control", 50);

    m_pipe_filled_load->msw_rescale();
    m_pipe_filled_unload->msw_rescale();
    m_pipe_empty_load->msw_rescale();
    m_pipe_empty_unload->msw_rescale();
    m_pipe_filled_load_unselected->msw_rescale();
    m_pipe_filled_unload_unselected->msw_rescale();
    m_pipe_empty_load_unselected->msw_rescale();
    m_pipe_empty_unload_unselected->msw_rescale();

    m_left_extruder_active_filled->msw_rescale();
    m_left_extruder_active_empty->msw_rescale();
    m_left_extruder_unactive_filled->msw_rescale();
    m_left_extruder_unactive_empty->msw_rescale();
    m_right_extruder_active_filled->msw_rescale();
    m_right_extruder_active_empty->msw_rescale();
    m_right_extruder_unactive_filled->msw_rescale();
    m_right_extruder_unactive_empty->msw_rescale();

    m_extruder_single_nozzle_empty_load->msw_rescale();
    m_extruder_single_nozzle_empty_unload->msw_rescale();
    m_extruder_single_nozzle_filled_load->msw_rescale();
    m_extruder_single_nozzle_filled_unload->msw_rescale();
    Layout();
    Refresh();
}

void ExtruderImage::setExtruderCount(int nozzle_num)
{
    m_nozzle_num = nozzle_num;
}

void ExtruderImage::setExtruderUsed(std::string loc)
{
    //current_nozzle_idx = nozzle_id;
    if (current_nozzle_loc == loc)
    {
        return;
    }

    current_nozzle_loc = loc;
    Refresh();
}

void ExtruderImage::update(ExtruderState single_state)
{
    m_single_ext_state = single_state;
}

void ExtruderImage::update(ExtruderState right_state, ExtruderState left_state) {
    m_left_ext_state = left_state;
    m_right_ext_state = right_state;
}

void ExtruderImage::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    render(dc);
}

void ExtruderImage::render(wxDC& dc)
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

void ExtruderImage::doRender(wxDC& dc)
{
    auto size = GetSize();
    //dc.DrawRectangle(0, FromDIP(5), size.x, size.y - FromDIP(5) - FromDIP(2));

    auto pot = wxPoint(size.x / 2, (size.y - m_pipe_filled_load->GetBmpSize().y - m_left_extruder_active_filled->GetBmpSize().y) / 2);

    if (m_nozzle_num >= 2){
        ScalableBitmap* left_nozzle_bmp;
        ScalableBitmap* right_nozzle_bmp;
        ScalableBitmap* left_pipe_bmp;
        ScalableBitmap* right_pipe_bmp;

        switch (m_right_ext_state)
        {
        case Slic3r::GUI::FILLED_LOAD:
            right_pipe_bmp   = current_nozzle_loc == "right" ? m_pipe_filled_load : m_pipe_filled_load_unselected;
            right_nozzle_bmp = current_nozzle_loc == "right" ? m_right_extruder_active_filled : m_right_extruder_unactive_filled;
            break;
        case Slic3r::GUI::FILLED_UNLOAD:
            right_pipe_bmp   = current_nozzle_loc == "right" ? m_pipe_filled_unload : m_pipe_filled_unload_unselected;
            right_nozzle_bmp = current_nozzle_loc == "right" ? m_right_extruder_active_filled : m_right_extruder_unactive_filled;
            break;
        case Slic3r::GUI::EMPTY_LOAD:
            right_pipe_bmp   = current_nozzle_loc == "right" ? m_pipe_empty_load : m_pipe_empty_load_unselected;
            right_nozzle_bmp = current_nozzle_loc == "right" ? m_right_extruder_active_empty : m_right_extruder_unactive_empty;
            break;
        case Slic3r::GUI::EMPTY_UNLOAD:
            right_pipe_bmp   = current_nozzle_loc == "right" ? m_pipe_empty_unload : m_pipe_empty_unload_unselected;
            right_nozzle_bmp = current_nozzle_loc == "right" ? m_right_extruder_active_empty : m_right_extruder_unactive_empty;
            break;
        default:
            break;
        }

        switch (m_left_ext_state)
        {
        case Slic3r::GUI::FILLED_LOAD:
            left_pipe_bmp   = current_nozzle_loc == "left" ? m_pipe_filled_load : m_pipe_filled_load_unselected;
            left_nozzle_bmp = current_nozzle_loc == "left" ? m_left_extruder_active_filled : m_left_extruder_unactive_filled;
            break;
        case Slic3r::GUI::FILLED_UNLOAD:
            left_pipe_bmp   = current_nozzle_loc == "left" ? m_pipe_filled_unload : m_pipe_filled_unload_unselected;
            left_nozzle_bmp = current_nozzle_loc == "left" ? m_left_extruder_active_filled : m_left_extruder_unactive_filled;
            break;
        case Slic3r::GUI::EMPTY_LOAD:
            left_pipe_bmp   = current_nozzle_loc == "left" ? m_pipe_empty_load : m_pipe_empty_load_unselected;
            left_nozzle_bmp = current_nozzle_loc == "left" ? m_left_extruder_active_empty : m_left_extruder_unactive_empty;
            break;
        case Slic3r::GUI::EMPTY_UNLOAD:
            left_pipe_bmp   = current_nozzle_loc == "left" ? m_pipe_empty_unload : m_pipe_empty_unload_unselected;
            left_nozzle_bmp = current_nozzle_loc == "left" ? m_left_extruder_active_empty : m_left_extruder_unactive_empty;
            break;
        default:
            break;
        }

        left_pipe_bmp = m_pipe_filled_load;
        right_pipe_bmp = m_pipe_filled_load;

        dc.DrawBitmap(left_pipe_bmp->bmp(), pot.x - left_nozzle_bmp->GetBmpWidth() / 2 - left_pipe_bmp->GetBmpWidth() / 2 + left_pipe_bmp->GetBmpWidth() / 5, pot.y);
        dc.DrawBitmap(left_nozzle_bmp->bmp(), pot.x - left_nozzle_bmp->GetBmpWidth(), pot.y + left_pipe_bmp->GetBmpSize().y);
        dc.DrawBitmap(right_pipe_bmp->bmp(), pot.x + right_nozzle_bmp->GetBmpWidth() / 2 - right_pipe_bmp->GetBmpWidth() / 2 - right_pipe_bmp->GetBmpWidth() / 5, pot.y);
        dc.DrawBitmap(right_nozzle_bmp->bmp(), pot.x, pot.y + right_pipe_bmp->GetBmpSize().y);
    }
    else{

        ScalableBitmap* nozzle_bmp = nullptr;
        switch (m_single_ext_state)
        {
            case Slic3r::GUI::FILLED_LOAD: nozzle_bmp = m_extruder_single_nozzle_filled_load; break;
            case Slic3r::GUI::FILLED_UNLOAD: nozzle_bmp = m_extruder_single_nozzle_filled_unload; break;
            case Slic3r::GUI::EMPTY_LOAD: nozzle_bmp = m_extruder_single_nozzle_empty_load; break;
            case Slic3r::GUI::EMPTY_UNLOAD:  nozzle_bmp = m_extruder_single_nozzle_empty_unload; break;
            default: break;
        }

        if (nozzle_bmp)
        {
            dc.DrawBitmap(nozzle_bmp->bmp(), pot.x - nozzle_bmp->GetBmpWidth() / 2, (size.y - nozzle_bmp->GetBmpHeight()) / 2);
        }
    }
}

#define SWITCHING_STATUS_BTN_SIZE wxSize(FromDIP(25), FromDIP(26))
ExtruderSwithingStatus::ExtruderSwithingStatus(wxWindow *parent)
    : wxPanel(parent)
{
    m_switching_status_label = new Label(this);
    m_switching_status_label->SetFont(::Label::Body_13);
    if (parent)
    { m_switching_status_label->SetBackgroundColour(parent->GetBackgroundColour());
    }

    m_button_quit = new Button(this, _CTX(L_CONTEXT("Quit", "Quit_Switching"), "Quit_Switching"), "", 0, FromDIP(22));
    m_button_quit->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    m_button_quit->Bind(wxEVT_BUTTON, &ExtruderSwithingStatus::on_quit, this);

    m_button_retry = new Button(this, _L("Retry"), "", 0, FromDIP(22));
    m_button_retry->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    m_button_retry->Bind(wxEVT_BUTTON, &ExtruderSwithingStatus::on_retry, this);

    wxBoxSizer *btn_sizer  = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->Add(m_button_quit, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    btn_sizer->Add(m_button_retry, 0, wxALIGN_CENTER_VERTICAL, 0);

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_switching_status_label, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
    main_sizer->Add(btn_sizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
    SetSizer(main_sizer);

    Layout();
}

void ExtruderSwithingStatus::updateBy(MachineObject *obj)
{
    m_obj = obj;
    if (!m_obj)
    {
        Show(false);
    }
    else
    {
        /*do not display while command sended in a mean while*/
        if ((time(nullptr) - m_last_ctrl_time) > HOLD_TIME_6SEC)
        {
            updateBy(obj->GetExtderSystem());
        }
    }
}

void ExtruderSwithingStatus::updateBy(const DevExtderSystem* ext_system)
{
    Show(ext_system->GetTotalExtderCount() > 1);
    if (!IsShown()) { return; }

    auto state = ext_system->GetSwitchState();
    {
        if (state == DevExtderSwitchState::ES_SWITCHING)
        {
            m_switching_status_label->SetLabel(_L("Switching..."));
            m_switching_status_label->SetForegroundColour(StateColor::darkModeColorFor("#262E30"));
            m_switching_status_label->Show(true);
        }
        else if (state == DevExtderSwitchState::ES_SWITCHING_FAILED)
        {
            m_switching_status_label->SetLabel(_L("Switching failed"));
            m_switching_status_label->SetForegroundColour(StateColor::darkModeColorFor(*wxRED));
            m_switching_status_label->Show(true);
        }
        else
        {
            m_switching_status_label->Show(false);
        }
    }

    if (state != DevExtderSwitchState::ES_SWITCHING_FAILED)
    {
        showQuitBtn(false);
        showRetryBtn(false);
        return;
    }

    /*can not quit if it's printing*/
    if (m_obj && !m_obj->is_in_printing() && !m_obj->is_in_printing_pause())
    {
        showQuitBtn(true);
    }

    showRetryBtn(true);
}

void ExtruderSwithingStatus::showQuitBtn(bool show)
{
    if (m_button_quit->IsShown() != show)
    {
        m_button_quit->Show(show);
        Layout();
    }
}

void ExtruderSwithingStatus::showRetryBtn(bool show)
{
    if (m_button_retry->IsShown() != show) {
        m_button_retry->Show(show);
        Layout();
    }
}

bool ExtruderSwithingStatus::has_content_shown() const
{
    if (!IsShown()) { return false; }
    if (!m_switching_status_label->IsShown() && !m_button_quit->IsShown() && !m_button_retry->IsShown()) { return false; }

    return true;
}

void ExtruderSwithingStatus::msw_rescale()
{
    m_button_quit->Rescale(); // ORCA
    m_button_retry->Rescale(); // ORCA
    Layout();
}

void ExtruderSwithingStatus::on_quit(wxCommandEvent &event)
{
    Show(false);

    if (m_obj)
    {
        m_obj->command_ams_control("abort");
        m_last_ctrl_time = time(nullptr);
    }
}

void ExtruderSwithingStatus::on_retry(wxCommandEvent &event)
{
    Show(false);

    if (m_obj)
    {
        m_obj->command_ams_control("resume");
        m_last_ctrl_time = time(nullptr);
    }
}

PrintingTaskPanel::PrintingTaskPanel(wxWindow* parent, PrintingTaskType type)
    : wxPanel(parent, wxID_ANY,wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
{
    m_type = type;
    m_question_button = nullptr;
    create_panel(this);
    SetBackgroundColour(*wxWHITE);
    m_bitmap_background = ScalableBitmap(this, "thumbnail_grid", m_bitmap_thumbnail->GetSize().y);

    m_bitmap_thumbnail->Bind(wxEVT_PAINT, &PrintingTaskPanel::paint, this);
}

PrintingTaskPanel::~PrintingTaskPanel()
{
    if (m_question_button) {
        delete m_question_button;
        m_question_button = nullptr;
    }
}

void PrintingTaskPanel::create_panel(wxWindow* parent)
{
    wxBoxSizer *sizer                 = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *bSizer_printing_title = new wxBoxSizer(wxHORIZONTAL);

    m_panel_printing_title = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, PAGE_TITLE_HEIGHT), wxTAB_TRAVERSAL);
    m_panel_printing_title->SetBackgroundColour(STATUS_TITLE_BG);

    m_staticText_printing = new wxStaticText(m_panel_printing_title, wxID_ANY ,_L("Printing Progress"));
    m_staticText_printing->Wrap(-1);
    //m_staticText_printing->SetFont(PAGE_TITLE_FONT);
    m_staticText_printing->SetForegroundColour(PAGE_TITLE_FONT_COL);

    bSizer_printing_title->Add(m_staticText_printing, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, PAGE_TITLE_LEFT_MARGIN);
    bSizer_printing_title->Add(0, 0, 1, wxEXPAND, 0);

    m_panel_printing_title->SetSizer(bSizer_printing_title);
    m_panel_printing_title->Layout();
    bSizer_printing_title->Fit(m_panel_printing_title);

    m_bitmap_thumbnail = new wxStaticBitmap(parent, wxID_ANY, m_thumbnail_placeholder.bmp(), wxDefaultPosition, TASK_THUMBNAIL_SIZE, 0);
    m_bitmap_thumbnail->SetMaxSize(TASK_THUMBNAIL_SIZE);
    m_bitmap_thumbnail->SetMinSize(TASK_THUMBNAIL_SIZE);

    wxBoxSizer *bSizer_subtask_info = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *bSizer_task_name = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *bSizer_task_name_hor = new wxBoxSizer(wxHORIZONTAL);
    wxPanel*    task_name_panel      = new wxPanel(parent);

    m_staticText_subtask_value = new wxStaticText(task_name_panel, wxID_ANY, _L("N/A"), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT | wxST_ELLIPSIZE_END);
    m_staticText_subtask_value->SetMaxSize(wxSize(FromDIP(600), -1));
    m_staticText_subtask_value->Wrap(-1);
    #ifdef __WXOSX_MAC__
    m_staticText_subtask_value->SetFont(::Label::Body_13);
    #else
    m_staticText_subtask_value->SetFont(wxFont(13, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("HarmonyOS Sans SC")));
    #endif
    m_staticText_subtask_value->SetForegroundColour(wxColour(44, 44, 46));

    m_bitmap_static_use_time = new wxStaticBitmap(task_name_panel, wxID_ANY, m_bitmap_use_time.bmp(), wxDefaultPosition, wxSize(FromDIP(16), FromDIP(16)));

    m_staticText_consumption_of_time = new wxStaticText(task_name_panel, wxID_ANY, "0m", wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_consumption_of_time->SetFont(::Label::Body_12);
    m_staticText_consumption_of_time->SetForegroundColour(wxColour(0x68, 0x68, 0x68));
    m_staticText_consumption_of_time->Wrap(-1);


    m_bitmap_static_use_weight = new wxStaticBitmap(task_name_panel, wxID_ANY, m_bitmap_use_weight.bmp(), wxDefaultPosition, wxSize(FromDIP(16), FromDIP(16)));


    m_staticText_consumption_of_weight = new wxStaticText(task_name_panel, wxID_ANY, "0g", wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_consumption_of_weight->SetFont(::Label::Body_12);
    m_staticText_consumption_of_weight->SetForegroundColour(wxColour(0x68, 0x68, 0x68));
    m_staticText_consumption_of_weight->Wrap(-1);

    bSizer_task_name_hor->Add(m_staticText_subtask_value, 1, wxALL | wxEXPAND, 0);
    bSizer_task_name_hor->Add(m_bitmap_static_use_time, 0, wxALIGN_CENTER_VERTICAL, 0);
    bSizer_task_name_hor->Add(m_staticText_consumption_of_time, 0, wxALIGN_CENTER_VERTICAL|wxLEFT, FromDIP(3));
    bSizer_task_name_hor->Add(0, 0, 0, wxLEFT, FromDIP(10));
    bSizer_task_name_hor->Add(m_bitmap_static_use_weight, 0, wxALIGN_CENTER_VERTICAL, 0);
    bSizer_task_name_hor->Add(m_staticText_consumption_of_weight, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(3));
    bSizer_task_name_hor->Add(0, 0, 0, wxRIGHT, FromDIP(10));


    task_name_panel->SetSizer(bSizer_task_name_hor);
    task_name_panel->Layout();
    task_name_panel->Fit();

    bSizer_task_name->Add(task_name_panel, 0, wxEXPAND, FromDIP(5));


    m_staticText_profile_value = new wxStaticText(parent, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT | wxST_ELLIPSIZE_END);
    m_staticText_profile_value->Wrap(-1);
#ifdef __WXOSX_MAC__
    m_staticText_profile_value->SetFont(::Label::Body_11);
#else
    m_staticText_profile_value->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("HarmonyOS Sans SC")));
#endif

    m_staticText_profile_value->SetForegroundColour(0x6B6B6B);

    auto progress_lr_panel = new wxPanel(parent, wxID_ANY);
    progress_lr_panel->SetBackgroundColour(*wxWHITE);

    m_gauge_progress = new ProgressBar(progress_lr_panel, wxID_ANY, 100, wxDefaultPosition, wxDefaultSize);
    m_gauge_progress->SetValue(0);
    m_gauge_progress->SetHeight(PROGRESSBAR_HEIGHT);

    wxBoxSizer *bSizer_task_btn = new wxBoxSizer(wxHORIZONTAL);

    bSizer_task_btn->Add(FromDIP(10), 0, 0);

    StateColor white_bg(std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Disabled), std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Pressed),
                          std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Hovered), std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Enabled),
                          std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Normal));

    m_button_partskip = new Button(progress_lr_panel, wxEmptyString, "print_control_partskip_disable", 0, 20, wxID_ANY);
    m_button_partskip->Enable(false);
    m_button_partskip->Hide();
    m_button_partskip->SetBackgroundColor(white_bg);
    m_button_partskip->SetIcon("print_control_partskip_disable");
    m_button_partskip->SetBorderColor(*wxWHITE);
    m_button_partskip->SetFont(Label::Body_12);
    m_button_partskip->SetCornerRadius(0);
    m_button_partskip->SetToolTip(_L("Parts Skip"));
    m_button_partskip->Bind(wxEVT_ENTER_WINDOW, [this](auto &e) { m_button_partskip->SetIcon("print_control_partskip_hover"); });
    m_button_partskip->Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) { m_button_partskip->SetIcon("print_control_partskip"); });

    m_button_pause_resume = new ScalableButton(progress_lr_panel, wxID_ANY, "print_control_pause", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER,true);

    m_button_pause_resume->Bind(wxEVT_ENTER_WINDOW, [this](auto &e) {
        if (m_button_pause_resume->GetToolTipText() == _L("Pause")) {
            m_button_pause_resume->SetBitmap_("print_control_pause_hover");
        }

        if (m_button_pause_resume->GetToolTipText() == _L("Resume")) {
            m_button_pause_resume->SetBitmap_("print_control_resume_hover");
        }
    });

    m_button_pause_resume->Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) {
        auto        buf = m_button_pause_resume->GetClientData();
        if (m_button_pause_resume->GetToolTipText() == _L("Pause")) {
            m_button_pause_resume->SetBitmap_("print_control_pause");
        }

        if (m_button_pause_resume->GetToolTipText() == _L("Resume")) {
            m_button_pause_resume->SetBitmap_("print_control_resume");
        }
    });

    m_button_abort = new ScalableButton(progress_lr_panel, wxID_ANY, "print_control_stop", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true);
    m_button_abort->SetToolTip(_L("Stop"));

    m_button_abort->Bind(wxEVT_ENTER_WINDOW, [this](auto &e) {
        m_button_abort->SetBitmap_("print_control_stop_hover");
    });

    m_button_abort->Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) {
        m_button_abort->SetBitmap_("print_control_stop"); }
    );

    wxBoxSizer *bSizer_buttons = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *bSizer_text = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *bSizer_finish_time = new wxBoxSizer(wxHORIZONTAL);
    wxPanel* penel_text = new wxPanel(progress_lr_panel);
    wxPanel* penel_finish_time = new wxPanel(progress_lr_panel);

    penel_text->SetBackgroundColour(*wxWHITE);
    penel_finish_time->SetBackgroundColour(*wxWHITE);

    wxBoxSizer *sizer_percent = new wxBoxSizer(wxVERTICAL);
    sizer_percent->Add(0, 0, 1, wxEXPAND, 0);

    wxBoxSizer *sizer_percent_icon  = new wxBoxSizer(wxVERTICAL);
    sizer_percent_icon->Add(0, 0, 1, wxEXPAND, 0);


    m_staticText_progress_percent = new wxStaticText(penel_text, wxID_ANY, "0", wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_progress_percent->SetFont(::Label::Head_18);
    m_staticText_progress_percent->SetMaxSize(wxSize(-1, FromDIP(20)));
    m_staticText_progress_percent->SetForegroundColour(wxColour(0, 150, 136));

    m_staticText_progress_percent_icon = new wxStaticText(penel_text, wxID_ANY, "%", wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_progress_percent_icon->SetFont(::Label::Body_11);
    m_staticText_progress_percent_icon->SetMaxSize(wxSize(-1, FromDIP(13)));
    m_staticText_progress_percent_icon->SetForegroundColour(wxColour(0, 150, 136));

    sizer_percent->Add(m_staticText_progress_percent, 0, 0, 0);

    #ifdef __WXOSX_MAC__
    sizer_percent_icon->Add(m_staticText_progress_percent_icon, 0, wxBOTTOM, FromDIP(2));
    #else
    sizer_percent_icon->Add(m_staticText_progress_percent_icon, 0, 0, 0);
    #endif


    m_staticText_progress_left = new wxStaticText(penel_text, wxID_ANY, L("N/A"), wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_progress_left->Wrap(-1);
    m_staticText_progress_left->SetFont(wxFont(12, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("HarmonyOS Sans SC")));
    m_staticText_progress_left->SetForegroundColour(wxColour(146, 146, 146));

    m_staticText_layers = new wxStaticText(penel_text, wxID_ANY, _L("Layer: N/A"));
    m_staticText_layers->SetFont(wxFont(12, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("HarmonyOS Sans SC")));
    m_staticText_layers->SetForegroundColour(wxColour(146, 146, 146));
    m_staticText_layers->Hide();

    bSizer_text->Add(sizer_percent, 0, wxEXPAND, 0);
    bSizer_text->Add(sizer_percent_icon, 0, wxEXPAND, 0);
    bSizer_text->Add(0, 0, 1, wxEXPAND, 0);
    bSizer_text->Add(m_staticText_layers, 0, wxALIGN_CENTER_VERTICAL | wxALL, 0);
    bSizer_text->Add(0, 0, 0, wxLEFT, FromDIP(20));
    bSizer_text->Add(m_staticText_progress_left, 0, wxALIGN_CENTER_VERTICAL | wxALL, 0);

    m_printing_stage_panel = new wxPanel(penel_finish_time);
    wxBoxSizer *printingstage_vertical_sizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *printingstage_horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_printing_stage_underline = new wxPanel(m_printing_stage_panel);
    m_printing_stage_underline->SetMaxSize(wxSize(-1, FromDIP(1)));
    m_printing_stage_underline->SetMinSize(wxSize(-1, FromDIP(1)));
    m_printing_stage_underline->SetBackgroundColour(wxColour(146, 146, 146));
    m_printing_stage_underline->Hide();

    m_printing_stage_value = new wxStaticText(m_printing_stage_panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT | wxST_ELLIPSIZE_END);
    m_printing_stage_value->Wrap(-1);
    m_printing_stage_value->SetMaxSize(wxSize(FromDIP(800), -1));
#ifdef __WXOSX_MAC__
    m_printing_stage_value->SetFont(::Label::Body_11);
#else
    m_printing_stage_value->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("HarmonyOS Sans SC")));
#endif
    m_printing_stage_value->SetForegroundColour(STAGE_TEXT_COL);

    m_printing_stage_value->Bind(wxEVT_LEFT_UP, &PrintingTaskPanel::on_stage_clicked, this);

    m_printing_stage_value->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent &event) {
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj         = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj && obj->stage_curr == 58) {
            m_printing_stage_value->SetCursor(wxCursor(wxCURSOR_HAND));
            m_printing_stage_underline->Show();
        } else {
            m_printing_stage_value->SetCursor(wxCursor(wxCURSOR_ARROW));
            m_printing_stage_underline->Hide();
        }
        m_printing_stage_panel->Layout();
        Layout();
        event.Skip();
    });
    m_printing_stage_value->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &event) {
        auto *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj && obj->stage_curr == 58) {
            m_printing_stage_value->SetCursor(wxCURSOR_ARROW);
            m_printing_stage_underline->Hide();
        }
        m_printing_stage_panel->Layout();
        Layout();
        event.Skip();
    });

    // penel_text->SetMaxSize(wxSize(FromDIP(600), -1));
    penel_text->SetSizer(bSizer_text);
    penel_text->Layout();


    // Create question button
    m_question_button = new ScalableButton(m_printing_stage_panel, wxID_ANY, "thermal_question", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true);
    m_question_button->SetToolTip(_L("Click to view thermal preconditioning explanation"));
    m_question_button->SetBackgroundColour(wxColour(255, 255, 255));
    m_question_button->Hide(); // Hide by default
    m_question_button->Bind(wxEVT_LEFT_UP, &PrintingTaskPanel::on_stage_clicked, this);
    m_question_button->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent &event) {
        auto          *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj         = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj && obj->stage_curr == 58) {
            m_question_button->SetCursor(wxCursor(wxCURSOR_HAND));
            m_printing_stage_underline->Show();
        }
        event.Skip();
    });
    m_question_button->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &event) {
        auto          *dev_manager = wxGetApp().getDeviceManager();
        MachineObject *obj         = dev_manager ? dev_manager->get_selected_machine() : nullptr;
        if (obj && obj->stage_curr == 58) {
            m_question_button->SetCursor(wxCURSOR_ARROW);
            m_printing_stage_underline->Hide();
            event.Skip();
        }
    });

    printingstage_horizontal_sizer->Add(m_printing_stage_value, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, 0);
    printingstage_horizontal_sizer->Add(m_question_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));
    printingstage_vertical_sizer->Add(printingstage_horizontal_sizer, 0, wxALIGN_CENTER_VERTICAL, 0);
    printingstage_vertical_sizer->Add(m_printing_stage_underline, 0, wxEXPAND |wxALIGN_TOP, 0);
    m_printing_stage_panel->SetSizer(printingstage_vertical_sizer);

    // Orca: display the end time of the print
    m_staticText_progress_end = new wxStaticText(penel_finish_time, wxID_ANY, L("N/A"), wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_progress_end->Wrap(-1);
    m_staticText_progress_end->SetFont(
        wxFont(12, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("HarmonyOS Sans SC")));
    m_staticText_progress_end->SetForegroundColour(wxColour(146, 146, 146));
    bSizer_finish_time->Add(m_printing_stage_panel, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, 0);
    bSizer_finish_time->Add(0, 0, 1, wxEXPAND, 0);
    bSizer_finish_time->Add(m_staticText_progress_end, 0, wxLEFT | wxEXPAND, 0);
    // penel_finish_time->SetMaxSize(wxSize(FromDIP(600), -1));
    penel_finish_time->SetSizer(bSizer_finish_time);
    penel_finish_time->Layout();

    auto progress_lr_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto progress_left_sizer = new wxBoxSizer(wxVERTICAL);
    auto progress_right_sizer = new wxBoxSizer(wxHORIZONTAL);

    progress_left_sizer->Add(penel_text, 0, wxEXPAND | wxALL, 0);
    progress_left_sizer->Add(m_gauge_progress, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(10));


    progress_left_sizer->Add(penel_finish_time, 0, wxEXPAND |wxALL, 0);
    // progress_left_sizer->SetMaxSize(wxSize(FromDIP(600), -1));

    progress_right_sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, FromDIP(18));
    progress_right_sizer->Add(m_button_partskip, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(0));//5
    progress_right_sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, FromDIP(18));
    progress_right_sizer->Add(m_button_pause_resume, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(0));
    progress_right_sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, FromDIP(18));
    progress_right_sizer->Add(m_button_abort, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(0));
    progress_right_sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, FromDIP(18));

    progress_lr_sizer->Add(progress_left_sizer, 1,   wxEXPAND | wxALL, 0);
    progress_lr_sizer->Add(progress_right_sizer, 0,  wxEXPAND | wxALL , 0);

    progress_lr_panel->SetSizer(progress_lr_sizer);
    progress_lr_panel->SetMaxSize(wxSize(FromDIP(720), -1));

    progress_lr_panel->Layout();
    progress_lr_panel->Fit();

    bSizer_subtask_info->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(14));
    bSizer_subtask_info->Add(bSizer_task_name, 0, wxEXPAND|wxRIGHT, FromDIP(18));
    bSizer_subtask_info->Add(m_staticText_profile_value, 0, wxEXPAND | wxTOP, FromDIP(5));
    bSizer_subtask_info->Add(progress_lr_panel, 0, wxEXPAND | wxTOP, FromDIP(5));

    m_printing_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_printing_sizer->SetMinSize(wxSize(PAGE_MIN_WIDTH, -1));
    m_printing_sizer->Add(m_bitmap_thumbnail, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxLEFT, FromDIP(12));
    m_printing_sizer->Add(FromDIP(8), 0, 0, wxEXPAND, 0);
    m_printing_sizer->Add(bSizer_subtask_info, 1, wxALL | wxEXPAND, 0);

    m_staticline = new wxPanel( parent, wxID_ANY);
    m_staticline->SetBackgroundColour(wxColour(238,238,238));
    m_staticline->Layout();
    m_staticline->Hide();

    m_panel_error_txt = new wxPanel(parent, wxID_ANY);
    m_panel_error_txt->SetBackgroundColour(*wxWHITE);

    wxBoxSizer *static_text_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_error_text = new Label(m_panel_error_txt, "", LB_AUTO_WRAP);
    m_error_text->SetForegroundColour(wxColour(255, 0, 0));
    static_text_sizer->Add(m_error_text, 1, wxEXPAND | wxLEFT, FromDIP(17));

    m_button_clean = new Button(m_panel_error_txt, _L("Clear"));
    m_button_clean->SetStyle(ButtonStyle::Regular, ButtonType::Window);

    static_text_sizer->Add( FromDIP(10), 0, 0, 0, 0 );
    static_text_sizer->Add(m_button_clean, 0, wxALIGN_CENTRE_VERTICAL|wxRIGHT, FromDIP(5));

    m_panel_error_txt->SetSizer(static_text_sizer);
    m_panel_error_txt->Hide();

    sizer->Add(m_panel_printing_title, 0, wxEXPAND | wxALL, 0);
    sizer->Add(0, FromDIP(12), 0);
    sizer->Add(m_printing_sizer, 0, wxEXPAND | wxALL, 0);
    sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    sizer->Add(m_staticline, 0, wxEXPAND | wxALL, FromDIP(10));
    sizer->Add(m_panel_error_txt, 0, wxEXPAND | wxALL, 0);
    sizer->Add(0, FromDIP(12), 0);

    m_score_staticline = new wxPanel(parent, wxID_ANY);
    m_score_staticline->SetBackgroundColour(wxColour(238, 238, 238));
    m_score_staticline->Layout();
    m_score_staticline->Hide();
    sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    sizer->Add(m_score_staticline, 0, wxEXPAND | wxALL, FromDIP(10));
    m_request_failed_panel    = new wxPanel(parent, wxID_ANY);
    m_request_failed_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer *static_request_failed_panel_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_request_failed_info = new wxStaticText(m_request_failed_panel, wxID_ANY, _L("You have completed printing the mall model, \nbut the synchronization of rating information has failed."), wxDefaultPosition, wxDefaultSize, 0);
    m_request_failed_info->Wrap(-1);
    m_request_failed_info->SetForegroundColour(*wxRED);
    m_request_failed_info->SetFont(::Label::Body_10);
    static_request_failed_panel_sizer->Add(m_request_failed_info, 0, wxEXPAND | wxALL, FromDIP(10));

    m_button_market_retry = new Button(m_request_failed_panel, _L("Retry"));
    m_button_market_retry->SetStyle(ButtonStyle::Confirm, ButtonType::Window);

    static_request_failed_panel_sizer->Add(0, 0, 1, wxEXPAND, 0);
    static_request_failed_panel_sizer->Add(m_button_market_retry, 0, wxEXPAND | wxALL, FromDIP(10));
    m_request_failed_panel->SetSizer(static_request_failed_panel_sizer);
    m_request_failed_panel->Hide();
    sizer->Add(m_request_failed_panel, 0, wxEXPAND | wxALL, FromDIP(10));


    m_score_subtask_info = new wxPanel(parent, wxID_ANY);
    m_score_subtask_info->SetBackgroundColour(*wxWHITE);

    wxBoxSizer *  static_score_sizer = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_score_text  = new wxStaticText(m_score_subtask_info, wxID_ANY, _L("How do you like this printing file?"), wxDefaultPosition, wxDefaultSize, 0);
    static_score_text->Wrap(-1);
    static_score_sizer->Add(static_score_text, 1, wxEXPAND | wxALL, FromDIP(10));
    m_has_rated_prompt = new wxStaticText(m_score_subtask_info, wxID_ANY, _L("(The model has already been rated. Your rating will overwrite the previous rating.)"), wxDefaultPosition, wxDefaultSize, 0);
    m_has_rated_prompt->Wrap(-1);
    m_has_rated_prompt->SetForegroundColour(*wxBLACK);
    m_has_rated_prompt->SetFont(::Label::Body_10);
    m_has_rated_prompt->Hide();

    m_star_count                        = 0;
    wxBoxSizer *static_score_star_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_score_star.resize(5);
    for (int i = 0; i < m_score_star.size(); ++i) {
        m_score_star[i] = new ScalableButton(m_score_subtask_info, wxID_ANY, "score_star_dark", wxEmptyString, wxSize(FromDIP(26), FromDIP(26)), wxDefaultPosition,
                                             wxBU_EXACTFIT | wxNO_BORDER, true, 26);
        m_score_star[i]->SetMinSize(wxSize(FromDIP(26), FromDIP(26)));
        m_score_star[i]->SetMaxSize(wxSize(FromDIP(26), FromDIP(26)));
        m_score_star[i]->Bind(wxEVT_LEFT_DOWN, [this, i](auto &e) {
            for (int j = 0; j < m_score_star.size(); ++j) {
                ScalableBitmap light_star = ScalableBitmap(nullptr, "score_star_light", 26);
                m_score_star[j]->SetBitmap(light_star.bmp());
                if (m_score_star[j] == m_score_star[i]) {
                    m_star_count = j + 1;
                    break;
                }
            }
            for (int k = m_star_count; k < m_score_star.size(); ++k) {
                ScalableBitmap dark_star = ScalableBitmap(nullptr, "score_star_dark", 26);
                m_score_star[k]->SetBitmap(dark_star.bmp());
            }
            m_star_count_dirty = true;
            m_button_market_scoring->Enable(true);
        });
        static_score_star_sizer->Add(m_score_star[i], 1, wxEXPAND | wxLEFT, FromDIP(5));
    }

    m_button_market_scoring = new Button(m_score_subtask_info, _L("Rate"));
    m_button_market_scoring->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    m_button_market_scoring->Enable(false);

    static_score_star_sizer->Add(0, 0, 1, wxEXPAND, 0);
    static_score_star_sizer->Add(m_button_market_scoring, 0, wxEXPAND | wxRIGHT, FromDIP(10));
    static_score_sizer->Add(static_score_star_sizer, 0, wxEXPAND, FromDIP(10));
    static_score_sizer->Add(m_has_rated_prompt, 1, wxEXPAND | wxALL, FromDIP(10));

    m_score_subtask_info->SetSizer(static_score_sizer);
    m_score_subtask_info->Layout();
    m_score_subtask_info->Hide();

    sizer->Add(m_score_subtask_info, 0, wxEXPAND | wxALL, 0);
    sizer->Add(0, FromDIP(12), 0);

    if (m_type == CALIBRATION) {
        m_panel_printing_title->Hide();
        m_bitmap_thumbnail->Hide();
        task_name_panel->Hide();
        m_staticText_profile_value->Hide();
    }

    parent->SetSizer(sizer);
    parent->Layout();
    parent->Fit();
}

void PrintingTaskPanel::paint(wxPaintEvent&)
{
    wxPaintDC dc(m_bitmap_thumbnail);
    if (wxGetApp().dark_mode()) {
        if (m_brightness_value > 0 && m_brightness_value < SHOW_BACKGROUND_BITMAP_PIXEL_THRESHOLD) {
            dc.DrawBitmap(m_bitmap_background.bmp(), 0, 0);
            dc.SetTextForeground(*wxBLACK);
        }
        else
            dc.SetTextForeground(*wxWHITE);
    }
    else
        dc.SetTextForeground(*wxBLACK);
    if (m_thumbnail_bmp_display.IsOk()) {
        dc.DrawBitmap(m_thumbnail_bmp_display, wxPoint(0, 0));
    }
    dc.SetFont(Label::Body_12);
    
    if (m_plate_index >= 0) {
        wxString plate_id_str = wxString::Format("%d", m_plate_index);
        dc.DrawText(plate_id_str, wxPoint(4, 4));
    }
}

void PrintingTaskPanel::set_has_reted_text(bool has_rated)
{
    if (has_rated) {
        m_has_rated_prompt->Show();
    } else {
        m_has_rated_prompt->Hide();
    }
    Layout();
    Fit();
}

void PrintingTaskPanel::msw_rescale()
{
    m_panel_printing_title->SetSize(wxSize(-1, FromDIP(PAGE_TITLE_HEIGHT)));
    m_printing_sizer->SetMinSize(wxSize(PAGE_MIN_WIDTH, -1));
    //m_staticText_printing->SetMinSize(wxSize(PAGE_TITLE_TEXT_WIDTH, PAGE_TITLE_HEIGHT));
    m_gauge_progress->SetHeight(PROGRESSBAR_HEIGHT);
    m_gauge_progress->Rescale();
    m_button_pause_resume->msw_rescale();
    m_button_abort->msw_rescale();
    m_bitmap_thumbnail->SetSize(TASK_THUMBNAIL_SIZE);
}

void PrintingTaskPanel::init_bitmaps()
{
    m_thumbnail_placeholder     = ScalableBitmap(this, "monitor_placeholder", 120);
    m_bitmap_use_time           = ScalableBitmap(this, "print_info_time", 16);
    m_bitmap_use_weight         = ScalableBitmap(this, "print_info_weight", 16);
}

void PrintingTaskPanel::init_scaled_buttons()
{
    m_button_clean->SetMinSize(wxSize(FromDIP(48), FromDIP(24)));
    m_button_clean->SetCornerRadius(FromDIP(12));
}

void PrintingTaskPanel::error_info_reset()
{
    if (m_panel_error_txt->IsShown()) {
        m_staticline->Hide();
        m_panel_error_txt->Hide();
        m_panel_error_txt->GetParent()->Layout();
        m_error_text->SetLabel(wxEmptyString);
    }
}

void PrintingTaskPanel::show_error_msg(wxString msg)
{
    m_staticline->Show();
    m_panel_error_txt->Show();
    m_error_text->SetLabel(msg);
}

void PrintingTaskPanel::reset_printing_value()
{
    this->set_thumbnail_img(m_thumbnail_placeholder.bmp(), m_thumbnail_placeholder.name());
    this->set_plate_index(-1);
}

void PrintingTaskPanel::enable_partskip_button(MachineObject* obj, bool enable)
{
    int stage = 0;
    bool in_calibration_mode = false;
    if( obj && (obj->print_type == "system" || CalibUtils::get_calib_mode_by_name(obj->subtask_name, stage) != CalibMode::Calib_None)){
        in_calibration_mode = true;
    }

    if (!enable || in_calibration_mode) {
        m_button_partskip->Enable(false);
        m_button_partskip->SetLabel("");
        m_button_partskip->SetIcon("print_control_partskip_disable");
    }else if(obj && obj->is_support_brtc){
        m_button_partskip->Enable(true);
        m_button_partskip->SetIcon("print_control_partskip");   
    }
}

void PrintingTaskPanel::enable_pause_resume_button(bool enable, std::string type)
{
    if (!enable) {
        m_button_pause_resume->Enable(false);

        if (type == "pause_disable") {
            m_button_pause_resume->SetBitmap_("print_control_pause_disable");
        }
        else if (type == "resume_disable") {
            m_button_pause_resume->SetBitmap_("print_control_resume_disable");
        }
    }
    else {
        m_button_pause_resume->Enable(true);
        if (type == "resume") {
        m_button_pause_resume->SetBitmap_("print_control_resume");
        if (m_button_pause_resume->GetToolTipText() != _L("Resume")) { m_button_pause_resume->SetToolTip(_L("Resume")); }
        }
        else if (type == "pause") {
        m_button_pause_resume->SetBitmap_("print_control_pause");
        if (m_button_pause_resume->GetToolTipText() != _L("Pause")) { m_button_pause_resume->SetToolTip(_L("Pause")); }
        }
    }
}

void PrintingTaskPanel::enable_abort_button(bool enable)
{
    if (!enable) {
        m_button_abort->Enable(false);
        m_button_abort->SetBitmap_("print_control_stop_disable");
    }
    else {
        m_button_abort->Enable(true);
        m_button_abort->SetBitmap_("print_control_stop");
    }
}

void PrintingTaskPanel::update_subtask_name(wxString name)
{
    if (m_staticText_subtask_value->GetLabelText() != name)
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": " << name;
    }
    m_staticText_subtask_value->SetLabelText(name);
}

void PrintingTaskPanel::update_stage_value(wxString stage, int val)
{
    m_printing_stage_value->SetLabelText(stage);
    m_gauge_progress->SetValue(val);
}

void PrintingTaskPanel::update_stage_value_with_machine(wxString stage, int val, MachineObject *obj)
{
    m_gauge_progress->SetValue(val);
    m_printing_stage_value->SetLabelText(stage);

    if (obj && obj->stage_curr == 58) {
        m_question_button->Show(); // Show question button
    } else {
        m_question_button->Hide(); // Hide question button
        m_printing_stage_underline->Hide();
    }
    m_printing_stage_panel->Layout();
    Layout();
}

void PrintingTaskPanel::on_stage_clicked(wxMouseEvent &event)
{
    auto *dev_manager = wxGetApp().getDeviceManager();
    MachineObject *obj = dev_manager ? dev_manager->get_selected_machine() : nullptr;

    if (obj && obj->stage_curr == 58) {
            wxWindow *top    = wxGetTopLevelParent(this);
            ThermalPreconditioningDialog m_thermal_dialog(top ? top : this, obj->get_dev_id() , "Calculating...");
            m_thermal_dialog.ShowModal();
    }

    event.Skip();
}

void PrintingTaskPanel::update_progress_percent(wxString percent, wxString icon)
{
    m_staticText_progress_percent->SetLabelText(percent);
    m_staticText_progress_percent_icon->SetLabelText(icon);
}

void PrintingTaskPanel::update_left_time(wxString time)
{
    m_staticText_progress_left->SetLabelText(time);
}

void PrintingTaskPanel::update_left_time(int mc_left_time)
{
    // update gcode progress
    std::string left_time;
    wxString    left_time_text = NA_STR;

    try {
        left_time = get_bbl_monitor_time_dhm(mc_left_time);
    }
    catch (...) {
        ;
    }

    if (!left_time.empty()) left_time_text = wxString::Format("-%s", left_time);
    update_left_time(left_time_text);

    //Update end time
    std::string end_time;
    wxString    end_time_text = NA_STR;
    try {
        end_time = get_bbl_monitor_end_time_dhm(mc_left_time);
    } catch (...) {
        ;
    }
    if (!end_time.empty())
        end_time_text = wxString::Format("%s", end_time);
    else
        end_time_text = NA_STR;

    m_staticText_progress_end->SetLabelText(end_time_text);

}

void PrintingTaskPanel::update_layers_num(bool show, wxString num)
{
    if ((show == m_staticText_layers->IsShown()) && (num == m_staticText_layers->GetLabelText()))
    {
        return;
    }

    if (show) {
        m_staticText_layers->Show(true);
        m_staticText_layers->SetLabelText(num);
    }
    else {
        m_staticText_layers->Show(false);
        m_staticText_layers->SetLabelText(num);
    }
}

void PrintingTaskPanel::show_priting_use_info(bool show, wxString time /*= wxEmptyString*/, wxString weight /*= wxEmptyString*/)
{
    if (show) {
        if (!m_staticText_consumption_of_time->IsShown()) {
            m_bitmap_static_use_time->Show();
            m_staticText_consumption_of_time->Show();
        }

        if (!m_staticText_consumption_of_weight->IsShown()) {
            m_bitmap_static_use_weight->Show();
            m_staticText_consumption_of_weight->Show();
        }

        m_staticText_consumption_of_time->SetLabelText(time);
        m_staticText_consumption_of_weight->SetLabelText(weight);
    }
    else {
        m_staticText_consumption_of_time->SetLabelText("0m");
        m_staticText_consumption_of_weight->SetLabelText("0g");
        if (m_staticText_consumption_of_time->IsShown()) {
            m_bitmap_static_use_time->Hide();
            m_staticText_consumption_of_time->Hide();
        }

        if (m_staticText_consumption_of_weight->IsShown()) {
            m_bitmap_static_use_weight->Hide();
            m_staticText_consumption_of_weight->Hide();
        }    }
}


void PrintingTaskPanel::show_profile_info(bool show, wxString profile /*= wxEmptyString*/)
{
    if (show) {
        if (!m_staticText_profile_value->IsShown()) { m_staticText_profile_value->Show(); }
        m_staticText_profile_value->SetLabelText(profile);
    }
    else {
        m_staticText_profile_value->SetLabelText(wxEmptyString);
        m_staticText_profile_value->Hide();
    }
}

// the API will buff the bmp and bmp_name
// when bmp_name is empty, the API will replace the image on force
void PrintingTaskPanel::set_thumbnail_img(const wxBitmap& bmp, const std::string& bmp_name)
{
    if (!bmp_name.empty() && m_thumbnail_bmp_display_name == bmp_name)  {
        return;
    }

    m_thumbnail_bmp_display_name = bmp_name;
    m_thumbnail_bmp_display = bmp;
    Refresh();
}

void PrintingTaskPanel::set_plate_index(int plate_idx)
{
    m_plate_index = plate_idx;
}

void PrintingTaskPanel::market_scoring_show()
{ 
    m_score_staticline->Show();
    m_score_subtask_info->Show();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " show market scoring page";
}

void PrintingTaskPanel::market_scoring_hide()
{
    m_score_staticline->Hide();
    m_score_subtask_info->Hide();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " hide market scoring page";
}

void PrintingTaskPanel::set_star_count(int star_count)
{
    m_star_count = star_count;

    for (int i = 0; i < m_score_star.size(); ++i) {
        if (i < star_count) {
            ScalableBitmap light_star = ScalableBitmap(nullptr, "score_star_light", 26);
            m_score_star[i]->SetBitmap(light_star.bmp());
        } else {
            ScalableBitmap dark_star = ScalableBitmap(nullptr, "score_star_dark", 26);
            m_score_star[i]->SetBitmap(dark_star.bmp());
        }
    }
}

StatusBasePanel::StatusBasePanel(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size, long style, const wxString &name)
    : wxScrolledWindow(parent, id, pos, size, wxHSCROLL | wxVSCROLL)
{
    this->SetScrollRate(25, 25);
    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;
    obj = dev->get_selected_machine();

    init_bitmaps();

    this->SetBackgroundColour(wxColour(0xEE, 0xEE, 0xEE));

    wxBoxSizer *bSizer_status = new wxBoxSizer(wxVERTICAL);

    auto m_panel_separotor_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, PAGE_SPACING), wxTAB_TRAVERSAL);
    m_panel_separotor_top->SetBackgroundColour(STATUS_PANEL_BG);

    bSizer_status->Add(m_panel_separotor_top, 0, wxEXPAND | wxALL, 0);

    wxBoxSizer *bSizer_status_below = new wxBoxSizer(wxHORIZONTAL);

    auto m_panel_separotor_left = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_panel_separotor_left->SetBackgroundColour(STATUS_PANEL_BG);
    m_panel_separotor_left->SetMinSize(wxSize(PAGE_SPACING, -1));

    bSizer_status_below->Add(m_panel_separotor_left, 0, wxEXPAND | wxALL, 0);

    wxBoxSizer *bSizer_left = new wxBoxSizer(wxVERTICAL);

    auto m_monitoring_sizer = create_monitoring_page();
    bSizer_left->Add(m_monitoring_sizer, 1, wxEXPAND | wxALL, 0);

    auto m_panel_separotor1 = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_panel_separotor1->SetBackgroundColour(STATUS_PANEL_BG);
    m_panel_separotor1->SetMinSize(wxSize(-1, PAGE_SPACING));
    m_panel_separotor1->SetMaxSize(wxSize(-1, PAGE_SPACING));
    m_monitoring_sizer->Add(m_panel_separotor1, 0, wxEXPAND, 0);

    m_project_task_panel = new PrintingTaskPanel(this, PrintingTaskType::PRINGINT);
    m_project_task_panel->init_bitmaps();
    m_monitoring_sizer->Add(m_project_task_panel, 0, wxALL | wxEXPAND , 0);

//    auto m_panel_separotor2 = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
//    m_panel_separotor2->SetBackgroundColour(STATUS_PANEL_BG);
//    m_panel_separotor2->SetMinSize(wxSize(-1, PAGE_SPACING));
//    bSizer_left->Add(m_panel_separotor2, 1, wxEXPAND, 0);

    bSizer_status_below->Add(bSizer_left, 1, wxALL | wxEXPAND, 0);

    auto m_panel_separator_middle = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL);
    m_panel_separator_middle->SetBackgroundColour(STATUS_PANEL_BG);
    m_panel_separator_middle->SetMinSize(wxSize(PAGE_SPACING, -1));

    bSizer_status_below->Add(m_panel_separator_middle, 0, wxEXPAND | wxALL, 0);

    m_machine_ctrl_panel = new wxPanel(this);
    m_machine_ctrl_panel->SetBackgroundColour(*wxWHITE);
    m_machine_ctrl_panel->SetDoubleBuffered(true);
    auto m_machine_control = create_machine_control_page(m_machine_ctrl_panel);
    m_machine_ctrl_panel->SetSizer(m_machine_control);
    m_machine_ctrl_panel->Layout();
    m_machine_control->Fit(m_machine_ctrl_panel);

    bSizer_status_below->Add(m_machine_ctrl_panel, 0, wxALL, 0);

    m_panel_separator_right = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(PAGE_SPACING, -1), wxTAB_TRAVERSAL);
    m_panel_separator_right->SetBackgroundColour(STATUS_PANEL_BG);

    bSizer_status_below->Add(m_panel_separator_right, 0, wxEXPAND | wxALL, 0);

    bSizer_status->Add(bSizer_status_below, 1, wxALL | wxEXPAND, 0);

    m_panel_separotor_bottom = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, PAGE_SPACING), wxTAB_TRAVERSAL);
    m_panel_separotor_bottom->SetBackgroundColour(STATUS_PANEL_BG);

    bSizer_status->Add(m_panel_separotor_bottom, 0, wxEXPAND | wxALL, 0);
    this->SetSizerAndFit(bSizer_status);
    this->Layout();
}

StatusBasePanel::~StatusBasePanel()
{
    delete m_media_play_ctrl;

    if (m_custom_camera_view) {
        delete m_custom_camera_view;
        m_custom_camera_view = nullptr;
    }
}

void StatusBasePanel::init_bitmaps()
{
    static Slic3r::GUI::BitmapCache cache;
    m_bitmap_item_prediction = create_scaled_bitmap("monitor_item_prediction", nullptr, 16);
    m_bitmap_item_cost       = create_scaled_bitmap("monitor_item_cost", nullptr, 16);
    m_bitmap_item_print      = create_scaled_bitmap("monitor_item_print", nullptr, 18);
    m_bitmap_axis_home       = ScalableBitmap(this, "monitor_axis_home", 32);
    m_bitmap_lamp_on         = ScalableBitmap(this, "monitor_lamp_on", 24);
    m_bitmap_lamp_off        = ScalableBitmap(this, "monitor_lamp_off", 24);
    m_bitmap_fan_on          = ScalableBitmap(this, "monitor_fan_on", 22);
    m_bitmap_fan_off         = ScalableBitmap(this, "monitor_fan_off", 22);
    m_bitmap_speed           = ScalableBitmap(this, "monitor_speed", 24);
    m_bitmap_speed_active    = ScalableBitmap(this, "monitor_speed_active", 24);
    
    m_thumbnail_brokenimg    = ScalableBitmap(this, "monitor_brokenimg", 120);
    m_thumbnail_sdcard       = ScalableBitmap(this, "monitor_sdcard_thumbnail", 120);
    //m_bitmap_camera          = create_scaled_bitmap("monitor_camera", nullptr, 18);
    m_bitmap_extruder_empty_load      = *cache.load_png("monitor_extruder_empty_load", FromDIP(28), FromDIP(70), false, false);
    m_bitmap_extruder_filled_load     = *cache.load_png("monitor_extruder_filled_load", FromDIP(28), FromDIP(70), false, false);
    m_bitmap_extruder_empty_unload    = *cache.load_png("monitor_extruder_empty_unload", FromDIP(28), FromDIP(70), false, false);
    m_bitmap_extruder_filled_unload   = *cache.load_png("monitor_extruder_filled_unload", FromDIP(28), FromDIP(70), false, false);

    m_bitmap_sdcard_state_abnormal = ScalableBitmap(this, wxGetApp().dark_mode() ? "sdcard_state_abnormal_dark" : "sdcard_state_abnormal", 20);
    m_bitmap_sdcard_state_normal = ScalableBitmap(this, wxGetApp().dark_mode() ? "sdcard_state_normal_dark" : "sdcard_state_normal", 20);
    m_bitmap_sdcard_state_no = ScalableBitmap(this, wxGetApp().dark_mode() ? "sdcard_state_no_dark" : "sdcard_state_no", 20);
    m_bitmap_recording_on = ScalableBitmap(this, wxGetApp().dark_mode() ? "monitor_recording_on_dark" : "monitor_recording_on", 20);
    m_bitmap_recording_off = ScalableBitmap(this, wxGetApp().dark_mode() ? "monitor_recording_off_dark" : "monitor_recording_off", 20);
    m_bitmap_timelapse_on = ScalableBitmap(this, wxGetApp().dark_mode() ? "monitor_timelapse_on_dark" : "monitor_timelapse_on", 20);
    m_bitmap_timelapse_off = ScalableBitmap(this, wxGetApp().dark_mode() ? "monitor_timelapse_off_dark" : "monitor_timelapse_off", 20);
    m_bitmap_vcamera_on = ScalableBitmap(this, wxGetApp().dark_mode() ? "monitor_vcamera_on_dark" : "monitor_vcamera_on", 20);
    m_bitmap_vcamera_off = ScalableBitmap(this, wxGetApp().dark_mode() ? "monitor_vcamera_off_dark" : "monitor_vcamera_off", 20);
    m_bitmap_switch_camera = ScalableBitmap(this, wxGetApp().dark_mode() ? "camera_switch_dark" : "camera_switch", 20);

}

wxBoxSizer *StatusBasePanel::create_monitoring_page()
{
    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

    m_panel_monitoring_title = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, PAGE_TITLE_HEIGHT), wxTAB_TRAVERSAL);
    m_panel_monitoring_title->SetBackgroundColour(STATUS_TITLE_BG);

    wxBoxSizer *bSizer_monitoring_title;
    bSizer_monitoring_title = new wxBoxSizer(wxHORIZONTAL);

    m_staticText_monitoring = new Label(m_panel_monitoring_title, _L("Camera"));
    m_staticText_monitoring->Wrap(-1);
    //m_staticText_monitoring->SetFont(PAGE_TITLE_FONT);
    m_staticText_monitoring->SetForegroundColour(PAGE_TITLE_FONT_COL);
    bSizer_monitoring_title->Add(m_staticText_monitoring, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, PAGE_TITLE_LEFT_MARGIN);


    bSizer_monitoring_title->Add(FromDIP(13), 0, 0, 0);
    bSizer_monitoring_title->AddStretchSpacer();

    m_staticText_timelapse = new wxStaticText(m_panel_monitoring_title, wxID_ANY, _L("Timelapse"), wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_timelapse->Wrap(-1);
    m_staticText_timelapse->Hide();
    bSizer_monitoring_title->Add(m_staticText_timelapse, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    m_mqtt_source = new wxStaticText(m_panel_monitoring_title, wxID_ANY, "MqttSource", wxDefaultPosition, wxDefaultSize, 0);
    m_mqtt_source->Wrap(-1);
    m_mqtt_source->Hide();
    bSizer_monitoring_title->Add(m_mqtt_source, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    m_bmToggleBtn_timelapse = new SwitchButton(m_panel_monitoring_title);
    m_bmToggleBtn_timelapse->SetMinSize(SWITCH_BUTTON_SIZE);
    m_bmToggleBtn_timelapse->Hide();
    bSizer_monitoring_title->Add(m_bmToggleBtn_timelapse, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

#if !BBL_RELEASE_TO_PUBLIC
    m_staticText_timelapse->Show();
    m_bmToggleBtn_timelapse->Show();
    m_bmToggleBtn_timelapse->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        if (e.IsChecked())
            wxGetApp().getAgent()->start_subscribe("tunnel");
        else
            wxGetApp().getAgent()->stop_subscribe("tunnel");
    });
#endif

    //m_bitmap_camera_img = new wxStaticBitmap(m_panel_monitoring_title, wxID_ANY, m_bitmap_camera , wxDefaultPosition, wxSize(FromDIP(32), FromDIP(18)), 0);
    //m_bitmap_camera_img->SetMinSize(wxSize(FromDIP(32), FromDIP(18)));
    //bSizer_monitoring_title->Add(m_bitmap_camera_img, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    m_bitmap_sdcard_img = new wxStaticBitmap(m_panel_monitoring_title, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(38), FromDIP(24)), 0);
    m_bitmap_sdcard_img->SetMinSize(wxSize(FromDIP(38), FromDIP(24)));

    m_bitmap_timelapse_img = new wxStaticBitmap(m_panel_monitoring_title, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(38), FromDIP(24)), 0);
    m_bitmap_timelapse_img->SetMinSize(wxSize(FromDIP(38), FromDIP(24)));
    m_bitmap_timelapse_img->Hide();

    m_bitmap_recording_img = new wxStaticBitmap(m_panel_monitoring_title, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(38), FromDIP(24)), 0);
    m_bitmap_recording_img->SetMinSize(wxSize(FromDIP(38), FromDIP(24)));
    m_bitmap_timelapse_img->Hide();

    m_bitmap_vcamera_img = new wxStaticBitmap(m_panel_monitoring_title, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(38), FromDIP(24)), 0);
    m_bitmap_vcamera_img->SetMinSize(wxSize(FromDIP(38), FromDIP(24)));
    m_bitmap_vcamera_img->Hide();

    m_setting_button = new CameraItem(m_panel_monitoring_title, "camera_setting", "camera_setting_hover");
    m_setting_button->SetMinSize(wxSize(FromDIP(38), FromDIP(24)));
    m_setting_button->SetBackgroundColour(STATUS_TITLE_BG);

    m_camera_switch_button = new wxStaticBitmap(m_panel_monitoring_title, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(38), FromDIP(24)), 0);
    m_camera_switch_button->SetMinSize(wxSize(FromDIP(38), FromDIP(24)));
    m_camera_switch_button->SetBackgroundColour(STATUS_TITLE_BG);
    m_camera_switch_button->SetBitmap(m_bitmap_switch_camera.bmp());
    m_camera_switch_button->Bind(wxEVT_LEFT_DOWN, &StatusBasePanel::on_camera_switch_toggled, this);
    m_camera_switch_button->Bind(wxEVT_RIGHT_DOWN, [this](auto& e) {
        const std::string js_request_pip = R"(
            document.querySelector('video').requestPictureInPicture();
        )";
        m_custom_camera_view->RunScript(js_request_pip);
    });
    m_camera_switch_button->Hide();

    m_bitmap_sdcard_img->SetToolTip(_L("Storage"));
    m_bitmap_timelapse_img->SetToolTip(_L("Timelapse"));
    m_bitmap_recording_img->SetToolTip(_L("Video"));
    m_bitmap_vcamera_img->SetToolTip(_L("Go Live"));
    m_setting_button->SetToolTip(_L("Camera Setting"));
    m_camera_switch_button->SetToolTip(_L("Switch Camera View"));

    bSizer_monitoring_title->Add(m_camera_switch_button, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    bSizer_monitoring_title->Add(m_bitmap_sdcard_img, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    bSizer_monitoring_title->Add(m_bitmap_timelapse_img, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    bSizer_monitoring_title->Add(m_bitmap_recording_img, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    bSizer_monitoring_title->Add(m_bitmap_vcamera_img, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    bSizer_monitoring_title->Add(m_setting_button, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    bSizer_monitoring_title->Add(FromDIP(13), 0, 0);

    m_panel_monitoring_title->SetSizer(bSizer_monitoring_title);
    m_panel_monitoring_title->Layout();
    bSizer_monitoring_title->Fit(m_panel_monitoring_title);
    sizer->Add(m_panel_monitoring_title, 0, wxEXPAND | wxALL, 0);

//    media_ctrl_panel              = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize);
//    media_ctrl_panel->SetBackgroundColour(*wxBLACK);
//    wxBoxSizer *bSizer_monitoring = new wxBoxSizer(wxVERTICAL);
    m_media_ctrl = new wxMediaCtrl2(this);
    m_media_ctrl->SetMinSize(wxSize(PAGE_MIN_WIDTH, FromDIP(288)));

    m_custom_camera_view = WebView::CreateWebView(this, wxEmptyString);
    m_custom_camera_view->EnableContextMenu(false);
    Bind(wxEVT_WEBVIEW_NAVIGATING, &StatusBasePanel::on_webview_navigating, this, m_custom_camera_view->GetId());

    m_media_play_ctrl = new MediaPlayCtrl(this, m_media_ctrl, wxDefaultPosition, wxSize(-1, FromDIP(40)));
    m_custom_camera_view->Hide();
    m_custom_camera_view->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, [this](wxWebViewEvent& evt) {
        if (evt.GetString() == "leavepictureinpicture") {
            // When leaving PiP, video gets paused in some cases and toggling play
            // programmatically does not work.
            m_custom_camera_view->Reload();
        }
        else if (evt.GetString() == "enterpictureinpicture") {
            toggle_builtin_camera();
        }
    });

    sizer->Add(m_media_ctrl, 1, wxEXPAND | wxALL, 0);
    sizer->Add(m_custom_camera_view, 1, wxEXPAND | wxALL, 0);
    sizer->Add(m_media_play_ctrl, 0, wxEXPAND | wxALL, 0);
//    media_ctrl_panel->SetSizer(bSizer_monitoring);
//    media_ctrl_panel->Layout();
//
//    sizer->Add(media_ctrl_panel, 1, wxEXPAND | wxALL, 1);

    if (wxGetApp().app_config->get("camera", "enable_custom_source") == "true") {
        handle_camera_source_change();
    }

    return sizer;
}

void StatusBasePanel::on_webview_navigating(wxWebViewEvent& evt) {
    wxGetApp().CallAfter([this] {
        remove_controls();
    });
}

wxBoxSizer *StatusBasePanel::create_machine_control_page(wxWindow *parent)
{
    wxBoxSizer *bSizer_right = new wxBoxSizer(wxVERTICAL);

    m_panel_control_title = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, PAGE_TITLE_HEIGHT), wxTAB_TRAVERSAL);
    m_panel_control_title->SetBackgroundColour(STATUS_TITLE_BG);

    wxBoxSizer *bSizer_control_title = new wxBoxSizer(wxHORIZONTAL);
    m_staticText_control             = new Label(m_panel_control_title,_L("Control"));
    m_staticText_control->Wrap(-1);
    //m_staticText_control->SetFont(PAGE_TITLE_FONT);
    m_staticText_control->SetForegroundColour(PAGE_TITLE_FONT_COL);

    m_parts_btn = new Button(m_panel_control_title, _L("Printer Parts"));
    m_parts_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Window);

    m_options_btn = new Button(m_panel_control_title, _L("Print Options"));
    m_options_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
  
    m_safety_btn = new Button(m_panel_control_title, _L("Safety Options"));
    m_safety_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Window);

    m_calibration_btn = new Button(m_panel_control_title, _L("Calibration"));
    m_calibration_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    m_calibration_btn->EnableTooltipEvenDisabled();
  
    m_options_btn->Hide();
    m_safety_btn->Hide();

    bSizer_control_title->Add(m_staticText_control, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, PAGE_TITLE_LEFT_MARGIN);
    bSizer_control_title->Add(0, 0, 1, wxEXPAND, 0);
    bSizer_control_title->Add(m_parts_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    bSizer_control_title->Add(m_options_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    bSizer_control_title->Add(m_safety_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    bSizer_control_title->Add(m_calibration_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

    m_panel_control_title->SetSizer(bSizer_control_title);
    m_panel_control_title->Layout();
    bSizer_control_title->Fit(m_panel_control_title);
    bSizer_right->Add(m_panel_control_title, 0, wxALL | wxEXPAND, 0);

    wxBoxSizer *bSizer_control = new wxBoxSizer(wxVERTICAL);

    auto temp_axis_ctrl_sizer = create_temp_axis_group(parent);
    auto m_ams_ctrl_sizer = create_ams_group(parent);
    auto m_filament_load_sizer = create_filament_group(parent);

    bSizer_control->Add(0, 0, 0, wxTOP, FromDIP(8));
    bSizer_control->Add(temp_axis_ctrl_sizer,   0, wxALIGN_CENTER|wxLEFT|wxRIGHT, FromDIP(8));
    bSizer_control->Add(0, 0, 0, wxTOP, FromDIP(6));
    bSizer_control->Add(m_ams_ctrl_sizer,       0, wxALIGN_CENTER|wxLEFT|wxRIGHT, FromDIP(8));
    bSizer_control->Add(0, 0, 0, wxTOP, FromDIP(6));
    bSizer_control->Add(m_filament_load_sizer,  0, wxALIGN_CENTER|wxLEFT|wxRIGHT, FromDIP(8));
    bSizer_control->Add(0, 0, 0, wxTOP, FromDIP(4));

    bSizer_right->Add(bSizer_control, 1, wxEXPAND | wxALL, 0);

    return bSizer_right;
}

wxBoxSizer *StatusBasePanel::create_temp_axis_group(wxWindow *parent)
{
    auto        sizer         = new wxBoxSizer(wxVERTICAL);
    auto        box           = new StaticBox(parent);

    StateColor box_colour(std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
    StateColor box_border_colour(std::pair<wxColour, int>(STATUS_PANEL_BG, StateColor::Normal));

    box->SetBackgroundColor(box_colour);
    box->SetBorderColor(box_border_colour);
    box->SetCornerRadius(5);

    box->SetMinSize(wxSize(FromDIP(586), -1));
    box->SetMaxSize(wxSize(FromDIP(586), -1));

    wxBoxSizer *content_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *m_temp_ctrl   = create_temp_control(box);


    m_temp_temp_line = new wxPanel(box);
    m_temp_temp_line->SetMaxSize(wxSize(FromDIP(1), -1));
    m_temp_temp_line->SetMinSize(wxSize(FromDIP(1), -1));
    m_temp_temp_line->SetBackgroundColour(STATIC_BOX_LINE_COL);


    auto m_axis_sizer = create_axis_control(box);
    auto bedPanel = create_bed_control(box);

    wxBoxSizer *extruder_sizer = create_extruder_control(box);
    wxBoxSizer* axis_and_bed_control_sizer = new wxBoxSizer(wxVERTICAL);
    axis_and_bed_control_sizer->Add(m_axis_sizer, 0, wxEXPAND | wxALL, 0);
    axis_and_bed_control_sizer->Add(bedPanel, 0, wxALIGN_CENTER, 0);

    content_sizer->Add(m_temp_ctrl, 0, wxEXPAND | wxALL, FromDIP(5));
    content_sizer->Add(m_temp_temp_line, 0, wxEXPAND, 1);
    //content_sizer->Add(FromDIP(9), 0, 0, wxEXPAND, 1);
    /*content_sizer->Add(0, 0, 0, wxLEFT, FromDIP(18));
    content_sizer->Add(m_axis_sizer, 0, wxALIGN_CENTER_VERTICAL | wxALL, 0);
    content_sizer->Add(0, 0, 0, wxLEFT, FromDIP(18));
    content_sizer->Add(bed_sizer, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, FromDIP(12));*/
    content_sizer->Add(axis_and_bed_control_sizer, 1, wxALIGN_CENTER, 0);
    //content_sizer->Add(0, 0, 0, wxLEFT, FromDIP(18));

    m_temp_extruder_line = new wxPanel(box);
    m_temp_extruder_line->SetMaxSize(wxSize(FromDIP(1), -1));
    m_temp_extruder_line->SetMinSize(wxSize(FromDIP(1), -1));
    m_temp_extruder_line->SetBackgroundColour(STATIC_BOX_LINE_COL);

    content_sizer->Add(m_temp_extruder_line, 0, wxEXPAND, 1);
    content_sizer->Add(extruder_sizer, 0, wxEXPAND  | wxTOP | wxBOTTOM, FromDIP(12));
    content_sizer->Add(0, 0, 0, wxRIGHT, FromDIP(3));

    box->SetSizer(content_sizer);
    sizer->Add(box, 0, wxEXPAND | wxALL, FromDIP(9));
    return sizer;
}

wxBoxSizer *StatusBasePanel::create_temp_control(wxWindow *parent)
{
    auto sizer = new wxBoxSizer(wxVERTICAL);

    wxWindowID nozzle_id = wxWindow::NewControlId();
    m_tempCtrl_nozzle    = new TempInput(parent, nozzle_id, TEMP_BLANK_STR, TempInputType::TEMP_OF_NORMAL_TYPE, TEMP_BLANK_STR, wxString("monitor_nozzle_temp"),
                                      wxString("monitor_nozzle_temp_active"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_tempCtrl_nozzle->SetMinSize(TEMP_CTRL_MIN_SIZE_ALIGN_TWO_ICON);
    m_tempCtrl_nozzle->AddTemp(0); // zero is default temp
    m_tempCtrl_nozzle->SetMinTemp(20);
    m_tempCtrl_nozzle->SetMaxTemp(300);
    m_tempCtrl_nozzle->SetBorderWidth(FromDIP(2));

    StateColor tempinput_text_colour(std::make_pair(DISCONNECT_TEXT_COL, (int) StateColor::Disabled), std::make_pair(NORMAL_TEXT_COL, (int) StateColor::Normal));
    StateColor tempinput_border_colour(std::make_pair(*wxWHITE, (int)StateColor::Disabled), std::make_pair(BUTTON_HOVER_COL, (int)StateColor::Focused),
        std::make_pair(BUTTON_HOVER_COL, (int)StateColor::Hovered), std::make_pair(*wxWHITE, (int)StateColor::Normal));

    m_tempCtrl_nozzle->SetTextColor(tempinput_text_colour);
    m_tempCtrl_nozzle->SetBorderColor(tempinput_border_colour);

    m_tempCtrl_nozzle_deputy = new TempInput(parent, nozzle_id, TEMP_BLANK_STR, TempInputType::TEMP_OF_NORMAL_TYPE, TEMP_BLANK_STR, wxString("monitor_nozzle_temp"), wxString("monitor_nozzle_temp_active"),
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_tempCtrl_nozzle_deputy->SetMinSize(TEMP_CTRL_MIN_SIZE_ALIGN_TWO_ICON);
    m_tempCtrl_nozzle_deputy->AddTemp(0); // zero is default temp
    m_tempCtrl_nozzle_deputy->SetMinTemp(20);
    m_tempCtrl_nozzle_deputy->SetMaxTemp(300);
    m_tempCtrl_nozzle_deputy->SetBorderWidth(FromDIP(2));

    m_tempCtrl_nozzle_deputy->SetTextColor(tempinput_text_colour);
    m_tempCtrl_nozzle_deputy->SetBorderColor(tempinput_border_colour);

    sizer->Add(m_tempCtrl_nozzle_deputy, 0, wxEXPAND | wxALL, 1);
    sizer->Add(m_tempCtrl_nozzle, 0, wxEXPAND | wxALL, 1);
    m_tempCtrl_nozzle_deputy->Hide();

    m_line_nozzle = new StaticLine(parent);
    m_line_nozzle->SetLineColour(STATIC_BOX_LINE_COL);
    m_line_nozzle->SetSize(wxSize(FromDIP(1), -1));
    sizer->Add(m_line_nozzle, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    wxWindowID bed_id = wxWindow::NewControlId();
    m_tempCtrl_bed    = new TempInput(parent, bed_id, TEMP_BLANK_STR, TempInputType::TEMP_OF_NORMAL_TYPE, TEMP_BLANK_STR, wxString("monitor_bed_temp"),
        wxString("monitor_bed_temp_active"), wxDefaultPosition,wxDefaultSize, wxALIGN_CENTER);
    m_tempCtrl_bed->AddTemp(0); // zero is default temp
    m_tempCtrl_bed->SetMinTemp(bed_temp_range[0]);
    m_tempCtrl_bed->SetMaxTemp(bed_temp_range[1]);
    m_tempCtrl_bed->SetMinSize(TEMP_CTRL_MIN_SIZE_ALIGN_ONE_ICON);
    m_tempCtrl_bed->SetBorderWidth(FromDIP(2));
    m_tempCtrl_bed->SetTextColor(tempinput_text_colour);
    m_tempCtrl_bed->SetBorderColor(tempinput_border_colour);
    sizer->Add(m_tempCtrl_bed, 0, wxEXPAND | wxALL, 1);

    auto line = new StaticLine(parent);
    line->SetLineColour(STATIC_BOX_LINE_COL);
    sizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    wxWindowID frame_id = wxWindow::NewControlId();
    m_tempCtrl_chamber    = new TempInput(parent, frame_id, TEMP_BLANK_STR, TempInputType::TEMP_OF_NORMAL_TYPE, TEMP_BLANK_STR, wxString("monitor_frame_temp"),
        wxString("monitor_frame_temp_active"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_tempCtrl_chamber->AddTemp(0); // zero is default temp
    m_tempCtrl_chamber->SetReadOnly(true);
    m_tempCtrl_chamber->SetMinTemp(default_champer_temp_min);
    m_tempCtrl_chamber->SetMaxTemp(default_champer_temp_max);
    m_tempCtrl_chamber->SetMinSize(TEMP_CTRL_MIN_SIZE_ALIGN_ONE_ICON);
    m_tempCtrl_chamber->SetBorderWidth(FromDIP(2));
    m_tempCtrl_chamber->SetTextColor(tempinput_text_colour);
    m_tempCtrl_chamber->SetBorderColor(tempinput_border_colour);
    sizer->Add(m_tempCtrl_chamber, 0, wxEXPAND | wxALL, 1);

    m_misc_ctrl_sizer = create_misc_control(parent);
    sizer->Add(m_misc_ctrl_sizer, 0, wxEXPAND, 0);
    return sizer;
}

wxBoxSizer *StatusBasePanel::create_misc_control(wxWindow *parent)
{
    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer *line_sizer = new wxBoxSizer(wxHORIZONTAL);

    /* create speed control */
    m_switch_speed = new ImageSwitchButton(parent, m_bitmap_speed_active, m_bitmap_speed);
    m_switch_speed->SetLabels("100%", "100%");
    m_switch_speed->SetMinSize(MISC_BUTTON_2FAN_SIZE);
    m_switch_speed->SetMaxSize(MISC_BUTTON_2FAN_SIZE);
    m_switch_speed->SetPadding(FromDIP(3));
    m_switch_speed->SetBorderWidth(FromDIP(2));
    m_switch_speed->SetFont(Label::Head_13);
    m_switch_speed->SetTextColor(StateColor(std::make_pair(DISCONNECT_TEXT_COL, (int) StateColor::Disabled), std::make_pair(NORMAL_TEXT_COL, (int) StateColor::Normal)));
    m_switch_speed->SetValue(false);

    line_sizer->Add(m_switch_speed, 1, wxALIGN_CENTER | wxALL, 0);

    auto line = new StaticLine(parent, true);
    line->SetLineColour(STATIC_BOX_LINE_COL);
    line_sizer->Add(line, 0, wxEXPAND | wxTOP | wxBOTTOM, 4);

    /* create lamp control */
    m_switch_lamp = new ImageSwitchButton(parent, m_bitmap_lamp_on, m_bitmap_lamp_off);
    m_switch_lamp->SetLabels(_L("Lamp"), _L("Lamp"));
    m_switch_lamp->SetMinSize(MISC_BUTTON_2FAN_SIZE);
    m_switch_lamp->SetMaxSize(MISC_BUTTON_2FAN_SIZE);
    m_switch_lamp->SetPadding(FromDIP(3));
    m_switch_lamp->SetBorderWidth(FromDIP(2));
    m_switch_lamp->SetFont(Label::Head_13);
    m_switch_lamp->SetTextColor(StateColor(std::make_pair(DISCONNECT_TEXT_COL, (int) StateColor::Disabled), std::make_pair(NORMAL_TEXT_COL, (int) StateColor::Normal)));
    line_sizer->Add(m_switch_lamp, 1, wxALIGN_CENTER | wxALL, 0);

    //sizer->Add(line_sizer, 0, wxEXPAND, FromDIP(5));
    line = new StaticLine(parent);
    line->SetLineColour(STATIC_BOX_LINE_COL);
    sizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    m_fan_panel = new StaticBox(parent);
    m_fan_panel->SetMinSize(MISC_BUTTON_PANEL_SIZE);
    m_fan_panel->SetMaxSize(MISC_BUTTON_PANEL_SIZE);
    m_fan_panel->SetBackgroundColor(*wxWHITE);
    m_fan_panel->SetBorderWidth(0);
    m_fan_panel->SetCornerRadius(0);

    auto fan_line_sizer          = new wxBoxSizer(wxHORIZONTAL);
    m_switch_fan = new FanSwitchButton(m_fan_panel, m_bitmap_fan_on, m_bitmap_fan_off);
    m_switch_fan->SetValue(false);
    m_switch_fan->SetMinSize(MISC_BUTTON_1FAN_SIZE);
    m_switch_fan->SetMaxSize(MISC_BUTTON_1FAN_SIZE);
    m_switch_fan->SetPadding(FromDIP(1));
    m_switch_fan->SetBorderWidth(0);
    m_switch_fan->SetCornerRadius(0);
    m_switch_fan->SetFont(::Label::Body_10);
    m_switch_fan->UseTextFan();
    m_switch_fan->SetTextColor(
        StateColor(std::make_pair(DISCONNECT_TEXT_COL, (int)StateColor::Disabled), std::make_pair(NORMAL_FAN_TEXT_COL, (int)StateColor::Normal)));

    m_switch_fan->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {
        m_fan_panel->SetBackgroundColor(wxColour(0, 150, 136));
        });

    m_switch_fan->Bind(wxEVT_LEAVE_WINDOW, [this, parent](auto& e) {
        m_fan_panel->SetBackgroundColor(parent->GetBackgroundColour());
        });

    fan_line_sizer->Add(m_switch_fan, 1, wxEXPAND|wxALL, FromDIP(2));

    m_fan_panel->SetSizer(fan_line_sizer);
    m_fan_panel->Layout();
    m_fan_panel->Fit();
    sizer->Add(m_fan_panel, 0, wxEXPAND, FromDIP(5));
    line = new StaticLine(parent);
    line->SetLineColour(STATIC_BOX_LINE_COL);
    sizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    sizer->Add(line_sizer, 0, wxEXPAND, FromDIP(5));
    return sizer;
}

void StatusBasePanel::reset_temp_misc_control()
{
    // reset temp string
    m_tempCtrl_nozzle->SetIconNormal();
    m_tempCtrl_nozzle->SetLabel(TEMP_BLANK_STR);
    m_tempCtrl_nozzle->GetTextCtrl()->SetValue(TEMP_BLANK_STR);

    m_tempCtrl_nozzle_deputy->SetIconNormal();
    m_tempCtrl_nozzle_deputy->SetLabel(TEMP_BLANK_STR);
    m_tempCtrl_nozzle_deputy->GetTextCtrl()->SetValue(TEMP_BLANK_STR);

    m_tempCtrl_bed->SetIconNormal();
    m_tempCtrl_bed->SetLabel(TEMP_BLANK_STR);
    m_tempCtrl_bed->GetTextCtrl()->SetValue(TEMP_BLANK_STR);

    m_tempCtrl_chamber->SetIconNormal();
    m_tempCtrl_chamber->SetLabel(TEMP_BLANK_STR);
    m_tempCtrl_chamber->GetTextCtrl()->SetValue(TEMP_BLANK_STR);

    m_tempCtrl_nozzle->Enable(true);
    m_tempCtrl_nozzle_deputy->Enable(true);
    m_tempCtrl_chamber->Enable(true);
    m_tempCtrl_bed->Enable(true);

    // reset misc control
    m_switch_speed->SetLabels("100%", "100%");
    m_switch_speed->SetValue(false);
    m_switch_lamp->SetLabels(_L("Lamp"), _L("Lamp"));
    m_switch_lamp->SetValue(false);
    /*m_switch_nozzle_fan->SetValue(false);
    m_switch_printing_fan->SetValue(false);
    m_switch_cham_fan->SetValue(false);*/
}

wxBoxSizer *StatusBasePanel::create_axis_control(wxWindow *parent)
{
    auto sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddStretchSpacer();
    m_bpButton_xy = new AxisCtrlButton(parent, m_bitmap_axis_home);
    m_bpButton_xy->SetTextColor(StateColor(std::make_pair(DISCONNECT_TEXT_COL, (int) StateColor::Disabled), std::make_pair(NORMAL_TEXT_COL, (int) StateColor::Normal)));
    m_bpButton_xy->SetMinSize(AXIS_MIN_SIZE);
    m_bpButton_xy->SetSize(AXIS_MIN_SIZE);
    sizer->AddStretchSpacer();
    sizer->Add(m_bpButton_xy, 0, wxALIGN_CENTER | wxALL, 0);
    sizer->AddStretchSpacer();
    return sizer;
}

wxPanel *StatusBasePanel::create_bed_control(wxWindow *parent)
{
    wxBoxSizer *bSizer_z_ctrl = new wxBoxSizer(wxHORIZONTAL);
    auto        panel         = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(*wxWHITE);

    StateColor z_10_ctrl_bg(std::pair<wxColour, int>(BUTTON_PRESS_COL, StateColor::Pressed), std::pair<wxColour, int>(BUTTON_NORMAL1_COL, StateColor::Normal));
    StateColor z_10_ctrl_bd(std::pair<wxColour, int>(BUTTON_HOVER_COL, StateColor::Hovered), std::pair<wxColour, int>(BUTTON_NORMAL1_COL, StateColor::Normal));

    StateColor z_1_ctrl_bg(std::pair<wxColour, int>(BUTTON_PRESS_COL, StateColor::Pressed), std::pair<wxColour, int>(BUTTON_NORMAL2_COL, StateColor::Normal));
    StateColor z_1_ctrl_bd(std::pair<wxColour, int>(BUTTON_HOVER_COL, StateColor::Hovered), std::pair<wxColour, int>(BUTTON_NORMAL2_COL, StateColor::Normal));

    m_bpButton_z_10 = new Button(panel, wxString("10"), "monitor_bed_up", 0, 15); // Orca Dont scale icon size 
    m_bpButton_z_10->SetFont(::Label::Body_12);
    m_bpButton_z_10->SetBorderWidth(0);
    m_bpButton_z_10->SetBackgroundColor(z_10_ctrl_bg);
    m_bpButton_z_10->SetBorderColor(z_10_ctrl_bd);
    m_bpButton_z_10->SetTextColor(StateColor(std::make_pair(DISCONNECT_TEXT_COL, (int) StateColor::Disabled), std::make_pair(NORMAL_TEXT_COL, (int) StateColor::Normal)));
    m_bpButton_z_10->SetMinSize(Z_BUTTON_SIZE);
    m_bpButton_z_10->SetSize(Z_BUTTON_SIZE);
    m_bpButton_z_10->SetCornerRadius(0);
    m_bpButton_z_1 = new Button(panel, wxString(" 1"), "monitor_bed_up", 0, 15); // Orca Dont scale icon size 
    m_bpButton_z_1->SetFont(::Label::Body_12);
    m_bpButton_z_1->SetBorderWidth(0);
    m_bpButton_z_1->SetBackgroundColor(z_1_ctrl_bg);
    m_bpButton_z_1->SetBorderColor(z_1_ctrl_bd);
    m_bpButton_z_1->SetMinSize(Z_BUTTON_SIZE);
    m_bpButton_z_1->SetSize(Z_BUTTON_SIZE);
    m_bpButton_z_1->SetTextColor(StateColor(std::make_pair(DISCONNECT_TEXT_COL, (int) StateColor::Disabled), std::make_pair(NORMAL_TEXT_COL, (int) StateColor::Normal)));

    //bSizer_z_ctrl->Add(0, FromDIP(6), 0, wxEXPAND, 0);

    m_staticText_z_tip = new wxStaticText(panel, wxID_ANY, _L("Bed"), wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_z_tip->SetFont(::Label::Body_12);
    if (wxGetApp().app_config->get("language") == "de_DE") m_staticText_z_tip->SetFont(::Label::Body_11);
    m_staticText_z_tip->Wrap(-1);
    m_staticText_z_tip->SetForegroundColour(TEXT_LIGHT_FONT_COL);
    m_bpButton_z_down_1 = new Button(panel, wxString(" 1"), "monitor_bed_down", 0, 15); // Orca Dont scale icon size 
    m_bpButton_z_down_1->SetFont(::Label::Body_12);
    m_bpButton_z_down_1->SetBorderWidth(0);
    m_bpButton_z_down_1->SetBackgroundColor(z_1_ctrl_bg);
    m_bpButton_z_down_1->SetBorderColor(z_1_ctrl_bd);
    m_bpButton_z_down_1->SetMinSize(Z_BUTTON_SIZE);
    m_bpButton_z_down_1->SetSize(Z_BUTTON_SIZE);
    m_bpButton_z_down_1->SetTextColor(StateColor(std::make_pair(DISCONNECT_TEXT_COL, (int) StateColor::Disabled), std::make_pair(NORMAL_TEXT_COL, (int) StateColor::Normal)));

    m_bpButton_z_down_10 = new Button(panel, wxString("10"), "monitor_bed_down", 0, 15); // Orca Dont scale icon size 
    m_bpButton_z_down_10->SetFont(::Label::Body_12);
    m_bpButton_z_down_10->SetBorderWidth(0);
    m_bpButton_z_down_10->SetBackgroundColor(z_10_ctrl_bg);
    m_bpButton_z_down_10->SetBorderColor(z_10_ctrl_bd);
    m_bpButton_z_down_10->SetMinSize(Z_BUTTON_SIZE);
    m_bpButton_z_down_10->SetSize(Z_BUTTON_SIZE);
    m_bpButton_z_down_10->SetTextColor(StateColor(std::make_pair(DISCONNECT_TEXT_COL, (int) StateColor::Disabled), std::make_pair(NORMAL_TEXT_COL, (int) StateColor::Normal)));

    bSizer_z_ctrl->Add(m_bpButton_z_10, 0, wxEXPAND | wxLEFT | wxRIGHT, 0);
    bSizer_z_ctrl->Add(m_bpButton_z_1, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(2));
    bSizer_z_ctrl->Add(m_staticText_z_tip, 0, wxALIGN_CENTRE, FromDIP(5));
    bSizer_z_ctrl->Add(m_bpButton_z_down_1, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(2));
    bSizer_z_ctrl->Add(m_bpButton_z_down_10, 0, wxEXPAND | wxLEFT | wxRIGHT, 0);

    panel->SetSizer(bSizer_z_ctrl);
    panel->Layout();
    panel->Fit();

    return panel;
}

wxBoxSizer *StatusBasePanel::create_extruder_control(wxWindow *parent)
{
    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *bSizer_e_ctrl = new wxBoxSizer(wxVERTICAL);
    auto        panel = new wxPanel(parent,wxID_ANY);

    panel->SetBackgroundColour(*wxWHITE);
    panel->SetSize(wxSize(FromDIP(143), -1));
    panel->SetMinSize(wxSize(FromDIP(143), -1));
    panel->SetMaxSize(wxSize(FromDIP(143), -1));

    StateColor e_ctrl_bg(std::pair<wxColour, int>(BUTTON_PRESS_COL, StateColor::Pressed), std::pair<wxColour, int>(BUTTON_NORMAL1_COL, StateColor::Normal));
    StateColor e_ctrl_bd(std::pair<wxColour, int>(BUTTON_HOVER_COL, StateColor::Hovered), std::pair<wxColour, int>(BUTTON_NORMAL1_COL, StateColor::Normal));

    m_nozzle_btn_panel = new SwitchBoard(panel, _L("Left"), _L("Right"), wxSize(FromDIP(126), FromDIP(26)));
    m_nozzle_btn_panel->SetAutoDisableWhenSwitch();

    m_bpButton_e_10 = new Button(panel, "", "monitor_extruder_up", 0, 22); // Orca Dont scale icon size 
    m_bpButton_e_10->SetBorderWidth(2);
    m_bpButton_e_10->SetBackgroundColor(e_ctrl_bg);
    m_bpButton_e_10->SetBorderColor(e_ctrl_bd);
    m_bpButton_e_10->SetMinSize(wxSize(FromDIP(40), FromDIP(40)));

    m_extruder_book = new wxSimplebook(panel, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(45), FromDIP(112)), 0);

    m_extruder_book->InsertPage(0, new wxPanel(panel), "");
    for (int nozzle_num = 1; nozzle_num <= 2; nozzle_num++) {
        auto extruder_img = new ExtruderImage(m_extruder_book, wxID_ANY, nozzle_num);
        m_extruder_book->InsertPage(nozzle_num, extruder_img, "");
        m_extruderImage.push_back(extruder_img);
    }
    m_extruder_book->SetSelection(0);

    m_bpButton_e_down_10 = new Button(panel, "", "monitor_extruder_down", 0, 22); // Orca Dont scale icon size 
    m_bpButton_e_down_10->SetBorderWidth(2);
    m_bpButton_e_down_10->SetBackgroundColor(e_ctrl_bg);
    m_bpButton_e_down_10->SetBorderColor(e_ctrl_bd);
    m_bpButton_e_down_10->SetMinSize(wxSize(FromDIP(40), FromDIP(40)));

    m_extruder_switching_status = new ExtruderSwithingStatus(panel);
    m_extruder_switching_status->SetForegroundColour(TEXT_LIGHT_FONT_COL);

    m_extruder_label = new ::Label(panel, _L("Extruder"));
    m_extruder_label->SetFont(::Label::Body_13);
    m_extruder_label->SetForegroundColour(TEXT_LIGHT_FONT_COL);

    bSizer_e_ctrl->Add(0, 0, 0, wxTOP, FromDIP(15));
    bSizer_e_ctrl->Add(m_nozzle_btn_panel, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    bSizer_e_ctrl->Add(0, 0, 0, wxTOP, FromDIP(15));
    bSizer_e_ctrl->Add(m_bpButton_e_10, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    bSizer_e_ctrl->Add(0, 0, 0, wxTOP, FromDIP(7));
    bSizer_e_ctrl->Add(m_extruder_book, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    bSizer_e_ctrl->Add(0, 0, 0, wxTOP, FromDIP(7));
    bSizer_e_ctrl->Add(m_bpButton_e_down_10, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    bSizer_e_ctrl->Add(0, 0, 1, wxEXPAND, 0);
    bSizer_e_ctrl->Add(m_extruder_switching_status, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    bSizer_e_ctrl->Add(m_extruder_label, 0, wxTOP | wxALIGN_CENTER_HORIZONTAL, FromDIP(10));

    panel->SetSizer(bSizer_e_ctrl);
    panel->Layout();
    sizer->Add(panel, 1, wxEXPAND, 0);
    return sizer;
}

wxBoxSizer *StatusBasePanel::create_ams_group(wxWindow *parent)
{
    auto sizer     = new wxBoxSizer(wxVERTICAL);
    auto sizer_box = new wxBoxSizer(wxVERTICAL);

    m_ams_control_box = new StaticBox(parent);

    StateColor box_colour(std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
    StateColor box_border_colour(std::pair<wxColour, int>(STATUS_PANEL_BG, StateColor::Normal));

    m_ams_control_box->SetBackgroundColor(box_colour);
    m_ams_control_box->SetBorderColor(box_border_colour);
    m_ams_control_box->SetCornerRadius(5);

    m_ams_control_box->SetMinSize(wxSize(FromDIP(586), -1));
    m_ams_control_box->SetBackgroundColour(*wxWHITE);
#if !BBL_RELEASE_TO_PUBLIC
    m_ams_debug = new wxStaticText(m_ams_control_box, wxID_ANY, _L("Debug Info"), wxDefaultPosition, wxDefaultSize, 0);
    sizer_box->Add(m_ams_debug, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    m_ams_debug->Hide();
#endif

    m_ams_control = new AMSControl(m_ams_control_box, wxID_ANY);
    //m_ams_control->SetMinSize(wxSize(FromDIP(510), FromDIP(286)));
    m_ams_control->SetDoubleBuffered(true);
    sizer_box->Add(m_ams_control, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(10));

    m_ams_control_box->SetBackgroundColour(*wxWHITE);
    m_ams_control_box->SetSizer(sizer_box);
    m_ams_control_box->Layout();
    m_ams_control_box->Fit();
    sizer->Add(m_ams_control_box, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(0));
    return sizer;
}

wxBoxSizer* StatusBasePanel::create_filament_group(wxWindow* parent)
{
    auto sizer = new wxBoxSizer(wxVERTICAL);

    auto sizer_scale_panel = new wxBoxSizer(wxHORIZONTAL);
    m_scale_panel          = new wxPanel(parent);
    m_scale_panel->SetMinSize(wxSize(FromDIP(586), FromDIP(40)));
    m_scale_panel->SetMaxSize(wxSize(FromDIP(586), FromDIP(40)));
    m_scale_panel->SetBackgroundColour(*wxWHITE);

    auto m_title_filament_loading = new Label(m_scale_panel, _L("Filament loading..."));
    m_title_filament_loading->SetBackgroundColour(*wxWHITE);
    m_title_filament_loading->SetForegroundColour(wxColour(0, 137, 123));
    m_title_filament_loading->SetFont(::Label::Body_14);

    m_img_filament_loading = new wxStaticBitmap(m_scale_panel, wxID_ANY, create_scaled_bitmap("filament_load_fold", this, 24), wxDefaultPosition, wxSize(FromDIP(24), FromDIP(24)), 0);

    sizer_scale_panel->Add(0, 0, 0, wxLEFT, FromDIP(20));
    sizer_scale_panel->Add(m_title_filament_loading, 0, wxALIGN_CENTER, 0);
    sizer_scale_panel->Add(m_img_filament_loading, 0, wxALIGN_CENTER, 0);
    m_scale_panel->SetSizer(sizer_scale_panel);
    m_scale_panel->Layout();
    m_scale_panel->Fit();
    m_scale_panel->Hide();


    m_title_filament_loading->Bind(wxEVT_LEFT_DOWN, &StatusBasePanel::expand_filament_loading, this);
    m_scale_panel->Bind(wxEVT_LEFT_DOWN, &StatusBasePanel::expand_filament_loading, this);


    auto sizer_box = new wxBoxSizer(wxVERTICAL);

    StateColor box_colour(std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
    StateColor box_border_colour(std::pair<wxColour, int>(STATUS_PANEL_BG, StateColor::Normal));

    m_filament_load_box = new StaticBox(parent);
    m_filament_load_box->SetBackgroundColor(box_colour);
    m_filament_load_box->SetBorderColor(box_border_colour);
    m_filament_load_box->SetCornerRadius(5);
    m_filament_load_box->SetMinSize(wxSize(FromDIP(586), -1));
    m_filament_load_box->SetMaxSize(wxSize(FromDIP(586), -1));
    m_filament_load_box->SetBackgroundColour(*wxWHITE);
    m_filament_load_box->SetSizer(sizer_box);

    m_filament_step = new FilamentLoad(m_filament_load_box, wxID_ANY);
    m_filament_step->SetDoubleBuffered(true);
    m_filament_step->set_min_size(wxSize(wxSize(FromDIP(300), FromDIP(215))));
    m_filament_step->set_max_size(wxSize(wxSize(FromDIP(300), FromDIP(215))));
    m_filament_step->SetBackgroundColour(*wxWHITE);

    m_filament_load_img = new wxStaticBitmap(m_filament_load_box, wxID_ANY, wxNullBitmap);
    m_filament_load_img->SetBackgroundColour(*wxWHITE);

    wxBoxSizer *steps_sizer = new wxBoxSizer(wxHORIZONTAL);
    steps_sizer->Add(m_filament_step, 0, wxALIGN_LEFT, FromDIP(20));
    steps_sizer->Add(m_filament_load_img, 0, wxALIGN_TOP, FromDIP(30));
    steps_sizer->AddStretchSpacer();

    m_button_retry = new Button(m_filament_load_box, _L("Retry"));
    m_button_retry->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    //m_button_retry->Hide();

    m_button_retry->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        BOOST_LOG_TRIVIAL(info) << "on_ams_retry";
        if (obj) { obj->command_ams_control("resume"); }
    });


    sizer_box->Add(steps_sizer, 0, wxEXPAND | wxALIGN_LEFT | wxTOP, FromDIP(5));
    sizer_box->Add(m_button_retry, 0, wxLEFT, FromDIP(28));
    sizer_box->Add(0, 0, 0, wxTOP, FromDIP(5));
    m_filament_load_box->SetBackgroundColour(*wxWHITE);
    m_filament_load_box->Layout();
    m_filament_load_box->Fit();
    m_filament_load_box->Hide();
    sizer->Add(m_scale_panel, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(5));
    sizer->Add(m_filament_load_box, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, 0);
    return sizer;
}

void StatusBasePanel::expand_filament_loading(wxMouseEvent& e)
{
    auto tag_show = false;
    if (m_filament_load_box->IsShown()) {
        tag_show = false;
        m_img_filament_loading->SetBitmap(create_scaled_bitmap("filament_load_fold", this, 24));
    } else {
        tag_show = true;
        m_img_filament_loading->SetBitmap(create_scaled_bitmap("filament_load_expand", this, 24));
    }

    if (obj)
    {
        static int load_img_size = 215;
        if (obj->is_series_n())
        {
            m_filament_load_img->SetBitmap(create_scaled_bitmap("filament_load_n_series", this, load_img_size));
        }
        else if (obj->is_series_x())
        {
            m_filament_load_img->SetBitmap(create_scaled_bitmap("filament_load_x_series", this, load_img_size));
        }
        else if (obj->is_series_p())
        {
            m_filament_load_img->SetBitmap(create_scaled_bitmap("filament_load_p_series", this, load_img_size));
        }
        else if (obj->is_series_o())
        {
            const auto& ext_system = obj->GetExtderSystem();
            if (ext_system->GetTotalExtderCount() == 2)
            {
                int cur_extder_id = ext_system->GetCurrentExtderId();
                if (cur_extder_id == MAIN_EXTRUDER_ID)
                {
                    m_filament_load_img->SetBitmap(create_scaled_bitmap("filament_load_o_series_right", this, load_img_size));
                }
                else if (cur_extder_id == DEPUTY_EXTRUDER_ID)
                {
                    m_filament_load_img->SetBitmap(create_scaled_bitmap("filament_load_o_series_left", this, load_img_size));
                }
            }
            else
            {
                m_filament_load_img->SetBitmap(create_scaled_bitmap("filament_load_o_series", this, load_img_size));
            }
        }
    }

    m_filament_load_box->Show(tag_show);
    ///m_button_retry->Show(tag_show);
    m_filament_step->Show(tag_show);
    Layout();
    Fit();
    wxGetApp().mainframe->m_monitor->get_status_panel()->Layout();
    wxGetApp().mainframe->m_monitor->Layout();
}

void StatusBasePanel::show_ams_group(bool show)
{
    if (m_ams_control->IsShown() != show) {
        m_ams_control->Show(show);
        m_ams_control->Layout();
        m_ams_control->Fit();
        Layout();
        Fit();
        wxGetApp().mainframe->m_monitor->Layout();
    }

    if (m_ams_control_box->IsShown() != show) {
        m_ams_control_box->Show(show);
        m_ams_control->Layout();
        m_ams_control->Fit();
        Layout();
        Fit();
        wxGetApp().mainframe->m_monitor->Layout();
    }
}

void StatusBasePanel::show_filament_load_group(bool show)
{
    if (m_scale_panel->IsShown() != show) {
        m_scale_panel->Show(show);
        if (!show) {
            m_img_filament_loading->SetBitmap(create_scaled_bitmap("filament_load_fold", this, 24));
            m_img_filament_loading->Refresh();
        }

        // m_scale_panel control the display of m_filament_load_box
        if (!show && m_filament_load_box->IsShown()) {
            m_filament_load_box->Show(false);
        }

        auto cur_ext = obj->GetExtderSystem()->GetCurrentExtder();
        m_filament_step->SetupSteps(cur_ext ? cur_ext->HasFilamentInExt() : false);

        Layout();
        Fit();

        wxGetApp().mainframe->m_monitor->get_status_panel()->Layout();
        wxGetApp().mainframe->m_monitor->Layout();
    }
}

void StatusPanel::update_camera_state(MachineObject* obj)
{
    if (!obj) return;

    //sdcard
    auto sdcard_state = obj->GetStorage()->get_sdcard_state();
    if (m_last_sdcard != sdcard_state) {
        if (sdcard_state == DevStorage::NO_SDCARD) {
            m_bitmap_sdcard_img->SetBitmap(m_bitmap_sdcard_state_no.bmp());
            m_bitmap_sdcard_img->SetToolTip(_L("No Storage"));
        } else if (sdcard_state == DevStorage::HAS_SDCARD_NORMAL) {
            m_bitmap_sdcard_img->SetBitmap(m_bitmap_sdcard_state_normal.bmp());
            m_bitmap_sdcard_img->SetToolTip(_L("Storage"));
        } else if (sdcard_state == DevStorage::HAS_SDCARD_ABNORMAL) {
            m_bitmap_sdcard_img->SetBitmap(m_bitmap_sdcard_state_abnormal.bmp());
            m_bitmap_sdcard_img->SetToolTip(_L("Storage Abnormal"));
        } else {
            m_bitmap_sdcard_img->SetBitmap(m_bitmap_sdcard_state_normal.bmp());
            m_bitmap_sdcard_img->SetToolTip(_L("Storage"));
        }
        m_last_sdcard = sdcard_state;
        m_panel_monitoring_title->Layout();
    }

    //recording
    if (m_last_recording != (obj->is_recording() ? 1 : 0)) {
        if (obj->is_recording()) {
            m_bitmap_recording_img->SetBitmap(m_bitmap_recording_on.bmp());
        }
        else {
            m_bitmap_recording_img->SetBitmap(m_bitmap_recording_off.bmp());
        }
        m_last_recording = obj->is_recording() ? 1 : 0;
    }

    if (!m_bitmap_recording_img->IsShown()) {
        m_bitmap_recording_img->Show();
        m_panel_monitoring_title->Layout();
    }

    /*if (m_bitmap_recording_img->IsShown())
        m_bitmap_recording_img->Hide();*/

    //timelapse
    if (obj->is_support_timelapse) {
        if (m_last_timelapse != (obj->is_timelapse() ? 1: 0)) {
            if (obj->is_timelapse()) {
                m_bitmap_timelapse_img->SetBitmap(m_bitmap_timelapse_on.bmp());
            } else {
                m_bitmap_timelapse_img->SetBitmap(m_bitmap_timelapse_off.bmp());
            }
            m_last_timelapse = obj->is_timelapse() ? 1 : 0;
        }

        if (!m_bitmap_timelapse_img->IsShown()) {
            m_bitmap_timelapse_img->Show();
            m_panel_monitoring_title->Layout();
        }
    } else {
        if (m_bitmap_timelapse_img->IsShown()) {
            m_bitmap_timelapse_img->Hide();
            m_panel_monitoring_title->Layout();
        }
    }

    //vcamera
    if (obj->virtual_camera) {
        if (m_last_vcamera != (m_media_play_ctrl->IsStreaming() ? 1: 0)) {
            if (m_media_play_ctrl->IsStreaming()) {
                m_bitmap_vcamera_img->SetBitmap(m_bitmap_vcamera_on.bmp());
            } else {
                m_bitmap_vcamera_img->SetBitmap(m_bitmap_vcamera_off.bmp());
            }
            m_last_vcamera = m_media_play_ctrl->IsStreaming() ? 1 : 0;
        }

        if (!m_bitmap_vcamera_img->IsShown()) {
            m_bitmap_vcamera_img->Show();
            m_panel_monitoring_title->Layout();
        }
    } else {
        if (m_bitmap_vcamera_img->IsShown()) {
            m_bitmap_vcamera_img->Hide();
            m_panel_monitoring_title->Layout();
        }
    }

    //camera setting
    if (m_camera_popup && m_camera_popup->IsShown()) {
        bool show_vcamera = m_media_play_ctrl->IsStreaming();
        m_camera_popup->update(show_vcamera);
    }
}

StatusPanel::StatusPanel(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size, long style, const wxString &name)
    : StatusBasePanel(parent, id, pos, size, style)
{
    init_scaled_buttons();
    m_buttons.push_back(m_bpButton_z_10);
    m_buttons.push_back(m_bpButton_z_1);
    m_buttons.push_back(m_bpButton_z_down_1);
    m_buttons.push_back(m_bpButton_z_down_10);
    m_buttons.push_back(m_bpButton_e_10);
    m_buttons.push_back(m_bpButton_e_down_10);

    obj = nullptr;
    m_score_data         = new ScoreData;
    m_score_data->rating_id = -1;
    /* set default values */
    m_switch_lamp->SetValue(false);
    /*m_switch_printing_fan->SetValue(false);
    m_switch_nozzle_fan->SetValue(false);
    m_switch_cham_fan->SetValue(false);*/
    //m_switch_fan->SetValue(false);

    /* set default enable state */
    m_project_task_panel->enable_partskip_button(nullptr, false);
    m_project_task_panel->enable_pause_resume_button(false, "resume_disable");
    m_project_task_panel->enable_abort_button(false);


    Bind(wxEVT_WEBREQUEST_STATE, &StatusPanel::on_webrequest_state, this);

    Bind(wxCUSTOMEVT_SET_TEMP_FINISH, [this](wxCommandEvent e) {
        int  id   = e.GetInt();
        if (id == m_tempCtrl_bed->GetType()) {
            on_set_bed_temp();
        } else if (id == m_tempCtrl_nozzle->GetType()) {
            if (e.GetString() == wxString::Format("%d", MAIN_EXTRUDER_ID)) {
                on_set_nozzle_temp(MAIN_EXTRUDER_ID);
            } else if (e.GetString() == wxString::Format("%d", DEPUTY_EXTRUDER_ID)) {
                on_set_nozzle_temp(DEPUTY_EXTRUDER_ID);
            } else {
                on_set_nozzle_temp(UNIQUE_EXTRUDER_ID);//there is only one nozzle
            }
        } else if (id == m_tempCtrl_chamber->GetType()) {
            if (!m_tempCtrl_chamber->IsOnChanging()) {
                m_tempCtrl_chamber->SetOnChanging();
                on_set_chamber_temp();
                m_tempCtrl_chamber->ReSetOnChanging();
            }
        }
    });


    // Connect Events
    m_project_task_panel->get_bitmap_thumbnail()->Connect(wxEVT_LEFT_DOWN, wxMouseEventHandler(StatusPanel::refresh_thumbnail_webrequest), NULL, this);
    m_project_task_panel->get_partskip_button()->Connect(wxEVT_LEFT_DOWN, wxCommandEventHandler(StatusPanel::on_subtask_partskip), NULL, this);
    m_project_task_panel->get_pause_resume_button()->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_subtask_pause_resume), NULL, this);
    m_project_task_panel->get_abort_button()->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_subtask_abort), NULL, this);
    m_project_task_panel->get_market_scoring_button()->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_market_scoring), NULL, this);
    m_project_task_panel->get_market_retry_buttom()->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_market_retry), NULL, this); 
    m_project_task_panel->get_clean_button()->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_print_error_clean), NULL, this);

    m_setting_button->Connect(wxEVT_LEFT_DOWN, wxMouseEventHandler(StatusPanel::on_camera_enter), NULL, this);
    m_setting_button->Connect(wxEVT_LEFT_DCLICK, wxMouseEventHandler(StatusPanel::on_camera_enter), NULL, this);
    m_tempCtrl_bed->Connect(wxEVT_KILL_FOCUS, wxFocusEventHandler(StatusPanel::on_bed_temp_kill_focus), NULL, this);
    m_tempCtrl_bed->Connect(wxEVT_SET_FOCUS, wxFocusEventHandler(StatusPanel::on_bed_temp_set_focus), NULL, this);
    m_tempCtrl_nozzle->Connect(wxEVT_KILL_FOCUS, wxFocusEventHandler(StatusPanel::on_nozzle_temp_kill_focus), NULL, this);
    m_tempCtrl_nozzle->Connect(wxEVT_SET_FOCUS, wxFocusEventHandler(StatusPanel::on_nozzle_temp_set_focus), NULL, this);
    m_tempCtrl_nozzle_deputy->Connect(wxEVT_KILL_FOCUS, wxFocusEventHandler(StatusPanel::on_nozzle_temp_kill_focus), NULL, this);
    m_tempCtrl_nozzle_deputy->Connect(wxEVT_SET_FOCUS, wxFocusEventHandler(StatusPanel::on_nozzle_temp_set_focus), NULL, this);
    m_tempCtrl_chamber->Connect(wxEVT_KILL_FOCUS, wxFocusEventHandler(StatusPanel::on_cham_temp_kill_focus), NULL, this);
    m_tempCtrl_chamber->Connect(wxEVT_SET_FOCUS, wxFocusEventHandler(StatusPanel::on_cham_temp_set_focus), NULL, this);
    m_switch_lamp->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_lamp_switch), NULL, this);
    //m_switch_nozzle_fan->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_nozzle_fan_switch), NULL, this); // TODO
    //m_switch_printing_fan->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_nozzle_fan_switch), NULL, this);
    //m_switch_cham_fan->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_nozzle_fan_switch), NULL, this);

    m_switch_fan->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_nozzle_fan_switch), NULL, this); // TODO
    //m_switch_fan->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_nozzle_fan_switch), NULL, this);
    //m_switch_fan->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_nozzle_fan_switch), NULL, this);

    m_bpButton_xy->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_axis_ctrl_xy), NULL, this); // TODO
    m_bpButton_z_10->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_axis_ctrl_z_up_10), NULL, this);
    m_bpButton_z_1->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_axis_ctrl_z_up_1), NULL, this);
    m_bpButton_z_down_1->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_axis_ctrl_z_down_1), NULL, this);
    m_bpButton_z_down_10->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_axis_ctrl_z_down_10), NULL, this);
    m_bpButton_e_10->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_axis_ctrl_e_up_10), NULL, this);
    m_bpButton_e_down_10->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_axis_ctrl_e_down_10), NULL, this);
    m_nozzle_btn_panel->Connect(wxCUSTOMEVT_SWITCH_POS, wxCommandEventHandler(StatusPanel::on_nozzle_selected), NULL, this);

    Bind(EVT_AMS_EXTRUSION_CALI, &StatusPanel::on_filament_extrusion_cali, this);
    Bind(EVT_AMS_LOAD, &StatusPanel::on_ams_load, this);
    Bind(EVT_AMS_UNLOAD, &StatusPanel::on_ams_unload, this);
    Bind(EVT_AMS_SWITCH, &StatusPanel::on_ams_switch, this);
    Bind(EVT_AMS_FILAMENT_BACKUP, &StatusPanel::on_ams_filament_backup, this);
    Bind(EVT_AMS_SETTINGS, &StatusPanel::on_ams_setting_click, this);
    Bind(EVT_AMS_REFRESH_RFID, &StatusPanel::on_ams_refresh_rfid, this);
    Bind(EVT_AMS_ON_SELECTED, &StatusPanel::on_ams_selected, this);
    Bind(EVT_AMS_ON_FILAMENT_EDIT, &StatusPanel::on_filament_edit, this);
    Bind(EVT_VAMS_ON_FILAMENT_EDIT, &StatusPanel::on_ext_spool_edit, this);
    Bind(EVT_AMS_GUIDE_WIKI, &StatusPanel::on_ams_guide, this);
    Bind(EVT_AMS_RETRY, &StatusPanel::on_ams_retry, this);
    Bind(EVT_FAN_CHANGED, &StatusPanel::on_fan_changed, this);
    Bind(EVT_SECONDARY_CHECK_RESUME, &StatusPanel::on_subtask_pause_resume, this);
    Bind(EVT_SECONDARY_CHECK_RETRY, [this](auto &e) { if (m_ams_control) { m_ams_control->on_retry(); }});

    m_switch_speed->Connect(wxEVT_LEFT_DOWN, wxCommandEventHandler(StatusPanel::on_switch_speed), NULL, this);
    m_calibration_btn->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_start_calibration), NULL, this);
    m_options_btn->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_show_print_options), NULL, this);
    m_safety_btn->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_show_safety_options), NULL, this);
    m_parts_btn->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_show_parts_options), NULL, this);
}

StatusPanel::~StatusPanel()
{
    // Disconnect Events
    m_project_task_panel->get_bitmap_thumbnail()->Disconnect(wxEVT_LEFT_DOWN, wxMouseEventHandler(StatusPanel::refresh_thumbnail_webrequest), NULL, this);
    m_project_task_panel->get_partskip_button()->Disconnect(wxEVT_LEFT_DOWN, wxCommandEventHandler(StatusPanel::on_subtask_partskip), NULL, this);
    m_project_task_panel->get_pause_resume_button()->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_subtask_pause_resume), NULL, this);
    m_project_task_panel->get_abort_button()->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_subtask_abort), NULL, this);
    m_project_task_panel->get_market_scoring_button()->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_market_scoring), NULL, this);
    m_project_task_panel->get_market_retry_buttom()->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_market_retry), NULL, this); 
    m_project_task_panel->get_clean_button()->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_print_error_clean), NULL, this);

    m_setting_button->Disconnect(wxEVT_LEFT_DOWN, wxMouseEventHandler(StatusPanel::on_camera_enter), NULL, this);
    m_setting_button->Disconnect(wxEVT_LEFT_DCLICK, wxMouseEventHandler(StatusPanel::on_camera_enter), NULL, this);
    m_tempCtrl_bed->Disconnect(wxEVT_KILL_FOCUS, wxFocusEventHandler(StatusPanel::on_bed_temp_kill_focus), NULL, this);
    m_tempCtrl_bed->Disconnect(wxEVT_SET_FOCUS, wxFocusEventHandler(StatusPanel::on_bed_temp_set_focus), NULL, this);
    m_tempCtrl_nozzle->Disconnect(wxEVT_KILL_FOCUS, wxFocusEventHandler(StatusPanel::on_nozzle_temp_kill_focus), NULL, this);
    m_tempCtrl_nozzle->Disconnect(wxEVT_SET_FOCUS, wxFocusEventHandler(StatusPanel::on_nozzle_temp_set_focus), NULL, this);

    m_tempCtrl_nozzle_deputy->Disconnect(wxEVT_KILL_FOCUS, wxFocusEventHandler(StatusPanel::on_nozzle_temp_kill_focus), NULL, this);
    m_tempCtrl_nozzle_deputy->Disconnect(wxEVT_SET_FOCUS, wxFocusEventHandler(StatusPanel::on_nozzle_temp_set_focus), NULL, this);

    m_switch_lamp->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_lamp_switch), NULL, this);
    /*m_switch_nozzle_fan->Disconnect(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_nozzle_fan_switch), NULL, this);
    m_switch_printing_fan->Disconnect(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_nozzle_fan_switch), NULL, this);
    m_switch_cham_fan->Disconnect(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_nozzle_fan_switch), NULL, this);*/

    //m_switch_fan->Disconnect(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_nozzle_fan_switch), NULL, this);
    //m_switch_fan->Disconnect(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_nozzle_fan_switch), NULL, this);
    m_switch_fan->Disconnect(wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_nozzle_fan_switch), NULL, this);

    m_bpButton_xy->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_axis_ctrl_xy), NULL, this);
    m_bpButton_z_10->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_axis_ctrl_z_up_10), NULL, this);
    m_bpButton_z_1->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_axis_ctrl_z_up_1), NULL, this);
    m_bpButton_z_down_1->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_axis_ctrl_z_down_1), NULL, this);
    m_bpButton_z_down_10->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_axis_ctrl_z_down_10), NULL, this);
    m_bpButton_e_10->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_axis_ctrl_e_up_10), NULL, this);
    m_bpButton_e_down_10->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_axis_ctrl_e_down_10), NULL, this);
    m_nozzle_btn_panel->Disconnect(wxCUSTOMEVT_SWITCH_POS, wxCommandEventHandler(StatusPanel::on_nozzle_selected), NULL, this);
    m_switch_speed->Disconnect(wxEVT_LEFT_DOWN, wxCommandEventHandler(StatusPanel::on_switch_speed), NULL, this);
    m_calibration_btn->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_start_calibration), NULL, this);
    m_options_btn->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_show_print_options), NULL, this);
    m_safety_btn->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_show_safety_options), NULL, this);
    m_parts_btn->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(StatusPanel::on_show_parts_options), NULL, this);

    // remove warning dialogs
    if (abort_dlg != nullptr)
        delete abort_dlg;

    if (ctrl_e_hint_dlg != nullptr)
        delete ctrl_e_hint_dlg;

    if (sdcard_hint_dlg != nullptr)
        delete sdcard_hint_dlg;

    if (m_score_data != nullptr) { 
        delete m_score_data;
    }
}

void StatusPanel::init_scaled_buttons()
{
    m_project_task_panel->init_scaled_buttons();
    m_bpButton_z_10->SetMinSize(Z_BUTTON_SIZE);
    m_bpButton_z_10->SetCornerRadius(0);
    m_bpButton_z_1->SetMinSize(Z_BUTTON_SIZE);
    m_bpButton_z_1->SetCornerRadius(0);
    m_bpButton_z_down_1->SetMinSize(Z_BUTTON_SIZE);
    m_bpButton_z_down_1->SetCornerRadius(0);
    m_bpButton_z_down_10->SetMinSize(Z_BUTTON_SIZE);
    m_bpButton_z_down_10->SetCornerRadius(0);
    m_bpButton_e_10->SetMinSize(wxSize(FromDIP(40), FromDIP(40)));
    m_bpButton_e_10->SetCornerRadius(FromDIP(12));
    m_bpButton_e_down_10->SetMinSize(wxSize(FromDIP(40), FromDIP(40)));
    m_bpButton_e_down_10->SetCornerRadius(FromDIP(12));
}

void StatusPanel::on_market_scoring(wxCommandEvent &event) { 
    if (obj && obj->is_makeworld_subtask() && obj->rating_info && obj->rating_info->request_successful) { // model is mall model and has rating_id
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": on_market_scoring" ;
        if (m_score_data && m_score_data->rating_id == obj->rating_info->rating_id) { // current score data for model is same as mall model
            if (m_score_data->star_count != m_project_task_panel->get_star_count()) m_score_data->star_count = m_project_task_panel->get_star_count();
            ScoreDialog m_score_dlg(this, m_score_data);
            int ret = m_score_dlg.ShowModal();
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": old data";

            if (ret == wxID_OK) { 
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": old data is upload";
                m_score_data->rating_id = -1;
                m_project_task_panel->set_star_count_dirty(false);
                if (obj) obj->get_model_mall_result_need_retry = true;
                return;
            }
            if (m_score_data != nullptr) {
                delete m_score_data;
                m_score_data = nullptr;
            }
            m_score_data = new ScoreData(m_score_dlg.get_score_data()); // when user do not submit score, store the data for next opening the score dialog
            m_project_task_panel->set_star_count(m_score_data->star_count);
        } else {
            int star_count      = m_project_task_panel->get_star_count_dirty() ? m_project_task_panel->get_star_count() : obj->rating_info->start_count;
            bool        success_print = obj->rating_info->success_printed;
            ScoreDialog m_score_dlg(this, obj->get_modeltask()->design_id, obj->get_modeltask()->model_id, obj->get_modeltask()->profile_id, obj->rating_info->rating_id,
                                    success_print, star_count);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": new data";

            std::string comment = obj->rating_info->content;
            if (!comment.empty()) { m_score_dlg.set_comment(comment); }
            
            std::vector<std::string> images_json_array;
            images_json_array = obj->rating_info->image_url_paths;
            if (!images_json_array.empty()) m_score_dlg.set_cloud_bitmap(images_json_array);
            
            int ret = m_score_dlg.ShowModal();

            if (ret == wxID_OK) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": new data is upload";
                m_score_data->rating_id = -1;
                m_project_task_panel->set_star_count_dirty(false);
                if (obj) obj->get_model_mall_result_need_retry = true;
                return;
            }
            if (m_score_data != nullptr) {
                delete m_score_data;
                m_score_data = nullptr;
            }
            m_score_data = new ScoreData(m_score_dlg.get_score_data());
            m_project_task_panel->set_star_count(m_score_data->star_count);
        }
    }
}

void StatusPanel::on_market_retry(wxCommandEvent &event)
{
    if (obj) {
    obj->get_model_mall_result_need_retry = true;
    } else {
        BOOST_LOG_TRIVIAL(info)<< __FUNCTION__ << "retury failed";
    }
}

void StatusPanel::update_partskip_button(MachineObject *obj) {
    if (!obj) return;

    auto partskip_button = m_project_task_panel->get_partskip_button();
    if( obj->is_support_partskip ){
        partskip_button->Show();
    }else{
        partskip_button->Hide();
    }
    BOOST_LOG_TRIVIAL(info) << "part skip: is_support_partskip: "<< obj->is_support_partskip;
}

void StatusPanel::on_subtask_partskip(wxCommandEvent &event)
{
    if (m_partskip_dlg == nullptr) {
        m_partskip_dlg = new PartSkipDialog(this->GetParent());
    }
    
    auto dm = GUI::wxGetApp().getDeviceManager();
    m_partskip_dlg->InitSchedule(dm->get_selected_machine());
    BOOST_LOG_TRIVIAL(info) << "part skip: initial part skip dialog.";
    if(m_partskip_dlg->ShowModal() == wxID_OK){
        int cnt = m_partskip_dlg->GetAllSkippedPartsNum();
        m_project_task_panel->set_part_skipped_count(cnt);
        m_project_task_panel->set_part_skipped_dirty(5);
        BOOST_LOG_TRIVIAL(info) << "part skip: prepare to filter printer dirty data.";
    }
}

void StatusPanel::on_subtask_pause_resume(wxCommandEvent &event)
{
    if (obj) {
        if (obj->can_resume()) {
            BOOST_LOG_TRIVIAL(info) << "monitor: resume current print task dev_id =" << obj->get_dev_id();
            obj->command_task_resume();
        }
        else {
            BOOST_LOG_TRIVIAL(info) << "monitor: pause current print task dev_id =" << obj->get_dev_id();
            obj->command_task_pause();
        }
    }
}

void StatusPanel::on_subtask_abort(wxCommandEvent &event)
{
    if (abort_dlg == nullptr) {
        abort_dlg = new SecondaryCheckDialog(this->GetParent(), wxID_ANY, _L("Cancel print"));
        abort_dlg->Bind(EVT_SECONDARY_CHECK_CONFIRM, [this](wxCommandEvent &e) {
            if (obj) {
                BOOST_LOG_TRIVIAL(info) << "monitor: stop current print task dev_id =" << obj->get_dev_id();
                obj->command_task_abort();
            }
        });
    }
    abort_dlg->update_text(_L("Are you sure you want to stop this print?"));
    abort_dlg->m_button_cancel->SetLabel(_L("No"));
    abort_dlg->m_button_ok->SetLabel(_L("Yes"));
    abort_dlg->on_show();
    abort_dlg->Raise();
}

void StatusPanel::error_info_reset()
{
    m_project_task_panel->error_info_reset();
}

void StatusPanel::on_print_error_clean(wxCommandEvent &event)
{
    error_info_reset();
    skip_print_error = obj->print_error;
    char buf[32];
    ::sprintf(buf, "%08X", skip_print_error);
    BOOST_LOG_TRIVIAL(info) << "skip_print_error: " << buf;
    before_error_code = obj->print_error;
}

void StatusPanel::on_webrequest_state(wxWebRequestEvent &evt)
{
    BOOST_LOG_TRIVIAL(trace) << "monitor: monitor_panel web request state = " << evt.GetState();
    switch (evt.GetState()) {
    case wxWebRequest::State_Completed: {
        if (m_current_print_mode != PrintingTaskType::CALIBRATION ||(m_calib_mode == CalibMode::Calib_Flow_Rate && m_calib_method == CalibrationMethod::CALI_METHOD_MANUAL)) {
            wxImage img(*evt.GetResponse().GetStream());
            img_list.insert(std::make_pair(m_request_url, img));
            wxImage resize_img = img.Scale(m_project_task_panel->get_bitmap_thumbnail()->GetSize().x, m_project_task_panel->get_bitmap_thumbnail()->GetSize().y, wxIMAGE_QUALITY_HIGH);
            m_project_task_panel->set_thumbnail_img(resize_img, "");
            m_project_task_panel->set_brightness_value(get_brightness_value(resize_img));
        }
        if (obj) {
            m_project_task_panel->set_plate_index(obj->m_plate_index);
        } else {
            m_project_task_panel->set_plate_index(-1);
        }
        task_thumbnail_state = ThumbnailState::TASK_THUMBNAIL;
        break;
    }
    case wxWebRequest::State_Failed:
    case wxWebRequest::State_Cancelled:
    case wxWebRequest::State_Unauthorized: {
        m_project_task_panel->set_thumbnail_img(m_thumbnail_brokenimg.bmp(), m_thumbnail_brokenimg.name());
        m_project_task_panel->set_plate_index(-1);
        task_thumbnail_state = ThumbnailState::BROKEN_IMG;
        break;
    }
    case wxWebRequest::State_Active:
    case wxWebRequest::State_Idle: break;
    default: break;
    }
}

void StatusPanel::refresh_thumbnail_webrequest(wxMouseEvent& event)
{
    if (!obj) return;
    if (task_thumbnail_state != ThumbnailState::BROKEN_IMG) return;

    if (obj->slice_info) {
        m_request_url = wxString(obj->slice_info->thumbnail_url);
        if (!m_request_url.IsEmpty()) {
            web_request = wxWebSession::GetDefault().CreateRequest(this, m_request_url);
            BOOST_LOG_TRIVIAL(trace) << "monitor: create new webrequest, state = " << web_request.GetState() << ", url = " << m_request_url;
            if (web_request.GetState() == wxWebRequest::State_Idle)
                web_request.Start();
            BOOST_LOG_TRIVIAL(trace) << "monitor: start new webrequest, state = " << web_request.GetState() << ", url = " << m_request_url;
        }
    }
}


bool StatusPanel::is_task_changed(MachineObject* obj)
{
    if (!obj)
        return false;

    if (last_subtask != obj->subtask_
        || last_profile_id != obj->profile_id_
        || last_task_id != obj->task_id_
        ) {
        last_subtask = obj->subtask_;
        last_profile_id = obj->profile_id_;
        last_task_id = obj->task_id_;
        request_model_info_flag = false;
        m_project_task_panel->set_star_count_dirty(false);
        return true;
    }
    return false;
}

void StatusPanel::update(MachineObject *obj)
{
    if (!obj || !obj->is_info_ready())
    {
        m_nozzle_btn_panel->Disable();
        return;
    }

    //m_project_task_panel->Freeze();
    update_subtask(obj);
    //m_project_task_panel->Thaw();

#if !BBL_RELEASE_TO_PUBLIC
    auto delay1 = std::chrono::duration_cast<std::chrono::milliseconds>(obj->last_utc_time - std::chrono::system_clock::now()).count();
    auto delay2 = std::chrono::duration_cast<std::chrono::milliseconds>(obj->last_push_time - std::chrono::system_clock::now()).count();
    auto delay  = wxString::Format(" %ld/%ld", delay1, delay2);
    m_staticText_timelapse->SetLabel((obj->is_lan_mode_printer() ? "Local Mqtt" : obj->is_tunnel_mqtt ? "Tunnel Mqtt" : "Cloud Mqtt") + delay);
    m_bmToggleBtn_timelapse->Enable(!obj->is_lan_mode_printer());
    m_bmToggleBtn_timelapse->SetValue(obj->is_tunnel_mqtt);
#endif

#if !BBL_RELEASE_TO_PUBLIC
    if (obj->HasRecentCloudMessage() && obj->HasRecentLanMessage()) m_mqtt_source->SetLabel("Cloud+Lan");
    else if (obj->HasRecentCloudMessage()) m_mqtt_source->SetLabel("Cloud");
    else if (obj->HasRecentLanMessage()) m_mqtt_source->SetLabel("Lan");
    else m_mqtt_source->SetLabel("None");
    m_mqtt_source->Show();
#endif

    //m_machine_ctrl_panel->Freeze();
    if (obj->is_in_printing() && !obj->can_resume()) {
        show_printing_status(false, true);
    } else {
        show_printing_status();
    }

    /*STUDIO-12573*/
    if (!obj->is_fdm_type()) {
        m_switch_lamp->Enable(false);
    }

    update_temp_ctrl(obj);
    update_misc_ctrl(obj);

    update_ams(obj);
    update_cali(obj);

    if (obj) {
        //nozzle ui
        //m_button_left_of_extruder->SetSelected();

        // update extrusion calibration
        if (m_extrusion_cali_dlg) {
            m_extrusion_cali_dlg->update_machine_obj(obj);
            m_extrusion_cali_dlg->update();
        }

        // update calibration status
        if (calibration_dlg != nullptr) {
            calibration_dlg->update_machine_obj(obj);
            calibration_dlg->update_cali(obj);
        }

        std::string current_printer_type = obj->printer_type;
        bool supports_safety = DevPrinterConfigUtil::support_safety_options(current_printer_type);

        DevConfig* config = obj->GetConfig();

        if (config->SupportFirstLayerInspect() || config->SupportAIMonitor() || obj->is_support_build_plate_marker_detect || obj->is_support_auto_recovery_step_loss) {
            m_options_btn->Show();
            if (print_options_dlg) {
                print_options_dlg->update_machine_obj(obj);
                print_options_dlg->update_options(obj);
            }
        } else {
            m_options_btn->Hide();
        }


        if (obj->support_door_open_check()) {
            if (supports_safety) {
                m_safety_btn->Show();
                if (safety_options_dlg) {
                    safety_options_dlg->update_machine_obj(obj);
                    safety_options_dlg->update_options(obj);
                }
            } else {
                m_safety_btn->Hide();
            }
        } else {
            m_safety_btn->Hide();
        }

        m_parts_btn->Show();


        if (m_panel_control_title) {
            m_panel_control_title->Layout();
        }

        if (!obj->dev_connection_type.empty()) {
            auto iter_connect_type = m_print_connect_types.find(obj->get_dev_id());
            if (iter_connect_type != m_print_connect_types.end()) {
                if (iter_connect_type->second != obj->dev_connection_type) {

                    if (iter_connect_type->second == "lan" && obj->dev_connection_type == "cloud") {
                        m_print_connect_types[obj->get_dev_id()] = obj->dev_connection_type;
                    }

                    if (iter_connect_type->second == "cloud" && obj->dev_connection_type == "lan") {
                        m_print_connect_types[obj->get_dev_id()] = obj->dev_connection_type;
                    }
                }
            }
            m_print_connect_types[obj->get_dev_id()] = obj->dev_connection_type;
        }

        update_error_message();
    }

    update_camera_state(obj);

    //m_machine_ctrl_panel->Thaw();
}

void StatusPanel::show_recenter_dialog() {
    RecenterDialog dlg(this);
    if (dlg.ShowModal() == wxID_OK)
        obj->command_go_home();
}


void StatusPanel::update_error_message()
{
    if (!obj) return;

    static int last_error = -1;

    if (obj->print_error <= 0) {
        error_info_reset();
    } else if (obj->print_error != last_error) {
        /* clear old dialog */
        if (m_print_error_dlg) { delete m_print_error_dlg; }

        /* show device error message*/
        m_print_error_dlg = new DeviceErrorDialog(obj, this);
        wxString error_msg = m_print_error_dlg->show_error_code(obj->print_error);
        BOOST_LOG_TRIVIAL(info) << "print error: device error code = "<< obj->print_error;

        /* show error message on task panel */
        if(!error_msg.IsEmpty()) { m_project_task_panel->show_error_msg(error_msg); }
    }

    last_error = obj->print_error;
}

void StatusPanel::show_printing_status(bool ctrl_area, bool temp_area)
{
    if (!ctrl_area) {
        m_bpButton_xy->Enable(false);
        m_bpButton_z_10->Enable(false);
        m_bpButton_z_1->Enable(false);
        m_bpButton_z_down_1->Enable(false);
        m_bpButton_z_down_10->Enable(false);
        m_bpButton_e_10->Enable(false);
        m_bpButton_e_down_10->Enable(false);

		m_bpButton_z_10->SetIcon("monitor_bed_up_disable");
		m_bpButton_z_1->SetIcon("monitor_bed_up_disable");
		m_bpButton_z_down_1->SetIcon("monitor_bed_down_disable");
		m_bpButton_z_down_10->SetIcon("monitor_bed_down_disable");
        m_bpButton_e_10->SetIcon("monitor_extruder_up_disable");
        m_bpButton_e_down_10->SetIcon("monitor_extrduer_down_disable");

        m_staticText_z_tip->SetForegroundColour(DISCONNECT_TEXT_COL);
        m_extruder_label->SetForegroundColour(DISCONNECT_TEXT_COL);
    } else {
        m_bpButton_xy->Enable();
        m_bpButton_z_10->Enable();
        m_bpButton_z_1->Enable();
        m_bpButton_z_down_1->Enable();
        m_bpButton_z_down_10->Enable();
        m_bpButton_e_10->Enable();
        m_bpButton_e_down_10->Enable();

        m_bpButton_z_10->SetIcon("monitor_bed_up");
        m_bpButton_z_1->SetIcon("monitor_bed_up");
        m_bpButton_z_down_1->SetIcon("monitor_bed_down");
        m_bpButton_z_down_10->SetIcon("monitor_bed_down");
        m_bpButton_e_10->SetIcon("monitor_extruder_up");
        m_bpButton_e_down_10->SetIcon("monitor_extrduer_down");

        m_staticText_z_tip->SetForegroundColour(TEXT_LIGHT_FONT_COL);
        m_extruder_label->SetForegroundColour(TEXT_LIGHT_FONT_COL);
    }

    if (!temp_area) {
        m_tempCtrl_nozzle->Enable(false);
        m_tempCtrl_nozzle_deputy->Enable(false);
        m_tempCtrl_bed->Enable(false);
        m_tempCtrl_chamber->Enable(false);
        m_switch_speed->Enable(false);
        m_switch_speed->SetValue(false);
        m_switch_lamp->Enable(false);
        /*m_switch_nozzle_fan->Enable(false);
        m_switch_printing_fan->Enable(false);
        m_switch_cham_fan->Enable(false);*/
        m_switch_fan->Enable(false);
    } else {
        m_tempCtrl_nozzle->Enable();
        m_tempCtrl_nozzle_deputy->Enable();
        m_tempCtrl_bed->Enable();
        m_tempCtrl_chamber->Enable();
        m_switch_speed->Enable();
        m_switch_speed->SetValue(true);
        m_switch_lamp->Enable();
        /*m_switch_nozzle_fan->Enable();
        m_switch_printing_fan->Enable();
        m_switch_cham_fan->Enable();*/
        m_switch_fan->Enable();
    }
}

void StatusPanel::update_temp_ctrl(MachineObject *obj)
{
    if (!obj) return;

    DevBed* bed = obj->GetBed();
    int bed_cur_temp = bed->GetBedTemp();
    int bed_target_temp = bed->GetBedTempTarget();
    m_tempCtrl_bed->SetCurrTemp((int) bed_cur_temp);

    auto limit = obj->get_bed_temperature_limit();
    if (obj->bed_temp_range.size() > 1) {
        limit = obj->bed_temp_range[1];
    }
    m_tempCtrl_bed->SetMaxTemp(limit);

    if (obj->nozzle_temp_range.size() >= 2) {
        m_tempCtrl_nozzle->SetMinTemp(obj->nozzle_temp_range[0]);
        m_tempCtrl_nozzle->SetMaxTemp(obj->nozzle_temp_range[1]);

        m_tempCtrl_nozzle_deputy->SetMinTemp(obj->nozzle_temp_range[0]);
        m_tempCtrl_nozzle_deputy->SetMaxTemp(obj->nozzle_temp_range[1]);
    }

    // update temprature if not input temp target
    if (m_temp_bed_timeout > 0) {
        m_temp_bed_timeout--;
    } else {
        if (!bed_temp_input) { m_tempCtrl_bed->SetTagTemp((int) bed_target_temp); }
    }

    if ((bed_target_temp - bed_cur_temp) >= TEMP_THRESHOLD_VAL) {
        m_tempCtrl_bed->SetIconActive();
    } else {
        m_tempCtrl_bed->SetIconNormal();
    }

    bool to_update_layout = false;
    int nozzle_num = obj->GetExtderSystem()->GetTotalExtderCount();
    if (nozzle_num == 1)
    {
        m_tempCtrl_nozzle->SetCurrTemp(obj->GetExtderSystem()->GetNozzleTempCurrent(MAIN_EXTRUDER_ID));
        m_tempCtrl_nozzle->SetCurrType(TEMP_OF_NORMAL_TYPE);

        m_tempCtrl_nozzle_deputy->SetCurrType(TEMP_OF_NORMAL_TYPE);
        m_tempCtrl_nozzle_deputy->SetLabel(TEMP_BLANK_STR);
        m_tempCtrl_nozzle_deputy->Hide();

        if (m_tempCtrl_nozzle->GetMinSize() != TEMP_CTRL_MIN_SIZE_ALIGN_ONE_ICON)
        {
            to_update_layout = true;
            m_tempCtrl_nozzle->SetMinSize(TEMP_CTRL_MIN_SIZE_ALIGN_ONE_ICON);
        }
    }
    else if (nozzle_num == 2)
    {
        m_tempCtrl_nozzle->SetCurrType(TEMP_OF_MAIN_NOZZLE_TYPE);
        m_tempCtrl_nozzle->SetCurrTemp(obj->GetExtderSystem()->GetNozzleTempCurrent(MAIN_EXTRUDER_ID));
        m_tempCtrl_nozzle->Show();

        m_tempCtrl_nozzle_deputy->SetCurrType(TEMP_OF_DEPUTY_NOZZLE_TYPE);
        m_tempCtrl_nozzle_deputy->SetCurrTemp(obj->GetExtderSystem()->GetNozzleTempCurrent(DEPUTY_EXTRUDER_ID));
        m_tempCtrl_nozzle_deputy->Show();

        if (m_tempCtrl_nozzle->GetMinSize() != TEMP_CTRL_MIN_SIZE_ALIGN_TWO_ICON)
        {
            to_update_layout = true;
            m_tempCtrl_nozzle->SetMinSize(TEMP_CTRL_MIN_SIZE_ALIGN_TWO_ICON);
        }
    }

    if (m_temp_nozzle_timeout > 0) {
        m_temp_nozzle_timeout--;
    } else {
        if (!nozzle_temp_input) {
            auto main_extder = obj->GetExtderSystem()->GetExtderById(MAIN_EXTRUDER_ID);
            if (main_extder)
            {
                m_tempCtrl_nozzle->SetTagTemp(main_extder->GetTargetTemp());
                m_tempCtrl_nozzle->SetCurrTemp((int)main_extder->GetCurrentTemp());
                if (main_extder->GetTargetTemp() - main_extder->GetCurrentTemp() > TEMP_THRESHOLD_VAL)
                {
                    m_tempCtrl_nozzle->SetIconActive();
                }
                else
                {
                    m_tempCtrl_nozzle->SetIconNormal();
                }
            }
        }
    }

    if (m_temp_nozzle_deputy_timeout > 0) {
        m_temp_nozzle_deputy_timeout--;
    }
    else {
        if (!nozzle_temp_input && nozzle_num >= 2) {
            auto deputy_extder = obj->GetExtderSystem()->GetExtderById(DEPUTY_EXTRUDER_ID);
            if (deputy_extder)
            {
                m_tempCtrl_nozzle_deputy->SetTagTemp(deputy_extder->GetTargetTemp());
                m_tempCtrl_nozzle_deputy->SetCurrTemp((int)deputy_extder->GetCurrentTemp());
                if (deputy_extder->GetTargetTemp() - deputy_extder->GetCurrentTemp() > TEMP_THRESHOLD_VAL)
                {
                    m_tempCtrl_nozzle_deputy->SetIconActive();
                }
                else
                {
                    m_tempCtrl_nozzle_deputy->SetIconNormal();
                }
            }
        }
    }

    // support current temp for chamber
    if (obj->get_printer_series() == PrinterSeries::SERIES_X1)
    {
        m_tempCtrl_chamber->SetCurrTemp(obj->chamber_temp);
    }
    else
    {
        m_tempCtrl_chamber->SetCurrTemp(TEMP_BLANK_STR);
    }

    // support edit chamber temp
    DevConfig* config = obj->GetConfig();
    if (config->SupportChamberEdit())
    {
        m_tempCtrl_chamber->SetReadOnly(false);
        m_tempCtrl_chamber->Enable();
        m_tempCtrl_chamber->SetMinTemp(config->GetChamberTempEditMin());
        m_tempCtrl_chamber->SetMaxTemp(config->GetChamberTempEditMax());
        m_tempCtrl_chamber->AddTemp(0); // zero is default temp
        wxCursor cursor(wxCURSOR_IBEAM);
        m_tempCtrl_chamber->GetTextCtrl()->SetCursor(cursor);

        if (m_temp_chamber_timeout > 0)
        {
            m_temp_chamber_timeout--;
        }
        else
        {
            /*update temprature if not input temp target*/
            if (!cham_temp_input) { m_tempCtrl_chamber->SetTagTemp(obj->chamber_temp_target); }
        }
    }
    else
    {
        m_tempCtrl_chamber->SetReadOnly(true);
        m_tempCtrl_chamber->SetTagTemp(TEMP_BLANK_STR);

        wxCursor cursor(wxCURSOR_ARROW);
        m_tempCtrl_chamber->GetTextCtrl()->SetCursor(cursor);
    }

    if ((obj->chamber_temp_target - obj->chamber_temp) >= TEMP_THRESHOLD_VAL) {
        m_tempCtrl_chamber->SetIconActive();
    }
    else {
        m_tempCtrl_chamber->SetIconNormal();
    }

    if (to_update_layout)
    {
        this->Layout();
    }
}

void StatusPanel::update_misc_ctrl(MachineObject *obj)
{
    auto get_extder_shown_state = [](bool ext_has_filament) -> ExtruderState
    {
        // no data to distinguish ExtruderState::UNLOAD or LOAD, use LOAD png as default
        return ext_has_filament ? ExtruderState::FILLED_LOAD : ExtruderState::EMPTY_LOAD;
    };

    if (!obj) return;

    /*extder*/
    auto extder_system = obj->GetExtderSystem();
    m_nozzle_num     = extder_system->GetTotalExtderCount();
    int select_index = m_nozzle_num - 1;

    if (m_nozzle_num >= 2) {
        m_extruder_book->SetSelection(m_nozzle_num);

        /*style*/
        m_nozzle_btn_panel->Show();
        m_extruderImage[select_index]->setExtruderCount(m_nozzle_num);

        if (obj->GetExtderSystem()->GetTotalExtderSize() > 1)
        {
            m_extruderImage[select_index]->update(get_extder_shown_state(obj->GetExtderSystem()->HasFilamentInExt(0)),
                                                  get_extder_shown_state(obj->GetExtderSystem()->HasFilamentInExt(1)));
        }

        /*current*/
        /*update when extder position changed or the machine changed*/
        if (obj->GetExtderSystem()->GetCurrentExtderId() == 0xf)
        {
            m_extruderImage[select_index]->setExtruderUsed("");
            m_nozzle_btn_panel->updateState("");
        }
        else if (obj->GetExtderSystem()->GetCurrentExtderId() == MAIN_EXTRUDER_ID)
        {
            m_extruderImage[select_index]->setExtruderUsed("right");
            m_nozzle_btn_panel->updateState("right");
        }
        else if (obj->GetExtderSystem()->GetCurrentExtderId() == DEPUTY_EXTRUDER_ID)
        {
            m_extruderImage[select_index]->setExtruderUsed("left");
            m_nozzle_btn_panel->updateState("left");
        }

        m_nozzle_btn_panel->SetClientData(obj);

        /*enable status*/
        /* Can do switch while printing pause STUDIO-9789*/
        if ((obj->is_in_printing() && !obj->is_in_printing_pause()) ||
            obj->ams_status_main == AMS_STATUS_MAIN_FILAMENT_CHANGE ||
            obj->targ_nozzle_id_from_pc != INVALID_EXTRUDER_ID)
        {
            m_nozzle_btn_panel->Disable();
        }
        else
        {
            m_nozzle_btn_panel->Enable();
        }
    } else {
        m_nozzle_btn_panel->Hide();
        m_extruder_book->SetSelection(m_nozzle_num);
        m_extruderImage[select_index]->setExtruderCount(m_nozzle_num);

        if (extder_system->GetTotalExtderSize() > 0)
        {
            ExtruderState shown_state = get_extder_shown_state(extder_system->HasFilamentInExt(0));
            m_extruderImage[select_index]->update(shown_state);
        }
    }

    /*switch extder*/
    m_extruder_switching_status->updateBy(obj);
    m_extruder_label->Show(!m_extruder_switching_status->has_content_shown());/*hide the label if there are shown infos from m_extruder_switching_status*/

    /*other*/
    if (obj->is_core_xy()) {
        m_staticText_z_tip->SetLabel(_L("Bed"));
    } else {
        m_staticText_z_tip->SetLabel("Z");
    }

    // update extruder icon
    update_extruder_status(obj);

    if (obj->is_fdm_type()) {
        if (!m_fan_panel->IsShown())
            m_fan_panel->Show();
        bool is_suppt_part_fun = true;
        bool is_suppt_aux_fun  = obj->GetFan()->GetSupportAuxFanData();
        bool is_suppt_cham_fun = obj->GetFan()->GetSupportChamberFan();
        if (m_fan_control_popup) { m_fan_control_popup->update_fan_data(obj); }
    } else {
        if (m_fan_panel->IsShown()) {
            m_fan_panel->Hide();
        }
        if (m_fan_control_popup && m_fan_control_popup->IsShown())
            m_fan_control_popup->Hide();
    }

    obj->is_series_o() ? m_switch_fan->UseTextAirCondition() : m_switch_fan->UseTextFan();

    //update cham fan

    /*other*/
    bool light_on = obj->GetLamp()->IsChamberLightOn();
    BOOST_LOG_TRIVIAL(trace) << "light: " << (light_on ? "on" : "off");
    if (m_switch_lamp_timeout > 0)
        m_switch_lamp_timeout--;
    else {
        m_switch_lamp->SetValue(light_on);
        /*wxString label = light_on ? "On" : "Off";
        m_switch_lamp->SetLabels(label, label);*/
    }

    if (speed_lvl_timeout > 0)
        speed_lvl_timeout--;
    else {
        // update speed
        this->speed_lvl = obj->GetPrintingSpeedLevel();
            wxString text_speed = wxString::Format("%d%%", obj->printing_speed_mag);
            m_switch_speed->SetLabels(text_speed, text_speed);
    }
}

void StatusPanel::update_extruder_status(MachineObject* obj)
{
    if (!obj) return;
}

void StatusPanel::update_ams(MachineObject *obj)
{
    // update obj in sub dlg
    if (m_ams_setting_dlg && m_ams_setting_dlg->IsShown()) {
        m_ams_setting_dlg->UpdateByObj(obj);
    }
    if (m_filament_setting_dlg) { m_filament_setting_dlg->obj = obj; }

    if (obj && (obj->last_cali_version != obj->cali_version) && (obj->is_lan_mode_printer() || obj->is_security_control_ready())) {
        obj->last_cali_version = obj->cali_version;
        PACalibExtruderInfo cali_info;
        cali_info.nozzle_diameter        = obj->GetExtderSystem()->GetNozzleDiameter(0);
        cali_info.use_extruder_id        = false;
        cali_info.use_nozzle_volume_type = false;
        CalibUtils::emit_get_PA_calib_infos(cali_info);
    }

    bool     is_support_virtual_tray    = obj->ams_support_virtual_tray;
    bool     is_support_filament_backup = obj->is_support_filament_backup;

    if (obj && (obj->is_lan_mode_printer() || obj->is_security_control_ready())) {
        obj->check_ams_filament_valid();
    }

    AMSModel ams_mode = AMSModel::GENERIC_AMS;
    if ((obj->is_enable_np || obj->is_enable_ams_np) && obj->GetFilaSystem()->GetAmsList().size() > 0) {
        ams_mode = AMSModel(obj->GetFilaSystem()->GetAmsList().begin()->second->GetAmsType());
    } else if (obj->get_printer_ams_type() == "f1") {
        ams_mode = AMSModel::AMS_LITE; // STUDIO-14066
    }

    if (!obj || !obj->is_connected()) {
        last_tray_exist_bits  = -1;
        last_ams_exist_bits   = -1;
        last_tray_is_bbl_bits = -1;
        last_read_done_bits   = -1;
        last_reading_bits     = -1;
        last_ams_version      = -1;
        BOOST_LOG_TRIVIAL(trace) << "machine object" << obj->get_dev_name() << " was disconnected, set show_ams_group is false";

        m_ams_control->SetAmsModel(AMSModel::EXT_AMS, ams_mode);
        show_ams_group(false);
        show_filament_load_group(false);
        m_ams_control->show_auto_refill(false);
    } else {
        m_ams_control->SetAmsModel(ams_mode, ams_mode);
        m_filament_step->SetAmsModel(ams_mode, ams_mode);
        show_ams_group(true);
        //show_filament_load_group(true);

        if (obj->GetFilaSystem()->GetAmsList().empty() || obj->ams_exist_bits == 0) {
            m_ams_control->show_auto_refill(false);
        } else {
            m_ams_control->show_auto_refill(true);
        }
    }

    //if (is_support_virtual_tray) m_ams_control->update_vams_kn_value(obj->vt_slot[0], obj);
    if (m_filament_setting_dlg) m_filament_setting_dlg->update();


    std::vector<AMSinfo> ams_info;
    const auto& ams_list = obj->GetFilaSystem()->GetAmsList();
    for (auto ams = ams_list.begin(); ams != ams_list.end(); ams++) {
        AMSinfo info;
        info.ams_id = ams->first;
        if (ams->second->IsExist() && info.parse_ams_info(obj, ams->second, obj->GetFilaSystem()->IsDetectRemainEnabled(), obj->is_support_ams_humidity)) {
            ams_info.push_back(info);
        }
    }

    std::vector<AMSinfo> ext_info;
    ext_info.clear();
    for (auto slot : obj->vt_slot) {
        AMSinfo info;
        info.parse_ext_info(obj, slot);
        if (ams_mode == AMSModel::AMS_LITE) info.ext_type = AMSModelOriginType::LITE_EXT;
        ext_info.push_back(info);
    }

    // must select a current can
    m_ams_control->UpdateAms(obj->get_printer_series_str(), obj->printer_type, ams_info, ext_info, *obj->GetExtderSystem(), obj->get_dev_id(), false);

    last_tray_exist_bits  = obj->tray_exist_bits;
    last_ams_exist_bits   = obj->ams_exist_bits;
    last_tray_is_bbl_bits = obj->tray_is_bbl_bits;
    last_read_done_bits   = obj->tray_read_done_bits;
    last_reading_bits     = obj->tray_reading_bits;
    last_ams_version      = obj->ams_version;

    std::string curr_ams_id = m_ams_control->GetCurentAms();
    std::string curr_can_id = m_ams_control->GetCurrentCan(curr_ams_id);
    bool        is_vt_tray  = false;
    if (obj->GetExtderSystem()->GetCurrentAmsId() == std::to_string(VIRTUAL_TRAY_MAIN_ID)) is_vt_tray = true;

    // set segment 1, 2
    //if (!obj->is_enable_np) {
    //    if (obj->m_tray_now == std::to_string(255) || obj->m_tray_now == std::to_string(254)) {
    //        m_ams_control->SetAmsStep(obj->m_extder_data.extders[MAIN_NOZZLE_ID].snow.ams_id, obj->m_extder_data.extders[MAIN_NOZZLE_ID].snow.slot_id, AMSPassRoadType::AMS_ROAD_TYPE_UNLOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
    //    } else {
    //        /*if (obj->m_tray_now != "255" && obj->is_filament_at_extruder() && !obj->m_tray_id.empty()) {
    //            m_ams_control->SetAmsStep(obj->m_extder_data.extders[MAIN_NOZZLE_ID].snow.ams_id, obj->m_extder_data.extders[MAIN_NOZZLE_ID].snow.slot_id,
    //                                      AMSPassRoadType::AMS_ROAD_TYPE_LOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP2);
    //        } else if (obj->m_tray_now != "255") {
    //            m_ams_control->SetAmsStep(obj->m_extder_data.extders[MAIN_NOZZLE_ID].snow.ams_id, obj->m_extder_data.extders[MAIN_NOZZLE_ID].snow.slot_id,
    //                                      AMSPassRoadType::AMS_ROAD_TYPE_LOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP1);
    //        } else {
    //            m_ams_control->SetAmsStep(obj->m_extder_data.extders[MAIN_NOZZLE_ID].snow.ams_id, obj->m_extder_data.extders[MAIN_NOZZLE_ID].snow.slot_id,
    //                                      AMSPassRoadType::AMS_ROAD_TYPE_UNLOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
    //        }*/
    //        if () {

    //        }
    //    }

    //    m_ams_control->SetExtruder(obj->is_filament_at_extruder(), obj->m_extder_data.extders[MAIN_NOZZLE_ID].snow.ams_id, obj->m_extder_data.extders[MAIN_NOZZLE_ID].snow.slot_id);
    //} else {
        /*right*/
    if (obj->GetExtderSystem()->GetTotalExtderCount() > 0) {
        auto ext = obj->GetExtderSystem()->GetExtderById(MAIN_EXTRUDER_ID);
        if (ext->HasFilamentInExt()) {
            if (ext->GetSlotNow().ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID) || ext->GetSlotNow().ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
                m_ams_control->SetAmsStep(ext->GetSlotNow().ams_id, "0", AMSPassRoadType::AMS_ROAD_TYPE_LOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP3);
            } else {
                m_ams_control->SetAmsStep(ext->GetSlotNow().ams_id, ext->GetSlotNow().slot_id, AMSPassRoadType::AMS_ROAD_TYPE_LOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP2);
            }
        } else {
            m_ams_control->SetAmsStep(ext->GetSlotNow().ams_id, ext->GetSlotNow().slot_id, AMSPassRoadType::AMS_ROAD_TYPE_UNLOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
        }
        m_ams_control->SetExtruder(ext->HasFilamentInExt(), MAIN_EXTRUDER_ID, ext->GetSlotNow().ams_id, ext->GetSlotNow().slot_id);
    }

    /*left*/
    if (obj->GetExtderSystem()->GetTotalExtderCount() > 1) {
        auto ext = obj->GetExtderSystem()->GetExtderById(DEPUTY_EXTRUDER_ID);
        if (ext->HasFilamentInExt()) {
            if (ext->GetSlotNow().ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID) || ext->GetSlotNow().ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
                m_ams_control->SetAmsStep(ext->GetSlotNow().ams_id, "0", AMSPassRoadType::AMS_ROAD_TYPE_LOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP3);
            } else {
                m_ams_control->SetAmsStep(ext->GetSlotNow().ams_id, ext->GetSlotNow().slot_id, AMSPassRoadType::AMS_ROAD_TYPE_LOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP2);
            }
        } else {
            m_ams_control->SetAmsStep(ext->GetSlotNow().ams_id, ext->GetSlotNow().slot_id, AMSPassRoadType::AMS_ROAD_TYPE_UNLOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
        }
        m_ams_control->SetExtruder(ext->HasFilamentInExt(), DEPUTY_EXTRUDER_ID, ext->GetSlotNow().ams_id, ext->GetSlotNow().slot_id);
    }

    update_filament_loading_panel(obj);


    const auto& amslist = obj->GetFilaSystem()->GetAmsList();
    for (auto ams_it = amslist.begin(); ams_it != amslist.end(); ams_it++) {
        std::string ams_id = ams_it->first;
        try {
            int ams_id_int = atoi(ams_id.c_str());
            for (auto tray_it = ams_it->second->GetTrays().begin(); tray_it != ams_it->second->GetTrays().end(); tray_it++) {
                std::string tray_id     = tray_it->first;
                int         tray_id_int = atoi(tray_id.c_str());
                // new protocol
                if (ams_id_int < 128) {
                    if ((obj->tray_reading_bits & (1 << (ams_id_int * 4 + tray_id_int))) != 0) {
                        m_ams_control->PlayRridLoading(ams_id, tray_id);
                    } else {
                        m_ams_control->StopRridLoading(ams_id, tray_id);
                    }
                } else {
                    int check_flag = (1 << (16 + ams_id_int - 128));
                    if ((obj->tray_reading_bits & check_flag) != 0) {
                        m_ams_control->PlayRridLoading(ams_id, tray_id);
                    } else {
                        m_ams_control->StopRridLoading(ams_id, tray_id);
                    }
                }
            }
        } catch (...) {}
    }

    update_ams_control_state(curr_ams_id, curr_can_id);
}


void StatusPanel::update_ams_control_state(std::string ams_id, std::string slot_id)
{
    wxString load_error_info, unload_error_info;

    if (obj->is_in_printing() && !obj->can_resume()) {
        load_error_info = _L("The printer is busy with another print job.");
        unload_error_info = _L("The printer is busy with another print job.");
    } else if (obj->can_resume() && !devPrinterUtil::IsVirtualSlot(ams_id)) {
        load_error_info = _L("When printing is paused, filament loading and unloading are only supported for external slots.");
        unload_error_info = _L("When printing is paused, filament loading and unloading are only supported for external slots.");
    } else {
        /*switch now*/
        bool in_switch_filament = false;

        if (obj->is_enable_np) {
            if (obj->GetExtderSystem()->IsBusyLoading()) { in_switch_filament = true; }
        } else if (obj->ams_status_main == AMS_STATUS_MAIN_FILAMENT_CHANGE) {
            in_switch_filament = true;
        }

        if (in_switch_filament) {
            load_error_info = _L("Current extruder is busy changing filament.");
            unload_error_info = _L("Current extruder is busy changing filament.");
        }

        if (ams_id.empty() || slot_id.empty()) {
            load_error_info = _L("Choose an AMS slot then press \"Load\" or \"Unload\" button to automatically load or unload filaments.");
            unload_error_info = _L("Choose an AMS slot then press \"Load\" or \"Unload\" button to automatically load or unload filaments.");
        } else if (ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID) || ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
            for (auto ext : obj->GetExtderSystem()->GetExtruders()) {
                if (ext.GetSlotNow().ams_id == ams_id && ext.GetSlotNow().slot_id == slot_id)
                {
                    load_error_info = _L("Current slot has already been loaded.");
                }
            }
        } else {
            for (auto ext : obj->GetExtderSystem()->GetExtruders()) {
                if (ext.GetSlotNow().ams_id == ams_id && ext.GetSlotNow().slot_id == slot_id)
                {
                    load_error_info = _L("Current slot has already been loaded.");
                }
            }

            /*empty*/
            auto ams_item = obj->GetFilaSystem()->GetAmsById(ams_id);
            if (!ams_item)
            {
                load_error_info = _L("Choose an AMS slot then press \"Load\" or \"Unload\" button to automatically load or unload filaments.");
            }
            else
            {
                auto tray_item = ams_item->GetTray(slot_id);
                if (!tray_item)
                {
                    load_error_info = _L("Choose an AMS slot then press \"Load\" or \"Unload\" button to automatically load or unload filaments.");
                }
                else if (!tray_item->is_exists)
                {
                    load_error_info = _L("The selected slot is empty.");
                }
            }
        }
    }

    m_ams_control->EnableLoadFilamentBtn(load_error_info.empty(), ams_id, slot_id, load_error_info);
    m_ams_control->EnableUnLoadFilamentBtn(unload_error_info.empty(), ams_id, slot_id,unload_error_info);
}

void StatusPanel::update_cali(MachineObject *obj)
{
    if (!obj) return;

    // disable calibration button in 2D
    if (!obj->is_fdm_type()) {
        m_calibration_btn->SetToolTip(_L("Printer 2D mode does not support 3D calibration"));
        m_calibration_btn->SetLabel(_L("Calibration"));
        m_calibration_btn->Disable();
        return;
    } else if (!m_calibration_btn->IsEnabled()) {
        m_calibration_btn->SetToolTip(wxEmptyString);
        m_calibration_btn->Enable();
    }

    if (obj->is_calibration_running()) {
        m_calibration_btn->SetLabel(_L("Calibrating"));
        if (calibration_dlg && calibration_dlg->IsShown()) {
            m_calibration_btn->Disable();
        } else {
            m_calibration_btn->Enable();
        }
    } else {
        // IDLE
        m_calibration_btn->SetLabel(_L("Calibration"));
        // disable in printing
        if (obj->is_in_printing()) {
            m_calibration_btn->Disable();
        } else {
            m_calibration_btn->Enable();
        }
    }
}

void StatusPanel::update_calib_bitmap() {
    m_current_print_mode = PrintingTaskType::NOT_CLEAR;  //printing task might be changed when updating.
    if (calib_bitmap != nullptr) {
        delete calib_bitmap;
        calib_bitmap = nullptr;
    }
}

void StatusPanel::update_basic_print_data(bool def)
{
    if (def) {
        if (!obj) return;
        if (!obj->slice_info) return;
        wxString prediction = wxString::Format("%s", get_bbl_time_dhms(obj->slice_info->prediction));
        wxString weight = wxString::Format("%.2fg", obj->slice_info->weight);

        m_project_task_panel->show_priting_use_info(true, prediction, weight);
    }
    else {
        m_project_task_panel->show_priting_use_info(false, "0m", "0g");
    }
}

void StatusPanel::update_model_info()
{
    auto get_subtask_fn = [this](BBLModelTask* subtask) {
        CallAfter([this, subtask]() { 
            if (obj && obj->subtask_id_ == subtask->task_id) {
                obj->set_modeltask(subtask);
            }
        });
    };

     
    if (wxGetApp().getAgent() && obj) {
        BBLSubTask* curr_task = obj->get_subtask();
        if (curr_task) {
            BBLModelTask* curr_model_task = obj->get_modeltask();
            if (!curr_model_task && !request_model_info_flag) {
                curr_model_task = new BBLModelTask();
                curr_model_task->task_id = curr_task->task_id;
                request_model_info_flag = true;
                if (!curr_model_task->task_id.empty() && curr_model_task->task_id.compare("0") != 0) {
                    wxGetApp().getAgent()->get_subtask(curr_model_task,  get_subtask_fn);
                }
            }
        }
    }
}

void StatusPanel::update_subtask(MachineObject *obj)
{
    if (!obj) return;
    if (m_current_print_mode != PRINGINT) {
        if (calib_bitmap == nullptr) {
            m_calib_mode = get_obj_calibration_mode(obj, m_calib_method, cali_stage);
            if (m_calib_mode == CalibMode::Calib_None)
                m_current_print_mode = PRINGINT;
            // the printing task is calibrattion, not normal printing.
            else if (m_calib_mode != CalibMode::Calib_None) {
                m_current_print_mode = CALIBRATION;
                auto get_bitmap = [](wxString& png_path, int width, int height) {
                    wxImage image(width, height);
                    image.LoadFile(png_path, wxBITMAP_TYPE_PNG);
                    image = image.Scale(width, height, wxIMAGE_QUALITY_NORMAL);
                    return wxBitmap(image);
                };
                wxString png_path = "";
                int width = m_project_task_panel->get_bitmap_thumbnail()->GetSize().x;
                int height = m_project_task_panel->get_bitmap_thumbnail()->GetSize().y;
                if (m_calib_method == CALI_METHOD_AUTO || m_calib_method == CalibrationMethod::CALI_METHOD_NEW_AUTO) {
                    std::string image_name = obj->get_auto_pa_cali_thumbnail_img_str();
                    if (m_calib_mode == CalibMode::Calib_PA_Line) {
                        if (obj->is_multi_extruders()) {
                            int cur_ext_id = obj->GetExtderSystem()->GetCurrentExtderId();
                            if (cur_ext_id == 0) {
                                image_name += "_right";
                            }
                            else {
                                image_name += "_left";
                            }
                        }
                        png_path = (boost::format("%1%/images/%2%.png") % resources_dir() % image_name).str();
                    }
                    else if (m_calib_mode == CalibMode::Calib_Flow_Rate) {
                        png_path = (boost::format("%1%/images/flow_rate_calibration_auto.png") % resources_dir()).str();
                    }

                }
                else if (m_calib_method == CALI_METHOD_MANUAL) {
                    if (m_calib_mode== CalibMode::Calib_PA_Line) {
                        if (cali_stage == 0) {  // Line mode
                            png_path = (boost::format("%1%/images/fd_calibration_manual.png") % resources_dir()).str();
                        }
                        else if (cali_stage == 1) { // Pattern mode
                            png_path = (boost::format("%1%/images/fd_pattern_manual_device.png") % resources_dir()).str();
                        }
                    }
                }
                if (png_path != "") {
                    calib_bitmap = new wxBitmap;
                    *calib_bitmap = get_bitmap(png_path, width, height);
                }
            }
        }
        if (calib_bitmap != nullptr)
            m_project_task_panel->set_thumbnail_img(*calib_bitmap, "");
    }

    m_project_task_panel->show_layers_num(obj->is_support_layer_num);

    update_model_info();
    update_partskip_button(obj);
    update_printer_parts_options(obj);

    if (obj->is_system_printing() || obj->is_in_calibration()) {
        reset_printing_values();
    } else if (obj->is_in_printing() || obj->print_status == "FINISH") {
        update_partskip_subtask(obj);

        if (obj->is_in_prepare() || obj->print_status == "SLICING") {
            m_project_task_panel->market_scoring_hide();
            m_project_task_panel->get_request_failed_panel()->Hide();
            m_project_task_panel->enable_partskip_button(nullptr, false);
            m_project_task_panel->enable_abort_button(false);
            m_project_task_panel->enable_pause_resume_button(false, "pause_disable");
            wxString prepare_text;
            bool show_percent = true;

            if (obj->is_in_prepare()) {
                prepare_text = wxString::Format(_L("Downloading..."));
            }
            else if (obj->print_status == "SLICING") {
                if (obj->queue_number <= 0) {
                    prepare_text = wxString::Format(_L("Cloud Slicing..."));
                } else {
                    prepare_text = wxString::Format(_L("In Cloud Slicing Queue, there are %s tasks ahead."), std::to_string(obj->queue_number));
                    show_percent = false;
                }
            } else
                prepare_text = wxString::Format(_L("Downloading..."));

            if (obj->gcode_file_prepare_percent >= 0 && obj->gcode_file_prepare_percent <= 100 && show_percent)
                prepare_text += wxString::Format("(%d%%)", obj->gcode_file_prepare_percent);

            m_project_task_panel->update_stage_value_with_machine(prepare_text, 0, obj);
            m_project_task_panel->update_progress_percent(NA_STR, wxEmptyString);
            m_project_task_panel->update_left_time(NA_STR);
            m_project_task_panel->update_layers_num(true, wxString::Format(_L("Layer: %s"), NA_STR));
            m_project_task_panel->update_subtask_name(wxString::Format("%s", GUI::from_u8(obj->subtask_name)));


            if (obj->get_modeltask() && obj->get_modeltask()->design_id > 0) {
                m_project_task_panel->show_profile_info(true, wxString::FromUTF8(obj->get_modeltask()->profile_name));
            }
            else {
                m_project_task_panel->show_profile_info(false);
            }
            update_basic_print_data(false);
        } else {
            if (obj->can_resume()) {
                m_project_task_panel->enable_pause_resume_button(true, "resume");
            } else {
                 m_project_task_panel->enable_pause_resume_button(true, "pause");
            }
            m_project_task_panel->enable_partskip_button(obj, true);
            // update printing stage
            m_project_task_panel->update_left_time(obj->mc_left_time);
            if (obj->subtask_) {
                m_project_task_panel->update_stage_value_with_machine(obj->get_curr_stage(), obj->subtask_->task_progress, obj);
                m_project_task_panel->update_progress_percent(wxString::Format("%d", obj->subtask_->task_progress), "%");
                m_project_task_panel->update_layers_num(true, wxString::Format(_L("Layer: %d/%d"), obj->curr_layer, obj->total_layers));

            } else {
                m_project_task_panel->update_stage_value_with_machine(obj->get_curr_stage(), 0, obj);
                m_project_task_panel->update_progress_percent(NA_STR, wxEmptyString);
                m_project_task_panel->update_layers_num(true, wxString::Format(_L("Layer: %s"), NA_STR));
            }

            if (obj->is_printing_finished()) {
                obj->update_model_task();
                m_project_task_panel->enable_abort_button(false);
                m_project_task_panel->enable_partskip_button(nullptr, false);
                m_project_task_panel->enable_pause_resume_button(false, "resume_disable");
                // is makeworld subtask
                if (wxGetApp().has_model_mall() && obj->is_makeworld_subtask()) {
                    // has model mall rating result
                    if (obj && obj->rating_info && obj->rating_info->request_successful) {
                        m_project_task_panel->get_request_failed_panel()->Hide();
                        BOOST_LOG_TRIVIAL(info) << "model mall result request successful";
                        // has start count
                        if (!m_project_task_panel->get_star_count_dirty()) {
                            if (obj->rating_info->start_count > 0) {
                                m_project_task_panel->set_star_count(obj->rating_info->start_count);
                                m_project_task_panel->set_star_count_dirty(true);
                                BOOST_LOG_TRIVIAL(info) << "Initialize scores";
                                m_project_task_panel->get_market_scoring_button()->Enable(true);
                                m_project_task_panel->set_has_reted_text(true);
                            } else {
                                m_project_task_panel->set_star_count(0);
                                m_project_task_panel->set_star_count_dirty(false);
                                m_project_task_panel->get_market_scoring_button()->Enable(false);
                                m_project_task_panel->set_has_reted_text(false);
                            }
                        }
                        m_project_task_panel->market_scoring_show();
                    } else if (obj && obj->rating_info && !obj->rating_info->request_successful) {
                        BOOST_LOG_TRIVIAL(info) << "model mall result request failed";
                        if (403 != obj->rating_info->http_code) {
                            BOOST_LOG_TRIVIAL(info) << "Request need retry";
                            m_project_task_panel->get_market_retry_buttom()->Enable(!obj->get_model_mall_result_need_retry);
                            m_project_task_panel->get_request_failed_panel()->Show();
                        } else {
                            BOOST_LOG_TRIVIAL(info) << "Request rejected";
                        }
                    }
                } else {
                    m_project_task_panel->market_scoring_hide();
                }
            } else { // model printing is not finished, hide scoring page
                m_project_task_panel->enable_abort_button(true);
                m_project_task_panel->market_scoring_hide();
                m_project_task_panel->get_request_failed_panel()->Hide();
            }
        }

        m_project_task_panel->update_subtask_name(wxString::Format("%s", GUI::from_u8(obj->subtask_name)));

        if (obj->get_modeltask() && obj->get_modeltask()->design_id > 0) {
            m_project_task_panel->show_profile_info(true, wxString::FromUTF8(obj->get_modeltask()->profile_name));
        }
        else {
            m_project_task_panel->show_profile_info(false);
        }

        //update thumbnail
        if (obj->is_sdcard_printing()) {
            update_basic_print_data(false);
            update_sdcard_subtask(obj);
        } else {
            update_basic_print_data(true);
            update_cloud_subtask(obj);
        }
    } else {
        reset_printing_values();
    }
}

void StatusPanel::update_partskip_subtask(MachineObject *obj){
    if (!obj) return;
    if (!obj->subtask_) return;

    auto partskip_button = m_project_task_panel->get_partskip_button();
    if (partskip_button) { 
        int part_cnt = 0;
        if(m_project_task_panel->get_part_skipped_dirty() > 0){
            m_project_task_panel->set_part_skipped_dirty(m_project_task_panel->get_part_skipped_dirty() - 1);
            part_cnt = m_project_task_panel->get_part_skipped_count();
            BOOST_LOG_TRIVIAL(info) << "part skip: stop recv printer dirty data.";
        }else{
            part_cnt = obj->m_partskip_ids.size();
            BOOST_LOG_TRIVIAL(info) << "part skip: recv printer normal data.";
        }
        if (part_cnt > 0)
            partskip_button->SetLabel(wxString::Format("(%d)", part_cnt));
        else 
            partskip_button->SetLabel("");
    }

    if(m_partskip_dlg && m_partskip_dlg->IsShown()) {
        m_partskip_dlg->UpdatePartsStateFromPrinter(obj);
    }
}

void StatusPanel::update_cloud_subtask(MachineObject *obj)
{
    if (!obj) return;
    if (!obj->subtask_) return;

    if (is_task_changed(obj)) {
        obj->set_modeltask(nullptr);
        obj->free_slice_info();
        reset_printing_values();
        BOOST_LOG_TRIVIAL(info) << "monitor: change to sub task id = " << obj->subtask_->task_id;
        if (web_request.IsOk() && web_request.GetState() == wxWebRequest::State_Active) {
            BOOST_LOG_TRIVIAL(info) << "web_request: cancelled";
            web_request.Cancel();
        }
        m_start_loading_thumbnail = true;
    }

    if (m_start_loading_thumbnail) {
        update_calib_bitmap();
        if (obj->slice_info) {
            m_request_url = wxString(obj->slice_info->thumbnail_url);
            if (!m_request_url.IsEmpty()) {
                wxImage                               img;
                std::map<wxString, wxImage>::iterator it = img_list.find(m_request_url);
                if (it != img_list.end()) {
                    if (m_current_print_mode != PrintingTaskType::CALIBRATION  ||(m_calib_mode == CalibMode::Calib_Flow_Rate && m_calib_method == CalibrationMethod::CALI_METHOD_MANUAL)) {
                        img = it->second;
                        wxImage resize_img = img.Scale(m_project_task_panel->get_bitmap_thumbnail()->GetSize().x, m_project_task_panel->get_bitmap_thumbnail()->GetSize().y);
                        m_project_task_panel->set_thumbnail_img(resize_img, "");
                        m_project_task_panel->set_brightness_value(get_brightness_value(resize_img));
                    }
                    if (this->obj) {
                        m_project_task_panel->set_plate_index(obj->m_plate_index);
                    } else {
                        m_project_task_panel->set_plate_index(-1);
                    }
                    task_thumbnail_state = ThumbnailState::TASK_THUMBNAIL;
                    BOOST_LOG_TRIVIAL(trace) << "web_request: use cache image";
                } else {
                    web_request = wxWebSession::GetDefault().CreateRequest(this, m_request_url);
                    BOOST_LOG_TRIVIAL(trace) << "monitor: start request thumbnail, url = " << m_request_url;
                    web_request.Start();
                    m_start_loading_thumbnail = false;
                }
            }
        }
    }
}

void StatusPanel::update_sdcard_subtask(MachineObject *obj)
{
    if (!obj) return;

    if (!m_load_sdcard_thumbnail) {
        update_calib_bitmap();
        if (m_current_print_mode != PrintingTaskType::CALIBRATION) {
            m_project_task_panel->get_bitmap_thumbnail()->SetBitmap(m_thumbnail_sdcard.bmp());
            m_project_task_panel->set_thumbnail_img(m_thumbnail_sdcard.bmp(), m_thumbnail_sdcard.name());
        }
        task_thumbnail_state = ThumbnailState::SDCARD_THUMBNAIL;
        m_load_sdcard_thumbnail = true;
    }
}

void StatusPanel::reset_printing_values()
{
    m_project_task_panel->enable_partskip_button(nullptr, false);
    m_project_task_panel->enable_pause_resume_button(false, "pause_disable");
    m_project_task_panel->enable_abort_button(false);
    m_project_task_panel->reset_printing_value();
    m_project_task_panel->update_subtask_name(NA_STR);
    m_project_task_panel->show_profile_info(false);
   // m_project_task_panel->update_stage_value_with_machine(wxEmptyString, 0, obj);
    m_project_task_panel->update_stage_value_with_machine(wxEmptyString, 0, obj);
    //obj->get_curr_stage()
    m_project_task_panel->update_progress_percent(NA_STR, wxEmptyString);

    m_project_task_panel->market_scoring_hide();
    m_project_task_panel->get_request_failed_panel()->Hide();
    update_basic_print_data(false);
    m_project_task_panel->update_left_time(NA_STR);
    m_project_task_panel->update_layers_num(true, wxString::Format(_L("Layer: %s"), NA_STR));
    update_calib_bitmap();
    
    task_thumbnail_state = ThumbnailState::PLACE_HOLDER;
    m_start_loading_thumbnail = false;
    m_load_sdcard_thumbnail   = false;
    skip_print_error = 0;
    this->Layout();
}

void StatusPanel::on_axis_ctrl_xy(wxCommandEvent &event)
{
    if (!obj) return;

    //check is at home
    static std::unordered_set<int> s_x_ctrl_idxes { 1, 3, 5, 7};
    static std::unordered_set<int> s_y_ctrl_idxes{ 0, 2, 4, 6 };
    if (s_x_ctrl_idxes.count(event.GetInt()) != 0 && !obj->is_axis_at_home("X"))
    {
        BOOST_LOG_TRIVIAL(info) << "axis x is not at home";
        show_recenter_dialog();
        return;
    }
    else if (s_y_ctrl_idxes.count(event.GetInt()) != 0 && !obj->is_axis_at_home("Y"))
    {
        BOOST_LOG_TRIVIAL(info) << "axis y is not at home";
        show_recenter_dialog();
        return;
    }

    if (event.GetInt() == 0)      { obj->command_axis_control("Y", 1.0, 10.0f, 3000); }
    else if (event.GetInt() == 1) { obj->command_axis_control("X", 1.0, -10.0f, 3000); }
    else if (event.GetInt() == 2) { obj->command_axis_control("Y", 1.0, -10.0f, 3000); }
    else if (event.GetInt() == 3) { obj->command_axis_control("X", 1.0, 10.0f, 3000); }
    else if (event.GetInt() == 4) { obj->command_axis_control("Y", 1.0, 1.0f, 3000); }
    else if (event.GetInt() == 5) { obj->command_axis_control("X", 1.0, -1.0f, 3000); }
    else if (event.GetInt() == 6) { obj->command_axis_control("Y", 1.0, -1.0f, 3000); }
    else if (event.GetInt() == 7) { obj->command_axis_control("X", 1.0, 1.0f, 3000); }
    else if (event.GetInt() == 8) {
        if (obj) { obj->command_go_home(); }
    }
}

bool StatusPanel::check_axis_z_at_home(MachineObject* obj)
{
    if (obj) {
        if (!obj->is_axis_at_home("Z")) {
            BOOST_LOG_TRIVIAL(info) << "axis z is not at home";
            show_recenter_dialog();
            return false;
        }
        return true;
    }
    return false;
}

void StatusPanel::on_axis_ctrl_z_up_10(wxCommandEvent &event)
{    
    if (obj) {
        obj->command_axis_control("Z", 1.0, -10.0f, 900);
        if (!check_axis_z_at_home(obj))
            return;
    }
}

void StatusPanel::on_axis_ctrl_z_up_1(wxCommandEvent &event)
{
    if (obj) {
        obj->command_axis_control("Z", 1.0, -1.0f, 900);
        if (!check_axis_z_at_home(obj))
            return;
    }
}

void StatusPanel::on_axis_ctrl_z_down_1(wxCommandEvent &event)
{
    if (obj) {
        obj->command_axis_control("Z", 1.0, 1.0f, 900);
        if (!check_axis_z_at_home(obj))
            return;
    }
}

void StatusPanel::on_axis_ctrl_z_down_10(wxCommandEvent &event)
{
    if (obj) {
        obj->command_axis_control("Z", 1.0, 10.0f, 900);
        if (!check_axis_z_at_home(obj))
            return;
    }
}

void StatusPanel::axis_ctrl_e_hint(bool up_down)
{
    if (ctrl_e_hint_dlg == nullptr) {
        /* ctrl_e_hint_dlg = new SecondaryCheckDialog(this->GetParent(), wxID_ANY, _L("Warning"), SecondaryCheckDialog::VisibleButtons::CONFIRM_AND_CANCEL, wxDefaultPosition,
         ctrl_e_hint_dlg->update_text(_L("Please heat the nozzle to above 170°C before loading or unloading filament."));
         ctrl_e_hint_dlg->m_show_again_checkbox->Hide();
         ctrl_e_hint_dlg->m_button_cancel->Hide();
         ctrl_e_hint_dlg->m_staticText_release_note->SetMaxSize(wxSize(FromDIP(360), -1));
         ctrl_e_hint_dlg->m_staticText_release_note->SetMinSize(wxSize(FromDIP(360), -1));
         ctrl_e_hint_dlg->Fit();*/
        ctrl_e_hint_dlg = new MessageDialog(this, _L("Please heat the nozzle to above 170°C before loading or unloading filament."), wxString(_L("Warning")), wxOK | wxCENTER);
    }
      ctrl_e_hint_dlg->ShowModal();
   // ctrl_e_hint_dlg->on_show();
}

void StatusPanel::on_axis_ctrl_e_up_10(wxCommandEvent &event)
{
    if (obj) {
        auto ext = obj->GetExtderSystem()->GetCurrentExtder();
        if (ext && ext->GetCurrentTemp() >= TEMP_THRESHOLD_ALLOW_E_CTRL)
            if (obj->is_enable_np) {
                obj->command_extruder_control(ext->GetExtId(), -10.0f);
            } else {
                obj->command_axis_control("E", 1.0, -10.0f, 900);
            }

        else
            axis_ctrl_e_hint(true);
    }
}

void StatusPanel::on_axis_ctrl_e_down_10(wxCommandEvent &event)
{
    if (obj) {
        auto ext = obj->GetExtderSystem()->GetCurrentExtder();
        if (ext && ext->GetCurrentTemp() >= TEMP_THRESHOLD_ALLOW_E_CTRL)
            if (obj->is_enable_np) {
                obj->command_extruder_control(ext->GetExtId(), 10.0f);
            } else {
                obj->command_axis_control("E", 1.0, 10.0f, 900);
            }
        else
            axis_ctrl_e_hint(false);
    }
}

void StatusPanel::on_set_bed_temp()
{
    if (!obj) {return;}

    wxString str = m_tempCtrl_bed->GetTextCtrl()->GetValue();
    try {
        long bed_temp;
        if (str.ToLong(&bed_temp) && obj) {
            set_hold_count(m_temp_bed_timeout);

            int limit = obj->get_bed_temperature_limit();
            if (obj->bed_temp_range.size() > 1) {
                limit = obj->bed_temp_range[1];
            }

            if (bed_temp >= limit) {
                BOOST_LOG_TRIVIAL(info) << "can not set over limit = " << limit << ", set temp = " << bed_temp;
                bed_temp = limit;
                m_tempCtrl_bed->SetTagTemp(wxString::Format("%d", bed_temp));
                m_tempCtrl_bed->Warning(false);
            }
            obj->command_set_bed(bed_temp);
        }
    } catch (...) {
        ;
    }
}

void StatusPanel::on_set_nozzle_temp(int nozzle_id)
{
    if (!obj) {return;}

    try {
        long nozzle_temp;

        if (nozzle_id == MAIN_EXTRUDER_ID) {
            wxString str = m_tempCtrl_nozzle->GetTextCtrl()->GetValue();
            if (str.ToLong(&nozzle_temp) && obj) {
                set_hold_count(m_temp_nozzle_timeout);
                if (nozzle_temp > m_tempCtrl_nozzle->get_max_temp()) {
                    nozzle_temp = m_tempCtrl_nozzle->get_max_temp();
                    m_tempCtrl_nozzle->SetTagTemp(wxString::Format("%d", nozzle_temp));
                    m_tempCtrl_nozzle->Warning(false);
                }
                if (m_tempCtrl_nozzle->GetCurrType() == TempInputType::TEMP_OF_NORMAL_TYPE) {
                    obj->command_set_nozzle(nozzle_temp);
                } else {
                    obj->command_set_nozzle_new(MAIN_EXTRUDER_ID, nozzle_temp);
                }
            }
        }

        if (nozzle_id == DEPUTY_EXTRUDER_ID) {
            wxString str = m_tempCtrl_nozzle_deputy->GetTextCtrl()->GetValue();
            if (str.ToLong(&nozzle_temp) && obj) {
                set_hold_count(m_temp_nozzle_deputy_timeout);
                if (nozzle_temp > m_tempCtrl_nozzle_deputy->get_max_temp()) {
                    nozzle_temp = m_tempCtrl_nozzle_deputy->get_max_temp();
                    m_tempCtrl_nozzle_deputy->SetTagTemp(wxString::Format("%d", nozzle_temp));
                    m_tempCtrl_nozzle_deputy->Warning(false);
                }
                obj->command_set_nozzle_new(DEPUTY_EXTRUDER_ID, nozzle_temp);
            }
        }
    } catch (...) {
        ;
    }
}

void StatusPanel::on_set_chamber_temp()
{
    if (!obj) {return;}

    wxString str = m_tempCtrl_chamber->GetTextCtrl()->GetValue();
    try {
        long chamber_temp;
        if (str.ToLong(&chamber_temp) && obj) {
            set_hold_count(m_temp_chamber_timeout);
            if (chamber_temp > m_tempCtrl_chamber->get_max_temp()) {
                chamber_temp = m_tempCtrl_chamber->get_max_temp();
                m_tempCtrl_chamber->SetTagTemp(wxString::Format("%d", chamber_temp));
                m_tempCtrl_chamber->Warning(false);
            }

            if(obj->is_in_printing() && obj->GetFan()->GetSupportAirduct() && obj->GetFan()->is_at_cooling_mode())
            {
#ifndef __APPLE__
                MessageDialog champer_switch_head_dlg(this, _L("Chamber temperature cannot be changed in cooling mode while printing."), wxEmptyString, wxICON_WARNING | wxOK);
#else
                wxMessageDialog champer_switch_head_dlg(this, _L("Chamber temperature cannot be changed in cooling mode while printing."), wxEmptyString, wxICON_WARNING | wxOK);
#endif
                champer_switch_head_dlg.ShowModal();
                return;
            }
            else if (!obj->GetFan()->is_at_heating_mode() && chamber_temp >= obj->GetConfig()->GetChamberTempSwitchHeat())
            {
#ifndef __APPLE__
                MessageDialog champer_switch_head_dlg(this, _L("If the chamber temperature exceeds 40\u2103, the system will automatically switch to heating mode. "
                                                                "Please confirm whether to switch."), wxEmptyString, wxICON_WARNING | wxOK | wxCANCEL);
#else
                /*STUDIO-10386 MessageDialog here may cause block in macOS, use wxMessageDialog*/
                wxMessageDialog champer_switch_head_dlg(this, _L("If the chamber temperature exceeds 40\u2103, the system will automatically switch to heating mode. "
                                                                   "Please confirm whether to switch."), wxEmptyString, wxICON_WARNING | wxOK | wxCANCEL);
#endif
                if (champer_switch_head_dlg.ShowModal() != wxID_OK) { return; }
            }

            obj->command_set_chamber(chamber_temp);
        }
    }
    catch (...) {
        ;
    }
}

void StatusPanel::on_ams_load(SimpleEvent &event)
{
    BOOST_LOG_TRIVIAL(info) << "on_ams_load";
    on_ams_load_curr();
}

void StatusPanel::update_load_with_temp()
{
    if (!obj->is_filament_at_extruder()) {
        m_is_load_with_temp = true;
    }
    else {
        m_is_load_with_temp = false;
    }
}

void StatusPanel::on_ams_load_curr()
{
    if (obj) {
        std::string                            curr_ams_id = m_ams_control->GetCurentAms();
        std::string                            curr_can_id = m_ams_control->GetCurrentCan(curr_ams_id);


        update_load_with_temp();
        //virtual tray
        if (curr_ams_id.compare(std::to_string(VIRTUAL_TRAY_MAIN_ID)) == 0 ||
            curr_ams_id.compare(std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) == 0)
        {
            int vt_slot_idx = 0;
            if (curr_ams_id.compare(std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) == 0)
            {
                vt_slot_idx = 1;
            }

            int old_temp = -1;
            int new_temp = -1;
            DevAmsTray* curr_tray = &obj->vt_slot[vt_slot_idx];

            if (!curr_tray) return;

            try {
                if (!curr_tray->nozzle_temp_max.empty() && !curr_tray->nozzle_temp_min.empty())
                    old_temp = (atoi(curr_tray->nozzle_temp_min.c_str()) + atoi(curr_tray->nozzle_temp_max.c_str())) / 2;
                if (!curr_tray->nozzle_temp_max.empty() && !curr_tray->nozzle_temp_min.empty())
                    new_temp = (atoi(curr_tray->nozzle_temp_min.c_str()) + atoi(curr_tray->nozzle_temp_max.c_str())) / 2;
            }
            catch (...) {
                ;
            }

            if (obj->is_enable_np || obj->is_enable_ams_np) {
                try {
                    if (!curr_ams_id.empty() && !curr_can_id.empty()) {
                        obj->command_ams_change_filament(true, curr_ams_id, "0", old_temp, new_temp);
                    }
                } catch (...) {}
            } else {
                obj->command_ams_change_filament(true, "254", "0", old_temp, new_temp);
            }
        }

        std::map<std::string, DevAms*>::iterator it = obj->GetFilaSystem()->GetAmsList().find(curr_ams_id);
        if (it == obj->GetFilaSystem()->GetAmsList().end()) {
            BOOST_LOG_TRIVIAL(trace) << "ams: find " << curr_ams_id << " failed";
            return;
        }
        auto tray_it = it->second->GetTrays().find(curr_can_id);
        if (tray_it == it->second->GetTrays().end()) {
            BOOST_LOG_TRIVIAL(trace) << "ams: find " << curr_can_id << " failed";
            return;
        }
        DevAmsTray* curr_tray = obj->get_curr_tray();
        DevAmsTray* targ_tray = obj->get_ams_tray(curr_ams_id, curr_can_id);

        int old_temp = -1;
        int new_temp = -1;

        if (curr_tray && targ_tray) {
            try {
                if (!curr_tray->nozzle_temp_max.empty() && !curr_tray->nozzle_temp_min.empty())
                    old_temp = (atoi(curr_tray->nozzle_temp_min.c_str()) + atoi(curr_tray->nozzle_temp_max.c_str())) / 2;
                if (!targ_tray->nozzle_temp_max.empty() && !targ_tray->nozzle_temp_min.empty())
                    new_temp = (atoi(targ_tray->nozzle_temp_min.c_str()) + atoi(targ_tray->nozzle_temp_max.c_str())) / 2;
            } catch (...) {
                ;
            }
        }

        int tray_index = atoi(curr_ams_id.c_str()) * 4 + atoi(tray_it->second->id.c_str());

        if (obj->is_enable_np) {
            try {
                if (!curr_ams_id.empty() && !curr_can_id.empty()) {
                    obj->command_ams_change_filament(true, curr_ams_id, curr_can_id, old_temp, new_temp);
                }
            }
            catch (...){}
        } else {
            obj->command_ams_change_filament(true, curr_ams_id, curr_can_id, old_temp, new_temp);
        }
    }
}

void StatusPanel::on_ams_load_vams(wxCommandEvent& event) {
    BOOST_LOG_TRIVIAL(info) << "on_ams_load_vams_tray";

    m_ams_control->SwitchAms(std::to_string(VIRTUAL_TRAY_MAIN_ID));
    on_ams_load_curr();
}

void StatusPanel::on_ams_switch(SimpleEvent &event)
{
    if(obj){

        /*right*/
        if (obj->GetExtderSystem()->GetTotalExtderCount() > 0) {
            auto ext = obj->GetExtderSystem()->GetExtderById(MAIN_EXTRUDER_ID);
            if (ext->HasFilamentInExt()) {
                if (ext->GetSlotNow().ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID) || ext->GetSlotNow().ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
                    m_ams_control->SetAmsStep(ext->GetSlotNow().ams_id, "0", AMSPassRoadType::AMS_ROAD_TYPE_LOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP3);
                } else {
                    m_ams_control->SetAmsStep(ext->GetSlotNow().ams_id, ext->GetSlotNow().slot_id, AMSPassRoadType::AMS_ROAD_TYPE_LOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP2);
                }
            } else {
                m_ams_control->SetAmsStep(ext->GetSlotNow().ams_id, ext->GetSlotNow().slot_id, AMSPassRoadType::AMS_ROAD_TYPE_UNLOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
            }
            m_ams_control->SetExtruder(ext->HasFilamentInExt(), MAIN_EXTRUDER_ID, ext->GetSlotNow().ams_id, ext->GetSlotNow().slot_id);
        }

        /*left*/
        if (obj->GetExtderSystem()->GetTotalExtderCount() > 1) {
            auto ext = obj->GetExtderSystem()->GetExtruders()[DEPUTY_EXTRUDER_ID];
            if (ext.HasFilamentInExt()) {
                if (ext.GetSlotNow().ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID) || ext.GetSlotNow().ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
                    m_ams_control->SetAmsStep(ext.GetSlotNow().ams_id, "0", AMSPassRoadType::AMS_ROAD_TYPE_LOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP3);
                } else {
                    m_ams_control->SetAmsStep(ext.GetSlotNow().ams_id, ext.GetSlotNow().slot_id, AMSPassRoadType::AMS_ROAD_TYPE_LOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP2);
                }
            } else {
                m_ams_control->SetAmsStep(ext.GetSlotNow().ams_id, ext.GetSlotNow().slot_id, AMSPassRoadType::AMS_ROAD_TYPE_UNLOAD, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
            }
            m_ams_control->SetExtruder(ext.HasFilamentInExt(), DEPUTY_EXTRUDER_ID, ext.GetSlotNow().ams_id, ext.GetSlotNow().slot_id);
        }
    }
}

void StatusPanel::on_ams_unload(SimpleEvent &event)
{
    if (obj) {
        std::string curr_ams_id = m_ams_control->GetCurentAms();
        std::string curr_can_id = m_ams_control->GetCurrentCan(curr_ams_id);

        if (obj->is_enable_np) {
            try {
                for (auto ext : obj->GetExtderSystem()->GetExtruders()) {
                    if (ext.GetSlotNow().ams_id == curr_ams_id && ext.GetSlotNow().slot_id == curr_can_id) { obj->command_ams_change_filament(false, curr_ams_id, "255"); }
                }
            } catch (...) {}
        } else {
            obj->command_ams_change_filament(false, curr_ams_id, "255");
        }
    }
}

void StatusPanel::on_ams_filament_backup(SimpleEvent& event)
{
    if (obj) {
        AmsReplaceMaterialDialog* m_replace_material_popup = new AmsReplaceMaterialDialog(this);
        m_replace_material_popup->update_machine_obj(obj);
        m_replace_material_popup->ShowModal();
    }
}

void StatusPanel::on_ams_setting_click(SimpleEvent &event)
{
    if (obj) {
        if (!m_ams_setting_dlg) {
            m_ams_setting_dlg = new AMSSetting((wxWindow*)this, wxID_ANY);
        }

        m_ams_setting_dlg->UpdateByObj(obj);
        m_ams_setting_dlg->Show();
    }
}

void StatusPanel::on_filament_extrusion_cali(wxCommandEvent &event)
{
    if (!m_extrusion_cali_dlg)
        m_extrusion_cali_dlg = new ExtrusionCalibration((wxWindow*)this, wxID_ANY);

    if (obj) {
        m_extrusion_cali_dlg->obj = obj;
        std::string ams_id = m_ams_control->GetCurentAms();
        std::string tray_id = m_ams_control->GetCurrentCan(ams_id);
        if (tray_id.empty() && ams_id.compare(std::to_string(VIRTUAL_TRAY_MAIN_ID)) != 0) {
            wxString txt = _L("Please select an AMS slot before calibration");
            MessageDialog msg_dlg(nullptr, txt, wxEmptyString, wxICON_WARNING | wxOK);
            msg_dlg.ShowModal();
            return;
        }

        int ams_id_int  = 0;
        int tray_id_int = 0;


        // set ams_filament id is is bbl filament
        if (ams_id.compare(std::to_string(VIRTUAL_TRAY_MAIN_ID)) == 0) {
            tray_id_int = VIRTUAL_TRAY_MAIN_ID;
            m_extrusion_cali_dlg->ams_filament_id = "";
        }
        else {
            ams_id_int = atoi(ams_id.c_str());
            tray_id_int = atoi(tray_id.c_str());

            auto tray = obj->GetFilaSystem()->GetAmsTray(ams_id, tray_id);
            if (tray) {
                if (DevFilaSystem::IsBBL_Filament(tray->tag_uid))
                    m_extrusion_cali_dlg->ams_filament_id = tray->setting_id;
                else
                    m_extrusion_cali_dlg->ams_filament_id = "";
            }
        }

        try {
            m_extrusion_cali_dlg->ams_id = ams_id_int;
            m_extrusion_cali_dlg->tray_id = tray_id_int;
            m_extrusion_cali_dlg->SetPosition(m_staticText_control->GetScreenPosition());
            m_extrusion_cali_dlg->Popup();
        } catch(...) {
            ;
        }
    }
}

void StatusPanel::on_filament_edit(wxCommandEvent &event)
{
    // update params
    if (!m_filament_setting_dlg) m_filament_setting_dlg = new AMSMaterialsSetting((wxWindow *) this, wxID_ANY);

    int current_position_x = m_ams_control->GetScreenPosition().x;
    int current_position_y = m_ams_control->GetScreenPosition().y - FromDIP(40);
    auto drect = wxDisplay(GetParent()).GetGeometry().GetHeight() - FromDIP(50);
    current_position_y = current_position_y + m_filament_setting_dlg->GetSize().GetHeight() > drect ? drect - m_filament_setting_dlg->GetSize().GetHeight() : current_position_y;

    if (obj) {
        m_filament_setting_dlg->obj = obj;

        int ams_id = event.GetInt();
        int slot_id = event.GetString().IsEmpty() ? 0 : std::stoi(event.GetString().ToStdString());

        try {
            m_filament_setting_dlg->ams_id  = ams_id;
            m_filament_setting_dlg->slot_id = slot_id;

            std::string sn_number;
            std::string filament;
            std::string temp_max;
            std::string temp_min;
            wxString    k_val;
            wxString    n_val;

            auto tray = obj->GetFilaSystem()->GetAmsTray(std::to_string(ams_id), std::to_string(slot_id));
            if (tray)
            {
                k_val = wxString::Format("%.3f", tray->k);
                n_val = wxString::Format("%.3f", tray->n);
                wxColor color = DevAmsTray::decode_color(tray->color);
                // m_filament_setting_dlg->set_color(color);

                std::vector<wxColour> cols;
                for (auto col : tray->cols) { cols.push_back(DevAmsTray::decode_color(col)); }
                m_filament_setting_dlg->set_ctype(tray->ctype);
                m_filament_setting_dlg->ams_filament_id = tray->setting_id;

                if (m_filament_setting_dlg->ams_filament_id.empty())
                {
                    m_filament_setting_dlg->set_empty_color(color);
                }
                else
                {
                    m_filament_setting_dlg->set_color(color);
                    m_filament_setting_dlg->set_colors(cols);
                }

                m_filament_setting_dlg->m_is_third = !DevFilaSystem::IsBBL_Filament(tray->tag_uid);
                if (!m_filament_setting_dlg->m_is_third)
                {
                    sn_number = tray->uuid;
                    filament = tray->sub_brands;
                    temp_max = tray->nozzle_temp_max;
                    temp_min = tray->nozzle_temp_min;
                }
            }

            m_filament_setting_dlg->Move(wxPoint(current_position_x, current_position_y));
            m_filament_setting_dlg->Popup(filament, sn_number, temp_min, temp_max, k_val, n_val);
        } catch (...) {
            ;
        }
    }
}

void StatusPanel::on_ext_spool_edit(wxCommandEvent &event)
{
    // update params
    if (!m_filament_setting_dlg) m_filament_setting_dlg = new AMSMaterialsSetting((wxWindow*)this, wxID_ANY);

    int current_position_x = m_ams_control->GetScreenPosition().x;
    int current_position_y = m_ams_control->GetScreenPosition().y - FromDIP(40);
    auto drect = wxDisplay(GetParent()).GetGeometry().GetHeight() - FromDIP(50);
    current_position_y = current_position_y + m_filament_setting_dlg->GetSize().GetHeight() > drect ? drect - m_filament_setting_dlg->GetSize().GetHeight() : current_position_y;

    if (obj) {
        m_filament_setting_dlg->obj = obj;

        int ams_id                     = event.GetInt();
        int slot_id                    = event.GetString().IsEmpty() ? 0 : std::stoi(event.GetString().ToStdString());

        m_filament_setting_dlg->ams_id = ams_id;
        m_filament_setting_dlg->slot_id  = slot_id;
        int nozzle_index = ams_id == VIRTUAL_TRAY_MAIN_ID ? 0 : 1;

        try {
            std::string sn_number;
            std::string filament;
            std::string temp_max;
            std::string temp_min;
            wxString k_val;
            wxString n_val;
            k_val                                   = wxString::Format("%.3f", obj->vt_slot[nozzle_index].k);
            n_val                                   = wxString::Format("%.3f", obj->vt_slot[nozzle_index].n);
            wxColor color                           = DevAmsTray::decode_color(obj->vt_slot[nozzle_index].color);
            m_filament_setting_dlg->ams_filament_id = obj->vt_slot[nozzle_index].setting_id;

            std::vector<wxColour> cols;
            for (auto col : obj->vt_slot[nozzle_index].cols) {
                cols.push_back(DevAmsTray::decode_color(col));
            }
            m_filament_setting_dlg->set_ctype(obj->vt_slot[nozzle_index].ctype);

            if (m_filament_setting_dlg->ams_filament_id.empty()) {
                m_filament_setting_dlg->set_empty_color(color);
            }
            else {
                m_filament_setting_dlg->set_color(color);
                m_filament_setting_dlg->set_colors(cols);
            }

            m_filament_setting_dlg->m_is_third = !DevFilaSystem::IsBBL_Filament(obj->vt_slot[nozzle_index].tag_uid);
            if (!m_filament_setting_dlg->m_is_third) {
                sn_number = obj->vt_slot[nozzle_index].uuid;
                filament  = obj->vt_slot[nozzle_index].sub_brands;
                temp_max  = obj->vt_slot[nozzle_index].nozzle_temp_max;
                temp_min  = obj->vt_slot[nozzle_index].nozzle_temp_min;
            }

            m_filament_setting_dlg->Move(wxPoint(current_position_x,current_position_y));
            m_filament_setting_dlg->Popup(filament, sn_number, temp_min, temp_max, k_val, n_val);
        }
        catch (...) {
            ;
        }
    }
}

void StatusPanel::on_ams_refresh_rfid(wxCommandEvent &event)
{
    if (obj) {

        //std::string curr_ams_id = m_ams_control->GetCurentAms();
        if (event.GetInt() < 0 || event.GetInt() > VIRTUAL_TRAY_MAIN_ID){
            return;
        }
        std::string curr_ams_id = std::to_string(event.GetInt());
        // do not support refresh rfid for VIRTUAL_TRAY_MAIN_ID
        if (curr_ams_id.compare(std::to_string(VIRTUAL_TRAY_MAIN_ID)) == 0) {
            return;
        }
        std::string curr_can_id = event.GetString().ToStdString();

        std::map<std::string, DevAms *>::iterator it = obj->GetFilaSystem()->GetAmsList().find(curr_ams_id);
        if (it == obj->GetFilaSystem()->GetAmsList().end()) {
            BOOST_LOG_TRIVIAL(trace) << "ams: find " << curr_ams_id << " failed";
            return;
        }
        auto slot_it = it->second->GetTrays().find(curr_can_id);
        if (slot_it == it->second->GetTrays().end()) {
            BOOST_LOG_TRIVIAL(trace) << "ams: find " << curr_can_id << " failed";
            return;
        }

        auto has_filament_at_extruder = false;
        auto use_new_command = false;

        if (obj->is_enable_np || obj->is_enable_ams_np) {
            use_new_command = true;
            if (it->second->GetExtruderId() < obj->GetExtderSystem()->GetTotalExtderSize()) {
                has_filament_at_extruder = obj->GetExtderSystem()->HasFilamentInExt(it->second->GetExtruderId());
            }
        } else {
            has_filament_at_extruder = obj->is_filament_at_extruder();
        }

        if (has_filament_at_extruder) {
            MessageDialog msg_dlg(nullptr, _L("Cannot read filament info: the filament is loaded to the tool head,please unload the filament and try again."), wxEmptyString,
                                  wxICON_WARNING | wxYES);
            msg_dlg.ShowModal();
            return;
        }


        try {
            if (!use_new_command) {
                int tray_index = atoi(curr_ams_id.c_str()) * 4 + atoi(slot_it->second->id.c_str());
                obj->command_ams_refresh_rfid(std::to_string(tray_index));
            }

            if (use_new_command) {
                obj->command_ams_refresh_rfid2(stoi(curr_ams_id), stoi(curr_can_id));
            }

        } catch (...) {
            ;
        }
    }
}

void StatusPanel::on_ams_selected(wxCommandEvent &event)
{
    if (obj) {
        std::string curr_ams_id = m_ams_control->GetCurentAms();
        std::string curr_selected_ams_id = std::to_string(event.GetInt());

        if (curr_ams_id.compare(std::to_string(VIRTUAL_TRAY_MAIN_ID)) == 0) {
            return;
        } else {
            std::string curr_can_id = event.GetString().ToStdString();
            std::map<std::string, DevAms *>::iterator it = obj->GetFilaSystem()->GetAmsList().find(curr_ams_id);
            if (it == obj->GetFilaSystem()->GetAmsList().end()) {
                BOOST_LOG_TRIVIAL(trace) << "ams: find " << curr_ams_id << " failed";
                return;
            }
            auto tray_it = it->second->GetTrays().find(curr_can_id);
            if (tray_it == it->second->GetTrays().end()) {
                BOOST_LOG_TRIVIAL(trace) << "ams: find " << curr_can_id << " failed";
                return;
            }
            try {
                int tray_index = atoi(curr_ams_id.c_str()) * 4 + atoi(tray_it->second->id.c_str());
                obj->command_ams_select_tray(std::to_string(tray_index));
            } catch (...) {
                ;
            }
        }
    }
}

void StatusPanel::on_ams_guide(wxCommandEvent& event)
{
    wxString ams_wiki_url;
    if (m_ams_control && m_ams_control->m_is_none_ams_mode == AMSModel::GENERIC_AMS) {
        ams_wiki_url = "https://wiki.bambulab.com/en/software/bambu-studio/use-ams-on-bambu-studio";
    }
    else if (m_ams_control && m_ams_control->m_is_none_ams_mode == AMSModel::AMS_LITE) {
        ams_wiki_url = "https://wiki.bambulab.com/en/ams-lite";
    }
    else {
        ams_wiki_url = "https://wiki.bambulab.com/en/software/bambu-studio/use-ams-on-bambu-studio";
    }

    wxLaunchDefaultBrowser(ams_wiki_url);
}

void StatusPanel::on_ams_retry(wxCommandEvent& event)
{
    BOOST_LOG_TRIVIAL(info) << "on_ams_retry";
    if (obj) {
        obj->command_ams_control("resume");
    }
}


void StatusPanel::on_fan_changed(wxCommandEvent& event)
{
    auto type = event.GetInt();
    auto speed = atoi(event.GetString().c_str());
    set_hold_count(this->m_switch_cham_fan_timeout);
}

void StatusPanel::on_cham_temp_kill_focus(wxFocusEvent& event)
{
    event.Skip();
    cham_temp_input = false;
}

void StatusPanel::on_cham_temp_set_focus(wxFocusEvent& event)
{
    event.Skip();
    cham_temp_input = true;
}

void StatusPanel::on_bed_temp_kill_focus(wxFocusEvent &event)
{
    event.Skip();
    bed_temp_input = false;
}

void StatusPanel::on_bed_temp_set_focus(wxFocusEvent &event)
{
    event.Skip();
    bed_temp_input = true;
}

void StatusPanel::on_nozzle_temp_kill_focus(wxFocusEvent &event)
{
    event.Skip();
    nozzle_temp_input = false;
}

void StatusPanel::on_nozzle_temp_set_focus(wxFocusEvent &event)
{
    event.Skip();
    nozzle_temp_input = true;
}

void StatusPanel::on_switch_speed(wxCommandEvent &event)
{
    auto now = boost::posix_time::microsec_clock::universal_time();
    if ((now - speed_dismiss_time).total_milliseconds() < 200) {
        speed_dismiss_time = now - boost::posix_time::seconds(1);
        return;
    }
#if __WXOSX__
    // MacOS has focus problem
    PopupWindow *popUp = new PopupWindow(nullptr);
#else
    PopupWindow *popUp = new PopupWindow(m_switch_speed);
#endif
#ifdef __WXMSW__
    popUp->BindUnfocusEvent();
#endif
    popUp->SetBackgroundColour(StateColor::darkModeColorFor(0xeeeeee));
    StepCtrl *step = new StepCtrl(popUp, wxID_ANY);
    wxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(step, 1, wxEXPAND, 0);
    popUp->SetSizer(sizer);
    auto em = em_unit(this);
    popUp->SetSize(em * 36, em * 8);
    step->SetHint(_L("This only takes effect during printing"));
    step->AppendItem(_L("Silent"), "");
    step->AppendItem(_L("Standard"), "");
    step->AppendItem(_L("Sport"), "");
    step->AppendItem(_L("Ludicrous"), "");

    // default speed lvl
    int selected_item = 1;
    if (obj) {
        int speed_lvl_idx = obj->GetPrintingSpeedLevel() - 1;
        if (speed_lvl_idx >= 0 && speed_lvl_idx < 4) {
            selected_item = speed_lvl_idx;
        }
    }
    step->SelectItem(selected_item);

    if (!obj->is_in_printing()) {
        step->Bind(wxEVT_LEFT_DOWN, [](auto& e) {
            return; });
    }

    step->Bind(EVT_STEP_CHANGED, [this](auto &e) {
        this->speed_lvl        = e.GetInt() + 1;
        if (obj) {
            set_hold_count(this->speed_lvl_timeout);
            obj->command_set_printing_speed((DevPrintingSpeedLevel)this->speed_lvl);
        }
    });
    popUp->Bind(wxEVT_SHOW, [this, popUp](auto &e) {
        if (!e.IsShown()) {
            popUp->Destroy();
            speed_dismiss_time = boost::posix_time::microsec_clock::universal_time();
        }
        });

    wxPoint pos = m_switch_speed->ClientToScreen(wxPoint(0, -6));
    popUp->Position(pos, {0, m_switch_speed->GetSize().y + 12});
    popUp->Popup();
}

void StatusPanel::on_printing_fan_switch(wxCommandEvent &event)
{
   /* if (!obj) return;

    bool value = m_switch_printing_fan->GetValue();

    if (value) {
        obj->command_control_fan(MachineObject::FanType::BIG_COOLING_FAN, true);
        m_switch_printing_fan->SetValue(true);
        set_hold_count(this->m_switch_printing_fan_timeout);
    } else {
        obj->command_control_fan(MachineObject::FanType::BIG_COOLING_FAN, false);
        m_switch_printing_fan->SetValue(false);
        set_hold_count(this->m_switch_printing_fan_timeout);
    }*/
}

void StatusPanel::on_nozzle_fan_switch(wxCommandEvent &event)
{
    if (m_fan_control_popup) {
        m_fan_control_popup->Destroy();
        m_fan_control_popup = nullptr;
    }

    if (!obj) { return; }
    if (obj->GetFan()->GetAirDuctData().modes.empty())
    {
        obj->GetFan()->converse_to_duct(true, obj->GetFan()->GetSupportAuxFanData(), obj->GetFan()->GetSupportChamberFan());
    }

    m_fan_control_popup = new FanControlPopupNew(this, obj, obj->GetFan()->GetAirDuctData());

    auto pos = m_switch_fan->GetScreenPosition();
    pos.y = pos.y + m_switch_fan->GetSize().y;

    int display_idx = wxDisplay::GetFromWindow(this);
    auto display = wxDisplay(display_idx).GetClientArea();


    wxSize screenSize = wxSize(display.GetWidth(), display.GetHeight());
    wxSize fan_popup_size = m_fan_control_popup->GetSize();

    pos.x -= FromDIP(150);
    pos.y -= FromDIP(20);
    if (screenSize.y - fan_popup_size.y < FromDIP(300)) {
        pos.y = (screenSize.y - fan_popup_size.y) / 2;
    }

    m_fan_control_popup->SetPosition(pos);
    m_fan_control_popup->ShowModal();
}
void StatusPanel::on_lamp_switch(wxCommandEvent &event)
{
    if (!obj) return;

    bool value = m_switch_lamp->GetValue();

    if (value) {
        m_switch_lamp->SetValue(true);
        // do not update when timeout > 0
        set_hold_count(this->m_switch_lamp_timeout);
        obj->GetLamp()->CtrlSetChamberLight(DevLamp::LIGHT_EFFECT_ON);
    } else {
        if (obj->GetLamp()->HasLampCloseRecheck()){
            MessageDialog msg_dlg(nullptr, _L("Turning off the lights during the task will cause the failure of AI monitoring, like spaghetti detection. Please choose carefully."), wxEmptyString, wxICON_WARNING | wxOK | wxCANCEL);
            msg_dlg.SetButtonLabel(wxID_OK, _L("Keep it On"));
            msg_dlg.SetButtonLabel(wxID_CANCEL, _L("Turn it Off"));
            if (msg_dlg.ShowModal() != wxID_CANCEL) {
                return;
            }
        }

        m_switch_lamp->SetValue(false);
        set_hold_count(this->m_switch_lamp_timeout);
        obj->GetLamp()->CtrlSetChamberLight(DevLamp::LIGHT_EFFECT_OFF);
    }
}

void StatusPanel::on_switch_vcamera(wxMouseEvent &event)
{
    //if (!obj) return;
    //bool value = m_recording_button->get_switch_status();
    //obj->command_ipcam_record(!value);
    m_media_play_ctrl->ToggleStream();
    show_vcamera = m_media_play_ctrl->IsStreaming();
    if (m_camera_popup)
        m_camera_popup->sync_vcamera_state(show_vcamera);
}

void StatusPanel::on_camera_enter(wxMouseEvent& event)
{
    if (obj) {
        if (m_camera_popup == nullptr)
            m_camera_popup = std::make_shared<CameraPopup>(this);
        m_camera_popup->check_func_supported(obj);
        m_camera_popup->sync_vcamera_state(show_vcamera);
        m_camera_popup->Bind(EVT_VCAMERA_SWITCH, &StatusPanel::on_switch_vcamera, this);
        m_camera_popup->Bind(EVT_SDCARD_ABSENT_HINT, [this](wxCommandEvent &e) {
            if (sdcard_hint_dlg == nullptr) {
                sdcard_hint_dlg = new SecondaryCheckDialog(this->GetParent(), wxID_ANY, _L("Warning"), SecondaryCheckDialog::VisibleButtons::ONLY_CONFIRM); // ORCA VisibleButtons instead ButtonStyle 
                sdcard_hint_dlg->update_text(_L("Can't start this without storage."));
            }
            sdcard_hint_dlg->on_show();
            });
        m_camera_popup->Bind(EVT_CAM_SOURCE_CHANGE, &StatusPanel::on_camera_source_change, this);
        wxWindow* ctrl = (wxWindow*)event.GetEventObject();
        wxPoint   pos = ctrl->ClientToScreen(wxPoint(0, 0));
        wxSize        sz   = ctrl->GetSize();
        pos.x += sz.x;
        pos.y += sz.y;
        m_camera_popup->SetPosition(pos);
        m_camera_popup->update(m_media_play_ctrl->IsStreaming());
        m_camera_popup->Popup();
    }
}

void StatusBasePanel::on_camera_source_change(wxCommandEvent& event)
{
    handle_camera_source_change();
}

void StatusBasePanel::handle_camera_source_change()
{
    const auto new_cam_url = wxGetApp().app_config->get("camera", "custom_source");
    const auto enabled = wxGetApp().app_config->get("camera", "enable_custom_source") == "true";

    if (enabled && !new_cam_url.empty()) {
        m_custom_camera_view->LoadURL(new_cam_url);
        toggle_custom_camera();
        m_camera_switch_button->Show();
    } else {
        toggle_builtin_camera();
        m_camera_switch_button->Hide();
    }
}

void StatusBasePanel::toggle_builtin_camera()
{
    m_custom_camera_view->Hide();
    m_media_ctrl->Show();
    m_media_play_ctrl->Show();
}

void StatusBasePanel::toggle_custom_camera()
{
    const auto enabled = wxGetApp().app_config->get("camera", "enable_custom_source") == "true";

    if (enabled) {
        m_custom_camera_view->Show();
        m_media_ctrl->Hide();
        m_media_play_ctrl->Hide();
    }
}

void StatusBasePanel::on_camera_switch_toggled(wxMouseEvent& event)
{
    const auto enabled = wxGetApp().app_config->get("camera", "enable_custom_source") == "true";
    if (enabled && m_media_ctrl->IsShown()) {
        toggle_custom_camera();
    } else {
        toggle_builtin_camera();
    }
}

void StatusBasePanel::remove_controls()
{
    const std::string js_cleanup_video_element = R"(
        document.body.style.overflow='hidden';
        const video = document.querySelector('video');
        video.setAttribute('style', 'width: 100% !important;');
        video.removeAttribute('controls');
        video.addEventListener('leavepictureinpicture', () => {
            window.wx.postMessage('leavepictureinpicture');
        });
        video.addEventListener('enterpictureinpicture', () => {
            window.wx.postMessage('enterpictureinpicture');
        });
    )";
    m_custom_camera_view->RunScript(js_cleanup_video_element);
}

void StatusPanel::on_camera_leave(wxMouseEvent& event)
{
    if (obj && m_camera_popup) {
        m_camera_popup->Dismiss();
    }
}

void StatusPanel::on_auto_leveling(wxCommandEvent &event)
{
    if (obj) obj->command_auto_leveling();
}

void StatusPanel::on_xyz_abs(wxCommandEvent &event)
{
    if (obj) obj->command_xyz_abs();
}


void StatusPanel::on_nozzle_selected(wxCommandEvent &event)
{
    if (obj) {

        /*Enable switch head while printing is paused STUDIO-9789*/
        if ((obj->is_in_printing() && !obj->is_in_printing_pause()) || obj->ams_status_main == AMS_STATUS_MAIN_FILAMENT_CHANGE) {
            MessageDialog dlg(nullptr, _L("The printer is busy with another print job."), _L("Error"), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return;
        }

        auto nozzle_id = event.GetInt();
        if (obj->GetCtrl()->command_select_extruder(nozzle_id) == 0)
        {
            return;
        }
    }

    m_nozzle_btn_panel->Enable();
}

void StatusPanel::on_show_print_options(wxCommandEvent& event)
{
    if (obj) {
        // Always show print options dialog for all machines
        if (print_options_dlg == nullptr) {
            print_options_dlg = new PrintOptionsDialog(this);
            print_options_dlg->update_machine_obj(obj);
            print_options_dlg->update_options(obj);
            print_options_dlg->ShowModal();
        }
        else {
            print_options_dlg->update_machine_obj(obj);
            print_options_dlg->update_options(obj);
            print_options_dlg->ShowModal();
        }
    }
}

void StatusPanel::on_show_safety_options(wxCommandEvent& event)
{
    if (obj) {
        std::string current_printer_type = obj->printer_type;
        bool supports_safety = DevPrinterConfigUtil::support_safety_options(current_printer_type);
        if (supports_safety) {
            if (safety_options_dlg == nullptr) {
                safety_options_dlg = new SafetyOptionsDialog(this);
                safety_options_dlg->update_machine_obj(obj);
                safety_options_dlg->update_options(obj);
                safety_options_dlg->ShowModal();
            }
            else {
                safety_options_dlg->update_machine_obj(obj);
                safety_options_dlg->update_options(obj);
                safety_options_dlg->ShowModal();
            }
        }
    }
}

void StatusPanel::on_show_parts_options(wxCommandEvent &event)
{
    if (obj) {
        if (print_parts_dlg == nullptr) {
            print_parts_dlg = new PrinterPartsDialog(this);
            print_parts_dlg->update_machine_obj(obj);
            print_parts_dlg->ShowModal();
        }
        else {
            print_parts_dlg->update_machine_obj(obj);
            print_parts_dlg->ShowModal();
        }
    }
}

void StatusPanel::update_printer_parts_options(MachineObject* obj_)
{
    if(obj_){
        if(print_parts_dlg && print_parts_dlg->IsShown()){
            print_parts_dlg->update_machine_obj(obj_);
            print_parts_dlg->UpdateNozzleInfo();
        }
    }
}

void StatusPanel::on_start_calibration(wxCommandEvent &event)
{
    if (obj) {
        if (calibration_dlg == nullptr) {
            calibration_dlg = new CalibrationDialog();
            calibration_dlg->update_machine_obj(obj);
            calibration_dlg->update_cali(obj);
            calibration_dlg->ShowModal();
        } else {
            calibration_dlg->update_machine_obj(obj);
            calibration_dlg->update_cali(obj);
            calibration_dlg->ShowModal();
        }
    }
}

bool StatusPanel::is_stage_list_info_changed(MachineObject *obj)
{
    if (!obj) return true;

    if (last_stage_list_info.size() != obj->stage_list_info.size()) return true;

    for (int i = 0; i < last_stage_list_info.size(); i++) {
        if (last_stage_list_info[i] != obj->stage_list_info[i]) return true;
    }
    last_stage_list_info = obj->stage_list_info;
    return false;
}

void StatusPanel::set_default()
{
    BOOST_LOG_TRIVIAL(trace) << "status_panel: set_default";
    obj                  = nullptr;
    last_subtask         = nullptr;
    last_tray_exist_bits = -1;
    speed_lvl         = 1;
    speed_lvl_timeout = 0;
    m_switch_lamp_timeout = 0;
    m_temp_nozzle_timeout = 0;
    m_temp_nozzle_deputy_timeout = 0;
    m_temp_bed_timeout = 0;
    m_temp_chamber_timeout = 0;
    m_switch_nozzle_fan_timeout = 0;
    m_switch_printing_fan_timeout = 0;
    m_switch_cham_fan_timeout = 0;
    m_show_ams_group = false;
    m_show_filament_group = false;
    reset_printing_values();

    m_bitmap_timelapse_img->Hide();
    m_bitmap_recording_img->Hide();
    m_bitmap_vcamera_img->Hide();
    m_setting_button->Show();
    m_tempCtrl_chamber->Show();
    m_options_btn->Show();
    m_safety_btn->Show();
    m_parts_btn->Show();


    if (m_panel_control_title) {
        m_panel_control_title->Layout();
        m_panel_control_title->Refresh();
    }

    reset_temp_misc_control();
    m_extruder_switching_status->Hide();
    m_ams_control->Hide();
    m_ams_control_box->Hide();
    m_ams_control->Reset();
    m_scale_panel->Hide();
    m_filament_load_box->Hide();
    m_filament_step->Hide();
    error_info_reset();
#ifndef __WXGTK__
    SetFocus();
#endif
}

void StatusPanel::show_status(int status)
{
    if (last_status == status) return;
    last_status = status;

    if (((status & (int) MonitorStatus::MONITOR_DISCONNECTED) != 0)
     || ((status & (int) MonitorStatus::MONITOR_DISCONNECTED_SERVER) != 0)
     || ((status & (int)MonitorStatus::MONITOR_CONNECTING) != 0)
     || ((status & (int)MonitorStatus::MONITOR_NO_PRINTER) != 0)
        ) {
        show_printing_status(false, false);
        m_calibration_btn->Disable();
        m_options_btn->Disable();
        m_safety_btn->Disable();
        m_parts_btn->Disable();
        m_panel_monitoring_title->Disable();
    } else if ((status & (int) MonitorStatus::MONITOR_NORMAL) != 0) {
        show_printing_status(true, true);
        m_calibration_btn->Disable();
        m_options_btn->Enable();
        m_safety_btn->Enable();
        m_parts_btn->Enable();
        m_panel_monitoring_title->Enable();
    }
}

void StatusPanel::set_hold_count(int& count)
{
    count = COMMAND_TIMEOUT;
}

void StatusPanel::rescale_camera_icons()
{
    if (!GetParent() || IsBeingDeleted()) return;
    if (!m_setting_button || !m_media_play_ctrl || !m_bitmap_vcamera_img || !m_bitmap_sdcard_img || !m_bitmap_recording_img || !m_bitmap_timelapse_img) return;

    m_setting_button->msw_rescale();


    m_bitmap_sdcard_state_abnormal = ScalableBitmap(this, wxGetApp().dark_mode()?"sdcard_state_abnormal_dark":"sdcard_state_abnormal", 20);
    m_bitmap_sdcard_state_normal = ScalableBitmap(this, wxGetApp().dark_mode()?"sdcard_state_normal_dark":"sdcard_state_normal", 20);
    m_bitmap_sdcard_state_no = ScalableBitmap(this, wxGetApp().dark_mode()?"sdcard_state_no_dark":"sdcard_state_no", 20);
    m_bitmap_recording_on = ScalableBitmap(this, wxGetApp().dark_mode()?"monitor_recording_on_dark":"monitor_recording_on", 20);
    m_bitmap_recording_off = ScalableBitmap(this, wxGetApp().dark_mode()?"monitor_recording_off_dark":"monitor_recording_off", 20);
    m_bitmap_timelapse_on = ScalableBitmap(this, wxGetApp().dark_mode()?"monitor_timelapse_on_dark":"monitor_timelapse_on", 20);
    m_bitmap_timelapse_off = ScalableBitmap(this, wxGetApp().dark_mode()?"monitor_timelapse_off_dark":"monitor_timelapse_off", 20);
    m_bitmap_vcamera_on = ScalableBitmap(this, wxGetApp().dark_mode()?"monitor_vcamera_on_dark":"monitor_vcamera_on", 20);
    m_bitmap_vcamera_off = ScalableBitmap(this, wxGetApp().dark_mode()?"monitor_vcamera_off_dark":"monitor_vcamera_off", 20);

    if (m_media_play_ctrl->IsStreaming()) {
        m_bitmap_vcamera_img->SetBitmap(m_bitmap_vcamera_on.bmp());
    }
    else {
        m_bitmap_vcamera_img->SetBitmap(m_bitmap_vcamera_off.bmp());
    }

    if (!obj) return;

    if (obj->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::NO_SDCARD) {
        m_bitmap_sdcard_img->SetBitmap(m_bitmap_sdcard_state_no.bmp());
    } else if (obj->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::HAS_SDCARD_NORMAL) {
        m_bitmap_sdcard_img->SetBitmap(m_bitmap_sdcard_state_normal.bmp());
    } else if (obj->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::HAS_SDCARD_ABNORMAL) {
        m_bitmap_sdcard_img->SetBitmap(m_bitmap_sdcard_state_abnormal.bmp());
    } else {
        m_bitmap_sdcard_img->SetBitmap(m_bitmap_sdcard_state_normal.bmp());
    }

    if (obj->is_recording()) {
        m_bitmap_recording_img->SetBitmap(m_bitmap_recording_on.bmp());
    } else {
        m_bitmap_recording_img->SetBitmap(m_bitmap_recording_off.bmp());
    }

    if (obj->is_timelapse()) {
        m_bitmap_timelapse_img->SetBitmap(m_bitmap_timelapse_on.bmp());
    } else {
        m_bitmap_timelapse_img->SetBitmap(m_bitmap_timelapse_off.bmp());
    }
}

void StatusPanel::on_sys_color_changed()
{
    m_project_task_panel->msw_rescale();
    m_bitmap_speed.msw_rescale();
    m_bitmap_speed_active.msw_rescale();
    m_switch_speed->SetImages(m_bitmap_speed, m_bitmap_speed);
    m_ams_control->msw_rescale();
    rescale_camera_icons();
}

void StatusPanel::msw_rescale()
{
    init_bitmaps();
    m_project_task_panel->init_bitmaps();
    m_project_task_panel->msw_rescale();
    m_panel_monitoring_title->SetSize(wxSize(-1, FromDIP(PAGE_TITLE_HEIGHT)));
    //m_staticText_monitoring->SetMinSize(wxSize(PAGE_TITLE_TEXT_WIDTH, PAGE_TITLE_HEIGHT));
    m_bmToggleBtn_timelapse->Rescale();
    m_panel_control_title->SetSize(wxSize(-1, FromDIP(PAGE_TITLE_HEIGHT)));
    //m_staticText_control->SetMinSize(wxSize(-1, PAGE_TITLE_HEIGHT));
    m_media_play_ctrl->msw_rescale();
    m_bpButton_xy->SetBitmap(m_bitmap_axis_home);
    m_bpButton_xy->SetMinSize(AXIS_MIN_SIZE);
    m_bpButton_xy->SetSize(AXIS_MIN_SIZE);
    m_temp_extruder_line->SetSize(wxSize(FromDIP(1), -1));
    update_extruder_status(obj);
    //m_bitmap_extruder_img->SetMinSize(EXTRUDER_IMAGE_SIZE);

    for (Button *btn : m_buttons) { btn->Rescale(); }
    init_scaled_buttons();


    m_bpButton_xy->Rescale();
    auto size = TEMP_CTRL_MIN_SIZE_ALIGN_ONE_ICON;
    if (obj && obj->GetExtderSystem()->GetTotalExtderCount() >= 2) size = TEMP_CTRL_MIN_SIZE_ALIGN_TWO_ICON;
    m_tempCtrl_nozzle->SetMinSize(size);
    m_tempCtrl_nozzle->Rescale();
    m_tempCtrl_nozzle_deputy->SetMinSize(size);
    m_tempCtrl_nozzle_deputy->Rescale();
    m_line_nozzle->SetSize(wxSize(-1, FromDIP(1)));
    m_tempCtrl_bed->SetMinSize(size);
    m_tempCtrl_bed->Rescale();
    m_tempCtrl_chamber->SetMinSize(size);
    m_tempCtrl_chamber->Rescale();

    for(int i = 0; i < m_extruder_book->GetPageCount(); i++)
    {
        ExtruderImage* ext_img = dynamic_cast<ExtruderImage*> (m_extruder_book->GetPage(i));
        if (ext_img)
        {
            ext_img->msw_rescale();
        }
    }

    m_bitmap_speed.msw_rescale();
    m_bitmap_speed_active.msw_rescale();

    m_switch_speed->SetImages(m_bitmap_speed, m_bitmap_speed);
    m_switch_speed->SetMinSize(MISC_BUTTON_2FAN_SIZE);
    m_switch_speed->Rescale();
    m_switch_lamp->SetImages(m_bitmap_lamp_on, m_bitmap_lamp_off);
    m_switch_lamp->SetMinSize(MISC_BUTTON_2FAN_SIZE);
    m_switch_lamp->Rescale();
    /*m_switch_nozzle_fan->SetImages(m_bitmap_fan_on, m_bitmap_fan_off);
    m_switch_nozzle_fan->Rescale();
    m_switch_printing_fan->SetImages(m_bitmap_fan_on, m_bitmap_fan_off);
    m_switch_printing_fan->Rescale();
    m_switch_cham_fan->SetImages(m_bitmap_fan_on, m_bitmap_fan_off);
    m_switch_cham_fan->Rescale();*/

    m_switch_fan->SetImages(m_bitmap_fan_on, m_bitmap_fan_off);
    m_switch_fan->Rescale();
    if (m_fan_control_popup)
    {
        m_fan_control_popup->msw_rescale();
    }

    //m_switch_fan->SetImages(m_bitmap_fan_on, m_bitmap_fan_off);
    //m_switch_fan->Rescale();

    m_bpButton_z_10->Rescale();
    m_bpButton_z_1->Rescale();
    m_bpButton_z_down_1->Rescale();
    m_bpButton_z_down_10->Rescale();
    m_extruder_switching_status->msw_rescale();

    m_ams_control->msw_rescale();
    // m_filament_step->Rescale();


    m_calibration_btn->SetMinSize(wxSize(-1, FromDIP(26)));
    m_calibration_btn->Rescale();

    m_options_btn->SetMinSize(wxSize(-1, FromDIP(26)));
    m_options_btn->Rescale(); 

    m_safety_btn->SetMinSize(wxSize(-1, FromDIP(26)));
    m_safety_btn->Rescale();

    m_parts_btn->SetMinSize(wxSize(-1, FromDIP(26)));
    m_parts_btn->Rescale();

    rescale_camera_icons();

    Layout();
    Refresh();
}

void StatusPanel::update_filament_loading_panel(MachineObject* obj)
{
    if (!obj) {
        show_filament_load_group(false);
        return;
    }

    bool ams_loading_state = false;
    auto ams_status_sub = obj->ams_status_sub;

    if (obj->is_enable_np) {
        ams_loading_state = obj->GetExtderSystem()->IsBusyLoading();
    } else if (obj->ams_status_main == AMS_STATUS_MAIN_FILAMENT_CHANGE) {
        ams_loading_state = true;
    }

    if (ams_loading_state) {
        update_load_with_temp();

        const std::string& cur_ams_id = obj->GetExtderSystem()->GetCurrentAmsId();
        const std::string& cur_tray_id = obj->GetExtderSystem()->GetCurrentSlotId();
        if (!cur_ams_id.empty() && !cur_tray_id.empty()) {
            m_filament_step->updateID(std::atoi(cur_ams_id.c_str()), std::atoi(cur_tray_id.c_str()));
        }

        auto loading_ext = obj->GetExtderSystem()->GetLoadingExtder();
        auto tar = loading_ext ? loading_ext->GetSlotTarget() : DevAmsSlotInfo();
        bool busy_for_vt_loading = (tar.ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID) || tar.ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) && !obj->is_target_slot_unload();
        if (busy_for_vt_loading) {
            // wait to heat hotend
            if (ams_status_sub == 0x02) {
                m_filament_step->SetFilamentStep(FilamentStep::STEP_HEAT_NOZZLE, FilamentStepType::STEP_TYPE_VT_LOAD);
            } else if (ams_status_sub == 0x05) {
                m_filament_step->SetFilamentStep(FilamentStep::STEP_PUSH_NEW_FILAMENT, FilamentStepType::STEP_TYPE_VT_LOAD);
            } else if (ams_status_sub == 0x06) {
                m_filament_step->SetFilamentStep(FilamentStep::STEP_CONFIRM_EXTRUDED, FilamentStepType::STEP_TYPE_VT_LOAD);
            } else if (ams_status_sub == 0x07) {
                m_filament_step->SetFilamentStep(FilamentStep::STEP_PURGE_OLD_FILAMENT, FilamentStepType::STEP_TYPE_VT_LOAD);
            } else {
                m_filament_step->SetFilamentStep(FilamentStep::STEP_IDLE, FilamentStepType::STEP_TYPE_VT_LOAD);
                ams_loading_state = false;
            }
        } else {
            // wait to heat hotend
            if (ams_status_sub == 0x02) {
                if (!obj->is_target_slot_unload()) {
                    m_filament_step->SetFilamentStep(FilamentStep::STEP_HEAT_NOZZLE, FilamentStepType::STEP_TYPE_LOAD);
                } else {
                    m_filament_step->SetFilamentStep(FilamentStep::STEP_HEAT_NOZZLE, FilamentStepType::STEP_TYPE_UNLOAD);
                }
            } else if (ams_status_sub == 0x03) {
                if (!obj->is_target_slot_unload()) {
                    m_filament_step->SetFilamentStep(FilamentStep::STEP_CUT_FILAMENT, FilamentStepType::STEP_TYPE_LOAD);
                } else {
                    m_filament_step->SetFilamentStep(FilamentStep::STEP_CUT_FILAMENT, FilamentStepType::STEP_TYPE_UNLOAD);
                }
            } else if (ams_status_sub == 0x04) {
                if (!obj->is_target_slot_unload()) {
                    m_filament_step->SetFilamentStep(FilamentStep::STEP_PULL_CURR_FILAMENT, FilamentStepType::STEP_TYPE_LOAD);
                } else {
                    m_filament_step->SetFilamentStep(FilamentStep::STEP_PULL_CURR_FILAMENT, FilamentStepType::STEP_TYPE_UNLOAD);
                }
            } else if (ams_status_sub == 0x05) {
                if (!obj->is_target_slot_unload()) {
                    if (m_is_load_with_temp) {
                        m_filament_step->SetFilamentStep(FilamentStep::STEP_CUT_FILAMENT, FilamentStepType::STEP_TYPE_LOAD);
                    } else {
                        m_filament_step->SetFilamentStep(FilamentStep::STEP_PUSH_NEW_FILAMENT, FilamentStepType::STEP_TYPE_LOAD);
                    }

                } else {
                    m_filament_step->SetFilamentStep(FilamentStep::STEP_PUSH_NEW_FILAMENT, FilamentStepType::STEP_TYPE_UNLOAD);
                }
            } else if (ams_status_sub == 0x06) {
                if (!obj->is_target_slot_unload()) {
                    m_filament_step->SetFilamentStep(FilamentStep::STEP_PUSH_NEW_FILAMENT, FilamentStepType::STEP_TYPE_LOAD);
                } else {
                    m_filament_step->SetFilamentStep(FilamentStep::STEP_PUSH_NEW_FILAMENT, FilamentStepType::STEP_TYPE_UNLOAD);
                }
            } else if (ams_status_sub == 0x07) {
                if (!obj->is_target_slot_unload()) {
                    if (m_is_load_with_temp) {
                        m_filament_step->SetFilamentStep(FilamentStep::STEP_PULL_CURR_FILAMENT, FilamentStepType::STEP_TYPE_LOAD);
                    } else {
                        m_filament_step->SetFilamentStep(FilamentStep::STEP_PURGE_OLD_FILAMENT, FilamentStepType::STEP_TYPE_LOAD);
                    }
                } else {
                    m_filament_step->SetFilamentStep(FilamentStep::STEP_PURGE_OLD_FILAMENT, FilamentStepType::STEP_TYPE_UNLOAD);
                }
            } else if (ams_status_sub == 0x08) {
                if (!obj->is_target_slot_unload()) {
                    m_filament_step->SetFilamentStep(FilamentStep::STEP_CHECK_POSITION, FilamentStepType::STEP_TYPE_LOAD);
                } else {
                    m_filament_step->SetFilamentStep(FilamentStep::STEP_CHECK_POSITION, FilamentStepType::STEP_TYPE_UNLOAD);
                }
            } else if (ams_status_sub == 0x09) {
                //just wait
            } else if (ams_status_sub == 0x0B) {
                if (!obj->is_target_slot_unload()) {
                    m_filament_step->SetFilamentStep(FilamentStep::STEP_CHECK_POSITION, FilamentStepType::STEP_TYPE_LOAD);
                } else {
                    m_filament_step->SetFilamentStep(FilamentStep::STEP_CHECK_POSITION, FilamentStepType::STEP_TYPE_UNLOAD);
                }
            } else {
                m_filament_step->SetFilamentStep(FilamentStep::STEP_IDLE, FilamentStepType::STEP_TYPE_UNLOAD);
                ams_loading_state = false;
            }
        }
    } else if (obj->ams_status_main == AMS_STATUS_MAIN_ASSIST) {
        m_filament_step->SetFilamentStep(FilamentStep::STEP_IDLE, FilamentStepType::STEP_TYPE_LOAD);
        ams_loading_state = false;
    } else {
        m_filament_step->SetFilamentStep(FilamentStep::STEP_IDLE, FilamentStepType::STEP_TYPE_LOAD);
        ams_loading_state = false;
    }

    show_filament_load_group(ams_loading_state);
}

ScoreDialog::ScoreDialog(wxWindow *parent, int design_id, std::string model_id, int profile_id, int rating_id, bool success_printed, int star_count)
    : DPIDialog(parent, wxID_ANY, _L("Rate the Print Profile"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    , m_design_id(design_id)
    , m_model_id(model_id)
    , m_profile_id(profile_id)
    , m_star_count(star_count)
    , m_rating_id(rating_id)
    , m_success_printed(success_printed)
    , m_upload_status_code(StatusCode::CODE_NUMBER)
{
    m_tocken.reset(new int(0));

    wxBoxSizer *m_main_sizer = get_main_sizer();

    this->SetSizer(m_main_sizer);
    Fit();
    Layout();
    wxGetApp().UpdateDlgDarkUI(this);
}

ScoreDialog::ScoreDialog(wxWindow *parent, ScoreData *score_data)
    : DPIDialog(parent, wxID_ANY, _L("Rate the Print Profile"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    , m_design_id(score_data->design_id)
    , m_rating_id(score_data->rating_id)
    , m_model_id(score_data->model_id)
    , m_profile_id(score_data->profile_id)
    , m_star_count(score_data->star_count)
    , m_success_printed(score_data->success_printed)
    , m_upload_status_code(StatusCode::CODE_NUMBER)
{
    m_tocken.reset(new int(0));
    
    wxBoxSizer *m_main_sizer = get_main_sizer(score_data->local_to_url_image, score_data->comment_text);

    m_image_url_paths        = score_data->image_url_paths;
    

    this->SetSizer(m_main_sizer);
    Fit();
    Layout();
    wxGetApp().UpdateDlgDarkUI(this);

}

ScoreDialog::~ScoreDialog() {}

void ScoreDialog::on_dpi_changed(const wxRect &suggested_rect) {}

void ScoreDialog::OnBitmapClicked(wxMouseEvent &event)
{
    wxStaticBitmap *clickedBitmap = dynamic_cast<wxStaticBitmap *>(event.GetEventObject());
    if (m_image.find(clickedBitmap) != m_image.end()) { 
        if (!m_image[clickedBitmap].is_selected) {
            for (auto panel : m_image[clickedBitmap].image_broad) { 
                panel->Show();
            }
            m_image[clickedBitmap].is_selected = true;
            m_selected_image_list.insert(clickedBitmap);
        } else {
            for (auto panel : m_image[clickedBitmap].image_broad) { 
                panel->Hide(); 
            }
            m_image[clickedBitmap].is_selected = false;
            m_selected_image_list.erase(clickedBitmap);
            m_selected_image_list.erase(clickedBitmap);
        }
    }
    if (m_selected_image_list.empty())
        m_delete_photo->Hide();
    else
        m_delete_photo->Show();
    Fit();
    Layout();

}

 std::set <std::pair<wxStaticBitmap * ,wxString>> ScoreDialog::add_need_upload_imgs()
{ 
    std::set<std::pair<wxStaticBitmap *, wxString>> need_upload_images;
    for (auto bitmap : m_image) { 
        if (!bitmap.second.is_uploaded) {
            wxString &local_image_path = bitmap.second.local_image_url;
            if (!local_image_path.empty()) { need_upload_images.insert(std::make_pair(bitmap.first, local_image_path)); }
        }
    }
    return need_upload_images;
}



std::pair<wxStaticBitmap *, ScoreDialog::ImageMsg> ScoreDialog::create_local_thumbnail(wxString &local_path)
{
    std::pair<wxStaticBitmap *, ImageMsg> bitmap_to_image_msg;
    if (local_path.empty()) return bitmap_to_image_msg;

    ImageMsg cur_image_msg;
    cur_image_msg.local_image_url = local_path;
    cur_image_msg.img_url_paths   = "";
    cur_image_msg.is_uploaded     = false;
    
    wxStaticBitmap *imageCtrl = new wxStaticBitmap(this, wxID_ANY, wxBitmap(wxImage(local_path, wxBITMAP_TYPE_ANY).Rescale(FromDIP(80), FromDIP(60))), wxDefaultPosition,
                                                   wxDefaultSize, 0);
    imageCtrl->Bind(wxEVT_LEFT_DOWN, &ScoreDialog::OnBitmapClicked, this);

    m_image_sizer->Add(create_broad_sizer(imageCtrl, cur_image_msg), 0, wxALL, 5);

    bitmap_to_image_msg.first = imageCtrl;
    bitmap_to_image_msg.second = cur_image_msg;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": local picture is download";
    return bitmap_to_image_msg;
}

std::pair<wxStaticBitmap *, ScoreDialog::ImageMsg> ScoreDialog::create_oss_thumbnail(std::string &oss_path)
{
    std::pair<wxStaticBitmap *, ImageMsg> bitmap_to_image_msg;
    if (oss_path.empty()) return bitmap_to_image_msg;

    ImageMsg cur_image_msg;
    cur_image_msg.local_image_url = "";
    cur_image_msg.img_url_paths   = oss_path;
    cur_image_msg.is_uploaded     = true;


    wxImage         image(Slic3r::resources_dir() + "/images/oss_picture_loading.png", wxBITMAP_TYPE_ANY);
    wxStaticBitmap *imageCtrl = new wxStaticBitmap(this, wxID_ANY, wxBitmap(image.Rescale(FromDIP(80), FromDIP(60))), wxDefaultPosition, wxDefaultSize, 0);
    imageCtrl->Bind(wxEVT_LEFT_DOWN, &ScoreDialog::OnBitmapClicked, this);

    Slic3r::Http http   = Slic3r::Http::get(oss_path);
    std::string  suffix = oss_path.substr(oss_path.find_last_of(".") + 1);
    http.header("accept", "image/" + suffix) //"image/" + suffix
        .header("Accept-Encoding", "gzip")
        .on_complete([this, imageCtrl, time = std::weak_ptr<int>(m_tocken)](std::string body, unsigned int status) {
            if (time.expired()) return;
            wxMemoryInputStream stream(body.data(), body.size());
            wxImage             success_image;
            if (success_image.LoadFile(stream, wxBITMAP_TYPE_ANY)) {
                CallAfter([this, success_image, imageCtrl]() { update_static_bitmap(imageCtrl, success_image); });

            } else {
                CallAfter([this, imageCtrl]() { update_static_bitmap(imageCtrl, fail_image); });
            }
        })
        .on_error([this, imageCtrl, &oss_path](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << "load oss picture failed, oss path: " << oss_path << " status:" << status << " error:" << error;
            CallAfter([this, imageCtrl]() { update_static_bitmap(imageCtrl, fail_image); });
        }).perform();

    m_image_sizer->Add(create_broad_sizer(imageCtrl, cur_image_msg), 0, wxALL, 5);

    bitmap_to_image_msg.first  = imageCtrl;
    bitmap_to_image_msg.second = cur_image_msg;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": oss picture is download";
    return bitmap_to_image_msg;
}

void ScoreDialog::update_static_bitmap(wxStaticBitmap* static_bitmap, wxImage image)
{
    static_bitmap->SetBitmap(wxBitmap(image.Rescale(FromDIP(80), FromDIP(60))));
    Layout();
    Fit();
    //Refresh();
}

wxBoxSizer *ScoreDialog::create_broad_sizer(wxStaticBitmap *bitmap, ImageMsg& cur_image_msg)
{ 
    // tb: top and bottom  lr: left and right
    auto m_image_tb_broad = new wxBoxSizer(wxVERTICAL);
    auto line_top         = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    line_top->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    m_image_tb_broad->Add(line_top, 0, wxEXPAND, 0);
    cur_image_msg.image_broad.push_back(line_top);
    line_top->Hide();

    auto m_image_lr_broad = new wxBoxSizer(wxHORIZONTAL);
    auto line_left        = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(1, -1), wxTAB_TRAVERSAL);
    line_left->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    m_image_lr_broad->Add(line_left, 0, wxEXPAND, 0);
    cur_image_msg.image_broad.push_back(line_left);
    line_left->Hide();

    m_image_lr_broad->Add(bitmap, 0, wxALL, 5);

    auto line_right = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(1, -1), wxTAB_TRAVERSAL);
    line_right->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    m_image_lr_broad->Add(line_right, 0, wxEXPAND, 0);
    m_image_tb_broad->Add(m_image_lr_broad, 0, wxEXPAND, 0);
    cur_image_msg.image_broad.push_back(line_right);
    line_right->Hide();

    auto line_bottom = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    line_bottom->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    m_image_tb_broad->Add(line_bottom, 0, wxEXPAND, 0);
    cur_image_msg.image_broad.push_back(line_bottom);
    line_bottom->Hide();

    cur_image_msg.is_selected    = false;
    cur_image_msg.image_tb_broad = m_image_tb_broad;

    return m_image_tb_broad;
}

void ScoreDialog::init() {
    SetBackgroundColour(*wxWHITE);
    SetMinSize(wxSize(FromDIP(540), FromDIP(380)));

    fail_image = wxImage(Slic3r::resources_dir() + "/images/oss_picture_load_failed.png", wxBITMAP_TYPE_ANY);
}

wxBoxSizer *ScoreDialog::get_score_sizer() { 
    wxBoxSizer    *score_sizer      = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *static_score_text = new wxStaticText(this, wxID_ANY, _L("Rate"), wxDefaultPosition, wxDefaultSize, 0);
    static_score_text->Wrap(-1);
    score_sizer->Add(static_score_text, 1, wxEXPAND | wxLEFT, FromDIP(24));
    score_sizer->Add(0, 0, 1, wxEXPAND, 0);
    return score_sizer;
}

wxBoxSizer *ScoreDialog::get_star_sizer()
{
    wxBoxSizer *static_score_star_sizer = new wxBoxSizer(wxHORIZONTAL);
    static_score_star_sizer->AddSpacer(FromDIP(20));
    m_score_star.resize(5);
    for (int i = 0; i < m_score_star.size(); ++i) {
        if (!m_success_printed && m_star_count > 3) {
            m_star_count = 3;
            warning_text->Show();
            Layout();
            Fit();
        }
        if (i < m_star_count) {
            m_score_star[i] = new ScalableButton(this, wxID_ANY, "score_star_light", wxEmptyString, wxSize(FromDIP(26), FromDIP(26)), wxDefaultPosition,
                                                 wxBU_EXACTFIT | wxNO_BORDER, true, 26);
        } else
            m_score_star[i] = new ScalableButton(this, wxID_ANY, "score_star_dark", wxEmptyString, wxSize(FromDIP(26), FromDIP(26)), wxDefaultPosition,
                                                 wxBU_EXACTFIT | wxNO_BORDER, true, 26);

        m_score_star[i]->SetMinSize(wxSize(FromDIP(26), FromDIP(26)));
        m_score_star[i]->SetMaxSize(wxSize(FromDIP(26), FromDIP(26)));
        m_score_star[i]->Bind(wxEVT_LEFT_DOWN, [this, i](auto &e) {
            if (!m_success_printed && i >= 3) {
                warning_text->Show();
                Layout();
                Fit();
                return;
            } else {
                warning_text->Hide();
                Layout();
                Fit();
            }
            for (int j = 0; j < m_score_star.size(); ++j) {
                ScalableBitmap light_star = ScalableBitmap(nullptr, "score_star_light", 26);
                m_score_star[j]->SetBitmap(light_star.bmp());
                if (m_score_star[j] == m_score_star[i]) {
                    m_star_count = j + 1;
                    break;
                }
            }
            for (int k = m_star_count; k < m_score_star.size(); ++k) {
                ScalableBitmap dark_star = ScalableBitmap(nullptr, "score_star_dark", 26);
                m_score_star[k]->SetBitmap(dark_star.bmp());
            }
        });
        static_score_star_sizer->Add(m_score_star[i], 1, wxEXPAND | wxLEFT, FromDIP(5));
    }

    return static_score_star_sizer;
}

wxBoxSizer* ScoreDialog::get_comment_text_sizer() {
    wxBoxSizer*    m_comment_sizer    = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *static_comment_text = new wxStaticText(this, wxID_ANY, _L("Comment"), wxDefaultPosition, wxDefaultSize, 0);
    static_comment_text->Wrap(-1);
    m_comment_sizer->Add(static_comment_text, 1, wxEXPAND | wxLEFT, FromDIP(24));
    m_comment_sizer->Add(0, 0, 1, wxEXPAND, 0);
    return m_comment_sizer;
}

void ScoreDialog::create_comment_text(const wxString& comment) {
    m_comment_text = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(492), FromDIP(104)), wxTE_MULTILINE);
    m_comment_text->SetBackgroundColour(wxColor(*wxWHITE));

    if (!comment.empty()) {
        m_comment_text->SetValue(comment);
    }
    m_comment_text->SetHint(_L("Rate this print"));
    m_comment_text->SetBackgroundColour(*wxWHITE);
    //m_comment_text->SetForegroundColour(wxColor("#BBBBBB"));
    m_comment_text->SetMinSize(wxSize(FromDIP(492), FromDIP(104)));

    m_comment_text->Bind(wxEVT_SET_FOCUS, [this](auto &event) {
        if (wxGetApp().dark_mode()) {
            m_comment_text->SetForegroundColour(wxColor(*wxWHITE));
        } else
            m_comment_text->SetForegroundColour(wxColor(*wxBLACK));
        m_comment_text->Refresh();
        event.Skip();
    });
}

wxBoxSizer *ScoreDialog::get_photo_btn_sizer() {
    wxBoxSizer *    m_photo_sizer    = new wxBoxSizer(wxHORIZONTAL);
    ScalableBitmap little_photo  = wxGetApp().dark_mode() ? ScalableBitmap(this, "single_little_photo_dark", 20) : ScalableBitmap(this, "single_little_photo", 20);
    wxStaticBitmap *little_photo_img   = new wxStaticBitmap(this, wxID_ANY, little_photo.bmp(), wxDefaultPosition, wxSize(FromDIP(20), FromDIP(20)), 0);
    m_photo_sizer->Add(little_photo_img, 0, wxEXPAND | wxLEFT, FromDIP(24));
    m_add_photo = new Label(this, _L("Add Photo"));
    m_add_photo->SetBackgroundColour(*wxWHITE);
    //m_add_photo->SetForegroundColour(wxColor("#898989"));
    m_add_photo->SetSize(wxSize(-1, FromDIP(20)));
    m_photo_sizer->Add(m_add_photo, 0, wxEXPAND | wxLEFT, FromDIP(12));

    m_delete_photo = new Label(this, _L("Delete Photo"));
    m_delete_photo->SetBackgroundColour(*wxWHITE);
    //m_delete_photo->SetForegroundColour(wxColor("#898989"));
    m_delete_photo->SetSize(wxSize(-1, FromDIP(20)));
    m_photo_sizer->Add(m_delete_photo, 0, wxEXPAND | wxLEFT, FromDIP(12));
    m_delete_photo->Hide();
    m_photo_sizer->Add(0, 0, 1, wxEXPAND, 0);

    m_add_photo->Bind(wxEVT_LEFT_DOWN, [this](auto &e) {
        // add photo logic
        wxFileDialog openFileDialog(this, "Select Images", "", "", "Image files (*.png;*.jpg;*jpeg)|*.png;*.jpg;*.jpeg", wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);

        if (openFileDialog.ShowModal() == wxID_CANCEL) return;

        wxArrayString filePaths;
        openFileDialog.GetPaths(filePaths);
        //wxArrayString filePaths_reduction;
        std::vector<std::pair<wxString, std::string>> local_path;
        for (int i = 0; i < filePaths.GetCount(); i++) { //It's ugly, but useful
            bool is_repeat = false;
            for (auto image : m_image) {
                if (filePaths[i] == image.second.local_image_url) { 
                    is_repeat = true;
                    continue;
                }
            }
            if (!is_repeat) {
                local_path.push_back(std::make_pair(filePaths[i], ""));
                if (local_path.size() + m_image.size() > m_photo_nums) { 
                    break; 
                }
            }
            
        }

        load_photo(local_path);

        m_image_sizer->Layout();
        this->Fit();
        this->Layout();
    });

        m_delete_photo->Bind(wxEVT_LEFT_DOWN, [this](auto &e) {
            for (auto it = m_selected_image_list.begin(); it != m_selected_image_list.end();) {
                auto bitmap = *it;
                m_image_sizer->Detach(m_image[bitmap].image_tb_broad);
                m_image[bitmap].image_tb_broad->DeleteWindows();

                m_image.erase(bitmap);
                it = m_selected_image_list.erase(it);
            }
            m_image_url_paths.clear();
            for (const auto& bitmap : m_image) {
                if (bitmap.second.is_uploaded) {
                    if (!bitmap.second.img_url_paths.empty()) {
                        m_image_url_paths.push_back(bitmap.second.img_url_paths);
                    }
                }
            }
            m_delete_photo->Hide();
            Layout();
            Fit();
        });

    return m_photo_sizer;
}

wxBoxSizer *ScoreDialog::get_button_sizer()
{
    wxBoxSizer *bSizer_button = new wxBoxSizer(wxHORIZONTAL);
    bSizer_button->Add(0, 0, 1, wxEXPAND, 0);

    m_button_ok = new Button(this, _L("Submit"));
    m_button_ok->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    bSizer_button->Add(m_button_ok, 0, wxRIGHT, FromDIP(ButtonProps::ChoiceButtonGap()));

    m_button_ok->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        m_upload_status_code = StatusCode::UPLOAD_PROGRESS;

        if (m_star_count == 0) {
            MessageDialog dlg(this, _L("Please click on the star first."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxOK);
            dlg.ShowModal();
            return;
        }

        std::set<std::pair<wxStaticBitmap *, wxString>> need_upload_images = add_need_upload_imgs();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": get need upload picture count: " << need_upload_images.size();

        std::string  comment = into_u8(m_comment_text->GetValue());
        unsigned int http_code;
        std::string  http_error;
        wxString  error_info;

        if (!need_upload_images.empty()) {
            std::string config;
            int         ret = wxGetApp().getAgent()->get_oss_config(config, wxGetApp().app_config->get_country_code(), http_code, http_error);
            if (ret == -1) {
                error_info += into_u8(_L("Get oss config failed.")) + "\n\thttp code: " + std::to_string(http_code) + "\n\thttp error: " + http_error;
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": get oss config filed and http_error: " << http_error;
                m_upload_status_code = StatusCode::UPLOAD_EXIST_ISSUE;
            }
            if (m_upload_status_code == StatusCode::UPLOAD_PROGRESS) {
                int             need_upload_nums   = need_upload_images.size();
                int             upload_nums        = 0;
                int             upload_failed_nums = 0;
                ProgressDialog *progress_dialog    = new ProgressDialog(_L("Upload Pictures"), _L("Number of images successfully uploaded") + ": " + std::to_string(upload_nums) + "/" + std::to_string(need_upload_nums), need_upload_nums, this);
                for (std::set<std::pair<wxStaticBitmap *, wxString>>::iterator it = need_upload_images.begin(); it != need_upload_images.end();) {
                    std::pair<wxStaticBitmap *, wxString> need_upload     = *it;
                    std::string need_upload_uf8 = into_u8(need_upload.second);
                    //Local path when incoming, cloud path when outgoing
                    ret = wxGetApp().getAgent()->put_rating_picture_oss(config, need_upload_uf8, m_model_id, m_profile_id, http_code, http_error);
                    std::unordered_map<wxStaticBitmap *, ImageMsg>::iterator iter;
                    switch (ret) {
                    case 0:
                        upload_nums++;
                        iter = m_image.find(need_upload.first);
                        if (m_image.end() != iter) {
                            iter->second.img_url_paths = need_upload_uf8;
                            iter->second.is_uploaded   = true;
                            m_image_url_paths.push_back(need_upload_uf8);
                        }
                        it++;
                        progress_dialog->Update(upload_nums, _L("Number of images successfully uploaded") + ": " + std::to_string(upload_nums) + "/" + std::to_string(need_upload_nums));
                        progress_dialog->Fit();
                        BOOST_LOG_TRIVIAL(info) << "put_rating_picture_oss: model_id [" << m_model_id << "] profile_id [" << m_profile_id << "] http_code [" << http_code
                                                << "] http_error [" << http_error << "] config [" << config << "]  image_path [" << need_upload.second << "]";
                        break;
                    case -1:
                        error_info += need_upload.second + _L(" upload failed").ToUTF8().data() + "\n\thttp code:" + std::to_string(http_code) + "\n\thttp_error:" + http_error + "\n";
                        m_upload_status_code = StatusCode::UPLOAD_IMG_FAILED;
                        ++it;
                        break;
                    case BAMBU_NETWORK_ERR_PARSE_CONFIG_FAILED:
                        error_info += need_upload.second + _L(" upload config prase failed\n").ToUTF8().data() + "\n";
                        m_upload_status_code = StatusCode::UPLOAD_IMG_FAILED;
                        ++it;
                        break;
                    case BAMBU_NETWORK_ERR_NO_CORRESPONDING_BUCKET:
                        error_info += need_upload.second + _L(" No corresponding storage bucket\n").ToUTF8().data() + "\n";
                        m_upload_status_code = StatusCode::UPLOAD_IMG_FAILED;
                        ++it;
                        break;
                    case BAMBU_NETWORK_ERR_OPEN_FILE_FAILED:
                        error_info += need_upload.second + _L(" cannot be opened\n").ToUTF8().data() + "\n";
                        m_upload_status_code = StatusCode::UPLOAD_IMG_FAILED;
                        ++it;
                        break;
                    }
                }
                progress_dialog->Hide();
                if (progress_dialog) { 
                    delete progress_dialog;
                    progress_dialog = nullptr;
                }

                if (m_upload_status_code == StatusCode::UPLOAD_IMG_FAILED) {
                    std::string   upload_failed_images = into_u8(_L("The following issues occurred during the process of uploading images. Do you want to ignore them?\n\n"));
                    MessageDialog dlg_info(this, upload_failed_images + error_info, wxString(_L("info")), wxOK | wxNO | wxCENTER);
                    if (dlg_info.ShowModal() == wxID_OK) {
                        m_upload_status_code = StatusCode::UPLOAD_PROGRESS;
                    }
                }
            }
        }

        if (m_upload_status_code == StatusCode::UPLOAD_PROGRESS) {
            int            ret = wxGetApp().getAgent()->put_model_mall_rating(m_rating_id, m_star_count, comment, m_image_url_paths, http_code, http_error);
            MessageDialog *dlg_info;
            switch (ret) {
            case 0: EndModal(wxID_OK); break;
            case BAMBU_NETWORK_ERR_GET_RATING_ID_FAILED:
                dlg_info = new MessageDialog(this, _L("Synchronizing the printing results. Please retry a few seconds later."), wxString(_L("info")), wxOK | wxCENTER);
                dlg_info->ShowModal();
                delete dlg_info;
                break;
            default: // Upload failed and obtaining instance_id failed
                if (ret == -1)
                    error_info += _L("Upload failed\n").ToUTF8().data();
                else
                    error_info += _L("obtaining instance_id failed\n").ToUTF8().data();
                if (!error_info.empty()) { BOOST_LOG_TRIVIAL(info) << error_info; }

                dlg_info = new MessageDialog(this,
                                             _L("Your comment result cannot be uploaded due to the following reasons:\n\n  error code: ") +
                                             std::to_string(http_code) + "\n  " + _L("error message: ") + http_error +
                                             _L("\n\nWould you like to redirect to the webpage to give a rating?"),
                                             wxString(_L("info")), wxOK | wxNO | wxCENTER);
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": upload rating picture failed and http error" << http_error;
                if (dlg_info->ShowModal() == wxID_OK) {
                    market_model_scoring_page(m_design_id);
                    EndModal(wxID_OK);
                }
                delete dlg_info;
                break;
            }
        } else if (m_upload_status_code == StatusCode::UPLOAD_IMG_FAILED) {
            MessageDialog *dlg_info = new MessageDialog(this,
                                                        _L("Some of your images failed to upload. Would you like to redirect to the webpage to give a rating?"),
                                                        wxString(_L("info")), wxOK | wxNO | wxCENTER);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": upload rating picture failed or get oss config failed";
            if (dlg_info->ShowModal() == wxID_OK) {
                market_model_scoring_page(m_design_id);
                EndModal(wxID_OK);
            }
            delete dlg_info;
            if (!error_info.empty()) { BOOST_LOG_TRIVIAL(info) << error_info; }
        }
    });

    m_button_cancel = new Button(this, _L("Cancel"));
    m_button_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    bSizer_button->Add(m_button_cancel, 0, wxRIGHT, FromDIP(ButtonProps::ChoiceButtonGap()));

    m_button_cancel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) { EndModal(wxID_CANCEL); });

    return bSizer_button;
}

void ScoreDialog::load_photo(const std::vector<std::pair<wxString, std::string>> &filePaths)
{
    for (size_t i = 0; i < filePaths.size(); ++i) {
        if (m_image.size() < m_photo_nums) {
            std::pair<wxString, std::string> local_to_url_path = filePaths[i];
            wxString                         filePath          = local_to_url_path.first;

            ImageMsg cur_image_msg;

            if (filePath.empty()) {  // local img path is empty, oss url path is exist
                std::string oss_url_path = local_to_url_path.second;
                //to do: load oss image, create wxStaticBitmap

                if (!oss_url_path.empty()) {
                    m_image.insert(create_oss_thumbnail(oss_url_path));
                }
                continue;
            } else {
                m_image.insert(create_local_thumbnail(filePath));
            }

        } else {
            MessageDialog *dlg_info_up_to_8 = new MessageDialog(this, _L("You can select up to 16 images."), wxString(_L("info")), wxOK | wxCENTER);
            dlg_info_up_to_8->ShowModal();
            break;
        }

    }
}

wxBoxSizer *ScoreDialog::get_main_sizer(const std::vector<std::pair<wxString, std::string>> &images, const wxString &comment)
{
    init();
    wxBoxSizer *m_main_sizer = new wxBoxSizer(wxVERTICAL);
    // top line
    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line_top->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    m_main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    m_main_sizer->Add(0, 0, 0, wxTOP, FromDIP(32));

    warning_text = new wxStaticText(this, wxID_ANY, _L("At least one successful print record of this print profile is required \nto give a positive rating (4 or 5 stars)."));
    warning_text->SetForegroundColour(*wxRED);
    warning_text->SetFont(::Label::Body_13);

    wxBoxSizer *score_sizer = get_score_sizer();
    m_main_sizer->Add(score_sizer, 0, wxEXPAND, FromDIP(20));
    m_main_sizer->Add(0, 0, 0, wxBOTTOM, FromDIP(8));

    wxBoxSizer *static_score_star_sizer = get_star_sizer();
    m_main_sizer->Add(static_score_star_sizer, 1, wxEXPAND | wxBOTTOM, FromDIP(20));

    m_main_sizer->Add(warning_text, 0, wxEXPAND | wxLEFT, FromDIP(24));
    m_main_sizer->Add(0, 0, 0, wxBOTTOM, FromDIP(8));
    warning_text->Hide();

    wxBoxSizer *m_comment_sizer = get_comment_text_sizer();
    m_main_sizer->Add(m_comment_sizer, 0, wxEXPAND, FromDIP(20));
    m_main_sizer->Add(0, 0, 0, wxBOTTOM, FromDIP(8));

    create_comment_text(comment);
    m_main_sizer->Add(m_comment_text, 0, wxLEFT, FromDIP(24));

    wxBoxSizer *m_photo_sizer = get_photo_btn_sizer();
    m_main_sizer->Add(m_photo_sizer, 0, wxEXPAND | wxTOP, FromDIP(8));

    m_image_sizer = new wxGridSizer(5, FromDIP(5), FromDIP(5));
    if (!images.empty()) { 
        load_photo(images);
    }
    m_main_sizer->Add(m_image_sizer, 0, wxEXPAND | wxLEFT, FromDIP(24));
    m_main_sizer->Add(0, 0, 1, wxEXPAND, 0);

    wxBoxSizer *bSizer_button = get_button_sizer();
    m_main_sizer->Add(bSizer_button, 0, wxEXPAND | wxBOTTOM, FromDIP(24));

    return m_main_sizer;
}

ScoreData ScoreDialog::get_score_data() { 
    ScoreData score_data;
    score_data.rating_id          = m_rating_id;
    score_data.design_id          = m_design_id;
    score_data.model_id           = m_model_id;
    score_data.profile_id         = m_profile_id;
    score_data.star_count         = m_star_count;
    score_data.success_printed    = m_success_printed;
    score_data.comment_text       = m_comment_text->GetValue();
    score_data.image_url_paths    = m_image_url_paths;
    for (auto img : m_image) { score_data.local_to_url_image.push_back(std::make_pair(img.second.local_image_url, img.second.img_url_paths)); }
    
    return score_data;
}

void ScoreDialog::set_comment(std::string comment)
{
    if (m_comment_text) { 

        m_comment_text->SetValue(wxString::FromUTF8(comment));
    }
}

void ScoreDialog::set_cloud_bitmap(std::vector<std::string> cloud_bitmaps)
{ 
    m_image_url_paths = cloud_bitmaps;
    for (std::string &url : cloud_bitmaps) {
        if (std::string::npos == url.find(m_model_id)) continue;
        m_image.insert(create_oss_thumbnail(url));
    }
    Layout();
    Fit();
}

} // namespace GUI
} // namespace Slic3r
