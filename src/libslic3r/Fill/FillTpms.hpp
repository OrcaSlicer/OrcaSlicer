#ifndef slic3r_FillTpms_hpp_
#define slic3r_FillTpms_hpp_

#include <utility>

#include "FillBase.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polyline.hpp"

namespace Slic3r {
class Point;
struct TpmsEquation;

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
    // Lines of the layer over bbox, at the frequency of the regular pattern in radians per mm.
    virtual Polylines make_layer(const TpmsEquation &tpms, double frequency, const BoundingBox &bbox) const;
};

// Schwarz Primitive (P) surface, elongated along z: cos(x) + cos(y) + cos(z / 1.5) = -0.3.
class FillTpmsP : public FillTpmsFK
{
public:
    Fill* clone() const override { return new FillTpmsP(*this); }

protected:
    Equation  equation() const override;
    float     period_factor() const override { return 1.805f; }
    Polylines make_layer(const TpmsEquation &tpms, double frequency, const BoundingBox &bbox) const override;
};

// Schoen I-WP surface: 2 (cos x cos y + cos y cos z + cos z cos x) - (cos 2x + cos 2y + cos 2z) = 0.
class FillTpmsIWP : public FillTpmsFK
{
public:
    Fill* clone() const override { return new FillTpmsIWP(*this); }

protected:
    Equation  equation() const override;
    float     period_factor() const override { return 2.683f; }
    Polylines make_layer(const TpmsEquation &tpms, double frequency, const BoundingBox &bbox) const override;
};

} // namespace Slic3r

#endif // slic3r_FillTpms_hpp_
