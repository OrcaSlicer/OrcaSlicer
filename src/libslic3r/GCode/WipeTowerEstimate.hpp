#pragma once

#include <cstddef>

namespace Slic3r {

class ConfigBase;

// Pre-slice footprint of the wipe tower, shared by validation (Print), the GUI's placement
// clamp/preview/arrange and the CLI placement. The arithmetic is shared; the inputs below are
// not, so a change to how one caller derives them has to be mirrored in the others.
struct WipeTowerFootprint
{
    double width      = 0.; // effective width: equals depth for a rib wall, which squares the tower
    double depth      = 0.; // 0 when these inputs imply no tower
    double height     = 0.; // tallest object; drives the stability floor and the auto brim
    double brim_width = 0.; // configured width, auto (-1) resolved by height
};

// filaments_cnt: filaments purged on the plate. The config cannot see custom G-code tool
//                changes, so a count derived from the model must include them
//                (Print::extruders(true)) or a real tower is sized as if it were never built.
// layer_height:  thinnest layer the tower will be planned at.
// any_raft:      any object on the plate prints a raft, which puts the tower on every layer
//                below it. Caller-resolved: raft_layers is a PrintObjectConfig key, absent
//                from Print's config and overridable per object.
WipeTowerFootprint estimate_wipe_tower_footprint(const ConfigBase &config, size_t filaments_cnt, double layer_height, double max_object_height, bool any_raft);

} // namespace Slic3r
