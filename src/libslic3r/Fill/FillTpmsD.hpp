#ifndef slic3r_FillTpmsD_hpp_
#define slic3r_FillTpmsD_hpp_

#include <utility>

#include "libslic3r/libslic3r.h"
#include "FillBase.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polyline.hpp"

namespace Slic3r {
class Point;

class FillTpmsD : public Fill
{
public:
    FillTpmsD() {}
    Fill* clone() const override { return new FillTpmsD(*this); }

    // require bridge flow since most of this pattern hangs in air
    bool use_bridge_flow() const override { return false; }
  

    // Correction applied to regular infill angle to maximize printing
    // speed in default configuration (degrees)
    static constexpr float CorrectionAngle = -45.;

    void _fill_surface_single(const FillParams&              params,
                              unsigned int                   thickness_layers,
                              const std::pair<float, Point>& direction,
                              ExPolygon                      expolygon,
                              Polylines&                     polylines_out) override;

    bool is_self_crossing() override { return false; }

    // Density adjustment to have a good %of weight.
    static constexpr double DensityAdjust = 2.1;

    // Gyroid upper resolution tolerance (mm^-2)
    static constexpr double PatternTolerance = 0.1;

};

} // namespace Slic3r

#endif // slic3r_FillTpmsD_hpp_