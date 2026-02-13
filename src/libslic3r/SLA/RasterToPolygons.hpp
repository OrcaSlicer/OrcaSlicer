#ifndef RASTERTOPOLYGONS_HPP
#define RASTERTOPOLYGONS_HPP

#include "libslic3r/ExPolygon.hpp"

namespace Slic3r {
namespace sla {

class RasterGrayscaleAA;

ExPolygons raster_to_polygons(const RasterGrayscaleAA &rst, Vec2i32 windowsize = {1, 1});

}} // namespace Slic3r::sla

#endif // RASTERTOPOLYGONS_HPP
