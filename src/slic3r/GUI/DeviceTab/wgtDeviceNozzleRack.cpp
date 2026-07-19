//**********************************************************/
/* File: wgtDeviceNozzleRack.cpp
*  Description: The Device-tab panel with the toolhead nozzle and the H2C induction hotend rack.
*
*  \n class wgtDeviceNozzleRack;
*  \n class wgtDeviceNozzleRackToolHead;
*  \n class wgtDeviceNozzleRackArea;
*  \n class wgtDeviceNozzleRackPos;
*
*  The wgtDeviceNozzleRackNozzleItem tile and the EVT_NOZZLE_RACK_NOZZLE_ITEM_SELECTED event live in
*  wgtDeviceNozzleRackNozzleItem.{h,cpp}, so they are not redefined here.
//**********************************************************/

#include "wgtDeviceNozzleRack.h"
#include "wgtDeviceNozzleRackUpdate.h"

#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"

#define WX_DIP_SIZE(x, y) wxSize(FromDIP(x), FromDIP(y))

#define L_RAW_A_STR _L("Row A")
#define L_RAW_B_STR _L("Row B")

// Orca: StateColor lacks these grey constants, so mirror the values here.
static const wxColour WGT_GREY200 = wxColour(248, 248, 248);
static const wxColour WGT_GREY300 = wxColour(238, 238, 238);

static wxColour s_hgreen_clr("#009688");

namespace Slic3r::GUI
{

// Tints the "yellow" template pixels of a nozzle bitmap with the loaded filament colour.
// Duplicates the file-local helper of the same purpose in wgtDeviceNozzleRackNozzleItem.cpp.
static wxBitmap SetNozzleBmpColor(const wxBitmap& bmp, const std::string& color_str) {
    if(color_str.empty()) return bmp;

    wxImage img = bmp.ConvertToImage();
    wxColour color("#" + color_str);

    for (int y = 0; y < img.GetHeight(); ++y) {
        for (int x = 0; x < img.GetWidth(); ++x) {
            unsigned char r = img.GetRed(x, y);
            unsigned char g = img.GetGreen(x, y);
            unsigned char b = img.GetBlue(x, y);

            /*replace yellow with color*/
            if ( r >= 180 && g >= 180 && b <= 150) {
                img.SetRGB(x, y, color.Red(), color.Green(), color.Blue());
            }
        }
    }

    return wxBitmap(img, -1, bmp.GetScaleFactor());
}

// Orca: StateColor has no gray button style, so define one file-local. This panel is its only
// consumer, so keeping it here avoids touching the shared StateColor widget.
static StateColor s_button_style_gray()
{
    return StateColor(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
                      std::pair<wxColour, int>(*wxWHITE, StateColor::Focused),
                      std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                      std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
}

wgtDeviceNozzleRack::wgtDeviceNozzleRack(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : wxPanel(parent, id, pos, size, style)
{
    CreateGui();
}

void wgtDeviceNozzleRack::CreateGui()
{
    m_toolhead_panel = new wgtDeviceNozzleRackToolHead(this);
    m_rack_area = new wgtDeviceNozzleRackArea(this);

    wxPanel* separator = new wxPanel(this);
    separator->SetMaxSize(wxSize(FromDIP(1), -1));
    separator->SetMinSize(wxSize(FromDIP(1), -1));
    separator->SetBackgroundColour(WGT_GREY300);

    wxSizer* main_sizer = new wxBoxSizer(wxHORIZONTAL);
    main_sizer->AddStretchSpacer();
    main_sizer->Add(m_toolhead_panel, 0, wxEXPAND);
    main_sizer->Add(separator, 0, wxEXPAND);
    main_sizer->Add(m_rack_area, 0, wxEXPAND);
    main_sizer->AddStretchSpacer();

    SetSizer(main_sizer);
    SetMaxSize(WX_DIP_SIZE(586, -1));
    SetMinSize(WX_DIP_SIZE(586, -1));
    SetSize(WX_DIP_SIZE(586, -1));
    Layout();

    wxGetApp().UpdateDarkUIWin(this);
}

void wgtDeviceNozzleRack::UpdateRackInfo(std::shared_ptr<DevNozzleRack> rack)
{
    if (!rack->IsSupported()) { return; }

    m_nozzle_rack = rack;
    if (m_nozzle_rack.expired()) { return; }

    DevNozzleSystem* nozzle_system = m_nozzle_rack.lock()->GetNozzleSystem();
    if (nozzle_system)
    {
        m_toolhead_panel->UpdateToolHeadInfo(nozzle_system->GetExtNozzle(MAIN_EXTRUDER_ID));
        m_rack_area->UpdateRackInfo(m_nozzle_rack);
    }
}

void wgtDeviceNozzleRack::Rescale()
{
    m_toolhead_panel->Rescale();
    m_rack_area->Rescale();
    Layout();
}

class wgtDeviceNozzleRackTitle : public StaticBox
{
public:
    wgtDeviceNozzleRackTitle(wxWindow* parent, const wxString& title) : StaticBox(parent)
    {
        SetBackgroundColour(WGT_GREY200);
        SetBorderColor(*wxWHITE);
        SetCornerRadius(0);

        m_title_label = new Label(this, title);
        m_title_label->SetFont(Label::Body_14);
        m_title_label->SetBackgroundColour(WGT_GREY200);

        wxSizer* title_sizer = new wxBoxSizer(wxHORIZONTAL);
        title_sizer->AddStretchSpacer();
        title_sizer->Add(m_title_label, 0, wxEXPAND | wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(5));
        title_sizer->AddStretchSpacer();
        SetSizer(title_sizer);
    };

public:
    void SetLabel(const wxString& new_label) { m_title_label->SetLabel(new_label); }

private:
    Label* m_title_label;
};


void wgtDeviceNozzleRackToolHead::CreateGui()
{
    wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);

    // Create Header
    wgtDeviceNozzleRackTitle* title_box = new wgtDeviceNozzleRackTitle(this, _L("Toolhead"));
    mainSizer->Add(title_box, 0, wxEXPAND | wxTOP);
    mainSizer->AddStretchSpacer();

    // Image
    m_extruder_nozzle_empty = new ScalableBitmap(this, "dev_rack_toolhead_empty", 98);
    m_extruder_nozzle_normal = new ScalableBitmap(this, "dev_rack_toolhead_normal", 98);
    m_toolhead_icon = new wxStaticBitmap(this, wxID_ANY, m_extruder_nozzle_empty->bmp(), wxDefaultPosition, WX_DIP_SIZE(98, 98));
    mainSizer->Add(m_toolhead_icon, 0, wxALIGN_CENTRE_HORIZONTAL | wxTOP, FromDIP(20));

    // Nozzle info
    m_nozzle_diamenter_label = new Label(this);
    m_nozzle_diamenter_label->SetFont(Label::Body_13);
    m_nozzle_diamenter_label->SetBackgroundColour(*wxWHITE);
    mainSizer->Add(m_nozzle_diamenter_label, 0, wxALIGN_CENTRE_HORIZONTAL | wxBOTTOM | wxTOP, FromDIP(5));

    m_nozzle_flowtype_label = new Label(this);
    m_nozzle_flowtype_label->SetFont(Label::Body_13);
    m_nozzle_flowtype_label->SetBackgroundColour(*wxWHITE);
    mainSizer->Add(m_nozzle_flowtype_label, 0, wxALIGN_CENTRE_HORIZONTAL);
    mainSizer->AddStretchSpacer();

    // Set sizer
    SetSizer(mainSizer);
    SetMaxSize(WX_DIP_SIZE(132, -1));
    SetMinSize(WX_DIP_SIZE(132, -1));
    SetSize(WX_DIP_SIZE(132, -1));
}

void wgtDeviceNozzleRackToolHead::UpdateToolHeadInfo(const DevNozzle& extruder_nozzle)
{
    /* Labels */
    if (extruder_nozzle.IsEmpty())
    {
        m_nozzle_diamenter_label->Show(false);
        m_nozzle_flowtype_label->SetLabel(_L("Empty"));
    }
    else if (extruder_nozzle.IsUnknown())
    {
        m_nozzle_diamenter_label->Show(false);
        m_nozzle_flowtype_label->SetLabel(_L("Unknown"));
    }
    else if (extruder_nozzle.IsAbnormal())
    {
        m_nozzle_diamenter_label->Show(false);
        m_nozzle_flowtype_label->SetLabel(_L("Error"));
    }
    else /*extruder_nozzle.IsNormal()*/
    {
        m_nozzle_diamenter_label->Show(true);
        m_nozzle_diamenter_label->SetLabel(extruder_nozzle.GetNozzleDiameterStr());
        m_nozzle_flowtype_label->SetLabel(extruder_nozzle.GetNozzleFlowTypeStr());
    }

    /* Icon*/
    bool extruder_exist = !extruder_nozzle.IsEmpty();
    if (m_extruder_nozzle_exist != extruder_exist)
    {
        m_extruder_nozzle_exist = extruder_exist;
        m_filament_color = extruder_nozzle.GetFilamentColor();
        m_toolhead_icon->SetBitmap(m_extruder_nozzle_exist ? SetNozzleBmpColor(m_extruder_nozzle_normal->bmp(), m_filament_color) : m_extruder_nozzle_empty->bmp());
        m_toolhead_icon->Refresh();
    }
}

void wgtDeviceNozzleRackToolHead::Rescale()
{
    m_extruder_nozzle_normal->msw_rescale();
    m_extruder_nozzle_empty->msw_rescale();
    m_toolhead_icon->SetBitmap(m_extruder_nozzle_exist ? SetNozzleBmpColor(m_extruder_nozzle_normal->bmp(), m_filament_color) : m_extruder_nozzle_empty->bmp());

    Layout();
    Refresh();
}

void wgtDeviceNozzleRackArea::CreateGui()
{
    wxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    // Create Header
    m_title_nozzle_rack = new wgtDeviceNozzleRackTitle(this, _L("Induction Hotend Rack"));
    main_sizer->Add(m_title_nozzle_rack, 0, wxEXPAND | wxTOP);

    // Create Simple Book
    m_simple_book = new wxSimplebook(this, wxID_ANY);

    wxSizer* content_sizer = new wxBoxSizer(wxVERTICAL);

    m_panel_content = new wxPanel(m_simple_book, wxID_ANY);
    m_panel_refresh = new wxPanel(m_simple_book, wxID_ANY);

    // Create Hotends ans Rack Position Panel
    wxSizer* hotends_rack_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Hotends
    m_hotends_sizer = new wxBoxSizer(wxVERTICAL);
    m_arow_nozzles_box = CreateNozzleBox( { 0, 2, 4});
    m_brow_nozzles_box = CreateNozzleBox( { 1, 3, 5});
    m_hotends_sizer->Add(m_arow_nozzles_box);
    m_hotends_sizer->Add(m_brow_nozzles_box);
    hotends_rack_sizer->Add(m_hotends_sizer, 0, wxLEFT, FromDIP(8));

    // Rack
    m_rack_pos_panel = new wgtDeviceNozzleRackPos(m_panel_content);
    hotends_rack_sizer->Add(m_rack_pos_panel, 0, wxEXPAND);
    content_sizer->Add(hotends_rack_sizer, 0);

    wxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_hotends_infos = new Button(m_panel_content, _L("Hotends Info"));
    m_btn_hotends_infos->SetFont(Label::Body_12);
    m_btn_hotends_infos->SetBackgroundColor(s_button_style_gray());
    m_btn_hotends_infos->SetBackgroundColour(*wxWHITE);
    m_btn_hotends_infos->Bind(wxEVT_BUTTON, &wgtDeviceNozzleRackArea::OnBtnHotendsInfos, this);

    m_btn_read_all = new Button(m_panel_content, _L("Read All"));
    m_btn_read_all->SetFont(Label::Body_12);
    m_btn_read_all->SetBackgroundColor(s_button_style_gray());
    m_btn_read_all->SetBackgroundColour(*wxWHITE);
    m_btn_read_all->Bind(wxEVT_BUTTON, &wgtDeviceNozzleRackArea::OnBtnReadAll, this);

    btn_sizer->Add(m_btn_hotends_infos, 0, wxLEFT);
    btn_sizer->Add(m_btn_read_all, 0, wxLEFT, FromDIP(5));
    content_sizer->Add(btn_sizer, 0, wxLEFT, FromDIP(10));

    /* refresh panel */
    wxSizer* refresh_sizer = CreateRefreshBook(m_panel_refresh);

    m_panel_content->SetSizer(content_sizer);
    m_panel_refresh->SetSizer(refresh_sizer);
    m_simple_book->AddPage(m_panel_content, "Content");
    m_simple_book->AddPage(m_panel_refresh, "Refresh");
    main_sizer->Add(m_simple_book, 1, wxEXPAND);

    m_simple_book->SetSelection(0);

    SetSizer(main_sizer);
    Layout();
    Fit();
}

wxSizer* wgtDeviceNozzleRackArea::CreateRefreshBook(wxPanel* parent)
{
    wxSizer* refresh_sizer = new wxBoxSizer(wxVERTICAL);

    std::vector<std::string> list{"ams_rfid_1", "ams_rfid_2", "ams_rfid_3", "ams_rfid_4"};
    m_refresh_icon = new AnimaIcon(parent, wxID_ANY, list, "refresh_printer", 100);
    m_refresh_icon->SetMinSize(wxSize(FromDIP(25), FromDIP(25)));

    wxSizer* progress_sizer = new wxBoxSizer(wxHORIZONTAL);

    Label* progress_prefix = new Label(parent, _L("Reading "));
    progress_prefix->SetBackgroundColour(*wxWHITE);
    m_progress_refresh = new Label(parent, "(1/6)");
    m_progress_refresh->SetFont(Label::Body_14);
    m_progress_refresh->SetBackgroundColour(*wxWHITE);
    m_progress_refresh->SetForegroundColour(*wxGREEN);
    Label* progress_suffix = new Label(parent, " ...");
    progress_suffix->SetBackgroundColour(*wxWHITE);

    progress_sizer->Add(progress_prefix, 0, wxLEFT);
    progress_sizer->Add(m_progress_refresh, 0, wxLEFT);
    progress_sizer->Add(progress_suffix, 0, wxLEFT);

    Label* refresh_tip = new Label(parent, _L("Please wait"));
    refresh_tip->SetBackgroundColour(*wxWHITE);

    refresh_sizer->Add(0, 0, 1, wxEXPAND, 0);
    refresh_sizer->Add(m_refresh_icon, 0, wxALIGN_CENTER_HORIZONTAL, 0);
    refresh_sizer->Add(progress_sizer, 0, wxALIGN_CENTER_HORIZONTAL, FromDIP(0));
    refresh_sizer->Add(refresh_tip, 0, wxALIGN_CENTER_HORIZONTAL, FromDIP(0));
    refresh_sizer->Add(0, 0, 1, wxEXPAND, 0);

    return refresh_sizer;
}

StaticBox* wgtDeviceNozzleRackArea::CreateNozzleBox(const std::vector<int> nozzle_idxes)
{
    StaticBox* nozzle_box = new StaticBox(m_panel_content);
    nozzle_box->SetBackgroundColor(*wxWHITE);
    nozzle_box->SetBorderColor(*wxWHITE);
    nozzle_box->SetCornerRadius(0);

    wxSizer* h_sizer = new wxBoxSizer(wxHORIZONTAL);
    for (auto start_idx : nozzle_idxes)
    {
        wgtDeviceNozzleRackNozzleItem* nozzle_item = new wgtDeviceNozzleRackNozzleItem(nozzle_box, start_idx);
        m_nozzle_items[start_idx] = nozzle_item;
        h_sizer->Add(nozzle_item, 0, wxALL, FromDIP(8));
    }

    nozzle_box->SetSizer(h_sizer);
    return nozzle_box;
}

void wgtDeviceNozzleRackArea::UpdateNozzleItems(const std::unordered_map<int, wgtDeviceNozzleRackNozzleItem*>& nozzle_items,
    std::shared_ptr<DevNozzleRack> nozzle_rack)
{
    for (auto iter : nozzle_items)
    {
        iter.second->Update(nozzle_rack);
    }

    /*update nozzle possition and background*/
    if (nozzle_rack->GetReadingCount() != 0)
    {
        m_progress_refresh->SetLabel(wxString::Format("(%d/%d)", nozzle_rack->GetReadingIdx(), nozzle_rack->GetReadingCount()));
        if(!m_refresh_icon->IsPlaying()) {
            m_simple_book->SetSelection(1);
            m_refresh_icon->Play();
        }
        return;
    } else{
        m_refresh_icon->Stop();
        m_simple_book->SetSelection(0);
    }

    const DevNozzleRack::RackPos new_pos = nozzle_rack->GetPosition();
    const DevNozzleRack::RackStatus new_status = nozzle_rack->GetStatus();
    if (m_rack_pos != new_pos || m_rack_status != new_status)
    {
        m_rack_pos = new_pos;
        m_rack_status = new_status;
        if (m_rack_status == DevNozzleRack::RACK_STATUS_IDLE)
        {
            m_hotends_sizer->Clear();
            if (m_rack_pos == DevNozzleRack::RACK_POS_B_TOP)
            {
                m_hotends_sizer->Add(m_brow_nozzles_box);
                m_hotends_sizer->Add(m_arow_nozzles_box);
            }
            else if (m_rack_pos == DevNozzleRack::RACK_POS_A_TOP)
            {
                m_hotends_sizer->Add(m_arow_nozzles_box);
                m_hotends_sizer->Add(m_brow_nozzles_box);
            }
            else
            {
                m_hotends_sizer->Add(m_arow_nozzles_box);
                m_hotends_sizer->Add(m_brow_nozzles_box);
            }
        }
    }
}

void wgtDeviceNozzleRackArea::UpdateRackInfo(std::weak_ptr<DevNozzleRack> rack)
{
    m_nozzle_rack = rack;
    const auto& nozzle_rack = rack.lock();
    if (nozzle_rack)
    {
        UpdateNozzleItems(m_nozzle_items, nozzle_rack);
        m_rack_pos_panel->UpdateRackPos(nozzle_rack);
        m_btn_read_all->Enable(nozzle_rack->CtrlCanReadAll());
    }

    if (m_rack_upgrade_dlg && m_rack_upgrade_dlg->IsShown())
    {
        m_rack_upgrade_dlg->UpdateRackInfo(nozzle_rack);
    }
};

void wgtDeviceNozzleRackArea::OnBtnHotendsInfos(wxCommandEvent& evt)
{
    const auto& nozzle_rack = m_nozzle_rack.lock();
    if (nozzle_rack)
    {
        m_rack_upgrade_dlg = new wgtDeviceNozzleRackUpgradeDlg((wxWindow*)wxGetApp().mainframe, nozzle_rack);
        m_rack_upgrade_dlg->ShowModal();

        delete m_rack_upgrade_dlg;
        m_rack_upgrade_dlg = nullptr;
    }

    evt.Skip();
}

void wgtDeviceNozzleRackArea::OnBtnReadAll(wxCommandEvent& evt)
{
    if (const auto nozzle_rack = m_nozzle_rack.lock())
    {
        nozzle_rack->CtrlRackReadAll(true);
    }

    evt.Skip();
}

void wgtDeviceNozzleRackArea::Rescale()
{
    for (auto item : m_nozzle_items)
    {
        item.second->Rescale();
    }

    m_rack_pos_panel->Rescale();
    m_btn_hotends_infos->Rescale();
    m_btn_read_all->Rescale();
}

static void s_set_bg_style(StaticBox* box,
    ScalableButton* btn,
    Label* label_row,
    Label* label_row_status,
    const wxColour& clr)
{
    box->SetBorderColor(clr);
    box->SetBackgroundColor(clr);
    btn->SetBackgroundColour(clr);
    label_row->SetBackgroundColour(clr);
    label_row_status->SetBackgroundColour(clr);
}

void wgtDeviceNozzleRackPos::CreateGui()
{
    // RowA
    m_rowup_panel = new StaticBox(this, wxID_ANY);
    m_rowup_panel->SetCornerRadius(0);

    wxBoxSizer* rowa_sizer = new wxBoxSizer(wxVERTICAL);
    rowa_sizer->AddStretchSpacer();
    m_btn_rowup = new ScalableButton(m_rowup_panel, wxID_ANY, "dev_rack_row_up", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, false, 25);
    m_btn_rowup->Bind(wxEVT_ENTER_WINDOW, [this](auto&) { SetCursor(wxCURSOR_HAND); });
    m_btn_rowup->Bind(wxEVT_LEAVE_WINDOW, [this](auto&) { SetCursor(wxCURSOR_ARROW); });
    m_btn_rowup->Bind(wxEVT_BUTTON, &wgtDeviceNozzleRackPos::OnMoveRackUp, this);
    rowa_sizer->Add(m_btn_rowup, 0, wxALIGN_CENTER | wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    m_label_rowup_status = new Label(m_rowup_panel);
    m_label_rowup_status->SetFont(Label::Body_12);
    m_label_rowup_status->Show(false);
    rowa_sizer->Add(m_label_rowup_status, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(10));

    m_label_rowup = new Label(m_rowup_panel);
    m_label_rowup->SetFont(Label::Body_14);
    rowa_sizer->Add(m_label_rowup, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(10));
    rowa_sizer->AddStretchSpacer();

    m_rowup_panel->SetSizer(rowa_sizer);

    // homing
    m_btn_homing = new ScalableButton(this, wxID_ANY, "dev_rack_home", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, false, 25);
    m_btn_homing->SetBackgroundColour(WGT_GREY200);
    m_btn_homing->Bind(wxEVT_ENTER_WINDOW, [this](auto&) { SetCursor(wxCURSOR_HAND); });
    m_btn_homing->Bind(wxEVT_LEAVE_WINDOW, [this](auto&) { SetCursor(wxCURSOR_ARROW); });
    m_btn_homing->Bind(wxEVT_BUTTON, &wgtDeviceNozzleRackPos::OnBtnHomingRack, this);

    // Row B
    m_rowbottom_panel = new StaticBox(this, wxID_ANY);
    m_rowbottom_panel->SetCornerRadius(0);

    wxBoxSizer* rowb_sizer = new wxBoxSizer(wxVERTICAL);
    rowb_sizer->AddStretchSpacer();

    m_btn_rowbottom_up = new ScalableButton(m_rowbottom_panel, wxID_ANY, "dev_rack_row_up", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, false, 25);
    m_btn_rowbottom_up->Bind(wxEVT_BUTTON, &wgtDeviceNozzleRackPos::OnMoveRackDown, this);
    m_btn_rowbottom_up->Bind(wxEVT_ENTER_WINDOW, [this](auto&) { SetCursor(wxCURSOR_HAND); });
    m_btn_rowbottom_up->Bind(wxEVT_LEAVE_WINDOW, [this](auto&) { SetCursor(wxCURSOR_ARROW); });
    rowb_sizer->Add(m_btn_rowbottom_up, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(10));

    m_label_rowbottom_status = new Label(m_rowbottom_panel);
    m_label_rowbottom_status->SetFont(Label::Body_12);
    m_label_rowbottom_status->Show(false);
    rowb_sizer->Add(m_label_rowbottom_status, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(10));

    m_label_rowbottom = new Label(m_rowbottom_panel);
    m_label_rowbottom->SetFont(Label::Body_14);
    rowb_sizer->Add(m_label_rowbottom, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(10));
    rowb_sizer->AddStretchSpacer();

    m_rowbottom_panel->SetSizer(rowb_sizer);

    // bg style
    SetBackgroundColour(*wxWHITE);
    s_set_bg_style(m_rowup_panel, m_btn_rowup, m_label_rowup, m_label_rowup_status, *wxWHITE);
    s_set_bg_style(m_rowbottom_panel, m_btn_rowbottom_up, m_label_rowbottom, m_label_rowbottom_status, *wxWHITE);

    // main sizer
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_rowup_panel, 1, wxALIGN_TOP | wxEXPAND | wxALIGN_CENTER);
    main_sizer->Add(m_btn_homing, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(10));
    main_sizer->Add(m_rowbottom_panel, 1, wxALIGN_BOTTOM | wxEXPAND | wxALIGN_CENTER);
    SetSizer(main_sizer);

    SetMinSize(WX_DIP_SIZE(85, -1));

    Layout();
    Fit();
}

void wgtDeviceNozzleRackPos::UpdateRackPos(const std::shared_ptr<DevNozzleRack>& rack)
{
    m_rack = rack;
    if (rack)
    {
        UpdateRackPos(rack->GetPosition(), rack->GetStatus(), rack->GetReadingCount() > 0);
    }
}

static void s_show_label(Label* label, const wxString& text)
{
    label->SetLabel(text);
    label->Show();
}

static void s_show_label(Label* label, const wxString& text, const wxColour& text_color)
{
    label->SetLabel(text);
    label->SetForegroundColour(StateColor::darkModeColorFor(text_color));
    label->Show();
}

void wgtDeviceNozzleRackPos::UpdateRackPos(DevNozzleRack::RackPos new_pos,
    DevNozzleRack::RackStatus new_status, bool is_reading)
{
    // While reading, both rows show a "Running..." status and the move buttons are hidden.
    if (is_reading)
    {
        s_show_label(m_label_rowup, L_RAW_A_STR, *wxBLACK);
        s_show_label(m_label_rowup_status, _L("Running..."));

        s_show_label(m_label_rowbottom, L_RAW_B_STR, *wxBLACK);
        s_show_label(m_label_rowbottom_status, _L("Running..."));

        m_btn_rowup->Show(false);
        m_btn_rowbottom_up->Show(false);

        m_rack_pos = DevNozzleRack::RACK_POS_UNKNOWN;
        m_rack_status = DevNozzleRack::RACK_STATUS_UNKNOWN;
        return;
    }

    if (new_pos != m_rack_pos || m_rack_status != new_status)
    {
        m_rack_pos = new_pos;
        m_rack_status = new_status;

        if (m_rack_status != DevNozzleRack::RACK_STATUS_IDLE)
        {
            s_show_label(m_label_rowup, L_RAW_A_STR, *wxBLACK);
            s_show_label(m_label_rowup_status, _L("Running..."));

            s_show_label(m_label_rowbottom, L_RAW_B_STR, *wxBLACK);
            s_show_label(m_label_rowbottom_status, _L("Running..."));

            m_btn_rowup->Show(false);
            m_btn_rowbottom_up->Show(false);
        }
        else
        {
            if (new_pos == DevNozzleRack::RACK_POS_A_TOP)
            {
                s_show_label(m_label_rowup, L_RAW_A_STR, s_hgreen_clr);
                s_show_label(m_label_rowup_status, _L("Raised"));

                m_rowbottom_panel->SetBorderColor(*wxWHITE);
                m_rowbottom_panel->SetBackgroundColor(*wxWHITE);
                s_show_label(m_label_rowbottom, L_RAW_B_STR, *wxBLACK);
                m_label_rowbottom_status->Show(false);

                m_btn_rowup->Show(false);
                m_btn_rowbottom_up->Show(true);
            }
            else if (new_pos == DevNozzleRack::RACK_POS_B_TOP)
            {
                s_show_label(m_label_rowup, L_RAW_B_STR, s_hgreen_clr);
                s_show_label(m_label_rowup_status, _L("Raised"));
                s_show_label(m_label_rowbottom, L_RAW_A_STR, *wxBLACK);
                m_label_rowbottom_status->Show(false);

                m_btn_rowup->Show(false);
                m_btn_rowbottom_up->Show(true);
            }
            else
            {
                s_show_label(m_label_rowup, L_RAW_A_STR, *wxBLACK);
                m_label_rowup_status->Show(false);

                s_show_label(m_label_rowbottom, L_RAW_B_STR, *wxBLACK);
                m_label_rowbottom_status->Show(false);

                m_btn_rowup->Show(true);
                m_btn_rowbottom_up->Show(true);
            }
        }

        Layout();
        Refresh();
    }
};

void wgtDeviceNozzleRackPos::OnMoveRackUp(wxCommandEvent& evt)
{
    auto rack = m_rack.lock();
    if (rack)
    {
        if (m_label_rowup->GetLabel() == L_RAW_A_STR)
        {
            rack->CtrlRackPosMove(DevNozzleRack::RACK_POS_A_TOP);
        }
        else if (m_label_rowup->GetLabel() == L_RAW_B_STR)
        {
            rack->CtrlRackPosMove(DevNozzleRack::RACK_POS_B_TOP);
        }
    }
    evt.Skip();
}

void wgtDeviceNozzleRackPos::OnMoveRackDown(wxCommandEvent& evt)
{
    auto rack = m_rack.lock();
    if (rack)
    {
        if (m_label_rowbottom->GetLabel() == L_RAW_A_STR)
        {
            rack->CtrlRackPosMove(DevNozzleRack::RACK_POS_A_TOP);
        }
        else if (m_label_rowbottom->GetLabel() == L_RAW_B_STR)
        {
            rack->CtrlRackPosMove(DevNozzleRack::RACK_POS_B_TOP);
        }
    }
    evt.Skip();
}

void wgtDeviceNozzleRackPos::OnBtnHomingRack(wxCommandEvent& evt)
{
    if (auto rack = m_rack.lock())
    {
        rack->CtrlRackPosGoHome();
    }
    evt.Skip();
}

void wgtDeviceNozzleRackPos::Rescale()
{
    m_btn_rowup->msw_rescale();
    m_btn_rowbottom_up->msw_rescale();
    m_btn_homing->msw_rescale();
}

};// end of namespace Slic3r::GUI
