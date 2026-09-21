#include "TextureBakeDecimate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>

#include <boost/log/trivial.hpp>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

namespace Slic3r {
namespace TextureBake {

namespace {

// Symmetric 4x4 quadric, as its 10 upper-triangle values.
struct Quadric
{
    std::array<double, 10> q{};

    void add_plane(double a, double b, double c, double d)
    {
        q[0] += a * a; q[1] += a * b; q[2] += a * c; q[3] += a * d;
                       q[4] += b * b; q[5] += b * c; q[6] += b * d;
                                      q[7] += c * c; q[8] += c * d;
                                                     q[9] += d * d;
    }
    void operator+=(const Quadric &o)
    {
        for (int i = 0; i < 10; ++i)
            q[size_t(i)] += o.q[size_t(i)];
    }
    double eval(double x, double y, double z) const
    {
        return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x +
               q[4] * y * y + 2 * q[5] * y * z + 2 * q[6] * y +
               q[7] * z * z + 2 * q[8] * z + q[9];
    }
};

double eval_sum(const std::vector<Quadric> &qs, int v1, int v2, const Vec3d &p)
{
    return qs[size_t(v1)].eval(p.x(), p.y(), p.z()) + qs[size_t(v2)].eval(p.x(), p.y(), p.z());
}

// The position minimising the summed quadric, if the system is well conditioned enough to trust.
bool solve_q(const std::vector<Quadric> &qs, int v1, int v2, Vec3d &out)
{
    const auto &A = qs[size_t(v1)].q;
    const auto &B = qs[size_t(v2)].q;
    const double a00 = A[0] + B[0], a01 = A[1] + B[1], a02 = A[2] + B[2];
    const double a11 = A[4] + B[4], a12 = A[5] + B[5], a22 = A[7] + B[7];
    const double b0 = -(A[3] + B[3]), b1 = -(A[6] + B[6]), b2 = -(A[8] + B[8]);

    const double det = a00 * (a11 * a22 - a12 * a12) - a01 * (a01 * a22 - a12 * a02) +
                       a02 * (a01 * a12 - a11 * a02);
    const double max_el = std::max({ std::abs(a00), std::abs(a01), std::abs(a02), std::abs(a11),
                                     std::abs(a12), std::abs(a22) });
    // Scaled with the matrix, so it means the same at any model scale.
    const double threshold = max_el * max_el * max_el * 1e-10;
    if (std::abs(det) < std::max(threshold, 1e-30))
        return false;

    const double inv = 1.0 / det;
    out.x() = inv * (b0 * (a11 * a22 - a12 * a12) - a01 * (b1 * a22 - a12 * b2) + a02 * (b1 * a12 - a11 * b2));
    out.y() = inv * (a00 * (b1 * a22 - a12 * b2) - b0 * (a01 * a22 - a12 * a02) + a02 * (a01 * b2 - b1 * a02));
    out.z() = inv * (a00 * (a11 * b2 - b1 * a12) - a01 * (a01 * b2 - b1 * a02) + b0 * (a01 * a12 - a11 * a02));
    return true;
}

Vec3d face_normal_unit(const std::vector<Vec3d> &pos, int a, int b, int c)
{
    const Vec3d  n   = (pos[size_t(b)] - pos[size_t(a)]).cross(pos[size_t(c)] - pos[size_t(a)]);
    const double len = n.norm();
    return (len > 0.0) ? Vec3d(n / len) : Vec3d::Zero();
}

// Versions are captured at push time; a mismatch on pop means a later collapse invalidated the entry.
// Lazy deletion, far cheaper than removing entries eagerly - but it means the heap accumulates stale
// duplicates, so its size has to be reserved up front and it is compacted once the dead entries
// dominate (see maybe_compact below).
//
// The collapse target is not stored. A matching version stamp means neither endpoint's quadric nor its
// position has changed since the push, so the target recomputes to exactly the same value on pop - and
// the entry drops from 40 bytes to 24. Sifting is most of this stage's time, and it is memory traffic.
struct HeapEntry
{
    double   cost;
    int      v1, v2;
    uint32_t ver1, ver2;
    bool     operator>(const HeapEntry &o) const { return cost > o.cost; }
};

} // namespace

DecimateResult decimate(const TriSoup &geometry, size_t target_triangles, bool harvest_flat,
                        double harvest_tol, const std::vector<uint8_t> &locked_faces,
                        const DecimateProgressFn &on_progress, const std::vector<int> &face_color)
{
    DecimateResult result;
    const size_t   n = geometry.pos.size();
    if (n < 3) {
        result.geometry = geometry;
        return result;
    }

    // The finest grid. Anything coarser fuses distinct fine-feature vertices on a displaced mesh,
    // leaving it non-manifold before decimation starts and producing open edges afterwards.
    QuantizedPointMap  vert_map(WELD_GRID_DECIMATION, std::min(n, size_t(1) << 22));
    std::vector<Vec3d> pos;
    std::vector<int>   remap(n);
    for (size_t i = 0; i < n; ++i) {
        const int idx = vert_map.get_or_set(geometry.pos[i], int(pos.size()));
        if (vert_map.inserted())
            pos.push_back(geometry.pos[i].cast<double>());
        remap[i] = idx;
    }
    const size_t     vert_count = pos.size();
    const size_t     face_count = n / 3;
    std::vector<int> faces(face_count * 3);
    for (size_t i = 0; i < n; ++i)
        faces[i] = remap[i];

    if (face_count <= target_triangles && !harvest_flat) {
        result.geometry = geometry;
        return result;
    }

    // An edge with a locked endpoint never reaches the heap.
    std::vector<uint8_t> locked_vert;
    size_t               locked_face_count = 0;
    if (!locked_faces.empty()) {
        locked_vert.assign(vert_count, 0);
        for (size_t f = 0; f < face_count && f < locked_faces.size(); ++f) {
            if (!locked_faces[f])
                continue;
            ++locked_face_count;
            for (int k = 0; k < 3; ++k)
                locked_vert[size_t(faces[f * 3 + size_t(k)])] = 1;
        }
    }
    // With the locked faces alone at the target, chasing it would grind the free region to its guard
    // limit for nothing - harvest only, and say so.
    const bool locked_over_budget =
        !locked_vert.empty() && face_count > target_triangles && locked_face_count >= target_triangles;
    result.locked_over_budget = locked_over_budget;
    if (locked_over_budget && !harvest_flat) {
        result.geometry = geometry;
        return result;
    }

    std::vector<Quadric> quadrics(vert_count);
    {
        // The plane per face is independent; accumulating it into the three incident vertices is not,
        // so only the first half is parallel.
        std::vector<Vec4d> planes(face_count, Vec4d::Zero());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, face_count),
                          [&](const tbb::blocked_range<size_t> &range) {
                              for (size_t f = range.begin(); f < range.end(); ++f) {
                                  const int a = faces[f * 3], b = faces[f * 3 + 1], c = faces[f * 3 + 2];
                                  if (a < 0)
                                      continue;
                                  const Vec3d nrm = face_normal_unit(pos, a, b, c);
                                  if (nrm.isZero())
                                      continue;
                                  planes[f] = Vec4d(nrm.x(), nrm.y(), nrm.z(), -nrm.dot(pos[size_t(a)]));
                              }
                          });
        for (size_t f = 0; f < face_count; ++f) {
            const Vec4d &pl = planes[f];
            if (pl.head<3>().isZero())
                continue;
            for (int k = 0; k < 3; ++k)
                quadrics[size_t(faces[f * 3 + size_t(k)])].add_plane(pl.x(), pl.y(), pl.z(), pl.w());
        }
    }

    // Two penalty planes per endpoint on a sharp interior edge, each perpendicular to one adjacent
    // face and containing the edge, constraining the vertex to the crease line.
    {
        struct EdgeRec { int va, vb, f0, f1; uint8_t count; };
        std::vector<EdgeRec> edges;
        QuantizedPointMap    edge_idx(1.0, std::min(face_count * 3, size_t(1) << 22));
        for (size_t f = 0; f < face_count; ++f) {
            if (faces[f * 3] < 0)
                continue;
            for (int e = 0; e < 3; ++e) {
                const int va = faces[f * 3 + size_t(e)];
                const int vb = faces[f * 3 + size_t((e + 1) % 3)];
                const int lo = std::min(va, vb), hi = std::max(va, vb);
                const int ei = edge_idx.get_or_set_key(lo, hi, 0, int(edges.size()));
                if (edge_idx.inserted())
                    edges.push_back({ lo, hi, int(f), -1, 1 });
                else if (edges[size_t(ei)].count == 1) {
                    edges[size_t(ei)].f1    = int(f);
                    edges[size_t(ei)].count = 2;
                } else
                    // Non-manifold; never feeds a crease.
                    edges[size_t(ei)].count = 3;
            }
        }

        const double sqrt_w = std::sqrt(DECIMATE_CREASE_WEIGHT);
        for (const EdgeRec &er : edges) {
            if (er.count != 2)
                continue; // boundary or non-manifold
            const Vec3d n0 = face_normal_unit(pos, faces[size_t(er.f0) * 3], faces[size_t(er.f0) * 3 + 1],
                                              faces[size_t(er.f0) * 3 + 2]);
            const Vec3d n1 = face_normal_unit(pos, faces[size_t(er.f1) * 3], faces[size_t(er.f1) * 3 + 1],
                                              faces[size_t(er.f1) * 3 + 2]);
            const bool color_edge = face_color.size() > std::max(size_t(er.f0), size_t(er.f1)) &&
                                    face_color[size_t(er.f0)] != face_color[size_t(er.f1)];
            if (!color_edge && n0.dot(n1) >= DECIMATE_CREASE_COS)
                continue; // smooth enough to be no crease, and no colour changes across it

            const Vec3d  e    = pos[size_t(er.vb)] - pos[size_t(er.va)];
            const double elen = e.norm();
            if (elen <= 0.0)
                continue;
            const Vec3d ed = e / elen;
            for (const Vec3d &fn : { n0, n1 }) {
                Vec3d        pn   = fn.cross(ed);
                const double plen = pn.norm();
                if (plen < 1e-10)
                    continue; // edge parallel to the face normal
                pn /= plen;
                const double d = -pn.dot(pos[size_t(er.va)]);
                // sqrt(w) on the inputs gives w times the accumulated products.
                for (const int v : { er.va, er.vb })
                    quadrics[size_t(v)].add_plane(pn.x() * sqrt_w, pn.y() * sqrt_w, pn.z() * sqrt_w,
                                                  d * sqrt_w);
            }
        }
    }

    // Vertex-face incidence as intrusive linked lists of slots over flat arrays.
    const size_t     S = face_count * 3;
    std::vector<int> vf_head(vert_count, -1), slot_face(S), slot_vert(S), slot_next(S, -1),
        slot_prev(S, -1), face_slot(S, -1);
    for (size_t f = 0; f < face_count; ++f)
        for (int k = 0; k < 3; ++k) {
            const int s = int(f) * 3 + k;
            const int v = faces[size_t(s)];
            slot_face[size_t(s)] = int(f);
            slot_vert[size_t(s)] = v;
            slot_next[size_t(s)] = vf_head[size_t(v)];
            slot_prev[size_t(s)] = -1;
            if (vf_head[size_t(v)] >= 0)
                slot_prev[size_t(vf_head[size_t(v)])] = s;
            vf_head[size_t(v)]   = s;
            face_slot[size_t(s)] = s;
        }
    const auto unlink_slot = [&](int s) {
        const int p = slot_prev[size_t(s)], nx = slot_next[size_t(s)];
        if (p >= 0) slot_next[size_t(p)] = nx;
        else        vf_head[size_t(slot_vert[size_t(s)])] = nx;
        if (nx >= 0) slot_prev[size_t(nx)] = p;
    };
    const auto move_slot = [&](int s, int nv) {
        unlink_slot(s);
        slot_next[size_t(s)] = vf_head[size_t(nv)];
        slot_prev[size_t(s)] = -1;
        if (vf_head[size_t(nv)] >= 0)
            slot_prev[size_t(vf_head[size_t(nv)])] = s;
        vf_head[size_t(nv)]  = s;
        slot_vert[size_t(s)] = nv;
    };

    std::vector<uint8_t>  active(vert_count, 1);
    std::vector<uint32_t> version(vert_count, 0);
    std::vector<uint32_t> nb_stamp(vert_count, 0), lk_stamp(vert_count, 0);
    uint32_t              epoch = 1, lk_epoch = 1;
    size_t                active_faces = face_count;

    // A plain vector driven by the heap algorithms, so the capacity can be reserved. Lazy deletion
    // means roughly one entry per edge plus one per re-push after each collapse; the reserve below is
    // sized from the edge count and simply grows if a mesh needs more.
    std::vector<HeapEntry> heap;
    heap.reserve(std::min<size_t>(face_count * 3, size_t(1) << 24));
    const auto heap_push = [&](HeapEntry e) {
        heap.push_back(e);
        std::push_heap(heap.begin(), heap.end(), std::greater<HeapEntry>());
    };
    const auto heap_pop = [&]() {
        std::pop_heap(heap.begin(), heap.end(), std::greater<HeapEntry>());
        const HeapEntry e = heap.back();
        heap.pop_back();
        return e;
    };
    size_t pops = 0, stale_pops = 0, compactions = 0;

    // An entry is stale once either endpoint has been removed or moved by a later collapse.
    const auto is_stale = [&](const HeapEntry &e) {
        return !active[size_t(e.v1)] || !active[size_t(e.v2)] || version[size_t(e.v1)] != e.ver1 ||
               version[size_t(e.v2)] != e.ver2;
    };
    // Measured on a 2.4 M -> 750 k run, 82% of pops were stale: every collapse re-pushes the survivor's
    // edges and orphans the old ones, so the heap grows to several times the live edge set and every
    // sift walks that much further through memory. Dropping the dead entries and re-heapifying once
    // they dominate costs one linear pass, amortised against the growth that triggered it.
    //
    // Keyed to the live face count rather than to the heap's own size: lazy popping keeps the heap from
    // ever doubling, but the live edge set (about 1.5 per face) shrinks as decimation proceeds, so by the
    // end the heap is several times what is still collapsible. Compact once it passes twice that.
    const auto maybe_compact = [&]() {
        if (heap.size() < std::max<size_t>(size_t(1) << 16, active_faces * 3))
            return;
        heap.erase(std::remove_if(heap.begin(), heap.end(), is_stale), heap.end());
        std::make_heap(heap.begin(), heap.end(), std::greater<HeapEntry>());
        ++compactions;
    };

    // Where an edge collapses to. Also re-run on pop instead of stored - see HeapEntry.
    const auto collapse_target = [&](int v1, int v2) -> Vec3d {
        Vec3d p;
        if (!solve_q(quadrics, v1, v2, p)) {
            const Vec3d  mid = (pos[size_t(v1)] + pos[size_t(v2)]) * 0.5;
            const double e1  = eval_sum(quadrics, v1, v2, pos[size_t(v1)]);
            const double e2  = eval_sum(quadrics, v1, v2, pos[size_t(v2)]);
            const double em  = eval_sum(quadrics, v1, v2, mid);
            const double emin = std::min({ e1, e2, em });
            const double etol = emin * 1e-2 + 1e-12;
            // The midpoint when the three are near-equal, i.e. flat: it moves adjacent triangles
            // least, so fewer normal flips and no stalling on coplanar geometry.
            if (em <= emin + etol)      p = mid;
            else if (e1 <= e2)          p = pos[size_t(v1)];
            else                        p = pos[size_t(v2)];
        }
        return p;
    };
    const auto push_edge = [&](int v1, int v2) {
        const Vec3d p = collapse_target(v1, v2);
        // The cost is evaluated at the exact target and the collapse moves to its float rounding -
        // the same split as when the rounded target was stored in the entry, so no ordering changes.
        // Where quadric costs are all near zero, shorter edges first keeps triangle quality up.
        const double len2 = (pos[size_t(v2)] - pos[size_t(v1)]).squaredNorm();
        heap_push({ eval_sum(quadrics, v1, v2, p) + len2 * 1e-8, v1, v2, version[size_t(v1)],
                    version[size_t(v2)] });
    };

    {
        QuantizedPointMap seed_seen(1.0, std::min(face_count * 3, size_t(1) << 22));
        for (size_t f = 0; f < face_count; ++f) {
            if (faces[f * 3] < 0)
                continue;
            for (int e = 0; e < 3; ++e) {
                const int va = faces[f * 3 + size_t(e)];
                const int vb = faces[f * 3 + size_t((e + 1) % 3)];
                if (!locked_vert.empty() && (locked_vert[size_t(va)] || locked_vert[size_t(vb)]))
                    continue;
                seed_seen.get_or_set_key(std::min(va, vb), std::max(va, vb), 0, 1);
                if (seed_seen.inserted())
                    push_edge(va, vb);
            }
        }
    }

    // 0 means a stale entry, 1 a boundary edge, 2 or more safe.
    const auto shared_face_count = [&](int v1, int v2) {
        int count = 0;
        for (int s = vf_head[size_t(v1)]; s >= 0; s = slot_next[size_t(s)]) {
            const int f = slot_face[size_t(s)];
            if (faces[size_t(f) * 3] < 0)
                continue;
            for (int k = 0; k < 3; ++k)
                if (faces[size_t(f) * 3 + size_t(k)] == v2) {
                    if (++count >= 2)
                        return 2;
                    break;
                }
        }
        return count;
    };

    // Safe only when the sole common neighbours of the endpoints are the apexes of the faces the edge
    // already shares; any other would pile a third triangle onto an edge after the collapse.
    const auto has_link_violation = [&](int v1, int v2, uint32_t ep) {
        for (int s = vf_head[size_t(v1)]; s >= 0; s = slot_next[size_t(s)]) {
            const int f = slot_face[size_t(s)];
            if (faces[size_t(f) * 3] < 0)
                continue;
            for (int k = 0; k < 3; ++k)
                if (const int x = faces[size_t(f) * 3 + size_t(k)]; x != v1)
                    lk_stamp[size_t(x)] = ep;
        }
        int shared = 0;
        for (int s = vf_head[size_t(v1)]; s >= 0; s = slot_next[size_t(s)]) {
            const int f = slot_face[size_t(s)];
            if (faces[size_t(f) * 3] < 0)
                continue;
            const int a = faces[size_t(f) * 3], b = faces[size_t(f) * 3 + 1], c = faces[size_t(f) * 3 + 2];
            if (a == v2 || b == v2 || c == v2) {
                ++shared;
                const int apex = (a != v1 && a != v2) ? a : (b != v1 && b != v2) ? b : c;
                lk_stamp[size_t(apex)] = ep + 1; // a legal shared-face apex
            }
        }
        if (shared > 2)
            return true; // already non-manifold
        for (int s = vf_head[size_t(v2)]; s >= 0; s = slot_next[size_t(s)]) {
            const int f = slot_face[size_t(s)];
            if (faces[size_t(f) * 3] < 0)
                continue;
            for (int k = 0; k < 3; ++k) {
                const int x = faces[size_t(f) * 3 + size_t(k)];
                if (x != v2 && x != v1 && lk_stamp[size_t(x)] == ep)
                    return true;
            }
        }
        return false;
    };

    // Squared-dot, so no square root or division. Faces containing the other endpoint are the ones
    // being removed, so they are skipped.
    const auto check_flipped = [&](int vc, int vo, const Vec3d &np) {
        for (int s = vf_head[size_t(vc)]; s >= 0; s = slot_next[size_t(s)]) {
            const size_t f = size_t(slot_face[size_t(s)]);
            if (faces[f * 3] < 0)
                continue;
            const int fa = faces[f * 3], fb = faces[f * 3 + 1], fc = faces[f * 3 + 2];
            if (fa == vo || fb == vo || fc == vo)
                continue;
            const Vec3d oa = pos[size_t(fa)], ob = pos[size_t(fb)], oc = pos[size_t(fc)];
            const Vec3d on = (ob - oa).cross(oc - oa);
            const Vec3d na = (fa == vc) ? np : oa;
            const Vec3d nb = (fb == vc) ? np : ob;
            const Vec3d nc = (fc == vc) ? np : oc;
            const Vec3d nn = (nb - na).cross(nc - na);
            const double raw = on.dot(nn);
            if (raw < 0.0)
                return true;
            if (raw * raw < DECIMATE_FLIP_DOT * DECIMATE_FLIP_DOT * on.squaredNorm() * nn.squaredNorm())
                return true;
        }
        return false;
    };

    const size_t init_faces   = active_faces;
    const size_t to_remove    = std::max<size_t>(1, init_faces > target_triangles
                                                        ? init_faces - target_triangles : init_faces);
    const double harvest_ceil = harvest_tol * harvest_tol;
    bool         reached_target = locked_over_budget;
    double       last_progress  = 0.0;

    while (!heap.empty()) {
        if (active_faces <= target_triangles) {
            if (!harvest_flat)
                break;
            reached_target = true;
        }

        const HeapEntry top = heap_pop();
        ++pops;
        // The popped entry is the cheapest left, so exceeding the tolerance ends the run.
        if (reached_target && top.cost > harvest_ceil)
            break;

        const int v1 = top.v1, v2 = top.v2;
        if (is_stale(top)) {
            ++stale_pops;
            continue;
        }
        if (shared_face_count(v1, v2) < 2)
            continue;
        lk_epoch += 2; // +2 so ep and ep+1 cannot collide with the next call
        if (has_link_violation(v1, v2, lk_epoch))
            continue;
        const Vec3d target = collapse_target(v1, v2).cast<float>().cast<double>();
        if (check_flipped(v1, v2, target) || check_flipped(v2, v1, target))
            continue;

        // v1 survives at the new position, v2 goes.
        pos[size_t(v1)] = target;
        quadrics[size_t(v1)] += quadrics[size_t(v2)];
        ++version[size_t(v1)];

        for (int s = vf_head[size_t(v2)]; s >= 0;) {
            const size_t f      = size_t(slot_face[size_t(s)]);
            const int    s_next = slot_next[size_t(s)]; // read before the list is modified
            if (faces[f * 3] >= 0) {
                for (int k = 0; k < 3; ++k)
                    if (faces[f * 3 + size_t(k)] == v2) {
                        faces[f * 3 + size_t(k)] = v1;
                        break;
                    }
                const int fa = faces[f * 3], fb = faces[f * 3 + 1], fc = faces[f * 3 + 2];
                if (fa == fb || fb == fc || fa == fc) {
                    for (int k = 0; k < 3; ++k)
                        if (const int sk = face_slot[f * 3 + size_t(k)]; sk >= 0) {
                            unlink_slot(sk);
                            face_slot[f * 3 + size_t(k)] = -1;
                        }
                    faces[f * 3] = faces[f * 3 + 1] = faces[f * 3 + 2] = -1;
                    --active_faces;
                } else
                    move_slot(s, v1);
            }
            s = s_next;
        }
        active[size_t(v2)] = 0;

        ++epoch;
        for (int sv = vf_head[size_t(v1)]; sv >= 0; sv = slot_next[size_t(sv)]) {
            const size_t f = size_t(slot_face[size_t(sv)]);
            if (faces[f * 3] < 0)
                continue;
            for (int k = 0; k < 3; ++k) {
                const int nb = faces[f * 3 + size_t(k)];
                if (nb == v1 || nb_stamp[size_t(nb)] == epoch)
                    continue;
                nb_stamp[size_t(nb)] = epoch;
                // v1 is never locked - a locked edge never entered the heap.
                if (active[size_t(nb)] && (locked_vert.empty() || !locked_vert[size_t(nb)]))
                    push_edge(v1, nb);
            }
        }
        maybe_compact();

        if (on_progress) {
            const double p = std::min(1.0, double(init_faces - active_faces) / double(to_remove));
            if (p - last_progress > 0.005) {
                last_progress = p;
                if (!on_progress(p))
                    break;
            }
        }
    }

    BOOST_LOG_TRIVIAL(info) << "TextureBake decimate: pops=" << pops << " stale=" << stale_pops
                            << " compactions=" << compactions << " heap_peak=" << heap.capacity()
                            << " faces=" << active_faces;

    // Rebuild from the surviving faces, with per-face normals.
    TriSoup &out = result.geometry;
    for (size_t f = 0; f < face_count; ++f) {
        if (faces[f * 3] < 0)
            continue;
        const Vec3f a = pos[size_t(faces[f * 3])].cast<float>();
        const Vec3f b = pos[size_t(faces[f * 3 + 1])].cast<float>();
        const Vec3f c = pos[size_t(faces[f * 3 + 2])].cast<float>();
        const Vec3f nrm = (b - a).cross(c - a).normalized();
        out.pos.insert(out.pos.end(), { a, b, c });
        out.nrm.insert(out.nrm.end(), { nrm, nrm, nrm });
    }
    return result;
}

} // namespace TextureBake
} // namespace Slic3r
