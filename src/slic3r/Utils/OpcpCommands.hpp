#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// The commands the device layer sends to a printer, one builder per wire message, in the dialect every
// printer agent receives (OPCP 1.1, which is Bambu's MQTT JSON). Each returns the message without
// sending it; `sequence_id` is the number from PrinterSequenceIds. The namespaces mirror the message's
// top-level key. Field names, types and values are the wire contract, so a builder never changes one
// without changing every agent that translates it.
namespace Slic3r::opcp {

using json = nlohmann::json;

namespace info {
json get_version(int sequence_id);
} // namespace info

namespace pushing {
json pushall(int sequence_id);
// `command` is "start" or "stop".
json pushing(int sequence_id, const std::string& command);
} // namespace pushing

namespace system {
json get_access_code(int sequence_id);
// Closes the print-error dialog for `print_error` on the printer's screen.
json close_print_error_dialog(int sequence_id, int print_error);
json ledctrl(int sequence_id, const std::string& led_node, const std::string& led_mode, int on_time, int off_time, int loops, int interval);
// `config`: 0 off, 1 warn, 2 pause the print.
json set_door_stat(int sequence_id, int config);
json print_cache_set(int sequence_id, bool save);
} // namespace system

namespace upgrade {
json upgrade_confirm(int sequence_id);
json consistency_confirm(int sequence_id);
json start(int sequence_id, const std::string& url, const std::string& module, const std::string& version);
} // namespace upgrade

namespace print {
json clean_print_error(int sequence_id, const std::string& subtask_id, int print_error);
json skip_objects(int sequence_id, const std::vector<int>& object_ids);
json stop(int sequence_id);
json stop_job(int sequence_id, const std::string& job_id);
json pause(int sequence_id);
json resume(int sequence_id);
json idle_ignore(int sequence_id, const std::string& error, int type);
// An answer to an HMS error during a job: `command` is "resume", "ignore" or "stop".
json hms_action(int sequence_id, const std::string& command, const std::string& error, const std::string& job_id);
json stop_buzzer(int sequence_id);
json set_nozzle_temp(int sequence_id, int extruder_index, int target_temp);
json refresh_nozzle(int sequence_id);
json set_chamber_temp(int sequence_id, int temp);
json select_extruder(int sequence_id, int extruder_index);
json set_extrusion_length(int sequence_id, int extruder_index, int length);
json print_speed(int sequence_id, int level);
// One print option: `key` names it, as in {"sound_enable": false}.
json print_option(int sequence_id, const std::string& key, const json& value);
json print_option_auto_recovery(int sequence_id, int option, bool auto_recovery);
json set_against_continued_heating_mode(int sequence_id, bool enable);
json close_air_filter(int sequence_id);
json ams_user_setting(int sequence_id, bool startup_read_option, bool tray_read_option, bool calibrate_remain_flag);
// `action` is one of "resume", "reset", "pause", "done", "abort".
json ams_control(int sequence_id, const std::string& action);
json auto_stop_ams_dry(int sequence_id);
json ams_reset(int sequence_id);
json ams_filament_drying(int sequence_id,
                         int ams_id,
                         int mode,
                         const std::string& filament,
                         int temp,
                         int duration_hours,
                         bool rotate_tray,
                         int cooling_temp,
                         bool close_power_conflict);
} // namespace print

namespace camera {
json ipcam_record_set(int sequence_id, bool enable);
json ipcam_timelapse(int sequence_id, bool enable);
json ipcam_resolution_set(int sequence_id, const std::string& resolution);
json ipcam_cap_pic_set(int sequence_id, bool enable);
json timelapse_storage_check(int sequence_id, const std::string& storage, int total_layer);
json delete_oldest_timelapse(int sequence_id, const std::string& storage, int total_layer);
} // namespace camera

namespace xcam {
// `sensitivity` is left out when empty.
json xcam_control_set(int sequence_id, const std::string& module_name, bool on, const std::string& sensitivity = "");
} // namespace xcam

} // namespace Slic3r::opcp
