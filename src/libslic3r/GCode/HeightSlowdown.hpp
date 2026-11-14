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
};

// The ramp of an object runs from `ramp_first_z` (the first layer that is not kept as it is) to
// `ramp_last_z` (its last layer), both print heights of model layers (support and raft layers are not
// part of it). Below the ramp the normal values are kept. If the ramp has a single layer, it gets
// `end_factor`.
inline HeightSlowdownFactors height_slowdown_factors(double print_z, double ramp_first_z, double ramp_last_z,
                                                     double start_factor, double end_factor)
{
    HeightSlowdownFactors out;
    if (print_z < ramp_first_z)
        return out;

    const bool   ramp     = ramp_last_z > ramp_first_z;
    const double progress = ramp ? std::clamp((print_z - ramp_first_z) / (ramp_last_z - ramp_first_z), 0., 1.) : 1.;
    out.speed  = ramp ? start_factor + (end_factor - start_factor) * progress : end_factor;
    out.motion = 1. + (std::min(end_factor, 1.) - 1.) * progress;
    return out;
}

} // namespace Slic3r

#endif // slic3r_GCode_HeightSlowdown_hpp_
