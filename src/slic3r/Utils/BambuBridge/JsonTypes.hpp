#pragma once

#include "../BambuNetworkTypes.hpp"
#include <nlohmann/json.hpp>

namespace Slic3r {

// A wire schema shared by both architectures. All fields are required in protocol v1.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(detectResult, result_msg, command, dev_id, model_id, dev_name, version, bind_state, connect_type)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PrintParams, dev_id, task_name, project_name, preset_name, filename, config_filename, plate_index, ftp_folder, ftp_file, ftp_file_md5, nozzle_mapping, ams_mapping, ams_mapping2, ams_mapping_info, nozzles_info, connection_type, comments, origin_profile_id, stl_design_id, origin_model_id, print_type, dst_file, dev_name, dev_ip, use_ssl_for_ftp, use_ssl_for_mqtt, username, password, task_bed_leveling, task_flow_cali, task_vibration_cali, task_layer_inspect, task_record_timelapse, task_timelapse_use_internal, task_use_ams, task_bed_type, extra_options, auto_bed_leveling, auto_flow_cali, auto_offset_cali, extruder_cali_manual_mode, task_ext_change_assist, try_emmc_print, svc_context, slicer_uid, queue_plate_id)







} // namespace Slic3r
