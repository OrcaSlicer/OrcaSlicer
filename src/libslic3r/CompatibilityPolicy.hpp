#ifndef slic3r_CompatibilityPolicy_hpp_
#define slic3r_CompatibilityPolicy_hpp_

#include <cstddef>
#include <string>
#include <vector>

#include "PrintConfig.hpp"

namespace Slic3r {

// Pure, headless policy module for the "compatibility" feature.
//
// It classifies config option keys into process / filament / printer scopes,
// filters a full merged config into a "compatibility payload" (process +
// filament keys only, never printer / machine keys), and applies such a payload
// onto a user's current config while keeping their printer untouched.
//
// The module is intentionally free of any GUI / wxWidgets dependency so it can
// be unit-tested headlessly in tests/libslic3r.
namespace CompatibilityPolicy {

// Result of filtering a full merged config into a compatibility payload.
struct FilterResult {
    // Filtered process + filament keys (filament keys stay per-extruder arrays).
    DynamicPrintConfig payload;
    // Keys dropped (machine / cross-ref / bookkeeping / unclassified).
    std::vector<std::string> dropped;
    // Keys renamed via legacy handling (populated on apply, not filter).
    std::vector<std::string> substituted;
    // Filament slot indices whose type matched the user's (for gating).
    std::vector<size_t> filament_slots;
};

// Build the compatibility payload from a full merged config.
// full_config: the merged FullPrintConfig being saved.
// user_current: the opening user's current config (used for filament-type gating
//               and extruder count).
FilterResult filter(const DynamicPrintConfig& full_config, const DynamicPrintConfig& user_current);

// Result of applying a compatibility payload onto the user's current config.
struct ApplyResult {
    // Keys written into the project_config layer.
    std::vector<std::string> applied;
    // Keys skipped (printer / cross-ref / bookkeeping / unclassified).
    std::vector<std::string> dropped;
    // Filament keys dropped because they are material-critical and no filament
    // slot matched the user's type (reported separately so the UI can warn).
    std::vector<std::string> material_dropped;
    // Keys renamed via legacy handling (populated on apply, not filter).
    std::vector<std::string> substituted;
};

// Apply a compatibility payload onto the user's current config.
// out: the project_config layer to write into (a DynamicPrintConfig).
// payload: the filtered payload from filter() or deserialized from a file.
// user_current: the user's current config (extruder count, filament types).
ApplyResult apply(DynamicPrintConfig& out, const DynamicPrintConfig& payload, const DynamicPrintConfig& user_current);

// Serialize a FilterResult.payload to a JSON string (for embedding in the 3MF).
std::string serialize_payload(const DynamicPrintConfig& payload);

// Deserialize a JSON string back into a DynamicPrintConfig (for applying on load).
// Uses ConfigBase::load_from_json with a ConfigSubstitutionContext so legacy
// renames are honored; any substitutions are collected into `substituted`.
DynamicPrintConfig deserialize_payload(const std::string& json, std::vector<std::string>& substituted);

} // namespace CompatibilityPolicy
} // namespace Slic3r

#endif // slic3r_CompatibilityPolicy_hpp_
