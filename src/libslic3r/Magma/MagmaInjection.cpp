#include "MagmaInjection.hpp"
#include "MagmaTubeMap.hpp"

#include "../GCode.hpp"
#include "../GCode/GCodeProcessor.hpp"
#include "../ShortestPath.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include <igl/ramer_douglas_peucker.h>

namespace Slic3r {
namespace magma {

std::vector<InjectionPoint> collect_injection_points(
    const MagmaTubeMap& tube_map,
    int layer_id)
{
    std::vector<InjectionPoint> points;

    const auto& pairs = tube_map.u_tube_pairs();
    for (int i = 0; i < static_cast<int>(pairs.size()); ++i) {
        const auto& pair = pairs[i];
        if (pair.pair_end_layer != layer_id || pair.volume_mm3 <= 0)
            continue;
        points.push_back({pair.injection_center, pair.volume_mm3,
                          i, pair.pair_start_layer, pair.window_center_layer});
    }

    // Order injection points for minimal travel using OrcaSlicer's greedy
    // TSP solver (KD-tree backed).  Much better than a raster sweep for
    // irregular point distributions near model boundaries.
    if (points.size() > 1) {
        Points scaled_pts;
        scaled_pts.reserve(points.size());
        for (const auto& pt : points)
            scaled_pts.push_back(Point(scale_(pt.position.x()), scale_(pt.position.y())));
        std::vector<size_t> order = chain_points(scaled_pts);
        std::vector<InjectionPoint> ordered;
        ordered.reserve(points.size());
        for (size_t idx : order)
            ordered.push_back(std::move(points[idx]));
        points = std::move(ordered);
    }

    return points;
}

// Generate the spiral-following center-line of a U-tube and simplify with
// Douglas-Peucker.  Returns waypoints tracing: top of cell_a → descent →
// window crossing → ascent → top of cell_b.
static std::vector<Vec3d> build_tube_viz_waypoints(
    const MagmaTubeMap& tube_map,
    const UTubePair& pair,
    double layer_z,         // Z at pair_end_layer
    int    window_center_layer)
{
    float  iw = tube_map.interior_width();

    // Helper: Z height for a given layer index (uses actual per-layer print_z)
    auto z_for_layer = [&](int L) -> double {
        return tube_map.print_z(L);
    };

    // Z offsets: raise tube bottom so rendered cylinder doesn't poke through floor
    double z_bot_raw = z_for_layer(pair.pair_start_layer);
    double z_bot = z_bot_raw + iw / 2.0;  // sit ON the floor
    double z_window = z_for_layer(window_center_layer);
    // Clamp window Z above the raised bottom
    if (z_window < z_bot)
        z_window = z_bot;

    std::vector<Vec3d> full_path;

    // Phase 1: Descend through cell A (top → window center)
    for (int L = pair.pair_end_layer; L >= window_center_layer; --L) {
        Vec2d c = tube_map.lattice_at(L).cell_center(pair.cell_a);
        double z = (L == pair.pair_end_layer) ? layer_z :
                   (L <= window_center_layer) ? z_window :
                   std::max(z_for_layer(L), z_bot);
        full_path.push_back({c.x(), c.y(), z});
    }

    // Phase 2: Cross to cell B at window center Z
    {
        Vec2d c = tube_map.lattice_at(window_center_layer).cell_center(pair.cell_b);
        full_path.push_back({c.x(), c.y(), z_window});
    }

    // Phase 3: Ascend through cell B (window center → top)
    for (int L = window_center_layer + 1; L <= pair.pair_end_layer; ++L) {
        Vec2d c = tube_map.lattice_at(L).cell_center(pair.cell_b);
        double z = (L == pair.pair_end_layer) ? layer_z :
                   std::max(z_for_layer(L), z_bot);
        full_path.push_back({c.x(), c.y(), z});
    }

    // Simplify with 3D Ramer-Douglas-Peucker.
    // When spirals are off, each phase is a straight line so RDP reduces
    // ~20-120 points down to ~5 (top_A, bot_A, window_B, bot_B, top_B).
    // When spirals are on, RDP keeps points where the helix bends noticeably.
    if (full_path.size() > 2) {
        Eigen::MatrixXd P(full_path.size(), 3);
        for (size_t i = 0; i < full_path.size(); ++i)
            P.row(i) = full_path[i].transpose();

        Eigen::MatrixXd S;
        Eigen::VectorXi J;
        igl::ramer_douglas_peucker(P, double(iw) * 0.1, S, J);

        std::vector<Vec3d> simplified;
        simplified.reserve(S.rows());
        for (int i = 0; i < S.rows(); ++i)
            simplified.push_back(S.row(i).transpose());
        return simplified;
    }
    return full_path;
}

// Format waypoints as a MAGMA_TUBE G-code comment.
static std::string format_tube_viz_comment(const std::vector<Vec3d>& waypoints, float width)
{
    std::ostringstream oss;
    oss << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Magma_Tube)
        << "n=" << waypoints.size() << " w=" << width << " pts=";
    for (size_t i = 0; i < waypoints.size(); ++i) {
        if (i > 0) oss << ';';
        char buf[64];
        snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f",
                 waypoints[i].x(), waypoints[i].y(), waypoints[i].z());
        oss << buf;
    }
    oss << '\n';
    return oss.str();
}

// Park at a safe position and change nozzle temperature, waiting until reached.
// Handles z-hop, XY park, extra retract/unretract around the temperature wait.
// Find park position and generate temp change G-code using shared SafeParkPosition.
std::string park_and_set_temp(
    GCode& gcodegen,
    bool park_enabled,
    double layer_z,
    double park_z_hop,
    double extra_retract,
    int target_temp,
    const char* z_comment,
    const char* xy_comment)
{
    if (park_enabled) {
        ParkResult park = gcodegen.safe_park().find_safe_position(
            gcodegen.layer(), nullptr, gcodegen.last_pos());
        return SafeParkPosition::park_and_set_temp(
            gcodegen, park, layer_z, park_z_hop, extra_retract,
            target_temp, z_comment, xy_comment);
    }
    // Not parking — just retract and set temperature.
    std::string gcode;
    gcode += gcodegen.retract(false, false);
    gcode += gcodegen.writer().set_temperature(target_temp, true);
    return gcode;
}

std::string generate_injection_gcode(
    GCode& gcodegen,
    const MagmaTubeMap& tube_map,
    const std::vector<InjectionPoint>& points,
    double layer_z,
    double actual_layer_height)
{
    if (points.empty())
        return {};

    std::string gcode;
    const auto& config = gcodegen.config();
    char buf[256];

    // --- Config values ---
    double injection_speed_vol = config.magma_injection_speed.value;
    bool iron_tube_ends      = config.magma_iron_tube_ends.value;
    double fill_factor       = config.magma_tube_fill_factor.value;
    if (fill_factor <= 0)
        return {};

    double slam_depth        = 0.0;  // resolved below (auto mode needs extruder/nozzle info)
    int    dwell_ms          = config.magma_injection_dwell.value;
    double inj_z_hop         = config.magma_injection_z_hop.value;
    bool inj_retract         = config.magma_injection_retract.value;

    // Get extruder info (ToolOrdering handles filament switching before we get here)
    unsigned int extruder_id = gcodegen.writer().filament()->id();
    double filament_diameter = config.filament_diameter.get_at(extruder_id);
    double filament_area = (PI / 4.0) * filament_diameter * filament_diameter;

    // Resolve Z-slam depth: auto-derive from nozzle cone geometry, or use the
    // manual value. Auto presses just far enough that the cone above the flat
    // widens to cover the tube opening: z = (opening - flat) / (2*tan(half_angle)),
    // with a 0.1mm floor for seal contact even when the flat already covers it.
    if (config.magma_injection_z_slam_auto.value) {
        double opening = tube_map.tube_opening_diameter();
        double flat    = config.magma_nozzle_outer_diameter.value;
        if (flat <= 0.0)
            flat = 3.0 * config.nozzle_diameter.get_at(extruder_id);  // matches auto tube-sizing fallback
        double half_angle_rad = config.magma_nozzle_cone_half_angle.value * PI / 180.0;
        double gap = opening - flat;
        double computed = (gap > 0.0 && half_angle_rad > 1e-6)
                              ? gap / (2.0 * std::tan(half_angle_rad)) : 0.0;
        slam_depth = std::max(0.1, computed);
    } else {
        slam_depth = config.magma_injection_z_slam.value;
    }
    slam_depth = std::min(slam_depth, 3.5);

    // Plunge ("slam-melt"): ramp the nozzle deeper during the injection so the hot
    // tip sinks into the softening tube top and keeps the seal pressed shut as the
    // channel fills. Ramps from slam_depth to slam_depth + plunge_depth; clamp the
    // total so we never drive absurdly far into the part.
    double plunge_depth = config.magma_injection_plunge.value
                              ? std::max(0.0, config.magma_injection_plunge_depth.value)
                              : 0.0;
    plunge_depth = std::min(plunge_depth, std::max(0.0, 4.0 - slam_depth));

    // Raw volume→E conversion: 1/cross_section, without filament_flow_ratio.
    // Injection volume is geometrically computed from tube dimensions; applying
    // flow ratio would double-correct (fill_factor already controls fill amount).
    double e_per_mm3 = 1.0 / filament_area;

    // Injection volumetric speed, capped at hotend melt rate.
    double vol_speed = std::max(1.0, injection_speed_vol);
    double max_vol = config.filament_max_volumetric_speed.get_at(extruder_id);
    if (max_vol > 0)
        vol_speed = std::min(vol_speed, max_vol);

    // Convert volumetric speed to filament feedrate
    double feedrate_mms = vol_speed / filament_area;
    double feedrate_mmmin = feedrate_mms * 60.0;

    // Z move speed for the injection slam/lift/hop moves. Inherit the printer's
    // Z travel speed (same fallback as GCodeWriter::_travel_to_z); the firmware
    // caps it at max_z_velocity. A hardcoded feedrate left the nozzle lingering
    // on the hot tube top.
    double z_speed_mms = config.travel_speed_z.value;
    if (z_speed_mms <= 0.)
        z_speed_mms = config.travel_speed.value;
    int z_feedrate = (int)(z_speed_mms * 60.0);
    if (z_feedrate < 60) z_feedrate = 60;

    // Suppress the slicer's retraction-lift for the rest of this injection pass
    // so our manual z-hop is the SOLE owner of Z. OrcaSlicer's z-hop rides on
    // retraction and is gated by retract_lift_enforce/can_lift (GCode::retract):
    // on Top-only / Bottom-only lift settings it would NOT lift mid-layer during
    // injection at all (hot nozzle dragged across the part), and when it does
    // lift it fights our manual hop. Zeroing the writer's per-filament z_hop for
    // this scope removes that interaction; the guard restores it on any exit.
    struct ZHopGuard {
        GCodeConfig&       cfg;
        ConfigOptionFloats saved;
        explicit ZHopGuard(GCodeConfig& c) : cfg(c), saved(c.z_hop) {
            std::fill(cfg.z_hop.values.begin(), cfg.z_hop.values.end(), 0.0);
        }
        ~ZHopGuard() { cfg.z_hop = saved; }
    } z_hop_guard(gcodegen.writer().config);

    // --- Injection loop ---
    // Temperature and fan markers are managed by the caller (GCode.cpp
    // injection phase) so that multiple objects share one heat/cool cycle.
    float display_dim = tube_map.interior_width();

    for (const auto& pt : points) {
        double volume = pt.volume_mm3 * fill_factor;
        if (volume <= 0)
            continue;

        // Travel to injection point.
        // travel_to() handles its own retract/z-hop for long moves.
        Point scaled_pos(scale_(pt.position.x()), scale_(pt.position.y()));
        gcode += gcodegen.travel_to(scaled_pos, erMagmaInjection, "move to injection point");

        // Return from injection z-hop to layer height.
        // (Manual Z because built-in lift doesn't support custom heights.)
        if (inj_z_hop > 0) {
            sprintf(buf, "G1 Z%.3f F%d ; injection z-hop down\n", layer_z, z_feedrate);
            gcode += buf;
        }

        // Unretract — undoes both the injection retract (state-tracked via
        // GCodeWriter::retract) and any travel retract from travel_to().
        gcode += gcodegen.unretract();

        // Z-slam: lower nozzle into surface to seal against hole
        if (slam_depth > 0) {
            sprintf(buf, "G1 Z%.3f F%d ; z-slam seal\n", layer_z - slam_depth, z_feedrate);
            gcode += buf;
        }

        // Set role and display dimensions for this injection.
        // Emitted per-injection (after unretract) so that only the actual
        // injection extrusion is classified as erMagmaInjection.
        sprintf(buf, ";%s%s\n",
                GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(),
                ExtrusionEntity::role_to_string(erMagmaInjection).c_str());
        gcode += buf;
        sprintf(buf, ";%s%g\n",
                GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height).c_str(),
                display_dim);
        gcode += buf;
        sprintf(buf, ";%s%g\n",
                GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width).c_str(),
                display_dim);
        gcode += buf;

        // Build tube visualization waypoints (simplified with 3D RDP)
        const auto& pair = tube_map.u_tube_pairs()[pt.pair_index];
        auto waypoints = build_tube_viz_waypoints(
            tube_map, pair, layer_z, pt.window_center_layer);

        // Emit tube visualization metadata for GCodeProcessor
        if (!waypoints.empty())
            gcode += format_tube_viz_comment(waypoints, tube_map.interior_width());

        // Split injection into per-segment G1 E commands so the preview
        // slider shows progressive tube filling.  Each G1 gets its own
        // G-code line number, giving it a separate slider position.
        double filament_length = volume * e_per_mm3;
        gcode += gcodegen.writer().set_speed(feedrate_mmmin);
        Vec2d xy(gcodegen.writer().get_position().x(),
                 gcodegen.writer().get_position().y());

        // Per-segment extrusion amounts. With viz waypoints we split E along the
        // tube path (drives the preview slider); otherwise a single segment, or,
        // when plunging, a fixed split so Z can ramp smoothly.
        std::vector<double> seg_e;
        if (waypoints.size() >= 2) {
            double total_path_len = 0;
            for (size_t i = 1; i < waypoints.size(); ++i)
                total_path_len += (waypoints[i] - waypoints[i - 1]).norm();
            for (size_t i = 1; i < waypoints.size(); ++i) {
                double seg_len = (waypoints[i] - waypoints[i - 1]).norm();
                seg_e.push_back((total_path_len > 0)
                    ? filament_length * (seg_len / total_path_len)
                    : filament_length / double(waypoints.size() - 1));
            }
        } else if (plunge_depth > 0.0) {
            const int N = 8;
            for (int i = 0; i < N; ++i)
                seg_e.push_back(filament_length / double(N));
        } else {
            seg_e.push_back(filament_length);
        }

        // Emit the segments, optionally sinking Z a little before each so the
        // nozzle plunges from slam_depth to slam_depth + plunge_depth by the end.
        const int K = (int) seg_e.size();
        for (int k = 0; k < K; ++k) {
            if (plunge_depth > 0.0) {
                double z = layer_z - (slam_depth + plunge_depth * double(k + 1) / double(K));
                sprintf(buf, "G1 Z%.3f F%d ; injection plunge\n", z, z_feedrate);
                gcode += buf;
                // The raw Z move above sets F to the (fast) Z feedrate and bypasses
                // the writer's speed tracking, so the writer won't restore it.
                // Re-emit the injection feedrate or the extrude below inherits the
                // Z feedrate and injects far too fast.
                sprintf(buf, "G1 F%.3f\n", feedrate_mmmin);
                gcode += buf;
            }
            gcode += gcodegen.writer().extrude_to_xy(
                xy, seg_e[k], K > 1 ? "injection segment" : "Magma injection");
        }

        // Dwell: hold nozzle sealed while plastic spreads through tube
        if (dwell_ms > 0) {
            sprintf(buf, "G4 P%d ; injection dwell\n", dwell_ms);
            gcode += buf;
        }

        // Retract BEFORE z-slam release to avoid dragging filament up
        // from the injection point as the nozzle lifts.
        // Uses writer().retract() directly instead of gcodegen.retract() because:
        //   - gcodegen.retract() adds wipe moves (nozzle is z-slammed into surface)
        //   - gcodegen.retract() adds Z-lift (we manage Z manually: slam release then hop)
        //   - gcodegen.retract() resets E (unnecessary mid-injection-loop)
        if (inj_retract)
            gcode += gcodegen.writer().retract();

        // Z-slam release: return to normal layer height (also undoes any plunge).
        if (slam_depth > 0 || plunge_depth > 0) {
            sprintf(buf, "G1 Z%.3f F%d ; z-slam release\n", layer_z, z_feedrate);
            gcode += buf;
        }

        // Post-injection Z-hop to clear nozzle from ooze blob.
        //
        // INTENTIONAL: these manual Z moves do NOT sync GCodeWriter's tracked Z
        // (no set_position). That is load-bearing, not a bug: the writer keeps
        // thinking Z == nominal layer height, so the next travel_to() emits a
        // pure-XY move and the nozzle stays at full hop height across the entire
        // travel -- ideal ooze clearance. Calling set_position() here would make
        // travel_to() believe it must descend to layer height, lowering the
        // nozzle *during* the travel (a diagonal drag across the part). The very
        // next "injection z-hop down" brings Z back to layer height at the
        // destination, and every extrusion is preceded by an absolute z-slam
        // move, so physical Z is always correct regardless of tracked state.
        //
        // Note: in OrcaSlicer z-hop is a *retraction-time* lift (emitted inside
        // GCode::retract), so it only appears on travels long enough to retract.
        // Short combed hops between nearby injections don't retract, so this is
        // the only clearance there. On a longer travel that does retract with
        // z-hop enabled, travel_to's lift rides along and can reduce our hop to
        // that lift height for that travel -- still safe.
        if (inj_z_hop > 0) {
            sprintf(buf, "G1 Z%.3f F%d ; injection z-hop\n", layer_z + inj_z_hop, z_feedrate);
            gcode += buf;
        }
    }

    // Reset forced dimensions
    sprintf(buf, ";%s0\n",
            GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height).c_str());
    gcode += buf;
    sprintf(buf, ";%s0\n",
            GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width).c_str());
    gcode += buf;

    // --- Ironing of tube ends ---
    if (iron_tube_ends) {
        // Emit ironing role tag
        sprintf(buf, ";%s%s\n",
                GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(),
                ExtrusionEntity::role_to_string(erIroning).c_str());
        gcode += buf;

        // Ironing parameters: use magma-specific values if set, otherwise fall back
        // to regular ironing settings. Print::validate() ensures at least one source
        // is configured when magma_iron_tube_ends is enabled.
        double ir_flow_pct = (config.magma_ironing_flow.value > 0)
            ? config.magma_ironing_flow.value / 100.0
            : config.ironing_flow.value / 100.0;
        double ir_spacing = (config.magma_ironing_spacing.value > 0)
            ? config.magma_ironing_spacing.value
            : config.ironing_spacing.value;
        if (ir_spacing <= 0)
            ir_spacing = 0.1;  // safety floor — config min allows 0
        double ir_speed = (config.magma_ironing_speed.value > 0)
            ? config.magma_ironing_speed.value * 60.0
            : config.ironing_speed.value * 60.0;  // mm/s → mm/min

        float nozzle_d = config.nozzle_diameter.get_at(extruder_id);
        double ir_height = actual_layer_height * ir_flow_pct;
        double ir_flow_mm3_per_mm = nozzle_d * ir_height;
        double ir_e_per_mm = ir_flow_mm3_per_mm * e_per_mm3;

        // Iron each injection hole with serpentine parallel lines
        for (const auto& pt : points) {
            double radius = tube_map.interior_width() / 2.0 - nozzle_d / 2.0;
            if (radius <= 0.05)
                continue;

            bool left_to_right = true;
            for (double dy = -radius; dy <= radius + 0.001; dy += ir_spacing) {
                double r2 = radius * radius - dy * dy;
                if (r2 <= 0)
                    continue;
                double half_chord = std::sqrt(r2);

                double x0 = pt.position.x() + (left_to_right ? -half_chord : half_chord);
                double x1 = pt.position.x() + (left_to_right ? half_chord : -half_chord);
                double y  = pt.position.y() + dy;

                // Travel to line start
                Point start_s(scale_(x0), scale_(y));
                gcode += gcodegen.travel_to(start_s, erIroning, "iron start");
                gcode += gcodegen.unretract();

                // Extrude ironing line (use point_to_gcode for correct
                // instance offset — Vec2d(x1,y) is object-local coords).
                double line_len = 2.0 * half_chord;
                double e_val = line_len * ir_e_per_mm;
                gcode += gcodegen.writer().set_speed(ir_speed);
                gcode += gcodegen.writer().extrude_to_xy(
                    gcodegen.point_to_gcode(Point(scale_(x1), scale_(y))),
                    e_val, "tube iron");

                left_to_right = !left_to_right;
            }
            gcode += gcodegen.retract(false, false);
        }
    }

    return gcode;
}

} // namespace magma
} // namespace Slic3r
