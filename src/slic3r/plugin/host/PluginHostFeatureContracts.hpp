#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

// A copied semantic contract advertised by the host.  It identifies a closed
// host feature, not a plugin capability, host build, or a live host object.
struct PluginHostFeatureContract {
    std::string feature_id;
    uint32_t    major_version { 0 };
    uint32_t    minor_version { 0 };
};

// Return the complete, deterministic set of semantic host contracts compiled
// into this build.  Callers must require the feature and compatible major
// version they need; a missing or incompatible entry is unavailable.
std::vector<PluginHostFeatureContract> plugin_host_feature_contracts();

} // namespace Slic3r
