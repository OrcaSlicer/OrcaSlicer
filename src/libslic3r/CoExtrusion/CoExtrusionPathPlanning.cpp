#include "CoExtrusionPathPlanning.hpp"

#include "SurfacePathMatcher.hpp"
#include "SurfaceProvenance.hpp"
#include "DelayCompensator.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Slic3r::CoExtrusion {

namespace {

std::optional<CAxisMotionPlannerConfig> make_motion_config(const FullPrintConfig &config)
{
    const std::optional<CAxisRotationMode> rotation_mode =
        parse_c_axis_rotation_mode(config.coextrusion_c_axis_rotation_mode.value);
    const std::optional<LargeRotationStrategy> large_rotation_strategy =
        parse_large_rotation_strategy(config.coextrusion_large_rotation_strategy.value);
    if (!rotation_mode || !large_rotation_strategy)
        return std::nullopt;

    CAxisMotionPlannerConfig motion_config;
    motion_config.rotation_mode = config.coextrusion_c_axis_has_slip_ring.value ?
        *rotation_mode : CAxisRotationMode::LimitedRange;
    motion_config.large_rotation_strategy = *large_rotation_strategy;
    motion_config.min_angle_deg = config.coextrusion_c_axis_min.value;
    motion_config.max_angle_deg = config.coextrusion_c_axis_max.value;
    motion_config.max_speed_deg_s = config.coextrusion_c_axis_max_speed.value > 0.0 ?
        std::min(config.coextrusion_c_axis_max_speed.value, GENTLE_C_AXIS_TRANSITION_SPEED_DEG_S) :
        GENTLE_C_AXIS_TRANSITION_SPEED_DEG_S;
    motion_config.max_acceleration_deg_s2 = config.coextrusion_c_axis_max_acceleration.value > 0.0 ?
        std::min(config.coextrusion_c_axis_max_acceleration.value, GENTLE_C_AXIS_TRANSITION_ACCELERATION_DEG_S2) :
        GENTLE_C_AXIS_TRANSITION_ACCELERATION_DEG_S2;
    motion_config.max_jerk_deg_s = config.coextrusion_c_axis_max_jerk.value;
    motion_config.angle_tolerance_deg = config.coextrusion_angle_tolerance.value;
    motion_config.transition_speed_deg_s = motion_config.max_speed_deg_s;
    if (config.coextrusion_c_axis_max_jerk.value > 0.0)
        motion_config.transition_speed_deg_s = std::min(
            motion_config.transition_speed_deg_s, config.coextrusion_c_axis_max_jerk.value);
    motion_config.transition_acceleration_deg_s2 = motion_config.max_acceleration_deg_s2;
    motion_config.transition_path_speed_mm_s = GENTLE_C_AXIS_TRANSITION_PATH_SPEED_MM_S;
    // Keep every extrusion move inside the angular interval resolved for its
    // own surface colour. A colour boundary is handled by a non-extruding
    // preposition instead of rotating through the preceding colour region.
    motion_config.spread_transitions = false;
    motion_config.preposition_first_segment = true;
    return motion_config;
}

bool is_supported_surface_role(ExtrusionRole role)
{
    return role == erExternalPerimeter || role == erOverhangPerimeter ||
           role == erTopSolidInfill || role == erBottomSurface;
}

std::vector<double> durations_for_intents(
    const std::vector<CAxisPathIntent> &intents,
    const std::vector<PathTimingRange> &timing)
{
    std::vector<double> durations;
    durations.reserve(intents.size());
    for (const CAxisPathIntent &intent : intents) {
        const double intent_length = intent.path_end_mm - intent.path_start_mm;
        double covered_length = 0.0;
        double duration       = 0.0;
        for (const PathTimingRange &range : timing) {
            const double range_length = range.path_end_mm - range.path_start_mm;
            if (range_length <= 0.0 || !std::isfinite(range.duration_s) || range.duration_s < 0.0)
                continue;

            const double overlap_start = std::max(intent.path_start_mm, range.path_start_mm);
            const double overlap_end   = std::min(intent.path_end_mm, range.path_end_mm);
            const double overlap       = overlap_end - overlap_start;
            if (overlap <= 0.0)
                continue;
            covered_length += overlap;
            duration += range.duration_s * overlap / range_length;
        }

        if (intent_length <= 0.0 || covered_length <= 0.0) {
            durations.emplace_back(std::numeric_limits<double>::quiet_NaN());
        } else {
            // Subdivision rounding may move an endpoint by a scaled coordinate.
            // Preserve the average speed when that creates a tiny uncovered tail.
            durations.emplace_back(duration * intent_length / covered_length);
        }
    }
    return durations;
}

} // namespace

std::optional<PlannedCoExtrusionPath> CoExtrusionPathPlanning::plan(
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
    std::optional<double> previous_angle_deg)
{
    std::optional<PreparedCoExtrusionPath> prepared = prepare(
        print_object,
        layer_index,
        slice_z_mm,
        object_print_z_mm,
        path,
        timing,
        effective_mm3_per_mm,
        surface_resolver,
        config,
        filament_id,
        previous_angle_deg);
    if (!prepared)
        return std::nullopt;

    std::vector<PreparedCoExtrusionPath> compensated = compensate_delay_sequence(
        { std::move(*prepared) }, config, filament_id);
    if (compensated.empty())
        return std::nullopt;
    return finalize(compensated.front(), config, previous_angle_deg);
}

std::optional<PreparedCoExtrusionPath> CoExtrusionPathPlanning::prepare(
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
    std::optional<double> previous_angle_deg)
{
    if (!config.coextrusion_c_axis_enabled.value || !config.coextrusion_surface_control.value ||
        !is_supported_surface_role(path.role()))
        return std::nullopt;

    const std::shared_ptr<const SurfaceSliceSidecar> &sidecar = print_object.coextrusion_surface_sidecar();
    const bool is_vertical_surface = path.role() == erExternalPerimeter || path.role() == erOverhangPerimeter;
    const bool is_nonplanar_path = path.z_contoured || dynamic_cast<const ExtrusionPathSloped*>(&path) != nullptr;
    if (is_vertical_surface && !is_nonplanar_path &&
        (!sidecar || sidecar->empty() || sidecar->layer(layer_index) == nullptr))
        return std::nullopt;

    std::string profile_error;
    const std::optional<Profile> profile = Profile::parse(
        config.filament_coextrusion_profile.get_at(filament_id), &profile_error);
    if (!profile || profile->empty())
        return std::nullopt;

    const std::optional<TopBottomStrategy> top_bottom_strategy =
        parse_top_bottom_strategy(config.coextrusion_top_bottom_strategy.value);
    if (!top_bottom_strategy)
        return std::nullopt;

    const double max_segment_length = config.coextrusion_max_segment_length.value;
    const double max_source_distance = std::max(0.25, 0.5 * std::max(0.0, double(path.width)) + 0.2);
    std::vector<MatchedSurfacePathSegment> matched;
    if (is_nonplanar_path) {
        matched = SurfacePathMatcher::match_nonplanar_surface_path(
            surface_resolver, object_print_z_mm, path, max_segment_length, max_source_distance);
    } else if (is_vertical_surface) {
        matched = SurfacePathMatcher::match_external_path(
            *sidecar, layer_index, path, max_segment_length, max_source_distance);
    } else {
        const double max_horizontal_distance = std::max(0.3, std::max(0.0, double(path.height)) + 0.1);
        matched = SurfacePathMatcher::match_horizontal_surface_path(
            surface_resolver, slice_z_mm, path, max_segment_length, max_horizontal_distance);
    }
    if (matched.empty() || std::none_of(matched.begin(), matched.end(),
            [](const MatchedSurfacePathSegment &segment) { return segment.surface.has_value(); }))
        return std::nullopt;

    DirectionResolverConfig direction_config;
    direction_config.use_xy_normal_only =
        config.coextrusion_color_method.value == CoExtrusionColorMethod::NormalXY;
    direction_config.axis_direction = config.coextrusion_c_axis_direction.value;
    direction_config.axis_zero_offset_deg = config.coextrusion_c_axis_zero_offset.value;
    direction_config.filament_calibration_offset_deg =
        config.filament_coextrusion_calibration_offset.get_at(filament_id);
    direction_config.normal_xy_threshold = config.coextrusion_normal_xy_threshold.value;
    // The motion planner may retain an angle just outside its mathematical
    // interval by angle_tolerance. Shrink the resolver interval by at least
    // the same amount so this anti-jitter allowance never crosses the
    // physical color-sector boundary.
    direction_config.angular_margin_deg = std::max(
        config.coextrusion_angular_safety_margin.value,
        config.coextrusion_angle_tolerance.value);
    direction_config.top_bottom_strategy = *top_bottom_strategy;
    // Read the finalized extrusion path, including variable-width perimeter
    // pieces and first-layer height, rather than the nominal nozzle diameter.
    // Bridges are not flattened against a layer. Nonplanar/scarf paths do not
    // yet provide the local deposited thickness perpendicular to their tangent;
    // retain the circular model instead of mistaking Z travel for thickness.
    if (!is_nonplanar_path && !is_bridge(path.role()) && !path.is_force_no_extrusion()) {
        direction_config.bead_width_mm = path.width;
        direction_config.bead_height_mm = path.height;
    }

    CAxisIntentBuilder intent_builder { SurfaceDirectionResolver(direction_config) };

    PreparedCoExtrusionPath result;
    result.intents = intent_builder.build(
        matched, surface_resolver, slice_z_mm, *profile, previous_angle_deg);
    result.durations_s = durations_for_intents(result.intents, timing);
    result.effective_mm3_per_mm = effective_mm3_per_mm;
    return result;
}

std::vector<PreparedCoExtrusionPath> CoExtrusionPathPlanning::compensate_delay_sequence(
    const std::vector<PreparedCoExtrusionPath> &paths,
    const FullPrintConfig &config,
    size_t filament_id)
{
    if (paths.empty())
        return paths;

    const std::optional<DelayModel> delay_model = parse_delay_model(
        config.filament_coextrusion_delay_model.get_at(filament_id));
    if (!delay_model || *delay_model == DelayModel::Disabled)
        return paths;

    DelayCompensationConfig delay_config;
    delay_config.model = *delay_model;
    delay_config.response_delay_s = config.filament_coextrusion_response_delay_time.get_at(filament_id);
    delay_config.transport_volume_mm3 = config.filament_coextrusion_transport_volume.get_at(filament_id);

    std::vector<DelayCompensationPath> delay_paths;
    delay_paths.reserve(paths.size());
    for (const PreparedCoExtrusionPath &path : paths)
        delay_paths.push_back({ path.intents, path.durations_s, path.effective_mm3_per_mm });
    delay_paths = DelayCompensator::compensate_sequence(delay_paths, delay_config);

    std::vector<PreparedCoExtrusionPath> compensated = paths;
    for (size_t index = 0; index < compensated.size(); ++index)
        compensated[index].intents = std::move(delay_paths[index].intents);
    return compensated;
}

std::optional<PlannedCoExtrusionPath> CoExtrusionPathPlanning::finalize(
    const PreparedCoExtrusionPath &path,
    const FullPrintConfig &config,
    std::optional<double> previous_angle_deg)
{
    const std::optional<CAxisMotionPlannerConfig> motion_config = make_motion_config(config);
    if (!motion_config)
        return std::nullopt;

    PlannedCoExtrusionPath result;
    result.intents = path.intents;
    result.motion = CAxisMotionPlanner(*motion_config).plan(
        result.intents, path.durations_s, previous_angle_deg);
    return result;
}

std::vector<PlannedCoExtrusionPath> CoExtrusionPathPlanning::finalize_sequence(
    const std::vector<PreparedCoExtrusionPath> &paths,
    const FullPrintConfig &config,
    std::optional<double> previous_angle_deg)
{
    const std::optional<CAxisMotionPlannerConfig> motion_config = make_motion_config(config);
    if (!motion_config)
        return {};

    std::vector<CAxisPathIntent> flattened_intents;
    std::vector<double> flattened_durations;
    std::vector<size_t> path_offsets;
    path_offsets.reserve(paths.size() + 1);
    path_offsets.emplace_back(0);
    for (const PreparedCoExtrusionPath &path : paths) {
        flattened_intents.insert(flattened_intents.end(), path.intents.begin(), path.intents.end());
        flattened_durations.insert(flattened_durations.end(), path.durations_s.begin(), path.durations_s.end());
        path_offsets.emplace_back(flattened_intents.size());
    }

    CAxisMotionPlan flattened_motion = CAxisMotionPlanner(*motion_config).plan(
        flattened_intents, flattened_durations, previous_angle_deg);
    if (flattened_motion.segments.size() != flattened_intents.size())
        return {};

    std::vector<PlannedCoExtrusionPath> planned;
    planned.reserve(paths.size());
    for (size_t path_index = 0; path_index < paths.size(); ++path_index) {
        PlannedCoExtrusionPath current;
        current.intents = paths[path_index].intents;
        const size_t begin = path_offsets[path_index];
        const size_t end = path_offsets[path_index + 1];
        current.motion.segments.assign(
            flattened_motion.segments.begin() + begin,
            flattened_motion.segments.begin() + end);
        for (size_t index = 0; index < current.motion.segments.size(); ++index)
            current.motion.segments[index].intent_index = index;
        const auto first_controlled = std::find_if(
            current.motion.segments.begin(), current.motion.segments.end(),
            [](const CAxisMotionSegment &segment) {
                return segment.target_angle_deg &&
                       segment.status != CAxisMotionStatus::UnreachableAngle &&
                       segment.status != CAxisMotionStatus::InvalidTiming &&
                       segment.status != CAxisMotionStatus::MissingInitialAngle;
            });
        // Only position at the first controlled segment. If that segment holds
        // its angle, the first later C correction is ordinary synchronized
        // tracking, not a reason to interrupt extrusion midway through a path.
        if (first_controlled != current.motion.segments.end() && first_controlled->emit_axis_command &&
            first_controlled->status != CAxisMotionStatus::IndependentRotationRequired) {
            first_controlled->status = CAxisMotionStatus::PrepositionRequired;
            first_controlled->planned_duration_s = first_controlled->nominal_duration_s;
            first_controlled->xyz_speed_scale = 1.0;
        }
        if (!current.motion.segments.empty()) {
            current.motion.initial_positioning_angle_deg =
                current.motion.segments.front().start_angle_deg;
            // An uncoloured tail has no target but retains the physical angle.
            current.motion.final_angle_deg = current.motion.segments.back().target_angle_deg ?
                current.motion.segments.back().target_angle_deg :
                current.motion.segments.back().start_angle_deg;
        }
        current.motion.has_unreachable_angle = std::any_of(
            current.motion.segments.begin(), current.motion.segments.end(),
            [](const CAxisMotionSegment &segment) { return segment.status == CAxisMotionStatus::UnreachableAngle; });
        current.motion.has_invalid_timing = std::any_of(
            current.motion.segments.begin(), current.motion.segments.end(),
            [](const CAxisMotionSegment &segment) { return segment.status == CAxisMotionStatus::InvalidTiming; });
        planned.emplace_back(std::move(current));
    }
    return planned;
}

} // namespace Slic3r::CoExtrusion
