#pragma once
#include "libslic3r/calib.hpp"
#include "../GUI/DeviceManager.hpp"
#include "../GUI/Jobs/PrintJob.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"

namespace Slic3r {

class ProgressIndicator;
class Preset;

namespace GUI {
extern const double MIN_PA_K_VALUE;
extern const double MAX_PA_K_VALUE;

class CalibInfo
{
public:
    int                                index = -1;
    int                                extruder_id = 0;
    int                                ams_id = 0;
    int                                slot_id = 0;
    float                              nozzle_diameter;
    ExtruderType                       extruder_type{ExtruderType::etDirectDrive};
    NozzleVolumeType                   nozzle_volume_type;
    Calib_Params                       params;
    Preset*                            printer_prest;
    Preset*                            filament_prest;
    Preset*                            print_prest;
    BedType                            bed_type;
    std::string                        filament_color;
    std::string                        dev_id;
    std::string                        select_ams;
    std::shared_ptr<ProgressIndicator> process_bar;
};

class CalibUtils
{
public:
    CalibUtils(){};
    static std::unique_ptr<Worker> print_worker;

    static CalibMode get_calib_mode_by_name(const std::string name, int &cali_stage);

    static void calib_PA(const X1CCalibInfos& calib_infos, int mode, wxString& error_message);

    static void emit_get_PA_calib_results(float nozzle_diameter);
    static bool get_PA_calib_results(std::vector<PACalibResult> &pa_calib_results);

    static void emit_get_PA_calib_infos(const PACalibExtruderInfo &cali_info);
    static bool get_PA_calib_tab(std::vector<PACalibResult> &pa_calib_infos);

    static void set_PA_calib_result(const std::vector<PACalibResult>& pa_calib_values, bool is_auto_cali);
    static void select_PA_calib_result(const PACalibIndexInfo &pa_calib_info);
    static void delete_PA_calib_result(const PACalibIndexInfo &pa_calib_info);

    static void calib_flowrate_X1C(const X1CCalibInfos& calib_infos, wxString& error_message);
    static void emit_get_flow_ratio_calib_results(float nozzle_diameter);
    static bool get_flow_ratio_calib_results(std::vector<FlowRatioCalibResult> &flow_ratio_calib_results);
    static bool calib_flowrate(int pass, const CalibInfo &calib_info, wxString &error_message);

    static void calib_pa_pattern(const CalibInfo &calib_info, Model &model);

    static void set_for_auto_pa_model_and_config(const std::vector<CalibInfo> &calib_info, DynamicPrintConfig &full_config, Model &model);

    static bool calib_generic_auto_pa_cali(const std::vector<CalibInfo> &calib_info, wxString & error_message);
    static bool calib_generic_PA(const CalibInfo &calib_info, wxString &error_message);
    static void calib_temptue(const CalibInfo &calib_info, wxString &error_message);
    static void calib_max_vol_speed(const CalibInfo &calib_info, wxString &error_message);
    static void calib_VFA(const CalibInfo &calib_info, wxString &error_message);
    static void calib_retraction(const CalibInfo &calib_info, wxString &error_message);

    //help function
    static bool is_support_auto_pa_cali(std::string filament_id);

    static int get_selected_calib_idx(const std::vector<PACalibResult> &pa_calib_values, int cali_idx);
    static bool get_pa_k_n_value_by_cali_idx(const MachineObject* obj, int cali_idx, float& out_k, float& out_n);

    static bool validate_input_name(wxString name);
    static bool validate_input_k_value(wxString k_text, float* output_value);
    static bool validate_input_flow_ratio(wxString flow_ratio, float* output_value);

    static bool check_printable_status_before_cali(const MachineObject *obj, const X1CCalibInfos &cali_infos, wxString &error_message);
    static bool check_printable_status_before_cali(const MachineObject *obj, const CalibInfo &cali_info, wxString &error_message);
    static bool check_printable_status_before_cali(const MachineObject *obj, const std::vector<CalibInfo> &cali_infos, wxString &error_message);

private:
    static bool process_and_store_3mf(Model* model, const DynamicPrintConfig& full_config, const Calib_Params& params, wxString& error_message);
    static void send_to_print(const CalibInfo &calib_info, wxString& error_message, int flow_ratio_mode = 0); // 0: none  1: coarse  2: fine
    static void send_to_print(const std::vector<CalibInfo> &calib_infos, wxString &error_message, int flow_ratio_mode = 0); // 0: none  1: coarse  2: fine
};

extern void get_tray_ams_and_slot_id(MachineObject* obj, int in_tray_id, int &ams_id, int &slot_id, int &tray_id);

extern void get_default_k_n_value(const std::string &filament_id, float &k, float &n);
extern wxString get_nozzle_volume_type_name(NozzleVolumeType type);

}
}