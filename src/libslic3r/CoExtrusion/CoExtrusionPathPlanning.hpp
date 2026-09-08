#pragma once

#include "CAxisMotionPlanner.hpp"

#include <optional>
#include <vector>

namespace Slic3r {

class FullPrintConfig;
class PrintObject;

namespace CoExtrusion {

inline constexpr double GENTLE_C_AXIS_TRANSITION_SPEED_DEG_S = 360.0;
inline constexpr double GENTLE_C_AXIS_TRANSITION_ACCELERATION_DEG_S2 = 1440.0;
inline constexpr double GENTLE_C_AXIS_TRANSITION_PATH_SPEED_MM_S = 15.0;

class ObjectSurfaceProvenanceResolver;

// Maps a finalized XYZ speed interval back to cumulative XY distance along the
// original ExtrusionPath. duration_s may include Z motion for contoured paths.
struct PathTimingRange {
    double path_start_mm { 0.0 };
    double path_end_mm { 0.0 };
    double duration_s { 0.0 };
};

struct PreparedCoExtrusionPath {
    std::vector<CAxisPathIntent> intents;
    std::vector<double>          durations_s;
    double                       effective_mm3_per_mm { 0.0 };
};

struct PlannedCoExtrusionPath {
    std::vector<CAxisPathIntent> intents;
    CAxisMotionPlan              motion;
};

class CoExtrusionPathPlanning
{
public:
    // Geometry stage. This may be run for a group of finalized paths before
    // any of them is emitted.
    static std::optional<PreparedCoExtrusionPath> prepare(
        const PrintObject &print_object,
        size_t layer_index,
        double slice_z_mm,
        double object_print_z_mm,
        const ExtrusionPath &path,
        const std::vector<PathTimingRange> &timing,
        double effective_mm3_per_mm,
        const ObjectSurfaceProvenanceResolver &surface_resolver,
        const FullPrintConfig &config,
        size_t filament_id,
        std::optional<double> previous_angle_deg);

    // Delay stage. All entries are expected to use the same active filament
    // and to already be in final print order.
    static std::vector<PreparedCoExtrusionPath> compensate_delay_sequence(
        const std::vector<PreparedCoExtrusionPath> &paths,
        const FullPrintConfig &config,
        size_t filament_id);

    // Motion stage. Call sequentially so each result's final angle becomes the
    // reference angle for the next prepared path.
    static std::optional<PlannedCoExtrusionPath> finalize(
        const PreparedCoExtrusionPath &path,
        const FullPrintConfig &config,
        std::optional<double> previous_angle_deg);

    static std::vector<PlannedCoExtrusionPath> finalize_sequence(
        const std::vector<PreparedCoExtrusionPath> &paths,
        const FullPrintConfig &config,
        std::optional<double> previous_angle_deg);

    // Single-path compatibility entry point.
    static std::optional<PlannedCoExtrusionPath> plan(
        const PrintObject &print_object,
        size_t layer_index,
        double slice_z_mm,
        double object_print_z_mm,
        const ExtrusionPath &path,
        const std::vector<PathTimingRange> &timing,
        double effective_mm3_per_mm,
        const ObjectSurfaceProvenanceResolver &surface_resolver,
        const FullPrintConfig &config,
        size_t filament_id,
        std::optional<double> previous_angle_deg);
};

} // namespace CoExtrusion
} // namespace Slic3r
