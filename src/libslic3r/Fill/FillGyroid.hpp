#ifndef slic3r_FillGyroid_hpp_
#define slic3r_FillGyroid_hpp_

#include "../libslic3r.h"

#include "FillBase.hpp"

namespace Slic3r {

class FillGyroid : public Fill
{
public:
    FillGyroid() {}
    Fill* clone() const override { return new FillGyroid(*this); }

    // require bridge flow since most of this pattern hangs in air
    bool use_bridge_flow() const override { return false; }
    bool is_self_crossing() override { return false; }

    // Correction applied to regular infill angle to maximize printing
    // speed in default configuration (degrees)
    static constexpr float CorrectionAngle = -45.;

    // Density adjustment to have a good %of weight.
    static constexpr double DensityAdjust = 2.44;

    // Gyroid upper resolution tolerance (mm^-2)
    static constexpr double PatternTolerance = 0.2;


protected:
    void _fill_surface_single(
        const FillParams                &params, 
        unsigned int                     thickness_layers,
        const std::pair<float, Point>   &direction, 
        ExPolygon                        expolygon,
        Polylines                       &polylines_out) override;
};

} // namespace Slic3r

#endif // slic3r_FillGyroid_hpp_
