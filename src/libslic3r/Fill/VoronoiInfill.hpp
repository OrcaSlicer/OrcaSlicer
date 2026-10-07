#pragma once

#include "../BoundingBox.hpp"
#include "../ExPolygon.hpp"
#include "../Polyline.hpp"
#include "VoronoiInfillCgal.hpp"
#include "VoronoiPointCloud.hpp"
#include <limits>
#include <memory>
#include <utility>
#include <vector>
#include <cstddef>

namespace Slic3r {

// Printable sections of a 3D point field, with replaceable site sampling.
class VoronoiInfill
{
public:
    explicit VoronoiInfill(std::shared_ptr<const Voronoi::PointCloudProvider> provider = std::make_shared<Voronoi::PoissonPointCloud>())
        : m_cloud(std::move(provider)) {}

    Polylines fill(const BoundingBox &object_bounds, const ExPolygon &region, double spacing, double density, double z);
    // Geometry only, before graph cleanup or traversal; useful for independent
    // distribution experiments and stage-by-stage verification.
    Voronoi::Layer fill_geometry(const BoundingBox &object_bounds, const ExPolygon &region,
                                double spacing, double density, double z, double width, double height,
                                double reference_height = -1., double row_spacing = -1.);
    Voronoi::Layer fill_layer(const BoundingBox &object_bounds, const ExPolygon &region,
                             double spacing, double density, double z, double width, double height,
                             double reference_height = -1., double row_spacing = -1.,
                             size_t layer_id = 0);

private:
    Voronoi::ActivePointCloud m_cloud;
    BoundingBox m_bounds;
    double      m_site_spacing { 0. };
    double      m_layer_z { std::numeric_limits<double>::quiet_NaN() };
    double      m_layer_width { 0. };
    double      m_layer_height { 0. };
    double      m_row_spacing { 0. };
    Voronoi::Section m_section;
    std::vector<Voronoi::Band> m_bands;
};

} // namespace Slic3r
