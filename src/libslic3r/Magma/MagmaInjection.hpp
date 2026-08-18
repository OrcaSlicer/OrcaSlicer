#ifndef slic3r_MagmaInjection_hpp_
#define slic3r_MagmaInjection_hpp_

#include "../Point.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class GCode;

namespace magma {

class MagmaTubeMap;

struct InjectionPoint {
    Vec2d  position;      // XY center of injection cell (mm, unscaled)
    double volume_mm3;    // volume to inject (after fill_factor)
    int    pair_index;    // index into tube map pairs
    int    start_layer;   // pair_start_layer (for computing z_bot)
    int    window_center_layer;  // center layer of window gap (for visualization)
};

// Collect injection points for tubes whose cap layer == layer_id.
// Generate injection + ironing G-code for one object's points.
// Temperature management and fan markers are handled by the caller
// (GCode.cpp injection phase) — this function is the per-object core.
std::string generate_injection_gcode(
    GCode& gcodegen,
    const MagmaTubeMap& tube_map,
    const std::vector<InjectionPoint>& points,
    double layer_z,
    double actual_layer_height);

// Park at a safe position and change nozzle temperature.
// Exposed so GCode.cpp can call it for the global injection phase
// heat-up / cool-down (once per layer, not per object).
std::string park_and_set_temp(
    GCode& gcodegen,
    bool park_enabled,
    double layer_z,
    double park_z_hop,
    double extra_retract,
    int target_temp,
    const char* z_comment,
    const char* xy_comment);

} // namespace magma
} // namespace Slic3r

#endif // slic3r_MagmaInjection_hpp_
