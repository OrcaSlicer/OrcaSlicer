#ifndef slic3r_FillTpmsFK_hpp_
#define slic3r_FillTpmsFK_hpp_

#include <utility>

#include "FillBase.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polyline.hpp"

namespace Slic3r {
class Point;

class FillTpmsFK : public Fill
{
public:
    FillTpmsFK() {}
    Fill* clone() const override { return new FillTpmsFK(*this); }

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
    bool aligned_to_origin() const override { return true; }

protected:
    // Implicit TPMS equation with a period of 2 PI.
    using Equation = float (*)(float x, float y, float z);
    virtual Equation equation() const;
    // Field period per (line spacing / density), calibrated so the line length matches the density.
    virtual float period_factor() const { return 4.18f; }
};

// Schwarz Primitive (P) surface: cos(x) + cos(y) + cos(z) = 0. Traced like Fischer-Koch, only the equation differs.
class FillTpmsP : public FillTpmsFK
{
public:
    Fill* clone() const override { return new FillTpmsP(*this); }

protected:
    Equation equation() const override;
    // Its cells are small, so above ~50% density they get coarse on the 0.4 mm marching-squares grid (ScalarField::gsizef).
    float    period_factor() const override { return 1.85f; }
};

} // namespace Slic3r

#endif // slic3r_FillTpmsFK_hpp_