#include "AMSControl.hpp"
#include "Label.hpp"
#include "../I18N.hpp"
#include "../GUI_App.hpp"

#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/DeviceTab/uiAmsHumidityPopup.h"

#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevFilaSwitch.h"

#include <wx/anybutton.h>
#include <wx/checklst.h>
#include <wx/gdicmn.h>
#include "slic3r/GUI/AmsMappingPopup.hpp"
#include "slic3r/GUI/AMSDryControl.hpp"
#include <wx/scrolwin.h>
#include <wx/panel.h>
#include "slic3r/GUI/Widgets/AMSItem.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include <cstddef>
#include <wx/event.h>
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include <string>
#include <wx/colour.h>
#include <vector>
#include <utility>
#include <cassert>
#include <memory>
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/Event.hpp"
#include <cstdlib>
#include <algorithm>
#include <tuple>
#include <wx/font.h>
#include <wx/simplebook.h>
#include <wx/dcgraph.h>
#include <wx/artprov.h>

#include <boost/log/trivial.hpp>
#include <wx/string.h>
#include "slic3r/GUI/DeviceCore/DevUtil.h"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/Widgets/StepCtrl.hpp"
#include "slic3r/GUI/wxExtensions.hpp"


namespace Slic3r { namespace GUI {

#define AMS_CANS_SIZE wxSize(FromDIP(284), -1)
#define AMS_CANS_WINDOW_SIZE wxSize(FromDIP(264), -1)
#define SINGLE_SLOT_AMS_PANEL_SIZE wxSize(FromDIP(264), FromDIP(160))


AMSControl::AMSControl(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size)
    : wxSimplebook(parent, wxID_ANY, pos, size)
    , m_Humidity_tip_popup(AmsHumidityTipPopup(this))
    , m_percent_humidity_dry_popup(new uiAmsPercentHumidityDryPopup(this))
    , m_ams_introduce_popup(AmsIntroducePopup(this))
    , m_ams_dry_ctr_win(new AMSDryCtrWin(this))
{
    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (dev) {
        MachineObject *obj = dev->get_selected_machine();
        parse_object(obj);
    }

    SetBackgroundColour(*wxWHITE);
    // normal mode
    //Freeze();
    m_sizer_body = new wxBoxSizer(wxVERTICAL);
    m_amswin                 = new wxWindow(this, wxID_ANY);
    m_amswin->SetBackgroundColour(*wxWHITE);
    m_amswin->SetSize(wxSize(FromDIP(578), -1));
    m_amswin->SetMinSize(wxSize(FromDIP(578), -1));


    m_sizer_ams_items = new wxBoxSizer(wxHORIZONTAL);

    /*right items*/
    m_panel_prv_left = new wxScrolledWindow(m_amswin, wxID_ANY);
    m_panel_prv_left->SetScrollRate(10, 0);
    m_panel_prv_left->SetSize(AMS_ITEMS_PANEL_SIZE);
    m_panel_prv_left->SetMinSize(AMS_ITEMS_PANEL_SIZE);
    //m_panel_prv_left->SetBackgroundColour(0x4169E1);
    m_panel_prv_left->SetBackgroundColour(AMS_CONTROL_DEF_BLOCK_BK_COLOUR);
    m_sizer_prv_left = new wxBoxSizer(wxHORIZONTAL);
    m_panel_prv_left->SetSizer(m_sizer_prv_left);
    m_panel_prv_left->Layout();
    //m_sizer_items_left->Fit(m_panel_prv_left);

    /*right items*/
    m_panel_prv_right = new wxScrolledWindow(m_amswin, wxID_ANY);
    m_panel_prv_right->SetScrollRate(10, 0);
    m_panel_prv_right->SetSize(AMS_ITEMS_PANEL_SIZE);
    m_panel_prv_right->SetMinSize(AMS_ITEMS_PANEL_SIZE);
    //m_panel_prv_right->SetBackgroundColour(0x4169E1);
    m_panel_prv_right->SetBackgroundColour(AMS_CONTROL_DEF_BLOCK_BK_COLOUR);
    m_sizer_prv_right = new wxBoxSizer(wxHORIZONTAL);
    m_panel_prv_right->SetSizer(m_sizer_prv_right);
    m_panel_prv_right->Layout();
    //m_sizer_items_right->Fit(m_panel_prv_right);

    /*m_sizer_ams_items->Add(m_panel_prv_left, 0, wxALIGN_CENTER|wxLEFT|wxRIGHT, FromDIP(5));
    m_sizer_ams_items->Add(m_panel_prv_right, 0, wxALIGN_CENTER|wxLEFT|wxRIGHT, FromDIP(5));*/
    m_sizer_ams_items->Add(m_panel_prv_left, 0, wxLEFT | wxRIGHT, FromDIP(5));
    m_sizer_ams_items->Add(m_panel_prv_right, 0, wxLEFT | wxRIGHT, FromDIP(5));

    //m_panel_prv_right->Hide();

    //m_sizer_ams_body = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_ams_body = new wxBoxSizer(wxHORIZONTAL);

    //ams area
    m_sizer_ams_area_left = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_ams_area_right = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_down_road = new wxBoxSizer(wxHORIZONTAL);

    m_simplebook_ams_left = new wxSimplebook(m_amswin, wxID_ANY, wxDefaultPosition, AMS_CANS_WINDOW_SIZE, 0);
    m_simplebook_ams_left->SetBackgroundColour(AMS_CONTROL_DEF_BLOCK_BK_COLOUR);
    //m_sizer_ams_area_left->Add(m_simplebook_ams_left, 0, wxLEFT | wxRIGHT, FromDIP(5));
    m_sizer_ams_area_left->Add(m_simplebook_ams_left, 0, wxALIGN_CENTER, 0);

    m_simplebook_ams_right = new wxSimplebook(m_amswin, wxID_ANY, wxDefaultPosition, AMS_CANS_WINDOW_SIZE, 0);
    m_simplebook_ams_right->SetBackgroundColour(AMS_CONTROL_DEF_BLOCK_BK_COLOUR);
    //m_sizer_ams_area_right->Add(m_simplebook_ams_right, 0, wxLEFT | wxRIGHT, FromDIP(5));
    m_sizer_ams_area_right->Add(m_simplebook_ams_right, 0, wxALIGN_CENTER, 0);

    m_panel_down_road = new wxPanel(m_amswin, wxID_ANY, wxDefaultPosition, AMS_DOWN_ROAD_SIZE, 0);
    m_panel_down_road->SetBackgroundColour(AMS_CONTROL_DEF_BLOCK_BK_COLOUR);

    m_down_road = new AMSRoadDownPart(m_panel_down_road, wxID_ANY, wxDefaultPosition, AMS_DOWN_ROAD_SIZE);
    m_sizer_down_road->Add(m_panel_down_road, 0, wxTOP, 0);

    // ams mode
    //
    m_simplebook_ams_right->SetBackgroundColour(AMS_CONTROL_DEF_BLOCK_BK_COLOUR);


    m_sizer_ams_area_left->Layout();
    m_sizer_ams_area_right->Layout();


    m_sizer_ams_option = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_option_left = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_option_mid = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_option_right = new wxBoxSizer(wxHORIZONTAL);

    auto m_panel_option_left    = new wxPanel(m_amswin);
    auto m_panel_option_right   = new wxPanel(m_amswin);

    m_panel_option_left->SetBackgroundColour(*wxWHITE);
    m_panel_option_right->SetBackgroundColour(*wxWHITE);

    m_panel_option_left->SetSizer(m_sizer_option_left);
    m_panel_option_right->SetSizer(m_sizer_option_right);

    m_panel_option_left->SetMinSize(wxSize(FromDIP(180), -1));
    m_panel_option_left->SetMaxSize(wxSize(FromDIP(180), -1));

    m_panel_option_right->SetMinSize(wxSize(FromDIP(180), -1));
    m_panel_option_right->SetMaxSize(wxSize(FromDIP(180), -1));

    /*option left*/
    m_button_auto_refill = new Button(m_panel_option_left, _L("Auto Refill"));
    m_button_auto_refill->SetStyle(ButtonStyle::Regular, ButtonType::Choice);

    m_button_ams_setting_normal = ScalableBitmap(this, "ams_setting_normal", 24);
    m_button_ams_setting_hover = ScalableBitmap(this, "ams_setting_hover", 24);
    m_button_ams_setting_press = ScalableBitmap(this, "ams_setting_press", 24);

    m_button_ams_setting = new wxStaticBitmap(m_panel_option_left, wxID_ANY, m_button_ams_setting_normal.bmp(), wxDefaultPosition, wxSize(FromDIP(24), FromDIP(24)));
    m_button_ams_setting->SetMinSize(wxSize(FromDIP(24), FromDIP(24)));
    m_button_ams_setting->SetMaxSize(wxSize(FromDIP(24), FromDIP(24)));
    m_sizer_option_left->Add(m_button_auto_refill, 0, wxALIGN_CENTER, 0);
    m_sizer_option_left->Add(0, 0, 0, wxLEFT, FromDIP(20));
    m_sizer_option_left->Add(m_button_ams_setting, 0, wxALIGN_CENTER, 0);


    /*option mid*/
    m_extruder = new AMSextruder(m_amswin, wxID_ANY, m_total_ext_count, wxDefaultPosition, AMS_EXTRUDER_SIZE);
    m_sizer_option_mid->Add( m_extruder, 0, wxALIGN_CENTER, 0 );

    // Orca: filament-switch routing glyph; hidden by default so it stays inert (zero layout impact)
    // on any printer without a Filament Track Switch. Shown from UpdateAms only when installed.
    m_switcher = new SwitcherImage(m_amswin, wxID_ANY, "fila_switch", wxSize(FromDIP(29), FromDIP(16)), wxDefaultPosition);
    m_switcher->Hide();
    m_sizer_option_mid->Add(m_switcher, 0, wxALIGN_CENTER | wxLEFT, FromDIP(6));


    /*option right*/
    m_button_extruder_feed = new Button(m_panel_option_right, _L("Load"));
    m_button_extruder_feed->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_button_extruder_feed->EnableTooltipEvenDisabled();


    if (wxGetApp().app_config->get("language") == "de_DE") m_button_extruder_feed->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "fr_FR") m_button_extruder_feed->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "ru_RU") m_button_extruder_feed->SetLabel("Load");
    if (wxGetApp().app_config->get("language") == "nl_NL") m_button_extruder_feed->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "hu_HU") m_button_extruder_feed->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "ja_JP") m_button_extruder_feed->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "sv_SE") m_button_extruder_feed->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "cs_CZ") m_button_extruder_feed->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "uk_UA") m_button_extruder_feed->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "pt_BR") m_button_extruder_feed->SetLabel("Load");

    m_button_extruder_back = new Button(m_panel_option_right, _L("Unload"));
    m_button_auto_refill->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_button_extruder_back->EnableTooltipEvenDisabled();

    if (wxGetApp().app_config->get("language") == "de_DE") m_button_extruder_back->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "fr_FR") m_button_extruder_back->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "ru_RU") m_button_extruder_back->SetLabel("Unload");
    if (wxGetApp().app_config->get("language") == "nl_NL") m_button_extruder_back->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "hu_HU") m_button_extruder_back->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "ja_JP") m_button_extruder_back->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "sv_SE") m_button_extruder_back->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "cs_CZ") m_button_extruder_back->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "uk_UA") m_button_extruder_back->SetFont(Label::Body_9);
    if (wxGetApp().app_config->get("language") == "pt_BR") m_button_extruder_back->SetLabel("Unload");


    //m_sizer_option_right->Add(0, 0, 1, wxEXPAND, 0);
    m_sizer_option_right->Add(m_button_extruder_back, 0, wxLEFT, FromDIP(0));
    m_sizer_option_right->Add(m_button_extruder_feed, 0, wxLEFT, FromDIP(20));

    m_panel_option_left->Layout();
    m_panel_option_right->Layout();

    m_sizer_ams_option->Add(m_panel_option_left, 0, wxALIGN_TOP, 0);
    m_sizer_ams_option->Add( 0, 0, 1, wxEXPAND, 0);
    m_sizer_ams_option->Add(m_sizer_option_mid, 0, wxALIGN_TOP, 0);
    m_sizer_ams_option->Add( 0, 0, 1, wxEXPAND, 0);
    m_sizer_ams_option->Add(m_panel_option_right, 0, wxALIGN_TOP, 0);


    m_sizer_ams_body->Add(m_sizer_ams_area_left, wxALIGN_CENTER, 0);
    m_sizer_ams_body->AddSpacer(FromDIP(10));
    m_sizer_ams_body->Add(m_sizer_ams_area_right, wxALIGN_CENTER, 0);

    m_sizer_body->Add(m_sizer_ams_items, 0, wxALIGN_CENTER, 0);
    m_sizer_ams_gap = m_sizer_body->Add(0, 0, 1, wxEXPAND | wxTOP, FromDIP(10));
    m_sizer_body->Add(m_sizer_ams_body, 0, wxALIGN_CENTER, 0);
    m_sizer_body->Add(m_sizer_down_road, 0, wxALIGN_CENTER, 0);
    m_sizer_body->Add(m_sizer_ams_option, 0, wxEXPAND, 0);

    m_amswin->SetSizer(m_sizer_body);
    m_amswin->Layout();
    m_amswin->Fit();
    //Thaw();

    SetSize(m_amswin->GetSize());
    SetMinSize(m_amswin->GetSize());


    AddPage(m_amswin, wxEmptyString, false);


    m_button_extruder_feed->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(AMSControl::on_filament_load), NULL, this);
    m_button_extruder_back->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(AMSControl::on_filament_unload), NULL, this);
    m_button_auto_refill->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(AMSControl::auto_refill), NULL, this);

    m_button_ams_setting->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
        m_button_ams_setting->SetBitmap(m_button_ams_setting_hover.bmp());
        e.Skip();
    });
    m_button_ams_setting->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        m_button_ams_setting->SetBitmap(m_button_ams_setting_press.bmp());
        on_ams_setting_click(e);
        e.Skip();
    });

    m_button_ams_setting->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
        m_button_ams_setting->SetBitmap(m_button_ams_setting_normal.bmp());
        e.Skip();
    });

    Bind(EVT_AMS_SHOW_HUMIDITY_TIPS, [this](wxCommandEvent& evt) {
        uiAmsHumidityInfo *info    = (uiAmsHumidityInfo *) evt.GetClientData();
        if (info)
        {
            Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
            MachineObject *obj = nullptr;
            if (dev) {
                obj = dev->get_selected_machine();
            }

            if (info->ams_type == AMSModel::GENERIC_AMS)
            {
                wxPoint img_pos = ClientToScreen(wxPoint(0, 0));
                wxPoint popup_pos(img_pos.x - m_Humidity_tip_popup.GetSize().GetWidth() + FromDIP(150), img_pos.y - FromDIP(80));
                m_Humidity_tip_popup.Position(popup_pos, wxSize(0, 0));

                int humidity_value = info->humidity_display_idx;
                if (humidity_value > 0 && humidity_value <= 5) { m_Humidity_tip_popup.set_humidity_level(humidity_value); }
                m_Humidity_tip_popup.Popup();
            } else if (obj && obj->is_support_remote_dry && (info->ams_type == AMSModel::N3F_AMS || info->ams_type == AMSModel::N3S_AMS)){
                m_ams_dry_ctr_win->set_ams_id(info->ams_id);

                wxPoint img_pos = ClientToScreen(wxPoint(0, 0));
                wxPoint popup_pos(img_pos.x - m_ams_dry_ctr_win->GetSize().GetWidth() + FromDIP(150), img_pos.y - FromDIP(80));
                m_ams_dry_ctr_win->Move(popup_pos);
                m_ams_dry_ctr_win->ShowModal();
            } else {
                m_percent_humidity_dry_popup->UpdateInfo(info);

                wxPoint img_pos = ClientToScreen(wxPoint(0, 0));
                wxPoint popup_pos(img_pos.x - m_percent_humidity_dry_popup->GetSize().GetWidth() + FromDIP(150), img_pos.y - FromDIP(80));
                m_percent_humidity_dry_popup->Move(popup_pos);
                m_percent_humidity_dry_popup->ShowModal();
            }
        }

        delete info;
    });
    Bind(EVT_AMS_ON_SELECTED, &AMSControl::AmsSelectedSwitch, this);
}

void AMSControl::on_retry()
{
    post_event(wxCommandEvent(EVT_AMS_RETRY));
}

AMSControl::~AMSControl()
{
    if (m_ams_dry_ctr_win) {
        delete m_ams_dry_ctr_win;
    }
}

std::string AMSControl::GetCurentAms() {
    return m_current_ams;
}
std::string AMSControl::GetCurentShowAms(AMSPanelPos pos) {
    if (pos == AMSPanelPos::RIGHT_PANEL){
        return m_current_show_ams_right;
    }
    else{
        return m_current_show_ams_left;
    }
}

std::string AMSControl::GetCurrentCan(std::string amsid)
{
    std::string current_can;
    for (auto ams_item : m_ams_item_list) {
        AmsItem* item = ams_item.second;
        if (item == nullptr){
            continue;
        }
        if (item->get_ams_id() == amsid) {
            current_can = item->GetCurrentCan();
            return current_can;
        }
    }
    return current_can;
}

bool AMSControl::IsAmsInRightPanel(std::string ams_id) {
    if (m_total_ext_count == 2){
        // Orca: inlet-aware panel routing (see AMSinfo::routes_to_main_extruder)
        if (m_ams_item_list.find(ams_id) != m_ams_item_list.end() && m_ams_item_list[ams_id]->routes_to_main_extruder()) {
            return true;
        }
        else{
            return false;
        }
    }
    else if (m_total_ext_count == 1){
        for (auto id : m_item_ids[MAIN_EXTRUDER_ID]){
            if (id == ams_id){
                return true;
            }
        }
        return false;
    }
    else {
        // Generic N Nozzles
    }
    return false;
}

void AMSControl::AmsSelectedSwitch(wxCommandEvent& event) {
    std::string ams_id_selected = std::to_string(event.GetInt());
    if (m_current_ams != ams_id_selected){
        m_current_ams = ams_id_selected;
    }
    if (m_current_show_ams_left != ams_id_selected && m_current_show_ams_left != "") {
        auto item = m_ams_item_list[m_current_show_ams_left];
        if (!item) return;
        try{
            const auto& can_lib_list = item->get_can_lib_list();
            for (auto can : can_lib_list) {
                can.second->UnSelected();
            }
        }
        catch (...){
            ;
        }
    }
    else if (m_current_show_ams_right != ams_id_selected && m_current_show_ams_right != "") {
        auto item = m_ams_item_list[m_current_show_ams_right];
        if (!item) return;
        try {
            const auto &can_lib_list = item->get_can_lib_list();
            for (auto can : can_lib_list) {
                can.second->UnSelected();
            }
        }
        catch (...) {
            ;
        }
    }
}

wxColour AMSControl::GetCanColour(std::string amsid, std::string canid)
{
    wxColour col = *wxWHITE;
    for (auto i = 0; i < m_ams_info.size(); i++) {
        if (m_ams_info[i].ams_id == amsid) {
            for (auto o = 0; o < m_ams_info[i].cans.size(); o++) {
                if (m_ams_info[i].cans[o].can_id == canid) {
                    col = m_ams_info[i].cans[o].material_colour;
                }
            }
        }
    }
    return col;
}

void AMSControl::EnableLoadFilamentBtn(bool enable, const std::string& ams_id, const std::string& can_id,const wxString& tips)
{
    m_button_extruder_feed->Enable(enable);
    if (m_button_extruder_feed->GetToolTipText() != tips) {
        BOOST_LOG_TRIVIAL(info) << "ams_id=" << ams_id << ", can_id=" << can_id << "  Set Load Filament Button ToolTip : " << tips.ToUTF8();
        m_button_extruder_feed->SetToolTip(tips);
    }
}

void AMSControl::EnableUnLoadFilamentBtn(bool enable, const std::string& ams_id, const std::string& can_id,const wxString& tips)
{
    m_button_extruder_back->Enable(enable);
    if (m_button_extruder_back->GetToolTipText() != tips) {
        BOOST_LOG_TRIVIAL(info) << "ams_id=" << ams_id << ", can_id=" << can_id << "  Set Unload Filament Button ToolTip : " << tips.ToUTF8();
        m_button_extruder_back->SetToolTip(tips);
    }
}

void AMSControl::EnterNoneAMSMode()
{
    //m_vams_lib->m_ams_model = m_ext_model;
    if(m_is_none_ams_mode == AMSModel::EXT_AMS) return;
    m_panel_prv_left->Hide();

    m_simplebook_ams_left->SetSelection(0);
    m_extruder->no_ams_mode(true);
    //m_button_ams_setting->Hide();
    //m_button_extruder_feed->Show();
    //m_button_extruder_back->Show();

    ShowFilamentTip(false);
    m_amswin->Layout();
    m_amswin->Fit();
    Layout();
    m_is_none_ams_mode = AMSModel::EXT_AMS;
}

void AMSControl::EnterGenericAMSMode()
{
    if(m_is_none_ams_mode == AMSModel::GENERIC_AMS) return;
    m_extruder->no_ams_mode(false);
    m_amswin->Layout();
    m_amswin->Fit();
    Layout();
    m_is_none_ams_mode = AMSModel::GENERIC_AMS;
}

void AMSControl::EnterExtraAMSMode()
{
    //m_vams_lib->m_ams_model = m_ext_model;
    if(m_is_none_ams_mode == AMSModel::AMS_LITE) return;
    m_panel_prv_left->Hide();

    m_simplebook_ams_left->SetSelection(2);
    m_extruder->no_ams_mode(false);
    m_amswin->Layout();
    m_amswin->Fit();
    Layout();
    Refresh(true);
    m_is_none_ams_mode = AMSModel::AMS_LITE;

}

void AMSControl::PlayRridLoading(wxString amsid, wxString canid)
{
    auto iter = m_ams_item_list.find(amsid.ToStdString());

    if (iter != m_ams_item_list.end()) {
        AmsItem* cans = iter->second;
        cans->PlayRridLoading(canid);
    }
}

void AMSControl::StopRridLoading(wxString amsid, wxString canid)
{
    auto iter = m_ams_item_list.find(amsid.ToStdString());

    if (iter != m_ams_item_list.end()) {
        AmsItem* cans = iter->second;
        cans->StopRridLoading(canid);
    }
}

void AMSControl::msw_rescale()
{
    m_button_ams_setting_normal.msw_rescale();
    m_button_ams_setting_hover.msw_rescale();
    m_button_ams_setting_press.msw_rescale();
    m_button_ams_setting->SetBitmap(m_button_ams_setting_normal.bmp());

    if (m_extruder) m_extruder->msw_rescale();

    if (m_button_extruder_feed) m_button_extruder_feed->Rescale(); // ORCA
    if (m_button_extruder_back) m_button_extruder_back->Rescale(); // ORCA
    if (m_button_auto_refill) m_button_auto_refill->Rescale();     // ORCA
    if (m_button_ams_setting) m_button_ams_setting->SetMinSize(wxSize(FromDIP(25), FromDIP(24)));


    for (auto ams_item : m_ams_item_list) {
        if (ams_item.second){
            ams_item.second->msw_rescale();
        }
    }
    for (auto ams_prv : m_ams_preview_list) {
        if (ams_prv.second){
            ams_prv.second->msw_rescale();
        }
    }
    for (auto ext_img : m_ext_image_list) {
        if (ext_img.second) {
            ext_img.second->msw_rescale();
        }
    }
    if (m_down_road){
        m_down_road->msw_rescale();
    }

    if (m_percent_humidity_dry_popup){
        m_percent_humidity_dry_popup->msw_rescale();
    }

    if (m_ams_dry_ctr_win) {
        m_ams_dry_ctr_win->msw_rescale();
    }

    m_Humidity_tip_popup.msw_rescale();

    Layout();
    Refresh();
}

void AMSControl::CreateAms()
{
    auto caninfo0_0 = Caninfo{"def_can_0", (""), *wxWHITE, AMSCanType::AMS_CAN_TYPE_VIRTUAL};
    auto caninfo0_1 = Caninfo{"def_can_1", (""), *wxWHITE, AMSCanType::AMS_CAN_TYPE_VIRTUAL };
    auto caninfo0_2 = Caninfo{"def_can_2", (""), *wxWHITE, AMSCanType::AMS_CAN_TYPE_VIRTUAL };
    auto caninfo0_3 = Caninfo{"def_can_3", (""), *wxWHITE, AMSCanType::AMS_CAN_TYPE_VIRTUAL };

    auto caninfo1_0 = Caninfo{ "def_can_0", (""), *wxWHITE, AMSCanType::AMS_CAN_TYPE_VIRTUAL };
    auto caninfo1_1 = Caninfo{ "def_can_1", (""), *wxWHITE, AMSCanType::AMS_CAN_TYPE_VIRTUAL };
    auto caninfo1_2 = Caninfo{ "def_can_2", (""), *wxWHITE, AMSCanType::AMS_CAN_TYPE_VIRTUAL };
    auto caninfo1_3 = Caninfo{ "def_can_3", (""), *wxWHITE, AMSCanType::AMS_CAN_TYPE_VIRTUAL };

    AMSinfo                        ams1 = AMSinfo{"0", std::vector<Caninfo>{caninfo0_0, caninfo0_1, caninfo0_2, caninfo0_3}, 0};
    AMSinfo                        ams2 = AMSinfo{"1", std::vector<Caninfo>{caninfo0_0, caninfo0_1, caninfo0_2, caninfo0_3}, 0 };
    AMSinfo                        ams3 = AMSinfo{"2", std::vector<Caninfo>{caninfo0_0, caninfo0_1, caninfo0_2, caninfo0_3}, 0 };
    AMSinfo                        ams4 = AMSinfo{"3", std::vector<Caninfo>{caninfo0_0, caninfo0_1, caninfo0_2, caninfo0_3}, 0 };

    AMSinfo                        ams5 = AMSinfo{ "4", std::vector<Caninfo>{caninfo1_0, caninfo1_1, caninfo1_2, caninfo1_3}, 1 };
    AMSinfo                        ams6 = AMSinfo{ "5", std::vector<Caninfo>{caninfo1_0, caninfo1_1, caninfo1_2, caninfo1_3}, 1 };
    AMSinfo                        ams7 = AMSinfo{ "6", std::vector<Caninfo>{caninfo1_0, caninfo1_1, caninfo1_2, caninfo1_3}, 1 };
    AMSinfo                        ams8 = AMSinfo{ "7", std::vector<Caninfo>{caninfo1_0, caninfo1_1, caninfo1_2, caninfo1_3}, 1 };
    std::vector<AMSinfo>           ams_info{ams1, ams2, ams3, ams4, ams5, ams6, ams7, ams8 };
    std::vector<AMSinfo>::iterator it;
    //Freeze();
    for (it = ams_info.begin(); it != ams_info.end(); it++) {
        AddAmsPreview(*it, AMSModel::GENERIC_AMS);
        AddAms(*it);
        //AddExtraAms(*it);
        m_ams_info.push_back(*it);
    }
    if (m_single_nozzle_no_ams)
    {
        m_simplebook_ams_left->Hide();
    }
    else {
        m_sizer_prv_left->Layout();
        m_sizer_prv_right->Layout();
    }
    //Thaw();
}


void AMSControl::ClearAms() {
    m_sizer_ams_items->Clear(false);

    m_simplebook_ams_right->DeleteAllPages();
    m_simplebook_ams_left->DeleteAllPages();
    m_simplebook_ams_right->DestroyChildren();
    m_simplebook_ams_left->DestroyChildren();
    m_simplebook_ams_right->Layout();
    m_simplebook_ams_left->Layout();
    m_simplebook_ams_right->Refresh();
    m_simplebook_ams_left->Refresh();

    for (auto it : m_ams_preview_list) {
        if (it.second) it.second->Destroy();
    }
    m_ams_preview_list.clear();
    m_ext_image_list.clear();

    m_left_page_index = 0;
    m_right_page_index = 0;

    m_ams_item_list.clear();
    m_sizer_prv_right->Clear();
    m_sizer_prv_left->Clear();

    if (m_nozzle_book) {
        m_nozzle_book->Destroy();
        m_nozzle_book = nullptr;
    } else {
        for (auto &pane : m_nozzle_panes) {
            if (pane.preview_panel) pane.preview_panel->Destroy();
            if (pane.ams_book) pane.ams_book->Destroy();
            if (pane.page) pane.page->Destroy();
        }
    }
    m_item_ids.assign(std::max(m_total_ext_count, 2), {});
    m_nozzle_panes.clear();
    m_active_nozzle_id = 0;
    m_current_ams.clear();
    m_current_show_ams_left.clear();
    m_current_show_ams_right.clear();
    pair_id.clear();
}

void AMSControl::restore_legacy_ams_layout()
{
    m_sizer_ams_items->Clear(false);
    m_sizer_ams_items->SetOrientation(wxHORIZONTAL);
    m_sizer_body->GetItem(m_sizer_ams_items)->SetFlag(wxALIGN_CENTER);
    m_sizer_ams_items->Add(m_panel_prv_left, 0, wxLEFT | wxRIGHT, FromDIP(5));
    m_sizer_ams_items->Add(m_panel_prv_right, 0, wxLEFT | wxRIGHT, FromDIP(5));

    m_panel_prv_left->Show();
    m_panel_prv_right->Show();
    m_simplebook_ams_left->Show();
    m_simplebook_ams_right->Show();
    m_panel_down_road->Show();
    m_sizer_body->Show(m_sizer_ams_body, true);
    m_sizer_option_mid->Show(m_extruder, true);
    m_down_road->SetSingleSideLayout(false);
    m_sizer_ams_gap->SetProportion(1);
    m_sizer_ams_gap->SetBorder(FromDIP(10));
    m_down_road->SetNozzleCount(m_total_ext_count);
}

bool AMSControl::use_multi_nozzle_layout() const
{
    const bool is_bbl = wxGetApp().preset_bundle && wxGetApp().preset_bundle->is_bbl_vendor();
    return is_bbl ? m_total_ext_count > 2 : m_nozzle_book != nullptr;
}

void AMSControl::CreateAmsMultiNozzle(const std::string &series_name, const std::string &printer_type) {
    const size_t pane_count = static_cast<size_t>(std::max(m_total_ext_count, 0));
    m_nozzle_panes.resize(pane_count);
    m_active_nozzle_id = 0;

    m_sizer_ams_items->Clear(false);
    m_sizer_ams_items->SetOrientation(wxVERTICAL);
    m_sizer_body->GetItem(m_sizer_ams_items)->SetFlag(wxALIGN_CENTER);
    m_sizer_body->Show(m_sizer_ams_body, false);
    m_sizer_ams_gap->SetProportion(0);
    m_sizer_ams_gap->SetBorder(0);

    m_panel_prv_left->Hide();
    m_panel_prv_right->Hide();
    m_simplebook_ams_left->Hide();
    m_simplebook_ams_right->Hide();
    m_panel_down_road->Show();
    m_sizer_option_mid->Show(m_extruder, true);
    m_switcher->Hide();
    m_down_road->SetNozzleCount(1);
    m_down_road->SetSingleSideLayout(true, AMSPanelPos::LEFT_PANEL);
    m_down_road->UpdateLeft(1, AMSRoadShowMode::AMS_ROAD_MODE_SINGLE);
    m_down_road->UpdateRight(1, AMSRoadShowMode::AMS_ROAD_MODE_NONE);
    m_extruder->updateNozzleNum(1, series_name);

    m_nozzle_book = new wxSimplebook(m_amswin, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0);
    m_nozzle_book->SetMinSize(wxSize(FromDIP(264), -1));
    m_nozzle_book->SetBackgroundColour(StateColor::darkModeColorFor(AMS_CONTROL_DEF_BLOCK_BK_COLOUR));
    m_sizer_ams_items->Add(m_nozzle_book, 0, wxALIGN_CENTER);

    for (size_t nozzle_id = 0; nozzle_id < pane_count; ++nozzle_id) {
        auto &pane = m_nozzle_panes[nozzle_id];
        // A generic page is one H2C side. Keep the left-side geometry as the
        // canonical single-nozzle presentation; the road and AMS item code
        // still use the same side-specific paths as the legacy layout.
        pane.panel_pos = AMSPanelPos::LEFT_PANEL;
        pane.page = new wxPanel(m_nozzle_book, wxID_ANY);
        pane.page->SetMinSize(wxSize(FromDIP(264), -1));
        pane.page->SetBackgroundColour(StateColor::darkModeColorFor(AMS_CONTROL_DEF_BLOCK_BK_COLOUR));
        pane.page_sizer = new wxBoxSizer(wxVERTICAL);
        pane.page->SetSizer(pane.page_sizer);

        pane.preview_panel = new wxScrolledWindow(pane.page, wxID_ANY);
        pane.preview_panel->SetScrollRate(10, 0);
        pane.preview_panel->SetSize(AMS_ITEMS_PANEL_SIZE);
        pane.preview_panel->SetMinSize(AMS_ITEMS_PANEL_SIZE);
        pane.preview_panel->SetBackgroundColour(StateColor::darkModeColorFor(AMS_CONTROL_DEF_BLOCK_BK_COLOUR));

        pane.preview_sizer = new wxBoxSizer(wxHORIZONTAL);
        pane.preview_panel->SetSizer(pane.preview_sizer);

        // Generic lane cards grow to their calculated width for 1-8 lanes. Keep the
        // book at the legacy minimum, but allow the page to expand to that width.
        pane.ams_book = new wxSimplebook(pane.page, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0);
        pane.ams_book->SetMinSize(wxSize(FromDIP(264), -1));
        pane.ams_book->SetBackgroundColour(StateColor::darkModeColorFor(AMS_CONTROL_DEF_BLOCK_BK_COLOUR));
        // wxALIGN_CENTER does not centre horizontally inside a HORIZONTAL sizer, so a book wider
        // than pane.ams_area's own minimum would sit at ams_area's left edge and overflow to the
        // right, shifting the whole card column off the shared down-road connector axis. A pair of
        // stretch spacers keeps the book centred on the pane (and thus on the connector) at any
        // book width; ams_area is expanded below so the spacers have room to work with.
        pane.ams_area = new wxBoxSizer(wxHORIZONTAL);
        pane.ams_area->AddStretchSpacer(1);
        pane.ams_area->Add(pane.ams_book, 0, wxALIGN_CENTER, 0);
        pane.ams_area->AddStretchSpacer(1);

        pane.page_sizer->Add(pane.preview_panel, 0, wxALIGN_CENTER | wxTOP, FromDIP(5));
        pane.page_sizer->Add(pane.ams_area, 0, wxEXPAND | wxTOP, FromDIP(10));

        m_nozzle_book->AddPage(pane.page, wxEmptyString, nozzle_id == 0);
    }

    auto add_info = [this, series_name, printer_type, pane_count](AMSinfo info) {
        if (pane_count == 0) return;

        info.nozzle_id = std::clamp(info.nozzle_id, 0, static_cast<int>(pane_count) - 1);
        AddAmsPreview(info, info.ams_type);
        if (info.ams_type == AMSModel::EXT_AMS && info.cans.size() == 1)
            AddAms({info}, series_name, printer_type, m_nozzle_panes[info.nozzle_id].panel_pos);
        else
            AddAms(info);
    };

    for (const auto &info : m_ams_info)
        add_info(info);

    for (auto &info : m_ext_info) {
        const int virtual_tray_id = std::atoi(info.ams_id.c_str());
        if (virtual_tray_id <= VIRTUAL_TRAY_MAIN_ID &&
            virtual_tray_id >= VIRTUAL_TRAY_MAIN_ID - static_cast<int>(pane_count) + 1)
            info.nozzle_id = VIRTUAL_TRAY_MAIN_ID - virtual_tray_id;
        add_info(info);
    }

    for (auto &pane : m_nozzle_panes) {
        if (pane.ams_book && pane.ams_book->GetPageCount() > 0)
            pane.ams_book->SetSelection(0);
        if (pane.preview_sizer) pane.preview_sizer->Layout();
        if (pane.preview_panel) pane.preview_panel->FitInside();
        if (pane.ams_book) pane.ams_book->Layout();
        if (pane.page) pane.page->Layout();

        if (!pane.current_ams.empty()) {
            auto preview = m_ams_preview_list.find(pane.current_ams);
            if (preview != m_ams_preview_list.end() && preview->second)
                preview->second->OnSelected();
        }
    }

    if (m_nozzle_book && pane_count > 0) {
        m_nozzle_book->SetSelection(0);
        m_current_ams = m_nozzle_panes[0].current_ams;
        if (!m_current_ams.empty())
            SwitchAms(m_current_ams);
        else {
            m_down_road->UpdateLeft(1, AMSRoadShowMode::AMS_ROAD_MODE_NONE);
            m_down_road->UpdateRight(1, AMSRoadShowMode::AMS_ROAD_MODE_NONE);
        }
    }
    m_amswin->Layout();
    m_amswin->Fit();
    // Generic N Nozzles
}

void AMSControl::CreateAmsDoubleNozzle(const std::string &series_name, const std::string &printer_type)
{
    restore_legacy_ams_layout();

    std::vector<AMSinfo> single_info_left;
    std::vector<AMSinfo> single_info_right;

    //Freeze();
    // Orca: place each AMS by its switch inlet when a Filament Track Switch is installed, else by the pinned
    // nozzle_id (routes_to_main_extruder). Switch-less machines are unaffected; ext spools keep nozzle_id.
    for (auto ams_info = m_ams_info.begin(); ams_info != m_ams_info.end(); ams_info++){
        if (ams_info->cans.size() == GENERIC_AMS_SLOT_NUM){
            ams_info->routes_to_main_extruder() ? m_item_ids[MAIN_EXTRUDER_ID].push_back(ams_info->ams_id) : m_item_ids[DEPUTY_EXTRUDER_ID].push_back(ams_info->ams_id);
            AddAmsPreview(*ams_info, ams_info->ams_type);
            AddAms(*ams_info);
        }
        else if (ams_info->cans.size() == 1){

            if (ams_info->routes_to_main_extruder()){
                single_info_right.push_back(*ams_info);
                if (single_info_right.size() == 2){
                    single_info_right[0].routes_to_main_extruder() ? m_item_ids[MAIN_EXTRUDER_ID].push_back(single_info_right[0].ams_id) : m_item_ids[DEPUTY_EXTRUDER_ID].push_back(single_info_right[0].ams_id);
                    single_info_right[1].routes_to_main_extruder() ? m_item_ids[MAIN_EXTRUDER_ID].push_back(single_info_right[1].ams_id) : m_item_ids[DEPUTY_EXTRUDER_ID].push_back(single_info_right[1].ams_id);
                    AddAms(single_info_right, series_name, printer_type);
                    AddAmsPreview(single_info_right, AMSPanelPos::RIGHT_PANEL);
                    pair_id.push_back(std::make_pair(single_info_right[0].ams_id, single_info_right[1].ams_id));
                    single_info_right.clear();
                }
            }
            else if (!ams_info->routes_to_main_extruder()){
                single_info_left.push_back(*ams_info);
                if (single_info_left.size() == 2){
                    single_info_left[0].routes_to_main_extruder() ? m_item_ids[MAIN_EXTRUDER_ID].push_back(single_info_left[0].ams_id) : m_item_ids[DEPUTY_EXTRUDER_ID].push_back(single_info_left[0].ams_id);
                    single_info_left[1].routes_to_main_extruder() ? m_item_ids[MAIN_EXTRUDER_ID].push_back(single_info_left[1].ams_id) : m_item_ids[DEPUTY_EXTRUDER_ID].push_back(single_info_left[1].ams_id);
                    AddAms(single_info_left, series_name, printer_type);
                    AddAmsPreview(single_info_left, AMSPanelPos::LEFT_PANEL);
                    pair_id.push_back(std::make_pair(single_info_left[0].ams_id, single_info_left[1].ams_id));
                    single_info_left.clear();
                }
            }
        }
    }
    if (m_ext_info.size() <= 1) {
        BOOST_LOG_TRIVIAL(trace) << "vt_slot empty!";
        assert(0);
        return;
    }
    AMSinfo ext_info;
    for (auto info : m_ext_info){
        if (info.ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID)){
            ext_info = info;
            single_info_right.push_back(ext_info);
            break;
        }
    }
    //wait add


    single_info_right[0].routes_to_main_extruder() ? m_item_ids[MAIN_EXTRUDER_ID].push_back(single_info_right[0].ams_id) : m_item_ids[DEPUTY_EXTRUDER_ID].push_back(single_info_right[0].ams_id);
    if (single_info_right.size() == 2){
        single_info_right[1].routes_to_main_extruder() ? m_item_ids[MAIN_EXTRUDER_ID].push_back(single_info_right[1].ams_id) : m_item_ids[DEPUTY_EXTRUDER_ID].push_back(single_info_right[1].ams_id);
        pair_id.push_back(std::make_pair(single_info_right[0].ams_id, single_info_right[1].ams_id));
    }
    AddAms(single_info_right, series_name, printer_type);
    AddAmsPreview(single_info_right, AMSPanelPos::RIGHT_PANEL);
    single_info_right.clear();

    for (auto info : m_ext_info) {
        if (info.ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
            ext_info = info;
            single_info_left.push_back(ext_info);
            break;
        }
    }
    //wait add
    single_info_left[0].routes_to_main_extruder() ? m_item_ids[MAIN_EXTRUDER_ID].push_back(single_info_left[0].ams_id) : m_item_ids[DEPUTY_EXTRUDER_ID].push_back(single_info_left[0].ams_id);
    if (single_info_left.size() == 2){
        single_info_left[1].routes_to_main_extruder() ? m_item_ids[MAIN_EXTRUDER_ID].push_back(single_info_left[1].ams_id) : m_item_ids[DEPUTY_EXTRUDER_ID].push_back(single_info_left[1].ams_id);
        pair_id.push_back(std::make_pair(single_info_left[0].ams_id, single_info_left[1].ams_id));
    }
    AddAmsPreview(single_info_left, AMSPanelPos::LEFT_PANEL);
    AddAms(single_info_left, series_name, printer_type);
    single_info_left.clear();

    m_sizer_prv_left->Layout();
    m_sizer_prv_right->Layout();
    m_simplebook_ams_left->Show();
    m_simplebook_ams_right->Show();
    if (m_ams_info.size() > 0){
        m_panel_prv_left->Show();
        m_panel_prv_right->Show();
    }
    else{
        m_panel_prv_left->Hide();
        m_panel_prv_right->Hide();
    }
    m_simplebook_ams_left->SetSelection(0);
    m_simplebook_ams_right->SetSelection(0);

    auto left_init_mode = findFirstMode(AMSPanelPos::LEFT_PANEL);
    auto right_init_mode = findFirstMode(AMSPanelPos::RIGHT_PANEL);


    m_down_road->UpdateLeft(2, left_init_mode);
    m_down_road->UpdateRight(2, right_init_mode);

    m_extruder->updateNozzleNum(2);

    m_current_show_ams_left = m_item_ids[DEPUTY_EXTRUDER_ID].size() > 0 ? m_item_ids[DEPUTY_EXTRUDER_ID][0] : "";
    m_current_show_ams_right = m_item_ids[MAIN_EXTRUDER_ID].size() > 0 ? m_item_ids[MAIN_EXTRUDER_ID][0] : "";

    m_current_ams = "";
    m_down_road->UpdatePassRoad(AMSPanelPos::LEFT_PANEL, -1, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
    m_extruder->OnAmsLoading(false, DEPUTY_EXTRUDER_ID);
    m_down_road->UpdatePassRoad(AMSPanelPos::RIGHT_PANEL, -1, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
    m_extruder->OnAmsLoading(false, MAIN_EXTRUDER_ID);

    m_amswin->Layout();
    m_amswin->Fit();

    //Thaw();
}

void AMSControl::CreateAmsSingleNozzle(const std::string &series_name, const std::string &printer_type)
{
    restore_legacy_ams_layout();

    std::vector<int>m_item_nums{0,0};
    std::vector<AMSinfo> single_info;

    //Freeze();

    //add ams data
    for (auto ams_info = m_ams_info.begin(); ams_info != m_ams_info.end(); ams_info++) {
        if (ams_info->cans.size() == GENERIC_AMS_SLOT_NUM) {
            m_item_ids[DEPUTY_EXTRUDER_ID].push_back(ams_info->ams_id);
            AddAmsPreview(*ams_info, ams_info->ams_type);
            AddAms(*ams_info, AMSPanelPos::LEFT_PANEL);
            //AddExtraAms(*ams_info);
        }
        else if (ams_info->cans.size() == 1) {
            m_item_ids[DEPUTY_EXTRUDER_ID].push_back(ams_info->ams_id);
            AddAmsPreview(*ams_info, ams_info->ams_type);
            AddAms(*ams_info, AMSPanelPos::LEFT_PANEL);

            /*single_info.push_back(*ams_info);
            if (single_info.size() == MAX_AMS_NUM_IN_PANEL) {
                m_item_ids[DEPUTY_NOZZLE_ID].push_back(single_info[0].ams_id);
                m_item_ids[DEPUTY_NOZZLE_ID].push_back(single_info[1].ams_id);
                m_item_nums[DEPUTY_NOZZLE_ID]++;
                pair_id.push_back(std::make_pair(single_info[0].ams_id, single_info[1].ams_id));
                AddAmsPreview(single_info, AMSPanelPos::LEFT_PANEL);
                AddAms(single_info, AMSPanelPos::LEFT_PANEL);
                single_info.clear();
            }*/
        }
    }
    if (single_info.size() > 0){
        m_item_ids[DEPUTY_EXTRUDER_ID].push_back(single_info[0].ams_id);
        m_item_nums[DEPUTY_EXTRUDER_ID]++;
        AddAms(single_info, series_name, printer_type, AMSPanelPos::LEFT_PANEL);
        AddAmsPreview(single_info, AMSPanelPos::LEFT_PANEL);
        single_info.clear();
    }

    // data ext data
    if (m_ext_info.size() <= 0){
        BOOST_LOG_TRIVIAL(trace) << "vt_slot empty!";
        return;
    }

    single_info.push_back(m_ext_info[0]);
    m_item_ids[MAIN_EXTRUDER_ID].push_back(single_info[0].ams_id);
    AddAms(single_info, series_name, printer_type, AMSPanelPos::RIGHT_PANEL);
    auto left_init_mode = findFirstMode(AMSPanelPos::LEFT_PANEL);
    auto right_init_mode = findFirstMode(AMSPanelPos::RIGHT_PANEL);

    m_panel_prv_right->Hide();
    m_panel_prv_left->Hide();
    if (m_ams_info.size() > 0){
        m_simplebook_ams_left->Show();
        m_simplebook_ams_right->Show();
        m_simplebook_ams_left->SetSelection(0);
        m_simplebook_ams_right->SetSelection(0);

        if (m_ams_info.size() > 1){
            m_sizer_prv_right->Layout();
            m_panel_prv_right->Show();
        }
        m_down_road->UpdateLeft(1, left_init_mode);
        m_down_road->UpdateRight(1, right_init_mode);
    }
    else {
        m_panel_prv_left->Hide();
        m_panel_prv_right->Hide();
        m_simplebook_ams_left->Hide();
        m_simplebook_ams_right->Show();

        m_simplebook_ams_right->SetSelection(0);
        m_down_road->UpdateLeft(1, left_init_mode);
        m_down_road->UpdateRight(1, right_init_mode);
    }
    m_current_show_ams_left = m_item_ids[DEPUTY_EXTRUDER_ID].size() > 0 ? m_item_ids[DEPUTY_EXTRUDER_ID][0] : "";
    m_current_show_ams_right = m_item_ids[MAIN_EXTRUDER_ID].size() > 0 ? m_item_ids[MAIN_EXTRUDER_ID][0] : "";
    m_current_ams = "";

    m_down_road->UpdatePassRoad(AMSPanelPos::LEFT_PANEL, -1, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
    m_down_road->UpdatePassRoad(AMSPanelPos::RIGHT_PANEL, -1, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
    m_extruder->updateNozzleNum(1);
    m_extruder->OnAmsLoading(false, MAIN_EXTRUDER_ID);

    m_amswin->Layout();
    m_amswin->Fit();

    //Refresh();
    //Thaw();
}

void AMSControl::Reset()
{
    m_ams_info.clear();
    m_ext_info.clear();
    m_dev_id.clear();
    ClearAms();

    Layout();
}

void AMSControl::show_noams_mode()
{
    EnterGenericAMSMode();
}

void AMSControl::show_auto_refill(bool show)
{
    if (m_button_auto_refill->IsShown() == show)
    {
        return;
    }

    m_button_auto_refill->Show(show);
    m_amswin->Layout();
    m_amswin->Fit();
}

void AMSControl::enable_ams_setting(bool en)
{
    m_button_ams_setting->Enable(en);
}

void AMSControl::show_vams_kn_value(bool show)
{
    //m_vams_lib->show_kn_value(show);
}

void AMSControl::UpdateAmsDryControl(MachineObject* obj)
{
    if (!m_ams_dry_ctr_win->IsShown()) {
        return;
    }

    if (!obj || !obj->GetFilaSystem()) {
        m_ams_dry_ctr_win->Close();
        return;
    }

    std::weak_ptr<DevFilaSystem> weak_fila_system = obj->GetFilaSystem();

    if (auto locaked_fila_system = weak_fila_system.lock()) {
        m_ams_dry_ctr_win->update(locaked_fila_system, obj);
    } else {
        m_ams_dry_ctr_win->Close();
        return;
    }
}

std::vector<AMSinfo> AMSControl::GenerateSimulateData() {
    auto caninfo0_0 = Caninfo{ "0", (""), *wxRED, AMSCanType::AMS_CAN_TYPE_VIRTUAL };
    auto caninfo0_1 = Caninfo{ "1", (""), *wxGREEN, AMSCanType::AMS_CAN_TYPE_VIRTUAL };
    auto caninfo0_2 = Caninfo{ "2", (""), *wxBLUE, AMSCanType::AMS_CAN_TYPE_VIRTUAL };
    auto caninfo0_3 = Caninfo{ "3", (""), *wxYELLOW, AMSCanType::AMS_CAN_TYPE_VIRTUAL };

    auto caninfo1_0 = Caninfo{ "0", (""), wxColour(255, 255, 0), AMSCanType::AMS_CAN_TYPE_VIRTUAL };
    auto caninfo1_1 = Caninfo{ "1", (""), wxColour(255, 0, 255), AMSCanType::AMS_CAN_TYPE_VIRTUAL };
    auto caninfo1_2 = Caninfo{ "2", (""), wxColour(0, 255, 255), AMSCanType::AMS_CAN_TYPE_VIRTUAL };
    auto caninfo1_3 = Caninfo{ "3", (""), wxColour(200, 80, 150), AMSCanType::AMS_CAN_TYPE_VIRTUAL };

    AMSinfo                        ams1 = AMSinfo{ "0", std::vector<Caninfo>{caninfo0_0, caninfo0_1, caninfo0_2, caninfo0_3}, 0 };
    AMSinfo                        ams2 = AMSinfo{ "1", std::vector<Caninfo>{caninfo0_0, caninfo0_1, caninfo0_2, caninfo0_3}, 0 };
    AMSinfo                        ams3 = AMSinfo{ "2", std::vector<Caninfo>{caninfo0_0, caninfo0_1, caninfo0_2, caninfo0_3}, 0 };
    AMSinfo                        ams4 = AMSinfo{ "3", std::vector<Caninfo>{caninfo0_0, caninfo0_1, caninfo0_2, caninfo0_3}, 0 };

    AMSinfo                        singleams1 = AMSinfo{ "0", std::vector<Caninfo>{caninfo0_0}, 0 };
    AMSinfo                        singleams2 = AMSinfo{ "1", std::vector<Caninfo>{caninfo0_0}, 0 };
    AMSinfo                        singleams3 = AMSinfo{ "2", std::vector<Caninfo>{caninfo0_0}, 0 };
    AMSinfo                        singleams4 = AMSinfo{ "3", std::vector<Caninfo>{caninfo0_0}, 0 };
    singleams1.ams_type = AMSModel::N3S_AMS;
    singleams2.ams_type = AMSModel::N3S_AMS;
    singleams3.ams_type = AMSModel::N3S_AMS;
    singleams4.ams_type = AMSModel::N3S_AMS;

    AMSinfo                        ams5 = AMSinfo{ "4", std::vector<Caninfo>{caninfo1_0, caninfo1_1, caninfo1_2, caninfo1_3}, 1 };
    AMSinfo                        ams6 = AMSinfo{ "5", std::vector<Caninfo>{caninfo1_0, caninfo1_1, caninfo1_2, caninfo1_3}, 1 };
    AMSinfo                        ams7 = AMSinfo{ "6", std::vector<Caninfo>{caninfo1_0, caninfo1_1, caninfo1_2, caninfo1_3}, 1 };
    AMSinfo                        ams8 = AMSinfo{ "7", std::vector<Caninfo>{caninfo1_0, caninfo1_1, caninfo1_2, caninfo1_3}, 1 };

    AMSinfo                        singleams5 = AMSinfo{ "4", std::vector<Caninfo>{caninfo1_0}, 1 };
    AMSinfo                        singleams6 = AMSinfo{ "5", std::vector<Caninfo>{caninfo1_0}, 1 };
    AMSinfo                        singleams7 = AMSinfo{ "6", std::vector<Caninfo>{caninfo1_0}, 1 };
    AMSinfo                        singleams8 = AMSinfo{ "7", std::vector<Caninfo>{caninfo1_0}, 1 };
    AMSinfo                        singleams9 = AMSinfo{ "8", std::vector<Caninfo>{caninfo1_0}, 1 };
    singleams5.ams_type = AMSModel::N3S_AMS;
    singleams6.ams_type = AMSModel::N3S_AMS;
    singleams7.ams_type = AMSModel::N3S_AMS;
    singleams8.ams_type = AMSModel::N3S_AMS;
    singleams9.ams_type = AMSModel::N3S_AMS;

    ams3.current_can_id = "2";
    ams3.current_step = AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP2;
    ams5.current_can_id = "2";
    ams5.current_step = AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP2;
    std::vector<AMSinfo>generic_ams = { ams1, ams2, ams3, ams4, ams5, ams6, ams7, ams8 };
    std::vector<AMSinfo>single_ams = { singleams1, singleams2, singleams3, singleams4, singleams5, singleams6, singleams7, singleams8, singleams9 };
    std::vector<AMSinfo>ams_info = { ams1, singleams2, ams3, singleams4, ams5, singleams6, ams7, singleams8, singleams9 };
    return ams_info;
}


void AMSControl::UpdateAms(const std::string   &series_name,
                           const std::string   &printer_type,
                           std::vector<AMSinfo> ams_info,
                           std::vector<AMSinfo> ext_info,
                           DevExtderSystem           data,
                           std::string          dev_id,
                           MachineObject*       obj,
                           bool                 is_reset,
                           bool                 test)
{
    if (!test){
        // update item
        bool fresh = false;

        // basic check
        if (m_ams_info.size() == ams_info.size() && m_total_ext_count == data.GetTotalExtderCount() && m_dev_id == dev_id && m_ext_info.size() == ext_info.size())
        {
            for (int i = 0; i < m_ams_info.size(); i++){
                if (m_ams_info[i].ams_id != ams_info[i].ams_id){
                    fresh = true;
                }

                // Orca: rebuild when the panel assignment changes; inlet-aware so a Filament Track Switch
                // re-plug (same pinned nozzle_id, different inlet) still refreshes placement.
                if (m_ams_info[i].routes_to_main_extruder() != ams_info[i].routes_to_main_extruder()) {
                    fresh = true;
                }
                if (m_ams_info[i].nozzle_id != ams_info[i].nozzle_id) {
                    fresh = true;
                }
            }

            for (int i = 0; i < static_cast<int>(m_ext_info.size()); ++i) {
                if (m_ext_info[i].ams_id != ext_info[i].ams_id ||
                    m_ext_info[i].nozzle_id != ext_info[i].nozzle_id ||
                    m_ext_info[i].ext_type != ext_info[i].ext_type) {
                    fresh = true;
                }
            }
        }
        else{
            fresh = true;
        }

        m_ams_info.clear();
        m_ams_info = ams_info;
        m_total_ext_count = data.GetTotalExtderCount();
        m_ext_info.clear();
        m_ext_info = ext_info;
        m_dev_id = dev_id;
        if (fresh){
            ClearAms();
            bool is_bbl = wxGetApp().preset_bundle && wxGetApp().preset_bundle->is_bbl_vendor();
            if (is_bbl) {
                if (m_total_ext_count == 1)
                    CreateAmsSingleNozzle(series_name, printer_type);
                else if (m_total_ext_count == 2)
                    CreateAmsDoubleNozzle(series_name, printer_type);
            }
            else
                CreateAmsMultiNozzle(series_name, printer_type);

            SetSize(wxSize(FromDIP(578), -1));
            SetMinSize(wxSize(FromDIP(578), -1));
            Layout();
        }
        // update cans

        for (auto ams_item : m_ams_item_list) {
            if (ams_item.second == nullptr){
                continue;
            }
            std::string ams_id = ams_item.second->get_ams_id();
            AmsItem* cans = ams_item.second;
            if (cans->get_ams_id() == std::to_string(VIRTUAL_TRAY_MAIN_ID) || cans->get_ams_id() == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
                for (auto ifo : m_ext_info) {
                    if (ifo.ams_id == ams_id) {
                        cans->UpdateInfo(ifo);
                        cans->show_sn_value(m_ams_model == AMSModel::AMS_LITE ? false : true);
                    }
                }
            }
            else{
                for (auto ifo : m_ams_info) {
                    if (ifo.ams_id == ams_id) {
                        cans->UpdateInfo(ifo);
                        cans->show_sn_value(m_ams_model == AMSModel::AMS_LITE ? false : true);
                    }
                }
            }
        }

        // 2D mode (laser/cut) makes every spool view-only: show the read-only (eye) icon while the spool
        // stays clickable to open the read-only filament dialog.
        // Orca: gated on the device mode via MachineObject::is_fdm_type(); obj is null for callers that do
        // not supply it, leaving spools editable.
        const bool view_only = obj && !obj->is_fdm_type();
        for (auto ams_item : m_ams_item_list) {
            if (ams_item.second == nullptr) { continue; }
            for (auto lib_it : ams_item.second->get_can_lib_list()) {
                if (lib_it.second) { lib_it.second->set_view_only(view_only); }
            }
        }

        for (auto ams_prv : m_ams_preview_list) {
            std::string id = ams_prv.second->get_ams_id();
            auto item = m_ams_item_list.find(id);
            if (item != m_ams_item_list.end())
            { ams_prv.second->UpdateInfo(item->second->get_ams_info());
            }
        }
    }

    /*update humidity popup*/
    if (m_percent_humidity_dry_popup->IsShown())
    {
        std::string target_id = m_percent_humidity_dry_popup->get_owner_ams_id();
        for (const auto& the_info : ams_info)
        {
            if (target_id == the_info.ams_id)
            {
                uiAmsHumidityInfo humidity_info;
                humidity_info.ams_id = the_info.ams_id;
                humidity_info.humidity_display_idx = the_info.get_humidity_display_idx();
                humidity_info.humidity_percent = the_info.humidity_raw;
                humidity_info.left_dry_time = the_info.left_dray_time;
                humidity_info.current_temperature = the_info.current_temperature;
                m_percent_humidity_dry_popup->UpdateInfo(&humidity_info);
                break;
            }
        }
    }

    /*update ams extruder*/
    if (m_total_ext_count <= 2 && m_extruder->updateNozzleNum(m_total_ext_count, series_name))
    {
        m_amswin->Layout();
    }

    /*update switch status*/
    // Orca: inert on any printer without a Filament Track Switch — install is false, so the banner is
    // never materialized, the glyph stays hidden, and every ShowRoad(true) below is a no-op.
    const auto [install, ready] = isFilaSwitchReady();
    show_switcher_status(install && (!ready));
    bool isShow = install && m_total_ext_count >= 2;
    if (m_switcher->IsShown() != isShow)
    {
        m_switcher->Show(isShow);
        m_sizer_body->Layout();
        m_sizer_body->Fit(this);
        this->Layout();
        this->Refresh(true);
        this->Update();
    }

    /*update ext road visibility when fila switch installed*/
    bool road_visibility_changed = false;
    for (auto& [ams_id, ams_item] : m_ams_item_list) {
        if (!ams_item) continue;
        // Orca: drop the external-spool road (AMSModel::EXT_AMS) while the switch routes all filament.
        const bool should_show_road = !(install && ams_item->get_ams_model() == AMSModel::EXT_AMS);
        if (ams_item->ShowRoad(should_show_road)) { road_visibility_changed = true; }
    }
    if (road_visibility_changed) { m_amswin->Layout(); }
}

void AMSControl::AddAmsPreview(AMSinfo info, AMSModel type)
{
    AMSPreview *ams_prv = nullptr;

    // Match AddAms: the generic multi-nozzle layout's preview panel is the visible one for
    // non-Bambu vendors at any extruder count, not only above two.
    if (use_multi_nozzle_layout()) {
        const int nozzle_id = std::clamp(info.nozzle_id, 0, m_total_ext_count - 1);
        auto &pane = m_nozzle_panes[nozzle_id];
        ams_prv = new AMSPreview(pane.preview_panel, wxID_ANY, info, type);
        pane.preview_sizer->Add(ams_prv, 0, wxALIGN_CENTER | wxLEFT, FromDIP(6));
    }
    else if (info.routes_to_main_extruder())
    {
        ams_prv = new AMSPreview(m_panel_prv_right, wxID_ANY, info, type);
        m_sizer_prv_right->Add(ams_prv, 0, wxALIGN_CENTER | wxLEFT, FromDIP(6));
    }
    else if (!info.routes_to_main_extruder())
    {
        ams_prv = new AMSPreview(m_panel_prv_left, wxID_ANY, info, type);
        m_sizer_prv_left->Add(ams_prv, 0, wxALIGN_CENTER | wxLEFT, FromDIP(6));
    }

    if (ams_prv){
        ams_prv->Bind(wxEVT_LEFT_DOWN, [this, ams_prv](wxMouseEvent &e) {
            SwitchAms(ams_prv->get_ams_id());
            e.Skip();
        });
        m_ams_preview_list[info.ams_id] = ams_prv;
    }
}

void AMSControl::createAms(wxSimplebook* parent, int& idx, AMSinfo info, AMSPanelPos pos) {
    auto ams_item = new AmsItem(parent, info, info.ams_type, pos);
    parent->InsertPage(idx, ams_item, wxEmptyString, true);

    // Generic AMS cards may be wider than the legacy 264 DIP page. Propagate
    // that calculated width through the page and simplebook so 5-8 lanes are
    // laid out fully instead of being clipped by the old minimum width.
    if (info.ams_type == AMSModel::GENERIC_AMS) {
        const int required_width = ams_item->GetMinSize().x;
        if (required_width > 0) {
            const int page_width = std::max(parent->GetMinSize().x, required_width);
            parent->SetMinSize(wxSize(page_width, -1));
            if (auto *page = parent->GetParent()) {
                page->SetMinSize(wxSize(std::max(page->GetMinSize().x, page_width), -1));
                if (auto *nozzle_book = page->GetParent())
                    nozzle_book->SetMinSize(wxSize(std::max(nozzle_book->GetMinSize().x, page_width), -1));
            }
        }
        // Above eight lanes, AmsItem is a fixed-minimum viewport over a wider
        // lane strip. Expand the viewport through its page's sizers rather
        // than propagating the strip's virtual width into the card/book.
        // Reset back to the centered fixed layout for eight or fewer lanes so a
        // later small unit is not left in expand mode.
        const bool generic_lane_layout = !wxGetApp().preset_bundle || !wxGetApp().preset_bundle->is_bbl_vendor();
        if (generic_lane_layout) {
            if (auto *page = parent->GetParent()) {
                auto pane = std::find_if(m_nozzle_panes.begin(), m_nozzle_panes.end(),
                    [page](const NozzleAmsPane &candidate) { return candidate.page == page; });
                if (pane != m_nozzle_panes.end() && m_nozzle_book) {
                    const bool expand_viewport = info.cans.size() > 8;
                    if (auto *book_item = pane->ams_area->GetItem(pane->ams_book)) {
                        book_item->SetProportion(expand_viewport ? 1 : 0);
                        book_item->SetFlag(expand_viewport ? wxEXPAND : wxALIGN_CENTER);
                    }
                    // Keep the centring spacers (items 0 and 2) elastic only in the centred layout;
                    // in the >8-lane viewport they must collapse so the book can fill ams_area.
                    if (auto *lead = pane->ams_area->GetItem(static_cast<size_t>(0)))
                        lead->SetProportion(expand_viewport ? 0 : 1);
                    if (auto *trail = pane->ams_area->GetItem(static_cast<size_t>(2)))
                        trail->SetProportion(expand_viewport ? 0 : 1);
                    if (auto *area_item = pane->page_sizer->GetItem(pane->ams_area))
                        area_item->SetFlag(wxEXPAND | wxTOP);
                    if (auto *nozzle_book_item = m_sizer_ams_items->GetItem(m_nozzle_book))
                        nozzle_book_item->SetFlag(expand_viewport ? wxEXPAND : wxALIGN_CENTER);
                    if (auto *items_item = m_sizer_body->GetItem(m_sizer_ams_items))
                        items_item->SetFlag(expand_viewport ? wxEXPAND : wxALIGN_CENTER);
                    m_amswin->Layout();
                }
            }
        }
    }
    ams_item->set_selection(idx);
    idx++;

    m_ams_item_list[info.ams_id] = ams_item;
}

AMSRoadShowMode AMSControl::findFirstMode(AMSPanelPos pos) {
    auto init_mode = AMSRoadShowMode::AMS_ROAD_MODE_NONE;
    std::string ams_id = "";
    if (pos == AMSPanelPos::LEFT_PANEL && m_item_ids[DEPUTY_EXTRUDER_ID].size() > 0){
        ams_id = m_item_ids[DEPUTY_EXTRUDER_ID][0];
    }
    else if (pos == AMSPanelPos::RIGHT_PANEL && m_item_ids[MAIN_EXTRUDER_ID].size() > 0){
        ams_id = m_item_ids[MAIN_EXTRUDER_ID][0];
    }

    auto item = m_ams_item_list.find(ams_id);
    if (ams_id.empty() || item == m_ams_item_list.end()) return init_mode;

    if (item->second->get_can_count() == GENERIC_AMS_SLOT_NUM) {
        if (item->second->get_ams_model() == AMSModel::AMS_LITE) return AMSRoadShowMode::AMS_ROAD_MODE_AMS_LITE;
        if (item->second->get_ams_model() == AMSModel::EXT_AMS && item->second->get_ext_type() == AMSModelOriginType::LITE_EXT) return AMSRoadShowMode::AMS_ROAD_MODE_AMS_LITE;
        return AMSRoadShowMode::AMS_ROAD_MODE_FOUR;
    }
    else if (item->second->get_can_count() > GENERIC_AMS_SLOT_NUM) {
        return AMSRoadShowMode::AMS_ROAD_MODE_GENERIC;
    }
    else {
        for (auto ids : pair_id){
            if (ids.first == ams_id || ids.second == ams_id){
                return AMSRoadShowMode::AMS_ROAD_MODE_DOUBLE;
            }
        }
        if (item->second->get_ams_model() == AMSModel::EXT_AMS && item->second->get_ext_type() == AMSModelOriginType::LITE_EXT) return AMSRoadShowMode::AMS_ROAD_MODE_AMS_LITE;
        if (item->second->get_ams_model() == AMSModel::N3S_AMS) return AMSRoadShowMode::AMS_ROAD_MODE_SINGLE_N3S;
        return AMSRoadShowMode::AMS_ROAD_MODE_SINGLE;
    }
}

void AMSControl::createAmsPanel(wxSimplebook *parent, int &idx, std::vector<AMSinfo> infos, const std::string &series_name, const std::string &printer_type, AMSPanelPos pos, int total_ext_num)
{
    if (infos.size() <= 0) return;

    wxPanel* book_panel = new wxPanel(parent);
    wxBoxSizer* book_sizer = new wxBoxSizer(wxHORIZONTAL);
    book_panel->SetBackgroundColour(StateColor::darkModeColorFor(AMS_CONTROL_DEF_LIB_BK_COLOUR));
    // Size the page to the AMS book it becomes, not to the legacy fixed width: an AMS card is
    // added directly as the book page (so it always spans the book), while a single external
    // spool is wrapped here. Matching the book keeps the Ext card centred on the same axis as
    // the AMS cards and the shared down-road connector anchored below them.
    const wxSize book_panel_size(std::max(parent->GetMinSize().x, AMS_PANEL_SIZE.x), AMS_PANEL_SIZE.y);
    book_panel->SetSize(book_panel_size);
    book_panel->SetMinSize(book_panel_size);

    AmsItem* ams1 = nullptr, * ams2 = nullptr;
    ams1 = new AmsItem(book_panel, infos[0], infos[0].ams_type, pos);
    if (ams1->get_ext_image()) { ams1->get_ext_image()->setTotalExtNum(series_name, printer_type, total_ext_num); }

    if (infos.size() == MAX_AMS_NUM_IN_PANEL) {    //n3s and ? in a panel
        ams2 = new AmsItem(book_panel, infos[1], infos[1].ams_type, pos);
        if (ams2->get_ext_image()) { ams2->get_ext_image()->setTotalExtNum(series_name, printer_type, total_ext_num); }

        if (pos == AMSPanelPos::LEFT_PANEL) {
            book_sizer->Add(ams1, 0, wxLEFT, FromDIP(4));
            book_sizer->Add(ams2, 0, wxLEFT, FromDIP(30));
        }
        else {
            book_sizer->Add(ams1, 0, wxLEFT, FromDIP(72));
            book_sizer->Add(ams2, 0, wxLEFT, FromDIP(30));
        }
    }
    else {   //only an ext in a panel
        if (ams1->get_ext_image()) { ams1->get_ext_image()->setShowAmsExt(false);}

        if (ams1->get_ams_model() == AMSModel::EXT_AMS) {
            if (ams1->get_ext_type() == LITE_EXT) {
                //book_sizer->Add(ams1, 0, wxALIGN_CENTER_HORIZONTAL, 0);
                book_sizer->Add(ams1, 0, wxLEFT, (book_panel->GetSize().x - ams1->GetSize().x) / 2);
            }
            else if (pos == AMSPanelPos::SINGLE_PANEL) {
                // Centre the single external-spool card explicitly, using the same fixed-margin
                // technique as the Lite-EXT branch above. Two stretch spacers centred the card
                // against the wrapper's own width rather than the AMS book, which left the card
                // (and the connector drawn below it) off the book/connector axis.
                book_sizer->Add(ams1, 0, wxLEFT,
                                std::max(0, (book_panel_size.x - ams1->GetSize().x) / 2));
            }
            else{
                auto ext_image = new AMSExtImage(book_panel, pos, m_total_ext_count, false);
                book_sizer->Add(ams1, 0, wxLEFT, FromDIP(30));
                book_sizer->Add(ext_image, 0, wxEXPAND | wxLEFT, FromDIP(30));
                ext_image->setTotalExtNum(series_name, printer_type, total_ext_num);
                m_ext_image_list[infos[0].ams_id] = ext_image;
            }
        }
    }

    book_panel->SetSizer(book_sizer);
    book_panel->Layout();
    book_panel->Fit();

    parent->InsertPage(idx, book_panel, wxEmptyString, true);
    ams1->SetBackgroundColour(StateColor::darkModeColorFor(AMS_CONTROL_DEF_LIB_BK_COLOUR));
    ams1->set_selection(idx);
    m_ams_item_list[infos[0].ams_id] = ams1;
    if (ams2) {
        ams2->SetBackgroundColour(StateColor::darkModeColorFor(AMS_CONTROL_DEF_LIB_BK_COLOUR));
        ams2->set_selection(idx);
        m_ams_item_list[infos[1].ams_id] = ams2;
    }
    idx++;
}

void AMSControl::AddAms(AMSinfo info, AMSPanelPos pos)
{
    // The generic multi-nozzle layout (non-Bambu vendors) owns the only visible book; a unit must
    // land there for any extruder count. Keying off m_total_ext_count instead sent 1- and 2-extruder
    // generic printers into the legacy simplebooks that CreateAmsMultiNozzle hides.
    if (use_multi_nozzle_layout()) {
        const int nozzle_id = std::clamp(info.nozzle_id, 0, m_total_ext_count - 1);
        auto &pane = m_nozzle_panes[nozzle_id];
        pane.item_ids.push_back(info.ams_id);
        m_item_ids[nozzle_id].push_back(info.ams_id);
        if (pane.current_ams.empty()) pane.current_ams = info.ams_id;
        createAms(pane.ams_book, pane.page_index, info, AMSPanelPos::SINGLE_PANEL);
        pane.ams_book->Fit();
        pane.ams_book->Layout();
    }
    else if (m_total_ext_count == 2){
        if (info.routes_to_main_extruder()){
            createAms(m_simplebook_ams_right, m_right_page_index, info, AMSPanelPos::RIGHT_PANEL);
        }
        else if (!info.routes_to_main_extruder()){
            createAms(m_simplebook_ams_left, m_left_page_index, info, AMSPanelPos::LEFT_PANEL);
        }
    }
    else if (m_total_ext_count == 1){
        createAms(m_simplebook_ams_left, m_left_page_index, info, AMSPanelPos::LEFT_PANEL);
    }
    else {
        // Generic N Nozzles
    }
    m_simplebook_ams_left->Layout();
    m_simplebook_ams_right->Layout();
    m_simplebook_ams_left->Refresh();
    m_simplebook_ams_right->Refresh();

}

//void AMSControl::AddExtraAms(AMSinfo info)
//{
//    auto ams_item = new AmsItem(m_simplebook_extra_cans_left, info, AMSModel::EXTRA_AMS);
//    m_ams_item_list[info.ams_id] = ams_item;
//
//    if (info.nozzle_id == 1)
//    {
//        m_simplebook_extra_cans_left->AddPage(ams_item, wxEmptyString, false);
//        ams_item->m_selection = m_simplebook_extra_cans_left->GetPageCount() - 1;
//    }
//    else if (info.nozzle_id == 0)
//    {
//        m_simplebook_extra_cans_right->AddPage(ams_item, wxEmptyString, false);
//        ams_item->m_selection = m_simplebook_extra_cans_right->GetPageCount() - 1;
//    }
//
//}

void AMSControl::AddAms(std::vector<AMSinfo> single_info, const std::string &series_name, const std::string &printer_type, AMSPanelPos pos)
{
     if (single_info.size() <= 0){
        return;
    }
    if (use_multi_nozzle_layout()) {
        const int nozzle_id = std::clamp(single_info.front().nozzle_id, 0, m_total_ext_count - 1);
        auto &pane = m_nozzle_panes[nozzle_id];
        for (const auto &info : single_info) {
            pane.item_ids.push_back(info.ams_id);
            m_item_ids[nozzle_id].push_back(info.ams_id);
        }
        if (pane.current_ams.empty()) pane.current_ams = single_info.front().ams_id;
        createAmsPanel(pane.ams_book, pane.page_index, single_info, series_name, printer_type, AMSPanelPos::SINGLE_PANEL, m_total_ext_count);
        pane.ams_book->Fit();
        pane.ams_book->Layout();
    }
    else if (m_total_ext_count == 2) {
        if (single_info[0].routes_to_main_extruder()) {
            createAmsPanel(m_simplebook_ams_right, m_right_page_index, single_info, series_name, printer_type, AMSPanelPos::RIGHT_PANEL, m_total_ext_count);
        }
        else if (!single_info[0].routes_to_main_extruder()) {
            createAmsPanel(m_simplebook_ams_left, m_left_page_index, single_info, series_name, printer_type, AMSPanelPos::LEFT_PANEL, m_total_ext_count);
        }
    }
    else if (m_total_ext_count == 1) {
        if (pos == AMSPanelPos::RIGHT_PANEL) {
            createAmsPanel(m_simplebook_ams_right, m_right_page_index, single_info, series_name, printer_type, AMSPanelPos::RIGHT_PANEL, m_total_ext_count);
        }
        else {
            createAmsPanel(m_simplebook_ams_left, m_left_page_index, single_info, series_name, printer_type, AMSPanelPos::LEFT_PANEL, m_total_ext_count);
        }
    }
    else {
        // Generic N Nozzles
    }

    m_simplebook_ams_left->Layout();
    m_simplebook_ams_right->Layout();
    m_simplebook_ams_left->Refresh();
    m_simplebook_ams_right->Refresh();
}

//void AMSControl::AddExtAms(int ams_id) {
//    if (m_ams_item_list.find(std::to_string(ams_id)) != m_ams_item_list.end())
//    {
//        //mode = AMSModel::EXTRA_AMS;
//        AmsItem* ams_item;
//        AMSinfo ext_info;
//
//        if (ams_id == VIRTUAL_TRAY_MAIN_ID)
//        {
//            ext_info.ams_id = std::to_string(VIRTUAL_TRAY_MAIN_ID);
//            ext_info.nozzle_id = 0;
//            ams_item = new AmsItem(m_simplebook_ams_right, ext_info, AMSModel::EXTRA_AMS);
//            m_simplebook_ams_right->AddPage(ams_item, wxEmptyString, false);
//            ams_item->m_selection = m_simplebook_ams_right->GetPageCount() - 1;
//        }
//        else if (ams_id == VIRTUAL_TRAY_DEPUTY_ID)
//        {
//            ext_info.ams_id = std::to_string(VIRTUAL_TRAY_DEPUTY_ID);
//            ext_info.nozzle_id = 1;
//            ams_item = new AmsItem(m_simplebook_ams_left, ext_info, AMSModel::EXTRA_AMS);
//            m_simplebook_ams_left->AddPage(ams_item, wxEmptyString, false);
//            ams_item->m_selection = m_simplebook_ams_left->GetPageCount() - 1;
//        }
//        m_ams_generic_item_list[std::to_string(ams_id)] = ams_item;
//    }
//}

void AMSControl::AddAmsPreview(std::vector<AMSinfo>single_info, AMSPanelPos pos) {
    if (single_info.size() <= 0) return;

    if (use_multi_nozzle_layout()) {
        const int nozzle_id = std::clamp(single_info.front().nozzle_id, 0, m_total_ext_count - 1);
        auto &pane = m_nozzle_panes[nozzle_id];
        for (const auto &info : single_info) {
            auto *ams_prv = new AMSPreview(pane.preview_panel, wxID_ANY, info, info.ams_type);
            pane.preview_sizer->Add(ams_prv, 0, wxALIGN_CENTER | wxLEFT, FromDIP(6));
            ams_prv->Bind(wxEVT_LEFT_DOWN, [this, ams_prv](wxMouseEvent &e) {
                SwitchAms(ams_prv->get_ams_id());
                e.Skip();
            });
            m_ams_preview_list[info.ams_id] = ams_prv;
        }
        return;
    }

    AMSPreview* ams_prv = nullptr;
    AMSPreview* ams_prv2 = nullptr;
    if (pos == AMSPanelPos::RIGHT_PANEL){
        ams_prv = new AMSPreview(m_panel_prv_right, wxID_ANY, single_info[0], single_info[0].ams_type);
        m_sizer_prv_right->Add(ams_prv, 0, wxALIGN_CENTER | wxLEFT, FromDIP(6));
        if (single_info.size() == 2)
        {
            ams_prv2 = new AMSPreview(m_panel_prv_right, wxID_ANY, single_info[1], single_info[1].ams_type);
            m_sizer_prv_right->Add(ams_prv2, 0, wxALIGN_CENTER | wxLEFT, 0);
        }
    }
    else
    {
        ams_prv = new AMSPreview(m_panel_prv_left, wxID_ANY, single_info[0], single_info[0].ams_type);
        m_sizer_prv_left->Add(ams_prv, 0, wxALIGN_CENTER | wxLEFT, FromDIP(6));
        if (single_info.size() == 2)
        {
            ams_prv2 = new AMSPreview(m_panel_prv_left, wxID_ANY, single_info[1], single_info[1].ams_type);
            m_sizer_prv_left->Add(ams_prv2, 0, wxALIGN_CENTER | wxLEFT, 0);
        }
    }

    if (ams_prv) {
        ams_prv->Bind(wxEVT_LEFT_DOWN, [this, ams_prv](wxMouseEvent& e) {
            SwitchAms(ams_prv->get_ams_id());
            e.Skip();
            });
        m_ams_preview_list[single_info[0].ams_id] = ams_prv;
    }
    if (ams_prv2) {
        ams_prv2->Bind(wxEVT_LEFT_DOWN, [this, ams_prv2](wxMouseEvent& e) {
            SwitchAms(ams_prv2->get_ams_id());
            e.Skip();
            });
        m_ams_preview_list[single_info[1].ams_id] = ams_prv2;
    }
}

void AMSControl::SelectNozzle(int nozzle_id)
{
    if (nozzle_id < 0 || nozzle_id >= static_cast<int>(m_nozzle_panes.size()))
        return;

    if (m_active_nozzle_id == nozzle_id &&
        (!m_nozzle_book || m_nozzle_book->GetSelection() == nozzle_id))
        return;

    m_active_nozzle_id = nozzle_id;
    if (m_nozzle_book && m_nozzle_book->GetSelection() != nozzle_id)
        m_nozzle_book->SetSelection(nozzle_id);
    m_current_ams = m_nozzle_panes[nozzle_id].current_ams;
    if (!m_current_ams.empty()) {
        SwitchAms(m_current_ams);
        return;
    }
    if (m_down_road) {
        m_down_road->UpdateLeft(1, AMSRoadShowMode::AMS_ROAD_MODE_NONE);
        m_down_road->UpdateRight(1, AMSRoadShowMode::AMS_ROAD_MODE_NONE);
    }
    if (m_extruder)
        m_extruder->OnAmsLoading(false, 0);
    post_event(SimpleEvent(EVT_AMS_SWITCH));
}

void AMSControl::SwitchAms(std::string ams_id)
{
    if (use_multi_nozzle_layout()) {
        NozzleAmsPane *pane = nullptr;
        int nozzle_id = -1;
        for (int index = 0; index < static_cast<int>(m_nozzle_panes.size()); ++index) {
            auto &candidate = m_nozzle_panes[index];
            if (std::find(candidate.item_ids.begin(), candidate.item_ids.end(), ams_id) != candidate.item_ids.end()) {
                pane = &candidate;
                nozzle_id = index;
                break;
            }
        }
        if (!pane) return;

        m_active_nozzle_id = nozzle_id;
        if (m_nozzle_book && m_nozzle_book->GetSelection() != nozzle_id)
            m_nozzle_book->SetSelection(nozzle_id);

        pane->current_ams = ams_id;
        m_current_ams = ams_id;
        m_current_select = ams_id;

        for (const auto &id : pane->item_ids) {
            auto preview = m_ams_preview_list.find(id);
            if (preview == m_ams_preview_list.end() || !preview->second) continue;
            if (id == ams_id) preview->second->OnSelected();
            else preview->second->UnSelected();
        }

        auto item = m_ams_item_list.find(ams_id);
        if (item != m_ams_item_list.end() && item->second) {
            if (pane->ams_book)
                pane->ams_book->SetSelection(item->second->get_selection());

            AMSRoadShowMode road_mode = AMSRoadShowMode::AMS_ROAD_MODE_SINGLE;
            if (item->second->get_can_count() == GENERIC_AMS_SLOT_NUM)
                road_mode = AMSRoadShowMode::AMS_ROAD_MODE_FOUR;
            else if (item->second->get_can_count() > GENERIC_AMS_SLOT_NUM)
                road_mode = AMSRoadShowMode::AMS_ROAD_MODE_GENERIC;
            else if (item->second->get_ams_model() == AMSModel::N3S_AMS)
                road_mode = AMSRoadShowMode::AMS_ROAD_MODE_SINGLE_N3S;
            else if (item->second->get_ams_model() == AMSModel::AMS_LITE ||
                     (item->second->get_ams_model() == AMSModel::EXT_AMS &&
                      item->second->get_ext_type() == AMSModelOriginType::LITE_EXT))
                road_mode = AMSRoadShowMode::AMS_ROAD_MODE_AMS_LITE;

            if (m_down_road) {
                if (pane->panel_pos == AMSPanelPos::LEFT_PANEL) {
                    m_down_road->UpdateLeft(1, road_mode);
                    m_down_road->UpdateRight(1, AMSRoadShowMode::AMS_ROAD_MODE_NONE);
                } else {
                    m_down_road->UpdateLeft(1, AMSRoadShowMode::AMS_ROAD_MODE_NONE);
                    m_down_road->UpdateRight(1, road_mode);
                }
                m_down_road->UpdatePassRoad(pane->panel_pos, -1, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
            }
            if (m_extruder)
                m_extruder->OnAmsLoading(false, 0);
        }

        post_event(SimpleEvent(EVT_AMS_SWITCH));
        return;
    }

    if(ams_id == m_current_show_ams_left || ams_id == m_current_show_ams_right){return;}

    bool is_in_right = IsAmsInRightPanel(ams_id);
    if (is_in_right){
        m_current_show_ams_right = ams_id;
        m_down_road->UpdatePassRoad(AMSPanelPos::RIGHT_PANEL, -1, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
    }
    else{
        m_current_show_ams_left = ams_id;
        m_down_road->UpdatePassRoad(AMSPanelPos::LEFT_PANEL, -1, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
    }


    for (auto prv_it : m_ams_preview_list) {
        AMSPreview* prv = prv_it.second;
        if (prv->get_ams_id() == m_current_show_ams_left || prv->get_ams_id() == m_current_show_ams_right) {
            prv->OnSelected();
            m_current_select = ams_id;

            bool ready_selected = false;
            for (auto item_it : m_ams_item_list) {
                AmsItem* item = item_it.second;
                if (item->get_ams_id() == ams_id) {
                    for (auto lib_it : item->get_can_lib_list()) {
                        AMSLib* lib = lib_it.second;
                        if (lib->is_selected()) {
                            ready_selected = true;
                        }
                    }
                }
            }
            if (is_in_right){
                m_current_show_ams_right = ams_id;
            }
            else{
                m_current_show_ams_left = ams_id;
            }

        } else {
            prv->UnSelected();
        }
    }

    for (auto ams_item : m_ams_item_list) {
        AmsItem* item = ams_item.second;
        if (item->get_ams_id() == ams_id) {
            auto ids = item->get_panel_pos() == AMSPanelPos::LEFT_PANEL ? m_item_ids[DEPUTY_EXTRUDER_ID] : m_item_ids[MAIN_EXTRUDER_ID];
            auto pos = item->get_panel_pos();
            for (auto id : ids) {
                if (id == item->get_ams_id()) {
                    pos == AMSPanelPos::LEFT_PANEL ? m_simplebook_ams_left->SetSelection(item->get_selection()) : m_simplebook_ams_right->SetSelection(item->get_selection());
                    if (item->get_can_count() == GENERIC_AMS_SLOT_NUM) {
                        if (item->get_ams_model() == AMSModel::AMS_LITE) {
                            if (pos == AMSPanelPos::LEFT_PANEL) {
                                m_down_road->UpdateLeft(m_total_ext_count, AMSRoadShowMode::AMS_ROAD_MODE_AMS_LITE);
                            } else {
                                m_down_road->UpdateRight(m_total_ext_count, AMSRoadShowMode::AMS_ROAD_MODE_AMS_LITE);
                            }
                        }
                        else {
                            if (pos == AMSPanelPos::LEFT_PANEL) {
                                m_down_road->UpdateLeft(m_total_ext_count, AMSRoadShowMode::AMS_ROAD_MODE_FOUR);
                            } else {
                                m_down_road->UpdateRight(m_total_ext_count, AMSRoadShowMode::AMS_ROAD_MODE_FOUR);
                            }
                        }
                    }
                    else {
                        AMSRoadShowMode mode = AMSRoadShowMode::AMS_ROAD_MODE_SINGLE;

                        if (item->get_can_count() > GENERIC_AMS_SLOT_NUM)
                            mode = AMSRoadShowMode::AMS_ROAD_MODE_GENERIC;
                        else if (item->get_ams_model() == AMSModel::N3S_AMS)
                            mode = AMSRoadShowMode::AMS_ROAD_MODE_SINGLE_N3S;

                        if (item->get_can_count() <= GENERIC_AMS_SLOT_NUM) {
                            for (auto it : pair_id) {
                                if (it.first == ams_id || it.second == ams_id) {
                                    mode = AMSRoadShowMode::AMS_ROAD_MODE_DOUBLE;
                                    break;
                                }
                            }
                        }
                        pos == AMSPanelPos::LEFT_PANEL ? m_down_road->UpdateLeft(m_total_ext_count, mode)
                            : m_down_road->UpdateRight(m_total_ext_count, mode);
                        if (pos == AMSPanelPos::LEFT_PANEL) {
                            m_down_road->UpdatePassRoad(AMSPanelPos::LEFT_PANEL, -1, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
                        } else {
                            m_down_road->UpdatePassRoad(AMSPanelPos::RIGHT_PANEL, -1, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
                        }
                    }
                }
            }
        }
    }

    post_event(SimpleEvent(EVT_AMS_SWITCH));
}

void AMSControl::ShowFilamentTip(bool hasams)
{
    //m_simplebook_right->SetSelection(0);
    if (hasams) {
        m_tip_right_top->Show();
        m_tip_load_info->SetLabelText(_L("Choose an AMS slot then press \"Load\" or \"Unload\" button to automatically load or unload filament."));
    } else {
        // m_tip_load_info->SetLabelText(_L("Before loading, please make sure the filament is pushed into toolhead."));
        m_tip_right_top->Hide();
        m_tip_load_info->SetLabelText(wxEmptyString);
    }

    m_tip_load_info->SetMinSize(AMS_STEP_SIZE);
    m_tip_load_info->Wrap(AMS_STEP_SIZE.x - FromDIP(5));
    m_sizer_right_tip->Layout();
}

bool AMSControl::Enable(bool enable)
{
    for (auto prv_it : m_ams_preview_list) {
        AMSPreview* prv = prv_it.second;
        prv->Enable(enable);
    }

    for (auto item_it : m_ams_item_list) {
        AmsItem* item = item_it.second;
        item->Enable(enable);
    }

    m_button_extruder_feed->Enable(enable);
    m_button_extruder_back->Enable(enable);
    m_button_auto_refill->Enable(enable);
    m_button_ams_setting->Enable(enable);

    m_filament_load_step->Enable(enable);
    return wxWindow::Enable(enable);
}

void AMSControl::SetExtruder(bool on_off, int nozzle_id, std::string ams_id, std::string slot_id)
{
    AmsItem *item = nullptr;
    if (m_ams_item_list.find(ams_id) != m_ams_item_list.end()) { item = m_ams_item_list[ams_id]; }

    if (use_multi_nozzle_layout()) {
        if (nozzle_id < 0 || nozzle_id >= static_cast<int>(m_nozzle_panes.size()))
            return;

        if (!m_extruder)
            return;

        if (on_off && item)
            m_extruder->OnAmsLoading(true, 0, item->GetTagColr(slot_id));
        else
            m_extruder->OnAmsLoading(false, 0);
        return;
    }

    if (on_off && item) {
        auto col = item->GetTagColr(slot_id);
        m_extruder->OnAmsLoading(true, nozzle_id, col);
    }
    else {
        m_extruder->OnAmsLoading(false, nozzle_id);
    }
}

void AMSControl::SetAmsStep(std::string ams_id, std::string canid, AMSPassRoadType type, AMSPassRoadSTEP step)
{
    AmsItem* ams = nullptr;
    auto amsit = m_ams_item_list.find(ams_id);
    bool in_same_page = false;

    if (amsit != m_ams_item_list.end()) {ams = amsit->second;}
    else {return;}
    if (ams == nullptr) return;

    m_last_ams_id = ams_id;
    m_last_tray_id = canid;
    int can_index = atoi(canid.c_str());

    std::vector<std::string> cur_left_ams;
    std::vector<std::string> cur_right_ams;

    std::string ams_id_left = GetCurentShowAms(AMSPanelPos::LEFT_PANEL);
    std::string ams_id_right = GetCurentShowAms(AMSPanelPos::RIGHT_PANEL);

    for (auto it : pair_id) {
        if ((ams_id_left == it.first || ams_id_left == it.second)) {
            cur_left_ams.push_back(it.first);
            cur_left_ams.push_back(it.second);
        }
        else if ((ams_id_right == it.first || ams_id_right == it.second)) {
            cur_right_ams.push_back(it.first);
            cur_right_ams.push_back(it.second);
        }
    }

    auto left = !IsAmsInRightPanel(ams_id);
    auto length = -1;
    auto model = AMSModel::AMS_LITE;
    auto in_pair = false;

    if (std::find(cur_left_ams.begin(), cur_left_ams.end(), ams_id) != cur_left_ams.end()) {
        in_same_page = true;
    }

    if (std::find(cur_right_ams.begin(), cur_right_ams.end(), ams_id) != cur_right_ams.end()) {
        in_same_page = true;
    }

    //Set path length in different case
    model  = ams->get_ams_model();

    if (ams->get_can_count() == GENERIC_AMS_SLOT_NUM) {
        length = left ? 129 : 145;
    } else if (ams->get_can_count() == 1) {
        for (auto it : pair_id){
            if (it.first == ams_id){
                length = left ? 218 : 124;
                in_pair = true;
                break;
            }
            else if (it.second == ams_id){
                length = left ? 124 : 232;
                in_pair = true;
                break;
            }
        }

        if (!in_pair && model == N3S_AMS) {
            length = left ? 129 : 232;
        }
    }

    if (model == AMSModel::AMS_LITE){
        length = left ? 145 : 45;
    }
    if (model == EXT_AMS && ams->get_ext_type() == AMSModelOriginType::LITE_EXT) {

       if (m_ams_info.size() == 0 && m_ext_info.size() == 1) {
           length = 13;
       } else {
           length = 145;
       }
    }

    if (model == EXT_AMS && ams->get_ext_type() == AMSModelOriginType::GENERIC_EXT) {
        if (m_ams_info.size() == 0 && m_ext_info.size() == 1) {
            left = true;
            length = 50;
        } else {
            /*check in pair*/
            if (in_pair) {
                length = left ? 110 : 232;
            } else {
                length = left ? 192 : 82;
            }
        }
    }

    for (auto i = 0; i < m_ams_info.size(); i++) {
        if (m_ams_info[i].ams_id == ams_id) {
            m_ams_info[i].current_step = step;
            m_ams_info[i].current_can_id = canid;
        }
    }
    for (auto i = 0; i < m_ext_info.size(); i++) {
        if (m_ext_info[i].ams_id == ams_id) {
            m_ext_info[i].current_step = step;
            m_ext_info[i].current_can_id = canid;
        }
    }


    AMSinfo info;
    if (m_ams_item_list.find(ams_id) != m_ams_item_list.end()) {
        info = m_ams_item_list[ams_id]->get_ams_info();
    }
    else{
        return;
    }

    if (use_multi_nozzle_layout()) {
        const int nozzle_id = std::clamp(ams->get_nozzle_id(), 0, m_total_ext_count - 1);
        if (nozzle_id >= static_cast<int>(m_nozzle_panes.size()))
            return;
        auto &pane = m_nozzle_panes[nozzle_id];
        AMSRoadShowMode road_mode = AMSRoadShowMode::AMS_ROAD_MODE_SINGLE;
        if (ams->get_can_count() == GENERIC_AMS_SLOT_NUM)
            road_mode = AMSRoadShowMode::AMS_ROAD_MODE_FOUR;
        else if (ams->get_can_count() > GENERIC_AMS_SLOT_NUM)
            road_mode = AMSRoadShowMode::AMS_ROAD_MODE_GENERIC;
        else if (ams->get_ams_model() == AMSModel::N3S_AMS)
            road_mode = AMSRoadShowMode::AMS_ROAD_MODE_SINGLE_N3S;
        else if (ams->get_ams_model() == AMSModel::AMS_LITE ||
                 (ams->get_ams_model() == AMSModel::EXT_AMS && ams->get_ext_type() == AMSModelOriginType::LITE_EXT))
            road_mode = AMSRoadShowMode::AMS_ROAD_MODE_AMS_LITE;

        if (can_index >= 0 && can_index < static_cast<int>(info.cans.size()) && m_down_road)
            m_down_road->SetPassRoadColour(pane.panel_pos == AMSPanelPos::LEFT_PANEL,
                                           info.cans[can_index].material_colour);
        if (m_down_road) {
            if (pane.panel_pos == AMSPanelPos::LEFT_PANEL) {
                m_down_road->UpdateLeft(1, road_mode);
                m_down_road->UpdateRight(1, AMSRoadShowMode::AMS_ROAD_MODE_NONE);
            } else {
                m_down_road->UpdateLeft(1, AMSRoadShowMode::AMS_ROAD_MODE_NONE);
                m_down_road->UpdateRight(1, road_mode);
            }
        }

        int road_length = 0;

        if (step == AMSPassRoadSTEP::AMS_ROAD_STEP_NONE) {
            ams->SetAmsStep(ams_id, canid, type, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
            if (m_down_road)
                m_down_road->UpdatePassRoad(pane.panel_pos, -1, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
            if (m_extruder)
                m_extruder->OnAmsLoading(false, 0);
        }
        else if (step == AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP1) {
            ams->SetAmsStep(ams_id, canid, type, AMSPassRoadSTEP::AMS_ROAD_STEP_1);
            if (m_down_road)
                m_down_road->UpdatePassRoad(pane.panel_pos, road_length, AMSPassRoadSTEP::AMS_ROAD_STEP_1);
            if (m_extruder)
                m_extruder->OnAmsLoading(false, 0);
        }
        else if (step == AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP2) {
            ams->SetAmsStep(ams_id, canid, type, AMSPassRoadSTEP::AMS_ROAD_STEP_2);
            if (m_down_road)
                m_down_road->UpdatePassRoad(pane.panel_pos, road_length, AMSPassRoadSTEP::AMS_ROAD_STEP_2);
            if (m_extruder)
                m_extruder->OnAmsLoading(true, 0, ams->GetTagColr(canid));
        }
        else if (step == AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP3) {
            ams->SetAmsStep(ams_id, canid, type, AMSPassRoadSTEP::AMS_ROAD_STEP_3);
            if (m_down_road)
                m_down_road->UpdatePassRoad(pane.panel_pos, road_length, AMSPassRoadSTEP::AMS_ROAD_STEP_3);
            if (m_extruder)
                m_extruder->OnAmsLoading(true, 0, ams->GetTagColr(canid));
        }
        return;
    }

    if (can_index >= 0 && can_index < info.cans.size())
    {
        m_down_road->SetPassRoadColour(left, info.cans[can_index].material_colour);
    }

    AMSPanelPos pos = left ? AMSPanelPos::LEFT_PANEL : AMSPanelPos::RIGHT_PANEL;

    if (step == AMSPassRoadSTEP::AMS_ROAD_STEP_NONE) {
        //cans->SetAmsStep(canid, type, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
        ams->SetAmsStep(ams_id, canid, type, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
        if (ams_id_left == ams_id || ams_id_right == ams_id || in_same_page) {
            m_down_road->UpdatePassRoad(pos, -1, AMSPassRoadSTEP::AMS_ROAD_STEP_NONE);
            m_extruder->OnAmsLoading(false, ams->get_nozzle_id());
        }
    }

    if (step == AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP1) {
        ams->SetAmsStep(ams_id, canid, type, AMSPassRoadSTEP::AMS_ROAD_STEP_1);
        if (ams_id_left == ams_id || ams_id_right == ams_id || in_same_page) {
            m_down_road->UpdatePassRoad(pos, length, AMSPassRoadSTEP::AMS_ROAD_STEP_1);
            m_extruder->OnAmsLoading(false, ams->get_nozzle_id());
        }
        else
        {
            m_down_road->UpdatePassRoad(pos, -1, AMSPassRoadSTEP::AMS_ROAD_STEP_1);
        }
    }

    if (step == AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP2) {
        ams->SetAmsStep(ams_id, canid, type, AMSPassRoadSTEP::AMS_ROAD_STEP_2);
        if (ams_id_left == ams_id || ams_id_right == ams_id || in_same_page) {
            m_down_road->UpdatePassRoad(pos, length, AMSPassRoadSTEP::AMS_ROAD_STEP_2);
            m_extruder->OnAmsLoading(true, ams->get_nozzle_id(), ams->GetTagColr(canid));
        }
        else
        {
            m_down_road->UpdatePassRoad(pos, -1, AMSPassRoadSTEP::AMS_ROAD_STEP_2);
        }
    }
    if (step == AMSPassRoadSTEP::AMS_ROAD_STEP_COMBO_LOAD_STEP3) {
        ams->SetAmsStep(ams_id, canid, type, AMSPassRoadSTEP::AMS_ROAD_STEP_3);
        if (ams_id_left == ams_id || ams_id_right == ams_id || in_same_page)
        {
            m_down_road->UpdatePassRoad(pos, length, AMSPassRoadSTEP::AMS_ROAD_STEP_3);
            m_extruder->OnAmsLoading(true, ams->get_nozzle_id(), ams->GetTagColr(canid));
        }
        else
        {
            m_down_road->UpdatePassRoad(pos, -1, AMSPassRoadSTEP::AMS_ROAD_STEP_3);
        }
    }
}

void AMSControl::on_filament_load(wxCommandEvent &event)
{
    /*If the filament is unknown, show warning*/
    const auto& filament_id = get_filament_id(m_current_ams, GetCurrentCan(m_current_ams));
    if (filament_id.empty())
    {
        MessageDialog msg_dlg(nullptr, _L("Filament type is unknown which is required to perform this action. Please set target filament's informations."),
                              wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    for (auto i = 0; i < m_ams_info.size(); i++) {
        if (m_ams_info[i].ams_id == m_current_ams) { m_ams_info[i].current_action = AMSAction::AMS_ACTION_LOAD; }
    }
    post_event(SimpleEvent(EVT_AMS_LOAD));
}

void AMSControl::on_extrusion_cali(wxCommandEvent &event)
{
    for (auto i = 0; i < m_ams_info.size(); i++) {
        if (m_ams_info[i].ams_id == m_current_ams) { m_ams_info[i].current_action = AMSAction::AMS_ACTION_CALI; }
    }
    post_event(SimpleEvent(EVT_AMS_EXTRUSION_CALI));
}

void AMSControl::on_filament_unload(wxCommandEvent &event)
{
    for (auto i = 0; i < m_ams_info.size(); i++) {
        if (m_ams_info[i].ams_id == m_current_ams) { m_ams_info[i].current_action = AMSAction::AMS_ACTION_UNLOAD; }
    }
    post_event(SimpleEvent(EVT_AMS_UNLOAD));
}

void AMSControl::auto_refill(wxCommandEvent& event)
{
    post_event(SimpleEvent(EVT_AMS_FILAMENT_BACKUP));
}

void AMSControl::on_ams_setting_click(wxMouseEvent &event)
{
    post_event(SimpleEvent(EVT_AMS_SETTINGS));
}

std::tuple<bool, bool> AMSControl::isFilaSwitchReady()
{
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return {false, false};
    MachineObject* obj = dev->get_selected_machine();
    if (!obj) return {false, false};
    // Orca: GetFilaSwitch() is a raw non-null accessor (BBS returns a shared_ptr).
    DevFilaSwitch* fila_switch = obj->GetFilaSwitch();
    if (fila_switch)
    {
        return {fila_switch->IsInstalled(), fila_switch->IsReady()};
    }
    return {false, false};
}

void AMSControl::show_switcher_status(bool show)
{
    // Orca: never materialize the banner on printers that never request it, so the AMS control is
    // byte-identical without a Filament Track Switch (UpdateAms calls this with show=false every refresh).
    if (!show && tipPanel == nullptr) { return; }

    if (tipPanel == nullptr)
    {
        m_sizer_body->Add(0, 0, 1, wxEXPAND | wxTOP, FromDIP(5));
        tipPanel = new wxPanel(m_amswin);
        tipPanel->SetBackgroundColour(wxColour(255, 153, 0));
        tipSizer = new wxBoxSizer(wxHORIZONTAL);
        tipPanel->SetSizer(tipSizer);
        icon = new wxStaticBitmap(tipPanel, wxID_ANY,
            wxArtProvider::GetBitmap(wxART_INFORMATION, wxART_MESSAGE_BOX, wxSize(FromDIP(16), FromDIP(16))));
        tipSizer->Add(icon, 0, wxALL, FromDIP(8));
        tipText = new wxStaticText(tipPanel, wxID_ANY, _L("AMS has not been initialized. Please initialize it before use."));
        tipText->SetForegroundColour(wxColour(255, 255, 255));
        tipText->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
        tipText->Wrap(-1);
        tipText->SetMinSize(wxSize(-1, -1));
        tipSizer->Add(tipText, 0, wxALL | wxALIGN_CENTER_VERTICAL | wxEXPAND, FromDIP(8));
        m_sizer_body->Add(tipPanel, 1, wxEXPAND, 0);
    }
    if (tipPanel->IsShown() == show)
    {
        return;
    }
    tipPanel->Show(show);
    m_amswin->Layout();
    m_amswin->Fit();
}

void AMSControl::parse_object(MachineObject* obj) {
    if (!obj || obj->GetFilaSystem()->GetAmsList().size() == 0)
    {
        return;
    }
    m_ams_info.clear();
    for (auto ams : obj->GetFilaSystem()->GetAmsList())
    {
        AMSinfo info;
        info.parse_ams_info(obj, ams.second);
        m_ams_info.push_back(info);
    }
}

std::string AMSControl::get_filament_id(const std::string& ams_id, const std::string& can_id)
{
    for (const auto& ams_info : m_ams_info)
    {
        if (ams_info.ams_id == m_current_ams)
        {
            bool found = false;
            const auto& can_info = ams_info.get_caninfo(this->GetCurrentCan(m_current_ams), found);
            if (found)
            {
                return can_info.filament_id;
            }
        }
    }

    for (const auto& ext_info : m_ext_info)
    {
        if (ext_info.ams_id == m_current_ams)
        {
            bool found = false;
            const auto& can_info = ext_info.get_caninfo(this->GetCurrentCan(m_current_ams), found);
            if (found)
            {
                return can_info.filament_id;
            }
        }
    }

    return std::string();
}

void AMSControl::on_clibration_again_click(wxMouseEvent &event) { post_event(SimpleEvent(EVT_AMS_CLIBRATION_AGAIN)); }

void AMSControl::on_clibration_cancel_click(wxMouseEvent &event) { post_event(SimpleEvent(EVT_AMS_CLIBRATION_CANCEL)); }

void AMSControl::post_event(wxEvent &&event)
{
    event.SetEventObject(m_parent);
    wxPostEvent(m_parent, event);
}

}} // namespace Slic3r::GUI
