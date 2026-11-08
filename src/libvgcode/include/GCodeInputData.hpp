///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_GCODEINPUTDATA_HPP
#define VGCODE_GCODEINPUTDATA_HPP

#include "PathVertex.hpp"
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
    // UNLAYERED_MOD: palette of unit tool axes referenced by PathVertex::tool_axis_id.
    // Index 0 must exist and must be +Z. If left empty the viewer supplies { +Z }, so a producer
    // that does not know about tilted layers keeps working unchanged.
    //
    std::vector<Vec3> tool_axes;
    //
    // Declarative cut planes: palette for the section view, indexed by PathVertex::cut_segment_id.
    // Supplied by the caller rather than hardcoded here so it stays identical to the colours the
    // Prepare tab's chunk preview uses (Slic3r::GUI::cut_segment_color).
    //
    Palette cut_segment_colors;
};

} // namespace libvgcode

#endif // VGCODE_BITSET_HPP
