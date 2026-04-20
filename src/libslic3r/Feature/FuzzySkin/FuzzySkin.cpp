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

// Thanks Cura developers for this function.
void fuzzy_polyline(Points& poly, bool closed, coordf_t slice_z, const FuzzySkinConfig& cfg)
{
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

    struct ConfigSurfaces {
        FuzzySkinConfig config;
        SurfacesPtr     surfaces;
    };

    std::vector<ConfigSurfaces> regions;
    regions.reserve(g.compatible_regions->size());
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
                                  region_config.fuzzy_skin_mode};

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
    return a.thickness         == b.thickness
        && a.point_distance    == b.point_distance
        && a.noise_type        == b.noise_type
        && a.noise_scale       == b.noise_scale
        && a.noise_octaves     == b.noise_octaves
        && a.noise_persistence == b.noise_persistence
        && a.mode              == b.mode;
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
        // Merge regions that produce identical fuzzy effects (differ only in type).
        // When the style (e.g. External) and a painted region (All) both fuzzify this loop
        // with the same noise parameters, merging avoids splitting the perimeter at the
        // painted boundary — eliminating discontinuity artifacts.
        auto merged_regions = collect_merged_fuzzy_regions(regions, perimeter_generator.layer_id, extrusion->inset_idx, is_contour);
        if (!merged_regions.empty()) {

            // Fast path: single merged region — apply directly without splitting
            if (merged_regions.size() == 1 && merged_regions.front().expolygons.empty()) {
                fuzzy_extrusion_line(extrusion->junctions, slice_z, *merged_regions.front().config);
                return;
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
                    fuzzy_extrusion_line(extrusion->junctions, slice_z, *r.config);
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
