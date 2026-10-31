#include "CrealityPrintAgent.hpp"
#include "CrealityPrint.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "bambu_networking.hpp"
#include "IPrinterAgent.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"

#include <boost/log/trivial.hpp>
#include "libslic3r/Config.hpp"
#include <exception>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <utility>

using json = nlohmann::json;

namespace Slic3r {

namespace {

constexpr const char* CrealityPrintAgent_VERSION = "0.1.0";

std::string to_lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

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

// Lower-case words of a preset name with the "@scope" suffix dropped:
// "Generic PLA Matte @Creality K2-all" -> {"generic", "pla", "matte"}.
std::vector<std::string> name_words(const std::string& name)
{
    std::vector<std::string> words;
    std::string              word;
    for (char c : name.substr(0, name.find('@'))) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!word.empty()) {
            words.push_back(word);
            word.clear();
        }
    }
    if (!word.empty())
        words.push_back(word);
    return words;
}

} // namespace

// Score visible compatible filament presets against the CFS spool metadata and
// return the best-matching filament_id. Scoring:
//   +20  preset name contains brand_name as a substring
//        (e.g. "Hyper PLA" in "Hyper PLA @Creality K2 0.4 nozzle")
//   +10  the preset belongs to the spool's vendor - by its owning VendorProfile OR by
//        its name. The profile test is what finds the vendor's own generics, which are
//        named "Generic <material> @<scope>" and do not repeat the vendor; the name test
//        still finds a third party filament shipped inside that vendor's bundle, which
//        carries the bundle owner's profile but names its real brand.
//    -5  per word of the preset name the spool never mentioned ("generic" excepted -
//        it marks the unbranded base product rather than a qualifier), so the least
//        specific preset that still explains the spool wins. Without it a spool
//        reporting only "PLA" scores "Generic PLA High Speed" and "Generic PLA Matte"
//        exactly as high as "Generic PLA"; those are three products with three
//        filament_ids, so whichever sorted first won and the printer got the wrong
//        one. Applied after the score gate, so it only reorders genuine matches.
//   Tiebreak: prefer the SYSTEM (shipped) preset over user copies, then by name so the
//   winner never depends on how std::sort leaves equal elements. User copies of generic
//   presets inherit a generic filament_id from their parent, so preferring the user copy
//   can collapse a brand-specific match back to "Generic PLA" via the inherited id.
// Requires the preset's declared filament_type to equal the spool's base type
// (PLA/PETG/ABS/...) so we never auto-pick a PETG preset for a PLA spool.
// Falls back to a visible system base of the same type when nothing scores.
std::string CrealityPrintAgent::match_filament_preset(const PresetCollection& filaments,
                                                      const std::string&      vendor,
                                                      const std::string&      brand_name,
                                                      const std::string&      base_type)
{
    if (base_type.empty()) return {}; // a spool the printer could not name has no preset

    const std::string vendor_lower = to_lower(vendor);
    const std::string brand_lower  = to_lower(brand_name);
    const std::string type_lower   = to_lower(base_type);

    // Everything the spool told us about itself, as words. A word in a preset's name
    // that is not in here is a qualifier the spool never claimed.
    std::set<std::string> spool_words{"generic"};
    for (const std::string& src : {brand_lower, vendor_lower, type_lower})
        for (auto& w : name_words(src))
            spool_words.insert(std::move(w));

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
        // Profile OR name - neither alone covers both the vendor's own generics and the
        // third party filaments shipped inside its bundle. See the header comment.
        if (!vendor_lower.empty()
            && ((p.vendor != nullptr && to_lower(p.vendor->name) == vendor_lower)
                || name_lower.find(vendor_lower) != std::string::npos))
            score += 10;

        if (score == 0) continue;

        for (const auto& w : name_words(p.name))
            if (spool_words.count(w) == 0)
                score -= 5;

        matches.push_back({&p, score, !p.is_system && !p.is_default});
    }

    if (matches.empty()) {
        // first_visible_idx_by_type() returns any visible preset when no base of this
        // type exists. Never sync an unrelated material to the CFS.
        const auto&       fallback_preset = filaments.preset(filaments.first_visible_idx_by_type(base_type));
        const std::string fallback        = fallback_preset.filament_id;
        const bool        fallback_ok     = fallback_preset.config.opt_string("filament_type", 0u) == base_type &&
                                    has_visible_base_preset(filaments, fallback);
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: no preset scored for spool {" << vendor << " "
            << brand_name << " (" << base_type << ")} after considering " << considered
            << " presets; falling back to generic preset id \"" << fallback << "\""
            << (fallback_ok ? "" : " (no visible base of this type; returning empty)");
        return fallback_ok ? fallback : std::string();
    }

    std::sort(matches.begin(), matches.end(),
              [](const Match& a, const Match& b) {
                  if (a.score   != b.score)   return a.score > b.score;
                  if (a.is_user != b.is_user) return !a.is_user; // prefer system over user
                  return a.preset->name < b.preset->name;        // keep the winner deterministic
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
//           "vendor": str, "type": str, "name": str, // all "" for an unknown RFID tag
//           "color": "#dRRGGBB" }, ...
//       ]}, ...
//   ]}}
bool CrealityPrintAgent::parse_cfs_response(const std::string&    response,
                                            std::vector<CFSSlot>& slots,
                                            int&                  box_count,
                                            std::string&          error)
{

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
        const int box_st   = safe_json_int(box, "state");
        const int box_type = safe_json_int(box, "type");
        if (box_st != 1)   continue; // inactive boxes
        if (box_type != 0) continue; // non-CFS (external spool holder, handled separately by upload dialog)

        const int cfs_index = cfs_count++;

        if (!box.contains("materials") || !box["materials"].is_array())
            continue;

        for (const auto& mat : box["materials"]) {
            // CFS slot state encoding observed across K2 family firmwares:
            //   * K2 (base) / K2 Pro    : 0 = empty, 1 = loaded.
            //   * K2 Plus (1.1.5.5/CFS 1.4.2 onwards): 0 = empty,
            //                                          1 = loaded AND currently
            //                                              selected as the active
            //                                              spool for printing,
            //                                          2 = loaded but not selected.
            // We treat anything non-zero as loaded. A loaded spool with an unknown RFID
            // tag reports only its colour, so only an entry with no colour either is blank.
            const int s_state = safe_json_int(mat, "state");
            const std::string s_vendor = safe_json_string(mat, "vendor");
            const std::string s_type   = safe_json_string(mat, "type");
            const std::string s_color  = safe_json_string(mat, "color");
            if (s_state == 0)                                          continue; // explicitly empty
            if (s_vendor.empty() && s_type.empty() && s_color.empty()) continue; // blank entry

            CFSSlot s;
            s.box_id        = cfs_index;
            s.slot_id       = safe_json_int(mat, "id");
            s.vendor        = s_vendor;
            s.brand_name    = safe_json_string(mat, "name");
            s.filament_type = s_type;
            s.color_hex     = s_color.empty() ? "#FFFFFF" : s_color;

            // Creality reports colour as "#" + a digit + "RRGGBB". Drop the digit,
            // as Creality Print does, to get a standard "#RRGGBB".
            if (s.color_hex.size() == 8 && s.color_hex[0] == '#')
                s.color_hex = "#" + s.color_hex.substr(2);

            slots.push_back(std::move(s));
        }
    }

    box_count = cfs_count;
    return true;
}

// Each connected box reports under its own "T<n>" key which of its slots feeds the toolhead,
// as the slot's letter, or "None":
//   { "result": { "status": { "box": { "T1": { "state": "connect", "filament": "A" } } } } }
std::optional<int> CrealityPrintAgent::parse_loaded_slot(const std::string& response)
{
    const json resp = json::parse(response, nullptr, false);
    if (resp.is_discarded() || !resp.contains("result") || !resp["result"].contains("status")
        || !resp["result"]["status"].contains("box"))
        return std::nullopt;

    const auto&        box = resp["result"]["status"]["box"];
    std::optional<int> loaded;
    for (int box_num = 1; box_num <= 4; ++box_num) {
        const std::string key = "T" + std::to_string(box_num);
        if (!box.contains(key) || !box[key].is_object() || box[key].value("state", "") != "connect")
            continue;
        loaded = loaded.value_or(-1);
        const std::string filament = box[key].value("filament", "");
        if (filament.size() == 1 && filament[0] >= 'A' && filament[0] <= 'D')
            return (box_num - 1) * 4 + (filament[0] - 'A');
    }
    return loaded;
}

void CrealityPrintAgent::report_loaded_slot(int loaded_slot)
{
    auto*          dev_manager = GUI::wxGetApp().getDeviceManager();
    MachineObject* obj         = dev_manager ? dev_manager->get_my_machine(device_info.dev_id) : nullptr;
    if (obj)
        ExtderSystemParser::ParseLoadedSlot(loaded_slot, obj->GetExtderSystem());
}

void CrealityPrintAgent::on_printer_connected(const std::string& dev_id)
{
    // The device page shows the slots, and Load and Unload need them, without a sync from the sidebar.
    fetch_filament_info_on_main(dev_id);
}

bool CrealityPrintAgent::fetch_filament_info(std::string dev_id, FilamentSyncMode /*sync_mode*/)
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

    // Only a K1-family `box` object is known to take the commands Load, Unload and the slot refresh
    // send, and it alone names the slot feeding the toolhead, whichever way the slots are read.
    const std::optional<int> loaded_slot = host.is_k1_family_board() ? parse_loaded_slot(query_box_object()) : std::nullopt;
    can_command_box                      = loaded_slot.has_value();

    const auto defer_to_base = [&] {
        const bool synced = MoonrakerPrinterAgent::fetch_filament_info(dev_id);
        if (synced && loaded_slot)
            report_loaded_slot(*loaded_slot);
        return synced;
    };

    // Defer to base if this isn't a K-series board with CFS firmware support.
    if (!host.supports_multi_color_print()) {
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: " << host.model_name()
            << " is not CFS-capable, deferring to base Moonraker agent";
        return defer_to_base();
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
        return defer_to_base();
    }

    if (box_count == 0) {
        // No active CFS boxes attached — printer is in direct-spool mode. Let the
        // base agent take over so the user still gets whatever filament info
        // Moonraker exposes.
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: no active CFS boxes, deferring to base agent";
        return defer_to_base();
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
    if (loaded_slot)
        report_loaded_slot(*loaded_slot);
    return true;
}

std::string CrealityPrintAgent::change_filament_gcode(bool load, int ams_id, int slot_id, int nozzle_temp)
{
    // The box's own Tn_extrude_temp in the stock K1 box.cfg, used when the device page knows no
    // temperature for the filaments involved.
    constexpr int default_temp = 220;
    const int     temp         = nozzle_temp > 0 ? nozzle_temp : default_temp;

    // Both cut the filament at a fixed position, so the toolhead has to be homed. T<n> then cuts
    // what is loaded, feeds slot n and flushes it; BOX_QUIT_MATERIAL cuts and pulls the filament
    // back into its box. Slot n counts four per box across the chain. The nozzle goes off at the
    // end, as it does after the printer's own feed: it would otherwise stay hot until the printer
    // idles out.
    std::string gcode = "IF_NEED_HOME\nM109 S" + std::to_string(temp) + "\n";
    gcode += load ? "T" + std::to_string(ams_id * 4 + slot_id) : std::string("BOX_QUIT_MATERIAL");
    return gcode + "\nM104 S0";
}

std::string CrealityPrintAgent::refresh_slot_gcode(int ams_id, int slot_id)
{
    // ADDR counts boxes from 1; NUM is a bit mask of the slots, A being 1.
    return "BOX_INFO_REFRESH ADDR=" + std::to_string(ams_id + 1) + " NUM=" + std::to_string(1 << slot_id);
}

int CrealityPrintAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    const json j = json::parse(json_str, nullptr, false);
    if (!can_command_box || j.is_discarded() || !j.contains("print") || j["print"].value("command", "") != "ams_change_filament")
        return MoonrakerPrinterAgent::send_message_to_printer(std::move(dev_id), std::move(json_str), qos, flag);

    const auto& cmd     = j["print"];
    const int   ams_id  = cmd.value("ams_id", -1);
    const int   slot_id = cmd.value("slot_id", -1);
    // An unload marks itself with slot 255; the external spool and other virtual trays sit above
    // the 16 boxes' ids and have no box to load them.
    const bool  load    = slot_id != 255;
    if (load && (ams_id < 0 || ams_id >= 16 || slot_id < 0 || slot_id > 3))
        return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;

    const int         temp  = std::max(cmd.value("curr_temp", -1), cmd.value("tar_temp", -1));
    const std::string gcode = change_filament_gcode(load, ams_id, slot_id, temp);
    BOOST_LOG_TRIVIAL(info) << "CrealityPrintAgent: " << (load ? "loading" : "unloading") << " the CFS: " << gcode;

    // Heating, cutting and flushing take minutes; the slots are read back once the box is done.
    send_gcode_async(dev_id, gcode, [this, dev_id](bool success) {
        if (!success)
            BOOST_LOG_TRIVIAL(warning) << "CrealityPrintAgent: the CFS did not finish changing filament";
        fetch_filament_info_on_main(dev_id);
    }, 600);
    return BAMBU_NETWORK_SUCCESS;
}

int CrealityPrintAgent::command_ams_refresh_rfid(std::string dev_id, int ams_id, int slot_id, int sequence_id, bool lan_mode)
{
    // The device page's older form names no box and counts the slot across every box.
    if (ams_id < 0 && slot_id >= 0) {
        ams_id  = slot_id / 4;
        slot_id = slot_id % 4;
    }
    if (!can_command_box || ams_id < 0 || slot_id < 0 || slot_id > 3)
        return MoonrakerPrinterAgent::command_ams_refresh_rfid(std::move(dev_id), ams_id, slot_id, sequence_id, lan_mode);

    const std::string gcode = refresh_slot_gcode(ams_id, slot_id);
    BOOST_LOG_TRIVIAL(info) << "CrealityPrintAgent: reading a CFS tag: " << gcode;
    send_gcode_async(dev_id, gcode, [this, dev_id](bool success) {
        if (!success)
            BOOST_LOG_TRIVIAL(warning) << "CrealityPrintAgent: the CFS did not read the slot's tag again";
        fetch_filament_info_on_main(dev_id);
    }, 120);
    return BAMBU_NETWORK_SUCCESS;
}

} // namespace Slic3r
