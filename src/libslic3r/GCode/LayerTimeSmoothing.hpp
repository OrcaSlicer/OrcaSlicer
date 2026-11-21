#ifndef slic3r_LayerTimeSmoothing_hpp_
#define slic3r_LayerTimeSmoothing_hpp_

#include "libslic3r/PrintConfig.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

// Limits how much the estimated print time may change from one layer to the next.
//
// A step change of the layer time is a step change of the thermal history of the part: it changes how far
// the previous layer has cooled and contracted at the moment the next layer is welded onto it, and it shows
// up on vertical walls as banding, sunken or bulging regions, especially with glossy and high shrinkage
// filaments. The per layer print times are treated as a curve over Z and the solver enforces
//
//     t[i] >= t[i-1] * (1 - v)    and    t[i-1] >= t[i] * (1 - v)
//
// where v is the maximum allowed relative variation. Layers are only ever slowed down, and two caps limit the cost:
// a maximum slow down per layer (and the minimum print speed of the filaments, which the caller folds into the caps)
// and a maximum increase of the total print time.
//
// This class holds the pure part. CoolingBuffer applies the resulting layer times to the parsed G-code lines.
class LayerTimeSmoother
{
public:
    struct Params
    {
        // Maximum allowed relative change of the layer time between adjacent layers, 0 .. 1.
        float max_variation = 0.25f;
        // Maximum relative increase of the time of a single layer (2 = the layer may take up to three times as long).
        float max_layer_slowdown = 2.f;
        // Maximum relative increase of the total time of the smoothed layers.
        float max_total_increase = 0.2f;
        // Extrusions which may be slowed down.
        LayerTimeSmoothingScope scope = ltsAll;
    };

    struct LayerStats
    {
        // Layer time before smoothing (after the cooling slow down), seconds.
        float time_before = 0.f;
        // Layer time after smoothing, seconds.
        float time_after = 0.f;
        // Highest layer time reachable within the caps, seconds.
        float time_cap = 0.f;
        // Whether the layer was slowed down.
        bool smoothed = false;
    };

    explicit LayerTimeSmoother(const Params &params) : m_params(params) {}

    static Params params_from_config(const PrintConfig &config);

    const Params &params() const { return m_params; }

    // Find the target time of each layer. times[i] is the current time of the i-th layer in seconds, max_times[i] the
    // highest time the layer can reach by slowing down (FLT_MAX if not limited by the minimum print speed).
    // Layers before first_layer are left untouched and do not constrain their neighbours.
    // Records the statistics for layer_comment().
    std::vector<float> solve(const std::vector<float> &times, const std::vector<float> &max_times, size_t first_layer = 1);

    // Maximum variation that was finally enforced. Larger than Params::max_variation if the limit on the
    // total print time increase had to be applied.
    float effective_max_variation() const { return m_effective_max_variation; }
    const std::vector<LayerStats> &stats() const { return m_stats; }

    // Record the time a layer ended up with after its lines were stretched.
    void set_time_after(size_t idx, float time_after);

    // Diagnostic G-code comment for a layer processed by solve(), terminated by a newline.
    std::string layer_comment(size_t idx) const;

    // The pure parts of the algorithm, exposed for unit tests.
    // Find the smallest layer times t >= times with t[i] <= caps[i], which satisfy the variation limit wherever the caps allow it.
    static std::vector<float> solve_target_times(const std::vector<float> &times, const std::vector<float> &caps, float max_variation, size_t first_layer);
    // Same as above, with the variation limit relaxed until the total time increase fits into max_total_increase.
    // effective_max_variation receives the variation limit that was finally used.
    static std::vector<float> solve_target_times(const std::vector<float> &times, const std::vector<float> &caps, float max_variation,
                                                 float max_total_increase, size_t first_layer, float &effective_max_variation);

private:
    Params                  m_params;
    std::vector<LayerStats> m_stats;
    float                   m_effective_max_variation = 0.f;
};

} // namespace Slic3r

#endif // slic3r_LayerTimeSmoothing_hpp_
