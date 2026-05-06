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
    // Strip subtype suffixes ("PLA Silk", "PLA+", "ABS Pro") to base type so the
    // preset_bundle->filaments.filament_id_by_type() lookup succeeds.
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
                // Fall back to the visible preset that matches by base type. A
                // proper vendor+brand-aware match can be layered on later.
                std::string setting_id = bundle->filaments.filament_id_by_type(tray.tray_type);
                if (!setting_id.empty() && has_visible_base_preset(bundle->filaments, setting_id))
                    tray.tray_info_idx = setting_id;
            }

            trays.push_back(std::move(tray));
        }
    }

    build_ams_payload(box_count, max_slots - 1, trays);
    return true;
}

} // namespace Slic3r
