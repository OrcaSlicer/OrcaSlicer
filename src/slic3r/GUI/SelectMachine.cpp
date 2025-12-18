#include "SelectMachine.hpp"
#include "I18N.hpp"

#include "libslic3r/Utils.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r/Color.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Preview.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/Label.hpp"
#include "ConnectPrinter.hpp"
#include "Jobs/BoostThreadWorker.hpp"
#include "Jobs/PlaterWorker.hpp"

#include "DeviceCore/DevConfig.h"
#include "DeviceCore/DevNozzleSystem.h"
#include "DeviceCore/DevExtensionTool.h"
#include "DeviceCore/DevExtruderSystem.h"
#include "DeviceCore/DevFilaBlackList.h"
#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevMapping.h"
#include "DeviceCore/DevStorage.h"

#include <wx/progdlg.h>
#include <wx/clipbrd.h>
#include <wx/dcgraph.h>
#include <wx/mstream.h>
#include <miniz.h>
#include <algorithm>
#include "Plater.hpp"
#include "Notebook.hpp"
#include "BitmapCache.hpp"
#include "BindDialog.hpp"

namespace Slic3r { namespace GUI {

wxDEFINE_EVENT(EVT_SWITCH_PRINT_OPTION, wxCommandEvent);
wxDEFINE_EVENT(EVT_UPDATE_USER_MACHINE_LIST, wxCommandEvent);
wxDEFINE_EVENT(EVT_PRINT_JOB_CANCEL, wxCommandEvent);
wxDEFINE_EVENT(EVT_CLEAR_IPADDRESS, wxCommandEvent);

#define INITIAL_NUMBER_OF_MACHINES 0

#define MACHINE_LIST_REFRESH_INTERVAL 2000

#define WRAP_GAP FromDIP(2)

static wxString task_canceled_text = _L("Task canceled");

std::string get_nozzle_volume_type_cloud_string(NozzleVolumeType nozzle_volume_type)
{
    if (nozzle_volume_type == NozzleVolumeType::nvtStandard) {
        return "standard_flow";
    }
    else if (nozzle_volume_type == NozzleVolumeType::nvtHighFlow) {
        return "high_flow";
    }
    else {
        assert(false);
        return "";
    }
}

std::vector<wxString> SelectMachineDialog::MACHINE_BED_TYPE_STRING;
std::vector<string> SelectMachineDialog::MachineBedTypeString;
void                SelectMachineDialog::init_machine_bed_types()
{
    if (MACHINE_BED_TYPE_STRING.size() == 0) {
        MACHINE_BED_TYPE_STRING = {//_L("Auto"),
                                   _L("Bambu Cool Plate") + " / " + _L("PLA Plate"),
                                   _L("Bambu Engineering Plate"),
                                   _L("Bambu Smooth PEI Plate") + "/" + _L("High temperature Plate"),
                                   _L("Bambu Textured PEI Plate"),
                                   _L("Bambu Cool Plate SuperTack")
        };
        MachineBedTypeString    = {//"auto",
                                   "pc", "pe", "pei", "pte", "suprtack"};
    }
}
SelectMachineDialog::SelectMachineDialog(Plater *plater)
    : DPIDialog(static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Send print job"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_plater(plater), m_export_3mf_cancel(false)
    , m_mapping_popup(AmsMapingPopup(this))
    , m_mapping_tip_popup(AmsMapingTipPopup(this))
    , m_mapping_tutorial_popup(AmsTutorialPopup(this))
{
    m_mapping_popup.EnableExtMappingFilaTypeCheck(false);

    init_machine_bed_types();
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__


    if (wxGetApp().app_config->get("language") == "zh_CN") {
        ops_auto.push_back(POItem{"auto", _L("Auto")});
        ops_auto.push_back(POItem{"on", _L("On")});
        ops_auto.push_back(POItem{"off", _L("Off")});

        ops_no_auto.push_back(POItem{"on", _L("On")});
        ops_no_auto.push_back(POItem{"off", _L("Off")});
    } else {
        ops_auto.push_back(POItem{"auto", "Auto"});
        ops_auto.push_back(POItem{"on", "On"});
        ops_auto.push_back(POItem{"off", "Off"});

        ops_no_auto.push_back(POItem{"on", "On"});
        ops_no_auto.push_back(POItem{"off", "Off"});
    }

    // bind
    Bind(wxEVT_CLOSE_WINDOW, &SelectMachineDialog::on_cancel, this);

    for (int i = 0; i < BED_TYPE_COUNT; i++) { m_bedtype_list.push_back(MACHINE_BED_TYPE_STRING[i]); }

    // font
    SetFont(wxGetApp().normal_font());

    Freeze();
    SetBackgroundColour(m_colour_def_color);

    wxBoxSizer* m_scroll_sizer = new wxBoxSizer(wxVERTICAL);
    m_scroll_area              = new wxScrolledWindow(this);
    m_scroll_area->SetScrollRate(0, 20);
    m_scroll_area->SetBackgroundColour(m_colour_def_color);
    m_scroll_area->SetMinSize(wxSize(FromDIP(700), FromDIP(600)));
    m_scroll_area->SetMaxSize(wxSize(FromDIP(700), FromDIP(600)));

    m_line_top = new wxPanel(m_scroll_area, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));

    /*mode switch*/
     /*auto m_sizer_mode_switch = new wxBoxSizer(wxHORIZONTAL);
     m_mode_print = new SendModeSwitchButton(scroll_area, _L("Print"), true);
     m_mode_send = new SendModeSwitchButton(scroll_area,_L("Save to printer"), false);
     m_sizer_mode_switch->Add(m_mode_print, 0, wxALIGN_CENTER, 0);
     m_sizer_mode_switch->Add(0, 0, 0, wxLEFT, FromDIP(8));
     m_sizer_mode_switch->Add(m_mode_send,0, wxALIGN_CENTER, 0);

    m_mode_print->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        m_mode_print->setSelected(true);
        m_mode_send->setSelected(false);
        e.Skip();
    });

    m_mode_send->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        m_mode_print->setSelected(false);
        m_mode_send->setSelected(true);
        e.Skip();
    });*/

    m_basic_panel = new wxPanel(m_scroll_area, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_basic_panel->SetBackgroundColour(*wxWHITE);
    m_basicl_sizer = new wxBoxSizer(wxHORIZONTAL);

    /*basic info*/
    /*thumbnail*/
    auto m_sizer_thumbnail_area = new wxBoxSizer(wxHORIZONTAL);

    auto m_panel_image = new wxPanel(m_basic_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_panel_image->SetBackgroundColour(m_colour_def_color);
    m_sizer_thumbnail = new wxBoxSizer(wxHORIZONTAL);
    m_thumbnailPanel = new ThumbnailPanel(m_panel_image);
    m_thumbnailPanel->SetSize(wxSize(FromDIP(198), FromDIP(198)));
    m_thumbnailPanel->SetMinSize(wxSize(FromDIP(198), FromDIP(198)));
    m_thumbnailPanel->SetMaxSize(wxSize(FromDIP(198), FromDIP(198)));
    m_thumbnailPanel->SetBackgroundColour(*wxWHITE);
    m_sizer_thumbnail->Add(m_thumbnailPanel, 0, wxALIGN_CENTER, 0);
    m_panel_image->SetSizer(m_sizer_thumbnail);
    m_panel_image->Layout();

    m_sizer_thumbnail_area->Add(m_panel_image, 0, wxALIGN_CENTER, 0);
    m_sizer_thumbnail_area->Layout();

    /*basic info right*/
    auto  sizer_basic_right_info = new wxBoxSizer(wxVERTICAL);

    /*rename*/
    auto sizer_rename = new wxBoxSizer(wxHORIZONTAL);

    m_rename_switch_panel = new wxSimplebook(m_basic_panel);
    m_rename_switch_panel->SetBackgroundColour(*wxWHITE);
    m_rename_switch_panel->SetSize(wxSize(FromDIP(360), FromDIP(25)));
    m_rename_switch_panel->SetMinSize(wxSize(FromDIP(360), FromDIP(25)));
    m_rename_switch_panel->SetMaxSize(wxSize(FromDIP(360), FromDIP(25)));

    m_rename_normal_panel = new wxPanel(m_rename_switch_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_rename_normal_panel->SetBackgroundColour(*wxWHITE);
    rename_sizer_v = new wxBoxSizer(wxVERTICAL);
    rename_sizer_h = new wxBoxSizer(wxHORIZONTAL);

    m_rename_text = new wxStaticText(m_rename_normal_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_rename_text->SetFont(::Label::Head_14);
    m_rename_text->SetBackgroundColour(*wxWHITE);
    m_rename_text->SetMaxSize(wxSize(FromDIP(340), -1));
    rename_editable       = new ScalableBitmap(m_scroll_area, "rename_edit", FromDIP(13)); // ORCA Match edit icon and its size
    rename_editable_light = new ScalableBitmap(m_scroll_area, "rename_edit", FromDIP(13)); // ORCA Match edit icon and its size
    m_rename_button = new wxStaticBitmap(m_rename_normal_panel, wxID_ANY, rename_editable->bmp(), wxDefaultPosition, wxSize(FromDIP(20), FromDIP(20)), 0);
    m_rename_button->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_HAND); });
    m_rename_button->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_ARROW); });

    rename_sizer_h->Add(m_rename_text, 0, wxALIGN_CENTER, 0); // ORCA align text with icon
    rename_sizer_h->Add(m_rename_button, 0, wxALIGN_CENTER| wxLEFT, FromDIP(3)); // ORCA add gap between text and icon
    rename_sizer_v->Add(rename_sizer_h, 1, wxTOP, 0);

    m_rename_normal_panel->SetSizer(rename_sizer_v);
    m_rename_normal_panel->Layout();
    rename_sizer_v->Fit(m_rename_normal_panel);

    auto m_rename_edit_panel = new wxPanel(m_rename_switch_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_rename_edit_panel->SetBackgroundColour(*wxWHITE);
    auto rename_edit_sizer_v = new wxBoxSizer(wxVERTICAL);

    m_rename_input = new ::TextInput(m_rename_edit_panel, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_rename_input->GetTextCtrl()->SetFont(::Label::Body_13);
    m_rename_input->SetSize(wxSize(FromDIP(360), FromDIP(24)));
    m_rename_input->SetMinSize(wxSize(FromDIP(360), FromDIP(24)));
    m_rename_input->SetMaxSize(wxSize(FromDIP(360), FromDIP(24)));
    m_rename_input->Bind(wxEVT_TEXT_ENTER, [this](auto& e) {on_rename_enter();});
    m_rename_input->Bind(wxEVT_KILL_FOCUS, [this](auto& e) {
        if (!m_rename_input->HasFocus() && !m_rename_text->HasFocus())
            on_rename_enter();
        else
            e.Skip(); });
    rename_edit_sizer_v->Add(m_rename_input, 1, wxALIGN_CENTER, 0);


    m_rename_edit_panel->SetSizer(rename_edit_sizer_v);
    m_rename_edit_panel->Layout();
    rename_edit_sizer_v->Fit(m_rename_edit_panel);

    m_rename_button->Bind(wxEVT_LEFT_DOWN, &SelectMachineDialog::on_rename_click, this);
    m_rename_switch_panel->AddPage(m_rename_normal_panel, wxEmptyString, true);
    m_rename_switch_panel->AddPage(m_rename_edit_panel, wxEmptyString, false);

    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE) {
            if (m_rename_switch_panel->GetSelection() == 0) {
                e.Skip();
            }
            else {
                m_rename_switch_panel->SetSelection(0);
                m_rename_text->SetLabel(m_current_project_name);
                m_rename_normal_panel->Layout();
            }
        }
        else {
            e.Skip();
        }
    });


    /*weight & time*/
    wxBoxSizer *m_sizer_basic_weight_time = new wxBoxSizer(wxHORIZONTAL);

    print_time   = new ScalableBitmap(m_scroll_area, "print-time", 18);
    timeimg = new wxStaticBitmap(m_basic_panel, wxID_ANY, print_time->bmp(), wxDefaultPosition, wxSize(FromDIP(18), FromDIP(18)), 0);
    m_stext_time = new Label(m_basic_panel, wxEmptyString);
    m_stext_time->SetFont(Label::Body_13);

    print_weight   = new ScalableBitmap(m_scroll_area, "print-weight", 18);
    weightimg = new wxStaticBitmap(m_basic_panel, wxID_ANY, print_weight->bmp(), wxDefaultPosition, wxSize(FromDIP(18), FromDIP(18)), 0);
    m_stext_weight = new Label(m_basic_panel, wxEmptyString);
    m_stext_weight->SetFont(Label::Body_13);

    m_sizer_basic_weight_time->Add(timeimg, 0, wxALIGN_CENTER, 0);
    m_sizer_basic_weight_time->Add(m_stext_time, 0, wxALIGN_CENTER|wxLEFT, FromDIP(6));
    m_sizer_basic_weight_time->AddSpacer(FromDIP(30));
    m_sizer_basic_weight_time->Add(weightimg, 0, wxALIGN_CENTER, 0);
    m_sizer_basic_weight_time->Add(m_stext_weight, 0, wxALIGN_CENTER|wxLEFT, FromDIP(6));

    /*last & next page*/
    auto last_plate_sizer = new wxBoxSizer(wxVERTICAL);
    m_bitmap_last_plate = new wxStaticBitmap(m_basic_panel, wxID_ANY, create_scaled_bitmap("go_last_plate", m_scroll_area, 25), wxDefaultPosition, wxSize(FromDIP(25), FromDIP(25)), 0);
    m_bitmap_last_plate->Hide();
    last_plate_sizer->Add(m_bitmap_last_plate, 0, wxALIGN_CENTER, 0);

    auto next_plate_sizer = new wxBoxSizer(wxVERTICAL);
    m_bitmap_next_plate = new wxStaticBitmap(m_basic_panel, wxID_ANY, create_scaled_bitmap("go_next_plate", m_scroll_area, 25), wxDefaultPosition, wxSize(FromDIP(25), FromDIP(25)), 0);
    m_bitmap_next_plate->Hide();
    next_plate_sizer->Add(m_bitmap_next_plate, 0, wxALIGN_CENTER, 0);

    sizer_rename->Add(m_rename_switch_panel, 0,  wxALIGN_CENTER, 0);
    sizer_rename->Add(0, 0, 0, wxEXPAND, 0);
    sizer_rename->Add(m_bitmap_last_plate, 0,  wxALIGN_CENTER, 0);
    sizer_rename->Add(m_bitmap_next_plate, 0,  wxALIGN_CENTER, 0);

    m_printer_box = new PrinterInfoBox(m_basic_panel, this);


    m_text_printer_msg = new PrinterMsgPanel(m_basic_panel);
    m_text_printer_msg->SetMinSize(wxSize(FromDIP(420), -1));
    m_text_printer_msg->SetMaxSize(wxSize(FromDIP(420), -1));
    m_text_printer_msg->SetBackgroundColour(m_colour_def_color);
    m_text_printer_msg->SetFont(::Label::Body_13);
    m_text_printer_msg->Hide();

    m_text_printer_msg_tips = new Label(m_basic_panel);
    m_text_printer_msg_tips->SetMinSize(wxSize(FromDIP(420), FromDIP(24)));
    m_text_printer_msg_tips->SetMaxSize(wxSize(FromDIP(420), FromDIP(24)));
    m_text_printer_msg_tips->SetFont(::Label::Body_13);
    m_text_printer_msg_tips->SetForegroundColour(wxColour(0x6B, 0x6B, 0x6B));
    m_text_printer_msg_tips->Hide();
    m_text_printer_msg_tips->GetAlignment();

    sizer_basic_right_info->Add(sizer_rename, 0, wxTOP, 0);
    sizer_basic_right_info->Add(0, 0, 0, wxTOP, FromDIP(5));
    sizer_basic_right_info->Add(m_sizer_basic_weight_time, 0, wxTOP, 0);
    sizer_basic_right_info->Add(0, 0, 0, wxTOP, FromDIP(10));
    sizer_basic_right_info->Add(m_printer_box, 0, wxTOP, 0);
    sizer_basic_right_info->Add(0, 0, 0, wxTOP, FromDIP(4));
    sizer_basic_right_info->Add(m_text_printer_msg, 0, wxLEFT, 0);
    sizer_basic_right_info->AddSpacer(FromDIP(10));
    sizer_basic_right_info->Add(m_text_printer_msg_tips, 0, wxLEFT, 0);


    m_basicl_sizer->Add(m_sizer_thumbnail_area, 0, wxLEFT, 0);
    m_basicl_sizer->Add(0, 0, 0, wxLEFT, FromDIP(8));
    m_basicl_sizer->Add(sizer_basic_right_info, 0, wxLEFT, 0);

    m_basic_panel->SetSizer(m_basicl_sizer);
    m_basic_panel->Layout();


    /*filaments info*/
    wxBoxSizer* sizer_split_filament = new wxBoxSizer(wxHORIZONTAL);

    auto m_stext_filament_title = new Label(m_scroll_area, _L("Filament"));
    m_stext_filament_title->SetFont(::Label::Head_13);
    m_stext_filament_title->SetForegroundColour(0x909090);

    auto m_split_line_filament = new wxPanel(m_scroll_area, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_split_line_filament->SetBackgroundColour(0xeeeeee);
    m_split_line_filament->SetMinSize(wxSize(-1, 1));
    m_split_line_filament->SetMaxSize(wxSize(-1, 1));

    m_sizer_autorefill = new wxBoxSizer(wxHORIZONTAL);
    m_ams_backup_tip = new Label(m_scroll_area, _L("Auto Refill"));
    m_ams_backup_tip->SetFont(::Label::Head_13);
    m_ams_backup_tip->SetForegroundColour(wxColour("#009688"));
    m_ams_backup_tip->SetBackgroundColour(*wxWHITE);
    img_ams_backup = new wxStaticBitmap(m_scroll_area, wxID_ANY, create_scaled_bitmap("automatic_material_renewal", this, 16), wxDefaultPosition, wxSize(FromDIP(16), FromDIP(16)), 0);
    img_ams_backup->SetBackgroundColour(*wxWHITE);

    m_sizer_autorefill->Add(0, 0, 1, wxEXPAND, 0);
    m_sizer_autorefill->Add(img_ams_backup, 0, wxALL, FromDIP(3));
    m_sizer_autorefill->Add(m_ams_backup_tip, 0, wxTOP, FromDIP(5));

    m_ams_backup_tip->Hide();
    img_ams_backup->Hide();

    m_ams_backup_tip->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_HAND); });
    img_ams_backup->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_HAND); });

    m_ams_backup_tip->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_ARROW); });
    img_ams_backup->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_ARROW); });

    m_ams_backup_tip->Bind(wxEVT_LEFT_DOWN, [this](auto& e) { if (!m_is_in_sending_mode) { popup_filament_backup(); on_rename_enter(); }  });
    img_ams_backup->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {if (!m_is_in_sending_mode) popup_filament_backup(); on_rename_enter(); });

    sizer_split_filament->Add(0, 0, 0, wxEXPAND, 0);
    sizer_split_filament->Add(m_stext_filament_title, 0, wxALIGN_CENTER, 0);
    sizer_split_filament->Add(m_split_line_filament, 1, wxALIGN_CENTER_VERTICAL, 0);
    sizer_split_filament->Add(m_sizer_autorefill, 0, wxALIGN_CENTER, 0);
    /*filament area*/
    /*1 extruder*/
    m_filament_panel = new StaticBox(m_scroll_area);
    m_filament_panel->SetBackgroundColour(wxColour("#F8F8F8"));
    m_filament_panel->SetBorderWidth(0);
    m_filament_panel->SetMinSize(wxSize(FromDIP(637), -1));
    m_filament_panel->SetMaxSize(wxSize(FromDIP(637), -1));
    m_filament_panel_sizer = new wxBoxSizer(wxVERTICAL);

    m_sizer_ams_mapping = new wxGridSizer(0, 10, FromDIP(7), FromDIP(7));
    m_filament_panel_sizer->Add(m_sizer_ams_mapping, 0, wxEXPAND|wxALL, FromDIP(10));
    m_filament_panel->SetSizer(m_filament_panel_sizer);
    m_filament_panel->Layout();
    m_filament_panel->Fit();

    /*left & right extruder*/
    m_sizer_filament_2extruder = new wxBoxSizer(wxHORIZONTAL);

    m_filament_left_panel = new StaticBox(m_scroll_area);
    m_filament_left_panel->SetBackgroundColour(wxColour("#F8F8F8"));
    m_filament_left_panel->SetBorderWidth(0);
    m_filament_left_panel->SetMinSize(wxSize(FromDIP(315), -1));
    m_filament_left_panel->SetMaxSize(wxSize(FromDIP(315), -1));

    m_filament_panel_left_sizer = new wxBoxSizer(wxVERTICAL);
    auto left_recommend_title_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto left_recommend_title1 = new Label(m_filament_left_panel, _L("Left Nozzle"));
    left_recommend_title1->SetFont(::Label::Head_13);
    left_recommend_title1->SetBackgroundColour(wxColour("#F8F8F8"));
    left_recommend_title_sizer->Add(left_recommend_title1, 0, wxALIGN_CENTER, 0);

    m_sizer_ams_mapping_left = new wxGridSizer(0, 5, FromDIP(7), FromDIP(7));
    m_filament_panel_left_sizer->Add(left_recommend_title_sizer, 0, wxLEFT|wxRIGHT|wxTOP, FromDIP(10));
    m_filament_panel_left_sizer->Add(m_sizer_ams_mapping_left, 0, wxEXPAND|wxALL, FromDIP(10));
    m_filament_left_panel->SetSizer(m_filament_panel_left_sizer);
    m_filament_left_panel->Layout();

    m_filament_right_panel = new StaticBox(m_scroll_area);
    m_filament_right_panel->SetBorderWidth(0);
    m_filament_right_panel->SetBackgroundColour(wxColour("#F8F8F8"));
    m_filament_right_panel->SetMinSize(wxSize(FromDIP(315), -1));
    m_filament_right_panel->SetMaxSize(wxSize(FromDIP(315), -1));

    m_filament_panel_right_sizer = new wxBoxSizer(wxVERTICAL);
    auto right_recommend_title_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto right_recommend_title1 = new Label(m_filament_right_panel, _L("Right Nozzle"));
    right_recommend_title1->SetFont(::Label::Head_13);
    right_recommend_title1->SetBackgroundColour(wxColour("#F8F8F8"));
    right_recommend_title_sizer->Add(right_recommend_title1, 0, wxALIGN_CENTER, 0);

    m_sizer_ams_mapping_right = new wxGridSizer(0, 5, FromDIP(7), FromDIP(7));
    m_filament_panel_right_sizer->Add(right_recommend_title_sizer, 0, wxLEFT|wxRIGHT|wxTOP, FromDIP(10));
    m_filament_panel_right_sizer->Add(m_sizer_ams_mapping_right, 0, wxEXPAND|wxALL, FromDIP(10));
    m_filament_right_panel->SetSizer(m_filament_panel_right_sizer);
    m_filament_right_panel->Layout();

    m_sizer_filament_2extruder->Add(m_filament_left_panel, 0, wxEXPAND, 0);
    m_sizer_filament_2extruder->Add( 0, 0, 1, wxEXPAND, 0);
    m_sizer_filament_2extruder->Add(m_filament_right_panel, 0, wxEXPAND, 0);
    m_sizer_filament_2extruder->Layout();

    m_filament_panel->Hide();

    m_statictext_ams_msg = new PrinterMsgPanel(m_scroll_area);
    m_statictext_ams_msg->SetMinSize(wxSize(FromDIP(655), -1));
    m_statictext_ams_msg->SetMaxSize(wxSize(FromDIP(655), -1));
    m_statictext_ams_msg->SetBackgroundColour(m_colour_def_color);
    m_statictext_ams_msg->SetFont(::Label::Body_13);
    m_statictext_ams_msg->Hide();

    /*ams mapping suggestions*/

    m_link_edit_nozzle = new Label(m_scroll_area, wxEmptyString);
    m_link_edit_nozzle->SetFont(::Label::Body_13);
    m_link_edit_nozzle->SetForegroundColour("#009688");
    m_link_edit_nozzle->SetBackgroundColour(*wxWHITE);
    m_link_edit_nozzle->Bind(wxEVT_ENTER_WINDOW, [this](auto &e) { SetCursor(wxCURSOR_HAND); });
    m_link_edit_nozzle->Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) { SetCursor(wxCURSOR_ARROW); });
    m_link_edit_nozzle->SetLabel(_L("Not satisfied with the grouping of filaments? Regroup and slice ->"));

    m_link_edit_nozzle->Bind(wxEVT_LEFT_DOWN, [this](auto &e) {

        if (this && this->m_is_in_sending_mode) {
            return;
        }

        EndModal(wxID_CLOSE);
        Plater *       plater = wxGetApp().plater();
        wxCommandEvent evt(EVT_OPEN_FILAMENT_MAP_SETTINGS_DIALOG);
        evt.SetEventObject(plater);
        evt.SetInt(1); // 1 means from gcode viewer
        wxPostEvent(plater, evt);
    });

    m_check_ext_change_assist = new CheckBox(m_scroll_area, wxID_ANY);
    m_check_ext_change_assist->SetValue(false);
    m_check_ext_change_assist->SetBackgroundColour(*wxWHITE);
    m_check_ext_change_assist->SetToolTip(_L("Manually change external spool during printing for multi-color printing"));
    m_check_ext_change_assist->Hide();
    m_label_ext_change_assist = new Label(m_scroll_area, _L("Multi-color with external"));
    m_label_ext_change_assist->Wrap(-1);
    m_label_ext_change_assist->Hide();
    m_label_ext_change_assist->SetMaxSize(wxSize(FromDIP(200), -1));
    m_label_ext_change_assist->SetFont(::Label::Body_13);
    m_label_ext_change_assist->SetBackgroundColour(*wxWHITE);
    m_label_ext_change_assist->SetToolTip(_L("Manually change external spool during printing for multi-color printing"));

    wxSizer* suggestion_sizer = new wxBoxSizer(wxHORIZONTAL);
    suggestion_sizer->Add(m_link_edit_nozzle, 0, wxLEFT, 0);
    suggestion_sizer->Add(0, 0, 1, wxEXPAND, 0);
    suggestion_sizer->Add(m_check_ext_change_assist, 0, wxRIGHT, FromDIP(10));
    suggestion_sizer->Add(m_label_ext_change_assist, 0, wxRIGHT, 0);

    m_mapping_sugs_sizer = new wxBoxSizer(wxHORIZONTAL);
    //auto m_img_mapping_sugs = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("warning", this, 16), wxDefaultPosition, wxSize(FromDIP(16), FromDIP(16)));
    auto m_txt_mapping_sugs = new Label(m_scroll_area, wxEmptyString);
    m_txt_mapping_sugs->SetFont(::Label::Body_13);
    m_txt_mapping_sugs->SetForegroundColour(wxColour(0xFF, 0x6F, 0x00));
    m_txt_mapping_sugs->SetMinSize(wxSize(FromDIP(580), -1));
    m_txt_mapping_sugs->SetMaxSize(wxSize(FromDIP(580), -1));
    m_txt_mapping_sugs->SetBackgroundColour(*wxWHITE);
    m_txt_mapping_sugs->SetLabel(_L("Your filament grouping method in the sliced file is not optimal."));
    //m_mapping_sugs_sizer->Add(m_img_mapping_sugs, 0, wxALIGN_CENTER, 0);
    m_mapping_sugs_sizer->Add(m_txt_mapping_sugs, 0, wxALIGN_CENTER, 0);

    m_change_filament_times_sizer = new wxBoxSizer(wxHORIZONTAL);
    //auto m_img_change_filament_times = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("warning", this, 16), wxDefaultPosition, wxSize(FromDIP(16), FromDIP(16)));
    m_txt_change_filament_times = new Label(m_scroll_area, wxEmptyString);
    m_txt_change_filament_times->SetFont(::Label::Body_13);
    m_txt_change_filament_times->SetMinSize(wxSize(FromDIP(580), -1));
    m_txt_change_filament_times->SetMaxSize(wxSize(FromDIP(580), -1));
    m_txt_change_filament_times->SetForegroundColour(wxColour(0xFF, 0x6F, 0x00));
    m_txt_change_filament_times->SetBackgroundColour(*wxWHITE);
    m_txt_change_filament_times->SetLabel(wxEmptyString);
    //m_change_filament_times_sizer->Add(m_img_change_filament_times, 0, wxTOP, FromDIP(2));
    m_change_filament_times_sizer->Add(m_txt_change_filament_times, 0, wxTOP, 0);

    /*Advanced Options*/
    wxBoxSizer* sizer_split_options = new wxBoxSizer(wxHORIZONTAL);
    auto m_split_options_line = new wxPanel(m_scroll_area, wxID_ANY);
    m_split_options_line->SetBackgroundColour(0xEEEEEE);
    m_split_options_line->SetSize(wxSize(-1, FromDIP(1)));
    m_split_options_line->SetMinSize(wxSize(-1, FromDIP(1)));
    m_split_options_line->SetMaxSize(wxSize(-1, FromDIP(1)));
    sizer_split_options->Add(0, 0, 0, wxEXPAND, 0);
    sizer_split_options->Add(m_split_options_line, 1, wxALIGN_CENTER, 0);

    m_options_other = new wxPanel(m_scroll_area);


    auto option_timelapse = new PrintOption(m_options_other, _L("Timelapse"), wxEmptyString, ops_no_auto, "timelapse");

        auto option_auto_bed_level = new PrintOption(
        m_options_other, _L("Auto Bed Leveling"),
        _L("This checks the flatness of heatbed. Leveling makes extruded height uniform.\n*Automatic mode: Run a leveling check(about 10 seconds). Skip if surface is fine."),
        ops_auto, "bed_leveling");

    auto option_flow_dynamics_cali =
        new PrintOption(m_options_other, _L("Flow Dynamics Calibration"),
                        _L("This process determines the dynamic flow values to improve overall print quality.\n*Automatic mode: Skip if the filament was calibrated recently."),
                        ops_auto, "flow_cali");

    auto option_nozzle_offset_cali_cali  = new PrintOption(
        m_options_other,
        _L("Nozzle Offset Calibration"),
        _L("Calibrate nozzle offsets to enhance print quality.\n*Automatic mode: Check for calibration before printing. Skip if unnecessary."),
        ops_auto, "nozzle_offset_cali"
    );

    m_sizer_options = new wxGridSizer(0, 2, FromDIP(5), FromDIP(10));
    m_sizer_options->Add(option_timelapse, 0, wxEXPAND);
    m_sizer_options->Add(option_auto_bed_level, 0, wxEXPAND);
    m_sizer_options->Add(option_flow_dynamics_cali, 0, wxEXPAND);
    m_sizer_options->Add(option_nozzle_offset_cali_cali, 0, wxEXPAND);

    m_checkbox_list_order.push_back(option_timelapse);
    m_checkbox_list_order.push_back(option_auto_bed_level);
    m_checkbox_list_order.push_back(option_flow_dynamics_cali);
    m_checkbox_list_order.push_back(option_nozzle_offset_cali_cali);

    m_options_other->SetSizer(m_sizer_options);
    m_options_other->Layout();
    m_options_other->Fit();

    m_checkbox_list["timelapse"]     = option_timelapse;
    m_checkbox_list["bed_leveling"]  = option_auto_bed_level;
    m_checkbox_list["flow_cali"]     = option_flow_dynamics_cali;
    m_checkbox_list["nozzle_offset_cali"] = option_nozzle_offset_cali_cali;
    for (auto print_opt : m_checkbox_list_order) {
        print_opt->Bind(EVT_SWITCH_PRINT_OPTION, [this](auto &e) { save_option_vals(); });
    }

    option_auto_bed_level->Hide();
    option_flow_dynamics_cali->Hide();
    option_nozzle_offset_cali_cali->Hide();

    m_simplebook   = new wxSimplebook(this, wxID_ANY, wxDefaultPosition, SELECT_MACHINE_DIALOG_SIMBOOK_SIZE2, 0);
    m_simplebook->SetMinSize(SELECT_MACHINE_DIALOG_SIMBOOK_SIZE2);
    m_simplebook->SetMaxSize(SELECT_MACHINE_DIALOG_SIMBOOK_SIZE2);

    // perpare mode
    m_panel_prepare = new wxPanel(m_simplebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_panel_prepare->SetBackgroundColour(m_colour_def_color);
    wxBoxSizer *m_sizer_prepare = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *m_sizer_pcont   = new wxBoxSizer(wxVERTICAL);

    m_button_ensure = new Button(m_panel_prepare, _L("Send"));
    m_button_ensure->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_button_ensure->Bind(wxEVT_BUTTON, &SelectMachineDialog::on_ok_btn, this);

    m_sizer_pcont->Add(0, 0, 1, wxEXPAND, 0);
    m_sizer_pcont->Add(m_button_ensure, 0,wxALIGN_CENTER, 0);

    m_sizer_prepare->Add(0, 0, 1, wxTOP, FromDIP(12));
    m_sizer_prepare->Add(m_sizer_pcont, 0, wxALIGN_CENTER, 0);

    m_panel_prepare->SetSizer(m_sizer_prepare);
    m_panel_prepare->Layout();
    m_simplebook->AddPage(m_panel_prepare, wxEmptyString, true);

    // sending mode
    m_status_bar    = std::make_shared<BBLStatusBarPrint>(m_simplebook);
    m_panel_sending = m_status_bar->get_panel();
    m_simplebook->AddPage(m_panel_sending, wxEmptyString, false);
    
    m_worker = std::make_unique<PlaterWorker<BoostThreadWorker>>(this, m_status_bar, "send_worker");

    // finish mode
    m_panel_finish = new wxPanel(m_simplebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_panel_finish->SetBackgroundColour(wxColour(135, 206, 250));
    wxBoxSizer *m_sizer_finish   = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *m_sizer_finish_v = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *m_sizer_finish_h = new wxBoxSizer(wxHORIZONTAL);

    auto imgsize      = FromDIP(25);
    auto completedimg = new wxStaticBitmap(m_panel_finish, wxID_ANY, create_scaled_bitmap("completed", m_panel_finish, 25), wxDefaultPosition, wxSize(imgsize, imgsize), 0);
    m_sizer_finish_h->Add(completedimg, 0, wxALIGN_CENTER | wxALL, FromDIP(5));

    m_statictext_finish = new wxStaticText(m_panel_finish, wxID_ANY, L("send completed"), wxDefaultPosition, wxDefaultSize, 0);
    m_statictext_finish->Wrap(-1);
    m_statictext_finish->SetForegroundColour(wxColour(0, 150, 136));
    m_sizer_finish_h->Add(m_statictext_finish, 0, wxALIGN_CENTER | wxALL, FromDIP(5));

    m_sizer_finish_v->Add(m_sizer_finish_h, 1, wxALIGN_CENTER, 0);

    m_sizer_finish->Add(m_sizer_finish_v, 1, wxALIGN_CENTER, 0);

    m_panel_finish->SetSizer(m_sizer_finish);
    m_panel_finish->Layout();
    m_sizer_finish->Fit(m_panel_finish);
    m_simplebook->AddPage(m_panel_finish, wxEmptyString, false);

    //show bind failed info
    m_sw_print_failed_info = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(SELECT_MACHINE_DIALOG_SIMBOOK_SIZE2.x, FromDIP(125)), wxVSCROLL);
    m_sw_print_failed_info->SetBackgroundColour(*wxWHITE);
    m_sw_print_failed_info->SetScrollRate(0, 5);
    m_sw_print_failed_info->SetMinSize(wxSize(SELECT_MACHINE_DIALOG_SIMBOOK_SIZE2.x, FromDIP(125)));
    m_sw_print_failed_info->SetMaxSize(wxSize(SELECT_MACHINE_DIALOG_SIMBOOK_SIZE2.x, FromDIP(125)));

    wxBoxSizer* sizer_print_failed_info = new wxBoxSizer(wxVERTICAL);
    m_sw_print_failed_info->SetSizer(sizer_print_failed_info);


    wxBoxSizer* sizer_error_code = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* sizer_error_desc = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* sizer_extra_info = new wxBoxSizer(wxHORIZONTAL);

    auto st_title_error_code = new wxStaticText(m_sw_print_failed_info, wxID_ANY, _L("Error code"));
    auto st_title_error_code_doc = new wxStaticText(m_sw_print_failed_info, wxID_ANY,": ");
    m_st_txt_error_code = new Label(m_sw_print_failed_info, wxEmptyString);
    st_title_error_code->SetForegroundColour(0x909090);
    st_title_error_code_doc->SetForegroundColour(0x909090);
    m_st_txt_error_code->SetForegroundColour(0x909090);
    st_title_error_code->SetFont(::Label::Body_13);
    st_title_error_code_doc->SetFont(::Label::Body_13);
    m_st_txt_error_code->SetFont(::Label::Body_13);
    st_title_error_code->SetMinSize(wxSize(FromDIP(74), -1));
    st_title_error_code->SetMaxSize(wxSize(FromDIP(74), -1));
    m_st_txt_error_code->SetMinSize(wxSize(FromDIP(500), -1));
    m_st_txt_error_code->SetMaxSize(wxSize(FromDIP(500), -1));
    sizer_error_code->Add(st_title_error_code, 0, wxALL, 0);
    sizer_error_code->Add(st_title_error_code_doc, 0, wxALL, 0);
    sizer_error_code->Add(m_st_txt_error_code, 0, wxALL, 0);


    auto st_title_error_desc = new wxStaticText(m_sw_print_failed_info, wxID_ANY, wxT("Error desc"));
    auto st_title_error_desc_doc = new wxStaticText(m_sw_print_failed_info, wxID_ANY,": ");
    m_st_txt_error_desc = new Label(m_sw_print_failed_info, wxEmptyString);
    st_title_error_desc->SetForegroundColour(0x909090);
    st_title_error_desc_doc->SetForegroundColour(0x909090);
    m_st_txt_error_desc->SetForegroundColour(0x909090);
    st_title_error_desc->SetFont(::Label::Body_13);
    st_title_error_desc_doc->SetFont(::Label::Body_13);
    m_st_txt_error_desc->SetFont(::Label::Body_13);
    st_title_error_desc->SetMinSize(wxSize(FromDIP(74), -1));
    st_title_error_desc->SetMaxSize(wxSize(FromDIP(74), -1));
    m_st_txt_error_desc->SetMinSize(wxSize(FromDIP(500), -1));
    m_st_txt_error_desc->SetMaxSize(wxSize(FromDIP(500), -1));
    sizer_error_desc->Add(st_title_error_desc, 0, wxALL, 0);
    sizer_error_desc->Add(st_title_error_desc_doc, 0, wxALL, 0);
    sizer_error_desc->Add(m_st_txt_error_desc, 0, wxALL, 0);

    auto st_title_extra_info = new wxStaticText(m_sw_print_failed_info, wxID_ANY, wxT("Extra info"));
    auto st_title_extra_info_doc = new wxStaticText(m_sw_print_failed_info, wxID_ANY, ": ");
    m_st_txt_extra_info = new Label(m_sw_print_failed_info, wxEmptyString);
    st_title_extra_info->SetForegroundColour(0x909090);
    st_title_extra_info_doc->SetForegroundColour(0x909090);
    m_st_txt_extra_info->SetForegroundColour(0x909090);
    st_title_extra_info->SetFont(::Label::Body_13);
    st_title_extra_info_doc->SetFont(::Label::Body_13);
    m_st_txt_extra_info->SetFont(::Label::Body_13);
    st_title_extra_info->SetMinSize(wxSize(FromDIP(74), -1));
    st_title_extra_info->SetMaxSize(wxSize(FromDIP(74), -1));
    m_st_txt_extra_info->SetMinSize(wxSize(FromDIP(500), -1));
    m_st_txt_extra_info->SetMaxSize(wxSize(FromDIP(500), -1));
    sizer_extra_info->Add(st_title_extra_info, 0, wxALL, 0);
    sizer_extra_info->Add(st_title_extra_info_doc, 0, wxALL, 0);
    sizer_extra_info->Add(m_st_txt_extra_info, 0, wxALL, 0);


    m_link_network_state = new wxHyperlinkCtrl(m_sw_print_failed_info, wxID_ANY,_L("Check the status of current system services"),"");
    m_link_network_state->SetFont(::Label::Body_12);
    m_link_network_state->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {wxGetApp().link_to_network_check();});
    m_link_network_state->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {m_link_network_state->SetCursor(wxCURSOR_HAND);});
    m_link_network_state->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {m_link_network_state->SetCursor(wxCURSOR_ARROW);});

    sizer_print_failed_info->Add(m_link_network_state, 0, wxLEFT, 5);
    sizer_print_failed_info->Add(sizer_error_code, 0, wxLEFT, 5);
    sizer_print_failed_info->Add(0, 0, 0, wxTOP, FromDIP(3));
    sizer_print_failed_info->Add(sizer_error_desc, 0, wxLEFT, 5);
    sizer_print_failed_info->Add(0, 0, 0, wxTOP, FromDIP(3));
    sizer_print_failed_info->Add(sizer_extra_info, 0, wxLEFT, 5);

    m_scroll_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    m_scroll_sizer->Add(0, 0, 0, wxTOP, FromDIP(11));
    //m_sizer_main->Add(m_sizer_mode_switch, 0, wxALIGN_CENTER, 0);
    //m_scroll_sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    m_scroll_sizer->AddStretchSpacer();
    m_scroll_sizer->Add(m_basic_panel, 0, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(15));
    m_scroll_sizer->Add(0, 0, 0, wxTOP, FromDIP(14));
    m_scroll_sizer->Add(sizer_split_filament, 1, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(15));
    m_scroll_sizer->Add(m_filament_panel, 0, wxALIGN_CENTER|wxLEFT|wxRIGHT, FromDIP(15));
    m_scroll_sizer->Add(m_sizer_filament_2extruder, 0, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(15));
    m_scroll_sizer->Add(0, 0, 0, wxTOP, FromDIP(6));
    m_scroll_sizer->Add(m_statictext_ams_msg, 0, wxLEFT|wxRIGHT, FromDIP(15));
    m_scroll_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));
    m_scroll_sizer->Add(m_mapping_sugs_sizer, 0, wxLEFT|wxRIGHT, FromDIP(15));
    m_scroll_sizer->Add(m_change_filament_times_sizer, 0,wxLEFT|wxRIGHT, FromDIP(15));
    // m_scroll_sizer->Add(m_link_edit_nozzle, 0, wxLEFT|wxRIGHT, FromDIP(15));
    m_scroll_sizer->Add(suggestion_sizer, 0, wxLEFT|wxRIGHT|wxEXPAND, FromDIP(15));
    m_scroll_sizer->Add(sizer_split_options, 1, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(15));
    m_scroll_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));
    m_scroll_sizer->Add(m_options_other, 0, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(15));
    m_scroll_sizer->Add(0, 0, 0, wxTOP, FromDIP(30));

    show_print_failed_info(false);

    m_scroll_area->SetSizer(m_scroll_sizer);

    wxBoxSizer *sizer_main = new wxBoxSizer(wxVERTICAL);
    sizer_main->Add(m_scroll_area, 1, wxEXPAND, 0);
    sizer_main->Add(m_simplebook, 0, wxALIGN_CENTER, 0);
    sizer_main->Add(0, 0, 0, wxTOP, FromDIP(10));
    sizer_main->Add(m_sw_print_failed_info, 0, wxALIGN_CENTER, 0);
    sizer_main->Add(0, 0, 0, wxTOP, FromDIP(18));

    SetSizer(sizer_main);
    Layout();
    Fit();
    Thaw();

    init_bind();
    init_timer();
    Centre(wxBOTH);
    wxGetApp().UpdateDlgDarkUI(this);
}

void SelectMachineDialog::init_bind()
{
    Bind(wxEVT_TIMER, &SelectMachineDialog::on_timer, this);
    Bind(EVT_CLEAR_IPADDRESS, &SelectMachineDialog::clear_ip_address_config, this);
    Bind(EVT_SHOW_ERROR_INFO, [this](auto& e) {show_print_failed_info(true);});
    Bind(EVT_UPDATE_USER_MACHINE_LIST, &SelectMachineDialog::update_printer_combobox, this);
    Bind(EVT_PRINT_JOB_CANCEL, &SelectMachineDialog::on_print_job_cancel, this);
    Bind(EVT_SET_FINISH_MAPPING, &SelectMachineDialog::on_set_finish_mapping, this);
    Bind(wxEVT_LEFT_DOWN, [this](auto& e) {check_fcous_state(this);e.Skip();});
    m_panel_prepare->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {check_fcous_state(this);e.Skip();});
    m_basic_panel->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {check_fcous_state(this);e.Skip();});
    m_bitmap_last_plate->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_HAND); });
    m_bitmap_last_plate->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_ARROW); });
    m_bitmap_next_plate->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_HAND); });
    m_bitmap_next_plate->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_ARROW); });


    Bind(EVT_CONNECT_LAN_MODE_PRINT, [this](wxCommandEvent& e) {
        if (e.GetInt() == 0) {
            DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
            if (!dev) return;
            MachineObject* obj = dev->get_selected_machine();
            if (!obj) return;

            if (obj->get_dev_id() == e.GetString()) {
                m_printer_box->SetPrinterName(wxString::FromUTF8(obj->get_dev_name()) + "(LAN)");
            }
        }
    });

    m_bitmap_last_plate->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        if (m_print_plate_idx > 0) {
            m_print_plate_idx--;
            update_page_turn_state(true);
            set_default_from_sdcard();
            Layout();
        }
    });

    m_bitmap_next_plate->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        if (m_print_plate_idx < (m_print_plate_total - 1)) {
            m_print_plate_idx++;
            update_page_turn_state(true);
            set_default_from_sdcard();
            Layout();
        }
    });

    m_check_ext_change_assist->Bind(wxEVT_TOGGLEBUTTON, [this](auto &e) {
        m_ext_change_assist = e.IsChecked();
        e.Skip();
    });
}

void SelectMachineDialog::show_print_failed_info(bool show, int code, wxString description, wxString extra)
{

    if (show) {
        if (!m_sw_print_failed_info->IsShown()) {
            m_sw_print_failed_info->Show(true);

            m_st_txt_error_code->SetLabelText(wxString::Format("%d", m_print_error_code));
            m_st_txt_error_desc->SetLabelText( wxGetApp().filter_string(m_print_error_msg));
            m_st_txt_extra_info->SetLabelText( wxGetApp().filter_string(m_print_error_extra));

            m_st_txt_error_code->Wrap(FromDIP(500));
            m_st_txt_error_desc->Wrap(FromDIP(500));
            m_st_txt_extra_info->Wrap(FromDIP(500));
        }
        else {
            m_sw_print_failed_info->Show(false);
        }
        Layout();
        Fit();
    }
    else {
        if (!m_sw_print_failed_info->IsShown()) {return;}
        m_sw_print_failed_info->Show(false);
        m_st_txt_error_code->SetLabelText(wxEmptyString);
        m_st_txt_error_desc->SetLabelText(wxEmptyString);
        m_st_txt_extra_info->SetLabelText(wxEmptyString);
        Layout();
        Fit();
    }
}

void SelectMachineDialog::check_fcous_state(wxWindow* window)
{
    if (window == m_rename_input || window == m_rename_input->GetTextCtrl()) { on_rename_enter(); }
    auto children = window->GetChildren();
    for (auto child : children) {
        check_fcous_state(child);
    }
}

void SelectMachineDialog::popup_filament_backup()
{
    if (m_print_status == PrintStatusUnsupportedPrinter) {
        return;
    }

    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;
    if (dev->get_selected_machine()/* && dev->get_selected_machine()->filam_bak.size() > 0*/) {
        AmsReplaceMaterialDialog* m_replace_material_popup = new AmsReplaceMaterialDialog(this);
        m_replace_material_popup->update_machine_obj(dev->get_selected_machine());
        m_replace_material_popup->ShowModal();
    }
}

void SelectMachineDialog::update_select_layout(MachineObject *obj)
{
    update_option_opts(obj);

    load_option_vals(obj);
    if (obj && obj->get_printer_arch() == PrinterArch::ARCH_I3) { m_checkbox_list["timelapse"]->setValue("off"); } /*off timelapse on selected for n series by zhimin.zeng*/
    save_option_vals(obj);

    Layout();
    Fit();
}

void SelectMachineDialog::prepare_mode(bool refresh_button)
{
    Enable_Auto_Refill(true);
    show_print_failed_info(false);

    m_is_in_sending_mode = false;
    m_worker->wait_for_idle();

    if (wxIsBusy())
        wxEndBusyCursor();

    if (refresh_button) {
        EnableEditing(true);
        Enable_Send_Button(true);
    }

    m_status_bar->reset();
    if (m_simplebook->GetSelection() != 0) {
        m_simplebook->SetSelection(0);
        Layout();
        Fit();
    }

    if (m_print_page_mode != PrintPageModePrepare) {
        m_print_page_mode = PrintPageModePrepare;
        for (auto it = m_materialList.begin(); it != m_materialList.end(); it++) {
            it->second->item->enable();
        }
    }
}

void SelectMachineDialog::sending_mode()
{
    // disable combobox
    Enable_Auto_Refill(false);
    EnableEditing(false);

    m_is_in_sending_mode = true;
    if (m_simplebook->GetSelection() != 1){
        m_simplebook->SetSelection(1);
        Layout();
        Fit();
    }


    if (m_print_page_mode != PrintPageModeSending) {
        m_print_page_mode = PrintPageModeSending;
        for (auto it = m_materialList.begin(); it != m_materialList.end(); it++) {
            it->second->item->disable();
        }
    }
}

void SelectMachineDialog::finish_mode()
{
    EnableEditing(true);

    m_print_page_mode = PrintPageModeFinish;
    m_is_in_sending_mode = false;
    m_simplebook->SetSelection(2);
    Layout();
    Fit();
}


void SelectMachineDialog::sync_ams_mapping_result(std::vector<FilamentInfo> &result)
{
    if (result.empty()) {
        BOOST_LOG_TRIVIAL(info) << "ams_mapping result is empty";
        for (auto it = m_materialList.begin(); it != m_materialList.end(); it++) {
            wxString ams_id = "Ext";//
            wxColour ams_col = wxColour(0xCE, 0xCE, 0xCE);
            it->second->item->set_ams_info(ams_col, ams_id);
        }
        return;
    }

    for (auto f = result.begin(); f != result.end(); f++) {
        BOOST_LOG_TRIVIAL(trace) << "ams_mapping f id = " << f->id << ", tray_id = " << f->tray_id << ", color = " << f->color << ", type = " << f->type;

        MaterialHash::iterator iter = m_materialList.begin();
        while (iter != m_materialList.end()) {
            int           id   = iter->second->id;
            Material *    item = iter->second;
            MaterialItem *m    = item->item;

            if (f->id == id) {
                wxString ams_id;
                wxColour ams_col;

                if (f->tray_id == VIRTUAL_TRAY_MAIN_ID || f->tray_id == VIRTUAL_TRAY_DEPUTY_ID)
                {
                    ams_id = "Ext";
                }else if (f->tray_id >= 0) {
                    ams_id = wxGetApp().transition_tridid(f->tray_id);
                } else {
                    ams_id = "-";
                }

                if (!f->color.empty()) {
                    ams_col = DevAmsTray::decode_color(f->color);
                } else {
                    // default color
                    ams_col = wxColour(0xCE, 0xCE, 0xCE);
                }
                std::vector<wxColour> cols;
                for (auto col : f->colors) {
                    cols.push_back(DevAmsTray::decode_color(col));
                }
                m->set_ams_info(ams_col, ams_id,f->ctype, cols);
                break;
            }
            iter++;
        }
    }
    auto tab_index = (MainFrame::TabPosition) dynamic_cast<Notebook *>(wxGetApp().tab_panel())->GetSelection();
    if (tab_index == MainFrame::TabPosition::tp3DEditor || tab_index == MainFrame::TabPosition::tpPreview) {
        updata_thumbnail_data_after_connected_printer();
    }
}

void print_ams_mapping_result(std::vector<FilamentInfo>& result)
{
    if (result.empty()) {
        BOOST_LOG_TRIVIAL(info) << "print_ams_mapping_result: empty";
    }

    char buffer[256];
    for (int i = 0; i < result.size(); i++) {
        ::sprintf(buffer, "print_ams_mapping: F(%02d) -> A(%02d)", result[i].id+1, result[i].tray_id+1);
        BOOST_LOG_TRIVIAL(info) << std::string(buffer);
    }
}

bool SelectMachineDialog::do_ams_mapping(MachineObject *obj_,bool use_ams)
{
    if (!obj_) return false;
    if (!obj_->HasAms()) { use_ams = false;} /*STUDIO-11971*/
    obj_->get_ams_colors(m_cur_colors_in_thumbnail);
    // try color and type mapping

    const auto& full_config = wxGetApp().preset_bundle->full_config();
    const auto& project_config = wxGetApp().preset_bundle->project_config;
    size_t nozzle_nums = full_config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();

    if (m_print_type == FROM_NORMAL) {
        m_filaments_map = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_real_filament_maps(project_config);
    }

    int filament_result = 0;
    std::vector<bool> map_opt;  //four values: use_left_ams, use_right_ams, use_left_ext, use_right_ext
    if (nozzle_nums > 1){
        //get nozzle property, the extders are same?
        if (true/*!can_hybrid_mapping(obj_get_extder_data())*/){
            std::vector<FilamentInfo>           m_ams_mapping_result_left, m_ams_mapping_result_right;
            std::vector<FilamentInfo>           m_filament_left, m_filament_right;
            for (auto it = m_filaments.begin(); it != m_filaments.end(); it++){
                if (it->id < 0 || it->id > m_filaments_map.size()){
                    BOOST_LOG_TRIVIAL(info) << "error: do_ams_mapping: m_filaments[].it" << it->id;
                    BOOST_LOG_TRIVIAL(info) << "m_filaments_map.size()" << m_filaments_map.size();
                    return false;
                }

                if (it->id < m_filaments_map.size()) {
                    if (m_filaments_map[it->id] == 1)
                        m_filament_left.push_back(*it);
                    else if (m_filaments_map[it->id] == 2)
                        m_filament_right.push_back(*it);
                }
            }

            bool has_left_ams = false, has_right_ams = false;
            for (auto ams_item : obj_->GetFilaSystem()->GetAmsList()) {
                if (ams_item.second->GetExtruderId() == 0) {
                    if (obj_->is_main_extruder_on_left())
                        has_left_ams = true;
                    else
                        has_right_ams = true;
                }
                else if (ams_item.second->GetExtruderId() == 1) {
                    if (obj_->is_main_extruder_on_left())
                        has_right_ams = true;
                    else
                        has_left_ams = true;
                }

                if (has_left_ams && has_right_ams)
                    break;
            }

            map_opt = {true, false, !has_left_ams, false};   //four values: use_left_ams, use_right_ams, use_left_ext, use_right_ext
            if (!use_ams) {
                map_opt[0] = false;
                map_opt[2] = true;
            }
            int result_first = DevMappingUtil::ams_filament_mapping(obj_, m_filament_left, m_ams_mapping_result_left, map_opt);
            map_opt = { false, true, false, !has_right_ams };
            if (!use_ams) {
                map_opt[1] = false;
                map_opt[3] = true;
            }
            int result_second = DevMappingUtil::ams_filament_mapping(obj_, m_filament_right, m_ams_mapping_result_right, map_opt);

            //m_ams_mapping_result.clear();
            m_ams_mapping_result.resize(m_ams_mapping_result_left.size() + m_ams_mapping_result_right.size());
            std::merge(m_ams_mapping_result_left.begin(), m_ams_mapping_result_left.end(), m_ams_mapping_result_right.begin(),
                m_ams_mapping_result_right.end(), m_ams_mapping_result.begin(),
                [](const FilamentInfo& f1, const FilamentInfo& f2) {
                    return f1.id < f2.id; // Merge based on age
                });
            filament_result = (result_first && result_second);
        }
        //can hybrid mapping
        else {
            map_opt = { true, true, true, true };   //four values: use_left_ams, use_right_ams, use_left_ext, use_right_ext
            if (!use_ams) {
                map_opt[0] = false;
                map_opt[1] = false;
            }
            filament_result = DevMappingUtil::ams_filament_mapping(obj_, m_filaments, m_ams_mapping_result, map_opt);
        }
        //When filaments cannot be matched automatically, whether to use ext for automatic supply
        //auto_supply_with_ext(obj_->vt_slot);
    }

    //single nozzle
    else {
        if (obj_->is_support_amx_ext_mix_mapping()){
            map_opt = { false, true, false, false }; //four values: use_left_ams, use_right_ams, use_left_ext, use_right_ext
            if (!use_ams) {
                map_opt[1] = false;
                map_opt[3] = true;
            }
            filament_result = DevMappingUtil::ams_filament_mapping(obj_, m_filaments, m_ams_mapping_result, map_opt);
            //auto_supply_with_ext(obj_->vt_slot);
        }
        else {
            map_opt = { false, true, false, false };
            if (!use_ams) {
                map_opt[1] = false;
                map_opt[3] = true;
            }
            filament_result = DevMappingUtil::ams_filament_mapping(obj_, m_filaments, m_ams_mapping_result, map_opt);
        }
    }

    if (filament_result == 0) {
        print_ams_mapping_result(m_ams_mapping_result);
        std::string ams_array;
        std::string ams_array2;
        std::string mapping_info;
        get_ams_mapping_result(ams_array, ams_array2, mapping_info);
        if (ams_array.empty()) {
            reset_ams_material();
            if (m_print_type == FROM_NORMAL) {
                updata_thumbnail_data_after_connected_printer(); // FIX:update cur plate thumbnail
            }
            BOOST_LOG_TRIVIAL(info) << "ams_mapping_array=[]";
        } else {
            sync_ams_mapping_result(m_ams_mapping_result);
            BOOST_LOG_TRIVIAL(info) << "ams_mapping_array=" << ams_array;
            BOOST_LOG_TRIVIAL(info) << "ams_mapping_array2=" << ams_array2;
            BOOST_LOG_TRIVIAL(info) << "ams_mapping_info=" << mapping_info;
        }
        return DevMappingUtil::is_valid_mapping_result(obj_, m_ams_mapping_result);
    } else {
        // do not support ams mapping try to use order mapping
        bool is_valid = DevMappingUtil::is_valid_mapping_result(obj_, m_ams_mapping_result);
        if (filament_result != 1 && !is_valid) {
            //reset invalid result
            for (int i = 0; i < m_ams_mapping_result.size(); i++) {
                m_ams_mapping_result[i].tray_id = -1;
                m_ams_mapping_result[i].distance = 99999;
            }
        }
        sync_ams_mapping_result(m_ams_mapping_result);
        return is_valid;
    }

    return true;
}

bool SelectMachineDialog::get_ams_mapping_result(std::string &mapping_array_str, std::string& mapping_array_str2, std::string &ams_mapping_info)
{
    if (m_ams_mapping_result.empty())
        return false;

    bool valid_mapping_result = true;
    int invalid_count = 0;
    for (int i = 0; i < m_ams_mapping_result.size(); i++) {
        if (m_ams_mapping_result[i].tray_id == -1) {
            valid_mapping_result = false;
            invalid_count++;
        }
    }

    if (invalid_count == m_ams_mapping_result.size()) {
        return false;
    } else {
        if (m_print_type == FROM_NORMAL) {
            json mapping_v0_json   = json::array();
            json mapping_v1_json   = json::array();
            json mapping_info_json = json::array();

            /* get filament maps */
            std::vector<int> filament_maps;
            Plater *         plater = wxGetApp().plater();
            if (plater) {
                PartPlate *curr_plate = plater->get_partplate_list().get_curr_plate();
                if (curr_plate) {
                    filament_maps = curr_plate->get_filament_maps();
                } else {
                    BOOST_LOG_TRIVIAL(error) << "get_ams_mapping_result, curr_plate is nullptr";
                }
            } else {
                BOOST_LOG_TRIVIAL(error) << "get_ams_mapping_result, plater is nullptr";
            }

            for (int i = 0; i < wxGetApp().preset_bundle->filament_presets.size(); i++) {
                int  tray_id = -1;
                json mapping_item_v1;
                mapping_item_v1["ams_id"]  = 0xff;
                mapping_item_v1["slot_id"] = 0xff;
                json mapping_item;
                mapping_item["ams"]          = tray_id;
                mapping_item["targetColor"]  = "";
                mapping_item["filamentId"]   = "";
                mapping_item["filamentType"] = "";
                for (int k = 0; k < m_ams_mapping_result.size(); k++) {
                    if (m_ams_mapping_result[k].id == i) {
                        tray_id                      = m_ams_mapping_result[k].tray_id;
                        mapping_item["ams"]          = tray_id;
                        mapping_item["filamentType"] = m_filaments[k].type;
                        if (i >= 0 && i < wxGetApp().preset_bundle->filament_presets.size()) {
                            auto it = wxGetApp().preset_bundle->filaments.find_preset(wxGetApp().preset_bundle->filament_presets[i]);
                            if (it != nullptr) { mapping_item["filamentId"] = it->filament_id; }
                        }
                        /* nozzle id */
                        if (i >= 0 && i < filament_maps.size()) { mapping_item["nozzleId"] = convert_filament_map_nozzle_id_to_task_nozzle_id(filament_maps[i]); }
                        // convert #RRGGBB to RRGGBBAA
                        mapping_item["sourceColor"] = m_filaments[k].color;
                        mapping_item["targetColor"] = m_ams_mapping_result[k].color;
                        if (tray_id == VIRTUAL_TRAY_MAIN_ID || tray_id == VIRTUAL_TRAY_DEPUTY_ID) { tray_id = -1; }

                        /*new ams mapping data*/
                        try {
                            if (m_ams_mapping_result[k].ams_id.empty() || m_ams_mapping_result[k].slot_id.empty()) { // invalid case
                                mapping_item_v1["ams_id"]  = VIRTUAL_TRAY_MAIN_ID;
                                mapping_item_v1["slot_id"] = VIRTUAL_TRAY_MAIN_ID;
                            } else {
                                mapping_item_v1["ams_id"]  = std::stoi(m_ams_mapping_result[k].ams_id);
                                mapping_item_v1["slot_id"] = std::stoi(m_ams_mapping_result[k].slot_id);
                            }
                        } catch (...) {}
                    }
                }
                mapping_v0_json.push_back(tray_id);
                mapping_v1_json.push_back(mapping_item_v1);
                mapping_info_json.push_back(mapping_item);
            }

            mapping_array_str  = mapping_v0_json.dump();
            mapping_array_str2 = mapping_v1_json.dump();
            ams_mapping_info   = mapping_info_json.dump();
            return valid_mapping_result;
        } else if(m_print_type == FROM_SDCARD_VIEW){
            json mapping_v0_json   = json::array();
            json mapping_v1_json   = json::array();

            for (int  i = 0; i < m_filaments_map.size(); i++) {
                int mapping_result = -1;
                json mapping_item_v1;
                mapping_item_v1["ams_id"]  = 0xff;
                mapping_item_v1["slot_id"] = 0xff;

                try {
                    for (int k = 0; k < m_ams_mapping_result.size(); k++) {
                        if (m_ams_mapping_result[k].id == i) {
                            mapping_result = m_ams_mapping_result[k].tray_id;
                            mapping_item_v1["ams_id"]  = std::stoi(m_ams_mapping_result[k].ams_id);
                            mapping_item_v1["slot_id"] = std::stoi(m_ams_mapping_result[k].slot_id);
                        }
                    }
                } catch (...) {}

                mapping_v0_json.push_back(mapping_result);
                mapping_v1_json.push_back(mapping_item_v1);
                mapping_array_str  = mapping_v0_json.dump();
                mapping_array_str2 = mapping_v1_json.dump();
            }
            return true;
        }
    }
    return true;
}

bool SelectMachineDialog::build_nozzles_info(std::string& nozzles_info)
{
    /* init nozzles info */
    json nozzle_info_json = json::array();
    nozzles_info = nozzle_info_json.dump();

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle)
        return false;
    auto opt_nozzle_diameters = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    if (opt_nozzle_diameters == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "build_nozzles_info, opt_nozzle_diameters is nullptr";
        return false;
    }
    auto opt_nozzle_volume_type = preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    if (opt_nozzle_volume_type == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "build_nozzles_info, opt_nozzle_volume_type is nullptr";
        return false;
    }
    json nozzle_item;
    /* only o1d two nozzles has build_nozzles info now */
    if (opt_nozzle_diameters->size() != 2) {
        return false;
    }
    for (size_t i = 0; i < opt_nozzle_diameters->size(); i++) {
        if (i == (size_t)ConfigNozzleIdx::NOZZLE_LEFT) {
            nozzle_item["id"] = CloudTaskNozzleId::NOZZLE_LEFT;
        }
        else if (i == (size_t)ConfigNozzleIdx::NOZZLE_RIGHT) {
            nozzle_item["id"] = CloudTaskNozzleId::NOZZLE_RIGHT;
        }
        else {
            /* unknown ConfigNozzleIdx */
            BOOST_LOG_TRIVIAL(error) << "build_nozzles_info, unknown ConfigNozzleIdx = " << i;
            assert(false);
            continue;
        }
        nozzle_item["type"] = nullptr;
        if (i >= 0 && i < opt_nozzle_volume_type->size()) {
            nozzle_item["flowSize"] = get_nozzle_volume_type_cloud_string((NozzleVolumeType)opt_nozzle_volume_type->get_at(i));
        }
        if (i >= 0 && i < opt_nozzle_diameters->size()) {
            nozzle_item["diameter"] = opt_nozzle_diameters->get_at(i);
        }
        nozzle_info_json.push_back(nozzle_item);
    }
    nozzles_info = nozzle_info_json.dump();
    return true;
}

bool SelectMachineDialog::can_hybrid_mapping(DevExtderSystem data) {
    // Mixed mappings are not allowed
    return false;

    if (data.GetTotalExtderCount() <= 1 || !wxGetApp().preset_bundle)
        return false;

    //The default two extruders are left, right, but the order of the extruders on the machine is right, left.
    //Therefore, some adjustments need to be made.
    std::vector<std::string>flow_type_of_machine;
    for (const auto& ext : data.GetExtruders())
    {
        std::string type_str = ext.GetNozzleFlowType() == NozzleFlowType::H_FLOW ? "High Flow" : "Standard";
        flow_type_of_machine.push_back(type_str);
    }

    //get the nozzle type of preset --> flow_types
    const Preset& current_printer = wxGetApp().preset_bundle->printers.get_selected_preset();
    const Preset* base_printer = wxGetApp().preset_bundle->printers.get_preset_base(current_printer);
    std::string base_name = base_printer->name;
    auto flow_data = wxGetApp().app_config->get_nozzle_volume_types_from_config(base_name);
    std::vector<string> flow_types;
    boost::split(flow_types, flow_data, boost::is_any_of(","));
    if (flow_types.size() <= 1 || flow_types.size() != flow_type_of_machine.size()) return false;

    //Only when all preset nozzle types and machine nozzle types are exactly the same, return true.
    auto type = flow_types[0];
    for (int i = 0; i < flow_types.size(); i++){
        if (flow_types[i] != type || flow_type_of_machine[i] != type)
            return false;
    }
    return true;
}

//When filaments cannot be matched automatically, whether to use ext for automatic supply
void SelectMachineDialog::auto_supply_with_ext(std::vector<DevAmsTray> slots) {
    if (slots.size() <= 0) return;

    for (int i = 0; i < m_ams_mapping_result.size(); i++) {
        auto it = m_ams_mapping_result[i];
        if (it.ams_id == "") {
            DevAmsTray slot("");
            if (m_filaments_map[it.id] == 1 && slots.size() > 1) slot = slots[1];
            else if (m_filaments_map[it.id] == 2) slot = slots[0];
            if (slot.id.empty()) continue;
            m_ams_mapping_result[i].ams_id = slot.id;
            m_ams_mapping_result[i].color = slot.color;
            m_ams_mapping_result[i].type = slot.m_fila_type;
            m_ams_mapping_result[i].colors = slot.cols;
            m_ams_mapping_result[i].tray_id = atoi(slot.id.c_str());
            m_ams_mapping_result[i].slot_id = "0";
        }
    }
}

bool SelectMachineDialog::is_nozzle_type_match(DevExtderSystem data, wxString& error_message) const {
    if (data.GetTotalExtderCount() <= 1 || !wxGetApp().preset_bundle)
        return false;

    const auto& project_config = wxGetApp().preset_bundle->project_config;
    //check nozzle used
    auto used_filaments = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_used_filaments(); // 1 based
    auto filament_maps  = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_real_filament_maps(project_config);  // 1 based
    std::map<int, std::string> used_extruders_flow;
    std::vector<int> used_extruders; // 0 based
    for (auto f : used_filaments) {
        int filament_extruder = filament_maps[f - 1] - 1;
        if (std::find(used_extruders.begin(), used_extruders.end(), filament_extruder) == used_extruders.end()) used_extruders.emplace_back(filament_extruder);
    }

    std::sort(used_extruders.begin(), used_extruders.end());

    auto nozzle_volume_type_opt = dynamic_cast<const ConfigOptionEnumsGeneric *>(wxGetApp().preset_bundle->project_config.option("nozzle_volume_type"));
    for (auto i = 0; i < used_extruders.size(); i++) {
        if (nozzle_volume_type_opt) {
            NozzleVolumeType nozzle_volume_type = (NozzleVolumeType) (nozzle_volume_type_opt->get_at(used_extruders[i]));
            if (nozzle_volume_type == NozzleVolumeType::nvtStandard) { used_extruders_flow[used_extruders[i]] = "Standard";}
            else {used_extruders_flow[used_extruders[i]] = "High Flow";}
        }
    }

    vector<int> map_extruders = {1, 0};


    // The default two extruders are left, right, but the order of the extruders on the machine is right, left.
    std::vector<std::string> flow_type_of_machine;
    for (const auto& it : data.GetExtruders())
    {
        if (it.GetNozzleFlowType() == NozzleFlowType::H_FLOW)
        {
            flow_type_of_machine.push_back(L("High Flow"));
        }
        else if (it.GetNozzleFlowType() == NozzleFlowType::S_FLOW)
        {
            flow_type_of_machine.push_back(L("Standard"));
        }
    }

    //Only when all preset nozzle types and machine nozzle types are exactly the same, return true.
    for (std::map<int, std::string>::iterator it = used_extruders_flow.begin(); it!= used_extruders_flow.end(); it++) {
        int target_machine_nozzle_id = map_extruders[it->first];

        if (target_machine_nozzle_id < flow_type_of_machine.size()) {
            if (flow_type_of_machine[target_machine_nozzle_id] != used_extruders_flow[it->first]) {

                wxString pos;
                if (target_machine_nozzle_id == DEPUTY_EXTRUDER_ID)
                {
                    pos = _L("left nozzle");
                }
                else if(target_machine_nozzle_id == MAIN_EXTRUDER_ID)
                {
                    pos = _L("right nozzle");
                }

                error_message = wxString::Format(_L("The nozzle flow setting of %s(%s) doesn't match with the slicing file(%s). "
                                                    "Please make sure the nozzle installed matches with settings in printer, "
                                                    "then set the corresponding printer preset while slicing."), pos,
                                                    _L(flow_type_of_machine[target_machine_nozzle_id]),
                                                    _L(used_extruders_flow[it->first]));
                return false;
            }
        }
    }
    return true;
}

int SelectMachineDialog::convert_filament_map_nozzle_id_to_task_nozzle_id(int nozzle_id)
{
    if (nozzle_id == (int)FilamentMapNozzleId::NOZZLE_LEFT) {
        return (int)CloudTaskNozzleId::NOZZLE_LEFT;
    }
    else if (nozzle_id == (int)FilamentMapNozzleId::NOZZLE_RIGHT) {
        return (int)CloudTaskNozzleId::NOZZLE_RIGHT;
    }
    else {
        /* unsupported nozzle id */
        assert(false);
        return nozzle_id;
    }
}

void SelectMachineDialog::prepare(int print_plate_idx)
{
    m_print_plate_idx = print_plate_idx;
}

void SelectMachineDialog::update_print_status_msg()
{
    bool is_ams_update =  m_statictext_ams_msg->UpdateInfos(m_pre_print_checker.filamentList);
    bool is_printer_update =  m_text_printer_msg->UpdateInfos(m_pre_print_checker.printerList);
    if (is_printer_update || is_ams_update) {
        Layout();
        Fit();
    }

 }

void SelectMachineDialog::update_print_error_info(int code, std::string msg, std::string extra)
{
    m_print_error_code  = code;
    m_print_error_msg   = msg;
    m_print_error_extra = extra;
}

bool SelectMachineDialog::check_sdcard_for_timelpase(MachineObject* obj)
{
    if (!obj) return false;

    // must set to a status if return true
    if (m_checkbox_list["timelapse"]->IsShown() && m_checkbox_list["timelapse"]->getValue() == "on")
    {
        if (obj->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::NO_SDCARD) {
            show_status(PrintDialogStatus::PrintStatusTimelapseNoSdcard);
            return true;
        }
    }

    return false;
}

void SelectMachineDialog::show_status(PrintDialogStatus status, std::vector<wxString> params, wxString wiki_url)
{
    wxString msg;
    wxString tips;

    if ( params.size() > 0 ) {
        msg = params[0];
    } else if (params.size() > 1) {
        tips = params[1];
    }

    if (m_print_status != status) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": update_status: " << status << "(" << PrePrintChecker::get_print_status_info(status) << ")";
    }
    m_print_status = status;

    // all message
    if (status == PrintDialogStatus::PrintStatusInit) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusNoUserLogin) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusReadyToGo) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(true);
    } else if (status == PrintDialogStatus::PrintStatusInvalidPrinter) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusConnectingServer) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusReading) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusReadingTimeout) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusModeNotFDM) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusInUpgrading) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusInSystemPrinting) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusInPrinting) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusAmsOnSettingup) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusAmsMappingInvalid) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusAmsMappingMixInvalid) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusNozzleDataInvalid) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusNozzleMatchInvalid) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintStatusNozzleDiameterMismatch) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintStatusNozzleTypeMismatch) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintStatusColorQuantityExceed) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    }

    else if (status == PrintDialogStatus::PrintStatusAmsMappingU0Invalid) {
        wxString msg_text;
        if (params.size() > 1)
            msg_text = wxString::Format(_L("Filament %s does not match the filament in AMS slot %s. Please update the printer firmware to support AMS slot assignment."), params[0], params[1]);
        else
            msg_text = _L("Filament does not match the filament in AMS slot. Please update the printer firmware to support AMS slot assignment.");

        msg = msg_text;
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusRefreshingMachineList) {
        Enable_Refresh_Button(false);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusSending) {
        Enable_Refresh_Button(false);
        Enable_Send_Button(false);
        sending_mode();
    } else if (status == PrintDialogStatus::PrintStatusLanModeNoSdcard) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusLanModeSDcardNotAvailable) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusNoSdcard) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    }else if (status == PrintDialogStatus::PrintStatusUnsupportedPrinter) {
        wxString msg_text;
        try
        {
            DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
            if (!dev) return;

            //source print
            MachineObject* obj_ = dev->get_selected_machine();
            if (obj_ == nullptr) return;
            auto sourcet_print_name = obj_->get_printer_type_display_str();
            sourcet_print_name.Replace(wxT("Bambu Lab "), wxEmptyString);

            //target print
            std::string target_model_id;
            if (m_print_type == PrintFromType::FROM_NORMAL){
                PresetBundle* preset_bundle = wxGetApp().preset_bundle;
                target_model_id = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);
            }
            else if (m_print_type == PrintFromType::FROM_SDCARD_VIEW) {
                if (m_required_data_plate_data_list.size() > 0) {
                    target_model_id = m_required_data_plate_data_list[m_print_plate_idx]->printer_model_id;
                }
            }

            auto target_print_name = wxString(DevPrinterConfigUtil::get_printer_display_name(target_model_id));
            target_print_name.Replace(wxT("Bambu Lab "), wxEmptyString);
            msg_text = wxString::Format(_L("The selected printer (%s) is incompatible with the print file configuration (%s). Please adjust the printer preset in the prepare page or choose a compatible printer on this page."), sourcet_print_name, target_print_name);


            msg = msg_text;
            Enable_Refresh_Button(true);
            Enable_Send_Button(false);
        }
        catch (...)
        {
            Enable_Refresh_Button(true);
            Enable_Send_Button(false);
        }



    } else if (status == PrintDialogStatus::PrintStatusTimelapseNoSdcard) {  //this warning
        Enable_Refresh_Button(true);
        Enable_Send_Button(true);
    } else if (status == PrintDialogStatus::PrintStatusNeedForceUpgrading) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusNeedConsistencyUpgrading) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusBlankPlate) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusNotSupportedPrintAll) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintDialogStatus::PrintStatusTimelapseWarning) {  //this is warning
        wxString   msg_text;
        PartPlate *plate = m_plater->get_partplate_list().get_curr_plate();
        for (auto warning : plate->get_slice_result()->warnings) {
            if (warning.msg == NOT_GENERATE_TIMELAPSE) {
                if (warning.error_code == "10014001") {
                    msg_text = _L("When enable spiral vase mode, machines with I3 structure will not generate timelapse videos.");
                }
                else if (warning.error_code == "10014002") {
                    msg_text = _L("The current printer does not support timelapse in Traditional Mode when printing By-Object.");
                }
            }
        }
        msg = msg_text;
        Enable_Refresh_Button(true);
        Enable_Send_Button(true);
    } else if (status == PrintStatusToolHeadCoolingFanWarning) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(true);
    } else if (status == PrintStatusMixAmsAndVtSlotWarning) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(true);
    } else if (status == PrintStatusTPUUnsupportAutoCali) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintStatusHasFilamentInBlackListError) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(false);
    } else if (status == PrintStatusWarningKvalueNotUsed) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(true);
    } else if (status == PrintStatusHasFilamentInBlackListWarning) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(true);
    } else if (status == PrintStatusFilamentWarningHighChamberTempCloseDoor || status == PrintStatusFilamentWarningHighChamberTemp) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(true);
    } else if (status == PrintDialogStatus::PrintStatusFilamentWarningHighChamberTempSoft || status == PrintDialogStatus::PrintStatusFilamentWarningUnknownHighChamberTempSoft) {
        Enable_Refresh_Button(true);
        Enable_Send_Button(true);
    }

    /*enter perpare mode*/
    prepare_mode(false);
    m_pre_print_checker.add(status, msg, tips, wiki_url);

}

void SelectMachineDialog::init_timer()
{
    m_refresh_timer = new wxTimer();
    m_refresh_timer->SetOwner(this);
}

void SelectMachineDialog::on_cancel(wxCloseEvent &event)
{
    if (m_mapping_popup.IsShown())
        m_mapping_popup.Dismiss();

    m_worker->cancel_all();
    this->EndModal(wxID_CANCEL);
}

bool SelectMachineDialog::is_blocking_printing(MachineObject* obj_)
{
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return true;
    auto target_model = obj_->printer_type;
    std::string source_model = "";

    if (m_print_type == PrintFromType::FROM_NORMAL) {
        PresetBundle* preset_bundle = wxGetApp().preset_bundle;
        source_model = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);


    }else if (m_print_type == PrintFromType::FROM_SDCARD_VIEW) {
        if (m_required_data_plate_data_list.size() > 0) {
            source_model = m_required_data_plate_data_list[m_print_plate_idx]->printer_model_id;
        }
    }

    if (source_model != target_model) {
        std::vector<std::string> compatible_machine = obj_->get_compatible_machine();
        vector<std::string>::iterator it = find(compatible_machine.begin(), compatible_machine.end(), source_model);
        if (it == compatible_machine.end()) {
            return true;
        }
    }

    return false;
}

static std::unordered_set<int> _get_used_nozzle_idxes()
{
    std::unordered_set<int> used_nozzle_idxes;

    DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (dev) {
        MachineObject *obj_ = dev->get_selected_machine();
        if (obj_) {
            try {
                PresetBundle *preset_bundle   = wxGetApp().preset_bundle;
                PartPlate *cur_plate          = wxGetApp().plater()->get_partplate_list().get_curr_plate();
                auto       used_filament_idxs = cur_plate->get_used_filaments(); /*the index is started from 1*/
                for (int used_filament_idx : used_filament_idxs) {
                    int used_nozzle_idx = cur_plate->get_physical_extruder_by_filament_id(preset_bundle->full_config(), used_filament_idx);
                    used_nozzle_idxes.insert(used_nozzle_idx);
                }
            } catch (const std::exception &) {}
        }
    }

    return used_nozzle_idxes;
}


static bool _is_nozzle_data_valid(MachineObject* obj_, const DevExtderSystem &ext_data)
{
    if (obj_ == nullptr) return false;

    PresetBundle *preset_bundle        = wxGetApp().preset_bundle;

    try {
        PartPlate *cur_plate          = wxGetApp().plater()->get_partplate_list().get_curr_plate();
        auto       used_filament_idxs = cur_plate->get_used_filaments(); /*the index is started from 1*/
        for (int used_filament_idx : used_filament_idxs)
        {
            int used_nozzle_idx = cur_plate->get_physical_extruder_by_filament_id(preset_bundle->full_config(), used_filament_idx);
            if (ext_data.GetNozzleType(used_nozzle_idx) == NozzleType::ntUndefine ||
                ext_data.GetNozzleDiameter(used_nozzle_idx) <= 0.0f ||
                ext_data.GetNozzleFlowType(used_nozzle_idx) == NozzleFlowType::NONE_FLOWTYPE) {
                return false;
            }
        }
    } catch (const std::exception &) {
        return false;
    }

    return true;
}


/**************************************************************//*
 * @param tag_nozzle_type -- return the mismatch nozzle type
 * @param tag_nozzle_diameter -- return the target nozzle_diameter but mismatch
 * @return is same or not
/*************************************************************/
static bool _is_same_nozzle_diameters(MachineObject* obj, float &tag_nozzle_diameter, int& mismatch_nozzle_id)
{
    if (obj == nullptr) return false;

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    auto opt_nozzle_diameters = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    if (!opt_nozzle_diameters)
    {
        return false;
    }

    try
    {
        PartPlate* cur_plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
        auto used_filament_idxs = cur_plate->get_used_filaments();/*the index is started from 1*/
        for (int used_filament_idx : used_filament_idxs)
        {
            int used_nozzle_idx = cur_plate->get_physical_extruder_by_filament_id(preset_bundle->full_config(), used_filament_idx);
            if (used_nozzle_idx == -1)
            {
                assert(0);
                return false;
            }

            tag_nozzle_diameter = float(opt_nozzle_diameters->get_at(used_nozzle_idx));
            if (tag_nozzle_diameter != obj->GetExtderSystem()->GetNozzleDiameter(used_nozzle_idx))
            {
                mismatch_nozzle_id = used_nozzle_idx;
                return false;
            }
        }
    }
    catch (const std::exception&)
    {
        return false;
    }

    return true;
}

bool SelectMachineDialog::is_nozzle_hrc_matched(const DevExtder* extruder, std::string& filament_type) const
{
    auto printer_nozzle_hrc = Print::get_hrc_by_nozzle_type(extruder->GetNozzleType());

    auto preset_bundle = wxGetApp().preset_bundle;
    MaterialHash::const_iterator iter = m_materialList.begin();
    while (iter != m_materialList.end()) {
        Material* item = iter->second;
        MaterialItem* m = item->item;
        auto filament_nozzle_hrc = preset_bundle->get_required_hrc_by_filament_type(m->m_material_name.ToStdString());

        if (abs(filament_nozzle_hrc) > abs(printer_nozzle_hrc)) {
            filament_type = m->m_material_name.ToStdString();
            BOOST_LOG_TRIVIAL(info) << "filaments hardness mismatch: filament = " << filament_type << " printer_nozzle_hrc = " << printer_nozzle_hrc;
            return false;
        }

        iter++;
    }

    return true;
}

bool SelectMachineDialog::is_same_printer_model()
{
    bool result = true;
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return result;

    MachineObject* obj_ = dev->get_selected_machine();

    assert(obj_->get_dev_id() == m_printer_last_select);
    if (obj_ == nullptr) {
        return result;
    }

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if(preset_bundle == nullptr) return result;
    const auto source_model = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);
    const auto target_model = obj_->printer_type;
    // Orca: ignore P1P -> P1S
    if (source_model != target_model) {
        if ((source_model == "C12" && target_model == "C11") || (source_model == "C11" && target_model == "C12") ||
            ((obj_->is_support_upgrade_kit && obj_->installed_upgrade_kit) && (source_model == "C12"))) {
            return true;
        }

        BOOST_LOG_TRIVIAL(info) << "printer_model: source = " << source_model;
        BOOST_LOG_TRIVIAL(info) << "printer_model: target = " << target_model;
        return false;
    }

    if (obj_->is_support_upgrade_kit && obj_->installed_upgrade_kit) {
        BOOST_LOG_TRIVIAL(info) << "printer_model: source = " << source_model;
        BOOST_LOG_TRIVIAL(info) << "printer_model: target = " << obj_->printer_type << " (plus)";
        return false;
    }
    return true;
}

void SelectMachineDialog::show_errors(wxString &info)
{
    ConfirmBeforeSendDialog confirm_dlg(this, wxID_ANY, _L("Errors"));
    confirm_dlg.update_text(info);
    confirm_dlg.on_show();
}

void SelectMachineDialog::on_ok_btn(wxCommandEvent &event)
{
    bool has_slice_warnings = false;
    bool is_printing_block  = false;

    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;
    MachineObject* obj_ = dev->get_selected_machine();
    if (!obj_) return;

    std::vector<ConfirmBeforeSendInfo> confirm_text;

    // check more than one using in same external spool
    std::unordered_set<string> main_external_spool_filas;
    std::unordered_set<string> deputy_external_spool_filas;
    for (const auto& mapping_info : m_ams_mapping_result) {
        if (mapping_info.ams_id == VIRTUAL_AMS_MAIN_ID_STR){
            main_external_spool_filas.insert(mapping_info.filament_id);
        } else if (mapping_info.ams_id == VIRTUAL_AMS_DEPUTY_ID_STR) {
            deputy_external_spool_filas.insert(mapping_info.filament_id);
        }
    }

    if (main_external_spool_filas.size() > 1 || deputy_external_spool_filas.size() > 1) {
        if(!m_ext_change_assist ){
            confirm_text.push_back(ConfirmBeforeSendInfo(_L("More than one filament types have been mapped to the same external spool, which may cause printing issues. The printer won't pause during printing.")));
            has_slice_warnings = true;
        }
    } else {
        //check filaments type in external spool
        for (const auto& mapping_info : m_ams_mapping_result) {
            if (mapping_info.ams_id != VIRTUAL_AMS_MAIN_ID_STR && mapping_info.ams_id != VIRTUAL_AMS_DEPUTY_ID_STR) {
                continue;
            }

            const auto& mapped_fila_type = obj_->get_filament_type(mapping_info.ams_id, mapping_info.slot_id);
            if (!mapped_fila_type.empty() && (mapped_fila_type != mapping_info.type)) {
                confirm_text.push_back(ConfirmBeforeSendInfo(_L("The filament type setting of external spool is different from the filament in the slicing file.")));
                has_slice_warnings = true;
                break;
            }
        }
    }

    //Check Printer Model Id
    bool is_same_printer_type = is_same_printer_model();
    if (!is_same_printer_type && (m_print_type == PrintFromType::FROM_NORMAL)) {
        confirm_text.push_back(ConfirmBeforeSendInfo(_L("The printer type selected when generating G-code is not consistent with the currently selected printer. It is recommended that you use the same printer type for slicing.")));
        has_slice_warnings = true;
    }

    //check blacklist
    for (auto i = 0; i < m_ams_mapping_result.size(); i++) {

        const auto& ams_id = m_ams_mapping_result[i].get_ams_id();
        const auto& slot_id = m_ams_mapping_result[i].get_slot_id();

        auto tid = m_ams_mapping_result[i].tray_id;

        std::string filament_type = boost::to_upper_copy(m_ams_mapping_result[i].type);
        std::string filament_brand;

        for (auto fs : m_filaments) {
            if (fs.id == m_ams_mapping_result[i].id) {
                filament_brand = m_filaments[i].brand;
            }
        }

        bool in_blacklist = false;
        std::string action;
        wxString info;
        wxString wiki_url;
        DevFilaBlacklist::check_filaments_in_blacklist_url(obj_->printer_type, filament_brand, filament_type, m_ams_mapping_result[i].filament_id, ams_id, slot_id, "", in_blacklist,
                                                        action, info, wiki_url);
        if (in_blacklist && action == "warning") {
            confirm_text.push_back(ConfirmBeforeSendInfo(info, wiki_url));
            has_slice_warnings = true;
        }
    }

    PartPlate* plate = m_plater->get_partplate_list().get_curr_plate();

    bool has_show_traditional_timelapse_waring = false;
    for (auto warning : plate->get_slice_result()->warnings) {
        if (warning.msg == NOT_SUPPORT_TRADITIONAL_TIMELAPSE) {
            if (!has_show_traditional_timelapse_waring && (m_checkbox_list["timelapse"]->getValue() == "on")) {
                confirm_text.push_back(ConfirmBeforeSendInfo(Plater::get_slice_warning_string(warning)));
                has_show_traditional_timelapse_waring = true;
                has_slice_warnings = true;
            }
        }
        else if (warning.msg == SMOOTH_TIMELAPSE_WITHOUT_PRIME_TOWER) {
            if (m_checkbox_list["timelapse"]->getValue() == "on") {
                confirm_text.push_back(ConfirmBeforeSendInfo(Plater::get_slice_warning_string(warning)));
                has_slice_warnings = true;
            }
        }
        else if (warning.msg == NOT_GENERATE_TIMELAPSE) {
            continue;
        }
        else if(warning.msg == NOZZLE_HRC_CHECKER){
            wxString error_info = Plater::get_slice_warning_string(warning);
            if (error_info.IsEmpty()) {
                error_info = wxString::Format("%s\n", warning.msg);
            }

            confirm_text.push_back(ConfirmBeforeSendInfo(error_info));
            has_slice_warnings = true;
        }
    }


    //check for unidentified material
    auto mapping_result = m_mapping_popup.parse_ams_mapping(obj_->GetFilaSystem()->GetAmsList());
    auto has_unknown_filament = false;

    for (auto i = 0; i < m_ams_mapping_result.size(); i++) {

        const auto& ams_id = m_ams_mapping_result[i].get_ams_id();
        const auto& slot_id = m_ams_mapping_result[i].get_slot_id();

        auto tid = m_ams_mapping_result[i].tray_id;

        std::string filament_type = boost::to_upper_copy(m_ams_mapping_result[i].type);
        std::string filament_brand;
        std::string filament_id;

        for (auto fs : m_filaments) {
            if (fs.id == m_ams_mapping_result[i].id) {
                filament_brand = m_filaments[i].brand;
                filament_id = m_filaments[i].filament_id;
            }
        }

        for (auto miter : mapping_result) {
            //matching
            if (miter.id == tid) {
                if (miter.type == TrayType::THIRD || miter.type == TrayType::EMPTY) {
                    has_unknown_filament = true;
                    break;
                }
            }
        }
    }

    if (has_unknown_filament) {
        has_slice_warnings = true;
        confirm_text.push_back(ConfirmBeforeSendInfo(_L("There are some unknown filaments in the AMS mappings. Please check whether they are the required filaments. If they are okay, press \"Confirm\" to start printing.")));
    }

    if (has_slice_warnings)
    {
        ConfirmBeforeSendDialog confirm_dlg(this, wxID_ANY, _L("Warning"));
        if (is_printing_block)
        {
            confirm_dlg.hide_button_ok();
            confirm_dlg.edit_cancel_button_txt(_L("Close"), true);
        }
        confirm_dlg.Bind(EVT_SECONDARY_CHECK_CONFIRM, [this, &confirm_dlg](wxCommandEvent& e)
            {
                confirm_dlg.on_hide();
                this->on_send_print();
            });

        // STUDIO-9580
        /* use warning color if there are warning and normal messages* /
        /* use indexes if there are several messages*/
        /* add header and ending if there are several messages or has none block warnings*/
        if (confirm_text.size() > 1 || !is_printing_block)
        {
            bool has_warning_msg = false;
            bool has_normal_msg = false;

            if (confirm_text.size() > 1) {
                  std::unordered_set<wxString>       seen_texts;
                  std::vector<ConfirmBeforeSendInfo> unique_texts;

                  for (const auto &item : confirm_text) {
                      if (seen_texts.insert(item.text).second)
                      {
                          unique_texts.push_back(item);
                      }
                  }

                  for (size_t i = 0; i < unique_texts.size(); ++i) {
                      unique_texts[i].text = wxString::Format("%d. %s", i + 1, unique_texts[i].text);
                      if (unique_texts[i].level == ConfirmBeforeSendInfo::InfoLevel::Warning)
                          has_warning_msg = true;
                      else
                          has_normal_msg = true;
                  }
                  confirm_text.swap(unique_texts);
            }

            std::vector<ConfirmBeforeSendInfo> shown_infos;
            shown_infos.emplace_back(ConfirmBeforeSendInfo(_L("Please check the following:")));
            for (const auto& item : confirm_text)
            {
                shown_infos.emplace_back(item);
            }

            if (is_printing_block)
            {
                shown_infos.emplace_back(ConfirmBeforeSendInfo(_L("Please fix the error above, otherwise printing cannot continue."), wxEmptyString, ConfirmBeforeSendInfo::InfoLevel::Warning));
            }
            else
            {
                shown_infos.emplace_back(ConfirmBeforeSendInfo(_L("Please click the confirm button if you still want to proceed with printing.")));
            }

            confirm_dlg.update_text(shown_infos, has_warning_msg&& has_normal_msg);
        }
        else
        {
            confirm_dlg.update_text(confirm_text, false);
        }

        confirm_dlg.on_show();
    }
    else
    {
        this->on_send_print();
    }
}

wxString SelectMachineDialog::format_steel_name(NozzleType type)
{
    if (type == NozzleType::ntHardenedSteel) {
        return _L("Hardened Steel");
    }
    else if (type == NozzleType::ntStainlessSteel) {
        return _L("Stainless Steel");
    }
    else if (type == NozzleType::ntTungstenCarbide) {
        return _L("Tungsten Carbide");
    }

    return _L("Unknown");
}

void SelectMachineDialog::EnableEditing(bool enable)
{
    /*project*/
    m_rename_button->Enable(enable);

    /*printer*/
    m_printer_box->EnableEditing(enable);

    /*mapping*/
    for (size_t i = 0; i < m_sizer_ams_mapping_left->GetItemCount(); ++i) {
        wxSizerItem *item = m_sizer_ams_mapping_left->GetItem(i);
        if (item) {item->GetWindow()->Enable(enable);}
    }

    for (size_t i = 0; i < m_sizer_ams_mapping_right->GetItemCount(); ++i) {
        wxSizerItem *item = m_sizer_ams_mapping_right->GetItem(i);
        if (item) {item->GetWindow()->Enable(enable);}
    }

    for (size_t i = 0; i < m_sizer_ams_mapping->GetItemCount(); ++i) {
        wxSizerItem *item = m_sizer_ams_mapping->GetItem(i);
        if (item) { item->GetWindow()->Enable(enable); }
    }

    /*mapping link*/
    //m_link_edit_nozzle->Enable(enable);

    /*options*/
    for (auto iter : m_checkbox_list)
    {
        iter.second->enable(enable);
    }
}

/*content height > FromDIP(650), make the area scrollable*/
/*content height < FromDIP(650), make the area size as the content*/
void SelectMachineDialog::update_scroll_area_size() {

    wxSize new_size(FromDIP(700), -1);

    int height = m_scroll_area->GetSizer()->CalcMin().GetHeight();
    if (height < FromDIP(650)) {
        new_size.SetHeight(height);
    } else {
        new_size.SetHeight(FromDIP(650));
    }

    if (m_scroll_area->GetSize() != new_size) {
        m_scroll_area->SetMaxSize(new_size);
        m_scroll_area->SetMinSize(new_size);
        Layout();
        Fit();
    }
}

void SelectMachineDialog::update_options_layout()
{
    std::unordered_set<wxWindow*> shown_options;
    const wxSizerItemList& children = m_sizer_options->GetChildren();
    for (auto item : children) {
        if (item && item->IsWindow()) { shown_options.insert(item->GetWindow()); }
    }

    std::unordered_set<wxWindow *> toshow_options;
    for (auto option : m_checkbox_list_order) {
        if (option->IsShown()) { toshow_options.insert(option); }
    }

    /*To show changed, update layout*/
    if (shown_options != toshow_options) {
        m_sizer_options->Clear();
        for (auto option : m_checkbox_list_order) {
            if (option->IsShown()) { m_sizer_options->Add(option, 0, wxEXPAND); }
        }
    }
}

void SelectMachineDialog::update_option_opts(MachineObject *obj)
{
    if (!obj)
    {
        for (auto opt : m_checkbox_list) { opt.second->Hide(); }
        update_options_layout();
        return;
    }

    /*timelapse*/
    m_checkbox_list["timelapse"]->Show();

    /*bed_leveling*/
    if (obj->is_support_bed_leveling == 2) {
        m_checkbox_list["bed_leveling"]->Show();
        m_checkbox_list["bed_leveling"]->update_options(ops_auto, _L("This checks the flatness of heatbed. Leveling makes extruded height uniform.\n*Automatic mode: Run a "
                                                                     "leveling check(about 10 seconds). Skip if surface is fine."));
    } else if (obj->is_support_bed_leveling == 1) {
        m_checkbox_list["bed_leveling"]->Show();
        m_checkbox_list["bed_leveling"]->update_options(ops_no_auto, _L("This checks the flatness of heatbed. Leveling makes extruded height uniform."));
    } else {
        m_checkbox_list["bed_leveling"]->Hide();
    }

    /*nozzle_offset_cali*/
    bool support_cali_nozzle_offset = obj->GetConfig()->SupportCalibrationNozzleOffset();
    if (support_cali_nozzle_offset) {
        m_checkbox_list["nozzle_offset_cali"]->update_options(ops_auto, _L("Calibrate nozzle offsets to enhance print quality.\n*Automatic mode: Check for calibration before printing. Skip if unnecessary."));
    }
    m_checkbox_list["nozzle_offset_cali"]->Show(support_cali_nozzle_offset);

    /*flow_cali*/
    if (obj->is_support_pa_calibration) {
        if (obj->GetConfig()->SupportCalibrationPA_FlowAuto() && can_support_pa_auto_cali())
        {
            m_checkbox_list["flow_cali"]->update_options(ops_auto, _L("This process determines the dynamic flow values to improve overall print quality.\n*Automatic mode: Skip if the filament was calibrated recently."));
        } else {
            m_checkbox_list["flow_cali"]->update_options(ops_no_auto, _L("This process determines the dynamic flow values to improve overall print quality."));
        }
    }
    m_checkbox_list["flow_cali"]->Show(obj->is_support_pa_calibration);

    update_options_layout();
}

static bool _HasExt(const std::vector<FilamentInfo> &ams_mapping_result) {
    if (ams_mapping_result.empty()) {
        return true;
    };

    for (const auto &info : ams_mapping_result) {
        if (info.ams_id == VIRTUAL_AMS_MAIN_ID_STR || info.ams_id == VIRTUAL_AMS_DEPUTY_ID_STR && !info.ams_id.empty()) {
            return true;
        }
    }

    return false;
}

static bool _HasAms(const std::vector<FilamentInfo> &ams_mapping_result) {
    for (const auto &info : ams_mapping_result) {
        if (info.ams_id != VIRTUAL_AMS_MAIN_ID_STR && info.ams_id != VIRTUAL_AMS_DEPUTY_ID_STR && !info.ams_id.empty()) {
            return true;
        }
    }

    return false;
}

bool SelectMachineDialog::is_enable_external_change_assist(std::vector<FilamentInfo>& ams_mapping_result) {
    // if (_HasAms(ams_mapping_result)) return false;

    std::map<std::string, int> v_ams_map;
    v_ams_map[VIRTUAL_AMS_MAIN_ID_STR] = 0;
    v_ams_map[VIRTUAL_AMS_DEPUTY_ID_STR] = 0;
    for(auto info : m_ams_mapping_result){
        v_ams_map[info.ams_id]++;
    }
    return (v_ams_map[VIRTUAL_AMS_MAIN_ID_STR] > 1) || (v_ams_map[VIRTUAL_AMS_DEPUTY_ID_STR] > 1);
}

void SelectMachineDialog::load_option_vals(MachineObject *obj)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " start"
                            << ", dev_id = " << (obj ? obj->get_dev_id() : "NULL")
                            << ", sending mode= " << m_is_in_sending_mode;

    if (m_is_in_sending_mode) { return;}

    AppConfig* config = wxGetApp().app_config;
    if (!config) return;
    if (!obj) return;

    for (auto item : m_checkbox_list) {
        PrintOption       *opt = item.second;
        const std::string &val = config->get(obj->printer_type, item.first);
        if (opt->contain_opt(val)) {
            opt->setValue(val);
        } else if (opt->contain_opt("auto")) {
            opt->setValue("auto");
        } else if (opt->contain_opt("on")) {
            opt->setValue("on");
        }
    }

    /*STUDIO-9197*/
    wxString error_messgae;
    if (obj->canEnableTimelapse(error_messgae) && !has_timelapse_warning(error_messgae)) {
        m_checkbox_list["timelapse"]->enable(true);
        m_checkbox_list["timelapse"]->update_tooltip(wxEmptyString);
    } else {
        m_checkbox_list["timelapse"]->enable(false);
        m_checkbox_list["timelapse"]->setValue("off");
        m_checkbox_list["timelapse"]->update_tooltip(error_messgae);
    }
}

void SelectMachineDialog::save_option_vals()
{
    DeviceManager* dev_manager = wxGetApp().getDeviceManager();
    if (!dev_manager) { return; }

    MachineObject* obj = dev_manager->get_my_machine(m_printer_last_select);
    if (obj) save_option_vals(obj);
}

void SelectMachineDialog::save_option_vals(MachineObject *obj) {
    AppConfig *config = wxGetApp().app_config;
    if (!config) return;
    if (!obj) return;
    for (auto item : m_checkbox_list) {
        config->set(obj->printer_type, item.first, item.second->getValue());
    }
}

void SelectMachineDialog::Enable_Auto_Refill(bool enable)
{
    if (enable) {
        m_ams_backup_tip->SetForegroundColour(wxColour("#009688"));
    }
    else {
        m_ams_backup_tip->SetForegroundColour(wxColour(0x90, 0x90, 0x90));
    }
    m_ams_backup_tip->Refresh();
}

void SelectMachineDialog::on_send_print()
{
    BOOST_LOG_TRIVIAL(info) << "print_job: on_ok to send";
    m_is_canceled = false;
    Enable_Send_Button(false);

    if (m_mapping_popup.IsShown())
        m_mapping_popup.Dismiss();

    if (m_print_type == PrintFromType::FROM_NORMAL && m_is_in_sending_mode)
        return;

    int result = 0;
    if (m_printer_last_select.empty()) {
        return;
    }

    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;

    MachineObject* obj_ = dev->get_selected_machine();
    assert(obj_->get_dev_id() == m_printer_last_select);
    if (obj_ == nullptr) { return; }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", print_job: for send task, current printer id =  " << m_printer_last_select << std::endl;
    show_status(PrintDialogStatus::PrintStatusSending);

    m_status_bar->reset();
    m_status_bar->set_prog_block();
    m_status_bar->set_cancel_callback_fina([this]() {
        BOOST_LOG_TRIVIAL(info) << "print_job: enter canceled";
        m_worker->cancel_all();
        m_is_canceled = true;
        wxCommandEvent* event = new wxCommandEvent(EVT_PRINT_JOB_CANCEL);
        wxQueueEvent(this, event);
        });

    if (m_is_canceled) {
        BOOST_LOG_TRIVIAL(info) << "print_job: m_is_canceled";
        m_status_bar->set_status_text(task_canceled_text);
        return;
    }

    // enter sending mode
    sending_mode();
    m_status_bar->enable_cancel_button();

    // get ams_mapping_result
    std::string ams_mapping_array;
    std::string ams_mapping_array2;
    std::string ams_mapping_info;

    get_ams_mapping_result(ams_mapping_array,ams_mapping_array2, ams_mapping_info);

    if (m_print_type == PrintFromType::FROM_NORMAL) {
        result = m_plater->send_gcode(m_print_plate_idx, [this](int export_stage, int current, int total, bool& cancel) {
            if (this->m_is_canceled) return;
            bool     cancelled = false;
            wxString msg = _L("Preparing print job");
            m_status_bar->update_status(msg, cancelled, 10, true);
            m_export_3mf_cancel = cancel = cancelled;
            });

        if (m_is_canceled || m_export_3mf_cancel) {
            BOOST_LOG_TRIVIAL(info) << "print_job: m_export_3mf_cancel or m_is_canceled";
            m_status_bar->set_status_text(task_canceled_text);
            return;
        }

        if (result < 0) {
            wxString msg = _L("Abnormal print file data. Please slice again");
            m_status_bar->set_status_text(msg);
            return;
        }

        // export config 3mf if needed
        if (!obj_->is_lan_mode_printer()) {
            result = m_plater->export_config_3mf(m_print_plate_idx);
            if (result < 0) {
                BOOST_LOG_TRIVIAL(info) << "export_config_3mf failed, result = " << result;
                return;
            }
        }
        if (m_is_canceled || m_export_3mf_cancel) {
            BOOST_LOG_TRIVIAL(info) << "print_job: m_export_3mf_cancel or m_is_canceled";
            m_status_bar->set_status_text(task_canceled_text);
            return;
        }
    }

    m_print_job = std::make_shared<PrintJob>(m_printer_last_select);
    m_print_job->m_dev_ip = obj_->get_dev_ip();
    m_print_job->m_ftp_folder = obj_->get_ftp_folder();
    m_print_job->m_access_code = obj_->get_access_code();
#if !BBL_RELEASE_TO_PUBLIC
    m_print_job->m_local_use_ssl_for_ftp = wxGetApp().app_config->get("enable_ssl_for_ftp") == "true" ? true : false;
    m_print_job->m_local_use_ssl_for_mqtt = wxGetApp().app_config->get("enable_ssl_for_mqtt") == "true" ? true : false;
#else
    m_print_job->m_local_use_ssl_for_ftp = obj_->local_use_ssl_for_ftp;
    m_print_job->m_local_use_ssl_for_mqtt = obj_->local_use_ssl_for_mqtt;
#endif
    m_print_job->connection_type = obj_->connection_type();
    m_print_job->cloud_print_only = obj_->is_support_cloud_print_only;

    if (m_print_type == PrintFromType::FROM_NORMAL) {
        BOOST_LOG_TRIVIAL(info) << "print_job: m_print_type = from_normal";
        m_print_job->m_print_type = "from_normal";
        m_print_job->set_project_name(m_current_project_name.utf8_string());
    }
    else if(m_print_type == PrintFromType::FROM_SDCARD_VIEW){
        BOOST_LOG_TRIVIAL(info) << "print_job: m_print_type = from_sdcard_view";
        m_print_job->m_print_type = "from_sdcard_view";
        //m_print_job->connection_type = "lan";

        try {
            m_print_job->m_print_from_sdc_plate_idx = m_required_data_plate_data_list[m_print_plate_idx]->plate_index + 1;
            m_print_job->set_dst_name(m_required_data_file_path);
        }
        catch (...) {}
        BOOST_LOG_TRIVIAL(info) << "print_job: m_print_plate_idx =" << m_print_job->m_print_from_sdc_plate_idx;

        auto input_str_arr = wxGetApp().split_str(m_required_data_file_name, ".gcode.3mf");
        if (input_str_arr.size() <= 1) {
            input_str_arr = wxGetApp().split_str(m_required_data_file_name, ".3mf");
            if (input_str_arr.size() > 1) {
                m_print_job->set_project_name(input_str_arr[0]);
            }
        }
        else {
            m_print_job->set_project_name(input_str_arr[0]);
        }
    }

    m_print_job->task_ams_mapping      = ams_mapping_array;
    m_print_job->task_ams_mapping2     = ams_mapping_array2;
    m_print_job->task_ams_mapping_info = ams_mapping_info;

    /* build nozzles info for multi extruders printers */
    if (build_nozzles_info(m_print_job->task_nozzles_info)) {
        BOOST_LOG_TRIVIAL(error) << "build_nozzle_info errors";
    }

    m_print_job->sdcard_state = obj_->GetStorage()->get_sdcard_state();    
    m_print_job->has_sdcard =  wxGetApp().app_config->get("allow_abnormal_storage") == "true"
            ? (m_print_job->sdcard_state == DevStorage::SdcardState::HAS_SDCARD_NORMAL
               || m_print_job->sdcard_state == DevStorage::SdcardState::HAS_SDCARD_ABNORMAL)
            : m_print_job->sdcard_state == DevStorage::SdcardState::HAS_SDCARD_NORMAL;

    m_print_job->could_emmc_print = obj_->is_support_print_with_emmc;


    bool timelapse_option = m_checkbox_list["timelapse"]->IsShown()?true:false;
    if (timelapse_option) {
        timelapse_option = m_checkbox_list["timelapse"]->getValue() == "on";
    }

    m_print_job->set_print_config(
        MachineBedTypeString[0],
        (m_checkbox_list["bed_leveling"]->getValue() == "on"),
        (m_checkbox_list["flow_cali"]->getValue() == "on"),
        false,
        timelapse_option,
        true,
        m_ext_change_assist,
        m_checkbox_list["bed_leveling"]->getValueInt(),
        m_checkbox_list["flow_cali"]->getValueInt(),
        m_checkbox_list["nozzle_offset_cali"]->getValueInt()
    );

    if (obj_->HasAms()) {
        bool has_ext = _HasExt(m_ams_mapping_result);
        bool has_ams = _HasAms(m_ams_mapping_result);

        if (has_ams && !has_ext) {
            m_print_job->task_use_ams = true;
        }
        else if (has_ext && !has_ams) {
            m_print_job->task_use_ams = false;
        }
        else {
            m_print_job->task_use_ams = true;
        }
    } else {
        m_print_job->task_use_ams = false;
    }

    BOOST_LOG_TRIVIAL(info) << "print_job: timelapse_option = " << timelapse_option;
    BOOST_LOG_TRIVIAL(info) << "print_job: use_ams = " << m_print_job->task_use_ams;

    m_print_job->on_success([this]() { finish_mode(); });

    m_print_job->on_check_ip_address_fail([this]() {
        wxCommandEvent* evt = new wxCommandEvent(EVT_CLEAR_IPADDRESS);
        wxQueueEvent(this, evt);
        wxGetApp().show_ip_address_enter_dialog();
     });

    // update ota version
    NetworkAgent* agent = wxGetApp().getAgent();
    if (agent) {
        std::string dev_ota_str = "dev_ota_ver:" + obj_->get_dev_id();
        agent->track_update_property(dev_ota_str, obj_->get_ota_version());
    }

    replace_job(*m_worker, m_print_job);
    BOOST_LOG_TRIVIAL(info) << "print_job: start print job";
}

void SelectMachineDialog::clear_ip_address_config(wxCommandEvent& e)
{
    prepare_mode();
}

void SelectMachineDialog::update_user_machine_list()
{
    NetworkAgent* m_agent = wxGetApp().getAgent();
    if (m_agent && m_agent->is_user_login()) {
        boost::thread get_print_info_thread = Slic3r::create_thread([this, token = std::weak_ptr<int>(m_token)] {
            NetworkAgent* agent = wxGetApp().getAgent();
            unsigned int http_code;
            std::string body;
            int result = agent->get_user_print_info(&http_code, &body);
            CallAfter([token, this, result, body] {
                if (token.expired()) {return;}
                if (result == 0) {
                    m_print_info = body;
                }
                else {
                    m_print_info = "";
                }
                wxCommandEvent event(EVT_UPDATE_USER_MACHINE_LIST);
                event.SetEventObject(this);
                wxPostEvent(this, event);
            });
        });
    } else {
        wxCommandEvent event(EVT_UPDATE_USER_MACHINE_LIST);
        event.SetEventObject(this);
        wxPostEvent(this, event);
    }
}

void SelectMachineDialog::on_refresh(wxCommandEvent &event)
{
    BOOST_LOG_TRIVIAL(info) << "m_printer_last_select: on_refresh";
    show_status(PrintDialogStatus::PrintStatusRefreshingMachineList);
    update_user_machine_list();
}

void SelectMachineDialog::on_set_finish_mapping(wxCommandEvent &evt)
{
    auto selection_data = evt.GetString();
    auto selection_data_arr = wxSplit(selection_data.ToStdString(), '|');

    BOOST_LOG_TRIVIAL(info) << "The ams mapping selection result: data is " << selection_data;

    if (selection_data_arr.size() == 8) {
        auto ams_colour      = wxColour(wxAtoi(selection_data_arr[0]), wxAtoi(selection_data_arr[1]), wxAtoi(selection_data_arr[2]), wxAtoi(selection_data_arr[3]));
        int  old_filament_id = (int) wxAtoi(selection_data_arr[5]);
        if (m_print_type == PrintFromType::FROM_NORMAL) {//todo:support sd card
            change_default_normal(old_filament_id, ams_colour);
            final_deal_edge_pixels_data(m_preview_thumbnail_data);
            set_default_normal(m_preview_thumbnail_data); // do't reset ams
        }

        int ctype = 0;
        std::vector<wxColour> material_cols;
        std::vector<std::string> tray_cols;
        for (auto mapping_item : m_mapping_popup.m_mapping_item_list) {
            if (mapping_item->m_tray_data.id == evt.GetInt()) {
                ctype = mapping_item->m_tray_data.ctype;
                material_cols = mapping_item->m_tray_data.material_cols;
                for (auto col : mapping_item->m_tray_data.material_cols) {
                    wxString color = wxString::Format("#%02X%02X%02X%02X", col.Red(), col.Green(), col.Blue(), col.Alpha());
                    tray_cols.push_back(color.ToStdString());
                }
                break;
            }
        }


        for (auto i = 0; i < m_ams_mapping_result.size(); i++) {
            if (m_ams_mapping_result[i].id == wxAtoi(selection_data_arr[5])) {
                m_ams_mapping_result[i].tray_id = evt.GetInt();
                auto ams_colour = wxColour(wxAtoi(selection_data_arr[0]), wxAtoi(selection_data_arr[1]), wxAtoi(selection_data_arr[2]), wxAtoi(selection_data_arr[3]));
                wxString color = wxString::Format("#%02X%02X%02X%02X", ams_colour.Red(), ams_colour.Green(), ams_colour.Blue(), ams_colour.Alpha());
                m_ams_mapping_result[i].color = color.ToStdString();
                m_ams_mapping_result[i].ctype = ctype;
                m_ams_mapping_result[i].colors = tray_cols;

                m_ams_mapping_result[i].ams_id = selection_data_arr[6].ToStdString();
                m_ams_mapping_result[i].slot_id = selection_data_arr[7].ToStdString();

                if (m_ams_mapping_result[i].ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID) || m_ams_mapping_result[i].ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
                }else if (m_ams_mapping_result[i].ams_id >= std::to_string(0)) {
                }
            }
            BOOST_LOG_TRIVIAL(trace) << "The ams mapping result: id is " << m_ams_mapping_result[i].id << "tray_id is " << m_ams_mapping_result[i].tray_id;
        }

        MaterialHash::iterator iter = m_materialList.begin();
        while (iter != m_materialList.end()) {
            Material*        item = iter->second;
            MaterialItem *m = item->item;
            if (item->id == m_current_filament_id) {
                auto ams_colour = wxColour(wxAtoi(selection_data_arr[0]), wxAtoi(selection_data_arr[1]), wxAtoi(selection_data_arr[2]), wxAtoi(selection_data_arr[3]));
                m->set_ams_info(ams_colour, selection_data_arr[4], ctype, material_cols);
            }
            iter++;
        }
    }

    update_filament_change_count();
}

void SelectMachineDialog::on_print_job_cancel(wxCommandEvent &evt)
{
    BOOST_LOG_TRIVIAL(info) << "print_job: canceled";

    EnableEditing(true);
    show_status(PrintDialogStatus::PrintStatusInit);
    // enter prepare mode
    prepare_mode();
}

bool  SelectMachineDialog::is_timeout()
{
    if (m_timeout_count > 15 * 1000 / LIST_REFRESH_INTERVAL) {
        return true;
    }
    return false;
}

int SelectMachineDialog::update_print_required_data(Slic3r::DynamicPrintConfig config, Slic3r::Model model, Slic3r::PlateDataPtrs plate_data_list, std::string file_name, std::string file_path)
{
    m_required_data_plate_data_list.clear();
    m_required_data_config = config;
    m_required_data_model = model;
    //m_required_data_plate_data_list = plate_data_list;
    for (auto i = 0; i < plate_data_list.size(); i++) {
        if (!plate_data_list[i]->gcode_file.empty()) {
            m_required_data_plate_data_list.push_back(plate_data_list[i]);
        }
    }

    m_required_data_file_name = file_name;
    m_required_data_file_path = file_path;
    return m_required_data_plate_data_list.size();
}

void  SelectMachineDialog::reset_timeout()
{
    m_timeout_count = 0;
}


static bool
_compare_obj_names(MachineObject* obj1, MachineObject* obj2)
{
    return obj1->get_dev_name() < obj2->get_dev_name();
}

/*******************************************************************
*@note   _collect_machine_list
*@param  dev_manager -- the device manager
*@param  sorted_machine_objs -- return the sorted machine objects
*@param  best_one -- return the best one
*/
/*******************************************************************/
static void
_collect_sorted_machines(Slic3r::DeviceManager* dev_manager,
                         std::vector<MachineObject*>& sorted_machine_objs)
{
    sorted_machine_objs.clear();
    if (!dev_manager)
    {
        return;
    }

    /* Step 1 :Collect the target and compatible types*/
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    const std::string& printer_type = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);
    const auto& compatible_types_list = DevPrinterConfigUtil::get_compatible_machine(printer_type);
    std::set<std::string> compatible_types_set(compatible_types_list.begin(), compatible_types_list.end());

    /* Step 2: collect different machine list*/
    MachineObject* cur_selected_obj = dev_manager->get_selected_machine();
    std::vector<MachineObject*> match_avaliable_list;  // match and availiable machines
    std::vector<MachineObject*> match_inavaliable_list;// match but inavaliable machines
    std::vector<MachineObject*> other_list;// other bound machines
    auto _collect_machine = [&](MachineObject* obj)
    {
         if (obj == cur_selected_obj)
         {
             return;
         }

        if (obj->printer_type == printer_type)
        {
            obj->is_avaliable() ? match_avaliable_list.push_back(obj) : match_inavaliable_list.push_back(obj);
        }
        else
        {
            other_list.push_back(obj);
        }
    };

    // collect from user machine list
    const auto& user_machine_list = dev_manager->get_my_machine_list();// user machine list
    for (const auto& elem : user_machine_list)
    {
        MachineObject* mobj = elem.second;
        if (mobj && (mobj->is_online() || mobj->is_connected()))
        {
            _collect_machine(mobj);
        }
    }

    /* Step 2: Sort the lists*/
    std::sort(match_avaliable_list.begin(), match_avaliable_list.end(), _compare_obj_names);
    std::sort(match_inavaliable_list.begin(), match_inavaliable_list.end(), _compare_obj_names);
    std::sort(other_list.begin(), other_list.end(), _compare_obj_names);

    /* Step 3: Get the sorted objects*/
    auto _collect_sorted_objs = [](const std::vector<MachineObject*>& obj_list,
                                   std::vector<MachineObject*>& sorted_machine_objs)
    {
        for (auto obj : obj_list)
        {
            sorted_machine_objs.push_back(obj);
        }
    };

    // the shown list, STUDIO-8235
    if (cur_selected_obj)
    {
        std::vector<MachineObject*> cur_selected_obj_list{ cur_selected_obj };
        _collect_sorted_objs(cur_selected_obj_list, sorted_machine_objs);
    }
    _collect_sorted_objs(match_avaliable_list, sorted_machine_objs);
    _collect_sorted_objs(match_inavaliable_list, sorted_machine_objs);
    _collect_sorted_objs(other_list, sorted_machine_objs);
}

void SelectMachineDialog::update_user_printer()
{
    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;

    // update user print info
    if (!m_print_info.empty()) {
        dev->parse_user_print_info(m_print_info);
        m_print_info = "";
    }

    // update the machine list, and select a default machine
    _collect_sorted_machines(dev, m_list);
    m_printer_box->SetPrinters(m_list);
    if (!m_list.empty())
    {
        m_printer_last_select = m_list.front()->get_dev_id();
        m_printer_box->GetPrinterComboBox()->SetSelection(0);
        wxCommandEvent event(wxEVT_COMBOBOX);
        event.SetEventObject(m_printer_box->GetPrinterComboBox());
        wxPostEvent(m_printer_box->GetPrinterComboBox(), event);
    }
    else
    {
        m_printer_last_select = "";
        update_select_layout(nullptr);
        m_printer_box->GetPrinterComboBox()->SetTextLabel("");
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "for send task, current printer id =  " << m_printer_last_select << std::endl;
}

void SelectMachineDialog::on_rename_click(wxMouseEvent& event)
{
    m_is_rename_mode = true;
    m_rename_input->GetTextCtrl()->SetValue(m_current_project_name);
    m_rename_switch_panel->SetSelection(1);
    m_rename_input->GetTextCtrl()->SetFocus();
    m_rename_input->GetTextCtrl()->SetInsertionPointEnd();
}

void SelectMachineDialog::on_rename_enter()
{
    if (m_is_rename_mode == false){
        return;
    }
    else {
        m_is_rename_mode = false;
    }

    auto     new_file_name = m_rename_input->GetTextCtrl()->GetValue();
    wxString temp;
    int      num = 0;
    for (auto t : new_file_name) {
        if (t == wxString::FromUTF8("\x20")) {
            num++;
            if (num == 1) temp += t;
        } else {
            num = 0;
            temp += t;
        }
    }
    new_file_name         = temp;
    auto     m_valid_type = Valid;
    wxString info_line;

    const char* unusable_symbols = "<>[]:/\\|?*\"";

    const std::string unusable_suffix = PresetCollection::get_suffix_modified(); //"(modified)";
    for (size_t i = 0; i < std::strlen(unusable_symbols); i++) {
        if (new_file_name.find_first_of(unusable_symbols[i]) != std::string::npos) {
            info_line = _L("Name is invalid;") + "\n" + _L("illegal characters:") + " " + unusable_symbols;
            m_valid_type = NoValid;
            break;
        }
    }

    if (m_valid_type == Valid && new_file_name.find(unusable_suffix) != std::string::npos) {
        info_line = _L("Name is invalid;") + "\n" + _L("illegal suffix:") + "\n\t" + from_u8(PresetCollection::get_suffix_modified());
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_file_name.empty()) {
        info_line = _L("The name is not allowed to be empty.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_file_name.find_first_of(' ') == 0) {
        info_line = _L("The name is not allowed to start with space character.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_file_name.find_last_of(' ') == new_file_name.length() - 1) {
        info_line = _L("The name is not allowed to end with space character.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_file_name.size()  >= 100) {
        info_line = _L("The name length exceeds the limit.");
        m_valid_type = NoValid;
    }

    if (m_valid_type != Valid) {
        MessageDialog msg_wingow(nullptr, info_line, "", wxICON_WARNING | wxOK);
        if (msg_wingow.ShowModal() == wxID_OK) {
             m_rename_switch_panel->SetSelection(0);
             m_rename_text->SetLabel(m_current_project_name);
             m_rename_normal_panel->Layout();
             return;
        }
    }

    m_current_project_name = new_file_name;
    m_rename_switch_panel->SetSelection(0);
    m_rename_text->SetLabelText(m_current_project_name);
    m_rename_normal_panel->Layout();
}

void SelectMachineDialog::update_printer_combobox(wxCommandEvent &event)
{
    show_status(PrintDialogStatus::PrintStatusInit);
    update_user_printer();
}

void SelectMachineDialog::on_timer(wxTimerEvent &event)
{
    DeviceManager* dev_ = Slic3r::GUI::wxGetApp().getDeviceManager();
    if(!dev_) return;
    MachineObject* obj_ = dev_->get_my_machine(m_printer_last_select);
    if(!obj_) return;

    if (obj_->GetExtderSystem()->GetTotalExtderCount() > 1)
    {
        change_materialitem_tip(false); /*mapping to both ams and ext, is supported while total_extder_count is 2*/
    } else {
        change_materialitem_tip(true);
    }

    if (!obj_->GetFilaSystem()->HasAms()
        || obj_->ams_exist_bits == 0
        || !obj_->is_support_filament_backup
        || !obj_->GetExtderSystem()->HasFilamentBackup()
        || !obj_->GetFilaSystem()->IsAutoRefillEnabled()
        || !_HasAms(m_ams_mapping_result)) {
        if (m_ams_backup_tip->IsShown()) {
            m_ams_backup_tip->Hide();
            img_ams_backup->Hide();
        }
    }
    else {
        if (!m_ams_backup_tip->IsShown()) {
            m_ams_backup_tip->Show();
            img_ams_backup->Show();
        }
    }

    load_option_vals(obj_);
    update_show_status(obj_);
    update_print_status_msg();
    //update_scroll_area_size();/*STUDIO-12867 the page maybe blank in some platform. FIXME*/
}

void SelectMachineDialog::on_selection_changed(wxCommandEvent &event)
{
    /* reset timeout and reading printer info */
    m_status_bar->reset();
    m_timeout_count      = 0;
    m_ams_mapping_res  = false;
    m_ams_mapping_valid  = false;
    m_ams_mapping_result.clear();
    m_pre_print_checker.clear();

    m_link_edit_nozzle->Show(false);
    m_mapping_sugs_sizer->Show(false);
    m_change_filament_times_sizer->Show(false);
    m_txt_change_filament_times->Show(false);


    update_print_status_msg();
    auto selection = m_printer_box->GetPrinterComboBox()->GetSelection();
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;

    MachineObject* obj = nullptr;
    for (int i = 0; i < m_list.size(); i++) {
        if (i == selection) {

            //check lan mode machine
            if (m_list[i]->is_lan_mode_printer() && !m_list[i]->has_access_right()) {
                ConnectPrinterDialog dlg(wxGetApp().mainframe, wxID_ANY, _L("Input access code"));
                dlg.set_machine_object(m_list[i]);
                auto res = dlg.ShowModal();
                m_printer_last_select = "";
                m_printer_box->GetPrinterComboBox()->SetSelection(-1);
                m_printer_box->GetPrinterComboBox()->Refresh();
                m_printer_box->GetPrinterComboBox()->Update();
            }

            m_printer_last_select = m_list[i]->get_dev_id();
            obj = m_list[i];

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "for send task, current printer id =  " << m_printer_last_select << std::endl;
            break;
        }
    }

    if (obj && !obj->get_lan_mode_connection_state()) {
        obj->command_get_version();
        obj->command_request_push_all();
        if (!dev->get_selected_machine()) {
            dev->set_selected_machine(m_printer_last_select);
        }else if (dev->get_selected_machine()->get_dev_id() != m_printer_last_select) {
            dev->set_selected_machine(m_printer_last_select);
        }

        // Has changed machine unrecoverably
        m_check_flag = false;
    } else {
        BOOST_LOG_TRIVIAL(error) << "on_selection_changed dev_id not found";
        return;
    }


    //reset print status
    show_status(PrintDialogStatus::PrintStatusInit);
    update_show_status();
    update_print_status_msg();
}

void SelectMachineDialog::update_ams_check(MachineObject *obj)
{
    if (!obj) {return;}

    if (!obj->is_enable_np) {
        if (obj->HasAms()) {
            change_materialitem_tip(false);
        }
        else {
            change_materialitem_tip(true);
        }
    }else{
        change_materialitem_tip(false);
    }
}


void SelectMachineDialog::update_filament_change_count()
{
    /*check filament change times*/
    PartPlate *  part_plate   = m_plater->get_partplate_list().get_curr_plate();
    PrintBase *  print        = nullptr;
    GCodeResult *gcode_result = nullptr;

    m_change_filament_times_sizer->Show(false);
    m_txt_change_filament_times->Show(false);
    m_mapping_sugs_sizer->Show(false);
    m_link_edit_nozzle->Show(false);

    DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;
    MachineObject *obj = dev->get_selected_machine();
    if (!obj) return;
    if (m_print_type == FROM_SDCARD_VIEW) return;

    std::vector<int> filament_ids;
    for (auto mr : m_ams_mapping_result) {
        if (mr.ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID) || mr.ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID)) {
            filament_ids.push_back(mr.id);
        }
    }

    /*check nozzle & filament is it the best*/
    auto stats = m_plater->get_partplate_list().get_current_fff_print().statistics_by_extruder();
    auto best  = stats.stats_by_multi_extruder_best;
    auto curr  = stats.stats_by_multi_extruder_curr;

    int hand_changes_count = curr.filament_change_count - best.filament_change_count;
    int saving_weight      = curr.filament_flush_weight - best.filament_flush_weight;

    if (obj->GetExtderSystem()->GetTotalExtderCount() > 1) { m_link_edit_nozzle->Show(true); }

    if (hand_changes_count > 0 || saving_weight > 0) {
        m_mapping_sugs_sizer->Show(true);
        m_change_filament_times_sizer->Show(true);
        m_txt_change_filament_times->Show(true);
        m_txt_change_filament_times->SetLabel(wxString::Format(_L("Cost %dg filament and %d changes more than optimal grouping."), saving_weight, hand_changes_count));
        m_txt_change_filament_times->Wrap(FromDIP(580));
        m_txt_change_filament_times->Layout();
    }

    Layout();
    Fit();
}

static wxString _check_kval_not_default(const MachineObject* obj, const std::vector<FilamentInfo> &mapping_result)
{
    if (!obj) { return wxEmptyString;}

    wxString ams_names;
    for (const auto& info : mapping_result) {
        if (!obj->contains_tray(info.ams_id, std::to_string(info.tray_id))) { continue; }
        if (obj->get_tray(info.ams_id, std::to_string(info.tray_id)).cali_idx == -1) { continue; } /*-1 means default*/

        wxString ams_name;
        if (info.tray_id == VIRTUAL_TRAY_MAIN_ID) {
            ams_name = "Right-Ext";
        } else if (info.tray_id == VIRTUAL_TRAY_DEPUTY_ID) {
            ams_name = "Left-Ext";
        } else {
            ams_name = wxGetApp().transition_tridid(info.tray_id);
        }

        if (!ams_names.empty()) { ams_names += ", ";}
        ams_names += ams_name;
    }

    return ams_names;
}

static wxString _get_nozzle_name(int total_ext_count, int ext_id)
{
    if (total_ext_count == 2) {
        if (ext_id == MAIN_EXTRUDER_ID) {
            return _L("right nozzle");
        } else {
            return _L("left nozzle");
        }
    }

    return _L("nozzle");
}

static wxString _get_ext_loc_str(const std::unordered_set<int>& extruders, int total_ext_num)
{
    assert(!extruders.empty());
    if (total_ext_num == 1)
    {
        return _L("extruder");
    }
    else if (total_ext_num == 2)
    {
        if (extruders.size() == 2)
        {
            return _L("both extruders");
        }
        else if (extruders.size() == 1)
        {
            auto iter = extruders.begin();
            if (*iter == MAIN_EXTRUDER_ID)
            {
                return _L("right extruder");
            }
            else
            {
                return _L("left extruder");
            }
        }
    }

    return wxEmptyString;
}

void SelectMachineDialog::update_show_status(MachineObject* obj_)
{
    m_pre_print_checker.clear();

    /*agent check and printer valid check*/
    NetworkAgent* agent = Slic3r::GUI::wxGetApp().getAgent();
    if (!agent) {
        show_status(PrintDialogStatus::PrintStatusNoUserLogin);
        update_ams_check(nullptr);
        return;
    }

    if (!obj_) {
        show_status(PrintDialogStatus::PrintStatusInvalidPrinter);
        update_ams_check(nullptr);
        return;
    }

    /*combobox check*/
    if (m_print_type == PrintFromType::FROM_SDCARD_VIEW) {
        m_printer_box->GetPrinterComboBox()->Disable();
    } else {
        if (get_status() == PrintDialogStatus::PrintStatusRefreshingMachineList)
            m_printer_box->GetPrinterComboBox()->Disable();
        else
            m_printer_box->GetPrinterComboBox()->Enable();
    }

    /*mode check*/
    if (get_status() == PrintDialogStatus::PrintStatusRefreshingMachineList || get_status() == PrintDialogStatus::PrintStatusSending) return;

    /*no valid gcode file*/
    if (m_print_type == PrintFromType::FROM_NORMAL) {
        PartPlate* plate = m_plater->get_partplate_list().get_curr_plate();
        if (plate && !plate->is_valid_gcode_file()) {
            show_status(PrintDialogStatus::PrintStatusBlankPlate);
            return;
        }
    }

    /** error check **/
    /* check cloud machine connections */
    if (!obj_->is_lan_mode_printer() && !agent->is_server_connected()) {
        show_status(PrintDialogStatus::PrintStatusConnectingServer);
        reset_timeout();
        return;
    }

    if (!obj_->is_info_ready()) {
        if (is_timeout()) {
            m_ams_mapping_result.clear();
            sync_ams_mapping_result(m_ams_mapping_result);
            show_status(PrintDialogStatus::PrintStatusReadingTimeout);
        } else {
            m_timeout_count++;
            show_status(PrintDialogStatus::PrintStatusReading);
        }
        return;
    }

    reset_timeout();

    /*check print all*/
    if (!obj_->GetConfig()->SupportPrintAllPlates() && m_print_plate_idx == PLATE_ALL_IDX)
    {
        show_status(PrintDialogStatus::PrintStatusNotSupportedPrintAll);
        return;
    }

    if (!m_check_flag && obj_->is_info_ready()) {
        update_select_layout(obj_);
        update_ams_check(obj_);
        m_check_flag = true;
    }

    /*do ams mapping if no ams result*/
    if (m_ams_mapping_result.empty()) {
        do_ams_mapping(obj_, true);
        update_filament_change_count();
    }

    /* multi color external change assist*/
    if(obj_->is_support_ext_change_assist && !m_check_ext_change_assist->IsShown()){
        m_check_ext_change_assist->Show(true);
        m_label_ext_change_assist->Show(true);
    }else if(!obj_->is_support_ext_change_assist &&m_check_ext_change_assist->IsShown()){
        m_check_ext_change_assist->Hide();
        m_label_ext_change_assist->Hide();
    }
    /*check external change assist*/
    if(!m_ams_mapping_result.empty() && is_enable_external_change_assist(m_ams_mapping_result)){
        m_check_ext_change_assist->Enable(true);
    }else{
        m_check_ext_change_assist->SetValue(false);
        m_check_ext_change_assist->Enable(false);
    }

     /*reading done*/
    if (wxGetApp().app_config) {
        if (obj_->upgrade_force_upgrade) {
            show_status(PrintDialogStatus::PrintStatusNeedForceUpgrading);
            return;
        }

        if (obj_->upgrade_consistency_request) {
            show_status(PrintStatusNeedConsistencyUpgrading);
            return;
        }
    }

    if (!obj_->is_fdm_type()) {
        show_status(PrintDialogStatus::PrintStatusModeNotFDM);
        return;
    }

    if (is_blocking_printing(obj_)) {
        show_status(PrintDialogStatus::PrintStatusUnsupportedPrinter);
        return;
    } else if (obj_->is_in_upgrading()) {
        show_status(PrintDialogStatus::PrintStatusInUpgrading);
        return;
    } else if (obj_->is_system_printing()) {
        show_status(PrintDialogStatus::PrintStatusInSystemPrinting);
        return;
    } else if (obj_->is_in_printing() || obj_->ams_status_main == AMS_STATUS_MAIN_FILAMENT_CHANGE) {
        show_status(PrintDialogStatus::PrintStatusInPrinting);
        return;
    } else if (!obj_->GetConfig()->SupportPrintWithoutSD() && (obj_->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::NO_SDCARD)) {
        show_status(PrintDialogStatus::PrintStatusNoSdcard);
        return;
    }
    if (wxGetApp().preset_bundle->filament_presets.size() > 16 && m_print_type != PrintFromType::FROM_SDCARD_VIEW) { 
        if (!obj_->is_enable_ams_np && !obj_->is_enable_np)
        {
            show_status(PrintDialogStatus::PrintStatusColorQuantityExceed);
            return;
        }
    }

    /*check sdcard when if lan mode printer*/
    if (obj_->is_lan_mode_printer()) {
        if (obj_->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::NO_SDCARD
            && !obj_->is_support_print_with_emmc) {
            show_status(PrintDialogStatus::PrintStatusLanModeNoSdcard);
            return;
        } else if (obj_->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::HAS_SDCARD_READONLY) {
            show_status(PrintDialogStatus::PrintStatusLanModeSDcardNotAvailable);
            return;
        } else if(obj_->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::HAS_SDCARD_ABNORMAL &&  (wxGetApp().app_config->get("allow_abnormal_storage") == "false")){
            show_status(PrintDialogStatus::PrintStatusLanModeSDcardNotAvailable);
            return;
        }
    }

    if (!can_support_pa_auto_cali() && m_checkbox_list["flow_cali"]->IsShown() && m_checkbox_list["flow_cali"]->getValue() == "on") {
        show_status(PrintDialogStatus::PrintStatusTPUUnsupportAutoCali);
        return;
    }

    /*disable print when there is no mapping*/
    if (obj_->GetExtderSystem()->GetTotalExtderCount() > 1) {
        for (auto mres : m_ams_mapping_result) {
            if (mres.ams_id.empty() && mres.slot_id.empty()) {
                show_status(PrintDialogStatus::PrintStatusAmsMappingInvalid);
                return;
            }
        }
    }

    const auto &full_config = wxGetApp().preset_bundle->full_config();
    size_t      nozzle_nums = full_config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();

    /*the nozzle type of preset and machine are different*/
    if (nozzle_nums > 1 && m_print_type == FROM_NORMAL) {
        if (!_is_nozzle_data_valid(obj_, *obj_->GetExtderSystem())) {
            show_status(PrintDialogStatus::PrintStatusNozzleDataInvalid);
            return;
        }

        wxString error_message;
        if (!is_nozzle_type_match(*obj_->GetExtderSystem(), error_message)) {
            std::vector<wxString> params{error_message};
            params.emplace_back(_L("Tips: If you changed your nozzle of your printer lately, Please go to 'Device -> Printer parts' to change your nozzle setting."));
            show_status(PrintDialogStatus::PrintStatusNozzleMatchInvalid, params);
            return;
        }
    }

    // check nozzle type and diameter
    if (m_print_type == PrintFromType::FROM_NORMAL)
    {
        int mismatch_nozzle_id = 0;
        float nozzle_diameter = 0;
        if (!_is_same_nozzle_diameters(obj_, nozzle_diameter, mismatch_nozzle_id))
        {
            std::vector<wxString> msg_params;
            if (obj_->GetExtderSystem()->GetTotalExtderCount() == 2) {
                wxString mismatch_nozzle_str;
                if (mismatch_nozzle_id == MAIN_EXTRUDER_ID) {
                    mismatch_nozzle_str = _L("right nozzle");
                } else {
                    mismatch_nozzle_str = _L("left nozzle");
                }

                const wxString &nozzle_config = wxString::Format(_L("The %s diameter(%.1fmm) of current printer doesn't match with the slicing file (%.1fmm). "
                                                                    "Please make sure the nozzle installed matches with settings in printer, then set the "
                                                                    "corresponding printer preset when slicing."),
                                                                 mismatch_nozzle_str, obj_->GetExtderSystem()->GetNozzleDiameter(mismatch_nozzle_id), nozzle_diameter);
                msg_params.emplace_back(nozzle_config);
            } else {
                const wxString &nozzle_config = wxString::Format(_L("The current nozzle diameter (%.1fmm) doesn't match with the slicing file (%.1fmm). "
                                                                    "Please make sure the nozzle installed matches with settings in printer, then set the "
                                                                    "corresponding printer preset when slicing."),
                                                                 obj_->GetExtderSystem()->GetNozzleDiameter(0), nozzle_diameter);
                msg_params.emplace_back(nozzle_config);
            }

            msg_params.emplace_back(_L("Tips: If you changed your nozzle of your printer lately, Please go to 'Device -> Printer parts' to change your nozzle setting."));
            show_status(PrintDialogStatus::PrintStatusNozzleDiameterMismatch, msg_params);
            return;
        }

        const auto &used_nozzle_idxes = _get_used_nozzle_idxes();
        for (const auto &extder : obj_->GetExtderSystem()->GetExtruders()) {
            if (used_nozzle_idxes.count(extder.GetNozzleId()) == 0) { continue; }

            std::string filament_type;
            if (!is_nozzle_hrc_matched(&extder, filament_type)) {
                std::vector<wxString> error_msg;
                error_msg.emplace_back(wxString::Format(_L("The hardness of current material (%s) exceeds the hardness of %s(%s). Please verify the nozzle or material settings and try again."),
                                                           filament_type, _get_nozzle_name(obj_->GetExtderSystem()->GetTotalExtderCount(), extder.GetNozzleId()), format_steel_name(extder.GetNozzleType())));
                show_status(PrintDialogStatus::PrintStatusNozzleTypeMismatch, error_msg);
                return;
            }
        }
    }

    if (!DevPrinterConfigUtil::support_ams_ext_mix_print(obj_->printer_type)) {
        bool useAms = _HasAms(m_ams_mapping_result);
        bool useExt = _HasExt(m_ams_mapping_result);
        if (useAms && useExt) {
             show_status(PrintDialogStatus::PrintStatusAmsMappingMixInvalid);
             return;
        }
    }

    if (obj_->GetFilaSystem()->IsAmsSettingUp()) {
        show_status(PrintDialogStatus::PrintStatusAmsOnSettingup);
        return;
    }

    if (!m_ams_mapping_res && !DevMappingUtil::is_valid_mapping_result(obj_, m_ams_mapping_result)) {
        show_status(PrintDialogStatus::PrintStatusAmsMappingInvalid);
        return;
    }

    // filaments check for black list
    for (auto i = 0; i < m_ams_mapping_result.size(); i++) {
        const auto &ams_id  = m_ams_mapping_result[i].get_ams_id();
        const auto &slot_id = m_ams_mapping_result[i].get_slot_id();

        auto tid = m_ams_mapping_result[i].tray_id;

        std::string filament_type = boost::to_upper_copy(m_ams_mapping_result[i].type);
        std::string filament_brand;

        for (auto fs : m_filaments) {
            if (fs.id == m_ams_mapping_result[i].id) { filament_brand = m_filaments[i].brand; }
        }

        bool        in_blacklist = false;
        std::string action;
        wxString info;
        wxString wiki_url;
        DevFilaBlacklist::check_filaments_in_blacklist_url(obj_->printer_type, filament_brand, filament_type, m_ams_mapping_result[i].filament_id, ams_id, slot_id, "", in_blacklist,
                                                        action, info, wiki_url);
        if (in_blacklist) {

            std::vector<wxString> error_msg { info };
            if (action == "prohibition") {
                show_status(PrintDialogStatus::PrintStatusHasFilamentInBlackListError, error_msg, wiki_url);
                return;
            }
            else if (action == "warning") {
                show_status(PrintDialogStatus::PrintStatusHasFilamentInBlackListWarning, error_msg, wiki_url);/** warning check **/
              //  return;
            }
        }
    }

    /** warning check **/
    struct ExtruderStatus
    {
        bool has_ams{false};
        bool has_vt_slot{false};
    };
    std::vector<ExtruderStatus> extruder_status(nozzle_nums);
    for (const FilamentInfo &item : m_ams_mapping_result) {
        if (item.ams_id.empty()) continue;

        int extruder_id = obj_->get_extruder_id_by_ams_id(item.ams_id);
        if (devPrinterUtil::IsVirtualSlot(item.ams_id))
            extruder_status[extruder_id].has_vt_slot = true;
        else
            extruder_status[extruder_id].has_ams = true;
    }
    for (auto extruder : extruder_status) {
        if (extruder.has_ams && extruder.has_vt_slot) {
            show_status(PrintDialogStatus::PrintStatusMixAmsAndVtSlotWarning);
          //  return;
        }
    }
    if (m_ams_mapping_res) {
        if (has_timelapse_warning()) {
            show_status(PrintDialogStatus::PrintStatusTimelapseWarning);
          //  return;
        }
    } else {
        if (DevMappingUtil::is_valid_mapping_result(obj_, m_ams_mapping_result)) {
            if (!check_sdcard_for_timelpase(obj_)) {
                if (has_timelapse_warning()) {
                    show_status(PrintDialogStatus::PrintStatusTimelapseWarning);
                   // return;
                }
            }
        }
    }

    /*STUDIO-10970 check the k value and flow cali option*/
    if (m_checkbox_list["flow_cali"]->IsShown() && m_checkbox_list["flow_cali"]->getValue() == "auto") {
        const auto &not_default_ams_names = _check_kval_not_default(obj_, m_ams_mapping_result);
        if (!not_default_ams_names.empty()) {
            std::vector<wxString> params{not_default_ams_names};
            show_status(PrintDialogStatus::PrintStatusWarningKvalueNotUsed);
           // return;
        }
    }

    /*Check high temperture slicing*/
    if (m_print_type == PrintFromType::FROM_NORMAL) {
        std::set<string>  high_temp_filaments;
        std::unordered_set<int> known_fila_soften_extruders;
        std::unordered_set<int> unknown_fila_soften_extruders;
        auto preset_full_config = wxGetApp().preset_bundle->full_config();
        auto chamber_temperatures = preset_full_config.option<ConfigOptionInts>("chamber_temperature");
        for (const FilamentInfo& item : m_ams_mapping_result) {
            try
            {
                int chamber_temp = chamber_temperatures->values[item.id];
                if (chamber_temp >= 40) {
                    high_temp_filaments.insert(item.get_display_filament_type());// high printing chamber temperature
                }

                for (const auto& extder : obj_->GetExtderSystem()->GetExtruders()) { // check vitrification
                    if (extder.HasFilamentInExt()) {
                        const auto& fila_id = obj_->get_filament_id(extder.GetSlotNow().ams_id, extder.GetSlotNow().slot_id);
                        auto filament_info = wxGetApp().preset_bundle->get_filament_by_filament_id(fila_id);
                        if (filament_info) {
                            if (filament_info->temperature_vitrification - chamber_temp <= 5) {
                                known_fila_soften_extruders.insert(extder.GetExtId());
                            }
                        } else {
                            // the minimum temperature_vitrification of the known filaments is 43 degrees
                            if (43 - chamber_temp <= 5) { unknown_fila_soften_extruders.insert(extder.GetExtId()); }
                        }
                    }
                }
            }
            catch (std::exception&) { assert(0); }
        }

        if (!high_temp_filaments.empty()) {
            wxString filament_strs;/*join the filament strs*/
            for (auto filament : high_temp_filaments) {
                filament_strs += filament;
                if (filament != *high_temp_filaments.rbegin()) {
                    filament_strs += ", ";
                }
            }

            if (obj_->GetConfig()->HasChamber()) {
                const auto& msg = wxString::Format(_L("[ %s ] requires printing in a high-temperature environment.Please close the door."), filament_strs);
                show_status(PrintDialogStatus::PrintStatusFilamentWarningHighChamberTempCloseDoor, { msg });
                if (PrePrintChecker::is_error(PrintDialogStatus::PrintStatusFilamentWarningHighChamberTempCloseDoor)) { return; }
            } else {
                const auto& msg = wxString::Format(_L("[ %s ] requires printing in a high-temperature environment."), filament_strs);
                show_status(PrintDialogStatus::PrintStatusFilamentWarningHighChamberTemp, { msg });
                if (PrePrintChecker::is_error(PrintDialogStatus::PrintStatusFilamentWarningHighChamberTemp)) { return; }
            }
        }

        if (!known_fila_soften_extruders.empty()) {
            const wxString& msg = wxString::Format(_L("The filament on %s may soften. Please unload."),
                _get_ext_loc_str(known_fila_soften_extruders, obj_->GetExtderSystem()->GetTotalExtderCount()));
            show_status(PrintDialogStatus::PrintStatusFilamentWarningHighChamberTempSoft, std::vector<wxString> {msg});
            if (PrePrintChecker::is_error(PrintDialogStatus::PrintStatusFilamentWarningHighChamberTempSoft)) { return; }
        }

        if (!unknown_fila_soften_extruders.empty()) {
            const wxString& msg = wxString::Format(_L("The filament on %s is unknown and may soften. Please set filament."),
                _get_ext_loc_str(unknown_fila_soften_extruders, obj_->GetExtderSystem()->GetTotalExtderCount()));
            show_status(PrintDialogStatus::PrintStatusFilamentWarningUnknownHighChamberTempSoft, std::vector<wxString> {msg});
            if (PrePrintChecker::is_error(PrintDialogStatus::PrintStatusFilamentWarningUnknownHighChamberTempSoft)) { return; }
        }
    }

    // check extension tool warning
    UpdateStatusCheckWarning_ExtensionTool(obj_);

    /** normal check **/
    show_status(PrintDialogStatus::PrintStatusReadyToGo);
}

bool SelectMachineDialog::has_timelapse_warning(wxString &msg_text)
{
    PartPlate *plate = m_plater->get_partplate_list().get_curr_plate();
    for (auto warning : plate->get_slice_result()->warnings) {
        if (warning.msg == NOT_GENERATE_TIMELAPSE) {
            if (warning.error_code == "10014001") {
                msg_text = _L("When enable spiral vase mode, machines with I3 structure will not generate timelapse videos.");
            } else if (warning.error_code == "10014002") {
                msg_text = _L("The current printer does not support timelapse in Traditional Mode when printing By-Object.");
            }

            return true;
        }
    }

    return false;
}

bool SelectMachineDialog::can_support_pa_auto_cali()
{
    DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev)
        return true;
    MachineObject *obj = dev->get_selected_machine();
    if (!obj)
        return true;

    std::vector<std::string> unsupport_auto_cali_filaments = DevPrinterConfigUtil::get_unsupport_auto_cali_filaments(obj->printer_type);
    if (!unsupport_auto_cali_filaments.empty()) {
        auto iter = std::find_if(m_filaments.begin(), m_filaments.end(),
            [&unsupport_auto_cali_filaments](const FilamentInfo &item) {
            auto iter = std::find(unsupport_auto_cali_filaments.begin(), unsupport_auto_cali_filaments.end(), item.filament_id);
            return iter != unsupport_auto_cali_filaments.end();
        });

        return iter == m_filaments.end();
    }
    return true;
}

void SelectMachineDialog::reset_ams_material()
{
    MaterialHash::iterator iter = m_materialList.begin();
    while (iter != m_materialList.end()) {
        int           id = iter->first;
        Material* item = iter->second;
        MaterialItem* m = item->item;
        wxString ams_id = "-";
        wxColour ams_col = wxColour(0xEE, 0xEE, 0xEE);
        m->set_ams_info(ams_col, ams_id);
        iter++;
    }
}

void SelectMachineDialog::Enable_Refresh_Button(bool en)
{
    m_printer_box->EnableRefreshButton(en);
}

void SelectMachineDialog::Enable_Send_Button(bool en)
{
    if (!en) {
        if (m_button_ensure->IsEnabled()) {
            m_button_ensure->Disable();
            // ORCA no need to set colors again
        }
    } else {
        if (!m_button_ensure->IsEnabled()) {
            m_button_ensure->Enable();
            // ORCA no need to set colors again
        }
    }
}

void SelectMachineDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    print_time->msw_rescale();
    timeimg->SetBitmap(print_time->bmp());
    print_weight->msw_rescale();
    weightimg->SetBitmap(print_weight->bmp());
    rename_editable->msw_rescale();
    rename_editable_light->msw_rescale();
    if (ams_mapping_help_icon != nullptr) {
        ams_mapping_help_icon->msw_rescale();
        if (img_amsmapping_tip)img_amsmapping_tip->SetBitmap(ams_mapping_help_icon->bmp());
    }
    m_button_ensure->Rescale(); // ORCA
    m_status_bar->msw_rescale();

    for (auto material1 : m_materialList) {
        material1.second->item->msw_rescale();
    }

    for(auto opt : m_checkbox_list)
    {
        opt.second->msw_rescale();
    }

    m_mapping_popup.msw_rescale();

    Fit();
    Refresh();
}

void SelectMachineDialog::set_default()
{
    if (m_print_type == PrintFromType::FROM_NORMAL) {
        m_printer_box->SetDefault(false);
        m_rename_normal_panel->Show(true);
    }
    else if (m_print_type == PrintFromType::FROM_SDCARD_VIEW) {
        m_printer_box->SetDefault(true);
        m_rename_normal_panel->Show(false);
    }

    //project name
    m_rename_switch_panel->SetSelection(0);

    wxString filename = m_plater->get_export_gcode_filename("", true, m_print_plate_idx == PLATE_ALL_IDX ? true : false);
    if (m_print_plate_idx == PLATE_ALL_IDX && filename.empty()) {
        filename = _L("Untitled");
    }

    if (filename.empty()) {
        filename = m_plater->get_export_gcode_filename("", true);
        if (filename.empty()) filename = _L("Untitled");
    }

    fs::path filename_path(filename.c_str());
    std::string file_name  = filename_path.filename().string();
    if (from_u8(file_name).find(_L("Untitled")) != wxString::npos) {
        PartPlate *part_plate = m_plater->get_partplate_list().get_plate(m_print_plate_idx);
        if (part_plate) {
            if (std::vector<ModelObject *> objects = part_plate->get_objects_on_this_plate(); objects.size() > 0) {
                file_name = objects[0]->name;
                for (int i = 1; i < objects.size(); i++) {
                    file_name += (" + " + objects[i]->name);
                }
            }
            if (file_name.size() > 100) {
                file_name = file_name.substr(0, 97) + "...";
            }
        }
    }
    m_current_project_name = wxString::FromUTF8(file_name);


    //unsupported character filter
    m_current_project_name = from_u8(filter_characters(m_current_project_name.ToUTF8().data(), "<>[]:/\\|?*\""));

    m_rename_text->SetLabelText(m_current_project_name);
    m_rename_normal_panel->Layout();

    //clear combobox
    m_list.clear();
    m_printer_last_select = "";
    m_print_info = "";
    m_mapping_sugs_sizer->Show(false);
    m_change_filament_times_sizer->Show(false);
    m_txt_change_filament_times->Show(false);

    // rset status bar
    m_status_bar->reset();

    NetworkAgent* agent = wxGetApp().getAgent();
    if (agent) {
        if (agent->is_user_login()) {
            show_status(PrintDialogStatus::PrintStatusInit);
        }
    }

    if (m_print_type == PrintFromType::FROM_NORMAL) {
        reset_and_sync_ams_list();
        set_default_normal(m_plater->get_partplate_list().get_curr_plate()->thumbnail_data);
    }
    else if (m_print_type == PrintFromType::FROM_SDCARD_VIEW) {
        update_page_turn_state(true);
        set_default_from_sdcard();
    }

    Layout();
    Fit();
}

void SelectMachineDialog::change_materialitem_tip(bool no_ams_only_ext)
{
    MaterialHash::iterator iter = m_materialList.begin();
    while (iter != m_materialList.end()) {
        int       id   = iter->first;
        Material *item = iter->second;
        if (item) {
            if (no_ams_only_ext && item->item->m_ams_name == "Ext") {
                item->item->SetToolTip(wxEmptyString);
            }
            else {
                wxString tip_text;
                if (item->item->m_match) {
                    tip_text = _L("Upper half area:  Original\nLower half area:  Filament in AMS\nAnd you can click it to modify");
                } else {
                    tip_text = _L("Unable to automatically match to suitable filament. Please click to manually match.");
                }

                if (item->item->GetToolTipText() != tip_text) {
                    item->item->SetToolTip(tip_text);
                }
            }
        }
        iter++;
    }
}
void SelectMachineDialog::reset_and_sync_ams_list()
{
    // for black list
    std::vector<std::string> materials;
    std::vector<std::string> brands;
    std::vector<std::string> display_materials;
    std::vector<std::string> m_filaments_id;
    auto                     preset_bundle = wxGetApp().preset_bundle;

    for (auto filament_name : preset_bundle->filament_presets) {
        for (int f_index = 0; f_index < preset_bundle->filaments.size(); f_index++) {
            PresetCollection *filament_presets = &wxGetApp().preset_bundle->filaments;
            Preset *          preset           = &filament_presets->preset(f_index);
            int size = preset_bundle->filaments.size();
            if (preset && filament_name.compare(preset->name) == 0) {
                std::string display_filament_type;
                std::string filament_type = preset->config.get_filament_type(display_filament_type);
                std::string m_filament_id = preset->filament_id;
                display_materials.push_back(display_filament_type);
                materials.push_back(filament_type);
                m_filaments_id.push_back(m_filament_id);

                std::string m_vendor_name = "";
                auto        vendor        = dynamic_cast<ConfigOptionStrings *>(preset->config.option("filament_vendor"));
                if (vendor && (vendor->values.size() > 0)) {
                    std::string vendor_name = vendor->values[0];
                    m_vendor_name           = vendor_name;
                }
                brands.push_back(m_vendor_name);
            }
        }
    }

    auto           extruders = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_used_filaments();
    BitmapCache    bmcache;
    MaterialHash::iterator iter = m_materialList.begin();
    while (iter != m_materialList.end()) {
        int       id   = iter->first;
        Material *item = iter->second;
        item->item->Destroy();
        delete item;
        iter++;
    }

    m_sizer_ams_mapping->Clear();
    m_materialList.clear();
    m_filaments.clear();
    m_ams_tooltip =_L("Upper half area:  Original\nLower half area:  Filament in AMS\nAnd you can click it to modify");

    const auto& full_config = wxGetApp().preset_bundle->full_config();
    size_t nozzle_nums = full_config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();

    bool use_double_extruder = nozzle_nums > 1 ? true : false;
    if (use_double_extruder)
    {
        const auto& project_config = preset_bundle->project_config;
        m_filaments_map = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_real_filament_maps(project_config);
    }

    for (auto i = 0; i < extruders.size(); i++) {
        auto          extruder = extruders[i] - 1;
        auto          colour   = wxGetApp().preset_bundle->project_config.opt_string("filament_colour", (unsigned int) extruder);
        unsigned char rgb[4];
        bmcache.parse_color4(colour, rgb);

        auto colour_rgb = wxColour((int) rgb[0], (int) rgb[1], (int) rgb[2], (int) rgb[3]);
        if (extruder >= materials.size() || extruder < 0 || extruder >= display_materials.size()) continue;

        MaterialItem* item = nullptr;
        if (use_double_extruder)
        {

            if (m_filaments_map[extruder] == 1)
            {
                item = new MaterialItem(m_filament_left_panel, colour_rgb, _L(display_materials[extruder]));
                m_sizer_ams_mapping_left->Add(item, 0, wxALL, FromDIP(5));
            }
            else if (m_filaments_map[extruder] == 2)
            {
                item = new MaterialItem(m_filament_right_panel, colour_rgb, _L(display_materials[extruder]));
                m_sizer_ams_mapping_right->Add(item, 0, wxALL, FromDIP(5));
            }
        }
        else
        {
            item = new MaterialItem(m_filament_panel, colour_rgb, _L(display_materials[extruder]));
            m_sizer_ams_mapping->Add(item, 0, wxALL, FromDIP(5));
        }
        item->SetToolTip(m_ams_tooltip);
        item->Bind(wxEVT_LEFT_UP, [this, item, materials, extruder](wxMouseEvent &e) {});
        item->Bind(wxEVT_LEFT_DOWN, [this, item, materials, extruder](wxMouseEvent &e) {
            if (!item->m_enable) {return;}
            if (!m_check_flag || m_print_status == PrintDialogStatus::PrintStatusUnsupportedPrinter) { return; } /*STUDIO-11301*/

            MaterialHash::iterator iter = m_materialList.begin();
            while (iter != m_materialList.end()) {
                int           id   = iter->first;
                Material *    item = iter->second;
                MaterialItem *m    = item->item;
                m->on_normal();
                iter++;
            }

            m_current_filament_id = extruder;
            item->on_selected();

            auto    mouse_pos = ClientToScreen(e.GetPosition());
            wxPoint rect      = item->ClientToScreen(wxPoint(0, 0));

            // update ams data
            DeviceManager *dev_manager = Slic3r::GUI::wxGetApp().getDeviceManager();
            if (!dev_manager) return;
            MachineObject *obj_ = dev_manager->get_selected_machine();
            const auto& full_config = wxGetApp().preset_bundle->full_config();
            size_t nozzle_nums = full_config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
            if (nozzle_nums > 1)
            {
                if (obj_ && can_hybrid_mapping(*obj_->GetExtderSystem()))
                {
                    m_mapping_popup.set_show_type(ShowType::LEFT_AND_RIGHT);
                }
                else if (m_filaments_map[extruder] == 1)
                {
                    m_mapping_popup.set_show_type(ShowType::LEFT);
                }
                else if(m_filaments_map[extruder] == 2)
                {
                    m_mapping_popup.set_show_type(ShowType::RIGHT);
                }
            } else {
                m_mapping_popup.set_show_type(ShowType::RIGHT);
            }
            if (obj_) {
                if (m_mapping_popup.IsShown()) return;
                wxPoint pos = item->ClientToScreen(wxPoint(0, 0));
                pos.y += item->GetRect().height;
                m_mapping_popup.Move(pos);

                if (obj_ && obj_->get_dev_id() == m_printer_last_select) {
                    m_mapping_popup.set_parent_item(item);
                    m_mapping_popup.set_only_show_ext_spool(false);
                    m_mapping_popup.set_current_filament_id(extruder);
                    m_mapping_popup.set_tag_texture(materials[extruder]);
                    m_mapping_popup.set_send_win(this);//fix bug:fisrt click is not valid
                    m_mapping_popup.update(obj_, m_ams_mapping_result);
                    m_mapping_popup.Popup();
                }
            }
        });

        Material *material_item = new Material();
        material_item->id       = extruder;
        material_item->item     = item;
        m_materialList[i]       = material_item;

        // build for ams mapping
        if (extruder < materials.size() && extruder >= 0) {
            FilamentInfo info;
            info.id          = extruder;
            info.type        = materials[extruder];
            info.brand       = brands[extruder];
            info.filament_id = m_filaments_id[extruder];
            info.color       = wxString::Format("#%02X%02X%02X%02X", colour_rgb.Red(), colour_rgb.Green(), colour_rgb.Blue(), colour_rgb.Alpha()).ToStdString();
            m_filaments.push_back(info);
        }
    }

    if (use_double_extruder)
    {
        m_filament_left_panel->Show();
        m_filament_right_panel->Show();
        m_filament_panel->Hide();

        m_sizer_ams_mapping_left->SetCols(4);
        m_sizer_ams_mapping_left->Layout();
        m_filament_panel_left_sizer->Layout();
        m_filament_left_panel->Layout();

        m_sizer_ams_mapping_right->SetCols(4);
        m_sizer_ams_mapping_right->Layout();
        m_filament_panel_right_sizer->Layout();
        m_filament_right_panel->Layout();
    }
    else
    {
        m_filament_left_panel->Hide();
        m_filament_right_panel->Hide();
        m_filament_panel->Show();
        m_sizer_ams_mapping->SetCols(8);
        m_sizer_ams_mapping->Layout();
        m_filament_panel_sizer->Layout();
    }

    // reset_ams_material();//show "-"
}

void SelectMachineDialog::clone_thumbnail_data() {
    //record preview_colors
    MaterialHash::iterator iter               = m_materialList.begin();
    if (m_preview_colors_in_thumbnail.size() != m_materialList.size()) {
        m_preview_colors_in_thumbnail.resize(m_materialList.size());
    }
    while (iter != m_materialList.end()) {
        int           id   = iter->first;
        Material *    item = iter->second;
        MaterialItem *m    = item->item;
        m_preview_colors_in_thumbnail[id] = m->m_material_coloul;
        if (item->id < m_cur_colors_in_thumbnail.size()) {
            m_cur_colors_in_thumbnail[item->id] = m->m_ams_coloul;
        }
        else {//exist empty or unrecognized type ams in machine
            m_cur_colors_in_thumbnail.resize(item->id + 1);
            m_cur_colors_in_thumbnail[item->id] = m->m_ams_coloul;
        }
        iter++;
    }
    //copy data
    auto &data   = m_cur_input_thumbnail_data;
    m_preview_thumbnail_data.reset();
    m_preview_thumbnail_data.set(data.width, data.height);
    if (data.width > 0 && data.height > 0) {
        for (unsigned int r = 0; r < data.height; ++r) {
            unsigned int rr = (data.height - 1 - r) * data.width;
            for (unsigned int c = 0; c < data.width; ++c) {
                unsigned char *origin_px   = (unsigned char *) data.pixels.data() + 4 * (rr + c);
                unsigned char *new_px      = (unsigned char *) m_preview_thumbnail_data.pixels.data() + 4 * (rr + c);
                for (size_t i = 0; i < 4; i++) {
                    new_px[i] = origin_px[i];
                }
            }
        }
    }
    //record_edge_pixels_data
    record_edge_pixels_data();
}

void SelectMachineDialog::record_edge_pixels_data()
{
    auto is_not_in_preview_colors = [this](unsigned char r, unsigned char g , unsigned char b , unsigned char a) {
        for (size_t i = 0; i < m_preview_colors_in_thumbnail.size(); i++) {
            wxColour  render_color  = adjust_color_for_render(m_preview_colors_in_thumbnail[i]);
            if (render_color.Red() == r && render_color.Green() == g && render_color.Blue() == b /*&& render_color.Alpha() == a*/) {
                return false;
            }
        }
        return true;
    };
    ThumbnailData &data = m_cur_no_light_thumbnail_data;
    ThumbnailData &origin_data = m_cur_input_thumbnail_data;
    if (data.width > 0 && data.height > 0 && (data.width == origin_data.width && data.height == origin_data.height) ) {
        m_edge_pixels.resize(data.width * data.height);
        for (unsigned int r = 0; r < data.height; ++r) {
            unsigned int rr        = (data.height - 1 - r) * data.width;
            for (unsigned int c = 0; c < data.width; ++c) {
                unsigned char *no_light_px = (unsigned char *) data.pixels.data() + 4 * (rr + c);
                unsigned char *origin_px          = (unsigned char *) origin_data.pixels.data() + 4 * (rr + c);
                m_edge_pixels[r * data.width + c] = false;
                if (origin_px[3] > 0) {
                    if (is_not_in_preview_colors(no_light_px[0], no_light_px[1], no_light_px[2], origin_px[3])) {
                        m_edge_pixels[r * data.width + c] = true;
                    }
                }
            }
        }
    }
}

wxColour SelectMachineDialog::adjust_color_for_render(const wxColour &color)
{
    ColorRGBA _temp_color_color(color.Red() / 255.0f, color.Green() / 255.0f, color.Blue() / 255.0f, color.Alpha() / 255.0f);
    auto                 _temp_color_color_ = adjust_color_for_rendering(_temp_color_color);
    wxColour             render_color((int) (_temp_color_color_[0] * 255.0f), (int) (_temp_color_color_[1] * 255.0f), (int) (_temp_color_color_[2] * 255.0f),
                          (int) (_temp_color_color_[3] * 255.0f));
    return render_color;
}

void SelectMachineDialog::final_deal_edge_pixels_data(ThumbnailData &data)
{
    if (data.width > 0 && data.height > 0 && m_edge_pixels.size() >0 ) {
        for (unsigned int r = 0; r < data.height; ++r) {
             unsigned int rr            = (data.height - 1 - r) * data.width;
             bool         exist_rr_up   = r >= 1 ? true : false;
             bool         exist_rr_down = r <= data.height - 2 ? true : false;
             unsigned int rr_up         = exist_rr_up ? (data.height - 1 - (r - 1)) * data.width : 0;
             unsigned int rr_down       = exist_rr_down ? (data.height - 1 - (r + 1)) * data.width : 0;
             for (unsigned int c = 0; c < data.width; ++c) {
                  bool         exist_c_left  = c >= 1 ? true : false;
                  bool         exist_c_right = c <= data.width - 2 ? true : false;
                  unsigned int c_left        = exist_c_left ? c - 1 : 0;
                  unsigned int c_right       = exist_c_right ? c + 1 : 0;
                  unsigned char *cur_px   = (unsigned char *) data.pixels.data() + 4 * (rr + c);
                  unsigned char *relational_pxs[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
                  if (exist_rr_up && exist_c_left) { relational_pxs[0] = (unsigned char *) data.pixels.data() + 4 * (rr_up + c_left); }
                  if (exist_rr_up) { relational_pxs[1] = (unsigned char *) data.pixels.data() + 4 * (rr_up + c); }
                  if (exist_rr_up && exist_c_right) { relational_pxs[2] = (unsigned char *) data.pixels.data() + 4 * (rr_up + c_right); }
                  if (exist_c_left) { relational_pxs[3] = (unsigned char *) data.pixels.data() + 4 * (rr + c_left); }
                  if (exist_c_right) { relational_pxs[4] = (unsigned char *) data.pixels.data() + 4 * (rr + c_right); }
                  if (exist_rr_down && exist_c_left) { relational_pxs[5] = (unsigned char *) data.pixels.data() + 4 * (rr_down + c_left); }
                  if (exist_rr_down) { relational_pxs[6] = (unsigned char *) data.pixels.data() + 4 * (rr_down + c); }
                  if (exist_rr_down && exist_c_right) { relational_pxs[7] = (unsigned char *) data.pixels.data() + 4 * (rr_down + c_right); }
                  if (cur_px[3] > 0 && m_edge_pixels[r * data.width + c]) {
                       int rgba_sum[4] = {0, 0, 0, 0};
                       int valid_count = 0;
                       for (size_t k = 0; k < 8; k++) {
                           if (relational_pxs[k]) {
                               if (k == 0 && m_edge_pixels[(r - 1) * data.width + c_left]) {
                                    continue;
                                }
                               if (k == 1 && m_edge_pixels[(r - 1) * data.width + c]) {
                                    continue;
                                }
                                if (k == 2 && m_edge_pixels[(r - 1) * data.width + c_right]) {
                                    continue;
                                }
                                if (k == 3 && m_edge_pixels[r * data.width + c_left]) {
                                    continue;
                                }
                                if (k == 4 && m_edge_pixels[r * data.width + c_right]) {
                                    continue;
                                }
                                if (k == 5 && m_edge_pixels[(r + 1) * data.width + c_left]) {
                                    continue;
                                }
                                if (k == 6 && m_edge_pixels[(r + 1) * data.width + c]) {
                                    continue;
                                }
                                if (k == 7 && m_edge_pixels[(r + 1) * data.width + c_right]) {
                                    continue;
                                }
                                for (size_t m = 0; m < 4; m++) {
                                    rgba_sum[m] += relational_pxs[k][m];
                                }
                                valid_count++;
                           }
                       }
                       if (valid_count > 0) {
                            for (size_t m = 0; m < 4; m++) {
                                cur_px[m] = std::clamp(int(rgba_sum[m] / (float)valid_count), 0, 255);
                            }
                       }
                  }
             }
        }
    }
}

void SelectMachineDialog::updata_thumbnail_data_after_connected_printer()
{
    // change thumbnail_data
    ThumbnailData &input_data          = m_plater->get_partplate_list().get_curr_plate()->thumbnail_data;
    ThumbnailData &no_light_data = m_plater->get_partplate_list().get_curr_plate()->no_light_thumbnail_data;
    if (input_data.width == 0 || input_data.height == 0 || no_light_data.width == 0 || no_light_data.height == 0) {
        wxGetApp().plater()->update_all_plate_thumbnails(false);
    }
    unify_deal_thumbnail_data(input_data, no_light_data);
}

void SelectMachineDialog::unify_deal_thumbnail_data(ThumbnailData &input_data, ThumbnailData &no_light_data) {
    if (input_data.width == 0 || input_data.height == 0 || no_light_data.width == 0 || no_light_data.height == 0) {
        BOOST_LOG_TRIVIAL(error) << "SelectMachineDialog::no_light_data is empty,error";
        return;
    }
    m_cur_input_thumbnail_data    = input_data;
    m_cur_no_light_thumbnail_data = no_light_data;
    clone_thumbnail_data();
    MaterialHash::iterator iter               = m_materialList.begin();
    if (m_cur_colors_in_thumbnail.size() > 0) {
        change_default_normal(-1, wxColour());
        final_deal_edge_pixels_data(m_preview_thumbnail_data);
        set_default_normal(m_preview_thumbnail_data);
    } else {
        set_default_normal(input_data);
    }
}

void SelectMachineDialog::change_default_normal(int old_filament_id, wxColour temp_ams_color)
{
    if (m_cur_colors_in_thumbnail.size() == 0) {
        BOOST_LOG_TRIVIAL(error) << "SelectMachineDialog::change_default_normal:error:m_cur_colors_in_thumbnail.size() == 0";
        return;
    }
    if (old_filament_id >= 0) {
        if (old_filament_id < m_cur_colors_in_thumbnail.size()) {
            m_cur_colors_in_thumbnail[old_filament_id] = temp_ams_color;
        }
        else {
            BOOST_LOG_TRIVIAL(error) << "SelectMachineDialog::change_default_normal:error:old_filament_id > m_cur_colors_in_thumbnail.size()";
            return;
        }
    }
    ThumbnailData& data = m_cur_input_thumbnail_data;
    ThumbnailData& no_light_data = m_cur_no_light_thumbnail_data;
    if (data.width > 0 && data.height > 0 && data.width == no_light_data.width && data.height == no_light_data.height) {
        for (unsigned int r = 0; r < data.height; ++r) {
            unsigned int rr = (data.height - 1 - r) * data.width;
            for (unsigned int c = 0; c < data.width; ++c) {
                unsigned char *no_light_px   = (unsigned char *) no_light_data.pixels.data() + 4 * (rr + c);
                unsigned char *origin_px = (unsigned char *) data.pixels.data() + 4 * (rr + c);
                unsigned char *new_px        = (unsigned char *) m_preview_thumbnail_data.pixels.data() + 4 * (rr + c);
                if (origin_px[3]  > 0 && m_edge_pixels[r * data.width + c] == false) {
                    auto filament_id = 255 - no_light_px[3];
                    if (filament_id >= m_cur_colors_in_thumbnail.size()) {
                        continue;
                    }
                    wxColour temp_ams_color_in_loop = m_cur_colors_in_thumbnail[filament_id];
                    wxColour ams_color              = adjust_color_for_render(temp_ams_color_in_loop);
                    //change color
                    new_px[3]                       = ams_color.Alpha(); //origin_px[3]; // alpha
                    int origin_rgb = origin_px[0] + origin_px[1] + origin_px[2];
                    int no_light_px_rgb   = no_light_px[0] + no_light_px[1] + no_light_px[2];
                    unsigned char i               = 0;
                    if (origin_rgb >= no_light_px_rgb) {//Brighten up
                        unsigned char cur_single_color = ams_color.Red();
                        new_px[i]                      = std::clamp(cur_single_color + (origin_px[i] - no_light_px[i]), 0, 255);
                        i++;
                        cur_single_color = ams_color.Green();
                        new_px[i]                      = std::clamp(cur_single_color + (origin_px[i] - no_light_px[i]), 0, 255);
                        i++;
                        cur_single_color =  ams_color.Blue();
                        new_px[i]                      = std::clamp(cur_single_color + (origin_px[i] - no_light_px[i]), 0, 255);
                    } else {//Dimming
                        float         ratio            = origin_rgb / (float) no_light_px_rgb;
                        unsigned char cur_single_color = ams_color.Red();
                        new_px[i]                      = std::clamp((int)(cur_single_color * ratio), 0, 255);
                        i++;
                        cur_single_color = ams_color.Green();
                        new_px[i]        = std::clamp((int) (cur_single_color * ratio), 0, 255);
                        i++;
                        cur_single_color = ams_color.Blue();
                        new_px[i]        = std::clamp((int) (cur_single_color * ratio), 0, 255);
                    }
                }
            }
        }
    }
    else {
        BOOST_LOG_TRIVIAL(error) << "SelectMachineDialog::change_defa:no_light_data is empty,error";
    }
}

void SelectMachineDialog::set_default_normal(const ThumbnailData &data)
{
    update_page_turn_state(false);
    if (data.is_valid()) {
        wxImage image(data.width, data.height);
        image.InitAlpha();
        for (unsigned int r = 0; r < data.height; ++r) {
            unsigned int rr = (data.height - 1 - r) * data.width;
            for (unsigned int c = 0; c < data.width; ++c) {
                unsigned char *px = (unsigned char *) data.pixels.data() + 4 * (rr + c);
                image.SetRGB((int) c, (int) r, px[0], px[1], px[2]);
                image.SetAlpha((int) c, (int) r, px[3]);
            }
        }
        image = image.Rescale(FromDIP(198), FromDIP(198));
        m_thumbnailPanel->set_thumbnail(image);
    }

    m_basic_panel->Layout();
    m_basic_panel->Fit();

    // disable pei bed
    DeviceManager *dev_manager = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev_manager) return;
    MachineObject *obj_       = dev_manager->get_selected_machine();
    wxSize         screenSize = wxGetDisplaySize();
    auto           dialogSize = this->GetSize();

#ifdef __WINDOWS__

#endif // __WXOSX_MAC__
    // basic info
    auto       aprint_stats = m_plater->get_partplate_list().get_current_fff_print().print_statistics();
    wxString   time;
    PartPlate *plate = m_plater->get_partplate_list().get_curr_plate();
    if (plate) {
        if (plate->get_slice_result()) { time = wxString::Format("%s", short_time(get_time_dhms(plate->get_slice_result()->print_statistics.modes[0].time))); }
    }

    char weight[64];
    if (wxGetApp().app_config->get("use_inches") == "1") {
        ::sprintf(weight, "%.2f oz", aprint_stats.total_weight * 0.035274); // ORCA remove spacing before text
    } else {
        ::sprintf(weight, "%.2f g", aprint_stats.total_weight); // ORCA remove spacing before text
    }

    m_stext_time->SetLabel(time);
    m_stext_weight->SetLabel(weight);
}

void SelectMachineDialog::set_default_from_sdcard()
{
    DeviceManager *dev_manager = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev_manager) return;
    MachineObject *obj_ = dev_manager->get_selected_machine();
    if (!obj_) { return; };

    m_printer_box->SetPrinterName(wxString::FromUTF8(obj_->get_dev_name()));

    m_print_plate_total = m_required_data_plate_data_list.size();
    update_page_turn_state(true);

    ThumbnailData& data = m_required_data_plate_data_list[m_print_plate_idx]->plate_thumbnail;

    if (data.pixels.size() > 0) {
        wxMemoryInputStream mis((unsigned char*)data.pixels.data(), data.pixels.size());
        wxImage image = wxImage(mis);
        image = image.Rescale(FromDIP(198), FromDIP(198));
        m_thumbnailPanel->set_thumbnail(image);
    }

    //for black list
    std::vector<std::string> materials;
    std::vector<std::string> brands;
    std::vector<std::string> display_materials;

    for (auto i = 0; i < m_required_data_plate_data_list[m_print_plate_idx]->slice_filaments_info.size(); i++) {
        FilamentInfo fo = m_required_data_plate_data_list[m_print_plate_idx]->slice_filaments_info[i];
        display_materials.push_back(fo.type);
        materials.push_back(fo.type);
        brands.push_back(fo.brand);
    }

    auto opt_nozzle_diameters = m_required_data_config.option<ConfigOptionFloats>("nozzle_diameter");
    auto diameters_count = 1;
    if (opt_nozzle_diameters) {
        diameters_count = opt_nozzle_diameters->size();
    }

    //std::vector<int> filament_map;
    m_filaments_map.clear();
    if (m_required_data_plate_data_list[m_print_plate_idx]->config.option<ConfigOptionInts>("filament_map")){
        m_filaments_map = m_required_data_plate_data_list[m_print_plate_idx]->config.option<ConfigOptionInts>("filament_map")->values;
    }

    // ams mapping area
    if (diameters_count > 1) {
        m_filament_left_panel->Show();
        m_filament_right_panel->Show();
        m_filament_panel->Hide();

        m_sizer_ams_mapping_left->SetCols(4);
        m_sizer_ams_mapping_left->Layout();
        m_filament_panel_left_sizer->Layout();
        m_filament_left_panel->Layout();

        m_sizer_ams_mapping_right->SetCols(4);
        m_sizer_ams_mapping_right->Layout();
        m_filament_panel_right_sizer->Layout();
        m_filament_right_panel->Layout();

    } else {
        m_filament_left_panel->Hide();
        m_filament_right_panel->Hide();
        m_filament_panel->Show();
        m_sizer_ams_mapping->SetCols(8);
        m_sizer_ams_mapping->Layout();
        m_filament_panel_sizer->Layout();
    }

    //init MaterialItem
    MaterialHash::iterator iter = m_materialList.begin();
    while (iter != m_materialList.end()) {
        int       id = iter->first;
        Material* item = iter->second;
        item->item->Destroy();
        delete item;
        iter++;
    }

    m_ams_mapping_result.clear();
    m_sizer_ams_mapping->Clear();
    m_sizer_ams_mapping_left->Clear();
    m_sizer_ams_mapping_right->Clear();
    m_materialList.clear();
    m_filaments.clear();

    for (auto i = 0; i < m_required_data_plate_data_list[m_print_plate_idx]->slice_filaments_info.size(); i++) {
        FilamentInfo fo = m_required_data_plate_data_list[m_print_plate_idx]->slice_filaments_info[i];

        MaterialItem* item = nullptr;

        if (diameters_count > 1) {
            if (fo.id < m_filaments_map.size()) {
                auto nozzle_id = m_filaments_map[fo.id];

                if (nozzle_id == 1) {
                    item = new MaterialItem(m_filament_left_panel, wxColour(fo.color), fo.get_display_filament_type());
                    m_sizer_ams_mapping_left->Add(item, 0, wxALL, FromDIP(5));
                } else if (nozzle_id == 2) {
                    item = new MaterialItem(m_filament_right_panel, wxColour(fo.color), fo.get_display_filament_type());
                    m_sizer_ams_mapping_right->Add(item, 0, wxALL, FromDIP(5));
                }
            }
        } else {
            item = new MaterialItem(m_filament_panel, wxColour(fo.color), fo.get_display_filament_type());
            m_sizer_ams_mapping->Add(item, 0, wxALL, FromDIP(5));
        }

        item->Bind(wxEVT_LEFT_UP, [this, item, materials](wxMouseEvent& e) {});
        item->Bind(wxEVT_LEFT_DOWN, [this, obj_, item, materials, diameters_count, fo](wxMouseEvent& e) {
            if (!item->m_enable) {return;}
            if (!m_check_flag || m_print_status == PrintDialogStatus::PrintStatusUnsupportedPrinter) { return; } /*STUDIO-11301*/

            MaterialHash::iterator iter = m_materialList.begin();
            while (iter != m_materialList.end()) {
                int           id = iter->first;
                Material* item = iter->second;
                MaterialItem* m = item->item;
                m->on_normal();
                iter++;
            }

            try {
                m_current_filament_id = fo.id;
            }
            catch (...) {}
            item->on_selected();


            auto    mouse_pos = ClientToScreen(e.GetPosition());
            wxPoint rect = item->ClientToScreen(wxPoint(0, 0));

            if (obj_) {
                if (m_mapping_popup.IsShown()) return;
                wxPoint pos = item->ClientToScreen(wxPoint(0, 0));
                pos.y += item->GetRect().height;
                m_mapping_popup.Move(pos);

                if (diameters_count > 1) {
                    if (obj_ && can_hybrid_mapping(*obj_->GetExtderSystem())) {
                        m_mapping_popup.set_show_type(ShowType::LEFT_AND_RIGHT);
                    } else if (m_filaments_map[m_current_filament_id] == 1) {
                        m_mapping_popup.set_show_type(ShowType::LEFT);
                    } else if (m_filaments_map[m_current_filament_id] == 2) {
                        m_mapping_popup.set_show_type(ShowType::RIGHT);
                    }
                } else {
                    m_mapping_popup.set_show_type(ShowType::RIGHT);
                }

                if (obj_ && obj_->get_dev_id() == m_printer_last_select)
                {
                    m_mapping_popup.set_parent_item(item);
                    m_mapping_popup.set_current_filament_id(fo.id);
                    m_mapping_popup.set_tag_texture(fo.type);
                    m_mapping_popup.set_send_win(this);
                    m_mapping_popup.update(obj_, m_ams_mapping_result);
                    m_mapping_popup.Popup();
                }
            }
            });

        Material* material_item = new Material();
        material_item->id = fo.id;
        material_item->item = item;
        m_materialList[i] = material_item;

        // build for ams mapping
        m_filaments.push_back(fo);
    }

    if (m_required_data_plate_data_list[m_print_plate_idx]->slice_filaments_info.size() <= 4) {
        m_sizer_ams_mapping->SetCols(m_required_data_plate_data_list[m_print_plate_idx]->slice_filaments_info.size());
    }
    else {
        m_sizer_ams_mapping->SetCols(4);
    }

    m_basic_panel->Layout();
    m_basic_panel->Fit();

    wxSize screenSize = wxGetDisplaySize();
    auto dialogSize = this->GetSize();

    reset_ams_material();

    // basic info
    try {
        float float_time = std::stof(m_required_data_plate_data_list[m_print_plate_idx]->get_gcode_prediction_str());
        double float_weight = std::stof(m_required_data_plate_data_list[m_print_plate_idx]->get_gcode_weight_str());
        wxString   time;
        time = wxString::Format("%s", short_time(get_time_dhms(float_time)));
        char weight[64];
        ::sprintf(weight, "%.2f g", float_weight); // ORCA remove spacing before text
        m_stext_time->SetLabel(time);
        m_stext_weight->SetLabel(weight);
    }
    catch (...) {}
}

void SelectMachineDialog::update_page_turn_state(bool show)
{
     m_bitmap_last_plate->Show(show);
     m_bitmap_next_plate->Show(show);

     if (show) {
         if (m_print_plate_idx <= 0) { m_bitmap_last_plate->Disable(); }
         else { m_bitmap_last_plate->Enable(); }

         if ((m_print_plate_idx + 1) >= m_print_plate_total) { m_bitmap_next_plate->Disable(); }
         else { m_bitmap_next_plate->Enable(); }

         if (m_print_plate_total == 1) {
             m_bitmap_last_plate->Show(false);
             m_bitmap_next_plate->Show(false);
         }
     }
}

void SelectMachineDialog::sys_color_changed()
{
    if (wxGetApp(). dark_mode()) {
        //rename_button->SetIcon("ams_editable_light");
        m_rename_button->SetBitmap(rename_editable_light->bmp());

    }
    else {
        m_rename_button->SetBitmap(rename_editable->bmp());
    }
    m_rename_button->Refresh();
}

bool SelectMachineDialog::Show(bool show)
{
    if (show) {
        EnableEditing(true);
        m_options_other->Show();
        m_refresh_timer->Start(LIST_REFRESH_INTERVAL);

        //set a default machine when obj is null
        if (DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager()) {
            if (!dev->get_selected_machine()) {
                dev->load_last_machine();
            }
        };
    } else {
        m_refresh_timer->Stop();
        return DPIDialog::Show(false);
    }
    show_init();
    show_status(PrintDialogStatus::PrintStatusInit);


    PresetBundle& preset_bundle = *wxGetApp().preset_bundle;
    const auto& project_config = preset_bundle.project_config;

    const t_config_enum_values &enum_keys_map = ConfigOptionEnum<BedType>::get_enum_values();
    const ConfigOptionEnum<BedType>* bed_type=project_config.option<ConfigOptionEnum<BedType>>("curr_bed_type");
    std::string plate_name;
    for (auto& elem : enum_keys_map) {
        if (elem.second == bed_type->value)
            plate_name = elem.first;
    }

    m_printer_box->UpdatePlate(plate_name);

    // set default value when show this dialog
    wxGetApp().UpdateDlgDarkUI(this);
    wxGetApp().reset_to_active();
    set_default();
    update_user_machine_list();

    Layout();
    Fit();
    CenterOnParent();
    return DPIDialog::Show(show);
}

void SelectMachineDialog::show_init() {
    m_ams_mapping_result.clear();
}

SelectMachineDialog::~SelectMachineDialog()
{
    delete m_refresh_timer;
}

void SelectMachineDialog::UpdateStatusCheckWarning_ExtensionTool(MachineObject* obj_)
{
    if (!obj_)
    {
        return;
    }

    if (!DevPrinterConfigUtil::support_print_check_extension_fan_f000_mounted(obj_->printer_type))
    {
        return;
    }

    if (auto extension_tool = obj_->GetExtensionTool().lock())
    {
        if (extension_tool->IsToolTypeFanF000() && !extension_tool->IsMounted() )
        {
            for (const FilamentInfo& item : m_ams_mapping_result)
            {
                auto filament_info = wxGetApp().preset_bundle->get_filament_by_filament_id(item.filament_id);
                if (filament_info && (filament_info->temperature_vitrification <= 50))
                {
                    show_status(PrintDialogStatus::PrintStatusToolHeadCoolingFanWarning,
                                { _L("Install toolhead enhanced cooling fan to prevent filament softening.")},
                                "https://e.bambulab.com/t?c=l3T7caKGeNt3omA9");
                    return;
                }
            }
        }
    }
}

 ThumbnailPanel::ThumbnailPanel(wxWindow *parent, wxWindowID winid, const wxPoint &pos, const wxSize &size)
     : wxPanel(parent, winid, pos, size)
 {
#ifdef __WINDOWS__
     SetDoubleBuffered(true);
#endif //__WINDOWS__

     SetBackgroundStyle(wxBG_STYLE_CUSTOM);
     wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
     m_staticbitmap    = new wxStaticBitmap(parent, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxDefaultSize);
     m_background_bitmap = ScalableBitmap(this,"thumbnail_grid",256);
     sizer->Add(m_staticbitmap, 1, wxEXPAND, 0);
     Bind(wxEVT_PAINT, &ThumbnailPanel::OnPaint, this);
     SetSizer(sizer);
     Layout();
     Fit();
 }

 void ThumbnailPanel::set_thumbnail(wxImage &img)
 {
     m_brightness_value = get_brightness_value(img);
     m_bitmap = img;
     //Paint the background bitmap to the thumbnail bitmap with wxMemoryDC
     wxMemoryDC dc;
     bitmap_with_background.Create(wxSize(m_bitmap.GetWidth(), m_bitmap.GetHeight()));
     dc.SelectObject(bitmap_with_background);
     dc.DrawBitmap(m_background_bitmap.bmp(), 0, 0);
     dc.DrawBitmap(m_bitmap, 0, 0);
     dc.SelectObject(wxNullBitmap);
     Refresh();
 }

 void ThumbnailPanel::OnPaint(wxPaintEvent& event) {

     wxPaintDC dc(this);
     render(dc);
 }

 void ThumbnailPanel::render(wxDC& dc) {

     if (wxGetApp().dark_mode() && m_brightness_value < SHOW_BACKGROUND_BITMAP_PIXEL_THRESHOLD) {
         #ifdef __WXMSW__
             wxMemoryDC memdc;
             wxBitmap bmp(GetSize());
             memdc.SelectObject(bmp);
             memdc.DrawBitmap(bitmap_with_background, 0, 0);
             dc.Blit(0, 0, GetSize().GetWidth(), GetSize().GetHeight(), &memdc, 0, 0);
        #else
             dc.DrawBitmap(bitmap_with_background, 0, 0);
        #endif
     }
     else
         dc.DrawBitmap(m_bitmap, 0, 0);

 }

 ThumbnailPanel::~ThumbnailPanel() {}

 PrintOption::PrintOption(wxWindow *parent, wxString title, wxString tips, std::vector<POItem> ops, std::string param)
     : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
 {
#ifdef __WINDOWS__
     SetDoubleBuffered(true);
#endif //__WINDOWS__
     Bind(wxEVT_PAINT, &PrintOption::OnPaint, this);

     SetMinSize(wxSize(-1, FromDIP(28)));
     SetMaxSize(wxSize(-1, FromDIP(28)));

     m_ops = ops;
     m_param = param;
     m_full_title = title;

     SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
     wxBoxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);

     m_printoption_title = new Label(this, title);
     m_printoption_title->SetFont(Label::Body_13);

     update_title_display();

     m_printoption_tips = new ScalableButton(this, wxID_ANY, "icon_qusetion", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true);
     m_printoption_tips->SetMinSize(wxSize(FromDIP(18), FromDIP(18)));
     m_printoption_tips->SetMaxSize(wxSize(FromDIP(18), FromDIP(18)));

     m_printoption_item = new PrintOptionItem(this, m_ops, param);
     m_printoption_item->SetFont(Label::Body_13);

     sizer->Add(m_printoption_title, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, 0);
     sizer->Add(m_printoption_tips, 0, wxLEFT | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, FromDIP(2));
     sizer->AddStretchSpacer();
     sizer->Add(m_printoption_item, 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL, 0);

     SetSizer(sizer);
     Layout();
     Fit();

     update_tooltip(tips);
 }

void PrintOption::OnPaint(wxPaintEvent &event)
 {
     wxPaintDC dc(this);
     doRender(dc);
 }

void PrintOption::render(wxDC &dc)
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

void PrintOption::doRender(wxDC &dc)
{
    auto size = GetSize();
    dc.SetPen(wxPen(*wxTRANSPARENT_PEN));
    dc.SetBrush(GetBackgroundColour());
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(3));
}

void PrintOption::enable(bool en)
{
    if (m_printoption_item->is_enabled() != en)
    {
        m_printoption_item->enable(en);

        if (en) {
            m_printoption_title->SetForegroundColour(StateColor::darkModeColorFor("#262E30"));
        } else {
            m_printoption_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour(144, 144, 144)));
        }
    }
}

void PrintOption::setValue(std::string value)
{
    m_printoption_item->setValue(value); }

bool PrintOption::contain_opt(const std::string &opt) const
{
    for (const auto &op : m_ops)
    {
        if (op.key == opt)
        {
            return true;
        }
    }

    return false;
}

void PrintOption::update_options(std::vector<POItem> ops, const wxString &tips)
{
    if (m_ops != ops) {
        m_ops = ops;
        m_printoption_item->update_options(ops);
    }

    update_tooltip(tips);
}

void PrintOption::update_tooltip(const wxString &tips)
{
    if (m_printoption_tips->GetToolTipText() != tips) {
        m_printoption_tips->SetToolTip(tips);
    }

    m_printoption_tips->Show(!m_printoption_tips->GetToolTipText().IsEmpty());
}

std::string PrintOption::getValue()
{
    return m_printoption_item->getValue();
}

int PrintOption::getValueInt()
{
    if (m_printoption_item->getValue() == "off") {
        return 0;
    } else if (m_printoption_item->getValue() == "on") {
        return 1;
    } else if (m_printoption_item->getValue() == "auto") {
        return 2;
    } else {
        return 2;
    }
}

void PrintOption::update_title_display()
{
    if (!m_printoption_title || m_full_title.IsEmpty()) {
        return;
    }

    wxGCDC dc;
    wxSize titleSize = dc.GetTextExtent(m_full_title);
    int maxTitleWidth = FromDIP(150);

    wxString displayTitle = m_full_title;
    if (titleSize.x > maxTitleWidth) {
        displayTitle = wxControl::Ellipsize(m_full_title, dc, wxELLIPSIZE_END, maxTitleWidth);
        m_printoption_title->SetToolTip(m_full_title);
    } else {
        m_printoption_title->SetToolTip(wxEmptyString);
    }

    m_printoption_title->SetLabel(displayTitle);
}

void PrintOption::msw_rescale()
{
    m_printoption_item->msw_rescale();
    m_printoption_tips->msw_rescale();
    update_title_display();
}

PrintOptionItem::PrintOptionItem(wxWindow* parent, std::vector<POItem> ops, std::string param)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    m_param = param;
    SetBackgroundColour(PRINT_OPT_ITEM_BG_GRAY);

    Bind(wxEVT_PAINT, &PrintOptionItem::OnPaint, this);
    Bind(wxEVT_ENTER_WINDOW, [this](auto& e) { SetCursor(wxCURSOR_HAND); });
    Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) { SetCursor(wxCURSOR_ARROW); });
    Bind(wxEVT_LEFT_DOWN, &PrintOptionItem::on_left_down, this);

    m_selected_bk = ScalableBitmap(this, "print_options_bg", 22);
    m_selected_bk_dark = ScalableBitmap(this, "print_options_bg_dark", 22);
    m_selected_disabled_bk = ScalableBitmap(this, "print_options_bg_disabled", 22);
    m_selected_disabled_bk_dark = ScalableBitmap(this, "print_options_bg_disabled_dark", 22);

    // update the options
    update_options(ops);
}

void PrintOptionItem::OnPaint(wxPaintEvent& event)
{
    wxPaintDC dc(this);
    doRender(dc);
}

void PrintOptionItem::render(wxDC& dc)
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

void PrintOptionItem::on_left_down(wxMouseEvent& evt)
{
    if (!m_enable)
    {
        return;
    }

    auto pos = ClientToScreen(evt.GetPosition());
    auto rect = ClientToScreen(wxPoint(0, 0));
    auto select_size = GetSize().x / m_ops.size();

    int i = 0;
    for (const auto& entry : m_ops)
    {
        auto left_edge = rect.x + i * select_size;
        auto right_edge = rect.x + (i + 1) * select_size;

        if (pos.x > left_edge && pos.x < right_edge)
        {
            selected_key = entry.key;
        }
        i++;
    }

    wxCommandEvent event(EVT_SWITCH_PRINT_OPTION);
    event.SetString(selected_key);
    event.SetEventObject(GetParent());
    wxPostEvent(GetParent(), event);

    Refresh();
}

void PrintOptionItem::doRender(wxDC& dc)
{
    auto size = GetSize();
    dc.SetPen(wxPen(*wxTRANSPARENT_PEN));
    dc.SetBrush(GetBackgroundColour());
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(5));

    auto left = FromDIP(4);

    int selected = 0;
    for (const auto& entry : m_ops)
    {
        if (entry.key == selected_key)
        {
            break;
        }
        selected++;
    }

    /*selected*/
    auto selected_left = selected * PRINT_OPT_WIDTH + FromDIP(4);
    if (m_enable) {
        if (!wxGetApp().dark_mode()) {
            dc.DrawBitmap(m_selected_bk.bmp(), selected_left, (size.y - m_selected_bk.GetBmpHeight()) / 2);
        } else {
            dc.DrawBitmap(m_selected_bk_dark.bmp(), selected_left, (size.y - m_selected_bk_dark.GetBmpHeight()) / 2);
        }

    } else {
        if (!wxGetApp().dark_mode()) {
            dc.DrawBitmap(m_selected_disabled_bk.bmp(), selected_left, (size.y - m_selected_disabled_bk.GetBmpHeight()) / 2);
        } else {
            dc.DrawBitmap(m_selected_bk_dark.bmp(), selected_left, (size.y - m_selected_bk_dark.GetBmpHeight()) / 2);
        }
    }

    for (auto it = m_ops.begin(); it != m_ops.end(); ++it)
    {
        auto text_key = it->key;
        auto text_value = it->value;

        if (text_key == selected_key)
        {
            const wxColour& clr = m_enable ? StateColor::darkModeColorFor("#009688") : StateColor::darkModeColorFor(wxColour(144, 144, 144));
            dc.SetPen(wxPen(clr));
            dc.SetTextForeground(clr);

            dc.SetFont(::Label::Body_13);
            auto text_size = dc.GetTextExtent(text_value);
            auto text_left = left + (PRINT_OPT_WIDTH - text_size.x) / 2;
            auto text_top = (size.y - text_size.y) / 2;
            dc.DrawText(text_value, wxPoint(text_left, text_top));
        }
        else
        {
            const wxColour& clr = m_enable ? StateColor::darkModeColorFor("#262E30") : StateColor::darkModeColorFor(wxColour(144, 144, 144));
            dc.SetPen(wxPen(clr));
            dc.SetTextForeground(clr);

            dc.SetFont(::Label::Body_13);
            auto text_size = dc.GetTextExtent(text_value);
            auto text_left = left + (PRINT_OPT_WIDTH - text_size.x) / 2;
            auto text_top = (size.y - text_size.y) / 2;
            dc.DrawText(text_value, wxPoint(text_left, text_top));
        }

        left += PRINT_OPT_WIDTH;
    }
}

void PrintOptionItem::setValue(std::string value)
{
    if (selected_key != value) {
        selected_key = value;
        Refresh();
    }
}

 SendModeSwitchButton::SendModeSwitchButton(wxWindow *parent, wxString mode, bool sel)
     : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
 {
#ifdef __WINDOWS__
     SetDoubleBuffered(true);
#endif //__WINDOWS__

     is_selected = sel;
     SetLabel(mode);
     Bind(wxEVT_PAINT, &SendModeSwitchButton::OnPaint, this);
     Bind(wxEVT_ENTER_WINDOW, [this](auto &e) { SetCursor(wxCURSOR_HAND); });
     Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) { SetCursor(wxCURSOR_ARROW); });

     SetBackgroundColour(*wxWHITE);
     static Slic3r::GUI::BitmapCache cache;
     m_img_selected       = ScalableBitmap(this, "switch_send_mode_on", 28);
     m_img_unselected     = ScalableBitmap(this, "switch_send_mode_off", 28);
     m_img_selected_tag   = ScalableBitmap(this, "switch_send_mode_tag_on", 16);
     m_img_unselected_tag = ScalableBitmap(this, "switch_send_mode_tag_off", 16);

     auto tagSize = wxSize(FromDIP(185), FromDIP(28));

     SetMinSize(tagSize);
     SetMaxSize(tagSize);
     wxGetApp().UpdateDarkUIWin(this);
 }

void SendModeSwitchButton::OnPaint(wxPaintEvent &event)
{
    wxPaintDC dc(this);
    doRender(dc);
}

void SendModeSwitchButton::render(wxDC &dc)
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

void SendModeSwitchButton::doRender(wxDC &dc)
{
    dc.SetPen(wxPen(*wxTRANSPARENT_PEN));
    dc.SetBrush(wxBrush(*wxTRANSPARENT_BRUSH));

    auto size = GetSize();
    auto textSize = dc.GetTextExtent(GetLabel());
    auto left =(size.x - textSize.x - m_img_selected_tag.GetBmpSize().x - FromDIP(8)) / 2;

    dc.SetFont(::Label::Head_14);

    if (is_selected) {
        dc.DrawBitmap(m_img_selected.bmp(), wxPoint(0, 0));
        dc.DrawBitmap(m_img_selected_tag.bmp(), wxPoint(left, (size.y - m_img_selected_tag.GetBmpSize().y) / 2));
        dc.SetTextForeground("#009688");
    }else {
        dc.DrawBitmap(m_img_unselected.bmp(), wxPoint(0, 0));
        dc.DrawBitmap(m_img_unselected_tag.bmp(), wxPoint(left, (size.y - m_img_selected_tag.GetBmpSize().y) / 2));
        dc.SetTextForeground(0x5C5C5C);
    }
    dc.DrawText(GetLabel(), left + m_img_selected_tag.GetBmpSize().x + FromDIP(8), (size.y - textSize.y) / 2);
}

void SendModeSwitchButton::on_left_down(wxMouseEvent &evt)
{
   setSelected(!is_selected);
}

void SendModeSwitchButton::msw_rescale()
{
    m_img_selected.msw_rescale();
    m_img_unselected.msw_rescale();
    m_img_selected_tag.msw_rescale();
    m_img_unselected_tag.msw_rescale();
    Refresh();
}

void SendModeSwitchButton::setSelected(bool selected)
{
    if (is_selected != selected)
    {
        is_selected = selected;
        Refresh();
    }
}

PrinterInfoBox::PrinterInfoBox(wxWindow* parent, SelectMachineDialog* select_dialog)
    : StaticBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNO_BORDER), m_select_dialog(select_dialog)
{
    Create();
}

void PrinterInfoBox::UpdatePlate(const std::string& plate_name)
{
    if (plate_name.empty())
    {
        //m_text_bed_type->Hide();
    }
    else
    {
        wxString name;
        if (plate_name == "Cool Plate") {
            name = _L("Smooth Cool Plate");
            m_bed_image->SetBitmap(create_scaled_bitmap("bed_cool", this, 40));
        }
        else if (plate_name == "Engineering Plate") {
            name = _L("Engineering Plate");
            m_bed_image->SetBitmap(create_scaled_bitmap("bed_engineering", this, 40));
        }
        else if (plate_name == "High Temp Plate") {
            name = _L("Smooth High Temp Plate");
            m_bed_image->SetBitmap(create_scaled_bitmap("bed_high_templ", this, 40));
        }
        else if (plate_name == "Textured PEI Plate") {
            name = _L("Textured PEI Plate");
            m_bed_image->SetBitmap(create_scaled_bitmap("bed_pei", this, 40));
        }
        else if (plate_name == "Supertack Plate") {
            name = _L("Cool Plate (Supertack)");
            m_bed_image->SetBitmap(create_scaled_bitmap("bed_cool_supertack", this, 40));
        }

        m_bed_image->SetToolTip(name);

        //if (name.length() > 8) {
        //    m_text_bed_type->SetFont(Label::Body_9);
        //}
        //else{
        //    m_text_bed_type->SetFont(Label::Body_12);
        //}
        //m_text_bed_type->SetLabelText(name);
        //m_text_bed_type->Show();
    }
}

static wxString _get_tips(MachineObject* obj_)
{
    wxString tips;
    tips = obj_->get_printer_type_display_str();

    wxString ext_diameter;
    if (obj_->GetExtderSystem()->GetTotalExtderCount() == 1) {
        ext_diameter += wxString::FromDouble(obj_->GetExtderSystem()->GetNozzleDiameter(0));
        ext_diameter += "mm";
    } else if (obj_->GetExtderSystem()->GetTotalExtderCount() == 2) {
        ext_diameter += wxString::FromDouble(obj_->GetExtderSystem()->GetNozzleDiameter(1));//Left
        ext_diameter += "/";
        ext_diameter += wxString::FromDouble(obj_->GetExtderSystem()->GetNozzleDiameter(0));
        ext_diameter += "mm";
    } else {
        assert(0);
    }

    if (!ext_diameter.empty()) {
        tips += "  ";
        tips += ext_diameter;
    }

    return tips;
}

void PrinterInfoBox::SetPrinters(const std::vector<MachineObject*>& sorted_printers)
{
    m_comboBox_printer->Clear();

    std::vector<DropDown::Item> drop_items;
    for (MachineObject* obj : sorted_printers)
    {
        wxString shown_dev_name = wxString::FromUTF8(obj->get_dev_name());
        if (obj->is_lan_mode_printer()) {
            shown_dev_name += "(LAN)";
        }

        DropDown::Item drop_item;
        drop_item.text = shown_dev_name;
        drop_item.text_static_tips = _get_tips(obj);

        // update image
        try
        {
            drop_item.icon = create_scaled_bitmap("printer_preview_" + obj->printer_type, this, 32);
            drop_item.icon_textctrl = create_scaled_bitmap("printer_preview_" + obj->printer_type, this, 52);
        }
        catch (const std::exception&)
        {
            drop_item.icon = create_scaled_bitmap("printer_preview_BL-P001", this, 32);
            drop_item.icon_textctrl = create_scaled_bitmap("printer_preview_BL-P001", this, 52);
        }

        drop_item.tip = obj->get_printer_type_display_str();
        drop_items.emplace_back(drop_item);
    }

    m_comboBox_printer->SetItems(drop_items);
}

void PrinterInfoBox::EnableEditing(bool enable)
{
    m_comboBox_printer->Enable(enable);
    m_button_refresh->Enable(enable);
}

void PrinterInfoBox::EnableRefreshButton(bool enable)
{
    if (m_button_refresh->IsEnabled() != enable) {
        m_button_refresh->Enable(enable);
    }
}

void PrinterInfoBox::SetDefault(bool from_sd)
{
    if (!from_sd)
    {
        m_comboBox_printer->Show(true);
        m_button_refresh->Show(true);
    }
    else
    {
        m_comboBox_printer->Show(true);
        m_button_refresh->Show(false);
    }

    m_comboBox_printer->Clear();
    m_comboBox_printer->SetValue(wxEmptyString);
    m_comboBox_printer->Enable();
}

void PrinterInfoBox::Create()
{
    /*printer combobox*/
    wxBoxSizer* sizer_split_printer = new wxBoxSizer(wxHORIZONTAL);
    m_stext_printer_title = new Label(this, _L("Printer"), wxALIGN_TOP);
    m_stext_printer_title->SetFont(::Label::Head_13);
    m_stext_printer_title->SetForegroundColour(0x909090);

    m_button_question = new ScalableButton(this, wxID_ANY, "icon_qusetion", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true);
    m_button_question->Bind(wxEVT_BUTTON, &PrinterInfoBox::OnBtnQuestionClicked, this);
    m_button_question->SetToolTip(_L("Click here if you can't connect to the printer"));
    m_button_question->SetMinSize(wxSize(FromDIP(18), FromDIP(18)));
    m_button_question->SetMaxSize(wxSize(FromDIP(18), FromDIP(18)));

    auto m_split_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_split_line->SetBackgroundColour(0xeeeeee);
    m_split_line->SetMinSize(wxSize(-1, 1));
    m_split_line->SetMaxSize(wxSize(-1, 1));
    sizer_split_printer->Add(0, 0, 0, wxEXPAND, 0);
    sizer_split_printer->Add(m_stext_printer_title, 0, wxALIGN_CENTER_VERTICAL, 0);
    sizer_split_printer->AddSpacer(FromDIP(5));
    sizer_split_printer->Add(m_button_question, 0, wxALIGN_CENTER_VERTICAL, 0);
    sizer_split_printer->Add(m_split_line, 1, wxALIGN_CENTER_VERTICAL, 0);

    wxBoxSizer* sizer_printer_area = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* sizer_bed_staticbox = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* sizer_printer_staticbox = new wxBoxSizer(wxHORIZONTAL);

    /*printer area*/
    auto printer_staticbox = new StaticBox(this);
    printer_staticbox->SetMinSize(wxSize(FromDIP(338), FromDIP(68)));
    printer_staticbox->SetMaxSize(wxSize(FromDIP(338), FromDIP(68)));
    printer_staticbox->SetBorderColor(wxColour("#CECECE"));

    m_comboBox_printer = new ComboBox(printer_staticbox, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
    m_comboBox_printer->SetBorderWidth(0);
    m_comboBox_printer->SetMinSize(wxSize(FromDIP(300), FromDIP(60)));
    m_comboBox_printer->SetMaxSize(wxSize(FromDIP(300), FromDIP(60)));
    m_comboBox_printer->SetBackgroundColor(*wxWHITE);
    m_comboBox_printer->Bind(wxEVT_COMBOBOX, &SelectMachineDialog::on_selection_changed, m_select_dialog);

    m_button_refresh = new ScalableButton(printer_staticbox, wxID_ANY, "refresh_printer", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true);
    m_button_refresh->Bind(wxEVT_BUTTON, &SelectMachineDialog::on_refresh, m_select_dialog);

    sizer_printer_staticbox->Add(0, 0, 0, wxLEFT, FromDIP(7));
    sizer_printer_staticbox->Add(m_comboBox_printer, 0, wxALIGN_CENTER, 0);
    sizer_printer_staticbox->Add(m_button_refresh, 0, wxALIGN_CENTER, 0);

    printer_staticbox->SetSizer(sizer_printer_staticbox);
    printer_staticbox->Layout();
    printer_staticbox->Fit();

    /*bed area*/
    auto bed_staticbox = new StaticBox(this);
    bed_staticbox->SetMinSize(wxSize(FromDIP(68), FromDIP(68)));
    bed_staticbox->SetMaxSize(wxSize(FromDIP(68), FromDIP(68)));
    bed_staticbox->SetBorderColor(wxColour("#EEEEEE"));

    m_bed_image = new wxStaticBitmap(bed_staticbox, wxID_ANY, create_scaled_bitmap("bed_cool", this, 40));
    m_bed_image->SetBackgroundColour(*wxWHITE);
    m_bed_image->SetMinSize(wxSize(FromDIP(40), FromDIP(40)));
    m_bed_image->SetMaxSize(wxSize(FromDIP(40), FromDIP(40)));

    //m_text_bed_type = new Label(bed_staticbox);
    //m_text_bed_type->SetForegroundColour(wxColour(144, 144, 144));
    //m_text_bed_type->SetMaxSize(wxSize(FromDIP(80), FromDIP(24)));
    //m_text_bed_type->SetFont(Label::Body_13);

    //sizer_bed_staticbox->Add(0, 0, 0, wxTOP, FromDIP(10));
    sizer_bed_staticbox->AddStretchSpacer();
    sizer_bed_staticbox->Add(m_bed_image, 0, wxALIGN_CENTER, 0);
    sizer_bed_staticbox->AddStretchSpacer();
    //sizer_bed_staticbox->AddSpacer(FromDIP(6));
    //sizer_bed_staticbox->Add(m_text_bed_type, 0, wxALIGN_CENTER, 0);

    bed_staticbox->SetSizer(sizer_bed_staticbox);
    bed_staticbox->Layout();
    bed_staticbox->Fit();

    sizer_printer_area->Add(printer_staticbox, 0, wxALIGN_CENTER, 0);
    sizer_printer_area->Add(0, 0, 0, wxLEFT, FromDIP(4));
    sizer_printer_area->Add(bed_staticbox, 0, wxALIGN_CENTER, 0);


    wxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    //main_sizer->Add(m_text_bed_type, 0, wxTOP, 0);
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    main_sizer->Add(sizer_split_printer, 1, wxEXPAND, 0);
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(8));
    main_sizer->Add(sizer_printer_area, 0, wxTOP, 0);
    SetSizer(main_sizer);
}

void PrinterInfoBox::OnBtnQuestionClicked(wxCommandEvent& event)
{
    wxLaunchDefaultBrowser(wxT("https://wiki.bambulab.com/en/software/bambu-studio/failed-to-connect-printer"));
}


}
} // namespace Slic3r::GUI
