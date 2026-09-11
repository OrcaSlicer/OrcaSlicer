#pragma once
#include <nlohmann/json.hpp>
#include "slic3r/Utils/json_diff.hpp"


#include "DevDefs.h"
#include "libslic3r/calib.hpp" // Orca: lowercase filename (Linux case-sensitive)


namespace Slic3r {

class MachineObject;


enum class CalibStatus{
    IDLE = 0,
    REQUEST,
    WAITING,
    FINISHED,
};

enum class ManualPaCaliMethod {
    PA_LINE = 0,
    PA_PATTERN,
};

class DevCalib
{
protected:
    bool                    m_support_new_auto_cali{false};
    int                     m_calib_version {-1};
    std::optional<int>      m_last_calib_version;

public:
    DevCalib(MachineObject *obj) : m_owner(obj){};
    MachineObject* GetOwner() const {return m_owner; };

    void RequestPAResult();
    CalibStatus GetPAResultStatus() const {return m_pa_results_status;}
    bool IsPAResultReady() const { return m_pa_results_status == CalibStatus::FINISHED;}
    void ResetPAResult();

    /* calib history */
    int RequestPAHistory(const PACalibExtruderInfo &calib_info);
    CalibStatus GetPAHistoryStatus() const {return m_pa_table_status;}
    bool IsPAHistoryReady() const { return m_pa_table_status == CalibStatus::FINISHED;}
    void ResetPAHistory();

    void RequestFlowRateResult();
    CalibStatus GetFlowRateResultStatus() const {return m_flow_results_status;}
    bool IsFlowRateReady() const { return m_flow_results_status == CalibStatus::FINISHED;}
    void ResetFlowRateResult();

    int  GetCalibVersion() const {return m_calib_version;}
    void SyncCalibVersion() { if (IsVersionInited()) { m_last_calib_version = m_calib_version; } }
    void ResetCalibVersion() {m_last_calib_version.reset();}
    bool IsVersionExpired() const;
    bool IsVersionInited() const { return m_calib_version > -1;}

    bool IsSupportNewAutoCali() const {return m_support_new_auto_cali;}

public:
    void                        SetStashCalibFinished(bool finished) {m_calib_finished = finished;}
    bool                        GetStashCalibFinished() { return m_calib_finished;}

    void                        SetStashFlowRatio(float ratio) { m_flow_ratio = ratio;}
    float                       GetStashFlowRatio() const {return m_flow_ratio;}

    FlowRatioCalibrationType    GetFlowRatioCalibType() {return m_flow_ratio_calibration_type;}
    void                        SetFlowRatioCalibType(const FlowRatioCalibrationType &type) { m_flow_ratio_calibration_type = type; }

    ManualPaCaliMethod          GetManualPaCalibMethod() {return m_manual_pa_cali_method;}
    void                        SetManualPaCalibMethod(const ManualPaCaliMethod& method) { m_manual_pa_cali_method = method;}

    NozzleDiameterType          GetSelectedNozzleDiameter() {return m_selected_nozzle_diameter;}
    void                        SetSelectedNozzleDiameter(const NozzleDiameterType& diameter) { m_selected_nozzle_diameter = diameter;}

    std::vector<CaliPresetInfo> GetSelectedCalibPreset() {return m_selected_calib_preset;}
    void                        ResetSelectedCalibPreset() { m_selected_calib_preset.clear();}
    void                        SetSelectedCalibPreset(const std::vector<CaliPresetInfo>& preset) { m_selected_calib_preset = preset;}

    std::vector<PACalibResult>          GetPAHistory() const {return m_pa_calib_tab; }
    std::vector<PACalibResult>          GetPAResult() const {return m_pa_calib_results; }
    std::vector<FlowRatioCalibResult>   GetFlowRatioResult() const {return m_flow_ratio_results; }

protected:
    void ExtrusionCalibSetParse(const json &jj);
    void ExtrusionCalibSelectParse(const json &jj);
    void ExtrusionCalibGetTableParse(const json &jj);
    void ExtrusionCalibGetResultParse(const json &jj);
    void FlowrateGetResultParse(const json &jj);

private:
    MachineObject* m_owner{nullptr};

    std::vector<PACalibResult>          m_pa_calib_tab;
    std::vector<PACalibResult>          m_pa_calib_results;
    std::vector<FlowRatioCalibResult>   m_flow_ratio_results;

    CalibStatus m_pa_results_status{CalibStatus::IDLE};
    CalibStatus m_pa_table_status{CalibStatus::IDLE};
    CalibStatus m_flow_results_status{CalibStatus::IDLE};

    FlowRatioCalibrationType   m_flow_ratio_calibration_type{FlowRatioCalibrationType::COMPLETE_CALIBRATION};
    ManualPaCaliMethod         m_manual_pa_cali_method{ManualPaCaliMethod::PA_LINE};

    // calibration page selected info
    // 1: record when start calibration in preset page
    // 2: reset when start calibration in start page
    // 3: save tray_id, filament_id, setting_id, and name, nozzle_dia
    // std::vector<CaliPresetInfo> selected_cali_preset;
    NozzleDiameterType          m_selected_nozzle_diameter{NozzleDiameterType::NONE_DIAMETER_TYPE};
    std::vector<CaliPresetInfo> m_selected_calib_preset;

    // stash calibrating info
    float                       m_flow_ratio { 0.0 };
    bool                        m_calib_finished{false};

public:
    static void ParseCalibVersion(const json& j, DevCalib* system);

    static void ParseSupportNewAutoCalib(int flag, DevCalib* system);

    static void ParseV1_0(const json& print_json, DevCalib* system, bool key_field_only);
};

} // namespace Slic3r