#include "CrealityCfs.hpp"
#include "CrealityPrint.hpp"
#include "Http.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

namespace Slic3r {

namespace {

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

// CFS spool material code -> (vendor, product name, base type). Extracted from
// Creality's on-printer material database
// (/mnt/UDISK/creality/userdata/box/material_database.json; mirrored by the
// community K2-RFID project, db/{k1,k2,hi}.json). The CFS `box` object and the
// MIFARE tag report a filamentId whose last 5 digits are this catalogue id (the
// leading digit is a printer-series prefix). Resolving it to brand + product name
// lets creality_match_filament_preset() pick the exact Orca preset ("Hyper PLA",
// "CR-PLA Matte", ...) instead of only a generic-by-type fallback.
struct CfsCatalogEntry { const char* vendor; const char* name; const char* type; };

const std::map<std::string, CfsCatalogEntry>& cfs_catalog()
{
    static const std::map<std::string, CfsCatalogEntry> tbl = {
        {"00001", {"Generic", "Generic PLA", "PLA"}},
        {"00002", {"Generic", "Generic PLA-Silk", "PLA"}},
        {"00003", {"Generic", "Generic PETG", "PETG"}},
        {"00004", {"Generic", "Generic ABS", "ABS"}},
        {"00005", {"Generic", "Generic TPU", "TPU"}},
        {"00006", {"Generic", "Generic PLA-CF", "PLA-CF"}},
        {"00007", {"Generic", "Generic ASA", "ASA"}},
        {"00008", {"Generic", "Generic PA", "PA"}},
        {"00009", {"Generic", "Generic PA-CF", "PA-CF"}},
        {"00010", {"Generic", "Generic BVOH", "BVOH"}},
        {"00011", {"Generic", "Generic PVA", "PVA"}},
        {"00012", {"Generic", "Generic HIPS", "HIPS"}},
        {"00013", {"Generic", "Generic PET-CF", "PET-CF"}},
        {"00014", {"Generic", "Generic PETG-CF", "PETG-CF"}},
        {"00015", {"Generic", "Generic PA6-CF", "PA6-CF"}},
        {"00016", {"Generic", "Generic PAHT-CF", "PAHT-CF"}},
        {"00017", {"Generic", "Generic PPS", "PPS"}},
        {"00018", {"Generic", "Generic PPS-CF", "PPS-CF"}},
        {"00019", {"Generic", "Generic PP", "PP"}},
        {"00020", {"Generic", "Generic PET", "PET"}},
        {"00021", {"Generic", "Generic PC", "PC"}},
        {"00022", {"Generic", "Generic PA612-CF", "PA-CF"}},
        {"00023", {"Generic", "Generic Support for PA", "PA"}},
        {"00024", {"Generic", "Generic Support for PLA", "PLA"}},
        {"00025", {"Generic", "Generic PA12-CF", "PA-CF"}},
        {"00026", {"Generic", "Generic TPU 64D", "TPU"}},
        {"00027", {"Generic", "Generic PETG-GF", "PETG-GF"}},
        {"00031", {"Generic", "Generic PP-CF", "PP-CF"}},
        {"00032", {"Generic", "Generic PCTG", "PCTG"}},
        {"00033", {"Generic", "Generic ASA-CF", "ASA-CF"}},
        {"00034", {"Generic", "Generic PA6-GF", "PA-GF"}},
        {"00035", {"eSUN", "PLA-LW", "PLA"}},
        {"01001", {"Creality", "Hyper PLA", "PLA"}},
        {"01002", {"Creality", "Hyper L-W PLA", "PLA"}},
        {"01004", {"Creality", "Hyper Stardust", "PLA"}},
        {"01601", {"Creality", "Soleyin Ultra PLA", "PLA"}},
        {"02001", {"Creality", "Hyper PLA-CF", "PLA-CF"}},
        {"03001", {"Creality", "Hyper ABS", "ABS"}},
        {"04001", {"Creality", "CR-PLA", "PLA"}},
        {"05001", {"Creality", "CR-Silk", "PLA"}},
        {"06001", {"Creality", "CR-PETG", "PETG"}},
        {"06002", {"Creality", "Hyper PETG", "PETG"}},
        {"06003", {"Creality", "Hyper PETG-CF", "PETG-CF"}},
        {"07001", {"Creality", "CR-ABS", "ABS"}},
        {"07002", {"Creality", "Hyper PC", "PC"}},
        {"08001", {"Creality", "Ender-PLA", "PLA"}},
        {"09001", {"Creality", "EN-PLA+", "PLA"}},
        {"09002", {"Creality", "ENDER FAST PLA", "PLA"}},
        {"10001", {"Creality", "HP-TPU", "TPU"}},
        {"11001", {"Creality", "CR-Nylon", "PA"}},
        {"12002", {"Creality", "Hyper PPA-CF", "PA-CF"}},
        {"12003", {"Creality", "Hyper PAHT-CF", "PA-CF"}},
        {"12004", {"Creality", "Hyper PA612-CF", "PA612-CF"}},
        {"12005", {"Creality", "Hyper PA6-CF", "PA6-CF"}},
        {"13001", {"Creality", "CR-PLA Carbon", "PLA-CF"}},
        {"14001", {"Creality", "CR-PLA Matte", "PLA"}},
        {"15001", {"Creality", "CR-PLA Fluo", "PLA"}},
        {"16001", {"Creality", "CR-TPU", "TPU"}},
        {"17001", {"Creality", "CR-Wood", "PLA"}},
        {"18001", {"Creality", "HP Ultra PLA", "PLA"}},
        {"19001", {"Creality", "HP-ASA", "ASA"}},
        {"29001", {"Creality", "Hyper Marble", "PLA"}},
        {"E1001", {"eSUN", "PLA+", "PLA"}},
        {"P1001", {"Polymaker", "Panchroma PLA Satin", "PLA"}},
        {"P1002", {"Polymaker", "PolySonic PLA Pro", "PLA"}},
        {"P1003", {"Polymaker", "Panchroma PLA Matte", "PLA"}},
    };
    return tbl;
}

// Keep only hex digits and take the last 6, turning Creality's "0RRGGBB" (or
// "#0RRGGBB") into a plain "RRGGBB" that build_ams_payload's colour normaliser
// accepts.
std::string normalize_color(const std::string& raw)
{
    std::string hex;
    for (char c : raw)
        if (std::isxdigit(static_cast<unsigned char>(c)))
            hex.push_back(c);
    if (hex.size() > 6)
        hex = hex.substr(hex.size() - 6);
    return hex;
}

} // namespace

std::string creality_normalize_filament_type(const std::string& filament_type)
{
    static const std::vector<std::string> bases = {
        "PETG", "PET", "PLA", "ABS", "ASA", "TPU", "PC", "PA", "PVA", "HIPS"
    };
    for (const auto& base : bases) {
        if (filament_type.rfind(base, 0) == 0) return base;
    }
    return filament_type;
}

bool creality_cfs_lookup_material(const std::string& code, std::string& vendor, std::string& name, std::string& type)
{
    const auto& tbl = cfs_catalog();
    auto try_key = [&](const std::string& k) {
        auto it = tbl.find(k);
        if (it == tbl.end())
            return false;
        vendor = it->second.vendor;
        name   = it->second.name;
        type   = it->second.type;
        return true;
    };
    if (try_key(code))
        return true;
    if (code.size() > 5 && try_key(code.substr(code.size() - 5)))
        return true;
    return false;
}

// Scoring:
//   +20  preset name contains brand_name as a substring
//        (e.g. "Hyper PLA" in "Hyper PLA @Creality K2 0.4 nozzle")
//   +10  preset name contains the vendor substring (e.g. "Creality")
//   Tiebreak: prefer SYSTEM (shipped) presets over user copies — brand-specific
//   system presets carry their own filament_id, while user copies of generic
//   presets inherit a generic id and would collapse a brand match back to generic.
// Requires the preset's declared filament_type to equal the spool's base type so
// we never auto-pick a PETG preset for a PLA spool. Falls back to
// filaments.filament_id_by_type(base_type) when nothing scores.
std::string creality_match_filament_preset(const PresetCollection& filaments,
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
        // Deliberately do NOT filter on get_preset_base(p) == &p: owners keep
        // tweaked copies of system presets (derived presets) that we still want.
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
        const std::string fallback    = filaments.filament_id_by_type(base_type);
        const bool        fallback_ok = has_visible_base_preset(filaments, fallback);
        BOOST_LOG_TRIVIAL(info)
            << "CrealityCFS: no preset scored for spool {" << vendor << " "
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
        << "CrealityCFS: matched spool {" << vendor << " " << brand_name
        << " (" << base_type << ")} -> preset \"" << matches.front().preset->name
        << "\" (score=" << matches.front().score
        << ", " << matches.size() << " candidate(s) of " << considered << " considered)";

    return matches.front().preset->filament_id;
}

// ---------------------------------------------------------------------------
// K1 provider — Klipper `box` object over Moonraker
// ---------------------------------------------------------------------------

CrealityCfsK1Provider::CrealityCfsK1Provider(std::string dev_ip, std::string api_key, std::string base_url)
    : m_dev_ip(std::move(dev_ip)), m_api_key(std::move(api_key)), m_base_url(std::move(base_url))
{}

// Schema (verified 2026-06-20 against a K1C, CFS firmware box version 1.1.3):
//   "box": {
//     "same_material": [
//       ["103001","0000000",["T1A"],"ABS"],   // [type_code, "0RRGGBB", [slot labels], type]
//       ["114001","00B359A",["T1B"],"PLA"], ... ] }
// Slot labels encode position: "T<box><slot>" with box 1..4 and slot A..D.
bool CrealityCfsK1Provider::fetch(std::vector<CfsTray>& trays, int& box_count)
{
    trays.clear();
    box_count = 0;

    // Derive the bare host. On the K1C the device IP's port 80 serves Creality's
    // own control API (not Moonraker), so target Moonraker's ports: 7125 (direct)
    // first, then the nginx-proxied web ports (4409/4408), then the recorded
    // base_url. Verified 2026-06-20: K1C answers on :7125 and :4409, 404s on :80.
    std::string host = m_dev_ip;
    if (host.empty() && !m_base_url.empty())
        host = m_base_url;
    // Normalize to a bare host and remember any user-configured port. The print_host
    // may carry an upload port (80 / 4408 / 4409 / 4410 / ...); the Moonraker box may
    // answer on that port or on a known default, so we try the configured port first
    // and then fall back to the usual Moonraker ports. (Leaving the port on the host
    // would build "host:4409:7125", which is invalid.)
    if (auto p = host.find("://"); p != std::string::npos)
        host = host.substr(p + 3);
    if (auto s = host.find('/'); s != std::string::npos)
        host = host.substr(0, s);
    std::string configured_port;
    if (auto c = host.rfind(':'); c != std::string::npos) {
        configured_port = host.substr(c + 1);
        host            = host.substr(0, c);
    }
    if (host.empty())
        return false;

    std::vector<std::string> candidates;
    if (!configured_port.empty())
        candidates.push_back("http://" + host + ":" + configured_port);
    for (const char* port : {"7125", "4409", "4408", "4410"})
        candidates.push_back("http://" + host + ":" + port);
    if (!m_base_url.empty())
        candidates.push_back(m_base_url);

    auto query_box = [&](const std::string& base) -> std::string {
        std::string body;
        bool        ok   = false;
        auto        http = Http::get(base + "/printer/objects/query?box");
        if (!m_api_key.empty())
            http.header("X-Api-Key", m_api_key);
        http.timeout_connect(3)
            .timeout_max(8)
            .on_complete([&](std::string b, unsigned status) { if (status == 200) { body = std::move(b); ok = true; } })
            .on_error([&](std::string, std::string, unsigned) {})
            .perform_sync();
        return ok ? body : std::string();
    };

    nlohmann::json json;
    bool           found = false;
    for (const auto& base : candidates) {
        std::string body = query_box(base);
        if (body.empty())
            continue;
        json = nlohmann::json::parse(body, nullptr, false, true);
        if (json.is_discarded())
            continue;
        if (json.contains("result") && json["result"].contains("status")
            && json["result"]["status"].contains("box")) {
            BOOST_LOG_TRIVIAL(info) << "CrealityCFS(K1): found Moonraker `box` object at " << base;
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    const auto& box = json["result"]["status"]["box"];
    if (!box.contains("same_material") || !box["same_material"].is_array())
        return false;

    auto* bundle   = GUI::wxGetApp().preset_bundle;
    int   max_box  = 0;

    for (const auto& entry : box["same_material"]) {
        if (!entry.is_array() || entry.size() < 4)
            continue;
        const std::string code      = entry[0].is_string() ? entry[0].get<std::string>() : std::string();
        const std::string color_raw = entry[1].is_string() ? entry[1].get<std::string>() : std::string();
        const auto&       labels    = entry[2];
        const std::string type_str  = entry[3].is_string() ? entry[3].get<std::string>() : std::string();
        if (type_str.empty() || !labels.is_array())
            continue;

        // Resolve the filamentId code to vendor + product name via the catalogue;
        // fall back to the box's own type string when the code is unknown.
        std::string cat_vendor, cat_name, cat_type;
        const bool        resolved  = creality_cfs_lookup_material(code, cat_vendor, cat_name, cat_type);
        const std::string base_type = creality_normalize_filament_type(resolved && !cat_type.empty() ? cat_type : type_str);
        const std::string color_hex = normalize_color(color_raw);
        const std::string info_idx  = bundle
            ? creality_match_filament_preset(bundle->filaments, cat_vendor, cat_name, base_type)
            : std::string();

        for (const auto& lbl_json : labels) {
            if (!lbl_json.is_string())
                continue;
            const std::string lbl = lbl_json.get<std::string>(); // e.g. "T1A"
            if (lbl.size() < 3 || std::toupper(static_cast<unsigned char>(lbl[0])) != 'T')
                continue;

            const char slot_ch = static_cast<char>(std::toupper(static_cast<unsigned char>(lbl.back())));
            if (slot_ch < 'A' || slot_ch > 'Z')
                continue;
            const int slot = slot_ch - 'A';

            int box_num = 0;
            try {
                box_num = std::stoi(lbl.substr(1, lbl.size() - 2));
            } catch (...) {
                continue;
            }
            if (box_num < 1)
                continue;

            // Remaining percentage lives in the per-box T<n>.remain_len array.
            int               remain = -1;
            const std::string tkey   = "T" + std::to_string(box_num);
            if (box.contains(tkey) && box[tkey].contains("remain_len") && box[tkey]["remain_len"].is_array()) {
                const auto& rl = box[tkey]["remain_len"];
                if (slot >= 0 && slot < (int) rl.size() && rl[slot].is_string()) {
                    try { remain = std::stoi(rl[slot].get<std::string>()); } catch (...) {}
                }
            }

            CfsTray tray;
            tray.slot_index    = (box_num - 1) * 4 + slot;
            tray.has_filament  = true;
            tray.tray_type     = base_type;
            tray.tray_color    = color_hex;
            tray.tray_info_idx = info_idx;
            tray.remain        = remain;
            trays.push_back(std::move(tray));

            max_box = std::max(max_box, box_num);
        }
    }

    box_count = max_box;
    return !trays.empty();
}

// ---------------------------------------------------------------------------
// K2 provider — proprietary port-9999 WebSocket boxsInfo
// ---------------------------------------------------------------------------

CrealityCfsK2Provider::CrealityCfsK2Provider(std::string dev_ip, std::string api_key)
    : m_dev_ip(std::move(dev_ip)), m_api_key(std::move(api_key))
{}

// Schema (verified 2026-05-06 against K2 Combo F021 firmware v1.1.260206):
//   { "boxsInfo": { "materialBoxs": [
//     { "id": int, "state": int, "type": int,    // type 0 = CFS, 1 = external spool
//       "materials": [ { "id": int, "state": int, "vendor": str, "type": str,
//                        "name": str, "color": "#0RRGGBB" }, ... ] }, ... ] } }
bool CrealityCfsK2Provider::fetch(std::vector<CfsTray>& trays, int& box_count)
{
    trays.clear();
    box_count = 0;

    if (m_dev_ip.empty())
        return false;

    // Reuse CrealityPrint's model detection + WS helpers (upstream PR #13291).
    DynamicPrintConfig cfg;
    cfg.set_key_value("print_host",                  new ConfigOptionString("http://" + m_dev_ip));
    cfg.set_key_value("print_host_webui",            new ConfigOptionString(""));
    cfg.set_key_value("printhost_cafile",            new ConfigOptionString(""));
    cfg.set_key_value("printhost_port",              new ConfigOptionString(""));
    cfg.set_key_value("printhost_apikey",            new ConfigOptionString(m_api_key));
    cfg.set_key_value("printhost_ssl_ignore_revoke", new ConfigOptionBool(false));

    CrealityPrint host(&cfg);
    BOOST_LOG_TRIVIAL(info) << "CrealityCFS(K2): probing boxsInfo on " << host.model_name();

    const std::string response = host.query_boxes_info();
    if (response.empty())
        return false;

    nlohmann::json resp = nlohmann::json::parse(response, nullptr, false, true);
    if (resp.is_discarded()
        || !resp.contains("boxsInfo")
        || !resp["boxsInfo"].contains("materialBoxs"))
        return false;

    auto* bundle = GUI::wxGetApp().preset_bundle;

    // Sequential AMS-style index for accepted CFS boxes. The K2's raw box.id has
    // gaps (id 0 = external spool holder, type 1, skipped) — renumber 0,1,2,...
    int cfs_count = 0;
    for (const auto& box : resp["boxsInfo"]["materialBoxs"]) {
        if (box.value("state", 0) != 1) continue; // inactive box
        if (box.value("type",  0) != 0) continue; // non-CFS (external spool holder)

        const int cfs_index = cfs_count++;
        if (!box.contains("materials") || !box["materials"].is_array())
            continue;

        for (const auto& mat : box["materials"]) {
            // Slot state across K2 firmwares: 0 = empty; non-zero = loaded
            // (K2 Plus uses 1 = loaded+selected, 2 = loaded). Also skip blank
            // entries (no vendor and no type) regardless of state.
            const int         s_state  = mat.value("state",  0);
            const std::string s_vendor = mat.value("vendor", std::string());
            const std::string s_type   = mat.value("type",   std::string());
            if (s_state == 0)                       continue;
            if (s_vendor.empty() && s_type.empty()) continue;

            const int         slot_id   = mat.value("id", 0);
            const std::string brand     = mat.value("name", std::string());
            const std::string base_type = creality_normalize_filament_type(s_type);
            const std::string color_hex = normalize_color(mat.value("color", std::string("FFFFFF")));
            const std::string info_idx  = bundle
                ? creality_match_filament_preset(bundle->filaments, s_vendor, brand, base_type)
                : std::string();

            CfsTray tray;
            tray.slot_index    = cfs_index * 4 + slot_id;
            tray.has_filament  = true;
            tray.tray_type     = base_type;
            tray.tray_color    = color_hex;
            tray.tray_info_idx = info_idx;
            tray.remain        = mat.value("percent", -1);
            trays.push_back(std::move(tray));
        }
    }

    box_count = cfs_count;
    return box_count > 0 && !trays.empty();
}

} // namespace Slic3r
