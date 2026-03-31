#ifndef SLIC3R_FILAMENT_MIXER_H
#define SLIC3R_FILAMENT_MIXER_H

#include <string>
#include <vector>

namespace Slic3r {

void filament_mixer_lerp(unsigned char r1, unsigned char g1, unsigned char b1,
                         unsigned char r2, unsigned char g2, unsigned char b2,
                         float t,
                         unsigned char* out_r, unsigned char* out_g, unsigned char* out_b);

void filament_mixer_lerp_float(float r1, float g1, float b1,
                               float r2, float g2, float b2,
                               float t,
                               float* out_r, float* out_g, float* out_b);

void filament_mixer_lerp_linear_float(float r1, float g1, float b1,
                                      float r2, float g2, float b2,
                                      float t,
                                      float* out_r, float* out_g, float* out_b);

// Blend two hex colors ("#RRGGBB") by ratio (0.0 ~ 1.0 for color_b).
// Returns "#RRGGBB" string.
std::string blend_color(const std::string& hex_a, const std::string& hex_b, float ratio_b);

// Parse comma-separated 1-based component IDs, e.g. "1,3" → {1, 3}.
std::vector<unsigned int> parse_mixed_components(const std::string &str);

// Parse comma-separated ratio values, e.g. "0.7,0.3" → {0.7, 0.3}.
// Returns equal ratios (1/n each) when str is empty or invalid.
// Normalizes so the sum equals 1.0.
std::vector<double> parse_mixed_ratios(const std::string &str, size_t n_components);

// Returns true if any element in is_mixed is true.
// ConfigOptionBools stores values as std::vector<unsigned char>.
bool has_any_mixed_filament(const std::vector<unsigned char> &is_mixed);

// Check which mixed filament slots have broken component references.
// Returns 0-based indices of mixed slots whose components reference
// filaments beyond num_physical (i.e., deleted filaments).
std::vector<size_t> check_mixed_filament_integrity(
    const std::vector<unsigned char> &is_mixed,
    const std::vector<std::string>   &comp_strs,
    size_t num_physical);

// Expand mixed filament slots in an extruder list to their physical components.
// Input/output are 0-based indices. Non-mixed slots pass through unchanged.
// Result is sorted and deduplicated.
std::vector<unsigned int> expand_mixed_filaments(
    const std::vector<unsigned int> &extruders_0based,
    const std::vector<unsigned char> &is_mixed,
    const std::vector<std::string>  &comp_strs);

} // namespace Slic3r

#endif
