#ifndef slic3r_Magma_MagmaTubeMap_hpp_
#define slic3r_Magma_MagmaTubeMap_hpp_

#include "MagmaTriangleCell.hpp"
#include "MagmaSpiralOffset.hpp"
#include "../PrintConfig.hpp"
#include "../ExPolygon.hpp"
#include "../BoundingBox.hpp"

#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>
#include <climits>

namespace Slic3r {

class Layer;
struct SlicingParameters;

namespace magma {

// Effective Magma reinforcement pattern for a region. In dual-infill mode the reinforcement
// is the OUTER zone (dual_infill_outer_pattern); otherwise it is the region's sparse pattern.
// Single source of truth so MagmaTubeMap::build, the pre-slice warnings in Print::validate,
// and the fill path all key off the same pattern (sparse_infill_pattern is the inner yolk in
// dual mode and must NOT drive seal/overlap geometry).
//
// The dual outer-pattern option is typed as the full InfillPattern enum but only Magma values
// are valid for the reinforcement zone (the GUI dropdown lists only those). An imported
// profile / edited 3mf / API call could set a non-Magma value, which would build an ordinary
// infill with no U-tube channels while injection still runs into nothing — so clamp a stray
// non-Magma outer pattern to Triangle. The non-dual case returns the sparse pattern verbatim
// (callers use is_magma_pattern() on the result to decide whether the region is Magma at all).
// Resolve the tube interior width exactly as MagmaTubeMap::build() does.
//
// Manual: the user's value verbatim.
// Auto:   the largest tube the injection immersion budget allows, resolved through THIS
//         pattern's geometry. Immersion exists only to fit a tube wider than the nozzle
//         flat -- a tube the flat covers is sealed by seating on the rim with no descent,
//         so a bigger budget buys a bigger tube and nothing else.
//
// `plunge_reserve` is the plunge depth that must still fit INSIDE the budget after the seal.
// Without it, auto sizing spent the whole budget on the seal depth and clamp_plunge_depth()
// then found zero headroom -- so the plunge was mathematically always 0 in Auto mode, i.e. on
// the default configuration. The seal held by a single fixed press with no slam-melt at all,
// silently, with the setting switched on in the UI. Reserving it here keeps
// `magma_max_immersion` meaning what it says: the TOTAL depth the nozzle reaches, seal plus
// plunge, rather than the seal alone.
//
// Lives here, not inside MagmaTubeMap.cpp, because Print::validate() must predict the
// same geometry the injection G-code will actually use. Resolving it twice is how the
// two silently drift apart.
inline double effective_interior_width(const MagmaGeometry &geometry,
                                       MagmaTubeWidthMode mode,
                                       double manual_width,
                                       double nozzle_flat,
                                       double line_width,
                                       double cone_half_angle_deg,
                                       double max_immersion,
                                       double plunge_reserve)
{
    if (mode == MagmaTubeWidthMode::Manual)
        return manual_width;
    const double seal_budget = std::max(0.0, max_immersion - std::max(0.0, plunge_reserve));
    return geometry.interior_for_opening(
        max_opening_for_immersion(nozzle_flat, cone_half_angle_deg, seal_budget), line_width);
}

// Scalar overload so the GUI (which holds a DynamicPrintConfig, not a PrintRegionConfig)
// can resolve the pattern through the same rule instead of reimplementing it.
inline InfillPattern magma_effective_pattern(bool dual_infill_enabled,
                                             InfillPattern outer,
                                             InfillPattern sparse) {
    if (dual_infill_enabled) {
        // Substituting a default here would silently print a different lattice than the
        // one that was asked for, so Print::validate() reports the substitution. The UI
        // enum only offers Magma patterns; this is reachable from a hand-edited or
        // foreign 3MF. Keep the two in step if this ever changes.
        return is_magma_pattern(outer) ? outer : ipMagmaTriangle;
    }
    return sparse;
}

inline InfillPattern magma_effective_pattern(const PrintRegionConfig &config) {
    if (config.dual_infill_enabled.value) {
        InfillPattern outer = config.dual_infill_outer_pattern.value;
        return is_magma_pattern(outer) ? outer : ipMagmaTriangle;
    }
    return config.sparse_infill_pattern.value;
}

// Per-cell layer presence and interior area
struct CellPresence {
    int first_layer = INT_MAX;   // first layer where cell exists in magma region
    int last_layer  = -1;        // last layer where cell exists in magma region
    std::vector<bool>   layers;  // indexed [layer_id - first_layer]
    std::vector<double> areas;   // parallel, interior area in scaled^2 units
    std::vector<double> distances; // parallel, distance to nearest boundary (mm, unscaled)
    std::vector<double> opening_radii; // parallel, max injection-point→opening boundary (mm)
    std::vector<Vec2d>  injection_pts; // parallel, injection point (mm): clipped-opening
                                       // centroid; == cell centre for unclipped cells

    bool   present(int layer_id) const;
    double area(int layer_id) const;
    double distance(int layer_id) const;
    double opening_radius(int layer_id) const;
    Vec2d  injection_point(int layer_id) const;
    void   mark_present(int layer_id, double area_val, double dist_val,
                        double opening_r, const Vec2d &inj_pt);
};

// A U-tube pair assignment — for tri-hex, a manifold (one hub + N equal-length vent
// legs). cell_a is the injection HUB, cell_b the PRIMARY vent leg (the 1-leg U-tube
// for triangle/square). extra_vents holds any additional legs added by the post-solver
// extra-vent sweep (empty for triangle/square). All legs span the same [start,cap] with
// windows pinned to the tube bottom; one injection fills the hub + every leg.
struct UTubePair {
    TriangleCell cell_a;          // injection side (HUB for tri-hex)
    TriangleCell cell_b;          // primary vent leg
    std::vector<TriangleCell> extra_vents;  // tri-hex manifold: additional vent legs (sweep-assigned)
    int  pair_start_layer;        // first layer of this tube segment
    int  pair_end_layer;          // last layer of this tube segment (inclusive)
    double window_end_z;          // Z coordinate (mm) where the window ends — pure mm, no rounding
    double volume_mm3;            // injection volume (hub + all legs combined)

    // Pre-computed during build (avoids lattice + binary search at G-code time).
    // injection_center starts as the cell_inset centroid and is REFINED by measure_volumes
    // (post-fill) to the real deposited cap-cavity centroid; measured_cap_opening_dia is the
    // real opening the seal must cover (farthest extent of that cavity from the center). Both
    // come from one cap-layer measurement so center + seal stay consistent across all patterns.
    Vec2d  injection_center;      // XY center for injection (refined to real cap-cavity centroid)
    int    window_center_layer;   // center layer of window gap
    double measured_cap_opening_dia = 0.0;  // real cap opening (0 → cap_opening_diameter falls
                                            // back to the cell_inset model)
};

// Per-layer data: Z heights and pre-built lattice with spiral offset.
struct LayerData {
    double print_z;   // cumulative Z (top of layer)
    double height;    // individual layer height
    // Lattice (with spiral offset) for this layer, built via the pattern's
    // factory. shared_ptr because LayerData is copied/stored and the abstract
    // MagmaLattice is not value-copyable.
    std::shared_ptr<MagmaLattice> lattice;

    double bottom_z() const { return print_z - height; }
};

class MagmaTubeMap {
public:
    // Build the tube map. Called from PrintObject::infill() before TBB fill loop.
    using ProgressFn      = std::function<void(int, int)>; // (current, total)
    using ThrowIfCanceled = std::function<void()>;

    static std::unique_ptr<MagmaTubeMap> build(
        const std::vector<Layer*> &layers,
        const PrintRegionConfig &config,
        const PrintConfig &print_config,
        const PrintObjectConfig &obj_config,
        const SlicingParameters &slicing_params,
        ProgressFn progress_fn = nullptr,
        ThrowIfCanceled throw_if_canceled = nullptr);

    // === Query interface (thread-safe, const) ===

    // Is this pair's window open (gap present) on this layer? Pure Z + layer-range
    // check — no XY geometry. Per-shape toolpaths use this to decide whether to
    // cut a window gap for the pair, then compute the cut geometry themselves.
    bool window_open_at(const UTubePair &pair, int layer_id) const;

    // Is this cell assigned to a tube (true) or should be solid fill (false)?
    bool is_paired(const TriangleCell &cell) const;

    // Accessors for shared params (so FillMagma uses identical values)
    const SpiralParams& spiral_params() const { return m_spiral_params; }
    double cell_spacing() const { return m_cell_spacing; }
    float  interior_width() const { return m_interior_width; }

    // Circumscribed-circle diameter of the inset triangle = the tube opening the
    // nozzle flat (plus cone, when it descends) must cover to seal. Auto tube
    // sizing makes this approximately the nozzle tip flat. Used by auto_seal_depth().
    double tube_opening_diameter() const;
    // Actual tube-opening diameter at a pair's cap (injection) layer: 2× the max
    // distance from the injection cell centre to its clipped opening boundary.
    // Falls back to tube_opening_diameter() if unavailable. Drives the per-tube
    // auto_seal_depth() so the seal matches each tube's real opening.
    double cap_opening_diameter(const UTubePair &pair) const;
    // Centre-to-centre distance to an edge-sharing neighbour cell (injection crater
    // clearance), via the active pattern's geometry.
    double neighbor_centroid_distance() const;
    // Per-layer render bore (mm) for a cell's injected column: the cell's ideal open bore
    // (kind 0 = interior width; tri-hex vent = inset-triangle inscribed) scaled by
    // sqrt(clipped_area(layer) / max_area), so a column narrows on layers where the part
    // clips the cell. Cosmetic (preview only).
    double cell_bore_at(const TriangleCell &cell, int layer) const;

    // Pre-built lattice with spiral offset for a given layer.
    // Eliminates repeated sin/cos + lattice construction. Returned through the
    // MagmaLattice interface so consumers stay pattern-agnostic.
    // Only layers at or above first_layer_id() carry real data -- see the note in build().
    bool   valid_layer(int layer_id) const {
        return layer_id >= m_first_layer_id && layer_id < int(m_layer_data.size());
    }
    int    first_layer_id() const { return m_first_layer_id; }
    // The pattern this map's lattice, spacing and U-tube pairs were built for. The fill path
    // attaches one object-wide map to whichever FillMagma subclass a region produced, so the
    // two can disagree when regions select different Magma patterns -- windows then land on
    // unrelated lines with no diagnostic.
    InfillPattern pattern() const { return m_pattern; }
    const MagmaLattice& lattice_at(int layer_id) const { return *m_layer_data[layer_id].lattice; }

    // Window center layer for a U-tube pair: the layer at or above
    // the Z midpoint of the window gap.  Tube fill exists from this
    // layer upward to pair_end_layer.
    int window_center_layer(const UTubePair& pair) const;

    // For injection G-code and visualization
    const std::vector<UTubePair>& u_tube_pairs() const { return m_pairs; }

    // Measure injected volumes from the REAL deposited toolpath. Call AFTER
    // PrintObject::infill() (when layer fills exist). Overwrites pair.volume_mm3 and
    // rebuilds the injection cap-layer list.
    void measure_volumes(const std::vector<Layer*> &layers);

    float layer_height() const { return m_layer_height; }
    double window_height_mm() const { return m_window_spec.window_height_mm; }

    // Per-layer data accessors (adaptive layer height support)
    double print_z(int layer_id) const;
    double layer_height_at(int layer_id) const;

    // Non-empty if slicing should display a warning to the user
    // (e.g. the tri-hex vents are too narrow to stay open).
    const std::string& warning_message() const { return m_warning_message; }

    // Layer IDs that have injection caps (pair_end_layer of each UTubePair).
    // Used by ToolOrdering to register injection filament on the correct layers.
    const std::vector<int>& injection_layer_ids() const { return m_injection_layer_ids; }

    // Statistics for debug logging
    int num_cells() const { return static_cast<int>(m_cells.size()); }
    int num_pairs() const { return static_cast<int>(m_pairs.size()); }
    int num_solid_cells() const;

private:
    MagmaTubeMap() = default;

    // Build phases
    void scan_layers(const std::vector<Layer*> &layers);
    void assign_tubes(ProgressFn progress_fn, ThrowIfCanceled throw_if_canceled);
    // Tri-hex only: purely-additive post-solver pass that gives each vent extra legs
    // on bordering hub-tubes whose full range it can fill (see DESIGN-TRIHEX.md §4).
    void assign_extra_vents();
    void precompute_window_end_z();
    void precompute_injection_data();
    // Zero-offset lattice for cell IDENTITY (a,b,c,kind) + topology (neighbors/is_up).
    // Both are offset-independent, so one cached instance serves scan_layers, the solver,
    // and the extra-vent sweep instead of each constructing its own. Lazily built.
    const MagmaLattice& topology_lattice() const;

    // Adaptive layer height helpers
    double span_height_mm(int start_layer, int end_layer) const;

    // Data
    std::unordered_map<TriangleCell, CellPresence, TriangleCellHash> m_cells;
    std::vector<UTubePair> m_pairs;
    // Maps each cell to its tube pair indices. A cell with multiple stacked
    // segments will have multiple entries.
    // Empty vector = solid fill (no tube partner found).
    std::unordered_map<TriangleCell, std::vector<int>, TriangleCellHash> m_cell_pair_index;

    // Per-layer data: Z heights and lattice with spiral offset.
    // Indexed by layer_id. Built in build() from the layers vector.
    std::vector<LayerData> m_layer_data;

    // Cached zero-offset lattice (cell identity + topology); see topology_lattice().
    mutable std::unique_ptr<MagmaLattice> m_topology_lattice;

    // Pattern selection (geometry formulas + lattice construction).
    // m_geometry points at the shared, stateless per-shape strategy
    // (magma_geometry_for); m_pattern picks the lattice factory.
    InfillPattern         m_pattern = ipMagmaTriangle;
    const MagmaGeometry  *m_geometry = nullptr;

    // Config
    SpiralParams m_spiral_params;
    WindowSpec   m_window_spec;
    double m_cell_spacing;
    float  m_interior_width;
    float  m_line_width;
    double m_min_cap_clearance = 0.0; // min centre→boundary clearance for a sealable
                                      // injection (≈ nozzle flat radius + margin)
    float  m_layer_height;            // nominal config layer height (fallback)
    float  m_min_layer_height;        // smallest layer height actually printed in this object
    double m_dodge_distance;          // boundary dodge distance in mm (stagger target)
    double m_max_tube_height_mm;      // max tube height in mm (drives boundary placement)
    double m_min_tube_height_mm;      // min tube height in mm
    int    m_num_layers;
    // First index in m_layer_data that corresponds to a real object layer. Non-zero when a
    // raft is present, because Layer::id() is absolute and includes the raft offset.
    int    m_first_layer_id = 0;
    bool   m_dual_infill_enabled;
    MagmaTubeSolverMode m_solver_mode = MagmaTubeSolverMode::Refined;
    double m_solver_timeout = 20.0;
    MagmaInjectionEdgePref m_injection_edge_pref = MagmaInjectionEdgePref::Interior;

    // Triangle vertex overlap correction
    std::string m_warning_message;           // non-empty if slicing warning should be shown
    std::vector<int> m_injection_layer_ids;  // sorted, deduplicated cap layer IDs
};

using MagmaTubeMapPtr = std::unique_ptr<MagmaTubeMap>;

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaTubeMap_hpp_
