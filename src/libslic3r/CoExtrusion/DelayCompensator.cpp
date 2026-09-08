#include "DelayCompensator.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace Slic3r::CoExtrusion {

std::vector<CAxisPathIntent> DelayCompensator::compensate(
    const std::vector<CAxisPathIntent> &intents,
    const std::vector<double> &durations_s,
    const DelayCompensationConfig &config)
{
    DelayCompensationPath path;
    path.intents = intents;
    path.durations_s = durations_s;
    path.effective_mm3_per_mm = config.effective_mm3_per_mm;

    std::vector<DelayCompensationPath> compensated = compensate_sequence({ std::move(path) }, config);
    return compensated.empty() ? std::vector<CAxisPathIntent>{} : std::move(compensated.front().intents);
}

std::vector<DelayCompensationPath> DelayCompensator::compensate_sequence(
    const std::vector<DelayCompensationPath> &paths,
    const DelayCompensationConfig &config)
{
    if (paths.empty() || config.model == DelayModel::Disabled)
        return paths;

    struct FlatIntent {
        size_t path_index { 0 };
        size_t intent_index { 0 };
        double sequence_start_mm { 0.0 };
    };

    std::vector<FlatIntent> flattened;
    size_t intent_count = 0;
    for (const DelayCompensationPath &path : paths)
        intent_count += path.intents.size();
    flattened.reserve(intent_count);

    double sequence_distance_mm = 0.0;
    for (size_t path_index = 0; path_index < paths.size(); ++path_index) {
        const DelayCompensationPath &path = paths[path_index];
        for (size_t intent_index = 0; intent_index < path.intents.size(); ++intent_index) {
            const CAxisPathIntent &intent = path.intents[intent_index];
            flattened.push_back({ path_index, intent_index, sequence_distance_mm });
            sequence_distance_mm += std::max(0.0, intent.path_end_mm - intent.path_start_mm);
        }
    }

    if (flattened.empty())
        return paths;

    std::vector<double> weights(flattened.size(), 0.0);
    double lookahead = 0.0;
    if (config.model == DelayModel::FixedTime) {
        if (!(config.response_delay_s > 0.0) || !std::isfinite(config.response_delay_s))
            return paths;
        lookahead = config.response_delay_s;
        for (size_t index = 0; index < flattened.size(); ++index) {
            const FlatIntent &flat = flattened[index];
            const std::vector<double> &durations = paths[flat.path_index].durations_s;
            if (flat.intent_index < durations.size() && durations[flat.intent_index] > 0.0 &&
                std::isfinite(durations[flat.intent_index]))
                weights[index] = durations[flat.intent_index];
        }
    } else {
        if (!(config.transport_volume_mm3 > 0.0) || !std::isfinite(config.transport_volume_mm3))
            return paths;
        lookahead = config.transport_volume_mm3;
        for (size_t index = 0; index < flattened.size(); ++index) {
            const FlatIntent &flat = flattened[index];
            const DelayCompensationPath &path = paths[flat.path_index];
            if (!(path.effective_mm3_per_mm > 0.0) || !std::isfinite(path.effective_mm3_per_mm))
                continue;
            const CAxisPathIntent &intent = path.intents[flat.intent_index];
            weights[index] = std::max(0.0, intent.path_end_mm - intent.path_start_mm) *
                             path.effective_mm3_per_mm;
        }
    }

    std::vector<double> cumulative(flattened.size() + 1, 0.0);
    for (size_t index = 0; index < flattened.size(); ++index)
        cumulative[index + 1] = cumulative[index] + weights[index];
    if (!(cumulative.back() > 0.0))
        return paths;

    std::vector<DelayCompensationPath> compensated = paths;
    for (size_t command_index = 0; command_index < flattened.size(); ++command_index) {
        const double target_metric = cumulative[command_index] + lookahead;
        const auto upper = std::upper_bound(cumulative.begin(), cumulative.end(), target_metric);
        const size_t source_index = std::min(
            flattened.size() - 1,
            upper == cumulative.begin() ? size_t(0) : size_t(std::distance(cumulative.begin(), upper) - 1));

        const FlatIntent &command = flattened[command_index];
        const FlatIntent &source_ref = flattened[source_index];
        CAxisPathIntent &target = compensated[command.path_index].intents[command.intent_index];
        const CAxisPathIntent &source = paths[source_ref.path_index].intents[source_ref.intent_index];
        target.status          = source.status;
        target.slice_source    = source.slice_source;
        target.surface         = source.surface;
        target.direction       = source.direction;
        target.control_source_path_index = source_ref.path_index;
        target.control_source_path_mm = source.path_start_mm;
        target.compensation_advance_mm = std::max(
            0.0, source_ref.sequence_start_mm - command.sequence_start_mm);
    }
    return compensated;
}

} // namespace Slic3r::CoExtrusion
