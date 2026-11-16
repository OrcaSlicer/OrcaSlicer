///|/ Copyright (c) Prusa Research 2020 - 2023 Enrico Turri @enricoturri1966
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_LibVGCodeWrapper_hpp_
#define slic3r_LibVGCodeWrapper_hpp_

#include <../../src/libvgcode/include/Viewer.hpp>
#include <../../src/libvgcode/include/PathVertex.hpp>
#include <../../src/libvgcode/include/GCodeInputData.hpp>
#include <../../src/libvgcode/include/ColorRange.hpp>
#include <stddef.h>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

#include "../../src/libvgcode/include/Types.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Point.hpp"


namespace Slic3r {
class Print;

namespace CustomGCode {
struct Item;
}  // namespace CustomGCode
} // namespace Slic3r

namespace libvgcode {
class Viewer;

// mapping from Slic3r::Vec3f to libvgcode::Vec3
extern Vec3 convert(const Slic3r::Vec3f& v);

// mapping from libvgcode::Vec3 to Slic3r::Vec3f
extern Slic3r::Vec3f convert(const Vec3& v);

// mapping from Slic3r::Matrix4f to libvgcode::Mat4x4
extern Mat4x4 convert(const Slic3r::Matrix4f& m);

// mapping from libvgcode::Color to Slic3r::ColorRGBA
extern Slic3r::ColorRGBA convert(const Color& c);

// mapping from Slic3r::ColorRGBA to libvgcode::Color
extern Color convert(const Slic3r::ColorRGBA& c);

// ORCA: mapping from libvgcode::ColorWithAlpha to Slic3r::ColorRGBA
extern Slic3r::ColorRGBA convert(const ColorWithAlpha& c);

// ORCA: mapping from encoded filament colors to libvgcode::PaletteWithAlpha, keeping their opacity
extern PaletteWithAlpha convert_filament_colors(const std::vector<std::string>& colors_str);

// mapping from libvgcode::EGCodeExtrusionRole to Slic3r::ExtrusionRole
extern Slic3r::ExtrusionRole convert(EGCodeExtrusionRole role);

// mapping from Slic3r::ExtrusionRole to libvgcode::EGCodeExtrusionRole
extern EGCodeExtrusionRole convert(Slic3r::ExtrusionRole role);

// mapping from Slic3r::EMoveType to libvgcode::EMoveType
extern EMoveType convert(Slic3r::EMoveType type);

// mapping from Slic3r::GUI::Preview::OptionType to libvgcode::EOptionType
// extern EOptionType convert(const Slic3r::GUI::Preview::OptionType& type);

// mapping from Slic3r::PrintEstimatedStatistics::ETimeMode to libvgcode::ETimeMode
extern ETimeMode convert(const Slic3r::PrintEstimatedStatistics::ETimeMode& mode);

// mapping from libvgcode::ETimeMode to Slic3r::PrintEstimatedStatistics::ETimeMode
extern Slic3r::PrintEstimatedStatistics::ETimeMode convert(const ETimeMode& mode);

// Which layers' infill closes every object printed on them, for GCodeInputData::layers_closed.
// Sparse infill closes an object on a layer. Solid infill closes it only where the object has no
// sparse infill left in the direction the band looks from: its top shells close it as seen from
// above, its bottom shells as seen from below, and an object with no sparse infill anywhere is
// closed by any infill. In between, infill combination leaves a layer with walls and the solid
// infill under its skins alone, and its middle open. A layer of support alone closes nothing. The
// objects are the ones the G-code labels its moves with; moves of no object count as one object
// of their own.
class LayersClosedByInfill
{
public:
    void add(uint32_t layer_id, int object_id, EGCodeExtrusionRole role, EMoveType type);
    // One entry per layer, bit 0 closed from above and bit 1 closed from below, 0 for a layer
    // nothing was added for. Empty when no move belonged to an object (objects not labelled in
    // the G-code): the viewer then judges the layers by their roles as a whole, which tells more
    // than one tally of the plate.
    std::vector<uint8_t> finish(size_t layers_count) const;

private:
    // per layer, per object: bit 0 for walls, bit 1 for sparse infill, bit 2 for other infill
    std::vector<std::map<int, uint8_t>> m_layers;
    // per object, the first and last layer with sparse infill
    std::map<int, std::pair<uint32_t, uint32_t>> m_sparse_span;
    bool m_labelled{ false };
};

// mapping from Slic3r::GCodeProcessorResult to libvgcode::GCodeInputData
extern GCodeInputData convert(const Slic3r::GCodeProcessorResult& result, const std::vector<std::string>& str_tool_colors,
    const std::vector<std::string>& str_color_print_colors, const Viewer& viewer,
    const Slic3r::Transform3d* belt_xform = nullptr);

// mapping from Slic3r::Print to libvgcode::GCodeInputData
extern GCodeInputData convert(const Slic3r::Print& print, const std::vector<std::string>& str_tool_colors,
    const std::vector<std::string>& str_color_print_colors, const std::vector<Slic3r::CustomGCode::Item>& color_print_values,
    size_t extruders_count);

} // namespace libvgcode

#endif // slic3r_LibVGCodeWrapper_hpp_
