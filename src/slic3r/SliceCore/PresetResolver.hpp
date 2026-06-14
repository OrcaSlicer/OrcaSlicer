#pragma once

#include <string>
#include <vector>

#include "SliceTypes.hpp"

namespace Slic3r {
namespace SliceCore {

/// Resolve a PresetSelection against the on-disk datadir and return a merged
/// DynamicPrintConfig ready for slicing.
/// On failure, returns an empty config and sets \p err to a descriptive message.
DynamicPrintConfig resolve(const PresetSelection &sel,
                           const std::string      &datadir,
                           std::string            &err);

/// Names of all visible, non-default presets enumerated from a PresetBundle
/// loaded from \p datadir.
struct PresetNames {
    std::vector<std::string> printers;
    std::vector<std::string> processes;
    std::vector<std::string> filaments;
};

/// Load the PresetBundle from \p datadir and fill \p out with the names of all
/// visible, non-default printer/process/filament presets.
/// Returns true on success.  On failure returns false and sets \p err.
/// Never throws.
bool enumerate_preset_names(const std::string &datadir,
                             PresetNames       &out,
                             std::string       &err);

} // namespace SliceCore
} // namespace Slic3r
