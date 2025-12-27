#pragma once
#include <nlohmann/json.hpp>
#include "slic3r/Utils/json_diff.hpp"
#include <wx/string.h>

#include "DevDefs.h"

namespace Slic3r {

class MachineObject;

class DevPrintOptions
{
    friend class DevPrintOptionsParser;
public:
    DevPrintOptions(MachineObject* obj): m_obj(obj) {}


public:
    void SetPrintingSpeedLevel(DevPrintingSpeedLevel speed_level);
    DevPrintingSpeedLevel GetPrintingSpeedLevel() const { return m_speed_level;}

    // detect options
    int command_xcam_control_ai_monitoring(bool on_off, std::string lvl);
    int command_xcam_control_first_layer_inspector(bool on_off, bool print_halt);
    int command_xcam_control_buildplate_marker_detector(bool on_off);
    int command_xcam_control_auto_recovery_step_loss(bool on_off);
    int command_xcam_control_allow_prompt_sound(bool on_off);
    int command_xcam_control_filament_tangle_detect(bool on_off);
    int command_xcam_control_idelheatingprotect_detector(bool on_off);


    int command_xcam_control(std::string module_name, bool on_off,  MachineObject *obj ,std::string lvl = "");
    // set print option
    int command_set_printing_option(bool auto_recovery, MachineObject *obj);
    // set prompt sound
    int command_set_prompt_sound(bool prompt_sound, MachineObject *obj);
    // set fliament tangle detect
    int command_set_filament_tangle_detect(bool fliament_tangle_detect, MachineObject *obj);

    int command_set_against_continued_heating_mode(bool on_off);

    void parse_auto_recovery_step_loss_status(int flag);
    void parse_allow_prompt_sound_status(int flag);
    void parse_filament_tangle_detect_status(int flag);

    bool GetAiMonitoring() const { return xcam_ai_monitoring; }
    bool GetFirstLayerInspector() const{ return xcam_first_layer_inspector; }
    bool GetBuildplateMarkerDetector() const { return xcam_buildplate_marker_detector; }
    bool GetAutoRecoveryStepLoss() const { return xcam_auto_recovery_step_loss; }
    bool GetAllowPromptSound() const { return xcam_allow_prompt_sound; }
    bool GetFilamentTangleDetect() const { return xcam_filament_tangle_detect; }
    int  GetIdelHeatingProtectEenabled() const { return idel_heating_protect_enabled; }
    string GetAiMonitoringSensitivity() const { return xcam_ai_monitoring_sensitivity; }


private:
    // print option
    DevPrintingSpeedLevel m_speed_level = SPEED_LEVEL_INVALID;

    // detect options
    bool        xcam_ai_monitoring{false};

    std::string xcam_ai_monitoring_sensitivity;
    bool        xcam_buildplate_marker_detector{false};
    bool        xcam_first_layer_inspector{false};
    bool        xcam_auto_recovery_step_loss{false};
    bool        xcam_allow_prompt_sound{false};
    bool        xcam_filament_tangle_detect{false};
    int         idel_heating_protect_enabled           = -1;
    time_t      xcam_ai_monitoring_hold_start          = 0;
    time_t      xcam_buildplate_marker_hold_start      = 0;
    time_t      xcam_first_layer_hold_start            = 0;
    time_t      xcam_auto_recovery_hold_start          = 0;
    time_t      xcam_prompt_sound_hold_start           = 0;
    time_t      xcam_filament_tangle_detect_hold_start = 0;
    time_t      idel_heating_protect_hold_strat        = 0;

    MachineObject* m_obj;/*owner*/
};

class DevPrintOptionsParser
{
public:
    static void Parse(DevPrintOptions* opts, const nlohmann::json& print_json);

    //V1 stands for parse_json; V2 stands for parse_new_json
    static void ParseDetectionV1_0(DevPrintOptions *opts, MachineObject *obj, const nlohmann::json &print_json);
    static void ParseDetectionV1_1(DevPrintOptions *opts, MachineObject *obj, const nlohmann::json &print_json, bool enable);
    static void ParseDetectionV1_2(DevPrintOptions *opts, MachineObject *obj, const nlohmann::json &print_json);

    static void ParseDetectionV2_0(DevPrintOptions *opts, std::string cfg);
    static void ParseDetectionV2_1(DevPrintOptions *opts, std::string cfg);
};

} // namespace Slic3r