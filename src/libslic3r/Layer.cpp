#include "Layer.hpp"
#include "ClipperUtils.hpp"
#include "Print.hpp"
#include "Fill/Fill.hpp"
#include "ShortestPath.hpp"
#include "SVG.hpp"
#include "BoundingBox.hpp"

#include <boost/log/trivial.hpp>

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

bool Layer::is_perimeter_compatible(const PrintRegion& a, const PrintRegion& b)
{
    const PrintRegionConfig& config       = a.config();
    const PrintRegionConfig& other_config = b.config();

    return config.wall_filament             == other_config.wall_filament
		&& config.wall_loops                  == other_config.wall_loops
		&& config.wall_sequence               == other_config.wall_sequence
		&& config.is_infill_first             == other_config.is_infill_first
		&& config.inner_wall_speed             == other_config.inner_wall_speed
		&& config.outer_wall_speed    == other_config.outer_wall_speed
		&& config.small_perimeter_speed    == other_config.small_perimeter_speed
        && config.gap_infill_speed.value == other_config.gap_infill_speed.value
        && config.filter_out_gap_fill.value == other_config.filter_out_gap_fill.value
		&& config.detect_overhang_wall                   == other_config.detect_overhang_wall
		&& config.overhang_reverse                       == other_config.overhang_reverse
		&& config.overhang_reverse_threshold             == other_config.overhang_reverse_threshold
		&& config.wall_direction                         == other_config.wall_direction
		&& config.opt_serialize("inner_wall_line_width") == other_config.opt_serialize("inner_wall_line_width")
		&& config.opt_serialize("outer_wall_line_width") == other_config.opt_serialize("outer_wall_line_width")
		&& config.detect_thin_wall                  == other_config.detect_thin_wall
		&& config.infill_wall_overlap              == other_config.infill_wall_overlap
        && config.top_bottom_infill_wall_overlap              == other_config.top_bottom_infill_wall_overlap
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

// Here the perimeters are created cummulatively for all layer regions sharing the same parameters influencing the perimeters.
// The perimeter paths and the thin fills (ExtrusionEntityCollection) are assigned to the first compatible layer region.
// The resulting fill surface is split back among the originating regions.
void Layer::make_perimeters()
{
    BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id();

    regions_by_fuzzify.clear();

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
                    if (is_perimeter_compatible(this_region, other_region))
		            {
			 			other_layerm->perimeters.clear();
			 			other_layerm->fills.clear();
			 			other_layerm->thin_fills.clear();
		                layerms.push_back(other_layerm);
		                done[it - m_regions.begin()] = true;
		            }
		        }

	        if (layerms.size() == 1) {  // optimization
	            (*layerm)->fill_surfaces.surfaces.clear();
                (*layerm)->make_perimeters((*layerm)->slices, {*layerm}, &(*layerm)->fill_surfaces, &(*layerm)->fill_no_overlap_expolygons);
	            (*layerm)->fill_expolygons = to_expolygons((*layerm)->fill_surfaces.surfaces);
	        } else {
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
	                // merge the surfaces assigned to each group
	                for (std::pair<const unsigned short,Surfaces> &surfaces_with_extra_perimeters : slices)
	                    new_slices.append(offset_ex(surfaces_with_extra_perimeters.second, ClipperSafetyOffset), surfaces_with_extra_perimeters.second.front());
	            }

	            // make perimeters
	            SurfaceCollection fill_surfaces;
                //BBS
                ExPolygons fill_no_overlap;
	            layerm_config->make_perimeters(new_slices, layerms, &fill_surfaces, &fill_no_overlap);

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
	            }
	        }
	    }

    // Union fuzzy regions
    if (regions_by_fuzzify.size() == 1) {
        // Only one region, means this layer should either be entirely fuzzified or not fuzzified at all,
        // so no point to keep the region polygon
        if (!regions_by_fuzzify.begin()->first.fuzzify()) {
            regions_by_fuzzify.clear();
        } else {
            regions_by_fuzzify.begin()->second.clear();
        }
    } else {
        for (auto fuzzify = regions_by_fuzzify.begin(); fuzzify != regions_by_fuzzify.end();) {
            // Skip non-fuzzify regions
            if (!fuzzify->first.fuzzify()) {
                ++fuzzify;
                continue;
            }

            // Remove region that should not be fuzzified on first layer
            if (this->id() <= 0 && !fuzzify->first.fuzzy_first_layer) {
                fuzzify = regions_by_fuzzify.erase(fuzzify);
                continue;
            }

            fuzzify->second = intersection_ex(offset_ex(fuzzify->second, ClipperSafetyOffset), this->lslices);
            if (!fuzzify->second.empty() && this->upper_layer && !this->upper_layer->lslices.empty()) {
                // Clip the fuzzy region by upper layer, so the top surface that is covered by upper layer is not fuzzified
                fuzzify->second = diff_ex(fuzzify->second, this->upper_layer->lslices);
            }

            // Remove empty one
            if (fuzzify->second.empty()) {
                fuzzify = regions_by_fuzzify.erase(fuzzify);
            } else {
                ++fuzzify;
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
    const auto print_config = this->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting;
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
    const auto print_config = this->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting;
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
    const auto print_config = this->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting;
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
        float density = layerm->region().config().sparse_infill_density;
        InfillPattern pattern = layerm->region().config().sparse_infill_pattern;
        if (density == 0.)
            return -1;

        //BBS: rough estimation and need to be optimized
        double spacing = flow.scaled_spacing() * (100 - density) / density;
        switch (pattern) {
            case ipConcentric:
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
