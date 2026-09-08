#pragma once

#include "SurfaceDirectionResolver.hpp"
#include "SurfacePathMatcher.hpp"
#include "SurfaceProvenance.hpp"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace Slic3r::CoExtrusion {

enum class CAxisIntentStatus {
    Resolved,
    NoSurfaceSource,
    UncoloredSurface,
    MissingFaceProvenance,
    DirectionUnavailable,
};

struct CAxisPathIntent {
    Line                              segment;
    double                            path_start_mm { 0.0 };
    double                            path_end_mm { 0.0 };
    // Path index and local distance of the future intent selected by delay
    // compensation. The index is zero for the legacy single-path API.
    size_t                            control_source_path_index { 0 };
    double                            control_source_path_mm { 0.0 };
    // Extruded-path distance between this command and its control source.
    // Unlike path_start_mm, this remains meaningful across path boundaries.
    double                            compensation_advance_mm { 0.0 };
    // Painted color at this path segment. This remains tied to geometry when
    // delay compensation advances the mechanical direction from a later
    // segment, so G-code preview still reproduces the original painting.
    std::optional<SurfaceColorId>     target_color_id;
    CAxisIntentStatus                 status { CAxisIntentStatus::NoSurfaceSource };
    std::optional<SurfaceSliceMatch>  slice_source;
    std::optional<SurfaceProvenance>  surface;
    std::optional<DirectionResolution> direction;
};

class CAxisIntentBuilder
{
public:
    explicit CAxisIntentBuilder(SurfaceDirectionResolver resolver)
        : m_direction_resolver(std::move(resolver))
    {}

    std::vector<CAxisPathIntent> build(
        const std::vector<MatchedSurfacePathSegment> &segments,
        const ObjectSurfaceProvenanceResolver &surface_resolver,
        double slice_z_mm,
        const Profile &profile,
        std::optional<double> initial_reference_angle_deg = std::nullopt) const;

private:
    SurfaceDirectionResolver m_direction_resolver;
};

} // namespace Slic3r::CoExtrusion
