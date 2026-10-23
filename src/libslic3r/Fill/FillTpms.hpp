#ifndef slic3r_FillTpms_hpp_
#define slic3r_FillTpms_hpp_

#include <algorithm>
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
    // Marching-squares grid cell in mm for a field period in mm. 0.4 mm suits Fischer-Koch's large cells.
    virtual float trace_cell(float /*period*/) const { return 0.4f; }
};

// Schwarz Primitive (P) surface, elongated along z: cos(x) + cos(y) + cos(z / 1.5) = -0.3.
class FillTpmsP : public FillTpmsFK
{
public:
    Fill* clone() const override { return new FillTpmsP(*this); }

protected:
    Equation equation() const override;
    float    period_factor() const override { return 1.805f; }
    // Its cells are 2-3 times smaller than Fischer-Koch's, so trace them on a grid that scales with the period.
    float    trace_cell(float period) const override { return std::min(0.4f, period / 32.f); }
};

// Schoen I-WP surface: 2 (cos x cos y + cos y cos z + cos z cos x) - (cos 2x + cos 2y + cos 2z) = 0.
class FillTpmsIWP : public FillTpmsFK
{
public:
    Fill* clone() const override { return new FillTpmsIWP(*this); }

protected:
    Equation equation() const override;
    float    period_factor() const override { return 2.62f; }
    float    trace_cell(float period) const override { return std::min(0.4f, period / 32.f); }
};

} // namespace Slic3r

#endif // slic3r_FillTpms_hpp_