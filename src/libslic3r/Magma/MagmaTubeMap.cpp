#include "MagmaTubeMap.hpp"
#include "MagmaTubeSolver.hpp"

#include "../Layer.hpp"
#include "../Print.hpp"
#include "../Slicing.hpp"
#include "../ClipperUtils.hpp"
#include "../Polygon.hpp"
#include "../ExPolygon.hpp"
#include "../Surface.hpp"
#include "../I18N.hpp"
#include "../format.hpp"

#define L(s) Slic3r::I18N::translate(s)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <queue>
#include <set>
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace magma {

// ============================================================================
// CellPresence
// ============================================================================

bool CellPresence::present(int layer_id) const
{
    if (layer_id < first_layer || layer_id > last_layer)
        return false;
    int idx = layer_id - first_layer;
    return idx >= 0 && idx < int(layers.size()) && layers[idx];
}

double CellPresence::area(int layer_id) const
{
    if (layer_id < first_layer || layer_id > last_layer)
        return 0.0;
    int idx = layer_id - first_layer;
    if (idx < 0 || idx >= int(areas.size()))
        return 0.0;
    return areas[idx];
}

double CellPresence::distance(int layer_id) const
{
    if (layer_id < first_layer || layer_id > last_layer)
        return 0.0;
    int idx = layer_id - first_layer;
    if (idx < 0 || idx >= int(distances.size()))
        return 0.0;
    return distances[idx];
}

void CellPresence::mark_present(int layer_id, double area_val, double dist_val)
{
    if (first_layer == INT_MAX) {
        // First time: initialize
        first_layer = layer_id;
        last_layer  = layer_id;
        layers.assign(1, true);
        areas.assign(1, area_val);
        distances.assign(1, dist_val);
        return;
    }

    // Expand range if needed
    if (layer_id < first_layer) {
        int extend = first_layer - layer_id;
        layers.insert(layers.begin(), extend, false);
        areas.insert(areas.begin(), extend, 0.0);
        distances.insert(distances.begin(), extend, 0.0);
        first_layer = layer_id;
    }
    if (layer_id > last_layer) {
        int extend = layer_id - last_layer;
        layers.insert(layers.end(), extend, false);
        areas.insert(areas.end(), extend, 0.0);
        distances.insert(distances.end(), extend, 0.0);
        last_layer = layer_id;
    }

    int idx = layer_id - first_layer;
    layers[idx]    = true;
    areas[idx]     = area_val;
    distances[idx] = dist_val;
}

// ============================================================================
// MagmaTubeMap — Statistics
// ============================================================================

int MagmaTubeMap::num_solid_cells() const
{
    int count = 0;
    for (const auto &kv : m_cell_pair_index)
        if (kv.second.empty()) ++count;
    return count;
}

// ============================================================================
// MagmaTubeMap — Per-layer data accessors and helpers
// ============================================================================

double MagmaTubeMap::print_z(int layer_id) const
{
    if (layer_id >= 0 && layer_id < int(m_layer_data.size()))
        return m_layer_data[layer_id].print_z;
    return 0.0;
}

double MagmaTubeMap::layer_height_at(int layer_id) const
{
    if (layer_id >= 0 && layer_id < int(m_layer_data.size()))
        return m_layer_data[layer_id].height;
    return double(m_layer_height);
}

int MagmaTubeMap::window_center_layer(const UTubePair& pair) const
{
    double start_bottom = print_z(pair.pair_start_layer)
                        - layer_height_at(pair.pair_start_layer);
    double window_mid_z = (start_bottom + pair.window_end_z) / 2.0;
    for (int l = pair.pair_start_layer; l <= pair.pair_end_layer; ++l) {
        if (print_z(l) >= window_mid_z)
            return l;
    }
    return pair.pair_start_layer;
}

double MagmaTubeMap::span_height_mm(int start_layer, int end_layer) const
{
    if (start_layer < 0 || end_layer < start_layer)
        return 0.0;
    int sz = int(m_layer_data.size());
    if (start_layer >= sz || end_layer >= sz)
        return 0.0;
    // print_z is cumulative (top of layer), so span height =
    // top of end_layer minus bottom of start_layer.
    return m_layer_data[end_layer].print_z - m_layer_data[start_layer].bottom_z();
}

int MagmaTubeMap::layer_at_height_from(int start_layer, double target_mm) const
{
    if (start_layer < 0 || start_layer >= int(m_layer_data.size()))
        return m_num_layers - 1;
    // Target z = bottom of start_layer + target_mm
    double target_z = m_layer_data[start_layer].bottom_z() + target_mm;
    // Binary search: find first layer with print_z >= target_z
    auto it = Slic3r::lower_bound_by_predicate(
        m_layer_data.begin(), m_layer_data.end(),
        [target_z](const LayerData &ld) { return ld.print_z < target_z; });
    if (it == m_layer_data.end())
        return m_num_layers - 1;
    return static_cast<int>(it - m_layer_data.begin());
}

// ============================================================================
// MagmaTubeMap — Build
// ============================================================================

// Get effective interior width based on tube width mode.
// Auto: derive from nozzle OD (or fallback to 3× bore).
// Manual: use user-specified value directly.
static float get_effective_interior_width(MagmaTubeWidthMode mode,
                                          float manual_width, float nozzle_diameter,
                                          float nozzle_od, float line_width)
{
    if (mode == MagmaTubeWidthMode::Manual)
        return manual_width;
    // Auto mode
    if (nozzle_od > 0)
        return static_cast<float>(calculate_auto_interior_width_from_od(nozzle_od, line_width));
    return static_cast<float>(calculate_auto_interior_width(nozzle_diameter));
}

std::unique_ptr<MagmaTubeMap> MagmaTubeMap::build(
    const std::vector<Layer*> &layers,
    const PrintRegionConfig &config,
    const PrintConfig &print_config,
    const PrintObjectConfig &obj_config,
    const SlicingParameters &slicing_params,
    ProgressFn progress_fn,
    ThrowIfCanceled throw_if_canceled)
{
    auto t_start = std::chrono::high_resolution_clock::now();

    auto map = std::unique_ptr<MagmaTubeMap>(new MagmaTubeMap());

    // Extract config — use the sparse infill extruder's nozzle (1-based index in config)
    const int sparse_extruder_idx = std::max(0, config.sparse_infill_filament.value - 1);
    const float nozzle_diameter = static_cast<float>(print_config.nozzle_diameter.get_at(sparse_extruder_idx));
    map->m_line_width = static_cast<float>(config.sparse_infill_line_width.get_abs_value(nozzle_diameter));
    if (map->m_line_width <= 0.f)
        map->m_line_width = nozzle_diameter;
    const float nozzle_od = static_cast<float>(config.magma_nozzle_outer_diameter.value);
    const auto tube_mode = config.magma_tube_width_mode.value;
    map->m_interior_width = get_effective_interior_width(
        tube_mode, config.magma_interior_width.value, nozzle_diameter,
        nozzle_od, map->m_line_width);
    map->m_cell_spacing = cell_spacing_from_geometry(map->m_interior_width, map->m_line_width);
    // Nominal config layer height (fallback for missing layers in lookup tables)
    map->m_layer_height = static_cast<float>(obj_config.layer_height.value);
    map->m_dual_infill_enabled = config.dual_infill_enabled.value;
    map->m_solver_mode    = obj_config.magma_tube_solver_mode.value;
    map->m_solver_timeout = obj_config.magma_solver_timeout.value;

    // Build per-layer height/z tables for adaptive layer height support.
    // Must happen before WindowSpec and spiral params since they use m_min_layer_height.
    {
        int max_layer_id = 0;
        for (const Layer *l : layers)
            max_layer_id = std::max(max_layer_id, static_cast<int>(l->id()));
        map->m_num_layers = max_layer_id + 1;
        map->m_layer_data.resize(map->m_num_layers);
        for (const Layer *l : layers) {
            int lid = static_cast<int>(l->id());
            map->m_layer_data[lid].print_z = l->print_z;
            map->m_layer_data[lid].height  = l->height;
        }
        // Use min_layer_height from SlicingParameters (already computed across
        // all extruders by OrcaSlicer). Clamp to nominal if something is off.
        map->m_min_layer_height = static_cast<float>(
            slicing_params.min_layer_height > 0
                ? std::min(slicing_params.min_layer_height, double(map->m_layer_height))
                : map->m_layer_height);
    }

    // Spiral params — use min_layer_height to cap helix angle at thinnest layer
    bool spiral_enabled = config.magma_spiral_interlock.value;
    map->m_spiral_params = compute_spiral_params(map->m_interior_width, map->m_line_width,
                                                  map->m_min_layer_height, spiral_enabled);

    // Window spec (handles window height auto-calculation).
    map->m_window_spec = WindowSpec::from_config(
        static_cast<float>(config.magma_window_height_mm.value),
        map->m_interior_width,
        map->m_line_width,
        map->m_layer_height
    );

    // Min tube height: window height (×2 for solid wall above+below) plus 2 layers of padding.
    map->m_min_tube_height_mm = map->m_window_spec.window_height_mm * 2.0
                                + 2.0 * double(map->m_min_layer_height);

    // Tube height from user config (mm).
    // Clamped to m_min_tube_height_mm so tubes always fit at least one U-tube pair.
    {
        double user_tube_mm = std::max(1.0, config.magma_tube_height.value);
        map->m_max_tube_height_mm = std::max(user_tube_mm, map->m_min_tube_height_mm);

        if (user_tube_mm < map->m_min_tube_height_mm) {
            map->m_warning_message = Slic3r::format(
                "Magma max tube height (%.1fmm) is too short to fit a complete U-tube pair. "
                "Minimum is %.1fmm (2 × window height + 2 layers). "
                "Using minimum tube height.",
                user_tube_mm, map->m_min_tube_height_mm);
        }
    }

    // Injection speed vs filament max volumetric speed.
    if (map->m_warning_message.empty()) {
        double inj_speed = obj_config.magma_injection_speed.value;
        int inj_filament = obj_config.magma_injection_filament.value;
        unsigned int inj_idx = (inj_filament > 0)
            ? (unsigned int)(inj_filament - 1)
            : (unsigned int)std::max(0, config.sparse_infill_filament.value - 1);
        double max_vol = print_config.filament_max_volumetric_speed.get_at(inj_idx);
        if (max_vol > 0 && inj_speed > max_vol) {
            map->m_warning_message = Slic3r::format(
                "Magma injection speed (%.1f mm\u00b3/s) exceeds the filament's "
                "max volumetric speed (%.1f mm\u00b3/s). It will be capped automatically.",
                inj_speed, max_vol);
        }
    }

    // Boundary dodge distance: how far apart adjacent tube boundaries should be (mm).
    // Auto (0): 4 × max_layer_height. Ensures at least 4 solid layers bridging
    // each boundary discontinuity. Natural 3-level stagger from triangle lattice.
    {
        double user_dodge = config.magma_boundary_dodge.value;
        if (user_dodge <= 0.0) {
            double max_lh = slicing_params.max_layer_height;
            if (max_lh <= 0.0) max_lh = double(map->m_layer_height);
            map->m_dodge_distance = 4.0 * max_lh;
        } else {
            map->m_dodge_distance = user_dodge;
        }
    }

    // Build per-layer lattice cache (eliminates repeated sin/cos + TriangleLattice construction)
    for (int i = 0; i < map->m_num_layers; ++i)
        map->m_layer_data[i].lattice = lattice_for_layer(map->m_cell_spacing, map->m_spiral_params, i);

    // Read injection edge preference
    map->m_injection_edge_pref = obj_config.magma_injection_edge_pref.value;

    // ---- Triangle vertex overlap correction ----
    //
    // Computed before scan_layers so that the area threshold uses the effective
    // (post-correction) line width — matching the actual deposited bead width.
    //
    // In a triangle grid, 3 families of parallel lines cross at 60° angles.
    // At each vertex, lines overlap, depositing material twice. The excess
    // fraction of total deposited material is:
    //
    //   excess_frac = 3w / (4S)
    //
    // Two correction levers:
    //   1. Line width: reduce infill flow so deposited width shrinks from w to w_eff.
    //      Clamped at OrcaSlicer's min_bead_width (default 85% of nozzle).
    //   2. Injection volume: subtract per-layer excess from tube fill volume.
    //      Always applied, regardless of line width correction setting.
    {
        map->m_overlap_line_correction = config.magma_overlap_line_correction.value;
        map->m_effective_line_width = map->m_line_width;

        double S = map->m_cell_spacing;
        double w = map->m_line_width;
        double excess_frac = 3.0 * w / (4.0 * S);

        if (map->m_overlap_line_correction && excess_frac > 0.0) {
            // Minimum corrected line width: use magma_overlap_min_width if set,
            // otherwise default to 90% of nozzle diameter.
            double min_nozzle = *std::min_element(
                print_config.nozzle_diameter.values.begin(),
                print_config.nozzle_diameter.values.end());
            double min_pct = config.magma_overlap_min_width.value;
            if (min_pct <= 0)
                min_pct = 90.0;  // auto: 90% of nozzle
            double min_width = min_pct * 0.01 * min_nozzle;

            // Reduce line width by the excess fraction, but don't go below min_width
            double w_corrected = w * (1.0 - excess_frac);
            if (w_corrected < min_width)
                w_corrected = min_width;

            map->m_overlap_flow_correction = w_corrected / w;
            map->m_effective_line_width = w_corrected;
        } else {
            map->m_overlap_flow_correction = 1.0;
            map->m_effective_line_width = w;
        }

        BOOST_LOG_TRIVIAL(info) << "Magma overlap correction: excess_frac=" << excess_frac
            << " flow_correction=" << map->m_overlap_flow_correction
            << " w_eff=" << map->m_effective_line_width << "mm"
            << " (line_correction=" << (map->m_overlap_line_correction ? "on" : "off") << ")";
    }

    // Build phases (scan_layers uses m_effective_line_width for area threshold)
    map->scan_layers(layers);
    map->detect_constrictions();
    map->assign_tubes(progress_fn, throw_if_canceled);
    map->precompute_window_end_z();

    map->compute_volumes(layers);
    map->precompute_injection_data();
    map->precompute_window_gaps();

    // Build sorted list of injection cap layer IDs (for ToolOrdering)
    {
        std::vector<int> &ids = map->m_injection_layer_ids;
        ids.reserve(map->m_pairs.size());
        for (const UTubePair &pair : map->m_pairs)
            if (pair.volume_mm3 > 0)
                ids.push_back(pair.pair_end_layer);
        sort_remove_duplicates(ids);
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();

    BOOST_LOG_TRIVIAL(info) << "MagmaTubeMap built in " << ms << "ms: "
        << map->num_cells() << " cells, "
        << map->num_pairs() << " pairs, "
        << map->num_solid_cells() << " solid | "
        << "max_tube_height=" << map->m_max_tube_height_mm << "mm"
        << " dodge=" << map->m_dodge_distance << "mm";

    return map;
}

// ============================================================================
// MagmaTubeMap — scan_layers
// ============================================================================

void MagmaTubeMap::scan_layers(const std::vector<Layer*> &layers)
{
    // Pre-compute ideal inset triangle area (scaled^2) for interior cells.
    // Uses m_effective_line_width (post overlap correction) so the inset matches
    // the actual deposited bead width, not the nominal line width.
    const double half_line_width = m_effective_line_width * 0.5;
    const double inset_area_mm2 = inset_triangle_area(m_cell_spacing, m_effective_line_width);
    const double inset_area_scaled2 = inset_area_mm2 * 1e12;  // (1e6)^2

    // Boundary cells require ≥90% of ideal tube area to be considered for
    // tube pairing and injection.  At lower coverage the cell is clipped by the
    // infill boundary edge — infill lines still print (FillMagma clips to the
    // fill region independently) but the tube shouldn't receive injection.
    const double min_area_scaled2 = inset_area_scaled2 * 0.90;

    // Interior inset: if cell center is this far inside the zone region,
    // the tube inscribed circle (diameter = interior_width) fits entirely.
    // This is the tube-flow-relevant check, not the triangle-corner check.
    const coord_t interior_inset = scale_(m_interior_width * 0.5);

    // FIXED reference lattice for cell IDENTITY (a,b,c coordinates).
    // Cell identity must be stable across layers for tube pairing to work.
    // Spiral offset is applied per-layer for POSITION checks below.
    TriangleLattice ref_lattice(m_cell_spacing, 0.0, 0.0);

    for (int i = 0; i < int(layers.size()); ++i) {
        const Layer *layer = layers[i];
        // Use Layer::id() — not the array index — so that pair_start_layer,
        // pair_end_layer, and CellPresence layer indices all match what
        // FillMagma and GCode will query with (Layer::id() includes raft offset).
        const int layer_id = static_cast<int>(layer->id());

        // Spiral-offset lattice for this layer's actual cell positions.
        // Cell identity comes from ref_lattice, but position checks use the
        // spiral-offset position (matching what FillMagma actually renders).
        const TriangleLattice &layer_lattice = m_layer_data[layer_id].lattice;

        // Collect zone outer surfaces where tube infill will be generated
        ExPolygons zone_regions;
        for (const LayerRegion *layerm : layer->regions()) {
            const PrintRegionConfig &region_config = layerm->region().config();
            for (const Surface &surface : layerm->fill_surfaces.surfaces) {
                if (surface.surface_type == stInternal
                           && (region_config.dual_infill_enabled || is_magma_pattern(region_config.sparse_infill_pattern.value))) {
                    zone_regions.push_back(surface.expolygon);
                }
            }
        }

        zone_regions = union_ex(zone_regions);
        if (zone_regions.empty())
            continue;

        // Shrink region to identify fully-interior cells: tube inscribed circle
        // fits entirely within zone region → no per-cell clipping needed.
        ExPolygons interior_region = offset_ex(zone_regions, -interior_inset);

        // Enumerate cells from fixed reference lattice (stable identity)
        BoundingBox bbox = get_extents(zone_regions);
        // Expand bbox slightly to account for spiral offset moving cells
        bbox.offset(scale_(m_interior_width));
        std::vector<TriangleCell> cells = ref_lattice.enumerate_cells(bbox);

        for (const TriangleCell &cell : cells) {
            // Use spiral-offset position for containment checks
            Vec2d center_mm = layer_lattice.cell_center(cell);
            Point center_pt(scale_(center_mm.x()), scale_(center_mm.y()));

            // Fast path: center inside tube-clearance inset → fully unobstructed
            bool is_interior = false;
            for (const ExPolygon &ep : interior_region) {
                if (ep.contains(center_pt)) {
                    is_interior = true;
                    break;
                }
            }

            if (is_interior) {
                // Distance to nearest boundary (mm)
                Point proj = projection_onto(zone_regions, center_pt);
                double dist_mm = unscale<double>((center_pt - proj).cast<double>().norm());
                m_cells[cell].mark_present(layer_id, inset_area_scaled2, dist_mm);
                continue;
            }

            // Center must at least be inside the original region
            bool in_region = false;
            for (const ExPolygon &ep : zone_regions) {
                if (ep.contains(center_pt)) {
                    in_region = true;
                    break;
                }
            }

            if (!in_region)
                continue;

            // Boundary cell: compute actual tube area at spiral-offset position
            std::array<Vec2d, 3> corners = layer_lattice.cell_corners(cell);
            Polygon triangle;
            triangle.points.reserve(3);
            triangle.points.emplace_back(scale_(corners[0].x()), scale_(corners[0].y()));
            triangle.points.emplace_back(scale_(corners[1].x()), scale_(corners[1].y()));
            triangle.points.emplace_back(scale_(corners[2].x()), scale_(corners[2].y()));

            ExPolygons inset = offset_ex(triangle, -scale_(half_line_width));
            if (inset.empty())
                continue;

            ExPolygons clipped = intersection_ex(inset, zone_regions);
            if (clipped.empty())
                continue;

            double area = 0.0;
            for (const ExPolygon &ep : clipped)
                area += std::abs(ep.area());

            if (area < min_area_scaled2)
                continue;

            // Distance to nearest boundary (mm)
            Point proj = projection_onto(zone_regions, center_pt);
            double dist_mm = unscale<double>((center_pt - proj).cast<double>().norm());
            m_cells[cell].mark_present(layer_id, area, dist_mm);
        }
    }
}

// ============================================================================
// MagmaTubeMap — detect_constrictions
// ============================================================================

void MagmaTubeMap::detect_constrictions()
{
    // Two-tier constriction detection:
    // Tier 1 (fast): area ratio between consecutive layers
    // Tier 2 (expensive): polygon overlap check for flagged cells
    //
    // A constriction splits the cell's presence into separate spans,
    // preventing a tube from bridging across a near-discontinuity.

    int constrictions_found = 0;

    for (auto &[cell, presence] : m_cells) {
        if (presence.first_layer == presence.last_layer)
            continue;

        for (int i = presence.first_layer; i < presence.last_layer; ++i) {
            if (!presence.present(i) || !presence.present(i + 1))
                continue;

            double area_i   = presence.area(i);
            double area_ip1 = presence.area(i + 1);

            if (area_i <= 0.0 || area_ip1 <= 0.0)
                continue;

            // Tier 1: Area ratio heuristic (fast)
            double ratio = std::min(area_i, area_ip1) / std::max(area_i, area_ip1);
            if (ratio > 0.7)
                continue;  // Healthy overlap, skip expensive check

            // Tier 2: For cells that fail the ratio check, we use a stricter
            // area-based threshold. Full polygon overlap would require storing
            // per-layer zone regions (expensive). Instead, use a tighter ratio.
            if (ratio < 0.3) {
                // Severe constriction: split the cell at this boundary
                int idx = i + 1 - presence.first_layer;
                if (idx > 0 && idx < int(presence.layers.size())) {
                    presence.layers[idx] = false;
                    presence.areas[idx]  = 0.0;
                    ++constrictions_found;
                }
            }
        }
    }

    if (constrictions_found > 0)
        BOOST_LOG_TRIVIAL(info) << "MagmaTubeMap: " << constrictions_found << " constrictions found";
}

// ============================================================================
// MagmaTubeMap — assign_tubes (delegates to CP-SAT solver)
// ============================================================================

void MagmaTubeMap::assign_tubes(ProgressFn progress_fn, ThrowIfCanceled throw_if_canceled)
{
    MagmaTubeSolver solver(m_cells, m_layer_data,
                           m_min_tube_height_mm, m_max_tube_height_mm,
                           m_num_layers, m_dodge_distance,
                           m_solver_mode, m_solver_timeout);
    solver.solve(m_pairs, m_cell_pair_index, progress_fn, throw_if_canceled);

    if (solver.unknown_block_count() > 0 && m_warning_message.empty()) {
        m_warning_message = Slic3r::format(
            "Magma tube solver: %1% block(s) timed out without finding a solution. "
            "Greedy results were preserved for those blocks. Increase solver timeout "
            "for better coverage.",
            solver.unknown_block_count());
    }
}

// Old greedy PQ code deleted — replaced by MagmaTubeSolver (CP-SAT).
// See DESIGN-TUBE-SOLVER.md for the new architecture.

// ============================================================================
// MagmaTubeMap — precompute_window_end_z
// ============================================================================

void MagmaTubeMap::precompute_window_end_z()
{
    // Compute window_end_z for each pair: pure mm, no layer conversion.
    // A layer is "in the window" if its bottom (print_z - height) < window_end_z.
    // This is immune to variable layer height and floating-point rounding.
    const double wh_mm = m_window_spec.window_height_mm;

    for (UTubePair &pair : m_pairs) {
        double start_bottom = m_layer_data[pair.pair_start_layer].bottom_z();
        pair.window_end_z = start_bottom + wh_mm;
    }
}

// ============================================================================
// MagmaTubeMap — precompute_injection_data
// ============================================================================

void MagmaTubeMap::precompute_injection_data()
{
    for (UTubePair &pair : m_pairs) {
        // Determine injection side based on edge preference:
        // Interior = inject into cell further from edge (default)
        // Exterior = inject into cell closer to edge
        int cap_layer = pair.pair_end_layer;
        auto it_a = m_cells.find(pair.cell_a);
        auto it_b = m_cells.find(pair.cell_b);
        if (it_a != m_cells.end() && it_b != m_cells.end()) {
            double dist_a = it_a->second.distance(cap_layer);
            double dist_b = it_b->second.distance(cap_layer);
            bool swap = (m_injection_edge_pref == MagmaInjectionEdgePref::Interior)
                ? (dist_a < dist_b)   // A is closer to edge, want interior → swap
                : (dist_a > dist_b);  // A is further from edge, want exterior → swap
            if (swap)
                std::swap(pair.cell_a, pair.cell_b);
        }

        pair.injection_center = m_layer_data[cap_layer].lattice.cell_center(pair.cell_a);
        pair.window_center_layer = window_center_layer(pair);
    }
}

// ============================================================================
// MagmaTubeMap — precompute_window_gaps
// ============================================================================

// Merge overlapping/adjacent intervals in a sorted vector.
static void merge_intervals(std::vector<std::pair<double, double>> &intervals)
{
    if (intervals.size() <= 1)
        return;
    std::sort(intervals.begin(), intervals.end());
    std::vector<std::pair<double, double>> merged;
    merged.push_back(intervals[0]);
    for (size_t i = 1; i < intervals.size(); ++i) {
        if (intervals[i].first <= merged.back().second + 0.01)
            merged.back().second = std::max(merged.back().second, intervals[i].second);
        else
            merged.push_back(intervals[i]);
    }
    intervals = std::move(merged);
}

static WindowGaps compute_window_gaps_for_layer(
    int layer_id,
    const std::vector<UTubePair> &pairs,
    const TriangleLattice &lattice,
    double cell_spacing,
    const std::vector<LayerData> &layer_data)
{
    WindowGaps result;
    result.cell_spacing = cell_spacing;
    result.offset_x = lattice.offset_x();
    result.offset_y = lattice.offset_y();

    // For each pair with an open window on this layer, determine the shared
    // edge between cell_a and cell_b and add a gap on the correct line family.
    for (const auto &pair : pairs) {
        if (layer_id < pair.pair_start_layer || layer_id > pair.pair_end_layer)
            continue;
        if (layer_data[layer_id].bottom_z() >= pair.window_end_z)
            continue;

        SharedEdge edge = shared_edge(pair.cell_a, pair.cell_b);

        switch (edge) {
        case SharedEdge::Horizontal: {
            const TriangleCell &up = pair.cell_a.is_up() ? pair.cell_a : pair.cell_b;
            int row = up.b;
            Vec2d v0 = lattice.to_world(up.a, row);
            Vec2d v1 = lattice.to_world(up.a + 1, row);
            double x_lo = std::min(v0.x(), v1.x());
            double x_hi = std::max(v0.x(), v1.x());
            result.horiz[row].push_back({x_lo, x_hi});
            break;
        }
        case SharedEdge::Col60: {
            int col = std::max(pair.cell_a.a, pair.cell_b.a);
            int row = std::min(pair.cell_a.b, pair.cell_b.b);
            Vec2d v0 = lattice.to_world(col, row);
            Vec2d v1 = lattice.to_world(col, row + 1);
            double y_lo = std::min(v0.y(), v1.y());
            double y_hi = std::max(v0.y(), v1.y());
            result.col60[col].push_back({y_lo, y_hi});
            break;
        }
        case SharedEdge::Diag120: {
            const TriangleCell &up = pair.cell_a.is_up() ? pair.cell_a : pair.cell_b;
            int diag = up.a + up.b + 1;
            Vec2d v0 = lattice.to_world(up.a + 1, up.b);
            Vec2d v1 = lattice.to_world(up.a, up.b + 1);
            double y_lo = std::min(v0.y(), v1.y());
            double y_hi = std::max(v0.y(), v1.y());
            result.diag120[diag].push_back({y_lo, y_hi});
            break;
        }
        }
    }

    // Sort and merge intervals per line index
    for (auto &[key, intervals] : result.horiz)
        merge_intervals(intervals);
    for (auto &[key, intervals] : result.col60)
        merge_intervals(intervals);
    for (auto &[key, intervals] : result.diag120)
        merge_intervals(intervals);

    return result;
}

void MagmaTubeMap::precompute_window_gaps()
{
    // Determine which layers have any window gap by scanning all pairs.
    std::set<int> gap_layers;
    for (const auto &pair : m_pairs) {
        for (int lid = pair.pair_start_layer; lid <= pair.pair_end_layer; ++lid) {
            if (lid >= m_num_layers) continue;
            if (m_layer_data[lid].bottom_z() < pair.window_end_z)
                gap_layers.insert(lid);
        }
    }

    // Pre-compute WindowGaps for each layer that has gaps.
    for (int lid : gap_layers)
        m_window_gaps_cache[lid] = compute_window_gaps_for_layer(
            lid, m_pairs, m_layer_data[lid].lattice, m_cell_spacing, m_layer_data);
}

// ============================================================================
// MagmaTubeMap — compute_volumes
// ============================================================================

void MagmaTubeMap::compute_volumes(const std::vector<Layer*> &layers)
{
    // Build layer_id → height map for per-layer volume calculation.
    // Handles initial layer height, adaptive/dynamic layer heights, and raft offsets.
    std::unordered_map<int, double> layer_heights;
    for (const Layer *layer : layers)
        layer_heights[static_cast<int>(layer->id())] = layer->height;

    double edge_len = triangle_side_length(m_cell_spacing);
    // Inset side: the gap between two adjacent inset interiors runs along
    // the inset edge, not the full edge.  Use this for window gap volume.
    double inset_side = edge_len - m_line_width * SQRT3;

    // Vertex overlap excess area per triangle cell (mm²).
    //
    // At each triangle vertex, 3 line families cross at 60° creating overlap
    // regions where material is deposited twice. This excess material physically
    // occupies tube interior space, so injection volume must be reduced.
    //
    // Per full triangle cell, the overlap excess area is:
    //   (3√3/4) × w_eff²  ≈  1.299 × w_eff²
    //
    // where w_eff is the effective deposited line width (after flow correction
    // if enabled, or nominal line width if disabled).
    //
    // Computed per-layer to handle adaptive/variable layer heights correctly.
    // Each UTubePair spans 2 triangle cells, so we multiply by 2.
    constexpr double OVERLAP_AREA_FACTOR = 1.299038105676658;  // 3√3/4
    double w_eff = m_effective_line_width;
    double excess_area_per_cell_mm2 = OVERLAP_AREA_FACTOR * w_eff * w_eff;

    // Track volume reduction across all pairs for user warning
    double total_orig_volume      = 0.0;
    double total_overlap_excess   = 0.0;
    int    zero_volume_pairs      = 0;

    for (UTubePair &pair : m_pairs) {
        double tube_volume_scaled2_mm = 0.0;  // accumulated (area_scaled2 * height_mm)
        double window_height_mm = 0.0;
        double overlap_excess_volume = 0.0;

        for (int layer_id = pair.pair_start_layer; layer_id <= pair.pair_end_layer; ++layer_id) {
            auto it_a = m_cells.find(pair.cell_a);
            auto it_b = m_cells.find(pair.cell_b);

            double area_a = (it_a != m_cells.end()) ? it_a->second.area(layer_id) : 0.0;
            double area_b = (it_b != m_cells.end()) ? it_b->second.area(layer_id) : 0.0;

            auto h_it = layer_heights.find(layer_id);
            double lh = (h_it != layer_heights.end()) ? h_it->second : double(m_layer_height);

            tube_volume_scaled2_mm += (area_a + area_b) * lh;

            // Accumulate window height for window layers (pure Z check)
            if (m_layer_data[layer_id].bottom_z() < pair.window_end_z)
                window_height_mm += lh;

            // Per-layer overlap excess: 2 cells per pair
            overlap_excess_volume += excess_area_per_cell_mm2 * 2.0 * lh;
        }

        // Convert: area in scaled^2 → mm^2 via SCALING_FACTOR^2 (1e-12)
        double tube_volume = tube_volume_scaled2_mm * SCALING_FACTOR * SCALING_FACTOR;

        // Window gap volume: opening between paired cell interiors along shared edge.
        // Length = inset side (not full edge — interiors are smaller than the outer triangle).
        double window_volume = inset_side * m_line_width * window_height_mm;

        double orig_volume = tube_volume + window_volume;
        pair.volume_mm3 = std::max(0.0, orig_volume - overlap_excess_volume);

        // Accumulate totals for average reduction warning
        total_orig_volume    += orig_volume;
        total_overlap_excess += std::min(overlap_excess_volume, orig_volume);
        if (pair.volume_mm3 <= 0.0)
            ++zero_volume_pairs;
    }

    // Warn when average overlap correction is aggressive — may indicate triangles
    // too small for reliable injection. Threshold: 33% average volume loss.
    double avg_reduction_frac = (total_orig_volume > 0.0)
        ? total_overlap_excess / total_orig_volume : 0.0;
    if (avg_reduction_frac > 0.33) {
        m_warning_message = Slic3r::format(
            L("Magma triangle vertex overlap correction reduced average injection "
              "volume by %1%%%. %2% tube pair(s) have zero injection volume. "
              "Consider increasing the interior width to create larger triangles "
              "with less overlap."),
            int(std::round(avg_reduction_frac * 100)),
            zero_volume_pairs);
    }
}

// ============================================================================
// MagmaTubeMap — Query Interface
// ============================================================================

bool MagmaTubeMap::is_window_open(const TriangleCell &cell, int layer_id) const
{
    auto it = m_cell_pair_index.find(cell);
    if (it == m_cell_pair_index.end() || it->second.empty())
        return false;

    // Pure mm check: a layer is in the window if its bottom is below window_end_z.
    // No layer-count conversion, immune to variable layer height and FP rounding.
    if (layer_id < 0 || layer_id >= int(m_layer_data.size()))
        return false;
    double layer_bottom = m_layer_data[layer_id].bottom_z();

    for (int pair_idx : it->second) {
        const UTubePair &pair = m_pairs[pair_idx];
        if (layer_id >= pair.pair_start_layer &&
            layer_id <= pair.pair_end_layer &&
            layer_bottom < pair.window_end_z)
            return true;
    }

    return false;
}

bool MagmaTubeMap::is_paired(const TriangleCell &cell) const
{
    auto it = m_cell_pair_index.find(cell);
    return (it != m_cell_pair_index.end() && !it->second.empty());
}

ExPolygons MagmaTubeMap::get_unfilled_cell_interiors(int layer_id) const
{
    ExPolygons result;
    const double half_lw = m_line_width * 0.5;

    const TriangleLattice &lattice = m_layer_data[layer_id].lattice;

    for (const auto &[cell, presence] : m_cells) {
        if (!presence.present(layer_id))
            continue;

        // Check if any tube pair covers this cell on this layer
        bool covered = false;
        auto idx_it = m_cell_pair_index.find(cell);
        if (idx_it != m_cell_pair_index.end()) {
            for (int pi : idx_it->second) {
                const UTubePair &pair = m_pairs[pi];
                if (layer_id >= pair.pair_start_layer &&
                    layer_id <= pair.pair_end_layer) {
                    covered = true;
                    break;
                }
            }
        }
        if (covered)
            continue;

        // Unfilled cell — build inset triangle at spiral-offset position
        std::array<Vec2d, 3> corners = lattice.cell_corners(cell);
        Polygon triangle;
        triangle.points.reserve(3);
        for (const Vec2d &c : corners)
            triangle.points.emplace_back(scale_(c.x()), scale_(c.y()));

        ExPolygons inset = offset_ex(triangle, -scale_(half_lw));
        for (ExPolygon &ep : inset)
            result.push_back(std::move(ep));
    }
    return result;
}



WindowGaps MagmaTubeMap::window_gaps(int layer_id) const
{
    auto it = m_window_gaps_cache.find(layer_id);
    if (it != m_window_gaps_cache.end())
        return it->second;
    // No gaps on this layer — return empty result with correct offsets
    const TriangleLattice &lattice = m_layer_data[layer_id].lattice;
    return {m_cell_spacing, lattice.offset_x(), lattice.offset_y(), {}, {}, {}};
}

} // namespace magma
} // namespace Slic3r
