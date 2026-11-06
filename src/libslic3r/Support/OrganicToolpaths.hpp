#pragma once

// The toolpath helpers of Organic tree supports: the axes of their interfaces, their walls, the solid parts of their
// bodies, and the parts of them standing on nothing.

#include "SupportLayer.hpp"
#include "SupportParameters.hpp"
#include "../BoundingBox.hpp"
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

// Reserve dense tree feet only in a sparse first-layer flange; fill a solid flange as one region.
void organic_fill_first_layer(ExtrusionEntitiesPtr &dst, const Polygons &polygons, const Polygons &feet,
    Fill *filler, float density, ExtrusionRole role, const Flow &flow, const SupportParameters &support_params);

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

// The part of an island of a hollow Organic body printed solid, given the solid polygons of its layer: all of an island
// mostly solid, a tip, printed with no wall, or else the solid part two lines inside its outline, where its walls lie.
// walled tells whether the island is printed with a wall.
Polygons organic_solid_fill(const ExPolygon &island, const Polygons &solid, coord_t width, bool &walled);

// Print the solid parts of the islands of polygons, the bodies of a hollow Organic layer, as organic_solid_fill() finds
// them given the solid polygons of the layer, into dst as spirals of the configured nozzle. Returns the islands printed
// with a wall.
Polygons organic_extrude_solid_islands(ExtrusionEntitiesPtr &dst, const Polygons &polygons, const Polygons &solid, const Flow &flow,
    const PrintConfig &print_config, const PrintObjectConfig &object_config);

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
// sequential printing uses the object's own layer IDs.
std::vector<coordf_t> floating_support_islands(const PrintObject &object, double min_area, const ToolOrdering &ordering,
    const std::map<const Layer*, int> &nozzle_layers = {});
bool organic_support_needs_final_check(const PrintObject &object);
void organic_update_support_warning(PrintObject &object, std::vector<coordf_t> heights);

// Scarf selection happens during export. With zero Scarf flow, record only material actually
// emitted by each instance, so conditional and partial seams cannot supply imaginary footing.
class OrganicScarfFooting
{
public:
    explicit OrganicScarfFooting(Print &print);
    bool empty() const { return m_objects.empty(); }
    void record(const Layer &layer, const Point &origin, const ExtrusionPath &path, double z, bool extruding);
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
