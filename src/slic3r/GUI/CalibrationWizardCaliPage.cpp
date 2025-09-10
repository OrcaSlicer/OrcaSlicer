#include "CalibrationWizardCaliPage.hpp"
#include "MainFrame.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"

#include "DeviceCore/DevManager.h"

namespace Slic3r { namespace GUI {

static const wxString NA_STR = _L("N/A");


CalibrationCaliPage::CalibrationCaliPage(wxWindow* parent, CalibMode cali_mode, CaliPageType cali_type,
    wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : CalibrationWizardPage(parent, id, pos, size, style)
{
    m_cali_mode = cali_mode;

    m_page_type = cali_type;

    m_top_sizer = new wxBoxSizer(wxVERTICAL);

    create_page(this);

    this->SetSizer(m_top_sizer);
    Layout();
    m_top_sizer->Fit(this);
}

CalibrationCaliPage::~CalibrationCaliPage()
{
    m_printing_panel->get_pause_resume_button()->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(CalibrationCaliPage::on_subtask_pause_resume), NULL, this);
    m_printing_panel->get_abort_button()->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(CalibrationCaliPage::on_subtask_abort), NULL, this);
}

void CalibrationCaliPage::create_page(wxWindow* parent)
{
    m_page_caption = new CaliPageCaption(parent, m_cali_mode);
    m_page_caption->show_prev_btn(true);
    m_top_sizer->Add(m_page_caption, 0, wxEXPAND, 0);

    wxArrayString steps;
    steps.Add(_L("Preset"));
    steps.Add(_L("Calibration"));
    steps.Add(_L("Record Factor"));
    m_step_panel = new CaliPageStepGuide(parent, steps);
    m_step_panel->set_steps(1);
    m_top_sizer->Add(m_step_panel, 0, wxEXPAND, 0);

    m_picture_panel = new CaliPagePicture(parent);
    m_top_sizer->Add(m_picture_panel, 0, wxEXPAND, 0);
    m_top_sizer->AddSpacer(FromDIP(20));

    set_cali_img();

    m_printing_panel = new PrintingTaskPanel(parent, PrintingTaskType::CALIBRATION);
    m_printing_panel->SetDoubleBuffered(true);
    m_printing_panel->SetSize({ CALIBRATION_PROGRESSBAR_LENGTH, -1 });
    m_printing_panel->SetMinSize({ CALIBRATION_PROGRESSBAR_LENGTH, -1 });
    m_printing_panel->enable_pause_resume_button(false, "resume_disable");
    m_printing_panel->enable_abort_button(false);

    m_top_sizer->Add(m_printing_panel, 0, wxALIGN_CENTER, 0);
    m_action_panel = new CaliPageActionPanel(parent, m_cali_mode, CaliPageType::CALI_PAGE_CALI);
    m_top_sizer->Add(m_action_panel, 0, wxEXPAND, 0);

    m_printing_panel->get_pause_resume_button()->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(CalibrationCaliPage::on_subtask_pause_resume), NULL, this);
    m_printing_panel->get_abort_button()->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(CalibrationCaliPage::on_subtask_abort), NULL, this);
}

void CalibrationCaliPage::on_subtask_pause_resume(wxCommandEvent& event)
{
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;
    MachineObject* obj = dev->get_selected_machine();
    if (!obj) return;

    if (obj->can_resume())
        obj->command_task_resume();
    else
        obj->command_task_pause();
}

void CalibrationCaliPage::on_subtask_abort(wxCommandEvent& event)
{
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;
    MachineObject* obj = dev->get_selected_machine();
    if (!obj) return;

    if (abort_dlg == nullptr) {
        abort_dlg = new SecondaryCheckDialog(this->GetParent(), wxID_ANY, _L("Cancel print"));
        abort_dlg->Bind(EVT_SECONDARY_CHECK_CONFIRM, [this, obj](wxCommandEvent& e) {
            if (obj) obj->command_task_abort();
            });
    }
    abort_dlg->update_text(_L("Are you sure you want to cancel this print?"));
    abort_dlg->on_show();
}

void CalibrationCaliPage::set_cali_img()
{
    if (m_cali_mode == CalibMode::Calib_PA_Line) {
        if (m_cali_method == CalibrationMethod::CALI_METHOD_MANUAL) {
            CalibrationMethod method;
            int               cali_stage    = 0;
            CalibMode         obj_cali_mode = get_obj_calibration_mode(curr_obj, method, cali_stage);
            set_pa_cali_image(cali_stage);
        }
        else if (m_cali_method == CalibrationMethod::CALI_METHOD_AUTO || m_cali_method == CalibrationMethod::CALI_METHOD_NEW_AUTO) {
            if (curr_obj) {
                std::string image_name = curr_obj->get_auto_pa_cali_thumbnail_img_str();
                if (curr_obj->is_multi_extruders()) {
                    if (m_cur_extruder_id == 0) {
                        image_name += "_right";
                    } else {
                        image_name += "_left";
                    }
                }
                m_picture_panel->set_bmp(ScalableBitmap(this, image_name, 400));
            }
            else {
                m_picture_panel->set_bmp(ScalableBitmap(this, "fd_calibration_auto", 400));
            }
        }
    }
    else if (m_cali_mode == CalibMode::Calib_Flow_Rate) {
        if (m_cali_method == CalibrationMethod::CALI_METHOD_MANUAL) {
            if (m_page_type == CaliPageType::CALI_PAGE_CALI)
                m_picture_panel->set_bmp(ScalableBitmap(this, "flow_rate_calibration_coarse", 400));
            if (m_page_type == CaliPageType::CALI_PAGE_FINE_CALI)
                m_picture_panel->set_bmp(ScalableBitmap(this, "flow_rate_calibration_fine", 400));
            else
                m_picture_panel->set_bmp(ScalableBitmap(this, "flow_rate_calibration_coarse", 400));
        }
        else if (m_cali_method == CalibrationMethod::CALI_METHOD_AUTO) {
            m_picture_panel->set_bmp(ScalableBitmap(this, "flow_rate_calibration_auto", 400));
        }
    }
    else if (m_cali_mode == CalibMode::Calib_Vol_speed_Tower) {
        m_picture_panel->set_bmp(ScalableBitmap(this, "max_volumetric_speed_calibration", 400));
    }
}

void CalibrationCaliPage::set_pa_cali_image(int stage)
{
    if (m_cali_mode == CalibMode::Calib_PA_Line && m_cali_method == CALI_METHOD_MANUAL) {
        if (stage == 0) {
            m_picture_panel->set_bmp(ScalableBitmap(this, "fd_calibration_manual", 400));
        } else if (stage == 1) {
            m_picture_panel->set_bmp(ScalableBitmap(this, "fd_pattern_manual", 400));
        }
    }
}

void CalibrationCaliPage::clear_last_job_status()
{
    m_is_between_start_and_running = true;
}

void CalibrationCaliPage::update(MachineObject* obj)
{
    if (this->IsShown()) {
        if (obj) {
            if (obj->print_status != "RUNNING") {
                BOOST_LOG_TRIVIAL(info) << "on_show_cali_page - machine object status:"
                                        << " dev_id = " << obj->get_dev_id()
                                        << ", print_type = " << obj->printer_type
                                        << ", printer_status = " << obj->print_status
                                        << ", is_connected = " << obj->is_connected()
                                        << ", m_is_between_start_and_running = " << m_is_between_start_and_running
                                        << ", cali_finished = " << obj->cali_finished
                                        << ", cali_version = " << obj->cali_version
                                        << ", cache_flow_ratio = " << obj->cache_flow_ratio
                                        << ", sub_task_name = " << obj->subtask_name
                                        << ", gcode_file_name = " << obj->m_gcode_file
                                        << ", get_pa_calib_result" << obj->get_pa_calib_result
                                        << ", get_flow_calib_result" << obj->get_flow_calib_result;
            }
        }
        else {
            BOOST_LOG_TRIVIAL(info) << "on_show_cali_page - machine object is nullptr";
        }
    }

    static int get_result_count = 0;
    // enable calibration when finished
    bool enable_cali = false;
    if (obj) {
        if (obj->GetExtderSystem()->GetCurrentExtderId() != m_cur_extruder_id) {
            m_cur_extruder_id = obj->GetExtderSystem()->GetCurrentExtderId();
            set_cali_img();
        }

        if (obj->print_error > 0) {
            StatusPanel* status_panel = Slic3r::GUI::wxGetApp().mainframe->m_monitor->get_status_panel();
            status_panel->obj = obj;
            status_panel->update_error_message();
        }

        if (obj->print_status == "RUNNING")
            m_is_between_start_and_running = false;
        if (obj->is_connecting() || !obj->is_connected() || m_is_between_start_and_running) {
            reset_printing_values();
            m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_CALI_NEXT, false);
            return;
        }

        if (m_cali_mode == CalibMode::Calib_PA_Line || m_cali_mode == CalibMode::Calib_Auto_PA_Line) {
            if (m_cali_method == CalibrationMethod::CALI_METHOD_AUTO || m_cali_method == CalibrationMethod::CALI_METHOD_NEW_AUTO) {
                if (get_obj_calibration_mode(obj) == m_cali_mode) {
                    if (obj->is_printing_finished()) {
                        if (obj->print_status == "FINISH") {
                            if (obj->get_pa_calib_result) {
                                enable_cali = true;
                            }
                            else {
                                // use selected diameter, add a counter to timeout, add a warning tips when get result failed
                                CalibUtils::emit_get_PA_calib_results(get_selected_calibration_nozzle_dia(obj));
                                BOOST_LOG_TRIVIAL(trace) << "CalibUtils::emit_get_PA_calib_results, auto count = " << get_result_count++;
                                enable_cali = false;
                            }
                        }
                        else if (obj->print_status == "FAILED") {
                            enable_cali = false;
                        }
                    }
                    else {
                        enable_cali = false;
                    }
                }
            } else if (m_cali_method == CalibrationMethod::CALI_METHOD_MANUAL) {
                if (get_obj_calibration_mode(obj) == m_cali_mode && obj->is_printing_finished()) {
                    enable_cali = true;
                } else {
                    enable_cali = false;
                }
            } else {
                assert(false);
            }
            m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_CALI_NEXT, enable_cali);
        } else if (m_cali_mode == CalibMode::Calib_Flow_Rate) {
            if (m_cali_method == CalibrationMethod::CALI_METHOD_AUTO) {
                if (get_obj_calibration_mode(obj) == m_cali_mode) {
                    if (obj->is_printing_finished()) {
                        if (obj->print_status == "FINISH") {
                            if (obj->get_flow_calib_result) {
                                enable_cali = true;
                            }
                            else {
                                // use selected diameter, add a counter to timeout, add a warning tips when get result failed
                                CalibUtils::emit_get_flow_ratio_calib_results(get_selected_calibration_nozzle_dia(obj));
                                enable_cali = false;
                            }
                        }
                        else if (obj->print_status == "FAILED") {
                            enable_cali = false;
                        }
                    }
                    else {
                        enable_cali = false;
                    }
                }
            } else if (m_cali_method == CalibrationMethod::CALI_METHOD_MANUAL) {
                if (get_obj_calibration_mode(obj) == m_cali_mode && obj->is_printing_finished()) {
                    // use selected diameter, add a counter to timeout, add a warning tips when get result failed
                    enable_cali = true;
                }
                else {
                    enable_cali = false;
                }
            } else {
                //assert(false);
            }
            m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_CALI_NEXT, enable_cali);
        } 
        else if (m_cali_mode == CalibMode::Calib_Vol_speed_Tower) {
            if (get_obj_calibration_mode(obj) == m_cali_mode && obj->is_printing_finished()) {
                enable_cali = true;
            } else {
                enable_cali = false;
            }
        }
        else {
            assert(false);
        }
        m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_NEXT, enable_cali);
    }

    // only display calibration printing status
    if (get_obj_calibration_mode(obj) == m_cali_mode) {
        update_subtask(obj);
    } else {
        update_subtask(nullptr);
    }
}

void CalibrationCaliPage::update_subtask(MachineObject* obj)
{
    if (!obj) return;

    if (obj->is_support_layer_num) {
        m_printing_panel->update_layers_num(true);
    }
    else {
        m_printing_panel->update_layers_num(false);
    }

    if (obj->is_system_printing()
        || obj->is_in_calibration()) {
        reset_printing_values();
    }
    else if (obj->is_in_printing() || obj->print_status == "FINISH") {
        if (obj->is_in_prepare() || obj->print_status == "SLICING") {
            m_printing_panel->get_market_scoring_button()->Hide();
            m_printing_panel->enable_abort_button(false);
            m_printing_panel->enable_pause_resume_button(false, "pause_disable");
            wxString prepare_text;
            bool show_percent = true;

            if (obj->is_in_prepare()) {
                prepare_text = wxString::Format(_L("Downloading..."));
            }
            else if (obj->print_status == "SLICING") {
                if (obj->queue_number <= 0) {
                    prepare_text = wxString::Format(_L("Cloud Slicing..."));
                }
                else {
                    prepare_text = wxString::Format(_L("In Cloud Slicing Queue, there are %s tasks ahead."), std::to_string(obj->queue_number));
                    show_percent = false;
                }
            }
            else
                prepare_text = wxString::Format(_L("Downloading..."));

            if (obj->gcode_file_prepare_percent >= 0 && obj->gcode_file_prepare_percent <= 100 && show_percent)
                prepare_text += wxString::Format("(%d%%)", obj->gcode_file_prepare_percent);

            m_printing_panel->update_stage_value(prepare_text, 0);
            m_printing_panel->update_progress_percent(NA_STR, wxEmptyString);
            m_printing_panel->update_left_time(NA_STR);
            m_printing_panel->update_layers_num(true, wxString::Format(_L("Layer: %s"), NA_STR));
            m_printing_panel->update_subtask_name(wxString::Format("%s", GUI::from_u8(obj->subtask_name)));


            if (obj->get_modeltask() && obj->get_modeltask()->design_id > 0) {
                m_printing_panel->show_profile_info(true, wxString::FromUTF8(obj->get_modeltask()->profile_name));
            }
            else {
                m_printing_panel->show_profile_info(false);
            }

            if (obj->slice_info)
                update_basic_print_data(false, obj->slice_info->weight, obj->slice_info->prediction);
        }
        else {
            if (obj->can_resume()) {
                m_printing_panel->enable_pause_resume_button(true, "resume");

            }
            else {
                m_printing_panel->enable_pause_resume_button(true, "pause");
            }
            if (obj->print_status == "FINISH") {

                m_printing_panel->enable_abort_button(false);
                m_printing_panel->enable_pause_resume_button(false, "resume_disable");

                bool is_market_task = obj->get_modeltask() && obj->get_modeltask()->design_id > 0;
                if (is_market_task) {
                    m_printing_panel->get_market_scoring_button()->Show();
                    BOOST_LOG_TRIVIAL(info) << "SHOW_SCORE_BTU: design_id [" << obj->get_modeltask()->design_id << "] print_finish [" << m_print_finish << "]";
                    if (!m_print_finish && IsShownOnScreen()) {
                        m_print_finish = true;
                    }
                }
                else {
                    m_printing_panel->get_market_scoring_button()->Hide();
                }
            }
            else {
                m_printing_panel->enable_abort_button(true);
                m_printing_panel->get_market_scoring_button()->Hide();
                if (m_print_finish) {
                    m_print_finish = false;
                }
            }
            // update printing stage

            m_printing_panel->update_left_time(obj->mc_left_time);
            if (obj->subtask_) {
                m_printing_panel->update_stage_value(obj->get_curr_stage(), obj->subtask_->task_progress);
                m_printing_panel->update_progress_percent(wxString::Format("%d", obj->subtask_->task_progress), "%");
                m_printing_panel->update_layers_num(true, wxString::Format(_L("Layer: %d/%d"), obj->curr_layer, obj->total_layers));

            }
            else {
                m_printing_panel->update_stage_value(obj->get_curr_stage(), 0);
                m_printing_panel->update_progress_percent(NA_STR, wxEmptyString);
                m_printing_panel->update_layers_num(true, wxString::Format(_L("Layer: %s"), NA_STR));
            }
        }

        m_printing_panel->update_subtask_name(wxString::Format("%s", GUI::from_u8(obj->subtask_name)));

        if (obj->get_modeltask() && obj->get_modeltask()->design_id > 0) {
            m_printing_panel->show_profile_info(wxString::FromUTF8(obj->get_modeltask()->profile_name));
        }
        else {
            m_printing_panel->show_profile_info(false);
        }

    }
    else {
        reset_printing_values();
    }

    this->Layout();
    this->Fit();
}

void CalibrationCaliPage::update_basic_print_data(bool def, float weight, int prediction)
{
    if (def) {
        wxString str_prediction = wxString::Format("%s", get_bbl_time_dhms(prediction));
        wxString str_weight = wxString::Format("%.2fg", weight);

        m_printing_panel->show_priting_use_info(true, str_prediction, str_weight);
    }
    else {
        m_printing_panel->show_priting_use_info(false, "0m", "0g");
    }
}

void CalibrationCaliPage::reset_printing_values()
{
    m_printing_panel->enable_pause_resume_button(false, "pause_disable");
    m_printing_panel->enable_abort_button(false);
    m_printing_panel->reset_printing_value();
    m_printing_panel->update_subtask_name(NA_STR);
    m_printing_panel->show_profile_info(false);
    m_printing_panel->update_stage_value(wxEmptyString, 0);
    m_printing_panel->update_progress_percent(NA_STR, wxEmptyString);
    m_printing_panel->get_market_scoring_button()->Hide();
    m_printing_panel->update_left_time(NA_STR);
    m_printing_panel->update_layers_num(true, wxString::Format(_L("Layer: %s"), NA_STR));
    update_basic_print_data(false);
    this->Layout();
    this->Fit();
}

void CalibrationCaliPage::on_device_connected(MachineObject* obj)
{
    reset_printing_values();
}

void CalibrationCaliPage::set_cali_method(CalibrationMethod method)
{
    m_cali_method = method;

    set_cali_img();

    wxArrayString auto_steps;
    auto_steps.Add(_L("Preset"));
    auto_steps.Add(_L("Calibration"));
    auto_steps.Add(_L("Record Factor"));

    wxArrayString manual_steps;
    manual_steps.Add(_L("Preset"));
    manual_steps.Add(_L("Calibration1"));
    manual_steps.Add(_L("Calibration2"));
    manual_steps.Add(_L("Record Factor"));

    if (method == CalibrationMethod::CALI_METHOD_AUTO || method == CalibrationMethod::CALI_METHOD_NEW_AUTO) {
        m_step_panel->set_steps_string(auto_steps);
        m_step_panel->set_steps(1);
    }
    else if (method == CalibrationMethod::CALI_METHOD_MANUAL) {
        if (m_cali_mode == CalibMode::Calib_PA_Line) {
            m_step_panel->set_steps_string(auto_steps);
            m_step_panel->set_steps(1);
        } else {
            m_step_panel->set_steps_string(manual_steps);
            if (m_page_type == CaliPageType::CALI_PAGE_CALI)
                m_step_panel->set_steps(1);
            else if (m_page_type == CaliPageType::CALI_PAGE_FINE_CALI) {
                m_step_panel->set_steps(2);
            }
            else {
                m_step_panel->set_steps(1);
            }
        }
    }
    else {
        assert(false);
    }
}

bool CalibrationCaliPage::Show(bool show /*= true*/)
{
    if (true) {
        reset_printing_values();
    }
    return wxPanel::Show(show);
}

void CalibrationCaliPage::msw_rescale()
{
    CalibrationWizardPage::msw_rescale();
    m_picture_panel->msw_rescale();
}

float CalibrationCaliPage::get_selected_calibration_nozzle_dia(MachineObject* obj)
{
    // return selected if this is set
    if (obj->cali_selected_nozzle_dia > 1e-3 && obj->cali_selected_nozzle_dia < 10.0f)
        return obj->cali_selected_nozzle_dia;

    // return default nozzle if nozzle diameter is set
    if (obj->GetExtderSystem()->GetNozzleDiameter(0) > 1e-3 && obj->GetExtderSystem()->GetNozzleDiameter(0) < 10.0f)
        return obj->GetExtderSystem()->GetNozzleDiameter(0);

    // return 0.4 by default
    return 0.4;
}

}}