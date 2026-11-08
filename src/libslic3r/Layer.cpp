#include "Layer.hpp"
#include "ClipperUtils.hpp"
#include "Polygon.hpp"
#include "Point.hpp"
#include "ExPolygon.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Exception.hpp"
#include "Flow.hpp"
#include "Line.hpp"
#include "Polyline.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "ShortestPath.hpp"
#include "SVG.hpp"
#include "BoundingBox.hpp"
#include "Surface.hpp"
#include "libslic3r.h"
#include "Utils.hpp"

#include <algorithm>
#include <boost/log/trivial.hpp>
#include <vector>
#include <cstddef>
#include <utility>
#include <cassert>
#include <cmath>
#include <limits>
#include <map>
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

// Orca: Position of `pt` along `polyline`, as the index of the closest segment plus the covered fraction of it.
static double position_along(const Polyline &polyline, const Point &pt)
{
    double best_dist2 = std::numeric_limits<double>::max();
    double best_pos   = 0.;
    for (size_t i = 1; i < polyline.points.size(); ++ i) {
        const Line   segment(polyline.points[i - 1], polyline.points[i]);
        const Point  foot  = pt.projection_onto(segment);
        const double dist2 = (foot - pt).cast<double>().squaredNorm();
        if (dist2 < best_dist2) {
            const double length = segment.length();
            best_dist2 = dist2;
            best_pos   = double(i - 1) + (length > 0. ? (foot - segment.a).cast<double>().norm() / length : 0.);
        }
    }
    return best_pos;
}

// Orca: The parts of `path` outside `area`, in the order and the direction they are extruded in, which clipping
// does not preserve.
static ExtrusionPaths extrusion_path_outside(const ExtrusionPath &path, const Polygons &area)
{
    const Polyline polyline = path.polyline.to_polyline();
    std::vector<std::pair<double, Polyline>> pieces;
    for (Polyline &piece : diff_pl(polyline, area)) {
        if (piece.size() < 2)
            continue;
        // Sample inside the end segments, as the end points of a closed path sit at both of its ends.
        double start = position_along(polyline, lerp(piece.points[0], piece.points[1], 0.25));
        double end   = position_along(polyline, lerp(piece.points[piece.size() - 2], piece.points.back(), 0.75));
        if (end < start) {
            piece.reverse();
            std::swap(start, end);
        }
        pieces.emplace_back(start, std::move(piece));
    }
    std::sort(pieces.begin(), pieces.end(), [](const auto &l, const auto &r) { return l.first < r.first; });

    ExtrusionPaths out;
    out.reserve(pieces.size());
    for (auto &piece : pieces)
        out.emplace_back(Polyline3(piece.second), path);
    return out;
}

// Orca: Removes the parts of the extrusions lying over `area`. A wall crossing its boundary is split into the
// runs left outside, each printed as a path of its own.
static void remove_extrusions_over(ExtrusionEntityCollection &collection, const Polygons &area, bool include_width = false)
{
    ExtrusionEntitiesPtr kept;
    kept.reserve(collection.entities.size());
    for (ExtrusionEntity *entity : collection.entities) {
        if (auto *child = dynamic_cast<ExtrusionEntityCollection*>(entity)) {
            remove_extrusions_over(*child, area, include_width);
            if (child->empty())
                delete child;
            else
                kept.emplace_back(child);
            continue;
        }
        const ExtrusionPaths *source = nullptr;
        ExtrusionPaths        single;
        if (auto *loop = dynamic_cast<ExtrusionLoop*>(entity))
            source = &loop->paths;
        else if (auto *multi_path = dynamic_cast<ExtrusionMultiPath*>(entity))
            source = &multi_path->paths;
        else if (auto *path = dynamic_cast<ExtrusionPath*>(entity)) {
            single.emplace_back(*path);
            source = &single;
        }
        if (source == nullptr) {
            kept.emplace_back(entity);
            continue;
        }

        bool          unchanged = true;
        ExtrusionPaths pieces;
        for (const ExtrusionPath &path : *source) {
            ExtrusionPaths outside = extrusion_path_outside(path, include_width ?
                offset(area, scale_(path.width / 2.) + SCALED_EPSILON) : area);
            unchanged &= outside.size() == 1 && std::abs(outside.front().length() - path.length()) < SCALED_EPSILON;
            append(pieces, std::move(outside));
        }
        if (unchanged) {
            kept.emplace_back(entity);
            continue;
        }

        std::vector<ExtrusionPaths> runs;
        for (ExtrusionPath &piece : pieces) {
            if (runs.empty() || runs.back().back().last_point() != piece.first_point())
                runs.emplace_back();
            runs.back().emplace_back(std::move(piece));
        }
        // A loop cut away from its start point runs on from its last piece into its first one.
        if (entity->is_loop() && runs.size() > 1 && runs.back().back().last_point() == runs.front().front().first_point()) {
            append(runs.back(), std::move(runs.front()));
            runs.front() = std::move(runs.back());
            runs.pop_back();
        }
        for (ExtrusionPaths &run : runs) {
            double length = 0.;
            for (const ExtrusionPath &path : run)
                length += path.length();
            // A stub no longer than it is wide would only leave a blob.
            if (length <= scale_(run.front().width))
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

void Layer::reserve_bridge_anchors()
{
    if (m_regions.size() < 2 || lower_layer == nullptr)
        return;
    const auto bridge_footprint_offset = [this](const LayerRegion &region) {
        const Flow flow = region.bridging_flow(frSolidInfill, object()->config().thick_bridges);
        return float(scale_(std::max(0., (double(flow.width()) - flow.spacing()) / 2.)) + SCALED_EPSILON);
    };
    // Region workers have finished: assigning anchors here avoids racing their surface reads. All anchors are
    // collected before any bridge is extended below.
    std::vector<Polygons> reservations(m_regions.size());
    for (size_t region_idx = 0; region_idx < m_regions.size(); ++region_idx) {
        const Polygons region_slices = to_polygons(m_regions[region_idx]->slices.surfaces);
        for (const LayerRegion *other : m_regions)
            if (other != m_regions[region_idx])
                for (const Surface &surface : other->fill_surfaces.surfaces)
                    if (surface.surface_type == stBottomBridge)
                        polygons_append(reservations[region_idx],
                                        intersection(offset(surface.expolygon, bridge_footprint_offset(*other)), region_slices));
    }
    for (size_t region_idx = 0; region_idx < m_regions.size(); ++region_idx) {
        LayerRegion *region = m_regions[region_idx];
        const Polygons anchors = union_(reservations[region_idx]);
        if (anchors.empty())
            continue;
        m_bridge_anchors_reserved = true;
        Polygons removed_walls = region->perimeters.polygons_covered_by_width();
        remove_extrusions_over(region->perimeters, anchors, true);
        remove_extrusions_over(region->thin_fills, anchors, true);
        const Polygons kept_walls = region->perimeters.polygons_covered_by_width();
        removed_walls = intersection(diff(removed_walls, kept_walls), to_polygons(lower_layer->lslices));

        Surfaces surfaces;
        for (const Surface &surface : region->fill_surfaces.surfaces)
            for (ExPolygon &expoly : diff_ex(ExPolygons{surface.expolygon}, anchors))
                surfaces.emplace_back(surface, std::move(expoly));
        region->fill_surfaces.surfaces = std::move(surfaces);
        region->fill_expolygons = diff_ex(region->fill_expolygons, anchors);
        region->fill_no_overlap_expolygons = diff_ex(region->fill_no_overlap_expolygons, anchors);

        // The walls touching an anchor are removed whole. Extend the bridge over their place up to the walls kept,
        // so the anchor is as wide all along the boundary and leaves no strip to be filled with gap fill.
        const Polygons freed = diff(union_(removed_walls, anchors), to_polygons(region->fill_surfaces.surfaces));
        for (LayerRegion *other : m_regions) {
            if (other == region)
                continue;
            const Polygons extension = diff(freed, offset(kept_walls, bridge_footprint_offset(*other)));
            Surfaces extended;
            for (Surface &surface : other->fill_surfaces.surfaces) {
                if (surface.surface_type != stBottomBridge)
                    continue;
                ExPolygons merged = closing_ex(union_ex(union_(to_polygons(surface.expolygon), extension)), float(SCALED_EPSILON));
                bool first = true;
                for (ExPolygon &expoly : intersection_ex(merged, lslices)) {
                    if (intersection_ex(ExPolygons{expoly}, ExPolygons{surface.expolygon}).empty())
                        continue;
                    if (first) {
                        surface.expolygon = std::move(expoly);
                        first = false;
                    } else
                        extended.emplace_back(surface, std::move(expoly));
                }
            }
            append(other->fill_surfaces.surfaces, std::move(extended));
        }
    }
}

// Orca: The parts of the other regions of `layer` within reach of the walls of `layerms` where those hang over the
// void. The regions of a multi-material object each get walls along their own outline, so a bridge segmented into
// another filament would be walled in mid air along its neighbours, where a single-material bridge has no walls
// and is bridged over with infill. Generating the walls over these parts too and removing what lands on them
// keeps the walls there to the outline of the object, as on a single-material bridge.
static ExPolygons overhanging_neighbour_areas(const Layer &layer, const LayerRegionPtrs &layerms)
{
    const LayerRegion &layerm     = *layerms.front();
    const int          wall_loops = layerm.region().config().wall_loops.value;
    if (layer.lower_layer == nullptr || wall_loops <= 0)
        return {};

    Polygons own;
    Polygons others;
    for (const LayerRegion *region : layer.regions())
        polygons_append(std::find(layerms.begin(), layerms.end(), region) == layerms.end() ? others : own,
                        to_polygons(region->slices.surfaces));
    if (others.empty())
        return {};

    const Flow ext_perimeter_flow = layerm.flow(frExternalPerimeter);
    // Narrow overhangs, such as the one a sloped wall leaves on each layer, are printed as walls anyway.
    const ExPolygons overhangs = opening_ex(diff_ex(own, layer.lower_layer->lslices), float(ext_perimeter_flow.scaled_width()));
    if (overhangs.empty())
        return {};
    // Far enough for the walls along the far side of these parts to stay off the own slices.
    const float reach = float(ext_perimeter_flow.scaled_width() + wall_loops * layerm.flow(frPerimeter).scaled_spacing());
    return intersection_ex(others, offset(overhangs, reach));
}

// Here the perimeters are created cummulatively for all layer regions sharing the same parameters influencing the perimeters.
// The perimeter paths and the thin fills (ExtrusionEntityCollection) are assigned to the first compatible layer region.
// The resulting fill surface is split back among the originating regions.
void Layer::make_perimeters()
{
    m_bridge_anchors_reserved = false;
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

	        const ExPolygons neighbours = overhanging_neighbour_areas(*this, layerms);
	        ExPolygons bridge_exclusions;
	        if (!neighbours.empty()) {
	            Polygons own_slices;
	            for (const LayerRegion *region : layerms)
	                polygons_append(own_slices, to_polygons(region->slices.surfaces));
	            for (ExPolygon &overhang : diff_ex(own_slices, lower_layer->lslices))
	                if (!intersection_ex(offset_ex(overhang, float(SCALED_EPSILON)), neighbours).empty())
	                    bridge_exclusions.emplace_back(std::move(overhang));
	        }
	        const auto remove_walls_over_neighbours = [&neighbours](LayerRegion &layerm_out) {
	            const Polygons area = to_polygons(neighbours);
	            remove_extrusions_over(layerm_out.perimeters, area);
	            remove_extrusions_over(layerm_out.thin_fills, area);
	        };

	        if (layerms.size() == 1) {  // optimization
	            (*layerm)->fill_surfaces.surfaces.clear();
	            if (neighbours.empty()) {
                    (*layerm)->make_perimeters((*layerm)->slices, {*layerm}, &(*layerm)->fill_surfaces, &(*layerm)->fill_no_overlap_expolygons);
	            } else {
	                SurfaceCollection slices;
	                slices.append(union_safety_offset_ex(union_(to_polygons((*layerm)->slices.surfaces), to_polygons(neighbours))),
	                              (*layerm)->slices.surfaces.front());
	                (*layerm)->make_perimeters(slices, {*layerm}, &(*layerm)->fill_surfaces, &(*layerm)->fill_no_overlap_expolygons, &bridge_exclusions);
	                remove_walls_over_neighbours(**layerm);
	                Surfaces fill_surfaces;
	                for (const Surface &surface : (*layerm)->fill_surfaces.surfaces)
	                    for (ExPolygon &expoly : diff_ex(ExPolygons{ surface.expolygon }, neighbours))
	                        fill_surfaces.emplace_back(surface, std::move(expoly));
	                (*layerm)->fill_surfaces.surfaces = std::move(fill_surfaces);
	                (*layerm)->fill_no_overlap_expolygons = diff_ex((*layerm)->fill_no_overlap_expolygons, neighbours);
	            }
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
	                if (Surfaces &surfaces = slices.begin()->second; ! neighbours.empty())
	                    for (const ExPolygon &expoly : neighbours)
	                        surfaces.emplace_back(surfaces.front(), expoly);
	                // merge the surfaces assigned to each group
	                for (std::pair<const unsigned short,Surfaces> &surfaces_with_extra_perimeters : slices)
	                    new_slices.append(offset_ex(surfaces_with_extra_perimeters.second, ClipperSafetyOffset), surfaces_with_extra_perimeters.second.front());
	            }

	            // make perimeters
	            SurfaceCollection fill_surfaces;
                //BBS
                ExPolygons fill_no_overlap;
	            layerm_config->make_perimeters(new_slices, layerms, &fill_surfaces, &fill_no_overlap, neighbours.empty() ? nullptr : &bridge_exclusions);
	            if (! neighbours.empty())
	                remove_walls_over_neighbours(*layerm_config);

	            // assign fill_surfaces to each layer
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
	                    polygons_append(all_region_slices_p, to_polygons(neighbours));
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
