#include "QidiPrinterAgent.hpp"
#include "Http.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <sstream>

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

std::string to_hex_string(uint64_t value)
{
    std::ostringstream stream;
    stream << std::hex << std::uppercase << value;
    return stream.str();
}

bool looks_like_host(const std::string& value)
{
    if (value.empty()) {
        return false;
    }
    if (value.find(' ') != std::string::npos) {
        return false;
    }
    return value.find('.') != std::string::npos || value.find(':') != std::string::npos;
}

constexpr const char* k_no_api_key = "__NO_API_KEY__";

bool is_numeric(const std::string& value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string normalize_base_url(std::string host, const std::string& port)
{
    boost::trim(host);
    if (host.empty()) {
        return "";
    }

    std::string value = host;
    if (is_numeric(port) && value.find("://") == std::string::npos && value.find(':') == std::string::npos) {
        value += ":" + port;
    }

    if (!boost::istarts_with(value, "http://") && !boost::istarts_with(value, "https://")) {
        value = "http://" + value;
    }

    if (value.size() > 1 && value.back() == '/') {
        value.pop_back();
    }

    return value;
}

std::string extract_host(const std::string& base_url)
{
    std::string host = base_url;
    auto        pos  = host.find("://");
    if (pos != std::string::npos) {
        host = host.substr(pos + 3);
    }
    pos = host.find('/');
    if (pos != std::string::npos) {
        host = host.substr(0, pos);
    }
    return host;
}

std::string join_url(const std::string& base_url, const std::string& path)
{
    if (base_url.empty()) {
        return "";
    }
    if (path.empty()) {
        return base_url;
    }
    if (base_url.back() == '/' && path.front() == '/') {
        return base_url.substr(0, base_url.size() - 1) + path;
    }
    if (base_url.back() != '/' && path.front() != '/') {
        return base_url + "/" + path;
    }
    return base_url + path;
}

std::string normalize_api_key(const std::string& api_key)
{
    if (api_key.empty() || api_key == k_no_api_key) {
        return "";
    }
    return api_key;
}

std::string normalize_model_key(std::string value)
{
    boost::algorithm::to_lower(value);
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c)) {
            normalized.push_back(static_cast<char>(c));
        }
    }
    return normalized;
}

std::string infer_series_id(const std::string& model_id, const std::string& dev_name)
{
    std::string source = model_id.empty() ? dev_name : model_id;
    boost::trim(source);
    if (source.empty()) {
        return "";
    }
    if (is_numeric(source)) {
        return source;
    }

    const std::string key = normalize_model_key(source);
    if (key.find("q2") != std::string::npos) {
        return "1";
    }
    if (key.find("xmax") != std::string::npos && key.find("4") != std::string::npos) {
        return "3";
    }
    if ((key.find("xplus") != std::string::npos || key.find("plus") != std::string::npos) && key.find("4") != std::string::npos) {
        return "0";
    }
    return "";
}

struct WsEndpoint
{
    std::string host;
    std::string port;
    std::string target;
    bool        secure = false;
};

bool parse_ws_endpoint(const std::string& base_url, WsEndpoint& endpoint)
{
    if (base_url.empty()) {
        return false;
    }

    std::string url = base_url;
    if (boost::istarts_with(url, "https://")) {
        endpoint.secure = true;
        url             = url.substr(8);
    } else if (boost::istarts_with(url, "http://")) {
        url = url.substr(7);
    }

    auto slash = url.find('/');
    if (slash != std::string::npos) {
        url = url.substr(0, slash);
    }
    if (url.empty()) {
        return false;
    }

    endpoint.host = url;
    endpoint.port = endpoint.secure ? "443" : "80";
    if (auto colon = url.rfind(':'); colon != std::string::npos && url.find(']') == std::string::npos) {
        endpoint.host = url.substr(0, colon);
        endpoint.port = url.substr(colon + 1);
    }

    endpoint.target = "/websocket";
    return !endpoint.host.empty() && !endpoint.port.empty();
}

std::string map_moonraker_state(std::string state)
{
    boost::algorithm::to_lower(state);
    if (state == "printing") {
        return "RUNNING";
    }
    if (state == "paused") {
        return "PAUSE";
    }
    if (state == "complete") {
        return "FINISH";
    }
    if (state == "error" || state == "cancelled") {
        return "FAILED";
    }
    return "IDLE";
}

std::string normalize_filament_type(const std::string& filament_type)
{
    std::string trimmed = filament_type;
    boost::trim(trimmed);
    std::string upper = trimmed;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    if (upper.find("PLA") != std::string::npos)
        return "PLA";
    if (upper.find("ABS") != std::string::npos)
        return "ABS";
    if (upper.find("PETG") != std::string::npos)
        return "PETG";
    if (upper.find("TPU") != std::string::npos)
        return "TPU";
    if (upper.find("ASA") != std::string::npos)
        return "ASA";
    if (upper.find("PA") != std::string::npos || upper.find("NYLON") != std::string::npos)
        return "PA";
    if (upper.find("PC") != std::string::npos)
        return "PC";
    if (upper.find("PVA") != std::string::npos)
        return "PVA";

    return trimmed;
}
} // namespace

namespace Slic3r {

const std::string QidiPrinterAgent_VERSION = "0.0.1";

QidiPrinterAgent::QidiPrinterAgent(std::string log_dir) : OrcaPrinterAgent(std::move(log_dir))
{
    BOOST_LOG_TRIVIAL(info) << "QidiPrinterAgent: Constructor";
}

QidiPrinterAgent::~QidiPrinterAgent()
{
    stop_status_stream();
}

AgentInfo QidiPrinterAgent::get_agent_info_static()
{
    return AgentInfo{.id = "qidi", .name = "Qidi Printer Agent", .version = QidiPrinterAgent_VERSION, .description = "Qidi printer agent"};
}

int QidiPrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    (void) qos;
    (void) flag;
    return handle_request(dev_id, json_str);
}

int QidiPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    (void) qos;
    (void) flag;
    return handle_request(dev_id, json_str);
}

int QidiPrinterAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    (void) username;
    (void) use_ssl;
    std::string base_url = normalize_base_url(dev_ip, "");
    std::string api_key  = normalize_api_key(password);

    PrinthostConfig config;
    if (get_printhost_config(config)) {
        if (base_url.empty()) {
            base_url = config.base_url;
        }
        if (api_key.empty()) {
            api_key = normalize_api_key(config.api_key);
        }
    }

    if (base_url.empty()) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent: connect_printer missing host for dev_id=" << dev_id;
        dispatch_local_connect(ConnectStatusFailed, dev_id, "host_missing");
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    if (dev_id.empty()) {
        dev_id = extract_host(base_url);
    }

    {
        std::lock_guard<std::mutex> lock(payload_mutex);
        status_cache     = nlohmann::json::object();
        last_ams_payload = nlohmann::json();
    }
    ws_last_emit_ms.store(0);

    store_host(dev_id, base_url, api_key);
    start_status_stream(dev_id, base_url, api_key);
    dispatch_local_connect(ConnectStatusOk, dev_id, "0");
    dispatch_printer_connected(dev_id);
    BOOST_LOG_TRIVIAL(info) << "QidiPrinterAgent: connect_printer - dev_id=" << dev_id << ", dev_ip=" << dev_ip;
    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::disconnect_printer()
{
    stop_status_stream();
    return BAMBU_NETWORK_SUCCESS;
}

bool QidiPrinterAgent::start_discovery(bool start, bool sending)
{
    (void) sending;
    if (start) {
        announce_printhost_device();
    }
    return true;
}

int QidiPrinterAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    (void) sec_link;

    std::string base_url = normalize_base_url(dev_ip, "");
    if (base_url.empty()) {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    PrinthostConfig config;
    get_printhost_config(config);
    const std::string api_key = normalize_api_key(config.api_key);

    QidiDeviceInfo info;
    std::string    error;
    if (!fetch_device_info(base_url, api_key, info, error)) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent: bind_detect failed: " << error;
        return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    }

    detect.dev_id       = info.dev_id.empty() ? dev_ip : info.dev_id;
    if (!info.model_id.empty()) {
        detect.model_id = info.model_id;
    } else if (!config.model_id.empty()) {
        detect.model_id = config.model_id;
    } else {
        detect.model_id = config.model_name;
    }
    detect.dev_name     = info.dev_name.empty() ? config.model_name : info.dev_name;
    detect.version      = info.version;
    detect.connect_type = "lan";
    detect.bind_state   = "free";

    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        on_ssdp_msg_fn = fn;
    }
    if (fn) {
        announce_printhost_device();
    }
    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_printer_connected_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_local_connect_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_local_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    queue_on_main_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

void QidiPrinterAgent::fetch_filament_info(std::string dev_id)
{
    // Look up MachineObject via DeviceManager
    auto* dev_manager = GUI::wxGetApp().getDeviceManager();
    if (!dev_manager) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent::fetch_filament_info: DeviceManager is null";
        return;
    }
    MachineObject* obj = dev_manager->get_my_machine(dev_id);
    if (!obj) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent::fetch_filament_info: MachineObject not found for dev_id=" << dev_id;
        return;
    }

    const std::string base_url = resolve_host(dev_id);
    if (base_url.empty()) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent::fetch_filament_info: Missing host for dev_id=" << dev_id;
        return;
    }
    const std::string api_key = resolve_api_key(dev_id, "");

    std::vector<QidiSlotInfo> slots;
    int                       box_count = 0;
    std::string               error;
    if (!fetch_slot_info(base_url, api_key, slots, box_count, error)) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent::fetch_filament_info: Failed to fetch slot info: " << error;
        return;
    }

    QidiFilamentDict dict;
    if (!fetch_filament_dict(base_url, api_key, dict, error)) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent::fetch_filament_info: Failed to fetch filament dict: " << error;
    }

    std::string series_id;
    {
        QidiDeviceInfo info;
        std::string    device_error;
        if (fetch_device_info(base_url, api_key, info, device_error)) {
            series_id = infer_series_id(info.model_id, info.dev_name);
        }
    }

    auto build_setting_id = [&](const QidiSlotInfo& slot, const std::string& tray_type) {
        const int vendor = (slot.vendor_type == 1) ? 1 : 0;
        if (is_numeric(series_id) && slot.filament_type > 0) {
            return "QD_" + series_id + "_" + std::to_string(vendor) + "_" + std::to_string(slot.filament_type);
        }
        return map_filament_type_to_setting_id(tray_type);
    };

    // Build BBL-format JSON for DevFilaSystemParser::ParseV1_0
    // The expected format matches BBL's print.push_status AMS subset
    nlohmann::json ams_json = nlohmann::json::object();
    nlohmann::json ams_array = nlohmann::json::array();

    // Calculate ams_exist_bits and tray_exist_bits
    unsigned long ams_exist_bits = 0;
    unsigned long tray_exist_bits = 0;

    for (int ams_id = 0; ams_id < box_count; ++ams_id) {
        ams_exist_bits |= (1 << ams_id);

        nlohmann::json ams_unit = nlohmann::json::object();
        ams_unit["id"] = std::to_string(ams_id);
        ams_unit["info"] = "2100";  // AMS_LITE type (2), main extruder (0)

        nlohmann::json tray_array = nlohmann::json::array();
        for (int slot_id = 0; slot_id < 4; ++slot_id) {
            const int          slot_index = ams_id * 4 + slot_id;
            const QidiSlotInfo slot       = slot_index < static_cast<int>(slots.size()) ? slots[slot_index] : QidiSlotInfo{};

            nlohmann::json tray_json = nlohmann::json::object();
            tray_json["id"] = std::to_string(slot_id);
            tray_json["tag_uid"] = "0000000000000000";

            if (slot.filament_exists) {
                tray_exist_bits |= (1 << slot_index);

                std::string filament_type = "PLA";
                auto        filament_it   = dict.filaments.find(slot.filament_type);
                if (filament_it != dict.filaments.end()) {
                    filament_type = filament_it->second;
                }
                std::string tray_type = normalize_filament_type(filament_type);
                std::string setting_id = build_setting_id(slot, tray_type);

                std::string color    = "FFFFFFFF";
                auto        color_it = dict.colors.find(slot.color_index);
                if (color_it != dict.colors.end()) {
                    color = normalize_color(color_it->second);
                }

                tray_json["tray_info_idx"] = setting_id;
                tray_json["tray_type"] = tray_type;
                tray_json["tray_color"] = color;
            } else {
                tray_json["tray_info_idx"] = "";
                tray_json["tray_type"] = "";
                tray_json["tray_color"] = "00000000";
            }

            tray_array.push_back(tray_json);
        }
        ams_unit["tray"] = tray_array;
        ams_array.push_back(ams_unit);
    }

    // Format as hex strings (matching BBL protocol)
    std::ostringstream ams_exist_ss;
    ams_exist_ss << std::hex << std::uppercase << ams_exist_bits;
    std::ostringstream tray_exist_ss;
    tray_exist_ss << std::hex << std::uppercase << tray_exist_bits;

    ams_json["ams"] = ams_array;
    ams_json["ams_exist_bits"] = ams_exist_ss.str();
    ams_json["tray_exist_bits"] = tray_exist_ss.str();

    // Wrap in the expected structure for ParseV1_0
    nlohmann::json print_json = nlohmann::json::object();
    print_json["ams"] = ams_json;

    // Call the parser to populate DevFilaSystem
    DevFilaSystemParser::ParseV1_0(print_json, obj, obj->GetFilaSystem(), false);

    BOOST_LOG_TRIVIAL(info) << "QidiPrinterAgent::fetch_filament_info: Populated DevFilaSystem with "
                            << box_count << " AMS units";
}

int QidiPrinterAgent::handle_request(const std::string& dev_id, const std::string& json_str)
{
    auto json = nlohmann::json::parse(json_str, nullptr, false);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent: Invalid JSON request";
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    }

    if (json.contains("info") && json["info"].contains("command")) {
        const auto& command = json["info"]["command"];
        if (command.is_string() && command.get<std::string>() == "get_version") {
            return send_version_info(dev_id);
        }
    }

    if (json.contains("system") && json["system"].contains("command")) {
        const auto& command = json["system"]["command"];
        if (command.is_string() && command.get<std::string>() == "get_access_code") {
            return send_access_code(dev_id);
        }
    }

    // if (json.contains("pushing") && json["pushing"].contains("command")) {
    //     const auto& command = json["pushing"]["command"];
    //     if (command.is_string()) {
    //         const auto cmd = command.get<std::string>();
    //         if (cmd == "pushall" || cmd == "start") {
    //             return sync_filament_list(dev_id);
    //         }
    //     }
    // }

    return BAMBU_NETWORK_SUCCESS;
}

bool QidiPrinterAgent::get_printhost_config(PrinthostConfig& config) const
{
    auto* preset_bundle = GUI::wxGetApp().preset_bundle;
    if (!preset_bundle) {
        return false;
    }

    auto& preset      = preset_bundle->printers.get_edited_preset();
    const auto& printer_cfg = preset.config;
    const DynamicPrintConfig* host_cfg = &printer_cfg;
    config.host = host_cfg->opt_string("print_host");
    if (config.host.empty()) {
        if (auto* physical_cfg = preset_bundle->physical_printers.get_selected_printer_config()) {
            if (!physical_cfg->opt_string("print_host").empty()) {
                host_cfg = physical_cfg;
                config.host = host_cfg->opt_string("print_host");
            }
        }
    }
    if (config.host.empty()) {
        return false;
    }

    config.port       = host_cfg->opt_string("printhost_port");
    config.api_key    = host_cfg->opt_string("printhost_apikey");
    config.model_id   = preset.get_printer_type(preset_bundle);
    config.model_name = printer_cfg.opt_string("printer_model");
    config.base_url   = normalize_base_url(config.host, config.port);

    return !config.base_url.empty();
}

bool QidiPrinterAgent::fetch_device_info(const std::string& base_url,
                                         const std::string& api_key,
                                         QidiDeviceInfo& info,
                                         std::string& error) const
{
    auto fetch_json = [&](const std::string& url, nlohmann::json& out) {
        std::string response_body;
        bool        success = false;
        std::string http_error;

        auto http = Http::get(url);
        if (!api_key.empty()) {
            http.header("X-Api-Key", api_key);
        }
        http.timeout_connect(10)
            .timeout_max(30)
            .on_complete([&](std::string body, unsigned status) {
                if (status == 200) {
                    response_body = body;
                    success       = true;
                } else {
                    http_error = "HTTP error: " + std::to_string(status);
                }
            })
            .on_error([&](std::string body, std::string err, unsigned status) {
                http_error = err;
                if (status > 0) {
                    http_error += " (HTTP " + std::to_string(status) + ")";
                }
            })
            .perform_sync();

        if (!success) {
            error = http_error.empty() ? "Connection failed" : http_error;
            return false;
        }

        out = nlohmann::json::parse(response_body, nullptr, false, true);
        if (out.is_discarded()) {
            error = "Invalid JSON response";
            return false;
        }
        return true;
    };

    nlohmann::json json;
    std::string    url = join_url(base_url, "/machine/device_info");
    if (!fetch_json(url, json)) {
        url = join_url(base_url, "/printer/info");
        if (!fetch_json(url, json)) {
            return false;
        }
    }

    nlohmann::json result = json.contains("result") ? json["result"] : json;
    info.dev_name         = result.value("machine_name", result.value("hostname", ""));
    info.dev_id           = result.value("machine_uuid", "");
    if (info.dev_id.empty()) {
        info.dev_id = result.value("serial_number", "");
    }
    info.model_id = result.value("model", "");
    info.version  = result.value("software_version", result.value("firmware_version", ""));

    return true;
}

bool QidiPrinterAgent::fetch_server_info(const std::string& base_url,
                                         const std::string& api_key,
                                         std::string& version,
                                         std::string& error) const
{
    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(join_url(base_url, "/server/info"));
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    nlohmann::json result = json.contains("result") ? json["result"] : json;
    if (result.contains("moonraker_version") && result["moonraker_version"].is_string()) {
        version = result["moonraker_version"].get<std::string>();
    } else if (result.contains("version") && result["version"].is_string()) {
        version = result["version"].get<std::string>();
    }

    return true;
}

bool QidiPrinterAgent::fetch_object_list(const std::string& base_url,
                                         const std::string& api_key,
                                         std::set<std::string>& objects,
                                         std::string& error) const
{
    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(join_url(base_url, "/printer/objects/list"));
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    nlohmann::json result = json.contains("result") ? json["result"] : json;
    if (!result.contains("objects") || !result["objects"].is_array()) {
        error = "Unexpected JSON structure";
        return false;
    }

    objects.clear();
    for (const auto& entry : result["objects"]) {
        if (entry.is_string()) {
            objects.insert(entry.get<std::string>());
        }
    }

    return !objects.empty();
}

int QidiPrinterAgent::send_version_info(const std::string& dev_id)
{
    const std::string base_url = resolve_host(dev_id);
    if (base_url.empty()) {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    const std::string api_key = resolve_api_key(dev_id, "");

    std::string version;
    std::string error;
    if (!fetch_server_info(base_url, api_key, version, error)) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent: Failed to fetch server info: " << error;
    }
    if (version.empty()) {
        version = "moonraker";
    }

    nlohmann::json payload;
    payload["info"]["command"] = "get_version";
    payload["info"]["result"]  = "success";
    payload["info"]["module"]  = nlohmann::json::array();

    nlohmann::json module;
    module["name"]         = "ota";
    module["sw_ver"]       = version;
    module["product_name"] = "Moonraker";
    payload["info"]["module"].push_back(module);

    dispatch_message(dev_id, payload.dump());
    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::send_access_code(const std::string& dev_id)
{
    nlohmann::json payload;
    payload["system"]["command"] = "get_access_code";
    payload["system"]["access_code"] = resolve_api_key(dev_id, "");
    dispatch_message(dev_id, payload.dump());
    return BAMBU_NETWORK_SUCCESS;
}

void QidiPrinterAgent::announce_printhost_device()
{
    PrinthostConfig config;
    if (!get_printhost_config(config)) {
        return;
    }

    const std::string base_url = config.base_url;
    if (base_url.empty()) {
        return;
    }

    OnMsgArrivedFn ssdp_fn;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        ssdp_fn = on_ssdp_msg_fn;
        if (!ssdp_fn) {
            return;
        }
        if (ssdp_announced_host == base_url && !ssdp_announced_id.empty()) {
            return;
        }
    }

    const std::string dev_id   = extract_host(base_url);
    const std::string dev_name = config.model_name.empty() ? "Qidi Printer" : config.model_name;
    const std::string model_id = config.model_id;

    if (auto* app_config = GUI::wxGetApp().app_config) {
        const std::string access_code = normalize_api_key(config.api_key).empty() ? k_no_api_key : config.api_key;
        app_config->set_str("access_code", dev_id, access_code);
        app_config->set_str("user_access_code", dev_id, access_code);
    }

    store_host(dev_id, base_url, normalize_api_key(config.api_key));

    nlohmann::json payload;
    payload["dev_name"]     = dev_name;
    payload["dev_id"]       = dev_id;
    payload["dev_ip"]       = extract_host(base_url);
    payload["dev_type"]     = model_id.empty() ? dev_name : model_id;
    payload["dev_signal"]   = "0";
    payload["connect_type"] = "lan";
    payload["bind_state"]   = "free";
    payload["sec_link"]     = "secure";
    payload["ssdp_version"] = "v1";

    ssdp_fn(payload.dump());

    std::lock_guard<std::mutex> lock(state_mutex);
    ssdp_announced_host = base_url;
    ssdp_announced_id   = dev_id;
}

void QidiPrinterAgent::dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg)
{
    OnLocalConnectedFn local_fn;
    QueueOnMainFn      queue_fn;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        local_fn = on_local_connect_fn;
        queue_fn = queue_on_main_fn;
    }
    if (!local_fn) {
        return;
    }

    auto dispatch = [state, dev_id, msg, local_fn]() { local_fn(state, dev_id, msg); };
    if (queue_fn) {
        queue_fn(dispatch);
    } else {
        dispatch();
    }
}

void QidiPrinterAgent::dispatch_printer_connected(const std::string& dev_id)
{
    OnPrinterConnectedFn connected_fn;
    QueueOnMainFn        queue_fn;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        connected_fn = on_printer_connected_fn;
        queue_fn     = queue_on_main_fn;
    }
    if (!connected_fn) {
        return;
    }

    auto dispatch = [dev_id, connected_fn]() { connected_fn(dev_id); };
    if (queue_fn) {
        queue_fn(dispatch);
    } else {
        dispatch();
    }
}

void QidiPrinterAgent::start_status_stream(const std::string& dev_id, const std::string& base_url, const std::string& api_key)
{
    stop_status_stream();
    if (base_url.empty()) {
        return;
    }

    ws_stop.store(false);
    ws_thread = std::thread([this, dev_id, base_url, api_key]() {
        run_status_stream(dev_id, base_url, api_key);
    });
}

void QidiPrinterAgent::stop_status_stream()
{
    ws_stop.store(true);
    if (ws_thread.joinable()) {
        ws_thread.join();
    }
}

void QidiPrinterAgent::run_status_stream(std::string dev_id, std::string base_url, std::string api_key)
{
    WsEndpoint endpoint;
    if (!parse_ws_endpoint(base_url, endpoint)) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent: websocket endpoint invalid for base_url=" << base_url;
        return;
    }
    if (endpoint.secure) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent: websocket wss not supported for base_url=" << base_url;
        return;
    }

    try {
        net::io_context ioc;
        tcp::resolver   resolver{ioc};
        beast::tcp_stream stream{ioc};

        stream.expires_after(std::chrono::seconds(10));
        auto const results = resolver.resolve(endpoint.host, endpoint.port);
        stream.connect(results);

        websocket::stream<beast::tcp_stream> ws{std::move(stream)};
        ws.set_option(websocket::stream_base::decorator([&](websocket::request_type& req) {
            req.set(http::field::user_agent, "OrcaSlicer");
            if (!api_key.empty()) {
                req.set("X-Api-Key", api_key);
            }
        }));

        std::string host_header = endpoint.host;
        if (!endpoint.port.empty() && endpoint.port != "80") {
            host_header += ":" + endpoint.port;
        }
        ws.handshake(host_header, endpoint.target);
        ws.text(true);

        std::set<std::string> subscribe_objects = {"print_stats", "virtual_sdcard"};
        std::set<std::string> available_objects;
        std::string           list_error;
        if (fetch_object_list(base_url, api_key, available_objects, list_error)) {
            if (available_objects.count("heater_bed") != 0) {
                subscribe_objects.insert("heater_bed");
            }
            if (available_objects.count("fan") != 0) {
                subscribe_objects.insert("fan");
            }

            for (const auto& name : available_objects) {
                if (name == "extruder" || name.rfind("extruder", 0) == 0) {
                    subscribe_objects.insert(name);
                    if (name == "extruder") {
                        break;
                    }
                }
            }
        } else {
            BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent: object list unavailable: " << list_error;
            subscribe_objects.insert("extruder");
            subscribe_objects.insert("heater_bed");
            subscribe_objects.insert("fan");
        }

        nlohmann::json subscribe;
        subscribe["jsonrpc"] = "2.0";
        subscribe["method"]  = "printer.objects.subscribe";
        nlohmann::json objects = nlohmann::json::object();
        for (const auto& name : subscribe_objects) {
            objects[name] = nullptr;
        }
        subscribe["params"]["objects"] = std::move(objects);
        subscribe["id"] = 1;
        ws.write(net::buffer(subscribe.dump()));

        while (!ws_stop.load()) {
            ws.next_layer().expires_after(std::chrono::seconds(2));
            beast::flat_buffer buffer;
            beast::error_code  ec;
            ws.read(buffer, ec);
            if (ec == beast::error::timeout) {
                const auto now_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                const auto last_ms = ws_last_emit_ms.load();
                if (last_ms == 0 || now_ms - last_ms >= 10000) {
                    nlohmann::json message;
                    {
                        std::lock_guard<std::mutex> lock(payload_mutex);
                        message = build_print_payload_locked(nullptr);
                    }
                    dispatch_message(dev_id, message.dump());
                    ws_last_emit_ms.store(now_ms);
                }
                continue;
            }
            if (ec == websocket::error::closed) {
                break;
            }
            if (ec) {
                BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent: websocket read error: " << ec.message();
                break;
            }
            handle_ws_message(dev_id, beast::buffers_to_string(buffer.data()));
        }

        beast::error_code ec;
        ws.close(websocket::close_code::normal, ec);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent: websocket exception: " << e.what();
    }
}

void QidiPrinterAgent::handle_ws_message(const std::string& dev_id, const std::string& payload)
{
    auto json = nlohmann::json::parse(payload, nullptr, false);
    if (json.is_discarded()) {
        return;
    }

    bool updated = false;
    if (json.contains("result") && json["result"].contains("status") &&
        json["result"]["status"].is_object()) {
        update_status_cache(json["result"]["status"]);
        updated = true;
    }

    if (json.contains("method") && json["method"].is_string()) {
        const std::string method = json["method"].get<std::string>();
        if (method == "notify_status_update" && json.contains("params") &&
            json["params"].is_array() && !json["params"].empty() &&
            json["params"][0].is_object()) {
            update_status_cache(json["params"][0]);
            updated = true;
        } else if (method == "notify_klippy_ready") {
            nlohmann::json updates;
            updates["print_stats"]["state"] = "standby";
            update_status_cache(updates);
            updated = true;
        } else if (method == "notify_klippy_shutdown") {
            nlohmann::json updates;
            updates["print_stats"]["state"] = "error";
            update_status_cache(updates);
            updated = true;
        }
    }

    if (updated) {
        nlohmann::json message;
        {
            std::lock_guard<std::mutex> lock(payload_mutex);
            message = build_print_payload_locked(nullptr);
        }
        dispatch_message(dev_id, message.dump());
        const auto now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        ws_last_emit_ms.store(now_ms);
    }
}

void QidiPrinterAgent::update_status_cache(const nlohmann::json& updates)
{
    if (!updates.is_object()) {
        return;
    }

    std::lock_guard<std::mutex> lock(payload_mutex);
    if (!status_cache.is_object()) {
        status_cache = nlohmann::json::object();
    }

    for (const auto& item : updates.items()) {
        if (item.value().is_object()) {
            nlohmann::json& target = status_cache[item.key()];
            if (!target.is_object()) {
                target = nlohmann::json::object();
            }
            for (const auto& field : item.value().items()) {
                target[field.key()] = field.value();
            }
        } else {
            status_cache[item.key()] = item.value();
        }
    }
}

nlohmann::json QidiPrinterAgent::build_print_payload_locked(const nlohmann::json* ams_override) const
{
    nlohmann::json payload;
    payload["print"]["command"]            = "push_status";
    payload["print"]["msg"]                = 0;
    payload["print"]["support_mqtt_alive"] = true;

    if (ams_override) {
        payload["print"]["ams"] = *ams_override;
    } else if (!last_ams_payload.is_null()) {
        payload["print"]["ams"] = last_ams_payload;
    }

    std::string state = "IDLE";
    if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("state") &&
        status_cache["print_stats"]["state"].is_string()) {
        state = map_moonraker_state(status_cache["print_stats"]["state"].get<std::string>());
    }
    payload["print"]["gcode_state"] = state;

    const nlohmann::json* extruder = nullptr;
    if (status_cache.contains("extruder") && status_cache["extruder"].is_object()) {
        extruder = &status_cache["extruder"];
    } else {
        for (const auto& item : status_cache.items()) {
            if (item.value().is_object() && item.key().rfind("extruder", 0) == 0) {
                extruder = &item.value();
                break;
            }
        }
    }

    if (extruder) {
        if (extruder->contains("temperature") && (*extruder)["temperature"].is_number()) {
            payload["print"]["nozzle_temper"] = (*extruder)["temperature"].get<float>();
        }
        if (extruder->contains("target") && (*extruder)["target"].is_number()) {
            payload["print"]["nozzle_target_temper"] = (*extruder)["target"].get<float>();
        }
    }

    if (status_cache.contains("heater_bed") && status_cache["heater_bed"].is_object()) {
        const auto& bed = status_cache["heater_bed"];
        if (bed.contains("temperature") && bed["temperature"].is_number()) {
            payload["print"]["bed_temper"] = bed["temperature"].get<float>();
        }
        if (bed.contains("target") && bed["target"].is_number()) {
            payload["print"]["bed_target_temper"] = bed["target"].get<float>();
        }
    }

    if (status_cache.contains("fan") && status_cache["fan"].is_object()) {
        const auto& fan = status_cache["fan"];
        if (fan.contains("speed") && fan["speed"].is_number()) {
            double speed = fan["speed"].get<double>();
            int    pwm   = 0;
            if (speed <= 1.0) {
                pwm = static_cast<int>(speed * 255.0 + 0.5);
            } else {
                pwm = static_cast<int>(speed + 0.5);
            }
            pwm = std::clamp(pwm, 0, 255);
            payload["print"]["fan_gear"] = pwm;
        }
    }

    if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("filename") &&
        status_cache["print_stats"]["filename"].is_string()) {
        payload["print"]["subtask_name"] = status_cache["print_stats"]["filename"].get<std::string>();
    }

    int mc_percent = -1;
    if (status_cache.contains("virtual_sdcard") &&
        status_cache["virtual_sdcard"].contains("progress") &&
        status_cache["virtual_sdcard"]["progress"].is_number()) {
        const double progress = status_cache["virtual_sdcard"]["progress"].get<double>();
        if (progress >= 0.0) {
            mc_percent = std::clamp(static_cast<int>(progress * 100.0 + 0.5), 0, 100);
        }
    }
    if (mc_percent >= 0) {
        payload["print"]["mc_percent"] = mc_percent;
    }

    if (status_cache.contains("print_stats") &&
        status_cache["print_stats"].contains("total_duration") &&
        status_cache["print_stats"].contains("print_duration") &&
        status_cache["print_stats"]["total_duration"].is_number() &&
        status_cache["print_stats"]["print_duration"].is_number()) {
        const double total   = status_cache["print_stats"]["total_duration"].get<double>();
        const double elapsed = status_cache["print_stats"]["print_duration"].get<double>();
        if (total > 0.0 && elapsed >= 0.0) {
            const auto remaining_minutes = std::max(0, static_cast<int>((total - elapsed) / 60.0));
            payload["print"]["mc_remaining_time"] = remaining_minutes;
        }
    }

    const auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    payload["t_utc"] = now_ms;

    return payload;
}

std::string QidiPrinterAgent::resolve_host(const std::string& dev_id) const
{
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto                        it = host_by_device.find(dev_id);
        if (it != host_by_device.end()) {
            return it->second;
        }
    }

    PrinthostConfig config;
    if (get_printhost_config(config)) {
        return config.base_url;
    }

    return looks_like_host(dev_id) ? normalize_base_url(dev_id, "") : "";
}

std::string QidiPrinterAgent::resolve_api_key(const std::string& dev_id, const std::string& fallback) const
{
    std::string api_key = normalize_api_key(fallback);
    if (!api_key.empty()) {
        return api_key;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto                        it = api_key_by_device.find(dev_id);
        if (it != api_key_by_device.end() && !it->second.empty()) {
            return it->second;
        }
    }

    PrinthostConfig config;
    if (get_printhost_config(config)) {
        return normalize_api_key(config.api_key);
    }

    return "";
}

void QidiPrinterAgent::store_host(const std::string& dev_id, const std::string& host, const std::string& api_key)
{
    if (host.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    host_by_device[dev_id] = host;
    if (!api_key.empty()) {
        api_key_by_device[dev_id] = api_key;
    }
}

bool QidiPrinterAgent::fetch_slot_info(const std::string& base_url,
                                       const std::string& api_key,
                                       std::vector<QidiSlotInfo>& slots,
                                       int&                       box_count,
                                       std::string&               error) const
{
    std::string url = join_url(base_url, "/printer/objects/query?save_variables=variables");
    for (int i = 0; i < 16; ++i) {
        url += "&box_stepper%20slot" + std::to_string(i) + "=runout_button";
    }

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    if (!json.contains("result") || !json["result"].contains("status") || !json["result"]["status"].contains("save_variables") ||
        !json["result"]["status"]["save_variables"].contains("variables")) {
        error = "Unexpected JSON structure";
        return false;
    }

    auto& variables = json["result"]["status"]["save_variables"]["variables"];
    auto& status    = json["result"]["status"];

    box_count = variables.value("box_count", 1);
    if (box_count < 0) {
        box_count = 0;
    }

    const int max_slots = box_count * 4;
    slots.clear();
    slots.reserve(max_slots);

    for (int i = 0; i < max_slots; ++i) {
        QidiSlotInfo slot;
        slot.slot_index    = i;
        slot.color_index   = variables.value("color_slot" + std::to_string(i), 1);
        slot.filament_type = variables.value("filament_slot" + std::to_string(i), 1);
        slot.vendor_type   = variables.value("vendor_slot" + std::to_string(i), 0);

        std::string box_stepper_key = "box_stepper slot" + std::to_string(i);
        slot.filament_exists        = false;
        if (status.contains(box_stepper_key)) {
            auto& box_stepper = status[box_stepper_key];
            if (box_stepper.contains("runout_button") && !box_stepper["runout_button"].is_null()) {
                int runout_button    = box_stepper["runout_button"].get<int>();
                slot.filament_exists = (runout_button == 0);
            }
        }
        slots.push_back(slot);
    }

    return true;
}

void QidiPrinterAgent::parse_ini_section(const std::string& content, const std::string& section_name, std::map<int, std::string>& result)
{
    std::istringstream stream(content);
    std::string        line;
    bool               in_section     = false;
    std::string        section_header = "[" + section_name + "]";

    while (std::getline(stream, line)) {
        boost::trim(line);
        if (!line.empty() && line[0] == '[') {
            in_section = (line == section_header);
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (in_section) {
            auto pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key   = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                boost::trim(key);
                boost::trim(value);
                try {
                    int index     = std::stoi(key);
                    result[index] = value;
                } catch (...) {}
            }
        }
    }
}

void QidiPrinterAgent::parse_filament_sections(const std::string& content, std::map<int, std::string>& result)
{
    std::istringstream stream(content);
    std::string        line;
    int                current_fila_index = -1;

    while (std::getline(stream, line)) {
        boost::trim(line);
        if (!line.empty() && line[0] == '[') {
            current_fila_index = -1;
            if (line.size() > 5 && line.substr(0, 5) == "[fila" && line.back() == ']') {
                std::string num_str = line.substr(5, line.size() - 6);
                try {
                    current_fila_index = std::stoi(num_str);
                } catch (...) {
                    current_fila_index = -1;
                }
            }
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (current_fila_index > 0) {
            auto pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key   = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                boost::trim(key);
                boost::trim(value);
                if (key == "filament") {
                    result[current_fila_index] = value;
                }
            }
        }
    }
}

bool QidiPrinterAgent::fetch_filament_dict(const std::string& base_url,
                                           const std::string& api_key,
                                           QidiFilamentDict& dict,
                                           std::string& error) const
{
    std::string url = join_url(base_url, "/server/files/config/officiall_filas_list.cfg");

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    dict.colors.clear();
    dict.filaments.clear();
    parse_ini_section(response_body, "colordict", dict.colors);
    parse_filament_sections(response_body, dict.filaments);

    return !dict.colors.empty();
}

std::string QidiPrinterAgent::normalize_color(const std::string& color)
{
    std::string value = color;
    boost::trim(value);
    if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) {
        value = value.substr(2);
    }
    if (!value.empty() && value[0] == '#') {
        value = value.substr(1);
    }
    std::string normalized;
    for (char c : value) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    if (normalized.size() == 6) {
        normalized += "FF";
    }
    if (normalized.size() != 8) {
        return "00000000";
    }
    return normalized;
}

std::string QidiPrinterAgent::map_filament_type_to_setting_id(const std::string& filament_type)
{
    std::string upper = filament_type;
    boost::trim(upper);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    if (upper == "PLA") {
        return "QD_1_0_1";
    }
    if (upper == "ABS") {
        return "QD_1_0_11";
    }
    if (upper == "PETG") {
        return "QD_1_0_41";
    }
    if (upper == "TPU") {
        return "QD_1_0_50";
    }
    return "";
}

void QidiPrinterAgent::dispatch_message(const std::string& dev_id, const std::string& payload)
{
    OnMessageFn   local_fn;
    OnMessageFn   cloud_fn;
    QueueOnMainFn queue_fn;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        local_fn = on_local_message_fn;
        cloud_fn = on_message_fn;
        queue_fn = queue_on_main_fn;
    }

    auto dispatch = [dev_id, payload, local_fn, cloud_fn]() {
        if (local_fn) {
            local_fn(dev_id, payload);
            return;
        }
        if (cloud_fn) {
            cloud_fn(dev_id, payload);
        }
    };

    if (queue_fn) {
        queue_fn(dispatch);
    } else {
        dispatch();
    }
}

} // namespace Slic3r
