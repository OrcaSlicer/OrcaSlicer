#include "TextureBakeRepair.hpp"
#include "libslic3r/TextureBake/TextureBakeIndex.hpp"
#include "libslic3r/Point.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

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

TriSoup resolve_t_junctions(const TriSoup &geometry, const RepairOptions &opts,
                            std::vector<int> *face_color)
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
    // Parallel to `faces` throughout, so a split or a dropped degenerate keeps the two in step.
    const bool       track_color = face_color != nullptr && !face_color->empty();
    std::vector<int> colors;
    faces.reserve(n_tri);
    if (track_color)
        colors.reserve(n_tri);
    for (size_t t = 0; t < n_tri; ++t) {
        const int a = vid[t * 3], b = vid[t * 3 + 1], c = vid[t * 3 + 2];
        if (a == b || b == c || a == c)
            continue;
        const Vec3d u = vert[size_t(b)] - vert[size_t(a)];
        const Vec3d w = vert[size_t(c)] - vert[size_t(a)];
        if (u.cross(w).squaredNorm() < DEGENERATE_AREA_SQ)
            continue;
        faces.push_back({ a, b, c });
        if (track_color)
            colors.push_back(t < face_color->size() ? (*face_color)[t] : -1);
    }

    // Snapping to the export grid can collapse two distinct fine-grid vertices into one cell and leave
    // pairs of duplicate or opposite-wound triangles sharing all three vertices (zero-volume double-sided
    // fins whose edges with the rest of the mesh carry 3 or 4 incident faces). Strip any duplicate
    // triangle whose removal resolves a non-manifold edge before running the T-junction pass.
    auto strip_nonmanifold_duplicate_faces = [&]() {
        std::unordered_map<uint64_t, int> e_count;
        for (const auto &f : faces)
            for (int e = 0; e < 3; ++e)
                ++e_count[edge_key(f[size_t(e)], f[size_t((e + 1) % 3)])];
        bool any_nm = false;
        for (const auto &[key, c] : e_count)
            if (c > 2) {
                any_nm = true;
                break;
            }
        if (!any_nm)
            return;

        struct TriKey {
            int a, b, c;
            bool operator==(const TriKey &o) const { return a == o.a && b == o.b && c == o.c; }
        };
        struct TriKeyHash {
            size_t operator()(const TriKey &k) const {
                return (size_t(uint32_t(k.a)) * 73856093u) ^ (size_t(uint32_t(k.b)) * 19349663u) ^
                       (size_t(uint32_t(k.c)) * 83492791u);
            }
        };
        std::unordered_map<TriKey, std::vector<size_t>, TriKeyHash> groups;
        for (size_t fi = 0; fi < faces.size(); ++fi) {
            std::array<int, 3> s = faces[fi];
            std::sort(s.begin(), s.end());
            groups[TriKey{ s[0], s[1], s[2] }].push_back(fi);
        }
        std::vector<uint8_t> drop(faces.size(), 0);
        bool                 dropped_any = false;
        for (const auto &[tk, list] : groups) {
            if (list.size() < 2)
                continue;
            const uint64_t ek0 = edge_key(tk.a, tk.b);
            const uint64_t ek1 = edge_key(tk.b, tk.c);
            const uint64_t ek2 = edge_key(tk.c, tk.a);
            // If all three edges have >2 incident faces, a pair of opposite-wound fins can both be cancelled.
            // If at least one edge has >2 incident faces, extra duplicate copies beyond the first are dropped.
            size_t idx = 0;
            while (idx + 1 < list.size() && e_count[ek0] > 2 && e_count[ek1] > 2 && e_count[ek2] > 2) {
                drop[list[idx]]     = 1;
                drop[list[idx + 1]] = 1;
                e_count[ek0] -= 2;
                e_count[ek1] -= 2;
                e_count[ek2] -= 2;
                idx += 2;
                dropped_any = true;
            }
            for (size_t k = std::max<size_t>(1, idx); k < list.size(); ++k) {
                if (e_count[ek0] > 2 || e_count[ek1] > 2 || e_count[ek2] > 2) {
                    drop[list[k]] = 1;
                    --e_count[ek0];
                    --e_count[ek1];
                    --e_count[ek2];
                    dropped_any = true;
                }
            }
        }
        if (dropped_any) {
            std::vector<std::array<int, 3>> kept;
            std::vector<int>                kept_colors;
            kept.reserve(faces.size());
            if (track_color)
                kept_colors.reserve(colors.size());
            for (size_t fi = 0; fi < faces.size(); ++fi) {
                if (drop[fi])
                    continue;
                kept.push_back(faces[fi]);
                if (track_color)
                    kept_colors.push_back(colors[fi]);
            }
            faces.swap(kept);
            if (track_color)
                colors.swap(kept_colors);
        }
    };
    strip_nonmanifold_duplicate_faces();

    // If snapping to the export grid still merged distinct fine-grid vertices across a link violation
    // (leaving an edge with >2 incident faces that duplicate-triangle removal could not resolve), split
    // only those pinched vertices back to the decimation grid (1 um) where the link condition held.
    if (Q < WELD_GRID_DECIMATION) {
        std::unordered_map<uint64_t, int> e_count;
        for (const auto &f : faces)
            for (int e = 0; e < 3; ++e)
                ++e_count[edge_key(f[size_t(e)], f[size_t((e + 1) % 3)])];
        std::unordered_set<int> nm_verts;
        for (const auto &[key, c] : e_count)
            if (c > 2) {
                nm_verts.insert(int(uint32_t(key >> 32)));
                nm_verts.insert(int(uint32_t(key & 0xFFFFFFFFu)));
            }
        if (!nm_verts.empty()) {
            const double       Q_fine = WELD_GRID_DECIMATION;
            QuantizedPointMap  fine_map(Q_fine, nm_verts.size() * 8 + 16);
            for (size_t i = 0; i < n_tri * 3; ++i) {
                if (nm_verts.find(vid[i]) == nm_verts.end())
                    continue;
                const Vec3f &p  = geometry.pos[i];
                const int    id = fine_map.get_or_set(p, int(vert.size()));
                if (fine_map.inserted())
                    vert.emplace_back(double(grid_round(double(p.x()) * Q_fine)) / Q_fine,
                                      double(grid_round(double(p.y()) * Q_fine)) / Q_fine,
                                      double(grid_round(double(p.z()) * Q_fine)) / Q_fine);
                vid[i] = id;
            }
            faces.clear();
            if (track_color)
                colors.clear();
            for (size_t t = 0; t < n_tri; ++t) {
                const int a = vid[t * 3], b = vid[t * 3 + 1], c = vid[t * 3 + 2];
                if (a == b || b == c || a == c)
                    continue;
                const Vec3d u = vert[size_t(b)] - vert[size_t(a)];
                const Vec3d w = vert[size_t(c)] - vert[size_t(a)];
                if (u.cross(w).squaredNorm() < DEGENERATE_AREA_SQ)
                    continue;
                faces.push_back({ a, b, c });
                if (track_color)
                    colors.push_back(t < face_color->size() ? (*face_color)[t] : -1);
            }
            strip_nonmanifold_duplicate_faces();
        }
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
                const int a    = f[size_t(e)];
                const int b    = f[size_t((e + 1) % 3)];
                const int apex = f[size_t((e + 2) % 3)];
                if (e_count[edge_key(a, b)] != 1)
                    continue; // only a boundary edge carries an unresolved T-junction
                const Vec3d  A     = vert[size_t(a)];
                const Vec3d  ev    = vert[size_t(b)] - A;
                const double elen2 = ev.squaredNorm();
                if (elen2 < 1e-20)
                    continue;
                std::vector<std::pair<double, int>> found;
                for (const int c : bv) {
                    if (c == a || c == b || c == apex)
                        continue;
                    // Splitting (a, b, apex) at c adds two triangles sharing the interior edge (c, apex).
                    // If (c, apex) already exists in the mesh, adding two more faces on it would make it
                    // a non-manifold edge (3+ incident faces).
                    if (const auto it_ca = e_count.find(edge_key(c, apex)); it_ca != e_count.end() && it_ca->second > 0)
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
                        if (sp.mids.empty() || sp.mids.back() != m.second)
                            sp.mids.push_back(m.second);
                    // Reject the split if any resulting sub-edge along (a, ..., b) already has 2 incident faces,
                    // if any interior edge (mid_v, apex) is already in use, or if any resulting sub-triangle is degenerate.
                    bool valid_subedges = !sp.mids.empty();
                    int  prev_v         = a;
                    for (const int mid_v : sp.mids) {
                        const auto it_e  = e_count.find(edge_key(prev_v, mid_v));
                        const auto it_ca = e_count.find(edge_key(mid_v, apex));
                        const Vec3d u1   = vert[size_t(mid_v)] - vert[size_t(prev_v)];
                        const Vec3d w1   = vert[size_t(apex)] - vert[size_t(prev_v)];
                        if ((it_e != e_count.end() && it_e->second >= 2) ||
                            (it_ca != e_count.end() && it_ca->second > 0) ||
                            u1.cross(w1).squaredNorm() < DEGENERATE_AREA_SQ) {
                            valid_subedges = false;
                            break;
                        }
                        prev_v = mid_v;
                    }
                    if (valid_subedges) {
                        const auto  it_e = e_count.find(edge_key(prev_v, b));
                        const Vec3d u1   = vert[size_t(b)] - vert[size_t(prev_v)];
                        const Vec3d w1   = vert[size_t(apex)] - vert[size_t(prev_v)];
                        if ((it_e != e_count.end() && it_e->second >= 2) ||
                            u1.cross(w1).squaredNorm() < DEGENERATE_AREA_SQ)
                            valid_subedges = false;
                    }
                    if (valid_subedges) {
                        // Update edge counts immediately so later faces in the same pass cannot collide
                        // on any new sub-edge or interior fan edge.
                        --e_count[edge_key(a, b)];
                        int cur_v = a;
                        for (const int mid_v : sp.mids) {
                            ++e_count[edge_key(cur_v, mid_v)];
                            e_count[edge_key(mid_v, apex)] += 2;
                            cur_v = mid_v;
                        }
                        ++e_count[edge_key(cur_v, b)];
                        splits.emplace(fi, std::move(sp));
                        break; // one site per face per pass; iteration handles cascades
                    }
                }
            }
        }
        if (splits.empty())
            break;

        std::vector<std::array<int, 3>> next;
        std::vector<int>                next_colors;
        next.reserve(faces.size() + splits.size() * 2);
        if (track_color)
            next_colors.reserve(next.capacity());
        for (size_t fi = 0; fi < faces.size(); ++fi) {
            const auto it = splits.find(fi);
            if (it == splits.end()) {
                next.push_back(faces[fi]);
                if (track_color)
                    next_colors.push_back(colors[fi]);
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
            for (size_t s = 0; s + 1 < seq.size(); ++s) {
                next.push_back({ seq[s], seq[s + 1], apex });
                if (track_color)
                    next_colors.push_back(colors[fi]); // every piece of a split face keeps its colour
            }
        }
        faces.swap(next);
        if (track_color)
            colors.swap(next_colors);
    }
    if (track_color)
        *face_color = std::move(colors);

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
