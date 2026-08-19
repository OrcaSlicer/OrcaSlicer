#include "MagmaTubeMap.hpp"
#include "MagmaResolved.hpp"
#include "MagmaTubeSolver.hpp"
#include "MagmaPatterns.hpp"

#include "../Layer.hpp"
#include "../Flow.hpp"
#include "../Print.hpp"
#include "../Slicing.hpp"
#include "../ClipperUtils.hpp"
#include "../Polygon.hpp"
#include "../ExPolygon.hpp"
#include "../Surface.hpp"
#include "../ExtrusionEntityCollection.hpp"   // polygons_covered_by_width (measured volume)
#include "../I18N.hpp"
#include "../format.hpp"

#define L(s) Slic3r::I18N::translate(s)

#include <algorithm>
#include <chrono>
#include <climits>
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

double CellPresence::opening_radius(int layer_id) const
{
    if (layer_id < first_layer || layer_id > last_layer)
        return 0.0;
    int idx = layer_id - first_layer;
    if (idx < 0 || idx >= int(opening_radii.size()))
        return 0.0;
    return opening_radii[idx];
}

Vec2d CellPresence::injection_point(int layer_id) const
{
    if (layer_id < first_layer || layer_id > last_layer)
        return Vec2d(0.0, 0.0);
    int idx = layer_id - first_layer;
    if (idx < 0 || idx >= int(injection_pts.size()))
        return Vec2d(0.0, 0.0);
    return injection_pts[idx];
}

void CellPresence::mark_present(int layer_id, double area_val, double dist_val,
                               double opening_r, const Vec2d &inj_pt)
{
    if (first_layer == INT_MAX) {
        // First time: initialize
        first_layer = layer_id;
        last_layer  = layer_id;
        layers.assign(1, true);
        areas.assign(1, area_val);
        distances.assign(1, dist_val);
        opening_radii.assign(1, opening_r);
        injection_pts.assign(1, inj_pt);
        return;
    }

    // Expand range if needed
    if (layer_id < first_layer) {
        int extend = first_layer - layer_id;
        layers.insert(layers.begin(), extend, false);
        areas.insert(areas.begin(), extend, 0.0);
        distances.insert(distances.begin(), extend, 0.0);
        opening_radii.insert(opening_radii.begin(), extend, 0.0);
        injection_pts.insert(injection_pts.begin(), extend, Vec2d(0.0, 0.0));
        first_layer = layer_id;
    }
    if (layer_id > last_layer) {
        int extend = layer_id - last_layer;
        layers.insert(layers.end(), extend, false);
        areas.insert(areas.end(), extend, 0.0);
        distances.insert(distances.end(), extend, 0.0);
        opening_radii.insert(opening_radii.end(), extend, 0.0);
        injection_pts.insert(injection_pts.end(), extend, Vec2d(0.0, 0.0));
        last_layer = layer_id;
    }

    int idx = layer_id - first_layer;
    layers[idx]        = true;
    areas[idx]         = area_val;
    distances[idx]     = dist_val;
    opening_radii[idx] = opening_r;
    injection_pts[idx] = inj_pt;
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
    if (valid_layer(layer_id))
        return m_layer_data[layer_id].print_z;
    return 0.0;
}

double MagmaTubeMap::layer_height_at(int layer_id) const
{
    if (valid_layer(layer_id))
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
    if (end_layer < start_layer || ! valid_layer(start_layer) || ! valid_layer(end_layer))
        return 0.0;
    // print_z is cumulative (top of layer), so span height =
    // top of end_layer minus bottom of start_layer.
    return m_layer_data[end_layer].print_z - m_layer_data[start_layer].bottom_z();
}

// ============================================================================
// MagmaTubeMap — Build
// ============================================================================

// Tube interior width sizing lives in MagmaTubeMap.hpp as effective_interior_width() so
// Print::validate() predicts the same geometry this build() produces. It used to be a
// static here that called the triangle-specific free function directly, which meant
// rectilinear and hex tubes were silently sized with the triangle formula.

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

    // Every config-derived number comes from the one shared resolver, so the lattice built
    // here, the warnings in Print::validate() and the PresetHints readout cannot describe
    // three different tubes -- which is exactly what happened while each resolved its own.
    MagmaResolved res;
    if (! resolve_magma(config, obj_config, print_config, res)) {
        // Unreachable from PrintObject, which only calls this for a region that already
        // resolves to a Magma pattern. Say so rather than handing back a null map that
        // surfaces later as FillMagmaBase::require_tube_map's "no tube map" slicing error,
        // several steps away from the config that actually caused it.
        BOOST_LOG_TRIVIAL(error)
            << "MagmaTubeMap::build called for a region whose effective pattern is not a Magma "
               "pattern; no tube map will be built.";
        return nullptr;
    }

    map->m_line_width     = static_cast<float>(res.line_width);
    map->m_pattern        = res.pattern;
    map->m_geometry       = res.geometry;
    map->m_interior_width = static_cast<float>(res.interior_width);
    map->m_cell_spacing   = res.cell_spacing;

    // Nominal config layer height (fallback for missing layers in lookup tables)
    map->m_layer_height = static_cast<float>(obj_config.layer_height.value);
    map->m_dual_infill_enabled = config.dual_infill_enabled.value;
    map->m_solver_mode    = obj_config.magma_tube_solver_mode.value;
    map->m_solver_timeout = obj_config.magma_solver_timeout.value;

    // Build per-layer height/z tables for adaptive layer height support.
    // Must happen before WindowSpec and spiral params since they use m_min_layer_height.
    {
        // m_layer_data is indexed by ABSOLUTE Layer::id(), and object layer ids start at
        // slicing_parameters().raft_layers() (PrintObjectSlice.cpp) -- so with a raft the
        // rows below the first object layer belong to no layer we know anything about.
        // Record where the valid range begins rather than leaving zero-filled rows that
        // read as legitimate 0mm-tall layers at Z=0: that seeded the solver's minimum layer
        // height with 0.0, divided by it, and disabled CP-SAT refinement on every raft print.
        int max_layer_id = 0;
        int min_layer_id = INT_MAX;
        for (const Layer *l : layers) {
            max_layer_id = std::max(max_layer_id, static_cast<int>(l->id()));
            min_layer_id = std::min(min_layer_id, static_cast<int>(l->id()));
        }
        map->m_first_layer_id = (min_layer_id == INT_MAX) ? 0 : min_layer_id;
        map->m_num_layers = max_layer_id + 1;
        map->m_layer_data.resize(map->m_num_layers);
        for (const Layer *l : layers) {
            int lid = static_cast<int>(l->id());
            map->m_layer_data[lid].print_z = l->print_z;
            map->m_layer_data[lid].height  = l->height;
        }
        // The smallest layer height this object actually PRINTS, taken from the layers we
        // just walked. This used to be slicing_params.min_layer_height, which is the machine
        // floor (min_layer_height_from_nozzle, ~0.07mm) and has nothing to do with the object
        // -- so m_min_tube_height_mm's "+ 2 layers" of seal-wall padding came out at 0.14mm
        // instead of 0.4mm on a 0.2mm print, admitting tubes whose sealing wall the nozzle
        // punches straight through. The real per-layer heights were already in hand.
        {
            double min_h = 0.0;
            for (int L = map->m_first_layer_id; L < map->m_num_layers; ++L)
                if (map->m_layer_data[L].height > 0.0 &&
                    (min_h <= 0.0 || map->m_layer_data[L].height < min_h))
                    min_h = map->m_layer_data[L].height;
            map->m_min_layer_height = static_cast<float>(min_h > 0.0 ? min_h : map->m_layer_height);
        }
    }

    // Spiral params — use min_layer_height to cap helix angle at thinnest layer
    bool spiral_enabled = obj_config.magma_spiral_interlock.value;
    map->m_spiral_params = compute_spiral_params(map->m_interior_width, map->m_line_width,
                                                  map->m_min_layer_height, spiral_enabled);

    // Window spec (handles window height auto-calculation).
    map->m_window_spec = WindowSpec::from_config(
        *map->m_geometry,
        static_cast<float>(obj_config.magma_window_height_mm.value),
        map->m_interior_width,
        map->m_line_width,
        map->m_layer_height
    );

    // Min tube height: window height plus enough sealing wall above it (×1.5 ≈ half a
    // window of wall for the nozzle to seal against) plus 2 layers of padding. The
    // sealing wall (≈0.5×window + 2 layers) must exceed the nozzle z-slam depth so the
    // slam doesn't punch through into the window — true for typical square/triangle
    // geometry; see the seal discussion for tying this directly to slam depth.
    map->m_min_tube_height_mm = map->m_window_spec.window_height_mm * 1.5
                                + 2.0 * double(map->m_min_layer_height);

    // Tube height from user config (mm).
    // Clamped to m_min_tube_height_mm so tubes always fit at least one U-tube pair.
    {
        double user_tube_mm = std::max(1.0, obj_config.magma_tube_height.value);
        // Candidate tube heights are layer-boundary differences — discrete steps of
        // one layer height. A [min,max] window narrower than one layer can straddle a
        // gap between two consecutive achievable heights and admit NONE (e.g. min=max=
        // 4.96mm with 0.2mm layers: 24 layers=4.8mm is too short, 25=5.0mm too tall),
        // silently yielding 0 tubes. Keep the window at least one (max) layer wide so a
        // discrete height always lands inside it. Widening only raises the upper bound;
        // the solver still picks the largest discrete height ≤ the true max.
        double layer_headroom = slicing_params.max_layer_height > 0.0
            ? slicing_params.max_layer_height
            : double(map->m_layer_height);
        double floor_mm = map->m_min_tube_height_mm + layer_headroom;
        map->m_max_tube_height_mm = std::max(user_tube_mm, floor_mm);

        if (user_tube_mm < map->m_min_tube_height_mm) {
            map->m_warning_message = Slic3r::format(
                "Magma max tube height (%.1fmm) is too short to fit a complete U-tube pair. "
                "Minimum is %.1fmm (1.5 × window height + 2 layers). "
                "Using minimum tube height.",
                user_tube_mm, map->m_min_tube_height_mm);
        }
    }

    // Injection speed vs filament max volumetric speed.
    if (map->m_warning_message.empty()) {
        double inj_speed = obj_config.magma_injection_speed.value;
        double max_vol = print_config.filament_max_volumetric_speed.get_at(
            (unsigned int)res.injection_extruder);
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
        double user_dodge = obj_config.magma_boundary_dodge.value;
        if (user_dodge <= 0.0) {
            double max_lh = slicing_params.max_layer_height;
            if (max_lh <= 0.0) max_lh = double(map->m_layer_height);
            map->m_dodge_distance = 4.0 * max_lh;
        } else {
            map->m_dodge_distance = user_dodge;
        }
    }

    // Build per-layer lattice cache (eliminates repeated sin/cos + lattice
    // construction). Built via the pattern factory so each layer's lattice
    // matches the selected pattern (triangle today).
    for (int i = map->m_first_layer_id; i < map->m_num_layers; ++i)
        map->m_layer_data[i].lattice = lattice_for_layer(map->m_pattern, map->m_cell_spacing, map->m_spiral_params, i, map->m_line_width);

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
    // Vertex-overlap correction is now VOLUME-ONLY: measure_volumes subtracts the doubled
    // material at line crossings from the injected volume. The old second lever -- thinning
    // every deposited bead to average out a local excess -- was removed. It shipped off by
    // default (changing line width causes its own print problems), and it was the sole reason
    // this class carried two line widths, which had already produced several sites that
    // disagreed about which one to use. Vertex overlap is a geometry problem; the right fix is
    // shortening the crossing lines, not thinning all of them. See git history.

    // Cap-seal clearance gate (used by scan_layers' geometry-sanity check): the
    // injection nozzle flat seats at the cell centre, so a tube layer needs ≥ flat/2
    // clearance from that centre to the nearest boundary. A spike poking toward the
    // centre — or a centre sitting too close to a wall — collapses this even when the
    // area is fine. (flat/2 exactly: no margin, or boundary cells whose centre is just
    // over flat/2 from a wall get wrongly dropped.) Resolved from the injection
    // filament's nozzle, matching MagmaInjection.
    map->m_min_cap_clearance = 0.5 * res.injection_nozzle_flat;  // injection nozzle flat radius

    // Build phases (scan_layers uses m_line_width for area threshold).
    // scan_layers' 70%-of-ideal area gate already excludes pinched/under-area
    // layers from a cell's presence, so no separate constriction pass is needed.
    map->scan_layers(layers);
    map->assign_tubes(progress_fn, throw_if_canceled);
    map->assign_extra_vents();   // tri-hex: add manifold legs (no-op for other patterns)
    map->precompute_window_end_z();
    map->precompute_injection_data();

    // Injection volumes (pair.volume_mm3) and the injection cap-layer list are NOT computed
    // here — they're measured from the REAL toolpath in measure_volumes(), called after
    // PrintObject::infill(). Until then volumes are 0; nothing before injection G-code reads them.

    // Catch-all: Magma is enabled but produced no reinforcement tubes. Surfaces the
    // actual OUTCOME (not just a bad-setting guess) with the most likely reason, so an
    // empty slice never passes silently. Overrides any earlier setting-level warning —
    // with zero tubes those are moot (no injection happens at all).
    if (map->m_pairs.empty()) {
        // Tallest contiguous single-cell column. If even this is below the minimum tube
        // height, no tube can form regardless of pairing; otherwise the failure is in
        // pairing (adjacency / shared height), not part height.
        double tallest_run_mm = 0.0;
        for (const auto &kv : map->m_cells) {
            const CellPresence &cp = kv.second;
            if (cp.first_layer == INT_MAX) continue;
            int run_start = -1;
            for (int L = cp.first_layer; L <= cp.last_layer; ++L) {
                bool present = cp.present(L);
                if (present && run_start < 0) run_start = L;
                if ((!present || L == cp.last_layer) && run_start >= 0) {
                    int run_end = present ? L : L - 1;
                    tallest_run_mm = std::max(tallest_run_mm,
                                              map->span_height_mm(run_start, run_end));
                    run_start = -1;
                }
            }
        }

        std::string reason;
        if (map->m_cells.empty()) {
            reason =
                "no Magma cells fit inside the part — the cell size is too large for this "
                "object's cross-section. Reduce the Magma interior width or infill spacing.";
        } else if (tallest_run_mm + 1e-6 < map->m_min_tube_height_mm) {
            reason = Slic3r::format(
                "the reinforced region is only %.1fmm tall, below the %.1fmm minimum for one "
                "U-tube (1.5 × window height + 2 layers). Use a taller part, or reduce the "
                "Magma interior width to shrink the window (and the minimum).",
                tallest_run_mm, map->m_min_tube_height_mm);
        } else {
            reason =
                "cells are tall enough but none could be paired into U-tubes — neighbouring "
                "cells may not share enough height or area. Try a smaller cell size, denser "
                "infill spacing, or a more uniform part shape.";
        }
        map->m_warning_message = "Magma produced no reinforcement tubes: " + reason;
    }

    // Tri-hex: warn when the triangle vents are too small to stay open. The vent is an
    // equilateral triangle of side e = cell_spacing/sqrt3; inset by the wall it shrinks to an
    // open inscribed diameter = cell_spacing/3 - line_width.
    // Below ~1mm, the line-overlap plastic plus ordinary print imperfections seal the vent
    // and block injection — tri-hex only works for large tubes. Only set when no harder
    // failure above already claimed the warning.
    if (map->m_pattern == ipMagmaTriHex && map->m_warning_message.empty()) {
        const double vent_dia = map->m_cell_spacing / 3.0 - double(map->m_line_width);
        if (vent_dia < 1.0) {
            map->m_warning_message = Slic3r::format(
                "Magma Tri-hex vents are only %.2f mm across after line overlap — they may seal "
                "off and block injection. Tri-hex needs large tubes: use an interior width of "
                "about 4 mm or more (or switch to Magma Honeycomb / Rectilinear) for reliable vents.",
                vent_dia);
        }
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
// MagmaTubeMap — tube_opening_diameter
// ============================================================================

double MagmaTubeMap::tube_opening_diameter() const
{
    // Opening = circumscribed circle of the inset (hollow) cell, via the shared
    // per-shape geometry so the injection seal math and Print::validate()'s seal
    // warning use one source of truth.
    return m_geometry->opening_diameter(m_cell_spacing, m_line_width);
}

double MagmaTubeMap::cap_opening_diameter(const UTubePair &pair) const
{
    // Per-tube seal opening. Preferred: the REAL deposited cap cavity measured post-fill
    // (measure_volumes) — generic across patterns and free of the cell_inset model's hex
    // doubled-wall over-read. Fallbacks: the scan_layers cell_inset opening, then the global ideal.
    if (pair.measured_cap_opening_dia > 0.0)
        return pair.measured_cap_opening_dia;
    auto it = m_cells.find(pair.cell_a);
    if (it != m_cells.end()) {
        double r = it->second.opening_radius(pair.pair_end_layer);
        if (r > 0.0)
            return 2.0 * r;
    }
    return tube_opening_diameter();
}

double MagmaTubeMap::neighbor_centroid_distance() const
{
    // Centre-to-centre distance to an edge-sharing neighbour, via the per-shape
    // geometry (triangle: side/sqrt(3); square: cell spacing).
    return m_geometry->neighbor_centroid_distance(m_cell_spacing);
}

double MagmaTubeMap::cell_bore_at(const TriangleCell &cell, int layer) const
{
    // Ideal (full) open bore for this cell's kind: kind 0 (triangle/square cell, or the
    // tri-hex hex hub) inscribes the interior width; tri-hex triangle vents inscribe the
    // inset triangle (side e - lw·√3, inscribed diameter = side/√3).
    double ideal_bore;
    if (cell.kind == 0) {
        ideal_bore = m_interior_width;
    } else {
        // The tri-hex vent bore. Must stay the same quantity (spacing/3 - lw) the
        // vent-width warning at the top of build() computes -- these were once two
        // spellings of one formula evaluated on two different line widths.
        double e     = m_cell_spacing * INV_SQRT3;
        double inset = e - double(m_line_width) * SQRT3;
        ideal_bore = inset > 0.0 ? inset * INV_SQRT3 : m_interior_width;
    }
    // Narrow on clipped layers: the bead area scales with bore², so the per-layer bore is
    // the ideal scaled by sqrt(area(layer) / max_area). max_area ≈ the unclipped area.
    auto it = m_cells.find(cell);
    if (it == m_cells.end())
        return ideal_bore;
    const CellPresence &p = it->second;
    double a = p.area(layer);
    if (a <= 0.0)
        return ideal_bore;
    double amax = 0.0;
    for (double av : p.areas)
        amax = std::max(amax, av);
    if (amax <= 0.0)
        return ideal_bore;
    return ideal_bore * std::sqrt(std::min(1.0, a / amax));
}

const MagmaLattice& MagmaTubeMap::topology_lattice() const
{
    if (!m_topology_lattice)
        m_topology_lattice = make_magma_lattice(m_pattern, m_cell_spacing, 0.0, 0.0, m_line_width);
    return *m_topology_lattice;
}

// ============================================================================
// cell_inset_polygons — a cell's hollow open cross-section
// ============================================================================
//
// The single source of truth for "cell inner corners after accounting for line
// width": the cell's wall outline (cell_corners) inset by half the bead width.
// Used by the presence scan (ideal + clipped area), the per-kind ideal cache, and
// volume integration so every path measures a cell the same way. offset_ex can
// split or empty a thin cell, hence ExPolygons.
static ExPolygons cell_inset_polygons(const MagmaLattice &lattice,
                                      const TriangleCell &cell,
                                      double half_line_width)
{
    std::vector<Vec2d> corners = lattice.cell_corners(cell);
    Polygon poly;
    poly.points.reserve(corners.size());
    for (const Vec2d &c : corners)
        poly.points.emplace_back(scale_(c.x()), scale_(c.y()));
    return offset_ex(poly, -scale_(half_line_width));
}

// ============================================================================
// MagmaTubeMap — scan_layers
// ============================================================================

void MagmaTubeMap::scan_layers(const std::vector<Layer*> &layers)
{
    // Half the deposited bead width: the inset between a cell's wall outline
    // (cell_corners) and its hollow open cross-section. Uses m_line_width
    // (post overlap correction) so the inset matches the actual bead, not nominal.
    const double half_line_width = m_line_width * 0.5;

    // Per-layer presence gate: a cell counts as present on a layer if its clipped
    // tube area is ≥70% of ideal. Deliberately loose — injection volume scales with
    // each layer's ACTUAL clipped area (see measure_volumes), so an admitted partial
    // cell is injected proportionally, never over-injected. A little mid-tube clipping
    // is harmless; what actually governs a reliable injection is the SEAL at the cap
    // layer (the nozzle must press against solid wall around the opening), not uniform
    // per-layer fullness. 70% also keeps marginal boundary/corner cells present on the
    // first layer (whose fill region is ~0.08mm/side smaller from the wider first-layer
    // wall) without any special-case grace. Applied per-kind (see ideal_area_for below)
    // so tri-hex's small triangle vents are gated against their own ideal, not the hub's.
    const double presence_area_frac = 0.70;

    // Interior inset: if cell center is this far inside the zone region,
    // the tube inscribed circle (diameter = interior_width) fits entirely.
    // This is the tube-flow-relevant check, not the triangle-corner check.
    const coord_t interior_inset = scale_(m_interior_width * 0.5);

    // FIXED reference lattice for cell IDENTITY (a,b,c coordinates). Cell identity must
    // be stable across layers for tube pairing to work; spiral offset is applied
    // per-layer for POSITION checks below. Shared (cached) zero-offset instance.
    const MagmaLattice &ref_lattice = topology_lattice();

    // Per-kind ideal {open area (scaled^2), opening radius (mm)}, cached on first use.
    // Derived from cell_inset_polygons — the SAME inset-of-cell_corners the boundary
    // and volume paths use — so every code path measures a cell identically. This is
    // what sizes tri-hex's small triangle vents correctly (they'd otherwise inherit
    // the much larger hex-hub area and be over-injected / wrongly gated). Single-kind
    // patterns (triangle/square) just populate kind 0. opening radius = farthest inset
    // corner from the cell centre. Indexed by kind (0=hub/tri/square, 1/2=trihex vents).
    std::pair<double, double> kind_ideal[3];
    bool kind_done[3] = { false, false, false };
    auto ideal_for = [&](const TriangleCell &cell) -> std::pair<double, double> {
        const uint8_t k = cell.kind;
        if (k < 3 && kind_done[k]) return kind_ideal[k];
        ExPolygons inset = cell_inset_polygons(ref_lattice, cell, half_line_width);
        double area = 0.0;
        for (const ExPolygon &ep : inset)
            area += std::abs(ep.area());
        Vec2d  ctr = ref_lattice.cell_center(cell);
        Point  ctr_pt(scale_(ctr.x()), scale_(ctr.y()));
        double opr_scaled = 0.0;
        for (const ExPolygon &ep : inset)
            for (const Point &p : ep.contour.points)
                opr_scaled = std::max(opr_scaled, (ctr_pt - p).cast<double>().norm());
        std::pair<double, double> val{ area, unscale<double>(opr_scaled) };
        if (k < 3) { kind_ideal[k] = val; kind_done[k] = true; }
        return val;
    };

    for (int i = 0; i < int(layers.size()); ++i) {
        const Layer *layer = layers[i];
        // Use Layer::id() — not the array index — so that pair_start_layer,
        // pair_end_layer, and CellPresence layer indices all match what
        // FillMagma and GCode will query with (Layer::id() includes raft offset).
        const int layer_id = static_cast<int>(layer->id());

        // Spiral-offset lattice for this layer's actual cell positions.
        // Cell identity comes from ref_lattice, but position checks use the
        // spiral-offset position (matching what FillMagma actually renders).
        const MagmaLattice &layer_lattice = *m_layer_data[layer_id].lattice;

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
                Point proj = projection_onto(zone_regions, center_pt);
                double dist_mm = unscale<double>((center_pt - proj).cast<double>().norm());
                // Same clearance gate as the boundary path. Interior cells almost always
                // pass (centre ≥ interior_inset from any wall), but this still catches a
                // too-small interior width where even an unclipped cell can't seat the nozzle.
                if (dist_mm < m_min_cap_clearance)
                    continue;
                // Unclipped: cavity == full cell, so the injection point is the cell centre.
                std::pair<double, double> ideal = ideal_for(cell);
                m_cells[cell].mark_present(layer_id, ideal.first, dist_mm,
                                           ideal.second, center_mm);
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

            // Boundary cell: actual open area at the spiral-offset position = the cell's
            // inset open polygon clipped to the zone.
            ExPolygons inset = cell_inset_polygons(layer_lattice, cell, half_line_width);
            if (inset.empty())
                continue;

            ExPolygons clipped = intersection_ex(inset, zone_regions);
            if (clipped.empty())
                continue;

            double area = 0.0;
            for (const ExPolygon &ep : clipped)
                area += std::abs(ep.area());

            if (area < presence_area_frac * ideal_for(cell).first)
                continue;

            // Injection point: centroid of the largest clipped piece. For an unclipped
            // cell this equals the cell centre; for a clipped cell it shifts INTO the
            // cavity (away from the part wall), giving the nozzle more clearance. For a
            // regular polygon the centroid is the inscribed-circle centre, so this is
            // exact for square/hex and good for triangle. Fall back to the cell centre if
            // a concave clip puts the centroid outside the opening.
            const ExPolygon *piece = &clipped.front();
            for (const ExPolygon &ep : clipped)
                if (std::abs(ep.area()) > std::abs(piece->area())) piece = &ep;
            Point inj_pt = piece->contour.centroid();
            if (!piece->contains(inj_pt))
                inj_pt = center_pt;
            Vec2d inj_mm(unscale<double>(inj_pt.x()), unscale<double>(inj_pt.y()));

            // Geometry-sanity gate at the injection point: the nozzle flat seats here, so
            // it needs ≥ flat/2 clearance to the opening boundary. A spike poking toward
            // the injection point collapses this even when the area is fine. Clearance is
            // measured at the actual injection point — not whether a clear circle exists
            // elsewhere in the opening.
            double clearance_mm = unscale<double>(
                (inj_pt - projection_onto(clipped, inj_pt)).cast<double>().norm());
            if (clearance_mm < m_min_cap_clearance)
                continue;

            // Opening radius: farthest point of the clipped opening from the injection
            // point — the seal cone must spread this far to cover it. Drives the per-tube
            // the per-tube seal depth (see cap_opening_diameter()).
            double opening_r_scaled = 0.0;
            for (const ExPolygon &ep : clipped)
                for (const Point &p : ep.contour.points)
                    opening_r_scaled = std::max(opening_r_scaled,
                                                (inj_pt - p).cast<double>().norm());
            double opening_r = unscale<double>(opening_r_scaled);

            // distance: cell centre → nearest boundary, for injection-side preference.
            Point proj = projection_onto(zone_regions, center_pt);
            double dist_mm = unscale<double>((center_pt - proj).cast<double>().norm());

            m_cells[cell].mark_present(layer_id, area, dist_mm, opening_r, inj_mm);
        }
    }

    // Post-scan summary: per-cell layer span vs. the object's top layer index.
    // If tubes stop one below the top, last_layer here will be top-1 for most
    // cells. Grep "MagmaScanSpan".
    {
        int max_layer_id = layers.empty() ? -1
            : static_cast<int>(layers.back()->id());
        int cells_reaching_top = 0;
        int min_last = INT_MAX, max_last = -1;
        for (const auto &[cell, presence] : m_cells) {
            if (presence.last_layer == max_layer_id) ++cells_reaching_top;
            min_last = std::min(min_last, presence.last_layer);
            max_last = std::max(max_last, presence.last_layer);
        }
        BOOST_LOG_TRIVIAL(info) << "MagmaScanSpan top_layer_id=" << max_layer_id
            << " cells=" << m_cells.size()
            << " reaching_top=" << cells_reaching_top
            << " last_layer[min=" << (min_last == INT_MAX ? -1 : min_last)
            << " max=" << max_last << "]";
    }
}

// ============================================================================
// MagmaTubeMap — assign_tubes (delegates to CP-SAT solver)
// ============================================================================

void MagmaTubeMap::assign_tubes(ProgressFn progress_fn, ThrowIfCanceled throw_if_canceled)
{
    // Cell topology (neighbors/is_up) — offset-independent, so the cached zero-offset
    // lattice serves every layer's connectivity queries.
    MagmaTubeSolver solver(topology_lattice(), m_cells, m_layer_data, m_first_layer_id,
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

// ============================================================================
// MagmaTubeMap — assign_extra_vents (tri-hex manifold legs)
// ============================================================================
//
// The solver matched each hub-tube to exactly ONE primary vent (cell_b). This pass —
// tri-hex only, purely additive — gives each remaining triangle vent extra legs on the
// bordering hub-tubes whose entire [start,cap] range it can fill. Per DESIGN-TRIHEX.md
// §4: a candidate hub-tube is feasible only if the vent is present AND unclaimed (not
// already its own primary) on EVERY layer of the range (a block/claim inside the range
// would trap air). Among feasible candidates we pick a max-coverage NON-overlapping
// subset by weighted interval scheduling (a vent can be one leg per layer, but may serve
// several stacked hub-tubes at disjoint heights). Each vent is independent — hubs are
// uncapped, so any hub-tube can host legs from several of its bordering vents. Because
// feasibility was already secured by the solver's matching, this can never strand a hub.

void MagmaTubeMap::assign_extra_vents()
{
    if (m_pattern != ipMagmaTriHex || m_pairs.empty())
        return;

    // Cached zero-offset lattice for neighbour topology (vent -> its bordering hubs).
    const MagmaLattice &lat = topology_lattice();

    auto hub_of  = [](const UTubePair &p) { return p.cell_a.kind == THK_HEX ? p.cell_a : p.cell_b; };
    auto vent_of = [](const UTubePair &p) { return p.cell_a.kind == THK_HEX ? p.cell_b : p.cell_a; };

    // Index hub-tubes by their hub cell; collect each vent's primary-claimed ranges.
    std::unordered_map<TriangleCell, std::vector<int>, TriangleCellHash> tubes_by_hub;
    std::unordered_map<TriangleCell, std::vector<std::pair<int, int>>, TriangleCellHash> primary_claim;
    for (int pi = 0; pi < int(m_pairs.size()); ++pi) {
        const UTubePair &p = m_pairs[pi];
        tubes_by_hub[hub_of(p)].push_back(pi);
        primary_claim[vent_of(p)].push_back({ p.pair_start_layer, p.pair_end_layer });
    }

    int extra_legs_added = 0;

    for (const auto &cell_kv : m_cells) {
        const TriangleCell &V = cell_kv.first;
        if (V.kind == THK_HEX)              // hubs are not vents
            continue;
        const CellPresence &pres = cell_kv.second;

        auto it_claim = primary_claim.find(V);
        auto available_at = [&](int L) -> bool {
            if (!pres.present(L))
                return false;
            if (it_claim != primary_claim.end())
                for (const auto &rng : it_claim->second)
                    if (L >= rng.first && L <= rng.second)
                        return false;       // busy as this layer's primary
            return true;
        };

        // Feasible candidate hub-tubes on V's bordering hubs (excluding tubes where V is
        // already the primary) whose ENTIRE range is available.
        struct Cand { int pi; int start; int cap; };
        std::vector<Cand> cands;
        for (const CellId &H : lat.neighbors(V)) {
            auto it = tubes_by_hub.find(H);
            if (it == tubes_by_hub.end())
                continue;
            for (int pi : it->second) {
                const UTubePair &p = m_pairs[pi];
                if (vent_of(p) == V)
                    continue;
                bool ok = true;
                for (int L = p.pair_start_layer; L <= p.pair_end_layer; ++L)
                    if (!available_at(L)) { ok = false; break; }
                if (ok)
                    cands.push_back({ pi, p.pair_start_layer, p.pair_end_layer });
            }
        }
        if (cands.empty())
            continue;

        // Weighted interval scheduling: max total layers covered by a non-overlapping
        // subset. Sort by cap, DP with latest-compatible-predecessor, then backtrack.
        std::sort(cands.begin(), cands.end(),
                  [](const Cand &a, const Cand &b) { return a.cap < b.cap; });
        const int n = int(cands.size());
        auto wt = [&](int i) { return cands[i].cap - cands[i].start + 1; };  // layers covered
        std::vector<int> pred(n, -1);
        for (int i = 0; i < n; ++i)
            for (int j = i - 1; j >= 0; --j)
                if (cands[j].cap < cands[i].start) { pred[i] = j; break; }
        std::vector<int> best(n + 1, 0);   // best[i] over cands[0..i-1]
        for (int i = 1; i <= n; ++i) {
            int incl = wt(i - 1) + (pred[i - 1] >= 0 ? best[pred[i - 1] + 1] : 0);
            best[i] = std::max(best[i - 1], incl);
        }
        for (int i = n; i > 0; ) {
            int incl = wt(i - 1) + (pred[i - 1] >= 0 ? best[pred[i - 1] + 1] : 0);
            if (incl > best[i - 1]) {       // candidate i-1 is in the optimal set
                const int pi = cands[i - 1].pi;
                m_pairs[pi].extra_vents.push_back(V);
                // Register the leg as covered, exactly as the solver registers cell_a/cell_b,
                // so every consumer of m_cell_pair_index (is_paired, the fill generator)
                // sees it as belonging to this hub-tube over the tube's range.
                m_cell_pair_index[V].push_back(pi);
                ++extra_legs_added;
                i = (pred[i - 1] >= 0) ? pred[i - 1] + 1 : 0;
            } else {
                --i;
            }
        }
    }

    BOOST_LOG_TRIVIAL(info) << "MagmaTriHex extra-vent sweep: added " << extra_legs_added
        << " extra vent legs across " << m_pairs.size() << " hub-tubes";
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
        int cap_layer = pair.pair_end_layer;

        // Choose the injection (cap) side.
        if (pair.cell_a.kind != pair.cell_b.kind) {
            // Tri-hex: a pair's two cells differ in kind. The injection HUB is always
            // the hexagon (THK_HEX == kind 0); the triangle is the vent. Edge preference
            // does not apply — the geometry fixes which side is the hub.
            if (pair.cell_a.kind != THK_HEX)
                std::swap(pair.cell_a, pair.cell_b);
        } else {
            // Single-kind patterns (triangle / square): edge preference picks the side.
            // Interior = inject into the cell further from the part edge (default);
            // Exterior = inject into the cell closer to the edge.
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
        }

        // Injection point = the cap layer's stored clipped-cavity centroid (== cell
        // centre for unclipped cells). Falls back to the regular lattice centre if the
        // cell record is missing (shouldn't happen for a paired cell).
        auto it_inj = m_cells.find(pair.cell_a);
        pair.injection_center = (it_inj != m_cells.end())
            ? it_inj->second.injection_point(cap_layer)
            : m_layer_data[cap_layer].lattice->cell_center(pair.cell_a);
        pair.window_center_layer = window_center_layer(pair);
    }
}

// ============================================================================
// MagmaTubeMap — measure_volumes (from the actual deposited toolpath, post-fill)
// ============================================================================
//
// For each pair, the injected cavity at a layer is  (cell_a ∪ cell_b ∪ vents) ∩ zone  minus
// the deposited magma walls. Summed over the run × layer height, that single measurement nets
// out doubled walls, line-crossing overlaps, the window gap, and part-edge clipping — no
// per-pattern correction. Runs after PrintObject::infill(), when layer->regions()->fills hold
// the real paths.
void MagmaTubeMap::measure_volumes(const std::vector<Layer*> &layers)
{
    std::unordered_map<int, const Layer*> by_id;
    for (const Layer *L : layers) by_id[int(L->id())] = L;

    // Layer ids touched by any pair.
    std::set<int> used;
    for (const UTubePair &p : m_pairs)
        for (int L = p.pair_start_layer; L <= p.pair_end_layer; ++L) used.insert(L);

    // Per-layer cache (shared by all pairs on a layer): the magma zone + deposited walls.
    std::unordered_map<int, ExPolygons> zone_by_layer, walls_by_layer;
    std::unordered_map<int, double> h_by_layer;
    for (int lid : used) {
        auto it = by_id.find(lid);
        if (it == by_id.end()) continue;
        const Layer *layer = it->second;
        h_by_layer[lid] = double(layer->height);
        ExPolygons zone; Polygons walls;
        for (const LayerRegion *lr : layer->regions()) {
            const PrintRegionConfig &rc = lr->region().config();
            if (!(rc.dual_infill_enabled || is_magma_pattern(rc.sparse_infill_pattern.value)))
                continue;
            for (const Surface &s : lr->fill_surfaces.surfaces)
                if (s.surface_type == stInternal) zone.push_back(s.expolygon);
            append(walls, lr->fills.polygons_covered_by_width(0.f));
        }
        zone_by_layer[lid]  = union_ex(zone);
        walls_by_layer[lid] = union_ex(walls);
    }

    // The injection volume is corrected for vertex overlap: where infill lines cross, the
    // second bead's material bulges into the void and polygons_covered_by_width -- which
    // unions the crossing lines -- never captures it, so it has to be subtracted here. This is
    // the only overlap correction that remains; the flow-thinning lever was removed.
    const double excess_unit = m_geometry->vertex_overlap_excess_area(m_line_width);

    m_injection_layer_ids.clear();
    int    zero   = 0;
    double t_meas = 0.0;
    for (UTubePair &pair : m_pairs) {
        // Per-pair overlap excess area (mm^2 per unit height), apportioned per cell. Tri-hex
        // charges by corner count (hub 6, vent 3, /4 into each incident cell); the single-cell
        // patterns return their per-cell value directly (hex returns 0 — no crossings).
        double excess_rate;
        if (m_pattern == ipMagmaTriHex) {
            auto charge = [&](const TriangleCell &c) {
                return 0.25 * ((c.kind == THK_HEX) ? 6.0 : 3.0) * excess_unit;
            };
            excess_rate = charge(pair.cell_a) + charge(pair.cell_b);
            for (const TriangleCell &ev : pair.extra_vents) excess_rate += charge(ev);
        } else {
            excess_rate = excess_unit * double(2 + int(pair.extra_vents.size()));
        }
        double vol = 0.0;
        for (int lid = pair.pair_start_layer; lid <= pair.pair_end_layer; ++lid) {
            auto zit = zone_by_layer.find(lid);
            if (zit == zone_by_layer.end() || zit->second.empty()) continue;
            const MagmaLattice &lat   = lattice_at(lid);
            const ExPolygons   &walls = walls_by_layer[lid];
            // Open cross-section of a set of cells at this layer: their corner polygons clipped
            // to the magma zone, minus the deposited walls. Shared by the per-layer volume sum
            // (all cells) and the cap injection-center / seal-opening measurement (cell_a only).
            auto cell_cavity = [&](const std::vector<TriangleCell> &cs) -> ExPolygons {
                Polygons polys;
                for (const TriangleCell &c : cs) {
                    std::vector<Vec2d> cor = lat.cell_corners(c);
                    if (cor.size() < 3) continue;
                    Polygon poly;
                    for (const Vec2d &v : cor) poly.points.push_back(Point(scale_(v.x()), scale_(v.y())));
                    polys.push_back(std::move(poly));
                }
                ExPolygons z = intersection_ex(union_ex(polys), zit->second);
                return (walls.empty() || z.empty()) ? z : diff_ex(z, walls);
            };

            std::vector<TriangleCell> all_cells{ pair.cell_a, pair.cell_b };
            for (const TriangleCell &ev : pair.extra_vents) all_cells.push_back(ev);
            ExPolygons cavity = cell_cavity(all_cells);
            if (cavity.empty()) continue;
            double a2 = 0.0;
            for (const ExPolygon &ep : cavity) a2 += std::abs(ep.area());
            vol += unscale<double>(unscale<double>(a2)) * h_by_layer[lid];
            // Subtract the over-extruded crossing material that squeezes into the void on this
            // layer. excess_rate scales with the deposited width: residual when the flow
            // correction is on, full when off. Zero for honeycomb (no crossings).
            vol -= excess_rate * h_by_layer[lid];

            // At the cap (top) layer, measure the injection cell's REAL open cross-section (same
            // helper, cell_a only) and derive BOTH the injection center (its centroid) and the
            // seal opening. One source → center + seal stay consistent; generic across patterns.
            if (lid == pair.pair_end_layer) {
                ExPolygons cap = cell_cavity({ pair.cell_a });
                if (!cap.empty()) {
                    const ExPolygon *big = &cap.front();
                    for (const ExPolygon &ep : cap)
                        if (std::abs(ep.area()) > std::abs(big->area())) big = &ep;
                    Point c = big->contour.centroid();
                    if (!big->contains(c)) {                       // concave clip → cell center
                        Vec2d cc = lat.cell_center(pair.cell_a);
                        c = Point(scale_(cc.x()), scale_(cc.y()));
                    }
                    pair.injection_center = Vec2d(unscale<double>(c.x()), unscale<double>(c.y()));
                    // Seal opening = CIRCUMSCRIBED diameter: 2× the FARTHEST point of the real cap
                    // cavity from the injection center. The cone must press deep enough to reach
                    // the furthest open point — including the small wall-gap slivers the real
                    // cavity captures, which are real opening. As the hot nozzle descends it covers
                    // everything inside that radius and deforms the nearer material outward to
                    // close it, so reaching the farthest extent seals the whole opening.
                    double r = 0.0;
                    for (const Point &p : big->contour.points)
                        r = std::max(r, (c - p).cast<double>().norm());
                    pair.measured_cap_opening_dia = 2.0 * unscale<double>(r);
                }
            }
        }
        if (vol < 0.0) vol = 0.0;   // overlap can exceed a tiny cell's cavity → not injectable
        t_meas += vol;
        pair.volume_mm3 = vol;
        if (vol > 0.0) m_injection_layer_ids.push_back(pair.pair_end_layer);
        else ++zero;
    }
    sort_remove_duplicates(m_injection_layer_ids);
    BOOST_LOG_TRIVIAL(info) << "Magma measured volumes: " << m_pairs.size() << " pairs, "
        << zero << " zero-volume | total=" << t_meas << "mm3 (overlap@lw="
        << m_line_width << "mm)";
}

// ============================================================================
// MagmaTubeMap — Query Interface
// ============================================================================

bool MagmaTubeMap::window_open_at(const UTubePair &pair, int layer_id) const
{
    // Pure Z + layer-range check. bottom_z = print_z - height, so this equals
    // the old "layer in [start,end] && layer_data[layer].bottom_z() < window_end_z".
    return layer_id >= pair.pair_start_layer && layer_id <= pair.pair_end_layer
        && (print_z(layer_id) - layer_height_at(layer_id)) < pair.window_end_z;
}

bool MagmaTubeMap::is_paired(const TriangleCell &cell) const
{
    auto it = m_cell_pair_index.find(cell);
    return (it != m_cell_pair_index.end() && !it->second.empty());
}


} // namespace magma
} // namespace Slic3r
