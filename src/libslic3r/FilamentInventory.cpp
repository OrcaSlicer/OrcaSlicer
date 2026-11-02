#include "FilamentInventory.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
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
