///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_GCODEINPUTDATA_HPP
#define VGCODE_GCODEINPUTDATA_HPP

#include "PathVertex.hpp"
#include <cstdint>
#include <vector>
#include "Types.hpp"

namespace libvgcode {

struct GCodeInputData
{
    //
    // Whether or not the gcode was generated with spiral vase mode enabled.
    // Required to properly detect fictitious layer changes when spiral vase mode is enabled.
    //
    bool spiral_vase_mode{ false };
    //
    // List of path vertices (gcode moves)
    // See: PathVertex
    //
    std::vector<PathVertex> vertices;
    //
    // Palette for extruders colors
    //
    PaletteWithAlpha tools_colors;
    //
    // Palette for color print colors
    //
    PaletteWithAlpha color_print_colors;
    //
    // Optional, one entry per layer: whether the layer's infill closes every object printed on
    // it, so that the end bands of the reduced set may stop there, see
    // Viewer::get_reduced_detail_whole_layers(). Bit 0: closed as seen from above, for the band
    // at the top of the range; bit 1: closed as seen from below, for the band at the bottom. An
    // object's top shells close it from above and its bottom shells from below. Left empty, or
    // of another size than the layer count, the viewer judges each layer from the roles of the
    // whole layer instead.
    //
    std::vector<uint8_t> layers_closed;
};

} // namespace libvgcode

#endif // VGCODE_BITSET_HPP
