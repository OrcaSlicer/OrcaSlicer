///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_GCODEINPUTDATA_HPP
#define VGCODE_GCODEINPUTDATA_HPP

#include "PathVertex.hpp"

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
    // Magma injection tube geometry, drawn by a separate custom pass (NOT part of the
    // toolpath `vertices`). Each manifold's sub-polylines (hub column, vent legs) are
    // concatenated; a Seam-type vertex separates sub-polylines so they don't join.
    // Tube points are Extrude type with role MagmaInjection, per-vertex width, and the
    // injection layer in `layer_id`.
    //
    std::vector<PathVertex> magma_vertices;
    //
    // Palette for extruders colors
    //
    Palette tools_colors;
    //
    // Palette for color print colors
    //
    Palette color_print_colors;
};

} // namespace libvgcode

#endif // VGCODE_BITSET_HPP
