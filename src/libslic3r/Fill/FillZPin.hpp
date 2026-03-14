#pragma once
#include "Fill.hpp"
#include "../ExPolygon.hpp"

namespace Slic3r {

struct ZPinGrid {
    std::vector<Point>  centres;
    std::vector<int>    col_indices;         // column index for each centre point
    coordf_t            diameter             = 0.0;
    int                 depth               = 0;
    bool                xy_stagger          = false; // offset odd columns by pitch/2 in Y
    bool                layer_stagger       = false; // fire odd columns at a layer offset
    int                 layer_stagger_offset = 0;   // layers to shift odd columns (0 = depth/2)
    ZPinStyle           style               = ZPinStyle::Spiral;
};

class FillZPin {
public:
    static ZPinGrid    compute_grid(const BoundingBoxf3& bb, const PrintObjectConfig& cfg, double nozzle_diam);
    static ExPolygons  void_polygons(const ZPinGrid& grid, const ExPolygons& infill_area, size_t layer_id);
};

} // namespace Slic3r
