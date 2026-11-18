// Copyright (c) 2022 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#include <algorithm> //For std::partition_copy and std::min_element.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <cassert>
#include <cmath>
#include <math.h>
#include <limits>
#include <unordered_set>

#include "WallToolPaths.hpp"

#include "../AABBTreeLines.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Arachne/utils/PolygonsPointIndex.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Arachne/BeadingStrategy/BeadingStrategyFactory.hpp"
#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/SparsePointGrid.hpp"
#include "libslic3r/Arachne/utils/SquareGrid.hpp"
#include "SkeletalTrapezoidation.hpp"
#include "../ClipperUtils.hpp"
#include "libslic3r/libslic3r.h"
#include "utils/linearAlg2D.hpp"
#include "EdgeGrid.hpp"
#include "utils/SparseLineGrid.hpp"
#include "Geometry.hpp"
#include "utils/PolylineStitcher.hpp"
#include "SVG.hpp"
#include "Utils.hpp"

#include <boost/log/trivial.hpp>
#include <utility>
#include <vector>

//#define ARACHNE_STITCH_PATCH_DEBUG

namespace Slic3r::Arachne
{

WallToolPathsParams make_paths_params(const int layer_id, const PrintObjectConfig &print_object_config, const PrintConfig &print_config)
{
    WallToolPathsParams input_params;
    {
        const double min_nozzle_diameter = *std::min_element(print_config.nozzle_diameter.values.begin(), print_config.nozzle_diameter.values.end());
        if (const auto &min_feature_size_opt = print_object_config.min_feature_size)
            input_params.min_feature_size = min_feature_size_opt.value * 0.01 * min_nozzle_diameter;

        if (const auto &min_wall_length_factor_opt = print_object_config.min_length_factor)
            input_params.min_length_factor = min_wall_length_factor_opt.value;
        else
            input_params.min_length_factor = 0.5f;

        if (layer_id == 0) {
            if (const auto &initial_layer_min_bead_width_opt = print_object_config.initial_layer_min_bead_width)
                input_params.min_bead_width = initial_layer_min_bead_width_opt.value * 0.01 * min_nozzle_diameter;
        } else {
            if (const auto &min_bead_width_opt = print_object_config.min_bead_width)
                input_params.min_bead_width = min_bead_width_opt.value * 0.01 * min_nozzle_diameter;
        }

        if (const auto &wall_transition_filter_deviation_opt = print_object_config.wall_transition_filter_deviation)
            input_params.wall_transition_filter_deviation = wall_transition_filter_deviation_opt.value * 0.01 * min_nozzle_diameter;

        if (const auto &wall_transition_length_opt = print_object_config.wall_transition_length)
            input_params.wall_transition_length = wall_transition_length_opt.value * 0.01 * min_nozzle_diameter;

        input_params.wall_transition_angle   = print_object_config.wall_transition_angle.value;
        input_params.wall_distribution_count = print_object_config.wall_distribution_count.value;

        input_params.is_top_or_bottom_layer = false; // Set to default value

        if (const auto& wall_maximum_resolution_opt = print_object_config.wall_maximum_resolution)
            input_params.wall_maximum_resolution = scaled<coord_t>(wall_maximum_resolution_opt.value);

        if (const auto& wall_maximum_deviation_opt = print_object_config.wall_maximum_deviation)
            input_params.wall_maximum_deviation = scaled<coord_t>(wall_maximum_deviation_opt.value);
    }

    return input_params;
}

WallToolPaths::WallToolPaths(const Polygons& outline, const coord_t bead_width_0, const coord_t bead_width_x,
                             const size_t inset_count, const coord_t wall_0_inset, const coordf_t layer_height, const WallToolPathsParams &params)
    : outline(outline)
    , bead_width_0(bead_width_0)
    , bead_width_x(bead_width_x)
    , inset_count(inset_count)
    , wall_0_inset(wall_0_inset)
    , layer_height(layer_height)
    , print_thin_walls(Slic3r::Arachne::fill_outline_gaps)
    , min_feature_size(scaled<coord_t>(params.min_feature_size))
    , min_bead_width(scaled<coord_t>(params.min_bead_width))
    , small_area_length(static_cast<double>(bead_width_0) / 2.)
    , wall_transition_filter_deviation(scaled<coord_t>(params.wall_transition_filter_deviation))
    , toolpaths_generated(false)
    , m_params(params)
{
}

void simplify(Polygon &thiss, const int64_t smallest_line_segment_squared, const int64_t allowed_error_distance_squared)
{
    if (thiss.size() < 3) {
        thiss.points.clear();
        return;
    }
    if (thiss.size() == 3)
        return;

    Polygon new_path;
    Point previous = thiss.points.back();
    Point previous_previous = thiss.points.at(thiss.points.size() - 2);
    Point current = thiss.points.at(0);

    /* When removing a vertex, we check the height of the triangle of the area
     being removed from the original polygon by the simplification. However,
     when consecutively removing multiple vertices the height of the previously
     removed vertices w.r.t. the shortcut path changes.
     In order to not recompute the new height value of previously removed
     vertices we compute the height of a representative triangle, which covers
     the same amount of area as the area being cut off. We use the Shoelace
     formula to accumulate the area under the removed segments. This works by
     computing the area in a 'fan' where each of the blades of the fan go from
     the origin to one of the segments. While removing vertices the area in
     this fan accumulates. By subtracting the area of the blade connected to
     the short-cutting segment we obtain the total area of the cutoff region.
     From this area we compute the height of the representative triangle using
     the standard formula for a triangle area: A = .5*b*h
     */
    int64_t accumulated_area_removed = int64_t(previous.x()) * int64_t(current.y()) - int64_t(previous.y()) * int64_t(current.x()); // Twice the Shoelace formula for area of polygon per line segment.

    for (size_t point_idx = 0; point_idx < thiss.points.size(); point_idx++) {
        current = thiss.points.at(point_idx % thiss.points.size());

        //Check if the accumulated area doesn't exceed the maximum.
        Point next;
        if (point_idx + 1 < thiss.points.size()) {
            next = thiss.points.at(point_idx + 1);
        } else if (point_idx + 1 == thiss.points.size() && new_path.size() > 1) { // don't spill over if the [next] vertex will then be equal to [previous]
            next = new_path[0]; //Spill over to new polygon for checking removed area.
        } else {
            next = thiss.points.at((point_idx + 1) % thiss.points.size());
        }
        const int64_t removed_area_next = int64_t(current.x()) * int64_t(next.y()) - int64_t(current.y()) * int64_t(next.x()); // Twice the Shoelace formula for area of polygon per line segment.
        const int64_t negative_area_closing = int64_t(next.x()) * int64_t(previous.y()) - int64_t(next.y()) * int64_t(previous.x()); // area between the origin and the short-cutting segment
        accumulated_area_removed += removed_area_next;

        const int64_t length2 = (current - previous).cast<int64_t>().squaredNorm();

        // Orca:
        // Checking if the segment's length is smaller than 5 microns (0.005mm).
        // The value of `length2` is scaled and squared, so we need to compare it with the squared value of 5 microns
        if (length2 < Slic3r::sqr(scaled<coord_t>(0.005))) {
            // We're allowed to always delete segments of less than 5 micron.
            continue;
        }

        const int64_t area_removed_so_far = accumulated_area_removed + negative_area_closing; // close the shortcut area polygon
        const int64_t base_length_2 = (next - previous).cast<int64_t>().squaredNorm();

        if (base_length_2 == 0) //Two line segments form a line back and forth with no area.
            continue; //Remove the vertex.
        //We want to check if the height of the triangle formed by previous, current and next vertices is less than allowed_error_distance_squared.
        //1/2 L = A           [actual area is half of the computed shoelace value] // Shoelace formula is .5*(...) , but we simplify the computation and take out the .5
        //A = 1/2 * b * h     [triangle area formula]
        //L = b * h           [apply above two and take out the 1/2]
        //h = L / b           [divide by b]
        //h^2 = (L / b)^2     [square it]
        //h^2 = L^2 / b^2     [factor the divisor]
        const int64_t height_2 = double(area_removed_so_far) * double(area_removed_so_far) / double(base_length_2);
        // Orca: The value of `height_2` is squared, so we need to compare it with the squared value
        if ((height_2 <= Slic3r::sqr(colinear_vertex_tolerance()) //Almost exactly colinear (barring rounding errors).
             && Line::distance_to_infinite(current, previous, next) <= double(colinear_vertex_tolerance()))) // make sure that height_2 is not small because of cancellation of positive and negative areas
            continue;

        if (length2 < smallest_line_segment_squared
            && height_2 <= allowed_error_distance_squared) // removing the vertex doesn't introduce too much error.)
        {
            const int64_t next_length2 = (current - next).cast<int64_t>().squaredNorm();
            if (next_length2 > 4 * smallest_line_segment_squared) {
                // Special case; The next line is long. If we were to remove this, it could happen that we get quite noticeable artifacts.
                // We should instead move this point to a location where both edges are kept and then remove the previous point that we wanted to keep.
                // By taking the intersection of these two lines, we get a point that preserves the direction (so it makes the corner a bit more pointy).
                // We just need to be sure that the intersection point does not introduce an artifact itself.
                Point intersection_point;
                bool has_intersection = Line(previous_previous, previous).intersection_infinite(Line(current, next), &intersection_point);
                if (!has_intersection
                    || Line::distance_to_infinite_squared(intersection_point, previous, current) > double(allowed_error_distance_squared)
                    || (intersection_point - previous).cast<int64_t>().squaredNorm() > smallest_line_segment_squared  // The intersection point is way too far from the 'previous'
                    || (intersection_point - next).cast<int64_t>().squaredNorm() > smallest_line_segment_squared)     // and 'next' points, so it shouldn't replace 'current'
                {
                    // We can't find a better spot for it, but the size of the line is more than 5 micron.
                    // So the only thing we can do here is leave it in...
                }
                else {
                    // New point seems like a valid one.
                    current = intersection_point;
                    // If there was a previous point added, remove it.
                    if(!new_path.empty()) {
                        new_path.points.pop_back();
                        previous = previous_previous;
                    }
                }
            } else {
                continue; //Remove the vertex.
            }
        }
        //Don't remove the vertex.
        accumulated_area_removed = removed_area_next; // so that in the next iteration it's the area between the origin, [previous] and [current]
        previous_previous = previous;
        previous = current; //Note that "previous" is only updated if we don't remove the vertex.
        new_path.points.push_back(current);
    }

    thiss = new_path;
}

/*!
     * Removes vertices of the polygons to make sure that they are not too high
     * resolution.
     *
     * This removes points which are connected to line segments that are shorter
     * than the `smallest_line_segment`, unless that would introduce a deviation
     * in the contour of more than `allowed_error_distance`.
     *
     * Criteria:
     * 1. Never remove a vertex if either of the connceted segments is larger than \p smallest_line_segment
     * 2. Never remove a vertex if the distance between that vertex and the final resulting polygon would be higher than \p allowed_error_distance
     * 3. The direction of segments longer than \p smallest_line_segment always
     * remains unaltered (but their end points may change if it is connected to
     * a small segment)
     *
     * Simplify uses a heuristic and doesn't neccesarily remove all removable
     * vertices under the above criteria, but simplify may never violate these
     * criteria. Unless the segments or the distance is smaller than the
     * rounding error of 5 micron.
     *
     * Vertices which introduce an error of less than 5 microns are removed
     * anyway, even if the segments are longer than the smallest line segment.
     * This makes sure that (practically) colinear line segments are joined into
     * a single line segment.
     * \param smallest_line_segment Maximal length of removed line segments.
     * \param allowed_error_distance If removing a vertex introduces a deviation
     * from the original path that is more than this distance, the vertex may
     * not be removed.
 */
void simplify(Polygons &thiss, const int64_t smallest_line_segment = scaled<coord_t>(0.01), const int64_t allowed_error_distance = scaled<coord_t>(0.005))
{
    const int64_t allowed_error_distance_squared = int64_t(allowed_error_distance) * int64_t(allowed_error_distance);
    const int64_t smallest_line_segment_squared = int64_t(smallest_line_segment) * int64_t(smallest_line_segment);
    for (size_t p = 0; p < thiss.size(); p++)
    {
        simplify(thiss[p], smallest_line_segment_squared, allowed_error_distance_squared);
        if (thiss[p].size() < 3)
        {
            thiss.erase(thiss.begin() + p);
            p--;
        }
    }
}

typedef SparseLineGrid<PolygonsPointIndex, PolygonsPointIndexSegmentLocator> LocToLineGrid;
std::unique_ptr<LocToLineGrid>                                               createLocToLineGrid(const Polygons &polygons, int square_size)
{
    unsigned int n_points = 0;
    for (const auto &poly : polygons)
        n_points += poly.size();

    auto ret = std::make_unique<LocToLineGrid>(square_size, n_points);

    for (unsigned int poly_idx = 0; poly_idx < polygons.size(); poly_idx++)
        for (unsigned int point_idx = 0; point_idx < polygons[poly_idx].size(); point_idx++)
            ret->insert(PolygonsPointIndex(&polygons, poly_idx, point_idx));
    return ret;
}

/* Note: Also tries to solve for near-self intersections, when epsilon >= 1
 */
void fixSelfIntersections(const coord_t epsilon, Polygons &thiss)
{
    if (epsilon < 1)
        return;

    const int64_t half_epsilon = (epsilon + 1) / 2;

    // Points too close to line segments should be moved a little away from those line segments, but less than epsilon,
    //   so at least half-epsilon distance between points can still be guaranteed.
    const coord_t grid_size  = scaled<coord_t>(2.);
    auto              query_grid = createLocToLineGrid(thiss, grid_size);

    const auto    move_dist         = std::max<int64_t>(2L, half_epsilon - 2);
    const int64_t half_epsilon_sqrd = half_epsilon * half_epsilon;

    const size_t n = thiss.size();
    for (size_t poly_idx = 0; poly_idx < n; poly_idx++) {
        const size_t pathlen = thiss[poly_idx].size();
        for (size_t point_idx = 0; point_idx < pathlen; ++point_idx) {
            Point &pt = thiss[poly_idx][point_idx];
            for (const auto &line : query_grid->getNearby(pt, epsilon)) {
                const size_t line_next_idx = (line.point_idx + 1) % thiss[line.poly_idx].size();
                if (poly_idx == line.poly_idx && (point_idx == line.point_idx || point_idx == line_next_idx))
                    continue;

                const Line segment(thiss[line.poly_idx][line.point_idx], thiss[line.poly_idx][line_next_idx]);
                Point      segment_closest_point;
                segment.distance_to_squared(pt, &segment_closest_point);

                if (half_epsilon_sqrd >= (pt - segment_closest_point).cast<int64_t>().squaredNorm()) {
                    const Point  &other = thiss[poly_idx][(point_idx + 1) % pathlen];
                    const Vec2i64 vec   = (LinearAlg2D::pointIsLeftOfLine(other, segment.a, segment.b) > 0 ? segment.b - segment.a : segment.a - segment.b).cast<int64_t>();
                    assert(Slic3r::sqr(double(vec.x())) < double(std::numeric_limits<int64_t>::max()));
                    assert(Slic3r::sqr(double(vec.y())) < double(std::numeric_limits<int64_t>::max()));
                    const int64_t len   = vec.norm();
                    pt.x() += (-vec.y() * move_dist) / len;
                    pt.y() += (vec.x() * move_dist) / len;
                }
            }
        }
    }
}

/*!
     * Removes overlapping consecutive line segments which don't delimit a positive area.
 */
void removeDegenerateVerts(Polygons &thiss)
{
    for (size_t poly_idx = 0; poly_idx < thiss.size(); poly_idx++) {
        Polygon &poly = thiss[poly_idx];
        Polygon  result;

        auto isDegenerate = [](const Point &last, const Point &now, const Point &next) {
            Vec2i64 last_line = (now - last).cast<int64_t>();
            Vec2i64 next_line = (next - now).cast<int64_t>();
            return last_line.dot(next_line) == -1 * last_line.norm() * next_line.norm();
        };
        bool isChanged = false;
        for (size_t idx = 0; idx < poly.size(); idx++) {
            const Point &last = (result.size() == 0) ? poly.back() : result.back();
            if (idx + 1 == poly.size() && result.size() == 0)
                break;

            const Point &next = (idx + 1 == poly.size()) ? result[0] : poly[idx + 1];
            if (isDegenerate(last, poly[idx], next)) { // lines are in the opposite direction
                // don't add vert to the result
                isChanged = true;
                while (result.size() > 1 && isDegenerate(result[result.size() - 2], result.back(), next))
                    result.points.pop_back();
            } else {
                result.points.emplace_back(poly[idx]);
            }
        }

        if (isChanged) {
            if (result.size() > 2) {
                poly = result;
            } else {
                thiss.erase(thiss.begin() + poly_idx);
                poly_idx--; // effectively the next iteration has the same poly_idx (referring to a new poly which is not yet processed)
            }
        }
    }
}

void removeSmallAreas(Polygons &thiss, const double min_area_size, const bool remove_holes)
{
    auto new_end = thiss.end();
    if (remove_holes) {
        for (auto it = thiss.begin(); it < new_end;) {
            // All polygons smaller than target are removed by replacing them with a polygon from the back of the vector.
            if (fabs(it->area()) < min_area_size) {
                --new_end;
                *it = std::move(*new_end);
                continue; // Don't increment the iterator such that the polygon just swapped in is checked next.
            }
            ++it;
        }
    } else {
        // For each polygon, computes the signed area, move small outlines at the end of the vector and keep pointer on small holes
        Polygons small_holes;
        for (auto it = thiss.begin(); it < new_end;) {
            if (double area = it->area(); fabs(area) < min_area_size) {
                if (area >= 0) {
                    --new_end;
                    if (it < new_end) {
                        std::swap(*new_end, *it);
                        continue;
                    } else { // Don't self-swap the last Path
                        break;
                    }
                } else {
                    small_holes.push_back(*it);
                }
            }
            ++it;
        }

        // Removes small holes that have their first point inside one of the removed outlines
        // Iterating in reverse ensures that unprocessed small holes won't be moved
        const auto removed_outlines_start = new_end;
        for (auto hole_it = small_holes.rbegin(); hole_it < small_holes.rend(); hole_it++)
            for (auto outline_it = removed_outlines_start; outline_it < thiss.end(); outline_it++)
                if (Polygon(*outline_it).contains(*hole_it->begin())) {
                    new_end--;
                    *hole_it = std::move(*new_end);
                    break;
                }
    }
    thiss.resize(new_end-thiss.begin());
}

void removeColinearEdges(Polygon &poly, const double max_deviation_angle)
{
    // TODO: Can be made more efficient (for example, use pointer-types for process-/skip-indices, so we can swap them without copy).
    size_t num_removed_in_iteration = 0;
    do {
        num_removed_in_iteration = 0;
        std::vector<bool> process_indices(poly.points.size(), true);

        bool go = true;
        while (go) {
            go = false;

            const auto  &rpath   = poly;
            const size_t pathlen = rpath.size();
            if (pathlen <= 3)
                return;

            std::vector<bool> skip_indices(poly.points.size(), false);

            Polygon new_path;
            for (size_t point_idx = 0; point_idx < pathlen; ++point_idx) {
                // Don't iterate directly over process-indices, but do it this way, because there are points _in_ process-indices that should nonetheless
                // be skipped:
                if (!process_indices[point_idx]) {
                    new_path.points.push_back(rpath[point_idx]);
                    continue;
                }

                // Should skip the last point for this iteration if the old first was removed (which can be seen from the fact that the new first was skipped):
                if (point_idx == (pathlen - 1) && skip_indices[0]) {
                    skip_indices[new_path.size()] = true;
                    go                            = true;
                    new_path.points.push_back(rpath[point_idx]);
                    break;
                }

                const Point &prev = rpath[(point_idx - 1 + pathlen) % pathlen];
                const Point &pt   = rpath[point_idx];
                const Point &next = rpath[(point_idx + 1) % pathlen];

                float angle = LinearAlg2D::getAngleLeft(prev, pt, next); // [0 : 2 * pi]
                if (angle >= float(M_PI)) { angle -= float(M_PI); }                    // map [pi : 2 * pi] to [0 : pi]

                // Check if the angle is within limits for the point to 'make sense', given the maximum deviation.
                // If the angle indicates near-parallel segments ignore the point 'pt'
                if (angle > max_deviation_angle && angle < M_PI - max_deviation_angle) {
                    new_path.points.push_back(pt);
                } else if (point_idx != (pathlen - 1)) {
                    // Skip the next point, since the current one was removed:
                    skip_indices[new_path.size()] = true;
                    go                            = true;
                    new_path.points.push_back(next);
                    ++point_idx;
                }
            }
            poly = new_path;
            num_removed_in_iteration += pathlen - poly.points.size();

            process_indices.clear();
            process_indices.insert(process_indices.end(), skip_indices.begin(), skip_indices.end());
        }
    } while (num_removed_in_iteration > 0);
}

void removeColinearEdges(Polygons &thiss, const double max_deviation_angle = 0.0005)
{
    for (int p = 0; p < int(thiss.size()); p++) {
        removeColinearEdges(thiss[p], max_deviation_angle);
        if (thiss[p].size() < 3) {
            thiss.erase(thiss.begin() + p);
            p--;
        }
    }
}

const std::vector<VariableWidthLines> &WallToolPaths::generate()
{
    if (this->inset_count < 1)
        return toolpaths;

    const coord_t smallest_segment = m_params.wall_maximum_resolution;
    const coord_t allowed_distance = m_params.wall_maximum_deviation;
    const coord_t epsilon_offset = (allowed_distance / 2) - 1;
    const double  transitioning_angle = Geometry::deg2rad(m_params.wall_transition_angle);
    const coord_t discretization_step_size = scaled<coord_t>(0.8);

    // Simplify outline for boost::voronoi consumption. Absolutely no self intersections or near-self intersections allowed:
    // TODO: Open question: Does this indeed fix all (or all-but-one-in-a-million) cases for manifold but otherwise possibly complex polygons?
    Polygons prepared_outline = offset(offset(offset(outline, -epsilon_offset), epsilon_offset * 2), -epsilon_offset);
    simplify(prepared_outline, smallest_segment, allowed_distance);
    fixSelfIntersections(epsilon_offset, prepared_outline);
    removeDegenerateVerts(prepared_outline);
    removeColinearEdges(prepared_outline, 0.005);
    // Removing collinear edges may introduce self intersections, so we need to fix them again
    fixSelfIntersections(epsilon_offset, prepared_outline);
    removeDegenerateVerts(prepared_outline);
    removeSmallAreas(prepared_outline, small_area_length * small_area_length, false);

    // The functions above could produce intersecting polygons that could cause a crash inside Arachne.
    // Applying Clipper union should be enough to get rid of this issue.
    // Clipper union also fixed an issue in Arachne that in post-processing Voronoi diagram, some edges
    // didn't have twin edges. (a non-planar Voronoi diagram probably caused this).
    prepared_outline = union_(prepared_outline);

    if (area(prepared_outline) <= 0) {
        assert(toolpaths.empty());
        return toolpaths;
    }

    const float external_perimeter_extrusion_width = Flow::rounded_rectangle_extrusion_width_from_spacing(unscale<float>(bead_width_0), float(this->layer_height));
    const float perimeter_extrusion_width          = Flow::rounded_rectangle_extrusion_width_from_spacing(unscale<float>(bead_width_x), float(this->layer_height));

    const coord_t wall_transition_length = scaled<coord_t>(this->m_params.wall_transition_length);
	
	const double wall_split_middle_threshold = std::clamp(2. * unscaled<double>(this->min_bead_width) / external_perimeter_extrusion_width - 1., 0.01, 0.99); // For an uneven nr. of lines: When to split the middle wall into two.
    const double wall_add_middle_threshold   = std::clamp(unscaled<double>(this->min_bead_width) / perimeter_extrusion_width, 0.01, 0.99); // For an even nr. of lines: When to add a new middle in between the innermost two walls.
    
    const int wall_distribution_count = this->m_params.wall_distribution_count;
    const size_t max_bead_count = (inset_count < std::numeric_limits<coord_t>::max() / 2) ? 2 * inset_count : std::numeric_limits<coord_t>::max();
    const auto beading_strat = BeadingStrategyFactory::makeStrategy
        (
            bead_width_0,
            bead_width_x,
            wall_transition_length,
            transitioning_angle,
            print_thin_walls,
            min_bead_width,
            min_feature_size,
            wall_split_middle_threshold,
            wall_add_middle_threshold,
            max_bead_count,
            wall_0_inset,
            wall_distribution_count
        );
    const coord_t transition_filter_dist   = scaled<coord_t>(100.f);
    const coord_t allowed_filter_deviation = wall_transition_filter_deviation;
    SkeletalTrapezoidation wall_maker
    (
        prepared_outline,
        *beading_strat,
        beading_strat->getTransitioningAngle(),
        discretization_step_size,
        transition_filter_dist,
        allowed_filter_deviation,
        wall_transition_length
    );
    wall_maker.generateToolpaths(toolpaths);

    stitchToolPaths(toolpaths, this->bead_width_x);

    removeSmallLines(toolpaths);

    applyOverhangWallOverlap();

    separateOutInnerContour();

    simplifyToolPaths(toolpaths, m_params);

    removeEmptyToolPaths(toolpaths);
    assert(std::is_sorted(toolpaths.cbegin(), toolpaths.cend(),
                          [](const VariableWidthLines& l, const VariableWidthLines& r)
                          {
                              return l.front().inset_idx < r.front().inset_idx;
                          }) && "WallToolPaths should be sorted from the outer 0th to inner_walls");
    toolpaths_generated = true;
    return toolpaths;
}

namespace {

struct WallSupportSample {
    double distance;
    double unsupported;
};

struct WallOverlapPoint {
    double distance;
    double overlap;
};

using WallSupport = std::vector<WallSupportSample>;
using WallOverlapProfile = std::vector<WallOverlapPoint>;
using WallDistancer = AABBTreeLines::LinesDistancer<ThickLine>;

struct WallOverlapRegion {
    double begin, end;
    size_t first_target, last_target;
    WallOverlapPoint entry, exit;
};

// Support and overlap are separate scalar fields on the original wall's arc length.
template<class Sample>
double profile_value(const std::vector<Sample> &profile, double distance, double Sample::*value)
{
    auto right = std::upper_bound(profile.begin(), profile.end(), distance,
                                 [](double s, const Sample &point) { return s < point.distance; });
    if (right == profile.begin())
        return (*right).*value;
    if (right == profile.end())
        return profile.back().*value;
    const Sample &left = *(right - 1);
    const double t = (distance - left.distance) / (right->distance - left.distance);
    return left.*value + t * ((*right).*value - left.*value);
}

double overlap_factor(double unsupported, double start, double full)
{
    return full > start ? std::clamp((unsupported - start) / (full - start), 0., 1.) : double(unsupported > start);
}

WallSupport insert_support_thresholds(const WallSupport &support, double start, double full)
{
    std::vector<double> thresholds{0., start, full};
    std::sort(thresholds.begin(), thresholds.end());
    thresholds.erase(std::unique(thresholds.begin(), thresholds.end()), thresholds.end());
    WallSupport samples;
    samples.reserve(support.size());
    samples.push_back(support.front());
    for (size_t i = 1; i < support.size(); ++i) {
        const WallSupportSample &a = support[i - 1];
        const WallSupportSample &b = support[i];
        // Append crossings in path order, without a temporary array for each segment.
        for (size_t j = 0; j < thresholds.size(); ++j) {
            const double threshold = thresholds[b.unsupported > a.unsupported ? j : thresholds.size() - 1 - j];
            if ((a.unsupported < threshold && threshold < b.unsupported) ||
                (b.unsupported < threshold && threshold < a.unsupported)) {
                const double t = (threshold - a.unsupported) / (b.unsupported - a.unsupported);
                samples.push_back({a.distance + t * (b.distance - a.distance), threshold});
            }
        }
        samples.push_back(b);
    }
    return samples;
}

std::vector<WallOverlapRegion> find_overlap_regions(const WallSupport &samples, double start, double full, double max_length)
{
    std::vector<double> zero_positions;
    for (const WallSupportSample &sample : samples)
        if (sample.unsupported <= 0.)
            zero_positions.push_back(sample.distance);

    std::vector<WallOverlapRegion> regions;
    for (size_t i = 0; i < samples.size();) {
        if (samples[i].unsupported <= start) {
            ++i;
            continue;
        }
        const size_t first = i;
        double peak = samples[i].unsupported;
        while (i + 1 < samples.size() && samples[i + 1].unsupported > start)
            peak = std::max(peak, samples[++i].unsupported);
        const size_t last = i++;
        const size_t begin = first > 0 ? first - 1 : first;
        const size_t end = last + 1 < samples.size() ? last + 1 : last;
        const double target = std::min(full, peak);
        size_t first_target = begin, last_target = end;
        while (first_target < last && samples[first_target].unsupported < target)
            ++first_target;
        while (last_target > first && samples[last_target].unsupported < target)
            --last_target;
        // Open endpoints stop extension without forcing an active wall to zero overlap.
        if (first == 0)
            first_target = first;
        if (last + 1 == samples.size())
            last_target = last;

        double entry = std::max(samples.front().distance, samples[begin].distance - max_length);
        double exit = std::min(samples.back().distance, samples[end].distance + max_length);
        const auto previous_zero = std::upper_bound(zero_positions.begin(), zero_positions.end(), samples[begin].distance);
        if (previous_zero != zero_positions.begin())
            entry = std::max(entry, *(previous_zero - 1));
        const auto next_zero = std::lower_bound(zero_positions.begin(), zero_positions.end(), samples[end].distance);
        if (next_zero != zero_positions.end())
            exit = std::min(exit, *next_zero);
        regions.push_back({samples[begin].distance, samples[end].distance, first_target, last_target, {entry, 0.}, {exit, 0.}});
    }
    return regions;
}

double minimum_support_position(const WallSupport &samples, double begin, double end)
{
    WallSupport candidates{{begin, std::max(0., profile_value(samples, begin, &WallSupportSample::unsupported))}};
    auto it = std::upper_bound(samples.begin(), samples.end(), begin,
                               [](double s, const WallSupportSample &sample) { return s < sample.distance; });
    for (; it != samples.end() && it->distance < end; ++it)
        candidates.push_back({it->distance, std::max(0., it->unsupported)});
    candidates.push_back({end, std::max(0., profile_value(samples, end, &WallSupportSample::unsupported))});
    const double minimum = std::min_element(candidates.begin(), candidates.end(),
        [](const WallSupportSample &a, const WallSupportSample &b) { return a.unsupported < b.unsupported; })->unsupported;
    const double middle = 0.5 * (begin + end);
    double handoff = begin, best_distance = std::numeric_limits<double>::max();
    // For disjoint equal minima, choose the minimum plateau closest to the middle.
    constexpr double support_tolerance = 1e-9;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (std::abs(candidates[i].unsupported - minimum) > support_tolerance)
            continue;
        const double plateau_begin = candidates[i].distance;
        while (i + 1 < candidates.size() && std::abs(candidates[i + 1].unsupported - minimum) <= support_tolerance)
            ++i;
        const double midpoint = 0.5 * (plateau_begin + candidates[i].distance);
        if (std::abs(midpoint - middle) < best_distance) {
            handoff = midpoint;
            best_distance = std::abs(midpoint - middle);
        }
    }
    return handoff;
}

void merge_overlap_transitions(std::vector<WallOverlapRegion> &regions, const WallSupport &samples, double start, double full)
{
    for (size_t i = 1; i < regions.size(); ++i) {
        WallOverlapRegion &left = regions[i - 1];
        WallOverlapRegion &right = regions[i];
        const double begin = std::max(left.end, right.entry.distance);
        const double end = std::min(right.begin, left.exit.distance);
        if (begin > end)
            continue;

        const double handoff = minimum_support_position(samples, begin, end);
        const WallSupportSample &left_target = samples[left.last_target];
        const WallSupportSample &right_target = samples[right.first_target];
        const double left_height = full <= start ? 1. : overlap_factor(left_target.unsupported, start, full);
        const double right_height = full <= start ? 1. : overlap_factor(right_target.unsupported, start, full);
        const double left_value = left.exit.distance > left_target.distance ?
            left_height * std::clamp((left.exit.distance - handoff) / (left.exit.distance - left_target.distance), 0., 1.) : left_height;
        const double right_value = right_target.distance > right.entry.distance ?
            right_height * std::clamp((handoff - right.entry.distance) / (right_target.distance - right.entry.distance), 0., 1.) : right_height;
        left.exit = right.entry = {handoff, 0.5 * (left_value + right_value)};
    }
}

WallOverlapProfile make_overlap_profile(const WallSupport &support, double start, double full, double max_length)
{
    const WallSupport samples = insert_support_thresholds(support, start, full);
    std::vector<WallOverlapRegion> regions = find_overlap_regions(samples, start, full, max_length);
    merge_overlap_transitions(regions, samples, start, full);

    WallOverlapProfile profile;
    auto append = [&profile](WallOverlapPoint point) {
        if (!profile.empty() && point.distance == profile.back().distance)
            profile.back() = point;
        else
            profile.push_back(point);
    };
    append({samples.front().distance, overlap_factor(samples.front().unsupported, start, full)});
    for (const WallOverlapRegion &region : regions) {
        if (region.entry.distance < samples[region.first_target].distance)
            append(region.entry);
        for (size_t i = region.first_target; i <= region.last_target; ++i)
            append({samples[i].distance, full <= start ? 1. : overlap_factor(samples[i].unsupported, start, full)});
        if (region.exit.distance > samples[region.last_target].distance)
            append(region.exit);
    }
    append({samples.back().distance, overlap_factor(samples.back().unsupported, start, full)});
    return profile;
}

WallOverlapProfile wall_overlap_profile(const WallSupport &support, bool closed, double start, double full, double max_length)
{
    if (!closed)
        return make_overlap_profile(support, start, full, max_length);

    // A loop above the trigger everywhere has no entry/exit to extend.
    if (std::all_of(support.begin(), support.end(), [start](const WallSupportSample &sample) { return sample.unsupported > start; })) {
        WallOverlapProfile profile;
        profile.reserve(support.size());
        for (const WallSupportSample &sample : support)
            profile.push_back({sample.distance, overlap_factor(sample.unsupported, start, full)});
        return profile;
    }

    // Three periods let the same linear algorithm handle the seam. Keep the middle one.
    const double length = support.back().distance;
    WallSupport periodic;
    periodic.reserve(3 * support.size());
    for (size_t period = 0; period < 3; ++period)
        for (size_t i = 0; i + 1 < support.size(); ++i)
            periodic.push_back({support[i].distance + period * length, support[i].unsupported});
    periodic.push_back({3 * length, support.front().unsupported});
    const WallOverlapProfile expanded = make_overlap_profile(periodic, start, full, std::min(max_length, length));
    WallOverlapProfile profile{{0., profile_value(expanded, length, &WallOverlapPoint::overlap)}};
    for (const WallOverlapPoint &point : expanded)
        if (point.distance > length && point.distance < 2 * length)
            profile.push_back({point.distance - length, point.overlap});
    profile.push_back({length, profile.front().overlap});
    return profile;
}

ThickLines overlap_wall_segments(const VariableWidthLines &walls)
{
    ThickLines segments;
    for (const ExtrusionLine &wall : walls)
        if (!wall.is_odd && wall.junctions.size() >= 2)
            append(segments, to_thick_polyline(wall).thicklines());
    return segments;
}

ExtrusionJunctions sample_overlap_wall(const ExtrusionJunctions &junctions)
{
    ExtrusionJunctions sampled;
    sampled.reserve(junctions.size());
    for (size_t i = 1; i < junctions.size(); ++i) {
        const ExtrusionJunction &a = junctions[i - 1];
        const ExtrusionJunction &b = junctions[i];
        sampled.push_back(a);
        const size_t segments = size_t(std::ceil((b.p - a.p).cast<double>().norm() / scale_(0.2)));
        for (size_t j = 1; j < segments; ++j) {
            const double t = double(j) / segments;
            sampled.emplace_back((a.p.cast<double>() + t * (b.p - a.p).cast<double>()).cast<coord_t>(),
                                 coord_t(a.w + t * (b.w - a.w)), a.perimeter_index);
        }
    }
    sampled.push_back(junctions.back());
    return sampled;
}

WallSupport sample_wall_support(const ExtrusionJunctions &junctions, const WallDistancer &outer,
                               const AABBTreeLines::LinesDistancer<Line> &support, double reach)
{
    WallSupport samples;
    samples.reserve(junctions.size());
    double position = 0.;
    for (size_t i = 0; i < junctions.size(); ++i) {
        const Point &point = junctions[i].p;
        if (i > 0)
            position += (point - junctions[i - 1].p).cast<double>().norm();
        const auto [distance, source_idx, nearest] = outer.distance_from_lines_extra<false>(point);
        double unsupported = 0.;
        // Only paired walls within reach contribute support and overlap.
        if (distance > 0 && distance < reach) {
            const ThickLine &source = outer.get_line(source_idx);
            const Vec2d direction = (source.b - source.a).cast<double>();
            const double t = direction.squaredNorm() > 0 ?
                std::clamp((nearest - source.a.cast<double>()).dot(direction) / direction.squaredNorm(), 0., 1.) : 0.;
            const double width = source.a_width + t * (source.b_width - source.a_width);
            if (width > 0) {
                const double support_distance = support.distance_from_lines<true>(nearest.cast<coord_t>());
                // Retain negative values until the zero crossings have been interpolated.
                unsupported = std::min(1., 0.5 + support_distance / width);
            }
        }
        samples.push_back({position, unsupported});
    }
    return samples;
}

// Insert profile control points into the geometry and return one overlap factor per junction.
std::vector<double> apply_overlap_profile(ExtrusionJunctions &junctions, const WallSupport &support, const WallOverlapProfile &profile)
{
    ExtrusionJunctions sampled;
    sampled.reserve(junctions.size() + profile.size());
    std::vector<double> factors;
    factors.reserve(sampled.capacity());
    sampled.push_back(junctions.front());
    factors.push_back(profile_value(profile, 0., &WallOverlapPoint::overlap));
    size_t control = 0;
    for (size_t i = 1; i < junctions.size(); ++i) {
        const ExtrusionJunction &a = junctions[i - 1];
        const ExtrusionJunction &b = junctions[i];
        const double begin = support[i - 1].distance, end = support[i].distance;
        while (control < profile.size() && profile[control].distance < end) {
            const WallOverlapPoint &point = profile[control++];
            if (point.distance <= begin)
                continue;
            const double t = (point.distance - begin) / (end - begin);
            sampled.emplace_back(lerp(a.p, b.p, t), coord_t(a.w + t * (double(b.w) - a.w)), a.perimeter_index);
            factors.push_back(point.overlap);
        }
        sampled.push_back(b);
        factors.push_back(profile_value(profile, end, &WallOverlapPoint::overlap));
    }
    junctions.swap(sampled);
    return factors;
}

bool move_wall_towards_outer(ExtrusionLine &wall, const WallDistancer &original_outer, const WallDistancer &current_outer,
                            const std::vector<double> &overlap_factors, double extra_overlap, double cumulative_overlap, double reach)
{
    const ExtrusionJunctions unshifted = wall.junctions;
    const size_t vertex_count = wall.is_closed && unshifted.front().p == unshifted.back().p ? unshifted.size() - 1 : unshifted.size();
    bool moved = false;
    for (size_t point_idx = 0; point_idx < wall.junctions.size(); ++point_idx) {
        ExtrusionJunction &junction = wall.junctions[point_idx];
        const auto [distance, line_idx, nearest] = current_outer.distance_from_lines_extra<false>(junction.p);
        if (distance <= 0 || distance >= reach)
            continue;
        const double original_distance = original_outer.distance_from_lines<false>(junction.p);
        const double shift = std::clamp(distance - original_distance + extra_overlap * overlap_factors[point_idx], 0., cumulative_overlap);
        if (shift <= 0)
            continue;

        const Vec2d position = junction.p.cast<double>();
        const Vec2d first_direction = (nearest - position) / distance;
        Vec2d displacement = first_direction * shift;
        const size_t vertex_idx = point_idx == vertex_count ? 0 : point_idx;
        if (vertex_count >= 3 && (wall.is_closed || (vertex_idx > 0 && vertex_idx + 1 < vertex_count))) {
            const Vec2d incoming = (unshifted[vertex_idx].p - unshifted[(vertex_idx + vertex_count - 1) % vertex_count].p).cast<double>();
            const Vec2d outgoing = (unshifted[(vertex_idx + 1) % vertex_count].p - unshifted[vertex_idx].p).cast<double>();
            if (incoming.norm() > 0 && outgoing.norm() > 0) {
                const double turn_cos = incoming.normalized().dot(outgoing.normalized());
                if (turn_cos < 0.95 && turn_cos > -0.95) {
                    // At corners, move along the miter of connected outer edges.
                    const Line &first_line = current_outer.get_line(line_idx);
                    const double corner_tolerance = scale_(0.1);
                    double best_difference = corner_tolerance;
                    for (size_t candidate_idx : current_outer.all_lines_in_radius(junction.p, distance + corner_tolerance)) {
                        if (candidate_idx == line_idx)
                            continue;
                        const Line &candidate = current_outer.get_line(candidate_idx);
                        if (candidate.a != first_line.a && candidate.a != first_line.b &&
                            candidate.b != first_line.a && candidate.b != first_line.b)
                            continue;
                        if (candidate.a == candidate.b)
                            continue;
                        const Vec2d second_direction = junction.p.projection_onto(candidate).cast<double>() - position;
                        const double second_distance = second_direction.norm();
                        const double difference = std::abs(second_distance - distance);
                        if (second_distance == 0 || difference >= best_difference)
                            continue;
                        const Vec2d second_unit = second_direction / second_distance;
                        const double directions_dot = first_direction.dot(second_unit);
                        if (directions_dot >= 0.9 || directions_dot <= -0.5)
                            continue;
                        displacement = shift * (first_direction + second_unit) / (1. + directions_dot);
                        best_difference = difference;
                    }
                }
            }
        }
        junction.p = (position + displacement).cast<coord_t>();
        moved = true;
    }
    return moved;
}

} // namespace

void WallToolPaths::applyOverhangWallOverlap()
{
    if (m_params.overhang_support == nullptr || m_params.overhang_wall_overlap <= 0 || toolpaths.empty())
        return;

    // Snapshot the original geometry, including the zero-width fill-boundary marker.
    // ThickLine keeps endpoint widths attached to the segments indexed by the AABB tree.
    std::vector<ThickLines> original_walls;
    original_walls.reserve(toolpaths.size());
    for (const VariableWidthLines &walls : toolpaths)
        original_walls.push_back(overlap_wall_segments(walls));
    const ThickLines anchor = overlap_wall_segments(m_params.outer_wall_anchor);
    const AABBTreeLines::LinesDistancer<Line> support_lines(to_lines(*m_params.overhang_support));

    double cumulative_overlap = 0.;
    bool outer_shifted = false;
    for (size_t inset = 0; inset < toolpaths.size(); ++inset) {
        if (inset == 0 && anchor.empty())
            continue; // The actual outer contour stays in place.
        const ThickLines &original_outer = inset == 0 ? anchor : original_walls[inset - 1];
        if (original_outer.empty() || original_walls[inset].empty()) {
            cumulative_overlap = 0.;
            outer_shifted = false;
            continue;
        }

        const bool is_inner_contour = original_walls[inset].front().a_width == 0.;
        const double outer_spacing = inset == 0 ? m_params.outer_wall_anchor_spacing : inset == 1 ? bead_width_0 : bead_width_x;
        const double inner_spacing = inset == 0 ? bead_width_0 : bead_width_x;
        const double wall_spacing = 0.5 * (outer_spacing + inner_spacing);
        const double extra_overlap = is_inner_contour ? 0. : m_params.overhang_wall_overlap * std::min(outer_spacing, inner_spacing);
        cumulative_overlap += extra_overlap;
        if (is_inner_contour && !outer_shifted)
            continue;

        const WallDistancer original_outer_lines(original_outer);
        const WallDistancer current_outer_lines(inset == 0 ? anchor : overlap_wall_segments(toolpaths[inset - 1]));
        const double reach = wall_spacing + cumulative_overlap + scale_(0.5);
        bool inner_shifted = false;
        for (ExtrusionLine &wall : toolpaths[inset]) {
            if (wall.is_odd || wall.junctions.size() < 2)
                continue;
            wall.junctions = sample_overlap_wall(wall.junctions);
            std::vector<double> overlap_factors(wall.junctions.size(), 0.);
            if (!is_inner_contour) {
                const WallSupport support = sample_wall_support(wall.junctions, original_outer_lines, support_lines, reach);
                if (support.back().distance <= 0.)
                    continue;
                const WallOverlapProfile profile = wall_overlap_profile(support, wall.is_closed,
                    m_params.overhang_wall_overlap_start, m_params.overhang_wall_overlap_full,
                    scale_(m_params.overhang_wall_overlap_transition_length));
                overlap_factors = apply_overlap_profile(wall.junctions, support, profile);
            }
            if (move_wall_towards_outer(wall, original_outer_lines, current_outer_lines, overlap_factors,
                                       extra_overlap, cumulative_overlap, reach))
                inner_shifted = true;
        }
        outer_shifted = inner_shifted;
    }
}

void WallToolPaths::stitchToolPaths(std::vector<VariableWidthLines> &toolpaths, const coord_t bead_width_x)
{
    const coord_t stitch_distance = bead_width_x - 1; //In 0-width contours, junctions can cause up to 1-line-width gaps. Don't stitch more than 1 line width.

    for (unsigned int wall_idx = 0; wall_idx < toolpaths.size(); wall_idx++) {
        VariableWidthLines& wall_lines = toolpaths[wall_idx];

        VariableWidthLines stitched_polylines;
        VariableWidthLines closed_polygons;
        PolylineStitcher<VariableWidthLines, ExtrusionLine, ExtrusionJunction>::stitch(wall_lines, stitched_polylines, closed_polygons, stitch_distance);
#ifdef ARACHNE_STITCH_PATCH_DEBUG
        for (const ExtrusionLine& line : stitched_polylines) {
            if ( ! line.is_odd && line.polylineLength() > 3 * stitch_distance && line.size() > 3) {
                BOOST_LOG_TRIVIAL(error) << "Some even contour lines could not be closed into polygons!";
                assert(false && "Some even contour lines could not be closed into polygons!");
                BoundingBox aabb;
                for (auto line2 : wall_lines)
                    for (auto j : line2)
                        aabb.merge(j.p);
                {
                    static int iRun = 0;
                    SVG svg(debug_out_path("contours_before.svg-%d.png", iRun), aabb);
                    std::array<const char *, 8> colors    = {"gray", "black", "blue", "green", "lime", "purple", "red", "yellow"};
                    size_t                      color_idx = 0;
                    for (auto& inset : toolpaths)
                        for (auto& line2 : inset) {
                            // svg.writePolyline(line2.toPolygon(), col);

                            Polygon poly = line2.toPolygon();
                            Point last = poly.front();
                            for (size_t idx = 1 ; idx < poly.size(); idx++) {
                                Point here = poly[idx];
                                svg.draw(Line(last, here), colors[color_idx]);
//                                svg.draw_text((last + here) / 2, std::to_string(line2.junctions[idx].region_id).c_str(), "black");
                                last = here;
                            }
                            svg.draw(poly[0], colors[color_idx]);
                            // svg.nextLayer();
                            // svg.writePoints(poly, true, 0.1);
                            // svg.nextLayer();
                            color_idx = (color_idx + 1) % colors.size();
                        }
                }
                {
                    static int iRun = 0;
                    SVG svg(debug_out_path("contours-%d.svg", iRun), aabb);
                    for (auto& inset : toolpaths)
                        for (auto& line2 : inset)
                            svg.draw_outline(line2.toPolygon(), "gray");
                    for (auto& line2 : stitched_polylines) {
                        const char *col = line2.is_odd ? "gray" : "red";
                        if ( ! line2.is_odd)
                            std::cerr << "Non-closed even wall of size: " << line2.size()  << " at " << line2.front().p << "\n";
                        if ( ! line2.is_odd)
                            svg.draw(line2.front().p);
                        Polygon poly = line2.toPolygon();
                        Point last = poly.front();
                        for (size_t idx = 1 ; idx < poly.size(); idx++)
                        {
                            Point here = poly[idx];
                            svg.draw(Line(last, here), col);
                            last = here;
                        }
                    }
                    for (auto line2 : closed_polygons)
                        svg.draw(line2.toPolygon());
                }
            }
        }
#endif // ARACHNE_STITCH_PATCH_DEBUG
        wall_lines = stitched_polylines; // replace input toolpaths with stitched polylines

        for (ExtrusionLine& wall_polygon : closed_polygons)
        {
            if (wall_polygon.junctions.empty())
            {
                continue;
            }

            // PolylineStitcher, in some cases, produced closed extrusion (polygons),
            // but the endpoints differ by a small distance. So we reconnect them.
            // FIXME Lukas H.: Investigate more deeply why it is happening.
            if (wall_polygon.junctions.front().p != wall_polygon.junctions.back().p &&
                (wall_polygon.junctions.back().p - wall_polygon.junctions.front().p).cast<double>().norm() < stitch_distance) {
                wall_polygon.junctions.emplace_back(wall_polygon.junctions.front());
            }
            wall_polygon.is_closed = true;
            wall_lines.emplace_back(std::move(wall_polygon)); // add stitched polygons to result
        }
#ifdef DEBUG
        for (ExtrusionLine& line : wall_lines)
        {
            assert(line.inset_idx == wall_idx);
        }
#endif // DEBUG
    }
}

template<typename T> bool shorterThan(const T &shape, const coord_t check_length)
{
    const auto *p0     = &shape.back();
    int64_t     length = 0;
    for (const auto &p1 : shape) {
        length += (*p0 - p1).template cast<int64_t>().norm();
        if (length >= check_length)
            return false;
        p0 = &p1;
    }
    return true;
}

void WallToolPaths::removeSmallLines(std::vector<VariableWidthLines> &toolpaths)
{
    for (VariableWidthLines &inset : toolpaths) {
        for (size_t line_idx = 0; line_idx < inset.size(); line_idx++) {
            ExtrusionLine &line      = inset[line_idx];
            coord_t        min_width = std::numeric_limits<coord_t>::max();
            for (const ExtrusionJunction &j : line)
                min_width = std::min(min_width, j.w);
            // Only use min_length_factor for non-topmost, to prevent top gaps. Otherwise use default value.
            if (line.is_odd && !line.is_closed && shorterThan(line, m_params.is_top_or_bottom_layer ? (min_width / 2) : (min_width * m_params.min_length_factor))) { // remove line
                line = std::move(inset.back());
                inset.erase(--inset.end());
                line_idx--; // reconsider the current position
            }
        }
    }
}

void WallToolPaths::simplifyToolPaths(std::vector<VariableWidthLines>& toolpaths, const WallToolPathsParams& params)
{
    const int64_t maximum_resolution = params.wall_maximum_resolution;
    const int64_t maximum_deviation  = params.wall_maximum_deviation;

    const int64_t smallest_line_segment_squared  = maximum_resolution * maximum_resolution;
    const int64_t allowed_error_distance_squared = maximum_deviation * maximum_deviation;

    const int64_t maximum_extrusion_area_deviation = Slic3r::Arachne::meshfix_maximum_extrusion_area_deviation(); // unit: μm²

    for (VariableWidthLines& lines : toolpaths)
    {
        for (ExtrusionLine& line : lines)
        {
            line.simplify(smallest_line_segment_squared, allowed_error_distance_squared, maximum_extrusion_area_deviation);
        }
    }
}

const std::vector<VariableWidthLines> &WallToolPaths::getToolPaths()
{
    if (!toolpaths_generated)
        return generate();
    return toolpaths;
}

void WallToolPaths::separateOutInnerContour()
{
    //We'll remove all 0-width paths from the original toolpaths and store them separately as polygons.
    std::vector<VariableWidthLines> actual_toolpaths;
    actual_toolpaths.reserve(toolpaths.size()); //A bit too much, but the correct order of magnitude.
    std::vector<VariableWidthLines> contour_paths;
    contour_paths.reserve(toolpaths.size() / inset_count);
    inner_contour.clear();
    for (const VariableWidthLines &inset : toolpaths) {
        if (inset.empty())
            continue;
        bool is_contour = false;
        for (const ExtrusionLine &line : inset) {
            for (const ExtrusionJunction &j : line) {
                if (j.w == 0)
                    is_contour = true;
                else
                    is_contour = false;
                break;
            }
        }

        if (is_contour) {
#ifdef DEBUG
            for (const ExtrusionLine &line : inset)
                for (const ExtrusionJunction &j : line)
                    assert(j.w == 0);
#endif // DEBUG
            for (const ExtrusionLine &line : inset) {
                if (line.is_odd)
                    continue;            // odd lines don't contribute to the contour
                else if (line.is_closed) // sometimes an very small even polygonal wall is not stitched into a polygon
                    inner_contour.emplace_back(line.toPolygon());
            }
        } else {
            actual_toolpaths.emplace_back(inset);
        }
    }
    if (!actual_toolpaths.empty())
        toolpaths = std::move(actual_toolpaths); // Filtered out the 0-width paths.
    else
        toolpaths.clear();

    //The output walls from the skeletal trapezoidation have no known winding order, especially if they are joined together from polylines.
    //They can be in any direction, clockwise or counter-clockwise, regardless of whether the shapes are positive or negative.
    //To get a correct shape, we need to make the outside contour positive and any holes inside negative.
    //This can be done by applying the even-odd rule to the shape. This rule is not sensitive to the winding order of the polygon.
    //The even-odd rule would be incorrect if the polygon self-intersects, but that should never be generated by the skeletal trapezoidation.
    inner_contour = union_(inner_contour, pftEvenOdd);
}

const Polygons& WallToolPaths::getInnerContour()
{
    if (!toolpaths_generated && inset_count > 0)
    {
        generate();
    }
    else if(inset_count == 0)
    {
        return outline;
    }
    return inner_contour;
}

bool WallToolPaths::removeEmptyToolPaths(std::vector<VariableWidthLines> &toolpaths)
{
    toolpaths.erase(std::remove_if(toolpaths.begin(), toolpaths.end(), [](const VariableWidthLines& lines)
                                   {
                                       return lines.empty();
                                   }), toolpaths.end());
    return toolpaths.empty();
}

/*!
     * Get the order constraints of the insets when printing walls per region / hole.
     * Each returned pair consists of adjacent wall lines where the left has an inset_idx one lower than the right.
     *
     * Odd walls follow the same order as even ones, so that the centre line of a narrow wall is an inner wall
     * like any other when printing inner to outer.
     *
     * \param outer_to_inner Whether the wall polygons with a lower inset_idx should go before those with a higher one.
 */
WallToolPaths::ExtrusionLineSet WallToolPaths::getRegionOrder(const std::vector<ExtrusionLine *> &input, const bool outer_to_inner)
{
    ExtrusionLineSet order_requirements;
    // We build a grid where we map toolpath vertex locations to toolpaths,
    // so that we can easily find which two toolpaths are next to each other,
    // which is the requirement for there to be an order constraint.
    //
    // We use a PointGrid rather than a LineGrid to save on computation time.
    // In very rare cases two insets might lie next to each other without having neighboring vertices, e.g.
    //  \            .
    //   |  /        .
    //   | /         .
    //   ||          .
    //   | \         .
    //   |  \        .
    //  /            .
    // However, because of how Arachne works this will likely never be the case for two consecutive insets.
    // On the other hand one could imagine that two consecutive insets of a very large circle
    // could be simplify()ed such that the remaining vertices of the two insets don't align.
    // In those cases the order requirement is not captured,
    // which means that the PathOrderOptimizer *might* result in a violation of the user set path order.
    // This problem is expected to be not so severe and happen very sparsely.

    coord_t max_line_w = 0u;
    for (const ExtrusionLine *line : input) // compute max_line_w
        for (const ExtrusionJunction &junction : *line)
            max_line_w = std::max(max_line_w, junction.w);
    if (max_line_w == 0u)
        return order_requirements;

    struct LineLoc
    {
        ExtrusionJunction    j;
        const ExtrusionLine *line;
    };
    struct Locator
    {
        Point operator()(const LineLoc &elem) { return elem.j.p; }
    };

    // How much farther two verts may be apart due to corners.
    // This distance must be smaller than 2, because otherwise
    // we could create an order requirement between e.g.
    // wall 2 of one region and wall 3 of another region,
    // while another wall 3 of the first region would lie in between those two walls.
    // However, higher values are better against the limitations of using a PointGrid rather than a LineGrid.
    constexpr float diagonal_extension = 1.9f;
    const auto      searching_radius   = coord_t(max_line_w * diagonal_extension);
    using GridT                        = SparsePointGrid<LineLoc, Locator>;
    GridT grid(searching_radius);

    for (const ExtrusionLine *line : input)
        for (const ExtrusionJunction &junction : *line) grid.insert(LineLoc{junction, line});
    for (const std::pair<const SquareGrid::GridPoint, LineLoc> &pair : grid) {
        const LineLoc       &lineloc_here = pair.second;
        const ExtrusionLine *here         = lineloc_here.line;
        Point                loc_here     = pair.second.j.p;
        std::vector<LineLoc> nearby_verts = grid.getNearby(loc_here, searching_radius);
        for (const LineLoc &lineloc_nearby : nearby_verts) {
            const ExtrusionLine *nearby = lineloc_nearby.line;
            if (nearby == here)
                continue;
            if (nearby->inset_idx == here->inset_idx)
                continue;
            if (nearby->inset_idx > here->inset_idx + 1)
                continue; // not directly adjacent
            if (here->inset_idx > nearby->inset_idx + 1)
                continue; // not directly adjacent
            if (!shorter_then(loc_here - lineloc_nearby.j.p, (lineloc_here.j.w + lineloc_nearby.j.w) / 2 * diagonal_extension))
                continue; // points are too far away from each other
            if ((nearby->inset_idx < here->inset_idx) == outer_to_inner) {
                order_requirements.emplace(std::make_pair(nearby, here));
            } else {
                assert((nearby->inset_idx > here->inset_idx) == outer_to_inner);
                order_requirements.emplace(std::make_pair(here, nearby));
            }
        }
    }
    return order_requirements;
}

} // namespace Slic3r::Arachne
