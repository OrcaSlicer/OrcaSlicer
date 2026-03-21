#pragma once

#include "libslic3r.h"
#include "Layer.hpp"
#include "PrintConfig.hpp"
#include "ExPolygon.hpp"
#include "BoundingBox.hpp"

#include <vector>
#include <cmath>

namespace Slic3r {

// Per-layer geometric analysis results used by the optimizer to make decisions.
struct LayerGeometryAnalysis {
    size_t   layer_id          = 0;
    coordf_t print_z           = 0;
    coordf_t layer_height      = 0;

    // Cross-section area of the layer (mm^2)
    double   cross_section_area = 0;
    // Total perimeter length of all islands (mm)
    double   perimeter_length   = 0;
    // Number of disconnected islands in this layer
    size_t   island_count       = 0;
    // Fraction of layer area that is overhang (0.0 - 1.0)
    double   overhang_fraction  = 0;
    // Whether this layer contains any bridging regions
    bool     has_bridges        = false;
    // Area change ratio vs previous layer (> 1.0 = growing, < 1.0 = shrinking)
    double   area_change_ratio  = 1.0;
    // Thermal mass estimate: area * height (mm^3) — how much material this layer deposits
    double   thermal_mass       = 0;
    // Minimum feature width detected in this layer (mm)
    double   min_feature_width  = 0;
    // Whether this layer is a top surface (nothing above in some regions)
    bool     has_top_surfaces   = false;
    // Whether this layer is a bottom surface (nothing below in some regions)
    bool     has_bottom_surfaces = false;
    // Bounding box diagonal of the layer (mm) — proxy for print head travel extent
    double   layer_extent       = 0;
};

// Optimized per-layer print parameters computed by the AutoSliceOptimizer.
struct OptimizedLayerParams {
    // Temperature adjustments
    int      nozzle_temperature    = 0;   // 0 means "use default"
    int      bed_temperature       = 0;   // 0 means "use default"

    // Speed multipliers (1.0 = no change)
    double   speed_multiplier      = 1.0;
    double   perimeter_speed_mult  = 1.0;
    double   infill_speed_mult     = 1.0;
    double   bridge_speed_mult     = 1.0;

    // Flow adjustments
    double   flow_multiplier       = 1.0;
    double   line_width_multiplier = 1.0;

    // Cooling
    int      fan_speed             = -1;  // -1 means "use default"

    // Whether any parameters were actually modified for this layer
    bool     is_modified           = false;

    // Human-readable reason for the optimization (for G-code comments)
    std::string optimization_notes;
};

// Analyzes a single layer's geometry to produce a LayerGeometryAnalysis.
class LayerAnalyzer {
public:
    // Analyze a layer, optionally with reference to the previous layer for delta calculations.
    static LayerGeometryAnalysis analyze(const Layer& layer, const Layer* prev_layer);

private:
    // Calculate the total area of a set of ExPolygons (in mm^2, unscaled)
    static double total_area(const ExPolygons& expolygons);
    // Calculate the total perimeter length of ExPolygons (in mm, unscaled)
    static double total_perimeter_length(const ExPolygons& expolygons);
    // Estimate minimum feature width from the islands
    static double estimate_min_feature_width(const ExPolygons& expolygons);
    // Calculate overhang fraction by comparing to previous layer
    static double calculate_overhang_fraction(const ExPolygons& current, const ExPolygons& previous);
    // Check for top/bottom surfaces in a layer's regions
    static bool check_surface_type(const Layer& layer, SurfaceType type);
};

// Configuration parameters for the auto-slice optimizer, extracted from PrintConfig.
struct AutoSliceConfig {
    bool     enabled                     = false;

    // Material thermal properties
    int      nozzle_temp_base            = 200;    // Base nozzle temp from profile
    int      nozzle_temp_range_low       = 190;    // Min allowable nozzle temp
    int      nozzle_temp_range_high      = 230;    // Max allowable nozzle temp
    int      bed_temp_base               = 60;
    double   glass_transition_temp       = 60;     // Tg for the material
    double   filament_max_volumetric_speed = 15.0; // mm^3/s

    // Machine capabilities
    double   nozzle_diameter             = 0.4;
    double   max_print_speed             = 300;    // mm/s
    double   max_accel                   = 5000;   // mm/s^2

    // Cooling parameters
    double   min_layer_time              = 5.0;    // seconds
    int      fan_speed_min               = 0;
    int      fan_speed_max               = 100;
    int      full_fan_speed_layer        = 4;

    // Chamber info
    int      chamber_temperature         = 0;      // 0 = no chamber heating

    // Quality targets
    double   target_quality              = 0.8;    // 0.0 = speed priority, 1.0 = quality priority
};

// The main auto-slice optimization engine.
// Analyzes all layers of a print object and computes optimal per-layer parameters
// based on geometry, material properties, machine capabilities, and thermal conditions.
class AutoSliceOptimizer {
public:
    explicit AutoSliceOptimizer(const AutoSliceConfig& config);

    // Analyze all layers and compute optimized parameters.
    // Call this after slicing is complete but before G-code generation.
    void optimize(const std::vector<Layer*>& layers);

    // Get the optimized parameters for a specific layer.
    // Returns default (unmodified) params if the layer was not analyzed.
    const OptimizedLayerParams& get_layer_params(size_t layer_id) const;

    // Get the geometry analysis for a specific layer.
    const LayerGeometryAnalysis& get_layer_analysis(size_t layer_id) const;

    // Check if optimization is enabled and has been run.
    bool is_active() const { return m_config.enabled && !m_layer_params.empty(); }

    // Generate a G-code comment block summarizing the optimization for a layer.
    std::string generate_layer_comment(size_t layer_id) const;

    // Build an AutoSliceConfig from the full print config.
    static AutoSliceConfig build_config(const FullPrintConfig& print_config);

private:
    AutoSliceConfig m_config;
    std::vector<LayerGeometryAnalysis> m_analyses;
    std::vector<OptimizedLayerParams>  m_layer_params;

    // Default params returned when layer_id is out of range
    static const OptimizedLayerParams  s_default_params;
    static const LayerGeometryAnalysis s_default_analysis;

    // Core optimization algorithms
    void compute_temperature_optimization();
    void compute_speed_optimization();
    void compute_flow_optimization();
    void compute_cooling_optimization();

    // Helpers
    double estimate_layer_print_time(const LayerGeometryAnalysis& analysis) const;
    double compute_thermal_load_factor(size_t layer_idx) const;
    int    clamp_temperature(int temp) const;
    double clamp_speed_multiplier(double mult) const;
};

} // namespace Slic3r
