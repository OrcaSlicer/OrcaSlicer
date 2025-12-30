#ifndef slic3r_GUI_CalibrationWizardSavePage_hpp_
#define slic3r_GUI_CalibrationWizardSavePage_hpp_

#include "CalibrationWizardPage.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r { namespace GUI {

enum CaliSaveStyle {
    CALI_SAVE_P1P_STYLE = 0,
    CALI_SAVE_X1_STYLE,
};


class CalibrationCommonSavePage : public CalibrationWizardPage
{
public:
    CalibrationCommonSavePage(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = wxTAB_TRAVERSAL);

protected:
    wxBoxSizer* m_top_sizer;
};

class PAColumnDataPanel : wxPanel {
public:
    PAColumnDataPanel(
        wxWindow* parent,
        bool is_failed,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);

    bool is_failed() { return m_is_failed; }
    int get_col_idx() { return m_col_idx; }
    wxString get_k_str();
    wxString get_n_str();
    wxString get_name();
    void set_data(wxString k_str, wxString n_str, wxString name);

private:
    wxBoxSizer* m_top_sizer;
    TextInput* m_k_value_input;
    TextInput* m_n_value_input;
    ComboBox* m_comboBox_tray_name;
    int m_col_idx;
    bool m_is_failed;
};

class CaliSavePresetValuePanel : public wxPanel
{
protected:
    wxBoxSizer* m_top_sizer;
    CaliPagePicture* m_picture_panel;
    Label* m_value_title;
    Label* m_save_name_title;
    ::TextInput* m_input_value;
    ::TextInput* m_input_name;


public:
    CaliSavePresetValuePanel(
        wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);

    void create_panel(wxWindow* parent);

    void set_img(const std::string& bmp_name_in);
    void set_value_title(const wxString& title);
    void set_save_name_title(const wxString& title);
    void get_value(double& value);
    void get_save_name(std::string& name);
    void set_save_name(const std::string& name);
    void msw_rescale();
};


class CaliPASaveAutoPanel : public wxPanel
{
public:
    CaliPASaveAutoPanel(
        wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);

    void create_panel(wxWindow* parent);

    void set_machine_obj(MachineObject* obj) { m_obj = obj; }

    std::vector<std::pair<int, std::string>> default_naming(std::vector<std::pair<int, std::string>> preset_names);
    void sync_cali_result(const std::vector<PACalibResult>& cali_result, const std::vector<PACalibResult>& history_result);
    void save_to_result_from_widgets(wxWindow* window, bool* out_is_valid, wxString* out_msg);
    bool get_result(std::vector<PACalibResult>& out_result);
    bool is_all_failed() { return m_is_all_failed; }

protected:
    void sync_cali_result_for_multi_extruder(const std::vector<PACalibResult> &cali_result, const std::vector<PACalibResult> &history_result);

protected:
    wxBoxSizer* m_top_sizer;
    wxPanel* m_complete_text_panel;
    wxPanel* m_part_failed_panel;
    wxPanel*    m_grid_panel{ nullptr };
    wxPanel*    m_multi_extruder_grid_panel{ nullptr };
    std::map<int, PACalibResult> m_calib_results;// map<tray_id, PACalibResult>
    std::vector<PACalibResult> m_history_results;
    bool m_is_all_failed{ true };
    MachineObject* m_obj{ nullptr };
};

class CaliPASaveManualPanel : public wxPanel
{
public:
    CaliPASaveManualPanel(
        wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);
    void create_panel(wxWindow* parent);
    void set_save_img();
    void set_pa_cali_method(ManualPaCaliMethod method);

    void set_machine_obj(MachineObject* obj) { m_obj = obj; }

    void set_default_name(const wxString& name);

    bool get_result(PACalibResult& out_result);

    virtual bool Show(bool show = true) override;

    void msw_rescale();

protected:
    wxBoxSizer* m_top_sizer;
    Label *          m_complete_text;
    CaliPagePicture* m_picture_panel;
    ::TextInput* m_save_name_input;
    ::TextInput* m_k_val;
    ::TextInput* m_n_val;

    MachineObject* m_obj{ nullptr };
};

class CaliPASaveP1PPanel : public wxPanel
{
public:
    CaliPASaveP1PPanel(
        wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);
    void create_panel(wxWindow* parent);
    void set_save_img();
    void set_pa_cali_method(ManualPaCaliMethod method);

    bool get_result(float* out_k, float* out_n);

    virtual bool Show(bool show = true) override;

    void msw_rescale();

protected:
    wxBoxSizer* m_top_sizer;
    Label *          m_complete_text;
    CaliPagePicture* m_picture_panel;
    ::TextInput* m_k_val;
    ::TextInput* m_n_val;
};

class CalibrationPASavePage : public CalibrationCommonSavePage
{
public:
    CalibrationPASavePage(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = wxTAB_TRAVERSAL);

    void create_page(wxWindow* parent);
    void set_cali_method(CalibrationMethod method) override;
    // sync widget value from obj cali result
    void sync_cali_result(MachineObject* obj);
    bool get_auto_result(std::vector<PACalibResult>& result) { return m_auto_panel->get_result(result); }
    bool is_all_failed() { return m_auto_panel->is_all_failed(); }
    bool get_manual_result(PACalibResult& result) { return m_manual_panel->get_result(result); }
    bool get_p1p_result(float* k, float* n) { return m_p1p_panel->get_result(k, n); }

    void show_panels(CalibrationMethod method, const PrinterSeries printer_ser);

    void on_device_connected(MachineObject* obj);

    void update(MachineObject* obj) override;

    virtual bool Show(bool show = true) override;

    void msw_rescale() override;

protected:
    CaliPageStepGuide*  m_step_panel { nullptr };
    CaliPASaveAutoPanel*  m_auto_panel { nullptr };
    CaliPASaveManualPanel* m_manual_panel { nullptr };
    CaliPASaveP1PPanel* m_p1p_panel{ nullptr };
    PAPageHelpPanel* m_help_panel{ nullptr };

    CaliSaveStyle m_save_style;
};

class CalibrationFlowX1SavePage : public CalibrationCommonSavePage
{
public:
    CalibrationFlowX1SavePage(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = wxTAB_TRAVERSAL);

    void create_page(wxWindow* parent);

    // sync widget value from cali flow rate result
    void sync_cali_result(const std::vector<FlowRatioCalibResult>& cali_result);
    void save_to_result_from_widgets(wxWindow* window, bool* out_is_valid, wxString* out_msg);
    bool get_result(std::vector<std::pair<wxString, float>>& out_results);
    bool is_all_failed() { return m_is_all_failed; }

    virtual bool Show(bool show = true) override;

    void msw_rescale() override;

protected:
    CaliPageStepGuide* m_step_panel{ nullptr };
    wxPanel* m_complete_text_panel;
    wxPanel* m_part_failed_panel;
    wxPanel*           m_grid_panel{ nullptr };
    std::map<int, std::pair<wxString, float>> m_save_results; // map<tray_id, <name, flow ratio>>
    bool m_is_all_failed{ true };
};

class CalibrationFlowCoarseSavePage : public CalibrationCommonSavePage
{
public:
    CalibrationFlowCoarseSavePage(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = wxTAB_TRAVERSAL);

    void create_page(wxWindow* parent);
    void set_save_img();

    void set_default_options(const wxString &name);

    bool is_skip_fine_calibration();

    void set_curr_flow_ratio(float value);

    bool get_result(float* out_value, wxString* out_name);

    virtual bool Show(bool show = true) override;

    void update_print_error_info(int code, const std::string& msg, const std::string& extra) { m_sending_panel->update_print_error_info(code, msg, extra); }

    void on_cali_start_job();

    void on_cali_finished_job();

    void on_cali_cancel_job();

    std::shared_ptr<ProgressIndicator> get_sending_progress_bar() {
        return m_sending_panel->get_sending_progress_bar();
    }

    void msw_rescale() override;

protected:
    CaliPageStepGuide* m_step_panel{ nullptr };
    CaliPagePicture*   m_picture_panel;
    ComboBox*          m_optimal_block_coarse;
    TextInput*         m_save_name_input;

    Label* m_coarse_calc_result_text;
    CheckBox* m_checkBox_skip_calibration;

    bool m_skip_fine_calibration = false;
    float m_curr_flow_ratio;
    float m_coarse_flow_ratio;

    CaliPageSendingPanel* m_sending_panel{ nullptr };
};

class CalibrationFlowFineSavePage : public CalibrationCommonSavePage
{
public:
    CalibrationFlowFineSavePage(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = wxTAB_TRAVERSAL);

    void create_page(wxWindow* parent);
    void set_save_img();

    void set_default_options(const wxString &name);

    void set_curr_flow_ratio(float value);

    bool get_result(float* out_value, wxString* out_name);

    virtual bool Show(bool show = true) override;

    void msw_rescale() override;

protected:
    CaliPageStepGuide* m_step_panel{ nullptr };
    CaliPagePicture*   m_picture_panel;
    ComboBox*          m_optimal_block_fine;
    TextInput*         m_save_name_input;

    Label* m_fine_calc_result_text;

    float m_curr_flow_ratio;
    float m_fine_flow_ratio;
};

class CalibrationMaxVolumetricSpeedSavePage : public CalibrationCommonSavePage
{
public:
    CalibrationMaxVolumetricSpeedSavePage(wxWindow *parent, wxWindowID id = wxID_ANY,
        const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize, long style = wxTAB_TRAVERSAL);

    void create_page(wxWindow *parent);
    void set_save_img();

    bool get_save_result(double &value, std::string &name);

    void set_prest_name(const std::string &name) { m_save_preset_panel->set_save_name(name); };

    virtual bool Show(bool show = true) override;

protected:
    CaliPageStepGuide *m_step_panel{nullptr};
    CaliSavePresetValuePanel *m_save_preset_panel;
};


}} // namespace Slic3r::GUI

#endif