#include "PerimeterGenerator.hpp"
#include "AABBTreeLines.hpp"
#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Feature/FuzzySkin/FuzzySkin.hpp"
#include "PrintConfig.hpp"
#include "ShortestPath.hpp"
#include "VariableWidth.hpp"
#include "Arachne/WallToolPaths.hpp"
#include "Geometry/ConvexHull.hpp"
#include "ExPolygonCollection.hpp"
#include "Geometry.hpp"
#include "Line.hpp"
#include <cmath>
#include <cassert>
#include <unordered_set>
#include <thread>
#include "libslic3r/AABBTreeLines.hpp"
#include "Print.hpp"
static const int overhang_sampling_number = 6;
static const double narrow_loop_length_threshold = 10;
//BBS: when the width of expolygon is smaller than
//ext_perimeter_width + ext_perimeter_spacing  * (1 - SMALLER_EXT_INSET_OVERLAP_TOLERANCE),
//we think it's small detail area and will generate smaller line width for it
static constexpr double SMALLER_EXT_INSET_OVERLAP_TOLERANCE = 0.22;

namespace Slic3r {
    
using namespace Slic3r::Feature::FuzzySkin;

// Hierarchy of perimeters.
class PerimeterGeneratorLoop {
public:
    // Polygon of this contour.
    Polygon                             polygon;
    // Is it a contour or a hole?
    bool                                is_contour;
    // BBS: is perimeter using smaller width
    bool is_smaller_width_perimeter;
    // Depth in the hierarchy. External perimeter has depth = 0. An external perimeter could be both a contour and a hole.
    unsigned short                      depth;
    // Children contour, may be both CCW and CW oriented (outer contours or holes).
    std::vector<PerimeterGeneratorLoop> children;

    PerimeterGeneratorLoop(const Polygon &polygon, unsigned short depth, bool is_contour, bool is_small_width_perimeter = false) :
        polygon(polygon), is_contour(is_contour), is_smaller_width_perimeter(is_small_width_perimeter), depth(depth) {}
    // External perimeter. It may be CCW or CW oriented (outer contour or hole contour).
    bool is_external() const { return this->depth == 0; }
    // An island, which may have holes, but it does not have another internal island.
    bool is_internal_contour() const;
};

using PerimeterGeneratorLoops = std::vector<PerimeterGeneratorLoop>;

template<class _T>
static bool detect_steep_overhang(const PrintRegionConfig *config,
                                  bool                     is_contour,
                                  const BoundingBox       &extrusion_bboxs,
                                  double                   extrusion_width,
                                  const _T                 extrusion,
                                  const ExPolygons        *lower_slices,
                                  bool                    &steep_overhang_contour,
                                  bool                    &steep_overhang_hole)
{
    double threshold = config->overhang_reverse_threshold.get_abs_value(extrusion_width);
    // Special case: reverse on every even (from GUI POV) layer
    if (threshold < EPSILON) {
        if (is_contour) {
            steep_overhang_contour = true;
        } else {
            steep_overhang_hole = true;
        }

        return true;
    }

    Polygons lower_slcier_chopped = ClipperUtils::clip_clipper_polygons_with_subject_bbox(*lower_slices, extrusion_bboxs, true);

    // All we need to check is whether we have lines outside `threshold`
    double off = threshold - 0.5 * extrusion_width;

    auto limiton_polygons = offset(lower_slcier_chopped, float(scale_(off)));

    auto remain_polylines = diff_pl(extrusion, limiton_polygons);
    if (!remain_polylines.empty()) {
        if (is_contour) {
            steep_overhang_contour = true;
        } else {
            steep_overhang_hole = true;
        }

        return true;
    }

    return false;
}

static ExtrusionEntityCollection traverse_loops(const PerimeterGenerator &perimeter_generator, const PerimeterGeneratorLoops &loops, ThickPolylines &thin_walls,
    bool &steep_overhang_contour, bool &steep_overhang_hole)
{
    // loops is an arrayref of ::Loop objects
    // turn each one into an ExtrusionLoop object
    ExtrusionEntityCollection   coll;
    
    // Detect steep overhangs
    bool overhangs_reverse = perimeter_generator.config->overhang_reverse &&
                             perimeter_generator.layer_id % 2 == 1; // Only calculate overhang degree on even (from GUI POV) layers

    for (const PerimeterGeneratorLoop &loop : loops) {
        bool is_external = loop.is_external();
        bool is_small_width = loop.is_smaller_width_perimeter;

        ExtrusionRole role;
        ExtrusionLoopRole loop_role;
        role = is_external ? erExternalPerimeter : erPerimeter;
        if (loop.is_internal_contour()) {
            // Note that we set loop role to ContourInternalPerimeter
            // also when loop is both internal and external (i.e.
            // there's only one contour loop).
            loop_role = elrInternal;
        } else {
            loop_role = loop.is_contour? elrDefault : elrHole;
        }

        // BBS: get lower polygons series, width, mm3_per_mm
        const std::vector<Polygons> *lower_polygons_series;
        double extrusion_mm3_per_mm;
        double extrusion_width;
        if (is_external) {
            if (is_small_width) {
                //BBS: smaller width external perimeter
                lower_polygons_series = &perimeter_generator.m_smaller_external_lower_polygons_series;
                extrusion_mm3_per_mm = perimeter_generator.smaller_width_ext_mm3_per_mm();
                extrusion_width = perimeter_generator.smaller_ext_perimeter_flow.width();
            } else {
                //BBS: normal external perimeter
                lower_polygons_series = &perimeter_generator.m_external_lower_polygons_series;
                extrusion_mm3_per_mm = perimeter_generator.ext_mm3_per_mm();
                extrusion_width = perimeter_generator.ext_perimeter_flow.width();
            }
        } else {
            //BBS: normal perimeter
            lower_polygons_series = &perimeter_generator.m_lower_polygons_series;
            extrusion_mm3_per_mm = perimeter_generator.mm3_per_mm();
            extrusion_width = perimeter_generator.perimeter_flow.width();
        }

        // Apply fuzzy skin if it is enabled for at least some part of the polygon.
        const Polygon polygon = apply_fuzzy_skin(loop.polygon, perimeter_generator, loop.depth, loop.is_contour);

        ExtrusionPaths paths;
        if (perimeter_generator.config->detect_overhang_wall && perimeter_generator.layer_id > perimeter_generator.object_config->raft_layers) {
            // detect overhanging/bridging perimeters

            // get non 100% overhang paths by intersecting this loop with the grown lower slices
            // prepare grown lower layer slices for overhang detection
            BoundingBox bbox(polygon.points);
            bbox.offset(SCALED_EPSILON);

            // Always reverse extrusion if use fuzzy skin: https://github.com/OrcaSlicer/OrcaSlicer/pull/2413#issuecomment-1769735357
            if (overhangs_reverse && perimeter_generator.has_fuzzy_skin) {
                if (loop.is_contour) {
                    steep_overhang_contour = true;
                } else if (perimeter_generator.has_fuzzy_hole) {
                    steep_overhang_hole = true;
                }
            }
            // Detect steep overhang
            // Skip the check if we already found steep overhangs
            bool found_steep_overhang = (loop.is_contour && steep_overhang_contour) || (!loop.is_contour && steep_overhang_hole);
            if (overhangs_reverse && !found_steep_overhang) {
                detect_steep_overhang(perimeter_generator.config, loop.is_contour, bbox, extrusion_width, Polygons{polygon}, perimeter_generator.lower_slices,
                                      steep_overhang_contour, steep_overhang_hole);
            }

            Polylines remain_polines;

            Polygons lower_polygons_series_clipped = ClipperUtils::clip_clipper_polygons_with_subject_bbox(lower_polygons_series->back(), bbox);

            Polylines inside_polines = intersection_pl({polygon}, lower_polygons_series_clipped);


            remain_polines = diff_pl({polygon}, lower_polygons_series_clipped);

            if (!inside_polines.empty())
                extrusion_paths_append(
                    paths,
                    std::move(inside_polines),
                    role,
                    extrusion_mm3_per_mm,
                    extrusion_width,
                    (float)perimeter_generator.layer_height);
            
            // get 100% overhang paths by checking what parts of this loop fall
            // outside the grown lower slices (thus where the distance between
            // the loop centerline and original lower slices is >= half nozzle diameter
            if (remain_polines.size() != 0) {
                extrusion_paths_append(paths, std::move(remain_polines),
                                       erOverhangPerimeter, perimeter_generator.mm3_per_mm_overhang(),
                                       perimeter_generator.overhang_flow.width(),
                                       perimeter_generator.overhang_flow.height());
            }

            // Reapply the nearest point search for starting point.
            // We allow polyline reversal because Clipper may have randomly reversed polylines during clipping.
            if(paths.empty()) continue;
            chain_and_reorder_extrusion_paths(paths, &paths.front().first_point());
        } else {
            if (overhangs_reverse && perimeter_generator.layer_id > perimeter_generator.object_config->raft_layers) {
                // Always reverse if detect overhang wall is not enabled
                steep_overhang_contour = true;
                steep_overhang_hole    = true;
            }

            ExtrusionPath path(role);
            //BBS.
            path.polyline = polygon.split_at_first_point();
            path.mm3_per_mm = extrusion_mm3_per_mm;
            path.width = extrusion_width;
            path.height     = (float)perimeter_generator.layer_height;
            paths.emplace_back(std::move(path));
        }

        coll.append(ExtrusionLoop(std::move(paths), loop_role));
    }

    // Append thin walls to the nearest-neighbor search (only for first iteration)
    if (! thin_walls.empty()) {
        variable_width(thin_walls, erExternalPerimeter, perimeter_generator.ext_perimeter_flow, coll.entities);
        thin_walls.clear();
    }

    // Traverse children and build the final collection.
	Point zero_point(0, 0);
	std::vector<std::pair<size_t, bool>> chain = chain_extrusion_entities(coll.entities, &zero_point);
    ExtrusionEntityCollection out;
    for (const std::pair<size_t, bool> &idx : chain) {
		assert(coll.entities[idx.first] != nullptr);
        if (idx.first >= loops.size()) {
            // This is a thin wall.
			out.entities.reserve(out.entities.size() + 1);
            out.entities.emplace_back(coll.entities[idx.first]);
			coll.entities[idx.first] = nullptr;
            if (idx.second)
				out.entities.back()->reverse();
        } else {
            const PerimeterGeneratorLoop &loop = loops[idx.first];
            assert(thin_walls.empty());
            ExtrusionEntityCollection children = traverse_loops(perimeter_generator, loop.children, thin_walls, steep_overhang_contour, steep_overhang_hole);
            out.entities.reserve(out.entities.size() + children.entities.size() + 1);
            ExtrusionLoop *eloop = static_cast<ExtrusionLoop*>(coll.entities[idx.first]);
            coll.entities[idx.first] = nullptr;

            eloop->make_counter_clockwise();
            eloop->inset_idx = loop.depth;
            if (loop.is_contour) {
                out.append(std::move(children.entities));
                out.entities.emplace_back(eloop);
            } else {
                out.entities.emplace_back(eloop);
                out.append(std::move(children.entities));
            }
        }
    }
    return out;
}

static ClipperLib_Z::Paths clip_extrusion(const ClipperLib_Z::Path& subject, const ClipperLib_Z::Paths& clip, ClipperLib_Z::ClipType clipType)
{
    ClipperLib_Z::Clipper clipper;
    clipper.ZFillFunction([](const ClipperLib_Z::IntPoint& e1bot, const ClipperLib_Z::IntPoint& e1top, const ClipperLib_Z::IntPoint& e2bot,
        const ClipperLib_Z::IntPoint& e2top, ClipperLib_Z::IntPoint& pt) {
            ClipperLib_Z::IntPoint start = e1bot;
            ClipperLib_Z::IntPoint end = e1top;

            if (start.z() <= 0 && end.z() <= 0) {
                start = e2bot;
                end = e2top;
            }

            assert(start.z() >= 0 && end.z() >= 0);

            // Interpolate extrusion line width.
            double length_sqr = (end - start).cast<double>().squaredNorm();
            double dist_sqr = (pt - start).cast<double>().squaredNorm();
            double t = std::sqrt(dist_sqr / length_sqr);

            pt.z() = start.z() + coord_t((end.z() - start.z()) * t);
        });

    clipper.AddPath(subject, ClipperLib_Z::ptSubject, false);
    clipper.AddPaths(clip, ClipperLib_Z::ptClip, true);

    ClipperLib_Z::Paths    clipped_paths;
    {
        ClipperLib_Z::PolyTree clipped_polytree;
        clipper.Execute(clipType, clipped_polytree, ClipperLib_Z::pftNonZero, ClipperLib_Z::pftNonZero);
        ClipperLib_Z::PolyTreeToPaths(std::move(clipped_polytree), clipped_paths);
    }

    // Clipped path could contain vertices from the clip with a Z coordinate equal to zero.
    // For those vertices, we must assign value based on the subject.
    // This happens only in sporadic cases.
    for (ClipperLib_Z::Path& path : clipped_paths)
        for (ClipperLib_Z::IntPoint& c_pt : path)
            if (c_pt.z() == 0) {
                // Now we must find the corresponding line on with this point is located and compute line width (Z coordinate).
                if (subject.size() <= 2)
                    continue;

                const Point pt(c_pt.x(), c_pt.y());
                Point       projected_pt_min;
                auto        it_min = subject.begin();
                auto        dist_sqr_min = std::numeric_limits<double>::max();
                Point       prev(subject.front().x(), subject.front().y());
                for (auto it = std::next(subject.begin()); it != subject.end(); ++it) {
                    Point curr(it->x(), it->y());
                    Point projected_pt = pt.projection_onto(Line(prev, curr));
                    if (double dist_sqr = (projected_pt - pt).cast<double>().squaredNorm(); dist_sqr < dist_sqr_min) {
                        dist_sqr_min = dist_sqr;
                        projected_pt_min = projected_pt;
                        it_min = std::prev(it);
                    }
                    prev = curr;
                }

                assert(dist_sqr_min <= SCALED_EPSILON);
                assert(std::next(it_min) != subject.end());

                const Point  pt_a(it_min->x(), it_min->y());
                const Point  pt_b(std::next(it_min)->x(), std::next(it_min)->y());
                const double line_len = (pt_b - pt_a).cast<double>().norm();
                const double dist = (projected_pt_min - pt_a).cast<double>().norm();
                c_pt.z() = coord_t(double(it_min->z()) + (dist / line_len) * double(std::next(it_min)->z() - it_min->z()));
            }

    assert([&clipped_paths = std::as_const(clipped_paths)]() -> bool {
        for (const ClipperLib_Z::Path& path : clipped_paths)
            for (const ClipperLib_Z::IntPoint& pt : path)
                if (pt.z() <= 0)
                    return false;
        return true;
    }());

    return clipped_paths;
}

struct PerimeterGeneratorArachneExtrusion
{
    Arachne::ExtrusionLine* extrusion = nullptr;
    // Indicates if closed ExtrusionLine is a contour or a hole. Used it only when ExtrusionLine is a closed loop.
    bool is_contour = false;
};

static ExtrusionEntityCollection traverse_extrusions(const PerimeterGenerator& perimeter_generator, std::vector<PerimeterGeneratorArachneExtrusion>& pg_extrusions,
    bool &steep_overhang_contour, bool &steep_overhang_hole)
{
    // Detect steep overhangs
    bool overhangs_reverse = perimeter_generator.config->overhang_reverse &&
                             perimeter_generator.layer_id % 2 == 1;  // Only calculate overhang degree on even (from GUI POV) layers

    ExtrusionEntityCollection extrusion_coll;
    for (PerimeterGeneratorArachneExtrusion& pg_extrusion : pg_extrusions) {
        Arachne::ExtrusionLine* extrusion = pg_extrusion.extrusion;
        if (extrusion->empty())
            continue;

        const bool    is_external = extrusion->inset_idx == 0;
        ExtrusionRole role = is_external ? erExternalPerimeter : erPerimeter;

        const bool  is_contour = !extrusion->is_closed || pg_extrusion.is_contour;
        apply_fuzzy_skin(extrusion, perimeter_generator, is_contour);

        ExtrusionPaths paths;
        // detect overhanging/bridging perimeters
        if (perimeter_generator.config->detect_overhang_wall && perimeter_generator.layer_id > perimeter_generator.object_config->raft_layers) {
            ClipperLib_Z::Path extrusion_path;
            extrusion_path.reserve(extrusion->size());
            BoundingBox extrusion_path_bbox;
            for (const Arachne::ExtrusionJunction &ej : extrusion->junctions) {
                extrusion_path.emplace_back(ej.p.x(), ej.p.y(), ej.w);
                extrusion_path_bbox.merge(Point(ej.p.x(), ej.p.y()));
            }

            ClipperLib_Z::Paths lower_slices_paths;
            {
                lower_slices_paths.reserve(perimeter_generator.lower_slices_polygons().size());
                Points clipped;
                extrusion_path_bbox.offset(SCALED_EPSILON);
                for (const Polygon &poly : perimeter_generator.lower_slices_polygons()) {
                    clipped.clear();
                    ClipperUtils::clip_clipper_polygon_with_subject_bbox(poly.points, extrusion_path_bbox, clipped);
                    if (!clipped.empty()) {
                        lower_slices_paths.emplace_back();
                        ClipperLib_Z::Path &out = lower_slices_paths.back();
                        out.reserve(clipped.size());
                        for (const Point &pt : clipped)
                          out.emplace_back(pt.x(), pt.y(), 0);
                    }
                }
            }

            // get non-overhang paths by intersecting this loop with the grown lower slices
            extrusion_paths_append(paths, clip_extrusion(extrusion_path, lower_slices_paths, ClipperLib_Z::ctIntersection), role,
                                   is_external ? perimeter_generator.ext_perimeter_flow : perimeter_generator.perimeter_flow);

            // Always reverse extrusion if use fuzzy skin: https://github.com/OrcaSlicer/OrcaSlicer/pull/2413#issuecomment-1769735357
            if (overhangs_reverse && perimeter_generator.has_fuzzy_skin) {
                if (pg_extrusion.is_contour) {
                    steep_overhang_contour = true;
                } else if (perimeter_generator.has_fuzzy_hole) {
                    steep_overhang_hole = true;
                }
            }
            // Detect steep overhang
            // Skip the check if we already found steep overhangs
            bool found_steep_overhang = (pg_extrusion.is_contour && steep_overhang_contour) || (!pg_extrusion.is_contour && steep_overhang_hole);
            if (overhangs_reverse && !found_steep_overhang) {
                std::map<double, ExtrusionPaths> recognization_paths;
                for (const ExtrusionPath &path : paths) {
                    if (recognization_paths.count(path.width))
                        recognization_paths[path.width].emplace_back(std::move(path));
                    else
                        recognization_paths.insert(std::pair<double, ExtrusionPaths>(path.width, {std::move(path)}));
                }
                for (const auto &it : recognization_paths) {
                    Polylines be_clipped;

                    for (const ExtrusionPath &p : it.second) {
                        be_clipped.emplace_back(std::move(p.polyline));
                    }

                    BoundingBox extrusion_bboxs = get_extents(be_clipped);

                    if (detect_steep_overhang(perimeter_generator.config, pg_extrusion.is_contour, extrusion_bboxs, it.first, be_clipped, perimeter_generator.lower_slices,
                        steep_overhang_contour, steep_overhang_hole)) {
                        break;
                    }
                }
            }

            // get overhang paths by checking what parts of this loop fall
            // outside the grown lower slices (thus where the distance between
            // the loop centerline and original lower slices is >= half nozzle diameter
            extrusion_paths_append(paths, clip_extrusion(extrusion_path, lower_slices_paths, ClipperLib_Z::ctDifference), erOverhangPerimeter,
                perimeter_generator.overhang_flow);

            // Reapply the nearest point search for starting point.
            // We allow polyline reversal because Clipper may have randomly reversed polylines during clipping.
            // Arachne sometimes creates extrusion with zero-length (just two same endpoints);
            if (!paths.empty()) {
                Point start_point = paths.front().first_point();
                if (!extrusion->is_closed) {
                    // Especially for open extrusion, we need to select a starting point that is at the start
                    // or the end of the extrusions to make one continuous line. Also, we prefer a non-overhang
                    // starting point.
                    struct PointInfo
                    {
                        size_t occurrence = 0;
                        bool   is_overhang = false;
                    };
                    std::unordered_map<Point, PointInfo, PointHash> point_occurrence;
                    for (const ExtrusionPath& path : paths) {
                        ++point_occurrence[path.polyline.first_point()].occurrence;
                        ++point_occurrence[path.polyline.last_point()].occurrence;
                        if (path.role() == erOverhangPerimeter) {
                            point_occurrence[path.polyline.first_point()].is_overhang = true;
                            point_occurrence[path.polyline.last_point()].is_overhang = true;
                        }
                    }

                    // Prefer non-overhang point as a starting point.
                    for (const std::pair<Point, PointInfo> pt : point_occurrence)
                        if (pt.second.occurrence == 1) {
                            start_point = pt.first;
                            if (!pt.second.is_overhang) {
                                start_point = pt.first;
                                break;
                            }
                        }
                }

                chain_and_reorder_extrusion_paths(paths, &start_point);

                if (overhangs_reverse) {
                    for (const ExtrusionPath& path : paths) {
                        if (path.role() == erOverhangPerimeter) {
                            if (pg_extrusion.is_contour)
                                steep_overhang_contour = true;
                            else
                                steep_overhang_hole = true;
                            break;
                        }
                    }
                }
            }
        }
        else {
            if (overhangs_reverse && perimeter_generator.layer_id > perimeter_generator.object_config->raft_layers) {
                // Always reverse if detect overhang wall is not enabled
                steep_overhang_contour = true;
                steep_overhang_hole    = true;
            }

            extrusion_paths_append(paths, *extrusion, role, is_external ? perimeter_generator.ext_perimeter_flow : perimeter_generator.perimeter_flow);
        }

        // Append paths to collection.
        if (!paths.empty()) {
            if (extrusion->is_closed) {
                ExtrusionLoop extrusion_loop(std::move(paths), pg_extrusion.is_contour ? elrDefault : elrHole);
                extrusion_loop.make_counter_clockwise();
                // TODO: it seems in practice that ExtrusionLoops occasionally have significantly disconnected paths,
                // triggering the asserts below. Is this a problem?
                for (auto it = std::next(extrusion_loop.paths.begin()); it != extrusion_loop.paths.end(); ++it) {
                    assert(it->polyline.points.size() >= 2);
                    assert(std::prev(it)->polyline.last_point() == it->polyline.first_point());
                }
                assert(extrusion_loop.paths.front().first_point() == extrusion_loop.paths.back().last_point());

                extrusion_coll.append(std::move(extrusion_loop));
            }
            else {
                // Because we are processing one ExtrusionLine all ExtrusionPaths should form one connected path.
                // But there is possibility that due to numerical issue there is poss
                // TODO: do we need some tolerance for disconnected paths below?
                for (auto it = std::next(paths.begin()); it != paths.end(); ++it) {
                    assert(it->polyline.points.size() >= 2);
                    assert(std::prev(it)->polyline.last_point() == it->polyline.first_point());
                }
                ExtrusionMultiPath multi_path;
                multi_path.paths.emplace_back(std::move(paths.front()));

                for (auto it_path = std::next(paths.begin()); it_path != paths.end(); ++it_path) {
                    if (multi_path.paths.back().last_point() != it_path->first_point()) {
                        extrusion_coll.append(ExtrusionMultiPath(std::move(multi_path)));
                        multi_path = ExtrusionMultiPath();
                    }
                    multi_path.paths.emplace_back(std::move(*it_path));
                }

                extrusion_coll.append(ExtrusionMultiPath(std::move(multi_path)));
            }
        }
    }

    return extrusion_coll;
}

void PerimeterGenerator::split_top_surfaces(const ExPolygons &orig_polygons, ExPolygons &top_fills,
                                            ExPolygons &non_top_polygons, ExPolygons &fill_clip) const {
    // other perimeters
    coord_t perimeter_width = this->perimeter_flow.scaled_width();
    coord_t perimeter_spacing = this->perimeter_flow.scaled_spacing();

    // external perimeters
    coord_t ext_perimeter_width = this->ext_perimeter_flow.scaled_width();
    coord_t ext_perimeter_spacing = this->ext_perimeter_flow.scaled_spacing();

    bool has_gap_fill = this->config->gap_infill_speed.value > 0;

    // split the polygons with top/not_top
    // get the offset from solid surface anchor
    coord_t offset_top_surface =
        scale_(0.9 * (config->wall_loops.value == 0
                          ? 0.
                          : unscaled(double(ext_perimeter_width +
                                            perimeter_spacing * int(int(config->wall_loops.value) - int(1))))));
    // if possible, try to not push the extra perimeters inside the sparse infill
    if (offset_top_surface >
        0.9 * (config->wall_loops.value <= 1 ? 0. : (perimeter_spacing * (config->wall_loops.value - 1))))
        offset_top_surface -=
            coord_t(0.9 * (config->wall_loops.value <= 1 ? 0. : (perimeter_spacing * (config->wall_loops.value - 1))));
    else
        offset_top_surface = 0;
    // don't takes into account too thin areas
    // get boungding box of last
    BoundingBox last_box = get_extents(orig_polygons);
    last_box.offset(SCALED_EPSILON);

    // skip if the exposed area is smaller than "min_width_top_surface"
    double min_width_top_surface = std::max(double(ext_perimeter_spacing / 2. + 10), scale_(config->min_width_top_surface.get_abs_value(unscale_(perimeter_width))));

    // get the Polygons upper the polygon this layer
    Polygons upper_polygons_series_clipped;
    if (object_config->interface_shells) {
        auto upper_slicer_same_region = to_expolygons(this->upper_slices_same_region->surfaces);
        upper_polygons_series_clipped = ClipperUtils::clip_clipper_polygons_with_subject_bbox(upper_slicer_same_region, last_box);
    } else
        upper_polygons_series_clipped = ClipperUtils::clip_clipper_polygons_with_subject_bbox(*this->upper_slices, last_box);

    upper_polygons_series_clipped          = offset(upper_polygons_series_clipped, min_width_top_surface);

    // set the clip to a virtual "second perimeter"
    fill_clip = offset_ex(orig_polygons, -double(ext_perimeter_spacing));
    // get the real top surface
    ExPolygons grown_lower_slices;
    ExPolygons bridge_checker;
    auto nozzle_diameter = this->print_config->nozzle_diameter.get_at(this->config->wall_filament - 1);
    // Check whether surface be bridge or not
    if (this->lower_slices != NULL) {
        // BBS: get the Polygons below the polygon this layer
        Polygons lower_polygons_series_clipped =
            ClipperUtils::clip_clipper_polygons_with_subject_bbox(*this->lower_slices, last_box);
        double bridge_offset = std::max(double(ext_perimeter_spacing), (double(perimeter_width)));
        // SoftFever: improve bridging
        const float bridge_margin =
            std::min(float(scale_(BRIDGE_INFILL_MARGIN)), float(scale_(nozzle_diameter * BRIDGE_INFILL_MARGIN / 0.4)));
        bridge_checker = offset_ex(diff_ex(orig_polygons, lower_polygons_series_clipped, ApplySafetyOffset::Yes),
                                   1.5 * bridge_offset + bridge_margin + perimeter_spacing / 2.);
    }
    ExPolygons delete_bridge = diff_ex(orig_polygons, bridge_checker, ApplySafetyOffset::Yes);

    ExPolygons top_polygons = diff_ex(delete_bridge, upper_polygons_series_clipped, ApplySafetyOffset::Yes);
    // get the not-top surface, from the "real top" but enlarged by external_infill_margin (and the
    // min_width_top_surface we removed a bit before)
    ExPolygons temp_gap = diff_ex(top_polygons, fill_clip);
    ExPolygons inner_polygons =
        diff_ex(orig_polygons,
                offset_ex(top_polygons, offset_top_surface + min_width_top_surface - double(ext_perimeter_spacing / 2.)),
                ApplySafetyOffset::Yes);
    // get the enlarged top surface, by using inner_polygons instead of upper_slices, and clip it for it to be exactly
    // the polygons to fill.
    top_polygons = diff_ex(fill_clip, inner_polygons, ApplySafetyOffset::Yes);
    // increase by half peri the inner space to fill the frontier between last and stored.
    top_fills = union_ex(top_fills, top_polygons);
    //set the clip to the external wall but go back inside by infill_extrusion_width/2 to be sure the extrusion won't go outside even with a 100% overlap.
    double infill_spacing_unscaled = this->config->sparse_infill_line_width.get_abs_value(nozzle_diameter);
    if (infill_spacing_unscaled == 0) infill_spacing_unscaled = Flow::auto_extrusion_width(frInfill, nozzle_diameter);
    fill_clip = offset_ex(orig_polygons, double(ext_perimeter_spacing / 2.) - scale_(infill_spacing_unscaled / 2.));
    // ExPolygons oldLast = last;

    non_top_polygons = intersection_ex(inner_polygons, orig_polygons);
    if (has_gap_fill)
        non_top_polygons = union_ex(non_top_polygons, temp_gap);
    //{
    //    std::stringstream stri;
    //    stri << this->layer_id << "_1_"<< i <<"_only_one_peri"<< ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(to_polylines(top_fills), "green");
    //    svg.draw(to_polylines(inner_polygons), "yellow");
    //    svg.draw(to_polylines(top_polygons), "cyan");
    //    svg.draw(to_polylines(oldLast), "orange");
    //    svg.draw(to_polylines(last), "red");
    //    svg.Close();
    //}
}

// Port "extra perimeters on overhangs" from PrusaSlicer. Original author: PavelMikus pavel.mikus.mail@seznam.cz
// Based on: https://github.com/prusa3d/PrusaSlicer/blob/c05542590d7c2d73eb69bbf7a82a482a075815c1/src/libslic3r/PerimeterGenerator.cpp#L667-L1071
// find out if paths touch - at least one point of one path is within limit distance of second path
bool paths_touch(const ExtrusionPath &path_one, const ExtrusionPath &path_two, double limit_distance)
{
    AABBTreeLines::LinesDistancer<Line> lines_two{path_two.as_polyline().lines()};
    for (size_t pt_idx = 0; pt_idx < path_one.polyline.size(); pt_idx++) {
        if (lines_two.distance_from_lines<false>(path_one.polyline.points[pt_idx]) < limit_distance) { return true; }
    }
    AABBTreeLines::LinesDistancer<Line> lines_one{path_one.as_polyline().lines()};
    for (size_t pt_idx = 0; pt_idx < path_two.polyline.size(); pt_idx++) {
        if (lines_one.distance_from_lines<false>(path_two.polyline.points[pt_idx]) < limit_distance) { return true; }
    }
    return false;
}

Polylines reconnect_polylines(const Polylines &polylines, double limit_distance)
{
    if (polylines.empty())
        return polylines;

    std::unordered_map<size_t, Polyline> connected;
    connected.reserve(polylines.size());
    for (size_t i = 0; i < polylines.size(); i++) {
        if (!polylines[i].empty()) {
            connected.emplace(i, polylines[i]);
        }
    }

    for (size_t a = 0; a < polylines.size(); a++) {
        if (connected.find(a) == connected.end()) {
            continue;
        }
        Polyline &base = connected.at(a);
        for (size_t b = a + 1; b < polylines.size(); b++) {
            if (connected.find(b) == connected.end()) {
                continue;
            }
            Polyline &next = connected.at(b);
            if ((base.last_point() - next.first_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                base.append(std::move(next));
                connected.erase(b);
            } else if ((base.last_point() - next.last_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                base.points.insert(base.points.end(), next.points.rbegin(), next.points.rend());
                connected.erase(b);
            } else if ((base.first_point() - next.last_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                next.append(std::move(base));
                base = std::move(next);
                base.reverse();
                connected.erase(b);
            } else if ((base.first_point() - next.first_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                base.reverse();
                base.append(std::move(next));
                base.reverse();
                connected.erase(b);
            }
        }
    }

    Polylines result;
    for (auto &ext : connected) {
        result.push_back(std::move(ext.second));
    }

    return result;
}

ExtrusionPaths sort_extra_perimeters(const ExtrusionPaths& extra_perims, int index_of_first_unanchored, double extrusion_spacing)
{
    if (extra_perims.empty()) return {};

    std::vector<std::unordered_set<size_t>> dependencies(extra_perims.size());
    for (size_t path_idx = 0; path_idx < extra_perims.size(); path_idx++) {
        for (size_t prev_path_idx = 0; prev_path_idx < path_idx; prev_path_idx++) {
            if (paths_touch(extra_perims[path_idx], extra_perims[prev_path_idx], extrusion_spacing * 1.5f)) {
                       dependencies[path_idx].insert(prev_path_idx);        
            }
        }
    }

    std::vector<bool> processed(extra_perims.size(), false);
    for (int path_idx = 0; path_idx < index_of_first_unanchored; path_idx++) {
        processed[path_idx] = true;
    }

    for (size_t i = index_of_first_unanchored; i < extra_perims.size(); i++) {
        bool change = false;
        for (size_t path_idx = index_of_first_unanchored; path_idx < extra_perims.size(); path_idx++) {
            if (processed[path_idx])
                       continue;
            auto processed_dep = std::find_if(dependencies[path_idx].begin(), dependencies[path_idx].end(),
                                              [&](size_t dep) { return processed[dep]; });
            if (processed_dep != dependencies[path_idx].end()) {
                for (auto it = dependencies[path_idx].begin(); it != dependencies[path_idx].end();) {
                    if (!processed[*it]) {
                        dependencies[*it].insert(path_idx);
                        dependencies[path_idx].erase(it++);
                    } else {
                        ++it;
                    }
                }
                processed[path_idx] = true;
                change              = true;
            }
        }
        if (!change) {
            break;
        }
    }

    Point current_point = extra_perims.begin()->first_point();

    ExtrusionPaths sorted_paths{};
    size_t         null_idx = size_t(-1);
    size_t         next_idx = null_idx;
    bool           reverse  = false;
    while (true) {
        if (next_idx == null_idx) { // find next pidx to print
            double dist = std::numeric_limits<double>::max();
            for (size_t path_idx = 0; path_idx < extra_perims.size(); path_idx++) {
                if (!dependencies[path_idx].empty())
                    continue;
                const auto &path   = extra_perims[path_idx];
                double      dist_a = (path.first_point() - current_point).cast<double>().squaredNorm();
                if (dist_a < dist) {
                    dist     = dist_a;
                    next_idx = path_idx;
                    reverse  = false;
                }
                double dist_b = (path.last_point() - current_point).cast<double>().squaredNorm();
                if (dist_b < dist) {
                    dist     = dist_b;
                    next_idx = path_idx;
                    reverse  = true;
                }
            }
            if (next_idx == null_idx) {
                       break;
            }
        } else {
            // we have valid next_idx, add it to the sorted paths, update dependencies, update current point and potentialy set new next_idx
            ExtrusionPath path = extra_perims[next_idx];
            if (reverse) {
                path.reverse();
            }
            sorted_paths.push_back(path);
            assert(dependencies[next_idx].empty());
            dependencies[next_idx].insert(null_idx);
            current_point = sorted_paths.back().last_point();
            for (size_t path_idx = 0; path_idx < extra_perims.size(); path_idx++) {
                dependencies[path_idx].erase(next_idx);
            }
            double dist = std::numeric_limits<double>::max();
            next_idx    = null_idx;

            for (size_t path_idx = next_idx + 1; path_idx < extra_perims.size(); path_idx++) {
                if (!dependencies[path_idx].empty()) {
                    continue;
                }
                const ExtrusionPath &next_path = extra_perims[path_idx];
                double               dist_a    = (next_path.first_point() - current_point).cast<double>().squaredNorm();
                if (dist_a < dist) {
                    dist     = dist_a;
                    next_idx = path_idx;
                    reverse  = false;
                }
                double dist_b = (next_path.last_point() - current_point).cast<double>().squaredNorm();
                if (dist_b < dist) {
                    dist     = dist_b;
                    next_idx = path_idx;
                    reverse  = true;
                }
            }
            if (dist > scaled(5.0)) {
                next_idx = null_idx;
            }
        }
    }

    ExtrusionPaths reconnected;
    reconnected.reserve(sorted_paths.size());
    for (const ExtrusionPath &path : sorted_paths) {
        if (!reconnected.empty() && (reconnected.back().last_point() - path.first_point()).cast<double>().squaredNorm() <
                                        extrusion_spacing * extrusion_spacing * 4.0) {
            reconnected.back().polyline.points.insert(reconnected.back().polyline.points.end(), path.polyline.points.begin(),
                                                      path.polyline.points.end());
        } else {
            reconnected.push_back(path);
        }
    }

    ExtrusionPaths filtered;
    filtered.reserve(reconnected.size());
    for (ExtrusionPath &p : reconnected) {
        if (p.length() > 3 * extrusion_spacing) {
            filtered.push_back(p);
        }
    }

    return filtered;
}

#define EXTRA_PERIMETER_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.
// #define EXTRA_PERIM_DEBUG_FILES
// Function will generate extra perimeters clipped over nonbridgeable areas of the provided surface and returns both the new perimeters and
// Polygons filled by those clipped perimeters
std::tuple<std::vector<ExtrusionPaths>, Polygons> generate_extra_perimeters_over_overhangs(ExPolygons               infill_area,
                                                                                           const Polygons          &lower_slices_polygons,
                                                                                           int                      perimeter_count,
                                                                                           const Flow              &overhang_flow,
                                                                                           double                   scaled_resolution,
                                                                                           const PrintObjectConfig &object_config,
                                                                                           const PrintConfig       &print_config)
{
    coord_t anchors_size = std::min(coord_t(scale_(EXTERNAL_INFILL_MARGIN)), overhang_flow.scaled_spacing() * (perimeter_count + 1));

    BoundingBox infill_area_bb = get_extents(infill_area).inflated(SCALED_EPSILON);
    Polygons optimized_lower_slices = ClipperUtils::clip_clipper_polygons_with_subject_bbox(lower_slices_polygons, infill_area_bb);
    Polygons overhangs  = diff(infill_area, optimized_lower_slices);

    if (overhangs.empty()) { return {}; }

    AABBTreeLines::LinesDistancer<Line> lower_layer_aabb_tree{to_lines(optimized_lower_slices)};
    Polygons                            anchors             = intersection(infill_area, optimized_lower_slices);
    Polygons                            inset_anchors       = diff(anchors,
                                                                   expand(overhangs, anchors_size + 0.1 * overhang_flow.scaled_width(), EXTRA_PERIMETER_OFFSET_PARAMETERS));
    Polygons                            inset_overhang_area = diff(infill_area, inset_anchors);

#ifdef EXTRA_PERIM_DEBUG_FILES
    {
        BoundingBox bbox = get_extents(inset_overhang_area);
        bbox.offset(scale_(1.));
        ::Slic3r::SVG svg(debug_out_path("inset_overhang_area").c_str(), bbox);
        for (const Line &line : to_lines(inset_anchors)) svg.draw(line, "purple", scale_(0.25));
        for (const Line &line : to_lines(inset_overhang_area)) svg.draw(line, "red", scale_(0.15));
        svg.Close();
    }
#endif

    Polygons inset_overhang_area_left_unfilled;

    std::vector<ExtrusionPaths> extra_perims; // overhang region -> extrusion paths
    for (const ExPolygon &overhang : union_ex(to_expolygons(inset_overhang_area))) {
        Polygons overhang_to_cover = to_polygons(overhang);
        Polygons expanded_overhang_to_cover = expand(overhang_to_cover, 1.1 * overhang_flow.scaled_spacing());
        Polygons shrinked_overhang_to_cover = shrink(overhang_to_cover, 0.1 * overhang_flow.scaled_spacing());

        Polygons real_overhang = intersection(overhang_to_cover, overhangs);
        if (real_overhang.empty()) {
            inset_overhang_area_left_unfilled.insert(inset_overhang_area_left_unfilled.end(), overhang_to_cover.begin(),
                                                     overhang_to_cover.end());
            continue;
        }
        ExtrusionPaths &overhang_region = extra_perims.emplace_back();

        Polygons anchoring         = intersection(expanded_overhang_to_cover, inset_anchors);
        Polygons perimeter_polygon = offset(union_(expand(overhang_to_cover, 0.1 * overhang_flow.scaled_spacing()), anchoring),
                                            -overhang_flow.scaled_spacing() * 0.6);

        Polygon anchoring_convex_hull = Geometry::convex_hull(anchoring);
        double  unbridgeable_area     = area(diff(real_overhang, {anchoring_convex_hull}));

        auto [dir, unsupp_dist] = detect_bridging_direction(real_overhang, anchors);

#ifdef EXTRA_PERIM_DEBUG_FILES
        {
            BoundingBox bbox = get_extents(anchoring_convex_hull);
            bbox.offset(scale_(1.));
            ::Slic3r::SVG svg(debug_out_path("bridge_check").c_str(), bbox);
            for (const Line &line : to_lines(perimeter_polygon)) svg.draw(line, "purple", scale_(0.25));
            for (const Line &line : to_lines(real_overhang)) svg.draw(line, "red", scale_(0.20));
            for (const Line &line : to_lines(anchoring_convex_hull)) svg.draw(line, "green", scale_(0.15));
            for (const Line &line : to_lines(anchoring)) svg.draw(line, "yellow", scale_(0.10));
            for (const Line &line : to_lines(diff_ex(perimeter_polygon, {anchoring_convex_hull}))) svg.draw(line, "black", scale_(0.10));
            for (const Line &line : to_lines(diff_pl(to_polylines(diff(real_overhang, anchors)), expand(anchors, float(SCALED_EPSILON)))))
                svg.draw(line, "blue", scale_(0.30));
            svg.Close();
        }
#endif

        if (unbridgeable_area < 0.2 * area(real_overhang) && unsupp_dist < total_length(real_overhang) * 0.2) {
            inset_overhang_area_left_unfilled.insert(inset_overhang_area_left_unfilled.end(),overhang_to_cover.begin(),overhang_to_cover.end());
            perimeter_polygon.clear();
        } else {
            //  fill the overhang with perimeters
            int continuation_loops = 2;
            while (continuation_loops >= 0) {
                auto prev = perimeter_polygon;
                // prepare next perimeter lines
                Polylines perimeter = intersection_pl(to_polylines(perimeter_polygon), shrinked_overhang_to_cover);

                // do not add the perimeter to result yet, first check if perimeter_polygon is not empty after shrinking - this would mean
                //  that the polygon was possibly too small for full perimeter loop and in that case try gap fill first
                perimeter_polygon = union_(perimeter_polygon, anchoring);
                perimeter_polygon = intersection(offset(perimeter_polygon, -overhang_flow.scaled_spacing()), expanded_overhang_to_cover);

                if (perimeter_polygon.empty()) { // fill possible gaps of single extrusion width
                    Polygons shrinked = intersection(offset(prev, -0.3 * overhang_flow.scaled_spacing()), expanded_overhang_to_cover);
                    if (!shrinked.empty()) {
                        extrusion_paths_append(overhang_region, reconnect_polylines(perimeter, overhang_flow.scaled_spacing()),
                                               ExtrusionRole::erOverhangPerimeter, overhang_flow.mm3_per_mm(), overhang_flow.width(),
                                               overhang_flow.height());
                    }

                    Polylines  fills;
                    ExPolygons gap = shrinked.empty() ? offset_ex(prev, overhang_flow.scaled_spacing() * 0.5) : to_expolygons(shrinked);

                    for (const ExPolygon &ep : gap) {
                        ep.medial_axis(0.75 * overhang_flow.scaled_width(), 3.0 * overhang_flow.scaled_spacing(), &fills);
                    }
                    if (!fills.empty()) {
                        fills = intersection_pl(fills, shrinked_overhang_to_cover);
                        extrusion_paths_append(overhang_region, reconnect_polylines(fills, overhang_flow.scaled_spacing()),
                                               ExtrusionRole::erOverhangPerimeter, overhang_flow.mm3_per_mm(), overhang_flow.width(),
                                               overhang_flow.height());
                    }
                    break;
                } else {
                    extrusion_paths_append(overhang_region, reconnect_polylines(perimeter, overhang_flow.scaled_spacing()),
                                           ExtrusionRole::erOverhangPerimeter, overhang_flow.mm3_per_mm(), overhang_flow.width(),
                                           overhang_flow.height());
                }

                if (intersection(perimeter_polygon, real_overhang).empty()) { continuation_loops--; }

                if (prev == perimeter_polygon) {
#ifdef EXTRA_PERIM_DEBUG_FILES
                    BoundingBox bbox = get_extents(perimeter_polygon);
                    bbox.offset(scale_(5.));
                    ::Slic3r::SVG svg(debug_out_path("perimeter_polygon").c_str(), bbox);
                    for (const Line &line : to_lines(perimeter_polygon)) svg.draw(line, "blue", scale_(0.25));
                    for (const Line &line : to_lines(overhang_to_cover)) svg.draw(line, "red", scale_(0.20));
                    for (const Line &line : to_lines(real_overhang)) svg.draw(line, "green", scale_(0.15));
                    for (const Line &line : to_lines(anchoring)) svg.draw(line, "yellow", scale_(0.10));
                    svg.Close();
#endif
                    break;
                }
            }

            perimeter_polygon = expand(perimeter_polygon, 0.5 * overhang_flow.scaled_spacing());
            perimeter_polygon = union_(perimeter_polygon, anchoring);
            inset_overhang_area_left_unfilled.insert(inset_overhang_area_left_unfilled.end(), perimeter_polygon.begin(),perimeter_polygon.end());

#ifdef EXTRA_PERIM_DEBUG_FILES
            BoundingBox bbox = get_extents(inset_overhang_area);
            bbox.offset(scale_(2.));
            ::Slic3r::SVG svg(debug_out_path("pre_final").c_str(), bbox);
            for (const Line &line : to_lines(perimeter_polygon)) svg.draw(line, "blue", scale_(0.05));
            for (const Line &line : to_lines(anchoring)) svg.draw(line, "green", scale_(0.05));
            for (const Line &line : to_lines(overhang_to_cover)) svg.draw(line, "yellow", scale_(0.05));
            for (const Line &line : to_lines(inset_overhang_area_left_unfilled)) svg.draw(line, "red", scale_(0.05));
            svg.Close();
#endif
            overhang_region.erase(std::remove_if(overhang_region.begin(), overhang_region.end(),
                                                 [](const ExtrusionPath &p) { return p.empty(); }),
                                  overhang_region.end());

            if (!overhang_region.empty()) {
                // there is a special case, where the first (or last) generated overhang perimeter eats all anchor space.
                // When this happens, the first overhang perimeter is also a closed loop, and needs special check
                // instead of the following simple is_anchored lambda, which checks only the first and last point (not very useful on closed
                // polyline)
                bool first_overhang_is_closed_and_anchored =
                    (overhang_region.front().first_point() == overhang_region.front().last_point() &&
                     !intersection_pl(overhang_region.front().polyline, optimized_lower_slices).empty());
                     
                auto is_anchored = [&lower_layer_aabb_tree](const ExtrusionPath &path) {
                    return lower_layer_aabb_tree.distance_from_lines<true>(path.first_point()) <= 0 ||
                           lower_layer_aabb_tree.distance_from_lines<true>(path.last_point()) <= 0;
                };
                if (!first_overhang_is_closed_and_anchored) {
                    std::reverse(overhang_region.begin(), overhang_region.end());
                } else {
                    size_t min_dist_idx = 0;
                    double min_dist = std::numeric_limits<double>::max();
                    for (size_t i = 0; i < overhang_region.front().polyline.size(); i++) {
                        Point p = overhang_region.front().polyline[i];
                        if (double d = lower_layer_aabb_tree.distance_from_lines<true>(p) < min_dist) {
                            min_dist = d;
                            min_dist_idx = i;
                        }
                    }
                    std::rotate(overhang_region.front().polyline.begin(), overhang_region.front().polyline.begin() + min_dist_idx,
                                overhang_region.front().polyline.end());
                }
                auto first_unanchored          = std::stable_partition(overhang_region.begin(), overhang_region.end(), is_anchored);
                int  index_of_first_unanchored = first_unanchored - overhang_region.begin();
                overhang_region = sort_extra_perimeters(overhang_region, index_of_first_unanchored, overhang_flow.scaled_spacing());
            }
        }
    }

#ifdef EXTRA_PERIM_DEBUG_FILES
    BoundingBox bbox = get_extents(inset_overhang_area);
    bbox.offset(scale_(2.));
    ::Slic3r::SVG svg(debug_out_path(("final" + std::to_string(rand())).c_str()).c_str(), bbox);
    for (const Line &line : to_lines(inset_overhang_area_left_unfilled)) svg.draw(line, "blue", scale_(0.05));
    for (const Line &line : to_lines(inset_overhang_area)) svg.draw(line, "green", scale_(0.05));
    for (const Line &line : to_lines(diff(inset_overhang_area, inset_overhang_area_left_unfilled))) svg.draw(line, "yellow", scale_(0.05));
    svg.Close();
#endif

    inset_overhang_area_left_unfilled = union_(inset_overhang_area_left_unfilled);

    return {extra_perims, diff(inset_overhang_area, inset_overhang_area_left_unfilled)};
}

void PerimeterGenerator::apply_extra_perimeters(ExPolygons &infill_area)
{
    if (!m_spiral_vase && this->lower_slices != nullptr && this->config->detect_overhang_wall && this->config->extra_perimeters_on_overhangs &&
        this->config->wall_loops > 0 && this->layer_id > this->object_config->raft_layers) {
        // Generate extra perimeters on overhang areas, and cut them to these parts only, to save print time and material
        auto [extra_perimeters, filled_area] = generate_extra_perimeters_over_overhangs(infill_area, this->lower_slices_polygons(),
                                                                                        this->config->wall_loops, this->overhang_flow,
                                                                                        this->m_scaled_resolution, *this->object_config,
                                                                                        *this->print_config);
        if (!extra_perimeters.empty()) {
            ExtrusionEntityCollection *this_islands_perimeters = static_cast<ExtrusionEntityCollection *>(this->loops->entities.back());
            ExtrusionEntityCollection  new_perimeters{};
            new_perimeters.no_sort = this_islands_perimeters->no_sort;
            for (const ExtrusionPaths &paths : extra_perimeters) {
                new_perimeters.append(paths);
            }
            new_perimeters.append(this_islands_perimeters->entities);
            this_islands_perimeters->swap(new_perimeters);

            SurfaceCollection orig_surfaces = *this->fill_surfaces;
            this->fill_surfaces->clear();
            for (const auto &surface : orig_surfaces.surfaces) {
                auto new_surfaces = diff_ex({surface.expolygon}, filled_area);
                this->fill_surfaces->append(new_surfaces, surface);
            }
        }
    }
}

// Reorient loop direction
static void reorient_perimeters(ExtrusionEntityCollection &entities, bool steep_overhang_contour, bool steep_overhang_hole, bool reverse_internal_only)
{
    if (steep_overhang_hole || steep_overhang_contour) {
        for (auto entity : entities) {
            if (entity->is_loop()) {
                ExtrusionLoop *eloop = static_cast<ExtrusionLoop *>(entity);
                // Only reverse when needed
                bool need_reverse = ((eloop->loop_role() & elrHole) == elrHole) ? steep_overhang_hole : steep_overhang_contour;
                
                bool isExternal = false;
                if(reverse_internal_only){
                    for(auto path : eloop->paths){
                        if(path.role() == erExternalPerimeter){
                            isExternal = true;
                            break;
                        }
                    }
                }
                
                if (need_reverse && !isExternal) {
                    eloop->make_clockwise();
                }
            }
        }
    }
}

void PerimeterGenerator::process_classic()
{
    group_region_by_fuzzify(*this);

    // other perimeters
    m_mm3_per_mm               		= this->perimeter_flow.mm3_per_mm();
    coord_t perimeter_width         = this->perimeter_flow.scaled_width();
    coord_t perimeter_spacing       = this->perimeter_flow.scaled_spacing();

    // external perimeters
    m_ext_mm3_per_mm           		= this->ext_perimeter_flow.mm3_per_mm();
    coord_t ext_perimeter_width     = this->ext_perimeter_flow.scaled_width();

    coord_t ext_perimeter_spacing   = this->ext_perimeter_flow.scaled_spacing();
    coord_t ext_perimeter_spacing2;
    // Orca: ignore precise_outer_wall if wall_sequence is not InnerOuter
    if(config->precise_outer_wall && config->wall_sequence == WallSequence::InnerOuter)
        ext_perimeter_spacing2 = scaled<coord_t>(0.5f * (this->ext_perimeter_flow.width() + this->perimeter_flow.width()));
    else
        ext_perimeter_spacing2 = scaled<coord_t>(0.5f * (this->ext_perimeter_flow.spacing() + this->perimeter_flow.spacing()));

    // overhang perimeters
    m_mm3_per_mm_overhang      		= this->overhang_flow.mm3_per_mm();

    // solid infill
    coord_t solid_infill_spacing    = this->solid_infill_flow.scaled_spacing();

    // prepare grown lower layer slices for overhang detection
    if (this->lower_slices != nullptr && this->config->detect_overhang_wall) {
        // We consider overhang any part where the entire nozzle diameter is not supported by the
        // lower layer, so we take lower slices and offset them by half the nozzle diameter used
        // in the current layer
        double nozzle_diameter = this->print_config->nozzle_diameter.get_at(this->config->wall_filament - 1);
        m_lower_slices_polygons = offset(*this->lower_slices, float(scale_(+nozzle_diameter / 2)));
    }

    // Calculate the minimum required spacing between two adjacent traces.
    // This should be equal to the nominal flow spacing but we experiment
    // with some tolerance in order to avoid triggering medial axis when
    // some squishing might work. Loops are still spaced by the entire
    // flow spacing; this only applies to collapsing parts.
    // For ext_min_spacing we use the ext_perimeter_spacing calculated for two adjacent
    // external loops (which is the correct way) instead of using ext_perimeter_spacing2
    // which is the spacing between external and internal, which is not correct
    // and would make the collapsing (thus the details resolution) dependent on
    // internal flow which is unrelated.
    coord_t min_spacing         = coord_t(perimeter_spacing      * (1 - INSET_OVERLAP_TOLERANCE));
    coord_t ext_min_spacing     = coord_t(ext_perimeter_spacing  * (1 - INSET_OVERLAP_TOLERANCE));
    bool    has_gap_fill 		= this->config->gap_infill_speed.value > 0;

    // BBS: this flow is for smaller external perimeter for small area
    coord_t ext_min_spacing_smaller = coord_t(ext_perimeter_spacing * (1 - SMALLER_EXT_INSET_OVERLAP_TOLERANCE));
    this->smaller_ext_perimeter_flow = this->ext_perimeter_flow;
    // BBS: to be checked
    this->smaller_ext_perimeter_flow = this->smaller_ext_perimeter_flow.with_width(SCALING_FACTOR *
        (ext_perimeter_width - 0.5 * SMALLER_EXT_INSET_OVERLAP_TOLERANCE * ext_perimeter_spacing));
    m_ext_mm3_per_mm_smaller_width = this->smaller_ext_perimeter_flow.mm3_per_mm();

    // prepare grown lower layer slices for overhang detection
    m_lower_polygons_series = generate_lower_polygons_series(this->perimeter_flow.width());
    if (ext_perimeter_width == perimeter_width){
        m_external_lower_polygons_series = m_lower_polygons_series;
    } else {
        m_external_lower_polygons_series = generate_lower_polygons_series(this->ext_perimeter_flow.width());
    }
    m_smaller_external_lower_polygons_series = generate_lower_polygons_series(this->smaller_ext_perimeter_flow.width());
    // we need to process each island separately because we might have different
    // extra perimeters for each one
    Surfaces all_surfaces = this->slices->surfaces;

    process_no_bridge(all_surfaces, perimeter_spacing, ext_perimeter_width);
    // BBS: don't simplify too much which influence arc fitting when export gcode if arc_fitting is enabled
    double surface_simplify_resolution = (print_config->enable_arc_fitting && !this->has_fuzzy_skin) ? 0.2 * m_scaled_resolution : m_scaled_resolution;
    //BBS: reorder the surface to reduce the travel time
    ExPolygons surface_exp;
    for (const Surface &surface : all_surfaces)
        surface_exp.push_back(surface.expolygon);
    std::vector<size_t> surface_order = chain_expolygons(surface_exp);
    for (size_t order_idx = 0; order_idx < surface_order.size(); order_idx++) {
        const Surface &surface = all_surfaces[surface_order[order_idx]];
        // detect how many perimeters must be generated for this island
        int loop_number = this->config->wall_loops + surface.extra_perimeters - 1;  // 0-indexed loops
        int sparse_infill_density = this->config->sparse_infill_density.value;
        if (this->config->alternate_extra_wall && this->layer_id % 2 == 1 && !m_spiral_vase && sparse_infill_density > 0) // add alternating extra wall
            loop_number++;
        if (this->layer_id == object_config->raft_layers && this->config->only_one_wall_first_layer)
            loop_number = 0;
        // Set the topmost layer to be one wall
        if (loop_number > 0 && config->only_one_wall_top && this->upper_slices == nullptr)
            loop_number = 0;

        ExPolygons last        = union_ex(surface.expolygon.simplify_p(surface_simplify_resolution));
        ExPolygons gaps;
        ExPolygons top_fills;
        ExPolygons fill_clip;
        if (loop_number >= 0) {
            // In case no perimeters are to be generated, loop_number will equal to -1.
            std::vector<PerimeterGeneratorLoops> contours(loop_number+1);    // depth => loops
            std::vector<PerimeterGeneratorLoops> holes(loop_number+1);       // depth => loops
            ThickPolylines thin_walls;
            // we loop one time more than needed in order to find gaps after the last perimeter was applied
            for (int i = 0;; ++ i) {  // outer loop is 0
                // Calculate next onion shell of perimeters.
                ExPolygons offsets;
                ExPolygons offsets_with_smaller_width;
                if (i == 0) {
                    // look for thin walls
                    if (this->config->detect_thin_wall) {
                        // the minimum thickness of a single loop is:
                        // ext_width/2 + ext_spacing/2 + spacing/2 + width/2
                        offsets = offset2_ex(last,
                            -float(ext_perimeter_width / 2. + ext_min_spacing / 2. - 1),
                            +float(ext_min_spacing / 2. - 1));
                        // the following offset2 ensures almost nothing in @thin_walls is narrower than $min_width
                        // (actually, something larger than that still may exist due to mitering or other causes)
                        coord_t min_width = coord_t(scale_(this->ext_perimeter_flow.nozzle_diameter() / 3));
                        ExPolygons expp = opening_ex(
                            // medial axis requires non-overlapping geometry
                            diff_ex(last, offset(offsets, float(ext_perimeter_width / 2.) + ClipperSafetyOffset)),
                            float(min_width / 2.));
                        // the maximum thickness of our thin wall area is equal to the minimum thickness of a single loop
                        for (ExPolygon &ex : expp)
                            ex.medial_axis(min_width, ext_perimeter_width + ext_perimeter_spacing2, &thin_walls);
                    } else {
                        coord_t ext_perimeter_smaller_width = this->smaller_ext_perimeter_flow.scaled_width();
                        for (const ExPolygon& expolygon : last) {
                            // BBS: judge whether it's narrow but not too long island which is hard to place two line
                            ExPolygons expolys;
                            expolys.push_back(expolygon);
                            ExPolygons offset_result = offset2_ex(expolys,
                                -float(ext_perimeter_width / 2. + ext_min_spacing_smaller / 2.),
                                +float(ext_min_spacing_smaller / 2.));
                            if (offset_result.empty() &&
                                expolygon.area() < (double)(ext_perimeter_width + ext_min_spacing_smaller) * scale_(narrow_loop_length_threshold)) {
                                // BBS: for narrow external loop, use smaller line width
                                ExPolygons temp_result = offset_ex(expolygon, -float(ext_perimeter_smaller_width / 2.));
                                offsets_with_smaller_width.insert(offsets_with_smaller_width.end(), temp_result.begin(), temp_result.end());
                            }
                            else {
                                //BBS: for not narrow loop, use normal external perimeter line width
                                ExPolygons temp_result = offset_ex(expolygon, -float(ext_perimeter_width / 2.));
                                offsets.insert(offsets.end(), temp_result.begin(), temp_result.end());
                            }
                        }
                    }
                    if (m_spiral_vase && (offsets.size() > 1 || offsets_with_smaller_width.size() > 1)) {
                        // Remove all but the largest area polygon.
                        keep_largest_contour_only(offsets);
                        //BBS
                        if (offsets.empty())
                            //BBS: only have small width loop, then keep the largest in spiral vase mode
                            keep_largest_contour_only(offsets_with_smaller_width);
                        else
                            //BBS: have large area, clean the small width loop
                            offsets_with_smaller_width.clear();
                    }
                } else {
                    //FIXME Is this offset correct if the line width of the inner perimeters differs
                    // from the line width of the infill?
                    coord_t distance = (i == 1) ? ext_perimeter_spacing2 : perimeter_spacing;
                    //BBS
                    //offsets = this->config->thin_walls ?
                        // This path will ensure, that the perimeters do not overfill, as in
                        // prusa3d/Slic3r GH #32, but with the cost of rounding the perimeters
                        // excessively, creating gaps, which then need to be filled in by the not very
                        // reliable gap fill algorithm.
                        // Also the offset2(perimeter, -x, x) may sometimes lead to a perimeter, which is larger than
                        // the original.
                        //offset2_ex(last,
                        //        - float(distance + min_spacing / 2. - 1.),
                        //        float(min_spacing / 2. - 1.)) :
                        // If "detect thin walls" is not enabled, this paths will be entered, which
                        // leads to overflows, as in prusa3d/Slic3r GH #32
                        //offset_ex(last, - float(distance));

                    //BBS: For internal perimeter, we should "enable" thin wall strategy in which offset2 is used to
                    // remove too closed line, so that gap fill can be used for such internal narrow area in following
                    // handling.
                    offsets = offset2_ex(last,
                        -float(distance + min_spacing / 2. - 1.),
                        float(min_spacing / 2. - 1.));
                    // look for gaps
                    if (has_gap_fill)
                        // not using safety offset here would "detect" very narrow gaps
                        // (but still long enough to escape the area threshold) that gap fill
                        // won't be able to fill but we'd still remove from infill area
                        append(gaps, diff_ex(
                            offset(last,    - float(0.5 * distance)),
                            offset(offsets,   float(0.5 * distance + 10))));  // safety offset
                }
                if (offsets.empty() && offsets_with_smaller_width.empty()) {
                    // Store the number of loops actually generated.
                    loop_number = i - 1;
                    // No region left to be filled in.
                    last.clear();
                    break;
                } else if (i > loop_number) {
                    // If i > loop_number, we were looking just for gaps.
                    break;
                }
                {
                    for (const ExPolygon& expolygon : offsets) {
                        // Outer contour may overlap with an inner contour,
                        // inner contour may overlap with another inner contour,
                        // outer contour may overlap with itself.
                        //FIXME evaluate the overlaps, annotate each point with an overlap depth,
                        // compensate for the depth of intersection.
                        contours[i].emplace_back(expolygon.contour, i, true);

                        if (!expolygon.holes.empty()) {
                            holes[i].reserve(holes[i].size() + expolygon.holes.size());
                            for (const Polygon& hole : expolygon.holes)
                                holes[i].emplace_back(hole, i, false);
                        }
                    }

                    //BBS: save perimeter loop which use smaller width
                    if (i == 0) {
                        for (const ExPolygon& expolygon : offsets_with_smaller_width) {
                            contours[i].emplace_back(PerimeterGeneratorLoop(expolygon.contour, i, true, true));
                            if (!expolygon.holes.empty()) {
                                holes[i].reserve(holes[i].size() + expolygon.holes.size());
                                for (const Polygon& hole : expolygon.holes)
                                    holes[i].emplace_back(PerimeterGeneratorLoop(hole, i, false, true));
                            }
                        }
                    }
                }

                last = std::move(offsets);

                //BBS: refer to superslicer
                //store surface for top infill if only_one_wall_top
                if (i == 0 && i!=loop_number && config->only_one_wall_top && !surface.is_bridge() && this->upper_slices != NULL) {
                    this->split_top_surfaces(last, top_fills, last, fill_clip);
                }

                if (i == loop_number && (! has_gap_fill || this->config->sparse_infill_density.value == 0)) {
                	// The last run of this loop is executed to collect gaps for gap fill.
                	// As the gap fill is either disabled or not
                	break;
                }
            }

            // nest loops: holes first
            for (int d = 0; d <= loop_number; ++ d) {
                PerimeterGeneratorLoops &holes_d = holes[d];
                // loop through all holes having depth == d
                for (int i = 0; i < (int)holes_d.size(); ++ i) {
                    const PerimeterGeneratorLoop &loop = holes_d[i];
                    // find the hole loop that contains this one, if any
                    for (int t = d + 1; t <= loop_number; ++ t) {
                        for (int j = 0; j < (int)holes[t].size(); ++ j) {
                            PerimeterGeneratorLoop &candidate_parent = holes[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                holes_d.erase(holes_d.begin() + i);
                                -- i;
                                goto NEXT_LOOP;
                            }
                        }
                    }
                    // if no hole contains this hole, find the contour loop that contains it
                    for (int t = loop_number; t >= 0; -- t) {
                        for (int j = 0; j < (int)contours[t].size(); ++ j) {
                            PerimeterGeneratorLoop &candidate_parent = contours[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                holes_d.erase(holes_d.begin() + i);
                                -- i;
                                goto NEXT_LOOP;
                            }
                        }
                    }
                    NEXT_LOOP: ;
                }
            }
            // nest contour loops
            for (int d = loop_number; d >= 1; -- d) {
                PerimeterGeneratorLoops &contours_d = contours[d];
                // loop through all contours having depth == d
                for (int i = 0; i < (int)contours_d.size(); ++ i) {
                    const PerimeterGeneratorLoop &loop = contours_d[i];
                    // find the contour loop that contains it
                    for (int t = d - 1; t >= 0; -- t) {
                        for (size_t j = 0; j < contours[t].size(); ++ j) {
                            PerimeterGeneratorLoop &candidate_parent = contours[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                contours_d.erase(contours_d.begin() + i);
                                -- i;
                                goto NEXT_CONTOUR;
                            }
                        }
                    }
                    NEXT_CONTOUR: ;
                }
            }
            // at this point, all loops should be in contours[0]
            bool steep_overhang_contour = false;
            bool steep_overhang_hole    = false;
            const WallDirection wall_direction = config->wall_direction;
            if (wall_direction != WallDirection::Auto) {
                // Skip steep overhang detection if wall direction is specified
                steep_overhang_contour = true;
                steep_overhang_hole    = true;
            }
            ExtrusionEntityCollection entities = traverse_loops(*this, contours.front(), thin_walls, steep_overhang_contour, steep_overhang_hole);
            // All walls are counter-clockwise initially, so we don't need to reorient it if that's what we want
            if (wall_direction != WallDirection::CounterClockwise) {
                reorient_perimeters(entities, steep_overhang_contour, steep_overhang_hole,
                                    // Reverse internal only if the wall direction is auto
                                    this->config->overhang_reverse_internal_only && wall_direction == WallDirection::Auto);
            }

            // if brim will be printed, reverse the order of perimeters so that
            // we continue inwards after having finished the brim
            // TODO: add test for perimeter order
            bool is_outer_wall_first = this->config->wall_sequence == WallSequence::OuterInner;
            if (is_outer_wall_first ||
                //BBS: always print outer wall first when there indeed has brim.
                (this->layer_id == 0 &&
                    this->object_config->brim_type == BrimType::btOuterOnly &&
                    this->object_config->brim_width.value > 0))
                entities.reverse();
            // Orca: sandwich mode. Apply after 1st layer.
            else if ((this->config->wall_sequence == WallSequence::InnerOuterInner) && layer_id > 0){
                entities.reverse(); // reverse all entities - order them from external to internal
                if(entities.entities.size()>2){ // 3 walls minimum needed to do inner outer inner ordering
                    int position = 0; // index to run the re-ordering for multiple external perimeters in a single island.
                    int arr_i, arr_j = 0;    // indexes to run through the walls in the for loops
                    int outer, first_internal, second_internal, max_internal, current_perimeter; // allocate index values
                    
                    // Initiate reorder sequence to bring any index 1 (first internal) perimeters ahead of any second internal perimeters
                    // Leaving these out of order will result in print defects on the external wall as they will be extruded prior to any
                    // external wall. To do the re-ordering, we are creating two extrusion arrays - reordered_extrusions which will contain
                    // the reordered extrusions and skipped_extrusions will contain the ones that were skipped in the scan
                    ExtrusionEntityCollection reordered_extrusions, skipped_extrusions;
                    bool found_second_internal = false; // helper variable to indicate the start of a new island
                    
                    for(auto extrusion_to_reorder : entities.entities){ //scan the perimeters to reorder
                        switch (extrusion_to_reorder->inset_idx) {
                            case 0: // external perimeter
                                if(found_second_internal){ //new island - move skipped extrusions to reordered array
                                    for(auto extrusion_skipped : skipped_extrusions)
                                        reordered_extrusions.append(*extrusion_skipped);
                                    skipped_extrusions.clear();
                                }
                                reordered_extrusions.append(*extrusion_to_reorder);
                                break;
                            case 1: // first internal perimeter
                                reordered_extrusions.append(*extrusion_to_reorder);
                                break;
                            default: // second internal+ perimeter -> put them in the skipped extrusions array
                                skipped_extrusions.append(*extrusion_to_reorder);
                                found_second_internal = true;
                                break;
                        }
                    }
                    if(entities.entities.size()>reordered_extrusions.size()){
                        // we didnt find any more islands, so lets move the remaining skipped perimeters to the reordered extrusions list.
                        for(auto extrusion_skipped : skipped_extrusions)
                            reordered_extrusions.append(*extrusion_skipped);
                        skipped_extrusions.clear();
                    }
                    
                    // Now start the sandwich mode wall re-ordering using the reordered_extrusions as the basis
                    // scan to find the external perimeter, first internal, second internal and last perimeter in the island.
                    // We then advance the position index to move to the second "island" and continue until there are no more
                    // perimeters left.
                    while (position < reordered_extrusions.size()) {
                        outer = first_internal = second_internal = current_perimeter = -1; // initialise all index values to -1
                        max_internal = reordered_extrusions.size()-1; // initialise the maximum internal perimeter to the last perimeter on the extrusion list
                        // run through the walls to get the index values that need re-ordering until the first one for each
                        // is found. Start at "position" index to enable the for loop to iterate for multiple external
                        // perimeters in a single island
                        for (arr_i = position; arr_i < reordered_extrusions.size(); ++arr_i) {
                            switch (reordered_extrusions.entities[arr_i]->inset_idx) {
                                case 0: // external perimeter
                                    if (outer == -1)
                                        outer = arr_i;
                                    break;
                                case 1: // first internal wall
                                    if (first_internal==-1 && arr_i>outer && outer!=-1){
                                        first_internal = arr_i;
                                    }
                                    break;
                                case 2: // second internal wall
                                    if (second_internal == -1 && arr_i > first_internal && outer!=-1){
                                        second_internal = arr_i;
                                    }
                                    break;
                            }
                            if(outer >-1 && first_internal>-1 && second_internal>-1 && reordered_extrusions.entities[arr_i]->inset_idx == 0){ // found a new external perimeter after we've found all three perimeters to re-order -> this means we entered a new island.
                                arr_i=arr_i-1; //step back one perimeter
                                max_internal = arr_i; // new maximum internal perimeter is now this as we have found a new external perimeter, hence a new island.
                                break; // exit the for loop
                            }
                        }
                    
                        if (outer > -1 && first_internal > -1 && second_internal > -1) { // found perimeters to re-order?
                            ExtrusionEntityCollection inner_outer_extrusions; // temporary collection to hold extrusions for reordering
            
                            for (arr_j = max_internal; arr_j >=position; --arr_j){ // go inside out towards the external perimeter (perimeters in reverse order) and store all internal perimeters until the first one identified with inset index 2
                                if(arr_j >= second_internal){
                                    inner_outer_extrusions.append(*reordered_extrusions.entities[arr_j]);
                                    current_perimeter++;
                                }
                            }
                            
                            for (arr_j = position; arr_j < second_internal; ++arr_j){ // go outside in and map the remaining perimeters (external and first internal wall(s)) using the outside in wall order
                                inner_outer_extrusions.append(*reordered_extrusions.entities[arr_j]);
                            }
                            
                            for(arr_j = position; arr_j <= max_internal; ++arr_j) // replace perimeter array with the new re-ordered array
                                entities.replace(arr_j, *inner_outer_extrusions.entities[arr_j-position]);
                        } else
                            break;
                        // go to the next perimeter from the current position to continue scanning for external walls in the same island
                        position = arr_i + 1;
                    }
                }
            }
            
            // append perimeters for this slice as a collection
            if (! entities.empty())
                this->loops->append(entities);

        } // for each loop of an island

        // fill gaps
        if (! gaps.empty()) {
            // collapse
            double min = 0.2 * perimeter_width * (1 - INSET_OVERLAP_TOLERANCE);
            double max = 2. * perimeter_spacing;
            ExPolygons gaps_ex = diff_ex(
                //FIXME offset2 would be enough and cheaper.
                opening_ex(gaps, float(min / 2.)),
                offset2_ex(gaps, - float(max / 2.), float(max / 2. + ClipperSafetyOffset)));
            ThickPolylines polylines;
            for (ExPolygon& ex : gaps_ex) {
                //BBS: Use DP simplify to avoid duplicated points and accelerate medial-axis calculation as well.
                ex.douglas_peucker(surface_simplify_resolution);
                ex.medial_axis(min, max, &polylines);
            }

#ifdef GAPS_OF_PERIMETER_DEBUG_TO_SVG
            {
                static int irun = 0;
                BoundingBox bbox_svg;
                bbox_svg.merge(get_extents(gaps_ex));
                {
                    std::stringstream stri;
                    stri << "debug_gaps_ex_" << irun << ".svg";
                    SVG svg(stri.str(), bbox_svg);
                    svg.draw(to_polylines(gaps_ex), "blue", 0.5);
                    svg.Close();
                }
                ++ irun;
            }
#endif
            // SoftFever: filter out tiny gap fills
            polylines.erase(std::remove_if(polylines.begin(), polylines.end(),
                [&](const ThickPolyline& p) {
                    return p.length() < scale_(config->filter_out_gap_fill.value);
                }), polylines.end());


            if (! polylines.empty()) {
				ExtrusionEntityCollection gap_fill;
				variable_width(polylines, erGapFill, this->solid_infill_flow, gap_fill.entities);
                /*  Make sure we don't infill narrow parts that are already gap-filled
                    (we only consider this surface's gaps to reduce the diff() complexity).
                    Growing actual extrusions ensures that gaps not filled by medial axis
                    are not subtracted from fill surfaces (they might be too short gaps
                    that medial axis skips but infill might join with other infill regions
                    and use zigzag).  */
                //FIXME Vojtech: This grows by a rounded extrusion width, not by line spacing,
                // therefore it may cover the area, but no the volume.
                last = diff_ex(last, gap_fill.polygons_covered_by_width(10.f));
                this->gap_fill->append(std::move(gap_fill.entities));

			}
        }

        // create one more offset to be used as boundary for fill
        // we offset by half the perimeter spacing (to get to the actual infill boundary)
        // and then we offset back and forth by half the infill spacing to only consider the
        // non-collapsing regions
        coord_t inset =
            (loop_number < 0) ? 0 :
            (loop_number == 0) ?
                // one loop
                ext_perimeter_spacing / 2 :
                // two or more loops?
                perimeter_spacing / 2;
        
        // only apply infill overlap if we actually have one perimeter
        coord_t infill_peri_overlap = 0;
        coord_t top_infill_peri_overlap = 0;
        if (inset > 0) {
            if(this->layer_id == 0 || this->upper_slices == nullptr){
                infill_peri_overlap = coord_t(scale_(this->config->top_bottom_infill_wall_overlap.get_abs_value(unscale<double>(inset + solid_infill_spacing / 2))));
            }else{
                infill_peri_overlap = coord_t(scale_(this->config->infill_wall_overlap.get_abs_value(unscale<double>(inset + solid_infill_spacing / 2))));
                top_infill_peri_overlap = coord_t(scale_(this->config->top_bottom_infill_wall_overlap.get_abs_value(unscale<double>(inset + solid_infill_spacing / 2))));
            }
            inset -= infill_peri_overlap;
        }
        // simplify infill contours according to resolution
        Polygons pp;
        for (ExPolygon &ex : last)
            ex.simplify_p(m_scaled_resolution, &pp);
        ExPolygons not_filled_exp = union_ex(pp);
        // collapse too narrow infill areas
        coord_t min_perimeter_infill_spacing = coord_t(solid_infill_spacing * (1. - INSET_OVERLAP_TOLERANCE));

        ExPolygons infill_exp = offset2_ex(
            not_filled_exp,
            float(-inset - min_perimeter_infill_spacing / 2.),
            float(min_perimeter_infill_spacing / 2.));
        // append infill areas to fill_surfaces
        //if any top_fills, grow them by ext_perimeter_spacing/2 to have the real un-anchored fill
        ExPolygons top_infill_exp = intersection_ex(fill_clip, offset_ex(top_fills, double(ext_perimeter_spacing / 2)));
        if (!top_fills.empty()) {
            infill_exp = union_ex(infill_exp, offset_ex(top_infill_exp, double(top_infill_peri_overlap)));
        }
        this->fill_surfaces->append(infill_exp, stInternal);

        apply_extra_perimeters(infill_exp);

        // BBS: get the no-overlap infill expolygons
        {
            ExPolygons polyWithoutOverlap;
            if (min_perimeter_infill_spacing / 2 > infill_peri_overlap)
                polyWithoutOverlap = offset2_ex(
                    not_filled_exp,
                    float(-inset - min_perimeter_infill_spacing / 2.),
                    float(min_perimeter_infill_spacing / 2 - infill_peri_overlap));
            else
                polyWithoutOverlap = offset_ex(
                    not_filled_exp,
                    double(-inset - infill_peri_overlap));
            if (!top_fills.empty())
                polyWithoutOverlap = union_ex(polyWithoutOverlap, top_infill_exp);
            this->fill_no_overlap->insert(this->fill_no_overlap->end(), polyWithoutOverlap.begin(), polyWithoutOverlap.end());
        }

    } // for each island
}

//BBS:
void PerimeterGenerator::add_infill_contour_for_arachne( ExPolygons        infill_contour,
                                                         int                loops,
                                                         coord_t            ext_perimeter_spacing,
                                                         coord_t            perimeter_spacing,
                                                         coord_t            min_perimeter_infill_spacing,
                                                         coord_t            spacing,
                                                         bool               is_inner_part)
{
    if( offset_ex(infill_contour, -float(spacing / 2.)).empty() )
    {
        infill_contour.clear(); // Infill region is too small, so let's filter it out.
    }

    // create one more offset to be used as boundary for fill
    // we offset by half the perimeter spacing (to get to the actual infill boundary)
    // and then we offset back and forth by half the infill spacing to only consider the
    // non-collapsing regions
    coord_t insert = (loops < 0) ? 0: ext_perimeter_spacing;
    if (is_inner_part || loops > 0)
        insert = perimeter_spacing;

    insert = coord_t(scale_(this->config->infill_wall_overlap.get_abs_value(unscale<double>(insert))));

    Polygons inner_pp;
    for (ExPolygon &ex : infill_contour)
        ex.simplify_p(m_scaled_resolution, &inner_pp);

    this->fill_surfaces->append(offset2_ex(union_ex(inner_pp), float(-min_perimeter_infill_spacing / 2.), float(insert + min_perimeter_infill_spacing / 2.)), stInternal);

    append(*this->fill_no_overlap, offset2_ex(union_ex(inner_pp), float(-min_perimeter_infill_spacing / 2.), float(+min_perimeter_infill_spacing / 2.)));
}

// Orca: sacrificial bridge layer algorithm ported from SuperSlicer
void PerimeterGenerator::process_no_bridge(Surfaces& all_surfaces, coord_t perimeter_spacing, coord_t ext_perimeter_width)
{
    //store surface for bridge infill to avoid unsupported perimeters (but the first one, this one is always good)
    if (this->config->counterbore_hole_bridging != chbNone
        && this->lower_slices != NULL && !this->lower_slices->empty()) {
        const coordf_t bridged_infill_margin = scale_(BRIDGE_INFILL_MARGIN);

        for (size_t surface_idx = 0; surface_idx < all_surfaces.size(); surface_idx++) {
            Surface* surface = &all_surfaces[surface_idx];
            ExPolygons last = { surface->expolygon };
            //compute our unsupported surface
            ExPolygons unsupported = diff_ex(last, *this->lower_slices, ApplySafetyOffset::Yes);
            if (!unsupported.empty()) {
                //remove small overhangs
                ExPolygons unsupported_filtered = offset2_ex(unsupported, double(-perimeter_spacing), double(perimeter_spacing));
                if (!unsupported_filtered.empty()) {
                    //to_draw.insert(to_draw.end(), last.begin(), last.end());
                    //extract only the useful part of the lower layer. The safety offset is really needed here.
                    ExPolygons support = diff_ex(last, unsupported, ApplySafetyOffset::Yes);
                    if (!unsupported.empty()) {
                        //only consider the part that can be bridged (really, by the bridge algorithm)
                        //first, separate into islands (ie, each ExPlolygon)
                        int numploy = 0;
                        //only consider the bottom layer that intersect unsupported, to be sure it's only on our island.
                        ExPolygonCollection lower_island(support);
                        //a detector per island
                        ExPolygons bridgeable;
                        for (ExPolygon unsupported : unsupported_filtered) {
                            BridgeDetector detector{ unsupported,
                                                    lower_island.expolygons,
                                                    perimeter_spacing };
                            if (detector.detect_angle(Geometry::deg2rad(this->config->bridge_angle.value)))
                                expolygons_append(bridgeable, union_ex(detector.coverage(-1, true)));
                        }
                        if (!bridgeable.empty()) {
                            //check if we get everything or just the bridgeable area
                            if (/*this->config->counterbore_hole_bridging.value == chbNoPeri || */this->config->counterbore_hole_bridging.value == chbFilled) {
                                //we bridge everything, even the not-bridgeable bits
                                for (size_t i = 0; i < unsupported_filtered.size();) {
                                    ExPolygon& poly_unsupp = *(unsupported_filtered.begin() + i);
                                    Polygons contour_simplified = poly_unsupp.contour.simplify(perimeter_spacing);
                                    ExPolygon poly_unsupp_bigger = poly_unsupp;
                                    Polygons contour_bigger = offset(poly_unsupp_bigger.contour, bridged_infill_margin);
                                    if (contour_bigger.size() == 1) poly_unsupp_bigger.contour = contour_bigger[0];

                                    //check convex, has some bridge, not overhang
                                    if (contour_simplified.size() == 1 && contour_bigger.size() == 1 && contour_simplified[0].concave_points().size() == 0
                                        && intersection_ex(bridgeable, ExPolygons{ poly_unsupp }).size() > 0
                                        && diff_ex(ExPolygons{ poly_unsupp_bigger }, union_ex(last, offset_ex(bridgeable, bridged_infill_margin + perimeter_spacing / 2)), ApplySafetyOffset::Yes).size() == 0
                                    ) {
                                        //ok, keep it
                                        i++;
                                    } else {
                                        unsupported_filtered.erase(unsupported_filtered.begin() + i);
                                    }
                                }
                                unsupported_filtered = intersection_ex(last,
                                                                       offset2_ex(unsupported_filtered, double(-perimeter_spacing / 2), double(bridged_infill_margin + perimeter_spacing / 2)));
                                if (this->config->counterbore_hole_bridging.value == chbFilled) {
                                    for (ExPolygon& expol : unsupported_filtered) {
                                        //check if the holes won't be covered by the upper layer
                                        //TODO: if we want to do that, we must modify the geometry before making perimeters.
                                        //if (this->upper_slices != nullptr && !this->upper_slices->expolygons.empty()) {
                                        //    for (Polygon &poly : expol.holes) poly.make_counter_clockwise();
                                        //    float perimeterwidth = this->config->perimeters == 0 ? 0 : (this->ext_perimeter_flow.scaled_width() + (this->config->perimeters - 1) + this->perimeter_flow.scaled_spacing());
                                        //    std::cout << "test upper slices with perimeterwidth=" << perimeterwidth << "=>" << offset_ex(this->upper_slices->expolygons, -perimeterwidth).size();
                                        //    if (intersection(Polygons() = { expol.holes }, to_polygons(offset_ex(this->upper_slices->expolygons, -this->ext_perimeter_flow.scaled_width() / 2))).empty()) {
                                        //        std::cout << " EMPTY";
                                        //        expol.holes.clear();
                                        //    } else {
                                        //    }
                                        //    std::cout << "\n";
                                        //} else {
                                        expol.holes.clear();
                                        //}

                                        //detect inside volume
                                        for (size_t surface_idx_other = 0; surface_idx_other < all_surfaces.size(); surface_idx_other++) {
                                            if (surface_idx == surface_idx_other) continue;
                                            if (intersection_ex(ExPolygons() = { expol }, ExPolygons() = { all_surfaces[surface_idx_other].expolygon }).size() > 0) {
                                                //this means that other_surf was inside an expol holes
                                                //as we removed them, we need to add a new one
                                                ExPolygons new_poly = offset2_ex(ExPolygons{ all_surfaces[surface_idx_other].expolygon }, double(-bridged_infill_margin - perimeter_spacing), double(perimeter_spacing));
                                                if (new_poly.size() == 1) {
                                                    all_surfaces[surface_idx_other].expolygon = new_poly[0];
                                                    expol.holes.push_back(new_poly[0].contour);
                                                    expol.holes.back().make_clockwise();
                                                } else {
                                                    for (size_t idx = 0; idx < new_poly.size(); idx++) {
                                                        Surface new_surf = all_surfaces[surface_idx_other];
                                                        new_surf.expolygon = new_poly[idx];
                                                        all_surfaces.push_back(new_surf);
                                                        expol.holes.push_back(new_poly[idx].contour);
                                                        expol.holes.back().make_clockwise();
                                                    }
                                                    all_surfaces.erase(all_surfaces.begin() + surface_idx_other);
                                                    if (surface_idx_other < surface_idx) {
                                                        surface_idx--;
                                                        surface = &all_surfaces[surface_idx];
                                                    }
                                                    surface_idx_other--;
                                                }
                                            }
                                        }
                                    }

                                }
                                //TODO: add other polys as holes inside this one (-margin)
                            } else if (/*this->config->counterbore_hole_bridging.value == chbBridgesOverhangs || */this->config->counterbore_hole_bridging.value == chbBridges) {
                                //simplify to avoid most of artefacts from printing lines.
                                ExPolygons bridgeable_simplified;
                                for (ExPolygon& poly : bridgeable) {
                                    poly.simplify(perimeter_spacing, &bridgeable_simplified);
                                }
                                bridgeable_simplified = offset2_ex(bridgeable_simplified, -ext_perimeter_width, ext_perimeter_width);
                                //bridgeable_simplified = intersection_ex(bridgeable_simplified, unsupported_filtered);
                                //offset by perimeter spacing because the simplify may have reduced it a bit.
                                //it's not dangerous as it will be intersected by 'unsupported' later
                                //FIXME: add overlap in this->fill_surfaces->append
                                //FIXME: it overlap inside unsuppported not-bridgeable area!

                                //bridgeable_simplified = offset2_ex(bridgeable_simplified, (double)-perimeter_spacing, (double)perimeter_spacing * 2);
                                //ExPolygons unbridgeable = offset_ex(diff_ex(unsupported, bridgeable_simplified), perimeter_spacing * 3 / 2);
                                //ExPolygons unbridgeable = intersection_ex(unsupported, diff_ex(unsupported_filtered, offset_ex(bridgeable_simplified, ext_perimeter_width / 2)));
                                //unbridgeable = offset2_ex(unbridgeable, -ext_perimeter_width, ext_perimeter_width);


                                // if (this->config->counterbore_hole_bridging.value == chbBridges) {
                                    ExPolygons unbridgeable = unsupported_filtered;
                                    for (ExPolygon& expol : unbridgeable)
                                        expol.holes.clear();
                                    unbridgeable = diff_ex(unbridgeable, bridgeable_simplified);
                                    unbridgeable = offset2_ex(unbridgeable, -ext_perimeter_width * 2, ext_perimeter_width * 2);
                                    ExPolygons bridges_temp = offset2_ex(intersection_ex(last, diff_ex(unsupported_filtered, unbridgeable), ApplySafetyOffset::Yes), -ext_perimeter_width / 4, ext_perimeter_width / 4);
                                    //remove the overhangs section from the surface polygons
                                    ExPolygons reference = last;
                                    last = diff_ex(last, unsupported_filtered);
                                    //ExPolygons no_bridge = diff_ex(offset_ex(unbridgeable, ext_perimeter_width * 3 / 2), last);
                                    //bridges_temp = diff_ex(bridges_temp, no_bridge);
                                    coordf_t offset_to_do = bridged_infill_margin;
                                    bool first = true;
                                    unbridgeable = diff_ex(unbridgeable, offset_ex(bridges_temp, ext_perimeter_width));
                                    while (offset_to_do > ext_perimeter_width * 1.5) {
                                        unbridgeable = offset2_ex(unbridgeable, -ext_perimeter_width / 4, ext_perimeter_width * 2.25, ClipperLib::jtSquare);
                                        bridges_temp = diff_ex(bridges_temp, unbridgeable);
                                        bridges_temp = offset_ex(bridges_temp, ext_perimeter_width, ClipperLib::jtMiter, 6.);
                                        unbridgeable = diff_ex(unbridgeable, offset_ex(bridges_temp, ext_perimeter_width));
                                        offset_to_do -= ext_perimeter_width;
                                        first = false;
                                    }
                                    unbridgeable = offset_ex(unbridgeable, ext_perimeter_width + offset_to_do, ClipperLib::jtSquare);
                                    bridges_temp = diff_ex(bridges_temp, unbridgeable);
                                    unsupported_filtered = offset_ex(bridges_temp, offset_to_do);
                                    unsupported_filtered = intersection_ex(unsupported_filtered, reference);
                                // } else {
                                //     ExPolygons unbridgeable = intersection_ex(unsupported, diff_ex(unsupported_filtered, offset_ex(bridgeable_simplified, ext_perimeter_width / 2)));
                                //     unbridgeable = offset2_ex(unbridgeable, -ext_perimeter_width, ext_perimeter_width);
                                //     unsupported_filtered = unbridgeable;

                                //     ////put the bridge area inside the unsupported_filtered variable
                                //     //unsupported_filtered = intersection_ex(last,
                                //     //    diff_ex(
                                //     //    offset_ex(bridgeable_simplified, (double)perimeter_spacing / 2),
                                //     //    unbridgeable
                                //     //    )
                                //     //    );
                                // }
                            } else {
                                unsupported_filtered.clear();
                            }
                        } else {
                            unsupported_filtered.clear();
                        }
                    }

                    if (!unsupported_filtered.empty()) {

                        //add this directly to the infill list.
                        // this will avoid to throw wrong offsets into a good polygons
                        this->fill_surfaces->append(
                            unsupported_filtered,
                            stInternal);

                        // store the results
                        last = diff_ex(last, unsupported_filtered, ApplySafetyOffset::Yes);
                        //remove "thin air" polygons (note: it assumes that all polygons below will be extruded)
                        for (int i = 0; i < last.size(); i++) {
                            if (intersection_ex(support, ExPolygons() = { last[i] }).empty()) {
                                this->fill_surfaces->append(
                                    ExPolygons() = { last[i] },
                                    stInternal);
                                last.erase(last.begin() + i);
                                i--;
                            }
                        }
                    }
                }
            }
            if (last.size() == 0) {
                all_surfaces.erase(all_surfaces.begin() + surface_idx);
                surface_idx--;
            } else {
                surface->expolygon = last[0];
                for (size_t idx = 1; idx < last.size(); idx++) {
                    all_surfaces.emplace_back(*surface, last[idx]);
                }
            }
        }
    }
}

// ORCA:
// Inner Outer Inner wall ordering mode perimeter order optimisation functions
/**
 * @brief Finds all perimeters touching a given set of reference lines, given as indexes.
 *
 * @param entities The list of PerimeterGeneratorArachneExtrusion entities.
 * @param referenceIndices A set of indices representing the reference points.
 * @param threshold_external The distance threshold to consider for proximity for a reference perimeter with inset index 0
 * @param threshold_internal The distance threshold to consider for proximity for a reference perimeter with inset index 1+
 * @param considered_inset_idx What perimeter inset index are we searching for (eg. if we are searching for first internal perimeters proximate to the current reference perimeter, this value should be set to 1 etc).
 * @return std::vector<int> A vector of indices representing the touching perimeters.
 */
std::vector<int> findAllTouchingPerimeters(const std::vector<PerimeterGeneratorArachneExtrusion>& entities, const std::unordered_set<int>& referenceIndices, size_t threshold_external, size_t threshold_internal , size_t considered_inset_idx) {
    std::unordered_set<int> touchingIndices;

    for (const int refIdx : referenceIndices) {
        const auto& referenceEntity = entities[refIdx];
        Points referencePoints = Arachne::to_points(*referenceEntity.extrusion);
        for (size_t i = 0; i < entities.size(); ++i) {
            // Skip already considered references and the reference entity
            if (referenceIndices.count(i) > 0) continue;
            const auto& entity = entities[i];
            if (entity.extrusion->inset_idx == 0) continue; // Ignore inset index 0 (external) perimeters from the re-ordering even if they are touching

            if (entity.extrusion->inset_idx != considered_inset_idx) { // Find Inset index perimeters that match the requested inset index
                continue; // skip if they dont match
            }
            
            Points points = Arachne::to_points(*entity.extrusion);
            double distance = MultiPoint::minimumDistanceBetweenLinesDefinedByPoints(referencePoints, points);
            // Add to touchingIndices if within threshold distance
            size_t threshold=0;
            if(referenceEntity.extrusion->inset_idx == 0)
                threshold = threshold_external;
            else
                threshold = threshold_internal;
            if (distance <= threshold) {
                touchingIndices.insert(i);
            }
        }
    }
    return std::vector<int>(touchingIndices.begin(), touchingIndices.end());
}

/**
 * @brief Reorders perimeters based on proximity to the reference perimeter
 *
 * This approach finds all perimeters touching the external perimeter first and then finds all perimeters touching these new ones until none are left
 * It ensures a level-by-level traversal, similar to BFS in graph theory.
 *
 * @param entities The list of PerimeterGeneratorArachneExtrusion entities.
 * @param referenceIndex The index of the reference perimeter.
 * @param threshold_external The distance threshold to consider for proximity for a reference perimeter with inset index 0
 * @param threshold_internal The distance threshold to consider for proximity for a reference perimeter with inset index 1+
 * @return std::vector<PerimeterGeneratorArachneExtrusion> The reordered list of perimeters based on proximity.
 */
std::vector<PerimeterGeneratorArachneExtrusion> reorderPerimetersByProximity(std::vector<PerimeterGeneratorArachneExtrusion> entities, size_t threshold_external, size_t threshold_internal) {
    std::vector<PerimeterGeneratorArachneExtrusion> reordered;
    std::unordered_set<int> includedIndices;

    // Function to reorder perimeters starting from a given reference index
    auto reorderFromReference = [&](int referenceIndex) {
        std::unordered_set<int> firstLevelIndices;
        firstLevelIndices.insert(referenceIndex);

        // Find first level touching perimeters
        std::vector<int> firstLevelTouchingIndices = findAllTouchingPerimeters(entities, firstLevelIndices, threshold_external, threshold_internal, 1);
        // Bring the largest first level perimeter to the front
        // The longest first neighbour is most likely the dominant proximate perimeter
        // hence printing it immediately after the external perimeter should speed things up
        if (!firstLevelTouchingIndices.empty()) {
            auto maxIt = std::max_element(firstLevelTouchingIndices.begin(), firstLevelTouchingIndices.end(), [&entities](int a, int b) {
                return entities[a].extrusion->getLength() < entities[b].extrusion->getLength();
            });
            std::iter_swap(maxIt, firstLevelTouchingIndices.end() - 1);
        }
        // Insert first level perimeters into reordered list
        reordered.push_back(entities[referenceIndex]);
        includedIndices.insert(referenceIndex);

        for (int idx : firstLevelTouchingIndices) {
            if (includedIndices.count(idx) == 0) {
                reordered.push_back(entities[idx]);
                includedIndices.insert(idx);
            }
        }

        // Loop through all inset indices above 1
        size_t currentInsetIndex = 2;
        while (true) {
            std::unordered_set<int> currentLevelIndices(firstLevelTouchingIndices.begin(), firstLevelTouchingIndices.end());
            std::vector<int> currentLevelTouchingIndices = findAllTouchingPerimeters(entities, currentLevelIndices, threshold_external, threshold_internal, currentInsetIndex);

            // Break if no more touching perimeters are found
            if (currentLevelTouchingIndices.empty()) {
                break;
            }

            // Exclude any already included indices from the current level touching indices
            currentLevelTouchingIndices.erase(
                std::remove_if(currentLevelTouchingIndices.begin(), currentLevelTouchingIndices.end(),
                    [&](int idx) { return includedIndices.count(idx) > 0; }),
                currentLevelTouchingIndices.end());

            // Bring the largest current level perimeter to the end
            if (!currentLevelTouchingIndices.empty()) {
                auto maxIt = std::max_element(currentLevelTouchingIndices.begin(), currentLevelTouchingIndices.end(), [&entities](int a, int b) {
                    return entities[a].extrusion->getLength() < entities[b].extrusion->getLength();
                });
                std::iter_swap(maxIt, currentLevelTouchingIndices.begin());
            }

            // Insert current level perimeters into reordered list
            for (int idx : currentLevelTouchingIndices) {
                if (includedIndices.count(idx) == 0) {
                    reordered.push_back(entities[idx]);
                    includedIndices.insert(idx);
                }
            }

            // Prepare for the next level
            firstLevelTouchingIndices = currentLevelTouchingIndices;
            currentInsetIndex++;
        }
    };

    // Loop through all perimeters and reorder starting from each inset index 0 perimeter
    for (size_t refIdx = 0; refIdx < entities.size(); ++refIdx) {
        if (entities[refIdx].extrusion->inset_idx == 0 && includedIndices.count(refIdx) == 0) {
            reorderFromReference(refIdx);
        }
    }

    // Append any remaining entities that were not included
    for (size_t i = 0; i < entities.size(); ++i) {
        if (includedIndices.count(i) == 0) {
            reordered.push_back(entities[i]);
        }
    }

    return reordered;
}

/**
 * @brief Reorders the vector to bring external perimeter (i.e. paths with inset index 0) that are also contours (i.e. external facing lines) to the front.
 *
 * This function uses a stable partition to move all external perimeter contour elements to the front of the vector,
 * while maintaining the relative order of non-contour elements.
 *
 * @param ordered_extrusions The vector of PerimeterGeneratorArachneExtrusion to reorder.
 */
void bringContoursToFront(std::vector<PerimeterGeneratorArachneExtrusion>& ordered_extrusions) {
    std::stable_partition(ordered_extrusions.begin(), ordered_extrusions.end(), [](const PerimeterGeneratorArachneExtrusion& extrusion) {
        return (extrusion.extrusion->is_contour() && extrusion.extrusion->inset_idx==0);
    });
}
// ORCA:
// Inner Outer Inner wall ordering mode perimeter order optimisation functions ended


// Thanks, Cura developers, for implementing an algorithm for generating perimeters with variable width (Arachne) that is based on the paper
// "A framework for adaptive width control of dense contour-parallel toolpaths in fused deposition modeling"
void PerimeterGenerator::process_arachne()
{
    group_region_by_fuzzify(*this);

    // other perimeters
    m_mm3_per_mm = this->perimeter_flow.mm3_per_mm();
    coord_t perimeter_spacing = this->perimeter_flow.scaled_spacing();

    // external perimeters
    m_ext_mm3_per_mm = this->ext_perimeter_flow.mm3_per_mm();
    coord_t ext_perimeter_width = this->ext_perimeter_flow.scaled_width();
    coord_t ext_perimeter_spacing = this->ext_perimeter_flow.scaled_spacing();
    coord_t ext_perimeter_spacing2 = scaled<coord_t>(0.5f * (this->ext_perimeter_flow.spacing() + this->perimeter_flow.spacing()));
    // overhang perimeters
    m_mm3_per_mm_overhang = this->overhang_flow.mm3_per_mm();

    // solid infill
    coord_t solid_infill_spacing = this->solid_infill_flow.scaled_spacing();

    // prepare grown lower layer slices for overhang detection
    if (this->lower_slices != nullptr && this->config->detect_overhang_wall) {
        // We consider overhang any part where the entire nozzle diameter is not supported by the
        // lower layer, so we take lower slices and offset them by half the nozzle diameter used
        // in the current layer
        double nozzle_diameter = this->print_config->nozzle_diameter.get_at(this->config->wall_filament - 1);
        m_lower_slices_polygons = offset(*this->lower_slices, float(scale_(+nozzle_diameter / 2)));
    }

    Surfaces all_surfaces = this->slices->surfaces;

    process_no_bridge(all_surfaces, perimeter_spacing, ext_perimeter_width);
    // BBS: don't simplify too much which influence arc fitting when export gcode if arc_fitting is enabled
    double surface_simplify_resolution = (print_config->enable_arc_fitting && !this->has_fuzzy_skin) ? 0.2 * m_scaled_resolution : m_scaled_resolution;
    // we need to process each island separately because we might have different
    // extra perimeters for each one
    for (const Surface& surface : all_surfaces) {
        coord_t bead_width_0 = ext_perimeter_spacing;
        // detect how many perimeters must be generated for this island
        int loop_number = this->config->wall_loops + surface.extra_perimeters - 1; // 0-indexed loops
        int sparse_infill_density = this->config->sparse_infill_density.value;
        if (this->config->alternate_extra_wall && this->layer_id % 2 == 1 && !m_spiral_vase && sparse_infill_density > 0) // add alternating extra wall
            loop_number++;

        // Set the bottommost layer to be one wall
        const bool is_bottom_layer = (this->layer_id == object_config->raft_layers) ? true : false;
        if (is_bottom_layer && this->config->only_one_wall_first_layer)
            loop_number = 0;

        // Orca: set the topmost layer to be one wall according to the config
        const bool is_topmost_layer = (this->upper_slices == nullptr) ? true : false;
        if (is_topmost_layer && loop_number > 0 && config->only_one_wall_top)
            loop_number = 0;
        
        auto apply_precise_outer_wall = config->precise_outer_wall && config->wall_sequence == WallSequence::InnerOuter;
        // Orca: properly adjust offset for the outer wall if precise_outer_wall is enabled.
        ExPolygons last = offset_ex(surface.expolygon.simplify_p(surface_simplify_resolution),
                       apply_precise_outer_wall? -float(ext_perimeter_width - ext_perimeter_spacing )
                                                 : -float(ext_perimeter_width / 2. - ext_perimeter_spacing / 2.));
        
        Arachne::WallToolPathsParams input_params = Arachne::make_paths_params(this->layer_id, *object_config, *print_config);
        // Set params is_top_or_bottom_layer for adjusting short-wall removal sensitivity.
        input_params.is_top_or_bottom_layer = (is_bottom_layer || is_topmost_layer) ? true : false;

        coord_t wall_0_inset = 0;
        if (apply_precise_outer_wall)
           wall_0_inset = -coord_t(ext_perimeter_width / 2 - ext_perimeter_spacing / 2);

        //PS: One wall top surface for Arachne
        ExPolygons top_expolygons;
        // Calculate how many inner loops remain when TopSurfaces is selected.
        const int inner_loop_number = (config->only_one_wall_top && upper_slices != nullptr) ? loop_number - 1 : -1;

        // Set one perimeter when TopSurfaces is selected.
        if (config->only_one_wall_top && loop_number > 0)
            loop_number = 0;

        Arachne::WallToolPathsParams input_params_tmp = input_params;
        
        Polygons   last_p = to_polygons(last);
        Arachne::WallToolPaths wallToolPaths(last_p, bead_width_0, perimeter_spacing, coord_t(loop_number + 1),
                                               wall_0_inset, layer_height, input_params_tmp);
        std::vector<Arachne::VariableWidthLines>   perimeters = wallToolPaths.getToolPaths();
        ExPolygons  infill_contour = union_ex(wallToolPaths.getInnerContour());

        // Check if there are some remaining perimeters to generate (the number of perimeters
        // is greater than one together with enabled the single perimeter on top surface feature).
        if (inner_loop_number >= 0) {
            assert(upper_slices != nullptr);

            // Infill contour bounding box.
            BoundingBox infill_contour_bbox = get_extents(infill_contour);
            infill_contour_bbox.offset(SCALED_EPSILON);
            
            coord_t perimeter_width = this->perimeter_flow.scaled_width();

            // Get top ExPolygons from current infill contour.
            Polygons upper_slices_clipped;
            if (object_config->interface_shells) {
                auto upper_slicer_same_region = to_expolygons(this->upper_slices_same_region->surfaces);
                upper_slices_clipped = ClipperUtils::clip_clipper_polygons_with_subject_bbox(upper_slicer_same_region, infill_contour_bbox);
            } else
                upper_slices_clipped = ClipperUtils::clip_clipper_polygons_with_subject_bbox(*upper_slices, infill_contour_bbox);

            top_expolygons = diff_ex(infill_contour, upper_slices_clipped);

            if (!top_expolygons.empty()) {
                if (lower_slices != nullptr) {
                    const float      bridge_offset          = float(std::max<coord_t>(ext_perimeter_spacing, perimeter_width));
                    const Polygons   lower_slices_clipped   = ClipperUtils::clip_clipper_polygons_with_subject_bbox(*lower_slices, infill_contour_bbox);
                    const ExPolygons current_slices_bridges = offset_ex(diff_ex(top_expolygons, lower_slices_clipped), bridge_offset);

                    // Remove bridges from top surface polygons.
                    top_expolygons = diff_ex(top_expolygons, current_slices_bridges);
                }

                // Filter out areas that are too thin and expand top surface polygons a bit to hide the wall line.
                // ORCA: skip if the top surface area is smaller than "min_width_top_surface"
                const float top_surface_min_width = std::max<float>(float(ext_perimeter_spacing) / 4.f + scaled<float>(0.00001), float(scale_(config->min_width_top_surface.get_abs_value(unscale_(perimeter_width)))) / 4.f);
                // Shrink the polygon to remove the small areas, then expand it back out plus a maragin to hide the wall line a little.
                // ORCA: Expand the polygon with half the perimeter width in addition to the contracted amount,
                // not the full perimeter width as PS does, to enable thin lettering to print on the top surface without nozzle collisions
                // due to thin lines being generated
                top_expolygons = offset2_ex(top_expolygons, -top_surface_min_width, top_surface_min_width + float(perimeter_width * 0.85));

                // Get the not-top ExPolygons (including bridges) from current slices and expanded real top ExPolygons (without bridges).
                const ExPolygons not_top_expolygons = diff_ex(infill_contour, top_expolygons);

                // Get final top ExPolygons.
                top_expolygons = intersection_ex(top_expolygons, infill_contour);

                const Polygons not_top_polygons = to_polygons(offset_ex(not_top_expolygons,wall_0_inset));
                Arachne::WallToolPaths inner_wall_tool_paths(not_top_polygons, perimeter_spacing, perimeter_spacing, coord_t(inner_loop_number + 1), 0, layer_height, input_params_tmp);
                std::vector<Arachne::VariableWidthLines> inner_perimeters = inner_wall_tool_paths.getToolPaths();

                // Recalculate indexes of inner perimeters before merging them.
                if (!perimeters.empty()) {
                    for (Arachne::VariableWidthLines &inner_perimeter : inner_perimeters) {
                        if (inner_perimeter.empty())
                            continue;
                        for (Arachne::ExtrusionLine &el : inner_perimeter)
                            ++el.inset_idx;
                    }
                }

                perimeters.insert(perimeters.end(), inner_perimeters.begin(), inner_perimeters.end());
                infill_contour = union_ex(top_expolygons, inner_wall_tool_paths.getInnerContour());
            } else {
                // There is no top surface ExPolygon, so we call Arachne again with parameters
                // like when the single perimeter feature is disabled.
                Arachne::WallToolPaths no_single_perimeter_tool_paths(last_p, bead_width_0, perimeter_spacing, coord_t(inner_loop_number + 2), wall_0_inset, layer_height, input_params_tmp);
                perimeters     = no_single_perimeter_tool_paths.getToolPaths();
                infill_contour = union_ex(no_single_perimeter_tool_paths.getInnerContour());
            }
        }
        //PS

        loop_number = int(perimeters.size()) - 1;

        #ifdef ARACHNE_DEBUG
        {
            static int iRun = 0;
            export_perimeters_to_svg(debug_out_path("arachne-perimeters-%d-%d.svg", layer_id, iRun++), to_polygons(last), perimeters, union_ex(wallToolPaths.getInnerContour()));
        }
#endif

        // All closed ExtrusionLine should have the same the first and the last point.
        // But in rare cases, Arachne produce ExtrusionLine marked as closed but without
        // equal the first and the last point.
        assert([&perimeters = std::as_const(perimeters)]() -> bool {
            for (const Arachne::VariableWidthLines& perimeter : perimeters)
                for (const Arachne::ExtrusionLine& el : perimeter)
                    if (el.is_closed && el.junctions.front().p != el.junctions.back().p)
                        return false;
            return true;
        }());

        int start_perimeter = int(perimeters.size()) - 1;
        int end_perimeter = -1;
        int direction = -1;

		bool is_outer_wall_first =
            	this->config->wall_sequence == WallSequence::OuterInner ||
            	this->config->wall_sequence == WallSequence::InnerOuterInner;
        
        if (layer_id == 0){ // disable inner outer inner algorithm after the first layer
        	is_outer_wall_first =
            	this->config->wall_sequence == WallSequence::OuterInner;
        }
        if (is_outer_wall_first) {
            start_perimeter = 0;
            end_perimeter = int(perimeters.size());
            direction = 1;
        }

        std::vector<Arachne::ExtrusionLine*> all_extrusions;
        for (int perimeter_idx = start_perimeter; perimeter_idx != end_perimeter; perimeter_idx += direction) {
            if (perimeters[perimeter_idx].empty())
                continue;
            for (Arachne::ExtrusionLine& wall : perimeters[perimeter_idx])
                all_extrusions.emplace_back(&wall);
        }

        // Find topological order with constraints from extrusions_constrains.
        std::vector<size_t>              blocked(all_extrusions.size(), 0); // Value indicating how many extrusions it is blocking (preceding extrusions) an extrusion.
        std::vector<std::vector<size_t>> blocking(all_extrusions.size());   // Each extrusion contains a vector of extrusions that are blocked by this extrusion.
        std::unordered_map<const Arachne::ExtrusionLine*, size_t> map_extrusion_to_idx;
        for (size_t idx = 0; idx < all_extrusions.size(); idx++)
            map_extrusion_to_idx.emplace(all_extrusions[idx], idx);

        auto extrusions_constrains = Arachne::WallToolPaths::getRegionOrder(all_extrusions, is_outer_wall_first);
        for (auto [before, after] : extrusions_constrains) {
            auto after_it = map_extrusion_to_idx.find(after);
            ++blocked[after_it->second];
            blocking[map_extrusion_to_idx.find(before)->second].emplace_back(after_it->second);
        }

        std::vector<bool> processed(all_extrusions.size(), false);          // Indicate that the extrusion was already processed.
        Point             current_position = all_extrusions.empty() ? Point::Zero() : all_extrusions.front()->junctions.front().p; // Some starting position.
        std::vector<PerimeterGeneratorArachneExtrusion> ordered_extrusions;         // To store our result in. At the end we'll std::swap.
        ordered_extrusions.reserve(all_extrusions.size());

        while (ordered_extrusions.size() < all_extrusions.size()) {
            size_t best_candidate = 0;
            double best_distance_sqr = std::numeric_limits<double>::max();
            bool   is_best_closed = false;

            std::vector<size_t> available_candidates;
            for (size_t candidate = 0; candidate < all_extrusions.size(); ++candidate) {
                if (processed[candidate] || blocked[candidate])
                    continue; // Not a valid candidate.
                available_candidates.push_back(candidate);
            }

            std::sort(available_candidates.begin(), available_candidates.end(), [&all_extrusions](const size_t a_idx, const size_t b_idx) -> bool {
                return all_extrusions[a_idx]->is_closed < all_extrusions[b_idx]->is_closed;
                });

            for (const size_t candidate_path_idx : available_candidates) {
                auto& path = all_extrusions[candidate_path_idx];

                if (path->junctions.empty()) { // No vertices in the path. Can't find the start position then or really plan it in. Put that at the end.
                    if (best_distance_sqr == std::numeric_limits<double>::max()) {
                        best_candidate = candidate_path_idx;
                        is_best_closed = path->is_closed;
                    }
                    continue;
                }

                const Point candidate_position = path->junctions.front().p;
                double      distance_sqr = (current_position - candidate_position).cast<double>().norm();
                if (distance_sqr < best_distance_sqr) { // Closer than the best candidate so far.
                    if (path->is_closed || (!path->is_closed && best_distance_sqr != std::numeric_limits<double>::max()) || (!path->is_closed && !is_best_closed)) {
                        best_candidate = candidate_path_idx;
                        best_distance_sqr = distance_sqr;
                        is_best_closed = path->is_closed;
                    }
                }
            }

            auto& best_path = all_extrusions[best_candidate];
            ordered_extrusions.push_back({ best_path, best_path->is_contour() });
            processed[best_candidate] = true;
            for (size_t unlocked_idx : blocking[best_candidate])
                blocked[unlocked_idx]--;

            if (!best_path->junctions.empty()) { //If all paths were empty, the best path is still empty. We don't upate the current position then.
                if (best_path->is_closed)
                    current_position = best_path->junctions[0].p; //We end where we started.
                else
                    current_position = best_path->junctions.back().p; //Pick the other end from where we started.
            }
        }

       // printf("New Layer: Layer ID %d\n",layer_id); //debug - new layer
        if (this->config->wall_sequence == WallSequence::InnerOuterInner && layer_id > 0) { // only enable inner outer inner algorithm after first layer
            if (ordered_extrusions.size() > 2) { // 3 walls minimum needed to do inner outer inner ordering
                int position = 0; // index to run the re-ordering for multiple external perimeters in a single island.
                int arr_i, arr_j = 0;    // indexes to run through the walls in the for loops
                int outer, first_internal, second_internal, max_internal, current_perimeter; // allocate index values
                
                // To address any remaining scenarios where the outer perimeter contour is not first on the list as arachne sometimes reorders the perimeters when clustering
                // for OI mode that is used the basis for IOI
                bringContoursToFront(ordered_extrusions);
                std::vector<PerimeterGeneratorArachneExtrusion> reordered_extrusions;
                
                // Debug statement to print spacing values:
                //printf("External threshold - Ext perimeter: %d Ext spacing: %d Int perimeter: %d Int spacing: %d\n", this->ext_perimeter_flow.scaled_width(),this->ext_perimeter_flow.scaled_spacing(),this->perimeter_flow.scaled_width(), this->perimeter_flow.scaled_spacing());

                // Get searching thresholds. For an external perimeter we take the external perimeter spacing/2 plus the internal perimeter spacing/2 and expand by the factor
                // rounding errors. When precise wall is enabled, the external perimeter full spacing is used.
                coord_t threshold_external = (apply_precise_outer_wall)
                    // Precise outer wall ⇒ use “full external spacing”
                    ? ( this->ext_perimeter_flow.scaled_spacing()
                        + this->perimeter_flow.scaled_spacing()/2.0 )
                    // Normal ⇒ half ext spacing + half int spacing
                    : ( this->ext_perimeter_flow.scaled_spacing()/2.0
                        + this->perimeter_flow.scaled_spacing()/2.0 );
                
                // For the intenal perimeter threshold, the distance is the internal perimeter spacing expanded by the factor to cover rounding errors.
                coord_t threshold_internal = this->perimeter_flow.scaled_spacing();
                
                // Re-order extrusions based on distance
                // Alorithm will aggresively optimise for the appearance of the outermost perimeter
                ordered_extrusions = reorderPerimetersByProximity(ordered_extrusions,threshold_external,threshold_internal );
                reordered_extrusions = ordered_extrusions; // copy them into the reordered extrusions vector to allow for IOI operations to be performed below without altering the base ordered extrusions list.
                
                // Now start the sandwich mode wall re-ordering using the reordered_extrusions as the basis
                // scan to find the external perimeter, first internal, second internal and last perimeter in the island.
                // We then advance the position index to move to the second island and continue until there are no more
                // perimeters left.
                while (position < reordered_extrusions.size()) {
                    outer = first_internal = second_internal = current_perimeter = -1; // initialise all index values to -1
                    max_internal = reordered_extrusions.size()-1; // initialise the maximum internal perimeter to the last perimeter on the extrusion list
                    // run through the walls to get the index values that need re-ordering until the first one for each
                    // is found. Start at "position" index to enable the for loop to iterate for multiple external
                    // perimeters in a single island
                    // printf("Reorder Loop. Position %d, extrusion list size: %d, Outer index %d, inner index %d, second inner index %d\n", position, reordered_extrusions.size(),outer,first_internal,second_internal);
                    for (arr_i = position; arr_i < reordered_extrusions.size(); ++arr_i) {
                        // printf("Perimeter: extrusion inset index %d, ordered extrusions array position %d\n",reordered_extrusions[arr_i].extrusion->inset_idx, arr_i);
                        switch (reordered_extrusions[arr_i].extrusion->inset_idx) {
                            case 0: // external perimeter
                                if (outer == -1)
                                    outer = arr_i;
                                break;
                            case 1: // first internal wall
                                if (first_internal==-1 && arr_i>outer && outer!=-1){
                                    first_internal = arr_i;
                                }
                                break;
                            case 2: // second internal wall
                                if (second_internal == -1 && arr_i > first_internal && outer!=-1){
                                    second_internal = arr_i;
                                }
                                break;
                        }
                        if(outer >-1 && first_internal>-1 && reordered_extrusions[arr_i].extrusion->inset_idx == 0){  // found a new external perimeter after we've found at least a first internal perimeter to re-order.
                                                                                                                      // This means we entered a new island.
                            arr_i=arr_i-1; //step back one perimeter
                            max_internal = arr_i; // new maximum internal perimeter is now this as we have found a new external perimeter, hence a new island.
                            break; // exit the for loop
                        }
                    }
                    
                    // printf("Layer ID %d, Outer index %d, inner index %d, second inner index %d, maximum internal perimeter %d \n",layer_id,outer,first_internal,second_internal, max_internal);
                    if (outer > -1 && first_internal > -1 && second_internal > -1) { // found all three perimeters to re-order? If not the perimeters will be processed outside in.
                        std::vector<PerimeterGeneratorArachneExtrusion> inner_outer_extrusions; // temporary array to hold extrusions for reordering
                        inner_outer_extrusions.resize(max_internal - position + 1); // reserve array containing the number of perimeters before a new island. Variables are array indexes hence need to add +1 to convert to position allocations
                        // printf("Allocated array size %d, max_internal index %d, start position index %d \n",max_internal-position+1,max_internal,position);
                        
                        for (arr_j = max_internal; arr_j >=position; --arr_j){ // go inside out towards the external perimeter (perimeters in reverse order) and store all internal perimeters until the first one identified with inset index 2
                            if(arr_j >= second_internal){
                                //printf("Inside out loop: Mapped perimeter index %d to array position %d\n", arr_j, max_internal-arr_j);
                                inner_outer_extrusions[max_internal-arr_j] = reordered_extrusions[arr_j];
                                current_perimeter++; 
                            }
                        }
                        
                        for (arr_j = position; arr_j < second_internal; ++arr_j){ // go outside in and map the remaining perimeters (external and first internal wall(s)) using the outside in wall order
                            // printf("Outside in loop: Mapped perimeter index %d to array position %d\n", arr_j, current_perimeter+1);
                            inner_outer_extrusions[++current_perimeter] = reordered_extrusions[arr_j];
                        }
                        
                        for(arr_j = position; arr_j <= max_internal; ++arr_j) // replace perimeter array with the new re-ordered array
                            ordered_extrusions[arr_j] = inner_outer_extrusions[arr_j-position];
                    }
                    // go to the next perimeter from the current position to continue scanning for external walls in the same island
                    position = arr_i + 1;
                }
            }
        }
        
        bool steep_overhang_contour = false;
        bool steep_overhang_hole    = false;
        const WallDirection wall_direction = config->wall_direction;
        if (wall_direction != WallDirection::Auto) {
            // Skip steep overhang detection if wall direction is specified
            steep_overhang_contour = true;
            steep_overhang_hole    = true;
        }
        if (ExtrusionEntityCollection extrusion_coll = traverse_extrusions(*this, ordered_extrusions, steep_overhang_contour, steep_overhang_hole); !extrusion_coll.empty()) {
            // All walls are counter-clockwise initially, so we don't need to reorient it if that's what we want
            if (wall_direction != WallDirection::CounterClockwise) {
                reorient_perimeters(extrusion_coll, steep_overhang_contour, steep_overhang_hole,
                                    // Reverse internal only if the wall direction is auto
                                    this->config->overhang_reverse_internal_only && wall_direction == WallDirection::Auto);
            }
            this->loops->append(extrusion_coll);
        }

        const coord_t spacing = (perimeters.size() == 1) ? ext_perimeter_spacing2 : perimeter_spacing;

        if (offset_ex(infill_contour, -float(spacing / 2.)).empty())
            infill_contour.clear(); // Infill region is too small, so let's filter it out.

        // create one more offset to be used as boundary for fill
        // we offset by half the perimeter spacing (to get to the actual infill boundary)
        // and then we offset back and forth by half the infill spacing to only consider the
        // non-collapsing regions
        coord_t inset =
            (loop_number < 0) ? 0 :
            (loop_number == 0) ?
            // one loop
            ext_perimeter_spacing :
            // two or more loops?
            perimeter_spacing;
        coord_t top_inset = inset;
        
        top_inset = coord_t(scale_(this->config->top_bottom_infill_wall_overlap.get_abs_value(unscale<double>(inset))));
        if(is_topmost_layer || is_bottom_layer)
            inset = coord_t(scale_(this->config->top_bottom_infill_wall_overlap.get_abs_value(unscale<double>(inset))));
        else
            inset = coord_t(scale_(this->config->infill_wall_overlap.get_abs_value(unscale<double>(inset))));
        
        // simplify infill contours according to resolution
        Polygons pp;
        for (ExPolygon& ex : infill_contour)
            ex.simplify_p(m_scaled_resolution, &pp);
        ExPolygons not_filled_exp = union_ex(pp);
        // collapse too narrow infill areas
        const auto    min_perimeter_infill_spacing = coord_t(solid_infill_spacing * (1. - INSET_OVERLAP_TOLERANCE));

        ExPolygons infill_exp = offset2_ex(
            not_filled_exp,
            float(-min_perimeter_infill_spacing / 2.),
            float(inset + min_perimeter_infill_spacing / 2.));
        // append infill areas to fill_surfaces
        if (!top_expolygons.empty()) {
            infill_exp = union_ex(infill_exp, offset_ex(top_expolygons, double(top_inset)));
        }
        this->fill_surfaces->append(infill_exp, stInternal);

        apply_extra_perimeters(infill_exp);

        // BBS: get the no-overlap infill expolygons
        {
            ExPolygons polyWithoutOverlap;
            polyWithoutOverlap = offset2_ex(
                not_filled_exp,
                float(-min_perimeter_infill_spacing / 2.),
                float(+min_perimeter_infill_spacing / 2.));
            if (!top_expolygons.empty())
                polyWithoutOverlap = union_ex(polyWithoutOverlap, top_expolygons);
            this->fill_no_overlap->insert(this->fill_no_overlap->end(), polyWithoutOverlap.begin(), polyWithoutOverlap.end());
        }
    }
}

bool PerimeterGeneratorLoop::is_internal_contour() const
{
    // An internal contour is a contour containing no other contours
    if (! this->is_contour)
        return false;
    for (const PerimeterGeneratorLoop &loop : this->children)
        if (loop.is_contour)
            return false;
    return true;
}

std::vector<Polygons> PerimeterGenerator::generate_lower_polygons_series(float width)
{
    float nozzle_diameter = print_config->nozzle_diameter.get_at(config->wall_filament - 1);
    float start_offset = -0.5 * width;
    float end_offset = 0.5 * nozzle_diameter;

    assert(overhang_sampling_number >= 3);
    // generate offsets
    std::vector<float> offset_series;
    offset_series.reserve(2);

     offset_series.push_back(start_offset + 0.5 * (end_offset - start_offset) / (overhang_sampling_number - 1));
     offset_series.push_back(end_offset);
    std::vector<Polygons> lower_polygons_series;
    if (this->lower_slices == NULL) {
        return lower_polygons_series;
    }

    // offset expolygon to generate series of polygons
    for (int i = 0; i < offset_series.size(); i++) {
        lower_polygons_series.emplace_back(offset(*this->lower_slices, float(scale_(offset_series[i]))));
    }
    return lower_polygons_series;
}

}
