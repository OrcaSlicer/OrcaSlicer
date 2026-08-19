#ifndef slic3r_GCode_SafeParkPosition_hpp_
#define slic3r_GCode_SafeParkPosition_hpp_

#include "../ExPolygon.hpp"
#include "../Point.hpp"

#include <optional>
#include <string>

namespace Slic3r {

class GCode;
class Layer;
class SupportLayer;

// Priority tier for safe park positions.
// Lower value = safer (ooze is less likely to affect print quality).
enum class ParkPriority {
    Empty       = 1,  // Outside cumulative object footprint
    Support     = 2,  // Over support on current layer
    SparseInfill = 3, // Over sparse infill on current layer
    SolidInfill = 4,  // Over solid internal infill on current layer
    None        = 5   // No safe XY position found
};

struct ParkResult {
    std::optional<Point> position;                    // nullopt = z-hop only (Priority None)
    ParkPriority         priority = ParkPriority::None;

    bool needs_z_hop() const { return priority >= ParkPriority::SparseInfill; }
};

// The E word for the park path's extra retract / unretract.
//
// This is the C0 bug reduced to arithmetic, and it is a header function so a test can reach
// it: under RELATIVE E a retraction is the delta (negative); under ABSOLUTE E the same text
// means "move the E axis TO that coordinate", so the delta must be turned into a target.
// Emitting -2.0 at E=850 on an absolute-E machine is an ~850mm retraction that pulls the
// filament past the drive gear. 14 shipped machine profiles run absolute E, and this path is
// shared with ooze prevention, so it is not gated behind Magma being enabled.
//
// Both are symmetric: the unretract returns E exactly to e_before_park, so the writer's
// tracked position stays correct without touching it.
inline double park_extra_retract_e(bool relative_e, double e_before_park, double extra)
{
    return relative_e ? -extra : e_before_park - extra;
}
inline double park_extra_unretract_e(bool relative_e, double e_before_park, double extra)
{
    return relative_e ? extra : e_before_park;
}

// Maintains cumulative XY object footprint during G-code generation.
// Multi-object aware: update() merges slices from all objects at each Z.
// Simplifies polygons (~1mm tolerance) since parking only needs coarse precision.
//
// Only the OBJECT footprint is tracked cumulatively. Support, infill type,
// etc. are checked on the current layer only — support from lower layers
// is empty space above, not a concern for parking.
class SafeParkPosition {
public:
    // Add object layer slices to the cumulative footprint. Call for EACH
    // object's layer at this Z (process_layer passes vector<LayerToPrint>).
    // Simplifies layer polygons before accumulating (~1mm tolerance).
    void update(const Layer* object_layer);

    // Find safe XY park position near nozzle_pos.
    // 5-tier priority: empty > support > sparse infill > solid infill > none.
    // Returns ParkResult with position and priority tier.
    ParkResult find_safe_position(
        const Layer* object_layer,
        const SupportLayer* support_layer,
        const Point& nozzle_pos,
        coord_t margin = scale_(2.0)) const;

    // Generate G-code to park, change temperature, and return.
    // Sequence: retract → (z-hop if needed) → XY travel → extra retract → M109 → unretract.
    static std::string park_and_set_temp(
        GCode& gcodegen,
        const ParkResult& park,
        double layer_z,
        double park_z_hop,
        double extra_retract,
        int target_temp,
        const char* z_comment,
        const char* xy_comment);

private:
    ExPolygons m_cumulative_footprint;  // simplified cumulative OBJECT area only

    // Find nearest centroid in safe regions to nozzle_pos.
    static std::optional<Point> nearest_centroid(
        const ExPolygons& safe_regions, const Point& nozzle_pos);

    static constexpr double SIMPLIFY_TOLERANCE = 1.0;  // mm
};

} // namespace Slic3r

#endif // slic3r_GCode_SafeParkPosition_hpp_
