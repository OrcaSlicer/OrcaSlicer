#pragma once

#include "Color.hpp"
#include "MixedFilament.hpp"

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
    float         distance    = 1e30f; // CIE76; smaller = closer
};

// Nearest printable short-stack (period <= period_cap, no 4th component) on the given physicals.
MixMatchResult match_printable_mix(
    const ColorRGB              &target,
    const std::vector<ColorRGB> &physicals,     // C/M/Y/K; extra slots beyond 4 ignored
    const std::vector<float>    *td = nullptr,  // optional; default Panchroma TDs when n==4
    int                          period_cap = 4);

// Trim, optional '#', 6 or 8 hex digits → "#RRGGBB" / "#RRGGBBFF". Empty if invalid.
std::string normalize_mix_match_hex(const std::string &text);

// Predicted swatch for an existing mix. Same Yule-Nielsen n=3 as the lattice.
ColorRGB predicted_swatch_for_mix(const MixedFilament          &mf,
                                  const std::vector<ColorRGB>  &physicals,
                                  const std::vector<float>     *td = nullptr);

} // namespace Slic3r
