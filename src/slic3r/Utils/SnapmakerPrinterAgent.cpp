#include "SnapmakerPrinterAgent.hpp"
#include "Http.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "IPrinterAgent.hpp"
#include "NetworkAgentFactory.hpp"
#include "bambu_networking.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>
#include <chrono>
#include <functional>
#include <exception>
#include <regex>
#include <tuple>
#include <vector>
#include <string>
#include "libslic3r/Preset.hpp"
#include <cstddef>
#include <utility>
#include <algorithm>
#include <cctype>

namespace Slic3r {

namespace {

constexpr const char* SNAPMAKER_AGENT_VERSION = "0.0.1";

// Ranks a candidate that declares no usable colour behind every candidate that does, while
// still leaving it ahead of no match at all. The largest real distance is 3 * 255^2 = 195075.
constexpr unsigned int UNRANKED_COLOR_DISTANCE = 0xfffffffeu;

// filament_vendor of the presets Orca ships in OrcaFilamentLibrary, set in that library's own
// filament/base/fdm_filament_common.json, so every "Generic ..." preset inherits it.
constexpr const char* GENERIC_FILAMENT_VENDOR = "Generic";

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

// A tag reports the fibre loading in its sub type ("CF10", "GF30"), while the material types in
// MaterialType::all() and the profile names only ever say CF or GF. Expects upper case input.
std::string normalize_fibre_sub_type(const std::string& sub)
{
    for (const char* fibre : {"CF", "GF"}) {
        const std::string prefix = fibre;
        if (sub.size() >= prefix.size() && sub.compare(0, prefix.size(), prefix) == 0 &&
            std::all_of(sub.begin() + static_cast<std::string::difference_type>(prefix.size()), sub.end(),
                        [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
            return prefix;
    }
    return sub;
}

// Like contains_word_ci, but a trailing digit run belongs to the same word, so a CF sub type
// matches a "PA12-CF10" preset as well as "PA12-CF". A trailing letter does not: "CFX" is
// a different product.
bool contains_sub_type_ci(const std::string& haystack, const std::string& needle)
{
    if (needle.empty() || needle.size() > haystack.size())
        return false;

    const auto equal_ci = [](unsigned char a, unsigned char b) { return std::toupper(a) == std::toupper(b); };
    const auto is_word  = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };

    for (auto it = haystack.begin(); it != haystack.end(); ++it) {
        it = std::search(it, haystack.end(), needle.begin(), needle.end(), equal_ci);
        if (it == haystack.end())
            return false;

        auto after = it + static_cast<std::string::difference_type>(needle.size());
        while (after != haystack.end() && std::isdigit(static_cast<unsigned char>(*after)) != 0)
            ++after;

        const bool left_ok  = it == haystack.begin() || !is_word(*(it - 1));
        const bool right_ok = after == haystack.end() || !is_word(*after);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

// Tags written by older tools (or as a workaround) carry the full product name in the vendor
// field, e.g. vendor "Anycubic PLA Basic" with an empty sub type, while profiles may use either
// "Anycubic" or "Anycubic PLA Basic". Treat the vendors as matching when they are equal or when
// either contains the other as whole words, ignoring case.
bool vendor_matches(const std::string& tag_vendor, const std::string& profile_vendor)
{
    if (tag_vendor.empty() || profile_vendor.empty())
        return false;
    return contains_word_ci(tag_vendor, profile_vendor) || contains_word_ci(profile_vendor, tag_vendor);
}
// Rank the visible, compatible base presets of one filament type by colour distance, preferring a
// name that carries the spool's sub type. Candidates are limited to vendor_filter, which nothing
// matches when it is empty; tag_vendor is the tag's own vendor field, which a legacy tag uses to
// name the variant.
std::string closest_preset_by_colour(const PresetCollection& filaments, const std::string& vendor_filter,
                                     const std::string& tag_vendor, const std::string& filament_type,
                                     const std::string& sub_wanted, unsigned int target_color_value)
{
    std::string  best_match_id       = "";
    unsigned int best_color_distance = 0xffffffffu;
    bool         best_matches_sub    = false;

    for (const auto& p : filaments.get_presets()) {
        // Only a base preset can be loaded: sync_ams_list() resolves a filament_id to the first
        // base preset carrying it, so a derived copy never loads and could outrank its parent.
        if (p.is_visible && p.is_compatible && filaments.get_preset_base(p) == &p && !p.filament_id.empty() &&
            vendor_matches(vendor_filter, p.config.opt_string("filament_vendor", 0u)) &&
            p.config.opt_string("filament_type", 0u) == filament_type) {
            // A profile that specifies no colour, or one that cannot be parsed, is unranked: it
            // is kept as a last resort rather than scored as black, which would otherwise let a
            // colourless profile win every dark spool and beat profiles that do declare a colour.
            std::string  p_color       = p.config.opt_string("default_filament_colour", 0u);
            unsigned int p_color_value = 0;
            unsigned int distance      = UNRANKED_COLOR_DISTANCE;
            if (parse_rgb(p_color, p_color_value)) {
                // Euclidean colour distance in RGB space
                const int dr = ((target_color_value & 0xff) - (p_color_value & 0xff));
                const int dg = (((target_color_value >> 8) & 0xff) - ((p_color_value >> 8) & 0xff));
                const int db = (((target_color_value >> 16) & 0xff) - ((p_color_value >> 16) & 0xff));
                distance      = static_cast<unsigned int>(dr * dr + dg * dg + db * db);
            }

            // Variants (Matte, Silk, Basic, Eco, ...) are not filament_type values -- the
            // profiles carry them in the preset name, so "Snapmaker PLA Matte" and
            // "Snapmaker PLA Basic" are both vendor Snapmaker, type PLA. Prefer a candidate
            // whose name mentions the spool's sub type, and rank by colour within that
            // preference, so a matte spool stops resolving to whichever variant happens to
            // sit nearest in colour.
            // A profile's own filament_subtype counts too. When the tag has no sub type, a
            // legacy tag may still name the variant in its vendor field ("Anycubic PLA Silk"),
            // so look for the profile's subtype, or the whole tag vendor, there instead.
            const std::string p_sub = p.config.has("filament_subtype") ? p.config.opt_string("filament_subtype", 0u) : std::string();
            const bool matches_sub  = !sub_wanted.empty() ?
                                          (contains_sub_type_ci(p.name, sub_wanted) || contains_sub_type_ci(p_sub, sub_wanted)) :
                                          (contains_word_ci(tag_vendor, p_sub) || contains_word_ci(p.name, tag_vendor) ||
                                           contains_word_ci(tag_vendor, p.name));
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

std::string SnapmakerPrinterAgent::find_closest_color_preset_by_vendor_and_type(const PresetCollection& filaments,
                                                                                const std::string&      vendor_name,
                                                                                const std::string&      filament_type,
                                                                                const std::string&      sub_type,
                                                                                const std::string&      color_rgba)
{
    // The printer returns RGBA in the format RRGGBBAA, but profiles store color as #RRGGBB,
    // so we must remove # and ignore alpha channel for distance calculation.
    // An unusable colour cannot rank candidates; leave the match to the type-only lookup.
    unsigned int target_color_value = 0;
    if (!parse_rgb(color_rgba, target_color_value))
        return {};

    const std::string sub_wanted = normalize_fibre_sub_type(trim_and_upper(sub_type));

    // The spool's own vendor first, with the sub type as a preference. A tag that reports no
    // vendor matches no profile's filament_vendor, so such a spool finds nothing in this pass.
    const std::string vendor_match = closest_preset_by_colour(filaments, vendor_name, vendor_name, filament_type,
                                                              sub_wanted, target_color_value);
    if (!vendor_match.empty())
        return vendor_match;

    // Orca ships no preset for this spool's vendor, so fall back to its own Generic presets: they
    // cover the sub types the U1 sees (Generic PLA Silk @U1, Generic PLA High Speed @U1,
    // Generic PETG HF @U1) and carry no other brand's temperatures or flow. That beats another
    // brand's preset of the same finish, and it beats the caller's fallback,
    // filament_id_by_type(), which takes the first visible preset of the type whatever its vendor
    // and finish.
    return closest_preset_by_colour(filaments, GENERIC_FILAMENT_VENDOR, vendor_name, filament_type, sub_wanted,
                                    target_color_value);
}

SnapmakerPrinterAgent::SnapmakerPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

AgentInfo SnapmakerPrinterAgent::get_agent_info_static()
{
    return AgentInfo{SNAPMAKER_PRINTER_AGENT_ID, "Snapmaker", SNAPMAKER_AGENT_VERSION, "Snapmaker printer agent"};
}

std::string SnapmakerPrinterAgent::combine_filament_type(const std::string& type, const std::string& sub_type)
{
    const std::string base = trim_and_upper(type);
    const std::string sub  = trim_and_upper(sub_type);

    if (base.empty())
        return "PLA";

    if (sub.empty() || sub == "NONE")
        return base;

    // Only CF and GF are material types, whatever fibre loading the tag reports ("CF10"); skip
    // the suffix when the base already carries it.
    const std::string fibre = normalize_fibre_sub_type(sub);
    if (fibre == "CF" || fibre == "GF") {
        const std::string suffix     = "-" + fibre;
        const bool        has_suffix = base.size() >= suffix.size() &&
                                base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0;
        return has_suffix ? base : base + suffix;
    }

    // Every other sub type is a finish or a brand name, not a material type.
    return base;
}

bool SnapmakerPrinterAgent::fetch_filament_info(std::string dev_id, FilamentSyncMode /*sync_mode*/)
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
                    BOOST_LOG_TRIVIAL(warning) << "Filament sync: Found profile for slot " << i << ": "
                                               << filament_id;
                } else {
                    tray.tray_info_idx = bundle->filaments.filament_id_by_type(tray.tray_type);
                    if (tray.tray_info_idx.empty())
                        tray.tray_info_idx = map_filament_type_to_generic_id(tray.tray_type);
                }
            } else {
                tray.tray_info_idx = map_filament_type_to_generic_id(tray.tray_type);
            }

            // Extract NFC temperature data if available
            if (nfc_info.is_array() && i < static_cast<int>(nfc_info.size()) && nfc_info[i].is_object()) {
                auto& nfc_slot = nfc_info[i];
                tray.bed_temp  = safe_json_int(nfc_slot, "BED_TEMP");
                // build_ams_payload publishes this as `nozzle_temp_max`, and FIRST_LAYER_TEMP is 0 for external tags.
                tray.nozzle_temp = safe_json_int(nfc_slot, "HOTEND_MAX_TEMP");
            }
        }

        trays.emplace_back(std::move(tray));
    }

    build_ams_payload(1, slot_count - 1, trays);
    return true;
}

bool SnapmakerPrinterAgent::firmware_supports_print_options(const std::string& firmware_version)
{
    // The U1 reports its firmware version as moonraker_version, read from /etc/VERSION
    static const std::regex version_regex(R"(^\s*[vV]?(\d+)\.(\d+)(?:\.(\d+))?)");
    std::smatch             match;
    if (!std::regex_search(firmware_version, match, version_regex)) {
        return false;
    }
    try {
        const int major = std::stoi(match[1].str());
        const int minor = std::stoi(match[2].str());
        const int patch = match[3].matched ? std::stoi(match[3].str()) : 0;
        return std::make_tuple(major, minor, patch) >= std::make_tuple(1, 3, 0);
    } catch (const std::exception&) {
        return false; // A component too large for an int
    }
}

nlohmann::json SnapmakerPrinterAgent::build_map_table(const std::string& ams_mapping)
{
    // Limits checked by the firmware's SET_PRINT_TASK_PARAMETERS (klippy/extras/print_task_config.py)
    constexpr int logical_extruder_count  = 32;
    constexpr int physical_extruder_count = 4;

    nlohmann::json map_table = nlohmann::json::array();
    const auto     mapping   = nlohmann::json::parse(ams_mapping, nullptr, false);
    if (!mapping.is_array()) {
        return map_table;
    }
    for (int filament = 0; filament < int(mapping.size()) && filament < logical_extruder_count; ++filament) {
        if (!mapping[filament].is_number_integer()) {
            continue;
        }
        // An unmapped filament is left out: the firmware accepts a negative toolhead and indexes it from the end
        const int toolhead = mapping[filament].get<int>();
        if (toolhead < 0 || toolhead >= physical_extruder_count) {
            continue;
        }
        map_table.push_back({filament, toolhead});
    }
    return map_table;
}

nlohmann::json SnapmakerPrinterAgent::build_start_local_print_params(const PrintParams& params, const std::string& filename)
{
    // Moonraker turns each option into a parameter of SDCARD_PRINT_FILE_WITH_PARAMETERS, written with Python's str(),
    // so a JSON true would arrive as "True", which the firmware's get_int() rejects. The switches are sent as 0 or 1.
    nlohmann::json options{
        {"bed_level", params.task_bed_leveling ? 1 : 0},
        {"flow_calibrate", params.task_flow_cali ? 1 : 0},
        {"time_lapse_camera", params.task_record_timelapse ? 1 : 0},
    };
    // Without a map_table the firmware prints each filament with the toolhead of the same number
    nlohmann::json map_table = build_map_table(params.ams_mapping);
    if (!map_table.empty()) {
        options["map_table"] = std::move(map_table);
    }

    nlohmann::json request{{"path", filename}, {"options", std::move(options)}};
    // The firmware prints Metadata/plate_<print_plate>.gcode from a 3MF, defaulting to plate 1
    if (boost::iends_with(filename, ".3mf") && params.plate_index > 0) {
        request["print_plate"] = params.plate_index;
    }
    return request;
}

bool SnapmakerPrinterAgent::init_device_info(const std::string& dev_id,
                                             const std::string& dev_ip,
                                             const std::string& username,
                                             const std::string& password,
                                             bool               use_ssl,
                                             const std::string& port)
{
    // The base clears the firmware version here, so a newly selected printer starts without options until it reports one
    print_options_supported.store(false);
    return MoonrakerPrinterAgent::init_device_info(dev_id, dev_ip, username, password, use_ssl, port);
}

bool SnapmakerPrinterAgent::fetch_device_info(const std::string&           base_url,
                                              const std::string&           api_key,
                                              MoonrakerDeviceInfo&         info,
                                              std::string&                 error,
                                              const std::function<bool()>& cancelled) const
{
    if (!MoonrakerPrinterAgent::fetch_device_info(base_url, api_key, info, error, cancelled)) {
        return false;
    }
    const bool supported = firmware_supports_print_options(info.version);
    print_options_supported.store(supported);
    BOOST_LOG_TRIVIAL(info) << "SnapmakerPrinterAgent: firmware " << info.version
                            << (supported ? " applies print options" : " predates print options, prints start without them");
    return true;
}

bool SnapmakerPrinterAgent::print_uploaded_file(const PrintParams& params, const std::string& filename)
{
    if (!print_options_supported.load()) {
        return MoonrakerPrinterAgent::print_uploaded_file(params, filename);
    }

    const nlohmann::json request_params = build_start_local_print_params(params, filename);
    BOOST_LOG_TRIVIAL(info) << "SnapmakerPrinterAgent: server.files.start_local_print " << request_params.dump();

    // Moonraker only serves this endpoint over its websocket and MQTT. It answers once the firmware has run its
    // pre-print checks, so the wait matches the agent's other print starts.
    nlohmann::json result;
    std::string    error;
    if (!send_websocket_request("server.files.start_local_print", request_params, std::chrono::seconds(60), result, error)) {
        // Not retried with SDCARD_PRINT_FILE: after a timeout the print may already be starting
        BOOST_LOG_TRIVIAL(error) << "SnapmakerPrinterAgent: start_local_print failed: " << error;
        return false;
    }
    // The firmware reports its own failures, such as a nozzle mismatch, in the result rather than as a JSON-RPC error
    const std::string state = result.is_object() ? result.value("state", std::string()) : std::string();
    if (state != "success") {
        const bool        has_message = result.is_object() && result.contains("message") && result["message"].is_string();
        const std::string message     = has_message ? result["message"].get<std::string>() : result.dump();
        BOOST_LOG_TRIVIAL(error) << "SnapmakerPrinterAgent: the printer refused the print: " << message;
        return false;
    }
    return true;
}

void SnapmakerPrinterAgent::add_print_capabilities(nlohmann::json& print) const
{
    if (!print_options_supported.load()) {
        return;
    }
    // Shows the flow calibration and timelapse options of the print dialog; bed leveling is shown by the base agent
    print["support_flow_calibration"] = true;
    print["support_timelapse"]        = true;
    // The device model reads home_flag after support_flow_calibration, and its bit 16 overrides that flag
    const int home_flag = print.contains("home_flag") && print["home_flag"].is_number_integer() ? print["home_flag"].get<int>() : 0;
    print["home_flag"]  = home_flag | (1 << 16);
}

} // namespace Slic3r
