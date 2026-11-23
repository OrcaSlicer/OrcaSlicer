#pragma once

#include <cstddef>
#include <vector>

namespace Slic3r {
namespace GUI {

struct TickGroup
{
    size_t first{0};
    size_t count{0};
    float  pos{0.f};
};

std::vector<TickGroup> group_ticks_by_position(const std::vector<float> &positions, float min_distance);

} // namespace GUI
} // namespace Slic3r
