#include "SheetMetal.hpp"

#include "libslic3r/I18N.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <GProp_GProps.hxx>
#include <gp_Ax1.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Trsf.hxx>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <NCollection_IndexedDataMap.hxx>
#include <NCollection_List.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <BRep_Tool.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>
#include <memory>
#include <cstddef>
#include <math.h>
#include <TopAbs_ShapeEnum.hxx>
#include <TopAbs_State.hxx>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {
namespace SheetMetal {

namespace {

gp_Pnt P(const Vec3d& v) { return gp_Pnt(v.x(), v.y(), v.z()); }

// A planar outline made of straight lines and three-point arcs, closed into a face.
struct Outline {
    std::vector<TopoDS_Edge> edges;
    void line(const Vec3d& a, const Vec3d& b)
    {
        if ((a - b).norm() < 1e-9) return;
        edges.push_back(BRepBuilderAPI_MakeEdge(P(a), P(b)).Edge());
    }
    void arc(const Vec3d& a, const Vec3d& mid, const Vec3d& b)
    {
        GC_MakeArcOfCircle mk(P(a), P(mid), P(b));
        if (!mk.IsDone()) throw std::runtime_error(_u8L("sheet metal: a bend could not be built"));
        edges.push_back(BRepBuilderAPI_MakeEdge(mk.Value()).Edge());
    }
    TopoDS_Face face() const
    {
        BRepBuilderAPI_MakeWire mw;
        for (const TopoDS_Edge& e : edges) mw.Add(e);
        if (!mw.IsDone()) throw std::runtime_error(_u8L("sheet metal: the outline is not closed"));
        BRepBuilderAPI_MakeFace mf(mw.Wire(), true);
        if (!mf.IsDone()) throw std::runtime_error(_u8L("sheet metal: the outline is not planar"));
        return mf.Face();
    }
};

TopoDS_Shape prism(const Outline& o, const Vec3d& v)
{
    return BRepPrimAPI_MakePrism(o.face(), gp_Vec(v.x(), v.y(), v.z())).Shape();
}

TopoDS_Shape fuse(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    BRepAlgoAPI_Fuse f(a, b);
    if (!f.IsDone() || f.Shape().IsNull()) throw std::runtime_error(_u8L("sheet metal: joining a flange failed"));
    return f.Shape();
}

TopoDS_Shape cut(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    BRepAlgoAPI_Cut c(a, b);
    if (!c.IsDone() || c.Shape().IsNull()) throw std::runtime_error(_u8L("sheet metal: trimming the plate failed"));
    return c.Shape();
}

TopoDS_Shape unify(const TopoDS_Shape& s)
{
    try {
        ShapeUpgrade_UnifySameDomain u(s, true, true, true);
        u.Build();
        return u.Shape().IsNull() ? s : u.Shape();
    } catch (...) {
        return s;
    }
}

Vec3d vec(const gp_Pnt& p) { return Vec3d(p.X(), p.Y(), p.Z()); }

// The straight edge of a plate: its ends, direction, the plate-face normal ("up") and the direction into the plate ("in").
struct PlateEdge {
    Vec3d p0, p1, e, up, in;
    double len{0};
};

using EdgeFaces = NCollection_IndexedDataMap<TopoDS_Shape, NCollection_List<TopoDS_Shape>, TopTools_ShapeMapHasher>;

Vec3d outward_normal(const TopoDS_Face& f)
{
    BRepAdaptor_Surface s(f);
    if (s.GetType() != GeomAbs_Plane) throw std::runtime_error(_u8L("sheet metal: the edge must lie on flat sheet faces"));
    const gp_Dir d = s.Plane().Axis().Direction();
    Vec3d n(d.X(), d.Y(), d.Z());
    return f.Orientation() == TopAbs_REVERSED ? Vec3d(-n) : n;
}

PlateEdge analyse(const TopoDS_Shape& body, const EdgeFaces& ef, const TopoDS_Edge& edge, double t)
{
    if (!ef.Contains(edge)) throw std::runtime_error(_u8L("sheet metal: the edge is not part of the body"));
    BRepAdaptor_Curve c(edge);
    if (c.GetType() != GeomAbs_Line) throw std::runtime_error(_u8L("sheet metal: only straight edges can take a flange"));
    PlateEdge pe;
    pe.p0 = vec(c.Value(c.FirstParameter()));
    pe.p1 = vec(c.Value(c.LastParameter()));
    pe.len = (pe.p1 - pe.p0).norm();
    if (pe.len < 1e-6) throw std::runtime_error(_u8L("sheet metal: the edge is too short"));
    pe.e = (pe.p1 - pe.p0) / pe.len;

    std::vector<TopoDS_Face> faces;
    for (const TopoDS_Shape& s : ef.FindFromKey(edge)) faces.push_back(TopoDS::Face(s));
    if (faces.size() != 2) throw std::runtime_error(_u8L("sheet metal: the edge must be shared by two faces"));
    // Which of the two faces is the plate face and which the end of the sheet? The sheet is t thick under the plate face and
    // empty just below it, and there is material a little way into the plate. (A face's own extent cannot tell: after an
    // earlier flange the end face is one merged face that is much wider than t.)
    const Vec3d mid = 0.5 * (pe.p0 + pe.p1);
    auto solid_at = [&](const Vec3d& q) {
        for (TopExp_Explorer ex(body, TopAbs_SOLID); ex.More(); ex.Next()) {
            BRepClass3d_SolidClassifier cl(ex.Current(), P(q), 1e-7);
            if (cl.State() == TopAbs_IN) return true;
        }
        return false;
    };
    Vec3d n[2];
    for (int i = 0; i < 2; ++i) n[i] = outward_normal(faces[size_t(i)]);
    int end_i = -1;
    for (int i = 0; i < 2 && end_i < 0; ++i) {   // hypothesis: face i is the end face, the other is the plate face
        const Vec3d &pl = n[1 - i], &en = n[i];
        const Vec3d in = -en;
        if (solid_at(mid - pl * (0.5 * t) + in * (0.1 * t)) && !solid_at(mid - pl * (1.05 * t) + in * (0.1 * t)))
            end_i = i;
    }
    if (end_i < 0) throw std::runtime_error(_u8L("sheet metal: the edge is not at the end of a sheet of the rule's thickness"));
    pe.up = n[1 - end_i];
    pe.in = -n[end_i];
    if (std::abs(pe.in.dot(pe.e)) > 1e-6 || std::abs(pe.in.dot(pe.up)) > 1e-6)
        throw std::runtime_error(_u8L("sheet metal: the edge is not a square end of the sheet"));
    return pe;
}

struct Flange {
    PlateEdge pe;
    Vec3d O, W;               // origin of the flange frame (outer sharp) and its "up" axis
    double a0{0}, a1{0};      // extent of the bend along the edge
    double w0{0}, w1{0};      // extent of the flat leg along the edge (longer when the corner is closed)
};

} // namespace

TopoDS_Shape edge_flanges(const TopoDS_Shape& body, const std::vector<TopoDS_Edge>& edges, const Rule& rule,
                          double height, double angle_deg, bool flip)
{
    if (edges.empty()) throw std::runtime_error(_u8L("sheet metal: pick at least one edge"));
    if (angle_deg <= 0 || angle_deg > 170) throw std::runtime_error(_u8L("sheet metal: the flange angle must be between 0 and 170 degrees"));
    const double t = rule.t, R = rule.radius(), Ro = rule.outer();
    const double th = angle_deg * M_PI / 180.0;
    const double s = Ro * std::tan(th / 2);   // setback of the flat plate from the edge
    const double leg = height - s;
    if (leg <= 1e-6) throw std::runtime_error(_u8L("sheet metal: the flange height must be more than the bend setback"));

    EdgeFaces ef;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, ef);
    std::vector<Flange> fl;
    for (const TopoDS_Edge& e : edges) {
        Flange f;
        f.pe = analyse(body, ef, e, t);
        // The flange rises on the plate-face side; flipped, it drops on the other one.
        f.W = flip ? Vec3d(-f.pe.up) : f.pe.up;
        f.O = flip ? f.pe.p0 : Vec3d(f.pe.p0 - f.pe.up * t);
        f.a0 = f.w0 = 0;
        f.a1 = f.w1 = f.pe.len;
        fl.push_back(f);
    }
    // Where two perpendicular 90-degree flanges meet at an edge end the bends stop short by the other's setback and the
    // legs run on past them until their inner corners are `t` apart (open corner, no fill).
    if (std::abs(angle_deg - 90.0) < 1e-6 && fl.size() > 1) {
        const double ext = std::max(0.0, Ro - t - t / std::sqrt(2.0));
        for (size_t i = 0; i < fl.size(); ++i)
            for (size_t j = 0; j < fl.size(); ++j) {
                if (i == j || std::abs(fl[i].pe.e.dot(fl[j].pe.e)) > 1e-6) continue;
                for (int end = 0; end < 2; ++end) {
                    const Vec3d& pi = end == 0 ? fl[i].pe.p0 : fl[i].pe.p1;
                    if ((pi - fl[j].pe.p0).norm() > 1e-5 && (pi - fl[j].pe.p1).norm() > 1e-5) continue;
                    if (end == 0) { fl[i].a0 += s; fl[i].w0 += s - ext; }
                    else          { fl[i].a1 -= s; fl[i].w1 -= s - ext; }
                }
            }
    }

    // 1. trim the plate back by the setback (a little beyond each edge end, so a bend of an earlier neighbour that
    //    runs under the new flange is trimmed too; only the sheet level below the bend top is touched, never a wall)
    TopoDS_Shape result = body;
    for (const Flange& f : fl) {
        const Vec3d &m = f.pe.in, &e = f.pe.e;
        auto at = [&](double u, double w, double a) { return Vec3d(f.O + m * u + f.W * w + e * a); };
        const double lo = -1.0, ext_a = Ro;
        Outline o;
        const Vec3d q0 = at(lo, lo, -ext_a), q1 = at(s, lo, -ext_a), q2 = at(s, Ro, -ext_a), q3 = at(lo, Ro, -ext_a);
        o.line(q0, q1); o.line(q1, q2); o.line(q2, q3); o.line(q3, q0);
        result = cut(result, prism(o, e * (f.pe.len + 2 * ext_a)));
    }

    // 2. the bends and the legs
    for (const Flange& f : fl) {
        const Vec3d &m = f.pe.in, &e = f.pe.e;
        auto at = [&](double u, double w, double a) { return Vec3d(f.O + m * u + f.W * w + e * a); };
        const Vec3d C2(s, Ro, 0);   // bend centre in (u, w)
        auto pt = [&](double rad, double phi, double a) { return at(C2.x() + rad * std::cos(phi), C2.y() + rad * std::sin(phi), a); };
        const double phi0 = -M_PI / 2, phi1 = phi0 - th, phim = 0.5 * (phi0 + phi1);
        const double ca = f.a0, cl = f.a1 - f.a0;
        if (cl > 1e-9) {
            Outline b;
            b.arc(pt(Ro, phi0, ca), pt(Ro, phim, ca), pt(Ro, phi1, ca));
            b.line(pt(Ro, phi1, ca), pt(R, phi1, ca));
            b.arc(pt(R, phi1, ca), pt(R, phim, ca), pt(R, phi0, ca));
            b.line(pt(R, phi0, ca), pt(Ro, phi0, ca));
            result = fuse(result, prism(b, e * cl));
        }
        // the leg continues the bend's end along (-cos th, sin th) in (u, w)
        const double dir_u = -std::cos(th), dir_w = std::sin(th);
        auto leg_pt = [&](double rad, double a) {
            return at(C2.x() + rad * std::cos(phi1) + leg * dir_u, C2.y() + rad * std::sin(phi1) + leg * dir_w, a);
        };
        const double la = f.w0, ll = f.w1 - f.w0;
        if (ll > 1e-9) {
            Outline l;
            l.line(pt(Ro, phi1, la), leg_pt(Ro, la));
            l.line(leg_pt(Ro, la), leg_pt(R, la));
            l.line(leg_pt(R, la), pt(R, phi1, la));
            l.line(pt(R, phi1, la), pt(Ro, phi1, la));
            result = fuse(result, prism(l, e * ll));
        }
    }
    return unify(result);
}

TopoDS_Shape contour_flange(const std::vector<Vec3d>& path, const Vec3d& normal, double width, const Rule& rule, bool flip)
{
    if (path.size() < 2) throw std::runtime_error(_u8L("sheet metal: the contour needs at least one line"));
    if (std::abs(width) < 1e-6) throw std::runtime_error(_u8L("sheet metal: the width must not be zero"));
    const double t = rule.t, R = rule.radius();
    const Vec3d n = normal.normalized();
    const size_t N = path.size();

    std::vector<Vec3d> dir(N - 1), side(N - 1);
    for (size_t i = 0; i + 1 < N; ++i) {
        const Vec3d d = path[i + 1] - path[i];
        if (d.norm() < 1e-9) throw std::runtime_error(_u8L("sheet metal: the contour has a zero-length line"));
        dir[i]  = d.normalized();
        side[i] = dir[i].cross(n) * (flip ? -1.0 : 1.0);   // the material side of this line
    }

    // Per corner: the bend circle is concentric for both faces; the path face is the OUTER face when the bend turns
    // toward the material and the INNER face when it turns away.
    struct Corner { bool bend{false}; Vec3d c, in_t, out_t, mid; double rp{0}, ro{0}; };
    std::vector<Corner> cn(N);
    for (size_t i = 1; i + 1 < N; ++i) {
        const Vec3d &d0 = dir[i - 1], &d1 = dir[i];
        const double cs = std::clamp(d0.dot(d1), -1.0, 1.0);
        const double ang = std::acos(cs);
        if (ang < 1e-6) continue;
        if (ang > M_PI - 1e-3) throw std::runtime_error(_u8L("sheet metal: the contour folds back on itself"));
        Corner& c = cn[i];
        c.bend = true;
        const bool toward = d1.dot(side[i - 1]) > 0;
        c.rp = toward ? R + t : R;
        c.ro = toward ? R : R + t;
        const double T = c.rp * std::tan(ang / 2);
        c.in_t  = path[i] - d0 * T;
        c.out_t = path[i] + d1 * T;
        const Vec3d bis = (d1 - d0).normalized();
        c.c = path[i] + bis * (c.rp / std::cos(ang / 2));
        c.mid = c.c + (path[i] - c.c).normalized() * c.rp;
    }
    // the straight part of each line must have room left between its two bends
    for (size_t i = 0; i + 1 < N; ++i) {
        const double a = cn[i].bend ? (cn[i].out_t - path[i]).norm() : 0.0;
        const double b = cn[i + 1].bend ? (cn[i + 1].in_t - path[i + 1]).norm() : 0.0;
        if (a + b > (path[i + 1] - path[i]).norm() + 1e-9)
            throw std::runtime_error(_u8L("sheet metal: a contour line is too short for its bends"));
    }

    auto opp = [&](const Vec3d& pathpt, const Corner& c) { return Vec3d(c.c + (pathpt - c.c) * (c.ro / c.rp)); };
    Outline o;
    // along the path face
    Vec3d cur = path[0];
    for (size_t i = 1; i + 1 < N; ++i) {
        if (!cn[i].bend) continue;
        o.line(cur, cn[i].in_t);
        o.arc(cn[i].in_t, cn[i].mid, cn[i].out_t);
        cur = cn[i].out_t;
    }
    o.line(cur, path[N - 1]);
    // cap, then back along the opposite face
    const Vec3d last_off = path[N - 1] + side[N - 2] * t;
    o.line(path[N - 1], last_off);
    cur = last_off;
    for (int i = int(N) - 2; i >= 1; --i) {
        if (!cn[size_t(i)].bend) continue;
        const Corner& c = cn[size_t(i)];
        const Vec3d a = opp(c.out_t, c), b = opp(c.in_t, c), m = opp(c.mid, c);
        o.line(cur, a);
        o.arc(a, m, b);
        cur = b;
    }
    const Vec3d first_off = path[0] + side[0] * t;
    o.line(cur, first_off);
    o.line(first_off, path[0]);
    return unify(prism(o, n * width));
}

TopoDS_Shape flat_hem(const TopoDS_Shape& body, const TopoDS_Edge& edge, const Rule& rule, double length)
{
    const double t = rule.t, g = rule.gap;
    const double Rh = g + t;                 // outer radius of the fold
    const double leg = length - Rh;
    if (leg <= 1e-6) throw std::runtime_error(_u8L("sheet metal: the hem is too short for its fold"));

    EdgeFaces ef;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, ef);
    // here the "end face" is the thickness face of the flange top, so `analyse` gives `in` = into the sheet along the
    // flange (down) and `up` = the normal of the sheet face the edge lies on (the face to fold away from)
    const PlateEdge pe = analyse(body, ef, edge, t);
    const Vec3d out = -pe.in;                // continues the flange
    const Vec3d fold = -pe.up;               // over the sheet
    auto at = [&](double f, double o) { return Vec3d(pe.p0 + fold * f + out * o); };
    Outline h;
    const double cf = Rh;
    h.arc(at(0, 0), at(cf, Rh), at(2 * Rh, 0));
    h.line(at(2 * Rh, 0), at(2 * Rh, -leg));
    h.line(at(2 * Rh, -leg), at(t + 2 * g, -leg));
    h.line(at(t + 2 * g, -leg), at(t + 2 * g, 0));
    h.arc(at(t + 2 * g, 0), at(cf, g), at(t, 0));
    h.line(at(t, 0), at(0, 0));
    return unify(fuse(body, prism(h, pe.e * pe.len)));
}


// ---- unfold / refold ------------------------------------------------------------------------------------------------

namespace {

double volume_of(const TopoDS_Shape& s)
{
    if (s.IsNull()) return 0;
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return std::abs(g.Mass());
}

std::vector<TopoDS_Shape> solids_of(const TopoDS_Shape& s)
{
    std::vector<TopoDS_Shape> v;
    if (s.IsNull()) return v;
    for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next())
        if (volume_of(ex.Current()) > 1e-9) v.push_back(ex.Current());
    return v;
}

TopoDS_Shape fuzzy_fuse(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    BRepAlgoAPI_Fuse f;
    NCollection_List<TopoDS_Shape> args, tools;
    args.Append(a); tools.Append(b);
    f.SetArguments(args); f.SetTools(tools);
    f.SetFuzzyValue(1e-6);
    f.Build();
    if (!f.IsDone() || f.Shape().IsNull()) throw std::runtime_error(_u8L("sheet metal: joining the flat pieces failed"));
    return f.Shape();
}

TopoDS_Shape moved(const TopoDS_Shape& s, const gp_Trsf& t)
{
    return BRepBuilderAPI_Transform(s, t, true).Shape();
}

struct Bend {
    Vec3d A, a, X, Y;        // cylinder: a point on the axis, the axis, and the frame its angle is measured in
    double u0{0}, u1{0}, v0{0}, v1{0};
    double theta{0};
    TopoDS_Shape solid;      // the bend sector of the sheet
    int web0{-1}, web1{-1};  // the flat pieces at the u0 and u1 ends
    Vec3d radial(double u) const { return X * std::cos(u) + Y * std::sin(u); }
};

// the wedge of space around a bend between two angles (radius reaches past the outer face), `a0`..`a1` measured as u
TopoDS_Shape wedge(const Bend& b, double a0, double a1, double v0, double v1, double radius)
{
    const Vec3d base = b.A + b.a * v0;
    auto pt = [&](double rad, double ang) { return Vec3d(base + b.radial(ang) * rad); };
    Outline o;
    o.line(base, pt(radius, a0));
    o.arc(pt(radius, a0), pt(radius, 0.5 * (a0 + a1)), pt(radius, a1));
    o.line(pt(radius, a1), base);
    return prism(o, b.a * (v1 - v0));
}

} // namespace

struct Unfolding {
    TopoDS_Shape folded, flat;
    struct Web { TopoDS_Shape flat_solid; gp_Trsf to_flat; };
    struct Strip {
        TopoDS_Shape flat_solid;
        gp_Trsf parent;                    // folded -> flat transform of the piece the strip hangs from
        Vec3d C, a, rp, w;                 // bend centre, axis, radial at the parent end, direction the radial turns in
        double theta{0}, BA{0}, v0{0}, v1{0}, Rw{0};
        Vec3d A;
    };
    std::vector<Web> webs;
    std::vector<Strip> strips;
};

std::shared_ptr<const Unfolding> unfold(const TopoDS_Shape& body, const Rule& rule)
{
    const double t = rule.t, R = rule.radius(), Ro = rule.outer();
    auto solid_at = [&](const Vec3d& q) {
        for (TopExp_Explorer ex(body, TopAbs_SOLID); ex.More(); ex.Next()) {
            BRepClass3d_SolidClassifier cl(ex.Current(), P(q), 1e-7);
            if (cl.State() == TopAbs_IN) return true;
        }
        return false;
    };

    // 1. the bends: outer cylinder faces of radius R + t that bulge out of the material and turn less than half a circle
    std::vector<Bend> bends;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Cylinder || std::abs(s.Cylinder().Radius() - Ro) > 1e-5) continue;
        Bend b;
        const gp_Cylinder cy = s.Cylinder();
        b.A = vec(cy.Location());
        b.a = Vec3d(cy.Axis().Direction().X(), cy.Axis().Direction().Y(), cy.Axis().Direction().Z());
        b.X = Vec3d(cy.Position().XDirection().X(), cy.Position().XDirection().Y(), cy.Position().XDirection().Z());
        b.Y = Vec3d(cy.Position().YDirection().X(), cy.Position().YDirection().Y(), cy.Position().YDirection().Z());
        BRepTools::UVBounds(f, b.u0, b.u1, b.v0, b.v1);
        b.theta = b.u1 - b.u0;
        if (b.theta < 1e-4 || b.theta > M_PI - 1e-3) continue;
        const double um = 0.5 * (b.u0 + b.u1), vm = 0.5 * (b.v0 + b.v1);
        const Vec3d rad = b.radial(um), pm = b.A + b.a * vm + rad * Ro;
        if (solid_at(pm + rad * (0.1 * t)) || !solid_at(pm - rad * (0.5 * t))) continue;   // a hole, not a bend
        bends.push_back(b);
    }
    if (bends.empty()) throw std::runtime_error(_u8L("unfold: the body has no bends"));

    // 2. cut the sheet into the bend sectors and what is left (the flat pieces)
    auto u = std::make_shared<Unfolding>();
    u->folded = body;
    TopoDS_Shape rest = body;
    for (Bend& b : bends) {
        b.solid = BRepAlgoAPI_Common(body, wedge(b, b.u0, b.u1, b.v0, b.v1, Ro + 0.5 * t)).Shape();
        rest = cut(rest, wedge(b, b.u0, b.u1, b.v0, b.v1, Ro + 0.5 * t));
    }
    const std::vector<TopoDS_Shape> webs = solids_of(rest);
    if (webs.size() < 2) throw std::runtime_error(_u8L("unfold: the sheet did not separate into bends and flat pieces"));
    for (Bend& b : bends) {
        const double vm = 0.5 * (b.v0 + b.v1);
        for (int end = 0; end < 2; ++end) {
            const Vec3d q = b.A + b.a * vm + b.radial(end == 0 ? b.u0 : b.u1) * (0.5 * (R + Ro));
            const TopoDS_Shape vtx = BRepBuilderAPI_MakeVertex(P(q)).Vertex();
            for (size_t i = 0; i < webs.size(); ++i) {
                BRepExtrema_DistShapeShape d(vtx, webs[i]);
                if (d.IsDone() && d.Value() < 1e-5) { (end == 0 ? b.web0 : b.web1) = int(i); break; }
            }
        }
        if (b.web0 < 0 || b.web1 < 0) throw std::runtime_error(_u8L("unfold: a bend is not between two flat pieces"));
    }

    // 3. the stationary piece: the one holding the largest planar face
    int stat = 0;
    double best = -1;
    for (size_t i = 0; i < webs.size(); ++i)
        for (TopExp_Explorer ex(webs[i], TopAbs_FACE); ex.More(); ex.Next()) {
            const TopoDS_Face f = TopoDS::Face(ex.Current());
            if (BRepAdaptor_Surface(f).GetType() != GeomAbs_Plane) continue;
            GProp_GProps g;
            BRepGProp::SurfaceProperties(f, g);
            if (g.Mass() > best) { best = g.Mass(); stat = int(i); }
        }

    // 4. walk the bends outward from it, laying each flat piece into the plane of its parent
    std::vector<gp_Trsf> T(webs.size());
    std::vector<char> seen(webs.size(), 0);
    seen[size_t(stat)] = 1;
    u->strips.reserve(bends.size());
    for (bool grew = true; grew;) {
        grew = false;
        for (const Bend& b : bends) {
            for (int side = 0; side < 2; ++side) {
                const int par = side == 0 ? b.web0 : b.web1, chi = side == 0 ? b.web1 : b.web0;
                if (!seen[size_t(par)] || seen[size_t(chi)]) continue;
                const Vec3d rp = b.radial(side == 0 ? b.u0 : b.u1), rc = b.radial(side == 0 ? b.u1 : b.u0);
                // the arc turns from rp toward rc; w is the direction the radial turns in
                Vec3d w = b.a.cross(rp);
                if (w.dot(rc) < 0) w = -w;
                const double ang = std::atan2(rc.cross(rp).dot(b.a), rc.dot(rp));   // rotates the child's end onto the parent's
                const double BA = b.theta * (R + rule.k * t);
                gp_Trsf rot, tr;
                rot.SetRotation(gp_Ax1(P(b.A), gp_Dir(b.a.x(), b.a.y(), b.a.z())), ang);
                tr.SetTranslation(gp_Vec(w.x() * BA, w.y() * BA, w.z() * BA));
                T[size_t(chi)] = T[size_t(par)].Multiplied(tr.Multiplied(rot));
                seen[size_t(chi)] = 1;
                grew = true;
                // the strip: the bend's cross-section at the parent end, pulled straight along w for BA
                const Vec3d base = b.A + b.a * b.v0;
                Outline o;
                const Vec3d s0 = base + rp * R, s1 = base + rp * Ro;
                o.line(s0, s1); o.line(s1, s1 + w * BA); o.line(s1 + w * BA, s0 + w * BA); o.line(s0 + w * BA, s0);
                Unfolding::Strip st;
                st.parent = T[size_t(par)];
                st.flat_solid = moved(prism(o, b.a * (b.v1 - b.v0)), st.parent);
                st.C = b.A; st.A = b.A; st.a = b.a; st.rp = rp; st.w = w;
                st.theta = b.theta; st.BA = BA; st.v0 = b.v0; st.v1 = b.v1; st.Rw = Ro + 0.5 * t;
                u->strips.push_back(st);
            }
        }
    }
    for (char c : seen) if (!c) throw std::runtime_error(_u8L("unfold: the sheet is not one connected piece"));

    TopoDS_Shape flat;
    for (size_t i = 0; i < webs.size(); ++i) {
        Unfolding::Web w;
        w.to_flat = T[i];
        w.flat_solid = moved(webs[i], T[i]);
        u->webs.push_back(w);
        flat = flat.IsNull() ? w.flat_solid : fuzzy_fuse(flat, w.flat_solid);
    }
    for (const Unfolding::Strip& st : u->strips) flat = fuzzy_fuse(flat, st.flat_solid);
    u->flat = unify(flat);
    return u;
}

TopoDS_Shape flat_shape(const Unfolding& u) { return u.flat; }

TopoDS_Shape refold(const Unfolding& u, const TopoDS_Shape& edited_flat)
{
    const TopoDS_Shape removal = cut(u.flat, edited_flat);
    if (volume_of(removal) < 1e-9) return u.folded;
    TopoDS_Shape result = u.folded;

    // material taken from a flat piece goes back to the same place on the folded piece
    for (const Unfolding::Web& w : u.webs)
        for (const TopoDS_Shape& piece : solids_of(BRepAlgoAPI_Common(removal, w.flat_solid).Shape()))
            result = cut(result, moved(piece, w.to_flat.Inverted()));

    // material taken from a strip: the same span of the bend, as a wedge between two angles
    for (const Unfolding::Strip& st : u.strips)
        for (const TopoDS_Shape& piece : solids_of(BRepAlgoAPI_Common(removal, st.flat_solid).Shape())) {
            const TopoDS_Shape local = moved(piece, st.parent.Inverted());
            double s_lo = 1e300, s_hi = -1e300, x_lo = 1e300, x_hi = -1e300;
            const TriangleMesh mesh = SketchEngine::tessellate(local);
            for (const stl_vertex& v : mesh.its.vertices) {
                const Vec3d p(v.x(), v.y(), v.z());
                const double s = (p - st.C).dot(st.w), x = (p - st.A).dot(st.a);
                s_lo = std::min(s_lo, s); s_hi = std::max(s_hi, s);
                x_lo = std::min(x_lo, x); x_hi = std::max(x_hi, x);
            }
            s_lo = std::clamp(s_lo, 0.0, st.BA); s_hi = std::clamp(s_hi, 0.0, st.BA);
            x_lo = std::max(x_lo, st.v0);        x_hi = std::min(x_hi, st.v1);
            if (s_hi - s_lo < 1e-9 || x_hi - x_lo < 1e-9) continue;
            // wedge between the angles that these distances along the strip fold to, measured from the parent end
            const double a0 = s_lo / st.BA * st.theta, a1 = s_hi / st.BA * st.theta;
            const Vec3d base = st.A + st.a * x_lo;
            auto pt = [&](double ang) { return Vec3d(base + (st.rp * std::cos(ang) + st.w * std::sin(ang)) * st.Rw); };
            Outline o;
            o.line(base, pt(a0));
            o.arc(pt(a0), pt(0.5 * (a0 + a1)), pt(a1));
            o.line(pt(a1), base);
            result = cut(result, prism(o, st.a * (x_hi - x_lo)));
        }
    return unify(result);
}

} // namespace SheetMetal
} // namespace Slic3r
