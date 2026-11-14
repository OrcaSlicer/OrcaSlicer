#include "FilamentInventory.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {

using json = nlohmann::json;

static json slot_to_json(const PhysicalFilament& pf)
{
    json jpf = { {"id", pf.id}, {"color", pf.color}, {"type", pf.type}, {"preset", pf.preset},
                 {"slot", pf.slot}, {"extruder", pf.extruder}, {"virtual_tool", pf.virtual_tool} };
    if (!pf.name.empty())
        jpf["name"] = pf.name;
    if (!pf.unit.empty())
        jpf["unit"] = pf.unit;
    if (!pf.unit_label.empty())
        jpf["unit_label"] = pf.unit_label;
    if (!pf.head.empty())
        jpf["head"] = pf.head;
    return jpf;
}

static PhysicalFilament slot_from_json(const json& jpf)
{
    PhysicalFilament pf;
    auto str = [&jpf](const char* key, std::string& out) {
        if (jpf.contains(key) && jpf[key].is_string())
            out = jpf[key].get<std::string>();
    };
    auto integer = [&jpf](const char* key, int& out) {
        if (jpf.contains(key) && jpf[key].is_number_integer())
            out = jpf[key].get<int>();
    };
    integer("id", pf.id);
    str("color", pf.color);
    str("type", pf.type);
    str("preset", pf.preset);
    str("name", pf.name);
    str("unit", pf.unit);
    str("unit_label", pf.unit_label);
    str("head", pf.head);
    integer("slot", pf.slot);
    integer("extruder", pf.extruder);
    integer("virtual_tool", pf.virtual_tool);
    return pf;
}

static json inventory_to_json(const FilamentInventory& inv)
{
    json jslots = json::array();
    for (const auto& pf : inv.slots)
        jslots.push_back(slot_to_json(pf));
    json j;
    j["version"] = 3;
    j["next_id"] = inv.next_id;
    j["slots"]   = std::move(jslots);
    if (!inv.dialect.empty())
        j["dialect"] = inv.dialect;
    return j;
}

static FilamentInventory inventory_from_json(json j, size_t fallback_slot_count)
{
    FilamentInventory inv;
    if (j.is_object() && j.contains("slots") && j["slots"].is_array()) {
        for (const auto& jpf : j["slots"])
            inv.slots.push_back(jpf.is_object() ? slot_from_json(jpf) : PhysicalFilament{});
    } else if (j.is_object() && j.contains("tools") && j["tools"].is_array()) {
        // Legacy per-tool record: keep each nozzle's loaded row; swap rows are dropped.
        for (const auto& jtool : j["tools"]) {
            PhysicalFilament pf;
            if (jtool.is_array() && !jtool.empty() && jtool[0].is_object())
                pf = slot_from_json(jtool[0]);
            pf.slot     = int(inv.slots.size());
            pf.extruder = int(inv.slots.size());
            inv.slots.push_back(pf);
        }
    } else {
        j = json();
    }
    inv.ensure_slot_count(fallback_slot_count);

    // find() keys on id, so renumber duplicate or missing ids rather than drop the data, and keep
    // next_id above every id in use.
    int max_id = 0;
    for (const auto& pf : inv.slots)
        max_id = std::max(max_id, pf.id);
    int parsed_next_id = (j.is_object() && j.contains("next_id") && j["next_id"].is_number_integer()) ? j["next_id"].get<int>() : 1;
    int next_id = std::max(1, std::max(parsed_next_id, max_id + 1));

    std::unordered_set<int> seen_ids;
    for (auto& pf : inv.slots) {
        if (pf.empty())
            continue;
        if (pf.id > 0 && seen_ids.insert(pf.id).second)
            continue;
        pf.id = next_id++;
        seen_ids.insert(pf.id);
    }
    inv.next_id = next_id;
    if (j.is_object() && j.contains("dialect") && j["dialect"].is_string())
        inv.dialect = j["dialect"].get<std::string>();
    return inv;
}

std::string FilamentInventory::serialize() const { return inventory_to_json(*this).dump(); }

FilamentInventory FilamentInventory::deserialize(const std::string& s, size_t fallback_slot_count)
{
    // A parse failure yields a discarded value, which reads as malformed.
    return inventory_from_json(json::parse(s, nullptr, false), fallback_slot_count);
}

void FilamentInventory::ensure_ids()
{
    int max_id = 0;
    for (const auto& pf : slots)
        max_id = std::max(max_id, pf.id);
    next_id = std::max({1, next_id, max_id + 1});
    for (auto& pf : slots) {
        if (!pf.empty() && pf.id <= 0)
            pf.id = next_id++;
        else if (pf.empty() && pf.id > 0)
            pf.id = 0;
    }
}

void FilamentInventory::ensure_slot_count(size_t n)
{
    while (slots.size() < n) {
        PhysicalFilament pf;
        pf.slot     = int(slots.size());
        pf.extruder = int(slots.size());
        slots.push_back(pf);
    }
}

const PhysicalFilament* FilamentInventory::find(int id) const
{
    if (id <= 0)
        return nullptr;
    for (const auto& pf : slots)
        if (pf.id == id)
            return &pf;
    return nullptr;
}

static std::string trim_lower(const std::string& s) { return boost::algorithm::to_lower_copy(boost::algorithm::trim_copy(s)); }

// CF/GF and other variants are separate types and only match themselves.
static bool same_material_family(const std::string& a, const std::string& b)
{
    static const std::vector<std::vector<const char*>> families = {
        {"pla", "pla+"},
        {"petg", "petg+"},
    };
    for (const auto& family : families) {
        bool has_a = std::find(family.begin(), family.end(), a) != family.end();
        bool has_b = std::find(family.begin(), family.end(), b) != family.end();
        if (has_a && has_b)
            return true;
    }
    return false;
}

bool type_compatible(const std::string& project_type, const std::string& slot_type)
{
    std::string a = trim_lower(project_type);
    std::string b = trim_lower(slot_type);
    if (a.empty() || b.empty())
        return false;
    if (a == b)
        return true;
    return same_material_family(a, b);
}

// Squared RGB distance, alpha ignored. An unparseable color is max distance, so it ranks last.
static bool parse_hex_rgb(const std::string& s, int& r, int& g, int& b)
{
    if ((s.size() != 7 && s.size() != 9) || s[0] != '#')
        return false;
    try {
        r = std::stoi(s.substr(1, 2), nullptr, 16);
        g = std::stoi(s.substr(3, 2), nullptr, 16);
        b = std::stoi(s.substr(5, 2), nullptr, 16);
    } catch (...) {
        return false;
    }
    return true;
}

static float auto_map_color_distance(const std::string& a, const std::string& b)
{
    int ar, ag, ab, br, bg, bb;
    if (!parse_hex_rgb(a, ar, ag, ab) || !parse_hex_rgb(b, br, bg, bb))
        return std::numeric_limits<float>::max();
    float dr = float(ar - br), dg = float(ag - bg), db = float(ab - bb);
    return dr * dr + dg * dg + db * db;
}

// Squared-RGB cutoff beyond which auto-mapping won't propose a slot. Unknown colors are exempt.
static constexpr float kMaxFamilyColorDistance = 10000.f;

// Vendor per the "<Vendor> <Type> <SubType>" naming convention; empty without a preset.
static std::string preset_vendor_token(const std::string& preset)
{
    size_t space = preset.find(' ');
    return trim_lower(space == std::string::npos ? preset : preset.substr(0, space));
}

static bool within_color_cutoff(float dist)
{
    return dist == std::numeric_limits<float>::max() || dist <= kMaxFamilyColorDistance;
}

std::vector<int> auto_map_filaments(const std::vector<ProjectFilamentInfo>& project, const FilamentInventory& inventory)
{
    std::vector<int> result(project.size(), -1);
    for (size_t i = 0; i < project.size(); ++i) {
        const std::string project_vendor = trim_lower(project[i].vendor);
        // Color-nearest non-empty slot that `eligible` accepts; ties keep the lowest slot index unless
        // prefer_vendor lets a vendor match win.
        auto nearest = [&](auto eligible, bool prefer_vendor, float& best_dist) {
            const PhysicalFilament* best        = nullptr;
            bool                    best_vendor = false;
            for (const PhysicalFilament& pf : inventory.slots) {
                if (pf.empty())
                    continue;
                const float dist = auto_map_color_distance(project[i].color, pf.color);
                if (!eligible(pf, dist))
                    continue;
                const bool vendor = prefer_vendor && !project_vendor.empty() && project_vendor == preset_vendor_token(pf.preset);
                if (best == nullptr || dist < best_dist || (dist == best_dist && vendor && !best_vendor)) {
                    best        = &pf;
                    best_dist   = dist;
                    best_vendor = vendor;
                }
            }
            return best;
        };

        float dist = 0.f;
        // Cut off after picking: a same-preset slot of unknown color must not beat a nearer one that is
        // out of range; the filament falls through to the family tier instead.
        if (!project[i].preset.empty()) {
            const PhysicalFilament* pf = nearest([&](const PhysicalFilament& s, float) { return s.preset == project[i].preset; }, false, dist);
            if (pf != nullptr && within_color_cutoff(dist)) {
                result[i] = pf->id;
                continue;
            }
        }
        const PhysicalFilament* pf = nearest(
            [&](const PhysicalFilament& s, float d) { return type_compatible(project[i].type, s.type) && within_color_cutoff(d); }, true, dist);
        if (pf != nullptr)
            result[i] = pf->id;
    }
    return result;
}

std::string derive_filament_subtype(const std::string& display_name, const std::string& vendor, const std::string& type)
{
    std::vector<std::string> tokens;
    boost::algorithm::split(tokens, display_name, boost::algorithm::is_any_of(" "));
    std::string result;
    for (const std::string& token : tokens)
        if (!token.empty() && !boost::algorithm::iequals(token, vendor) && !boost::algorithm::iequals(token, type))
            result += (result.empty() ? "" : " ") + token;
    return result;
}

bool inventory_all_unset(const FilamentInventory& inv)
{
    for (const auto& pf : inv.slots)
        if (!pf.empty())
            return false;
    return true;
}

std::vector<int> compute_physical_map_proposal(
    const std::vector<std::string>& filament_colors,
    const std::vector<std::string>& filament_types,
    const std::vector<std::string>& filament_vendors,
    const std::vector<std::string>& filament_presets,
    const std::vector<int>&         plate_filaments,
    const FilamentInventory&        inventory)
{
    std::vector<ProjectFilamentInfo> project(filament_colors.size());
    for (size_t i = 0; i < project.size(); ++i) {
        project[i].color  = filament_colors[i];
        project[i].type   = i < filament_types.size() ? filament_types[i] : std::string();
        project[i].vendor = i < filament_vendors.size() ? filament_vendors[i] : std::string();
        project[i].preset = i < filament_presets.size() ? filament_presets[i] : std::string();
    }
    const std::vector<int> auto_result = auto_map_filaments(project, inventory);

    std::vector<int> proposal(plate_filaments.size(), 0);
    for (size_t i = 0; i < plate_filaments.size(); ++i) {
        int filament_id = plate_filaments[i] - 1; // stored 1-based, arrays are 0-based
        int matched = (filament_id >= 0 && (size_t) filament_id < auto_result.size()) ? auto_result[filament_id] : -1;
        proposal[i] = matched > 0 ? matched : 0;
    }
    return proposal;
}

std::string FilamentInventories::serialize() const
{
    json jpresets = json::object();
    for (const auto& [preset_name, inv] : by_preset)
        jpresets[preset_name] = inventory_to_json(inv);
    json j;
    j["version"] = 1;
    j["presets"] = std::move(jpresets);
    return j.dump();
}

FilamentInventories FilamentInventories::deserialize(const std::string& s)
{
    FilamentInventories store;

    json j;
    try {
        j = json::parse(s);
    } catch (...) {
        store.parse_error = true;
        BOOST_LOG_TRIVIAL(error) << "FilamentInventories::deserialize: could not parse the stored "
            "inventory blob (" << s.size() << " bytes) as JSON; returning an empty store without "
            "touching the saved blob";
        return store;
    }
    if (!j.is_object()) {
        store.parse_error = true;
        BOOST_LOG_TRIVIAL(error) << "FilamentInventories::deserialize: stored inventory blob's "
            "top level is not a JSON object (is " << j.type_name() << "); returning an empty "
            "store without touching the saved blob";
        return store;
    }

    // Legacy values are always objects, so a legacy preset named "version" can't pass as an envelope.
    const bool  is_envelope = j.contains("version") && j["version"].is_number_integer() &&
                               j.contains("presets") && j["presets"].is_object();
    const json& jpresets    = is_envelope ? j["presets"] : j;

    for (const auto& [preset_name, jinv] : jpresets.items()) {
        if (!jinv.is_object())
            continue;
        store.by_preset[preset_name] = inventory_from_json(jinv, 0);
    }
    return store;
}

FilamentInventory& FilamentInventories::for_preset(const std::string& printer_preset_name, size_t slot_count)
{
    FilamentInventory& inv = by_preset[printer_preset_name];
    inv.ensure_slot_count(slot_count);
    return inv;
}

} // namespace Slic3r
