#ifndef slic3r_FillRadial_hpp_
#define slic3r_FillRadial_hpp_

#include "FillBase.hpp"

namespace Slic3r {

// Bridge fill for annular surfaces: draws spokes from the inner hole
// straight out to the outer contour instead of parallel lines, so no span
// is much longer than necessary. Falls back to FillRectilinear when the
// surface has no hole to radiate from.
class FillRadial : public Fill
{
public:
    ~FillRadial() override = default;
    bool is_self_crossing() override { return false; }
    // require bridge flow since this pattern only makes sense for bridges
    bool use_bridge_flow() const override { return true; }

protected:
    Fill* clone() const override { return new FillRadial(*this); };
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

#endif // slic3r_FillRadial_hpp_
