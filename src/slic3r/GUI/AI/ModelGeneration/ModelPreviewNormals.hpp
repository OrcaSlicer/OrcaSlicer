#pragma once

#include "libslic3r/TriangleMesh.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace Slic3r::GUI::ModelPreviewNormals {

// Smooth only across a consistently oriented, manifold edge below the crease
// angle. A shared vertex alone never connects two surfaces or thin back faces.
inline std::vector<Vec3f> corner_normals(const indexed_triangle_set& mesh, float crease_degrees = 45.f)
{
    struct Edge { uint64_t key; uint32_t face; uint8_t side; };
    const size_t faces = mesh.indices.size();
    std::vector<Vec3f> weighted(faces), normals(faces * 3);
    for (Vec3f& normal : normals) normal.setZero();
    std::vector<uint32_t> parent(faces * 3);
    std::iota(parent.begin(), parent.end(), 0);
    std::vector<Edge> edges;
    edges.reserve(faces * 3);
    for (size_t face = 0; face < faces; ++face) {
        const auto& f = mesh.indices[face];
        const Vec3f cross = (mesh.vertices[f[1]] - mesh.vertices[f[0]]).cross(
            mesh.vertices[f[2]] - mesh.vertices[f[0]]);
        weighted[face] = cross.allFinite() ? cross : Vec3f::Zero().eval();
        for (uint8_t side = 0; side < 3; ++side) {
            const uint32_t a = uint32_t(f[side]), b = uint32_t(f[(side + 1) % 3]);
            if (a != b) edges.push_back({(uint64_t(std::min(a, b)) << 32) | std::max(a, b), uint32_t(face), side});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.key < b.key; });
    const auto root = [&](uint32_t id) {
        while (parent[id] != id) { parent[id] = parent[parent[id]]; id = parent[id]; }
        return id;
    };
    const float min_dot = std::cos(crease_degrees * .01745329252f);
    for (size_t begin = 0; begin < edges.size();) {
        size_t end = begin + 1;
        while (end < edges.size() && edges[end].key == edges[begin].key) ++end;
        if (end - begin == 2) {
            const Edge& a = edges[begin]; const Edge& b = edges[begin + 1];
            const auto& fa = mesh.indices[a.face]; const auto& fb = mesh.indices[b.face];
            const int a0 = fa[a.side], a1 = fa[(a.side + 1) % 3];
            const int b0 = fb[b.side], b1 = fb[(b.side + 1) % 3];
            const float lengths = weighted[a.face].norm() * weighted[b.face].norm();
            if (a.face != b.face && a0 == b1 && a1 == b0 && lengths > 1e-12f &&
                weighted[a.face].dot(weighted[b.face]) >= min_dot * lengths) {
                const uint32_t a_first = a.face * 3 + a.side;
                const uint32_t a_second = a.face * 3 + (a.side + 1) % 3;
                const uint32_t b_first = b.face * 3 + b.side;
                const uint32_t b_second = b.face * 3 + (b.side + 1) % 3;
                const auto unite = [&](uint32_t left, uint32_t right) {
                    const uint32_t l = root(left), r = root(right);
                    parent[std::max(l, r)] = std::min(l, r);
                };
                unite(a_first, b_second);
                unite(a_second, b_first);
            }
        }
        begin = end;
    }
    std::vector<Edge>().swap(edges);
    for (size_t face = 0; face < faces; ++face)
        for (uint32_t corner = 0; corner < 3; ++corner)
            normals[root(uint32_t(face * 3 + corner))] += weighted[face];
    for (size_t index = 0; index < normals.size(); ++index)
        if (parent[index] == index && normals[index].norm() > 1e-8f)
            normals[index].normalize();
    // Roots always have the smallest corner ID in their group. Fill from the
    // end so an output corner cannot overwrite an unconsumed root normal.
    for (size_t index = normals.size(); index-- > 0;) {
        const uint32_t representative = root(uint32_t(index));
        if (normals[representative].squaredNorm() > 1e-16f)
            normals[index] = normals[representative];
        else {
            const Vec3f& own = weighted[index / 3];
            normals[index] = own.squaredNorm() > 1e-16f ? Vec3f(own.normalized()) : Vec3f(Vec3f::UnitZ());
        }
    }
    return normals;
}

} // namespace Slic3r::GUI::ModelPreviewNormals
