#pragma once

#include "FillBase.hpp"
#include "../BoundingBox.hpp"
#include "VoronoiInfill.hpp"
#include "VoronoiRouting.hpp"
#include <memory>
#include <vector>
#include <utility>

namespace Slic3r {

class FillVoronoi : public Fill
{
public:
    FillVoronoi() = default;
    FillVoronoi(const FillVoronoi &other)
        : Fill(other), m_surface_boundary(other.m_surface_boundary), m_floor_support(other.m_floor_support),
          m_infill(std::make_shared<VoronoiInfill>(*other.m_infill)) {}
    FillVoronoi &operator=(const FillVoronoi &other)
        { if (this != &other) *this = FillVoronoi(other); return *this; }
    FillVoronoi &operator=(FillVoronoi &&) = default;
    void set_point_cloud(std::shared_ptr<const Voronoi::PointCloudProvider> provider, VoronoiInfillCache *cache = nullptr)
    {
        if (cache) {
            auto &engine = cache->clouds[provider];
            if (!engine) engine = std::make_shared<VoronoiInfill>(provider);
            m_infill = engine;
        } else {
            m_infill = std::make_shared<VoronoiInfill>(std::move(provider));
        }
    }
    void set_floor_support(ExPolygons support) { m_floor_support = std::move(support); }
    Polylines fill_surface(const Surface *surface, const FillParams &params) override;
    Fill *clone() const override { return new FillVoronoi(*this); }
    bool no_sort() const override { return true; }
    bool is_self_crossing() override { return false; }
    Polylines fill_band(const Voronoi::Band &band, FillParams params) const;
    std::vector<Voronoi::PathGroup> fill_paths(Voronoi::Layer layer, const FillParams &params) const;

protected:
    float _layer_angle(size_t) const override { return 0.f; }
    void _fill_surface_single(const FillParams &params, unsigned int thickness_layers,
                              const std::pair<float, Point> &direction, ExPolygon expolygon,
                              Polylines &polylines_out) override;

private:
    ExPolygon m_surface_boundary;
    ExPolygons m_floor_support;
    std::shared_ptr<VoronoiInfill> m_infill = std::make_shared<VoronoiInfill>();
};

} // namespace Slic3r
