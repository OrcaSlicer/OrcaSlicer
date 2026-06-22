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
    double fill_factor       = config.magma_tube_fill_factor.value;
    if (fill_factor <= 0)
        return {};

    double slam_depth        = 0.0;  // resolved below (auto mode needs extruder/nozzle info)
    int    dwell_ms          = config.magma_injection_dwell.value;
    bool inj_retract         = config.magma_injection_retract.value;

    // Crater wipe ("plow the displaced rim back, scrape nozzle clean").
    bool   wipe_enabled      = config.magma_injection_iron.value;
    int    wipe_turns        = std::max(1, config.magma_injection_iron_turns.value);
    // Wipe speed: explicit setting if given, else the slicer's ironing speed (this
    // replaces ironing and is the same kind of slow surface-finishing move), else
    // travel speed as a last resort.
    double wipe_speed_mms    = config.magma_injection_iron_speed.value > 0.0
        ? config.magma_injection_iron_speed.value
        : (config.ironing_speed.value > 0.0 ? config.ironing_speed.value : config.travel_speed.value);
    if (wipe_speed_mms < 1.0) wipe_speed_mms = 30.0;
    double wipe_hover        = std::max(0.0, config.magma_injection_iron_hover.value);
    double wipe_margin       = std::max(0.0, config.magma_injection_iron_margin.value);

    // Get extruder info (ToolOrdering handles filament switching before we get here)
    unsigned int extruder_id = gcodegen.writer().filament()->id();
    double filament_diameter = config.filament_diameter.get_at(extruder_id);
    double filament_area = (PI / 4.0) * filament_diameter * filament_diameter;

    // Resolve Z-slam depth: auto-derive from nozzle cone geometry (cone widens
    // from the flat to cover the opening), or use the manual value. The seal math
    // lives in MagmaTriangleCell.hpp so the injection here and Print::validate()
    // stay in lockstep.
    double seal_flat = resolve_nozzle_flat(config.magma_nozzle_outer_diameter.value,
                                           config.nozzle_diameter.get_at(extruder_id));
    if (config.magma_injection_z_slam_auto.value) {
        slam_depth = auto_slam_depth(tube_map.tube_opening_diameter(), seal_flat,
                                     config.magma_nozzle_cone_half_angle.value);
    } else {
        slam_depth = std::min(config.magma_injection_z_slam.value, MAGMA_SLAM_CLAMP);
    }

    // Plunge ("slam-melt"): ramp the nozzle deeper during the injection so the hot
    // tip sinks into the softening tube top and keeps the seal pressed shut as the
    // channel fills. Ramps from slam_depth to slam_depth + plunge_depth, clamped so
    // the total intrusion stays bounded.
    double plunge_depth = config.magma_injection_plunge.value
                              ? clamp_plunge_depth(slam_depth, config.magma_injection_plunge_depth.value)
                              : 0.0;

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

    // --- Injection loop ---
    // Temperature and fan markers are managed by the caller (GCode.cpp
    // injection phase) so that multiple objects share one heat/cool cycle.
    float display_dim = tube_map.interior_width();

    for (const auto& pt : points) {
        double volume = pt.volume_mm3 * fill_factor;
        if (volume <= 0)
            continue;

        // Travel to injection point. The built-in travel_to() handles retraction,
        // avoid-crossing-perimeters, and the printer's own z-hop; the unretract
        // below also undoes any lift it applied.
        Point scaled_pos(scale_(pt.position.x()), scale_(pt.position.y()));
        gcode += gcodegen.travel_to(scaled_pos, erMagmaInjection, "move to injection point");

        // Unretract — undoes the previous injection's retract (state-tracked via
        // GCodeWriter::retract) and any travel retract/lift from travel_to().
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

        // --- Finish: break the seal, retract, then wipe the crater ---
        // Lift a little to crack the seal BEFORE retracting, so we don't pull the
        // freshly-injected plug back up through the still-sealed interface. 0.3mm
        // relieves the contact pressure regardless of how deep the plunge was.
        const double break_lift = 0.3;
        double deep_z = layer_z - slam_depth - plunge_depth;   // nozzle depth after plunge
        sprintf(buf, "G1 Z%.3f F%d ; injection break-lift\n", deep_z + break_lift, z_feedrate);
        gcode += buf;
        if (inj_retract)
            gcode += gcodegen.writer().retract();

        if (wipe_enabled) {
            // Crater ironing: spiral the nozzle inward over the injection point so
            // the angled cone plows the displaced rim back into the crater (pushing
            // it in + down) and irons the surface flat, while scraping the nozzle
            // clean so it doesn't string to the next tube. The flat hovers over
            // neighbouring cells (so it never irons a neighbour's air hole shut)
            // and only descends to layer height over our own crater.

            // Classify the pass for the preview. Role = Ironing: this is a special
            // ironing finish, not an injection, so it shouldn't carry the injection
            // role. WIPE markers wrap the moves so the processor registers them as
            // the "Wipe" move type (its own preview toggle) rather than lumping these
            // non-extruding plow moves under the injection role -- role and move-type
            // are independent in the viewer, and a non-extruding clean-up reads
            // naturally as a wipe. Only the injection extrusion stays Magma injection.
            sprintf(buf, ";%s%s\n",
                    GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(),
                    ExtrusionEntity::role_to_string(erIroning).c_str());
            gcode += buf;
            gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start) + "\n";

            // Centre = current nozzle XY (the injection point). All spiral points
            // are computed relative to it, in G-code (origin-applied) coordinates.
            Vec2d c(gcodegen.writer().get_position().x(),
                    gcodegen.writer().get_position().y());

            // Nozzle geometry: r_flat = radius of the flat tip (resolved once above);
            // the cone above it rises at cone_rad from vertical.
            double r_flat   = seal_flat / 2.0;
            double cone_rad = config.magma_nozzle_cone_half_angle.value * PI / 180.0;

            // R_open = our tube opening's vertex radius (inset/hollow triangle, so
            //          it already excludes the cell walls / line width).
            // D      = centre-to-centre distance to an edge-sharing neighbour cell
            //          (for the triangle grid this is exactly side/sqrt(3)).
            double R_open   = tube_map.tube_opening_diameter() / 2.0;
            double D        = triangle_side_length(tube_map.cell_spacing()) / std::sqrt(3.0);

            // cap = largest spiral radius at which the flat may PRESS (descend to
            // layer height). A neighbour air hole's far vertex sits at (D + R_open)
            // from us; we keep the flat's outer edge (r_flat past the spiral radius)
            // at least 0.5mm short of it so a sliver of every neighbour hole stays
            // open for air to escape. Inside cap we press; outside (or if cap <= 0,
            // i.e. cells too tight to press anywhere) we hover.
            double cap      = (D + R_open) - 0.5 - r_flat;

            // crater_r = footprint radius of the intrusion: the cone reaches this
            //            radius at the bottom of the slam+plunge.
            // start_R  = begin the spiral this far out, so it catches the whole
            //            displaced rim (margin beyond the footprint).
            double crater_r = r_flat + (slam_depth + plunge_depth) * std::tan(cone_rad);
            double start_R  = crater_r + wipe_margin;

            double hover_z  = layer_z + wipe_hover;                   // height while hovering
            int    wf       = std::max(60, (int)(wipe_speed_mms * 60.0));  // mm/s -> mm/min
            const int seg_per_rev = 16;                              // circle smoothness

            // Z is a STEP, not a ramp: press flat ON the surface (layer height)
            // everywhere it is safe (inside the neighbour-clear cap), and only
            // hover above the surface where the flat would otherwise reach a
            // neighbour's air hole (rad > cap), or always if cap <= 0 (no room).
            auto iron_z = [&](double rad) {
                return (cap > 0.0 && rad <= cap) ? layer_z : hover_z;
            };
            auto emit_iron = [&](double rad, double ang, const char* tag) {
                sprintf(buf, "G1 X%.3f Y%.3f Z%.3f F%d ; %s\n",
                        c.x() + rad * std::cos(ang), c.y() + rad * std::sin(ang),
                        iron_z(rad), wf, tag);
                gcode += buf;
            };

            // One full rotation at the start radius first, to shear/plow the whole
            // rim evenly before converging (avoids dragging a wad inward).
            for (int s = 1; s <= seg_per_rev; ++s)
                emit_iron(start_R, double(s) / double(seg_per_rev) * 2.0 * PI, "crater iron rim");

            // Then spiral inward to the centre.
            int total = std::max(1, wipe_turns * seg_per_rev);
            for (int s = 1; s <= total; ++s) {
                double frac = double(s) / double(total);            // 0..1, outer -> centre
                emit_iron(start_R * (1.0 - frac), frac * wipe_turns * 2.0 * PI, "crater iron");
            }
            // One short stroke across the centre to flatten the gathered mound,
            // only where we can actually press (cap > 0, i.e. there is room).
            if (cap > 0.0) {
                double fl = std::min(cap, r_flat);
                sprintf(buf, "G1 X%.3f Y%.3f Z%.3f F%d ; crater iron flatten\n", c.x() - fl, c.y(), layer_z, wf); gcode += buf;
                sprintf(buf, "G1 X%.3f Y%.3f Z%.3f F%d ; crater iron flatten\n", c.x() + fl, c.y(), layer_z, wf); gcode += buf;
            }
            // Return to centre at layer height and resync the writer position (the
            // raw moves above bypassed its tracking) so the next travel_to plans
            // its path/avoid-crossing from the right spot.
            sprintf(buf, "G1 X%.3f Y%.3f Z%.3f F%d ; crater iron end\n", c.x(), c.y(), layer_z, wf);
            gcode += buf;
            gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End) + "\n";
            gcodegen.writer().set_position(Vec3d(c.x(), c.y(), layer_z));
        } else {
            // No wipe: just return to layer height and resync the writer's Z.
            sprintf(buf, "G1 Z%.3f F%d ; z-slam release\n", layer_z, z_feedrate);
            gcode += buf;
            Vec3d p = gcodegen.writer().get_position(); p.z() = layer_z;
            gcodegen.writer().set_position(p);
        }
        // No manual z-hop: the next iteration's travel_to() handles lift + travel.
    }

    // Reset forced dimensions
    sprintf(buf, ";%s0\n",
            GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height).c_str());
    gcode += buf;
    sprintf(buf, ";%s0\n",
            GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width).c_str());
    gcode += buf;

    return gcode;
}

} // namespace magma
} // namespace Slic3r
