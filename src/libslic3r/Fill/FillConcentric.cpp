#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Surface.hpp"
#include "../VariableWidth.hpp"
#include "Arachne/WallToolPaths.hpp"

#include "FillConcentric.hpp"
#include <libslic3r/ShortestPath.hpp>

namespace Slic3r {

static bool should_spiralize_concentric(const FillParams &params)
{
    return params.config != nullptr &&
           params.config->spiralized.value &&
           (params.extrusion_role == erTopSolidInfill || params.extrusion_role == erBottomSurface);
}

static Points loop_points_opened(Polyline loop_path)
{
    if (loop_path.points.size() > 1 && loop_path.points.front() == loop_path.points.back())
        loop_path.points.pop_back();
    return std::move(loop_path.points);
}

static Polylines generate_spiralized_concentric_polylines(
    const FillParams &params, 
    const Polygons   &loops, 
    const coord_t     distance,
    const bool        is_classic)
{
    Polylines output;
    Polyline spiral;
    Point current_pos(0, 0);
    const double jump_threshold = 2.0 * double(distance);

    auto find_sharpest_corner = [](const Polygon& loop) -> int {
        size_t n = loop.points.size();
        if (n < 3) return 0;

        double max_cos = -2.0;
        int best_idx   = 0;
        for (size_t i = 0; i < n; ++i) {
            const Point& p_prev = loop.points[(i - 1 + n) % n];
            const Point& p      = loop.points[i];
            const Point& p_next = loop.points[(i + 1) % n];

            Vec2d v1    = (p_prev - p).cast<double>();
            Vec2d v2    = (p_next - p).cast<double>();
            double len1 = v1.norm();
            double len2 = v2.norm();
            if (len1 < 1e-6 || len2 < 1e-6) continue;

            double cos_val = v1.dot(v2) / (len1 * len2);
            if (cos_val > max_cos) {
                max_cos  = cos_val;
                best_idx = (int) i;
            }
        }
        return best_idx;
    };

    int start_idx = is_classic ? 1 : find_sharpest_corner(loops.front());

    for (size_t i = 0; i < loops.size(); ++i) {
        const Polygon& loop = loops[i];

        int idx;
        if (spiral.empty()) {
            idx = start_idx;
        } else {
            idx = current_pos.nearest_point_index(loop.points);
        }

        Polyline loop_path(loop.split_at_index(idx));
        loop_path.points = loop_points_opened(std::move(loop_path));
        if (loop_path.size() < 2) continue;

        // Island detection:
        if (!spiral.empty()) {
            double dist_to_new_start = spiral.last_point().distance_to(loop_path.points.front());
            if (dist_to_new_start > jump_threshold) {
                if (!spiral.empty())
                    output.emplace_back(std::move(spiral));

                spiral.clear();
                current_pos = Point(0, 0);
                idx = is_classic ? 1 : find_sharpest_corner(loop);
                loop_path = Polyline(loop.split_at_index(idx));
                loop_path.points = loop_points_opened(std::move(loop_path));
                if (loop_path.size() < 2) continue;
            }
        }

        // Clip the last segment of the loop to avoid overlapping with the next loop. The last loop is clipped by half the distance.
        const bool last_loop = (i + 1 == loops.size());
        double gap           = last_loop ? 0.5 * double(distance) : double(distance);
        loop_path.points.push_back(loop_path.points.front());
        const Point& p_prev = loop_path.points[loop_path.points.size() - 2];
        const Point& p_last = loop_path.points.back();
        const Point& p_next = loop_path.points[1];
        Vec2d v1            = (p_last - p_prev).cast<double>();
        Vec2d v2            = (p_next - p_last).cast<double>();
        double len1         = v1.norm();
        double len2         = v2.norm();
        double clip_len     = gap;
        if (len1 > 1e-6 && len2 > 1e-6) {
            double dot   = v1.dot(v2);
            double cross = v1.x() * v2.y() - v1.y() * v2.x();
            double alpha = std::atan2(std::abs(cross), dot);

            if (alpha > M_PI / 8 && alpha < M_PI / 3) {
                clip_len = gap / std::sin(alpha);
            } else {
                clip_len = gap;
            }
        }

        loop_path.clip_end(clip_len);

        if (spiral.empty()) {
            spiral = std::move(loop_path);
        } else {
            spiral.append(std::move(loop_path));
        }
        current_pos = spiral.last_point();
    }

    if (!spiral.empty()) {
        if (params.fill_order == SurfaceFillOrder::Outward)
            std::reverse(spiral.begin(), spiral.end());
        output.emplace_back(std::move(spiral));
    }
    
    return output;
}


void FillConcentric::_fill_surface_single(
    const FillParams                &params, 
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction, 
    ExPolygon                        expolygon,
    Polylines                       &polylines_out)
{
    // no rotation is supported for this infill pattern
    BoundingBox bounding_box = expolygon.contour.bounding_box();
    
    coord_t min_spacing = scale_(this->spacing) * params.multiline;
    coord_t distance = coord_t(min_spacing / params.density);
    
    if (params.density > 0.9999f && !params.dont_adjust) {
        distance = this->_adjust_solid_spacing(bounding_box.size()(0), distance);
        this->spacing = unscale<double>(distance);
    }

    // Contract surface polygon by half line width to avoid excesive overlap with perimeter
    ExPolygons contracted = offset_ex(expolygon, -float(scale_(0.5 * (params.multiline - 1) * this->spacing )));

    Polygons loops = to_polygons(contracted);

    ExPolygons last { std::move(contracted) };
    while (! last.empty()) {
        last = offset2_ex(last, -(distance + min_spacing/2), +min_spacing/2);
        append(loops, to_polygons(last));
    }

    // generate paths from the outermost to the innermost, to avoid
    // adhesion problems of the first central tiny loops
    loops = union_pt_chained_outside_in(loops);

    const bool spiralized = should_spiralize_concentric(params);
    const bool is_classic = this->print_object_config == nullptr ||
                            this->print_object_config->wall_generator.value == PerimeterGeneratorType::Classic;

    if (spiralized) {
        Polylines spiral_result = generate_spiralized_concentric_polylines(params, loops, distance, is_classic);
        append(polylines_out, spiral_result);
    } else {
        // Orca: an outward fill order prints the innermost loops first instead.
        if (params.fill_order == SurfaceFillOrder::Outward)
            std::reverse(loops.begin(), loops.end());

        Point last_pos(0, 0);
        for (const Polygon& loop : loops) {
            polylines_out.emplace_back(loop.split_at_index(last_pos.nearest_point_index(loop.points)));
            last_pos = polylines_out.back().last_point();
        }
    }

    // split paths using a nearest neighbor search
    size_t iPathFirst = polylines_out.size();

    // Apply multiline offset if needed
    multiline_fill(polylines_out, params, spacing);

    // clip the paths to prevent the extruder from getting exactly on the first point of the loop
    // Keep valid paths only.
    size_t j = iPathFirst;
    for (size_t i = iPathFirst; i < polylines_out.size(); ++ i) {
        polylines_out[i].clip_end(this->loop_clipping);
        if (polylines_out[i].is_valid()) {
            if (j < i)
                polylines_out[j] = std::move(polylines_out[i]);
            ++ j;
        }
    }
    if (j < polylines_out.size())
        polylines_out.erase(polylines_out.begin() + j, polylines_out.end());
    //TODO: return ExtrusionLoop objects to get better chained paths,
    // otherwise the outermost loop starts at the closest point to (0, 0).
    // We want the loops to be split inside the G-code generator to get optimum path planning.
}

void FillConcentric::_fill_surface_single(const FillParams& params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point>& direction,
    ExPolygon                      expolygon,
    ThickPolylines& thick_polylines_out)
{
    assert(params.use_arachne);
    assert(this->print_config != nullptr && this->print_object_config != nullptr);

    // no rotation is supported for this infill pattern
    Point   bbox_size = expolygon.contour.bounding_box().size();
    coord_t min_spacing = scaled<coord_t>(this->spacing);

    if (should_spiralize_concentric(params)) {
        Polylines polylines;
        this->_fill_surface_single(params, thickness_layers, direction, std::move(expolygon), polylines);
        append(thick_polylines_out, to_thick_polylines(std::move(polylines), min_spacing));
        return;
    }

    if (params.density > 0.9999f && !params.dont_adjust) {
        coord_t                loops_count = std::max(bbox_size.x(), bbox_size.y()) / min_spacing + 1;
        Polygons               polygons = offset(expolygon, float(min_spacing) / 2.f);

        double min_nozzle_diameter = *std::min_element(print_config->nozzle_diameter.values.begin(), print_config->nozzle_diameter.values.end());
        Arachne::WallToolPathsParams input_params;
        input_params.min_bead_width = 0.85 * min_nozzle_diameter;
        input_params.min_feature_size = 0.25 * min_nozzle_diameter;
        input_params.wall_transition_length = 1.0 * min_nozzle_diameter;
        input_params.wall_transition_angle = 10;
        input_params.wall_transition_filter_deviation = 0.25 * min_nozzle_diameter;
        input_params.wall_distribution_count = 1;

        Arachne::WallToolPaths wallToolPaths(polygons, min_spacing, min_spacing, loops_count, 0, params.layer_height, input_params);

        std::vector<Arachne::VariableWidthLines>    loops = wallToolPaths.getToolPaths();
        std::vector<const Arachne::ExtrusionLine*> all_extrusions;
        for (Arachne::VariableWidthLines& loop : loops) {
            if (loop.empty())
                continue;
            for (const Arachne::ExtrusionLine& wall : loop)
                all_extrusions.emplace_back(&wall);
        }

        // Orca: a forced fill order prints the loops in strictly monotonic depth order so
        // that surfaces broken up by holes or slots cannot hop outward and back inward.
        const bool forced_fill_order = params.fill_order != SurfaceFillOrder::Default;
        if (forced_fill_order) {
            const bool outward = params.fill_order == SurfaceFillOrder::Outward;
            std::stable_sort(all_extrusions.begin(), all_extrusions.end(),
                             [outward](const Arachne::ExtrusionLine *a, const Arachne::ExtrusionLine *b) {
                                 return outward ? a->inset_idx > b->inset_idx : a->inset_idx < b->inset_idx;
                             });
        }

        // Split paths using a nearest neighbor search.
        size_t firts_poly_idx = thick_polylines_out.size();
        Point  last_pos(0, 0);
        for (const Arachne::ExtrusionLine* extrusion : all_extrusions) {
            if (extrusion->empty())
                continue;

            ThickPolyline thick_polyline = Arachne::to_thick_polyline(*extrusion);
            if (extrusion->is_closed)
                thick_polyline.start_at_index(last_pos.nearest_point_index(thick_polyline.points));
            thick_polylines_out.emplace_back(std::move(thick_polyline));
            last_pos = thick_polylines_out.back().last_point();
        }

        // clip the paths to prevent the extruder from getting exactly on the first point of the loop
        // Keep valid paths only.
        size_t j = firts_poly_idx;
        for (size_t i = firts_poly_idx; i < thick_polylines_out.size(); ++i) {
            thick_polylines_out[i].clip_end(this->loop_clipping);
            if (thick_polylines_out[i].is_valid()) {
                if (j < i)
                    thick_polylines_out[j] = std::move(thick_polylines_out[i]);
                ++j;
            }
        }
        if (j < thick_polylines_out.size())
            thick_polylines_out.erase(thick_polylines_out.begin() + int(j), thick_polylines_out.end());

        if (!forced_fill_order)
            reorder_by_shortest_traverse(thick_polylines_out);
    }
    else {
        Polylines polylines;
        this->_fill_surface_single(params, thickness_layers, direction, expolygon, polylines);
        append(thick_polylines_out, to_thick_polylines(std::move(polylines), min_spacing));
    }
}

} // namespace Slic3r
