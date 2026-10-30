#ifndef slic3r_FillTpms_hpp_
#define slic3r_FillTpms_hpp_

#include <utility>

#include "FillBase.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {
class Point;

// Gyroid and the TPMS patterns. What sets each pattern apart is in its row of the table of surfaces in FillTpms.cpp.
class FillTpms : public Fill
{
public:
    explicit FillTpms(InfillPattern pattern);
    Fill* clone() const override { return new FillTpms(*this); }

    // require bridge flow since most of this pattern hangs in air
    bool use_bridge_flow() const override { return false; }
    bool is_self_crossing() override { return false; }
    bool aligned_to_origin() const override { return true; }
    bool scales_its_cells() const override { return true; }

    // Correction applied to regular infill angle to maximize printing
    // speed in default configuration (degrees)
    static constexpr float CorrectionAngle = -45.;

    // Period factor of Gyroid, from the density adjustment of 2.44 it is calibrated with.
    static constexpr double GyroidPeriodFactor = 2. * PI / 2.44;

    struct Surface;

protected:
    void _fill_surface_single(const FillParams&              params,
                              unsigned int                   thickness_layers,
                              const std::pair<float, Point>& direction,
                              ExPolygon                      expolygon,
                              Polylines&                     polylines_out) override;

private:
    const Surface *m_surface;
};

} // namespace Slic3r

#endif // slic3r_FillTpms_hpp_
