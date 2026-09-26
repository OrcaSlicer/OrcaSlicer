#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/ProjectTask.hpp"

namespace Slic3r {
namespace GUI {

// True when the normalized ams_mapping2 carries any entry the agent's serializer
// would put on the wire: every integer pair except the {255,255} unmatched
// sentinel, external slots ({255,0}/{254,0}) included. This mirrors
// OrcaPrinterAgent::build_filament_mapping exactly, so the GUI gate and the
// agent gate agree; a mismatch lets an entry reach the agent and be refused late
// with a generic publish error instead of the designed message.
inline bool has_engaged_filament_mapping(const std::string& ams_mapping2)
{
    const nlohmann::json mapping = nlohmann::json::parse(ams_mapping2, nullptr, false);
    if (mapping.is_discarded() || !mapping.is_array())
        return false;
    for (const auto& entry : mapping) {
        if (!entry.is_object())
            continue;
        const auto ams_id_it  = entry.find("ams_id");
        const auto slot_id_it = entry.find("slot_id");
        if (ams_id_it == entry.end() || slot_id_it == entry.end())
            continue;
        if (!ams_id_it->is_number_integer() || !slot_id_it->is_number_integer())
            continue;
        if (ams_id_it->get<int>() == 255 && slot_id_it->get<int>() == 255)
            continue; // unmatched/unused sentinel: the serializer drops it
        return true;
    }
    return false;
}

// A used filament with no target would be silently dropped from the wire
// mapping, so the print must be refused rather than run the wrong material.
// m_ams_mapping_result carries exactly the filaments the slice uses.
inline bool has_used_filament_without_target(const std::vector<FilamentInfo>& result)
{
    for (const auto& f : result) {
        if (f.get_ams_id() < 0 || f.get_slot_id() < 0)
            return true;
    }
    return false;
}

// True when at least one used filament already has a target. The used-unmapped
// refusal applies to a partially mapped print; a print with no mapping at all
// is handled by the existing all-invalid send-path flow.
inline bool has_any_mapped_target(const std::vector<FilamentInfo>& result)
{
    for (const auto& f : result) {
        if (f.get_ams_id() >= 0 && f.get_slot_id() >= 0)
            return true;
    }
    return false;
}

} // namespace GUI
} // namespace Slic3r
