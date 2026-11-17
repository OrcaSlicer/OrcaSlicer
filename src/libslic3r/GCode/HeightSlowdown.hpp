#ifndef slic3r_GCode_HeightSlowdown_hpp_
#define slic3r_GCode_HeightSlowdown_hpp_

#include <algorithm>

namespace Slic3r {

// Speed factors of the height slowdown at one layer of an object.
struct HeightSlowdownFactors
{
    // Applies to the printing speed. Can be above 1 when the ramp starts above 100%.
    double speed{1.};
    // Applies to acceleration. Never above 1.
    double motion{1.};
    // Applies to the acceleration of travel moves. Never above 1.
    double travel_acceleration{1.};
    // The slowdown does not take the printing speed or the acceleration below these. A normal value that is already
    // lower is kept. Set by the caller from the config, 0 for no limit.
    double min_speed{0.};
    double min_acceleration{0.};
};

// A scaled value that is not taken below `min_value`, unless the `normal` value was below it already.
inline double height_slowdown_limited(double scaled, double normal, double min_value)
{
    return std::max(scaled, std::min(normal, min_value));
}

// The ramp of an object runs from `ramp_first_z` (the first layer that is not kept as it is, a print height of
// a model layer) to `ramp_last_z` (the end height, which can be above the object). Above it the end values are kept.
// Below the ramp the normal values are kept. If the ramp has no height, it gets `end_factor`.
inline HeightSlowdownFactors height_slowdown_factors(double print_z, double ramp_first_z, double ramp_last_z,
                                                     double start_factor, double end_factor, double end_travel_acceleration_factor = 1.)
{
    HeightSlowdownFactors out;
    if (print_z < ramp_first_z)
        return out;

    const bool   ramp     = ramp_last_z > ramp_first_z;
    const double progress = ramp ? std::clamp((print_z - ramp_first_z) / (ramp_last_z - ramp_first_z), 0., 1.) : 1.;
    out.speed  = ramp ? start_factor + (end_factor - start_factor) * progress : end_factor;
    out.motion = 1. + (std::min(end_factor, 1.) - 1.) * progress;
    out.travel_acceleration = 1. + (std::min(end_travel_acceleration_factor, 1.) - 1.) * progress;
    return out;
}

} // namespace Slic3r

#endif // slic3r_GCode_HeightSlowdown_hpp_
