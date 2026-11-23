#pragma once

#include <vector>

#include "LegendModel.hpp"
#include "CanvasUi/Types.hpp"

namespace Slic3r {
namespace GUI {

// The filament every plate of the project takes, summed per filament, with the time and cost of all of them.
struct AllPlatesStats
{
    std::vector<LegendFilament> rows;
    float                       time{0.f};
    double                      cost{0.0};
    bool                        imperial{false};
};

void render_all_plates_stats(const AllPlatesStats& stats, const CanvasUi::Point& center);

} // namespace GUI
} // namespace Slic3r
