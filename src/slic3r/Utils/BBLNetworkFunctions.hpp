#pragma once

#include "BambuNetworkTypes.hpp"

namespace Slic3r {

class BBLModelTask;
using OnGetSubTaskFn = std::function<void(BBLModelTask*)>;

// ============================================================================
// Function Pointer Types (copied from NetworkAgent.hpp)
// ============================================================================

typedef bool (*func_check_debug_consistent)(bool is_debug);
typedef std::string (*func_get_version)(void);
typedef void* (*func_create_agent)(std::string log_dir);
typedef int (*func_destroy_agent)(void *agent);
typedef int (*func_init_log)(void *agent);
typedef int (*func_set_config_dir)(void *agent, std::string config_dir);
typedef int (*func_set_cert_file)(void *agent, std::string folder, std::string filename);
typedef int (*func_set_country_code)(void *agent, std::string country_code);
typedef int (*func_start)(void *agent);
typedef int (*func_set_on_ssdp_msg_fn)(void *agent, OnMsgArrivedFn fn);
typedef int (*func_set_on_printer_connected_fn)(void *agent, OnPrinterConnectedFn fn);
typedef int (*func_set_on_server_connected_fn)(void *agent, OnServerConnectedFn fn);
typedef int (*func_set_on_http_error_fn)(void *agent, OnHttpErrorFn fn);
typedef int (*func_set_get_country_code_fn)(void *agent, GetCountryCodeFn fn);
typedef int (*func_set_on_subscribe_failure_fn)(void *agent, GetSubscribeFailureFn fn);
typedef int (*func_set_on_message_fn)(void *agent, OnMessageFn fn);
typedef int (*func_set_on_user_message_fn)(void *agent, OnMessageFn fn);
typedef int (*func_set_on_local_connect_fn)(void *agent, OnLocalConnectedFn fn);
typedef int (*func_set_on_local_message_fn)(void *agent, OnMessageFn fn);
typedef int (*func_set_queue_on_main_fn)(void *agent, QueueOnMainFn fn);
typedef int (*func_connect_server)(void *agent);
typedef bool (*func_is_server_connected)(void *agent);
typedef int (*func_refresh_connection)(void *agent);
typedef int (*func_start_subscribe)(void *agent, std::string module);
typedef int (*func_stop_subscribe)(void *agent, std::string module);
typedef int (*func_add_subscribe)(void *agent, std::vector<std::string> dev_list);
typedef int (*func_del_subscribe)(void *agent, std::vector<std::string> dev_list);
typedef void (*func_enable_multi_machine)(void *agent, bool enable);
typedef int (*func_send_message)(void *agent, std::string dev_id, std::string json_str, int qos, int flag);
typedef int (*func_connect_printer)(void *agent, std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl);
typedef int (*func_disconnect_printer)(void *agent);
typedef int (*func_send_message_to_printer)(void *agent, std::string dev_id, std::string json_str, int qos, int flag);
typedef int (*func_check_cert)(void* agent);
typedef void (*func_install_device_cert)(void* agent, std::string dev_id, bool lan_only);
typedef bool (*func_start_discovery)(void *agent, bool start, bool sending);
typedef int (*func_change_user)(void *agent, std::string user_info);
typedef bool (*func_is_user_login)(void *agent);
typedef int (*func_user_logout)(void *agent, bool request);
typedef std::string (*func_get_user_id)(void *agent);
typedef std::string (*func_get_user_name)(void *agent);
typedef std::string (*func_get_user_avatar)(void *agent);
typedef std::string (*func_get_user_nickanme)(void *agent);
typedef std::string (*func_build_login_cmd)(void *agent);
typedef std::string (*func_build_logout_cmd)(void *agent);
typedef std::string (*func_build_login_info)(void *agent);
typedef int (*func_ping_bind)(void *agent, std::string ping_code);
typedef int (*func_bind_detect)(void *agent, std::string dev_ip, std::string sec_link, detectResult& detect);
typedef int (*func_set_server_callback)(void *agent, OnServerErrFn fn);
typedef int (*func_bind)(void *agent, std::string dev_ip, std::string dev_id, std::string dev_model, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn);
typedef int (*func_unbind)(void *agent, std::string dev_id);
typedef std::string (*func_get_bambulab_host)(void *agent);
typedef std::string (*func_get_user_selected_machine)(void *agent);
typedef int (*func_set_user_selected_machine)(void *agent, std::string dev_id);
typedef int (*func_start_print)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_local_print_with_record)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_send_gcode_to_sdcard)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_local_print)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
typedef int (*func_start_sdcard_print)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
typedef int (*func_get_user_presets)(void *agent, std::map<std::string, std::map<std::string, std::string>>* user_presets);
typedef std::string (*func_request_setting_id)(void *agent, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code);
typedef int (*func_put_setting)(void *agent, std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code);
typedef int (*func_get_setting_list)(void *agent, std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn);
typedef int (*func_get_setting_list2)(void *agent, std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn);
typedef int (*func_delete_setting)(void *agent, std::string setting_id);
typedef int (*func_set_extra_http_header)(void *agent, std::map<std::string, std::string> extra_headers);
typedef int (*func_get_my_message)(void *agent, int type, int after, int limit, unsigned int* http_code, std::string* http_body);
typedef int (*func_check_user_task_report)(void *agent, int* task_id, bool* printable);
typedef int (*func_get_user_print_info)(void *agent, unsigned int* http_code, std::string* http_body);
typedef int (*func_get_user_tasks)(void *agent, TaskQueryParams params, std::string* http_body);
typedef int (*func_get_printer_firmware)(void *agent, std::string dev_id, unsigned* http_code, std::string* http_body);
typedef int (*func_get_task_plate_index)(void *agent, std::string task_id, int* plate_index);
typedef int (*func_get_user_info)(void *agent, int* identifier);
typedef int (*func_request_bind_ticket)(void *agent, std::string* ticket);
typedef int (*func_get_subtask_info)(void *agent, std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string *http_body);
typedef int (*func_get_slice_info)(void *agent, std::string project_id, std::string profile_id, int plate_index, std::string* slice_json);
typedef int (*func_query_bind_status)(void *agent, std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body);
typedef int (*func_modify_printer_name)(void *agent, std::string dev_id, std::string dev_name);
typedef int (*func_get_camera_url)(void *agent, std::string dev_id, std::function<void(std::string)> callback);
typedef int (*func_get_design_staffpick)(void *agent, int offset, int limit, std::function<void(std::string)> callback);
typedef int (*func_start_pubilsh)(void *agent, PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out);
typedef int (*func_get_model_publish_url)(void *agent, std::string* url);
typedef int (*func_get_subtask)(void *agent, BBLModelTask* task, OnGetSubTaskFn getsub_fn);
typedef int (*func_get_model_mall_home_url)(void *agent, std::string* url);
typedef int (*func_get_model_mall_detail_url)(void *agent, std::string* url, std::string id);
typedef int (*func_get_my_profile)(void *agent, std::string token, unsigned int *http_code, std::string *http_body);
typedef int (*func_get_my_token)(void *agent, std::string ticket, unsigned int *http_code, std::string *http_body);
typedef int (*func_track_enable)(void *agent, bool enable);
typedef int (*func_track_remove_files)(void *agent);
typedef int (*func_track_event)(void *agent, std::string evt_key, std::string content);
typedef int (*func_track_header)(void *agent, std::string header);
typedef int (*func_track_update_property)(void *agent, std::string name, std::string value, std::string type);
typedef int (*func_track_get_property)(void *agent, std::string name, std::string& value, std::string type);
typedef int (*func_put_model_mall_rating_url)(void *agent, int rating_id, int score, std::string content, std::vector<std::string> images, unsigned int &http_code, std::string &http_error);
typedef int (*func_get_oss_config)(void *agent, std::string &config, std::string country_code, unsigned int &http_code, std::string &http_error);
typedef int (*func_put_rating_picture_oss)(void *agent, std::string &config, std::string &pic_oss_path, std::string model_id, int profile_id, unsigned int &http_code, std::string &http_error);
typedef int (*func_get_model_mall_rating_result)(void *agent, int job_id, std::string &rating_result, unsigned int &http_code, std::string &http_error);
typedef int (*func_get_mw_user_preference)(void *agent, std::function<void(std::string)> callback);
typedef int (*func_get_mw_user_4ulist)(void *agent, int seed, int limit, std::function<void(std::string)> callback);

// Legacy function pointer types (for the 01.10.01 DLL)
typedef int (*func_start_print_legacy)(void *agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_local_print_with_record_legacy)(void *agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_send_gcode_to_sdcard_legacy)(void *agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_local_print_legacy)(void *agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
typedef int (*func_start_sdcard_print_legacy)(void* agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
typedef int (*func_send_message_legacy)(void* agent, std::string dev_id, std::string json_str, int qos);
typedef int (*func_send_message_to_printer_legacy)(void* agent, std::string dev_id, std::string json_str, int qos);

// 02.03.00 function pointer types. Only PrintParams differs from the current ABI; send_message
// and send_message_to_printer already take the flag argument in this series.
typedef int (*func_start_print_0203)(void *agent, PrintParams_0203 params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_local_print_with_record_0203)(void *agent, PrintParams_0203 params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_send_gcode_to_sdcard_0203)(void *agent, PrintParams_0203 params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_local_print_0203)(void *agent, PrintParams_0203 params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
typedef int (*func_start_sdcard_print_0203)(void* agent, PrintParams_0203 params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);

// bind() gained dev_model in 02.08.01; the legacy and 02.03.00 series share the older form.
typedef int (*func_bind_pre0208)(void *agent, std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn);

// Exported by every supported generation, but only bound here.
typedef int (*func_set_on_user_login_fn)(void *agent, OnUserLoginFn fn);
typedef std::string (*func_get_studio_info_url)(void *agent);

// Present since 02.03.00, null on the legacy plugin.
typedef int (*func_report_consent)(void *agent, std::string expand);

// Added by the 02.08.01.52 plugin ABI (null on older plugins).
typedef int (*func_get_camera_url_for_golive)(void *agent, std::string dev_id, std::string sdev_id, std::function<void(std::string)> callback);
typedef int (*func_get_hms_snapshot)(void *agent, std::string& dev_id, std::string& file_name, std::function<void(std::string, int)> callback);
typedef int (*func_get_filament_spools)(void *agent, FilamentQueryParams params, std::string* http_body);
typedef int (*func_create_filament_spool)(void *agent, std::string request_body, std::string* http_body);
typedef int (*func_update_filament_spool)(void *agent, std::string spool_id, std::string request_body, std::string* http_body);
typedef int (*func_delete_filament_spools)(void *agent, FilamentDeleteParams params, std::string* http_body);
typedef int (*func_get_filament_config)(void *agent, std::string* http_body);
typedef int (*func_sync_ams_filaments)(void *agent, AmsSyncParams params, std::string* http_body);

} // namespace Slic3r
