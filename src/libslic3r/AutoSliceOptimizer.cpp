#include "AutoSliceOptimizer.hpp"
#include "ClipperUtils.hpp"

#include <algorithm>
#include <numeric>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace Slic3r {

// Static defaults
const OptimizedLayerParams  AutoSliceOptimizer::s_default_params  = {};
const LayerGeometryAnalysis AutoSliceOptimizer::s_default_analysis = {};

// ============================================================================
// LayerAnalyzer implementation
// ============================================================================

double LayerAnalyzer::total_area(const ExPolygons& expolygons)
{
    double area = 0;
    for (const auto& expoly : expolygons)
        area += expoly.area();
    // area() returns in scaled coordinates squared, convert to mm^2
    return std::abs(area) * SCALING_FACTOR * SCALING_FACTOR;
}

double LayerAnalyzer::total_perimeter_length(const ExPolygons& expolygons)
{
    double length = 0;
    for (const auto& expoly : expolygons) {
        length += expoly.contour.length();
        for (const auto& hole : expoly.holes)
            length += hole.length();
    }
    return length * SCALING_FACTOR;
}

double LayerAnalyzer::estimate_min_feature_width(const ExPolygons& expolygons)
{
    if (expolygons.empty())
        return 0;

    double min_width = std::numeric_limits<double>::max();
    for (const auto& expoly : expolygons) {
        BoundingBox bbox = get_extents(expoly);
        double w = bbox.size()(0) * SCALING_FACTOR;
        double h = bbox.size()(1) * SCALING_FACTOR;
        double local_min = std::min(w, h);
        if (local_min > 0 && local_min < min_width)
            min_width = local_min;
    }
    return min_width == std::numeric_limits<double>::max() ? 0 : min_width;
}

double LayerAnalyzer::calculate_overhang_fraction(const ExPolygons& current, const ExPolygons& previous)
{
    if (current.empty() || previous.empty())
        return current.empty() ? 0 : 1.0;

    double current_area = total_area(current);
    if (current_area < 1e-6)
        return 0;

    // Compute the difference: areas in current that are NOT in previous
    ExPolygons overhang = diff_ex(current, previous);
    double overhang_area = total_area(overhang);

    return std::min(overhang_area / current_area, 1.0);
}

bool LayerAnalyzer::check_surface_type(const Layer& layer, SurfaceType type)
{
    for (const LayerRegion* region : layer.regions()) {
        for (const Surface& surface : region->slices.surfaces) {
            if (surface.surface_type == type)
                return true;
        }
    }
    return false;
}

LayerGeometryAnalysis LayerAnalyzer::analyze(const Layer& layer, const Layer* prev_layer)
{
    LayerGeometryAnalysis result;
    result.layer_id     = layer.id();
    result.print_z      = layer.print_z;
    result.layer_height = layer.height;

    // Cross-section geometry
    result.cross_section_area = total_area(layer.lslices);
    result.perimeter_length   = total_perimeter_length(layer.lslices);
    result.island_count       = layer.lslices.size();
    result.min_feature_width  = estimate_min_feature_width(layer.lslices);

    // Thermal mass: volume of material deposited in this layer
    result.thermal_mass = result.cross_section_area * layer.height;

    // Bounding box extent
    if (!layer.lslices.empty()) {
        BoundingBox bbox = get_extents(layer.lslices);
        double dx = bbox.size()(0) * SCALING_FACTOR;
        double dy = bbox.size()(1) * SCALING_FACTOR;
        result.layer_extent = std::sqrt(dx * dx + dy * dy);
    }

    // Overhang analysis (compared to previous layer)
    if (prev_layer && !prev_layer->lslices.empty()) {
        result.overhang_fraction = calculate_overhang_fraction(layer.lslices, prev_layer->lslices);
        double prev_area = total_area(prev_layer->lslices);
        result.area_change_ratio = (prev_area > 1e-6) ? result.cross_section_area / prev_area : 1.0;
    } else {
        result.overhang_fraction = 0;
        result.area_change_ratio = 1.0;
    }

    // Bridge detection
    result.has_bridges = false;
    for (const LayerRegion* region : layer.regions()) {
        if (!region->unsupported_bridge_edges.empty()) {
            result.has_bridges = true;
            break;
        }
    }

    // Surface type detection
    result.has_top_surfaces    = check_surface_type(layer, stTop);
    result.has_bottom_surfaces = check_surface_type(layer, stBottom) ||
                                 check_surface_type(layer, stBottomBridge);

    return result;
}

// ============================================================================
// AutoSliceOptimizer implementation
// ============================================================================

AutoSliceOptimizer::AutoSliceOptimizer(const AutoSliceConfig& config)
    : m_config(config)
{
}

void AutoSliceOptimizer::optimize(const std::vector<Layer*>& layers)
{
    if (!m_config.enabled || layers.empty())
        return;

    // Phase 1: Analyze all layer geometries
    m_analyses.clear();
    m_analyses.reserve(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        const Layer* prev = (i > 0) ? layers[i - 1] : nullptr;
        m_analyses.push_back(LayerAnalyzer::analyze(*layers[i], prev));
    }

    // Phase 2: Initialize optimized params
    m_layer_params.clear();
    m_layer_params.resize(layers.size());

    // Phase 3: Run optimization passes
    compute_temperature_optimization();
    compute_speed_optimization();
    compute_flow_optimization();
    compute_cooling_optimization();
}

const OptimizedLayerParams& AutoSliceOptimizer::get_layer_params(size_t layer_id) const
{
    if (layer_id < m_layer_params.size())
        return m_layer_params[layer_id];
    return s_default_params;
}

const LayerGeometryAnalysis& AutoSliceOptimizer::get_layer_analysis(size_t layer_id) const
{
    if (layer_id < m_analyses.size())
        return m_analyses[layer_id];
    return s_default_analysis;
}

// ============================================================================
// Temperature optimization
// ============================================================================
// Adjusts nozzle temperature based on:
// 1. Thermal load (large layers need more heat to maintain flow)
// 2. Overhang fraction (reduce temp for better bridging/overhang quality)
// 3. Small features (reduce temp for detail preservation)
// 4. Layer time (short layers = less cooling time = lower temp needed)
// 5. Chamber temperature compensation

void AutoSliceOptimizer::compute_temperature_optimization()
{
    if (m_analyses.empty())
        return;

    // Compute reference thermal mass (median across all layers)
    std::vector<double> masses;
    masses.reserve(m_analyses.size());
    for (const auto& a : m_analyses)
        if (a.thermal_mass > 0)
            masses.push_back(a.thermal_mass);

    if (masses.empty())
        return;

    std::sort(masses.begin(), masses.end());
    double median_mass = masses[masses.size() / 2];

    for (size_t i = 0; i < m_analyses.size(); ++i) {
        const auto& analysis = m_analyses[i];
        auto& params = m_layer_params[i];

        int temp_adjustment = 0;
        std::string notes;

        // 1. Thermal load factor: large cross-sections need more heat
        //    Layers with more material act as heat sinks, pulling temp from the nozzle
        if (median_mass > 0 && analysis.thermal_mass > 0) {
            double load_ratio = analysis.thermal_mass / median_mass;
            // Large layers (load_ratio > 1.5): bump temp up to +8C
            // Small layers (load_ratio < 0.5): drop temp up to -5C
            if (load_ratio > 1.2) {
                int bump = static_cast<int>(std::min((load_ratio - 1.0) * 10.0, 8.0));
                temp_adjustment += bump;
                if (bump != 0) notes += "thermal_load:+" + std::to_string(bump) + "C ";
            } else if (load_ratio < 0.5) {
                int drop = static_cast<int>(std::min((1.0 - load_ratio) * 8.0, 5.0));
                temp_adjustment -= drop;
                if (drop != 0) notes += "low_mass:-" + std::to_string(drop) + "C ";
            }
        }

        // 2. Overhang compensation: reduce temperature for better overhang quality
        //    Lower temps = faster solidification = less droop
        if (analysis.overhang_fraction > 0.15) {
            int overhang_drop = static_cast<int>(analysis.overhang_fraction * 12.0);
            overhang_drop = std::min(overhang_drop, 10);
            temp_adjustment -= overhang_drop;
            notes += "overhang:-" + std::to_string(overhang_drop) + "C ";
        }

        // 3. Bridge compensation: significant temp reduction for bridging
        if (analysis.has_bridges) {
            temp_adjustment -= 8;
            notes += "bridge:-8C ";
        }

        // 4. Small feature preservation: lower temp for thin walls/small details
        if (analysis.min_feature_width > 0 && analysis.min_feature_width < m_config.nozzle_diameter * 3.0) {
            int detail_drop = static_cast<int>((1.0 - analysis.min_feature_width / (m_config.nozzle_diameter * 3.0)) * 6.0);
            temp_adjustment -= detail_drop;
            if (detail_drop != 0) notes += "detail:-" + std::to_string(detail_drop) + "C ";
        }

        // 5. Chamber temperature compensation: if chamber is heated,
        //    the part retains more heat, so we can reduce nozzle temp slightly
        if (m_config.chamber_temperature > 40) {
            int chamber_offset = static_cast<int>((m_config.chamber_temperature - 40) * 0.15);
            chamber_offset = std::min(chamber_offset, 5);
            temp_adjustment -= chamber_offset;
            if (chamber_offset != 0) notes += "chamber:-" + std::to_string(chamber_offset) + "C ";
        }

        // 6. Top surface quality boost: slightly higher temp for better layer adhesion on top
        if (analysis.has_top_surfaces && !analysis.has_bridges) {
            temp_adjustment += 3;
            notes += "top_surface:+3C ";
        }

        // Apply quality factor weighting
        // Higher quality target = more aggressive optimization
        // Lower quality target = stay closer to defaults
        temp_adjustment = static_cast<int>(temp_adjustment * m_config.target_quality);

        if (temp_adjustment != 0) {
            params.nozzle_temperature = clamp_temperature(m_config.nozzle_temp_base + temp_adjustment);
            params.is_modified = true;
            params.optimization_notes += notes;
        }
    }
}

// ============================================================================
// Speed optimization
// ============================================================================
// Adjusts print speed based on:
// 1. Layer area (small layers = slow down for cooling time)
// 2. Overhang regions (slow down for quality)
// 3. Island count (many islands = more travel, less benefit from high speed)
// 4. Volumetric flow limits (respect material max flow rate)
// 5. First layer / bottom layers (always slower)

void AutoSliceOptimizer::compute_speed_optimization()
{
    if (m_analyses.empty())
        return;

    // Reference area: median cross-section
    std::vector<double> areas;
    areas.reserve(m_analyses.size());
    for (const auto& a : m_analyses)
        if (a.cross_section_area > 0)
            areas.push_back(a.cross_section_area);

    if (areas.empty())
        return;

    std::sort(areas.begin(), areas.end());
    double median_area = areas[areas.size() / 2];

    for (size_t i = 0; i < m_analyses.size(); ++i) {
        const auto& analysis = m_analyses[i];
        auto& params = m_layer_params[i];

        double speed_mult = 1.0;
        double perim_mult = 1.0;
        double infill_mult = 1.0;
        std::string notes;

        // 1. Minimum layer time enforcement
        //    If estimated print time is short, slow down to allow cooling
        double est_time = estimate_layer_print_time(analysis);
        if (est_time > 0 && est_time < m_config.min_layer_time) {
            double time_factor = m_config.min_layer_time / est_time;
            // Don't slow down more than 3x
            time_factor = std::min(time_factor, 3.0);
            speed_mult /= time_factor;
            notes += "min_time:x" + std::to_string(1.0 / time_factor).substr(0, 4) + " ";
        }

        // 2. Small layer area: slow down perimeters for cooling
        if (median_area > 0 && analysis.cross_section_area > 0) {
            double area_ratio = analysis.cross_section_area / median_area;
            if (area_ratio < 0.3) {
                // Very small layer - significantly slow down
                perim_mult *= std::max(0.4, area_ratio / 0.3);
                notes += "small_area:perim_slow ";
            }
        }

        // 3. Overhang speed reduction
        if (analysis.overhang_fraction > 0.1) {
            double overhang_slowdown = 1.0 - analysis.overhang_fraction * 0.5;
            overhang_slowdown = std::max(overhang_slowdown, 0.5);
            perim_mult *= overhang_slowdown;
            notes += "overhang:perim_slow ";
        }

        // 4. Bridge speed: use a controlled speed for bridging
        double bridge_mult = 1.0;
        if (analysis.has_bridges) {
            bridge_mult = 0.6; // 60% of normal speed for bridges
            notes += "bridge:slow ";
        }

        // 5. Volumetric flow rate limiting
        //    Ensure we don't exceed the material's max volumetric speed
        if (m_config.filament_max_volumetric_speed > 0 && analysis.layer_height > 0) {
            // Approximate max speed from volumetric limit:
            // volumetric_speed = width * height * speed
            // speed_max = volumetric_speed / (width * height)
            double max_flow_speed = m_config.filament_max_volumetric_speed /
                                    (m_config.nozzle_diameter * analysis.layer_height);
            double flow_limit_mult = std::min(1.0, max_flow_speed / m_config.max_print_speed);
            if (flow_limit_mult < 1.0) {
                speed_mult *= flow_limit_mult;
                notes += "flow_limit:x" + std::to_string(flow_limit_mult).substr(0, 4) + " ";
            }
        }

        // 6. Area change rate: rapid changes = potential quality issues
        if (analysis.area_change_ratio > 2.0 || analysis.area_change_ratio < 0.5) {
            perim_mult *= 0.85;
            notes += "area_change:slow ";
        }

        // Apply quality weighting: higher quality = more aggressive speed reductions
        // Lower quality = closer to full speed
        auto apply_quality = [&](double mult) -> double {
            return 1.0 + (mult - 1.0) * m_config.target_quality;
        };

        speed_mult  = apply_quality(speed_mult);
        perim_mult  = apply_quality(perim_mult);
        infill_mult = apply_quality(infill_mult);
        bridge_mult = apply_quality(bridge_mult);

        // Clamp all multipliers
        speed_mult  = clamp_speed_multiplier(speed_mult);
        perim_mult  = clamp_speed_multiplier(perim_mult);
        infill_mult = clamp_speed_multiplier(infill_mult);
        bridge_mult = clamp_speed_multiplier(bridge_mult);

        bool modified = (std::abs(speed_mult - 1.0) > 0.01 ||
                         std::abs(perim_mult - 1.0) > 0.01 ||
                         std::abs(infill_mult - 1.0) > 0.01 ||
                         std::abs(bridge_mult - 1.0) > 0.01);

        if (modified) {
            params.speed_multiplier     = speed_mult;
            params.perimeter_speed_mult = perim_mult;
            params.infill_speed_mult    = infill_mult;
            params.bridge_speed_mult    = bridge_mult;
            params.is_modified          = true;
            params.optimization_notes  += notes;
        }
    }
}

// ============================================================================
// Flow optimization
// ============================================================================
// Adjusts extrusion flow and line width:
// 1. Overhang regions: reduce flow to prevent drooping
// 2. Bridging: specific bridge flow adjustments
// 3. Top surfaces: fine-tune flow for surface quality
// 4. Area transitions: adjust flow during rapid geometry changes

void AutoSliceOptimizer::compute_flow_optimization()
{
    for (size_t i = 0; i < m_analyses.size(); ++i) {
        const auto& analysis = m_analyses[i];
        auto& params = m_layer_params[i];

        double flow_mult = 1.0;
        double width_mult = 1.0;
        std::string notes;

        // 1. Overhang flow reduction
        //    Less material on overhangs = better quality, less drooping
        if (analysis.overhang_fraction > 0.2) {
            double overhang_flow = 1.0 - analysis.overhang_fraction * 0.2;
            overhang_flow = std::max(overhang_flow, 0.8);
            flow_mult *= overhang_flow;
            notes += "overhang_flow:" + std::to_string(overhang_flow).substr(0, 4) + " ";
        }

        // 2. Bridge flow: typically reduced to create taut strands
        if (analysis.has_bridges) {
            flow_mult *= 0.90;
            notes += "bridge_flow:0.90 ";
        }

        // 3. Top surface: slightly reduced flow for smoother finish
        if (analysis.has_top_surfaces && !analysis.has_bridges) {
            flow_mult *= 0.97;
            notes += "top_flow:0.97 ";
        }

        // 4. Small features: slightly increase line width for adhesion
        if (analysis.min_feature_width > 0 &&
            analysis.min_feature_width < m_config.nozzle_diameter * 2.5) {
            width_mult *= 1.05;
            notes += "thin_wall_width:1.05 ";
        }

        // 5. Large area change: slight flow boost to improve layer adhesion
        //    when transitioning from small to large cross section
        if (analysis.area_change_ratio > 1.8) {
            flow_mult *= 1.02;
            notes += "growth_flow:1.02 ";
        }

        // Apply quality weighting
        flow_mult  = 1.0 + (flow_mult - 1.0) * m_config.target_quality;
        width_mult = 1.0 + (width_mult - 1.0) * m_config.target_quality;

        // Clamp
        flow_mult  = std::clamp(flow_mult, 0.75, 1.15);
        width_mult = std::clamp(width_mult, 0.85, 1.15);

        bool modified = (std::abs(flow_mult - 1.0) > 0.005 ||
                         std::abs(width_mult - 1.0) > 0.005);

        if (modified) {
            params.flow_multiplier       = flow_mult;
            params.line_width_multiplier = width_mult;
            params.is_modified           = true;
            params.optimization_notes   += notes;
        }
    }
}

// ============================================================================
// Cooling optimization
// ============================================================================
// Adjusts fan speed per layer:
// 1. Small cross-section layers need maximum cooling
// 2. Bridging layers need high fan for strand solidification
// 3. Large layers with good thermal mass need less fan
// 4. First few layers: respect the "full fan speed layer" setting
// 5. Overhang regions benefit from extra cooling

void AutoSliceOptimizer::compute_cooling_optimization()
{
    if (m_analyses.empty())
        return;

    // Reference area
    std::vector<double> areas;
    for (const auto& a : m_analyses)
        if (a.cross_section_area > 0)
            areas.push_back(a.cross_section_area);

    if (areas.empty())
        return;

    std::sort(areas.begin(), areas.end());
    double median_area = areas[areas.size() / 2];

    for (size_t i = 0; i < m_analyses.size(); ++i) {
        const auto& analysis = m_analyses[i];
        auto& params = m_layer_params[i];

        // Start with default fan behavior for first layers
        if (static_cast<int>(i) < m_config.full_fan_speed_layer)
            continue;

        int fan_speed = -1; // -1 = default
        std::string notes;

        // Base fan speed: inversely proportional to cross-section area
        // Small layers = high fan, large layers = lower fan
        if (median_area > 0 && analysis.cross_section_area > 0) {
            double area_ratio = analysis.cross_section_area / median_area;
            if (area_ratio < 0.5) {
                // Small layer: needs more cooling
                fan_speed = m_config.fan_speed_max;
                notes += "small_layer:max_fan ";
            } else if (area_ratio < 0.8) {
                // Medium-small: interpolate toward max
                double t = (0.8 - area_ratio) / 0.3;
                fan_speed = m_config.fan_speed_min +
                    static_cast<int>(t * (m_config.fan_speed_max - m_config.fan_speed_min));
                notes += "med_small:fan=" + std::to_string(fan_speed) + " ";
            }
        }

        // Bridges: always max fan
        if (analysis.has_bridges) {
            fan_speed = m_config.fan_speed_max;
            notes += "bridge:max_fan ";
        }

        // Overhangs: boost fan
        if (analysis.overhang_fraction > 0.2 && fan_speed < m_config.fan_speed_max) {
            int overhang_fan = m_config.fan_speed_min +
                static_cast<int>(analysis.overhang_fraction *
                    (m_config.fan_speed_max - m_config.fan_speed_min));
            fan_speed = std::max(fan_speed, overhang_fan);
            notes += "overhang:fan=" + std::to_string(fan_speed) + " ";
        }

        // Chamber temperature consideration: with heated chamber,
        // external fan is less effective and may cause warping
        if (m_config.chamber_temperature > 50 && fan_speed > 0) {
            int reduction = static_cast<int>((m_config.chamber_temperature - 50) * 0.5);
            fan_speed = std::max(0, fan_speed - reduction);
            if (reduction > 0) notes += "chamber:fan-" + std::to_string(reduction) + " ";
        }

        if (fan_speed >= 0) {
            fan_speed = std::clamp(fan_speed, 0, 100);
            params.fan_speed          = fan_speed;
            params.is_modified        = true;
            params.optimization_notes += notes;
        }
    }
}

// ============================================================================
// Helper methods
// ============================================================================

double AutoSliceOptimizer::estimate_layer_print_time(const LayerGeometryAnalysis& analysis) const
{
    if (analysis.cross_section_area < 1e-6)
        return 0;

    // Very rough estimate: total extrusion length * (1 / speed)
    // Extrusion length ~ (area / line_width) + perimeter_length
    double line_width = m_config.nozzle_diameter * 1.1; // typical
    double fill_length = analysis.cross_section_area / line_width;
    double total_length = fill_length + analysis.perimeter_length;
    double avg_speed = m_config.max_print_speed * 0.5; // assume ~50% of max on average

    return (avg_speed > 0) ? total_length / avg_speed : 0;
}

double AutoSliceOptimizer::compute_thermal_load_factor(size_t layer_idx) const
{
    if (layer_idx >= m_analyses.size())
        return 1.0;

    // Look at a window of layers around the current one to estimate heat accumulation
    constexpr size_t window = 5;
    size_t start = (layer_idx > window) ? layer_idx - window : 0;
    size_t end   = std::min(layer_idx + window + 1, m_analyses.size());

    double total_mass = 0;
    for (size_t i = start; i < end; ++i)
        total_mass += m_analyses[i].thermal_mass;

    double avg_mass = total_mass / (end - start);
    return (avg_mass > 0) ? m_analyses[layer_idx].thermal_mass / avg_mass : 1.0;
}

int AutoSliceOptimizer::clamp_temperature(int temp) const
{
    return std::clamp(temp, m_config.nozzle_temp_range_low, m_config.nozzle_temp_range_high);
}

double AutoSliceOptimizer::clamp_speed_multiplier(double mult) const
{
    return std::clamp(mult, 0.3, 1.5);
}

std::string AutoSliceOptimizer::generate_layer_comment(size_t layer_id) const
{
    if (layer_id >= m_layer_params.size())
        return "";

    const auto& params = m_layer_params[layer_id];
    if (!params.is_modified)
        return "";

    std::ostringstream ss;
    ss << "; AUTO_SLICE_OPT layer " << layer_id << ": ";

    if (params.nozzle_temperature > 0)
        ss << "temp=" << params.nozzle_temperature << "C ";
    if (std::abs(params.speed_multiplier - 1.0) > 0.01)
        ss << "speed=x" << std::fixed << std::setprecision(2) << params.speed_multiplier << " ";
    if (std::abs(params.perimeter_speed_mult - 1.0) > 0.01)
        ss << "perim_speed=x" << std::fixed << std::setprecision(2) << params.perimeter_speed_mult << " ";
    if (std::abs(params.flow_multiplier - 1.0) > 0.005)
        ss << "flow=x" << std::fixed << std::setprecision(3) << params.flow_multiplier << " ";
    if (params.fan_speed >= 0)
        ss << "fan=" << params.fan_speed << "% ";

    ss << "[" << params.optimization_notes << "]";
    return ss.str() + "\n";
}

AutoSliceConfig AutoSliceOptimizer::build_config(const FullPrintConfig& print_config)
{
    AutoSliceConfig config;

    // Check if auto-slice is enabled
    config.enabled = print_config.auto_slice_optimization.value;

    if (!config.enabled)
        return config;

    // Material temperatures
    config.nozzle_temp_base = print_config.nozzle_temperature.get_at(0);
    if (!print_config.nozzle_temperature_range_low.values.empty())
        config.nozzle_temp_range_low = print_config.nozzle_temperature_range_low.get_at(0);
    if (!print_config.nozzle_temperature_range_high.values.empty())
        config.nozzle_temp_range_high = print_config.nozzle_temperature_range_high.get_at(0);

    // Filament properties
    if (!print_config.temperature_vitrification.values.empty())
        config.glass_transition_temp = print_config.temperature_vitrification.get_at(0);
    if (!print_config.filament_max_volumetric_speed.values.empty())
        config.filament_max_volumetric_speed = print_config.filament_max_volumetric_speed.get_at(0);

    // Machine capabilities
    config.nozzle_diameter = print_config.nozzle_diameter.get_at(0);
    if (!print_config.machine_max_speed_x.values.empty())
        config.max_print_speed = std::min(print_config.machine_max_speed_x.get_at(0),
                                          print_config.machine_max_speed_y.get_at(0));
    if (!print_config.machine_max_acceleration_extruding.values.empty())
        config.max_accel = print_config.machine_max_acceleration_extruding.get_at(0);

    // Cooling
    if (!print_config.slow_down_layer_time.values.empty())
        config.min_layer_time = print_config.slow_down_layer_time.get_at(0);
    if (!print_config.fan_min_speed.values.empty())
        config.fan_speed_min = static_cast<int>(print_config.fan_min_speed.get_at(0));
    if (!print_config.fan_max_speed.values.empty())
        config.fan_speed_max = static_cast<int>(print_config.fan_max_speed.get_at(0));
    if (!print_config.full_fan_speed_layer.values.empty())
        config.full_fan_speed_layer = print_config.full_fan_speed_layer.get_at(0);

    // Chamber temperature
    if (!print_config.chamber_temperature.values.empty())
        config.chamber_temperature = print_config.chamber_temperature.get_at(0);

    // Quality target
    config.target_quality = print_config.auto_slice_quality_target.value;

    return config;
}

} // namespace Slic3r
