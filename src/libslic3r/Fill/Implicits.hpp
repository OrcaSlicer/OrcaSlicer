#ifndef slic3r_Implicits_hpp_
#define slic3r_Implicits_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {

// Parameters for the gyroid implicit scalar field.
//
// Field:
//   f(x,y,z) = sin(a)cos(b) + sin(b)cos(c) + sin(c)cos(a)
// where
//   a = (2*pi / period) * x
//   b = (2*pi / period) * y
//   c = omega * (2*pi / period) * z
//
// f == 0 is the gyroid TPMS surface. omega >= 1 anisotropically tightens the
// pattern along Z, shortening the effective vertical strand length to improve
// column-buckling resistance. See compute_omega_factor below for the derivation.
struct GyroidParams
{
    float period; // XY wavelength in object-space units the evaluator receives
    float omega;  // Z anisotropy factor (>= 1)
};

inline float gyroid(const Vec3f &p, const GyroidParams &gp)
{
    const float baseline = float(2.0 * PI) / std::max(gp.period, 1e-3f);
    const float a = baseline * p.x();
    const float b = baseline * p.y();
    const float c = gp.omega * baseline * p.z();
    return std::sin(a) * std::cos(b) + std::sin(b) * std::cos(c) + std::sin(c) * std::cos(a);
}

// Euler-Bernoulli-derived Z anisotropy factor. Low density -> long slender
// vertical strands -> larger omega shortens them. Clamped to [1.0, 2.0].
// Kept in double precision to match the existing FillGyroid computation byte-for-byte.
inline double compute_omega_factor(double density_adjusted, double line_spacing, double layer_height)
{
    double lh_ratio   = (line_spacing > 0.) ? layer_height / line_spacing : 0.5;
    double correction = 1.0 / std::sqrt(1.0 + lh_ratio);
    double raw        = std::sqrt(1.0 / std::max(density_adjusted, 0.1)) * correction;
    return std::clamp(raw, 1.0, 2.0);
}

// Schwarz Diamond (TPMS-D):
//   f = sin(a)sin(b)sin(c) - cos(a)cos(b)cos(c),  a/b/c = (2*pi/period) * x/y/z
// FillTpmsD's closed-form wave generator solves exactly this zero set
// (see the derivation comment in FillTpmsD.cpp make_waves); the field's
// period in mm is 2*pi * spacing / density_adjusted, isotropic.
inline float schwarz_d(const Vec3f &p, float period)
{
    const float k = float(2.0 * PI) / std::max(period, 1e-3f);
    const float a = k * p.x(), b = k * p.y(), c = k * p.z();
    return std::sin(a) * std::sin(b) * std::sin(c) - std::cos(a) * std::cos(b) * std::cos(c);
}

// Fischer-Koch S:
//   f = cos(2a)sin(b)cos(c) + cos(2b)sin(c)cos(a) + cos(2c)sin(a)cos(b)
// Same float expression as FillTpmsFK's marching-squares ScalarField so the
// two stay in lockstep. Period in mm = 4.18 * spacing * multiline /
// min(0.9, density) (FillTpmsFK.cpp vari_T).
inline float fischer_koch_s_freq(const Vec3f &p, float freq)
{
    const float fx = freq * p.x(), fy = freq * p.y(), fz = freq * p.z();
    return cosf(2 * fx) * sinf(fy) * cosf(fz) + cosf(2 * fy) * sinf(fz) * cosf(fx) + cosf(2 * fz) * sinf(fx) * cosf(fy);
}

inline float fischer_koch_s(const Vec3f &p, float period)
{
    return fischer_koch_s_freq(p, float(2.0 * PI) / std::max(period, 1e-3f));
}

} // namespace Slic3r

#endif // slic3r_Implicits_hpp_
