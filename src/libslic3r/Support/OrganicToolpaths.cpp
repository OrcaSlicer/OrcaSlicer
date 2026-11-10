#include "OrganicToolpaths.hpp"
#include "BeltFloorContext.hpp"
#include "SupportCommon.hpp"
#include "SupportLayer.hpp"
#include "SupportParameters.hpp"
#include "TreeSupportCommon.hpp"
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
#include "../Surface.hpp"
#include "../VariableWidth.hpp"
#include "../libslic3r.h"
#include "../format.hpp"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// Reserve dense feet in a sparse flange so trees cannot fall between its lines.
// A solid flange already holds them; partitioning it would add internal ring perimeters.
void organic_fill_first_layer(ExtrusionEntitiesPtr &dst, const Polygons &polygons, const Polygons &feet,
    Fill *filler, float density, ExtrusionRole role, const Flow &flow, const SupportParameters &support_params)
{
    const Polygons pads = density < 1.f ? intersection(polygons, feet) : Polygons();
    fill_expolygons_with_sheath_generate_paths(dst, pads.empty() ? polygons : diff(polygons, pads), filler, density,
        role, flow, support_params, true, true);
    if (! pads.empty())
        fill_expolygons_with_sheath_generate_paths(dst, pads, filler, 1.f, role, flow, support_params, true, true);
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

static bool at_axis_end(const Polyline &part, const Polyline &axis)
{
    auto is_axis_end = [&axis](const Point &point) { return point == axis.first_point() || point == axis.last_point(); };
    return is_axis_end(part.first_point()) || is_axis_end(part.last_point());
}

// Whether a run of the axis of a bead width wide outside what holds it hangs: longer than a line and ending in the air or
// leaving the bead-wide corridor between its bearings, or a straight bridge between bearings longer than max_span.
static bool run_hangs(const Polyline &run, const Polyline &axis, const bool closed, const double width, const double max_span)
{
    if (run.length() <= width)
        return false;
    const Line span(run.first_point(), run.last_point());
    const bool bent = std::any_of(run.points.begin(), run.points.end(),
        [&](const Point &point) { return span.distance_to(point) > width + scale_(.01); });
    return bent || run.length() > max_span || (! closed && at_axis_end(run, axis));
}

// The runs of an axis of a bead width wide outside reachable that hang.
static Polylines hanging_parts(const Polyline &axis, const double width, const Polygons &reachable, const double max_span)
{
    const bool closed = axis.first_point().distance_to(axis.last_point()) <= width;
    Polylines  parts;
    if (closed) {
        Points loop = axis.points;
        if (axis.is_closed())
            loop.pop_back();
        // Closed clipping rejoins pieces split at the source seam.
        parts = diff_pl(Polygons{ Polygon(std::move(loop)) }, reachable);
    } else
        parts = diff_pl(Polylines{ axis }, reachable);
    parts.erase(std::remove_if(parts.begin(), parts.end(),
        [&](const Polyline &part) { return ! run_hangs(part, axis, closed, width, max_span); }), parts.end());
    return parts;
}

// A path of a bead printed in several paths.
struct BeadPiece {
    Polyline axis;
    coord_t  width;
};

// How many runs hang of a bead of paths of different widths whose whole axis is axis. Each path is held only as far as its
// own width reaches, while a run goes on across the ends of the paths, which are no ends of the bead, and is as wide as the
// widest path it spans.
template<typename Reach, typename Span>
static size_t varying_bead_hanging_runs(const std::vector<BeadPiece> &pieces, const Polyline &axis, const Reach &reach, const Span &max_span)
{
    const bool             closed = axis.first_point().distance_to(axis.last_point()) <= std::max(pieces.front().width, pieces.back().width);
    std::vector<BeadPiece> runs;
    BeadPiece              tail{ {}, 0 };
    const auto close_tail = [&] {
        if (! tail.axis.points.empty())
            runs.emplace_back(std::move(tail));
        tail = BeadPiece{ {}, 0 };
    };
    const auto touches = [](const Polyline &part, const Point &point) { return part.first_point() == point || part.last_point() == point; };
    for (const BeadPiece &piece : pieces) {
        Polylines   parts = diff_pl(Polylines{ piece.axis }, reach(piece.width));
        const Point in = piece.axis.first_point(), out = piece.axis.last_point();
        const auto  next = std::find_if(parts.begin(), parts.end(), [&](const Polyline &part) { return touches(part, in); });
        if (next != parts.end() && ! tail.axis.points.empty() && tail.axis.last_point() == in) {
            Polyline part = std::move(*next);
            parts.erase(next);
            if (part.first_point() != in)
                part.reverse();
            tail.axis.points.insert(tail.axis.points.end(), part.points.begin() + 1, part.points.end());
            tail.width = std::max(tail.width, piece.width);
            if (part.last_point() != out)
                close_tail();
        } else
            close_tail();
        for (Polyline &part : parts)
            if (touches(part, out)) {
                if (part.last_point() != out)
                    part.reverse();
                tail = BeadPiece{ std::move(part), piece.width };
            } else
                runs.push_back(BeadPiece{ std::move(part), piece.width });
    }
    // The seam of a bead closed on itself, whether or not its ends quite meet, joins its last run to its first.
    if (! tail.axis.points.empty() && closed) {
        const auto first = std::find_if(runs.begin(), runs.end(), [&](const BeadPiece &run) { return touches(run.axis, axis.first_point()); });
        if (first != runs.end()) {
            if (first->axis.first_point() != axis.first_point())
                first->axis.reverse();
            const bool meet = tail.axis.last_point() == first->axis.first_point();
            tail.axis.points.insert(tail.axis.points.end(), first->axis.points.begin() + (meet ? 1 : 0), first->axis.points.end());
            tail.width = std::max(tail.width, first->width);
            runs.erase(first);
        }
    }
    close_tail();
    return size_t(std::count_if(runs.begin(), runs.end(), [&](const BeadPiece &run) {
        return run_hangs(run.axis, axis, closed, double(run.width), max_span(double(run.width)));
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
                    // it carries every remaining component, so an incomplete grid cannot obstruct the fallback.
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
                        const Polygons footing = offset(union_(material), float(scaled<double>(0.01)));
                        if (std::all_of(loads.begin(), loads.end(), [&](const ExPolygon &part) {
                                return ! intersection(to_polygons(part), footing).empty(); })) {
                            printed = union_(material);
                            extrusion_entities_append_paths(plane.layer->support_fills.entities, std::move(anchors),
                                ExtrusionRole::erSupportMaterialInterface, flow.mm3_per_mm(), flow.width(), flow.height());
                            unheld.clear();
                            break;
                        }
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
    const Polygons close = this->around({ get_extents(part).inflated(coord_t(DefaultMiterLimit * grow) + SCALED_EPSILON) });
    return ! close.empty() && ! intersection(part, grow > 0.f ? offset(close, grow) : close).empty();
}

Polygons organic_bead(const Polyline &axis, double width)
{
    return offset(axis, float(0.5 * width), jtRound, SCALED_EPSILON, etOpenRound);
}

// Whether a part of the axis starts or ends at an end of the axis.
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

// Whether the flow of a support path of this role is set to nothing.
static bool support_role_has_no_flow(const PrintObjectConfig &config, const ExtrusionRole role)
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

struct ModelFootprint {
    bool planar_cached = false;
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

static std::vector<double> resolved_top_heights(const PrintObject &object, const Layer &layer, const PrintRegion &region,
    const ToolOrdering &ordering, int layer_index)
{
    const Print &print = *object.print();
    const PrintConfig &printer = print.config();
    const PrintRegionConfig &config = region.config();
    const LayerTools &tools = ordering.tools_for_layer(layer.print_z);
    const auto has_flow = [&](unsigned int filament) {
        size_t slot = print.get_extruder_id(filament);
        if (const auto groups = print.get_layered_nozzle_group_result()) {
            const auto nozzle = groups->get_nozzle_for_filament(int(filament), layer_index);
            // Match NOZZLE_CONFIG's published grouping and its missing-entry fallback.
            slot = nozzle ? size_t(get_config_index_base(nozzle->volume_type,
                ExtruderType(printer.extruder_type.get_at(nozzle->extruder_id)), nozzle->extruder_id + 1,
                config.print_extruder_variant.values, config.print_extruder_id.values)) : 0;
        }
        return config.top_solid_infill_flow_ratio.get_at(slot) != 0.;
    };
    const unsigned int slot = (tools.extruder_override == 0 ? config.top_surface_filament_id.value : int(tools.extruder_override)) - 1;
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

// Contoured paths may leave their nominal layer's Z band. Index their actual bounds, but construct planar
// footprints only when a root reaches that layer. An unprinted interior of a model outline provides no footing.
static Polygons model_material(const PrintObject &object, const Layer &layer, const double min_z, const double max_z,
    ModelFootprints &cache, const ToolOrdering *ordering)
{
    auto [it, inserted] = cache.layers.try_emplace(&layer);
    ModelFootprint &footprint = it->second;
    if (ordering != nullptr) {
        if (inserted) {
            footprint.min_z = footprint.max_z = layer.print_z;
            for (const LayerRegion *region : layer.regions()) {
                int layer_index = int(layer.id());
                if (cache.nozzle_layers != nullptr)
                    if (const auto index = cache.nozzle_layers->find(&layer); index != cache.nozzle_layers->end())
                        layer_index = index->second;
                const auto top_heights = resolved_top_heights(object, layer, region->region(), *ordering, layer_index);
                const auto visit = [&](const ExtrusionPath &path) {
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
                for_each_extrusion_path(region->perimeters, visit);
                for_each_extrusion_path(region->fills, visit);
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
    const auto visit_paths = [&](const auto &visitor) {
        for (const LayerRegion *region : layer.regions()) {
            const PrintRegionConfig &config = region->region().config();
            const bool top_has_flow = model_top_has_flow(config, *object.print());
            const auto visit_printed = [&](const ExtrusionPath &path) {
                if (model_path_has_flow(path, config, object.config(), layer.id() == 0 && std::abs(layer.bottom_z()) < EPSILON, top_has_flow))
                    visitor(path);
            };
            for_each_extrusion_path(region->perimeters, visit_printed);
            for_each_extrusion_path(region->fills, visit_printed);
        }
    };
    if (inserted) {
        footprint.min_z = footprint.max_z = layer.print_z;
        visit_paths([&](const ExtrusionPath &path) {
            if (path.z_contoured) {
                footprint.contoured.push_back(&path);
                for (const Point3 &point : path.polyline.points) {
                    const double z = layer.print_z + unscale<double>(point.z());
                    footprint.min_z = std::min(footprint.min_z, z);
                    footprint.max_z = std::max(footprint.max_z, z);
                }
            }
        });
    }
    if (footprint.max_z < min_z || footprint.min_z > max_z)
        return {};
    Polygons material;
    if (layer.print_z >= min_z && layer.print_z <= max_z) {
        if (!footprint.planar_cached) {
            visit_paths([&](const ExtrusionPath &path) {
                if (!path.z_contoured)
                    append(footprint.planar, organic_bead(path.as_polyline(), scale_(path.width)));
            });
            footprint.planar = union_(footprint.planar);
            footprint.planar_cached = true;
        }
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
// floating_islands().
static Polygons support_footing(const PrintObject &object, const std::vector<Polygons> &covered, const size_t i)
{
    return offset(support_below(object, covered, i), footing_reach);
}

// Where the axis of a bead width wide stands on footing: within half its width of it. trim_unsupported_base() joins the
// ends it cuts along the edge of that reach grown by SCALED_EPSILON, which holds them, and the reach takes such a join in
// with a margin of as much again: lying on its edge, rounding would leave the join over nothing.
static Polygons bead_reach(const Polygons &footing, const double width)
{
    return offset(footing, float(.5 * width + 2. * SCALED_EPSILON), jtRound, SCALED_EPSILON);
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
    void clip(const Polygons &footing)
    {
        if (! m_axes.empty())
            for (const Polyline &piece : diff_pl(m_axes, bead_reach(footing, double(m_narrowest))))
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
    ExPolygons     floating;
    for (ExPolygon &island : union_ex(covered[i])) {
        const Polygons part = to_polygons(island);
        bool rooted = footing.touch(part);
        // Most islands stand on support. Avoid constructing model footprints until one actually needs them.
        if (! rooted && ! model_checked) {
            add_model_footing();
            rooted = footing.touch(part);
        }
        if (! rooted)
            floating.emplace_back(std::move(island));
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

// The longest bridge between two bearings the support allows: a line between two lines of the base, the gap a roof spans
// between the bodies it lies on, see generate_support_areas(), or between two lines of a bottom interface, which the
// bodies standing on it span.
static double free_span(const PrintObjectConfig &config, const double width)
{
    const int bottom_layers = config.support_interface_bottom_layers.value < 0 ? config.support_interface_top_layers.value :
        config.support_interface_bottom_layers.value;
    return scale_(std::max(config.support_base_pattern_spacing.value,
        bottom_layers > 0 ? config.support_bottom_interface_spacing.value : 0.)) + width;
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

// The number of runs of the beads of layer i outside its floating islands that hang, see hanging_parts(), over the
// material below: the support below and, where the support may stand on it, the model. Sideways contact joins such a run
// to a rooted XY island but puts nothing below it, so the island check does not see it.
static size_t hanging_runs(const PrintObject &object, const std::vector<Polygons> &covered, const size_t i,
    const ExPolygons &floating, ModelFootprints &model, const ToolOrdering *ordering)
{
    const SupportLayer &layer  = *object.support_layers()[i];
    const auto         &config = object.config();
    // The raft prints lines of its own, spaced as it is set to.
    if (layer.print_z - layer.height < EPSILON || i < object.slicing_parameters().raft_layers() || !hollow_organic(config))
        return 0;
    Polygons                    footing     = support_footing(object, covered, i);
    bool                        model_added = config.support_on_build_plate_only;
    std::map<coord_t, Polygons> reach_by_width;
    size_t                      runs = 0;
    const auto reach = [&](const coord_t width) -> const Polygons & {
        auto [it, inserted] = reach_by_width.try_emplace(width);
        if (inserted)
            it->second = bead_reach(footing, double(width));
        return it->second;
    };
    const auto max_span = [&](const double width) { return free_span(config, width); };
    AxesOutsideReach outside;
    for_each_extrusion_path(layer.support_fills, [&](const ExtrusionPath &path) {
        if (flat_support(config, path))
            outside.add(path.as_polyline(), coord_t(scale_(path.width)));
    });
    outside.clip(footing);
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
                varying_bead_hanging_runs(pieces, axis, reach, max_span);
        };
        size_t found = hanging();
        // Most beads stand on support. Construct the model footing only once one needs it.
        if (found != 0 && ! model_added) {
            append(footing, offset(model_footing(object, i, model, ordering), footing_reach));
            reach_by_width.clear();
            model_added = true;
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
// outside all of its floating islands.
static void append_floating_heights(std::vector<coordf_t> &heights, const PrintObject &object,
    const std::vector<Polygons> &covered, const size_t i, const ExPolygons &floating, ModelFootprints &model,
    const ToolOrdering *ordering = nullptr, const double min_area = 0.)
{
    const size_t large = std::count_if(floating.begin(), floating.end(),
        [min_area](const ExPolygon &island) { return island.area() >= min_area; });
    heights.insert(heights.end(), large + hanging_runs(object, covered, i, floating, model, ordering),
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

// Stops a pass over all the layers once the slicing is cancelled: at thin layers such a pass runs for long.
static void throw_if_canceled(const PrintObject &object)
{
    if (object.print()->canceled())
        throw CanceledException();
}

std::vector<coordf_t> floating_support_islands(const PrintObject &object, const double min_area)
{
    if (on_belt(object))
        return {};
    const std::vector<Polygons> covered = support_covered(object);
    std::vector<coordf_t>       floating;
    ModelFootprints            model;
    for (size_t i = 0; i < covered.size(); ++ i) {
        throw_if_canceled(object);
        append_floating_heights(floating, object, covered, i, floating_islands(object, covered, i, model), model, nullptr,
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
    ModelFootprints model;
    model.nozzle_layers = &nozzle_layers;
    for (size_t i = 0; i < covered.size(); ++i) {
        throw_if_canceled(object);
        append_floating_heights(floating, object, covered, i, floating_islands(object, covered, i, model, &ordering), model,
            &ordering, min_area);
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

bool organic_support_needs_final_check(const PrintObject &object)
{
    const auto &config = object.config();
    const Print &print = *object.print();
    return !config.support_on_build_plate_only && !object.support_layers().empty() && hollow_organic(config) && !on_belt(object) &&
        (print.has_custom_tool_overrides() || has_any_mixed_filament(print.config().filament_is_mixed.values) ||
            (print.is_BBL_printer() && is_auto_filament_map_mode(print.get_filament_map_mode()) && top_flow_depends_on_nozzle(object)));
}

void organic_update_support_warning(PrintObject &object, std::vector<coordf_t> heights)
{
    std::sort(heights.begin(), heights.end());
    heights.erase(std::unique(heights.begin(), heights.end(), [](double a, double b) { return std::abs(a - b) < EPSILON; }), heights.end());
    std::string message;
    if (!heights.empty()) {
        constexpr size_t max_heights = 5;
        std::string list;
        for (size_t i = 0; i < std::min(heights.size(), max_heights); ++i)
            list += (i == 0 ? "" : ", ") + float_to_string_decimal_point(heights[i], 2);
        if (heights.size() > max_heights)
            list += ", ...";
        // TRN %1% is the object name; %2% lists the heights of the first unsupported islands in millimetres.
        message = Slic3r::format(_u8L("It seems the organic supports of object %1% have parts printed with nothing under them, "
            "starting on the layers at Z (mm): %2%. "
            "Consider a filled base pattern, such as Rectilinear, instead of a hollow one."), object.model_object()->name, list);
    }
    object.update_step_warning(posSupportMaterial, PrintStateBase::WarningLevel::NON_CRITICAL, message,
        PrintStateBase::SlicingOrganicSupportWarning);
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

void OrganicScarfFooting::record(const Layer &layer, const Point &origin, const ExtrusionPath &path,
    const double z, const bool extruding)
{
    auto it = m_objects.find(layer.object());
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
        const auto covered = support_covered(*object);
        const double sliver = organic_sliver_area(*object);
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
                append_floating_heights(heights, *object, covered, i, floating_islands(*object, covered, i, model), model, nullptr, sliver);
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

// Connect retained bearings without forbidden material or an excessive free span.
static bool connect_wall_bearings(const Point &a, const Point &b, const Polygons &footing,
    const Polygons &legal, bool limited, double max_bridge, Polyline &connection)
{
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
    double shortest = std::numeric_limits<double>::max();
    for (const Polygon &boundary : to_polygons(union_ex(limited ? intersection(footing, legal) : footing))) {
        const Polyline loop = boundary.split_at_first_point();
        Point start = a.projection_onto(loop), end = b.projection_onto(loop);
        if (start.distance_to(a) > 2. * SCALED_EPSILON || end.distance_to(b) > 2. * SCALED_EPSILON)
            continue;
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
    return shortest < std::numeric_limits<double>::max();
}

// Clip in source order: Clipper may otherwise reorder spliced inner loops.
static bool shorten_wall_axis(const Polyline &axis, const Polygons &cut, const Polygons &footing,
    const Polygons &legal, bool limited, double max_bridge, double closure_distance, Polyline &result)
{
    const BoundingBox bounds = get_extents(cut);
    const auto append_run = [&](const Polyline &part) {
        if (!result.empty() && result.last_point().distance_to(part.first_point()) > SCALED_EPSILON) {
            Polyline connection;
            if (!connect_wall_bearings(result.last_point(), part.first_point(), footing, legal, limited, max_bridge, connection))
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
        if (!connect_wall_bearings(result.last_point(), result.first_point(), footing, legal, limited, max_bridge, connection))
            return false;
        result.append(connection);
    }
    return true;
}

// Shorten unsupported base axes while preserving the rooted wall and its closure.
static size_t trim_unsupported_base(ExtrusionEntityCollection &collection, const Polygons &region,
    const Polygons &footing, const Polygons *legal, double bridge_spacing)
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
            trimmed += trim_unsupported_base(nested, region, footing, legal, bridge_spacing);
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
            Polyline repaired;
            if (!shorten_wall_axis(axis, clip.cut, clip.footing, clip.legal, legal != nullptr,
                    scale_(bridge_spacing) + double(width), double(width), repaired) ||
                axis.length() - repaired.length() <= SCALED_EPSILON) {
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
    std::vector<Polygons> covered = support_covered(object);
    size_t                removed = 0;
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
    for (size_t i = 0; i < covered.size(); ++ i) {
        throw_if_canceled(object);
        const SupportLayer &layer = *object.support_layers()[i];
        ExtrusionEntityCollection &fills = object.support_layers()[i]->support_fills;
        // The islands of the last pass, which drops none of them: those at least max_area large left on nothing.
        ExPolygons kept;
        bool changed;
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
            }
            // A dot of the base, a path no longer than it is wide, stands on what lies under its axis. One whose axis lies
            // over nothing hangs, though its edge touches what is below and a bead beside it keeps it in a rooted island:
            // hanging_parts() leaves runs that short out. Arachne lays such dots where a solid part runs out, over the slit
            // between the loops of the layer below. An interface is a contact, kept as a tongue's load is.
            if (hollow_organic(config) && layer.bottom_z() >= EPSILON) {
                Polygons footing;
                bool     support_added = false;
                bool     model_added   = config.support_on_build_plate_only;
                const size_t dots = remove_extrusions_if(fills, [&](const ExtrusionEntity &entity) {
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
                    if (! support_added) {
                        footing       = support_footing(object, covered, i);
                        support_added = true;
                    }
                    if (! intersection_pl(axes, footing).empty())
                        return false;
                    if (model_added)
                        return true;
                    append(footing, offset(model_footing(object, i, model, nullptr), footing_reach));
                    model_added = true;
                    return intersection_pl(axes, footing).empty();
                });
                if (dots != 0) {
                    removed += dots;
                    changed = true;
                }
            }
            if (trim_walls && layer.bottom_z() >= EPSILON) {
                Polygons footing = support_footing(object, covered, i);
                Polygons tongues;
                std::map<coord_t, Polygons> reach_by_width;
                // Where the support may stand on the model, a tongue over it stands on it: the model footing joins the support
                // below once a tongue is found without it.
                const auto find_tongues = [&]() {
                    const auto may_hang = [](const ExtrusionPath &path) {
                        return path.role() == erSupportMaterial && ! path.z_contoured && path.mm3_per_mm > 0. &&
                            path.polyline.points.size() >= 2 && path.length() > scale_(path.width);
                    };
                    AxesOutsideReach outside;
                    for_each_extrusion_path(fills, [&](const ExtrusionPath &path) {
                        if (may_hang(path))
                            outside.add(path.as_polyline(), scale_(path.width));
                    });
                    outside.clip(footing);
                    for_each_extrusion_path(fills, [&](const ExtrusionPath &path) {
                        if (! may_hang(path))
                            return;
                        const Polyline axis = path.as_polyline();
                        if (! outside.near(axis))
                            return;
                        const coord_t width = scale_(path.width);
                        auto [it, inserted] = reach_by_width.try_emplace(width);
                        if (inserted)
                            it->second = bead_reach(footing, double(width));
                        for (const Polyline &part : hanging_parts(axis, double(width), it->second,
                                 free_span(config, double(width))))
                            for (const ExPolygon &tongue : union_ex(organic_bead(part, double(width))))
                                if (config.support_on_build_plate_only ? unused_appendage(i, tongue) : carries_nothing(i, tongue))
                                    append(tongues, to_polygons(tongue));
                    });
                };
                find_tongues();
                if (! tongues.empty() && ! config.support_on_build_plate_only) {
                    append(footing, offset(model_footing(object, i, model, nullptr), footing_reach));
                    reach_by_width.clear();
                    tongues.clear();
                    find_tongues();
                }
                if (!tongues.empty()) {
                    Polygons legal = to_polygons(layer.support_islands);
                    Polygons interfaces;
                    if (!layer.support_islands.empty())
                        for_each_extrusion_path(fills, [&](const ExtrusionPath &path) {
                            if (path.role() == erSupportMaterialInterface)
                                append(interfaces, organic_bead(path.as_polyline(), scale_(path.width)));
                        });
                    if (!interfaces.empty())
                        legal = diff(legal, union_(interfaces));
                    const size_t trimmed = trim_unsupported_base(fills, union_(tongues), footing,
                        layer.support_islands.empty() ? nullptr : &legal, config.support_base_pattern_spacing.value);
                    removed += trimmed;
                    changed = changed || trimmed != 0;
                }
            }
            if (changed) {
                covered[i] = support_material(fills, &object, layer.id() == 0 && std::abs(layer.bottom_z()) < EPSILON);
                rooted_layers = std::min(rooted_layers, i);
            }
            // Clipping a tongue can disconnect tiny fill paths on the same layer.
        } while (changed);
        // The layers above leave this one as it is now.
        if (floating != nullptr)
            append_floating_heights(*floating, object, covered, i, kept, model);
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

// Organic: print the solid parts of a hollow body, the bodies widened under an interface, as spirals from their outline
// in, see organic_solid_fill(), and return the islands to be printed as walls. The spiral is Arachne's, its loops as
// wide as the part leaves room for: loops of a fixed width leave a slit up to a line wide where they run out, along the
// middle of a part mostly, and as the part widens going up, the loop the layer above starts there lies over the slit.
Polygons organic_extrude_solid_islands(ExtrusionEntitiesPtr &dst, const Polygons &polygons, const Polygons &solid, const Flow &flow,
    const PrintConfig &print_config, const PrintObjectConfig &object_config)
{
    if (solid.empty())
        return polygons;
    Polygons              walls;
    std::unique_ptr<Fill> filler;
    FillParams            params;
    params.density        = 1.f;
    params.dont_adjust    = false;
    params.use_arachne    = true;
    params.layer_height   = flow.height();
    params.extrusion_role = ExtrusionRole::erSupportMaterial;
    for (const ExPolygon &island : union_ex(polygons)) {
        bool     walled = true;
        Polygons fill   = organic_solid_fill(island, solid, flow.scaled_width(), walled);
        if (walled)
            append(walls, to_polygons(island));
        if (fill.empty())
            continue;
        if (! filler) {
            filler.reset(Fill::new_from_type(ipSpiralInset));
            filler->spacing             = flow.spacing();
            filler->print_config        = &print_config;
            filler->print_object_config = &object_config;
        }
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
    return walls;
}

} // namespace Slic3r
