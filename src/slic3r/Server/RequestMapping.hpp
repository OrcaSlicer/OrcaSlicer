#pragma once

// RequestMapping.hpp — JSON -> SliceCore::SliceRequest parsing.
//
// Extracted from the anonymous namespace in Handlers.cpp so that
// json_to_slice_request can be unit-tested independently of the HTTP layer.

#include <nlohmann/json.hpp>

#include "SliceTypes.hpp"

namespace Slic3r {
namespace Server {

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
//
// Declared here (rather than in an anonymous namespace) so that both the HTTP
// handler (json_to_slice_request) and the CLI (--placement-json) can call it
// without duplicating the parsing logic.
void parse_objects(const nlohmann::json &arr,
                   std::vector<Slic3r::SliceCore::ObjectPlacement> &out);

// Parses a JSON slice-request body into a SliceCore::SliceRequest.
// Throws nlohmann::json::exception on malformed field types; unknown / missing
// optional fields are silently ignored (lenient).  Existing behaviour is
// preserved exactly — only GAP 1 (presets.overrides) and GAP 4 (transform
// flags) are new additions.
SliceCore::SliceRequest json_to_slice_request(const nlohmann::json &j);

} // namespace Server
} // namespace Slic3r
