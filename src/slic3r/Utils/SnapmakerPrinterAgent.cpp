#include "SnapmakerPrinterAgent.hpp"
#include "DeviceJson.hpp"

#include <boost/format.hpp>
#include "MoonrakerPrinterAgent.hpp"
#include "IPrinterAgent.hpp"
#include "NetworkAgentFactory.hpp"
#include "bambu_networking.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/log/trivial.hpp>
#include <vector>
#include <string>
#include "libslic3r/Preset.hpp"
#include <cstddef>
#include <utility>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <ios>
#include <cstdint>
#include <algorithm>
#include <cctype>
#include "PrintHost.hpp"

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

std::string SnapmakerProtocol::render_map_table(const std::vector<int>& filament_map_1based)
{
    std::string map_table = "[";
    bool        first = true;
    for (size_t i = 0; i < filament_map_1based.size(); ++i) {
        // Unassigned filaments are omitted; the device keeps its default mapping for them.
        if (filament_map_1based[i] <= 0)
            continue;
        if (!first) map_table += ", ";
        map_table += "[" + std::to_string(i) + ", " + std::to_string(filament_map_1based[i] - 1) + "]";
        first = false;
    }
    map_table += "]";
    return map_table;
}

std::string SnapmakerProtocol::build_start_script(const std::string& filename, const std::vector<int>& filament_map_1based)
{
    // The firmware reads an absent BED_LEVEL as off; the touchscreen default is on.
    return "SDCARD_PRINT_FILE_WITH_PARAMETERS FILENAME=\"" + filename + "\" MAP_TABLE=\"" +
           render_map_table(filament_map_1based) + "\" BED_LEVEL=\"1\"";
}

namespace {
// The firmware parses these as Python literals: "[a, b, c]", strings single-quoted.
template<class T, class Fmt> std::string render_list(const std::vector<T>& values, Fmt fmt)
{
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += ", ";
        out += fmt(values[i]);
    }
    return out + "]";
}

// Trailing zeros trimmed as the screen renders them: "220.0", "0.42", "0.966".
std::string render_double(double v)
{
    std::string out = (boost::format("%.6f") % v).str();
    const size_t dot = out.find('.');
    if (dot != std::string::npos) {
        size_t last = out.find_last_not_of('0');
        if (last == dot) ++last; // keep exactly one decimal, matching "220.0"
        out.erase(last + 1);
    }
    return out;
}

std::string quoted_param(const std::string& key, const std::string& value) { return " " + key + "=\"" + value + "\""; }
} // namespace

std::string SnapmakerProtocol::build_start_script(const std::string& filename, const DevicePrintJobInfo& job)
{
    // Options are always stated, even when "0": the firmware reads an absent parameter as off.
    std::string script = "SDCARD_PRINT_FILE_WITH_PARAMETERS";
    script += quoted_param("FILENAME", filename);
    script += quoted_param("MAP_TABLE", render_map_table(job.filament_map_1based));
    script += quoted_param("BED_LEVEL", job.option_on("bed_leveling") ? "1" : "0");
    script += quoted_param("TIME_LAPSE_CAMERA", job.option_on("time_lapse") ? "1" : "0");

    // Only tools this plate uses are calibrated.
    const bool flow = job.option_on("flow_calibrate");
    script += quoted_param("FLOW_CALIBRATE", flow ? "1" : "0");
    script += quoted_param("FLOW_CALIBRATE_EXTRUDERS",
                           render_list(flow ? job.used_physical_tools : std::vector<int>{},
                                       [](int v) { return std::to_string(v); }));
    // End-of-print unloading is left to the printer's own setting.
    script += quoted_param("END_UNLOAD_FILAMENT",
                           render_list(std::vector<int>(job.nozzle_diameter.size(), 0),
                                       [](int v) { return std::to_string(v); }));

    // The firmware calibrates flow under the plate's conditions.
    script += quoted_param("LINE_WIDTH", render_double(job.line_width));
    script += quoted_param("LAYER_HEIGHT", render_double(job.layer_height));
    script += quoted_param("OUTER_WALL_SPEED", render_double(job.outer_wall_speed));

    // Per physical tool.
    script += quoted_param("NOZZLE_DIAMETER_LIST", render_list(job.nozzle_diameter, render_double));
    // Per logical filament.
    script += quoted_param("NOZZLE_TEMP", render_list(job.nozzle_temp, render_double));
    script += quoted_param("FILAMENT_TYPE", render_list(job.filament_type,
                                                        [](const std::string& v) { return "'" + v + "'"; }));
    script += quoted_param("FILAMENT_FLOW_RATIO", render_list(job.flow_ratio, render_double));
    script += quoted_param("FILAMENT_DIAMETER", render_list(job.filament_diameter, render_double));
    script += quoted_param("FILAMENT_USED_G", render_list(job.used_g, render_double));
    script += quoted_param("FILAMENT_USED_MM", render_list(job.used_mm, render_double));
    return script;
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

std::string SnapmakerProtocol::card_uid_hex(const nlohmann::json& nfc_slot)
{
    const auto it = nfc_slot.find("CARD_UID");
    if (it == nfc_slot.end() || (!it->is_array() && !it->is_number_integer()))
        return std::string();

    std::ostringstream uid;
    uid << std::hex << std::uppercase << std::setfill('0');
    if (it->is_array()) {
        for (const auto& byte : *it) {
            if (!byte.is_number_integer())
                return std::string();
            uid << std::setw(2) << (byte.get<uint64_t>() & 0xFF);
        }
    } else {
        uid << std::setw(16) << it->get<uint64_t>();
    }

    // An all-zero id means no tag.
    std::string text = uid.str();
    if (text.size() < 16)
        text.insert(0, 16 - text.size(), '0');
    return text.find_first_not_of('0') == std::string::npos ? std::string() : text;
}

// Orca: may throw on unexpected field shapes; NetworkAgent::fetch_filament_info catches.
bool SnapmakerPrinterAgent::fetch_filament_info(std::string dev_id, FilamentSyncMode /*sync_mode*/)
{
    if (!ensure_device_info(dev_id))
        return false;
    if (fetch_afc_filament_info())
        return true;

    std::string url = join_url(device_info.base_url, "/printer/objects/query?print_task_config&filament_detect");

    std::string response_body;
    std::string http_error;
    if (!fetch_body(url, device_info.api_key, response_body, http_error)) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: HTTP request failed: " << http_error << " url=" << url << " dev_id=" << dev_id << " dev_ip=" << device_info.dev_ip;
        return false;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: Invalid JSON response dev_id=" << dev_id;
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
        tray.slot         = i;
        tray.extruder     = i;
        tray.virtual_tool = i;
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

            // Extract NFC data if available; a tagged slot is authoritative.
            if (nfc_info.is_array() && i < static_cast<int>(nfc_info.size()) && nfc_info[i].is_object()) {
                auto& nfc_slot = nfc_info[i];
                tray.bed_temp  = safe_json_int(nfc_slot, "BED_TEMP");
                // build_ams_payload publishes this as `nozzle_temp_max`, and FIRST_LAYER_TEMP is 0 for external tags.
                tray.nozzle_temp = safe_json_int(nfc_slot, "HOTEND_MAX_TEMP");
                tray.tag_uid     = SnapmakerProtocol::card_uid_hex(nfc_slot);
            }
        }

        trays.emplace_back(std::move(tray));
    }

    build_ams_payload(slot_count, slot_count - 1, trays, AmsUnitShape::Toolchanger);
    return true;
}

bool SnapmakerPrinterAgent::push_filament_info(std::string dev_id, const FilamentSlotInfo& info)
{
    if (afc_active())
        return MoonrakerPrinterAgent::push_filament_info(dev_id, info);
    if (!ensure_device_info(dev_id))
        return false;

    // Every parameter is mandatory: omitting one errors yet partially applies, so empty values
    // are sent quoted. Sub-type and vendor may contain spaces.
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

    if (!send_gcode_sync(dev_id, script))
        return false;
    BOOST_LOG_TRIVIAL(info) << "SnapmakerPrinterAgent::push_filament_info ok: " << script;
    return true;
}

// One agent instance addresses one bound device, so the stash is not keyed by dev_id.
bool SnapmakerPrinterAgent::send_filament_mapping(const std::string& /*dev_id*/, const std::vector<int>& tool_to_slot)
{
    m_pending_filament_map = tool_to_slot;
    return true;
}

bool SnapmakerPrinterAgent::print_uploaded_file(const PrintParams& params, const std::string& filename)
{
    if (m_pending_filament_map.empty())
        return MoonrakerPrinterAgent::print_uploaded_file(params, filename);
    return send_gcode_sync(device_info.dev_id, SnapmakerProtocol::build_start_script(filename, std::exchange(m_pending_filament_map, {})));
}

} // namespace Slic3r
