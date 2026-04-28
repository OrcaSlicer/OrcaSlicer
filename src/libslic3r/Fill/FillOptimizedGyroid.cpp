// FillOptimizedGyroid.cpp
// Physics-optimized gyroid infill for ElegooSlicer.
//
// Derived directly from FillGyroid.cpp.  Two parameters are auto-tuned:
//   omega     — spatial-frequency multiplier (wavelength control)
//   amplitude — wave-height scale
//
// Physics derivations:
//   omega:     Euler-Bernoulli buckling  → omega = sqrt(density_adj), [0.5, 2.0]
//   amplitude: Curved-beam bending stress → A = 0.55/omega^2,        [0.20, 0.65]

#include "../ClipperUtils.hpp"
#include "../ShortestPath.hpp"
#include "../Surface.hpp"
#include <cmath>
#include <algorithm>
#include "FillBase.hpp"
#include "FillOptimizedGyroid.hpp"

namespace Slic3r {

// ─────────────────────────────────────────────────────────────────────────────
// Physics auto-tuning
// ─────────────────────────────────────────────────────────────────────────────

double FillOptimizedGyroid::compute_omega_factor(double density_adjusted,
                                                  double line_spacing,
                                                  double layer_height)
{
    double lh_ratio   = (line_spacing > 0.) ? layer_height / line_spacing : 0.5;
    double correction = 1.0 / std::sqrt(1.0 + lh_ratio);
    double raw        = std::sqrt(density_adjusted) * correction;
    return std::clamp(raw, 0.5, 2.0);
}

double FillOptimizedGyroid::compute_amplitude_factor(double /*density_adjusted*/,
                                                      double omega)
{
    constexpr double k = 0.55;
    double raw = (omega > 1e-9) ? k / (omega * omega) : 0.65;
    return std::clamp(raw, 0.20, 0.65);
}

// ─────────────────────────────────────────────────────────────────────────────
// Modified gyroid surface function
// Same as FillGyroid's f() with x scaled by omega and output by amplitude.
// ─────────────────────────────────────────────────────────────────────────────

static inline double f_opt(double x, double z_sin, double z_cos,
                            double omega, double amplitude,
                            bool vertical, bool flip)
{
    const double ox = omega * x;
    double result;

    if (vertical) {
        double phase_offset = (z_cos < 0 ? M_PI : 0) + M_PI;
        double a   = sin(ox + phase_offset);
        double b   = -z_cos;
        double res = z_sin * cos(ox + phase_offset + (flip ? M_PI : 0.));
        double r   = sqrt(sqr(a) + sqr(b));
        result     = asin(a / r) + asin(res / r) + M_PI;
    } else {
        double phase_offset = z_sin < 0 ? M_PI : 0.;
        double a   = cos(ox + phase_offset);
        double b   = -z_sin;
        double res = z_cos * sin(ox + phase_offset + (flip ? 0 : M_PI));
        double r   = sqrt(sqr(a) + sqr(b));
        result     = asin(a / r) + asin(res / r) + 0.5 * M_PI;
    }

    return amplitude * result;
}

// ─────────────────────────────────────────────────────────────────────────────
// One-period generator (adaptive resolution)
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<Vec2d> make_one_period_opt(
    double width, double scaleFactor,
    double z_cos, double z_sin,
    double omega, double amplitude,
    bool vertical, bool flip,
    double tolerance)
{
    std::vector<Vec2d> points;
    double dx    = M_PI_2 / omega;
    double limit = std::min(2. * M_PI, width);
    points.reserve(static_cast<size_t>(std::ceil(limit / tolerance / 3)));

    for (double x = 0.; x < limit - EPSILON; x += dx)
        points.emplace_back(Vec2d(x, f_opt(x, z_sin, z_cos, omega, amplitude, vertical, flip)));
    points.emplace_back(Vec2d(limit, f_opt(limit, z_sin, z_cos, omega, amplitude, vertical, flip)));

    for (;;) {
        size_t size = points.size();
        for (size_t i = 1; i < size; ++i) {
            const Vec2d& lp = points[i - 1];
            const Vec2d& rp = points[i];
            double x  = lp(0) + (rp(0) - lp(0)) / 2.;
            double y  = f_opt(x, z_sin, z_cos, omega, amplitude, vertical, flip);
            Vec2d  ip = {x, y};
            if (std::abs(cross2(Vec2d(ip - lp), Vec2d(ip - rp))) > sqr(tolerance))
                points.emplace_back(std::move(ip));
        }
        if (size == points.size())
            break;
        std::sort(points.begin(), points.end(),
                  [](const Vec2d& a, const Vec2d& b) { return a(0) < b(0); });
    }

    return points;
}

// ─────────────────────────────────────────────────────────────────────────────
// Wave builder
// ─────────────────────────────────────────────────────────────────────────────

static Polyline make_wave_opt(
    const std::vector<Vec2d>& one_period,
    double width, double height, double offset, double scaleFactor,
    double z_cos, double z_sin,
    double omega, double amplitude,
    bool vertical, bool flip)
{
    std::vector<Vec2d> points = one_period;
    double period = points.back()(0);

    if (width != period) {
        points.reserve(one_period.size() * size_t(std::floor(width / period)));
        points.pop_back();
        size_t n = points.size();
        do {
            points.emplace_back(points[points.size() - n].x() + period,
                                points[points.size() - n].y());
        } while (points.back()(0) < width - EPSILON);
        points.emplace_back(Vec2d(width,
            f_opt(width, z_sin, z_cos, omega, amplitude, vertical, flip)));
    }

    Polyline polyline;
    polyline.points.reserve(points.size());
    for (auto& point : points) {
        point(1) += offset;
        point(1)  = std::clamp(double(point.y()), 0., height);
        if (vertical)
            std::swap(point(0), point(1));
        polyline.points.emplace_back((point * scaleFactor).cast<coord_t>());
    }
    return polyline;
}

// ─────────────────────────────────────────────────────────────────────────────
// Full-layer wave assembly
// ─────────────────────────────────────────────────────────────────────────────

static Polylines make_optimized_gyroid_waves(
    double gridZ,
    double density_adjusted,
    double line_spacing,
    double width,
    double height,
    double omega,
    double amplitude)
{
    const double scaleFactor = scale_(line_spacing) / density_adjusted;
    const double tolerance   = std::min(line_spacing / 2.,
                                        FillOptimizedGyroid::PatternTolerance)
                               / unscale<double>(scaleFactor);

    const double z     = gridZ / scaleFactor;
    const double z_sin = sin(z);
    const double z_cos = cos(z);

    bool   vertical    = (std::abs(z_sin) <= std::abs(z_cos));
    double lower_bound = 0.;
    double upper_bound = height;
    bool   flip        = true;

    if (vertical) {
        flip        = false;
        lower_bound = -M_PI;
        upper_bound = width - M_PI_2;
        std::swap(width, height);
    }

    auto one_period_odd  = make_one_period_opt(width, scaleFactor,
                                                z_cos, z_sin,
                                                omega, amplitude,
                                                vertical, flip, tolerance);
    flip = !flip;
    auto one_period_even = make_one_period_opt(width, scaleFactor,
                                                z_cos, z_sin,
                                                omega, amplitude,
                                                vertical, flip, tolerance);

    Polylines result;
    for (double y0 = lower_bound; y0 < upper_bound + EPSILON; y0 += M_PI) {
        result.emplace_back(make_wave_opt(one_period_odd,
                                          width, height, y0, scaleFactor,
                                          z_cos, z_sin, omega, amplitude,
                                          vertical, flip));
        y0 += M_PI;
        if (y0 < upper_bound + EPSILON)
            result.emplace_back(make_wave_opt(one_period_even,
                                              width, height, y0, scaleFactor,
                                              z_cos, z_sin, omega, amplitude,
                                              vertical, flip));
    }
    return result;
}

// Required to fix constexpr linkage on macOS (same pattern as FillGyroid)
constexpr double FillOptimizedGyroid::PatternTolerance;

// ─────────────────────────────────────────────────────────────────────────────
// _fill_surface_single — called by the slicer once per layer region
// ─────────────────────────────────────────────────────────────────────────────

void FillOptimizedGyroid::_fill_surface_single(
    const FillParams              &params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point> &direction,
    ExPolygon                      expolygon,
    Polylines                     &polylines_out)
{
    auto infill_angle = float(this->angle + (CorrectionAngle * 2. * M_PI) / 360.);
    if (std::abs(infill_angle) >= EPSILON)
        expolygon.rotate(-infill_angle);

    BoundingBox bb = expolygon.contour.bounding_box();

    double  density_adjusted = std::max(0., params.density * DensityAdjust
                                            / params.multiline);
    coord_t distance         = coord_t(scale_(this->spacing) / density_adjusted);

    // Auto-tune omega and amplitude from density, line spacing, and layer height.
    // layer_height is provided by the slicer in FillParams per region.
    const double lh = (params.layer_height > 0.) ? double(params.layer_height)
                                                 : double(this->spacing);
    const double omega     = compute_omega_factor(density_adjusted,
                                                   this->spacing,
                                                   lh);
    const double amplitude = compute_amplitude_factor(density_adjusted, omega);

    bb.merge(align_to_grid(bb.min, Point(2 * M_PI * distance,
                                         2 * M_PI * distance)));
    bb.offset(10 * coord_t(scale_(this->spacing)));

    Polylines polylines = make_optimized_gyroid_waves(
        scale_(this->z),
        density_adjusted,
        this->spacing,
        std::ceil(bb.size()(0) / distance) + 1.,
        std::ceil(bb.size()(1) / distance) + 1.,
        omega,
        amplitude);

    for (Polyline &pl : polylines)
        pl.translate(bb.min);

    multiline_fill(polylines, params, spacing);

    polylines = intersection_pl(polylines, expolygon);

    if (!polylines.empty()) {
        const double minlength = scale_(0.8 * this->spacing);
        polylines.erase(
            std::remove_if(polylines.begin(), polylines.end(),
                [minlength](const Polyline &pl) {
                    return pl.length() < minlength;
                }),
            polylines.end());
    }

    if (!polylines.empty()) {
        size_t first_idx = polylines_out.size();
        chain_or_connect_infill(std::move(polylines), expolygon,
                                polylines_out, this->spacing, params);

        if (std::abs(infill_angle) >= EPSILON)
            for (auto it = polylines_out.begin() + first_idx;
                 it != polylines_out.end(); ++it)
                it->rotate(infill_angle);
    }
}

} // namespace Slic3r
