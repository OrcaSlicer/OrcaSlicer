#include "FillZPin.hpp"
#include "../ClipperUtils.hpp"
#include <cmath>

namespace Slic3r {

ZPinGrid FillZPin::compute_grid(const BoundingBoxf3& bb, const PrintObjectConfig& cfg, double nozzle_diam) {
    ZPinGrid g;
    g.diameter             = cfg.z_pin_diameter.value;
    g.depth                = cfg.z_pin_depth.value;
    g.xy_stagger           = cfg.z_pin_stagger.value;
    g.layer_stagger        = cfg.z_pin_layer_stagger.value;
    g.layer_stagger_offset = cfg.z_pin_layer_stagger_offset.value;
    g.style                = cfg.z_pin_style.value;

    // Wall clamping - prevents pins from breaching perimeters
    const double max_r = g.diameter * 0.5 - nozzle_diam * 0.5;
    if (max_r > 0) g.diameter = std::min(g.diameter, max_r * 2.0);

    const double pitch = cfg.z_pin_spacing.value;
    if (pitch <= 0) return g;  // guard against infinite loop
    int col = 0;
    for (double x = bb.min.x() + pitch * 0.5; x < bb.max.x(); x += pitch, ++col) {
        double y_off = (g.xy_stagger && col % 2 == 1) ? pitch * 0.5 : 0.0;
        for (double y = bb.min.y() + pitch * 0.5 + y_off; y < bb.max.y(); y += pitch) {
            g.centres.emplace_back(scaled<coord_t>(Vec2d(x, y)));
            g.col_indices.emplace_back(col);
        }
    }
    return g;
}

ExPolygons FillZPin::void_polygons(const ZPinGrid& grid, const ExPolygons& infill_area, size_t /*layer_id*/) {
    const int N = 12;
    const coord_t r = scaled<coord_t>(grid.diameter * 0.5);
    ExPolygons circles;
    for (const Point& c : grid.centres) {
        Polygon poly;
        for (int i = 0; i < N; ++i) {
            double a = 2.0 * M_PI * i / N;
            poly.points.emplace_back(c.x() + coord_t(r * std::cos(a)),
                                     c.y() + coord_t(r * std::sin(a)));
        }
        circles.emplace_back(std::move(poly));
    }
    return intersection_ex(circles, infill_area);
}

} // namespace Slic3r
