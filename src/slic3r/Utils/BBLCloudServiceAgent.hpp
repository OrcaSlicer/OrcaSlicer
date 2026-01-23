#ifndef __BBL_CLOUD_SERVICE_AGENT_HPP__
#define __BBL_CLOUD_SERVICE_AGENT_HPP__

#include "ICloudServiceAgent.hpp"
#include <string>
#include <memory>

namespace Slic3r {

/**
 * BBLCloudServiceAgent - BBL DLL wrapper implementation of ICloudServiceAgent.
 *
 * Delegates all cloud service and authentication operations to the proprietary
 * BBL network DLL through function pointers obtained from BBLNetworkPlugin singleton.
 * This class combines the functionality of the former BBLAuthAgent and BBLCloudServiceAgent.
 */
class BBLCloudServiceAgent : public ICloudServiceAgent {
public:
    BBLCloudServiceAgent();
    ~BBLCloudServiceAgent() override;

    // ========================================================================
    // ICloudServiceAgent Interface Implementation - Auth Methods
    // ========================================================================

    // Lifecycle
    int init_log() override;
    int set_config_dir(std::string config_dir) override;
    int set_cert_file(std::string folder, std::string filename) override;
    int set_country_code(std::string country_code) override;
    int start() override;

    // User Session Management
    int change_user(std::string user_info) override;
    bool is_user_login() override;
    int user_logout(bool request = false) override;
    std::string get_user_id() override;
    std::string get_user_name() override;
    std::string get_user_avatar() override;
    std::string get_user_nickname() override;

    // Login UI Support
    std::string build_login_cmd() override;
    std::string build_logout_cmd() override;
    std::string build_login_info() override;

    // Token Access (BBL manages tokens internally)
    std::string get_access_token() const override;
    std::string get_refresh_token() const override;
    bool ensure_token_fresh(const std::string& reason) override;

    // Auth Callbacks
    int set_on_user_login_fn(OnUserLoginFn fn) override;

    // ========================================================================
    // ICloudServiceAgent Interface Implementation - Cloud Methods
    // ========================================================================

    // Server Connectivity
    std::string get_cloud_service_host() override;
    std::string get_cloud_login_url(const std::string& language = "") override;
    int connect_server() override;
    bool is_server_connected() override;
    int refresh_connection() override;
    int start_subscribe(std::string module) override;
    int stop_subscribe(std::string module) override;
    int add_subscribe(std::vector<std::string> dev_list) override;
    int del_subscribe(std::vector<std::string> dev_list) override;
    void enable_multi_machine(bool enable) override;

    // Settings Synchronization
    int get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets) override;
    std::string request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) override;
    int put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) override;
    int get_setting_list(std::string bundle_version, ProgressFn pro_fn = nullptr, WasCancelledFn cancel_fn = nullptr) override;
    int get_setting_list2(std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn = nullptr, WasCancelledFn cancel_fn = nullptr) override;
    int delete_setting(std::string setting_id) override;

    // Cloud User Services
    int get_my_message(int type, int after, int limit, unsigned int* http_code, std::string* http_body) override;
    int check_user_task_report(int* task_id, bool* printable) override;
    int get_user_print_info(unsigned int* http_code, std::string* http_body) override;
    int get_user_tasks(TaskQueryParams params, std::string* http_body) override;
    int get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body) override;
    int get_task_plate_index(std::string task_id, int* plate_index) override;
    int get_user_info(int* identifier) override;
    int get_subtask_info(std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body) override;
    int get_slice_info(std::string project_id, std::string profile_id, int plate_index, std::string* slice_json) override;
    int query_bind_status(std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body) override;
    int modify_printer_name(std::string dev_id, std::string dev_name) override;

    // Model Mall & Publishing
    int get_camera_url(std::string dev_id, std::function<void(std::string)> callback) override;
    int get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback) override;
    int start_publish(PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out) override;
    int get_model_publish_url(std::string* url) override;
    int get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn) override;
    int get_model_mall_home_url(std::string* url) override;
    int get_model_mall_detail_url(std::string* url, std::string id) override;
    int get_my_profile(std::string token, unsigned int* http_code, std::string* http_body) override;

    // Analytics & Tracking
    int track_enable(bool enable) override;
    int track_remove_files() override;
    int track_event(std::string evt_key, std::string content) override;
    int track_header(std::string header) override;
    int track_update_property(std::string name, std::string value, std::string type = "string") override;
    int track_get_property(std::string name, std::string& value, std::string type = "string") override;
    bool get_track_enable() override;

    // Ratings & Reviews
    int put_model_mall_rating(int design_id, int score, std::string content, std::vector<std::string> images, unsigned int& http_code, std::string& http_error) override;
    int get_oss_config(std::string& config, std::string country_code, unsigned int& http_code, std::string& http_error) override;
    int put_rating_picture_oss(std::string& config, std::string& pic_oss_path, std::string model_id, int profile_id, unsigned int& http_code, std::string& http_error) override;
    int get_model_mall_rating_result(int job_id, std::string& rating_result, unsigned int& http_code, std::string& http_error) override;

    // Extra Features
    int set_extra_http_header(std::map<std::string, std::string> extra_headers) override;
    std::string get_studio_info_url() override;
    int get_mw_user_preference(std::function<void(std::string)> callback) override;
    int get_mw_user_4ulist(int seed, int limit, std::function<void(std::string)> callback) override;
    std::string get_version() override;

    // Cloud Callbacks
    int set_on_server_connected_fn(OnServerConnectedFn fn) override;
    int set_on_http_error_fn(OnHttpErrorFn fn) override;
    int set_get_country_code_fn(GetCountryCodeFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

private:
    bool m_enable_track{false};
};

} // namespace Slic3r

#endif // __BBL_CLOUD_SERVICE_AGENT_HPP__
