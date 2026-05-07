#include <random>

#include "libslic3r/Algorithm/LineSplit.hpp"
#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "FuzzySkin.hpp"

#include "libnoise/noise.h"

// #define DEBUG_FUZZY

using namespace Slic3r;

namespace Slic3r::Feature::FuzzySkin {

// Produces a random value between 0 and 1. Thread-safe.
static double random_value() {
    thread_local std::random_device rd;
    // Hash thread ID for random number seed if no hardware rng seed is available
    thread_local std::mt19937 gen(rd.entropy() > 0 ? rd() : std::hash<std::thread::id>()(std::this_thread::get_id()));
    thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(gen);
}

class UniformNoise: public noise::module::Module {
    public:
        UniformNoise(): Module (GetSourceModuleCount ()) {};

        virtual int GetSourceModuleCount() const { return 0; }
        virtual double GetValue(double x, double y, double z) const { return random_value() * 2 - 1; }
};

static std::unique_ptr<noise::module::Module> get_noise_module(const FuzzySkinConfig& cfg) {
    if (cfg.noise_type == NoiseType::Perlin) {
        auto perlin_noise = noise::module::Perlin();
        perlin_noise.SetFrequency(1 / cfg.noise_scale);
        perlin_noise.SetOctaveCount(cfg.noise_octaves);
        perlin_noise.SetPersistence(cfg.noise_persistence);
        return std::make_unique<noise::module::Perlin>(perlin_noise);
    } else if (cfg.noise_type == NoiseType::Billow) {
        auto billow_noise = noise::module::Billow();
        billow_noise.SetFrequency(1 / cfg.noise_scale);
        billow_noise.SetOctaveCount(cfg.noise_octaves);
        billow_noise.SetPersistence(cfg.noise_persistence);
        return std::make_unique<noise::module::Billow>(billow_noise);
    } else if (cfg.noise_type == NoiseType::RidgedMulti) {
        auto ridged_multi_noise = noise::module::RidgedMulti();
        ridged_multi_noise.SetFrequency(1 / cfg.noise_scale);
        ridged_multi_noise.SetOctaveCount(cfg.noise_octaves);
        return std::make_unique<noise::module::RidgedMulti>(ridged_multi_noise);
    } else if (cfg.noise_type == NoiseType::Voronoi) {
        auto voronoi_noise = noise::module::Voronoi();
        voronoi_noise.SetFrequency(1 / cfg.noise_scale);
        voronoi_noise.SetDisplacement(1.0);
        return std::make_unique<noise::module::Voronoi>(voronoi_noise);
    } else {
        return std::make_unique<UniformNoise>();
    }
}

// ---------------------------------------------------------------------------
// Ripple noise — deterministic sine-wave displacement along the path arc length.
//
// Unlike the other noise types, the ripple pattern is driven by cumulative arc
// length along the print path rather than world-space (x, y, z) coordinates.
// This gives a uniform wave period regardless of the polygon's geometry.
//
// A consistent visual anchor is established by finding the leftmost Y=0 crossing
// of the polygon (the point where the sine wave always peaks when phase shift is
// zero), ensuring the pattern aligns across layers.
//
// Per-layer-group phase shifting works as follows:
//   period_index  = floor(layer_id / layers_between_ripple_offset)
//   phase_shift   = period_index * ripple_offset * 2π  [radians]
//
// Setting layers_between_ripple_offset = 1 shifts the phase on every layer;
// setting it to N makes N consecutive layers share the same pattern.
// ---------------------------------------------------------------------------

// Compute the per-layer-group phase shift in radians.
static double ripple_phase_shift_rad(const FuzzySkinConfig& cfg)
{
    if (cfg.ripple_offset == 0.0 || cfg.layers_between_ripple_offset <= 0)
        return 0.0;

    const int    effective_layer = std::max(cfg.layer_id, 0);
    const int    period_index    = effective_layer / std::max(cfg.layers_between_ripple_offset, 1);
    const double raw_shift       = period_index * cfg.ripple_offset * (2.0 * M_PI);
    return fmod(raw_shift, 2.0 * M_PI);
}

// Find the arc-length (in mm) of the visual anchor point along the polygon perimeter.
// The anchor is the leftmost Y=0 crossing, falling back to the vertex with the
// smallest |y| if no crossing exists. The anchor is where sin(phase) = 1 (a peak)
// when the phase shift is zero, giving a stable reference across layers.
static double ripple_anchor_arc_mm(const Points& poly)
{
    const size_t np = poly.size();

    // Find anchor world position: leftmost Y=0 crossing.
    Vec2d anchor_world(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
    bool  found_crossing = false;
    for (size_t i = 0; i < np; ++i) {
        const double ya = unscale_(poly[i].y());
        const double yb = unscale_(poly[(i + 1) % np].y());
        if ((ya <= 0.0 && yb >= 0.0) || (ya >= 0.0 && yb <= 0.0)) {
            const double t       = (std::abs(yb - ya) < 1e-9) ? 0.0 : ya / (ya - yb);
            const double x_cross = unscale_(poly[i].x()) +
                                   std::max(0.0, std::min(1.0, t)) * (unscale_(poly[(i + 1) % np].x()) - unscale_(poly[i].x()));
            if (!found_crossing || x_cross < anchor_world.x()) {
                anchor_world   = Vec2d(x_cross, 0.0);
                found_crossing = true;
            }
        }
    }
    if (!found_crossing) {
        double best_abs_y = std::numeric_limits<double>::max();
        for (const Point& p : poly) {
            const double ay = std::abs(unscale_(p.y()));
            if (ay < best_abs_y) {
                best_abs_y   = ay;
                anchor_world = Vec2d(unscale_(p.x()), unscale_(p.y()));
            }
        }
    }

    // Find the arc-length of the closest point on the polyline to anchor_world.
    double anchor_arc_mm = 0.0;
    double best_dist_sq  = std::numeric_limits<double>::max();
    double accum_mm      = 0.0;
    for (size_t i = 0; i < np; ++i) {
        const Vec2d  pa_mm(unscale_(poly[i].x()), unscale_(poly[i].y()));
        const Vec2d  pb_mm(unscale_(poly[(i + 1) % np].x()), unscale_(poly[(i + 1) % np].y()));
        const Vec2d  seg     = pb_mm - pa_mm;
        const double seg_len = seg.norm();
        if (seg_len > 1e-9) {
            const double t       = std::max(0.0, std::min(1.0, (anchor_world - pa_mm).dot(seg) / (seg_len * seg_len)));
            const double dist_sq = (pa_mm + seg * t - anchor_world).squaredNorm();
            if (dist_sq < best_dist_sq) {
                best_dist_sq  = dist_sq;
                anchor_arc_mm = accum_mm + t * seg_len;
            }
        }
        accum_mm += seg_len;
    }
    return anchor_arc_mm;
}

// Apply a sine-wave ripple displacement to a closed polygon.
// Points are resampled at cfg.point_distance intervals along the perimeter.
static void fuzzy_polyline_ripple(Points& poly, const FuzzySkinConfig& cfg)
{
    const double amplitude    = unscale_(cfg.thickness);
    const double N            = static_cast<double>(cfg.ripples_per_layer);
    const double fill_step_mm = unscale_(cfg.point_distance);

    if (N <= 0.0 || fill_step_mm < 1e-6)
        return;

    // Compute total perimeter length in mm.
    const size_t np           = poly.size();
    double       perimeter_mm = 0.0;
    for (size_t i = 0; i < np; ++i)
        perimeter_mm += unscale_((poly[(i + 1) % np] - poly[i]).cast<double>().norm());

    if (perimeter_mm < 1e-6)
        return;

    const double anchor_arc_mm   = ripple_anchor_arc_mm(poly);
    const double phase_shift_rad = ripple_phase_shift_rad(cfg);

    // Phase function: φ(s) = N·2π·(s - anchor_arc) / perimeter + π/2 + phase_shift
    // Adding π/2 ensures sin(φ) = 1 at the anchor when phase_shift = 0 (a peak).
    const double phase_at_anchor = M_PI * 2.0 + phase_shift_rad;
    auto arc_phase = [&](double arc_mm) -> double { return N * (2.0 * M_PI) * (arc_mm - anchor_arc_mm) / perimeter_mm + phase_at_anchor; };

    Points out;
    out.reserve(static_cast<size_t>(perimeter_mm / fill_step_mm) + np * 2);

    double accum_mm = 0.0;
    for (size_t i = 0; i < np; ++i) {
        const Point& p0      = poly[i];
        const Point& p1      = poly[(i + 1) % np];
        const Vec2d  seg     = (p1 - p0).cast<double>();
        const double seg_len = seg.norm();
        if (seg_len < EPSILON)
            continue;

        const double seg_len_mm = unscale_(seg_len);
        const Vec2d  seg_unit   = seg / seg_len;
        const Vec2d  seg_perp   = perp(seg_unit);
        const double seg_end_mm = accum_mm + seg_len_mm;
        const double first_s    = std::ceil(accum_mm / fill_step_mm) * fill_step_mm;

        for (double s = first_s; s < seg_end_mm; s += fill_step_mm) {
            const double t    = (s - accum_mm) / seg_len_mm;
            const double disp = std::sin(arc_phase(s)) * amplitude;
            const Point  pt   = p0 + (seg * t).cast<coord_t>();
            out.emplace_back(pt + (seg_perp * scale_(disp)).cast<coord_t>());
        }

        accum_mm = seg_end_mm;
    }

    while (out.size() < 3)
        out.emplace_back(poly[poly.size() - 2]);

    if (out.size() >= 3)
        poly = std::move(out);
}

// Apply a sine-wave ripple displacement to an Arachne extrusion line.
// Mirrors fuzzy_polyline_ripple but operates on ExtrusionJunction vectors so
// that per-point line width (j.w) is preserved correctly.
static void fuzzy_extrusion_line_ripple(Arachne::ExtrusionJunctions& ext_lines, const FuzzySkinConfig& cfg)
{
    const double amplitude    = unscale_(cfg.thickness);
    const double N            = static_cast<double>(cfg.ripples_per_layer);
    const double fill_step_mm = unscale_(cfg.point_distance);

    if (N <= 0.0 || fill_step_mm < 1e-6)
        return;

    // Build a Points vector for perimeter/anchor calculations.
    Points poly;
    poly.reserve(ext_lines.size());
    for (const auto& j : ext_lines)
        poly.push_back(j.p);

    // Compute total length in mm.
    const size_t np           = poly.size();
    double       perimeter_mm = 0.0;
    for (size_t i = 0; i + 1 < np; ++i)
        perimeter_mm += unscale_((poly[i + 1] - poly[i]).cast<double>().norm());

    if (perimeter_mm < 1e-6)
        return;

    const double anchor_arc_mm   = ripple_anchor_arc_mm(poly);
    const double phase_shift_rad = ripple_phase_shift_rad(cfg);
    const double phase_at_anchor = M_PI * 2.0 + phase_shift_rad;

    auto arc_phase = [&](double arc_mm) -> double { return N * (2.0 * M_PI) * (arc_mm - anchor_arc_mm) / perimeter_mm + phase_at_anchor; };

    Arachne::ExtrusionJunctions out;
    out.reserve(static_cast<size_t>(perimeter_mm / fill_step_mm) + np * 2);

    double accum_mm = 0.0;
    for (size_t i = 0; i + 1 < np; ++i) {
        const Arachne::ExtrusionJunction& j0      = ext_lines[i];
        const Arachne::ExtrusionJunction& j1      = ext_lines[i + 1];
        const Vec2d                       seg     = (j1.p - j0.p).cast<double>();
        const double                      seg_len = seg.norm();
        if (seg_len < EPSILON)
            continue;

        const double seg_len_mm = unscale_(seg_len);
        const Vec2d  seg_unit   = seg / seg_len;
        const Vec2d  seg_perp   = perp(seg_unit);
        const double seg_end_mm = accum_mm + seg_len_mm;
        const double first_s    = std::ceil(accum_mm / fill_step_mm) * fill_step_mm;

        for (double s = first_s; s < seg_end_mm; s += fill_step_mm) {
            const double t    = (s - accum_mm) / seg_len_mm;
            const double disp = std::sin(arc_phase(s)) * amplitude;
            const Point  pt   = j0.p + (seg * t).cast<coord_t>();
            out.emplace_back(pt + (seg_perp * scale_(disp)).cast<coord_t>(), j1.w, j1.perimeter_index);
        }

        accum_mm = seg_end_mm;
    }

    while (out.size() < 3) {
        size_t point_idx = ext_lines.size() - 2;
        out.emplace_back(ext_lines[point_idx].p, ext_lines[point_idx].w, ext_lines[point_idx].perimeter_index);
        if (point_idx == 0)
            break;
        --point_idx;
    }

    if (out.size() >= 3)
        ext_lines = std::move(out);
}

// Thanks Cura developers for this function.
void fuzzy_polyline(Points& poly, bool closed, coordf_t slice_z, const FuzzySkinConfig& cfg)
{
    if (cfg.noise_type == NoiseType::Ripple) {
        if (poly.size() < 3)
            return;
        fuzzy_polyline_ripple(poly, cfg);
        return;
    }

    std::unique_ptr<noise::module::Module> noise = get_noise_module(cfg);

    const double min_dist_between_points = cfg.point_distance * 3. / 4.; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const double range_random_point_dist = cfg.point_distance / 2.;
    double dist_left_over = random_value() * (min_dist_between_points / 2.); // the distance to be traversed on the line before making the first new point
    Point* p0 = &poly.back();
    Points out;
    out.reserve(poly.size());
    for (Point &p1 : poly)
    {
        if (!closed) {
            // Skip the first point for open path
            closed = true;
            p0 = &p1;
            continue;
        }
        // 'a' is the (next) new point between p0 and p1
        Vec2d  p0p1      = (p1 - *p0).cast<double>();
        double p0p1_size = p0p1.norm();
        double p0pa_dist = dist_left_over;
        for (; p0pa_dist < p0p1_size;
            p0pa_dist += min_dist_between_points + random_value() * range_random_point_dist)
        {
            Point pa = *p0 + (p0p1 * (p0pa_dist / p0p1_size)).cast<coord_t>();
            double r = noise->GetValue(unscale_(pa.x()), unscale_(pa.y()), slice_z) * cfg.thickness;
            out.emplace_back(pa + (perp(p0p1).cast<double>().normalized() * r).cast<coord_t>());
        }
        dist_left_over = p0pa_dist - p0p1_size;
        p0 = &p1;
    }
    while (out.size() < 3) {
        size_t point_idx = poly.size() - 2;
        out.emplace_back(poly[point_idx]);
        if (point_idx == 0)
            break;
        -- point_idx;
    }
    if (out.size() >= 3)
        poly = std::move(out);
}

// Thanks Cura developers for this function.
void fuzzy_extrusion_line(Arachne::ExtrusionJunctions& ext_lines, coordf_t slice_z, const FuzzySkinConfig& cfg, bool closed)
{

    if (cfg.noise_type == NoiseType::Ripple) {
        if (ext_lines.size() < 3)
            return;
        fuzzy_extrusion_line_ripple(ext_lines, cfg);
        return;
    }

    std::unique_ptr<noise::module::Module> noise = get_noise_module(cfg);

    const double min_dist_between_points = cfg.point_distance * 3. / 4.; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const double range_random_point_dist = cfg.point_distance / 2.;
    const double min_extrusion_width = 0.01; // workaround for many print options. Need overwrite formula with the layer height parameter. The width must more than >>> layer_height * (1 - 0.25 * PI) * 1.05 <<< (last num is the coeff of overlay error case)
    double dist_left_over = random_value() * (min_dist_between_points / 2.); // the distance to be traversed on the line before making the first new point

    auto* p0 = &ext_lines.front();
    Arachne::ExtrusionJunctions out;
    out.reserve(ext_lines.size());
    for (auto& p1 : ext_lines) {
        // Orca: only skip the first point for closed path, open path should not skip any point
        if (closed) {
            if (p0->p == p1.p) { // Connect endpoints.
                out.emplace_back(p1.p, p1.w, p1.perimeter_index);
                continue;
            }
        }

        // 'a' is the (next) new point between p0 and p1
        Vec2d  p0p1 = (p1.p - p0->p).cast<double>();
        double p0p1_size = p0p1.norm();
        double p0pa_dist = dist_left_over;
        for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points + random_value() * range_random_point_dist) {
            Point pa = p0->p + (p0p1 * (p0pa_dist / p0p1_size)).cast<coord_t>();
            double r = noise->GetValue(unscale_(pa.x()), unscale_(pa.y()), slice_z) * cfg.thickness;
            switch (cfg.mode) { //the curly code for testing
                case FuzzySkinMode::Displacement :
                    out.emplace_back(pa + (perp(p0p1).cast<double>().normalized() * r).cast<coord_t>(), p1.w, p1.perimeter_index);
                    break;
                case FuzzySkinMode::Extrusion :
                    out.emplace_back(pa, std::max(p1.w + r + min_extrusion_width,  min_extrusion_width), p1.perimeter_index); 
                    break;
                case FuzzySkinMode::Combined :
                    double rad = std::max(p1.w + r + min_extrusion_width,  min_extrusion_width);
                    out.emplace_back(pa + (perp(p0p1).cast<double>().normalized() * ((rad  - p1.w) / 2)).cast<coord_t>(), rad, p1.perimeter_index); //0.05 - minimum width of extruded line
                    break;
            }
        }
        dist_left_over = p0pa_dist - p0p1_size;
        p0 = &p1;
    }

    while (out.size() < 3) {
        size_t point_idx = ext_lines.size() - 2;
        out.emplace_back(ext_lines[point_idx].p, ext_lines[point_idx].w, ext_lines[point_idx].perimeter_index);
        if (point_idx == 0)
            break;
        --point_idx;
    }

    if (ext_lines.back().p == ext_lines.front().p) { // Connect endpoints.
        out.front().p = out.back().p;
        out.front().w = out.back().w;
    }

    if (out.size() >= 3)
        ext_lines = std::move(out);
}

void group_region_by_fuzzify(PerimeterGenerator& g)
{
    g.regions_by_fuzzify.clear();
    g.has_fuzzy_skin = false;
    g.has_fuzzy_hole = false;

    std::unordered_map<FuzzySkinConfig, SurfacesPtr> regions;
    for (auto region : *g.compatible_regions) {
        const auto&           region_config = region->region().config();
        const FuzzySkinConfig cfg{region_config.fuzzy_skin,
                                  scaled<coord_t>(region_config.fuzzy_skin_thickness.value),
                                  scaled<coord_t>(region_config.fuzzy_skin_point_distance.value),
                                  region_config.fuzzy_skin_first_layer,
                                  region_config.fuzzy_skin_noise_type,
                                  region_config.fuzzy_skin_scale,
                                  region_config.fuzzy_skin_octaves,
                                  region_config.fuzzy_skin_persistence,
                                  region_config.fuzzy_skin_mode,
                                  region_config.fuzzy_skin_ripples_per_layer,
                                  region_config.fuzzy_skin_ripple_offset,
                                  region_config.fuzzy_skin_layers_between_ripple_offset,
                                  g.layer_id};
        auto&                 surfaces = regions[cfg];
        for (const auto& surface : region->slices.surfaces) {
            surfaces.push_back(&surface);
        }

        if (cfg.type != FuzzySkinType::None && cfg.type != FuzzySkinType::Disabled_fuzzy) {
            g.has_fuzzy_skin = true;
            if (cfg.type != FuzzySkinType::External) {
                g.has_fuzzy_hole = true;
            }
        }
    }

    if (regions.size() == 1) { // optimization
        g.regions_by_fuzzify[regions.begin()->first] = {};
        return;
    }

    for (auto& it : regions) {
        g.regions_by_fuzzify[it.first] = offset_ex(it.second, ClipperSafetyOffset);
    }
}

bool should_fuzzify(const FuzzySkinConfig& config, const int layer_id, const size_t loop_idx, const bool is_contour)
{
    const auto fuzziy_type = config.type;

    if (fuzziy_type == FuzzySkinType::None|| fuzziy_type == FuzzySkinType::Disabled_fuzzy) {
        return false;
    }
    if (!config.fuzzy_first_layer && layer_id <= 0) {
        // Do not fuzzy first layer unless told to
        return false;
    }

    const bool fuzzify_contours = loop_idx == 0 || fuzziy_type == FuzzySkinType::AllWalls;
    const bool fuzzify_holes    = fuzzify_contours && (fuzziy_type == FuzzySkinType::All || fuzziy_type == FuzzySkinType::AllWalls);

    return is_contour ? fuzzify_contours : fuzzify_holes;
}

Polygon apply_fuzzy_skin(const Polygon& polygon, const PerimeterGenerator& perimeter_generator, const size_t loop_idx, const bool is_contour)
{
    Polygon fuzzified;

    const auto  slice_z = perimeter_generator.slice_z;
    const auto& regions = perimeter_generator.regions_by_fuzzify;
    if (regions.size() == 1) { // optimization
        const auto& config  = regions.begin()->first;
        const bool  fuzzify = should_fuzzify(config, perimeter_generator.layer_id, loop_idx, is_contour);
        if (!fuzzify) {
            return polygon;
        }

        fuzzified = polygon;
        fuzzy_polyline(fuzzified.points, true, slice_z, config);
        return fuzzified;
    }

    // Find all affective regions
    std::vector<std::pair<const FuzzySkinConfig&, const ExPolygons&>> fuzzified_regions;
    fuzzified_regions.reserve(regions.size());
    for (const auto& region : regions) {
        if (should_fuzzify(region.first, perimeter_generator.layer_id, loop_idx, is_contour)) {
            fuzzified_regions.emplace_back(region.first, region.second);
        }
    }
    if (fuzzified_regions.empty()) {
        return polygon;
    }

#ifdef DEBUG_FUZZY
    {
        int i = 0;
        for (const auto& r : fuzzified_regions) {
            BoundingBox bbox = get_extents(perimeter_generator.slices->surfaces);
            bbox.offset(scale_(1.));
            ::Slic3r::SVG svg(debug_out_path("fuzzy_traverse_loops_%d_%d_%d_region_%d.svg", perimeter_generator.layer_id,
                                             is_contour ? 0 : 1, loop_idx, i)
                                  .c_str(),
                              bbox);
            svg.draw_outline(perimeter_generator.slices->surfaces);
            svg.draw_outline(polygon, "green");
            svg.draw(r.second, "red", 0.5);
            svg.draw_outline(r.second, "red");
            svg.Close();
            i++;
        }
    }
#endif

    // Split the loops into lines with different config, and fuzzy them separately
    fuzzified = polygon;
    for (const auto& r : fuzzified_regions) {
        auto splitted = Algorithm::split_line(fuzzified, r.second, true);
        if (splitted.empty()) {
            // No intersection, skip
            continue;
        }

        // Fuzzy splitted polygon
        if (std::all_of(splitted.begin(), splitted.end(), [](const Algorithm::SplitLineJunction& j) { return j.clipped; })) {
            // The entire polygon is fuzzified
            fuzzy_polyline(fuzzified.points, true, slice_z, r.first);
        } else {
            // Start from a non-clipped junction so wrapped clipped segments do
            // not need an artificial reconnection across the seam.
            const auto first_non_clipped = std::find_if(splitted.begin(), splitted.end(), [](const Algorithm::SplitLineJunction& j) {
                return !j.clipped;
            });
            if (first_non_clipped != splitted.begin()) {
                std::rotate(splitted.begin(), first_non_clipped, splitted.end());
            }
            Points segment;
            segment.reserve(splitted.size());
            fuzzified.points.clear();

            const auto fuzzy_current_segment = [&segment, &fuzzified, &r, slice_z]() {
                fuzzified.points.push_back(segment.front());
                const auto back = segment.back();
                fuzzy_polyline(segment, false, slice_z, r.first);
                fuzzified.points.insert(fuzzified.points.end(), segment.begin(), segment.end());
                fuzzified.points.push_back(back);
                segment.clear();
            };

            for (const auto& p : splitted) {
                if (p.clipped) {
                    segment.push_back(p.p);
                } else {
                    if (segment.empty()) {
                        fuzzified.points.push_back(p.p);
                    } else {
                        segment.push_back(p.p);
                        fuzzy_current_segment();
                    }
                }
            }
            if (!segment.empty()) {
                // Close the loop
                segment.push_back(splitted.front().p);
                fuzzy_current_segment();
            }
        }
    }

    return fuzzified;
}

void apply_fuzzy_skin(Arachne::ExtrusionLine* extrusion, const PerimeterGenerator& perimeter_generator, const bool is_contour)
{
    const auto  slice_z = perimeter_generator.slice_z;
    const auto& regions = perimeter_generator.regions_by_fuzzify;
    if (regions.size() == 1) { // optimization
        const auto& config  = regions.begin()->first;
        const bool  fuzzify = should_fuzzify(config, perimeter_generator.layer_id, extrusion->inset_idx, is_contour);
        if (fuzzify)
            fuzzy_extrusion_line(extrusion->junctions, slice_z, config);
    } else {
        // Find all affective regions
        std::vector<std::pair<const FuzzySkinConfig&, const ExPolygons&>> fuzzified_regions;
        fuzzified_regions.reserve(regions.size());
        for (const auto& region : regions) {
            if (should_fuzzify(region.first, perimeter_generator.layer_id, extrusion->inset_idx, is_contour)) {
                fuzzified_regions.emplace_back(region.first, region.second);
            }
        }
        if (!fuzzified_regions.empty()) {
 
#ifdef DEBUG_FUZZY
            {
                int i = 0;
                for (const auto& r : fuzzified_regions) {
                    BoundingBox bbox = get_extents(perimeter_generator.slices->surfaces);
                    bbox.offset(scale_(1.));
                    ::Slic3r::SVG svg(debug_out_path("fuzzy_traverse_loops_%d_%d_%d_region_%d.svg", perimeter_generator.layer_id,
                                                     is_contour ? 0 : 1, extrusion->inset_idx, i)
                                          .c_str(),
                                      bbox);

                    // Convert extrusion line to polygon for visualization
                    Polygon extrusion_polygon;
                    extrusion_polygon.points.reserve(extrusion->junctions.size());
                    for (const auto& junction : extrusion->junctions) {
                        extrusion_polygon.points.push_back(junction.p);
                    }

                    svg.draw_outline(perimeter_generator.slices->surfaces);
                    svg.draw_outline(extrusion_polygon, "green");
                    svg.draw(r.second, "red", 0.5);
                    svg.draw_outline(r.second, "red");
                    svg.Close();
                    i++;
                }
            }
#endif

            // Split the loops into lines with different config, and fuzzy them separately
            for (const auto& r : fuzzified_regions) {
                const auto splitted = Algorithm::split_line(*extrusion, r.second, false);
                if (splitted.empty()) {
                    // No intersection, skip
                    continue;
                }

                // Fuzzy splitted extrusion
                if (std::all_of(splitted.begin(), splitted.end(), [](const Algorithm::SplitLineJunction& j) { return j.clipped; })) {
                    // The entire polygon is fuzzified
                    fuzzy_extrusion_line(extrusion->junctions, slice_z, r.first);
                    continue;
                } else {
                    const auto                              current_ext = extrusion->junctions;
                    std::vector<Arachne::ExtrusionJunction> segment;
                    segment.reserve(current_ext.size());
                    extrusion->junctions.clear();

                    const auto fuzzy_current_segment = [&segment, &extrusion, &r, slice_z]() {
                        // Orca: non fuzzy points to isolate fuzzy region
                        const auto front = segment.front();
                        const auto back  = segment.back();

                        fuzzy_extrusion_line(segment, slice_z, r.first, false);
                        // Orca: only add non fuzzy point if it's not in the extrusion closing point.
                        if (!extrusion->junctions.empty() && extrusion->junctions.front().p != front.p) {
                            extrusion->junctions.push_back(front);
                        }
                        extrusion->junctions.insert(extrusion->junctions.end(), segment.begin(), segment.end());
                        // Orca: only add non fuzzy point if it's not in the extrusion closing point.
                        if (!extrusion->junctions.empty() && extrusion->junctions.back().p != front.p) {
                            extrusion->junctions.push_back(back);
                        }
                        segment.clear();
                    };

                    const auto to_ex_junction = [&current_ext](const Algorithm::SplitLineJunction& j) -> Arachne::ExtrusionJunction {
                        Arachne::ExtrusionJunction res = current_ext[j.get_src_index()];
                        if (!j.is_src()) {
                            res.p = j.p;
                        }
                        return res;
                    };

                    for (const auto& p : splitted) {
                        if (p.clipped) {
                            segment.push_back(to_ex_junction(p));
                        } else {
                            if (segment.empty()) {
                                extrusion->junctions.push_back(to_ex_junction(p));
                            } else {
                                segment.push_back(to_ex_junction(p));
                                fuzzy_current_segment();
                            }
                        }
                    }
                    if (!segment.empty()) {
                        fuzzy_current_segment();
                    }

                    //Orca: ensure the loop is closed after fuzzy
                    if (!extrusion->junctions.empty() && extrusion->junctions.front().p != extrusion->junctions.back().p) {
                        extrusion->junctions.back().p = extrusion->junctions.front().p;
                        extrusion->junctions.back().w = extrusion->junctions.front().w;
                    }
                }
            }
        }
    }
}

} // namespace Slic3r::Feature::FuzzySkin
