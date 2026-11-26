#pragma once

#include <algorithm>

namespace Slic3r {

enum class MoonrakerFilamentSource { None, Lanes, HappyHare, CrealityCfs, EmptyLanes };

// Select the first usable filament source, preserving empty lane topology.
// Each reader fills trays and the highest slot index; failed readers own no result.
template<class Trays, class LaneReader, class HappyHareReader, class CfsReader>
MoonrakerFilamentSource select_moonraker_filament_data(
    Trays& trays, int& max_lane_index, LaneReader read_lanes, HappyHareReader read_happy_hare, CfsReader read_cfs)
{
    trays.clear();
    max_lane_index                = 0;
    int empty_lane_data_max_index = -1;
    if (read_lanes(trays, max_lane_index)) {
        if (std::any_of(trays.begin(), trays.end(), [](const auto& tray) { return tray.has_filament; }))
            return MoonrakerFilamentSource::Lanes;
        empty_lane_data_max_index = max_lane_index;
    }

    trays.clear();
    max_lane_index = 0;
    if (read_happy_hare(trays, max_lane_index)) {
        max_lane_index = std::max(max_lane_index, empty_lane_data_max_index);
        return MoonrakerFilamentSource::HappyHare;
    }

    trays.clear();
    max_lane_index = 0;
    if (read_cfs(trays, max_lane_index))
        return MoonrakerFilamentSource::CrealityCfs;

    trays.clear();
    max_lane_index = empty_lane_data_max_index;
    if (empty_lane_data_max_index >= 0)
        return MoonrakerFilamentSource::EmptyLanes;
    max_lane_index = 0;
    return MoonrakerFilamentSource::None;
}

} // namespace Slic3r
