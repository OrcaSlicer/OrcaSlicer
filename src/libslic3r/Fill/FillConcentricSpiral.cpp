#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Surface.hpp"

#include "FillConcentricSpiral.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {

static Points loop_points_opened(Polyline loop_path)
{
    if (loop_path.points.size() > 1 && loop_path.points.front() == loop_path.points.back())
        loop_path.points.pop_back();
    return std::move(loop_path.points);
}

static Polylines generate_concentric_spiral_polylines(const FillParams& params,
                                                      const Polygons& loops,
                                                      const coord_t distance,
                                                      const bool is_classic)
{
    Polylines output;
    Polyline spiral;
    Point current_pos(0, 0);
    const double jump_threshold = 2.0 * double(distance);//Distance to jump to a new spiral.

    // Pick the sharpest corner of the first loop (only for arachne, for classic we always start at the second point of the loop).
    auto find_sharpest_corner = [](const Polygon& loop) -> int {
        size_t n = loop.points.size();
        if (n < 3)
            return 0;

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
            if (len1 < 1e-6 || len2 < 1e-6)
                continue;

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
        if (spiral.empty())
            idx = start_idx;
        else
            idx = current_pos.nearest_point_index(loop.points);

        Polyline loop_path(loop.split_at_index(idx));
        loop_path.points = loop_points_opened(std::move(loop_path));
        if (loop_path.size() < 2)
            continue;

        // Island jumping: if the distance between the last point of the current spiral and the first point of the new loop is too large, we
        // start a new spiral.
        if (!spiral.empty()) {
            double dist_to_new_start = spiral.last_point().distance_to(loop_path.points.front());
            if (dist_to_new_start > jump_threshold) {
                if (!spiral.empty())
                    output.emplace_back(std::move(spiral));

                spiral.clear();
                current_pos      = Point(0, 0);
                idx              = is_classic ? 1 : find_sharpest_corner(loop);
                loop_path        = Polyline(loop.split_at_index(idx));
                loop_path.points = loop_points_opened(std::move(loop_path));
                if (loop_path.size() < 2)
                    continue;
            }
        }

        //clipping the end of the loop to avoid overlapping with the next loop.
        // theoric gap = distance/sin(alpha) where alpha is the angle between the last segment of the loop and the first segment of the next loop.
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

            if (alpha > M_PI / 8 && alpha < M_PI / 3)
                clip_len = gap / std::sin(alpha);
            else
                clip_len = gap;
        }

        loop_path.clip_end(clip_len);

        if (spiral.empty())
            spiral = std::move(loop_path);
        else
            spiral.append(std::move(loop_path));

        current_pos = spiral.last_point();
    }

    //Fill order handling.
    if (!spiral.empty())
        output.emplace_back(std::move(spiral));

    if (params.fill_order == SurfaceFillOrder::Outward) {
        for (Polyline& path : output)
            std::reverse(path.begin(), path.end());
        std::reverse(output.begin(), output.end());
    }

    return output;
}

void FillConcentricSpiral::_fill_surface_single(const FillParams& params,
                                                unsigned int thickness_layers,
                                                const std::pair<float, Point>& direction,
                                                ExPolygon expolygon,
                                                Polylines& polylines_out)
{
    BoundingBox bounding_box = expolygon.contour.bounding_box();

    coord_t min_spacing = scale_(this->spacing) * params.multiline;
    coord_t distance    = coord_t(min_spacing / params.density);

    if (params.density > 0.9999f && !params.dont_adjust) {
        distance      = this->_adjust_solid_spacing(bounding_box.size()(0), distance);
        this->spacing = unscale<double>(distance);
    }

    ExPolygons contracted = offset_ex(expolygon, -float(scale_(0.5 * (params.multiline - 1) * this->spacing)));

    Polygons loops = to_polygons(contracted);

    ExPolygons last{std::move(contracted)};
    while (!last.empty()) {
        last = offset2_ex(last, -(distance + min_spacing / 2), +min_spacing / 2);
        append(loops, to_polygons(last));
    }

    loops = union_pt_chained_outside_in(loops);

    const bool is_classic   = this->print_object_config == nullptr ||
                              this->print_object_config->wall_generator.value == PerimeterGeneratorType::Classic;
    Polylines spiral_result = generate_concentric_spiral_polylines(params, loops, distance, is_classic);
    append(polylines_out, spiral_result);

    // Apply multiline offset if needed.
    multiline_fill(polylines_out, params, spacing);
}

} // namespace Slic3r
