#pragma once
// Path cut connectors: straight pegs / holes / dowel pins placed on the curved cut face left by
// PathCutGeometry, so the printed parts register (no shift, no twist) when glued.
//
// The cut face of a path cut is curved, so a connector cannot simply stand on a plane as in the planar
// cut. Every connector here is straight and shares one axis: the direction the cut face comes apart in
// (pull_direction), so all of them slide out of their holes together, even across a U-shaped face. Where it
// may go is decided by casting lines parallel to that axis through each side (a grid of the triangles
// projected along the axis keeps that linear in the triangle count): a connector is placed only where
// both sides have material all around it, deep enough, with a wall left, and where the cut face is flat
// enough under it for the peg to be anchored. Among the places that fit, they are spread out.
//
// Like the planar cut connectors, the result is volumes, not a mesh boolean: a peg is a model part of
// one side, a hole a negative volume of the other (subtracted when slicing), a dowel a separate pin.
//
// Material is told from the crossings along each line taken in pairs, so the model parts must not
// overlap each other (the cut core makes the same assumption).
//
// Only depends on PathCutGeometry.hpp (Eigen, admesh), so it can be unit-tested on its own.

#include "PathCutGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r { namespace PathCut {

enum class ConnectorType : int { None = 0, Plug = 1, Dowel = 2 };
enum class ConnectorShape : int { Cylinder = 0, Square = 1 };

struct ConnectorParams
{
    ConnectorType  type{ConnectorType::None};
    ConnectorShape shape{ConnectorShape::Cylinder};
    int            count{2};
    double         size{5.};      // diameter, or side of the square [mm]
    double         depth{5.};     // how far the peg (or each half of the pin) goes into a side [mm]
    double         clearance{0.2}; // added to the hole radius and depth [mm]
    double         wall{1.};      // material left around a hole [mm]
    // All connectors are parallel (or the pieces could not come apart): by default along the direction the
    // pieces come apart in; tilted away from it by `tilt`, towards `turn` (0 = up, around that direction) [deg].
    double         tilt{0.};
    double         turn{0.};
};

struct ConnectorResult
{
    std::vector<V3>      positions;      // on the cut face of the `from` side
    V3                   axis{V3::Zero()}; // from `from` into `to`
    indexed_triangle_set pegs;           // Plug: model part of `from`
    indexed_triangle_set holes_to;       // negative volume of `to`
    indexed_triangle_set holes_from;     // Dowel: negative volume of `from`
    indexed_triangle_set pin;            // Dowel: one pin, standing on z = 0 at the origin
};

namespace detail {

// Lines parallel to an axis through a closed mesh: all crossings, via the triangles projected along the
// axis into a uniform grid.
class LineCaster
{
public:
    LineCaster(const indexed_triangle_set &its, const Frame &f) : m_its(its)
    {
        const size_t nv = its.vertices.size(), nt = its.indices.size();
        m_p.resize(nv);
        m_h.resize(nv);
        V2 mn = V2::Constant(std::numeric_limits<double>::max()), mx = -mn;
        for (size_t i = 0; i < nv; ++i) {
            const V3 p = its.vertices[i].cast<double>();
            m_p[i]     = f.to2d(p);
            m_h[i]     = f.height(p);
            mn         = mn.cwiseMin(m_p[i]);
            mx         = mx.cwiseMax(m_p[i]);
        }
        if (nt == 0) return;
        const V2 ext = (mx - mn).cwiseMax(V2::Constant(1e-6));
        m_cell = std::max(std::sqrt(ext.x() * ext.y() / double(std::max<size_t>(nt / 2, 1))), 1e-3);
        while ((ext.x() / m_cell + 1.) * (ext.y() / m_cell + 1.) > 4e6) m_cell *= 1.5;
        m_mn = mn;
        m_nx = int(ext.x() / m_cell) + 1;
        m_ny = int(ext.y() / m_cell) + 1;
        m_start.assign(size_t(m_nx) * size_t(m_ny) + 1, 0);
        auto range = [this](const stl_triangle_vertex_indices &t, int &x0, int &x1, int &y0, int &y1) {
            V2 a = m_p[size_t(t[0])], b = a;
            for (int k = 1; k < 3; ++k) { a = a.cwiseMin(m_p[size_t(t[k])]); b = b.cwiseMax(m_p[size_t(t[k])]); }
            x0 = std::clamp(int((a.x() - m_mn.x()) / m_cell), 0, m_nx - 1);
            x1 = std::clamp(int((b.x() - m_mn.x()) / m_cell), 0, m_nx - 1);
            y0 = std::clamp(int((a.y() - m_mn.y()) / m_cell), 0, m_ny - 1);
            y1 = std::clamp(int((b.y() - m_mn.y()) / m_cell), 0, m_ny - 1);
        };
        for (const auto &t : its.indices) {
            int x0, x1, y0, y1;
            range(t, x0, x1, y0, y1);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) ++m_start[size_t(y) * size_t(m_nx) + size_t(x) + 1];
        }
        for (size_t i = 1; i < m_start.size(); ++i) m_start[i] += m_start[i - 1];
        m_tri.resize(m_start.back());
        std::vector<size_t> fill(m_start.begin(), m_start.end() - 1);
        for (size_t ti = 0; ti < nt; ++ti) {
            int x0, x1, y0, y1;
            range(its.indices[ti], x0, x1, y0, y1);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) m_tri[fill[size_t(y) * size_t(m_nx) + size_t(x)]++] = uint32_t(ti);
        }
    }

    // Heights (along the frame normal) where the line through q crosses the mesh, sorted. Returns false
    // when the count is odd (an open mesh, or an unlucky hit exactly on an edge).
    bool crossings(V2 q, std::vector<double> &out) const
    {
        out.clear();
        if (m_tri.empty()) return true;
        q += V2(1.234567e-7, 7.654321e-8) * m_cell; // off the grid of vertices and edges of regular meshes
        const int x = int((q.x() - m_mn.x()) / m_cell), y = int((q.y() - m_mn.y()) / m_cell);
        if (x < 0 || y < 0 || x >= m_nx || y >= m_ny) return true;
        const size_t c = size_t(y) * size_t(m_nx) + size_t(x);
        for (size_t i = m_start[c]; i < m_start[c + 1]; ++i) {
            const auto  &t = m_its.indices[m_tri[i]];
            const V2    &a = m_p[size_t(t[0])], &b = m_p[size_t(t[1])], &cc = m_p[size_t(t[2])];
            const double d = (b - a).x() * (cc - a).y() - (b - a).y() * (cc - a).x();
            if (std::abs(d) < 1e-14) continue; // edge-on to the line
            const double w1 = ((q - a).x() * (cc - a).y() - (q - a).y() * (cc - a).x()) / d;
            const double w2 = ((b - a).x() * (q - a).y() - (b - a).y() * (q - a).x()) / d;
            if (w1 < 0. || w2 < 0. || w1 + w2 > 1.) continue;
            out.push_back((1. - w1 - w2) * m_h[size_t(t[0])] + w1 * m_h[size_t(t[1])] + w2 * m_h[size_t(t[2])]);
        }
        std::sort(out.begin(), out.end());
        return (out.size() & 1) == 0;
    }

private:
    const indexed_triangle_set &m_its;
    std::vector<V2>             m_p;
    std::vector<double>         m_h;
    V2                          m_mn{V2::Zero()};
    double                      m_cell{1.};
    int                         m_nx{0}, m_ny{0};
    std::vector<size_t>         m_start;
    std::vector<uint32_t>       m_tri;
};

// A closed prism (cylinder with `sides` sides, or a square for 4) around the frame axis through q,
// from height h0 to h1 above the frame origin. `radius` is the circumradius; `turn` turns it about the axis.
inline void append_prism(indexed_triangle_set &its, const Frame &f, const V2 &q, double radius, double h0, double h1, int sides,
                         double turn = 0.)
{
    const int    base = int(its.vertices.size());
    const double rot  = (sides == 4 ? PI_ / 4. : 0.) + turn;
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < sides; ++i) {
            const double a = rot + 2. * PI_ * i / sides;
            its.vertices.emplace_back(f.to3d(q + radius * V2(std::cos(a), std::sin(a)), k == 0 ? h0 : h1).cast<float>());
        }
    its.vertices.emplace_back(f.to3d(q, h0).cast<float>());
    its.vertices.emplace_back(f.to3d(q, h1).cast<float>());
    const int c0 = base + 2 * sides, c1 = c0 + 1;
    for (int i = 0; i < sides; ++i) {
        const int j = (i + 1) % sides;
        const int a0 = base + i, b0 = base + j, a1 = base + sides + i, b1 = base + sides + j;
        its.indices.emplace_back(a0, b0, b1); // sides, counter-clockwise seen from outside (u, v, n right handed)
        its.indices.emplace_back(a0, b1, a1);
        its.indices.emplace_back(c0, b0, a0); // bottom, facing -n
        its.indices.emplace_back(c1, a1, b1); // top, facing +n
    }
}

} // namespace detail

// Triangles of the cut face of one side (cut faces with `level`, also those of earlier cuts), and the
// sum of their area vectors (outward of that side). Returns the total area.
inline double cut_face(const CutResult &cut, int level, std::vector<size_t> *face, V3 &nsum)
{
    const indexed_triangle_set &m = cut.mesh;
    double                      area = 0.;
    nsum                             = V3::Zero();
    std::vector<size_t> tris;
    tris.reserve(cut.cap_triangles + cut.earlier_faces.size());
    for (size_t i = cut.first_cap; i < cut.first_cap + cut.cap_triangles && i < m.indices.size(); ++i) tris.push_back(i);
    for (size_t i : cut.earlier_faces)
        if (i < m.indices.size()) tris.push_back(i);
    for (size_t i : tris) {
        const auto &t     = m.indices[i];
        int         votes = 0;
        for (int k = 0; k < 3; ++k)
            if (size_t(t[k]) < cut.level.size() && cut.level[size_t(t[k])] == level) ++votes;
        if (votes < 2) continue;
        const V3 a = m.vertices[size_t(t[0])].cast<double>(), b = m.vertices[size_t(t[1])].cast<double>(), c = m.vertices[size_t(t[2])].cast<double>();
        const V3 n = 0.5 * (b - a).cross(c - a);
        nsum += n;
        area += n.norm();
        if (face) face->push_back(i);
    }
    return area;
}

// The direction the pieces come apart in, for straight connectors. Each triangle of the face pulls the
// two sides apart along its normal; a connector only slides out of its hole along a direction no part of
// the face turns away from. So the direction chosen leaves the least face turned against it (its area
// times how far it turns), with a slight pull towards the mean normal to settle near ties and to cross
// the face squarely. For a U-shaped face that is along its walls whatever their lengths, where the mean
// normal leans towards the longer wall and the connectors jam; hand-drawn walls that are not quite
// parallel barely move it. Directions far from the mean normal are not taken: a path cut swept along the
// view direction also comes apart along it, but a connector there would lie in the face, not cross it.
// `area_vectors`: one per triangle, its normal times its area. `worst` (optional) gets the cosine between
// the direction and the normals most turned from it (2 % of the area left out): 1 for a flat face, 0 for
// a U, negative when the pieces cannot come apart straight.
inline V3 pull_direction(const std::vector<V3> &area_vectors, double *worst = nullptr)
{
    // Normals grouped by direction (about 3.5 degrees apart), each with its area: keeps the search cheap
    // however fine the face is.
    struct Bin { V3 n{V3::Zero()}; double area{0.}; };
    std::unordered_map<int64_t, Bin> bins;
    V3     mean  = V3::Zero();
    double total = 0.;
    for (const V3 &a : area_vectors) {
        const double area = a.norm();
        if (!(area > 0.)) continue;
        const V3 u = a / area;
        const int64_t key = (int64_t(std::lround(u.x() * 16.)) + 32) * 4096 + (int64_t(std::lround(u.y() * 16.)) + 32) * 64 +
                            (int64_t(std::lround(u.z() * 16.)) + 32);
        Bin &b = bins[key];
        b.n += a;
        b.area += area;
        mean += a;
        total += area;
    }
    if (bins.empty() || !(total > 0.)) {
        if (worst) *worst = -1.;
        return V3::UnitZ();
    }
    std::vector<Bin> bv;
    bv.reserve(bins.size());
    for (auto &kv : bins) {
        kv.second.n.normalize();
        bv.push_back(kv.second);
    }
    mean = mean.norm() > 0. ? V3(mean.normalized()) : bv.front().n;

    // How far the face turns from d: the cosine below which only 2 % of the area lies.
    std::vector<std::pair<double, double>> dots(bv.size());
    auto score = [&](const V3 &d) {
        for (size_t i = 0; i < bv.size(); ++i) dots[i] = {bv[i].n.dot(d), bv[i].area};
        std::sort(dots.begin(), dots.end());
        double acc = 0.;
        for (const auto &[c, w] : dots)
            if ((acc += w) > 0.02 * total) return c;
        return dots.back().first;
    };
    // How much of the face turns against d: area fraction times how far (the sine past square), plus a
    // hundredth of the turn from the mean normal.
    auto cost = [&](const V3 &d) {
        double against = 0.;
        for (const Bin &b : bv) against += b.area * std::max(0., -b.n.dot(d));
        return against / total + 0.01 * (1. - d.dot(mean));
    };
    V3     best      = mean;
    double best_cost = cost(mean);
    auto consider = [&](const V3 &d) {
        if (d.dot(mean) < 0.25) return; // more than 75 degrees off: would run along the face
        const double c = cost(d);
        if (c < best_cost) { best_cost = c; best = d; }
    };
    // Candidates: the normals themselves, then directions spread over the cap around the mean normal,
    // then finer and finer around the best so far.
    auto sweep = [&](const V3 &centre, double max_angle, int count) {
        const Frame g = frame_from_normal(centre, V3::Zero());
        for (int i = 1; i <= count; ++i) {
            const double t = std::acos(1. - (1. - std::cos(max_angle)) * (i - 0.5) / count); // even over the cap
            const double a = i * 2.39996322972865332;                                          // golden angle
            consider((g.n * std::cos(t) + (g.u * std::cos(a) + g.v * std::sin(a)) * std::sin(t)).normalized());
        }
    };
    for (const Bin &b : bv) consider(b.n);
    sweep(mean, 76. * PI_ / 180., 1500);
    sweep(best, 4. * PI_ / 180., 300);
    sweep(best, 0.6 * PI_ / 180., 200);
    if (worst) *worst = score(best);
    return best;
}

// Which side gets the pegs (Plug): the one whose cut face looks up, so the pegs point up (or level) as
// the parts lie after the cut and never reach below the part they stick out of, which would lift it off
// the bed; for a vertical cut face, the larger side. Returns the cut face level of that side (+1 = side_a).
inline int peg_side(const CutResult &cut, const indexed_triangle_set &side_a, const indexed_triangle_set &side_b)
{
    V3 n;
    if (cut_face(cut, 1, nullptr, n) > 0. && n.norm() > 0.) {
        const double z = n.normalized().z();
        if (std::abs(z) > 0.05) return z > 0. ? 1 : -1;
    }
    return std::abs(signed_volume(side_a)) >= std::abs(signed_volume(side_b)) ? 1 : -1;
}

// A place where a connector fits, with the faces measured along its axis: heights relative to p of the
// face of `from` and of `to` on the axis, the highest point of the face of `from` around it, the turn of
// a square about the axis, and how much room is left (larger = roomier).
struct ConnectorSpot
{
    V3     p{V3::Zero()};
    double from_face{0.}, to_face{0.}, from_top{0.};
    double angle{0.};
    double slack{0.};
    bool   forced{false}; // placed where the checks said it does not fit (the user asked for it)
};

// Why a connector does not fit at a given place.
enum class ConnectorFit : int {
    Ok = 0,
    Edge,      // too close to the edge of the cut face: a side has no material all around it
    Slope,     // the cut face is too steep or uneven under it
    ThinFrom,  // the part the peg stands in (or the pin's first hole) is too thin there
    ThinTo,    // the part with the hole is too thin there for the depth
    Hole,      // it would run into a negative volume already there (e.g. a hole of an earlier cut)
    Overlap,   // too close to another connector
    OffFace,   // not on the cut face
};

inline const char *connector_fit_message(ConnectorFit why)
{
    switch (why) {
    case ConnectorFit::Edge: return L("The connector does not fit there: too close to the edge of the cut face.");
    case ConnectorFit::Slope: return L("The connector does not fit there: the cut face is too steep or uneven under it.");
    case ConnectorFit::ThinFrom:
    case ConnectorFit::ThinTo: return L("The connector does not fit there: the part is too thin for its depth. Make it shorter or try another spot.");
    case ConnectorFit::Hole: return L("The connector does not fit there: it would run into a hole that is already there.");
    case ConnectorFit::Overlap: return L("The connector does not fit there: too close to another connector.");
    case ConnectorFit::OffFace: return L("Click on the cut face to place a connector.");
    default: return "";
    }
}

// Connectors tilted by hand: `pull` (the direction the pieces come apart in) tilted away from itself by
// `tilt` degrees (at most 80), towards `turn` degrees around it, measured from up (from +X when `pull` is
// vertical). tilt_of() gives the tilt and turn of an axis.
inline void tilt_frame(const V3 &pull, V3 &up, V3 &side)
{
    up = V3::UnitZ() - pull * pull.z();
    if (up.norm() < 1e-6) up = V3::UnitX() - pull * pull.x();
    up.normalize();
    side = pull.cross(up);
}
inline V3 tilted_axis(const V3 &pull, double tilt, double turn)
{
    V3 up, side;
    tilt_frame(pull, up, side);
    const double t = std::clamp(tilt, 0., 80.) * PI_ / 180., r = turn * PI_ / 180.;
    return (pull * std::cos(t) + (up * std::cos(r) + side * std::sin(r)) * std::sin(t)).normalized();
}
inline void tilt_of(const V3 &pull, const V3 &axis, double &tilt, double &turn)
{
    V3 up, side;
    tilt_frame(pull, up, side);
    const V3 a = axis.normalized();
    tilt = std::acos(std::clamp(a.dot(pull), -1., 1.)) * 180. / PI_;
    turn = tilt > 1e-6 ? std::atan2(a.dot(side), a.dot(up)) * 180. / PI_ : 0.;
}

// Places straight connectors across the cut face between `from` and `to`, the two sides of `cut` (cut
// faces with level `from_level` belong to `from`): automatically, or one by one where the user asks.
// Pegs (Plug) stick out of `from` into holes of `to`. `avoid` (optional, closed) is material that is
// already removed (negative volumes): no connector runs into it.
// Keeps references to `from`, `to` and `avoid`: they must outlive the placer.
class ConnectorPlacer
{
public:
    ConnectorPlacer(const CutResult &cut, int from_level, const indexed_triangle_set &from, const indexed_triangle_set &to,
                    const ConnectorParams &prm, const indexed_triangle_set *avoid = nullptr)
        : m_prm(prm)
    {
        if (!(prm.size > 0.) || !(prm.depth > 0.)) {
            m_error = L("No connectors fit: the cut face is too small or too thin for them. Make them smaller or shorter.");
            return;
        }
        const indexed_triangle_set &m = cut.mesh;
        // Cut face of `from`: its triangles, and the direction it comes apart from `to` in (pointing into
        // `to`), which every connector follows so that all of them slide out together.
        V3           nsum;
        const double area = cut_face(cut, from_level, &m_face_idx, nsum);
        std::vector<V3> area_vectors;
        area_vectors.reserve(m_face_idx.size());
        for (size_t i : m_face_idx) {
            const auto &t = m.indices[i];
            const V3    a = m.vertices[size_t(t[0])].cast<double>(), b = m.vertices[size_t(t[1])].cast<double>(),
                        c = m.vertices[size_t(t[2])].cast<double>();
            area_vectors.push_back(0.5 * (b - a).cross(c - a));
        }
        double worst = -1.;
        if (!m_face_idx.empty() && area > 1e-9) m_axis = pull_direction(area_vectors, &worst);
        // Too curved: the face turns well against every straight direction (a U, whose walls are parallel
        // to the direction, is fine), and its mean normal says the same.
        if (m_face_idx.empty() || !(area > 1e-9) || (worst < -0.1 && nsum.norm() <= 0.2 * area)) {
            m_error = L("No connectors fit: the cut face is too curved for a straight connector.");
            return;
        }
        m_pull = m_axis;
        if (prm.tilt > 1e-6) m_axis = tilted_axis(m_pull, prm.tilt, prm.turn);
        m_frame = frame_from_normal(m_axis, V3::Zero());
        // A copy of the face, for display and picking.
        std::unordered_map<int, int> vmap;
        for (size_t i : m_face_idx) {
            stl_triangle_vertex_indices t;
            for (int k = 0; k < 3; ++k) {
                const int v       = m.indices[i][k];
                auto [it, ins]    = vmap.emplace(v, int(m_face.vertices.size()));
                if (ins) m_face.vertices.push_back(m.vertices[size_t(v)]);
                t[k] = it->second;
            }
            m_face.indices.push_back(t);
        }
        m_square = prm.shape == ConnectorShape::Square;
        m_r      = 0.5 * prm.size;
        m_tol    = std::max(prm.clearance, 0.);
        m_wall   = std::max(prm.wall, 0.);
        m_embed  = std::clamp(0.4 * prm.depth, 1., 3.);
        m_dowel  = prm.type == ConnectorType::Dowel;
        // Outer extent that must be solid: the hole plus the wall, as a radius (circle) or half side (square).
        m_outer   = m_r + m_tol + m_wall;
        m_min_gap = 2. * m_outer * (m_square ? std::sqrt(2.) : 1.) + 1.;
        m_from    = std::make_unique<detail::LineCaster>(from, m_frame);
        m_to      = std::make_unique<detail::LineCaster>(to, m_frame);
        if (avoid && !avoid->indices.empty()) m_avoid = std::make_unique<detail::LineCaster>(*avoid, m_frame);
    }

    // Non-empty when no connector can go anywhere (cut face too curved, bad sizes).
    const std::string          &error() const { return m_error; }
    const V3                   &axis() const { return m_axis; }
    // The direction the pieces come apart in: the axis before any tilt asked for.
    const V3                   &pull_axis() const { return m_pull; }
    const indexed_triangle_set &face() const { return m_face; }
    double                      min_gap() const { return m_min_gap; }

    // Tries a connector at p, a point on the cut face (as picked on face()). `placed` are connectors
    // already there, kept at a distance. A square is turned about the axis to the orientation with the most room.
    ConnectorFit try_at(const V3 &p, ConnectorSpot &spot, const std::vector<ConnectorSpot> &placed = {}) const
    {
        if (!m_error.empty()) return ConnectorFit::OffFace;
        const V2 q = m_frame.to2d(p);
        for (const ConnectorSpot &o : placed)
            if ((m_frame.to2d(o.p) - q).norm() < m_min_gap) return ConnectorFit::Overlap;
        ConnectorFit first = ConnectorFit::Ok;
        bool         found = false;
        const int    turns = m_square ? 6 : 1; // 0, 15, ..., 75 degrees: a square repeats every 90
        for (int k = 0; k < turns; ++k) {
            ConnectorSpot s;
            const ConnectorFit why = fits(p, PI_ / 12. * k, s);
            if (why == ConnectorFit::Ok) {
                if (!found || s.slack > spot.slack) spot = s;
                found = true;
            } else if (k == 0)
                first = why;
        }
        return found ? ConnectorFit::Ok : first;
    }

    // A connector at p placed whatever the checks say (the user asked for it): its faces measured on the
    // axis where they are found, else at p.
    ConnectorSpot forced_at(const V3 &p) const
    {
        ConnectorSpot s;
        s.p = p;
        s.forced = true;
        if (!m_error.empty()) return s;
        const V2     q = m_frame.to2d(p);
        const double h = m_frame.height(p);
        double       a0 = 0., a1 = 0., b0 = 0., b1 = 0.;
        if (interval(*m_from, q, h, true, a0, a1)) s.from_face = s.from_top = a1;
        if (interval(*m_to, q, h, false, b0, b1)) s.to_face = b0;
        return s;
    }

    // Up to `count` connectors spread over the cut face: the roomiest spot first, then each next one as
    // far as possible from those already placed. `error` is set when not a single one fits.
    std::vector<ConnectorSpot> auto_place(int count, std::string &error, const std::function<bool()> &canceled = {}) const
    {
        std::vector<ConnectorSpot> placed;
        error = m_error;
        if (!error.empty() || count <= 0) return placed;
        const std::vector<V3> cand = candidates();
        std::vector<ConnectorSpot> ok;
        for (size_t i = 0; i < cand.size(); ++i) {
            if ((i & 63) == 0 && canceled && canceled()) return {};
            ConnectorSpot s;
            if (try_at(cand[i], s) == ConnectorFit::Ok) ok.push_back(s);
        }
        if (ok.empty()) {
            error = L("No connectors fit: the cut face is too small or too thin for them. Make them smaller or shorter.");
            return placed;
        }
        std::vector<double> dist(ok.size(), std::numeric_limits<double>::max());
        size_t pick = 0;
        for (size_t i = 1; i < ok.size(); ++i)
            if (ok[i].slack > ok[pick].slack) pick = i;
        while (int(placed.size()) < count) {
            placed.push_back(ok[pick]);
            const V2 last = m_frame.to2d(ok[pick].p);
            double   best = -1.;
            for (size_t i = 0; i < ok.size(); ++i) {
                dist[i] = std::min(dist[i], (m_frame.to2d(ok[i].p) - last).norm());
                if (dist[i] >= m_min_gap && dist[i] > best) { best = dist[i]; pick = i; }
            }
            if (best < 0.) break;
        }
        return placed;
    }

    // The connector volumes for the given spots.
    ConnectorResult build(const std::vector<ConnectorSpot> &spots) const
    {
        ConnectorResult res;
        res.axis = m_axis;
        if (!m_error.empty()) return res;
        const int    sides = m_square ? 4 : 32;
        const double rc    = m_square ? m_r * std::sqrt(2.) : m_r;                         // circumradius
        const double rh    = m_square ? (m_r + m_tol) * std::sqrt(2.) : m_r + m_tol;       // hole circumradius
        const double depth = m_prm.depth;
        // Depths count from each side's own face (the faces are a kerf apart): a pin of 2 x depth fits two
        // holes of depth + clearance, a peg reaching depth into `to` a hole of depth + clearance.
        for (const ConnectorSpot &s : spots) {
            res.positions.push_back(s.p);
            const V2     q = m_frame.to2d(s.p);
            const double h = m_frame.height(s.p);
            // Holes start below / end above the face they open on everywhere around (negative volumes only
            // remove material of their own part, so overshooting into the gap is harmless).
            detail::append_prism(res.holes_to, m_frame, q, rh, h - m_embed - 1., h + s.to_face + depth + m_tol, sides, s.angle);
            if (m_dowel)
                detail::append_prism(res.holes_from, m_frame, q, rh, h + s.from_face - depth - m_tol, h + s.from_top + 1., sides, s.angle);
            else
                detail::append_prism(res.pegs, m_frame, q, rc, h - m_embed, h + s.to_face + depth, sides, s.angle);
        }
        if (m_dowel && !spots.empty()) {
            Frame up;
            detail::append_prism(res.pin, up, V2::Zero(), rc, 0., 2. * depth, sides);
        }
        return res;
    }

private:
    // Points on the cut face, one per grid cell across the axis. Large cut face triangles (a flat cap is a
    // few big ones) are sampled inside, not only at their centroid, so a spot that fits is not missed for
    // lack of a candidate near it.
    std::vector<V3> candidates() const
    {
        double          cell = std::max(0.25 * m_r, 0.25);
        std::vector<V3> cand;
        for (int attempt = 0; attempt < 8; ++attempt, cell *= 1.6) {
            cand.clear();
            std::unordered_map<uint64_t, size_t> seen;
            std::vector<double> off; // distance of each candidate to its cell centre: keep the closest
            auto add = [&](const V3 &c) {
                const V2       q   = m_frame.to2d(c);
                const double   gx = std::floor(q.x() / cell), gy = std::floor(q.y() / cell);
                const uint64_t key = (uint64_t(uint32_t(int32_t(gx))) << 32) | uint32_t(int32_t(gy));
                const double   d   = (q - V2((gx + 0.5) * cell, (gy + 0.5) * cell)).norm();
                auto [it, ins]     = seen.emplace(key, cand.size());
                if (ins) { cand.push_back(c); off.push_back(d); }
                else if (d < off[it->second]) { cand[it->second] = c; off[it->second] = d; }
            };
            for (const auto &t : m_face.indices) {
                const V3 a = m_face.vertices[size_t(t[0])].cast<double>(), b = m_face.vertices[size_t(t[1])].cast<double>(),
                         c = m_face.vertices[size_t(t[2])].cast<double>();
                add((a + b + c) / 3.);
                const double longest = std::max({(b - a).norm(), (c - b).norm(), (a - c).norm()});
                const int    k       = std::min(64, int(longest / cell));
                for (int u = 1; u < k; ++u)
                    for (int v = 1; u + v < k; ++v)
                        add(a + (b - a) * (double(u) / k) + (c - a) * (double(v) / k));
            }
            if (cand.size() <= 6000) break;
        }
        return cand;
    }

    // Solid interval of a side along the line through q, as heights relative to the face point at h: the
    // one whose top (bottom for `to`) is nearest the face.
    bool interval(const detail::LineCaster &lc, const V2 &q, double h, bool top, double &lo, double &hi) const
    {
        if (!lc.crossings(q, m_hits) || m_hits.empty()) return false;
        double best = std::numeric_limits<double>::max();
        for (size_t i = 0; i + 1 < m_hits.size(); i += 2) {
            const double a = m_hits[i] - h, b = m_hits[i + 1] - h, d = std::abs(top ? b : a);
            if (d < best) { best = d; lo = a; hi = b; }
        }
        return true;
    }

    // Offsets across the axis where both sides are probed: the centre, then the outline of the hole plus
    // its wall (a circle, or the turned square) at half and full size.
    std::vector<V2> probes(double angle) const
    {
        std::vector<V2> out{V2::Zero()};
        constexpr int   n = 24;
        for (double scale : {0.5, 1.}) {
            const double e = scale * m_outer;
            for (int i = 0; i < n; ++i) {
                V2 o;
                if (!m_square) {
                    const double a = 2. * PI_ * i / n;
                    o              = e * V2(std::cos(a), std::sin(a));
                } else { // along the 4 edges of a square of half side e, 6 points each, from a corner
                    const int    edge = i / 6;
                    const double t    = (i % 6) / 6.;
                    const V2     c0   = e * V2(edge == 0 || edge == 3 ? 1. : -1., edge < 2 ? 1. : -1.);
                    const V2     c1   = e * V2(edge == 2 || edge == 3 ? 1. : -1., edge == 0 || edge == 3 ? 1. : -1.);
                    o                 = c0 + (c1 - c0) * t;
                }
                const double ca = std::cos(angle), sa = std::sin(angle);
                out.emplace_back(ca * o.x() - sa * o.y(), sa * o.x() + ca * o.y());
            }
        }
        return out;
    }

    // All probes are checked, and the most telling reason is reported: the edge of the face first (moving
    // the connector inwards is what helps), then a hole in the way, the slope, and the thickness.
    ConnectorFit fits(const V3 &p, double angle, ConnectorSpot &spot) const
    {
        const V2     q     = m_frame.to2d(p);
        const double h     = m_frame.height(p);
        const double depth = m_prm.depth;
        spot               = {p, 0., 0., -std::numeric_limits<double>::max(), angle, std::numeric_limits<double>::max()};
        ConnectorFit worst = ConnectorFit::Ok;
        auto rank = [](ConnectorFit w) {
            switch (w) {
            case ConnectorFit::Edge: return 4;
            case ConnectorFit::Hole: return 3;
            case ConnectorFit::Slope: return 2;
            case ConnectorFit::ThinFrom:
            case ConnectorFit::ThinTo: return 1;
            default: return 0;
            }
        };
        auto fail = [&](ConnectorFit w) { if (rank(w) > rank(worst)) worst = w; };
        const std::vector<V2> pr = probes(angle);
        for (size_t i = 0; i < pr.size(); ++i) {
            const V2 qq = q + pr[i];
            double   a0 = 0., a1 = 0., b0 = 0., b1 = 0.;
            if (!interval(*m_from, qq, h, true, a0, a1) || !interval(*m_to, qq, h, false, b0, b1)) return ConnectorFit::Edge;
            if (i == 0) { spot.from_face = a1; spot.to_face = b0; }
            spot.from_top = std::max(spot.from_top, a1);
            // `from`: the face is near p (not sloping past the sunk base) and solid down to the peg base,
            // or past the bottom of the hole of a pin.
            if (a1 < -m_embed + 0.3 || a1 > 0.5 * depth) fail(ConnectorFit::Slope);
            const double need_from = m_dowel ? spot.from_face - depth - m_tol - m_wall : -m_embed - 0.2;
            if (a0 > need_from) fail(ConnectorFit::ThinFrom);
            // `to`: its face is above the peg base and the peg goes in at least half way; solid past the
            // bottom of the hole, which is `depth` (+ clearance) into `to`, from its face.
            if (b0 < -m_embed || b0 > 0.5 * depth) fail(ConnectorFit::Slope);
            const double need_to = spot.to_face + depth + m_tol + m_wall;
            if (b1 < need_to) fail(ConnectorFit::ThinTo);
            // Clear of material removed already, over the whole height the connector and its holes take.
            if (m_avoid) {
                const double lo = (m_dowel ? need_from : -m_embed - 1.) - 0.1, hi = need_to + 0.1;
                if (!m_avoid->crossings(qq, m_hits))
                    fail(ConnectorFit::Hole); // odd count: be safe
                else
                    for (size_t k = 0; k + 1 < m_hits.size(); k += 2)
                        if (m_hits[k] - h < hi && m_hits[k + 1] - h > lo) { fail(ConnectorFit::Hole); break; }
            }
            spot.slack = std::min({spot.slack, need_from - a0, b1 - need_to, a1 + m_embed - 0.3});
        }
        return worst;
    }

    ConnectorParams                     m_prm;
    std::string                         m_error;
    V3                                  m_axis{V3::Zero()};
    V3                                  m_pull{V3::Zero()};
    Frame                               m_frame;
    std::vector<size_t>                 m_face_idx;
    indexed_triangle_set                m_face;
    bool                                m_square{false}, m_dowel{false};
    double                              m_r{0.}, m_tol{0.}, m_wall{0.}, m_embed{0.}, m_outer{0.}, m_min_gap{0.};
    std::unique_ptr<detail::LineCaster> m_from, m_to, m_avoid;
    mutable std::vector<double>         m_hits;
};

// Places up to prm.count connectors automatically (see ConnectorPlacer). `error` is set when not a
// single connector fits; fewer than asked is not an error.
inline ConnectorResult place_connectors(const CutResult &cut, int from_level, const indexed_triangle_set &from, const indexed_triangle_set &to,
                                        const ConnectorParams &prm, std::string &error, const std::function<bool()> &canceled = {},
                                        const indexed_triangle_set *avoid = nullptr)
{
    error.clear();
    if (prm.type == ConnectorType::None || prm.count <= 0) return {};
    const ConnectorPlacer placer(cut, from_level, from, to, prm, avoid);
    const std::vector<ConnectorSpot> spots = placer.auto_place(prm.count, error, canceled);
    if (spots.empty()) {
        ConnectorResult r;
        r.axis = placer.axis();
        return r;
    }
    return placer.build(spots);
}

}} // namespace Slic3r::PathCut
