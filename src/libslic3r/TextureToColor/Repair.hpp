#pragma once
#include "TriMesh.hpp"
#include "CgalUtils.hpp"
#include "Callbacks.hpp"
#include <CGAL/Polygon_mesh_processing/border.h>
#include <CGAL/Polygon_mesh_processing/manifoldness.h>
#include <CGAL/Polygon_mesh_processing/repair_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/Polygon_mesh_processing/stitch_borders.h>
#include <CGAL/Polygon_mesh_processing/triangulate_hole.h>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>

namespace Slic3r { namespace tex2color {

namespace PMP = CGAL::Polygon_mesh_processing;

// Default upper bound on the number of half-edges in any single boundary cycle
// that CloseBoundariesAndRepairManifoldness will attempt to triangulate. The
// cost of triangulate_hole grows non-linearly with cycle length, so this caps
// the worst-case per-hole work rather than the aggregate boundary size: a mesh
// with many small holes is still fully repaired, while a mesh containing one
// pathologically large hole skips triangulation entirely.
inline constexpr std::size_t MAX_REPAIRABLE_MESH_HOLE_EDGES = 500;

// Default upper bound on the aggregate number of boundary half-edges in the
// mesh (summed across every boundary cycle). When the total boundary length is
// excessive, even if each individual cycle is short, triangulating all of them
// usually indicates a severely fragmented input (e.g. heavily damaged scans)
// and rarely yields a usable result, so we skip hole closing entirely.
inline constexpr std::size_t MAX_REPAIRABLE_MESH_BOUNDARY_EDGES = 5000;

struct RepairSetting
{
    // Skip triangulating a boundary cycle whose half-edge count exceeds this.
    std::size_t max_hole_edges = MAX_REPAIRABLE_MESH_HOLE_EDGES;
    // Skip hole closing entirely when the total boundary half-edge count
    // (summed across all cycles) exceeds this.
    std::size_t max_boundary_edges = MAX_REPAIRABLE_MESH_BOUNDARY_EDGES;
};

struct BoundaryEdgeStats
{
    std::size_t total_boundary_edges = 0;
    std::size_t max_cycle_edges      = 0;
    std::size_t cycle_count          = 0;
};

// Read-only inspection of the mesh's boundary cycles. Caller is responsible for
// any pre-processing (e.g. stitch_borders) needed for the count to be meaningful.
inline BoundaryEdgeStats ComputeBoundaryEdgeStats(const cgalutils::CGALMesh& cgal_mesh)
{
    using CGALMesh = cgalutils::CGALMesh;
    using HalfedgeDescriptor = boost::graph_traits<CGALMesh>::halfedge_descriptor;

    std::vector<HalfedgeDescriptor> border_cycles;
    PMP::extract_boundary_cycles(cgal_mesh, std::back_inserter(border_cycles));

    BoundaryEdgeStats stats;
    stats.cycle_count = border_cycles.size();
    for (const HalfedgeDescriptor h0 : border_cycles) {
        std::size_t len = 0;
        HalfedgeDescriptor h = h0;
        do {
            ++len;
            h = next(h, cgal_mesh);
        } while (h != h0);
        stats.max_cycle_edges = std::max(stats.max_cycle_edges, len);
        stats.total_boundary_edges += len;
    }
    return stats;
}

// Unconditionally close every boundary cycle of the mesh and repair non-manifold
// vertices. The caller (e.g. RepairMesh) is expected to gate this call based on
// boundary statistics; entering this function always triggers triangulation.
inline void CloseBoundariesAndRepairManifoldness(cgalutils::CGALMesh& cgal_mesh)
{
    using CGALMesh = cgalutils::CGALMesh;
    using HalfedgeDescriptor = boost::graph_traits<CGALMesh>::halfedge_descriptor;
    using FaceDescriptor = boost::graph_traits<CGALMesh>::face_descriptor;

    PMP::stitch_borders(cgal_mesh);
    PMP::duplicate_non_manifold_vertices(cgal_mesh);

    std::vector<HalfedgeDescriptor> border_cycles;
    PMP::extract_boundary_cycles(cgal_mesh, std::back_inserter(border_cycles));

    for (const HalfedgeDescriptor h : border_cycles) {
        std::vector<FaceDescriptor> patch_faces;
        PMP::triangulate_hole(cgal_mesh, h, std::back_inserter(patch_faces));
    }

    PMP::remove_degenerate_faces(cgal_mesh);
    PMP::duplicate_non_manifold_vertices(cgal_mesh);
}

inline bool RepairMesh(const TriMesh& mesh,
                       std::shared_ptr<TriMesh>& out_mesh,
                       AlgoProgressCallback progress_callback = nullptr,
                       AlgoCancelCallback cancel_callback = nullptr,
                       const RepairSetting& setting = RepairSetting{})
{
    using Clock = std::chrono::steady_clock;
    auto elapsed_ms = [](Clock::time_point t0) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    };

    const Clock::time_point t_total = Clock::now();

    // Convert TriMesh to polygon soup (point container + triangle index container)
    std::vector<cgalutils::Kernel::Point_3> soup_points;
    std::vector<std::vector<std::size_t>> soup_triangles;

    soup_points.reserve(mesh.vertices.size());
    for (const TriVertex& v : mesh.vertices) {
        soup_points.emplace_back(v.x(), v.y(), v.z());
    }

    soup_triangles.reserve(mesh.indices.size());
    for (const TriFace& f : mesh.indices) {
        soup_triangles.push_back({static_cast<std::size_t>(f[0]),
                                  static_cast<std::size_t>(f[1]),
                                  static_cast<std::size_t>(f[2])});
    }

    if (progress_callback) {
        progress_callback({30, "Repairing polygon soup"});
    }
    if (cancel_callback && cancel_callback()) {
        return false;
    }

    {
        const auto t0 = Clock::now();
        PMP::repair_polygon_soup(soup_points, soup_triangles);
        BOOST_LOG_TRIVIAL(info) << "TextureToColor: RepairMesh stage=repair_polygon_soup took="
                                << elapsed_ms(t0) << " ms";
    }

    if (progress_callback) {
        progress_callback({50, "Orienting polygon soup"});
    }
    if (cancel_callback && cancel_callback()) {
        return false;
    }

    {
        const auto t0 = Clock::now();
        PMP::orient_polygon_soup(soup_points, soup_triangles);
        BOOST_LOG_TRIVIAL(info) << "TextureToColor: RepairMesh stage=orient_polygon_soup took="
                                << elapsed_ms(t0) << " ms";
    }

    if (progress_callback) {
        progress_callback({70, "Converting to CGAL mesh"});
    }
    if (cancel_callback && cancel_callback()) {
        return false;
    }

    cgalutils::CGALMesh cgal_mesh;
    {
        const auto t0 = Clock::now();
        PMP::polygon_soup_to_polygon_mesh(soup_points, soup_triangles, cgal_mesh);
        BOOST_LOG_TRIVIAL(info) << "TextureToColor: RepairMesh stage=polygon_soup_to_polygon_mesh took="
                                << elapsed_ms(t0) << " ms";
    }

    {
        const auto t0 = Clock::now();
        PMP::remove_degenerate_faces(cgal_mesh);
        BOOST_LOG_TRIVIAL(info) << "TextureToColor: RepairMesh stage=remove_degenerate_faces took="
                                << elapsed_ms(t0) << " ms";
    }

    if (progress_callback) {
        progress_callback({80, "Closing mesh boundaries"});
    }
    if (cancel_callback && cancel_callback()) {
        return false;
    }

    // Stitch borders and duplicate non-manifold vertices first so that the
    // boundary statistics below reflect the post-stitch topology; otherwise
    // boundaries that would close on stitching inflate the counts and may
    // cause the gate to skip hole filling unnecessarily.
    BoundaryEdgeStats stats;
    {
        const auto t0 = Clock::now();
        PMP::stitch_borders(cgal_mesh);
        PMP::duplicate_non_manifold_vertices(cgal_mesh);
        stats = ComputeBoundaryEdgeStats(cgal_mesh);
        BOOST_LOG_TRIVIAL(info) << "TextureToColor: RepairMesh stage=boundary_stats took="
                                << elapsed_ms(t0) << " ms"
                                << " total_boundary_edges=" << stats.total_boundary_edges
                                << " max_cycle_edges=" << stats.max_cycle_edges
                                << " cycle_count=" << stats.cycle_count;
    }

    const bool can_repair_holes =
        stats.total_boundary_edges <= setting.max_boundary_edges &&
        stats.max_cycle_edges      <= setting.max_hole_edges;

    if (can_repair_holes) {
        const auto t0 = Clock::now();
        CloseBoundariesAndRepairManifoldness(cgal_mesh);
        BOOST_LOG_TRIVIAL(info) << "TextureToColor: RepairMesh stage=close_boundaries took="
                                << elapsed_ms(t0) << " ms";
    } else {
        BOOST_LOG_TRIVIAL(info)
            << "TextureToColor: RepairMesh skip hole closing"
            << ", total_boundary_edges=" << stats.total_boundary_edges
            << " (limit=" << setting.max_boundary_edges << ")"
            << ", max_cycle_edges=" << stats.max_cycle_edges
            << " (limit=" << setting.max_hole_edges << ")"
            << ", cycle_count=" << stats.cycle_count;
    }

    if (progress_callback) {
        progress_callback({85, "Converting from CGAL mesh"});
    }
    if (cancel_callback && cancel_callback()) {
        return false;
    }

    std::shared_ptr<TriMesh> out;
    {
        const auto t0 = Clock::now();
        out = std::make_shared<TriMesh>(cgalutils::cgal_to_trimesh(cgal_mesh));
        BOOST_LOG_TRIVIAL(info) << "TextureToColor: RepairMesh stage=cgal_to_trimesh took="
                                << elapsed_ms(t0) << " ms";
    }

    out_mesh = std::move(out);
    if (progress_callback) {
        progress_callback({100, "Done"});
    }

    BOOST_LOG_TRIVIAL(info) << "TextureToColor: RepairMesh total=" << elapsed_ms(t_total) << " ms";

    return true;
}

} // namespace tex2color
} // namespace Slic3r
