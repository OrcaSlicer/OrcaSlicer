#pragma once

#include "Color.hpp"
#include "MixedFilament.hpp"

#include <array>
#include <string>
#include <vector>

namespace Slic3r {

struct MixMatchResult
{
    bool     valid        = false;
    enum class Kind { Physical, Mix } kind = Kind::Physical;
    unsigned physical_id  = 0; // 1-based if Physical
    MixedFilament mix;         // if Mix; empty-ish if Physical
    std::string   recipe_row;  // manager-serializable row, or empty if Physical
    ColorRGB      predicted;   // honesty swatch
    float         distance    = 1e30f; // mixer ΔE00 (CIEDE2000); smaller = closer
    bool          measured    = false; // LUT Lab used for distance
};

struct SwatchLUT;

// Mixer Lab ΔE00 (CIEDE2000). Shared by Match ranking, paint-bake collapse, and Match UI.
float mixer_delta_e00(const ColorRGB &u, const ColorRGB &v);

// Vendor physical cards for default TD scales (hex-gated, not n==4 alone).
struct SpectrumPhysicalCard {
    ColorRGB hex[4];
    float    td[4];
};

SpectrumPhysicalCard spectrum_panchroma_cmyk_card();
SpectrumPhysicalCard spectrum_panchroma_rgbw_card();

bool spectrum_physicals_match_card(const std::vector<ColorRGB> &physicals,
                                   const SpectrumPhysicalCard  &card);

// explicit td (size==n) wins; else CMYK / RGBW card match; else ones.
std::vector<float> spectrum_default_td_scale(const std::vector<ColorRGB> &physicals,
                                             const std::vector<float>    *td = nullptr);

std::array<std::string, 4> spectrum_rgbw_hexes();

// false if colour.size() < 4 (no writes). Else overwrite [0..3] only.
// If any existing value is 9-char #RRGGBBFF, append FF to stamped hexes.
bool spectrum_stamp_slot_hexes(std::vector<std::string>        &filament_colour,
                               const std::array<std::string, 4> &hexes);

// false if colour.size() < 4 (no writes).
// Else: if multi.size() < colour.size(), resize and fill NEW indices from colour;
// then multi[0..3] = colour[0..3]. Never shrink. Never rewrite i >= 4.
bool spectrum_stamp_multi_heads(std::vector<std::string>       &multi,
                                const std::vector<std::string> &colour);

// Manager-canonical Mix recipe row (same string as MixMatchResult::recipe_row).
std::string serialize_mix_recipe(const MixedFilament &mf);

enum class SpectrumPaletteAddOutcome { Append, SelectExisting, CapRefuse };

struct SpectrumPaletteAddResult {
    SpectrumPaletteAddOutcome outcome = SpectrumPaletteAddOutcome::Append;
    size_t                    index   = 0; // append index or existing row
};

// Duplicate = serialize_mix_recipe equality (includes manual_pattern).
// Not spectrum_cookbook_same_recipe ("1234" cycle ≠ 1:1 pair).
SpectrumPaletteAddResult spectrum_palette_try_add(
    std::vector<MixedFilament> &rows,
    const MixedFilament        &candidate,
    size_t                      cap = SPECTRUM_MIX_ENABLED_CAP);

// 0010 printable lattice: physicals + pair/triple generators + "1234".
// period_cap=4, no min-share / max-share / max_results. Physicals always included.
std::vector<MixMatchResult> spectrum_swatch_lattice(const std::vector<ColorRGB> &physicals);

// Nearest printable short-stack (period <= period_cap, no 4th component) on the given physicals.
// Mix rows whose largest component share * 100 > max_component_percent are dropped; Physicals always pass.
// lut nullptr → predicted-RGB mixer ΔE00 (HEAD path). LUT hit → mixer ΔE00 on measured Lab vs target Lab.
MixMatchResult match_printable_mix(
    const ColorRGB              &target,
    const std::vector<ColorRGB> &physicals,     // first four physicals; extras beyond 4 ignored
    const std::vector<float>    *td = nullptr,  // optional; else CMYK/RGBW card TDs when hexes match
    int                          period_cap = 4,
    int                          max_component_percent = 100,
    const SwatchLUT             *lut = nullptr);

// Ranked lattice matches (mixer ΔE00, then shorter max(period, pattern_len)).
// Dark-neutral override runs before truncate and keeps CIE76 for Physical distance.
// Mix rows with integer min-share < min_component_percent are dropped; Physicals always pass.
// Mix rows whose largest component share * 100 > max_component_percent are dropped.
// lut nullptr → byte-identical to ranking without a lut pointer.
std::vector<MixMatchResult> match_printable_candidates(
    const ColorRGB              &target,
    const std::vector<ColorRGB> &physicals,
    const std::vector<float>    *td = nullptr,
    int                          period_cap = 4,
    int                          min_component_percent = 25,
    size_t                       max_results = 12,
    int                          max_component_percent = 100,
    const SwatchLUT             *lut = nullptr);

// Pair "A+B ra:rb" / triple "A+B+C ra:rb:rc". Names by 1-based id when provided; else digits.
std::string mix_recipe_label(const MixedFilament &mf, const std::vector<std::string> *slot_names = nullptr);

// "Mix 5  C+Y 2:1" / "Mix 5  1+3 2:1". Prefix + mix_recipe_label. English Catch2 contract (no wx).
std::string mix_surface_label(int id_1based, const MixedFilament &mf,
                              const std::vector<std::string> *slot_names = nullptr);

// Sidebar Color Mixing rows. Mix N = physical_n+1+i in manager order.
struct SpectrumMixListRow {
    int         mix_id_1based = 0;
    std::string surface_label;
    bool        enabled = true;
};

// Enabled mixes only (MixedFilamentManager stores enabled rows). Empty defs → empty.
std::vector<SpectrumMixListRow> spectrum_mix_list_rows(const std::string &defs, size_t physical_n);

// Trim, optional '#', 6 or 8 hex digits → "#RRGGBB" / "#RRGGBBFF". Empty if invalid.
std::string normalize_mix_match_hex(const std::string &text);

// True → hex/picker commit must keep the current candidate pick (skip rebuild).
// False → rebuild and select ranked [0].
// Compare decoded ColorRGB (operator==), never raw hex strings.
// cached_valid false, or parsed RGB != cached, → false.
inline bool spectrum_match_same_target(bool cached_valid, const ColorRGB &cached, const ColorRGB &parsed)
{
    return cached_valid && cached == parsed;
}

// Predicted swatch for an existing mix via ColorMix / prusa-fdm-mixer.
ColorRGB predicted_swatch_for_mix(const MixedFilament          &mf,
                                  const std::vector<ColorRGB>  &physicals,
                                  const std::vector<float>     *td = nullptr);

// physicals in order (1-based IDs 1..n), then one ColorMix swatch per enabled mix
// (same order as MixedFilamentManager virtual IDs). Cap is a mix-append ceiling,
// NOT a physical truncate: physicals are always kept in full;
// if physicals.size() >= cap, return all physicals and append no mixes.
// Default cap 0 resolves to spectrum_preview_color_cap(n) = n + SPECTRUM_MIX_ENABLED_CAP
// BEFORE the never-slice early-return (a leftover 0 would make size() >= cap always true).
// Color Painting passes EXTRUDERS_LIMIT (persist cap / ExtruderMax) explicitly — not the sentinel.
// ColorMix swatch is not Snapmaker printed LUT / Z-stack.
// Disabled rows are not in the manager.
std::vector<ColorRGB> preview_filament_colors(
    const std::vector<ColorRGB> &physicals,
    const std::string           &mixed_filament_definitions,
    size_t                       cap = 0);

} // namespace Slic3r
