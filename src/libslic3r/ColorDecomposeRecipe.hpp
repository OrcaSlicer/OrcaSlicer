#ifndef SLIC3R_COLOR_DECOMPOSE_RECIPE_HPP
#define SLIC3R_COLOR_DECOMPOSE_RECIPE_HPP

#include <string>
#include <vector>

namespace Slic3r {

enum class ColorDecomposeRecipeMode {
    MaterialList,
    CMYW,
    RYBW
};

struct ColorDecomposeRgb {
    unsigned char r{0};
    unsigned char g{0};
    unsigned char b{0};
};

struct ColorDecomposePhysicalFilament {
    std::string  color_hex;
    std::string  name;
    std::string  type;
    bool         is_mixed{false};
    unsigned int filament_index{0}; // 1-based physical filament index
};

struct ColorDecomposeRecipeComponent {
    std::string  color_hex;
    std::string  base_color;
    int          ratio{0};
    unsigned int filament_index{0}; // 1-based for physical filaments, 0 for standard base colors
};

struct ColorDecomposeRecipeResult {
    bool valid{false};
    ColorDecomposeRecipeMode mode{ColorDecomposeRecipeMode::MaterialList};
    std::string matched_color_hex;
    std::vector<ColorDecomposeRecipeComponent> components;
};

std::string color_decompose_rgb_to_hex(const ColorDecomposeRgb& rgb);
bool color_decompose_hex_to_rgb(const std::string& hex, ColorDecomposeRgb& out);

ColorDecomposeRecipeResult recommend_from_physical_filaments(
    const ColorDecomposeRgb& target,
    const std::vector<ColorDecomposePhysicalFilament>& physical_filaments,
    const std::string& preferred_material_type);

ColorDecomposeRecipeResult lookup_standard_recipe(
    const ColorDecomposeRgb& target,
    ColorDecomposeRecipeMode mode,
    const std::string& preferred_material_type);

// Look up the measured blend color for an exact (component_hexes, ratios) match
// in the standard color recipe table. Returns the measured hex color if found
// with reliable source data ("measured" or "interpolated"), empty string otherwise.
std::string lookup_measured_blend_color(const std::vector<std::string>& component_hexes,
                                       const std::vector<int>& ratios);

} // namespace Slic3r

#endif // SLIC3R_COLOR_DECOMPOSE_RECIPE_HPP
