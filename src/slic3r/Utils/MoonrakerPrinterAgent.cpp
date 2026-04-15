#include "MoonrakerPrinterAgent.hpp"
#include "Http.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "../GUI/DeviceCore/DevStorage.h"
#include "../GUI/DeviceCore/DevFirmware.h"
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
#include <cstdio>
#include <cctype>
#include <map>
#include <thread>

namespace {

namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;

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

MoonrakerPrinterAgent::MoonrakerPrinterAgent(std::string log_dir) : m_cloud_agent(nullptr) { (void) log_dir; }

MoonrakerPrinterAgent::~MoonrakerPrinterAgent()
{
    {
        std::lock_guard<std::recursive_mutex> lock(connect_mutex);
        device_info = MoonrakerDeviceInfo{};
        ++connect_generation;
    }
    if (connect_thread.joinable()) {
        connect_thread.join();
    }
    stop_status_stream();
}

AgentInfo MoonrakerPrinterAgent::get_agent_info_static()
{
    return AgentInfo{"moonraker", "Moonraker", MoonrakerPrinterAgent_VERSION, "Klipper/Moonraker printer agent"};
}

void MoonrakerPrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    m_cloud_agent = cloud;
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
    if (dev_id.empty() || dev_ip.empty()) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: connect_printer missing dev_id or dev_ip";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    std::string base_url;
    std::string api_key;
    uint64_t    gen;
    {
        std::lock_guard<std::recursive_mutex> lock(connect_mutex);
        init_device_info(dev_id, dev_ip, username, password, use_ssl);
        gen      = ++connect_generation;
        base_url = device_info.base_url;
        api_key  = device_info.api_key;
        if (connect_thread.joinable()) {
            connect_thread.detach();
        }
    }

    // Stop existing status stream and clear state
    stop_status_stream();
    {
        std::lock_guard<std::recursive_mutex> lock(payload_mutex);
        status_cache = nlohmann::json::object();
    }
    ws_last_emit_ms.store(0);
    ws_last_dispatch_ms.store(0);
    last_print_state.clear();

    // Launch connection in background thread (capture by value to avoid data races)
    {
        std::lock_guard<std::recursive_mutex> lock(connect_mutex);
        connect_thread = std::thread([this, dev_id, base_url, api_key, gen]() { perform_connection_async(dev_id, base_url, api_key, gen); });
    }

    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::disconnect_printer()
{
    {
        std::lock_guard<std::recursive_mutex> lock(connect_mutex);
        device_info = MoonrakerDeviceInfo{};
        ++connect_generation; // Invalidate any in-flight connection
        if (connect_thread.joinable()) {
            connect_thread.detach();
        }
    }

    stop_status_stream();
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::check_cert() { return BAMBU_NETWORK_SUCCESS; }

void MoonrakerPrinterAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    (void) dev_id;
    (void) lan_only;
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
    (void) ping_code;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    (void) sec_link;

    detect.dev_id   = device_info.dev_id.empty() ? dev_ip : device_info.dev_id;
    detect.model_id = device_info.model_id.empty() ? device_info.model_name : device_info.model_id;
    // Prefer fetched hostname, then preset model name, then generic fallback
    detect.dev_name     = device_info.dev_name;
    detect.model_id     = device_info.model_id;
    detect.version      = device_info.version;
    detect.connect_type = "lan";
    detect.bind_state   = "free";

    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::bind(
    std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn)
{
    (void) dev_ip;
    (void) dev_id;
    (void) sec_link;
    (void) timezone;
    (void) improved;
    (void) update_fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::unbind(std::string dev_id)
{
    (void) dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::request_bind_ticket(std::string* ticket)
{
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
    (void) params;
    (void) update_fn;
    (void) cancel_fn;
    (void) wait_fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::start_local_print_with_record(PrintParams      params,
                                                         OnUpdateStatusFn update_fn,
                                                         WasCancelledFn   cancel_fn,
                                                         OnWaitFn         wait_fn)
{
    (void) params;
    (void) update_fn;
    (void) cancel_fn;
    (void) wait_fn;
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::start_send_gcode_to_sdcard(PrintParams      params,
                                                      OnUpdateStatusFn update_fn,
                                                      WasCancelledFn   cancel_fn,
                                                      OnWaitFn         wait_fn)
{
    (void) wait_fn;

    if (update_fn)
        update_fn(PrintingStageCreate, 0, "Preparing...");

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
    if (!upload_gcode(params.filename, safe_filename, device_info.base_url, device_info.api_key, update_fn, cancel_fn)) {
        return BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED;
    }

    if (update_fn)
        update_fn(PrintingStageFinished, 100, "File uploaded");
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (update_fn)
        update_fn(PrintingStageCreate, 0, "Preparing...");

    // Check cancellation
    if (cancel_fn && cancel_fn()) {
        return BAMBU_NETWORK_ERR_CANCELED;
    }
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
    if (update_fn)
        update_fn(PrintingStageUpload, 0, "Uploading G-code...");
    if (!upload_gcode(gcode_path, upload_filename, device_info.base_url, device_info.api_key, update_fn, cancel_fn)) {
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    }

    // Check cancellation
    if (cancel_fn && cancel_fn()) {
        return BAMBU_NETWORK_ERR_CANCELED;
    }

    // Start print via gcode script (simpler than JSON-RPC)
    if (update_fn)
        update_fn(PrintingStageSending, 0, "Starting print...");
    std::string gcode = "SDCARD_PRINT_FILE FILENAME=" + upload_filename;
    if (!send_gcode(device_info.dev_id, gcode)) {
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }

    if (update_fn)
        update_fn(PrintingStageFinished, 100, "Print started");
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
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

void MoonrakerPrinterAgent::build_ams_payload(int ams_count, int max_lane_index, const std::vector<AmsTrayData>& trays)
{
    // Look up MachineObject via DeviceManager
    auto* dev_manager = GUI::wxGetApp().getDeviceManager();
    if (!dev_manager) {
        return;
    }
    MachineObject* obj = dev_manager->get_my_machine(device_info.dev_id);
    if (!obj) {
        return;
    }

    // Color normalization helper (handles #RRGGBB, 0xRRGGBB -> RRGGBBAA)
    auto normalize_color = [](const std::string& color) -> std::string {
        std::string value = color;
        boost::trim(value);

        // Remove 0x or 0X prefix if present
        if (value.size() >= 2 && (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0)) {
            value = value.substr(2);
        }
        // Remove # prefix if present
        if (!value.empty() && value[0] == '#') {
            value = value.substr(1);
        }

        // Extract only hex digits
        std::string normalized;
        for (char c : value) {
            if (std::isxdigit(static_cast<unsigned char>(c))) {
                normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            }
        }

        // If 6 hex digits, add FF alpha
        if (normalized.size() == 6) {
            normalized += "FF";
        }

        // Validate length - return default if invalid
        if (normalized.size() != 8) {
            return "00000000";
        }

        return normalized;
    };

    // Helper that populates a tray JSON object from an AmsTrayData entry.
    // slot_id is the AMS-local slot (0..3); afc_tool_number is the real AFC
    // "map = T<N>" value carried through to GCodeWriter::toolchange so that
    // G-code T# is independent of UI grouping order. -1 = no override.
    auto fill_tray_json = [&](const AmsTrayData* tray, int slot_id, int afc_tool_number,
                              nlohmann::json& tray_json, bool& has_filament_out) {
        tray_json["id"]      = std::to_string(slot_id);
        tray_json["tag_uid"] = "0000000000000000";
        if (afc_tool_number >= 0) {
            tray_json["afc_tool_number"] = afc_tool_number;
        }
        has_filament_out = tray && tray->has_filament;
        if (tray && tray->has_filament) {
            tray_json["tray_info_idx"] = tray->tray_info_idx;
            tray_json["tray_type"]     = tray->tray_type;
            tray_json["tray_color"]    = normalize_color(tray->tray_color);
            if (!tray->spoolman_id.empty()) {
                tray_json["spoolman_id"] = tray->spoolman_id;
            }
            if (!tray->filament_name.empty()) {
                tray_json["spoolman_filament_name"] = tray->filament_name;
            }
            if (tray->bed_temp > 0) {
                tray_json["bed_temp"] = std::to_string(tray->bed_temp);
            }
            if (tray->nozzle_temp > 0) {
                tray_json["nozzle_temp_max"] = std::to_string(tray->nozzle_temp);
                tray_json["nozzle_temp_min"] = std::to_string(tray->nozzle_temp);
            }
            if (!tray->vendor_name.empty()) {
                tray_json["spoolman_vendor_name"] = tray->vendor_name;
            }
        } else {
            tray_json["tray_info_idx"]         = "";
            tray_json["tray_type"]             = "";
            tray_json["tray_color"]            = "00000000";
            tray_json["tray_slot_placeholder"] = "1";
        }
    };

    // Encode the AMS "info" hex string. Low nibble (bits 0-3) is the AMS
    // type_id (2 = AMS_LITE). The extruder nibble (bits 8-11, read by the
    // parser via DevUtil::get_flag_bits(info, 8, 4)) is intentionally left
    // at 0 for AFC-sourced payloads: the real per-lane T# now travels via
    // the tray-level "afc_tool_number" field and is consumed by
    // GCodeWriter::toolchange. Leaving the nibble at 0 keeps every lane on
    // the "main" BBL extruder convention so the filament_ams_list sort key
    // (Plater.cpp: (GetExtruderId()?0:0x10000) + ams_id*4+slot_id) remains
    // stable regardless of how many Klipper toolheads the lanes map to.
    auto encode_info = [](int /*tool_number*/, int type_id) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%04X", (type_id & 0xF));
        return std::string(buf);
    };

    // Build BBL-format JSON for DevFilaSystemParser::ParseV1_0
    nlohmann::json ams_json  = nlohmann::json::object();
    nlohmann::json ams_array = nlohmann::json::array();

    // 64-bit bitmasks: supports up to 64 lanes (16 AMS units * 4 slots).
    uint64_t ams_exist_bits  = 0;
    uint64_t tray_exist_bits = 0;

    // Detect whether the callers supplied AFC unit grouping. When any tray
    // carries a unit_name we group by (unit, extruder) and tag each synthesized
    // AMS unit with its extruder tool number. Otherwise fall back to the
    // legacy 4-lanes-per-AMS bucketing (extruder 0) used by the lane_data path.
    bool use_unit_grouping = false;
    for (const auto& t : trays) {
        if (!t.unit_name.empty()) {
            use_unit_grouping = true;
            break;
        }
    }

    if (use_unit_grouping) {
        // Group by (unit_name, extruder_tool_number), preserving original order.
        struct GroupKey
        {
            std::string unit;
            int         tool;
            bool operator==(const GroupKey& o) const { return unit == o.unit && tool == o.tool; }
        };
        std::vector<GroupKey>                       group_order;
        std::vector<std::vector<const AmsTrayData*>> groups;
        for (const auto& t : trays) {
            GroupKey key{t.unit_name, t.extruder_tool_number};
            auto     it = std::find(group_order.begin(), group_order.end(), key);
            if (it == group_order.end()) {
                group_order.push_back(key);
                groups.emplace_back();
                groups.back().push_back(&t);
            } else {
                groups[std::distance(group_order.begin(), it)].push_back(&t);
            }
        }

        // Sort lanes within each group by their original slot_index so slot_id
        // 0..3 follows the lane numbering inside the AFC unit.
        for (auto& g : groups) {
            std::sort(g.begin(), g.end(),
                      [](const AmsTrayData* a, const AmsTrayData* b) { return a->slot_index < b->slot_index; });
        }

        int next_ams_id = 0;
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            const auto& key   = group_order[gi];
            const auto& lanes = groups[gi];
            // Split groups larger than 4 lanes into multiple 4-slot AMS units.
            for (size_t chunk_start = 0; chunk_start < lanes.size(); chunk_start += 4) {
                size_t chunk_end = std::min(chunk_start + 4, lanes.size());

                nlohmann::json ams_unit = nlohmann::json::object();
                ams_unit["id"]          = std::to_string(next_ams_id);
                ams_unit["info"]        = encode_info(key.tool, /*type_id=AMS_LITE*/ 2);

                nlohmann::json tray_array = nlohmann::json::array();
                for (size_t slot_id = 0; slot_id < chunk_end - chunk_start; ++slot_id) {
                    const AmsTrayData* tray = lanes[chunk_start + slot_id];
                    nlohmann::json     tray_json;
                    bool               has_filament = false;
                    // Carry the real AFC tool number (the lane's "map=T<N>") so
                    // GCodeWriter::toolchange can emit the correct T# regardless
                    // of where this tray ends up in the sidebar's iteration order.
                    int afc_tool = tray ? tray->extruder_tool_number : -1;
                    fill_tray_json(tray, static_cast<int>(slot_id), afc_tool, tray_json, has_filament);
                    if (has_filament && next_ams_id < 16) {
                        tray_exist_bits |= (1ULL << (next_ams_id * 4 + slot_id));
                    }
                    tray_array.push_back(tray_json);
                }

                ams_unit["tray"] = tray_array;
                ams_array.push_back(ams_unit);
                if (next_ams_id < 64) {
                    ams_exist_bits |= (1ULL << next_ams_id);
                }
                ++next_ams_id;
            }
        }
    } else {
        // Legacy path: bucket every 4 lanes into a new AMS unit, all on extruder 0.
        // Here each tray's slot_index already equals the real T# (fetch_afc_status
        // sets slot_index = tool_number), so afc_tool == slot_index.
        for (int ams_id = 0; ams_id < ams_count; ++ams_id) {
            if (ams_id < 64) {
                ams_exist_bits |= (1ULL << ams_id);
            }

            nlohmann::json ams_unit = nlohmann::json::object();
            ams_unit["id"]          = std::to_string(ams_id);
            ams_unit["info"]        = "0002"; // AMS_LITE, extruder 0

            nlohmann::json tray_array           = nlohmann::json::array();
            int            max_slot_in_this_ams = std::min(3, max_lane_index - ams_id * 4);
            for (int slot_id = 0; slot_id <= max_slot_in_this_ams; ++slot_id) {
                int                slot_index = ams_id * 4 + slot_id;
                const AmsTrayData* tray       = nullptr;
                for (const auto& t : trays) {
                    if (t.slot_index == slot_index) {
                        tray = &t;
                        break;
                    }
                }
                nlohmann::json tray_json;
                bool           has_filament = false;
                fill_tray_json(tray, slot_id, slot_index, tray_json, has_filament);
                if (has_filament && slot_index < 64) {
                    tray_exist_bits |= (1ULL << slot_index);
                }
                tray_array.push_back(tray_json);
            }
            ams_unit["tray"] = tray_array;
            ams_array.push_back(ams_unit);
        }
    }

    // Format as hex strings (matching BBL protocol)
    std::ostringstream ams_exist_ss;
    ams_exist_ss << std::hex << std::uppercase << ams_exist_bits;
    std::ostringstream tray_exist_ss;
    tray_exist_ss << std::hex << std::uppercase << tray_exist_bits;

    ams_json["ams"]             = ams_array;
    ams_json["ams_exist_bits"]  = ams_exist_ss.str();
    ams_json["tray_exist_bits"] = tray_exist_ss.str();

    // Wrap in the expected structure for ParseV1_0
    nlohmann::json print_json = nlohmann::json::object();
    print_json["ams"]         = ams_json;

    // Call the parser to populate DevFilaSystem
    DevFilaSystemParser::ParseV1_0(print_json, obj, obj->GetFilaSystem(), false);
    BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent::build_ams_payload: Parsed " << trays.size() << " trays";

    // Set printer_type so update_sync_status() can match it against the preset's printer type.
    // Without this, the comparison fails and all sync badges are cleared.
    obj->printer_type = device_info.model_id;

    // Set push counters so is_info_ready() returns true for pull-mode agents.
    if (obj->m_push_count == 0) {
        obj->m_push_count = 1;
    }
    if (obj->m_full_msg_count == 0) {
        obj->m_full_msg_count = 1;
    }
    obj->last_push_time = std::chrono::system_clock::now();

    // Set storage state - Moonraker printers use virtual_sdcard, storage is always available.
    // This is required for SelectMachineDialog to allow printing (otherwise it blocks with "No SD card").
    obj->GetStorage()->set_sdcard_state(DevStorage::HAS_SDCARD_NORMAL);

    // Populate module_vers so is_info_ready() passes the version check.
    // Moonraker printers don't have BBL-style version info, but we need a non-empty map.
    if (obj->module_vers.empty()) {
        DevFirmwareVersionInfo ota_info;
        ota_info.name   = "ota";
        ota_info.sw_ver = "1.0.0"; // Placeholder version for Moonraker printers
        obj->module_vers.emplace("ota", ota_info);
    }
}

bool MoonrakerPrinterAgent::fetch_filament_info(std::string dev_id)
{
    // Prefer the live AFC status endpoint, which exposes the lane->extruder
    // mapping needed to place each lane under the correct OrcaSlicer extruder.
    // Fall back to the persistent Moonraker DB (lane_data) when AFC isn't
    // running or the endpoint is unavailable.
    {
        std::vector<AmsTrayData> trays;
        int                      max_lane_index = 0;
        if (fetch_afc_status(trays, max_lane_index) && !trays.empty()) {
            int ams_count = (max_lane_index + 4) / 4; // only used by legacy path; grouping ignores it
            build_ams_payload(ams_count, max_lane_index, trays);
            return true;
        }
    }

    // Fetch AFC lane data from Moonraker database (inline)
    std::string url = join_url(device_info.base_url, "/server/database/item?namespace=lane_data");

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!device_info.api_key.empty()) {
        http.header("X-Api-Key", device_info.api_key);
    }
    http.timeout_connect(5)
        .timeout_max(10)
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
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent::fetch_filament_info: Failed to fetch lane data: " << http_error;
        return false;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent::fetch_filament_info: Invalid JSON response";
        return false;
    }

    // Expected structure: { "result": { "namespace": "lane_data", "value": { "lane1": {...}, ... } } }
    if (!json.contains("result") || !json["result"].contains("value") || !json["result"]["value"].is_object()) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent::fetch_filament_info: Unexpected JSON structure or no lane_data found";
        return false;
    }

    // Parse response into AmsTrayData
    const auto&              value = json["result"]["value"];
    std::vector<AmsTrayData> trays;
    int                      max_lane_index = 0;

    // Null-safe JSON accessors: nlohmann::json::value() throws type_error
    // when the key exists but the value is null (type mismatch).
    auto safe_string = [](const nlohmann::json& obj, const char* key) -> std::string {
        auto it = obj.find(key);
        if (it != obj.end() && it->is_string())
            return it->get<std::string>();
        return "";
    };
    auto safe_string_or_number = [](const nlohmann::json& obj, const char* key) -> std::string {
        auto it = obj.find(key);
        if (it == obj.end())
            return "";
        if (it->is_string())
            return it->get<std::string>();
        if (it->is_number_integer())
            return std::to_string(it->get<long long>());
        if (it->is_number_unsigned())
            return std::to_string(it->get<unsigned long long>());
        return "";
    };
    auto safe_int = [](const nlohmann::json& obj, const char* key) -> int {
        auto it = obj.find(key);
        if (it != obj.end() && it->is_number())
            return it->get<int>();
        return 0;
    };
    struct SpoolmanFilamentData
    {
        std::string name;
        std::string spool_name;
        std::string spool_id;
        std::string filament_id;
        int         nozzle_temp{0};
        int         bed_temp{0};
        std::string color_hex;
        std::string vendor_name;
        std::string material;
    };

    auto read_spoolman_spool_data = [&](const nlohmann::json& spool_json) -> SpoolmanFilamentData {
        SpoolmanFilamentData data;
        data.spool_id = safe_string_or_number(spool_json, "id");
        if (spool_json.contains("name") && spool_json["name"].is_string())
            data.spool_name = spool_json["name"].get<std::string>();
        if (data.spool_name.empty() && spool_json.contains("comment") && spool_json["comment"].is_string())
            data.spool_name = spool_json["comment"].get<std::string>();
        if (spool_json.contains("filament") && spool_json["filament"].is_object()) {
            const auto& filament_json = spool_json["filament"];
            if (filament_json.contains("name") && filament_json["name"].is_string())
                data.name = filament_json["name"].get<std::string>();
            data.filament_id = safe_string_or_number(filament_json, "id");
            if (filament_json.contains("settings_extruder_temp") && filament_json["settings_extruder_temp"].is_number())
                data.nozzle_temp = filament_json["settings_extruder_temp"].get<int>();
            if (filament_json.contains("settings_bed_temp") && filament_json["settings_bed_temp"].is_number())
                data.bed_temp = filament_json["settings_bed_temp"].get<int>();
            if (filament_json.contains("color_hex") && filament_json["color_hex"].is_string())
                data.color_hex = filament_json["color_hex"].get<std::string>();
            if (filament_json.contains("vendor") && filament_json["vendor"].is_object()) {
                const auto& vendor_json = filament_json["vendor"];
                if (vendor_json.contains("name") && vendor_json["name"].is_string())
                    data.vendor_name = vendor_json["name"].get<std::string>();
            }
            if (filament_json.contains("material") && filament_json["material"].is_string())
                data.material = filament_json["material"].get<std::string>();
        }
        if (data.filament_id.empty()) {
            data.filament_id = safe_string_or_number(spool_json, "filament_id");
        }
        if (data.filament_id.empty() && spool_json.contains("filament") && !spool_json["filament"].is_object()) {
            if (spool_json["filament"].is_number_integer() || spool_json["filament"].is_number_unsigned()) {
                data.filament_id = std::to_string(spool_json["filament"].get<long long>());
            } else if (spool_json["filament"].is_string()) {
                data.filament_id = spool_json["filament"].get<std::string>();
            }
        }
        return data;
    };

    auto read_spoolman_filament_data = [&](const nlohmann::json& filament_json) -> SpoolmanFilamentData {
        SpoolmanFilamentData data;
        if (filament_json.contains("name") && filament_json["name"].is_string())
            data.name = filament_json["name"].get<std::string>();
        if (filament_json.contains("settings_extruder_temp") && filament_json["settings_extruder_temp"].is_number())
            data.nozzle_temp = filament_json["settings_extruder_temp"].get<int>();
        if (filament_json.contains("settings_bed_temp") && filament_json["settings_bed_temp"].is_number())
            data.bed_temp = filament_json["settings_bed_temp"].get<int>();
        if (filament_json.contains("color_hex") && filament_json["color_hex"].is_string())
            data.color_hex = filament_json["color_hex"].get<std::string>();
        if (filament_json.contains("vendor") && filament_json["vendor"].is_object()) {
            const auto& vendor_json = filament_json["vendor"];
            if (vendor_json.contains("name") && vendor_json["name"].is_string())
                data.vendor_name = vendor_json["name"].get<std::string>();
        }
        if (filament_json.contains("material") && filament_json["material"].is_string())
            data.material = filament_json["material"].get<std::string>();
        return data;
    };

    auto fetch_spoolman_filament_data = [&](const std::string& spool_id) -> SpoolmanFilamentData {
        SpoolmanFilamentData data;
        if (spool_id.empty())
            return data;

        auto build_spoolman_base_url = [&](const std::string& base_url, const std::string& spoolman_host) {
            constexpr int spoolman_port = 7912;
            const bool    has_override  = !spoolman_host.empty();
            std::string   host_input    = has_override ? spoolman_host : base_url;
            if (host_input.empty()) {
                return std::string{};
            }
            std::string scheme = "http://";
            std::string host   = host_input;
            auto        pos    = host.find("://");
            if (pos != std::string::npos) {
                scheme = host.substr(0, pos + 3);
                host   = host.substr(pos + 3);
            }
            auto slash_pos = host.find('/');
            if (slash_pos != std::string::npos) {
                host = host.substr(0, slash_pos);
            }
            if (host.empty()) {
                return std::string{};
            }
            auto has_port = [](const std::string& host_value) {
                auto colon_pos = host_value.rfind(':');
                if (colon_pos == std::string::npos || host_value.find(']') != std::string::npos) {
                    return false;
                }
                auto port_part = host_value.substr(colon_pos + 1);
                return !port_part.empty() &&
                       std::all_of(port_part.begin(), port_part.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
            };
            auto strip_port = [&](const std::string& host_value) {
                if (!has_port(host_value)) {
                    return host_value;
                }
                return host_value.substr(0, host_value.rfind(':'));
            };
            if (!has_override) {
                host = strip_port(host);
                return scheme + host + ":" + std::to_string(spoolman_port);
            }
            if (!has_port(host)) {
                return scheme + host + ":" + std::to_string(spoolman_port);
            }
            return scheme + host;
        };

        std::string spoolman_host_override;
        if (auto* preset_bundle = GUI::wxGetApp().preset_bundle) {
            if (preset_bundle->physical_printers.has_selection()) {
                spoolman_host_override = preset_bundle->physical_printers.get_selected_printer().config.opt_string("spoolman_host");
            }
        }
        if (auto* app_config = GUI::wxGetApp().app_config) {
            if (spoolman_host_override.empty()) {
                spoolman_host_override = app_config->get("spoolman_ip", device_info.dev_id);
            }
        }
        auto spoolman_base_url  = build_spoolman_base_url(device_info.base_url, spoolman_host_override);
        auto fetch_spoolman_url = [&](const std::string& path) {
            if (spoolman_base_url.empty()) {
                return std::string{};
            }
            std::string body;
            bool        ok   = false;
            auto        http = Http::get(join_url(spoolman_base_url, path));
            http.timeout_connect(5)
                .timeout_max(10)
                .on_complete([&](std::string response, unsigned status) {
                    if (status == 200) {
                        body = std::move(response);
                        ok   = true;
                    }
                })
                .perform_sync();
            return ok ? body : std::string{};
        };

        auto read_spoolman_response = [&](const std::string& body, bool expect_spool) {
            if (body.empty()) {
                return false;
            }
            auto response_json = nlohmann::json::parse(body, nullptr, false, true);
            if (response_json.is_discarded()) {
                return false;
            }
            const nlohmann::json* payload = &response_json;
            if (response_json.contains("result")) {
                payload = &response_json["result"];
            }
            if (expect_spool && payload->is_object()) {
                data = read_spoolman_spool_data(*payload);
                return !data.spool_name.empty() || !data.name.empty();
            }
            if (!expect_spool && payload->is_object()) {
                data = read_spoolman_filament_data(*payload);
                return !data.name.empty();
            }
            if (!expect_spool && payload->is_array()) {
                for (const auto& entry : *payload) {
                    if (!entry.is_object())
                        continue;
                    auto entry_id = safe_string_or_number(entry, "id");
                    if (entry_id == spool_id) {
                        data = read_spoolman_filament_data(entry);
                        return !data.name.empty();
                    }
                }
            }
            return false;
        };

        auto fetch_filament_from_id = [&](const std::string& filament_id) {
            if (filament_id.empty()) {
                return false;
            }
            return read_spoolman_response(fetch_spoolman_url("/api/v1/filament/" + filament_id), false);
        };

        if (read_spoolman_response(fetch_spoolman_url("/api/v1/spool/" + spool_id), true)) {
            if (data.name.empty() && !data.filament_id.empty()) {
                fetch_filament_from_id(data.filament_id);
            }
            return data;
        }
        if (read_spoolman_response(fetch_spoolman_url("/api/v1/filament/" + spool_id), false)) {
            return data;
        }
        if (read_spoolman_response(fetch_spoolman_url("/api/v1/spool"), true)) {
            if (data.name.empty() && !data.filament_id.empty()) {
                fetch_filament_from_id(data.filament_id);
            }
            return data;
        }
        if (read_spoolman_response(fetch_spoolman_url("/api/v1/filament"), false)) {
            return data;
        }

        std::string proxy_path = "/server/spoolman/proxy?path=/api/v1/spool/" + spool_id;
        std::string proxy_url  = join_url(device_info.base_url, proxy_path);

        std::string proxy_body;
        bool        proxy_ok   = false;
        auto        proxy_http = Http::get(proxy_url);
        if (!device_info.api_key.empty()) {
            proxy_http.header("X-Api-Key", device_info.api_key);
        }
        proxy_http.timeout_connect(5)
            .timeout_max(10)
            .on_complete([&](std::string body, unsigned status) {
                if (status == 200) {
                    proxy_body = std::move(body);
                    proxy_ok   = true;
                }
            })
            .perform_sync();

        if (proxy_ok) {
            auto proxy_json = nlohmann::json::parse(proxy_body, nullptr, false, true);
            if (!proxy_json.is_discarded()) {
                const nlohmann::json* spool_json = &proxy_json;
                if (proxy_json.contains("result")) {
                    spool_json = &proxy_json["result"];
                }
                if (spool_json->is_object()) {
                    data = read_spoolman_spool_data(*spool_json);
                    if (!data.spool_name.empty() || !data.name.empty()) {
                        if (data.name.empty() && !data.filament_id.empty()) {
                            proxy_path = "/server/spoolman/proxy?path=/api/v1/filament/" + data.filament_id;
                            proxy_url  = join_url(device_info.base_url, proxy_path);
                            proxy_body.clear();
                            proxy_ok                 = false;
                            auto proxy_filament_http = Http::get(proxy_url);
                            if (!device_info.api_key.empty()) {
                                proxy_filament_http.header("X-Api-Key", device_info.api_key);
                            }
                            proxy_filament_http.timeout_connect(5)
                                .timeout_max(10)
                                .on_complete([&](std::string body, unsigned status) {
                                    if (status == 200) {
                                        proxy_body = std::move(body);
                                        proxy_ok   = true;
                                    }
                                })
                                .perform_sync();
                            if (proxy_ok) {
                                auto proxy_filament_json = nlohmann::json::parse(proxy_body, nullptr, false, true);
                                if (!proxy_filament_json.is_discarded()) {
                                    const nlohmann::json* filament_json = &proxy_filament_json;
                                    if (proxy_filament_json.contains("result")) {
                                        filament_json = &proxy_filament_json["result"];
                                    }
                                    if (filament_json->is_object()) {
                                        data = read_spoolman_filament_data(*filament_json);
                                    }
                                }
                            }
                        }
                        return data;
                    }
                }
            }
        }

        proxy_path = "/server/spoolman/proxy?path=/api/v1/filament/" + spool_id;
        proxy_url  = join_url(device_info.base_url, proxy_path);
        proxy_body.clear();
        proxy_ok                 = false;
        auto proxy_filament_http = Http::get(proxy_url);
        if (!device_info.api_key.empty()) {
            proxy_filament_http.header("X-Api-Key", device_info.api_key);
        }
        proxy_filament_http.timeout_connect(5)
            .timeout_max(10)
            .on_complete([&](std::string body, unsigned status) {
                if (status == 200) {
                    proxy_body = std::move(body);
                    proxy_ok   = true;
                }
            })
            .perform_sync();

        if (proxy_ok) {
            auto proxy_json = nlohmann::json::parse(proxy_body, nullptr, false, true);
            if (!proxy_json.is_discarded()) {
                const nlohmann::json* filament_json = &proxy_json;
                if (proxy_json.contains("result")) {
                    filament_json = &proxy_json["result"];
                }
                if (filament_json->is_object()) {
                    data = read_spoolman_filament_data(*filament_json);
                    if (!data.name.empty())
                        return data;
                }
            }
        }

        proxy_path = "/server/spoolman/proxy?path=/api/v1/spool";
        proxy_url  = join_url(device_info.base_url, proxy_path);
        proxy_body.clear();
        proxy_ok              = false;
        auto proxy_spool_http = Http::get(proxy_url);
        if (!device_info.api_key.empty()) {
            proxy_spool_http.header("X-Api-Key", device_info.api_key);
        }
        proxy_spool_http.timeout_connect(5)
            .timeout_max(10)
            .on_complete([&](std::string body, unsigned status) {
                if (status == 200) {
                    proxy_body = std::move(body);
                    proxy_ok   = true;
                }
            })
            .perform_sync();

        if (proxy_ok) {
            auto proxy_json = nlohmann::json::parse(proxy_body, nullptr, false, true);
            if (!proxy_json.is_discarded() && proxy_json.is_array()) {
                for (const auto& entry : proxy_json) {
                    if (!entry.is_object())
                        continue;
                    auto entry_id = safe_string_or_number(entry, "id");
                    if (entry_id == spool_id) {
                        data = read_spoolman_spool_data(entry);
                        if (!data.name.empty() || !data.spool_name.empty()) {
                            if (data.name.empty() && !data.filament_id.empty()) {
                                proxy_path = "/server/spoolman/proxy?path=/api/v1/filament/" + data.filament_id;
                                proxy_url  = join_url(device_info.base_url, proxy_path);
                                proxy_body.clear();
                                proxy_ok                 = false;
                                auto proxy_filament_http = Http::get(proxy_url);
                                if (!device_info.api_key.empty()) {
                                    proxy_filament_http.header("X-Api-Key", device_info.api_key);
                                }
                                proxy_filament_http.timeout_connect(5)
                                    .timeout_max(10)
                                    .on_complete([&](std::string body, unsigned status) {
                                        if (status == 200) {
                                            proxy_body = std::move(body);
                                            proxy_ok   = true;
                                        }
                                    })
                                    .perform_sync();
                                if (proxy_ok) {
                                    auto proxy_filament_json = nlohmann::json::parse(proxy_body, nullptr, false, true);
                                    if (!proxy_filament_json.is_discarded()) {
                                        const nlohmann::json* filament_json = &proxy_filament_json;
                                        if (proxy_filament_json.contains("result")) {
                                            filament_json = &proxy_filament_json["result"];
                                        }
                                        if (filament_json->is_object()) {
                                            data = read_spoolman_filament_data(*filament_json);
                                        }
                                    }
                                }
                            }
                            return data;
                        }
                    }
                }
            }
        }

        proxy_path = "/server/spoolman/proxy?path=/api/v1/filament";
        proxy_url  = join_url(device_info.base_url, proxy_path);
        proxy_body.clear();
        proxy_ok             = false;
        auto proxy_list_http = Http::get(proxy_url);
        if (!device_info.api_key.empty()) {
            proxy_list_http.header("X-Api-Key", device_info.api_key);
        }
        proxy_list_http.timeout_connect(5)
            .timeout_max(10)
            .on_complete([&](std::string body, unsigned status) {
                if (status == 200) {
                    proxy_body = std::move(body);
                    proxy_ok   = true;
                }
            })
            .perform_sync();
        if (proxy_ok) {
            auto proxy_json = nlohmann::json::parse(proxy_body, nullptr, false, true);
            if (!proxy_json.is_discarded() && proxy_json.is_array()) {
                for (const auto& entry : proxy_json) {
                    if (!entry.is_object())
                        continue;
                    auto entry_id = safe_string_or_number(entry, "id");
                    if (entry_id == spool_id) {
                        data = read_spoolman_filament_data(entry);
                        if (!data.name.empty())
                            return data;
                    }
                }
            }
        }

        return data;
    };

    for (const auto& [lane_key, lane_obj] : value.items()) {
        if (!lane_obj.is_object()) {
            continue;
        }

        // Extract lane index from the "lane" field (tool number, 0-based)
        std::string lane_str   = safe_string(lane_obj, "lane");
        int         lane_index = -1;
        if (!lane_str.empty()) {
            try {
                lane_index = std::stoi(lane_str);
            } catch (...) {
                lane_index = -1;
            }
        }

        if (lane_index < 0) {
            continue;
        }

        AmsTrayData tray;
        tray.slot_index  = lane_index;
        tray.tray_color  = safe_string(lane_obj, "color");
        tray.tray_type   = safe_string(lane_obj, "material");
        tray.spoolman_id = safe_string_or_number(lane_obj, "spoolman_id");
        if (tray.spoolman_id.empty()) {
            tray.spoolman_id = safe_string_or_number(lane_obj, "spool_id");
        }
        if (tray.spoolman_id.empty() && lane_obj.contains("spool")) {
            const auto& spool_value = lane_obj["spool"];
            if (spool_value.is_object()) {
                tray.spoolman_id = safe_string_or_number(spool_value, "id");
                if (tray.spoolman_id.empty())
                    tray.spoolman_id = safe_string_or_number(spool_value, "spool_id");
            } else if (spool_value.is_string() || spool_value.is_number_integer() || spool_value.is_number()) {
                tray.spoolman_id = spool_value.dump();
                if (spool_value.is_string())
                    tray.spoolman_id = spool_value.get<std::string>();
            }
        }
        auto filament_data = fetch_spoolman_filament_data(tray.spoolman_id);
        if (tray.spoolman_id.empty() && !filament_data.spool_id.empty()) {
            tray.spoolman_id = filament_data.spool_id;
        }
        auto build_display_name = [](const SpoolmanFilamentData& data) {
            auto looks_like_numeric = [](std::string value) {
                boost::algorithm::trim(value);
                if (value.empty())
                    return false;
                if (value.front() == '#') {
                    value.erase(value.begin());
                    boost::algorithm::trim(value);
                }
                return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
            };
            std::string base_name = data.spool_name;
            if (!data.spool_id.empty() && (base_name == data.spool_id || looks_like_numeric(base_name))) {
                base_name.clear();
            }
            if (base_name.empty()) {
                if (!data.vendor_name.empty())
                    base_name += data.vendor_name;
                if (!data.name.empty())
                    base_name += (base_name.empty() ? "" : " ") + data.name;
                if (!data.material.empty())
                    base_name += (base_name.empty() ? "" : " ") + data.material;
            }
            if (base_name.empty())
                base_name = data.material;
            if (!data.spool_id.empty() && !base_name.empty() && base_name.find(data.spool_id) == std::string::npos) {
                base_name += " #" + data.spool_id;
            }
            return base_name;
        };
        tray.filament_name = build_display_name(filament_data);
        if (tray.filament_name.empty()) {
            tray.filament_name = filament_data.name;
        }
        tray.vendor_name = filament_data.vendor_name;
        if (tray.filament_name.empty()) {
            tray.filament_name = safe_string(lane_obj, "name");
        }
        if (tray.filament_name.empty()) {
            tray.filament_name = safe_string(lane_obj, "spool_name");
        }
        if (tray.filament_name.empty()) {
            tray.filament_name = safe_string(lane_obj, "filament_name");
        }
        if (tray.filament_name.empty()) {
            tray.filament_name = safe_string(lane_obj, "material");
        }
        if (tray.tray_type.empty() && !filament_data.material.empty()) {
            tray.tray_type = filament_data.material;
        }
        if (tray.tray_type.empty()) {
            tray.tray_type = safe_string(lane_obj, "material");
        }
        if (tray.nozzle_temp == 0 && filament_data.nozzle_temp > 0) {
            tray.nozzle_temp = filament_data.nozzle_temp;
        }
        if (tray.bed_temp == 0 && filament_data.bed_temp > 0) {
            tray.bed_temp = filament_data.bed_temp;
        }
        if (tray.tray_color.empty() && !filament_data.color_hex.empty()) {
            tray.tray_color = filament_data.color_hex;
        }
        int lane_bed_temp    = safe_int(lane_obj, "bed_temp");
        int lane_nozzle_temp = safe_int(lane_obj, "nozzle_temp");
        if (lane_bed_temp > 0) {
            tray.bed_temp = lane_bed_temp;
        }
        if (lane_nozzle_temp > 0) {
            tray.nozzle_temp = lane_nozzle_temp;
        }
        tray.has_filament  = !tray.tray_type.empty();
        auto* bundle       = GUI::wxGetApp().preset_bundle;
        tray.tray_info_idx = bundle ? bundle->filaments.filament_id_by_type(tray.tray_type) :
                                      map_filament_type_to_generic_id(tray.tray_type);

        max_lane_index = std::max(max_lane_index, lane_index);
        trays.push_back(tray);
    }

    if (trays.empty()) {
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent::fetch_filament_info: No AFC lanes found";
        return false;
    }

    // Calculate AMS count from max lane index (4 trays per AMS unit)
    int ams_count = (max_lane_index + 4) / 4;

    // Build and parse the AMS payload
    build_ams_payload(ams_count, max_lane_index, trays);
    return true;
}

std::string MoonrakerPrinterAgent::trim_and_upper(const std::string& input)
{
    std::string result = input;
    boost::trim(result);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

std::string MoonrakerPrinterAgent::map_filament_type_to_generic_id(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

    // Map to OrcaFilamentLibrary preset IDs (compatible with all printers)
    // Source: resources/profiles/OrcaFilamentLibrary/filament/

    // PLA variants
    if (upper == "PLA")
        return "OGFL99";
    if (upper == "PLA-CF")
        return "OGFL98";
    if (upper == "PLA SILK" || upper == "PLA-SILK")
        return "OGFL96";
    if (upper == "PLA HIGH SPEED" || upper == "PLA-HS" || upper == "PLA HS")
        return "OGFL95";

    // ABS/ASA variants
    if (upper == "ABS")
        return "OGFB99";
    if (upper == "ASA")
        return "OGFB98";

    // PETG/PET variants
    if (upper == "PETG" || upper == "PET")
        return "OGFG99";
    if (upper == "PCTG")
        return "OGFG97";

    // PA/Nylon variants
    if (upper == "PA" || upper == "NYLON")
        return "OGFN99";
    if (upper == "PA-CF")
        return "OGFN98";
    if (upper == "PPA" || upper == "PPA-CF")
        return "OGFN97";
    if (upper == "PPA-GF")
        return "OGFN96";

    // PC variants
    if (upper == "PC")
        return "OGFC99";

    // PP/PE variants
    if (upper == "PE")
        return "OGFP99";
    if (upper == "PP")
        return "OGFP97";

    // Support materials
    if (upper == "PVA")
        return "OGFS99";
    if (upper == "HIPS")
        return "OGFS98";
    if (upper == "BVOH")
        return "OGFS97";

    // TPU variants
    if (upper == "TPU")
        return "OGFU99";

    // Other materials
    if (upper == "EVA")
        return "OGFR99";
    if (upper == "PHA")
        return "OGFR98";
    if (upper == "COPE")
        return "OGFLC99";
    if (upper == "SBS")
        return "OFLSBS99";

    // Unknown material
    return UNKNOWN_FILAMENT_ID;
}

int MoonrakerPrinterAgent::handle_request(const std::string& dev_id, const std::string& json_str)
{
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

        // Print control commands
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
                int         temp  = json["print"]["temp"].get<int>();
                std::string gcode = "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=" + std::to_string(temp);
                send_gcode(dev_id, gcode);
                return BAMBU_NETWORK_SUCCESS;
            }
        }

        // Nozzle temperature - UI sends "target_temp" and "extruder_index" fields
        if (cmd == "set_nozzle_temp") {
            if (json["print"].contains("target_temp") && json["print"]["target_temp"].is_number()) {
                int temp         = json["print"]["target_temp"].get<int>();
                int extruder_idx = 0; // Default to main extruder
                if (json["print"].contains("extruder_index") && json["print"]["extruder_index"].is_number()) {
                    extruder_idx = json["print"]["extruder_index"].get<int>();
                }
                std::string heater = (extruder_idx == 0) ? "extruder" : "extruder" + std::to_string(extruder_idx);
                std::string gcode  = "SET_HEATER_TEMPERATURE HEATER=" + heater + " TARGET=" + std::to_string(temp);
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

bool MoonrakerPrinterAgent::init_device_info(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    device_info         = MoonrakerDeviceInfo{};
    auto* preset_bundle = GUI::wxGetApp().preset_bundle;
    if (!preset_bundle) {
        return false;
    }

    auto&       preset      = preset_bundle->printers.get_edited_preset();
    const auto& printer_cfg = preset.config;
    device_info.dev_ip      = dev_ip;

    device_info.api_key    = password;
    device_info.model_name = printer_cfg.opt_string("printer_model");
    device_info.model_id   = preset.get_printer_type(preset_bundle);
    device_info.base_url   = use_ssl ? "https://" + dev_ip : "http://" + dev_ip;
    device_info.dev_id     = dev_id;
    device_info.version    = "";
    device_info.dev_name   = device_info.dev_id;

    return true;
}

bool MoonrakerPrinterAgent::fetch_device_info(const std::string&   base_url,
                                              const std::string&   api_key,
                                              MoonrakerDeviceInfo& info,
                                              std::string&         error) const
{
    auto fetch_json = [&](const std::string& url, nlohmann::json& out) {
        std::string response_body;
        bool        success = false;
        std::string http_error;

        auto http = Http::get(url);
        if (!api_key.empty()) {
            http.header("X-Api-Key", api_key);
        }
        http.timeout_connect(5)
            .timeout_max(10)
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
    info.version          = result.value("moonraker_version", "");
    info.klippy_state     = result.value("klippy_state", "");

    return true;
}

bool MoonrakerPrinterAgent::query_printer_status(const std::string& base_url,
                                                 const std::string& api_key,
                                                 nlohmann::json&    status,
                                                 std::string&       error) const
{
    std::string url = join_url(base_url, "/printer/objects/query?print_stats&virtual_sdcard&extruder&heater_bed&fan");

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.timeout_connect(5)
        .timeout_max(10)
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
    nlohmann::json payload;
    payload["script"]       = gcode;
    std::string payload_str = payload.dump();

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::post(join_url(device_info.base_url, "/printer/gcode/script"));
    if (!device_info.api_key.empty()) {
        http.header("X-Api-Key", device_info.api_key);
    }
    http.header("Content-Type", "application/json")
        .set_post_body(payload_str)
        .timeout_connect(5)
        .timeout_max(10)
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
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: send_gcode failed: " << http_error;
        return false;
    }

    return true;
}

bool MoonrakerPrinterAgent::fetch_object_list(const std::string&     base_url,
                                              const std::string&     api_key,
                                              std::set<std::string>& objects,
                                              std::string&           error) const
{
    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(join_url(base_url, "/printer/objects/list"));
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.timeout_connect(5)
        .timeout_max(10)
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
    nlohmann::json payload;
    payload["info"]["command"] = "get_version";
    payload["info"]["result"]  = "success";
    payload["info"]["module"]  = nlohmann::json::array();

    nlohmann::json module;
    module["name"]         = "ota";
    module["sw_ver"]       = device_info.version;
    module["product_name"] = "Moonraker";
    payload["info"]["module"].push_back(module);

    dispatch_message(dev_id, payload.dump());
    return BAMBU_NETWORK_SUCCESS;
}

int MoonrakerPrinterAgent::send_access_code(const std::string& dev_id)
{
    nlohmann::json payload;
    payload["system"]["command"]     = "get_access_code";
    payload["system"]["access_code"] = device_info.api_key;
    dispatch_message(dev_id, payload.dump());
    return BAMBU_NETWORK_SUCCESS;
}

void MoonrakerPrinterAgent::announce_printhost_device()
{
    OnMsgArrivedFn ssdp_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        ssdp_fn = on_ssdp_msg_fn;
        if (!ssdp_fn) {
            return;
        }
        if (ssdp_announced_host == device_info.base_url && !ssdp_announced_id.empty()) {
            return;
        }
    }

    // Try to fetch actual device name from Moonraker
    // Priority: 1) Moonraker hostname, 2) Preset model name, 3) Generic fallback
    std::string         dev_name;
    MoonrakerDeviceInfo info;
    std::string         fetch_error;
    if (fetch_device_info(device_info.base_url, device_info.api_key, info, fetch_error) && !info.dev_name.empty()) {
        dev_name = info.dev_name;
    } else {
        dev_name = device_info.model_name.empty() ? "Moonraker Printer" : device_info.model_name;
    }

    const std::string model_id = device_info.model_id;

    if (auto* app_config = GUI::wxGetApp().app_config) {
        const std::string access_code = device_info.api_key.empty() ? "88888888" : device_info.api_key;
        app_config->set_str("access_code", device_info.dev_id, access_code);
        app_config->set_str("user_access_code", device_info.dev_id, access_code);
    }

    nlohmann::json payload;
    payload["dev_name"]     = dev_name;
    payload["dev_id"]       = device_info.dev_id;
    payload["dev_ip"]       = device_info.dev_ip;
    payload["dev_type"]     = model_id.empty() ? dev_name : model_id;
    payload["dev_signal"]   = "0";
    payload["connect_type"] = "lan";
    payload["bind_state"]   = "free";
    payload["sec_link"]     = "secure";
    payload["ssdp_version"] = "v1";

    ssdp_fn(payload.dump());

    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        ssdp_announced_host = device_info.base_url;
        ssdp_announced_id   = device_info.dev_id;

        // Set this as the selected machine if nothing is currently selected
        if (selected_machine.empty()) {
            selected_machine = device_info.dev_id;
        }
    }
}

void MoonrakerPrinterAgent::dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg)
{
    OnLocalConnectedFn local_fn;
    QueueOnMainFn      queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
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
    ws_thread = std::thread([this, dev_id, base_url, api_key]() { run_status_stream(dev_id, base_url, api_key); });
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
    ws_reconnect_requested.store(false); // Reset reconnect flag
    int       retry_count   = 0;
    const int max_retries   = 10;
    const int base_delay_ms = 1000;

    while (!ws_stop.load() && retry_count < max_retries) {
        bool connection_lost = false; // Flag to distinguish clean shutdown from unexpected disconnect

        try {
            net::io_context   ioc;
            tcp::resolver     resolver{ioc};
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
            identify["jsonrpc"]               = "2.0";
            identify["method"]                = "server.connection.identify";
            identify["params"]["client_name"] = "OrcaSlicer";
            identify["params"]["version"]     = MoonrakerPrinterAgent_VERSION;
            identify["params"]["type"]        = "agent";
            identify["params"]["url"]         = "https://github.com/SoftFever/OrcaSlicer";
            identify["id"]                    = 0;
            ws.write(net::buffer(identify.dump()));

            std::set<std::string> subscribe_objects = {"print_stats", "virtual_sdcard"};
            std::set<std::string> available_objects;
            std::string           list_error;
            if (fetch_object_list(base_url, api_key, available_objects, list_error)) {
                {
                    std::lock_guard<std::recursive_mutex> lock(payload_mutex);
                    this->available_objects = std::move(available_objects);
                }

                if (this->available_objects.count("heater_bed") != 0) {
                    subscribe_objects.insert("heater_bed");
                }
                if (this->available_objects.count("fan") != 0) {
                    subscribe_objects.insert("fan");
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
                subscribe_objects.insert("toolhead"); // Add toolhead as fallback
                subscribe_objects.insert("fan");      // Try to subscribe to fan as fallback
            }

            nlohmann::json subscribe;
            subscribe["jsonrpc"]   = "2.0";
            subscribe["method"]    = "printer.objects.subscribe";
            nlohmann::json objects = nlohmann::json::object();
            for (const auto& name : subscribe_objects) {
                objects[name] = nullptr;
            }
            subscribe["params"]["objects"] = std::move(objects);
            subscribe["id"]                = 1;
            ws.write(net::buffer(subscribe.dump()));

            // Read loop
            while (!ws_stop.load()) {
                ws.next_layer().expires_after(std::chrono::seconds(2));
                beast::flat_buffer buffer;
                beast::error_code  ec;
                ws.read(buffer, ec);
                if (ec == beast::error::timeout) {
                    const auto now_ms = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
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

    bool updated     = false;
    bool is_critical = false; // Track if this is a critical update that bypasses throttle

    // Check for subscription response (has "result.status") - initial subscription is critical
    if (json.contains("result") && json["result"].contains("status") && json["result"]["status"].is_object()) {
        update_status_cache(json["result"]["status"]);
        updated     = true;
        is_critical = true; // Initial subscription response - dispatch immediately
    }

    // Check for status update notifications
    if (json.contains("method") && json["method"].is_string()) {
        const std::string method = json["method"].get<std::string>();
        if (method == "notify_status_update" && json.contains("params") && json["params"].is_array() && !json["params"].empty() &&
            json["params"][0].is_object()) {
            update_status_cache(json["params"][0]);
            updated = true;
            // Note: is_critical stays false for regular status updates (telemetry)
        } else if (method == "notify_klippy_ready") {
            nlohmann::json updates;
            updates["print_stats"]["state"] = "standby";
            update_status_cache(updates);
            updated     = true;
            is_critical = true; // Klippy events are critical
        } else if (method == "notify_klippy_shutdown") {
            nlohmann::json updates;
            updates["print_stats"]["state"] = "error";
            update_status_cache(updates);
            updated     = true;
            is_critical = true; // Klippy events are critical
        }
        // Handle Klippy disconnect - update status and trigger reconnection
        else if (method == "notify_klippy_disconnected") {
            // Klippy disconnected - update status to reflect disconnect state
            nlohmann::json updates;
            updates["print_stats"]["state"] = "error";
            update_status_cache(updates);
            updated     = true;
            is_critical = true; // Klippy events are critical
            // Set flag to trigger reconnection after dispatching the status update
            ws_reconnect_requested.store(true);
            BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent: Klippy disconnected, triggering reconnection";
        }
    }

    // Check for print state changes (critical - always dispatch immediately)
    if (updated && !is_critical) {
        std::string current_state;
        {
            std::lock_guard<std::recursive_mutex> lock(payload_mutex);
            if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("state") &&
                status_cache["print_stats"]["state"].is_string()) {
                current_state = status_cache["print_stats"]["state"].get<std::string>();
            }
        }

        if (!current_state.empty() && current_state != last_print_state) {
            is_critical      = true;
            last_print_state = current_state;
        }
    }

    if (updated) {
        const auto now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        const auto last_dispatch_ms = ws_last_dispatch_ms.load();

        // Dispatch if: critical change OR throttle interval elapsed
        const bool should_dispatch = is_critical || last_dispatch_ms == 0 || now_ms - last_dispatch_ms >= STATUS_UPDATE_INTERVAL_MS;

        if (should_dispatch) {
            nlohmann::json message;
            {
                std::lock_guard<std::recursive_mutex> lock(payload_mutex);
                message = build_print_payload_locked();
            }

            dispatch_message(dev_id, message.dump());
            ws_last_dispatch_ms.store(now_ms);
            ws_last_emit_ms.store(now_ms); // Also update heartbeat timer
        }
        // else: skip dispatch, cache is updated for next dispatch cycle
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

    // Map Moonraker state to Bambu stage numbers
    int mc_print_stage = 0;
    if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("state")) {
        std::string mr_state = status_cache["print_stats"]["state"].get<std::string>();
        if (mr_state == "printing")
            mc_print_stage = 1;
        else if (mr_state == "paused")
            mc_print_stage = 2;
        else if (mr_state == "complete")
            mc_print_stage = 3;
        else if (mr_state == "error")
            mc_print_stage = 4;
    }
    payload["print"]["mc_print_stage"] = mc_print_stage;

    // Leave mc_print_error_code and print_error at 0
    // UI expects numeric HMS codes - setting to 1 shows generic error dialog
    // Only set if real mapping from Moonraker error strings to HMS codes is defined
    payload["print"]["mc_print_error_code"] = 0;
    payload["print"]["print_error"]         = 0;

    // Map homed axes to bit field: X=bit0, Y=bit1, Z=bit2
    // WARNING: This only sets bits 0-2, clearing support flags (bit 3+)
    // Bit 3 = 220V voltage, bit 4 = auto recovery, etc.
    // This is acceptable for Moonraker (no AMS, different feature set)
    int home_flag = 0;
    if (status_cache.contains("toolhead") && status_cache["toolhead"].contains("homed_axes")) {
        std::string homed = status_cache["toolhead"]["homed_axes"].get<std::string>();
        if (homed.find('X') != std::string::npos)
            home_flag |= 1; // bit 0
        if (homed.find('Y') != std::string::npos)
            home_flag |= 2; // bit 1
        if (homed.find('Z') != std::string::npos)
            home_flag |= 4; // bit 2
    }
    payload["print"]["home_flag"] = home_flag;

    // Moonraker doesn't provide temperature ranges via API - use hardcoded defaults
    payload["print"]["nozzle_temp_range"] = {100, 370}; // Typical Klipper range
    payload["print"]["bed_temp_range"]    = {0, 120};   // Typical bed range

    payload["print"]["support_send_to_sd"] = true;
    // Detect bed_leveling support from available objects (bed_mesh or probe)
    // Default to 0 (not supported) if neither object exists
    bool has_bed_leveling                    = (available_objects.count("bed_mesh") != 0 || available_objects.count("probe") != 0);
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
            pwm                          = std::clamp(pwm, 0, 255);
            payload["print"]["fan_gear"] = pwm;
        } else if (fan.contains("power") && fan["power"].is_number()) {
            double power                 = fan["power"].get<double>();
            int    pwm                   = static_cast<int>(power * 255.0 + 0.5);
            pwm                          = std::clamp(pwm, 0, 255);
            payload["print"]["fan_gear"] = pwm;
        }
    }
    // If "fan" object doesn't exist, don't include fan_gear in payload

    if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("filename") &&
        status_cache["print_stats"]["filename"].is_string()) {
        payload["print"]["subtask_name"] = status_cache["print_stats"]["filename"].get<std::string>();
    }

    if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("filename")) {
        payload["print"]["gcode_file"] = status_cache["print_stats"]["filename"];
    }

    int mc_percent = -1;
    if (status_cache.contains("virtual_sdcard") && status_cache["virtual_sdcard"].contains("progress") &&
        status_cache["virtual_sdcard"]["progress"].is_number()) {
        const double progress = status_cache["virtual_sdcard"]["progress"].get<double>();
        if (progress >= 0.0) {
            mc_percent = std::clamp(static_cast<int>(progress * 100.0 + 0.5), 0, 100);
        }
    }
    if (mc_percent >= 0) {
        payload["print"]["mc_percent"] = mc_percent;
    }

    if (status_cache.contains("print_stats") && status_cache["print_stats"].contains("total_duration") &&
        status_cache["print_stats"].contains("print_duration") && status_cache["print_stats"]["total_duration"].is_number() &&
        status_cache["print_stats"]["print_duration"].is_number()) {
        const double total   = status_cache["print_stats"]["total_duration"].get<double>();
        const double elapsed = status_cache["print_stats"]["print_duration"].get<double>();
        if (total > 0.0 && elapsed >= 0.0) {
            const auto remaining_minutes          = std::max(0, static_cast<int>((total - elapsed) / 60.0));
            payload["print"]["mc_remaining_time"] = remaining_minutes;
        }
    }

    const auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    payload["t_utc"] = now_ms;

    return payload;
}

void MoonrakerPrinterAgent::dispatch_message(const std::string& dev_id, const std::string& payload)
{
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

bool MoonrakerPrinterAgent::upload_gcode(const std::string& local_path,
                                         const std::string& filename,
                                         const std::string& base_url,
                                         const std::string& api_key,
                                         OnUpdateStatusFn   update_fn,
                                         WasCancelledFn     cancel_fn)
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
    if (file_size > 1024 * 1024 * 1024) { // 1GB limit
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: File too large: " << file_size << " bytes";
        return false;
    }

    // Sanitize filename to prevent path traversal attacks
    std::string safe_filename = sanitize_filename(filename);

    bool        result = true;
    std::string http_error;

    // Use Http::form_add and Http::form_add_file
    auto http = Http::post(join_url(base_url, "/server/files/upload"));
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.form_add("root", "gcodes") // Upload to gcodes directory
        .form_add("print", "false") // Don't auto-start print
        .form_add_file("file", source_path.string(), safe_filename)
        .timeout_connect(5)
        .timeout_max(300) // 5 minutes for large files
        .on_complete([&](std::string body, unsigned status) {
            (void) body;
            (void) status;
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: Upload error: " << err << " HTTP " << status;
            http_error = err;
            result     = false;
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
    return send_gcode(dev_id, "PAUSE") ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
}

int MoonrakerPrinterAgent::resume_print(const std::string& dev_id)
{
    return send_gcode(dev_id, "RESUME") ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
}

int MoonrakerPrinterAgent::cancel_print(const std::string& dev_id)
{
    return send_gcode(dev_id, "CANCEL_PRINT") ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
}

bool MoonrakerPrinterAgent::send_jsonrpc_command(const std::string&    base_url,
                                                 const std::string&    api_key,
                                                 const nlohmann::json& request,
                                                 std::string&          response) const
{
    std::string request_str = request.dump();
    std::string url         = join_url(base_url, "/printer/print/start");

    bool        success = false;
    std::string http_error;

    auto http = Http::post(url);
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.header("Content-Type", "application/json")
        .set_post_body(request_str)
        .timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response = body;
                success  = true;
            } else {
                http_error = "HTTP " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) { http_error = err; })
        .perform_sync();

    if (!success) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: JSON-RPC command failed: " << http_error;
    }

    return success;
}

void MoonrakerPrinterAgent::perform_connection_async(const std::string& dev_id,
                                                     const std::string& base_url,
                                                     const std::string& api_key,
                                                     uint64_t           generation)
{
    auto is_stale = [&]() { return generation != connect_generation.load(); };

    int         result = BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    std::string error_msg;

    // Early exit if a newer connection was started before we begin
    if (is_stale()) {
        return;
    }

    try {
        MoonrakerDeviceInfo fetched_info;
        if (!fetch_device_info(base_url, api_key, fetched_info, error_msg)) {
            BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: Failed to fetch server info: " << error_msg;
            // Orca todo: revist here, for now don't send error, this is set current MachineObject to null
            // dispatch_local_connect(ConnectStatusFailed, dev_id, "server_info_failed");
            return;
        }

        // Commit fetched info back to device_info under lock, only if still current
        {
            std::lock_guard<std::recursive_mutex> lock(connect_mutex);
            if (is_stale()) {
                return;
            }
            device_info.dev_name     = fetched_info.dev_name;
            device_info.version      = fetched_info.version;
            device_info.klippy_state = fetched_info.klippy_state;
        }

// Orca todo: disable websocket for now, as we don't use MonitorPanel for Moonraker printers yet
#if 0
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
#endif

        // Success!
        result = BAMBU_NETWORK_SUCCESS;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "MoonrakerPrinterAgent: Connection exception: " << e.what();
        error_msg = std::string("exception: ") + e.what();
        result    = BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    }

    // Only dispatch if this connection is still the current one
    if (result == BAMBU_NETWORK_SUCCESS && !is_stale()) {
        dispatch_local_connect(ConnectStatusOk, dev_id, "0");
        dispatch_printer_connected(dev_id);
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent: connect_printer completed - dev_id=" << dev_id;
    } else if (result != BAMBU_NETWORK_SUCCESS && result != BAMBU_NETWORK_ERR_CANCELED) {
        // Orca todo: revist here, for now don't send error, this is set current MachineObject to null
        // dispatch_local_connect(ConnectStatusFailed, dev_id, error_msg);
    }
}

bool MoonrakerPrinterAgent::is_numeric(const std::string& value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string MoonrakerPrinterAgent::normalize_base_url(std::string host, const std::string& port)
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

std::string MoonrakerPrinterAgent::join_url(const std::string& base_url, const std::string& path) const
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

// Sanitize filename to prevent path traversal attacks
// Extracts only the basename, removing any path components
std::string MoonrakerPrinterAgent::sanitize_filename(const std::string& filename)
{
    if (filename.empty()) {
        return "print.gcode";
    }
    namespace fs = boost::filesystem;
    fs::path    p(filename);
    std::string basename = p.filename().string();
    if (basename.empty() || basename == "." || basename == "..") {
        return "print.gcode";
    }
    return basename;
}

bool MoonrakerPrinterAgent::fetch_afc_status(std::vector<AmsTrayData>& trays, int& max_lane_index)
{
    trays.clear();
    max_lane_index = 0;

    std::string url = join_url(device_info.base_url, "/printer/afc/status");

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!device_info.api_key.empty()) {
        http.header("X-Api-Key", device_info.api_key);
    }
    http.timeout_connect(5)
        .timeout_max(10)
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
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent::fetch_afc_status: endpoint unavailable (" << http_error
                                << "), falling back to lane_data";
        return false;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(warning) << "MoonrakerPrinterAgent::fetch_afc_status: invalid JSON";
        return false;
    }

    // Locate the AFC payload. Moonraker wraps responses in {"result": ...}.
    // The status block key is literally "status:" (trailing colon) in the
    // observed samples, but we'll accept either "status" or "status:" to be
    // resilient. Inside we expect an "AFC" object with unit keys and a
    // "system" sub-object describing extruders.
    const nlohmann::json* afc = nullptr;
    if (json.contains("result") && json["result"].is_object()) {
        const auto& result = json["result"];
        for (const char* key : {"status:", "status"}) {
            if (result.contains(key) && result[key].is_object() && result[key].contains("AFC") &&
                result[key]["AFC"].is_object()) {
                afc = &result[key]["AFC"];
                break;
            }
        }
        if (!afc && result.contains("AFC") && result["AFC"].is_object()) {
            afc = &result["AFC"];
        }
    }
    if (!afc) {
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent::fetch_afc_status: no AFC block in response";
        return false;
    }

    auto safe_string = [](const nlohmann::json& obj, const char* key) -> std::string {
        auto it = obj.find(key);
        if (it == obj.end() || it->is_null())
            return "";
        if (it->is_string())
            return it->get<std::string>();
        if (it->is_number_integer())
            return std::to_string(it->get<long long>());
        if (it->is_number_unsigned())
            return std::to_string(it->get<unsigned long long>());
        if (it->is_number())
            return std::to_string(it->get<double>());
        return "";
    };
    auto safe_int = [](const nlohmann::json& obj, const char* key) -> int {
        auto it = obj.find(key);
        if (it == obj.end() || it->is_null())
            return 0;
        if (it->is_number())
            return it->get<int>();
        if (it->is_string()) {
            try {
                return std::stoi(it->get<std::string>());
            } catch (...) {
                return 0;
            }
        }
        return 0;
    };

    // Build extruder name -> tool_number map from AFC system info.
    std::map<std::string, int> extruder_tool_map;
    if (afc->contains("system") && (*afc)["system"].is_object()) {
        const auto& system = (*afc)["system"];
        if (system.contains("extruders") && system["extruders"].is_object()) {
            for (const auto& [name, info] : system["extruders"].items()) {
                if (!info.is_object())
                    continue;
                int tool = safe_int(info, "tool_number");
                extruder_tool_map[name] = tool;
            }
        }
    }

    // Helper to resolve a Spoolman spool id to full filament data via Moonraker proxy.
    struct SpoolData
    {
        std::string spool_name;
        std::string filament_name;
        std::string vendor_name;
        std::string color_hex;
        std::string material;
        int         nozzle_temp = 0;
        int         bed_temp    = 0;
    };
    auto fetch_spool_data = [&](const std::string& spool_id) -> SpoolData {
        SpoolData data;
        if (spool_id.empty())
            return data;
        // Moonraker exposes Spoolman via its proxy endpoint, matching the pattern
        // used by fetch_moonraker_filament_data in the lane_data path.
        std::string spool_url =
            join_url(device_info.base_url, "/server/spoolman/proxy?path=/api/v1/spool/") + spool_id;
        std::string body;
        bool        ok = false;
        auto        req = Http::get(spool_url);
        if (!device_info.api_key.empty()) {
            req.header("X-Api-Key", device_info.api_key);
        }
        req.timeout_connect(5)
            .timeout_max(8)
            .on_complete([&](std::string b, unsigned st) {
                if (st == 200) {
                    body = std::move(b);
                    ok   = true;
                }
            })
            .perform_sync();
        if (!ok)
            return data;
        auto parsed = nlohmann::json::parse(body, nullptr, false, true);
        if (parsed.is_discarded())
            return data;
        // Moonraker proxy wraps the response; Spoolman direct returns the object.
        const nlohmann::json* spool = nullptr;
        if (parsed.contains("result") && parsed["result"].is_object())
            spool = &parsed["result"];
        else if (parsed.is_object())
            spool = &parsed;
        if (!spool)
            return data;
        if (spool->contains("name") && (*spool)["name"].is_string())
            data.spool_name = (*spool)["name"].get<std::string>();
        if (spool->contains("filament") && (*spool)["filament"].is_object()) {
            const auto& fil = (*spool)["filament"];
            if (fil.contains("name") && fil["name"].is_string())
                data.filament_name = fil["name"].get<std::string>();
            if (fil.contains("material") && fil["material"].is_string())
                data.material = fil["material"].get<std::string>();
            if (fil.contains("color_hex") && fil["color_hex"].is_string())
                data.color_hex = fil["color_hex"].get<std::string>();
            if (fil.contains("settings_extruder_temp") && fil["settings_extruder_temp"].is_number())
                data.nozzle_temp = fil["settings_extruder_temp"].get<int>();
            if (fil.contains("settings_bed_temp") && fil["settings_bed_temp"].is_number())
                data.bed_temp = fil["settings_bed_temp"].get<int>();
            if (fil.contains("vendor") && fil["vendor"].is_object() && fil["vendor"].contains("name") &&
                fil["vendor"]["name"].is_string())
                data.vendor_name = fil["vendor"]["name"].get<std::string>();
        }
        return data;
    };

    // Iterate AFC units. Each key in the AFC object is either a unit name
    // (Ace_1, AMS_1, Turtle_1, ...) or the shared "system" / "Tools" helpers.
    // Units contain a "system" metadata entry plus one child object per lane.
    int global_lane_counter = 0;
    for (const auto& [unit_name, unit_obj] : afc->items()) {
        if (!unit_obj.is_object())
            continue;
        if (unit_name == "system" || unit_name == "Tools")
            continue;

        // Track per-unit lane count so Tools-only units with no actual lanes
        // (e.g. "Toolchanger" placeholder) are skipped.
        bool has_any_lane = false;

        for (const auto& [lane_key, lane_obj] : unit_obj.items()) {
            if (!lane_obj.is_object())
                continue;
            if (lane_key == "system")
                continue;

            has_any_lane = true;

            AmsTrayData tray;
            tray.unit_name = unit_name;

            // Resolve extruder tool number via the lane.extruder string.
            std::string extruder_name = safe_string(lane_obj, "extruder");
            auto        it_ex         = extruder_tool_map.find(extruder_name);
            tray.extruder_tool_number = (it_ex != extruder_tool_map.end()) ? it_ex->second : 0;

            // Use the "map" field (T0..T15) as a stable global lane index when
            // available; otherwise fall back to numeric suffix of the lane key
            // (e.g. "lane12" -> 12); last resort, an incrementing counter.
            int         global_lane = -1;
            std::string map_str     = safe_string(lane_obj, "map");
            if (!map_str.empty() && (map_str[0] == 'T' || map_str[0] == 't')) {
                try {
                    global_lane = std::stoi(map_str.substr(1));
                } catch (...) {
                    global_lane = -1;
                }
            }
            if (global_lane < 0) {
                size_t      digit_start = lane_key.find_first_of("0123456789");
                if (digit_start != std::string::npos) {
                    try {
                        global_lane = std::stoi(lane_key.substr(digit_start));
                    } catch (...) {
                        global_lane = -1;
                    }
                }
            }
            if (global_lane < 0) {
                global_lane = global_lane_counter;
            }
            tray.slot_index = global_lane;
            ++global_lane_counter;
            max_lane_index  = std::max(max_lane_index, global_lane);

            tray.tray_type  = safe_string(lane_obj, "material");
            tray.tray_color = safe_string(lane_obj, "color");

            // Spool id: AFC exposes numeric "spool_id" directly.
            tray.spoolman_id = safe_string(lane_obj, "spool_id");
            if (tray.spoolman_id.empty()) {
                tray.spoolman_id = safe_string(lane_obj, "spoolman_id");
            }

            int lane_nozzle_temp = safe_int(lane_obj, "extruder_temp");
            int lane_bed_temp    = safe_int(lane_obj, "bed_temp");
            if (lane_nozzle_temp > 0) tray.nozzle_temp = lane_nozzle_temp;
            if (lane_bed_temp > 0) tray.bed_temp = lane_bed_temp;

            // Enrich with Spoolman data where AFC alone is insufficient.
            if (!tray.spoolman_id.empty()) {
                SpoolData data = fetch_spool_data(tray.spoolman_id);
                if (tray.tray_type.empty() && !data.material.empty())
                    tray.tray_type = data.material;
                if (tray.tray_color.empty() && !data.color_hex.empty())
                    tray.tray_color = data.color_hex;
                if (tray.nozzle_temp == 0 && data.nozzle_temp > 0)
                    tray.nozzle_temp = data.nozzle_temp;
                if (tray.bed_temp == 0 && data.bed_temp > 0)
                    tray.bed_temp = data.bed_temp;
                if (!data.vendor_name.empty())
                    tray.vendor_name = data.vendor_name;

                std::string display_name;
                if (!data.spool_name.empty())
                    display_name = data.spool_name;
                else if (!data.filament_name.empty())
                    display_name = data.filament_name;
                if (!tray.spoolman_id.empty() && !display_name.empty() &&
                    display_name.find(tray.spoolman_id) == std::string::npos) {
                    display_name += " #" + tray.spoolman_id;
                }
                tray.filament_name = display_name;
            }

            tray.has_filament  = !tray.tray_type.empty() || !tray.tray_color.empty() || !tray.spoolman_id.empty();
            auto* bundle       = GUI::wxGetApp().preset_bundle;
            tray.tray_info_idx = bundle ? bundle->filaments.filament_id_by_type(tray.tray_type) :
                                          map_filament_type_to_generic_id(tray.tray_type);

            trays.push_back(std::move(tray));
        }

        if (!has_any_lane) {
            continue;
        }
    }

    if (trays.empty()) {
        BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent::fetch_afc_status: no AFC lanes found";
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "MoonrakerPrinterAgent::fetch_afc_status: parsed " << trays.size()
                            << " lanes across " << extruder_tool_map.size() << " extruders";
    return true;
}

} // namespace Slic3r
