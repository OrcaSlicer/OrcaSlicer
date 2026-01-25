#include "GUI_Utils.hpp"
#include "GUI_App.hpp"
#include <wx/panel.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/gbsizer.h>
#include <wx/webrequest.h>
#include <wx/control.h>
#include <wx/dcclient.h>
#include <wx/display.h>
#include <wx/mstream.h>
#include <wx/sstream.h>
#include <wx/zstream.h>
#include <wx/window.h>
#include <wx/dcgraph.h>
#include <wx/glcanvas.h>
#include <wx/utils.h>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>

#include "Widgets/CheckBox.hpp"
#include "Widgets/Label.hpp"

#include "MsgDialog.hpp"
#include "Printer/PrinterFileSystem.h"
#include "PartSkipDialog.hpp"
#include "SkipPartCanvas.hpp"
#include "MediaPlayCtrl.h"

#include "DeviceCore/DevManager.h"

namespace Slic3r { namespace GUI {

extern wxString hide_passwd(wxString url, std::vector<wxString> const &passwords);
extern void     refresh_agora_url(char const *device, char const *dev_ver, char const *channel, void *context, void (*callback)(void *context, char const *url));

StateColor percent_bg(std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Disabled),
                      std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Pressed),
                      std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Hovered),
                      std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Enabled),
                      std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Normal));

static StateColor zoom_bg(std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Disabled),
                          std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
                          std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                          std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Enabled),
                          std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Normal));

static StateColor zoom_bd(std::pair<wxColour, int>(wxColour(144, 144, 144), StateColor::Disabled), std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));
static StateColor zoom_text(std::pair<wxColour, int>(wxColour(144, 144, 144), StateColor::Disabled), std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));

static StateColor btn_bg_white(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
                               std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                               std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));

static StateColor btn_bg_gray(std::pair<wxColour, int>(wxColour(194, 194, 194), StateColor::Pressed),
                              std::pair<wxColour, int>(wxColour(194, 194, 194), StateColor::Hovered),
                              std::pair<wxColour, int>(wxColour(194, 194, 194), StateColor::Normal));

static StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
                               std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
                               std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));

PartSkipDialog::PartSkipDialog(wxWindow *parent) : DPIDialog(parent, wxID_ANY, _L("Skip Objects"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    std::time_t       t = std::time(0);
    std::stringstream buf;
    buf << put_time(std::localtime(&t), "%a_%b_%d_%H_%M_%S/");
    m_timestamp = buf.str();

    std::string icon_path = (boost::format("%1%/images/OrcaSlicerTitle.ico") % Slic3r::resources_dir()).str();
    SetIcon(wxIcon(Slic3r::encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));
    SetBackgroundColour(*wxWHITE);

    m_sizer = new wxBoxSizer(wxVERTICAL);

    m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line_top->SetMinSize(wxSize(-1, 1));
    m_line_top->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    m_sizer->Add(m_line_top, 0, wxEXPAND | wxTOP, FromDIP(0));

    m_simplebook = new wxSimplebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0);
    m_simplebook->SetMinSize(wxSize(FromDIP(720), FromDIP(535)));
    m_simplebook->SetMaxSize(wxSize(FromDIP(720), FromDIP(535)));
    m_book_first_panel = new wxPanel(m_simplebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_book_third_panel = new wxPanel(m_simplebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_book_third_panel->SetBackgroundColour(*wxWHITE);
    m_dlg_sizer         = new wxBoxSizer(wxVERTICAL);
    m_dlg_content_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_canvas_sizer      = new wxBoxSizer(wxVERTICAL);

    // page 3
    wxGLAttributes canvasAttrs;
    canvasAttrs.PlatformDefaults().Defaults().Stencil(8).EndList();
    m_canvas = new SkipPartCanvas(m_book_third_panel, canvasAttrs);
    m_canvas->SetMinSize(wxSize(FromDIP(400), FromDIP(400)));
    m_canvas->SetMaxSize(wxSize(FromDIP(400), FromDIP(400)));

    m_canvas_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_canvas_btn_sizer->SetMinSize(wxSize(-1, FromDIP(28)));

    zoom_bg.setTakeFocusedAsHovered(false);

    m_zoom_out_btn = new Button(m_book_third_panel, wxEmptyString);
    m_zoom_out_btn->SetIcon("canvas_zoom_out");
    m_zoom_out_btn->SetToolTip(_L("Zoom Out"));
    m_zoom_out_btn->SetBackgroundColor(zoom_bg);
    m_zoom_out_btn->SetBorderColor(wxColour(238, 238, 238));
    m_zoom_out_btn->SetCornerRadius(0);
    m_zoom_out_btn->SetMinSize(wxSize(FromDIP(56), FromDIP(28)));

    m_percent_label = new Button(m_book_third_panel, "100%");
    m_percent_label->SetBackgroundColor(percent_bg);
    m_percent_label->SetBorderColor(wxColour(238, 238, 238));
    m_percent_label->SetMinSize(wxSize(FromDIP(56), FromDIP(28)));
    m_percent_label->SetCornerRadius(0);

    m_zoom_in_btn = new Button(m_book_third_panel, wxEmptyString);
    m_zoom_in_btn->SetIcon("canvas_zoom_in");
    m_zoom_in_btn->SetToolTip(_L("Zoom In"));
    m_zoom_in_btn->SetBackgroundColor(zoom_bg);
    m_zoom_in_btn->SetBorderColor(wxColour(238, 238, 238));
    m_zoom_in_btn->SetMinSize(wxSize(FromDIP(56), FromDIP(28)));
    m_zoom_in_btn->SetCornerRadius(0);

    m_switch_drag_btn = new Button(m_book_third_panel, wxEmptyString);
    m_switch_drag_btn->SetIcon("canvas_drag");
    m_switch_drag_btn->SetToolTip(_L("Drag"));
    m_switch_drag_btn->SetBackgroundColor(*wxWHITE);
    m_switch_drag_btn->SetBorderColor(wxColour(238, 238, 238));
    m_switch_drag_btn->SetMinSize(wxSize(FromDIP(56), FromDIP(28)));
    m_switch_drag_btn->SetCornerRadius(0);

    m_canvas_btn_sizer->Add(m_zoom_out_btn, 0, wxEXPAND | wxLEFT, FromDIP(105));
    m_canvas_btn_sizer->Add(m_percent_label, 0, wxEXPAND, 0);
    m_canvas_btn_sizer->Add(m_zoom_in_btn, 0, wxEXPAND, 0);
    m_canvas_btn_sizer->Add(m_switch_drag_btn, 0, wxEXPAND | wxRIGHT, FromDIP(88));

    m_list_sizer = new wxBoxSizer(wxVERTICAL);
    m_list_sizer->SetMinSize(wxSize(FromDIP(267), FromDIP(422)));

    auto all_checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_all_checkbox          = new CheckBox(m_book_third_panel, wxID_ANY);
    m_all_checkbox->SetValue(false);
    m_all_checkbox->SetMinSize(wxSize(FromDIP(18), FromDIP(18)));
    m_all_checkbox->SetBackgroundColour(*wxWHITE);
    m_all_label = new Label(m_book_third_panel, _L("Select All"));
    m_all_label->Wrap(-1);
    m_all_label->SetMinSize(wxSize(-1, FromDIP(18)));
    m_all_label->SetBackgroundColour(*wxWHITE);

    m_all_label->SetMinSize(wxSize(267, -1));
    m_all_label->SetMaxSize(wxSize(267, -1));
    all_checkbox_sizer->Add(m_all_checkbox, 0, wxALIGN_CENTER_VERTICAL, 0);
    all_checkbox_sizer->Add(m_all_label, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(8));

    m_line = new wxPanel(m_book_third_panel, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(267), FromDIP(1)), wxTAB_TRAVERSAL);
    m_line->SetMinSize(wxSize(FromDIP(267), 1));
    m_line->SetMaxSize(wxSize(FromDIP(267), 1));
    m_line->SetBackgroundColour(wxColor(238, 238, 238));

    m_list_view = new wxScrolledWindow(m_book_third_panel, wxID_ANY, wxDefaultPosition, wxSize(267, -1), wxHSCROLL | wxVSCROLL);
    m_list_view->SetScrollRate(5, 5);
    m_list_view->SetMinSize(wxSize(FromDIP(267), FromDIP(378)));
    m_list_view->SetMaxSize(wxSize(FromDIP(267), FromDIP(378)));
    m_list_view->SetBackgroundColour(*wxWHITE);

    m_scroll_sizer = new wxBoxSizer(wxVERTICAL);
    m_list_view->SetSizer(m_scroll_sizer);
    m_list_view->Layout();

    m_dlg_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_dlg_btn_sizer->SetMinSize(wxSize(-1, FromDIP(54)));

    m_cnt_label = new Label(m_book_third_panel, wxEmptyString);
    m_cnt_label->Wrap(-1);
    m_cnt_label->SetBackgroundColour(*wxWHITE);
    m_cnt_label->SetForegroundColour(wxColour(0, 150, 136));
    m_cnt_label->SetFont(Label::Head_16);
    m_cnt_label->SetSize(wxSize(-1, FromDIP(20)));
    m_cnt_label->SetMaxSize(wxSize(-1, FromDIP(20)));

    m_tot_label = new Label(m_book_third_panel, wxEmptyString);
    m_tot_label->Wrap(-1);
    m_tot_label->SetBackgroundColour(*wxWHITE);
    m_tot_label->SetMinSize(wxSize(FromDIP(200), FromDIP(20)));

    m_apply_btn = new Button(m_book_third_panel, _L("Skip"));
    m_apply_btn->SetBackgroundColor(btn_bg_gray);
    m_apply_btn->SetTextColor(wxColour("#FFFFFE"));
    // m_apply_btn->SetBorderColor(wxColour(38, 46, 48));
    m_apply_btn->SetFont(Label::Body_14);
    m_apply_btn->SetSize(wxSize(FromDIP(80), FromDIP(32)));
    m_apply_btn->SetMinSize(wxSize(FromDIP(80), FromDIP(32)));
    m_apply_btn->SetCornerRadius(FromDIP(16));
    m_apply_btn->SetToolTip(wxEmptyString);

    m_canvas_sizer->Add(m_canvas, 0, wxLEFT | wxTOP | wxEXPAND, FromDIP(17));
    m_canvas_sizer->Add(m_canvas_btn_sizer, 0, wxTOP, FromDIP(8));
    m_list_sizer->Add(0, 0, 1, wxEXPAND, 0);

    m_list_sizer->Add(0, 0, 0, wxTOP, FromDIP(27));
    m_list_sizer->Add(all_checkbox_sizer, 0, wxLEFT, FromDIP(0));
    m_list_sizer->Add(m_line, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));
    m_list_sizer->Add(m_list_view, 0, wxEXPAND, 0);
    m_list_sizer->Add(0, 0, 1, wxEXPAND, 0);

    m_dlg_content_sizer->Add(m_canvas_sizer, 0, wxEXPAND, FromDIP(0));
    m_dlg_content_sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, FromDIP(22));
    m_dlg_content_sizer->Add(m_list_sizer, 0, wxRIGHT | wxEXPAND, FromDIP(14));

    m_dlg_btn_sizer->Add(0, 0, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(21));
    m_dlg_btn_sizer->Add(m_cnt_label, 0, wxALIGN_CENTER_VERTICAL, FromDIP(0));
#ifdef __WXMSW__
    m_dlg_btn_sizer->Add(m_tot_label, 0, wxLEFT | wxTOP | wxALIGN_CENTER_VERTICAL, FromDIP(2));
#else
    m_dlg_btn_sizer->Add(m_tot_label, 0, wxALIGN_CENTER_VERTICAL, 0);
#endif
    m_dlg_btn_sizer->Add(0, 0, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL, FromDIP(0));
    m_dlg_btn_sizer->Add(m_apply_btn, 0, wxALIGN_CENTER_VERTICAL, FromDIP(0));
    m_dlg_btn_sizer->Add(0, 0, 0, wxLEFT | wxEXPAND | wxALIGN_CENTER_VERTICAL, FromDIP(24));

    m_dlg_placeholder = new wxPanel(m_book_third_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_dlg_placeholder->SetMinSize(wxSize(-1, FromDIP(15)));
    m_dlg_placeholder->SetMaxSize(wxSize(-1, FromDIP(15)));
    m_dlg_placeholder->SetBackgroundColour(*wxWHITE);

    m_dlg_sizer->Add(m_dlg_content_sizer, 1, wxEXPAND, FromDIP(0));
    // m_dlg_sizer->Add( 0, 0, 1, wxEXPAND, FromDIP(0));
    m_dlg_sizer->Add(m_dlg_btn_sizer, 0, wxEXPAND, FromDIP(0));
    m_dlg_sizer->Add(m_dlg_placeholder, 0, 0, wxEXPAND, 0);

    m_book_third_panel->SetSizer(m_dlg_sizer);
    m_book_third_panel->Layout();
    m_dlg_sizer->Fit(m_book_third_panel);

    // page 2
    m_book_second_panel = new wxPanel(m_simplebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_book_second_panel->SetBackgroundColour(*wxWHITE);
    m_book_second_sizer     = new wxBoxSizer(wxVERTICAL);
    m_book_second_btn_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_retry_bitmap = new wxStaticBitmap(m_book_second_panel, -1, create_scaled_bitmap("partskip_retry", m_book_second_panel, 200), wxDefaultPosition, wxDefaultSize);
    m_retry_label  = new Label(m_book_second_panel, _L("Load skipping objects information failed. Please try again."));
    m_retry_label->Wrap(-1);
    m_retry_label->SetBackgroundColour(*wxWHITE);
    m_book_second_sizer->Add(0, 0, 1, wxEXPAND, 0);
    m_book_second_sizer->Add(m_retry_bitmap, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    m_book_second_sizer->Add(m_retry_label, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    m_book_second_sizer->Add(0, 0, 1, wxEXPAND, 0);

    m_second_retry_btn = new Button(m_book_second_panel, _L("Retry"));
    m_second_retry_btn->SetBackgroundColor(btn_bg_green);
    // m_second_retry_btn->SetBorderColor(wxColour(38, 46, 48));
    m_second_retry_btn->SetTextColor(*wxWHITE);
    m_second_retry_btn->SetFont(Label::Body_14);
    m_second_retry_btn->SetSize(wxSize(FromDIP(80), FromDIP(32)));
    m_second_retry_btn->SetMinSize(wxSize(FromDIP(80), FromDIP(32)));
    m_second_retry_btn->SetCornerRadius(FromDIP(16));
    m_second_retry_btn->Bind(wxEVT_BUTTON, &PartSkipDialog::OnRetryButton, this);
    m_book_second_btn_sizer->Add(m_second_retry_btn, 0, wxALL, FromDIP(24));

    m_book_second_sizer->Add(m_book_second_btn_sizer, 0, wxALIGN_RIGHT, 0);
    m_book_second_panel->SetSizer(m_book_second_sizer);
    m_book_second_panel->Layout();
    m_book_second_sizer->Fit(m_book_second_panel);

    // page 1
    m_book_first_panel = new wxPanel(m_simplebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_book_first_sizer = new wxBoxSizer(wxVERTICAL);
    // m_book_first_sizer->SetMinSize(wxSize(FromDIP(720), FromDIP(500)));

    auto                     m_loading_sizer = new wxBoxSizer(wxHORIZONTAL);
    std::vector<std::string> list{"ams_rfid_1", "ams_rfid_2", "ams_rfid_3", "ams_rfid_4"};
    m_loading_icon = new AnimaIcon(m_book_first_panel, wxID_ANY, list, "refresh_printer", 100);
    m_loading_icon->SetMinSize(wxSize(FromDIP(25), FromDIP(25)));

    m_loading_label = new Label(m_book_first_panel, _L("Loading..."));
    m_loading_label->Wrap(-1);
    m_loading_label->SetBackgroundColour(*wxWHITE);

    m_loading_sizer->Add(m_loading_icon, 0, wxALIGN_CENTER_VERTICAL, FromDIP(0));
    m_loading_sizer->Add(m_loading_label, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    m_book_first_sizer->Add(0, 0, 1, wxEXPAND, 0);
    m_book_first_sizer->Add(m_loading_sizer, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_CENTER_HORIZONTAL, 0);
    m_book_first_sizer->Add(0, 0, 1, wxEXPAND, 0);

    m_book_first_panel->SetSizer(m_book_first_sizer);
    m_book_first_panel->Layout();
    m_book_first_sizer->Fit(m_book_first_panel);

    m_simplebook->AddPage(m_book_first_panel, _("loading page"), false);
    m_simplebook->AddPage(m_book_second_panel, _("retry page"), false);
    m_simplebook->AddPage(m_book_third_panel, _("dialog page"), false);
    m_sizer->Add(m_simplebook, 1, wxEXPAND | wxALL, 5);

    SetSizer(m_sizer);
    m_zoom_in_btn->Bind(wxEVT_BUTTON, &PartSkipDialog::OnZoomIn, this);
    m_zoom_out_btn->Bind(wxEVT_BUTTON, &PartSkipDialog::OnZoomOut, this);
    m_switch_drag_btn->Bind(wxEVT_BUTTON, &PartSkipDialog::OnSwitchDrag, this);
    m_canvas->Bind(EVT_ZOOM_PERCENT, &PartSkipDialog::OnZoomPercent, this);
    m_canvas->Bind(EVT_CANVAS_PART, &PartSkipDialog::UpdatePartsStateFromCanvas, this);

    m_apply_btn->Bind(wxEVT_BUTTON, &PartSkipDialog::OnApplyDialog, this);
    m_all_checkbox->Bind(wxEVT_TOGGLEBUTTON, &PartSkipDialog::OnAllCheckbox, this);

    Layout();
    Fit();
    CentreOnParent();
}

PartSkipDialog::~PartSkipDialog() {}

void PartSkipDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    m_canvas->LoadPickImage(m_local_paths[0]);

    m_loading_icon->SetMinSize(wxSize(FromDIP(25), FromDIP(25)));
    m_retry_bitmap->SetBitmap(create_scaled_bitmap("partskip_retry", m_book_second_panel, 200));

    m_percent_label->SetMinSize(wxSize(FromDIP(56), FromDIP(28)));
    m_percent_label->SetMaxSize(wxSize(FromDIP(56), FromDIP(28)));
    m_zoom_out_btn->SetMinSize(wxSize(FromDIP(56), FromDIP(28)));
    m_zoom_out_btn->SetMaxSize(wxSize(FromDIP(56), FromDIP(28)));
    m_zoom_in_btn->SetMinSize(wxSize(FromDIP(56), FromDIP(28)));
    m_zoom_in_btn->SetMaxSize(wxSize(FromDIP(56), FromDIP(28)));
    m_switch_drag_btn->SetMinSize(wxSize(FromDIP(56), FromDIP(28)));
    m_switch_drag_btn->SetMaxSize(wxSize(FromDIP(56), FromDIP(28)));
    m_percent_label->Rescale();
    m_zoom_out_btn->Rescale();
    m_zoom_in_btn->Rescale();
    m_switch_drag_btn->Rescale();

    m_cnt_label->SetMaxSize(wxSize(-1, FromDIP(20)));

    m_tot_label->SetMinSize(wxSize(FromDIP(200), FromDIP(20)));
    m_tot_label->SetLabel(m_tot_label->GetLabel());
    m_tot_label->Refresh();

    m_line_top->SetMinSize(wxSize(-1, 1));
    m_line->SetMinSize(wxSize(FromDIP(267), 1));
    m_line->SetMaxSize(wxSize(FromDIP(267), 1));

    m_apply_btn->SetMinSize(wxSize(FromDIP(80), FromDIP(32)));
    m_apply_btn->SetCornerRadius(FromDIP(16));
    m_apply_btn->Rescale();

    m_dlg_placeholder->SetMinSize(wxSize(-1, FromDIP(15)));
    m_dlg_placeholder->SetMaxSize(wxSize(-1, FromDIP(15)));

    // m_second_retry_btn->SetSize(wxSize(-1, FromDIP(32)));
    m_second_retry_btn->SetMinSize(wxSize(FromDIP(80), FromDIP(32)));
    m_second_retry_btn->SetCornerRadius(FromDIP(16));
    m_second_retry_btn->Rescale();

    m_all_checkbox->SetMinSize(wxSize(FromDIP(18), FromDIP(18)));
    m_all_checkbox->Rescale();

    for (auto it = m_scroll_sizer->GetChildren().begin(); it != m_scroll_sizer->GetChildren().end(); ++it) {
        wxSizerItem *item = *it;
        if (item && item->IsSizer()) {
            wxSizer *sizer      = item->GetSizer();
            auto     check_item = sizer->GetItem((size_t) 0);
            if (check_item && check_item->IsWindow()) {
                wxWindow *window   = check_item->GetWindow();
                CheckBox *checkbox = dynamic_cast<CheckBox *>(window);
                checkbox->SetMinSize(wxSize(FromDIP(18), FromDIP(18)));
                checkbox->Rescale();
            }
        }
    }
    m_canvas_btn_sizer->SetMinSize(wxSize(-1, FromDIP(28)));
    m_canvas_btn_sizer->Layout();
    m_canvas_sizer->Layout();
    m_dlg_content_sizer->Layout();
    m_dlg_sizer->Layout();

    Fit();
    Layout();
}

std::string PartSkipDialog::create_tmp_path()
{
    boost::filesystem::path parent_path(temporary_dir());

    std::stringstream buf;
    buf << "/bamboo_task/";
    buf << m_timestamp;
    if (m_obj) {
        const auto& dev_id = m_obj->get_dev_id();
        buf << dev_id.substr(0, 3) << "_";
        buf << dev_id.substr(dev_id.length() - 3, 3) << "_";
        buf << m_obj->job_id_ << "/";
    } else {
        buf << 1 << "_" << 1 << "/";
    }
    std::string tmp_path = (parent_path / buf.str()).string();

    if (!std::filesystem::exists(tmp_path + "Metadata/") && !fs::create_directories(tmp_path + "Metadata/")) { wxMessageBox("create file failed."); }
    return tmp_path;
}

bool PartSkipDialog::is_local_file_existed(const std::vector<string> &local_paths)
{
    for (auto path : local_paths) {
        if (!std::filesystem::exists(path)) { return false; }
    }
    return true;
}

void PartSkipDialog::DownloadPartsFile()
{
    BOOST_LOG_TRIVIAL(info) << "part skip: create temp path begin.";
    m_tmp_path = create_tmp_path(); // wxGetApp().app_config->get("download_path");
    BOOST_LOG_TRIVIAL(info) << "part skip: create temp path end.";

    m_local_paths.clear();
    m_target_paths.clear();

    m_plate_idx = m_obj ? m_obj->m_plate_index : -1;
    if (m_plate_idx < 0) {
        m_plate_idx = 1;
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << "part skip: printer plate index is invalid.";
    }

    m_local_paths.push_back(m_tmp_path + "Metadata/pick_" + std::to_string(m_plate_idx) + ".png");
    m_local_paths.push_back(m_tmp_path + "Metadata/model_settings.config");
    m_local_paths.push_back(m_tmp_path + "Metadata/slice_info.config");

    m_target_paths.push_back("Metadata/pick_" + std::to_string(m_plate_idx) + ".png");
    m_target_paths.push_back("Metadata/model_settings.config");
    m_target_paths.push_back("Metadata/slice_info.config");

    if (!is_local_file_existed(m_local_paths)) {
        if (!m_file_sys) {
            m_file_sys = boost::make_shared<PrinterFileSystem>();
            m_file_sys->Attached();
            m_file_sys->Bind(EVT_STATUS_CHANGED, &PartSkipDialog::OnFileSystemEvent, this);
            m_file_sys->Bind(EVT_RAMDOWNLOAD, &PartSkipDialog::OnFileSystemResult, this);
            m_file_sys->Start();
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "part skip: print file system start.";
        } else {
            m_file_sys->Retry();
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "part skip: print file system retry.";
        }
    } else {
        m_file_sys->SendExistedFile();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "part skip: local parts info file is existed.";
    }
}
// actor
void PartSkipDialog::fetchUrl(boost::weak_ptr<PrinterFileSystem> wfs)
{
    boost::shared_ptr fs(wfs.lock());
    if (!fs) return;

    DeviceManager *dm  = GUI::wxGetApp().getDeviceManager();
    MachineObject *obj = dm->get_selected_machine();

    if (obj == nullptr) {
        fs->SetUrl("0");
        return;
    }
    std::string dev_ver = obj->get_ota_version();
    std::string dev_id  = obj->get_dev_id();
    // int         remote_proto = obj->get_file_remote();

    NetworkAgent *agent         = wxGetApp().getAgent();
    std::string   agent_version = agent ? agent->get_version() : "";

    auto url_state = m_url_state;
    if (obj->is_lan_mode_printer()) { url_state = URL_TCP; }

    if (agent) {
        switch (url_state) {
        case URL_TCP: {
            std::string devIP      = obj->get_dev_ip();
            std::string accessCode = obj->get_access_code();
            std::string tcp_url    = "bambu:///local/" + devIP + "?port=6000&user=" + "bblp" + "&passwd=" + accessCode;
            CallAfter([=] {
                boost::shared_ptr fs(wfs.lock());
                if (!fs) return;
                if (boost::algorithm::starts_with(tcp_url, "bambu:///")) {
                    fs->SetUrl(tcp_url);
                } else {
                    fs->SetUrl("3");
                }
            });
            break;
        }
        case URL_TUTK: {
            std::string protocols[] = {"", "\"tutk\"", "\"agora\"", "\"tutk\",\"agora\""};
            agent->get_camera_url(obj->get_dev_id() + "|" + dev_ver + "|" + protocols[3], [this, wfs, m = dev_id, v = agent->get_version(), dv = dev_ver](std::string url)
                {
                if (boost::algorithm::starts_with(url, "bambu:///")) {
                    url += "&device=" + m;
                    url += "&net_ver=" + v;
                    url += "&dev_ver=" + dv;
                    url += "&refresh_url=" + boost::lexical_cast<std::string>(&refresh_agora_url);
                    url += "&cli_id=" + wxGetApp().app_config->get("slicer_uuid");
                    url += "&cli_ver=" + std::string(SLIC3R_VERSION);
                }
                CallAfter([=] {
                    boost::shared_ptr fs(wfs.lock());
                    if (!fs) return;
                    if (boost::algorithm::starts_with(url, "bambu:///")) {
                        fs->SetUrl(url);
                    } else {
                        fs->SetUrl("3");
                    }
                });
            });
            break;
        }
        default: break;
        }
    }
}
// controller
void PartSkipDialog::OnFileSystemEvent(wxCommandEvent &e)
{
    e.Skip();
    auto              wfs = boost::weak_ptr(m_file_sys);
    boost::shared_ptr fs(wfs.lock());
    if (!fs) return;

    wxString msg;
    int      status = e.GetInt();
    // int      extra  = e.GetExtraLong();

    switch (status) {
    case PrinterFileSystem::Initializing:
    case PrinterFileSystem::Connecting: break;
    case PrinterFileSystem::ListSyncing: {
        m_file_sys->GetPickImages(m_local_paths, m_target_paths);
        break;
    }
    case PrinterFileSystem::Failed: {
        m_file_sys->Stop();
        if (m_url_state == URL_TCP) {
            m_url_state = URL_TUTK;
            m_file_sys->SetUrl("3");
            m_file_sys->Retry();
            BOOST_LOG_TRIVIAL(info) << "part skip: print file system connnect failed first.";
        } else {
            m_file_sys->SendConnectFail();
            BOOST_LOG_TRIVIAL(info) << "part skip: print file system connnect failed second.";
        }
        break;
    }
    case PrinterFileSystem::Reconnecting: break;
    }
    if (e.GetInt() == PrinterFileSystem::Initializing) {
        CallAfter([=] {
            boost::shared_ptr fs(wfs.lock());
            if (!fs) return;
            fetchUrl(boost::weak_ptr(fs));
            BOOST_LOG_TRIVIAL(info) << "part skip: fetch url, get parts info files from printer.";
        });
    }
}

// reseter: [TCP -> TUTK(TCP)] -> [TCP -> TUTK(TCP)]
void PartSkipDialog::OnFileSystemResult(wxCommandEvent &event)
{
    int result = event.GetInt();
    m_loading_icon->Stop();
    if (result == 0) {
        InitDialogUI();
        SetSimplebookPage(2);
        m_file_sys->Stop();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "part skip: on file system result success.";
    } else {
        m_url_state = URL_TCP;
        SetSimplebookPage(1);
        m_file_sys->Stop();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "part skip: on file system result failed.";
    }
}

void PartSkipDialog::InitSchedule(MachineObject *obj)
{
    m_obj = obj;
    SetSimplebookPage(0);
    m_loading_icon->Play();
    DownloadPartsFile();
}

void PartSkipDialog::OnRetryButton(wxCommandEvent &event)
{
    event.Skip();
    InitSchedule(m_obj);
    Refresh();
}

bool PartSkipDialog::is_drag_mode() { return m_is_drag == true; }

PartsInfo PartSkipDialog::GetPartsInfo()
{
    PartsInfo parts_info;
    for (auto [part_id, part_state] : this->m_parts_state) { parts_info.push_back(std::pair<int, PartState>(part_id, part_state)); }
    return parts_info;
}

void PartSkipDialog::OnZoomIn(wxCommandEvent &event)
{
    m_canvas->ZoomIn(20);
    UpdateZoomPercent();
}

void PartSkipDialog::OnZoomOut(wxCommandEvent &event)
{
    m_canvas->ZoomOut(20);
    UpdateZoomPercent();
}

void PartSkipDialog::OnSwitchDrag(wxCommandEvent &event)
{
    if (this->is_drag_mode()) {
        m_is_drag = false;
        m_switch_drag_btn->SetBackgroundColor(*wxWHITE);
        m_switch_drag_btn->SetIcon("canvas_drag");
    } else {
        m_is_drag = true;
        m_switch_drag_btn->SetBackgroundColor(wxColour(0, 150, 136));
        m_switch_drag_btn->SetIcon("canvas_drag_active");
    }
    m_canvas->SwitchDrag(m_is_drag);
}

void PartSkipDialog::OnZoomPercent(wxCommandEvent &event)
{
    m_zoom_percent = event.GetInt();
    if (m_zoom_percent >= 1000) {
        m_zoom_percent = 1000;
        m_zoom_in_btn->Enable(false);
        m_zoom_in_btn->SetIcon("canvas_zoom_in_disable");
    } else if (m_zoom_percent <= 100) {
        m_zoom_percent = 100;
        m_zoom_out_btn->Enable(false);
        m_zoom_out_btn->SetIcon("canvas_zoom_out_disable");
    } else {
        m_zoom_in_btn->Enable(true);
        m_zoom_out_btn->Enable(true);
        m_zoom_in_btn->SetIcon("canvas_zoom_in");
        m_zoom_out_btn->SetIcon("canvas_zoom_out");
    }

    UpdateZoomPercent();
}

void PartSkipDialog::UpdatePartsStateFromCanvas(wxCommandEvent &event)
{
    int       part_id    = event.GetExtraLong();
    PartState part_state = PartState(event.GetInt());

    m_parts_state[part_id] = part_state;
    if (part_state == psUnCheck) { m_all_checkbox->SetValue(false); }
    if (IsAllChecked()) { m_all_checkbox->SetValue(true); }

    UpdateApplyButtonStatus();
    UpdateDialogUI();
}

void PartSkipDialog::UpdateZoomPercent() { m_percent_label->SetLabel(wxString::Format("%d%%", m_zoom_percent)); }

void PartSkipDialog::UpdateCountLabel()
{
    int check_cnt = 0;
    int tot_cnt   = 0;
    for (auto [part_id, part_state] : m_parts_state) {
        if (part_state == PartState::psChecked) check_cnt++;
        if (part_state != PartState::psSkipped) tot_cnt++;
    }
    m_cnt_label->SetLabel(wxString::Format("%d", check_cnt));
    m_cnt_label->Fit();
    m_tot_label->SetLabel(wxString::Format(_L("/%d Selected"), tot_cnt));
    m_tot_label->Fit();
    m_dlg_btn_sizer->Layout();
}

bool PartSkipDialog::Show(bool show)
{
    if (show) {
        wxGetApp().UpdateDlgDarkUI(this);
        CentreOnParent();

        Layout();
        Fit();
    }
    return DPIDialog::Show(show);
}

void PartSkipDialog::InitDialogUI()
{
    m_print_lock              = true;
    is_model_support_partskip = false;
    BOOST_LOG_TRIVIAL(info) << "part skip: lock parts info from printer.";
    m_scroll_sizer->Clear(true);
    m_all_checkbox->SetValue(false);
    m_parts_state.clear();
    m_parts_name.clear();

    string pick_img   = m_local_paths[0];
    string slice_info = m_local_paths[2];

    m_switch_drag_btn->SetIcon("canvas_drag");
    m_switch_drag_btn->SetBackgroundColor(*wxWHITE);
    m_is_drag = false;
    m_canvas->SwitchDrag(false);
    m_canvas->SetZoomPercent(100);
    m_canvas->SetOffset(wxPoint(0, 0));

    BOOST_LOG_TRIVIAL(info) << "part skip: load canvas pick image begin.";
    m_canvas->LoadPickImage(pick_img);
    BOOST_LOG_TRIVIAL(info) << "part skip: load canvas pick image end.";
    ModelSettingHelper helper(slice_info);

    if (helper.Parse()) {
        is_model_support_partskip = helper.GetLabelObjectEnabled(m_plate_idx);
        auto parse_result         = helper.GetPlateObjects(m_plate_idx);
        for (const auto &part : parse_result) {
            m_parts_state[part.identify_id] = part.state;
            m_parts_name[part.identify_id]  = part.name;
        }
        if (m_obj) {
            std::vector<int> partskip_ids = m_obj->m_partskip_ids;
            for (auto part_id : partskip_ids) { m_parts_state[part_id] = PartState::psSkipped; }
        }

        for (const auto &[part_id, part_state] : m_parts_state) {
            auto line_sizer = new wxBoxSizer(wxHORIZONTAL);
            auto checkbox   = new CheckBox(m_list_view);
            auto label      = new Label(m_list_view, wxEmptyString);

            checkbox->Bind(
                wxEVT_TOGGLEBUTTON,
                [this, part_id = part_id](wxCommandEvent &event) {
                    m_parts_state[part_id] = event.IsChecked() ? PartState::psChecked : PartState::psUnCheck;
                    if (!event.IsChecked()) {
                        m_all_checkbox->SetValue(false);
                    } else if (IsAllChecked()) {
                        m_all_checkbox->SetValue(true);
                    }
                    m_canvas->UpdatePartsInfo(GetPartsInfo());
                    UpdateCountLabel();
                    UpdateApplyButtonStatus();
                    event.Skip();
                },
                checkbox->GetId());

            if (part_state == PartState::psChecked) {
                checkbox->SetValue(true);
                checkbox->Enable(true);
            } else if (part_state == PartState::psUnCheck) {
                checkbox->SetValue(false);
                checkbox->Enable(true);
            } else if (part_state == PartState::psSkipped) {
                checkbox->SetValue(true);
                checkbox->Enable(false);
            }
            label->SetLabel(wxString::FromUTF8(m_parts_name[part_id]));
            label->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
            label->SetForegroundColour(StateColor::darkModeColorFor(wxColor(107, 107, 107)));
            label->Wrap(-1);
            label->SetMinSize(wxSize(-1, FromDIP(18)));
            checkbox->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
            checkbox->SetMinSize(wxSize(FromDIP(18), FromDIP(18)));
            checkbox->SetMaxSize(wxSize(FromDIP(18), FromDIP(18)));

            line_sizer->Add(checkbox, 0, wxALIGN_CENTER_VERTICAL, 0);
            line_sizer->Add(label, 1, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(8));
            m_scroll_sizer->Add(line_sizer, 0, wxBOTTOM | wxEXPAND, FromDIP(12));
        }
        m_canvas->UpdatePartsInfo(GetPartsInfo());
        BOOST_LOG_TRIVIAL(info) << "part skip: update canvas parts info.";
    }

    m_scroll_sizer->Layout();
    m_list_view->FitInside();
    UpdateApplyButtonStatus();
    UpdateCountLabel();
    Refresh();
    m_print_lock = false;
    BOOST_LOG_TRIVIAL(info) << "part skip: unlock parts info from printer.";
}

void PartSkipDialog::UpdatePartsStateFromPrinter(MachineObject *obj)
{
    if (m_print_lock) {
        BOOST_LOG_TRIVIAL(info) << "part skip: parts info from printer is locked.";
        return;
    }
    m_obj = obj;
    if (m_obj) {
        bool             update_flag  = false;
        std::vector<int> partskip_ids = m_obj->m_partskip_ids;
        for (auto part_id : partskip_ids) {
            if (m_parts_state[part_id] != PartState::psSkipped) {
                m_parts_state[part_id] = PartState::psSkipped;
                update_flag            = true;
            }
        }
        if (update_flag) {
            m_canvas->UpdatePartsInfo(GetPartsInfo());
            UpdateDialogUI();
        }
    }
}

void PartSkipDialog::UpdateDialogUI()
{
    if (m_parts_state.size() != m_scroll_sizer->GetItemCount()) {
        BOOST_LOG_TRIVIAL(warning) << "part skip: m_parts_state and m_scroll_sizer mismatch.";
        return;
    }

    for (auto it = m_parts_state.begin(); it != m_parts_state.end(); ++it) {
        int  idx        = std::distance(m_parts_state.begin(), it);
        auto part_state = it->second;

        wxSizerItem *item = m_scroll_sizer->GetItem(idx);
        if (item && item->IsSizer()) {
            wxSizer *sizer      = item->GetSizer();
            auto     check_item = sizer->GetItem((size_t) 0);

            if (check_item && check_item->IsWindow()) {
                wxWindow *window   = check_item->GetWindow();
                CheckBox *checkbox = dynamic_cast<CheckBox *>(window);
                if (part_state == PartState::psChecked) {
                    checkbox->SetValue(true);
                } else if (part_state == PartState::psUnCheck) {
                    checkbox->SetValue(false);
                } else {
                    checkbox->SetValue(true);
                    checkbox->Enable(false);
                }
            }
        }
    }

    UpdateCountLabel();
    Refresh();
}

void PartSkipDialog::SetSimplebookPage(int page) { m_simplebook->SetSelection(page); }

bool PartSkipDialog::IsAllChecked()
{
    for (auto &[part_id, part_state] : m_parts_state) {
        if (part_state == PartState::psUnCheck) return false;
    }
    return true;
}

bool PartSkipDialog::IsAllCancled()
{
    for (auto &[part_id, part_state] : m_parts_state) {
        if (part_state == PartState::psChecked) return false;
    }
    return true;
}

void PartSkipDialog::OnAllCheckbox(wxCommandEvent &event)
{
    if (m_all_checkbox->GetValue()) {
        for (auto &[part_id, part_state] : m_parts_state) {
            if (part_state == PartState::psUnCheck) part_state = PartState::psChecked;
        }
    } else {
        for (auto &[part_id, part_state] : m_parts_state) {
            if (part_state == PartState::psChecked) part_state = PartState::psUnCheck;
        }
    }
    UpdateApplyButtonStatus();
    m_canvas->UpdatePartsInfo(GetPartsInfo());
    UpdateDialogUI();
    event.Skip();
}

void PartSkipDialog::UpdateApplyButtonStatus()
{
    if (IsAllCancled()) {
        m_apply_btn->SetBackgroundColor(btn_bg_gray);
        m_apply_btn->SetToolTip(_L("Nothing selected"));
        m_enable_apply_btn = false;
    } else if (m_parts_state.size() > 64) {
        m_apply_btn->SetBackgroundColor(btn_bg_gray);
        m_apply_btn->SetToolTip(_L("Over 64 objects in single plate"));
        m_enable_apply_btn = false;
    } else if (!is_model_support_partskip) {
        m_apply_btn->SetBackgroundColor(btn_bg_gray);
        m_apply_btn->SetToolTip(_L("The current print job cannot be skipped"));
        m_enable_apply_btn = false;
    } else {
        m_apply_btn->SetBackgroundColor(btn_bg_green);
        m_apply_btn->SetToolTip(wxEmptyString);
        m_enable_apply_btn = true;
    }
}

void PartSkipDialog::OnApplyDialog(wxCommandEvent &event)
{
    event.Skip();

    if (!m_enable_apply_btn) return;

    m_partskip_ids.clear();
    for (const auto &[part_id, part_state] : m_parts_state) {
        if (part_state == PartState::psChecked) { m_partskip_ids.push_back(part_id); }
    }

    bool all_skipped = true;
    for (auto [part_id, part_state] : m_parts_state) {
        if (part_state == PartState::psUnCheck) all_skipped = false;
    }

    PartSkipConfirmDialog confirm_dialog(this);
    if (all_skipped) {
        confirm_dialog.SetMsgLabel(_L("Skipping all objects."));
        confirm_dialog.SetTipLabel(_L("The printing job will be stopped. Continue?"));
    } else {
        confirm_dialog.SetMsgLabel(wxString::Format(_L("Skipping %d objects."), m_partskip_ids.size()));
        confirm_dialog.SetTipLabel(_L("This action cannot be undone. Continue?"));
    }

    if (confirm_dialog.ShowModal() == wxID_OK) {
        if (m_obj) {
            BOOST_LOG_TRIVIAL(info) << "part skip: skipping " << m_partskip_ids.size() << " objects.";

            if (all_skipped) {
                m_obj->command_task_abort();
                BOOST_LOG_TRIVIAL(info) << "part skip: command skip all parts, abort task.";
            } else if (m_partskip_ids.size() > 0) {
                m_obj->command_task_partskip(m_partskip_ids);
                BOOST_LOG_TRIVIAL(info) << "part skip: command skip " << m_partskip_ids.size() << " parts.";
            }
            EndModal(wxID_OK);
        } else {
            BOOST_LOG_TRIVIAL(warning) << "part skip: machine object is null.";
        }
    }
}

int PartSkipDialog::GetAllSkippedPartsNum()
{
    int skipped_cnt = 0;
    for (auto &[part_id, part_state] : m_parts_state) {
        if (part_state == PartState::psSkipped || part_state == PartState::psChecked) skipped_cnt++;
    }
    return skipped_cnt;
}

PartSkipConfirmDialog::PartSkipConfirmDialog(wxWindow *parent) : DPIDialog(parent, wxID_ANY, _L("Skip Objects"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    std::string icon_path = (boost::format("%1%/images/OrcaSlicerTitle.ico") % Slic3r::resources_dir()).str();
    SetIcon(wxIcon(Slic3r::encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));
    SetBackgroundColour(*wxWHITE);
    SetMinSize(wxSize(FromDIP(480), FromDIP(215)));
    SetSizeHints(wxDefaultSize, wxDefaultSize);

    wxBoxSizer *m_sizer;
    m_sizer = new wxBoxSizer(wxVERTICAL);

    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line_top->SetMinSize(wxSize(FromDIP(480), 1));
    m_line_top->SetMaxSize(wxSize(FromDIP(480), 1));
    m_line_top->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    m_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    m_sizer->Add(0, 0, 0, wxALL, FromDIP(15));

    m_msg_label = new Label(this, _L("Skipping objects."));
    m_msg_label->Wrap(-1);
    m_msg_label->SetBackgroundColour(*wxWHITE);

    m_tip_label = new Label(this, _L("This action cannot be undone. Continue?"));
    m_tip_label->Wrap(-1);
    m_tip_label->SetBackgroundColour(*wxWHITE);
    m_tip_label->SetForegroundColour(wxColor(92, 92, 92));

    m_sizer->Add(m_msg_label, 0, wxLEFT, FromDIP(29));
    m_sizer->Add(0, 0, 0, wxTOP, FromDIP(9));
    m_sizer->Add(m_tip_label, 0, wxLEFT, FromDIP(29));

    wxBoxSizer *m_button_sizer;
    m_button_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_button_sizer->SetMinSize(wxSize(FromDIP(480), FromDIP(54)));
    m_button_sizer->Add(0, 0, 1, wxEXPAND, 0);

    StateColor btn_bg_white(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed), std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                            std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));

    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(27, 136, 68), StateColor::Pressed), std::pair<wxColour, int>(wxColour(61, 203, 115), StateColor::Hovered),
                            std::pair<wxColour, int>(wxColour(0, 177, 66), StateColor::Normal));

    m_apply_button = new Button(this, _L("Continue"));
    m_apply_button->SetBackgroundColor(btn_bg_green);
    m_apply_button->SetTextColor(*wxWHITE);
    // m_apply_button->SetBorderColor(wxColour(38, 46, 48));
    m_apply_button->SetFont(Label::Body_14);
    m_apply_button->SetSize(wxSize(FromDIP(80), FromDIP(32)));
    m_apply_button->SetMinSize(wxSize(FromDIP(80), FromDIP(32)));
    m_apply_button->SetCornerRadius(FromDIP(16));
    m_apply_button->Bind(wxEVT_BUTTON, [this](auto &e) {
        EndModal(wxID_OK);
        e.Skip();
    });

    m_button_sizer->Add(m_apply_button, 0, wxRIGHT | wxBOTTOM, FromDIP(24));
    m_sizer->Add(m_button_sizer, 1, wxEXPAND, FromDIP(5));
    m_sizer->Fit(this);

    SetSizer(m_sizer);
    Layout();
    Fit();
}

PartSkipConfirmDialog::~PartSkipConfirmDialog() {}

bool PartSkipConfirmDialog::Show(bool show)
{
    if (show) {
        wxGetApp().UpdateDlgDarkUI(this);
        CentreOnParent();

        Layout();
        Fit();
    }
    return DPIDialog::Show(show);
}

void PartSkipConfirmDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    m_apply_button->SetMinSize(wxSize(FromDIP(80), FromDIP(32)));
    m_apply_button->SetCornerRadius(FromDIP(16));
    m_apply_button->Rescale();
    Layout();
    Fit();
}

Button *PartSkipConfirmDialog::GetConfirmButton() { return m_apply_button; }

void PartSkipConfirmDialog::SetMsgLabel(wxString msg) { m_msg_label->SetLabel(msg); }

void PartSkipConfirmDialog::SetTipLabel(wxString msg) { m_tip_label->SetLabel(msg); }

}} // namespace Slic3r::GUI
