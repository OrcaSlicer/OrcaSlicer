#include "TextureBakeFlip.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>

namespace Slic3r {
namespace TextureBake {

FlipResult flip_edges_to_height(const TriSoup &geometry, const std::vector<int> &face_parent_id,
                                const HeightSampleFn &sample, const FlipSettings &settings,
                                const std::vector<uint8_t> &locked)
{
    FlipResult result;
    result.geometry       = geometry;
    result.face_parent_id = face_parent_id;
    const size_t count    = geometry.pos.size();
    const size_t tri_ct   = count / 3;
    if (count == 0 || !sample || settings.passes <= 0)
        return result;

    // Weld, so a flip rewrites triangles in terms of shared vertices rather than positions.
    QuantizedPointMap  weld(WELD_GRID_GEOMETRY, std::min(count, size_t(1) << 22));
    std::vector<int>   vid(count);
    std::vector<Vec3f> pos;
    std::vector<float> weight; // exclude weight per unique vertex, the max over its copies
    const bool         has_weight = !geometry.exclude_weight.empty();
    for (size_t i = 0; i < count; ++i) {
        vid[i] = weld.get_or_set(geometry.pos[i], int(pos.size()));
        if (weld.inserted()) {
            pos.push_back(geometry.pos[i]);
            weight.push_back(has_weight ? geometry.exclude_weight[i] : 0.f);
        } else if (has_weight) {
            weight[size_t(vid[i])] = std::max(weight[size_t(vid[i])], geometry.exclude_weight[i]);
        }
    }
    const size_t nv = pos.size();

    // Triangles as vertex ids; a locked or excluded triangle never takes part.
    std::vector<std::array<int, 3>> tri(tri_ct);
    std::vector<uint8_t>            fixed(tri_ct, 0);
    for (size_t t = 0; t < tri_ct; ++t) {
        tri[t] = { vid[t * 3], vid[t * 3 + 1], vid[t * 3 + 2] };
        if (!locked.empty() && t < locked.size() && locked[t])
            fixed[t] = 1;
        if (has_weight && geometry.exclude_weight[t * 3] > 0.99f)
            fixed[t] = 1;
    }

    // Height per unique vertex along its area-weighted normal, the direction displacement will use.
    std::vector<Vec3f> nrm(nv, Vec3f::Zero());
    std::vector<Vec3f> face_n(tri_ct);
    const auto rebuild_normals = [&]() {
        std::fill(nrm.begin(), nrm.end(), Vec3f::Zero());
        for (size_t t = 0; t < tri_ct; ++t) {
            const Vec3f fn = (pos[size_t(tri[t][1])] - pos[size_t(tri[t][0])]).cross(pos[size_t(tri[t][2])] - pos[size_t(tri[t][0])]);
            face_n[t]      = fn;
            for (int k = 0; k < 3; ++k)
                nrm[size_t(tri[t][size_t(k)])] += fn;
        }
        for (Vec3f &n : nrm) {
            const float l = n.norm();
            n = (l > 0.f) ? Vec3f(n / l) : Vec3f(0.f, 0.f, 1.f);
        }
    };
    rebuild_normals();
    std::vector<float> h(nv, 0.f);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, nv), [&](const tbb::blocked_range<size_t> &r) {
        for (size_t v = r.begin(); v < r.end(); ++v)
            h[v] = sample(pos[v], nrm[v], nrm[v]);
    });
    float h_lo = std::numeric_limits<float>::max(), h_hi = -h_lo;
    for (const float x : h) { h_lo = std::min(h_lo, x); h_hi = std::max(h_hi, x); }
    const float range = h_hi - h_lo;
    if (!(range > 0.f))
        return result; // flat: every diagonal is as good as the other
    const float min_gain = float(settings.min_gain_fraction) * range;
    const float planar   = float(settings.min_planar_cos);

    struct Candidate
    {
        uint32_t t1, t2;   // the two triangles
        uint8_t  k1, k2;   // corner index in each where the shared edge starts (t1: a->c, t2: c->a)
        float    gain;
    };

    for (int pass = 0; pass < settings.passes; ++pass) {
        // Half-edges keyed by their undirected edge, sorted so the two halves of an interior edge land
        // next to each other; a run of exactly two with opposite directions is a manifold interior
        // edge. Sorting beats hashing here by an order of magnitude on a few million triangles.
        struct Half { uint64_t key; uint32_t corner; };
        std::vector<Half> half;
        half.reserve(count);
        for (size_t t = 0; t < tri_ct; ++t)
            for (int k = 0; k < 3; ++k) {
                const int from = tri[t][size_t(k)], to = tri[t][size_t((k + 1) % 3)];
                if (from == to) continue;
                const uint32_t lo = uint32_t(std::min(from, to)), hi = uint32_t(std::max(from, to));
                half.push_back({ (uint64_t(lo) << 32) | hi, uint32_t(t * 3 + size_t(k)) });
            }
        tbb::parallel_sort(half.begin(), half.end(), [](const Half &x, const Half &y) {
            return x.key != y.key ? x.key < y.key : x.corner < y.corner;
        });
        std::vector<std::pair<uint32_t, uint32_t>> edges; // (corner in t1, corner in t2), t1 < t2
        edges.reserve(half.size() / 2);
        for (size_t i = 0; i < half.size();) {
            size_t j = i + 1;
            while (j < half.size() && half[j].key == half[i].key) ++j;
            if (j - i == 2) {
                // Opposite directions: the lower vertex id is `from` in exactly one of the two.
                const uint32_t c1 = half[i].corner, c2 = half[i + 1].corner;
                const int f1 = tri[c1 / 3][size_t(c1 % 3)], f2 = tri[c2 / 3][size_t(c2 % 3)];
                if (f1 != f2)
                    edges.emplace_back(c1, c2);
            }
            i = j;
        }
        std::vector<Candidate> cands(edges.size());
        std::vector<uint8_t>   valid(edges.size(), 0);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, edges.size()), [&](const tbb::blocked_range<size_t> &r) {
            for (size_t i = r.begin(); i < r.end(); ++i) {
                const uint32_t c1 = uint32_t(edges[i].first), c2 = uint32_t(edges[i].second);
                const uint32_t t1 = c1 / 3, t2 = c2 / 3;
                const int      k1 = int(c1 % 3), k2 = int(c2 % 3);
                if (fixed[t1] || fixed[t2]) continue;
                if (!face_parent_id.empty() && face_parent_id[t1] != face_parent_id[t2]) continue;
                // a->c is the shared edge in t1, with b opposite; t2 runs c->a with d opposite.
                const int a = tri[t1][size_t(k1)], c = tri[t1][size_t((k1 + 1) % 3)], b = tri[t1][size_t((k1 + 2) % 3)];
                const int d = tri[t2][size_t((k2 + 2) % 3)];
                if (b == d) continue;
                // Level quads have nothing to gain; the test is on the corners, before any sampling.
                const float hmin = std::min({ h[size_t(a)], h[size_t(b)], h[size_t(c)], h[size_t(d)] });
                const float hmax = std::max({ h[size_t(a)], h[size_t(b)], h[size_t(c)], h[size_t(d)] });
                if (hmax - hmin < min_gain) continue;
                // Coplanar enough to have a real alternative, and convex so the alternative is valid:
                // the new triangles (a, d, b) and (d, c, b) must both face the way the quad does, with
                // a decent share of its area.
                const Vec3f n1 = face_n[t1], n2 = face_n[t2];
                const float l1 = n1.norm(), l2 = n2.norm();
                if (l1 <= 0.f || l2 <= 0.f || n1.dot(n2) < planar * l1 * l2) continue;
                const Vec3f quad_n = (n1 + n2).normalized();
                const Vec3f &pa = pos[size_t(a)], &pb = pos[size_t(b)], &pc = pos[size_t(c)], &pd = pos[size_t(d)];
                const Vec3f m1 = (pd - pa).cross(pb - pa), m2 = (pc - pd).cross(pb - pd);
                const float area_old = l1 + l2, area_new = m1.dot(quad_n) + m2.dot(quad_n);
                const float min_part = 0.05f * area_old;
                if (m1.dot(quad_n) < min_part || m2.dot(quad_n) < min_part) continue;
                if (std::abs(area_new - area_old) > 0.02f * area_old) continue; // not the same quad: folded
                // The diagonals' midpoint errors.
                const auto mid_err = [&](int u, int w) {
                    const Vec3f p = 0.5f * (pos[size_t(u)] + pos[size_t(w)]);
                    Vec3f       n = nrm[size_t(u)] + nrm[size_t(w)];
                    const float l = n.norm();
                    n = (l > 0.f) ? Vec3f(n / l) : quad_n;
                    return std::abs(sample(p, n, n) - 0.5f * (h[size_t(u)] + h[size_t(w)]));
                };
                const float gain = mid_err(a, c) - mid_err(b, d);
                if (gain < min_gain) continue;
                cands[i] = { t1, t2, uint8_t(k1), uint8_t(k2), gain };
                valid[i] = 1;
            }
        });
        std::vector<Candidate> chosen;
        for (size_t i = 0; i < cands.size(); ++i)
            if (valid[i]) chosen.push_back(cands[i]);
        std::sort(chosen.begin(), chosen.end(), [](const Candidate &x, const Candidate &y) {
            return x.gain != y.gain ? x.gain > y.gain : (x.t1 != y.t1 ? x.t1 < y.t1 : x.t2 < y.t2);
        });
        // Best first, and a triangle changes at most once per pass.
        std::vector<uint8_t> touched(tri_ct, 0);
        size_t               applied = 0;
        for (const Candidate &cd : chosen) {
            if (touched[cd.t1] || touched[cd.t2]) continue;
            const int a = tri[cd.t1][cd.k1], c = tri[cd.t1][size_t((cd.k1 + 1) % 3)], b = tri[cd.t1][size_t((cd.k1 + 2) % 3)];
            const int d = tri[cd.t2][size_t((cd.k2 + 2) % 3)];
            tri[cd.t1] = { a, d, b };
            tri[cd.t2] = { d, c, b };
            touched[cd.t1] = touched[cd.t2] = 1;
            ++applied;
        }
        result.flipped += applied;
        if (applied == 0)
            break;
        rebuild_normals(); // face normals feed the planarity test of the next pass
    }

    if (result.flipped == 0)
        return result;

    // Back to the soup: positions, weights and per-face normals from the (possibly rewritten) triangles.
    for (size_t t = 0; t < tri_ct; ++t) {
        for (int k = 0; k < 3; ++k) {
            const size_t i = t * 3 + size_t(k);
            const int    v = tri[t][size_t(k)];
            result.geometry.pos[i] = pos[size_t(v)];
            if (has_weight)
                result.geometry.exclude_weight[i] = weight[size_t(v)];
        }
        Vec3f       n   = (result.geometry.pos[t * 3 + 1] - result.geometry.pos[t * 3]).cross(result.geometry.pos[t * 3 + 2] - result.geometry.pos[t * 3]);
        const float len = n.norm();
        n = (len > 0.f) ? Vec3f(n / len) : Vec3f(0.f, 0.f, 1.f);
        result.geometry.nrm[t * 3] = result.geometry.nrm[t * 3 + 1] = result.geometry.nrm[t * 3 + 2] = n;
    }
    return result;
}

} // namespace TextureBake
} // namespace Slic3r
