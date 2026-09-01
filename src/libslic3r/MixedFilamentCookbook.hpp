// Ordered house CMYK mix cookbook (0016). One-click append seed — not Snapmaker auto_generate.
#pragma once

#include "MixedFilament.hpp"

#include <cstddef>
#include <vector>

namespace Slic3r {

// Ordered house recipes for n_use = min(num_physical, 4). Empty if n_use < 2.
std::vector<MixedFilament> spectrum_cookbook_recipes(size_t num_physical);

// Same A/B/C + gcd-reduced ratios. Ignores enabled / pattern / xa / xb.
// If component_c == 0, ratio_c is treated as 0 for comparison.
bool spectrum_cookbook_same_recipe(const MixedFilament &a, const MixedFilament &b);

struct MixCookbookAppend {
    std::vector<MixedFilament> added;
    size_t skipped_duplicate = 0;
    size_t skipped_cap       = 0;
};

// Walk recipes in order. Skip same-recipe vs any existing row.
// Stop proposing when enabled(existing+already-added) + 1 would exceed
// mix_enabled_cap (default SPECTRUM_MIX_ENABLED_CAP). Physical count is not
// part of this gate (volume Mix N is independent of paint persist 16).
MixCookbookAppend spectrum_cookbook_append(
    const std::vector<MixedFilament> &existing,
    size_t num_physical,
    size_t mix_enabled_cap = SPECTRUM_MIX_ENABLED_CAP);

} // namespace Slic3r
