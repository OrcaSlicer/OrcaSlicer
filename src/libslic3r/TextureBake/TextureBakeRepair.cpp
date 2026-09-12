#include "TextureBakeRepair.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace Slic3r {
namespace TextureBake {

namespace {

inline uint64_t edge_key(int a, int b)
{
    const uint32_t lo = uint32_t(std::min(a, b)), hi = uint32_t(std::max(a, b));
    return (uint64_t(lo) << 32) | uint64_t(hi);
}

// On the export grid a squared cross product is either 0 (collinear) or at least about 1e-16, the
// smallest real triangle being one grid unit per leg, so this separates the two cleanly.
constexpr double DEGENERATE_AREA_SQ = 1e-18;

} // namespace

EdgeDefects count_edge_defects(const TriSoup &geometry, double quant)
{
    EdgeDefects       out;
    const size_t      n = geometry.pos.size();
    out.triangles = n / 3;
    QuantizedPointMap vmap(quant, std::min(n, size_t(1) << 22));
    std::vector<int>  id(n);
    int               next = 0;
    for (size_t i = 0; i < n; ++i) {
        id[i] = vmap.get_or_set(geometry.pos[i], next);
        if (vmap.inserted())
            ++next;
    }
    std::unordered_map<uint64_t, int> counts;
    for (size_t t = 0; t + 2 < n; t += 3) {
        const int a = id[t], b = id[t + 1], c = id[t + 2];
        if (a == b || b == c || a == c)
            continue;
        const int tri[3] = { a, b, c };
        for (int e = 0; e < 3; ++e)
            ++counts[edge_key(tri[e], tri[(e + 1) % 3])];
    }
    for (const auto &[key, c] : counts) {
        (void) key;
        if (c == 1)      ++out.open;
        else if (c > 2)  ++out.non_manifold;
    }
    return out;
}

size_t count_area_slivers(const TriSoup &geometry)
{
    size_t n = 0;
    for (size_t t = 0; t + 2 < geometry.pos.size(); t += 3) {
        const Vec3d u = (geometry.pos[t + 1] - geometry.pos[t]).cast<double>();
        const Vec3d v = (geometry.pos[t + 2] - geometry.pos[t]).cast<double>();
        // The threshold a slicer applies: area below 1e-12 mm^2.
        if (u.cross(v).squaredNorm() < 1e-24)
            ++n;
    }
    return n;
}

TriSoup resolve_t_junctions(const TriSoup &geometry, const RepairOptions &opts)
{
    const size_t n_tri  = geometry.triangle_count();
    const double on_tol2 = opts.on_seg_tol * opts.on_seg_tol;
    const double Q       = opts.weld_quant;

    // Snapped, not just welded: keeping unrounded coordinates lets a thin triangle pass the
    // degeneracy test here and then collapse to collinear once the file is written, punching the very
    // hole this pass prevents. Snapping makes the check see what will be written.
    QuantizedPointMap  vmap(Q, std::min(n_tri * 3, size_t(1) << 22));
    std::vector<Vec3d> vert;
    std::vector<int>   vid(n_tri * 3);
    for (size_t i = 0; i < n_tri * 3; ++i) {
        const Vec3f &p  = geometry.pos[i];
        const int    id = vmap.get_or_set(p, int(vert.size()));
        if (vmap.inserted())
            vert.emplace_back(double(grid_round(double(p.x()) * Q)) / Q,
                              double(grid_round(double(p.y()) * Q)) / Q,
                              double(grid_round(double(p.z()) * Q)) / Q);
        vid[i] = id;
    }

    // Dropped: faces whose corners welded together, and needles - distinct but collinear on this
    // grid. A needle reads as watertight yet is deleted downstream, and dropping it leaves exactly
    // the on-edge-vertex topology the pass below closes.
    std::vector<std::array<int, 3>> faces;
    faces.reserve(n_tri);
    for (size_t t = 0; t < n_tri; ++t) {
        const int a = vid[t * 3], b = vid[t * 3 + 1], c = vid[t * 3 + 2];
        if (a == b || b == c || a == c)
            continue;
        const Vec3d u = vert[size_t(b)] - vert[size_t(a)];
        const Vec3d w = vert[size_t(c)] - vert[size_t(a)];
        if (u.cross(w).squaredNorm() < DEGENERATE_AREA_SQ)
            continue;
        faces.push_back({ a, b, c });
    }

    for (int iter = 0; iter < opts.max_iters; ++iter) {
        std::unordered_map<uint64_t, int> e_count;
        for (const auto &f : faces)
            for (int e = 0; e < 3; ++e)
                ++e_count[edge_key(f[size_t(e)], f[size_t((e + 1) % 3)])];

        std::unordered_set<int> bverts;
        for (const auto &[key, c] : e_count) {
            if (c != 1)
                continue;
            bverts.insert(int(uint32_t(key >> 32)));
            bverts.insert(int(uint32_t(key & 0xFFFFFFFFu)));
        }
        if (bverts.empty())
            break;
        const std::vector<int> bv(bverts.begin(), bverts.end());

        struct Split { int a, b; std::vector<int> mids; };
        std::unordered_map<size_t, Split> splits;
        for (size_t fi = 0; fi < faces.size(); ++fi) {
            const auto &f = faces[fi];
            for (int e = 0; e < 3; ++e) {
                const int a = f[size_t(e)], b = f[size_t((e + 1) % 3)];
                if (e_count[edge_key(a, b)] != 1)
                    continue; // only a boundary edge carries an unresolved T-junction
                const Vec3d  A     = vert[size_t(a)];
                const Vec3d  ev    = vert[size_t(b)] - A;
                const double elen2 = ev.squaredNorm();
                if (elen2 < 1e-20)
                    continue;
                std::vector<std::pair<double, int>> found;
                for (const int c : bv) {
                    if (c == a || c == b)
                        continue;
                    const Vec3d  cv = vert[size_t(c)] - A;
                    const double tp = cv.dot(ev) / elen2;
                    if (tp <= 1e-4 || tp >= 1.0 - 1e-4)
                        continue; // strictly between the ends
                    if ((cv - ev * tp).squaredNorm() < on_tol2)
                        found.emplace_back(tp, c);
                }
                if (!found.empty()) {
                    std::sort(found.begin(), found.end(),
                              [](const auto &x, const auto &y) { return x.first < y.first; });
                    Split sp{ a, b, {} };
                    for (const auto &m : found)
                        sp.mids.push_back(m.second);
                    splits.emplace(fi, std::move(sp));
                    break; // one site per face per pass; iteration handles cascades
                }
            }
        }
        if (splits.empty())
            break;

        std::vector<std::array<int, 3>> next;
        next.reserve(faces.size() + splits.size() * 2);
        for (size_t fi = 0; fi < faces.size(); ++fi) {
            const auto it = splits.find(fi);
            if (it == splits.end()) {
                next.push_back(faces[fi]);
                continue;
            }
            const auto &f  = faces[fi];
            const auto &sp = it->second;
            const int   apex = (f[0] != sp.a && f[0] != sp.b) ? f[0]
                               : (f[1] != sp.a && f[1] != sp.b) ? f[1]
                                                                : f[2];
            // Walk the base the way the face already traverses it, so the winding survives.
            bool dir_ab = false;
            for (int e = 0; e < 3; ++e)
                if (f[size_t(e)] == sp.a && f[size_t((e + 1) % 3)] == sp.b) {
                    dir_ab = true;
                    break;
                }
            std::vector<int> seq;
            if (dir_ab) {
                seq.push_back(sp.a);
                seq.insert(seq.end(), sp.mids.begin(), sp.mids.end());
                seq.push_back(sp.b);
            } else {
                seq.push_back(sp.b);
                seq.insert(seq.end(), sp.mids.rbegin(), sp.mids.rend());
                seq.push_back(sp.a);
            }
            for (size_t s = 0; s + 1 < seq.size(); ++s)
                next.push_back({ seq[s], seq[s + 1], apex });
        }
        faces.swap(next);
    }

    TriSoup out;
    out.pos.reserve(faces.size() * 3);
    out.nrm.reserve(faces.size() * 3);
    for (const auto &f : faces) {
        const Vec3f a = vert[size_t(f[0])].cast<float>();
        const Vec3f b = vert[size_t(f[1])].cast<float>();
        const Vec3f c = vert[size_t(f[2])].cast<float>();
        Vec3f       nrm = (b - a).cross(c - a);
        const float len = nrm.norm();
        nrm = (len > 0.f) ? Vec3f(nrm / len) : Vec3f(0.f, 0.f, 1.f);
        out.pos.insert(out.pos.end(), { a, b, c });
        out.nrm.insert(out.nrm.end(), { nrm, nrm, nrm });
    }
    return out;
}

} // namespace TextureBake
} // namespace Slic3r
