#pragma once

#include "CAxisIntentBuilder.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace Slic3r::CoExtrusion {

enum class CAxisMotionStatus {
    NoCommand,
    MissingInitialAngle,
    Initialized,
    Synchronized,
    SlowDownRequired,
    PrepositionRequired,
    IndependentRotationRequired,
    UnreachableAngle,
    InvalidTiming,
};

struct CAxisMotionPlannerConfig {
    CAxisRotationMode     rotation_mode { CAxisRotationMode::ShortestPath };
    LargeRotationStrategy large_rotation_strategy { LargeRotationStrategy::SlowDown };
    double                min_angle_deg { -180.0 };
    double                max_angle_deg { 180.0 };
    double                max_speed_deg_s { 120.0 };
    double                max_acceleration_deg_s2 { 360.0 };
    double                max_jerk_deg_s { 0.0 };
    double                transition_speed_deg_s { 180.0 };
    double                transition_acceleration_deg_s2 { 720.0 };
    double                transition_path_speed_mm_s { 10.0 };
    double                angle_tolerance_deg { 0.0 };
    bool                  spread_transitions { false };
    bool                  preposition_first_segment { false };
};

struct CAxisMotionSegment {
    size_t                    intent_index { 0 };
    CAxisMotionStatus         status { CAxisMotionStatus::NoCommand };
    double                    path_start_mm { 0.0 };
    double                    path_end_mm { 0.0 };
    double                    nominal_duration_s { 0.0 };
    double                    planned_duration_s { 0.0 };
    double                    minimum_rotation_duration_s { 0.0 };
    double                    xyz_speed_scale { 1.0 };
    std::optional<double>     start_angle_deg;
    std::optional<double>     target_angle_deg;
    std::optional<double>     ideal_target_angle_deg;
    double                    angular_delta_deg { 0.0 };
    bool                      emit_axis_command { false };
    bool                      transition_spread { false };
    bool                      held_by_tolerance { false };
    bool                      speed_limited { false };
    bool                      acceleration_limited { false };
    bool                      jerk_limited { false };
};

struct CAxisMotionPlan {
    std::vector<CAxisMotionSegment> segments;
    std::optional<double>           initial_positioning_angle_deg;
    std::optional<double>           final_angle_deg;
    bool                            has_unreachable_angle { false };
    bool                            has_invalid_timing { false };
};

// Converts geometry intents into continuous command angles. Durations must be
// calculated from the finalized XYZ path speed and use the same input order as
// intents. The planner returns constraints and fallbacks but never emits G-code.
class CAxisMotionPlanner
{
public:
    explicit CAxisMotionPlanner(CAxisMotionPlannerConfig config) : m_config(config) {}

    CAxisMotionPlan plan(
        const std::vector<CAxisPathIntent> &intents,
        const std::vector<double> &nominal_durations_s,
        std::optional<double> initial_angle_deg = std::nullopt) const;

private:
    struct AngleSelection {
        double angle_deg { 0.0 };
        bool   held_by_tolerance { false };
    };

    std::optional<AngleSelection> select_angle(
        const AngularInterval &interval,
        std::optional<double> previous_angle_deg) const;

    double minimum_rotation_duration(double angular_distance_deg) const;
    double transition_rotation_duration(double angular_distance_deg) const;
    void   spread_transitions(
        CAxisMotionPlan &plan,
        const std::vector<CAxisPathIntent> &intents,
        double initial_angle_deg) const;

    CAxisMotionPlannerConfig m_config;
};

} // namespace Slic3r::CoExtrusion
