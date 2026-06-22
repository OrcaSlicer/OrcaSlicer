#include "BBLPrinterAgent.hpp"
#include "BBLNetworkPlugin.hpp"
#include "NetworkAgentFactory.hpp"
#include "orca/Session.hpp"
#include "orca/plugin_api.h"

#include <boost/log/trivial.hpp>
#include <memory>
#include <string>

namespace {

// ------------------------------------------------------------
// Phase 2.4.6 — engine plugin-slot bridge.
//
// The orca engine drives printer agents through a C ABI vtable
// (orca_slot_printer_agent_t). BBLOrcaBridge is what the engine sees behind
// the opaque void* instance pointer; it owns a fresh Slic3r::BBLPrinterAgent
// and translates the C surface into the existing IPrinterAgent surface.
// Out-of-impedance methods (start_print, cancel_print) are intentionally left
// NULL in the vtable so PrinterAgentAdapter returns NotImplemented to engine
// consumers — see register_with_orca_session.
// ------------------------------------------------------------
struct BBLOrcaBridge
{
    std::string                              device_id;
    orca_printer_state_t                     state{ORCA_PRINTER_STATE_DISCONNECTED};
    std::unique_ptr<Slic3r::BBLPrinterAgent> agent;
    const orca_printer_host_t*               host{nullptr};
};

extern "C" {

static void* bbl_create_instance(const orca_printer_host_t* host, void* /*user_data*/)
{
    auto* bridge  = new BBLOrcaBridge();
    bridge->host  = host;
    bridge->agent = std::make_unique<Slic3r::BBLPrinterAgent>();
    return bridge;
}

static void bbl_destroy_instance(void* instance)
{
    auto* bridge = static_cast<BBLOrcaBridge*>(instance);
    delete bridge;
}

static orca_error_code_t bbl_connect(void*       instance,
                                     const char* device_id,
                                     const char* host_or_ip,
                                     int         /*port*/,    // BBL agent has no port
                                     const char* username,
                                     const char* password,
                                     int         use_tls)
{
    auto* bridge = static_cast<BBLOrcaBridge*>(instance);
    if (bridge == nullptr || bridge->agent == nullptr)
        return ORCA_ERR_INVALID_ARGUMENT;

    const std::string dev_id = (device_id  != nullptr) ? device_id  : "";
    const std::string dev_ip = (host_or_ip != nullptr) ? host_or_ip : "";
    const std::string user   = (username   != nullptr) ? username   : "";
    const std::string pw     = (password   != nullptr) ? password   : "";

    bridge->device_id = dev_id;
    const int rc = bridge->agent->connect_printer(dev_id, dev_ip, user, pw, use_tls != 0);
    if (rc == BAMBU_NETWORK_SUCCESS) {
        bridge->state = ORCA_PRINTER_STATE_CONNECTED;
        return ORCA_OK;
    }
    bridge->state = ORCA_PRINTER_STATE_ERROR;
    return ORCA_ERR_IO;
}

static orca_error_code_t bbl_disconnect(void* instance)
{
    auto* bridge = static_cast<BBLOrcaBridge*>(instance);
    if (bridge == nullptr || bridge->agent == nullptr)
        return ORCA_ERR_INVALID_ARGUMENT;
    const int rc = bridge->agent->disconnect_printer();
    bridge->state = ORCA_PRINTER_STATE_DISCONNECTED;
    return (rc == BAMBU_NETWORK_SUCCESS) ? ORCA_OK : ORCA_ERR_IO;
}

static orca_printer_state_t bbl_current_state(void* instance)
{
    auto* bridge = static_cast<BBLOrcaBridge*>(instance);
    if (bridge == nullptr)
        return ORCA_PRINTER_STATE_DISCONNECTED;
    return bridge->state;
}

static orca_error_code_t bbl_send_command(void*       instance,
                                          const char* payload,
                                          size_t      payload_len)
{
    auto* bridge = static_cast<BBLOrcaBridge*>(instance);
    if (bridge == nullptr || bridge->agent == nullptr)
        return ORCA_ERR_INVALID_ARGUMENT;
    const std::string msg = (payload != nullptr) ? std::string{payload, payload_len} : std::string{};
    const int rc = bridge->agent->send_message_to_printer(bridge->device_id, msg, /*qos=*/0, /*flag=*/0);
    return (rc == BAMBU_NETWORK_SUCCESS) ? ORCA_OK : ORCA_ERR_IO;
}

// BBLPrinterAgent::start_print takes a Bambu-shaped PrintParams that doesn't
// cleanly fit orca::PrintJob (v1); start_print / cancel_print are left NULL
// in the vtable so PrinterAgentAdapter returns NotImplemented for them.

} // extern "C"

} // namespace

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

int BBLPrinterAgent::bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_bind();
    if (func && agent) {
        return func(agent, dev_ip, dev_id, sec_link, timezone, improved, update_fn);
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

void BBLPrinterAgent::register_with_orca_session(::orca::Session* session)
{
    if (session == nullptr) {
        return;
    }

    static bool s_registered = false;
    if (s_registered) {
        return;
    }

    // Function-local statics hold the C strings handed to the vtable so their
    // pointers remain stable for the lifetime of the process.
    static const AgentInfo   s_info        = BBLPrinterAgent::get_agent_info_static();
    static const std::string s_id          = s_info.id;
    static const std::string s_name        = s_info.name;
    static const std::string s_version     = s_info.version;
    static const std::string s_description = s_info.description;

    static orca_slot_printer_agent_t s_vtable = []() {
        orca_slot_printer_agent_t vt{};
        vt.struct_size       = sizeof(orca_slot_printer_agent_t);
        vt.agent_id          = s_id.c_str();
        vt.agent_name        = s_name.c_str();
        vt.agent_version     = s_version.c_str();
        vt.agent_description = s_description.c_str();
        vt.create_instance   = &bbl_create_instance;
        vt.destroy_instance  = &bbl_destroy_instance;
        vt.connect           = &bbl_connect;
        vt.disconnect        = &bbl_disconnect;
        vt.current_state     = &bbl_current_state;
        vt.send_command      = &bbl_send_command;
        // start_print / cancel_print intentionally NULL — BBLPrinterAgent's
        // start_print takes a Bambu-shaped PrintParams that doesn't cleanly fit
        // orca::PrintJob (v1). See PrinterAgentAdapter for NotImplemented behavior.
        vt.start_print       = nullptr;
        vt.cancel_print      = nullptr;
        return vt;
    }();

    const orca_plugin_slot_id_t slot_id = session->add_printer_agent_slot(
        /*owning_plugin_id=*/"orcaslicer.bambu",
        /*vtable=*/&s_vtable,
        /*user_data=*/nullptr,
        /*priority=*/0);
    if (slot_id != 0) {
        s_registered = true;
        BOOST_LOG_TRIVIAL(info) << "BBLPrinterAgent: registered with orca engine plugin registry (slot_id="
                                << slot_id << ")";
    } else {
        BOOST_LOG_TRIVIAL(warning) << "BBLPrinterAgent: failed to register with orca engine plugin registry";
    }
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
