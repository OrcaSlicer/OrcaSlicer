#include <stdio.h>
#include <stdlib.h>
#include <set>
#include <algorithm>

#include <boost/log/trivial.hpp>
#include "libslic3r/Utils.hpp"
#include "NetworkAgent.hpp"
#include "BBLNetworkPlugin.hpp"

namespace Slic3r {

namespace {

template<typename Fn>
int invoke_on_all_cloud_agents(const std::map<std::string, std::shared_ptr<ICloudServiceAgent>>& cloud_agents, Fn&& fn)
{
    if (cloud_agents.empty()) {
        return -1;
    }

    int result = 0;
    for (const auto& cloud_agent_pair : cloud_agents) {
        const int ret = fn(*cloud_agent_pair.second);
        if (result == 0 && ret != 0) {
            result = ret;
        }
    }

    return result;
}

} // namespace

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

bool NetworkAgent::versioned_library_exists(const std::string& version) { return BBLNetworkPlugin::versioned_library_exists(version); }

bool NetworkAgent::legacy_library_exists() { return BBLNetworkPlugin::legacy_library_exists(); }

void NetworkAgent::remove_legacy_library() { BBLNetworkPlugin::remove_legacy_library(); }

std::vector<std::string> NetworkAgent::scan_plugin_versions() { return BBLNetworkPlugin::scan_plugin_versions(); }

int NetworkAgent::initialize_network_module(bool using_backup, const std::string& version)
{
    return BBLNetworkPlugin::instance().initialize(using_backup, version);
}

int NetworkAgent::unload_network_module() { return BBLNetworkPlugin::instance().unload(); }

bool NetworkAgent::is_network_module_loaded() { return BBLNetworkPlugin::instance().is_loaded(); }

#if defined(_MSC_VER) || defined(_WIN32)
HMODULE NetworkAgent::get_bambu_source_entry() { return BBLNetworkPlugin::instance().get_bambu_source_entry(); }
#else
void* NetworkAgent::get_bambu_source_entry() { return BBLNetworkPlugin::instance().get_bambu_source_entry(); }
#endif

std::string NetworkAgent::get_version() { return BBLNetworkPlugin::instance().get_version(); }

void* NetworkAgent::get_network_function(const char* name) { return BBLNetworkPlugin::instance().get_network_function(name); }

NetworkLibraryLoadError NetworkAgent::get_load_error() { return BBLNetworkPlugin::instance().get_load_error(); }

void NetworkAgent::clear_load_error() { BBLNetworkPlugin::instance().clear_load_error(); }

void NetworkAgent::set_load_error(const std::string& message, const std::string& technical_details, const std::string& attempted_path)
{
    BBLNetworkPlugin::instance().set_load_error(message, technical_details, attempted_path);
}

// ============================================================================
// Constructors
// ============================================================================

NetworkAgent::NetworkAgent(std::shared_ptr<ICloudServiceAgent> cloud_agent, std::shared_ptr<IPrinterAgent> printer_agent)
    : m_printer_agent(std::move(printer_agent))
{
    if (!cloud_agent) {
        BOOST_LOG_TRIVIAL(warning) << "Null cloud agent provided, skipping agent initialization";
        return;
    }
    if (cloud_agent->get_id().empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Invalid cloud agent with empty ID provided, skipping agent initialization";
        return;
    }
    m_cloud_agents.emplace(cloud_agent->get_id(), std::move(cloud_agent));
}

NetworkAgent::~NetworkAgent()
{
    // Note: We don't destroy the agent here anymore since it's managed by BBLNetworkPlugin singleton
    // The singleton manages the agent lifecycle
}

void NetworkAgent::add_cloud_agent(const std::string& provider, std::shared_ptr<ICloudServiceAgent> agent)
{
    if (agent) {
        m_cloud_agents[provider] = std::move(agent);
    }
}

void NetworkAgent::set_printer_agent(std::shared_ptr<IPrinterAgent> printer_agent)
{
    if (!printer_agent) {
        return;
    }

    // Disconnect all callbacks from the old agent
    auto old_printer_agent = m_printer_agent;

    m_printer_agent    = std::move(printer_agent);
    m_printer_agent_id = m_printer_agent->get_agent_info().id;

    // Disconnect the old agent's connections/threads.
    if (old_printer_agent && old_printer_agent != m_printer_agent) {
        old_printer_agent->disconnect_printer();
        apply_printer_callbacks(old_printer_agent, {});
    }

    apply_printer_callbacks(m_printer_agent, m_printer_callbacks);
}

void* NetworkAgent::get_network_agent() { return BBLNetworkPlugin::instance().get_agent(); }

void NetworkAgent::apply_printer_callbacks(const std::shared_ptr<IPrinterAgent>& printer_agent, const PrinterCallbacks& callbacks)
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

std::shared_ptr<ICloudServiceAgent> NetworkAgent::get_cloud_agent(const std::string& provider) const
{
    const auto& key = (provider.empty() || provider == ORCA_CLOUD_PROVIDER) ? ORCA_CLOUD_PROVIDER : provider;
    auto it = m_cloud_agents.find(key);
    return it != m_cloud_agents.end() ? it->second : nullptr;
}

// ============================================================================
// Shared agent methods
// ============================================================================

int NetworkAgent::set_queue_on_main_fn(QueueOnMainFn fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    m_printer_callbacks.queue_on_main_fn = fn;

    int ret = -1;
    if (cloud_agent)
        ret = cloud_agent->set_queue_on_main_fn(fn);
    if (m_printer_agent)
        m_printer_agent->set_queue_on_main_fn(fn);
    return ret;
}

// ============================================================================
// Cloud agent methods
// ============================================================================

int NetworkAgent::init_log()
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.init_log(); });
}

int NetworkAgent::set_config_dir(std::string config_dir)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [&config_dir](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_config_dir(config_dir); });
}

int NetworkAgent::set_cert_file(std::string folder, std::string filename)
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [&folder, &filename](ICloudServiceAgent& cloud_agent) {
        return cloud_agent.set_cert_file(folder, filename);
    });
}

int NetworkAgent::set_country_code(std::string country_code)
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [&country_code](ICloudServiceAgent& cloud_agent) {
        return cloud_agent.set_country_code(country_code);
    });
}

int NetworkAgent::start()
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.start(); });
}

int NetworkAgent::set_on_server_connected_fn(AppOnServerConnectedFn fn)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [fn](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_on_server_connected_fn(fn); });
}

int NetworkAgent::set_on_http_error_fn(AppOnHttpErrorFn fn)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [fn](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_on_http_error_fn(fn); });
}

int NetworkAgent::set_get_country_code_fn(GetCountryCodeFn fn)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [fn](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_get_country_code_fn(fn); });
}

int NetworkAgent::change_user(std::string user_info, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->change_user(std::move(user_info));
    return -1;
}

bool NetworkAgent::is_user_login(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->is_user_login();
    return false;
}

int NetworkAgent::user_logout(bool request, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->user_logout(request);
    return -1;
}

std::string NetworkAgent::get_user_id(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_id();
    return "";
}

std::string NetworkAgent::get_user_name(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_name();
    return "";
}

std::string NetworkAgent::get_user_avatar(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_avatar();
    return "";
}

std::string NetworkAgent::get_user_nickname(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_nickname();
    return "";
}

std::string NetworkAgent::build_login_cmd(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->build_login_cmd();
    return "";
}

std::string NetworkAgent::build_logout_cmd(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->build_logout_cmd();
    return "";
}

std::string NetworkAgent::build_login_info(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->build_login_info();
    return "";
}

std::string NetworkAgent::get_cloud_service_host(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_cloud_service_host();
    return "";
}

std::string NetworkAgent::get_cloud_login_url(const std::string& language, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_cloud_login_url(language);
    return "";
}

int NetworkAgent::connect_server()
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.connect_server(); });
}

bool NetworkAgent::is_server_connected(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->is_server_connected();
    return false;
}

int NetworkAgent::refresh_connection(const std::string& provider)
{
    if(provider.empty())
        return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.refresh_connection(); });
    else {
        const auto cloud_agent = get_cloud_agent(provider);
        if (cloud_agent)
            return cloud_agent->refresh_connection();
        return -1;
    }
     
}

void NetworkAgent::enable_multi_machine(bool enable, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        cloud_agent->enable_multi_machine(enable);
}

int NetworkAgent::get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_presets(user_presets);
    return -1;
}

std::string NetworkAgent::request_setting_id(std::string                         name,
                                             std::map<std::string, std::string>* values_map,
                                             unsigned int*                       http_code,
                                             const std::string&                  provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->request_setting_id(std::move(name), values_map, http_code);
    return "";
}

int NetworkAgent::put_setting(std::string                         setting_id,
                              std::string                         name,
                              std::map<std::string, std::string>* values_map,
                              unsigned int*                       http_code,
                              const std::string&                  provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->put_setting(std::move(setting_id), std::move(name), values_map, http_code);
    return -1;
}

int NetworkAgent::get_setting_list(std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_setting_list(std::move(bundle_version), pro_fn, cancel_fn);
    return -1;
}

int NetworkAgent::get_setting_list2(
    std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_setting_list2(std::move(bundle_version), chk_fn, pro_fn, cancel_fn);
    return -1;
}

int NetworkAgent::delete_setting(std::string setting_id, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->delete_setting(std::move(setting_id));
    return -1;
}

int NetworkAgent::get_my_message(int type, int after, int limit, unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_my_message(type, after, limit, http_code, http_body);
    return -1;
}

int NetworkAgent::check_user_task_report(int* task_id, bool* printable, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->check_user_task_report(task_id, printable);
    return -1;
}

int NetworkAgent::get_user_print_info(unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_print_info(http_code, http_body);
    return -1;
}

int NetworkAgent::get_user_tasks(TaskQueryParams params, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_tasks(params, http_body);
    return -1;
}

int NetworkAgent::get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_printer_firmware(std::move(dev_id), http_code, http_body);
    return -1;
}

int NetworkAgent::get_task_plate_index(std::string task_id, int* plate_index, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_task_plate_index(std::move(task_id), plate_index);
    return -1;
}

int NetworkAgent::get_user_info(int* identifier, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_info(identifier);
    return -1;
}

int NetworkAgent::get_subtask_info(
    std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_subtask_info(std::move(subtask_id), task_json, http_code, http_body);
    return -1;
}

int NetworkAgent::get_slice_info(
    std::string project_id, std::string profile_id, int plate_index, std::string* slice_json, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_slice_info(std::move(project_id), std::move(profile_id), plate_index, slice_json);
    return -1;
}

int NetworkAgent::query_bind_status(std::vector<std::string> query_list,
                                    unsigned int*            http_code,
                                    std::string*             http_body,
                                    const std::string&       provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->query_bind_status(std::move(query_list), http_code, http_body);
    return -1;
}

int NetworkAgent::modify_printer_name(std::string dev_id, std::string dev_name, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->modify_printer_name(std::move(dev_id), std::move(dev_name));
    return -1;
}

int NetworkAgent::get_camera_url(std::string dev_id, std::function<void(std::string)> callback, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_camera_url(std::move(dev_id), std::move(callback));
    return -1;
}

int NetworkAgent::get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_design_staffpick(offset, limit, std::move(callback));
    return -1;
}

int NetworkAgent::start_publish(
    PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->start_publish(params, update_fn, cancel_fn, out);
    return -1;
}

int NetworkAgent::get_model_publish_url(std::string* url, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_model_publish_url(url);
    return -1;
}

int NetworkAgent::get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_subtask(task, getsub_fn);
    return -1;
}

int NetworkAgent::get_model_mall_home_url(std::string* url, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_model_mall_home_url(url);
    return -1;
}

int NetworkAgent::get_model_mall_detail_url(std::string* url, std::string id, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_model_mall_detail_url(url, std::move(id));
    return -1;
}

int NetworkAgent::get_my_profile(std::string token, unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_my_profile(std::move(token), http_code, http_body);
    return -1;
}

int NetworkAgent::get_my_token(std::string ticket, unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_my_token(std::move(ticket), http_code, http_body);
    return -1;
}

int NetworkAgent::track_enable(bool enable, const std::string& provider)
{
    this->enable_track     = enable;
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_enable(enable);
    return -1;
}

int NetworkAgent::track_remove_files(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_remove_files();
    return -1;
}

int NetworkAgent::track_event(std::string evt_key, std::string content, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_event(std::move(evt_key), std::move(content));
    return -1;
}

int NetworkAgent::track_header(std::string header, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_header(std::move(header));
    return -1;
}

int NetworkAgent::track_update_property(std::string name, std::string value, std::string type, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_update_property(std::move(name), std::move(value), std::move(type));
    return -1;
}

int NetworkAgent::track_get_property(std::string name, std::string& value, std::string type, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_get_property(std::move(name), value, std::move(type));
    return -1;
}

int NetworkAgent::put_model_mall_rating(int                      design_id,
                                        int                      score,
                                        std::string              content,
                                        std::vector<std::string> images,
                                        unsigned int&            http_code,
                                        std::string&             http_error,
                                        const std::string&       provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->put_model_mall_rating(design_id, score, std::move(content), std::move(images), http_code, http_error);
    return -1;
}

int NetworkAgent::get_oss_config(
    std::string& config, std::string country_code, unsigned int& http_code, std::string& http_error, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_oss_config(config, std::move(country_code), http_code, http_error);
    return -1;
}

int NetworkAgent::put_rating_picture_oss(std::string&       config,
                                         std::string&       pic_oss_path,
                                         std::string        model_id,
                                         int                profile_id,
                                         unsigned int&      http_code,
                                         std::string&       http_error,
                                         const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->put_rating_picture_oss(config, pic_oss_path, std::move(model_id), profile_id, http_code, http_error);
    return -1;
}

int NetworkAgent::get_model_mall_rating_result(
    int job_id, std::string& rating_result, unsigned int& http_code, std::string& http_error, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_model_mall_rating_result(job_id, rating_result, http_code, http_error);
    return -1;
}

int NetworkAgent::get_mw_user_preference(std::function<void(std::string)> callback, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_mw_user_preference(std::move(callback));
    return -1;
}

int NetworkAgent::get_mw_user_4ulist(int seed, int limit, std::function<void(std::string)> callback, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_mw_user_4ulist(seed, limit, std::move(callback));
    return -1;
}

// ============================================================================
// Printer agent methods
// ============================================================================

int NetworkAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    m_printer_callbacks.on_ssdp_msg_fn = fn;
    if (m_printer_agent)
        return m_printer_agent->set_on_ssdp_msg_fn(fn);
    return -1;
}

int NetworkAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    m_printer_callbacks.on_printer_connected_fn = fn;
    if (m_printer_agent)
        return m_printer_agent->set_on_printer_connected_fn(fn);
    return -1;
}

int NetworkAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    m_printer_callbacks.on_subscribe_failure_fn = fn;
    if (m_printer_agent)
        return m_printer_agent->set_on_subscribe_failure_fn(fn);
    return -1;
}

int NetworkAgent::set_on_message_fn(OnMessageFn fn)
{
    m_printer_callbacks.on_message_fn = fn;
    if (m_printer_agent)
        return m_printer_agent->set_on_message_fn(fn);
    return -1;
}

int NetworkAgent::set_on_user_message_fn(OnMessageFn fn)
{
    m_printer_callbacks.on_user_message_fn = fn;
    if (m_printer_agent)
        return m_printer_agent->set_on_user_message_fn(fn);
    return -1;
}

int NetworkAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    m_printer_callbacks.on_local_connect_fn = fn;
    if (m_printer_agent)
        return m_printer_agent->set_on_local_connect_fn(fn);
    return -1;
}

int NetworkAgent::set_on_local_message_fn(OnMessageFn fn)
{
    m_printer_callbacks.on_local_message_fn = fn;
    if (m_printer_agent)
        return m_printer_agent->set_on_local_message_fn(fn);
    return -1;
}

int NetworkAgent::set_server_callback(OnServerErrFn fn)
{
    m_printer_callbacks.on_server_err_fn = fn;
    if (m_printer_agent)
        return m_printer_agent->set_server_callback(fn);
    return -1;
}

int NetworkAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    if (m_printer_agent)
        return m_printer_agent->send_message(dev_id, json_str, qos, flag);
    return -1;
}

int NetworkAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    if (m_printer_agent)
        return m_printer_agent->connect_printer(dev_id, dev_ip, username, password, use_ssl);
    return -1;
}

int NetworkAgent::disconnect_printer()
{
    if (m_printer_agent)
        return m_printer_agent->disconnect_printer();
    return -1;
}

int NetworkAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    if (m_printer_agent)
        return m_printer_agent->send_message_to_printer(dev_id, json_str, qos, flag);
    return -1;
}

int NetworkAgent::check_cert()
{
    if (m_printer_agent)
        return m_printer_agent->check_cert();
    return -1;
}

void NetworkAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    if (m_printer_agent)
        m_printer_agent->install_device_cert(dev_id, lan_only);
}

bool NetworkAgent::start_discovery(bool start, bool sending)
{
    if (m_printer_agent)
        return m_printer_agent->start_discovery(start, sending);
    return false;
}

int NetworkAgent::ping_bind(std::string ping_code)
{
    if (m_printer_agent)
        return m_printer_agent->ping_bind(ping_code);
    return -1;
}

int NetworkAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    if (m_printer_agent)
        return m_printer_agent->bind_detect(dev_ip, sec_link, detect);
    return -1;
}

int NetworkAgent::bind(
    std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn)
{
    if (m_printer_agent)
        return m_printer_agent->bind(dev_ip, dev_id, sec_link, timezone, improved, update_fn);
    return -1;
}

int NetworkAgent::unbind(std::string dev_id)
{
    if (m_printer_agent)
        return m_printer_agent->unbind(dev_id);
    return -1;
}

std::string NetworkAgent::get_user_selected_machine()
{
    if (m_printer_agent)
        return m_printer_agent->get_user_selected_machine();
    return "";
}

int NetworkAgent::set_user_selected_machine(std::string dev_id)
{
    if (m_printer_agent)
        return m_printer_agent->set_user_selected_machine(dev_id);
    return -1;
}

int NetworkAgent::start_subscribe(std::string module)
{
    if (m_printer_agent)
        return m_printer_agent->start_subscribe(std::move(module));
    return -1;
}

int NetworkAgent::stop_subscribe(std::string module)
{
    if (m_printer_agent)
        return m_printer_agent->stop_subscribe(std::move(module));
    return -1;
}

int NetworkAgent::add_subscribe(std::vector<std::string> dev_list)
{
    if (m_printer_agent)
        return m_printer_agent->add_subscribe(std::move(dev_list));
    return -1;
}

int NetworkAgent::del_subscribe(std::vector<std::string> dev_list)
{
    if (m_printer_agent)
        return m_printer_agent->del_subscribe(std::move(dev_list));
    return -1;
}

int NetworkAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    if (m_printer_agent)
        return m_printer_agent->start_print(params, update_fn, cancel_fn, wait_fn);
    return -1;
}

int NetworkAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    if (m_printer_agent)
        return m_printer_agent->start_local_print_with_record(params, update_fn, cancel_fn, wait_fn);
    return -1;
}

int NetworkAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    if (m_printer_agent)
        return m_printer_agent->start_send_gcode_to_sdcard(params, update_fn, cancel_fn, wait_fn);
    return -1;
}

int NetworkAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (m_printer_agent)
        return m_printer_agent->start_local_print(params, update_fn, cancel_fn);
    return -1;
}

int NetworkAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (m_printer_agent)
        return m_printer_agent->start_sdcard_print(params, update_fn, cancel_fn);
    return -1;
}

FilamentSyncMode NetworkAgent::get_filament_sync_mode() const
{
    if (m_printer_agent)
        return m_printer_agent->get_filament_sync_mode();
    return FilamentSyncMode::none;
}

bool NetworkAgent::fetch_filament_info(std::string dev_id)
{
    if (m_printer_agent) {
        return m_printer_agent->fetch_filament_info(dev_id);
    }
    return false;
}

int NetworkAgent::request_bind_ticket(std::string* ticket)
{
    if (m_printer_agent)
        return m_printer_agent->request_bind_ticket(ticket);
    return -1;
}

} // namespace Slic3r
