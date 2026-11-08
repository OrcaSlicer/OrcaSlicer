// UNLAYERED_MOD: nonplanar layers -- curved top shells on an ordinary 3-axis printer.
//
// Gentle top surfaces (slope up to nonplanar_max_angle) are printed as a stack of curved layers
// that follow the model surface, instead of a staircase of flat layers. Per object:
//
//  1. A height field of the top envelope is ray-cast from above on a grid. Cells whose surface
//     slope is at most the effective max angle form connected patches; a patch must span more than
//     about one layer height in Z (flat tops stay planar), must have material under it for the
//     whole shell, and is trimmed so that it never asks the nozzle to go deeper than
//     nonplanar_max_height below the layer it is printed in, nor next to printed material the
//     hotend envelope would hit.
//  2. In every layer, extrusions inside a patch footprint whose layer top is above S(x,y) - T + h/2
//     are cut away (paths are split at the boundary); the material below stays planar.
//  3. n = round(T / h) curved layers are added to the highest layer that intersects the patch.
//     Curved layer k follows Z = S(x,y) - (n-1-k) h; each has one edge loop half a bead inside the
//     footprint and solid lines inside it (top_surface_pattern on the top one, alternating below).
//     The first curved layer meters its flow to the real gap down to the planar top beneath it.
//
// G-code: shell paths are ExtrusionPath::nonplanar_shell; GCode lifts travel moves above the
// printed curved material and never wipes along it.
#pragma once

#include "Layer.hpp"

#include <vector>

namespace Slic3r {

class PrintObject;
class PrintRegionConfig;
class ConfigBase;
namespace sla { class IndexedMesh; }

struct NonplanarTopStats
{
    size_t patches        = 0;  // curved patches built
    size_t curved_layers  = 0;  // curved layers per patch (n), max over patches
    size_t shell_paths    = 0;  // extrusion paths added
    size_t clipped_layers = 0;  // planar layers that lost material to a patch
    size_t skipped_tops   = 0;  // tops whose band failed to fill: dropped, their planar top surface kept
    double max_shell_z    = 0;  // highest curved point, print frame
    double kept_planar_mm2 = 0; // area that met the clip rule but stayed planar: no top curved bead over it
    double ms_analysis    = 0;  // height field, patches, clearance
    double ms_shells      = 0;  // curved toolpaths
    double ms_clip        = 0;  // clipping the planar layers
};

// Curved layers per patch: nonplanar_layer_count, or 0 = the planar top shell's layer count.
int nonplanar_layer_count(const PrintRegionConfig &region, double layer_height);

// True if any printing region of the object asks for nonplanar layers.
bool nonplanar_top_requested(const PrintObject &object);

// nonplanar_max_angle, capped by the printer's hotend envelope (the smallest of the four
// nonplanar_hotend_angle_* sides) when enable_nonplanar is on and the envelope height is > 0.
double nonplanar_effective_max_angle(const PrintRegionConfig &region, const ConfigBase &printer);

// nonplanar_hold_back: the areas of this layer that a held island covers later (it pauses here and is
// printed after its top's shell), so they are not a real top: ironing skips them. Empty otherwise.
ExPolygons nonplanar_held_above(const Layer &layer);

// Builds the curved shells of one object and clips the planar material they replace. Idempotent:
// shells from a previous run are removed first. `mesh` and `ceilings` are exactly what
// PrintObject::contour_z() builds (slice frame, ground-level offset applied).
NonplanarTopStats make_nonplanar_top(PrintObject                               &object,
                                     const sla::IndexedMesh                    &mesh,
                                     const std::vector<Layer::ContourZCeiling> &ceilings);

// Test hook, null in production: called once per curved layer at the start of its gap fill. A test sets it to throw,
// to check that a fill failing inside a band costs only that band and not the slice.
extern void (*nonplanar_top_band_test_hook)();

} // namespace Slic3r
