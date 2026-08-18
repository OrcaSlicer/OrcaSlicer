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

// One renderable column of an injection manifold: a centerline with a per-point render
// width (parallel vectors), so the column can taper layer-by-layer where the part clips it.
struct TubeVizLine { std::vector<Vec3d> pts; std::vector<double> widths; };

// Keep at most max_n points per column (endpoints + evenly spaced), preserving widths. The
// preview reveals the tube one segment at a time to animate the fill, so a straight column
// needs intermediate points (RDP would collapse it to 2, which can't animate); this bounds
// the count instead of keeping every layer.
static void subsample_with_widths(std::vector<Vec3d>& pts, std::vector<double>& widths, size_t max_n)
{
    if (pts.size() <= max_n || max_n < 2)
        return;
    std::vector<Vec3d>  np;  np.reserve(max_n);
    std::vector<double> nw;  nw.reserve(max_n);
    for (size_t i = 0; i < max_n; ++i) {
        const size_t idx = (i * (pts.size() - 1)) / (max_n - 1); // includes first and last
        np.push_back(pts[idx]);
        nw.push_back(idx < widths.size() ? widths[idx] : 0.4);
    }
    pts = std::move(np); widths = std::move(nw);
}

// Format the manifold (hub + legs) as ONE MAGMA_TUBE comment. lines[0] is the hub; the
// rest branch from the hub's last point. Each point carries its own width:
// pts=x,y,z,w;... (see GCodeProcessor's parser).
static std::string format_tube_viz_comment(const std::vector<TubeVizLine>& lines)
{
    std::ostringstream oss;
    oss << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Magma_Tube)
        << "np=" << lines.size() << " n=";
    for (size_t i = 0; i < lines.size(); ++i) { if (i) oss << ','; oss << lines[i].pts.size(); }
    oss << " pts=";
    bool first = true;
    for (const TubeVizLine& line : lines)
        for (size_t k = 0; k < line.pts.size(); ++k) {
            if (!first) oss << ';';
            first = false;
            char buf[80];
            snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f,%.3f",
                     line.pts[k].x(), line.pts[k].y(), line.pts[k].z(),
                     k < line.widths.size() ? line.widths[k] : 0.1);
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
        // Deliberately NOT travel_speed: plowing molten rim material at a travel feedrate
        // (150-300 mm/s) tears it away instead of shearing it back into the crater.
        : (config.ironing_speed.value > 0.0 ? config.ironing_speed.value : MAGMA_IRON_SPEED_FALLBACK);
    if (wipe_speed_mms < 1.0) wipe_speed_mms = MAGMA_IRON_SPEED_FALLBACK;
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
    const double max_immersion = config.magma_max_immersion.value;
    const double slam_press    = config.magma_auto_slam_press.value;
    // How deep the hot nozzle may travel inside a tube, from any path. Slam, the user
    // offset and the plunge all spend from this one budget.
    const double immersion_budget = std::min(MAGMA_SLAM_CLAMP,
                                             std::max(std::max(0.0, slam_press), std::max(0.0, max_immersion)));
    if (config.magma_injection_z_slam_auto.value) {
        slam_depth = auto_slam_depth(tube_map.tube_opening_diameter(), seal_flat,
                                     config.magma_nozzle_cone_half_angle.value,
                                     max_immersion, slam_press);
        // User trim on the auto depth (+ deeper / - shallower), re-clamped to the same
        // budget so it stays the single limit on immersion. Raise Max nozzle immersion
        // if you need more than it allows.
        slam_depth = std::min(immersion_budget,
                              std::max(0.0, slam_depth + config.magma_injection_z_slam_offset.value));
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

    // Injection volumetric speed. 0 = run at the filament's max volumetric speed: the tube
    // is a narrow cross-section and the melt is cooling the whole way down, so filling at
    // the material's melt limit fills more reliably than a fixed rate -- and that limit is
    // a filament property, so it adapts across materials on its own. A nonzero value is
    // taken as-is, still capped at the melt rate.
    double max_vol = config.filament_max_volumetric_speed.get_at(extruder_id);
    double vol_speed;
    if (injection_speed_vol <= 0.0)
        vol_speed = max_vol > 0.0 ? max_vol : 10.0;   // no melt rate configured: prior default
    else
        vol_speed = max_vol > 0.0 ? std::min(injection_speed_vol, max_vol) : injection_speed_vol;
    vol_speed = std::max(1.0, vol_speed);

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

        // Per-tube auto z-slam: seal to THIS tube's actual cap opening (measured in
        // scan_layers) instead of the global ideal — boundary-cap tubes have smaller
        // openings and would otherwise be over-slammed. Manual mode keeps the fixed
        // slam_depth resolved above. plunge_depth tracks slam_depth.
        if (config.magma_injection_z_slam_auto.value) {
            const auto& slam_pair = tube_map.u_tube_pairs()[pt.pair_index];
            slam_depth = auto_slam_depth(tube_map.cap_opening_diameter(slam_pair), seal_flat,
                                         config.magma_nozzle_cone_half_angle.value,
                                         max_immersion, slam_press);
            slam_depth = std::min(immersion_budget,
                                  std::max(0.0, slam_depth + config.magma_injection_z_slam_offset.value));
            plunge_depth = config.magma_injection_plunge.value
                               ? clamp_plunge_depth(slam_depth, config.magma_injection_plunge_depth.value)
                               : 0.0;
        }

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

        // Tube visualization: ONE MAGMA_TUBE comment carrying the whole manifold — a HUB
        // column (injection top → window) at the hub bore, plus one LEG per vent (cell_b +
        // extra_vents), each branching from the hub's last point and drawn at its own vent
        // bore. The renderer fills the hub, then advances all legs together. Columns follow
        // the spiral per layer and are RDP-simplified; the hub also sizes the E-split below.
        const auto&  pair    = tube_map.u_tube_pairs()[pt.pair_index];
        const int    cap     = pair.pair_end_layer;
        const int    wc      = pt.window_center_layer;
        const double iw      = tube_map.interior_width();
        const double z_floor = tube_map.print_z(pair.pair_start_layer) + iw * 0.5;
        auto z_at = [&](int L) { double z = tube_map.print_z(L); return z < z_floor ? z_floor : z; };

        std::vector<TubeVizLine> lines;
        // Hub column: injection top → window, at the injection XY (pts[0] = the nozzle).
        // Per-point width = the hub's per-layer bore (narrows where the part clips it).
        TubeVizLine hub;
        for (int L = cap; L >= wc; --L) {
            hub.pts.push_back({ pair.injection_center.x(), pair.injection_center.y(),
                                (L == cap) ? layer_z : z_at(L) });
            hub.widths.push_back(tube_map.cell_bore_at(pair.cell_a, L));
        }
        subsample_with_widths(hub.pts, hub.widths, 16);
        const Vec3d junction = hub.pts.empty()
            ? Vec3d(pair.injection_center.x(), pair.injection_center.y(), z_at(wc))
            : hub.pts.back();
        lines.push_back(std::move(hub));
        // Each leg starts at the hub's BOTTOM (the junction, at the window Z) so the
        // manifold reads as the hub tube ending at the bottom and branching out: fan
        // HORIZONTALLY to the vent (same Z), then rise up the vent. Rendered sequentially
        // (each leg a contiguous Noop-separated run). NOTE: multi-vent manifolds still
        // show a "spider" — leg 2+'s Noop separator has its previous point at the prior
        // vent-top, and the gcode→viewer convert inserts a phantom vertex there, drawing
        // a diagonal back to the junction. Single-vent (triangle/rectilinear/1-vent tri-hex)
        // is correct because the Noop's previous point IS the junction (hub bottom).
        auto add_leg = [&](const magma::CellId& vent) {
            TubeVizLine leg;
            // Start at the hub bottom with the HUB bore, so the connector through the window
            // is a frustum tapering from the wide hub/window mouth down to the thin vent,
            // instead of a tiny constant-width line. The vent column then holds the vent bore.
            leg.pts.push_back(junction);                          // hub bottom (junction)
            leg.widths.push_back(tube_map.cell_bore_at(pair.cell_a, wc));
            for (int L = wc; L <= cap; ++L) {
                Vec2d c = tube_map.lattice_at(L).cell_center(vent);
                leg.pts.push_back({ c.x(), c.y(), (L == cap) ? layer_z : z_at(L) });
                leg.widths.push_back(tube_map.cell_bore_at(vent, L));
            }
            subsample_with_widths(leg.pts, leg.widths, 16);
            if (leg.pts.size() >= 2)
                lines.push_back(std::move(leg));
        };
        add_leg(pair.cell_b);
        for (const auto& ev : pair.extra_vents)
            add_leg(ev);
        // Shift the manifold from object-local MODEL coords into G-code coords using the
        // RELIABLE nozzle position (the writer is parked at this injection point after the
        // travel above). hub[0] is the model injection centre, so obj_off maps it onto the
        // real injection XY. The preview parser then applies only the standard gcode->render
        // offset (plate + extruder) like any move — no fragile m_end_position reconstruction.
        const Vec2d obj_off(gcodegen.writer().get_position().x() - pair.injection_center.x(),
                            gcodegen.writer().get_position().y() - pair.injection_center.y());
        for (auto& line : lines)
            for (auto& p : line.pts) { p.x() += obj_off.x(); p.y() += obj_off.y(); }
        // The MAGMA_TUBE comment carries the manifold geometry (hub + legs, per-layer
        // centres and widths) to the preview's separate custom tube renderer. It does NOT
        // drive the toolpath: the injection below renders as the real in-place nozzle move.
        if (!lines[0].pts.empty())
            gcode += format_tube_viz_comment(lines);

        double filament_length = volume * e_per_mm3;
        Vec2d xy(gcodegen.writer().get_position().x(),
                 gcodegen.writer().get_position().y());

        // Inject WHILE plunging: fold the Z descent into the extrude moves so the nozzle sinks
        // as the channel fills, compensating for the rim melting where it touches the nozzle.
        //
        // The catch is pacing. G-code F is the CARTESIAN speed, and here the cartesian distance
        // is only the small plunge depth while E carries mm of filament. Setting F to the
        // extruder's volumetric feedrate (the old code) makes the firmware finish the tiny
        // cartesian move almost instantly and cram the E to the extruder's max velocity — a
        // hard, fast burst that oozes around the nozzle (under-fill) with Z/extruder stutter
        // (the "bounce"). Instead set F so the cartesian move LASTS the extrusion time:
        //   F = vol_feedrate * (plunge_depth / filament_length)
        // Then E flows at the user's chosen volumetric rate while Z sinks plunge_depth over the
        // same time — a continuous inject-while-plunge.
        //
        // A folded move with this E:cartesian ratio trips Klipper's max_extrude_cross_section
        // guard, which is a printer.cfg setting (NOT runtime-settable from G-code) and must be
        // raised for injection. Without plunge we inject in place as pure-E moves, which the
        // firmware paces by the extruder directly (split only to stay under the extrude-only
        // move distance).
        const int N = (plunge_depth > 0.0)
            ? 8 : std::max(1, std::min(8, (int) std::ceil(filament_length / 40.0)));
        if (plunge_depth > 0.0) {
            const double f_inject = feedrate_mmmin * plunge_depth / std::max(1e-4, filament_length);
            gcode += gcodegen.writer().set_speed(f_inject);
            for (int k = 0; k < N; ++k) {
                double z = layer_z - (slam_depth + plunge_depth * double(k + 1) / double(N));
                gcode += gcodegen.writer().extrude_to_xyz(
                    Vec3d(xy.x(), xy.y(), z), filament_length / double(N),
                    N > 1 ? "injection segment" : "Magma injection");
            }
        } else {
            gcode += gcodegen.writer().set_speed(feedrate_mmmin);
            for (int k = 0; k < N; ++k)
                gcode += gcodegen.writer().extrude_to_xy(
                    xy, filament_length / double(N), N > 1 ? "injection segment" : "Magma injection");
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
            // D      = centre-to-centre distance to an edge-sharing neighbour cell,
            //          from the active pattern's geometry (triangle: side/sqrt(3);
            //          square: cell spacing).
            double R_open   = tube_map.tube_opening_diameter() / 2.0;
            double D        = tube_map.neighbor_centroid_distance();

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
            // Margin 0 = auto. Any Z below the print surface displaces wall material out AND
            // up: it rides the nozzle bevel and piles into a rim just outside the mouth, like
            // an impact crater. The spiral plows that back in with the BEVEL, not the flat, so
            // the flat has to begin entirely outside the rim -- start_R - r_flat >= crater_r,
            // i.e. margin >= r_flat -- plus a little clearance for the piled material itself.
            double margin   = wipe_margin > 0.0 ? wipe_margin : r_flat + MAGMA_IRON_RIM_CLEARANCE;
            double start_R  = crater_r + margin;

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

            // Build the iron path (one full rim rotation to shear the whole rim evenly,
            // then spiral inward to the centre) as 3D points, then RDP-simplify before
            // emitting. The radius shrinks to 0, so a fixed seg/rev massively over-resolves
            // the inner turns; RDP at a sub-line-width tolerance keeps the rim's smoothness
            // but collapses the redundant inner points — far fewer G-code moves (file size +
            // preview slider steps) with negligible change to the ironed path. 3D so the
            // press/hover Z step is preserved.
            std::vector<Vec3d> iron_pts;
            iron_pts.reserve(static_cast<size_t>(seg_per_rev) * (wipe_turns + 1) + 2);
            auto add_iron = [&](double rad, double ang) {
                iron_pts.emplace_back(c.x() + rad * std::cos(ang), c.y() + rad * std::sin(ang), iron_z(rad));
            };
            for (int s = 1; s <= seg_per_rev; ++s)                          // rim
                add_iron(start_R, double(s) / double(seg_per_rev) * 2.0 * PI);
            const int total = std::max(1, wipe_turns * seg_per_rev);        // spiral inward
            for (int s = 1; s <= total; ++s) {
                const double frac = double(s) / double(total);              // 0..1, outer -> centre
                add_iron(start_R * (1.0 - frac), frac * wipe_turns * 2.0 * PI);
            }
            if (iron_pts.size() > 2) {
                Eigen::MatrixXd P(iron_pts.size(), 3);
                for (size_t i = 0; i < iron_pts.size(); ++i) P.row(i) = iron_pts[i].transpose();
                Eigen::MatrixXd S; Eigen::VectorXi J;
                igl::ramer_douglas_peucker(P, 0.08, S, J);                  // 0.08 mm: well under line width
                std::vector<Vec3d> simplified; simplified.reserve(J.size());
                for (int i = 0; i < J.size(); ++i) {
                    const int idx = J(i);
                    if (idx >= 0 && idx < int(iron_pts.size())) simplified.push_back(iron_pts[idx]);
                }
                iron_pts = std::move(simplified);
            }
            for (const Vec3d& p : iron_pts) {
                sprintf(buf, "G1 X%.3f Y%.3f Z%.3f F%d ; crater iron\n", p.x(), p.y(), p.z(), wf);
                gcode += buf;
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
