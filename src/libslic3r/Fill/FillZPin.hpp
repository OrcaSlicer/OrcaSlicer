#pragma once
#include "Fill.hpp"
#include "../ExPolygon.hpp"
#include "../BoundingBox.hpp"
#include <unordered_map>

namespace Slic3r {

class Layer;

struct ZPinGrid {
    std::vector<Point>  centres;       // all hex grid positions (model space, scaled)
    coordf_t            diameter = 0;  // pin diameter = grid pitch
    int                 depth    = 0;  // max pin lifetime (layers)
    coordf_t            spacing  = 0;  // min distance between active pins
    ZPinStyle           style    = ZPinStyle::Spiral;
};

class FillZPin {
public:
    // Generate a hex-packed grid with pitch = diameter from model bounding box.
    static ZPinGrid compute_hex_grid(const BoundingBoxf3& bb, double diameter);

    // Core Bridson scheduler: assigns firing centres + void indices per layer.
    //
    // IMPORTANT: Must be called AFTER detect_surfaces_type() so that
    // fill_surfaces have classified surface types. The valid pre-pass
    // checks that pin positions overlap internal fill surfaces; calling
    // before surface classification will produce incorrect schedules.
    //
    // layers:        object layers (for lslices geometry + fill_surfaces types)
    // grid:          hex grid with diameter/depth/spacing
    // obj_cfg:       print object config (wall loops etc.)
    // region_cfg:    default region config (wall line widths)
    // schedule[lid]: output firing centres per layer
    // void_indices[lid]: output active cell indices per layer (for void subtraction)
    static void schedule_all_layers(
        const std::vector<Layer*>&              layers,
        const ZPinGrid&                         grid,
        const PrintObjectConfig&                obj_cfg,
        const PrintRegionConfig&                region_cfg,
        std::vector<std::vector<Point>>&        schedule,
        std::vector<std::vector<int>>&          void_indices);

    // Generate void circles for specific cell indices, clipped to infill_area.
    static ExPolygons void_polygons_for_indices(
        const ZPinGrid&         grid,
        const std::vector<int>& indices,
        const ExPolygons&       infill_area);

    // Legacy compat — generates voids for ALL centres (used nowhere now, kept for safety).
    static ExPolygons void_polygons(const ZPinGrid& grid, const ExPolygons& infill_area, size_t layer_id);

private:
    // Spatial hash for O(1) neighbor distance queries during Bridson placement.
    class SpatialHash {
    public:
        SpatialHash(coordf_t cell_size, const std::vector<Point>& points);
        // Return indices of points within 'radius' of 'p'.
        std::vector<int> query(const Point& p, coordf_t radius) const;
    private:
        coordf_t m_cell_size;
        const std::vector<Point>& m_points;
        std::unordered_map<int64_t, std::vector<int>> m_cells;
        int64_t cell_key(coord_t x, coord_t y) const;
    };
};

} // namespace Slic3r
