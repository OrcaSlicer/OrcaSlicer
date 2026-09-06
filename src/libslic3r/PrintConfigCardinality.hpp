// PrintConfigCardinality - one declared cardinality per vector config option, driving all per-N array sizing.
//
// Many options hold "one value per extruder / filament / variant". cardinality_of() classifies each vector
// option into a single ConfigCardinality; expected_size() turns that plus a config's per-N counts into the
// length the option should have. enforce_cardinality() applies this across a whole config at the Print::apply
// boundary, so the slice never indexes a per-N array past its end. Every current vector option is classified
// (per-N or explicitly Custom), and the [Cardinality] completeness test fails if a new one is left unclassified.

#ifndef slic3r_PrintConfigCardinality_hpp_
#define slic3r_PrintConfigCardinality_hpp_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;
class DynamicPrintConfig;

// Which per-N count sets a vector option's length (Scalar/Custom = not per-N; the sizer leaves them alone).
enum class ConfigCardinality : uint8_t {
    Scalar,                 // single value, or an owner-sized/fixed-shape vector (never per-N)
    PerExtruder,            // length == extruder count
    PerFilament,            // length == filament count
    PerExtruderVariant,     // length == extruder-variant count
    PerExtruderVariantDual, // length == extruder-variant count * 2 (the machine_max_* normal + silent-mode pair)
    PerFilamentVariant,     // length == filament-variant count
    PerProcessVariant,      // length == process-variant count
    Custom,                 // a vector option the sizer must NOT touch (matrix/geometry/meta/etc.),
                            // distinct from Scalar so every vector option is explicitly classified.
};

// Classify one option's cardinality (Scalar for scalars / owner-sized vectors; see build_cardinality_map).
ConfigCardinality cardinality_of(const std::string &opt_key);

// The per-N counts of a config: how many extruders, filaments, and variant slots each axis has.
struct AxisCounts {
    size_t extruders = 1, filaments = 1;
    size_t extruder_variant = 1, filament_variant = 1, process_variant = 1;
};

// Read the counts from a config; resolve a cardinality plus counts to a length (0 for Scalar/Custom, "do not size").
AxisCounts axis_counts(const ConfigBase &config);
size_t     expected_size(ConfigCardinality cardinality, const AxisCounts &n);

// Values per axis slot: 2 for the machine_max_* normal + silent-mode pair (PerExtruderVariantDual), 1 otherwise.
size_t cardinality_stride(ConfigCardinality cardinality);

// Resize one option in `config` to the length its cardinality implies. No-op for Scalar/Custom and for an
// unset option with no default to replicate.
void resize_to_cardinality(ConfigBase &config, const std::string &key, const AxisCounts &n);

// Per-N options a config authors to a length that contradicts its axis count, schema defaults excluded (only an
// authored wrong length counts).
struct CardinalityMismatch { std::string key; size_t got, want; };
std::vector<CardinalityMismatch> cardinality_contradictions(const ConfigBase &config);

// The config-mutating entry points are DynamicPrintConfig methods (declared in PrintConfig.hpp):
//   config.enforce_cardinality()              - size every per-N option; call at the Print::apply boundary, after
//                                         variant expansion, so no slice indexes a per-N array past its end.
//   config.enforce_per_filament_cardinality() - the load-time subset (plain per-filament only), run from
//                                         Preset::normalize before variant expansion.
//   config.get_parameter_size(key, n)   - legacy per-key shim (set_num_extruders' path); prefer expected_size().

} // namespace Slic3r

#endif // slic3r_PrintConfigCardinality_hpp_
