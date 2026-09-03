#include "TextureBakeRegularize.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace Slic3r {
namespace TextureBake {

namespace {

// Vertex-to-triangle lists as intrusive doubly linked lists of corner slots over flat arrays. Slot s
// is corner (triangle * 3 + k), owned by corners[s]. Deleted and moved corners are unlinked, so a
// collapse costs no allocation.
struct SlotLists
{
    std::vector<int> head, next, prev;

    void init(size_t vertex_count, size_t slot_count)
    {
        head.assign(vertex_count, -1);
        next.assign(slot_count, -1);
        prev.assign(slot_count, -1);
    }
    void link(int s, const std::vector<int> &corners)
    {
        const int v = corners[size_t(s)];
        const int h = head[size_t(v)];
        prev[size_t(s)] = -1;
        next[size_t(s)] = h;
        if (h != -1)
            prev[size_t(h)] = s;
        head[size_t(v)] = s;
    }
    void unlink(int s, const std::vector<int> &corners)
    {
        const int p = prev[size_t(s)], n = next[size_t(s)];
        if (p != -1) next[size_t(p)] = n;
        else         head[size_t(corners[size_t(s)])] = n;
        if (n != -1) prev[size_t(n)] = p;
    }
};

} // namespace

RegularizeResult regularize_mesh(const TriSoup &geometry, const std::vector<int> &face_parent_id,
                                 double max_edge_length, const RegularizeOptions &opts)
{
    RegularizeResult result;
    const size_t     tri_count = geometry.triangle_count();
    if (tri_count == 0 || max_edge_length <= 0.0) {
        result.geometry       = geometry;
        result.face_parent_id = face_parent_id;
        return result;
    }

    const double base_max_len_sq = (max_edge_length * opts.slack) * (max_edge_length * opts.slack);
    const double aggr_max_len_sq =
        (max_edge_length * opts.aggressive_slack) * (max_edge_length * opts.aggressive_slack);
    const double extreme_aspect2 = opts.extreme_sliver_aspect * opts.extreme_sliver_aspect;
    const double aspect_thr2     = opts.aspect_threshold * opts.aspect_threshold;

    // Double precision: a collapse writes a midpoint back and later collapses read it, so rounding
    // would accumulate.
    QuantizedPointMap pos_map(WELD_GRID_GEOMETRY, std::min(tri_count * 3, size_t(1) << 22));
    std::vector<Vec3d> vert;
    std::vector<int>   corners(tri_count * 3);
    vert.reserve(tri_count);
    for (size_t i = 0; i < tri_count * 3; ++i) {
        const Vec3f &p  = geometry.pos[i];
        const int    id = pos_map.get_or_set(p, int(vert.size()));
        if (pos_map.inserted())
            vert.push_back(p.cast<double>());
        corners[i] = id;
    }
    const size_t vert_count = vert.size();

    std::vector<Vec3d>   tri_nrm(tri_count, Vec3d::Zero());
    std::vector<uint8_t> tri_deleted(tri_count, 0);
    result.face_parent_id = face_parent_id;
    if (result.face_parent_id.size() != tri_count)
        result.face_parent_id.assign(tri_count, 0);

    const auto sq_dist = [&](int a, int b) { return (vert[size_t(a)] - vert[size_t(b)]).squaredNorm(); };

    const auto recompute_face_normal = [&](size_t t) {
        const Vec3d &a = vert[size_t(corners[t * 3])];
        const Vec3d  n = (vert[size_t(corners[t * 3 + 1])] - a).cross(vert[size_t(corners[t * 3 + 2])] - a);
        const double len = n.norm();
        tri_nrm[t] = (len > 0.0) ? Vec3d(n / len) : Vec3d::Zero();
    };
    for (size_t t = 0; t < tri_count; ++t)
        recompute_face_normal(t);

    // Never updated - the normal gate measures against these, so drift cannot compound across rounds.
    const std::vector<Vec3d> orig_nrm = tri_nrm;

    // Squared thinness, the longest edge over the shortest altitude:
    //   thinness = lmax / hmin = lmax^2 / (2 * area),  so thinness^2 = lmax^4 / |AB x AC|^2
    //
    // Not lmax/lmin, which misses what matters here: three near-collinear points can have all edges
    // similar, so an edge ratio reports about 2 and the gate skips a triangle with near-zero area
    // whose corners sample three unrelated texels. An equilateral scores about 1.15.
    const auto tri_aspect_sq = [&](size_t t) -> double {
        const Vec3d &a  = vert[size_t(corners[t * 3])];
        const Vec3d  ab = vert[size_t(corners[t * 3 + 1])] - a;
        const Vec3d  ac = vert[size_t(corners[t * 3 + 2])] - a;
        const Vec3d  bc = vert[size_t(corners[t * 3 + 2])] - vert[size_t(corners[t * 3 + 1])];
        const double lmax2  = std::max({ ab.squaredNorm(), ac.squaredNorm(), bc.squaredNorm() });
        const double cross2 = ab.cross(ac).squaredNorm();
        return cross2 > 0.0 ? lmax2 * lmax2 / cross2 : std::numeric_limits<double>::infinity();
    };

    SlotLists slots;
    slots.init(vert_count, tri_count * 3);
    for (size_t s = 0; s < tri_count * 3; ++s)
        slots.link(int(s), corners);

    // O(1) membership without clearing a set per collapse.
    std::vector<uint32_t> vert_stamp(vert_count, 0), tri_stamp(tri_count, 0);
    uint32_t              stamp_gen = 0;

    // Both endpoints of a hard edge are barred from being collapse endpoints, preserving such
    // corners exactly while leaving flat-face interiors free.
    //
    // Skipped when either triangle is an extreme sliver: a sliver's normal is dominated by where its
    // far apex sits, so noise pivots it tens of degrees with no feature behind it, and freezing on
    // that would lock the very chains this pass exists to dissolve. Genuine features are bordered by
    // well-shaped triangles and are unaffected.
    std::vector<uint8_t> frozen_vert(vert_count, 0);
    {
        std::vector<double> tri_thin2(tri_count);
        for (size_t t = 0; t < tri_count; ++t)
            tri_thin2[t] = tri_aspect_sq(t);
        QuantizedPointMap edge_seen(1.0, std::min(tri_count * 3, size_t(1) << 22));
        for (size_t t = 0; t < tri_count; ++t)
            for (int e = 0; e < 3; ++e) {
                const int u = corners[t * 3 + size_t(e)];
                const int v = corners[t * 3 + size_t((e + 1) % 3)];
                const int lo = std::min(u, v), hi = std::max(u, v);
                const int other = edge_seen.get_or_set_key(lo, hi, 0, int(t));
                if (edge_seen.inserted())
                    continue;
                if (tri_thin2[t] > extreme_aspect2 || tri_thin2[size_t(other)] > extreme_aspect2)
                    continue;
                if (tri_nrm[t].dot(tri_nrm[size_t(other)]) < opts.sharp_edge_cos) {
                    frozen_vert[size_t(u)] = 1;
                    frozen_vert[size_t(v)] = 1;
                }
            }
    }

    // Exclusion freeze. The weight is constant across a face's corners, so the first one answers.
    if (opts.preserve_excluded && !geometry.exclude_weight.empty())
        for (size_t t = 0; t < tri_count; ++t)
            if (geometry.exclude_weight[t * 3] > 0.99f)
                for (int k = 0; k < 3; ++k)
                    frozen_vert[size_t(corners[t * 3 + size_t(k)])] = 1;

    std::vector<int> wing_scratch, affected_scratch;

    const auto third_vertex = [&](size_t t, int u, int v) {
        const int a = corners[t * 3], b = corners[t * 3 + 1], c = corners[t * 3 + 2];
        if (a != u && a != v) return a;
        if (b != u && b != v) return b;
        return c;
    };
    const auto triangles_sharing_edge = [&](int u, int v) -> std::vector<int> & {
        wing_scratch.clear();
        for (int s = slots.head[size_t(u)]; s != -1; s = slots.next[size_t(s)]) {
            const size_t t = size_t(s) / 3;
            if (tri_deleted[t])
                continue;
            if (corners[t * 3] == v || corners[t * 3 + 1] == v || corners[t * 3 + 2] == v)
                wing_scratch.push_back(int(t));
        }
        return wing_scratch;
    };

    RegularizeRejectStats &stats = result.reject_stats;

    const auto try_collapse = [&](int u, int v) -> bool {
        if (u == v)
            return false;
        if (frozen_vert[size_t(u)] || frozen_vert[size_t(v)]) { ++stats.frozen; return false; }

        // Two wings means a manifold interior edge.
        std::vector<int> &wings = triangles_sharing_edge(u, v);
        if (wings.size() != 2) { ++stats.wing_count; return false; }
        const size_t w0 = size_t(wings[0]), w1 = size_t(wings[1]);
        const int    apex1 = third_vertex(w0, u, v), apex2 = third_vertex(w1, u, v);
        if (apex1 == apex2) { ++stats.folded_apex; return false; }

        // The edge cap loosens if *either* wing is extreme, since the re-subdivision recovers an
        // over-long edge. The normal cap needs *both*, which is what protects fillets.
        const double w1a = tri_aspect_sq(w0), w2a = tri_aspect_sq(w1);
        const bool   either_extreme = w1a > extreme_aspect2 || w2a > extreme_aspect2;
        const bool   both_extreme   = w1a > extreme_aspect2 && w2a > extreme_aspect2;
        const double eff_max_len_sq = either_extreme ? aggr_max_len_sq : base_max_len_sq;
        const double eff_normal_cos =
            both_extreme ? opts.aggressive_normal_delta_cos : opts.max_normal_delta_cos;

        // A vertex sharing a triangle with both endpoints, other than the wing apexes, would go
        // non-manifold. Stamp one side's neighbours, scan the other against them.
        ++stamp_gen;
        for (int s = slots.head[size_t(v)]; s != -1; s = slots.next[size_t(s)]) {
            const size_t t = size_t(s) / 3;
            if (tri_deleted[t])
                continue;
            for (int k = 0; k < 3; ++k)
                if (const int x = corners[t * 3 + size_t(k)]; x != v)
                    vert_stamp[size_t(x)] = stamp_gen;
        }
        for (int s = slots.head[size_t(u)]; s != -1; s = slots.next[size_t(s)]) {
            const size_t t = size_t(s) / 3;
            if (tri_deleted[t])
                continue;
            for (int k = 0; k < 3; ++k) {
                const int x = corners[t * 3 + size_t(k)];
                if (x != u && x != v && x != apex1 && x != apex2 && vert_stamp[size_t(x)] == stamp_gen) {
                    ++stats.link_condition;
                    return false;
                }
            }
        }

        const Vec3d m = (vert[size_t(u)] + vert[size_t(v)]) * 0.5;

        // Everything using either endpoint; the wings are being deleted.
        ++stamp_gen;
        affected_scratch.clear();
        for (const int endpoint : { u, v })
            for (int s = slots.head[size_t(endpoint)]; s != -1; s = slots.next[size_t(s)]) {
                const size_t t = size_t(s) / 3;
                if (tri_deleted[t] || t == w0 || t == w1)
                    continue;
                if (tri_stamp[t] != stamp_gen) {
                    tri_stamp[t] = stamp_gen;
                    affected_scratch.push_back(int(t));
                }
            }

        // Validate every affected triangle before touching anything.
        for (const int ti : affected_scratch) {
            const size_t t = size_t(ti);
            Vec3d        p[3];
            for (int k = 0; k < 3; ++k) {
                const int x = corners[t * 3 + size_t(k)];
                p[k] = (x == u || x == v) ? m : vert[size_t(x)];
            }
            const double ab2 = (p[1] - p[0]).squaredNorm();
            const double bc2 = (p[2] - p[1]).squaredNorm();
            const double ca2 = (p[0] - p[2]).squaredNorm();
            if (ab2 > eff_max_len_sq || bc2 > eff_max_len_sq || ca2 > eff_max_len_sq) {
                ++stats.edge_cap;
                return false;
            }
            const Vec3d  n    = (p[1] - p[0]).cross(p[2] - p[0]);
            const double nlen = n.norm();
            if (nlen <= 0.0) { ++stats.degenerate; return false; }
            if ((n / nlen).dot(orig_nrm[t]) < eff_normal_cos) { ++stats.normal_change; return false; }
        }

        // Apply: move u to the merged position and redirect every reference to v.
        vert[size_t(u)] = m;
        for (const size_t w : { w0, w1 }) {
            tri_deleted[w] = 1;
            for (int k = 0; k < 3; ++k)
                slots.unlink(int(w * 3) + k, corners);
        }
        // A non-wing triangle contains v exactly once, so moving its slots suffices.
        for (int s = slots.head[size_t(v)]; s != -1;) {
            const int ns = slots.next[size_t(s)];
            slots.unlink(s, corners);
            corners[size_t(s)] = u;
            slots.link(s, corners);
            recompute_face_normal(size_t(s) / 3);
            s = ns;
        }
        for (int s = slots.head[size_t(u)]; s != -1; s = slots.next[size_t(s)]) {
            const size_t t = size_t(s) / 3;
            if (!tri_deleted[t])
                recompute_face_normal(t);
        }
        return true;
    };

    for (int round = 0; round < opts.maxrounds; ++round) {
        // Rebuilt each round so earlier collapses inform the priorities.
        std::vector<int>    cand;
        std::vector<double> cand_aspect;
        for (size_t t = 0; t < tri_count; ++t) {
            if (tri_deleted[t])
                continue;
            const int a = corners[t * 3], b = corners[t * 3 + 1], c = corners[t * 3 + 2];
            if (std::min({ sq_dist(a, b), sq_dist(b, c), sq_dist(c, a) }) <= 0.0)
                continue;
            const double aspect2 = tri_aspect_sq(t);
            if (aspect2 < aspect_thr2)
                continue;
            cand.push_back(int(t));
            cand_aspect.push_back(aspect2);
        }
        // Worst first; ties keep ascending order so the pass is deterministic.
        std::vector<int> order(cand.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(),
                         [&](int x, int y) { return cand_aspect[size_t(x)] > cand_aspect[size_t(y)]; });

        size_t round_collapses = 0;
        for (const int oi : order) {
            const size_t t = size_t(cand[size_t(oi)]);
            if (tri_deleted[t])
                continue;
            const int a = corners[t * 3], b = corners[t * 3 + 1], c = corners[t * 3 + 2];
            // All three edges, shortest first: a sliver straddling a seam has its shortest edge
            // crossing it, which the normal gate refuses, while a long edge along one surface
            // collapses safely. Trying only the shortest would leave those stuck.
            struct Cand { double len2; int u, v; };
            Cand e[3] = { { sq_dist(a, b), a, b }, { sq_dist(b, c), b, c }, { sq_dist(c, a), c, a } };
            std::stable_sort(std::begin(e), std::end(e),
                             [](const Cand &x, const Cand &y) { return x.len2 < y.len2; });
            if (try_collapse(e[0].u, e[0].v) || try_collapse(e[1].u, e[1].v) ||
                try_collapse(e[2].u, e[2].v))
                ++round_collapses;
        }
        result.collapse_count += round_collapses;
        if (round_collapses == 0)
            break;
    }

    // Drop deleted triangles and rebuild the soup.
    const bool have_weights = !geometry.exclude_weight.empty();
    std::vector<int> out_parent;
    TriSoup         &out = result.geometry;
    for (size_t t = 0; t < tri_count; ++t) {
        if (tri_deleted[t])
            continue;
        for (int k = 0; k < 3; ++k)
            out.pos.push_back(vert[size_t(corners[t * 3 + size_t(k)])].cast<float>());
        if (have_weights) {
            // Constant across a face's corners.
            const float w = geometry.exclude_weight[t * 3];
            out.exclude_weight.insert(out.exclude_weight.end(), { w, w, w });
        }
        out_parent.push_back(result.face_parent_id[t]);
    }
    result.face_parent_id = std::move(out_parent);

    // Rebuilt from the compacted geometry - the collapses moved vertices.
    out.nrm.assign(out.pos.size(), Vec3f::Zero());
    {
        std::vector<Vec3d> accum(out.pos.size(), Vec3d::Zero());
        QuantizedPointMap  weld(WELD_GRID_GEOMETRY, out.pos.size());
        std::vector<int>   vid(out.pos.size());
        int                next = 0;
        for (size_t i = 0; i < out.pos.size(); ++i) {
            vid[i] = weld.get_or_set(out.pos[i], next);
            if (weld.inserted())
                ++next;
        }
        std::vector<Vec3d> vn(size_t(next), Vec3d::Zero());
        for (size_t t = 0; t * 3 < out.pos.size(); ++t) {
            const Vec3d a = out.pos[t * 3].cast<double>();
            const Vec3d n = (out.pos[t * 3 + 1].cast<double>() - a).cross(out.pos[t * 3 + 2].cast<double>() - a);
            for (int k = 0; k < 3; ++k)
                vn[size_t(vid[t * 3 + size_t(k)])] += n;
        }
        for (size_t i = 0; i < out.pos.size(); ++i) {
            const Vec3d &n = vn[size_t(vid[i])];
            const double l = n.norm();
            out.nrm[i] = (l > 0.0) ? Vec3d(n / l).cast<float>() : Vec3f(0.f, 0.f, 1.f);
        }
    }
    return result;
}

} // namespace TextureBake
} // namespace Slic3r
