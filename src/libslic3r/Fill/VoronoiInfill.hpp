#pragma once

#include "../BoundingBox.hpp"
#include "../ExPolygon.hpp"
#include "../Polyline.hpp"
#include "VoronoiInfillCgal.hpp"
#include "VoronoiPointCloud.hpp"
#include <limits>
#include <map>
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
        : m_provider(std::move(provider)), m_cloud(m_provider) {}

    // Geometry only, before graph cleanup or traversal; useful for independent
    // distribution experiments and stage-by-stage verification.
    Voronoi::Layer fill_geometry(const BoundingBox &object_bounds, const ExPolygon &region,
                                double spacing, double density, double z, double width, double height,
                                double reference_height = -1., double row_spacing = -1., int wall_count = 1);
    Polylines route_walls(Polylines walls, size_t layer_id, const Points &boundary_nodes = {}) const;

private:
    std::shared_ptr<const Voronoi::PointCloudProvider> m_provider;
    Voronoi::ActivePointCloud m_cloud;
    BoundingBox m_bounds;
    double      m_site_spacing { 0. };
    double      m_layer_z { std::numeric_limits<double>::quiet_NaN() };
    double      m_layer_width { 0. };
    double      m_layer_height { 0. };
    double      m_row_spacing { 0. };
    int m_wall_count { 0 };
    Voronoi::CellGeometry m_geometry;
    Voronoi::Section m_section;
    std::vector<Voronoi::Band> m_bands;
};

// One cache per ascending worker batch; engines are never shared by workers.
struct VoronoiInfillCache {
    std::map<std::shared_ptr<const Voronoi::PointCloudProvider>, std::shared_ptr<VoronoiInfill>> clouds;
};

} // namespace Slic3r
