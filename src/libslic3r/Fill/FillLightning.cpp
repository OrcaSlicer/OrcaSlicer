#include "../ClipperUtils.hpp"
#include "../Print.hpp"
#include "../ShortestPath.hpp"
#include "FillBase.hpp"
#include "FillCornerSmoothing.hpp"
#include "FillLightning.hpp"
#include "Lightning/Generator.hpp"

namespace Slic3r::FillLightning {

void Filler::_fill_surface_single(
    const FillParams              &params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point> &direction,
    ExPolygon                      expolygon,
    Polylines                     &polylines_out)
{
    const Layer &layer      = generator->getTreesForLayer(this->layer_id);
    Polylines    fill_lines = layer.convertToLines(to_polygons(expolygon), scaled<coord_t>(0.5 * this->spacing - this->overlap));

    // Orca: round the turns of the branches. Hairpins are left sharp, as they cannot be rounded, and
    // the reach is capped: cutting a corner moves the branch, and a branch is as long as the object
    // rather than as long as one cell of a pattern, so half of a leg would merge it with its neighbour
    // instead of rounding the turn between them. Half the distance between two branches keeps them
    // apart. With more than one line per infill wall the branches are printed as outlines drawn around
    // them, and the outlines of branches that run into each other merge into a single one; moving a
    // branch by more than a fraction of its printed width breaks such an outline up into separate
    // loops, so that width bounds the reach as well.
    const double branch_width   = scaled<double>(this->spacing) * params.multiline;
    const double branch_spacing = branch_width / std::max(double(params.density), EPSILON);
    const double max_reach      = 0.5 * (params.multiline > 1 ? branch_width : branch_spacing);
    smooth_polylines_corners(fill_lines, params.smooth_factor, scaled<double>(params.resolution), max_reach);

    // Apply multiline offset if needed
    multiline_fill(fill_lines, params, spacing);

    fill_lines = Slic3r::intersection_pl(std::move(fill_lines), expolygon);

    chain_or_connect_infill(std::move(fill_lines), expolygon, polylines_out, this->spacing, params);
}

void GeneratorDeleter::operator()(Generator *p) {
    delete p;
}

GeneratorPtr build_generator(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback)
{
    return GeneratorPtr(new Generator(print_object, throw_on_cancel_callback));
}

} // namespace Slic3r::FillAdaptive
