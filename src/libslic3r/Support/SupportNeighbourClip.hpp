#pragma once

// UNLAYERED_MOD -- keep every object's support out of the OTHER objects on the plate.
//
// Support (normal, tree, organic, and the cut-plane cascade alike) is generated one PrintObject at
// a time and only ever avoids its own object: nothing in Support/* looks at the rest of the plate.
// Stock OrcaSlicer / PrusaSlicer behave the same, and the conflict checker only reports it. So an
// overhang whose support column falls on a neighbouring part printed that column straight through
// the neighbour (a user report: "the support clipped into the other part").
//
// clip_supports_to_neighbours() runs once all supports exist and cuts each object's finished
// support extrusions (and its support islands) away from every other object placement at the
// same height, grown by the object's support XY distance plus half the support line width, and
// by its support Z distances above and below. It works in plate coordinates (instance shifts),
// matches the neighbour's layers by Z interval, so objects with different layer heights work,
// and prefilters everything by bounding box, so a plate whose objects are apart costs one box
// test per pair.
//
// It is re-run on every Print::process(): moving an object only invalidates the skirt/brim and
// G-code, not the support, so each support layer keeps its unclipped extrusions the first time
// the clip touches it and is restored from them before the next clip.
//
// Scope: objects sliced in the plate frame -- no layer rotation, not on a belt, not a tilted
// cut-plane segment (root segments qualify). Their 2D slices are plate XY. Tilted objects neither
// get clipped nor act as obstacles; their layers are not plate XY.

#include <functional>

namespace Slic3r {

class Print;

// Returns the number of support layers whose extrusions the clip changed in this run.
size_t clip_supports_to_neighbours(Print &print, const std::function<void()> &throw_if_canceled);

} // namespace Slic3r
