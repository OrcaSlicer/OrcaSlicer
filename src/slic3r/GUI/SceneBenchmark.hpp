#pragma once

#include <vector>

namespace Slic3r {
namespace GUI {

struct FrameTimeStats
{
    double fps{ 0.0 };
    double average_ms{ 0.0 };
    double median_ms{ 0.0 };
    double p95_ms{ 0.0 };
    double p99_ms{ 0.0 };
    double max_ms{ 0.0 };
};

// Nearest-rank percentiles, so every value is a measured frame time.
FrameTimeStats frame_time_stats(std::vector<double> frame_ms);

// Loads the OrcaSliced Combo into a new project and turns the camera around it in Prepare, then in
// Preview once sliced, then reports the frame times and render timings of both.
void run_scene_benchmark();

} // namespace GUI
} // namespace Slic3r
