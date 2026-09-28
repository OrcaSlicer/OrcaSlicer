#include "FilamentInventory.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_set>

#include <boost/log/trivial.hpp>

namespace Slic3r {

using json = nlohmann::json;

static const char* kind_to_string(PhysicalFilament::Kind kind)
{
    return kind == PhysicalFilament::Kind::Mmu ? "mmu" : "manual";
}

static PhysicalFilament::Kind kind_from_string(const std::string& s)
{
    return s == "mmu" ? PhysicalFilament::Kind::Mmu : PhysicalFilament::Kind::Manual;
}

static json slot_to_json(const PhysicalFilament& pf)
{
    json jpf = { {"id", pf.id}, {"color", pf.color}, {"type", pf.type}, {"preset", pf.preset}, {"kind", kind_to_string(pf.kind)},
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
    if (jpf.contains("kind") && jpf["kind"].is_string())
        pf.kind = kind_from_string(jpf["kind"].get<std::string>());
    str("name", pf.name);
    str("unit", pf.unit);
    str("unit_label", pf.unit_label);
    str("head", pf.head);
    integer("slot", pf.slot);
    integer("extruder", pf.extruder);
    integer("virtual_tool", pf.virtual_tool);
    return pf;
}

std::string FilamentInventory::serialize() const
{
    json jslots = json::array();
    for (const auto& pf : slots)
        jslots.push_back(slot_to_json(pf));
    json j;
    j["version"] = 3;
    j["next_id"] = next_id;
    j["slots"]   = std::move(jslots);
    if (!dialect.empty())
        j["dialect"] = dialect;
    return j.dump();
}

FilamentInventory FilamentInventory::deserialize(const std::string& s, size_t fallback_slot_count)
{
    FilamentInventory inv;
    json j;
    try {
        j = json::parse(s);
    } catch (...) {
        j = json();
    }

    if (j.is_object() && j.contains("slots") && j["slots"].is_array()) {
        for (const auto& jpf : j["slots"])
            inv.slots.push_back(jpf.is_object() ? slot_from_json(jpf) : PhysicalFilament{});
    } else if (j.is_object() && j.contains("tools") && j["tools"].is_array()) {
        // The per-tool record: one list per nozzle, the loaded filament first and swap rows
        // after it. A slot is one filament now, so only the loaded row survives, positioned and
        // fed by its tool index (one slot per extruder is what that model described).
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

    // Reconcile ids from untrusted input: find()/slot_index_of() key on id, so a duplicate would
    // make one entry unreachable, and a next_id at or below an id already in use would let the
    // allocator mint a fresh collision the moment it's used. Renumber the colliding or
    // missing/invalid (<=0) id via the allocator rather than dropping the entry -- that preserves
    // the caller's color/type data. Empty slots (nothing recorded) keep whatever id parsed,
    // including 0/absent; they carry no data to collide over.
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
            continue; // first time we've seen this id; keep it
        pf.id = next_id++;
        seen_ids.insert(pf.id);
    }
    inv.next_id = next_id;
    if (j.is_object() && j.contains("dialect") && j["dialect"].is_string())
        inv.dialect = j["dialect"].get<std::string>();
    return inv;
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
            pf.id = 0; // a cleared slot round-trips as canonically empty, not a stale reference
    }
}

void FilamentInventory::ensure_slot_count(size_t n)
{
    while (slots.size() < n) {
        PhysicalFilament pf;
        pf.slot     = int(slots.size());
        pf.extruder = int(slots.size()); // the no-sync fallback: one slot per nozzle
        slots.push_back(pf);
    }
}

void FilamentInventory::apply_synced_slot(size_t slot_idx, const PhysicalFilament& slot)
{
    ensure_slot_count(slot_idx + 1);
    slots[slot_idx] = slot;
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

int FilamentInventory::slot_index_of(int id) const
{
    if (id <= 0)
        return -1;
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i].id == id)
            return static_cast<int>(i);
    return -1;
}

std::vector<std::string> FilamentInventory::units() const
{
    std::vector<std::string> out;
    for (const auto& pf : slots)
        if (!pf.unit.empty() && std::find(out.begin(), out.end(), pf.unit) == out.end())
            out.push_back(pf.unit);
    return out;
}

std::vector<int> FilamentInventory::slots_of_extruder(int extruder) const
{
    std::vector<int> out;
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i].extruder == extruder)
            out.push_back(int(i));
    return out;
}

// --- auto_map_filaments helpers -------------------------------------------------------------

static std::string trim_lower(const std::string& s)
{
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    size_t end = s.find_last_not_of(" \t\r\n");
    std::string out = s.substr(begin, end - begin + 1);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return out;
}

// Table-driven family compatibility, deliberately small and explicit so it is trivial to extend.
// CF/GF/wood/etc variants are their own normalized types (never in a family table entry), so they
// only match themselves exactly -- never a plain base type.
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

// Squared-RGB distance (not a perceptual ΔE), used for the auto-mapper's color matching. Accepts "#RRGGBB" or
// "#RRGGBBAA" (alpha ignored). An unparseable/missing color on either
// side is treated as maximally far so it never outranks a real color match, while still letting a
// type-only candidate win when it is the only compatible one.
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

// A gross color mismatch must not be auto-proposed without confirmation -- the user may still
// choose red-for-blue by hand, but auto-map must not propose it unasked. Colors farther apart
// than this squared-RGB distance disqualify an otherwise family-compatible candidate at the
// family/color tier, and an otherwise exact-preset match at the preset tier (a matching preset
// string alone doesn't prove the spool in front of the user is the one meant when its color is
// nowhere close). A durable assignment is exempt (an explicit user pick, not a derived match), as
// is a slot/filament with no color recorded (never disqualified, only ranked last).
static constexpr float kMaxFamilyColorDistance = 10000.f;

// First whitespace-delimited token of a slot's preset display name, i.e. its vendor per the
// "<Vendor> <Type> <SubType>" convention documented on derive_filament_subtype above. Empty when
// the slot carries no preset (unknown vendor).
static std::string preset_vendor_token(const std::string& preset)
{
    size_t space = preset.find(' ');
    return trim_lower(space == std::string::npos ? preset : preset.substr(0, space));
}

AutoMapResult auto_map_filaments(
    const std::vector<ProjectFilamentInfo>& project,
    const FilamentInventory&                inventory,
    const std::vector<SlotAssignment>&      assignments)
{
    AutoMapResult result;
    result.filament_map.assign(project.size(), 1);   // fallback tool, overwritten below on match
    result.physical_map.assign(project.size(), -1);

    std::map<int, SlotAssignment> assignment_by_filament;
    for (const auto& a : assignments)
        assignment_by_filament[a.filament_idx] = a; // last one wins on a duplicate filament_idx

    for (size_t i = 0; i < project.size(); ++i) {
        auto ait = assignment_by_filament.find((int) i);
        if (ait != assignment_by_filament.end()) {
            const SlotAssignment& a = ait->second;
            if (a.tool >= 0 && (size_t) a.tool < inventory.slots.size() && a.slot == 0 &&
                !inventory.slots[a.tool].empty()) {
                result.filament_map[i] = a.tool + 1;
                result.physical_map[i] = inventory.slots[a.tool].id;
                continue; // durable assignment wins outright, even over a closer color match
            }
            // Stale (the slot no longer exists, was a retired swap row, or has since gone empty
            // -- e.g. a printer sync cleared it): fall through to matching.
        }

        // An exact profile match is trusted over anything derived from type/color -- checked
        // before family/color matching, but only after a durable assignment (which still wins
        // outright even over an exact-preset slot). Among several exact-preset slots (e.g. the
        // same preset loaded in two slots with different spool colors), the nearest color wins --
        // an unknown color on either side is neutral, same tie-break rule as the family/color
        // tier below -- then the lowest slot index.
        if (!project[i].preset.empty()) {
            bool  found_exact = false;
            float exact_dist  = std::numeric_limits<float>::max();
            int   exact_slot  = -1;
            int   exact_id    = 0;
            for (size_t si = 0; si < inventory.slots.size(); ++si) {
                const PhysicalFilament& pf = inventory.slots[si];
                if (pf.empty() || pf.preset.empty() || pf.preset != project[i].preset)
                    continue;
                const float dist = auto_map_color_distance(project[i].color, pf.color);
                // A strict non-improvement keeps the current best, so a full tie naturally keeps
                // the lowest slot index.
                if (!found_exact || dist < exact_dist) {
                    found_exact = true;
                    exact_dist  = dist;
                    exact_slot  = (int) si;
                    exact_id    = pf.id;
                }
            }
            if (found_exact && (exact_dist == std::numeric_limits<float>::max() || exact_dist <= kMaxFamilyColorDistance)) {
                result.filament_map[i] = exact_slot + 1;
                result.physical_map[i] = exact_id;
                continue;
            }
        }

        bool   found      = false;
        float  best_dist  = 0.f;
        bool   best_vendor = false;
        int    best_tool  = -1;
        int    best_id    = 0;
        const std::string project_vendor = trim_lower(project[i].vendor);

        for (size_t si = 0; si < inventory.slots.size(); ++si) {
            const PhysicalFilament& pf = inventory.slots[si];
            if (pf.empty())
                continue;
            if (!type_compatible(project[i].type, pf.type))
                continue;

            float dist = auto_map_color_distance(project[i].color, pf.color);
            // A known, finite distance beyond the cutoff disqualifies the candidate; an
            // unknown color on either side (the color_distance sentinel) still qualifies on
            // type alone, same as before -- it's simply ranked last since it never wins a
            // distance comparison against a real, close color match.
            if (dist != std::numeric_limits<float>::max() && dist > kMaxFamilyColorDistance)
                continue;
            bool vendor_match = !project_vendor.empty() && project_vendor == preset_vendor_token(pf.preset);

            bool better;
            if (!found)
                better = true;
            else if (dist < best_dist)
                better = true;
            else if (dist > best_dist)
                better = false;
            else
                better = vendor_match && !best_vendor; // equal distance: vendor breaks the tie
            // Anything else (a strict non-improvement) keeps the current best -- since slots
            // are scanned in ascending order, a full tie naturally keeps the lowest slot index.

            if (better) {
                found       = true;
                best_dist   = dist;
                best_vendor = vendor_match;
                best_tool   = (int) si;
                best_id     = pf.id;
            }
        }

        if (found) {
            result.filament_map[i] = best_tool + 1;
            result.physical_map[i] = best_id;
        } else {
            result.unmatched.push_back((int) i);
        }
    }

    return result;
}

std::string dump_slot_assignments(const std::map<std::string, std::vector<SlotAssignment>>& by_device)
{
    json j = json::object();
    for (const auto& [device_key, assignments] : by_device) {
        json jassignments = json::array();
        for (const auto& a : assignments)
            jassignments.push_back({ {"filament", a.filament_idx}, {"tool", a.tool}, {"slot", a.slot} });
        j[device_key] = std::move(jassignments);
    }
    return j.dump();
}

std::map<std::string, std::vector<SlotAssignment>> load_slot_assignments(const std::string& s)
{
    std::map<std::string, std::vector<SlotAssignment>> result;

    json j;
    try {
        j = json::parse(s);
    } catch (...) {
        return result;
    }
    if (!j.is_object())
        return result;

    for (const auto& [device_key, jassignments] : j.items()) {
        if (!jassignments.is_array())
            continue;
        std::vector<SlotAssignment> assignments;
        for (const auto& ja : jassignments) {
            if (!ja.is_object())
                continue;
            if (!ja.contains("filament") || !ja["filament"].is_number_integer())
                continue;
            if (!ja.contains("tool") || !ja["tool"].is_number_integer())
                continue;
            if (!ja.contains("slot") || !ja["slot"].is_number_integer())
                continue;
            SlotAssignment a;
            a.filament_idx = ja["filament"].get<int>();
            a.tool         = ja["tool"].get<int>();
            a.slot         = ja["slot"].get<int>();
            if (a.filament_idx < 0 || a.tool < 0 || a.slot < 0)
                continue; // corrupt entry: skip rather than fabricate a plausible-looking one
            assignments.push_back(a);
        }
        result[device_key] = std::move(assignments);
    }
    return result;
}

std::vector<ManualMapViolation> validate_manual_map(
    const std::vector<ProjectFilamentInfo>& project,
    const std::vector<int>&                 filament_map,
    const std::vector<int>&                 physical_map,
    const FilamentInventory&                inventory)
{
    std::vector<ManualMapViolation> violations;

    for (size_t i = 0; i < project.size(); ++i) {
        int physical_id = i < physical_map.size() ? physical_map[i] : 0;
        const PhysicalFilament* target = nullptr;
        if (physical_id > 0) {
            target = inventory.find(physical_id);
        } else {
            int tool = i < filament_map.size() ? filament_map[i] : 0;
            if (tool >= 1 && (size_t) tool <= inventory.slots.size())
                target = &inventory.slots[tool - 1];
        }

        if (target == nullptr || target->empty()) {
            violations.push_back({(int) i, ManualMapViolationKind::EmptyTarget});
            continue; // an empty target has neither type nor color to compare
        }

        if (!project[i].type.empty() && !target->type.empty() && !type_compatible(project[i].type, target->type))
            violations.push_back({(int) i, ManualMapViolationKind::FamilyMismatch});

        float dist = auto_map_color_distance(project[i].color, target->color);
        if (dist != std::numeric_limits<float>::max() && dist > kMaxFamilyColorDistance)
            violations.push_back({(int) i, ManualMapViolationKind::GrossColorMismatch});
    }

    return violations;
}

std::string inventory_confirmation_fingerprint(const FilamentInventory& inventory)
{
    // Plain FNV-1a over a delimited encoding of the confirmed content -- not cryptographic, just
    // needs to be stable within a process/build and sensitive to every field this doc promises.
    // Delimiters (both between fields and between slots) prevent adjacent-field concatenation
    // collisions (e.g. type "A"+color "BC" must not hash the same as type "AB"+color "C").
    static constexpr uint64_t kFnvOffset = 14695981039346656037ull;
    static constexpr uint64_t kFnvPrime  = 1099511628211ull;
    uint64_t h = kFnvOffset;
    auto mix = [&h](const std::string& s) {
        for (unsigned char c : s) {
            h ^= c;
            h *= kFnvPrime;
        }
        h ^= '\x1f'; // field/record delimiter
        h *= kFnvPrime;
    };

    mix(std::to_string(inventory.slots.size()));
    for (const auto& pf : inventory.slots) {
        mix(pf.type);
        mix(pf.color);
        mix(pf.preset);
    }

    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << h;
    return oss.str();
}

std::string dump_manual_map_confirmations(const std::map<std::string, std::map<int, std::string>>& by_device_plate)
{
    json j = json::object();
    for (const auto& [device_key, by_plate] : by_device_plate) {
        json jplates = json::object();
        for (const auto& [plate_index, fingerprint] : by_plate)
            jplates[std::to_string(plate_index)] = fingerprint;
        j[device_key] = std::move(jplates);
    }
    return j.dump();
}

std::map<std::string, std::map<int, std::string>> load_manual_map_confirmations(const std::string& s)
{
    std::map<std::string, std::map<int, std::string>> result;

    json j;
    try {
        j = json::parse(s);
    } catch (...) {
        return result;
    }
    if (!j.is_object())
        return result;

    for (const auto& [device_key, jplates] : j.items()) {
        if (!jplates.is_object())
            continue;
        std::map<int, std::string> by_plate;
        for (const auto& [plate_str, jfingerprint] : jplates.items()) {
            if (!jfingerprint.is_string())
                continue;
            int plate_index = 0;
            try {
                plate_index = std::stoi(plate_str);
            } catch (...) {
                continue; // non-numeric key: corrupt entry, skip rather than fabricate index 0
            }
            by_plate[plate_index] = jfingerprint.get<std::string>();
        }
        if (!by_plate.empty())
            result[device_key] = std::move(by_plate);
    }
    return result;
}

std::string derive_filament_subtype(const std::string& display_name, const std::string& vendor, const std::string& type)
{
    auto lower = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        return v;
    };
    const std::string vendor_l = lower(vendor);
    const std::string type_l   = lower(type);

    std::string result;
    size_t pos = 0;
    while (pos < display_name.size()) {
        size_t space = display_name.find(' ', pos);
        if (space == std::string::npos)
            space = display_name.size();
        std::string token = display_name.substr(pos, space - pos);
        std::string token_l = lower(token);
        if (!token.empty() && token_l != vendor_l && token_l != type_l) {
            if (!result.empty())
                result += ' ';
            result += token;
        }
        pos = space + 1;
    }
    return result;
}

bool inventory_all_unset(const FilamentInventory& inv)
{
    for (const auto& pf : inv.slots)
        if (!pf.empty())
            return false;
    return true;
}

std::vector<ProjectFilamentInfo> build_project_filament_info(
    const std::vector<std::string>& filament_colors,
    const std::vector<std::string>& filament_types,
    const std::vector<std::string>& filament_vendors,
    const std::vector<std::string>& filament_presets)
{
    std::vector<ProjectFilamentInfo> project(filament_colors.size());
    for (size_t i = 0; i < project.size(); ++i) {
        project[i].color  = filament_colors[i];
        project[i].type   = i < filament_types.size() ? filament_types[i] : std::string();
        project[i].vendor = i < filament_vendors.size() ? filament_vendors[i] : std::string();
        project[i].preset = i < filament_presets.size() ? filament_presets[i] : std::string();
    }
    return project;
}

std::vector<int> compute_physical_map_proposal(
    const std::vector<std::string>& filament_colors,
    const std::vector<std::string>& filament_types,
    const std::vector<std::string>& filament_vendors,
    const std::vector<std::string>& filament_presets,
    const std::vector<int>&         plate_filaments,
    const std::vector<int>&         stored_physical_map,
    const std::vector<int>&         stored_filament_map,
    const FilamentInventory&        inventory,
    bool                             stored_map_confirmed)
{
    // Orca: bootstrap (nothing recorded anywhere) has no physical filament to propose -- a
    // confirmed row instead reproposes its previously-stored TOOL, sentinel-encoded as
    // -(tool) so FilamentMapRowsPanel (which derives the same bootstrap/not split from this
    // same inventory) can tell it apart from a real (positive) physical id. This covers a plate
    // confirmed via bootstrap with the "record as loaded" offer left unchecked: its physical map
    // stays all-0 (nothing was ever recorded to point at), so without this fallback every reopen
    // would silently reset every row instead of honoring filament_map.
    if (inventory_all_unset(inventory)) {
        const int tool_count = (int) inventory.slots.size();
        std::vector<int> proposal(plate_filaments.size(), 0);
        for (size_t i = 0; i < plate_filaments.size(); ++i) {
            int f    = plate_filaments[i];
            int tool = (stored_map_confirmed && f >= 1 && f <= (int) stored_filament_map.size()) ? stored_filament_map[f - 1] : 0;
            proposal[i] = (tool >= 1 && tool <= tool_count) ? -tool : 0;
        }
        return proposal;
    }

    // Matching is delegated to auto_map_filaments -- the same core the slice-time auto-mapper
    // uses, so the dialog's proposal and slice-time mapping never disagree. Built through the
    // shared build_project_filament_info (not a second inline copy) so this ProjectFilamentInfo
    // list -- vendor included -- matches slice-time's exactly; no durable SlotAssignments are
    // passed here, since this proposal has its own (richer) stored-map handling below.
    std::vector<ProjectFilamentInfo> project = build_project_filament_info(filament_colors, filament_types, filament_vendors, filament_presets);
    AutoMapResult auto_result = auto_map_filaments(project, inventory, {});

    std::vector<int> proposal(plate_filaments.size(), 0);
    for (size_t i = 0; i < plate_filaments.size(); ++i) {
        int f      = plate_filaments[i];
        int stored = (stored_map_confirmed && f >= 1 && f <= (int) stored_physical_map.size()) ? stored_physical_map[f - 1] : 0;
        const PhysicalFilament* pf = stored > 0 ? inventory.find(stored) : nullptr;
        bool stored_valid = pf != nullptr && !pf->empty();
        if (stored_valid) {
            proposal[i] = stored;
            continue;
        }
        // Mixed mode: a confirmed pick of a tool with NOTHING recorded stores physical 0 plus
        // the tool in filament_map. As long as that tool is still empty, repropose the pick as
        // the -(tool) sentinel (same encoding as bootstrap) instead of auto-matching it away;
        // once the tool gains a recorded filament, auto-match takes over below.
        if (stored_map_confirmed && stored == 0 && f >= 1 && f <= (int) stored_filament_map.size()) {
            int tool = stored_filament_map[f - 1];
            if (tool >= 1 && tool <= (int) inventory.slots.size() && inventory.slots[tool - 1].empty()) {
                proposal[i] = -tool;
                continue;
            }
        }
        int filament_id = f - 1; // stored 1-based, arrays are 0-based
        int matched = (filament_id >= 0 && (size_t) filament_id < auto_result.physical_map.size())
                          ? auto_result.physical_map[filament_id] : -1;
        proposal[i] = matched > 0 ? matched : 0;
    }
    return proposal;
}

std::string FilamentInventories::serialize() const
{
    json jpresets = json::object();
    for (const auto& [preset_name, inv] : by_preset)
        jpresets[preset_name] = json::parse(inv.serialize());
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

    // An envelope is only recognized when BOTH "version" (an integer) and "presets" (an object)
    // are present. A legacy (pre-envelope) store is itself an object keyed by preset name, whose
    // VALUES are always inventory objects (see FilamentInventory::serialize) -- never integers --
    // so a legacy preset literally named "version" can never make j["version"] an integer, and
    // this rule still classifies that store as legacy, loading it correctly.
    const bool  is_envelope = j.contains("version") && j["version"].is_number_integer() &&
                               j.contains("presets") && j["presets"].is_object();
    const json& jpresets    = is_envelope ? j["presets"] : j;

    for (const auto& [preset_name, jinv] : jpresets.items()) {
        if (!jinv.is_object())
            continue;
        store.by_preset[preset_name] = FilamentInventory::deserialize(jinv.dump(), 0);
    }
    return store;
}

FilamentInventory& FilamentInventories::for_preset(const std::string& printer_preset_name, size_t slot_count)
{
    FilamentInventory& inv = by_preset[printer_preset_name]; // default-constructed (no slots yet) on first use
    inv.ensure_slot_count(slot_count);
    return inv;
}

} // namespace Slic3r
