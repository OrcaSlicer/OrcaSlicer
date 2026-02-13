#include "BBLCloudServiceAgent.hpp"
#include "BBLNetworkPlugin.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r {

BBLCloudServiceAgent::BBLCloudServiceAgent() = default;

BBLCloudServiceAgent::~BBLCloudServiceAgent() = default;

// ============================================================================
// Lifecycle (merged from BBLAuthAgent)
// ============================================================================

int BBLCloudServiceAgent::init_log()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_init_log();
    if (func && agent) {
        return func(agent);
    }
    return -1;
}

int BBLCloudServiceAgent::set_config_dir(std::string config_dir)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_config_dir();
    if (func && agent) {
        return func(agent, config_dir);
    }
    return -1;
}

int BBLCloudServiceAgent::set_cert_file(std::string folder, std::string filename)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_cert_file();
    if (func && agent) {
        return func(agent, folder, filename);
    }
    return -1;
}

int BBLCloudServiceAgent::set_country_code(std::string country_code)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_country_code();
    if (func && agent) {
        return func(agent, country_code);
    }
    return -1;
}

int BBLCloudServiceAgent::start()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_start();
    if (func && agent) {
        return func(agent);
    }
    return -1;
}

// ============================================================================
// User Session Management (merged from BBLAuthAgent)
// ============================================================================

int BBLCloudServiceAgent::change_user(std::string user_info)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_change_user();
    if (func && agent) {
        return func(agent, user_info);
    }
    return -1;
}

bool BBLCloudServiceAgent::is_user_login()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_is_user_login();
    if (func && agent) {
        return func(agent);
    }
    return false;
}

int BBLCloudServiceAgent::user_logout(bool request)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_user_logout();
    if (func && agent) {
        return func(agent, request);
    }
    return -1;
}

std::string BBLCloudServiceAgent::get_user_id()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_user_id();
    if (func && agent) {
        return func(agent);
    }
    return "";
}

std::string BBLCloudServiceAgent::get_user_name()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_user_name();
    if (func && agent) {
        return func(agent);
    }
    return "";
}

std::string BBLCloudServiceAgent::get_user_avatar()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_user_avatar();
    if (func && agent) {
        return func(agent);
    }
    return "";
}

std::string BBLCloudServiceAgent::get_user_nickname()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_user_nickanme();
    if (func && agent) {
        return func(agent);
    }
    return "";
}

// ============================================================================
// Login UI Support (merged from BBLAuthAgent)
// ============================================================================

std::string BBLCloudServiceAgent::build_login_cmd()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_build_login_cmd();
    if (func && agent) {
        return func(agent);
    }
    return "";
}

std::string BBLCloudServiceAgent::build_logout_cmd()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_build_logout_cmd();
    if (func && agent) {
        return func(agent);
    }
    return "";
}

std::string BBLCloudServiceAgent::build_login_info()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_build_login_info();
    if (func && agent) {
        return func(agent);
    }
    return "";
}

// ============================================================================
// Token Access (merged from BBLAuthAgent)
// ============================================================================

std::string BBLCloudServiceAgent::get_access_token() const
{
    // BBL DLL manages tokens internally, not exposed via function pointer
    // Return empty string - BBL agents inject tokens automatically
    return "";
}

std::string BBLCloudServiceAgent::get_refresh_token() const
{
    // BBL DLL manages tokens internally, not exposed via function pointer
    return "";
}

bool BBLCloudServiceAgent::ensure_token_fresh(const std::string& reason)
{
    // BBL DLL handles token refresh internally
    // Always return true assuming the DLL manages this
    (void)reason;
    return true;
}

// ============================================================================
// Auth Callbacks (merged from BBLAuthAgent)
// ============================================================================

int BBLCloudServiceAgent::set_on_user_login_fn(OnUserLoginFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_user_login_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

// ============================================================================
// Server Connectivity
// ============================================================================

std::string BBLCloudServiceAgent::get_cloud_service_host()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_bambulab_host();
    if (func && agent) {
        return func(agent);
    }
    return "";
}

std::string BBLCloudServiceAgent::get_cloud_login_url(const std::string& language)
{
    std::string host_url = get_cloud_service_host();
    if (host_url.empty()) {
        return "";
    }

    if (language.empty()) {
        return host_url + "/sign-in";
    }
    return host_url + "/" + language + "/sign-in";
}

int BBLCloudServiceAgent::connect_server()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_connect_server();
    if (func && agent) {
        return func(agent);
    }
    return -1;
}

bool BBLCloudServiceAgent::is_server_connected()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_is_server_connected();
    if (func && agent) {
        return func(agent);
    }
    return false;
}

int BBLCloudServiceAgent::refresh_connection()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_refresh_connection();
    if (func && agent) {
        return func(agent);
    }
    return -1;
}

int BBLCloudServiceAgent::start_subscribe(std::string module)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_start_subscribe();
    if (func && agent) {
        return func(agent, module);
    }
    return -1;
}

int BBLCloudServiceAgent::stop_subscribe(std::string module)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_stop_subscribe();
    if (func && agent) {
        return func(agent, module);
    }
    return -1;
}

int BBLCloudServiceAgent::add_subscribe(std::vector<std::string> dev_list)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_add_subscribe();
    if (func && agent) {
        return func(agent, dev_list);
    }
    return -1;
}

int BBLCloudServiceAgent::del_subscribe(std::vector<std::string> dev_list)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_del_subscribe();
    if (func && agent) {
        return func(agent, dev_list);
    }
    return -1;
}

void BBLCloudServiceAgent::enable_multi_machine(bool enable)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_enable_multi_machine();
    if (func && agent) {
        func(agent, enable);
    }
}

// ============================================================================
// Settings Synchronization
// ============================================================================

int BBLCloudServiceAgent::get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_user_presets();
    if (func && agent) {
        return func(agent, user_presets);
    }
    return -1;
}

std::string BBLCloudServiceAgent::request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_request_setting_id();
    if (func && agent) {
        return func(agent, name, values_map, http_code);
    }
    return "";
}

int BBLCloudServiceAgent::put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_put_setting();
    if (func && agent) {
        return func(agent, setting_id, name, values_map, http_code);
    }
    return -1;
}

int BBLCloudServiceAgent::get_setting_list(std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_setting_list();
    if (func && agent) {
        return func(agent, bundle_version, pro_fn, cancel_fn);
    }
    return -1;
}

int BBLCloudServiceAgent::get_setting_list2(std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_setting_list2();
    if (func && agent) {
        return func(agent, bundle_version, chk_fn, pro_fn, cancel_fn);
    }
    return -1;
}

int BBLCloudServiceAgent::delete_setting(std::string setting_id)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_delete_setting();
    if (func && agent) {
        return func(agent, setting_id);
    }
    return -1;
}

// ============================================================================
// Cloud User Services
// ============================================================================

int BBLCloudServiceAgent::get_my_message(int type, int after, int limit, unsigned int* http_code, std::string* http_body)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_my_message();
    if (func && agent) {
        return func(agent, type, after, limit, http_code, http_body);
    }
    return -1;
}

int BBLCloudServiceAgent::check_user_task_report(int* task_id, bool* printable)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_check_user_task_report();
    if (func && agent) {
        return func(agent, task_id, printable);
    }
    return -1;
}

int BBLCloudServiceAgent::get_user_print_info(unsigned int* http_code, std::string* http_body)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_user_print_info();
    if (func && agent) {
        return func(agent, http_code, http_body);
    }
    return -1;
}

int BBLCloudServiceAgent::get_user_tasks(TaskQueryParams params, std::string* http_body)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_user_tasks();
    if (func && agent) {
        return func(agent, params, http_body);
    }
    return -1;
}

int BBLCloudServiceAgent::get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_printer_firmware();
    if (func && agent) {
        return func(agent, dev_id, http_code, http_body);
    }
    return -1;
}

int BBLCloudServiceAgent::get_task_plate_index(std::string task_id, int* plate_index)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_task_plate_index();
    if (func && agent) {
        return func(agent, task_id, plate_index);
    }
    return -1;
}

int BBLCloudServiceAgent::get_user_info(int* identifier)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_user_info();
    if (func && agent) {
        return func(agent, identifier);
    }
    return -1;
}

int BBLCloudServiceAgent::get_subtask_info(std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_subtask_info();
    if (func && agent) {
        return func(agent, subtask_id, task_json, http_code, http_body);
    }
    return -1;
}

int BBLCloudServiceAgent::get_slice_info(std::string project_id, std::string profile_id, int plate_index, std::string* slice_json)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_slice_info();
    if (func && agent) {
        return func(agent, project_id, profile_id, plate_index, slice_json);
    }
    return -1;
}

int BBLCloudServiceAgent::query_bind_status(std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_query_bind_status();
    if (func && agent) {
        return func(agent, query_list, http_code, http_body);
    }
    return -1;
}

int BBLCloudServiceAgent::modify_printer_name(std::string dev_id, std::string dev_name)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_modify_printer_name();
    if (func && agent) {
        return func(agent, dev_id, dev_name);
    }
    return -1;
}

// ============================================================================
// Model Mall & Publishing
// ============================================================================

int BBLCloudServiceAgent::get_camera_url(std::string dev_id, std::function<void(std::string)> callback)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_camera_url();
    if (func && agent) {
        return func(agent, dev_id, callback);
    }
    return -1;
}

int BBLCloudServiceAgent::get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_design_staffpick();
    if (func && agent) {
        return func(agent, offset, limit, callback);
    }
    return -1;
}

int BBLCloudServiceAgent::start_publish(PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_start_publish();
    if (func && agent) {
        return func(agent, params, update_fn, cancel_fn, out);
    }
    return -1;
}

int BBLCloudServiceAgent::get_model_publish_url(std::string* url)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_model_publish_url();
    if (func && agent) {
        return func(agent, url);
    }
    return -1;
}

int BBLCloudServiceAgent::get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_subtask();
    if (func && agent) {
        return func(agent, task, getsub_fn);
    }
    return -1;
}

int BBLCloudServiceAgent::get_model_mall_home_url(std::string* url)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_model_mall_home_url();
    if (func && agent) {
        return func(agent, url);
    }
    return -1;
}

int BBLCloudServiceAgent::get_model_mall_detail_url(std::string* url, std::string id)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_model_mall_detail_url();
    if (func && agent) {
        return func(agent, url, id);
    }
    return -1;
}

int BBLCloudServiceAgent::get_my_profile(std::string token, unsigned int* http_code, std::string* http_body)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_my_profile();
    if (func && agent) {
        return func(agent, token, http_code, http_body);
    }
    return -1;
}

// ============================================================================
// Analytics & Tracking
// ============================================================================

int BBLCloudServiceAgent::track_enable(bool enable)
{
    m_enable_track = enable;
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_track_enable();
    if (func && agent) {
        return func(agent, enable);
    }
    return -1;
}

int BBLCloudServiceAgent::track_remove_files()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_track_remove_files();
    if (func && agent) {
        return func(agent);
    }
    return -1;
}

int BBLCloudServiceAgent::track_event(std::string evt_key, std::string content)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_track_event();
    if (func && agent) {
        return func(agent, evt_key, content);
    }
    return -1;
}

int BBLCloudServiceAgent::track_header(std::string header)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_track_header();
    if (func && agent) {
        return func(agent, header);
    }
    return -1;
}

int BBLCloudServiceAgent::track_update_property(std::string name, std::string value, std::string type)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_track_update_property();
    if (func && agent) {
        return func(agent, name, value, type);
    }
    return -1;
}

int BBLCloudServiceAgent::track_get_property(std::string name, std::string& value, std::string type)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_track_get_property();
    if (func && agent) {
        return func(agent, name, value, type);
    }
    return -1;
}

bool BBLCloudServiceAgent::get_track_enable()
{
    return m_enable_track;
}

// ============================================================================
// Ratings & Reviews
// ============================================================================

int BBLCloudServiceAgent::put_model_mall_rating(int design_id, int score, std::string content, std::vector<std::string> images, unsigned int& http_code, std::string& http_error)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_put_model_mall_rating();
    if (func && agent) {
        return func(agent, design_id, score, content, images, http_code, http_error);
    }
    return -1;
}

int BBLCloudServiceAgent::get_oss_config(std::string& config, std::string country_code, unsigned int& http_code, std::string& http_error)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_oss_config();
    if (func && agent) {
        return func(agent, config, country_code, http_code, http_error);
    }
    return -1;
}

int BBLCloudServiceAgent::put_rating_picture_oss(std::string& config, std::string& pic_oss_path, std::string model_id, int profile_id, unsigned int& http_code, std::string& http_error)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_put_rating_picture_oss();
    if (func && agent) {
        return func(agent, config, pic_oss_path, model_id, profile_id, http_code, http_error);
    }
    return -1;
}

int BBLCloudServiceAgent::get_model_mall_rating_result(int job_id, std::string& rating_result, unsigned int& http_code, std::string& http_error)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_model_mall_rating_result();
    if (func && agent) {
        return func(agent, job_id, rating_result, http_code, http_error);
    }
    return -1;
}

// ============================================================================
// Extra Features
// ============================================================================

int BBLCloudServiceAgent::set_extra_http_header(std::map<std::string, std::string> extra_headers)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_extra_http_header();
    if (func && agent) {
        return func(agent, extra_headers);
    }
    return -1;
}

std::string BBLCloudServiceAgent::get_studio_info_url()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_studio_info_url();
    if (func && agent) {
        return func(agent);
    }
    return "";
}

int BBLCloudServiceAgent::get_mw_user_preference(std::function<void(std::string)> callback)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_mw_user_preference();
    if (func && agent) {
        return func(agent, callback);
    }
    return -1;
}

int BBLCloudServiceAgent::get_mw_user_4ulist(int seed, int limit, std::function<void(std::string)> callback)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_mw_user_4ulist();
    if (func && agent) {
        return func(agent, seed, limit, callback);
    }
    return -1;
}

std::string BBLCloudServiceAgent::get_version()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto func = plugin.get_get_version();
    if (func) {
        return func();
    }
    return "";
}

// ============================================================================
// Cloud Callbacks
// ============================================================================

int BBLCloudServiceAgent::set_on_server_connected_fn(OnServerConnectedFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_server_connected_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLCloudServiceAgent::set_on_http_error_fn(OnHttpErrorFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_http_error_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLCloudServiceAgent::set_get_country_code_fn(GetCountryCodeFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_get_country_code_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLCloudServiceAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_queue_on_main_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

} // namespace Slic3r
