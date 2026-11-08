#include "LayerTimeSmoothing.hpp"

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cstdio>
#include <utility>

#include <boost/log/trivial.hpp>

namespace Slic3r {

// Layer time changes below this value are ignored, seconds.
static constexpr float LTS_EPSILON = 1e-3f;

LayerTimeSmoother::Params LayerTimeSmoother::params_from_config(const PrintConfig &config)
{
    Params params;
    params.max_variation      = std::clamp(float(config.layer_time_max_variation.value) * 0.01f, 0.f, 1.f);
    params.max_layer_slowdown = std::max(0.f, float(config.layer_time_smoothing_max_slowdown.value) * 0.01f);
    params.max_total_increase = std::max(0.f, float(config.layer_time_smoothing_max_time_increase.value) * 0.01f);
    params.scope              = config.layer_time_smoothing_scope.value;
    return params;
}

std::vector<float> LayerTimeSmoother::solve_target_times(const std::vector<float> &times, const std::vector<float> &caps, float max_variation, size_t first_layer)
{
    assert(caps.size() == times.size());
    std::vector<float> t = times;
    const size_t n = t.size();
    if (n < 2 || first_layer + 1 >= n)
        return t;
    // A neighbour may be at most this much shorter than a layer.
    const float ratio = 1.f - std::clamp(max_variation, 0.f, 1.f);
    if (ratio <= 0.f)
        return t;

    auto raise = [&t, &caps](size_t i, float lower_bound) -> bool {
        float target = std::min(lower_bound, caps[i]);
        if (target > t[i] + LTS_EPSILON) {
            t[i] = target;
            return true;
        }
        return false;
    };

    // Raising a layer may violate the constraint with its other neighbour, therefore sweep forward and backward
    // until nothing changes. Layers are only ever raised and each is bounded by its cap, so this converges,
    // normally within two sweeps. A layer that hits its cap does not propagate the constraint any further.
    for (size_t sweep = 0; sweep < 64; ++ sweep) {
        bool changed = false;
        for (size_t i = first_layer + 1; i < n; ++ i)
            changed |= raise(i, t[i - 1] * ratio);
        for (size_t i = n - 1; i > first_layer; -- i)
            changed |= raise(i - 1, t[i] * ratio);
        if (! changed)
            break;
    }
    return t;
}

std::vector<float> LayerTimeSmoother::solve_target_times(const std::vector<float> &times, const std::vector<float> &caps, float max_variation,
                                                         float max_total_increase, size_t first_layer, float &effective_max_variation)
{
    effective_max_variation = max_variation;
    std::vector<float> t = solve_target_times(times, caps, max_variation, first_layer);
    const size_t n = times.size();

    double base = 0.;
    for (size_t i = first_layer; i < n; ++ i)
        base += times[i];
    auto increase = [&times, first_layer, n](const std::vector<float> &t) {
        double d = 0.;
        for (size_t i = first_layer; i < n; ++ i)
            d += double(t[i]) - double(times[i]);
        return d;
    };
    const double budget = double(max_total_increase) * base + LTS_EPSILON;
    if (increase(t) <= budget)
        return t;

    // The total increase shrinks monotonically with a growing variation limit (a limit of 100% changes nothing),
    // therefore the smallest limit that fits into the budget can be found by bisection.
    float lo = max_variation;
    float hi = 1.f;
    std::vector<float> best = times;
    for (size_t iter = 0; iter < 24 && hi - lo > 1e-3f; ++ iter) {
        float mid = 0.5f * (lo + hi);
        std::vector<float> candidate = solve_target_times(times, caps, mid, first_layer);
        if (increase(candidate) <= budget) {
            hi   = mid;
            best = std::move(candidate);
        } else
            lo = mid;
    }
    effective_max_variation = hi;
    return best;
}

std::vector<float> LayerTimeSmoother::solve(const std::vector<float> &times, const std::vector<float> &max_times, size_t first_layer)
{
    const size_t n = times.size();
    assert(max_times.size() == n);
    m_stats.assign(n, LayerStats{});
    m_effective_max_variation = m_params.max_variation;
    if (n == 0)
        return {};

    // Per layer cap: the user limit on the slow down of a single layer, and the minimum print speed of the filaments.
    std::vector<float> caps(n);
    for (size_t i = 0; i < n; ++ i) {
        m_stats[i].time_before = times[i];
        float cap = times[i];
        if (i >= first_layer)
            cap = std::min(times[i] * (1.f + m_params.max_layer_slowdown), std::max(times[i], max_times[i]));
        caps[i]             = cap;
        m_stats[i].time_cap = cap;
        m_stats[i].time_after = times[i];
    }

    std::vector<float> targets = solve_target_times(times, caps, m_params.max_variation, m_params.max_total_increase, first_layer, m_effective_max_variation);
    BOOST_LOG_TRIVIAL(info) << "Layer time smoothing: max variation " << m_effective_max_variation * 100.f << "%";
    return targets;
}

void LayerTimeSmoother::set_time_after(size_t idx, float time_after)
{
    if (idx >= m_stats.size())
        return;
    LayerStats &s = m_stats[idx];
    s.time_after  = time_after;
    s.smoothed    = time_after > s.time_before + LTS_EPSILON;
}

std::string LayerTimeSmoother::layer_comment(size_t idx) const
{
    if (idx >= m_stats.size())
        return std::string();
    const LayerStats &s      = m_stats[idx];
    float             factor = s.time_after > 0.f ? s.time_before / s.time_after : 1.f;
    char              buf[128];
    snprintf(buf, sizeof(buf), "; LAYER_TIME_SMOOTHING factor=%.3f t_raw=%.2f t_out=%.2f\n", factor, s.time_before, s.time_after);
    return std::string(buf);
}

} // namespace Slic3r
