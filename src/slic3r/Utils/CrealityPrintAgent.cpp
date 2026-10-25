#include "CrealityPrintAgent.hpp"
#include "CrealityPrint.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "IPrinterAgent.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <boost/log/trivial.hpp>
#include "libslic3r/Config.hpp"
#include <exception>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
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

// A CrealityPrint helper pointed at the agent's printer, for its model detection and
// port-9999 WebSocket requests.
std::unique_ptr<CrealityPrint> make_cfs_host(const std::string& dev_ip, const std::string& api_key)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("print_host",                  new ConfigOptionString("http://" + dev_ip));
    cfg.set_key_value("print_host_webui",            new ConfigOptionString(""));
    cfg.set_key_value("printhost_cafile",            new ConfigOptionString(""));
    cfg.set_key_value("printhost_port",              new ConfigOptionString(""));
    cfg.set_key_value("printhost_apikey",            new ConfigOptionString(api_key));
    cfg.set_key_value("printhost_ssl_ignore_revoke", new ConfigOptionBool(false));
    return std::make_unique<CrealityPrint>(&cfg);
}

std::string json_string(const json& obj, const char* key)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Numbers arrive as JSON numbers or as numeric strings, depending on the command and firmware.
double json_number(const json& obj, const char* key, double fallback = 0.)
{
    const auto it = obj.find(key);
    if (it == obj.end())
        return fallback;
    if (it->is_number())
        return it->get<double>();
    if (it->is_string()) {
        const auto& text  = it->get_ref<const std::string&>();
        char*       end   = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        return end != text.c_str() ? value : fallback;
    }
    return fallback;
}

// "RRGGBBAA" (Orca's AMS spelling), "#RRGGBB" or "#0RRGGBB" (Creality's) -> lower-case "rrggbb",
// or "" when unreadable.
std::string color_rgb(std::string color)
{
    if (!color.empty() && color[0] == '#')
        color.erase(0, 1);
    if (color.size() == 7 && color[0] == '0')
        color.erase(0, 1);
    if (color.size() < 6)
        return {};
    color.resize(6);
    for (auto& c : color) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return {};
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return color;
}

bool is_ams_filament_setting(const json& msg)
{
    return msg.is_object() && msg.contains("print") && msg["print"].is_object()
        && json_string(msg["print"], "command") == "ams_filament_setting";
}

// The filament preset a Device tab slot edit refers to, by its filament_id.
const Preset* find_filament_preset(const PresetCollection& filaments, const std::string& filament_id)
{
    const Preset* found = nullptr;
    for (const auto& p : filaments.get_presets()) {
        if (p.filament_id != filament_id || p.is_default)
            continue;
        if (p.is_visible && p.is_compatible)
            return &p;
        if (found == nullptr)
            found = &p;
    }
    return found;
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
// Falls back to filaments.filament_id_by_type(base_type) when nothing scores.
std::string CrealityPrintAgent::match_filament_preset(const PresetCollection& filaments,
                                                      const std::string&      vendor,
                                                      const std::string&      brand_name,
                                                      const std::string&      base_type)
{
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
//           "vendor": str, "type": str, "name": str,
//           "color": "#0RRGGBB" }, ...
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
        const int box_st   = box.value("state", 0);
        const int box_type = box.value("type",  0);
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
            // We treat anything non-zero as loaded. Belt-and-braces: also skip
            // entries that look blank (no vendor and no type) regardless of state.
            const int s_state = mat.value("state", 0);
            const std::string s_vendor = mat.value("vendor", std::string());
            const std::string s_type   = mat.value("type",   std::string());
            if (s_state == 0)                       continue; // explicitly empty
            if (s_vendor.empty() && s_type.empty()) continue; // blank entry — likely empty under a different state encoding

            CFSSlot s;
            s.box_id        = cfs_index;
            s.slot_id       = mat.value("id",     0);
            s.vendor        = s_vendor;
            s.brand_name    = mat.value("name",   "");
            s.filament_type = s_type;
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

bool CrealityPrintAgent::fetch_filament_info(std::string dev_id, FilamentSyncMode /*sync_mode*/)
{
    if (device_info.dev_ip.empty()) {
        BOOST_LOG_TRIVIAL(warning)
            << "CrealityPrintAgent::fetch_filament_info: no device IP, falling back to base agent";
        return MoonrakerPrinterAgent::fetch_filament_info(std::move(dev_id));
    }

    // Build a CrealityPrint helper so we can use its model detection + WS helpers
    // (added in upstream PR #13291).
    const auto     host_ptr = make_cfs_host(device_info.dev_ip, device_info.api_key);
    CrealityPrint& host     = *host_ptr;

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

    publish_cfs_slots(slots, box_count);
    return true;
}

void CrealityPrintAgent::publish_cfs_slots(const std::vector<CFSSlot>& slots, int box_count)
{
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
}

// Walks boxsInfo exactly like parse_cfs_response() (active type-0 boxes, in order), but returns
// the slot record whatever its state: a write to a slot without a spool is still a write.
bool CrealityPrintAgent::find_cfs_material(const std::string& response,
                                           int                cfs_index,
                                           int                slot_id,
                                           int&               box_id,
                                           json&              material)
{
    const json resp = json::parse(response, nullptr, false);
    if (resp.is_discarded() || !resp.contains("boxsInfo") || !resp["boxsInfo"].contains("materialBoxs"))
        return false;

    int index = 0;
    for (const auto& box : resp["boxsInfo"]["materialBoxs"]) {
        if (box.value("state", 0) != 1 || box.value("type", 0) != 0)
            continue;
        if (index++ != cfs_index)
            continue;
        if (!box.contains("materials") || !box["materials"].is_array())
            return false;
        for (const auto& mat : box["materials"]) {
            if (mat.value("id", -1) == slot_id) {
                box_id   = box.value("id", 0);
                material = mat;
                return true;
            }
        }
        return false;
    }
    return false;
}

// The retMaterials fields used here (numbers may also come as numeric strings):
//   { "base": { "id": "00001", "brand": "Generic", "meterialType": "PLA", "name": "Generic PLA",
//               "minTemp": 190, "maxTemp": 240, ... },
//     "kvParam": { "pressure_advance": 0.04, ... } }
// "meterialType" is the firmware's spelling; "materialType" is accepted as well.
bool CrealityPrintAgent::apply_material_library(const std::string& response, CFSMaterial& material)
{
    const json resp = json::parse(response, nullptr, false);
    if (resp.is_discarded() || !resp.contains("retMaterials") || !resp["retMaterials"].is_array())
        return false;

    const std::string type_lower = to_lower(material.filament_type);
    const std::string base_lower = to_lower(normalize_filament_type(material.filament_type));
    const std::string name_lower = to_lower(material.name);

    auto entry_type = [](const json& base) {
        std::string t = json_string(base, "meterialType");
        return t.empty() ? json_string(base, "materialType") : t;
    };

    // 1: same name, 2: one name contains the other (closest length wins), 3: the generic entry
    // of the same type, exact type before base type.
    const json* best      = nullptr;
    int         best_rank = 4;
    size_t      best_diff = std::string::npos;
    for (const auto& entry : resp["retMaterials"]) {
        if (!entry.is_object() || !entry.contains("base") || !entry["base"].is_object())
            continue;
        const json&       base      = entry["base"];
        const std::string etype     = to_lower(entry_type(base));
        const std::string ename     = to_lower(json_string(base, "name"));
        const bool        same_type = etype == type_lower;
        const bool        same_base = same_type || to_lower(normalize_filament_type(entry_type(base))) == base_lower;
        if (ename.empty() || !same_base)
            continue;

        int    rank = 4;
        size_t diff = 0;
        if (ename == name_lower) {
            rank = 1;
        } else if (!name_lower.empty()
                   && (ename.find(name_lower) != std::string::npos || name_lower.find(ename) != std::string::npos)) {
            rank = 2;
            diff = ename.size() > name_lower.size() ? ename.size() - name_lower.size() : name_lower.size() - ename.size();
        } else if (to_lower(json_string(base, "brand")) == "generic") {
            rank = 3;
            diff = same_type ? 0 : 1;
        }
        if (rank < best_rank || (rank == best_rank && diff < best_diff)) {
            best      = &entry;
            best_rank = rank;
            best_diff = diff;
        }
    }
    if (best == nullptr)
        return false;

    const json& base = (*best)["base"];
    material.rfid          = json_string(base, "id");
    material.vendor        = json_string(base, "brand");
    material.filament_type = entry_type(base);
    material.name          = json_string(base, "name");
    if (material.min_temp == 0)
        material.min_temp = static_cast<int>(json_number(base, "minTemp"));
    if (material.max_temp == 0)
        material.max_temp = static_cast<int>(json_number(base, "maxTemp"));
    if (material.pressure_advance == 0. && best->contains("kvParam") && (*best)["kvParam"].is_object())
        material.pressure_advance = json_number((*best)["kvParam"], "pressure_advance");
    return true;
}

json CrealityPrintAgent::build_modify_material(int box_id, int slot_id, const CFSMaterial& material)
{
    if (material.is_clear())
        return {{"boxId", box_id}, {"id", slot_id}, {"rfid", ""}, {"type", ""}, {"vendor", ""}, {"name", ""},
                {"color", ""}, {"minTemp", 0}, {"maxTemp", 0}, {"pressure", 0}};

    // The printer stores colours as "#0rrggbb". Creality Print sends the temperatures as
    // floats (it adds 1e-8); that is mirrored so the firmware gets what its own client sends.
    const std::string rgb = color_rgb(material.color_hex);
    return {{"boxId", box_id},
            {"id", slot_id},
            {"rfid", material.rfid},
            {"type", material.filament_type},
            {"vendor", material.vendor},
            {"name", material.name},
            {"color", rgb.empty() ? std::string() : "#0" + rgb},
            {"minTemp", material.min_temp + 1e-8},
            {"maxTemp", material.max_temp + 1e-8},
            {"pressure", material.pressure_advance}};
}

bool CrealityPrintAgent::material_matches(const json& slot, const CFSMaterial& material)
{
    const std::string vendor = json_string(slot, "vendor");
    const std::string type   = json_string(slot, "type");
    const std::string name   = json_string(slot, "name");
    if (material.is_clear())
        return slot.value("state", 0) == 0 || (vendor.empty() && type.empty() && name.empty());

    if (to_lower(type) != to_lower(material.filament_type) || to_lower(name) != to_lower(material.name))
        return false;
    if (!material.vendor.empty() && to_lower(vendor) != to_lower(material.vendor))
        return false;
    const std::string rgb = color_rgb(material.color_hex);
    return rgb.empty() || color_rgb(json_string(slot, "color")) == rgb;
}

int CrealityPrintAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    const json msg = json::parse(json_str, nullptr, false);
    if (is_ams_filament_setting(msg))
        return set_cfs_slot(msg["print"]);
    return MoonrakerPrinterAgent::send_message(std::move(dev_id), std::move(json_str), qos, flag);
}

int CrealityPrintAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    const json msg = json::parse(json_str, nullptr, false);
    if (is_ams_filament_setting(msg))
        return set_cfs_slot(msg["print"]);
    return MoonrakerPrinterAgent::send_message_to_printer(std::move(dev_id), std::move(json_str), qos, flag);
}

// Device tab slot edit -> modifyMaterial. Runs on the caller's thread and blocks for the
// write and its read-back, like fetch_filament_info() does for the read.
int CrealityPrintAgent::set_cfs_slot(const json& cmd)
{
    const int cfs_index = static_cast<int>(json_number(cmd, "ams_id", -1.));
    const int slot_id   = static_cast<int>(json_number(cmd, "slot_id", json_number(cmd, "tray_id", -1.)));
    if (device_info.dev_ip.empty() || cfs_index < 0 || slot_id < 0) {
        BOOST_LOG_TRIVIAL(warning) << "CrealityPrintAgent: ams_filament_setting without a printer or slot: " << cmd.dump();
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    const auto host = make_cfs_host(device_info.dev_ip, device_info.api_key);
    // Only the K2 family is known to take modifyMaterial; K1-family CFS and other models
    // keep reporting that slot writes are not available.
    if (!host->supports_cfs_write())
        return ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE;

    // The external spool holder (virtual tray) is not published as a CFS slot, so it has no
    // CFS address either.
    int  box_id = 0;
    json current;
    if (!find_cfs_material(host->query_boxes_info(), cfs_index, slot_id, box_id, current)) {
        BOOST_LOG_TRIVIAL(warning) << "CrealityPrintAgent: no CFS slot " << cfs_index << "/" << slot_id;
        return ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE;
    }

    // The Device tab's Reset sends neither a filament id nor a type: that clears the slot.
    CFSMaterial       material;
    const std::string filament_id = json_string(cmd, "tray_info_idx");
    const std::string tray_type   = json_string(cmd, "tray_type");
    if (!filament_id.empty() || !tray_type.empty()) {
        const std::string rgb  = color_rgb(json_string(cmd, "tray_color"));
        material.filament_type = tray_type;
        material.color_hex     = rgb.empty() ? std::string() : "#" + rgb;
        material.min_temp      = static_cast<int>(json_number(cmd, "nozzle_temp_min"));
        material.max_temp      = static_cast<int>(json_number(cmd, "nozzle_temp_max"));

        auto* bundle = GUI::wxGetApp().preset_bundle;
        if (const Preset* preset = bundle ? find_filament_preset(bundle->filaments, filament_id) : nullptr) {
            const auto& config = preset->config;
            if (const auto* vendor = config.option<ConfigOptionStrings>("filament_vendor"); vendor && !vendor->values.empty())
                material.vendor = vendor->values.front();
            if (material.filament_type.empty())
                if (const auto* type = config.option<ConfigOptionStrings>("filament_type"); type && !type->values.empty())
                    material.filament_type = type->values.front();
            if (const auto* pa = config.option<ConfigOptionFloats>("pressure_advance"); pa && !pa->values.empty())
                material.pressure_advance = pa->values.front();
            material.name = preset->name.substr(0, preset->name.find('@'));
            while (!material.name.empty() && material.name.back() == ' ')
                material.name.pop_back();
        }
        if (material.filament_type.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "CrealityPrintAgent: ams_filament_setting without a filament type: " << cmd.dump();
            return BAMBU_NETWORK_ERR_INVALID_RESULT;
        }
        if (material.vendor.empty())
            material.vendor = "Generic";
        if (material.name.empty())
            material.name = material.vendor + " " + material.filament_type;

        // The printer keys a slot by its library id; without the library the preset's own
        // identity is written.
        if (!apply_material_library(host->query_material_library(), material))
            BOOST_LOG_TRIVIAL(info) << "CrealityPrintAgent: no material library entry for " << material.name;

        if (material.min_temp <= 0 || material.max_temp <= 0) {
            BOOST_LOG_TRIVIAL(warning) << "CrealityPrintAgent: refusing to write " << material.name << " without a nozzle temperature range";
            return BAMBU_NETWORK_ERR_INVALID_RESULT;
        }
    }

    if (!host->modify_material(build_modify_material(box_id, slot_id, material)))
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;

    // Verify by reading the slot back; some firmwares apply the change a moment later.
    std::string response;
    bool        applied = false;
    for (int attempt = 0; attempt < 3 && !applied; ++attempt) {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        response = host->query_boxes_info();
        int  unused_box_id = 0;
        json slot;
        applied = find_cfs_material(response, cfs_index, slot_id, unused_box_id, slot) && material_matches(slot, material);
    }

    BOOST_LOG_TRIVIAL(info) << "CrealityPrintAgent: CFS slot " << box_id << "/" << slot_id << " "
                            << (material.is_clear() ? std::string("clear") : material.vendor + " " + material.name + " " + material.color_hex)
                            << (applied ? ": applied" : ": NOT applied");

    // Republish what the printer now reports, so the Device tab shows the real slot either way.
    std::vector<CFSSlot> slots;
    int                  box_count = 0;
    std::string          parse_err;
    if (parse_cfs_response(response, slots, box_count, parse_err) && box_count > 0)
        publish_cfs_slots(slots, box_count);

    if (!applied) {
        GUI::wxGetApp().CallAfter([] {
            GUI::show_error(nullptr, _L("The printer did not apply the CFS slot change. Make sure it is idle and that the "
                                        "slot does not hold an RFID spool, then try again."));
        });
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }
    return BAMBU_NETWORK_SUCCESS;
}

} // namespace Slic3r
