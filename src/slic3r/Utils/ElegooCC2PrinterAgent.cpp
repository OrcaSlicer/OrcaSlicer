#include "ElegooCC2PrinterAgent.hpp"

#include "FilamentSync.hpp"
#include "Http.hpp"
#include "IPrinterAgent.hpp"
#include "MqttWebSocketClient.hpp"
#include "NetworkAgentFactory.hpp"
#include "bambu_networking.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

constexpr char cc2_model_id[]      = "Elegoo-CC2";
constexpr char cc2_mqtt_port[]     = "9001";
constexpr char cc2_default_token[] = "123456"; // What ElegooLink sends when the access code is empty
constexpr int canvas_request_id    = 1;
constexpr int canvas_method        = 2005;

std::optional<int> read_int_value(const nlohmann::json& value)
{
    if (!value.is_number_integer())
        return std::nullopt;
    if (value.is_number_unsigned()) {
        const auto v = value.get<std::uint64_t>();
        if (v > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            return std::nullopt;
        return static_cast<int>(v);
    }
    const auto v = value.get<std::int64_t>();
    if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max())
        return std::nullopt;
    return static_cast<int>(v);
}

std::optional<int> read_int(const nlohmann::json& obj, const char* key)
{
    const auto it = obj.find(key);
    return it == obj.end() ? std::nullopt : read_int_value(*it);
}

std::optional<std::string> read_string(const nlohmann::json& obj, const char* key)
{
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string())
        return std::nullopt;
    return it->get<std::string>();
}

bool is_valid_serial(const std::string& sn)
{
    if (sn.empty() || sn.size() > 64)
        return false;
    return std::all_of(sn.begin(), sn.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

// Explicit ASCII ranges: std::isalnum would also accept letters above 0x7F in a single-byte locale.
bool is_host_char(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-'; }

bool is_digits(const std::string& s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string read_config_string(const DynamicPrintConfig& cfg, const char* key)
{
    const auto* option = cfg.opt<ConfigOptionString>(key);
    return option ? option->value : std::string();
}

// ASCII only: the <cctype> functions follow the C locale and would rewrite bytes of UTF-8 text in a single-byte one.
std::string normalize_type(const std::string& raw)
{
    constexpr char whitespace[] = " \t\n\v\f\r";
    const auto first            = raw.find_first_not_of(whitespace);
    if (first == std::string::npos)
        return {};
    std::string out = raw.substr(first, raw.find_last_not_of(whitespace) - first + 1);
    for (char& c : out)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return out;
}

std::string make_client_id()
{
    std::random_device random;
    std::uniform_int_distribution<int> digit(0, 15);
    std::string id = "orca";
    for (int i = 0; i < 6; ++i)
        id += "0123456789abcdef"[digit(random)];
    return id;
}

std::string mask_serial(const std::string& sn) { return sn.size() > 2 ? "***" + sn.substr(sn.size() - 2) : "***"; }

const char* error_name(ElegooCC2PrinterAgent::Error error)
{
    using Error = ElegooCC2PrinterAgent::Error;
    switch (error) {
    case Error::None: return "None";
    case Error::Unauthorized: return "Unauthorized";
    case Error::HttpError: return "HttpError";
    case Error::BadSystemInfo: return "BadSystemInfo";
    case Error::NotCC2: return "NotCC2";
    case Error::ConnectFailed: return "ConnectFailed";
    case Error::Timeout: return "Timeout";
    case Error::MqttRejected: return "MqttRejected";
    case Error::SubscribeRejected: return "SubscribeRejected";
    case Error::RegisterRejected: return "RegisterRejected";
    case Error::TooManyClients: return "TooManyClients";
    case Error::BadResponse: return "BadResponse";
    case Error::NoCanvas: return "NoCanvas";
    case Error::NoTarget: return "NoTarget";
    case Error::Exception: return "Exception";
    }
    return "Unknown";
}

ElegooCC2PrinterAgent::Error to_error(MqttWebSocketClient::Status status)
{
    using Error  = ElegooCC2PrinterAgent::Error;
    using Status = MqttWebSocketClient::Status;
    switch (status) {
    case Status::Ok: return Error::None;
    case Status::Timeout: return Error::Timeout;
    case Status::ConnackRejected: return Error::MqttRejected;
    case Status::SubscribeRejected: return Error::SubscribeRejected;
    case Status::ProtocolError: return Error::BadResponse;
    case Status::ConnectFailed:
    case Status::HandshakeFailed:
    case Status::Closed: return Error::ConnectFailed;
    }
    return Error::ConnectFailed;
}

} // namespace

ElegooCC2PrinterAgent::Error ElegooCC2PrinterAgent::parse_system_info(unsigned http_status, const std::string& body, SystemInfo& out)
{
    out = {};
    // Http reports status 0 when curl got no HTTP answer: refused, unreachable or timed out.
    if (http_status == 0)
        return Error::ConnectFailed;
    if (http_status == 401)
        return Error::Unauthorized;
    if (http_status != 200)
        return Error::HttpError;

    const nlohmann::json doc = nlohmann::json::parse(body, nullptr, false);
    if (doc.is_discarded() || !doc.is_object())
        return Error::BadSystemInfo;

    const auto error_code = read_int(doc, "error_code");
    if (!error_code.has_value() || *error_code != 0)
        return Error::BadSystemInfo;

    const auto info = doc.find("system_info");
    if (info == doc.end() || !info->is_object())
        return Error::BadSystemInfo;

    const auto model = read_string(*info, "machine_model");
    if (!model.has_value())
        return Error::BadSystemInfo;
    if (*model != "Centauri Carbon 2")
        return Error::NotCC2;

    const auto sn = read_string(*info, "sn");
    if (!sn.has_value() || !is_valid_serial(*sn))
        return Error::BadSystemInfo;

    out.sn            = *sn;
    out.machine_model = *model;
    return Error::None;
}

ElegooCC2PrinterAgent::Error ElegooCC2PrinterAgent::parse_canvas_info(const nlohmann::json& result, std::vector<CanvasUnit>& out)
{
    out.clear();
    if (!result.is_object())
        return Error::BadResponse;

    const auto error_code = read_int(result, "error_code");
    if (!error_code.has_value() || *error_code != 0)
        return Error::BadResponse;

    const auto canvas_info = result.find("canvas_info");
    if (canvas_info == result.end() || !canvas_info->is_object())
        return Error::BadResponse;

    const auto canvas_list = canvas_info->find("canvas_list");
    if (canvas_list == canvas_info->end() || !canvas_list->is_array())
        return Error::BadResponse;

    std::vector<CanvasUnit> units;
    for (const auto& unit_json : *canvas_list) {
        if (!unit_json.is_object())
            return Error::BadResponse;

        // Only these two states are known; anything else would make the snapshot silently drop the unit.
        const auto connected = read_int(unit_json, "connected");
        if (connected != 0 && connected != 1)
            return Error::BadResponse;
        if (connected == 0)
            continue;

        const auto tray_list = unit_json.find("tray_list");
        if (tray_list == unit_json.end() || !tray_list->is_array() || tray_list->size() != 4)
            return Error::BadResponse;

        CanvasUnit unit{};
        std::array<bool, 4> seen{};
        for (const auto& tray_json : *tray_list) {
            if (!tray_json.is_object())
                return Error::BadResponse;

            const auto tray_id = read_int(tray_json, "tray_id");
            if (!tray_id.has_value() || *tray_id < 0 || *tray_id >= 4 || seen[static_cast<std::size_t>(*tray_id)])
                return Error::BadResponse;

            const auto status = read_int(tray_json, "status");
            if (!status.has_value() || *status < 0 || *status > 2)
                return Error::BadResponse;

            CanvasTray tray;
            tray.status          = *status;
            tray.brand           = read_string(tray_json, "brand").value_or("");
            tray.name            = read_string(tray_json, "filament_name").value_or("");
            tray.type            = read_string(tray_json, "filament_type").value_or("");
            tray.color           = read_string(tray_json, "filament_color").value_or("");
            tray.max_nozzle_temp = read_int(tray_json, "max_nozzle_temp").value_or(0);

            const auto idx = static_cast<std::size_t>(*tray_id);
            seen[idx]      = true;
            unit[idx]      = std::move(tray);
        }
        units.push_back(std::move(unit));
    }

    if (units.empty())
        return Error::NoCanvas;
    // The AMS document builder refuses more units than ParseV1_0 can read; reject them here as a bad answer.
    if (units.size() > FilamentSync::max_ams_units)
        return Error::BadResponse;

    out = std::move(units);
    return Error::None;
}

std::vector<std::array<FilamentSync::AmsSlot, 4>> ElegooCC2PrinterAgent::to_ams_slots(const std::vector<CanvasUnit>& units,
                                                                                      const PresetCollection& filaments)
{
    std::vector<std::array<FilamentSync::AmsSlot, 4>> out;
    out.reserve(units.size());
    for (const CanvasUnit& unit : units) {
        std::array<FilamentSync::AmsSlot, 4> slots{};
        for (std::size_t i = 0; i < unit.size(); ++i) {
            const CanvasTray& tray = unit[i];
            if (tray.status != 1 && tray.status != 2)
                continue;

            FilamentSync::AmsSlot& slot = slots[i];
            slot.present                = true;
            slot.type                   = normalize_type(tray.type);
            if (const auto color = FilamentSync::parse_hex_rgb(tray.color))
                slot.color_rgba = *color;
            if (!slot.type.empty() && !slot.color_rgba.empty())
                slot.filament_id = FilamentSync::match_filament_preset(filaments, {tray.brand, tray.name, slot.type});
            if (tray.max_nozzle_temp > 0)
                slot.nozzle_temp_max = tray.max_nozzle_temp;
        }
        out.push_back(std::move(slots));
    }
    return out;
}

std::optional<std::string> ElegooCC2PrinterAgent::printer_model_id(Preset& printer, PresetBundle* preset_bundle)
{
    // The preset getter reads printer_model unchecked, so a preset without it must not reach the getter.
    if (!printer.config.opt<ConfigOptionString>("printer_model"))
        return std::nullopt;
    return printer.get_current_printer_type(preset_bundle);
}

std::optional<ElegooCC2PrinterAgent::Target> ElegooCC2PrinterAgent::resolve_target(const DynamicPrintConfig& printer_cfg,
                                                                                   const std::string& model_id,
                                                                                   const std::string& dev_id)
{
    if (model_id != cc2_model_id)
        return std::nullopt;
    const std::string print_host = read_config_string(printer_cfg, "print_host");
    const std::string port       = read_config_string(printer_cfg, "printhost_port");
    // Checked before the path is stripped, which would hide a query, a fragment or userinfo after it. The agent speaks only
    // plain HTTP and WebSocket, so a secure address is refused rather than sent the token unencrypted.
    if (print_host.empty() || print_host.find_first_of("?#@") != std::string::npos || print_host.rfind("https://", 0) == 0 ||
        dev_id != MachineObject::dev_id_from_address(print_host, port))
        return std::nullopt;

    // Without printhost_port the address keeps a port written into print_host.
    Target target{dev_id, MachineObject::dev_id_from_address(print_host), port, read_config_string(printer_cfg, "printhost_apikey")};
    if (const auto colon = target.host.find(':'); colon != std::string::npos) {
        const std::string inline_port = target.host.substr(colon + 1);
        if (!is_digits(inline_port))
            return std::nullopt;
        if (target.http_port.empty())
            target.http_port = inline_port;
        target.host.erase(colon);
    }
    // Only an IPv4 address or a DNS name, so nothing else reaches the URL or the logs; IPv6 is not supported yet.
    if (target.host.empty() || !std::all_of(target.host.begin(), target.host.end(), is_host_char) ||
        (!target.http_port.empty() && !is_digits(target.http_port)))
        return std::nullopt;
    if (target.token.empty())
        target.token = cc2_default_token;
    return target;
}

ElegooCC2PrinterAgent::Error ElegooCC2PrinterAgent::query_canvas(const std::string& host,
                                                                 const std::string& mqtt_port,
                                                                 const std::string& sn,
                                                                 const std::string& token,
                                                                 const std::string& client_id,
                                                                 std::chrono::steady_clock::time_point deadline,
                                                                 nlohmann::json& canvas_result)
{
    using Status                    = MqttWebSocketClient::Status;
    canvas_result                   = nullptr;
    const std::string device_topic  = "elegoo/" + sn + "/";
    const std::string session_topic = device_topic + client_id + "/";

    MqttWebSocketClient client(deadline);
    const Error error = [&] {
        std::uint8_t connack_code = 0;
        Status status             = client.connect(host, mqtt_port, client_id, "elegoo", token, connack_code);
        if (status == Status::Ok)
            status = client.subscribe({session_topic + "register_response", session_topic + "api_response"});
        if (status == Status::Ok)
            status = client.publish(device_topic + "api_register",
                                    nlohmann::json{{"client_id", client_id}, {"request_id", client_id}}.dump());
        std::string payload;
        if (status == Status::Ok)
            status = client.wait_for([&](const std::string& topic,
                                         const std::string&) { return topic == session_topic + "register_response"; },
                                     payload);
        if (status != Status::Ok)
            return to_error(status);

        const std::string answer = read_string(nlohmann::json::parse(payload, nullptr, false), "error").value_or("");
        if (answer == "too many clients")
            return Error::TooManyClients;
        if (answer != "ok" && answer != "already registered")
            return Error::RegisterRejected;

        status = client.publish(session_topic + "api_request", nlohmann::json{{"id", canvas_request_id}, {"method", canvas_method}}.dump());
        if (status == Status::Ok)
            status = client.wait_for(
                [&](const std::string& topic, const std::string& body) {
                    if (topic != session_topic + "api_response")
                        return false;
                    const nlohmann::json doc = nlohmann::json::parse(body, nullptr, false);
                    return read_int(doc, "id") == canvas_request_id && read_int(doc, "method") == canvas_method;
                },
                payload);
        if (status != Status::Ok)
            return to_error(status);

        nlohmann::json doc = nlohmann::json::parse(payload, nullptr, false);
        const auto result  = doc.find("result");
        if (result == doc.end())
            return Error::BadResponse;
        canvas_result = std::move(*result);
        return Error::None;
    }();
    client.close();
    return error;
}

void ElegooCC2PrinterAgent::apply_snapshot(MachineObject& obj, const std::vector<std::array<FilamentSync::AmsSlot, 4>>& slots)
{
    DevFilaSystemParser::ParseV1_0(FilamentSync::build_ams_print_json(slots, "CANVAS"), &obj, obj.GetFilaSystem().get(), false);
    mark_snapshot_ready(obj);
}

void ElegooCC2PrinterAgent::mark_snapshot_ready(MachineObject& obj)
{
    obj.printer_type = cc2_model_id;
    obj.set_filament_snapshot_ready(true);
}

void ElegooCC2PrinterAgent::mark_snapshot_failed(MachineObject& obj) { obj.set_filament_snapshot_ready(false); }

ElegooCC2PrinterAgent::ElegooCC2PrinterAgent(std::string /*log_dir*/) : m_client_id(make_client_id()) {}

AgentInfo ElegooCC2PrinterAgent::get_agent_info_static()
{ return AgentInfo{ELEGOO_CC2_PRINTER_AGENT_ID, "Elegoo CC2", "0.1.0", "Elegoo Centauri Carbon 2 CANVAS filament sync"}; }

// The filament read needs no cloud, callback or connection state: it opens its own short session when asked.
void ElegooCC2PrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent>) {}

int ElegooCC2PrinterAgent::send_message(std::string, std::string, int, int) { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

int ElegooCC2PrinterAgent::connect_printer(const PrinterConnectionParams&) { return BAMBU_NETWORK_SUCCESS; }

int ElegooCC2PrinterAgent::disconnect_printer() { return BAMBU_NETWORK_SUCCESS; }

int ElegooCC2PrinterAgent::send_message_to_printer(std::string, std::string, int, int) { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

// No discovery: GUI_App::select_machine creates the machine from the preset's print_host.
bool ElegooCC2PrinterAgent::start_discovery(bool, bool) { return true; }

int ElegooCC2PrinterAgent::bind_detect(std::string, std::string, detectResult&) { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

int ElegooCC2PrinterAgent::set_server_callback(OnServerErrFn) { return BAMBU_NETWORK_SUCCESS; }

std::string ElegooCC2PrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_selected_machine;
}

int ElegooCC2PrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_selected_machine = std::move(dev_id);
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooCC2PrinterAgent::start_print(PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn)
{ return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

int ElegooCC2PrinterAgent::start_local_print_with_record(PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn)
{ return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

int ElegooCC2PrinterAgent::start_send_gcode_to_sdcard(PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn)
{ return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

int ElegooCC2PrinterAgent::start_local_print(PrintParams, OnUpdateStatusFn, WasCancelledFn) { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

int ElegooCC2PrinterAgent::start_sdcard_print(PrintParams, OnUpdateStatusFn, WasCancelledFn) { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

int ElegooCC2PrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn) { return BAMBU_NETWORK_SUCCESS; }

int ElegooCC2PrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn) { return BAMBU_NETWORK_SUCCESS; }

int ElegooCC2PrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn) { return BAMBU_NETWORK_SUCCESS; }

int ElegooCC2PrinterAgent::set_on_message_fn(OnMessageFn) { return BAMBU_NETWORK_SUCCESS; }

int ElegooCC2PrinterAgent::set_on_user_message_fn(OnMessageFn) { return BAMBU_NETWORK_SUCCESS; }

int ElegooCC2PrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn) { return BAMBU_NETWORK_SUCCESS; }

int ElegooCC2PrinterAgent::set_on_local_message_fn(OnMessageFn) { return BAMBU_NETWORK_SUCCESS; }

int ElegooCC2PrinterAgent::set_queue_on_main_fn(QueueOnMainFn) { return BAMBU_NETWORK_SUCCESS; }

bool ElegooCC2PrinterAgent::fetch_filament_info(std::string dev_id, FilamentSyncMode /*sync_mode*/)
{
    // One budget for the whole read, so the GUI thread waits at most this long.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);

    MachineObject* obj = nullptr;
    std::string host;
    SystemInfo info;
    Error error = Error::None;
    try {
        error = [&] {
            GUI::GUI_App& app             = GUI::wxGetApp();
            PresetBundle* preset_bundle   = app.preset_bundle;
            DeviceManager* device_manager = app.getDeviceManager();
            if (device_manager)
                obj = device_manager->get_my_machine(dev_id);
            if (!preset_bundle || !obj)
                return Error::NoTarget;

            Preset& printer     = preset_bundle->printers.get_edited_preset();
            const auto model_id = printer_model_id(printer, preset_bundle);
            if (!model_id)
                return Error::NoTarget;
            const auto target = resolve_target(printer.config, *model_id, dev_id);
            if (!target)
                return Error::NoTarget;
            host = target->host;

            unsigned http_status = 0;
            std::string body;
            const std::string authority = target->http_port.empty() ? target->host : target->host + ":" + target->http_port;
            // The token goes only in the query, never in a header, so libcurl has no credential header of Orca's to forward on a redirect.
            Http::get("http://" + authority + "/system/info?X-Token=" + Http::url_encode(target->token))
                .timeout_connect(1)
                .timeout_max(1)
                .size_limit(64 * 1024)
                .on_complete([&](std::string response, unsigned status) {
                    body        = std::move(response);
                    http_status = status;
                })
                .on_error([&](std::string response, std::string, unsigned status) {
                    body        = std::move(response);
                    http_status = status;
                })
                .perform_sync();
            Error step = parse_system_info(http_status, body, info);
            if (step != Error::None)
                return step;

            nlohmann::json canvas_result;
            step = query_canvas(target->host, cc2_mqtt_port, info.sn, target->token, m_client_id, deadline, canvas_result);
            if (step != Error::None)
                return step;

            std::vector<CanvasUnit> units;
            step = parse_canvas_info(canvas_result, units);
            if (step != Error::None)
                return step;
            apply_snapshot(*obj, to_ams_slots(units, preset_bundle->filaments));
            return Error::None;
        }();
    } catch (const std::exception&) {
        error = Error::Exception;
    }
    if (error == Error::None)
        return true;

    BOOST_LOG_TRIVIAL(warning) << "Elegoo CC2 filament sync failed: " << error_name(error) << ", host " << host << ", SN "
                               << mask_serial(info.sn);
    if (obj)
        mark_snapshot_failed(*obj);
    return false;
}

} // namespace Slic3r
