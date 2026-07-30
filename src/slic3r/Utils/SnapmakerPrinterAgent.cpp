#include "SnapmakerPrinterAgent.hpp"
#include "Http.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <thread>

namespace Slic3r {

namespace {

constexpr const char* SNAPMAKER_AGENT_VERSION = "0.0.1";

// Safely access a parallel array by index, returning a fallback if out of bounds.
template<typename T>
T safe_at(const std::vector<T>& vec, int index, const T& fallback)
{
    return (index >= 0 && index < static_cast<int>(vec.size())) ? vec[index] : fallback;
}

std::string find_closest_color_preset_by_vendor_and_type(const PresetCollection& filaments,
                                                         const std::string&      vendor_name,
                                                         const std::string&      filament_type,
                                                         const std::string&      color_rgba)
{
    std::string best_match_id       = "";
    int         best_color_distance = 0xffffffff;

    for (const auto& p : filaments.get_presets()) {
        if (p.is_visible && p.is_compatible &&
            // Filament profile must be detached from parent to be considered for matching
            filaments.get_preset_base(p) == &p && p.config.opt_string("filament_vendor", 0u) == vendor_name &&
            p.config.opt_string("filament_type", 0u) == filament_type) {
            // The printer returns RGBA in the format RRGGBBAA, but profiles store color as #RRGGBB,
            // so we must remove # and ignore alpha channel for distance calculation
            unsigned int target_color_value = std::stoul(color_rgba.substr(0, color_rgba.length() - 2), nullptr, 16);

            std::string  p_color = p.config.opt_string("default_filament_colour", 0u);
            unsigned int p_color_value;
            if (!p_color.empty()) {
                unsigned int hash_pos = p_color.find("#");
                p_color_value         = std::stoul(p_color.substr(hash_pos != std::string::npos ? hash_pos + 1 : 0), nullptr, 16);
            } else {
                // Default to black if no color specified in profile. Assume other profiles might be a closer color match.
                // Could be a problem if the target color is also black and there exist a specific profile for that type, vendor and color
                // combination.
                p_color_value = 0;
            }

            // Calculate Euclidean color distance in RGB space
            int dr = ((target_color_value & 0xff) - (p_color_value & 0xff));
            int dg = (((target_color_value >> 8) & 0xff) - ((p_color_value >> 8) & 0xff));
            int db = (((target_color_value >> 16) & 0xff) - ((p_color_value >> 16) & 0xff));
            unsigned int distance = dr * dr + dg * dg + db * db;

            if (distance < best_color_distance) {
                best_color_distance = distance;
                best_match_id       = p.filament_id;
            }
        }
    }
    return best_match_id;
}

} // anonymous namespace

SnapmakerPrinterAgent::SnapmakerPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

int SnapmakerPrinterAgent::command_start_camera(std::string dev_id)
{
    (void) dev_id;
    // why: the printer executes this over the websocket but answers only over MQTT, and the
    // call itself blocks on socket I/O - it fires from the camera view's renew timer on the UI
    // thread, so run it detached rather than block the caller on a reply that never comes.
    // note: interval is dead time in SECONDS on top of a ~0.455 s capture, so 0 is the 2.15 fps
    // ceiling (1 measures 0.63 fps), and it cannot be changed while a capture task is running.
    std::thread([this] {
        send_ws_rpc("camera.start_monitor",
                    {{"domain", "lan"}, {"interval", 0}, {"expect_pw", false}});
    }).detach();
    return BAMBU_NETWORK_SUCCESS;
}

std::string SnapmakerPrinterAgent::webcam_stream_override(const std::string& base_url) const
{
    const std::string snapshot_url = join_url(base_url, "/server/files/camera/monitor.jpg");

    // why: one wrapper file per printer - two U1s would otherwise overwrite each other's URL.
    const boost::filesystem::path page = boost::filesystem::path(data_dir()) / "cache" /
        ("snapmaker_camera_" + sanitize_filename(device_info.dev_ip) + ".html");

    // why: the printer writes a still JPEG at ~2 fps, so the page polls it with a cache buster
    // instead of consuming a stream. Chaining the next request off onload (never a bare
    // setInterval) keeps requests from piling up when the printer is slow to answer.
    const std::string html =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Camera</title><style>"
        "html,body{margin:0;height:100%;background:#000;overflow:hidden}"
        "img{width:100%;height:100%;object-fit:contain;display:block}</style></head>"
        "<body><img id=\"frame\" alt=\"\"><script>\n"
        "var src=\"" + snapshot_url + "\";\n"
        "var img=document.getElementById(\"frame\");\n"
        "function next(){img.src=src+\"?_nocache=\"+Date.now()+\"_\"+Math.floor(Math.random()*10000);}\n"
        "img.onload=function(){setTimeout(next,250);};\n"
        "img.onerror=function(){setTimeout(next,1000);};\n"
        "next();\n"
        "</script></body></html>\n";

    std::string write_error;
    try {
        boost::filesystem::create_directories(page.parent_path());
        boost::nowide::ofstream out(page.string().c_str(), std::ios::binary | std::ios::trunc);
        out << html;
        out.close();
        // note: an ofstream reports a failed write in its state, not by throwing.
        if (!out) {
            write_error = "write failed";
        }
    } catch (const std::exception& e) {
        write_error = e.what();
    }
    if (!write_error.empty()) {
        // why: no wrapper means no camera - a raw monitor.jpg URL would render one frozen frame
        // and read as a broken feed, so fall back to showing nothing and say why in the log.
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent: could not write camera page " << page.string()
                                   << ": " << write_error;
        return {};
    }

    return "file://" + page.generic_string();
}

AgentInfo SnapmakerPrinterAgent::get_agent_info_static()
{
    return AgentInfo{"snapmaker", "Snapmaker", SNAPMAKER_AGENT_VERSION, "Snapmaker printer agent"};
}

std::string SnapmakerPrinterAgent::combine_filament_type(const std::string& type, const std::string& sub_type)
{
    const std::string base = trim_and_upper(type);
    const std::string sub  = trim_and_upper(sub_type);

    if (base.empty())
        return "PLA";

    if (sub.empty() || sub == "NONE")
        return base;

    if (sub == "CF")
        return base + "-CF";
    if (sub == "GF")
        return base + "-GF";
    if (sub == "SNAPSPEED" || sub == "HS")
        return base + " HIGH SPEED";
    if (sub == "SILK")
        return base + " SILK";
    if (sub == "WOOD")
        return base + " WOOD";
    if (sub == "MATTE")
        return base + " MATTE";
    if (sub == "MARBLE")
        return base + " MARBLE";

    // Unrecognized sub-type (brand names like Polylite, Basic, etc.) -- use base type only
    return base;
}

bool SnapmakerPrinterAgent::fetch_filament_info(std::string dev_id)
{
    std::string url = join_url(device_info.base_url, "/printer/objects/query?print_task_config&filament_detect");

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
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: HTTP request failed: " << http_error;
        return false;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: Invalid JSON response";
        return false;
    }

    // Navigate to result.status.print_task_config
    if (!json.contains("result") || !json["result"].contains("status") ||
        !json["result"]["status"].contains("print_task_config")) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: Missing print_task_config in response";
        return false;
    }

    auto& ptc = json["result"]["status"]["print_task_config"];

    // Read parallel arrays from print_task_config
    auto filament_exist    = ptc.value("filament_exist", std::vector<bool>{});
    auto filament_type     = ptc.value("filament_type", std::vector<std::string>{});
    auto filament_sub_type = ptc.value("filament_sub_type", std::vector<std::string>{});
    auto filament_color    = ptc.value("filament_color_rgba", std::vector<std::string>{});
    auto filament_vendor   = ptc.value("filament_vendor", std::vector<std::string>{});

    const int slot_count = static_cast<int>(filament_exist.size());
    if (slot_count == 0) {
        BOOST_LOG_TRIVIAL(info) << "SnapmakerPrinterAgent::fetch_filament_info: No filament slots reported";
        return false;
    }

    // Read NFC filament_detect data for temperature info (optional)
    nlohmann::json nfc_info;
    if (json["result"]["status"].contains("filament_detect") &&
        json["result"]["status"]["filament_detect"].contains("info")) {
        nfc_info = json["result"]["status"]["filament_detect"]["info"];
    }

    static const std::string empty_str;
    static const std::string default_color = "FFFFFFFF";

    std::vector<AmsTrayData> trays;
    trays.reserve(slot_count);

    for (int i = 0; i < slot_count; ++i) {
        AmsTrayData tray;
        tray.slot_index   = i;
        tray.has_filament = filament_exist[i];

        if (tray.has_filament) {
            tray.tray_type     = combine_filament_type(safe_at(filament_type, i, empty_str),
                                                       safe_at(filament_sub_type, i, empty_str));
            tray.tray_color    = safe_at(filament_color, i, default_color);

            auto* bundle = GUI::wxGetApp().preset_bundle;
            // Try to find a matching preset for this filament based on vendor, type and color.
            // If not found, default to traditional search by type only or generic type mapping.
            if (bundle) {
                std::string vendor      = safe_at(filament_vendor, i, empty_str);
                std::string filament_id = find_closest_color_preset_by_vendor_and_type(bundle->filaments, vendor, tray.tray_type,
                                                                                       tray.tray_color);

                if (!filament_id.empty()) {
                    tray.tray_info_idx = filament_id;
                    BOOST_LOG_TRIVIAL(warning) << "Filament sync: Found manufacturer-specific profile for slot " << i << ": "
                                               << filament_id;
                } else {
                    tray.tray_info_idx = bundle->filaments.filament_id_by_type(tray.tray_type);
                }
            } else {
                tray.tray_info_idx = map_filament_type_to_generic_id(tray.tray_type);
            }

            // Extract NFC temperature data if available
            if (nfc_info.is_array() && i < static_cast<int>(nfc_info.size()) && nfc_info[i].is_object()) {
                auto& nfc_slot = nfc_info[i];
                std::string vendor = nfc_slot.value("VENDOR", "NONE");
                if (vendor != "NONE" && !vendor.empty()) {
                    tray.bed_temp    = nfc_slot.value("BED_TEMP", 0);
                    tray.nozzle_temp = nfc_slot.value("FIRST_LAYER_TEMP", 0);
                }
            }
        }

        trays.emplace_back(std::move(tray));
    }

    build_ams_payload(1, slot_count - 1, trays);
    return true;
}

} // namespace Slic3r
