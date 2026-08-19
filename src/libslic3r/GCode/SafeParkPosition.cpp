#include "SafeParkPosition.hpp"

#include <algorithm>

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

    // E mode and the current E position, captured before the normal retract so the absolute
    // restore below targets the position the writer will expect afterwards.
    const bool   relative_e = gcodegen.config().use_relative_e_distances.value;
    const double e_before_park = (gcodegen.writer().filament() != nullptr)
                                     ? gcodegen.writer().filament()->E() : 0.0;
    // Retraction feedrate: the filament's configured speed rather than a hardcoded 1800.
    const int park_e_feedrate = (gcodegen.writer().filament() != nullptr)
                                    ? std::max(60, gcodegen.writer().filament()->retract_speed() * 60)
                                    : 1800;

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
        //
        // Emitted raw because GCodeWriter::retract() no-ops here: Extruder::retract() computes
        // max(0, length - m_retracted) and we are already retracted by the full length. But the
        // writer is also the thing that knows the E mode -- Extruder::retract() zeroes m_E first
        // under use_relative_e_distances -- so emitting a raw delta is only correct in relative
        // mode. In ABSOLUTE mode "G1 E-2" means "move the E axis to absolute position -2", i.e.
        // an ~850mm retraction that ejects the filament. 14 shipped machine profiles run
        // absolute E, and this path is shared with ooze prevention (GCode.cpp), so it is not
        // gated behind Magma. Branch explicitly and command the absolute target instead.
        //
        // Either way E ends where it started (the symmetric restore below), so the writer's
        // tracked position stays correct without touching it.
        if (extra_retract > 0) {
            snprintf(buf, sizeof(buf), "G1 E%.4f F%d ; park extra retract\n",
                     park_extra_retract_e(relative_e, e_before_park, extra_retract),
                     park_e_feedrate);
            gcode += buf;
        }
    } else {
        // No safe XY — z-hop only as fallback.
        double park_z = layer_z + park_z_hop;
        gcode += gcodegen.writer().travel_to_z(park_z, z_comment);
    }

    gcode += gcodegen.writer().set_temperature(target_temp, true);

    if (park.position && extra_retract > 0) {
        snprintf(buf, sizeof(buf), "G1 E%.4f F%d ; park extra unretract\n",
                 park_extra_unretract_e(relative_e, e_before_park, extra_retract),
                 park_e_feedrate);
        gcode += buf;
    }

    return gcode;
}

} // namespace Slic3r
