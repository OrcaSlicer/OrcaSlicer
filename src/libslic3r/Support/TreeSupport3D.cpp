// Tree supports by Thomas Rahm, losely based on Tree Supports by CuraEngine.
// Original source of Thomas Rahm's tree supports:
// https://github.com/ThomasRahm/CuraEngine
//
// Original CuraEngine copyright:
// Copyright (c) 2021 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#include "TreeSupport3D.hpp"
#include "AABBTreeIndirect.hpp"
#include "AABBTreeLines.hpp"
#include "BuildVolume.hpp"
#include "ClipperUtils.hpp"
#include "EdgeGrid.hpp"
#include "Fill/Fill.hpp"
#include "Layer.hpp"
#include "Print.hpp"
#include "MultiPoint.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "MutablePolygon.hpp"
#include "SupportCommon.hpp"
#include "TriangleMeshSlicer.hpp"
#include "TreeSupport.hpp"
#include "I18N.hpp"

#include <cassert>
#include <chrono>
#include <fstream>
#include <optional>
#include <stdio.h>
#include <string>
#include <string_view>

#include <boost/log/trivial.hpp>

#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>
#include <tbb/spin_mutex.h>

#if defined(TREE_SUPPORT_SHOW_ERRORS) && defined(_WIN32)
    #define TREE_SUPPORT_SHOW_ERRORS_WIN32
#endif

#define TREE_SUPPORT_ORGANIC_NUDGE_NEW 1

#ifndef TREE_SUPPORT_ORGANIC_NUDGE_NEW
// Old version using OpenVDB, works but it is extremely slow for complex meshes.
#include "../OpenVDBUtilsLegacy.hpp"
#include <openvdb/tools/VolumeToSpheres.h>
#endif // TREE_SUPPORT_ORGANIC_NUDGE_NEW

#ifndef _L
#define _L(s) Slic3r::I18N::translate(s)
#endif

 //#define TREESUPPORT_DEBUG_SVG

namespace Slic3r
{

namespace TreeSupport3D
{

using LineInformation = std::vector<std::pair<Point, LineStatus>>;
using LineInformations = std::vector<LineInformation>;
using namespace std::literals;

static inline void validate_range(const Point &pt)
{
    static constexpr const int32_t hi = 65536 * 16384;
    if (pt.x() > hi || pt.y() > hi || -pt.x() > hi || -pt.y() > hi)
      throw ClipperLib::clipperException("Coordinate outside allowed range");
}

static inline void validate_range(const Points &points)
{
    for (const Point &p : points)
        validate_range(p);
}

static inline void validate_range(const MultiPoint &mp)
{
    validate_range(mp.points);
}

static inline void validate_range(const Polygons &polygons)
{
    for (const Polygon &p : polygons)
        validate_range(p);
}

static inline void validate_range(const Polylines &polylines)
{
    for (const Polyline &p : polylines)
        validate_range(p);
}

static inline void validate_range(const LineInformation &lines)
{
    for (const auto& p : lines)
        validate_range(p.first);
}

static inline void validate_range(const LineInformations &lines)
{
    for (const LineInformation &l : lines)
        validate_range(l);
}

static inline void check_self_intersections(const Polygons &polygons, const std::string_view message)
{
#ifdef TREE_SUPPORT_SHOW_ERRORS_WIN32
    if (!intersecting_edges(polygons).empty())
        ::MessageBoxA(nullptr, (std::string("TreeSupport infill self intersections: ") + std::string(message)).c_str(), "Bug detected!", MB_OK | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_ICONWARNING);
#endif // TREE_SUPPORT_SHOW_ERRORS_WIN32
}
static inline void check_self_intersections(const ExPolygon &expoly, const std::string_view message)
{
#ifdef TREE_SUPPORT_SHOW_ERRORS_WIN32
    check_self_intersections(to_polygons(expoly), message);
#endif // TREE_SUPPORT_SHOW_ERRORS_WIN32
}

static std::vector<std::pair<TreeSupportSettings, std::vector<size_t>>> group_meshes(const Print &print, const std::vector<size_t> &print_object_ids)
{
    std::vector<std::pair<TreeSupportSettings, std::vector<size_t>>> grouped_meshes;

    //FIXME this is ugly, it does not belong here.
    for (size_t object_id : print_object_ids) {
        const PrintObject       &print_object  = *print.get_object(object_id);
        const PrintObjectConfig &object_config = print_object.config();
        if (object_config.support_top_z_distance < EPSILON)
            // || min_feature_size < scaled<coord_t>(0.1) that is the minimum line width
            TreeSupportSettings::soluble = true;
    }

    size_t largest_printed_mesh_idx = 0;

    // Group all meshes that can be processed together. NOTE this is different from mesh-groups! Only one setting object is needed per group,
    // as different settings in the same group may only occur in the tip, which uses the original settings objects from the meshes.
    for (size_t object_id : print_object_ids) {
        const PrintObject       &print_object  = *print.get_object(object_id);

        bool found_existing_group = false;
        TreeSupportSettings next_settings{ TreeSupportMeshGroupSettings{ print_object }, print_object.slicing_parameters() };
        //FIXME for now only a single object per group is enabled.
#if 0
        for (size_t idx = 0; idx < grouped_meshes.size(); ++ idx)
            if (next_settings == grouped_meshes[idx].first) {
                found_existing_group = true;
                grouped_meshes[idx].second.emplace_back(object_id);
                // handle some settings that are only used for performance reasons. This ensures that a horrible set setting intended to improve performance can not reduce it drastically.
                grouped_meshes[idx].first.performance_interface_skip_layers = std::min(grouped_meshes[idx].first.performance_interface_skip_layers, next_settings.performance_interface_skip_layers);
            }
#endif
        if (! found_existing_group)
            grouped_meshes.emplace_back(next_settings, std::vector<size_t>{ object_id });

        // no need to do this per mesh group as adaptive layers and raft setting are not setable per mesh.
        if (print.get_object(largest_printed_mesh_idx)->layers().back()->print_z < print_object.layers().back()->print_z)
            largest_printed_mesh_idx = object_id;
    }

#if 0
    {
        std::vector<coord_t> known_z(storage.meshes[largest_printed_mesh_idx].layers.size());
        for (size_t z = 0; z < storage.meshes[largest_printed_mesh_idx].layers.size(); z++)
            known_z[z] = storage.meshes[largest_printed_mesh_idx].layers[z].printZ;
        for (size_t idx = 0; idx < grouped_meshes.size(); ++ idx)
            grouped_meshes[idx].first.setActualZ(known_z);
    }
#endif

    return grouped_meshes;
}

#if 0
// todo remove as only for debugging relevant
[[nodiscard]] static std::string getPolygonAsString(const Polygons& poly)
{
    std::string ret;
    for (auto path : poly)
        for (Point p : path) {
            if (ret != "")
                ret += ",";
            ret += "(" + std::to_string(p.x()) + "," + std::to_string(p.y()) + ")";
        }
    return ret;
}

[[nodiscard]] static const std::vector<Polygons> generate_overhangs(const TreeSupportSettings &settings, PrintObject &print_object, std::function<void()> throw_on_cancel)
{
    const size_t num_raft_layers   = settings.raft_layers.size();
    const size_t num_object_layers = print_object.layer_count();
    const size_t num_layers        = num_object_layers + num_raft_layers;
    std::vector<Polygons> out(num_layers, Polygons{});

    const PrintConfig       &print_config           = print_object.print()->config();
    const PrintObjectConfig &config                 = print_object.config();
    const bool               support_auto           = config.enable_support.value && is_auto(config.support_type.value);
    const int                support_enforce_layers = config.enforce_support_layers.value;
    std::vector<Polygons>    enforcers_layers{ print_object.slice_support_enforcers() };
    std::vector<Polygons>    blockers_layers{ print_object.slice_support_blockers() };
    print_object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, enforcers_layers);
    print_object.project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER, blockers_layers);
    const int                support_threshold      = config.support_threshold_angle.value;
    const bool               support_threshold_auto = support_threshold == 0;
    // +1 makes the threshold inclusive
    double                   tan_threshold          = support_threshold_auto ? 0. : tan(M_PI * double(support_threshold + 1) / 180.);
    //FIXME this is a fudge constant!
    auto                     enforcer_overhang_offset = scaled<double>(config.tree_support_tip_diameter.value);
    const coordf_t radius_sample_resolution = g_config_tree_support_collision_resolution;

    // calc the extrudable expolygons of each layer
    const coordf_t extrusion_width = config.line_width.value;
    const coordf_t extrusion_width_scaled = scale_(extrusion_width);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, print_object.layer_count()),
        [&](const tbb::blocked_range<size_t>& range) {
            for (size_t layer_nr = range.begin(); layer_nr < range.end(); layer_nr++) {
                if (print_object.print()->canceled())
                    break;
                Layer* layer = print_object.get_layer(layer_nr);
                // Filter out areas whose diameter that is smaller than extrusion_width, but we don't want to lose any details.
                layer->lslices_extrudable = intersection_ex(layer->lslices, offset2_ex(layer->lslices, -extrusion_width_scaled / 2, extrusion_width_scaled));
            }
        });

    size_t num_overhang_layers = support_auto ? num_object_layers : std::min(num_object_layers, std::max(size_t(support_enforce_layers), enforcers_layers.size()));
    tbb::parallel_for(tbb::blocked_range<LayerIndex>(1, num_overhang_layers),
        [&print_object, &config, &print_config, &enforcers_layers, &blockers_layers,
         support_auto, support_enforce_layers, support_threshold_auto, tan_threshold, enforcer_overhang_offset, num_raft_layers, radius_sample_resolution, &throw_on_cancel, &out]
        (const tbb::blocked_range<LayerIndex> &range) {
        for (LayerIndex layer_id = range.begin(); layer_id < range.end(); ++ layer_id) {
            const Layer   &current_layer  = *print_object.get_layer(layer_id);
            const Layer   &lower_layer    = *print_object.get_layer(layer_id - 1);
            // Full overhangs with zero lower_layer_offset and no blockers applied.
            Polygons       raw_overhangs;
            bool           raw_overhangs_calculated = false;
            // Final overhangs.
            Polygons       overhangs;
            // For how many layers full overhangs shall be supported.
            const bool     enforced_layer = layer_id < support_enforce_layers;
            if (support_auto || enforced_layer) {
                float lower_layer_offset;
                if (enforced_layer)
                    lower_layer_offset = 0;
                else if (support_threshold_auto) {
                    float external_perimeter_width = 0;
                    for (const LayerRegion *layerm : lower_layer.regions())
                        external_perimeter_width += layerm->flow(frExternalPerimeter).scaled_width();
                    external_perimeter_width /= lower_layer.region_count();
                    lower_layer_offset = external_perimeter_width - float(scale_(config.support_threshold_overlap.get_abs_value(unscale_(external_perimeter_width))));
                } else
                    lower_layer_offset = scaled<float>(lower_layer.height / tan_threshold);
                Polygons lower_layer_offseted = offset(lower_layer.lslices_extrudable, lower_layer_offset);
                overhangs = diff(current_layer.lslices_extrudable, lower_layer_offseted);
                if (lower_layer_offset == 0) {
                    raw_overhangs = overhangs;
                    raw_overhangs_calculated = true;
                }
                if (! (enforced_layer || blockers_layers.empty() || blockers_layers[layer_id].empty()))
                    overhangs = diff(overhangs, offset_ex(union_(blockers_layers[layer_id]), scale_(radius_sample_resolution)), ApplySafetyOffset::Yes);
                //if (config.bridge_no_support) {
                //    for (const LayerRegion *layerm : current_layer.regions())
                //        remove_bridges_from_contacts(print_config, lower_layer, *layerm,
                //            float(layerm->flow(frExternalPerimeter).scaled_width()), overhangs);
                //}
            }
            //check_self_intersections(overhangs, "generate_overhangs1");
            if (! enforcers_layers.empty() && ! enforcers_layers[layer_id].empty()) {
                // Has some support enforcers at this layer, apply them to the overhangs, don't apply the support threshold angle.
                //enforcers_layers[layer_id] = union_(enforcers_layers[layer_id]);
                //check_self_intersections(enforcers_layers[layer_id], "generate_overhangs - enforcers");
                //check_self_intersections(to_polygons(lower_layer.lslices), "generate_overhangs - lowerlayers");
                if (Polygons enforced_overhangs = intersection(raw_overhangs_calculated ? raw_overhangs : diff(current_layer.lslices, lower_layer.lslices), enforcers_layers[layer_id] /*, ApplySafetyOffset::Yes */);
                    ! enforced_overhangs.empty()) {
                    //FIXME this is a hack to make enforcers work on steep overhangs.
                    //check_self_intersections(enforced_overhangs, "generate_overhangs - enforced overhangs1");
                    //Polygons enforced_overhangs_prev = enforced_overhangs;
                    //check_self_intersections(to_polygons(union_ex(enforced_overhangs)), "generate_overhangs - enforced overhangs11");
                    //check_self_intersections(offset(union_ex(enforced_overhangs),
                    //FIXME enforcer_overhang_offset is a fudge constant!
                    enforced_overhangs = diff(offset(union_ex(enforced_overhangs), enforcer_overhang_offset),
                        lower_layer.lslices);
#ifdef TREESUPPORT_DEBUG_SVG
//                    if (! intersecting_edges(enforced_overhangs).empty())
                    {
                        static int irun = 0;
                        SVG::export_expolygons(debug_out_path("treesupport-self-intersections-%d.svg", ++irun),
                            { { { current_layer.lslices },        { "current_layer.lslices", "yellow", 0.5f } },
                              { { lower_layer.lslices },          { "lower_layer.lslices", "gray", 0.5f } },
                              { { union_ex(enforced_overhangs) }, { "enforced_overhangs", "red",  "black", "", scaled<coord_t>(0.1f), 0.5f } } });
                    }
#endif // TREESUPPORT_DEBUG_SVG
                    //check_self_intersections(enforced_overhangs, "generate_overhangs - enforced overhangs2");
                    overhangs = overhangs.empty() ? std::move(enforced_overhangs) : union_(overhangs, enforced_overhangs);
                    //check_self_intersections(overhangs, "generate_overhangs - enforcers");
                }
            }
            out[layer_id + num_raft_layers] = std::move(overhangs);
            throw_on_cancel();
        }
    });

#if 0
    if (num_raft_layers > 0) {
        const Layer   &first_layer = *print_object.get_layer(0);
        // Final overhangs.
        Polygons       overhangs = 
            // Don't apply blockes on raft layer.
            //(! blockers_layers.empty() && ! blockers_layers[layer_id].empty() ? 
            //    diff(first_layer.lslices, blockers_layers[layer_id], ApplySafetyOffset::Yes) :
                to_polygons(first_layer.lslices);
#if 0
        if (! enforcers_layers.empty() && ! enforcers_layers[layer_id].empty()) {
            if (Polygons enforced_overhangs = intersection(first_layer.lslices, enforcers_layers[layer_id] /*, ApplySafetyOffset::Yes */);
                ! enforced_overhangs.empty()) {
                //FIXME this is a hack to make enforcers work on steep overhangs.
                //FIXME enforcer_overhang_offset is a fudge constant!
                enforced_overhangs = offset(union_ex(enforced_overhangs), enforcer_overhang_offset);
                overhangs = overhangs.empty() ? std::move(enforced_overhangs) : union_(overhangs, enforced_overhangs);
            }
        }   
#endif
        out[num_raft_layers] = std::move(overhangs);
        throw_on_cancel();
    }
#endif

    return out;
}
#endif

/*!
 * \brief Precalculates all avoidances, that could be required.
 *
 * \param storage[in] Background storage to access meshes.
 * \param currently_processing_meshes[in] Indexes of all meshes that are processed in this iteration
 */
[[nodiscard]] static LayerIndex precalculate(const Print &print, const std::vector<Polygons> &overhangs, const TreeSupportSettings &config, const std::vector<size_t> &object_ids, TreeModelVolumes &volumes, std::function<void()> throw_on_cancel)
{
    // calculate top most layer that is relevant for support
    LayerIndex max_layer = 0;
    for (size_t object_id : object_ids) {
        const PrintObject &print_object      = *print.get_object(object_id);
        const int       num_raft_layers      = int(config.raft_layers.size());
        const int       num_layers           = int(print_object.layer_count()) + num_raft_layers;
        int             max_support_layer_id = 0;
        for (int layer_id = std::max<int>(num_raft_layers, 1); layer_id < num_layers; ++ layer_id)
            if (! overhangs[layer_id].empty())
                max_support_layer_id = layer_id;
        max_layer = std::max(max_support_layer_id - int(config.z_distance_top_layers), 0);
    }
    if (max_layer > 0)
        // The actual precalculation happens in TreeModelVolumes.
        volumes.precalculate(*print.get_object(object_ids.front()), max_layer, throw_on_cancel);
    return max_layer;
}

// picked from convert_lines_to_internal()
[[nodiscard]] LineStatus get_avoidance_status(const Point& p, coord_t radius, LayerIndex layer_idx,
    const TreeModelVolumes& volumes, const TreeSupportSettings& config)
{
    const bool min_xy_dist = config.xy_distance > config.xy_min_distance;

    LineStatus type = LineStatus::INVALID;

    if (!contains(volumes.getAvoidance(radius, layer_idx, TreeModelVolumes::AvoidanceType::FastSafe, false, min_xy_dist), p))
        type = LineStatus::TO_BP_SAFE;
    else if (!contains(volumes.getAvoidance(radius, layer_idx, TreeModelVolumes::AvoidanceType::Fast, false, min_xy_dist), p))
        type = LineStatus::TO_BP;
    else if (config.support_rests_on_model && !contains(volumes.getAvoidance(radius, layer_idx, TreeModelVolumes::AvoidanceType::FastSafe, true, min_xy_dist), p))
        type = LineStatus::TO_MODEL_GRACIOUS_SAFE;
    else if (config.support_rests_on_model && !contains(volumes.getAvoidance(radius, layer_idx, TreeModelVolumes::AvoidanceType::Fast, true, min_xy_dist), p))
        type = LineStatus::TO_MODEL_GRACIOUS;
    else if (config.support_rests_on_model && !contains(volumes.getCollision(radius, layer_idx, min_xy_dist), p))
        type = LineStatus::TO_MODEL;

    return type;
}

/*!
 * \brief Converts a Polygons object representing a line into the internal format.
 *
 * \param polylines[in] The Polyline that will be converted.
 * \param layer_idx[in] The current layer.
 * \return All lines of the \p polylines object, with information for each point regarding in which avoidance it is currently valid in.
 */
// Called by generate_initial_areas()
[[nodiscard]] static LineInformations convert_lines_to_internal(
    const TreeModelVolumes &volumes, const TreeSupportSettings &config,
    const Polylines &polylines, LayerIndex layer_idx)
{
    const bool min_xy_dist = config.xy_distance > config.xy_min_distance;

    LineInformations result;
    // Also checks if the position is valid, if it is NOT, it deletes that point
    for (const Polyline &line : polylines) {
        LineInformation res_line;
        for (Point p : line) {
            if (! contains(volumes.getAvoidance(config.getRadius(0), layer_idx, TreeModelVolumes::AvoidanceType::FastSafe, false, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_BP_SAFE);
            else if (! contains(volumes.getAvoidance(config.getRadius(0), layer_idx, TreeModelVolumes::AvoidanceType::Fast, false, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_BP);
            else if (config.support_rests_on_model && ! contains(volumes.getAvoidance(config.getRadius(0), layer_idx, TreeModelVolumes::AvoidanceType::FastSafe, true, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_MODEL_GRACIOUS_SAFE);
            else if (config.support_rests_on_model && ! contains(volumes.getAvoidance(config.getRadius(0), layer_idx, TreeModelVolumes::AvoidanceType::Fast, true, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_MODEL_GRACIOUS);
            else if (config.support_rests_on_model && ! contains(volumes.getCollision(config.getRadius(0), layer_idx, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_MODEL);
            else if (!res_line.empty()) {
                result.emplace_back(res_line);
                res_line.clear();
            }
        }
        if (!res_line.empty()) {
            result.emplace_back(res_line);
            res_line.clear();
        }
    }

    validate_range(result);
    return result;
}

#if 0
/*!
 * \brief Converts lines in internal format into a Polygons object representing these lines.
 *
 * \param lines[in] The lines that will be converted.
 * \return All lines of the \p lines object as a Polygons object.
 */
[[nodiscard]] static Polylines convert_internal_to_lines(LineInformations lines)
{
    Polylines result;
    for (LineInformation line : lines) {
        Polyline path;
        for (auto point_data : line)
            path.points.emplace_back(point_data.first);
        result.emplace_back(std::move(path));
    }
    validate_range(result);
    return result;
}
#endif

/*!
 * \brief Evaluates if a point has to be added now. Required for a split_lines call in generate_initial_areas().
 *
 * \param current_layer[in] The layer on which the point lies, point and its status.
 * \return whether the point is valid.
 */
[[nodiscard]] static bool evaluate_point_for_next_layer_function(
    const TreeModelVolumes &volumes, const TreeSupportSettings &config,
    size_t current_layer, const std::pair<Point, LineStatus> &p)
{
    using AvoidanceType = TreeModelVolumes::AvoidanceType;
    const bool min_xy_dist = config.xy_distance > config.xy_min_distance;
    if (! contains(volumes.getAvoidance(config.getRadius(0), current_layer - 1, p.second == LineStatus::TO_BP_SAFE ? AvoidanceType::FastSafe : AvoidanceType::Fast, false, min_xy_dist), p.first))
        return true;
    if (config.support_rests_on_model && (p.second != LineStatus::TO_BP && p.second != LineStatus::TO_BP_SAFE))
        return ! contains(
            p.second == LineStatus::TO_MODEL_GRACIOUS || p.second == LineStatus::TO_MODEL_GRACIOUS_SAFE ?
                volumes.getAvoidance(config.getRadius(0), current_layer - 1, p.second == LineStatus::TO_MODEL_GRACIOUS_SAFE ? AvoidanceType::FastSafe : AvoidanceType::Fast, true, min_xy_dist) :
                volumes.getCollision(config.getRadius(0), current_layer - 1, min_xy_dist),
            p.first);
    return false;
}

/*!
 * \brief Evaluates which points of some lines are not valid one layer below and which are. Assumes all points are valid on the current layer. Validity is evaluated using supplied lambda.
 *
 * \param lines[in] The lines that have to be evaluated.
 * \param evaluatePoint[in] The function used to evaluate the points.
 * \return A pair with which points are still valid in the first slot and which are not in the second slot.
 */
template<typename EvaluatePointFn>
[[nodiscard]] static std::pair<LineInformations, LineInformations> split_lines(const LineInformations &lines, EvaluatePointFn evaluatePoint)
{
    // assumes all Points on the current line are valid

    LineInformations keep;
    LineInformations set_free;
    for (const std::vector<std::pair<Point, LineStatus>> &line : lines) {
        bool            current_keep = true;
        LineInformation resulting_line;
        for (const std::pair<Point, LineStatus> &me : line) {
            if (evaluatePoint(me) != current_keep) {
                if (! resulting_line.empty())
                    (current_keep ? &keep : &set_free)->emplace_back(std::move(resulting_line));
                current_keep = !current_keep;
            }
            resulting_line.emplace_back(me);
        }
        if (! resulting_line.empty())
            (current_keep ? &keep : &set_free)->emplace_back(std::move(resulting_line));
    }
    validate_range(keep);
    validate_range(set_free);
    return std::pair<std::vector<std::vector<std::pair<Point, LineStatus>>>, std::vector<std::vector<std::pair<Point, LineStatus>>>>(keep, set_free);
}

// Ported from CURA's PolygonUtils::getNextPointWithDistance()
// Sample a next point at distance "dist" from start_pt on polyline segment (start_idx, start_idx + 1).
// Returns sample point and start index of its segment on polyline if such sample exists.
static std::optional<std::pair<Point, size_t>> polyline_sample_next_point_at_distance(const Points &polyline, const Point &start_pt, size_t start_idx, double dist)
{
    const double                dist2  = sqr(dist);
    const auto                  dist2i = int64_t(dist2);
    const auto eps    = scaled<double>(0.01);

    for (size_t i = start_idx + 1; i < polyline.size(); ++ i) {
        const Point p1 = polyline[i];
        if ((p1 - start_pt).cast<int64_t>().squaredNorm() >= dist2i) {
            // The end point is outside the circle with center "start_pt" and radius "dist".
            const Point p0  = polyline[i - 1];
            Vec2d       v   = (p1 - p0).cast<double>();
            double      l2v = v.squaredNorm();
            if (l2v < sqr(eps)) {
                // Very short segment.
                Point c = (p0 + p1) / 2;
                if (std::abs((start_pt - c).cast<double>().norm() - dist) < eps)
                    return std::pair<Point, size_t>{ c, i - 1 };
                else
                    continue;
            }
            Vec2d p0f = (start_pt - p0).cast<double>();
            // Foot point of start_pt into v.
            Vec2d foot_pt = v * (p0f.dot(v) / l2v);
            // Vector from foot point of "start_pt" to "start_pt".
            Vec2d xf = p0f - foot_pt;
            // Squared distance of "start_pt" from the ray (p0, p1).
            double l2_from_line = xf.squaredNorm();
            // Squared distance of an intersection point of a circle with center at the foot point.
            if (double l2_intersection = dist2 - l2_from_line;
                l2_intersection > - SCALED_EPSILON) {
                // The ray (p0, p1) touches or intersects a circle centered at "start_pt" with radius "dist".
                // Distance of the circle intersection point from the foot point.
                l2_intersection = std::max(l2_intersection, 0.);
                if ((v - foot_pt).cast<double>().squaredNorm() >= l2_intersection) {
                    // Intersection of the circle with the segment (p0, p1) is on the right side (close to p1) from the foot point.
                    Point p = p0 + (foot_pt + v * sqrt(l2_intersection / l2v)).cast<coord_t>();
                    validate_range(p);
                    return std::pair<Point, size_t>{ p, i - 1 };
                }
            }
        }
    }
    return {};
}

/*!
 * \brief Eensures that every line segment is about distance in length. The resulting lines may differ from the original but all points are on the original
 *
 * \param input[in] The lines on which evenly spaced points should be placed.
 * \param distance[in] The distance the points should be from each other.
 * \param min_points[in] The amount of points that have to be placed. If not enough can be placed the distance will be reduced to place this many points.
 * \return A Polygons object containing the evenly spaced points. Does not represent an area, more a collection of points on lines.
 */
[[nodiscard]] static Polylines ensure_maximum_distance_polyline(const Polylines &input, double distance, size_t min_points)
{
    Polylines result;
    for (Polyline part : input) {
        if (part.empty())
            continue;

        double len = length(part.points);
        Polyline line;
        double current_distance = std::max(distance, scaled<double>(0.1));
        if (len < 2 * distance && min_points <= 1)
        {
            // Insert the opposite point of the first one.
            //FIXME pretty expensive
            Polyline pl(part);
            pl.clip_end(len / 2);
            line.points.emplace_back(pl.points.back());
        }
        else
        {
            size_t optimal_end_index = part.size() - 1;

            if (part.front() == part.back()) {
                size_t optimal_start_index = 0;
                // If the polyline was a polygon, there is a high chance it was an overhang. Overhangs that are <60� tend to be very thin areas, so lets get the beginning and end of them and ensure that they are supported.
                // The first point of the line will always be supported, so rotate the order of points in this polyline that one of the two corresponding points that are furthest from each other is in the beginning.
                // The other will be manually added (optimal_end_index)
                coord_t max_dist2_between_vertecies = 0;
                for (size_t idx = 0; idx < part.size() - 1; ++ idx) {
                    for (size_t inner_idx = 0; inner_idx < part.size() - 1; inner_idx++) {
                        if ((part[idx] - part[inner_idx]).cast<double>().squaredNorm() > max_dist2_between_vertecies) {
                            optimal_start_index = idx;
                            optimal_end_index = inner_idx;
                            max_dist2_between_vertecies = (part[idx] - part[inner_idx]).cast<double>().squaredNorm();
                        }
                    }
                }
                std::rotate(part.begin(), part.begin() + optimal_start_index, part.end() - 1);
                part[part.size() - 1] = part[0]; // restore that property that this polyline ends where it started.
                optimal_end_index = (part.size() + optimal_end_index - optimal_start_index - 1) % (part.size() - 1);
            }

            while (line.size() < min_points && current_distance >= scaled<double>(0.1))
            {
                line.clear();
                Point current_point = part[0];
                line.points.emplace_back(part[0]);
                if (min_points > 1 || (part[0] - part[optimal_end_index]).cast<double>().norm() > current_distance)
                    line.points.emplace_back(part[optimal_end_index]);
                size_t current_index = 0;
                std::optional<std::pair<Point, size_t>> next_point;
                double next_distance = current_distance;
                // Get points so that at least min_points are added and they each are current_distance away from each other. If that is impossible, decrease current_distance a bit.
                // The input are lines, that means that the line from the last to the first vertex does not have to exist, so exclude all points that are on this line!
                while ((next_point = polyline_sample_next_point_at_distance(part.points, current_point, current_index, next_distance))) {
                    // Not every point that is distance away, is valid, as it may be much closer to another point. This is especially the case when the overhang is very thin.
                    // So this ensures that the points are actually a certain distance from each other.
                    // This assurance is only made on a per polygon basis, as different but close polygon may not be able to use support below the other polygon.
                    double min_distance_to_existing_point = std::numeric_limits<double>::max();
                    for (Point p : line)
                        min_distance_to_existing_point = std::min(min_distance_to_existing_point, (p - next_point->first).cast<double>().norm());
                    if (min_distance_to_existing_point >= current_distance) {
                        // viable point was found. Add to possible result.
                        line.points.emplace_back(next_point->first);
                        current_point = next_point->first;
                        current_index = next_point->second;
                        next_distance = current_distance;
                    } else {
                        if (current_point == next_point->first) {
                            // In case a fixpoint is encountered, better aggressively overcompensate so the code does not become stuck here...
                            BOOST_LOG_TRIVIAL(warning) << "Tree Support: Encountered a fixpoint in polyline_sample_next_point_at_distance. This is expected to happen if the distance (currently " << next_distance <<
                                ") is smaller than 100";
                            tree_supports_show_error("Encountered issue while placing tips. Some tips may be missing."sv, true);
                            if (next_distance > 2 * current_distance)
                                // This case should never happen, but better safe than sorry.
                                break;
                            next_distance += current_distance;
                            continue;
                        }
                        // if the point was too close, the next possible viable point is at least distance-min_distance_to_existing_point away from the one that was just checked.
                        next_distance = std::max(current_distance - min_distance_to_existing_point, scaled<double>(0.1));
                        current_point = next_point->first;
                        current_index = next_point->second;
                    }
                }
                current_distance *= 0.9;
            }
        }
        result.emplace_back(std::move(line));
    }
    validate_range(result);
    return result;
}

/*!
 * \brief Returns Polylines representing the (infill) lines that will result in slicing the given area
 *
 * \param area[in] The area that has to be filled with infill.
 * \param roof[in] Whether the roofing or regular support settings should be used.
 * \param layer_idx[in] The current layer index.
 * \param support_infill_distance[in] The distance that should be between the infill lines.
 *
 * \return A Polygons object that represents the resulting infill lines.
 */
[[nodiscard]] static Polylines generate_support_infill_lines(
    // Polygon to fill in with a zig-zag pattern supporting an overhang.
    const Polygons          &polygon,
    const SupportParameters &support_params,
    bool roof, LayerIndex layer_idx, coord_t support_infill_distance)
{
#if 0
    Polygons gaps;
    // as we effectivly use lines to place our supportPoints we may use the Infill class for it, while not made for it it works perfect

    const EFillMethod pattern = roof ? config.roof_pattern : config.support_pattern;

//    const bool zig_zaggify_infill = roof ? pattern == EFillMethod::ZIG_ZAG : config.zig_zaggify_support;
    const bool connect_polygons = false;
    constexpr coord_t support_roof_overlap = 0;
    constexpr size_t infill_multiplier = 1;
    constexpr coord_t outline_offset = 0;
    const int support_shift = roof ? 0 : support_infill_distance / 2;
    const size_t wall_line_count = include_walls && !roof ? config.support_wall_count : 0;
    const Point infill_origin;
    constexpr Polygons* perimeter_gaps = nullptr;
    constexpr bool use_endpieces = true;
    const bool connected_zigzags = roof ? false : config.connect_zigzags;
    const size_t zag_skip_count = roof ? 0 : config.zag_skip_count;
    constexpr coord_t pocket_size = 0;
    std::vector<AngleRadians> angles = roof ? config.support_roof_angles : config.support_infill_angles;
    std::vector<VariableWidthLines> toolpaths;

    const coord_t z = config.getActualZ(layer_idx);
    int divisor = static_cast<int>(angles.size());
    int index = ((layer_idx % divisor) + divisor) % divisor;
    const AngleRadians fill_angle = angles[index];
    Infill roof_computation(pattern, true /* zig_zaggify_infill */, connect_polygons, polygon,
        roof ? config.support_roof_line_width : config.support_line_width, support_infill_distance, support_roof_overlap, infill_multiplier,
        fill_angle, z, support_shift, config.resolution, wall_line_count, infill_origin,
        perimeter_gaps, connected_zigzags, use_endpieces, false /* skip_some_zags */, zag_skip_count, pocket_size);
    Polygons polygons;
    Polygons lines;
    roof_computation.generate(toolpaths, polygons, lines, config.settings);
    append(lines, to_polylines(polygons));
    return lines;
#else
//    const bool connected_zigzags = roof ? false : config.connect_zigzags;
//    const int support_shift = roof ? 0 : support_infill_distance / 2;

    const Flow            &flow   = roof ? support_params.support_material_interface_flow : support_params.support_material_flow;
    std::unique_ptr<Fill>  filler = std::unique_ptr<Fill>(Fill::new_from_type(roof ? support_params.interface_fill_pattern : support_params.base_fill_pattern));
    FillParams             fill_params;

    filler->layer_id = layer_idx;
    filler->spacing  = flow.spacing();
    filler->angle = roof ?
        //fixme support_layer.interface_id() instead of layer_idx
        (support_params.interface_angle + (layer_idx & 1) ? float(- M_PI / 4.) : float(+ M_PI / 4.)) :
        support_params.base_angle;

    fill_params.density     = float(roof ? support_params.interface_density : scaled<float>(filler->spacing) / (scaled<float>(filler->spacing) + float(support_infill_distance)));
    fill_params.dont_adjust = true;

    Polylines out;
    for (ExPolygon &expoly : union_ex(polygon)) {
        // The surface type does not matter.
        assert(area(expoly) > 0.);
#ifdef TREE_SUPPORT_SHOW_ERRORS_WIN32
        if (area(expoly) <= 0.)
            ::MessageBoxA(nullptr, "TreeSupport infill negative area", "Bug detected!", MB_OK | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_ICONWARNING);
#endif // TREE_SUPPORT_SHOW_ERRORS_WIN32
        assert(intersecting_edges(to_polygons(expoly)).empty());
        check_self_intersections(expoly, "generate_support_infill_lines");
        Surface surface(stInternal, std::move(expoly));
        try {
            Polylines pl = filler->fill_surface(&surface, fill_params);
            assert(pl.empty() || get_extents(surface.expolygon).inflated(SCALED_EPSILON).contains(get_extents(pl)));
#ifdef TREE_SUPPORT_SHOW_ERRORS_WIN32
            if (! pl.empty() && ! get_extents(surface.expolygon).inflated(SCALED_EPSILON).contains(get_extents(pl)))
                ::MessageBoxA(nullptr, "TreeSupport infill failure", "Bug detected!", MB_OK | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_ICONWARNING);
#endif // TREE_SUPPORT_SHOW_ERRORS_WIN32
            append(out, std::move(pl));
        } catch (InfillFailedException &) {
        }
    }
    validate_range(out);
    return out;
#endif
}

/*!
 * \brief Unions two Polygons. Ensures that if the input is non empty that the output also will be non empty.
 * \param first[in] The first Polygon.
 * \param second[in] The second Polygon.
 * \return The union of both Polygons
 */
[[nodiscard]] static Polygons safe_union(const Polygons first, const Polygons second = {})
{
    // unionPolygons can slowly remove Polygons under certain circumstances, because of rounding issues (Polygons that have a thin area).
    // This does not cause a problem when actually using it on large areas, but as influence areas (representing centerpoints) can be very thin, this does occur so this ugly workaround is needed
    // Here is an example of a Polygons object that will loose vertices when unioning, and will be gone after a few times unionPolygons was called:
    /*
    Polygons example;
    Polygon exampleInner;
    exampleInner.add(Point(120410,83599));//A
    exampleInner.add(Point(120384,83643));//B
    exampleInner.add(Point(120399,83618));//C
    exampleInner.add(Point(120414,83591));//D
    exampleInner.add(Point(120423,83570));//E
    exampleInner.add(Point(120419,83580));//F
    example.add(exampleInner);
    for(int i=0;i<10;i++){
         log("Iteration %d Example area: %f\n",i,area(example));
         example=example.unionPolygons();
    }
*/

    Polygons result;
    if (! first.empty() || ! second.empty()) {
        result = union_(first, second);
        if (result.empty()) {
            BOOST_LOG_TRIVIAL(debug) << "Caught an area destroying union, enlarging areas a bit.";
            // just take the few lines we have, and offset them a tiny bit. Needs to be offsetPolylines, as offset may aleady have problems with the area.
            result = union_(offset(to_polylines(first), scaled<float>(0.002), jtMiter, 1.2), offset(to_polylines(second), scaled<float>(0.002), jtMiter, 1.2));
        }
    }

    return result;
}

/*!
 * \brief Offsets (increases the area of) a polygons object in multiple steps to ensure that it does not lag through over a given obstacle.
 * \param me[in] Polygons object that has to be offset.
 * \param distance[in] The distance by which me should be offset. Expects values >=0.
 * \param collision[in] The area representing obstacles.
 * \param last_step_offset_without_check[in] The most it is allowed to offset in one step.
 * \param min_amount_offset[in] How many steps have to be done at least. As this uses round offset this increases the amount of vertices, which may be required if Polygons get very small. Required as arcTolerance is not exposed in offset, which should result with a similar result.
 * \return The resulting Polygons object.
 */
[[nodiscard]] static Polygons safe_offset_inc(const Polygons& me, coord_t distance, const Polygons& collision, coord_t safe_step_size, coord_t last_step_offset_without_check, size_t min_amount_offset)
{
    bool do_final_difference = last_step_offset_without_check == 0;
    Polygons ret = safe_union(me); // ensure sane input

    // Trim the collision polygons with the region of interest for diff() efficiency.
    Polygons collision_trimmed_buffer;
    auto collision_trimmed = [&collision_trimmed_buffer, &collision, &ret, distance]() -> const Polygons& {
        if (collision_trimmed_buffer.empty() && ! collision.empty())
            collision_trimmed_buffer = ClipperUtils::clip_clipper_polygons_with_subject_bbox(collision, get_extents(ret).inflated(std::max((coord_t)0, distance) + SCALED_EPSILON));
        return collision_trimmed_buffer;
    };

    if (distance == 0)
        return do_final_difference ? diff(ret, collision_trimmed()) : union_(ret);
    if (safe_step_size < 0 || last_step_offset_without_check < 0) {
        BOOST_LOG_TRIVIAL(warning) << "Offset increase got invalid parameter!";
        tree_supports_show_error("Negative offset distance... How did you manage this ?"sv, true);
        return do_final_difference ? diff(ret, collision_trimmed()) : union_(ret);
    }

    coord_t step_size = safe_step_size;
    int     steps = distance > last_step_offset_without_check ? (distance - last_step_offset_without_check) / step_size : 0;
    if (distance - steps * step_size > last_step_offset_without_check) {
        if ((steps + 1) * step_size <= distance)
            // This will be the case when last_step_offset_without_check >= safe_step_size
            ++ steps;
        else
            do_final_difference = true;
    }
    if (steps + (distance < last_step_offset_without_check || (distance % step_size) != 0) < int(min_amount_offset) && min_amount_offset > 1) {
        // yes one can add a bool as the standard specifies that a result from compare operators has to be 0 or 1
        // reduce the stepsize to ensure it is offset the required amount of times
        step_size = distance / min_amount_offset;
        if (step_size >= safe_step_size) {
            // effectivly reduce last_step_offset_without_check
            step_size = safe_step_size;
            steps = min_amount_offset;
        } else
            steps = distance / step_size;
    }
    // offset in steps
    for (int i = 0; i < steps; ++ i) {
        ret = diff(offset(ret, step_size, ClipperLib::jtRound, scaled<float>(0.01)), collision_trimmed());
        // ensure that if many offsets are done the performance does not suffer extremely by the new vertices of jtRound.
        if (i % 10 == 7)
            ret = polygons_simplify(ret, scaled<double>(0.015), polygons_strictly_simple);
    }
    // offset the remainder
    float last_offset = distance - steps * step_size;
    if (last_offset > SCALED_EPSILON)
        ret = offset(ret, distance - steps * step_size, ClipperLib::jtRound, scaled<float>(0.01));
    ret = polygons_simplify(ret, scaled<double>(0.015), polygons_strictly_simple);

    if (do_final_difference)
        ret = diff(ret, collision_trimmed());
    return union_(ret);
}

class RichInterfacePlacer : public InterfacePlacer {
public:
    RichInterfacePlacer(
        const InterfacePlacer        &interface_placer,
        const TreeModelVolumes       &volumes,
        bool                          force_tip_to_roof,
        size_t                        num_support_layers,
        std::vector<SupportElements> &move_bounds)
    :
        InterfacePlacer(interface_placer),
        volumes(volumes), force_tip_to_roof(force_tip_to_roof), move_bounds(move_bounds)
    {
        m_already_inserted.assign(num_support_layers, {});
        this->min_xy_dist = this->config.xy_distance > this->config.xy_min_distance;
        m_base_radius = scaled<coord_t>(0.01);
        m_base_circle = Polygon{ make_circle(m_base_radius, SUPPORT_TREE_CIRCLE_RESOLUTION) };

    }
    const TreeModelVolumes                             &volumes;
    // Radius of the tree tip is large enough to be covered by an interface.
    const bool                                          force_tip_to_roof;
    bool                                                min_xy_dist;

public:
    // called by sample_overhang_area()
    void add_points_along_lines(
        // Insert points (tree tips or top contact interfaces) along these lines.
        LineInformations    lines,
        // Start at this layer.
        LayerIndex          insert_layer_idx,
        // Insert this number of interface layers.
        size_t              roof_tip_layers,
        // True if an interface is already generated above these lines.
        size_t              supports_roof_layers,
        // The element tries to not move until this dtt is reached.
        size_t              dont_move_until)
    {
        validate_range(lines);
        // Add tip area as roof (happens when minimum roof area > minimum tip area) if possible
        size_t dtt_roof_tip;
        for (dtt_roof_tip = 0; dtt_roof_tip < roof_tip_layers && insert_layer_idx - dtt_roof_tip >= 1; ++ dtt_roof_tip) {
            size_t this_layer_idx = insert_layer_idx - dtt_roof_tip;
            auto evaluateRoofWillGenerate = [&](const std::pair<Point, LineStatus> &p) {
                //FIXME Vojtech: The circle is just shifted, it has a known size, the infill should fit all the time!
    #if 0
                Polygon roof_circle;
                for (Point corner : base_circle)
                    roof_circle.points.emplace_back(p.first + corner * config.min_radius);
                return !generate_support_infill_lines({ roof_circle }, config, true, insert_layer_idx - dtt_roof_tip, config.support_roof_line_distance).empty();
    #else
                return true;
    #endif
            };

            {
                std::pair<LineInformations, LineInformations> split =
                    // keep all lines that are still valid on the next layer
                    split_lines(lines, [this, this_layer_idx](const std::pair<Point, LineStatus> &p)
                        { return evaluate_point_for_next_layer_function(volumes, config, this_layer_idx, p); });
                LineInformations points = std::move(split.second);
                // Not all roofs are guaranteed to actually generate lines, so filter these out and add them as points.
                split = split_lines(split.first, evaluateRoofWillGenerate);
                lines = std::move(split.first);
                append(points, split.second);
                // add all points that would not be valid
                for (const LineInformation &line : points)
                    for (const std::pair<Point, LineStatus> &point_data : line)
                        add_point_as_influence_area(point_data, this_layer_idx,
                            // don't move until
                            roof_tip_layers - dtt_roof_tip,
                            // supports roof
                            dtt_roof_tip + supports_roof_layers > 0,
                            // disable ovalization
                            false);
            }

            // add all tips as roof to the roof storage
            Polygons new_roofs;
            for (const LineInformation &line : lines)
                //FIXME sweep the tip radius along the line?
                for (const std::pair<Point, LineStatus> &p : line) {
                    Polygon roof_circle{ m_base_circle };
                    roof_circle.scale(config.min_radius / m_base_radius);
                    roof_circle.translate(p.first);
                    new_roofs.emplace_back(std::move(roof_circle));
                }
            this->add_roof(std::move(new_roofs), this_layer_idx, dtt_roof_tip + supports_roof_layers);
        }

        for (const LineInformation &line : lines) {
            // If a line consists of enough tips, the assumption is that it is not a single tip, but part of a simulated support pattern.
            // Ovalisation should be disabled for these to improve the quality of the lines when tip_diameter=line_width
            bool disable_ovalistation = config.min_radius < 3 * config.support_line_width && roof_tip_layers == 0 && dtt_roof_tip == 0 && line.size() > 5;
            for (const std::pair<Point, LineStatus> &point_data : line)
                add_point_as_influence_area(point_data, insert_layer_idx - dtt_roof_tip,
                    // don't move until
                    dont_move_until > dtt_roof_tip ? dont_move_until - dtt_roof_tip : 0,
                    // supports roof
                    dtt_roof_tip + supports_roof_layers > 0,
                    disable_ovalistation);
        }
    }

private:
    // called by this->add_points_along_lines()
    void add_point_as_influence_area(std::pair<Point, LineStatus> p, LayerIndex insert_layer, size_t dont_move_until, bool roof, bool skip_ovalisation)
    {
        bool to_bp = p.second == LineStatus::TO_BP || p.second == LineStatus::TO_BP_SAFE;
        bool gracious = to_bp || p.second == LineStatus::TO_MODEL_GRACIOUS || p.second == LineStatus::TO_MODEL_GRACIOUS_SAFE;
        bool safe_radius = p.second == LineStatus::TO_BP_SAFE || p.second == LineStatus::TO_MODEL_GRACIOUS_SAFE;
        if (! config.support_rests_on_model && ! to_bp) {
            BOOST_LOG_TRIVIAL(warning) << "Tried to add an invalid support point";
            tree_supports_show_error("Unable to add tip. Some overhang may not be supported correctly."sv, true);
            return;
        }
        Polygons circle{ m_base_circle };
        circle.front().translate(p.first);
        {
            Point hash_pos = p.first / ((config.min_radius + 1) / 10);
            std::lock_guard<std::mutex> critical_section_movebounds(m_mutex_movebounds);
            if (!m_already_inserted[insert_layer].count(hash_pos)) {
                // normalize the point a bit to also catch points which are so close that inserting it would achieve nothing
                m_already_inserted[insert_layer].emplace(hash_pos);
                static constexpr const size_t dtt = 0;
                SupportElementState state;
                state.target_height = insert_layer;
                state.target_position = p.first;
                state.next_position = p.first;
                state.layer_idx = insert_layer;
                state.effective_radius_height = dtt;
                state.to_buildplate = to_bp;
                state.distance_to_top = dtt;
                state.result_on_layer = p.first;
                assert(state.result_on_layer_is_set());
                state.increased_to_model_radius = 0;
                state.to_model_gracious = gracious;
                state.elephant_foot_increases = 0;
                state.use_min_xy_dist = min_xy_dist;
                state.supports_roof = roof;
                state.dont_move_until = dont_move_until;
                state.can_use_safe_radius = safe_radius;
                state.missing_roof_layers = force_tip_to_roof ? dont_move_until : 0;
                state.skip_ovalisation = skip_ovalisation;
                move_bounds[insert_layer].emplace_back(state, std::move(circle));
            }
        }
    }

    // Outputs
    std::vector<SupportElements>                       &move_bounds;

    // Temps
    coord_t m_base_radius;
    Polygon                                       m_base_circle;

    // Mutexes, guards
    std::mutex                                          m_mutex_movebounds;
    std::vector<std::unordered_set<Point, PointHash>>   m_already_inserted;
};

int generate_raft_contact(
    const PrintObject               &print_object,
    const TreeSupportSettings       &config,
    InterfacePlacer                 &interface_placer)
{
    int raft_contact_layer_idx = -1;
    if (print_object.has_raft() && print_object.layer_count() > 0) {
        // Produce raft contact layer outside of the tree support loop, so that no trees will be generated for the raft contact layer.
        // Raft layers supporting raft contact interface will be produced by the classic raft generator.
        // Find the raft contact layer.
        raft_contact_layer_idx = int(config.raft_layers.size()) - 1;
        while (raft_contact_layer_idx > 0 && config.raft_layers[raft_contact_layer_idx] > print_object.slicing_parameters().raft_contact_top_z + EPSILON)
            -- raft_contact_layer_idx;
        // Create the raft contact layer.
        const ExPolygons &lslices   = print_object.get_layer(0)->lslices;
        double            expansion = print_object.config().raft_expansion.value;
        interface_placer.add_roof_unguarded(expansion > 0 ? expand(lslices, scaled<float>(expansion)) : to_polygons(lslices), raft_contact_layer_idx, 0);
    }
    return raft_contact_layer_idx;
}

void finalize_raft_contact(
    const PrintObject               &print_object,
    const int                        raft_contact_layer_idx,
    SupportGeneratorLayersPtr       &top_contacts,
    std::vector<SupportElements>    &move_bounds)
{
    if (raft_contact_layer_idx >= 0) {
        const size_t first_tree_layer = print_object.slicing_parameters().raft_layers() - 1;
        // Remove tree tips that start below the raft contact,
        // remove interface layers below the raft contact.
        for (size_t i = 0; i < first_tree_layer; ++i) {
            top_contacts[i] = nullptr;
            move_bounds[i].clear();
        }
        if (raft_contact_layer_idx >= 0 && print_object.config().raft_expansion.value > 0) {
            // If any tips at first_tree_layer now are completely inside the expanded raft layer, remove them as well before they are propagated to the ground.
            Polygons &raft_polygons = top_contacts[raft_contact_layer_idx]->polygons;
            EdgeGrid::Grid grid(get_extents(raft_polygons).inflated(SCALED_EPSILON));
            grid.create(raft_polygons, Polylines{}, coord_t(scale_(10.)));
            SupportElements &first_layer_move_bounds = move_bounds[first_tree_layer];
            double threshold = scaled<double>(print_object.config().raft_expansion.value) * 2.;
            first_layer_move_bounds.erase(std::remove_if(first_layer_move_bounds.begin(), first_layer_move_bounds.end(),
                [&grid, threshold](const SupportElement &el) {
                    coordf_t dist;
                    if (grid.signed_distance_edges(el.state.result_on_layer, threshold, dist)) {
                        assert(std::abs(dist) < threshold + SCALED_EPSILON);
                        // Support point is inside the expanded raft, remove it.
                        return dist < - 0.;
                    }
                    return false;
                }), first_layer_move_bounds.end());
    #if 0
            // Remove the remaining tips from the raft: Closing operation on tip circles.
            if (! first_layer_move_bounds.empty()) {
                const double eps = 0.1;
                // All tips supporting this layer are expected to have the same radius.
                double       radius = support_element_radius(config, first_layer_move_bounds.front());
                // Connect the tips with the following closing radius.
                double       closing_distance = radius;
                Polygon      circle = make_circle(radius + closing_distance, eps);
                Polygons     circles;
                circles.reserve(first_layer_move_bounds.size());
                for (const SupportElement &el : first_layer_move_bounds) {
                    circles.emplace_back(circle);
                    circles.back().translate(el.state.result_on_layer);
                }
                raft_polygons = diff(raft_polygons, offset(union_(circles), - closing_distance));
            }
    #endif
        }
    }
}

// Called by generate_initial_areas(), used in parallel by multiple layers.
// Produce
// 1) Maximum num_support_roof_layers roof (top interface & contact) layers.
// 2) Tree tips supporting either the roof layers or the object itself.
// num_support_roof_layers should always be respected:
// If num_support_roof_layers contact layers could not be produced, then the tree tip
// is augmented with SupportElementState::missing_roof_layers
// and the top "missing_roof_layers" of such particular tree tips are supposed to be coverted to
// roofs aka interface layers by the tool path generator.
void sample_overhang_area(
    // Area to support
    Polygons                           &&overhang_area,
    // If true, then the overhang_area is likely large and wide, thus it is worth to try
    // to cover it with continuous interfaces supported by zig-zag patterned tree tips.
    const bool                           large_horizontal_roof,
    // Index of the top suport layer generated by this function.
    const size_t                         layer_idx,
    // Maximum number of roof (contact, interface) layers between the overhang and tree tips to be generated.
    const size_t                         num_support_roof_layers,
    //
    const coord_t                        connect_length,
    // Configuration classes
    const TreeSupportMeshGroupSettings& mesh_group_settings,
    // Configuration & Output
    RichInterfacePlacer& interface_placer)
{
    // Assumption is that roof will support roof further up to avoid a lot of unnecessary branches. Each layer down it is checked whether the roof area
    // is still large enough to be a roof and aborted as soon as it is not. This part was already reworked a few times, and there could be an argument
    // made to change it again if there are actual issues encountered regarding supporting roofs.
    // Main problem is that some patterns change each layer, so just calculating points and checking if they are still valid an layer below is not useful,
    // as the pattern may be different one layer below. Same with calculating which points are now no longer being generated as result from
    // a decreasing roof, as there is no guarantee that a line will be above these points. Implementing a separate roof support behavior
    // for each pattern harms maintainability as it very well could be >100 LOC
    auto generate_roof_lines = [&interface_placer, &mesh_group_settings](const Polygons &area, LayerIndex layer_idx) -> Polylines {
        return generate_support_infill_lines(area, interface_placer.support_parameters, true, layer_idx, mesh_group_settings.support_roof_line_distance);
    };

    LineInformations        overhang_lines;
    // Track how many top contact / interface layers were already generated.
    size_t                  dtt_roof             = 0;
    size_t                  layer_generation_dtt = 0;

    if (large_horizontal_roof) {
        assert(num_support_roof_layers > 0);
        // Sometimes roofs could be empty as the pattern does not generate lines if the area is narrow enough (i am looking at you, concentric infill).
        // To catch these cases the added roofs are saved to be evaluated later.
        std::vector<Polygons>   added_roofs(num_support_roof_layers);
        Polygons                last_overhang = overhang_area;
        for (dtt_roof = 0; dtt_roof < num_support_roof_layers && layer_idx - dtt_roof >= 1; ++ dtt_roof) {
            // here the roof is handled. If roof can not be added the branches will try to not move instead
            Polygons forbidden_next;
            {
                const bool min_xy_dist = interface_placer.config.xy_distance > interface_placer.config.xy_min_distance;
                const Polygons &forbidden_next_raw = interface_placer.config.support_rests_on_model ?
                    interface_placer.volumes.getCollision(interface_placer.config.getRadius(0), layer_idx - (dtt_roof + 1), min_xy_dist) :
                    interface_placer.volumes.getAvoidance(interface_placer.config.getRadius(0), layer_idx - (dtt_roof + 1), TreeModelVolumes::AvoidanceType::Fast, false, min_xy_dist);
                // prevent rounding errors down the line
                //FIXME maybe use SafetyOffset::Yes at the following diff() instead?
                forbidden_next = offset(union_ex(forbidden_next_raw), scaled<float>(0.005), jtMiter, 1.2);
            }
            Polygons overhang_area_next = diff(overhang_area, forbidden_next);
            if (area(overhang_area_next) < mesh_group_settings.minimum_roof_area) {
                // Next layer down the roof area would be to small so we have to insert our roof support here.
                if (dtt_roof > 0) {
                    size_t dtt_before = dtt_roof - 1;
                    // Produce support head points supporting an interface layer: First produce the interface lines, then sample them.
                    overhang_lines = split_lines(
                        convert_lines_to_internal(interface_placer.volumes, interface_placer.config,
                            ensure_maximum_distance_polyline(generate_roof_lines(last_overhang, layer_idx - dtt_before), connect_length, 1), layer_idx - dtt_before),
                        [&interface_placer, layer_idx, dtt_before](const std::pair<Point, LineStatus> &p)
                            { return evaluate_point_for_next_layer_function(interface_placer.volumes, interface_placer.config, layer_idx - dtt_before, p); })
                        .first;
                }
                break;
            }
            added_roofs[dtt_roof] = overhang_area;
            last_overhang   = std::move(overhang_area);
            overhang_area = std::move(overhang_area_next);
        }

        layer_generation_dtt = std::max(dtt_roof, size_t(1)) - 1; // 1 inside max and -1 outside to avoid underflow. layer_generation_dtt=dtt_roof-1 if dtt_roof!=0;
        // if the roof should be valid, check that the area does generate lines. This is NOT guaranteed.
        if (overhang_lines.empty() && dtt_roof != 0 && generate_roof_lines(overhang_area, layer_idx - layer_generation_dtt).empty())
            for (size_t idx = 0; idx < dtt_roof; idx++) {
                // check for every roof area that it has resulting lines. Remember idx 1 means the 2. layer of roof => higher idx == lower layer
                if (generate_roof_lines(added_roofs[idx], layer_idx - idx).empty()) {
                    dtt_roof = idx;
                    layer_generation_dtt = std::max(dtt_roof, size_t(1)) - 1;
                    break;
                }
            }
        added_roofs.erase(added_roofs.begin() + dtt_roof, added_roofs.end());
        interface_placer.add_roofs(std::move(added_roofs), layer_idx);
    }

    if (overhang_lines.empty()) {
        // support_line_width to form a line here as otherwise most will be unsupported. Technically this violates branch distance, but not only is this the only reasonable choice,
        // but it ensures consistant behaviour as some infill patterns generate each line segment as its own polyline part causing a similar line forming behaviour.
        // This is not doen when a roof is above as the roof will support the model and the trees only need to support the roof
        bool supports_roof = dtt_roof > 0;
        bool continuous_tips = !supports_roof && large_horizontal_roof;
        Polylines polylines = ensure_maximum_distance_polyline(
            generate_support_infill_lines(overhang_area, interface_placer.support_parameters, supports_roof, layer_idx - layer_generation_dtt,
                supports_roof ? mesh_group_settings.support_roof_line_distance : mesh_group_settings.support_tree_branch_distance),
            continuous_tips ? interface_placer.config.min_radius / 2 : connect_length, 1);
        size_t point_count = 0;
        for (const Polyline &poly : polylines)
            point_count += poly.size();
        const size_t min_support_points = std::max(coord_t(1), std::min(coord_t(3), coord_t(total_length(overhang_area) / connect_length)));
        if (point_count <= min_support_points) {
            // add the outer wall (of the overhang) to ensure it is correct supported instead. Try placing the support points in a way that they fully support the outer wall, instead of just the with half of the the support line width.
            // I assume that even small overhangs are over one line width wide, so lets try to place the support points in a way that the full support area generated from them
            // will support the overhang (if this is not done it may only be half). This WILL NOT be the case when supporting an angle of about < 60 degrees so there is a fallback,
            // as some support is better than none.
            Polygons reduced_overhang_area = offset(union_ex(overhang_area), -interface_placer.config.support_line_width / 2.2, jtMiter, 1.2);
            polylines = ensure_maximum_distance_polyline(
                to_polylines(
                    ! reduced_overhang_area.empty() &&
                        area(offset(diff_ex(overhang_area, reduced_overhang_area), std::max(interface_placer.config.support_line_width, connect_length), jtMiter, 1.2)) < sqr(scaled<double>(0.001)) ?
                    reduced_overhang_area :
                    overhang_area),
                connect_length, min_support_points);
        }
        overhang_lines = convert_lines_to_internal(interface_placer.volumes, interface_placer.config, polylines, layer_idx - dtt_roof);
    }

    assert(dtt_roof <= layer_idx);
    if (dtt_roof >= layer_idx && large_horizontal_roof)
        // Reached buildplate when generating contact, interface and base interface layers.
        interface_placer.add_roof_build_plate(std::move(overhang_area), dtt_roof);
    else {
        // normal trees have to be generated
        const bool roof_enabled = num_support_roof_layers > 0;
        interface_placer.add_points_along_lines(
            // Sample along these lines
            overhang_lines,
            // First layer index to insert the tree tips or interfaces.
            layer_idx - dtt_roof,
            // Remaining roof tip layers.
            interface_placer.force_tip_to_roof ? num_support_roof_layers - dtt_roof : 0,
            // Supports roof already? How many roof layers were already produced above these tips?
            dtt_roof,
            // Don't move until the following distance to top is reached.
            roof_enabled ? num_support_roof_layers - dtt_roof : 0);
    }
}

/*!
 * \brief Creates the initial influence areas (that can later be propagated down) by placing them below the overhang.
 *
 * Generates Points where the Model should be supported and creates the areas where these points have to be placed.
 *
 * \param mesh[in] The mesh that is currently processed.
 * \param move_bounds[out] Storage for the influence areas.
 * \param storage[in] Background storage, required for adding roofs.
 */
static void generate_initial_areas(
    const PrintObject               &print_object,
    const TreeModelVolumes          &volumes,
    const TreeSupportSettings       &config,
    const std::vector<Polygons>     &overhangs,
    std::vector<SupportElements>    &move_bounds,
    InterfacePlacer                 &interface_placer,
    std::function<void()>            throw_on_cancel)
{
    using                           AvoidanceType = TreeModelVolumes::AvoidanceType;
    TreeSupportMeshGroupSettings    mesh_group_settings(print_object);

    // To ensure z_distance_top_layers are left empty between the overhang (zeroth empty layer), the support has to be added z_distance_top_layers+1 layers below
    const size_t z_distance_delta = config.z_distance_top_layers + 1;

    const bool min_xy_dist = config.xy_distance > config.xy_min_distance;

#if 0
    if (mesh.overhang_areas.size() <= z_distance_delta)
        return;
#endif

    const coord_t connect_length = (config.support_line_width * 100. / mesh_group_settings.support_tree_top_rate) + std::max(2. * config.min_radius - 1.0 * config.support_line_width, 0.0);
    // As r*r=x*x+y*y (circle equation): If a circle with center at (0,0) the top most point is at (0,r) as in y=r.
    // This calculates how far one has to move on the x-axis so that y=r-support_line_width/2.
    // In other words how far does one need to move on the x-axis to be support_line_width/2 away from the circle line.
    // As a circle is round this length is identical for every axis as long as the 90 degrees angle between both remains.
    const coord_t circle_length_to_half_linewidth_change = config.min_radius < config.support_line_width ?
        config.min_radius / 2 :
        scale_(sqrt(sqr(unscale<double>(config.min_radius)) - sqr(unscale<double>(config.min_radius - config.support_line_width / 2))));
    // Extra support offset to compensate for larger tip radiis. Also outset a bit more when z overwrites xy, because supporting something with a part of a support line is better than not supporting it at all.
    //FIXME Vojtech: This is not sufficient for support enforcers to work.
    //FIXME There is no account for the support overhang angle.
    //FIXME There is no account for the width of the collision regions.
    const coord_t extra_outset = std::max(coord_t(0), config.min_radius - config.support_line_width / 2) + (min_xy_dist ? config.support_line_width / 2 : 0)
        //FIXME this is a heuristic value for support enforcers to work.
//        + 10 * config.support_line_width;
        ;
    const size_t  num_support_roof_layers = mesh_group_settings.support_roof_layers;
    const bool    roof_enabled        = num_support_roof_layers > 0;
    const bool    force_tip_to_roof   = roof_enabled && (interface_placer.support_parameters.soluble_interface || sqr<double>(config.min_radius) * M_PI > mesh_group_settings.minimum_roof_area);
    // cap for how much layer below the overhang a new support point may be added, as other than with regular support every new inserted point
    // may cause extra material and time cost.  Could also be an user setting or differently calculated. Idea is that if an overhang
    // does not turn valid in double the amount of layers a slope of support angle would take to travel xy_distance, nothing reasonable will come from it.
    // The 2*z_distance_delta is only a catch for when the support angle is very high.
    // Used only if not min_xy_dist.
    coord_t max_overhang_insert_lag = 0;
    if (config.z_distance_top_layers > 0) {
        max_overhang_insert_lag = 2 * config.z_distance_top_layers;

    //FIXME
        if (mesh_group_settings.support_angle > EPSILON && mesh_group_settings.support_angle < 0.5 * M_PI - EPSILON) {
            //FIXME mesh_group_settings.support_angle does not apply to enforcers and also it does not apply to automatic support angle (by half the external perimeter width).
            //used by max_overhang_insert_lag, only if not min_xy_dist.
            const auto max_overhang_speed  = coord_t(tan(mesh_group_settings.support_angle) * config.layer_height);
            max_overhang_insert_lag = std::max(max_overhang_insert_lag, round_up_divide(config.xy_distance, max_overhang_speed / 2));
        }
    }

    size_t                                          num_support_layers;
    int                                             raft_contact_layer_idx;
    // Layers with their overhang regions.
    std::vector<std::pair<size_t, const Polygons*>>  raw_overhangs;

    {
        const size_t num_raft_layers     = config.raft_layers.size();
        const size_t first_support_layer = std::max(int(num_raft_layers) - int(z_distance_delta), 1);
        num_support_layers  = size_t(std::max(0, int(print_object.layer_count()) + int(num_raft_layers) - int(z_distance_delta)));
        raft_contact_layer_idx = generate_raft_contact(print_object, config, interface_placer);
        // Enumerate layers for which the support tips may be generated from overhangs above.
        raw_overhangs.reserve(num_support_layers - first_support_layer);
        for (size_t layer_idx = first_support_layer; layer_idx < num_support_layers; ++ layer_idx)
            if (const size_t overhang_idx = layer_idx + z_distance_delta; ! overhangs[overhang_idx].empty())
                raw_overhangs.push_back({ layer_idx, &overhangs[overhang_idx] });
    }

    RichInterfacePlacer rich_interface_placer{ interface_placer, volumes, force_tip_to_roof, num_support_layers, move_bounds };

    tbb::parallel_for(tbb::blocked_range<size_t>(0, raw_overhangs.size()),
        [&volumes, &config, &raw_overhangs, &mesh_group_settings,
         min_xy_dist, roof_enabled, num_support_roof_layers, extra_outset, circle_length_to_half_linewidth_change, connect_length,
         &rich_interface_placer, &throw_on_cancel](const tbb::blocked_range<size_t> &range) {
        for (size_t raw_overhang_idx = range.begin(); raw_overhang_idx < range.end(); ++ raw_overhang_idx) {
            size_t           layer_idx    = raw_overhangs[raw_overhang_idx].first;
            const Polygons  &overhang_raw = *raw_overhangs[raw_overhang_idx].second;

            // take the least restrictive avoidance possible
            Polygons relevant_forbidden;
            {
                const Polygons &relevant_forbidden_raw = config.support_rests_on_model ?
                    volumes.getCollision(config.getRadius(0), layer_idx, min_xy_dist) :
                    volumes.getAvoidance(config.getRadius(0), layer_idx, AvoidanceType::Fast, false, min_xy_dist);
                // prevent rounding errors down the line, points placed directly on the line of the forbidden area may not be added otherwise.
                relevant_forbidden = offset(union_ex(relevant_forbidden_raw), scaled<float>(0.005), jtMiter, 1.2);
            }

            // every overhang has saved if a roof should be generated for it. This can NOT be done in the for loop as an area may NOT have a roof
            // even if it is larger than the minimum_roof_area when it is only larger because of the support horizontal expansion and
            // it would not have a roof if the overhang is offset by support roof horizontal expansion instead. (At least this is the current behavior of the regular support)
            Polygons overhang_regular;
            {
                // When support_offset = 0 safe_offset_inc will only be the difference between overhang_raw and relevant_forbidden, that has to be calculated anyway.
                overhang_regular = safe_offset_inc(overhang_raw, mesh_group_settings.support_offset, relevant_forbidden, config.min_radius * 1.75 + config.xy_min_distance, 0, 1);
                //check_self_intersections(overhang_regular, "overhang_regular1");

                // offset ensures that areas that could be supported by a part of a support line, are not considered unsupported overhang
                Polygons remaining_overhang = intersection(
                    diff(mesh_group_settings.support_offset == 0 ?
                            overhang_raw :
                            offset(union_ex(overhang_raw), mesh_group_settings.support_offset, jtMiter, 1.2),
                         offset(union_ex(overhang_regular), config.support_line_width * 0.5, jtMiter, 1.2)),
                    relevant_forbidden);

                // Offset the area to compensate for large tip radiis. Offset happens in multiple steps to ensure the tip is as close to the original overhang as possible.
                //+config.support_line_width / 80  to avoid calculating very small (useless) offsets because of rounding errors.
                //FIXME likely a better approach would be to find correspondences between the full overhang and the trimmed overhang
                // and if there is no correspondence, project the missing points to the clipping curve.
                for (coord_t extra_total_offset_acc = 0; ! remaining_overhang.empty() && extra_total_offset_acc + config.support_line_width / 8 < extra_outset; ) {
                    const coord_t offset_current_step = std::min(
                        extra_total_offset_acc + 2 * config.support_line_width > config.min_radius ?
                            config.support_line_width / 8 :
                            circle_length_to_half_linewidth_change,
                        extra_outset - extra_total_offset_acc);
                    extra_total_offset_acc += offset_current_step;
                    const Polygons &raw_collision = volumes.getCollision(0, layer_idx, true);
                    const coord_t   offset_step   = config.xy_min_distance + config.support_line_width;
                    // Reducing the remaining overhang by the areas already supported.
                    //FIXME 1.5 * extra_total_offset_acc seems to be too much, it may remove some remaining overhang without being supported at all.
                    remaining_overhang = diff(remaining_overhang, safe_offset_inc(overhang_regular, 1.5 * extra_total_offset_acc, raw_collision, offset_step, 0, 1));
                    // Extending the overhangs by the inflated remaining overhangs.
                    overhang_regular   = union_(overhang_regular, diff(safe_offset_inc(remaining_overhang, extra_total_offset_acc, raw_collision, offset_step, 0, 1), relevant_forbidden));
                    //check_self_intersections(overhang_regular, "overhang_regular2");
                }
#if 0
                // If the xy distance overrides the z distance, some support needs to be inserted further down.
                //=> Analyze which support points do not fit on this layer and check if they will fit a few layers down (while adding them an infinite amount of layers down would technically be closer the the setting description, it would not produce reasonable results. )
                if (! min_xy_dist) {
                    LineInformations overhang_lines;
                    {
                        //Vojtech: Generate support heads at support_tree_branch_distance spacing by producing a zig-zag infill at support_tree_branch_distance spacing,
                        // which is then resmapled
                        // support_line_width to form a line here as otherwise most will be unsupported. Technically this violates branch distance,
                        // mbut not only is this the only reasonable choice, but it ensures consistent behavior as some infill patterns generate
                        // each line segment as its own polyline part causing a similar line forming behavior. Also it is assumed that
                        // the area that is valid a layer below is to small for support roof.
                        Polylines polylines = ensure_maximum_distance_polyline(
                            generate_support_infill_lines(remaining_overhang, support_params, false, layer_idx, mesh_group_settings.support_tree_branch_distance),
                            config.min_radius, 1);
                        if (polylines.size() <= 3)
                            // add the outer wall to ensure it is correct supported instead
                            polylines = ensure_maximum_distance_polyline(to_polylines(remaining_overhang), connect_length, 3);
                        for (const auto &line : polylines) {
                            LineInformation res_line;
                            for (Point p : line)
                                res_line.emplace_back(p, LineStatus::INVALID);
                            overhang_lines.emplace_back(res_line);
                        }
                        validate_range(overhang_lines);
                    }
                    for (size_t lag_ctr = 1; lag_ctr <= max_overhang_insert_lag && !overhang_lines.empty() && layer_idx - coord_t(lag_ctr) >= 1; lag_ctr++) {
                        // get least restricted avoidance for layer_idx-lag_ctr
                        const Polygons &relevant_forbidden_below = config.support_rests_on_model ?
                            volumes.getCollision(config.getRadius(0), layer_idx - lag_ctr, min_xy_dist) :
                            volumes.getAvoidance(config.getRadius(0), layer_idx - lag_ctr, AvoidanceType::Fast, false, min_xy_dist);
                        // it is not required to offset the forbidden area here as the points wont change: If points here are not inside the forbidden area neither will they be later when placing these points, as these are the same points.
                        auto evaluatePoint = [&](std::pair<Point, LineStatus> p) { return contains(relevant_forbidden_below, p.first); };

                        std::pair<LineInformations, LineInformations> split = split_lines(overhang_lines, evaluatePoint); // keep all lines that are invalid
                        overhang_lines = split.first;
                        // Set all now valid lines to their correct LineStatus. Easiest way is to just discard Avoidance information for each point and evaluate them again.
                        LineInformations fresh_valid_points = convert_lines_to_internal(volumes, config, convert_internal_to_lines(split.second), layer_idx - lag_ctr);
                        validate_range(fresh_valid_points);

                        rich_interface_placer.add_points_along_lines(fresh_valid_points, (force_tip_to_roof && lag_ctr <= num_support_roof_layers) ? num_support_roof_layers : 0, layer_idx - lag_ctr, false, roof_enabled ? num_support_roof_layers : 0);
                    }
                }
#endif
            }

            throw_on_cancel();

            if (roof_enabled) {
                // Try to support the overhangs by dense interfaces for num_support_roof_layers, cover the bottom most interface with tree tips.
                static constexpr const coord_t support_roof_offset = 0;
                Polygons overhang_roofs = safe_offset_inc(overhang_raw, support_roof_offset, relevant_forbidden, config.min_radius * 2 + config.xy_min_distance, 0, 1);
                if (mesh_group_settings.minimum_support_area > 0)
                    remove_small(overhang_roofs, mesh_group_settings.minimum_roof_area);
                overhang_regular = diff(overhang_regular, overhang_roofs, ApplySafetyOffset::Yes);
                //check_self_intersections(overhang_regular, "overhang_regular3");
                for (ExPolygon &roof_part : union_ex(overhang_roofs)) {
                    sample_overhang_area(to_polygons(std::move(roof_part)), true, layer_idx, num_support_roof_layers, connect_length,
                        mesh_group_settings, rich_interface_placer);
                    throw_on_cancel();
                }
            }
            // Either the roof is not enabled, then these are all the overhangs to be supported,
            // or roof is enabled and these are the thin overhangs at object slopes (not horizontal overhangs).
            if (mesh_group_settings.minimum_support_area > 0)
                remove_small(overhang_regular, mesh_group_settings.minimum_support_area);

            for (ExPolygon &support_part : union_ex(overhang_regular)) {
                sample_overhang_area(to_polygons(std::move(support_part)),
                    false, layer_idx, num_support_roof_layers, connect_length,
                    mesh_group_settings, rich_interface_placer);
                throw_on_cancel();
            }
        }
    });

    finalize_raft_contact(print_object, raft_contact_layer_idx, interface_placer.top_contacts_mutable(), move_bounds);
}

static unsigned int move_inside(const Polygons &polygons, Point &from, int distance = 0, int64_t maxDist2 = std::numeric_limits<int64_t>::max())
{
    Point  ret = from;
    double bestDist2 = std::numeric_limits<double>::max();
    auto   bestPoly = static_cast<unsigned int>(-1);
    bool   is_already_on_correct_side_of_boundary = false; // whether [from] is already on the right side of the boundary
    for (unsigned int poly_idx = 0; poly_idx < polygons.size(); ++ poly_idx) {
        const Polygon &poly = polygons[poly_idx];
        if (poly.size() < 2)
            continue;
        Point p0 = poly[poly.size() - 2];
        Point p1 = poly.back();
        // because we compare with vSize2 here (no division by zero), we also need to compare by vSize2 inside the loop
        // to avoid integer rounding edge cases
        bool projected_p_beyond_prev_segment = (p1 - p0).cast<int64_t>().dot((from - p0).cast<int64_t>()) >= (p1 - p0).cast<int64_t>().squaredNorm();
        for (const Point& p2 : poly) {
            // X = A + Normal(B-A) * (((B-A) dot (P-A)) / VSize(B-A));
            //   = A +       (B-A) *  ((B-A) dot (P-A)) / VSize2(B-A);
            // X = P projected on AB
            const Point& a = p1;
            const Point& b = p2;
            const Point& p = from;
            Vec2i64 ab = (b - a).cast<int64_t>();
            Vec2i64 ap = (p - a).cast<int64_t>();
            int64_t ab_length2 = ab.squaredNorm();
            if (ab_length2 <= 0) { //A = B, i.e. the input polygon had two adjacent points on top of each other.
                p1 = p2; //Skip only one of the points.
                continue;
            }
            int64_t dot_prod = ab.dot(ap);
            if (dot_prod <= 0) { // x is projected to before ab
                if (projected_p_beyond_prev_segment) {
                    //  case which looks like:   > .
                    projected_p_beyond_prev_segment = false;
                    Point& x = p1;

                    auto dist2 = (x - p).cast<int64_t>().squaredNorm();
                    if (dist2 < bestDist2) {
                        bestDist2 = dist2;
                        bestPoly = poly_idx;
                        if (distance == 0)
                            ret = x;
                        else {
                            Vec2d  abd   = ab.cast<double>();
                            Vec2d  p1p2  = (p1 - p0).cast<double>();
                            double lab   = abd.norm();
                            double lp1p2 = p1p2.norm();
                            // inward direction irrespective of sign of [distance]
                            Vec2d inward_dir = perp(abd * (scaled<double>(10.0) / lab) + p1p2 * (scaled<double>(10.0) / lp1p2));
                            // MM2INT(10.0) to retain precision for the eventual normalization
                            ret = x + (inward_dir * (distance / inward_dir.norm())).cast<coord_t>();
                            is_already_on_correct_side_of_boundary = inward_dir.dot((p - x).cast<double>()) * distance >= 0;
                        }
                    }
                } else {
                    projected_p_beyond_prev_segment = false;
                    p0 = p1;
                    p1 = p2;
                    continue;
                }
            } else if (dot_prod >= ab_length2) {
                // x is projected to beyond ab
                projected_p_beyond_prev_segment = true;
                p0 = p1;
                p1 = p2;
                continue;
            } else {
                // x is projected to a point properly on the line segment (not onto a vertex). The case which looks like | .
                projected_p_beyond_prev_segment = false;
                Point x = a + (ab.cast<double>() * (double(dot_prod) / double(ab_length2))).cast<coord_t>();
                auto dist2 = (p - x).cast<int64_t>().squaredNorm();
                if (dist2 < bestDist2) {
                    bestDist2 = dist2;
                    bestPoly = poly_idx;
                    if (distance == 0)
                        ret = x;
                    else {
                        Vec2d abd = ab.cast<double>();
                        Vec2d inward_dir = perp(abd * (distance / abd.norm())); // inward or outward depending on the sign of [distance]
                        ret = x + inward_dir.cast<coord_t>();
                        is_already_on_correct_side_of_boundary = inward_dir.dot((p - x).cast<double>()) >= 0;
                    }
                }
            }
            p0 = p1;
            p1 = p2;
        }
    }
    // when the best point is already inside and we're moving inside, or when the best point is already outside and we're moving outside
    if (is_already_on_correct_side_of_boundary) {
        if (bestDist2 < distance * distance)
            from = ret;
        else {
            // from = from; // original point stays unaltered. It is already inside by enough distance
        }
        return bestPoly;
    } else if (bestDist2 < maxDist2) {
        from = ret;
        return bestPoly;
    }
    return -1;
}

static Point move_inside_if_outside(const Polygons &polygons, Point from, int distance = 0, int64_t maxDist2 = std::numeric_limits<int64_t>::max())
{
    if (! contains(polygons, from))
        move_inside(polygons, from);
    return from;
}

/*!
 * \brief Checks if an influence area contains a valid subsection and returns the corresponding metadata and the new Influence area.
 *
 * Calculates an influence areas of the layer below, based on the influence area of one element on the current layer.
 * Increases every influence area by maximum_move_distance_slow. If this is not enough, as in we would change our gracious or to_buildplate status the influence areas are instead increased by maximum_move_distance_slow.
 * Also ensures that increasing the radius of a branch, does not cause it to change its status (like to_buildplate ). If this were the case, the radius is not increased instead.
 *
 * Warning: The used format inside this is different as the SupportElement does not have a valid area member. Instead this area is saved as value of the dictionary. This was done to avoid not needed heap allocations.
 *
 * \param settings[in] Which settings have to be used to check validity.
 * \param layer_idx[in] Number of the current layer.
 * \param parent[in] The metadata of the parents influence area.
 * \param relevant_offset[in] The maximal possible influence area. No guarantee regarding validity with current layer collision required, as it is ensured in-function!
 * \param to_bp_data[out] The part of the Influence area that can reach the buildplate.
 * \param to_model_data[out] The part of the Influence area that do not have to reach the buildplate. This has overlap with new_layer_data.
 * \param increased[out]  Area than can reach all further up support points. No assurance is made that the buildplate or the model can be reached in accordance to the user-supplied settings.
 * \param overspeed[in] How much should the already offset area be offset again. Usually this is 0.
 * \param mergelayer[in] Will the merge method be called on this layer. This information is required as some calculation can be avoided if they are not required for merging.
 * \return A valid support element for the next layer regarding the calculated influence areas. Empty if no influence are can be created using the supplied influence area and settings.
 */
[[nodiscard]] static std::optional<SupportElementState> increase_single_area(
    const TreeModelVolumes      &volumes,
    const TreeSupportSettings   &config,
    const AreaIncreaseSettings  &settings,
    const LayerIndex             layer_idx,
    const SupportElement        &parent,
    const Polygons              &relevant_offset,
    Polygons                    &to_bp_data,
    Polygons                    &to_model_data,
    Polygons                    &increased,
    const coord_t                overspeed,
    const bool                   mergelayer)
{
    SupportElementState current_elem{ SupportElementState::propagate_down(parent.state) };
    Polygons check_layer_data;
    if (settings.increase_radius)
        current_elem.effective_radius_height += 1;
    coord_t radius = support_element_collision_radius(config, current_elem);

    const auto _tiny_area_threshold = tiny_area_threshold();
    if (settings.move) {
        increased = relevant_offset;
        if (overspeed > 0) {
            const coord_t safe_movement_distance =
                (current_elem.use_min_xy_dist ? config.xy_min_distance : config.xy_distance) +
                (std::min(config.z_distance_top_layers, config.z_distance_bottom_layers) > 0 ? config.min_feature_size : 0);
            // The difference to ensure that the result not only conforms to wall_restriction, but collision/avoidance is done later.
            // The higher last_safe_step_movement_distance comes exactly from the fact that the collision will be subtracted later.
            increased = safe_offset_inc(increased, overspeed, volumes.getWallRestriction(support_element_collision_radius(config, parent.state), layer_idx, parent.state.use_min_xy_dist),
                safe_movement_distance, safe_movement_distance + radius, 1);
        }
        if (settings.no_error && settings.move)
            // as ClipperLib::jtRound has to be used for offsets this simplify is VERY important for performance.
            polygons_simplify(increased, scaled<float>(0.025), polygons_strictly_simple);
    } else
        // if no movement is done the areas keep parent area as no move == offset(0)
        increased = parent.influence_area;

    if (mergelayer || current_elem.to_buildplate) {
        to_bp_data = safe_union(diff_clipped(increased, volumes.getAvoidance(radius, layer_idx - 1, settings.type, false, settings.use_min_distance)));
        if (! current_elem.to_buildplate && area(to_bp_data) > _tiny_area_threshold) {
            // mostly happening in the tip, but with merges one should check every time, just to be sure.
            current_elem.to_buildplate = true; // sometimes nodes that can reach the buildplate are marked as cant reach, tainting subtrees. This corrects it.
            BOOST_LOG_TRIVIAL(debug) << "Corrected taint leading to a wrong to model value on layer " << layer_idx - 1 << " targeting " <<
                current_elem.target_height << " with radius " << radius;
        }
    }
    if (config.support_rests_on_model) {
        if (mergelayer || current_elem.to_model_gracious)
            to_model_data = safe_union(diff_clipped(increased, volumes.getAvoidance(radius, layer_idx - 1, settings.type, true, settings.use_min_distance)));

        if (!current_elem.to_model_gracious) {
            if (mergelayer && area(to_model_data) >= _tiny_area_threshold) {
                current_elem.to_model_gracious = true;
                BOOST_LOG_TRIVIAL(debug) << "Corrected taint leading to a wrong non gracious value on layer " << layer_idx - 1 << " targeting " <<
                    current_elem.target_height << " with radius " << radius;
            } else
                // Cannot route to gracious areas. Push the tree away from object and route it down anyways.
                to_model_data = safe_union(diff_clipped(increased, volumes.getCollision(radius, layer_idx - 1, settings.use_min_distance)));
        }
    }

    check_layer_data = current_elem.to_buildplate ? to_bp_data : to_model_data;

    if (settings.increase_radius && area(check_layer_data) > _tiny_area_threshold) {
        auto validWithRadius = [&](coord_t next_radius) {
            if (volumes.ceilRadius(next_radius, settings.use_min_distance) <= volumes.ceilRadius(radius, settings.use_min_distance))
                return true;

            Polygons to_bp_data_2;
            if (current_elem.to_buildplate)
                // regular union as output will not be used later => this area should always be a subset of the safe_union one (i think)
                to_bp_data_2 = diff_clipped(increased, volumes.getAvoidance(next_radius, layer_idx - 1, settings.type, false, settings.use_min_distance));
            Polygons to_model_data_2;
            if (config.support_rests_on_model && !current_elem.to_buildplate)
                to_model_data_2 = diff_clipped(increased,
                    current_elem.to_model_gracious ?
                        volumes.getAvoidance(next_radius, layer_idx - 1, settings.type, true, settings.use_min_distance) :
                        volumes.getCollision(next_radius, layer_idx - 1, settings.use_min_distance));
            Polygons check_layer_data_2 = current_elem.to_buildplate ? to_bp_data_2 : to_model_data_2;
            return area(check_layer_data_2) > _tiny_area_threshold;
        };
        coord_t ceil_radius_before = volumes.ceilRadius(radius, settings.use_min_distance);

        if (support_element_collision_radius(config, current_elem) < config.increase_radius_until_radius && support_element_collision_radius(config, current_elem) < support_element_radius(config, current_elem)) {
            coord_t target_radius = std::min(support_element_radius(config, current_elem), config.increase_radius_until_radius);
            coord_t current_ceil_radius = volumes.getRadiusNextCeil(radius, settings.use_min_distance);

            while (current_ceil_radius < target_radius && validWithRadius(volumes.getRadiusNextCeil(current_ceil_radius + 1, settings.use_min_distance)))
                current_ceil_radius = volumes.getRadiusNextCeil(current_ceil_radius + 1, settings.use_min_distance);
            size_t resulting_eff_dtt = current_elem.effective_radius_height;
            while (resulting_eff_dtt + 1 < current_elem.distance_to_top &&
                config.getRadius(resulting_eff_dtt + 1, current_elem.elephant_foot_increases) <= current_ceil_radius &&
                config.getRadius(resulting_eff_dtt + 1, current_elem.elephant_foot_increases) <= support_element_radius(config, current_elem))
                ++ resulting_eff_dtt;
            current_elem.effective_radius_height = resulting_eff_dtt;
        }
        radius = support_element_collision_radius(config, current_elem);

        const coord_t foot_radius_increase = std::max(config.bp_radius_increase_per_layer - config.branch_radius_increase_per_layer, 0.0);
        // Is nearly all of the time 1, but sometimes an increase of 1 could cause the radius to become bigger than recommendedMinRadius,
        // which could cause the radius to become bigger than precalculated.
        double planned_foot_increase = std::min(1.0, double(config.recommendedMinRadius(layer_idx - 1) - support_element_radius(config, current_elem)) / foot_radius_increase);
//FIXME
        bool increase_bp_foot = planned_foot_increase > 0 && current_elem.to_buildplate;
//        bool increase_bp_foot = false;

        if (increase_bp_foot && support_element_radius(config, current_elem) >= config.branch_radius && support_element_radius(config, current_elem) >= config.increase_radius_until_radius)
            if (validWithRadius(config.getRadius(current_elem.effective_radius_height, current_elem.elephant_foot_increases + planned_foot_increase))) {
                current_elem.elephant_foot_increases += planned_foot_increase;
                radius = support_element_collision_radius(config, current_elem);
            }

        if (ceil_radius_before != volumes.ceilRadius(radius, settings.use_min_distance)) {
            if (current_elem.to_buildplate)
                to_bp_data = safe_union(diff_clipped(increased, volumes.getAvoidance(radius, layer_idx - 1, settings.type, false, settings.use_min_distance)));
            if (config.support_rests_on_model && (!current_elem.to_buildplate || mergelayer))
                to_model_data = safe_union(diff_clipped(increased,
                    current_elem.to_model_gracious ?
                        volumes.getAvoidance(radius, layer_idx - 1, settings.type, true, settings.use_min_distance) :
                        volumes.getCollision(radius, layer_idx - 1, settings.use_min_distance)
                ));
            check_layer_data = current_elem.to_buildplate ? to_bp_data : to_model_data;
            if (area(check_layer_data) < _tiny_area_threshold) {
                BOOST_LOG_TRIVIAL(debug) << "Lost area by doing catch up from " << ceil_radius_before << " to radius " <<
                    volumes.ceilRadius(support_element_collision_radius(config, current_elem), settings.use_min_distance);
                tree_supports_show_error("Area lost catching up radius. May not cause visible malformation."sv, true);
            }
        }
    }

    return area(check_layer_data) > _tiny_area_threshold ? std::optional<SupportElementState>(current_elem) : std::optional<SupportElementState>();
}

struct SupportElementInfluenceAreas {
    // All influence areas: both to build plate and model.
    Polygons                        influence_areas;
    // Influence areas just to build plate.
    Polygons                        to_bp_areas;
    // Influence areas just to model.
    Polygons                        to_model_areas;

    void clear() {
        this->influence_areas.clear();
        this->to_bp_areas.clear();
        this->to_model_areas.clear();
    }
};

struct SupportElementMerging {
    SupportElementState                     state;
    /*!
     * \brief All elements in the layer above the current one that are supported by this element
     */
    SupportElement::ParentIndices           parents;

    SupportElementInfluenceAreas            areas;
    // Bounding box of all influence areas.
    Eigen::AlignedBox<coord_t, 2>           bbox_data;

    const Eigen::AlignedBox<coord_t, 2>&    bbox() const { return bbox_data;}
    const Point                             centroid() const { return (bbox_data.min() + bbox_data.max()) / 2; }
    void                                    set_bbox(const BoundingBox& abbox)
        { Point eps { coord_t(SCALED_EPSILON), coord_t(SCALED_EPSILON) }; bbox_data = { abbox.min - eps, abbox.max + eps }; }

    // Called by the AABBTree builder to get an index into the vector of source elements.
    // Not needed, thus zero is returned.
    static size_t                           idx() { return 0; }
};

/*!
 * \brief Increases influence areas as far as required.
 *
 * Calculates influence areas of the layer below, based on the influence areas of the current layer.
 * Increases every influence area by maximum_move_distance_slow. If this is not enough, as in it would change the gracious or to_buildplate status, the influence areas are instead increased by maximum_move_distance.
 * Also ensures that increasing the radius of a branch, does not cause it to change its status (like to_buildplate ). If this were the case, the radius is not increased instead.
 *
 * Warning: The used format inside this is different as the SupportElement does not have a valid area member. Instead this area is saved as value of the dictionary. This was done to avoid not needed heap allocations.
 *
 * \param to_bp_areas[out] Influence areas that can reach the buildplate
 * \param to_model_areas[out] Influence areas that do not have to reach the buildplate. This has overlap with new_layer_data, as areas that can reach the buildplate are also considered valid areas to the model.
 * This redundancy is required if a to_buildplate influence area is allowed to merge with a to model influence area.
 * \param influence_areas[out] Area than can reach all further up support points. No assurance is made that the buildplate or the model can be reached in accordance to the user-supplied settings.
 * \param bypass_merge_areas[out] Influence areas ready to be added to the layer below that do not need merging.
 * \param last_layer[in] Influence areas of the current layer.
 * \param layer_idx[in] Number of the current layer.
 * \param mergelayer[in] Will the merge method be called on this layer. This information is required as some calculation can be avoided if they are not required for merging.
 */
static void increase_areas_one_layer(
    const TreeModelVolumes              &volumes,
    const TreeSupportSettings           &config,
    // New areas at the layer below layer_idx
    std::vector<SupportElementMerging>  &merging_areas,
    // Layer above merging_areas.
    const LayerIndex                     layer_idx,
    // Layer elements above merging_areas.
    SupportElements                     &layer_elements,
    // If false, the merging_areas will not be merged for performance reasons.
    const bool                           mergelayer,
    std::function<void()>                throw_on_cancel)
{
    using AvoidanceType = TreeModelVolumes::AvoidanceType;

    tbb::parallel_for(tbb::blocked_range<size_t>(0, merging_areas.size(), 1),
        [&](const tbb::blocked_range<size_t> &range) {
        for (size_t merging_area_idx = range.begin(); merging_area_idx < range.end(); ++ merging_area_idx) {
            SupportElementMerging   &merging_area   = merging_areas[merging_area_idx];
            assert(merging_area.parents.size() == 1);
            SupportElement          &parent         = layer_elements[merging_area.parents.front()];
            SupportElementState      elem           = SupportElementState::propagate_down(parent.state);
            const Polygons          &wall_restriction =
                // Abstract representation of the model outline. If an influence area would move through it, it could teleport through a wall.
                volumes.getWallRestriction(support_element_collision_radius(config, parent.state), layer_idx, parent.state.use_min_xy_dist);

#ifdef TREESUPPORT_DEBUG_SVG
            SVG::export_expolygons(debug_out_path("treesupport-increase_areas_one_layer-%d-%ld.svg", layer_idx, int(merging_area_idx)),
                { { { union_ex(wall_restriction) },      { "wall_restricrictions", "gray", 0.5f } },
                  { { union_ex(parent.influence_area) }, { "parent", "red",  "black", "", scaled<coord_t>(0.1f), 0.5f } } });
#endif // TREESUPPORT_DEBUG_SVG

            Polygons to_bp_data, to_model_data;
            coord_t radius = support_element_collision_radius(config, elem);

            // When the radius increases, the outer "support wall" of the branch will have been moved farther away from the center (as this is the definition of radius).
            // As it is not specified that the support_tree_angle has to be one of the center of the branch, it is here seen as the smaller angle of the outer wall of the branch, to the outer wall of the same branch one layer above.
            // As the branch may have become larger the distance between these 2 walls is smaller than the distance of the center points.
            // These extra distance is added to the movement distance possible for this layer.

            coord_t extra_speed = 5; // The extra speed is added to both movement distances. Also move 5 microns faster than allowed to avoid rounding errors, this may cause issues at VERY VERY small layer heights.
            coord_t extra_slow_speed = 0; // Only added to the slow movement distance.
            const coord_t ceiled_parent_radius = volumes.ceilRadius(support_element_collision_radius(config, parent.state), parent.state.use_min_xy_dist);
            coord_t projected_radius_increased = config.getRadius(parent.state.effective_radius_height + 1, parent.state.elephant_foot_increases);
            coord_t projected_radius_delta = projected_radius_increased - support_element_collision_radius(config, parent.state);

            // When z distance is more than one layer up and down the Collision used to calculate the wall restriction will always include the wall (and not just the xy_min_distance) of the layer above and below like this (d = blocked area because of z distance):
            /*
             *  layer z+1:dddddiiiiiioooo
             *  layer z+0:xxxxxdddddddddd
             *  layer z-1:dddddxxxxxxxxxx
             *  For more detailed visualisation see calculateWallRestrictions
             */
            const coord_t safe_movement_distance =
                (elem.use_min_xy_dist ? config.xy_min_distance : config.xy_distance) +
                (std::min(config.z_distance_top_layers, config.z_distance_bottom_layers) > 0 ? config.min_feature_size : 0);
            if (ceiled_parent_radius == volumes.ceilRadius(projected_radius_increased, parent.state.use_min_xy_dist) ||
                projected_radius_increased < config.increase_radius_until_radius)
                // If it is guaranteed possible to increase the radius, the maximum movement speed can be increased, as it is assumed that the maximum movement speed is the one of the slower moving wall
                extra_speed += projected_radius_delta;
            else
                // if a guaranteed radius increase is not possible, only increase the slow speed
                // Ensure that the slow movement distance can not become larger than the fast one.
                extra_slow_speed += std::min(projected_radius_delta, (config.maximum_move_distance + extra_speed) - (config.maximum_move_distance_slow + extra_slow_speed));

            if (config.layer_start_bp_radius > layer_idx &&
                config.recommendedMinRadius(layer_idx - 1) < config.getRadius(elem.effective_radius_height + 1, elem.elephant_foot_increases)) {
                // can guarantee elephant foot radius increase
                if (ceiled_parent_radius == volumes.ceilRadius(config.getRadius(parent.state.effective_radius_height + 1, parent.state.elephant_foot_increases + 1), parent.state.use_min_xy_dist))
                    extra_speed += config.bp_radius_increase_per_layer;
                else
                    extra_slow_speed += std::min(coord_t(config.bp_radius_increase_per_layer),
                                                 config.maximum_move_distance - (config.maximum_move_distance_slow + extra_slow_speed));
            }

            const coord_t fast_speed = config.maximum_move_distance + extra_speed;
            const coord_t slow_speed = config.maximum_move_distance_slow + extra_speed + extra_slow_speed;

            Polygons offset_slow, offset_fast;

            bool add = false;
            bool bypass_merge = false;
            constexpr bool increase_radius = true, no_error = true, use_min_radius = true, move = true; // aliases for better readability

            // Determine in which order configurations are checked if they result in a valid influence area. Check will stop if a valid area is found
            std::vector<AreaIncreaseSettings> order;
            auto insertSetting = [&](AreaIncreaseSettings settings, bool back) {
                if (std::find(order.begin(), order.end(), settings) == order.end()) {
                    if (back)
                        order.emplace_back(settings);
                    else
                        order.insert(order.begin(), settings);
                }
            };

            const bool parent_moved_slow = elem.last_area_increase.increase_speed < config.maximum_move_distance;
            const bool avoidance_speed_mismatch = parent_moved_slow && elem.last_area_increase.type != AvoidanceType::Slow;
            if (elem.last_area_increase.move && elem.last_area_increase.no_error && elem.can_use_safe_radius && !mergelayer &&
                !avoidance_speed_mismatch && (elem.distance_to_top >= config.tip_layers || parent_moved_slow)) {
                // assume that the avoidance type that was best for the parent is best for me. Makes this function about 7% faster.
                insertSetting({ elem.last_area_increase.type, elem.last_area_increase.increase_speed < config.maximum_move_distance ? slow_speed : fast_speed,
                    increase_radius, elem.last_area_increase.no_error, !use_min_radius, elem.last_area_increase.move }, true);
                insertSetting({ elem.last_area_increase.type, elem.last_area_increase.increase_speed < config.maximum_move_distance ? slow_speed : fast_speed,
                    !increase_radius, elem.last_area_increase.no_error, !use_min_radius, elem.last_area_increase.move }, true);
            }
            // branch may still go though a hole, so a check has to be done whether the hole was already passed, and the regular avoidance can be used.
            if (!elem.can_use_safe_radius) {
                // if the radius until which it is always increased can not be guaranteed, move fast. This is to avoid holes smaller than the real branch radius.
                // This does not guarantee the avoidance of such holes, but ensures they are avoided if possible.
                // order.emplace_back(AvoidanceType::Slow,!increase_radius,no_error,!use_min_radius,move);
                insertSetting({ AvoidanceType::Slow, slow_speed, increase_radius, no_error, !use_min_radius, !move }, true); // did we go through the hole
                // in many cases the definition of hole is overly restrictive, so to avoid unnecessary fast movement in the tip, it is ignored there for a bit.
                // This CAN cause a branch to go though a hole it otherwise may have avoided.
                if (elem.distance_to_top < round_up_divide(config.tip_layers, size_t(2)))
                    insertSetting({ AvoidanceType::Fast, slow_speed, increase_radius, no_error, !use_min_radius, !move }, true);
                insertSetting({ AvoidanceType::FastSafe, fast_speed, increase_radius, no_error, !use_min_radius, !move }, true); // did we manage to avoid the hole
                insertSetting({ AvoidanceType::FastSafe, fast_speed, !increase_radius, no_error, !use_min_radius, move }, true);
                insertSetting({ AvoidanceType::Fast, fast_speed, !increase_radius, no_error, !use_min_radius, move }, true);
            } else {
                insertSetting({ AvoidanceType::Slow, slow_speed, increase_radius, no_error, !use_min_radius, move }, true);
                // while moving fast to be able to increase the radius (b) may seems preferable (over a) this can cause the a sudden skip in movement,
                // which looks similar to a layer shift and can reduce stability.
                // as such idx have chosen to only use the user setting for radius increases as a friendly recommendation.
                insertSetting({ AvoidanceType::Slow, slow_speed, !increase_radius, no_error, !use_min_radius, move }, true); // a
                if (elem.distance_to_top < config.tip_layers)
                    insertSetting({ AvoidanceType::FastSafe, slow_speed, increase_radius, no_error, !use_min_radius, move }, true);
                insertSetting({ AvoidanceType::FastSafe, fast_speed, increase_radius, no_error, !use_min_radius, move }, true); // b
                insertSetting({ AvoidanceType::FastSafe, fast_speed, !increase_radius, no_error, !use_min_radius, move }, true);
            }

            if (elem.use_min_xy_dist) {
                std::vector<AreaIncreaseSettings> new_order;
                // if the branch currently has to use min_xy_dist check if the configuration would also be valid
                // with the regular xy_distance before checking with use_min_radius (Only happens when Support Distance priority is z overrides xy )
                for (AreaIncreaseSettings settings : order) {
                    new_order.emplace_back(settings);
                    new_order.push_back({ settings.type, settings.increase_speed, settings.increase_radius, settings.no_error, use_min_radius, settings.move });
                }
                order = new_order;
            }
            if (elem.to_buildplate || (elem.to_model_gracious && intersection(parent.influence_area, volumes.getPlaceableAreas(radius, layer_idx, throw_on_cancel)).empty())) {
                // error case
                // it is normal that we wont be able to find a new area at some point in time if we wont be able to reach layer 0 aka have to connect with the model
                insertSetting({ AvoidanceType::Fast, fast_speed, !increase_radius, !no_error, elem.use_min_xy_dist, move }, true);
            }
            if (elem.distance_to_top < elem.dont_move_until && elem.can_use_safe_radius) // only do not move when holes would be avoided in every case.
                // Only do not move when already in a no hole avoidance with the regular xy distance.
                insertSetting({ AvoidanceType::Slow, 0, increase_radius, no_error, !use_min_radius, !move }, false);

            Polygons inc_wo_collision;
            // Check whether it is faster to calculate the area increased with the fast speed independently from the slow area, or time could be saved by reusing the slow area to calculate the fast one.
            // Calculated by comparing the steps saved when calcualting idependently with the saved steps when not.
            bool offset_independant_faster = radius / safe_movement_distance - int(config.maximum_move_distance + extra_speed < radius + safe_movement_distance) >
                                             round_up_divide((extra_speed + extra_slow_speed + config.maximum_move_distance_slow), safe_movement_distance);
            for (const AreaIncreaseSettings &settings : order) {
                if (settings.move) {
                    if (offset_slow.empty() && (settings.increase_speed == slow_speed || ! offset_independant_faster)) {
                        // offsetting in 2 steps makes our offsetted area rounder preventing (rounding) errors created by to pointy areas. At this point one can see that the Polygons class
                        // was never made for precision in the single digit micron range.
                        offset_slow = safe_offset_inc(parent.influence_area, extra_speed + extra_slow_speed + config.maximum_move_distance_slow,
                            wall_restriction, safe_movement_distance, offset_independant_faster ? safe_movement_distance + radius : 0, 2);
#ifdef TREESUPPORT_DEBUG_SVG
                        SVG::export_expolygons(debug_out_path("treesupport-increase_areas_one_layer-slow-%d-%ld.svg", layer_idx, int(merging_area_idx)),
                            { { { union_ex(wall_restriction) }, { "wall_restricrictions", "gray", 0.5f } },
                              { { union_ex(offset_slow) },      { "offset_slow", "red",  "black", "", scaled<coord_t>(0.1f), 0.5f } } });
#endif // TREESUPPORT_DEBUG_SVG
                    }
                    if (offset_fast.empty() && settings.increase_speed != slow_speed) {
                        if (offset_independant_faster)
                            offset_fast = safe_offset_inc(parent.influence_area, extra_speed + config.maximum_move_distance,
                                wall_restriction, safe_movement_distance, offset_independant_faster ? safe_movement_distance + radius : 0, 1);
                        else {
                            const coord_t delta_slow_fast = config.maximum_move_distance - (config.maximum_move_distance_slow + extra_slow_speed);
                            offset_fast = safe_offset_inc(offset_slow, delta_slow_fast, wall_restriction, safe_movement_distance, safe_movement_distance + radius, offset_independant_faster ? 2 : 1);
                        }
#ifdef TREESUPPORT_DEBUG_SVG
                        SVG::export_expolygons(debug_out_path("treesupport-increase_areas_one_layer-fast-%d-%ld.svg", layer_idx, int(merging_area_idx)),
                            { { { union_ex(wall_restriction) }, { "wall_restricrictions", "gray", 0.5f } },
                              { { union_ex(offset_fast) },      { "offset_fast", "red",  "black", "", scaled<coord_t>(0.1f), 0.5f } } });
#endif // TREESUPPORT_DEBUG_SVG
                    }
                }
                std::optional<SupportElementState> result;
                inc_wo_collision.clear();
                if (!settings.no_error) {
                    // ERROR CASE
                    // if the area becomes for whatever reason something that clipper sees as a line, offset would stop working, so ensure that even if it would be a line wrongly, it still actually has an area that can be increased
                    Polygons lines_offset = offset(to_polylines(parent.influence_area), scaled<float>(0.005), jtMiter, 1.2);
                    Polygons base_error_area = union_(parent.influence_area, lines_offset);
                    result = increase_single_area(volumes, config, settings, layer_idx, parent,
                        base_error_area, to_bp_data, to_model_data, inc_wo_collision, (config.maximum_move_distance + extra_speed) * 1.5, mergelayer);
#ifdef TREE_SUPPORT_SHOW_ERRORS
                    BOOST_LOG_TRIVIAL(error)
#else // TREE_SUPPORT_SHOW_ERRORS
                    BOOST_LOG_TRIVIAL(warning)
#endif // TREE_SUPPORT_SHOW_ERRORS
                          << "Influence area could not be increased! Data about the Influence area: "
                             "Radius: " << radius << " at layer: " << layer_idx - 1 << " NextTarget: " << elem.layer_idx << " Distance to top: " << elem.distance_to_top <<
                             " Elephant foot increases " << elem.elephant_foot_increases << " use_min_xy_dist " << elem.use_min_xy_dist << " to buildplate " << elem.to_buildplate <<
                             " gracious " << elem.to_model_gracious << " safe " << elem.can_use_safe_radius << " until move " << elem.dont_move_until << " \n "
                             "Parent " << &parent << ": Radius: " << support_element_collision_radius(config, parent.state) << " at layer: " << layer_idx << " NextTarget: " << parent.state.layer_idx <<
                             " Distance to top: " << parent.state.distance_to_top << " Elephant foot increases " << parent.state.elephant_foot_increases << "  use_min_xy_dist " << parent.state.use_min_xy_dist <<
                             " to buildplate " << parent.state.to_buildplate << " gracious " << parent.state.to_model_gracious << " safe " << parent.state.can_use_safe_radius << " until move " << parent.state.dont_move_until;
                    tree_supports_show_error("Potentially lost branch!"sv, true);
#ifdef TREE_SUPPORTS_TRACK_LOST
                    if (result)
                        result->lost = true;
#endif // TREE_SUPPORTS_TRACK_LOST
                } else
                    result = increase_single_area(volumes, config, settings, layer_idx, parent,
                        settings.increase_speed == slow_speed ? offset_slow : offset_fast, to_bp_data, to_model_data, inc_wo_collision, 0, mergelayer);

                if (result) {
                    elem = *result;
                    radius = support_element_collision_radius(config, elem);
                    elem.last_area_increase = settings;
                    add = true;
                    // do not merge if the branch should not move or the priority has to be to get farther away from the model.
                    bypass_merge = !settings.move || (settings.use_min_distance && elem.distance_to_top < config.tip_layers);
                    if (settings.move)
                        elem.dont_move_until = 0;
                    else
                        elem.result_on_layer = parent.state.result_on_layer;

                    elem.can_use_safe_radius = settings.type != AvoidanceType::Fast;

                    if (!settings.use_min_distance)
                        elem.use_min_xy_dist = false;
                    if (!settings.no_error)
#ifdef TREE_SUPPORT_SHOW_ERRORS
                        BOOST_LOG_TRIVIAL(error)
#else // TREE_SUPPORT_SHOW_ERRORS
                        BOOST_LOG_TRIVIAL(info)
#endif // TREE_SUPPORT_SHOW_ERRORS
                            << "Trying to keep area by moving faster than intended: Success";
                    break;
                } else if (!settings.no_error)
                    BOOST_LOG_TRIVIAL(warning) << "Trying to keep area by moving faster than intended: FAILURE! WRONG BRANCHES LIKLY!";
            }

            if (add) {
                // Union seems useless, but some rounding errors somewhere can cause to_bp_data to be slightly bigger than it should be.
                assert(! inc_wo_collision.empty() || ! to_bp_data.empty() || ! to_model_data.empty());
                Polygons max_influence_area = safe_union(
                    diff_clipped(inc_wo_collision, volumes.getCollision(radius, layer_idx - 1, elem.use_min_xy_dist)),
                    safe_union(to_bp_data, to_model_data));
                merging_area.state = elem;
                assert(!max_influence_area.empty());
                merging_area.set_bbox(get_extents(max_influence_area));
                merging_area.areas.influence_areas = std::move(max_influence_area);
                if (! bypass_merge) {
                    if (elem.to_buildplate)
                        merging_area.areas.to_bp_areas = std::move(to_bp_data);
                    if (config.support_rests_on_model)
                        merging_area.areas.to_model_areas = std::move(to_model_data);
                }
            } else {
                // If the bottom most point of a branch is set, later functions will assume that the position is valid, and ignore it.
                // But as branches connecting with the model that are to small have to be culled, the bottom most point has to be not set.
                // A point can be set on the top most tip layer (maybe more if it should not move for a few layers).
                parent.state.result_on_layer_reset();
                parent.state.to_model_gracious = false;
#ifdef TREE_SUPPORTS_TRACK_LOST
                parent.state.verylost = true;
#endif // TREE_SUPPORTS_TRACK_LOST
            }

            throw_on_cancel();
        }
    }, tbb::simple_partitioner());
}

[[nodiscard]] static SupportElementState merge_support_element_states(
    const SupportElementState &first, const SupportElementState &second, const Point &next_position, const coord_t layer_idx,
    const TreeSupportSettings &config)
{
    SupportElementState out;
    out.next_position   = next_position;
    out.layer_idx       = layer_idx;
    out.use_min_xy_dist = first.use_min_xy_dist || second.use_min_xy_dist;
    out.supports_roof   = first.supports_roof || second.supports_roof;
    out.dont_move_until = std::max(first.dont_move_until, second.dont_move_until);
    out.can_use_safe_radius = first.can_use_safe_radius || second.can_use_safe_radius;
    out.missing_roof_layers = std::min(first.missing_roof_layers, second.missing_roof_layers);
    out.skip_ovalisation = false;
    if (first.target_height > second.target_height) {
        out.target_height   = first.target_height;
        out.target_position = first.target_position;
    } else {
        out.target_height   = second.target_height;
        out.target_position = second.target_position;
    }
    out.effective_radius_height = std::max(first.effective_radius_height, second.effective_radius_height);
    out.distance_to_top = std::max(first.distance_to_top, second.distance_to_top);

    out.to_buildplate = first.to_buildplate && second.to_buildplate;
    out.to_model_gracious = first.to_model_gracious && second.to_model_gracious; // valid as we do not merge non-gracious with gracious

    out.elephant_foot_increases = 0;
    if (config.bp_radius_increase_per_layer > 0) {
        coord_t foot_increase_radius = std::abs(std::max(support_element_collision_radius(config, second), support_element_collision_radius(config, first)) - support_element_collision_radius(config, out));
        // elephant_foot_increases has to be recalculated, as when a smaller tree with a larger elephant_foot_increases merge with a larger branch
        // the elephant_foot_increases may have to be lower as otherwise the radius suddenly increases. This results often in a non integer value.
        out.elephant_foot_increases = foot_increase_radius / (config.bp_radius_increase_per_layer - config.branch_radius_increase_per_layer);
    }

    // set last settings to the best out of both parents. If this is wrong, it will only cause a small performance penalty instead of weird behavior.
    out.last_area_increase = {
        std::min(first.last_area_increase.type, second.last_area_increase.type),
        std::min(first.last_area_increase.increase_speed, second.last_area_increase.increase_speed),
        first.last_area_increase.increase_radius || second.last_area_increase.increase_radius,
        first.last_area_increase.no_error || second.last_area_increase.no_error,
        first.last_area_increase.use_min_distance && second.last_area_increase.use_min_distance,
        first.last_area_increase.move || second.last_area_increase.move };

    return out;
}

static bool merge_influence_areas_two_elements(
    const TreeModelVolumes &volumes, const TreeSupportSettings &config, const LayerIndex layer_idx,
    SupportElementMerging &dst, SupportElementMerging &src)
{
    // Don't merge gracious with a non gracious area as bad placement could negatively impact reliability of the whole subtree.
    const bool merging_gracious_and_non_gracious = dst.state.to_model_gracious != src.state.to_model_gracious;
    // Could cause some issues with the increase of one area, as it is assumed that if the smaller is increased
    // by the delta to the larger it is engulfed by it already. But because a different collision
    // may be removed from the in draw_area() generated circles, this assumption could be wrong.
    const bool merging_min_and_regular_xy        = dst.state.use_min_xy_dist != src.state.use_min_xy_dist;

    if (merging_gracious_and_non_gracious || merging_min_and_regular_xy)
        return false;

    const bool dst_radius_bigger = support_element_collision_radius(config, dst.state) > support_element_collision_radius(config, src.state);
    const SupportElementMerging &smaller_rad = dst_radius_bigger ? src : dst;
    const SupportElementMerging &bigger_rad  = dst_radius_bigger ? dst : src;
    const coord_t real_radius_delta = std::abs(support_element_radius(config, bigger_rad.state) - support_element_radius(config, smaller_rad.state));
    {
        // Testing intersection of bounding boxes.
        // Expand the smaller radius branch bounding box to match the lambda intersect_small_with_bigger() below.
        // Because the lambda intersect_small_with_bigger() applies a rounded offset, a snug offset of the bounding box
        // is sufficient. On the other side, if a mitered offset was used by the lambda,
        // the bounding box expansion would have to account for the mitered extension of the sharp corners.
        Eigen::AlignedBox<coord_t, 2> smaller_bbox = smaller_rad.bbox();
        smaller_bbox.min() -= Point{ real_radius_delta, real_radius_delta };
        smaller_bbox.max() += Point{ real_radius_delta, real_radius_delta };
        if (! smaller_bbox.intersects(bigger_rad.bbox()))
            return false;
    }

    // Accumulator of a radius increase of a "to model" branch by merging in a "to build plate" branch.
    coord_t increased_to_model_radius = 0;
    const bool merging_to_bp                     = dst.state.to_buildplate && src.state.to_buildplate;
    if (! merging_to_bp) {
        // Get the real radius increase as the user does not care for the collision model.
        if (dst.state.to_buildplate != src.state.to_buildplate) {
            // Merging a "to build plate" branch with a "to model" branch.
            // Don't allow merging a thick "to build plate" branch into a thinner "to model" branch.
            const coord_t rdst = support_element_radius(config, dst.state);
            const coord_t rsrc = support_element_radius(config, src.state);
            if (dst.state.to_buildplate) {
                if (rsrc < rdst)
                    increased_to_model_radius = src.state.increased_to_model_radius + rdst - rsrc;
            } else {
                if (rsrc > rdst)
                    increased_to_model_radius = dst.state.increased_to_model_radius + rsrc - rdst;
            }
            if (increased_to_model_radius > config.max_to_model_radius_increase)
                return false;
        }
        // if a merge could place a stable branch on unstable ground, would be increasing the radius further
        // than allowed to when merging to model and to_bp trees or would merge to model before it is known
        // they will even been drawn the merge is skipped
        if (! dst.state.supports_roof && ! src.state.supports_roof &&
            std::max(src.state.distance_to_top, dst.state.distance_to_top) < config.min_dtt_to_model)
            return false;
    }

    // Area of the bigger radius is used to ensure correct placement regarding the relevant avoidance,
    // so if that would change an invalid area may be created.
    if (! bigger_rad.state.can_use_safe_radius && smaller_rad.state.can_use_safe_radius)
        return false;

    // the bigger radius is used to verify that the area is still valid after the increase with the delta.
    // If there were a point where the big influence area could be valid with can_use_safe_radius
    // the element would already be can_use_safe_radius.
    // the smaller radius, which gets increased by delta may reach into the area where use_min_xy_dist is no longer required.
    const bool use_min_radius = bigger_rad.state.use_min_xy_dist && smaller_rad.state.use_min_xy_dist;

    // The idea is that the influence area with the smaller collision radius is increased by the radius difference.
    // If this area has any intersections with the influence area of the larger collision radius, a branch (of the larger collision radius) placed in this intersection, has already engulfed the branch of the smaller collision radius.
    // Because of this a merge may happen even if the influence areas (that represent possible center points of branches) do not intersect yet.
    // Remember that collision radius <= real radius as otherwise this assumption would be false.
    const coord_t   smaller_collision_radius    = support_element_collision_radius(config, smaller_rad.state);
    const Polygons &collision                   = volumes.getCollision(smaller_collision_radius, layer_idx - 1, use_min_radius);
    auto            intersect_small_with_bigger = [real_radius_delta, smaller_collision_radius, &collision, &config](const Polygons &small, const Polygons &bigger) {
        return intersection(
            safe_offset_inc(
                small, real_radius_delta, collision,
                // -3 avoids possible rounding errors
                2 * (config.xy_distance + smaller_collision_radius - 3), 0, 0),
            bigger);
    };
//#define TREES_MERGE_RATHER_LATER
    Polygons intersect = 
#ifdef TREES_MERGE_RATHER_LATER
        intersection(
#else
        intersect_small_with_bigger(            
#endif
        merging_to_bp ? smaller_rad.areas.to_bp_areas : smaller_rad.areas.to_model_areas,
        merging_to_bp ? bigger_rad.areas.to_bp_areas : bigger_rad.areas.to_model_areas);

    const auto _tiny_area_threshold = tiny_area_threshold();
    // dont use empty as a line is not empty, but for this use-case it very well may be (and would be one layer down as union does not keep lines)
    // check if the overlap is large enough (Small ares tend to attract rounding errors in clipper).
    if (area(intersect) <= _tiny_area_threshold)
        return false;

    // While 0.025 was guessed as enough, i did not have reason to change it.
    if (area(offset(intersect, scaled<float>(-0.025), jtMiter, 1.2)) <= _tiny_area_threshold)
        return false;

#ifdef TREES_MERGE_RATHER_LATER
    intersect = 
        intersect_small_with_bigger(
            merging_to_bp ? smaller_rad.areas.to_bp_areas : smaller_rad.areas.to_model_areas,
            merging_to_bp ? bigger_rad.areas.to_bp_areas : bigger_rad.areas.to_model_areas);
#endif

    // Do the actual merge now that the branches are confirmed to be able to intersect.
    // calculate which point is closest to the point of the last merge (or tip center if no merge above it has happened)
    // used at the end to estimate where to best place the branch on the bottom most layer
    // could be replaced with a random point inside the new area
    Point new_pos = move_inside_if_outside(intersect, dst.state.next_position);

    SupportElementState new_state = merge_support_element_states(dst.state, src.state, new_pos, layer_idx - 1, config);
    new_state.increased_to_model_radius = increased_to_model_radius == 0 ?
        // increased_to_model_radius was not set yet. Propagate maximum.
        std::max(dst.state.increased_to_model_radius, src.state.increased_to_model_radius) :
        increased_to_model_radius;

    // Rather unioning with "intersect" due to some rounding errors.
    Polygons influence_areas = safe_union(
        intersect_small_with_bigger(smaller_rad.areas.influence_areas, bigger_rad.areas.influence_areas),
        intersect);

    Polygons to_model_areas;
    if (merging_to_bp && config.support_rests_on_model)
        to_model_areas = new_state.to_model_gracious ?
            // Rather unioning with "intersect" due to some rounding errors.
            safe_union(
                intersect_small_with_bigger(smaller_rad.areas.to_model_areas, bigger_rad.areas.to_model_areas),
                intersect) :
            influence_areas;

    dst.parents.insert(dst.parents.end(), src.parents.begin(), src.parents.end());
    dst.state = new_state;
    dst.areas.influence_areas = std::move(influence_areas);
    dst.areas.to_bp_areas.clear();
    dst.areas.to_model_areas.clear();
    if (merging_to_bp) {
        dst.areas.to_bp_areas = std::move(intersect);
        if (config.support_rests_on_model)
            dst.areas.to_model_areas = std::move(to_model_areas);
    } else
        dst.areas.to_model_areas = std::move(intersect);
    // Update the bounding box.
    BoundingBox bbox(get_extents(dst.areas.influence_areas));
    bbox.merge(get_extents(dst.areas.to_bp_areas));
    bbox.merge(get_extents(dst.areas.to_model_areas));
    dst.set_bbox(bbox);
    // Clear the source data.
    src.areas.clear();
    src.parents.clear();
    return true;
}

/*!
 * \brief Merges Influence Areas if possible.
 *
 * Branches which do overlap have to be merged. This helper merges all elements in input with the elements into reduced_new_layer.
 * Elements in input_aabb are merged together if possible, while elements reduced_new_layer_aabb are not checked against each other.
 *
 * \param reduced_aabb[in,out] The already processed elements.
 * \param input_aabb[in] Not yet processed elements
 * \param to_bp_areas[in] The Elements of the current Layer that will reach the buildplate. Value is the influence area where the center of a circle of support may be placed.
 * \param to_model_areas[in] The Elements of the current Layer that do not have to reach the buildplate. Also contains main as every element that can reach the buildplate is not forced to.
 * Value is the influence area where the center of a circle of support may be placed.
 * \param influence_areas[in] The influence areas without avoidance removed.
 * \param insert_bp_areas[out] Elements to be inserted into the main dictionary after the Helper terminates.
 * \param insert_model_areas[out] Elements to be inserted into the secondary dictionary after the Helper terminates.
 * \param insert_influence[out] Elements to be inserted into the dictionary containing the largest possibly valid influence area (ignoring if the area may not be there because of avoidance)
 * \param erase[out] Elements that should be deleted from the above dictionaries.
 * \param layer_idx[in] The Index of the current Layer.
 */

static SupportElementMerging* merge_influence_areas_leaves(
    const TreeModelVolumes &volumes, const TreeSupportSettings &config, const LayerIndex layer_idx,
    SupportElementMerging * const dst_begin, SupportElementMerging *dst_end)
{
    // Merging at the lowest level of the AABB tree. Checking one against each other, O(n^2).
    assert(dst_begin < dst_end);
    for (SupportElementMerging *i = dst_begin; i + 1 < dst_end;) {
        for (SupportElementMerging *j = i + 1; j != dst_end;)
            if (merge_influence_areas_two_elements(volumes, config, layer_idx, *i, *j)) {
                // i was merged with j, j is empty.
                if (j != -- dst_end)
                    *j = std::move(*dst_end);
                goto merged;
            } else
                ++ j;
        // not merged
        ++ i;
    merged:
        ;
    }
    return dst_end;
}

static SupportElementMerging* merge_influence_areas_two_sets(
    const TreeModelVolumes &volumes, const TreeSupportSettings &config, const LayerIndex layer_idx,
    SupportElementMerging * const dst_begin, SupportElementMerging *       dst_end,
    SupportElementMerging *       src_begin, SupportElementMerging * const src_end)
{
    // Merging src into dst.
    // Areas of src should not overlap with areas of another elements of src.
    // Areas of dst should not overlap with areas of another elements of dst.
    // The memory from dst_begin to src_end is reserved for the merging operation,
    // src follows dst.
    assert(src_begin < src_end);
    assert(dst_begin < dst_end);
    assert(dst_end <= src_begin);
    for (SupportElementMerging *src = src_begin; src != src_end; ++ src) {
        SupportElementMerging         *dst      = dst_begin;
        SupportElementMerging         *merged   = nullptr;
        for (; dst != dst_end; ++ dst)
            if (merge_influence_areas_two_elements(volumes, config, layer_idx, *dst, *src)) {
                merged = dst ++;
                if (src != src_begin)
                    // Compactify src.
                    *src = std::move(*src_begin);
                ++ src_begin;
                break;
            }
        for (; dst != dst_end;)
            if (merge_influence_areas_two_elements(volumes, config, layer_idx, *merged, *dst)) {
                // Compactify dst.
                if (dst != -- dst_end)
                    *dst = std::move(*dst_end);
            } else
                ++ dst;
    }
    // Compactify src elements that were not merged with dst to the end of dst.
    assert(dst_end <= src_begin);
    if (dst_end == src_begin)
        dst_end = src_end;
    else
        while (src_begin != src_end)
            *dst_end ++ = std::move(*src_begin ++);

    return dst_end;
}

/*!
 * \brief Merges Influence Areas at one layer if possible.
 *
 * Branches which do overlap have to be merged. This manages the helper and uses a divide and conquer approach to parallelize this problem. This parallelization can at most accelerate the merging by a factor of 2.
 *
 * \param to_bp_areas[in] The Elements of the current Layer that will reach the buildplate.
 *  Value is the influence area where the center of a circle of support may be placed.
 * \param to_model_areas[in] The Elements of the current Layer that do not have to reach the buildplate. Also contains main as every element that can reach the buildplate is not forced to.
 *  Value is the influence area where the center of a circle of support may be placed.
 * \param influence_areas[in] The Elements of the current Layer without avoidances removed. This is the largest possible influence area for this layer.
 *  Value is the influence area where the center of a circle of support may be placed.
 * \param layer_idx[in] The current layer.
 */
static void merge_influence_areas(
    const TreeModelVolumes             &volumes,
    const TreeSupportSettings          &config,
    const LayerIndex                    layer_idx,
    std::vector<SupportElementMerging> &influence_areas,
    std::function<void()>               throw_on_cancel)
{
    const size_t input_size = influence_areas.size();
    if (input_size == 0)
        return;

    // Merging by divide & conquer.
    // The majority of time is consumed by Clipper polygon operations, intersection is accelerated by bounding boxes.
    // Sorting input into an AABB tree helps to perform most of the intersections at first iterations,
    // thus reducing computation when merging larger subtrees.
    // The actual merge logic is found in merge_influence_areas_two_sets.

    // Build an AABB tree over the influence areas.
    //FIXME A full tree does not need to be built, the lowest level branches will be always bucketed.
    // However the additional time consumed is negligible.
    AABBTreeIndirect::Tree<2, coord_t> tree;
    // Sort influence_areas in place.
    tree.build_modify_input(influence_areas);

    throw_on_cancel();

    // Prepare the initial buckets as ranges of influence areas. The initial buckets contain power of 2 influence areas to follow
    // the branching of the AABB tree.
    // Vectors of ranges of influence areas, following the branching of the AABB tree:
    std::vector<std::pair<SupportElementMerging*, SupportElementMerging*>> buckets;
    // Initial number of buckets for 1st round of merging.
    size_t num_buckets_initial;
    {
        // How many buckets per first merge iteration?
        const size_t num_threads     = tbb::this_task_arena::max_concurrency();
        // 4 buckets per thread if possible,
        const size_t num_buckets_min = (input_size + 2) / 4;
        // 2 buckets per thread otherwise.
        const size_t num_buckets_max = input_size / 2;
        num_buckets_initial          = num_buckets_min >= num_threads ? num_buckets_min : num_buckets_max;
        const size_t bucket_size     = num_buckets_min >= num_threads ? 4 : 2;
        // Fill in the buckets.
        SupportElementMerging *it = influence_areas.data();
        // Reserve one more bucket to keep a single influence area which will not be merged in the first iteration.
        buckets.reserve(num_buckets_initial + 1);
        for (size_t i = 0; i < num_buckets_initial; ++ i, it += bucket_size)
            buckets.emplace_back(std::make_pair(it, it + bucket_size));
        SupportElementMerging *it_end = influence_areas.data() + influence_areas.size();
        if (buckets.back().second >= it_end) {
            // Last bucket is less than size 4, but bigger than size 1.
            buckets.back().second = std::min(buckets.back().second, it_end);
        } else {
            // Last bucket is size 1, it will not be merged in the first iteration.
            assert(it + 1 == it_end);
            buckets.emplace_back(std::make_pair(it, it_end));
        }
    }

    // 1st merge iteration, merge one with each other.
    tbb::parallel_for(tbb::blocked_range<size_t>(0, num_buckets_initial),
        [&](const tbb::blocked_range<size_t> &range) {
        for (size_t idx = range.begin(); idx < range.end(); ++ idx) {
            // Merge bucket_count adjacent to each other, merging uneven bucket numbers into even buckets
            buckets[idx].second = merge_influence_areas_leaves(volumes, config, layer_idx, buckets[idx].first, buckets[idx].second);
            throw_on_cancel();
        }
    });

    // Further merge iterations, merging one AABB subtree with another one, hopefully minimizing intersections between the elements
    // of each of the subtree.
    while (buckets.size() > 1) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, buckets.size() / 2),
            [&](const tbb::blocked_range<size_t> &range) {
            for (size_t idx = range.begin(); idx < range.end(); ++ idx) {
                const size_t bucket_pair_idx = idx * 2;
                // Merge bucket_count adjacent to each other, merging uneven bucket numbers into even buckets
                buckets[bucket_pair_idx].second = merge_influence_areas_two_sets(volumes, config, layer_idx,
                    buckets[bucket_pair_idx].first, buckets[bucket_pair_idx].second,
                    buckets[bucket_pair_idx + 1].first, buckets[bucket_pair_idx + 1].second);
                throw_on_cancel();
            }
        });
        // Remove odd buckets, which were merged into even buckets.
        size_t new_size = (buckets.size() + 1) / 2;
        for (size_t i = 1; i < new_size; ++ i)
            buckets[i] = std::move(buckets[i * 2]);
        buckets.erase(buckets.begin() + new_size, buckets.end());
    }
}

/*!
 * \brief Propagates influence downwards, and merges overlapping ones.
 *
 * \param move_bounds[in,out] All currently existing influence areas
 */
static void create_layer_pathing(const TreeModelVolumes &volumes, const TreeSupportSettings &config, std::vector<SupportElements> &move_bounds, std::function<void()> throw_on_cancel)
{
#ifdef SLIC3R_TREESUPPORTS_PROGRESS
    const double data_size_inverse = 1 / double(move_bounds.size());
    double progress_total = TREE_PROGRESS_PRECALC_AVO + TREE_PROGRESS_PRECALC_COLL + TREE_PROGRESS_GENERATE_NODES;
#endif // SLIC3R_TREESUPPORTS_PROGRESS

    auto dur_inc   = std::chrono::duration_values<std::chrono::nanoseconds>::zero();
    auto dur_total = std::chrono::duration_values<std::chrono::nanoseconds>::zero();

    LayerIndex last_merge_layer_idx = move_bounds.size();
    bool new_element = false;
    const auto _tiny_area_threshold = tiny_area_threshold();

    // Ensures at least one merge operation per 3mm height, 50 layers, 1 mm movement of slow speed or 5mm movement of fast speed (whatever is lowest). Values were guessed.
    size_t max_merge_every_x_layers = std::min(std::min(5000 / (std::max(config.maximum_move_distance, coord_t(100))), 1000 / std::max(config.maximum_move_distance_slow, coord_t(20))), 3000 / config.layer_height);
    size_t merge_every_x_layers = 1;
    // Calculate the influence areas for each layer below (Top down)
    // This is done by first increasing the influence area by the allowed movement distance, and merging them with other influence areas if possible
    for (int layer_idx = int(move_bounds.size()) - 1; layer_idx > 0; -- layer_idx)
        if (SupportElements &prev_layer = move_bounds[layer_idx]; ! prev_layer.empty()) {
            // merging is expensive and only parallelized to a max speedup of 2. As such it may be useful in some cases to only merge every few layers to improve performance.
            bool had_new_element = new_element;
            const bool merge_this_layer = had_new_element || size_t(last_merge_layer_idx - layer_idx) >= merge_every_x_layers;
            if (had_new_element)
                merge_every_x_layers = 1;
            const auto ta               = std::chrono::high_resolution_clock::now();

            // ### Increase the influence areas by the allowed movement distance
            std::vector<SupportElementMerging> influence_areas;
            influence_areas.reserve(prev_layer.size());
            for (int32_t element_idx = 0; element_idx < int32_t(prev_layer.size()); ++ element_idx) {
                SupportElement &el = prev_layer[element_idx];
                assert(!el.influence_area.empty());
                SupportElement::ParentIndices parents;
                parents.emplace_back(element_idx);
                influence_areas.push_back({ el.state, parents });
            }
            increase_areas_one_layer(volumes, config, influence_areas, layer_idx, prev_layer, merge_this_layer, throw_on_cancel);

            // Place already fully constructed elements to the output, remove them from influence_areas.
            SupportElements &this_layer = move_bounds[layer_idx - 1];
            influence_areas.erase(std::remove_if(influence_areas.begin(), influence_areas.end(),
                [&this_layer, &_tiny_area_threshold, layer_idx](SupportElementMerging &elem) {
                    if (elem.areas.influence_areas.empty())
                        // This area was removed completely due to collisions.
                        return true;
                    if (elem.areas.to_bp_areas.empty() && elem.areas.to_model_areas.empty()) {
                        if (area(elem.areas.influence_areas) < _tiny_area_threshold) {
                            BOOST_LOG_TRIVIAL(warning) << "Insert Error of Influence area bypass on layer " << layer_idx - 1;
                            tree_supports_show_error("Insert error of area after bypassing merge.\n"sv, true);
                        }
                        // Move the area to output.
                        this_layer.emplace_back(elem.state, std::move(elem.parents), std::move(elem.areas.influence_areas));
                        return true;
                    }
                    // Keep the area.
                    return false;
                }),
                influence_areas.end());

            dur_inc += std::chrono::high_resolution_clock::now() - ta;
            new_element = ! move_bounds[layer_idx - 1].empty();
            if (merge_this_layer) {
                bool reduced_by_merging = false;
                if (size_t count_before_merge = influence_areas.size(); count_before_merge > 1) {
                    // ### Calculate which influence areas overlap, and merge them into a new influence area (simplified: an intersection of influence areas that have such an intersection)
                    merge_influence_areas(volumes, config, layer_idx, influence_areas, throw_on_cancel);
                    reduced_by_merging = count_before_merge > influence_areas.size();
                }
                last_merge_layer_idx = layer_idx;
                if (! reduced_by_merging && ! had_new_element)
                    merge_every_x_layers = std::min(max_merge_every_x_layers, merge_every_x_layers + 1);
            }

            dur_total += std::chrono::high_resolution_clock::now() - ta;

            // Save calculated elements to output, and allocate Polygons on heap, as they will not be changed again.
            for (SupportElementMerging &elem : influence_areas)
                if (! elem.areas.influence_areas.empty()) {
                    Polygons new_area = safe_union(elem.areas.influence_areas);
                    if (area(new_area) < _tiny_area_threshold) {
                        BOOST_LOG_TRIVIAL(warning) << "Insert Error of Influence area on layer " << layer_idx - 1 << ". Origin of " << elem.parents.size() << " areas. Was to bp " << elem.state.to_buildplate;
                        tree_supports_show_error("Insert error of area after merge.\n"sv, true);
                    }
                    this_layer.emplace_back(elem.state, std::move(elem.parents), std::move(new_area));
                }

    #ifdef SLIC3R_TREESUPPORTS_PROGRESS
            progress_total += data_size_inverse * TREE_PROGRESS_AREA_CALC;
            Progress::messageProgress(Progress::Stage::SUPPORT, progress_total * m_progress_multiplier + m_progress_offset, TREE_PROGRESS_TOTAL);
    #endif
            throw_on_cancel();
        }

    BOOST_LOG_TRIVIAL(info) << "Time spent with creating influence areas' subtasks: Increasing areas " << dur_inc.count() / 1000000 <<
        " ms merging areas: " << (dur_total - dur_inc).count() / 1000000 << " ms";
}

/*!
 * \brief Sets the result_on_layer for all parents based on the SupportElement supplied.
 *
 * \param elem[in] The SupportElements, which parent's position should be determined.
 */
static void set_points_on_areas(const SupportElement &elem, SupportElements *layer_above)
{
    assert(!elem.state.deleted);
    assert(layer_above != nullptr || elem.parents.empty());

    // Based on the branch center point of the current layer, the point on the next (further up) layer is calculated.
    if (! elem.state.result_on_layer_is_set()) {
        BOOST_LOG_TRIVIAL(warning) << "Uninitialized support element";
        tree_supports_show_error("Uninitialized support element. A branch may be missing.\n"sv, true);
        return;
    }

    if (layer_above)
        for (int32_t next_elem_idx : elem.parents) {
            assert(next_elem_idx >= 0);
            SupportElement &next_elem = (*layer_above)[next_elem_idx];
            assert(! next_elem.state.deleted);
            // if the value was set somewhere else it it kept. This happens when a branch tries not to move after being unable to create a roof.
            if (! next_elem.state.result_on_layer_is_set()) {
                // Move inside has edgecases (see tests) so DONT use Polygons.inside to confirm correct move, Error with distance 0 is <= 1
                // it is not required to check if how far this move moved a point as is can be larger than maximum_movement_distance.
                // While this seems like a problem it may for example occur after merges.
                next_elem.state.result_on_layer = move_inside_if_outside(next_elem.influence_area, elem.state.result_on_layer);
                // do not call recursive because then amount of layers would be restricted by the stack size
            }
            // Mark the parent element as accessed from a valid child element.
            next_elem.state.marked = true;
        }
}

static void set_to_model_contact_simple(SupportElement &elem)
{
    const Point best = move_inside_if_outside(elem.influence_area, elem.state.next_position);
    elem.state.result_on_layer = best;
    BOOST_LOG_TRIVIAL(debug) << "Added NON gracious Support On Model Point (" << best.x() << "," << best.y() << "). The current layer is " << elem.state.layer_idx;
}

/*!
 * \brief Get the best point to connect to the model and set the result_on_layer of the relevant SupportElement accordingly.
 *
 * \param move_bounds[in,out] All currently existing influence areas
 * \param first_elem[in,out] SupportElement that did not have its result_on_layer set meaning that it does not have a child element.
 * \param layer_idx[in] The current layer.
 */
static void set_to_model_contact_to_model_gracious(
    const TreeModelVolumes          &volumes,
    const TreeSupportSettings       &config,
    std::vector<SupportElements>    &move_bounds,
    SupportElement                  &first_elem,
    std::function<void()>            throw_on_cancel)
{
    SupportElement *last_successfull_layer = nullptr;

    // check for every layer upwards, up to the point where this influence area was created (either by initial insert or merge) if the branch could be placed on it, and highest up layer index.
    {
        SupportElement *elem = &first_elem;
        for (LayerIndex layer_check = elem->state.layer_idx;
            ! intersection(elem->influence_area, volumes.getPlaceableAreas(support_element_collision_radius(config, elem->state), layer_check, throw_on_cancel)).empty();
            elem = &move_bounds[++ layer_check][elem->parents.front()]) {
            assert(elem->state.layer_idx == layer_check);
            assert(! elem->state.deleted);
            assert(elem->state.to_model_gracious);
            last_successfull_layer = elem;
            if (elem->parents.size() != 1)
                // Reached merge point.
                break;
        }
    }

    // Could not find valid placement, even though it should exist => error handling
    if (last_successfull_layer == nullptr) {
        BOOST_LOG_TRIVIAL(warning) << "No valid placement found for to model gracious element on layer " << first_elem.state.layer_idx;
        tree_supports_show_error("Could not fine valid placement on model! Just placing it down anyway. Could cause floating branches."sv, true);
        first_elem.state.to_model_gracious = false;
        set_to_model_contact_simple(first_elem);
    } else {
        // Found a gracious area above first_elem. Remove all below last_successfull_layer.
        {
            LayerIndex parent_layer_idx = first_elem.state.layer_idx;
            for (SupportElement *elem = &first_elem; elem != last_successfull_layer; elem = &move_bounds[++ parent_layer_idx][elem->parents.front()]) {
                assert(! elem->state.deleted);
                elem->state.deleted = true;
            }
        }
        // Guess a point inside the influence area, in which the branch will be placed in.
        const Point best = move_inside_if_outside(last_successfull_layer->influence_area, last_successfull_layer->state.next_position);
        last_successfull_layer->state.result_on_layer = best;
        BOOST_LOG_TRIVIAL(debug) << "Added gracious Support On Model Point (" << best.x() << "," << best.y() << "). The current layer is " << last_successfull_layer;
    }
}

// Remove elements marked as "deleted", update indices to parents.
static void remove_deleted_elements(std::vector<SupportElements> &move_bounds)
{
    std::vector<int32_t> map_parents;
    std::vector<int32_t> map_current;
    for (LayerIndex layer_idx = LayerIndex(move_bounds.size()) - 1; layer_idx >= 0; -- layer_idx) {
        SupportElements &layer = move_bounds[layer_idx];
        map_current.clear();
        for (int32_t i = 0; i < int32_t(layer.size());) {
            SupportElement &element = layer[i];
            if (element.state.deleted) {
                if (map_current.empty()) {
                    // Initialize with identity map.
                    map_current.assign(layer.size(), 0);
                    std::iota(map_current.begin(), map_current.end(), 0);
                }
                // Delete all "deleted" elements from the end of the layer vector.
                while (i < int32_t(layer.size()) && layer.back().state.deleted) {
                    layer.pop_back();
                    // Mark as deleted in the map.
                    map_current[layer.size()] = -1;
                }
                assert(i == layer.size() || i + 1 < layer.size());
                if (i + 1 < int32_t(layer.size())) {
                    element = std::move(layer.back());
                    layer.pop_back();
                    // Mark the current element as deleted.
                    map_current[i] = -1;
                    // Mark the moved element as moved to index i.
                    map_current[layer.size()] = i;
                }
            } else {
                // Current element is not deleted. Update its parent indices.
                if (! map_parents.empty())
                    for (int32_t &parent_idx : element.parents)
                        parent_idx = map_parents[parent_idx];
                ++ i;
            }
        }
        std::swap(map_current, map_parents);
    }
}

/*!
 * \brief Set the result_on_layer point for all influence areas
 *
 * \param move_bounds[in,out] All currently existing influence areas
 */
static void create_nodes_from_area(
    const TreeModelVolumes       &volumes,
    const TreeSupportSettings    &config,
    std::vector<SupportElements> &move_bounds,
    std::function<void()>         throw_on_cancel)
{
    // Initialize points on layer 0, with a "random" point in the influence area.
    // Point is chosen based on an inaccurate estimate where the branches will split into two, but every point inside the influence area would produce a valid result.
    {
        SupportElements *layer_above = move_bounds.size() > 1 ? &move_bounds[1] : nullptr;
        if (layer_above) {
            for (SupportElement &elem : *layer_above)
                elem.state.marked = false;
        }
        for (SupportElement &init : move_bounds.front()) {
            init.state.result_on_layer = move_inside_if_outside(init.influence_area, init.state.next_position);
            // Also set the parent nodes, as these will be required for the first iteration of the loop below and mark the parent nodes.
            set_points_on_areas(init, layer_above);
        }
    }

    throw_on_cancel();

    for (LayerIndex layer_idx = 1; layer_idx < LayerIndex(move_bounds.size()); ++ layer_idx) {
        auto &layer       = move_bounds[layer_idx];
        auto *layer_above = layer_idx + 1 < LayerIndex(move_bounds.size()) ? &move_bounds[layer_idx + 1] : nullptr;
        if (layer_above)
            for (SupportElement &elem : *layer_above)
                elem.state.marked = false;
        for (SupportElement &elem : layer) {
            assert(! elem.state.deleted);
            assert(elem.state.layer_idx == layer_idx);
            // check if the resulting center point is not yet set
            if (! elem.state.result_on_layer_is_set()) {
                if (elem.state.to_buildplate || (elem.state.distance_to_top < config.min_dtt_to_model && ! elem.state.supports_roof)) {
                    if (elem.state.to_buildplate) {
                        BOOST_LOG_TRIVIAL(warning) << "Uninitialized Influence area targeting " << elem.state.target_position.x() << "," << elem.state.target_position.y()
                                                   << ") "
                            "at target_height: " << elem.state.target_height << " layer: " << layer_idx;
                        tree_supports_show_error("Uninitialized support element! A branch could be missing or exist partially."sv, true);
                    }
                    // we dont need to remove yet the parents as they will have a lower dtt and also no result_on_layer set
                    elem.state.deleted = true;
                } else {
                    // set the point where the branch will be placed on the model
                    if (elem.state.to_model_gracious)
                        set_to_model_contact_to_model_gracious(volumes, config, move_bounds, elem, throw_on_cancel);
                    else
                        set_to_model_contact_simple(elem);
                }
            }
            if (! elem.state.deleted && ! elem.state.marked && elem.state.target_height == layer_idx)
                // Just a tip surface with no supporting element.
                elem.state.deleted = true;
            if (elem.state.deleted) {
                for (int32_t parent_idx : elem.parents)
                    // When the roof was not able to generate downwards enough, the top elements may have not moved, and have result_on_layer already set.
                    // As this branch needs to be removed => all parents result_on_layer have to be invalidated.
                    (*layer_above)[parent_idx].state.result_on_layer_reset();
            }
            if (! elem.state.deleted) {
                // Element is valid now setting points in the layer above and mark the parent nodes.
                set_points_on_areas(elem, layer_above);
            }
        }
        throw_on_cancel();
    }

#ifndef NDEBUG
    // Verify the tree connectivity including the branch slopes.
    for (LayerIndex layer_idx = 0; layer_idx + 1 < LayerIndex(move_bounds.size()); ++ layer_idx) {
        auto &layer = move_bounds[layer_idx];
        auto &above = move_bounds[layer_idx + 1];
        for (SupportElement &elem : layer)
            if (! elem.state.deleted) {
                for (int32_t iparent : elem.parents) {
                    SupportElement &parent = above[iparent];
                    assert(! parent.state.deleted);
                    assert(elem.state.result_on_layer_is_set() == parent.state.result_on_layer_is_set());
                    if (elem.state.result_on_layer_is_set()) {
                        double radius_increase = support_element_radius(config, elem) - support_element_radius(config, parent);
                        assert(radius_increase >= 0);
                        double shift = (elem.state.result_on_layer - parent.state.result_on_layer).cast<double>().norm();
                        //FIXME this assert fails a lot. Is it correct?
//                        assert(shift < radius_increase + 2. * config.maximum_move_distance_slow);
                    }
                }
            }
        }
#endif // NDEBUG

    remove_deleted_elements(move_bounds);

#ifndef NDEBUG
    // Verify the tree connectivity including the branch slopes.
    for (LayerIndex layer_idx = 0; layer_idx + 1 < LayerIndex(move_bounds.size()); ++ layer_idx) {
        auto &layer = move_bounds[layer_idx];
        auto &above = move_bounds[layer_idx + 1];
        for (SupportElement &elem : layer) {
            assert(! elem.state.deleted);
            for (int32_t iparent : elem.parents) {
                SupportElement &parent = above[iparent];
                assert(! parent.state.deleted);
                assert(elem.state.result_on_layer_is_set() == parent.state.result_on_layer_is_set());
                if (elem.state.result_on_layer_is_set()) {
                    double radius_increase = support_element_radius(config, elem) - support_element_radius(config, parent);
                    assert(radius_increase >= 0);
                    double shift = (elem.state.result_on_layer - parent.state.result_on_layer).cast<double>().norm();
                    //FIXME this assert fails a lot. Is it correct?
//                    assert(shift < radius_increase + 2. * config.maximum_move_distance_slow);
                }
            }
        }
    }
#endif // NDEBUG
}

template<bool flip_normals>
void triangulate_fan(indexed_triangle_set &its, int ifan, int ibegin, int iend)
{
    // at least 3 vertices, increasing order.
    assert(ibegin + 3 <= iend);
    assert(ibegin >= 0 && iend <= its.vertices.size());
    assert(ifan >= 0 && ifan < its.vertices.size());
    int num_faces = iend - ibegin;
    its.indices.reserve(its.indices.size() + num_faces * 3);
    for (int v = ibegin, u = iend - 1; v < iend; u = v ++) {
        if (flip_normals)
            its.indices.push_back({ ifan, u, v });
        else
            its.indices.push_back({ ifan, v, u });
    }
}

static void triangulate_strip(indexed_triangle_set &its, int ibegin1, int iend1, int ibegin2, int iend2)
{
    // at least 3 vertices, increasing order.
    assert(ibegin1 + 3 <= iend1);
    assert(ibegin1 >= 0 && iend1 <= its.vertices.size());
    assert(ibegin2 + 3 <= iend2);
    assert(ibegin2 >= 0 && iend2 <= its.vertices.size());
    int n1 = iend1 - ibegin1;
    int n2 = iend2 - ibegin2;
    its.indices.reserve(its.indices.size() + (n1 + n2) * 3);

    // For the first vertex of 1st strip, find the closest vertex on the 2nd strip.
    int istart2 = ibegin2;
    {
        const Vec3f &p1    = its.vertices[ibegin1];
        auto         d2min = std::numeric_limits<float>::max();
        for (int i = ibegin2; i < iend2; ++ i) {
            const Vec3f &p2 = its.vertices[i];
            const float d2  = (p2 - p1).squaredNorm();
            if (d2 < d2min) {
                d2min = d2;
                istart2 = i;
            }
        }
    }

    // Now triangulate the strip zig-zag fashion taking always the shortest connection if possible.
    for (int u = ibegin1, v = istart2; n1 > 0 || n2 > 0;) {
        bool take_first;
        int u2, v2;
        auto update_u2 = [&u2, u, ibegin1, iend1]() {
            u2 = u;
            if (++ u2 == iend1)
                u2 = ibegin1;
        };
        auto update_v2 = [&v2, v, ibegin2, iend2]() {
            v2 = v;
            if (++ v2 == iend2)
                v2 = ibegin2;
        };
        if (n1 == 0) {
            take_first = false;
            update_v2();
        } else if (n2 == 0) {
            take_first = true;
            update_u2();
        } else {
            update_u2();
            update_v2();
            float l1 = (its.vertices[u2] - its.vertices[v]).squaredNorm();
            float l2 = (its.vertices[v2] - its.vertices[u]).squaredNorm();
            take_first = l1 < l2;
        }
        if (take_first) {
            its.indices.push_back({ u, u2, v });
            -- n1;
            u = u2;
        } else {
            its.indices.push_back({ u, v2, v });
            -- n2;
            v = v2;
        }
    }
}

// Discretize 3D circle, append to output vector, return ranges of indices of the points added.
static std::pair<int, int> discretize_circle(const Vec3f &center, const Vec3f &normal, const float radius, const float eps, std::vector<Vec3f> &pts)
{
    // Calculate discretization step and number of steps.
    float angle_step = 2. * acos(1. - eps / radius);
    auto  nsteps     = int(ceil(2 * M_PI / angle_step));
    angle_step = 2 * M_PI / nsteps;

    // Prepare coordinate system for the circle plane.
    Vec3f x = normal.cross(Vec3f(0.f, -1.f, 0.f)).normalized();
    Vec3f y = normal.cross(x).normalized();
    assert(std::abs(x.cross(y).dot(normal) - 1.f) < EPSILON);

    // Discretize the circle.
    int begin = int(pts.size());
    pts.reserve(pts.size() + nsteps);
    float angle = 0;
    x *= radius;
    y *= radius;
    for (int i = 0; i < nsteps; ++ i) {
        pts.emplace_back(center + x * cos(angle) + y * sin(angle));
        angle += angle_step;
    }
    return { begin, int(pts.size()) };
}

// Returns Z span of the generated mesh.
static std::pair<float, float> extrude_branch(
    const std::vector<const SupportElement*>&path,
    const TreeSupportSettings               &config,
    const SlicingParameters                 &slicing_params,
    const std::vector<SupportElements>      &move_bounds,
    indexed_triangle_set                    &result)
{
    Vec3d p1, p2, p3;
    Vec3d v1, v2;
    Vec3d nprev;
    Vec3d ncurrent;
    assert(path.size() >= 2);
    static constexpr const float eps = 0.015f;
    std::pair<int, int> prev_strip;
    float zmin = 0;
    float zmax = 0;

    for (size_t ipath = 1; ipath < path.size(); ++ ipath) {
        const SupportElement &prev    = *path[ipath - 1];
        const SupportElement &current = *path[ipath];
        assert(prev.state.layer_idx + 1 == current.state.layer_idx);
        p1 = to_3d(unscaled<double>(prev   .state.result_on_layer), layer_z(slicing_params, config, prev   .state.layer_idx));
        p2 = to_3d(unscaled<double>(current.state.result_on_layer), layer_z(slicing_params, config, current.state.layer_idx));
        v1 = (p2 - p1).normalized();
        if (ipath == 1) {
            nprev = v1;
            // Extrude the bottom half sphere.
            float radius     = unscaled<float>(support_element_radius(config, prev));
            float angle_step = 2. * acos(1. - eps / radius);
            auto  nsteps     = int(ceil(M_PI / (2. * angle_step)));
            angle_step       = M_PI / (2. * nsteps);
            int   ifan       = int(result.vertices.size());
            result.vertices.emplace_back((p1 - nprev * radius).cast<float>());
            zmin = result.vertices.back().z();
            float angle = angle_step;
            for (int i = 1; i < nsteps; ++ i, angle += angle_step) {
                std::pair<int, int> strip = discretize_circle((p1 - nprev * radius * cos(angle)).cast<float>(), nprev.cast<float>(), radius * sin(angle), eps, result.vertices);
                if (i == 1)
                    triangulate_fan<false>(result, ifan, strip.first, strip.second);
                else
                    triangulate_strip(result, prev_strip.first, prev_strip.second, strip.first, strip.second);
//                sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++ irun);
//                its_write_obj(result, fname);
                prev_strip = strip;
            }
        }
        if (ipath + 1 == path.size()) {
            // End of the tube.
            ncurrent = v1;
            // Extrude the top half sphere.
            float radius = unscaled<float>(support_element_radius(config, current));
            float angle_step = 2. * acos(1. - eps / radius);
            auto  nsteps = int(ceil(M_PI / (2. * angle_step)));
            angle_step = M_PI / (2. * nsteps);
            auto angle = float(M_PI / 2.);
            for (int i = 0; i < nsteps; ++ i, angle -= angle_step) {
                std::pair<int, int> strip = discretize_circle((p2 + ncurrent * radius * cos(angle)).cast<float>(), ncurrent.cast<float>(), radius * sin(angle), eps, result.vertices);
                triangulate_strip(result, prev_strip.first, prev_strip.second, strip.first, strip.second);
//                sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++ irun);
//                its_write_obj(result, fname);
                prev_strip = strip;
            }
            int ifan = int(result.vertices.size());
            result.vertices.emplace_back((p2 + ncurrent * radius).cast<float>());
            zmax = result.vertices.back().z();
            triangulate_fan<true>(result, ifan, prev_strip.first, prev_strip.second);
//            sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++ irun);
//            its_write_obj(result, fname);
        } else {
            const SupportElement &next = *path[ipath + 1];
            assert(current.state.layer_idx + 1 == next.state.layer_idx);
            p3 = to_3d(unscaled<double>(next.state.result_on_layer), layer_z(slicing_params, config, next.state.layer_idx));
            v2 = (p3 - p2).normalized();
            ncurrent = (v1 + v2).normalized();
            float radius = unscaled<float>(support_element_radius(config, current));
            std::pair<int, int> strip = discretize_circle(p2.cast<float>(), ncurrent.cast<float>(), radius, eps, result.vertices);
            triangulate_strip(result, prev_strip.first, prev_strip.second, strip.first, strip.second);
            prev_strip = strip;
//            sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++irun);
//            its_write_obj(result, fname);
        }
#if 0
        if (circles_intersect(p1, nprev, support_element_radius(settings, prev), p2, ncurrent, support_element_radius(settings, current))) {
            // Cannot connect previous and current slice using a simple zig-zag triangulation,
            // because the two circles intersect.

        } else {
            // Continue with chaining.

        }
#endif
    }

    return std::make_pair(zmin, zmax);
}


#ifdef TREE_SUPPORT_ORGANIC_NUDGE_NEW

// New version using per layer AABB trees of lines for nudging spheres away from an object.
static void organic_smooth_branches_avoid_collisions(
    const PrintObject                                   &print_object,
    const TreeModelVolumes                              &volumes,
    const TreeSupportSettings                           &config,
    std::vector<SupportElements>                        &move_bounds,
    const std::vector<std::pair<SupportElement*, int>>  &elements_with_link_down,
    const std::vector<size_t>                           &linear_data_layers,
    std::function<void()>                                throw_on_cancel)
{
    struct LayerCollisionCache {
        coord_t          min_element_radius{ std::numeric_limits<coord_t>::max() };
        bool             min_element_radius_known() const { return this->min_element_radius != std::numeric_limits<coord_t>::max(); }
        coord_t          collision_radius{ 0 };
        std::vector<Linef> lines;
        AABBTreeIndirect::Tree<2, double> aabbtree_lines;
        bool             empty() const { return this->lines.empty(); }
    };
    std::vector<LayerCollisionCache> layer_collision_cache;
    layer_collision_cache.reserve(1024);
    const SlicingParameters &slicing_params = print_object.slicing_parameters();
    for (const std::pair<SupportElement*, int>& element : elements_with_link_down) {
        LayerIndex layer_idx = element.first->state.layer_idx;
        if (size_t num_layers = layer_idx + 1; num_layers > layer_collision_cache.size()) {
            if (num_layers > layer_collision_cache.capacity())
                reserve_power_of_2(layer_collision_cache, num_layers);
            layer_collision_cache.resize(num_layers, {});
        }
        auto& l = layer_collision_cache[layer_idx];
        l.min_element_radius = std::min(l.min_element_radius, support_element_radius(config, *element.first));
    }

    throw_on_cancel();

    for (LayerIndex layer_idx = 0; layer_idx < LayerIndex(layer_collision_cache.size()); ++layer_idx)
        if (LayerCollisionCache& l = layer_collision_cache[layer_idx]; !l.min_element_radius_known())
            l.min_element_radius = 0;
        else {
            //FIXME
            l.min_element_radius = 0;
            std::optional<std::pair<coord_t, std::reference_wrapper<const Polygons>>> res = volumes.get_collision_lower_bound_area(layer_idx, l.min_element_radius);
            assert(res.has_value());
            l.collision_radius = res->first;
            Lines alines = to_lines(res->second.get());
            l.lines.reserve(alines.size());
            for (const Line &line : alines)
                l.lines.push_back({ unscaled<double>(line.a), unscaled<double>(line.b) });
            l.aabbtree_lines = AABBTreeLines::build_aabb_tree_over_indexed_lines(l.lines);
            throw_on_cancel();
        }

    struct CollisionSphere {
        const SupportElement& element;
        int                   element_below_id;
        const bool            locked;
        float                 radius;
        // Current position, when nudged away from the collision.
        Vec3f                 position;
        // Previous position, for Laplacian smoothing.
        Vec3f                 prev_position;
        //
        Vec3f                 last_collision;
        double                last_collision_depth;
        // Minimum Z for which the sphere collision will be evaluated.
        // Limited by the minimum sloping angle and by the bottom of the tree.
        float                 min_z{ -std::numeric_limits<float>::max() };
        // Maximum Z for which the sphere collision will be evaluated.
        // Limited by the minimum sloping angle and by the tip of the current branch.
        float                 max_z{ std::numeric_limits<float>::max() };
        uint32_t              layer_begin;
        uint32_t              layer_end;
    };

    std::vector<CollisionSphere> collision_spheres;
    collision_spheres.reserve(elements_with_link_down.size());
    for (const std::pair<SupportElement*, int> &element_with_link : elements_with_link_down) {
        const SupportElement &element   = *element_with_link.first;
        const int             link_down = element_with_link.second;
        collision_spheres.push_back({
            element,
            link_down,
            // locked
            element.parents.empty() || (link_down == -1 && element.state.layer_idx > 0),
            unscaled<float>(support_element_radius(config, element)),
            // 3D position
            to_3d(unscaled<float>(element.state.result_on_layer), float(layer_z(slicing_params, config, element.state.layer_idx)))
        });
        // Update min_z coordinate to min_z of the tree below.
        CollisionSphere &collision_sphere = collision_spheres.back();
        if (link_down != -1) {
            const size_t offset_below = linear_data_layers[element.state.layer_idx - 1];
            collision_sphere.min_z = collision_spheres[offset_below + link_down].min_z;
        } else
            collision_sphere.min_z = collision_sphere.position.z();
    }
    // Update max_z by propagating max_z from the tips of the branches.
    for (int collision_sphere_id = int(collision_spheres.size()) - 1; collision_sphere_id >= 0; -- collision_sphere_id) {
        CollisionSphere &collision_sphere = collision_spheres[collision_sphere_id];
        if (collision_sphere.element.parents.empty())
            // Tip
            collision_sphere.max_z = collision_sphere.position.z();
        else {
            // Below tip
            const size_t offset_above = linear_data_layers[collision_sphere.element.state.layer_idx + 1];
            for (auto iparent : collision_sphere.element.parents) {
                float parent_z = collision_spheres[offset_above + iparent].max_z;
//                    collision_sphere.max_z = collision_sphere.max_z == std::numeric_limits<float>::max() ? parent_z : std::max(collision_sphere.max_z, parent_z);
                collision_sphere.max_z = std::min(collision_sphere.max_z, parent_z);
            }
        }
    }
    // Update min_z / max_z to limit the search Z span of a given sphere for collision detection.
    for (CollisionSphere &collision_sphere : collision_spheres) {
        //FIXME limit the collision span by the tree slope.
        collision_sphere.min_z = std::max(collision_sphere.min_z, collision_sphere.position.z() - collision_sphere.radius);
        collision_sphere.max_z = std::min(collision_sphere.max_z, collision_sphere.position.z() + collision_sphere.radius);
        collision_sphere.layer_begin = std::min(collision_sphere.element.state.layer_idx, layer_idx_ceil(slicing_params, config, collision_sphere.min_z));
        assert(collision_sphere.layer_begin < layer_collision_cache.size());
        collision_sphere.layer_end   = std::min(LayerIndex(layer_collision_cache.size()), std::max(collision_sphere.element.state.layer_idx, layer_idx_floor(slicing_params, config, collision_sphere.max_z)) + 1);
    }

    throw_on_cancel();

    static constexpr const double collision_extra_gap = 0.1;
    static constexpr const double max_nudge_collision_avoidance = 0.5;
    static constexpr const double max_nudge_smoothing = 0.2;
    static constexpr const size_t num_iter = 100; // 1000;
    for (size_t iter = 0; iter < num_iter; ++ iter) {
        // Back up prev position before Laplacian smoothing.
        for (CollisionSphere &collision_sphere : collision_spheres)
            collision_sphere.prev_position = collision_sphere.position;
        std::atomic<size_t> num_moved{ 0 };
        tbb::parallel_for(tbb::blocked_range<size_t>(0, collision_spheres.size()),
            [&collision_spheres, &layer_collision_cache, &slicing_params, &config, &linear_data_layers, &num_moved, &throw_on_cancel](const tbb::blocked_range<size_t> range) {
            for (size_t collision_sphere_id = range.begin(); collision_sphere_id < range.end(); ++ collision_sphere_id)
                if (CollisionSphere &collision_sphere = collision_spheres[collision_sphere_id]; ! collision_sphere.locked) {
                    // Calculate collision of multiple 2D layers against a collision sphere.
                    collision_sphere.last_collision_depth = - std::numeric_limits<double>::max();
                    for (uint32_t layer_id = collision_sphere.layer_begin; layer_id != collision_sphere.layer_end; ++ layer_id) {
                        double dz = (layer_id - collision_sphere.element.state.layer_idx) * slicing_params.layer_height;
                        if (double r2 = sqr(collision_sphere.radius) - sqr(dz); r2 > 0) {
                            if (const LayerCollisionCache &layer_collision_cache_item = layer_collision_cache[layer_id]; ! layer_collision_cache_item.empty()) {
                                size_t hit_idx_out;
                                Vec2d  hit_point_out;
                                if (double dist = sqrt(AABBTreeLines::squared_distance_to_indexed_lines(
                                    layer_collision_cache_item.lines, layer_collision_cache_item.aabbtree_lines, Vec2d(to_2d(collision_sphere.position).cast<double>()),
                                    hit_idx_out, hit_point_out, r2)); dist >= 0.) {
                                    double collision_depth = sqrt(r2) - dist;
                                    if (collision_depth > collision_sphere.last_collision_depth) {
                                        collision_sphere.last_collision_depth = collision_depth;
                                        collision_sphere.last_collision = to_3d(hit_point_out.cast<float>(), float(layer_z(slicing_params, config, layer_id)));
                                    }
                                }
                            }
                        }
                    }
                    if (collision_sphere.last_collision_depth > 0) {
                        // Collision detected to be removed.
                        // Nudge the circle center away from the collision.
                        if (collision_sphere.last_collision_depth > EPSILON)
                            // a little bit of hysteresis to detect end of
                            ++ num_moved;
                        // Shift by maximum 2mm.
                        double nudge_dist = std::min(std::max(0., collision_sphere.last_collision_depth + collision_extra_gap), max_nudge_collision_avoidance);
                        Vec2d nudge_vector = (to_2d(collision_sphere.position) - to_2d(collision_sphere.last_collision)).cast<double>().normalized() * nudge_dist;
                        collision_sphere.position.head<2>() += (nudge_vector * nudge_dist).cast<float>();
                    }
                    // Laplacian smoothing
                    Vec2d avg{ 0, 0 };
                    //const SupportElements &above = move_bounds[collision_sphere.element.state.layer_idx + 1];
                    const size_t           offset_above = linear_data_layers[collision_sphere.element.state.layer_idx + 1];
                    double weight = 0.;
                    for (auto iparent : collision_sphere.element.parents) {
                        double w = collision_sphere.radius;
                        avg += w * to_2d(collision_spheres[offset_above + iparent].prev_position.cast<double>());
                        weight += w;
                    }
                    if (collision_sphere.element_below_id != -1) {
                        const size_t offset_below = linear_data_layers[collision_sphere.element.state.layer_idx - 1];
                        const double w = weight; //  support_element_radius(config, move_bounds[element.state.layer_idx - 1][below]);
                        avg += w * to_2d(collision_spheres[offset_below + collision_sphere.element_below_id].prev_position.cast<double>());
                        weight += w;
                    }
                    avg /= weight;
                    static constexpr const double smoothing_factor = 0.5;
                    Vec2d old_pos = to_2d(collision_sphere.position).cast<double>();
                    Vec2d new_pos = (1. - smoothing_factor) * old_pos + smoothing_factor * avg;
                    Vec2d shift   = new_pos - old_pos;
                    double nudge_dist_max = shift.norm();
                    // Shift by maximum 1mm, less than the collision avoidance factor.
                    double nudge_dist = std::min(std::max(0., nudge_dist_max), max_nudge_smoothing);
                    collision_sphere.position.head<2>() += (shift.normalized() * nudge_dist).cast<float>();

                    throw_on_cancel();
                }
        });
#if 0
        std::vector<double> stat;
        for (CollisionSphere& collision_sphere : collision_spheres)
            if (!collision_sphere.locked)
                stat.emplace_back(collision_sphere.last_collision_depth);
        std::sort(stat.begin(), stat.end());
        printf("iteration: %d, moved: %d, collision depth: min %lf, max %lf, median %lf\n", int(iter), int(num_moved), stat.front(), stat.back(), stat[stat.size() / 2]);
#endif
        if (num_moved == 0)
            break;
    }

    for (size_t i = 0; i < collision_spheres.size(); ++ i)
        elements_with_link_down[i].first->state.result_on_layer = scaled<coord_t>(to_2d(collision_spheres[i].position));
}
#else // TREE_SUPPORT_ORGANIC_NUDGE_NEW
// Old version using OpenVDB, works but it is extremely slow for complex meshes.
static void organic_smooth_branches_avoid_collisions(
    const PrintObject                                   &print_object,
    const TreeModelVolumes                              &volumes,
    const TreeSupportSettings                           &config,
    std::vector<SupportElements>                        &move_bounds,
    const std::vector<std::pair<SupportElement*, int>>  &elements_with_link_down,
    const std::vector<size_t>                           &linear_data_layers,
    std::function<void()>                                throw_on_cancel)
{
    TriangleMesh mesh = print_object.model_object()->raw_mesh();
    mesh.transform(print_object.trafo_centered());
    double scale = 10.;
    openvdb::FloatGrid::Ptr grid = mesh_to_grid(mesh.its, openvdb::math::Transform{}, scale, 0., 0.);
    std::unique_ptr<openvdb::tools::ClosestSurfacePoint<openvdb::FloatGrid>> closest_surface_point = openvdb::tools::ClosestSurfacePoint<openvdb::FloatGrid>::create(*grid);
    std::vector<openvdb::Vec3R> pts, prev, projections;
    std::vector<float> distances;
    for (const std::pair<SupportElement*, int>& element : elements_with_link_down) {
        Vec3d pt = to_3d(unscaled<double>(element.first->state.result_on_layer), layer_z(print_object.slicing_parameters(), config, element.first->state.layer_idx)) * scale;
        pts.push_back({ pt.x(), pt.y(), pt.z() });
    }

    const double collision_extra_gap = 1. * scale;
    const double max_nudge_collision_avoidance = 2. * scale;
    const double max_nudge_smoothing = 1. * scale;

    static constexpr const size_t num_iter = 100; // 1000;
    for (size_t iter = 0; iter < num_iter; ++ iter) {
        prev = pts;
        projections = pts;
        distances.assign(pts.size(), std::numeric_limits<float>::max());
        closest_surface_point->searchAndReplace(projections, distances);
        size_t num_moved = 0;
        for (size_t i = 0; i < projections.size(); ++ i) {
            const SupportElement &element = *elements_with_link_down[i].first;
            const int            below    = elements_with_link_down[i].second;
            const bool           locked   = (below == -1 && element.state.layer_idx > 0) || element.state.locked();
            if (! locked && pts[i] != projections[i]) {
                // Nudge the circle center away from the collision.
                Vec3d v{ projections[i].x() - pts[i].x(), projections[i].y() - pts[i].y(), projections[i].z() - pts[i].z() };
                double depth = v.norm();
                assert(std::abs(distances[i] - depth) < EPSILON);
                double radius = unscaled<double>(support_element_radius(config, element)) * scale;
                if (depth < radius) {
                    // Collision detected to be removed.
                    ++ num_moved;
                    double dxy = sqrt(sqr(radius) - sqr(v.z()));
                    double nudge_dist_max = dxy - std::hypot(v.x(), v.y())
                        //FIXME 1mm gap
                        + collision_extra_gap;
                    // Shift by maximum 2mm.
                    double nudge_dist = std::min(std::max(0., nudge_dist_max), max_nudge_collision_avoidance);
                    Vec2d nudge_v = to_2d(v).normalized() * (- nudge_dist);
                    pts[i].x() += nudge_v.x();
                    pts[i].y() += nudge_v.y();
                }
            }
            // Laplacian smoothing
            if (! locked && ! element.parents.empty()) {
                Vec2d avg{ 0, 0 };
                const SupportElements &above = move_bounds[element.state.layer_idx + 1];
                const size_t           offset_above = linear_data_layers[element.state.layer_idx + 1];
                double weight = 0.;
                for (auto iparent : element.parents) {
                    double w = support_element_radius(config, above[iparent]);
                    avg.x() += w * prev[offset_above + iparent].x();
                    avg.y() += w * prev[offset_above + iparent].y();
                    weight += w;
                }
                size_t cnt = element.parents.size();
                if (below != -1) {
                    const size_t offset_below = linear_data_layers[element.state.layer_idx - 1];
                    const double w = weight; //  support_element_radius(config, move_bounds[element.state.layer_idx - 1][below]);
                    avg.x() += w * prev[offset_below + below].x();
                    avg.y() += w * prev[offset_below + below].y();
                    ++ cnt;
                    weight += w;
                }
                //avg /= double(cnt);
                avg /= weight;
                static constexpr const double smoothing_factor = 0.5;
                Vec2d old_pos{ pts[i].x(), pts[i].y() };
                Vec2d new_pos = (1. - smoothing_factor) * old_pos + smoothing_factor * avg;
                Vec2d shift = new_pos - old_pos;
                double nudge_dist_max = shift.norm();
                // Shift by maximum 1mm, less than the collision avoidance factor.
                double nudge_dist = std::min(std::max(0., nudge_dist_max), max_nudge_smoothing);
                Vec2d nudge_v = shift.normalized() * nudge_dist;
                pts[i].x() += nudge_v.x();
                pts[i].y() += nudge_v.y();
            }
        }
//            printf("iteration: %d, moved: %d\n", int(iter), int(num_moved));
        if (num_moved == 0)
            break;
    }

    for (size_t i = 0; i < projections.size(); ++ i) {
        elements_with_link_down[i].first->state.result_on_layer.x() = scaled<coord_t>(pts[i].x()) / scale;
        elements_with_link_down[i].first->state.result_on_layer.y() = scaled<coord_t>(pts[i].y()) / scale;
    }
}
#endif // TREE_SUPPORT_ORGANIC_NUDGE_NEW

/*!
 * \brief Create the areas that need support.
 *
 * These areas are stored inside the given SliceDataStorage object.
 * \param storage The data storage where the mesh data is gotten from and
 * where the resulting support areas are stored.
 */
static void generate_support_areas(Print &print, TreeSupport* tree_support, const BuildVolume &build_volume, const std::vector<size_t> &print_object_ids, std::function<void()> throw_on_cancel)
{
    // Settings with the indexes of meshes that use these settings.
    std::vector<std::pair<TreeSupportSettings, std::vector<size_t>>> grouped_meshes = group_meshes(print, print_object_ids);
    if (grouped_meshes.empty())
        return;

    size_t counter = 0;

    // Process every mesh group. These groups can not be processed parallel, as the processing in each group is parallelized, and nested parallelization is disables and slow.
    for (std::pair<TreeSupportSettings, std::vector<size_t>> &processing : grouped_meshes)
    {
        // process each combination of meshes
        // this struct is used to easy retrieve setting. No other function except those in TreeModelVolumes and generate_initial_areas() have knowledge of the existence of multiple meshes being processed.
        //FIXME this is a copy
        // Contains config settings to avoid loading them in every function. This was done to improve readability of the code.
        const TreeSupportSettings &config = processing.first;
        BOOST_LOG_TRIVIAL(info) << "Processing support tree mesh group " << counter + 1 << " of " << grouped_meshes.size() << " containing " << grouped_meshes[counter].second.size() << " meshes.";
        auto t_start = std::chrono::high_resolution_clock::now();
#if 0
        std::vector<Polygons> exclude(num_support_layers);
        // get all already existing support areas and exclude them
        tbb::parallel_for(tbb::blocked_range<size_t>(0, num_support_layers),
            [&](const tbb::blocked_range<size_t> &range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                Polygons exlude_at_layer;
                append(exlude_at_layer, storage.support.supportLayers[layer_idx].support_bottom);
                append(exlude_at_layer, storage.support.supportLayers[layer_idx].support_roof);
                for (auto part : storage.support.supportLayers[layer_idx].support_infill_parts)
                    append(exlude_at_layer, part.outline);
                exclude[layer_idx] = union_(exlude_at_layer);
            }
        });
#endif
#ifdef SLIC3R_TREESUPPORTS_PROGRESS
        m_progress_multiplier = 1.0 / double(grouped_meshes.size());
        m_progress_offset = counter == 0 ? 0 : TREE_PROGRESS_TOTAL * (double(counter) * m_progress_multiplier);
#endif // SLIC3R_TREESUPPORT_PROGRESS
        PrintObject &print_object = *print.get_object(processing.second.front());
        // Generator for model collision, avoidance and internal guide volumes.
        TreeModelVolumes volumes{ print_object, build_volume, config.maximum_move_distance, config.maximum_move_distance_slow, processing.second.front(),
#ifdef SLIC3R_TREESUPPORTS_PROGRESS
            m_progress_multiplier, m_progress_offset,
#endif // SLIC3R_TREESUPPORTS_PROGRESS
            /* additional_excluded_areas */{} };

        //FIXME generating overhangs just for the first mesh of the group.
        assert(processing.second.size() == 1);

#if 1
        // use smart overhang detection
        std::vector<Polygons>        overhangs;
        tree_support->detect_overhangs();
        const int       num_raft_layers = int(config.raft_layers.size());
        const int       num_layers = int(print_object.layer_count()) + num_raft_layers;
        overhangs.resize(num_layers);
        for (size_t i = 0; i < print_object.layer_count(); i++) {
            for (ExPolygon& expoly : print_object.get_layer(i)->loverhangs) {
                Polygons polys = to_polygons(expoly);
                if (tree_support->overhang_types[&expoly] == TreeSupport::SharpTail) { polys = offset(polys, scale_(0.2));
                }
                append(overhangs[i + num_raft_layers], polys);
            }
        }
        // add vertical enforcer points
        std::vector<float> zs = zs_from_layers(print_object.layers());
        Polygon            base_circle = make_circle(scale_(0.5), SUPPORT_TREE_CIRCLE_RESOLUTION);
        for (auto &pt_and_normal :tree_support->m_vertical_enforcer_points) {
            auto pt     = pt_and_normal.first;
            auto normal = pt_and_normal.second; // normal seems useless
            auto iter   = std::lower_bound(zs.begin(), zs.end(), pt.z());
            if (iter != zs.end()) {
                size_t layer_nr = iter - zs.begin();
                if (layer_nr > 0 && layer_nr < print_object.layer_count()) {
                    Polygon circle = base_circle;
                    circle.translate(to_2d(pt).cast<coord_t>());
                    overhangs[layer_nr + num_raft_layers].emplace_back(std::move(circle));
                }
            }
        }
#else
        std::vector<Polygons>        overhangs = generate_overhangs(config, *print.get_object(processing.second.front()), throw_on_cancel);
#endif
        // ### Precalculate avoidances, collision etc.
        size_t num_support_layers = precalculate(print, overhangs, processing.first, processing.second, volumes, throw_on_cancel);
        bool   has_support = num_support_layers > 0;
        bool   has_raft    = config.raft_layers.size() > 0;
        num_support_layers = std::max(num_support_layers, config.raft_layers.size());

        if (num_support_layers == 0)
            continue;

        SupportParameters            support_params(print_object);
        support_params.with_sheath = true;
// Don't override the support density of tree supports, as the support density is used for raft.
// The trees will have the density zeroed in tree_supports_generate_paths()
//        support_params.support_density = 0;


        SupportGeneratorLayerStorage layer_storage;
        SupportGeneratorLayersPtr    top_contacts;
        SupportGeneratorLayersPtr    bottom_contacts;
        SupportGeneratorLayersPtr    interface_layers;
        SupportGeneratorLayersPtr    base_interface_layers;
        SupportGeneratorLayersPtr    intermediate_layers(num_support_layers, nullptr);
        if (support_params.has_top_contacts || has_raft)
            top_contacts.assign(num_support_layers, nullptr);
        if (support_params.has_bottom_contacts)
            bottom_contacts.assign(num_support_layers, nullptr);
        if (support_params.has_interfaces() || has_raft)
            interface_layers.assign(num_support_layers, nullptr);
        if (support_params.has_base_interfaces() || has_raft)
            base_interface_layers.assign(num_support_layers, nullptr);

        auto remove_undefined_layers = [&bottom_contacts, &top_contacts, &interface_layers, &base_interface_layers, &intermediate_layers]() {
            auto doit = [](SupportGeneratorLayersPtr& layers) {
                layers.erase(std::remove_if(layers.begin(), layers.end(), [](const SupportGeneratorLayer* ptr) { return ptr == nullptr; }), layers.end());
            };
            doit(bottom_contacts);
            doit(top_contacts);
            doit(interface_layers);
            doit(base_interface_layers);
            doit(intermediate_layers);
        };

        InterfacePlacer              interface_placer{
            print_object.slicing_parameters(), support_params, config,
            // Outputs
            layer_storage, top_contacts, interface_layers, base_interface_layers };

        if (has_support) {
            auto t_precalc = std::chrono::high_resolution_clock::now();

            // value is the area where support may be placed. As this is calculated in CreateLayerPathing it is saved and reused in draw_areas
            std::vector<SupportElements> move_bounds(num_support_layers);

            // ### Place tips of the support tree
            for (size_t mesh_idx : processing.second)
                generate_initial_areas(*print.get_object(mesh_idx), volumes, config, overhangs, 
                    move_bounds, interface_placer, throw_on_cancel);
            auto t_gen = std::chrono::high_resolution_clock::now();

#ifdef TREESUPPORT_DEBUG_SVG
            for (size_t layer_idx = 0; layer_idx < move_bounds.size(); ++layer_idx) {
                Polygons polys;
                for (auto& area : move_bounds[layer_idx])
                    append(polys, area.influence_area);
                if (auto begin = move_bounds[layer_idx].begin(); begin != move_bounds[layer_idx].end())
                    SVG::export_expolygons(debug_out_path("treesupport-initial_areas-%d.svg", layer_idx),
                        { { { union_ex(volumes.getWallRestriction(support_element_collision_radius(config, begin->state), layer_idx, begin->state.use_min_xy_dist)) },
                            { "wall_restricrictions", "gray", 0.5f } },
                          { { union_ex(polys) }, { "parent", "red",  "black", "", scaled<coord_t>(0.1f), 0.5f } } });
            }
    #endif // TREESUPPORT_DEBUG_SVG

            // ### Propagate the influence areas downwards. This is an inherently serial operation.
            print.set_status(60, _L("Generating support"));
            create_layer_pathing(volumes, config, move_bounds, throw_on_cancel);
            auto t_path = std::chrono::high_resolution_clock::now();

            // ### Set a point in each influence area
            create_nodes_from_area(volumes, config, move_bounds, throw_on_cancel);
            auto t_place = std::chrono::high_resolution_clock::now();

            // ### draw these points as circles
            // this new function give correct result when raft is also enabled
            organic_draw_branches(
                *print.get_object(processing.second.front()), volumes, config, move_bounds,
                bottom_contacts, top_contacts, interface_placer, intermediate_layers, layer_storage,
                throw_on_cancel);

            //tree_support->move_bounds_to_contact_nodes(move_bounds, print_object, config);

            remove_undefined_layers();

            std::tie(interface_layers, base_interface_layers) = generate_interface_layers(print_object.config(), support_params,
                bottom_contacts, top_contacts, interface_layers, base_interface_layers, intermediate_layers, layer_storage);

            auto t_draw = std::chrono::high_resolution_clock::now();
            auto dur_pre_gen = 0.001 * std::chrono::duration_cast<std::chrono::microseconds>(t_precalc - t_start).count();
            auto dur_gen = 0.001 * std::chrono::duration_cast<std::chrono::microseconds>(t_gen - t_precalc).count();
            auto dur_path = 0.001 * std::chrono::duration_cast<std::chrono::microseconds>(t_path - t_gen).count();
            auto dur_place = 0.001 * std::chrono::duration_cast<std::chrono::microseconds>(t_place - t_path).count();
            auto dur_draw = 0.001 * std::chrono::duration_cast<std::chrono::microseconds>(t_draw - t_place).count();
            auto dur_total = 0.001 * std::chrono::duration_cast<std::chrono::microseconds>(t_draw - t_start).count();
            BOOST_LOG_TRIVIAL(info) <<
                "Total time used creating Tree support for the currently grouped meshes: " << dur_total << " ms. "
                "Different subtasks:\nCalculating Avoidance: " << dur_pre_gen << " ms "
                "Creating inital influence areas: " << dur_gen << " ms "
                "Influence area creation: " << dur_path << "ms "
                "Placement of Points in InfluenceAreas: " << dur_place << "ms "
                "Drawing result as support " << dur_draw << " ms";
    //        if (config.branch_radius==2121)
    //            BOOST_LOG_TRIVIAL(error) << "Why ask questions when you already know the answer twice.\n (This is not a real bug, please dont report it.)";
            
            move_bounds.clear();
        } else if (generate_raft_contact(print_object, config, interface_placer) >= 0) {
            remove_undefined_layers();
        } else
            // No raft.
            continue;

        // Produce the support G-code.
        SupportGeneratorLayersPtr raft_layers = generate_raft_base(print_object, support_params, print_object.slicing_parameters(), top_contacts, interface_layers, base_interface_layers, intermediate_layers, layer_storage);
        SupportGeneratorLayersPtr layers_sorted = generate_support_layers(print_object, raft_layers, bottom_contacts, top_contacts, intermediate_layers, interface_layers, base_interface_layers);

        // BBS: This is a hack to avoid the support being generated outside the bed area. See #4769.
        tbb::parallel_for_each(layers_sorted.begin(), layers_sorted.end(), [&](SupportGeneratorLayer *layer) {
            if (layer) layer->polygons = intersection(layer->polygons, volumes.m_bed_area);
        });

        // Don't fill in the tree supports, make them hollow with just a single sheath line.
        print.set_status(69, _L("Generating support"));
        generate_support_toolpaths(print_object.support_layers(), print_object.config(), support_params, print_object.slicing_parameters(),
            raft_layers, bottom_contacts, top_contacts, intermediate_layers, interface_layers, base_interface_layers);

        auto t_end = std::chrono::high_resolution_clock::now();
        BOOST_LOG_TRIVIAL(info) << "Total time of organic tree support: " << 0.001 * std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count() << " ms";
 #if 0
//#ifdef SLIC3R_DEBUG
        {
            static int iRun = 0;
            ++ iRun;
            size_t layer_id = 0;
            for (int i = 0; i < int(layers_sorted.size());) {
                // Find the last layer with roughly the same print_z, find the minimum layer height of all.
                // Due to the floating point inaccuracies, the print_z may not be the same even if in theory they should.
                int j = i + 1;
                coordf_t zmax = layers_sorted[i]->print_z + EPSILON;
                bool empty = layers_sorted[i]->polygons.empty();
                for (; j < layers_sorted.size() && layers_sorted[j]->print_z <= zmax; ++j)
                    if (!layers_sorted[j]->polygons.empty())
                        empty = false;
                if (!empty) {
                    export_print_z_polygons_to_svg(
                        debug_out_path("support-%d-%lf.svg", iRun, layers_sorted[i]->print_z).c_str(),
                        layers_sorted.data() + i, j - i);
                    export_print_z_polygons_and_extrusions_to_svg(
                        debug_out_path("support-w-fills-%d-%lf.svg", iRun, layers_sorted[i]->print_z).c_str(),
                        layers_sorted.data() + i, j - i,
                        *print_object.support_layers()[layer_id]);
                    ++layer_id;
                }
                i = j;
            }
        }
#endif /* SLIC3R_DEBUG */

        ++ counter;
    }

//   storage.support.generated = true;
}

// Organic specific: Smooth branches and produce one cummulative mesh to be sliced.
void organic_draw_branches(
    PrintObject                     &print_object,
    TreeModelVolumes                &volumes, 
    const TreeSupportSettings       &config,
    std::vector<SupportElements>    &move_bounds,

    // I/O:
    SupportGeneratorLayersPtr       &bottom_contacts,
    SupportGeneratorLayersPtr       &top_contacts,
    InterfacePlacer                 &interface_placer,

    // Output:
    SupportGeneratorLayersPtr       &intermediate_layers,
    SupportGeneratorLayerStorage    &layer_storage,

    std::function<void()>            throw_on_cancel)
{
    // All SupportElements are put into a layer independent storage to improve parallelization.
    std::vector<std::pair<SupportElement*, int>> elements_with_link_down;
    std::vector<size_t>                          linear_data_layers;
    {
        std::vector<std::pair<SupportElement*, int>> map_downwards_old;
        std::vector<std::pair<SupportElement*, int>> map_downwards_new;
        linear_data_layers.emplace_back(0);
        for (LayerIndex layer_idx = 0; layer_idx < LayerIndex(move_bounds.size()); ++ layer_idx) {
            SupportElements *layer_above = layer_idx + 1 < LayerIndex(move_bounds.size()) ? &move_bounds[layer_idx + 1] : nullptr;
            map_downwards_new.clear();
            std::sort(map_downwards_old.begin(), map_downwards_old.end(), [](auto& l, auto& r) { return l.first < r.first;  });
            SupportElements &layer = move_bounds[layer_idx];
            for (size_t elem_idx = 0; elem_idx < layer.size(); ++ elem_idx) {
                SupportElement &elem = layer[elem_idx];
                int child = -1;
                if (layer_idx > 0) {
                    auto it = std::lower_bound(map_downwards_old.begin(), map_downwards_old.end(), &elem, [](auto& l, const SupportElement* r) { return l.first < r; });
                    if (it != map_downwards_old.end() && it->first == &elem) {
                        child = it->second;
                        // Only one link points to a node above from below.
                        assert(!(++it != map_downwards_old.end() && it->first == &elem));
                    }
#ifndef NDEBUG
                    {
                        const SupportElement *pchild = child == -1 ? nullptr : &move_bounds[layer_idx - 1][child];
                        assert(pchild ? pchild->state.result_on_layer_is_set() : elem.state.target_height > layer_idx);
                    }
#endif // NDEBUG
                }
                for (int32_t parent_idx : elem.parents) {
                    SupportElement &parent = (*layer_above)[parent_idx];
                    if (parent.state.result_on_layer_is_set())
                        map_downwards_new.emplace_back(&parent, elem_idx);
                }

                elements_with_link_down.push_back({ &elem, int(child) });
            }
            std::swap(map_downwards_old, map_downwards_new);
            linear_data_layers.emplace_back(elements_with_link_down.size());
        }
    }

    throw_on_cancel();

    organic_smooth_branches_avoid_collisions(print_object, volumes, config, move_bounds, elements_with_link_down, linear_data_layers, throw_on_cancel);

    // Reduce memory footprint. After this point only finalize_interface_and_support_areas() will use volumes and from that only collisions with zero radius will be used.
    volumes.clear_all_but_object_collision();

    // Unmark all nodes.
    for (SupportElements &elements : move_bounds)
        for (SupportElement &element : elements)
            element.state.marked = false;

    // Traverse all nodes, generate tubes.
    // Traversal stack with nodes and their current parent

    struct Branch {
        std::vector<const SupportElement*> path;
        bool                               has_root{ false };
        bool                               has_tip { false };
    };

    struct Slice {
        Polygons polygons;
        Polygons bottom_contacts;
        size_t   num_branches{ 0 };
    };

    struct Tree {
        std::vector<Branch>  branches;

        std::vector<Slice>   slices;
        LayerIndex           first_layer_id{ -1 };
    };

    std::vector<Tree>        trees;

    struct TreeVisitor {
        static void visit_recursive(std::vector<SupportElements> &move_bounds, SupportElement &start_element, Tree &out) {
            assert(! start_element.state.marked && ! start_element.parents.empty());
            // Collect elements up to a bifurcation above.
            start_element.state.marked = true;
            // For each branch bifurcating from this point:
            //SupportElements &layer       = move_bounds[start_element.state.layer_idx];
            SupportElements &layer_above = move_bounds[start_element.state.layer_idx + 1];
            bool root = out.branches.empty();
            for (size_t parent_idx = 0; parent_idx < start_element.parents.size(); ++ parent_idx) {
                Branch branch;
                branch.path.emplace_back(&start_element);
                // Traverse each branch until it branches again.
                SupportElement &first_parent = layer_above[start_element.parents[parent_idx]];
                assert(! first_parent.state.marked);
                assert(branch.path.back()->state.layer_idx + 1 == first_parent.state.layer_idx);
                branch.path.emplace_back(&first_parent);
                if (first_parent.parents.size() < 2)
                    first_parent.state.marked = true;
                SupportElement *next_branch = nullptr;
                if (first_parent.parents.size() == 1) {
                    for (SupportElement *parent = &first_parent;;) {
                        assert(parent->state.marked);
                        SupportElement &next_parent = move_bounds[parent->state.layer_idx + 1][parent->parents.front()];
                        assert(! next_parent.state.marked);
                        assert(branch.path.back()->state.layer_idx + 1 == next_parent.state.layer_idx);
                        branch.path.emplace_back(&next_parent);
                        if (next_parent.parents.size() > 1) {
                            // Branching point was reached.
                            next_branch = &next_parent;
                            break;
                        }
                        next_parent.state.marked = true;
                        if (next_parent.parents.size() == 0)
                            // Tip is reached.
                            break;
                        parent = &next_parent;
                    }
                } else if (first_parent.parents.size() > 1)
                    // Branching point was reached.
                    next_branch = &first_parent;
                assert(branch.path.size() >= 2);
                assert(next_branch == nullptr || ! next_branch->state.marked);
                branch.has_root = root;
                branch.has_tip  = ! next_branch;
                out.branches.emplace_back(std::move(branch));
                if (next_branch)
                    visit_recursive(move_bounds, *next_branch, out);
            }
        }
    };

    for (LayerIndex layer_idx = 0; layer_idx + 1 < LayerIndex(move_bounds.size()); ++ layer_idx) {
//        int ielement;
        for (SupportElement& start_element : move_bounds[layer_idx]) {
            if (!start_element.state.marked && !start_element.parents.empty()) {
#if 0
                int found = 0;
                if (layer_idx > 0) {
                    for (auto& el : move_bounds[layer_idx - 1]) {
                        for (auto iparent : el.parents)
                            if (iparent == ielement)
                                ++found;
                    }
                    if (found != 0)
                        printf("Found: %d\n", found);
                }
#endif
                trees.push_back({});
                TreeVisitor::visit_recursive(move_bounds, start_element, trees.back());
                assert(!trees.back().branches.empty());
                //FIXME debugging
#if 0
                if (start_element.state.lost) {
                }
                else if (start_element.state.verylost) {
                } else
                    trees.pop_back();
#endif
            }
//            ++ ielement;
        }
    }

    const SlicingParameters &slicing_params = print_object.slicing_parameters();
    MeshSlicingParams mesh_slicing_params;
    mesh_slicing_params.mode = MeshSlicingParams::SlicingMode::Positive;

    tbb::parallel_for(tbb::blocked_range<size_t>(0, trees.size(), 1),
        [&trees, &volumes, &config, &slicing_params, &move_bounds, &mesh_slicing_params, &throw_on_cancel](const tbb::blocked_range<size_t> &range) {
            indexed_triangle_set    partial_mesh;
            std::vector<float>      slice_z;
            std::vector<Polygons>   bottom_contacts;
            for (size_t tree_id = range.begin(); tree_id < range.end(); ++ tree_id) {
                Tree &tree = trees[tree_id];
                for (const Branch &branch : tree.branches) {
                    // Triangulate the tube.
                    partial_mesh.clear();
                    std::pair<float, float> zspan = extrude_branch(branch.path, config, slicing_params, move_bounds, partial_mesh);
                    LayerIndex layer_begin = branch.has_root ?
                        branch.path.front()->state.layer_idx : 
                        std::min(branch.path.front()->state.layer_idx, layer_idx_ceil(slicing_params, config, zspan.first));
                    LayerIndex layer_end   = (branch.has_tip ?
                        branch.path.back()->state.layer_idx :
                        std::max(branch.path.back()->state.layer_idx, layer_idx_floor(slicing_params, config, zspan.second))) + 1;
                    slice_z.clear();
                    for (LayerIndex layer_idx = layer_begin; layer_idx < layer_end; ++ layer_idx) {
                        const double print_z  = layer_z(slicing_params, config, layer_idx);
                        const double bottom_z = layer_idx > 0 ? layer_z(slicing_params, config, layer_idx - 1) : 0.;
                        slice_z.emplace_back(float(0.5 * (bottom_z + print_z)));
                    }
                    std::vector<Polygons> slices = slice_mesh(partial_mesh, slice_z, mesh_slicing_params, throw_on_cancel);
                    bottom_contacts.clear();
                    //FIXME parallelize?
                    for (LayerIndex i = 0; i < LayerIndex(slices.size()); ++i) {
                        slices[i] = diff_clipped(slices[i], volumes.getCollision(0, layer_begin + i, true)); // FIXME parent_uses_min || draw_area.element->state.use_min_xy_dist);
                        slices[i] = intersection(slices[i], volumes.m_bed_area);
                    }
                    size_t num_empty = 0;
                    if (slices.front().empty()) {
                        // Some of the initial layers are empty.
                        num_empty = std::find_if(slices.begin(), slices.end(), [](auto &s) { return !s.empty(); }) - slices.begin();
                    } else {
                        if (branch.has_root) {
                            if (branch.path.front()->state.to_model_gracious) {
                                if (config.settings.support_floor_layers > 0)
                                    //FIXME one may just take the whole tree slice as bottom interface.
                                    bottom_contacts.emplace_back(intersection_clipped(slices.front(), volumes.getPlaceableAreas(0, layer_begin, [] {})));
                            } else if (layer_begin > 0) {
                                // Drop down areas that do rest non - gracefully on the model to ensure the branch actually rests on something.
                                struct BottomExtraSlice {
                                    Polygons polygons;
                                    double   area;
                                };
                                std::vector<BottomExtraSlice>   bottom_extra_slices;
                                Polygons                        rest_support;
                                coord_t                         bottom_radius = support_element_radius(config, *branch.path.front());
                                // Don't propagate further than 1.5 * bottom radius.
                                //LayerIndex                      layers_propagate_max = 2 * bottom_radius / config.layer_height;
                                LayerIndex                      layers_propagate_max = 5 * bottom_radius / config.layer_height;
                                LayerIndex                      layer_bottommost = branch.path.front()->state.verylost ? 
                                    // If the tree bottom is hanging in the air, bring it down to some surface.
                                    0 : 
                                    //FIXME the "verylost" branches should stop when crossing another support.
                                    std::max(0, layer_begin - layers_propagate_max);
                                double                          support_area_min_radius = M_PI * sqr(double(config.branch_radius));
                                double                          support_area_stop = std::max(0.2 * M_PI * sqr(double(bottom_radius)), 0.5 * support_area_min_radius);
                                 // Only propagate until the rest area is smaller than this threshold.
                                //double                          support_area_min = 0.1 * support_area_min_radius;
                                for (LayerIndex layer_idx = layer_begin - 1; layer_idx >= layer_bottommost; -- layer_idx) {
                                    rest_support = diff_clipped(rest_support.empty() ? slices.front() : rest_support, volumes.getCollision(0, layer_idx, false));
                                    double rest_support_area = area(rest_support);
                                    if (rest_support_area < support_area_stop)
                                        // Don't propagate a fraction of the tree contact surface.
                                        break;
                                    bottom_extra_slices.push_back({ rest_support, rest_support_area });
                                }
                                // Now remove those bottom slices that are not supported at all.
#if 0
                                while (! bottom_extra_slices.empty()) {
                                    Polygons this_bottom_contacts = intersection_clipped(
                                        bottom_extra_slices.back().polygons, volumes.getPlaceableAreas(0, layer_begin - LayerIndex(bottom_extra_slices.size()), [] {}));
                                    if (area(this_bottom_contacts) < support_area_min)
                                        bottom_extra_slices.pop_back();
                                    else {
                                        // At least a fraction of the tree bottom is considered to be supported.
                                        if (config.settings.support_floor_layers > 0)
                                            // Turn this fraction of the tree bottom into a contact layer.
                                            bottom_contacts.emplace_back(std::move(this_bottom_contacts));
                                        break;
                                    }
                                }
#endif
                                if (config.settings.support_floor_layers > 0)
                                    for (int i = int(bottom_extra_slices.size()) - 2; i >= 0; -- i)
                                        bottom_contacts.emplace_back(
                                            intersection_clipped(bottom_extra_slices[i].polygons, volumes.getPlaceableAreas(0, layer_begin - i - 1, [] {})));
                                layer_begin -= LayerIndex(bottom_extra_slices.size());
                                slices.insert(slices.begin(), bottom_extra_slices.size(), {});
                                auto it_dst = slices.begin();
                                for (auto it_src = bottom_extra_slices.rbegin(); it_src != bottom_extra_slices.rend(); ++ it_src)
                                    *it_dst ++ = std::move(it_src->polygons);
                            }
                        }
                        
#if 0
                        //FIXME branch.has_tip seems to not be reliable.
                        if (branch.has_tip && interface_placer.support_parameters.has_top_contacts)
                            // Add top slices to top contacts / interfaces / base interfaces.
                            for (int i = int(branch.path.size()) - 1; i >= 0; -- i) {
                                const SupportElement &el = *branch.path[i];
                                if (el.state.missing_roof_layers == 0)
                                    break;
                                //FIXME Move or not?
                                interface_placer.add_roof(std::move(slices[int(slices.size()) - i - 1]), el.state.layer_idx,
                                    interface_placer.support_parameters.num_top_interface_layers + 1 - el.state.missing_roof_layers);
                            }
#endif
                    }

                    layer_begin += LayerIndex(num_empty);
                    while (! slices.empty() && slices.back().empty()) {
                        slices.pop_back();
                        -- layer_end;
                    }
                    if (layer_begin < layer_end) {
                        LayerIndex new_begin = tree.first_layer_id == -1 ? layer_begin : std::min(tree.first_layer_id, layer_begin);
                        LayerIndex new_end   = tree.first_layer_id == -1 ? layer_end : std::max(tree.first_layer_id + LayerIndex(tree.slices.size()), layer_end);
                        size_t     new_size  = size_t(new_end - new_begin);
                        if (tree.first_layer_id == -1) {
                        } else if (tree.slices.capacity() < new_size) {
                            std::vector<Slice> new_slices;
                            new_slices.reserve(new_size);
                            if (LayerIndex dif = tree.first_layer_id - new_begin; dif > 0)
                                new_slices.insert(new_slices.end(), dif, {});
                            append(new_slices, std::move(tree.slices));
                            tree.slices.swap(new_slices);
                        } else if (LayerIndex dif = tree.first_layer_id - new_begin; dif > 0)
                            tree.slices.insert(tree.slices.begin(), tree.first_layer_id - new_begin, {});
                        tree.slices.insert(tree.slices.end(), new_size - tree.slices.size(), {});
                        layer_begin -= LayerIndex(num_empty);
                        for (LayerIndex i = layer_begin; i != layer_end; ++ i) {
                            int j = i - layer_begin;
                            if (Polygons &src = slices[j]; ! src.empty()) {
                                Slice &dst = tree.slices[i - new_begin];
                                if (++ dst.num_branches > 1) {
                                    append(dst.polygons, std::move(src));
                                    if (j < int(bottom_contacts.size()))
                                        append(dst.bottom_contacts, std::move(bottom_contacts[j]));
                                } else {
                                    dst.polygons = std::move(std::move(src));
                                    if (j < int(bottom_contacts.size()))
                                        dst.bottom_contacts = std::move(bottom_contacts[j]);
                                }
                            }
                        }
                        tree.first_layer_id = new_begin;
                    }
                }
            }
        }, tbb::simple_partitioner());

    tbb::parallel_for(tbb::blocked_range<size_t>(0, trees.size(), 1),
        [&trees, &throw_on_cancel](const tbb::blocked_range<size_t> &range) {
        for (size_t tree_id = range.begin(); tree_id < range.end(); ++ tree_id) {
            Tree &tree = trees[tree_id];
            for (Slice &slice : tree.slices)
                if (slice.num_branches > 1) {
                    slice.polygons        = union_(slice.polygons);
                    slice.bottom_contacts = union_(slice.bottom_contacts);
                    slice.num_branches = 1;
                }
            throw_on_cancel();
        }
    }, tbb::simple_partitioner());

    size_t num_layers = 0;
    for (Tree &tree : trees)
        if (tree.first_layer_id >= 0)
            num_layers = std::max(num_layers, size_t(tree.first_layer_id + tree.slices.size()));

    std::vector<Slice> slices(num_layers, Slice{});
    for (Tree &tree : trees)
        if (tree.first_layer_id >= 0) {
            for (LayerIndex i = tree.first_layer_id; i != tree.first_layer_id + LayerIndex(tree.slices.size()); ++ i)
                if (Slice &src = tree.slices[i - tree.first_layer_id]; ! src.polygons.empty()) {
                    Slice &dst = slices[i];
                    if (++ dst.num_branches > 1) {
                        append(dst.polygons,        std::move(src.polygons));
                        append(dst.bottom_contacts, std::move(src.bottom_contacts));
                    } else {
                        dst.polygons        = std::move(src.polygons);
                        dst.bottom_contacts = std::move(src.bottom_contacts);
                    }
                }
        }

    tbb::parallel_for(tbb::blocked_range<size_t>(0, std::min(move_bounds.size(), slices.size()), 1),
        [&print_object, &config, &slices, &bottom_contacts, &top_contacts, &intermediate_layers, &layer_storage, &throw_on_cancel](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            Slice &slice = slices[layer_idx];
            assert(intermediate_layers[layer_idx] == nullptr);
            Polygons base_layer_polygons     = slice.num_branches > 1 ? union_(slice.polygons) : std::move(slice.polygons);
            Polygons bottom_contact_polygons = slice.num_branches > 1 ? union_(slice.bottom_contacts) : std::move(slice.bottom_contacts);

            if (! base_layer_polygons.empty()) {
                // Most of the time in this function is this union call. Can take 300+ ms when a lot of areas are to be unioned.
                base_layer_polygons = smooth_outward(union_(base_layer_polygons), config.support_line_width); //FIXME was .smooth(50);
                //smooth_outward(closing(std::move(bottom), closing_distance + minimum_island_radius, closing_distance, SUPPORT_SURFACES_OFFSET_PARAMETERS), smoothing_distance) :
                // simplify a bit, to ensure the output does not contain outrageous amounts of vertices. Should not be necessary, just a precaution.
                base_layer_polygons = polygons_simplify(base_layer_polygons, std::min(scaled<double>(0.03), double(config.resolution)), polygons_strictly_simple);
            }

            // Subtract top contact layer polygons from support base.
            SupportGeneratorLayer *top_contact_layer = top_contacts.empty() ? nullptr : top_contacts[layer_idx];
            if (top_contact_layer && ! top_contact_layer->polygons.empty() && ! base_layer_polygons.empty()) {
                base_layer_polygons = diff(base_layer_polygons, top_contact_layer->polygons);
                if (! bottom_contact_polygons.empty())
                    //FIXME it may be better to clip bottom contacts with top contacts first after they are propagated to produce interface layers.
                    bottom_contact_polygons = diff(bottom_contact_polygons, top_contact_layer->polygons);
            }
            if (! bottom_contact_polygons.empty()) {
                base_layer_polygons = diff(base_layer_polygons, bottom_contact_polygons);
                SupportGeneratorLayer *bottom_contact_layer = bottom_contacts[layer_idx] = &layer_allocate(
                    layer_storage, SupporLayerType::BottomContact, print_object.slicing_parameters(), config, layer_idx);
                bottom_contact_layer->polygons = std::move(bottom_contact_polygons);
            }
            if (! base_layer_polygons.empty()) {
                SupportGeneratorLayer *base_layer = intermediate_layers[layer_idx] = &layer_allocate(
                    layer_storage, SupporLayerType::Base, print_object.slicing_parameters(), config, layer_idx);
                base_layer->polygons = union_(base_layer_polygons);
            }

            throw_on_cancel();
        }
    }, tbb::simple_partitioner());
}

} // namespace TreeSupport3D

void generate_tree_support_3D(PrintObject &print_object, TreeSupport* tree_support, std::function<void()> throw_on_cancel)
{
    size_t idx = 0;
    for (const PrintObject *po : print_object.print()->objects()) {
        if (po == &print_object)
            break;
        ++idx;
    }

    Points bedpts = tree_support->m_machine_border.contour.points;
    Pointfs bedptsf;
    std::transform(bedpts.begin(), bedpts.end(), std::back_inserter(bedptsf), [](const Point &p) { return unscale(p); });
    BuildVolume build_volume{ bedptsf, tree_support->m_print_config->printable_height, {}, {} };

    TreeSupport3D::generate_support_areas(*print_object.print(), tree_support, build_volume, { idx }, throw_on_cancel);
}

} // namespace Slic3r
