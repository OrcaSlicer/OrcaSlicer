#include "PreviewMarks.hpp"

#include <cstddef>
#include <cstdlib>
#include <vector>

namespace Slic3r {
namespace GUI {

std::vector<TickGroup> group_ticks_by_position(const std::vector<float> &positions, float min_distance)
{
    std::vector<TickGroup> groups;
    size_t i = 0;
    while (i < positions.size()) {
        size_t j = i + 1;
        while (j < positions.size() && std::abs(positions[j] - positions[j - 1]) < min_distance)
            ++j;
        groups.push_back({i, j - i, 0.5f * (positions[i] + positions[j - 1])});
        i = j;
    }
    return groups;
}

} // namespace GUI
} // namespace Slic3r
