#pragma once

#include "CAxisIntentBuilder.hpp"

#include <vector>

namespace Slic3r::CoExtrusion {

struct DelayCompensationConfig {
    DelayModel model { DelayModel::Disabled };
    double response_delay_s { 0.0 };
    double transport_volume_mm3 { 0.0 };
    double effective_mm3_per_mm { 0.0 };
};

struct DelayCompensationPath {
    std::vector<CAxisPathIntent> intents;
    std::vector<double> durations_s;
    double effective_mm3_per_mm { 0.0 };
};

// Advances orientation requirements along finalized print order. The result
// keeps every command's geometry and painted target color, but may source its
// mechanical control intent from a future path.
class DelayCompensator
{
public:
    // Compatibility wrapper for callers that only have one finalized path.
    static std::vector<CAxisPathIntent> compensate(
        const std::vector<CAxisPathIntent> &intents,
        const std::vector<double> &durations_s,
        const DelayCompensationConfig &config);

    // Compensates a continuous sequence without clamping at intermediate path
    // boundaries. Path-local distances are retained in each intent while
    // compensation_advance_mm is measured over the flattened extrusion order.
    static std::vector<DelayCompensationPath> compensate_sequence(
        const std::vector<DelayCompensationPath> &paths,
        const DelayCompensationConfig &config);
};

} // namespace Slic3r::CoExtrusion
