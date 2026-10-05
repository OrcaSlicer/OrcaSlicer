#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/ProjectTask.hpp"
#include "DeviceManager.hpp"

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

// A device with no AMS units has a single source: the external spool. OrcaSlicer's
// auto-mapping force-selects it for every filament (DevMapping.cpp), but that is not
// a lane choice: keep it out of the send gate and off print.gcode_file.
inline void drop_forced_external_selection(bool device_has_ams, std::string& ams_mapping2)
{
    if (!device_has_ams)
        ams_mapping2.clear();
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


// Refusal reason for a printer agent that serializes lane selection into
// print.gcode_file's per-print filament_mapping field.
enum class MappingSendError {
    none,        // no refusal; ams_mapping2 is normalized for send
    unsupported, // a mapping is engaged but the connector did not advertise it
    incomplete,  // a used filament has no target while others do
};

// Applies the send-time mapping policy and normalizes ams_mapping2 in place.
// Agents that do not speak filament_mapping keep their legacy payload untouched.
inline MappingSendError prepare_filament_mapping_for_send(MachineObject* obj,
                                                          std::string& ams_mapping2,
                                                          const std::vector<FilamentInfo>& mapping_result)
{
    if (!obj || !obj->printer_uses_filament_mapping())
        return MappingSendError::none;

    // A device with no AMS units has one source, the external spool:
    // auto-mapping force-selects it, which is not a lane choice. Drop it before
    // the capability gate so it cannot refuse a print nobody mapped.
    drop_forced_external_selection(obj->HasAms(), ams_mapping2);

    if (!obj->printer_supports_feature("filament_mapping") && has_engaged_filament_mapping(ams_mapping2))
        return MappingSendError::unsupported;
    if (has_any_mapped_target(mapping_result) && has_used_filament_without_target(mapping_result))
        return MappingSendError::incomplete;
    return MappingSendError::none;
}

} // namespace GUI
} // namespace Slic3r
