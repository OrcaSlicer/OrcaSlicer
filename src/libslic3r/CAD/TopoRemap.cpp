#include "TopoRemap.hpp"
#include "GeometryEngine.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>
#include <cstddef>
#include <memory>
#include <utility>
#include "libslic3r/Point.hpp"
#include <TopAbs_ShapeEnum.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <GeomAbs_CurveType.hxx>

namespace Slic3r { namespace TopoRemap {

namespace {

struct Box { Vec3d lo, hi; };

Box box_of(const TopoDS_Shape& s)
{
    Bnd_Box b; BRepBndLib::Add(s, b);
    double x0, y0, z0, x1, y1, z1; b.Get(x0, y0, z0, x1, y1, z1);
    return {{x0, y0, z0}, {x1, y1, z1}};
}

// A point as fractions of the box (0 = low face, 1 = high face). A flat axis reads 0.5.
Vec3d rel(const Box& b, const Vec3d& p)
{
    Vec3d r;
    for (int i = 0; i < 3; ++i) { const double e = b.hi[i] - b.lo[i]; r[i] = e > 1e-9 ? (p[i] - b.lo[i]) / e : 0.5; }
    return r;
}

struct Desc {
    int    kind{-1};       // GeomAbs surface/curve type
    Vec3d  dir{Vec3d::Zero()};   // face: outward normal (plane) or axis (others, sign-free); edge: direction, sign-free
    bool   signed_dir{false};
    Vec3d  pos{Vec3d::Zero()};   // position in box fractions
    std::vector<int> around;     // sorted kinds of what it touches
};

Vec3d unsigned_dir(Vec3d d)
{
    if (d.norm() < 1e-12) return d;
    d.normalize();
    for (int i = 0; i < 3; ++i) if (std::abs(d[i]) > 1e-9) { if (d[i] < 0) d = -d; break; }
    return d;
}

struct Topo {
    std::vector<TopoDS_Face> faces;
    std::vector<TopoDS_Edge> edges;
    Box box;
    TopTools_IndexedDataMapOfShapeListOfShape edge_faces;   // edge -> faces
    std::vector<Desc> fd, ed;
};

int face_index(const Topo& t, const TopoDS_Shape& f)
{
    for (size_t i = 0; i < t.faces.size(); ++i) if (t.faces[i].IsSame(f)) return int(i);
    return -1;
}

Topo build(const TopoDS_Shape& s)
{
    Topo t;
    t.faces = GeometryEngine::faces_of(s);
    t.edges = GeometryEngine::edges_of(s);
    t.box = box_of(s);
    TopExp::MapShapesAndAncestors(s, TopAbs_EDGE, TopAbs_FACE, t.edge_faces);
    t.fd.resize(t.faces.size());
    t.ed.resize(t.edges.size());
    for (size_t i = 0; i < t.faces.size(); ++i) {
        const TopoDS_Face& f = t.faces[i];
        Desc& d = t.fd[i];
        if (f.IsNull()) continue;
        d.kind = int(BRepAdaptor_Surface(f).GetType());
        d.pos = rel(t.box, GeometryEngine::face_centroid_world(f));
        if (d.kind == int(GeomAbs_Plane)) { d.dir = GeometryEngine::face_normal_world(f); d.signed_dir = true; }
        else {
            GeometryEngine::CylinderFace c = GeometryEngine::cylinder_of_face(f);
            d.dir = c.ok ? unsigned_dir(c.axis) : Vec3d::Zero();
        }
    }
    for (size_t i = 0; i < t.edges.size(); ++i) {
        const TopoDS_Edge& e = t.edges[i];
        Desc& d = t.ed[i];
        if (e.IsNull()) continue;
        d.kind = int(BRepAdaptor_Curve(e).GetType());
        const std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(e);
        if (pts.size() < 2) continue;
        double len = 0; for (size_t k = 1; k < pts.size(); ++k) len += (pts[k] - pts[k - 1]).norm();
        double acc = 0; Vec3d mid = pts.front();
        for (size_t k = 1; k < pts.size() && acc < len * 0.5; ++k) { acc += (pts[k] - pts[k - 1]).norm(); mid = pts[k]; }
        d.pos = rel(t.box, mid);
        if (d.kind == int(GeomAbs_Line)) d.dir = unsigned_dir(pts.back() - pts.front());
        else { GeometryEngine::CylinderFace c = GeometryEngine::circle_of_edge(e); if (c.ok) d.dir = unsigned_dir(c.axis); }
        const int idx = t.edge_faces.FindIndex(e);
        if (idx > 0) for (const TopoDS_Shape& fs : t.edge_faces.FindFromIndex(idx)) {
            const int fi = face_index(t, fs);
            if (fi >= 0 && t.fd[fi].kind >= 0) d.around.push_back(t.fd[fi].kind);
        }
        std::sort(d.around.begin(), d.around.end());
    }
    // a face's neighbourhood: the kinds of the faces across its edges
    for (size_t i = 0; i < t.faces.size(); ++i) {
        for (const TopoDS_Edge& e : GeometryEngine::edges_of_face(t.faces[i])) {
            const int idx = t.edge_faces.FindIndex(e);
            if (idx <= 0) continue;
            for (const TopoDS_Shape& fs : t.edge_faces.FindFromIndex(idx))
                if (!fs.IsSame(t.faces[i])) { const int fi = face_index(t, fs); if (fi >= 0) t.fd[i].around.push_back(t.fd[fi].kind); }
        }
        std::sort(t.fd[i].around.begin(), t.fd[i].around.end());
    }
    return t;
}

// build() walks every face and edge of a body; a rebuild asks about several ids of the same two bodies (every id of every
// feature), so the descriptors are kept for the last few shapes. A shape is immutable, and the cache holds it, so IsSame is
// a safe key. One cache per thread: the preview rebuilds on a worker thread.
std::shared_ptr<const Topo> build_cached(const TopoDS_Shape& s)
{
    thread_local std::vector<std::pair<TopoDS_Shape, std::shared_ptr<const Topo>>> cache;
    for (auto it = cache.begin(); it != cache.end(); ++it)
        if (it->first.IsSame(s)) {
            if (it != cache.begin()) std::rotate(cache.begin(), it, it + 1);   // most recent first
            return cache.front().second;
        }
    cache.insert(cache.begin(), {s, std::make_shared<const Topo>(build(s))});
    if (cache.size() > 8) cache.pop_back();
    return cache.front().second;
}

// Lower is closer; infinity = cannot be the same kind of thing.
double distance(const Desc& a, const Desc& b)
{
    if (a.kind != b.kind || a.kind < 0) return std::numeric_limits<double>::infinity();
    double s = (a.pos - b.pos).norm();
    if (a.dir.norm() > 1e-9 && b.dir.norm() > 1e-9) s += 2.0 * (a.signed_dir ? 1.0 - a.dir.dot(b.dir) : 1.0 - std::abs(a.dir.dot(b.dir)));
    if (a.around != b.around) s += 0.5;
    return s;
}

int remap(const std::vector<Desc>& before, const std::vector<Desc>& now, int old_id)
{
    if (old_id < 0 || old_id >= int(before.size())) return -1;
    const Desc& o = before[old_id];
    if (old_id < int(now.size()) && distance(o, now[old_id]) < 1e-6) return old_id;   // still the same thing
    double best = std::numeric_limits<double>::infinity(), second = best;
    int arg = -1;
    for (size_t i = 0; i < now.size(); ++i) {
        const double d = distance(o, now[i]);
        if (d < best) { second = best; best = d; arg = int(i); }
        else if (d < second) second = d;
    }
    if (arg < 0 || best > 0.6) return -1;           // nothing close
    if (second - best < 0.02) return -1;            // two as good as each other: do not guess
    return arg;
}

} // namespace

int remap_face(const TopoDS_Shape& before, const TopoDS_Shape& now, int old_id)
{
    if (before.IsNull() || now.IsNull()) return -1;
    if (before.IsSame(now)) return old_id;
    return remap(build_cached(before)->fd, build_cached(now)->fd, old_id);
}

int remap_edge(const TopoDS_Shape& before, const TopoDS_Shape& now, int old_id)
{
    if (before.IsNull() || now.IsNull()) return -1;
    if (before.IsSame(now)) return old_id;
    return remap(build_cached(before)->ed, build_cached(now)->ed, old_id);
}

}} // namespace Slic3r::TopoRemap
