#include "TriangleSelector.hpp"
#include "Model.hpp"

#include <boost/container/small_vector.hpp>
#include <boost/log/trivial.hpp>
#include <cstddef>
#include <tbb/parallel_for.h>

#ifndef NDEBUG
//    #define EXPENSIVE_DEBUG_CHECKS
#endif // NDEBUG

namespace Slic3r {

// Check if the line is whole inside the sphere, or it is partially inside (intersecting) the sphere.
// Inspired by Christer Ericson's Real-Time Collision Detection, pp. 177-179.
static bool test_line_inside_sphere(const Vec3f &line_a, const Vec3f &line_b, const Vec3f &sphere_p, const float sphere_radius)
{
    const float sphere_radius_sqr = Slic3r::sqr(sphere_radius);
    const Vec3f line_dir          = line_b - line_a;   // n
    const Vec3f origins_diff      = line_a - sphere_p; // m

    const float m_dot_m           = origins_diff.dot(origins_diff);
    // Check if any of the end-points of the line is inside the sphere.
    if (m_dot_m <= sphere_radius_sqr || (line_b - sphere_p).squaredNorm() <= sphere_radius_sqr)
        return true;

    // Check if the infinite line is going through the sphere.
    const float n_dot_n = line_dir.dot(line_dir);
    const float m_dot_n = origins_diff.dot(line_dir);

    const float eq_a    = n_dot_n;
    const float eq_b    = m_dot_n;
    const float eq_c    = m_dot_m - sphere_radius_sqr;

    const float discr = eq_b * eq_b - eq_a * eq_c;
    // A negative discriminant corresponds to the infinite line infinite not going through the sphere.
    if (discr < 0.f)
        return false;

    // Check if the finite line is going through the sphere.
    const float discr_sqrt = std::sqrt(discr);
    const float t1         = (-eq_b - discr_sqrt) / eq_a;
    if (0.f <= t1 && t1 <= 1.f)
        return true;

    const float t2 = (-eq_b + discr_sqrt) / eq_a;
    if (0.f <= t2 && t2 <= 1.f && discr_sqrt > 0.f)
        return true;

    return false;
}

// Check if the line is whole inside the finite cylinder, or it is partially inside (intersecting) the finite cylinder.
// Inspired by Christer Ericson's Real-Time Collision Detection, pp. 194-198.
static bool test_line_inside_cylinder(const Vec3f &line_a, const Vec3f &line_b, const Vec3f &cylinder_P, const Vec3f &cylinder_Q, const float cylinder_radius)
{
    assert(cylinder_P != cylinder_Q);
    const Vec3f cylinder_dir                    = cylinder_Q - cylinder_P; // d
    auto        is_point_inside_finite_cylinder = [&cylinder_P, &cylinder_Q, &cylinder_radius, &cylinder_dir](const Vec3f &pt) {
        const Vec3f first_center_diff  = cylinder_P - pt;
        const Vec3f second_center_diff = cylinder_Q - pt;
        // First, check if the point pt is laying between planes defined by cylinder_p and cylinder_q.
        // Then check if it is inside the cylinder between cylinder_p and cylinder_q.
        return first_center_diff.dot(cylinder_dir) <= 0 && second_center_diff.dot(cylinder_dir) >= 0 &&
               (first_center_diff.cross(cylinder_dir).norm() / cylinder_dir.norm()) <= cylinder_radius;
    };

    // Check if any of the end-points of the line is inside the cylinder.
    if (is_point_inside_finite_cylinder(line_a) || is_point_inside_finite_cylinder(line_b))
       return true;

    // Check if the line is going through the cylinder.
    const Vec3f origins_diff = line_a - cylinder_P;     // m
    const Vec3f line_dir     = line_b - line_a;         // n

    const float m_dot_d = origins_diff.dot(cylinder_dir);
    const float n_dot_d = line_dir.dot(cylinder_dir);
    const float d_dot_d = cylinder_dir.dot(cylinder_dir);

    const float n_dot_n = line_dir.dot(line_dir);
    const float m_dot_n = origins_diff.dot(line_dir);
    const float m_dot_m = origins_diff.dot(origins_diff);

    const float eq_a    = d_dot_d * n_dot_n - n_dot_d * n_dot_d;
    const float eq_b    = d_dot_d * m_dot_n - n_dot_d * m_dot_d;
    const float eq_c    = d_dot_d * (m_dot_m - Slic3r::sqr(cylinder_radius)) - m_dot_d * m_dot_d;

    const float discr   = eq_b * eq_b - eq_a * eq_c;
    // A negative discriminant corresponds to the infinite line not going through the infinite cylinder.
    if (discr < 0.0f)
        return false;

    // Check if the finite line is going through the finite cylinder.
    const float discr_sqrt = std::sqrt(discr);
    const float t1         = (-eq_b - discr_sqrt) / eq_a;
    if (0.f <= t1 && t1 <= 1.f)
        if (const float cylinder_endcap_t1 = m_dot_d + t1 * n_dot_d; 0.f <= cylinder_endcap_t1 && cylinder_endcap_t1 <= d_dot_d)
            return true;

    const float t2 = (-eq_b + discr_sqrt) / eq_a;
    if (0.f <= t2 && t2 <= 1.f)
        if (const float cylinder_endcap_t2 = (m_dot_d + t2 * n_dot_d); 0.f <= cylinder_endcap_t2 && cylinder_endcap_t2 <= d_dot_d)
            return true;

    return false;
}

// Check if the line is whole inside the capsule, or it is partially inside (intersecting) the capsule.
static bool test_line_inside_capsule(const Vec3f &line_a, const Vec3f &line_b, const Vec3f &capsule_p, const Vec3f &capsule_q, const float capsule_radius) {
    assert(capsule_p != capsule_q);

    // Check if the line intersect any of the spheres forming the capsule.
    if (test_line_inside_sphere(line_a, line_b, capsule_p, capsule_radius) || test_line_inside_sphere(line_a, line_b, capsule_q, capsule_radius))
        return true;

    // Check if the line intersects the cylinder between the centers of the spheres.
    return test_line_inside_cylinder(line_a, line_b, capsule_p, capsule_q, capsule_radius);
}

#ifndef NDEBUG
bool TriangleSelector::verify_triangle_midpoints(const Triangle &tr) const
{
    for (int i = 0; i < 3; ++ i) {
        int v1   = tr.verts_idxs[i];
        int v2   = tr.verts_idxs[next_idx_modulo(i, 3)];
        int vmid = this->triangle_midpoint(tr, v1, v2);
        assert(vmid >= -1);
        if (vmid != -1) {
            Vec3f c1 = 0.5f * (m_vertices[v1].v + m_vertices[v2].v);
            Vec3f c2 = m_vertices[vmid].v;
            float d  = (c2 - c1).norm();
            assert(std::abs(d) < EPSILON);
        }
    }
    return true;
}

bool TriangleSelector::verify_triangle_neighbors(const Triangle &tr, const Vec3i32 &neighbors) const
{
    assert(neighbors(0) >= -1);
    assert(neighbors(1) >= -1);
    assert(neighbors(2) >= -1);
    assert(verify_triangle_midpoints(tr));

    for (int i = 0; i < 3; ++i)
        if (neighbors(i) != -1) {
            const Triangle &tr2 = m_triangles[neighbors(i)];
            assert(verify_triangle_midpoints(tr2));
            int v1 = tr.verts_idxs[i];
            int v2 = tr.verts_idxs[next_idx_modulo(i, 3)];
            assert(tr2.verts_idxs[0] == v1 || tr2.verts_idxs[1] == v1 || tr2.verts_idxs[2] == v1);
            int j = tr2.verts_idxs[0] == v1 ? 0 : tr2.verts_idxs[1] == v1 ? 1 : 2;
            assert(tr2.verts_idxs[j] == v1);
            assert(tr2.verts_idxs[prev_idx_modulo(j, 3)] == v2);
        }
    return true;
}
#endif // NDEBUG

// sides_to_split==-1 : just restore previous split
void TriangleSelector::Triangle::set_division(int sides_to_split, int special_side_idx)
{
    assert(sides_to_split >= 0 && sides_to_split <= 3);
    assert(special_side_idx >= 0 && special_side_idx < 3);
    assert(sides_to_split == 1 || sides_to_split == 2 || special_side_idx == 0);
    this->number_of_splits = char(sides_to_split);
    this->special_side_idx = char(special_side_idx);
}

inline bool is_point_inside_triangle(const Vec3f &pt, const Vec3f &p1, const Vec3f &p2, const Vec3f &p3)
{
    // Real-time collision detection, Ericson, Chapter 3.4
    auto barycentric = [&pt, &p1, &p2, &p3]() -> Vec3f {
        std::array<Vec3f, 3> v     = {p2 - p1, p3 - p1, pt - p1};
        float                d00   = v[0].dot(v[0]);
        float                d01   = v[0].dot(v[1]);
        float                d11   = v[1].dot(v[1]);
        float                d20   = v[2].dot(v[0]);
        float                d21   = v[2].dot(v[1]);
        float                denom = d00 * d11 - d01 * d01;

        Vec3f barycentric_cords(1.f, (d11 * d20 - d01 * d21) / denom, (d00 * d21 - d01 * d20) / denom);
        barycentric_cords.x() = barycentric_cords.x() - barycentric_cords.y() - barycentric_cords.z();
        return barycentric_cords;
    };

    Vec3f barycentric_cords = barycentric();
    return std::all_of(begin(barycentric_cords), end(barycentric_cords), [](float cord) { return 0.f <= cord && cord <= 1.0; });
}

int TriangleSelector::select_unsplit_triangle(const Vec3f &hit, int facet_idx, const Vec3i32 &neighbors) const
{
    assert(facet_idx < int(m_triangles.size()));
    const Triangle *tr = &m_triangles[facet_idx];
    if (!tr->valid())
        return -1;

    if (!tr->is_split()) {
        if (const std::array<int, 3> &t_vert = m_triangles[facet_idx].verts_idxs; is_point_inside_triangle(hit, m_vertices[t_vert[0]].v, m_vertices[t_vert[1]].v, m_vertices[t_vert[2]].v))
            return facet_idx;

        return -1;
    }

    assert(this->verify_triangle_neighbors(*tr, neighbors));

    int num_of_children = tr->number_of_split_sides() + 1;
    if (num_of_children != 1) {
        for (int i = 0; i < num_of_children; ++i) {
            assert(i < int(tr->children.size()));
            assert(tr->children[i] < int(m_triangles.size()));
            // Recursion, deep first search over the children of this triangle.
            // All children of this triangle were created by splitting a single source triangle of the original mesh.

            const std::array<int, 3> &t_vert = m_triangles[tr->children[i]].verts_idxs;
            if (is_point_inside_triangle(hit, m_vertices[t_vert[0]].v, m_vertices[t_vert[1]].v, m_vertices[t_vert[2]].v))
                return this->select_unsplit_triangle(hit, tr->children[i], this->child_neighbors(*tr, neighbors, i));
        }
    }

    return -1;
}

int TriangleSelector::select_unsplit_triangle(const Vec3f &hit, int facet_idx) const
{
    assert(facet_idx < int(m_triangles.size()));
    if (!m_triangles[facet_idx].valid())
        return -1;

    Vec3i32 neighbors = m_neighbors[facet_idx];
    assert(this->verify_triangle_neighbors(m_triangles[facet_idx], neighbors));
    return this->select_unsplit_triangle(hit, facet_idx, neighbors);
}

void TriangleSelector::select_patch(int facet_start, std::unique_ptr<Cursor> &&cursor, EnforcerBlockerType new_state, const Transform3d& trafo_no_translate, bool triangle_splitting, float highlight_by_angle_deg)
{
    assert(facet_start < m_orig_size_indices);

    // Save current cursor center, squared radius and camera direction, so we don't
    // have to pass it around.
    m_cursor = std::move(cursor);

    // In case user changed cursor size since last time, update triangle edge limit.
    // It is necessary to compare the internal radius in m_cursor! radius is in
    // world coords and does not change after scaling.
    if (m_old_cursor_radius_sqr != m_cursor->radius_sqr) {
        // BBS: improve details for large cursor radius
        TriangleSelector::HeightRange* hr_cursor = dynamic_cast<TriangleSelector::HeightRange*>(m_cursor.get());
        if (hr_cursor == nullptr) {
            set_edge_limit(std::min(std::sqrt(m_cursor->radius_sqr) / 5.f, 0.05f));
            m_old_cursor_radius_sqr = m_cursor->radius_sqr;
        }
        else {
            set_edge_limit(0.1);
            m_old_cursor_radius_sqr = 0.1;
        }
    }

    const float highlight_angle_limit = -cos(Geometry::deg2rad(highlight_by_angle_deg));

    // BBS
    std::vector<int> start_facets;
    HeightRange* hr_cursor = dynamic_cast<HeightRange*>(m_cursor.get());
    if (hr_cursor) {
        for (int facet_id = 0; facet_id < m_orig_size_indices; facet_id++) {
            const Triangle& tr = m_triangles[facet_id];
            if (m_cursor->is_edge_inside_cursor(tr, m_vertices)) {
                start_facets.push_back(facet_id);
            }
        }
    }
    else {
        start_facets.push_back(facet_start);
    }

    // Keep track of facets of the original mesh we already processed.
    std::vector<bool> visited(m_orig_size_indices, false);

    for (int i = 0; i < start_facets.size(); i++) {
        int start_facet_id = start_facets[i];
        if (visited[start_facet_id])
            continue;

        // Now start with the facet the pointer points to and check all adjacent facets.
        std::vector<int> facets_to_check;
        facets_to_check.reserve(16);
        facets_to_check.emplace_back(start_facet_id);

        // Breadth-first search around the hit point. facets_to_check may grow significantly large.
        // Head of the bread-first facets_to_check FIFO.
        int facet_idx = 0;
        while (facet_idx < int(facets_to_check.size())) {
            int          facet = facets_to_check[facet_idx];
            const Vec3f& facet_normal = m_face_normals[m_triangles[facet].source_triangle];
            Matrix3f     normal_matrix = static_cast<Matrix3f>(trafo_no_translate.matrix().block(0, 0, 3, 3).inverse().transpose().cast<float>());
            float        world_normal_z = (normal_matrix* facet_normal).normalized().z();
            if (!visited[facet] && (highlight_by_angle_deg == 0.f || world_normal_z < highlight_angle_limit)) {
                if (select_triangle(facet, new_state, triangle_splitting)) {
                    // add neighboring facets to list to be processed later
                    for (int neighbor_idx : m_neighbors[facet])
                        if (neighbor_idx >= 0 && m_cursor->is_facet_visible(neighbor_idx, m_face_normals))
                            facets_to_check.push_back(neighbor_idx);
                }
            }
            visited[facet] = true;
            ++facet_idx;
        }
    }
}

bool TriangleSelector::is_facet_clipped(int facet_idx, const ClippingPlane &clp) const
{
    for (int vert_idx : m_triangles[facet_idx].verts_idxs)
        if (clp.is_active() && clp.is_mesh_point_clipped(m_vertices[vert_idx].v))
            return true;

    return false;
}

void TriangleSelector::seed_fill_select_triangles(const Vec3f &hit, int facet_start, const Transform3d& trafo_no_translate,
                                                  const ClippingPlane &clp, float seed_fill_angle, float highlight_by_angle_deg,
                                                  bool force_reselection)
{
    assert(facet_start < m_orig_size_indices);

    // Recompute seed fill only if the cursor is pointing on facet unselected by seed fill or a clipping plane is active.
    if (int start_facet_idx = select_unsplit_triangle(hit, facet_start); start_facet_idx >= 0 && m_triangles[start_facet_idx].is_selected_by_seed_fill() && !force_reselection && !clp.is_active())
        return;

    this->seed_fill_unselect_all_triangles();

    std::vector<bool> visited(m_triangles.size(), false);
    std::queue<int>   facet_queue;
    facet_queue.push(facet_start);

    const double facet_angle_limit     = cos(Geometry::deg2rad(seed_fill_angle)) - EPSILON;
    const float  highlight_angle_limit = -cos(Geometry::deg2rad(highlight_by_angle_deg));

    // Depth-first traversal of neighbors of the face hit by the ray thrown from the mouse cursor.
    while (!facet_queue.empty()) {
        int current_facet = facet_queue.front();
        facet_queue.pop();

        const Vec3f &facet_normal = m_face_normals[m_triangles[current_facet].source_triangle];
        Matrix3f     normal_matrix  = static_cast<Matrix3f>(trafo_no_translate.matrix().block(0, 0, 3, 3).inverse().transpose().cast<float>());
        float        world_normal_z = (normal_matrix * facet_normal).normalized().z();
        if (!visited[current_facet] && (highlight_by_angle_deg == 0.f || world_normal_z < highlight_angle_limit)) {
            if (m_triangles[current_facet].is_split()) {
                for (int split_triangle_idx = 0; split_triangle_idx <= m_triangles[current_facet].number_of_split_sides(); ++split_triangle_idx) {
                    assert(split_triangle_idx < int(m_triangles[current_facet].children.size()));
                    assert(m_triangles[current_facet].children[split_triangle_idx] < int(m_triangles.size()));
                    if (int child = m_triangles[current_facet].children[split_triangle_idx]; !visited[child])
                        // Child triangle shares normal with its parent. Select it.
                        facet_queue.push(child);
                }
            } else
                m_triangles[current_facet].select_by_seed_fill();

            if (current_facet < m_orig_size_indices)
                // Propagate over the original triangles.
                for (int neighbor_idx : m_neighbors[current_facet]) {
                    assert(neighbor_idx >= -1);
                    if (neighbor_idx >= 0 && !visited[neighbor_idx] && !is_facet_clipped(neighbor_idx, clp)) {
                        // Check if neighbour_facet_idx is satisfies angle in seed_fill_angle and append it to facet_queue if it do.
                        const Vec3f &n1 = m_face_normals[m_triangles[neighbor_idx].source_triangle];
                        const Vec3f &n2 = m_face_normals[m_triangles[current_facet].source_triangle];
                        if (std::clamp(n1.dot(n2), 0.f, 1.f) >= facet_angle_limit)
                            facet_queue.push(neighbor_idx);
                    }
                }
        }
        visited[current_facet] = true;
    }
}

void TriangleSelector::precompute_all_neighbors_recursive(const int facet_idx, const Vec3i32 &neighbors, const Vec3i32 &neighbors_propagated, std::vector<Vec3i32> &neighbors_out, std::vector<Vec3i32> &neighbors_propagated_out) const
{
    assert(facet_idx < int(m_triangles.size()));

    const Triangle *tr = &m_triangles[facet_idx];
    if (!tr->valid())
        return;

    neighbors_out[facet_idx]            = neighbors;
    neighbors_propagated_out[facet_idx] = neighbors_propagated;
    if (tr->is_split()) {
        assert(this->verify_triangle_neighbors(*tr, neighbors));

        int num_of_children = tr->number_of_split_sides() + 1;
        if (num_of_children != 1) {
            for (int i = 0; i < num_of_children; ++i) {
                assert(i < int(tr->children.size()));
                assert(tr->children[i] < int(m_triangles.size()));
                // Recursion, deep first search over the children of this triangle.
                // All children of this triangle were created by splitting a single source triangle of the original mesh.
                const Vec3i32 child_neighbors = this->child_neighbors(*tr, neighbors, i);
                this->precompute_all_neighbors_recursive(tr->children[i], child_neighbors,
                                                         this->child_neighbors_propagated(*tr, neighbors_propagated, i, child_neighbors), neighbors_out,
                                                         neighbors_propagated_out);
            }
        }
    }
}

std::pair<std::vector<Vec3i32>, std::vector<Vec3i32>> TriangleSelector::precompute_all_neighbors() const
{
    std::vector<Vec3i32> neighbors(m_triangles.size(), Vec3i32(-1, -1, -1));
    std::vector<Vec3i32> neighbors_propagated(m_triangles.size(), Vec3i32(-1, -1, -1));
    for (int facet_idx = 0; facet_idx < this->m_orig_size_indices; ++facet_idx) {
        neighbors[facet_idx]            = m_neighbors[facet_idx];
        neighbors_propagated[facet_idx] = neighbors[facet_idx];
        assert(this->verify_triangle_neighbors(m_triangles[facet_idx], neighbors[facet_idx]));
        if (m_triangles[facet_idx].is_split())
            this->precompute_all_neighbors_recursive(facet_idx, neighbors[facet_idx], neighbors_propagated[facet_idx], neighbors, neighbors_propagated);
    }
    return std::make_pair(std::move(neighbors), std::move(neighbors_propagated));
}

// It appends all triangles that are touching the edge (vertexi, vertexj) of the triangle.
// It doesn't append the triangles that are touching the triangle only by part of the edge that means the triangles are from lower depth.
void TriangleSelector::append_touching_subtriangles(int itriangle, int vertexi, int vertexj, std::vector<int> &touching_subtriangles_out) const
{
    if (itriangle == -1)
        return;

    auto process_subtriangle = [this, &itriangle, &vertexi, &vertexj, &touching_subtriangles_out](const int subtriangle_idx, Partition partition) -> void {
        assert(subtriangle_idx != -1);
        if (!m_triangles[subtriangle_idx].is_split())
            touching_subtriangles_out.emplace_back(subtriangle_idx);
        else if (int midpoint = this->triangle_midpoint(itriangle, vertexi, vertexj); midpoint != -1)
            append_touching_subtriangles(subtriangle_idx, partition == Partition::First ? vertexi : midpoint, partition == Partition::First ? midpoint : vertexj, touching_subtriangles_out);
        else
            append_touching_subtriangles(subtriangle_idx, vertexi, vertexj, touching_subtriangles_out);
    };

    std::pair<int, int> touching = this->triangle_subtriangles(itriangle, vertexi, vertexj);
    if (touching.first != -1)
        process_subtriangle(touching.first, Partition::First);

    if (touching.second != -1)
        process_subtriangle(touching.second, Partition::Second);
}

// It appends all edges that are touching the edge (vertexi, vertexj) of the triangle and are not selected by seed fill
// It doesn't append the edges that are touching the triangle only by part of the edge that means the triangles are from lower depth.
void TriangleSelector::append_touching_edges(int itriangle, int vertexi, int vertexj, std::vector<Vec2i32> &touching_edges_out) const
{
    if (itriangle == -1)
        return;

    auto process_subtriangle = [this, &itriangle, &vertexi, &vertexj, &touching_edges_out](const int subtriangle_idx, Partition partition) -> void {
        assert(subtriangle_idx != -1);
        if (!m_triangles[subtriangle_idx].is_split()) {
            if (!m_triangles[subtriangle_idx].is_selected_by_seed_fill()) {
                int midpoint = this->triangle_midpoint(itriangle, vertexi, vertexj);
                if (partition == Partition::First && midpoint != -1) {
                    touching_edges_out.emplace_back(vertexi, midpoint);
                } else if (partition == Partition::First && midpoint == -1) {
                    touching_edges_out.emplace_back(vertexi, vertexj);
                } else {
                    assert(midpoint != -1 && partition == Partition::Second);
                    touching_edges_out.emplace_back(midpoint, vertexj);
                }
            }
        } else if (int midpoint = this->triangle_midpoint(itriangle, vertexi, vertexj); midpoint != -1)
            append_touching_edges(subtriangle_idx, partition == Partition::First ? vertexi : midpoint, partition == Partition::First ? midpoint : vertexj,
                                  touching_edges_out);
        else
            append_touching_edges(subtriangle_idx, vertexi, vertexj, touching_edges_out);
    };

    std::pair<int, int> touching = this->triangle_subtriangles(itriangle, vertexi, vertexj);
    if (touching.first != -1)
        process_subtriangle(touching.first, Partition::First);

    if (touching.second != -1)
        process_subtriangle(touching.second, Partition::Second);
}

// BBS: add seed_fill_angle parameter
void TriangleSelector::bucket_fill_select_triangles(const Vec3f& hit, int facet_start, const ClippingPlane &clp, float seed_fill_angle, bool propagate, bool force_reselection)
{
    int start_facet_idx = select_unsplit_triangle(hit, facet_start);
    assert(start_facet_idx != -1);
    // Recompute bucket fill only if the cursor is pointing on facet unselected by bucket fill or a clipping plane is active.
    if (start_facet_idx == -1 || (m_triangles[start_facet_idx].is_selected_by_seed_fill() && !force_reselection && !clp.is_active()))
        return;

    assert(!m_triangles[start_facet_idx].is_split());
    EnforcerBlockerType start_facet_state = m_triangles[start_facet_idx].get_state();
    this->seed_fill_unselect_all_triangles();

    if (!propagate) {
        m_triangles[start_facet_idx].select_by_seed_fill();
        return;
    }

    // seed_fill_angle < 0.f to disable edge detection
    const double facet_angle_limit = (seed_fill_angle < 0.f ? -1.f : cos(Geometry::deg2rad(seed_fill_angle))) - EPSILON;

    auto get_all_touching_triangles = [this](int facet_idx, const Vec3i32 &neighbors, const Vec3i32 &neighbors_propagated) -> std::vector<int> {
        assert(facet_idx != -1 && facet_idx < int(m_triangles.size()));
        assert(this->verify_triangle_neighbors(m_triangles[facet_idx], neighbors));
        std::vector<int> touching_triangles;
        Vec3i32            vertices = {m_triangles[facet_idx].verts_idxs[0], m_triangles[facet_idx].verts_idxs[1], m_triangles[facet_idx].verts_idxs[2]};
        append_touching_subtriangles(neighbors(0), vertices(1), vertices(0), touching_triangles);
        append_touching_subtriangles(neighbors(1), vertices(2), vertices(1), touching_triangles);
        append_touching_subtriangles(neighbors(2), vertices(0), vertices(2), touching_triangles);

        for (int neighbor_idx : neighbors_propagated)
            if (neighbor_idx != -1 && !m_triangles[neighbor_idx].is_split())
                touching_triangles.emplace_back(neighbor_idx);

        return touching_triangles;
    };

    auto [neighbors, neighbors_propagated] = this->precompute_all_neighbors();
    std::vector<bool>  visited(m_triangles.size(), false);
    std::queue<int>    facet_queue;

    facet_queue.push(start_facet_idx);
    while (!facet_queue.empty()) {
        int current_facet = facet_queue.front();
        facet_queue.pop();
        assert(!m_triangles[current_facet].is_split());

        if (!visited[current_facet]) {
            m_triangles[current_facet].select_by_seed_fill();

            std::vector<int> touching_triangles = get_all_touching_triangles(current_facet, neighbors[current_facet], neighbors_propagated[current_facet]);
            for(const int tr_idx : touching_triangles) {
                if (tr_idx < 0 || visited[tr_idx] || m_triangles[tr_idx].get_state() != start_facet_state || is_facet_clipped(tr_idx, clp))
                    continue;

                const Vec3f& n1 = m_face_normals[m_triangles[tr_idx].source_triangle];
                const Vec3f& n2 = m_face_normals[m_triangles[current_facet].source_triangle];
                if (seed_fill_angle >= -EPSILON && std::clamp(n1.dot(n2), 0.f, 1.f) < facet_angle_limit)
                    continue;

                assert(!m_triangles[tr_idx].is_split());
                facet_queue.push(tr_idx);
            }
        }

        visited[current_facet] = true;
    }
}

// Selects either the whole triangle (discarding any children it had), or divides
// the triangle recursively, selecting just subtriangles truly inside the circle.
// This is done by an actual recursive call. Returns false if the triangle is
// outside the cursor.
// Called by select_patch() and by itself.
bool TriangleSelector::select_triangle(int facet_idx, EnforcerBlockerType type, bool triangle_splitting)
{
    assert(facet_idx < int(m_triangles.size()));

    if (! m_triangles[facet_idx].valid())
        return false;

    Vec3i32 neighbors = m_neighbors[facet_idx];
    assert(this->verify_triangle_neighbors(m_triangles[facet_idx], neighbors));

    if (! select_triangle_recursive(facet_idx, neighbors, type, triangle_splitting))
        return false;

    // In case that all children are leafs and have the same state now,
    // they may be removed and substituted by the parent triangle.
    remove_useless_children(facet_idx);

#ifdef EXPENSIVE_DEBUG_CHECKS
    // Make sure that we did not lose track of invalid triangles.
    assert(m_invalid_triangles == std::count_if(m_triangles.begin(), m_triangles.end(),
               [](const Triangle& tr) { return ! tr.valid(); }));
#endif // EXPENSIVE_DEBUG_CHECKS

    // Do garbage collection maybe?
    if (2*m_invalid_triangles > int(m_triangles.size()))
        garbage_collect();

    return true;
}

// Return child of itriangle at a CCW oriented side (vertexi, vertexj), either first or 2nd part.
// If the side sharing (vertexi, vertexj) is not split, return -1.
int TriangleSelector::neighbor_child(const Triangle &tr, int vertexi, int vertexj, Partition partition) const
{
    if (tr.number_of_split_sides() == 0)
        // If this triangle is not split, then there is no upper / lower subtriangle sharing the edge.
        return -1;

    // Find the triangle edge.
    int edge = tr.verts_idxs[0] == vertexi ? 0 : tr.verts_idxs[1] == vertexi ? 1 : 2;
    assert(tr.verts_idxs[edge] == vertexi);
    assert(tr.verts_idxs[next_idx_modulo(edge, 3)] == vertexj);

    int child_idx;
    if (tr.number_of_split_sides() == 1) {
        if (edge != next_idx_modulo(tr.special_side(), 3))
            // A child may or may not be split at this side.
            return this->neighbor_child(m_triangles[tr.children[edge == tr.special_side() ? 0 : 1]], vertexi, vertexj, partition);
        child_idx = partition == Partition::First ? 0 : 1;
    } else if (tr.number_of_split_sides() == 2) {
        if (edge == next_idx_modulo(tr.special_side(), 3))
            // A child may or may not be split at this side.
            return this->neighbor_child(m_triangles[tr.children[2]], vertexi, vertexj, partition);
        child_idx = edge == tr.special_side() ?
            (partition == Partition::First ? 0 : 1) :
            (partition == Partition::First ? 2 : 0);
    } else {
        assert(tr.number_of_split_sides() == 3);
        assert(tr.special_side() == 0);
        switch(edge) {
        case 0:  child_idx = partition == Partition::First ? 0 : 1; break;
        case 1:  child_idx = partition == Partition::First ? 1 : 2; break;
        default: assert(edge == 2);
                 child_idx = partition == Partition::First ? 2 : 0; break;
        }
    }
    return tr.children[child_idx];
}

// Return child of itriangle at a CCW oriented side (vertexi, vertexj), either first or 2nd part.
// If itriangle == -1 or if the side sharing (vertexi, vertexj) is not split, return -1.
int TriangleSelector::neighbor_child(int itriangle, int vertexi, int vertexj, Partition partition) const
{
    return itriangle == -1 ? -1 : this->neighbor_child(m_triangles[itriangle], vertexi, vertexj, partition);
}

std::pair<int, int> TriangleSelector::triangle_subtriangles(int itriangle, int vertexi, int vertexj) const
{
    return itriangle == -1 ? std::make_pair(-1, -1) : Slic3r::TriangleSelector::triangle_subtriangles(m_triangles[itriangle], vertexi, vertexj);
}

std::pair<int, int> TriangleSelector::triangle_subtriangles(const Triangle &tr, int vertexi, int vertexj)
{
    if (tr.number_of_split_sides() == 0)
        // If this triangle is not split, then there is no subtriangles touching the edge.
        return std::make_pair(-1, -1);

    // Find the triangle edge.
    int edge = tr.verts_idxs[0] == vertexi ? 0 : tr.verts_idxs[1] == vertexi ? 1 : 2;
    assert(tr.verts_idxs[edge] == vertexi);
    assert(tr.verts_idxs[next_idx_modulo(edge, 3)] == vertexj);

    if (tr.number_of_split_sides() == 1) {
        return edge == next_idx_modulo(tr.special_side(), 3) ? std::make_pair(tr.children[0], tr.children[1]) :
                                                                     std::make_pair(tr.children[edge == tr.special_side() ? 0 : 1], -1);
    } else if (tr.number_of_split_sides() == 2) {
        return edge == next_idx_modulo(tr.special_side(), 3) ? std::make_pair(tr.children[2], -1) :
               edge == tr.special_side()                           ? std::make_pair(tr.children[0], tr.children[1]) :
                                                                     std::make_pair(tr.children[2], tr.children[0]);
    } else {
        assert(tr.number_of_split_sides() == 3);
        assert(tr.special_side() == 0);
        return edge == 0 ? std::make_pair(tr.children[0], tr.children[1]) :
               edge == 1 ? std::make_pair(tr.children[1], tr.children[2]) :
                           std::make_pair(tr.children[2], tr.children[0]);
    }

    return std::make_pair(-1, -1);
}

// Return existing midpoint of CCW oriented side (vertexi, vertexj).
// If itriangle == -1 or if the side sharing (vertexi, vertexj) is not split, return -1.
int TriangleSelector::triangle_midpoint(const Triangle &tr, int vertexi, int vertexj) const
{
    if (tr.number_of_split_sides() == 0)
        // If this triangle is not split, then there is no upper / lower subtriangle sharing the edge.
        return -1;

    // Find the triangle edge.
    int edge = tr.verts_idxs[0] == vertexi ? 0 : tr.verts_idxs[1] == vertexi ? 1 : 2;
    assert(tr.verts_idxs[edge] == vertexi);
    assert(tr.verts_idxs[next_idx_modulo(edge, 3)] == vertexj);

    if (tr.number_of_split_sides() == 1) {
        return edge == next_idx_modulo(tr.special_side(), 3) ?
            m_triangles[tr.children[0]].verts_idxs[2] :
            this->triangle_midpoint(m_triangles[tr.children[edge == tr.special_side() ? 0 : 1]], vertexi, vertexj);
    } else if (tr.number_of_split_sides() == 2) {
        return edge == next_idx_modulo(tr.special_side(), 3) ?
                    this->triangle_midpoint(m_triangles[tr.children[2]], vertexi, vertexj) :
               edge == tr.special_side() ?
                    m_triangles[tr.children[0]].verts_idxs[1] :
                    m_triangles[tr.children[1]].verts_idxs[2];
    } else {
        assert(tr.number_of_split_sides() == 3);
        assert(tr.special_side() == 0);
        return
            (edge == 0) ? m_triangles[tr.children[0]].verts_idxs[1] :
            (edge == 1) ? m_triangles[tr.children[1]].verts_idxs[2] :
                          m_triangles[tr.children[2]].verts_idxs[2];
    }
}

// Return existing midpoint of CCW oriented side (vertexi, vertexj).
// If itriangle == -1 or if the side sharing (vertexi, vertexj) is not split, return -1.
int TriangleSelector::triangle_midpoint(int itriangle, int vertexi, int vertexj) const
{
    return itriangle == -1 ? -1 : this->triangle_midpoint(m_triangles[itriangle], vertexi, vertexj);
}

int TriangleSelector::triangle_midpoint_or_allocate(int itriangle, int vertexi, int vertexj)
{
    int midpoint = this->triangle_midpoint(itriangle, vertexi, vertexj);
    if (midpoint == -1) {
        Vec3f c = 0.5f * (m_vertices[vertexi].v + m_vertices[vertexj].v);
#ifdef EXPENSIVE_DEBUG_CHECKS
        // Verify that the vertex is really a new one.
        auto it = std::find_if(m_vertices.begin(), m_vertices.end(), [c](const Vertex &v) {
            return v.ref_cnt > 0 && (v.v - c).norm() < EPSILON; });
        assert(it == m_vertices.end());
#endif // EXPENSIVE_DEBUG_CHECKS
        // Allocate a new vertex, possibly reusing the free list.
        if (m_free_vertices_head == -1) {
            // Allocate a new vertex.
            midpoint = int(m_vertices.size());
            m_vertices.emplace_back(c);
        } else {
            // Reuse a vertex from the free list.
            assert(m_free_vertices_head >= -1 && m_free_vertices_head < int(m_vertices.size()));
            midpoint = m_free_vertices_head;
            memcpy(&m_free_vertices_head, &m_vertices[midpoint].v[0], sizeof(m_free_vertices_head));
            assert(m_free_vertices_head >= -1 && m_free_vertices_head < int(m_vertices.size()));
            m_vertices[midpoint].v = c;
        }
        assert(m_vertices[midpoint].ref_cnt == 0);
    } else {
#ifndef NDEBUG
        Vec3f c1 = 0.5f * (m_vertices[vertexi].v + m_vertices[vertexj].v);
        Vec3f c2 = m_vertices[midpoint].v;
        float d = (c2 - c1).norm();
        assert(std::abs(d) < EPSILON);
#endif // NDEBUG
        assert(m_vertices[midpoint].ref_cnt > 0);
    }
    return midpoint;
}

// Return neighbors of ith child of a triangle given neighbors of the triangle.
// Returns -1 if such a neighbor does not exist at all, or it does not exist
// at the same depth as the ith child.
// Using the same splitting strategy as TriangleSelector::split_triangle()
Vec3i32 TriangleSelector::child_neighbors(const Triangle &tr, const Vec3i32 &neighbors, int child_idx) const
{
    assert(this->verify_triangle_neighbors(tr, neighbors));

    assert(child_idx >= 0 && child_idx <= tr.number_of_split_sides());
    int   i = tr.special_side();
    int   j = next_idx_modulo(i, 3);
    int   k = next_idx_modulo(j, 3);

    Vec3i32 out;
    switch (tr.number_of_split_sides()) {
    case 1:
        switch (child_idx) {
        case 0:
            out(0) = neighbors(i);
            out(1) = this->neighbor_child(neighbors(j), tr.verts_idxs[k], tr.verts_idxs[j], Partition::Second);
            out(2) = tr.children[1];
            break;
        default:
            assert(child_idx == 1);
            out(0) = this->neighbor_child(neighbors(j), tr.verts_idxs[k], tr.verts_idxs[j], Partition::First);
            out(1) = neighbors(k);
            out(2) = tr.children[0];
            break;
        }
        break;

    case 2:
        switch (child_idx) {
        case 0:
            out(0) = this->neighbor_child(neighbors(i), tr.verts_idxs[j], tr.verts_idxs[i], Partition::Second);
            out(1) = tr.children[1];
            out(2) = this->neighbor_child(neighbors(k), tr.verts_idxs[i], tr.verts_idxs[k], Partition::First);
            break;
        case 1:
            assert(child_idx == 1);
            out(0) = this->neighbor_child(neighbors(i), tr.verts_idxs[j], tr.verts_idxs[i], Partition::First);
            out(1) = tr.children[2];
            out(2) = tr.children[0];
            break;
        default:
            assert(child_idx == 2);
            out(0) = neighbors(j);
            out(1) = this->neighbor_child(neighbors(k), tr.verts_idxs[i], tr.verts_idxs[k], Partition::Second);
            out(2) = tr.children[1];
            break;
        }
        break;

    case 3:
        assert(tr.special_side() == 0);
        switch (child_idx) {
        case 0:
            out(0) = this->neighbor_child(neighbors(0), tr.verts_idxs[1], tr.verts_idxs[0], Partition::Second);
            out(1) = tr.children[3];
            out(2) = this->neighbor_child(neighbors(2), tr.verts_idxs[0], tr.verts_idxs[2], Partition::First);
            break;
        case 1:
            out(0) = this->neighbor_child(neighbors(0), tr.verts_idxs[1], tr.verts_idxs[0], Partition::First);
            out(1) = this->neighbor_child(neighbors(1), tr.verts_idxs[2], tr.verts_idxs[1], Partition::Second);
            out(2) = tr.children[3];
            break;
        case 2:
            out(0) = this->neighbor_child(neighbors(1), tr.verts_idxs[2], tr.verts_idxs[1], Partition::First);
            out(1) = this->neighbor_child(neighbors(2), tr.verts_idxs[0], tr.verts_idxs[2], Partition::Second);
            out(2) = tr.children[3];
            break;
        default:
            assert(child_idx == 3);
            out(0) = tr.children[1];
            out(1) = tr.children[2];
            out(2) = tr.children[0];
            break;
        }
        break;

    default:
        assert(false);
    }

    assert(this->verify_triangle_neighbors(tr, neighbors));
    assert(this->verify_triangle_neighbors(m_triangles[tr.children[child_idx]], out));
    return out;
}

// Return neighbors of the ith child of a triangle given neighbors of the triangle.
// If such a neighbor doesn't exist, return the neighbor from the previous depth.
Vec3i32 TriangleSelector::child_neighbors_propagated(const Triangle &tr, const Vec3i32 &neighbors_propagated, int child_idx, const Vec3i32 &child_neighbors) const
{
    int i = tr.special_side();
    int j = next_idx_modulo(i, 3);
    int k = next_idx_modulo(j, 3);

    Vec3i32 out = child_neighbors;
    auto  replace_if_not_exists = [&out, &neighbors_propagated](int index_to_replace, int neighbor_idx) {
        if (out(index_to_replace) == -1)
            out(index_to_replace) = neighbors_propagated(neighbor_idx);
    };

    switch (tr.number_of_split_sides()) {
    case 1:
        switch (child_idx) {
        case 0:
            replace_if_not_exists(0, i);
            replace_if_not_exists(1, j);
            break;
        default:
            assert(child_idx == 1);
            replace_if_not_exists(0, j);
            replace_if_not_exists(1, k);
            break;
        }
        break;

    case 2:
        switch (child_idx) {
        case 0:
            replace_if_not_exists(0, i);
            replace_if_not_exists(2, k);
            break;
        case 1:
            assert(child_idx == 1);
            replace_if_not_exists(0, i);
            break;
        default:
            assert(child_idx == 2);
            replace_if_not_exists(0, j);
            replace_if_not_exists(1, k);
            break;
        }
        break;

    case 3:
        assert(tr.special_side() == 0);
        switch (child_idx) {
        case 0:
            replace_if_not_exists(0, 0);
            replace_if_not_exists(2, 2);
            break;
        case 1:
            replace_if_not_exists(0, 0);
            replace_if_not_exists(1, 1);
            break;
        case 2:
            replace_if_not_exists(0, 1);
            replace_if_not_exists(1, 2);
            break;
        default:
            assert(child_idx == 3);
            break;
        }
        break;

    default: assert(false);
    }

    return out;
}

bool TriangleSelector::select_triangle_recursive(int facet_idx, const Vec3i32 &neighbors, EnforcerBlockerType type, bool triangle_splitting)
{
    assert(facet_idx < int(m_triangles.size()));

    Triangle* tr = &m_triangles[facet_idx];
    if (! tr->valid())
        return false;

    assert(this->verify_triangle_neighbors(*tr, neighbors));

    int num_of_inside_vertices = m_cursor->vertices_inside(*tr, m_vertices);

    if (num_of_inside_vertices == 0
     && ! m_cursor->is_pointer_in_triangle(*tr, m_vertices)
     && ! m_cursor->is_edge_inside_cursor(*tr, m_vertices))
        return false;

    if (num_of_inside_vertices == 3) {
        // dump any subdivision and select whole triangle
        undivide_triangle(facet_idx);
        tr->set_state(type);
    } else {
        // the triangle is partially inside, let's recursively divide it
        // (if not already) and try selecting its children.

        if (! tr->is_split() && tr->get_state() == type) {
            // This is leaf triangle that is already of correct type as a whole.
            // No need to split, all children would end up selected anyway.
            return true;
        }

        if (triangle_splitting)
            split_triangle(facet_idx, neighbors);
        else if (!m_triangles[facet_idx].is_split())
            m_triangles[facet_idx].set_state(type);
        tr = &m_triangles[facet_idx]; // might have been invalidated by split_triangle().

        int num_of_children = tr->number_of_split_sides() + 1;
        if (num_of_children != 1) {
            for (int i=0; i<num_of_children; ++i) {
                assert(i < int(tr->children.size()));
                assert(tr->children[i] < int(m_triangles.size()));
                // Recursion, deep first search over the children of this triangle.
                // All children of this triangle were created by splitting a single source triangle of the original mesh.
                select_triangle_recursive(tr->children[i], this->child_neighbors(*tr, neighbors, i), type, triangle_splitting);
                tr = &m_triangles[facet_idx]; // might have been invalidated
            }
        }
    }

    return true;
}

void TriangleSelector::set_facet(int facet_idx, EnforcerBlockerType state)
{
    assert(facet_idx < m_orig_size_indices);
    undivide_triangle(facet_idx);
    assert(! m_triangles[facet_idx].is_split());
    m_triangles[facet_idx].set_state(state);
}

// called by select_patch()->select_triangle()...select_triangle()
// to decide which sides of the triangle to split and to actually split it calling set_division() and perform_split().
void TriangleSelector::split_triangle(int facet_idx, const Vec3i32 &neighbors)
{
    if (m_triangles[facet_idx].is_split()) {
        // The triangle is divided already.
        return;
    }

    Triangle* tr = &m_triangles[facet_idx];
    assert(this->verify_triangle_neighbors(*tr, neighbors));

    EnforcerBlockerType old_type = tr->get_state();

    // If we got here, we are about to actually split the triangle.
    const double limit_squared = m_edge_limit_sqr;

    std::array<int, 3>& facet = tr->verts_idxs;
    std::array<const stl_vertex*, 3> pts = { &m_vertices[facet[0]].v,
                                             &m_vertices[facet[1]].v,
                                             &m_vertices[facet[2]].v};
    std::array<stl_vertex, 3> pts_transformed; // must stay in scope of pts !!!

    // In case the object is non-uniformly scaled, transform the
    // points to world coords.
    if (! m_cursor->uniform_scaling) {
        for (size_t i=0; i<pts.size(); ++i) {
            pts_transformed[i] = m_cursor->trafo * (*pts[i]);
            pts[i] = &pts_transformed[i];
        }
    }

    std::array<double, 3> sides = {(*pts[2] - *pts[1]).squaredNorm(),
                                   (*pts[0] - *pts[2]).squaredNorm(),
                                   (*pts[1] - *pts[0]).squaredNorm()};

    boost::container::small_vector<int, 3> sides_to_split;
    int side_to_keep = -1;
    for (int pt_idx = 0; pt_idx<3; ++pt_idx) {
        if (sides[pt_idx] > limit_squared)
            sides_to_split.push_back(pt_idx);
        else
            side_to_keep = pt_idx;
    }
    if (sides_to_split.empty()) {
        // This shall be unselected.
        tr->set_division(0, 0);
        return;
    }

    // Save how the triangle will be split. Second argument makes sense only for one
    // or two split sides, otherwise the value is ignored.
    tr->set_division(int(sides_to_split.size()),
        sides_to_split.size() == 2 ? side_to_keep : sides_to_split[0]);

    perform_split(facet_idx, neighbors, old_type);
}

// Is pointer in a triangle?
bool TriangleSelector::Cursor::is_pointer_in_triangle(const Triangle &tr, const std::vector<Vertex> &vertices) const {
    const Vec3f& p1 = vertices[tr.verts_idxs[0]].v;
    const Vec3f& p2 = vertices[tr.verts_idxs[1]].v;
    const Vec3f& p3 = vertices[tr.verts_idxs[2]].v;
    return this->is_pointer_in_triangle(p1, p2, p3);
}

// Determine whether this facet is potentially visible (still can be obscured).
bool TriangleSelector::Cursor::is_facet_visible(const Cursor &cursor, int facet_idx, const std::vector<Vec3f> &face_normals)
{
    assert(facet_idx < int(face_normals.size()));
    Vec3f n = face_normals[facet_idx];
    if (!cursor.uniform_scaling)
        n = cursor.trafo_normal * n;
    return n.dot(cursor.dir) < 0.f;
}

// How many vertices of a triangle are inside the circle?
int TriangleSelector::Cursor::vertices_inside(const Triangle &tr, const std::vector<Vertex> &vertices) const
{
    int inside = 0;
    for (size_t i = 0; i < 3; ++i)
        if (this->is_mesh_point_inside(vertices[tr.verts_idxs[i]].v))
            ++inside;

    return inside;
}

// Is any edge inside Sphere cursor?
bool TriangleSelector::Sphere::is_edge_inside_cursor(const Triangle &tr, const std::vector<Vertex> &vertices) const
{
    std::array<Vec3f, 3> pts;
    for (int i = 0; i < 3; ++i) {
        pts[i] = vertices[tr.verts_idxs[i]].v;
        if (!this->uniform_scaling)
            pts[i] = this->trafo * pts[i];
    }

    for (int side = 0; side < 3; ++side) {
        const Vec3f &edge_a = pts[side];
        const Vec3f &edge_b = pts[side < 2 ? side + 1 : 0];
        if (test_line_inside_sphere(edge_a, edge_b, this->center, this->radius))
            return true;
    }
    return false;
}

// Is edge inside cursor?
bool TriangleSelector::Circle::is_edge_inside_cursor(const Triangle &tr, const std::vector<Vertex> &vertices) const
{
    std::array<Vec3f, 3> pts;
    for (int i = 0; i < 3; ++i) {
        pts[i] = vertices[tr.verts_idxs[i]].v;
        if (!this->uniform_scaling)
            pts[i] = this->trafo * pts[i];
    }

    const Vec3f &p = this->center;
    for (int side = 0; side < 3; ++side) {
        const Vec3f &a      = pts[side];
        const Vec3f &b      = pts[side < 2 ? side + 1 : 0];
        Vec3f        s      = (b - a).normalized();
        float        t      = (p - a).dot(s);
        Vec3f        vector = a + t * s - p;

        // vector is 3D vector from center to the intersection. What we want to
        // measure is length of its projection onto plane perpendicular to dir.
        float dist_sqr = vector.squaredNorm() - std::pow(vector.dot(this->dir), 2.f);
        if (dist_sqr < this->radius_sqr && t >= 0.f && t <= (b - a).norm())
            return true;
    }
    return false;
}

// BBS
bool TriangleSelector::HeightRange::is_pointer_in_triangle(const Vec3f& p1_, const Vec3f& p2_, const Vec3f& p3_) const
{
    return false;
}

bool TriangleSelector::HeightRange::is_mesh_point_inside(const Vec3f& point) const
{
    // just use 40% edge limit as tolerance
    const float tolerance = 0.02;
    const Vec3f transformed_point = trafo * point;
    float top_z = m_z_world + m_height + tolerance;
    float bot_z = m_z_world - tolerance;

    return transformed_point.z() > bot_z && transformed_point.z() < top_z;
}

bool TriangleSelector::HeightRange::is_edge_inside_cursor(const Triangle& tr, const std::vector<Vertex>& vertices) const
{
    float top_z = m_z_world + m_height + EPSILON;
    float bot_z = m_z_world - EPSILON;
    std::array<Vec3f, 3> pts;
    for (int i = 0; i < 3; ++i) {
        pts[i] = vertices[tr.verts_idxs[i]].v;
        pts[i] = this->trafo * pts[i];
    }

    return !((pts[0].z() < bot_z && pts[1].z() < bot_z && pts[2].z() < bot_z) ||
             (pts[0].z() > top_z && pts[1].z() > top_z && pts[2].z() > top_z));
}

// Recursively remove all subtriangles.
void TriangleSelector::undivide_triangle(int facet_idx)
{
    assert(facet_idx < int(m_triangles.size()));
    Triangle& tr = m_triangles[facet_idx];

    if (tr.is_split()) {
        for (int i = 0; i <= tr.number_of_split_sides(); ++i) {
            int       child    = tr.children[i];
            Triangle &child_tr = m_triangles[child];
            assert(child_tr.valid());
            undivide_triangle(child);
            for (int j = 0; j < 3; ++j) {
                int     iv = child_tr.verts_idxs[j];
                Vertex &v  = m_vertices[iv];
                assert(v.ref_cnt > 0);
                if (-- v.ref_cnt == 0) {
                    // Release this vertex.
                    // Chain released vertices into a linked list through ref_cnt.
                    assert(m_free_vertices_head >= -1 && m_free_vertices_head < int(m_vertices.size()));
                    memcpy(&m_vertices[iv].v[0], &m_free_vertices_head, sizeof(m_free_vertices_head));
                    m_free_vertices_head = iv;
                    assert(m_free_vertices_head >= -1 && m_free_vertices_head < int(m_vertices.size()));
                }
            }
            // Chain released triangles into a linked list through children[0].
            assert(child_tr.valid());
            child_tr.m_valid = false;
            assert(m_free_triangles_head >= -1 && m_free_triangles_head < int(m_triangles.size()));
            assert(m_free_triangles_head == -1 || ! m_triangles[m_free_triangles_head].valid());
            child_tr.children[0] = m_free_triangles_head;
            m_free_triangles_head = child;
            assert(m_free_triangles_head >= -1 && m_free_triangles_head < int(m_triangles.size()));
            ++m_invalid_triangles;
        }
        tr.set_division(0, 0); // not split
    }
}

void TriangleSelector::remove_useless_children(int facet_idx)
{
    // Check that all children are leafs of the same type. If not, try to
    // make them (recursive call). Remove them if sucessful.

    assert(facet_idx < int(m_triangles.size()) && m_triangles[facet_idx].valid());
    Triangle& tr = m_triangles[facet_idx];

    if (! tr.is_split()) {
        // This is a leaf, there nothing to do. This can happen during the
        // first (non-recursive call). Shouldn't otherwise.
        return;
    }

    // Call this for all non-leaf children.
    for (int child_idx=0; child_idx<=tr.number_of_split_sides(); ++child_idx) {
        assert(child_idx < int(m_triangles.size()) && m_triangles[child_idx].valid());
        if (m_triangles[tr.children[child_idx]].is_split())
            remove_useless_children(tr.children[child_idx]);
    }


    // Return if a child is not leaf or two children differ in type.
    EnforcerBlockerType first_child_type = EnforcerBlockerType::NONE;
    for (int child_idx=0; child_idx<=tr.number_of_split_sides(); ++child_idx) {
        if (m_triangles[tr.children[child_idx]].is_split())
            return;
        if (child_idx == 0)
            first_child_type = m_triangles[tr.children[0]].get_state();
        else if (m_triangles[tr.children[child_idx]].get_state() != first_child_type)
            return;
    }

    // If we got here, the children can be removed.
    undivide_triangle(facet_idx);
    tr.set_state(first_child_type);
}

void TriangleSelector::garbage_collect()
{
    // First make a map from old to new triangle indices.
    int new_idx = m_orig_size_indices;
    std::vector<int> new_triangle_indices(m_triangles.size(), -1);
    for (int i = m_orig_size_indices; i<int(m_triangles.size()); ++i)
        if (m_triangles[i].valid())
            new_triangle_indices[i] = new_idx ++;

    // Now we know which vertices are not referenced anymore. Make a map
    // from old idxs to new ones, like we did for triangles.
    new_idx = m_orig_size_vertices;
    std::vector<int> new_vertices_indices(m_vertices.size(), -1);
    for (int i=m_orig_size_vertices; i<int(m_vertices.size()); ++i) {
        assert(m_vertices[i].ref_cnt >= 0);
        if (m_vertices[i].ref_cnt != 0)
            new_vertices_indices[i] = new_idx ++;
    }

    // We can remove all invalid triangles and vertices that are no longer referenced.
    m_triangles.erase(std::remove_if(m_triangles.begin()+m_orig_size_indices, m_triangles.end(),
                          [](const Triangle& tr) { return ! tr.valid(); }),
                      m_triangles.end());
    m_vertices.erase(std::remove_if(m_vertices.begin()+m_orig_size_vertices, m_vertices.end(),
                          [](const Vertex& vert) { return vert.ref_cnt == 0; }),
                      m_vertices.end());

    // Now go through all remaining triangles and update changed indices.
    for (Triangle& tr : m_triangles) {
        assert(tr.valid());

        if (tr.is_split()) {
            // There are children. Update their indices.
            for (int j=0; j<=tr.number_of_split_sides(); ++j) {
                assert(new_triangle_indices[tr.children[j]] != -1);
                tr.children[j] = new_triangle_indices[tr.children[j]];
            }
        }

        // Update indices into m_vertices. The original vertices are never
        // touched and need not be reindexed.
        for (int& idx : tr.verts_idxs) {
            if (idx >= m_orig_size_vertices) {
                assert(new_vertices_indices[idx] != -1);
                idx = new_vertices_indices[idx];
            }
        }
    }

    m_invalid_triangles = 0;
    m_free_triangles_head = -1;
    m_free_vertices_head = -1;
}

void TriangleSelector::remap_triangle_state(const EnforcerBlockerStateMap& state_map)
{
    if (m_triangles.empty())
        return;

    tbb::parallel_for(tbb::blocked_range<size_t>(0, m_triangles.size()), [this, &state_map](const tbb::blocked_range<size_t>& range) {
        for (size_t i = range.begin(); i != range.end(); ++i) {
            Triangle& tr = m_triangles[i];
            if (tr.valid()) {
                const auto current_state = static_cast<size_t>(tr.get_state());
                tr.set_state(state_map[current_state]);
            }
        }
    });
}

TriangleSelector::TriangleSelector(const TriangleMesh& mesh, float edge_limit)
    : m_mesh{mesh}, m_neighbors(its_face_neighbors(mesh.its)), m_face_normals(its_face_normals(mesh.its)), m_edge_limit(edge_limit)
{
    reset();
}

void TriangleSelector::reset()
{
    m_vertices.clear();
    m_triangles.clear();
    m_invalid_triangles = 0;
    m_free_triangles_head = -1;
    m_free_vertices_head = -1;
    m_vertices.reserve(m_mesh.its.vertices.size());
    for (const stl_vertex& vert : m_mesh.its.vertices)
        m_vertices.emplace_back(vert);
    m_triangles.reserve(m_mesh.its.indices.size());
    for (size_t i = 0; i < m_mesh.its.indices.size(); ++i) {
        const stl_triangle_vertex_indices &ind = m_mesh.its.indices[i];
        push_triangle(ind[0], ind[1], ind[2], int(i));
    }
    m_orig_size_vertices = int(m_vertices.size());
    m_orig_size_indices  = int(m_triangles.size());
}

void TriangleSelector::set_edge_limit(float edge_limit)
{
    m_edge_limit_sqr = std::pow(edge_limit, 2.f);
}

int TriangleSelector::push_triangle(int a, int b, int c, int source_triangle, const EnforcerBlockerType state)
{
    for (int i : {a, b, c}) {
        assert(i >= 0 && i < int(m_vertices.size()));
        ++m_vertices[i].ref_cnt;
    }
    int idx;
    if (m_free_triangles_head == -1) {
        // Allocate a new triangle.
        assert(m_invalid_triangles == 0);
        idx = int(m_triangles.size());
        m_triangles.emplace_back(a, b, c, source_triangle, state);
    } else {
        // Reuse triangle from the free list.
        assert(m_free_triangles_head >= -1 && m_free_triangles_head < int(m_triangles.size()));
        assert(! m_triangles[m_free_triangles_head].valid());
        assert(m_invalid_triangles > 0);
        idx = m_free_triangles_head;
        m_free_triangles_head = m_triangles[idx].children[0];
        -- m_invalid_triangles;
        assert(m_free_triangles_head >= -1 && m_free_triangles_head < int(m_triangles.size()));
        assert(m_free_triangles_head == -1 || ! m_triangles[m_free_triangles_head].valid());
        assert(m_invalid_triangles >= 0);
        assert((m_invalid_triangles == 0) == (m_free_triangles_head == -1));
        m_triangles[idx] = {a, b, c, source_triangle, state};
    }
    assert(m_triangles[idx].valid());
    return idx;
}

// called by deserialize() and select_patch()->select_triangle()->...select_triangle()->split_triangle()
// Split a triangle based on Triangle::number_of_split_sides() and Triangle::special_side()
// by allocating child triangles and midpoint vertices.
// Midpoint vertices are possibly reused by traversing children of neighbor triangles.
void TriangleSelector::perform_split(int facet_idx, const Vec3i32 &neighbors, EnforcerBlockerType old_state)
{
    // Reserve space for the new triangles upfront, so that the reference to this triangle will not change.
    {
        size_t num_triangles_new = m_triangles.size() + m_triangles[facet_idx].number_of_split_sides() + 1;
        if (m_triangles.capacity() < num_triangles_new)
            m_triangles.reserve(next_highest_power_of_2(num_triangles_new));
    }

    Triangle &tr = m_triangles[facet_idx];
    assert(tr.is_split());

    // indices of triangle vertices
#ifdef NDEBUG
    boost::container::small_vector<int, 6> verts_idxs;
#else // NDEBUG
    // For easier debugging.
    std::vector<int> verts_idxs;
    verts_idxs.reserve(6);
#endif // NDEBUG
    for (int j=0, idx = tr.special_side(); j<3; ++j, idx = next_idx_modulo(idx, 3))
        verts_idxs.push_back(tr.verts_idxs[idx]);

    auto get_alloc_vertex = [this, &neighbors, &verts_idxs](int edge, int i1, int i2) -> int {
        return this->triangle_midpoint_or_allocate(neighbors(edge), verts_idxs[i1], verts_idxs[i2]);
    };

    int ichild = 0;
    switch (tr.number_of_split_sides()) {
    case 1:
        verts_idxs.insert(verts_idxs.begin()+2, get_alloc_vertex(next_idx_modulo(tr.special_side(), 3), 2, 1));
        tr.children[ichild ++] = push_triangle(verts_idxs[0], verts_idxs[1], verts_idxs[2], tr.source_triangle, old_state);
        tr.children[ichild   ] = push_triangle(verts_idxs[2], verts_idxs[3], verts_idxs[0], tr.source_triangle, old_state);
        break;

    case 2:
        verts_idxs.insert(verts_idxs.begin()+1, get_alloc_vertex(tr.special_side(), 1, 0));
        verts_idxs.insert(verts_idxs.begin()+4, get_alloc_vertex(prev_idx_modulo(tr.special_side(), 3), 0, 3));
        tr.children[ichild ++] = push_triangle(verts_idxs[0], verts_idxs[1], verts_idxs[4], tr.source_triangle, old_state);
        tr.children[ichild ++] = push_triangle(verts_idxs[1], verts_idxs[2], verts_idxs[4], tr.source_triangle, old_state);
        tr.children[ichild   ] = push_triangle(verts_idxs[2], verts_idxs[3], verts_idxs[4], tr.source_triangle, old_state);
        break;

    case 3:
        assert(tr.special_side() == 0);
        verts_idxs.insert(verts_idxs.begin()+1, get_alloc_vertex(0, 1, 0));
        verts_idxs.insert(verts_idxs.begin()+3, get_alloc_vertex(1, 3, 2));
        verts_idxs.insert(verts_idxs.begin()+5, get_alloc_vertex(2, 0, 4));
        tr.children[ichild ++] = push_triangle(verts_idxs[0], verts_idxs[1], verts_idxs[5], tr.source_triangle, old_state);
        tr.children[ichild ++] = push_triangle(verts_idxs[1], verts_idxs[2], verts_idxs[3], tr.source_triangle, old_state);
        tr.children[ichild ++] = push_triangle(verts_idxs[3], verts_idxs[4], verts_idxs[5], tr.source_triangle, old_state);
        tr.children[ichild   ] = push_triangle(verts_idxs[1], verts_idxs[3], verts_idxs[5], tr.source_triangle, old_state);
        break;

    default:
        break;
    }

#ifndef NDEBUG
    assert(this->verify_triangle_neighbors(tr, neighbors));
    for (int i = 0; i <= tr.number_of_split_sides(); ++i) {
        Vec3i32 n = this->child_neighbors(tr, neighbors, i);
        assert(this->verify_triangle_neighbors(m_triangles[tr.children[i]], n));
    }
#endif // NDEBUG
}

bool TriangleSelector::has_facets(EnforcerBlockerType state) const
{
    for (const Triangle& tr : m_triangles)
        if (tr.valid() && ! tr.is_split() && tr.get_state() == state)
            return true;
    return false;
}

int TriangleSelector::num_facets(EnforcerBlockerType state) const
{
    int cnt = 0;
    for (const Triangle& tr : m_triangles)
        if (tr.valid() && ! tr.is_split() && tr.get_state() == state)
            ++ cnt;
    return cnt;
}

indexed_triangle_set TriangleSelector::get_facets(EnforcerBlockerType state) const
{
    indexed_triangle_set out;
    std::vector<int> vertex_map(m_vertices.size(), -1);
    for (const Triangle& tr : m_triangles) {
        if (tr.valid() && ! tr.is_split() && tr.get_state() == state) {
            stl_triangle_vertex_indices indices;
            for (int i=0; i<3; ++i) {
                int j = tr.verts_idxs[i];
                if (vertex_map[j] == -1) {
                    vertex_map[j] = int(out.vertices.size());
                    out.vertices.emplace_back(m_vertices[j].v);
                }
                indices[i] = vertex_map[j];
            }
            out.indices.emplace_back(indices);
        }
    }
    return out;
}

// BBS
void TriangleSelector::get_facets(std::vector<indexed_triangle_set>& facets_per_type) const
{
    facets_per_type.clear();

    for (int type = (int)EnforcerBlockerType::NONE; type <= (int)EnforcerBlockerType::ExtruderMax; type++) {
        facets_per_type.emplace_back();
        indexed_triangle_set& its = facets_per_type.back();
        std::vector<int> vertex_map(m_vertices.size(), -1);

        for (const Triangle& tr : m_triangles) {
            if (tr.valid() && !tr.is_split() && tr.get_state() == (EnforcerBlockerType)type) {
                stl_triangle_vertex_indices indices;
                for (int i = 0; i < 3; ++i) {
                    int j = tr.verts_idxs[i];
                    if (vertex_map[j] == -1) {
                        vertex_map[j] = int(its.vertices.size());
                        its.vertices.emplace_back(m_vertices[j].v);
                    }
                    indices[i] = vertex_map[j];
                }
                its.indices.emplace_back(indices);
            }
        }
    }
}

indexed_triangle_set TriangleSelector::get_facets_strict(EnforcerBlockerType state) const
{
    indexed_triangle_set out;

    size_t num_vertices = 0;
    for (const Vertex &v : m_vertices)
        if (v.ref_cnt > 0)
            ++ num_vertices;
    out.vertices.reserve(num_vertices);
    std::vector<int> vertex_map(m_vertices.size(), -1);
    for (size_t i = 0; i < m_vertices.size(); ++ i)
        if (const Vertex &v = m_vertices[i]; v.ref_cnt > 0) {
            vertex_map[i] = int(out.vertices.size());
            out.vertices.emplace_back(v.v);
        }

    for (int itriangle = 0; itriangle < m_orig_size_indices; ++ itriangle)
        this->get_facets_strict_recursive(m_triangles[itriangle], m_neighbors[itriangle], state, out.indices);

    for (auto &triangle : out.indices)
        for (int i = 0; i < 3; ++ i)
            triangle(i) = vertex_map[triangle(i)];

    return out;
}

void TriangleSelector::get_facets_strict_recursive(
    const Triangle                              &tr,
    const Vec3i32                                 &neighbors,
    EnforcerBlockerType                          state,
    std::vector<stl_triangle_vertex_indices>    &out_triangles) const
{
    if (tr.is_split()) {
        for (int i = 0; i <= tr.number_of_split_sides(); ++ i)
            this->get_facets_strict_recursive(
                m_triangles[tr.children[i]],
                this->child_neighbors(tr, neighbors, i),
                state, out_triangles);
    } else if (tr.get_state() == state)
        this->get_facets_split_by_tjoints({tr.verts_idxs[0], tr.verts_idxs[1], tr.verts_idxs[2]}, neighbors, out_triangles);
}

void TriangleSelector::get_facets_split_by_tjoints(const Vec3i32 &vertices, const Vec3i32 &neighbors, std::vector<stl_triangle_vertex_indices> &out_triangles) const
{
// Export this triangle, but first collect the T-joint vertices along its edges.
    Vec3i32 midpoints(
        this->triangle_midpoint(neighbors(0), vertices(1), vertices(0)),
        this->triangle_midpoint(neighbors(1), vertices(2), vertices(1)),
        this->triangle_midpoint(neighbors(2), vertices(0), vertices(2)));
    int splits = (midpoints(0) != -1) + (midpoints(1) != -1) + (midpoints(2) != -1);
    switch (splits) {
    case 0:
        // Just emit this triangle.
        out_triangles.emplace_back(vertices(0), vertices(1), vertices(2));
        break;
    case 1:
    {
        // Split to two triangles
        int i = midpoints(0) != -1 ? 2 : midpoints(1) != -1 ? 0 : 1;
        int j = next_idx_modulo(i, 3);
        int k = next_idx_modulo(j, 3);
        this->get_facets_split_by_tjoints(
            { vertices(i), vertices(j), midpoints(j) },
            { neighbors(i),
              this->neighbor_child(neighbors(j), vertices(k), vertices(j), Partition::Second),
              -1 },
              out_triangles);
        this->get_facets_split_by_tjoints(
            { midpoints(j), vertices(k), vertices(i) },
            { this->neighbor_child(neighbors(j), vertices(k), vertices(j), Partition::First),
              neighbors(k),
              -1 },
              out_triangles);
        break;
    }
    case 2:
    {
        // Split to three triangles.
        int i = midpoints(0) == -1 ? 2 : midpoints(1) == -1 ? 0 : 1;
        int j = next_idx_modulo(i, 3);
        int k = next_idx_modulo(j, 3);
        this->get_facets_split_by_tjoints(
            { vertices(i), midpoints(i), midpoints(k) },
            { this->neighbor_child(neighbors(i), vertices(j), vertices(i), Partition::Second),
              -1,
              this->neighbor_child(neighbors(k), vertices(i), vertices(k), Partition::First) },
              out_triangles);
        this->get_facets_split_by_tjoints(
            { midpoints(i), vertices(j), midpoints(k) },
            { this->neighbor_child(neighbors(i), vertices(j), vertices(i), Partition::First),
              -1, -1 },
              out_triangles);
        this->get_facets_split_by_tjoints(
            { vertices(j), vertices(k), midpoints(k) },
            { neighbors(j),
              this->neighbor_child(neighbors(k), vertices(i), vertices(k), Partition::Second),
              -1 },
              out_triangles);
        break;
    }
    default:
        assert(splits == 3);
        // Split to 4 triangles.
        this->get_facets_split_by_tjoints(
            { vertices(0), midpoints(0), midpoints(2) },
            { this->neighbor_child(neighbors(0), vertices(1), vertices(0), Partition::Second),
              -1,
              this->neighbor_child(neighbors(2), vertices(0), vertices(2), Partition::First) },
              out_triangles);
        this->get_facets_split_by_tjoints(
            { midpoints(0), vertices(1), midpoints(1) },
            { this->neighbor_child(neighbors(0), vertices(1), vertices(0), Partition::First),
              this->neighbor_child(neighbors(1), vertices(2), vertices(1), Partition::Second),
              -1 },
              out_triangles);
        this->get_facets_split_by_tjoints(
            { midpoints(1), vertices(2), midpoints(2) },
            { this->neighbor_child(neighbors(1), vertices(2), vertices(1), Partition::First),
              this->neighbor_child(neighbors(2), vertices(0), vertices(2), Partition::Second),
              -1 },
              out_triangles);
        out_triangles.emplace_back(midpoints);
        break;
    }
}

std::vector<Vec2i32> TriangleSelector::get_seed_fill_contour() const {
    std::vector<Vec2i32> edges_out;
    for (int facet_idx = 0; facet_idx < this->m_orig_size_indices; ++facet_idx) {
        const Vec3i32 neighbors = m_neighbors[facet_idx];
        assert(this->verify_triangle_neighbors(m_triangles[facet_idx], neighbors));
        this->get_seed_fill_contour_recursive(facet_idx, neighbors, neighbors, edges_out);
    }

    return edges_out;
}

void TriangleSelector::get_seed_fill_contour_recursive(const int facet_idx, const Vec3i32 &neighbors, const Vec3i32 &neighbors_propagated, std::vector<Vec2i32> &edges_out) const {
    assert(facet_idx != -1 && facet_idx < int(m_triangles.size()));
    assert(this->verify_triangle_neighbors(m_triangles[facet_idx], neighbors));
    const Triangle *tr = &m_triangles[facet_idx];
    if (!tr->valid())
        return;

    if (tr->is_split()) {
        int num_of_children = tr->number_of_split_sides() + 1;
        if (num_of_children != 1) {
            for (int i = 0; i < num_of_children; ++i) {
                assert(i < int(tr->children.size()));
                assert(tr->children[i] < int(m_triangles.size()));
                // Recursion, deep first search over the children of this triangle.
                // All children of this triangle were created by splitting a single source triangle of the original mesh.
                const Vec3i32 child_neighbors = this->child_neighbors(*tr, neighbors, i);
                this->get_seed_fill_contour_recursive(tr->children[i], child_neighbors,
                                                      this->child_neighbors_propagated(*tr, neighbors_propagated, i, child_neighbors), edges_out);
            }
        }
    } else if (tr->is_selected_by_seed_fill()) {
        Vec3i32 vertices = {m_triangles[facet_idx].verts_idxs[0], m_triangles[facet_idx].verts_idxs[1], m_triangles[facet_idx].verts_idxs[2]};
        append_touching_edges(neighbors(0), vertices(1), vertices(0), edges_out);
        append_touching_edges(neighbors(1), vertices(2), vertices(1), edges_out);
        append_touching_edges(neighbors(2), vertices(0), vertices(2), edges_out);

        // It appends the edges that are touching the triangle only by part of the edge that means the triangles are from lower depth.
        for (int idx = 0; idx < 3; ++idx)
            if (int neighbor_tr_idx = neighbors_propagated(idx); neighbor_tr_idx != -1 && !m_triangles[neighbor_tr_idx].is_split() && !m_triangles[neighbor_tr_idx].is_selected_by_seed_fill())
                edges_out.emplace_back(vertices(idx), vertices(next_idx_modulo(idx, 3)));
    }
}

TriangleSelector::TriangleSplittingData TriangleSelector::serialize() const {
    // Each original triangle of the mesh is assigned a number encoding its state
    // or how it is split. Each triangle is encoded by 4 bits (xxyy) or 8 bits (zzzzxxyy):
    // leaf triangle: xx = EnforcerBlockerType (Only values 0, 1, and 2. Value 3 is used as an indicator for additional 4 bits.), yy = 0
    // leaf triangle: xx = 0b11, yy = 0b00, zzzz = EnforcerBlockerType (subtracted by 3)
    // non-leaf:      xx = special side, yy = number of split sides
    // These are bitwise appended and formed into one 64-bit integer.

    // The function returns a map from original triangle indices to
    // stream of bits encoding state and offsprings.

    // Using an explicit function object to support recursive call of Serializer::serialize().
    // This is cheaper than the previous implementation using a recursive call of type erased std::function.
    // (std::function calls using a pointer, while this implementation calls directly).
    struct Serializer {
        const TriangleSelector* triangle_selector;
        TriangleSplittingData data;

        void serialize(int facet_idx) {
            const Triangle& tr = triangle_selector->m_triangles[facet_idx];

            // Always save number of split sides. It is zero for unsplit triangles.
            int split_sides = tr.number_of_split_sides();
            assert(split_sides >= 0 && split_sides <= 3);

            data.bitstream.push_back(split_sides & 0b01);
            data.bitstream.push_back(split_sides & 0b10);

            if (split_sides) {
                // If this triangle is split, save which side is split (in case
                // of one split) or kept (in case of two splits). The value will
                // be ignored for 3-side split.
                assert(tr.is_split() && split_sides > 0);
                assert(tr.special_side() >= 0 && tr.special_side() <= 3);
                data.bitstream.push_back(tr.special_side() & 0b01);
                data.bitstream.push_back(tr.special_side() & 0b10);
                // Now save all children.
                // Serialized in reverse order for compatibility with PrusaSlicer 2.3.1.
                for (int child_idx = split_sides; child_idx >= 0; -- child_idx)
                    this->serialize(tr.children[child_idx]);
            } else {
                // In case this is leaf, we better save information about its state.
                int n = int(tr.get_state());
                if (n <= static_cast<size_t>(EnforcerBlockerType::ExtruderMax))
                    data.used_states[n] = true;

                if (n >= 3) {
                    assert(n <= 16);
                    if (n <= 16) {
                        // Store "11" plus 4 bits of (n-3).
                        data.bitstream.insert(data.bitstream.end(), { true, true });
                        n -= 3;
                        for (size_t bit_idx = 0; bit_idx < 4; ++bit_idx)
                            data.bitstream.push_back(n & (uint64_t(0b0001) << bit_idx));
                    }
                } else {
                    // Simple case, compatible with PrusaSlicer 2.3.1 and older for storing paint on supports and seams.
                    // Store 2 bits of n.
                    data.bitstream.push_back(n & 0b01);
                    data.bitstream.push_back(n & 0b10);
                }
            }
        }
    } out { this };

    out.data.triangles_to_split.reserve(m_orig_size_indices);
    for (int i=0; i<m_orig_size_indices; ++i)
        if (const Triangle& tr = m_triangles[i]; tr.is_split() || tr.get_state() != EnforcerBlockerType::NONE) {
            // Store index of the first bit assigned to ith triangle.
            out.data.triangles_to_split.emplace_back(i, int(out.data.bitstream.size()));
            // out the triangle bits.
            out.serialize(i);
        }

    // May be stored onto Undo / Redo stack, thus conserve memory.
    out.data.triangles_to_split.shrink_to_fit();
    out.data.bitstream.shrink_to_fit();
    return out.data;
}

void TriangleSelector::deserialize(const TriangleSplittingData &data,
                                   bool                         needs_reset,
                                   EnforcerBlockerType          max_ebt,
                                   EnforcerBlockerType          to_delete_filament,
                                   EnforcerBlockerType          replace_filament)
{
    if (needs_reset)
        reset(); // dump any current state
    for (auto [triangle_id, ibit] : data.triangles_to_split) {
        if (triangle_id >= int(m_triangles.size())) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "array bound:error:triangle_id >= int(m_triangles.size())";
            return;
        }
    }
    // Reserve number of triangles as if each triangle was saved with 4 bits.
    // With MMU painting this estimate may be somehow low, but better than nothing.
    m_triangles.reserve(std::max(m_mesh.its.indices.size(), data.bitstream.size() / 4));
    // Number of triangles is twice the number of vertices on a large manifold mesh of genus zero.
    // Here the triangles count account for both the nodes and leaves, thus the following line may overestimate.
    m_vertices.reserve(std::max(m_mesh.its.vertices.size(), m_triangles.size() / 2));

    // Vector to store all parents that have offsprings.
    struct ProcessingInfo {
        int facet_id = 0;
        Vec3i32 neighbors { -1, -1, -1 };
        int processed_children = 0;
        int total_children = 0;
    };
    // Depth-first queue of a source mesh triangle and its childern.
    // kept outside of the loop to avoid re-allocating inside the loop.
    std::vector<ProcessingInfo> parents;

    for (auto [triangle_id, ibit] : data.triangles_to_split) {
        assert(triangle_id < int(m_triangles.size()));
        assert(ibit < int(data.bitstream.size()));
        auto next_nibble = [&data, &ibit = ibit]() {
            int n = 0;
            for (int i = 0; i < 4; ++ i)
                n |= data.bitstream[ibit ++] << i;
            return n;
        };

        parents.clear();
        while (true) {
            // Read next triangle info.
            int code = next_nibble();
            int num_of_split_sides = code & 0b11;
            int num_of_children = num_of_split_sides == 0 ? 0 : num_of_split_sides + 1;
            bool is_split = num_of_children != 0;
            // Only valid if not is_split. Value of the second nibble was subtracted by 3, so it is added back.
            auto state = is_split ? EnforcerBlockerType::NONE : EnforcerBlockerType((code & 0b1100) == 0b1100 ? next_nibble() + 3 : code >> 2);

            // BBS
            if (state == to_delete_filament)
                state = replace_filament;
            else if (to_delete_filament != EnforcerBlockerType::NONE && state != EnforcerBlockerType::NONE) {
                state = state > to_delete_filament ? EnforcerBlockerType((int)state - 1) : state;
            }

            if (state > max_ebt) {
                assert(false);
                state = EnforcerBlockerType::NONE;
            }

            // Only valid if is_split.
            int special_side = code >> 2;

            // Take care of the first iteration separately, so handling of the others is simpler.
            if (parents.empty()) {
                if (is_split) {
                    // root is split, add it into list of parents and split it.
                    // then go to the next.
                    Vec3i32 neighbors = m_neighbors[triangle_id];
                    parents.push_back({triangle_id, neighbors, 0, num_of_children});
                    m_triangles[triangle_id].set_division(num_of_split_sides, special_side);
                    perform_split(triangle_id, neighbors, EnforcerBlockerType::NONE);
                    continue;
                } else {
                    // root is not split. just set the state and that's it.
                    m_triangles[triangle_id].set_state(state);
                    break;
                }
            }

            // This is not the first iteration. This triangle is a child of last seen parent.
            assert(! parents.empty());
            assert(parents.back().processed_children < parents.back().total_children);

            if (ProcessingInfo& last = parents.back();  is_split) {
                // split the triangle and save it as parent of the next ones.
                const Triangle &tr = m_triangles[last.facet_id];
                int   child_idx = last.total_children - last.processed_children - 1;
                Vec3i32 neighbors = this->child_neighbors(tr, last.neighbors, child_idx);
                int this_idx = tr.children[child_idx];
                m_triangles[this_idx].set_division(num_of_split_sides, special_side);
                perform_split(this_idx, neighbors, EnforcerBlockerType::NONE);
                parents.push_back({this_idx, neighbors, 0, num_of_children});
            } else {
                // this triangle belongs to last split one
                int child_idx = last.total_children - last.processed_children - 1;
                m_triangles[m_triangles[last.facet_id].children[child_idx]].set_state(state);
                ++last.processed_children;
            }

            // If all children of the past parent triangle are claimed, move to grandparent.
            while (parents.back().processed_children == parents.back().total_children) {
                parents.pop_back();

                if (parents.empty())
                    break;

                // And increment the grandparent children counter, because
                // we have just finished that branch and got back here.
                ++parents.back().processed_children;
            }

            // In case we popped back the root, we should be done.
            if (parents.empty())
                break;
        }
    }
}

void TriangleSelector::TriangleSplittingData::update_used_states(const size_t bitstream_start_idx) {
    assert(bitstream_start_idx < this->bitstream.size());
    assert(!this->bitstream.empty() && this->bitstream.size() != bitstream_start_idx);
    assert((this->bitstream.size() - bitstream_start_idx) % 4 == 0);

    if (this->bitstream.empty() || this->bitstream.size() == bitstream_start_idx)
        return;

    size_t nibble_idx = bitstream_start_idx;

    auto read_next_nibble = [&data_bitstream = std::as_const(this->bitstream), &nibble_idx]() -> uint8_t {
        assert(nibble_idx + 3 < data_bitstream.size());
        uint8_t code = 0;
        for (size_t bit_idx = 0; bit_idx < 4; ++bit_idx)
            code |= data_bitstream[nibble_idx++] << bit_idx;
        return code;
    };

    while (nibble_idx < this->bitstream.size()) {
        const uint8_t code = read_next_nibble();

        if (const bool is_split = (code & 0b11) != 0; is_split)
            continue;

        const uint8_t facet_state = (code & 0b1100) == 0b1100 ? read_next_nibble() + 3 : code >> 2;
        assert(facet_state < this->used_states.size());
        if (facet_state >= this->used_states.size())
            continue;

        this->used_states[facet_state] = true;
    }
}

// Lightweight variant of deserialization, which only tests whether a face of test_state exists.
bool TriangleSelector::has_facets(const TriangleSplittingData &data, const EnforcerBlockerType test_state) {
    // Depth-first queue of a number of unvisited children.
    // Kept outside of the loop to avoid re-allocating inside the loop.
    std::vector<int> parents_children;
    parents_children.reserve(64);

    for (const TriangleBitStreamMapping &triangle_id_and_ibit : data.triangles_to_split) {
        int ibit = triangle_id_and_ibit.bitstream_start_idx;
        assert(ibit < int(data.bitstream.size()));
        auto next_nibble = [&data, &ibit = ibit]() {
            int n = 0;
            for (int i = 0; i < 4; ++ i)
                n |= data.bitstream[ibit ++] << i;
            return n;
        };
        // < 0 -> negative of a number of children
        // >= 0 -> state
        auto num_children_or_state = [&next_nibble]() -> int {
            int code               = next_nibble();
            int num_of_split_sides = code & 0b11;
            return num_of_split_sides == 0 ?
                ((code & 0b1100) == 0b1100 ? next_nibble() + 3 : code >> 2) :
                - num_of_split_sides - 1;
        };

        int state = num_children_or_state();
        if (state < 0) {
            // Root is split.
            parents_children.clear();
            parents_children.emplace_back(- state);
            do {
                if (-- parents_children.back() >= 0) {
                    int state = num_children_or_state();
                    if (state < 0)
                        // Child is split.
                        parents_children.emplace_back(- state);
                    else if (state == int(test_state))
                        // Child is not split and a face of test_state was found.
                        return true;
                } else
                    parents_children.pop_back();
            } while (! parents_children.empty());
        } else if (state == int(test_state))
            // Root is not split and a face of test_state was found.
            return true;
    }

    return false;
}

void TriangleSelector::seed_fill_unselect_all_triangles()
{
    for (Triangle &triangle : m_triangles)
        if (!triangle.is_split())
            triangle.unselect_by_seed_fill();
}

void TriangleSelector::seed_fill_apply_on_triangles(EnforcerBlockerType new_state)
{
    for (Triangle &triangle : m_triangles)
        if (!triangle.is_split() && triangle.is_selected_by_seed_fill())
            triangle.set_state(new_state);

    for (Triangle &triangle : m_triangles)
        if (triangle.is_split() && triangle.valid()) {
            size_t facet_idx = &triangle - &m_triangles.front();
            remove_useless_children(int(facet_idx));
        }
}

TriangleSelector::Cursor::Cursor(const Vec3f &source_, float radius_world, const Transform3d &trafo_, const ClippingPlane &clipping_plane_)
    : source{source_}, trafo{trafo_.cast<float>()}, clipping_plane{clipping_plane_}
{
    Vec3d sf = Geometry::Transformation(trafo_).get_scaling_factor();
    if (is_approx(sf(0), sf(1)) && is_approx(sf(1), sf(2))) {
        radius          = float(radius_world / sf(0));
        radius_sqr      = float(Slic3r::sqr(radius_world / sf(0)));
        uniform_scaling = true;
    } else {
        // In case that the transformation is non-uniform, all checks whether
        // something is inside the cursor should be done in world coords.
        // First transform source in world coords and remember that we did this.
        source          = trafo * source;
        uniform_scaling = false;
        radius          = radius_world;
        radius_sqr      = Slic3r::sqr(radius_world);
        trafo_normal    = trafo.linear().inverse().transpose();
    }
}

TriangleSelector::SinglePointCursor::SinglePointCursor(const Vec3f& center_, const Vec3f& source_, float radius_world, const Transform3d& trafo_, const ClippingPlane &clipping_plane_)
    : center{center_}, Cursor(source_, radius_world, trafo_, clipping_plane_)
{
    // In case that the transformation is non-uniform, all checks whether
    // something is inside the cursor should be done in world coords.
    // Because of the center is transformed.
    if (!uniform_scaling)
        center = trafo * center;

    // Calculate dir, in whatever coords is appropriate.
    dir = (center - source).normalized();
}

TriangleSelector::DoublePointCursor::DoublePointCursor(const Vec3f &first_center_, const Vec3f &second_center_, const Vec3f &source_, float radius_world, const Transform3d &trafo_, const ClippingPlane &clipping_plane_)
    : first_center{first_center_}, second_center{second_center_}, Cursor(source_, radius_world, trafo_, clipping_plane_)
{
    if (!uniform_scaling) {
        first_center  = trafo * first_center_;
        second_center = trafo * second_center_;
    }

    // Calculate dir, in whatever coords is appropriate.
    dir = (first_center - source).normalized();
}

// Returns true if clipping plane is not active or if the point not clipped by clipping plane.
inline static bool is_mesh_point_not_clipped(const Vec3f &point, const TriangleSelector::ClippingPlane &clipping_plane)
{
    return !clipping_plane.is_active() || !clipping_plane.is_mesh_point_clipped(point);
}

// Is a point (in mesh coords) inside a Sphere cursor?
bool TriangleSelector::Sphere::is_mesh_point_inside(const Vec3f &point) const
{
    const Vec3f transformed_point = uniform_scaling ? point : Vec3f(trafo * point);
    if ((center - transformed_point).squaredNorm() < radius_sqr)
        return is_mesh_point_not_clipped(point, clipping_plane);

    return false;
}

// Is a point (in mesh coords) inside a Circle cursor?
bool TriangleSelector::Circle::is_mesh_point_inside(const Vec3f &point) const
{
    const Vec3f transformed_point = uniform_scaling ? point : Vec3f(trafo * point);
    const Vec3f diff              = center - transformed_point;

    if ((diff - diff.dot(dir) * dir).squaredNorm() < radius_sqr)
        return is_mesh_point_not_clipped(point, clipping_plane);

    return false;
}

// Is a point (in mesh coords) inside a Capsule3D cursor?
bool TriangleSelector::Capsule3D::is_mesh_point_inside(const Vec3f &point) const
{
    const Vec3f transformed_point  = uniform_scaling ? point : Vec3f(trafo * point);
    const Vec3f first_center_diff  = this->first_center - transformed_point;
    const Vec3f second_center_diff = this->second_center - transformed_point;
    if (first_center_diff.squaredNorm() < this->radius_sqr || second_center_diff.squaredNorm() < this->radius_sqr)
        return is_mesh_point_not_clipped(point, clipping_plane);

    // First, check if the point pt is laying between planes defined by first_center and second_center.
    // Then check if it is inside the cylinder between first_center and second_center.
    const Vec3f centers_diff = this->second_center - this->first_center;
    if (first_center_diff.dot(centers_diff) <= 0.f && second_center_diff.dot(centers_diff) >= 0.f && (first_center_diff.cross(centers_diff).norm() / centers_diff.norm()) <= this->radius)
        return is_mesh_point_not_clipped(point, clipping_plane);

    return false;
}

// Is a point (in mesh coords) inside a Capsule2D cursor?
bool TriangleSelector::Capsule2D::is_mesh_point_inside(const Vec3f &point) const
{
    const Vec3f transformed_point           = uniform_scaling ? point : Vec3f(trafo * point);
    const Vec3f first_center_diff           = this->first_center - transformed_point;
    const Vec3f first_center_diff_projected = first_center_diff - first_center_diff.dot(this->dir) * this->dir;
    if (first_center_diff_projected.squaredNorm() < this->radius_sqr)
        return is_mesh_point_not_clipped(point, clipping_plane);

    const Vec3f second_center_diff           = this->second_center - transformed_point;
    const Vec3f second_center_diff_projected = second_center_diff - second_center_diff.dot(this->dir) * this->dir;
    if (second_center_diff_projected.squaredNorm() < this->radius_sqr)
        return is_mesh_point_not_clipped(point, clipping_plane);

    const Vec3f centers_diff           = this->second_center - this->first_center;
    const Vec3f centers_diff_projected = centers_diff - centers_diff.dot(this->dir) * this->dir;

    // First, check if the point is laying between first_center and second_center.
    if (first_center_diff_projected.dot(centers_diff_projected) <= 0.f && second_center_diff_projected.dot(centers_diff_projected) >= 0.f) {
        // Vector in the direction of line |AD| of the rectangle that intersects the circle with the center in first_center.
        const Vec3f rectangle_da_dir              = centers_diff.cross(this->dir);
        // Vector pointing from first_center to the point 'A' of the rectangle.
        const Vec3f first_center_rectangle_a_diff = rectangle_da_dir.normalized() * this->radius;
        const Vec3f rectangle_a                   = this->first_center - first_center_rectangle_a_diff;
        const Vec3f rectangle_d                   = this->first_center + first_center_rectangle_a_diff;
        // Now check if the point is laying inside the rectangle between circles with centers in first_center and second_center.
        if ((rectangle_a - transformed_point).dot(rectangle_da_dir) <= 0.f && (rectangle_d - transformed_point).dot(rectangle_da_dir) >= 0.f)
            return is_mesh_point_not_clipped(point, clipping_plane);
    }

    return false;
}

// p1, p2, p3 are in mesh coords!
static bool is_circle_pointer_inside_triangle(const Vec3f &p1_, const Vec3f &p2_, const Vec3f &p3_, const Vec3f &center, const Vec3f &dir, const bool uniform_scaling, const Transform3f &trafo) {
    const Vec3f& q1 = center + dir;
    const Vec3f& q2 = center - dir;

    auto signed_volume_sign = [](const Vec3f& a, const Vec3f& b,
                                 const Vec3f& c, const Vec3f& d) -> bool {
        return ((b-a).cross(c-a)).dot(d-a) > 0.;
    };

    // In case the object is non-uniformly scaled, do the check in world coords.
    const Vec3f& p1 = uniform_scaling ? p1_ : Vec3f(trafo * p1_);
    const Vec3f& p2 = uniform_scaling ? p2_ : Vec3f(trafo * p2_);
    const Vec3f& p3 = uniform_scaling ? p3_ : Vec3f(trafo * p3_);

    if (signed_volume_sign(q1,p1,p2,p3) == signed_volume_sign(q2,p1,p2,p3))
        return false;

    bool pos = signed_volume_sign(q1,q2,p1,p2);
    return signed_volume_sign(q1,q2,p2,p3) == pos && signed_volume_sign(q1,q2,p3,p1) == pos;
}

// p1, p2, p3 are in mesh coords!
bool TriangleSelector::SinglePointCursor::is_pointer_in_triangle(const Vec3f &p1_, const Vec3f &p2_, const Vec3f &p3_) const
{
    return is_circle_pointer_inside_triangle(p1_, p2_, p3_, center, dir, uniform_scaling, trafo);
}

// p1, p2, p3 are in mesh coords!
bool TriangleSelector::DoublePointCursor::is_pointer_in_triangle(const Vec3f &p1_, const Vec3f &p2_, const Vec3f &p3_) const
{
    return is_circle_pointer_inside_triangle(p1_, p2_, p3_, first_center, dir, uniform_scaling, trafo) ||
           is_circle_pointer_inside_triangle(p1_, p2_, p3_, second_center, dir, uniform_scaling, trafo);
}

bool line_plane_intersection(const Vec3f &line_a, const Vec3f &line_b, const Vec3f &plane_origin, const Vec3f &plane_normal, Vec3f &out_intersection)
{
    Vec3f line_dir      = line_b - line_a;
    float t_denominator = plane_normal.dot(line_dir);
    if (t_denominator == 0.f)
        return false;

    // Compute 'd' in plane equation by using some point (origin) on the plane
    float plane_d = plane_normal.dot(plane_origin);
    if (float t = (plane_d - plane_normal.dot(line_a)) / t_denominator; t >= 0.f && t <= 1.f) {
        out_intersection = line_a + t * line_dir;
        return true;
    }

    return false;
}

bool TriangleSelector::Capsule3D::is_edge_inside_cursor(const Triangle &tr, const std::vector<Vertex> &vertices) const
{
    std::array<Vec3f, 3> pts;
    for (int i = 0; i < 3; ++i) {
        pts[i] = vertices[tr.verts_idxs[i]].v;
        if (!this->uniform_scaling)
            pts[i] = this->trafo * pts[i];
    }

    for (int side = 0; side < 3; ++side) {
        const Vec3f &edge_a = pts[side];
        const Vec3f &edge_b = pts[side < 2 ? side + 1 : 0];
        if (test_line_inside_capsule(edge_a, edge_b, this->first_center, this->second_center, this->radius))
            return true;
    }

    return false;
}

// Is edge inside cursor?
bool TriangleSelector::Capsule2D::is_edge_inside_cursor(const Triangle &tr, const std::vector<Vertex> &vertices) const
{
    std::array<Vec3f, 3> pts;
    for (int i = 0; i < 3; ++i) {
        pts[i] = vertices[tr.verts_idxs[i]].v;
        if (!this->uniform_scaling)
            pts[i] = this->trafo * pts[i];
    }

    const Vec3f centers_diff                  = this->second_center - this->first_center;
    // Vector in the direction of line |AD| of the rectangle that intersects the circle with the center in first_center.
    const Vec3f rectangle_da_dir              = centers_diff.cross(this->dir);
    // Vector pointing from first_center to the point 'A' of the rectangle.
    const Vec3f first_center_rectangle_a_diff = rectangle_da_dir.normalized() * this->radius;
    const Vec3f rectangle_a                   = this->first_center - first_center_rectangle_a_diff;
    const Vec3f rectangle_d                   = this->first_center + first_center_rectangle_a_diff;

    auto edge_inside_rectangle = [&self = std::as_const(*this), &centers_diff](const Vec3f &edge_a, const Vec3f &edge_b, const Vec3f &plane_origin, const Vec3f &plane_normal) -> bool {
        Vec3f intersection(-1.f, -1.f, -1.f);
        if (line_plane_intersection(edge_a, edge_b, plane_origin, plane_normal, intersection)) {
            // Now check if the intersection point is inside the rectangle. That means it is between 'first_center' and 'second_center', resp. between 'A' and 'B'.
            if (self.first_center.dot(centers_diff) <= intersection.dot(centers_diff) && intersection.dot(centers_diff) <= self.second_center.dot(centers_diff))
                return true;
        }
        return false;
    };

    for (int side = 0; side < 3; ++side) {
        const Vec3f &edge_a     = pts[side];
        const Vec3f &edge_b     = pts[side < 2 ? side + 1 : 0];
        const Vec3f  edge_dir   = edge_b - edge_a;
        const Vec3f  edge_dir_n = edge_dir.normalized();

        float t1      = (this->first_center - edge_a).dot(edge_dir_n);
        float t2      = (this->second_center - edge_a).dot(edge_dir_n);
        Vec3f vector1 = edge_a + t1 * edge_dir_n - this->first_center;
        Vec3f vector2 = edge_a + t2 * edge_dir_n - this->second_center;

        // Vectors vector1 and vector2 are 3D vector from centers to the intersections. What we want to
        // measure is length of its projection onto plane perpendicular to dir.
        if (float dist = vector1.squaredNorm() - std::pow(vector1.dot(this->dir), 2.f); dist < this->radius_sqr && t1 >= 0.f && t1 <= edge_dir.norm())
            return true;

        if (float dist = vector2.squaredNorm() - std::pow(vector2.dot(this->dir), 2.f); dist < this->radius_sqr && t2 >= 0.f && t2 <= edge_dir.norm())
            return true;

        // Check if the edge is passing through the rectangle between first_center and second_center.
        if (edge_inside_rectangle(edge_a, edge_b, rectangle_a, (rectangle_d - rectangle_a)) || edge_inside_rectangle(edge_a, edge_b, rectangle_d, (rectangle_a - rectangle_d)))
            return true;
    }

    return false;
}

// ORCA: Helper to extract used states from serialized data
std::vector<EnforcerBlockerType> TriangleSelector::extract_used_facet_states(const TriangleSplittingData &data)
{
    std::vector<EnforcerBlockerType> out;
    for (size_t i = 0; i < data.used_states.size(); ++i) {
        if (data.used_states[i])
            out.push_back(static_cast<EnforcerBlockerType>(i));
    }
    return out;
}

} // namespace Slic3r
