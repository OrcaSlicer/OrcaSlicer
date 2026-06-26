#ifndef slic3r_NozzleAgnostic_hpp_
#define slic3r_NozzleAgnostic_hpp_

#include <optional>
#include <string>
#include <vector>

#include "Config.hpp"

// Pure libslic3r helpers for making a process profile nozzle-agnostic: convert absolute
// nozzle-diameter-scaled fields to percentages so one profile prints correctly on mixed nozzles.
// A field is in scope iff it is a coFloatOrPercent option with ratio_over == "nozzle_diameter".

namespace Slic3r {

// Used only by ref/ptr below, so the heavy headers stay in the .cpp.
class Preset;
class PresetBundle;
class DynamicPrintConfig;

namespace nozzle_agnostic {

// --- conversion math + in-scope rule ---

double      to_percent(double abs_mm, double base_nozzle);          // abs / base * 100, exact

// Idempotent: already-percent and base <= 0 are returned unchanged. Returns a new value, never mutates.
ConfigOptionFloatOrPercent convert_field(const ConfigOptionFloatOrPercent &in, double base_nozzle);

bool                     is_in_scope(const ConfigOptionDef &def);   // coFloatOrPercent && ratio_over=="nozzle_diameter"
std::vector<std::string> scoped_keys(const ConfigDef &def_registry);

// --- detect / resolve / refuse ---

bool is_mixed_nozzle(const std::vector<double> &nozzles);           // >= 2 distinct diameters
bool has_absolute_scoped_fields(const DynamicPrintConfig &process_cfg);

// Base divisor = the nozzle the selected process profile is compatible_printers-bound to.
// nullopt when it cannot be resolved to exactly one diameter (refuse rather than guess).
std::optional<double> resolve_base_nozzle(const Preset &process, const PresetBundle &bundle);

} // namespace nozzle_agnostic
} // namespace Slic3r

#endif // slic3r_NozzleAgnostic_hpp_
