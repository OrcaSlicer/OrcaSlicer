#ifndef slic3r_GUI_ColorDecomposeSupport_hpp_
#define slic3r_GUI_ColorDecomposeSupport_hpp_

#include <string>
#include <vector>
#include <wx/string.h>
#include <wx/colour.h>
#include "ColorDecomposeDialog.hpp"

class wxWindow;

namespace Slic3r {
class Preset;
namespace GUI {

// ---- Constants ----

inline constexpr const char* kDecomposePlaBasicType     = "PLA Basic";
inline constexpr const char* kDecomposePetgBasicType    = "PETG Basic";
inline constexpr const char* kDecomposePlaShortType     = "PLA";
inline constexpr const char* kDecomposePetgShortType    = "PETG";
inline constexpr const char* kDecomposePlaFilamentId    = "GFA00";
inline constexpr const char* kDecomposePetgFilamentId   = "GFG00";
inline constexpr const char* kDecomposeBambuPresetPrefix = "Bambu ";

// ---- Types ----

struct DecomposeOfficialComponent {
    DecomposeBaseColor base_color{DecomposeBaseColor::None};
    std::string        color_hex;
    std::string        filament_id;
};

struct DecomposeMissingComponent {
    size_t                     component_idx{0};
    DecomposeOfficialComponent official_component;
    std::string                preset_name;
    wxString                   display_name;
};

struct MixedFilamentResult;

// ---- Functions ----

std::string decompose_normalize_color_hex(std::string color);

const char* decompose_base_color_en(DecomposeBaseColor color);

wxString decompose_base_color_display(DecomposeBaseColor color);

std::string decompose_basic_type_from_source(size_t source_config_idx,
                                             size_t source_physical_idx,
                                             const std::vector<std::string>& physical_types);

std::string decompose_basic_filament_id(const std::string& basic_type);

void set_created_standard_component_metadata(size_t config_idx, const DecomposeOfficialComponent& component);

DecomposeOfficialComponent lookup_decompose_official_component(
    const std::string& basic_type,
    DecomposeBaseColor base_color,
    const wxColour& fallback);

std::string find_decompose_standard_preset_name(size_t source_config_idx, const std::string& basic_type);

// Returns "PLA Basic" / "PETG Basic" when preset_name names an official Bambu
// basic filament, else an empty string.
std::string official_basic_type_from_preset_name(const std::string& preset_name);

// Resolve display type for color-decompose: official Bambu Basic overrides
// get_filament_type when preset name matches; empty/missing -> "PLA".
std::string filament_type_for_color_decompose(Preset* preset);

int find_existing_decompose_component(
    const DecomposeOfficialComponent& component,
    const std::vector<std::string>& physical_colors,
    const std::vector<size_t>& physical_config_indices,
    size_t source_config_idx);

bool prepare_decompose_mixed_result(
    const ColorDecomposeResult& result,
    size_t source_config_idx,
    size_t source_physical_idx,
    const std::vector<std::string>& physical_colors,
    const std::vector<std::string>& physical_types,
    const std::vector<size_t>& physical_config_indices,
    MixedFilamentResult& out_result,
    std::vector<DecomposeMissingComponent>& missing);

// For standard modes: how many base colors are not reusable from physical list.
// MaterialList returns 0. When physical_config_indices is null, indices are 0..n-1.
size_t count_decompose_new_physical_filaments(
    const ColorDecomposeResult& result,
    const std::vector<std::string>& physical_colors,
    const std::vector<std::string>& physical_types,
    size_t source_physical_idx,
    const std::vector<size_t>* physical_config_indices);

bool confirm_create_decompose_missing_components(wxWindow* parent,
    const std::vector<DecomposeMissingComponent>& missing);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ColorDecomposeSupport_hpp_
