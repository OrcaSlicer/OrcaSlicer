///|/ Wave overhang generator interface (algorithm abstraction).
///|/
///|/ Released under the terms of the AGPLv3 or higher.
///|/
#ifndef slic3r_WaveOverhangs_IGenerator_hpp_
#define slic3r_WaveOverhangs_IGenerator_hpp_

#include <tuple>
#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::WaveOverhangs {

// Ring spacing mode (uniform constant step vs progressively growing step).
enum class SpacingMode { Uniform, Progressive };

// Inter-ring seam/direction mode (see wave_overhang_seam_mode in PrintConfig).
enum class SeamMode { Alternating, Aligned, Random };

struct CommonParams {
    int         perimeter_count        = 1;
    int         additional_shell_count = 0;
    double      line_spacing           = 0.35;
    double      line_width             = 0.4;
    Flow        overhang_flow;
    double      scaled_resolution      = 1.0;
    SpacingMode spacing_mode           = SpacingMode::Uniform;
    SeamMode    seam_mode              = SeamMode::Alternating;
    double      min_length_mm          = 0.0;   // mm; skip overhangs whose contour length is below this.
    int         max_iterations         = 0;     // 0 = unlimited; safety cap on main loop (wavefronts per region).
    // Wavefront-propagation tunables.
    double      perimeter_overlap      = 0.1;   // mm; extend wave propagation toward perimeters.
    double      minimum_wave_width     = 0.7;   // mm; split wave region when a neck is narrower than this.
    WaveOverhangPattern pattern        = WaveOverhangPattern::Smart;
    double      min_new_area           = 0.01;  // mm^2; early-termination threshold on new-area growth.
    bool        use_instead_of_bridges = false; // when true, wave over flat bridgeable spans too.
    // Corner-aware spacing taper: densify line spacing near sharp overhang corners
    // so short cantilevered wave lines have neighbours to fuse with. The master
    // gate is `corner_taper_enable`; when false the main propagation runs
    // verbatim regardless of the other three values. The downstream generator
    // ALSO refuses to engage when line_spacing_corner is 0 or >= line_spacing,
    // or when corner_taper_distance is 0, so a partially-configured taper is
    // a no-op rather than a silent surprise.
    bool        corner_taper_enable    = false;
    double      line_spacing_corner    = 0.0;   // mm; 0 or >= line_spacing means taper off.
    double      corner_taper_distance  = 0.0;   // mm; radius of corner influence. 0 = taper off.
    double      corner_angle_threshold = 90.0;  // degrees; interior angle below this is a corner.
};

struct GenerateResult {
    std::vector<ExtrusionPaths> paths;     // per-region
    Polygons                    residual;  // covered area (subtracted from infill upstream)
};

class IGenerator {
public:
    virtual ~IGenerator() = default;
    virtual GenerateResult generate(const ExPolygons   &overhang_area,
                                    const Polygons     &lower_slices_polygons,
                                    const CommonParams &params) = 0;
    virtual const char *name() const = 0;
};

} // namespace Slic3r::WaveOverhangs

#endif
