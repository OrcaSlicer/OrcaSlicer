#pragma once

#include <vector>

namespace Slic3r {

class ConfigBase;
enum class WipeTowerType;

// Pre-slice footprint of the wipe tower, shared by validation (Print), the GUI's placement
// clamp/preview/arrange and the CLI placement. The arithmetic is shared; the inputs below are
// not, so a change to how one caller derives them has to be mirrored in the others.
struct WipeTowerFootprint
{
    double width      = 0.; // effective width: equals depth for a rib wall, which squares the tower
    double depth      = 0.; // 0 when these inputs imply no tower
    double height     = 0.; // tallest object; drives the stability floor and the auto brim
    double brim_width = 0.; // printed width: auto (-1) resolved by height, laid in whole loops
};

// Which planner builds the tower: Bambu Lab printers always get Type1, the rest follow
// wipe_tower_type. The rule Print::wipe_tower_type() and the CLI apply, read off the config so
// the GUI and CLI placement can resolve it without a Print.
WipeTowerType resolve_wipe_tower_type(const ConfigBase &config);

// filament_ids:  0-based filaments purged on the plate. The config cannot see custom G-code tool
//                changes, so ids derived from the model must include them
//                (Print::extruders(true)) or a real tower is sized as if it were never built.
// layer_height:  thinnest layer the objects are sliced at. The first layer is folded in here.
// any_raft:      any object on the plate prints a raft, which puts the tower on every layer
//                below it. Caller-resolved: raft_layers is a PrintObjectConfig key, absent
//                from Print's config and overridable per object.
WipeTowerFootprint estimate_wipe_tower_footprint(const ConfigBase                &config,
                                                 WipeTowerType                    tower_type,
                                                 const std::vector<unsigned int> &filament_ids,
                                                 double                           layer_height,
                                                 double                           max_object_height,
                                                 bool                             any_raft);

} // namespace Slic3r
