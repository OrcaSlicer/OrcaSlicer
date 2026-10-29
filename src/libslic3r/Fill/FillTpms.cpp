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
static constexpr float SchwarzPStretch = 1.5f;
static constexpr float SchwarzPLevel   = 0.3f;
static float schwarz_p(float x, float y, float z) { return cosf(x) + cosf(y) + cosf(z * (1.f / SchwarzPStretch)) + SchwarzPLevel; }

// The ring cos(u) + cos(v) = c of a layer of Schwarz P around the origin for 0 <= c < 2, closed and counterclockwise
// from the X axis, within tolerance of the curve. The ring is symmetric about both axes and both diagonals.
static std::vector<Vec2d> schwarz_p_ring(double c, double tolerance)
{
    auto u = [c](double v) { return std::acos(std::clamp(c - std::cos(v), -1., 1.)); };
    // An eighth from the X axis to the diagonal, where u = v = acos(c / 2).
    const double       diagonal = std::acos(0.5 * c);
    std::vector<Vec2d> eighth{Vec2d(u(0.), 0.), Vec2d(diagonal, diagonal)};
    // Split a chord while the curve halfway along it is farther from it than tolerance.
    for (size_t i = 0; i + 1 < eighth.size();) {
        const double v     = 0.5 * (eighth[i].y() + eighth[i + 1].y());
        const Vec2d  mid(u(v), v);
        const Vec2d  chord = eighth[i + 1] - eighth[i];
        if (std::abs(cross2(chord, Vec2d(mid - eighth[i]))) > tolerance * chord.norm())
            eighth.insert(eighth.begin() + i + 1, mid);
        else
            ++i;
    }
    // A quarter up to the Y axis, mirrored across the diagonal, then the other three mirrored across the axes.
    std::vector<Vec2d> quarter = eighth;
    for (auto it = eighth.rbegin() + 1; it != eighth.rend(); ++it)
        quarter.emplace_back(it->y(), it->x());
    std::vector<Vec2d> ring = quarter;
    ring.reserve(4 * quarter.size());
    auto append = [&ring, &quarter](double sx, double sy, bool reversed) {
        for (size_t i = 1; i < quarter.size(); ++i) {
            const Vec2d &p = quarter[reversed ? quarter.size() - 1 - i : i];
            ring.emplace_back(sx * p.x(), sy * p.y());
        }
    };
    append(-1., 1., true);
    append(-1., -1., false);
    append(1., -1., true);
    return ring;
}

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
    if (!tpms.regular_layers())
        return make_tpms_polylines(tpms, Vec3d::Constant(frequency), tpms_sampling_bbox(bbox, cell), this->z, cell);

    // Every layer of the regular cells is a grid of equal rings cos(x) + cos(y) = level, made at once: around (0, 0)
    // for a positive level, around (PI, PI) for a negative one.
    const double level = -(std::cos(frequency * this->z / tpms.scale.z() / SchwarzPStretch) + SchwarzPLevel);
    if (std::abs(level) >= 2.)
        return {};
    const std::vector<Vec2d> ring   = schwarz_p_ring(std::abs(level), std::min(SPARSE_INFILL_RESOLUTION, cell / 8.) * frequency);
    const double             offset = level < 0. ? PI : 0.;
    // The rings reach this far from their centers along the axes.
    const double reach    = std::acos(std::abs(level) - 1.);
    auto         first    = [&](coord_t min) { return int(std::ceil((unscaled(min) * frequency - offset - reach) / (2. * PI))); };
    auto         last     = [&](coord_t max) { return int(std::floor((unscaled(max) * frequency - offset + reach) / (2. * PI))); };
    const double to_coord = 1. / (frequency * SCALING_FACTOR);
    Polylines    polylines;
    for (int i = first(bbox.min.x()); i <= last(bbox.max.x()); ++i)
        for (int j = first(bbox.min.y()); j <= last(bbox.max.y()); ++j) {
            const Vec2d center(2. * PI * i + offset, 2. * PI * j + offset);
            Polyline   &polyline = polylines.emplace_back();
            polyline.points.reserve(ring.size());
            for (const Vec2d &p : ring)
                polyline.points.emplace_back(Vec2d((center + p) * to_coord));
        }
    return polylines;
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
