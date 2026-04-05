#include "SafeParkPosition.hpp"

#include "../GCode.hpp"
#include "../Layer.hpp"
#include "../ClipperUtils.hpp"
#include "../BoundingBox.hpp"
#include "../Surface.hpp"

#include <limits>
#include <cstdio>

#include <boost/log/trivial.hpp>

namespace Slic3r {

// ============================================================================
// SafeParkPosition — update
// ============================================================================

void SafeParkPosition::update(const Layer* object_layer)
{
    if (!object_layer || object_layer->lslices.empty())
        return;
    // Simplify polygons before accumulating (same pattern as TreeSupport).
    // 1mm tolerance prevents vertex explosion across hundreds of layers.
    ExPolygons simplified;
    for (const ExPolygon& poly : object_layer->lslices)
        poly.simplify(scale_(SIMPLIFY_TOLERANCE), &simplified);
    m_cumulative_footprint = union_ex(m_cumulative_footprint, simplified);
}

// ============================================================================
// SafeParkPosition — nearest_centroid
// ============================================================================

std::optional<Point> SafeParkPosition::nearest_centroid(
    const ExPolygons& safe_regions, const Point& nozzle_pos)
{
    Point best;
    double best_dist2 = std::numeric_limits<double>::max();
    bool found = false;

    for (const ExPolygon& ep : safe_regions) {
        Point centroid = ep.contour.centroid();
        double d2 = (centroid - nozzle_pos).cast<double>().squaredNorm();
        if (d2 < best_dist2) {
            best_dist2 = d2;
            best = centroid;
            found = true;
        }
    }

    return found ? std::optional<Point>(best) : std::nullopt;
}

// ============================================================================
// SafeParkPosition — find_safe_position
// ============================================================================

ParkResult SafeParkPosition::find_safe_position(
    const Layer* object_layer,
    const SupportLayer* support_layer,
    const Point& nozzle_pos,
    coord_t margin) const
{
    if (m_cumulative_footprint.empty())
        return {};

    BoundingBox bbox = get_extents(m_cumulative_footprint);
    bbox.offset(margin);

    // Priority 1: Empty space — outside cumulative object footprint.
    // Ooze falls into air (or onto support/brim below, removable).
    {
        ExPolygons empty = diff_ex(bbox.polygon(), m_cumulative_footprint);
        ExPolygons safe = offset_ex(empty, -margin);
        auto pos = nearest_centroid(safe, nozzle_pos);
        if (pos)
            return { pos, ParkPriority::Empty };
    }

    // Priority 2: Over support on current layer.
    // Temporary material, removed after print.
    if (support_layer && !support_layer->support_islands.empty()) {
        ExPolygons safe = offset_ex(support_layer->support_islands, -margin);
        auto pos = nearest_centroid(safe, nozzle_pos);
        if (pos)
            return { pos, ParkPriority::Support };
    }

    // Priority 3: Over sparse infill on current layer.
    // Has gaps — ooze partially falls through, internal.
    if (object_layer) {
        ExPolygons sparse_regions;
        for (const LayerRegion* layerm : object_layer->regions()) {
            for (const Surface& surface : layerm->fill_surfaces.surfaces) {
                if (surface.is_sparse_fill() ||
                    surface.surface_type == stInternalVoid)
                    sparse_regions.push_back(surface.expolygon);
            }
        }
        if (!sparse_regions.empty()) {
            sparse_regions = union_ex(sparse_regions);
            ExPolygons safe = offset_ex(sparse_regions, -margin);
            auto pos = nearest_centroid(safe, nozzle_pos);
            if (pos)
                return { pos, ParkPriority::SparseInfill };
        }
    }

    // Priority 4: Over solid internal infill on current layer.
    // Flat surface but covered by top layers later.
    if (object_layer) {
        ExPolygons solid_regions;
        for (const LayerRegion* layerm : object_layer->regions()) {
            for (const Surface& surface : layerm->fill_surfaces.surfaces) {
                if (surface.surface_type == stInternalSolid ||
                    surface.surface_type == stInternalBridge)
                    solid_regions.push_back(surface.expolygon);
            }
        }
        if (!solid_regions.empty()) {
            solid_regions = union_ex(solid_regions);
            ExPolygons safe = offset_ex(solid_regions, -margin);
            auto pos = nearest_centroid(safe, nozzle_pos);
            if (pos)
                return { pos, ParkPriority::SolidInfill };
        }
    }

    // Priority 5: No safe XY position found — caller should z-hop only.
    return {};
}

// ============================================================================
// SafeParkPosition — park_and_set_temp
// ============================================================================

std::string SafeParkPosition::park_and_set_temp(
    GCode& gcodegen,
    const ParkResult& park,
    double layer_z,
    double park_z_hop,
    double extra_retract,
    int target_temp,
    const char* z_comment,
    const char* xy_comment)
{
    std::string gcode;
    char buf[256];

    gcode += gcodegen.retract(false, false);

    if (park.position) {
        // Z-hop only when parking over printed material (Priority 3+).
        if (park.needs_z_hop()) {
            double park_z = layer_z + park_z_hop;
            gcode += gcodegen.writer().travel_to_z(park_z, z_comment);
        }

        gcode += gcodegen.writer().travel_to_xy(
            gcodegen.point_to_gcode(*park.position), xy_comment);

        // Extra retraction beyond the normal retract (already performed above).
        // Uses raw G1 E because writer().retract() would no-op (already retracted).
        // The symmetric unretract below restores E to the position the state machine expects.
        if (extra_retract > 0) {
            snprintf(buf, sizeof(buf), "G1 E-%.4f F1800 ; park extra retract\n", extra_retract);
            gcode += buf;
        }
    } else {
        // No safe XY — z-hop only as fallback.
        double park_z = layer_z + park_z_hop;
        gcode += gcodegen.writer().travel_to_z(park_z, z_comment);
    }

    gcode += gcodegen.writer().set_temperature(target_temp, true);

    if (park.position && extra_retract > 0) {
        snprintf(buf, sizeof(buf), "G1 E%.4f F1800 ; park extra unretract\n", extra_retract);
        gcode += buf;
    }

    return gcode;
}

} // namespace Slic3r
