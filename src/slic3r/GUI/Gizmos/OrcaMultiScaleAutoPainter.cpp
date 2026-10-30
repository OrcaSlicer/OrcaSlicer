// ============================================================================
// File: src/slic3r/GUI/Gizmos/OrcaMultiScaleAutoPainter.cpp
// Target Repository: https://github.com/OrcaSlicer/OrcaSlicer
//
// Description:
//   Multi-Scale Low-Pass Edge Transition Segmentation + Gemini Cloud AI
//   Auto-Painter for OrcaSlicer's Multi-Material Painting Gizmo
//   (GLGizmoMmuSegmentation / TriangleSelector).
//
// Core Algorithm:
//   1. Low-Resolution Geometric Proxy (Taubin Scale-Space Mesh Pyramid):
//      Constructs a low-resolution view of the 3D mesh via volume-preserving
//      Taubin vertex filtering (attenuating high-frequency surface noise and
//      layer/scan ripples while preserving macro structural raises).
//   2. Directionally-Normalized Persistent Edge Transition Detector:
//      Measures dihedral angles on both the fine mesh and the low-resolution
//      proxy, subtracting the directional background curvature (median angle
//      of parallel edges in adjacent hops) so faceted cylinders/spheres remain
//      unified while real step raises (guards, wraps, collars, embossed trim)
//      stand out sharply regardless of mesh tessellation density.
//   3. Global + Per-Section Sensitivity Map:
//      Default sensitivity (0.35) only splits regions at significant edge
//      transitions that persist in the low-resolution view. Users can increase
//      global sensitivity or paint a local sensitivity boost onto specific
//      sections to capture finer details locally.
//   4. Watershed Region Growing + Boundary Smoothing:
//      Floods from smooth interior basins up to persistent edge transition
//      barriers, absorbs tiny specks, and smooths jagged triangle boundaries.
//   5. Gemini Multimodal API Color Assignment:
//      Queries Gemini (gemini-2.5-flash) with the segmented region descriptors,
//      multi-view viewport snapshots, and loaded AMS filament hex colors to
//      assign each region to the best extruder slot via TriangleSelector.
// ============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef ORCASLICER_BUILD
// Native OrcaSlicer headers when compiled inside src/slic3r/GUI/Gizmos/
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoMmuSegmentation.hpp"
#include <imgui/imgui.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#endif

namespace Slic3r {
namespace GUI {

// ============================================================================
// 1. Minimal Math & Mesh Primitives (Bridges Eigen / indexed_triangle_set)
// ============================================================================

struct Vec3f {
    float x{0.0f}, y{0.0f}, z{0.0f};

    Vec3f() = default;
    Vec3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3f operator+(const Vec3f& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3f operator-(const Vec3f& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3f operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3f& operator+=(const Vec3f& o) { x += o.x; y += o.y; z += o.z; return *this; }

    float dot(const Vec3f& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3f cross(const Vec3f& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float norm() const { return std::sqrt(std::max(0.0f, dot(*this))); }
    Vec3f normalized() const {
        float n = norm();
        return (n > 1e-8f) ? (*this * (1.0f / n)) : Vec3f{0.0f, 0.0f, 1.0f};
    }
};

struct IndexedTriangle {
    int v0{0}, v1{0}, v2{0};
};

// Filament slot info from OrcaSlicer's AMS / Extruder configuration
struct FilamentSlot {
    int extruder_id{1};        // 1-based extruder index in OrcaSlicer
    std::string name;          // e.g. "Tan Wood PLA", "Dark Teal PLA"
    std::string hex_color;     // e.g. "#D8A468"
};

// Segmented 3D surface region produced by the multi-scale edge detector
struct SegmentedRegion {
    int region_id{0};
    std::vector<int> triangle_indices;
    float total_area_mm2{0.0f};
    Vec3f centroid{0.0f, 0.0f, 0.0f};
    Vec3f mean_normal{0.0f, 0.0f, 1.0f};
    float z_min_mm{0.0f};
    float z_max_mm{0.0f};
    float mean_radial_distance_mm{0.0f};
    float mean_relief_step_deg{0.0f};  // Boundary transition prominence in degrees
    std::vector<int> neighbor_region_ids;
    int assigned_extruder_id{1};
};

// Configuration for Multi-Scale Low-Pass Edge Transition Segmentation
struct MultiScaleConfig {
    // Default 0.35 = Only significant edge raises that persist in the low-resolution
    // view act as color boundaries. Increase toward 1.0 for finer details.
    float global_sensitivity{0.35f};

    // Number of volume-preserving Taubin vertex smoothing passes used to build
    // the "low-resolution view" of the mesh.
    int lowres_smoothing_passes{5};

    // Taubin low-pass filter coefficients (attenuates high-frequency ripples without shrinking)
    float lambda_smooth{0.40f};
    float mu_inflate{-0.42f};

    // Default edge transition threshold (degrees of curvature-corrected transition
    // across a step/raise) when sensitivity == 0.35.
    float default_transition_angle_deg{22.0f};

    // Extra boost applied to concave-to-convex step raises (where a feature rises
    // out of a base surface, like armor trim, grip wraps, or embossed lettering).
    float step_raise_boost{1.35f};

    // Minimum region area (as fraction of total mesh surface area) before speck merging
    float min_region_area_fraction{0.003f};

    // Boundary smoothing passes (removes single-triangle stair-stepped jaggies)
    int boundary_smoothing_passes{3};
};

// ============================================================================
// 2. Multi-Scale Edge Transition Segmenter
// ============================================================================

class MultiScaleEdgeSegmenter {
public:
    MultiScaleEdgeSegmenter(
        const std::vector<Vec3f>& vertices,
        const std::vector<IndexedTriangle>& triangles)
        : m_vertices(vertices), m_triangles(triangles)
    {
        build_topology_and_geometry();
        m_local_sensitivity_boost.assign(m_triangles.size(), 0.0f);
    }

    void clear_local_sensitivity() {
        std::fill(m_local_sensitivity_boost.begin(), m_local_sensitivity_boost.end(), 0.0f);
    }

    // Paint local sensitivity boost inside a 3D brush sphere (for finer detail in specific sections)
    void paint_local_sensitivity_sphere(const Vec3f& center, float radius, float delta_sensitivity) {
        const float r2 = radius * radius;
        for (size_t i = 0; i < m_triangles.size(); ++i) {
            Vec3f diff = m_centroids[i] - center;
            float d2 = diff.dot(diff);
            if (d2 <= r2) {
                float falloff = 1.0f - std::sqrt(d2) / std::max(radius, 1e-5f);
                float updated = m_local_sensitivity_boost[i] + delta_sensitivity * falloff;
                m_local_sensitivity_boost[i] = std::clamp(updated, -0.35f, 0.65f);
            }
        }
    }

    // Boost sensitivity inside an axis-aligned 3D bounding box section
    void set_section_sensitivity_box(const Vec3f& bmin, const Vec3f& bmax, float sensitivity_boost) {
        for (size_t i = 0; i < m_triangles.size(); ++i) {
            const Vec3f& c = m_centroids[i];
            if (c.x >= bmin.x && c.x <= bmax.x &&
                c.y >= bmin.y && c.y <= bmax.y &&
                c.z >= bmin.z && c.z <= bmax.z) {
                m_local_sensitivity_boost[i] = std::clamp(sensitivity_boost, -0.35f, 0.65f);
            }
        }
    }

    // Execute the multi-scale low-pass edge transition segmentation
    std::vector<SegmentedRegion> segment(const MultiScaleConfig& cfg) {
        const size_t num_tris = m_triangles.size();
        if (num_tris == 0) return {};

        // Step 1: Construct the Low-Resolution View of the mesh by applying
        // volume-preserving Taubin low-pass filtering to the vertex positions,
        // then recomputing face normals on the low-res geometry.
        std::vector<Vec3f> lowres_verts = compute_taubin_smoothed_vertices(
            cfg.lowres_smoothing_passes, cfg.lambda_smooth, cfg.mu_inflate);
        std::vector<Vec3f> lowres_normals = compute_face_normals(lowres_verts);

        // Use raw normals for fine detail detection
        std::vector<Vec3f> fine_normals = m_raw_normals;

        // Precompute raw dihedral angles on both scales
        std::vector<std::array<float, 3>> fine_dihedral(num_tris, {0.0f, 0.0f, 0.0f});
        std::vector<std::array<float, 3>> lowres_dihedral(num_tris, {0.0f, 0.0f, 0.0f});
        for (size_t i = 0; i < num_tris; ++i) {
            for (int e = 0; e < 3; ++e) {
                int j = m_neighbors[i][e];
                if (j < 0) continue;
                fine_dihedral[i][e] = angle_deg(fine_normals[i], fine_normals[j]);
                lowres_dihedral[i][e] = angle_deg(lowres_normals[i], lowres_normals[j]);
            }
        }

        // Step 2: Score every triangle edge for Persistent Raised Edge Transition Saliency
        std::vector<std::array<float, 3>> edge_saliency(num_tris, {0.0f, 0.0f, 0.0f});
        std::vector<float> tri_max_saliency(num_tris, 0.0f);

        for (size_t i = 0; i < num_tris; ++i) {
            for (int e = 0; e < 3; ++e) {
                int j = m_neighbors[i][e];
                if (j < 0) continue;

                // Estimate directional background curvature along parallel edges in adjacent faces.
                float dir_bg_fine = directional_background_angle(static_cast<int>(i), e, fine_dihedral);
                float dir_bg_lowres = directional_background_angle(static_cast<int>(i), e, lowres_dihedral);

                float net_fine_deg = std::max(0.0f, fine_dihedral[i][e] - dir_bg_fine);
                float net_lowres_1hop = std::max(0.0f, lowres_dihedral[i][e] - dir_bg_lowres);

                int opp_i = opposite_neighbor_across_edge(static_cast<int>(i), e);
                int opp_j = opposite_neighbor_from_target(static_cast<int>(i), j);
                float raw_lowres_2hop = angle_deg(lowres_normals[opp_i], lowres_normals[opp_j]);
                float net_lowres_2hop = std::min(1.8f * std::max(net_fine_deg, net_lowres_1hop), std::max(0.0f, 0.55f * (raw_lowres_2hop - 2.2f * dir_bg_lowres)));

                float persistent_lowres_deg = std::max(net_lowres_1hop, net_lowres_2hop);

                // Step raise boost when an edge exhibits a sharp fine dihedral transition
                bool is_step_raise = (net_fine_deg > 10.0f && persistent_lowres_deg > 4.0f);
                float raise_multiplier = is_step_raise ? cfg.step_raise_boost : 1.0f;

                // Effective sensitivity at this edge (Global Slider + Local Section Boost)
                float local_boost = 0.5f * (m_local_sensitivity_boost[i] + m_local_sensitivity_boost[j]);
                float eff_sens = std::clamp(cfg.global_sensitivity + local_boost, 0.05f, 1.0f);

                // Blend between pure low-res persistence (at default sens = 0.35)
                // and fine-scale detail detection (as sensitivity -> 1.0)
                float fine_blend = std::clamp((eff_sens - 0.35f) / 0.55f, 0.0f, 1.0f);
                float effective_transition_deg =
                    ((1.0f - 0.55f * fine_blend) * persistent_lowres_deg +
                     (0.85f * fine_blend) * net_fine_deg) * raise_multiplier;

                // Threshold scales down smoothly as sensitivity increases
                float relative_sens = (eff_sens - 0.35f);
                float threshold_deg = cfg.default_transition_angle_deg * std::exp(-2.1f * relative_sens);
                threshold_deg = std::clamp(threshold_deg, 4.0f, 50.0f);

                float saliency = effective_transition_deg / threshold_deg;
                edge_saliency[i][e] = saliency;
                tri_max_saliency[i] = std::max(tri_max_saliency[i], saliency);
            }
        }

        // Step 4: Watershed Flood-Fill from Smooth Interior Basins
        std::vector<int> tri_order(num_tris);
        std::iota(tri_order.begin(), tri_order.end(), 0);
        std::sort(tri_order.begin(), tri_order.end(), [&](int a, int b) {
            return tri_max_saliency[a] < tri_max_saliency[b];
        });

        std::vector<int> labels(num_tris, -1);
        int current_label = 0;

        for (int seed : tri_order) {
            if (labels[seed] != -1) continue;
            if (tri_max_saliency[seed] >= 1.0f) continue;

            std::queue<int> q;
            q.push(seed);
            labels[seed] = current_label;

            while (!q.empty()) {
                int u = q.front();
                q.pop();

                for (int e = 0; e < 3; ++e) {
                    int v = m_neighbors[u][e];
                    if (v < 0 || labels[v] != -1) continue;

                    if (edge_saliency[u][e] >= 1.0f) continue;
                    if (tri_max_saliency[v] >= 1.0f && edge_saliency[u][e] >= 0.50f) continue;

                    labels[v] = current_label;
                    q.push(v);
                }
            }
            ++current_label;
        }

        // Step 5: Assign bevel/ridge transition triangles to their nearest basin
        assign_unlabeled_ridge_triangles(labels, edge_saliency, current_label);

        // Step 6: Absorb tiny speck islands below minimum area threshold
        merge_small_islands(labels, edge_saliency, cfg);

        // Step 7: Smooth region boundaries
        for (int pass = 0; pass < cfg.boundary_smoothing_passes; ++pass) {
            smooth_region_boundaries(labels, edge_saliency);
        }

        return build_region_descriptors(labels, fine_normals, edge_saliency);
    }

private:
    const std::vector<Vec3f>& m_vertices;
    const std::vector<IndexedTriangle>& m_triangles;

    std::vector<Vec3f> m_centroids;
    std::vector<Vec3f> m_raw_normals;
    std::vector<float> m_areas;
    std::vector<std::array<int, 3>> m_neighbors;
    std::vector<std::array<Vec3f, 3>> m_edge_dirs;
    std::vector<std::vector<int>> m_vertex_adj;
    std::vector<float> m_local_sensitivity_boost;
    float m_total_area{0.0f};
    Vec3f m_bbox_min{0.0f, 0.0f, 0.0f};
    Vec3f m_bbox_max{0.0f, 0.0f, 0.0f};

    struct UndirectedEdge {
        int a, b;
        bool operator==(const UndirectedEdge& o) const { return a == o.a && b == o.b; }
    };
    struct EdgeHash {
        size_t operator()(const UndirectedEdge& e) const {
            return (static_cast<size_t>(e.a) << 32) ^ static_cast<size_t>(e.b);
        }
    };

    static float angle_deg(const Vec3f& a, const Vec3f& b) {
        float d = std::clamp(a.dot(b), -1.0f, 1.0f);
        return std::acos(d) * (180.0f / 3.14159265f);
    }

    void build_topology_and_geometry() {
        const size_t n = m_triangles.size();
        m_centroids.resize(n);
        m_raw_normals.resize(n);
        m_areas.resize(n);
        m_neighbors.assign(n, {-1, -1, -1});
        m_edge_dirs.resize(n);
        m_vertex_adj.assign(m_vertices.size(), {});
        m_total_area = 0.0f;

        if (!m_vertices.empty()) {
            m_bbox_min = m_vertices.front();
            m_bbox_max = m_vertices.front();
            for (const auto& v : m_vertices) {
                m_bbox_min.x = std::min(m_bbox_min.x, v.x);
                m_bbox_min.y = std::min(m_bbox_min.y, v.y);
                m_bbox_min.z = std::min(m_bbox_min.z, v.z);
                m_bbox_max.x = std::max(m_bbox_max.x, v.x);
                m_bbox_max.y = std::max(m_bbox_max.y, v.y);
                m_bbox_max.z = std::max(m_bbox_max.z, v.z);
            }
        }

        std::unordered_map<UndirectedEdge, std::pair<int, int>, EdgeHash> edge_map;
        edge_map.reserve(n * 3);

        for (size_t i = 0; i < n; ++i) {
            const auto& t = m_triangles[i];
            const Vec3f& p0 = m_vertices[t.v0];
            const Vec3f& p1 = m_vertices[t.v1];
            const Vec3f& p2 = m_vertices[t.v2];

            m_centroids[i] = (p0 + p1 + p2) * (1.0f / 3.0f);
            Vec3f cr = (p1 - p0).cross(p2 - p0);
            m_areas[i] = 0.5f * cr.norm();
            m_total_area += m_areas[i];
            m_raw_normals[i] = cr.normalized();

            const int verts[3] = {t.v0, t.v1, t.v2};
            for (int e = 0; e < 3; ++e) {
                int va = verts[e];
                int vb = verts[(e + 1) % 3];
                m_edge_dirs[i][e] = (m_vertices[vb] - m_vertices[va]).normalized();
                m_vertex_adj[va].push_back(vb);
                m_vertex_adj[vb].push_back(va);

                UndirectedEdge ue{std::min(va, vb), std::max(va, vb)};
                auto it = edge_map.find(ue);
                if (it == edge_map.end()) {
                    edge_map.emplace(ue, std::make_pair(static_cast<int>(i), e));
                } else {
                    int other_tri = it->second.first;
                    int other_edge = it->second.second;
                    m_neighbors[i][e] = other_tri;
                    m_neighbors[other_tri][other_edge] = static_cast<int>(i);
                }
            }
        }

        for (auto& adj : m_vertex_adj) {
            std::sort(adj.begin(), adj.end());
            adj.erase(std::unique(adj.begin(), adj.end()), adj.end());
        }
    }

    std::vector<Vec3f> compute_taubin_smoothed_vertices(
        int passes, float lambda, float mu) const
    {
        std::vector<Vec3f> current = m_vertices;
        std::vector<Vec3f> next(current.size());

        auto laplacian_step = [&](float weight) {
            for (size_t v = 0; v < current.size(); ++v) {
                const auto& nb_list = m_vertex_adj[v];
                if (nb_list.empty()) {
                    next[v] = current[v];
                    continue;
                }
                Vec3f avg{0.0f, 0.0f, 0.0f};
                for (int nb : nb_list) {
                    avg += current[nb];
                }
                avg = avg * (1.0f / static_cast<float>(nb_list.size()));
                next[v] = current[v] + (avg - current[v]) * weight;
            }
            current.swap(next);
        };

        for (int p = 0; p < passes; ++p) {
            laplacian_step(lambda);
            laplacian_step(mu);
        }
        return current;
    }

    std::vector<Vec3f> compute_face_normals(const std::vector<Vec3f>& verts) const {
        std::vector<Vec3f> normals(m_triangles.size());
        for (size_t i = 0; i < m_triangles.size(); ++i) {
            const auto& t = m_triangles[i];
            Vec3f cr = (verts[t.v1] - verts[t.v0]).cross(verts[t.v2] - verts[t.v0]);
            normals[i] = cr.normalized();
        }
        return normals;
    }

    // Finds the background tessellation dihedral angle for edges running in the
    // same direction as edge (tri, edge_idx) across neighboring triangles.
    float directional_background_angle(
        int tri,
        int edge_idx,
        const std::vector<std::array<float, 3>>& dihedral_table) const
    {
        const Vec3f& dir = m_edge_dirs[tri][edge_idx];
        int partner_tri = m_neighbors[tri][edge_idx];
        std::vector<float> parallel_angles;
        parallel_angles.reserve(12);

        for (int e = 0; e < 3; ++e) {
            int nb1 = m_neighbors[tri][e];
            if (nb1 < 0) continue;

            // Check edges of 1-hop neighbor nb1
            for (int ne1 = 0; ne1 < 3; ++ne1) {
                int nb1_partner = m_neighbors[nb1][ne1];
                if ((nb1 == tri && nb1_partner == partner_tri) ||
                    (nb1 == partner_tri && nb1_partner == tri)) {
                    continue; // Skip the exact same undirected edge
                }
                if (std::fabs(dir.dot(m_edge_dirs[nb1][ne1])) > 0.90f) {
                    parallel_angles.push_back(dihedral_table[nb1][ne1]);
                }
            }

            // Check edges of 2-hop neighbors nb2
            for (int ne = 0; ne < 3; ++ne) {
                int nb2 = m_neighbors[nb1][ne];
                if (nb2 < 0 || nb2 == tri || nb2 == partner_tri) continue;
                for (int ne2 = 0; ne2 < 3; ++ne2) {
                    if (std::fabs(dir.dot(m_edge_dirs[nb2][ne2])) > 0.90f) {
                        parallel_angles.push_back(dihedral_table[nb2][ne2]);
                    }
                }
            }
        }

        if (parallel_angles.empty()) {
            return 0.0f;
        }
        std::sort(parallel_angles.begin(), parallel_angles.end());
        // Lower-quartile parallel angle:
        // - On a cylinder's longitudinal or diagonal edges, ALL parallel edges around the
        //   circumference have the same facet angle (~15 deg), so Q1 = 15 deg.
        // - At a latitudinal step edge, at least half of the parallel ring edges lie on the
        //   flat cylinder (~0 deg), so Q1 = 0 deg and the step's full angle is preserved.
        float q1_angle = parallel_angles[parallel_angles.size() / 4];
        return (q1_angle <= 20.0f) ? q1_angle : 0.0f;
    }

    int opposite_neighbor_across_edge(int tri, int edge_idx) const {
        int best = tri;
        float best_area = -1.0f;
        for (int e = 0; e < 3; ++e) {
            if (e == edge_idx) continue;
            int nb = m_neighbors[tri][e];
            if (nb >= 0 && m_areas[nb] > best_area) {
                best_area = m_areas[nb];
                best = nb;
            }
        }
        return best;
    }

    int opposite_neighbor_from_target(int source_tri, int target_tri) const {
        int best = target_tri;
        float best_dist = -1.0f;
        for (int e = 0; e < 3; ++e) {
            int nb = m_neighbors[target_tri][e];
            if (nb >= 0 && nb != source_tri) {
                float d = (m_centroids[nb] - m_centroids[source_tri]).norm();
                if (d > best_dist) {
                    best_dist = d;
                    best = nb;
                }
            }
        }
        return best;
    }

    void assign_unlabeled_ridge_triangles(
        std::vector<int>& labels,
        const std::vector<std::array<float, 3>>& edge_saliency,
        int& current_label) const
    {
        struct QueueItem {
            float cost;
            int tri;
            int label;
            bool operator>(const QueueItem& o) const { return cost > o.cost; }
        };
        std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> pq;

        for (size_t i = 0; i < labels.size(); ++i) {
            if (labels[i] == -1) continue;
            for (int e = 0; e < 3; ++e) {
                int nb = m_neighbors[i][e];
                if (nb >= 0 && labels[nb] == -1) {
                    pq.push({edge_saliency[i][e], nb, labels[i]});
                }
            }
        }

        while (!pq.empty()) {
            auto top = pq.top();
            pq.pop();
            if (labels[top.tri] != -1) continue;
            labels[top.tri] = top.label;
            for (int e = 0; e < 3; ++e) {
                int nb = m_neighbors[top.tri][e];
                if (nb >= 0 && labels[nb] == -1) {
                    pq.push({top.cost + edge_saliency[top.tri][e], nb, top.label});
                }
            }
        }

        for (size_t i = 0; i < labels.size(); ++i) {
            if (labels[i] == -1) {
                labels[i] = current_label++;
            }
        }
    }

    void merge_small_islands(
        std::vector<int>& labels,
        const std::vector<std::array<float, 3>>& edge_saliency,
        const MultiScaleConfig& cfg) const
    {
        bool merged_any = true;
        int safety_iters = 0;

        while (merged_any && safety_iters++ < 10) {
            merged_any = false;
            std::unordered_map<int, float> region_area;
            std::unordered_map<int, float> region_sens;
            std::unordered_map<int, int> region_count;

            for (size_t i = 0; i < labels.size(); ++i) {
                int lbl = labels[i];
                region_area[lbl] += m_areas[i];
                region_sens[lbl] += m_local_sensitivity_boost[i];
                region_count[lbl] += 1;
            }

            std::unordered_map<int, std::pair<float, int>> best_neighbor;
            for (size_t i = 0; i < labels.size(); ++i) {
                int la = labels[i];
                float mean_local_boost = region_sens[la] / std::max(1, region_count[la]);
                float eff_sens = std::clamp(cfg.global_sensitivity + mean_local_boost, 0.05f, 1.0f);

                float min_area = m_total_area * cfg.min_region_area_fraction *
                                 std::pow(1.0f - 0.70f * eff_sens, 1.4f);

                if (region_area[la] >= min_area) continue;

                for (int e = 0; e < 3; ++e) {
                    int j = m_neighbors[i][e];
                    if (j < 0) continue;
                    int lb = labels[j];
                    if (la == lb) continue;

                    float barrier = edge_saliency[i][e];
                    auto it = best_neighbor.find(la);
                    if (it == best_neighbor.end() || barrier < it->second.first) {
                        best_neighbor[la] = {barrier, lb};
                    }
                }
            }

            if (!best_neighbor.empty()) {
                for (size_t i = 0; i < labels.size(); ++i) {
                    auto it = best_neighbor.find(labels[i]);
                    if (it != best_neighbor.end()) {
                        labels[i] = it->second.second;
                        merged_any = true;
                    }
                }
            }
        }
    }

    void smooth_region_boundaries(
        std::vector<int>& labels,
        const std::vector<std::array<float, 3>>& edge_saliency) const
    {
        std::vector<int> next_labels = labels;
        for (size_t i = 0; i < labels.size(); ++i) {
            int my_label = labels[i];
            std::unordered_map<int, int> neighbor_counts;
            std::unordered_map<int, float> neighbor_saliency_sum;

            for (int e = 0; e < 3; ++e) {
                int j = m_neighbors[i][e];
                if (j < 0) continue;
                int nb_label = labels[j];
                neighbor_counts[nb_label]++;
                neighbor_saliency_sum[nb_label] += edge_saliency[i][e];
            }

            for (const auto& kv : neighbor_counts) {
                int candidate_label = kv.first;
                int count = kv.second;
                if (candidate_label != my_label && count >= 2) {
                    float avg_sal = neighbor_saliency_sum[candidate_label] / static_cast<float>(count);
                    if (avg_sal < 1.15f) {
                        next_labels[i] = candidate_label;
                        break;
                    }
                }
            }
        }
        labels.swap(next_labels);
    }

    std::vector<SegmentedRegion> build_region_descriptors(
        const std::vector<int>& raw_labels,
        const std::vector<Vec3f>& normals_fine,
        const std::vector<std::array<float, 3>>& edge_saliency) const
    {
        std::unordered_map<int, int> label_remap;
        std::vector<SegmentedRegion> regions;

        for (size_t i = 0; i < raw_labels.size(); ++i) {
            int raw = raw_labels[i];
            auto it = label_remap.find(raw);
            int idx = 0;
            if (it == label_remap.end()) {
                idx = static_cast<int>(regions.size());
                label_remap[raw] = idx;
                SegmentedRegion reg;
                reg.region_id = idx;
                reg.z_min_mm = std::numeric_limits<float>::max();
                reg.z_max_mm = std::numeric_limits<float>::lowest();
                regions.push_back(reg);
            } else {
                idx = it->second;
            }

            auto& reg = regions[idx];
            reg.triangle_indices.push_back(static_cast<int>(i));
            reg.total_area_mm2 += m_areas[i];
            reg.centroid += m_centroids[i] * m_areas[i];
            reg.mean_normal += normals_fine[i] * m_areas[i];

            float r_xy = std::sqrt(m_centroids[i].x * m_centroids[i].x +
                                   m_centroids[i].y * m_centroids[i].y);
            reg.mean_radial_distance_mm += r_xy * m_areas[i];
            reg.z_min_mm = std::min(reg.z_min_mm, m_centroids[i].z);
            reg.z_max_mm = std::max(reg.z_max_mm, m_centroids[i].z);
        }

        std::vector<std::set<int>> adj_sets(regions.size());
        std::vector<float> boundary_sal_sum(regions.size(), 0.0f);
        std::vector<int> boundary_edge_count(regions.size(), 0);

        for (auto& reg : regions) {
            if (reg.total_area_mm2 > 1e-6f) {
                reg.centroid = reg.centroid * (1.0f / reg.total_area_mm2);
                reg.mean_radial_distance_mm /= reg.total_area_mm2;
                reg.mean_normal = reg.mean_normal.normalized();
            }
        }

        for (size_t i = 0; i < raw_labels.size(); ++i) {
            int ra = label_remap[raw_labels[i]];
            for (int e = 0; e < 3; ++e) {
                int j = m_neighbors[i][e];
                if (j < 0) continue;
                int rb = label_remap[raw_labels[j]];
                if (ra != rb) {
                    adj_sets[ra].insert(rb);
                    boundary_sal_sum[ra] += edge_saliency[i][e];
                    boundary_edge_count[ra]++;
                }
            }
        }

        for (size_t r = 0; r < regions.size(); ++r) {
            regions[r].neighbor_region_ids.assign(adj_sets[r].begin(), adj_sets[r].end());
            if (boundary_edge_count[r] > 0) {
                regions[r].mean_relief_step_deg =
                    (boundary_sal_sum[r] / static_cast<float>(boundary_edge_count[r])) * 22.0f;
            }
        }

        std::sort(regions.begin(), regions.end(), [](const SegmentedRegion& a, const SegmentedRegion& b) {
            return a.z_min_mm < b.z_min_mm;
        });

        std::unordered_map<int, int> old_to_new;
        for (size_t i = 0; i < regions.size(); ++i) {
            old_to_new[regions[i].region_id] = static_cast<int>(i);
            regions[i].region_id = static_cast<int>(i);
        }
        for (auto& reg : regions) {
            for (auto& nb : reg.neighbor_region_ids) {
                nb = old_to_new[nb];
            }
            std::sort(reg.neighbor_region_ids.begin(), reg.neighbor_region_ids.end());
        }

        return regions;
    }
};

// ============================================================================
// 3. Gemini Cloud API Client + Graph-Coloring Fallback Assignment
// ============================================================================

class GeminiColorAssigner {
public:
    static std::string build_gemini_request_json(
        const std::vector<SegmentedRegion>& regions,
        const std::vector<FilamentSlot>& filaments,
        const std::string& user_style_prompt,
        const std::string& viewport_png_base64 = "")
    {
        std::ostringstream oss;
        oss << "{\n"
            << "  \"contents\": [{\n"
            << "    \"parts\": [\n";

        if (!viewport_png_base64.empty()) {
            oss << "      {\n"
                << "        \"inline_data\": {\n"
                << "          \"mime_type\": \"image/png\",\n"
                << "          \"data\": \"" << viewport_png_base64 << "\"\n"
                << "        }\n"
                << "      },\n";
        }

        oss << "      {\n"
            << "        \"text\": \"You are an expert 3D print multi-color segmentation assistant inside OrcaSlicer. "
            << "A 3D mesh has been segmented into distinct geometric regions separated by persistent low-resolution edge raises. "
            << "Assign each region_id to the best extruder_id (1-based) from the loaded AMS filaments so that adjacent regions across raised edge transitions receive contrasting, semantically appropriate colors.\\n\\n"
            << "User Style / Part Prompt: " << escape_json(user_style_prompt.empty() ? "Auto-contrast raised trims, wraps, and guards against the main body." : user_style_prompt) << "\\n\\n"
            << "Loaded AMS Filaments:\\n";

        for (const auto& f : filaments) {
            oss << "- Extruder " << f.extruder_id << ": " << escape_json(f.name)
                << " (" << escape_json(f.hex_color) << ")\\n";
        }

        oss << "\\nSegmented 3D Regions:\\n";
        for (const auto& r : regions) {
            oss << "- region_id=" << r.region_id
                << ", area_mm2=" << r.total_area_mm2
                << ", z_span_mm=[" << r.z_min_mm << ", " << r.z_max_mm << "]"
                << ", mean_radius_mm=" << r.mean_radial_distance_mm
                << ", boundary_step_deg=" << r.mean_relief_step_deg
                << ", adjacent_regions=[";
            for (size_t k = 0; k < r.neighbor_region_ids.size(); ++k) {
                if (k > 0) oss << ",";
                oss << r.neighbor_region_ids[k];
            }
            oss << "]\\n";
        }

        oss << "\\nReturn ONLY valid JSON of the form: {\\\"assignments\\\": [{\\\"region_id\\\": 0, \\\"extruder_id\\\": 1}, ...]}\"\n"
            << "      }\n"
            << "    ]\n"
            << "  }],\n"
            << "  \"generationConfig\": {\n"
            << "    \"responseMimeType\": \"application/json\",\n"
            << "    \"temperature\": 0.1\n"
            << "  }\n"
            << "}";
        return oss.str();
    }

    static void assign_extruders(
        std::vector<SegmentedRegion>& regions,
        const std::vector<FilamentSlot>& filaments,
        const std::string& api_key,
        const std::string& user_style_prompt,
        const std::string& viewport_png_base64 = "")
    {
        if (regions.empty() || filaments.empty()) return;

#ifdef ORCASLICER_BUILD
        if (!api_key.empty()) {
            if (call_gemini_api(regions, filaments, api_key, user_style_prompt, viewport_png_base64)) {
                return;
            }
        }
#else
        (void)api_key;
        (void)user_style_prompt;
        (void)viewport_png_base64;
#endif
        assign_relief_aware_graph_coloring(regions, filaments);
    }

    static void assign_relief_aware_graph_coloring(
        std::vector<SegmentedRegion>& regions,
        const std::vector<FilamentSlot>& filaments)
    {
        const int num_filaments = static_cast<int>(filaments.size());
        if (num_filaments <= 0) return;

        int largest_idx = 0;
        for (size_t i = 1; i < regions.size(); ++i) {
            if (regions[i].total_area_mm2 > regions[largest_idx].total_area_mm2) {
                largest_idx = static_cast<int>(i);
            }
        }
        float base_radius = regions[largest_idx].mean_radial_distance_mm;

        std::vector<int> assigned_slot(regions.size(), -1);
        std::vector<int> accent_usage(num_filaments, 0);

        for (size_t r = 0; r < regions.size(); ++r) {
            if (std::fabs(regions[r].mean_radial_distance_mm - base_radius) < 0.25f) {
                assigned_slot[r] = 0;
            }
        }

        for (size_t r = 0; r < regions.size(); ++r) {
            if (assigned_slot[r] != -1) continue;

            std::set<int> forbidden_slots;
            forbidden_slots.insert(0);
            for (int nb : regions[r].neighbor_region_ids) {
                if (assigned_slot[nb] != -1) {
                    forbidden_slots.insert(assigned_slot[nb]);
                }
            }

            int best_slot = -1;
            int min_use = std::numeric_limits<int>::max();
            for (int s = 1; s < num_filaments; ++s) {
                if (forbidden_slots.count(s) == 0 && accent_usage[s] < min_use) {
                    min_use = accent_usage[s];
                    best_slot = s;
                }
            }
            if (best_slot == -1) {
                best_slot = (num_filaments > 1) ? (1 + static_cast<int>(r % (num_filaments - 1))) : 0;
            }
            assigned_slot[r] = best_slot;
            accent_usage[best_slot]++;
        }

        for (size_t r = 0; r < regions.size(); ++r) {
            regions[r].assigned_extruder_id = filaments[assigned_slot[r]].extruder_id;
        }
    }

private:
    static std::string escape_json(const std::string& s) {
        std::ostringstream o;
        for (char c : s) {
            switch (c) {
                case '"': o << "\\\""; break;
                case '\\': o << "\\\\"; break;
                case '\n': o << "\\n"; break;
                case '\r': o << "\\r"; break;
                case '\t': o << "\\t"; break;
                default: o << c; break;
            }
        }
        return o.str();
    }

#ifdef ORCASLICER_BUILD
    static size_t curl_write_cb(void* contents, size_t size, size_t nmemb, std::string* out) {
        size_t total = size * nmemb;
        out->append(static_cast<char*>(contents), total);
        return total;
    }

    static bool call_gemini_api(
        std::vector<SegmentedRegion>& regions,
        const std::vector<FilamentSlot>& filaments,
        const std::string& api_key,
        const std::string& user_style_prompt,
        const std::string& viewport_png_base64)
    {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        std::string url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=" + api_key;
        std::string payload = build_gemini_request_json(regions, filaments, user_style_prompt, viewport_png_base64);
        std::string response_body;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK || http_code != 200) {
            return false;
        }

        try {
            auto root = nlohmann::json::parse(response_body);
            std::string inner_text = root["candidates"][0]["content"]["parts"][0]["text"].get<std::string>();
            auto parsed = nlohmann::json::parse(inner_text);

            for (const auto& item : parsed["assignments"]) {
                int reg_id = item["region_id"].get<int>();
                int ext_id = item["extruder_id"].get<int>();
                if (reg_id >= 0 && reg_id < static_cast<int>(regions.size())) {
                    regions[reg_id].assigned_extruder_id = ext_id;
                }
            }
            return true;
        } catch (...) {
            return false;
        }
    }
#endif
};

// ============================================================================
// 4. OrcaSlicer GLGizmoMmuSegmentation Integration Hook
// ============================================================================
#ifdef ORCASLICER_BUILD

class OrcaAutoPainterGizmoController {
public:
    MultiScaleConfig config;
    float local_brush_sensitivity_boost{0.45f};
    bool detail_sensitivity_brush_active{false};
    char style_prompt_buf[256]{"Auto-contrast raised trims, guards, and wraps against base shaft"};
    char gemini_api_key_buf[128]{""};
    int last_region_count{0};

    OrcaAutoPainterGizmoController() {
        if (const char* env_key = std::getenv("GEMINI_API_KEY")) {
            std::snprintf(gemini_api_key_buf, sizeof(gemini_api_key_buf), "%s", env_key);
        }
    }

    // Call inside GLGizmoMmuSegmentation::on_render_input_window() in
    // src/slic3r/GUI/Gizmos/GLGizmoMmuSegmentation.cpp
    void render_imgui_controls(
        const indexed_triangle_set& its,
        std::vector<std::unique_ptr<TriangleSelector>>& triangle_selectors,
        const std::vector<FilamentSlot>& filaments,
        std::function<void()> on_take_undo_snapshot,
        std::function<void()> on_sync_selectors_to_model)
    {
        ImGui::Separator();
        ImGui::Text("AI Multi-Scale Edge Auto-Painter");

        ImGui::SliderFloat(
            "Edge Sensitivity (Global)",
            &config.global_sensitivity,
            0.10f, 0.95f, "%.2f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Default (0.35): Only significant edge raises that remain visible in a low-resolution view create color boundaries.\n"
                "Increase toward 1.0 to split finer surface details across the whole model.");
        }

        ImGui::SliderInt(
            "Low-Res Filter Scale",
            &config.lowres_smoothing_passes,
            2, 16);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Controls how much the mesh is low-pass filtered to simulate the low-resolution view.");
        }

        ImGui::Checkbox("Paint Local Section Sensitivity Boost", &detail_sensitivity_brush_active);
        if (detail_sensitivity_brush_active) {
            ImGui::SliderFloat(
                "Section Detail Boost",
                &local_brush_sensitivity_boost,
                -0.30f, 0.60f, "+%.2f");
            if (ImGui::Button("Reset Local Section Boosts")) {
                if (m_segmenter) m_segmenter->clear_local_sensitivity();
            }
        }

        ImGui::InputText("Style Hint", style_prompt_buf, sizeof(style_prompt_buf));
        ImGui::InputText("Gemini API Key", gemini_api_key_buf, sizeof(gemini_api_key_buf), ImGuiInputTextFlags_Password);

        if (ImGui::Button("Auto-Paint by Edge Transitions (Gemini)")) {
            ensure_segmenter_initialized(its);
            on_take_undo_snapshot();

            auto regions = m_segmenter->segment(config);
            last_region_count = static_cast<int>(regions.size());

            GeminiColorAssigner::assign_extruders(
                regions,
                filaments,
                std::string(gemini_api_key_buf),
                std::string(style_prompt_buf));

            if (!triangle_selectors.empty() && triangle_selectors[0]) {
                TriangleSelector& selector = *triangle_selectors[0];
                selector.reset();
                for (const auto& reg : regions) {
                    auto state = static_cast<EnforcerBlockerType>(reg.assigned_extruder_id);
                    for (int tri_idx : reg.triangle_indices) {
                        selector.set_facet(tri_idx, state);
                    }
                }
                on_sync_selectors_to_model();
            }
        }

        if (last_region_count > 0) {
            ImGui::TextDisabled("Segmented into %d persistent edge regions.", last_region_count);
        }
    }

    void on_brush_stroke(const indexed_triangle_set& its, const Vec3f& hit_pos, float cursor_radius) {
        ensure_segmenter_initialized(its);
        m_segmenter->paint_local_sensitivity_sphere(hit_pos, cursor_radius, local_brush_sensitivity_boost);
    }

private:
    std::unique_ptr<MultiScaleEdgeSegmenter> m_segmenter;
    size_t m_cached_tri_count{0};

    void ensure_segmenter_initialized(const indexed_triangle_set& its) {
        if (m_segmenter && m_cached_tri_count == its.indices.size()) return;

        std::vector<Vec3f> verts;
        verts.reserve(its.vertices.size());
        for (const auto& v : its.vertices) {
            verts.emplace_back(v.x(), v.y(), v.z());
        }

        std::vector<IndexedTriangle> tris;
        tris.reserve(its.indices.size());
        for (const auto& f : its.indices) {
            tris.push_back({f(0), f(1), f(2)});
        }

        m_segmenter = std::make_unique<MultiScaleEdgeSegmenter>(verts, tris);
        m_cached_tri_count = tris.size();
    }
};

#endif // ORCASLICER_BUILD

} // namespace GUI
} // namespace Slic3r

// ============================================================================
// 5. Standalone Verification & CLI Test Harness (Compiles without wxWidgets)
// ============================================================================
#ifndef ORCASLICER_BUILD

namespace {

// Generates a watertight 3D tubular prop mesh modeled after the painted bow limb:
//   - Z in [0, 8]:    Raised Teal Tip Cap (radius = 5.4mm)
//   - Z in [8, 12]:   Lower Tan Shaft section (radius = 4.0mm)
//   - Z in [12, 26]:  Raised Maroon Grip Wrap (radius = 5.2mm)
//   - Z in [26, 72]:  Main Tan Shaft section (radius = 4.0mm, with subtle +-0.025mm surface ripple)
//   - Z in [72, 90]:  Raised Teal Ornate Guard (radius = 5.6mm) containing a finer
//                     decorative trim band at Z in [79, 83] (radius +0.28mm)
//   - Z in [90, 100]: Upper Tan Shaft tenon (radius = 4.0mm)
void generate_synthetic_bow_limb_mesh(
    std::vector<Slic3r::GUI::Vec3f>& verts,
    std::vector<Slic3r::GUI::IndexedTriangle>& tris)
{
    const int rings = 100;
    const int sectors = 24;
    const float length_z = 100.0f;

    verts.clear();
    tris.clear();

    for (int i = 0; i <= rings; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(rings);
        float z = t * length_z;

        float r = 4.0f + 0.025f * std::sin(z * 2.5f);

        if (z <= 8.0f) {
            r = 5.4f; // Raised Tip Cap
        } else if (z >= 12.0f && z <= 26.0f) {
            r = 5.2f; // Raised Grip Wrap
        } else if (z >= 72.0f && z <= 90.0f) {
            r = 5.6f; // Raised Ornate Guard
            // Subtle decorative trim band inside the guard
            if (z >= 79.0f && z <= 83.0f) {
                r += 0.28f;
            }
        }

        for (int s = 0; s < sectors; ++s) {
            float phi = (2.0f * 3.14159265f * static_cast<float>(s)) / static_cast<float>(sectors);
            verts.emplace_back(r * std::cos(phi), r * std::sin(phi), z);
        }
    }

    int bottom_center = static_cast<int>(verts.size());
    verts.emplace_back(0.0f, 0.0f, 0.0f);
    int top_center = static_cast<int>(verts.size());
    verts.emplace_back(0.0f, 0.0f, length_z);

    for (int i = 0; i < rings; ++i) {
        for (int s = 0; s < sectors; ++s) {
            int next_s = (s + 1) % sectors;
            int v00 = i * sectors + s;
            int v10 = i * sectors + next_s;
            int v01 = (i + 1) * sectors + s;
            int v11 = (i + 1) * sectors + next_s;

            tris.push_back({v00, v10, v11});
            tris.push_back({v00, v11, v01});
        }
    }

    for (int s = 0; s < sectors; ++s) {
        int next_s = (s + 1) % sectors;
        tris.push_back({bottom_center, next_s, s});
        int top_offset = rings * sectors;
        tris.push_back({top_center, top_offset + s, top_offset + next_s});
    }
}

} // namespace

int main() {
    using namespace Slic3r::GUI;

    std::vector<Vec3f> verts;
    std::vector<IndexedTriangle> tris;
    generate_synthetic_bow_limb_mesh(verts, tris);

    MultiScaleEdgeSegmenter segmenter(verts, tris);
    MultiScaleConfig cfg;
    cfg.global_sensitivity = 0.35f; // Default: low-resolution persistent raises only

    std::vector<FilamentSlot> filaments = {
        {1, "Tan Wood PLA", "#D8A468"},
        {2, "Dark Teal PLA", "#3A6470"},
        {3, "Maroon Wrap PLA", "#8B4356"},
        {4, "Gold Accent PLA", "#E5C158"}
    };

    // Pass 1: Default Sensitivity (0.35 - Low-Res Persistent Edge Raises Only)
    auto coarse_regions = segmenter.segment(cfg);
    GeminiColorAssigner::assign_extruders(
        coarse_regions, filaments, "", "Tan shaft, teal guard and tip, maroon grip wrap");

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "=== Pass 1: Default Sensitivity (0.35 - Low-Res Persistent Edges) ===\n";
    std::cout << "Detected " << coarse_regions.size() << " major regions:\n";
    for (const auto& r : coarse_regions) {
        std::cout << "  Region " << r.region_id
                  << " | Z=[" << std::setw(4) << r.z_min_mm << "mm, " << std::setw(5) << r.z_max_mm << "mm]"
                  << " | MeanRadius=" << r.mean_radial_distance_mm << "mm"
                  << " | Area=" << std::setw(6) << r.total_area_mm2 << " mm^2"
                  << " | Extruder=" << r.assigned_extruder_id
                  << " (" << filaments[r.assigned_extruder_id - 1].name << ")\n";
    }

    // Pass 2: Boost Local Sensitivity ONLY on the Upper Guard Section (Z in [70, 92])
    segmenter.set_section_sensitivity_box({-20.0f, -20.0f, 70.0f}, {20.0f, 20.0f, 92.0f}, 0.45f);
    auto detailed_regions = segmenter.segment(cfg);
    GeminiColorAssigner::assign_extruders(
        detailed_regions, filaments, "", "Tan shaft, teal guard with gold trim, maroon grip wrap");

    std::cout << "\n=== Pass 2: Local Section Sensitivity Boost (+0.45 on Upper Guard Z=[70,92]) ===\n";
    std::cout << "Detected " << detailed_regions.size() << " regions (fine guard trim now separated):\n";
    for (const auto& r : detailed_regions) {
        std::cout << "  Region " << r.region_id
                  << " | Z=[" << std::setw(4) << r.z_min_mm << "mm, " << std::setw(5) << r.z_max_mm << "mm]"
                  << " | MeanRadius=" << r.mean_radial_distance_mm << "mm"
                  << " | Area=" << std::setw(6) << r.total_area_mm2 << " mm^2"
                  << " | Extruder=" << r.assigned_extruder_id
                  << " (" << filaments[r.assigned_extruder_id - 1].name << ")\n";
    }

    return 0;
}
#endif
