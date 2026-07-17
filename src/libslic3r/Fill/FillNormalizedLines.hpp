#ifndef slic3r_FillNormalizedLines_hpp_
#define slic3r_FillNormalizedLines_hpp_

#include "FillBase.hpp"

namespace Slic3r {

// Bridge fill that follows the local curvature of the bridge boundary: along
// any stretch of boundary that curves toward the fill area (an inner hole,
// or an inward notch of the outer contour), lines are cast along the local
// normal instead of a single straight-line direction across the whole
// bridge, so no span is much longer than necessary. Falls back to
// FillRectilinear/FillMonotonic wherever the boundary is straight.
class FillNormalizedLines : public Fill
{
public:
    ~FillNormalizedLines() override = default;
    bool is_self_crossing() override { return false; }
    // require bridge flow since this pattern only makes sense for bridges
    bool use_bridge_flow() const override { return true; }

protected:
    Fill* clone() const override { return new FillNormalizedLines(*this); };
    void _fill_surface_single(
        const FillParams                &params,
        unsigned int                     thickness_layers,
        const std::pair<float, Point>   &direction,
        ExPolygon                        expolygon,
        Polylines                       &polylines_out) override;

    void _fill_surface_single(const FillParams& params,
        unsigned int                   thickness_layers,
        const std::pair<float, Point>& direction,
        ExPolygon                      expolygon,
        ThickPolylines& thick_polylines_out) override;

    bool no_sort() const override { return true; }
};

} // namespace Slic3r

#endif // slic3r_FillNormalizedLines_hpp_
