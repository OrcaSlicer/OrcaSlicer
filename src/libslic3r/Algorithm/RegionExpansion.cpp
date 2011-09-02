#include "RegionExpansion.hpp"

#include <libslic3r/AABBTreeIndirect.hpp>
#include <libslic3r/ClipperZUtils.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/Utils.hpp>

#include <numeric>

namespace Slic3r {
namespace Algorithm {

// Calculating radius discretization according to ClipperLib offsetter code, see void ClipperOffset::DoOffset(double delta)
inline double clipper_round_offset_error(double offset, double arc_tolerance)
{
    static constexpr const double def_arc_tolerance = 0.25;
    const double y =
        arc_tolerance <= 0 ?
            def_arc_tolerance :
            arc_tolerance > offset * def_arc_tolerance ?
                offset * def_arc_tolerance :
                arc_tolerance;
    double steps = std::min(M_PI / std::acos(1. - y / offset), offset * M_PI);
    return offset * (1. - cos(M_PI / steps));
}

RegionExpansionParameters RegionExpansionParameters::build(
    // Scaled expansion value
    float                full_expansion,
    // Expand by waves of expansion_step size (expansion_step is scaled).
    float                expansion_step,
    // Don't take more than max_nr_steps for small expansion_step.
    size_t               max_nr_expansion_steps)
{
    assert(full_expansion > 0);
    assert(expansion_step > 0);
    assert(max_nr_expansion_steps > 0);

    RegionExpansionParameters out;
    // Initial expansion of src to make the source regions intersect with boundary regions just a bit.
    // The expansion should not be too tiny, but also small enough, so the following expansion will
    // compensate for tiny_expansion and bring the wave back to the boundary without producing
    // ugly cusps where it touches the boundary.
    out.tiny_expansion = std::min(0.25f * full_expansion, scaled<float>(0.05f));
    size_t nsteps = size_t(ceil((full_expansion - out.tiny_expansion) / expansion_step));
    if (max_nr_expansion_steps > 0)
        nsteps = std::min(nsteps, max_nr_expansion_steps);
    assert(nsteps > 0);
    out.initial_step = (full_expansion - out.tiny_expansion) / nsteps;
    if (nsteps > 1 && 0.25 * out.initial_step < out.tiny_expansion) {
        // Decrease the step size by lowering number of steps.
        nsteps       = std::max<size_t>(1, (floor((full_expansion - out.tiny_expansion) / (4. * out.tiny_expansion))));
        out.initial_step = (full_expansion - out.tiny_expansion) / nsteps;
    }
    if (0.25 * out.initial_step < out.tiny_expansion || nsteps == 1) {
        out.tiny_expansion = 0.2f * full_expansion;
        out.initial_step   = 0.8f * full_expansion;
    }
    out.other_step           = out.initial_step;
    out.num_other_steps      = nsteps - 1;

    // Accuracy of the offsetter for wave propagation.
    out.arc_tolerance        = scaled<double>(0.1);
    out.shortest_edge_length = out.initial_step * ClipperOffsetShortestEdgeFactor;

    // Maximum inflation of seed contours over the boundary. Used to trim boundary to speed up
    // clipping during wave propagation. Needs to be in sync with the offsetter accuracy.
    // Clipper positive round offset should rather offset less than more.
    // Still a little bit of additional offset was added.
    out.max_inflation = (out.tiny_expansion + nsteps * out.initial_step) * 1.1;
//                (clipper_round_offset_error(out.tiny_expansion, co.ArcTolerance) + nsteps * clipper_round_offset_error(out.initial_step, co.ArcTolerance) * 1.5; // Account for uncertainty 

    return out;
}

// similar to expolygons_to_zpaths(), but each contour is expanded before converted to zpath.
// The expanded contours are then opened (the first point is repeated at the end).
static ClipperLib_Z::Paths expolygons_to_zpaths_expanded_opened(
    const ExPolygons &src, const float expansion, coord_t &base_idx)
{
    ClipperLib_Z::Paths out;
    out.reserve(2 * std::accumulate(src.begin(), src.end(), size_t(0),
        [](const size_t acc, const ExPolygon &expoly) { return acc + expoly.num_contours(); }));
    ClipperLib::ClipperOffset offsetter;
    offsetter.ShortestEdgeLength = expansion * ClipperOffsetShortestEdgeFactor;
    ClipperLib::Paths expansion_cache;
    for (const ExPolygon &expoly : src) {
        for (size_t icontour = 0; icontour < expoly.num_contours(); ++ icontour) {
            // Execute reorients the contours so that the outer most contour has a positive area. Thus the output
            // contours will be CCW oriented even though the input paths are CW oriented.
            // Offset is applied after contour reorientation, thus the signum of the offset value is reversed.
            offsetter.Clear();
            offsetter.AddPath(expoly.contour_or_hole(icontour).points, ClipperLib::jtSquare, ClipperLib::etClosedPolygon);
            expansion_cache.clear();
            offsetter.Execute(expansion_cache, icontour == 0 ? expansion : -expansion);
            append(out, ClipperZUtils::to_zpaths<true>(expansion_cache, base_idx));
        }
        ++ base_idx;
    }
    return out;
}

// Paths were created by splitting closed polygons into open paths and then by clipping them.
// Thus some pieces of the clipped polygons may now become split at the ends of the source polygons.
// Those ends are sorted lexicographically in "splits".
// Reconnect those split pieces.
static inline void merge_splits(ClipperLib_Z::Paths &paths, std::vector<std::pair<ClipperLib_Z::IntPoint, int>> &splits)
{
    for (auto it_path = paths.begin(); it_path != paths.end(); ) {
        ClipperLib_Z::Path &path = *it_path;
        assert(path.size() >= 2);
        bool merged = false;
        if (path.size() >= 2) {
            const ClipperLib_Z::IntPoint &front = path.front();
            const ClipperLib_Z::IntPoint &back  = path.back();
            // The path before clipping was supposed to cross the clipping boundary or be fully out of it.
            // Thus the clipped contour is supposed to become open, with one exception: The anchor expands into a closed hole.
            if (front.x() != back.x() || front.y() != back.y()) {
                // Look up the ends in "splits", possibly join the contours.
                // "splits" maps into the other piece connected to the same end point.
                auto find_end = [&splits](const ClipperLib_Z::IntPoint &pt) -> std::pair<ClipperLib_Z::IntPoint, int>* {
                    auto it = std::lower_bound(splits.begin(), splits.end(), pt,
                        [](const auto &l, const auto &r){ return ClipperZUtils::zpoint_lower(l.first, r); });
                    return it != splits.end() && it->first == pt ? &(*it) : nullptr;
                };
                auto *end = find_end(front);
                bool  end_front = true;
                if (! end) {
                    end_front = false;
                    end = find_end(back);
                }
                if (end) {
                    // This segment ends at a split point of the source closed contour before clipping.
                    if (end->second == -1) {
                        // Open end was found, not matched yet.
                        end->second = int(it_path - paths.begin());
                    } else {
                        // Open end was found and matched with end->second
                        ClipperLib_Z::Path &other_path = paths[end->second];
                        polylines_merge(other_path, other_path.front() == end->first, std::move(path), end_front);
                        if (std::next(it_path) == paths.end()) {
                            paths.pop_back();
                            break;
                        }
                        path = std::move(paths.back());
                        paths.pop_back();
                        merged = true;
                    }
                }
            }
        }
        if (! merged)
            ++ it_path;
    }
}

using AABBTreeBBoxes = AABBTreeIndirect::Tree<2, coord_t>;

static AABBTreeBBoxes build_aabb_tree_over_expolygons(const ExPolygons &expolygons) 
{
    // Calculate bounding boxes of internal slices.
    std::vector<AABBTreeIndirect::BoundingBoxWrapper> bboxes;
    bboxes.reserve(expolygons.size());
    for (size_t i = 0; i < expolygons.size(); ++ i)
        bboxes.emplace_back(i, get_extents(expolygons[i].contour));
    // Build AABB tree over bounding boxes of boundary expolygons.
    AABBTreeBBoxes out;
    out.build_modify_input(bboxes);
    return out;
}

static int sample_in_expolygons(
    // AABB tree over boundary expolygons
    const AABBTreeBBoxes &aabb_tree, 
    const ExPolygons     &expolygons,
    const Point          &sample)
{
    int out = -1;
    AABBTreeIndirect::traverse(aabb_tree,
        [&sample](const AABBTreeBBoxes::Node &node) {
            return node.bbox.contains(sample);
        },
        [&expolygons, &sample, &out](const AABBTreeBBoxes::Node &node) {
            assert(node.is_leaf());
            assert(node.is_valid());
            if (expolygons[node.idx].contains(sample)) {
                out = int(node.idx);
                // Stop traversal.
                return false;
            }
            // Continue traversal.
            return true;
        });
    return out;
}

std::vector<WaveSeed> wave_seeds(
    // Source regions that are supposed to touch the boundary.
    const ExPolygons      &src,
    // Boundaries of source regions touching the "boundary" regions will be expanded into the "boundary" region.
    const ExPolygons      &boundary,
    // Initial expansion of src to make the source regions intersect with boundary regions just a bit.
    float                  tiny_expansion,
    // Sort output by boundary ID and source ID.
    bool                   sorted)
{
    assert(tiny_expansion > 0);

    if (src.empty() || boundary.empty())
        return {};

    using Intersection  = ClipperZUtils::ClipperZIntersectionVisitor::Intersection;
    using Intersections = ClipperZUtils::ClipperZIntersectionVisitor::Intersections;

    ClipperLib_Z::Paths segments;
    Intersections       intersections;

    coord_t             idx_boundary_begin = 1;
    coord_t             idx_boundary_end   = idx_boundary_begin;
    coord_t             idx_src_end;

    {
        ClipperLib_Z::Clipper zclipper;
        ClipperZUtils::ClipperZIntersectionVisitor visitor(intersections);
        zclipper.ZFillFunction(visitor.clipper_callback());
        // as closed contours
        zclipper.AddPaths(ClipperZUtils::expolygons_to_zpaths(boundary, idx_boundary_end), ClipperLib_Z::ptClip, true);
        // as open contours
        std::vector<std::pair<ClipperLib_Z::IntPoint, int>> zsrc_splits;
        {
            idx_src_end = idx_boundary_end;
            ClipperLib_Z::Paths zsrc = expolygons_to_zpaths_expanded_opened(src, tiny_expansion, idx_src_end);
            zclipper.AddPaths(zsrc, ClipperLib_Z::ptSubject, false);
            zsrc_splits.reserve(zsrc.size());
            for (const ClipperLib_Z::Path &path : zsrc) {
                assert(path.size() >= 2);
                assert(path.front() == path.back());
                zsrc_splits.emplace_back(path.front(), -1);
            }
            std::sort(zsrc_splits.begin(), zsrc_splits.end(), [](const auto &l, const auto &r){ return ClipperZUtils::zpoint_lower(l.first, r.first); });
        }
        ClipperLib_Z::PolyTree polytree;
        zclipper.Execute(ClipperLib_Z::ctIntersection, polytree, ClipperLib_Z::pftNonZero, ClipperLib_Z::pftNonZero);
        ClipperLib_Z::PolyTreeToPaths(std::move(polytree), segments);
        merge_splits(segments, zsrc_splits);
    }

    // AABBTree over bounding boxes of boundaries.
    // Only built if necessary, that is if any of the seed contours is closed, thus there is no intersection point
    // with the boundary and all Z coordinates of the closed contour point to the source contour.
    AABBTreeBBoxes aabb_tree;

    // Sort paths into their respective islands.
    // Each src x boundary will be processed (wave expanded) independently.
    // Multiple pieces of a single src may intersect the same boundary.
    WaveSeeds out;
    out.reserve(segments.size());
    int iseed = 0;
    for (const ClipperLib_Z::Path &path : segments) {
        assert(path.size() >= 2);
        ClipperLib_Z::IntPoint front = path.front();
        ClipperLib_Z::IntPoint back  = path.back();
        // Both ends of a seed segment are supposed to be inside a single boundary expolygon.
        // Thus as long as the seed contour is not closed, it should be open at a boundary point.
        assert((front == back && front.z() >= idx_boundary_end && front.z() < idx_src_end) || 
            //(front.z() < 0 && back.z() < 0));
            // Hope that at least one end of an open polyline is clipped by the boundary, thus an intersection point is created.
            (front.z() < 0 || back.z() < 0));

        if (front != back && front.z() >= 0 && back.z() >= 0) {
            // Very rare case when both endpoints intersect boundary ExPolygons in existing points.
            // So the ZFillFunction callback hasn't been called.
            continue;
        } else
        if (front == back && (front.z() < idx_boundary_end)) {
            // This should be a very rare exception.
            // See https://github.com/prusa3d/PrusaSlicer/issues/12469.
            // Segement is open, yet its first point seems to be part of boundary polygon.
            // Take the first point with src polygon index.
            for (const ClipperLib_Z::IntPoint &point : path) {
                if (point.z() >= idx_boundary_end) {
                    front = point;
                    back = point;
                }
            }
        }

        const Intersection *intersection = nullptr;
        auto intersection_point_valid = [idx_boundary_end, idx_src_end](const Intersection &is) {
            return is.first >= 1 && is.first < idx_boundary_end &&
                   is.second >= idx_boundary_end && is.second < idx_src_end;
        };
        if (front.z() < 0) {
            const Intersection &is = intersections[- front.z() - 1];
            assert(intersection_point_valid(is));
            if (intersection_point_valid(is))
                intersection = &is;
        }
        if (! intersection && back.z() < 0) {
            const Intersection &is = intersections[- back.z() - 1];
            assert(intersection_point_valid(is));
            if (intersection_point_valid(is))
                intersection = &is;
        }
        if (intersection) {
            // The path intersects the boundary contour at least at one side. 
            out.push_back({ uint32_t(intersection->second - idx_boundary_end), uint32_t(intersection->first - 1), ClipperZUtils::from_zpath(path) });
        } else {
            // This should be a closed contour.
            assert(front == back && front.z() >= idx_boundary_end && front.z() < idx_src_end);
            // Find a source boundary expolygon of one sample of this closed path.
            if (aabb_tree.empty())
                aabb_tree = build_aabb_tree_over_expolygons(boundary);
            int boundary_id = sample_in_expolygons(aabb_tree, boundary, Point(front.x(), front.y()));
            // Boundary that contains the sample point was found.
            assert(boundary_id >= 0);
            if (boundary_id >= 0)
                out.push_back({ uint32_t(front.z() - idx_boundary_end), uint32_t(boundary_id), ClipperZUtils::from_zpath(path) });
        }
        ++ iseed;
    }

    if (sorted)
        // Sort the seeds by their intersection boundary and source contour.
        std::sort(out.begin(), out.end(), lower_by_boundary_and_src);
    return out;
}

static ClipperLib::Paths wavefront_initial(ClipperLib::ClipperOffset &co, const ClipperLib::Paths &polylines, float offset)
{
    ClipperLib::Paths out;
    out.reserve(polylines.size());
    ClipperLib::Paths out_this;
    for (const ClipperLib::Path &path : polylines) {
        assert(path.size() >= 2);
        co.Clear();
        co.AddPath(path, jtRound, path.front() == path.back() ? ClipperLib::etClosedLine : ClipperLib::etOpenRound);
        co.Execute(out_this, offset);
        append(out, std::move(out_this));
    }
    return out;
}

// Input polygons may consist of multiple expolygons, even nested expolygons.
// After inflation some polygons may thus overlap, however the overlap is being resolved during the successive
// clipping operation, thus it is not being done here.
static ClipperLib::Paths wavefront_step(ClipperLib::ClipperOffset &co, const ClipperLib::Paths &polygons, float offset)
{
    ClipperLib::Paths out;
    out.reserve(polygons.size());
    ClipperLib::Paths out_this;
    for (const ClipperLib::Path &polygon : polygons) {
        co.Clear();
        // Execute reorients the contours so that the outer most contour has a positive area. Thus the output
        // contours will be CCW oriented even though the input paths are CW oriented.
        // Offset is applied after contour reorientation, thus the signum of the offset value is reversed.
        co.AddPath(polygon, jtRound, ClipperLib::etClosedPolygon);
        bool ccw = ClipperLib::Orientation(polygon);
        co.Execute(out_this, ccw ? offset : - offset);
        if (! ccw) {
            // Reverse the resulting contours.
            for (ClipperLib::Path &path : out_this)
                std::reverse(path.begin(), path.end());
        }
        append(out, std::move(out_this));
    }
    return out;
}

static ClipperLib::Paths wavefront_clip(const ClipperLib::Paths &wavefront, const Polygons &clipping)
{
    ClipperLib::Clipper clipper;
    clipper.AddPaths(wavefront, ClipperLib::ptSubject, true);
    clipper.AddPaths(ClipperUtils::PolygonsProvider(clipping),  ClipperLib::ptClip, true);
    ClipperLib::Paths out;
    clipper.Execute(ClipperLib::ctIntersection, out, ClipperLib::pftPositive, ClipperLib::pftPositive);
    return out;
}

static Polygons propagate_wave_from_boundary(
    ClipperLib::ClipperOffset   &co,
    // Seed of the wave: Open polylines very close to the boundary.
    const ClipperLib::Paths     &seed,
    // Boundary inside which the waveform will propagate.
    const ExPolygon             &boundary,
    // How much to inflate the seed lines to produce the first wave area.
    const float                  initial_step,
    // How much to inflate the first wave area and the successive wave areas in each step.
    const float                  other_step,
    // Number of inflate steps after the initial step.
    const size_t                 num_other_steps,
    // Maximum inflation of seed contours over the boundary. Used to trim boundary to speed up
    // clipping during wave propagation.
    const float                  max_inflation)
{
    assert(! seed.empty() && seed.front().size() >= 2);
    Polygons clipping = ClipperUtils::clip_clipper_polygons_with_subject_bbox(boundary, get_extents<true>(seed).inflated(max_inflation));
    ClipperLib::Paths polygons = wavefront_clip(wavefront_initial(co, seed, initial_step), clipping);
    // Now offset the remaining 
    for (size_t ioffset = 0; ioffset < num_other_steps; ++ ioffset)
        polygons = wavefront_clip(wavefront_step(co, polygons, other_step), clipping);
    return to_polygons(polygons);
}

// Resulting regions are sorted by boundary id and source id.
std::vector<RegionExpansion> propagate_waves(const WaveSeeds &seeds, const ExPolygons &boundary, const RegionExpansionParameters &params)
{
    std::vector<RegionExpansion> out;
    ClipperLib::Paths            paths;
    ClipperLib::ClipperOffset co;
    co.ArcTolerance       = params.arc_tolerance;
    co.ShortestEdgeLength = params.shortest_edge_length;
    for (auto it_seed = seeds.begin(); it_seed != seeds.end();) {
        auto it = it_seed;
        paths.clear();
        for (; it != seeds.end() && it->boundary == it_seed->boundary && it->src == it_seed->src; ++ it)
            paths.emplace_back(it->path);
        // Propagate the wavefront while clipping it with the trimmed boundary.
        // Collect the expanded polygons, merge them with the source polygons.
        RegionExpansion re;
        for (Polygon &polygon : propagate_wave_from_boundary(co, paths, boundary[it_seed->boundary], params.initial_step, params.other_step, params.num_other_steps, params.max_inflation))
            out.push_back({ std::move(polygon), it_seed->src, it_seed->boundary });
        it_seed = it;
    }

    return out;
}

std::vector<RegionExpansion> propagate_waves(const ExPolygons &src, const ExPolygons &boundary, const RegionExpansionParameters &params)
{
    return propagate_waves(wave_seeds(src, boundary, params.tiny_expansion, true), boundary, params);
}

std::vector<RegionExpansion> propagate_waves(const ExPolygons &src, const ExPolygons &boundary,
    // Scaled expansion value
    float expansion, 
    // Expand by waves of expansion_step size (expansion_step is scaled).
    float expansion_step,
    // Don't take more than max_nr_steps for small expansion_step.
    size_t max_nr_steps)
{
    return propagate_waves(src, boundary, RegionExpansionParameters::build(expansion, expansion_step, max_nr_steps));
}

// Returns regions per source ExPolygon expanded into boundary.
std::vector<RegionExpansionEx> propagate_waves_ex(const WaveSeeds &seeds, const ExPolygons &boundary, const RegionExpansionParameters &params)
{
    std::vector<RegionExpansion> expanded = propagate_waves(seeds, boundary, params);
    assert(std::is_sorted(seeds.begin(), seeds.end(), [](const auto &l, const auto &r){ return l.boundary < r.boundary || (l.boundary == r.boundary && l.src < r.src); }));
    Polygons acc;
    std::vector<RegionExpansionEx> out;
    for (auto it = expanded.begin(); it != expanded.end(); ) {
        auto it2 = it;
        acc.clear();
        for (; it2 != expanded.end() && it2->boundary_id == it->boundary_id && it2->src_id == it->src_id; ++ it2)
            acc.emplace_back(std::move(it2->polygon));
        size_t size = it2 - it;
        if (size == 1)
            out.push_back({ ExPolygon{std::move(acc.front())}, it->src_id, it->boundary_id });
        else {
            ExPolygons expolys = union_ex(acc);
            reserve_more_power_of_2(out, expolys.size());
            for (ExPolygon &ex : expolys)
                out.push_back({ std::move(ex), it->src_id, it->boundary_id });
        }
        it = it2;
    }
    return out;
}

// Returns regions per source ExPolygon expanded into boundary.
std::vector<RegionExpansionEx> propagate_waves_ex(
    // Source regions that are supposed to touch the boundary.
    // Boundaries of source regions touching the "boundary" regions will be expanded into the "boundary" region.
    const ExPolygons    &src,
    const ExPolygons    &boundary,
    // Scaled expansion value
    float                full_expansion,
    // Expand by waves of expansion_step size (expansion_step is scaled).
    float                expansion_step,
    // Don't take more than max_nr_steps for small expansion_step.
    size_t               max_nr_expansion_steps)
{
    auto params = RegionExpansionParameters::build(full_expansion, expansion_step, max_nr_expansion_steps);
    return propagate_waves_ex(wave_seeds(src, boundary, params.tiny_expansion, true), boundary, params);
}

std::vector<Polygons> expand_expolygons(const ExPolygons &src, const ExPolygons &boundary,
    // Scaled expansion value
    float expansion, 
    // Expand by waves of expansion_step size (expansion_step is scaled).
    float expansion_step,
    // Don't take more than max_nr_steps for small expansion_step.
    size_t max_nr_steps)
{
    std::vector<Polygons> out(src.size(), Polygons{});
    for (RegionExpansion &r : propagate_waves(src, boundary, expansion, expansion_step, max_nr_steps))
        out[r.src_id].emplace_back(std::move(r.polygon));
    return out;
}

std::vector<ExPolygon> merge_expansions_into_expolygons(ExPolygons &&src, std::vector<RegionExpansion> &&expanded)
{
    // expanded regions will be merged into source regions, thus they will be re-sorted by source id.
    std::sort(expanded.begin(), expanded.end(), [](const auto &l, const auto &r) { return l.src_id < r.src_id; });
    uint32_t   last = 0;
    Polygons   acc;
    ExPolygons out;
    out.reserve(src.size());
    for (auto it = expanded.begin(); it != expanded.end();) {
        for (; last < it->src_id; ++ last)
            out.emplace_back(std::move(src[last]));
        acc.clear();
        assert(it->src_id == last);
        for (; it != expanded.end() && it->src_id == last; ++ it)
            acc.emplace_back(std::move(it->polygon));
        //FIXME offset & merging could be more efficient, for example one does not need to copy the source expolygon
        ExPolygon &src_ex = src[last ++];
        assert(! src_ex.contour.empty());
#if 0
        {
            static int iRun = 0;
            BoundingBox bbox = get_extents(acc);
            bbox.merge(get_extents(src_ex));
            SVG svg(debug_out_path("expand_merge_expolygons-failed-union=%d.svg", iRun ++).c_str(), bbox);
            svg.draw(acc);
            svg.draw_outline(acc, "black", scale_(0.05));
            svg.draw(src_ex, "red");
            svg.Close();
        }
#endif
        Point sample = src_ex.contour.front();
        append(acc, to_polygons(std::move(src_ex)));
        ExPolygons merged = union_safety_offset_ex(acc);
        // Expanding one expolygon by waves should not change connectivity of the source expolygon:
        // Single expolygon should be produced possibly with increased number of holes.
        if (merged.size() > 1) {
            // assert(merged.size() == 1);
            // There is something wrong with the initial waves. Most likely the bridge was not valid at all
            // or the boundary region was very close to some bridge edge, but not really touching.
            // Pick only a single merged expolygon, which contains one sample point of the source expolygon.
            auto aabb_tree = build_aabb_tree_over_expolygons(merged);
            int id = sample_in_expolygons(aabb_tree, merged, sample);
            assert(id != -1);
            if (id != -1)
                out.emplace_back(std::move(merged[id]));
        } else if (merged.size() == 1)
            out.emplace_back(std::move(merged.front()));
    }
    for (; last < uint32_t(src.size()); ++ last)
        out.emplace_back(std::move(src[last]));
    return out;
}

std::vector<ExPolygon> expand_merge_expolygons(ExPolygons &&src, const ExPolygons &boundary, const RegionExpansionParameters &params)
{
    // expanded regions are sorted by boundary id and source id
    std::vector<RegionExpansion> expanded = propagate_waves(src, boundary, params);
    return merge_expansions_into_expolygons(std::move(src), std::move(expanded));
}

} // Algorithm
} // Slic3r
