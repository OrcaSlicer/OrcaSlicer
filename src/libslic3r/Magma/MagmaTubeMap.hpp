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
    // nozzle flat (plus cone, when z-slamming) must cover to seal. Auto tube
    // sizing makes this approximately the nozzle tip flat. Used by auto z-slam.
    double tube_opening_diameter() const;
    // Actual tube-opening diameter at a pair's cap (injection) layer: 2× the max
    // distance from the injection cell centre to its clipped opening boundary.
    // Falls back to tube_opening_diameter() if unavailable. Drives the per-tube
    // auto z-slam so the seal matches each tube's real opening.
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

    // Triangle vertex overlap correction: flow multiplier for infill lines (≤1.0).
    // Applied in Fill.cpp via Flow::with_flow_ratio() to reduce deposited line width.
    double overlap_flow_correction() const { return m_overlap_flow_correction; }

    // Non-empty if slicing should display a warning to the user
    // (e.g. overlap correction reduced injection volume by >33%).
    const std::string& warning_message() const { return m_warning_message; }

    // Return inset triangle polygons for cells that exist on this layer but are
    // not covered by any UTubePair. These cells have infill lines but no injection,
    // so they should not be treated as solid for bridge detection.
    ExPolygons get_unfilled_cell_interiors(int layer_id) const;

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
    void compute_volumes(const std::vector<Layer*> &layers);

    // Zero-offset lattice for cell IDENTITY (a,b,c,kind) + topology (neighbors/is_up).
    // Both are offset-independent, so one cached instance serves scan_layers, the solver,
    // and the extra-vent sweep instead of each constructing its own. Lazily built.
    const MagmaLattice& topology_lattice() const;

    // Adaptive layer height helpers
    int    layer_at_height_from(int start_layer, double target_mm) const;
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
    float  m_min_layer_height;        // smallest layer height in the object
    double m_dodge_distance;          // boundary dodge distance in mm (stagger target)
    double m_max_tube_height_mm;      // max tube height in mm (drives boundary placement)
    double m_min_tube_height_mm;      // min tube height in mm
    int    m_num_layers;
    bool   m_dual_infill_enabled;
    MagmaTubeSolverMode m_solver_mode = MagmaTubeSolverMode::Refined;
    double m_solver_timeout = 20.0;
    MagmaInjectionEdgePref m_injection_edge_pref = MagmaInjectionEdgePref::Interior;

    // Triangle vertex overlap correction
    double m_overlap_flow_correction = 1.0;  // infill flow multiplier (≤1.0)
    double m_effective_line_width = 0.0;     // deposited line width after flow correction (mm)
    bool   m_overlap_line_correction = true; // config: whether to reduce line width for overlap
    std::string m_warning_message;           // non-empty if slicing warning should be shown
    std::vector<int> m_injection_layer_ids;  // sorted, deduplicated cap layer IDs
};

using MagmaTubeMapPtr = std::unique_ptr<MagmaTubeMap>;

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaTubeMap_hpp_
