#pragma once

#include "FillBase.hpp"
#include "VoronoiInfill.hpp"
#include "VoronoiRouting.hpp"

namespace Slic3r {

class FillVoronoi : public Fill
{
public:
    Fill *clone() const override { return new FillVoronoi(*this); }
    bool no_sort() const override { return true; }
    bool is_self_crossing() override { return false; }
    Polylines fill_band(const Voronoi::Band &band, const FillParams &params) const;
    std::vector<Voronoi::PathGroup> fill_paths(Voronoi::Layer layer, const FillParams &params,
                                             const Point &start_near) const;

protected:
    float _layer_angle(size_t) const override { return 0.f; }
    void _fill_surface_single(const FillParams &params, unsigned int thickness_layers,
                              const std::pair<float, Point> &direction, ExPolygon expolygon,
                              Polylines &polylines_out) override;

private:
    VoronoiInfill m_infill;
};

} // namespace Slic3r
