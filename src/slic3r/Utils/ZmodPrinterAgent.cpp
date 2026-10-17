#include "ZmodPrinterAgent.hpp"

#include "Flashforge.hpp"
#include "Http.hpp"
#include "PrintHost.hpp"
#include "libslic3r/GCode/ZmodHeader.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

constexpr const char* ZMOD_AGENT_VERSION = "0.0.1";

// Zmod's spool slots, numbered from 1; a slot feeds the tool of the same index.
constexpr int ZMOD_SLOTS = 4;

// Zmod's Timelapse plugin (https://github.com/ghzserg/timelapse) is moonraker-timelapse; its Klipper
// macros are loaded only while the plugin is enabled.
constexpr const char* TIMELAPSE_MACRO = "gcode_macro TIMELAPSE_TAKE_FRAME";

// Flashforge's config files carry /* */ comments, which JSON does not allow.
std::string strip_block_comments(std::string text)
{
    for (size_t start; (start = text.find("/*")) != std::string::npos;) {
        const size_t end = text.find("*/", start + 2);
        text.erase(start, end == std::string::npos ? std::string::npos : end + 2 - start);
    }
    return text;
}

} // anonymous namespace

ZmodPrinterAgent::ZmodPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

AgentInfo ZmodPrinterAgent::get_agent_info_static()
{
    return AgentInfo{ZMOD_PRINTER_AGENT_ID, "Zmod (Flashforge)", ZMOD_AGENT_VERSION, "Flashforge printers running Zmod"};
}

bool ZmodPrinterAgent::moonraker_get(const std::string& path, std::string& body) const
{
    std::string http_error;
    bool        success = false;
    auto        http    = Http::get(join_url(device_info.base_url, path));
    if (!device_info.api_key.empty())
        http.header("X-Api-Key", device_info.api_key);
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string response, unsigned status) {
            success = status == 200;
            body    = std::move(response);
            if (!success)
                http_error = "HTTP error: " + std::to_string(status);
        })
        .on_error([&](std::string /*body*/, std::string err, unsigned /*status*/) { http_error = std::move(err); })
        .perform_sync();
    if (!success)
        BOOST_LOG_TRIVIAL(warning) << "ZmodPrinterAgent: GET " << path << " failed: " << http_error;
    return success;
}

bool ZmodPrinterAgent::supports_timelapse() const { return has_klipper_object(TIMELAPSE_MACRO); }

void ZmodPrinterAgent::set_timelapse_plugin(bool enabled) const
{
    std::string http_error;
    bool        success = false;
    auto        http    = Http::post(join_url(device_info.base_url, "/machine/timelapse/settings"));
    if (!device_info.api_key.empty())
        http.header("X-Api-Key", device_info.api_key);
    http.header("Content-Type", "application/json")
        .set_post_body(nlohmann::json{{"enabled", enabled}}.dump())
        .timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string /*body*/, unsigned status) {
            success = status == 200;
            if (!success)
                http_error = "HTTP error: " + std::to_string(status);
        })
        .on_error([&](std::string /*body*/, std::string err, unsigned /*status*/) { http_error = std::move(err); })
        .perform_sync();
    // A timelapse is not worth failing the print over; the plugin keeps its previous setting.
    if (!success)
        BOOST_LOG_TRIVIAL(warning) << "ZmodPrinterAgent: setting timelapse enabled=" << enabled << " failed: " << http_error;
}

nlohmann::json ZmodPrinterAgent::zmod_color_status() const
{
    std::string body;
    if (!moonraker_get("/printer/objects/query?zmod_color", body))
        return {};
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.contains("result") || !json["result"].contains("status") ||
        !json["result"]["status"].contains("zmod_color"))
        return {};
    return json["result"]["status"]["zmod_color"];
}

bool ZmodPrinterAgent::fetch_filament_info(std::string /*dev_id*/, FilamentSyncMode /*sync_mode*/)
{
    const nlohmann::json status = zmod_color_status();
    if (!status.contains("slots") || !status["slots"].is_array()) {
        BOOST_LOG_TRIVIAL(warning) << "ZmodPrinterAgent::fetch_filament_info: no zmod_color slots";
        return false;
    }

    // Each slot: {"ID": "1", "Material": "PLA", "Color": "white", "HEX": "FFFFFF", "hasFilament": true}
    std::vector<AmsTrayData> trays;
    for (const auto& slot : status["slots"]) {
        if (!slot.is_object())
            continue;
        AmsTrayData tray;
        tray.slot_index = std::atoi(slot.value("ID", "0").c_str()) - 1;
        if (tray.slot_index < 0 || tray.slot_index >= ZMOD_SLOTS)
            continue;
        tray.has_filament = slot.value("hasFilament", false);
        if (tray.has_filament) {
            tray.tray_type  = trim_and_upper(slot.value("Material", ""));
            tray.tray_color = slot.value("HEX", "FFFFFF") + "FF";
            if (auto* bundle = GUI::wxGetApp().preset_bundle)
                tray.tray_info_idx = bundle->filaments.filament_id_by_type(tray.tray_type);
            else
                tray.tray_info_idx = map_filament_type_to_generic_id(tray.tray_type);
        }
        trays.emplace_back(std::move(tray));
    }
    if (trays.empty())
        return false;

    int max_slot = 0;
    for (const auto& tray : trays)
        max_slot = std::max(max_slot, tray.slot_index);
    build_ams_payload(1, max_slot, trays);
    return true;
}

std::map<int, int> ZmodPrinterAgent::tool_slots(const PrintParams& params) const
{
    // The slot index (0-based) for each tool number in the uploaded file. When the upload was
    // renumbered to physical tools that is the identity; otherwise it is the dialog's assignment of
    // filaments to slots.
    std::map<int, int> slots;
    if (const auto tool_map = upload_tool_map(params); !tool_map.empty()) {
        for (const auto& [filament, tool] : tool_map)
            slots[tool] = tool;
    } else {
        slots = parse_tool_mapping(params.ams_mapping, ZMOD_SLOTS);
    }
    return slots;
}

int ZmodPrinterAgent::upload_and_start(const PrintParams&      params,
                                       const std::string&      gcode_path,
                                       const std::string&      filename,
                                       const OnUpdateStatusFn& update_fn,
                                       const WasCancelledFn&   cancel_fn)
{
    // The plugin keeps its setting in Moonraker until changed, so every print sets it, whichever
    // way the print then starts.
    if (supports_timelapse())
        set_timelapse_plugin(params.task_record_timelapse);

    // With Flashforge's own screen, Flashforge's app runs the printer and takes the print the way
    // Flashforge's slicer sends it; Zmod's PRINT_ZCOLOR is for running without that screen.
    const nlohmann::json status = zmod_color_status();
    if (!status.value("display", false))
        return MoonrakerPrinterAgent::upload_and_start(params, gcode_path, filename, update_fn, cancel_fn);
    return upload_to_flashforge_app(params, gcode_path, filename, status, update_fn, cancel_fn);
}

std::string ZmodPrinterAgent::flashforge_app_host() const
{
    // Flashforge's app serves its API on the Wi-Fi interface only, so a printer reached through a
    // wired adapter is not reachable there on the address the agent uses; Zmod itself posts to
    // wlan0's address for the same reason.
    std::string body;
    if (moonraker_get("/machine/system_info", body)) {
        const auto json = nlohmann::json::parse(body, nullptr, false);
        const auto ptr  = nlohmann::json::json_pointer("/result/system_info/network/wlan0/ip_addresses");
        if (!json.is_discarded() && json.contains(ptr) && json[ptr].is_array())
            for (const auto& address : json[ptr])
                if (address.is_object() && address.value("family", "") == "ipv4" && !address.value("address", "").empty())
                    return address.value("address", "");
    }
    return device_info.base_url;
}

int ZmodPrinterAgent::upload_to_flashforge_app(const PrintParams&      params,
                                               const std::string&      gcode_path,
                                               const std::string&      filename,
                                               const nlohmann::json&   zmod_color,
                                               const OnUpdateStatusFn& update_fn,
                                               const WasCancelledFn&   cancel_fn)
{
    // The app's local API authenticates with the printer's serial number and LAN code, which
    // Flashforge keeps in config files that Moonraker serves.
    std::string general, network;
    if (!moonraker_get("/server/files/config/rw/general.json", general) || !moonraker_get("/server/files/config/rw/network.json", network))
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    const auto general_json = nlohmann::json::parse(strip_block_comments(general), nullptr, false);
    const auto network_json = nlohmann::json::parse(strip_block_comments(network), nullptr, false);
    const std::string serial_number = general_json.is_object() ? general_json.value("serialNumber", "") : "";
    const std::string check_code    = network_json.is_object() ? network_json.value("lanModeCode", "") : "";
    if (serial_number.empty() || check_code.empty()) {
        BOOST_LOG_TRIVIAL(error) << "ZmodPrinterAgent: no serial number or LAN code in the printer's config";
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    }

    // Each tool of the file names the slot it prints from, the way Flashforge's slicer maps them.
    nlohmann::json mappings = nlohmann::json::array();
    for (const auto& [tool, slot_index] : tool_slots(params)) {
        nlohmann::json slot_info;
        if (zmod_color.contains("slots") && zmod_color["slots"].is_array())
            for (const auto& slot : zmod_color["slots"])
                if (slot.is_object() && std::atoi(slot.value("ID", "0").c_str()) == slot_index + 1)
                    slot_info = slot;
        const std::string colour = "#" + (slot_info.is_object() ? slot_info.value("HEX", "FFFFFF") : std::string("FFFFFF"));
        mappings.push_back({{"toolId", tool},
                            {"slotId", slot_index + 1},
                            {"materialName", slot_info.is_object() ? slot_info.value("Material", "") : ""},
                            {"toolMaterialColor", colour},
                            {"slotMaterialColor", colour}});
    }

    DynamicPrintConfig host_config;
    host_config.set_key_value("print_host", new ConfigOptionString(flashforge_app_host()));
    host_config.set_key_value("flashforge_serial_number", new ConfigOptionString(serial_number));
    host_config.set_key_value("printhost_apikey", new ConfigOptionString(check_code));
    const Flashforge flashforge(&host_config);

    // Flashforge's own timelapse records from the screen's camera, which Zmod has users turn off
    // in favour of its own camera; the dialog's Timelapse option is the plugin's.
    PrintHostUpload upload;
    upload.source_path   = gcode_path;
    upload.upload_path   = filename;
    upload.post_action   = PrintHostPostUploadAction::StartPrint;
    upload.extended_info = {{"levelingBeforePrint", params.task_bed_leveling ? "1" : "0"},
                            {"flowCalibration", params.task_flow_cali ? "1" : "0"},
                            {"timeLapseVideo", "0"},
                            {"useMatlStation", "1"},
                            {"gcodeToolCnt", std::to_string(mappings.size())},
                            {"materialMappings", mappings.dump()}};

    if (update_fn)
        update_fn(PrintingStageUpload, 0, "Uploading G-code...");
    BOOST_LOG_TRIVIAL(info) << "ZmodPrinterAgent: sending " << filename << " to Flashforge's app with mappings " << mappings.dump();
    wxString   error;
    const bool sent = flashforge.upload(
        upload,
        [&](Http::Progress progress, bool& cancel) {
            if (cancel_fn && cancel_fn())
                cancel = true;
            if (update_fn && progress.ultotal > 0)
                update_fn(PrintingStageUpload, int(progress.ulnow * 100 / progress.ultotal), "Uploading G-code...");
        },
        [&](wxString message) { error = std::move(message); },
        [](wxString, wxString) {});
    if (!sent) {
        BOOST_LOG_TRIVIAL(error) << "ZmodPrinterAgent: Flashforge upload failed: " << error.ToUTF8().data();
        return (cancel_fn && cancel_fn()) ? BAMBU_NETWORK_ERR_CANCELED : BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    }
    return BAMBU_NETWORK_SUCCESS;
}

int ZmodPrinterAgent::start_uploaded_print(const PrintParams& params, const std::string& filename)
{
    // PRINT_ZCOLOR takes the slot (1-based) for each tool number in the file.
    const std::map<int, int> slots      = tool_slots(params);
    const int                tool_count = slots.empty() ? 1 : slots.rbegin()->first + 1;

    std::string escaped = filename;
    for (size_t pos = 0; (pos = escaped.find('"', pos)) != std::string::npos; pos += 2)
        escaped.insert(pos, "\\");

    std::string gcode = "_PREPARE_PRINT\nPRINT_ZCOLOR FILENAME=\"" + escaped + "\"" +
                        " LEVELING=" + std::to_string(params.task_bed_leveling ? 1 : 0) +
                        " AUTOPA=" + std::to_string(params.task_flow_cali ? 1 : 0) +
                        " ALLOWED_TOOL_COUNT=" + std::to_string(tool_count);
    for (int tool = 0; tool < tool_count; ++tool) {
        // A tool the file does not use still needs a slot; keep it on its own.
        const auto it = slots.find(tool);
        gcode += " T" + std::to_string(tool) + "=" + std::to_string((it != slots.end() ? it->second : std::min(tool, ZMOD_SLOTS - 1)) + 1);
    }

    // PRINT_ZCOLOR homes, heats and may run Auto PA before the file starts, which outlasts an HTTP
    // request, so it is not waited for; Zmod reports failures on the printer's console.
    BOOST_LOG_TRIVIAL(info) << "ZmodPrinterAgent: starting print:\n" << gcode;
    send_gcode_async(device_info.dev_id, gcode, [](bool ok) {
        if (!ok)
            BOOST_LOG_TRIVIAL(error) << "ZmodPrinterAgent: PRINT_ZCOLOR request failed";
    });
    return BAMBU_NETWORK_SUCCESS;
}

} // namespace Slic3r
