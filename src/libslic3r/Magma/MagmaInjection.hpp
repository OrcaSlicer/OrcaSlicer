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

// Ways an injection comes out weaker than the preview promised.
//
// Each of these leaves the lattice printed and some part of it unfilled, so the part reads
// as the feature merely underperforming rather than as something having gone wrong. They
// were all BOOST_LOG_TRIVIAL only, which does not survive the run -- the operator reads the
// G-code and the part, not the slicer's debug log. Counted here so the export can put them
// in both.
struct InjectionDiagnostics {
    int tubes_no_volume        = 0;  // measured cavity was empty; that U-tube prints hollow
    int layers_no_fill_factor  = 0;  // fill factor <= 0; every injection on the layer dropped
    int injections_invented_speed = 0;  // no volumetric speed to inject at; a rate nobody chose
    int injections_unlabelled  = 0;  // outside any exclude-object block; runs even if cancelled

    bool any() const
    {
        return tubes_no_volume || layers_no_fill_factor
            || injections_invented_speed || injections_unlabelled;
    }
    void merge(const InjectionDiagnostics &o)
    {
        tubes_no_volume            += o.tubes_no_volume;
        layers_no_fill_factor      += o.layers_no_fill_factor;
        injections_invented_speed  += o.injections_invented_speed;
        injections_unlabelled      += o.injections_unlabelled;
    }
    // One line per problem that actually occurred; empty when none did. Localized.
    std::vector<std::string> messages() const;
};

// Collect injection points for tubes whose cap layer == layer_id.
// Generate injection + ironing G-code for one object's points.
// Temperature management and fan markers are handled by the caller
// (GCode.cpp injection phase) — this function is the per-object core.
//
// `diag` accumulates across calls; the caller reports it once at the end of the export.
std::string generate_injection_gcode(
    GCode& gcodegen,
    const MagmaTubeMap& tube_map,
    const std::vector<InjectionPoint>& points,
    double layer_z,
    double actual_layer_height,
    InjectionDiagnostics& diag);

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
