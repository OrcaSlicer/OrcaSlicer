#ifndef slic3r_FillTpms_hpp_
#define slic3r_FillTpms_hpp_

#include <utility>

#include "FillBase.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PrintConfig.hpp"

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

// The TPMS surface of a TPMS type with cells smaller than Fischer-Koch's, traced on a grid that scales with its period.
class FillTpmsTraced : public FillTpmsFK
{
public:
    explicit FillTpmsTraced(InfillPattern type);
    Fill* clone() const override { return new FillTpmsTraced(*this); }

protected:
    Equation  equation() const override { return m_equation; }
    float     period_factor() const override { return m_period_factor; }
    Polylines make_layer(const TpmsEquation &tpms, double frequency, const BoundingBox &bbox) const override;

private:
    Equation m_equation;
    float    m_period_factor;
};

// Schwarz Primitive (P) surface, elongated along z: cos(x) + cos(y) + cos(z / 1.5) = -0.3.
class FillTpmsP : public FillTpmsTraced
{
public:
    FillTpmsP() : FillTpmsTraced(ipTpmsP) {}
    Fill* clone() const override { return new FillTpmsP(*this); }

protected:
    Polylines make_layer(const TpmsEquation &tpms, double frequency, const BoundingBox &bbox) const override;
};

} // namespace Slic3r

#endif // slic3r_FillTpms_hpp_
