#include "../ClipperUtils.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "FillTpms.hpp"
#include "AdaptiveInfillField.hpp"
#include "TpmsField.hpp"
#include <cmath>
#include <algorithm>
#include <cstddef>
#include <math.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

// Fischer - Koch S equation:
// cos(2x)sin(y)cos(z) + cos(2y)sin(z)cos(x) + cos(2z)sin(x)cos(y) = 0
static float fischer_koch(float x, float y, float z)
{
    return cosf(2 * x) * sinf(y) * cosf(z) + cosf(2 * y) * sinf(z) * cosf(x) + cosf(2 * z) * sinf(x) * cosf(y);
}

// Schwarz P equation with its cells elongated 1.5 times along z and the level set offset by -0.3:
// cos(x) + cos(y) + cos(z / 1.5) = -0.3. The taller cells and thicker vertical tubes raise the
// compressive and shear strength of the printed lattice at the same filament use.
static float schwarz_p(float x, float y, float z) { return cosf(x) + cosf(y) + cosf(z * (1.f / 1.5f)) + 0.3f; }

// Schoen I-WP equation: 2 (cos(x)cos(y) + cos(y)cos(z) + cos(z)cos(x)) - (cos(2x) + cos(2y) + cos(2z)) = 0
static float schoen_iwp(float x, float y, float z)
{
    return 2 * (cosf(x) * cosf(y) + cosf(y) * cosf(z) + cosf(z) * cosf(x)) - (cosf(2 * x) + cosf(2 * y) + cosf(2 * z));
}

FillTpmsFK::Equation FillTpmsFK::equation() const { return fischer_koch; }
FillTpmsFK::Equation FillTpmsP::equation() const { return schwarz_p; }
FillTpmsFK::Equation FillTpmsIWP::equation() const { return schoen_iwp; }

Polylines FillTpmsFK::make_layer(const TpmsEquation &tpms, double frequency, const BoundingBox &bbox) const
{
    return make_tpms_polylines(tpms, Vec3d::Constant(frequency), bbox, this->z);
}

Polylines FillTpmsP::make_layer(const TpmsEquation &tpms, double frequency, const BoundingBox &bbox) const
{
    // Its cells are 2-3 times smaller than Fischer-Koch's, so lines are traced on a grid that scales with the period.
    const double cell = std::min(TpmsGridCell, 2. * PI / frequency / 32.);
    return make_tpms_polylines(tpms, Vec3d::Constant(frequency), tpms_sampling_bbox(bbox, cell), this->z, cell);
}

Polylines FillTpmsIWP::make_layer(const TpmsEquation &tpms, double frequency, const BoundingBox &bbox) const
{
    // Like TPMS-P, its cells are smaller than Fischer-Koch's.
    const double cell = std::min(TpmsGridCell, 2. * PI / frequency / 32.);
    return make_tpms_polylines(tpms, Vec3d::Constant(frequency), tpms_sampling_bbox(bbox, cell), this->z, cell);
}

void FillTpmsFK::_fill_surface_single(const FillParams&              params,
                                      unsigned int                   thickness_layers,
                                      const std::pair<float, Point>& direction,
                                      ExPolygon                      expolygon,
                                      Polylines&                     polylines_out)
{
    if (params.adaptive_infill_mode == AdaptiveInfillMode::SteppedShells && this->adaptive_infill_field != nullptr) {
        fill_adaptive_infill_shells(*this->adaptive_infill_field, expolygon, this->z - 0.5 * params.layer_height, params, this->spacing,
                         [&](const FillParams &shell_params, const ExPolygon &shell) {
                             this->_fill_surface_single(shell_params, thickness_layers, direction, shell, polylines_out);
                         });
        return;
    }

    const TpmsEquation tpms(equation(), Vec3d(params.tpms_scale.data()), params.tpms_rotation_x);
    auto infill_angle = float(this->angle + (CorrectionAngle * 2 * M_PI) / 360.);
    if (std::abs(infill_angle) >= EPSILON)
        expolygon.rotate(-infill_angle);

    // Density (field period) adjusted to have a good %of weight.
    auto period = [&params, this](float density) { return period_factor() * spacing * params.multiline / std::min(0.9f, density); };

    BoundingBox bbox = expolygon.contour.bounding_box();
    // Enlarge the bounding box by the multi-line width to avoid artifacts at the edges.
    bbox.offset(scale_((params.multiline + 1) * spacing));
    Polylines polylines;
    if (params.adaptive_infill_mode != AdaptiveInfillMode::Disabled && this->adaptive_infill_field != nullptr) {
        polylines = make_adaptive_tpms({tpms, 2. * PI / period(params.density), 2. * PI / period(params.adaptive_infill_interior_density),
                                        params.adaptive_infill_gradient},
                                       *this->adaptive_infill_field, bbox, this->z, params.layer_height, spacing, infill_angle);
    } else {
        polylines = make_layer(tpms, float(2 * PI) / float(period(params.density)), bbox);
    }

    // Apply multiline offset if needed
    multiline_fill(polylines, params, spacing);

    // Prune the lines within the expolygon.
    polylines = intersection_pl(std::move(polylines), expolygon);

    if (!polylines.empty()) {
        // Remove very small bits, but be careful to not remove infill lines connecting thin walls!
        // The infill perimeter lines should be separated by around a single infill line width.
        const double minlength = scale_(0.8 * this->spacing);
        polylines.erase(std::remove_if(polylines.begin(), polylines.end(),
                                       [minlength](const Polyline& pl) { return pl.length() < minlength; }),
                        polylines.end());
    }

    if (!polylines.empty()) {
        // connect lines
        size_t polylines_out_first_idx = polylines_out.size();

        // chain_or_connect_infill(std::move(polylines), expolygon, polylines_out, this->spacing, params);
        // chain_infill not situable for this pattern due to internal "islands", this also affect performance a lot.
        connect_infill(std::move(polylines), expolygon, polylines_out, this->spacing, params);

        // new paths must be rotated back
        if (std::abs(infill_angle) >= EPSILON) {
            for (auto it = polylines_out.begin() + polylines_out_first_idx; it != polylines_out.end(); ++it)
                it->rotate(infill_angle);
        }
    }
}

} // namespace Slic3r
