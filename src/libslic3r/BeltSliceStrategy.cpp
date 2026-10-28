#include "BeltSliceStrategy.hpp"
#include "Model.hpp"
#include "BeltTransform.hpp"
#include "Point.hpp"
#include "PrintConfig.hpp"

#include <limits>
#include <algorithm>

namespace Slic3r {

void BeltSliceStrategy::apply_preslice_transforms(Transform3d           &trafo,
                                                  const PrintConfig     &config,
                                                  const ModelVolumePtrs &model_volumes,
                                                  double                *out_belt_min_z)
{
    // 1. Belt rotation — the sole mesh-side belt transform (matching
    //    BeltTransformPipeline::build_forward_transform).  Only active in
    //    belt-printer mode.
    bool has_rotation = false;
    if (config.belt_printer.value) {
        const Matrix3d rot = BeltTransformPipeline::build_rotation_matrix(config, &has_rotation);
        if (has_rotation) {
            Transform3d belt_xform = Transform3d::Identity();
            belt_xform.linear() = rot;
            trafo = belt_xform * trafo;
        }
    }

    if (!has_rotation)
        return;

    // 2. Z-shift — detect if the mesh clips below the build plate after the
    // transforms and lift it.  Each mesh vertex must be brought into object space
    // via mv->get_matrix() before applying the full trafo (which is in object
    // space).  Missing this on assemblies (where per-volume get_matrix() positions
    // each volume within the object) would compute min_z against mesh-local vertex
    // coordinates rather than object-space coordinates, so volumes translated along
    // the slicer's Z axis would be silently excluded from the bound check.

    //
    // The lift is measured to the lowest point of the SUPPORT region, not of the
    // mesh: the belt floor (z = shear * u in this rotated frame, u the from-axis
    // coordinate) runs below every vertex, and under the leading end of an
    // overhang it lies below the lowest vertex by up to the overhang's length
    // times the shear.  Supports have to reach that floor, and every support
    // generator works in layers at z >= 0, so z = 0 has to be the lowest floor
    // point under the footprint.  The layers between it and the first vertex
    // come out empty, which belt slicing already tolerates (the bottom corner
    // of a tilted part is a point).  Vertices on the belt have z == floor, so
    // for a part resting on the belt this is simply the floor at its leading
    // extreme, less the frame margin (see BeltTransformPipeline::frame_margin).
    BeltTransformPipeline::BeltFloorParams floor;
    const bool has_floor = BeltTransformPipeline::floor_shear(config, floor);
    double min_z = std::numeric_limits<double>::max();
    for (const ModelVolume *mv : model_volumes) {
        if (!mv->is_model_part()) continue;
        Transform3d vol_trafo = trafo * mv->get_matrix();
        const auto &its = mv->mesh().its;
        for (const stl_vertex &v : its.vertices) {
            Vec3d vm = v.cast<double>();
            Vec3d pt = vol_trafo * vm;
            min_z = std::min(min_z, pt.z());
            if (has_floor)
                min_z = std::min(min_z, floor.shear_factor * (floor.from_axis == 0 ? pt.x() : pt.y()));
        }
    }
    if (has_floor && min_z != std::numeric_limits<double>::max())
        min_z -= BeltTransformPipeline::frame_margin(floor);
    const double z_shift_val = (min_z < 0. && min_z != std::numeric_limits<double>::max()) ? -min_z : 0.;
    if (z_shift_val > 0.) {
        Transform3d z_shift = Transform3d::Identity();
        z_shift.matrix()(2, 3) = z_shift_val;
        trafo = z_shift * trafo;
    }
    // out_belt_min_z is only meaningful in belt mode.
    if (out_belt_min_z && config.belt_printer.value) {
        *out_belt_min_z = (min_z != std::numeric_limits<double>::max()) ? min_z : 0.;
    }
}

} // namespace Slic3r
