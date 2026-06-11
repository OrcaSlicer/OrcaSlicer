#pragma once

// RequestMapping.hpp — JSON -> SliceCore::SliceRequest parsing.
//
// Extracted from the anonymous namespace in Handlers.cpp so that
// json_to_slice_request can be unit-tested independently of the HTTP layer.
//
// parse_objects() has been relocated to SliceCore/ObjectPlacementJson.hpp so
// that it is available to both the OrcaSlicer app and orca-server via
// liborca_slice_core.  It is re-exported here for callers that previously
// included this header for parse_objects.

#include <nlohmann/json.hpp>

#include "SliceCore/ObjectPlacementJson.hpp"  // re-export Slic3r::SliceCore::parse_objects
#include "SliceTypes.hpp"

namespace Slic3r {
namespace Server {

// Parses a JSON slice-request body into a SliceCore::SliceRequest.
// Throws nlohmann::json::exception on malformed field types; unknown / missing
// optional fields are silently ignored (lenient).  Existing behaviour is
// preserved exactly — only GAP 1 (presets.overrides) and GAP 4 (transform
// flags) are new additions.
SliceCore::SliceRequest json_to_slice_request(const nlohmann::json &j);

} // namespace Server
} // namespace Slic3r
