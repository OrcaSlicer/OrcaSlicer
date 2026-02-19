//**********************************************************/
/* File: wgtDeviceNozzleRack.cpp
*  Description: The panel with rack info
*
*  \n class wgtDeviceNozzleRackArea;
*  \n class wgtDeviceNozzleRackNozzleItem;
*  \n class wgtDeviceNozzleRackToolHead;
*  \n class wgtDeviceNozzleRackPos;
//**********************************************************/

#include "wgtDeviceNozzleRack.h"
#include "wgtDeviceNozzleRackUpdate.h"

#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"

#include <unordered_set>

#define WX_DIP_SIZE_18 wxSize(FromDIP(18), FromDIP(18))
#define WX_DIP_SIZE_46 wxSize(FromDIP(46), FromDIP(46))
#define WX_DIP_SIZE(x, y) wxSize(FromDIP(x), FromDIP(y))

#define WGT_RACK_NOZZLE_SIZE WX_DIP_SIZE(88, 100)

#define L_RAW_A_STR _L("Row A")
#define L_RAW_B_STR _L("Row B")

static wxColour s_gray_clr("#B0B0B0");
static wxColour s_hgreen_clr("#00AE42");
static wxColour s_red_clr("#D01B1B");

static std::vector<int> a_nozzle_seq = { 0, 2, 4, 1, 3, 5 };
static std::vector<int> b_nozzle_seq = { 1, 3, 5, 0, 2, 4 };

wxDEFINE_EVENT(EVT_NOZZLE_RACK_NOZZLE_ITEM_SELECTED, wxCommandEvent);

namespace Slic3r::GUI
{

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
    separator->SetBackgroundColour(WXCOLOUR_GREY300);

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
        SetBackgroundColour(WXCOLOUR_GREY200);
        SetBorderColor(*wxWHITE);
        SetCornerRadius(0);

        m_title_label = new Label(this, title);
        m_title_label->SetFont(Label::Body_14);
        m_title_label->SetBackgroundColour(WXCOLOUR_GREY200);

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
    m_btn_hotends_infos->SetBackgroundColor(StateColor::createButtonStyleGray());
    m_btn_hotends_infos->SetBackgroundColour(*wxWHITE);
    m_btn_hotends_infos->Bind(wxEVT_BUTTON, &wgtDeviceNozzleRackArea::OnBtnHotendsInfos, this);

    m_btn_read_all = new Button(m_panel_content, _L("Read All"));
    m_btn_read_all->SetFont(Label::Body_12);
    m_btn_read_all->SetBackgroundColor(StateColor::createButtonStyleGray());
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

static void s_update_title(const std::shared_ptr<DevNozzleRack> rack, wgtDeviceNozzleRackTitle* title_label)
{
    wxString title = _L("Induction Hotend Rack");
    if (rack && (rack->GetReadingCount() > 0))
    {
        wxString pending = ": " + _L("Reading") + wxString::Format(" %d/%d", rack->GetReadingIdx(), rack->GetReadingCount());
        title += pending;
    }

    title_label->SetLabel(title);
}

#if 0
static void s_update_readall_btn(const std::shared_ptr<DevNozzleRack> rack, Button* btn)
{
    wxString label = _L("Read All");
    if (rack && rack->HasUnreliableNozzles())
    {
        label += "*";
    }

    if (btn->GetLabel() != label)
    {
        btn->SetLabel(label);
    }

    btn->Enable(rack->CtrlCanReadAll());
};
#endif

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
        // s_update_title(nozzle_rack, m_title_nozzle_rack);
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
    m_btn_homing->SetBackgroundColour(WXCOLOUR_GREY200);
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
    /* di*/
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

wgtDeviceNozzleRackNozzleItem::wgtDeviceNozzleRackNozzleItem(wxWindow* parent, int nozzle_id)
    : StaticBox(parent, wxID_ANY), m_nozzle_id(nozzle_id)
{
    CreateGui();
}

void wgtDeviceNozzleRackNozzleItem::CreateGui()
{
    // Background
    SetCornerRadius(FromDIP(5));
    SetBackgroundColor(*wxWHITE);

    // Top H
    wxSizer *top_h_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_nozzle_label_id = new Label(this);
    m_nozzle_label_id->SetFont(Label::Body_12);
    m_nozzle_label_id->SetBackgroundColour(*wxWHITE);
    m_nozzle_label_id->SetLabel(wxString::Format("%d", m_nozzle_id + 1));

    m_status             = NOZZLE_STATUS::NOZZLE_EMPTY;
    m_nozzle_empty_image = new ScalableBitmap(this, "dev_rack_nozzle_empty", 46);
    m_nozzle_icon = new wxStaticBitmap(this, wxID_ANY, m_nozzle_empty_image->bmp(), wxDefaultPosition, WX_DIP_SIZE_46);
    m_nozzle_icon->SetBackgroundColour(*wxWHITE);

    m_nozzle_selected_bitmap = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, WX_DIP_SIZE(20, 20));
    m_nozzle_selected_bitmap->SetBackgroundColour(*wxWHITE);

    top_h_sizer->Add(m_nozzle_label_id, 0, wxTOP | wxLEFT, FromDIP(6));
    top_h_sizer->AddSpacer(FromDIP(10));
    top_h_sizer->Add(m_nozzle_icon, 0, wxTOP, FromDIP(10));
    top_h_sizer->AddStretchSpacer();
    top_h_sizer->Add(m_nozzle_selected_bitmap, 0, wxTOP | wxRIGHT, FromDIP(2));

    // Bottom V
    wxBoxSizer* bottom_v = new wxBoxSizer(wxVERTICAL);

    wxSizer* label_h_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_nozzle_label_1 = new Label(this);
    m_nozzle_label_1->SetFont(Label::Body_12);
    m_nozzle_label_1->SetBackgroundColour(*wxWHITE);
    m_nozzle_label_1->SetLabel(_L("Empty"));

    label_h_sizer->Add(m_nozzle_label_1, 0, wxALIGN_LEFT);

    auto status_icon = create_scaled_bitmap("dev_rack_nozzle_error_icon", this, 14);
    m_nozzle_status_icon = new wxStaticBitmap(this, wxID_ANY, status_icon, wxDefaultPosition, WX_DIP_SIZE(14, 14));
    m_nozzle_status_icon->Bind(wxEVT_LEFT_DOWN, &wgtDeviceNozzleRackNozzleItem::OnBtnNozzleStatus, this);
    m_nozzle_status_icon->Bind(wxEVT_ENTER_WINDOW, [this](auto&) { SetCursor(wxCURSOR_HAND); });
    m_nozzle_status_icon->Bind(wxEVT_LEAVE_WINDOW, [this](auto&) { SetCursor(wxCURSOR_ARROW); });
    m_nozzle_status_icon->SetBackgroundColour(*wxWHITE);
    m_nozzle_status_icon->Show(false);

    label_h_sizer->Add(m_nozzle_status_icon, 0, wxALIGN_CENTER | wxLEFT, FromDIP(2));
    bottom_v->Add(label_h_sizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(2));

    m_nozzle_label_2 = new Label(this);
    m_nozzle_label_2->SetFont(Label::Body_12);
    m_nozzle_label_2->SetBackgroundColour(*wxWHITE);
    bottom_v->Add(m_nozzle_label_2, 0, wxALIGN_CENTER_HORIZONTAL);

    // Main sizer
    wxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(top_h_sizer);
    main_sizer->Add(bottom_v, 0, wxALIGN_CENTER_HORIZONTAL);
    SetSizer(main_sizer);

    SetMinSize(WGT_RACK_NOZZLE_SIZE);
    SetMaxSize(WGT_RACK_NOZZLE_SIZE);
    SetSize(WGT_RACK_NOZZLE_SIZE);
    Layout();
};

void wgtDeviceNozzleRackNozzleItem::SetSelected(bool selected)
{
    if (!m_enable_select){
        assert(false && "not support select");
        return;
    }

    if (m_is_selected != selected) {
        m_is_selected = selected;
        if (selected) {
            if (!m_nozzle_selected_image) {
                m_nozzle_selected_image = new ScalableBitmap(this, "dev_rack_nozzle_selected", 20);
            }

            m_nozzle_selected_bitmap->SetBitmap(m_nozzle_selected_image->bmp());
            SetBorderColor(StateColor::darkModeColorFor(s_hgreen_clr));
        } else {
            m_nozzle_selected_bitmap->SetBitmap(wxNullBitmap);
            SetBorderColor(StateColor::darkModeColorFor(s_gray_clr));
        }

        Refresh();
    }
}

void wgtDeviceNozzleRackNozzleItem::Update(const std::shared_ptr<DevNozzleRack> rack, bool on_rack /*= true*/)
{
    m_rack = rack;

    if (rack) {
        const auto        &nozzle_info  = on_rack ? rack->GetNozzle(m_nozzle_id) : rack->GetNozzleSystem()->GetExtNozzle(m_nozzle_id);
        const wxString    &diameter_str = nozzle_info.GetNozzleDiameterStr();
        const wxString    &flowtype_str = nozzle_info.GetNozzleFlowTypeStr();
        const std::string &color        = nozzle_info.GetFilamentColor();

        /*check empty first*/
        if (nozzle_info.IsEmpty()) {
            SetNozzleStatus(NOZZLE_STATUS::NOZZLE_EMPTY, _L("Empty"), wxEmptyString, color);
        } else if (nozzle_info.IsNormal()) {
            SetNozzleStatus(NOZZLE_STATUS::NOZZLE_NORMAL, diameter_str, flowtype_str, color);
        } else if (nozzle_info.IsAbnormal()) {
            SetNozzleStatus(NOZZLE_STATUS::NOZZLE_ERROR, _L("Error"), wxEmptyString, color);
        } else if (nozzle_info.IsUnknown()) {
            SetNozzleStatus(NOZZLE_STATUS::NOZZLE_UNKNOWN, _L("Unknown"), wxEmptyString, color);
        }
    }
}

void wgtDeviceNozzleRackNozzleItem::SetNozzleStatus(NOZZLE_STATUS status, const wxString& str1, const wxString& str2, const std::string& color)
{
    if (m_status != status || m_filament_color != color)
    {
        m_status = status;
        m_filament_color = color;
        switch (status)
        {
        case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_EMPTY:
        {
            if (!m_nozzle_empty_image) { m_nozzle_empty_image = new ScalableBitmap(this, "dev_rack_nozzle_empty", 46);}
            m_nozzle_icon->SetBitmap(m_nozzle_empty_image->bmp());
            break;
        }
        case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_NORMAL:
        {
            if (!m_nozzle_normal_image) { m_nozzle_normal_image = new ScalableBitmap(this, "dev_rack_nozzle_normal", 46);}
            m_nozzle_icon->SetBitmap(SetNozzleBmpColor(m_nozzle_normal_image->bmp(), m_filament_color));
            break;
        }
        case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_UNKNOWN:
        {
            if (!m_nozzle_unknown_image) { m_nozzle_unknown_image = new ScalableBitmap(this, "dev_rack_nozzle_unknown", 46);}
            m_nozzle_icon->SetBitmap(m_nozzle_unknown_image->bmp());
            break;
        }
        case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_ERROR:
        {
            if (!m_nozzle_error_image) { m_nozzle_error_image = new ScalableBitmap(this, "dev_rack_nozzle_error", 46);}
            m_nozzle_icon->SetBitmap(m_nozzle_error_image->bmp());
            break;
        }
        default:
        {
            break;
        }
        }

        if (status == wgtDeviceNozzleRackNozzleItem::NOZZLE_ERROR)
        {
            m_nozzle_label_1->SetForegroundColour(StateColor::darkModeColorFor(s_red_clr));
            m_nozzle_status_icon->Show(true);
        }
        else
        {
            m_nozzle_label_1->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));
            m_nozzle_status_icon->Show(false);
        }
    }

    bool update_layout = (m_nozzle_label_1->GetLabel() != str1 || m_nozzle_label_2->GetLabel() != str2);
    m_nozzle_label_1->SetLabel(str1);
    m_nozzle_label_2->SetLabel(str2);

    if (update_layout) {
        Layout();
    }
}

void wgtDeviceNozzleRackNozzleItem::OnBtnNozzleStatus(wxMouseEvent& evt)
{
    if (m_is_disabled) {
        return;
    }

    auto rack = m_rack.lock();
    if (rack && m_status == wgtDeviceNozzleRackNozzleItem::NOZZLE_ERROR)
    {
        MessageDialog dlg(nullptr, _L("The hotend is in an abnormal state and currently unavailable. "
            "Please go to 'Device -> Upgrade' to upgrade firmware."), _L("Abnormal Hotend"), wxICON_WARNING);
        dlg.AddButton(wxID_CANCEL, _L("Cancel"), false);
        dlg.AddButton(wxID_OK,_L("Jump to the upgrade page"), true);

        if (dlg.ShowModal() == wxID_OK) {
            wxGetApp().mainframe->m_monitor->jump_to_Upgrade();
        };
    }
}

void wgtDeviceNozzleRackNozzleItem::Rescale()
{
    if (m_nozzle_normal_image) { m_nozzle_normal_image->msw_rescale(); }
    if (m_nozzle_empty_image) { m_nozzle_empty_image->msw_rescale(); }
    if (m_nozzle_unknown_image) { m_nozzle_unknown_image->msw_rescale(); }
    if (m_nozzle_error_image) { m_nozzle_error_image->msw_rescale(); }

    auto status_icon = create_scaled_bitmap("dev_rack_nozzle_error_icon", this, 14);
    m_nozzle_status_icon->SetBitmap(status_icon);
    m_nozzle_status_icon->Refresh();

    if (m_nozzle_selected_image) {
        m_nozzle_selected_image->msw_rescale();
        if (m_is_selected) {
            m_nozzle_selected_bitmap->SetBitmap(m_nozzle_selected_image->bmp());
        }
    };

    switch (m_status)
    {
    case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_EMPTY:
    {
        m_nozzle_icon->SetBitmap(m_nozzle_empty_image->bmp());
        break;
    }
    case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_NORMAL:
    {
        m_nozzle_icon->SetBitmap(SetNozzleBmpColor(m_nozzle_normal_image->bmp(), m_filament_color));
        break;
    }
    case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_UNKNOWN:
    {
        m_nozzle_icon->SetBitmap(m_nozzle_unknown_image->bmp());
        break;
    }
    case Slic3r::GUI::wgtDeviceNozzleRackNozzleItem::NOZZLE_ERROR:
    {
        m_nozzle_icon->SetBitmap(m_nozzle_error_image->bmp());
        break;
    }
    default:
    {
        break;
    }
    };
};

void wgtDeviceNozzleRackNozzleItem::EnableSelect()
{
    if (m_enable_select == true) {
        return;
    };

    m_enable_select = true;
    m_nozzle_icon->Bind(wxEVT_LEFT_DOWN, [this](auto& evt) { OnItemSelected(evt); });
    m_nozzle_label_id->Bind(wxEVT_LEFT_DOWN, [this](auto& evt) { OnItemSelected(evt); });
    m_nozzle_label_1->Bind(wxEVT_LEFT_DOWN, [this](auto& evt) { OnItemSelected(evt); });
    m_nozzle_label_2->Bind(wxEVT_LEFT_DOWN, [this](auto& evt) { OnItemSelected(evt); });
    Bind(wxEVT_LEFT_DOWN, [this](auto& evt) { OnItemSelected(evt); });
}

void wgtDeviceNozzleRackNozzleItem::OnItemSelected(wxMouseEvent& evt)
{
    if (m_enable_select && !m_is_disabled){
        SetSelected(true);
        wxCommandEvent command_evt(EVT_NOZZLE_RACK_NOZZLE_ITEM_SELECTED, GetId());
        command_evt.SetEventObject(this);
        ProcessEvent(command_evt);
    }

    evt.Skip();
}


void wgtDeviceNozzleRackNozzleItem::SetDisable(bool disabled)
{
    if (m_is_disabled == disabled) {
        return;
    }

    m_is_disabled = disabled;

    auto bg_clr = disabled ? StateColor::darkModeColorFor("#E5E7EB") : StateColor::darkModeColorFor(*wxWHITE);
    m_nozzle_icon->SetBackgroundColour(bg_clr);
    m_nozzle_label_id->SetBackgroundColour(bg_clr);
    m_nozzle_label_1->SetBackgroundColour(bg_clr);
    m_nozzle_status_icon->SetBackgroundColour(bg_clr);
    m_nozzle_label_2->SetBackgroundColour(bg_clr);
    m_nozzle_selected_bitmap->SetBackgroundColour(bg_clr);

    SetBackgroundColor(bg_clr);
    Refresh();
};

};
