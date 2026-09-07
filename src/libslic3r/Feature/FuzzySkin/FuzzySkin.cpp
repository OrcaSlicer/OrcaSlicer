#include <random>

#include "libslic3r/Algorithm/LineSplit.hpp"
#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ClipperTools.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "FuzzySkin.hpp"

#include "libnoise/noise.h"

//#define DEBUG_FUZZY

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
//   phase_shift   = period_index * (ripple_offset / 100) * 2π  [radians]
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
    const double raw_shift       = period_index * (cfg.ripple_offset/100) * (2.0 * M_PI);
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

    if (poly.size() < 2)
        return;

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
        if (--point_idx == 0)
            break;
        --point_idx;
    }
    if (out.size() >= 3)
        poly = std::move(out);
}

// internal fuzzy skin parameters
struct FuzzySkinParams
{
    Arachne::ExtrusionJunctions& out;
    Arachne::ExtrusionJunctions& out2;
    FuzzySkinMode                current_mode;
    CornerType                   current_corner_type;
    const FuzzySkinConfig&       cfg;
    double                       z;
    bool                         one_way_expansion; // one_way_expansionmetric skin (true = Displacement+, Combined)
    noise::module::Module&       noise;
    coord_t                      params[3] = {0, 0, 0};
    bool                         draw_fill = true;
    bool                         draw_line = true;
    bool                         draw_corner = false;
    bool                         draw_required = false;
};

template<typename T> 
static double get_noise_value(const noise::module::Module& noise, const T& point, double z, bool one_way_expansion) {
    return one_way_expansion ? std::max(noise.GetValue(unscale_(point.x()), unscale_(point.y()), z) * .5 + .5, 0.) :
                               noise.GetValue(unscale_(point.x()), unscale_(point.y()), z);
};

#define WIDELINE_LIMIT 2.5
// Function for drawing a new point(s) on the fuzzy line
// vector - a variable that determines the direction to the required fuzzy skin point relative to the base point.
// j      - Extrusion base Junction point with coordinates, thickness and index. The index is a serial number of the wall.
//          The index changes for complex lines or void fillers, if they are generated (< 0 or > 99999).
//          Complex lines will be printed with the inner walls parameters before printing the outer wall.
// p      - additional parameters of Fuzzy Skin

static inline void place_point(Arachne::ExtrusionJunction& j, Arachne::ExtrusionJunctions& m, size_t tolerance) { 
    if ((!m.size() || (m.back().p - j.p).norm() > tolerance || m.back().w != j.w))
        m.push_back(j);
}

//static inline void place_point(Vec2d p, coord_t w, coord_t perimeter_index, Arachne::ExtrusionJunctions& m, size_t tolerance) 
//    { place_point(Arachne::ExtrusionJunction(p.cast<coord_t>(), w, perimeter_index), m, tolerance); }

static void fuzzy_point(Vec2d vector, Arachne::ExtrusionJunction j, FuzzySkinParams& p) {
    Vec2d point(j.p.cast<double>());
    Vec2d new_point(point + vector);
    Vec2d norm(vector.normalized());
    double distance = abs(vector.norm());
    const coord_t wall_width = p.cfg.wall_width;
    const coord_t real_width = j.w;
    const coord_t limit = real_width / 8;
    const coord_t index = j.perimeter_index;
    coord_t counter = p.params[0] / std::min(p.cfg.point_distance, limit); // reduce resolution up to wall real_width (nozzle dizmeter);
    Arachne::ExtrusionJunction j1(new_point.cast<coord_t>(), j.w, index);
    Arachne::ExtrusionJunction j2(new_point.cast<coord_t>(), 0, -1);
    switch (p.current_mode) {
    case FuzzySkinMode::Displacement: // classical algorithm, no any changed, two-way expansion
        place_point(j1, p.out, SCALED_EPSILON);
        break;
    case FuzzySkinMode::Displacement_plus: // classical algorithm, one-way expansion
        if (p.draw_line)
            place_point(j1, p.out, SCALED_EPSILON);
         if (p.draw_fill) {
            counter = p.params[0] / real_width;
            if ((counter != p.params[1]) || p.draw_required) {
                p.params[1] = counter;
                if (p.cfg.noise_type == NoiseType::Voronoi && !p.draw_corner) {
                    const Point side((perp(vector).normalized() * real_width * 0.5).cast<coord_t>());
                    double dist2 = std::min(get_noise_value(p.noise, j.p + side, p.z, p.one_way_expansion),
                                            get_noise_value(p.noise, j.p - side, p.z, p.one_way_expansion));
                    distance     = std::min(distance, dist2 * p.cfg.thickness);
                }
                if (distance < real_width * WIDELINE_LIMIT) { // conditions for determining when a wide line or zig-zag is filled
                    if (p.draw_corner)
                        distance *= 0.5;
                    if (p.out2.size() && p.params[2]) 
                        p.out2.back().w = j2.w = real_width;
                    else
                        j2.w = distance;
                    j2.p = j.p + (norm * (distance - j.w) * 0.5).cast<coord_t>();
                    p.params[2] = 0;
                } else { // draw zig-zag filling
                    if (!p.params[2] || p.out2.size())
                        p.out2.back().w = real_width;
                    j2.w = real_width;
                    distance -= real_width;
                    j2.p = (!p.draw_required && p.params[2]++ % 2) ? 
                                (p.draw_corner ? j.p + (norm * distance * 0.33).cast<coord_t>() : j.p) :
                                j.p + (norm * distance).cast<coord_t>();
                }
                if (j2.w)
                    place_point(j2, p.out2, SCALED_EPSILON);
            }
        }
        break;
    case FuzzySkinMode::Extrusion:
        if (p.cfg.point_distance >= limit || counter != p.params[1]) {
            p.params[1] = counter;
            j1.p        = point.cast<coord_t>();
            j1.w        = std::max(double(p.cfg.thickness) - distance, double(p.cfg.minimal_line));
            place_point(j1, p.out, SCALED_EPSILON);
        }
        break;
    case FuzzySkinMode::Combined:
        if (p.cfg.point_distance >= limit || counter != p.params[1]) {
            p.params[1] = counter;
            distance   += p.cfg.minimal_line;
            j1.p = (point + norm * (distance - real_width) * 0.5).cast<coord_t>();
            j1.w = distance;
            place_point(j1, p.out, SCALED_EPSILON);
        }
        break;
    case FuzzySkinMode::Fur:
        if (p.cfg.point_distance >= limit || counter != p.params[1] || p.draw_required) {
            p.params[1] = counter;
            j1.p        = (!p.draw_required && (p.params[2]++ % 2) ?
                               (p.one_way_expansion ? (p.draw_corner ? point + norm * distance * 0.5 : point) : point - vector) :
                               new_point).cast<coord_t>();
            j1.w        = p.cfg.point_distance < wall_width ? wall_width * p.cfg.point_distance / real_width : real_width;
            place_point(j1, p.out, SCALED_EPSILON);
            //place_point({(!p.draw_required && (p.params[2]++ % 2) ?
            //                 (p.one_way_expansion ? (p.draw_corner ? point + norm * distance * 0.5 : point) : point - vector) :
            //                  new_point).cast<coord_t>(),
            //             p.cfg.point_distance < wall_width ? wall_width * p.cfg.point_distance / real_width : real_width, index},
            //            p.out, SCALED_EPSILON);
        }
        break;
    }
};

// Thanks Cura developers for the base of this function.
// Orca: This function has been rewritten to increase its functionality.
// The initial extrusion line is converted to a Clipper compatible geometry, after that it can be filtered and converted into the desired result using standard painting functions.
// In the main loop, the polyline is divided into lines, and the gaps between them are filled using the angle formation algorithm.
// The parameters of the calculation point are passed to the position calculation function, which, among other things, 
// allows you to predict the characteristics and behavior of the final coordinate of the fuzzy envelope curve. 
// It is also possible to calculate the characteristics of an additional curve, such as those that fill in the gaps 
// if they were left over from the classical filling process. This method is also convenient for creating a textured and parametric fuzzy shell.
// At the end of the process, the resulting extrusion line is finally filtered to ensure unparalleled print quality.
// For clarity, the debugging section (DEBUG_FUZZY) was modified so that you can see all the features of the algorithms being used.

static void fuzzy_extrusion_line(Arachne::ExtrusionJunctions& ext_lines, coordf_t slice_z, const FuzzySkinConfig& cfg, bool closed) {
    if (cfg.noise_type == NoiseType::Ripple) {
        if (ext_lines.size() < 3)
            return;
        fuzzy_extrusion_line_ripple(ext_lines, cfg);
        return;
    }

    // Convert into polyline with Clipper geometry logic
    const bool& is_loop = closed;                             // 'closed' alias. It is better to leave it in case you need to check and refine the status of almost closed lines.
    Polyline main_contour(to_polyline(ext_lines, is_loop));   // keep the whole line for future use. Save the outline immediately to prevent the ext_lines reference from changing

    const size_t index(ext_lines.front().perimeter_index);    // perimeter index alias
    const double real_width(ext_lines[0].w);                  // real width alias (need to recalc by whole path)
    const double wall_width(cfg.wall_width);                  // wall width alias
    const double thickness(cfg.thickness);                    // thickness alias
    Arachne::ExtrusionJunction s_point(main_contour[0], real_width, index); 
    Arachne::ExtrusionJunction e_point(main_contour.back(), real_width, index); 

#define FILTER_CURLS     (thickness * 3.5)                    // the length of the line on which the self-intersections will be searched
#define FILTER_PERIMETER (thickness * 2)                      // the length of perimeter of the small polygons
#define FILTER_AREA      (std::pow(thickness, 2) * 0.25)      // the area of the small polygons
#define FILTER_IN_LENGTH (thickness)                          // the min length of the input line. These are usually is a short lines that are completely hidden in the fuzzy skin.

    // Simplify main contour
    main_contour.simplify(SCALED_EPSILON);
    const size_t size = main_contour.size();
    if (size < 2)
        return;

    // Calculate polyline length
    coord_t overall        = 0;
    for (size_t it = 1; it < size; it++) 
        overall += (main_contour[it] - main_contour[it - 1]).norm();

    const double distance  = cfg.point_distance;
    bool         one_way   = cfg.mode == FuzzySkinMode::Displacement_plus || cfg.mode == FuzzySkinMode::Combined || cfg.mode == FuzzySkinMode::Fur;
    
    // Return if the line too short or has undesired index
    if ((closed && overall < FILTER_IN_LENGTH) || (index > 0 && one_way))
        return;

    const bool is_random   = cfg.noise_type == NoiseType::Classic || cfg.mode == FuzzySkinMode::Fur;
    const bool is_thicked  = cfg.mode == FuzzySkinMode::Extrusion || cfg.mode == FuzzySkinMode::Combined;
    const bool is_filtered = cfg.mode == FuzzySkinMode::Displacement_plus || cfg.mode == FuzzySkinMode::Fur;

    // Determinate the random first point of the loop for best randomization
    if (is_loop && is_filtered) {
        main_contour.points.pop_back();
        std::rotate(main_contour.points.begin(), main_contour.points.begin() + random_value() * (size - 1), main_contour.points.end());
        main_contour.points.push_back(main_contour.points.front());
    }

    Arachne::ExtrusionJunctions out, out2; // out2 - additional complex contour (voids infill) for some modes
    size_t cap = (overall / std::min(distance, wall_width / 8) + size);
    out.reserve(std::min(cap, out.max_size()));
    if (cfg.mode == FuzzySkinMode::Displacement_plus)
        out2.reserve(std::min(cap * 2, out2.max_size()));
    
    const double min_dist_between_points = is_random ? distance * .75 : distance; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const double range_random_point_dist = is_random ? distance * .5  : 0.;       // is_random ? ... - disabled for the test function of precise corners when using 3D noise
    double dist_left_over = random_value() * (min_dist_between_points * .5);      // the distance to be traversed on the line before making the first new point
    coord_t minimal_line  = cfg.minimal_line;                                     // Minimal linewidth for this condition for height and spacing: parsms * float(1. - 0.25 * PI);

    std::unique_ptr<noise::module::Module> noise = get_noise_module(cfg);

    // Use this var for length calc. Determinate first random point for Fur and Displacement+ infill 
    overall += dist_left_over * min_dist_between_points;
    FuzzySkinParams params{out,     out2,   cfg.mode, cfg.corner_type, cfg, slice_z,
                           one_way, *noise, {overall, rand() * coord_t(distance) + overall, rand()}}; //{overall, overall + random_value() * distance, 0}

    // Get the angles map of cornerrs
    std::vector<double> corners(size, 0.);
    double last_angle = 0;
    for (int i = 0; i < size - 1; i++) {
        Point p       = main_contour[i + 1] - main_contour[i];
        double angle  = atan2(p.y(), p.x());
        corners[i]    = constrainPI(angle - last_angle);
        last_angle    = angle;
    }


    // Start point of the contour
    Vec2d p0(main_contour[0].cast<double>());
    Vec2d p1(main_contour[1].cast<double>());
    Vec2d p0p1(p1 - p0);
    Vec2d pa;
    Vec2d vect0(p0p1.normalized()); // line vector for current line
    Vec2d perp0;                    // perpendicular for the vector
    double r = get_noise_value(*noise, p0, slice_z, one_way) * thickness;
    Arachne::ExtrusionJunction j(p0.cast<coord_t>(), real_width, index);

    if (is_loop) 
        corners[0] = corners[size - 1] = constrainPI(corners[0] - last_angle);
    else {
        out.push_back(s_point); // connect the start end for unclosed path
        corners[0] = corners[size - 1] = 0.;
        // Set a bypass for non-closed paths
        // if (params.current_mode == FuzzySkinMode::Displacement_plus) {
        //    params.one_way_expansion = false;
        //    params.current_mode = FuzzySkinMode::Displacement;
        //}
        // if (params.current_mode == FuzzySkinMode::Combined) {
        //    params.one_way_expansion = false;
        //    params.current_mode = FuzzySkinMode::Extrusion;
        //}
    }

    switch (params.current_mode) {
    case FuzzySkinMode::Displacement: // classical algorithm, no any changed, two-way expansion
        params.one_way_expansion = one_way = false;
        break;
    }

    // Automatic corner shaper for certain types of fuzzy lines
    if (params.cfg.corner_type == CornerType::Auto) {
        params.current_corner_type = CornerType::Trapezoid; // default
        if (params.current_mode == FuzzySkinMode::Fur)
            params.current_corner_type = CornerType::Trapezoid;
        if (params.current_mode == FuzzySkinMode::Displacement_plus)
            switch (params.cfg.noise_type) {
            case NoiseType::RidgedMulti:
            case NoiseType::Voronoi: params.current_corner_type = CornerType::Full; break;
            case NoiseType::Billow:
            case NoiseType::Perlin: params.current_corner_type = CornerType::Round;
            }
        else
            switch (params.cfg.noise_type) {
            case NoiseType::RidgedMulti:
            case NoiseType::Voronoi: params.current_corner_type = CornerType::Full; break;
            case NoiseType::Classic: params.current_corner_type = CornerType::Round; break;
            }
    }

    for (size_t it = 1; it < size; it++) {
        p1 = main_contour[it].cast<double>();
        
        p0p1             = p1 - p0;
        vect0            = p0p1.normalized(); // line vector
        perp0            = perp(vect0);       // perpendicular of the p0p1 vector
        pa               = p0;                // 'a' is the (next) new point between p0 and p1
        double p0p1_size = p0p1.norm();
        double p0pa_dist = dist_left_over;

        for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points + random_value() * range_random_point_dist) {
            pa               = p0 + p0p1 * p0pa_dist / p0p1_size;
            params.params[0] = overall + p0pa_dist;
            fuzzy_point(perp0 * get_noise_value(*noise, pa, slice_z, one_way) * thickness,
                        Arachne::ExtrusionJunction (pa.cast<coord_t>(), real_width, index), params);
        }
        overall += p0p1_size;
 
        double const current_corner = corners[it];
        double const current_corner_2(current_corner / 2.);
        double const direction0     = constrainPI(atan2(vect0.y(), vect0.x()));
        double const direction1     = constrainPI(direction0 + current_corner);
        double theta                = 0.;
        Vec2d vect1(cos(direction1), sin(direction1)); // vector of p1p2
        Vec2d perp1(perp(vect1));
        r   = get_noise_value(*noise, p1, slice_z, one_way) * thickness;
        j.p = p1.cast<coord_t>();                      // set universal junction with wall wall_width parameter, if the extruded line is of variable wall_width, replace this parameter with "wall_width" of the point 

        // Draw a needed point outside perimeter at visible corner
        if (current_corner * r < 0.) {                 // for outside corners only
            if (abs(current_corner) > M_PI_8) {        // for weight angles only
                params.draw_corner = true;
                double _rs, _re;
                Vec2d vect_d;
                Vec2d sec_point;
                switch (params.current_corner_type) {  // end line point
                case CornerType::Groove:               // form the groove corner
                    params.draw_required = true;
                    fuzzy_point(perp0 * r, j, params);
                    params.draw_fill = false;
                    fuzzy_point(Vec2d(0., 0.), j, params);
                    params.draw_fill = true;
                    fuzzy_point(perp1 * r, j, params);
                    params.draw_required = false;
                    break;
                case CornerType::Cut: // form the cut corner
                    fuzzy_point(perp0 * r, j, params);
                    fuzzy_point(perp1 * r, j, params);
                    break;
                case CornerType::Trapezoid: // form the trapezoid corner
                    p0pa_dist -= p0p1_size;
                    theta     = constrainPI(direction0 + current_corner_2);
                    vect_d    = Vec2d(cos(theta), sin(theta));    // vector of connection line
                    sec_point = perp0 * r;                        // start point of corner line
                    p0p1_size = ((perp1 * r) - sec_point).norm(); // length of connection line
                    for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points + random_value() * range_random_point_dist) {
                        params.params[0] = overall + p0pa_dist;
                        Vec2d _vect_t(sec_point + vect_d * p0pa_dist); // vector of third point
                        fuzzy_point(_vect_t, j, params);
                    }
                    overall += p0p1_size;
                    break;
                case CornerType::Spike: // form the corner by its vertex
                    params.draw_required = true;
                    theta                = constrainPI(atan2(perp0.y(), perp0.x()) + current_corner_2);
                    if (abs(current_corner) > M_3PI_4) // for angles greater than 135 degrees, the vertex offset should be limited
                        fuzzy_point(Vec2d(cos(theta), sin(theta)) * r, j, params);
                    else
                        fuzzy_point(Vec2d(cos(theta), sin(theta)) * r / abs(cos(current_corner_2)), j, params);
                    params.draw_required = false;
                    break;
                case CornerType::Full: // form the full corner (harcoded, but it's work)
                    if (abs(current_corner) > M_3PI_4) // for angles greater than 135 degrees, the vertex offset should be limited
                        _rs = _re = get_noise_value(*noise, p1, slice_z, one_way) * thickness;
                    else { // otherwise get the real offset of the corner
                        _rs = abs(tan(current_corner_2)) * get_noise_value(*noise, p1 - vect1 * thickness, slice_z, one_way) * thickness;
                        _re = abs(tan(current_corner_2)) * get_noise_value(*noise, p1 + vect0 * thickness, slice_z, one_way) * thickness;
                    }
                    //params.draw_fill = false;
                    p0pa_dist -= p0p1_size;
                    p0p1_size = _rs;
                    for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points + random_value() * range_random_point_dist) {
                        params.params[0] = overall + p0pa_dist;
                        fuzzy_point(vect0 * p0pa_dist + perp0 * r, j, params);
                    }
                    overall += p0p1_size;
                    p0pa_dist -= p0p1_size;
                    p0p1_size = _re;
                    for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points + random_value() * range_random_point_dist) {
                        params.params[0] = overall + p0pa_dist;
                        fuzzy_point(vect1 * (p0pa_dist - p0p1_size) + perp1 * r, j, params);
                    }
                    overall += p0p1_size;
                    break;
                case CornerType::Round: // form round corner
                    p0pa_dist -= p0p1_size;
                    p0p1_size = abs(current_corner * r);
                    for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points + random_value() * range_random_point_dist) {
                        params.params[0] = overall + p0pa_dist;
                        theta            = constrainPI(atan2(perp0.y(), perp0.x()) + current_corner * p0pa_dist / p0p1_size);
                        fuzzy_point(Vec2d(cos(theta), sin(theta)) * r, j, params);
                    }
                    overall += p0p1_size;
                    break;
                }
                params.draw_corner = false;
            } else {
                params.params[0] = overall + p0pa_dist;
                //fuzzy_point(perp0 * r, j, params);
                theta = constrainPI(atan2(perp0.y(), perp0.x()) + current_corner_2);
                fuzzy_point(Vec2d(cos(theta), sin(theta)) * r / abs(cos(current_corner_2)), j, params);
                //fuzzy_point(perp1 * r, j, params);
            }
        }
        dist_left_over = p0pa_dist - p0p1_size;
        p0             = p1;
    }
 
    // Connect points
    if (!is_loop)
        out.push_back(e_point); // connect the finish end for unclosed path
    else if (out.front().p != out.back().p || out.front().w != out.back().w)
        out.push_back(out.front()); // if the closed path is not closed, close it

    if (out.size() < 2) // bypass for negative conditions
        return;

    // Check for minimal extrusion
    for (Arachne::ExtrusionJunction& ej : out)
        if (ej.w < cfg.minimal_line)
            ej.w = cfg.minimal_line;

    Polyline raw_contour(to_polyline(out, is_loop));
    raw_contour.simplify(SCALED_EPSILON);
    if (raw_contour.size() < 2) // bypass simplify fiolter
        raw_contour = to_polyline(out, is_loop);

    if (raw_contour.size() < 2) // bypass for negative conditions
        return;

    bool dir(is_counter_clockwise(main_contour) && is_loop);
    const size_t width_2 = wall_width / 2;
   
#ifdef DEBUG_FUZZY
    {
        BoundingBox bbox = get_extents(main_contour);
        bbox.offset(thickness * 2);

        // Set region counters
        static time_t last_session;
        time_t current_time = std::time(nullptr);
        static std::vector<size_t> regions(1024, 0);
        size_t layer_id(cfg.layer_id);
        
        static std::mutex debug_mute;
        debug_mute.lock();
        if (last_session + 3 < current_time)
            regions = std::vector<size_t>(1024, 0);
        if (regions.size() < layer_id)
            regions.resize(layer_id * 2, 0);
        regions[layer_id]++;
        last_session = current_time;
        debug_mute.unlock();

        ::Slic3r::SVG svg(debug_out_path("fuzzy_skin_%d_z%.2f_%s_%d.svg", layer_id + 1, slice_z, is_loop ? dir ? "int" : "ext" : "reg", regions[layer_id]).c_str(), bbox);

        const size_t width_4 = real_width / 4;
        const size_t width_8 = real_width / 8;

        // initial path
        draw(svg, main_contour, "red", wall_width, true);
        draw(svg, main_contour, "darkred", width_8, false);

        // raw outer fuzzy skin contour
        draw(svg, raw_contour, "orange", wall_width, true);
        draw(svg, raw_contour, "red", width_8, false);

        const ThickLines raw_lines(to_thick_lines(out, is_loop));
        // outer fuzzy skin limit
        Polyline fuzzy_shell_limit(offset_by_polygon(filter_by_polygon(main_contour, -wall_width), -thickness));
        draw(svg, offset_by_polygon(main_contour, -thickness), "darkcyan", width_4);
        draw(svg, fuzzy_shell_limit, "cyan", width_4);

        // filtered outer fuzzy skin contour
        if (cfg.mode == FuzzySkinMode::Displacement_plus) {
            const Polyline  fuzzy_filtered(filter_curls_reject(raw_contour, FILTER_CURLS));
            const Polylines fuzzy_contours(is_loop ? filter_contour(fuzzy_filtered, Polygon(fuzzy_shell_limit.points), -(double)(width_2),
                                                                    FILTER_PERIMETER, FILTER_AREA, SCALED_EPSILON) : Polylines({fuzzy_filtered}));
            draw(svg, fuzzy_contours, "blue", real_width, true);
            draw(svg, fuzzy_contours, "black", width_8, false);

            if (out2.size() > 1) {
                Polylines fuzzy_contour_limit;
                Polylines fuzzy_complex_limit;
                Polylines combined_contour;
                if (is_loop) {
                    if (fuzzy_contours.size()) {
                        fuzzy_contour_limit = {offset_by_polygon(main_contour, -wall_width * (WIDELINE_LIMIT - 0.5))};
                        fuzzy_complex_limit = {offset_by_polygon(fuzzy_contours, wall_width)};
                        combined_contour = combine_3(fuzzy_contour_limit, fuzzy_complex_limit, offset_by_polygon(fuzzy_contours, width_2));
                   } 
                } else {
                    if (fuzzy_contours.size()) {
                        Polyline main2(offset_(main_contour, width_2, FILTER_CURLS));
                        main2.reverse();
                        fuzzy_contour_limit = {combine(offset_(main_contour, -wall_width * (WIDELINE_LIMIT - 0.5), FILTER_CURLS),main2)};
                        fuzzy_complex_limit = {combine(offset_(fuzzy_filtered, wall_width, FILTER_CURLS), main2)};
                        combined_contour    = combine_3(fuzzy_contour_limit, fuzzy_complex_limit, 
                                                        Polylines{combine(offset_(fuzzy_filtered, width_2, FILTER_CURLS), main2)});
                    } 
                }
                const ThickLines in_contour(to_thick_lines(out2, is_loop));
                const ThickLines filtered_complex(filter_by_crop_contour(in_contour, combined_contour, FILTER_CURLS));
                // complex fill line
                draw(svg, in_contour, "yellow", wall_width, true);
                draw(svg, in_contour, "brown", width_8, false);

                // filtered complex fill line
                draw(svg, filtered_complex, "wheat", wall_width, true);
                draw(svg, filtered_complex, "chocolate", width_8, false);

                // shuttle fill limit
                if (fuzzy_contour_limit.size())
                    draw(svg, fuzzy_contour_limit, "pink", width_4);

                // outer contour limit
                if (fuzzy_complex_limit.size())
                    draw(svg, fuzzy_complex_limit, "violet", width_4);

                // shuttle fill limit combined with outer contour
                if (combined_contour.size())
                    draw(svg, combined_contour, "magenta", width_4);
            }
        } else {
            ThickLines thick_contour;
            if (is_loop && cfg.mode == FuzzySkinMode::Fur) {
                Polylines fuzzy_contours = filter_contour(filter_curls_reject(raw_contour, -thickness),
                                                          Polygon(fuzzy_shell_limit.points), 
                                                          0, FILTER_PERIMETER, FILTER_AREA, SCALED_EPSILON);
                thick_contour = to_thick_lines(dir ? get_counter_clockwise(fuzzy_contours) : 
                                                     get_clockwise(fuzzy_contours), real_width);
            } else if (cfg.mode == FuzzySkinMode::Combined)
                thick_contour = filter_curls_reject(simplify(to_thick_lines(out, is_loop)), FILTER_CURLS);
            else
                thick_contour = simplify(to_thick_lines(out, is_loop));
            draw(svg, thick_contour, "blue", real_width, true);
            draw(svg, thick_contour, "black", width_8, false);
        }
        svg.Close();
    }
#endif

    // filter out lines
    if (cfg.mode == FuzzySkinMode::Displacement_plus) {
        const Polyline fuzzy_shell_limit(offset_by_polygon(filter_by_polygon(main_contour, -wall_width), -thickness));
        const Polyline  fuzzy_filtered(filter_curls_reject(raw_contour, FILTER_CURLS));
        const Polylines fuzzy_contours(is_loop ? filter_contour(fuzzy_filtered, Polygon(fuzzy_shell_limit.points), -(double)(width_2),
                                                                FILTER_PERIMETER, FILTER_AREA, SCALED_EPSILON) : 
                                                                Polylines({fuzzy_filtered}));
        out = to_extrusion(fuzzy_contours, real_width, index, false, true);
         
        if (out2.size() > 1) {
            Polylines fuzzy_contour_limit;
            Polylines fuzzy_complex_limit;
            Polylines combined_contour;
            if (is_loop) {
                if (fuzzy_contours.size()) {
                    fuzzy_contour_limit = {offset_by_polygon(main_contour, -wall_width * (WIDELINE_LIMIT - 0.5))};
                    fuzzy_complex_limit = {offset_by_polygon(fuzzy_contours, wall_width)};
                    combined_contour    = combine_3(fuzzy_contour_limit, fuzzy_complex_limit, offset_by_polygon(fuzzy_contours, width_2));
                }
            } else {
                if (fuzzy_contours.size()) {
                    Polyline main2(offset_(main_contour, width_2, FILTER_CURLS));
                    main2.reverse();
                    fuzzy_contour_limit = {combine(offset_(main_contour, -wall_width * (WIDELINE_LIMIT - 0.5), FILTER_CURLS), main2)};
                    fuzzy_complex_limit = {combine(offset_(fuzzy_filtered, wall_width, FILTER_CURLS), main2)};
                    combined_contour    = combine_3(fuzzy_contour_limit, fuzzy_complex_limit,
                                                    Polylines{combine(offset_(fuzzy_filtered, width_2, FILTER_CURLS), main2)});
                }
            }
            ThickLines filtered_complex(filter_by_crop_contour(to_thick_lines(out2, is_loop), combined_contour, FILTER_CURLS));
            filtered_complex.insert(filtered_complex.begin(), ThickLine(filtered_complex[0].a, filtered_complex[0].a, 1, filtered_complex[0].a_width));
            filtered_complex.push_back(ThickLine(filtered_complex.back().b, filtered_complex.back().b, filtered_complex.back().b_width, 1));
            out2.clear();
            for (Arachne::ExtrusionJunction& point : to_extrusion(filtered_complex, -1, false, true)) // copy points from additional contour filtered_complex
                out2.emplace_back(point);
        }
    } else {
        if (is_loop && cfg.mode == FuzzySkinMode::Fur) {
            const Polyline fuzzy_shell_limit(offset_by_polygon(filter_by_polygon(main_contour, -wall_width), -thickness));
            Polylines fuzzy_contours(filter_contour(filter_curls_reject(raw_contour, -thickness), Polygon(fuzzy_shell_limit.points),
                                                    0, FILTER_PERIMETER, FILTER_AREA, SCALED_EPSILON));
            fuzzy_contours = dir ? get_counter_clockwise(fuzzy_contours) : get_clockwise(fuzzy_contours);
            out            = to_extrusion(fuzzy_contours, real_width, index, false, true);
        } else if (cfg.mode == FuzzySkinMode::Combined)
            out = to_extrusion(filter_curls_reject(simplify(to_thick_lines(out, is_loop)), FILTER_CURLS), index, false, true);
        else
            out = to_extrusion(simplify(to_thick_lines(out, is_loop)), index, false, true);
    } 

    out.reserve(out.size() + out2.size() + 2);

    // Add complex paths
    if (out2.size() > 1) 
        for (Arachne::ExtrusionJunction& point : out2)
            out.push_back(point);
    if (out.size() > 1) 
        ext_lines = std::move(out); // return ref
};

void group_region_by_fuzzify(PerimeterGenerator& g)
{
    g.regions_by_fuzzify.clear();
    g.has_fuzzy_skin = false;
    g.has_fuzzy_hole = false;

    struct ConfigSurfaces {
        FuzzySkinConfig config;
        SurfacesPtr     surfaces;
    };

    std::vector<ConfigSurfaces> regions;
    regions.reserve(g.compatible_regions->size());
    for (auto region : *g.compatible_regions) {
        const auto&   region_config = region->region().config();
        const coord_t _wall_width(scaled<coord_t>((region_config.outer_wall_line_width.value ? 
                                                  region_config.outer_wall_line_width.value :
                                                  g.object_config->line_width.value)) * g.print_config->filament_flow_ratio.get_at(g.print_config->master_extruder_id.value));
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
                                  g.layer_id,
                                  region_config.corner_type,
                                  _wall_width,
                                  scaled<coord_t>(std::max(float(g.layer_height), g.ext_perimeter_flow.spacing()) * 0.215)}; // param * (1. - 0.25 * PI)};

        auto it = std::find_if(regions.begin(), regions.end(), [&cfg](const ConfigSurfaces& item) {
            return item.config == cfg;
        });

        if (it == regions.end()) {
            regions.push_back({cfg, {}});
            it = regions.end() - 1;
        }

        auto& surfaces = it->surfaces;
        for (const auto& surface : region->slices.surfaces) {
            surfaces.push_back(&surface);
        }

        if (should_fuzzify(cfg, g.layer_id, 0, true)) {
            g.has_fuzzy_skin = true;
        }
        if (should_fuzzify(cfg, g.layer_id, 0, false)) {
            g.has_fuzzy_hole = true;
        }
    }

    if (regions.size() == 1) { // optimization
        g.regions_by_fuzzify.push_back({regions.front().config, {}});
        return;
    }

    g.regions_by_fuzzify.reserve(regions.size());
    for (const auto& region : regions) {
        g.regions_by_fuzzify.push_back({region.config, offset_ex(region.surfaces, ClipperSafetyOffset)});
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

    const bool fuzzify_contours = (loop_idx == 0 && fuzziy_type != FuzzySkinType::Hole) || fuzziy_type == FuzzySkinType::AllWalls;
    const bool fuzzify_holes    = (fuzziy_type == FuzzySkinType::Hole || fuzziy_type == FuzzySkinType::All || fuzziy_type == FuzzySkinType::AllWalls)
                                  && (loop_idx == 0 || fuzziy_type == FuzzySkinType::AllWalls);

    return is_contour ? fuzzify_contours : fuzzify_holes;
}

struct MergedFuzzyRegion {
    const FuzzySkinConfig *config;
    ExPolygons             expolygons;
};

// Compare whether two configs produce the same fuzzy effect (ignoring type/first_layer
// which only control which loops get fuzzified, not the noise itself).
static bool same_fuzzy_effect(const FuzzySkinConfig& a, const FuzzySkinConfig& b)
{
    return a.thickness                    == b.thickness
        && a.point_distance               == b.point_distance
        && a.noise_type                   == b.noise_type
        && a.noise_scale                  == b.noise_scale
        && a.noise_octaves                == b.noise_octaves
        && a.noise_persistence            == b.noise_persistence
        && a.mode                         == b.mode
        && a.ripples_per_layer            == b.ripples_per_layer
        && a.ripple_offset                == b.ripple_offset
        && a.layers_between_ripple_offset == b.layers_between_ripple_offset;
}

static std::vector<MergedFuzzyRegion> collect_merged_fuzzy_regions(const std::vector<std::pair<FuzzySkinConfig, ExPolygons>>& regions,
                                                                   const int                                              layer_id,
                                                                   const size_t                                           loop_idx,
                                                                   const bool                                             is_contour)
{
    // Merge regions that produce identical fuzzy effects (differ only in type).
    // When the style (e.g. External) and a painted region (All) both fuzzify this loop
    // with the same noise parameters, merging their ExPolygons avoids splitting the
    // perimeter at the painted boundary — eliminating discontinuity artifacts.
    std::vector<MergedFuzzyRegion> merged_regions;
    merged_regions.reserve(regions.size());
    for (const auto& region : regions) {
        if (!should_fuzzify(region.first, layer_id, loop_idx, is_contour)) {
            continue;
        }

        bool merged = false;
        for (auto& merged_region : merged_regions) {
            if (same_fuzzy_effect(*merged_region.config, region.first)) {
                if (merged_region.expolygons.empty()) {
                    // Already full coverage, nothing to add.
                } else if (region.second.empty()) {
                    merged_region.expolygons.clear();
                } else {
                    append(merged_region.expolygons, region.second);
                }
                merged = true;
                break;
            }
        }

        if (!merged) {
            merged_regions.push_back({&region.first, region.second});
        }
    }

    for (auto& merged_region : merged_regions) {
        if (!merged_region.expolygons.empty()) {
            merged_region.expolygons = union_ex(merged_region.expolygons);
        }
    }

    return merged_regions;
}

Polygon apply_fuzzy_skin(const Polygon& polygon, const PerimeterGenerator& perimeter_generator, const size_t loop_idx, const bool is_contour)
{
    Polygon fuzzified;

    const auto  slice_z = perimeter_generator.slice_z;
    const auto& regions = perimeter_generator.regions_by_fuzzify;
    if (perimeter_generator.config->fuzzy_skin.value != FuzzySkinType::None) { // only for non-painting fuzzy
        const auto& config  = regions.begin()->first;
        const bool  fuzzify = should_fuzzify(config, perimeter_generator.layer_id, loop_idx, is_contour);
        if (!fuzzify) {
            return polygon;
        }

        fuzzified = polygon;
        fuzzy_polyline(fuzzified.points, true, slice_z, config);
        return fuzzified;
    }

    // Merge regions that produce identical fuzzy effects (differ only in type).
    // When the style (e.g. External) and a painted region (All) both fuzzify this loop
    // with the same noise parameters, merging their ExPolygons avoids splitting the
    // perimeter at the painted boundary — eliminating discontinuity artifacts.
    auto merged_regions = collect_merged_fuzzy_regions(regions, perimeter_generator.layer_id, loop_idx, is_contour);
    if (merged_regions.empty()) {
        return polygon;
    }

    // Fast path: single merged region — apply directly without splitting
    if (merged_regions.size() == 1) {
        const auto& mr = merged_regions.front();
        if (mr.expolygons.empty()) {
            fuzzified = polygon;
            fuzzy_polyline(fuzzified.points, true, slice_z, *mr.config);
            return fuzzified;
        }
        // Fall through to split_line with a single region below
    }

#ifdef DEBUG_FUZZY
    {
        int i = 0;
        for (const auto& r : merged_regions) {
            BoundingBox bbox = get_extents(perimeter_generator.slices->surfaces);
            bbox.offset(scale_(1.));
            ::Slic3r::SVG svg(debug_out_path("fuzzy_traverse_loops_%d_%d_%d_region_%d.svg", perimeter_generator.layer_id,
                                             is_contour ? 0 : 1, loop_idx, i)
                                  .c_str(),
                              bbox);
            svg.draw_outline(perimeter_generator.slices->surfaces);
            svg.draw_outline(polygon, "green");
            svg.draw(r.expolygons, "red", 0.5);
            svg.draw_outline(r.expolygons, "red");
            svg.Close();
            i++;
        }
    }
#endif

    // Make each region's ExPolygons exclusive so overlapping regions don't double-fuzz
    // the same perimeter section. Later regions in the list take priority over earlier ones
    // in overlapping areas (matching modifier precedence order).
    for (size_t i = 0; i < merged_regions.size(); ++i)
        for (size_t j = i + 1; j < merged_regions.size(); ++j)
            if (!merged_regions[i].expolygons.empty() && !merged_regions[j].expolygons.empty())
                merged_regions[i].expolygons = diff_ex(merged_regions[i].expolygons, merged_regions[j].expolygons);

    // Split the loops into lines with different config, and fuzzy them separately
    fuzzified = polygon;
    for (const auto& r : merged_regions) {
        auto splitted = Algorithm::split_line(fuzzified, r.expolygons, true);
        if (splitted.empty()) {
            // No intersection, skip
            continue;
        }

        // Fuzzy splitted polygon
        if (std::all_of(splitted.begin(), splitted.end(), [](const Algorithm::SplitLineJunction& j) { return j.clipped; })) {
            // The entire polygon is fuzzified
            fuzzy_polyline(fuzzified.points, true, slice_z, *r.config);
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
                fuzzy_polyline(segment, false, slice_z, *r.config);
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

void apply_fuzzy_skin(Arachne::ExtrusionLine* extrusion, const PerimeterGenerator& perimeter_generator, const bool is_contour, const bool closed)
{
    const auto  slice_z = perimeter_generator.slice_z;
    const auto& regions = perimeter_generator.regions_by_fuzzify;
    if (perimeter_generator.config->fuzzy_skin.value != FuzzySkinType::None) { // only for non-painting fuzzy
        const auto& config  = regions.begin()->first;
        const bool  fuzzify = should_fuzzify(config, perimeter_generator.layer_id, extrusion->inset_idx, is_contour);
        if (fuzzify)
            fuzzy_extrusion_line(extrusion->junctions, slice_z, config, closed);
    } else {
        // Merge regions that produce identical fuzzy effects (differ only in type).
        // When the style (e.g. External) and a painted region (All) both fuzzify this loop
        // with the same noise parameters, merging avoids splitting the perimeter at the
        // painted boundary — eliminating discontinuity artifacts.
        auto merged_regions = collect_merged_fuzzy_regions(regions, perimeter_generator.layer_id, extrusion->inset_idx, is_contour);
        if (!merged_regions.empty()) {

            // Fast path: single merged region — apply directly without splitting
            if (merged_regions.size() == 1 && merged_regions.front().expolygons.empty()) {
                fuzzy_extrusion_line(extrusion->junctions, slice_z, *merged_regions.front().config, closed);
                return;
            }

            // Open path means this is a thin wall that collapsed into a single thick line, in this case the path will go exactly
            // between the middle two sides of the object. And since the paint segmentation never goes beyond the middle line because
            // it uses voronoi diagram, we need to expand the segmentation a little bit to make sure it covers the path.
            if (!closed) {
                for (auto& r : merged_regions) {
                    r.expolygons = offset_ex(r.expolygons, perimeter_generator.ext_perimeter_flow.scaled_width() / 10);
                }
            }

#ifdef DEBUG_FUZZY
            {
                int i = 0;
                for (const auto& r : merged_regions) {
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
                    svg.draw(r.expolygons, "red", 0.5);
                    svg.draw_outline(r.expolygons, "red");
                    svg.Close();
                    i++;
                }
            }
#endif

            // Make each region's ExPolygons exclusive so overlapping regions don't double-fuzz
            // the same perimeter section. Later regions in the list take priority over earlier ones
            // in overlapping areas.
            for (size_t i = 0; i < merged_regions.size(); ++i)
                for (size_t j = i + 1; j < merged_regions.size(); ++j)
                    if (!merged_regions[i].expolygons.empty() && !merged_regions[j].expolygons.empty())
                        merged_regions[i].expolygons = diff_ex(merged_regions[i].expolygons, merged_regions[j].expolygons);

            // Split the loops into lines with different config, and fuzzy them separately
            for (const auto& r : merged_regions) {
                const auto splitted = Algorithm::split_line(*extrusion, r.expolygons, false);
                if (splitted.empty()) {
                    // No intersection, skip
                    continue;
                }

                // Fuzzy splitted extrusion
                if (std::all_of(splitted.begin(), splitted.end(), [](const Algorithm::SplitLineJunction& j) { return j.clipped; })) {
                    // The entire polygon is fuzzified
                    fuzzy_extrusion_line(extrusion->junctions, slice_z, *r.config, closed);
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

                        fuzzy_extrusion_line(segment, slice_z, *r.config, false);
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
                    if (closed && !extrusion->junctions.empty() && extrusion->junctions.front().p != extrusion->junctions.back().p) {
                        extrusion->junctions.back().p = extrusion->junctions.front().p;
                        extrusion->junctions.back().w = extrusion->junctions.front().w;
                    }
                }
            }
        }

    }
}

} // namespace Slic3r::Feature::FuzzySkin
