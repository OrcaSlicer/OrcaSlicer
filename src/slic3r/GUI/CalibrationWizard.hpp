#ifndef slic3r_GUI_CalibrationWizard_hpp_
#define slic3r_GUI_CalibrationWizard_hpp_

#include "slic3r/Utils/CalibUtils.hpp"

#include "DeviceManager.hpp"
#include "CalibrationWizardPage.hpp"
#include "CalibrationWizardStartPage.hpp"
#include "CalibrationWizardPresetPage.hpp"
#include "CalibrationWizardCaliPage.hpp"
#include "CalibrationWizardSavePage.hpp"

namespace Slic3r { namespace GUI {


class CalibrationWizardPageStep
{
public:
    CalibrationWizardPageStep(CalibrationWizardPage* data) {
        page = data;
    }

    CalibrationWizardPageStep* prev { nullptr };
    CalibrationWizardPageStep* next { nullptr };
    CalibrationWizardPage*     page { nullptr };
    void chain(CalibrationWizardPageStep* step) {
        if (!step) return;
        this->next = step;
        step->prev = this;
    }
};

struct ConfigIndexValue
{
    float value{0};
    int   index{0};
};

class CalibrationWizard : public wxPanel {
public:
    CalibrationWizard(wxWindow* parent, CalibMode mode,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);

    ~CalibrationWizard();

    void on_cali_job_finished(wxCommandEvent& event);

    virtual void on_cali_job_finished(wxString evt_data) {}

    CalibrationWizardPageStep* get_curr_step() { return m_curr_step; }

    void show_step(CalibrationWizardPageStep* step);

    virtual void update(MachineObject* obj);

    virtual void on_device_connected(MachineObject* obj);

    virtual void set_cali_style(CalibrationStyle style) {
        m_cali_style = style;
    }

    virtual void set_cali_method(CalibrationMethod method);

    CalibMode get_calibration_mode() { return m_mode; }

    bool save_preset(const std::string &old_preset_name, const std::string &new_preset_name, const std::map<std::string, ConfigOption *> &key_values, wxString& message);
    bool save_preset_with_index(const std::string &old_preset_name, const std::string &new_preset_name, const std::map<std::string, ConfigIndexValue> &key_values, wxString &message);

    virtual void cache_preset_info(MachineObject *obj, float nozzle_dia, BedType bed_type);
    virtual void recover_preset_info(MachineObject *obj);
    virtual void back_preset_info(MachineObject *obj, bool cali_finish, bool back_cali_flag = true);

    void msw_rescale();
    void on_sys_color_changed();

protected:
    void on_cali_go_home();

protected:
    /* wx widgets*/
    wxScrolledWindow* m_scrolledWindow;
    wxBoxSizer* m_all_pages_sizer;

    CalibMode           m_mode;
    CalibrationStyle    m_cali_style;
    CalibrationMethod   m_cali_method { CalibrationMethod::CALI_METHOD_MANUAL };
    MachineObject*      curr_obj { nullptr };
    MachineObject*      last_obj { nullptr };

    CalibrationWizardPageStep* m_curr_step { nullptr };

    CalibrationWizardPageStep* start_step { nullptr };
    CalibrationWizardPageStep* preset_step { nullptr };
    CalibrationWizardPageStep* cali_step { nullptr };
    CalibrationWizardPageStep* save_step { nullptr };

    CalibrationWizardPageStep* cali_coarse_step { nullptr };
    CalibrationWizardPageStep* coarse_save_step { nullptr };
    CalibrationWizardPageStep* cali_fine_step { nullptr };
    CalibrationWizardPageStep* fine_save_step { nullptr };

    /* save steps of calibration pages */
    std::vector<CalibrationWizardPageStep*> m_page_steps;

    SecondaryCheckDialog *go_home_dialog = nullptr;
};

class PressureAdvanceWizard : public CalibrationWizard {
public:
    PressureAdvanceWizard(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = wxTAB_TRAVERSAL);
    ~PressureAdvanceWizard() {};

    void on_cali_job_finished(wxString evt_data) override;

protected:
    void create_pages();

    void on_cali_start();

    void on_cali_save();

    void on_cali_action(wxCommandEvent& evt);

    void update(MachineObject* obj) override;

    void on_device_connected(MachineObject* obj) override;

    bool can_save_cali_result(const std::vector<PACalibResult> &new_pa_cali_results);

    bool                       m_show_result_dialog = false;
    std::vector<PACalibResult> m_calib_results_history;
    int                        cali_version = -1;
};

class FlowRateWizard : public CalibrationWizard {
public:
    FlowRateWizard(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = wxTAB_TRAVERSAL);
    ~FlowRateWizard() {};

    void set_cali_method(CalibrationMethod method) override;

    void on_cali_job_finished(wxString evt_data) override;

    void cache_coarse_info(MachineObject *obj);

protected:
    void create_pages();

    void on_cali_action(wxCommandEvent& evt);

    void on_cali_start(CaliPresetStage stage = CaliPresetStage::CALI_MANULA_STAGE_NONE, float cali_value = 0.0f, FlowRatioCaliSource from_page = FlowRatioCaliSource::FROM_PRESET_PAGE);

    void on_cali_save();

    void update(MachineObject* obj) override;

    void on_device_connected(MachineObject* obj) override;

    std::map<std::string, ConfigIndexValue> generate_index_key_value(MachineObject *obj, const std::string &key, float value);
};

class MaxVolumetricSpeedWizard : public CalibrationWizard {
public:
    MaxVolumetricSpeedWizard(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = wxTAB_TRAVERSAL);
    ~MaxVolumetricSpeedWizard() {};

    void on_cali_job_finished(wxString evt_data) override;

protected:
    void create_pages();

    void on_cali_action(wxCommandEvent& evt);

    void on_cali_start();

    void on_cali_save();

    void on_device_connected(MachineObject *obj) override;
};

// save printer_type in command event
wxDECLARE_EVENT(EVT_DEVICE_CHANGED, wxCommandEvent);
wxDECLARE_EVENT(EVT_CALIBRATION_JOB_FINISHED, wxCommandEvent);

}} // namespace Slic3r::GUI

#endif
