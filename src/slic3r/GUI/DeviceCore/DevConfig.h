#pragma once
#include "libslic3r/CommonDefs.hpp"

#include "slic3r/Utils/json_diff.hpp"
#include <wx/string.h>
#include <limits>


namespace Slic3r
{

//Previous definitions
class MachineObject;


class DevConfig
{
public:
    DevConfig(MachineObject* obj) : m_obj(obj) {};
    ~DevConfig() = default;

public:
    // chamber
    bool HasChamber() const { return m_has_chamber; }
    bool SupportChamberEdit() const { return m_support_chamber_edit; }
    int  GetChamberTempEditMin() const { return m_chamber_temp_edit_min; }
    int  GetChamberTempEditMax() const { return m_chamber_temp_edit_max; }
    int  GetChamberTempSwitchHeat() const { return m_chamber_temp_switch_heat; }

    // print options
    bool SupportFirstLayerInspect() const { return m_support_first_layer_inspect; }
    bool SupportSaveRemotePrintFileToStorage() const { return m_support_save_remote_print_file_to_storage; }
    bool SupportAIMonitor() const { return m_support_ai_monitor; }

    bool SupportPrintWithoutSD() const { return m_support_print_without_sd; }
    bool SupportPrintAllPlates() const { return m_support_print_all; }

    // calibration options
    bool SupportCalibrationLidar() const { return m_support_calibration_lidar; }
    bool SupportCalibrationNozzleOffset() const { return m_support_calibration_nozzle_offset; }
    bool SupportCalibrationHighTempBed() const { return m_support_calibration_high_temp_bed; }
    bool SupportCaliClumpPos() const { return m_support_calibration_clump_pos; }

    bool SupportCalibrationPA_FlowAuto() const { return m_support_calibration_pa_flow_auto; }

public:
    /*Setters*/
    void ParseConfig(const json& print_json);

    void ParseChamberConfig(const json& print_json); // chamber
    void ParsePrintOptionsConfig(const json& print_json); // print options
    void ParseCalibrationConfig(const json& print_json); //cali

private:
    MachineObject* m_obj;

    /*configure vals*/
    // chamber
    bool m_has_chamber = false; // whether the machine has a chamber
    bool m_support_chamber_edit = false;
    int  m_chamber_temp_edit_min = 0;
    int  m_chamber_temp_edit_max = 60;
    int  m_chamber_temp_switch_heat = std::numeric_limits<int>::max(); /* the min temp to start heating, default to max */

    // print options
    bool m_support_first_layer_inspect = false;
    bool m_support_save_remote_print_file_to_storage = false;
    bool m_support_ai_monitor = false;

    bool m_support_print_without_sd = false;
    bool m_support_print_all = false;

    // calibration options
    bool m_support_calibration_lidar = false;
    bool m_support_calibration_nozzle_offset = false;
    bool m_support_calibration_high_temp_bed = false; // High-temperature Heatbed Calibration
    bool m_support_calibration_clump_pos     = false; // clump position calibration

    bool m_support_calibration_pa_flow_auto = false;// PA flow calibration. used in SendPrint
};

};