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
