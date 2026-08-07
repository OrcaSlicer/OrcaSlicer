#pragma once

#include <string>

namespace Slic3r {

// Experimental "Dynamic Composite Objects" print sequencing.
//
// A normal (by-layer) print advances every island of every object by one layer
// at a time. For composite objects such as a letter "H" standing upright, the
// two legs are two separate towers that are joined by a bridge only at a later
// height, so the nozzle hops from one leg to the other on every layer.
//
// This post-processing pass regroups the exported G-code: within a run of
// consecutive layers that share the same set of islands (the legs of the "H"),
// every layer of one island is emitted before the first layer of the next
// island is started. The bridge layer (where the islands merge into one) and
// every layer whose island structure differs are left in the original
// layer-by-layer order.
//
// The regrouping is only applied when it is safe for the moving hardware:
//   - if two islands are closer to each other than `clearance_radius` (mm) and
//   - the height difference reached between them during the run exceeds
//     `clearance_height` (mm),
// then the nozzle or gantry could collide with the taller island while the
// shorter one is being printed, so the whole feature is disabled and the input
// G-code is returned unchanged.
//
// The pass is best-effort: on any unrecognized input (multi-extruder prints,
// object exclusion markers, missing layer markers, parse or verification
// failures) the original G-code is returned unchanged, never a corrupted file.
std::string dynamic_composite_objects_process(const std::string& gcode,
                                              double clearance_radius,
                                              double clearance_height);

} // namespace Slic3r
