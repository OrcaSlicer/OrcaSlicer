#include "CAxisMotionPlanner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r::CoExtrusion {

namespace {

constexpr double ANGLE_EPSILON = 1e-9;
constexpr double TIME_EPSILON  = 1e-9;
constexpr double DISCONTINUITY_ANGLE_DEG = 45.0;

double positive_or_infinity(double value)
{
    return value > 0.0 && std::isfinite(value) ? value : std::numeric_limits<double>::infinity();
}

double rotation_duration(double angular_distance_deg, double speed_deg_s, double acceleration_deg_s2)
{
    const double distance = std::abs(angular_distance_deg);
    if (distance <= ANGLE_EPSILON)
        return 0.0;

    const double speed_limit = positive_or_infinity(speed_deg_s);
    const double acceleration = positive_or_infinity(acceleration_deg_s2);
    if (!std::isfinite(speed_limit) && !std::isfinite(acceleration))
        return 0.0;
    if (!std::isfinite(acceleration))
        return distance / speed_limit;
    if (!std::isfinite(speed_limit))
        return 2.0 * std::sqrt(distance / acceleration);

    const double acceleration_distance = speed_limit * speed_limit / acceleration;
    if (distance <= acceleration_distance)
        return 2.0 * std::sqrt(distance / acceleration);
    return 2.0 * speed_limit / acceleration + (distance - acceleration_distance) / speed_limit;
}

struct RotationProfile {
    double distance_deg { 0.0 };
    double duration_s { 0.0 };
    double acceleration_s { 0.0 };
    double cruise_s { 0.0 };
    double peak_speed_deg_s { 0.0 };
    double acceleration_deg_s2 { 0.0 };
};

RotationProfile make_rotation_profile(
    double angular_distance_deg,
    double speed_deg_s,
    double acceleration_deg_s2)
{
    RotationProfile profile;
    profile.distance_deg = angular_distance_deg;
    const double distance = std::abs(angular_distance_deg);
    if (distance <= ANGLE_EPSILON || speed_deg_s <= 0.0 || acceleration_deg_s2 <= 0.0)
        return profile;

    profile.acceleration_deg_s2 = acceleration_deg_s2;
    const double acceleration_distance = speed_deg_s * speed_deg_s / acceleration_deg_s2;
    if (distance <= acceleration_distance) {
        profile.acceleration_s = std::sqrt(distance / acceleration_deg_s2);
        profile.peak_speed_deg_s = acceleration_deg_s2 * profile.acceleration_s;
    } else {
        profile.acceleration_s = speed_deg_s / acceleration_deg_s2;
        profile.peak_speed_deg_s = speed_deg_s;
        profile.cruise_s = (distance - acceleration_distance) / speed_deg_s;
    }
    profile.duration_s = 2.0 * profile.acceleration_s + profile.cruise_s;
    return profile;
}

double rotation_profile_position(const RotationProfile &profile, double time_s)
{
    const double distance = std::abs(profile.distance_deg);
    if (distance <= ANGLE_EPSILON || profile.duration_s <= TIME_EPSILON)
        return 0.0;

    const double time = std::clamp(time_s, 0.0, profile.duration_s);
    const double acceleration_distance = 0.5 * profile.acceleration_deg_s2 *
                                         profile.acceleration_s * profile.acceleration_s;
    double position;
    if (time <= profile.acceleration_s) {
        position = 0.5 * profile.acceleration_deg_s2 * time * time;
    } else if (time <= profile.acceleration_s + profile.cruise_s) {
        position = acceleration_distance + profile.peak_speed_deg_s *
                   (time - profile.acceleration_s);
    } else {
        const double remaining = profile.duration_s - time;
        position = distance - 0.5 * profile.acceleration_deg_s2 * remaining * remaining;
    }
    return std::copysign(std::clamp(position, 0.0, distance), profile.distance_deg);
}

} // namespace

std::optional<CAxisMotionPlanner::AngleSelection> CAxisMotionPlanner::select_angle(
    const AngularInterval &interval,
    std::optional<double> previous_angle_deg) const
{
    const double half_width = std::clamp(interval.half_width_deg, 0.0, 180.0);
    const double center     = normalize_degrees(interval.center_deg);

    if (m_config.rotation_mode != CAxisRotationMode::LimitedRange) {
        if (previous_angle_deg) {
            if (interval.contains(*previous_angle_deg))
                return AngleSelection { *previous_angle_deg, false };

            const double nearest = interval.nearest_angle(*previous_angle_deg);
            if (std::abs(nearest - *previous_angle_deg) <= std::max(0.0, m_config.angle_tolerance_deg))
                return AngleSelection { *previous_angle_deg, true };
        }

        if (interval.full_circle())
            return AngleSelection { previous_angle_deg.value_or(0.0), false };

        const double reference = previous_angle_deg.value_or(0.0);
        double unwrapped_center = unwrap_near(center, reference);
        if (m_config.rotation_mode == CAxisRotationMode::PositiveOnly &&
            unwrapped_center + half_width < reference - ANGLE_EPSILON)
            unwrapped_center += 360.0;

        return AngleSelection {
            std::clamp(reference, unwrapped_center - half_width, unwrapped_center + half_width),
            false
        };
    }

    if (!std::isfinite(m_config.min_angle_deg) || !std::isfinite(m_config.max_angle_deg) ||
        m_config.min_angle_deg > m_config.max_angle_deg)
        return std::nullopt;

    const auto within_soft_limits = [this](double angle) {
        return angle >= m_config.min_angle_deg - ANGLE_EPSILON &&
               angle <= m_config.max_angle_deg + ANGLE_EPSILON;
    };

    if (previous_angle_deg && within_soft_limits(*previous_angle_deg)) {
        if (interval.contains(*previous_angle_deg))
            return AngleSelection { *previous_angle_deg, false };

        const double nearest = interval.nearest_angle(*previous_angle_deg);
        if (std::abs(nearest - *previous_angle_deg) <= std::max(0.0, m_config.angle_tolerance_deg))
            return AngleSelection { *previous_angle_deg, true };
    }

    if (interval.full_circle()) {
        const double angle = std::clamp(
            previous_angle_deg.value_or(0.0), m_config.min_angle_deg, m_config.max_angle_deg);
        return AngleSelection { angle, false };
    }

    const double reference = std::clamp(
        previous_angle_deg.value_or(0.0), m_config.min_angle_deg, m_config.max_angle_deg);
    const long long first_period = static_cast<long long>(
        std::ceil((m_config.min_angle_deg - (center + half_width)) / 360.0));
    const long long last_period = static_cast<long long>(
        std::floor((m_config.max_angle_deg - (center - half_width)) / 360.0));

    std::optional<double> best;
    for (long long period = first_period; period <= last_period; ++period) {
        const double lower = std::max(m_config.min_angle_deg, center - half_width + 360.0 * double(period));
        const double upper = std::min(m_config.max_angle_deg, center + half_width + 360.0 * double(period));
        if (lower > upper + ANGLE_EPSILON)
            continue;

        double candidate = std::clamp(reference, lower, upper);
        if (!best || std::abs(candidate - reference) < std::abs(*best - reference))
            best = candidate;
    }
    if (!best)
        return std::nullopt;
    return AngleSelection { *best, false };
}

double CAxisMotionPlanner::minimum_rotation_duration(double angular_distance_deg) const
{
    double speed_limit = positive_or_infinity(m_config.max_speed_deg_s);
    if (m_config.max_jerk_deg_s > 0.0 && std::isfinite(m_config.max_jerk_deg_s))
        speed_limit = std::min(speed_limit, m_config.max_jerk_deg_s);
    return rotation_duration(angular_distance_deg, speed_limit, m_config.max_acceleration_deg_s2);
}

double CAxisMotionPlanner::transition_rotation_duration(double angular_distance_deg) const
{
    return rotation_duration(
        angular_distance_deg,
        m_config.transition_speed_deg_s,
        m_config.transition_acceleration_deg_s2);
}

void CAxisMotionPlanner::spread_transitions(
    CAxisMotionPlan &plan,
    const std::vector<CAxisPathIntent> &intents,
    double initial_angle_deg) const
{
    const double speed_limit = m_config.transition_speed_deg_s;
    const double acceleration = m_config.transition_acceleration_deg_s2;
    const double path_speed = m_config.transition_path_speed_mm_s;
    if (!(speed_limit > 0.0) || !(acceleration > 0.0) || !(path_speed > 0.0) ||
        !std::isfinite(speed_limit) || !std::isfinite(acceleration) || !std::isfinite(path_speed))
        return;

    const size_t count = plan.segments.size();
    std::vector<std::optional<double>> targets(count);
    std::vector<double> required_durations(count, 0.0);
    std::vector<bool> spread(count, false);
    std::vector<bool> preposition(count, false);
    for (size_t index = 0; index < count; ++index) {
        CAxisMotionSegment &segment = plan.segments[index];
        segment.ideal_target_angle_deg = segment.target_angle_deg;
        targets[index] = segment.target_angle_deg;
    }

    const auto valid_segment = [&plan](size_t index) {
        const CAxisMotionSegment &segment = plan.segments[index];
        return segment.target_angle_deg.has_value() &&
               segment.status != CAxisMotionStatus::InvalidTiming &&
               segment.status != CAxisMotionStatus::UnreachableAngle &&
               segment.status != CAxisMotionStatus::MissingInitialAngle &&
               segment.nominal_duration_s > TIME_EPSILON;
    };
    const auto segment_length = [&plan](size_t index) {
        return std::max(0.0, plan.segments[index].path_end_mm - plan.segments[index].path_start_mm);
    };

    // A transition is an explicit colour boundary or a discontinuous orientation jump.
    // Plan it backwards so the requested angle is reached at the boundary, instead of
    // continuously chasing the target and carrying a permanent phase error around the wall.
    size_t run_start = 0;
    size_t last_boundary = 0;
    bool in_run = false;
    for (size_t index = 0; index < count; ++index) {
        if (!valid_segment(index)) {
            in_run = false;
            continue;
        }
        if (!in_run) {
            run_start = index;
            last_boundary = index;
            in_run = true;
            const double start_angle = plan.segments[index].start_angle_deg.value_or(initial_angle_deg);
            if (std::abs(*targets[index] - start_angle) > ANGLE_EPSILON)
                preposition[index] = true;
            continue;
        }

        const size_t previous = index - 1;
        const bool color_changed = index < intents.size() && previous < intents.size() &&
            intents[index].target_color_id && intents[previous].target_color_id &&
            intents[index].target_color_id != intents[previous].target_color_id;
        const bool discontinuity = std::abs(*targets[index] - *targets[previous]) >
                                   std::max(DISCONTINUITY_ANGLE_DEG,
                                            2.0 * std::max(0.0, m_config.angle_tolerance_deg));
        if (!color_changed && !discontinuity)
            continue;

        const double end_angle = *targets[index];
        size_t window_start = index;
        double available_length = 0.0;
        RotationProfile profile;
        double start_angle = *targets[previous];
        for (size_t candidate = index; candidate-- > last_boundary;) {
            window_start = candidate;
            available_length += segment_length(candidate);
            start_angle = candidate > run_start && targets[candidate - 1] ?
                *targets[candidate - 1] :
                plan.segments[run_start].start_angle_deg.value_or(initial_angle_deg);
            profile = make_rotation_profile(end_angle - start_angle, speed_limit, acceleration);
            if (available_length + ANGLE_EPSILON >= profile.duration_s * path_speed)
                break;
        }

        if (available_length <= ANGLE_EPSILON || profile.duration_s <= TIME_EPSILON) {
            preposition[index] = std::abs(end_angle - start_angle) > ANGLE_EPSILON;
            last_boundary = index;
            continue;
        }

        const double desired_length = profile.duration_s * path_speed;
        const double effective_path_speed = std::min(path_speed, available_length / profile.duration_s);
        const double idle_prefix = std::max(0.0, available_length - desired_length);
        double traversed = 0.0;
        for (size_t transition_index = window_start; transition_index < index; ++transition_index) {
            const double length = segment_length(transition_index);
            traversed += length;
            const double active_length = std::max(0.0, traversed - idle_prefix);
            const double time = std::min(profile.duration_s, active_length / effective_path_speed);
            targets[transition_index] = start_angle + rotation_profile_position(profile, time);
            required_durations[transition_index] = std::max(
                required_durations[transition_index], length / effective_path_speed);
            spread[transition_index] = true;
        }
        // Avoid a residual rounding step on the first segment of the new colour.
        targets[previous] = end_angle;
        last_boundary = index;
    }

    double commanded_angle = initial_angle_deg;
    for (size_t index = 0; index < count; ++index) {
        CAxisMotionSegment &segment = plan.segments[index];
        segment.transition_spread = spread[index];
        if (!targets[index] || !valid_segment(index))
            continue;

        segment.start_angle_deg = commanded_angle;
        segment.target_angle_deg = targets[index];
        segment.angular_delta_deg = *targets[index] - commanded_angle;
        segment.emit_axis_command = std::abs(segment.angular_delta_deg) > ANGLE_EPSILON;
        segment.speed_limited = false;
        segment.acceleration_limited = false;
        segment.jerk_limited = false;

        if (preposition[index]) {
            segment.status = CAxisMotionStatus::PrepositionRequired;
            segment.minimum_rotation_duration_s = transition_rotation_duration(segment.angular_delta_deg);
            segment.planned_duration_s = segment.nominal_duration_s;
            segment.xyz_speed_scale = 1.0;
            commanded_angle = *targets[index];
            continue;
        }

        const double average_speed_limit = m_config.max_jerk_deg_s > 0.0 ?
            std::min(speed_limit, m_config.max_jerk_deg_s) : speed_limit;
        const double speed_duration = segment.emit_axis_command ?
            std::abs(segment.angular_delta_deg) / average_speed_limit : 0.0;
        segment.minimum_rotation_duration_s = std::max(speed_duration, required_durations[index]);
        segment.planned_duration_s = std::max(
            segment.nominal_duration_s, segment.minimum_rotation_duration_s);
        segment.xyz_speed_scale = segment.nominal_duration_s > TIME_EPSILON ?
            std::clamp(segment.nominal_duration_s / segment.planned_duration_s, 0.0, 1.0) : 0.0;
        segment.speed_limited = speed_duration > segment.nominal_duration_s + TIME_EPSILON;
        segment.status = segment.xyz_speed_scale < 1.0 - TIME_EPSILON ?
            CAxisMotionStatus::SlowDownRequired : CAxisMotionStatus::Synchronized;
        commanded_angle = *targets[index];
    }
    plan.final_angle_deg = commanded_angle;
}

CAxisMotionPlan CAxisMotionPlanner::plan(
    const std::vector<CAxisPathIntent> &intents,
    const std::vector<double> &nominal_durations_s,
    std::optional<double> initial_angle_deg) const
{
    CAxisMotionPlan result;
    result.segments.reserve(intents.size());
    std::optional<double> previous_angle_deg = initial_angle_deg;
    std::optional<SurfaceColorId> previous_color_id;
    bool has_previous_oriented_segment = false;

    for (size_t index = 0; index < intents.size(); ++index) {
        const CAxisPathIntent &intent = intents[index];
        CAxisMotionSegment segment;
        segment.intent_index  = index;
        segment.path_start_mm = intent.path_start_mm;
        segment.path_end_mm   = intent.path_end_mm;
        segment.start_angle_deg = previous_angle_deg;

        if (index >= nominal_durations_s.size() || !std::isfinite(nominal_durations_s[index]) ||
            nominal_durations_s[index] < 0.0) {
            segment.status = CAxisMotionStatus::InvalidTiming;
            result.has_invalid_timing = true;
            result.segments.emplace_back(segment);
            continue;
        }
        segment.nominal_duration_s = nominal_durations_s[index];
        segment.planned_duration_s = segment.nominal_duration_s;

        if (intent.status != CAxisIntentStatus::Resolved || !intent.direction) {
            segment.status = CAxisMotionStatus::NoCommand;
            result.segments.emplace_back(segment);
            continue;
        }

        if (intent.direction->kind == DirectionIntentKind::HoldPrevious) {
            if (!previous_angle_deg) {
                segment.status = CAxisMotionStatus::MissingInitialAngle;
            } else {
                segment.status           = CAxisMotionStatus::Synchronized;
                segment.target_angle_deg = previous_angle_deg;
            }
            result.segments.emplace_back(segment);
            continue;
        }

        if (intent.direction->kind != DirectionIntentKind::OrientSurface ||
            !intent.direction->acceptable_angles) {
            segment.status = CAxisMotionStatus::NoCommand;
            result.segments.emplace_back(segment);
            continue;
        }

        const std::optional<AngleSelection> selection = select_angle(
            *intent.direction->acceptable_angles, previous_angle_deg);
        if (!selection) {
            segment.status = CAxisMotionStatus::UnreachableAngle;
            result.has_unreachable_angle = true;
            result.segments.emplace_back(segment);
            continue;
        }

        segment.target_angle_deg  = selection->angle_deg;
        segment.held_by_tolerance = selection->held_by_tolerance;
        const bool first_oriented_segment = !has_previous_oriented_segment;
        const bool color_changed = previous_color_id && intent.target_color_id &&
                                   *previous_color_id != *intent.target_color_id;
        previous_color_id = intent.target_color_id;
        has_previous_oriented_segment = true;
        if (!previous_angle_deg) {
            previous_angle_deg = selection->angle_deg;
            result.initial_positioning_angle_deg = selection->angle_deg;
            segment.status            = CAxisMotionStatus::Initialized;
            segment.start_angle_deg   = selection->angle_deg;
            segment.emit_axis_command = true;
            result.segments.emplace_back(segment);
            continue;
        }

        segment.angular_delta_deg = selection->angle_deg - *previous_angle_deg;
        segment.emit_axis_command = std::abs(segment.angular_delta_deg) > ANGLE_EPSILON;
        segment.minimum_rotation_duration_s = minimum_rotation_duration(segment.angular_delta_deg);

        const double nominal_speed = segment.nominal_duration_s > TIME_EPSILON ?
            std::abs(segment.angular_delta_deg) / segment.nominal_duration_s :
            (segment.emit_axis_command ? std::numeric_limits<double>::infinity() : 0.0);
        segment.speed_limited = m_config.max_speed_deg_s > 0.0 && nominal_speed > m_config.max_speed_deg_s + ANGLE_EPSILON;
        segment.jerk_limited  = m_config.max_jerk_deg_s > 0.0 && nominal_speed > m_config.max_jerk_deg_s + ANGLE_EPSILON;
        const double acceleration_duration = m_config.max_acceleration_deg_s2 > 0.0 ?
            2.0 * std::sqrt(std::abs(segment.angular_delta_deg) / m_config.max_acceleration_deg_s2) : 0.0;
        segment.acceleration_limited = acceleration_duration > segment.nominal_duration_s + TIME_EPSILON;

        // A short segment can exceed the conservative rest-to-rest timing even
        // for a 4-degree correction. That is continuous contour tracking, not
        // a large-rotation fallback: keep XY/E/C together and reduce feedrate.
        // Require an adjacent oriented segment of the same colour so gaps,
        // colour changes and path entry still use explicit positioning.
        const bool continuous_correction = index > 0 &&
            intents[index - 1].status == CAxisIntentStatus::Resolved &&
            intents[index - 1].direction &&
            intents[index - 1].direction->kind == DirectionIntentKind::OrientSurface &&
            intent.target_color_id && intents[index - 1].target_color_id == intent.target_color_id &&
            intents[index - 1].segment.b == intent.segment.a &&
            std::abs(segment.angular_delta_deg) <= DISCONTINUITY_ANGLE_DEG;

        if (!segment.emit_axis_command) {
            segment.status = CAxisMotionStatus::Synchronized;
        } else if (m_config.rotation_mode == CAxisRotationMode::LimitedRange &&
                   std::abs(segment.angular_delta_deg) > 180.0 + ANGLE_EPSILON) {
            // Equivalent orientations are not equivalent cable positions.
            // A long return across the range (e.g. 175 -> -175) must never
            // extrude through all intermediate colours, even if timing allows it.
            segment.status = CAxisMotionStatus::IndependentRotationRequired;
        } else if ((m_config.preposition_first_segment && first_oriented_segment) || color_changed) {
            segment.status = CAxisMotionStatus::PrepositionRequired;
        } else if (segment.minimum_rotation_duration_s <= segment.nominal_duration_s + TIME_EPSILON) {
            segment.status = CAxisMotionStatus::Synchronized;
        } else if (continuous_correction ||
                   m_config.large_rotation_strategy == LargeRotationStrategy::SlowDown) {
            // Continuous corrections must also work with older presets that
            // select preposition or independent_rotation for large jumps.
            segment.status             = CAxisMotionStatus::SlowDownRequired;
            segment.planned_duration_s = segment.minimum_rotation_duration_s;
            segment.xyz_speed_scale    = segment.nominal_duration_s > TIME_EPSILON ?
                std::clamp(segment.nominal_duration_s / segment.planned_duration_s, 0.0, 1.0) : 0.0;
        } else if (m_config.large_rotation_strategy == LargeRotationStrategy::Preposition) {
            segment.status = CAxisMotionStatus::PrepositionRequired;
        } else {
            segment.status = CAxisMotionStatus::IndependentRotationRequired;
        }

        previous_angle_deg = selection->angle_deg;
        result.segments.emplace_back(segment);
    }

    result.final_angle_deg = previous_angle_deg;
    if (m_config.spread_transitions && initial_angle_deg &&
        m_config.rotation_mode != CAxisRotationMode::LimitedRange)
        spread_transitions(result, intents, *initial_angle_deg);
    return result;
}

} // namespace Slic3r::CoExtrusion
