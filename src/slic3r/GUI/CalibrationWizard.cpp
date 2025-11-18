#include "CalibrationWizard.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "CalibrationWizardPage.hpp"
#include "../../libslic3r/calib.hpp"
#include "Tabbook.hpp"
#include "CaliHistoryDialog.hpp"

namespace Slic3r { namespace GUI {

#define CALIBRATION_DEBUG

wxDEFINE_EVENT(EVT_DEVICE_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_CALIBRATION_JOB_FINISHED, wxCommandEvent);

static const wxString NA_STR = _L("N/A");
static const float MIN_PA_K_VALUE_STEP = 0.001;
static const int MAX_PA_HISTORY_RESULTS_NUMS = 16;

std::map<int, Preset*> get_cached_selected_filament(MachineObject* obj) {
    std::map<int, Preset*> selected_filament_map;
    if (!obj) return selected_filament_map;

    PresetCollection* filament_presets = &wxGetApp().preset_bundle->filaments;
    for (auto selected_prest : obj->selected_cali_preset) {
        Preset* preset = filament_presets->find_preset(selected_prest.name);
        if (!preset)
            continue;

        selected_filament_map.emplace(std::make_pair(selected_prest.tray_id, preset));
    }
    return selected_filament_map;
}

struct TrayInfo
{
    int     extruder_id;
    NozzleVolumeType nozzle_volume_type;
    Preset *preset;
};
std::map<int, TrayInfo> get_cached_selected_filament_for_multi_extruder(MachineObject *obj)
{
    std::map<int, TrayInfo> selected_filament_map;
    if (!obj)
        return selected_filament_map;

    PresetCollection *filament_presets = &wxGetApp().preset_bundle->filaments;
    for (auto selected_prest : obj->selected_cali_preset) {
        TrayInfo tray_info;
        tray_info.preset = filament_presets->find_preset(selected_prest.name);
        if (!tray_info.preset)
            continue;

        tray_info.extruder_id = selected_prest.extruder_id;
        tray_info.nozzle_volume_type = selected_prest.nozzle_volume_type;
        selected_filament_map.emplace(std::make_pair(selected_prest.tray_id, tray_info));
    }
    return selected_filament_map;
}

bool is_pa_params_valid(const Calib_Params& params)
{
    if (params.start < MIN_PA_K_VALUE || params.end > MAX_PA_K_VALUE || params.step < EPSILON || params.end < params.start + params.step) {
        MessageDialog msg_dlg(nullptr,
            wxString::Format(_L("Please input valid values:\nStart value: >= %.1f\nEnd value: <= %.1f\nEnd value: > Start value\nValue step: >= %.3f"), MIN_PA_K_VALUE, MAX_PA_K_VALUE, MIN_PA_K_VALUE_STEP),
            wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return false;
    }
    return true;
}

CalibrationWizard::CalibrationWizard(wxWindow* parent, CalibMode mode, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : wxPanel(parent, id, pos, size, style)
    , m_mode(mode)
{
    SetBackgroundColour(wxColour(0xEEEEEE));

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    m_scrolledWindow = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxVSCROLL);
    m_scrolledWindow->SetScrollRate(5, 5);
    m_scrolledWindow->SetBackgroundColour(*wxWHITE);

    wxBoxSizer* padding_sizer = new wxBoxSizer(wxHORIZONTAL);
    padding_sizer->Add(0, 0, 1);

    m_all_pages_sizer = new wxBoxSizer(wxVERTICAL);
    padding_sizer->Add(m_all_pages_sizer, 0);

    padding_sizer->Add(0, 0, 1);

    m_scrolledWindow->SetSizer(padding_sizer);

    main_sizer->Add(m_scrolledWindow, 1, wxEXPAND | wxALL, FromDIP(10));

    this->SetSizer(main_sizer);
    this->Layout();
    main_sizer->Fit(this);

    Bind(EVT_CALIBRATION_JOB_FINISHED, &CalibrationWizard::on_cali_job_finished, this);

#if !BBL_RELEASE_TO_PUBLIC
    this->Bind(wxEVT_CHAR_HOOK, [this](auto& evt) {
        const int keyCode = evt.GetKeyCode();
        switch (keyCode)
        {
        case WXK_PAGEUP:
        {
            show_step(m_curr_step->prev);
            break;
        }
        case WXK_PAGEDOWN:
        {
            show_step(m_curr_step->next);
            break;
        }
        default:
            evt.Skip();
            break;
        }
        });
#endif
}

CalibrationWizard::~CalibrationWizard()
{
    ;
}

void CalibrationWizard::on_cali_job_finished(wxCommandEvent& event)
{
    this->on_cali_job_finished(event.GetString());
    event.Skip();
}

void CalibrationWizard::show_step(CalibrationWizardPageStep* step)
{
    if (!step)
        return;

    if (m_curr_step) {
        m_curr_step->page->Hide();
    }

    m_curr_step = step;

    if (m_curr_step) {
        m_curr_step->page->Show();
    }

    Layout();
}

void CalibrationWizard::update(MachineObject* obj)
{
    curr_obj = obj;

    /* only update curr step
    if (m_curr_step) {
        m_curr_step->page->update(obj);
    }
    */
    if (!obj) {
        for (int i = 0; i < m_page_steps.size(); i++) {
            if (m_page_steps[i]->page)
                m_page_steps[i]->page->on_reset_page();
        }
    }

    // update all page steps
    for (int i = 0; i < m_page_steps.size(); i++) {
        if (m_page_steps[i]->page)
            m_page_steps[i]->page->update(obj);
    }
}

void CalibrationWizard::on_device_connected(MachineObject* obj)
{
    if (!m_page_steps.empty())
        show_step(m_page_steps.front());

    recover_preset_info(obj);

    BOOST_LOG_TRIVIAL(info) << "on_device_connected - machine object status:"
                            << " dev_id = " << obj->get_dev_id()
                            << ", print_type = " << obj->printer_type
                            << ", printer_status = " << obj->print_status
                            << ", cali_finished = " << obj->cali_finished
                            << ", cali_version = " << obj->cali_version
                            << ", cache_flow_ratio = " << obj->cache_flow_ratio
                            << ", sub_task_name = " << obj->subtask_name
                            << ", gcode_file_name = " << obj->m_gcode_file;

    for (const CaliPresetInfo& preset_info : obj->selected_cali_preset) {
        BOOST_LOG_TRIVIAL(info) << "on_device_connected - selected preset: "
                                 << "tray_id = " << preset_info.tray_id
                                 << ", nozzle_diameter = " << preset_info.nozzle_diameter
                                 << ", filament_id = " << preset_info.filament_id
                                 << ", settring_id = " << preset_info.setting_id
                                 << ", name = " << preset_info.name;
    }

    for (int i = 0; i < m_page_steps.size(); i++) {
        if (m_page_steps[i]->page)
            m_page_steps[i]->page->on_device_connected(obj);
    }
}

void CalibrationWizard::set_cali_method(CalibrationMethod method)
{
    m_cali_method = method;
    for (int i = 0; i < m_page_steps.size(); i++) {
        if (m_page_steps[i]->page)
            m_page_steps[i]->page->set_cali_method(method);
    }
}

bool CalibrationWizard::save_preset(const std::string &old_preset_name, const std::string &new_preset_name, const std::map<std::string, ConfigOption *> &key_values, wxString& message)
{
    if (new_preset_name.empty()) {
        message = _L("The name cannot be empty.");
        return false;
    }

    PresetCollection *filament_presets = &wxGetApp().preset_bundle->filaments;
    Preset* preset = filament_presets->find_preset(old_preset_name);
    if (!preset) {
        message = wxString::Format(_L("The selected preset: %s was not found."), old_preset_name);
        return false;
    }

    Preset temp_preset = *preset;

    std::string new_name = filament_presets->get_preset_name_by_alias(new_preset_name);
    bool exist_preset = false;
    // If name is current, get the editing preset
    Preset *new_preset = filament_presets->find_preset(new_name);
    if (new_preset) {
        if (new_preset->is_system) {
            message = _L("The name cannot be the same as the system preset name.");
            return false;
        }

        if (new_preset != preset) {
            message = _L("The name is the same as another existing preset name");
            return false;
        }
        if (new_preset != &filament_presets->get_edited_preset()) new_preset = &temp_preset;
        exist_preset = true;
    } else {
        new_preset = &temp_preset;
    }

    for (auto item : key_values) {
        new_preset->config.set_key_value(item.first, item.second);
    }

    // Save the preset into Slic3r::data_dir / presets / section_name / preset_name.ini
    filament_presets->save_current_preset(new_name, false, false, new_preset);

    // BBS create new settings
    new_preset = filament_presets->find_preset(new_name, false, true);
    // Preset* preset = &m_presets.preset(it - m_presets.begin(), true);
    if (!new_preset) {
        BOOST_LOG_TRIVIAL(info) << "create new preset failed";
        message = _L("create new preset failed.");
        return false;
    }

    // set sync_info for sync service
    if (exist_preset) {
        new_preset->sync_info = "update";
        BOOST_LOG_TRIVIAL(info) << "sync_preset: update preset = " << new_preset->name;
    } else {
        new_preset->sync_info = "create";
        if (wxGetApp().is_user_login()) new_preset->user_id = wxGetApp().getAgent()->get_user_id();
        BOOST_LOG_TRIVIAL(info) << "sync_preset: create preset = " << new_preset->name;
    }
    new_preset->save_info();

    // Mark the print & filament enabled if they are compatible with the currently selected preset.
    // If saving the preset changes compatibility with other presets, keep the now incompatible dependent presets selected, however with a "red flag" icon showing that they are
    // no more compatible.
    wxGetApp().preset_bundle->update_compatible(PresetSelectCompatibleType::Never);

    // BBS if create a new prset name, preset changed from preset name to new preset name
    if (!exist_preset) { wxGetApp().plater()->sidebar().update_presets_from_to(Preset::Type::TYPE_FILAMENT, old_preset_name, new_preset->name); }

    // todo: zhimin
    // wxGetApp().mainframe->update_filament_tab_ui();
    return true;
}

bool CalibrationWizard::save_preset_with_index(const std::string &old_preset_name, const std::string &new_preset_name, const std::map<std::string, ConfigIndexValue> &key_values, wxString &message)
{
    if (new_preset_name.empty()) {
        message = _L("The name cannot be empty.");
        return false;
    }

    PresetCollection *filament_presets = &wxGetApp().preset_bundle->filaments;
    Preset           *preset           = filament_presets->find_preset(old_preset_name);
    if (!preset) {
        message = wxString::Format(_L("The selected preset: %s is not found."), old_preset_name);
        return false;
    }

    Preset temp_preset = *preset;

    std::string new_name     = filament_presets->get_preset_name_by_alias(new_preset_name);
    bool        exist_preset = false;
    // If name is current, get the editing preset
    Preset *new_preset = filament_presets->find_preset(new_name);
    if (new_preset) {
        if (new_preset->is_system) {
            message = _L("The name cannot be the same as the system preset name.");
            return false;
        }

        if (new_preset != preset) {
            message = _L("The name is the same as another existing preset name");
            return false;
        }
        if (new_preset != &filament_presets->get_edited_preset())
            new_preset = &temp_preset;
        exist_preset = true;
    } else {
        new_preset = &temp_preset;
    }

    for (auto item : key_values) {
        auto config_opt = new_preset->config.option<ConfigOptionFloatsNullable>(item.first);
        if (config_opt) {
            auto& config_value = config_opt->values;
            config_value[item.second.index] = item.second.value;
        }
        else {
            message = wxString::Format(_L("Could not find parameter: %s."), item.first);
        }
    }

    // Save the preset into Slic3r::data_dir / presets / section_name / preset_name.ini
    filament_presets->save_current_preset(new_name, false, false, new_preset);

    // BBS create new settings
    new_preset = filament_presets->find_preset(new_name, false, true);
    // Preset* preset = &m_presets.preset(it - m_presets.begin(), true);
    if (!new_preset) {
        BOOST_LOG_TRIVIAL(info) << "create new preset failed";
        message = _L("create new preset failed.");
        return false;
    }

    // set sync_info for sync service
    if (exist_preset) {
        new_preset->sync_info = "update";
        BOOST_LOG_TRIVIAL(info) << "sync_preset: update preset = " << new_preset->name;
    } else {
        new_preset->sync_info = "create";
        if (wxGetApp().is_user_login()) new_preset->user_id = wxGetApp().getAgent()->get_user_id();
        BOOST_LOG_TRIVIAL(info) << "sync_preset: create preset = " << new_preset->name;
    }
    new_preset->save_info();

    // Mark the print & filament enabled if they are compatible with the currently selected preset.
    // If saving the preset changes compatibility with other presets, keep the now incompatible dependent presets selected, however with a "red flag" icon showing that they are
    // no more compatible.
    wxGetApp().preset_bundle->update_compatible(PresetSelectCompatibleType::Never);

    // BBS if create a new prset name, preset changed from preset name to new preset name
    if (!exist_preset) { wxGetApp().plater()->sidebar().update_presets_from_to(Preset::Type::TYPE_FILAMENT, old_preset_name, new_preset->name); }

    return true;
}

void CalibrationWizard::cache_preset_info(MachineObject *obj, float nozzle_dia, BedType bed_type)
{
    if (!obj) return;

    CalibrationPresetPage* preset_page = (static_cast<CalibrationPresetPage*>(preset_step->page));

    std::map<int, Preset*> selected_filaments = preset_page->get_selected_filaments();

    obj->selected_cali_preset.clear();
    for (auto& item : selected_filaments) {
        CaliPresetInfo result;
        result.tray_id = item.first;
        result.nozzle_diameter = nozzle_dia;
        result.bed_type        = bed_type;
        result.filament_id = item.second->filament_id;
        result.setting_id = item.second->setting_id;
        result.name = item.second->name;

        if (obj->is_multi_extruders()) {
            int ams_id, slot_id, tray_id;
            get_tray_ams_and_slot_id(curr_obj, result.tray_id, ams_id, slot_id, tray_id);
            result.extruder_id = preset_page->get_extruder_id(ams_id);
            result.nozzle_diameter  = preset_page->get_nozzle_diameter(result.extruder_id);
        }
        else {
            result.extruder_id = 0;
        }
        result.nozzle_volume_type = preset_page->get_nozzle_volume_type(result.extruder_id);

        obj->selected_cali_preset.push_back(result);
    }

    CaliPresetStage stage;
    preset_page->get_cali_stage(stage, obj->cache_flow_ratio);

    back_preset_info(obj, false);
}

void CalibrationWizard::recover_preset_info(MachineObject *obj)
{
    std::vector<PrinterCaliInfo> back_infos = wxGetApp().app_config->get_printer_cali_infos();
    for (const auto& back_info : back_infos) {
        if (obj && (obj->get_dev_id() == back_info.dev_id) ) {
            obj->set_dev_id(back_info.dev_id);
            obj->cali_finished    = back_info.cali_finished;
            obj->cache_flow_ratio = back_info.cache_flow_ratio;
            obj->selected_cali_preset = back_info.selected_presets;
            obj->flow_ratio_calibration_type = back_info.cache_flow_rate_calibration_type;
        }
    }
}

void CalibrationWizard::back_preset_info(MachineObject *obj, bool cali_finish, bool back_cali_flag)
{
    if (!obj)
        return;

    PrinterCaliInfo printer_cali_info;
    printer_cali_info.dev_id           = obj->get_dev_id();
    printer_cali_info.cali_finished    = cali_finish;
    printer_cali_info.cache_flow_ratio = obj->cache_flow_ratio;
    printer_cali_info.selected_presets = obj->selected_cali_preset;
    printer_cali_info.cache_flow_rate_calibration_type = obj->flow_ratio_calibration_type;
    wxGetApp().app_config->save_printer_cali_infos(printer_cali_info, back_cali_flag);
}

void CalibrationWizard::msw_rescale()
{
    for (int i = 0; i < m_page_steps.size(); i++) {
        if (m_page_steps[i]->page)
            m_page_steps[i]->page->msw_rescale();
    }
}

void CalibrationWizard::on_sys_color_changed()
{
    for (int i = 0; i < m_page_steps.size(); i++) {
        if (m_page_steps[i]->page)
            m_page_steps[i]->page->on_sys_color_changed();
    }
}

void CalibrationWizard::on_cali_go_home()
{
    // can go home? confirm to continue
    CalibrationMethod method;
    int cali_stage = 0;
    CalibMode obj_cali_mode = get_obj_calibration_mode(curr_obj, method, cali_stage);

    bool double_confirm = false;
    CaliPageType page_type = get_curr_step()->page->get_page_type();
    if (page_type == CaliPageType::CALI_PAGE_COARSE_SAVE ||
        page_type == CaliPageType::CALI_PAGE_FINE_SAVE ||
        page_type == CaliPageType::CALI_PAGE_COMMON_SAVE ||
        page_type == CaliPageType::CALI_PAGE_FLOW_SAVE ||
        page_type == CaliPageType::CALI_PAGE_PA_SAVE) {
        double_confirm = true;
    }
    if (obj_cali_mode == m_mode && curr_obj && (curr_obj->is_in_printing() || double_confirm)) {
        if (go_home_dialog == nullptr)
            go_home_dialog = new SecondaryCheckDialog(this, wxID_ANY, _L("Confirm"));

        go_home_dialog->Bind(EVT_SECONDARY_CHECK_CONFIRM, [this, method](wxCommandEvent &e) {
            if (curr_obj) {
                curr_obj->command_task_abort();
            } else {
                assert(false);
            }
            if (!m_page_steps.empty()) {
                back_preset_info(curr_obj, true);
                show_step(m_page_steps.front());
            }
        });

        go_home_dialog->update_text(_L("Are you sure to cancel the current calibration and return to the home page?"));
        go_home_dialog->on_show();
    } else {
        if (!m_page_steps.empty()) {
            back_preset_info(curr_obj, true, obj_cali_mode == m_mode);
            show_step(m_page_steps.front());
        }
    }
}

PressureAdvanceWizard::PressureAdvanceWizard(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : CalibrationWizard(parent, CalibMode::Calib_PA_Line, id, pos, size, style)
{
    create_pages();
}

void PressureAdvanceWizard::on_cali_job_finished(wxString evt_data)
{
    int       cali_stage    = 0;
    CalibMode obj_cali_mode = CalibUtils::get_calib_mode_by_name(evt_data.ToStdString(), cali_stage);

    if (obj_cali_mode == m_mode) {
        show_step(cali_step);
    }
    // change ui, hide
    static_cast<CalibrationPresetPage *>(preset_step->page)->on_cali_finished_job();
}

void PressureAdvanceWizard::create_pages()
{
    start_step  = new CalibrationWizardPageStep(new CalibrationPAStartPage(m_scrolledWindow));
    preset_step = new CalibrationWizardPageStep(new CalibrationPresetPage(m_scrolledWindow, m_mode, false));
    cali_step   = new CalibrationWizardPageStep(new CalibrationCaliPage(m_scrolledWindow, m_mode));
    save_step   = new CalibrationWizardPageStep(new CalibrationPASavePage(m_scrolledWindow));

    m_all_pages_sizer->Add(start_step->page, 1, wxEXPAND | wxALL, FromDIP(25));
    m_all_pages_sizer->Add(preset_step->page, 1, wxEXPAND | wxALL, FromDIP(25));
    m_all_pages_sizer->Add(cali_step->page, 1, wxEXPAND | wxALL, FromDIP(25));
    m_all_pages_sizer->Add(save_step->page, 1, wxEXPAND | wxALL, FromDIP(25));


    m_page_steps.push_back(start_step);
    m_page_steps.push_back(preset_step);
    m_page_steps.push_back(cali_step);
    m_page_steps.push_back(save_step);

    for (int i = 0; i < m_page_steps.size() -1; i++) {
        m_page_steps[i]->chain(m_page_steps[i+1]);
    }

    for (int i = 0; i < m_page_steps.size(); i++) {
        m_page_steps[i]->page->Hide();
        m_page_steps[i]->page->Bind(EVT_CALI_ACTION, &PressureAdvanceWizard::on_cali_action, this);
    }

    if (!m_page_steps.empty())
        show_step(m_page_steps.front());
}

void PressureAdvanceWizard::on_cali_action(wxCommandEvent& evt)
{
    CaliPageActionType action = static_cast<CaliPageActionType>(evt.GetInt());
    if (action == CaliPageActionType::CALI_ACTION_MANAGE_RESULT) {
        HistoryWindow history_dialog(this, m_calib_results_history, m_show_result_dialog);
        history_dialog.on_device_connected(curr_obj);
        history_dialog.ShowModal();
    }
    else if (action == CaliPageActionType::CALI_ACTION_MANUAL_CALI) {
        preset_step->page->set_cali_filament_mode(CalibrationFilamentMode::CALI_MODEL_SINGLE);
        set_cali_method(CalibrationMethod::CALI_METHOD_MANUAL);
        preset_step->page->on_device_connected(curr_obj);
        show_step(m_curr_step->next);
    }
    else if (action == CaliPageActionType::CALI_ACTION_AUTO_CALI) {
        if (curr_obj && curr_obj->is_support_new_auto_cali_method) {
            set_cali_method(CalibrationMethod::CALI_METHOD_NEW_AUTO);
        }
        else {
            set_cali_method(CalibrationMethod::CALI_METHOD_AUTO);
        }
        CalibrationFilamentMode fila_mode = get_cali_filament_mode(curr_obj, m_mode);
        preset_step->page->set_cali_filament_mode(fila_mode);
        preset_step->page->on_device_connected(curr_obj);
        show_step(m_curr_step->next);
    }
    else if (action == CaliPageActionType::CALI_ACTION_NEXT) {
        show_step(m_curr_step->next);
    }
    else if (action == CaliPageActionType::CALI_ACTION_CALI_NEXT) {
        show_step(m_curr_step->next);
    }
    else if (action == CaliPageActionType::CALI_ACTION_PREV) {
        show_step(m_curr_step->prev);
    }
    else if (action == CaliPageActionType::CALI_ACTION_CALI) {
        on_cali_start();
    }
    else if (action == CaliPageActionType::CALI_ACTION_GO_HOME) {
        on_cali_go_home();
    } else if (action == CaliPageActionType::CALI_ACTION_PA_SAVE) {
        on_cali_save();
    }
}

void PressureAdvanceWizard::update(MachineObject* obj)
{
    CalibrationWizard::update(obj);

    if (!obj)
        return;

    if (!m_show_result_dialog) {
        if (obj->cali_version != -1 && obj->cali_version != cali_version) {
            cali_version = obj->cali_version;
            PACalibExtruderInfo cali_info;
            cali_info.nozzle_diameter = obj->GetExtderSystem()->GetNozzleDiameter(0);
            cali_info.use_extruder_id        = false;
            cali_info.use_nozzle_volume_type = false;
            CalibUtils::emit_get_PA_calib_infos(cali_info);
        }
    }
}

void PressureAdvanceWizard::on_device_connected(MachineObject* obj)
{
    CalibrationWizard::on_device_connected(obj);

    CalibrationMethod method;
    int cali_stage = 0;
    CalibMode obj_cali_mode = get_obj_calibration_mode(obj, method, cali_stage);
    obj->manual_pa_cali_method = ManualPaCaliMethod(cali_stage);

    // show cali step when obj is in pa calibration
    if (obj) {
        CalibrationWizard::set_cali_method(method);

        if (m_curr_step != cali_step) {
            if (obj_cali_mode == m_mode) {
                if (!obj->cali_finished && (obj->is_in_printing() || obj->is_printing_finished())) {
                    CalibrationWizard::set_cali_method(method);
                    CalibrationCaliPage *cali_page = (static_cast<CalibrationCaliPage *>(cali_step->page));
                    cali_page->set_pa_cali_image(cali_stage);
                    show_step(cali_step);
                }
            }
        }
    }
}

static bool get_preset_info(const DynamicConfig& config, const BedType plate_type, int& nozzle_temp, int& bed_temp, float& max_volumetric_speed)
{
    const ConfigOptionInts* nozzle_temp_opt = config.option<ConfigOptionInts>("nozzle_temperature");
    const ConfigOptionInts* opt_bed_temp_ints = config.option<ConfigOptionInts>(get_bed_temp_key(plate_type));
    const ConfigOptionFloats* speed_opt = config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    if (nozzle_temp_opt && speed_opt && opt_bed_temp_ints) {
        nozzle_temp = nozzle_temp_opt->get_at(0);
        max_volumetric_speed = speed_opt->get_at(0);
        bed_temp = opt_bed_temp_ints->get_at(0);
        if (bed_temp >= 0 && nozzle_temp >= 0 && max_volumetric_speed >= 0) {
            return true;
        }
    }
    return false;
}

static bool get_flow_ratio(const DynamicConfig& config, float& flow_ratio)
{
    const ConfigOptionFloatsNullable *flow_ratio_opt = config.option<ConfigOptionFloatsNullable>("filament_flow_ratio");
    if (flow_ratio_opt) {
        flow_ratio = flow_ratio_opt->get_at(0);
        if (flow_ratio > 0)
            return true;
    }
    return false;
}

void PressureAdvanceWizard::on_cali_start()
{
    if (!curr_obj) {
        MessageDialog msg_dlg(nullptr, _L("No Printer Connected!"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    if (curr_obj->is_connecting() || !curr_obj->is_connected()) {
        MessageDialog msg_dlg(nullptr, _L("Printer is not connected yet."), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    //clean PA result
    curr_obj->reset_pa_cali_result();

    std::string setting_id;
    BedType plate_type = BedType::btDefault;

    // save preset info to machine object
    CalibrationPresetPage* preset_page = (static_cast<CalibrationPresetPage*>(preset_step->page));
    std::map<int, Preset*> selected_filaments = preset_page->get_selected_filaments();
    if (selected_filaments.empty()) {
        MessageDialog msg_dlg(nullptr, _L("Please select filament to calibrate."), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    float nozzle_dia = -1;
    preset_page->get_preset_info(nozzle_dia, plate_type);

    CalibrationWizard::cache_preset_info(curr_obj, nozzle_dia, plate_type);
    if (/*nozzle_dia < 0 || */ plate_type == BedType::btDefault) {
        BOOST_LOG_TRIVIAL(error) << "CaliPreset: get preset info, nozzle and plate type error";
        return;
    }

    //std::string error_message;
    wxString wx_err_string;
    if (m_cali_method == CalibrationMethod::CALI_METHOD_AUTO && curr_obj->get_printer_series() == PrinterSeries::SERIES_X1) {
        X1CCalibInfos calib_infos;
        for (auto &item : selected_filaments) {
            int   nozzle_temp          = -1;
            int   bed_temp             = -1;
            float max_volumetric_speed = -1;

            if (!get_preset_info(item.second->config, plate_type, nozzle_temp, bed_temp, max_volumetric_speed)) {
                BOOST_LOG_TRIVIAL(error) << "CaliPreset: get preset info error";
                continue;
            }

            X1CCalibInfos::X1CCalibInfo calib_info;
            get_tray_ams_and_slot_id(curr_obj, item.first, calib_info.ams_id, calib_info.slot_id, calib_info.tray_id);
            calib_info.extruder_id          = preset_page->get_extruder_id(calib_info.ams_id);
            calib_info.extruder_type        = preset_page->get_extruder_type(calib_info.extruder_id);
            calib_info.nozzle_volume_type   = preset_page->get_nozzle_volume_type(calib_info.extruder_id);
            calib_info.nozzle_diameter      = preset_page->get_nozzle_diameter(calib_info.extruder_id);
            calib_info.filament_id          = item.second->filament_id;
            calib_info.setting_id           = item.second->setting_id;
            calib_info.bed_temp             = bed_temp;
            calib_info.nozzle_temp          = nozzle_temp;
            calib_info.max_volumetric_speed = max_volumetric_speed;
            calib_infos.calib_datas.push_back(calib_info);
        }
        calib_infos.cali_mode = CalibMode::Calib_PA_Line;
        CalibUtils::calib_PA(calib_infos, 0, wx_err_string); // mode = 0 for auto

        if (!wx_err_string.empty()) {
            MessageDialog msg_dlg(nullptr, wx_err_string, wxEmptyString, wxICON_WARNING | wxOK);
            msg_dlg.ShowModal();
            return;
        }

        show_step(m_curr_step->next);
    } else if (m_cali_method == CalibrationMethod::CALI_METHOD_MANUAL) {
        if (selected_filaments.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "CaliPreset: selected filaments is empty";
            return;
        }
        else {
            int   nozzle_temp          = -1;
            int   bed_temp             = -1;
            float max_volumetric_speed = -1;
            if (!get_preset_info(selected_filaments.begin()->second->config, plate_type, nozzle_temp, bed_temp, max_volumetric_speed)) {
                BOOST_LOG_TRIVIAL(error) << "CaliPreset: get preset info error";
                return;
            }

            int selected_tray_id = 0;
            CalibInfo calib_info;
            calib_info.dev_id            = curr_obj->get_dev_id();
            get_tray_ams_and_slot_id(curr_obj, selected_filaments.begin()->first, calib_info.ams_id, calib_info.slot_id, selected_tray_id);
            calib_info.extruder_id       = preset_page->get_extruder_id(calib_info.ams_id);
            calib_info.extruder_type     = preset_page->get_extruder_type(calib_info.extruder_id);
            calib_info.nozzle_volume_type = preset_page->get_nozzle_volume_type(calib_info.extruder_id);
            calib_info.select_ams         = std::to_string(selected_tray_id);
            Preset *preset               = selected_filaments.begin()->second;
            Preset * temp_filament_preset = new Preset(preset->type, preset->name + "_temp");
            temp_filament_preset->config = preset->config;
            if (preset->type == Preset::TYPE_FILAMENT)
                temp_filament_preset->filament_id = preset->filament_id;

            calib_info.bed_type      = plate_type;
            calib_info.process_bar   = preset_page->get_sending_progress_bar();
            calib_info.printer_prest = preset_page->get_printer_preset(curr_obj, preset_page->get_nozzle_diameter(calib_info.extruder_id));
            calib_info.print_prest   = preset_page->get_print_preset();
            calib_info.filament_prest = temp_filament_preset;

            std::map<int, DynamicPrintConfig> filament_list = preset_page->get_filament_ams_list();
            calib_info.filament_color = filament_list[selected_filaments.begin()->first].opt_string("filament_colour", 0u);

            wxArrayString values = preset_page->get_custom_range_values();
            if (values.size() != 3) {
                MessageDialog msg_dlg(nullptr, _L("The input value size must be 3."), wxEmptyString, wxICON_WARNING | wxOK);
                msg_dlg.ShowModal();
                return;
            } else {
                values[0].ToDouble(&calib_info.params.start);
                values[1].ToDouble(&calib_info.params.end);
                values[2].ToDouble(&calib_info.params.step);
            }
            calib_info.params.mode = preset_page->get_pa_cali_method();
            calib_info.params.print_numbers = true;

            if (!is_pa_params_valid(calib_info.params))
                return;

            ManualPaCaliMethod   pa_cali_method = ManualPaCaliMethod::PA_LINE;
            CalibrationCaliPage *cali_page = (static_cast<CalibrationCaliPage *>(cali_step->page));
            if (calib_info.params.mode == CalibMode::Calib_PA_Line)
                pa_cali_method = ManualPaCaliMethod::PA_LINE;
            else if (calib_info.params.mode == CalibMode::Calib_PA_Pattern)
                pa_cali_method = ManualPaCaliMethod::PA_PATTERN;

            cali_page->set_pa_cali_image(int(pa_cali_method));
            curr_obj->manual_pa_cali_method = pa_cali_method;

            if (curr_obj->get_printer_series() != PrinterSeries::SERIES_X1 && curr_obj->pa_calib_tab.size() >= MAX_PA_HISTORY_RESULTS_NUMS) {
                MessageDialog msg_dlg(nullptr, wxString::Format(_L("This machine type can only hold 16 history results per nozzle. "
                    "You can delete the existing historical results and then start calibration. "
                    "Or you can continue the calibration, but you cannot create new calibration historical results.\n"
                    "Do you still want to continue the calibration?"), MAX_PA_HISTORY_RESULTS_NUMS), wxEmptyString, wxICON_WARNING | wxYES | wxCANCEL);
                if (msg_dlg.ShowModal() != wxID_YES) {
                    return;
                }
            }

            if (!CalibUtils::calib_generic_PA(calib_info, wx_err_string)) {
                if (!wx_err_string.empty()) {
                    MessageDialog msg_dlg(nullptr, wx_err_string, wxEmptyString, wxICON_WARNING | wxOK);
                    msg_dlg.ShowModal();
                }
                return;
            }

            preset_page->on_cali_start_job();
        }
    } else if (m_cali_method == CalibrationMethod::CALI_METHOD_NEW_AUTO) {
        if (selected_filaments.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "CaliPreset: selected filaments is empty";
            return;
        }

        std::vector<CalibInfo> calib_infos;
        for (auto &item : selected_filaments) {
            int   nozzle_temp          = -1;
            int   bed_temp             = -1;
            float max_volumetric_speed = -1;

            if (!get_preset_info(item.second->config, plate_type, nozzle_temp, bed_temp, max_volumetric_speed)) {
                BOOST_LOG_TRIVIAL(error) << "CaliPreset: get preset info error";
                continue;
            }

            int       selected_tray_id = 0;
            CalibInfo calib_info;
            calib_info.dev_id = curr_obj->get_dev_id();
            get_tray_ams_and_slot_id(curr_obj, item.first, calib_info.ams_id, calib_info.slot_id, selected_tray_id);
            calib_info.index              = preset_page->get_index_by_tray_id(item.first);
            calib_info.extruder_id        = preset_page->get_extruder_id(calib_info.ams_id);
            calib_info.nozzle_diameter    = preset_page->get_nozzle_diameter(calib_info.extruder_id);
            calib_info.extruder_type      = preset_page->get_extruder_type(calib_info.extruder_id);
            calib_info.nozzle_volume_type = preset_page->get_nozzle_volume_type(calib_info.extruder_id);
            calib_info.select_ams         = std::to_string(selected_tray_id);
            Preset *preset                = item.second;
            Preset *temp_filament_preset  = new Preset(preset->type, preset->name + "_temp");
            temp_filament_preset->config  = preset->config;

            calib_info.bed_type       = plate_type;
            calib_info.process_bar    = preset_page->get_sending_progress_bar();
            calib_info.printer_prest  = preset_page->get_printer_preset(curr_obj, preset_page->get_nozzle_diameter(calib_info.extruder_id));
            calib_info.print_prest    = preset_page->get_print_preset();
            calib_info.filament_prest = temp_filament_preset;
            std::map<int, DynamicPrintConfig> filament_list = preset_page->get_filament_ams_list();
            calib_info.filament_color = filament_list[item.first].opt_string("filament_colour", 0u);
            calib_info.params.mode    = CalibMode::Calib_Auto_PA_Line;
            calib_infos.emplace_back(calib_info);
        }

        if (!CalibUtils::calib_generic_auto_pa_cali(calib_infos, wx_err_string)) {
            if (!wx_err_string.empty()) {
                MessageDialog msg_dlg(nullptr, wx_err_string, wxEmptyString, wxICON_WARNING | wxOK);
                msg_dlg.ShowModal();
            }
            return;
        }

        preset_page->on_cali_start_job();
    } else {
        assert(false);
        BOOST_LOG_TRIVIAL(error) << "CaliPreset: unsupported printer type or cali method";
        return;
    }

    CalibrationCaliPage* cali_page = (static_cast<CalibrationCaliPage*>(cali_step->page));
    cali_page->clear_last_job_status();
}

bool PressureAdvanceWizard::can_save_cali_result(const std::vector<PACalibResult> &new_pa_cali_results)
{
    if (!curr_obj)
        return false;

    std::vector<PACalibResult> to_save_result;
    for (auto &result : new_pa_cali_results) {
        auto iter = std::find_if(to_save_result.begin(), to_save_result.end(), [this, &result](const PACalibResult &item) {
            bool has_same_name = (item.name == result.name && item.filament_id == result.filament_id);
            if (curr_obj && curr_obj->is_multi_extruders()) {
                has_same_name &= (item.extruder_id == result.extruder_id && item.nozzle_volume_type == result.nozzle_volume_type);
            }
            return has_same_name;
        });
        if (iter != to_save_result.end()) {
            MessageDialog msg_dlg(nullptr, wxString::Format(_L("Only one of the results with the same name: %s will be saved. Are you sure you want to override the other results?"), iter->name), wxEmptyString,
                                  wxICON_WARNING | wxYES_NO);
            if (msg_dlg.ShowModal() != wxID_YES) {
                return false;
            } else {
                break;
            }
        }
        to_save_result.push_back(result);
    }

    std::string same_pa_names;
    for (auto new_pa_cali_result : new_pa_cali_results) {
        auto iter = std::find_if(curr_obj->pa_calib_tab.begin(), curr_obj->pa_calib_tab.end(), [this, &new_pa_cali_result](const PACalibResult &item) {
            bool is_same_name = (item.name == new_pa_cali_result.name && item.filament_id == new_pa_cali_result.filament_id &&
                                 item.nozzle_diameter == new_pa_cali_result.nozzle_diameter);
            if (curr_obj && curr_obj->is_multi_extruders()) {
                is_same_name &= (item.extruder_id == new_pa_cali_result.extruder_id && item.nozzle_volume_type == new_pa_cali_result.nozzle_volume_type);
            }
            return is_same_name;
        });

        if (iter != curr_obj->pa_calib_tab.end()) {
            same_pa_names += new_pa_cali_result.name;
            same_pa_names += ", ";
        }
    }

    if (!same_pa_names.empty()) {
        same_pa_names.erase(same_pa_names.size() - 2);
        wxString duplicate_name_info = wxString::Format(_L("There is already a historical calibration result with the same name: %s. Only one of the results with the same name "
                                                  "is saved. Are you sure you want to override the historical result?"), same_pa_names);

        if (curr_obj->is_multi_extruders())
            duplicate_name_info = wxString::Format(_L("Within the same extruder, the name(%s) must be unique when the filament type, nozzle diameter, and nozzle flow are the same.\n"
                                                      "Are you sure you want to override the historical result?"), same_pa_names);

        MessageDialog msg_dlg(nullptr, duplicate_name_info, wxEmptyString, wxICON_WARNING | wxYES_NO);
        if (msg_dlg.ShowModal() != wxID_YES)
            return false;
    }

    if (curr_obj->get_printer_series() != PrinterSeries::SERIES_X1 && curr_obj->pa_calib_tab.size() >= MAX_PA_HISTORY_RESULTS_NUMS) {
        MessageDialog msg_dlg(nullptr, wxString::Format(_L("This machine type can only hold %d history results per nozzle. This result will not be saved."), MAX_PA_HISTORY_RESULTS_NUMS),
                              wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return false;
    }
    return true;
}

void PressureAdvanceWizard::on_cali_save()
{
    if (curr_obj) {
        if (curr_obj->is_connecting() || !curr_obj->is_connected())
        {
            MessageDialog msg_dlg(nullptr, _L("Connecting to printer..."), wxEmptyString, wxOK);
            msg_dlg.ShowModal();
            return;
        }

        if (curr_obj->get_printer_series() == PrinterSeries::SERIES_X1) {
            if (m_cali_method == CalibrationMethod::CALI_METHOD_AUTO || m_cali_method == CalibrationMethod::CALI_METHOD_NEW_AUTO) {
                std::vector<PACalibResult> new_pa_cali_results;
                auto save_page = static_cast<CalibrationPASavePage*>(save_step->page);
                if (!save_page->get_auto_result(new_pa_cali_results)) {
                    return;
                }
                if (save_page->is_all_failed()) {
                    MessageDialog msg_dlg(nullptr, _L("The failed test result has been dropped."), wxEmptyString, wxOK);
                    msg_dlg.ShowModal();
                    back_preset_info(curr_obj, true);
                    show_step(start_step);
                    return;
                }
                if (!can_save_cali_result(new_pa_cali_results))
                    return;
                CalibUtils::set_PA_calib_result(new_pa_cali_results, true);
            }
            else if (m_cali_method == CalibrationMethod::CALI_METHOD_MANUAL) {
                PACalibResult new_pa_cali_result;
                auto save_page = static_cast<CalibrationPASavePage*>(save_step->page);
                if (!save_page->get_manual_result(new_pa_cali_result)) {
                    return;
                }
                if (!can_save_cali_result({new_pa_cali_result}))
                    return;
                CalibUtils::set_PA_calib_result({ new_pa_cali_result }, false);
            }

            MessageDialog msg_dlg(nullptr, _L("Flow Dynamics Calibration result has been saved to the printer."), wxEmptyString, wxOK);
            msg_dlg.ShowModal();
        }
        else if (curr_obj->get_printer_series() == PrinterSeries::SERIES_P1P) {
            if (curr_obj->cali_version >= 0) {
                PACalibResult new_pa_cali_result;
                auto          save_page = static_cast<CalibrationPASavePage *>(save_step->page);
                if (!save_page->get_manual_result(new_pa_cali_result)) {
                    return;
                }

                if (!can_save_cali_result({new_pa_cali_result}))
                    return;

                CalibUtils::set_PA_calib_result({new_pa_cali_result}, false);
            } else {
                auto  save_page   = static_cast<CalibrationPASavePage *>(save_step->page);
                float new_k_value = 0.0f;
                float new_n_value = 0.0f;
                if (!save_page->get_p1p_result(&new_k_value, &new_n_value)) {
                    return;
                }

                float                  nozzle_dia  = 0.4;
                BedType                plate_type  = BedType::btDefault;
                CalibrationPresetPage *preset_page = (static_cast<CalibrationPresetPage *>(preset_step->page));
                preset_page->get_preset_info(nozzle_dia, plate_type);
                std::map<int, Preset *> selected_filaments = get_cached_selected_filament(curr_obj);
                if (selected_filaments.empty()) {
                    BOOST_LOG_TRIVIAL(error) << "CaliPreset: get selected filaments error";
                    return;
                }
                int         tray_id    = selected_filaments.begin()->first;
                std::string setting_id = selected_filaments.begin()->second->setting_id;

                int   nozzle_temp          = -1;
                int   bed_temp             = -1;
                float max_volumetric_speed = -1;
                if (!get_preset_info(selected_filaments.begin()->second->config, plate_type, nozzle_temp, bed_temp, max_volumetric_speed)) {
                    BOOST_LOG_TRIVIAL(error) << "CaliPreset: get preset info error";
                    return;
                }

                curr_obj->command_extrusion_cali_set(tray_id, setting_id, "", new_k_value, new_n_value, bed_temp, nozzle_temp, max_volumetric_speed);

            }

            MessageDialog msg_dlg(nullptr, _L("Flow Dynamics Calibration result has been saved to the printer."), wxEmptyString, wxOK);
            msg_dlg.ShowModal();
        }
        else {
            assert(false);
        }
    }
    back_preset_info(curr_obj, true);
    show_step(start_step);
}

FlowRateWizard::FlowRateWizard(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : CalibrationWizard(parent, CalibMode::Calib_Flow_Rate, id, pos, size, style)
{
    create_pages();
}

void FlowRateWizard::create_pages()
{
    start_step = new CalibrationWizardPageStep(new CalibrationFlowRateStartPage(m_scrolledWindow));
    preset_step = new CalibrationWizardPageStep(new CalibrationPresetPage(m_scrolledWindow, m_mode, false));

    // manual
    cali_coarse_step = new CalibrationWizardPageStep(new CalibrationCaliPage(m_scrolledWindow, m_mode, CaliPageType::CALI_PAGE_CALI));
    coarse_save_step = new CalibrationWizardPageStep(new CalibrationFlowCoarseSavePage(m_scrolledWindow));
    cali_fine_step = new CalibrationWizardPageStep(new CalibrationCaliPage(m_scrolledWindow, m_mode, CaliPageType::CALI_PAGE_FINE_CALI));
    fine_save_step = new CalibrationWizardPageStep(new CalibrationFlowFineSavePage(m_scrolledWindow));

    // auto
    cali_step = new CalibrationWizardPageStep(new CalibrationCaliPage(m_scrolledWindow, m_mode));
    save_step = new CalibrationWizardPageStep(new CalibrationFlowX1SavePage(m_scrolledWindow));

    m_all_pages_sizer->Add(start_step->page, 1, wxEXPAND | wxALL, FromDIP(25));
    m_all_pages_sizer->Add(preset_step->page, 1, wxEXPAND | wxALL, FromDIP(25));
    m_all_pages_sizer->Add(cali_coarse_step->page, 1, wxEXPAND | wxALL, FromDIP(25));
    m_all_pages_sizer->Add(coarse_save_step->page, 1, wxEXPAND | wxALL, FromDIP(25));
    m_all_pages_sizer->Add(cali_fine_step->page, 1, wxEXPAND | wxALL, FromDIP(25));
    m_all_pages_sizer->Add(fine_save_step->page, 1, wxEXPAND | wxALL, FromDIP(25));

    m_all_pages_sizer->Add(cali_step->page, 1, wxEXPAND | wxALL, FromDIP(25));
    m_all_pages_sizer->Add(save_step->page, 1, wxEXPAND | wxALL, FromDIP(25));

    m_page_steps.push_back(start_step);
    m_page_steps.push_back(preset_step);
    m_page_steps.push_back(cali_coarse_step);
    m_page_steps.push_back(coarse_save_step);
    m_page_steps.push_back(cali_fine_step);
    m_page_steps.push_back(fine_save_step);

    //m_page_steps.push_back(cali_step);
    //m_page_steps.push_back(save_step);

    for (int i = 0; i < m_page_steps.size() - 1; i++) {
        m_page_steps[i]->chain(m_page_steps[i + 1]);
    }

    // hide all pages
    cali_step->page->Hide();
    save_step->page->Hide();
    for (int i = 0; i < m_page_steps.size(); i++) {
        m_page_steps[i]->page->Hide();
        m_page_steps[i]->page->Bind(EVT_CALI_ACTION, &FlowRateWizard::on_cali_action, this);
    }


    cali_step->page->Bind(EVT_CALI_ACTION, &FlowRateWizard::on_cali_action, this);
    save_step->page->Bind(EVT_CALI_ACTION, &FlowRateWizard::on_cali_action, this);

    if (!m_page_steps.empty())
        show_step(m_page_steps.front());

    set_cali_method(CalibrationMethod::CALI_METHOD_MANUAL);
}

void FlowRateWizard::on_cali_action(wxCommandEvent& evt)
{
    CaliPageActionType action = static_cast<CaliPageActionType>(evt.GetInt());
    if (action == CaliPageActionType::CALI_ACTION_MANAGE_RESULT) {
        ;
    }
    else if (action == CaliPageActionType::CALI_ACTION_MANUAL_CALI) {
        preset_step->page->set_cali_filament_mode(CalibrationFilamentMode::CALI_MODEL_SINGLE);
        this->set_cali_method(CalibrationMethod::CALI_METHOD_MANUAL);
        preset_step->page->on_device_connected(curr_obj);
        show_step(m_curr_step->next);
    }
    else if (action == CaliPageActionType::CALI_ACTION_AUTO_CALI) {
        CalibrationFilamentMode fila_mode = get_cali_filament_mode(curr_obj, m_mode);
        preset_step->page->set_cali_filament_mode(fila_mode);
        this->set_cali_method(CalibrationMethod::CALI_METHOD_AUTO);
        preset_step->page->on_device_connected(curr_obj);
        show_step(m_curr_step->next);
    }
    else if (action == CaliPageActionType::CALI_ACTION_NEXT) {
        show_step(m_curr_step->next);
    }
    else if (action == CaliPageActionType::CALI_ACTION_CALI_NEXT) {
        show_step(m_curr_step->next);
    }
    else if (action == CaliPageActionType::CALI_ACTION_PREV) {
        show_step(m_curr_step->prev);
    }
    else if (action == CaliPageActionType::CALI_ACTION_CALI) {
        if (m_cali_method == CalibrationMethod::CALI_METHOD_AUTO) {
            on_cali_start();
        }
        else if (m_cali_method == CalibrationMethod::CALI_METHOD_MANUAL) {
            CaliPresetStage stage = CaliPresetStage::CALI_MANULA_STAGE_NONE;
            float cali_value = 0.0f;
            static_cast<CalibrationPresetPage*>(preset_step->page)->get_cali_stage(stage, cali_value);
            on_cali_start(stage, cali_value, FlowRatioCaliSource::FROM_PRESET_PAGE);
            if (stage == CaliPresetStage::CALI_MANUAL_STAGE_2) {
                // set next step page
                m_curr_step->chain(cali_fine_step);
            }
            // automatically jump to next step when print job is sending finished.
        }
        else {
            on_cali_start();
        }
    }
    else if (action == CaliPageActionType::CALI_ACTION_FLOW_CALI_STAGE_2) {
        if (m_cali_method == CalibrationMethod::CALI_METHOD_MANUAL) {
            float new_flow_ratio = 0.0f;
            wxString temp_name = ""; // unused
            auto coarse_save_page = static_cast<CalibrationFlowCoarseSavePage*>(m_curr_step->page);
            if (!coarse_save_page->get_result(&new_flow_ratio, &temp_name)) {
                return;
            }
            on_cali_start(CaliPresetStage::CALI_MANUAL_STAGE_2, new_flow_ratio, FlowRatioCaliSource::FROM_COARSE_PAGE);
            // automatically jump to next step when print job is sending finished.
        }
    }
    else if (action == CaliPageActionType::CALI_ACTION_FLOW_SAVE) {
        on_cali_save();
    }
    else if (action == CaliPageActionType::CALI_ACTION_FLOW_COARSE_SAVE) {
        auto coarse_save_page = static_cast<CalibrationFlowCoarseSavePage*>(coarse_save_step->page);
        if (coarse_save_page->is_skip_fine_calibration()) {
            on_cali_save();
        }
    }
    else if (action == CaliPageActionType::CALI_ACTION_FLOW_FINE_SAVE) {
        on_cali_save();
    }
    else if (action == CaliPageActionType::CALI_ACTION_GO_HOME) {
        on_cali_go_home();
    }
}

void FlowRateWizard::on_cali_start(CaliPresetStage stage, float cali_value, FlowRatioCaliSource from_page)
{
    if (!curr_obj) return;

    //clean flow rate result
    curr_obj->reset_flow_rate_cali_result();

    float nozzle_dia = 0.4;
    std::string setting_id;
    BedType plate_type = BedType::btDefault;

    CalibrationPresetPage* preset_page = (static_cast<CalibrationPresetPage*>(preset_step->page));

    preset_page->get_preset_info(nozzle_dia, plate_type);

    std::map<int, Preset*> selected_filaments = preset_page->get_selected_filaments();
    if (from_page == FlowRatioCaliSource::FROM_PRESET_PAGE) {
        if (selected_filaments.empty()) {
            MessageDialog msg_dlg(nullptr, _L("Please select filament to calibrate."), wxEmptyString, wxICON_WARNING | wxOK);
            msg_dlg.ShowModal();
            return;
        }
        CalibrationWizard::cache_preset_info(curr_obj, nozzle_dia, plate_type);
    }
    else if (from_page == FlowRatioCaliSource::FROM_COARSE_PAGE) {
        selected_filaments = get_cached_selected_filament(curr_obj);
        if (selected_filaments.empty()) {
            MessageDialog msg_dlg(nullptr, _L("Please select filament to calibrate."), wxEmptyString, wxICON_WARNING | wxOK);
            msg_dlg.ShowModal();
            return;
        }
        cache_coarse_info(curr_obj);
    }

    if (m_cali_method == CalibrationMethod::CALI_METHOD_AUTO) {
        X1CCalibInfos calib_infos;
        for (auto& item : selected_filaments) {
            int nozzle_temp = -1;
            int bed_temp = -1;
            float max_volumetric_speed = -1;

            if (!get_preset_info(item.second->config, plate_type, nozzle_temp, bed_temp, max_volumetric_speed)) {
                BOOST_LOG_TRIVIAL(error) << "CaliPreset: get preset info error";
            }

            X1CCalibInfos::X1CCalibInfo calib_info;
            calib_info.tray_id          = item.first;
            get_tray_ams_and_slot_id(curr_obj, item.first, calib_info.ams_id, calib_info.slot_id, calib_info.tray_id);
            calib_info.extruder_id      = preset_page->get_extruder_id(calib_info.ams_id);
            calib_info.extruder_type    = preset_page->get_extruder_type(calib_info.extruder_id);
            calib_info.nozzle_volume_type = preset_page->get_nozzle_volume_type(calib_info.extruder_id);
            calib_info.nozzle_diameter  = preset_page->get_nozzle_diameter(calib_info.extruder_id);
            calib_info.filament_id      = item.second->filament_id;
            calib_info.setting_id       = item.second->setting_id;
            calib_info.bed_temp         = bed_temp;
            calib_info.nozzle_temp      = nozzle_temp;
            calib_info.max_volumetric_speed = max_volumetric_speed;
            float flow_ratio = 0.98;
            if (get_flow_ratio(item.second->config, flow_ratio))
                calib_info.flow_rate = flow_ratio;
            calib_infos.calib_datas.push_back(calib_info);
        }
        calib_infos.cali_mode = CalibMode::Calib_Flow_Rate;

        wxString wx_err_string;
        CalibUtils::calib_flowrate_X1C(calib_infos, wx_err_string);
        if (!wx_err_string.empty()) {
            MessageDialog msg_dlg(nullptr, wx_err_string, wxEmptyString, wxICON_WARNING | wxOK);
            msg_dlg.ShowModal();
            return;
        }
        show_step(m_curr_step->next);

        CalibrationCaliPage *cali_page = (static_cast<CalibrationCaliPage *>(cali_step->page));
        cali_page->clear_last_job_status();
    }
    else if (m_cali_method == CalibrationMethod::CALI_METHOD_MANUAL) {
        CalibrationFlowCoarseSavePage* coarse_page = (static_cast<CalibrationFlowCoarseSavePage*>(coarse_save_step->page));
        CalibInfo calib_info;
        calib_info.dev_id            = curr_obj->get_dev_id();
        Preset* temp_filament_preset = nullptr;
        int cali_stage = -1;
        wxString wx_err_string;

        // Recover to coarse and start fine print, should recover the selected_filaments
        CalibrationMethod temp_method;
        int temp_cali_tage = 0;
        CalibMode obj_cali_mode = get_obj_calibration_mode(curr_obj, temp_method, temp_cali_tage);
        if (selected_filaments.empty() && stage == CaliPresetStage::CALI_MANUAL_STAGE_2 && obj_cali_mode == CalibMode::Calib_Flow_Rate) {
            if (!curr_obj->selected_cali_preset.empty()) {
                int selected_tray_id = curr_obj->selected_cali_preset.front().tray_id;
                PresetCollection *filament_presets = &wxGetApp().preset_bundle->filaments;
                Preset* preset = filament_presets->find_preset(curr_obj->selected_cali_preset.front().name);
                plate_type = curr_obj->selected_cali_preset.front().bed_type;
                if (preset) {
                    selected_filaments.insert(std::make_pair(selected_tray_id, preset));
                }
            }
        }

        if (!selected_filaments.empty()) {
            int selected_tray_id  = 0;
            get_tray_ams_and_slot_id(curr_obj, selected_filaments.begin()->first, calib_info.ams_id, calib_info.slot_id, selected_tray_id);
            calib_info.select_ams         = std::to_string(selected_tray_id);
            calib_info.extruder_id = preset_page->get_extruder_id(calib_info.ams_id);
            calib_info.extruder_type      = preset_page->get_extruder_type(calib_info.extruder_id);
            calib_info.nozzle_volume_type = preset_page->get_nozzle_volume_type(calib_info.extruder_id);
            Preset* preset = selected_filaments.begin()->second;
            temp_filament_preset = new Preset(preset->type, preset->name + "_temp");
            temp_filament_preset->config = preset->config;
            if (preset->type == Preset::TYPE_FILAMENT)
                temp_filament_preset->filament_id = preset->filament_id;

            calib_info.bed_type = plate_type;
            calib_info.printer_prest = preset_page->get_printer_preset(curr_obj, preset_page->get_nozzle_diameter(calib_info.extruder_id));
            calib_info.print_prest = preset_page->get_print_preset();
            calib_info.params.mode = CalibMode::Calib_Flow_Rate;

            if (stage == CaliPresetStage::CALI_MANUAL_STAGE_1) {
                cali_stage = 1;
                calib_info.process_bar = preset_page->get_sending_progress_bar();
            }
            else if (stage == CaliPresetStage::CALI_MANUAL_STAGE_2) {
                cali_stage = 2;
                auto flow_ratio_values = temp_filament_preset->config.option<ConfigOptionFloatsNullable>("filament_flow_ratio")->values;
                std::map<std::string, ConfigIndexValue> key_value_map = generate_index_key_value(curr_obj, "filament_flow_ratio", cali_value);
                if (!key_value_map.empty()) {
                    flow_ratio_values[key_value_map.begin()->second.index] = key_value_map.begin()->second.value;
                }

                temp_filament_preset->config.set_key_value("filament_flow_ratio", new ConfigOptionFloatsNullable{flow_ratio_values});
                if (from_page == FlowRatioCaliSource::FROM_PRESET_PAGE) {
                    calib_info.process_bar = preset_page->get_sending_progress_bar();
                }
                else if (from_page == FlowRatioCaliSource::FROM_COARSE_PAGE) {
                    calib_info.process_bar = coarse_page->get_sending_progress_bar();
                }
            }
            calib_info.filament_prest = temp_filament_preset;

            std::map<int, DynamicPrintConfig> filament_list = preset_page->get_filament_ams_list();
            calib_info.filament_color = filament_list[selected_filaments.begin()->first].opt_string("filament_colour", 0u);

            if (cali_stage > 0) {
                if (!CalibUtils::calib_flowrate(cali_stage, calib_info, wx_err_string)) {
                    if (!wx_err_string.empty()) {
                        MessageDialog msg_dlg(nullptr, wx_err_string, wxEmptyString, wxICON_WARNING | wxOK);
                        msg_dlg.ShowModal();
                    }
                    return;
                }
            }
            else {
                wx_err_string = _L("Internal Error") + wxString(": Invalid calibration stage");
            }
        } else {
            wx_err_string = _L("Please select at least one filament for calibration");
        }

        if (!wx_err_string.empty()) {
            MessageDialog msg_dlg(nullptr, wx_err_string, wxEmptyString, wxICON_WARNING | wxOK);
            msg_dlg.ShowModal();
            return;
        }
        if (temp_filament_preset)
            delete temp_filament_preset;

        if (cali_stage == 1) {
            CalibrationCaliPage *cali_coarse_page = (static_cast<CalibrationCaliPage *>(cali_coarse_step->page));
            cali_coarse_page->clear_last_job_status();
            preset_page->on_cali_start_job();
        }
        else if (cali_stage == 2) {
            CalibrationCaliPage *cali_fine_page = (static_cast<CalibrationCaliPage *>(cali_fine_step->page));
            cali_fine_page->clear_last_job_status();
            if (from_page == FlowRatioCaliSource::FROM_PRESET_PAGE) {
                preset_page->on_cali_start_job();
            }
            else if (from_page == FlowRatioCaliSource::FROM_COARSE_PAGE) {
                coarse_page->on_cali_start_job();
            }
        }
    } else {
        assert(false);
    }
}

void FlowRateWizard::on_cali_save()
{
    if (curr_obj) {
        if (m_cali_method == CalibrationMethod::CALI_METHOD_AUTO) {
            std::vector<std::pair<wxString, float>> new_results;
            auto save_page = static_cast<CalibrationFlowX1SavePage*>(save_step->page);
            if (!save_page->get_result(new_results)) {
                return;
            }
            if (save_page->is_all_failed()) {
                MessageDialog msg_dlg(nullptr, _L("The failed test result has been dropped."), wxEmptyString, wxOK);
                msg_dlg.ShowModal();
                back_preset_info(curr_obj, true);
                show_step(start_step);
                return;
            }

            std::string old_preset_name;
            CalibrationPresetPage* preset_page = (static_cast<CalibrationPresetPage*>(preset_step->page));
            std::map<int, Preset*> selected_filaments = get_cached_selected_filament(curr_obj);
            if (!selected_filaments.empty()) {
                old_preset_name = selected_filaments.begin()->second->name;
            }
            for (int i = 0; i < new_results.size(); i++) {
                std::map<std::string, ConfigOption*> key_value_map;
                key_value_map.insert(std::make_pair("filament_flow_ratio", new ConfigOptionFloatsNullable{ new_results[i].second }));
                wxString message;
                if (!save_preset(old_preset_name, into_u8(new_results[i].first), key_value_map, message)) {
                    MessageDialog error_msg_dlg(nullptr, message, wxEmptyString, wxICON_WARNING | wxOK);
                    error_msg_dlg.ShowModal();
                    return;
                }
            }

            MessageDialog msg_dlg(nullptr, _L("Flow rate calibration result has been saved to preset."), wxEmptyString, wxOK);
            msg_dlg.ShowModal();
        }
        else if (m_cali_method == CalibrationMethod::CALI_METHOD_MANUAL) {
            float new_flow_ratio = 0.0f;
            wxString new_preset_name = "";
            if(m_curr_step->page->get_page_type() == CaliPageType::CALI_PAGE_COARSE_SAVE)
            {
                auto coarse_save_page = static_cast<CalibrationFlowCoarseSavePage*>(m_curr_step->page);
                if (!coarse_save_page->get_result(&new_flow_ratio, &new_preset_name)) {
                    BOOST_LOG_TRIVIAL(info) << "flow_rate_cali: get coarse result failed";
                    return;
                }
            }
            else if (m_curr_step->page->get_page_type() == CaliPageType::CALI_PAGE_FINE_SAVE)
            {
                auto fine_save_page = static_cast<CalibrationFlowFineSavePage*>(m_curr_step->page);
                if (!fine_save_page->get_result(&new_flow_ratio, &new_preset_name)) {
                    BOOST_LOG_TRIVIAL(info) << "flow_rate_cali: get fine result failed";
                    return;
                }
            }
            else {
                BOOST_LOG_TRIVIAL(info) << "flow_rate_cali: get result failed, not get result";
                return;
            }

            if (!CalibUtils::validate_input_name(new_preset_name))
                return;

            std::string old_preset_name;
            CalibrationPresetPage* preset_page = (static_cast<CalibrationPresetPage*>(preset_step->page));
            std::map<int, TrayInfo> selected_filaments = get_cached_selected_filament_for_multi_extruder(curr_obj);
            std::map<std::string, ConfigIndexValue> key_value_map = generate_index_key_value(curr_obj, "filament_flow_ratio", new_flow_ratio);

            if (!selected_filaments.empty()) {
                old_preset_name = selected_filaments.begin()->second.preset->name;
            }

            wxString message;
            if (!save_preset_with_index(old_preset_name, into_u8(new_preset_name), key_value_map, message)) {
                MessageDialog error_msg_dlg(nullptr, message, wxEmptyString, wxICON_WARNING | wxOK);
                error_msg_dlg.ShowModal();
                return;
            }

            MessageDialog msg_dlg(nullptr, _L("Flow rate calibration result has been saved to preset."), wxEmptyString, wxOK);
            msg_dlg.ShowModal();
        }
        else {
            assert(false);
        }
    }
    back_preset_info(curr_obj, true);
    show_step(start_step);
}

void FlowRateWizard::update(MachineObject* obj)
{
    CalibrationWizard::update(obj);
}

void FlowRateWizard::on_device_connected(MachineObject* obj)
{
    CalibrationWizard::on_device_connected(obj);

    CalibrationMethod method;
    int cali_stage = 0;
    CalibMode obj_cali_mode = get_obj_calibration_mode(obj, method, cali_stage);

    // show cali step when obj is in pa calibration
    if (obj) {
        this->set_cali_method(method);
        if (obj_cali_mode == m_mode) {
            if (!obj->cali_finished && (obj->is_in_printing() || obj->is_printing_finished())) {
                if (method == CalibrationMethod::CALI_METHOD_MANUAL) {
                    if (cali_stage == 1) {
                        if (m_curr_step != cali_coarse_step)
                            show_step(cali_coarse_step);
                    }
                    else if (cali_stage == 2) {
                        if (m_curr_step != cali_fine_step) {
                            show_step(cali_fine_step);
                        }
                    }
                    else
                        show_step(cali_coarse_step);
                }
                else if (method == CalibrationMethod::CALI_METHOD_AUTO) {
                    if (m_curr_step != cali_step)
                        show_step(cali_step);
                }
            }
        }
    }
}

std::map<std::string, ConfigIndexValue> FlowRateWizard::generate_index_key_value(MachineObject *obj, const std::string &key, float value)
{
    std::map<std::string, ConfigIndexValue> key_value_map;
    if (!obj)
        return key_value_map;

    std::map<int, TrayInfo> selected_filaments = get_cached_selected_filament_for_multi_extruder(obj);
    int  index = 0;
    if (!selected_filaments.empty()) {
        TrayInfo tray_info = selected_filaments.begin()->second;
        // todo multi_extruder: get_extruder_type from obj
        ExtruderType extruder_type = ExtruderType::etDirectDrive;
        index = get_index_for_extruder_parameter(tray_info.preset->config, "filament_flow_ratio", tray_info.extruder_id, extruder_type, tray_info.nozzle_volume_type);
        ConfigIndexValue config_value;
        config_value.index = index;
        config_value.value = value;
        key_value_map.insert(std::make_pair("filament_flow_ratio", config_value));
    }

    return key_value_map;
}

void FlowRateWizard::set_cali_method(CalibrationMethod method)
{
    m_cali_method = method;
    if (method == CalibrationMethod::CALI_METHOD_AUTO || method == CalibrationMethod::CALI_METHOD_NEW_AUTO) {
        m_page_steps.clear();
        m_page_steps.push_back(start_step);
        m_page_steps.push_back(preset_step);
        m_page_steps.push_back(cali_step);
        m_page_steps.push_back(save_step);

        for (int i = 0; i < m_page_steps.size() - 1; i++) {
            m_page_steps[i]->chain(m_page_steps[i + 1]);
        }
    }
    else if (method == CalibrationMethod::CALI_METHOD_MANUAL) {
        m_page_steps.clear();
        m_page_steps.push_back(start_step);
        m_page_steps.push_back(preset_step);
        m_page_steps.push_back(cali_coarse_step);
        m_page_steps.push_back(coarse_save_step);
        m_page_steps.push_back(cali_fine_step);
        m_page_steps.push_back(fine_save_step);

        for (int i = 0; i < m_page_steps.size() - 1; i++) {
            m_page_steps[i]->chain(m_page_steps[i + 1]);
        }
    }
    else {
        assert(false);
    }
    CalibrationWizard::set_cali_method(method);
}

void FlowRateWizard::on_cali_job_finished(wxString evt_data)
{
    int       cali_stage = 0;
    CalibMode obj_cali_mode = CalibUtils::get_calib_mode_by_name(evt_data.ToStdString(), cali_stage);

    if (obj_cali_mode == m_mode) {
        if (cali_stage == 1) {
            if (m_curr_step != cali_coarse_step)
                show_step(cali_coarse_step);
            // change ui, hide
            static_cast<CalibrationPresetPage*>(preset_step->page)->on_cali_finished_job();
        }
        else if (cali_stage == 2) {
            if (m_curr_step != cali_fine_step) {
                show_step(cali_fine_step);
            }
            // change ui, hide
            static_cast<CalibrationPresetPage*>(preset_step->page)->on_cali_finished_job();
            static_cast<CalibrationFlowCoarseSavePage*>(coarse_save_step->page)->on_cali_finished_job();
        }
        else
            show_step(cali_coarse_step);
    }
}

void FlowRateWizard::cache_coarse_info(MachineObject *obj)
{
    if (!obj) return;

    CalibrationFlowCoarseSavePage *coarse_page = (static_cast<CalibrationFlowCoarseSavePage *>(coarse_save_step->page));
    if (!coarse_page)
        return;

    wxString out_name;
    coarse_page->get_result(&obj->cache_flow_ratio, &out_name);

    back_preset_info(obj, false);
}

MaxVolumetricSpeedWizard::MaxVolumetricSpeedWizard(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : CalibrationWizard(parent, CalibMode::Calib_Vol_speed_Tower, id, pos, size, style)
{
    create_pages();
}

void MaxVolumetricSpeedWizard::create_pages()
{
    start_step  = new CalibrationWizardPageStep(new CalibrationMaxVolumetricSpeedStartPage(m_scrolledWindow));
    preset_step = new CalibrationWizardPageStep(new MaxVolumetricSpeedPresetPage(m_scrolledWindow, m_mode, true));

    // manual
    cali_step = new CalibrationWizardPageStep(new CalibrationCaliPage(m_scrolledWindow, m_mode));
    save_step = new CalibrationWizardPageStep(new CalibrationMaxVolumetricSpeedSavePage(m_scrolledWindow));

    m_all_pages_sizer->Add(start_step->page, 1, wxEXPAND | wxALL, FromDIP(25));
    m_all_pages_sizer->Add(preset_step->page, 1, wxEXPAND | wxALL, FromDIP(25));
    m_all_pages_sizer->Add(cali_step->page, 1, wxEXPAND | wxALL, FromDIP(25));
    m_all_pages_sizer->Add(save_step->page, 1, wxEXPAND | wxALL, FromDIP(25));

    m_page_steps.push_back(start_step);
    m_page_steps.push_back(preset_step);
    m_page_steps.push_back(cali_step);
    m_page_steps.push_back(save_step);

    for (int i = 0; i < m_page_steps.size() - 1; i++) {
        m_page_steps[i]->chain(m_page_steps[i + 1]);
    }

    for (int i = 0; i < m_page_steps.size(); i++) {
        m_page_steps[i]->page->Hide();
        m_page_steps[i]->page->Bind(EVT_CALI_ACTION, &MaxVolumetricSpeedWizard::on_cali_action, this);
    }

    for (auto page_step : m_page_steps) {
        page_step->page->Hide();
    }

    if (!m_page_steps.empty())
        show_step(m_page_steps.front());
    return;
}

void MaxVolumetricSpeedWizard::on_cali_action(wxCommandEvent& evt)
{
    CaliPageActionType action = static_cast<CaliPageActionType>(evt.GetInt());

    if (action == CaliPageActionType::CALI_ACTION_START) {
        show_step(m_curr_step->next);
    }
    else if (action == CaliPageActionType::CALI_ACTION_PREV) {
        show_step(m_curr_step->prev);
    }
    else if (action == CaliPageActionType::CALI_ACTION_CALI) {
        on_cali_start();
    }
    else if (action == CaliPageActionType::CALI_ACTION_NEXT) {
        show_step(m_curr_step->next);
    }
    else if (action == CaliPageActionType::CALI_ACTION_COMMON_SAVE) {
        on_cali_save();
    }
    else if (action == CaliPageActionType::CALI_ACTION_GO_HOME) {
        on_cali_go_home();
    }
}

void MaxVolumetricSpeedWizard::on_cali_start()
{
    float       nozzle_dia = 0.4;
    std::string setting_id;
    BedType     plate_type = BedType::btDefault;

    CalibrationPresetPage *preset_page = (static_cast<CalibrationPresetPage *>(preset_step->page));

    preset_page->get_preset_info(nozzle_dia, plate_type);

    CalibrationWizard::cache_preset_info(curr_obj, nozzle_dia, plate_type);

    wxArrayString values = preset_page->get_custom_range_values();
    Calib_Params  params;
    if (values.size() != 3) {
        MessageDialog msg_dlg(nullptr, _L("The input value size must be 3."), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }
    else {
        values[0].ToDouble(&params.start);
        values[1].ToDouble(&params.end);
        values[2].ToDouble(&params.step);
    }
    params.mode = m_mode;

    std::map<int, Preset *> selected_filaments = preset_page->get_selected_filaments();
    if (selected_filaments.empty()) {
        MessageDialog msg_dlg(nullptr, _L("Please select filament to calibrate."), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    CalibInfo calib_info;
    calib_info.params = params;
    calib_info.dev_id = curr_obj->get_dev_id();
    if (!selected_filaments.empty()) {
        int selected_tray_id = 0;
        get_tray_ams_and_slot_id(curr_obj, selected_filaments.begin()->first, calib_info.ams_id, calib_info.slot_id, selected_tray_id);
        calib_info.select_ams     = std::to_string(selected_tray_id);
        calib_info.extruder_id        = preset_page->get_extruder_id(calib_info.ams_id);
        calib_info.extruder_type      = preset_page->get_extruder_type(calib_info.extruder_id);
        calib_info.nozzle_volume_type = preset_page->get_nozzle_volume_type(calib_info.extruder_id);
        calib_info.filament_prest = selected_filaments.begin()->second;
        std::map<int, DynamicPrintConfig> filament_list = preset_page->get_filament_ams_list();
        calib_info.filament_color = filament_list[selected_filaments.begin()->first].opt_string("filament_colour", 0u);
    }

    calib_info.bed_type      = plate_type;
    calib_info.process_bar   = preset_page->get_sending_progress_bar();
    calib_info.printer_prest = preset_page->get_printer_preset(curr_obj, preset_page->get_nozzle_diameter(calib_info.extruder_id));
    calib_info.print_prest   = preset_page->get_print_preset();

    wxString wx_err_string;
    CalibUtils::calib_max_vol_speed(calib_info, wx_err_string);
    if (!wx_err_string.empty()) {
        MessageDialog msg_dlg(nullptr, wx_err_string, wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    preset_page->on_cali_start_job();

    CalibrationCaliPage* cali_page = (static_cast<CalibrationCaliPage*>(cali_step->page));
    cali_page->clear_last_job_status();
}

void MaxVolumetricSpeedWizard::on_cali_save()
{
    std::string old_preset_name;
    std::string new_preset_name;

    CalibrationPresetPage *preset_page = (static_cast<CalibrationPresetPage *>(preset_step->page));
    std::map<int, Preset *> selected_filaments = get_cached_selected_filament(curr_obj);
    if (!selected_filaments.empty()) {
        old_preset_name = selected_filaments.begin()->second->name;
    }

    double value = 0;
    CalibrationMaxVolumetricSpeedSavePage *save_page = (static_cast<CalibrationMaxVolumetricSpeedSavePage *>(save_step->page));
    if (!save_page->get_save_result(value, new_preset_name)) {
        BOOST_LOG_TRIVIAL(info) << "max_volumetric_speed_cali: get result failed";
        return;
    }

    if (!CalibUtils::validate_input_name(new_preset_name))
        return;

    std::map<std::string, ConfigOption *> key_value_map;
    key_value_map.insert(std::make_pair("filament_max_volumetric_speed", new ConfigOptionFloats{ value }));

    wxString message;
    if (!save_preset(old_preset_name, new_preset_name, key_value_map, message)) {
        MessageDialog error_msg_dlg(nullptr, message, wxEmptyString, wxICON_WARNING | wxOK);
        error_msg_dlg.ShowModal();
        return;
    }

    MessageDialog msg_dlg(nullptr, _L("Max volumetric speed calibration result has been saved to preset."), wxEmptyString, wxOK);
    msg_dlg.ShowModal();
    back_preset_info(curr_obj, true);
    show_step(start_step);
}

void MaxVolumetricSpeedWizard::on_cali_job_finished(wxString evt_data)
{
    int       cali_stage = 0;
    CalibMode obj_cali_mode = CalibUtils::get_calib_mode_by_name(evt_data.ToStdString(), cali_stage);

    if (obj_cali_mode == m_mode) {
        if (m_curr_step != cali_step) {
            show_step(cali_step);
        }
    }
    // change ui, hide
    static_cast<CalibrationPresetPage*>(preset_step->page)->on_cali_finished_job();
}

void MaxVolumetricSpeedWizard::on_device_connected(MachineObject *obj)
{
    CalibrationWizard::on_device_connected(obj);

    CalibrationMethod method;
    int               cali_stage    = 0;
    CalibMode         obj_cali_mode = get_obj_calibration_mode(obj, method, cali_stage);

    if (obj) {
        this->set_cali_method(method);
        if (obj_cali_mode == m_mode) {
            if (!obj->cali_finished && (obj->is_in_printing() || obj->is_printing_finished())) {
                if (m_curr_step != cali_step) {
                    show_step(cali_step);
                }
            }
        }
    }
}
}}
