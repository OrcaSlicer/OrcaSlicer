#pragma once

// Calibration modes, parameters and results: plain data that the app config and the GUI keep.
// The test pattern generators in calib.hpp need the G-code writer; this header does not.

#include <string>
#include <vector>

#include "PrintConfigBase.hpp"

namespace Slic3r {

enum class CalibMode : int {
    Calib_None = 0,
    Calib_PA_Line,
    Calib_PA_Pattern,
    Calib_PA_Tower,
    Calib_Auto_PA_Line,
    Calib_Flow_Rate,
    Calib_Temp_Tower,
    Calib_Vol_speed_Tower,
    Calib_VFA_Tower,
    Calib_Retraction_tower,
    Calib_Input_shaping_freq,
    Calib_Input_shaping_damp,
    Calib_Cornering
};

enum class CalibState { Start = 0, Preset, Calibration, CoarseSave, FineCalibration, Save, Finish };

struct Calib_Params
{
    Calib_Params() : mode(CalibMode::Calib_None){};
    int extruder_id = 0;
    double    start = 0.0, end = 1.0, step = 0.1;
    bool      print_numbers = false;
    double freqStartX = 0.0, freqEndX = 1.0, freqStartY = 0.0, freqEndY = 1.0;
    int test_model = 0;
    std::string shaper_type;
    std::vector<double> accelerations;
    std::vector<double> speeds;
    // Resolved layer height for the VFA tower (0 = auto: nozzle_diameter / 2). Each speed block is a
    // fixed number of layers tall, so this also determines the physical block height / tower height.
    double vfa_layer_height = 0.0;
    // Scale the calibration model to the nozzle diameter and set the layer height accordingly (temp tower / VFA).
    // When false the 0.4 mm / 0.2 mm reference model is printed as-is.
    bool nozzle_based_resize = true;

    CalibMode mode;
};

// Number of printed layers per speed block in the VFA tower. The base model has 5 mm blocks designed
// for a 0.2 mm layer height (0.4 mm nozzle), i.e. 25 layers per block.
static constexpr int vfa_layers_per_block = 25;
static constexpr double vfa_base_block_height = 5.0;
static constexpr double vfa_base_nozzle_diameter = 0.4;

enum FlowRatioCalibrationType {
    COMPLETE_CALIBRATION = 0,
    FINE_CALIBRATION,
};

class X1CCalibInfos
{
public:
    struct X1CCalibInfo
    {
        int         extruder_id = 0;
        int         tray_id;
        int         ams_id = 0;
        int         slot_id = 0;
        int         bed_temp;
        ExtruderType        extruder_type{ExtruderType::etDirectDrive};
        NozzleVolumeType    nozzle_volume_type = NozzleVolumeType::nvtStandard;
        int         nozzle_temp;
        float       nozzle_diameter;
        std::string filament_id;
        std::string setting_id;
        float       max_volumetric_speed;
        float       flow_rate = 0.98f; // for flow ratio
    };

    std::vector<X1CCalibInfo> calib_datas;
    CalibMode                 cali_mode{ CalibMode::Calib_None };
};

class CaliPresetInfo
{
public:
    int         tray_id;
    int         extruder_id = 0;
    NozzleVolumeType nozzle_volume_type{nvtStandard};
    BedType     bed_type{btDefault};
    float       nozzle_diameter;
    int         nozzle_pos_id{-1};
    std::string nozzle_sn;
    std::string filament_id;
    std::string setting_id;
    std::string name;
};

struct PrinterCaliInfo
{
    std::string                 dev_id;
    bool                        cali_finished = true;
    float                       cache_flow_ratio;
    std::vector<CaliPresetInfo> selected_presets;
    FlowRatioCalibrationType    cache_flow_rate_calibration_type = FlowRatioCalibrationType::COMPLETE_CALIBRATION;
};

class PACalibResult
{
public:
    enum CalibResult {
        CALI_RESULT_SUCCESS = 0,
        CALI_RESULT_PROBLEM = 1,
        CALI_RESULT_FAILED  = 2,
    };
    int         extruder_id = 0;
    NozzleVolumeType nozzle_volume_type;
    int         tray_id = 0;
    int         ams_id = 0;
    int         slot_id = 0;
    int         cali_idx = -1;
    int         nozzle_pos_id = -1; //-1 means no nozzle pos
    float       nozzle_diameter;
    std::string nozzle_sn;
    std::string filament_id;
    std::string setting_id;
    std::string name;
    float       k_value    = 0.0;
    float       n_coef     = 0.0;
    int         confidence = -1; // 0: success  1: uncertain  2: failed
};

struct PACalibIndexInfo
{
    int         extruder_id = 0;
    NozzleVolumeType nozzle_volume_type;
    int         tray_id = 0;
    int         ams_id = 0;
    int         slot_id = 0;
    int         cali_idx = -1; // -1 means default
    int         nozzle_pos_id = -1; //-1 means no nozzle pos
    float       nozzle_diameter;
    std::string nozzle_sn;
    std::string filament_id;
};

struct PACalibExtruderInfo
{
    int              extruder_id = 0;
    NozzleVolumeType nozzle_volume_type;
    int              nozzle_pos_id = -1; //-1 means no nozzle pos
    float            nozzle_diameter;
    std::string      nozzle_sn;
    std::string      filament_id = "";
    bool             use_extruder_id{true};
    bool             use_nozzle_volume_type{true};
};

struct PACalibTabInfo
{
    float pa_calib_tab_nozzle_dia;
    int   extruder_id;
    NozzleVolumeType nozzle_volume_type;
};

class FlowRatioCalibResult
{
public:
    int         tray_id;
    float       nozzle_diameter;
    std::string filament_id;
    std::string setting_id;
    float       flow_ratio;
    int         confidence; // 0: success  1: uncertain  2: failed
};

} // namespace Slic3r
