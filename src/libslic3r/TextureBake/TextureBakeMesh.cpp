#include "TextureBakeMesh.hpp"

#include <algorithm>

namespace Slic3r {
namespace TextureBake {

TriSoup to_soup(const indexed_triangle_set &its, const std::vector<uint8_t> &face_excluded)
{
    TriSoup      out;
    const size_t n = its.indices.size();
    out.pos.resize(n * 3);
    out.nrm.resize(n * 3);
    const bool have_excl = face_excluded.size() == n;
    if (have_excl)
        out.exclude_weight.resize(n * 3);

    for (size_t t = 0; t < n; ++t) {
        const stl_triangle_vertex_indices &tri = its.indices[t];
        const Vec3f a = its.vertices[size_t(tri[0])];
        const Vec3f b = its.vertices[size_t(tri[1])];
        const Vec3f c = its.vertices[size_t(tri[2])];
        Vec3f       nrm = (b - a).cross(c - a);
        const float len = nrm.norm();
        nrm = (len > 0.f) ? Vec3f(nrm / len) : Vec3f(0.f, 0.f, 1.f);
        out.pos[t * 3]     = a;
        out.pos[t * 3 + 1] = b;
        out.pos[t * 3 + 2] = c;
        // Per-face on purpose: the accurate indexer derives smooth normals and splits at sharp edges
        // itself, so averaged ones would pre-empt that.
        out.nrm[t * 3] = out.nrm[t * 3 + 1] = out.nrm[t * 3 + 2] = nrm;
        if (have_excl) {
            const float w = face_excluded[t] ? 1.f : 0.f;
            out.exclude_weight[t * 3] = out.exclude_weight[t * 3 + 1] = out.exclude_weight[t * 3 + 2] = w;
        }
    }
    return out;
}

indexed_triangle_set to_indexed_triangle_set(const TriSoup &soup)
{
    indexed_triangle_set out;
    const size_t         n = soup.pos.size();
    out.indices.reserve(n / 3);
    QuantizedPointMap map(WELD_GRID_GEOMETRY, std::min(n, size_t(1) << 22));
    std::vector<int>  id(n);
    for (size_t i = 0; i < n; ++i) {
        id[i] = map.get_or_set(soup.pos[i], int(out.vertices.size()));
        if (map.inserted())
            out.vertices.push_back(soup.pos[i]);
    }
    for (size_t t = 0; t + 2 < n; t += 3) {
        // Welded-together corners carry no area.
        if (id[t] == id[t + 1] || id[t + 1] == id[t + 2] || id[t] == id[t + 2])
            continue;
        out.indices.emplace_back(id[t], id[t + 1], id[t + 2]);
    }
    return out;
}

} // namespace TextureBake
} // namespace Slic3r
