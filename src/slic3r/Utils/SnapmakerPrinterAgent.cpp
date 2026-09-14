#include "SnapmakerPrinterAgent.hpp"
#include "Http.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace Slic3r {

namespace {

constexpr const char* SNAPMAKER_AGENT_VERSION = "0.0.1";

// Safely access a parallel array by index, returning a fallback if out of bounds.
template<typename T>
T safe_at(const std::vector<T>& vec, int index, const T& fallback)
{
    return (index >= 0 && index < static_cast<int>(vec.size())) ? vec[index] : fallback;
}

// Parse the RRGGBB part of a colour into 0xRRGGBB, skipping a leading '#' and
// dropping any alpha channel. Returns false rather than letting std::stoul throw
// on an empty or malformed value.
bool parse_rgb(const std::string& value, unsigned int& out)
{
    const size_t hash_pos = value.find('#');
    std::string  digits   = value.substr(hash_pos != std::string::npos ? hash_pos + 1 : 0);
    if (digits.size() > 6)
        digits.resize(6);
    if (digits.size() != 6)
        return false;
    for (char c : digits) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    }
    out = static_cast<unsigned int>(std::stoul(digits, nullptr, 16));
    return true;
}

// Case-insensitive whole-word search. The needle must not be flanked by alphanumeric
// characters, so "CF" matches "PETG-CF" and "PLA CF" but not "Scaffold" or "CF10".
bool contains_word_ci(const std::string& haystack, const std::string& needle)
{
    if (needle.empty() || needle.size() > haystack.size())
        return false;

    const auto equal_ci = [](unsigned char a, unsigned char b) { return std::toupper(a) == std::toupper(b); };
    const auto is_word  = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };

    for (auto it = haystack.begin(); it != haystack.end(); ++it) {
        it = std::search(it, haystack.end(), needle.begin(), needle.end(), equal_ci);
        if (it == haystack.end())
            return false;

        const auto after    = it + static_cast<std::string::difference_type>(needle.size());
        const bool left_ok  = it == haystack.begin() || !is_word(*(it - 1));
        const bool right_ok = after == haystack.end() || !is_word(*after);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

std::string find_closest_color_preset_by_vendor_and_type(const PresetCollection& filaments,
                                                         const std::string&      vendor_name,
                                                         const std::string&      filament_type,
                                                         const std::string&      sub_type,
                                                         const std::string&      color_rgba)
{
    std::string  best_match_id       = "";
    unsigned int best_color_distance = 0xffffffffu;
    bool         best_matches_sub    = false;

    // The printer returns RGBA in the format RRGGBBAA, but profiles store color as #RRGGBB,
    // so we must remove # and ignore alpha channel for distance calculation.
    // An unusable colour cannot rank candidates; leave the match to the type-only lookup.
    unsigned int target_color_value = 0;
    if (!parse_rgb(color_rgba, target_color_value))
        return best_match_id;

    for (const auto& p : filaments.get_presets()) {
        // A preset only needs a filament_id to serve as a tray identity. A user preset derived
        // from a system one inherits its parent's filament_id, which is also what the plate's
        // filament carries, so derived presets resolve correctly -- and the mapping uses the id
        // only to break ties between equal colour distances. Requiring a detached preset here
        // excluded essentially every user-made profile.
        if (p.is_visible && p.is_compatible && !p.filament_id.empty() &&
            p.config.opt_string("filament_vendor", 0u) == vendor_name &&
            p.config.opt_string("filament_type", 0u) == filament_type) {
            // Default to black when the profile specifies no color, or one that cannot be
            // parsed. Assume other profiles might be a closer color match. Could be a problem
            // if the target color is also black and there exist a specific profile for that
            // type, vendor and color combination.
            std::string  p_color       = p.config.opt_string("default_filament_colour", 0u);
            unsigned int p_color_value = 0;
            if (!parse_rgb(p_color, p_color_value))
                p_color_value = 0;

            // Calculate Euclidean color distance in RGB space
            int dr = ((target_color_value & 0xff) - (p_color_value & 0xff));
            int dg = (((target_color_value >> 8) & 0xff) - ((p_color_value >> 8) & 0xff));
            int db = (((target_color_value >> 16) & 0xff) - ((p_color_value >> 16) & 0xff));
            unsigned int distance = static_cast<unsigned int>(dr * dr + dg * dg + db * db);

            // Variants (Matte, Silk, Basic, Eco, ...) are not filament_type values -- the
            // profiles carry them in the preset name, so "Snapmaker PLA Matte" and
            // "Snapmaker PLA Basic" are both vendor Snapmaker, type PLA. Prefer a candidate
            // whose name mentions the spool's sub type, and rank by colour within that
            // preference, so a matte spool stops resolving to whichever variant happens to
            // sit nearest in colour.
            const bool matches_sub = contains_word_ci(p.name, sub_type);
            if (best_matches_sub && !matches_sub)
                continue;

            if ((matches_sub && !best_matches_sub) || distance < best_color_distance) {
                best_color_distance = distance;
                best_match_id       = p.filament_id;
                best_matches_sub    = matches_sub;
            }
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

    // Every other sub type is a finish or a brand name (Basic, Silk, Matte, Wood,
    // Marble, SnapSpeed, Polylite, ...), not a material. Presets are matched on
    // `filament_type`, whose values come from MaterialType::all(); "PLA SILK" is not
    // one of them, so composing it only makes the vendor+color search miss and leaves
    // filament_id_by_type to strip the modifier back to "PLA". Return the base type.
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
            const std::string raw_sub_type = safe_at(filament_sub_type, i, empty_str);
            tray.tray_type     = combine_filament_type(safe_at(filament_type, i, empty_str), raw_sub_type);
            tray.tray_color    = safe_at(filament_color, i, empty_str);
            if (tray.tray_color.empty())
                tray.tray_color = default_color;

            auto* bundle = GUI::wxGetApp().preset_bundle;
            // Try to find a matching preset for this filament based on vendor, type and color.
            // If not found, default to traditional search by type only or generic type mapping.
            if (bundle) {
                std::string vendor   = safe_at(filament_vendor, i, empty_str);
                std::string sub_type = raw_sub_type;
                if (trim_and_upper(sub_type) == "NONE")
                    sub_type.clear();
                std::string filament_id = find_closest_color_preset_by_vendor_and_type(bundle->filaments, vendor, tray.tray_type,
                                                                                       sub_type, tray.tray_color);

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
                tray.bed_temp  = safe_json_int(nfc_slot, "BED_TEMP");
                // build_ams_payload publishes tray.nozzle_temp as `nozzle_temp_max`, so it
                // has to be the hotend maximum. FIRST_LAYER_TEMP is neither that nor
                // dependable -- the firmware does not accept it through filament_detect/set,
                // so it stays 0 for every spool reported by an external tag reader.
                tray.nozzle_temp = safe_json_int(nfc_slot, "HOTEND_MAX_TEMP");
            }
        }

        trays.emplace_back(std::move(tray));
    }

    build_ams_payload(1, slot_count - 1, trays);
    return true;
}

} // namespace Slic3r
