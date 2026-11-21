#include "OuterWallSpeedSmoothing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "libslic3r/libslic3r.h"

namespace Slic3r {

namespace {

constexpr int   gaussian_window_size   = 11;
constexpr double gaussian_sigma         = 2.;
// The filtering stops once the speeds of all adjacent layers differ by at most this much [mm/s].
constexpr float gaussian_stop_threshold = 5.f;
constexpr int   max_filter_steps        = 1000;

std::vector<double> gaussian_kernel()
{
    std::vector<double> kernel;
    const int half = gaussian_window_size / 2;
    for (int x = -half; x <= half; ++ x)
        kernel.emplace_back(std::exp(-double(x) * x / (2. * gaussian_sigma * gaussian_sigma)) / (gaussian_sigma * std::sqrt(2. * PI)));
    return kernel;
}

bool needs_filtering(const std::vector<float> &speeds)
{
    for (size_t i = 0; i + 1 < speeds.size(); ++ i)
        if (speeds[i] > 0.f && speeds[i + 1] > 0.f && std::abs(speeds[i + 1] - speeds[i]) > gaussian_stop_threshold)
            return true;
    return false;
}

} // namespace

std::vector<float> smooth_outer_wall_speeds(const std::vector<float> &speeds)
{
    std::vector<float>        out = speeds;
    const std::vector<double> kernel = gaussian_kernel();
    double                    kernel_sum = 0.;
    for (double k : kernel)
        kernel_sum += k;
    const int half = int(kernel.size() / 2);
    const int last = int(out.size()) - 1;

    for (int step = 0; step < max_filter_steps && needs_filtering(out); ++ step) {
        // The filtered values are used by the following layers of the same pass.
        for (int layer = 0; layer <= last; ++ layer) {
            if (out[layer] <= 0.f)
                continue;
            double sum = 0.;
            for (size_t k = 0; k < kernel.size(); ++ k) {
                // Clamp the window to the chain, a layer without an outer wall contributes the speed of the current layer.
                const int pos = std::min(std::max(layer - half + int(k), 0), last);
                const double value = out[pos] > 0.f ? out[pos] : out[layer];
                sum += kernel[k] * value;
            }
            const double filtered = sum / kernel_sum;
            if (filtered < out[layer])
                out[layer] = float(filtered);
        }
    }
    return out;
}

} // namespace Slic3r
