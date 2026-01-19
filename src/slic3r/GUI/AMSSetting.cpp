#include "AMSSetting.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"

#include "slic3r/GUI/MsgDialog.hpp"

#include "slic3r/GUI/Widgets/AnimaController.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"

namespace Slic3r { namespace GUI {

AMSSetting::AMSSetting(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size, long style)
    : DPIDialog(parent, id, wxEmptyString, pos, size, style)
{
    create();
    wxGetApp().UpdateDlgDarkUI(this);
}
AMSSetting::~AMSSetting() {}

void AMSSetting::create()
{
    wxBoxSizer *m_sizer_main;
    m_sizer_main = new wxBoxSizer(wxVERTICAL);
    SetBackgroundColour(*wxWHITE);

    m_static_ams_settings = new wxStaticText(this, wxID_ANY, _L("AMS Settings"), wxDefaultPosition, wxDefaultSize, 0);
    m_static_ams_settings->SetFont(::Label::Head_14);
    m_static_ams_settings->SetForegroundColour(AMS_SETTING_GREY800);


    m_panel_body = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, -1), wxTAB_TRAVERSAL);
    m_panel_body->SetBackgroundColour(*wxWHITE);
    wxBoxSizer *m_sizerl_body = new wxBoxSizer(wxVERTICAL);

    m_ams_type = new AMSSettingTypePanel(m_panel_body, this);
    m_ams_type->Show(false);

    //m_ams_arrange_order = new AMSSettingArrangeAMSOrder(m_panel_body);
    //m_ams_arrange_order->Show(false);

    m_panel_Insert_material = new wxPanel(m_panel_body, wxID_ANY, wxDefaultPosition, wxSize(-1, -1), wxTAB_TRAVERSAL);
    m_panel_Insert_material->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* m_sizer_main_Insert_material = new wxBoxSizer(wxVERTICAL);

    // checkbox area 1
    wxBoxSizer *m_sizer_Insert_material  = new wxBoxSizer(wxHORIZONTAL);
    m_checkbox_Insert_material_auto_read = new ::CheckBox(m_panel_Insert_material);
    m_checkbox_Insert_material_auto_read->Bind(wxEVT_TOGGLEBUTTON, &AMSSetting::on_insert_material_read, this);
    m_sizer_Insert_material->Add(m_checkbox_Insert_material_auto_read, 0, wxALIGN_CENTER_VERTICAL);

    m_sizer_Insert_material->Add(0, 0, 0, wxLEFT, 12);

    m_title_Insert_material_auto_read = new wxStaticText(m_panel_Insert_material, wxID_ANY, _L("Insertion update"),
                                                         wxDefaultPosition, wxDefaultSize, 0);

    m_title_Insert_material_auto_read->SetFont(::Label::Head_13);
    m_title_Insert_material_auto_read->SetForegroundColour(AMS_SETTING_GREY800);
    m_title_Insert_material_auto_read->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_Insert_material->Add(m_title_Insert_material_auto_read, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT, 0);

    wxBoxSizer *m_sizer_Insert_material_tip = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_Insert_material_tip_inline      = new wxBoxSizer(wxVERTICAL);
    m_sizer_Insert_material_tip->Add(0, 0, 0, wxLEFT, 10);

    // tip line1
    m_tip_Insert_material_line1 = new Label(m_panel_Insert_material,
        _L("The AMS will automatically read the filament information when inserting a new Bambu Lab filament. This takes about 20 seconds.")
    );
    m_tip_Insert_material_line1->SetFont(::Label::Body_13);
    m_tip_Insert_material_line1->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_Insert_material_line1->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_Insert_material_line1->Wrap(AMS_SETTING_BODY_WIDTH);
    m_tip_Insert_material_line1->Hide();
    m_sizer_Insert_material_tip_inline->Add(m_tip_Insert_material_line1, 0, wxEXPAND, 0);

    // tip line2
    m_tip_Insert_material_line2 = new Label(m_panel_Insert_material,
        _L("Note: if a new filament is inserted during printing, the AMS will not automatically read any information until printing is completed.")
    );
    m_tip_Insert_material_line2->SetFont(::Label::Body_13);
    m_tip_Insert_material_line2->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_Insert_material_line2->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_Insert_material_line2->Wrap(AMS_SETTING_BODY_WIDTH);
    m_tip_Insert_material_line2->Hide();
    m_sizer_Insert_material_tip_inline->Add(m_tip_Insert_material_line2, 0, wxEXPAND | wxTOP, 8);

    // tip line3
    m_tip_Insert_material_line3 = new Label(m_panel_Insert_material,
        _L("When inserting a new filament, the AMS will not automatically read its information, leaving it blank for you to enter manually.")
    );
    m_tip_Insert_material_line3->SetFont(::Label::Body_13);
    m_tip_Insert_material_line3->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_Insert_material_line3->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_Insert_material_line3->Wrap(AMS_SETTING_BODY_WIDTH);
    m_tip_Insert_material_line3->Hide();
    m_sizer_Insert_material_tip_inline->Add(m_tip_Insert_material_line3, 0, wxEXPAND, 0);

    m_sizer_Insert_material_tip->Add(m_sizer_Insert_material_tip_inline, 1, wxALIGN_CENTER, 0);

    m_sizer_main_Insert_material->Add(m_sizer_Insert_material, 0, wxEXPAND | wxTOP, FromDIP(4));
    m_sizer_main_Insert_material->Add(m_sizer_Insert_material_tip, 0, wxEXPAND | wxLEFT | wxTOP, FromDIP(10));
    m_panel_Insert_material->SetSizer(m_sizer_main_Insert_material);

    // checkbox area 2
    wxBoxSizer *m_sizer_starting = new wxBoxSizer(wxHORIZONTAL);
    m_checkbox_starting_auto_read = new ::CheckBox(m_panel_body);
    m_checkbox_starting_auto_read->Bind(wxEVT_TOGGLEBUTTON, &AMSSetting::on_starting_read, this);
    m_sizer_starting->Add(m_checkbox_starting_auto_read, 0, wxALIGN_CENTER_VERTICAL);
    m_sizer_starting->Add(0, 0, 0, wxLEFT, 12);
    m_title_starting_auto_read = new wxStaticText(m_panel_body, wxID_ANY, _L("Power on update"), wxDefaultPosition,wxDefaultSize, 0);
    m_title_starting_auto_read->SetFont(::Label::Head_13);
    m_title_starting_auto_read->SetForegroundColour(AMS_SETTING_GREY800);
    m_title_starting_auto_read->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_starting->Add(m_title_starting_auto_read, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT, 0);

    wxBoxSizer *m_sizer_starting_tip = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_starting_tip->Add(0, 0, 0, wxLEFT, 10);

    // tip line
    m_sizer_starting_tip_inline = new wxBoxSizer(wxVERTICAL);

    m_tip_starting_line1 = new Label(m_panel_body,
        _L("The AMS will automatically read the information of inserted filament on start-up. It will take about 1 minute. The reading process will roll the filament spools.")
    );
    m_tip_starting_line1->SetFont(::Label::Body_13);
    m_tip_starting_line1->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_starting_line1->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_starting_line1->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_starting_tip_inline->Add(m_tip_starting_line1, 0, wxEXPAND, 0);

    m_tip_starting_line2 = new Label(m_panel_body,
        _L("The AMS will not automatically read information from inserted filament during startup and will continue to use the information recorded before the last shutdown.")
    );
    m_tip_starting_line2->SetFont(::Label::Body_13);
    m_tip_starting_line2->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_starting_line2->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_starting_line2->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_starting_tip_inline->Add(m_tip_starting_line2, 0, wxEXPAND,0);
    m_sizer_starting_tip->Add(m_sizer_starting_tip_inline, 1, wxALIGN_CENTER, 0);

    // checkbox area 3
    wxBoxSizer* m_sizer_remain = new wxBoxSizer(wxHORIZONTAL);
    m_checkbox_remain = new ::CheckBox(m_panel_body);
    m_checkbox_remain->Bind(wxEVT_TOGGLEBUTTON, &AMSSetting::on_remain, this);
    m_sizer_remain->Add(m_checkbox_remain, 0, wxALIGN_CENTER_VERTICAL);
    m_sizer_remain->Add(0, 0, 0, wxLEFT, 12);
    m_title_remain = new wxStaticText(m_panel_body, wxID_ANY, _L("Update remaining capacity"), wxDefaultPosition, wxDefaultSize, 0);
    m_title_remain->SetFont(::Label::Head_13);
    m_title_remain->SetForegroundColour(AMS_SETTING_GREY800);
    m_title_remain->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_remain->Add(m_title_remain, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT, 0);



    wxBoxSizer* m_sizer_remain_tip = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_remain_tip->Add(0, 0, 0, wxLEFT, 10);

    // tip line
    m_sizer_remain_inline = new wxBoxSizer(wxVERTICAL);

    m_tip_remain_line1 = new Label(m_panel_body, _L("AMS will attempt to estimate the remaining capacity of the Bambu Lab filaments."));
    m_tip_remain_line1->SetFont(::Label::Body_13);
    m_tip_remain_line1->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_remain_line1->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_remain_line1->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_remain_inline->Add(m_tip_remain_line1, 0, wxEXPAND, 0);
    m_sizer_remain_tip->Add(m_sizer_remain_inline, 1, wxALIGN_CENTER, 0);

    // checkbox area 4
    wxBoxSizer* m_sizer_switch_filament = new wxBoxSizer(wxHORIZONTAL);
    m_checkbox_switch_filament = new ::CheckBox(m_panel_body);
    m_checkbox_switch_filament->Bind(wxEVT_TOGGLEBUTTON, &AMSSetting::on_switch_filament, this);
    m_sizer_switch_filament->Add(m_checkbox_switch_filament, 0, wxALIGN_CENTER_VERTICAL);
    m_sizer_switch_filament->Add(0, 0, 0, wxLEFT, 12);
    m_title_switch_filament = new wxStaticText(m_panel_body, wxID_ANY, _L("AMS filament backup"), wxDefaultPosition, wxDefaultSize, 0);
    m_title_switch_filament->SetFont(::Label::Head_13);
    m_title_switch_filament->SetForegroundColour(AMS_SETTING_GREY800);
    m_title_switch_filament->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_switch_filament->Add(m_title_switch_filament, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT, 0);



    wxBoxSizer* m_sizer_switch_filament_tip = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_switch_filament_tip->Add(0, 0, 0, wxLEFT, 10);

    // tip line
    m_sizer_switch_filament_inline = new wxBoxSizer(wxVERTICAL);

    m_tip_switch_filament_line1 = new Label(m_panel_body,
        _L("AMS will continue to another spool with matching filament properties automatically when current filament runs out.")
    );
    m_tip_switch_filament_line1->SetFont(::Label::Body_13);
    m_tip_switch_filament_line1->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_switch_filament_line1->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_switch_filament_line1->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_switch_filament_inline->Add(m_tip_switch_filament_line1, 0, wxEXPAND, 0);
    m_sizer_switch_filament_tip->Add(m_sizer_switch_filament_inline, 1, wxALIGN_CENTER, 0);



    // checkbox area 5
    wxBoxSizer* m_sizer_air_print = new wxBoxSizer(wxHORIZONTAL);
    m_checkbox_air_print = new ::CheckBox(m_panel_body);
    m_checkbox_air_print->Bind(wxEVT_TOGGLEBUTTON, &AMSSetting::on_air_print_detect, this);
    m_sizer_air_print->Add(m_checkbox_air_print, 0, wxTOP, 1);
    m_sizer_air_print->Add(0, 0, 0, wxLEFT, 12);
    m_title_air_print = new wxStaticText(m_panel_body, wxID_ANY, _L("Air Printing Detection"), wxDefaultPosition, wxDefaultSize, 0);
    m_title_air_print->SetFont(::Label::Head_13);
    m_title_air_print->SetForegroundColour(AMS_SETTING_GREY800);
    m_title_air_print->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_air_print->Add(m_title_air_print, 1, wxEXPAND, 0);

    wxBoxSizer* m_sizer_air_print_tip = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_air_print_tip->Add(0, 0, 0, wxLEFT, 10);

    // tip line
    auto m_sizer_air_print_inline = new wxBoxSizer(wxVERTICAL);

    m_tip_air_print_line = new Label(m_panel_body,
        _L("Detects clogging and filament grinding, halting printing immediately to conserve time and filament.")
    );
    m_tip_air_print_line->SetFont(::Label::Body_13);
    m_tip_air_print_line->SetForegroundColour(AMS_SETTING_GREY700);
    m_tip_air_print_line->SetSize(wxSize(AMS_SETTING_BODY_WIDTH, -1));
    m_tip_air_print_line->Wrap(AMS_SETTING_BODY_WIDTH);
    m_sizer_air_print_inline->Add(m_tip_air_print_line, 0, wxEXPAND, 0);
    m_sizer_air_print_tip->Add(m_sizer_air_print_inline, 1, wxALIGN_CENTER, 0);

    m_checkbox_air_print->Hide();
    m_title_air_print->Hide();
    m_tip_air_print_line->Hide();


    // panel img
    wxPanel* m_panel_img = new wxPanel(m_panel_body, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_panel_img->SetBackgroundColour(AMS_SETTING_GREY200);
    wxBoxSizer *m_sizer_img = new wxBoxSizer(wxVERTICAL);
    m_am_img = new wxStaticBitmap(m_panel_img, wxID_ANY, create_scaled_bitmap("ams_icon", nullptr, 126), wxDefaultPosition, wxDefaultSize);
    m_sizer_img->Add(m_am_img, 0, wxALIGN_CENTER | wxTOP, 26);
    m_sizer_img->Add(0, 0, 0, wxTOP, 18);
    m_panel_img->SetSizer(m_sizer_img);
    m_panel_img->Layout();
    m_sizer_img->Fit(m_panel_img);

    m_sizer_remain_block = new wxBoxSizer(wxVERTICAL);
    m_sizer_remain_block->Add(m_sizer_remain, 0, wxEXPAND | wxTOP, FromDIP(8));
    m_sizer_remain_block->Add(0, 0, 0, wxTOP, 8);
    m_sizer_remain_block->Add(m_sizer_remain_tip, 0, wxLEFT, 18);
    m_sizer_remain_block->Add(0, 0, 0, wxTOP, 15);

    m_sizerl_body->AddSpacer(FromDIP(12));
    m_sizerl_body->Add(m_ams_type, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));
    //m_sizerl_body->Add(m_ams_arrange_order, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));    
    m_sizerl_body->Add(m_panel_Insert_material, 0, wxEXPAND | wxTOP, FromDIP(12));
    m_sizerl_body->Add(m_sizer_starting, 0, wxEXPAND | wxTOP, FromDIP(12));
    m_sizerl_body->Add(m_sizer_starting_tip, 0, wxEXPAND | wxTOP, FromDIP(12));
    m_sizerl_body->Add(m_sizer_remain_block, 0, wxEXPAND | wxTOP, FromDIP(12));
    m_sizerl_body->Add(m_sizer_switch_filament, 0, wxEXPAND | wxTOP, FromDIP(12));
    m_sizerl_body->Add(m_sizer_switch_filament_tip, 0, wxEXPAND | wxTOP, FromDIP(12));
    m_sizerl_body->Add(m_sizer_air_print, 0, wxEXPAND | wxTOP, FromDIP(12));
    m_sizerl_body->Add(m_sizer_air_print_tip, 0, wxEXPAND | wxTOP, FromDIP(12));
    m_sizerl_body->Add(m_panel_img, 1, wxEXPAND | wxALL, FromDIP(5));

    m_panel_body->SetSizer(m_sizerl_body);
    m_panel_body->Layout();
    m_sizerl_body->Fit(m_panel_body);
    m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(10));
    m_sizer_main->Add(m_static_ams_settings, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24));
    m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(10));
    m_sizer_main->Add(m_panel_body, 1, wxBottom | wxLEFT | wxRIGHT | wxEXPAND, FromDIP(24));

    this->SetSizer(m_sizer_main);
    this->Layout();
    m_sizer_main->Fit(this);

    this->Centre(wxBOTH);
    wxGetApp().UpdateDlgDarkUI(this);
}

void AMSSetting::UpdateByObj(MachineObject* obj)
{
    this->m_obj = obj;
    if (!obj) {
        this->Show(false);
        return;
    }

    update_ams_img(obj);

    m_ams_type->Update(obj);
    //m_ams_arrange_order->Update(obj);
    update_insert_material_read_mode(obj);
    m_sizer_remain_block->Show(obj->is_support_update_remain);
    update_starting_read_mode(obj->GetFilaSystem()->IsDetectOnPowerupEnabled());
    update_remain_mode(obj->GetFilaSystem()->IsDetectRemainEnabled());
    update_switch_filament(obj->GetFilaSystem()->IsAutoRefillEnabled());
    update_air_printing_detection(obj);

    update_firmware_switching_status();// on fila_firmware_switch
}

void AMSSetting::update_firmware_switching_status()
{
    if (!m_obj) {
        return;
    }

    auto fila_firmware_switch = m_obj->GetFilaSystem()->GetAmsFirmwareSwitch().lock();
    if (fila_firmware_switch->GetSuppotedFirmwares().empty()) {
        return;
    }

    if (m_switching == fila_firmware_switch->IsSwitching()) {
        return;
    }
    m_switching = fila_firmware_switch->IsSwitching();

    // BFS: Update all children
    auto children = GetChildren();
    while (!children.IsEmpty()) {
        auto win = children.front();
        children.pop_front();

        // do something with win
        if (win == m_static_ams_settings || win == m_ams_type) {
            continue;
        }

        if (dynamic_cast<wxStaticText*>(win) != nullptr ||
            dynamic_cast<CheckBox*>(win) != nullptr) {
            win->Enable(!m_switching);
        }

        for (auto child : win->GetChildren()) {
            children.push_back(child);
        }
    }
}

void AMSSetting::update_insert_material_read_mode(MachineObject* obj)
{
    if (obj) {
        auto setting = obj->GetFilaSystem()->GetAmsSystemSetting().IsDetectOnInsertEnabled();
        if (!setting.has_value()) {
            m_panel_Insert_material->Show(false);
            return;
        }

        // special case for A series
        if (auto ptr = obj->GetFilaSystem()->GetAmsFirmwareSwitch().lock(); ptr->SupportSwitchFirmware()) {
            if (ptr->GetCurrentFirmwareIdxSel() == DevAmsSystemFirmwareSwitch::IDX_LITE) {
                m_panel_Insert_material->Show(false);
                return;
            }
        } else if (DevPrinterConfigUtil::get_printer_use_ams_type(obj->printer_type) == "f1") {
            m_panel_Insert_material->Show(false);
            return;
        }

        std::string extra_ams_str = (boost::format("ams_f1/%1%") % 0).str();
        auto extra_ams_it = obj->module_vers.find(extra_ams_str);
        if (extra_ams_it != obj->module_vers.end()) {
            update_insert_material_read_mode(setting.value(), extra_ams_it->second.sw_ver);
        } else {
            update_insert_material_read_mode(setting.value(), "");
        }
    }
}

void AMSSetting::update_insert_material_read_mode(bool selected, std::string version)
{
    if (!version.empty() && version >= AMS_F1_SUPPORT_INSERTION_UPDATE_DEFAULT) {
        m_checkbox_Insert_material_auto_read->SetValue(true);
        m_checkbox_Insert_material_auto_read->Hide();
        m_title_Insert_material_auto_read->Hide();
        m_tip_Insert_material_line1->Hide();
        m_tip_Insert_material_line2->Hide();
        m_tip_Insert_material_line3->Hide();
        m_panel_Insert_material->Hide();
    }
    else {
        m_panel_Insert_material->Show();
        m_checkbox_Insert_material_auto_read->SetValue(selected);
        m_checkbox_Insert_material_auto_read->Show();
        m_title_Insert_material_auto_read->Show();
        if (selected) {
            m_tip_Insert_material_line1->Show();
            m_tip_Insert_material_line2->Show();
            m_tip_Insert_material_line3->Hide();
        }
        else {
            m_tip_Insert_material_line1->Hide();
            m_tip_Insert_material_line2->Hide();
            m_tip_Insert_material_line3->Show();
        }
    }
    m_panel_Insert_material->Layout();
    m_sizer_Insert_material_tip_inline->Layout();
    Layout();
    Fit();
}

void AMSSetting::update_ams_img(MachineObject* obj_)
{
    if (!obj_) {
        return;
    }

    std::string ams_icon_str = DevPrinterConfigUtil::get_printer_ams_img(obj_->printer_type);
    if (auto ams_switch = obj_->GetFilaSystem()->GetAmsFirmwareSwitch().lock();
        ams_switch->GetCurrentFirmwareIdxSel() == 1) {
        ams_icon_str = "ams_icon";// A series support AMS
    }

    // transfer to dark mode icon
    if (wxGetApp().dark_mode()&& ams_icon_str=="extra_icon") {
        ams_icon_str += "_dark";
    }

    if (ams_icon_str != m_ams_img_name) {
        m_am_img->SetBitmap(create_scaled_bitmap(ams_icon_str, nullptr, 126));
        m_am_img->Refresh();
    }
}

void AMSSetting::update_starting_read_mode(bool selected)
{
    m_checkbox_starting_auto_read->SetValue(selected);
    if (selected) { // selected
        m_tip_starting_line1->Show();
        m_tip_starting_line2->Hide();
    } else { // unselected
        m_tip_starting_line1->Hide();
        m_tip_starting_line2->Show();
    }
    m_sizer_starting_tip_inline->Layout();
    Layout();
    Fit();
}

void AMSSetting::update_remain_mode(bool selected)
{
    if (m_obj->is_support_update_remain) {
        m_checkbox_remain->Show();
        m_title_remain->Show();
        m_tip_remain_line1->Show();
        Layout();
    }
    else {
        m_checkbox_remain->Hide();
        m_title_remain->Hide();
        m_tip_remain_line1->Hide();
        Layout();
    }
    m_checkbox_remain->SetValue(selected);
}

void AMSSetting::update_switch_filament(bool selected)
{
    if (m_obj->is_support_filament_backup) {
        m_checkbox_switch_filament->Show();
        m_title_switch_filament->Show();
        m_tip_switch_filament_line1->Show();
        Layout();
    } else {
        m_checkbox_switch_filament->Hide();
        m_title_switch_filament->Hide();
        m_tip_switch_filament_line1->Hide();
        Layout();
    }
    m_checkbox_switch_filament->SetValue(selected);
}


void AMSSetting::update_air_printing_detection(MachineObject* obj)
{
    if(!obj) {
        return;
    }

    if (obj->is_support_air_print_detection) {
        m_checkbox_air_print->Show();
        m_title_air_print->Show();
        m_tip_air_print_line->Show();
    } else {
        m_checkbox_air_print->Hide();
        m_title_air_print->Hide();
        m_tip_air_print_line->Hide();
    }
    Layout();
    m_checkbox_air_print->SetValue(obj->ams_air_print_status);
}

void AMSSetting::on_insert_material_read(wxCommandEvent &event)
{
    // send command
    if (m_checkbox_Insert_material_auto_read->GetValue()) {
        // checked
        m_tip_Insert_material_line1->Show();
        m_tip_Insert_material_line2->Show();
        m_tip_Insert_material_line3->Hide();
    } else {
        // unchecked
        m_tip_Insert_material_line1->Hide();
        m_tip_Insert_material_line2->Hide();
        m_tip_Insert_material_line3->Show();
    }
    m_checkbox_Insert_material_auto_read->SetValue(event.GetInt());

    bool start_read_opt = m_checkbox_starting_auto_read->GetValue();
    bool tray_read_opt = m_checkbox_Insert_material_auto_read->GetValue();
    bool remain_opt = m_checkbox_remain->GetValue();

    m_obj->command_ams_user_settings(start_read_opt, tray_read_opt, remain_opt);

    m_sizer_Insert_material_tip_inline->Layout();
    Layout();
    Fit();

    event.Skip();
}

void AMSSetting::on_starting_read(wxCommandEvent &event)
{
    if (m_checkbox_starting_auto_read->GetValue()) {
        // checked
        m_tip_starting_line1->Show();
        m_tip_starting_line2->Hide();
    } else {
        // unchecked
        m_tip_starting_line1->Hide();
        m_tip_starting_line2->Show();
    }
    m_checkbox_starting_auto_read->SetValue(event.GetInt());

    bool start_read_opt = m_checkbox_starting_auto_read->GetValue();
    bool tray_read_opt  = m_checkbox_Insert_material_auto_read->GetValue();
    bool remain_opt = m_checkbox_remain->GetValue();

    m_obj->command_ams_user_settings(start_read_opt, tray_read_opt, remain_opt);

    m_sizer_starting_tip_inline->Layout();
    Layout();
    Fit();

    event.Skip();
}

void AMSSetting::on_remain(wxCommandEvent& event)
{
    bool start_read_opt = m_checkbox_starting_auto_read->GetValue();
    bool tray_read_opt = m_checkbox_Insert_material_auto_read->GetValue();
    bool remain_opt = m_checkbox_remain->GetValue();
    m_obj->command_ams_user_settings(start_read_opt, tray_read_opt, remain_opt);
    event.Skip();
}

void AMSSetting::on_switch_filament(wxCommandEvent& event)
{
    bool switch_filament = m_checkbox_switch_filament->GetValue();
    m_obj->command_ams_switch_filament(switch_filament);
    event.Skip();
}

void AMSSetting::on_air_print_detect(wxCommandEvent& event)
{
    bool air_print_detect = m_checkbox_air_print->GetValue();
    m_obj->command_ams_air_print_detect(air_print_detect);
    event.Skip();
}

void AMSSetting::on_dpi_changed(const wxRect &suggested_rect)
{
    if (!m_ams_img_name.empty()) {
        m_am_img->SetBitmap(create_scaled_bitmap(m_ams_img_name, nullptr, 126));
        m_am_img->Refresh();
    }
}

AMSSettingTypePanel::AMSSettingTypePanel(wxWindow* parent, AMSSetting* setting_dlg)
    : wxPanel(parent), m_setting_dlg(setting_dlg)
{
    CreateGui();
}

AMSSettingTypePanel::~AMSSettingTypePanel()
{
    if (m_switching_icon->IsPlaying()) {
        m_switching_icon->Stop();
    }
}

void AMSSettingTypePanel::CreateGui()
{
    wxBoxSizer* h_sizer = new wxBoxSizer(wxHORIZONTAL);

    Label* title = new Label(this, ::Label::Head_13, _L("AMS Type"));
    title->SetBackgroundColour(*wxWHITE);

    m_type_combobox = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(240, -1), 0, nullptr, wxCB_READONLY);
    m_type_combobox->SetMinSize(wxSize(240, -1));
    m_type_combobox->Bind(wxEVT_COMBOBOX, &AMSSettingTypePanel::OnAmsTypeChanged, this);

    m_switching_tips = new Label(this, ::Label::Body_14);
    m_switching_tips->SetBackgroundColour(*wxWHITE);
    m_switching_tips->Show(false);

    std::vector<std::string> list{ "ams_rfid_1", "ams_rfid_2", "ams_rfid_3", "ams_rfid_4" };
    m_switching_icon = new AnimaIcon(this, wxID_ANY, list, "refresh_printer", 100);
    m_switching_icon->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));

    h_sizer->Add(title, 0);
    h_sizer->AddStretchSpacer();
    h_sizer->Add(m_type_combobox, 0, wxEXPAND);
    h_sizer->Add(m_switching_icon, 0, wxALIGN_CENTER);
    h_sizer->Add(m_switching_tips, 0, wxEXPAND | wxLEFT | wxALIGN_CENTER, FromDIP(8));
    SetSizer(h_sizer);
    Layout();
    Fit();
}

void AMSSettingTypePanel::Update(const MachineObject* obj)
{
    if (!obj) {
        Show(false);
        return;
    }

    m_ams_firmware_switch = obj->GetFilaSystem()->GetAmsFirmwareSwitch();
    auto ptr = m_ams_firmware_switch.lock();
    if (!ptr) {
        Show(false);
        return;
    }

    if (!ptr->SupportSwitchFirmware()) {
        Show(false);
        return;
    }

    if (ptr->IsSwitching())  {
        int display_percent = obj->get_upgrade_percent();
        if (display_percent == 100 || display_percent == 0) {
            display_percent = 1;// special case, sometimes it's switching but percent is 0 or 100
        }
        const auto& tips = _L("Switching") + " " + wxString::Format("%d%%", display_percent);
        m_switching_tips->SetLabel(tips);
        m_switching_icon->Play();
        m_switching_tips->Show(true);
        m_switching_icon->Show(true);
        m_type_combobox->Show(false);
    } else {
        int current_idx = ptr->GetCurrentFirmwareIdxSel();
        auto ams_firmwares = ptr->GetSuppotedFirmwares();
        if (m_ams_firmwares != ams_firmwares || m_ams_firmware_current_idx != current_idx) {
            m_ams_firmware_current_idx = current_idx;
            m_ams_firmwares = ams_firmwares;

            m_type_combobox->Clear();
            for (auto ams_firmware : m_ams_firmwares) {
                if (m_ams_firmware_current_idx == ams_firmware.first) {
                    m_type_combobox->Append(_L(ams_firmware.second.m_name));
                } else {
                    m_type_combobox->Append(_L(ams_firmware.second.m_name));
                }
            }
            m_type_combobox->SetSelection(m_ams_firmware_current_idx);
        }

        if(m_switching_icon->IsPlaying()) {
            m_switching_icon->Stop();
        }

        m_switching_tips->Show(false);
        m_switching_icon->Show(false);
        m_type_combobox->Show(true);
    }

    Show(true);
    Layout();
}

void AMSSettingTypePanel::OnAmsTypeChanged(wxCommandEvent& event)
{
    auto part = m_ams_firmware_switch.lock();
    if (!part) {
        event.Skip();
        return;
    }

    int new_selection_idx = m_type_combobox->GetSelection();
    if (new_selection_idx == part->GetCurrentFirmwareIdxSel()) {
        event.Skip();
        return;
    }
   
    auto obj_ = part->GetFilaSystem()->GetOwner();
    if (obj_) {
        if (obj_->is_in_printing() || obj_->is_in_upgrading())  {
            MessageDialog dlg(this, _L("The printer is busy and cannot switch AMS type."), SLIC3R_APP_NAME + _L("Info"), wxOK | wxICON_INFORMATION);
            dlg.ShowModal();
            m_type_combobox->SetSelection(part->GetCurrentFirmwareIdxSel());
            return;
        }

        auto ext = obj_->GetExtderSystem()->GetCurrentExtder();
        if (ext && ext->HasFilamentInExt()) {
            MessageDialog dlg(this, _L("Please unload all filament before switching."), SLIC3R_APP_NAME + _L("Info"), wxOK | wxICON_INFORMATION);
            dlg.SetButtonLabel(wxID_OK, _L("Confirm"));
            dlg.ShowModal();
            m_type_combobox->SetSelection(part->GetCurrentFirmwareIdxSel());
            if (m_setting_dlg) {
                m_setting_dlg->EndModal(wxID_OK);
            }
            
            return;
        }

        MessageDialog dlg(this, _L("AMS type switching needs firmware update, taking about 30s. Switch now ?"), SLIC3R_APP_NAME + _L("Info"), wxOK | wxCANCEL | wxICON_INFORMATION);
        dlg.SetButtonLabel(wxID_OK, _L("Confirm"));
        int rtn = dlg.ShowModal();
        if (rtn != wxID_OK) {
            m_type_combobox->SetSelection(part->GetCurrentFirmwareIdxSel());
            return;
        }

        part->CrtlSwitchFirmware(new_selection_idx);
    }

    event.Skip();
}

#if 0 /*used option*/
AMSSettingArrangeAMSOrder::AMSSettingArrangeAMSOrder(wxWindow* parent)
    : wxPanel(parent)
{
    CreateGui();
}

void AMSSettingArrangeAMSOrder::CreateGui()
{
    wxBoxSizer* h_sizer = new wxBoxSizer(wxHORIZONTAL);
    Label* title = new Label(this, ::Label::Head_13, _L("Arrange AMS Order"));
    title->SetBackgroundColour(*wxWHITE);

    m_btn_rearrange = new ScalableButton(this, wxID_ANY, "dev_ams_rearrange");
    m_btn_rearrange->SetBackgroundColour(*wxWHITE);
    m_btn_rearrange->SetMinSize(wxSize(FromDIP(13), FromDIP(13)));
    m_btn_rearrange->Bind(wxEVT_BUTTON, &AMSSettingArrangeAMSOrder::OnBtnRearrangeClicked, this);
    h_sizer->Add(title, 0);
    h_sizer->AddStretchSpacer();
    h_sizer->Add(m_btn_rearrange, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    SetSizer(h_sizer);
    Layout();
    Fit();
}

void AMSSettingArrangeAMSOrder::Update(const MachineObject* obj)
{
    if (obj) {
        m_ams_firmware_switch = obj->GetFilaSystem()->GetAmsFirmwareSwitch();
        if (auto ptr = m_ams_firmware_switch.lock(); ptr->SupportSwitchFirmware()) {
            Show(true);
            return;
        }
    }

    Show(false);
}

void AMSSettingArrangeAMSOrder::OnBtnRearrangeClicked(wxCommandEvent& event)
{
    auto part = m_ams_firmware_switch.lock();
    if (part)  {
        MessageDialog dlg(this, _L("AMS ID will be reset. If you want a specific ID sequence, "
                                   "disconnect all AMS before resetting and connect them "
                                   "in the desired order after resetting."),
                                   SLIC3R_APP_NAME + _L("Info"), wxOK | wxCANCEL | wxICON_INFORMATION);
        int rtn = dlg.ShowModal();
        if (rtn == wxID_OK) {
            part->GetFilaSystem()->CtrlAmsReset();
        }
    }

    event.Skip();
}
#endif

}} // namespace Slic3r::GUI