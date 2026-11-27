#include "QidiPrinterAgent.hpp"
#include "Http.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "IPrinterAgent.hpp"
#include "NetworkAgentFactory.hpp"
#include "bambu_networking.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/log/trivial.hpp>
#include <cctype>
#include <algorithm>
#include "libslic3r/Preset.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

// Check whether any visible, compatible base preset in the collection has the given filament_id.
bool has_visible_base_preset(const PresetCollection& filaments, const std::string& filament_id)
{
    for (const auto& p : filaments.get_presets()) {
        if (p.is_visible && p.is_compatible
            && filaments.get_preset_base(p) == &p
            && p.filament_id == filament_id)
            return true;
    }
    return false;
}

// ams_mapping_info maps every print filament to its target: "ams" 255 is the external
// spool holder (VIRTUAL_TRAY_MAIN_ID); older callers send an empty string.
bool ams_mapping_uses_external(const std::string& ams_mapping_info)
{
    if (ams_mapping_info.empty())
        return false;
    const auto info = nlohmann::json::parse(ams_mapping_info, nullptr, false);
    if (!info.is_array())
        return false;
    for (const auto& filament : info) {
        if (filament.is_object() && filament.contains("ams") && filament["ams"].is_number_integer() &&
            filament["ams"].get<int>() == VIRTUAL_TRAY_MAIN_ID)
            return true;
    }
    return false;
}

// Qidi firmware color values may be 3/6/8 hex digits (with an optional '#'), while
// DevAmsTray::decode_color only accepts exactly RRGGBBAA (anything else renders white).
std::string normalize_hex_color(const std::string& raw)
{
    std::string c;
    c.reserve(8);
    for (char ch : raw) {
        if (ch == '#') continue;
        if (std::isspace(static_cast<unsigned char>(ch))) continue;
        c.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    auto is_hex = [](const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
    };
    if (!is_hex(c))
        return "ffffffff";
    if (c.size() == 3) {
        std::string expanded;
        expanded.reserve(6);
        for (char ch : c) { expanded.push_back(ch); expanded.push_back(ch); }
        c = expanded;
    }
    if (c.size() == 6)
        c += "ff";
    if (c.size() != 8)
        return "ffffffff";
    return c;
}

} // anonymous namespace

const std::string QidiPrinterAgent_VERSION = "0.0.1";

QidiPrinterAgent::QidiPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir))
{
}

QidiPrinterAgent::~QidiPrinterAgent()
{
    // Reap the webcam probe so the thread never outlives the agent; by the time running
    // is false the thread has already released m_camera_mutex, so this cannot deadlock.
    if (m_camera_probe.joinable())
        m_camera_probe.join();
}

AgentInfo QidiPrinterAgent::get_agent_info_static()
{
    return AgentInfo{QIDI_PRINTER_AGENT_ID, "Qidi", QidiPrinterAgent_VERSION, "Qidi printer agent"};
}

// ============================================================================
// Qidi gcode-dialect commands
// ============================================================================

int QidiPrinterAgent::command_ams_refresh_rfid(std::string dev_id, int ams_id, int slot_id, int sequence_id, bool lan_mode)
{
    // READ_RFID is a Box macro; slot numbering follows QIDI Studio's RefreshRfid task:
    // 0..15 Box slots, 16 external holder (Orca addresses that as a virtual ams id).
    int slot = 16;
    if (ams_id >= 0 && ams_id < 16)
        slot = ams_id * 4 + slot_id;
    nlohmann::json j;
    j["print"]["command"]     = "gcode_line";
    j["print"]["param"]       = "READ_RFID SLOT=" + std::to_string(slot);
    j["print"]["sequence_id"] = std::to_string(sequence_id);
    return lan_mode ? send_message_to_printer(dev_id, j.dump(), 0, 0)
                     : send_message(dev_id, j.dump(), 0, 0);
}

int QidiPrinterAgent::command_ams_select_tray(std::string /*dev_id*/, std::string /*tray_id*/, int /*sequence_id*/, bool /*lan_mode*/)
{
    // Qidi Box macros take the slot explicitly (LOAD/UPDATE ... SLOT=); there is no
    // standalone tray-select command, so acknowledge instead of raising a dialog.
    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::command_start_camera(std::string /*dev_id*/)
{
    // The Qidi camera is an mjpg-streamer HTTP endpoint resolved by resolve_camera();
    // there is no MQTT camera-start command to send.
    return BAMBU_NETWORK_SUCCESS;
}

bool QidiPrinterAgent::probe_snapshot(const std::string& url, const std::string& api_key)
{
    bool   ok   = false;
    auto   http = Http::get(url);
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.timeout_connect(2)
        .timeout_max(4)
        .on_complete([&](std::string /*body*/, unsigned status) { ok = status == 200; })
        .on_error([](std::string /*body*/, std::string /*err*/, unsigned /*status*/) {})
        .perform_sync();
    return ok;
}

void QidiPrinterAgent::resolve_camera() const
{
    std::lock_guard<std::mutex> lock(m_camera_mutex);
    if (m_camera_probe_running)
        return;
    const std::string base = device_info.base_url;
    // Arm the probe once per connection: it may not have started yet, or the base changed
    // after the previous connection (a committed "no camera" result is cached per base).
    if (base.empty() || m_camera_probe_base == base)
        return;
    if (m_camera_probe.joinable())
        m_camera_probe.join();
    m_camera_probe_base    = base;
    m_camera_url.clear();
    m_camera_mode          = CameraStreamMode::none;
    m_camera_probe_running = true;
    m_camera_probe = std::thread([this, base, api_key = device_info.api_key] { probe_camera(base, api_key); });
}

void QidiPrinterAgent::probe_camera(std::string base_url, std::string api_key) const
{
    // Candidate origins: the connection origin as configured, plus (when it carries an
    // explicit port) the bare default-port origin QIDI Studio uses for the webcam:
    // http://<ip>/webcam/?action=snapshot. Falls back to plain http for TLS connections.
    std::vector<std::string> origins;
    {
        std::string scheme;
        if (boost::algorithm::starts_with(base_url, "https://"))
            scheme = "https://";
        else if (boost::algorithm::starts_with(base_url, "http://"))
            scheme = "http://";

        std::string port;
        const std::string host = Http::get_host_from_url(base_url, &port);
        if (!host.empty() && !scheme.empty()) {
            origins.push_back(scheme + host + (port.empty() ? "" : (":" + port)));
            if (!port.empty() && port != "80" && port != "443")
                origins.push_back(scheme + host);
            if (scheme == "https://")
                origins.push_back("http://" + host);
        }
    }

    std::string snapshot_url; // verified snapshot endpoint, kept for HTTP snapshot polling
    for (const std::string& origin : origins) {
        if (snapshot_url.empty()) {
            // 1. Moonraker's webcam registry answers with the endpoints from moonraker.conf.
            auto        http = Http::get(origin + "/server/webcams/list");
            if (!api_key.empty())
                http.header("X-Api-Key", api_key);
            std::string body;
            unsigned    status = 0;
            http.timeout_connect(2)
                .timeout_max(3)
                .on_complete([&](std::string b, unsigned s) { body = std::move(b); status = s; })
                .on_error([](std::string, std::string, unsigned) {})
                .perform_sync();
            if (status == 200) {
                auto json = nlohmann::json::parse(body, nullptr, false, true);
                if (!json.is_discarded()) {
                    const nlohmann::json* list = &json;
                    if (json.contains("result") && json["result"].is_object() && json["result"].contains("webcams"))
                        list = &json["result"];
                    if (list->contains("webcams") && (*list)["webcams"].is_array()) {
                        for (const auto& cam : (*list)["webcams"]) {
                            std::string snap = cam.value("snapshot_url", std::string());
                            if (snap.empty())
                                continue;
                            if (!boost::algorithm::starts_with(snap, "http"))
                                snap = origin + (boost::algorithm::starts_with(snap, "/") ? "" : "/") + snap;
                            if (probe_snapshot(snap, api_key)) {
                                snapshot_url = snap;
                                break;
                            }
                        }
                    }
                }
            }
        }
        // 2. The path QIDI Studio itself uses: mjpg-streamer behind the web root.
        if (snapshot_url.empty()) {
            const std::string snap = origin + "/webcam/?action=snapshot";
            if (probe_snapshot(snap, api_key))
                snapshot_url = snap;
        }
        if (snapshot_url.empty())
            continue;
        break;
    }

    std::lock_guard<std::mutex> lock(m_camera_mutex);
    if (m_camera_probe_base != base_url) {
        // A newer connection superseded this probe; re-arm so its own probe can start.
        m_camera_probe_running = false;
        return;
    }
    m_camera_url = snapshot_url;
    // Snapshot polling: the verified endpoint, refreshed ~5x/s by WebMediaController.
    // The MJPEG sibling (action=stream) is deliberately not used unverified, so a dead
    // stream can never leave the panel black for good.
    m_camera_mode          = snapshot_url.empty() ? CameraStreamMode::none : CameraStreamMode::http_snapshot;
    m_camera_probe_running = false;
}

bool QidiPrinterAgent::fetch_filament_info(std::string dev_id, FilamentSyncMode /*sync_mode*/)
{
    std::string error;

    // 1. Fetch device info and infer series_id
    std::string series_id;
    {
        MoonrakerDeviceInfo info;
        if (fetch_device_info(device_info.base_url, device_info.api_key, info, error, {})) {
            series_id = infer_series_id(info.model_id, info.dev_name);
        }
    }
    if (series_id.empty()) {
        // Fall back to the configured Orca model if Moonraker doesn't expose a usable identifier.
        series_id = infer_series_id(device_info.model_id, device_info.model_name);
    }

    // 2. Fetch filament dictionary
    QidiFilamentDict dict;
    if (!fetch_filament_dict(device_info.base_url, device_info.api_key, dict, error)) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent::fetch_filament_info: Failed to fetch filament dict: " << error;
    }

    // 3. Fetch slot info and build AmsTrayData directly
    std::vector<AmsTrayData> trays;
    int                      box_count = 0;
    bool                     slots_ok  = false;
    try {
        slots_ok = fetch_slot_info(device_info.base_url, device_info.api_key, dict, series_id, trays, box_count, error);
    } catch (const nlohmann::json::exception& e) {
        // A slot value of an unexpected type fails the sync instead of escaping to the UI
        error = e.what();
    }
    if (!slots_ok) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent::fetch_filament_info: Failed to fetch slot info: " << error;
        return false;
    }

    // 4. Build the AMS payload
    build_ams_payload(box_count, box_count * 4 - 1, trays);

    // 5. Publish the external spool holder (fetched alongside Box slots as slot_index 255)
    const AmsTrayData* external = nullptr;
    for (const auto& tray : trays) {
        if (tray.slot_index == VIRTUAL_TRAY_MAIN_ID && tray.has_filament) {
            external = &tray;
            break;
        }
    }
    publish_external_spool(external);
    return true;
}

std::string QidiPrinterAgent::build_tool_slot_script(const std::string& ams_mapping, const std::string& ams_mapping_info)
{
    // The firmware has value_t0 to value_t15 and slots 0 to 15; the external spool holder
    // keeps its raw id 255 (VIRTUAL_TRAY_MAIN_ID), which Qidi's slotN variable also accepts.
    constexpr int slot_count    = 16;
    constexpr int external_slot = VIRTUAL_TRAY_MAIN_ID;

    // One entry per filament: its Box slot index, negative when no slot was chosen
    const auto mapping = nlohmann::json::parse(ams_mapping, nullptr, false);
    // ams_mapping_info carries the raw target per filament (255 = external spool holder),
    // which the v0 ams_mapping array has already flattened to -1. Older callers omit it.
    const auto info = ams_mapping_info.empty() ? nlohmann::json() : nlohmann::json::parse(ams_mapping_info, nullptr, false);
    std::string script;
    if (!mapping.is_array()) {
        return script;
    }
    for (int filament = 0; filament < int(mapping.size()) && filament < slot_count; ++filament) {
        int slot = -1;
        if (info.is_array() && filament < int(info.size()) && info[filament].is_object() && info[filament].contains("ams") &&
            info[filament]["ams"].is_number_integer()) {
            slot = info[filament]["ams"].get<int>();
        } else if (mapping[filament].is_number_integer()) {
            slot = mapping[filament].get<int>();
        }
        if (slot < 0 || (slot >= slot_count && slot != external_slot)) {
            continue;
        }
        if (!script.empty()) {
            script += '\n';
        }
        // SAVE_VARIABLE reads VALUE as a Python literal, so the slot name keeps its own quotes
        script += "SAVE_VARIABLE VARIABLE=value_t" + std::to_string(filament) + " VALUE=\"'slot" + std::to_string(slot) + "'\"";
    }
    return script;
}

bool QidiPrinterAgent::print_uploaded_file(const PrintParams& params, const std::string& filename)
{
    // Mirror Qidi Studio: the Box only loads value_t* slots while enable_box is set, so the
    // box state is written before the slot script. External spool (or the print dialog's
    // Enable Box toggle switched off) sends enable_box 0 and firmware skips the Box job.
    // "Standard" sync profile (task_qidi_enhanced == false): no Qidi SAVE_VARIABLE lines at
    // all - neither enable_box nor the value_t*/slotN script, plain agent behavior.
    std::string script;
    if (params.task_qidi_enhanced) {
        const bool        external  = ams_mapping_uses_external(params.ams_mapping_info);
        const bool        enable_box = params.task_enable_box && !external;
        script = std::string("SAVE_VARIABLE VARIABLE=enable_box VALUE=") + (enable_box ? "1" : "0");
        const std::string slot_script = build_tool_slot_script(params.ams_mapping, params.ams_mapping_info);
        if (!slot_script.empty()) {
            script += '\n';
            script += slot_script;
        }
    }
    if (!script.empty() && !send_gcode_sync(device_info.dev_id, script)) {
        return false;
    }

    // On the X-Max 4 and Q2, Klipper only opens the copy Moonraker's print start makes, so SDCARD_PRINT_FILE can't find the upload
    std::string response;
    return send_jsonrpc_command(device_info.base_url, device_info.api_key, print_start_request(params, filename), response);
}

nlohmann::json QidiPrinterAgent::print_start_request(const PrintParams& params, const std::string& filename)
{
    nlohmann::json request{{"filename", filename}};
    // Qidi's Moonraker prints Metadata/plate_<plateindex>.gcode from a 3MF, defaulting to plate 1
    if (boost::iends_with(filename, ".3mf"))
        request["plateindex"] = std::to_string(params.plate_index);
    return request;
}

bool QidiPrinterAgent::fetch_slot_info(const std::string&        base_url,
                                       const std::string&        api_key,
                                       const QidiFilamentDict&   dict,
                                       const std::string&        series_id,
                                       std::vector<AmsTrayData>& trays,
                                       int&                      box_count,
                                       std::string&              error)
{
    // "box" = Qidi Box slots, "extra" = external spool holder (Virtual Tray, published as slot 255).
    // Querying both at once; older firmware that rejects "extra" gets a Box-only retry below
    // so the Box slots keep working unchanged.
    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto query_slot_state = [&](const std::string& query) -> bool {
        std::string url = join_url(base_url, "/printer/objects/query?save_variables=variables&" + query);
        for (int i = 0; i < 16; ++i) {
            url += "&box_stepper%20slot" + std::to_string(i) + "=runout_button";
        }

        response_body.clear();
        success     = false;
        http_error.clear();

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
        return success;
    };

    bool queried = query_slot_state("multi_color_controller");
    if (!queried && http_error.rfind("HTTP error:", 0) == 0) {
        // Older firmware rejects the unknown "extra" field; Box-only keeps the sync working
        queried = query_slot_state("multi_color_controller=box");
    }
    if (!queried) {
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
    trays.clear();
    trays.reserve(max_slots);

    // Lambda to build setting_id from slot data
    auto build_setting_id = [&](int filament_type_idx, int vendor_type, const std::string& tray_type) {
        const int vendor = (vendor_type == 1) ? 1 : 0;
        if (is_numeric(series_id) && filament_type_idx > 0) {
            return "QD_" + series_id + "_" + std::to_string(vendor) + "_" + std::to_string(filament_type_idx);
        }
        return map_filament_type_to_setting_id(tray_type);
    };

    for (int i = 0; i < max_slots; ++i) {
        AmsTrayData tray;
        tray.slot_index = i;

        // Read slot variables. Newer firmware stores a hex color list and a material id instead of dictionary
        // indexes, and only in the slots it has rewritten, so each slot is checked for both forms.
        const auto color_value    = variables.value("color_slot" + std::to_string(i), nlohmann::json(1));
        const auto filament_value = variables.value("filament_slot" + std::to_string(i), nlohmann::json(1));
        const int color_index     = color_value.is_number() ? color_value.get<int>() : 0;
        const int filament_type   = filament_value.is_number() ? filament_value.get<int>() : 0;
        const int vendor_type     = variables.value("vendor_slot" + std::to_string(i), 0);

        // Check filament presence via runout sensor
        std::string box_stepper_key = "box_stepper slot" + std::to_string(i);
        tray.has_filament = false;
        if (status.contains(box_stepper_key)) {
            auto& box_stepper = status[box_stepper_key];
            if (box_stepper.contains("runout_button") && !box_stepper["runout_button"].is_null()) {
                int runout_button = box_stepper["runout_button"].template get<int>();
                tray.has_filament = (runout_button == 0);
            }
        }

        if (tray.has_filament) {
            // Look up filament type name from dictionary
            std::string filament_name = "PLA";
            auto filament_it = dict.filaments.find(filament_type);
            if (filament_it != dict.filaments.end()) {
                filament_name = filament_it->second;
            }
            if (filament_value.is_string()) {
                const nlohmann::json::json_pointer decoded_type("/multi_color_controller/box/box" + std::to_string(i / 4) +
                                                                "_filament_info_" + std::to_string(i % 4) + "_filament_type");
                if (status.contains(decoded_type) && status[decoded_type].is_string()) {
                    filament_name = status[decoded_type].get<std::string>();
                }
            }
            tray.tray_type = normalize_filament_type(filament_name);

            // Try Qidi-specific setting ID first; fall back to visible preset by type
            std::string setting_id = build_setting_id(filament_type, vendor_type, tray.tray_type);
            auto* bundle = GUI::wxGetApp().preset_bundle;
            if (!bundle) {
                tray.tray_info_idx = setting_id;
            } else if (!setting_id.empty() && has_visible_base_preset(bundle->filaments, setting_id)) {
                tray.tray_info_idx = setting_id;
            } else {
                tray.tray_info_idx = bundle->filaments.filament_id_by_type(tray.tray_type);
                if (tray.tray_info_idx.empty())
                    tray.tray_info_idx = map_filament_type_to_generic_id(tray.tray_type);
            }

            // Look up color from dictionary
            auto color_it = dict.colors.find(color_index);
            if (color_value.is_array() && !color_value.empty() && color_value[0].is_string()) {
                tray.tray_color = normalize_hex_color(color_value[0].get<std::string>());
            } else if (color_it != dict.colors.end()) {
                tray.tray_color = normalize_hex_color(color_it->second);
            } else {
                tray.tray_color = "ffffffff";
            }
        }

        trays.push_back(tray);
    }

    // The external spool holder (Virtual Tray). Qidi Studio reads multi_color_controller.extra
    // as its slot 16; Orca's mapping dialog shows it from MachineObject::vt_slot as Ext-R,
    // so it is fetched as slot_index 255 (VIRTUAL_TRAY_MAIN_ID) and published after the payload.
    if (status.contains("multi_color_controller") && status["multi_color_controller"].is_object()) {
        const auto& mcc = status["multi_color_controller"];
        if (mcc.contains("extra") && mcc["extra"].is_object()) {
            const auto& ext = mcc["extra"];
            if (ext.contains("filament_info") && ext["filament_info"].is_object()) {
                const auto& fi = ext["filament_info"];
                const bool worthwhile = fi.contains("filament_str") || fi.contains("filament_type") || fi.contains("color_rgb_str");

                AmsTrayData tray;
                tray.slot_index   = VIRTUAL_TRAY_MAIN_ID;
                tray.has_filament = worthwhile; // no runout sensor on the holder; presence comes from filament_info
                if (worthwhile) {
                    std::string raw_type = fi.value("filament_type", std::string("PLA"));
                    if (raw_type.empty())
                        raw_type = "PLA";
                    tray.tray_type = normalize_filament_type(raw_type);

                    // color_rgb_str is a one-element color list in Qidi firmware (same as Box slots)
                    std::string color;
                    if (fi.contains("color_rgb_str")) {
                        const auto& c = fi["color_rgb_str"];
                        if (c.is_array() && !c.empty() && c[0].is_string())
                            color = c[0].get<std::string>();
                        else if (c.is_string())
                            color = c.get<std::string>();
                    }
                    if (!color.empty())
                        tray.tray_color = normalize_hex_color(color);

                    const std::string setting_id = map_filament_type_to_setting_id(tray.tray_type);
                    auto*             bundle     = GUI::wxGetApp().preset_bundle;
                    if (!bundle) {
                        tray.tray_info_idx = setting_id;
                    } else if (!setting_id.empty() && has_visible_base_preset(bundle->filaments, setting_id)) {
                        tray.tray_info_idx = setting_id;
                    } else {
                        tray.tray_info_idx = bundle->filaments.filament_id_by_type(tray.tray_type);
                        if (tray.tray_info_idx.empty())
                            tray.tray_info_idx = map_filament_type_to_generic_id(tray.tray_type);
                    }
                }
                trays.push_back(tray);
            }
        }
    }

    return true;
}

// ParseV1_0 only fills AMS trays, never the virtual ones, so the external holder is published
// here: the mapping dialog renders MachineObject::vt_slot as Ext-R/Ext-L tiles from this alone.
void QidiPrinterAgent::publish_external_spool(const AmsTrayData* tray) const
{
    auto* dev_manager = GUI::wxGetApp().getDeviceManager();
    if (!dev_manager)
        return;
    MachineObject* obj = dev_manager->get_my_machine(device_info.dev_id);
    if (!obj)
        return;

    obj->vt_slot.clear();
    if (!tray)
        return;

    DevAmsTray ext(std::to_string(VIRTUAL_TRAY_MAIN_ID));
    ext.is_exists   = tray->has_filament;
    ext.m_fila_type = tray->tray_type;
    ext.setting_id  = tray->tray_info_idx;
    ext.UpdateColorFromStr(normalize_hex_color(tray->tray_color));
    obj->vt_slot.push_back(ext);
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

    dict.colors.clear();
    dict.filaments.clear();
    parse_ini_section(response_body, "colordict", dict.colors);
    parse_filament_sections(response_body, dict.filaments);

    return !dict.colors.empty();
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

std::string QidiPrinterAgent::map_filament_type_to_setting_id(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

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

std::string QidiPrinterAgent::normalize_model_key(std::string value)
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

std::string QidiPrinterAgent::infer_series_id(const std::string& model_id, const std::string& dev_name)
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

std::string QidiPrinterAgent::normalize_filament_type(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

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

    return upper;
}

} // namespace Slic3r
