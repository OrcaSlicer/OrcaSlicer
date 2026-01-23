#ifndef __BBL_NETWORK_PLUGIN_HPP__
#define __BBL_NETWORK_PLUGIN_HPP__

#include "bambu_networking.hpp"
#include "libslic3r/ProjectTask.hpp"
#include <string>
#include <memory>
#include <vector>
#include <map>
#include <functional>
#include <mutex>

#if defined(_MSC_VER) || defined(_WIN32)
#include <Windows.h>
#endif

namespace Slic3r {

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
typedef int (*func_set_on_user_login_fn)(void *agent, OnUserLoginFn fn);
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
typedef int (*func_bind)(void *agent, std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn);
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
typedef std::string (*func_get_studio_info_url)(void *agent);
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

// Legacy function pointer types (for older DLL versions)
typedef int (*func_start_print_legacy)(void *agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_local_print_with_record_legacy)(void *agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_send_gcode_to_sdcard_legacy)(void *agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_local_print_legacy)(void *agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
typedef int (*func_start_sdcard_print_legacy)(void* agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
typedef int (*func_send_message_legacy)(void* agent, std::string dev_id, std::string json_str, int qos);
typedef int (*func_send_message_to_printer_legacy)(void* agent, std::string dev_id, std::string json_str, int qos);

/**
 * BBLNetworkPlugin - Singleton managing the Bambu Lab network DLL.
 *
 * Responsibilities:
 * - Owns the DLL module handle (netwoking_module)
 * - Owns the DLL source module handle (source_module)
 * - Manages the shared void* agent handle
 * - Provides all function pointers to BBL agents
 *
 * Usage:
 *   auto& plugin = BBLNetworkPlugin::instance();
 *   if (plugin.initialize(version)) {
 *       plugin.create_agent(log_dir);
 *       // Now BBLCloudServiceAgent/BBLPrinterAgent can use plugin
 *   }
 */
class BBLNetworkPlugin {
public:
    // Singleton access
    static BBLNetworkPlugin& instance();

    // Delete copy/move
    BBLNetworkPlugin(const BBLNetworkPlugin&) = delete;
    BBLNetworkPlugin& operator=(const BBLNetworkPlugin&) = delete;
    BBLNetworkPlugin(BBLNetworkPlugin&&) = delete;
    BBLNetworkPlugin& operator=(BBLNetworkPlugin&&) = delete;

    // ========================================================================
    // Module Lifecycle
    // ========================================================================

    /**
     * Load the network DLL from the plugins folder.
     * @param using_backup If true, look in plugins/backup folder
     * @param version Required version string (e.g., "01.09.05.01")
     * @return 0 on success, -1 on failure
     */
    int initialize(bool using_backup = false, const std::string& version = "");

    /**
     * Unload the network DLL and clear all function pointers.
     * @return 0 on success
     */
    int unload();

    /**
     * Destroy the singleton instance.
     * Safe to call multiple times - does nothing if already destroyed.
     * Must be called during application shutdown before main() returns.
     */
    static void shutdown();

    /**
     * Check if DLL is currently loaded.
     */
    bool is_loaded() const;

    /**
     * Get the plugin version string.
     */
    std::string get_version() const;

    // ========================================================================
    // Agent Lifecycle
    // ========================================================================

    /**
     * Create the shared agent handle.
     * Only one agent can exist at a time.
     * @param log_dir Directory for log files
     * @return The created agent handle, or nullptr on failure
     */
    void* create_agent(const std::string& log_dir);

    /**
     * Destroy the shared agent handle.
     * @return 0 on success
     */
    int destroy_agent();

    /**
     * Get the current agent handle.
     * Returns nullptr if no agent created.
     */
    void* get_agent() const { return m_agent; }

    /**
     * Check if an agent has been created.
     */
    bool has_agent() const { return m_agent != nullptr; }

    // ========================================================================
    // DLL Module Accessors
    // ========================================================================

#if defined(_MSC_VER) || defined(_WIN32)
    HMODULE get_networking_module() const { return m_networking_module; }
    HMODULE get_source_module();
#else
    void* get_networking_module() const { return m_networking_module; }
    void* get_source_module();
#endif

    void* get_function(const char* name);

    // Aliases for backward compatibility with NetworkAgent API
    void* get_network_function(const char* name) { return get_function(name); }
#if defined(_MSC_VER) || defined(_WIN32)
    HMODULE get_bambu_source_entry() { return get_source_module(); }
#else
    void* get_bambu_source_entry() { return get_source_module(); }
#endif

    // ========================================================================
    // Utility Methods
    // ========================================================================

    static std::string get_libpath_in_current_directory(const std::string& library_name);
    static std::string get_versioned_library_path(const std::string& version);
    static bool versioned_library_exists(const std::string& version);
    static bool legacy_library_exists();
    static void remove_legacy_library();
    static std::vector<std::string> scan_plugin_versions();

    // ========================================================================
    // Error Handling
    // ========================================================================

    NetworkLibraryLoadError get_load_error() const { return m_load_error; }
    void clear_load_error();
    void set_load_error(const std::string& message,
                        const std::string& technical_details,
                        const std::string& attempted_path);

    // ========================================================================
    // Legacy Network Flag
    // ========================================================================

    bool use_legacy_network() const { return m_use_legacy_network; }
    void set_use_legacy_network(bool legacy) { m_use_legacy_network = legacy; }

    // ========================================================================
    // Function Pointer Accessors
    // ========================================================================

    func_check_debug_consistent get_check_debug_consistent() const { return m_check_debug_consistent; }
    func_get_version get_get_version() const { return m_get_version; }
    func_create_agent get_create_agent() const { return m_create_agent; }
    func_destroy_agent get_destroy_agent() const { return m_destroy_agent; }
    func_init_log get_init_log() const { return m_init_log; }
    func_set_config_dir get_set_config_dir() const { return m_set_config_dir; }
    func_set_cert_file get_set_cert_file() const { return m_set_cert_file; }
    func_set_country_code get_set_country_code() const { return m_set_country_code; }
    func_start get_start() const { return m_start; }
    func_set_on_ssdp_msg_fn get_set_on_ssdp_msg_fn() const { return m_set_on_ssdp_msg_fn; }
    func_set_on_user_login_fn get_set_on_user_login_fn() const { return m_set_on_user_login_fn; }
    func_set_on_printer_connected_fn get_set_on_printer_connected_fn() const { return m_set_on_printer_connected_fn; }
    func_set_on_server_connected_fn get_set_on_server_connected_fn() const { return m_set_on_server_connected_fn; }
    func_set_on_http_error_fn get_set_on_http_error_fn() const { return m_set_on_http_error_fn; }
    func_set_get_country_code_fn get_set_get_country_code_fn() const { return m_set_get_country_code_fn; }
    func_set_on_subscribe_failure_fn get_set_on_subscribe_failure_fn() const { return m_set_on_subscribe_failure_fn; }
    func_set_on_message_fn get_set_on_message_fn() const { return m_set_on_message_fn; }
    func_set_on_user_message_fn get_set_on_user_message_fn() const { return m_set_on_user_message_fn; }
    func_set_on_local_connect_fn get_set_on_local_connect_fn() const { return m_set_on_local_connect_fn; }
    func_set_on_local_message_fn get_set_on_local_message_fn() const { return m_set_on_local_message_fn; }
    func_set_queue_on_main_fn get_set_queue_on_main_fn() const { return m_set_queue_on_main_fn; }
    func_connect_server get_connect_server() const { return m_connect_server; }
    func_is_server_connected get_is_server_connected() const { return m_is_server_connected; }
    func_refresh_connection get_refresh_connection() const { return m_refresh_connection; }
    func_start_subscribe get_start_subscribe() const { return m_start_subscribe; }
    func_stop_subscribe get_stop_subscribe() const { return m_stop_subscribe; }
    func_add_subscribe get_add_subscribe() const { return m_add_subscribe; }
    func_del_subscribe get_del_subscribe() const { return m_del_subscribe; }
    func_enable_multi_machine get_enable_multi_machine() const { return m_enable_multi_machine; }
    func_send_message get_send_message() const { return m_send_message; }
    func_connect_printer get_connect_printer() const { return m_connect_printer; }
    func_disconnect_printer get_disconnect_printer() const { return m_disconnect_printer; }
    func_send_message_to_printer get_send_message_to_printer() const { return m_send_message_to_printer; }
    func_check_cert get_check_cert() const { return m_check_cert; }
    func_install_device_cert get_install_device_cert() const { return m_install_device_cert; }
    func_start_discovery get_start_discovery() const { return m_start_discovery; }
    func_change_user get_change_user() const { return m_change_user; }
    func_is_user_login get_is_user_login() const { return m_is_user_login; }
    func_user_logout get_user_logout() const { return m_user_logout; }
    func_get_user_id get_get_user_id() const { return m_get_user_id; }
    func_get_user_name get_get_user_name() const { return m_get_user_name; }
    func_get_user_avatar get_get_user_avatar() const { return m_get_user_avatar; }
    func_get_user_nickanme get_get_user_nickanme() const { return m_get_user_nickanme; }
    func_build_login_cmd get_build_login_cmd() const { return m_build_login_cmd; }
    func_build_logout_cmd get_build_logout_cmd() const { return m_build_logout_cmd; }
    func_build_login_info get_build_login_info() const { return m_build_login_info; }
    func_ping_bind get_ping_bind() const { return m_ping_bind; }
    func_bind_detect get_bind_detect() const { return m_bind_detect; }
    func_set_server_callback get_set_server_callback() const { return m_set_server_callback; }
    func_bind get_bind() const { return m_bind; }
    func_unbind get_unbind() const { return m_unbind; }
    func_get_bambulab_host get_get_bambulab_host() const { return m_get_bambulab_host; }
    func_get_user_selected_machine get_get_user_selected_machine() const { return m_get_user_selected_machine; }
    func_set_user_selected_machine get_set_user_selected_machine() const { return m_set_user_selected_machine; }
    func_start_print get_start_print() const { return m_start_print; }
    func_start_local_print_with_record get_start_local_print_with_record() const { return m_start_local_print_with_record; }
    func_start_send_gcode_to_sdcard get_start_send_gcode_to_sdcard() const { return m_start_send_gcode_to_sdcard; }
    func_start_local_print get_start_local_print() const { return m_start_local_print; }
    func_start_sdcard_print get_start_sdcard_print() const { return m_start_sdcard_print; }
    func_get_user_presets get_get_user_presets() const { return m_get_user_presets; }
    func_request_setting_id get_request_setting_id() const { return m_request_setting_id; }
    func_put_setting get_put_setting() const { return m_put_setting; }
    func_get_setting_list get_get_setting_list() const { return m_get_setting_list; }
    func_get_setting_list2 get_get_setting_list2() const { return m_get_setting_list2; }
    func_delete_setting get_delete_setting() const { return m_delete_setting; }
    func_get_studio_info_url get_get_studio_info_url() const { return m_get_studio_info_url; }
    func_set_extra_http_header get_set_extra_http_header() const { return m_set_extra_http_header; }
    func_get_my_message get_get_my_message() const { return m_get_my_message; }
    func_check_user_task_report get_check_user_task_report() const { return m_check_user_task_report; }
    func_get_user_print_info get_get_user_print_info() const { return m_get_user_print_info; }
    func_get_user_tasks get_get_user_tasks() const { return m_get_user_tasks; }
    func_get_printer_firmware get_get_printer_firmware() const { return m_get_printer_firmware; }
    func_get_task_plate_index get_get_task_plate_index() const { return m_get_task_plate_index; }
    func_get_user_info get_get_user_info() const { return m_get_user_info; }
    func_request_bind_ticket get_request_bind_ticket() const { return m_request_bind_ticket; }
    func_get_subtask_info get_get_subtask_info() const { return m_get_subtask_info; }
    func_get_slice_info get_get_slice_info() const { return m_get_slice_info; }
    func_query_bind_status get_query_bind_status() const { return m_query_bind_status; }
    func_modify_printer_name get_modify_printer_name() const { return m_modify_printer_name; }
    func_get_camera_url get_get_camera_url() const { return m_get_camera_url; }
    func_get_design_staffpick get_get_design_staffpick() const { return m_get_design_staffpick; }
    func_start_pubilsh get_start_publish() const { return m_start_publish; }
    func_get_model_publish_url get_get_model_publish_url() const { return m_get_model_publish_url; }
    func_get_subtask get_get_subtask() const { return m_get_subtask; }
    func_get_model_mall_home_url get_get_model_mall_home_url() const { return m_get_model_mall_home_url; }
    func_get_model_mall_detail_url get_get_model_mall_detail_url() const { return m_get_model_mall_detail_url; }
    func_get_my_profile get_get_my_profile() const { return m_get_my_profile; }
    func_track_enable get_track_enable() const { return m_track_enable; }
    func_track_remove_files get_track_remove_files() const { return m_track_remove_files; }
    func_track_event get_track_event() const { return m_track_event; }
    func_track_header get_track_header() const { return m_track_header; }
    func_track_update_property get_track_update_property() const { return m_track_update_property; }
    func_track_get_property get_track_get_property() const { return m_track_get_property; }
    func_put_model_mall_rating_url get_put_model_mall_rating() const { return m_put_model_mall_rating; }
    func_get_oss_config get_get_oss_config() const { return m_get_oss_config; }
    func_put_rating_picture_oss get_put_rating_picture_oss() const { return m_put_rating_picture_oss; }
    func_get_model_mall_rating_result get_get_model_mall_rating_result() const { return m_get_model_mall_rating_result; }
    func_get_mw_user_preference get_get_mw_user_preference() const { return m_get_mw_user_preference; }
    func_get_mw_user_4ulist get_get_mw_user_4ulist() const { return m_get_mw_user_4ulist; }

    // ========================================================================
    // Legacy Helper
    // ========================================================================

    static PrintParams_Legacy as_legacy(PrintParams& param);

private:
    // Singleton instance pointer (heap-allocated for explicit lifetime control)
    static BBLNetworkPlugin* s_instance;

    BBLNetworkPlugin();
    ~BBLNetworkPlugin();

    void load_all_function_pointers();
    void clear_all_function_pointers();

    // Module handles
#if defined(_MSC_VER) || defined(_WIN32)
    HMODULE m_networking_module{nullptr};
    HMODULE m_source_module{nullptr};
#else
    void* m_networking_module{nullptr};
    void* m_source_module{nullptr};
#endif

    // Shared agent handle
    void* m_agent{nullptr};

    // Load error state
    NetworkLibraryLoadError m_load_error;

    // Legacy network compatibility flag
    bool m_use_legacy_network{true};

    // Function pointers
    func_check_debug_consistent m_check_debug_consistent{nullptr};
    func_get_version m_get_version{nullptr};
    func_create_agent m_create_agent{nullptr};
    func_destroy_agent m_destroy_agent{nullptr};
    func_init_log m_init_log{nullptr};
    func_set_config_dir m_set_config_dir{nullptr};
    func_set_cert_file m_set_cert_file{nullptr};
    func_set_country_code m_set_country_code{nullptr};
    func_start m_start{nullptr};
    func_set_on_ssdp_msg_fn m_set_on_ssdp_msg_fn{nullptr};
    func_set_on_user_login_fn m_set_on_user_login_fn{nullptr};
    func_set_on_printer_connected_fn m_set_on_printer_connected_fn{nullptr};
    func_set_on_server_connected_fn m_set_on_server_connected_fn{nullptr};
    func_set_on_http_error_fn m_set_on_http_error_fn{nullptr};
    func_set_get_country_code_fn m_set_get_country_code_fn{nullptr};
    func_set_on_subscribe_failure_fn m_set_on_subscribe_failure_fn{nullptr};
    func_set_on_message_fn m_set_on_message_fn{nullptr};
    func_set_on_user_message_fn m_set_on_user_message_fn{nullptr};
    func_set_on_local_connect_fn m_set_on_local_connect_fn{nullptr};
    func_set_on_local_message_fn m_set_on_local_message_fn{nullptr};
    func_set_queue_on_main_fn m_set_queue_on_main_fn{nullptr};
    func_connect_server m_connect_server{nullptr};
    func_is_server_connected m_is_server_connected{nullptr};
    func_refresh_connection m_refresh_connection{nullptr};
    func_start_subscribe m_start_subscribe{nullptr};
    func_stop_subscribe m_stop_subscribe{nullptr};
    func_add_subscribe m_add_subscribe{nullptr};
    func_del_subscribe m_del_subscribe{nullptr};
    func_enable_multi_machine m_enable_multi_machine{nullptr};
    func_send_message m_send_message{nullptr};
    func_connect_printer m_connect_printer{nullptr};
    func_disconnect_printer m_disconnect_printer{nullptr};
    func_send_message_to_printer m_send_message_to_printer{nullptr};
    func_check_cert m_check_cert{nullptr};
    func_install_device_cert m_install_device_cert{nullptr};
    func_start_discovery m_start_discovery{nullptr};
    func_change_user m_change_user{nullptr};
    func_is_user_login m_is_user_login{nullptr};
    func_user_logout m_user_logout{nullptr};
    func_get_user_id m_get_user_id{nullptr};
    func_get_user_name m_get_user_name{nullptr};
    func_get_user_avatar m_get_user_avatar{nullptr};
    func_get_user_nickanme m_get_user_nickanme{nullptr};
    func_build_login_cmd m_build_login_cmd{nullptr};
    func_build_logout_cmd m_build_logout_cmd{nullptr};
    func_build_login_info m_build_login_info{nullptr};
    func_ping_bind m_ping_bind{nullptr};
    func_bind_detect m_bind_detect{nullptr};
    func_set_server_callback m_set_server_callback{nullptr};
    func_bind m_bind{nullptr};
    func_unbind m_unbind{nullptr};
    func_get_bambulab_host m_get_bambulab_host{nullptr};
    func_get_user_selected_machine m_get_user_selected_machine{nullptr};
    func_set_user_selected_machine m_set_user_selected_machine{nullptr};
    func_start_print m_start_print{nullptr};
    func_start_local_print_with_record m_start_local_print_with_record{nullptr};
    func_start_send_gcode_to_sdcard m_start_send_gcode_to_sdcard{nullptr};
    func_start_local_print m_start_local_print{nullptr};
    func_start_sdcard_print m_start_sdcard_print{nullptr};
    func_get_user_presets m_get_user_presets{nullptr};
    func_request_setting_id m_request_setting_id{nullptr};
    func_put_setting m_put_setting{nullptr};
    func_get_setting_list m_get_setting_list{nullptr};
    func_get_setting_list2 m_get_setting_list2{nullptr};
    func_delete_setting m_delete_setting{nullptr};
    func_get_studio_info_url m_get_studio_info_url{nullptr};
    func_set_extra_http_header m_set_extra_http_header{nullptr};
    func_get_my_message m_get_my_message{nullptr};
    func_check_user_task_report m_check_user_task_report{nullptr};
    func_get_user_print_info m_get_user_print_info{nullptr};
    func_get_user_tasks m_get_user_tasks{nullptr};
    func_get_printer_firmware m_get_printer_firmware{nullptr};
    func_get_task_plate_index m_get_task_plate_index{nullptr};
    func_get_user_info m_get_user_info{nullptr};
    func_request_bind_ticket m_request_bind_ticket{nullptr};
    func_get_subtask_info m_get_subtask_info{nullptr};
    func_get_slice_info m_get_slice_info{nullptr};
    func_query_bind_status m_query_bind_status{nullptr};
    func_modify_printer_name m_modify_printer_name{nullptr};
    func_get_camera_url m_get_camera_url{nullptr};
    func_get_design_staffpick m_get_design_staffpick{nullptr};
    func_start_pubilsh m_start_publish{nullptr};
    func_get_model_publish_url m_get_model_publish_url{nullptr};
    func_get_subtask m_get_subtask{nullptr};
    func_get_model_mall_home_url m_get_model_mall_home_url{nullptr};
    func_get_model_mall_detail_url m_get_model_mall_detail_url{nullptr};
    func_get_my_profile m_get_my_profile{nullptr};
    func_track_enable m_track_enable{nullptr};
    func_track_remove_files m_track_remove_files{nullptr};
    func_track_event m_track_event{nullptr};
    func_track_header m_track_header{nullptr};
    func_track_update_property m_track_update_property{nullptr};
    func_track_get_property m_track_get_property{nullptr};
    func_put_model_mall_rating_url m_put_model_mall_rating{nullptr};
    func_get_oss_config m_get_oss_config{nullptr};
    func_put_rating_picture_oss m_put_rating_picture_oss{nullptr};
    func_get_model_mall_rating_result m_get_model_mall_rating_result{nullptr};
    func_get_mw_user_preference m_get_mw_user_preference{nullptr};
    func_get_mw_user_4ulist m_get_mw_user_4ulist{nullptr};
};

} // namespace Slic3r

#endif // __BBL_NETWORK_PLUGIN_HPP__
