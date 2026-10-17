#pragma once

#include "../BoundingBox.hpp"
#include "../ExPolygon.hpp"
#include "../Polyline.hpp"
#include "libslic3r/Point.hpp"
#include "VoronoiInfillCgal.hpp"
#include "VoronoiPointCloud.hpp"
#include <limits>
#include <memory>
#include <stdexcept>
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

    struct GeometryParams {
        double extrusion_spacing; // Nominal single-line spacing used to calibrate point density.
        double density;
        double z;
        double width;
        double height;
        double reference_height = -1.;
        double row_spacing = -1.; // Actual flow spacing for floor rows.
        int wall_count = 1;
        double point_spacing = 0.; // Explicit cloud spacing overrides density calibration.
    };

    // Clip wall sections and floor bands to the fill region, before extrusion.
    Voronoi::Layer fill_geometry(const BoundingBox &object_bounds, const ExPolygon &region,
                                const GeometryParams &params);

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

// Each worker reuses one engine across its ascending layer batch.
struct VoronoiInfillCache {
    std::shared_ptr<VoronoiInfill> infill;
};

} // namespace Slic3r
