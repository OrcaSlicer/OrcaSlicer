#include "ZoneInterior.hpp"

#include <libslic3r/OpenVDBUtils.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/SLA/Hollowing.hpp>
#include <libslic3r/Utils.hpp>

#include <openvdb/tools/LevelSetFilter.h>
#include <openvdb/tools/Composite.h>
#include <openvdb/tools/LevelSetUtil.h>   // sdfInteriorMask
#include <openvdb/tools/Morphology.h>      // dilateActiveValues
#include <openvdb/tools/Prune.h>           // pruneInactive

#include <boost/log/trivial.hpp>

#include <chrono>

namespace Slic3r {
namespace zone_boundary {

void smooth_interior(sla::Interior &interior, const TriangleMesh &original_mesh, int iterations)
{
    // Note: interior.mesh may be empty if filter_thin_interior() was called first
    // We only need the grid for smoothing - mesh is regenerated at the end
    if (!interior.gridptr || iterations <= 0)
        return;

    BOOST_LOG_TRIVIAL(info) << "Zone boundary: Applying constrained smoothing (" << iterations << " iterations)";

    // Create grid from original mesh at same voxel scale for clamping constraint.
    // The valid zone is the original mesh offset inward by shell thickness.
    float voxel_scale = float(interior.voxel_scale);
    float out_range = 3.0f;
    float in_range = float(interior.nb_in);

    auto original_grid = mesh_to_grid(original_mesh.its, {}, voxel_scale, out_range, in_range);
    if (!original_grid) {
        BOOST_LOG_TRIVIAL(warning) << "Zone boundary: Failed to create original mesh grid for smoothing constraint";
        return;
    }

    // The valid zone grid: original mesh offset inward by thickness.
    // In SDF terms, we add the thickness offset to shift the zero level-set inward.
    // valid_zone_grid represents the boundary that the interior must not cross.
    double thickness_offset = interior.thickness;
    auto valid_zone_grid = redistance_grid(*original_grid, -thickness_offset, in_range, in_range);
    if (!valid_zone_grid) {
        BOOST_LOG_TRIVIAL(warning) << "Zone boundary: Failed to create valid zone grid";
        return;
    }

    // Apply constrained mean curvature smoothing iterations.
    // Each iteration: multiple smooth passes, then clamp to valid zone.
    // Batching smooth passes before clamping is more effective because:
    // - Mean curvature shrinks bumps INWARD (convex -> shrink)
    // - Clamping only prevents OUTWARD expansion (into shell zone)
    // - Clamping does NOT block bump removal
    // - Batching = more effective smoothing per CSG operation
    //
    // We use csgIntersectionCopy which:
    // - Takes const references (doesn't destroy valid_zone_grid)
    // - Maintains proper level set semantics (signedFloodFill, narrow band)
    // - Is internally parallelized with TBB

    const int smooth_passes_per_clamp = 5;  // Batch 5 smooth passes before clamping

    // Convergence detection via L1 energy (sum of |SDF| over all active voxels).
    // This is an O(N) read-only pass — trivial overhead vs the O(N) smoothing passes
    // with heavy per-voxel computation. Covers ALL voxels, so sharp features that
    // affect few voxels but change significantly are still detected proportionally.
    auto compute_l1_energy = [&]() -> double {
        double energy = 0;
        for (auto iter = interior.gridptr->cbeginValueOn(); iter; ++iter)
            energy += std::abs(iter.getValue());
        return energy;
    };

    double prev_energy = compute_l1_energy();
    double prev_rel_change = 0;

    BOOST_LOG_TRIVIAL(info) << "Zone boundary: Smoothing " << interior.gridptr->activeVoxelCount()
        << " active voxels, " << iterations << " max iterations x " << smooth_passes_per_clamp << " passes";

    auto t_smooth_start = std::chrono::high_resolution_clock::now();

    int actual_iterations = 0;
    for (int i = 0; i < iterations; ++i) {
        auto t_iter = std::chrono::high_resolution_clock::now();

        openvdb::tools::LevelSetFilter<openvdb::FloatGrid> filter(*interior.gridptr);

        // Apply multiple smoothing passes before clamping
        for (int j = 0; j < smooth_passes_per_clamp; ++j) {
            filter.meanCurvature();
        }

        // Clamp to valid zone using csgIntersectionCopy (preserves both inputs)
        interior.gridptr = openvdb::tools::csgIntersectionCopy(*interior.gridptr, *valid_zone_grid);

        ++actual_iterations;

        // Convergence check: relative change in L1 energy
        double curr_energy = compute_l1_energy();
        double rel_change = (prev_energy > 0) ? std::abs(curr_energy - prev_energy) / prev_energy : 0;

        auto iter_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - t_iter).count();

        BOOST_LOG_TRIVIAL(debug) << "Zone boundary: Smooth iter " << (i + 1) << "/" << iterations
            << " " << iter_ms << "ms, L1=" << curr_energy << ", rel_change=" << rel_change;

        // Plateau detection: if rel_change stopped decreasing, the smooth/clamp
        // cycle has reached equilibrium. No fixed threshold — adapts to the model.
        // Skip iter 0 (first iteration is always a large initial change).
        if (i > 0 && prev_rel_change > 0 && rel_change >= 0.5 * prev_rel_change) {
            BOOST_LOG_TRIVIAL(info) << "Zone boundary: Smooth plateaued at iteration " << (i + 1)
                << "/" << iterations << " (rel_change=" << rel_change << ")";
            break;
        }

        prev_rel_change = rel_change;
        prev_energy = curr_energy;
    }

    auto smooth_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - t_smooth_start).count();
    BOOST_LOG_TRIVIAL(info) << "Zone boundary: Smooth complete: " << actual_iterations
        << "/" << iterations << " iterations in " << smooth_ms << "ms";

    // Convert smoothed grid back to mesh
    double adaptivity = 0.;
    interior.mesh = grid_to_mesh(*interior.gridptr, 0.0, adaptivity);

    if (!interior.mesh.empty())
        sla::postprocess_interior_mesh(interior.mesh);

    BOOST_LOG_TRIVIAL(info) << "Zone boundary: Constrained smoothing complete";
}

void filter_thin_interior(sla::Interior &interior, double min_width)
{
    if (!interior.gridptr || min_width <= 0)
        return;

    BOOST_LOG_TRIVIAL(info) << "Zone boundary: Filtering thin inner zone sections (min width: " << min_width << "mm)";

    // Convert min_width to voxel scale
    // The threshold is half the min_width (radius of inscribed sphere)
    float threshold = float(min_width / 2.0 * interior.voxel_scale);
    int threshold_voxels = int(std::ceil(threshold));

    // 1. Extract mask of "thick core" - voxels where inscribed sphere radius >= threshold
    //    sdfInteriorMask returns mask where SDF <= isovalue
    //    Negative threshold = voxels at least threshold from boundary
    auto thick_mask = openvdb::tools::sdfInteriorMask(*interior.gridptr, -threshold);

    if (!thick_mask || thick_mask->tree().activeVoxelCount() == 0) {
        // No thick regions survive - clear the interior
        BOOST_LOG_TRIVIAL(warning) << "Zone boundary: No regions thick enough to survive filtering (min_width=" << min_width << "mm)";
        interior.gridptr->clear();
        interior.mesh.clear();
        return;
    }

    // 2. Dilate mask back to reach original surface (BINARY dilation = EXACT)
    openvdb::tools::dilateActiveValues(thick_mask->tree(), threshold_voxels,
                                        openvdb::tools::NN_FACE_EDGE_VERTEX,
                                        openvdb::tools::PRESERVE_TILES);

    // 3. Use mask to carve original level set
    //    Keep original SDF where mask is active, set to background (outside) elsewhere
    float background = interior.gridptr->background();
    size_t removed_count = 0;
    size_t kept_count = 0;

    for (auto iter = interior.gridptr->beginValueOn(); iter; ++iter) {
        openvdb::Coord coord = iter.getCoord();
        if (!thick_mask->tree().isValueOn(coord)) {
            iter.setValue(background);  // Set to outside
            removed_count++;
        } else {
            kept_count++;
        }
    }

    // Prune inactive voxels
    openvdb::tools::pruneInactive(interior.gridptr->tree());

    // Clear stale mesh - smooth_interior() will regenerate it
    interior.mesh.clear();

    BOOST_LOG_TRIVIAL(info) << "Zone boundary: Thin inner zone filtering complete (kept " << kept_count << " voxels)";
}

bool debug_export_interior(const sla::Interior &interior, const std::string &stage_name, int object_id)
{
    if (interior.mesh.empty()) {
        BOOST_LOG_TRIVIAL(debug) << "Zone boundary debug: Skipping export of empty mesh for stage " << stage_name;
        return false;
    }

    std::string filename = debug_out_path("zone_shell_obj%d_%s.stl", object_id, stage_name.c_str());

    bool success = its_write_stl_ascii(filename.c_str(), "zone_interior", interior.mesh);

    if (success) {
        BOOST_LOG_TRIVIAL(info) << "Zone boundary debug: Exported interior mesh to " << filename
                                << " (" << interior.mesh.indices.size() << " triangles)";
    } else {
        BOOST_LOG_TRIVIAL(error) << "Zone boundary debug: Failed to export interior mesh to " << filename;
    }

    return success;
}

} // namespace zone_boundary
} // namespace Slic3r
