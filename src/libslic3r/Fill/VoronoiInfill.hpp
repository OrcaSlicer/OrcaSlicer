#pragma once

#include "../BoundingBox.hpp"
#include "../ExPolygon.hpp"
#include "../Polyline.hpp"
#include "VoronoiInfillCgal.hpp"
#include "VoronoiPointCloud.hpp"
#include <limits>
#include <memory>
#include <stdexcept>
#include <map>
#include <tuple>
#include <utility>
#include <vector>
#include <cstddef>

namespace Slic3r {

// Printable sections of a 3D point field, with replaceable site sampling.
class VoronoiInfill
{
public:
    explicit VoronoiInfill(std::shared_ptr<const Voronoi::PointCloudProvider> provider = std::make_shared<Voronoi::PoissonPointCloud>())
        : m_provider(std::move(provider))
        { if (!m_provider) throw std::invalid_argument("Missing Voronoi point-cloud provider"); }

    void set_z_range(const Vec2d &range) { m_z_range = range; }

    // Geometry only, before graph cleanup or traversal; useful for independent
    // distribution experiments and stage-by-stage verification.
    Voronoi::Layer fill_geometry(const BoundingBox &object_bounds, const ExPolygon &region,
                                double spacing, double density, double z, double width, double height,
                                double reference_height = -1., double row_spacing = -1., int wall_count = 1);

private:
    std::shared_ptr<const Voronoi::PointCloudProvider> m_provider;
    std::vector<Vec3d> m_sites;
    std::shared_ptr<const Voronoi::CellGeometry> m_geometry;
    Vec2d m_z_range { Vec2d::Zero() };
    BoundingBoxf3 m_domain;
    double      m_site_spacing { 0. };
    double      m_layer_z { std::numeric_limits<double>::quiet_NaN() };
    double      m_layer_width { 0. };
    double      m_layer_height { 0. };
    double      m_row_spacing { 0. };
    int m_wall_count { 0 };
    Voronoi::Section m_section;
    std::vector<Voronoi::Band> m_bands;
};

// Each worker keeps its own engines while processing ascending layer batches.
struct VoronoiInfillCache {
    std::map<std::tuple<double, double, double>, std::shared_ptr<const Voronoi::PointCloudProvider>> rotated_clouds;
    std::map<std::shared_ptr<const Voronoi::PointCloudProvider>, std::shared_ptr<VoronoiInfill>> clouds;
};

} // namespace Slic3r
