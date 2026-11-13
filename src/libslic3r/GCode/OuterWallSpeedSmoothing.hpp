#pragma once

#include <vector>

namespace Slic3r {

// Smooth the outer wall speed of one chain of connected outer walls along Z.
// speeds[i] is the fastest outer wall feedrate of the chain on layer i (mm/s), a value <= 0 marks a layer where the
// chain has no outer wall. The speeds are only ever reduced: they are repeatedly filtered with a Gaussian window
// until the speeds of adjacent layers differ by at most 5 mm/s (or the iteration limit is reached).
std::vector<float> smooth_outer_wall_speeds(const std::vector<float> &speeds);

} // namespace Slic3r
