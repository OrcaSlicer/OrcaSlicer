// Copyright (c) 2023 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher.

#include "InterlockingGenerator.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "Layer.hpp"

namespace std {
template<> struct hash<Slic3r::GridPoint3>
{
    size_t operator()(const Slic3r::GridPoint3& pp) const noexcept
    {
        static int prime  = 31;
        int        result = 89;
        result            = static_cast<int>(result * prime + pp.x());
        result            = static_cast<int>(result * prime + pp.y());
        result            = static_cast<int>(result * prime + pp.z());
        return static_cast<size_t>(result);
    }
};
} // namespace std


namespace Slic3r {

void InterlockingGenerator::generate_interlocking_structure(PrintObject* print_object)
{
    const auto& config = print_object->config();
    if (!config.interlocking_beam) {
        return;
    }

    const float    rotation           = Geometry::deg2rad(config.interlocking_orientation.value);
    const coord_t  beam_layer_count   = config.interlocking_beam_layer_count;
    const int      interface_depth    = config.interlocking_depth;
    const int      boundary_avoidance = config.interlocking_boundary_avoidance;
    const coord_t  beam_width         = scaled(config.interlocking_beam_width.value);

    const DilationKernel interface_dilation(GridPoint3(interface_depth, interface_depth, interface_depth), DilationKernel::Type::PRISM);

    const bool           air_filtering = boundary_avoidance > 0;
    const DilationKernel air_dilation(GridPoint3(boundary_avoidance, boundary_avoidance, boundary_avoidance), DilationKernel::Type::PRISM);

    const coord_t cell_width = beam_width + beam_width;
    const Vec3crd cell_size(cell_width, cell_width, 2 * beam_layer_count);

    for (size_t region_a_index = 0; region_a_index < print_object->num_printing_regions(); region_a_index++) {
        const PrintRegion& region_a      = print_object->printing_region(region_a_index);
        const auto         extruder_nr_a = region_a.extruder(FlowRole::frExternalPerimeter);

        for (size_t region_b_index = region_a_index + 1; region_b_index < print_object->num_printing_regions(); region_b_index++) {
            const PrintRegion& region_b      = print_object->printing_region(region_b_index);
            const auto         extruder_nr_b = region_b.extruder(FlowRole::frExternalPerimeter);
            if (extruder_nr_a == extruder_nr_b) {
                continue;
            }

            InterlockingGenerator gen(*print_object, region_a_index, region_b_index, beam_width, boundary_avoidance, rotation, cell_size, beam_layer_count,
                                      interface_dilation, air_dilation, air_filtering);
            gen.generateInterlockingStructure();
        }
    }
}

std::pair<ExPolygons, ExPolygons> InterlockingGenerator::growBorderAreasPerpendicular(const ExPolygons& a, const ExPolygons& b, const coord_t& detect) const
{
    const coord_t min_line =
        std::min(print_object.printing_region(region_a_index).flow(print_object, frExternalPerimeter, 0.1).scaled_width(),
                 print_object.printing_region(region_b_index).flow(print_object, frExternalPerimeter, 0.1).scaled_width());

    const ExPolygons total_shrunk = offset_ex(union_ex(offset_ex(a, min_line), offset_ex(b, min_line)), 2 * -min_line);

    ExPolygons from_border_a = diff_ex(a, total_shrunk);
    ExPolygons from_border_b = diff_ex(b, total_shrunk);

    ExPolygons temp_a, temp_b;
    for (coord_t i = 0; i < (detect / min_line) + 2; ++i) {
        temp_a        = offset_ex(from_border_a, min_line);
        temp_b        = offset_ex(from_border_b, min_line);
        from_border_a = diff_ex(temp_a, temp_b);
        from_border_b = diff_ex(temp_b, temp_a);
    }

    return {from_border_a, from_border_b};
}

void InterlockingGenerator::handleThinAreas(const std::unordered_set<GridPoint3>& has_all_meshes) const
{
    const coord_t     number_of_beams_detect = boundary_avoidance;
    const coord_t     number_of_beams_expand = boundary_avoidance - 1;
    constexpr coord_t rounding_errors        = 5;

    const coord_t max_beam_width = beam_width;
    const coord_t detect         = (max_beam_width * number_of_beams_detect) + rounding_errors;
    const coord_t expand         = (max_beam_width * number_of_beams_expand) + rounding_errors;
    const coord_t close_gaps =
        std::min(print_object.printing_region(region_a_index).flow(print_object, frExternalPerimeter, 0.1).scaled_width(),
                 print_object.printing_region(region_b_index).flow(print_object, frExternalPerimeter, 0.1).scaled_width()) / 4;

    // Make an inclusionary polygon, to only actually handle thin areas near actual microstructures (so not in skin for example).
    std::vector<Polygons> near_interlock_per_layer;
    near_interlock_per_layer.assign(print_object.layer_count(), Polygons());
    for (const auto& cell : has_all_meshes) {
        const auto bottom_corner = vu.toLowerCorner(cell);
        for (coord_t layer_nr = bottom_corner.z();
             layer_nr < bottom_corner.z() + cell_size.z() && layer_nr < static_cast<coord_t>(near_interlock_per_layer.size()); ++layer_nr) {
            near_interlock_per_layer[static_cast<size_t>(layer_nr)].push_back(vu.toPolygon(cell));
        }
    }
    for (auto& near_interlock : near_interlock_per_layer) {
        near_interlock = offset(union_(closing(near_interlock, rounding_errors)), detect);
        polygons_rotate(near_interlock, rotation);
    }

    // Only alter layers when they are present in both meshes, zip should take care if that.
    for (size_t layer_nr = 0; layer_nr < print_object.layer_count(); layer_nr++){
        auto       layer   = print_object.get_layer(layer_nr);
        ExPolygons polys_a = to_expolygons(layer->get_region(region_a_index)->slices.surfaces);
        ExPolygons polys_b = to_expolygons(layer->get_region(region_b_index)->slices.surfaces);

        const auto [from_border_a, from_border_b] = growBorderAreasPerpendicular(polys_a, polys_b, detect);

        // Get the areas of each mesh that are _not_ thin (large), by performing a morphological open.
        const ExPolygons large_a = opening_ex(polys_a, detect);
        const ExPolygons large_b = opening_ex(polys_b, detect);

        // Derive the area that the thin areas need to expand into (so the added areas to the thin strips) from the information we already have.
        const ExPolygons thin_expansion_a =
            offset_ex(intersection_ex(intersection_ex(intersection_ex(large_b, offset_ex(diff_ex(polys_a, large_a), expand)),
                                                      near_interlock_per_layer[layer_nr]),
                                      from_border_a),
                      rounding_errors);
        const ExPolygons thin_expansion_b =
            offset_ex(intersection_ex(intersection_ex(intersection_ex(large_a, offset_ex(diff_ex(polys_b, large_b), expand)),
                                                      near_interlock_per_layer[layer_nr]),
                                      from_border_b),
                      rounding_errors);

        // Expanded thin areas of the opposing polygon should 'eat into' the larger areas of the polygon,
        // and conversely, add the expansions to their own thin areas.
        layer->get_region(region_a_index)->slices.set(closing_ex(diff_ex(union_ex(polys_a, thin_expansion_a), thin_expansion_b), close_gaps), stInternal);
        layer->get_region(region_b_index)->slices.set(closing_ex(diff_ex(union_ex(polys_b, thin_expansion_b), thin_expansion_a), close_gaps), stInternal);
    }
}

void InterlockingGenerator::generateInterlockingStructure() const
{
    std::vector<std::unordered_set<GridPoint3>> voxels_per_mesh = getShellVoxels(interface_dilation);

    std::unordered_set<GridPoint3>& has_any_mesh   = voxels_per_mesh[0];
    std::unordered_set<GridPoint3>& has_all_meshes = voxels_per_mesh[1];
    has_any_mesh.merge(has_all_meshes); // perform union and intersection simultaneously. Cannibalizes voxels_per_mesh

    if (has_all_meshes.empty()) {
        return;
    }

    const std::vector<ExPolygons> layer_regions = computeUnionedVolumeRegions();

    if (air_filtering) {
        std::unordered_set<GridPoint3> air_cells;
        addBoundaryCells(layer_regions, air_dilation, air_cells);

        for (const GridPoint3& p : air_cells) {
            has_all_meshes.erase(p);
        }

        handleThinAreas(has_all_meshes);
    }

    applyMicrostructureToOutlines(has_all_meshes, layer_regions);
}

std::vector<std::unordered_set<GridPoint3>> InterlockingGenerator::getShellVoxels(const DilationKernel& kernel) const
{
    std::vector<std::unordered_set<GridPoint3>> voxels_per_mesh(2);

    // mark all cells which contain some boundary
    for (size_t region_idx = 0; region_idx < 2; region_idx++)
    {
        const size_t region = (region_idx == 0) ? region_a_index : region_b_index;
        std::unordered_set<GridPoint3>& mesh_voxels = voxels_per_mesh[region_idx];

        std::vector<ExPolygons> rotated_polygons_per_layer(print_object.layer_count());
        for (size_t layer_nr = 0; layer_nr < print_object.layer_count(); layer_nr++)
        {
            auto layer = print_object.get_layer(layer_nr);
            rotated_polygons_per_layer[layer_nr] = to_expolygons(layer->get_region(region)->slices.surfaces);
            expolygons_rotate(rotated_polygons_per_layer[layer_nr], rotation);
        }

        addBoundaryCells(rotated_polygons_per_layer, kernel, mesh_voxels);
    }

    return voxels_per_mesh;
}

void InterlockingGenerator::addBoundaryCells(const std::vector<ExPolygons>&  layers,
                                             const DilationKernel&           kernel,
                                             std::unordered_set<GridPoint3>& cells) const
{
    auto voxel_emplacer = [&cells](GridPoint3 p) {
        if (p.z() < 0) {
            return true;
        }
        cells.emplace(p);
        return true;
    };

    for (size_t layer_nr = 0; layer_nr < layers.size(); layer_nr++) {
        const coord_t z = static_cast<coord_t>(layer_nr);
        vu.walkDilatedPolygons(layers[layer_nr], z, kernel, voxel_emplacer);
        ExPolygons skin = layers[layer_nr];
        if (layer_nr > 0) {
            skin = xor_ex(skin, layers[layer_nr - 1]);
        }
        skin = opening_ex(skin, cell_size.x() / 2.f); // remove superfluous small areas, which would anyway be included because of walkPolygons
        vu.walkDilatedAreas(skin, z, kernel, voxel_emplacer);
    }
}

std::vector<ExPolygons> InterlockingGenerator::computeUnionedVolumeRegions() const
{
    const size_t max_layer_count = print_object.layer_count() +
                                   1; // introduce ghost layer on top for correct skin computation of topmost layer.
    std::vector<ExPolygons> layer_regions(max_layer_count);

    for (size_t layer_nr = 0; layer_nr < max_layer_count - 1; layer_nr++) {
        auto& layer_region = layer_regions[static_cast<size_t>(layer_nr)];
        for (size_t region_idx : {region_a_index, region_b_index}) {
            auto layer = print_object.get_layer(layer_nr);
            expolygons_append(layer_region, to_expolygons(layer->get_region(region_idx)->slices.surfaces));
        }
        layer_region = closing_ex(layer_region, ignored_gap_); // Morphological close to merge meshes into single volume
        expolygons_rotate(layer_region, rotation);
    }
    return layer_regions;
}

std::vector<std::vector<ExPolygons>> InterlockingGenerator::generateMicrostructure() const
{
    std::vector<std::vector<ExPolygons>> cell_area_per_mesh_per_layer;
    cell_area_per_mesh_per_layer.resize(2);
    cell_area_per_mesh_per_layer[0].resize(2);
    const coord_t beam_w_sum = beam_width + beam_width;
    const coord_t middle     = cell_size.x() * beam_width / beam_w_sum;
    const coord_t width[2]   = {middle, cell_size.x() - middle};
    for (size_t mesh_idx : {0ul, 1ul}) {
        Point offset(mesh_idx ? middle : 0, 0);
        Point area_size(width[mesh_idx], cell_size.y());

        Polygon poly;
        poly.append(offset);
        poly.append(offset + Point(area_size.x(), 0));
        poly.append(offset + area_size);
        poly.append(offset + Point(0, area_size.y()));
        cell_area_per_mesh_per_layer[0][mesh_idx].emplace_back(poly);
    }
    cell_area_per_mesh_per_layer[1] = cell_area_per_mesh_per_layer[0];
    for (ExPolygons& polys : cell_area_per_mesh_per_layer[1]) {
        for (ExPolygon& poly : polys) {
            for (Point& p : poly.contour) {
                std::swap(p.x(), p.y());
            }
        }
    }
    return cell_area_per_mesh_per_layer;
}

void InterlockingGenerator::applyMicrostructureToOutlines(const std::unordered_set<GridPoint3>& cells,
                                                          const std::vector<ExPolygons>&        layer_regions) const
{
    std::vector<std::vector<ExPolygons>> cell_area_per_mesh_per_layer = generateMicrostructure();

    const float  unapply_rotation = -rotation;
    const size_t max_layer_count  = print_object.layer_count();

    std::vector<ExPolygons> structure_per_layer[2]; // for each mesh the structure on each layer

    // Every `beam_layer_count` number of layers are combined to an interlocking beam layer
    // to store these we need ceil(max_layer_count / beam_layer_count) of these layers
    // the formula is rewritten as (max_layer_count + beam_layer_count - 1) / beam_layer_count, so it works for integer division
    size_t num_interlocking_layers = (max_layer_count + static_cast<size_t>(beam_layer_count) - 1ul) /
                                     static_cast<size_t>(beam_layer_count);
    structure_per_layer[0].resize(num_interlocking_layers);
    structure_per_layer[1].resize(num_interlocking_layers);

    // Only compute cell structure for half the layers, because since our beams are two layers high, every odd layer of the structure will
    // be the same as the layer below.
    for (const GridPoint3& grid_loc : cells) {
        Vec3crd bottom_corner = vu.toLowerCorner(grid_loc);
        for (size_t mesh_idx = 0; mesh_idx < 2; mesh_idx++) {
            for (size_t layer_nr = bottom_corner.z(); layer_nr < bottom_corner.z() + cell_size.z() && layer_nr < max_layer_count;
                 layer_nr += beam_layer_count) {
                ExPolygons areas_here = cell_area_per_mesh_per_layer[static_cast<size_t>(layer_nr / beam_layer_count) %
                                                                cell_area_per_mesh_per_layer.size()][mesh_idx];
                for (auto & here : areas_here) {
                    here.translate(bottom_corner.x(), bottom_corner.y());
                }
                expolygons_append(structure_per_layer[mesh_idx][static_cast<size_t>(layer_nr / beam_layer_count)], areas_here);
            }
        }
    }

    for (size_t mesh_idx = 0; mesh_idx < 2; mesh_idx++) {
        for (size_t layer_nr = 0; layer_nr < structure_per_layer[mesh_idx].size(); layer_nr++) {
            ExPolygons& layer_structure = structure_per_layer[mesh_idx][layer_nr];
            layer_structure = union_ex(layer_structure);
            expolygons_rotate(layer_structure, unapply_rotation);
        }
    }

    for (size_t region_idx = 0; region_idx < 2; region_idx++) {
        const size_t region = (region_idx == 0) ? region_a_index : region_b_index;
        for (size_t layer_nr = 0; layer_nr < max_layer_count; layer_nr++) {
            ExPolygons layer_outlines = layer_regions[layer_nr];
            expolygons_rotate(layer_outlines, unapply_rotation);

            const ExPolygons areas_here = intersection_ex(structure_per_layer[region_idx][layer_nr / static_cast<size_t>(beam_layer_count)], layer_outlines);
            const ExPolygons& areas_other = structure_per_layer[!region_idx][layer_nr / static_cast<size_t>(beam_layer_count)];

            auto       layer  = print_object.get_layer(layer_nr);
            auto&      slices = layer->get_region(region)->slices;
            ExPolygons polys  = to_expolygons(slices.surfaces);
            slices.set(union_ex(diff_ex(polys, areas_other), // reduce layer areas inward with beams from other mesh
                                areas_here)                  // extend layer areas outward with newly added beams
                       , stInternal);
        }
    }
}

} // namespace Slic3r
