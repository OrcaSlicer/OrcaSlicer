#include "OrganicToolpaths.hpp"
#include "BeltFloorContext.hpp"
#include "SupportCommon.hpp"
#include "SupportLayer.hpp"
#include "SupportParameters.hpp"
#include "TreeSupportCommon.hpp"
#include "../AABBTreeIndirect.hpp"
#include "../BoundingBox.hpp"
#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../ExtrusionEntity.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Fill/FillBase.hpp"
#include "../FilamentMixer.hpp"
#include "../GCode/ToolOrdering.hpp"
#include "../I18N.hpp"
#include "../LocalesUtils.hpp"
#include "../MultiNozzleUtils.hpp"
#include "../Model.hpp"
#include "../Flow.hpp"
#include "../Geometry/MedialAxis.hpp"
#include "../Layer.hpp"
#include "../Line.hpp"
#include "../Point.hpp"
#include "../Polygon.hpp"
#include "../Polyline.hpp"
#include "../Print.hpp"
#include "../PrintBase.hpp"
#include "../PrintConfig.hpp"
#include "../Slicing.hpp"
#include "../Surface.hpp"
#include "../VariableWidth.hpp"
#include "../libslic3r.h"
#include "../format.hpp"

#include <clipper2/clipper.core.h>
#include <clipper2/clipper.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {

static float bead_reach_offset(double width);
static void  throw_if_canceled(const PrintObject &object);

// Reserve dense feet in a sparse flange so trees cannot fall between its lines.
// A solid flange already holds them; partitioning it would add internal ring perimeters.
// A lone foot starts no tree: it holds what the layer above prints over the inside of a body. It is reserved only where
// the first layer would leave some of that over nothing: where a bead within the cover of a lone foot, the farthest a
// load is held from a tip (reach under a roof, a line and a half under a tree, see
// organic_grow_trees_inside_hollow_bodies()), may lie farther than half its width from a bead printed, see
// bead_reach(). The bead is as narrow as the layer above may print one: Arachne does not print the spiral of a solid
// part narrower than 0.85 of the nozzle, see organic_extrude_solid_fill() and FillSpiralInset. A reserved foot prints
// the bead of a loop half a line inside its outline and lines that end within the area its fill leaves them, see
// fill_expolygons_with_sheath_generate_paths(), FillRectilinear and FillSupportBase: a line long enough to print passes
// through every disc in that area a little wider than a spacing, so the loop and the reach of such discs surely hold,
// and the paths of the pads decide elsewhere. Paths too short to print hold nothing. Each island is checked by what it
// prints alone, a part of what the layer prints, and only a lone foot on it changes that: the part of its lone feet
// within their cover of a place over nothing is reserved, or a whole foot where some of that part is too narrow to
// print or none of the foot is left there, and the layer is filled again and checked again over the cover of every lone
// foot. The last round, or one that would reserve nothing more, refills every island with a place still over nothing
// with all its lone feet reserved, and reserves the rest of every lone foot whose cover reaches an island refilled, its
// own island refilled in turn: a load the trees took as held by that foot across the gap between the islands is held by
// it again, as when every lone foot was reserved. Lines more than a line and that bead apart leave the middle of the
// gaps between them farther than half the bead from any, so all are reserved.
// The foot of a tree holds the tree above it, which may lean or merge a little beyond it, and what the trees took it
// to hold as they took a lone foot to, see organic_grow_trees_inside_hollow_bodies(): what lies within its cover.
// Closer lines check that cover too: a foot of a tree with a place there over nothing is reserved whole, as among
// lines farther apart, and the check starts again, the lone feet checked beside every foot of a tree reserved as at
// first. The feet of the trees never reserve a lone foot. Those on an island refilled in the last round are reserved
// too, as what the lone feet reserved there take from its flange is not checked again. After a few starts every foot
// of a tree is reserved.
void organic_fill_first_layer(ExtrusionEntitiesPtr &dst, const Polygons &polygons, const Polygons &feet, const Polygons &lone_feet,
    Fill *filler, float density, ExtrusionRole role, const Flow &flow, const SupportParameters &support_params,
    const PrintObject &object)
{
    const Polygons trees = density < 1.f ? intersection(polygons, feet) : Polygons();
    const Polygons lone  = density < 1.f ? diff(intersection(polygons, lone_feet), trees) : Polygons();
    Polygons       pads;
    const auto fill_flange = [&](ExtrusionEntitiesPtr &out) {
        fill_expolygons_with_sheath_generate_paths(out, pads.empty() ? polygons : diff(polygons, pads), filler, density,
            role, flow, support_params, true, true);
    };
    const auto fill_pads = [&](ExtrusionEntitiesPtr &out) {
        if (! pads.empty())
            fill_expolygons_with_sheath_generate_paths(out, pads, filler, 1.f, role, flow, support_params, true, true);
    };
    const coord_t width   = support_params.support_material_flow.scaled_width();
    const auto   &nozzles = object.print()->config().nozzle_diameter.values;
    const double  bead    = std::min(double(width), scaled<double>(0.85 * *std::min_element(nozzles.begin(), nozzles.end())));
    if (double(flow.scaled_spacing()) / density - double(flow.scaled_width()) > bead) {
        pads = lone.empty() ? trees : union_(trees, lone);
    } else if (! trees.empty() || ! lone.empty()) {
        const float  cover      = float(std::max(0.5 * double(organic_base_span(object) + width), 1.5 * double(width)));
        const float  printable  = 0.5f * float(flow.scaled_width());
        const float  reach_bead = bead_reach_offset(bead);
        const double tolerance  = scaled<double>(0.005);
        const auto   solid      = [tolerance](ExPolygons pieces) {
            pieces.erase(std::remove_if(pieces.begin(), pieces.end(), [tolerance](const ExPolygon &piece) {
                return offset(piece, - float(tolerance)).empty(); }), pieces.end());
            return pieces;
        };
        // Where the axis of a bead surely touches what the reserved feet print: within reach of the bead of a loop as it is
        // printed, its seam clipped and a dot left out, or of a line through a disc where the fill lets lines end, wide
        // enough for a line within half a spacing of its centre to run across it no shorter than a dot.
        const double spacing      = double(flow.scaled_spacing());
        const float  edge         = printable + reach_bead;
        const double dot          = 0.25 * std::min(double(flow.scaled_width()), double(width));
        const auto   pads_holding = [&](const Polygons &here) {
            const ExPolygons outlines = closing_ex(here, float(SCALED_EPSILON), float(SCALED_EPSILON) + printable);
            Polylines        loops;
            for (const ExPolygon &outline : outlines)
                for (Polyline &loop : draw_perimeters(outline, 0.15 * spacing))
                    if (loop.length() >= dot)
                        loops.emplace_back(std::move(loop));
            Polygons       held = offset(loops, edge, jtRound, tolerance, etOpenRound);
            const Polygons ends = offset(offset(to_polygons(outlines), - float(0.4 * spacing)), - float(0.5 * spacing));
            append(held, offset(offset(ends, - float(0.5 * std::hypot(spacing, dot)), jtRound, tolerance), edge - float(0.5 * spacing),
                jtRound, tolerance));
            return held;
        };
        const ExPolygons         islands = closing_ex(polygons, float(SCALED_EPSILON));
        std::vector<BoundingBox> island_boxes;
        for (const ExPolygon &island : islands)
            island_boxes.emplace_back(get_extents(island.contour));
        // The island of a point, of each lone foot, and the cover of every lone foot, island by island.
        const auto island_of = [&](const Point &point) {
            for (size_t n = 0; n < islands.size(); ++ n)
                if (island_boxes[n].contains(point) && islands[n].contains(point))
                    return n;
            return islands.size();
        };
        // A foot lies on one island, though a point of its outline cut along that of the island may round off it.
        const auto island_of_foot = [&](const ExPolygon &foot) {
            for (const Point &point : foot.contour.points)
                if (const size_t n = island_of(point); n < islands.size())
                    return n;
            const BoundingBox box = get_extents(foot.contour);
            for (size_t n = 0; n < islands.size(); ++ n)
                if (island_boxes[n].overlap(box) && ! intersection(to_polygons(foot), to_polygons(islands[n])).empty())
                    return n;
            return islands.size();
        };
        const ExPolygons                 lone_all = solid(union_ex(lone));
        std::vector<std::vector<size_t>> island_feet(islands.size());
        std::vector<size_t>              foot_island(lone_all.size());
        std::vector<std::vector<size_t>> reaches(lone_all.size());
        NearPolygons                     zones_near;
        for (size_t k = 0; k < lone_all.size(); ++ k) {
            throw_if_canceled(object);
            foot_island[k] = island_of_foot(lone_all[k]);
            if (foot_island[k] < islands.size()) {
                island_feet[foot_island[k]].push_back(k);
                reaches[k].push_back(foot_island[k]);
            }
            const Polygons    zone = offset(to_polygons(lone_all[k]), cover, jtRound, tolerance);
            const BoundingBox box  = get_extents(zone);
            for (size_t n = 0; n < islands.size(); ++ n)
                if (n != foot_island[k] && island_boxes[n].overlap(box) &&
                    ! intersection(zone,
                        ClipperUtils::clip_clipper_polygons_with_subject_bbox(islands[n], box)).empty())
                    reaches[k].push_back(n);
            zones_near.append(zone);
        }
        std::vector<Polygons> zones(islands.size());
        for (size_t n = 0; n < islands.size(); ++ n)
            if (const Polygons zone = zones_near.around({ island_boxes[n] }); ! zone.empty())
                zones[n] = intersection(ClipperUtils::clip_clipper_polygons_with_subject_bbox(zone, island_boxes[n].inflated(SCALED_EPSILON)),
                    to_polygons(islands[n]));
        // The feet of the trees, the cover of each on its island, and those reserved: one on no island is.
        const ExPolygons                 tree_all = solid(union_ex(trees));
        std::vector<size_t>              tree_island(tree_all.size());
        std::vector<std::vector<size_t>> island_trees(islands.size());
        std::vector<Polygons>            tree_covers(tree_all.size());
        std::vector<bool>                tree_reserved(tree_all.size(), false);
        for (size_t t = 0; t < tree_all.size(); ++ t) {
            throw_if_canceled(object);
            tree_island[t] = island_of_foot(tree_all[t]);
            if (tree_island[t] < islands.size()) {
                island_trees[tree_island[t]].push_back(t);
                const Polygons tree_cover = offset(to_polygons(tree_all[t]), cover, jtRound, tolerance);
                tree_covers[t] = intersection(tree_cover, ClipperUtils::clip_clipper_polygons_with_subject_bbox(
                    islands[tree_island[t]], get_extents(tree_cover).inflated(SCALED_EPSILON)));
            } else
                tree_reserved[t] = true;
        }
        // The check starts with the feet of the trees reserved as pads and the cover of the others to check, island by
        // island, and every lone foot left; after a few starts with every foot of a tree reserved.
        Polygons              left;
        std::vector<Polygons> tree_zones;
        int                   starts = 0;
        const auto            start  = [&]() {
            if (++ starts > 3)
                std::fill(tree_reserved.begin(), tree_reserved.end(), true);
            pads.clear();
            tree_zones.assign(islands.size(), Polygons());
            for (size_t t = 0; t < tree_all.size(); ++ t)
                if (tree_reserved[t])
                    append(pads, to_polygons(tree_all[t]));
                else
                    append(tree_zones[tree_island[t]], tree_covers[t]);
            pads = union_(pads);
            left = to_polygons(lone_all);
        };
        start();
        for (int round = 0;; ++ round) {
            throw_if_canceled(object);
            ExtrusionEntityCollection flange, reserved;
            fill_flange(flange.entities);
            fill_pads(reserved.entities);
            // The axes printed on each island, by width, but of the paths too short to print, see organic_remove_dots():
            // each expolygon of the flange and of the pads is filled into a collection of its own.
            using Axes = std::vector<std::map<float, Polylines>>;
            const auto by_island = [&](const ExtrusionEntityCollection &printed) {
                Axes axes(islands.size());
                for (const ExtrusionEntity *entity : printed.entities) {
                    size_t n = islands.size();
                    for_each_extrusion_path(*entity, [&](const ExtrusionPath &path) {
                        if (path.polyline.size() < 2)
                            return;
                        if (n == islands.size())
                            n = island_of(path.first_point());
                        if (n < islands.size() && (! zones[n].empty() || ! tree_zones[n].empty()) &&
                            path.length() >= 0.25 * std::min(double(scale_(path.width)), double(width)))
                            axes[n][path.width].emplace_back(path.as_polyline());
                    });
                }
                return axes;
            };
            const Axes flange_axes = by_island(flange);
            const Axes pad_axes    = by_island(reserved);
            // Where the axis of a bead touches the axes near box.
            const auto reach = [&](const std::map<float, Polylines> &axes, const BoundingBox &box) {
                Polygons held;
                for (const auto &[path_width, paths] : axes) {
                    Polylines near_paths;
                    for (const Polyline &axis : paths)
                        if (get_extents(axis).overlap(box))
                            near_paths.push_back(axis);
                    append(held, offset(near_paths, float(0.5 * scale_(path_width)) + reach_bead, jtRound, tolerance, etOpenRound));
                }
                return held;
            };
            NearPolygons pads_near;
            pads_near.append(pads);
            // The places of a zone on island n over nothing.
            const auto bare_in = [&](const size_t n, const Polygons &zone) {
                const BoundingBox near    = get_extents(zone).inflated(flow.scaled_width() + width);
                Polygons          holding = reach(flange_axes[n], near);
                append(holding, pads_holding(intersection(pads_near.around({ near }), to_polygons(islands[n]))));
                ExPolygons bare = solid(diff_ex(zone, holding));
                if (! bare.empty())
                    // The paths of the pads decide where the model of them leaves places over nothing.
                    bare = solid(diff_ex(bare,
                        reach(pad_axes[n], get_extents(bare).inflated(flow.scaled_width() + width))));
                return bare;
            };
            // A foot of a tree with a place within its cover over nothing is reserved, and the check starts again.
            bool tree_marked = false;
            for (size_t n = 0; n < islands.size(); ++ n)
                if (! tree_zones[n].empty()) {
                    throw_if_canceled(object);
                    if (const ExPolygons bare = bare_in(n, tree_zones[n]); ! bare.empty()) {
                        NearPolygons bare_near;
                        bare_near.append(to_polygons(bare));
                        for (const size_t t : island_trees[n])
                            if (! tree_reserved[t]) {
                                const Polygons around = bare_near.around({ get_extents(tree_covers[t]) });
                                if (! around.empty() && ! intersection(tree_covers[t], around).empty())
                                    tree_reserved[t] = tree_marked = true;
                            }
                    }
                }
            if (tree_marked) {
                start();
                round = -1;
                continue;
            }
            std::vector<ExPolygons> bare(islands.size());
            bool                    any_bare = false;
            for (size_t n = 0; n < islands.size(); ++ n)
                if (! left.empty() && ! zones[n].empty()) {
                    throw_if_canceled(object);
                    bare[n]  = bare_in(n, zones[n]);
                    any_bare = any_bare || ! bare[n].empty();
                }
            if (! any_bare) {
                // What was checked is printed.
                append(dst, std::move(flange.entities));
                flange.entities.clear();
                append(dst, std::move(reserved.entities));
                reserved.entities.clear();
                return;
            }
            if (round < 2) {
                Polygons     part;
                NearPolygons left_near;
                left_near.append(left);
                for (size_t n = 0; n < islands.size(); ++ n)
                    if (! bare[n].empty()) {
                        throw_if_canceled(object);
                        // The parts of the lone feet left on the island within their cover of such places, wide enough to
                        // print with the corners the opening cuts off them.
                        const Polygons island = to_polygons(islands[n]);
                        NearPolygons hit_near;
                        for (const ExPolygon &piece : bare[n])
                            hit_near.append(offset(to_polygons(piece), cover, jtRound, tolerance));
                        const Polygons &hit   = hit_near.polygons;
                        const Polygons raw    = intersection(intersection(left_near.around({ island_boxes[n] }), island), hit);
                        Polygons       got    = opening(raw, printable, jtRound, tolerance);
                        got = union_(got, intersection(offset(got, printable, jtRound, tolerance), raw));
                        // A foot with more of its part too narrow to print, or with none of it left near such a place, whole.
                        NearPolygons raw_near;
                        raw_near.append(raw);
                        NearPolygons lost_near;
                        lost_near.append(to_polygons(solid(diff_ex(raw, got))));
                        for (const size_t k : island_feet[n]) {
                            const Polygons    whole = to_polygons(lone_all[k]);
                            const BoundingBox box   = get_extents(lone_all[k].contour);
                            const auto        meets = [&whole, &box](const NearPolygons &near) {
                                const Polygons around = near.around({ box });
                                return ! around.empty() && ! intersection(whole, around).empty();
                            };
                            if (meets(hit_near) && (meets(lost_near) || ! meets(raw_near)))
                                append(got, whole);
                        }
                        append(part, std::move(got));
                    }
                if (! solid(diff_ex(part, pads)).empty()) {
                    left = to_polygons(solid(diff_ex(left, part)));
                    append(pads, std::move(part));
                    pads = union_(pads);
                    continue;
                }
            }
            // Every island with a place still over nothing is refilled, and the rest of every lone foot whose cover reaches
            // an island refilled is reserved, its own island refilled in turn, and so is every foot of a tree on one.
            NearPolygons          left_near;
            left_near.append(left);
            std::vector<Polygons> rest(lone_all.size());
            for (size_t k = 0; k < lone_all.size(); ++ k)
                if (const Polygons near_left = left_near.around({ get_extents(lone_all[k].contour) }); ! near_left.empty())
                    rest[k] = intersection(near_left, to_polygons(lone_all[k]));
            std::vector<bool> refilled(islands.size(), false);
            std::vector<bool> reserve(lone_all.size(), false);
            for (size_t n = 0; n < islands.size(); ++ n)
                refilled[n] = ! bare[n].empty();
            for (bool more = true; more;) {
                more = false;
                for (size_t k = 0; k < lone_all.size(); ++ k)
                    if (! reserve[k] && ! rest[k].empty() &&
                        std::any_of(reaches[k].begin(), reaches[k].end(), [&refilled](size_t n) { return bool(refilled[n]); })) {
                        reserve[k] = true;
                        if (foot_island[k] < islands.size() && ! refilled[foot_island[k]]) {
                            refilled[foot_island[k]] = true;
                            more                     = true;
                        }
                    }
            }
            for (size_t k = 0; k < lone_all.size(); ++ k)
                if (reserve[k])
                    append(pads, std::move(rest[k]));
            for (size_t t = 0; t < tree_all.size(); ++ t)
                if (! tree_reserved[t] && refilled[tree_island[t]])
                    append(pads, to_polygons(tree_all[t]));
            pads = union_(pads);
            break;
        }
    }
    fill_flange(dst);
    fill_pads(dst);
}

// Loops filling region from its outline in, the first half a line inside it. A loop leaves the middle of the region
// open, which what the layer above prints over it crosses, and the middle moves from layer to layer as a tree leans and
// trees grow into one, as a lattice of tips under a load grows. Where the gap inside the beads of the loops is wider than
// a line, another loop goes round it half a line inside: a bead over a gap no wider than a line touches the beads on both
// sides of it wherever it crosses it, see hanging_parts(). A loop shorter than a line is a dot and is left out.
static Polylines organic_inward_loops(const Polygons &region, const Flow &flow)
{
    Polylines   loops;
    const float width = float(flow.scaled_width());
    for (ExPolygon &part : closing_ex(region, float(SCALED_EPSILON), float(SCALED_EPSILON) + 0.5f * width))
        for (ExPolygons rings{ std::move(part) }; ! rings.empty();) {
            ExPolygons inside;
            for (const ExPolygon &ring : rings) {
                Polylines ring_loops = draw_perimeters(ring, 0.15 * flow.scaled_spacing());
                Polygons  beads;
                for (const Polyline &loop : ring_loops)
                    append(beads, organic_bead(loop, width));
                for (const ExPolygon &gap : diff_ex(ring, beads))
                    if (ExPolygons ring_in = offset_ex(gap, - 0.5f * width, jtRound, scaled<double>(0.005)); ! ring_in.empty())
                        append(inside, std::move(ring_in));
                append(loops, std::move(ring_loops));
            }
            rings = std::move(inside);
        }
    loops.erase(std::remove_if(loops.begin(), loops.end(),
        [&flow](const Polyline &loop) { return loop.length() < double(flow.scaled_width()); }), loops.end());
    return loops;
}

// The same inner loops are needed in the ordinary and raft passes. Cut them against what that pass actually prints
// and splice loops touching a base wall into that wall, preserving the original path's width and flow.
void organic_generate_inner_tree_paths(ExtrusionEntitiesPtr &dst, const Polygons &trees, const Polygons &printed, const Flow &flow,
    const ExtrusionEntitiesPtr *walls_given)
{
    Polylines loops = organic_inward_loops(diff(trees, offset(union_(printed), - 0.25f * float(flow.scaled_width()))), flow);
    std::vector<ExtrusionPath*> walls;
    auto add_walls = [&walls](const ExtrusionEntitiesPtr &entities) {
        for (ExtrusionEntity *entity : entities)
            if (auto *path = dynamic_cast<ExtrusionPath*>(entity); path != nullptr && path->role() == ExtrusionRole::erSupportMaterial)
                walls.push_back(path);
    };
    const ExtrusionEntitiesPtr &candidates = walls_given != nullptr ? *walls_given : dst;
    add_walls(candidates);
    for (ExtrusionEntity *entity : candidates)
        if (auto *collection = dynamic_cast<ExtrusionEntityCollection*>(entity))
            add_walls(collection->entities);
    Polylines axes;
    axes.reserve(walls.size());
    for (const ExtrusionPath *wall : walls)
        axes.emplace_back(wall->as_polyline());
    loops = organic_splice_loops_into_walls(axes, std::move(loops), flow.scaled_width());
    for (size_t i = 0; i < walls.size(); ++ i)
        if (axes[i].points != walls[i]->as_polyline().points)
            *walls[i] = ExtrusionPath(Polyline3(std::move(axes[i])), *walls[i]);
    extrusion_entities_append_paths(dst, std::move(loops),
        ExtrusionRole::erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height());
}

std::vector<SupportGeneratorLayer> organic_bottom_interface_layers(const SupportGeneratorLayersPtr &interfaces)
{
    std::vector<SupportGeneratorLayer> result;
    for (const SupportGeneratorLayer *layer : interfaces)
        if (layer != nullptr) {
            Polygons polygons = layer->bottom_interface_polygons.empty() && layer->layer_type == SupporLayerType::BottomInterface ?
                layer->polygons : intersection(layer->polygons, layer->bottom_interface_polygons);
            if (! polygons.empty()) {
                SupportGeneratorLayer &copy = result.emplace_back();
                copy.layer_type = SupporLayerType::BottomInterface;
                copy.print_z = layer->print_z;
                copy.bottom_z = layer->bottom_z;
                copy.height = layer->height;
                copy.bridging = layer->bridging;
                copy.polygons = std::move(polygons);
            }
        }
    return result;
}

// Whether a part of the axis starts or ends at an end of the axis.
static bool at_axis_end(const Polyline &part, const Polyline &axis)
{
    auto is_axis_end = [&axis](const Point &point) { return point == axis.first_point() || point == axis.last_point(); };
    return is_axis_end(part.first_point()) || is_axis_end(part.last_point());
}

// Whether a run of the axis of a bead outside what holds it hangs: longer than a line and ending in the air or leaving the
// bead-wide corridor between its bearings, or a straight bridge between bearings longer than the support may span. in_widths
// is its length in the widths of its bead and in_spans in such bridges, and segment_width the width of the bead along a
// segment of it.
template<typename SegmentWidth>
static bool run_hangs(const Polyline &run, const Polyline &axis, const bool closed, const double in_widths, const double in_spans,
    const SegmentWidth &segment_width)
{
    // Added up segment by segment in the order the run goes, in_widths and in_spans differ in their last digits from one way
    // along it to the other: a run as long as a width or a span is no longer than it either way.
    constexpr double rounding = 1e-9;
    if (in_widths <= 1. + rounding)
        return false;
    const Line span(run.first_point(), run.last_point());
    // The distance to the span is convex along a segment, so a segment leaves the corridor of its own width at an end if at all.
    bool       bent = false;
    for (size_t i = 0; i + 1 < run.size() && ! bent; ++ i)
        bent = std::max(span.distance_to(run.points[i]), span.distance_to(run.points[i + 1])) > segment_width(i) + scale_(.01);
    return bent || in_spans > 1. + rounding || (! closed && at_axis_end(run, axis));
}

// The runs of an axis of a bead width wide outside reachable that hang.
static Polylines hanging_parts(const Polyline &axis, const double width, const Polygons &reachable, const double max_span)
{
    const bool closed = axis.first_point().distance_to(axis.last_point()) <= width;
    // Only what is reachable near the axis: the whole of a large layer clipped against each axis on its own took most
    // of the time of the checks of the layer. The cut, see organic_outlines_into_box(), drops only points out of a box
    // around the axis a bead wider than it, with their neighbours out of it on the same side, and leaves out the
    // outlines that lie all out of the box: each point of the box lies in what is reachable or not as before, and
    // nothing is rounded.
    const Polygons near = organic_outlines_into_box(reachable, get_extents(axis).inflated(coord_t(std::ceil(width))));
    Polylines  parts;
    if (closed) {
        Points loop = axis.points;
        if (axis.is_closed())
            loop.pop_back();
        // Closed clipping rejoins pieces split at the source seam.
        parts = diff_pl(Polygons{ Polygon(std::move(loop)) }, near);
    } else
        parts = diff_pl(Polylines{ axis }, near);
    parts.erase(std::remove_if(parts.begin(), parts.end(),
        [&](const Polyline &part) {
            const double length = part.length();
            return ! run_hangs(part, axis, closed, length / width, length / max_span, [width](size_t) { return width; });
        }), parts.end());
    return parts;
}

// A path of a bead printed in several paths.
struct BeadPiece {
    Polyline axis;
    coord_t  width;
};

// A run outside what holds a bead of several paths, with the width of the path of each of its segments.
struct BeadRun {
    Polyline             axis;
    std::vector<coord_t> widths;
};

// The parts of the seam from a to b of a bead closed on itself that footing leaves free. Only the round caps of its ends,
// width_a and width_b wide, lie over it: a point of it is held where the cross-section of the caps there meets footing.
static Polylines free_seam_parts(const Point &a, const Point &b, const coord_t width_a, const coord_t width_b, const Polygons &footing)
{
    const Vec2d  from   = a.cast<double>();
    const Vec2d  along  = (b - a).cast<double>();
    const double length = along.norm();
    const Vec2d  dir    = along / length;
    const Vec2d  across = Vec2d(- dir.y(), dir.x()) * double(std::max(width_a, width_b));
    Polygon      cap_a  = make_circle(0.5 * double(width_a), double(SCALED_EPSILON));
    Polygon      cap_b  = make_circle(0.5 * double(width_b), double(SCALED_EPSILON));
    cap_a.translate(a);
    cap_b.translate(b);
    const auto point_at = [&](const Vec2d &p) { return Point(coord_t(std::round(p.x())), coord_t(std::round(p.y()))); };
    Polygon    slab({ point_at(from + across), point_at(from + along + across), point_at(from + along - across), point_at(from - across) });
    slab.make_counter_clockwise();
    // The stretches of the seam over which the caps meet footing.
    std::vector<std::pair<double, double>> held;
    for (const Polygon &contact : intersection(intersection(union_(Polygons{ cap_a, cap_b }), Polygons{ slab }), footing)) {
        double low = length, high = 0.;
        for (const Point &p : contact.points) {
            const double t = std::clamp((p.cast<double>() - from).dot(dir), 0., length);
            low  = std::min(low, t);
            high = std::max(high, t);
        }
        held.emplace_back(low, high);
    }
    // Free whole, however short: it joins the free stretches on both sides of it.
    if (held.empty())
        return { Polyline(a, b) };
    std::sort(held.begin(), held.end());
    const auto at = [&](const double t) { return t <= 0. ? a : t >= length ? b : point_at(from + dir * t); };
    Polylines  free;
    double     start = 0.;
    for (const auto &[low, high] : held) {
        if (low > start + double(SCALED_EPSILON))
            free.emplace_back(at(start), at(low));
        start = std::max(start, high);
    }
    if (length > start + double(SCALED_EPSILON))
        free.emplace_back(at(start), at(length));
    return free;
}

// How many runs hang of a bead of paths of different widths whose whole axis is axis. Each path is held only as far as its
// own width reaches, while a run goes on across the ends of the paths, which are no ends of the bead, each of its segments
// counting against the width of its own path: a short stretch of a wide path lends a narrow end hanging on from it nothing.
// reach(width, path, around) is where a bead width wide stands, as far as around from path.
template<typename Reach, typename Span>
static size_t varying_bead_hanging_runs(const std::vector<BeadPiece> &pieces, const Polyline &axis, const Reach &reach, const Span &max_span)
{
    // Closed on itself where the round caps of its ends touch.
    const bool             closed = axis.first_point().distance_to(axis.last_point()) <= 0.5 * double(pieces.front().width + pieces.back().width);
    std::vector<BeadRun>   runs;
    BeadRun                tail;
    const auto close_tail = [&] {
        if (! tail.axis.points.empty())
            runs.emplace_back(std::move(tail));
        tail = BeadRun{};
    };
    const auto extend = [](BeadRun &run, const Points &points, const size_t from, const coord_t width) {
        run.axis.points.insert(run.axis.points.end(), points.begin() + from, points.end());
        run.widths.resize(run.axis.points.size() - 1, width);
    };
    const auto touches = [](const Polyline &part, const Point &point) { return part.first_point() == point || part.last_point() == point; };
    // The seam of a bead closed on itself where its ends do not quite meet is a path of its own, as wide as the mean of their
    // widths whichever way the bead runs. Only the caps of the ends lie over it, and it is held where they reach.
    const bool             seam  = closed && axis.last_point() != axis.first_point();
    std::vector<BeadPiece> paths = pieces;
    if (seam)
        paths.push_back(BeadPiece{ Polyline(axis.last_point(), axis.first_point()), (pieces.back().width + pieces.front().width) / 2 });
    for (const BeadPiece &piece : paths) {
        Polylines   parts = seam && &piece == &paths.back() ?
            free_seam_parts(piece.axis.first_point(), piece.axis.last_point(), pieces.back().width, pieces.front().width,
                reach(0, piece.axis, std::max(pieces.back().width, pieces.front().width))) :
            diff_pl(Polylines{ piece.axis }, reach(piece.width, piece.axis, piece.width));
        const Point in = piece.axis.first_point(), out = piece.axis.last_point();
        const auto  next = std::find_if(parts.begin(), parts.end(), [&](const Polyline &part) { return touches(part, in); });
        if (next != parts.end() && ! tail.axis.points.empty() && tail.axis.last_point() == in) {
            Polyline part = std::move(*next);
            parts.erase(next);
            if (part.first_point() != in)
                part.reverse();
            extend(tail, part.points, 1, piece.width);
            if (part.last_point() != out)
                close_tail();
        } else
            close_tail();
        for (Polyline &part : parts)
            if (touches(part, out)) {
                if (part.last_point() != out)
                    part.reverse();
                tail = BeadRun{};
                extend(tail, part.points, 0, piece.width);
            } else
                extend(runs.emplace_back(), part.points, 0, piece.width);
    }
    // The last run of a bead closed on itself, ending where it starts, joins its first.
    if (! tail.axis.points.empty() && closed) {
        const auto first = std::find_if(runs.begin(), runs.end(), [&](const BeadRun &run) { return touches(run.axis, axis.first_point()); });
        if (first != runs.end()) {
            if (first->axis.first_point() != axis.first_point()) {
                first->axis.reverse();
                std::reverse(first->widths.begin(), first->widths.end());
            }
            tail.axis.points.insert(tail.axis.points.end(), first->axis.points.begin() + 1, first->axis.points.end());
            tail.widths.insert(tail.widths.end(), first->widths.begin(), first->widths.end());
            runs.erase(first);
        }
    }
    close_tail();
    return size_t(std::count_if(runs.begin(), runs.end(), [&](const BeadRun &run) {
        double in_widths = 0., in_spans = 0.;
        for (size_t i = 0; i < run.widths.size(); ++ i) {
            const double length = (run.axis.points[i + 1] - run.axis.points[i]).cast<double>().norm();
            in_widths += length / double(run.widths[i]);
            in_spans  += length / max_span(double(run.widths[i]));
        }
        return run_hangs(run.axis, axis, closed, in_widths, in_spans, [&run](const size_t i) { return double(run.widths[i]); });
    }));
}

void organic_anchor_bottom_interfaces(const std::vector<SupportLayer*> &layers,
    const std::vector<SupportGeneratorLayer> &interfaces, const SupportParameters &support_params)
{
    if (support_params.num_bottom_interface_layers == 0)
        return;
    struct Load {
        Polyline axis;
        double width;
    };
    struct Plane {
        SupportLayer *layer { nullptr };
        std::vector<const SupportGeneratorLayer*> interfaces;
        std::vector<Load> loads;
    };
    std::map<coord_t, Plane> planes;
    for (const SupportGeneratorLayer &interface : interfaces)
        if (! interface.polygons.empty()) {
            auto layer = std::lower_bound(layers.begin(), layers.end(), interface.print_z - EPSILON,
                [](const SupportLayer *layer, double z) { return layer->print_z < z; });
            if (layer != layers.end() && std::abs((*layer)->print_z - interface.print_z) < EPSILON) {
                Plane &plane = planes[scaled<coord_t>((*layer)->print_z)];
                plane.layer = *layer;
                plane.interfaces.push_back(&interface);
            }
        }
    if (planes.empty())
        return;
    // Float path heights may differ from the layer grid by a few nanometres.
    const auto below = [&](double z) {
        auto plane = planes.lower_bound(scaled<coord_t>(z - EPSILON));
        return plane != planes.end() && std::abs(plane->second.layer->print_z - z) < EPSILON ? plane : planes.end();
    };
    for (const SupportLayer *layer : layers)
        for_each_extrusion_path(layer->support_fills, [&](const ExtrusionPath &path) {
            if (path.role() == ExtrusionRole::erSupportMaterial)
                if (auto plane = below(layer->print_z - path.height); plane != planes.end())
                    plane->second.loads.push_back({path.as_polyline(), scale_(path.width)});
        });
    for (auto it = planes.rbegin(); it != planes.rend(); ++ it) {
        Plane &plane = it->second;
        if (plane.loads.empty())
            continue;
        Polygons loaded, printed;
        for (const Load &load : plane.loads)
            polygons_append(loaded, organic_bead(load.axis, load.width));
        loaded = union_(loaded);
        for_each_extrusion_path(plane.layer->support_fills, [&](const ExtrusionPath &path) {
            polygons_append(printed, organic_bead(path.as_polyline(), scale_(path.width)));
        });
        printed = union_(printed);
        const ExPolygons loads = union_ex(loaded);
        // The loads touching nothing printed, and the runs of the others hanging over it farther than a load spans between
        // two lines of the pattern, see hanging_parts(): a wall touching a line at a crossing and running on past the
        // outermost one in the air.
        const double max_span = scale_(support_params.bottom_interface_spacing);
        const auto unsupported = [&]() {
            Polygons result;
            const Polygons footing = offset(printed, float(scaled<double>(0.01)));
            for (const ExPolygon &part : loads)
                if (intersection(to_polygons(part), footing).empty())
                    polygons_append(result, to_polygons(part));
            std::map<coord_t, Polygons> reach_by_width;
            for (const Load &load : plane.loads) {
                auto [it, inserted] = reach_by_width.try_emplace(coord_t(load.width));
                if (inserted)
                    it->second = offset(footing, float(0.5 * load.width), jtRound, SCALED_EPSILON);
                for (const Polyline &run : hanging_parts(load.axis, load.width, it->second, max_span))
                    polygons_append(result, organic_bead(run, load.width));
            }
            return union_(result);
        };
        Polygons unheld = unsupported();
        if (! unheld.empty())
            for (const SupportGeneratorLayer *interface : plane.interfaces) {
                const Flow flow = interface->bridging ?
                    Flow::bridging_flow(interface->height, support_params.support_material_bottom_interface_flow.nozzle_diameter()) :
                    support_params.support_material_interface_flow.with_height(float(interface->height));
                // The interface polygons have already been clipped by their material's exclusions, bed and belt.
                // Keep the whole new bead inside them and outside all material already printed on this layer.
                const Polygons legal = offset(interface->polygons, - 0.5 * flow.scaled_width(), jtRound, SCALED_EPSILON);
                const Polygons reachable = offset(unheld, 1.5 * flow.scaled_width());
                if (std::any_of(plane.loads.begin(), plane.loads.end(),
                        [&](const Load &load) { return load.width + SCALED_EPSILON < flow.scaled_width(); })) {
                    // Plan a group of parallel beads before publishing individual anchors: a wide bead under one
                    // thin stem can otherwise occupy the only space for its neighbour. Accept a group only when
                    // it holds all that hangs: a grid leaving a run of a load hanging stands where an anchor along
                    // the run would go.
                    const BoundingBox box = get_extents(unheld);
                    const Point center = box.center();
                    const coord_t pitch = flow.scaled_width() + 2 * SCALED_EPSILON;
                    const Polygons free = diff(legal, offset(printed, 0.5 * flow.scaled_width(), jtRound, SCALED_EPSILON));
                    for (bool vertical : {false, true}) {
                        const coord_t reach = (vertical ? box.size().y() : box.size().x()) / 2 + 2 * flow.scaled_width();
                        const coord_t across = (vertical ? box.size().x() : box.size().y()) / 2 + 2 * flow.scaled_width();
                        Polylines grid;
                        for (coord_t shift = -(across / pitch) * pitch; shift <= across; shift += pitch) {
                            const Point step = vertical ? Point(shift, 0) : Point(0, shift);
                            const Point run = vertical ? Point(0, reach) : Point(reach, 0);
                            grid.emplace_back(Polyline({center + step - run, center + step + run}));
                        }
                        Polygons material = printed;
                        Polylines anchors;
                        for (Polyline &axis : intersection_pl(intersection_pl(grid, free), reachable))
                            if (axis.length() >= flow.scaled_width() - SCALED_EPSILON) {
                                const Polygons bead = organic_bead(axis, flow.scaled_width());
                                if (intersection(bead, unheld).empty() || ! intersection(bead, material).empty())
                                    continue;
                                append(material, bead);
                                anchors.emplace_back(std::move(axis));
                            }
                        Polygons previous = std::exchange(printed, union_(material));
                        if (! unsupported().empty()) {
                            printed = std::move(previous);
                            continue;
                        }
                        extrusion_entities_append_paths(plane.layer->support_fills.entities, std::move(anchors),
                            ExtrusionRole::erSupportMaterialInterface, flow.mm3_per_mm(), flow.width(), flow.height());
                        unheld.clear();
                        break;
                    }
                    if (unheld.empty())
                        break;
                }
                Polylines axes;
                for (const Load &load : plane.loads) {
                    if (flow.scaled_width() <= load.width + SCALED_EPSILON ||
                        (load.axis.points.size() == 2 && load.axis.length() >= flow.scaled_width()))
                        append(axes, intersection_pl(Polylines{load.axis}, legal));
                    else {
                        // A wider interface nozzle cannot follow a tiny loop without printing over itself.
                        const Point center = get_extents(load.axis).center();
                        const coord_t reach = coord_t(2.5 * flow.scaled_width());
                        for (const Point &direction : {Point(reach, 0), Point(0, reach), Point(reach, reach), Point(reach, -reach)})
                            axes.emplace_back(Polyline({center - direction, center + direction}));
                    }
                }
                const auto add_axes = [&](const Polylines &candidates) {
                    struct Candidate {
                        Polyline axis;
                        size_t contacts;
                    };
                    std::vector<Candidate> ranked;
                    const ExPolygons unsupported_parts = union_ex(unheld);
                    const Polygons occupied = offset(printed, 0.5 * flow.scaled_width(), jtRound, SCALED_EPSILON);
                    for (Polyline &axis : diff_pl(intersection_pl(candidates, legal), occupied)) {
                        if (axis.length() < flow.scaled_width() - SCALED_EPSILON)
                            continue;
                        const Polygons bead = organic_bead(axis, flow.scaled_width());
                        size_t contacts = 0;
                        for (const ExPolygon &part : unsupported_parts)
                            if (! intersection(bead, to_polygons(part)).empty())
                                ++ contacts;
                        if (contacts > 0)
                            ranked.push_back({std::move(axis), contacts});
                    }
                    // A wider interface bead can carry several thin stems. Prefer shared anchors before a local
                    // choice occupies the only legal space for its neighbours.
                    std::stable_sort(ranked.begin(), ranked.end(),
                        [](const Candidate &a, const Candidate &b) { return a.contacts > b.contacts; });
                    for (const Candidate &candidate : ranked) {
                        if (unheld.empty())
                            break;
                        // Leave room to continue past the point of contact to a printable length of one bead width.
                        const Polygons reachable = offset(unheld, 1.5 * flow.scaled_width());
                        Polylines parts = intersection_pl(Polylines{candidate.axis}, reachable);
                        parts = diff_pl(parts, offset(printed, 0.5 * flow.scaled_width(), jtRound, SCALED_EPSILON));
                        for (Polyline &part : parts)
                            if (part.length() >= flow.scaled_width() - SCALED_EPSILON) {
                                const Polygons bead = organic_bead(part, flow.scaled_width());
                                if (intersection(bead, unheld).empty())
                                    continue;
                                polygons_append(printed, bead);
                                printed = union_(printed);
                                extrusion_entities_append_paths(plane.layer->support_fills.entities, Polylines{std::move(part)},
                                    ExtrusionRole::erSupportMaterialInterface, flow.mm3_per_mm(), flow.width(), flow.height());
                                unheld = unsupported();
                            }
                    }
                };
                add_axes(axes);
                if (unheld.empty())
                    break;
                // Pattern lines and earlier anchors can obstruct the projected axis. Find an axis in the remaining
                // legal space, rather than narrowing the bead or overlapping material already printed there.
                axes.clear();
                for (const ExPolygon &free : diff_ex(legal, offset(printed, 0.5 * flow.scaled_width(), jtRound, SCALED_EPSILON)))
                    Geometry::MedialAxis(0., get_extents(free).size().cast<double>().norm(), free).build(&axes);
                // A Voronoi junction can split a printable straight run into several short arms. Try continuations
                // through the closest free axis as well; clipping below still enforces the actual bead clearance.
                Polylines continuations;
                for (const ExPolygon &part : union_ex(unheld)) {
                    const Point center = get_extents(part).center();
                    for (const Polyline &axis : axes) {
                        const Point seed = center.projection_onto(axis);
                        const coord_t reach = coord_t(2.5 * flow.scaled_width());
                        for (const Point &direction : {Point(reach, 0), Point(0, reach), Point(reach, reach), Point(reach, -reach)})
                            continuations.emplace_back(Polyline({seed - direction, seed + direction}));
                    }
                }
                add_axes(continuations);
                add_axes(axes);
            }
        // Carry only real interface paths that touch this load, rather than an unprinted projection of it. A new
        // anchor on the upper bottom-interface layer must itself acquire footing on the next one down.
        for_each_extrusion_path(plane.layer->support_fills, [&](const ExtrusionPath &path) {
            if (path.role() == ExtrusionRole::erSupportMaterialInterface)
                if (auto lower = below(plane.layer->print_z - path.height); lower != planes.end() && lower->first < it->first)
                    for (Polyline &axis : intersection_pl(Polylines{path.as_polyline()}, offset(loaded, 0.5 * scale_(path.width))))
                        lower->second.loads.push_back({std::move(axis), scale_(path.width)});
        });
    }
}

Polylines organic_contact_axes(const ExPolygon &region, coord_t width)
{
    const Polygons core = offset(to_polygons(region), -double(width));
    if (core.empty()) return region.medial_axis(0., 2. * width);
    Polylines medial;
    Geometry::MedialAxis(0., get_extents(region).size().cast<double>().norm(), region).build(&medial);
    return diff_pl(intersection_pl(medial, to_polygons(region)), core);
}

static Polylines organic_interface_medial_paths(const ExPolygon &region, coord_t width)
{
    Polylines medial;
    // Use the unpruned oriented Voronoi paths: the ExPolygon wrapper removes
    // endpoint paths shorter than twice the widest section, losing mixed arms.
    Geometry::MedialAxis(0., 2. * width, region).build(&medial);
    return medial;
}

Polylines organic_interface_gap_paths(const ExPolygon &region, const Polygons &material, coord_t width)
{
    const Polygons legal_axes = offset(to_polygons(region), -0.5 * width,
                                      jtRound, SCALED_EPSILON);
    const Polylines thin = intersection_pl(diff_pl(organic_interface_medial_paths(region, width),
        offset(to_polygons(region), -double(width))), legal_axes);
    // Trim against actual material, retaining positive bead overlap at each
    // attachment. Existing pattern axes and their flow remain untouched.
    return diff_pl(thin, material);
}

void NearPolygons::append(Polygons more)
{
    for (Polygon &polygon : more) {
        boxes.emplace_back(get_extents(polygon));
        polygons.emplace_back(std::move(polygon));
    }
}

Polygons NearPolygons::around(const std::vector<BoundingBox> &near) const
{
    Polygons out;
    for (size_t i = 0; i < polygons.size(); ++ i)
        if (std::any_of(near.begin(), near.end(), [&box = boxes[i]](const BoundingBox &other) { return box.overlap(other); }))
            out.push_back(polygons[i]);
    return out;
}

bool NearPolygons::touch(const Polygons &part) const
{
    // A mitered corner reaches up to DefaultMiterLimit times the offset from the outline.
    const BoundingBox box = get_extents(part).inflated(coord_t(DefaultMiterLimit * grow) + SCALED_EPSILON);
    // Not grown, they only clip the part, as their outlines cut at its box do.
    if (grow <= 0.f)
        return ! intersection(part, organic_outlines_into_box(this->around({ box }), box)).empty();
    // Grow only what of them lies near the part, as the whole of them grows there, see organic_footing_near_box():
    // grown whole for each small part of a large layer, they took most of the time of the checks of the layer.
    const Polygons near = organic_footing_near_box(*this, box, grow, jtMiter);
    return ! near.empty() && ! intersection(part, offset(near, grow)).empty();
}

Polygons organic_bead(const Polyline &axis, double width)
{
    return offset(axis, float(0.5 * width), jtRound, SCALED_EPSILON, etOpenRound);
}

Polylines organic_uncovered_wall_parts(const Polyline &axis, const Polygons &covered_axes, coord_t width)
{
    // Where the wall runs along the edge of the fill, its axis enters and leaves the covered area
    // every few microns. Each piece would be printed as a dash after a travel of its own.
    Polygons short_covered;
    for (const Polyline &covered : intersection_pl(Polylines{ axis }, covered_axes))
        if (covered.length() < width)
            polygons_append(short_covered, offset(covered, float(SCALED_EPSILON), jtRound, SCALED_EPSILON, etOpenRound));
    Polylines parts;
    for (Polyline &part : diff_pl(Polylines{ axis }, short_covered.empty() ? covered_axes : diff(covered_axes, short_covered)))
        // A piece at an end of the axis continues the wall beyond it.
        if (part.length() >= width || at_axis_end(part, axis))
            parts.emplace_back(std::move(part));
    return parts;
}

std::pair<Polylines, Polylines> organic_split_wall_axis(const Polyline &axis, const Polygons &region, coord_t min_length)
{
    // Thin masks around the short stretches between two parts on the other side.
    auto slivers = [&](const Polylines &parts) {
        Polygons result;
        for (const Polyline &part : parts)
            if (part.length() < min_length && !at_axis_end(part, axis))
                polygons_append(result, offset(part, float(SCALED_EPSILON), jtRound, SCALED_EPSILON, etOpenRound));
        return result;
    };
    const Polygons joined = diff(union_(region, slivers(diff_pl(Polylines{ axis }, region))),
                                 slivers(intersection_pl(Polylines{ axis }, region)));
    return { intersection_pl(Polylines{ axis }, joined), diff_pl(Polylines{ axis }, joined) };
}

// The point a length along a polyline, clamped to its ends, and the index of the segment it lies on.
static Point point_at_length(const Points &points, double length, size_t &segment)
{
    for (segment = 0; segment + 1 < points.size(); ++ segment) {
        const Vec2d  step = (points[segment + 1] - points[segment]).cast<double>();
        const double size = step.norm();
        if (length <= size)
            return size > 0. ? points[segment] + (step * (std::max(0., length) / size)).cast<coord_t>() : points[segment];
        length -= size;
    }
    segment = points.size() < 2 ? 0 : points.size() - 2;
    return points.back();
}

Polylines organic_splice_loops_into_walls(Polylines &walls, Polylines loops, const coord_t width)
{
    Polylines apart;
    // Only what comes within a line of the box of a loop can take it. Two units more for the rounding of the projections.
    const coord_t            reach = width + coord_t(SCALED_EPSILON) + 2;
    std::vector<BoundingBox> boxes;
    boxes.reserve(walls.size());
    for (const Polyline &wall : walls)
        boxes.emplace_back(get_extents(wall));
    for (Polyline &loop : loops) {
        // The vertex of the loop and the point of a wall closest to each other, no farther apart than a line.
        double best = double(width) + SCALED_EPSILON;
        size_t best_wall = walls.size(), best_segment = 0, best_vertex = 0;
        Point  best_point;
        BoundingBox around = get_extents(loop);
        around.offset(reach);
        for (size_t i = 0; i < walls.size(); ++ i) {
            if (! boxes[i].overlap(around))
                continue;
            for (size_t s = 0; s + 1 < walls[i].size(); ++ s) {
                const Line segment(walls[i].points[s], walls[i].points[s + 1]);
                if (std::max(segment.a.x(), segment.b.x()) < around.min.x() || std::min(segment.a.x(), segment.b.x()) > around.max.x() ||
                    std::max(segment.a.y(), segment.b.y()) < around.min.y() || std::min(segment.a.y(), segment.b.y()) > around.max.y())
                    continue;
                for (size_t j = 0; j < loop.size(); ++ j) {
                    const Point  projection = loop.points[j].projection_onto(segment);
                    const double distance   = (projection - loop.points[j]).cast<double>().norm();
                    if (distance < best) {
                        best = distance;
                        best_wall = i, best_segment = s, best_vertex = j;
                        best_point = projection;
                    }
                }
            }
        }
        if (best_wall == walls.size()) {
            apart.emplace_back(std::move(loop));
            continue;
        }
        // The wall leaves itself half a line before the point closest to the loop and comes back half a line after it, so
        // that the runs across, a line apart, cover the stretch of the wall between them, and goes round the loop the
        // long way between its points half a line on either side of the vertex closest to the wall: the path does not
        // cross itself or pass over what it has just printed. A loop shorter than two lines is a bead across.
        const Points &wall      = walls[best_wall].points;
        const double  half      = 0.5 * double(width);
        double        to_closest = (best_point - wall[best_segment]).cast<double>().norm();
        for (size_t s = 0; s < best_segment; ++ s)
            to_closest += (wall[s + 1] - wall[s]).cast<double>().norm();
        size_t      leave_segment, back_segment;
        const Point leave = point_at_length(wall, to_closest - half, leave_segment);
        const Point back  = point_at_length(wall, to_closest + half, back_segment);
        // The loop closed, from the vertex closest to the wall round to it.
        Points ring(loop.points.begin() + best_vertex, loop.points.end());
        ring.insert(ring.end(), loop.points.begin(), loop.points.begin() + best_vertex + 1);
        const double ring_length = Polyline(ring).length();
        Points       round;
        if (ring_length > 2. * double(width)) {
            size_t      from_segment, to_segment;
            const Point from = point_at_length(ring, half, from_segment);
            const Point to   = point_at_length(ring, ring_length - half, to_segment);
            round.emplace_back(from);
            round.insert(round.end(), ring.begin() + from_segment + 1, ring.begin() + to_segment + 1);
            round.emplace_back(to);
            // Entered from its end nearer the point the wall leaves at, so that the runs across do not cross.
            auto distance = [](const Point &a, const Point &b) { return (a - b).cast<double>().norm(); };
            if (distance(leave, from) + distance(to, back) > distance(leave, to) + distance(from, back))
                std::reverse(round.begin(), round.end());
        } else
            round.emplace_back(loop.points[best_vertex]);
        Polyline spliced;
        spliced.points.assign(wall.begin(), wall.begin() + leave_segment + 1);
        spliced.points.emplace_back(leave);
        append(spliced.points, std::move(round));
        spliced.points.emplace_back(back);
        spliced.points.insert(spliced.points.end(), wall.begin() + back_segment + 1, wall.end());
        walls[best_wall] = std::move(spliced);
        boxes[best_wall] = get_extents(walls[best_wall]);
    }
    return apart;
}

void organic_mark_double_walls(const SupportGeneratorLayersPtr &base_layers, const SupportParameters &support_params)
{
    const coord_t width = support_params.support_material_flow.scaled_width();
    // The beads of the second walls of the layer below.
    Polygons                     second_below;
    const SupportGeneratorLayer *below = nullptr;
    for (SupportGeneratorLayer *layer : base_layers) {
        layer->double_walls.clear();
        const bool on_below = below != nullptr && std::abs(below->print_z - layer->bottom_z) < EPSILON;
        // The first layer prints all of a body, so a second wall stands on the bed and on the first layer.
        const bool on_solid = layer->bottom_z < EPSILON || (on_below && below->bottom_z < EPSILON);
        Polygons   seconds;
        // The regions whose outlines tree_supports_generate_paths() lays the first walls along, half a line inside the bodies.
        for (const ExPolygon &region : closing_ex(layer->polygons, float(SCALED_EPSILON), float(SCALED_EPSILON + 0.5 * width))) {
            // A second wall a line inside the first starts on a body large enough where it stands on the bed or on the
            // first layer, and goes on wherever its axis lies on a second wall below but for less than a line, however
            // small the body gets, so as not to come and go with its area. One starting higher up would stand over the
            // inside of a body of one wall and print in the air.
            const Polygons second = offset(region, - float(width));
            if (second.empty())
                continue;
            if (on_solid ? region.area() > support_params.tree_branch_diameter_double_wall_area_scaled :
                           on_below && total_length(diff_pl(to_polylines(second), second_below)) < double(width)) {
                append(layer->double_walls, to_polygons(region));
                append(seconds, diff(offset(second, 0.5f * float(width)), offset(second, -0.5f * float(width))));
            }
        }
        second_below = std::move(seconds);
        below        = layer;
    }
}

static void collect_open_ends(const ExtrusionEntityCollection &collection, Points &ends)
{
    for (const ExtrusionEntity *entity : collection.entities)
        if (entity->is_collection())
            collect_open_ends(*static_cast<const ExtrusionEntityCollection*>(entity), ends);
        else if (!entity->is_loop()) {
            ends.emplace_back(entity->first_point());
            ends.emplace_back(entity->last_point());
        }
}

// Drop the extrusions of a collection that drop() picks, and the collections left empty. Return how many are dropped.
template<typename Drop> static size_t remove_extrusions_if(ExtrusionEntityCollection &collection, const Drop &drop)
{
    size_t                removed  = 0;
    ExtrusionEntitiesPtr &entities = collection.entities;
    // Delete only once the collection holds the extrusions that stay: if drop() throws, it is left whole.
    ExtrusionEntitiesPtr kept, dropped;
    kept.reserve(entities.size());
    dropped.reserve(entities.size());
    for (ExtrusionEntity *entity : entities) {
        bool keep = true;
        if (entity->is_collection()) {
            auto *nested = static_cast<ExtrusionEntityCollection*>(entity);
            removed += remove_extrusions_if(*nested, drop);
            keep = ! nested->empty();
        } else if (drop(*entity)) {
            ++ removed;
            keep = false;
        }
        (keep ? kept : dropped).push_back(entity);
    }
    entities.swap(kept);
    for (ExtrusionEntity *entity : dropped)
        delete entity;
    return removed;
}

// Whether pick() picks any of the extrusions of a collection, asked in the order remove_extrusions_if() asks drop().
template<typename Pick> static bool any_extrusion_of(const ExtrusionEntityCollection &collection, const Pick &pick)
{
    return std::any_of(collection.entities.begin(), collection.entities.end(), [&pick](const ExtrusionEntity *entity) {
        if (entity->is_collection())
            return any_extrusion_of(*static_cast<const ExtrusionEntityCollection*>(entity), pick);
        return bool(pick(*entity));
    });
}

// Removes the paths shorter than a quarter of their bead width, and of the line width, except those joining two other
// paths. Such a path remains where a path is clipped along the edge of a bead and adds next to nothing to the layer, but
// unless the G-code happens to continue another path with it, it is printed as a dot after a travel, often a retraction,
// of its own. A piece of a bead wider than a line, as along a neck, is no dot for being wide.
static void remove_dots(ExtrusionEntityCollection &collection, const Points &sorted_ends, const coord_t width)
{
    remove_extrusions_if(collection, [&sorted_ends, width](const ExtrusionEntity &entity) {
        const auto *path = dynamic_cast<const ExtrusionPath*>(&entity);
        if (path == nullptr || path->length() >= 0.25 * std::min(scale_(path->width), double(width)))
            return false;
        const Point first = path->first_point(), last = path->last_point();
        auto meets_other = [&](const Point &end) {
            const auto [begin, end_range] = std::equal_range(sorted_ends.begin(), sorted_ends.end(), end);
            return end_range - begin > (first == end) + (last == end);
        };
        return !meets_other(first) || !meets_other(last);
    });
}

void organic_remove_dots(ExtrusionEntityCollection &collection, const coord_t width)
{
    Points ends;
    collect_open_ends(collection, ends);
    std::sort(ends.begin(), ends.end());
    remove_dots(collection, ends, width);
}

bool support_role_has_no_flow(const PrintObjectConfig &config, const ExtrusionRole role)
{
    return config.set_other_flow_ratios &&
        ((role == erSupportMaterial && config.support_flow_ratio == 0.) ||
         (role == erSupportMaterialInterface && config.support_interface_flow_ratio == 0.));
}

// Use the same finite-width beads for anchoring and for removing floating support, including their round ends.
static Polygons support_material(const ExtrusionEntity &fills, const PrintObject *object = nullptr, const bool first_layer = false)
{
    Polygons covered;
    for_each_extrusion_path(fills, [&](const ExtrusionPath &path) {
        if (object != nullptr) {
            if (path.mm3_per_mm <= 0.)
                return;
            const PrintObjectConfig &config = object->config();
            if (support_role_has_no_flow(config, path.role()) ||
                (config.set_other_flow_ratios && first_layer && object->print()->default_region_config().first_layer_flow_ratio == 0.))
                return;
        }
        append(covered, organic_bead(path.as_polyline(), scale_(path.width)));
    });
    return union_(covered);
}

// The area the extrusions of each support layer of an object cover with their width.
static std::vector<Polygons> support_covered(const PrintObject &object)
{
    const auto            layers = object.support_layers();
    std::vector<Polygons> covered(layers.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()), [&object, &layers, &covered](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++ i)
            covered[i] = support_material(layers[i]->support_fills, &object,
                layers[i]->id() == 0 && std::abs(layers[i]->bottom_z()) < EPSILON);
    });
    return covered;
}

// Stops a long pass once the slicing is cancelled: one over all the layers at thin layers, or over the lone feet and
// islands of a first layer of thousands of separate supports.
static void throw_if_canceled(const PrintObject &object)
{
    if (object.print()->canceled())
        throw CanceledException();
}

struct ModelFootprint {
    // Built once, then only read, by the layers checked in parallel too, see remove_floating_support_crumbs().
    std::once_flag planar_built;
    Polygons planar;
    std::vector<const ExtrusionPath*> contoured;
    std::vector<std::pair<const ExtrusionPath*, double>> resolved;
    double min_z = 0.;
    double max_z = 0.;
};
struct ModelFootprints {
    std::map<const Layer*, ModelFootprint> layers;
    const std::map<const Layer*, int> *nozzle_layers = nullptr;
    const std::vector<Polygons> *emitted = nullptr;
    // The copy of the object whose tops the footprints resolved by the tool plan hold, see model_material().
    size_t copy = 0;
    // Without a tool plan, the index of all the layers is built at once, see model_material(), and then the map is
    // only read, by the layers checked in parallel too, see remove_floating_support_crumbs(). With one, the layers
    // are indexed as they are asked for, by one thread.
    std::once_flag indexed;
};

// Nozzle variants are assigned after support generation. With an unresolved assignment, keep a top path
// if any possible variant delivers material; a zero on an unrelated nozzle must not hide printed footing.
static bool model_top_has_flow(const PrintRegionConfig &config, const Print &print)
{
    const auto &ratios = config.top_solid_infill_flow_ratio;
    if (std::none_of(ratios.values.begin(), ratios.values.end(), [](double ratio) { return ratio == 0.; }))
        return true;
    if (std::all_of(ratios.values.begin(), ratios.values.end(), [](double ratio) { return ratio == 0.; }))
        return false;
    if (print.has_custom_tool_overrides() || (print.is_BBL_printer() && print.get_filament_map_mode() < fmmManual))
        return true;
    const PrintConfig &printer = print.config();
    const bool single_nozzle = print.get_filament_map_mode() != fmmNozzleManual &&
        std::none_of(printer.extruder_max_nozzle_count.values.begin(), printer.extruder_max_nozzle_count.values.end(),
            [](int count) { return count > 1; });
    const auto has_flow = [&](int filament_id) {
        const size_t filament = size_t(filament_id - 1);
        // A single-nozzle grouping uses the installed machine variant, even if a stale filament volume map
        // requests another interchangeable nozzle. Match ToolOrdering's fixed-nozzle path.
        if (single_nozzle) {
            const size_t nozzle = print.get_extruder_id(filament);
            const auto volume = nozzle < printer.nozzle_volume_type.size() ? NozzleVolumeType(printer.nozzle_volume_type.values[nozzle]) : nvtStandard;
            const auto type = ExtruderType(printer.extruder_type.get_at(nozzle));
            const int slot = find_variant_index(get_extruder_variant_string(type, volume), int(nozzle + 1),
                config.print_extruder_variant.values, config.print_extruder_id.values);
            return slot < 0 || ratios.get_at(size_t(slot)) != 0.;
        }
        // apply() resolves a fixed multi-nozzle/volume assignment to its process column.
        const auto &slots = printer.filament_map_2.values;
        return filament >= slots.size() || slots[filament] < 0 || ratios.get_at(size_t(slots[filament])) != 0.;
    };
    // Tool ordering selects a mixed slot's physical components later. Keep possible material from any
    // component rather than mistaking the virtual slot's nominal nozzle for the one that prints it.
    const auto filaments = expand_mixed_filament_ids({config.top_surface_filament_id.value}, printer);
    return filaments.empty() || std::any_of(filaments.begin(), filaments.end(), has_flow);
}

// Export applies these role multipliers after toolpath generation. A zero-flow move deposits no model
// footing, while positive calibration factors retain the nominal bead width used by the slicer.
static bool model_path_has_flow(const ExtrusionPath &path, const PrintRegionConfig &config,
    const PrintObjectConfig &object_config, const bool first_layer, const bool top_has_flow)
{
    if (path.mm3_per_mm <= 0.)
        return false;
    if (path.role() == erTopSolidInfill && !top_has_flow)
        return false;
    if (path.role() == erBottomSurface && config.bottom_solid_infill_flow_ratio == 0.)
        return false;
    if (path.role() == erInternalBridgeInfill && config.internal_bridge_flow == 0.)
        return false;
    if (!object_config.set_other_flow_ratios)
        return true;
    if (first_layer && config.first_layer_flow_ratio == 0.)
        return false;
    switch (path.role()) {
    case erExternalPerimeter: return config.outer_wall_flow_ratio != 0.;
    case erPerimeter: return config.inner_wall_flow_ratio != 0.;
    case erOverhangPerimeter: return config.overhang_flow_ratio != 0.;
    case erInternalInfill: return config.sparse_infill_flow_ratio != 0.;
    case erSolidInfill: return config.internal_solid_infill_flow_ratio != 0.;
    case erGapFill: return config.gap_fill_flow_ratio != 0.;
    default: return true;
    }
}

// Clip in XYZ before projecting: a sloped path can enter and leave the root's Z band several times.
static Polygons contoured_material(const ExtrusionPath &path, const double nominal_z, const double min_z, const double max_z)
{
    Polygons material;
    Polyline run;
    const auto flush = [&] {
        if (run.points.size() > 1)
            append(material, organic_bead(run, scale_(path.width)));
        run.points.clear();
    };
    const double low = scale_(min_z - nominal_z), high = scale_(max_z - nominal_z);
    const auto &points = path.polyline.points;
    for (size_t i = 1; i < points.size(); ++i) {
        const Point3 &a = points[i - 1], &b = points[i];
        double from = 0., to = 1.;
        if (a.z() == b.z()) {
            if (a.z() < low || a.z() > high) {
                flush();
                continue;
            }
        } else {
            const double first = (low - a.z()) / double(b.z() - a.z());
            const double last = (high - a.z()) / double(b.z() - a.z());
            from = std::max(0., std::min(first, last));
            to = std::min(1., std::max(first, last));
            if (from > to) {
                flush();
                continue;
            }
        }
        const auto at = [&](double t) { return Point(a.x() + t * double(b.x() - a.x()), a.y() + t * double(b.y() - a.y())); };
        const Point start = at(from), end = at(to);
        if (!run.points.empty() && run.points.back() != start)
            flush();
        if (run.points.empty())
            run.points.push_back(start);
        run.points.push_back(end);
        if (to < 1.)
            flush();
    }
    flush();
    return material;
}

// The heights at which the top surface of a region delivers material, printed with the filament of the given slot.
static std::vector<double> resolved_top_heights(const PrintObject &object, const Layer &layer, const PrintRegion &region,
    const ToolOrdering &ordering, int layer_index, const unsigned int slot)
{
    const Print &print = *object.print();
    const PrintConfig &printer = print.config();
    const PrintRegionConfig &config = region.config();
    const LayerTools &tools = ordering.tools_for_layer(layer.print_z);
    const auto has_flow = [&](unsigned int filament) {
        size_t column = print.get_extruder_id(filament);
        if (const auto groups = print.get_layered_nozzle_group_result()) {
            const auto nozzle = groups->get_nozzle_for_filament(int(filament), layer_index);
            // Match NOZZLE_CONFIG's published grouping and its missing-entry fallback.
            column = nozzle ? size_t(get_config_index_base(nozzle->volume_type,
                ExtruderType(printer.extruder_type.get_at(nozzle->extruder_id)), nozzle->extruder_id + 1,
                config.print_extruder_variant.values, config.print_extruder_id.values)) : 0;
        }
        return config.top_solid_infill_flow_ratio.get_at(column) != 0.;
    };
    const auto *group = tools.mixed_group_by_slot(slot);
    if (group == nullptr) {
        const unsigned int filament = tools.resolve_mixed(slot);
        if (filament == slot && slot < printer.filament_is_mixed.size() && printer.filament_is_mixed.get_at(slot))
            return {};
        return has_flow(filament) ? std::vector<double>{layer.print_z} : std::vector<double>{};
    }

    // Use the same per-object/per-volume sublayer planes as GCode's emission plan. Their order is already fixed.
    const double height = group->layer_height > 0. ? group->layer_height : layer.height;
    std::vector<double> heights = group->sub_heights;
    const LayerTools::MixedSubLayerGroup::ObjectGradient *gradient = nullptr;
    const bool per_volume = group->is_gradient && std::any_of(group->per_volume_gradient.begin(), group->per_volume_gradient.end(),
        [&object](const auto &entry) { return entry.first.obj == &object; });
    if (per_volume && region.gradient_volume_id().valid()) {
        const auto it = group->per_volume_gradient.find({&object, region.gradient_volume_id()});
        if (it == group->per_volume_gradient.end())
            return {};
        gradient = &it->second;
    } else if (group->is_gradient) {
        const auto it = group->per_object_gradient.find(&object);
        if (it != group->per_object_gradient.end())
            gradient = &it->second;
    }
    if (gradient != nullptr) {
        const double t = gradient->total_layers > 0 ? (2. * gradient->current_idx + 1.) / (2. * gradient->total_layers) : .5;
        const double first = gradient->curve.empty() ?
            gradient->gradient_start + (gradient->gradient_end - gradient->gradient_start) * t : sample_gradient_curve(gradient->curve, t);
        if (per_volume && region.gradient_volume_id().valid() && heights.size() == 2 && gradient->current_idx + 1 == gradient->total_layers) {
            const size_t dominant = first >= .5 ? size_t(group->gradient_first_sorted_idx) : size_t(1 - group->gradient_first_sorted_idx);
            if (group->components_0based[dominant] < group->components_0based[1 - dominant])
                return has_flow(group->components_0based[dominant]) ? std::vector<double>{layer.print_z} : std::vector<double>{};
        }
        for (size_t i = 0; i < heights.size(); ++i)
            heights[i] = (int(i) == group->gradient_first_sorted_idx ? first : 1. - first) * height;
    }
    std::vector<double> result;
    double z = layer.print_z - height;
    for (size_t i = 0; i < heights.size(); ++i) {
        z += heights[i];
        if (has_flow(group->components_0based[i]))
            result.push_back(z);
    }
    return result;
}

// The paths of a model layer that deliver material, see model_path_has_flow().
template<typename Visitor>
static void visit_model_paths(const PrintObject &object, const Layer &layer, const Visitor &visitor)
{
    const bool first = layer.id() == 0 && std::abs(layer.bottom_z()) < EPSILON;
    for (const LayerRegion *region : layer.regions()) {
        const PrintRegionConfig &config = region->region().config();
        const bool top_has_flow = model_top_has_flow(config, *object.print());
        const auto visit_printed = [&](const ExtrusionPath &path) {
            if (model_path_has_flow(path, config, object.config(), first, top_has_flow))
                visitor(path);
        };
        for_each_extrusion_path(region->perimeters, visit_printed);
        for_each_extrusion_path(region->fills, visit_printed);
    }
}

// Contoured paths may leave their nominal layer's Z band. Index their actual bounds, but construct planar
// footprints only when a root reaches that layer. An unprinted interior of a model outline provides no footing.
static Polygons model_material(const PrintObject &object, const Layer &layer, const double min_z, const double max_z,
    ModelFootprints &cache, const ToolOrdering *ordering)
{
    if (ordering != nullptr) {
        auto [it, inserted] = cache.layers.try_emplace(&layer);
        ModelFootprint &footprint = it->second;
        if (inserted) {
            throw_if_canceled(object);
            footprint.min_z = footprint.max_z = layer.print_z;
            const LayerTools &tools = ordering->tools_for_layer(layer.print_z);
            const WipingExtrusions &wiping = tools.wiping_extrusions();
            const bool flushed = object.config().flush_into_objects && wiping.is_anything_overridden();
            for (const LayerRegion *region : layer.regions()) {
                int layer_index = int(layer.id());
                if (cache.nozzle_layers != nullptr)
                    if (const auto index = cache.nozzle_layers->find(&layer); index != cache.nozzle_layers->end())
                        layer_index = index->second;
                std::map<unsigned int, std::vector<double>> heights_of;
                const auto heights = [&](const unsigned int slot) -> const std::vector<double>& {
                    auto found = heights_of.find(slot);
                    if (found == heights_of.end())
                        found = heights_of.emplace(slot,
                            resolved_top_heights(object, layer, region->region(), *ordering, layer_index, slot)).first;
                    return found->second;
                };
                unsigned int own = (tools.extruder_override == 0 ?
                    region->region().config().top_surface_filament_id.value : int(tools.extruder_override)) - 1;
                // The plan of a layer can leave out the filaments of an object the prime tower may flush into, which
                // then prints with any filament at hand: one the layer does without prints with its last filament
                // instead, see GCode::process_layer().
                if (const unsigned int resolved = tools.resolve_mixed(own);
                    !tools.extruders.empty() && !tools.has_extruder(resolved) && !tools.is_mixed_slot(resolved))
                    own = tools.extruders.back();
                const std::vector<double> &own_heights = heights(own);
                const auto visit = [&](const ExtrusionPath &path, const std::vector<double> &top_heights) {
                    if (!model_path_has_flow(path, region->region().config(), object.config(),
                            layer.id() == 0 && std::abs(layer.bottom_z()) < EPSILON, true))
                        return;
                    const auto add = [&](double z) {
                        footprint.resolved.emplace_back(&path, z);
                        footprint.min_z = std::min(footprint.min_z, z);
                        footprint.max_z = std::max(footprint.max_z, z);
                        if (path.z_contoured)
                            for (const Point3 &point : path.polyline.points) {
                                footprint.min_z = std::min(footprint.min_z, z + unscale<double>(point.z()));
                                footprint.max_z = std::max(footprint.max_z, z + unscale<double>(point.z()));
                            }
                    };
                    if (path.role() == erTopSolidInfill) {
                        for (double z : top_heights)
                            add(z);
                    } else
                        add(layer.print_z);
                };
                for_each_extrusion_path(region->perimeters,
                    [&](const ExtrusionPath &path) { visit(path, own_heights); });
                // The prime tower prints the parts of a copy of an object it flushes into with the filament it changes
                // to, or with the first or the last filament of the layer, see
                // WipingExtrusions::mark_wiping_extrusions() and ensure_perimeters_infills_order(). Only the tops
                // of the copy the footprints hold ask for it.
                for (const ExtrusionEntity *entity : region->fills.entities) {
                    const std::vector<double> *top_heights = nullptr;
                    for_each_extrusion_path(*entity, [&](const ExtrusionPath &path) {
                        if (top_heights == nullptr && path.role() == erTopSolidInfill) {
                            const int filament = flushed ? wiping.extruder_override(entity, &object, cache.copy) : -1;
                            top_heights = &heights(filament < 0 ? own : unsigned(filament));
                        }
                        visit(path, top_heights != nullptr ? *top_heights : own_heights);
                    });
                }
            }
        }
        Polygons material;
        if (footprint.max_z >= min_z && footprint.min_z <= max_z)
            for (const auto &[path, z] : footprint.resolved) {
                if (path->z_contoured)
                    append(material, contoured_material(*path, z, min_z, max_z));
                else if (z >= min_z && z <= max_z)
                    append(material, organic_bead(path->as_polyline(), scale_(path->width)));
            }
        return material;
    }
    std::call_once(cache.indexed, [&object, &cache] {
        for (const Layer *model_layer : object.layers()) {
            ModelFootprint &entry = cache.layers[model_layer];
            entry.min_z = entry.max_z = model_layer->print_z;
            visit_model_paths(object, *model_layer, [&](const ExtrusionPath &path) {
                if (path.z_contoured) {
                    entry.contoured.push_back(&path);
                    for (const Point3 &point : path.polyline.points) {
                        const double z = model_layer->print_z + unscale<double>(point.z());
                        entry.min_z = std::min(entry.min_z, z);
                        entry.max_z = std::max(entry.max_z, z);
                    }
                }
            });
        }
    });
    const auto found = cache.layers.find(&layer);
    if (found == cache.layers.end())
        return {};
    ModelFootprint &footprint = found->second;
    if (footprint.max_z < min_z || footprint.min_z > max_z)
        return {};
    Polygons material;
    if (layer.print_z >= min_z && layer.print_z <= max_z) {
        // Built once, the first time it is asked for, while other layers build theirs.
        std::call_once(footprint.planar_built, [&] {
            Polygons planar;
            visit_model_paths(object, layer, [&](const ExtrusionPath &path) {
                if (!path.z_contoured)
                    append(planar, organic_bead(path.as_polyline(), scale_(path.width)));
            });
            footprint.planar = union_(planar);
        });
        material = footprint.planar;
    }
    for (const ExtrusionPath *path : footprint.contoured)
        append(material, contoured_material(*path, layer.print_z, min_z, max_z));
    return material;
}

// Material on the support layers immediately below layer i.
static Polygons support_below(const PrintObject &object, const std::vector<Polygons> &covered, const size_t i)
{
    const auto     layers = object.support_layers();
    const coordf_t bottom = layers[i]->print_z - layers[i]->height;
    Polygons       below;
    for (size_t j = i; j -- > 0 && layers[j]->print_z > bottom - EPSILON;)
        if (layers[j]->print_z < bottom + EPSILON)
            append(below, covered[j]);
    return below;
}

// How far material reaches beyond its outline for a bead to stand on it: beads touching by their round ends stand on
// each other.
static const float footing_reach = float(scaled<double>(0.01));

// The material support_below() of layer i grown by footing_reach. Model footing, when permitted, is added by
// floating_islands() and LayerFooting.
static Polygons support_footing(const PrintObject &object, const std::vector<Polygons> &covered, const size_t i)
{
    return offset(support_below(object, covered, i), footing_reach);
}

// Where the axis of a bead width wide stands on footing: within half its width of it. trim_unsupported_base() joins the
// ends it cuts along the edge of that reach grown by SCALED_EPSILON, which holds them, and the reach takes such a join in
// with a margin of as much again: lying on its edge, rounding would leave the join over nothing.
static float bead_reach_offset(const double width)
{
    return float(.5 * width + 2. * SCALED_EPSILON);
}
static Polygons bead_reach(const Polygons &footing, const double width)
{
    return offset(footing, bead_reach_offset(width), jtRound, SCALED_EPSILON);
}

// Where the axes of a layer leave what holds up the narrowest of them, a wider bead reaching only farther. Clipping all the
// axes of a layer at once takes about as long as clipping one against the whole layer and most layers leave nothing outside,
// so the test of a run, which needs the reach of its own width, is left to the axes near such a piece.
class AxesOutsideReach {
public:
    void add(Polyline axis, const coord_t width)
    {
        if (axis.points.size() < 2)
            return;
        m_narrowest = std::min(m_narrowest, width);
        m_axes.emplace_back(std::move(axis));
    }
    // reach(width) is where the axis of a bead width wide stands, see bead_reach().
    template<typename Reach> void clip(const Reach &reach)
    {
        if (! m_axes.empty())
            for (const Polyline &piece : diff_pl(m_axes, reach(m_narrowest)))
                m_boxes.emplace_back(get_extents(piece).inflated(SCALED_EPSILON));
    }
    bool near(const Polyline &axis) const
    {
        const BoundingBox box = get_extents(axis);
        return std::any_of(m_boxes.begin(), m_boxes.end(), [&box](const BoundingBox &other) { return box.overlap(other); });
    }

private:
    Polylines                m_axes;
    std::vector<BoundingBox> m_boxes;
    coord_t                  m_narrowest = std::numeric_limits<coord_t>::max();
};

// How far below a support layer the model it stands on may lie: the bottom Z distance as the trees round it to whole
// layers, see TreeSupportSettings::z_distance_bottom_layers, farther than the distance itself with an independent support
// layer height, but at least 0.1 mm, which a bead laid over a surface sags across.
static double model_footing_gap(const PrintObject &object)
{
    const double gap    = object.slicing_parameters().gap_object_support;
    const double layer  = object.config().layer_height.value;
    const int    layers = TreeSupport3D::z_distance_in_layers(scaled<coord_t>(gap), scaled<coord_t>(layer));
    return std::max({ gap, layers * layer, 0.1 });
}

// The model material within model_footing_gap() below support layer i, which the support may stand on unless it is on the
// build plate only.
static Polygons model_footing(const PrintObject &object, const size_t i, ModelFootprints &model, const ToolOrdering *ordering)
{
    if (model.emitted != nullptr)
        return union_((*model.emitted)[i]);
    const SupportLayer &layer  = *object.support_layers()[i];
    const double        bottom = layer.print_z - layer.height;
    const double        gap    = model_footing_gap(object);
    Polygons            material;
    for (const Layer *model_layer : object.layers())
        append(material, model_material(object, *model_layer, bottom - gap - EPSILON, bottom + EPSILON, model, ordering));
    return union_(material);
}

// The footing of support layer i, see support_footing(), alone or with the model footing within reach below added, and
// where a bead of each width stands on it, see bead_reach(). What lies below a layer does not change while the layer is
// cleaned, so its checks share them, each found once and only once asked for: grown again for each check, the footing
// of the whole layer took most of their time.
class LayerFooting {
public:
    LayerFooting(const PrintObject &object, const std::vector<Polygons> &covered, const size_t i,
        ModelFootprints &model, const ToolOrdering *ordering) :
        m_object(object), m_covered(covered), m_i(i), m_model_prints(model), m_ordering(ordering)
    {}
    const Polygons &polygons(const bool with_model) { return footing(with_model).polygons; }
    // The polygons of the footing with their boxes.
    const NearPolygons &near_polygons(const bool with_model)
    {
        Footing &f = footing(with_model);
        if (f.near.polygons.empty())
            f.near.append(f.polygons);
        return f.near;
    }
    const Polygons &reach(const bool with_model, const coord_t width)
    {
        Footing &f = footing(with_model);
        auto [it, inserted] = f.reach.try_emplace(width);
        if (inserted)
            it->second = bead_reach(f.polygons, double(width));
        return it->second;
    }
    // The polygons of the footing that may share area with what lies within box: those whose boxes overlap it, with the
    // contour of each hole among them.
    Polygons near(const bool with_model, const BoundingBox &box) { return near_polygons(with_model).around({ box }); }

private:
    struct Footing {
        Polygons                    polygons;
        std::map<coord_t, Polygons> reach;
        NearPolygons                near;
    };
    Footing &footing(const bool with_model)
    {
        if (! m_support) {
            m_support.emplace();
            m_support->polygons = support_footing(m_object, m_covered, m_i);
        }
        if (! with_model)
            return *m_support;
        if (! m_with_model) {
            m_with_model.emplace();
            m_with_model->polygons = m_support->polygons;
            append(m_with_model->polygons,
                offset(model_footing(m_object, m_i, m_model_prints, m_ordering), footing_reach));
        }
        return *m_with_model;
    }

    const PrintObject           &m_object;
    const std::vector<Polygons> &m_covered;
    const size_t                 m_i;
    ModelFootprints             &m_model_prints;
    const ToolOrdering          *m_ordering;
    std::optional<Footing>       m_support;
    std::optional<Footing>       m_with_model;
};

// The islands standing on neither support below nor permitted model material within model_footing_gap(). The first
// layer stands on the bed.
static ExPolygons floating_islands(const PrintObject &object, const std::vector<Polygons> &covered, const size_t i,
    ModelFootprints &model, const ToolOrdering *ordering = nullptr, const double min_area = 0.)
{
    const SupportLayer &layer = *object.support_layers()[i];
    if (layer.print_z - layer.height < EPSILON)
        return {};
    // Grown by footing_reach only near each island it is checked against.
    NearPolygons footing;
    footing.grow = footing_reach;
    footing.append(support_below(object, covered, i));
    bool model_checked = object.config().support_on_build_plate_only;
    const auto add_model_footing = [&] {
        if (model_checked)
            return;
        footing.append(model_footing(object, i, model, ordering));
        model_checked = true;
    };
    // The islands are checked each on its own, in parallel: a layer of many bodies holds many of them.
    ExPolygons        islands = union_ex(covered[i]);
    std::vector<char> rooted(islands.size());
    const auto        check   = [&](const size_t from) {
        using Range = tbb::blocked_range<size_t>;
        tbb::parallel_for(Range(from, islands.size()), [&](const Range &range) {
            for (size_t k = range.begin(); k < range.end(); ++ k)
                rooted[k] = footing.touch(to_polygons(islands[k]));
        });
    };
    check(0);
    ExPolygons floating;
    for (size_t k = 0; k < islands.size(); ++ k) {
        // Most islands stand on support. Avoid constructing model footprints until one actually needs them: the islands
        // from this one on are checked again with them.
        if (! rooted[k] && ! model_checked) {
            add_model_footing();
            check(k);
        }
        if (! rooted[k])
            floating.emplace_back(std::move(islands[k]));
    }
    // The island area threshold suppresses specks. Beads hanging from a rooted island have their own bead-scale test, see
    // hanging_runs(); a short, sharply bent run can fail even at a small area.
    floating.erase(std::remove_if(floating.begin(), floating.end(),
        [min_area](const ExPolygon &island) { return island.area() < min_area; }), floating.end());
    return floating;
}

// Whether an object prints Organic support with a hollow base: a tree support type in any style but Slim, Strong and
// Hybrid, as SupportParameters resolves Default, Snug and Grid with it.
static bool hollow_organic(const PrintObjectConfig &config)
{
    return is_tree(config.support_type) && config.support_style != smsTreeSlim && config.support_style != smsTreeStrong &&
        config.support_style != smsTreeHybrid && (config.support_base_pattern == smpDefault || config.support_base_pattern == smpNone);
}

// The gap of a hollow base. A hollow base lays no lines of the base pattern, so their spacing means nothing to it and is
// not read; the gap is set by the support line and the layer height instead. The gap S is how wide a gap a line of the
// lowest roof layer spans between the bodies and the inner trees under it, see generate_support_areas(): every point
// of a roof is to lie within half of S plus half a line of what holds it, the inner trees stand sqrt(3) * (tip radius +
// that reach) apart, and free_span() lets any bead of the base, walls and trees as well as roofs, bridge as far. One value
// for the whole object keeps the trees, the trimming of the base and the warning agreeing on what holds.
//
// The trade-off: a narrower gap holds a roof at more points, a wider one grows fewer and larger trees. Below about three
// lines the gain at the roof does not show on prints while the cost does: bodies full of tiny trees, loops hardly wider
// than a line, with more travels, retractions and stringing, a support harder to remove and slower to generate. So the
// gap grows with the line, as everything of the trees does, and with the layer:
//
//     S = clamp(7 * layer height, line, 3 * line)
//
// - Three lines at most. With layers about half a line high, 0.1 mm under 0.22 mm lines and 0.2 mm under 0.42 mm lines,
//   prints left the underside of a roof over a gap of three lines as clean as over a gap of one, and the support came
//   off more easily. Wider gaps printed no better, while they leave the lines of a roof hanging over longer stretches.
// - One line at least, at fine layers. A line 0.02 mm high bridges next to nothing: under 0.22 mm lines the lowest roof
//   layer tore over the gaps between the trees with a gap of one line and more with three. No gap makes such layers
//   bridge, only thicker support layers would, but the narrowest gap the trees keep, one line, as a tree narrower than
//   that prints no loop (see prints_loop in organic_grow_trees_inside_hollow_bodies()), tears least.
// - In between, the gap follows the layer height, a thicker line bridging farther. Seven layer heights reach three lines
//   at layers 0.43 lines high, about where the three-line prints were made, and leave one line at layers a seventh of a
//   line high and finer, 0.03 mm under 0.22 mm lines. The factor is fitted to these two points, not derived: prints at
//   the heights in between, 0.06 mm under 0.22 mm lines, are yet to calibrate it.
//
// The layer height of the object is taken, the gap being one for the whole object: Organic supports print their layers
// above the first layer of the object that high, see layer_z(). They refuse a variable layer height (see
// Print::validate()), but a style of normal supports left with a tree support type, which SupportParameters resolves to
// Organic, escapes that check: its support still prints layers of that height beside the thinner or thicker layers of
// the object, and bridges as far. Nor does the precise Z height, which stretches or squeezes the top layers of the
// object to meet its height, change the layers of the support.
coord_t organic_base_span(const PrintObject &object)
{
    const SupportMaterialPattern base = object.config().support_base_pattern.value;
    if (base != smpDefault && base != smpNone)
        return scaled<coord_t>(object.config().support_base_pattern_spacing.value);
    const coord_t line = support_material_flow(&object).scaled_width();
    return std::clamp(coord_t(std::llround(scaled<double>(7. * object.config().layer_height.value))), line, 3 * line);
}

// The longest bridge between two bearings the support allows: a line across the gap of the base, see organic_base_span(),
// the gap a roof spans between the bodies it lies on, see generate_support_areas(), or between two lines of a bottom
// interface, which the bodies standing on it span. It is the length of the run of a bead out of reach of its bearings, see
// bead_reach(), so the gap in the material the bead crosses may be a width wider.
static double free_span(const PrintObjectConfig &config, const coord_t base_span, const double width)
{
    const int bottom_layers = config.support_interface_bottom_layers.value < 0 ? config.support_interface_top_layers.value :
        config.support_interface_bottom_layers.value;
    return std::max(double(base_span), bottom_layers > 0 ? scale_(config.support_bottom_interface_spacing.value) : 0.) + width;
}

// Whether a support path is printed flat and with some flow, as what holds up the support above.
static bool flat_support(const PrintObjectConfig &config, const ExtrusionPath &path)
{
    return is_support(path.role()) && ! path.z_contoured && path.mm3_per_mm > 0. && ! support_role_has_no_flow(config, path.role());
}

// Calls visitor with each bead of a collection, which a multi-path or a loop prints in several paths.
template<typename Visitor> static void for_each_bead(const ExtrusionEntityCollection &collection, Visitor &&visitor)
{
    for (const ExtrusionEntity *entity : collection.entities)
        if (entity->is_collection())
            for_each_bead(*static_cast<const ExtrusionEntityCollection*>(entity), visitor);
        else
            visitor(*entity);
}

// Whether an outline reaches into box: an edge of it meets the box, or it lies around the box.
static bool reaches_into(const Polygon &outline, const BoundingBox &box)
{
    for (size_t i = 0; i < outline.size(); ++ i)
        if (Line edge(outline.points[i], outline.points[(i + 1) % outline.size()]); edge.clip_with_bbox(box))
            return true;
    return outline.contains(box.center());
}

// An outline that does not reach into the box lies all out of it. Cut, one that only passes by a corner of the box
// keeps all its points, and clipping against many of them takes a time quadratic in them.
Polygons organic_outlines_into_box(const Polygons &outlines, const BoundingBox &box)
{
    Polygons out;
    for (const Polygon &outline : outlines)
        if (Polygon cut = ClipperUtils::clip_clipper_polygon_with_subject_bbox(outline, box); reaches_into(cut, box))
            out.push_back(std::move(cut));
    return out;
}

// The sides of box a point lies beyond, a bit each, as ClipperUtils::clip_clipper_polygon_with_subject_bbox() takes
// them.
static int sides_beyond(const BoundingBox &box, const Point &point)
{
    return int(point.x() < box.min.x()) | int(point.x() > box.max.x()) << 1 | int(point.y() < box.min.y()) << 2 |
        int(point.y() > box.max.y()) << 3;
}

// Whether the segments ab and cd come within distance of each other: an end of one lies that close to the other, or
// they cross.
static bool segments_within(const Point &a, const Point &b, const Point &c, const Point &d, const double distance)
{
    const double limit = distance * distance;
    if (Line::distance_to_squared(a, c, d) <= limit || Line::distance_to_squared(b, c, d) <= limit ||
        Line::distance_to_squared(c, a, b) <= limit || Line::distance_to_squared(d, a, b) <= limit)
        return true;
    const auto turn = [](const Point &p, const Point &q, const Point &r) {
        return cross2(Vec2d((q - p).cast<double>()), Vec2d((r - p).cast<double>()));
    };
    return turn(a, b, c) * turn(a, b, d) < 0. && turn(c, d, a) * turn(c, d, b) < 0.;
}

// An outline as offset_as_group() takes it: the area, by the sign of which offset() takes it for a contour or a hole,
// and its box.
struct OutlineShape {
    double      area;
    BoundingBox box;
};

// Whether offset() grows the outlines as one group, as c2_offset_as_group() decides it from their paths: only if the
// box of each hole lies strictly inside the box of a contour. Otherwise it grows them one by one and unites them
// again, which rounds otherwise where they cross.
static bool offset_as_group(const std::vector<OutlineShape> &outlines)
{
    std::vector<const BoundingBox*> contours;
    for (const OutlineShape &outline : outlines)
        if (outline.area > 0.)
            contours.push_back(&outline.box);
    return std::all_of(outlines.begin(), outlines.end(), [&contours](const OutlineShape &hole) {
        return hole.area >= 0. || std::any_of(contours.begin(), contours.end(), [&hole](const BoundingBox *contour) {
            return contour->min.x() < hole.box.min.x() && contour->max.x() > hole.box.max.x() &&
                contour->min.y() < hole.box.min.y() && contour->max.y() > hole.box.max.y();
        });
    });
}

// offset() grows each piece of an outline, an edge or the join at a point, within reach of it: delta for a round join,
// the miter limit times delta for a mitered one. It unites what it grows and rounds the points where grown pieces
// cross, and what grows into the box farther from its sides than the reach grows from the edges meeting the box,
// between such points or the ends of the edges grown: a bead along the edge of the reach could hang or not by the
// slope of an edge rounded otherwise. A piece grown crosses such an edge grown only if it lies within twice the reach
// of it, which an outline whose box does not overlap the box may too.
//
// The cut drops points out of the box beside neighbours out of it on the same side, see
// ClipperUtils::clip_clipper_polygon_with_subject_bbox(): each point of the box lies inside as many of the cut outlines
// as of the whole ones, and each edge meeting the box stays as it is. It keeps the points of each edge within twice
// the reach of an edge meeting the box too, with the neighbours that set the joins offset() places at them, and the
// points farthest each way, which keep the box of each outline. So of what it changes only the chord replacing a run
// of points dropped may come that near such an edge, and then it keeps the run too; an outline that does not reach
// into the box and has no edge that near is left out. Where offset() would not grow the whole or the cut outlines as
// one group, see offset_as_group(), uniting what it grows of each outline alone first, which rounds where a far piece
// of it crosses a piece near the box, where the cut turns the sign of the area of an outline, by which offset() takes
// it for a contour or a hole, and where offset() would drop a point of a cut outline, the outlines near the box are
// taken whole. offset() drops a point closer than a fraction of the offset to the last one it kept, going around an
// outline from its first point, and which of two such points it keeps sets the slope of an edge: the outlines are cut
// as offset() takes them, and two points the cut leaves as close would turn an edge again.
Polygons organic_footing_near_box(const NearPolygons &footing, const BoundingBox &box, const float delta,
    const JoinType join)
{
    Polygons     whole   = footing.around({ box });
    Polygons     thinned = offset_input(whole, delta);
    const double reach   = (join == jtRound ? 1. : DefaultMiterLimit) * std::abs(double(delta));
    const double apart   = 2. * reach + double(SCALED_EPSILON);
    // The edges meeting the box but those farther inside it than apart, which nothing out of the box comes near.
    const BoundingBox        inner = box.inflated(- (apart + 1.));
    std::vector<Line>                                 edges;
    std::vector<AABBTreeIndirect::BoundingBoxWrapper> spans;
    BoundingBox                                       zone;
    // The sides of the box each point lies beyond, outline after outline.
    std::vector<int>    beyond;
    std::vector<size_t> firsts;
    const auto          add_sides = [&box, &beyond, &firsts](const Polygon &outline) {
        firsts.push_back(beyond.size());
        for (const Point &point : outline.points)
            beyond.push_back(sides_beyond(box, point));
    };
    for (const Polygon &outline : thinned) {
        add_sides(outline);
        const int *sides = beyond.data() + firsts.back();
        for (size_t i = 0, n = outline.size(); i < n; ++ i) {
            const size_t next = i + 1 < n ? i + 1 : 0;
            const Point &a = outline.points[i], &b = outline.points[next];
            if ((sides[i] & sides[next]) != 0 || (inner.contains(a) && inner.contains(b)))
                continue;
            if (Line edge(a, b); edge.clip_with_bbox(box)) {
                BoundingBox span;
                span.merge(a);
                span.merge(b);
                span.offset(std::ceil(apart));
                zone.merge(span);
                // BoundingBoxWrapper grows each box by SCALED_EPSILON: taken off first, so that it takes in no outline
                // farther away.
                spans.emplace_back(edges.size(), span.inflated(- SCALED_EPSILON));
                edges.emplace_back(a, b);
            }
        }
    }
    // The boxes of the edges grown by apart, so that each segment is looked for only among those its box overlaps:
    // many strips across a long box would make the search quadratic in them. Most segments lie out of all of them,
    // and out of the box around them all, which rejects them at once.
    using Tree = AABBTreeIndirect::Tree<2, coord_t>;
    Tree reaches;
    reaches.build_modify_input(spans);
    const auto overlaps = [&reaches, &zone](const Point &low, const Point &high, const auto &visit) {
        if (zone.defined && high.x() >= zone.min.x() && low.x() <= zone.max.x() && high.y() >= zone.min.y() &&
            low.y() <= zone.max.y())
            AABBTreeIndirect::traverse(reaches, AABBTreeIndirect::intersecting(Tree::BoundingBox(low, high)),
                [&visit](const Tree::Node &node) { return ! visit(node.idx); });
    };
    // The outlines out of the box that come that near such an edge.
    for (size_t i = 0; ! reaches.empty() && i < footing.polygons.size(); ++ i)
        if (const BoundingBox &other = footing.boxes[i]; ! other.overlap(box)) {
            bool near = false;
            overlaps(other.min, other.max, [&near](size_t) {
                near = true;
                return true;
            });
            if (! near)
                continue;
            whole.push_back(footing.polygons[i]);
            for (Polygon &outline : offset_input(Polygons{ footing.polygons[i] }, delta)) {
                add_sides(outline);
                thinned.push_back(std::move(outline));
            }
        }
    // Whether the segment ab comes within apart of an edge meeting the box. The edges whose boxes its box overlaps are
    // tried in their order, the first near one ending the search: no more than the edges before it, as tried one by
    // one, whichever order the tree visits them in.
    std::vector<size_t> candidates;
    const auto          near_edges = [&](const Point &a, const Point &b) {
        candidates.clear();
        overlaps(a.cwiseMin(b), a.cwiseMax(b), [&candidates](const size_t i) {
            candidates.push_back(i);
            return false;
        });
        std::sort(candidates.begin(), candidates.end());
        return std::any_of(candidates.begin(), candidates.end(),
            [&](const size_t i) { return segments_within(a, b, edges[i].a, edges[i].b, apart); });
    };
    // A point in the box, and both points of an edge within apart of an edge meeting it, are kept with the neighbours
    // that set the joins offset() places at them: the corner of a miter, and for any join the crossing of both edges
    // grown, where offset() cuts a concave corner if the edges are long enough, see OffsetConcaveCrossing() in
    // Clipper2.
    Polygons                  near;
    std::vector<OutlineShape> whole_shapes, near_shapes;
    std::vector<char>         keep;
    std::vector<size_t>       kept;
    near.reserve(thinned.size());
    for (size_t k = 0; k < thinned.size(); ++ k) {
        const Polygon &outline = thinned[k];
        const Points  &points  = outline.points;
        const size_t   n       = points.size();
        const int     *sides   = beyond.data() + firsts[k];
        const auto    hold   = [&keep, n](const size_t at) {
            keep[(at + n - 1) % n] = keep[at] = keep[(at + 1) % n] = 1;
        };
        keep.assign(n, 0);
        bool        close     = false;
        size_t      extreme[] = { 0, 0, 0, 0 };
        BoundingBox extent(points.front(), points.front());
        for (size_t i = 0; i < n; ++ i) {
            const Point &point = points[i];
            const size_t next  = i + 1 < n ? i + 1 : 0;
            if (point.x() < extent.min.x()) {
                extent.min.x() = point.x();
                extreme[0]     = i;
            } else if (point.x() > extent.max.x()) {
                extent.max.x() = point.x();
                extreme[1]     = i;
            }
            if (point.y() < extent.min.y()) {
                extent.min.y() = point.y();
                extreme[2]     = i;
            } else if (point.y() > extent.max.y()) {
                extent.max.y() = point.y();
                extreme[3]     = i;
            }
            // A point in the box is kept with its neighbours, and an edge from it reaches into the box.
            if (sides[i] == 0)
                hold(i);
            else if (sides[next] != 0 && near_edges(point, points[next])) {
                close = true;
                hold(i);
                hold(next);
            }
        }
        for (const size_t i : extreme)
            keep[i] = 1;
        // The cut as ClipperUtils::clip_clipper_polygon_with_subject_bbox() makes it, keeping those points as well, and
        // the runs of points whose chords come within apart of an edge meeting the box.
        for (bool cut_again = true; cut_again;) {
            kept.clear();
            int last = sides[n - 1];
            for (size_t i = 0; i < n && ! (i + 1 == n && kept.empty()); ++ i)
                if (keep[i] || (last & sides[i] & sides[i + 1 < n ? i + 1 : kept.front()]) == 0) {
                    kept.push_back(i);
                    last = sides[i];
                }
            cut_again = false;
            for (size_t j = 0; j < kept.size(); ++ j)
                if (const size_t from = kept[j], to = kept[(j + 1) % kept.size()];
                    (from + 1) % n != to && near_edges(points[to], points[from])) {
                    for (size_t i = (from + 1) % n; i != to; i = (i + 1) % n)
                        keep[i] = 1;
                    cut_again = true;
                }
        }
        Polygon cut;
        cut.points.reserve(kept.size());
        for (const size_t i : kept)
            cut.points.push_back(points[i]);
        const double area = outline.area();
        whole_shapes.push_back({ area, extent });
        // An outline that does not reach into the box lies all out of it.
        if (! close && ! reaches_into(cut, box))
            continue;
        const double cut_area = cut.area();
        if (cut_area == 0. || (cut_area > 0.) != (area > 0.))
            return whole;
        near_shapes.push_back({ cut_area, get_extents(cut) });
        near.push_back(std::move(cut));
    }
    return ! offset_as_group(whole_shapes) || ! offset_as_group(near_shapes) ||
        count_points(offset_input(near, delta)) < count_points(near) ? whole : near;
}

// The number of runs of the beads of layer i outside its floating islands that hang, see hanging_parts(), over the
// material below: the support below and, where the support may stand on it, the model. Sideways contact joins such a run
// to a rooted XY island but puts nothing below it, so the island check does not see it.
static size_t hanging_runs(const PrintObject &object, const size_t i, const ExPolygons &floating, LayerFooting &footing,
    const coord_t base_span)
{
    const SupportLayer &layer  = *object.support_layers()[i];
    const auto         &config = object.config();
    // The raft prints lines of its own, spaced as it is set to.
    if (layer.print_z - layer.height < EPSILON || i < object.slicing_parameters().raft_layers() || !hollow_organic(config))
        return 0;
    bool       model_added = config.support_on_build_plate_only;
    bool       with_model  = false;
    size_t     runs        = 0;
    const auto reach = [&](const coord_t width) -> const Polygons & { return footing.reach(with_model, width); };
    // The reach near a path of a bead of several widths, grown from the footing within a box around the path only: Arachne
    // prints the paths of such a bead each as wide as it fits, and growing the footing of the whole layer for each width
    // took most of the time of these checks. The box leaves out only footing farther from the path than around, more than
    // the reach extends from the path and the caps of the ends of a seam from it.
    const auto reach_near = [&footing, &with_model](const coord_t width, const Polyline &path, const coord_t around) {
        BoundingBox box = get_extents(path);
        box.offset(double(around) + scaled<double>(.1));
        return bead_reach(organic_footing_near_box(footing.near_polygons(with_model), box,
            bead_reach_offset(double(width)), jtRound), double(width));
    };
    const auto max_span = [&](const double width) { return free_span(config, base_span, width); };
    AxesOutsideReach outside;
    for_each_extrusion_path(layer.support_fills, [&](const ExtrusionPath &path) {
        if (flat_support(config, path))
            outside.add(path.as_polyline(), coord_t(scale_(path.width)));
    });
    outside.clip(reach);
    // The ends of the paths of a bead, split where its width changes, are not ends of the bead: judge it as a whole.
    const auto count_runs = [&](const std::vector<BeadPiece> &pieces) {
        Polyline axis;
        for (const BeadPiece &piece : pieces) {
            const bool joins = ! axis.points.empty() && axis.last_point() == piece.axis.first_point();
            axis.points.insert(axis.points.end(), piece.axis.points.begin() + (joins ? 1 : 0), piece.axis.points.end());
        }
        if (axis.points.size() < 2 || ! outside.near(axis) ||
            std::any_of(floating.begin(), floating.end(),
                [&](const ExPolygon &island) { return island.contains(axis.first_point()); }))
            return;
        const coord_t width   = pieces.front().width;
        const bool    uniform = std::all_of(pieces.begin(), pieces.end(), [width](const BeadPiece &piece) { return piece.width == width; });
        const auto    hanging = [&] {
            return uniform ? hanging_parts(axis, double(width), reach(width), max_span(double(width))).size() :
                varying_bead_hanging_runs(pieces, axis, reach_near, max_span);
        };
        size_t found = hanging();
        // Most beads stand on support. Construct the model footing only once one needs it.
        if (found != 0 && ! model_added) {
            model_added = with_model = true;
            found       = hanging();
        }
        runs += found;
    };
    // A path not printed flat holds nothing up and parts the bead.
    for_each_bead(layer.support_fills, [&](const ExtrusionEntity &bead) {
        std::vector<BeadPiece> pieces;
        for_each_extrusion_path(bead, [&](const ExtrusionPath &path) {
            if (flat_support(config, path))
                pieces.push_back(BeadPiece{ path.as_polyline(), coord_t(scale_(path.width)) });
            else {
                count_runs(pieces);
                pieces.clear();
            }
        });
        count_runs(pieces);
    });
    return runs;
}

// Appends the print Z of layer i for each of its floating islands at least min_area large and each of its hanging runs
// outside all of its floating islands, standing on footing. base_span is organic_base_span() of the object, the same on
// all of its layers.
static void append_floating_heights(std::vector<coordf_t> &heights, const PrintObject &object, const size_t i,
    const ExPolygons &floating, LayerFooting &footing, const coord_t base_span, const double min_area = 0.)
{
    const size_t large = std::count_if(floating.begin(), floating.end(),
        [min_area](const ExPolygon &island) { return island.area() >= min_area; });
    heights.insert(heights.end(), large + hanging_runs(object, i, floating, footing, base_span),
        object.support_layers()[i]->print_z);
}

// On a belt printer the floor slopes, and support ends on it at any height: the checks below, which know a flat bed only,
// would take a part resting on the belt for one standing on nothing.
static bool on_belt(const PrintObject &object)
{
    BeltFloorContext belt;
    return belt.init(object.slicing_parameters(), object.print()->config());
}

double organic_sliver_area(const PrintObject &object)
{
    const double width = SupportParameters(object).support_material_flow.scaled_width();
    return 0.5 * width * width;
}

std::vector<coordf_t> floating_support_islands(const PrintObject &object, const double min_area)
{
    if (on_belt(object))
        return {};
    const std::vector<Polygons> covered = support_covered(object);
    std::vector<coordf_t>       floating;
    ModelFootprints            model;
    const coord_t               base_span = organic_base_span(object);
    for (size_t i = 0; i < covered.size(); ++ i) {
        throw_if_canceled(object);
        LayerFooting footing(object, covered, i, model, nullptr);
        append_floating_heights(floating, object, i, floating_islands(object, covered, i, model), footing, base_span,
            min_area);
    }
    return floating;
}

std::vector<coordf_t> floating_support_islands(const PrintObject &object, const double min_area, const ToolOrdering &ordering,
    const std::map<const Layer*, int> &nozzle_layers)
{
    if (on_belt(object) || ordering.empty())
        return {};
    const std::vector<Polygons> covered = support_covered(object);
    std::vector<coordf_t> floating;
    const coord_t base_span = organic_base_span(object);
    // The prime tower may print the tops of the copies of an object it flushes into with other filaments than each
    // other: the support they share stands on nothing where it does so over any of them. Copies whose tops print
    // with the same filaments stand alike, so one of them is checked for all.
    std::vector<std::vector<int>> top_filaments(object.instances().size());
    if (object.config().flush_into_objects && top_filaments.size() > 1)
        for (const Layer *layer : object.layers()) {
            const WipingExtrusions &wiping = ordering.tools_for_layer(layer->print_z).wiping_extrusions();
            if (wiping.is_anything_overridden())
                for (const LayerRegion *region : layer->regions())
                    for (const ExtrusionEntity *entity : region->fills.entities) {
                        bool top = false;
                        for_each_extrusion_path(*entity, [&top](const ExtrusionPath &path) {
                            top |= path.role() == erTopSolidInfill;
                        });
                        if (top)
                            for (size_t copy = 0; copy < top_filaments.size(); ++copy)
                                top_filaments[copy].push_back(wiping.extruder_override(entity, &object, copy));
                    }
        }
    std::vector<size_t> copies;
    for (size_t copy = 0; copy < std::max<size_t>(top_filaments.size(), 1); ++copy)
        if (std::none_of(copies.begin(), copies.end(),
                [&top_filaments, copy](size_t checked) { return top_filaments[checked] == top_filaments[copy]; }))
            copies.push_back(copy);
    for (const size_t copy : copies) {
        ModelFootprints model;
        model.nozzle_layers = &nozzle_layers;
        model.copy = copy;
        for (size_t i = 0; i < covered.size(); ++i) {
            throw_if_canceled(object);
            LayerFooting footing(object, covered, i, model, &ordering);
            append_floating_heights(floating, object, i, floating_islands(object, covered, i, model, &ordering),
                footing, base_span, min_area);
        }
    }
    return floating;
}

// Whether the top surface of a region of the object delivers material from some nozzle variants and none from others:
// only then does the nozzle that prints it decide its footing.
static bool top_flow_depends_on_nozzle(const PrintObject &object)
{
    const auto regions = object.all_regions();
    return std::any_of(regions.begin(), regions.end(), [](const PrintRegion &region) {
        const auto &ratios = region.config().top_solid_infill_flow_ratio.values;
        return std::any_of(ratios.begin(), ratios.end(), [](double ratio) { return ratio == 0.; }) &&
            std::any_of(ratios.begin(), ratios.end(), [](double ratio) { return ratio != 0.; });
    });
}

// Whether the settings of the object, flushed into or not, call for checking the warning of its Organic support again
// once the tool plan is known.
static bool final_check_called_for(const PrintObject &object, const bool flushed)
{
    const auto &config = object.config();
    const Print &print = *object.print();
    // An automatic nozzle assignment, or the tool plan of an object the prime tower may flush into, picks the nozzle
    // that prints a top only after the support is generated.
    const bool nozzle_picked_late = flushed ||
        (print.is_BBL_printer() && is_auto_filament_map_mode(print.get_filament_map_mode()));
    return !config.support_on_build_plate_only && hollow_organic(config) && !on_belt(object) &&
        (print.has_custom_tool_overrides() || has_any_mixed_filament(print.config().filament_is_mixed.values) ||
            (nozzle_picked_late && top_flow_depends_on_nozzle(object)));
}

bool organic_support_needs_final_check(const PrintObject &object)
{
    return !object.support_layers().empty() && final_check_called_for(object, object.config().flush_into_objects);
}

bool organic_support_final_check_ends_with_flushing(const PrintObject &object)
{
    // Reads no support layers: a copy of an identical object takes them over after its support step is done, see
    // PrintObject::copy_layers_from_shared_object().
    return final_check_called_for(object, true) && !final_check_called_for(object, false);
}

std::string organic_support_warning_message(const PrintObject &object)
{
    const std::vector<coordf_t> &heights = object.organic_support_heights().heights;
    if (heights.empty())
        return {};
    constexpr size_t max_heights = 5;
    std::string list;
    for (size_t i = 0; i < std::min(heights.size(), max_heights); ++i)
        list += (i == 0 ? "" : ", ") + float_to_string_decimal_point(heights[i], 2);
    if (heights.size() > max_heights)
        list += ", ...";
    return Slic3r::format(
        // TRN %1% is the object name; %2% lists the heights of the first unsupported islands in millimetres.
        _u8L("It seems the organic supports of object %1% have parts printed with nothing under them, "
             "starting on the layers at Z (mm): %2%. "
             "Consider a filled base pattern, such as Rectilinear, instead of a hollow one."),
        object.model_object()->name, list);
}

void organic_update_support_warning(PrintObject &object, std::vector<coordf_t> heights)
{
    std::sort(heights.begin(), heights.end());
    heights.erase(std::unique(heights.begin(), heights.end(), [](double a, double b) { return std::abs(a - b) < EPSILON; }), heights.end());
    const PrintObject *shared = object.get_shared_object();
    // Taken before the state mutex, which reading it takes too.
    const PrintStateBase::TimeStamp shared_timestamp =
        shared == nullptr ? 0 : shared->step_state_with_timestamp(posSupportMaterial).timestamp;
    // With the state mutex held: Print::apply() reads the heights and writes the name to word the warning anew for an
    // object renamed.
    object.compose_step_warning(posSupportMaterial, PrintStateBase::WarningLevel::NON_CRITICAL,
        [&object, &heights, shared, shared_timestamp] {
            object.set_organic_support_heights({ std::move(heights), shared, shared_timestamp });
            return organic_support_warning_message(object);
        },
        PrintStateBase::SlicingOrganicSupportWarning);
}

void organic_update_loaded_support_warning(PrintObject &object)
{
    if (object.support_layers().empty() || ! hollow_organic(object.config()))
        return;
    const SupportParameters support_params(object);
    organic_update_support_warning(object,
        floating_support_islands(object, scaled<double>(1.) * support_params.support_material_flow.scaled_width()));
}

void organic_update_shared_support_warning(PrintObject &object)
{
    const PrintObject *shared = object.get_shared_object();
    if (shared == nullptr)
        return;
    auto warns = [](const PrintObject &of) {
        const auto state = of.step_state_with_warnings(posSupportMaterial);
        return state.state == PrintStateBase::DONE && std::any_of(state.warnings.begin(), state.warnings.end(),
            [](const PrintStateBase::Warning &warning) {
                return warning.current && warning.message_id == PrintStateBase::SlicingOrganicSupportWarning;
            });
    };
    // This object takes over the layers again on every slice, which clears its warning. The checks after generating the
    // support, by the tool ordering and by the Scarf seams exported, may not run again, and may find for this object
    // what they do not for the other one: a Scarf seam starts nearest to where the nozzle comes from to each instance.
    // So it keeps the heights found for it while the support it shares stays as generated then, and takes those of the
    // other object else.
    const PrintObject::OrganicSupportHeights &own = object.organic_support_heights();
    if (own.shared == shared && own.shared_support_timestamp == shared->step_state_with_timestamp(posSupportMaterial).timestamp)
        organic_update_support_warning(object, own.heights);
    else if (warns(*shared))
        organic_update_support_warning(object, shared->organic_support_heights().heights);
}

OrganicScarfFooting::OrganicScarfFooting(Print &print)
{
    for (PrintObject *object : print.objects()) {
        const auto &config = object->config();
        if (config.support_on_build_plate_only || object->support_layers().empty() || on_belt(*object) || !hollow_organic(config))
            continue;
        const auto regions = object->all_regions();
        if (std::none_of(regions.begin(), regions.end(), [](const PrintRegion &region) {
                return region.config().scarf_joint_flow_ratio == 0. && region.config().seam_slope_type != SeamScarfType::None;
            }))
            continue;
        ObjectMaterial material{object, model_footing_gap(*object), {}, {}};
        for (size_t i = 0; i < object->support_layers().size(); ++i) {
            const SupportLayer &layer = *object->support_layers()[i];
            material.planes.emplace(layer.bottom_z(), i);
        }
        m_objects.emplace(object, std::move(material));
    }
}

void OrganicScarfFooting::record(const PrintObject &printed, const Point &origin, const ExtrusionPath &path,
    const double z, const bool extruding)
{
    auto it = m_objects.find(&printed);
    if (it == m_objects.end() || (!is_perimeter(path.role()) && !is_infill(path.role()) && path.role() != erGapFill))
        return;
    ObjectMaterial &object = it->second;
    auto [instance, inserted] = object.instances.try_emplace({origin.x(), origin.y()});
    if (inserted)
        instance->second.resize(object.object->support_layers().size());
    if (!extruding)
        return;
    double min_z = z, max_z = z;
    if (path.z_contoured)
        for (const Point3 &point : path.polyline.points) {
            min_z = std::min(min_z, z + unscale<double>(point.z()));
            max_z = std::max(max_z, z + unscale<double>(point.z()));
        }
    for (auto plane = object.planes.lower_bound(min_z - EPSILON);
         plane != object.planes.end() && plane->first <= max_z + object.gap + EPSILON; ++plane) {
        const double bottom = plane->first;
        append(instance->second[plane->second], path.z_contoured ?
            contoured_material(path, z, bottom - object.gap - EPSILON, bottom + EPSILON) :
            organic_bead(path.as_polyline(), scale_(path.width)));
    }
}

void OrganicScarfFooting::update_warnings()
{
    for (auto &[object, material] : m_objects) {
        const auto    covered   = support_covered(*object);
        const double  sliver    = organic_sliver_area(*object);
        const coord_t base_span = organic_base_span(*object);
        std::vector<coordf_t> heights;
        if (material.instances.empty())
            material.instances[{0, 0}].resize(covered.size());
        for (auto &[origin, footprints] : material.instances) {
            for (Polygons &plane : footprints)
                plane = union_(plane);
            ModelFootprints model;
            model.emitted = &footprints;
            for (size_t i = 0; i < covered.size(); ++i) {
                throw_if_canceled(*object);
                LayerFooting footing(*object, covered, i, model, nullptr);
                append_floating_heights(heights, *object, i, floating_islands(*object, covered, i, model), footing,
                    base_span, sliver);
            }
        }
        organic_update_support_warning(*material.object, std::move(heights));
    }
}

std::vector<PrintBase::SlicingStatus> organic_support_warning_statuses(const Print &print)
{
    std::vector<PrintBase::SlicingStatus> statuses;
    for (const PrintObject *object : print.objects()) {
        const auto state = object->step_state_with_warnings(posSupportMaterial);
        if (state.state != PrintStateBase::DONE)
            continue;
        for (const auto &warning : state.warnings)
            if (warning.current && warning.message_id == PrintStateBase::SlicingOrganicSupportWarning)
                statuses.emplace_back(*object, posSupportMaterial, warning.message,
                    PrintStateBase::SlicingOrganicSupportWarning, warning.level);
    }
    return statuses;
}

// Drop the extrusions of a collection lying in region, most of their beads in it, and the collections left empty. Return
// whether any is dropped. The crumbs of a layer are islands of whole extrusions, but a sliver the union of the beads of the
// layer leaves apart from them, where two loops nearly touch, lies along the edge of beads it is no part of.
static bool remove_extrusions_in(ExtrusionEntityCollection &collection, const Polygons &region)
{
    return remove_extrusions_if(collection, [&region](const ExtrusionEntity &entity) {
        const Polygons material = support_material(entity);
        return 2. * area(intersection(material, region)) > area(material); }) > 0;
}

// The beads of a layer whose walls are repaired, gathered before the repair and turned into polygons only near where a
// join needs them: those of the layer, which hold the layer above and of which a route round keeps off those the repair
// leaves untouched, and those of the layer above, which a join may hold. A route round no longer than the wall it
// repairs lies within half the length of the wall of its bearings, so beads of the layer above farther from the
// tongues than half the longest wall near them play no part; an island of the layer above that a join touches may
// stand on a bead of the layer anywhere, so those are all gathered.
class WallNeighbours
{
public:
    struct Bead
    {
        Polyline    axis;
        coord_t     width;
        BoundingBox box;
        // Of a wall the repair may shorten, which it keeps only outside the tongues.
        bool        repaired;
    };

    // adrift, if set, gives the islands of the layer on nothing once the tongues are cut off, see rooted().
    WallNeighbours(const PrintObjectConfig &config, const ExtrusionEntityCollection &fills, const Polygons &region,
        const ExtrusionEntityCollection *above, const Polygons *above_covered, std::function<Polygons()> adrift = {})
        : m_region(region), m_above_covered(above_covered), m_adrift_of(std::move(adrift))
    {
        const BoundingBox tongues = get_extents(region);
        BoundingBox       near    = tongues;
        double            reach   = 0.;
        for_each_extrusion_path(fills, [&](const ExtrusionPath &path) {
            if (get_extents(path.as_polyline()).overlap(tongues))
                reach = std::max(reach, .5 * path.length() + 2. * scale_(path.width));
        });
        near.offset(reach);
        const auto gather = [&](const ExtrusionEntityCollection &collection, std::vector<Bead> &beads, bool layer) {
            for_each_extrusion_path(collection, [&](const ExtrusionPath &path) {
                if (!flat_support(config, path))
                    return;
                Polyline          axis = path.as_polyline();
                const BoundingBox box  = get_extents(axis);
                if (!layer && !box.overlap(near))
                    return;
                const bool repaired = layer && path.role() == erSupportMaterial && box.overlap(tongues) &&
                    !intersection_pl(Polylines{axis}, region).empty();
                beads.push_back({std::move(axis), coord_t(scale_(path.width)), box, repaired});
            });
        };
        gather(fills, m_layer, true);
        if (above != nullptr)
            gather(*above, m_above, false);
    }

    // What a route round of a path width wide keeps its axis out of near box: the beads kept, grown by half that width,
    // which its bead then does not overlap. The walls repaired are not kept off, as a route leaving one runs beside it
    // along the edge of what holds both, see overprints().
    Polygons avoid(const BoundingBox &box, const coord_t width) const
    {
        Polygons kept;
        for (const Bead &bead : m_layer)
            if (!bead.repaired && bead.box.overlap(box))
                append(kept, organic_bead(bead.axis, double(bead.width)));
        return kept.empty() ? Polygons() : offset(kept, .5 * double(width) - SCALED_EPSILON, jtRound, SCALED_EPSILON);
    }

    // The axes of the walls repaired near box, as far as the repair keeps them.
    Polylines walls(const BoundingBox &box) const
    {
        Polylines walls;
        for (const Bead &bead : m_layer)
            if (bead.repaired && bead.box.overlap(box))
                append(walls, diff_pl(Polylines{bead.axis}, m_region));
        return walls;
    }

    // The beads of the layer near box: those kept and, as far as the repair keeps them, those of the walls repaired.
    Polygons material(const BoundingBox &box) const
    {
        Polygons kept, walls;
        this->beads_near(box, kept, walls);
        return union_(kept, walls);
    }

    // What of polygons of the layer lies on its islands that stand on the support below once the tongues are cut off:
    // an island on nothing, which cleanup drops or the warning reports, holds nothing above it.
    Polygons rooted(Polygons polygons) const
    {
        if (polygons.empty() || !m_adrift_of)
            return polygons;
        if (!m_adrift)
            m_adrift = m_adrift_of();
        if (m_adrift->empty())
            return polygons;
        return diff(polygons, ClipperUtils::clip_clipper_polygons_with_subject_bbox(*m_adrift,
                                  get_extents(polygons).inflated(SCALED_EPSILON)));
    }

    // The islands of the layer above whose boxes overlap box, whole.
    std::vector<const ExPolygon*> above_islands(const BoundingBox &box) const
    {
        if (m_above_covered == nullptr)
            return {};
        if (!m_islands) {
            m_islands.emplace(union_ex(*m_above_covered));
            for (const ExPolygon &island : *m_islands)
                m_island_boxes.emplace_back(get_extents(island.contour));
        }
        std::vector<const ExPolygon*> near;
        for (size_t i = 0; i < m_islands->size(); ++ i)
            if (m_island_boxes[i].overlap(box))
                near.push_back(&(*m_islands)[i]);
        return near;
    }

    // The beads of the layer above whose boxes overlap box.
    std::vector<const Bead*> above_beads(const BoundingBox &box) const
    {
        std::vector<const Bead*> near;
        for (const Bead &bead : m_above)
            if (bead.box.overlap(box))
                near.push_back(&bead);
        return near;
    }

private:
    void beads_near(const BoundingBox &box, Polygons &kept, Polygons &walls) const
    {
        for (const Bead &bead : m_layer)
            if (bead.box.overlap(box)) {
                if (!bead.repaired)
                    append(kept, organic_bead(bead.axis, double(bead.width)));
                else
                    for (const Polyline &part : diff_pl(Polylines{bead.axis}, m_region))
                        append(walls, organic_bead(part, double(bead.width)));
            }
    }

    const Polygons                  &m_region;
    const Polygons                  *m_above_covered;
    std::function<Polygons()>        m_adrift_of;
    mutable std::optional<Polygons>  m_adrift;
    std::vector<Bead>                m_layer;
    std::vector<Bead>                m_above;
    mutable std::optional<ExPolygons> m_islands;
    mutable std::vector<BoundingBox> m_island_boxes;
};

// What a route round between two bearings keeps its axis out of, if anything, see WallNeighbours::avoid().
using AvoidNear = std::function<Polygons(const Point &, const Point &)>;

// What of polygons lies in box, each clipped on its own and closed along the edges of the box, an edge crossing one cut
// on it to the nearest unit: nothing of them lies outside the box, where the edges of a cut that only drops the corners
// out of it, see ClipperUtils::clip_clipper_polygons_with_subject_bbox(), may cross each other.
static Polygons clip_to_box(const Polygons &polygons, const BoundingBox &box)
{
    Clipper2Lib::Paths64 paths;
    for (const Polygon &polygon : polygons)
        if (polygon.size() >= 3 && get_extents(polygon).overlap(box)) {
            Clipper2Lib::Path64 &path = paths.emplace_back();
            path.reserve(polygon.size());
            for (const Point &point : polygon.points)
                path.emplace_back(point.x(), point.y());
        }
    Polygons clipped;
    for (const Clipper2Lib::Path64 &path :
         Clipper2Lib::RectClip(Clipper2Lib::Rect64(box.min.x(), box.min.y(), box.max.x(), box.max.y()), paths)) {
        Polygon &polygon = clipped.emplace_back();
        polygon.points.reserve(path.size());
        for (const Clipper2Lib::Point64 &point : path)
            polygon.points.emplace_back(coord_t(point.x), coord_t(point.y));
    }
    return clipped;
}

// Connect retained bearings without forbidden material or an excessive free span. A route round runs along the edge of
// the footing and of what avoid gives it to keep off, if given. A straight join only crosses that. A join reaching the
// edge of the box of its bearings grown by reach is longer than twice reach, longer than the path the bearings shorten,
// see trim_unsupported_base(), which drops it: only the footing and the legal polygons in that box are looked at,
// clipped to it, see clip_to_box(), which changes them only on the edge of the box, so that a route over what it
// changes reaches the edge too. Clipping may join two outlines along that edge, and a bearing within rounding of both
// then lies on one loop twice: each of its places on a loop is tried.
static bool connect_wall_bearings(const Point &a, const Point &b, const Polygons &whole_footing,
    const Polygons &whole_legal, const AvoidNear *avoid, bool limited, double max_bridge, double reach,
    Polyline &connection)
{
    BoundingBox box;
    box.merge(a);
    box.merge(b);
    box.offset(reach);
    const Polygons footing = clip_to_box(whole_footing, box);
    const Polygons legal   = limited ? clip_to_box(whole_legal, box) : Polygons();
    const auto safe = [&](const Polyline &axis) {
        if (!contains(footing, axis.first_point()) || !contains(footing, axis.last_point()) ||
            (limited && !diff_pl(Polylines{axis}, legal).empty()))
            return false;
        for (const Polyline &gap : diff_pl(Polylines{axis}, footing))
            if (gap.length() > max_bridge)
                return false;
        return true;
    };
    connection = Polyline(a, b);
    if (safe(connection))
        return true;
    double         shortest = std::numeric_limits<double>::max();
    const Polygons around   = limited ? intersection(footing, legal) : footing;
    const Polygons off      = avoid != nullptr ? (*avoid)(a, b) : Polygons();
    // The places of a bearing on a closed loop: the nearest point of each run of its edges within rounding of it, which
    // meet within rounding of it too, a run going on over the first point of the loop. Two edges near it that meet far
    // from it, as two outlines clipping joins, are two places.
    const auto places = [](const Point &p, const Polyline &loop) {
        std::vector<std::pair<Point, double>> found;
        bool                                  first_near = false, last_near = false;
        const Lines                           lines = loop.lines();
        for (size_t i = 0; i < lines.size(); ++ i) {
            const Point  at       = p.projection_onto(lines[i]);
            const double distance = at.distance_to(p);
            const bool   near     = distance <= 2. * SCALED_EPSILON;
            const bool   joined   = near && last_near && lines[i].a.distance_to(p) <= 2. * SCALED_EPSILON;
            if (near && !joined)
                found.emplace_back(at, distance);
            else if (joined && distance < found.back().second)
                found.back() = {at, distance};
            first_near = first_near || (i == 0 && near);
            last_near  = near;
        }
        if (first_near && last_near && found.size() > 1 && lines.front().a.distance_to(p) <= 2. * SCALED_EPSILON) {
            if (found.back().second < found.front().second)
                found.front() = found.back();
            found.pop_back();
        }
        Points points;
        for (const auto &place : found)
            points.push_back(place.first);
        return points;
    };
    for (const Polygon &boundary : to_polygons(off.empty() ? union_ex(around) : diff_ex(around, off))) {
        const Polyline loop = boundary.split_at_first_point();
        for (const Point &from : places(a, loop))
            for (const Point &to : places(b, loop)) {
                Point    start = from, end = to;
                Polyline before, after;
                loop.split_at(start, &before, &after);
                after.append(before);
                Polyline forward, backward;
                after.split_at(end, &forward, &backward);
                backward.reverse();
                for (const Polyline &route : {forward, backward}) {
                    Polyline candidate;
                    candidate.append(a);
                    candidate.append(start);
                    candidate.append(route);
                    candidate.append(b);
                    const double length = candidate.length();
                    if (length < shortest && safe(candidate)) {
                        shortest = length;
                        connection = std::move(candidate);
                    }
                }
            }
    }
    return shortest < std::numeric_limits<double>::max();
}

// Clip in source order: Clipper may otherwise reorder spliced inner loops.
static bool shorten_wall_axis(const Polyline &axis, const Polygons &cut, const Polygons &footing,
    const Polygons &legal, const AvoidNear *avoid, bool limited, double max_bridge, double closure_distance,
    double reach, Polyline &result)
{
    const BoundingBox bounds = get_extents(cut);
    const auto append_run = [&](const Polyline &part) {
        if (!result.empty() && result.last_point().distance_to(part.first_point()) > SCALED_EPSILON) {
            Polyline connection;
            if (!connect_wall_bearings(result.last_point(), part.first_point(), footing, legal, avoid, limited,
                    max_bridge, reach, connection))
                return false;
            result.append(connection);
        }
        result.append(part);
        return true;
    };
    for (const Line &line : axis.lines()) {
        BoundingBox box;
        box.merge(line.a);
        box.merge(line.b);
        Polylines kept = box.overlap(bounds) ? diff_pl(Polyline(line.a, line.b), cut) : Polylines{Polyline(line.a, line.b)};
        for (Polyline &part : kept)
            if (line.a.distance_to(part.first_point()) > line.a.distance_to(part.last_point()))
                part.reverse();
        std::sort(kept.begin(), kept.end(), [&](const Polyline &one, const Polyline &two) {
            return line.a.distance_to(one.first_point()) < line.a.distance_to(two.first_point());
        });
        for (const Polyline &part : kept)
            if (!append_run(part))
                return false;
    }
    // Support walls may already carry a small seam gap. Their round bead ends
    // overlap; cutting across that seam must not turn it into an open wall.
    const bool cut_seam = result.size() > 1 &&
        (result.first_point() != axis.first_point() || result.last_point() != axis.last_point());
    const bool closed = axis.is_closed() ||
        (cut_seam && axis.first_point().distance_to(axis.last_point()) <= closure_distance);
    if (result.size() > 1 && closed && !result.is_closed()) {
        Polyline connection;
        if (!connect_wall_bearings(result.last_point(), result.first_point(), footing, legal, avoid, limited,
                max_bridge, reach, connection))
            return false;
        result.append(connection);
    }
    return true;
}

// Shorten unsupported base axes while preserving the rooted wall and its closure, bridging no farther than
// free_span(), or with support on the build plate only, which prints no bottom interface, than a line across the gap
// of the base, routes round kept off the beads the repair leaves untouched near them, see WallNeighbours::avoid().
// Where that fails, leaves a path no shorter, or leaves more of the layer above on nothing than the other repair, more
// whole islands or as many and longer runs, or as much but runs over what it keeps of the walls it repairs where the
// other does not, the path is repaired more widely: bridging as far as free_span(), and over the beads of the layer.
// Kept whole, the path would hang more.
static size_t trim_unsupported_base(ExtrusionEntityCollection &collection, const Polygons &region,
    const Polygons &footing, const Polygons *legal, const WallNeighbours &neighbours, const PrintObjectConfig &config,
    const coord_t base_span)
{
    ExtrusionEntityCollection original;
    collection.entities.swap(original.entities);
    ExtrusionEntityCollection result;
    size_t trimmed = 0;
    struct Clip {
        Polygons cut;
        Polygons footing;
        Polygons legal;
    };
    std::map<coord_t, Clip> clips_by_width;
    const BoundingBox bounds = get_extents(region);
    for (ExtrusionEntity *&entity : original.entities) {
        if (entity->is_collection()) {
            auto &nested = *static_cast<ExtrusionEntityCollection*>(entity);
            trimmed += trim_unsupported_base(nested, region, footing, legal, neighbours, config, base_span);
            if (!nested.empty())
                result.entities.push_back(entity);
            else
                delete entity;
            entity = nullptr;
            continue;
        }
        ExtrusionEntityCollection parts;
        bool cut = false;
        for_each_extrusion_path(*entity, [&](const ExtrusionPath &path) {
            if (path.role() != erSupportMaterial || path.z_contoured) {
                parts.append(path);
                return;
            }
            const Polyline axis = path.as_polyline();
            if (!get_extents(axis).overlap(bounds)) {
                parts.append(path);
                return;
            }
            const coord_t width = scale_(path.width);
            auto [it, inserted] = clips_by_width.try_emplace(width);
            Clip &clip = it->second;
            if (inserted) {
                clip.cut = diff(region, offset(footing, .5 * double(width), jtRound, SCALED_EPSILON));
                clip.footing = offset(footing, .5 * double(width) + SCALED_EPSILON, jtRound, SCALED_EPSILON);
                if (legal != nullptr)
                    clip.legal = offset(*legal, -.5 * double(width) + SCALED_EPSILON, jtRound, SCALED_EPSILON);
            }
            if (intersection_pl(Polylines{axis}, clip.cut).empty()) {
                parts.append(path);
                return;
            }
            // A route round no longer than the path lies within half its length of its bearings, within reach of
            // them with a line to spare.
            const double    reach    = .5 * axis.length() + double(width);
            bool            avoided  = false;
            const AvoidNear keep_off = [&](const Point &a, const Point &b) {
                BoundingBox box;
                box.merge(a);
                box.merge(b);
                box.offset(reach);
                Polygons off = neighbours.avoid(box, width);
                avoided = avoided || !off.empty();
                return off;
            };
            // Whether the joins of a repair run over what it keeps of the walls it repairs for longer than a line,
            // their axes within half a line of the axes of those walls but where they meet them: they would print
            // that much twice. Running beside a wall, a join overlaps its bead by half a line at most, as it does
            // leaving it.
            const auto overprints = [&](const Polyline &joined) {
                const Polylines joins =
                    diff_pl(Polylines{joined}, offset(Polylines{axis}, 2.f * float(SCALED_EPSILON)));
                if (joins.empty())
                    return false;
                BoundingBox box = get_extents(joins);
                box.offset(double(width));
                const Polylines walls = neighbours.walls(box);
                if (walls.empty())
                    return false;
                Polygons ends;
                for (const Polyline &join : joins)
                    for (const Point &end : {join.first_point(), join.last_point()}) {
                        Polygon disc = make_circle(.5 * double(width) + scaled<double>(.005), SCALED_EPSILON);
                        disc.translate(end);
                        ends.push_back(std::move(disc));
                    }
                double over = 0.;
                for (const Polyline &part :
                     intersection_pl(joins, diff(offset(walls, .5f * float(width) - scaled<float>(.005)), ends)))
                    over += part.length();
                return over > double(width);
            };
            const auto shortened = [&](const double max_bridge, const AvoidNear *avoid, Polyline &joined) {
                return shorten_wall_axis(axis, clip.cut, clip.footing, clip.legal, avoid, legal != nullptr, max_bridge,
                           double(width), reach, joined) &&
                    axis.length() - joined.length() > SCALED_EPSILON;
            };
            // What of the layer above the beads of one path hold that those of the other, with the rest of the layer,
            // leave on nothing: the islands touching the first but neither of the others, see floating_islands(), and
            // the length of the runs of beads within reach of the first but of neither of the others, see bead_reach(),
            // each longer than its bead is wide. The rest of the layer holds only where it stands on the support below,
            // see WallNeighbours::rooted(), and what holds an island may lie far from the path.
            struct Left
            {
                size_t islands = 0;
                double runs    = 0.;
                bool   none() const { return islands == 0 && runs == 0.; }
                // More whole islands on nothing, or as many and longer runs, by more than rounding.
                bool   more(const Left &other) const
                {
                    return islands != other.islands ? islands > other.islands :
                                                      runs > other.runs + scaled<double>(.001);
                }
            };
            const auto holds_more = [&](const Polyline &one, const Polyline &other) {
                Left           left;
                const Polygons beads = organic_bead(one, double(width));
                BoundingBox    box   = get_extents(beads);
                box.offset(2. * double(width));
                const std::vector<const ExPolygon*>             islands = neighbours.above_islands(box);
                const std::vector<const WallNeighbours::Bead*> above   = neighbours.above_beads(box);
                if (islands.empty() && above.empty())
                    return left;
                const Polygons theirs  = organic_bead(other, double(width));
                const Polygons rest    = union_(theirs, neighbours.rooted(neighbours.material(box)));
                const Polygons on_one  = offset(beads, footing_reach);
                const Polygons on_rest = offset(rest, footing_reach);
                for (const ExPolygon *island : islands) {
                    const Polygons shape = to_polygons(*island);
                    if (intersection(shape, on_one).empty() || !intersection(shape, on_rest).empty())
                        continue;
                    BoundingBox whole = get_extents(island->contour);
                    whole.offset(2. * double(width));
                    if (intersection(shape, offset(union_(theirs, neighbours.rooted(neighbours.material(whole))),
                                                footing_reach)).empty())
                        ++ left.islands;
                }
                std::map<coord_t, std::pair<Polygons, Polygons>> reach;
                for (const WallNeighbours::Bead *bead : above) {
                    auto [it, inserted] = reach.try_emplace(bead->width);
                    if (inserted)
                        it->second = { bead_reach(beads, double(bead->width)), bead_reach(rest, double(bead->width)) };
                    for (const Polyline &run :
                         diff_pl(intersection_pl(Polylines{bead->axis}, it->second.first), it->second.second))
                        if (run.length() > double(bead->width))
                            left.runs += run.length();
                }
                return left;
            };
            const double span  = free_span(config, base_span, double(width));
            const double first = config.support_on_build_plate_only ? double(base_span) + double(width) : span;
            Polyline     repaired;
            bool         shorter = shortened(first, &keep_off, repaired);
            // The other repair differs only where it may bridge farther or run over the beads kept. It is taken where
            // the first fails or leaves more of the layer above on nothing, whole islands counting before runs, and,
            // leaving as much, where the first prints over the walls it keeps and it does not. Else the first stays, as
            // the path kept whole would hang, and the layer above left on nothing too.
            if (Polyline across; (span > first || avoided) && shortened(span, nullptr, across)) {
                bool take = !shorter;
                if (!take) {
                    const Left gain    = holds_more(across, repaired);
                    const auto cleaner = [&] { return overprints(repaired) && !overprints(across); };
                    if (!gain.none()) {
                        const Left loss = holds_more(repaired, across);
                        take = gain.more(loss) || (!loss.more(gain) && cleaner());
                    } else
                        take = cleaner() && holds_more(repaired, across).none();
                }
                if (take) {
                    repaired = std::move(across);
                    shorter  = true;
                }
            }
            if (!shorter) {
                parts.append(path);
                return;
            }
            cut = true;
            if (repaired.size() > 1)
                parts.append(ExtrusionPath(Polyline3(std::move(repaired)), path));
        });
        if (cut) {
            ++trimmed;
            result.append(std::move(parts.entities));
            delete entity;
        } else
            result.entities.push_back(entity);
        entity = nullptr;
    }
    collection.entities.swap(result.entities);
    return trimmed;
}

size_t remove_floating_support_crumbs(PrintObject &object, const double max_area, std::vector<coordf_t> *floating)
{
    if (on_belt(object))
        return 0;
    std::vector<Polygons> covered   = support_covered(object);
    size_t                removed   = 0;
    const coord_t         base_span = organic_base_span(object);
    ModelFootprints       model;
    std::vector<Polygons> rooted;
    size_t rooted_layers = 0;
    double contact_margin = 0.;
    for (const Layer *layer : object.layers())
        contact_margin = std::max(contact_margin, layer->height);
    // Whether the model lies within the top Z distance over a shape of a support layer, which it may be the contact for.
    const auto under_model = [&](const SupportLayer &layer, const Polygons &shape) {
        const double top = layer.print_z + object.slicing_parameters().gap_support_object + contact_margin;
        for (const Layer *model_layer : object.layers()) {
            if (model_layer->bottom_z() > top + EPSILON)
                break;
            if (model_layer->bottom_z() >= layer.print_z - EPSILON && ! intersection(shape, to_polygons(model_layer->lslices)).empty())
                return true;
        }
        return false;
    };
    // A larger floating base fragment may be a dead appendage that joins a bed-rooted
    // branch above. Prune it only if it carries no interface or model contact before
    // that join. Raising the area cutoff alone would also discard needed tips.
    const auto unused_appendage = [&](size_t first, const ExPolygon &island) {
        if (!object.config().support_on_build_plate_only)
            return false;
        Polygons branch = to_polygons(island);
        for (size_t j = first; !branch.empty() && j < covered.size(); ++j) {
            const SupportLayer &layer = *object.support_layers()[j];
            bool interface = false;
            for_each_extrusion_path(layer.support_fills, [&](const ExtrusionPath &path) {
                if (path.role() == erSupportMaterialInterface &&
                    !intersection(organic_bead(path.as_polyline(), scale_(path.width)), branch).empty())
                    interface = true;
            });
            if (interface)
                return false;
            // With no top interface the base itself can be the requested contact.
            if (under_model(layer, branch))
                return false;
            if (j + 1 == covered.size())
                break;
            // Only the next layer is needed to recognize a rooted join. Preserve
            // the unchanged prefix instead of rebuilding every layer after a cut.
            if (rooted.empty())
                rooted.resize(covered.size());
            for (; rooted_layers <= j + 1; ++rooted_layers) {
                const SupportLayer &next_layer = *object.support_layers()[rooted_layers];
                NearPolygons footing;
                footing.grow = footing_reach;
                footing.append(support_below(object, rooted, rooted_layers));
                rooted[rooted_layers].clear();
                for (ExPolygon &part : union_ex(covered[rooted_layers]))
                    if (Polygons shape = to_polygons(std::move(part)); next_layer.bottom_z() < EPSILON || footing.touch(shape))
                        append(rooted[rooted_layers], std::move(shape));
            }
            Polygons next;
            NearPolygons reach, rooted_above;
            reach.append(offset(branch, float(scaled<double>(0.01))));
            rooted_above.append(rooted[j + 1]);
            for (ExPolygon &part : union_ex(covered[j + 1])) {
                Polygons shape = to_polygons(std::move(part));
                if (! rooted_above.touch(shape) && reach.touch(shape))
                    append(next, std::move(shape));
            }
            branch = std::move(next);
        }
        return true;
    };
    // Where the support may stand on the model, whether nothing stands on a part of layer i: no support of the layer above
    // over it, and no model within the top Z distance, which it may be the contact for.
    const auto carries_nothing = [&](size_t i, const ExPolygon &part) {
        const Polygons shape = to_polygons(part);
        if (i + 1 < covered.size() && ! intersection(shape, covered[i + 1]).empty())
            return false;
        return ! under_model(*object.support_layers()[i], shape);
    };
    const auto &config = object.config();
    const bool trim_walls = hollow_organic(config) && ! support_role_has_no_flow(config, erSupportMaterial);
    // A dot of the base, a path no longer than it is wide, stands on what lies under its axis. One whose axis lies over
    // nothing hangs, though its edge touches what is below and a bead beside it keeps it in a rooted island:
    // hanging_parts() leaves runs that short out. Arachne lays such dots where a solid part runs out, over the slit
    // between the loops of the layer below. An interface is a contact, kept as a tongue's load is. Where the support
    // may stand on the model, the model footing joins the support below once a dot is found standing on nothing
    // without it.
    const auto hanging_dot = [&config](const ExtrusionEntity &entity, LayerFooting &footing, bool &model_added,
                                 bool &with_model) {
        Polylines axes;
        double    length = 0.;
        double    width  = 0.;
        bool      flat   = true;
        for_each_extrusion_path(entity, [&](const ExtrusionPath &path) {
            flat = flat && path.role() == erSupportMaterial && flat_support(config, path);
            length += path.length();
            width = std::max(width, double(scale_(path.width)));
            axes.emplace_back(path.as_polyline());
        });
        if (! flat || axes.empty() || length > width)
            return false;
        // Only the footing near the axes, which gives a dot the answer the footing of the whole layer does.
        const auto stands = [&]() {
            // Point by point: get_extents() leaves out the box of a path along an axis, which has no area.
            BoundingBox box;
            for (const Polyline &axis : axes)
                for (const Point &point : axis.points)
                    box.merge(point);
            return ! intersection_pl(axes, footing.near(with_model, box.inflated(SCALED_EPSILON))).empty();
        };
        if (stands())
            return false;
        if (model_added)
            return true;
        model_added = with_model = true;
        return ! stands();
    };
    // The tongues of a layer: the runs of its base that hang over what holds them, see hanging_parts(), grown into
    // their beads, those that cut() takes.
    const auto find_tongues = [&config, base_span](const ExtrusionEntityCollection &fills, LayerFooting &footing,
                                  const bool with_model, const auto &cut) {
        const auto reach = [&](const coord_t width) -> const Polygons & { return footing.reach(with_model, width); };
        const auto may_hang = [](const ExtrusionPath &path) {
            return path.role() == erSupportMaterial && ! path.z_contoured && path.mm3_per_mm > 0. &&
                path.polyline.points.size() >= 2 && path.length() > scale_(path.width);
        };
        AxesOutsideReach outside;
        for_each_extrusion_path(fills, [&](const ExtrusionPath &path) {
            if (may_hang(path))
                outside.add(path.as_polyline(), scale_(path.width));
        });
        outside.clip(reach);
        // The axes near what lies outside the reach, each checked on its own, in parallel, against the reach of its
        // width, found first.
        std::vector<const ExtrusionPath*> near;
        std::vector<const Polygons*>      reaches;
        for_each_extrusion_path(fills, [&](const ExtrusionPath &path) {
            if (may_hang(path) && outside.near(path.as_polyline())) {
                near.push_back(&path);
                reaches.push_back(&reach(scale_(path.width)));
            }
        });
        std::vector<Polylines> hanging(near.size());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, near.size()), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t k = range.begin(); k < range.end(); ++ k) {
                const coord_t width = scale_(near[k]->width);
                hanging[k] = hanging_parts(near[k]->as_polyline(), double(width), *reaches[k],
                    free_span(config, base_span, double(width)));
            }
        });
        Polygons tongues;
        for (size_t k = 0; k < near.size(); ++ k) {
            const coord_t width = scale_(near[k]->width);
            for (const Polyline &part : hanging[k])
                for (const ExPolygon &tongue : union_ex(organic_bead(part, double(width))))
                    if (cut(tongue))
                        append(tongues, to_polygons(tongue));
        }
        return tongues;
    };
    // The passes below leave most layers as they are: no crumb, no dot, no tongue. Such a layer only adds its floating
    // heights, which, as all that its first pass finds, depend on it, the layer above and the layers it stands on
    // alone. So all the layers are first checked so, in parallel: read only, sharing the footprints of the model with
    // the passes, and without unused_appendage(), which keeps a cache of the layers it climbs: a layer that would call
    // it goes to the passes, as does one the passes find otherwise, and one over a layer they change. Checked layer
    // by layer only, these checks took almost all of the time of the passes.
    std::vector<std::optional<std::vector<coordf_t>>> settled(covered.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, covered.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++ i) {
            throw_if_canceled(object);
            const SupportLayer              &layer = *object.support_layers()[i];
            const ExtrusionEntityCollection &fills = layer.support_fills;
            LayerFooting                     footing(object, covered, i, model, nullptr);
            const ExPolygons                 kept = floating_islands(object, covered, i, model);
            const auto crumb = [max_area](const ExPolygon &island) { return island.area() < max_area; };
            if (config.support_on_build_plate_only ? ! kept.empty() : std::any_of(kept.begin(), kept.end(), crumb))
                continue;
            if (hollow_organic(config) && layer.bottom_z() >= EPSILON) {
                bool model_added = config.support_on_build_plate_only;
                bool with_model  = false;
                if (any_extrusion_of(fills, [&](const ExtrusionEntity &entity) {
                        return hanging_dot(entity, footing, model_added, with_model); }))
                    continue;
            }
            if (trim_walls && layer.bottom_z() >= EPSILON &&
                ! find_tongues(fills, footing, false, [&](const ExPolygon &tongue) {
                      return config.support_on_build_plate_only || carries_nothing(i, tongue); }).empty())
                continue;
            std::vector<coordf_t> heights;
            if (floating != nullptr)
                append_floating_heights(heights, object, i, kept, footing, base_span);
            settled[i] = std::move(heights);
        }
    });
    // The layers whose material the passes changed, and whether any of them lies right below layer i, see
    // support_below().
    std::vector<bool> changed_layers(covered.size(), false);
    const auto        below_changed = [&](const size_t i) {
        const auto     layers = object.support_layers();
        const coordf_t bottom = layers[i]->print_z - layers[i]->height;
        for (size_t j = i; j -- > 0 && layers[j]->print_z > bottom - EPSILON;)
            if (layers[j]->print_z < bottom + EPSILON && changed_layers[j])
                return true;
        return false;
    };
    for (size_t i = 0; i < covered.size(); ++ i) {
        throw_if_canceled(object);
        const SupportLayer &layer = *object.support_layers()[i];
        ExtrusionEntityCollection &fills = object.support_layers()[i]->support_fills;
        if (settled[i] && ! below_changed(i)) {
            // The pass of dots drops the collections it leaves empty, those already empty too.
            if (hollow_organic(config) && layer.bottom_z() >= EPSILON)
                remove_extrusions_if(fills, [](const ExtrusionEntity &) { return false; });
            if (floating != nullptr)
                append(*floating, std::move(*settled[i]));
            continue;
        }
        // The passes below change this layer only, not what it stands on.
        LayerFooting footing(object, covered, i, model, nullptr);
        // The islands of the last pass, which drops none of them: those at least max_area large left on nothing.
        ExPolygons kept;
        bool changed;
        // Whether the pass looks for dots and tongues: a dot or a tongue is one for its own axes and what lies below,
        // which a pass dropping whole extrusions leaves as they are, so after a pass that finds no tongue the next one
        // finds no dot and no tongue either, unless it drops crumbs: the tongues are looked for near the axes that
        // leave the reach of the narrowest of them, which a crumb may take away, and the reach of a wider bead, its
        // outline thinned by a wider offset, may leave an axis another one held. A pass that finds tongues may cut
        // paths short, making new dots, or leave a tongue it cannot cut off, which it may once other extrusions of the
        // layer go; and where the support stands on the build plate only, unused_appendage() judges a tongue by what
        // the layer holds, which any pass changes.
        bool cuts = true;
        do {
            changed = false;
            kept.clear();
            Polygons crumbs;
            for (ExPolygon &island : floating_islands(object, covered, i, model))
                if (island.area() < max_area || unused_appendage(i, island)) {
                    append(crumbs, to_polygons(std::move(island)));
                    ++removed;
                } else
                    kept.emplace_back(std::move(island));
            // A whole island contains complete extrusions; partial tongues are clipped below. A sliver the union of the
            // beads leaves apart from them lies in none of them: dropping nothing, it would repeat the pass for ever.
            if (!crumbs.empty() && remove_extrusions_in(fills, crumbs)) {
                changed = true;
                covered[i] = support_material(fills, &object, layer.id() == 0 && std::abs(layer.bottom_z()) < EPSILON);
                changed_layers[i] = true;
                cuts              = true;
            }
            const bool look = cuts;
            cuts            = config.support_on_build_plate_only;
            if (look && hollow_organic(config) && layer.bottom_z() >= EPSILON) {
                bool         model_added = config.support_on_build_plate_only;
                bool         with_model  = false;
                const size_t dots        = remove_extrusions_if(fills, [&](const ExtrusionEntity &entity) {
                    return hanging_dot(entity, footing, model_added, with_model);
                });
                if (dots != 0) {
                    removed += dots;
                    changed = true;
                }
            }
            if (look && trim_walls && layer.bottom_z() >= EPSILON) {
                const auto cut = [&](const ExPolygon &tongue) {
                    return config.support_on_build_plate_only ? unused_appendage(i, tongue) :
                                                                carries_nothing(i, tongue);
                };
                bool     with_model = false;
                Polygons tongues    = find_tongues(fills, footing, with_model, cut);
                // Where the support may stand on the model, a tongue over it stands on it: the model footing joins the support
                // below once a tongue is found without it.
                if (! tongues.empty() && ! config.support_on_build_plate_only) {
                    with_model = true;
                    tongues    = find_tongues(fills, footing, with_model, cut);
                }
                if (!tongues.empty()) {
                    cuts                  = true;
                    const Polygons region = union_(tongues);
                    // A route round runs along the edge of what holds it, where a bead of the layer may lie, and a
                    // join may hold the layer above.
                    // What stands on nothing once the tongues are cut off holds nothing above it: the islands of the
                    // layer so, found when a join first asks what holds the layer above.
                    const auto adrift = [&]() {
                        Polygons whole = std::move(covered[i]);
                        covered[i]     = diff(whole, region);
                        Polygons islands = to_polygons(floating_islands(object, covered, i, model));
                        covered[i]     = std::move(whole);
                        return islands;
                    };
                    const bool           top = i + 1 == covered.size();
                    const WallNeighbours neighbours(config, fills, region,
                        top ? nullptr : &object.support_layers()[i + 1]->support_fills,
                        top ? nullptr : &covered[i + 1], adrift);
                    Polygons legal = to_polygons(layer.support_islands);
                    Polygons interfaces;
                    if (!layer.support_islands.empty())
                        for_each_extrusion_path(fills, [&](const ExtrusionPath &path) {
                            if (path.role() == erSupportMaterialInterface)
                                append(interfaces, organic_bead(path.as_polyline(), scale_(path.width)));
                        });
                    if (!interfaces.empty())
                        legal = diff(legal, union_(interfaces));
                    const size_t trimmed = trim_unsupported_base(fills, region, footing.polygons(with_model),
                        layer.support_islands.empty() ? nullptr : &legal, neighbours, config, base_span);
                    removed += trimmed;
                    changed = changed || trimmed != 0;
                }
            }
            if (changed) {
                covered[i] = support_material(fills, &object, layer.id() == 0 && std::abs(layer.bottom_z()) < EPSILON);
                changed_layers[i] = true;
                rooted_layers = std::min(rooted_layers, i);
            }
            // Clipping a tongue can disconnect tiny fill paths on the same layer.
        } while (changed);
        // The layers above leave this one as it is now.
        if (floating != nullptr)
            append_floating_heights(*floating, object, i, kept, footing, base_span);
    }
    return removed;
}

Polygons organic_solid_fill(const ExPolygon &island, const Polygons &solid, const coord_t width, bool &walled)
{
    const Polygons island_polygons = to_polygons(island);
    const Polygons island_solid    = intersection(island_polygons, solid);
    walled = island_solid.empty() || 2. * area(island_solid) <= island.area();
    if (island_solid.empty())
        return {};
    return walled ? intersection(island_solid, offset(island_polygons, - 2.f * float(width))) : island_polygons;
}

Polygons organic_solid_material(const ExPolygon &island, const Polygons &solid, const coord_t width, bool &walled,
    Polygons *slivers, Polygons *parts)
{
    Polygons part = organic_solid_fill(island, solid, width, walled);
    if (! walled || part.empty())
        return part;
    // A sliver of a solid part narrower than a line, which a spiral of loops a line wide does not fill, is left out:
    // the paths print it only on what holds it up, see organic_split_solid_islands().
    Polygons material = opening(part, 0.5f * float(width));
    if (slivers != nullptr || parts != nullptr)
        if (Polygons out = diff(part, material); ! out.empty()) {
            if (slivers != nullptr)
                append(*slivers, std::move(out));
            if (parts != nullptr)
                append(*parts, std::move(part));
        }
    return material;
}

Polygons organic_solid_slivers(const Polygons &bodies, const Polygons &solid, const coord_t width)
{
    Polygons slivers;
    if (! solid.empty())
        for (const ExPolygon &body : union_ex(bodies)) {
            bool walled = true;
            organic_solid_material(body, solid, width, walled, &slivers);
        }
    return slivers;
}

namespace {

// What of the solid parts of a hollow Organic layer nothing holds up: the slivers the trees inside its bodies take for
// no material, see SupportGeneratorLayer::solid_slivers, but where a tree of the layer reaches them. Both are looked up
// by their boxes, as a large layer has thousands of each.
class UnheldSolid
{
public:
    // None on the print bed, which holds up all of a solid part. Where the trees grew none, the slivers of the bodies.
    UnheldSolid(const SupportGeneratorLayer *layer, const coord_t width)
    {
        if (layer == nullptr || layer->bottom_z < EPSILON || layer->solid_polygons.empty())
            return;
        if (layer->solid_slivers)
            m_slivers = &*layer->solid_slivers;
        else {
            m_found   = organic_solid_slivers(layer->polygons, layer->solid_polygons, width);
            m_slivers = &m_found;
        }
        if (m_slivers->empty())
            return;
        m_trees = &layer->inner_trees;
        index(*m_slivers, m_sliver_index);
        index(*m_trees, m_tree_index);
    }
    UnheldSolid(const UnheldSolid &)            = delete;
    UnheldSolid &operator=(const UnheldSolid &) = delete;

    bool empty() const { return m_slivers == nullptr || m_slivers->empty(); }

    // A part without the trees of the layer: a tree takes slivers for no material and may grow over them, but its loops
    // print there, which keep off what the layer prints otherwise and find no room left beside a bead across a tree two
    // lines wide, see organic_generate_inner_tree_paths(). Of a solid part, only slivers lie under a tree.
    Polygons without_trees(const Polygons &part) const
    {
        if (this->empty() || part.empty() || m_trees->empty())
            return part;
        const Polygons trees = near(*m_trees, m_tree_index, get_extents(part));
        return trees.empty() ? part : diff(part, trees);
    }

    // What of a part lies in the slivers.
    Polygons in_slivers(const Polygons &part) const
    {
        if (this->empty() || part.empty())
            return {};
        const Polygons slivers = near(*m_slivers, m_sliver_index, get_extents(part));
        return slivers.empty() ? Polygons() : intersection(part, slivers);
    }

    // The pieces of a solid part, whole, but those lying all in the slivers, which no tree reaches.
    Polygons held(const Polygons &part) const
    {
        if (this->empty() || part.empty())
            return part;
        const BoundingBox box     = get_extents(part);
        const Polygons    slivers = near(*m_slivers, m_sliver_index, box);
        if (slivers.empty())
            return part;
        // What of the part lies out of the slivers holds its piece up: the rest of a solid part the trees take for
        // material, or what of one an outline grown after them opens. But not the hairs, nanometres across, Clipper
        // leaves along the slivers where it rounds the points edges cross at.
        ExPolygons holding;
        for (ExPolygon &out : diff_ex(part, slivers))
            if (wider_than_rounding(out))
                holding.emplace_back(std::move(out));
        ExPolygons pieces = union_ex(part);
        if (pieces.size() == 1 && ! holding.empty())
            return part;
        std::vector<bool>   kept(pieces.size(), false);
        std::vector<size_t> all(pieces.size());
        for (size_t i = 0; i < pieces.size(); ++ i)
            all[i] = i;
        mark(pieces, all, holding, kept);
        // A tree reaching a piece of slivers alone holds it up too. The trees are looked for near each piece left, as
        // those between pieces far apart reach none of them. The pieces near the same trees are cut against them at
        // once, each tree once, and those near none in common apart: a cut sweeps all it is given together.
        std::vector<size_t>                left;
        std::vector<size_t>                group;
        std::vector<size_t>                near_left;
        std::unordered_map<size_t, size_t> first_near;
        const auto                         root = [&group](size_t j) {
            while (group[j] != j)
                j = group[j] = group[group[j]];
            return j;
        };
        for (size_t i = 0; i < pieces.size(); ++ i)
            if (! kept[i]) {
                const size_t j = left.size();
                left.push_back(i);
                group.push_back(j);
                visit_near(m_tree_index, get_extents(pieces[i].contour), [&](const size_t k) {
                    if (const auto [found, added] = first_near.try_emplace(k, j); added)
                        near_left.push_back(k);
                    else
                        group[root(j)] = root(found->second);
                });
            }
        if (! near_left.empty()) {
            // What a group's trees reach lies in its own pieces, among which alone it is looked for.
            std::vector<Polygons>            trees(left.size()), alone(left.size());
            std::vector<std::vector<size_t>> members(left.size());
            for (const size_t k : near_left)
                trees[root(first_near[k])].push_back((*m_trees)[k]);
            for (size_t j = 0; j < left.size(); ++ j)
                if (const size_t r = root(j); ! trees[r].empty()) {
                    append(alone[r], to_polygons(pieces[left[j]]));
                    members[r].push_back(left[j]);
                }
            for (size_t j = 0; j < left.size(); ++ j)
                if (! trees[j].empty())
                    mark(pieces, members[j], intersection_ex(alone[j], trees[j]), kept);
        }
        Polygons out;
        for (size_t i = 0; i < pieces.size(); ++ i)
            if (kept[i])
                append(out, to_polygons(std::move(pieces[i])));
        return out;
    }

private:
    using Tree = AABBTreeIndirect::Tree<2, coord_t>;

    // Twice the area by the perimeter is the width of a strip.
    static bool wider_than_rounding(const ExPolygon &out)
    {
        double perimeter = out.contour.length();
        for (const Polygon &hole : out.holes)
            perimeter += hole.length();
        return 2. * out.area() > double(SCALED_EPSILON) * perimeter;
    }
    static void index(const Polygons &polygons, Tree &tree)
    {
        std::vector<AABBTreeIndirect::BoundingBoxWrapper> boxes;
        boxes.reserve(polygons.size());
        for (size_t i = 0; i < polygons.size(); ++ i)
            boxes.emplace_back(i, get_extents(polygons[i]));
        tree.build_modify_input(boxes);
    }
    template<typename Visit> static void visit_near(const Tree &tree, const BoundingBox &box, Visit &&visit)
    {
        AABBTreeIndirect::traverse(tree, AABBTreeIndirect::intersecting(Tree::BoundingBox(box.min, box.max)),
            [&visit](const Tree::Node &node) {
                visit(node.idx);
                return true;
            });
    }
    // The polygons near the box, cut to it, as one of them may reach far beyond it.
    static Polygons near(const Polygons &polygons, const Tree &tree, const BoundingBox &box)
    {
        Polygons out;
        visit_near(tree, box, [&polygons, &out](const size_t i) { out.push_back(polygons[i]); });
        return ClipperUtils::clip_clipper_polygons_with_subject_bbox(out, box.inflated(SCALED_EPSILON));
    }
    // Marks the pieces anything of holding lies in, of the candidates, among which lies all of it. Each part of
    // holding lies in one piece, and its box in the box of the piece, but for what Clipper rounds: that piece is the
    // only one whose box holds it but where pieces nest in rings of others, and only then are those tried, each once
    // with all the parts its box holds. A piece with narrow pieces around it, in the gaps of a comb, is looked for in
    // no more than its own, and a narrow piece with many around it, a lattice, is tried once.
    static void mark(const ExPolygons &pieces, const std::vector<size_t> &candidates, const ExPolygons &holding,
        std::vector<bool> &kept)
    {
        if (holding.empty())
            return;
        std::vector<BoundingBox>                          boxes;
        std::vector<AABBTreeIndirect::BoundingBoxWrapper> wrapped;
        boxes.reserve(candidates.size());
        wrapped.reserve(candidates.size());
        for (size_t c = 0; c < candidates.size(); ++ c) {
            boxes.emplace_back(get_extents(pieces[candidates[c]].contour).inflated(SCALED_EPSILON));
            wrapped.emplace_back(c, boxes.back());
        }
        Tree tree;
        tree.build_modify_input(wrapped);
        std::vector<Polygons> tried(candidates.size());
        for (const ExPolygon &part : holding) {
            const BoundingBox   box = get_extents(part.contour);
            std::vector<size_t> around;
            visit_near(tree, box, [&boxes, &box, &around](const size_t c) {
                if (boxes[c].contains(box))
                    around.push_back(c);
            });
            if (around.size() == 1)
                kept[candidates[around.front()]] = true;
            else
                for (const size_t c : around)
                    append(tried[c], to_polygons(part));
        }
        for (size_t c = 0; c < candidates.size(); ++ c)
            if (const size_t i = candidates[c];
                ! kept[i] && ! tried[c].empty() && ! intersection(to_polygons(pieces[i]), tried[c]).empty())
                kept[i] = true;
    }

    Polygons        m_found;
    const Polygons *m_slivers = nullptr;
    const Polygons *m_trees   = nullptr;
    Tree            m_sliver_index;
    Tree            m_tree_index;
};

// Organic: the solid parts of a hollow body, the bodies widened under an interface, see organic_solid_fill(), and the
// islands to be printed as walls. The trees inside the bodies grow under no more of a solid part than they take for
// material, see organic_solid_material(): a sliver narrower than a line, printed as Arachne lays it or where it lays
// only dots as a bead a line wide along its middle, see organic_extrude_solid_fill(), hangs unless the rest of its
// piece or a tree under it holds it. So of an island printed with a wall, a body or a part of one an interface of its
// layer leaves, its walls cutting into the solid part along the interface too, a piece of the solid part is printed
// whole, but not one lying all in the slivers, which no tree reaches, and nothing of it over a tree of the layer. The
// trees of the layer below carry what of the slivers is printed, see organic_printed_slivers(). A piece the trees did
// not see, where the outline grew after them, is printed as it is.
Polygons split_solid_islands(const Polygons &polygons, const Polygons &solid, const coord_t width, Polygons &fill,
    const UnheldSolid &unheld)
{
    if (solid.empty())
        return polygons;
    Polygons walls;
    for (const ExPolygon &island : union_ex(polygons)) {
        bool     walled = true;
        // With the rule the inner trees use, see organic_grow_trees_inside_hollow_bodies().
        Polygons part   = organic_solid_fill(island, solid, width, walled);
        if (walled) {
            if (! unheld.empty())
                part = unheld.without_trees(unheld.held(part));
            append(walls, to_polygons(island));
        }
        append(fill, std::move(part));
    }
    return walls;
}

} // namespace

Polygons organic_split_solid_islands(const Polygons &polygons, const Polygons &solid, const coord_t width,
    Polygons &fill, const SupportGeneratorLayer *layer)
{
    const UnheldSolid unheld(layer, width);
    return split_solid_islands(polygons, solid, width, fill, unheld);
}

Polygons organic_printed_slivers(const Polygons &parts, const SupportGeneratorLayer &layer, const coord_t width)
{
    // The solid parts of all the islands at once: each piece of one lies in its island, apart from the others.
    const UnheldSolid unheld(&layer, width);
    return unheld.in_slivers(unheld.without_trees(unheld.held(parts)));
}

Polygons organic_extrude_solid_islands(ExtrusionEntitiesPtr &dst, const Polygons &polygons, const Polygons &solid, const Flow &flow,
    const PrintConfig &print_config, const PrintObjectConfig &object_config, const SupportGeneratorLayer *layer)
{
    if (solid.empty())
        return polygons;
    const UnheldSolid unheld(layer, flow.scaled_width());
    // Island by island, as the paths are to follow one another.
    Polygons walls;
    for (const ExPolygon &island : union_ex(polygons)) {
        Polygons fill;
        append(walls, split_solid_islands(to_polygons(island), solid, flow.scaled_width(), fill, unheld));
        organic_extrude_solid_fill(dst, fill, flow, print_config, object_config);
    }
    return walls;
}

// Organic: print solid parts of a hollow body as spirals from their outline in. The spiral is Arachne's, its loops as wide
// as the part leaves room for: loops of a fixed width leave a slit up to a line wide where they run out, along the middle
// of a part mostly, and as the part widens going up, the loop the layer above starts there lies over the slit.
void organic_extrude_solid_fill(ExtrusionEntitiesPtr &dst, const Polygons &fill, const Flow &flow, const PrintConfig &print_config,
    const PrintObjectConfig &object_config)
{
    if (fill.empty())
        return;
    FillParams params;
    params.density        = 1.f;
    params.dont_adjust    = false;
    params.use_arachne    = true;
    params.layer_height   = flow.height();
    params.extrusion_role = ExtrusionRole::erSupportMaterial;
    std::unique_ptr<Fill> filler(Fill::new_from_type(ipSpiralInset));
    filler->spacing             = flow.spacing();
    filler->print_config        = &print_config;
    filler->print_object_config = &object_config;
    // The fill sets its outermost loop half a line inside the outline, as a wall.
    for (ExPolygon &region : union_ex(fill)) {
        filler->set_bounding_box(get_extents(region));
        const Surface  surface(stInternal, std::move(region));
        ThickPolylines loops;
        try {
            loops = filler->fill_surface_arachne(&surface, params);
        } catch (InfillFailedException &) {
        }
        // A piece shorter than a quarter of a line, as Arachne fits in the middle of a part apart from the spiral, is a
        // dot printed after a travel of its own.
        loops.erase(std::remove_if(loops.begin(), loops.end(),
            [&flow](const ThickPolyline &loop) { return loop.length() < 0.25 * double(flow.scaled_width()); }), loops.end());
        // Arachne at times lays nothing but such dots in a part a couple of lines across. Left out, the part would hold
        // nothing up where the trees take it for material, see organic_grow_trees_inside_hollow_bodies(): it is filled
        // with loops going inward, or where it is too narrow for one, with a bead along its middle as wide as it is.
        if (loops.empty()) {
            if (Polylines inward = organic_inward_loops(to_polygons(surface.expolygon), flow); ! inward.empty()) {
                extrusion_entities_append_paths(dst, std::move(inward), ExtrusionRole::erSupportMaterial, flow.mm3_per_mm(),
                    flow.width(), flow.height());
                continue;
            }
            const float half = 0.5f * float(flow.scaled_width());
            Geometry::MedialAxis(half, 2. * flow.scaled_width(), surface.expolygon).build(&loops);
            for (ThickPolyline &middle : loops)
                for (coordf_t &bead : middle.width)
                    bead = std::max(bead, coordf_t(flow.scaled_width()));
        }
        for (const ThickPolyline &loop : loops) {
            // A path for each width along a piece, kept together so that they print in a row: apart, a short one
            // could be left alone between travels where dots are removed around it.
            ExtrusionMultiPath piece = thick_polyline_to_multi_path(loop, ExtrusionRole::erSupportMaterial, flow,
                scaled<float>(0.05), float(SCALED_EPSILON));
            if (piece.paths.empty())
                continue;
            if (piece.first_point() == piece.last_point())
                dst.emplace_back(new ExtrusionLoop(std::move(piece.paths)));
            else
                dst.emplace_back(new ExtrusionMultiPath(std::move(piece)));
        }
    }
}

} // namespace Slic3r
