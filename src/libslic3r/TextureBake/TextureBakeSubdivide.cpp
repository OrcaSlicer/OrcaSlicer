#include "TextureBakeSubdivide.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Slic3r {
namespace TextureBake {

namespace {

double edge_len_sq(const VertStore &v, int a, int b)
{
    return (v.pos[size_t(a)] - v.pos[size_t(b)]).squaredNorm();
}

// Both indexers accumulate raw, area-weighted cross products and normalise once at the end.
void normalize_store_normals(VertStore &verts)
{
    for (Vec3d &n : verts.nrm) {
        const double len = n.norm();
        n = (len > 0.0) ? Vec3d(n / len) : Vec3d(0.0, 0.0, 1.0);
    }
}

// Keyed by the raw parent-vertex pair rather than by position: two sharp-edge copies of one point
// need their own midpoints, since their normals differ even though the position does not.
int get_midpoint(VertStore &verts, QuantizedPointMap &cache, int a, int b,
                 QuantizedPointMap *pos_canon_map)
{
    const int lo = std::min(a, b), hi = std::max(a, b);
    if (const int cached = cache.get_key(lo, hi, 0); cached != -1)
        return cached;

    const Vec3d  m  = (verts.pos[size_t(a)] + verts.pos[size_t(b)]) * 0.5;
    Vec3d        n  = verts.nrm[size_t(a)] + verts.nrm[size_t(b)];
    const double nl = n.norm();
    n = (nl > 0.0) ? Vec3d(n / nl) : verts.nrm[size_t(a)];

    const int idx = verts.push(m, n);
    if (!verts.wgt.empty())
        verts.wgt.push_back((verts.wgt[size_t(a)] + verts.wgt[size_t(b)]) * 0.5);
    if (!verts.canon.empty() && pos_canon_map != nullptr)
        verts.canon.push_back(pos_canon_map->get_or_set(float(m.x()), float(m.y()), float(m.z()), idx));

    cache.get_or_set_key(lo, hi, 0, idx);
    return idx;
}

struct PassResult
{
    std::vector<int>     indices;
    std::vector<uint8_t> face_excluded;
    std::vector<int>     face_parent_id;
    bool                 changed = false;
    bool                 capped  = false;
};

// Three steps, so that no T-junction can appear:
//   1.   Mark every too-long edge globally, so both triangles on a shared edge decide alike.
//   1.5  Predict the exact resulting count from the marks (0->1, 1->2, 2->3, 3->4) and abort the
//        *whole* pass if it exceeds the cap - a partial pass leaves split parents beside unsplit
//        neighbours, the very crack step 1 prevents.
//   2.   Rebuild, allocating once at the now-known size.
PassResult subdivide_pass(VertStore &verts, const std::vector<int> &indices, double max_edge_length,
                          int safety_cap, const std::vector<uint8_t> &face_excluded,
                          QuantizedPointMap *pos_canon_map, const std::vector<int> &face_parent_id)
{
    PassResult   out;
    const double max_sq     = max_edge_length * max_edge_length;
    const size_t tri_count  = indices.size() / 3;
    const bool   have_canon = !verts.canon.empty();

    QuantizedPointMap mid_cache(1.0, 1 << 16);
    QuantizedPointMap split_edges(1.0, 1 << 16);

    // With canonical ids the key is the canonical *position* id, so split copies either side of a
    // sharp edge see one another's decision; without them the vertex index serves.
    const auto key_of = [&](int v) -> int64_t { return have_canon ? verts.canon[size_t(v)] : v; };
    const auto mark_edge = [&](int a, int b) {
        const int64_t u = key_of(a), v = key_of(b);
        if (u < v) split_edges.get_or_set_key(u, v, 0, 1);
        else       split_edges.get_or_set_key(v, u, 0, 1);
    };
    const auto is_marked = [&](int a, int b) {
        const int64_t u = key_of(a), v = key_of(b);
        return (u < v ? split_edges.get_key(u, v, 0) : split_edges.get_key(v, u, 0)) != -1;
    };

    // Step 1. An excluded triangle marks none of its own edges, so its interior never refines; its
    // boundary edges are still marked by an included neighbour, and it follows that split.
    for (size_t t = 0; t < tri_count; ++t) {
        if (!face_excluded.empty() && face_excluded[t])
            continue;
        const int a = indices[t * 3], b = indices[t * 3 + 1], c = indices[t * 3 + 2];
        if (edge_len_sq(verts, a, b) > max_sq) mark_edge(a, b);
        if (edge_len_sq(verts, b, c) > max_sq) mark_edge(b, c);
        if (edge_len_sq(verts, c, a) > max_sq) mark_edge(c, a);
    }
    if (split_edges.size() == 0) {
        out.indices        = indices;
        out.face_excluded  = face_excluded;
        out.face_parent_id = face_parent_id;
        return out; // changed stays false: nothing left to refine
    }

    // Step 1.5.
    size_t predicted = 0;
    for (size_t t = 0; t < tri_count; ++t) {
        const int a = indices[t * 3], b = indices[t * 3 + 1], c = indices[t * 3 + 2];
        const int n = int(is_marked(a, b)) + int(is_marked(b, c)) + int(is_marked(c, a));
        predicted += (n == 0) ? 1 : size_t(n + 1);
    }
    if (predicted > size_t(safety_cap)) {
        out.indices        = indices;
        out.face_excluded  = face_excluded;
        out.face_parent_id = face_parent_id;
        out.capped         = true;
        return out; // coarser than asked for, but watertight
    }

    // Step 2.
    out.indices.resize(predicted * 3);
    if (!face_excluded.empty())
        out.face_excluded.resize(predicted);
    if (!face_parent_id.empty())
        out.face_parent_id.resize(predicted);
    size_t wi = 0, fi = 0;
    const auto emit_face_data = [&](uint8_t excl, int pid, int times) {
        for (int k = 0; k < times; ++k) {
            if (!out.face_excluded.empty())  out.face_excluded[fi]  = excl;
            if (!out.face_parent_id.empty()) out.face_parent_id[fi] = pid;
            ++fi;
        }
    };
    const auto emit = [&](int x, int y, int z) {
        out.indices[wi++] = x; out.indices[wi++] = y; out.indices[wi++] = z;
    };

    for (size_t t = 0; t < tri_count; ++t) {
        const int     a    = indices[t * 3], b = indices[t * 3 + 1], c = indices[t * 3 + 2];
        const uint8_t excl = face_excluded.empty() ? uint8_t(0) : face_excluded[t];
        const int     pid  = face_parent_id.empty() ? 0 : face_parent_id[t];
        const bool    s_ab = is_marked(a, b), s_bc = is_marked(b, c), s_ca = is_marked(c, a);
        const int     n    = int(s_ab) + int(s_bc) + int(s_ca);

        if (n == 0) {
            emit(a, b, c);
            emit_face_data(excl, pid, 1);
        } else if (n == 3) {
            //        a
            //       / \
            //     mCA-mAB
            //     / \ / \
            //    c--mBC--b
            const int m_ab = get_midpoint(verts, mid_cache, a, b, pos_canon_map);
            const int m_bc = get_midpoint(verts, mid_cache, b, c, pos_canon_map);
            const int m_ca = get_midpoint(verts, mid_cache, c, a, pos_canon_map);
            emit(a, m_ab, m_ca);
            emit(m_ab, b, m_bc);
            emit(m_ca, m_bc, c);
            emit(m_ab, m_bc, m_ca);
            emit_face_data(excl, pid, 4);
        } else if (n == 1) {
            if (s_ab) {
                const int m = get_midpoint(verts, mid_cache, a, b, pos_canon_map);
                emit(a, m, c);
                emit(m, b, c);
            } else if (s_bc) {
                const int m = get_midpoint(verts, mid_cache, b, c, pos_canon_map);
                emit(a, b, m);
                emit(a, m, c);
            } else {
                const int m = get_midpoint(verts, mid_cache, c, a, pos_canon_map);
                emit(a, b, m);
                emit(m, b, c);
            }
            emit_face_data(excl, pid, 2);
        } else {
            // A corner triangle on the untouched-edge vertex, then the remaining quadrilateral split
            // along the midpoint-to-midpoint diagonal, which keeps the winding consistent.
            //
            // A sliver parent propagates: that inner diagonal inherits half the short edge and hands
            // the sliver to two children per pass. No better diagonal exists - one avoiding the
            // midpoints must pass through one of them, giving a zero-area triangle. Regularization
            // removes such slivers before the mesh reaches here.
            if (!s_ab) { // fan from c
                const int m_bc = get_midpoint(verts, mid_cache, b, c, pos_canon_map);
                const int m_ca = get_midpoint(verts, mid_cache, c, a, pos_canon_map);
                emit(a, b, m_bc);
                emit(a, m_bc, m_ca);
                emit(c, m_ca, m_bc);
            } else if (!s_bc) { // fan from a
                const int m_ab = get_midpoint(verts, mid_cache, a, b, pos_canon_map);
                const int m_ca = get_midpoint(verts, mid_cache, c, a, pos_canon_map);
                emit(a, m_ab, m_ca);
                emit(m_ab, b, c);
                emit(m_ab, c, m_ca);
            } else { // fan from b
                const int m_ab = get_midpoint(verts, mid_cache, a, b, pos_canon_map);
                const int m_bc = get_midpoint(verts, mid_cache, b, c, pos_canon_map);
                emit(b, m_bc, m_ab);
                emit(a, m_ab, m_bc);
                emit(a, m_bc, c);
            }
            emit_face_data(excl, pid, 3);
        }
    }

    out.changed = true;
    return out;
}

} // namespace

IndexedMesh to_indexed_fast(const TriSoup &geometry)
{
    // Preview path: a plain position merge - no clustering, no sharp-edge splitting, no canonical ids.
    IndexedMesh       out;
    const size_t      n = geometry.pos.size();
    QuantizedPointMap vert_map(WELD_GRID_GEOMETRY, std::min(n, size_t(1) << 22));
    out.indices.resize(n);
    const bool has_w = !geometry.exclude_weight.empty();

    for (size_t i = 0; i < n; ++i) {
        const Vec3f &p   = geometry.pos[i];
        const Vec3f  nf  = geometry.nrm.empty() ? Vec3f(0.f, 0.f, 1.f) : geometry.nrm[i];
        const int    idx = vert_map.get_or_set(p, int(out.verts.count()));
        if (vert_map.inserted()) {
            out.verts.push(p.cast<double>(), nf.cast<double>());
            if (has_w)
                out.verts.wgt.push_back(double(geometry.exclude_weight[i]));
        } else {
            out.verts.nrm[size_t(idx)] += nf.cast<double>();
            // Merge exclusion by maximum: any excluded face marks the shared vertex.
            if (has_w && double(geometry.exclude_weight[i]) > out.verts.wgt[size_t(idx)])
                out.verts.wgt[size_t(idx)] = double(geometry.exclude_weight[i]);
        }
        out.indices[i] = idx;
    }
    normalize_store_normals(out.verts);
    return out;
}

IndexedMesh to_indexed(const TriSoup &geometry)
{
    // Export path. Two vertices at one position merge only when their face normals agree to within
    // SUBDIVIDE_SHARP_ANGLE_DEG, which keeps a cylinder from faceting while stopping a cube's edge
    // normal from leaking into the flat face interiors as subdivision carries it inward.
    IndexedMesh  out;
    out.has_canon      = true;
    const size_t n     = geometry.pos.size();
    const bool   has_w = !geometry.exclude_weight.empty();
    const double sharp_cos = std::cos(SUBDIVIDE_SHARP_ANGLE_DEG * M_PI / 180.0);

    // Per-face normals: unit for the angle test, raw for the area-weighted accumulation.
    std::vector<Vec3d> face_unit(n), face_raw(n);
    for (size_t t = 0; t + 2 < n; t += 3) {
        const Vec3d  a   = geometry.pos[t].cast<double>();
        const Vec3d  b   = geometry.pos[t + 1].cast<double>();
        const Vec3d  c   = geometry.pos[t + 2].cast<double>();
        const Vec3d  r   = (b - a).cross(c - a);
        const double len = r.norm();
        const Vec3d  u   = (len > 0.0) ? Vec3d(r / len) : Vec3d(0.0, 0.0, 1.0);
        for (int v = 0; v < 3; ++v) {
            face_unit[t + size_t(v)] = u;
            face_raw[t + size_t(v)]  = r;
        }
    }

    out.indices.resize(n);
    out.pos_canon_map = QuantizedPointMap(WELD_GRID_GEOMETRY, std::min(n, size_t(1) << 22));
    struct Cluster { int idx; Vec3d fn_unit; };
    std::unordered_map<int, std::vector<Cluster>> clusters_by_canon;

    for (size_t i = 0; i < n; ++i) {
        const Vec3f &p = geometry.pos[i];
        // The first vertex at a position becomes its canonical id; later split copies share it.
        const int  canon_id       = out.pos_canon_map.get_or_set(p, int(out.verts.count()));
        const bool fresh_position = out.pos_canon_map.inserted();

        const auto add_vertex = [&](int canon) {
            const int idx = out.verts.push(p.cast<double>(), face_raw[i]);
            if (has_w)
                out.verts.wgt.push_back(double(geometry.exclude_weight[i]));
            out.verts.canon.push_back(canon);
            return idx;
        };

        if (fresh_position) {
            const int idx = add_vertex(canon_id);
            clusters_by_canon[canon_id].push_back({ idx, face_unit[i] });
            out.indices[i] = idx;
            continue;
        }

        std::vector<Cluster> &clusters = clusters_by_canon[canon_id];
        bool                  matched  = false;
        for (Cluster &cl : clusters) {
            if (cl.fn_unit.dot(face_unit[i]) < sharp_cos)
                continue;
            out.verts.nrm[size_t(cl.idx)] += face_raw[i];
            if (has_w && double(geometry.exclude_weight[i]) > out.verts.wgt[size_t(cl.idx)])
                out.verts.wgt[size_t(cl.idx)] = double(geometry.exclude_weight[i]);
            // Track the running average, so gradual curvature stays in one cluster instead of
            // fragmenting when a distant face exceeds the threshold against the seed's fixed normal.
            cl.fn_unit += face_unit[i];
            if (const double rl = cl.fn_unit.norm(); rl > 0.0)
                cl.fn_unit /= rl;
            out.indices[i] = cl.idx;
            matched        = true;
            break;
        }
        if (!matched) {
            // A sharp-edge split: a new vertex at the same position, sharing its canonical id.
            const int idx = add_vertex(canon_id);
            clusters.push_back({ idx, face_unit[i] });
            out.indices[i] = idx;
        }
    }

    normalize_store_normals(out.verts);
    return out;
}

TriSoup to_non_indexed(const VertStore &verts, const std::vector<int> &indices,
                       const std::vector<uint8_t> &face_excluded)
{
    TriSoup      out;
    const size_t tri_count = indices.size() / 3;
    out.pos.resize(tri_count * 3);
    out.nrm.resize(tri_count * 3);
    const bool want_weights = !face_excluded.empty() || !verts.wgt.empty();
    if (want_weights)
        out.exclude_weight.resize(tri_count * 3);

    for (size_t t = 0; t < tri_count; ++t) {
        // The per-face flag, not the interpolated weight: merging by maximum can push an *included*
        // face's corners to 1 when it borders two excluded neighbours, wrongly excluding it.
        const bool  have_face_flag = !face_excluded.empty();
        const float face_w         = have_face_flag ? (face_excluded[t] ? 1.f : 0.f) : 0.f;
        for (int v = 0; v < 3; ++v) {
            const size_t vidx = size_t(indices[t * 3 + size_t(v)]);
            out.pos[t * 3 + size_t(v)] = verts.pos[vidx].cast<float>();
            out.nrm[t * 3 + size_t(v)] = verts.nrm[vidx].cast<float>();
            if (want_weights)
                out.exclude_weight[t * 3 + size_t(v)] =
                    have_face_flag ? face_w : float(verts.wgt[vidx]);
        }
    }
    return out;
}

SubdivideResult subdivide(const TriSoup &geometry, double max_edge_length,
                          const std::vector<uint8_t> &face_excluded, bool fast, int safety_cap,
                          const SubdivideProgressFn &on_progress)
{
    SubdivideResult result;
    if (geometry.empty() || max_edge_length <= 0.0) {
        result.geometry = geometry;
        return result;
    }

    IndexedMesh        indexed   = fast ? to_indexed_fast(geometry) : to_indexed(geometry);
    QuantizedPointMap *canon_map = indexed.has_canon ? &indexed.pos_canon_map : nullptr;

    std::vector<int>     current_indices  = indexed.indices;
    std::vector<uint8_t> current_excluded = face_excluded;
    const size_t         initial_tris     = indexed.indices.size() / 3;
    std::vector<int>     current_parent(initial_tris);
    for (size_t i = 0; i < initial_tris; ++i)
        current_parent[i] = int(i);

    for (int iter = 0; iter < SUBDIVIDE_MAX_ITERATIONS; ++iter) {
        if (current_indices.size() / 3 >= size_t(safety_cap)) {
            result.safety_cap_hit = true;
            break;
        }

        PassResult pass = subdivide_pass(indexed.verts, current_indices, max_edge_length, safety_cap,
                                         current_excluded, canon_map, current_parent);
        current_indices = std::move(pass.indices);
        if (!pass.face_excluded.empty())
            current_excluded = std::move(pass.face_excluded);
        if (!pass.face_parent_id.empty())
            current_parent = std::move(pass.face_parent_id);
        if (pass.capped || current_indices.size() / 3 >= size_t(safety_cap))
            result.safety_cap_hit = true;

        if (on_progress) {
            // Reported after the pass, so the value falls each iteration instead of lagging a step.
            double max_edge_sq = 0.0;
            for (size_t t = 0; t + 2 < current_indices.size(); t += 3) {
                const int a = current_indices[t], b = current_indices[t + 1], c = current_indices[t + 2];
                max_edge_sq = std::max({ max_edge_sq, edge_len_sq(indexed.verts, a, b),
                                         edge_len_sq(indexed.verts, b, c),
                                         edge_len_sq(indexed.verts, c, a) });
            }
            if (!on_progress(std::min(0.95, double(iter + 1) / SUBDIVIDE_MAX_ITERATIONS),
                             current_indices.size() / 3, std::sqrt(max_edge_sq)))
                break; // whole passes only, so what we have is still crack-free
        }

        if (!pass.changed || result.safety_cap_hit)
            break;
    }

    result.geometry       = to_non_indexed(indexed.verts, current_indices, current_excluded);
    result.face_parent_id = std::move(current_parent);
    return result;
}

} // namespace TextureBake
} // namespace Slic3r
