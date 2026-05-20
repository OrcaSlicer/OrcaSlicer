#include "CrealityPrintAgent.hpp"
#include "CrealityPrint.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>

namespace Slic3r {

namespace {

constexpr const char* CrealityPrintAgent_VERSION = "0.1.0";

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

} // namespace

// Score visible compatible filament presets against the CFS spool metadata and
// return the best-matching filament_id. Scoring:
//   +20  preset name contains brand_name as a substring
//        (e.g. "Hyper PLA" in "Hyper PLA @Creality K2 0.4 nozzle")
//   +10  preset name contains the vendor substring (e.g. "Creality")
//   Tiebreak: prefer the SYSTEM (shipped) preset over user copies. Brand-
//   specific system presets carry their own filament_id; user copies of
//   generic presets inherit a generic filament_id from their parent, so
//   preferring the user copy can collapse a brand-specific match back to
//   "Generic PLA" via the inherited id. Plus: this code targets upstream
//   OrcaSlicer where shipping the user's local tuning would be wrong.
// Requires the preset's declared filament_type to equal the spool's base type
// (PLA/PETG/ABS/...) so we never auto-pick a PETG preset for a PLA spool.
// Falls back to filaments.filament_id_by_type(base_type) when nothing scores.
std::string CrealityPrintAgent::match_filament_preset(const PresetCollection& filaments,
                                                      const std::string&      vendor,
                                                      const std::string&      brand_name,
                                                      const std::string&      base_type)
{
    auto to_lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    const std::string vendor_lower = to_lower(vendor);
    const std::string brand_lower  = to_lower(brand_name);
    const std::string type_lower   = to_lower(base_type);

    struct Match {
        const Preset* preset;
        int           score;
        bool          is_user;
    };
    std::vector<Match> matches;

    int considered = 0;
    for (const auto& p : filaments.get_presets()) {
        if (!p.is_visible || !p.is_compatible) continue;
        // Note: we deliberately do NOT filter on get_preset_base(p) == &p.
        // K2 owners frequently keep tweaked copies of system presets
        // (e.g. "Creality Hyper PLA @K2 (Harky)" with their per-spool PA),
        // which are derived presets — filtering to bases-only would skip
        // exactly the presets users care about most.
        ++considered;

        std::string preset_type;
        if (const auto* ft = p.config.option<ConfigOptionStrings>("filament_type"))
            if (!ft->values.empty()) preset_type = ft->values.front();
        if (to_lower(preset_type) != type_lower) continue;

        const std::string name_lower = to_lower(p.name);
        int score = 0;
        if (!brand_lower.empty() && name_lower.find(brand_lower) != std::string::npos)
            score += 20;
        if (!vendor_lower.empty() && name_lower.find(vendor_lower) != std::string::npos)
            score += 10;

        if (score > 0)
            matches.push_back({&p, score, !p.is_system && !p.is_default});
    }

    if (matches.empty()) {
        const std::string fallback = filaments.filament_id_by_type(base_type);
        const bool        fallback_ok = has_visible_base_preset(filaments, fallback);
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: no preset scored for spool {" << vendor << " "
            << brand_name << " (" << base_type << ")} after considering " << considered
            << " presets; falling back to generic preset id \"" << fallback << "\""
            << (fallback_ok ? "" : " (NOT visible — returning empty)");
        return fallback_ok ? fallback : std::string();
    }

    std::sort(matches.begin(), matches.end(),
              [](const Match& a, const Match& b) {
                  if (a.score   != b.score)   return a.score > b.score;
                  if (a.is_user != b.is_user) return !a.is_user; // prefer system over user
                  return false;
              });

    BOOST_LOG_TRIVIAL(info)
        << "CrealityPrintAgent: matched spool {" << vendor << " " << brand_name
        << " (" << base_type << ")} -> preset \"" << matches.front().preset->name
        << "\" (score=" << matches.front().score
        << ", " << matches.size() << " candidate(s) of " << considered << " considered)";

    return matches.front().preset->filament_id;
}

CrealityPrintAgent::CrealityPrintAgent(std::string log_dir)
    : MoonrakerPrinterAgent(std::move(log_dir))
{
}

AgentInfo CrealityPrintAgent::get_agent_info_static()
{
    return AgentInfo{
        "crealityprint",
        "CrealityPrint",
        CrealityPrintAgent_VERSION,
        "Creality K-series printer agent (CFS-aware filament sync)"
    };
}

std::string CrealityPrintAgent::normalize_filament_type(const std::string& filament_type)
{
    static const std::vector<std::string> bases = {
        "PETG", "PET", "PLA", "ABS", "ASA", "TPU", "PC", "PA", "PVA", "HIPS"
    };
    for (const auto& base : bases) {
        if (filament_type.rfind(base, 0) == 0) return base;
    }
    return filament_type;
}

// Parse the boxsInfo JSON returned by CrealityPrint::query_boxes_info().
// Schema (verified 2026-05-06 against K2 Combo F021 firmware v1.1.260206):
//   { "boxsInfo": { "materialBoxs": [
//     { "id": int, "state": int, "type": int,    // type 0 = CFS, 1 = single-spool external
//       "materials": [
//         { "id": int, "state": int,             // state 1 = loaded
//           "vendor": str, "type": str, "name": str,
//           "color": "#0RRGGBB" }, ...
//       ]}, ...
//   ]}}
bool CrealityPrintAgent::parse_cfs_response(const std::string&    response,
                                            std::vector<CFSSlot>& slots,
                                            int&                  box_count,
                                            std::string&          error)
{
    using nlohmann::json;

    slots.clear();
    box_count = 0;

    if (response.empty()) {
        error = "empty response";
        return false;
    }

    json resp;
    try {
        resp = json::parse(response);
    } catch (const std::exception& e) {
        error = std::string("JSON parse error: ") + e.what();
        return false;
    }

    if (!resp.contains("boxsInfo") || !resp["boxsInfo"].contains("materialBoxs")) {
        error = "invalid schema (missing boxsInfo.materialBoxs)";
        return false;
    }

    // Sequential AMS-style index for accepted CFS boxes. The K2's raw box.id has
    // gaps (id 0 is the external spool holder, type=1, skipped) — using the raw id
    // would publish phantom slots for the gap. Renumber accepted boxes 0,1,2,...
    int cfs_count = 0;
    for (const auto& box : resp["boxsInfo"]["materialBoxs"]) {
        const int box_st   = box.value("state", 0);
        const int box_type = box.value("type",  0);
        if (box_st != 1)   continue; // inactive boxes
        if (box_type != 0) continue; // non-CFS (external spool holder, handled separately by upload dialog)

        const int cfs_index = cfs_count++;

        if (!box.contains("materials") || !box["materials"].is_array())
            continue;

        for (const auto& mat : box["materials"]) {
            if (mat.value("state", 0) != 1) continue; // empty slot

            CFSSlot s;
            s.box_id        = cfs_index;
            s.slot_id       = mat.value("id",     0);
            s.vendor        = mat.value("vendor", "");
            s.brand_name    = mat.value("name",   "");
            s.filament_type = mat.value("type",   "");
            s.color_hex     = mat.value("color",  "#FFFFFF");

            // Creality reports colour as "#0RRGGBB" (8 chars with a leading zero
            // after '#'). Normalise to standard "#RRGGBB".
            if (s.color_hex.size() == 8 && s.color_hex[0] == '#')
                s.color_hex = "#" + s.color_hex.substr(2);

            slots.push_back(std::move(s));
        }
    }

    box_count = cfs_count;
    return true;
}

bool CrealityPrintAgent::fetch_filament_info(std::string dev_id)
{
    if (device_info.dev_ip.empty()) {
        BOOST_LOG_TRIVIAL(warning)
            << "CrealityPrintAgent::fetch_filament_info: no device IP, falling back to base agent";
        return MoonrakerPrinterAgent::fetch_filament_info(std::move(dev_id));
    }

    // Build a CrealityPrint helper so we can use its model detection + WS helpers
    // (added in upstream PR #13291).
    DynamicPrintConfig cfg;
    cfg.set_key_value("print_host",                  new ConfigOptionString("http://" + device_info.dev_ip));
    cfg.set_key_value("print_host_webui",            new ConfigOptionString(""));
    cfg.set_key_value("printhost_cafile",            new ConfigOptionString(""));
    cfg.set_key_value("printhost_port",              new ConfigOptionString(""));
    cfg.set_key_value("printhost_apikey",            new ConfigOptionString(device_info.api_key));
    cfg.set_key_value("printhost_ssl_ignore_revoke", new ConfigOptionBool(false));

    CrealityPrint host(&cfg);

    // Defer to base if this isn't a K-series board with CFS firmware support.
    if (!host.supports_multi_color_print()) {
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: " << host.model_name()
            << " is not CFS-capable, deferring to base Moonraker agent";
        return MoonrakerPrinterAgent::fetch_filament_info(std::move(dev_id));
    }

    BOOST_LOG_TRIVIAL(info)
        << "CrealityPrintAgent: querying CFS slots on " << host.model_name();

    const std::string response = host.query_boxes_info();

    std::vector<CFSSlot> slots;
    int                  box_count = 0;
    std::string          parse_err;
    if (!parse_cfs_response(response, slots, box_count, parse_err)) {
        BOOST_LOG_TRIVIAL(warning)
            << "CrealityPrintAgent: CFS query failed (" << parse_err << "), "
            << "falling back to base agent";
        return MoonrakerPrinterAgent::fetch_filament_info(std::move(dev_id));
    }

    if (box_count == 0) {
        // No active CFS boxes attached — printer is in direct-spool mode. Let the
        // base agent take over so the user still gets whatever filament info
        // Moonraker exposes.
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: no active CFS boxes, deferring to base agent";
        return MoonrakerPrinterAgent::fetch_filament_info(std::move(dev_id));
    }

    BOOST_LOG_TRIVIAL(info)
        << "CrealityPrintAgent: " << box_count << " CFS box(es), "
        << slots.size() << " loaded slot(s)";

    // Index loaded slots by (box, slot) for O(1) lookup as we walk the full
    // box_count * 4 grid, emitting an AmsTrayData entry for each physical slot.
    std::map<std::pair<int, int>, const CFSSlot*> by_position;
    for (const auto& s : slots)
        by_position[{s.box_id, s.slot_id}] = &s;

    auto* bundle = GUI::wxGetApp().preset_bundle;

    const int max_slots = box_count * 4;
    std::vector<AmsTrayData> trays;
    trays.reserve(max_slots);

    for (int box = 0; box < box_count; ++box) {
        for (int idx = 0; idx < 4; ++idx) {
            AmsTrayData tray;
            tray.slot_index = box * 4 + idx;

            auto it = by_position.find({box, idx});
            if (it == by_position.end()) {
                tray.has_filament = false;
                trays.push_back(std::move(tray));
                continue;
            }

            const CFSSlot& s = *it->second;
            tray.has_filament = true;
            tray.tray_type    = normalize_filament_type(s.filament_type);
            tray.tray_color   = s.color_hex;

            if (bundle) {
                tray.tray_info_idx = match_filament_preset(
                    bundle->filaments, s.vendor, s.brand_name, tray.tray_type);
            }

            trays.push_back(std::move(tray));
        }
    }

    build_ams_payload(box_count, max_slots - 1, trays);
    return true;
}

CrealityPrintAgent::CFSAmsListResult CrealityPrintAgent::sync_filaments_into_ams_list(
    const DynamicPrintConfig& printer_cfg,
    PresetBundle*             bundle)
{
    CFSAmsListResult result;
    if (!bundle) {
        result.status = CFSAmsListResult::QueryFailed;
        result.detail = "no preset bundle";
        return result;
    }

    // Build a minimal config for the CrealityPrint helper. We can't pass printer_cfg
    // directly because its option set is wider than CrealityPrint's constructor expects.
    DynamicPrintConfig cfg;
    cfg.set_key_value("print_host",                  new ConfigOptionString(printer_cfg.opt_string("print_host")));
    cfg.set_key_value("print_host_webui",            new ConfigOptionString(printer_cfg.opt_string("print_host_webui")));
    cfg.set_key_value("printhost_cafile",            new ConfigOptionString(printer_cfg.opt_string("printhost_cafile")));
    cfg.set_key_value("printhost_port",              new ConfigOptionString(printer_cfg.opt_string("printhost_port")));
    cfg.set_key_value("printhost_apikey",            new ConfigOptionString(printer_cfg.opt_string("printhost_apikey")));
    cfg.set_key_value("printhost_ssl_ignore_revoke", new ConfigOptionBool(printer_cfg.opt_bool("printhost_ssl_ignore_revoke")));

    CrealityPrint host(&cfg);
    if (!host.supports_multi_color_print()) {
        BOOST_LOG_TRIVIAL(warning)
            << "CrealityPrintAgent::sync_filaments_into_ams_list: " << host.model_name()
            << " is not CFS-capable";
        result.status = CFSAmsListResult::NotCfsCapable;
        return result;
    }

    BOOST_LOG_TRIVIAL(warning)
        << "CrealityPrintAgent::sync_filaments_into_ams_list: querying CFS slots on "
        << host.model_name();

    const std::string response = host.query_boxes_info();

    std::vector<CFSSlot> slots;
    int                  box_count = 0;
    std::string          parse_err;
    if (!parse_cfs_response(response, slots, box_count, parse_err)) {
        BOOST_LOG_TRIVIAL(warning)
            << "CrealityPrintAgent::sync_filaments_into_ams_list: CFS query failed ("
            << parse_err << ")";
        result.status = CFSAmsListResult::QueryFailed;
        result.detail = parse_err;
        return result;
    }

    if (slots.empty()) {
        result.status = CFSAmsListResult::EmptySlots;
        return result;
    }

    BOOST_LOG_TRIVIAL(warning)
        << "CrealityPrintAgent::sync_filaments_into_ams_list: " << box_count
        << " CFS box(es), " << slots.size() << " loaded slot(s)";
    result.box_count        = box_count;
    result.loaded_slot_count = static_cast<int>(slots.size());

    auto& filaments = bundle->filaments;

    // Build filament_ams_list — same shape as Sidebar::build_filament_ams_list produces
    // for BBL printers, consumed by PresetBundle::sync_ams_list.
    // Map key encodes (extruder, ams_id, slot_id) — main extruder uses the 0x10000 prefix.
    constexpr int kMainExtruder = 0x10000;
    std::map<int, DynamicPrintConfig> new_filament_ams_list;

    for (const auto& s : slots) {
        const std::string normalized_type = normalize_filament_type(s.filament_type);
        const std::string matched_id      = match_filament_preset(
            filaments, s.vendor, s.brand_name, normalized_type);

        DynamicPrintConfig tray_config;
        tray_config.set_key_value("filament_id",                new ConfigOptionStrings{matched_id});
        tray_config.set_key_value("tag_uid",                    new ConfigOptionStrings{std::string()});
        tray_config.set_key_value("ams_id",                     new ConfigOptionStrings{std::to_string(s.box_id)});
        tray_config.set_key_value("slot_id",                    new ConfigOptionStrings{std::to_string(s.slot_id)});
        tray_config.set_key_value("filament_type",              new ConfigOptionStrings{normalized_type});
        const std::string tray_name = std::string(1, char('A' + s.box_id)) + std::to_string(s.slot_id + 1);
        tray_config.set_key_value("tray_name",                  new ConfigOptionStrings{tray_name});
        tray_config.set_key_value("filament_colour",            new ConfigOptionStrings{s.color_hex.empty() ? std::string("#FFFFFF") : s.color_hex});
        tray_config.set_key_value("filament_multi_colour",      new ConfigOptionStrings{});
        tray_config.set_key_value("filament_colour_type",       new ConfigOptionStrings{std::string("0")});
        tray_config.set_key_value("filament_exist",             new ConfigOptionBools{true});
        tray_config.set_key_value("filament_slot_placeholder",  new ConfigOptionBools{false});
        tray_config.set_key_value("filament_is_support",        new ConfigOptionBools{false});

        const int slot_in_filament_array = s.box_id * 4 + s.slot_id;
        const int map_key                = kMainExtruder + slot_in_filament_array;
        new_filament_ams_list.emplace(map_key, std::move(tray_config));

        BOOST_LOG_TRIVIAL(warning)
            << "CrealityPrintAgent::sync_filaments_into_ams_list: slot "
            << slot_in_filament_array << " spool {" << s.vendor << " " << s.brand_name
            << " (" << normalized_type << ")} -> filament_id=\"" << matched_id << "\"";
    }

    bundle->filament_ams_list = new_filament_ams_list;

    std::vector<std::pair<DynamicPrintConfig*, std::string>> unknowns;
    std::map<int, AMSMapInfo>                                empty_maps;
    MergeFilamentInfo                                        merge_info;
    const unsigned int n = bundle->sync_ams_list(unknowns, false /*use_map*/, empty_maps,
                                                 false /*enable_append*/, merge_info,
                                                 false /*color_only*/);

    BOOST_LOG_TRIVIAL(warning)
        << "CrealityPrintAgent::sync_filaments_into_ams_list: PresetBundle::sync_ams_list returned " << n;
    result.applied_filament_count = static_cast<int>(n);
    if (n == 0) {
        result.status = CFSAmsListResult::NoMatches;
        return result;
    }

    result.status = CFSAmsListResult::Success;
    return result;
}
} // namespace Slic3r
