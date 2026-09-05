#include "SnapmakerPrinterAgent.hpp"
#include "Http.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <map>
#include <set>

namespace Slic3r {

namespace {

constexpr const char* SNAPMAKER_AGENT_VERSION = "0.0.2";

// Safely access a parallel array by index, returning a fallback if out of bounds.
template<typename T> T safe_at(const std::vector<T>& vec, int index, const T& fallback)
{ return (index >= 0 && index < static_cast<int>(vec.size())) ? vec[index] : fallback; }

// Holds every recognized esthetic/structural filament sub-type and normalizes any raw
// spelling of one (from the AMS's filament_sub_type value, or a token found in a filament
// preset's display name) to its canonical form.
//
// From Snapmaker U1 stock firmware's FILAMENT_PROTO_SUB_TYPE_MAPPING
// (klippy/extras/filament_protocol.py): Matte, SnapSpeed, Silk, Support, HF, 95A,
// 95A HF, 90A, 85A, Wood, Translucent, Full Spectrum. Recognized here so preset names and
// extended/custom RFID data using these words are matched too -- not because matching is
// meant to mirror stock firmware's enum (it deliberately isn't; see
// SnapmakerPrinterAgent.md). "Basic" is intentionally excluded -- it's not a
// distinguishing sub-type (see combine_filament_type()'s comment on brand names).
class SubTypeRegistry
{
public:
    // One recognized sub-type's normalized form and human-readable display suffix.
    struct BaseEntry
    {
        const char* raw;
        const char* normalized;
        const char* english; // e.g. "PLA" + " High Speed" -> "PLA High Speed"
    };

    SubTypeRegistry()
    {
        // Base spellings, upper-cased, as they'd naturally be written out, paired with
        // their normalized form and a human-readable display suffix. Anything with a
        // space in the raw spelling also gets two more lookup variants added below: the
        // space removed ("HIGH SPEED" -> "HIGHSPEED") and the space replaced with a dash
        // ("HIGH SPEED" -> "HIGH-SPEED") -- either of which might show up in a raw
        // filament_sub_type value or a filament preset's display name.
        static const std::vector<BaseEntry> base = {
            {"CF", "CF", " CF"},
            {"GF", "GF", " GF"},
            {"SILK", "SILK", " Silk"},
            {"WOOD", "WOOD", " Wood"},
            {"MATTE", "MATTE", " Matte"},
            {"MARBLE", "MARBLE", " Marble"},
            {"SNAPSPEED", "HS", " High Speed"},
            {"HIGH SPEED", "HS", " High Speed"},
            {"HS", "HS", " High Speed"},
            {"SUPPORT", "SUPPORT", " Support"},
            {"HF", "HF", " HF"},
            {"95A", "95A", " 95A"},
            {"95A HF", "95AHF", " 95A HF"},
            {"90A", "90A", " 90A"},
            {"85A", "85A", " 85A"},
            {"TRANSLUCENT", "TRANSLUCENT", " Translucent"},
            {"FULL SPECTRUM", "FULLSPECTRUM", " Full Spectrum"},
            {"PLUS", "PLUS", " Plus"},
            {"TOUGH", "TOUGH", " Tough"},
        };

        for (const auto& entry : base) {
            const std::string raw = entry.raw;
            m_table.emplace(raw, entry);
            if (raw.find(' ') != std::string::npos) {
                m_multiword_spellings.push_back(raw);

                std::string no_space = raw;
                boost::erase_all(no_space, " ");
                m_table.emplace(no_space, entry);

                std::string dashed = raw;
                boost::replace_all(dashed, " ", "-");
                m_table.emplace(dashed, entry);
            }
        }
    }

    // Normalize a raw sub-type spelling to its canonical form, e.g. "SnapSpeed" -> "HS",
    // "High Speed" -> "HS", "95A-HF" -> "95AHF", "matte" -> "MATTE". Returns "" if raw
    // isn't a recognized sub-type.
    std::string normalize(const std::string& raw) const
    {
        auto it = m_table.find(boost::to_upper_copy(raw));
        return it != m_table.end() ? it->second.normalized : std::string();
    }

    // Human-readable display suffix for an already-normalized sub-type, e.g. "HS" ->
    // " High Speed". Returns "" if normalized isn't a recognized sub-type. Every
    // normalized form is also listed as one of its own raw spellings in base above, so
    // it's guaranteed to be a key in m_table too.
    std::string englishSuffix(const std::string& normalized) const
    {
        auto it = m_table.find(normalized);
        return it != m_table.end() ? it->second.english : std::string();
    }

    // Scrape every recognized sub-type out of free text (typically a filament preset's
    // display name), in normalized form. E.g. "Overture Matte PLA @base" -> {"MATTE"},
    // "Generic PLA High Speed @System" -> {"HS"}.
    //
    // A multi-word spelling (e.g. "High Speed") is collapsed to one word first, since
    // splitting on whitespace would otherwise break it into separate "HIGH"/"SPEED"
    // tokens that don't mean anything alone.
    std::set<std::string> findSubTypes(const std::string& text) const
    {
        std::string upper = boost::to_upper_copy(text);
        for (const std::string& phrase : m_multiword_spellings)
            boost::replace_all(upper, phrase, normalize(phrase));

        std::vector<std::string> tokens;
        boost::split(tokens, upper, boost::is_any_of(" \t-_@"), boost::token_compress_on);

        std::set<std::string> found;
        for (const std::string& token : tokens) {
            std::string normalized = normalize(token);
            if (!normalized.empty())
                found.insert(std::move(normalized));
        }

        return found;
    }

    // A single shared instance -- the table is static data with no per-call variation, so
    // there's no reason to rebuild it on every use.
    static const SubTypeRegistry& instance()
    {
        static const SubTypeRegistry registry;
        return registry;
    }

private:
    // Every recognized raw spelling (including the no-space/dashed variants added in the
    // constructor) mapped to its BaseEntry.
    std::map<std::string, BaseEntry> m_table;
    // The base (space-containing) spellings collected above, e.g. "HIGH SPEED",
    // "95A HF" -- the raw multi-word forms a preset name or filament_sub_type value might
    // spell out with a literal space, distinct from the no-space/dashed variants added to
    // m_table above.
    std::vector<std::string> m_multiword_spellings;
};

// Squared Euclidean distance in RGB space between an already-parsed target color value
// (0xBBGGRR, as unpacked from the printer's RRGGBBAA string) and a preset's own
// default_filament_colour config value (e.g. "#RRGGBB", or empty).
unsigned int color_distance(unsigned int target_color_value, const std::string& preset_color)
{
    unsigned int preset_color_value;
    if (!preset_color.empty()) {
        unsigned int hash_pos = preset_color.find("#");
        preset_color_value    = std::stoul(preset_color.substr(hash_pos != std::string::npos ? hash_pos + 1 : 0), nullptr, 16);
    } else {
        // Default to black if no color specified in profile. Assume other profiles might be a closer color match.
        // Could be a problem if the target color is also black and there exist a specific profile for that type, vendor and color
        // combination.
        preset_color_value = 0;
    }

    int dr = ((target_color_value & 0xff) - (preset_color_value & 0xff));
    int dg = (((target_color_value >> 8) & 0xff) - ((preset_color_value >> 8) & 0xff));
    int db = (((target_color_value >> 16) & 0xff) - ((preset_color_value >> 16) & 0xff));
    return dr * dr + dg * dg + db * db;
}

// Ranking key for a candidate preset. Two buckets fall out of subtype_matches: presets
// whose sub-type situation matches the tray's, and presets whose don't -- is_user_derived
// and distance only ever break ties within one of those two buckets, never across them
// (see is_better()).
struct MatchRank
{
    bool subtype_matches;
    bool is_user_derived;
    unsigned int distance;

    // Is *this* rank better than other? Checked tier by tier, most important first: a
    // sub-type match always outranks a non-match; only when that's tied does user-derived-vs-
    // system decide; only when that's also tied does the closer color win.
    bool is_better(const MatchRank& other) const
    {
        if (subtype_matches != other.subtype_matches)
            return subtype_matches;

        if (is_user_derived != other.is_user_derived)
            return is_user_derived;

        return distance < other.distance;
    }
};

} // anonymous namespace

SnapmakerPrinterAgent::SnapmakerPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

AgentInfo SnapmakerPrinterAgent::get_agent_info_static()
{ return AgentInfo{"snapmaker", "Snapmaker", SNAPMAKER_AGENT_VERSION, "Snapmaker printer agent"}; }

std::string SnapmakerPrinterAgent::combine_filament_type(const std::string& type, const std::string& sub_type)
{
    const std::string base = trim_and_upper(type);
    const std::string sub  = trim_and_upper(sub_type);

    if (base.empty())
        return "PLA";

    if (sub.empty() || sub == "NONE")
        return base;

    const std::string normalized = SubTypeRegistry::instance().normalize(sub);
    // Unrecognized sub-type (brand names like Polylite, Basic, etc.) -- use base type only
    return normalized.empty() ? base : base + SubTypeRegistry::instance().englishSuffix(normalized);
}

// Find the best-matching filament preset for an AMS tray, given the vendor, base
// filament type (e.g. "PLA"), esthetic sub-type (e.g. "MATTE", "HS", possibly empty), and
// color reported by the printer.
//
// Candidates must match vendor and base type exactly (system presets and user-derived
// presets alike -- a user's own tweaked copy of a system preset is a legitimate, often
// more specific, match). Among those, ranked from most to least specific (see MatchRank):
//   1. presets whose name-scraped sub-type situation matches the tray's -- a "MATTE" tray
//      prefers "Overture Matte PLA" over plain "Overture PLA", and a plain (no sub-type)
//      tray prefers plain "Overture PLA" over "Overture Matte PLA"
//   2. user-derived presets (a customized copy of a system preset) over system presets,
//      since a user's own tuning is more likely to reflect what they actually want
//   3. closest color match
bool SnapmakerPrinterAgent::try_vendor_subtype_color_match(const SlotInput& input, AmsTrayData* tray)
{
    auto* bundle = GUI::wxGetApp().preset_bundle;
    if (!bundle)
        return false;

    const Preset* best_match = nullptr;
    MatchRank best_rank{};

    // The printer returns RGBA in the format RRGGBBAA, but profiles store color as #RRGGBB,
    // so we must remove # and ignore alpha channel for distance calculation
    unsigned int target_color_value = std::stoul(input.color.substr(0, input.color.length() - 2), nullptr, 16);

    for (const auto& p : bundle->filaments.get_presets()) {
        if (!p.is_visible || !p.is_compatible ||
            !boost::iequals(p.config.opt_string("filament_vendor", 0u), input.vendor) ||
            !boost::iequals(p.config.opt_string("filament_type", 0u), input.base_type)) {
            continue;
        }

        // Does this preset's sub-type situation match the tray's? If the tray has a
        // sub-type, the preset must scrape to include it; if the tray has none, the
        // preset must scrape to no sub-type either -- a plain "Overture PLA" tray should
        // prefer plain "Overture PLA" over "Overture Matte PLA", not tie with it.
        // TODO - Some day we might have multiple sub-types per tray.
        std::set<std::string> preset_sub_types = SubTypeRegistry::instance().findSubTypes(p.name);
        bool subtype_matches = input.tray_sub_type.empty() ? preset_sub_types.empty()
                                                             : preset_sub_types.count(input.tray_sub_type) != 0;

        const bool is_user_derived  = !p.is_system;
        const unsigned int distance = color_distance(target_color_value, p.config.opt_string("default_filament_colour", 0u));

        MatchRank rank{subtype_matches, is_user_derived, distance};
        if (!best_match || rank.is_better(best_rank)) {
            best_match = &p;
            best_rank  = rank;
        }
    }

    if (!best_match || best_match->filament_id.empty())
        return false;

    BOOST_LOG_TRIVIAL(warning) << "Filament sync: Found manufacturer-specific profile for slot " << input.slot_index << ": "
                               << best_match->filament_id;

    tray->tray_type     = best_match->name;
    tray->tray_color    = input.color;
    tray->tray_info_idx = best_match->filament_id;
    tray->bed_temp      = input.bed_temp;
    tray->nozzle_temp   = input.nozzle_temp;
    return true;
}

bool SnapmakerPrinterAgent::try_type_only_match(const SlotInput& input, AmsTrayData* tray)
{
    auto* bundle = GUI::wxGetApp().preset_bundle;
    if (!bundle)
        return false;

    std::string filament_id = bundle->filaments.filament_id_by_type(input.display_type);
    if (filament_id.empty())
        return false;

    tray->tray_type     = input.display_type;
    tray->tray_color    = input.color;
    tray->tray_info_idx = filament_id;
    tray->bed_temp      = input.bed_temp;
    tray->nozzle_temp   = input.nozzle_temp;
    return true;
}

void SnapmakerPrinterAgent::apply_generic_type_map(const SlotInput& input, AmsTrayData* tray)
{
    tray->tray_type     = input.display_type;
    tray->tray_color    = input.color;
    tray->tray_info_idx = map_filament_type_to_generic_id(input.display_type);
    tray->bed_temp      = input.bed_temp;
    tray->nozzle_temp   = input.nozzle_temp;
}

bool SnapmakerPrinterAgent::fetch_filament_info(std::string dev_id)
{
    std::string url = join_url(device_info.base_url, "/printer/objects/query?print_task_config&filament_detect");

    std::string response_body;
    bool success = false;
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
    if (!json.contains("result") || !json["result"].contains("status") || !json["result"]["status"].contains("print_task_config")) {
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
    if (json["result"]["status"].contains("filament_detect") && json["result"]["status"]["filament_detect"].contains("info")) {
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
            const std::string trimmed_type = trim_and_upper(safe_at(filament_type, i, empty_str));
            const std::string raw_sub_type = trim_and_upper(safe_at(filament_sub_type, i, empty_str));

            SlotInput input;
            input.slot_index   = i;
            input.vendor       = safe_at(filament_vendor, i, empty_str);
            input.base_type    = trimmed_type.empty() ? "PLA" : trimmed_type;
            input.display_type = combine_filament_type(safe_at(filament_type, i, empty_str), safe_at(filament_sub_type, i, empty_str));
            input.color        = safe_at(filament_color, i, default_color);
            input.bed_temp     = 0;
            input.nozzle_temp  = 0;

            // Normalize the tray's sub-type the same way preset-name tokens are, so both
            // sides compare on the same vocabulary. Empty means unrecognized -- leave
            // input.tray_sub_type unset.
            input.tray_sub_type = SubTypeRegistry::instance().normalize(raw_sub_type);

            // Extract NFC temperature data if available
            if (nfc_info.is_array() && i < static_cast<int>(nfc_info.size()) && nfc_info[i].is_object()) {
                auto& nfc_slot     = nfc_info[i];
                std::string vendor = nfc_slot.value("VENDOR", "NONE");
                if (vendor != "NONE" && !vendor.empty()) {
                    input.bed_temp    = nfc_slot.value("BED_TEMP", 0);
                    input.nozzle_temp = nfc_slot.value("FIRST_LAYER_TEMP", 0);
                }
            }

            // Try each match strategy in order, from most to least specific. If neither
            // finds a match, fall back to the generic type map, which always succeeds.
            if (!try_vendor_subtype_color_match(input, &tray) && !try_type_only_match(input, &tray))
                apply_generic_type_map(input, &tray);
        }

        trays.emplace_back(std::move(tray));
    }

    build_ams_payload(1, slot_count - 1, trays);
    return true;
}

} // namespace Slic3r
