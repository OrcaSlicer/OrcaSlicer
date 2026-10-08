#pragma once

#include "FillBase.hpp"
#include "../BoundingBox.hpp"
#include "VoronoiInfill.hpp"
#include "VoronoiRouting.hpp"
#include "VoronoiHull.hpp"
#include <array>
#include <memory>
#include <vector>
#include <utility>

namespace Slic3r {

class FillAlignedRectilinear;

class FillVoronoi : public Fill
{
public:
    FillVoronoi() : m_provider(std::make_shared<Voronoi::PoissonPointCloud>()), m_infill(m_provider) {}
    void set_point_cloud(std::shared_ptr<const Voronoi::PointCloudProvider> provider)
        { m_provider = std::move(provider); m_infill = VoronoiInfill(m_provider); m_hull_key = {}; }
    void set_hull_threshold(double threshold) { m_hull_threshold = threshold; }
    Fill *clone() const override { return new FillVoronoi(*this); }
    bool no_sort() const override { return true; }
    bool is_self_crossing() override { return false; }
    Polylines fill_band(const Voronoi::Band &band, const FillParams &params) const;
    std::vector<Voronoi::PathGroup> fill_hull(const Voronoi::HullSection &hull, const ExPolygon &region, const FillParams &params) const;
    std::vector<Voronoi::PathGroup> fill_paths(Voronoi::Layer layer, const FillParams &params) const;

protected:
    float _layer_angle(size_t) const override { return 0.f; }
    void _fill_surface_single(const FillParams &params, unsigned int thickness_layers,
                              const std::pair<float, Point> &direction, ExPolygon expolygon,
                              Polylines &polylines_out) override;

private:
    Polylines solid_fill(FillAlignedRectilinear &filler, const ExPolygon &polygon,
                        FillParams params, bool monotonic = false) const;
    Voronoi::HullSection m_hull;
    BoundingBox m_hull_bounds;
    std::array<double, 4> m_hull_key {};
    std::shared_ptr<const Voronoi::PointCloudProvider> m_provider;
    VoronoiInfill m_infill;
    double m_hull_threshold { 0. };
};

} // namespace Slic3r
