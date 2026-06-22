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

// Per-cell layer presence and interior area
struct CellPresence {
    int first_layer = INT_MAX;   // first layer where cell exists in magma region
    int last_layer  = -1;        // last layer where cell exists in magma region
    std::vector<bool>   layers;  // indexed [layer_id - first_layer]
    std::vector<double> areas;   // parallel, interior area in scaled^2 units
    std::vector<double> distances; // parallel, distance to nearest boundary (mm, unscaled)

    bool   present(int layer_id) const;
    double area(int layer_id) const;
    double distance(int layer_id) const;
    void   mark_present(int layer_id, double area_val, double dist_val = 0.0);
};

// A U-tube pair assignment
struct UTubePair {
    TriangleCell cell_a;          // injection side
    TriangleCell cell_b;          // vent side
    int  pair_start_layer;        // first layer of this tube segment
    int  pair_end_layer;          // last layer of this tube segment (inclusive)
    double window_end_z;          // Z coordinate (mm) where the window ends — pure mm, no rounding
    double volume_mm3;            // injection volume (both cells combined)

    // Pre-computed during build (avoids lattice + binary search at G-code time)
    Vec2d  injection_center;      // XY center for injection (cell_a centroid at cap layer)
    int    window_center_layer;   // center layer of window gap
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
    // Centre-to-centre distance to an edge-sharing neighbour cell (injection crater
    // clearance), via the active pattern's geometry.
    double neighbor_centroid_distance() const;

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
    void detect_constrictions();
    void assign_tubes(ProgressFn progress_fn, ThrowIfCanceled throw_if_canceled);
    void precompute_window_end_z();
    void precompute_injection_data();
    void compute_volumes(const std::vector<Layer*> &layers);

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
