#include "ElegooPrinterAgent.hpp"
#include "WebSocketClient.hpp"
#include "FilamentSyncUtils.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <nlohmann/json.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace Slic3r {

static const std::string ElegooPrinterAgent_VERSION = "0.1.0";

ElegooPrinterAgent::ElegooPrinterAgent(std::string log_dir)
{
    (void) log_dir;
}

AgentInfo ElegooPrinterAgent::get_agent_info_static()
{
    return AgentInfo{"elegoo", "Elegoo", ElegooPrinterAgent_VERSION, "Elegoo CANVAS filament detection agent"};
}

// ============================================================================
// Connection
// ============================================================================

int ElegooPrinterAgent::connect_printer(std::string dev_id, std::string dev_ip,
                                         std::string username, std::string password, bool use_ssl)
{
    if (dev_id.empty() || dev_ip.empty()) {
        BOOST_LOG_TRIVIAL(error) << "ElegooPrinterAgent::connect_printer: missing dev_id or dev_ip";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    std::string host = extract_host(dev_ip);
    if (host.empty()) {
        BOOST_LOG_TRIVIAL(error) << "ElegooPrinterAgent::connect_printer: could not extract host from '" << dev_ip << "'";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    // Store connection info
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_dev_id = dev_id;
        m_dev_ip = dev_ip;

        auto* preset_bundle = GUI::wxGetApp().preset_bundle;
        if (preset_bundle) {
            auto& preset = preset_bundle->printers.get_edited_preset();
            m_model_id = preset.get_printer_type(preset_bundle);
        }
    }

    // Notify UI — actual connectivity is verified on first fetch_filament_info call
    dispatch_local_connect(ConnectStatusOk, dev_id, "0");
    dispatch_printer_connected(dev_id);

    BOOST_LOG_TRIVIAL(info) << "ElegooPrinterAgent::connect_printer: registered " << host;
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::disconnect_printer()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_dev_id.clear();
    m_dev_ip.clear();
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Filament Sync
// ============================================================================

bool ElegooPrinterAgent::fetch_filament_info(std::string dev_id)
{
    std::string host;
    std::string model_id;
    std::string device_id;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        host = extract_host(m_dev_ip);
        model_id = m_model_id;
        device_id = m_dev_id;
    }

    if (host.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "ElegooPrinterAgent::fetch_filament_info: No printer IP";
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "ElegooPrinterAgent::fetch_filament_info: querying " << host;

    std::vector<AmsTrayData> trays;
    int                      canvas_count = 0;
    std::string              error;

    if (!fetch_canvas_tray_info(host, trays, canvas_count, error)) {
        BOOST_LOG_TRIVIAL(warning) << "ElegooPrinterAgent::fetch_filament_info: " << error;
        return false;
    }

    if (trays.empty()) {
        BOOST_LOG_TRIVIAL(info) << "ElegooPrinterAgent::fetch_filament_info: No trays";
        return false;
    }

    int max_lane = trays.back().slot_index;
    FilamentSyncUtils::build_ams_payload(device_id, model_id, canvas_count, max_lane, trays);

    BOOST_LOG_TRIVIAL(info) << "ElegooPrinterAgent::fetch_filament_info: Synced " << trays.size()
                            << " tray(s) from " << canvas_count << " CANVAS unit(s)";
    return true;
}

bool ElegooPrinterAgent::fetch_canvas_tray_info(const std::string&        host,
                                                 std::vector<AmsTrayData>& trays,
                                                 int&                      canvas_count,
                                                 std::string&              error)
{
    trays.clear();
    canvas_count = 0;

    // 1. Connect — use WebSocketClient (same as ElegooLink uses)
    WebSocketClient ws;
    try {
        ws.connect(host, ELEGOO_WS_PORT, ELEGOO_WS_PATH);
    } catch (const std::exception& e) {
        error = std::string("WebSocket connect failed: ") + e.what();
        return false;
    }

    // 2. Build and send Cmd 324 request
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
    std::string request_id = boost::uuids::to_string(boost::uuids::random_generator()());

    nlohmann::json request = {
        {"Id", ""},
        {"Data", {
            {"Cmd", CANVAS_CMD},
            {"Data", nlohmann::json::object()},
            {"RequestID", request_id},
            {"MainboardID", ""},
            {"TimeStamp", now_ms},
            {"From", 1}
        }}
    };

    try {
        ws.send(request.dump());
    } catch (const std::exception& e) {
        error = std::string("WebSocket send failed: ") + e.what();
        return false;
    }

    // 3. Read responses — expect ACK then material data
    nlohmann::json material_data;
    bool           found_material = false;

    for (int attempt = 0; attempt < 5 && !found_material; ++attempt) {
        std::string msg;
        try {
            msg = ws.receive(5);
        } catch (const std::exception& e) {
            error = std::string("WebSocket receive failed: ") + e.what();
            return false;
        }

        BOOST_LOG_TRIVIAL(trace) << "ElegooPrinterAgent: received msg #" << attempt << " len=" << msg.size();

        auto json = nlohmann::json::parse(msg, nullptr, false, true);
        if (json.is_discarded()) continue;

        if (json.contains("canvas_list")) {
            material_data  = json;
            found_material = true;
        } else if (json.contains("Data") && json["Data"].is_object()) {
            auto& data1 = json["Data"];
            if (data1.contains("canvas_list")) {
                material_data  = data1;
                found_material = true;
            } else if (data1.contains("Data") && data1["Data"].is_object() && data1["Data"].contains("canvas_list")) {
                material_data  = data1["Data"];
                found_material = true;
            }
        }
    }

    if (!found_material) {
        error = "No canvas_list in response after 5 read attempts";
        return false;
    }

    // 4. Parse canvas_list
    auto& canvas_list = material_data["canvas_list"];
    if (!canvas_list.is_array() || canvas_list.empty()) {
        error = "canvas_list is empty or not an array";
        return false;
    }

    canvas_count    = static_cast<int>(canvas_list.size());
    int slot_offset = 0;

    for (const auto& canvas : canvas_list) {
        if (!canvas.contains("tray_list") || !canvas["tray_list"].is_array()) continue;

        int connected = canvas.value("connected", 0);
        if (connected == 0) {
            slot_offset += static_cast<int>(canvas["tray_list"].size());
            continue;
        }

        for (const auto& tray : canvas["tray_list"]) {
            AmsTrayData t;
            t.slot_index = slot_offset + tray.value("tray_id", 0);

            std::string filament_type = tray.value("filament_type", "");
            int         status        = tray.value("status", -1);

            t.has_filament = (status != 0 && !filament_type.empty());

            if (t.has_filament) {
                t.tray_type    = normalize_filament_type(filament_type);
                t.tray_color   = tray.value("filament_color", "");
                t.nozzle_temp  = tray.value("max_nozzle_temp", 0);

                auto* bundle = GUI::wxGetApp().preset_bundle;
                if (bundle) {
                    t.tray_info_idx = bundle->filaments.filament_id_by_type(t.tray_type);
                } else {
                    t.tray_info_idx = FilamentSyncUtils::map_filament_type_to_generic_id(t.tray_type);
                }
            }

            trays.emplace_back(std::move(t));
        }

        slot_offset += static_cast<int>(canvas["tray_list"].size());
    }

    return true;
}

// ============================================================================
// Helpers
// ============================================================================

std::string ElegooPrinterAgent::extract_host(const std::string& dev_ip) const
{
    std::string host = dev_ip;
    if (host.find("http://") == 0)  host = host.substr(7);
    if (host.find("https://") == 0) host = host.substr(8);
    auto slash = host.find('/');
    if (slash != std::string::npos) host = host.substr(0, slash);
    auto colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    return host;
}

std::string ElegooPrinterAgent::normalize_filament_type(const std::string& filament_type)
{
    std::string upper = FilamentSyncUtils::trim_and_upper(filament_type);

    if (upper.find("PLA") != std::string::npos) return "PLA";
    if (upper.find("PETG") != std::string::npos) return "PETG";
    if (upper.find("PET") != std::string::npos) return "PETG";
    if (upper.find("ABS") != std::string::npos) return "ABS";
    if (upper.find("ASA") != std::string::npos) return "ASA";
    if (upper.find("TPU") != std::string::npos) return "TPU";
    if (upper.find("PA") != std::string::npos || upper.find("NYLON") != std::string::npos) return "PA";
    if (upper.find("PC") != std::string::npos) return "PC";
    if (upper.find("PVA") != std::string::npos) return "PVA";
    if (upper.find("HIPS") != std::string::npos) return "HIPS";

    return upper;
}

void ElegooPrinterAgent::dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg)
{
    OnLocalConnectedFn fn;
    QueueOnMainFn      queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        fn       = m_on_local_connect;
        queue_fn = m_queue_on_main;
    }
    if (!fn) return;

    auto dispatch = [state, dev_id, msg, fn]() { fn(state, dev_id, msg); };
    if (queue_fn) {
        queue_fn(dispatch);
    } else {
        dispatch();
    }
}

void ElegooPrinterAgent::dispatch_printer_connected(const std::string& dev_id)
{
    OnPrinterConnectedFn fn;
    QueueOnMainFn        queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        fn       = m_on_printer_connected;
        queue_fn = m_queue_on_main;
    }
    if (!fn) return;

    auto dispatch = [dev_id, fn]() { fn(dev_id); };
    if (queue_fn) {
        queue_fn(dispatch);
    } else {
        dispatch();
    }
}

} // namespace Slic3r
