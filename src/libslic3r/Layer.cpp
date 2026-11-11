#include "Layer.hpp"
#include "ClipperUtils.hpp"
#include "Polygon.hpp"
#include "Point.hpp"
#include "ExPolygon.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Exception.hpp"
#include "Flow.hpp"
#include "Polyline.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "ShortestPath.hpp"
#include "SVG.hpp"
#include "BoundingBox.hpp"
#include "Surface.hpp"
#include "SurfaceCollection.hpp"
#include "libslic3r.h"
#include "Utils.hpp"

#include <algorithm>
#include <boost/log/trivial.hpp>
#include <vector>
#include <cstddef>
#include <utility>
#include <cassert>
#include <cmath>
#include <map>
#include <numeric>
#include "Config.hpp"
#include "MultiMaterialSegmentation.hpp"
#include "ObjectID.hpp"

namespace Slic3r {

Layer::~Layer()
{
    this->lower_layer = this->upper_layer = nullptr;
    for (LayerRegion *region : m_regions)
        delete region;
    m_regions.clear();
}

// Test whether whether there are any slices assigned to this layer.
bool Layer::empty() const
{
	for (const LayerRegion *layerm : m_regions)
        if (layerm != nullptr && ! layerm->slices.empty())
            // Non empty layer.
            return false;
    return true;
}

LayerRegion* Layer::add_region(const PrintRegion *print_region)
{
    m_regions.emplace_back(new LayerRegion(this, print_region));
    return m_regions.back();
}

// merge all regions' slices to get islands
void Layer::make_slices()
{
    ExPolygons slices;
    if (m_regions.size() == 1) {
        // optimization: if we only have one region, take its slices
        slices = to_expolygons(m_regions.front()->slices.surfaces);
    } else {
        Polygons slices_p;
        for (LayerRegion *layerm : m_regions)
            polygons_append(slices_p, to_polygons(layerm->slices.surfaces));
        slices = union_safety_offset_ex(slices_p);
    }

    this->lslices.clear();
    this->lslices.reserve(slices.size());

    // prepare ordering points
    Points ordering_points;
    ordering_points.reserve(slices.size());
    for (const ExPolygon &ex : slices)
        ordering_points.push_back(ex.contour.first_point());

    // sort slices
    std::vector<Points::size_type> order = chain_points(ordering_points);

    // populate slices vector
    for (size_t i : order)
        this->lslices.emplace_back(std::move(slices[i]));
}

static inline bool layer_needs_raw_backup(const Layer *layer)
{
    // BBS: backup raw slice for generating support
    //return ! (layer->regions().size() == 1 && (layer->id() > 0 || layer->object()->config().elefant_foot_compensation.value == 0));
    return true;
}

void Layer::backup_untyped_slices()
{
    if (layer_needs_raw_backup(this)) {
        for (LayerRegion *layerm : m_regions)
            layerm->raw_slices = to_expolygons(layerm->slices.surfaces);
    } else {
        assert(m_regions.size() == 1);
        m_regions.front()->raw_slices.clear();
    }
}

void Layer::restore_untyped_slices()
{
    if (layer_needs_raw_backup(this)) {
        for (LayerRegion *layerm : m_regions)
            layerm->slices.set(layerm->raw_slices, stInternal);
    } else {
        assert(m_regions.size() == 1);
        m_regions.front()->slices.set(this->lslices, stInternal);
    }
}

// Similar to Layer::restore_untyped_slices()
// To improve robustness of detect_surfaces_type() when reslicing (working with typed slices), see GH issue #7442.
// Only resetting layerm->slices if Slice::extra_perimeters is always zero or it will not be used anymore
// after the perimeter generator.
void Layer::restore_untyped_slices_no_extra_perimeters()
{
    if (layer_needs_raw_backup(this)) {
        for (LayerRegion *layerm : m_regions)
            //BBS: remove extra_perimeters. Always false
        	//if (! layerm->region().config().extra_perimeters.value)
            	layerm->slices.set(layerm->raw_slices, stInternal);
    } else {
    	assert(m_regions.size() == 1);
    	LayerRegion *layerm = m_regions.front();
    	// This optimization is correct, as extra_perimeters are only reused by prepare_infill() with multi-regions.
        //if (! layerm->region().config().extra_perimeters.value)
        	layerm->slices.set(this->lslices, stInternal);
    }
}

ExPolygons Layer::merged(float offset_scaled) const
{
	assert(offset_scaled >= 0.f);
    // If no offset is set, apply EPSILON offset before union, and revert it afterwards.
	float offset_scaled2 = 0;
	if (offset_scaled == 0.f) {
		offset_scaled  = float(  EPSILON);
		offset_scaled2 = float(- EPSILON);
    }
    Polygons polygons;
	for (LayerRegion *layerm : m_regions) {
		const PrintRegionConfig &config = layerm->region().config();
		// Our users learned to bend Slic3r to produce empty volumes to act as subtracters. Only add the region if it is non-empty.
		if (config.bottom_shell_layers > 0 || config.top_shell_layers > 0 || config.sparse_infill_density > 0. || config.wall_loops > 0)
			append(polygons, offset(layerm->slices.surfaces, offset_scaled));
	}
    ExPolygons out = union_ex(polygons);
	if (offset_scaled2 != 0.f)
		out = offset_ex(out, offset_scaled2);
    return out;
}

bool Layer::is_perimeter_compatible(const Print& print, const PrintRegion& a, const PrintRegion& b)
{
    const PrintRegionConfig& config       = a.config();
    const PrintRegionConfig& other_config = b.config();

        return config.outer_wall_filament_id       == other_config.outer_wall_filament_id
		&& config.inner_wall_filament_id       == other_config.inner_wall_filament_id
		&& config.wall_loops                  == other_config.wall_loops
		&& config.wall_sequence               == other_config.wall_sequence
		&& config.is_infill_first             == other_config.is_infill_first
		&& config.inner_wall_speed.get_at(print.get_extruder_id(config.outer_wall_filament_id)) == other_config.inner_wall_speed.get_at(print.get_extruder_id(config.outer_wall_filament_id))
		&& config.outer_wall_speed.get_at(print.get_extruder_id(config.outer_wall_filament_id)) == other_config.outer_wall_speed.get_at(print.get_extruder_id(config.outer_wall_filament_id))
		&& config.small_perimeter_speed.get_at(print.get_extruder_id(config.outer_wall_filament_id)) == other_config.small_perimeter_speed.get_at(print.get_extruder_id(config.outer_wall_filament_id))
		&& config.small_support_perimeter_speed.get_at(print.get_extruder_id(config.outer_wall_filament_id)) == other_config.small_support_perimeter_speed.get_at(print.get_extruder_id(config.outer_wall_filament_id))
        && config.gap_infill_speed.get_at(print.get_extruder_id(config.outer_wall_filament_id)) == other_config.gap_infill_speed.get_at(print.get_extruder_id(config.outer_wall_filament_id))
        && config.filter_out_gap_fill.value == other_config.filter_out_gap_fill.value
		&& config.detect_overhang_wall                   == other_config.detect_overhang_wall
		&& config.unsupported_wall_last                  == other_config.unsupported_wall_last
		&& config.overhang_reverse                       == other_config.overhang_reverse
		&& config.overhang_reverse_threshold             == other_config.overhang_reverse_threshold
		&& config.wall_direction                         == other_config.wall_direction
		&& config.opt_serialize("inner_wall_line_width") == other_config.opt_serialize("inner_wall_line_width")
		&& config.opt_serialize("outer_wall_line_width") == other_config.opt_serialize("outer_wall_line_width")
		&& config.detect_thin_wall                  == other_config.detect_thin_wall
		&& config.infill_wall_overlap              == other_config.infill_wall_overlap
        && config.top_bottom_infill_wall_overlap              == other_config.top_bottom_infill_wall_overlap
        // Orca: these flags directly change the effective wall count produced by the perimeter
        // generator. If two regions disagree on any of them, merging their slices into one shared make_perimeters
        // call would silently use the first region's flag for both.
        && config.only_one_wall_first_layer == other_config.only_one_wall_first_layer
        && config.only_one_wall_top         == other_config.only_one_wall_top
        && config.min_width_top_surface     == other_config.min_width_top_surface
        && config.seam_slope_type         == other_config.seam_slope_type
        && config.seam_slope_conditional == other_config.seam_slope_conditional
        && config.scarf_angle_threshold  == other_config.scarf_angle_threshold
        && config.scarf_overhang_threshold  == other_config.scarf_overhang_threshold
        && config.scarf_joint_speed       == other_config.scarf_joint_speed
        && config.scarf_joint_flow_ratio       == other_config.scarf_joint_flow_ratio
        && config.seam_slope_start_height == other_config.seam_slope_start_height
        && config.seam_slope_entire_loop  == other_config.seam_slope_entire_loop
        && config.seam_slope_min_length   == other_config.seam_slope_min_length
        && config.seam_slope_steps        == other_config.seam_slope_steps
        && config.seam_slope_inner_walls  == other_config.seam_slope_inner_walls;
}

// Orca: The parts of `slices` over the void wide enough to be bridged. Narrower ones, as the overhang a sloped wall
// leaves on each layer, are printed by the walls.
static ExPolygons bridged_overhangs(const Layer &layer, const ExPolygons &slices, coord_t wall_width)
{
    return layer.lower_layer == nullptr ? ExPolygons() : opening_ex(diff_ex(slices, layer.lower_layer->lslices), float(wall_width));
}

// Orca: Each region of a multi-material object, painted or a part of its own, is printed within its own outline, so a
// bridge segmented into a region of its own ends at its neighbours, where a single-material bridge reaches on over the
// supported area around it. Move a strip of the supported neighbours, as wide as their outer wall, to the bridge to
// anchor on, which the infill and the bridge detection then see. The walls of the neighbours are still generated over
// the strip, which is then left to the bridge, see Layer::make_perimeters().
void Layer::anchor_bridges_in_neighbours()
{
    // On the last layer every anchor would be a top surface.
    if (m_regions.size() < 2 || lower_layer == nullptr || upper_layer == nullptr)
        return;
    for (LayerRegion *bridge : m_regions) {
        const float      width     = float(bridge->flow(frExternalPerimeter).scaled_width());
        const ExPolygons slices    = to_expolygons(bridge->slices.surfaces);
        const ExPolygons overhangs = bridged_overhangs(*this, slices, coord_t(width));
        if (overhangs.empty())
            continue;
        // A top surface of a neighbour would show the bridge's filament. The slivers narrower than a wall the outline of
        // a sloped object leaves uncovered are not surfaces to see.
        const ExPolygons top = opening_ex(diff_ex(lslices, upper_layer->lslices), width / 2);
        // The strip replaces the outer wall of the neighbour, which it keeps along the outline of the object.
        ExPolygons anchors;
        for (const LayerRegion *neighbour : m_regions)
            if (neighbour != bridge && ! neighbour->slices.empty()) {
                const float neighbour_width = float(neighbour->flow(frExternalPerimeter).scaled_width());
                Polygons    reach           = intersection(offset(overhangs, neighbour_width), offset(lslices, -neighbour_width));
                if (! top.empty())
                    reach = diff(reach, top);
                append(anchors, intersection_ex(intersection_ex(neighbour->slices.surfaces, lower_layer->lslices), reach));
            }
        if (anchors.empty())
            continue;
        const Polygons inside = offset(lslices, -width);
        // The gaps narrower than a wall the anchors leave in the bridge, as of a thin line of a neighbour between two
        // bridges or of a crack between two regions, go to the bridge too, or they would be walled.
        const ExPolygons anchored = union_ex(slices, anchors);
        ExPolygons gaps = intersection_ex(diff_ex(closing_ex(anchored, width / 2), anchored), intersection(offset(anchors, width), inside));
        // A gap is kept or left whole: cutting it would leave a crack along the bridge, which would be walled.
        if (! top.empty())
            gaps.erase(std::remove_if(gaps.begin(), gaps.end(), [&top](const ExPolygon &gap) { return ! intersection_ex(gap, top).empty(); }),
                       gaps.end());
        append(anchors, std::move(gaps));
        for (LayerRegion *neighbour : m_regions)
            if (neighbour != bridge && ! neighbour->slices.empty())
                neighbour->slices.set(diff_ex(neighbour->slices.surfaces, anchors), stInternal);
        bridge->slices.set(union_ex(slices, anchors), stInternal);
    }
}

// Orca: The parts of the other regions of `layer` within reach of the walls of `layerms` along their overhangs. A bridge
// segmented into a region of its own would be walled in mid air along its neighbours, where a single-material bridge
// has no walls. Generating its walls over these parts too and removing what lands on them leaves the walls to the
// outline of the object, as on a single-material bridge.
static ExPolygons neighbours_along_overhangs(const Layer &layer, const LayerRegionPtrs &layerms)
{
    const LayerRegion &layerm     = *layerms.front();
    const int          wall_loops = layerm.region().config().wall_loops.value;
    if (layer.lower_layer == nullptr || wall_loops <= 0)
        return {};
    ExPolygons own;
    ExPolygons others;
    for (const LayerRegion *region : layer.regions())
        append(std::find(layerms.begin(), layerms.end(), region) == layerms.end() ? others : own, to_expolygons(region->slices.surfaces));
    const coord_t    wall_width = layerm.flow(frExternalPerimeter).scaled_width();
    const ExPolygons overhangs  = others.empty() ? ExPolygons() : bridged_overhangs(layer, own, wall_width);
    if (overhangs.empty())
        return {};
    // Far enough for the walls along the far side of these parts to stay off the anchor of the bridge, and for the
    // extra perimeters on overhangs to find the bridge anchored there, as on a single-material object.
    const float walls = float(wall_width + wall_loops * layerm.flow(frPerimeter).scaled_spacing());
    ExPolygons  out   = intersection_ex(others, offset(overhangs, walls + float(wall_width + scale_(EXTERNAL_INFILL_MARGIN))));
    // Only the neighbours the overhangs hang from, past the strip they may have handed over to anchor on.
    const Polygons along = offset(overhangs, float(2 * wall_width));
    out.erase(std::remove_if(out.begin(), out.end(), [&along](const ExPolygon &expoly) { return intersection(along, expoly).empty(); }), out.end());
    // Along the outline of the object, the neighbours may be thinner than the walls. Add the void past their outline,
    // so that the walls along it are removed too. The void past the outline of these regions keeps their walls.
    if (! out.empty())
        append(out, diff_ex(offset(out, walls), union_(to_polygons(layer.lslices), offset(own, float(wall_width / 2)))));
    return out;
}

// Orca: The runs of `polyline` outside `area`, in the order and the direction it is extruded in. Clipping a polyline
// crossing the boundary of `area` does not keep those, so it is clipped segment by segment.
static Polylines runs_outside(const Polyline &polyline, const Polygons &area)
{
    if (const Polylines whole = diff_pl(polyline, area); whole.empty() || std::abs(total_length(whole) - polyline.length()) < SCALED_EPSILON)
        return whole.empty() ? Polylines() : Polylines{ polyline };
    Polylines runs;
    for (size_t i = 1; i < polyline.size(); ++ i) {
        const Point &a      = polyline.points[i - 1];
        const Point &b      = polyline.points[i];
        const Vec2d  dir    = (b - a).cast<double>();
        Polylines    pieces = diff_pl(Polyline{ a, b }, area);
        for (Polyline &piece : pieces)
            if (dir.dot((piece.last_point() - piece.first_point()).cast<double>()) < 0.)
                piece.reverse();
        std::sort(pieces.begin(), pieces.end(), [&a, &dir](const Polyline &l, const Polyline &r) {
            return dir.dot((l.first_point() - a).cast<double>()) < dir.dot((r.first_point() - a).cast<double>());
        });
        for (Polyline &piece : pieces) {
            if (! runs.empty() && runs.back().last_point() == piece.first_point())
                runs.back().append(piece.last_point());
            else
                runs.emplace_back(std::move(piece));
        }
    }
    return runs;
}

// Orca: Removes the parts of the extrusions over `area`. One crossing its boundary is split into the runs left outside.
static void remove_extrusions_over(ExtrusionEntityCollection &collection, const Polygons &area)
{
    ExtrusionEntitiesPtr kept;
    for (ExtrusionEntity *entity : collection.entities) {
        if (auto *child = dynamic_cast<ExtrusionEntityCollection*>(entity)) {
            remove_extrusions_over(*child, area);
            if (child->empty())
                delete child;
            else
                kept.emplace_back(child);
            continue;
        }
        ExtrusionPaths paths;
        if (auto *loop = dynamic_cast<ExtrusionLoop*>(entity))
            paths = loop->paths;
        else if (auto *multi_path = dynamic_cast<ExtrusionMultiPath*>(entity))
            paths = multi_path->paths;
        else if (auto *path = dynamic_cast<ExtrusionPath*>(entity))
            paths.emplace_back(*path);
        std::vector<ExtrusionPaths> runs;
        double                      length = 0.;
        for (const ExtrusionPath &path : paths)
            for (Polyline &run : runs_outside(path.polyline.to_polyline(), area)) {
                length += run.length();
                if (runs.empty() || runs.back().back().last_point() != run.first_point())
                    runs.emplace_back();
                runs.back().emplace_back(Polyline3(run), path);
            }
        if (paths.empty() || std::abs(length - entity->length()) < SCALED_EPSILON) {
            kept.emplace_back(entity);
            continue;
        }
        // A loop cut away from its start runs on from its last run into its first one.
        if (entity->is_loop() && runs.size() > 1 && runs.back().back().last_point() == runs.front().front().first_point()) {
            append(runs.back(), std::move(runs.front()));
            runs.front() = std::move(runs.back());
            runs.pop_back();
        }
        for (ExtrusionPaths &run : runs) {
            const double run_length = std::accumulate(run.begin(), run.end(), 0., [](double l, const ExtrusionPath &p) { return l + p.length(); });
            // A stub no longer than it is wide would only leave a blob.
            if (run_length <= scale_(run.front().width))
                continue;
            ExtrusionEntity *clipped;
            if (run.size() == 1) {
                clipped = new ExtrusionPath(std::move(run.front()));
            } else {
                auto *multi_path  = new ExtrusionMultiPath();
                multi_path->paths = std::move(run);
                clipped           = multi_path;
            }
            clipped->inset_idx = entity->inset_idx;
            kept.emplace_back(clipped);
        }
        delete entity;
    }
    collection.entities = std::move(kept);
}

// Here the perimeters are created cummulatively for all layer regions sharing the same parameters influencing the perimeters.
// The perimeter paths and the thin fills (ExtrusionEntityCollection) are assigned to the first compatible layer region.
// The resulting fill surface is split back among the originating regions.
void Layer::make_perimeters()
{
    BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id();

    const auto clear_generated_extrusions = [](LayerRegion *layer_region) {
        layer_region->perimeters.clear();
        layer_region->fills.clear();
        layer_region->thin_fills.clear();
    };

    // keep track of regions whose perimeters we have already generated
    std::vector<unsigned char> done(m_regions.size(), false);

    for (LayerRegionPtrs::iterator layerm = m_regions.begin(); layerm != m_regions.end(); ++ layerm)
    	if ((*layerm)->slices.empty()) {
 			(*layerm)->perimeters.clear();
 			(*layerm)->fills.clear();
 			(*layerm)->thin_fills.clear();
    	} else {
	        size_t region_id = layerm - m_regions.begin();
	        if (done[region_id])
	            continue;
	        BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id() << ", region " << region_id;
	        done[region_id] = true;
	        const PrintRegion &this_region = (*layerm)->region();

	        // find compatible regions
	        LayerRegionPtrs layerms;
	        layerms.push_back(*layerm);
	        for (LayerRegionPtrs::const_iterator it = layerm + 1; it != m_regions.end(); ++it)
	            if (! (*it)->slices.empty()) {
		            LayerRegion* other_layerm = *it;
		            const PrintRegion &other_region = other_layerm->region();
                    // Per-part gradient tags a region with its owning ModelVolume; merging two
                    // differently-tagged regions would collapse volumes that need independent
                    // gradient runs. Both tags are invalid unless per-part gradient is on, so
                    // this is a no-op for every other configuration.
                    if (this_region.gradient_volume_id() != other_region.gradient_volume_id())
                        continue;
                    if (is_perimeter_compatible(*m_object->print(), this_region, other_region))
                    {
                        clear_generated_extrusions(other_layerm);
                        layerms.push_back(other_layerm);
                        done[it - m_regions.begin()] = true;
                    }
		        }

	        // Orca: The walls of these regions are generated over the neighbours along their bridges and over the strips they
	        // handed to bridges of other regions to anchor on, then removed from there.
	        ExPolygons clipped = neighbours_along_overhangs(*this, layerms);
	        Polygons   group_slices;
	        for (const LayerRegion *l : layerms)
	            polygons_append(group_slices, to_polygons(l->slices.surfaces));
	        for (const LayerRegion *l : layerms)
	            if (const ExPolygons slices = to_expolygons(l->slices.surfaces); slices != l->raw_slices)
	                // A strip handed to a region of this group keeps the walls they share.
	                append(clipped, diff_ex(l->raw_slices, group_slices));
	        clipped = union_ex(clipped);
	        if (layerms.size() == 1 && clipped.empty()) {  // optimization
	            (*layerm)->fill_surfaces.surfaces.clear();
                (*layerm)->make_perimeters((*layerm)->slices, {*layerm}, &(*layerm)->fill_surfaces, &(*layerm)->fill_no_overlap_expolygons);
	            (*layerm)->fill_expolygons = to_expolygons((*layerm)->fill_surfaces.surfaces);
	        } else {
	            // Orca: Unlike the compatible regions above, the initiating region has not
	            // been cleared yet and may contain paths from a previous incompatible run.
	            clear_generated_extrusions(*layerm);

	            SurfaceCollection new_slices;
	            // Use the region with highest infill rate, as the make_perimeters() function below decides on the gap fill based on the infill existence.
	            LayerRegion *layerm_config = layerms.front();
	            {
	                // group slices (surfaces) according to number of extra perimeters
	                std::map<unsigned short, Surfaces> slices;  // extra_perimeters => [ surface, surface... ]
	                for (LayerRegion *layerm : layerms) {
	                    for (const Surface &surface : layerm->slices.surfaces)
	                        slices[surface.extra_perimeters].emplace_back(surface);
	                    if (layerm->region().config().sparse_infill_density > layerm_config->region().config().sparse_infill_density)
	                    	layerm_config = layerm;
	                }
	                for (const ExPolygon &expoly : clipped)
	                    slices.begin()->second.emplace_back(slices.begin()->second.front(), expoly);
	                // merge the surfaces assigned to each group
	                for (std::pair<const unsigned short,Surfaces> &surfaces_with_extra_perimeters : slices)
	                    new_slices.append(offset_ex(surfaces_with_extra_perimeters.second, ClipperSafetyOffset), surfaces_with_extra_perimeters.second.front());
	            }

	            // make perimeters
	            SurfaceCollection fill_surfaces;
                //BBS
                ExPolygons fill_no_overlap;
	            layerm_config->make_perimeters(new_slices, layerms, &fill_surfaces, &fill_no_overlap);
	            if (! clipped.empty()) {
	                // The fill there is dropped below, with the fill outside the slices of these regions.
	                remove_extrusions_over(layerm_config->perimeters, to_polygons(clipped));
	                remove_extrusions_over(layerm_config->thin_fills, to_polygons(clipped));
	            }

	            // assign fill_surfaces to each layer, clearing those of a previous run first, which an empty fill would keep
	            for (LayerRegion *l : layerms) {
	                l->fill_expolygons.clear();
	                l->fill_surfaces.clear();
	                l->fill_no_overlap_expolygons.clear();
	            }
	            if (!fill_surfaces.surfaces.empty()) {
	                for (LayerRegionPtrs::iterator l = layerms.begin(); l != layerms.end(); ++l) {
	                    // Separate the fill surfaces.
	                    ExPolygons expp = intersection_ex(fill_surfaces.surfaces, (*l)->slices.surfaces);
	                    (*l)->fill_expolygons = expp;
	                    (*l)->fill_surfaces.set(std::move(expp), fill_surfaces.surfaces.front());
                        //BBS: Separate fill_no_overlap
                        (*l)->fill_no_overlap_expolygons = intersection_ex((*l)->slices.surfaces, fill_no_overlap);
	                }

	                // When counterbore hole bridging (chbFilled) is active, process_no_bridge may
	                // create fill surfaces that extend beyond all region slices (e.g. by clearing
	                // holes in the bridge expolygon). These "extra" fills are lost during the
	                // intersection-based splitting above. Recover them and assign to the first
	                // merged region so the sacrificial bridge layer is not broken.
	                if (layerm_config->region().config().counterbore_hole_bridging.value != chbNone) {
	                    Polygons all_region_slices_p;
	                    for (LayerRegion *l : layerms)
	                        polygons_append(all_region_slices_p, to_polygons(l->slices.surfaces));
	                    polygons_append(all_region_slices_p, to_polygons(clipped));
	                    ExPolygons extra_fill = diff_ex(fill_surfaces.surfaces, all_region_slices_p, ApplySafetyOffset::Yes);
	                    if (!extra_fill.empty()) {
	                        append(layerms.front()->fill_expolygons, extra_fill);
	                        layerms.front()->fill_expolygons = union_ex(layerms.front()->fill_expolygons);
	                        layerms.front()->fill_surfaces.append(std::move(extra_fill), fill_surfaces.surfaces.front());
	                    }
	                }
	            }
	        }
	    }
    BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id() << " - Done";
}

void Layer::export_region_slices_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (const auto *region : m_regions)
        for (const auto &surface : region->slices.surfaces)
            bbox.merge(get_extents(surface.expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const auto *region : m_regions)
        for (const auto &surface : region->slices.surfaces)
            svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type), transparency);
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void Layer::export_region_slices_to_svg_debug(const char *name) const
{
    static size_t idx = 0;
    this->export_region_slices_to_svg(debug_out_path("Layer-slices-%s-%d.svg", name, idx ++).c_str());
}

void Layer::export_region_fill_surfaces_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (const auto *region : m_regions)
        for (const auto &surface : region->slices.surfaces)
            bbox.merge(get_extents(surface.expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const auto *region : m_regions)
        for (const auto &surface : region->slices.surfaces)
            svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type), transparency);
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

//BBS: method to simplify support path
void Layer::simplify_support_entity_collection(ExtrusionEntityCollection* entity_collection)
{
    for (size_t i = 0; i < entity_collection->entities.size(); i++) {
        if (ExtrusionEntityCollection* collection = dynamic_cast<ExtrusionEntityCollection*>(entity_collection->entities[i]))
            this->simplify_support_entity_collection(collection);
        else if (ExtrusionPath* path = dynamic_cast<ExtrusionPath*>(entity_collection->entities[i]))
            this->simplify_support_path(path);
        else if (ExtrusionMultiPath* multipath = dynamic_cast<ExtrusionMultiPath*>(entity_collection->entities[i]))
            this->simplify_support_multi_path(multipath);
        else if (ExtrusionLoop* loop = dynamic_cast<ExtrusionLoop*>(entity_collection->entities[i]))
            this->simplify_support_loop(loop);
        else
            throw Slic3r::InvalidArgument("Invalid extrusion entity supplied to simplify_support_entity_collection()");
    }
}
//BBS: method to simplify support path
void Layer::simplify_support_path(ExtrusionPath * path)
{
    const auto &print_config = this->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting.value == ArcFittingMode::Enabled;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    if (enable_arc_fitting &&
        !spiral_mode) {
        path->simplify_by_fitting_arc(SCALED_SUPPORT_RESOLUTION);
    } else {
        path->simplify(scaled_resolution);
    }
}
//BBS: method to simplify support path
void Layer::simplify_support_multi_path(ExtrusionMultiPath* multipath)
{
    const auto &print_config = this->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting.value == ArcFittingMode::Enabled;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    for (size_t i = 0; i < multipath->paths.size(); ++i) {
        if (enable_arc_fitting &&
            !spiral_mode) {
            multipath->paths[i].simplify_by_fitting_arc(SCALED_SUPPORT_RESOLUTION);
        } else {
            multipath->paths[i].simplify(scaled_resolution);
        }
    }
}
//BBS: method to simplify support path
void Layer::simplify_support_loop(ExtrusionLoop* loop)
{
    const auto &print_config = this->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting.value == ArcFittingMode::Enabled;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    for (size_t i = 0; i < loop->paths.size(); ++i) {
        if (enable_arc_fitting &&
            !spiral_mode) {
            loop->paths[i].simplify_by_fitting_arc(SCALED_SUPPORT_RESOLUTION);
        } else {
            loop->paths[i].simplify(scaled_resolution);
        }
    }
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void Layer::export_region_fill_surfaces_to_svg_debug(const char *name) const
{
    static size_t idx = 0;
    this->export_region_fill_surfaces_to_svg(debug_out_path("Layer-fill_surfaces-%s-%d.svg", name, idx ++).c_str());
}

coordf_t Layer::get_sparse_infill_max_void_area()
{
    double max_void_area = 0.;
    for (auto layerm : m_regions) {
        Flow flow = layerm->flow(frInfill);
        const PrintRegionConfig &config = layerm->region().config();
        float density = config.sparse_infill_density;
        InfillPattern pattern = config.sparse_infill_pattern;
        if (density == 0.)
            return -1;
        // Orca: the adaptive TPMS infill is as sparse as its interior density.
        if (density < 100.f && config.adaptive_infill_mode != AdaptiveInfillMode::Disabled && is_adaptive_infill_pattern(pattern))
            density = std::min(density, std::max(1.f, float(config.adaptive_infill_interior_density)));

        //BBS: rough estimation and need to be optimized
        double spacing = flow.scaled_spacing() * (100 - density) / density;
        switch (pattern) {
            case ipConcentric:
            case ipSpiralInset:
            case ipRectilinear:
            case ipLine:
            case ipGyroid:
            case ipTpmsD:
            case ipTpmsFK:
            case ipAlignedRectilinear:
            case ipOctagramSpiral:
            case ipHilbertCurve:
            case ipLateralHoneycomb:
            case ip3DHoneycomb:
            case ipArchimedeanChords:
                max_void_area = std::max(max_void_area, spacing * spacing);
                break;
            case ipGrid:
            case ipLateralLattice:
            case ipHoneycomb:
            case ipLightning:
                max_void_area = std::max(max_void_area, 4.0 * spacing * spacing);
                break;
            case ipCubic:
            case ipAdaptiveCubic:
            case ipTriangles:
            case ipStars:
            case ipSupportCubic:
                max_void_area = std::max(max_void_area, 4.5 * spacing * spacing);
                break;
            default:
                max_void_area = std::max(max_void_area, spacing * spacing);
                break;
        }
    };
    return max_void_area;
}

size_t Layer::get_extruder_id(unsigned int filament_id) const
{
    return m_object->print()->get_extruder_id(filament_id);
}

BoundingBox get_extents(const LayerRegion &layer_region)
{
    BoundingBox bbox;
    if (!layer_region.slices.surfaces.empty()) {
        bbox = get_extents(layer_region.slices.surfaces.front());
        for (auto it = layer_region.slices.surfaces.cbegin() + 1; it != layer_region.slices.surfaces.cend(); ++it)
            bbox.merge(get_extents(*it));
    }
    return bbox;
}

BoundingBox get_extents(const LayerRegionPtrs &layer_regions)
{
    BoundingBox bbox;
    if (!layer_regions.empty()) {
        bbox = get_extents(*layer_regions.front());
        for (auto it = layer_regions.begin() + 1; it != layer_regions.end(); ++it)
            bbox.merge(get_extents(**it));
    }
    return bbox;
}

}
