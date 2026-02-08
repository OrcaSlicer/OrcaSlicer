#include "UpgradePanel.hpp"
#include <slic3r/GUI/Widgets/SideTools.hpp>
#include <slic3r/GUI/Widgets/Label.hpp>
#include <slic3r/GUI/I18N.hpp>
#include "slic3r/GUI/DeviceTab/uiDeviceUpdateVersion.h"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "libslic3r/Thread.hpp"

#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevManager.h"

namespace Slic3r {
namespace GUI {

static const wxColour TEXT_NORMAL_CLR = wxColour(0, 150, 136);
static const wxColour TEXT_FAILED_CLR = wxColour(255, 111, 0);

static const std::unordered_map<wxString, wxString> ACCESSORY_DISPLAY_STR = {
    {"N3F", "AMS 2 PRO"},
    {"N3S", "AMS HT"},
    {"O2L_PC", L("Air Pump")},
    {"O2L_10B", L("Laser 10W")},
    {"O2L_40B", L("Laser 40W")},
    {"O2L_PCM", L("Cutting Module")},
    {"O2L_ACM", "Active Cutting Module"},
    {"O2L_UCM", "Ultrasonic Cutting Module"},
    {"O2L-AFP", L("Auto Fire Extinguishing System")},
};

enum FIRMWARE_STASUS
{
    UNKOWN,
    TESTING,
    BETA,
    RELEASE,
};

MachineInfoPanel::MachineInfoPanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name)
    :wxPanel(parent, id, pos, size, style)
{
    this->SetBackgroundColour(wxColour(255, 255, 255));

    init_bitmaps();

    wxBoxSizer *m_top_sizer = new wxBoxSizer(wxVERTICAL);

    m_panel_caption = create_caption_panel(this);

    m_top_sizer->Add(m_panel_caption, 0, wxEXPAND | wxALL, 0);

    wxBoxSizer *m_main_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *m_main_left_sizer = new wxBoxSizer(wxVERTICAL);


    // ota
    wxBoxSizer *m_ota_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_printer_img = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(200), FromDIP(200)));

    m_printer_img->SetBitmap(m_img_printer.bmp());
    m_ota_sizer->Add(m_printer_img, 0, wxALIGN_TOP | wxALL, FromDIP(5));

    wxBoxSizer *m_ota_content_sizer = new wxBoxSizer(wxVERTICAL);

    m_ota_content_sizer->Add(0, 0, 1, wxEXPAND, 0);

    wxFlexGridSizer *m_ota_info_sizer = new wxFlexGridSizer(0, 2, 0, 0);
    m_ota_info_sizer->AddGrowableCol(1);
    m_ota_info_sizer->SetFlexibleDirection(wxHORIZONTAL);
    m_ota_info_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

    m_staticText_model_id = new wxStaticText(this, wxID_ANY, _L("Model:"), wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_model_id->Wrap(-1);
    m_staticText_model_id->SetFont(Label::Head_14);
    m_ota_info_sizer->Add(m_staticText_model_id, 0, wxALIGN_RIGHT | wxALL, FromDIP(5));

    m_staticText_model_id_val = new wxStaticText(this, wxID_ANY, "-", wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_model_id_val->Wrap(-1);
    m_ota_info_sizer->Add(m_staticText_model_id_val, 0, wxALL | wxEXPAND, FromDIP(5));

    m_staticText_sn = new wxStaticText(this, wxID_ANY, _L("Serial:"), wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_sn->Wrap(-1);
    m_staticText_sn->SetFont(Label::Head_14);
    m_ota_info_sizer->Add(m_staticText_sn, 0, wxALIGN_RIGHT | wxALL | wxEXPAND, FromDIP(5));

    m_staticText_sn_val = new wxStaticText(this, wxID_ANY, "-", wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_sn_val->Wrap(-1);
    m_ota_info_sizer->Add(m_staticText_sn_val, 0, wxALL | wxEXPAND, FromDIP(5));

    wxBoxSizer *m_ota_ver_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_ota_ver_sizer->Add(0, 0, 1, wxEXPAND, 0);

    m_ota_new_version_img = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(5), FromDIP(5)));
    m_ota_new_version_img->SetBitmap(upgrade_green_icon.bmp());
    m_ota_ver_sizer->Add(m_ota_new_version_img, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    m_staticText_ver = new wxStaticText(this, wxID_ANY, _L("Version:"), wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_ver->Wrap(-1);
    m_staticText_ver->SetFont(Label::Head_14);
    m_ota_ver_sizer->Add(m_staticText_ver, 0, wxALL, FromDIP(5));

    wxBoxSizer* m_ota_content_sizer2 = new wxBoxSizer(wxHORIZONTAL);

    m_staticText_ver_val = new wxStaticText(this, wxID_ANY, "-", wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_ver_val->Wrap(-1);


    m_staticText_beta_version = new wxStaticText(this, wxID_ANY, "Beta", wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_beta_version->SetForegroundColour("#778899");
    m_staticText_beta_version->Wrap(-1);
    m_staticText_beta_version->Hide();

    m_ota_content_sizer2->Add(m_staticText_ver_val, 0, wxALL|wxEXPAND, FromDIP(5));
    m_ota_content_sizer2->Add(m_staticText_beta_version, 0, wxALL | wxEXPAND, FromDIP(5));

    m_ota_info_sizer->Add(m_ota_ver_sizer, 0, wxEXPAND, 0);
    m_ota_info_sizer->Add(m_ota_content_sizer2, 0,  wxEXPAND, 0);

    m_ota_content_sizer->Add(m_ota_info_sizer, 0, wxEXPAND, 0);

    m_ota_content_sizer->Add(0, 0, 1, wxEXPAND, 0);

    m_ota_sizer->Add(m_ota_content_sizer, 1, wxEXPAND, 0);

    m_main_left_sizer->Add(m_ota_sizer, 0, wxEXPAND, 0);

    m_staticline = new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL);
    m_staticline->SetBackgroundColour(wxColour(206,206,206));
    m_staticline->Show(false);
    m_main_left_sizer->Add(m_staticline, 0, wxEXPAND | wxLEFT, FromDIP(40));


    // ams
    m_ams_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_ams_img   = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(200), FromDIP(200)));
    m_ams_img->SetBitmap(m_img_monitor_ams.bmp());
    m_ams_sizer->Add(m_ams_img, 0, wxALIGN_TOP | wxALL, FromDIP(5));

    wxBoxSizer *m_ams_content_sizer = new wxBoxSizer(wxVERTICAL);
    m_ams_content_sizer->Add(0, 40, 0, wxEXPAND, FromDIP(5));


    m_ahb_panel = new AmsPanel(this, wxID_ANY);
    m_ams_content_sizer->Add(m_ahb_panel, 0, wxEXPAND, 0);


    m_ams_info_sizer = new wxFlexGridSizer(0, 2, FromDIP(30), FromDIP(30));
    m_ams_info_sizer->SetFlexibleDirection(wxHORIZONTAL);
    m_ams_info_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_ALL);

    //for (auto i = 0; i < 4; i++) {
    //    auto amspanel = new AmsPanel(this, wxID_ANY);
    //    m_ams_info_sizer->Add(amspanel, 1, wxEXPAND, 5);
    //    amspanel->Hide();

    //    /*AmsPanelItem item = AmsPanelItem();
    //    item.id           = i;
    //    item.item         = amspanel;*/
    //    m_amspanel_list.Add(amspanel);
    //}

    m_ams_content_sizer->Add(m_ams_info_sizer, 0, wxEXPAND, 0);
    m_ams_sizer->Add(m_ams_content_sizer, 1, wxEXPAND, 0);

    m_main_left_sizer->Add(m_ams_sizer, 0, wxEXPAND, 0);

    //Hide ams
    show_ams(false, true);

    //
    m_extra_ams_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_extra_ams_img = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(200), FromDIP(200)));
    m_extra_ams_img->SetBitmap(m_img_extra_ams.bmp());

    m_extra_ams_sizer->Add(m_extra_ams_img, 0, wxALIGN_TOP | wxALL, FromDIP(5));

    wxBoxSizer* extra_ams_content_sizer = new wxBoxSizer(wxVERTICAL);
    extra_ams_content_sizer->Add(0, 40, 0, wxEXPAND, FromDIP(5));
    m_extra_ams_panel = new ExtraAmsPanel(this);
    extra_ams_content_sizer->Add(m_extra_ams_panel, 0, wxEXPAND, 0);

    m_extra_ams_sizer->Add(extra_ams_content_sizer, 1, wxEXPAND, 0);

    m_main_left_sizer->Add(m_extra_ams_sizer, 0, wxEXPAND, 0);

    show_extra_ams(false, true);

    m_staticline2 = new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL);
    m_staticline2->SetBackgroundColour(wxColour(206, 206, 206));
    m_main_left_sizer->Add(m_staticline2, 0, wxEXPAND | wxLEFT, FromDIP(40));

    // ext
    m_ext_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_ext_img = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(200), FromDIP(200)));
    m_ext_img->SetBitmap(m_img_ext.bmp());

    m_ext_sizer->Add(m_ext_img, 0, wxALIGN_TOP | wxALL, FromDIP(5));

    wxBoxSizer* ext_content_sizer = new wxBoxSizer(wxVERTICAL);
    ext_content_sizer->Add(0, 40, 0, wxEXPAND, FromDIP(5));
    m_ext_panel = new ExtensionPanel(this, wxID_ANY);
    ext_content_sizer->Add(m_ext_panel, 0, wxEXPAND, 0);

    m_ext_sizer->Add(ext_content_sizer, 1, wxEXPAND, 0);

    m_main_left_sizer->Add(m_ext_sizer, 0, wxEXPAND, 0);

    /* cutting module */
    createCuttingWidgets(m_main_left_sizer);
    createLaserWidgets(m_main_left_sizer);
    createAirPumpWidgets(m_main_left_sizer);
    createExtinguishWidgets(m_main_left_sizer);

    m_main_sizer->Add(m_main_left_sizer, 1, wxEXPAND, 0);

    wxBoxSizer *m_main_right_sizer = new wxBoxSizer(wxVERTICAL);

    m_main_right_sizer->SetMinSize(wxSize(FromDIP(137), -1));

    m_main_right_sizer->Add(0, FromDIP(50), 0, wxEXPAND, FromDIP(5));

    m_button_upgrade_firmware = new Button(this, _L("Update firmware"));
    m_button_upgrade_firmware->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_main_right_sizer->Add(m_button_upgrade_firmware, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(5));

    m_staticText_upgrading_info = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0);
    m_staticText_upgrading_info->Wrap(-1);
    m_main_right_sizer->Add(m_staticText_upgrading_info, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(5));

    m_upgrading_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_upgrading_sizer->Add(0, 0, 1, wxEXPAND, 0);

    m_upgrade_progress = new ProgressBar(this, wxID_ANY, 100, wxDefaultPosition, wxDefaultSize);
    m_upgrade_progress->SetValue(0);
    m_upgrade_progress->SetSize(wxSize(FromDIP(54), FromDIP(14)));
    m_upgrade_progress->SetMinSize(wxSize(FromDIP(54), FromDIP(14)));
    m_upgrading_sizer->Add(m_upgrade_progress, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    m_staticText_upgrading_percent = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize);
    m_staticText_upgrading_percent->Wrap(-1);
    m_upgrading_sizer->Add(m_staticText_upgrading_percent, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    m_upgrade_retry_img = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxDefaultSize);
    m_upgrading_sizer->Add(m_upgrade_retry_img, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    m_upgrading_sizer->Add(0, 0, 1, wxEXPAND, 0);

    m_main_right_sizer->Add(m_upgrading_sizer, 0, wxEXPAND, 0);

    wxBoxSizer *sizer_release_note = new wxBoxSizer(wxVERTICAL);


    m_staticText_release_note = new wxStaticText(this, wxID_ANY, _L("Release Note"), wxDefaultPosition, wxDefaultSize);
    m_staticText_release_note->Wrap(-1);
    m_staticText_release_note->SetForegroundColour(wxColour(0x1F,0x8E,0xEA));

    auto line_release_note = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    line_release_note->SetBackgroundColour(wxColour(0x1F, 0x8E, 0xEA));

    sizer_release_note->Add(m_staticText_release_note, 0, wxALL, 0);
    sizer_release_note->Add(line_release_note, 1, wxEXPAND | wxALL, 0);

    m_main_right_sizer->Add(sizer_release_note, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, 0);

    m_main_right_sizer->Add(0, 0, 1, wxEXPAND, 0);

    m_main_sizer->Add(m_main_right_sizer, 0, wxEXPAND, 0);

    m_top_sizer->Add(m_main_sizer, 1, wxEXPAND, 0);

    this->SetSizer(m_top_sizer);
    this->Layout();

    // Connect Events
    m_upgrade_retry_img->Bind(wxEVT_LEFT_UP, [this](auto &e) {
        upgrade_firmware_internal();
        });

    m_staticText_release_note->Bind(wxEVT_LEFT_DOWN, &MachineInfoPanel::on_show_release_note, this);
    m_button_upgrade_firmware->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(MachineInfoPanel::on_upgrade_firmware), NULL, this);
    wxGetApp().UpdateDarkUIWin(this);
}


wxPanel *MachineInfoPanel::create_caption_panel(wxWindow *parent)
{
    auto caption_panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    caption_panel->SetBackgroundColour(wxColour(248, 248, 248));
    caption_panel->SetMinSize(wxSize(FromDIP(925), FromDIP(36)));

    wxBoxSizer *m_caption_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_caption_sizer->Add(17, 0, 0, wxEXPAND, 0);

    m_upgrade_status_img = new wxStaticBitmap(caption_panel, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(5), FromDIP(5)));
    m_upgrade_status_img->SetBitmap(upgrade_gray_icon.bmp());
    m_upgrade_status_img->Hide();
    m_caption_sizer->Add(m_upgrade_status_img, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    m_caption_text = new wxStaticText(caption_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize);
    m_caption_text->SetForegroundColour("#262E30");
    m_caption_text->Wrap(-1);
    m_caption_sizer->Add(m_caption_text, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    caption_panel->SetSizer(m_caption_sizer);
    caption_panel->Layout();
    m_caption_sizer->Fit(caption_panel);

    return caption_panel;
}

void MachineInfoPanel::createAirPumpWidgets(wxBoxSizer* main_left_sizer)
{
    m_air_pump_line_above = new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL);
    m_air_pump_line_above->SetBackgroundColour(wxColour(206, 206, 206));
    main_left_sizer->Add(m_air_pump_line_above, 0, wxEXPAND | wxLEFT, FromDIP(40));

    m_air_pump_img = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(200), FromDIP(200)));
    m_air_pump_img->SetBitmap(m_img_air_pump.bmp());

    wxBoxSizer* content_sizer = new wxBoxSizer(wxVERTICAL);
    content_sizer->Add(0, 40, 0, wxEXPAND, FromDIP(5));
    m_air_pump_version = new uiDeviceUpdateVersion(this, wxID_ANY);
    content_sizer->Add(m_air_pump_version, 0, wxEXPAND, 0);

    m_air_pump_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_air_pump_sizer->Add(m_air_pump_img, 0, wxALIGN_TOP | wxALL, FromDIP(5));
    m_air_pump_sizer->Add(content_sizer, 1, wxEXPAND, 0);

    main_left_sizer->Add(m_air_pump_sizer, 0, wxEXPAND, 0);
}

void MachineInfoPanel::createCuttingWidgets(wxBoxSizer* main_left_sizer)
{
    m_cutting_line_above = new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL);
    m_cutting_line_above->SetBackgroundColour(wxColour(206, 206, 206));
    main_left_sizer->Add(m_cutting_line_above, 0, wxEXPAND | wxLEFT, FromDIP(40));

    m_cutting_img = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(200), FromDIP(200)));
    m_cutting_img->SetBitmap(m_img_cutting.bmp());

    wxBoxSizer* content_sizer = new wxBoxSizer(wxVERTICAL);
    content_sizer->Add(0, 40, 0, wxEXPAND, FromDIP(5));
    m_cutting_version = new uiDeviceUpdateVersion(this, wxID_ANY);
    content_sizer->Add(m_cutting_version, 0, wxEXPAND, 0);

    m_cutting_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cutting_sizer->Add(m_cutting_img, 0, wxALIGN_TOP | wxALL, FromDIP(5));
    m_cutting_sizer->Add(content_sizer, 1, wxEXPAND, 0);

    main_left_sizer->Add(m_cutting_sizer, 0, wxEXPAND, 0);
};

void MachineInfoPanel::createLaserWidgets(wxBoxSizer* main_left_sizer)
{
    m_laser_line_above = new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL);
    m_laser_line_above->SetBackgroundColour(wxColour(206, 206, 206));
    main_left_sizer->Add(m_laser_line_above, 0, wxEXPAND | wxLEFT, FromDIP(40));

    m_lazer_img = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(200), FromDIP(200)));


    m_lazer_img->SetBitmap(m_img_laser.bmp());

    wxBoxSizer* content_sizer = new wxBoxSizer(wxVERTICAL);
    content_sizer->Add(0, 40, 0, wxEXPAND, FromDIP(5));
    m_laser_version = new uiDeviceUpdateVersion(this, wxID_ANY);
    content_sizer->Add(m_laser_version, 0, wxEXPAND, 0);

    m_laser_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_laser_sizer->Add(m_lazer_img, 0, wxALIGN_TOP | wxALL, FromDIP(5));
    m_laser_sizer->Add(content_sizer, 1, wxEXPAND, 0);

    main_left_sizer->Add(m_laser_sizer, 0, wxEXPAND, 0);
}

void MachineInfoPanel::createExtinguishWidgets(wxBoxSizer* main_left_sizer)
{
    m_extinguish_line_above = new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL);
    m_extinguish_line_above->SetBackgroundColour(wxColour(206, 206, 206));
    main_left_sizer->Add(m_extinguish_line_above, 0, wxEXPAND | wxLEFT, FromDIP(40));

    m_extinguish_img = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(200), FromDIP(200)));
    m_extinguish_img->SetBitmap(m_img_extinguish.bmp());

    wxBoxSizer* content_sizer = new wxBoxSizer(wxVERTICAL);
    content_sizer->Add(0, 40, 0, wxEXPAND, FromDIP(5));
    m_extinguish_version = new uiDeviceUpdateVersion(this, wxID_ANY);
    content_sizer->Add(m_extinguish_version, 0, wxEXPAND, 0);

    m_extinguish_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_extinguish_sizer->Add(m_extinguish_img, 0, wxALIGN_TOP | wxALL, FromDIP(5));
    m_extinguish_sizer->Add(content_sizer, 1, wxEXPAND, 0);

    main_left_sizer->Add(m_extinguish_sizer, 0, wxEXPAND, 0);
}

void MachineInfoPanel::msw_rescale()
{
    rescale_bitmaps();
    m_button_upgrade_firmware->SetSize(wxSize(FromDIP(-1), FromDIP(24)));
    m_button_upgrade_firmware->SetMinSize(wxSize(FromDIP(-1), FromDIP(24)));
    m_button_upgrade_firmware->SetMaxSize(wxSize(FromDIP(-1), FromDIP(24)));
    m_button_upgrade_firmware->SetCornerRadius(FromDIP(12));
    m_ahb_panel->msw_rescale();
    for (auto &amspanel : m_amspanel_list) {
        amspanel->msw_rescale();
    }
    m_ext_panel->msw_rescale();
    Layout();
    Fit();
}

void MachineInfoPanel::init_bitmaps()
{
    try {
        m_img_printer     = ScalableBitmap(this, "printer_thumbnail_png", 160);
        m_img_monitor_ams = ScalableBitmap(this, "monitor_upgrade_ams_png", 150);
        m_img_ext         = ScalableBitmap(this, "monitor_upgrade_ext", 160);

        m_img_air_pump  = ScalableBitmap(this, "air_pump", 160);
        m_img_extra_ams = ScalableBitmap(this, "extra_icon_png", 160);

        m_img_laser          = ScalableBitmap(this, "laser", 160);
        m_img_cutting        = ScalableBitmap(this, "cut", 160);
        m_img_extinguish     = ScalableBitmap(this, "extinguish", 160);

        upgrade_green_icon   = ScalableBitmap(this, "monitor_upgrade_online", 5);
        upgrade_gray_icon    = ScalableBitmap(this, "monitor_upgrade_offline", 5);
        upgrade_yellow_icon  = ScalableBitmap(this, "monitor_upgrade_busy", 5);
    }
    catch (const std::exception &e)
    {
        BOOST_LOG_TRIVIAL(error) << "init_bitmaps failed to load resources: " << e.what();
    } catch (...)
    {
        BOOST_LOG_TRIVIAL(error) << "init_bitmaps failed to load resources: unknown error";
    }
}

void MachineInfoPanel::rescale_bitmaps()
{
    m_img_printer.msw_rescale();
    m_printer_img->SetBitmap(m_img_printer.bmp());
    m_img_monitor_ams.msw_rescale();
    m_ams_img->SetBitmap(m_img_monitor_ams.bmp());
    m_img_ext.msw_rescale();
    m_ext_img->SetBitmap(m_img_ext.bmp());
    upgrade_green_icon.msw_rescale();
    upgrade_gray_icon.msw_rescale();
    upgrade_yellow_icon.msw_rescale();
    m_ota_new_version_img->SetBitmap(upgrade_green_icon.bmp());
}

MachineInfoPanel::~MachineInfoPanel()
{
    // Disconnect Events
    m_button_upgrade_firmware->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(MachineInfoPanel::on_upgrade_firmware), NULL, this);

    if (confirm_dlg != nullptr)
        delete confirm_dlg;
}

void MachineInfoPanel::update_printer_imgs(MachineObject* obj)
{
    if (!obj) {return;}
    auto img = obj->get_printer_thumbnail_img_str();
    img += "_png";

    try
    {
        m_img_extra_ams = ScalableBitmap(this, "extra_icon_png", 160);
        m_img_printer   = ScalableBitmap(this, img, 160);
        m_printer_img->SetBitmap(m_img_printer.bmp());
        m_printer_img->Refresh();
        m_extra_ams_img->SetBitmap(m_img_extra_ams.bmp());
        m_extra_ams_img->Refresh();
    }
    catch (const std::exception &e)
    {
        BOOST_LOG_TRIVIAL(error) << "update_printer_imgs error : " << e.what();
    }
    catch (...)
    {
        BOOST_LOG_TRIVIAL(error) << "update_printer_imgs failed: unknown error";
    }
}

void MachineInfoPanel::update(MachineObject* obj)
{
    if (m_obj != obj)
        update_printer_imgs(obj);

    m_obj = obj;
    if (obj) {
        this->Freeze();
        //update online status img
        m_panel_caption->Freeze();
        if (!obj->is_connected()) {
            m_upgrade_status_img->SetBitmap(upgrade_gray_icon.bmp());
            wxString caption_text = wxString::Format("%s(%s)", from_u8(obj->get_dev_name()), _L("Offline"));
            m_caption_text->SetLabelText(caption_text);
            show_status((int)DevFirmwareUpgradingState::UpgradingUnavaliable);
        } else {
            show_status((int)obj->upgrade_display_state, obj->upgrade_status);
            if (obj->upgrade_display_state == DevFirmwareUpgradingState::UpgradingUnavaliable) {
                if (obj->can_abort()) {
                    wxString caption_text = wxString::Format("%s(%s)", from_u8(obj->get_dev_name()), _L("Printing"));
                    m_caption_text->SetLabelText(caption_text);
                } else {
                    wxString caption_text = wxString::Format("%s", from_u8(obj->get_dev_name()));
                    m_caption_text->SetLabelText(caption_text);
                }
                m_upgrade_status_img->SetBitmap(upgrade_yellow_icon.bmp());
            } else {
                wxString caption_text = wxString::Format("%s(%s)", from_u8(obj->get_dev_name()), _L("Idle"));
                m_caption_text->SetLabelText(caption_text);
                m_upgrade_status_img->SetBitmap(upgrade_green_icon.bmp());
            }
        }
        m_panel_caption->Layout();
        m_panel_caption->Thaw();

        // update version
        update_version_text(obj);

        // update ams and extension
        update_ams_ext(obj);

        // update
        update_air_pump(obj);
        update_cut(obj);
        update_laszer(obj);
        update_extinguish(obj);

        //update progress
        int upgrade_percent = obj->get_upgrade_percent();
        if (obj->upgrade_display_state == DevFirmwareUpgradingState::UpgradingInProgress) {
            m_upgrade_progress->SetValue(upgrade_percent);
            m_staticText_upgrading_percent->SetLabelText(wxString::Format("%d%%", upgrade_percent));
        } else if (obj->upgrade_display_state == DevFirmwareUpgradingState::UpgradingFinished) {
            wxString result_text = obj->get_upgrade_result_str(obj->upgrade_err_code);
            m_upgrade_progress->SetValue(upgrade_percent);
            m_staticText_upgrading_percent->SetLabelText(wxString::Format("%d%%", upgrade_percent));
        }

        wxString model_id_text = obj->get_printer_type_display_str();
        m_staticText_model_id_val->SetLabelText(model_id_text);
        wxString sn_text = obj->get_dev_id();
        m_staticText_sn_val->SetLabelText(sn_text.MakeUpper());

        this->Layout();
        this->Thaw();
    }
}

void MachineInfoPanel::update_version_text(MachineObject* obj)
{
    if (obj->upgrade_display_state == DevFirmwareUpgradingState::UpgradingInProgress) {
        m_staticText_ver_val->SetLabelText("-");
        //m_staticText_ams_ver_val->SetLabelText("-");
        m_ota_new_version_img->Hide();
    } else {
        // update version text
        auto it = obj->module_vers.find("ota");

        // old protocol
        if (obj->new_ver_list.empty() && !obj->m_new_ver_list_exist) {
            if (obj->upgrade_new_version
                && !obj->ota_new_version_number.empty()) {
                if (it != obj->module_vers.end()) {
                    wxString ver_text= it->second.sw_ver;
                    if ((it->second.firmware_flag & 0x3) == FIRMWARE_STASUS::BETA) {
                        ver_text+= wxString::Format("(%s)", _L("Beta version"));
                    }
                    ver_text += wxString::Format("->%s", obj->ota_new_version_number);
                    if (((it->second.firmware_flag >> 2) & 0x3) == FIRMWARE_STASUS::BETA) {
                        ver_text += wxString::Format("(%s)", _L("Beta version"));
                    }
                    //wxString ver_text = wxString::Format("%s->%s", it->second.sw_ver, obj->ota_new_version_number);
                    m_staticText_ver_val->SetLabelText(ver_text);
                }
                else {
                    m_staticText_ver_val->SetLabelText("-");
                }
                m_ota_new_version_img->Show();
            }
            else {
                if (it != obj->module_vers.end()) {
                    wxString ver_text = wxString::Format("%s(%s)", it->second.sw_ver, _L("Latest version"));
                    if ((it->second.firmware_flag & 0x3) == FIRMWARE_STASUS::BETA) {
                        m_staticText_beta_version->Show();
                    }
                    else {
                        m_staticText_beta_version->Hide();
                    }
                    m_staticText_ver_val->SetLabelText(ver_text);
                }
                else {
                    m_staticText_ver_val->SetLabelText("-");
                }
                m_ota_new_version_img->Hide();
            }
        } else {
            auto ota_it = obj->new_ver_list.find("ota");
            if (ota_it == obj->new_ver_list.end()) {
                if (it != obj->module_vers.end()) {
                    wxString ver_text = wxString::Format("%s(%s)", it->second.sw_ver, _L("Latest version"));
                    if ((it->second.firmware_flag & 0x3) == FIRMWARE_STASUS::BETA) {
                        m_staticText_beta_version->Show();
                    }
                    else {
                        m_staticText_beta_version->Hide();
                    }
                    m_staticText_ver_val->SetLabelText(ver_text);
                    m_ota_new_version_img->Hide();
                }
            } else {
                if (ota_it->second.sw_new_ver != ota_it->second.sw_ver) {
                    m_ota_new_version_img->Show();
                    wxString ver_text = wxString::Format("%s->%s", ota_it->second.sw_ver, ota_it->second.sw_new_ver);
                    if (it != obj->module_vers.end()) {
                        ver_text = ota_it->second.sw_ver;
                        if ((it->second.firmware_flag & 0x3) == FIRMWARE_STASUS::BETA) {
                            ver_text += wxString::Format("(%s)", _L("Beta version"));
                        }
                        ver_text += wxString::Format("->%s", ota_it->second.sw_new_ver);
                        if (((it->second.firmware_flag >> 2) & 0x3) == FIRMWARE_STASUS::BETA) {
                            ver_text += wxString::Format("(%s)", _L("Beta version"));
                        }
                    }
                    m_staticText_ver_val->SetLabelText(ver_text);
                } else {
                    if (it != obj->module_vers.end()) {
                        m_ota_new_version_img->Hide();
                        wxString ver_text = wxString::Format("%s(%s)", it->second.sw_ver, _L("Latest version"));
                        if ((it->second.firmware_flag & 0x3) == FIRMWARE_STASUS::BETA) {
                            m_staticText_beta_version->Show();
                        }
                        else {
                            m_staticText_beta_version->Hide();
                        }
                        m_staticText_ver_val->SetLabelText(ver_text);
                    }
                }
            }
        }
    }
}

void MachineInfoPanel::update_ams_ext(MachineObject *obj)
{
    bool has_hub_model = false;

    bool is_o_series = obj->is_series_o();

    //hub
    if (!obj->online_ahb || obj->module_vers.find("ahb") == obj->module_vers.end() || is_o_series)
        m_ahb_panel->Hide();
    else {
        has_hub_model = true;
        show_ams(true);

        for (auto i = 0; i < m_amspanel_list.GetCount(); i++) {
            AmsPanel *amspanel = m_amspanel_list[i];
            amspanel->Hide();
        }


        m_ahb_panel->Show();
        wxString hub_name = "-";
        if (!obj->module_vers.find("ahb")->second.product_name.empty())
        {
            hub_name = obj->module_vers.find("ahb")->second.product_name;
        }
        else
        {
            hub_name = "AMS HUB";
        }


        wxString hub_sn = "-";
        if (!obj->module_vers.find("ahb")->second.sn.empty()) {
            wxString sn_text = obj->module_vers.find("ahb")->second.sn;
            hub_sn           = sn_text.MakeUpper();
        }


        wxString hub_ver = "-";
        if (!obj->module_vers.find("ahb")->second.sw_ver.empty()) {
            wxString sn_text = obj->module_vers.find("ahb")->second.sw_ver;
            hub_ver          = sn_text.MakeUpper();
        }

       /* auto ver_item = obj->new_ver_list.find("ahb");
        if (ver_item != obj->new_ver_list.end()) {
            m_ahb_panel->m_ams_new_version_img->Show();
            hub_ver = wxString::Format("%s->%s", hub_ver, ver_item->second.sw_new_ver);
        } else {
            m_ahb_panel->m_ams_new_version_img->Hide();
            hub_ver = wxString::Format("%s(%s)", hub_ver, _L("Latest version"));
        }*/

        if (obj->new_ver_list.empty() && !obj->m_new_ver_list_exist) {
            if (obj->upgrade_new_version && obj->ahb_new_version_number.compare(obj->module_vers.find("ahb")->second.sw_ver) != 0) {
                m_ahb_panel->m_ams_new_version_img->Show();

                if (obj->ahb_new_version_number.empty()) {
                    hub_ver = wxString::Format("%s", obj->module_vers.find("ahb")->second.sw_ver);
                } else {
                    hub_ver = wxString::Format("%s->%s", obj->module_vers.find("ahb")->second.sw_ver, obj->ahb_new_version_number);
                }
            } else {
                m_ahb_panel->m_ams_new_version_img->Hide();
                wxString ver_text = wxString::Format("%s(%s)", obj->module_vers.find("ahb")->second.sw_ver, _L("Latest version"));
                hub_ver = ver_text;
            }
        } else {
            auto ver_item = obj->new_ver_list.find("ahb");

            if (ver_item == obj->new_ver_list.end()) {
                m_ahb_panel->m_ams_new_version_img->Hide();
                wxString ver_text = wxString::Format("%s(%s)", obj->module_vers.find("ahb")->second.sw_ver, _L("Latest version"));
                hub_ver           = ver_text;
            } else {
                if (ver_item->second.sw_new_ver != ver_item->second.sw_ver) {
                    m_ahb_panel->m_ams_new_version_img->Show();
                    wxString ver_text = wxString::Format("%s->%s", ver_item->second.sw_ver, ver_item->second.sw_new_ver);
                    hub_ver           = ver_text;
                } else {
                    m_ahb_panel->m_ams_new_version_img->Hide();
                    wxString ver_text = wxString::Format("%s(%s)", ver_item->second.sw_ver, _L("Latest version"));
                    hub_ver           = ver_text;
                }
            }
        }
        m_ahb_panel->m_staticText_ams->SetLabel(hub_name);
        m_ahb_panel->m_staticText_ams_sn_val->SetLabelText(hub_sn);
        m_ahb_panel->m_staticText_ams_ver_val->SetLabelText(hub_ver);
    }

    //ams
    if (obj->ams_exist_bits != 0)
    {
        std::string extra_ams_str = (boost::format("ams_f1/%1%") % 0).str();
        auto extra_ams_it = obj->module_vers.find(extra_ams_str);
        if (extra_ams_it != obj->module_vers.end()) {
            wxString sn_text = extra_ams_it->second.sn;
            sn_text = sn_text.MakeUpper();

            wxString ver_text = extra_ams_it->second.sw_ver;

            bool has_new_version = false;
            auto new_extra_ams_ver = obj->new_ver_list.find(extra_ams_str);
            if (new_extra_ams_ver != obj->new_ver_list.end())
                has_new_version = true;

            extra_ams_it->second.sw_new_ver;
            if (has_new_version) {
                m_extra_ams_panel->m_ams_new_version_img->Show();
                ver_text = new_extra_ams_ver->second.sw_ver;
                if ((extra_ams_it->second.firmware_flag & 0x3) == FIRMWARE_STASUS::BETA) {
                    ver_text += wxString::Format("(%s)", _L("Beta version"));
                }
                ver_text += wxString::Format("->%s", new_extra_ams_ver->second.sw_new_ver);
                if (((extra_ams_it->second.firmware_flag >> 2) & 0x3) == FIRMWARE_STASUS::BETA) {
                    ver_text += wxString::Format("(%s)", _L("Beta version"));
                }
            }
            else {
                m_extra_ams_panel->m_ams_new_version_img->Hide();
                ver_text = wxString::Format("%s(%s)", extra_ams_it->second.sw_ver, _L("Latest version"));
                if ((extra_ams_it->second.firmware_flag & 0x3) == FIRMWARE_STASUS::BETA) {
                    m_extra_ams_panel->m_staticText_beta_version->Show();
                }
                else {
                    m_extra_ams_panel->m_staticText_beta_version->Hide();
                }
            }
            wxString name_text = "-";
            if (!extra_ams_it->second.product_name.empty())
                name_text = extra_ams_it->second.product_name;
            else
                name_text = "AMS Lite";

            m_extra_ams_panel->m_staticText_ams->SetLabel(name_text);
            m_extra_ams_panel->m_staticText_ams_sn_val->SetLabelText(sn_text);
            m_extra_ams_panel->m_staticText_ams_ver_val->SetLabelText(ver_text);
            show_ams(false);
            show_extra_ams(true);
        }
        else {
            show_extra_ams(false);
            show_ams(true);
            std::map<int, DevFirmwareVersionInfo> ver_list = obj->get_ams_version();

            if (obj->GetFilaSystem()->GetAmsList().size() != m_amspanel_list.size()) {
                int add_count = obj->GetFilaSystem()->GetAmsList().size() - m_amspanel_list.size();
                if (add_count > 0) {
                    for (int i = 0; i < add_count; i++) {
                        auto amspanel = new AmsPanel(this, wxID_ANY);
                        wxGetApp().UpdateDarkUIWin(amspanel);
                        m_ams_info_sizer->Add(amspanel, 1, wxEXPAND, 5);
                        m_amspanel_list.Add(amspanel);
                    }
                }
                if (add_count < 0) {
                    for (int i = 0; i < -add_count; i++) {
                        m_amspanel_list.back()->Destroy();
                        m_amspanel_list.pop_back();
                    }
                }
            }

            for (auto i = 0; i < m_amspanel_list.GetCount(); i++) {
                AmsPanel* amspanel = m_amspanel_list[i];
                amspanel->Hide();
            }

            auto ams_index = 0;
            const auto& ams_list = obj->GetFilaSystem()->GetAmsList();
            for (std::map<std::string, DevAms*>::const_iterator iter = ams_list.cbegin(); iter != ams_list.cend(); iter++) {
                wxString ams_name;
                wxString ams_sn;
                wxString ams_ver;

                AmsPanel* amspanel = m_amspanel_list[ams_index];
                amspanel->Show();

                auto it = ver_list.find(atoi(iter->first.c_str()));

                if (it == ver_list.end()) {
                    continue;
                }

                auto ams_id = std::stoi(iter->second->GetAmsId());
                ams_id -= ams_id >= 128 ? 128 : 0;

                if (!it->second.product_name.empty())
                {
                    ams_name = it->second.product_name;
                }
                else
                {
                     size_t pos = it->second.name.find('/');
                     wxString ams_device_name = "AMS-%s";

                     if (pos != std::string::npos) {
                         wxString result = it->second.name.substr(0, pos);
                         result.MakeUpper();
                         if (auto str_it = ACCESSORY_DISPLAY_STR.find(result); str_it != ACCESSORY_DISPLAY_STR.end())
                             result = str_it->second;
                         ams_device_name = result + "-%s";
                     }

                     wxString ams_text = wxString::Format(ams_device_name, std::to_string(ams_id + 1));
                     ams_name = ams_text;
                }

                if (it == ver_list.end()) {
                    // hide this ams
                    ams_sn = "-";
                    ams_ver = "-";
                }
                else {
                    // update ams img
                    if (m_obj->upgrade_display_state == DevFirmwareUpgradingState::UpgradingInProgress) {
                        ams_ver = "-";
                        amspanel->m_ams_new_version_img->Hide();
                    }
                    else {
                        if (obj->new_ver_list.empty() && !obj->m_new_ver_list_exist) {
                            if (obj->upgrade_new_version &&
                                !obj->ams_new_version_number.empty() &&
                                obj->ams_new_version_number.compare(it->second.sw_ver) != 0) {
                                amspanel->m_ams_new_version_img->Show();

                                if (obj->ams_new_version_number.empty()) {
                                    ams_ver = wxString::Format("%s", it->second.sw_ver);
                                    if ((it->second.firmware_flag & 0x3) == FIRMWARE_STASUS::BETA) {
                                        amspanel->m_staticText_beta_version->Show();
                                    }
                                    else {
                                        amspanel->m_staticText_beta_version->Hide();
                                    }

                                }
                                else {
                                    //ams_ver = wxString::Format("%s->%s", it->second.sw_ver, obj->ams_new_version_number);
                                    ams_ver = it->second.sw_ver;
                                    if ((it->second.firmware_flag & 0x3) == FIRMWARE_STASUS::BETA) {
                                        ams_ver += wxString::Format("(%s)", _L("Beta version"));
                                    }
                                    ams_ver += wxString::Format("->%s", obj->ams_new_version_number);

                                }
                            }
                            else {
                                amspanel->m_ams_new_version_img->Hide();
                                wxString ver_text = wxString::Format("%s", it->second.sw_ver, _L("Latest version"));
                                if ((it->second.firmware_flag & 0x3) == FIRMWARE_STASUS::BETA)
                                {
                                    amspanel->m_staticText_beta_version->Show();
                                }
                                else
                                {
                                    amspanel->m_staticText_beta_version->Hide();
                                }
                                ams_ver = ver_text;
                            }
                        }
                        else if (!it->second.sw_new_ver.empty() && (it->second.sw_new_ver != it->second.sw_ver)) {
                            amspanel->m_ams_new_version_img->Show();
                            ams_ver = wxString::Format("%s->%s", it->second.sw_ver, it->second.sw_new_ver);
                        }
                        else {
                            std::string ams_idx = (boost::format("ams/%1%") % ams_id).str();
                            auto        ver_item = obj->new_ver_list.find(ams_idx);

                            if (ver_item == obj->new_ver_list.end()) {
                                amspanel->m_ams_new_version_img->Hide();
                                wxString ver_text = wxString::Format("%s(%s)", it->second.sw_ver, _L("Latest version"));
                                if ((it->second.firmware_flag & 0x3) == FIRMWARE_STASUS::BETA) {
                                    amspanel->m_staticText_beta_version->Show();
                                }
                                else {
                                    amspanel->m_staticText_beta_version->Hide();
                                }
                                ams_ver = ver_text;
                            }
                            else {
                                if (ver_item->second.sw_new_ver != ver_item->second.sw_ver) {
                                    amspanel->m_ams_new_version_img->Show();
                                    //wxString ver_text = wxString::Format("%s->%s", ver_item->second.sw_ver, ver_item->second.sw_new_ver);
                                    wxString ver_text = ver_item->second.sw_ver;
                                    if ((it->second.firmware_flag & 0x3) == FIRMWARE_STASUS::BETA) {
                                        ver_text += wxString::Format("(%s)", _L("Beta version"));
                                    }
                                    ver_text += wxString::Format("->%s", ver_item->second.sw_new_ver);
                                    if (((it->second.firmware_flag >> 2) & 0x3) == FIRMWARE_STASUS::BETA) {
                                        amspanel->m_staticText_beta_version->Show();
                                    }
                                    else {
                                        amspanel->m_staticText_beta_version->Hide();
                                    }
                                    ams_ver = ver_text;
                                }
                                else {
                                    amspanel->m_ams_new_version_img->Hide();
                                    wxString ver_text = wxString::Format("%s(%s)", ver_item->second.sw_ver, _L("Latest version"));
                                    if ((it->second.firmware_flag & 0x3) == FIRMWARE_STASUS::BETA) {
                                        amspanel->m_staticText_beta_version->Show();
                                    }
                                    else {
                                        amspanel->m_staticText_beta_version->Hide();
                                    }
                                    ams_ver = ver_text;
                                }
                            }
                        }
                    }

                    // update ams sn
                    if (it->second.sn.empty()) {
                        ams_sn = "-";
                    }
                    else {
                        wxString sn_text = it->second.sn;
                        ams_sn = sn_text.MakeUpper();
                    }
                }

                amspanel->m_staticText_ams->SetLabelText(ams_name);
                amspanel->m_staticText_ams_sn_val->SetLabelText(ams_sn);
                amspanel->m_staticText_ams_ver_val->SetLabelText(ams_ver);

                ams_index++;
            }
        }
    } else {
        if (!has_hub_model) { show_ams(false); }
        show_extra_ams(false);
    }

    //ext
    auto ext_module = obj->module_vers.find("ext");
    if (ext_module == obj->module_vers.end() || is_o_series)
        show_ext(false);
    else {
        wxString sn_text = ext_module->second.sn;
        sn_text = sn_text.MakeUpper();
        wxString ext_ver = "";


        // has new version
        bool has_new_version = false;
        auto new_ext_ver = obj->new_ver_list.find("ext");
        if (new_ext_ver != obj->new_ver_list.end())
            has_new_version = true;

        if (has_new_version) {
            m_ext_panel->m_ext_new_version_img->Show();
            ext_ver = wxString::Format("%s->%s", new_ext_ver->second.sw_ver, new_ext_ver->second.sw_new_ver);
        } else {
            m_ext_panel->m_ext_new_version_img->Hide();
            ext_ver = wxString::Format("%s(%s)", ext_module->second.sw_ver, _L("Latest version"));
        }

        // set sn and version
        m_ext_panel->m_staticText_ext_sn_val->SetLabelText(sn_text);
        m_ext_panel->m_staticText_ext_ver_val->SetLabelText(ext_ver);

        show_ext(true);
    }

    // STUDIO-11572 Update image
    bool contain_one_slot = false;
    bool contain_four_slot = false;
    const auto& ams_list = obj->GetFilaSystem()->GetAmsList();
    auto ams_iter = ams_list.begin();
    while (ams_iter != ams_list.end()) {
        if (ams_iter->second->GetSlotCount() == 1)
        {
            contain_one_slot = true;
        } else {
            contain_four_slot = true;
        }
        ams_iter++;
    }

    if (contain_four_slot) {
        if (m_img_monitor_ams.name() != "monitor_upgrade_ams") {
            m_img_monitor_ams = ScalableBitmap(this, "monitor_upgrade_ams_png", 150);
            m_ams_img->SetBitmap(m_img_monitor_ams.bmp());
        }
    } else if (contain_one_slot) {
        if (m_img_monitor_ams.name() != "monitor_upgrade_n3s") {
            m_img_monitor_ams = ScalableBitmap(this, "monitor_upgrade_n3s", 160);
            m_ams_img->SetBitmap(m_img_monitor_ams.bmp());
        }
    }

    this->Layout();
    this->Fit();
}

void MachineInfoPanel::update_air_pump(MachineObject* obj)
{
    if (obj && obj->air_pump_version_info.isValid())
    {
        m_air_pump_version->UpdateInfo(obj->air_pump_version_info);
        show_air_pump(true);
    }
    else
    {
        show_air_pump(false);
    }
}

void MachineInfoPanel::update_cut(MachineObject* obj)
{
    if (obj && obj->cutting_module_version_info.isValid())
    {
        m_cutting_version->UpdateInfo(obj->cutting_module_version_info);
        show_cut(true);
    }
    else
    {
        show_cut(false);
    }
}

void MachineInfoPanel::update_laszer(MachineObject* obj)
{
    if (obj && obj->laser_version_info.isValid())
    {
        m_laser_version->UpdateInfo(obj->laser_version_info);

        std::string current_product_name = obj->laser_version_info.product_name.ToStdString();

        if (m_last_laser_product_name != current_product_name) {
            std::string laser_image_name = "laser";

            if (current_product_name.find("10W") != std::string::npos) {
                laser_image_name = "laser";
            } else if (current_product_name.find("40W") != std::string::npos) {
                laser_image_name = "laser_40";
            }

            m_img_laser = ScalableBitmap(this, laser_image_name, 160);
            if (m_lazer_img) {
                m_lazer_img->SetBitmap(m_img_laser.bmp());
                m_lazer_img->Refresh();
            }

            m_last_laser_product_name = current_product_name;
        }
        show_laszer(true);
    }
    else
    {
        show_laszer(false);
    }
}

void MachineInfoPanel::update_extinguish(MachineObject* obj)
{
    if (obj && obj->extinguish_version_info.isValid())
    {
        m_extinguish_version->UpdateInfo(obj->extinguish_version_info);
        show_extinguish(true);
    }
    else
    {
        show_extinguish(false);
    }
}

void MachineInfoPanel::show_status(int status, std::string upgrade_status_str)
{
    if (last_status == status && last_status_str == upgrade_status_str) return;
    last_status     = status;
    last_status_str = upgrade_status_str;

    BOOST_LOG_TRIVIAL(trace) << "MachineInfoPanel: show_status = " << status << ", str = " << upgrade_status_str;

    Freeze();

    if (status == (int)DevFirmwareUpgradingState::UpgradingUnavaliable) {
        m_button_upgrade_firmware->Show();
        m_button_upgrade_firmware->Disable();
        for (size_t i = 0; i < m_upgrading_sizer->GetItemCount(); i++) {
            m_upgrading_sizer->Show(false);
        }
        m_upgrade_retry_img->Hide();
        m_staticText_upgrading_info->Hide();
        m_staticText_upgrading_percent->Hide();
    } else if (status == (int) DevFirmwareUpgradingState::UpgradingAvaliable) {
        m_button_upgrade_firmware->Show();
        m_button_upgrade_firmware->Enable();
        for (size_t i = 0; i < m_upgrading_sizer->GetItemCount(); i++) { m_upgrading_sizer->Show(false); }
        m_upgrade_retry_img->Hide();
        m_staticText_upgrading_info->Hide();
        m_staticText_upgrading_percent->Hide();
    } else if (status == (int) DevFirmwareUpgradingState::UpgradingInProgress) {
        m_button_upgrade_firmware->Disable();
        for (size_t i = 0; i < m_upgrading_sizer->GetItemCount(); i++) { m_upgrading_sizer->Show(true); }
        m_upgrade_retry_img->Hide();
        m_staticText_upgrading_info->Show();
        m_staticText_upgrading_info->SetLabel(_L("Updating"));
        m_staticText_upgrading_info->SetForegroundColour(TEXT_NORMAL_CLR);
        m_staticText_upgrading_percent->SetForegroundColour(TEXT_NORMAL_CLR);
        m_staticText_upgrading_percent->Show();
    } else if (status == (int) DevFirmwareUpgradingState::UpgradingFinished) {
        if (upgrade_status_str == "UPGRADE_FAIL") {
            m_staticText_upgrading_info->SetLabel(_L("Update failed"));
            m_staticText_upgrading_info->SetForegroundColour(TEXT_FAILED_CLR);
            for (size_t i = 0; i < m_upgrading_sizer->GetItemCount(); i++) { m_upgrading_sizer->Show(true); }
            m_button_upgrade_firmware->Disable();
            m_staticText_upgrading_info->Show();
            m_staticText_upgrading_percent->Show();
            m_upgrade_retry_img->Show();
        } else {
            m_staticText_upgrading_info->SetLabel(_L("Update successful"));
            m_staticText_upgrading_info->Show();
            for (size_t i = 0; i < m_upgrading_sizer->GetItemCount(); i++) { m_upgrading_sizer->Show(true); }
            m_button_upgrade_firmware->Disable();
            m_staticText_upgrading_info->SetForegroundColour(TEXT_NORMAL_CLR);
            m_staticText_upgrading_percent->SetForegroundColour(TEXT_NORMAL_CLR);
            m_staticText_upgrading_percent->Show();
            m_upgrade_retry_img->Hide();
        }
    } else {
        ;
    }
    Layout();
    Thaw();

}

void MachineInfoPanel::show_ams(bool show, bool force_update)
{
    if (m_last_ams_show != show || force_update) {
        m_ams_img->Show(show);
        m_ams_sizer->Show(show);
        m_staticline->Show(show);
        BOOST_LOG_TRIVIAL(trace) << "upgrade: show_ams = " << show;
    }
    m_last_ams_show = show;
}

void MachineInfoPanel::show_ext(bool show, bool force_update)
{
    if (m_last_ext_show != show || force_update) {
        m_ext_img->Show(show);
        m_ext_sizer->Show(show);
        m_staticline2->Show(show);
        BOOST_LOG_TRIVIAL(trace) << "upgrade: show_ext = " << show;
    }
    m_last_ext_show = show;
}

void MachineInfoPanel::show_extra_ams(bool show, bool force_update) {
    if (m_last_extra_ams_show != show || force_update) {
        m_extra_ams_img->Show(show);
        m_extra_ams_sizer->Show(show);
        m_staticline->Show(show);
        BOOST_LOG_TRIVIAL(trace) << "upgrade: show_extra_ams = " << show;
    }
    m_last_extra_ams_show = show;
}

void MachineInfoPanel::show_air_pump(bool show)
{
    if (m_air_pump_version->IsShown() != show)
    {
        m_air_pump_img->Show(show);
        m_air_pump_line_above->Show(show);
        m_air_pump_version->Show(show);
    }
}

void MachineInfoPanel::show_cut(bool show)
{
    if (m_cutting_version->IsShown() != show)
    {
        m_cutting_img->Show(show);
        m_cutting_line_above->Show(show);
        m_cutting_version->Show(show);
    }
}

void MachineInfoPanel::show_laszer(bool show)
{
    if (m_laser_version->IsShown() != show)
    {
        m_lazer_img->Show(show);
        m_laser_line_above->Show(show);
        m_laser_version->Show(show);
    }
}

void MachineInfoPanel::show_extinguish(bool show)
{
    if (m_extinguish_version->IsShown() != show)
    {
        m_extinguish_img->Show(show);
        m_extinguish_line_above->Show(show);
        m_extinguish_version->Show(show);
    }
}


void MachineInfoPanel::on_sys_color_changed()
{
    if (m_obj) {
        update_printer_imgs(m_obj);
    }
}

void MachineInfoPanel::confirm_upgrade(MachineObject* obj)
{
    if (obj) {
        obj->command_upgrade_confirm();
        obj->upgrade_display_state = DevFirmwareUpgradingState::UpgradingInProgress;
        obj->upgrade_display_hold_count = HOLD_COUNT_MAX;
        // enter in progress status first
        this->show_status((int)DevFirmwareUpgradingState::UpgradingInProgress);
    }
}

void MachineInfoPanel::upgrade_firmware_internal() {
    if (!m_obj)
        return;
    if (panel_type == ptOtaPanel) {
        m_obj->command_upgrade_firmware(m_ota_info);
    } else if (panel_type == ptAmsPanel) {
        m_obj->command_upgrade_firmware(m_ams_info);
    } else if (panel_type == ptPushPanel) {
        confirm_upgrade();
    }
}

void MachineInfoPanel::on_upgrade_firmware(wxCommandEvent &event)
{
    if (confirm_dlg == nullptr) {
        confirm_dlg = new SecondaryCheckDialog(this->GetParent(), wxID_ANY, _L("Update firmware"));
        confirm_dlg->Bind(EVT_SECONDARY_CHECK_CONFIRM, [this](wxCommandEvent& e) {
                this->confirm_upgrade(m_obj);
        });
    }
    confirm_dlg->update_text(_L("Are you sure you want to update? This will take about 10 minutes. Do not turn off the power while the printer is updating."));
    confirm_dlg->on_show();
}

void MachineInfoPanel::on_consisitency_upgrade_firmware(wxCommandEvent &event)
{
    if (confirm_dlg == nullptr) {
        confirm_dlg = new SecondaryCheckDialog(this->GetParent(), wxID_ANY, _L("Update firmware"));
        confirm_dlg->Bind(EVT_SECONDARY_CHECK_CONFIRM, [this](wxCommandEvent& e) {
            if (m_obj) {
                m_obj->command_consistency_upgrade_confirm();
            }
        });
    }
    confirm_dlg->update_text(_L("Are you sure you want to update? This will take about 10 minutes. Do not turn off the power while the printer is updating."));
    confirm_dlg->on_show();
}

void MachineInfoPanel::on_show_release_note(wxMouseEvent &event)
{
    DeviceManager *dev = wxGetApp().getDeviceManager();
    if (!dev) return;


    wxString next_version_release_note;
    wxString now_version_release_note;
    std::string version_number            = "";

    for (auto iter : m_obj->firmware_list) {
        if (iter.version == m_obj->ota_new_version_number) {
            version_number            = m_obj->ota_new_version_number;
            next_version_release_note = wxString::FromUTF8(iter.description);
        }
        if (iter.version == m_obj->get_ota_version()) {
            version_number           = m_obj->get_ota_version();
            now_version_release_note = wxString::FromUTF8(iter.description);
        }
    }

    ReleaseNoteDialog dlg;
    if (!m_obj->ota_new_version_number.empty()) {
        dlg.update_release_note(next_version_release_note, version_number);
        dlg.ShowModal();
        return;
    }

    if (!m_obj->get_ota_version().empty()) {
        dlg.update_release_note(now_version_release_note, version_number);
        dlg.ShowModal();
        return;
    }
}

UpgradePanel::UpgradePanel(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size, long style)
    :wxPanel(parent, id, pos, size, style)
{
    this->SetBackgroundColour(wxColour(238, 238, 238));

    auto m_main_sizer = new wxBoxSizer(wxVERTICAL);

    m_scrolledWindow = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scrolledWindow->SetScrollRate(5, 25);

    m_machine_list_sizer = new wxBoxSizer(wxVERTICAL);

    m_scrolledWindow->SetSizerAndFit(m_machine_list_sizer);

    m_main_sizer->Add(m_scrolledWindow, 1, wxEXPAND, 0);

    this->SetSizerAndFit(m_main_sizer);

    Layout();
}

UpgradePanel::~UpgradePanel()
{
    if (force_dlg != nullptr)
        delete force_dlg ;

    if (consistency_dlg != nullptr)
        delete consistency_dlg ;
}

void UpgradePanel::msw_rescale()
{
    if (m_push_upgrade_panel)
        m_push_upgrade_panel->msw_rescale();
}

void UpgradePanel::clean_push_upgrade_panel()
{
    if (m_push_upgrade_panel) {
        delete m_push_upgrade_panel;
        m_push_upgrade_panel = nullptr;
    }
}

void UpgradePanel::refresh_version_and_firmware(MachineObject* obj)
{
    BOOST_LOG_TRIVIAL(trace) << "refresh version";
    if (obj) {
        obj->command_get_version();
        obj->get_firmware_info();
        m_need_update = true;
    }
}

void UpgradePanel::update(MachineObject *obj)
{
    if (m_obj != obj) {
        m_obj = obj;
        refresh_version_and_firmware(obj);
    }

    Freeze();
    if (m_obj && m_need_update) {
        if (m_obj->is_firmware_info_valid()) {
            clean_push_upgrade_panel();
            m_push_upgrade_panel = new MachineInfoPanel(m_scrolledWindow);
            m_machine_list_sizer->Add(m_push_upgrade_panel, 0, wxTOP | wxALIGN_CENTER_HORIZONTAL, FromDIP(8));
            m_need_update = false;
        }
    }

    //force upgrade
    //unlock hint
    if (m_obj && (m_obj->upgrade_display_state == DevFirmwareUpgradingState::UpgradingFinished) && (last_forced_hint_status != m_obj->upgrade_display_state)) {
        last_forced_hint_status = m_obj->upgrade_display_state;
        m_show_forced_hint = true;
    }
    if (m_obj && m_show_forced_hint) {
        if (m_obj->upgrade_force_upgrade) {
            m_show_forced_hint = false;   //lock hint
            if (force_dlg == nullptr) {
                force_dlg = new SecondaryCheckDialog(this->GetParent(), wxID_ANY, _L("Update firmware"), SecondaryCheckDialog::VisibleButtons::CONFIRM_AND_CANCEL, wxDefaultPosition, wxDefaultSize);  // ORCA VisibleButtons instead ButtonStyle 
                force_dlg->Bind(EVT_SECONDARY_CHECK_CONFIRM, [this](wxCommandEvent& e) {
                    if (m_obj) {
                        m_obj->command_upgrade_confirm();
                        m_obj->upgrade_display_state = DevFirmwareUpgradingState::UpgradingInProgress;
                        m_obj->upgrade_display_hold_count = HOLD_COUNT_MAX;
                    }
                });
            }
            force_dlg->update_text(_L(
                 "An important update was detected and needs to be run before printing can continue. Do you want to update now? You can also update later from 'Upgrade firmware'."
            ));
            force_dlg->on_show();
        }
    }

    //consistency upgrade
    if (m_obj && (m_obj->upgrade_display_state == DevFirmwareUpgradingState::UpgradingFinished) && (last_consistency_hint_status != m_obj->upgrade_display_state)) {
        last_consistency_hint_status = m_obj->upgrade_display_state;
        m_show_consistency_hint = true;
    }
    if (m_obj && m_show_consistency_hint) {
        if (m_obj->upgrade_consistency_request) {
            m_show_consistency_hint = false;
            if (consistency_dlg == nullptr) {
                consistency_dlg = new SecondaryCheckDialog(this->GetParent(), wxID_ANY, _L("Update firmware"), SecondaryCheckDialog::VisibleButtons::CONFIRM_AND_CANCEL, wxDefaultPosition, wxDefaultSize);  // ORCA VisibleButtons instead ButtonStyle 
                consistency_dlg->Bind(EVT_SECONDARY_CHECK_CONFIRM, [this](wxCommandEvent& e) {
                    if (m_obj) {
                        m_obj->command_consistency_upgrade_confirm();
                    }
                });
            }
            consistency_dlg->update_text(_L(
                 "The firmware version is abnormal. Repairing and updating are required before printing. Do you want to update now? You can also update later on printer or update next time starting Orca."
            ));
            consistency_dlg->on_show();
        }
    }

    //update panels
    if (m_push_upgrade_panel) {
        m_push_upgrade_panel->update(obj);
    }

    if (!obj)
        clean_push_upgrade_panel();
    this->Layout();
    Thaw();

    m_obj = obj;
}

void UpgradePanel::show_status(int status)
{
    if (last_status == status) return;
    last_status = status;

    if (((status & (int)MonitorStatus::MONITOR_DISCONNECTED) != 0)
        || ((status & (int)MonitorStatus::MONITOR_DISCONNECTED_SERVER) != 0)
        || ((status & (int)MonitorStatus::MONITOR_CONNECTING) != 0)
        || ((status & (int)MonitorStatus::MONITOR_NO_PRINTER) != 0)
        ) {
        ;
    }
    else if ((status & (int)MonitorStatus::MONITOR_NORMAL) != 0) {
        ;
    }
}

void UpgradePanel::on_sys_color_changed()
{
    //add some protection for Dark mode
    if (m_push_upgrade_panel) {
        m_push_upgrade_panel->on_sys_color_changed();
    }
}

bool UpgradePanel::Show(bool show)
{
    if (show) {
        DeviceManager* dev = wxGetApp().getDeviceManager();
        if (dev) {
            MachineObject* obj = dev->get_selected_machine();
            refresh_version_and_firmware(obj);
        }
    }
    return wxPanel::Show(show);
}

 AmsPanel::AmsPanel(wxWindow *      parent,
                   wxWindowID      id /*= wxID_ANY*/,
                   const wxPoint & pos /*= wxDefaultPosition*/,
                   const wxSize &  size /*= wxDefaultSize*/,
                   long            style /*= wxTAB_TRAVERSAL*/,
                   const wxString &name /*= wxEmptyString*/)
    : wxPanel(parent,id,pos,size,style)
{
     upgrade_green_icon = ScalableBitmap(this, "monitor_upgrade_online", 5);

     auto ams_sizer = new wxFlexGridSizer(0, 2, 0, 0);
     ams_sizer->AddGrowableCol(1);
     ams_sizer->SetFlexibleDirection(wxHORIZONTAL);
     ams_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

     m_staticText_ams_model_id = new wxStaticText(this, wxID_ANY, _L("Model:"), wxDefaultPosition, wxDefaultSize, 0);
     m_staticText_ams_model_id->Wrap(-1);
     m_staticText_ams_model_id->SetFont(Label::Head_14);

     m_staticText_ams = new wxStaticText(this, wxID_ANY, "-", wxDefaultPosition, wxDefaultSize, 0);
     m_staticText_ams->SetForegroundColour("#262E30");
     m_staticText_ams->SetFont(Label::Head_14);
     m_staticText_ams->Wrap(-1);

     auto m_staticText_ams_sn = new wxStaticText(this, wxID_ANY, _L("Serial:"), wxDefaultPosition, wxDefaultSize, 0);
     m_staticText_ams_sn->SetForegroundColour("#262E30");
     m_staticText_ams_sn->Wrap(-1);
     m_staticText_ams_sn->SetFont(Label::Head_14);

     m_staticText_ams_sn_val = new wxStaticText(this, wxID_ANY, "-", wxDefaultPosition, wxDefaultSize, 0);
     m_staticText_ams_sn_val->SetForegroundColour("#262E30");
     m_staticText_ams_sn_val->Wrap(-1);

     wxBoxSizer *m_ams_ver_sizer = new wxBoxSizer(wxHORIZONTAL);

     m_ams_ver_sizer->Add(0, 0, 1, wxEXPAND, 0);

     m_ams_new_version_img = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(5), FromDIP(5)));
     m_ams_new_version_img->SetBitmap(upgrade_green_icon.bmp());
     m_ams_ver_sizer->Add(m_ams_new_version_img, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
     m_ams_new_version_img->Hide();

     auto m_staticText_ams_ver = new wxStaticText(this, wxID_ANY, _L("Version:"), wxDefaultPosition, wxDefaultSize, 0);
     m_staticText_ams_ver->Wrap(-1);
     m_staticText_ams_ver->SetFont(Label::Head_14);
     m_staticText_ams_ver->SetForegroundColour("#262E30");
     m_ams_ver_sizer->Add(m_staticText_ams_ver, 0, wxALL, FromDIP(5));

     m_staticText_ams_ver_val = new wxStaticText(this, wxID_ANY, "-", wxDefaultPosition, wxDefaultSize, 0);
     m_staticText_ams_ver_val->SetForegroundColour("#262E30");
     m_staticText_ams_ver_val->Wrap(-1);

     m_staticText_beta_version = new wxStaticText(this, wxID_ANY, "Beta", wxDefaultPosition, wxDefaultSize, 0);
     m_staticText_beta_version->SetForegroundColour("#778899");
     m_staticText_beta_version->Wrap(-1);
     m_staticText_beta_version->Hide();

     wxBoxSizer* content_info = new wxBoxSizer(wxHORIZONTAL);
     content_info->Add(m_staticText_ams_ver_val, 0, wxALL | wxEXPAND, FromDIP(5));
     content_info->Add(m_staticText_beta_version, 0, wxALL | wxEXPAND, FromDIP(5));

     ams_sizer->Add(m_staticText_ams_model_id, 0, wxALIGN_RIGHT | wxALL, FromDIP(5));
     ams_sizer->Add(m_staticText_ams, 0,  wxALL, FromDIP(5));
     ams_sizer->Add(m_staticText_ams_sn, 0, wxALIGN_RIGHT | wxALL, FromDIP(5));
     ams_sizer->Add(m_staticText_ams_sn_val, 0, wxALL | wxEXPAND, FromDIP(5));
     ams_sizer->Add(m_ams_ver_sizer, 1, wxEXPAND, FromDIP(5));
     ams_sizer->Add(content_info, 0,  wxEXPAND, FromDIP(5));
     ams_sizer->Add(0, 0, 1, wxEXPAND, 0);

     SetSizer(ams_sizer);
     Layout();
 }

 AmsPanel::~AmsPanel()
 {

 }

 void AmsPanel::msw_rescale() {
     upgrade_green_icon.msw_rescale();
     m_ams_new_version_img->SetBitmap(upgrade_green_icon.bmp());
 }

 ExtensionPanel::ExtensionPanel(wxWindow* parent,
     wxWindowID      id /*= wxID_ANY*/,
     const wxPoint& pos /*= wxDefaultPosition*/,
     const wxSize& size /*= wxDefaultSize*/,
     long            style /*= wxTAB_TRAVERSAL*/,
     const wxString& name /*= wxEmptyString*/)
     : wxPanel(parent, id, pos, size, style)
 {

     upgrade_green_icon = ScalableBitmap(this, "monitor_upgrade_online", 5);

     auto top_sizer = new wxBoxSizer(wxVERTICAL);

     auto ext_sizer = new wxFlexGridSizer(0, 2, 0, 0);
     ext_sizer->AddGrowableCol(1);
     ext_sizer->SetFlexibleDirection(wxHORIZONTAL);
     ext_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);


     m_staticText_ext = new wxStaticText(this, wxID_ANY, _L("Model:"), wxDefaultPosition, wxDefaultSize, 0);
     m_staticText_ext->SetForegroundColour("#262E30");
     m_staticText_ext->Wrap(-1);
     m_staticText_ext->SetFont(Label::Head_14);

     m_staticText_ext_val = new wxStaticText(this, wxID_ANY, _L("Extension Board"), wxDefaultPosition, wxDefaultSize, 0);
     m_staticText_ext_val->SetForegroundColour("#262E30");
     m_staticText_ext_val->SetFont(Label::Head_14);
     m_staticText_ext_val->Wrap(-1);

     auto m_staticText_ext_sn = new wxStaticText(this, wxID_ANY, _L("Serial:"), wxDefaultPosition, wxDefaultSize, 0);
     m_staticText_ext_sn->SetForegroundColour("#262E30");
     m_staticText_ext_sn->Wrap(-1);
     m_staticText_ext_sn->SetFont(Label::Head_14);

     m_staticText_ext_sn_val = new wxStaticText(this, wxID_ANY, "-", wxDefaultPosition, wxDefaultSize, 0);
     m_staticText_ext_sn_val->SetForegroundColour("#262E30");
     m_staticText_ext_sn_val->Wrap(-1);

     wxBoxSizer* m_ext_ver_sizer = new wxBoxSizer(wxHORIZONTAL);
     m_ext_ver_sizer->Add(0, 0, 1, wxEXPAND, 0);
     m_ext_new_version_img = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(5), FromDIP(5)));
     m_ext_new_version_img->SetBitmap(upgrade_green_icon.bmp());
     m_ext_ver_sizer->Add(m_ext_new_version_img, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
     m_ext_new_version_img->Hide();

     m_staticText_ext_ver = new wxStaticText(this, wxID_ANY, _L("Version:"), wxDefaultPosition, wxDefaultSize, 0);
     m_staticText_ext_ver->Wrap(-1);
     m_staticText_ext_ver->SetFont(Label::Head_14);
     m_staticText_ext_ver->SetForegroundColour("#262E30");
     m_ext_ver_sizer->Add(m_staticText_ext_ver, 0, wxALL, FromDIP(5));

     m_staticText_ext_ver_val = new wxStaticText(this, wxID_ANY, "-", wxDefaultPosition, wxDefaultSize, 0);
     m_staticText_ext_ver_val->SetForegroundColour("#262E30");
     m_staticText_ext_ver_val->Wrap(-1);

     ext_sizer->Add(m_staticText_ext, 0, wxALIGN_RIGHT | wxALL, FromDIP(5));
     ext_sizer->Add(m_staticText_ext_val, 0, wxALL | wxEXPAND, FromDIP(5));
     ext_sizer->Add(m_staticText_ext_sn, 0, wxALIGN_RIGHT | wxALL, FromDIP(5));
     ext_sizer->Add(m_staticText_ext_sn_val, 0, wxALL | wxEXPAND, FromDIP(5));
     ext_sizer->Add(m_ext_ver_sizer, 1, wxEXPAND, FromDIP(5));
     ext_sizer->Add(m_staticText_ext_ver_val, 0, wxALL | wxEXPAND, FromDIP(5));
     ext_sizer->Add(0, 0, 1, wxEXPAND, 0);

     top_sizer->Add(ext_sizer);
     SetSizer(top_sizer);
     Layout();
 }

 ExtensionPanel::~ExtensionPanel()
 {

 }

 void ExtensionPanel::msw_rescale()
 {
     upgrade_green_icon.msw_rescale();
     m_ext_new_version_img->SetBitmap(upgrade_green_icon.bmp());
 }

 ExtraAmsPanel::ExtraAmsPanel(wxWindow* parent,
     wxWindowID      id /*= wxID_ANY*/,
     const wxPoint& pos /*= wxDefaultPosition*/,
     const wxSize& size /*= wxDefaultSize*/,
     long            style /*= wxTAB_TRAVERSAL*/,
     const wxString& name /*= wxEmptyString*/)
     : AmsPanel(parent, id, pos, size, style)
 {

 }

}
}
