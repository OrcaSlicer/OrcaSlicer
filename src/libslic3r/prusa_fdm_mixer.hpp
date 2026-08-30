/*
 * prusa_fdm_mixer.hpp — Predicts the apparent color of FDM filament mixes.
 *
 * The prusa-fdm-mixer model, calibrated against measured prints. Produces
 * predictions with a median ΔE2000 of ~5.7 against real samples
 * (Linear RGB ~14.5 for comparison).
 *
 * Drop-in usage in PrusaSlicer / OrcaSlicer / any C++17 project:
 *
 *     #include "prusa_fdm_mixer.hpp"
 *
 *     std::vector<prusa_fdm_mixer::Part> parts = {
 *         { "#007a9d", 0.75 },   // 75% azure blue
 *         { "#f6b921", 0.25 },   // 25% yellow
 *     };
 *     std::string mixed_hex = prusa_fdm_mixer::mix(parts);
 *     // mixed_hex == "#XXXXXX" — predicted apparent color
 *
 * Properties:
 *   - Gradient-safe: a part with ratio 1.0 returns that part's hex exactly.
 *   - Smooth: small ratio changes produce small color changes.
 *   - Works for 2 or 3+ components; ratios should sum to 1.
 *
 * No external dependencies beyond the C++ standard library.
 *
 * Copyright (c) Prusa Research s.r.o.
 * MIT License — see LICENSE.
 */

#ifndef PRUSA_FDM_MIXER_HPP
#define PRUSA_FDM_MIXER_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace prusa_fdm_mixer {

/** A single filament component in a mix. */
struct Part {
    /** 6-digit hex color including leading '#', e.g. "#aabbcc". */
    std::string hex;
    /** Mixing ratio in [0, 1]. Ratios across all parts should sum to 1. */
    double ratio;
};

/** RGB triple, channel values in [0, 255]. */
struct RGB {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

/** CIELAB triple — L in [0, 100], a/b unbounded but typically [-128, 127]. */
struct LAB {
    double L;
    double a;
    double b;
};

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Predict the apparent color when filaments are mixed in the given ratios.
 * @param parts  Two or more parts; ratios should sum to ~1.
 * @return       Predicted hex color, e.g. "#aabbcc".
 */
std::string mix(const std::vector<Part>& parts);

/**
 * Same as mix() but returns an RGB struct.
 */
RGB mix_rgb(const std::vector<Part>& parts);

/* ============================================================================
 * Color-space conversions (exposed for callers who want them)
 * ============================================================================ */

/** Parse a "#rrggbb" hex string. Throws std::invalid_argument on bad input. */
RGB hex_to_rgb(const std::string& hex);

/** Format an RGB as "#rrggbb" lowercase. */
std::string rgb_to_hex(const RGB& rgb);

/** Convert sRGB (0-255 channels) to CIELAB (D65 white point). */
LAB rgb_to_lab(const RGB& rgb);

/** Convert CIELAB (D65 white point) back to sRGB (0-255 channels, clamped). */
RGB lab_to_rgb(const LAB& lab);

/** ΔE2000 perceptual color difference between two LAB colors. */
double delta_e_2000(const LAB& lab1, const LAB& lab2);

} // namespace prusa_fdm_mixer

#endif // PRUSA_FDM_MIXER_HPP
