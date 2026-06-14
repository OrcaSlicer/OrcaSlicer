#pragma once

// ObjectPlacementJson.hpp — JSON -> SliceCore::ObjectPlacement parsing.
//
// Moved from Server/RequestMapping.hpp so that both the OrcaSlicer app
// (--placement-json CLI path, compiled with SLIC3R_GUI=ON but without the
// orca-server translation units) and orca-server can share the implementation
// via liborca_slice_core, which both targets link.

#include <nlohmann/json.hpp>

#include "SliceTypes.hpp"

namespace Slic3r {
namespace SliceCore {

// Parses a JSON `objects` array into a vector of ObjectPlacement descriptors.
//
// Each element may contain:
//   index         (int)
//   name          (string)
//   position      ({x,y,z} object OR [x,y,z] array)
//   rotation      ({x,y,z} object OR [x,y,z] array)  — degrees per axis
//   scale         ({x,y,z} object → per-axis scale  OR number → uniform_scale)
//   mirror        ({x,y,z} bools  OR [bool,bool,bool] array)
//   orient        (int)
//   ensure_on_bed (bool)
//   printable     (bool)
//   instances     (array of {position, rotation_z, scale})
//
// Throws nlohmann::json::exception on type mismatch; unknown keys are silently
// ignored.  Only optionals that are present in the JSON are set.
void parse_objects(const nlohmann::json &arr,
                   std::vector<ObjectPlacement> &out);

} // namespace SliceCore
} // namespace Slic3r
