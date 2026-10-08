#pragma once

#include "VoronoiPointCloud.hpp"
#include "../ExPolygon.hpp"
#include "../BoundingBox.hpp"

namespace Slic3r::Voronoi {

// Independent of control-point generation, cell geometry and path routing.
struct HullSection {
    ExPolygons cavity;
    ExPolygons skin;
};

ExPolygons density_void(const PointCloudProvider &field, const BoundingBox &bounds, double z, double threshold);
HullSection hull_section(const PointCloudProvider &field, const BoundingBox &bounds,
                         double z, double height, double width, double threshold);

} // namespace Slic3r::Voronoi
