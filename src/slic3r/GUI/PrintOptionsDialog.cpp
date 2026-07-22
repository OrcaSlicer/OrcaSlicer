#include "PrintOptionsDialog.hpp"
#include <initializer_list>
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "libslic3r/Utils.hpp"
#include "Widgets/SwitchButton.hpp"
#include "MsgDialog.hpp"

#include "DeviceCore/DevConfig.h"
#include "DeviceCore/DevExtruderSystem.h"
#include "DeviceCore/DevNozzleSystem.h"
#include "DeviceCore/DevFan.h"
#include "DeviceCore/DevPrintOptions.h"

static const wxColour STATIC_BOX_LINE_COL = wxColour(238, 238, 238);
static const wxColour STATIC_TEXT_CAPTION_COL = wxColour(100, 100, 100);
static const wxColour STATIC_TEXT_EXPLAIN_COL = wxColour(100, 100, 100);

namespace Slic3r { namespace GUI {

static StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Disabled),
                               std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
                               std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
                               std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));

PrintOptionsDialog::PrintOptionsDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Print Options"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    this->SetDoubleBuffered(true);
    SetBackgroundColour(*wxWHITE);
    SetSize(FromDIP(480),FromDIP(520));


    m_scrollwindow = new wxScrolledWindow(this, wxID_ANY);
    m_scrollwindow->SetScrollRate(0, FromDIP(10));
    m_scrollwindow->SetBackgroundColour(*wxWHITE);
    m_scrollwindow->SetMinSize(wxSize(FromDIP(480), wxDefaultCoord));
    m_scrollwindow->SetMaxSize(wxSize(FromDIP(480), wxDefaultCoord));

    auto m_options_sizer = create_settings_group(m_scrollwindow);
    m_options_sizer->SetMinSize(wxSize(FromDIP(460), wxDefaultCoord));

    m_scrollwindow->SetSizer(m_options_sizer);

    wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);
    mainSizer->Add(m_scrollwindow, 1, wxEXPAND);
    this->SetSizer(mainSizer);

    m_options_sizer->Fit(m_scrollwindow);
    m_scrollwindow->FitInside();

    this->Layout();
    // mainSizer->Fit(this);
    //this->Fit();

     m_cb_ai_monitoring->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &evt) {
        if (obj) {
            int         level = ai_monitoring_level_list->GetSelection();
            std::string lvl   = sensitivity_level_to_msg_string((AiMonitorSensitivityLevel) level);
            if (!lvl.empty())
                obj->command_xcam_control_ai_monitoring(m_cb_ai_monitoring->GetValue(), lvl);
            else
                BOOST_LOG_TRIVIAL(warning) << "print_option: lvl = " << lvl;
        }
        evt.Skip();
    });


      // refine printer function options
    m_cb_spaghetti_detection->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt) {
        if (obj) {
            int         level = spaghetti_detection_level_list->GetSelection();
            std::string lvl   = sensitivity_level_to_msg_string((AiMonitorSensitivityLevel) level);
            if (!lvl.empty())
                obj->command_xcam_control_spaghetti_detection(m_cb_spaghetti_detection->GetValue(), lvl);
            else
                BOOST_LOG_TRIVIAL(warning) << "print_option: lvl = " << lvl;
        }
        evt.Skip();
    });


       m_cb_purgechutepileup_detection->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &evt) {
        if (obj) {
            int         level = purgechutepileup_detection_level_list->GetSelection();
            std::string lvl   = sensitivity_level_to_msg_string((AiMonitorSensitivityLevel) level);
            if (!lvl.empty())
                obj->command_xcam_control_purgechutepileup_detection(m_cb_purgechutepileup_detection->GetValue(), lvl);
            else
                BOOST_LOG_TRIVIAL(warning) << "print_option: lvl = " << lvl;
        }
        evt.Skip();
    });


       m_cb_nozzleclumping_detection->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &evt) {
           if (obj) {
               int         level = nozzleclumping_detection_level_list->GetSelection();
               std::string lvl   = sensitivity_level_to_msg_string((AiMonitorSensitivityLevel) level);
               if (!lvl.empty())
                   obj->command_xcam_control_nozzleclumping_detection(m_cb_nozzleclumping_detection->GetValue(), lvl);
               else
                   BOOST_LOG_TRIVIAL(warning) << "print_option: lvl = " << lvl;
           }
           evt.Skip();
       });

        m_cb_airprinting_detection->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &evt) {
           if (obj) {
               int         level = airprinting_detection_level_list->GetSelection();
               std::string lvl   = sensitivity_level_to_msg_string((AiMonitorSensitivityLevel) level);
               if (!lvl.empty())
                   obj->command_xcam_control_airprinting_detection(m_cb_airprinting_detection->GetValue(), lvl);
               else
                   BOOST_LOG_TRIVIAL(warning) << "print_option: lvl = " << lvl;
           }
           evt.Skip();
       });



    m_cb_first_layer->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt) {
        if (obj) {
            obj->command_xcam_control_first_layer_inspector(m_cb_first_layer->GetValue(), false);
        }
        evt.Skip();
    });

    m_cb_auto_recovery->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt) {
        if (obj) {
            obj->command_xcam_control_auto_recovery_step_loss(m_cb_auto_recovery->GetValue());
        }
        evt.Skip();
    });

    m_cb_save_remote_print_file_to_storage->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt)
    {
        if (obj) { obj->command_set_save_remote_print_file_to_storage(m_cb_save_remote_print_file_to_storage->GetValue());}
        evt.Skip();
    });

    m_cb_plate_mark->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt) {
        if (obj) {
            obj->command_xcam_control_buildplate_marker_detector(m_cb_plate_mark->GetValue());
        }
        evt.Skip();
    });
    m_cb_sup_sound->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt) {
        if (obj) {
            obj->command_xcam_control_allow_prompt_sound(m_cb_sup_sound->GetValue());
        }
        evt.Skip();
    });
    m_cb_filament_tangle->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt) {
        if (obj) {
            obj->command_xcam_control_filament_tangle_detect(m_cb_filament_tangle->GetValue());
        }
        evt.Skip();
    });
    m_cb_nozzle_blob->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt) {
        if (obj) {
            obj->command_nozzle_blob_detect(m_cb_nozzle_blob->GetValue());
        }
        evt.Skip();
        });

    m_cb_open_door->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt) {
        if (m_cb_open_door->GetValue()) {
            if (obj) { obj->command_set_door_open_check(MachineObject::DOOR_OPEN_CHECK_ENABLE_WARNING); }
        } else {
            if (obj) { obj->command_set_door_open_check(MachineObject::DOOR_OPEN_CHECK_DISABLE); }
        }
        evt.Skip();
    });

    open_door_switch_board->Bind(wxCUSTOMEVT_SWITCH_POS, [this](wxCommandEvent& evt)
    {
        if (evt.GetInt() == 0)
        {
            if (obj) { obj->command_set_door_open_check(MachineObject::DOOR_OPEN_CHECK_ENABLE_PAUSE_PRINT); }
        }
        else if (evt.GetInt() == 1)
        {
            if (obj) { obj->command_set_door_open_check(MachineObject::DOOR_OPEN_CHECK_ENABLE_WARNING); }
        }
        evt.Skip();
    });

    // Firmware print-option toggles (each gated on a fun2 capability bit, published via DevPrintOptions).
    m_cb_plate_align->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt) {
        if (obj) { obj->GetPrintOptions()->command_xcam_control_build_plate_align_detector(m_cb_plate_align->GetValue()); }
        evt.Skip();
    });

    m_cb_fod_check->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt) {
        if (obj) { obj->GetPrintOptions()->command_xcam_control_fod_check(m_cb_fod_check->GetValue()); }
        evt.Skip();
    });

    m_cb_displacement_detection->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt) {
        if (obj) { obj->GetPrintOptions()->command_xcam_control_displacement_detection(m_cb_displacement_detection->GetValue()); }
        evt.Skip();
    });

    m_cb_purify_air_at_print_end->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt) {
        if (obj) { obj->GetPrintOptions()->command_xcam_control_purify_air_at_print_end(m_cb_purify_air_at_print_end->GetValue()); }
        evt.Skip();
    });

    purify_air_switch_board->Bind(wxCUSTOMEVT_SWITCH_POS, [this](wxCommandEvent& evt) {
        if (evt.GetInt() == 0) {
            if (obj) { obj->GetPrintOptions()->command_xcam_control_purify_air_at_print_end((int) DevPrintOptions::PurifyAirAtPrintEndState::PurifyAirByOutside); }
        } else if (evt.GetInt() == 1) {
            if (obj) { obj->GetPrintOptions()->command_xcam_control_purify_air_at_print_end((int) DevPrintOptions::PurifyAirAtPrintEndState::PurifyAirByInside); }
        }
        evt.Skip();
    });

    m_smart_nozzle_blob_mode_switch->Bind(wxCUSTOMEVT_MULTISWITCH_SELECTION, [this](wxCommandEvent& evt) {
        if (!obj || !obj->GetPrintOptions()) { evt.Skip(); return; }
        int sel = m_smart_nozzle_blob_mode_switch->GetSelection();
        // UI: 0=Auto, 1=On, 2=Off -> protocol: 0=off, 1=on, 2=auto
        int mode_map[] = {2, 1, 0};
        if (sel < 0 || sel > 2) { evt.Skip(); return; }

        // If currently in Auto (current_detect_value == 2), a print is running, and a loaded
        // filament is stringing-prone, confirm before enabling — turning blob detection on
        // mid-print with such filament can degrade quality.
        const auto* blob_opt = obj->GetPrintOptions()->GetDetectionOption(PrintOptionEnum::Smart_Nozzle_Blob_Detection);
        const bool was_auto = blob_opt && blob_opt->current_detect_value == 2;
        if (sel == 1 /*On*/ && was_auto && obj->is_in_printing()
            && obj->any_loaded_filament_is_stringing_prone()) {
            wxString message = _L("There is stringing-prone filament in the current print job. "
                                  "Enabling nozzle clumping detection now may degrade print quality. "
                                  "Are you sure you want to enable it?");
            wxString caption = _L("Enable Nozzle Clumping Detection");
            // Themed warning dialog with Cancel as the highlighted default, so the safe choice (leave
            // detection off) is the default and Confirm must be actively picked.
            MessageDialog dialog(this, message, caption, wxICON_WARNING);
            dialog.AddButton(wxID_CANCEL, _L("Cancel"),  true);
            dialog.AddButton(wxID_OK,     _L("Confirm"), false);
            if (dialog.ShowModal() != wxID_OK) {
                // User cancelled: roll the switch back to Auto without sending the command.
                m_smart_nozzle_blob_mode_switch->SetSelection(0);
                update_smart_nozzle_blob_mode_desc(0);
                evt.Skip();
                return;
            }
        }

        obj->GetPrintOptions()->command_smart_nozzle_blob_detect_mode(mode_map[sel]);
        update_smart_nozzle_blob_mode_desc(sel);
        evt.Skip();
    });

    m_cb_snapshot_enable->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& evt) {
        if (!obj || !obj->GetPrintOptions()) { evt.Skip(); return; }
        if (m_cb_snapshot_enable->GetValue()) {
            wxString message = _L("When enabled, the printer will automatically capture photos of printed parts and upload them to the cloud. Would you like to enable this option?");
            wxString caption = _L("Confirm Enable Print Status Snapshot");
            wxMessageDialog dialog(this, message, caption, wxYES_NO | wxICON_QUESTION);
            dialog.SetYesNoLabels(_L("Confirm"), _L("Cancel"));
            if (dialog.ShowModal() == wxID_YES) {
                obj->GetPrintOptions()->command_snapshot_control(true);
                m_cb_snapshot_enable->SetValue(true);
            } else {
                m_cb_snapshot_enable->SetValue(false);
            }
        } else {
            obj->GetPrintOptions()->command_snapshot_control(false);
        }
        evt.Skip();
    });

    wxGetApp().UpdateDlgDarkUI(this);
}

PrintOptionsDialog::~PrintOptionsDialog()
{
    ai_monitoring_level_list->Disconnect(wxEVT_COMBOBOX, wxCommandEventHandler(PrintOptionsDialog::set_ai_monitor_sensitivity), NULL, this);
    // refine printer function options
    spaghetti_detection_level_list->Disconnect(wxEVT_COMBOBOX, wxCommandEventHandler(PrintOptionsDialog::set_spaghetti_detection_sensitivity), NULL, this);
    purgechutepileup_detection_level_list->Disconnect(wxEVT_COMBOBOX, wxCommandEventHandler(PrintOptionsDialog::set_purgechutepileup_detection_sensitivity), NULL, this);
    nozzleclumping_detection_level_list->Disconnect(wxEVT_COMBOBOX, wxCommandEventHandler(PrintOptionsDialog::set_nozzleclumping_detection_sensitivity), NULL, this);
    airprinting_detection_level_list->Disconnect(wxEVT_COMBOBOX, wxCommandEventHandler(PrintOptionsDialog::set_airprinting_detection_sensitivity), NULL, this);
}

void PrintOptionsDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    Fit();
}

void PrintOptionsDialog::update_ai_monitor_status()
{
    if (m_cb_ai_monitoring->GetValue()) {
        ai_monitoring_level_list->Enable();
    } else {
        ai_monitoring_level_list->Disable();
    }
}

// refine printer function options

void PrintOptionsDialog::update_spaghetti_detection_status()
{
    if (m_cb_spaghetti_detection->GetValue()) {
        spaghetti_detection_level_list->Enable();
    } else {
        spaghetti_detection_level_list->Disable();
    }
}

void PrintOptionsDialog::update_purgechutepileup_detection_status()
{
    if (m_cb_purgechutepileup_detection->GetValue()) {
        purgechutepileup_detection_level_list->Enable();
    } else {
        purgechutepileup_detection_level_list->Disable();
    }
}

void PrintOptionsDialog::update_nozzleclumping_detection_status()
{
    if (m_cb_nozzleclumping_detection->GetValue()) {
        nozzleclumping_detection_level_list->Enable();
    } else {
        nozzleclumping_detection_level_list->Disable();
    }
}

void PrintOptionsDialog::update_airprinting_detection_status()
{
    if (m_cb_airprinting_detection->GetValue()) {
        airprinting_detection_level_list->Enable();
    } else {
        airprinting_detection_level_list->Disable();
    }
}


void PrintOptionsDialog::update_options(MachineObject* obj_)
{
    if (!obj_) return;

    if (obj_->is_support_spaghetti_detection || obj_->is_support_purgechutepileup_detection || obj_->is_support_nozzleclumping_detection || obj_->is_support_airprinting_detection) {
        ai_refine_panel->Show();
        text_ai_detections->Show();
        text_ai_detections_caption->Show();
        m_line->Show();
    } else {
        ai_refine_panel->Hide();
        text_ai_detections->Hide();
        text_ai_detections_caption->Hide();
        m_line->Hide();
    }

    if (obj_->GetConfig()->SupportAIMonitor() && !obj_->xcam_disable_ai_detection_display) {
        text_ai_monitoring->Show();
        m_cb_ai_monitoring->Show();
        text_ai_monitoring_caption->Show();
        ai_monitoring_level_list->Show();
        ai_monitoring_bottom_space->Show(true);
        // line1->Show();
    } else {
        text_ai_monitoring->Hide();
        m_cb_ai_monitoring->Hide();
        text_ai_monitoring_caption->Hide();
        ai_monitoring_level_list->Hide();
        ai_monitoring_bottom_space->Show(false);
        //line1->Hide();
    }

   //refine printer function options
    if (obj_->is_support_spaghetti_detection) {
        text_spaghetti_detection->Show();
        m_cb_spaghetti_detection->Show();
        text_spaghetti_detection_caption0->Show();
        text_spaghetti_detection_caption1->Show();
        spaghetti_detection_level_list->Show();
        spaghetti_bottom_space->Show(true);

        //line1->Show();
    } else {
        text_spaghetti_detection->Hide();
        m_cb_spaghetti_detection->Hide();
        text_spaghetti_detection_caption0->Hide();
        text_spaghetti_detection_caption1->Hide();
        spaghetti_detection_level_list->Hide();
        spaghetti_bottom_space->Show(false);
        //line1->Hide();
    }


    if (obj_->is_support_purgechutepileup_detection) {
        text_purgechutepileup_detection->Show();
        m_cb_purgechutepileup_detection->Show();
        text_purgechutepileup_detection_caption0->Show();
        text_purgechutepileup_detection_caption1->Show();
        purgechutepileup_detection_level_list->Show();
        purgechutepileup_bottom_space->Show(true);
     // line1->Show();
    } else {
        text_purgechutepileup_detection->Hide();
        m_cb_purgechutepileup_detection->Hide();
        text_purgechutepileup_detection_caption0->Hide();
        text_purgechutepileup_detection_caption1->Hide();
        purgechutepileup_detection_level_list->Hide();
        purgechutepileup_bottom_space->Show(false);
       // line1->Hide();
    }


    if (obj_->is_support_nozzleclumping_detection) {
        text_nozzleclumping_detection->Show();
        m_cb_nozzleclumping_detection->Show();
        text_nozzleclumping_detection_caption0->Show();
        text_nozzleclumping_detection_caption1->Show();
        nozzleclumping_detection_level_list->Show();
        nozzleclumping_bottom_space->Show(true);

        // line1->Show();
    } else {
        text_nozzleclumping_detection->Hide();
        m_cb_nozzleclumping_detection->Hide();
        text_nozzleclumping_detection_caption0->Hide();
        text_nozzleclumping_detection_caption1->Hide();
        nozzleclumping_detection_level_list->Hide();
        nozzleclumping_bottom_space->Show(false);
       // line1->Hide();
    }

    if (obj_->is_support_airprinting_detection) {
        text_airprinting_detection->Show();
        m_cb_airprinting_detection->Show();
        text_airprinting_detection_caption0->Show();
        text_airprinting_detection_caption1->Show();
        airprinting_detection_level_list->Show();
        airprinting_bottom_space->Show(true);
    //    line1->Show();
    } else {
        text_airprinting_detection->Hide();
        m_cb_airprinting_detection->Hide();
        text_airprinting_detection_caption0->Hide();
        text_airprinting_detection_caption1->Hide();
        airprinting_detection_level_list->Hide();
       // line1->Hide();
        airprinting_bottom_space->Show(false);
    }



    if (obj_->is_support_build_plate_marker_detect) {
        if (obj_->m_plate_maker_detect_type == MachineObject::POS_CHECK && (text_plate_mark->GetLabel() != _L("Enable detection of build plate position"))) {
            text_plate_mark->SetLabel(_L("Enable detection of build plate position"));
            text_plate_mark_caption->SetLabel(_L("The localization tag of the build plate will be detected, and printing will be paused if the tag is not in predefined range."));
            text_plate_mark_caption->Wrap(FromDIP(400));
        } else if (obj_->m_plate_maker_detect_type == MachineObject::TYPE_POS_CHECK && (text_plate_mark->GetLabel() != _L("Build Plate Detection"))) {
            text_plate_mark->SetLabel(_L("Build Plate Detection"));
            text_plate_mark_caption->SetLabel(_L("Identifies the type and position of the build plate on the heatbed. Pausing printing if a mismatch is detected."));
            text_plate_mark_caption->Wrap(FromDIP(400));
        }

        text_plate_mark->Show();
        m_cb_plate_mark->Show();
        text_plate_mark_caption->Show();
      //  line2->Show();
    }
    else {
        text_plate_mark->Hide();
        m_cb_plate_mark->Hide();
        text_plate_mark_caption->Hide();
        line2->Hide();
    }

    if (obj_->GetConfig()->SupportFirstLayerInspect()) {
        text_first_layer->Show();
        m_cb_first_layer->Show();
       // line3->Show();
    }
    else {
        text_first_layer->Hide();
        m_cb_first_layer->Hide();
        line3->Hide();
    }

    if (obj_->is_support_auto_recovery_step_loss) {
        text_auto_recovery->Show();
        m_cb_auto_recovery->Show();
        //line4->Show();
    }
    else {
        text_auto_recovery->Hide();
        m_cb_auto_recovery->Hide();
        line4->Hide();
    }
    if (obj_->is_support_prompt_sound) {
        text_sup_sound->Show();
        m_cb_sup_sound->Show();
      //  line5->Show();
    }
    else {
        text_sup_sound->Hide();
        m_cb_sup_sound->Hide();
        line5->Hide();
    }
    if (obj_->is_support_filament_tangle_detect) {
        text_filament_tangle->Show();
        m_cb_filament_tangle->Show();
       // line6->Show();
    }
    else {
        text_filament_tangle->Hide();
        m_cb_filament_tangle->Hide();
        line6->Hide();
    }
    if (false/*obj_->is_support_nozzle_blob_detection*/) {
        text_nozzle_blob->Show();
        m_cb_nozzle_blob->Show();
        text_nozzle_blob_caption->Show();
       // line7->Show();
    }
    else {
        text_nozzle_blob->Hide();
        m_cb_nozzle_blob->Hide();
        text_nozzle_blob_caption->Hide();
        line7->Hide();
    }

    // Orca: firmware print-option toggles, each shown only when its fun2 capability bit is
    //       reported. A printer reporting none of these bits renders the dialog as before.
    auto* print_opts = obj_->GetPrintOptions();
    auto  show_row = [](bool support, std::initializer_list<wxWindow*> widgets, wxSizerItem* bottom_space) {
        for (auto* w : widgets) { if (w) w->Show(support); }
        if (bottom_space) bottom_space->Show(support);
    };

    auto* plate_align_opt = print_opts ? print_opts->GetDetectionOption(PrintOptionEnum::Buildplate_Align_Detection) : nullptr;
    show_row(plate_align_opt && plate_align_opt->is_support_detect, {m_cb_plate_align, text_plate_align, text_plate_align_caption}, plate_align_bottom_space);

    auto* fod_opt = print_opts ? print_opts->GetDetectionOption(PrintOptionEnum::FOD_Check_Detection) : nullptr;
    show_row(fod_opt && fod_opt->is_support_detect, {m_cb_fod_check, text_fod_check, text_fod_check_caption}, fod_check_bottom_space);

    auto* displacement_opt = print_opts ? print_opts->GetDetectionOption(PrintOptionEnum::Displacement_Detection) : nullptr;
    show_row(displacement_opt && displacement_opt->is_support_detect, {m_cb_displacement_detection, text_displacement_detection, text_displacement_detection_caption}, displacement_bottom_space);

    auto* smart_blob_opt = print_opts ? print_opts->GetDetectionOption(PrintOptionEnum::Smart_Nozzle_Blob_Detection) : nullptr;
    show_row(smart_blob_opt && smart_blob_opt->is_support_detect, {m_smart_nozzle_blob_mode_switch, text_smart_nozzle_blob, text_smart_nozzle_blob_mode_desc}, smart_nozzle_blob_bottom_space);

    UpdateOptionSavePrintFileToStorage(obj_);
    UpdateOptionOpenDoorCheck(obj_);
    UpdateOptionSnapshot(obj_);

    this->Freeze();

    if (plate_align_opt)   m_cb_plate_align->SetValue(plate_align_opt->current_detect_value == 1);
    if (fod_opt)           m_cb_fod_check->SetValue(fod_opt->current_detect_value == 1);
    if (displacement_opt)  m_cb_displacement_detection->SetValue(displacement_opt->current_detect_value == 1);
    if (smart_blob_opt && smart_blob_opt->is_support_detect) {
        // Protocol: 0=off, 1=on, 2=auto -> UI: 0=Auto, 1=On, 2=Off
        int mode   = smart_blob_opt->current_detect_value;
        int ui_map[] = {2, 1, 0};
        int ui_sel = (mode >= 0 && mode <= 2) ? ui_map[mode] : 0;
        m_smart_nozzle_blob_mode_switch->SetSelection(ui_sel);
        update_smart_nozzle_blob_mode_desc(ui_sel);
    }

    m_cb_first_layer->SetValue(obj_->xcam_first_layer_inspector);
    m_cb_plate_mark->SetValue(obj_->xcam_buildplate_marker_detector);
    m_cb_auto_recovery->SetValue(obj_->xcam_auto_recovery_step_loss);
    m_cb_sup_sound->SetValue(obj_->xcam_allow_prompt_sound);
    m_cb_filament_tangle->SetValue(obj_->xcam_filament_tangle_detect);
    m_cb_nozzle_blob->SetValue(obj_->nozzle_blob_detection_enabled);


    m_cb_ai_monitoring->SetValue(obj_->xcam_ai_monitoring);
    for (auto i = AiMonitorSensitivityLevel::LOW; i < LEVELS_NUM; i = (AiMonitorSensitivityLevel) (i + 1)) {
        if (sensitivity_level_to_msg_string(i) == obj_->xcam_ai_monitoring_sensitivity) {
            ai_monitoring_level_list->SetSelection((int) i);
            break;
        }
    }
    //refine printer function options
    m_cb_spaghetti_detection->SetValue(obj_->xcam_spaghetti_detection);
    for (auto i = AiMonitorSensitivityLevel::LOW; i < LEVELS_NUM; i = (AiMonitorSensitivityLevel) (i + 1)) {
        if (sensitivity_level_to_msg_string(i) == obj_->xcam_spaghetti_detection_sensitivity) {
            spaghetti_detection_level_list->SetSelection((int) i);
            break;
        }
    }

    m_cb_purgechutepileup_detection->SetValue(obj_->xcam_purgechutepileup_detection);
    for (auto i = AiMonitorSensitivityLevel::LOW; i < LEVELS_NUM; i = (AiMonitorSensitivityLevel) (i + 1)) {
         if (sensitivity_level_to_msg_string(i) == obj_->xcam_purgechutepileup_detection_sensitivity) {
            purgechutepileup_detection_level_list->SetSelection((int) i);
            break;
        }
    }

    m_cb_nozzleclumping_detection->SetValue(obj_->xcam_nozzleclumping_detection);
    for (auto i = AiMonitorSensitivityLevel::LOW; i < LEVELS_NUM; i = (AiMonitorSensitivityLevel) (i + 1)) {
        if (sensitivity_level_to_msg_string(i) == obj_->xcam_nozzleclumping_detection_sensitivity) {
            nozzleclumping_detection_level_list->SetSelection((int) i);
            break;
        }
    }


    m_cb_airprinting_detection->SetValue(obj_->xcam_airprinting_detection);
    for (auto i = AiMonitorSensitivityLevel::LOW; i < LEVELS_NUM; i = (AiMonitorSensitivityLevel) (i + 1)) {
        if (sensitivity_level_to_msg_string(i) == obj_->xcam_airprinting_detection_sensitivity) {
            airprinting_detection_level_list->SetSelection((int) i);
            break;
        }
    }

    update_ai_monitor_status();
    // refine printer function options
    update_spaghetti_detection_status();
    update_purgechutepileup_detection_status();
    update_nozzleclumping_detection_status();
    update_airprinting_detection_status();
    update_purify_air_at_print_end(obj_);


    this->Thaw();
    Layout();
}



void PrintOptionsDialog::UpdateOptionSavePrintFileToStorage(MachineObject *obj)
{
    if (obj && obj->GetConfig()->SupportSaveRemotePrintFileToStorage())
    {
        m_cb_save_remote_print_file_to_storage->SetValue(obj->get_save_remote_print_file_to_storage());
    }
    else
    {
        m_cb_save_remote_print_file_to_storage->Hide();
        text_save_remote_print_file_to_storage->Hide();
        text_save_remote_print_file_to_storage_explain->Hide();
    }
}

void PrintOptionsDialog::UpdateOptionOpenDoorCheck(MachineObject *obj)
{
    if (!obj || !obj->support_door_open_check()) {
        m_cb_open_door->Hide();
        text_open_door->Hide();
        open_door_switch_board->Hide();
        return;
    }

    // Determine if current printer supports safety options
    std::string current_printer_type = obj->printer_type;
    bool supports_safety = DevPrinterConfigUtil::support_safety_options(current_printer_type);

    // Hide door open check for printers that support safety options
    if (supports_safety) {
        m_cb_open_door->Hide();
        text_open_door->Hide();
        open_door_switch_board->Hide();
        return;
    }

    if (obj->get_door_open_check_state() != MachineObject::DOOR_OPEN_CHECK_DISABLE) {
        m_cb_open_door->SetValue(true);
        open_door_switch_board->Enable();

        if (obj->get_door_open_check_state() == MachineObject::DOOR_OPEN_CHECK_ENABLE_WARNING) {
            open_door_switch_board->updateState("left");
            open_door_switch_board->Refresh();
        } else if (obj->get_door_open_check_state() == MachineObject::DOOR_OPEN_CHECK_ENABLE_PAUSE_PRINT) {
            open_door_switch_board->updateState("right");
            open_door_switch_board->Refresh();
        }

    } else {
        m_cb_open_door->SetValue(false);
        open_door_switch_board->Disable();
    }

    m_cb_open_door->Show();
    text_open_door->Show();
    open_door_switch_board->Show();
}

void PrintOptionsDialog::update_purify_air_at_print_end(MachineObject *obj_)
{
    if (!obj_ || !obj_->GetPrintOptions()) return;

    auto* opt = obj_->GetPrintOptions()->GetDetectionOption(PrintOptionEnum::Purify_Air_At_Print_End);
    if (!opt || !opt->is_support_detect) {
        purify_air_switch_board->Disable();
        m_cb_purify_air_at_print_end->Hide();
        text_purify_air->Hide();
        text_purify_air_context->Hide();
        purify_air_switch_board->Hide();
        if (purify_air_bottom_space) purify_air_bottom_space->Show(false);
        return;
    }

    if (purify_air_bottom_space) purify_air_bottom_space->Show(true);
    m_cb_purify_air_at_print_end->Show();
    text_purify_air->Show();
    m_cb_purify_air_at_print_end->Enable();
    purify_air_switch_board->Enable();
    text_purify_air_context->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    text_purify_air->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));

    // Orca: this codebase's AirDuctData has no IsExaustFanExit() helper, so the exhaust/chamber
    //       fan is detected inline by scanning the air-duct parts for the chamber-fan id.
    bool has_exhaust_fan = false;
    for (const auto& part : obj_->GetFan()->GetAirDuctData().parts) {
        if (part.id == int(AIR_FUN::FAN_CHAMBER_0_IDX)) { has_exhaust_fan = true; break; }
    }

    if (has_exhaust_fan) {
        text_purify_air_context->SetLabel(_L("Purifies the chamber air as the print finishes, based on the selected mode."));
        purify_air_switch_board->Show();
        if (opt->current_detect_value != 0) {
            m_cb_purify_air_at_print_end->SetValue(true);
            purify_air_switch_board->updateState(opt->current_detect_value == 1 ? "left" : "right");
            purify_air_switch_board->Refresh();
        } else {
            purify_air_switch_board->Disable();
            m_cb_purify_air_at_print_end->SetValue(false);
        }
    } else {
        text_purify_air_context->SetLabel(_L("Purifies the chamber air through internal circulation as each print finishes."));
        purify_air_switch_board->Hide();
        m_cb_purify_air_at_print_end->SetValue(opt->current_detect_value != 0);
    }
    text_purify_air_context->Show();
    text_purify_air_context->Wrap(FromDIP(400));

    // Orca: disabled while a print is running (matches the reference); the reference's
    //       "unavailable during the task" toast is not ported.
    if (obj_->is_in_printing()) {
        m_cb_purify_air_at_print_end->Disable();
        purify_air_switch_board->Disable();
        text_purify_air_context->SetForegroundColour(wxColour(170, 170, 170));
        text_purify_air->SetForegroundColour(wxColour(170, 170, 170));
    }
}

void PrintOptionsDialog::update_smart_nozzle_blob_mode_desc(int selection)
{
    wxString desc;
    switch (selection) {
    case 0: // Auto
        desc = _L("Automatically match the corresponding switch strategy for leak-prone filaments (disable blob detection) and regular filaments (enable blob detection).");
        break;
    case 1: // On
        desc = _L("Detect whether the nozzle is wrapped by filament or other foreign matter.");
        break;
    case 2: // Off
        desc = _L("After disabling, nozzle wrapping cannot be detected, which may lead to print failure or nozzle damage.");
        break;
    default:
        desc = _L("Detect whether the nozzle is wrapped by filament or other foreign matter.");
        break;
    }
    text_smart_nozzle_blob_mode_desc->SetLabel(desc);
    text_smart_nozzle_blob_mode_desc->Wrap(FromDIP(400));
}

void PrintOptionsDialog::UpdateOptionSnapshot(MachineObject *obj)
{
    if (!IsShown()) { return; }

    auto* opt = (obj && obj->GetPrintOptions()) ? obj->GetPrintOptions()->GetDetectionOption(PrintOptionEnum::Snapshot_Detection) : nullptr;
    if (!opt || !opt->is_support_detect) {
        m_cb_snapshot_enable->Show(false);
        m_snapshot_sizer->Show(false);
        Layout();
        return;
    }

    if (!m_cb_snapshot_enable->IsShown()) {
        m_cb_snapshot_enable->Show(true);
        m_snapshot_sizer->Show(true);
        Layout();
    }

    if (time(nullptr) - opt->detect_hold_start > HOLD_TIME_6SEC) {
        m_cb_snapshot_enable->SetValue(opt->current_detect_value == 2);
    }
}

wxBoxSizer* PrintOptionsDialog::create_settings_group(wxWindow* parent)
{
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* line_sizer = new wxBoxSizer(wxHORIZONTAL);

  /*  auto m_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line->SetBackgroundColour(wxColour(166, 169, 170));
    sizer->Add(m_line, 0, wxEXPAND, 0);*/

    //wxPanel *m_line = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(1)), wxTAB_TRAVERSAL);
    //m_line->SetBackgroundColour(wxColour(166, 169, 170));
    //sizer->Add(m_line, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(0));

    ai_refine_panel             = new wxPanel(parent);
    wxBoxSizer *ai_refine_sizer = new wxBoxSizer(wxVERTICAL);
    ai_refine_panel->SetBackgroundColour(*wxWHITE);

    // ai detections
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    text_ai_detections = new Label(ai_refine_panel, _L("AI Detections"));
    text_ai_detections->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(text_ai_detections, 0, wxLEFT | wxRIGHT | wxDOWN | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    ai_refine_sizer->Add(0, 0, 0, wxTOP, FromDIP(20));
    ai_refine_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));

    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    text_ai_detections_caption = new Label(ai_refine_panel, _L("Printer will send assistant message or pause printing if any of the following problem is detected."));
    text_ai_detections_caption->SetFont(Label::Body_12);
    text_ai_detections_caption->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    text_ai_detections_caption->Wrap(FromDIP(400));
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(text_ai_detections_caption, 0,wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    ai_refine_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    ai_detections_bottom_space = ai_refine_sizer->Add(0, 0, 0, wxTOP, FromDIP(15));

     // ai monitoring with levels
    line_sizer         = new wxBoxSizer(wxHORIZONTAL);
    m_cb_ai_monitoring = new CheckBox(parent);
    text_ai_monitoring = new Label(parent, _L("Enable AI monitoring of printing"));
    text_ai_monitoring->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_ai_monitoring, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_ai_monitoring, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer                 = new wxBoxSizer(wxHORIZONTAL);
    text_ai_monitoring_caption = new Label(parent, _L("Pausing Sensitivity:"));
    text_ai_monitoring_caption->SetFont(Label::Body_12);
    text_ai_monitoring_caption->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    text_ai_monitoring_caption->Wrap(-1);

    ai_monitoring_level_list = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1), 0, NULL, wxCB_READONLY);
    for (auto i = AiMonitorSensitivityLevel::LOW; i < LEVELS_NUM; i = (AiMonitorSensitivityLevel) (i + 1)) {
        wxString level_option = sensitivity_level_to_label_string(i);
        ai_monitoring_level_list->Append(level_option);
    }

    if (ai_monitoring_level_list->GetCount() > 0) { ai_monitoring_level_list->SetSelection(0); }

    line_sizer->Add(FromDIP(30), 0, 0, 0);
    line_sizer->Add(text_ai_monitoring_caption, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(ai_monitoring_level_list, 0, wxEXPAND | wxALL, FromDIP(5));
    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    ai_monitoring_bottom_space = sizer->Add(0, 0, 0, wxTOP, FromDIP(12));

    //spaghetti detection  with levels
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_spaghetti_detection = new CheckBox(ai_refine_panel);
    text_spaghetti_detection = new Label(ai_refine_panel, _L("Spaghetti Detection"));
    text_spaghetti_detection->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_spaghetti_detection, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    line_sizer->Add(text_spaghetti_detection, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    ai_refine_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));

    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    text_spaghetti_detection_caption0 = new Label(ai_refine_panel, _L("Detect spaghetti failures (scattered lose filament)."));
    text_spaghetti_detection_caption0->SetFont(Label::Body_12);
    text_spaghetti_detection_caption0->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    text_spaghetti_detection_caption0->Wrap(-1);
    line_sizer->Add(FromDIP(30), 0, 0, 0);
    line_sizer->Add(text_spaghetti_detection_caption0, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    ai_refine_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));

    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    text_spaghetti_detection_caption1 = new Label(ai_refine_panel, _L("Pausing Sensitivity:"));
    text_spaghetti_detection_caption1->SetFont(Label::Body_12);
    text_spaghetti_detection_caption1->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    text_spaghetti_detection_caption1->Wrap(-1);

    spaghetti_detection_level_list = new ComboBox(ai_refine_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1), 0, NULL, wxCB_READONLY);
    for (auto i = AiMonitorSensitivityLevel::LOW; i < LEVELS_NUM; i = (AiMonitorSensitivityLevel) (i + 1)) {
        wxString level_option = sensitivity_level_to_label_string(i);
        spaghetti_detection_level_list->Append(level_option);
    }
    if (spaghetti_detection_level_list->GetCount() > 0) {
        spaghetti_detection_level_list->SetSelection(0);
    }

    line_sizer->Add(FromDIP(30), 0, 0, 0);
    line_sizer->Add(text_spaghetti_detection_caption1, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add( spaghetti_detection_level_list, 0, wxEXPAND|wxALL, FromDIP(5) );
    ai_refine_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));

   /* line1 = new StaticLine(parent, false);
    line1->SetLineColour(STATIC_BOX_LINE_COL);
    sizer->Add(line1, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));*/
    spaghetti_bottom_space = ai_refine_sizer->Add(0, 0, 0, wxTOP, FromDIP(12));

    //purge chute pile-up detection
    line_sizer               = new wxBoxSizer(wxHORIZONTAL);
    m_cb_purgechutepileup_detection = new CheckBox(ai_refine_panel);
    text_purgechutepileup_detection = new Label(ai_refine_panel, _L("Purge Chute Pile-Up Detection"));
    text_purgechutepileup_detection->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_purgechutepileup_detection, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    line_sizer->Add(text_purgechutepileup_detection, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    ai_refine_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));

    line_sizer                        = new wxBoxSizer(wxHORIZONTAL);
    text_purgechutepileup_detection_caption0 = new Label(ai_refine_panel, _L("Monitor if the waste is piled up in the purge chute."));
    text_purgechutepileup_detection_caption0->SetFont(Label::Body_12);
    text_purgechutepileup_detection_caption0->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    text_purgechutepileup_detection_caption0->Wrap(-1);
    line_sizer->Add(FromDIP(30), 0, 0, 0);
    line_sizer->Add(text_purgechutepileup_detection_caption0, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    ai_refine_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));

    line_sizer                        = new wxBoxSizer(wxHORIZONTAL);
    text_purgechutepileup_detection_caption1 = new Label(ai_refine_panel, _L("Pausing Sensitivity:"));
    text_purgechutepileup_detection_caption1->SetFont(Label::Body_12);
    text_purgechutepileup_detection_caption1->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    text_purgechutepileup_detection_caption1->Wrap(-1);

    purgechutepileup_detection_level_list = new ComboBox(ai_refine_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1), 0, NULL, wxCB_READONLY);
    for (auto i = AiMonitorSensitivityLevel::LOW; i < LEVELS_NUM; i = (AiMonitorSensitivityLevel) (i + 1)) {
        wxString level_option = sensitivity_level_to_label_string(i);
        purgechutepileup_detection_level_list->Append(level_option);
    }
    if (purgechutepileup_detection_level_list->GetCount() > 0) { purgechutepileup_detection_level_list->SetSelection(0); }

    line_sizer->Add(FromDIP(30), 0, 0, 0);
    line_sizer->Add(text_purgechutepileup_detection_caption1, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(purgechutepileup_detection_level_list, 0, wxEXPAND | wxALL, FromDIP(5));
    ai_refine_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    purgechutepileup_bottom_space = ai_refine_sizer->Add(0, 0, 0, wxTOP, FromDIP(12));


    //nozzle clumping detection
    line_sizer                      = new wxBoxSizer(wxHORIZONTAL);
    m_cb_nozzleclumping_detection = new CheckBox(ai_refine_panel);
    text_nozzleclumping_detection = new Label(ai_refine_panel, _L("Nozzle Clumping Detection"));
    text_nozzleclumping_detection->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_nozzleclumping_detection, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    line_sizer->Add(text_nozzleclumping_detection, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
   /* sizer->Add(0, 0, 0, wxTOP, FromDIP(10));*/
    ai_refine_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));

    line_sizer                               = new wxBoxSizer(wxHORIZONTAL);
    text_nozzleclumping_detection_caption0 = new Label(ai_refine_panel, _L("Check if the nozzle is clumping by filaments or other foreign objects."));
    text_nozzleclumping_detection_caption0->SetFont(Label::Body_12);
    text_nozzleclumping_detection_caption0->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    text_nozzleclumping_detection_caption0->Wrap(-1);
    line_sizer->Add(FromDIP(30), 0, 0, 0);
    line_sizer->Add(text_nozzleclumping_detection_caption0, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    ai_refine_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));


    line_sizer                             = new wxBoxSizer(wxHORIZONTAL);
    text_nozzleclumping_detection_caption1 = new Label(ai_refine_panel, _L("Pausing Sensitivity:"));
    text_nozzleclumping_detection_caption1->SetFont(Label::Body_12);
    text_nozzleclumping_detection_caption1->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    text_nozzleclumping_detection_caption1->Wrap(-1);

    nozzleclumping_detection_level_list = new ComboBox(ai_refine_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1), 0, NULL, wxCB_READONLY);
    for (auto i = AiMonitorSensitivityLevel::LOW; i < LEVELS_NUM; i = (AiMonitorSensitivityLevel) (i + 1)) {
        wxString level_option = sensitivity_level_to_label_string(i);
        nozzleclumping_detection_level_list->Append(level_option);
    }
    if (nozzleclumping_detection_level_list->GetCount() > 0) { nozzleclumping_detection_level_list->SetSelection(0); }

    line_sizer->Add(FromDIP(30), 0, 0, 0);
    line_sizer->Add(text_nozzleclumping_detection_caption1, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(nozzleclumping_detection_level_list, 0, wxEXPAND | wxALL, FromDIP(5));
    ai_refine_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    nozzleclumping_bottom_space = ai_refine_sizer->Add(0, 0, 0, wxTOP, FromDIP(12));


    //air printing detection
    line_sizer                      = new wxBoxSizer(wxHORIZONTAL);
    m_cb_airprinting_detection = new CheckBox(ai_refine_panel);
    text_airprinting_detection = new Label(ai_refine_panel, _L("Air Printing Detection"));
    text_airprinting_detection->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_airprinting_detection, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    line_sizer->Add(text_airprinting_detection, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
   /* sizer->Add(0, 0, 0, wxTOP, FromDIP(12));*/
    ai_refine_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));

    line_sizer                               = new wxBoxSizer(wxHORIZONTAL);
    text_airprinting_detection_caption0 = new Label(ai_refine_panel, _L("Detects air printing caused by nozzle clogging or filament grinding."));
    text_airprinting_detection_caption0->SetFont(Label::Body_12);
    text_airprinting_detection_caption0->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    text_airprinting_detection_caption0->Wrap(-1);
    line_sizer->Add(FromDIP(30), 0, 0, 0);
    line_sizer->Add(text_airprinting_detection_caption0, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    ai_refine_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));

    line_sizer                               = new wxBoxSizer(wxHORIZONTAL);
    text_airprinting_detection_caption1 = new Label(ai_refine_panel, _L("Pausing Sensitivity:"));
    text_airprinting_detection_caption1->SetFont(Label::Body_12);
    text_airprinting_detection_caption1->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    text_airprinting_detection_caption1->Wrap(-1);

    airprinting_detection_level_list = new ComboBox(ai_refine_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1), 0, NULL, wxCB_READONLY);
    for (auto i = AiMonitorSensitivityLevel::LOW; i < LEVELS_NUM; i = (AiMonitorSensitivityLevel) (i + 1)) {
        wxString level_option = sensitivity_level_to_label_string(i);
        airprinting_detection_level_list->Append(level_option);
    }
    if (airprinting_detection_level_list->GetCount() > 0) { airprinting_detection_level_list->SetSelection(0); }

    line_sizer->Add(FromDIP(30), 0, 0, 0);
    line_sizer->Add(text_airprinting_detection_caption1, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(airprinting_detection_level_list, 0, wxEXPAND | wxALL, FromDIP(5));
    ai_refine_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    airprinting_bottom_space = ai_refine_sizer->Add(0, 0, 0, wxTOP, FromDIP(12));

      ai_refine_panel->SetSizer(ai_refine_sizer);
    sizer->Add(ai_refine_panel, 0, wxEXPAND | wxRIGHT, FromDIP(18));

    //    sizer->Add(line1, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));

    m_line = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(400), FromDIP(1)));
    m_line->SetBackgroundColour(wxColour("#A6A9AA"));
    sizer->Add(m_line, 0, wxLEFT | wxBOTTOM, FromDIP(20));

    // detection of build plate position
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_plate_mark = new CheckBox(parent);
    text_plate_mark = new Label(parent, _L("Enable detection of build plate position"));
    text_plate_mark->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_plate_mark, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_plate_mark, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    line_sizer->Add(FromDIP(5), 0, 0, 0);

    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxString caption_text = _L("The localization tag of the build plate will be detected, and printing will be paused if the tag is not in predefined range.");
    text_plate_mark_caption = new Label(parent, caption_text);
    text_plate_mark_caption->Wrap(FromDIP(400));
    text_plate_mark_caption->SetFont(Label::Body_12);
    text_plate_mark_caption->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    line_sizer->Add(FromDIP(38), 0, 0, 0);
    line_sizer->Add(text_plate_mark_caption, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(0));

    //m_line = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(1)), wxTAB_TRAVERSAL);
    //m_line->SetBackgroundColour(wxColour(166, 169, 170));
    //sizer->Add(m_line, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));

    line2 = new StaticLine(parent, false);
    line2->SetLineColour(STATIC_BOX_LINE_COL);
    sizer->Add(line2, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    line2->Hide();
    sizer->Add(0, 0, 0, wxTOP, FromDIP(15));

    // detection of first layer
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_first_layer = new CheckBox(parent);
    text_first_layer = new Label(parent, _L("First Layer Inspection"));
    text_first_layer->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_first_layer, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_first_layer, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    line_sizer->Add(FromDIP(5), 0, 0, 0);

    line3 = new StaticLine(parent, false);
    line3->SetLineColour(STATIC_BOX_LINE_COL);
    sizer->Add(line3, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    line3->Hide();
    sizer->Add(0, 0, 0, wxTOP, FromDIP(15));

    // auto-recovery from step loss
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_auto_recovery = new CheckBox(parent);
    text_auto_recovery = new Label(parent, _L("Auto-recover from step loss"));
    text_auto_recovery->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_auto_recovery, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_auto_recovery, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    line_sizer->Add(FromDIP(10), 0, 0, 0);

    line4 = new StaticLine(parent, false);
    line4->SetLineColour(wxColour("#FFFFFF"));
    sizer->Add(line4, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    line4->Hide();
    sizer->Add(0, 0, 0, wxTOP, FromDIP(15));

     //Save remote file to local storage
    line_sizer     = new wxBoxSizer(wxHORIZONTAL);
    m_cb_save_remote_print_file_to_storage = new CheckBox(parent);
    text_save_remote_print_file_to_storage = new Label(parent, _L("Store Sent Files on External Storage"));
    text_save_remote_print_file_to_storage->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_save_remote_print_file_to_storage, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_save_remote_print_file_to_storage, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    text_save_remote_print_file_to_storage_explain = new Label(parent, _L("Save the printing files sent from the slicer and other apps on External Storage")); // Orca: neutral wording (vendor app names removed)
    text_save_remote_print_file_to_storage_explain->SetForegroundColour(STATIC_TEXT_EXPLAIN_COL);
    text_save_remote_print_file_to_storage_explain->SetFont(Label::Body_12);
    text_save_remote_print_file_to_storage_explain->Wrap(FromDIP(400));

    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    sizer->Add(text_save_remote_print_file_to_storage_explain, 0, wxLEFT, FromDIP(58));
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    //Allow prompt sound
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_sup_sound = new CheckBox(parent);
    text_sup_sound = new Label(parent, _L("Allow Prompt Sound"));
    text_sup_sound->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_sup_sound, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_sup_sound, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    line_sizer->Add(FromDIP(5), 0, 0, 0);

    line5 = new StaticLine(parent, false);
    line5->SetLineColour(STATIC_BOX_LINE_COL);
    sizer->Add(line5, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    line5->Hide();
    /*sizer->Add(0, 0, 0, wxTOP, FromDIP(20));*/
    sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    //filament tangle detect
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_filament_tangle = new CheckBox(parent);
    text_filament_tangle = new Label(parent, _L("Filament Tangle Detection"));
    text_filament_tangle->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_filament_tangle, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_filament_tangle, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    line_sizer->Add(FromDIP(5), 0, 0, 0);

    line6 = new StaticLine(parent, false);
    line6->SetLineColour(STATIC_BOX_LINE_COL);
    sizer->Add(line6, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    line6->Hide();
    //sizer->Add(0, 0, 0, wxTOP, FromDIP(20));
    sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    //nozzle blob detect
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_nozzle_blob = new CheckBox(parent);
    text_nozzle_blob = new Label(parent, _L("Nozzle Clumping Detection"));
    text_nozzle_blob->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_nozzle_blob, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_nozzle_blob, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    line_sizer->Add(FromDIP(5), 0, 0, 0);

    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxString nozzle_blob_caption_text = _L("Check if the nozzle is clumping by filament or other foreign objects.");
    text_nozzle_blob_caption = new Label(parent, nozzle_blob_caption_text);
    text_nozzle_blob_caption->SetFont(Label::Body_12);
    text_nozzle_blob_caption->Wrap(-1);
    text_nozzle_blob_caption->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    line_sizer->Add(FromDIP(30), 0, 0, 0);
    line_sizer->Add(text_nozzle_blob_caption, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(0));
    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));

    line7 = new StaticLine(parent, false);
    line7->SetLineColour(STATIC_BOX_LINE_COL);
    sizer->Add(line7, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

    text_nozzle_blob->Hide();
    m_cb_nozzle_blob->Hide();
    text_nozzle_blob_caption->Hide();
    line7->Hide();

    // ---- Firmware print-option toggles (hidden until their fun2 capability bit is reported) ----

    // Purify Air at Print End (tri-state: internal circulation / exhaust)
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_purify_air_at_print_end = new CheckBox(parent);
    text_purify_air = new Label(parent, _L("Purify Air at Print End"));
    text_purify_air->SetFont(Label::Body_14);
    text_purify_air_context = new Label(parent, wxEmptyString);
    text_purify_air_context->SetFont(Label::Body_12);
    text_purify_air_context->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    purify_air_switch_board = new SwitchBoard(parent, _L("Internal Circulation"), _L("Exhaust"), wxSize(FromDIP(300), FromDIP(26)));
    purify_air_switch_board->Disable();
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_purify_air_at_print_end, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_purify_air, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    sizer->Add(text_purify_air_context, 0, wxLEFT, FromDIP(58));
    sizer->Add(purify_air_switch_board, 0, wxLEFT, FromDIP(58));
    purify_air_bottom_space = sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    m_cb_purify_air_at_print_end->Hide();
    text_purify_air->Hide();
    text_purify_air_context->Hide();
    purify_air_switch_board->Hide();
    purify_air_bottom_space->Show(false);

    // Build plate alignment detection
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_plate_align = new CheckBox(parent);
    text_plate_align = new Label(parent, _L("Alignment Detection"));
    text_plate_align->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_plate_align, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_plate_align, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    text_plate_align_caption = new Label(parent, _L("Pauses printing when build plate misalignment is detected."));
    text_plate_align_caption->Wrap(FromDIP(400));
    text_plate_align_caption->SetFont(Label::Body_12);
    text_plate_align_caption->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    line_sizer->Add(FromDIP(38), 0, 0, 0);
    line_sizer->Add(text_plate_align_caption, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(0));
    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    plate_align_bottom_space = sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    m_cb_plate_align->Hide();
    text_plate_align->Hide();
    text_plate_align_caption->Hide();
    plate_align_bottom_space->Show(false);

    // Foreign Object Detection
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_fod_check = new CheckBox(parent);
    text_fod_check = new Label(parent, _L("Foreign Object Detection"));
    text_fod_check->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_fod_check, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_fod_check, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    text_fod_check_caption = new Label(parent, _L("Checks for any objects on the build plate at the start of a print to avoid collisions."));
    text_fod_check_caption->Wrap(FromDIP(400));
    text_fod_check_caption->SetFont(Label::Body_12);
    text_fod_check_caption->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    line_sizer->Add(FromDIP(38), 0, 0, 0);
    line_sizer->Add(text_fod_check_caption, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(0));
    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    fod_check_bottom_space = sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    m_cb_fod_check->Hide();
    text_fod_check->Hide();
    text_fod_check_caption->Hide();
    fod_check_bottom_space->Show(false);

    // Printed Part Displacement Detection
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_displacement_detection = new CheckBox(parent);
    text_displacement_detection = new Label(parent, _L("Printed Part Displacement Detection"));
    text_displacement_detection->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_displacement_detection, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_displacement_detection, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    text_displacement_detection_caption = new Label(parent, _L("Monitors the printed part during printing and alerts immediately if it shifts or collapses."));
    text_displacement_detection_caption->Wrap(FromDIP(400));
    text_displacement_detection_caption->SetFont(Label::Body_12);
    text_displacement_detection_caption->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    line_sizer->Add(FromDIP(38), 0, 0, 0);
    line_sizer->Add(text_displacement_detection_caption, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(0));
    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    displacement_bottom_space = sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    m_cb_displacement_detection->Hide();
    text_displacement_detection->Hide();
    text_displacement_detection_caption->Hide();
    displacement_bottom_space->Show(false);

    // Smart nozzle clumping detection (tri-mode: Auto / On / Off)
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    text_smart_nozzle_blob = new Label(parent, _L("Nozzle Clumping Detection"));
    text_smart_nozzle_blob->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(text_smart_nozzle_blob, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    text_smart_nozzle_blob_mode_desc = new Label(parent, _L("Checks if the nozzle is clumping by filament or other foreign objects."));
    text_smart_nozzle_blob_mode_desc->SetFont(Label::Body_12);
    text_smart_nozzle_blob_mode_desc->Wrap(FromDIP(400));
    text_smart_nozzle_blob_mode_desc->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(text_smart_nozzle_blob_mode_desc, 1, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    m_smart_nozzle_blob_mode_switch = new MultiSwitchButton(parent);
    m_smart_nozzle_blob_mode_switch->SetOptions({_L("Auto"), _L("On"), _L("Off")});
    m_smart_nozzle_blob_mode_switch->SetSelection(0);
    sizer->Add(m_smart_nozzle_blob_mode_switch, 0, wxLEFT, FromDIP(30));
    smart_nozzle_blob_bottom_space = sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    text_smart_nozzle_blob->Hide();
    m_smart_nozzle_blob_mode_switch->Hide();
    text_smart_nozzle_blob_mode_desc->Hide();
    smart_nozzle_blob_bottom_space->Show(false);

    //Open Door Detection
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_open_door = new CheckBox(parent);
    text_open_door = new Label(parent, _L("Open Door Detection"));
    text_open_door->SetFont(Label::Body_14);
    open_door_switch_board = new SwitchBoard(parent, _L("Notification"), _L("Pause printing"), wxSize(FromDIP(200), FromDIP(26)));
    open_door_switch_board->Disable();
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_open_door, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_open_door, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    sizer->Add(open_door_switch_board, 0, wxLEFT, FromDIP(58));
    sizer->Add(0, 0, 0, wxTOP, FromDIP(15));

    // Print status snapshot (enable path shows a confirm prompt; hidden until supported)
    m_snapshot_sizer = new wxBoxSizer(wxVERTICAL);
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cb_snapshot_enable = new CheckBox(parent);
    Label* text_snapshot = new Label(parent, _L("Print Status Snapshot"));
    text_snapshot->SetFont(Label::Body_14);
    line_sizer->Add(FromDIP(5), 0, 0, 0);
    line_sizer->Add(m_cb_snapshot_enable, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    line_sizer->Add(text_snapshot, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    m_snapshot_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    line_sizer = new wxBoxSizer(wxHORIZONTAL);
    Label* text_snapshot_caption = new Label(parent, _L("Automatically capture and upload print photos, showing defects during printing and the final result for remote viewing."));
    text_snapshot_caption->Wrap(FromDIP(400));
    text_snapshot_caption->SetFont(Label::Body_12);
    text_snapshot_caption->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    line_sizer->Add(FromDIP(38), 0, 0, 0);
    line_sizer->Add(text_snapshot_caption, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(0));
    m_snapshot_sizer->Add(line_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));
    sizer->Add(m_snapshot_sizer, 0, wxEXPAND | wxRIGHT, FromDIP(18));
    m_cb_snapshot_enable->Hide();
    m_snapshot_sizer->Show(false);

      ai_monitoring_level_list->Connect(wxEVT_COMBOBOX, wxCommandEventHandler(PrintOptionsDialog::set_ai_monitor_sensitivity), NULL, this);

    // refine printer function options
    spaghetti_detection_level_list->Connect(wxEVT_COMBOBOX, wxCommandEventHandler(PrintOptionsDialog::set_spaghetti_detection_sensitivity), NULL, this);
    purgechutepileup_detection_level_list->Connect(wxEVT_COMBOBOX, wxCommandEventHandler(PrintOptionsDialog::set_purgechutepileup_detection_sensitivity), NULL, this);
    nozzleclumping_detection_level_list->Connect(wxEVT_COMBOBOX, wxCommandEventHandler(PrintOptionsDialog::set_nozzleclumping_detection_sensitivity), NULL, this);
    airprinting_detection_level_list->Connect(wxEVT_COMBOBOX, wxCommandEventHandler(PrintOptionsDialog::set_airprinting_detection_sensitivity), NULL, this);

    return sizer;
}

wxString PrintOptionsDialog::sensitivity_level_to_label_string(enum AiMonitorSensitivityLevel level) {
    switch (level) {
    case LOW:
        return _L("Low");
    case MEDIUM:
        return _L("Medium");
    case HIGH:
        return _L("High");
    default:
        return "";
    }
    return "";
}

std::string PrintOptionsDialog::sensitivity_level_to_msg_string(enum AiMonitorSensitivityLevel level) {
    switch (level) {
    case LOW:
        return "low";
    case MEDIUM:
        return "medium";
    case HIGH:
        return "high";
    default:
        return "";
    }
    return "";
}

void PrintOptionsDialog::set_ai_monitor_sensitivity(wxCommandEvent &evt)
{
    int         level = ai_monitoring_level_list->GetSelection();
    std::string lvl   = sensitivity_level_to_msg_string((AiMonitorSensitivityLevel) level);

    if (obj && !lvl.empty()) {
        obj->command_xcam_control_ai_monitoring(m_cb_ai_monitoring->GetValue(), lvl);
    } else {
        BOOST_LOG_TRIVIAL(warning) << "print_option: obj is null or lvl = " << lvl;
    }
}

// refine printer function options
void PrintOptionsDialog::set_spaghetti_detection_sensitivity(wxCommandEvent &evt)
{
    int         level = spaghetti_detection_level_list->GetSelection();
    std::string lvl   = sensitivity_level_to_msg_string((AiMonitorSensitivityLevel) level);

    if (obj && !lvl.empty()) {
        obj->command_xcam_control_spaghetti_detection(m_cb_spaghetti_detection->GetValue(), lvl);
    } else {
        BOOST_LOG_TRIVIAL(warning) << "print_option: obj is null or lvl = " << lvl;
    }
}

void PrintOptionsDialog::set_purgechutepileup_detection_sensitivity(wxCommandEvent &evt)
{
    int         level = purgechutepileup_detection_level_list->GetSelection();
    std::string lvl   = sensitivity_level_to_msg_string((AiMonitorSensitivityLevel) level);

    if (obj && !lvl.empty()) {
        obj->command_xcam_control_purgechutepileup_detection(m_cb_purgechutepileup_detection->GetValue(), lvl);
    } else {
        BOOST_LOG_TRIVIAL(warning) << "print_option: obj is null or lvl = " << lvl;
    }
}

void PrintOptionsDialog::set_nozzleclumping_detection_sensitivity(wxCommandEvent &evt)
{
    int         level = nozzleclumping_detection_level_list->GetSelection();
    std::string lvl   = sensitivity_level_to_msg_string((AiMonitorSensitivityLevel) level);

    if (obj && !lvl.empty()) {
        obj->command_xcam_control_nozzleclumping_detection(m_cb_nozzleclumping_detection->GetValue(), lvl);
    } else {
        BOOST_LOG_TRIVIAL(warning) << "print_option: obj is null or lvl = " << lvl;
    }
}

void PrintOptionsDialog::set_airprinting_detection_sensitivity(wxCommandEvent &evt)
{
    int         level = airprinting_detection_level_list->GetSelection();
    std::string lvl   = sensitivity_level_to_msg_string((AiMonitorSensitivityLevel) level);

    if (obj && !lvl.empty()) {
        obj->command_xcam_control_airprinting_detection(m_cb_airprinting_detection->GetValue(), lvl);
    } else {
        BOOST_LOG_TRIVIAL(warning) << "print_option: obj is null or lvl = " << lvl;
    }
}

void PrintOptionsDialog::update_machine_obj(MachineObject *obj_)
{
    obj = obj_;
}

bool PrintOptionsDialog::Show(bool show)
{
    if (show) {
        wxGetApp().UpdateDlgDarkUI(this);
        CentreOnParent();
    }
    return DPIDialog::Show(show);
}

PrinterPartsDialog::PrinterPartsDialog(wxWindow* parent)
: DPIDialog(parent, wxID_ANY, _L("Printer Parts"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* single_sizer = new wxBoxSizer(wxVERTICAL);
    single_panel = new wxPanel(this);
    single_panel->SetBackgroundColour(*wxWHITE);

    wxBoxSizer* multiple_sizer = new wxBoxSizer(wxVERTICAL);
    multiple_panel = new wxPanel(this);
    multiple_panel->SetBackgroundColour(*wxWHITE);

    /*single nozzle*/
    auto single_line = new wxPanel(single_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    single_line->SetBackgroundColour(wxColour("#A6A9AA"));

    //nozzle type
    wxBoxSizer* line_sizer_nozzle_type = new wxBoxSizer(wxHORIZONTAL);

    auto nozzle_type = new Label(single_panel, _CTX(L_CONTEXT("Type", "Nozzle Type"), "Nozzle Type"));
    nozzle_type->SetFont(Label::Body_14);
    nozzle_type->SetMinSize(wxSize(FromDIP(180), -1));
    nozzle_type->SetMaxSize(wxSize(FromDIP(180), -1));
    nozzle_type->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    nozzle_type->Wrap(-1);

    nozzle_type_checkbox = new ComboBox(single_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(180), -1), 0, NULL, wxCB_READONLY);

    line_sizer_nozzle_type->Add(nozzle_type, 0, wxALIGN_CENTER, 5);
    line_sizer_nozzle_type->Add(0, 0, 1, wxEXPAND, 5);
    line_sizer_nozzle_type->Add(nozzle_type_checkbox, 0, wxALIGN_CENTER, 5);


    //nozzle diameter
    wxBoxSizer* line_sizer_nozzle_diameter = new wxBoxSizer(wxHORIZONTAL);
    auto nozzle_diameter  = new Label(single_panel, _CTX(L_CONTEXT("Diameter", "Nozzle Diameter"), "Nozzle Diameter"));
    nozzle_diameter->SetFont(Label::Body_14);
    nozzle_diameter->SetMinSize(wxSize(FromDIP(180), -1));
    nozzle_diameter->SetMaxSize(wxSize(FromDIP(180), -1));
    nozzle_diameter->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    nozzle_diameter->Wrap(-1);

    nozzle_diameter_checkbox = new ComboBox(single_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(180), -1), 0, NULL, wxCB_READONLY);

    line_sizer_nozzle_diameter->Add(nozzle_diameter, 0, wxALIGN_CENTER, 5);
    line_sizer_nozzle_diameter->Add(0, 0, 1, wxEXPAND, 5);
    line_sizer_nozzle_diameter->Add(nozzle_diameter_checkbox, 0, wxALIGN_CENTER, 5);

    //nozzle flow type
    wxBoxSizer* line_sizer_nozzle_flowtype = new wxBoxSizer(wxHORIZONTAL);
    nozzle_flow_type_label = new Label(single_panel, _CTX(L_CONTEXT("Flow", "Nozzle Flow"), "Nozzle Flow"));
    nozzle_flow_type_label->SetFont(Label::Body_14);
    nozzle_flow_type_label->SetMinSize(wxSize(FromDIP(180), -1));
    nozzle_flow_type_label->SetMaxSize(wxSize(FromDIP(180), -1));
    nozzle_flow_type_label->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    nozzle_flow_type_label->Wrap(-1);

    nozzle_flow_type_checkbox = new ComboBox(single_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(180), -1), 0, NULL, wxCB_READONLY);

    line_sizer_nozzle_flowtype->Add(nozzle_flow_type_label, 0, wxALIGN_CENTER, 5);
    line_sizer_nozzle_flowtype->Add(0, 0, 1, wxEXPAND, 5);
    line_sizer_nozzle_flowtype->Add(nozzle_flow_type_checkbox, 0, wxALIGN_CENTER, 5);

    wxSizer* h_tips_sizer = new wxBoxSizer(wxHORIZONTAL);
    change_nozzle_tips = new Label(single_panel, _L("Please change the nozzle settings on the printer."));
    change_nozzle_tips->SetFont(Label::Body_13);
    change_nozzle_tips->SetForegroundColour(STATIC_TEXT_CAPTION_COL);

    m_wiki_link = new HyperLink(single_panel, _L("Wiki Guide")); // ORCA
    m_wiki_link->SetFont(Label::Body_13);
    m_wiki_link->Bind(wxEVT_LEFT_DOWN, &PrinterPartsDialog::OnWikiClicked, this);

    h_tips_sizer->Add(change_nozzle_tips, 0, wxLEFT);
    h_tips_sizer->Add(m_wiki_link, 0,  wxLEFT, FromDIP(5));

    wxSizer* single_update_nozzle_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_single_update_nozzle_button = new Button(single_panel, _L("Refresh"));
    m_single_update_nozzle_button->SetBackgroundColor(btn_bg_green);
    m_single_update_nozzle_button->SetTextColor(wxColour("#FFFFFE"));
    m_single_update_nozzle_button->SetFont(Label::Body_14);
    m_single_update_nozzle_button->SetSize(wxSize(FromDIP(80), FromDIP(32)));
    m_single_update_nozzle_button->SetMinSize(wxSize(-1, FromDIP(32)));
    m_single_update_nozzle_button->Bind(wxEVT_BUTTON, &PrinterPartsDialog::OnNozzleRefresh, this);
    single_update_nozzle_sizer->Add(0, 0, 1, wxEXPAND, FromDIP(0));
    single_update_nozzle_sizer->Add(m_single_update_nozzle_button, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(18));

    single_sizer->Add(single_line, 0, wxEXPAND, 0);
    single_sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    single_sizer->Add(line_sizer_nozzle_type, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(10));
    single_sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    single_sizer->Add(line_sizer_nozzle_diameter, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(10));
    single_sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    single_sizer->Add(line_sizer_nozzle_flowtype, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(10));
    single_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));
    single_sizer->Add(h_tips_sizer, 0, wxLEFT, FromDIP(10));
    single_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));
    single_sizer->Add(single_update_nozzle_sizer, 0, wxLEFT | wxEXPAND, FromDIP(10));
    single_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));

    single_panel->SetSizer(single_sizer);
    single_panel->Layout();
    single_panel->Fit();

    /*multiple nozzle*/
    auto multi_line = new wxPanel(multiple_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    multi_line->SetBackgroundColour(wxColour("#A6A9AA"));

    /*left*/
    auto leftTitle = new Label(multiple_panel, _L("Left Nozzle"));
    leftTitle->SetFont(::Label::Head_14);
    leftTitle->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#2C2C2E")));

    wxBoxSizer *multiple_left_line_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto multiple_left_nozzle_type = new Label(multiple_panel, _CTX(L_CONTEXT("Type", "Nozzle Type"), "Nozzle Type"));
    multiple_left_nozzle_type->SetFont(Label::Body_14);
    multiple_left_nozzle_type->SetForegroundColour(STATIC_TEXT_CAPTION_COL);

    multiple_left_nozzle_type_checkbox = new ComboBox(multiple_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(180), -1), 0, NULL, wxCB_READONLY);

    auto multiple_left_nozzle_diameter = new Label(multiple_panel, _CTX(L_CONTEXT("Diameter", "Nozzle Diameter"), "Nozzle Diameter"));
    multiple_left_nozzle_diameter->SetFont(Label::Body_14);
    multiple_left_nozzle_diameter->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    multiple_left_nozzle_diameter_checkbox = new ComboBox(multiple_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(140), -1), 0, NULL, wxCB_READONLY);

    auto multiple_left_nozzle_flow = new Label(multiple_panel, _CTX(L_CONTEXT("Flow", "Nozzle Flow"), "Nozzle Flow"));
    multiple_left_nozzle_flow->SetFont(Label::Body_14);
    multiple_left_nozzle_flow->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    multiple_left_nozzle_flow_checkbox = new ComboBox(multiple_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(140), -1), 0, NULL, wxCB_READONLY);

    multiple_left_line_sizer->Add(multiple_left_nozzle_type, 0, wxALIGN_CENTER, 0);
    multiple_left_line_sizer->Add(0, 0, 0, wxLEFT, FromDIP(8));
    multiple_left_line_sizer->Add(multiple_left_nozzle_type_checkbox, 0, wxALIGN_CENTER, 0);
    multiple_left_line_sizer->Add(0, 0, 0, wxLEFT, FromDIP(15));
    multiple_left_line_sizer->Add(multiple_left_nozzle_diameter, 0, wxALIGN_CENTER, 0);
    multiple_left_line_sizer->Add(0, 0, 1, wxLEFT, FromDIP(8));
    multiple_left_line_sizer->Add(multiple_left_nozzle_diameter_checkbox, 0, wxALIGN_CENTER, 0);
    multiple_left_line_sizer->Add(0, 0, 0, wxLEFT, FromDIP(15));
    multiple_left_line_sizer->Add(multiple_left_nozzle_flow, 0, wxALIGN_CENTER, 0);
    multiple_left_line_sizer->Add(0, 0, 1, wxLEFT, FromDIP(8));
    multiple_left_line_sizer->Add(multiple_left_nozzle_flow_checkbox, 0, wxALIGN_CENTER, 0);

    /*right*/
    auto rightTitle = new Label(multiple_panel, _L("Right Nozzle"));
    rightTitle->SetFont(::Label::Head_14);
    rightTitle->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#2C2C2E")));

    wxBoxSizer *multiple_right_line_sizer  = new wxBoxSizer(wxHORIZONTAL);
    auto        multiple_right_nozzle_type = new Label(multiple_panel, _CTX(L_CONTEXT("Type", "Nozzle Type"), "Nozzle Type"));
    multiple_right_nozzle_type->SetFont(Label::Body_14);
    multiple_right_nozzle_type->SetForegroundColour(STATIC_TEXT_CAPTION_COL);

    multiple_right_nozzle_type_checkbox = new ComboBox(multiple_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(180), -1), 0, NULL, wxCB_READONLY);

    auto multiple_right_nozzle_diameter = new Label(multiple_panel, _CTX(L_CONTEXT("Diameter", "Nozzle Diameter"), "Nozzle Diameter"));
    multiple_right_nozzle_diameter->SetFont(Label::Body_14);
    multiple_right_nozzle_diameter->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    multiple_right_nozzle_diameter_checkbox = new ComboBox(multiple_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(140), -1), 0, NULL, wxCB_READONLY);

    auto multiple_right_nozzle_flow = new Label(multiple_panel, _CTX(L_CONTEXT("Flow", "Nozzle Flow"), "Nozzle Flow"));
    multiple_right_nozzle_flow->SetFont(Label::Body_14);
    multiple_right_nozzle_flow->SetForegroundColour(STATIC_TEXT_CAPTION_COL);
    multiple_right_nozzle_flow_checkbox = new ComboBox(multiple_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(140), -1), 0, NULL, wxCB_READONLY);

    multiple_right_line_sizer->Add(multiple_right_nozzle_type, 0, wxALIGN_CENTER, 0);
    multiple_right_line_sizer->Add(0, 0, 0, wxLEFT, FromDIP(8));
    multiple_right_line_sizer->Add(multiple_right_nozzle_type_checkbox, 0, wxALIGN_CENTER, 0);
    multiple_right_line_sizer->Add(0, 0, 0, wxLEFT, FromDIP(15));
    multiple_right_line_sizer->Add(multiple_right_nozzle_diameter, 0, wxALIGN_CENTER, 0);
    multiple_right_line_sizer->Add(0, 0, 1, wxLEFT, FromDIP(8));
    multiple_right_line_sizer->Add(multiple_right_nozzle_diameter_checkbox, 0, wxALIGN_CENTER, 0);
    multiple_right_line_sizer->Add(0, 0, 0, wxLEFT, FromDIP(15));
    multiple_right_line_sizer->Add(multiple_right_nozzle_flow, 0, wxALIGN_CENTER, 0);
    multiple_right_line_sizer->Add(0, 0, 1, wxLEFT, FromDIP(8));
    multiple_right_line_sizer->Add(multiple_right_nozzle_flow_checkbox, 0, wxALIGN_CENTER, 0);

    multiple_change_nozzle_tips = new Label(multiple_panel, _L("Please change the nozzle settings on the printer."));
    multiple_change_nozzle_tips->SetFont(Label::Body_13);
    multiple_change_nozzle_tips->SetForegroundColour(STATIC_TEXT_CAPTION_COL);

    multiple_wiki_link = new HyperLink(multiple_panel, _L("Wiki Guide")); // ORCA
    multiple_wiki_link->SetFont(Label::Body_13);
    multiple_wiki_link->Bind(wxEVT_LEFT_DOWN, &PrinterPartsDialog::OnWikiClicked, this);

    wxSizer* multiple_change_tips_sizer = new wxBoxSizer(wxHORIZONTAL);
    multiple_change_tips_sizer->Add(multiple_change_nozzle_tips, 0, wxLEFT);
    multiple_change_tips_sizer->Add(multiple_wiki_link, 0, wxLEFT, FromDIP(5));

    wxSizer* multiple_update_nozzle_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_multiple_update_nozzle_button = new Button(multiple_panel, _L("Refresh"));
    m_multiple_update_nozzle_button->SetBackgroundColor(btn_bg_green);
    m_multiple_update_nozzle_button->SetTextColor(wxColour("#FFFFFE"));
    m_multiple_update_nozzle_button->SetFont(Label::Body_14);
    m_multiple_update_nozzle_button->SetSize(wxSize(FromDIP(80), FromDIP(32)));
    m_multiple_update_nozzle_button->SetMinSize(wxSize(-1, FromDIP(32)));
    m_multiple_update_nozzle_button->Bind(wxEVT_BUTTON, &PrinterPartsDialog::OnNozzleRefresh, this);
    multiple_update_nozzle_sizer->Add(0, 0, 1, wxEXPAND, FromDIP(0));
    multiple_update_nozzle_sizer->Add(m_multiple_update_nozzle_button, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(18));

    multiple_sizer->Add(multi_line, 0, wxEXPAND, 0);
    multiple_sizer->Add(0, 0, 0, wxTOP, FromDIP(15));
    multiple_sizer->Add(leftTitle, 0, wxLEFT, FromDIP(18));
    multiple_sizer->Add(0, 0, 0, wxTOP, FromDIP(8));
    multiple_sizer->Add(multiple_left_line_sizer, 0, wxALIGN_CENTER|wxLEFT|wxRIGHT, FromDIP(18));
    multiple_sizer->Add(0, 0, 0, wxTOP, FromDIP(24));
    multiple_sizer->Add(rightTitle, 0, wxLEFT, FromDIP(18));
    multiple_sizer->Add(0, 0, 0, wxTOP, FromDIP(8));
    multiple_sizer->Add(multiple_right_line_sizer, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(18));
    multiple_sizer->Add(0, 0, 0, wxTOP, FromDIP(20));
    multiple_sizer->Add(multiple_change_tips_sizer, 0, wxLEFT, FromDIP(18));
    multiple_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));
    multiple_sizer->Add(multiple_update_nozzle_sizer, 0, wxLEFT | wxEXPAND, FromDIP(10));
    multiple_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));

    multiple_panel->SetSizer(multiple_sizer);
    multiple_panel->Layout();
    multiple_panel->Fit();

    /*inset data*/
    sizer->Add(single_panel, 0, wxEXPAND, 0);
    sizer->Add(multiple_panel, 0, wxEXPAND, 0);
    SetSizer(sizer);
    Layout();
    Fit();

    single_panel->Hide();

    wxGetApp().UpdateDlgDarkUI(this);
}

PrinterPartsDialog::~PrinterPartsDialog() {}

void PrinterPartsDialog::on_dpi_changed(const wxRect& suggested_rect) {
    m_single_update_nozzle_button->SetMinSize(wxSize(-1, FromDIP(32)));
    m_single_update_nozzle_button->SetCornerRadius(FromDIP(16));
    m_single_update_nozzle_button->Rescale();

    m_multiple_update_nozzle_button->SetMinSize(wxSize(-1, FromDIP(32)));
    m_multiple_update_nozzle_button->SetCornerRadius(FromDIP(16));
    m_multiple_update_nozzle_button->Rescale();

    Fit();
}
void PrinterPartsDialog::update_machine_obj(MachineObject* obj_) { if (obj_) { obj = obj_; }}
bool PrinterPartsDialog::Show(bool show)
{
    if (show) {
        wxGetApp().UpdateDlgDarkUI(this);
        CentreOnParent();

        /*disable editing*/
        EnableEditing(false);
        assert(DevPrinterConfigUtil::get_printer_can_set_nozzle(obj->printer_type) == false);/*editing is not supported*/

        if (obj->GetExtderSystem()->GetTotalExtderSize() <= 1) {
            single_panel->Show();
            multiple_panel->Hide();

            auto type     = obj->GetExtderSystem()->GetNozzleType(MAIN_EXTRUDER_ID);
            auto diameter = obj->GetExtderSystem()->GetNozzleDiameter(MAIN_EXTRUDER_ID);
            nozzle_type_checkbox->SetValue(GetString(type));
            nozzle_diameter_checkbox->SetValue(format_nozzle_diameter(diameter));

            // nozzle flow type
            nozzle_flow_type_label->Show(obj->is_nozzle_flow_type_supported());
            nozzle_flow_type_checkbox->Show(obj->is_nozzle_flow_type_supported());
            if (obj->is_nozzle_flow_type_supported())
            {
                auto flow_type = obj->GetExtderSystem()->GetNozzleFlowType(MAIN_EXTRUDER_ID);
                nozzle_flow_type_checkbox->SetValue(GetString(flow_type));
            }
            if (obj->is_support_refresh_nozzle) {
                m_single_update_nozzle_button->Show();
            } else {
                m_single_update_nozzle_button->Hide();
            }
        } else {
            single_panel->Hide();
            multiple_panel->Show();

            //left
            auto type      = obj->GetExtderSystem()->GetNozzleType(DEPUTY_EXTRUDER_ID);
            auto diameter  = obj->GetExtderSystem()->GetNozzleDiameter(DEPUTY_EXTRUDER_ID);
            auto flow_type = obj->GetExtderSystem()->GetNozzleFlowType(DEPUTY_EXTRUDER_ID);
            multiple_left_nozzle_type_checkbox->SetValue(GetString(type));
            multiple_left_nozzle_diameter_checkbox->SetValue(format_nozzle_diameter(diameter));
            multiple_left_nozzle_flow_checkbox->SetValue(GetString(flow_type));

            //right
            type      = obj->GetExtderSystem()->GetNozzleType(MAIN_EXTRUDER_ID);
            diameter  = obj->GetExtderSystem()->GetNozzleDiameter(MAIN_EXTRUDER_ID);
            flow_type = obj->GetExtderSystem()->GetNozzleFlowType(MAIN_EXTRUDER_ID);
            multiple_right_nozzle_type_checkbox->SetValue(GetString(type));
            multiple_right_nozzle_diameter_checkbox->SetValue(format_nozzle_diameter(diameter));
            multiple_right_nozzle_flow_checkbox->SetValue(GetString(flow_type));

            if (obj->is_support_refresh_nozzle) {
                m_multiple_update_nozzle_button->Show();
            } else {
                m_multiple_update_nozzle_button->Hide();
            }
        }

        Layout();
        Fit();
    }
    return DPIDialog::Show(show);
}

void PrinterPartsDialog::EnableEditing(bool enable) {

    nozzle_type_checkbox->Enable(enable);
    nozzle_diameter_checkbox->Enable(enable);
    nozzle_flow_type_checkbox->Enable(enable);

    multiple_left_nozzle_type_checkbox->Enable(enable);
    multiple_left_nozzle_diameter_checkbox->Enable(enable);
    multiple_left_nozzle_flow_checkbox->Enable(enable);

    multiple_right_nozzle_type_checkbox->Enable(enable);
    multiple_right_nozzle_diameter_checkbox->Enable(enable);
    multiple_right_nozzle_flow_checkbox->Enable(enable);

    change_nozzle_tips->Show(!enable);
    multiple_change_nozzle_tips->Show(!enable);
}

wxString PrinterPartsDialog::GetString(NozzleType nozzle_type) const {
    switch (nozzle_type) {
        case Slic3r::ntHardenedSteel:   return _L("Hardened Steel");
        case Slic3r::ntStainlessSteel:  return _L("Stainless Steel");
        case Slic3r::ntTungstenCarbide: return _L("Tungsten Carbide");
        case Slic3r::ntBrass:           return _L("Brass");
        case Slic3r::ntE3D:             return "E3D";
        default: break;
    }

    return _L("Unknown");
}

wxString PrinterPartsDialog::GetString(NozzleFlowType nozzle_flow_type) const {
    switch (nozzle_flow_type) {
        case Slic3r::S_FLOW: return _L("Standard");
        case Slic3r::H_FLOW: return _L("High flow");
        case Slic3r::U_FLOW: return _L("TPU High flow");
        default: break;
    }

    return wxEmptyString;
}

void PrinterPartsDialog::OnWikiClicked(wxMouseEvent& e)
{
    if (!obj) { return; }

    const wxString& url = obj->get_nozzle_replace_url();
    if (!url.IsEmpty()) {
        wxLaunchDefaultBrowser(url);
    } else {
        wxMessageBox(_L("No wiki link available for this printer."), _L("Error"), wxOK | wxICON_ERROR, this);
    }
}// PrinterPartsDialog::OnWikiClicked

void PrinterPartsDialog::OnNozzleRefresh(wxCommandEvent& e)
{
    if (!obj) { return; }

    BOOST_LOG_TRIVIAL(info) << "Send refresh nozzle command.";
    obj->command_refresh_nozzle();
}

void PrinterPartsDialog::UpdateNozzleInfo(){
    /* nozzle in checking*/
    if (obj->GetNozzleSystem()->IsRefreshing()) {
        if (single_panel->IsShown()) {
            m_single_update_nozzle_button->SetLabel(_L("Refreshing"));
            m_single_update_nozzle_button->Disable();
        } else if (multiple_panel->IsShown()) {
            m_multiple_update_nozzle_button->SetLabel(_L("Refreshing"));
            m_multiple_update_nozzle_button->Disable();
        }
        BOOST_LOG_TRIVIAL(info) << "Nozzle state in refreshing.";
        Layout();
        Fit();
        return;
    } else {
        if (single_panel->IsShown()) {
            m_single_update_nozzle_button->SetLabel(_L("Refresh"));
            m_single_update_nozzle_button->Enable();
        } else if (multiple_panel->IsShown()) {
            m_multiple_update_nozzle_button->SetLabel(_L("Refresh"));
            m_multiple_update_nozzle_button->Enable();
        }
        BOOST_LOG_TRIVIAL(info) << "Nozzle state in idle.";
    }

    if (single_panel->IsShown()) {
        auto type     = obj->GetExtderSystem()->GetNozzleType(MAIN_EXTRUDER_ID);
        auto diameter = obj->GetExtderSystem()->GetNozzleDiameter(MAIN_EXTRUDER_ID);
        nozzle_type_checkbox->SetValue(GetString(type));
        nozzle_diameter_checkbox->SetValue(format_nozzle_diameter(diameter));

        // nozzle flow type
        nozzle_flow_type_label->Show(obj->is_nozzle_flow_type_supported());
        nozzle_flow_type_checkbox->Show(obj->is_nozzle_flow_type_supported());
        if (obj->is_nozzle_flow_type_supported())
        {
            auto flow_type = obj->GetExtderSystem()->GetNozzleFlowType(MAIN_EXTRUDER_ID);
            nozzle_flow_type_checkbox->SetValue(GetString(flow_type));
        }
    } else if(multiple_panel->IsShown()){
        //left
        auto type      = obj->GetExtderSystem()->GetNozzleType(DEPUTY_EXTRUDER_ID);
        auto diameter  = obj->GetExtderSystem()->GetNozzleDiameter(DEPUTY_EXTRUDER_ID);
        auto flow_type = obj->GetExtderSystem()->GetNozzleFlowType(DEPUTY_EXTRUDER_ID);
        multiple_left_nozzle_type_checkbox->SetValue(GetString(type));
        multiple_left_nozzle_diameter_checkbox->SetValue(format_nozzle_diameter(diameter));
        multiple_left_nozzle_flow_checkbox->SetValue(GetString(flow_type));

        //right
        type      = obj->GetExtderSystem()->GetNozzleType(MAIN_EXTRUDER_ID);
        diameter  = obj->GetExtderSystem()->GetNozzleDiameter(MAIN_EXTRUDER_ID);
        flow_type = obj->GetExtderSystem()->GetNozzleFlowType(MAIN_EXTRUDER_ID);
        multiple_right_nozzle_type_checkbox->SetValue(GetString(type));
        multiple_right_nozzle_diameter_checkbox->SetValue(format_nozzle_diameter(diameter));
        multiple_right_nozzle_flow_checkbox->SetValue(GetString(flow_type));
    }

    Layout();
    Fit();
}

}} // namespace Slic3r::GUI
