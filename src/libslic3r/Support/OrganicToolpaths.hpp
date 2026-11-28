#pragma once

// The toolpath helpers of Organic tree supports: the axes of their interfaces, their walls, the solid parts of their
// bodies, and the parts of them standing on nothing.

#include "SupportLayer.hpp"
#include "SupportParameters.hpp"
#include "../BoundingBox.hpp"
#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../ExtrusionEntity.hpp"
#include "../Polygon.hpp"
#include "../Point.hpp"
#include "../Polyline.hpp"
#include "../PrintBase.hpp"
#include "../libslic3r.h"
#include "libslic3r/PrintConfig.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class ExtrusionEntityCollection;
class Fill;
class Flow;
class Layer;
class Print;
class PrintObject;
class SupportLayer;
class ToolOrdering;

// Reserve dense feet only in a sparse first-layer flange; fill a solid flange as one region. A foot is reserved only
// where what the first layer prints, island by island, would leave what it holds over nothing: a lone foot, which
// starts no tree, in part, the foot of a tree whole.
void organic_fill_first_layer(ExtrusionEntitiesPtr &dst, const Polygons &polygons, const Polygons &feet, const Polygons &lone_feet,
    Fill *filler, float density, ExtrusionRole role, const Flow &flow, const SupportParameters &support_params,
    const PrintObject &object);

// Print inner tree loops beside the material already printed on their layer, splicing loops touching a base wall into it.
// The walls are the support paths of dst, or those given where dst holds fill lines too, as a raft layer does.
void organic_generate_inner_tree_paths(ExtrusionEntitiesPtr &dst, const Polygons &trees, const Polygons &printed, const Flow &flow,
    const ExtrusionEntitiesPtr *walls = nullptr);

// Supply local interface paths beneath unheld base paths, carrying their load down through consecutive bottom
// interface layers. Uses actual extrusions so sparse interface polygons alone cannot terminate a tree in a gap.
// Take copies before toolpath ownership merges move or clear the original interface polygons.
std::vector<SupportGeneratorLayer> organic_bottom_interface_layers(const SupportGeneratorLayersPtr &interfaces);
void organic_anchor_bottom_interfaces(const std::vector<SupportLayer*> &layers,
    const std::vector<SupportGeneratorLayer> &interfaces, const SupportParameters &support_params);

// Contact obligations use the original overhang, independently of fill expansion.
Polylines organic_contact_axes(const ExPolygon &region, coord_t width);

// Complete narrow oriented regions of a dense Organic interface with legal
// fixed-width axes not already covered by material, the union of the beads of the chosen pattern.
Polylines organic_interface_gap_paths(const ExPolygon &region, const Polygons &material, coord_t width);

// The area a bead of the scaled width covers around its axis, with round ends.
Polygons organic_bead(const Polyline &axis, double width);

// The beads along the middle of the parts of Organic bodies the walls do not go round, with the widths they are printed
// with, their spacing: a wall cannot go round a part narrower than two lines without running back over its own bead.
// axis_regions are the regions half a line inside the bodies, which the first walls go round, and are left the parts of
// them the walls go round. A bead is a line wide at least where the body is a line wide, and on a strip narrower than
// a line it is wider than the strip by strip_widening at most, and as wide as the strip if strip_widening is not
// positive.
ThickPolylines organic_body_middles(const Polygons &bodies, ExPolygons &axis_regions, coord_t width,
    coordf_t strip_widening);

// Polygons with their bounding boxes, to clip a part only against those near it: a layer of support has thousands of
// parts, the loops of the trees inside its bodies, and clipping each against all the material of the layer would make
// the checks of the layer quadratic in them.
struct NearPolygons
{
    Polygons                 polygons;
    std::vector<BoundingBox> boxes;
    // How far the polygons reach beyond their outline, for touch().
    float                    grow = 0.f;

    void append(Polygons more);
    // The polygons whose boxes overlap any of the boxes. A hole lies inside its contour, so the contour of a hole near a
    // box is near it too.
    Polygons around(const std::vector<BoundingBox> &near) const;
    // Whether the part shares area with the polygons grown by grow.
    bool touch(const Polygons &part) const;
};

// The parts of a wall axis outside the axes already covered by fill, without dashes: covered
// stretches shorter than the bead width stay in the wall, and interior uncovered pieces shorter
// than it are left to the fill.
Polylines organic_uncovered_wall_parts(const Polyline &axis, const Polygons &covered_axes, coord_t width);

// The parts of a wall axis inside and outside region. A stretch shorter than min_length between two
// parts on the other side joins them, so that the wall does not change its role for a dot.
std::pair<Polylines, Polylines> organic_split_wall_axis(const Polyline &axis, const Polygons &region, coord_t min_length);

// The outlines of footing near box, to be offset by delta with join as the whole of footing is offset farther inside
// the box than the offset reaches from an outline, delta or mitered DefaultMiterLimit times delta: each edge of them
// meeting the box kept as it is, each point offset() keeps of them kept by offset() as it keeps it from footing, and
// nothing changed that comes within reach of such an edge grown. The polygons of footing may overlap; offset() is
// taken to grow them as one group, see c2_offset_as_group(), as it grows those of footing near the box.
Polygons organic_footing_near_box(const NearPolygons &footing, const BoundingBox &box, float delta, JoinType join);

// The outlines cut at box, see ClipperUtils::clip_clipper_polygons_with_subject_bbox(), but those that do not reach
// into it: each point of the box lies inside as many of them as of the outlines, and each edge meeting the box stays as
// it is.
Polygons organic_outlines_into_box(const Polygons &outlines, const BoundingBox &box);

// The part of an island of a hollow Organic body printed solid, given the solid polygons of its layer: all of an island
// mostly solid, a tip, printed with no wall, or else the solid part two lines inside its outline, where its walls lie.
// walled tells whether the island is printed with a wall.
Polygons organic_solid_fill(const ExPolygon &island, const Polygons &solid, coord_t width, bool &walled);
// What of it the trees inside the bodies take for solid material, see organic_grow_trees_inside_hollow_bodies(): all of
// an island printed with no wall, or the solid part where it is a line wide at least. The rest of the solid part, the
// slivers narrower than a line they take for none of it, is appended to slivers, if given, and the solid part of an
// island with slivers to parts, if given.
Polygons organic_solid_material(const ExPolygon &island, const Polygons &solid, coord_t width, bool &walled,
    Polygons *slivers = nullptr, Polygons *parts = nullptr);
// The slivers of the solid parts of all the islands of bodies, see organic_solid_material().
Polygons organic_solid_slivers(const Polygons &bodies, const Polygons &solid, coord_t width);

// What of the slivers of a hollow Organic layer its paths print, of the solid parts with slivers of the islands of its
// bodies given, see organic_solid_material() and organic_split_solid_islands(): the trees inside the bodies of the
// layer below carry it.
Polygons organic_printed_slivers(const Polygons &parts, const SupportGeneratorLayer &layer, coord_t width);

// The solid parts of the islands of polygons, the bodies of a hollow Organic layer or what its interfaces leave of
// them, as organic_solid_fill() finds them given the solid polygons of the layer, appended to fill: of an island
// printed with a wall, the pieces of it, given the layer, but those lying all in its slivers, see
// SupportGeneratorLayer::solid_slivers, which no tree of the layer reaches, and nothing of them over a tree of the
// layer, which prints its loops there; and none on the print bed. Returns the islands to be printed with a wall.
Polygons organic_split_solid_islands(const Polygons &polygons, const Polygons &solid, coord_t width, Polygons &fill,
    const SupportGeneratorLayer *layer = nullptr);
// Print fill, solid parts of a hollow Organic layer, into dst as spirals of the configured nozzle.
void organic_extrude_solid_fill(ExtrusionEntitiesPtr &dst, const Polygons &fill, const Flow &flow, const PrintConfig &print_config,
    const PrintObjectConfig &object_config);
// Print the solid parts of the islands of polygons, see organic_split_solid_islands(), into dst, see
// organic_extrude_solid_fill(), given their layer as organic_split_solid_islands() takes it. Returns the islands
// printed with a wall.
Polygons organic_extrude_solid_islands(ExtrusionEntitiesPtr &dst, const Polygons &polygons, const Polygons &solid, const Flow &flow,
    const PrintConfig &print_config, const PrintObjectConfig &object_config,
    const SupportGeneratorLayer *layer = nullptr);
// Whether the flow of a support path of this role is set to nothing: it prints nothing.
bool support_role_has_no_flow(const PrintObjectConfig &config, ExtrusionRole role);
// The gap a line of the support bridges between two lines of the base, scaled: their spacing for a filled base; for a
// hollow one, which lays none and so takes nothing from the base pattern spacing, one to three support lines by the
// layer height of the object, which the layers of the support keep above its first layer whatever its layer height
// profile; see the definition for why.
coord_t organic_base_span(const PrintObject &object);

// The islands of the support of an object, its extrusions with their width, standing on nothing: neither on the support
// of the layer below nor on the object within the bottom Z distance, or 0.1 mm, below. The first layer stands on the
// bed. Whole islands smaller than min_area are not counted. Returns the print Z of the layer of each island, going up; none on a
// belt printer, whose floor slopes. Before tool ordering, retain any possible material of an unresolved assignment.
// A hollow Organic support also counts the runs of its beads that hang beside a rooted island, as one joined to it
// sideways, inside a hollow body's cavity among them, with a bead-scale test instead of the island-area cutoff: longer
// than a bead and ending in the air or bent, or bridging straight farther than a line between two lines of the base.
std::vector<coordf_t> floating_support_islands(const PrintObject &object, double min_area);

// Use the final per-layer physical assignments (including mixed sublayers) instead of possible pre-ordering material.
// Merged layer printing supplies the config-layer IDs chosen by the exporter's first object layer at each Z;
// sequential printing uses the object's own layer IDs. A top of an object the prime tower may flush into counts with
// the filament that prints it, each copy the tower flushes into checked on its own.
std::vector<coordf_t> floating_support_islands(const PrintObject &object, double min_area, const ToolOrdering &ordering,
    const std::map<const Layer*, int> &nozzle_layers = {});
bool organic_support_needs_final_check(const PrintObject &object);
// Whether flushing into the object alone calls for the final check of its warning: switched off, flushing leaves a
// warning that only generating the support again finds anew.
bool organic_support_final_check_ends_with_flushing(const PrintObject &object);
// The warning that the Organic support of object stands on nothing in places, naming the object and the first heights
// of PrintObject::organic_support_heights(), or none where it keeps none.
std::string organic_support_warning_message(const PrintObject &object);
void organic_update_support_warning(PrintObject &object, std::vector<coordf_t> heights);
// The warning of an Organic support loaded with its object, which keeps none: what stands on nothing is found again as
// generating the support finds it, after dropping its crumbs.
void organic_update_loaded_support_warning(PrintObject &object);
// The warning of an object that takes over the layers of an identical one, see PrintObject::copy_layers_from_shared_object(),
// and so generates no support of its own: found on the layers it shares where the other object has one.
void organic_update_shared_support_warning(PrintObject &object);

// Scarf selection happens during export. With zero Scarf flow, record only material actually
// emitted by each instance, so conditional and partial seams cannot supply imaginary footing.
class OrganicScarfFooting
{
public:
    explicit OrganicScarfFooting(Print &print);
    bool empty() const { return m_objects.empty(); }
    // object is the object printed, which may share its layers with another, see PrintObject::copy_layers_from_shared_object().
    void record(const PrintObject &object, const Point &origin, const ExtrusionPath &path, double z, bool extruding);
    void update_warnings();
private:
    struct ObjectMaterial {
        PrintObject *object;
        double gap;
        std::multimap<double, size_t> planes;
        std::map<std::pair<coord_t, coord_t>, std::vector<Polygons>> instances;
    };
    std::map<const PrintObject*, ObjectMaterial> m_objects;
};

// Saved diagnostics of completed support steps, including slices reused from cache.
std::vector<PrintBase::SlicingStatus> organic_support_warning_statuses(const Print &print);

// The area below which an island of the support is a sliver the union of its beads leaves apart from them, not a bead:
// even a dot covers the disc of its width, about three quarters of the width squared.
double organic_sliver_area(const PrintObject &object);

// Drop the crumbs of the support of an object, the islands of its extrusions smaller than max_area standing on nothing,
// as floating_support_islands() finds them, going up so that a crumb standing on a crumb goes too. Bed-only base
// appendages that carry no contact before joining rooted support can also be dropped, and so can the dots of a hollow
// base, base paths no longer than they are wide, whose axis lies over nothing. Returning unloaded walls are shortened while
// preserving wall continuity. Returns the number of removed islands, dots and trimmed paths. With floating,
// also appends what is left standing on nothing as floating_support_islands(object, max_area) would find it, without
// going over the layers again.
size_t remove_floating_support_crumbs(PrintObject &object, double max_area, std::vector<coordf_t> *floating = nullptr);

// Organic: splice each loop whose axis lies within a line of a wall into the closest wall, which runs to the loop, around it
// and back: the bottom of a tree leaning into a wall then prints with it, beside the part of it already printed, rather
// than as a short path of its own after a travel. Returns the loops touching no wall.
Polylines organic_splice_loops_into_walls(Polylines &walls, Polylines loops, coord_t width);

// Organic, with a hollow base and the number of walls chosen by the area of a body: going up, mark the bodies of the base
// layers that get a second wall in SupportGeneratorLayer::double_walls, those large enough on the bed or the first layer
// and those whose second wall lies on a second wall below. A body's area alone flickers around the threshold from layer
// to layer, and a second wall over a body of one wall below prints in the air.
void organic_mark_double_walls(const SupportGeneratorLayersPtr &base_layers, const SupportParameters &support_params);

// Removes the paths of a collected support layer shorter than a quarter of their bead width and of the line width, except
// those joining two other paths: printed as a dot after a travel of its own, such a path adds next to nothing to the layer.
void organic_remove_dots(ExtrusionEntityCollection &collection, coord_t width);

} // namespace Slic3r
