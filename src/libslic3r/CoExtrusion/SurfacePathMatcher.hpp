#pragma once

#include "SurfaceSliceSidecar.hpp"
#include "../ExtrusionEntity.hpp"

#include <optional>
#include <vector>

namespace Slic3r::CoExtrusion {

class ObjectSurfaceProvenanceResolver;

struct MatchedSurfacePathSegment {
    Line                             segment;
    double                           path_start_mm { 0.0 };
    double                           path_end_mm { 0.0 };
    std::optional<double>            query_z_mm;
    std::optional<SurfaceSliceMatch> surface;
    // Actual nozzle height change over this segment, in millimeters.
    double                           z_delta_mm { 0.0 };
};

// Splits finalized visible-surface paths into bounded microsegments and
// resolves their painted source face from the sidecar or annotated mesh.
class SurfacePathMatcher
{
public:
    static std::vector<MatchedSurfacePathSegment> match_external_path(
        const SurfaceSliceSidecar &sidecar,
        size_t layer_index,
        const ExtrusionPath &path,
        double max_segment_length_mm,
        double max_source_distance_mm,
        double min_direction_alignment = 0.25);

    // Horizontal top/bottom paths do not intersect the per-layer slice
    // sidecar. Match their segment midpoints directly to the annotated mesh
    // face above/below the extrusion instead.
    static std::vector<MatchedSurfacePathSegment> match_horizontal_surface_path(
        const ObjectSurfaceProvenanceResolver &surface_resolver,
        double slice_z_mm,
        const ExtrusionPath &path,
        double max_segment_length_mm,
        double max_surface_distance_mm);

    // Sloped and Z-contoured paths leave the layer slicing plane. Resolve
    // every microsegment against the annotated mesh at its actual extrusion Z
    // instead of reusing the layer-wide slice provenance.
    static std::vector<MatchedSurfacePathSegment> match_nonplanar_surface_path(
        const ObjectSurfaceProvenanceResolver &surface_resolver,
        double object_print_z_mm,
        const ExtrusionPath &path,
        double max_segment_length_mm,
        double max_surface_distance_mm);
};

} // namespace Slic3r::CoExtrusion
