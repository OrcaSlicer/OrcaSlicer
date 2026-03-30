#include "FillZPin.hpp"
#include "../ClipperUtils.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../Surface.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <set>

namespace Slic3r {

// ─── Hex grid generation ───────────────────────────────────────────────────────

ZPinGrid FillZPin::compute_hex_grid(const BoundingBoxf3& bb, double diameter) {
    ZPinGrid g;
    g.diameter = diameter;
    if (diameter <= 0) return g;

    // Hex packing: rows separated by diameter * sqrt(3)/2, odd rows offset by diameter/2
    const double pitch  = diameter;
    const double row_h  = pitch * std::sqrt(3.0) / 2.0;

    int row = 0;
    for (double y = bb.min.y() + pitch * 0.5; y < bb.max.y(); y += row_h, ++row) {
        double x_off = (row % 2 == 1) ? pitch * 0.5 : 0.0;
        for (double x = bb.min.x() + pitch * 0.5 + x_off; x < bb.max.x(); x += pitch) {
            g.centres.emplace_back(scaled<coord_t>(Vec2d(x, y)));
        }
    }
    return g;
}

// ─── Spatial hash ──────────────────────────────────────────────────────────────

int64_t FillZPin::SpatialHash::cell_key(coord_t x, coord_t y) const {
    int64_t cx = static_cast<int64_t>(std::floor(static_cast<double>(x) / m_cell_size));
    int64_t cy = static_cast<int64_t>(std::floor(static_cast<double>(y) / m_cell_size));
    // Pack two 32-bit values into one 64-bit key
    return (cx << 32) ^ (cy & 0xFFFFFFFFLL);
}

FillZPin::SpatialHash::SpatialHash(coordf_t cell_size, const std::vector<Point>& points)
    : m_cell_size(cell_size), m_points(points) {
    for (int i = 0; i < static_cast<int>(points.size()); ++i) {
        m_cells[cell_key(points[i].x(), points[i].y())].push_back(i);
    }
}

std::vector<int> FillZPin::SpatialHash::query(const Point& p, coordf_t radius) const {
    std::vector<int> result;
    const coordf_t r2 = radius * radius;
    int64_t cx_min = static_cast<int64_t>(std::floor((static_cast<double>(p.x()) - radius) / m_cell_size));
    int64_t cx_max = static_cast<int64_t>(std::floor((static_cast<double>(p.x()) + radius) / m_cell_size));
    int64_t cy_min = static_cast<int64_t>(std::floor((static_cast<double>(p.y()) - radius) / m_cell_size));
    int64_t cy_max = static_cast<int64_t>(std::floor((static_cast<double>(p.y()) + radius) / m_cell_size));

    for (int64_t cx = cx_min; cx <= cx_max; ++cx) {
        for (int64_t cy = cy_min; cy <= cy_max; ++cy) {
            int64_t key = (cx << 32) ^ (cy & 0xFFFFFFFFLL);
            auto it = m_cells.find(key);
            if (it == m_cells.end()) continue;
            for (int idx : it->second) {
                const Point& q = m_points[idx];
                double dx = static_cast<double>(p.x() - q.x());
                double dy = static_cast<double>(p.y() - q.y());
                if (dx * dx + dy * dy <= r2)
                    result.push_back(idx);
            }
        }
    }
    return result;
}

// ─── Bridson scheduler ─────────────────────────────────────────────────────────
//
// ALGORITHM OVERVIEW — Bridson Poisson-disk Z-pin scheduler
// =========================================================
//
// Decides which hex-grid cells are active (accumulating void depth) on each
// layer, when they fire (cap), and where voids are subtracted from infill.
// Change this function AS A BLOCK — the steps are tightly coupled.
//
// Outputs
// -------
//   schedule[lid]      – centres that FIRE (deposit material) on this layer
//   void_indices[lid]  – cell indices that need voids on this layer
//
// Pre-passes
// ----------
//   1. valid[lid][i]:     centre i is inside eroded lslices on layer lid.
//   2. can_start[lid][i]: centre i is valid on layers lid..lid+max_depth+1,
//                          i.e. the cell can sustain a full pin cycle
//                          (1 floor + max_depth void + 1 fire layer).
//
// Per-layer loop
// --------------
//   Step 1 — Decrement remaining life of active pins.
//   Step 2 — Handle expiry:
//            (a) remaining == 0 → normal fire, add to schedule[lid].
//            (b) cell became invalid → silently deactivate, NO fire.
//                (Orphaned voids on previous layers get filled by infill.)
//   Step 3 — Record void_indices = currently-active cells (BEFORE placement).
//   Step 4 — Collect candidates using can_start (not just valid).
//            Cooldown: last_expired + 1 < lid (2-layer gap).
//   Step 5 — Greedy Bridson placement, remaining = max_depth + 1.
//
// Lifecycle of a pin (max_depth = D)
// ----------------------------------
//   Layer A:     activate (step 5), remaining = D+1. No void. [floor]
//   Layer A+1:   remaining D. Active → void_indices.          [void 1]
//   ...
//   Layer A+D:   remaining 1. Active → void_indices.          [void D]
//   Layer A+D+1: remaining 0. Step 2a → FIRE. Not in voids.  [cap]
//   Layer A+D+2: cooldown, not eligible.                      [cover]
//   Layer A+D+3: eligible for reactivation.
//
// Invariants
// ----------
//   - Void spans exactly max_depth layers.
//   - Fire only on full-depth pins (remaining countdown reached 0).
//   - 2-layer cooldown: fire layer + 1 cover layer before reactivation.
//   - Pins near geometry edges won't start (can_start lookahead).
//

void FillZPin::schedule_all_layers(
    const std::vector<Layer*>&              layers,
    const ZPinGrid&                         grid,
    const PrintObjectConfig&                obj_cfg,
    const PrintRegionConfig&                region_cfg,
    std::vector<std::vector<Point>>&        schedule,
    std::vector<std::vector<int>>&          void_indices)
{
    const int N = static_cast<int>(grid.centres.size());
    const int L = static_cast<int>(layers.size());
    if (N == 0 || L == 0 || grid.depth <= 0) {
        schedule.assign(L, {});
        void_indices.assign(L, {});
        return;
    }

    const coordf_t spacing_scaled = scaled<coordf_t>(grid.spacing);
    const coordf_t pin_radius     = scaled<coordf_t>(grid.diameter * 0.5);

    // Wall thickness for zone erosion: wall_loops × outer_wall_line_width
    const int    wall_loops = region_cfg.wall_loops.value;
    const double wall_lw    = region_cfg.outer_wall_line_width.value > 0
                            ? region_cfg.outer_wall_line_width.value
                            : 0.4; // fallback nozzle diameter
    const coordf_t wall_thickness = scaled<coordf_t>(wall_loops * wall_lw);
    const int max_depth = grid.depth;

    // ── Pre-pass 1: valid[lid][i] — centre inside eroded zone AND
    //    inside an internal fill surface (stInternal / stInternalSolid /
    //    stInternalVoid).  This prevents scheduling voids on layers where
    //    the fill is stTop / stBottom / stBottomBridge — the void
    //    subtraction skips those surface types so the void would never
    //    actually be created.
    std::vector<std::vector<bool>> valid(L, std::vector<bool>(N, false));
    for (int lid = 0; lid < L; ++lid) {
        const Layer* layer = layers[lid];
        if (!layer || layer->lslices.empty()) continue;
        ExPolygons zone;
        try {
            zone = offset_ex(layer->lslices, -(wall_thickness + pin_radius));
        } catch (...) {
            continue;
        }
        if (zone.empty()) continue;

        // Modifier-mesh suppression: subtract regions with enable_z_pins=false
        ExPolygons no_pin_zone;
        for (const LayerRegion* lr : layer->regions()) {
            if (!lr->region().config().enable_z_pins.value) {
                for (const Surface& s : lr->slices.surfaces)
                    no_pin_zone.push_back(s.expolygon);
            }
        }
        if (!no_pin_zone.empty()) {
            try {
                zone = diff_ex(zone, no_pin_zone);
            } catch (...) { /* keep original zone */ }
            if (zone.empty()) continue;
        }

        // Collect union of internal fill surfaces across all regions.
        // Only these surface types support void subtraction.
        ExPolygons internal_fill;
        for (const LayerRegion* lr : layer->regions()) {
            for (const Surface& s : lr->fill_surfaces.surfaces) {
                if (s.surface_type == stInternal ||
                    s.surface_type == stInternalSolid ||
                    s.surface_type == stInternalVoid) {
                    internal_fill.push_back(s.expolygon);
                }
            }
        }

        for (int i = 0; i < N; ++i) {
            const Point& c = grid.centres[i];
            // Check 1: inside eroded geometry
            bool geom_ok = false;
            for (const ExPolygon& ep : zone) {
                if (ep.contains(c)) { geom_ok = true; break; }
            }
            if (!geom_ok) continue;
            // Check 2: inside internal fill surface (not top/bottom shell)
            bool fill_ok = false;
            for (const ExPolygon& ep : internal_fill) {
                if (ep.contains(c)) { fill_ok = true; break; }
            }
            valid[lid][i] = fill_ok;
        }
    }

    // ── Pre-pass 2: can_start[lid][i] — valid for full pin cycle ──
    // A pin activated on layer lid needs valid on lid .. lid+max_depth+1
    // (1 floor + max_depth voids + 1 fire layer = max_depth+2 total).
    // Computed via suffix counting for O(N*L).
    const int cycle_len = max_depth + 2;
    std::vector<std::vector<bool>> can_start(L, std::vector<bool>(N, false));
    for (int i = 0; i < N; ++i) {
        int run = 0; // consecutive valid layers counting backward from lid
        for (int lid = L - 1; lid >= 0; --lid) {
            run = valid[lid][i] ? run + 1 : 0;
            can_start[lid][i] = (run >= cycle_len);
        }
    }

    // Build spatial hash for Bridson neighbor queries
    SpatialHash hash(spacing_scaled, grid.centres);

    // Per-cell state
    struct CellState {
        bool active       = false;
        int  remaining    = 0;     // layers of life left (counts down to 0 → fire)
        int  last_expired = -999;  // layer at which this cell last fired/deactivated
    };
    std::vector<CellState> cells(N);

    schedule.assign(L, {});
    void_indices.assign(L, {});

    for (int lid = 0; lid < L; ++lid) {
        // Step 1 — Decrement remaining life of all active pins.
        for (int i = 0; i < N; ++i) {
            if (cells[i].active)
                cells[i].remaining--;
        }

        // Step 2 — Expire or deactivate.
        std::vector<Point> firing;
        for (int i = 0; i < N; ++i) {
            if (!cells[i].active) continue;
            if (cells[i].remaining <= 0) {
                // Normal expiry: full void channel built → fire (cap).
                firing.push_back(grid.centres[i]);
                cells[i].active = false;
                cells[i].last_expired = lid;
            } else if (!valid[lid][i]) {
                // Geometry changed mid-cycle: silently deactivate, NO fire.
                // The orphaned voids on previous layers will be filled by
                // infill on subsequent layers. Firing here would deposit
                // material into an incomplete channel.
                cells[i].active = false;
                cells[i].last_expired = lid;
            }
        }
        schedule[lid] = std::move(firing);

        // Step 3 — Record void map: all cells still active after expiry.
        //          BEFORE new placement, so fired/deactivated cells are excluded
        //          and newly-placed cells won't appear until the next layer.
        {
            std::vector<int> active_indices;
            for (int i = 0; i < N; ++i) {
                if (cells[i].active)
                    active_indices.push_back(i);
            }
            void_indices[lid] = std::move(active_indices);
        }

        // Step 4 — Collect candidates: can_start (full-cycle lookahead),
        //          inactive, and cooled down (2-layer gap since last fire).
        std::vector<int> candidates;
        for (int i = 0; i < N; ++i) {
            if (can_start[lid][i] && !cells[i].active && cells[i].last_expired + 1 < lid)
                candidates.push_back(i);
        }
        // Sort by idle time descending — cells idle longest activate first,
        // naturally staggering so neighbors expire on different layers.
        std::sort(candidates.begin(), candidates.end(), [&](int a, int b) {
            return (lid - cells[a].last_expired) > (lid - cells[b].last_expired);
        });

        // Step 5 — Greedy Bridson placement.
        // remaining = max_depth + 1: activation layer has no void (step 3
        // precedes step 5), then max_depth void layers, then fire.
        std::vector<bool> active_mask(N, false);
        for (int i = 0; i < N; ++i) {
            if (cells[i].active)
                active_mask[i] = true;
        }

        for (int ci : candidates) {
            auto neighbors = hash.query(grid.centres[ci], spacing_scaled);
            bool too_close = false;
            for (int ni : neighbors) {
                if (ni != ci && active_mask[ni]) {
                    too_close = true;
                    break;
                }
            }
            if (too_close) continue;

            cells[ci].active    = true;
            cells[ci].remaining = max_depth + 1;
            active_mask[ci]     = true;
        }
    }
}

// ─── Void polygon generation ───────────────────────────────────────────────────

static ExPolygons make_circles(const std::vector<Point>& centres, coord_t radius, int n_sides = 32) {
    ExPolygons circles;
    circles.reserve(centres.size());
    for (const Point& c : centres) {
        Polygon poly;
        poly.points.reserve(n_sides);
        for (int i = 0; i < n_sides; ++i) {
            double a = 2.0 * M_PI * i / n_sides;
            poly.points.emplace_back(c.x() + coord_t(radius * std::cos(a)),
                                     c.y() + coord_t(radius * std::sin(a)));
        }
        circles.emplace_back(std::move(poly));
    }
    return circles;
}

ExPolygons FillZPin::void_polygons_for_indices(
    const ZPinGrid&         grid,
    const std::vector<int>& indices,
    const ExPolygons&       infill_area)
{
    if (indices.empty() || infill_area.empty()) return {};

    const coord_t r = scaled<coord_t>(grid.diameter * 0.5);
    std::vector<Point> pts;
    pts.reserve(indices.size());
    for (int idx : indices)
        pts.push_back(grid.centres[idx]);

    ExPolygons circles = make_circles(pts, r);
    try {
        return intersection_ex(circles, infill_area);
    } catch (...) {
        return {};
    }
}

ExPolygons FillZPin::void_polygons(const ZPinGrid& grid, const ExPolygons& infill_area, size_t /*layer_id*/) {
    if (grid.centres.empty() || infill_area.empty()) return {};
    const coord_t r = scaled<coord_t>(grid.diameter * 0.5);
    ExPolygons circles = make_circles(grid.centres, r);
    try {
        return intersection_ex(circles, infill_area);
    } catch (...) {
        return {};
    }
}

} // namespace Slic3r
