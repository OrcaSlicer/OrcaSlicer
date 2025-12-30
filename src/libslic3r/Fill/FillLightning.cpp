#include "../ClipperUtils.hpp"
#include "../Print.hpp"
#include "../ShortestPath.hpp"
#include "FillBase.hpp"
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
