// Open Filament Database catalog lookup + slot hex stamp (0061). wx-free.
#pragma once

#include "MixedFilament.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

struct SpectrumOfdVariant {
    std::string brand;
    std::string filament;   // product line e.g. "Matte PLA"
    std::string variant;    // colour name e.g. "Sky Blue"
    std::string material;   // e.g. "PLA" (optional)
    std::vector<std::string> color_hexes; // 1+; first is slot default; >1 = dual-color
    bool translucent = false;
    bool transparent = false;
};

// Parse seed JSON object `{ "variants": [ ... ] }` OR a JSON array of variants OR OFD NDJSON
// (one object per line). color_hex may be "#RRGGBB" string or array of those. Normalize to
// uppercase #RRGGBB. Skip rows with no usable hex. Never throws.
std::vector<SpectrumOfdVariant> spectrum_ofd_parse(const std::string &text);

// Load seed file then overlay user NDJSON if present (user rows appended; lookup can match either).
// Missing files → empty extra, not throw. Empty text → empty vector.
std::vector<SpectrumOfdVariant> spectrum_ofd_load_catalog(
    const std::string &seed_json_path,
    const std::string &user_ndjson_path = {});

// Case-insensitive contains. brand_filter empty = all brands. name_substring matches filament
// and/or variant (and brand if no brand filter).
std::vector<SpectrumOfdVariant> spectrum_ofd_lookup(
    const std::vector<SpectrumOfdVariant> &catalog,
    const std::string &brand_filter,
    const std::string &name_substring);

std::string spectrum_ofd_slot_hex(const SpectrumOfdVariant &v); // first hex or empty

// Stamp slot N. Writes colour[N]=first hex; multi[N]=space-joined hexes; type[N]="1"
// (solid dual stored as multi, not gradient "0"). If override_flags[N] is true and force==false,
// skip (return false, no writes). Catalog Apply uses force=true and then CLEARS override_flags[N].
// Resize vectors if needed so slot N exists; do not rewrite other slots. Hexes empty → false.
bool spectrum_ofd_stamp_slot(
    std::vector<std::string> &filament_colour,
    std::vector<std::string> &filament_multi_colour,
    std::vector<std::string> &filament_colour_type,
    std::vector<char>        &override_flags,
    size_t                    slot,
    const std::vector<std::string> &hexes,
    bool                      force);

enum class SpectrumMixSeedMode { Ask, Always, Never };

SpectrumMixSeedMode spectrum_mix_seed_mode_from_string(const std::string &s); // default Ask

enum class SpectrumMixSeedDecision { Prompt, Append, Skip };

// Never → Skip; already_prompted → Skip; any slot hex empty (after trim) → Skip;
// any enabled mix row exists → Skip; Always → Append; Ask → Prompt.
SpectrumMixSeedDecision spectrum_ofd_mix_seed_decision(
    const std::vector<MixedFilament> &rows,
    const std::vector<std::string>   &slot_hexes,
    SpectrumMixSeedMode               mode,
    bool                              already_prompted);

// Apply cookbook if user_yes or mode==Always; if Never or !user_yes → return existing unchanged.
// Empty + Yes → cookbook rows. Existing + Yes → spectrum_cookbook_append (no clobber).
std::vector<MixedFilament> spectrum_ofd_mix_seed_apply(
    const std::vector<MixedFilament> &existing,
    size_t                            num_physical,
    bool                              user_yes,
    SpectrumMixSeedMode               mode);

// Canonical mix-def string. Keeps disabled rows (load_definitions keep_disabled).
std::string spectrum_ofd_serialize_mix_rows(const std::vector<MixedFilament> &rows);

} // namespace Slic3r
