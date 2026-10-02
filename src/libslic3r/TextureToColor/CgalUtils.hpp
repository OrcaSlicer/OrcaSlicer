#pragma once
#include "TriMesh.hpp"
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <chrono>
#include <cstdio>
#include <map>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Slic3r { namespace tex2color {
namespace cgalutils {

using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using CGALMesh = CGAL::Surface_mesh<Kernel::Point_3>;

inline CGALMesh trimesh_to_cgal(const TriMesh& mesh) {
    CGALMesh cm;
    std::vector<CGALMesh::Vertex_index> vmap(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i)
        vmap[i] = cm.add_vertex(Kernel::Point_3(mesh.vertices[i].x(), mesh.vertices[i].y(), mesh.vertices[i].z()));
    for (const auto& f : mesh.indices) {
        cm.add_face(vmap[f[0]], vmap[f[1]], vmap[f[2]]);
    }
    return cm;
}

inline TriMesh cgal_to_trimesh(const CGALMesh& cm) {
    TriMesh mesh;
    std::map<CGALMesh::Vertex_index, size_t> vmap;
    size_t idx = 0;
    for (auto v : cm.vertices()) {
        if (!cm.is_valid(v) || cm.is_removed(v)) continue;
        auto p = cm.point(v);
        mesh.vertices.push_back(Vec3f((float)p.x(), (float)p.y(), (float)p.z()));
        vmap[v] = idx++;
    }
    for (auto f : cm.faces()) {
        if (!cm.is_valid(f) || cm.is_removed(f)) continue;
        auto h = cm.halfedge(f);
        auto v0 = cm.target(h);
        auto v1 = cm.target(cm.next(h));
        auto v2 = cm.target(cm.next(cm.next(h)));
        mesh.indices.push_back(Vec3i32((int)vmap[v0], (int)vmap[v1], (int)vmap[v2]));
    }
    return mesh;
}

inline bool is_mesh_halfedge_compatible(const TriMesh& mesh) {
    std::vector<std::unordered_set<std::size_t>> vtx_to_adj_faces(mesh.vertices.size());
    std::size_t edge_id = 0;
    std::vector<std::unordered_set<std::size_t>> edge_to_faces;
    std::vector<std::unordered_set<std::size_t>> vtx_to_prev_vtxs(mesh.vertices.size());
    std::vector<std::unordered_set<std::size_t>> vtx_to_next_vtxs(mesh.vertices.size());
    std::vector<std::unordered_map<std::size_t, std::size_t>> vtx_vtx_to_edge(mesh.vertices.size());

    for (std::size_t fid = 0; fid < mesh.indices.size(); ++fid) {
        const TriFace& face = mesh.indices[fid];
        if (face[0] == face[1] || face[1] == face[2] || face[2] == face[0]) {
            return false;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            if (static_cast<std::size_t>(face[i]) >= mesh.vertices.size()) {
                return false;
            }
            vtx_to_adj_faces[face[i]].insert(fid);

            std::size_t prev_vtx = face[(i + 2) % 3];
            std::size_t next_vtx = face[(i + 1) % 3];

            if (vtx_to_prev_vtxs[face[i]].count(prev_vtx)) {
                return false;
            }
            vtx_to_prev_vtxs[face[i]].insert(prev_vtx);

            if (vtx_to_next_vtxs[face[i]].count(next_vtx)) {
                return false;
            }
            vtx_to_next_vtxs[face[i]].insert(next_vtx);
        }

        for (std::size_t i = 0; i < 3; ++i) {
            std::size_t va = face[i];
            std::size_t vb = face[(i + 1) % 3];
            if (!vtx_vtx_to_edge[va].count(vb)) {
                vtx_vtx_to_edge[va][vb] = edge_id;
                vtx_vtx_to_edge[vb][va] = edge_id;
                ++edge_id;
                edge_to_faces.emplace_back(std::unordered_set<std::size_t>());
            }
            edge_to_faces[vtx_vtx_to_edge[va][vb]].insert(fid);
        }
    }

    for (std::size_t vid = 0; vid < mesh.vertices.size(); ++vid) {
        if (vtx_to_adj_faces[vid].empty()) {
            continue;
        }
        std::unordered_set<std::size_t> visited_faces;
        std::queue<std::size_t> face_queue;
        face_queue.push(*(vtx_to_adj_faces[vid].begin()));
        visited_faces.insert(*(vtx_to_adj_faces[vid].begin()));
        while (!face_queue.empty()) {
            std::size_t fid = face_queue.front();
            face_queue.pop();
            const TriFace& face = mesh.indices[fid];
            for (std::size_t i = 0; i < 3; ++i) {
                if (static_cast<std::size_t>(face[i]) != vid) {
                    continue;
                }
                std::size_t v_next = face[(i + 1) % 3];
                std::size_t v_prev = face[(i + 2) % 3];
                for (std::size_t nbr : {v_next, v_prev}) {
                    std::size_t eid = vtx_vtx_to_edge[vid][nbr];
                    for (std::size_t adj_fid : edge_to_faces[eid]) {
                        if (!visited_faces.count(adj_fid) && vtx_to_adj_faces[vid].count(adj_fid)) {
                            visited_faces.insert(adj_fid);
                            face_queue.push(adj_fid);
                        }
                    }
                }
                break;
            }
        }

        for (std::size_t fid : vtx_to_adj_faces[vid]) {
            if (!visited_faces.count(fid)) {
                return false;
            }
        }
    }

    return true;
}

inline bool convert_trimesh_to_cgal(const TriMesh& mesh, CGALMesh& cgal_mesh) {
    cgal_mesh = trimesh_to_cgal(mesh);
    return cgal_mesh.number_of_faces() > 0 || mesh.indices.empty();
}

inline bool convert_trimesh_to_cgal(
    const TriMesh& mesh, const std::vector<Vec2f>& vertex_uvs,
    CGALMesh& cgal_mesh, std::vector<Vec2f>& cgal_vertex_uvs)
{
    cgal_mesh.clear();
    std::vector<CGALMesh::Vertex_index> vmap(mesh.vertices.size());
    cgal_vertex_uvs.clear();

    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        vmap[i] = cgal_mesh.add_vertex(Kernel::Point_3(
            mesh.vertices[i].x(), mesh.vertices[i].y(), mesh.vertices[i].z()));
    }

    cgal_vertex_uvs.resize(cgal_mesh.num_vertices());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (i < vertex_uvs.size())
            cgal_vertex_uvs[vmap[i]] = vertex_uvs[i];
        else
            cgal_vertex_uvs[vmap[i]] = Vec2f(0.f, 0.f);
    }

    for (const auto& f : mesh.indices)
        cgal_mesh.add_face(vmap[f[0]], vmap[f[1]], vmap[f[2]]);

    return true;
}

} // namespace cgalutils
} // namespace tex2color
} // namespace Slic3r
