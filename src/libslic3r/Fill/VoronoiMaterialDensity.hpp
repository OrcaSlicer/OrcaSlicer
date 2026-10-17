#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace Slic3r::Voronoi {

// Zero is empty; 1..253 have sixteen steps per doubling, ending at 100%.
// 254 and 255 are reserved and must never enter interpolation.
inline uint8_t encode_material_density(double density)
{
    if (!std::isfinite(density) || density < 0.) throw std::invalid_argument("Invalid material density");
    if (density == 0.) return 0;
    return uint8_t(std::clamp(std::lround(253. + 16. * std::log2(std::min(1., density))), 1L, 253L));
}

inline double decode_material_density(uint8_t code)
{
    if (code >= 254) throw std::invalid_argument("Reserved material density code");
    return code == 0 ? 0. : std::exp2((double(code) - 253.) / 16.);
}

inline double modifier_blend(double inside_distance, double width)
{
    if (inside_distance < 0.) return 0.;
    if (width == 0.) return 1.;
    const double t = std::clamp(inside_distance / width, 0., 1.);
    return t * t * (3. - 2. * t);
}

} // namespace Slic3r::Voronoi
