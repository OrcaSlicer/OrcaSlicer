#ifndef slic3r_FillOptimizedGyroid_hpp_
#define slic3r_FillOptimizedGyroid_hpp_

#include "../libslic3r.h"
#include "FillBase.hpp"

namespace Slic3r {

// Physics-optimized gyroid infill.
// Identical interface to FillGyroid; two internal parameters (omega, amplitude)
// are auto-computed from density and spacing — not exposed to the user.
//
// omega:     spatial-frequency multiplier — Euler-Bernoulli buckling theory
// amplitude: wave-height scale            — curved-beam bending-stress theory
class FillOptimizedGyroid : public Fill
{
public:
    FillOptimizedGyroid() {}
    Fill* clone() const override { return new FillOptimizedGyroid(*this); }

    bool use_bridge_flow() const override { return false; }
    bool is_self_crossing() override { return false; }

    static constexpr float  CorrectionAngle  = -45.f;
    static constexpr double DensityAdjust    = 2.44;
    static constexpr double PatternTolerance = 0.2;

    static double compute_omega_factor(double density_adjusted,
                                       double line_spacing,
                                       double layer_height);
    static double compute_amplitude_factor(double density_adjusted,
                                           double omega);

protected:
    void _fill_surface_single(
        const FillParams                &params,
        unsigned int                     thickness_layers,
        const std::pair<float, Point>   &direction,
        ExPolygon                        expolygon,
        Polylines                       &polylines_out) override;
};

} // namespace Slic3r

#endif // slic3r_FillOptimizedGyroid_hpp_
