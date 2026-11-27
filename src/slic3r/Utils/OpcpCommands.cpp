#include "OpcpCommands.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace Slic3r::opcp {

namespace {

// {"<ns>": {"command": ..., "sequence_id": "<n>"}}, the shape every command starts from.
json command(const char* ns, const std::string& name, int sequence_id)
{
    json j;
    j[ns]["command"]     = name;
    j[ns]["sequence_id"] = std::to_string(sequence_id);
    return j;
}

} // namespace

namespace info {
json get_version(int sequence_id) { return command("info", "get_version", sequence_id); }
} // namespace info

namespace pushing {
json pushall(int sequence_id)
{
    json j                      = command("pushing", "pushall", sequence_id);
    j["pushing"]["version"]     = 1;
    j["pushing"]["push_target"] = 1;
    return j;
}

json pushing(int sequence_id, const std::string& command) { return Slic3r::opcp::command("pushing", command, sequence_id); }
} // namespace pushing

namespace system {
json get_access_code(int sequence_id) { return command("system", "get_access_code", sequence_id); }

json close_print_error_dialog(int sequence_id, int print_error)
{
    json j                = command("system", "uiop", sequence_id);
    j["system"]["name"]   = "print_error";
    j["system"]["action"] = "close";
    j["system"]["source"] = 1; // 0 the printer's screen, 1 the slicer
    j["system"]["type"]   = "dialog";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%08X", static_cast<unsigned>(print_error));
    j["system"]["err"] = std::string(buf);
    return j;
}

json ledctrl(int sequence_id, const std::string& led_node, const std::string& led_mode, int on_time, int off_time, int loops, int interval)
{
    json j                       = command("system", "ledctrl", sequence_id);
    j["system"]["led_node"]      = led_node;
    j["system"]["led_mode"]      = led_mode;
    j["system"]["led_on_time"]   = on_time;
    j["system"]["led_off_time"]  = off_time;
    j["system"]["loop_times"]    = loops;
    j["system"]["interval_time"] = interval;
    return j;
}

json set_door_stat(int sequence_id, int config)
{
    json j                = command("system", "set_door_stat", sequence_id);
    j["system"]["config"] = config;
    return j;
}

json print_cache_set(int sequence_id, bool save)
{
    json j                = command("system", "print_cache_set", sequence_id);
    j["system"]["config"] = save;
    return j;
}
} // namespace system

namespace upgrade {
namespace {
json from_slicer(const char* name, int sequence_id)
{
    json j                 = command("upgrade", name, sequence_id);
    j["upgrade"]["src_id"] = 1; // 1 for the slicer
    return j;
}
} // namespace

json upgrade_confirm(int sequence_id) { return from_slicer("upgrade_confirm", sequence_id); }
json consistency_confirm(int sequence_id) { return from_slicer("consistency_confirm", sequence_id); }

json start(int sequence_id, const std::string& url, const std::string& module, const std::string& version)
{
    json j                  = from_slicer("start", sequence_id);
    j["upgrade"]["url"]     = url;
    j["upgrade"]["module"]  = module;
    j["upgrade"]["version"] = version;
    return j;
}
} // namespace upgrade

namespace print {
namespace {
json with_param(const char* name, int sequence_id, const std::string& param)
{
    json j              = command("print", name, sequence_id);
    j["print"]["param"] = param;
    return j;
}
} // namespace

json clean_print_error(int sequence_id, const std::string& subtask_id, int print_error)
{
    json j                    = command("print", "clean_print_error", sequence_id);
    j["print"]["subtask_id"]  = subtask_id;
    j["print"]["print_error"] = print_error;
    return j;
}

json skip_objects(int sequence_id, const std::vector<int>& object_ids)
{
    json j                 = command("print", "skip_objects", sequence_id);
    j["print"]["obj_list"] = object_ids;
    return j;
}

json stop(int sequence_id) { return with_param("stop", sequence_id, ""); }

json stop_job(int sequence_id, const std::string& job_id)
{
    json j               = stop(sequence_id);
    j["print"]["job_id"] = job_id;
    return j;
}

json pause(int sequence_id) { return with_param("pause", sequence_id, ""); }
json resume(int sequence_id) { return with_param("resume", sequence_id, ""); }

json idle_ignore(int sequence_id, const std::string& error, int type)
{
    json j             = command("print", "idle_ignore", sequence_id);
    j["print"]["err"]  = error;
    j["print"]["type"] = type;
    return j;
}

json hms_action(int sequence_id, const std::string& command, const std::string& error, const std::string& job_id)
{
    json j               = with_param(command.c_str(), sequence_id, "reserve");
    j["print"]["err"]    = error;
    j["print"]["job_id"] = job_id;
    return j;
}

json stop_buzzer(int sequence_id)
{
    json j             = command("print", "buzzer_ctrl", sequence_id);
    j["print"]["mode"] = 0;
    return j;
}

json set_nozzle_temp(int sequence_id, int extruder_index, int target_temp)
{
    json j                       = command("print", "set_nozzle_temp", sequence_id);
    j["print"]["extruder_index"] = extruder_index;
    j["print"]["target_temp"]    = target_temp;
    return j;
}

json refresh_nozzle(int sequence_id) { return command("print", "refresh_nozzle", sequence_id); }

json set_chamber_temp(int sequence_id, int temp)
{
    json j                = command("print", "set_ctt", sequence_id);
    j["print"]["ctt_val"] = temp;
    return j;
}

json select_extruder(int sequence_id, int extruder_index)
{
    json j                       = command("print", "select_extruder", sequence_id);
    j["print"]["extruder_index"] = extruder_index;
    return j;
}

json set_extrusion_length(int sequence_id, int extruder_index, int length)
{
    json j                       = command("print", "set_extrusion_length", sequence_id);
    j["print"]["extruder_index"] = extruder_index;
    j["print"]["length"]         = length;
    return j;
}

json print_speed(int sequence_id, int level) { return with_param("print_speed", sequence_id, std::to_string(level)); }

json print_option(int sequence_id, const std::string& key, const json& value)
{
    json j          = command("print", "print_option", sequence_id);
    j["print"][key] = value;
    return j;
}

json print_option_auto_recovery(int sequence_id, int option, bool auto_recovery)
{
    json j               = print_option(sequence_id, "auto_recovery", auto_recovery);
    j["print"]["option"] = option;
    return j;
}

json set_against_continued_heating_mode(int sequence_id, bool enable)
{
    json j               = command("print", "set_against_continued_heating_mode", sequence_id);
    j["print"]["enable"] = enable;
    return j;
}

json close_air_filter(int sequence_id) { return command("print", "close_air_filt", sequence_id); }

json ams_user_setting(int sequence_id, bool startup_read_option, bool tray_read_option, bool calibrate_remain_flag)
{
    json j                              = command("print", "ams_user_setting", sequence_id);
    j["print"]["ams_id"]                = -1; // every unit
    j["print"]["startup_read_option"]   = startup_read_option;
    j["print"]["tray_read_option"]      = tray_read_option;
    j["print"]["calibrate_remain_flag"] = calibrate_remain_flag;
    return j;
}

json ams_control(int sequence_id, const std::string& action) { return with_param("ams_control", sequence_id, action); }
json auto_stop_ams_dry(int sequence_id) { return command("print", "auto_stop_ams_dry", sequence_id); }
json ams_reset(int sequence_id) { return command("print", "ams_reset", sequence_id); }

json ams_filament_drying(int sequence_id,
                         int ams_id,
                         int mode,
                         const std::string& filament,
                         int temp,
                         int duration_hours,
                         bool rotate_tray,
                         int cooling_temp,
                         bool close_power_conflict)
{
    json j                             = command("print", "ams_filament_drying", sequence_id);
    j["print"]["ams_id"]               = ams_id;
    j["print"]["mode"]                 = mode;
    j["print"]["filament"]             = filament;
    j["print"]["temp"]                 = temp;
    j["print"]["duration"]             = duration_hours;
    j["print"]["humidity"]             = 0;
    j["print"]["rotate_tray"]          = rotate_tray;
    j["print"]["cooling_temp"]         = cooling_temp;
    j["print"]["close_power_conflict"] = close_power_conflict;
    return j;
}
} // namespace print

namespace camera {
namespace {
json control(const char* name, int sequence_id, bool enable)
{
    json j                 = command("camera", name, sequence_id);
    j["camera"]["control"] = enable ? "enable" : "disable";
    return j;
}
} // namespace

json ipcam_record_set(int sequence_id, bool enable) { return control("ipcam_record_set", sequence_id, enable); }
json ipcam_timelapse(int sequence_id, bool enable) { return control("ipcam_timelapse", sequence_id, enable); }
json ipcam_cap_pic_set(int sequence_id, bool enable) { return control("ipcam_cap_pic_set", sequence_id, enable); }

json ipcam_resolution_set(int sequence_id, const std::string& resolution)
{
    json j                    = command("camera", "ipcam_resolution_set", sequence_id);
    j["camera"]["resolution"] = resolution;
    return j;
}

json timelapse_storage_check(int sequence_id, const std::string& storage, int total_layer)
{
    json j                     = command("camera", "ipcam_get_media_info", sequence_id);
    j["camera"]["sub_command"] = "is_timelapse_storage_enough";
    j["camera"]["storage"]     = storage;
    j["camera"]["total_layer"] = total_layer;
    return j;
}

json delete_oldest_timelapse(int sequence_id, const std::string& storage, int total_layer)
{
    json j                     = command("camera", "ipcam_delete_oldest_timelapse", sequence_id);
    j["camera"]["storage"]     = storage;
    j["camera"]["total_layer"] = total_layer;
    return j;
}
} // namespace camera

namespace xcam {
json xcam_control_set(int sequence_id, const std::string& module_name, bool on, const std::string& sensitivity)
{
    json j                   = command("xcam", "xcam_control_set", sequence_id);
    j["xcam"]["module_name"] = module_name;
    j["xcam"]["control"]     = on;
    j["xcam"]["enable"]      = on;   // older firmware reads this
    j["xcam"]["print_halt"]  = true; // older firmware reads this
    if (!sensitivity.empty())
        j["xcam"]["halt_print_sensitivity"] = sensitivity;
    return j;
}
} // namespace xcam

} // namespace Slic3r::opcp
