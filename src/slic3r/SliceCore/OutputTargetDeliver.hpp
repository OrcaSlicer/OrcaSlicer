#pragma once

#include <string>

#include "SliceTypes.hpp"

namespace Slic3r {
namespace SliceCore {

/// Deliver a finished gcode file to the destination described by \p out.
/// Returns true on success; on failure returns false and sets \p err.
bool deliver(const OutputTarget &out,
             const std::string  &gcode_path,
             std::string        &err);

} // namespace SliceCore
} // namespace Slic3r
