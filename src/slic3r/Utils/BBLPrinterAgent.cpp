#include "BBLPrinterAgent.hpp"
#include "BBLNetworkPlugin.hpp"
#include "NetworkAgentFactory.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {

BBLPrinterAgent::BBLPrinterAgent() = default;

BBLPrinterAgent::~BBLPrinterAgent() = default;

void BBLPrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    m_cloud_agent = cloud;
    // BBL DLL manages tokens internally, so this is just for interface compliance
}

// ============================================================================
// Communication
// ============================================================================

std::string BBLPrinterAgent::ams_refresh_rfid_gcode(const std::string& tray_id)
{
    return (boost::format("M620 R%1% \n") % tray_id).str();
}

std::string BBLPrinterAgent::ams_calibrate_gcode(int ams_id)
{
    return (boost::format("M620 C%1% \n") % ams_id).str();
}

std::string BBLPrinterAgent::ams_select_tray_gcode(const std::string& tray_id)
{
    return (boost::format("M620 P%1% \n") % tray_id).str();
}

int BBLPrinterAgent::command_ams_refresh_rfid(std::string dev_id, std::string tray_id, int sequence_id, bool lan_mode)
{
    const std::string gcode = ams_refresh_rfid_gcode(tray_id);
    BOOST_LOG_TRIVIAL(trace) << "ams_debug: gcode_cmd" << gcode;
    nlohmann::json j;
    j["print"]["command"] = "gcode_line";
    j["print"]["param"] = gcode;
    j["print"]["sequence_id"] = std::to_string(sequence_id);
    return publish(dev_id, j, lan_mode);
}

int BBLPrinterAgent::command_ams_calibrate(std::string dev_id, int ams_id, int sequence_id, bool lan_mode)
{
    const std::string gcode = ams_calibrate_gcode(ams_id);
    BOOST_LOG_TRIVIAL(trace) << "ams_debug: gcode_cmd" << gcode;
    nlohmann::json j;
    j["print"]["command"] = "gcode_line";
    j["print"]["param"] = gcode;
    j["print"]["sequence_id"] = std::to_string(sequence_id);
    return publish(dev_id, j, lan_mode);
}

int BBLPrinterAgent::command_ams_select_tray(std::string dev_id, std::string tray_id, int sequence_id, bool lan_mode)
{
    const std::string gcode = ams_select_tray_gcode(tray_id);
    BOOST_LOG_TRIVIAL(trace) << "ams_debug: gcode_cmd" << gcode;
    nlohmann::json j;
    j["print"]["command"] = "gcode_line";
    j["print"]["param"] = gcode;
    j["print"]["sequence_id"] = std::to_string(sequence_id);
    return publish(dev_id, j, lan_mode);
}

int BBLPrinterAgent::publish(const std::string& dev_id, const nlohmann::json& j, bool lan_mode)
{
    const int rtn = lan_mode ? send_message_to_printer(dev_id, j.dump(), 0, 0) : send_message(dev_id, j.dump(), 0, 0);
    if (rtn == 0) {
        BOOST_LOG_TRIVIAL(info) << "publish_json: " << j.dump() << " code: " << rtn;
    } else {
        BOOST_LOG_TRIVIAL(error) << "publish_json: " << j.dump() << " code: " << rtn;
    }
    return rtn;
}

int BBLPrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_send_message();
    if (func && agent) {
        if (plugin.use_legacy_network()) {
            auto legacy_func = reinterpret_cast<func_send_message_legacy>(func);
            return legacy_func(agent, dev_id, json_str, qos);
        }
        return func(agent, dev_id, json_str, qos, flag);
    }
    return -1;
}

int BBLPrinterAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_connect_printer();
    if (func && agent) {
        return func(agent, dev_id, dev_ip, username, password, use_ssl);
    }
    return -1;
}

int BBLPrinterAgent::disconnect_printer()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_disconnect_printer();
    if (func && agent) {
        return func(agent);
    }
    return -1;
}

int BBLPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_send_message_to_printer();
    if (func && agent) {
        if (plugin.use_legacy_network()) {
            auto legacy_func = reinterpret_cast<func_send_message_to_printer_legacy>(func);
            return legacy_func(agent, dev_id, json_str, qos);
        }
        return func(agent, dev_id, json_str, qos, flag);
    }
    return -1;
}

// ============================================================================
// Certificates
// ============================================================================

int BBLPrinterAgent::check_cert()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_check_cert();
    if (func && agent) {
        return func(agent);
    }
    return -1;
}

void BBLPrinterAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_install_device_cert();
    if (func && agent) {
        func(agent, dev_id, lan_only);
    }
}

// ============================================================================
// Discovery
// ============================================================================

bool BBLPrinterAgent::start_discovery(bool start, bool sending)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_start_discovery();
    if (func && agent) {
        return func(agent, start, sending);
    }
    return false;
}

// ============================================================================
// Binding
// ============================================================================

int BBLPrinterAgent::ping_bind(std::string ping_code)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_ping_bind();
    if (func && agent) {
        return func(agent, ping_code);
    }
    return -1;
}

int BBLPrinterAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_bind_detect();
    if (func && agent) {
        return func(agent, dev_ip, sec_link, detect);
    }
    return -1;
}

int BBLPrinterAgent::bind(std::string dev_ip, std::string dev_id, std::string dev_model, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_bind();
    if (func && agent) {
        return func(agent, dev_ip, dev_id, dev_model, sec_link, timezone, improved, update_fn);
    }
    return -1;
}

int BBLPrinterAgent::unbind(std::string dev_id)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_unbind();
    if (func && agent) {
        return func(agent, dev_id);
    }
    return -1;
}

int BBLPrinterAgent::request_bind_ticket(std::string* ticket)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_request_bind_ticket();
    if (func && agent) {
        return func(agent, ticket);
    }
    return -1;
}

int BBLPrinterAgent::get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_hms_snapshot();
    // dev_id/file_name are passed as lvalues to bind the plugin's std::string& params.
    // A null func (older plugin without this symbol) falls through to -1 so callers degrade gracefully.
    if (func && agent) {
        return func(agent, dev_id, file_name, callback);
    }
    return -1;
}

int BBLPrinterAgent::set_server_callback(OnServerErrFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_server_callback();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

// ============================================================================
// Machine Selection
// ============================================================================

std::string BBLPrinterAgent::get_user_selected_machine()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_user_selected_machine();
    if (func && agent) {
        return func(agent);
    }
    return "";
}

int BBLPrinterAgent::set_user_selected_machine(std::string dev_id)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_user_selected_machine();
    if (func && agent) {
        return func(agent, dev_id);
    }
    return -1;
}

// ============================================================================
// Subscriptions
// ============================================================================

int BBLPrinterAgent::start_subscribe(std::string module)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_start_subscribe();
    if (func && agent) {
        return func(agent, module);
    }
    return -1;
}

int BBLPrinterAgent::stop_subscribe(std::string module)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_stop_subscribe();
    if (func && agent) {
        return func(agent, module);
    }
    return -1;
}

int BBLPrinterAgent::add_subscribe(std::vector<std::string> dev_list)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_add_subscribe();
    if (func && agent) {
        return func(agent, dev_list);
    }
    return -1;
}

int BBLPrinterAgent::del_subscribe(std::vector<std::string> dev_list)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_del_subscribe();
    if (func && agent) {
        return func(agent, dev_list);
    }
    return -1;
}

// ============================================================================
// Agent Information
// ============================================================================
AgentInfo BBLPrinterAgent::get_agent_info_static()
{
    return AgentInfo{BBL_PRINTER_AGENT_ID, "Bambu Lab", "", "Bambu Lab printer agent"};
}

// ============================================================================
// Print Job Operations
// ============================================================================

int BBLPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_start_print();
    if (func && agent) {
        if (plugin.use_legacy_network()) {
            auto legacy_func = reinterpret_cast<func_start_print_legacy>(func);
            auto legacy_params = BBLNetworkPlugin::as_legacy(params);
            return legacy_func(agent, legacy_params, update_fn, cancel_fn, wait_fn);
        }
        return func(agent, params, update_fn, cancel_fn, wait_fn);
    }
    return -1;
}

int BBLPrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_start_local_print_with_record();
    if (func && agent) {
        if (plugin.use_legacy_network()) {
            auto legacy_func = reinterpret_cast<func_start_local_print_with_record_legacy>(func);
            auto legacy_params = BBLNetworkPlugin::as_legacy(params);
            return legacy_func(agent, legacy_params, update_fn, cancel_fn, wait_fn);
        }
        return func(agent, params, update_fn, cancel_fn, wait_fn);
    }
    return -1;
}

int BBLPrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_start_send_gcode_to_sdcard();
    if (func && agent) {
        if (plugin.use_legacy_network()) {
            auto legacy_func = reinterpret_cast<func_start_send_gcode_to_sdcard_legacy>(func);
            auto legacy_params = BBLNetworkPlugin::as_legacy(params);
            return legacy_func(agent, legacy_params, update_fn, cancel_fn, wait_fn);
        }
        return func(agent, params, update_fn, cancel_fn, wait_fn);
    }
    return -1;
}

int BBLPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_start_local_print();
    if (func && agent) {
        if (plugin.use_legacy_network()) {
            auto legacy_func = reinterpret_cast<func_start_local_print_legacy>(func);
            auto legacy_params = BBLNetworkPlugin::as_legacy(params);
            return legacy_func(agent, legacy_params, update_fn, cancel_fn);
        }
        return func(agent, params, update_fn, cancel_fn);
    }
    return -1;
}

int BBLPrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_start_sdcard_print();
    if (func && agent) {
        if (plugin.use_legacy_network()) {
            auto legacy_func = reinterpret_cast<func_start_sdcard_print_legacy>(func);
            auto legacy_params = BBLNetworkPlugin::as_legacy(params);
            return legacy_func(agent, legacy_params, update_fn, cancel_fn);
        }
        return func(agent, params, update_fn, cancel_fn);
    }
    return -1;
}

// ============================================================================
// Callbacks
// ============================================================================

int BBLPrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_ssdp_msg_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLPrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_printer_connected_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLPrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_subscribe_failure_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLPrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_message_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLPrinterAgent::set_on_user_message_fn(OnMessageFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_user_message_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLPrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_local_connect_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLPrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_local_message_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_queue_on_main_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

// ============================================================================
// Filament Operations
// ============================================================================

FilamentSyncMode BBLPrinterAgent::get_filament_sync_mode() const
{
    // BBL uses MQTT subscription for real-time filament updates
    return FilamentSyncMode::subscription;
}

} // namespace Slic3r
