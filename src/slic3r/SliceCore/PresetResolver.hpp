#pragma once

#include <string>

#include "SliceTypes.hpp"

namespace Slic3r {
namespace SliceCore {

/// Resolve a PresetSelection against the on-disk datadir and return a merged
/// DynamicPrintConfig ready for slicing.
/// On failure, returns an empty config and sets \p err to a descriptive message.
DynamicPrintConfig resolve(const PresetSelection &sel,
                           const std::string      &datadir,
                           std::string            &err);

} // namespace SliceCore
} // namespace Slic3r
