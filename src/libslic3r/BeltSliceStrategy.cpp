#include "BeltSliceStrategy.hpp"
#include "Model.hpp"

#include <limits>

#include <boost/log/trivial.hpp>
#ifdef SLIC3R_BELT_DIAGNOSTIC_LOG
#include <iomanip>
#include <sstream>
#include <thread>
#endif

namespace Slic3r {

void BeltSliceStrategy::apply_preslice_transforms(Transform3d           &trafo,
                                                  const PrintConfig     &config,
                                                  const ModelVolumePtrs &model_volumes,
                                                  double                *out_belt_min_z)
{
    // 1. Standalone pre-slice axis remap (works without belt mode).
    const bool has_remap = BeltTransformPipeline::has_preslice_remap(config);
    if (has_remap)
        trafo = BeltTransformPipeline::build_preslice_remap(config) * trafo;

    // 2. Belt rotation — the sole mesh-side belt transform (matching
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

    if (!has_remap && !has_rotation)
        return;

    // 3. Z-shift — detect if the mesh clips below the build plate after the
    // transforms and lift it.  Each mesh vertex must be brought into object space
    // via mv->get_matrix() before applying the full trafo (which is in object
    // space).  Missing this on assemblies (where per-volume get_matrix() positions
    // each volume within the object) would compute min_z against mesh-local vertex
    // coordinates rather than object-space coordinates, so volumes translated along
    // the slicer's Z axis would be silently excluded from the bound check.
#ifdef SLIC3R_BELT_DIAGNOSTIC_LOG
    // Capture the incoming trafo for diagnostic logging.
    // This is the slicer-frame transform AFTER remap + rotation but BEFORE z_shift.
    const Transform3d trafo_pre_shift = trafo;
    auto log_mat = [](const Matrix3d &m) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(4);
        ss << "[[" << m(0,0) << "," << m(0,1) << "," << m(0,2) << "],"
           << "["  << m(1,0) << "," << m(1,1) << "," << m(1,2) << "],"
           << "["  << m(2,0) << "," << m(2,1) << "," << m(2,2) << "]]";
        return ss.str();
    };
    auto log_vec3 = [](const Vec3d &v) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(4);
        ss << "(" << v.x() << "," << v.y() << "," << v.z() << ")";
        return ss.str();
    };
    BOOST_LOG_TRIVIAL(trace) << "[BELT-DEBUG] apply_preslice_transforms enter"
        << " has_rotation=" << has_rotation
        << " has_remap=" << has_remap
        << " trafo.linear=" << log_mat(trafo_pre_shift.linear())
        << " trafo.translation=" << log_vec3(trafo_pre_shift.translation())
        << " volumes=" << model_volumes.size();
#endif

    double min_z = std::numeric_limits<double>::max();
#ifdef SLIC3R_BELT_DIAGNOSTIC_LOG
    int vol_idx = 0;
#endif
    for (const ModelVolume *mv : model_volumes) {
#ifdef SLIC3R_BELT_DIAGNOSTIC_LOG
        if (!mv->is_model_part()) { ++vol_idx; continue; }
#else
        if (!mv->is_model_part()) continue;
#endif
        Transform3d vol_trafo = trafo * mv->get_matrix();
        const auto &its = mv->mesh().its;
#ifdef SLIC3R_BELT_DIAGNOSTIC_LOG
        // Per-volume bbox in mesh-frame and post-trafo slicer-frame.
        Vec3d mesh_min(std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
        Vec3d mesh_max(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest());
        Vec3d slicer_min(std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
        Vec3d slicer_max(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest());
        double vol_min_z = std::numeric_limits<double>::max();
#endif
        for (const stl_vertex &v : its.vertices) {
            Vec3d vm = v.cast<double>();
            Vec3d pt = vol_trafo * vm;
            min_z = std::min(min_z, pt.z());
#ifdef SLIC3R_BELT_DIAGNOSTIC_LOG
            mesh_min = mesh_min.cwiseMin(vm);
            mesh_max = mesh_max.cwiseMax(vm);
            slicer_min = slicer_min.cwiseMin(pt);
            slicer_max = slicer_max.cwiseMax(pt);
            vol_min_z = std::min(vol_min_z, pt.z());
#endif
        }
#ifdef SLIC3R_BELT_DIAGNOSTIC_LOG
        BOOST_LOG_TRIVIAL(trace) << "[BELT-DEBUG]   vol[" << vol_idx
            << "] id=" << mv->id().id << " name='" << mv->name << "'"
            << " mesh_bbox_min=" << log_vec3(mesh_min) << " mesh_bbox_max=" << log_vec3(mesh_max)
            << " get_matrix.translation=" << log_vec3(mv->get_matrix().translation())
            << " slicer_bbox_min=" << log_vec3(slicer_min) << " slicer_bbox_max=" << log_vec3(slicer_max)
            << " vol_min_z=" << vol_min_z;
        ++vol_idx;
#endif
    }
    const double z_shift_val = (min_z < 0. && min_z != std::numeric_limits<double>::max()) ? -min_z : 0.;
#ifdef SLIC3R_BELT_DIAGNOSTIC_LOG
    BOOST_LOG_TRIVIAL(trace) << "[BELT-DEBUG] combined min_z=" << min_z
        << " z_shift_val=" << z_shift_val;
#endif
    if (z_shift_val > 0.) {
        Transform3d z_shift = Transform3d::Identity();
        z_shift.matrix()(2, 3) = z_shift_val;
        trafo = z_shift * trafo;
    }
    // out_belt_min_z is only meaningful in belt mode; the standalone-remap path
    // never reported it.
    if (out_belt_min_z && config.belt_printer.value) {
        const double new_val = (min_z != std::numeric_limits<double>::max()) ? min_z : 0.;
#ifdef SLIC3R_BELT_DIAGNOSTIC_LOG
        BOOST_LOG_TRIVIAL(trace) << "[BELT-DEBUG] write m_belt_min_z tid=" << std::this_thread::get_id()
            << " target=" << out_belt_min_z << " old=" << *out_belt_min_z << " new=" << new_val;
#endif
        *out_belt_min_z = new_val;
    }
#ifdef SLIC3R_BELT_DIAGNOSTIC_LOG
    BOOST_LOG_TRIVIAL(trace) << "[BELT-DEBUG] apply_preslice_transforms exit"
        << " final_trafo.linear=" << log_mat(trafo.linear())
        << " final_trafo.translation=" << log_vec3(trafo.translation());
#endif
}

} // namespace Slic3r
