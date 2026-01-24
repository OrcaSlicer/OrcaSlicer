#include "MoonrakerPrinterAgent.hpp"
#include "Http.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <sstream>
#include <thread>

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

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

// Sanitize filename to prevent path traversal attacks
// Extracts only the basename, removing any path components
std::string sanitize_filename(const std::string& filename)
{
    if (filename.empty()) {
        return "print.gcode";
    }
    namespace fs = boost::filesystem;
    fs::path p(filename);
    std::string basename = p.filename().string();
    if (basename.empty() || basename == "." || basename == "..") {
        return "print.gcode";
    }
    return basename;
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

} // namespace

namespace Slic3r {

const std::string MoonrakerPrinterAgent_VERSION = "1.0.0";

MoonrakerPrinterAgent::MoonrakerPrinterAgent(std::string log_dir) : m_cloud_agent(nullptr)
{
    BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: Constructor - log_dir=" << log_dir;
    (void) log_dir;
}

MoonrakerPrinterAgent::~MoonrakerPrinterAgent()
{
    disconnect_printer();  // This will handle thread cleanup
}

AgentInfo MoonrakerPrinterAgent::get_agent_info_static()
{
    return AgentInfo{.id = "moonraker", .name = "Moonraker Printer Agent", .version = MoonrakerPrinterAgent_VERSION, .description = "Klipper/Moonraker printer agent"};
}

void MoonrakerPrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    m_cloud_agent = cloud;
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: Cloud agent set";
}

int MoonrakerPrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    (void) qos;
    (void) flag;
    return handle_request(dev_id, json_str);
}

int MoonrakerPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    (void) qos;
    (void) flag;
    return handle_request(dev_id, json_str);
}

int MoonrakerPrinterAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
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
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: connect_printer missing host";
        dispatch_local_connect(ConnectStatusFailed, dev_id, "host_missing");
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    if (dev_id.empty()) {
        dev_id = extract_host(base_url);
    }

    // Check if connection already in progress
    {
        std::lock_guard<std::recursive_mutex> lock(connect_mutex);
        if (connect_in_progress.load()) {
            BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: Connection already in progress, waiting...";
            // Don't reject - wait for previous connection to complete
            // This can happen if MonitorPanel triggers connect while previous connect is still running
        } else {
            connect_in_progress.store(true);
            connect_stop_requested.store(false);
        }
    }

    // Wait for previous connection thread to finish
    if (connect_thread.joinable()) {
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: Waiting for previous connection thread...";
        connect_thread.join();
    }

    // Now we can start a new connection
    {
        std::lock_guard<std::recursive_mutex> lock(connect_mutex);
        connect_in_progress.store(true);
        connect_stop_requested.store(false);
    }

    // Stop existing status stream and clear state
    stop_status_stream();
    {
        std::lock_guard<std::recursive_mutex> lock(payload_mutex);
        status_cache = nlohmann::json::object();
    }
    ws_last_emit_ms.store(0);

    store_host(dev_id, base_url, api_key);

    // Launch connection in background thread
    connect_thread = std::thread([this, dev_id, base_url, api_key]() {
        perform_connection_async(dev_id, base_url, api_key);
    });

    // Return immediately - UI is not blocked
    BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: connect_printer launched in background - dev_id=" << dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::disconnect_printer()
{
    // Stop connection thread if running
    {
        std::lock_guard<std::recursive_mutex> lock(connect_mutex);
        if (connect_in_progress.load()) {
            connect_stop_requested.store(true);
            // Wake up any sleeping
            connect_cv.notify_all();
        }
    }

    // Wait for connection thread to finish (with timeout)
    if (connect_thread.joinable()) {
        connect_thread.join();
    }

    stop_status_stream();
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::check_cert()
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: check_cert (stub)";
    return BAMBU_NETWORK_SUCCESS;
}

void MoonrakerPrinterAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: install_device_cert (stub) - dev_id=" << dev_id << ", lan_only=" << lan_only;
}

bool MoonrakerPrinterAgent::start_discovery(bool start, bool sending)
{
    (void) sending;
    if (start) {
        announce_printhost_device();
    }
    return true;
}

int MoonrakerPrinterAgent::ping_bind(std::string ping_code)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: ping_bind (stub) - ping_code=" << ping_code;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    (void) sec_link;

    std::string base_url = normalize_base_url(dev_ip, "");
    if (base_url.empty()) {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    PrinthostConfig config;
    get_printhost_config(config);
    const std::string api_key = normalize_api_key(config.api_key);

    MoonrakerDeviceInfo info;
    std::string         error;
    if (!fetch_device_info(base_url, api_key, info, error)) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: bind_detect failed: " << error;
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
    // Prefer fetched hostname, then preset model name, then generic fallback
    std::string fallback_name = config.model_name.empty() ? "Moonraker Printer" : config.model_name;
    detect.dev_name     = info.dev_name.empty() ? fallback_name : info.dev_name;
    detect.model_id     = "moonraker";
    detect.version      = info.version;
    detect.connect_type = "lan";
    detect.bind_state   = "free";

    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::bind(
    std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: bind (stub) - dev_id=" << dev_id;
    (void) dev_ip;
    (void) sec_link;
    (void) timezone;
    (void) improved;
    (void) update_fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::unbind(std::string dev_id)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: unbind (stub) - dev_id=" << dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::request_bind_ticket(std::string* ticket)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: request_bind_ticket (stub)";
    if (ticket)
        *ticket = "";
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_server_callback(OnServerErrFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_server_err_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

std::string MoonrakerPrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    return selected_machine;
}

int MoonrakerPrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    selected_machine = dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: start_print (stub) - task_name=" << params.task_name;
    (void) update_fn;
    (void) cancel_fn;
    (void) wait_fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: start_local_print_with_record (stub)";
    (void) params;
    (void) update_fn;
    (void) cancel_fn;
    (void) wait_fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    (void) wait_fn;

    if (update_fn) update_fn(PrintingStageCreate, 0, "Preparing...");

    const std::string base_url = resolve_host(params.dev_id);
    if (base_url.empty()) {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    const std::string api_key = resolve_api_key(params.dev_id, params.password);

    std::string filename = params.filename;
    if (filename.empty()) {
        filename = params.task_name;
    }
    if (!boost::iends_with(filename, ".gcode")) {
        filename += ".gcode";
    }

    // Sanitize filename to prevent path traversal attacks
    std::string safe_filename = sanitize_filename(filename);

    // Upload only, don't start print
    if (!upload_gcode(params.filename, safe_filename, base_url, api_key, update_fn, cancel_fn)) {
        return BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED;
    }

    if (update_fn) update_fn(PrintingStageFinished, 100, "File uploaded");
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (update_fn) update_fn(PrintingStageCreate, 0, "Preparing...");

    // Check cancellation
    if (cancel_fn && cancel_fn()) {
        return BAMBU_NETWORK_ERR_CANCELED;
    }

    const std::string base_url = resolve_host(params.dev_id);
    if (base_url.empty()) {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    const std::string api_key = resolve_api_key(params.dev_id, params.password);

    // Determine the G-code file to upload
    // params.filename may be .3mf, params.dst_file contains actual G-code
    std::string gcode_path = params.filename;
    if (!params.dst_file.empty()) {
        gcode_path = params.dst_file;
    }

    // Check if file exists and has .gcode extension
    namespace fs = boost::filesystem;
    fs::path source_path(gcode_path);
    if (!fs::exists(source_path)) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: G-code file does not exist: " << gcode_path;
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    }

    // Extract filename for upload (relative to gcodes root)
    std::string upload_filename = source_path.filename().string();
    if (!boost::iends_with(upload_filename, ".gcode")) {
        upload_filename += ".gcode";
    }
    // Sanitize filename to prevent path traversal attacks (extra safety)
    upload_filename = sanitize_filename(upload_filename);

    // Upload file
    if (update_fn) update_fn(PrintingStageUpload, 0, "Uploading G-code...");
    if (!upload_gcode(gcode_path, upload_filename, base_url, api_key, update_fn, cancel_fn)) {
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    }

    // Check cancellation
    if (cancel_fn && cancel_fn()) {
        return BAMBU_NETWORK_ERR_CANCELED;
    }

    // Start print via gcode script (simpler than JSON-RPC)
    if (update_fn) update_fn(PrintingStageSending, 0, "Starting print...");
    std::string gcode = "SDCARD_PRINT_FILE FILENAME=" + upload_filename;
    if (!send_gcode(params.dev_id, gcode)) {
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }

    if (update_fn) update_fn(PrintingStageFinished, 100, "Print started");
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: start_sdcard_print (stub)";
    (void) params;
    (void) update_fn;
    (void) cancel_fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        on_ssdp_msg_fn = fn;
    }
    // Call announce_printhost_device() outside the lock to avoid deadlock
    // since announce_printhost_device() also acquires state_mutex
    if (fn) {
        announce_printhost_device();
    }
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_printer_connected_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_subscribe_failure_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_on_user_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_user_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_local_connect_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    on_local_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    queue_on_main_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

void MoonrakerPrinterAgent::fetch_filament_info(std::string dev_id)
{
    // Moonraker doesn't have standard filament tracking like Qidi
    // This is a no-op for standard Moonraker installations
    // Note: QidiPrinterAgent overrides this method with actual implementation
    BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent::fetch_filament_info (base class no-op) called for dev_id=" << dev_id
                            << " - if you see this for Qidi printer, virtual dispatch is broken!";
}

int MoonrakerPrinterAgent::handle_request(const std::string& dev_id, const std::string& json_str)
    {
        BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: handle_request received: " << json_str;
        auto json = nlohmann::json::parse(json_str, nullptr, false);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: Invalid JSON request";
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    }

    // Handle info commands
    if (json.contains("info") && json["info"].contains("command")) {
        const auto& command = json["info"]["command"];
        if (command.is_string() && command.get<std::string>() == "get_version") {
            return send_version_info(dev_id);
        }
    }

    // Handle system commands
    if (json.contains("system") && json["system"].contains("command")) {
        const auto& command = json["system"]["command"];
        if (command.is_string() && command.get<std::string>() == "get_access_code") {
            return send_access_code(dev_id);
        }
    }

    // Handle print commands
    if (json.contains("print") && json["print"].contains("command")) {
        const auto& command = json["print"]["command"];
        if (!command.is_string()) {
            BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: print command is not a string";
            return BAMBU_NETWORK_ERR_INVALID_RESULT;
        }

        const std::string cmd = command.get<std::string>();
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: Received print command: " << cmd;

        // Handle gcode_line command - this is how G-code commands are sent from OrcaSlicer
        if (cmd == "gcode_line") {
            if (!json["print"].contains("param") || !json["print"]["param"].is_string()) {
                BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: gcode_line missing param value, full json: " << json_str;
                return BAMBU_NETWORK_ERR_INVALID_RESULT;
            }
            std::string gcode = json["print"]["param"].get<std::string>();

            // Extract sequence_id from request if present
            std::string sequence_id;
            if (json["print"].contains("sequence_id") && json["print"]["sequence_id"].is_string()) {
                sequence_id = json["print"]["sequence_id"].get<std::string>();
            }

            nlohmann::json response;
            response["print"]["command"] = "gcode_line";
            if (!sequence_id.empty()) {
                response["print"]["sequence_id"] = sequence_id;
            }
            response["print"]["param"] = gcode;

            if (send_gcode(dev_id, gcode)) {
                response["print"]["result"] = "success";
                dispatch_message(dev_id, response.dump());
                return BAMBU_NETWORK_SUCCESS;
            }
            response["print"]["result"] = "failed";
            dispatch_message(dev_id, response.dump());
            return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
        }

        // ===== NEW: Print control commands =====
        if (cmd == "pause") {
            return pause_print(dev_id);
        }
        if (cmd == "resume") {
            return resume_print(dev_id);
        }
        if (cmd == "stop") {
            return cancel_print(dev_id);
        }

        // Bed temperature - UI sends "temp" field
        if (cmd == "set_bed_temp") {
            if (json["print"].contains("temp") && json["print"]["temp"].is_number()) {
                int temp = json["print"]["temp"].get<int>();
                std::string gcode = "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=" + std::to_string(temp);
                send_gcode(dev_id, gcode);
                return BAMBU_NETWORK_SUCCESS;
            }
        }

        // Nozzle temperature - UI sends "target_temp" and "extruder_index" fields
        if (cmd == "set_nozzle_temp") {
            if (json["print"].contains("target_temp") && json["print"]["target_temp"].is_number()) {
                int temp = json["print"]["target_temp"].get<int>();
                int extruder_idx = 0;  // Default to main extruder
                if (json["print"].contains("extruder_index") && json["print"]["extruder_index"].is_number()) {
                    extruder_idx = json["print"]["extruder_index"].get<int>();
                }
                std::string heater = (extruder_idx == 0) ? "extruder" : "extruder" + std::to_string(extruder_idx);
                std::string gcode = "SET_HEATER_TEMPERATURE HEATER=" + heater + " TARGET=" + std::to_string(temp);
                send_gcode(dev_id, gcode);
                return BAMBU_NETWORK_SUCCESS;
            }
        }

        if (cmd == "home") {
            return send_gcode(dev_id, "G28") ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
        }
    }

    return BAMBU_NETWORK_SUCCESS;
}

bool MoonrakerPrinterAgent::get_printhost_config(PrinthostConfig& config) const
{
    auto* preset_bundle = GUI::wxGetApp().preset_bundle;
    if (!preset_bundle) {
        return false;
    }

    auto&      preset      = preset_bundle->printers.get_edited_preset();
    const auto& printer_cfg = preset.config;
    const DynamicPrintConfig* host_cfg = &printer_cfg;
    config.host = host_cfg->opt_string("print_host");
    if (config.host.empty()) {
        if (auto* physical_cfg = preset_bundle->physical_printers.get_selected_printer_config()) {
            if (!physical_cfg->opt_string("print_host").empty()) {
                host_cfg   = physical_cfg;
                config.host = host_cfg->opt_string("print_host");
            }
        }
    }
    if (config.host.empty()) {
        return false;
    }

    config.port       = host_cfg->opt_string("printhost_port");
    config.api_key    = host_cfg->opt_string("printhost_apikey");
    config.model_name = printer_cfg.opt_string("printer_model");
    config.base_url   = normalize_base_url(config.host, config.port);
    config.model_id   = preset.get_printer_type(preset_bundle);

    return !config.base_url.empty();
}

bool MoonrakerPrinterAgent::fetch_device_info(const std::string& base_url,
                                               const std::string& api_key,
                                               MoonrakerDeviceInfo& info,
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
    std::string    url = join_url(base_url, "/server/info");
    if (!fetch_json(url, json)) {
        return false;
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

bool MoonrakerPrinterAgent::fetch_server_info(const std::string& base_url,
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

bool MoonrakerPrinterAgent::fetch_server_info_json(const std::string& base_url,
                                                     const std::string& api_key,
                                                     nlohmann::json& info,
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

    info = nlohmann::json::parse(response_body, nullptr, false, true);
    if (info.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    return true;
}

bool MoonrakerPrinterAgent::query_printer_status(const std::string& base_url,
                                                   const std::string& api_key,
                                                   nlohmann::json& status,
                                                   std::string& error) const
{
    std::string url = join_url(base_url, "/printer/objects/query?print_stats&virtual_sdcard&extruder&heater_bed&fan");

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned status_code) {
            if (status_code == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status_code);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status_code) {
            http_error = err;
            if (status_code > 0) {
                http_error += " (HTTP " + std::to_string(status_code) + ")";
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

    if (!json.contains("result") || !json["result"].contains("status")) {
        error = "Unexpected JSON structure";
        return false;
    }

    status = json["result"]["status"];
    return true;
}

bool MoonrakerPrinterAgent::send_gcode(const std::string& dev_id, const std::string& gcode) const
{
    const std::string base_url = resolve_host(dev_id);
    if (base_url.empty()) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: send_gcode - empty base_url for dev_id=" << dev_id;
        return false;
    }
    const std::string api_key = resolve_api_key(dev_id, "");

    nlohmann::json payload;
    payload["script"] = gcode;
    std::string payload_str = payload.dump();

    BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: send_gcode to " << base_url << " with payload: " << payload_str;

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::post(join_url(base_url, "/printer/gcode/script"));
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.header("Content-Type", "application/json")
        .set_post_body(payload_str)
        .timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned status_code) {
            BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: send_gcode response status=" << status_code << " body=" << body;
            if (status_code == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status_code);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status_code) {
            BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: send_gcode error - body=" << body << " err=" << err << " status=" << status_code;
            http_error = err;
            if (status_code > 0) {
                http_error += " (HTTP " + std::to_string(status_code) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: send_gcode failed: " << http_error;
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: sent gcode successfully: " << gcode;
    return true;
}

bool MoonrakerPrinterAgent::fetch_object_list(const std::string& base_url,
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

int MoonrakerPrinterAgent::send_version_info(const std::string& dev_id)
{
    const std::string base_url = resolve_host(dev_id);
    if (base_url.empty()) {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    const std::string api_key = resolve_api_key(dev_id, "");

    std::string version;
    std::string error;
    if (!fetch_server_info(base_url, api_key, version, error)) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: Failed to fetch server info: " << error;
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

int MoonrakerPrinterAgent::send_access_code(const std::string& dev_id)
{
    nlohmann::json payload;
    payload["system"]["command"]     = "get_access_code";
    payload["system"]["access_code"] = resolve_api_key(dev_id, "");
    dispatch_message(dev_id, payload.dump());
    return BAMBU_NETWORK_SUCCESS;
}

void MoonrakerPrinterAgent::announce_printhost_device()
{
    BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: announce_printhost_device() called";

    PrinthostConfig config;
    if (!get_printhost_config(config)) {
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: announce_printhost_device - no printhost config";
        return;
    }

    const std::string base_url = config.base_url;
    if (base_url.empty()) {
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: announce_printhost_device - empty base_url";
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: announce_printhost_device - base_url=" << base_url;

    OnMsgArrivedFn ssdp_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        ssdp_fn = on_ssdp_msg_fn;
        if (!ssdp_fn) {
            BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: announce_printhost_device - no ssdp callback";
            return;
        }
        if (ssdp_announced_host == base_url && !ssdp_announced_id.empty()) {
            BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: announce_printhost_device - already announced";
            return;
        }
    }

    const std::string dev_id   = extract_host(base_url);
    const std::string api_key  = normalize_api_key(config.api_key);

    // Try to fetch actual device name from Moonraker
    // Priority: 1) Moonraker hostname, 2) Preset model name, 3) Generic fallback
    std::string dev_name;
    MoonrakerDeviceInfo info;
    std::string fetch_error;
    if (fetch_device_info(base_url, api_key, info, fetch_error) && !info.dev_name.empty()) {
        dev_name = info.dev_name;
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: Got device name from printer: " << dev_name;
    } else {
        dev_name = config.model_name.empty() ? "Moonraker Printer" : config.model_name;
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: Using fallback device name: " << dev_name
                                << " (fetch_error=" << fetch_error << ")";
    }

    const std::string model_id = config.model_id;

    if (auto* app_config = GUI::wxGetApp().app_config) {
        const std::string access_code = api_key.empty() ? k_no_api_key : api_key;
        app_config->set_str("access_code", dev_id, access_code);
        app_config->set_str("user_access_code", dev_id, access_code);
    }

    store_host(dev_id, base_url, api_key);

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

    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        ssdp_announced_host = base_url;
        ssdp_announced_id   = dev_id;

        // Set this as the selected machine if nothing is currently selected
        // This ensures auto-connect works when MonitorPanel opens
        if (selected_machine.empty()) {
            selected_machine = dev_id;
            BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: Auto-selected machine: " << dev_id;
        }
    }
}

void MoonrakerPrinterAgent::dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg)
{
    BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: dispatch_local_connect state=" << state
                           << " dev_id=" << dev_id << " msg=" << msg;

    OnLocalConnectedFn local_fn;
    QueueOnMainFn      queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        local_fn = on_local_connect_fn;
        queue_fn = queue_on_main_fn;
    }
    if (!local_fn) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: dispatch_local_connect - no callback registered!";
        return;
    }

    auto dispatch = [state, dev_id, msg, local_fn]() { local_fn(state, dev_id, msg); };
    if (queue_fn) {
        queue_fn(dispatch);
    } else {
        dispatch();
    }
}

void MoonrakerPrinterAgent::dispatch_printer_connected(const std::string& dev_id)
{
    OnPrinterConnectedFn connected_fn;
    QueueOnMainFn        queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
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

void MoonrakerPrinterAgent::start_status_stream(const std::string& dev_id, const std::string& base_url, const std::string& api_key)
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

void MoonrakerPrinterAgent::stop_status_stream()
{
    ws_stop.store(true);
    if (ws_thread.joinable()) {
        ws_thread.join();
    }
}

void MoonrakerPrinterAgent::run_status_stream(std::string dev_id, std::string base_url, std::string api_key)
{
    WsEndpoint endpoint;
    if (!parse_ws_endpoint(base_url, endpoint)) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: websocket endpoint invalid for base_url=" << base_url;
        return;
    }
    if (endpoint.secure) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: websocket wss not supported for base_url=" << base_url;
        return;
    }

    // Reconnection logic
    ws_reconnect_requested.store(false);  // Reset reconnect flag
    int retry_count = 0;
    const int max_retries = 10;
    const int base_delay_ms = 1000;

    while (!ws_stop.load() && retry_count < max_retries) {
        bool connection_lost = false;  // Flag to distinguish clean shutdown from unexpected disconnect

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

            // Send client identification
            nlohmann::json identify;
            identify["jsonrpc"] = "2.0";
            identify["method"] = "server.connection.identify";
            identify["params"]["client_name"] = "OrcaSlicer";
            identify["params"]["version"] = MoonrakerPrinterAgent_VERSION;
            identify["params"]["type"] = "agent";
            identify["params"]["url"] = "https://github.com/SoftFever/OrcaSlicer";
            identify["id"] = 0;
            ws.write(net::buffer(identify.dump()));

            std::set<std::string> subscribe_objects = {"print_stats", "virtual_sdcard"};
            std::set<std::string> available_objects;
            std::string           list_error;
            if (fetch_object_list(base_url, api_key, available_objects, list_error)) {
                // Store available_objects in member variable for feature detection
                {
                    std::lock_guard<std::recursive_mutex> lock(payload_mutex);
                    this->available_objects = std::move(available_objects);
                }

                std::string objects_str;
                for (const auto& name : this->available_objects) {
                    if (!objects_str.empty()) objects_str += ", ";
                    objects_str += name;
                }

                if (this->available_objects.count("heater_bed") != 0) {
                    subscribe_objects.insert("heater_bed");
                }
                // Only subscribe to "fan" if it exists (standard Moonraker API)
                if (this->available_objects.count("fan") != 0) {
                    subscribe_objects.insert("fan");
                } else {
                }

                // Add toolhead for homing status
                if (this->available_objects.count("toolhead") != 0) {
                    subscribe_objects.insert("toolhead");
                }

                // Add display_status for layer info (if available)
                if (this->available_objects.count("display_status") != 0) {
                    subscribe_objects.insert("display_status");
                }

                for (const auto& name : this->available_objects) {
                    if (name == "extruder" || name.rfind("extruder", 0) == 0) {
                        subscribe_objects.insert(name);
                        if (name == "extruder") {
                            break;
                        }
                    }
                }
            } else {
                subscribe_objects.insert("extruder");
                subscribe_objects.insert("heater_bed");
                subscribe_objects.insert("toolhead");  // Add toolhead as fallback
                subscribe_objects.insert("fan");  // Try to subscribe to fan as fallback
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

            // Read loop
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
                            std::lock_guard<std::recursive_mutex> lock(payload_mutex);
                            message = build_print_payload_locked();
                        }
                        dispatch_message(dev_id, message.dump());
                        ws_last_emit_ms.store(now_ms);
                    }
                    continue;
                }
                if (ec == websocket::error::closed) {
                    connection_lost = true;
                    break;
                }
                if (ec) {
                    BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: websocket read error: " << ec.message();
                    connection_lost = true;
                    break;
                }
                handle_ws_message(dev_id, beast::buffers_to_string(buffer.data()));
                // Check if handle_ws_message triggered reconnection request
                if (ws_reconnect_requested.exchange(false)) {
                    connection_lost = true;
                    break;
                }
            }

            beast::error_code ec;
            ws.close(websocket::close_code::normal, ec);

            // Only reset retry count on clean shutdown (not connection_lost)
            if (!connection_lost && !ws_stop.load()) {
                retry_count = 0;
            }

        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: websocket disconnected: " << e.what();
            connection_lost = true;
        }

        // Exit loop on clean shutdown
        if (!connection_lost) {
            break;
        }

        // Check if we should stop reconnection attempts
        if (ws_stop.load()) {
            break;
        }

        // Exponential backoff before reconnection
        int delay_ms = base_delay_ms * (1 << std::min(retry_count, 5));
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: Reconnecting in " << delay_ms << "ms (attempt " << (retry_count + 1) << ")";
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        retry_count++;
    }

    if (retry_count >= max_retries) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: Max reconnection attempts reached";
        dispatch_local_connect(ConnectStatusLost, dev_id, "max_retries");
    }
}

void MoonrakerPrinterAgent::handle_ws_message(const std::string& dev_id, const std::string& payload)
{
    auto json = nlohmann::json::parse(payload, nullptr, false);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: Invalid WebSocket message JSON";
        return;
    }

    bool updated = false;

    // Check for subscription response (has "result.status")
    if (json.contains("result") && json["result"].contains("status") &&
        json["result"]["status"].is_object()) {
        update_status_cache(json["result"]["status"]);
        updated = true;
    }

    // Check for status update notifications
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
        // Handle Klippy disconnect - update status and trigger reconnection
        else if (method == "notify_klippy_disconnected") {
            // Klippy disconnected - update status to reflect disconnect state
            nlohmann::json updates;
            updates["print_stats"]["state"] = "error";
            update_status_cache(updates);
            updated = true;
            // Set flag to trigger reconnection after dispatching the status update
            ws_reconnect_requested.store(true);
            BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: Klippy disconnected, triggering reconnection";
        }
    }

    if (updated) {
        nlohmann::json message;
        {
            std::lock_guard<std::recursive_mutex> lock(payload_mutex);
            message = build_print_payload_locked();
        }

        BOOST_LOG_TRIVIAL(trace) << "MoonrakerPrinterAgent: Dispatching payload: " << message.dump();
        dispatch_message(dev_id, message.dump());

        const auto now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        ws_last_emit_ms.store(now_ms);
    }
}

void MoonrakerPrinterAgent::update_status_cache(const nlohmann::json& updates)
{
    if (!updates.is_object()) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(payload_mutex);
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

nlohmann::json MoonrakerPrinterAgent::build_print_payload_locked() const
{
    nlohmann::json payload;
    payload["print"]["command"]            = "push_status";
    payload["print"]["msg"]                = 0;
    payload["print"]["support_mqtt_alive"] = true;

    std::string state = "IDLE";
    if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("state") &&
        status_cache["print_stats"]["state"].is_string()) {
        state = map_moonraker_state(status_cache["print_stats"]["state"].get<std::string>());
    }
    payload["print"]["gcode_state"] = state;

    // ===== NEW: Print Stage =====
    // Map Moonraker state to Bambu stage numbers
    int mc_print_stage = 0;
    if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("state")) {
        std::string mr_state = status_cache["print_stats"]["state"].get<std::string>();
        if (mr_state == "printing") mc_print_stage = 1;
        else if (mr_state == "paused") mc_print_stage = 2;
        else if (mr_state == "complete") mc_print_stage = 3;
        else if (mr_state == "error") mc_print_stage = 4;
    }
    payload["print"]["mc_print_stage"] = mc_print_stage;

    // ===== NEW: Error Codes =====
    // Leave mc_print_error_code and print_error at 0
    // UI expects numeric HMS codes - setting to 1 shows generic error dialog
    // Only set if real mapping from Moonraker error strings to HMS codes is defined
    payload["print"]["mc_print_error_code"] = 0;
    payload["print"]["print_error"] = 0;

    // ===== NEW: Home Flag =====
    // Map homed axes to bit field: X=bit0, Y=bit1, Z=bit2
    // WARNING: This only sets bits 0-2, clearing support flags (bit 3+)
    // Bit 3 = 220V voltage, bit 4 = auto recovery, etc.
    // This is acceptable for Moonraker (no AMS, different feature set)
    int home_flag = 0;
    if (status_cache.contains("toolhead") && status_cache["toolhead"].contains("homed_axes")) {
        std::string homed = status_cache["toolhead"]["homed_axes"].get<std::string>();
        if (homed.find('X') != std::string::npos) home_flag |= 1;  // bit 0
        if (homed.find('Y') != std::string::npos) home_flag |= 2;  // bit 1
        if (homed.find('Z') != std::string::npos) home_flag |= 4;  // bit 2
    }
    payload["print"]["home_flag"] = home_flag;

    // ===== NEW: Temperature Ranges =====
    // Moonraker doesn't provide this via API - use hardcoded defaults
    payload["print"]["nozzle_temp_range"] = {100, 370};  // Typical Klipper range
    payload["print"]["bed_temp_range"] = {0, 120};        // Typical bed range

    // ===== NEW: Feature Flags =====
    payload["print"]["support_send_to_sd"] = true;
    // Detect bed_leveling support from available objects (bed_mesh or probe)
    // Default to 0 (not supported) if neither object exists
    bool has_bed_leveling = (available_objects.count("bed_mesh") != 0 ||
                             available_objects.count("probe") != 0);
    payload["print"]["support_bed_leveling"] = has_bed_leveling ? 1 : 0;

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

    // Handle fan speed - only if Moonraker provides "fan" object (standard API)
    if (status_cache.contains("fan") && status_cache["fan"].is_object() && !status_cache["fan"].empty()) {
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
        } else if (fan.contains("power") && fan["power"].is_number()) {
            double power = fan["power"].get<double>();
            int pwm = static_cast<int>(power * 255.0 + 0.5);
            pwm = std::clamp(pwm, 0, 255);
            payload["print"]["fan_gear"] = pwm;
        }
    }
    // If "fan" object doesn't exist, don't include fan_gear in payload

    if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("filename") &&
        status_cache["print_stats"]["filename"].is_string()) {
        payload["print"]["subtask_name"] = status_cache["print_stats"]["filename"].get<std::string>();
    }

    // ===== NEW: G-code File Path =====
    if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("filename")) {
        payload["print"]["gcode_file"] = status_cache["print_stats"]["filename"];
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

    BOOST_LOG_TRIVIAL(trace) << "MoonrakerPrinterAgent: Built payload with gcode_state=" << state;

    return payload;
}

std::string MoonrakerPrinterAgent::resolve_host(const std::string& dev_id) const
{
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        auto                        it = host_by_device.find(dev_id);
        if (it != host_by_device.end()) {
            return it->second;
        }
    }

    PrinthostConfig config;
    if (get_printhost_config(config)) {
        return config.base_url;
    }

    return "";
}

std::string MoonrakerPrinterAgent::resolve_api_key(const std::string& dev_id, const std::string& fallback) const
{
    std::string api_key = normalize_api_key(fallback);
    if (!api_key.empty()) {
        return api_key;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
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

void MoonrakerPrinterAgent::store_host(const std::string& dev_id, const std::string& host, const std::string& api_key)
{
    if (host.empty()) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    host_by_device[dev_id] = host;
    if (!api_key.empty()) {
        api_key_by_device[dev_id] = api_key;
    }
}

void MoonrakerPrinterAgent::dispatch_message(const std::string& dev_id, const std::string& payload)
{
    BOOST_LOG_TRIVIAL(trace) << "MoonrakerPrinterAgent: dispatch_message dev_id=" << dev_id
                             << " payload_size=" << payload.size();

    OnMessageFn   local_fn;
    OnMessageFn   cloud_fn;
    QueueOnMainFn queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        local_fn = on_local_message_fn;
        cloud_fn = on_message_fn;
        queue_fn = queue_on_main_fn;
    }

    if (!local_fn && !cloud_fn) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: dispatch_message - no message callback registered!";
        return;
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

bool MoonrakerPrinterAgent::upload_gcode(
    const std::string& local_path,
    const std::string& filename,
    const std::string& base_url,
    const std::string& api_key,
    OnUpdateStatusFn update_fn,
    WasCancelledFn cancel_fn)
{
    namespace fs = boost::filesystem;

    // Validate file exists
    fs::path source_path(local_path);
    if (!fs::exists(source_path)) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: File does not exist: " << local_path;
        return false;
    }

    // Check file size
    std::uintmax_t file_size = fs::file_size(source_path);
    if (file_size > 1024 * 1024 * 1024) {  // 1GB limit
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: File too large: " << file_size << " bytes";
        return false;
    }

    // Sanitize filename to prevent path traversal attacks
    std::string safe_filename = sanitize_filename(filename);

    bool result = true;
    std::string http_error;

    // Use Http::form_add and Http::form_add_file
    auto http = Http::post(join_url(base_url, "/server/files/upload"));
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.form_add("root", "gcodes")  // Upload to gcodes directory
        .form_add("print", "false")   // Don't auto-start print
        .form_add_file("file", source_path.string(), safe_filename)
        .timeout_connect(10)
        .timeout_max(300)  // 5 minutes for large files
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << "MoonrakerPrinterAgent: Upload complete: HTTP " << status << " body: " << body;
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: Upload error: " << err << " HTTP " << status;
            http_error = err;
            result = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            // Check for cancellation via WasCancelledFn
            if (cancel_fn && cancel_fn()) {
                cancel = true;
                result = false;
                return;
            }
            // Report progress via OnUpdateStatusFn
            if (update_fn && progress.ultotal > 0) {
                int percent = static_cast<int>((progress.ulnow * 100) / progress.ultotal);
                update_fn(PrintingStageUpload, percent, "Uploading...");
            }
        })
        .perform_sync();

    if (!result) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: Upload failed: " << http_error;
        return false;
    }

    return true;
}

int MoonrakerPrinterAgent::pause_print(const std::string& dev_id)
{
    const std::string base_url = resolve_host(dev_id);
    const std::string api_key = resolve_api_key(dev_id, "");

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["method"] = "printer.print.pause";
    request["id"] = next_jsonrpc_id++;

    std::string response;
    // For JSON-RPC over HTTP, we need to use POST to /printer/print/pause
    // But Moonraker also supports this via WebSocket
    // For now, send via gcode script which is simpler
    std::string gcode = "PAUSE";
    return send_gcode(dev_id, gcode) ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
}

int MoonrakerPrinterAgent::resume_print(const std::string& dev_id)
{
    std::string gcode = "RESUME";
    return send_gcode(dev_id, gcode) ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
}

int MoonrakerPrinterAgent::cancel_print(const std::string& dev_id)
{
    std::string gcode = "CANCEL_PRINT";
    return send_gcode(dev_id, gcode) ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
}

bool MoonrakerPrinterAgent::send_jsonrpc_command(
    const std::string& base_url,
    const std::string& api_key,
    const nlohmann::json& request,
    std::string& response) const
{
    std::string request_str = request.dump();
    std::string url = join_url(base_url, "/printer/print/start");

    bool success = false;
    std::string http_error;

    auto http = Http::post(url);
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.header("Content-Type", "application/json")
        .set_post_body(request_str)
        .timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response = body;
                success = true;
            } else {
                http_error = "HTTP " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_error = err;
        })
        .perform_sync();

    if (!success) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: JSON-RPC command failed: " << http_error;
    }

    return success;
}

void MoonrakerPrinterAgent::perform_connection_async(
    const std::string& dev_id,
    const std::string& base_url,
    const std::string& api_key)
{
    int result = BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    std::string error_msg;

    try {
        // Check Klippy state
        nlohmann::json server_info;
        if (!fetch_server_info_json(base_url, api_key, server_info, error_msg)) {
            BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: Failed to fetch server info: " << error_msg;
            dispatch_local_connect(ConnectStatusFailed, dev_id, "server_info_failed");
            finish_connection();
            return;
        }

        nlohmann::json result_json = server_info.contains("result")
            ? server_info["result"] : server_info;
        std::string klippy_state = result_json.value("klippy_state", "");

        // Poll for Klippy ready state (with stop check)
        if (klippy_state == "startup") {
            for (int i = 0; i < 30; i++) {  // 30 second max
                {
                    std::unique_lock<std::recursive_mutex> lock(connect_mutex);
                    if (connect_stop_requested.load()) {
                        result = BAMBU_NETWORK_ERR_CANCELED;
                        break;
                    }
                }

                std::this_thread::sleep_for(std::chrono::seconds(1));

                if (fetch_server_info_json(base_url, api_key, server_info, error_msg)) {
                    result_json = server_info.contains("result")
                        ? server_info["result"] : server_info;
                    klippy_state = result_json.value("klippy_state", "");
                    if (klippy_state == "ready") break;
                }
            }
        }

        // Check final state
        if (klippy_state != "ready" && result == BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED) {
            std::string state_message = result_json.value("state_message", "Unknown error");
            BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: Klippy not ready: " << klippy_state
                << " - " << state_message;
            error_msg = "klippy_not_ready:" + klippy_state;
            dispatch_local_connect(ConnectStatusFailed, dev_id, error_msg);
            finish_connection();
            return;
        }

        // Query initial status
        nlohmann::json initial_status;
        if (query_printer_status(base_url, api_key, initial_status, error_msg)) {
            {
                update_status_cache(initial_status);
            }
            BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: Initial status queried successfully";
        } else {
            BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: Initial status query failed: " << error_msg;
        }

        // Start WebSocket status stream
        start_status_stream(dev_id, base_url, api_key);

        // Success!
        result = BAMBU_NETWORK_SUCCESS;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: Connection exception: " << e.what();
        error_msg = std::string("exception: ") + e.what();
        result = BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    }

    // Dispatch final result to UI
    if (result == BAMBU_NETWORK_SUCCESS) {
        dispatch_local_connect(ConnectStatusOk, dev_id, "0");
        dispatch_printer_connected(dev_id);
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: connect_printer completed - dev_id=" << dev_id;
    } else if (result != BAMBU_NETWORK_ERR_CANCELED) {
        dispatch_local_connect(ConnectStatusFailed, dev_id, error_msg);
    }

    finish_connection();
}

void MoonrakerPrinterAgent::finish_connection()
{
    std::lock_guard<std::recursive_mutex> lock(connect_mutex);
    connect_in_progress.store(false);
}

} // namespace Slic3r
