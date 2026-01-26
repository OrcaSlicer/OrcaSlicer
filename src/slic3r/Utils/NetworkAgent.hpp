#ifndef __NETWORK_Agent_HPP__
#define __NETWORK_Agent_HPP__

#include "bambu_networking.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "ICloudServiceAgent.hpp"
#include "IPrinterAgent.hpp"
#include <memory>


namespace Slic3r {

// Forward declaration
class BBLNetworkPlugin;

//the NetworkAgent class
class NetworkAgent
{

public:
    // Static utility methods - delegate to BBLNetworkPlugin
    static std::string get_libpath_in_current_directory(std::string library_name);
    static std::string get_versioned_library_path(const std::string& version);
    static bool versioned_library_exists(const std::string& version);
    static bool legacy_library_exists();
    static void remove_legacy_library();
    static std::vector<std::string> scan_plugin_versions();
    static int initialize_network_module(bool using_backup = false, const std::string& version = "");
    static int unload_network_module();
    static bool is_network_module_loaded();
#if defined(_MSC_VER) || defined(_WIN32)
    static HMODULE get_bambu_source_entry();
#else
    static void* get_bambu_source_entry();
#endif
    static std::string get_version();
    static void* get_network_function(const char* name);
    static bool use_legacy_network;

    static NetworkLibraryLoadError get_load_error();
    static void clear_load_error();
    static void set_load_error(const std::string& message, const std::string& technical_details, const std::string& attempted_path);

    // Traditional constructor (uses BBL DLL via singleton)
    NetworkAgent(std::string log_dir);

    // Sub-agent composition constructor (uses injected sub-agents)
    NetworkAgent(std::shared_ptr<ICloudServiceAgent> cloud_agent,
                 std::shared_ptr<IPrinterAgent> printer_agent);

    ~NetworkAgent();

    // Sub-agent accessors
    std::shared_ptr<ICloudServiceAgent> get_cloud_agent() const { return m_cloud_agent; }
    std::shared_ptr<IPrinterAgent> get_printer_agent() const { return m_printer_agent; }

    // Set the printer agent (for dynamic agent switching)
    void set_printer_agent(std::shared_ptr<IPrinterAgent> printer_agent);

    // Instance methods - delegate to sub-agents or BBLNetworkPlugin
    int init_log();
    int set_config_dir(std::string config_dir);
    int set_cert_file(std::string folder, std::string filename);
    int set_country_code(std::string country_code);
    int start();
    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn);
    int set_on_user_login_fn(OnUserLoginFn fn);
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn);
    int set_on_server_connected_fn(OnServerConnectedFn fn);
    int set_on_http_error_fn(OnHttpErrorFn fn);
    int set_get_country_code_fn(GetCountryCodeFn fn);
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn);
    int set_on_message_fn(OnMessageFn fn);
    int set_on_user_message_fn(OnMessageFn fn);
    int set_on_local_connect_fn(OnLocalConnectedFn fn);
    int set_on_local_message_fn(OnMessageFn fn);
    int set_queue_on_main_fn(QueueOnMainFn fn);
    int connect_server();
    bool is_server_connected();
    int refresh_connection();
    int start_subscribe(std::string module);
    int stop_subscribe(std::string module);
    int add_subscribe(std::vector<std::string> dev_list);
    int del_subscribe(std::vector<std::string> dev_list);
    void enable_multi_machine(bool enable);
    int send_message(std::string dev_id, std::string json_str, int qos, int flag);
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl);
    int disconnect_printer();
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag);
    int check_cert();
    void install_device_cert(std::string dev_id, bool lan_only);
    bool start_discovery(bool start, bool sending);
    int change_user(std::string user_info);
    bool is_user_login();
    int  user_logout(bool request = false);
    std::string get_user_id();
    std::string get_user_name();
    std::string get_user_avatar();
    std::string get_user_nickname();
    std::string build_login_cmd();
    std::string build_logout_cmd();
    std::string build_login_info();
    int ping_bind(std::string ping_code);
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect);
    int set_server_callback(OnServerErrFn fn);
    int bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn);
    int unbind(std::string dev_id);
    std::string get_cloud_service_host();
    std::string get_cloud_login_url(const std::string& language = "");
    std::string get_user_selected_machine();
    int set_user_selected_machine(std::string dev_id);
    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
    FilamentSyncMode get_filament_sync_mode() const;
    bool fetch_filament_info(std::string dev_id);
    int get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets);
    std::string request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code);
    int put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code);
    int get_setting_list(std::string bundle_version, ProgressFn pro_fn = nullptr, WasCancelledFn cancel_fn = nullptr);
    int get_setting_list2(std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn = nullptr, WasCancelledFn cancel_fn = nullptr);
    int delete_setting(std::string setting_id);
    std::string get_studio_info_url();
    int set_extra_http_header(std::map<std::string, std::string> extra_headers);
    int get_my_message(int type, int after, int limit, unsigned int* http_code, std::string* http_body);
    int check_user_task_report(int* task_id, bool* printable);
    int get_user_print_info(unsigned int* http_code, std::string* http_body);
    int get_user_tasks(TaskQueryParams params, std::string* http_body);
    int get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body);
    int get_task_plate_index(std::string task_id, int* plate_index);
    int get_user_info(int* identifier);
    int request_bind_ticket(std::string* ticket);
    int get_subtask_info(std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body);
    int get_slice_info(std::string project_id, std::string profile_id, int plate_index, std::string* slice_json);
    int query_bind_status(std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body);
    int modify_printer_name(std::string dev_id, std::string dev_name);
    int get_camera_url(std::string dev_id, std::function<void(std::string)> callback);
    int get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback);
    int start_publish(PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out);
    int get_model_publish_url(std::string* url);
    int get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn);
    int get_model_mall_home_url(std::string* url);
    int get_model_mall_detail_url(std::string* url, std::string id);
    int get_my_profile(std::string token, unsigned int* http_code, std::string* http_body);
    int track_enable(bool enable);
    int track_remove_files();
    int track_event(std::string evt_key, std::string content);
    int track_header(std::string header);
    int track_update_property(std::string name, std::string value, std::string type = "string");
    int track_get_property(std::string name, std::string& value, std::string type = "string");
    int put_model_mall_rating(int design_id, int score, std::string content, std::vector<std::string> images, unsigned int &http_code, std::string &http_error);
    int get_oss_config(std::string &config, std::string country_code, unsigned int &http_code, std::string &http_error);
    int put_rating_picture_oss(std::string &config, std::string &pic_oss_path, std::string model_id, int profile_id, unsigned int &http_code, std::string &http_error);
    int get_model_mall_rating_result(int job_id, std::string &rating_result, unsigned int &http_code, std::string &http_error);
    bool get_track_enable() { return enable_track; }

    int get_mw_user_preference(std::function<void(std::string)> callback);
    int get_mw_user_4ulist(int seed, int limit, std::function<void(std::string)> callback);

    // Get underlying agent handle from BBLNetworkPlugin
    void* get_network_agent();

private:
    struct PrinterCallbacks {
        OnMsgArrivedFn on_ssdp_msg_fn = nullptr;
        OnPrinterConnectedFn on_printer_connected_fn = nullptr;
        GetSubscribeFailureFn on_subscribe_failure_fn = nullptr;
        OnMessageFn on_message_fn = nullptr;
        OnMessageFn on_user_message_fn = nullptr;
        OnLocalConnectedFn on_local_connect_fn = nullptr;
        OnMessageFn on_local_message_fn = nullptr;
        QueueOnMainFn queue_on_main_fn = nullptr;
        OnServerErrFn on_server_err_fn = nullptr;
    };

    void apply_printer_callbacks(const std::shared_ptr<IPrinterAgent>& printer_agent,
                                 const PrinterCallbacks& callbacks);

    mutable std::mutex m_agent_mutex;  // Protect agent swapping
    PrinterCallbacks m_printer_callbacks;
    bool enable_track = false;

    // Sub-agent composition (for Orca/BBL mixed mode)
    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;
    std::shared_ptr<IPrinterAgent> m_printer_agent;
    std::string m_printer_agent_id;
};

}

#endif
