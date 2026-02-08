//Copyright (c) 2021 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#include "Generator.hpp"
#include "TreeNode.hpp"

#include "../../ClipperUtils.hpp"
#include "../../Layer.hpp"
#include "../../Print.hpp"

#include "ExPolygon.hpp"

/* Possible future tasks/optimizations,etc.:
 * - Improve connecting heuristic to favor connecting to shorter trees
 * - Change which node of a tree is the root when that would be better in reconnectRoots.
 * - (For implementation in Infill classes & elsewhere): Outline offset, infill-overlap & perimeter gaps.
 * - Allow for polylines, i.e. merge Tims PR about polyline fixes
 * - Unit Tests?
 * - Optimization: let the square grid store the closest point on boundary
 * - Optimization: only compute the closest dist to / point on boundary for the outer cells and flood-fill the rest
 * - Make a pass with Arachne over the output. Somehow.
 * - Generate all to-be-supported points at once instead of sequentially: See branch interlocking_gen PolygonUtils::spreadDots (Or work with sparse grids.)
 * - Lots of magic values ... to many to parameterize. But are they the best?
 * - Move more complex computations from Generator constructor to elsewhere.
 */

namespace Slic3r
{
static  std::string get_svg_filename(std::string layer_nr_or_z, std::string tag = "bbl_ts")
{
    static bool rand_init = false;

    if (!rand_init) {
        srand(time(NULL));
        rand_init = true;
    }

    int rand_num = rand() % 1000000;
    //makedir("./SVG");
    std::string prefix = "./SVG/";
    std::string suffix = ".svg";
    return prefix + tag + "_" + layer_nr_or_z /*+ "_" + std::to_string(rand_num)*/ + suffix;
}

Slic3r::SVG draw_two_overhangs_to_svg(size_t ts_layer, const ExPolygons& overhangs1, const ExPolygons& overhangs2)
{
    //if (overhangs1.empty() && overhangs2.empty())
    //    return ;
    BoundingBox bbox1 = get_extents(overhangs1);
    BoundingBox bbox2 = get_extents(overhangs2);
    bbox1.merge(bbox2);

    Slic3r::SVG svg(get_svg_filename(std::to_string(ts_layer), "two_overhangs_generator"), bbox1);
    //if (!svg.is_opened())        return;

    svg.draw(union_ex(overhangs1), "blue");
    svg.draw(union_ex(overhangs2), "red");

    return svg;
}
}


namespace Slic3r::FillLightning {

Generator::Generator(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback)
{
    const PrintConfig         &print_config         = print_object.print()->config();
    const PrintObjectConfig   &object_config        = print_object.config();
    const PrintRegionConfig   &region_config        = print_object.shared_regions()->all_regions.front()->config();
    const std::vector<double> &nozzle_diameters     = print_config.nozzle_diameter.values;
    double                     max_nozzle_diameter  = *std::max_element(nozzle_diameters.begin(), nozzle_diameters.end());
    const int                  n_multiline          = region_config.fill_multiline.value;
//    const int                  infill_extruder      = region_config.infill_extruder.value;
    const double               default_infill_extrusion_width = Flow::auto_extrusion_width(FlowRole::frInfill, float(max_nozzle_diameter));
    // Note: There's not going to be a layer below the first one, so the 'initial layer height' doesn't have to be taken into account.
    const double               layer_thickness      = scaled<double>(object_config.layer_height.value);

    m_infill_extrusion_width = scaled<float>(region_config.sparse_infill_line_width.get_abs_value(max_nozzle_diameter));
    // Orca: fix lightning infill divide by zero when infill line width is set to 0.
    // firstly attempt to set it to the default line width. If that is not provided either, set it to a sane default
    // based on the nozzle diameter.
    if (m_infill_extrusion_width < EPSILON)
        m_infill_extrusion_width = scaled<float>(
            object_config.line_width.get_abs_value(max_nozzle_diameter) < EPSILON ?
            default_infill_extrusion_width :
            object_config.line_width.get_abs_value(max_nozzle_diameter)
        );
    
    m_supporting_radius = coord_t(m_infill_extrusion_width) * 100 * n_multiline / region_config.sparse_infill_density;

    const double lightning_infill_overhang_angle      = M_PI / 4; // 45 degrees
    const double lightning_infill_prune_angle         = M_PI / 4; // 45 degrees
    const double lightning_infill_straightening_angle = M_PI / 4; // 45 degrees
    m_wall_supporting_radius                          = coord_t(layer_thickness * std::tan(lightning_infill_overhang_angle));
    m_prune_length                                    = coord_t(layer_thickness * std::tan(lightning_infill_prune_angle));
    m_straightening_max_distance                      = coord_t(layer_thickness * std::tan(lightning_infill_straightening_angle));

    generateInitialInternalOverhangs(print_object, throw_on_cancel_callback);
    generateTrees(print_object, throw_on_cancel_callback);
}

Generator::Generator(PrintObject* m_object, std::vector<Polygons>& contours, std::vector<Polygons>& overhangs, const std::function<void()> &throw_on_cancel_callback, float density)
{
    const PrintConfig         &print_config         = m_object->print()->config();
    const PrintObjectConfig   &object_config        = m_object->config();
    const PrintRegionConfig   &region_config        = m_object->shared_regions()->all_regions.front()->config();
    const std::vector<double> &nozzle_diameters     = print_config.nozzle_diameter.values;
    double                     max_nozzle_diameter  = *std::max_element(nozzle_diameters.begin(), nozzle_diameters.end());
//    const int                  infill_extruder      = region_config.infill_extruder.value;
    const double               default_infill_extrusion_width = Flow::auto_extrusion_width(FlowRole::frInfill, float(max_nozzle_diameter));
    // Note: There's not going to be a layer below the first one, so the 'initial layer height' doesn't have to be taken into account.
    const double               layer_thickness      = scaled<double>(object_config.layer_height.value);

    m_infill_extrusion_width = scaled<float>(region_config.sparse_infill_line_width.get_abs_value(max_nozzle_diameter));
    // Orca: fix lightning infill divide by zero when infill line width is set to 0.
    // firstly attempt to set it to the default line width. If that is not provided either, set it to a sane default
    // based on the nozzle diameter.
    if (m_infill_extrusion_width < EPSILON)
        m_infill_extrusion_width = scaled<float>(
            object_config.line_width.get_abs_value(max_nozzle_diameter) < EPSILON ?
            default_infill_extrusion_width :
            object_config.line_width.get_abs_value(max_nozzle_diameter)
        );
    //m_supporting_radius: against to the density of lightning, failures may happen if set to high density
    //higher density lightning makes support harder, more time-consuming on computing and printing, but more reliable on supporting overhangs
    //lower density lightning performs opposite
    //TODO: decide whether enable density controller in advanced options or not
    density = std::max(0.15f, density);
    m_supporting_radius = coord_t(m_infill_extrusion_width) / density;

    const double lightning_infill_overhang_angle = M_PI / 4; // 45 degrees
    const double lightning_infill_prune_angle = M_PI / 4; // 45 degrees
    const double lightning_infill_straightening_angle = M_PI / 4; // 45 degrees
    m_wall_supporting_radius = layer_thickness * std::tan(lightning_infill_overhang_angle);
    m_prune_length = layer_thickness * std::tan(lightning_infill_prune_angle);
    m_straightening_max_distance = layer_thickness * std::tan(lightning_infill_straightening_angle);

    m_overhang_per_layer = overhangs;

    generateTreesforSupport(contours, throw_on_cancel_callback);

    //for (size_t i = 0; i < overhangs.size(); i++)
    //{
    //    auto svg = draw_two_overhangs_to_svg(i, to_expolygons(contours[i]), to_expolygons(overhangs[i]));
    //    for (auto& root : m_lightning_layers[i].tree_roots)
    //        root->draw_tree(svg);
    //}
}

void Generator::generateInitialInternalOverhangs(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback)
{
    m_overhang_per_layer.resize(print_object.layers().size());

    Polygons infill_area_above;
    //Iterate from top to bottom, to subtract the overhang areas above from the overhang areas on the layer below, to get only overhang in the top layer where it is overhanging.
    for (int layer_nr = int(print_object.layers().size()) - 1; layer_nr >= 0; --layer_nr) {
        throw_on_cancel_callback();
        Polygons infill_area_here;
        for (const LayerRegion* layerm : print_object.get_layer(layer_nr)->regions())
            for (const Surface& surface : layerm->fill_surfaces.surfaces)
                if (surface.surface_type == stInternal || surface.surface_type == stInternalVoid)
                    append(infill_area_here, to_polygons(surface.expolygon));

        //Remove the part of the infill area that is already supported by the walls.
        Polygons overhang = diff(offset(infill_area_here, -float(m_wall_supporting_radius)), infill_area_above);

        m_overhang_per_layer[layer_nr] = overhang;
        infill_area_above = std::move(infill_area_here);
    }
}

const Layer& Generator::getTreesForLayer(const size_t& layer_id) const
{
    assert(layer_id < m_lightning_layers.size());
    return m_lightning_layers[layer_id];
}

void Generator::generateTrees(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback)
{
    const auto _locator_cell_size = locator_cell_size();
    m_lightning_layers.resize(print_object.layers().size());
    bboxs.resize(print_object.layers().size());
    std::vector<Polygons> infill_outlines(print_object.layers().size(), Polygons());

    // For-each layer from top to bottom:
    for (int layer_id = int(print_object.layers().size()) - 1; layer_id >= 0; layer_id--) {
        throw_on_cancel_callback();
        for (const LayerRegion *layerm : print_object.get_layer(layer_id)->regions())
            for (const Surface &surface : layerm->fill_surfaces.surfaces)
                if (surface.surface_type == stInternal || surface.surface_type == stInternalVoid)
                    append(infill_outlines[layer_id], to_polygons(surface.expolygon));
    }

    // For various operations its beneficial to quickly locate nearby features on the polygon:
    const size_t top_layer_id = print_object.layers().size() - 1;
    EdgeGrid::Grid outlines_locator(get_extents(infill_outlines[top_layer_id]).inflated(SCALED_EPSILON));
    outlines_locator.create(infill_outlines[top_layer_id], _locator_cell_size);

    // For-each layer from top to bottom:
    for (int layer_id = int(top_layer_id); layer_id >= 0; layer_id--) {
        throw_on_cancel_callback();
        Layer             &current_lightning_layer = m_lightning_layers[layer_id];
        const Polygons    &current_outlines        = infill_outlines[layer_id];
        const BoundingBox &current_outlines_bbox   = get_extents(current_outlines);

        bboxs[layer_id] = get_extents(current_outlines);

        // register all trees propagated from the previous layer as to-be-reconnected
        std::vector<NodeSPtr> to_be_reconnected_tree_roots = current_lightning_layer.tree_roots;

        current_lightning_layer.generateNewTrees(m_overhang_per_layer[layer_id], current_outlines, current_outlines_bbox, outlines_locator, m_supporting_radius, m_wall_supporting_radius, throw_on_cancel_callback);
        current_lightning_layer.reconnectRoots(to_be_reconnected_tree_roots, current_outlines, current_outlines_bbox, outlines_locator, m_supporting_radius, m_wall_supporting_radius);

        // Initialize trees for next lower layer from the current one.
        if (layer_id == 0)
            return;

        const Polygons &below_outlines      = infill_outlines[layer_id - 1];
        BoundingBox     below_outlines_bbox = get_extents(below_outlines).inflated(SCALED_EPSILON);
        if (const BoundingBox &outlines_locator_bbox = outlines_locator.bbox(); outlines_locator_bbox.defined)
            below_outlines_bbox.merge(outlines_locator_bbox);

        if (!current_lightning_layer.tree_roots.empty())
            below_outlines_bbox.merge(get_extents(current_lightning_layer.tree_roots).inflated(SCALED_EPSILON));

        outlines_locator.set_bbox(below_outlines_bbox);
        outlines_locator.create(below_outlines, _locator_cell_size);

        std::vector<NodeSPtr>& lower_trees = m_lightning_layers[layer_id - 1].tree_roots;
        for (auto& tree : current_lightning_layer.tree_roots)
            tree->propagateToNextLayer(lower_trees, below_outlines, outlines_locator, m_prune_length, m_straightening_max_distance, _locator_cell_size / 2);
    }
}

void Generator::generateTreesforSupport(std::vector<Polygons>& contours, const std::function<void()> &throw_on_cancel_callback)
{
    if (contours.empty()) return;

    m_lightning_layers.resize(contours.size());
    bboxs.resize(contours.size());

    const auto _locator_cell_size = locator_cell_size();
    // For various operations its beneficial to quickly locate nearby features on the polygon:
    const size_t top_layer_id = contours.size() - 1;
    EdgeGrid::Grid outlines_locator(get_extents(contours[top_layer_id]).inflated(SCALED_EPSILON));
    outlines_locator.create(contours[top_layer_id], _locator_cell_size);

    // For-each layer from top to bottom:
    for (int layer_id = int(top_layer_id); layer_id >= 0; layer_id--) {
		throw_on_cancel_callback();
        Layer& current_lightning_layer = m_lightning_layers[layer_id];
        const Polygons& current_outlines = contours[layer_id];
        const BoundingBox& current_outlines_bbox = get_extents(current_outlines);

        bboxs[layer_id] = get_extents(current_outlines);

        // register all trees propagated from the previous layer as to-be-reconnected
        std::vector<NodeSPtr> to_be_reconnected_tree_roots = current_lightning_layer.tree_roots;

        current_lightning_layer.generateNewTrees(m_overhang_per_layer[layer_id], current_outlines, current_outlines_bbox, outlines_locator, m_supporting_radius, m_wall_supporting_radius, throw_on_cancel_callback);
        current_lightning_layer.reconnectRoots(to_be_reconnected_tree_roots, current_outlines, current_outlines_bbox, outlines_locator, m_supporting_radius, m_wall_supporting_radius);

        // Initialize trees for next lower layer from the current one.
        if (layer_id == 0)
            return;

        const Polygons& below_outlines = contours[layer_id - 1];
        BoundingBox     below_outlines_bbox = get_extents(below_outlines).inflated(SCALED_EPSILON);
        if (const BoundingBox& outlines_locator_bbox = outlines_locator.bbox(); outlines_locator_bbox.defined)
            below_outlines_bbox.merge(outlines_locator_bbox);

        if (!current_lightning_layer.tree_roots.empty())
            below_outlines_bbox.merge(get_extents(current_lightning_layer.tree_roots).inflated(SCALED_EPSILON));

        outlines_locator.set_bbox(below_outlines_bbox);
        outlines_locator.create(below_outlines, _locator_cell_size);

        std::vector<NodeSPtr>& lower_trees = m_lightning_layers[layer_id - 1].tree_roots;
        for (auto& tree : current_lightning_layer.tree_roots)
            tree->propagateToNextLayer(lower_trees, below_outlines, outlines_locator, m_prune_length, m_straightening_max_distance, _locator_cell_size / 2);
    }
}

} // namespace Slic3r::FillLightning
