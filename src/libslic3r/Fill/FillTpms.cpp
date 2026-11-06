#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <math.h>
#include <utility>
#include <vector>

#include "../ClipperUtils.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"
#include "AdaptiveInfillField.hpp"
#include "FillBase.hpp"
#include "FillTpms.hpp"
#include "TpmsField.hpp"

namespace Slic3r {

// The equations of the surfaces, with a period of 2 PI.

static float gyroid(float x, float y, float z)
{
    return std::sin(x) * std::cos(y) + std::sin(y) * std::cos(z) + std::sin(z) * std::cos(x);
}

static float schwarz_d(float x, float y, float z)
{
    return std::sin(x) * std::sin(y) * std::sin(z) - std::cos(x) * std::cos(y) * std::cos(z);
}

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

// Schoen I-WP: 2(cos(x)cos(y) + cos(y)cos(z) + cos(z)cos(x)) - (cos(2x) + cos(2y) + cos(2z)) = 0
static float schoen_iwp(float x, float y, float z)
{
    return 2 * (cosf(x) * cosf(y) + cosf(y) * cosf(z) + cosf(z) * cosf(x)) - (cosf(2 * x) + cosf(2 * y) + cosf(2 * z));
}

// Neovius: 3(cos(x) + cos(y) + cos(z)) + 4cos(x)cos(y)cos(z) = 0
static float neovius(float x, float y, float z) { return 3 * (cosf(x) + cosf(y) + cosf(z)) + 4 * cosf(x) * cosf(y) * cosf(z); }

// Schoen F-RD: 4cos(x)cos(y)cos(z) - (cos(2x)cos(2y) + cos(2y)cos(2z) + cos(2z)cos(2x)) = 0
static float schoen_frd(float x, float y, float z)
{
    return 4 * cosf(x) * cosf(y) * cosf(z) - (cosf(2 * x) * cosf(2 * y) + cosf(2 * y) * cosf(2 * z) + cosf(2 * z) * cosf(2 * x));
}

// Lidinoid: (sin(2x)cos(y)sin(z) + sin(2y)cos(z)sin(x) + sin(2z)cos(x)sin(y)) / 2
//           - (cos(2x)cos(2y) + cos(2y)cos(2z) + cos(2z)cos(2x)) / 2 + 0.15 = 0
static float lidinoid(float x, float y, float z)
{
    return 0.5f * (sinf(2 * x) * cosf(y) * sinf(z) + sinf(2 * y) * cosf(z) * sinf(x) + sinf(2 * z) * cosf(x) * sinf(y)) -
           0.5f * (cosf(2 * x) * cosf(2 * y) + cosf(2 * y) * cosf(2 * z) + cosf(2 * z) * cosf(2 * x)) + 0.15f;
}

// Split P: 1.1(sin(2x)sin(z)cos(y) + sin(2y)sin(x)cos(z) + sin(2z)sin(y)cos(x))
//          - 0.2(cos(2x)cos(2y) + cos(2y)cos(2z) + cos(2z)cos(2x)) - 0.4(cos(2x) + cos(2y) + cos(2z)) = 0
static float split_p(float x, float y, float z)
{
    return 1.1f * (sinf(2 * x) * sinf(z) * cosf(y) + sinf(2 * y) * sinf(x) * cosf(z) + sinf(2 * z) * sinf(y) * cosf(x)) -
           0.2f * (cosf(2 * x) * cosf(2 * y) + cosf(2 * y) * cosf(2 * z) + cosf(2 * z) * cosf(2 * x)) -
           0.4f * (cosf(2 * x) + cosf(2 * y) + cosf(2 * z));
}

// A layer of a TPMS pattern in the fill frame.
struct TpmsLayer
{
    const TpmsEquation &tpms;
    coordf_t            z;
    double              spacing;
    int                 multiline;
    // Line spacing over the distance between the waves of the regular pattern, and its frequency in radians per mm.
    double              density_adjusted;
    double              frequency;
    // The box of the region, and with margins for the ends of the lines and their multiline copies.
    BoundingBox         bbox;
    BoundingBox         margin_bbox;
};

// Grid cell in mm of the surfaces with cells smaller than Fischer-Koch's, at most a 32nd of their period.
static double small_cell(double frequency) { return std::min(TpmsGridCell, 2. * PI / frequency / 32.); }

// ---------------------------------------------------------------------------
// Gyroid: waves solved from its equation, and the optimized Gyroid.

// Gyroid upper resolution tolerance (mm^-2)
static constexpr double GyroidPatternTolerance = 0.2;

static inline double f(double x, double z_sin, double z_cos, bool vertical, bool flip)
{
    if (vertical) {
        double phase_offset = (z_cos < 0 ? M_PI : 0) + M_PI;
        double a   = sin(x + phase_offset);
        double b   = - z_cos;
        double res = z_sin * cos(x + phase_offset + (flip ? M_PI : 0.));
        double r   = sqrt(sqr(a) + sqr(b));
        return asin(a/r) + asin(res/r) + M_PI;
    }
    else {
        double phase_offset = z_sin < 0 ? M_PI : 0.;
        double a   = cos(x + phase_offset);
        double b   = - z_sin;
        double res = z_cos * sin(x + phase_offset + (flip ? 0 : M_PI));
        double r   = sqrt(sqr(a) + sqr(b));
        return (asin(a/r) + asin(res/r) + 0.5 * M_PI);
    }
}

// Repeats one period of a wave from the last sample at or before x_min to the first one at or after x_max.
static inline Polyline make_wave(
    const std::vector<Vec2d>& one_period, double x_min, double x_max, double offset, double scaleFactor, bool vertical)
{
    const double period = one_period.back().x();
    // The last sample of a period is the first one of the next.
    const size_t n  = one_period.size() - 1;
    double       x0 = std::floor(x_min / period) * period;
    size_t       i  = 0;
    while (i + 1 < n && x0 + one_period[i + 1].x() <= x_min)
        ++i;

    Polyline polyline;
    polyline.points.reserve(size_t((x_max - x0) / period + 1.) * n + 1);
    for (;;) {
        Vec2d      point(x0 + one_period[i].x(), one_period[i].y() + offset);
        const bool last = point.x() >= x_max;
        if (vertical)
            std::swap(point(0), point(1));
        polyline.points.emplace_back((point * scaleFactor).cast<coord_t>());
        if (last)
            break;
        if (++i == n) {
            i = 0;
            x0 += period;
        }
    }
    return polyline;
}

static std::vector<Vec2d> make_one_period(double scaleFactor, double z_cos, double z_sin, bool vertical, bool flip, double tolerance)
{
    std::vector<Vec2d> points;
    double dx = M_PI_2; // exact coordinates on main inflexion lobes
    double limit = 2*M_PI;
    points.reserve(coord_t(ceil(limit / tolerance / 3)));

    for (double x = 0.; x < limit - EPSILON; x += dx) {
        points.emplace_back(Vec2d(x, f(x, z_sin, z_cos, vertical, flip)));
    }
    points.emplace_back(Vec2d(limit, f(limit, z_sin, z_cos, vertical, flip)));

    // piecewise increase in resolution up to requested tolerance
    for(;;)
    {
        size_t size = points.size();
        for (unsigned int i = 1;i < size; ++i) {
            auto& lp = points[i-1]; // left point
            auto& rp = points[i];   // right point
            double x = lp(0) + (rp(0) - lp(0)) / 2;
            double y = f(x, z_sin, z_cos, vertical, flip);
            Vec2d ip = {x, y};
            if (std::abs(cross2(Vec2d(ip - lp), Vec2d(ip - rp))) > sqr(tolerance)) {
                points.emplace_back(std::move(ip));
            }
        }

        if (size == points.size())
            break;
        else
        {
            // insert new points in order
            std::sort(points.begin(), points.end(),
                      [](const Vec2d &lhs, const Vec2d &rhs) { return lhs(0) < rhs(0); });
        }
    }

    return points;
}

// ---------------------------------------------------------------------------
// "Optimized" gyroid wave: marching-squares variant gated on
// params.gyroid_optimized. The wave shape is extracted from the gyroid
// implicit scalar field (see TpmsField) at iso=0, with
// the Z dimension's spatial frequency multiplied by an Euler-Bernoulli
// buckling-derived factor so the vertical strands become shorter columns,
// raising the critical buckling load against Z-axis compression.
//
// The formula is INVERTED from a naive "scale with density" derivation:
// at LOW density the gyroid strands are long and slender (prime buckling
// targets), so they need the most shortening; at high density the strands
// are already short and need little extra help. omega is therefore the
// inverse-square-root of density_adjusted:
//
//   omega = sqrt(1 / density_adj) / sqrt(1 + layer_h/spacing),
//           clamped [1.0, 2.0]
//
// fx and fy are left at the baseline frequency, so the per-XY-slice line
// length per unit area is preserved -> mass at the same `sparse_infill_density`
// setting matches the standard gyroid path. Strength gain comes purely from
// the shorter vertical column length (P_cr proportional to 1/L^2).
//
// Empirical Python sim (sim_gyroid_compare.py) at layer_h=0.20, spacing=0.45:
//
//   density   omega   line/std   strength/std   strength_per_mass
//     10%      2.00     1.00        2.84             2.84
//     15%      1.38     1.00        1.89             1.89
//     20%      1.19     1.00        1.42             1.42
//     30%      1.00     1.00        1.00             1.00
//     50%+     1.00     1.00        1.00             1.00
//
// When gyroid_optimized is false and every layer is a layer of the regular
// pattern, behavior is byte-identical to the standard parametric gyroid path below.
// ---------------------------------------------------------------------------

static inline double compute_omega_factor(double density_adjusted, double line_spacing, double layer_height)
{
    double lh_ratio   = (line_spacing > 0.) ? layer_height / line_spacing : 0.5;
    double correction = 1.0 / std::sqrt(1.0 + lh_ratio);
    double raw        = std::sqrt(1.0 / std::max(density_adjusted, 0.1)) * correction;
    return std::clamp(raw, 1.0, 2.0);
}

// Waves covering bbox, with the pattern anchored at origin.
static Polylines make_gyroid_waves(double gridZ, double density_adjusted, double line_spacing, const BoundingBox &bbox, const Point &origin)
{
    const double scaleFactor = scale_(line_spacing) / density_adjusted;

    // tolerance in scaled units. clamp the maximum tolerance as there's
    // no processing-speed benefit to do so beyond a certain point
    const double tolerance = std::min(line_spacing / 2, GyroidPatternTolerance) / unscale<double>(scaleFactor);

    //scale factor for 5% : 8 712 388
    // 1z = 10^-6 mm ?
    const double z     = gridZ / scaleFactor;
    const double z_sin = sin(z);
    const double z_cos = cos(z);

    bool vertical = (std::abs(z_sin) <= std::abs(z_cos));
    // Range to cover in pattern units, with the waves running along x.
    Vec2d lo = (bbox.min - origin).cast<double>() / scaleFactor;
    Vec2d hi = (bbox.max - origin).cast<double>() / scaleFactor;
    double lower_bound = 0.;
    bool flip = true;
    if (vertical) {
        flip = false;
        lower_bound = -M_PI;
        std::swap(lo(0), lo(1));
        std::swap(hi(0), hi(1));
    }

    std::vector<Vec2d> one_period_odd = make_one_period(scaleFactor, z_cos, z_sin, vertical, flip, tolerance); // creates one period of the waves, so it doesn't have to be recalculated all the time
    flip = !flip;                                                                   // even polylines are a bit shifted
    std::vector<Vec2d> one_period_even = make_one_period(scaleFactor, z_cos, z_sin, vertical, flip, tolerance);

    // Every wave spans [offset + f_min, offset + f_max] across.
    double f_min = std::numeric_limits<double>::max();
    double f_max = std::numeric_limits<double>::lowest();
    for (const std::vector<Vec2d> *one_period : { &one_period_odd, &one_period_even })
        for (const Vec2d &point : *one_period) {
            f_min = std::min(f_min, point.y());
            f_max = std::max(f_max, point.y());
        }

    Polylines result;
    for (int i = int(std::ceil((lo.y() - f_max - lower_bound) / M_PI)); lower_bound + i * M_PI + f_min <= hi.y(); ++i) {
        Polyline &wave = result.emplace_back(make_wave(i % 2 == 0 ? one_period_odd : one_period_even, lo.x(), hi.x(),
                                                       lower_bound + i * M_PI, scaleFactor, vertical));
        wave.translate(origin);
    }

    return result;
}

// The waves of a layer of the regular cells.
static Polylines gyroid_waves(const TpmsLayer &layer)
{
    BoundingBox bb = layer.bbox;
    // Distance between the gyroid waves in scaled coordinates.
    const coord_t distance = coord_t(scale_(layer.spacing) / layer.density_adjusted);
    // Anchor the pattern to our grid module; the 10-line shift keeps its established phase.
    const coord_t shift  = coord_t(10 * scale_(layer.spacing));
    const Point   origin = align_to_grid(bb.min, Point(2*M_PI*distance, 2*M_PI*distance)) - Point(shift, shift);
    // Keep the pattern ends and the multiline copies outside the contour.
    bb.offset(scale_(layer.spacing * layer.multiline));
    return make_gyroid_waves(scale_(layer.z) / layer.tpms.scale.z(), layer.density_adjusted, layer.spacing, bb, origin);
}

// ---------------------------------------------------------------------------
// TPMS-D: waves solved from its equation.

// TPMS-D upper resolution tolerance (mm^-2)
static constexpr double TpmsDPatternTolerance = 0.1;

static double scaled_floor(double x,double scale){
	return std::floor(x/scale)*scale;
}

static Polylines make_tpms_d_waves(double gridZ, double density_adjusted, double line_spacing, double width, double height)
{
    const double scaleFactor = scale_(line_spacing) / density_adjusted;

    // tolerance in scaled units. clamp the maximum tolerance as there's
    // no processing-speed benefit to do so beyond a certain point
    const double tolerance = std::min(line_spacing / 2, TpmsDPatternTolerance) / unscale<double>(scaleFactor);

    //scale factor for 5% : 8 712 388
    // 1z = 10^-6 mm ?
    const double z = gridZ / scaleFactor;
    Polylines result;

	//sin(x)*sin(y)*sin(z)-cos(x)*cos(y)*cos(z)=0
	//2*sin(x)*sin(y)*sin(z)-2*cos(x)*cos(y)*cos(z)=0
	//(cos(x-y)-cos(x+y))*sin(z)-(cos(x-y)+cos(x+y))*cos(z)=0
	//(sin(z)-cos(z))*cos(x-y)-(sin(z)+cos(z))*cos(x+y)=0
	double a=sin(z)-cos(z);
	double b=sin(z)+cos(z);
	//a*cos(x-y)-b*cos(x+y)=0
	//u=x-y, v=x+y
	double minU=0-height;
	double maxU=width-0;
	double minV=0+0;
	double maxV=width+height;
	//a*cos(u)-b*cos(v)=0
	//if abs(b)<abs(a), then u=acos(b/a*cos(v)) is a continuous line
	//otherwise we swap u and v
	const bool swapUV=(std::abs(a)>std::abs(b));
	if(swapUV) {
		std::swap(a,b);
		std::swap(minU,minV);
		std::swap(maxU,maxV);
	}
	std::vector<std::pair<double,double>> wave;
	{//fill one wave
		const auto v=[&](double u){return acos(a/b*cos(u));};
		const int initialSegments=16;
		for(int c=0;c<=initialSegments;++c){
			const double u=minU+2*M_PI*c/initialSegments;
			wave.emplace_back(u,v(u));
		}
		{//refine
			int current=0;
			while(current+1<int(wave.size())){
				const double u1=wave[current].first;
				const double u2=wave[current+1].first;
				const double middleU=(u1+u2)/2;
				const double v1=wave[current].second;
				const double v2=wave[current+1].second;
				const double middleV=v((u1+u2)/2);
				if(std::abs(middleV-(v1+v2)/2)>tolerance)
					wave.emplace(wave.begin()+current+1,middleU,middleV);
				else
					++current;
			}
		}
		for(int c=1;c<int(wave.size()) && wave.back().first<maxU;++c)//we start from 1 because the 0-th one is already duplicated as the last one in a period
			wave.emplace_back(wave[c].first+2*M_PI,wave[c].second);
	}
	for(double vShift=scaled_floor(minV,2*M_PI);vShift<maxV+2*M_PI;vShift+=2*M_PI) {
		for(bool forwardRoot:{false,true}) {
			result.emplace_back();
			for(const auto &pair:wave) {
				const double u=pair.first;
				double v=pair.second;
				v=(forwardRoot?v:-v)+vShift;
				const double x=(u+v)/2;
				const double y=(v-u)/2*(swapUV?-1:1);
				result.back().points.emplace_back(x*scaleFactor,y*scaleFactor);
			}
		}
	}
	//todo: select the step better
    return result;
}

// The waves of a layer of the regular cells, from the corner of the region aligned to the grid of the pattern.
static Polylines tpms_d_waves(const TpmsLayer &layer)
{
    BoundingBox bb = layer.bbox;
    // Distance between the waves in scaled coordinates.
    const coord_t distance = coord_t(scale_(layer.spacing) / layer.density_adjusted);
    // align bounding box to a multiple of our grid module
    bb.merge(align_to_grid(bb.min, Point(2*M_PI*distance, 2*M_PI*distance)));
    Polylines polylines = make_tpms_d_waves(scale_(layer.z) / layer.tpms.scale.z(), layer.density_adjusted, layer.spacing,
                                            ceil(bb.size()(0) / distance) + 1., ceil(bb.size()(1) / distance) + 1.);
    // shift the polyline to the grid origin
    for (Polyline &pl : polylines)
        pl.translate(bb.min);
    return polylines;
}

// ---------------------------------------------------------------------------
// TPMS-P: rings of its layers.

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

// The rings of a layer of the regular cells.
static Polylines tpms_p_rings(const TpmsLayer &layer)
{
    const double       frequency = layer.frequency;
    const BoundingBox &bbox      = layer.margin_bbox;
    const double       cell      = small_cell(frequency);

    // Every layer of the regular cells is a grid of equal rings cos(x) + cos(y) = level, made at once: around (0, 0)
    // for a positive level, around (PI, PI) for a negative one.
    const double level = -(std::cos(frequency * layer.z / layer.tpms.scale.z() / SchwarzPStretch) + SchwarzPLevel);
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

// ---------------------------------------------------------------------------
// The patterns and the fill they share.

struct FillTpms::Surface
{
    InfillPattern type;
    float (*equation)(float x, float y, float z);
    // Period at full density in line spacings, calibrated so that a density prints that share of the volume. The period
    // scales with the multiline count over the density, which is capped at max_density.
    double period_factor;
    float  max_density;
    // Cells smaller than Fischer-Koch's, traced on a grid of up to a 32nd of their period instead of TpmsGridCell.
    bool small_cells;
    // With the anchors off, chain the lines instead of connecting them: the many islands of the other patterns would
    // chain slowly.
    bool chain_unanchored;
    // The lines of the layers of the regular cells, for the patterns with a parametric form.
    Polylines (*parametric)(const TpmsLayer &layer);
};

// Each period factor but Gyroid's makes a cube of infill alone weigh its density; see docs/HLSD/tpms-infill.md.
static const FillTpms::Surface surfaces[] = {
    {ipGyroid, gyroid, FillTpms::GyroidPeriodFactor, 1.f, false, true, gyroid_waves},
    {ipTpmsD, schwarz_d, 3.019, 1.f, false, true, tpms_d_waves},
    {ipTpmsFK, fischer_koch, 4.225, 0.9f, true, false, nullptr},
    {ipTpmsP, schwarz_p, 1.840, 0.9f, true, false, tpms_p_rings},
    {ipTpmsIWP, schoen_iwp, 2.731, 0.9f, true, false, nullptr},
    {ipTpmsNeovius, neovius, 2.660, 0.9f, true, false, nullptr},
    {ipTpmsFRD, schoen_frd, 3.680, 0.9f, true, false, nullptr},
    {ipTpmsLidinoid, lidinoid, 4.828, 0.9f, true, false, nullptr},
    {ipTpmsSplitP, split_p, 3.965, 0.9f, true, false, nullptr},
};

FillTpms::FillTpms(InfillPattern pattern)
    : m_surface(std::find_if(std::begin(surfaces), std::end(surfaces), [pattern](const Surface &s) { return s.type == pattern; }))
{
    assert(m_surface != std::end(surfaces));
}

void FillTpms::_fill_surface_single(const FillParams&              params,
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

    const Surface     &surface = *m_surface;
    const TpmsEquation tpms(surface.equation, Vec3d(params.sparse_infill_scale.data()), params.sparse_infill_rotation_x);
    auto infill_angle = float(this->angle + (CorrectionAngle * 2 * M_PI) / 360.);
    if (std::abs(infill_angle) >= EPSILON)
        expolygon.rotate(-infill_angle);

    // Radians per spacing of single lines at full density.
    const double density_adjust = 2. * PI / surface.period_factor;
    // Radians per mm of the regular pattern at a density.
    auto frequency = [&params, &surface, density_adjust, this](double density) {
        return std::min(density, double(surface.max_density)) * density_adjust / (params.multiline * this->spacing);
    };
    const BoundingBox bbox = expolygon.contour.bounding_box();
    // Enlarge the bounding box by the multi-line width to avoid artifacts at the edges.
    BoundingBox margin_bbox = bbox;
    margin_bbox.offset(scale_((params.multiline + 1) * this->spacing));
    Polylines polylines;
    if (params.adaptive_infill_mode != AdaptiveInfillMode::Disabled && this->adaptive_infill_field != nullptr) {
        polylines = make_adaptive_tpms({tpms, frequency(params.density), frequency(params.adaptive_infill_interior_density), params.adaptive_infill_gradient},
                                       *this->adaptive_infill_field, margin_bbox, this->z, params.layer_height, this->spacing, infill_angle);
    } else {
        const double density_adjusted = std::max(0., double(std::min(params.density, surface.max_density)) * density_adjust / params.multiline);
        const double layer_frequency  = frequency(params.density);
        // Fill.cpp sets gyroid_optimized for Gyroid only.
        if (surface.parametric != nullptr && tpms.regular_layers() && !params.gyroid_optimized) {
            polylines = surface.parametric({tpms, this->z, this->spacing, params.multiline, density_adjusted, layer_frequency, bbox, margin_bbox});
        } else {
            const double cell = surface.small_cells ? small_cell(layer_frequency) : TpmsGridCell;
            double       z_frequency = layer_frequency;
            if (params.gyroid_optimized) {
                const double lh = (params.layer_height > 0.) ? double(params.layer_height) : double(this->spacing);
                z_frequency *= compute_omega_factor(density_adjusted, this->spacing * params.multiline, lh);
            }
            polylines = make_tpms_polylines(tpms, Vec3d(layer_frequency, layer_frequency, z_frequency), tpms_sampling_bbox(margin_bbox, cell),
                                            this->z, cell);
        }
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
        if (surface.chain_unanchored)
            chain_or_connect_infill(std::move(polylines), expolygon, polylines_out, this->spacing, params);
        else
            connect_infill(std::move(polylines), expolygon, polylines_out, this->spacing, params);

        // new paths must be rotated back
        if (std::abs(infill_angle) >= EPSILON) {
            for (auto it = polylines_out.begin() + polylines_out_first_idx; it != polylines_out.end(); ++it)
                it->rotate(infill_angle);
        }
    }
}

} // namespace Slic3r
