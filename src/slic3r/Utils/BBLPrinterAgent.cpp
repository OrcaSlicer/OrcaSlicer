#include "BBLPrinterAgent.hpp"
#include "BBLNetworkPlugin.hpp"
#include "NetworkAgentFactory.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include <unordered_map>

namespace Slic3r {

namespace {

// Bambu's own catalog ids for every filament this app ships, Bambu's own included, since Orca
// content-addresses those too. Keyed both ways so each of the four translation entry points
// below is a single lookup. Loaded once per process, on first use. A missing or malformed file
// logs once and leaves both maps empty, so every translation degrades to identity. Same shape
// as DevFilaBlacklist::load_filaments_blacklist_config.
struct BambuFilamentIdMap { std::unordered_map<std::string, std::string> to_bambu, to_orca; };

const BambuFilamentIdMap& bambu_filament_id_map()
{
    static const BambuFilamentIdMap map = [] {
        BambuFilamentIdMap m;
        const std::string path = resources_dir() + "/printers/bambu_filament_ids.json";
        try {
            boost::nowide::ifstream file(path);
            if (!file.is_open()) {
                BOOST_LOG_TRIVIAL(warning) << "Bambu filament id map not found, ids pass through untranslated: " << path;
                return m;
            }
            json doc;
            file >> doc;
            for (const auto& [orca_filament_id, row] : doc.at("filaments").items()) {
                const std::string bambu_id = row.at("bambu_id").get<std::string>();
                m.to_bambu.emplace(orca_filament_id, bambu_id);
                m.to_orca.emplace(bambu_id, orca_filament_id);
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Bambu filament id map unreadable, ids pass through untranslated: " << e.what();
            m = {};
        }
        return m;
    }();
    return map;
}

// Rewrites every string under "tray_info_idx", "filament_id" or "filamentId", at any depth, in place.
void rewrite_filament_ids(json& j, const std::unordered_map<std::string, std::string>& map)
{
    if (j.is_object()) {
        for (auto& [key, value] : j.items()) {
            if (value.is_string() && (key == "tray_info_idx" || key == "filament_id" || key == "filamentId")) {
                auto it = map.find(value.get_ref<const std::string&>());
                if (it != map.end())
                    value = it->second;
            } else
                rewrite_filament_ids(value, map);
        }
    } else if (j.is_array())
        for (auto& element : j)
            rewrite_filament_ids(element, map);
}

// Text that does not parse as JSON, or that mentions none of the id keys, comes back byte-identical.
std::string rewrite_filament_ids(std::string text, const std::unordered_map<std::string, std::string>& map)
{
    if (map.empty() || (text.find("tray_info_idx") == std::string::npos && text.find("filament_id") == std::string::npos &&
                        text.find("filamentId") == std::string::npos))
        return text;                                        // nothing to map, skip the parse (moved, not copied)
    try {
        json j = json::parse(text);
        rewrite_filament_ids(j, map);
        return j.dump();
    } catch (const std::exception&) {
        return text;                                        // not JSON: forward as received
    }
}

// Wraps an inbound message callback so every Bambu id it delivers arrives already translated.
// A null fn is a deregistration (see GUI_App.cpp's shutdown phase 1 and NetworkAgent::apply_printer_callbacks
// clearing callbacks with {}), and must stay null rather than become a live wrapper around an empty target.
OnMessageFn to_orca_messages(OnMessageFn fn)
{
    if (!fn)
        return fn;
    return [fn = std::move(fn)](std::string dev_id, std::string msg) { fn(std::move(dev_id), BBLPrinterAgent::to_orca_payload(std::move(msg))); };
}

} // namespace

std::string BBLPrinterAgent::to_orca_filament_id(const std::string& printer_filament_id) const
{
    const auto& map = bambu_filament_id_map().to_orca;
    auto it = map.find(printer_filament_id);
    return it != map.end() ? it->second : printer_filament_id;
}

std::string BBLPrinterAgent::from_orca_filament_id(const std::string& orca_filament_id) const
{
    const auto& map = bambu_filament_id_map().to_bambu;
    auto it = map.find(orca_filament_id);
    return it != map.end() ? it->second : orca_filament_id;
}

std::string BBLPrinterAgent::to_orca_payload(std::string json_text)
{
    return rewrite_filament_ids(std::move(json_text), bambu_filament_id_map().to_orca);
}

std::string BBLPrinterAgent::from_orca_payload(std::string json_text)
{
    return rewrite_filament_ids(std::move(json_text), bambu_filament_id_map().to_bambu);
}

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
    json_str = from_orca_payload(std::move(json_str));
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_send_message();
    if (func && agent) {
        // Only the legacy plug-in lacks `flag`; 02.03.00 already takes it, and routing that
        // series through the legacy form would silently drop MessageFlag sign/encrypt.
        switch (plugin.network_abi()) {
        case NetworkAbi::Legacy: {
            auto legacy_func = reinterpret_cast<func_send_message_legacy>(func);
            return legacy_func(agent, std::move(dev_id), std::move(json_str), qos);
        }
        case NetworkAbi::V0203:
        case NetworkAbi::Current:
            return func(agent, std::move(dev_id), std::move(json_str), qos, flag);
        default:
            return -1;
        }
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
    json_str = from_orca_payload(std::move(json_str));
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_send_message_to_printer();
    if (func && agent) {
        switch (plugin.network_abi()) {
        case NetworkAbi::Legacy: {
            auto legacy_func = reinterpret_cast<func_send_message_to_printer_legacy>(func);
            return legacy_func(agent, std::move(dev_id), std::move(json_str), qos);
        }
        case NetworkAbi::V0203:
        case NetworkAbi::Current:
            return func(agent, std::move(dev_id), std::move(json_str), qos, flag);
        default:
            return -1;
        }
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
        // dev_model was added in 02.08.01. Passing it to a plug-in that takes the 7-argument
        // form shifts every following argument, so the older generations get the older call.
        switch (plugin.network_abi()) {
        case NetworkAbi::Legacy:
        case NetworkAbi::V0203: {
            auto older_func = reinterpret_cast<func_bind_pre0208>(func);
            return older_func(agent, dev_ip, dev_id, sec_link, timezone, improved, update_fn);
        }
        case NetworkAbi::Current:
            return func(agent, dev_ip, dev_id, dev_model, sec_link, timezone, improved, update_fn);
        default:
            return -1;
        }
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

namespace {

// Shared dispatcher for the start_* operations, whose params layout differs per generation.
// The per-generation typedefs are template arguments so a swapped pair fails to compile
// (each arm's converted params must match the casted signature). Each arm converts and calls
// in one step: as_legacy()/as_0203() move out of `params` and their prvalue result lands in
// the by-value ABI argument without another copy; the Current arm moves `params` outright.
template <typename LegacyFn, typename Fn0203, typename CurrentFn, typename... CallbackFns>
int dispatch_start(CurrentFn func, PrintParams& params, const CallbackFns&... callbacks)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    if (!func || !agent)
        return -1;
    params.ams_mapping_info = BBLPrinterAgent::from_orca_payload(std::move(params.ams_mapping_info));
    switch (plugin.network_abi()) {
    case NetworkAbi::Legacy:
        return reinterpret_cast<LegacyFn>(func)(agent, BBLNetworkPlugin::as_legacy(params), callbacks...);
    case NetworkAbi::V0203:
        return reinterpret_cast<Fn0203>(func)(agent, BBLNetworkPlugin::as_0203(params), callbacks...);
    case NetworkAbi::Current:
        return func(agent, std::move(params), callbacks...);
    default:
        return -1;
    }
}

} // namespace

int BBLPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    return dispatch_start<func_start_print_legacy, func_start_print_0203>(
        BBLNetworkPlugin::instance().get_start_print(), params, update_fn, cancel_fn, wait_fn);
}

int BBLPrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    return dispatch_start<func_start_local_print_with_record_legacy, func_start_local_print_with_record_0203>(
        BBLNetworkPlugin::instance().get_start_local_print_with_record(), params, update_fn, cancel_fn, wait_fn);
}

int BBLPrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    return dispatch_start<func_start_send_gcode_to_sdcard_legacy, func_start_send_gcode_to_sdcard_0203>(
        BBLNetworkPlugin::instance().get_start_send_gcode_to_sdcard(), params, update_fn, cancel_fn, wait_fn);
}

int BBLPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    return dispatch_start<func_start_local_print_legacy, func_start_local_print_0203>(
        BBLNetworkPlugin::instance().get_start_local_print(), params, update_fn, cancel_fn);
}

int BBLPrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    return dispatch_start<func_start_sdcard_print_legacy, func_start_sdcard_print_0203>(
        BBLNetworkPlugin::instance().get_start_sdcard_print(), params, update_fn, cancel_fn);
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
        return func(agent, to_orca_messages(std::move(fn)));
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
        return func(agent, to_orca_messages(std::move(fn)));
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
