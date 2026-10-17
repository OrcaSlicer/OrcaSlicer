#pragma once

#include "../BoundingBox.hpp"
#include "../ExPolygon.hpp"
#include "../Polyline.hpp"
#include "libslic3r/Point.hpp"
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
    Polylines route_walls(Polylines walls, size_t layer_id) const;

private:
    Voronoi::ActivePointCloud m_cloud;
    Voronoi::CellGeometry m_geometry;
    size_t m_geometry_revision = std::numeric_limits<size_t>::max();
    BoundingBox m_bounds;
    double      m_site_spacing { 0. };
    double      m_layer_z { std::numeric_limits<double>::quiet_NaN() };
    double      m_layer_width { 0. };
    double      m_layer_height { 0. };
    double      m_row_spacing { 0. };
    int m_wall_count { 0 };
    Voronoi::Section m_section;
    std::vector<Voronoi::Band> m_bands;
};

// Successive fillers in an ascending worker batch reuse the same engine.
// Clients are sequential; concurrent workers own independent engines.
struct VoronoiEngineRegistry {
    std::shared_ptr<VoronoiInfill> infill;
};

} // namespace Slic3r
