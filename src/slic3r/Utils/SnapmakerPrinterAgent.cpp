#include "SnapmakerPrinterAgent.hpp"
#include "Http.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/log/trivial.hpp>
#include <iomanip>
#include <sstream>
#include <boost/algorithm/string.hpp>
#include <limits>
#include <cctype>
#include <algorithm>

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
                                                         const std::string&      base_type,
                                                         const std::string&      sub_type,
                                                         const std::string&      color_rgba)
{
    auto upper = [](std::string v) {
        boost::trim(v);
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return v;
    };
    const std::string want_type = upper(base_type);
    const std::string want_sub  = upper(sub_type);

    std::string best_match_id;
    long long   best_score = std::numeric_limits<long long>::min();

    for (const auto& p : filaments.get_presets()) {
        // System vendor presets inherit from a shared base, so a detached-from-parent
        // requirement would exclude the very profiles we want (e.g. "Snapmaker PLA SnapSpeed").
        if (!p.is_visible || !p.is_compatible)
            continue;
        if (p.config.opt_string("filament_vendor", 0u) != vendor_name)
            continue;
        // Match on the firmware's BASE type: a "PLA SnapSpeed" spool reports type PLA, and the
        // matching preset's filament_type is PLA too. Allow prefixed variants ("PLA-CF") so a
        // CF/GF sub-type can still find its dashed-type preset.
        const std::string p_type = upper(p.config.opt_string("filament_type", 0u));
        if (p_type != want_type && p_type.rfind(want_type, 0) != 0)
            continue;

        // The reported sub-type names the product line ("SnapSpeed", "PolyTerra", "Matte");
        // a preset whose name carries it is a far stronger signal than any color proximity.
        const bool sub_match = !want_sub.empty() && want_sub != "NONE" &&
                               (upper(p.alias.empty() ? p.name : p.alias).find(want_sub) != std::string::npos);

        // The printer returns RGBA as RRGGBBAA; profiles store #RRGGBB. Compare ignoring alpha;
        // a preset without a default color scores as black (worst case: ties broken by name).
        unsigned int target_color_value = std::stoul(color_rgba.substr(0, color_rgba.length() - 2), nullptr, 16);
        std::string  p_color            = p.config.opt_string("default_filament_colour", 0u);
        unsigned int p_color_value      = 0;
        if (!p_color.empty()) {
            size_t hash_pos = p_color.find('#');
            p_color_value   = std::stoul(p_color.substr(hash_pos != std::string::npos ? hash_pos + 1 : 0), nullptr, 16);
        }
        int dr = ((target_color_value & 0xff) - (p_color_value & 0xff));
        int dg = (((target_color_value >> 8) & 0xff) - ((p_color_value >> 8) & 0xff));
        int db = (((target_color_value >> 16) & 0xff) - ((p_color_value >> 16) & 0xff));
        long long distance = (long long) dr * dr + (long long) dg * dg + (long long) db * db;

        long long score = (sub_match ? (1LL << 40) : 0) - distance;
        if (score > best_score) {
            best_score    = score;
            best_match_id = p.filament_id;
        }
    }
    return best_match_id;
}

} // anonymous namespace

SnapmakerPrinterAgent::SnapmakerPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

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
    if (!ensure_device_info(dev_id))
        return false;

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
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: HTTP request failed: " << http_error << " url=" << url << " dev_id=" << dev_id << " dev_ip=" << device_info.dev_ip;
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
                std::string filament_id = find_closest_color_preset_by_vendor_and_type(bundle->filaments, vendor,
                                                                                       safe_at(filament_type, i, empty_str),
                                                                                       safe_at(filament_sub_type, i, empty_str),
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

            // Extract NFC data if available. A slot whose data comes from an NFC tag is
            // authoritative: stamp its CARD_UID into tag_uid (Bambu RFID convention) so
            // consumers can tell tag-backed slots from manually configured ones and honor
            // the tag on any write path.
            if (nfc_info.is_array() && i < static_cast<int>(nfc_info.size()) && nfc_info[i].is_object()) {
                auto& nfc_slot = nfc_info[i];
                std::string vendor = nfc_slot.value("VENDOR", "NONE");
                if (vendor != "NONE" && !vendor.empty()) {
                    tray.bed_temp    = nfc_slot.value("BED_TEMP", 0);
                    tray.nozzle_temp = nfc_slot.value("FIRST_LAYER_TEMP", 0);
                    uint64_t card_uid = nfc_slot.value("CARD_UID", (uint64_t) 0);
                    if (card_uid != 0) {
                        std::ostringstream uid;
                        uid << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << card_uid;
                        tray.tag_uid = uid.str();
                    }
                }
            }
        }

        trays.emplace_back(std::move(tray));
    }

    // U1 is a physical toolchanger: one 1-slot unit per tool, not a chunked 4-slot MMU box.
    build_ams_payload(slot_count, slot_count - 1, trays, AmsUnitShape::Toolchanger);
    return true;
}

bool SnapmakerPrinterAgent::push_filament_info(std::string dev_id, const FilamentSlotInfo& info)
{
    if (!ensure_device_info(dev_id))
        return false;

    // Dialect: the same command the printer's own device UI sends (see Snapmaker's device tab).
    // ALL parameters are mandatory -- omitting one both errors AND partially applies on current
    // firmware -- so empty values are sent as quoted-empty. Sub-type and vendor may contain
    // spaces; type and color are plain tokens.
    auto quoted = [](std::string v) {
        v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
        return "\"" + v + "\"";
    };
    std::string color = info.color_rgba.empty() ? "FFFFFFFF" : info.color_rgba;
    std::string script = "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=" + std::to_string(info.slot) +
                         " FILAMENT_TYPE=" + info.type +
                         " FILAMENT_SUBTYPE=" + quoted(info.sub_type) +
                         " FILAMENT_COLOR_RGBA=" + color +
                         " VENDOR=" + quoted(info.vendor) + " SAVE=1";

    std::string url = join_url(device_info.base_url, "/printer/gcode/script");
    nlohmann::json body;
    body["script"] = script;

    bool        success = false;
    std::string http_error;
    auto        http = Http::post(url);
    if (!device_info.api_key.empty())
        http.header("X-Api-Key", device_info.api_key);
    http.header("Content-Type", "application/json")
        .set_post_body(body.dump())
        .timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string, unsigned status) { success = (status == 200); })
        .on_error([&](std::string, std::string err, unsigned status) {
            http_error = err + " (HTTP " + std::to_string(status) + ")";
        })
        .perform_sync();

    if (!success)
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::push_filament_info failed: " << http_error
                                   << " url=" << url << " dev_id=" << dev_id << " dev_ip=" << device_info.dev_ip
                                   << ", script: " << script;
    else
        BOOST_LOG_TRIVIAL(info) << "SnapmakerPrinterAgent::push_filament_info ok: " << script;
    return success;
}

} // namespace Slic3r
