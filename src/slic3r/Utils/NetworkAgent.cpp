#include <stdio.h>
#include <stdlib.h>
#include <set>
#include <algorithm>

#include <boost/log/trivial.hpp>
#include "libslic3r/Utils.hpp"
#include "NetworkAgent.hpp"
#include "BBLNetworkPlugin.hpp"
#include "BBLCloudServiceAgent.hpp"
#include "BBLPrinterAgent.hpp"

namespace Slic3r {

bool NetworkAgent::use_legacy_network = true;

// ============================================================================
// Static methods - delegate to BBLNetworkPlugin
// ============================================================================

std::string NetworkAgent::get_libpath_in_current_directory(std::string library_name)
{
    return BBLNetworkPlugin::get_libpath_in_current_directory(library_name);
}

std::string NetworkAgent::get_versioned_library_path(const std::string& version)
{
    return BBLNetworkPlugin::get_versioned_library_path(version);
}

bool NetworkAgent::versioned_library_exists(const std::string& version)
{
    return BBLNetworkPlugin::versioned_library_exists(version);
}

bool NetworkAgent::legacy_library_exists()
{
    return BBLNetworkPlugin::legacy_library_exists();
}

void NetworkAgent::remove_legacy_library()
{
    BBLNetworkPlugin::remove_legacy_library();
}

std::vector<std::string> NetworkAgent::scan_plugin_versions()
{
    return BBLNetworkPlugin::scan_plugin_versions();
}

int NetworkAgent::initialize_network_module(bool using_backup, const std::string& version)
{
    return BBLNetworkPlugin::instance().initialize(using_backup, version);
}

int NetworkAgent::unload_network_module()
{
    return BBLNetworkPlugin::instance().unload();
}

bool NetworkAgent::is_network_module_loaded()
{
    return BBLNetworkPlugin::instance().is_loaded();
}

#if defined(_MSC_VER) || defined(_WIN32)
HMODULE NetworkAgent::get_bambu_source_entry()
{
    return BBLNetworkPlugin::instance().get_bambu_source_entry();
}
#else
void* NetworkAgent::get_bambu_source_entry()
{
    return BBLNetworkPlugin::instance().get_bambu_source_entry();
}
#endif

std::string NetworkAgent::get_version()
{
    return BBLNetworkPlugin::instance().get_version();
}

void* NetworkAgent::get_network_function(const char* name)
{
    return BBLNetworkPlugin::instance().get_network_function(name);
}

NetworkLibraryLoadError NetworkAgent::get_load_error()
{
    return BBLNetworkPlugin::instance().get_load_error();
}

void NetworkAgent::clear_load_error()
{
    BBLNetworkPlugin::instance().clear_load_error();
}

void NetworkAgent::set_load_error(const std::string& message, const std::string& technical_details, const std::string& attempted_path)
{
    BBLNetworkPlugin::instance().set_load_error(message, technical_details, attempted_path);
}

// ============================================================================
// Constructors
// ============================================================================

NetworkAgent::NetworkAgent(std::string log_dir)
{
    auto& plugin = BBLNetworkPlugin::instance();

    if (plugin.is_loaded()) {
        // Create agent if not already created
        if (!plugin.has_agent()) {
            plugin.create_agent(log_dir);
        }

        m_cloud_agent = std::make_shared<BBLCloudServiceAgent>();
        m_printer_agent = std::make_shared<BBLPrinterAgent>();
        m_printer_agent->set_cloud_agent(m_cloud_agent);
        m_printer_agent_id = m_printer_agent->get_agent_info().id;
    }
}

NetworkAgent::NetworkAgent(std::shared_ptr<ICloudServiceAgent> cloud_agent,
                           std::shared_ptr<IPrinterAgent> printer_agent)
    : m_cloud_agent(std::move(cloud_agent))
    , m_printer_agent(std::move(printer_agent))
{
}

NetworkAgent::~NetworkAgent()
{
    // Note: We don't destroy the agent here anymore since it's managed by BBLNetworkPlugin singleton
    // The singleton manages the agent lifecycle
}

void NetworkAgent::set_printer_agent(std::shared_ptr<IPrinterAgent> printer_agent)
{
    // Local copies to allow safe access after releasing the lock.
    // This pattern ensures the objects stay alive (via shared_ptr refcount) even if
    // another thread modifies m_printer_agent or m_printer_callbacks after we unlock.
    std::shared_ptr<IPrinterAgent> old_printer_agent;
    std::shared_ptr<IPrinterAgent> new_printer_agent;
    PrinterCallbacks callbacks;

    {
        // Critical section: protect access to shared state
        std::lock_guard<std::mutex> lock(m_agent_mutex);

        if (!printer_agent) {
            return;
        }

        // Disconnect all callbacks from the old agent
        apply_printer_callbacks(m_printer_agent, callbacks);
        // Capture the old agent before overwriting so we can disconnect it outside the lock
        old_printer_agent = m_printer_agent;
        // Take ownership of the incoming agent and update the agent ID
        m_printer_agent = std::move(printer_agent);
        m_printer_agent_id = m_printer_agent->get_agent_info().id;

        // Create local shared_ptr copies - this increments the reference count,
        // guaranteeing the agent object stays alive even if m_printer_agent
        // is modified by another thread after we unlock
        new_printer_agent = m_printer_agent;
        callbacks = m_printer_callbacks;
    }
    // Lock released here - m_agent_mutex is now free for other threads

    // Disconnect the old agent's connections/threads. The cache keeps it alive,
    // but we release its network resources while it's not the active agent.
    if (old_printer_agent && old_printer_agent != new_printer_agent)
        old_printer_agent->disconnect_printer();

    // Apply callbacks OUTSIDE the lock to avoid deadlock risk and minimize
    // critical section duration. The local shared_ptr copy ensures the agent
    // cannot be destroyed while we're using it.
    apply_printer_callbacks(new_printer_agent, callbacks);
}

void* NetworkAgent::get_network_agent()
{
    return BBLNetworkPlugin::instance().get_agent();
}

void NetworkAgent::apply_printer_callbacks(const std::shared_ptr<IPrinterAgent>& printer_agent,
                                           const PrinterCallbacks& callbacks)
{
    if (!printer_agent) {
        return;
    }

    printer_agent->set_on_ssdp_msg_fn(callbacks.on_ssdp_msg_fn);
    printer_agent->set_on_printer_connected_fn(callbacks.on_printer_connected_fn);
    printer_agent->set_on_subscribe_failure_fn(callbacks.on_subscribe_failure_fn);
    printer_agent->set_on_message_fn(callbacks.on_message_fn);
    printer_agent->set_on_user_message_fn(callbacks.on_user_message_fn);
    printer_agent->set_on_local_connect_fn(callbacks.on_local_connect_fn);
    printer_agent->set_on_local_message_fn(callbacks.on_local_message_fn);
    printer_agent->set_queue_on_main_fn(callbacks.queue_on_main_fn);
    printer_agent->set_server_callback(callbacks.on_server_err_fn);
}

// ============================================================================
// Instance methods - delegate to sub-agents
// ============================================================================

int NetworkAgent::init_log()
{
    if (m_cloud_agent) return m_cloud_agent->init_log();
    return -1;
}

int NetworkAgent::set_config_dir(std::string config_dir)
{
    if (m_cloud_agent) return m_cloud_agent->set_config_dir(config_dir);
    return -1;
}

int NetworkAgent::set_cert_file(std::string folder, std::string filename)
{
    if (m_cloud_agent) return m_cloud_agent->set_cert_file(folder, filename);
    return -1;
}

int NetworkAgent::set_country_code(std::string country_code)
{
    if (m_cloud_agent) return m_cloud_agent->set_country_code(country_code);
    return -1;
}

int NetworkAgent::start()
{
    if (m_cloud_agent) return m_cloud_agent->start();
    return -1;
}

int NetworkAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    std::shared_ptr<IPrinterAgent> printer_agent;
    {
        std::lock_guard<std::mutex> lock(m_agent_mutex);
        m_printer_callbacks.on_ssdp_msg_fn = fn;
        printer_agent = m_printer_agent;
    }
    if (printer_agent) return printer_agent->set_on_ssdp_msg_fn(fn);
    return -1;
}

int NetworkAgent::set_on_user_login_fn(OnUserLoginFn fn)
{
    if (m_cloud_agent) return m_cloud_agent->set_on_user_login_fn(fn);
    return -1;
}

int NetworkAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    std::shared_ptr<IPrinterAgent> printer_agent;
    {
        std::lock_guard<std::mutex> lock(m_agent_mutex);
        m_printer_callbacks.on_printer_connected_fn = fn;
        printer_agent = m_printer_agent;
    }
    if (printer_agent) return printer_agent->set_on_printer_connected_fn(fn);
    return -1;
}

int NetworkAgent::set_on_server_connected_fn(OnServerConnectedFn fn)
{
    if (m_cloud_agent) return m_cloud_agent->set_on_server_connected_fn(fn);
    return -1;
}

int NetworkAgent::set_on_http_error_fn(OnHttpErrorFn fn)
{
    if (m_cloud_agent) return m_cloud_agent->set_on_http_error_fn(fn);
    return -1;
}

int NetworkAgent::set_get_country_code_fn(GetCountryCodeFn fn)
{
    if (m_cloud_agent) return m_cloud_agent->set_get_country_code_fn(fn);
    return -1;
}

int NetworkAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    std::shared_ptr<IPrinterAgent> printer_agent;
    {
        std::lock_guard<std::mutex> lock(m_agent_mutex);
        m_printer_callbacks.on_subscribe_failure_fn = fn;
        printer_agent = m_printer_agent;
    }
    if (printer_agent) return printer_agent->set_on_subscribe_failure_fn(fn);
    return -1;
}

int NetworkAgent::set_on_message_fn(OnMessageFn fn)
{
    std::shared_ptr<IPrinterAgent> printer_agent;
    {
        std::lock_guard<std::mutex> lock(m_agent_mutex);
        m_printer_callbacks.on_message_fn = fn;
        printer_agent = m_printer_agent;
    }
    if (printer_agent) return printer_agent->set_on_message_fn(fn);
    return -1;
}

int NetworkAgent::set_on_user_message_fn(OnMessageFn fn)
{
    std::shared_ptr<IPrinterAgent> printer_agent;
    {
        std::lock_guard<std::mutex> lock(m_agent_mutex);
        m_printer_callbacks.on_user_message_fn = fn;
        printer_agent = m_printer_agent;
    }
    if (printer_agent) return printer_agent->set_on_user_message_fn(fn);
    return -1;
}

int NetworkAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::shared_ptr<IPrinterAgent> printer_agent;
    {
        std::lock_guard<std::mutex> lock(m_agent_mutex);
        m_printer_callbacks.on_local_connect_fn = fn;
        printer_agent = m_printer_agent;
    }
    if (printer_agent) return printer_agent->set_on_local_connect_fn(fn);
    return -1;
}

int NetworkAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::shared_ptr<IPrinterAgent> printer_agent;
    {
        std::lock_guard<std::mutex> lock(m_agent_mutex);
        m_printer_callbacks.on_local_message_fn = fn;
        printer_agent = m_printer_agent;
    }
    if (printer_agent) return printer_agent->set_on_local_message_fn(fn);
    return -1;
}

int NetworkAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    // Set on both agents
    std::shared_ptr<ICloudServiceAgent> cloud_agent;
    std::shared_ptr<IPrinterAgent> printer_agent;
    {
        std::lock_guard<std::mutex> lock(m_agent_mutex);
        m_printer_callbacks.queue_on_main_fn = fn;
        cloud_agent = m_cloud_agent;
        printer_agent = m_printer_agent;
    }

    int ret = 0;
    if (cloud_agent) ret = cloud_agent->set_queue_on_main_fn(fn);
    if (printer_agent) printer_agent->set_queue_on_main_fn(fn);
    return ret;
}

int NetworkAgent::connect_server()
{
    if (m_cloud_agent) return m_cloud_agent->connect_server();
    return -1;
}

bool NetworkAgent::is_server_connected()
{
    if (m_cloud_agent) return m_cloud_agent->is_server_connected();
    return false;
}

int NetworkAgent::refresh_connection()
{
    if (m_cloud_agent) return m_cloud_agent->refresh_connection();
    return -1;
}

int NetworkAgent::start_subscribe(std::string module)
{
    if (m_cloud_agent) return m_cloud_agent->start_subscribe(module);
    return -1;
}

int NetworkAgent::stop_subscribe(std::string module)
{
    if (m_cloud_agent) return m_cloud_agent->stop_subscribe(module);
    return -1;
}

int NetworkAgent::add_subscribe(std::vector<std::string> dev_list)
{
    if (m_cloud_agent) return m_cloud_agent->add_subscribe(dev_list);
    return -1;
}

int NetworkAgent::del_subscribe(std::vector<std::string> dev_list)
{
    if (m_cloud_agent) return m_cloud_agent->del_subscribe(dev_list);
    return -1;
}

void NetworkAgent::enable_multi_machine(bool enable)
{
    if (m_cloud_agent) m_cloud_agent->enable_multi_machine(enable);
}

int NetworkAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    if (m_printer_agent) return m_printer_agent->send_message(dev_id, json_str, qos, flag);
    return -1;
}

int NetworkAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    if (m_printer_agent) return m_printer_agent->connect_printer(dev_id, dev_ip, username, password, use_ssl);
    return -1;
}

int NetworkAgent::disconnect_printer()
{
    if (m_printer_agent) return m_printer_agent->disconnect_printer();
    return -1;
}

int NetworkAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    if (m_printer_agent) return m_printer_agent->send_message_to_printer(dev_id, json_str, qos, flag);
    return -1;
}

int NetworkAgent::check_cert()
{
    if (m_printer_agent) return m_printer_agent->check_cert();
    return -1;
}

void NetworkAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    if (m_printer_agent) m_printer_agent->install_device_cert(dev_id, lan_only);
}

bool NetworkAgent::start_discovery(bool start, bool sending)
{
    if (m_printer_agent) return m_printer_agent->start_discovery(start, sending);
    return false;
}

int NetworkAgent::change_user(std::string user_info)
{
    if (m_cloud_agent) return m_cloud_agent->change_user(user_info);
    return -1;
}

bool NetworkAgent::is_user_login()
{
    if (m_cloud_agent) return m_cloud_agent->is_user_login();
    return false;
}

int NetworkAgent::user_logout(bool request)
{
    if (m_cloud_agent) return m_cloud_agent->user_logout(request);
    return -1;
}

std::string NetworkAgent::get_user_id()
{
    if (m_cloud_agent) return m_cloud_agent->get_user_id();
    return "";
}

std::string NetworkAgent::get_user_name()
{
    if (m_cloud_agent) return m_cloud_agent->get_user_name();
    return "";
}

std::string NetworkAgent::get_user_avatar()
{
    if (m_cloud_agent) return m_cloud_agent->get_user_avatar();
    return "";
}

std::string NetworkAgent::get_user_nickname()
{
    if (m_cloud_agent) return m_cloud_agent->get_user_nickname();
    return "";
}

std::string NetworkAgent::build_login_cmd()
{
    if (m_cloud_agent) return m_cloud_agent->build_login_cmd();
    return "";
}

std::string NetworkAgent::build_logout_cmd()
{
    if (m_cloud_agent) return m_cloud_agent->build_logout_cmd();
    return "";
}

std::string NetworkAgent::build_login_info()
{
    if (m_cloud_agent) return m_cloud_agent->build_login_info();
    return "";
}

int NetworkAgent::ping_bind(std::string ping_code)
{
    if (m_printer_agent) return m_printer_agent->ping_bind(ping_code);
    return -1;
}

int NetworkAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    if (m_printer_agent) return m_printer_agent->bind_detect(dev_ip, sec_link, detect);
    return -1;
}

int NetworkAgent::set_server_callback(OnServerErrFn fn)
{
    std::shared_ptr<IPrinterAgent> printer_agent;
    {
        std::lock_guard<std::mutex> lock(m_agent_mutex);
        m_printer_callbacks.on_server_err_fn = fn;
        printer_agent = m_printer_agent;
    }
    if (printer_agent) return printer_agent->set_server_callback(fn);
    return -1;
}

int NetworkAgent::bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn)
{
    if (m_printer_agent) return m_printer_agent->bind(dev_ip, dev_id, sec_link, timezone, improved, update_fn);
    return -1;
}

int NetworkAgent::unbind(std::string dev_id)
{
    if (m_printer_agent) return m_printer_agent->unbind(dev_id);
    return -1;
}

std::string NetworkAgent::get_cloud_service_host()
{
    if (m_cloud_agent) return m_cloud_agent->get_cloud_service_host();
    return "";
}

std::string NetworkAgent::get_cloud_login_url(const std::string& language)
{
    if (m_cloud_agent) return m_cloud_agent->get_cloud_login_url(language);
    return "";
}

std::string NetworkAgent::get_user_selected_machine()
{
    if (m_printer_agent) return m_printer_agent->get_user_selected_machine();
    return "";
}

int NetworkAgent::set_user_selected_machine(std::string dev_id)
{
    if (m_printer_agent) return m_printer_agent->set_user_selected_machine(dev_id);
    return -1;
}

int NetworkAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    if (m_printer_agent) return m_printer_agent->start_print(params, update_fn, cancel_fn, wait_fn);
    return -1;
}

int NetworkAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    if (m_printer_agent) return m_printer_agent->start_local_print_with_record(params, update_fn, cancel_fn, wait_fn);
    return -1;
}

int NetworkAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    if (m_printer_agent) return m_printer_agent->start_send_gcode_to_sdcard(params, update_fn, cancel_fn, wait_fn);
    return -1;
}

int NetworkAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (m_printer_agent) return m_printer_agent->start_local_print(params, update_fn, cancel_fn);
    return -1;
}

int NetworkAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (m_printer_agent) return m_printer_agent->start_sdcard_print(params, update_fn, cancel_fn);
    return -1;
}

FilamentSyncMode NetworkAgent::get_filament_sync_mode() const
{
    if (m_printer_agent) return m_printer_agent->get_filament_sync_mode();
    return FilamentSyncMode::none;  // Default when no agent
}

bool NetworkAgent::fetch_filament_info(std::string dev_id)
{
    if (m_printer_agent) {
        return m_printer_agent->fetch_filament_info(dev_id);
    }
    return false;
}

int NetworkAgent::get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets)
{
    if (m_cloud_agent) return m_cloud_agent->get_user_presets(user_presets);
    return -1;
}

std::string NetworkAgent::request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    if (m_cloud_agent) return m_cloud_agent->request_setting_id(name, values_map, http_code);
    return "";
}

int NetworkAgent::put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    if (m_cloud_agent) return m_cloud_agent->put_setting(setting_id, name, values_map, http_code);
    return -1;
}

int NetworkAgent::get_setting_list(std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    if (m_cloud_agent) return m_cloud_agent->get_setting_list(bundle_version, pro_fn, cancel_fn);
    return -1;
}

int NetworkAgent::get_setting_list2(std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    if (m_cloud_agent) return m_cloud_agent->get_setting_list2(bundle_version, chk_fn, pro_fn, cancel_fn);
    return -1;
}

int NetworkAgent::delete_setting(std::string setting_id)
{
    if (m_cloud_agent) return m_cloud_agent->delete_setting(setting_id);
    return -1;
}

std::string NetworkAgent::get_studio_info_url()
{
    if (m_cloud_agent) return m_cloud_agent->get_studio_info_url();
    return "";
}

int NetworkAgent::set_extra_http_header(std::map<std::string, std::string> extra_headers)
{
    if (m_cloud_agent) return m_cloud_agent->set_extra_http_header(extra_headers);
    return -1;
}

int NetworkAgent::get_my_message(int type, int after, int limit, unsigned int* http_code, std::string* http_body)
{
    if (m_cloud_agent) return m_cloud_agent->get_my_message(type, after, limit, http_code, http_body);
    return -1;
}

int NetworkAgent::check_user_task_report(int* task_id, bool* printable)
{
    if (m_cloud_agent) return m_cloud_agent->check_user_task_report(task_id, printable);
    return -1;
}

int NetworkAgent::get_user_print_info(unsigned int* http_code, std::string* http_body)
{
    if (m_cloud_agent) return m_cloud_agent->get_user_print_info(http_code, http_body);
    return -1;
}

int NetworkAgent::get_user_tasks(TaskQueryParams params, std::string* http_body)
{
    if (m_cloud_agent) return m_cloud_agent->get_user_tasks(params, http_body);
    return -1;
}

int NetworkAgent::get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body)
{
    if (m_cloud_agent) return m_cloud_agent->get_printer_firmware(dev_id, http_code, http_body);
    return -1;
}

int NetworkAgent::get_task_plate_index(std::string task_id, int* plate_index)
{
    if (m_cloud_agent) return m_cloud_agent->get_task_plate_index(task_id, plate_index);
    return -1;
}

int NetworkAgent::get_user_info(int* identifier)
{
    if (m_cloud_agent) return m_cloud_agent->get_user_info(identifier);
    return -1;
}

int NetworkAgent::request_bind_ticket(std::string* ticket)
{
    if (m_printer_agent) return m_printer_agent->request_bind_ticket(ticket);
    return -1;
}

int NetworkAgent::get_subtask_info(std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body)
{
    if (m_cloud_agent) return m_cloud_agent->get_subtask_info(subtask_id, task_json, http_code, http_body);
    return -1;
}

int NetworkAgent::get_slice_info(std::string project_id, std::string profile_id, int plate_index, std::string* slice_json)
{
    if (m_cloud_agent) return m_cloud_agent->get_slice_info(project_id, profile_id, plate_index, slice_json);
    return -1;
}

int NetworkAgent::query_bind_status(std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body)
{
    if (m_cloud_agent) return m_cloud_agent->query_bind_status(query_list, http_code, http_body);
    return -1;
}

int NetworkAgent::modify_printer_name(std::string dev_id, std::string dev_name)
{
    if (m_cloud_agent) return m_cloud_agent->modify_printer_name(dev_id, dev_name);
    return -1;
}

int NetworkAgent::get_camera_url(std::string dev_id, std::function<void(std::string)> callback)
{
    if (m_cloud_agent) return m_cloud_agent->get_camera_url(dev_id, callback);
    return -1;
}

int NetworkAgent::get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback)
{
    if (m_cloud_agent) return m_cloud_agent->get_design_staffpick(offset, limit, callback);
    return -1;
}

int NetworkAgent::start_publish(PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out)
{
    if (m_cloud_agent) return m_cloud_agent->start_publish(params, update_fn, cancel_fn, out);
    return -1;
}

int NetworkAgent::get_model_publish_url(std::string* url)
{
    if (m_cloud_agent) return m_cloud_agent->get_model_publish_url(url);
    return -1;
}

int NetworkAgent::get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn)
{
    if (m_cloud_agent) return m_cloud_agent->get_subtask(task, getsub_fn);
    return -1;
}

int NetworkAgent::get_model_mall_home_url(std::string* url)
{
    if (m_cloud_agent) return m_cloud_agent->get_model_mall_home_url(url);
    return -1;
}

int NetworkAgent::get_model_mall_detail_url(std::string* url, std::string id)
{
    if (m_cloud_agent) return m_cloud_agent->get_model_mall_detail_url(url, id);
    return -1;
}

int NetworkAgent::get_my_profile(std::string token, unsigned int* http_code, std::string* http_body)
{
    if (m_cloud_agent) return m_cloud_agent->get_my_profile(token, http_code, http_body);
    return -1;
}

int NetworkAgent::track_enable(bool enable)
{
    this->enable_track = enable;
    if (m_cloud_agent) return m_cloud_agent->track_enable(enable);
    return -1;
}

int NetworkAgent::track_remove_files()
{
    if (m_cloud_agent) return m_cloud_agent->track_remove_files();
    return -1;
}

int NetworkAgent::track_event(std::string evt_key, std::string content)
{
    if (m_cloud_agent) return m_cloud_agent->track_event(evt_key, content);
    return -1;
}

int NetworkAgent::track_header(std::string header)
{
    if (m_cloud_agent) return m_cloud_agent->track_header(header);
    return -1;
}

int NetworkAgent::track_update_property(std::string name, std::string value, std::string type)
{
    if (m_cloud_agent) return m_cloud_agent->track_update_property(name, value, type);
    return -1;
}

int NetworkAgent::track_get_property(std::string name, std::string& value, std::string type)
{
    if (m_cloud_agent) return m_cloud_agent->track_get_property(name, value, type);
    return -1;
}

int NetworkAgent::put_model_mall_rating(int design_id, int score, std::string content, std::vector<std::string> images, unsigned int& http_code, std::string& http_error)
{
    if (m_cloud_agent) return m_cloud_agent->put_model_mall_rating(design_id, score, content, images, http_code, http_error);
    return -1;
}

int NetworkAgent::get_oss_config(std::string& config, std::string country_code, unsigned int& http_code, std::string& http_error)
{
    if (m_cloud_agent) return m_cloud_agent->get_oss_config(config, country_code, http_code, http_error);
    return -1;
}

int NetworkAgent::put_rating_picture_oss(std::string& config, std::string& pic_oss_path, std::string model_id, int profile_id, unsigned int& http_code, std::string& http_error)
{
    if (m_cloud_agent) return m_cloud_agent->put_rating_picture_oss(config, pic_oss_path, model_id, profile_id, http_code, http_error);
    return -1;
}

int NetworkAgent::get_model_mall_rating_result(int job_id, std::string& rating_result, unsigned int& http_code, std::string& http_error)
{
    if (m_cloud_agent) return m_cloud_agent->get_model_mall_rating_result(job_id, rating_result, http_code, http_error);
    return -1;
}

int NetworkAgent::get_mw_user_preference(std::function<void(std::string)> callback)
{
    if (m_cloud_agent) return m_cloud_agent->get_mw_user_preference(callback);
    return -1;
}

int NetworkAgent::get_mw_user_4ulist(int seed, int limit, std::function<void(std::string)> callback)
{
    if (m_cloud_agent) return m_cloud_agent->get_mw_user_4ulist(seed, limit, callback);
    return -1;
}

} // namespace Slic3r
