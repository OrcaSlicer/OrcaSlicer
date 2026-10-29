#include "libslic3r/CAD/GeometryEngine.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/I18N.hpp"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepLProp_SLProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <cstddef>
#include <algorithm>
#include <GeomAbs_SurfaceType.hxx>
#include <TopAbs_Orientation.hxx>
#include <Standard_Handle.hxx>
#include <cstdint>
#include <Poly_Triangle.hxx>
#include <GeomAbs_CurveType.hxx>
#include <gp_Cylinder.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <gp_Pln.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <math.h>
#include <gp_Mat.hxx>
#include <stdexcept>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <TopExp.hxx>
#include <TopTools.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <NCollection_IndexedDataMap.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_List.hxx>
#include <Poly_Triangulation.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <GeomLProp_SLProps.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <Geom_Surface.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Law_BSpline.hxx>
#include <gp_Pnt2d.hxx>
#include <NCollection_Array1.hxx>
#include <Law_BSpFunc.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <BRep_Tool.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <gp_Circ.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <STEPControl_Reader.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Standard_Failure.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <BRepTools_History.hxx>
#include <array>
#include <map>
#include <cmath>
#include <vector>
#include <string>
#include <utility>
#include <TopTools_ListOfShape.hxx>
#include <TopAbs_State.hxx>
#include <exception>

namespace Slic3r {

using ShapeIndexMap = NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;

// ---- STEP import (B-rep, not mesh) ----
std::vector<TopoDS_Shape> GeometryEngine::read_step_solids(const std::string& path, std::string& err)
{
    err.clear();
    std::vector<TopoDS_Shape> out;
    try {
        STEPControl_Reader reader;
        if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) {
            err = _u8L("cannot read STEP file");
            return out;
        }
        reader.TransferRoots();
        const TopoDS_Shape shape = reader.OneShape();
        if (shape.IsNull()) { err = _u8L("STEP file has no geometry"); return out; }
        // One body per top-level solid; fall back to the whole shape (shells/faces) if none.
        for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next())
            out.push_back(ex.Current());
        if (out.empty())
            out.push_back(shape);
    } catch (const Standard_Failure& e) {
        err = *e.what() ? e.what() : _u8L("OCCT failed to read STEP");
        out.clear();
    }
    return out;
}

// ---- Mesh -> B-rep (faceted, shared topology by construction) ----
//
// Port of mesh2step's brep_build.py. Two properties are load-bearing and easy to lose:
//
//  1. The edge cache is keyed on the UNORDERED vertex-index pair, and a triangle that walks
//     the edge backwards (i > j) gets edge.Reversed(). Consistently-wound meshes (STL/OBJ/3MF
//     all are) walk every shared edge in opposite directions from its two adjacent triangles,
//     so this reversal is exactly what leaves the faces coherently outward-oriented.
//  2. Degeneracy is split in two, deliberately. A triangle is dropped as sub-resolution noise
//     only if its longest edge is below `tolerance` (an absolute floor), while sliver rejection
//     is scale-INDEPENDENT (area < 1e-9 * longest_edge^2). Folding the two together under one
//     `area < tolerance^2` test rejects legitimate thin CAD slivers whenever tolerance is coarse
//     relative to them, turning a watertight input into a falsely-open shell — a real regression
//     mesh2step hit on a 62k-triangle mechanical part.
TopoDS_Shape GeometryEngine::mesh_to_brep(const indexed_triangle_set& its,
                                          double tolerance,
                                          double merge_angle_deg,
                                          MeshBrepStats& stats)
{
    stats = MeshBrepStats{};
    stats.input_tris = int(its.indices.size());
    if (tolerance <= 0.0)
        throw std::runtime_error("mesh_to_brep: tolerance must be > 0");
    if (its.indices.empty())
        throw std::runtime_error(_u8L("mesh_to_brep: mesh has no triangles"));

    // 1. Tolerance-quantized vertex dedup. A merged vertex keeps the exact coordinates of the
    //    first input occurrence — vertices are grouped by a cell, never snapped onto its grid.
    std::map<std::array<long long, 3>, int> cell_to_new;
    std::vector<int>   old_to_new(its.vertices.size(), -1);
    std::vector<Vec3d> verts;
    verts.reserve(its.vertices.size());
    for (size_t i = 0; i < its.vertices.size(); ++i) {
        const Vec3d p = its.vertices[i].cast<double>();
        const std::array<long long, 3> cell{ (long long) std::llround(p.x() / tolerance),
                                             (long long) std::llround(p.y() / tolerance),
                                             (long long) std::llround(p.z() / tolerance) };
        auto ins = cell_to_new.emplace(cell, int(verts.size()));
        if (ins.second)
            verts.push_back(p);
        old_to_new[i] = ins.first->second;
    }

    // 2. Reject degenerate triangles (see the two-part rule in the comment above).
    std::vector<Vec3i32> tris;
    tris.reserve(its.indices.size());
    for (const Vec3i32& t : its.indices) {
        const int a = old_to_new[t(0)], b = old_to_new[t(1)], c = old_to_new[t(2)];
        if (a == b || b == c || a == c) { ++stats.degenerate_collapsed; continue; }
        const Vec3d& pa = verts[a]; const Vec3d& pb = verts[b]; const Vec3d& pc = verts[c];
        const double e0 = (pb - pa).norm(), e1 = (pc - pb).norm(), e2 = (pa - pc).norm();
        const double longest = std::max(e0, std::max(e1, e2));
        if (longest < tolerance) { ++stats.degenerate_collapsed; continue; }
        const double area = 0.5 * (pb - pa).cross(pc - pa).norm();
        if (area < 1e-9 * longest * longest) { ++stats.degenerate_sliver; continue; }
        tris.emplace_back(a, b, c);
    }
    stats.kept_tris = int(tris.size());
    if (tris.empty())
        throw std::runtime_error(_u8L("mesh_to_brep: every triangle was rejected as degenerate "
                                      "(try a smaller tolerance)"));

    // 3. One face per triangle, sharing vertices and edges through the caches.
    std::vector<TopoDS_Vertex> vertex_cache(verts.size());
    std::vector<bool>          vertex_made(verts.size(), false);
    auto get_vertex = [&](int i) -> const TopoDS_Vertex& {
        if (!vertex_made[i]) {
            const Vec3d& p = verts[i];
            vertex_cache[i] = BRepBuilderAPI_MakeVertex(gp_Pnt(p.x(), p.y(), p.z())).Vertex();
            vertex_made[i]  = true;
        }
        return vertex_cache[i];
    };

    std::map<std::pair<int, int>, TopoDS_Edge> edge_cache;
    std::map<std::pair<int, int>, int>         edge_usage;
    auto get_edge = [&](int i, int j) -> TopoDS_Edge {
        const std::pair<int, int> key = (i < j) ? std::make_pair(i, j) : std::make_pair(j, i);
        ++edge_usage[key];
        auto it = edge_cache.find(key);
        if (it == edge_cache.end())
            it = edge_cache.emplace(key,
                     BRepBuilderAPI_MakeEdge(get_vertex(key.first), get_vertex(key.second)).Edge()).first;
        return (i > j) ? TopoDS::Edge(it->second.Reversed()) : it->second;
    };

    BRep_Builder  builder;
    TopoDS_Shell  shell;
    builder.MakeShell(shell);

    for (const Vec3i32& t : tris) {
        try {
            BRepBuilderAPI_MakeWire mk_wire(get_edge(t(0), t(1)), get_edge(t(1), t(2)), get_edge(t(2), t(0)));
            if (!mk_wire.IsDone()) { ++stats.faces_failed; continue; }
            BRepBuilderAPI_MakeFace mk_face(mk_wire.Wire());
            if (!mk_face.IsDone()) { ++stats.faces_failed; continue; }
            builder.Add(shell, mk_face.Face());
            ++stats.faces_built;
        } catch (const Standard_Failure&) {
            ++stats.faces_failed;
        }
    }

    // 4. Watertightness falls straight out of the usage counts the cache already gathered.
    for (const auto& kv : edge_usage) {
        if (kv.second == 1)      ++stats.boundary_edges;
        else if (kv.second >= 3) ++stats.nonmanifold_edges;
    }
    stats.unique_edges = int(edge_usage.size());
    stats.watertight   = stats.boundary_edges == 0 && stats.nonmanifold_edges == 0 && stats.unique_edges > 0;

    TopoDS_Shape shape = shell;
    if (stats.watertight && stats.faces_built > 0) {
        BRepBuilderAPI_MakeSolid mk_solid(shell);
        if (mk_solid.IsDone()) {
            TopoDS_Solid solid = mk_solid.Solid();
            GProp_GProps props;
            BRepGProp::VolumeProperties(solid, props);
            double vol = props.Mass();
            if (vol < 0.0) {                                  // inward-wound input
                solid = TopoDS::Solid(solid.Reversed());
                vol   = -vol;
            }
            if (vol > 0.0) {
                shape         = solid;
                stats.is_solid = true;
                stats.volume   = vol;
            }
        }
    }

    // 5. Optional coplanar merge. Faceted output is one planar face per triangle — exact, but
    //    you cannot meaningfully fillet or extrude a face that IS a single triangle. Merging
    //    coplanar neighbours is what turns the import into something the face/edge tools can
    //    actually operate on (a 12-triangle cube collapses to its 6 real faces).
    if (merge_angle_deg > 0.0) {
        try {
            ShapeUpgrade_UnifySameDomain unifier(shape, true, true, true);
            unifier.SetAngularTolerance(merge_angle_deg * M_PI / 180.0);
            unifier.SetLinearTolerance(tolerance);
            unifier.Build();
            const TopoDS_Shape merged = unifier.Shape();
            if (!merged.IsNull())
                shape = merged;
        } catch (const Standard_Failure&) {
            // Merging is an optimisation, not a correctness step: keep the exact faceted shape.
        }
    }
    stats.faces_final = face_count(shape);
    return shape;
}

// ---- Face classification ----

FaceGroup GeometryEngine::classify_face(const TopoDS_Face& face, const TopoDS_Shape& /*solid*/)
{
    try {
        BRepAdaptor_Surface surf(face);
        if (surf.GetType() == GeomAbs_Plane) {
            // Sample normal at center UV
            double u = (surf.FirstUParameter() + surf.LastUParameter()) / 2.0;
            double v = (surf.FirstVParameter() + surf.LastVParameter()) / 2.0;
            gp_Pnt pt; gp_Vec du, dv;
            surf.D1(u, v, pt, du, dv);
            gp_Dir n = du.Crossed(dv);
            if (face.Orientation() == TopAbs_REVERSED) n.Reverse();

            if (n.Z() > 0.7)  return FaceGroup::Top;
            if (n.Z() < -0.7) return FaceGroup::Bottom;
            return FaceGroup::Lateral;
        }
    } catch (...) {}
    return FaceGroup::Lateral;
}

// ---- Edge collection ----

std::vector<TopoDS_Edge> GeometryEngine::collect_edges(const TopoDS_Shape& solid, FaceGroup target)
{
    // Each edge ONCE. An explorer visits a shared edge from both of its faces, so walking one
    // handed every edge to the fillet twice; the de-duplicated map is also what edge_by_index
    // numbers edges by.
    std::vector<TopoDS_Edge> result;
    NCollection_IndexedDataMap<TopoDS_Shape, NCollection_List<TopoDS_Shape>, TopTools_ShapeMapHasher> edgeFaceMap;
    TopExp::MapShapesAndAncestors(solid, TopAbs_EDGE, TopAbs_FACE, edgeFaceMap);
    if (target == FaceGroup::All) {
        for (int i = 1; i <= edgeFaceMap.Extent(); ++i)
            result.push_back(TopoDS::Edge(edgeFaceMap.FindKey(i)));
        return result;
    }

    for (int ei = 1; ei <= edgeFaceMap.Extent(); ++ei) {
        const TopoDS_Edge& edge = TopoDS::Edge(edgeFaceMap.FindKey(ei));
        const NCollection_List<TopoDS_Shape>& faces = edgeFaceMap.FindFromIndex(ei);

        bool include = false;
        for (auto it = faces.begin(); it != faces.end(); ++it) {
            FaceGroup fg = classify_face(TopoDS::Face(*it), solid);
            if (target == FaceGroup::Top && fg == FaceGroup::Top)       { include = true; break; }
            if (target == FaceGroup::Bottom && fg == FaceGroup::Bottom) { include = true; break; }
            if (target == FaceGroup::Lateral && fg == FaceGroup::Lateral) { include = true; break; }
        }

        if (!include && target == FaceGroup::Top) {
            for (auto it = faces.begin(); it != faces.end(); ++it) {
                if (classify_face(TopoDS::Face(*it), solid) == FaceGroup::Top) { include = true; break; }
            }
        }
        if (!include && target == FaceGroup::Bottom) {
            for (auto it = faces.begin(); it != faces.end(); ++it) {
                if (classify_face(TopoDS::Face(*it), solid) == FaceGroup::Bottom) { include = true; break; }
            }
        }
        if (target == FaceGroup::Lateral && !include) {
            int lateralCount = 0;
            for (auto it = faces.begin(); it != faces.end(); ++it) {
                if (classify_face(TopoDS::Face(*it), solid) == FaceGroup::Lateral) ++lateralCount;
            }
            if (lateralCount >= 2) include = true;
        }

        if (include) result.push_back(edge);
    }

    return result;
}

// ---- Fillet/Chamfer ----

// All four dress-up entry points fail the same way — with a reason — instead of two of them
// handing the solid back unchanged, which recompute then reported as a success.
TopoDS_Shape GeometryEngine::apply_fillet(const TopoDS_Shape& solid, double radius, FaceGroup faces)
{
    if (radius <= 0.001) throw std::runtime_error(_u8L("the fillet radius must be greater than 0"));

    std::vector<TopoDS_Edge> edges = collect_edges(solid, faces);
    if (edges.empty()) throw std::runtime_error(_u8L("no edges to fillet in that group"));

    BRepFilletAPI_MakeFillet fillet(solid);
    for (const auto& edge : edges)
        fillet.Add(radius, edge);
    fillet.Build();

    // A too-large radius (e.g. >= half the smallest spanned dimension) makes the
    // operation degenerate; OCCT leaves IsDone() false. Report it instead of
    // silently returning the unfilleted solid (which reads as a false success).
    if (!fillet.IsDone()) throw std::runtime_error(_u8L("fillet radius too large for this geometry"));
    return fillet.Shape();
}

TopoDS_Shape GeometryEngine::apply_chamfer(const TopoDS_Shape& solid, double distance, FaceGroup faces)
{
    if (distance <= 0.001) throw std::runtime_error(_u8L("the chamfer distance must be greater than 0"));

    std::vector<TopoDS_Edge> edges = collect_edges(solid, faces);
    if (edges.empty()) throw std::runtime_error(_u8L("no edges to chamfer in that group"));

    BRepFilletAPI_MakeChamfer chamfer(solid);
    for (const auto& edge : edges)
        chamfer.Add(distance, edge); // symmetric chamfer
    chamfer.Build();

    if (!chamfer.IsDone()) throw std::runtime_error(_u8L("chamfer distance too large for this geometry"));
    return chamfer.Shape();
}

TopoDS_Shape GeometryEngine::apply_fillet(const TopoDS_Shape& solid, double radius, int edge_id)
{
    return apply_fillet(solid, radius, std::vector<int>{ edge_id });
}

TopoDS_Shape GeometryEngine::apply_chamfer(const TopoDS_Shape& solid, double distance, int edge_id)
{
    return apply_chamfer(solid, distance, std::vector<int>{ edge_id });
}

// Angle between the outward normals of the two faces that meet at `edge`, at the edge's midpoint (0 = flat, pi/2 = a box corner).
static double edge_normal_angle(const TopoDS_Shape& solid, const TopoDS_Edge& edge)
{
    TopTools_IndexedDataMapOfShapeListOfShape ef;
    TopExp::MapShapesAndAncestors(solid, TopAbs_EDGE, TopAbs_FACE, ef);
    if (!ef.Contains(edge)) throw std::runtime_error("fillet: the edge is not part of the body");
    const TopTools_ListOfShape& faces = ef.FindFromKey(edge);
    if (faces.Extent() != 2) throw std::runtime_error("fillet: a chord-length fillet needs an edge shared by exactly two faces");
    BRepAdaptor_Curve cur(edge);
    const gp_Pnt mid = cur.Value(0.5 * (cur.FirstParameter() + cur.LastParameter()));
    gp_Vec n[2];
    int k = 0;
    for (const TopoDS_Shape& s : faces) {
        const TopoDS_Face face = TopoDS::Face(s);
        BRepAdaptor_Surface surf(face);
        GeomAPI_ProjectPointOnSurf proj(mid, BRep_Tool::Surface(face));
        if (proj.NbPoints() < 1) throw std::runtime_error("fillet: cannot find the edge on its face");
        double u, v;
        proj.LowerDistanceParameters(u, v);
        BRepLProp_SLProps props(surf, u, v, 1, 1e-6);
        if (!props.IsNormalDefined()) throw std::runtime_error("fillet: the face normal is undefined at the edge");
        n[k] = gp_Vec(props.Normal());
        if (face.Orientation() == TopAbs_REVERSED) n[k].Reverse();
        ++k;
    }
    return n[0].Angle(n[1]);
}

// How much radius fits at relative position `u` along `edge`: the narrower of the two faces measured square to the edge
// (to the face's own boundary), times tan(half the angle between the faces).
static double taper_room(const TopoDS_Shape& solid, const TopoDS_Edge& edge, double u)
{
    TopTools_IndexedDataMapOfShapeListOfShape ef;
    TopExp::MapShapesAndAncestors(solid, TopAbs_EDGE, TopAbs_FACE, ef);
    if (!ef.Contains(edge)) throw std::runtime_error("fillet: the edge is not part of the body");
    BRepAdaptor_Curve cur(edge);
    // the ends of the edge sit on the faces' own boundaries, so measure a hair inside them
    const double t = cur.FirstParameter() + std::clamp(u, 0.002, 0.998) * (cur.LastParameter() - cur.FirstParameter());
    const gp_Pnt p = cur.Value(t);
    gp_Vec tan = cur.DN(t, 1);
    if (tan.Magnitude() < 1e-12) throw std::runtime_error("fillet: the edge has no direction here");
    tan.Normalize();
    const double beta = edge_normal_angle(solid, edge);
    double room = 1e300;
    for (const TopoDS_Shape& fs : ef.FindFromKey(edge)) {
        const TopoDS_Face face = TopoDS::Face(fs);
        BRepAdaptor_Surface surf(face);
        if (surf.GetType() != GeomAbs_Plane) throw std::runtime_error("fillet: fit taper needs flat faces beside the edge");
        gp_Vec n(surf.Plane().Axis().Direction());
        gp_Vec d = n.Crossed(tan);
        d.Normalize();
        BRepTopAdaptor_FClass2d cls(face, 1e-7);
        Handle(Geom_Surface) gs = BRep_Tool::Surface(face);
        auto inside = [&](double dist) {   // the point on the plane, found in the face's own (u, v) and classified there
            GeomAPI_ProjectPointOnSurf pr(gp_Pnt(p.XYZ() + d.XYZ() * dist), gs);
            if (pr.NbPoints() < 1) return false;
            double pu, pv;
            pr.LowerDistanceParameters(pu, pv);
            return cls.Perform(gp_Pnt2d(pu, pv)) == TopAbs_IN;
        };
        if (!inside(1e-4)) d.Reverse();
        if (!inside(1e-4)) throw std::runtime_error("fillet: cannot measure the room beside the edge");
        double lo = 0.0, hi = 1e-4;
        while (inside(hi) && hi < 1e5) { lo = hi; hi *= 2.0; }
        for (int i = 0; i < 40; ++i) { const double m = 0.5 * (lo + hi); (inside(m) ? lo : hi) = m; }
        room = std::min(room, lo);
    }
    return room * std::tan(0.5 * (M_PI - beta));
}

TopoDS_Shape GeometryEngine::apply_fillet(const TopoDS_Shape& solid, double radius, const std::vector<int>& edge_ids)
{
    return apply_fillet(solid, radius, edge_ids, FilletLaw{});
}

static TopoDS_Shape fillet_edges_raw(const TopoDS_Shape& solid, double radius, const std::vector<TopoDS_Edge>& edges, const GeometryEngine::FilletLaw& law)
{
    if (radius <= 0.001) throw std::runtime_error(_u8L("the fillet radius must be greater than 0"));
    if (edges.empty()) throw std::runtime_error("apply_fillet: no edge picked");
    if (law.r2 > 1e6 || radius > 1e6) throw std::runtime_error("the fillet radius is too large");

    BRepFilletAPI_MakeFillet mk(solid);
    auto add_spline = [&](std::vector<double> poles, std::vector<double> kn, const TopoDS_Edge& edge) {
            const int deg = int(kn.size()) - int(poles.size()) - 1;
            if (deg < 1 || deg > 8 || !std::is_sorted(kn.begin(), kn.end()) || kn.front() != 0.0 || kn.back() != 1.0)
                throw std::runtime_error("fillet: the radius spline needs sorted knots from 0 to 1 and degree = knots - radii - 1 between 1 and 8");
            for (double p : poles) if (!(p > 1e-3) || p > 1e6) throw std::runtime_error("fillet: every control radius must be greater than 0");
            if (law.has_start) {
                BRepAdaptor_Curve cur(edge);
                const gp_Pnt a = cur.Value(cur.FirstParameter()), b = cur.Value(cur.LastParameter());
                const gp_Pnt s(law.start.x(), law.start.y(), law.start.z());
                if (s.Distance(b) < s.Distance(a)) {   // the law is written from `start`; the edge runs the other way
                    std::reverse(poles.begin(), poles.end());
                    std::reverse(kn.begin(), kn.end());
                    for (double& k : kn) k = 1.0 - k;
                }
            }
            std::vector<double> uk; std::vector<int> mult;   // distinct knots with their multiplicities
            for (double k : kn) {
                if (!uk.empty() && std::abs(uk.back() - k) < 1e-12) ++mult.back();
                else { uk.push_back(k); mult.push_back(1); }
            }
            TColStd_Array1OfReal pa(1, int(poles.size())), ka(1, int(uk.size()));
            TColStd_Array1OfInteger ma(1, int(mult.size()));
            for (size_t i = 0; i < poles.size(); ++i) pa(int(i) + 1) = poles[i];
            for (size_t i = 0; i < uk.size(); ++i) { ka(int(i) + 1) = uk[i]; ma(int(i) + 1) = mult[i]; }
            Handle(Law_BSpline) bs = new Law_BSpline(pa, ka, ma, deg);
            // OCCT's own law hook (a Law_Function) crashes ChFiDS_FilSpine::Reset on this build, so the spline is handed over as
            // sampled (relative parameter, radius) pairs, which the builder interpolates smoothly itself.
            constexpr int N = 32;
            NCollection_Array1<gp_Pnt2d> samples(1, N + 1);
            for (int i = 0; i <= N; ++i) {
                const double u = double(i) / N;
                double r = 0;
                bs->D0(u, r);
                samples(i + 1) = gp_Pnt2d(u, std::max(r, 1e-3));
            }
            mk.Add(samples, edge);
    };
    for (const TopoDS_Edge& edge : edges) {
        if (law.chord) {
            const double s = std::sin(0.5 * edge_normal_angle(solid, edge));
            if (s < 1e-6) throw std::runtime_error("fillet: the two faces are tangent here, a chord length cannot be turned into a radius");
            mk.Add(radius / (2.0 * s), edge);
        } else if (!law.radii.empty()) {
            std::vector<double> poles(1, radius);
            poles.insert(poles.end(), law.radii.begin(), law.radii.end());
            add_spline(poles, law.knots, edge);
        } else if (law.fit_taper) {
            std::vector<double> kn(4, 0.0), poles;
            for (int i = 1; i < 8; ++i) kn.push_back(i / 8.0);
            kn.insert(kn.end(), 4, 1.0);
            for (int i = 0; i < 11; ++i) {
                const double u = (kn[size_t(i) + 1] + kn[size_t(i) + 2] + kn[size_t(i) + 3]) / 3.0;
                poles.push_back(std::min(radius, 0.9 * taper_room(solid, edge, u)));
            }
            add_spline(poles, kn, edge);
        } else if (law.r2 >= 0.0) {
            double r1 = radius, r2 = std::max(law.r2, 1e-3);   // a round that closes to a point is asked for as a very small one
            if (law.has_start) {
                BRepAdaptor_Curve cur(edge);
                const gp_Pnt a = cur.Value(cur.FirstParameter()), b = cur.Value(cur.LastParameter());
                const gp_Pnt s(law.start.x(), law.start.y(), law.start.z());
                if (s.Distance(b) < s.Distance(a)) std::swap(r1, r2);   // `radius` belongs at the end nearest `start`
            }
            mk.Add(r1, r2, edge);
        } else {
            mk.Add(radius, edge);
        }
    }
    mk.Build();

    if (!mk.IsDone()) throw std::runtime_error(_u8L("apply_fillet: OCCT fillet failed"));
    return mk.Shape();
}


// OCCT grows a fillet / chamfer along every edge tangent to a picked one (a whole contour), so three picked segments of a
// rounded top edge round all of it. The pick must be the only thing that changes: where a contour runs on past the picked
// edges, split the solid by a plane normal to the edge at each such junction, dress each piece (the plane face is no
// longer tangent, so the contour stops there) and fuse the pieces back. The dress ends flat on that plane.
static bool edge_in(const std::vector<TopoDS_Edge>& v, const TopoDS_Shape& e)
{
    for (const TopoDS_Edge& x : v) if (x.IsSame(e)) return true;
    return false;
}

static std::vector<TopoDS_Edge> contour_extras(const TopoDS_Shape& solid, const std::vector<TopoDS_Edge>& edges, bool fillet)
{
    std::vector<TopoDS_Edge> extras;
    auto scan = [&](auto& mk) {
        for (const TopoDS_Edge& e : edges) mk.Add(1.0, e);
        for (int i = 1; i <= mk.NbContours(); ++i)
            for (int j = 1; j <= mk.NbEdges(i); ++j) {
                const TopoDS_Edge x = mk.Edge(i, j);
                if (!edge_in(edges, x) && !edge_in(extras, x)) extras.push_back(x);
            }
    };
    try {
        if (fillet) { BRepFilletAPI_MakeFillet mk(solid); scan(mk); }
        else        { BRepFilletAPI_MakeChamfer mk(solid); scan(mk); }
    } catch (...) { extras.clear(); }
    return extras;
}

template<class Op>
static TopoDS_Shape dress_punctual(const TopoDS_Shape& solid, const std::vector<TopoDS_Edge>& edges, bool fillet, Op op)
{
    const std::vector<TopoDS_Edge> extras = contour_extras(solid, edges, fillet);
    if (extras.empty()) return op(solid, edges);

    // a plane normal to the contour at every vertex where a picked edge meets an unpicked one
    std::vector<std::pair<gp_Pnt, gp_Dir>> cuts;
    for (const TopoDS_Edge& x : extras)
        for (TopExp_Explorer ve(x, TopAbs_VERTEX); ve.More(); ve.Next()) {
            const TopoDS_Vertex v = TopoDS::Vertex(ve.Current());
            for (const TopoDS_Edge& s : edges)
                for (TopExp_Explorer se(s, TopAbs_VERTEX); se.More(); se.Next()) {
                    if (!se.Current().IsSame(v)) continue;
                    BRepAdaptor_Curve c(s);
                    gp_Pnt pt; gp_Vec t;
                    c.D1(BRep_Tool::Parameter(v, s), pt, t);
                    if (t.Magnitude() < 1e-9) continue;
                    bool dup = false;
                    for (const auto& k : cuts) if (k.first.Distance(pt) < 1e-6) { dup = true; break; }
                    if (!dup) cuts.emplace_back(pt, gp_Dir(t));
                }
        }
    if (cuts.empty()) return op(solid, edges);

    Bnd_Box bb;
    BRepBndLib::Add(solid, bb);
    const double L = 2.0 * std::sqrt(bb.SquareExtent()) + 10.0;
    TopTools_ListOfShape args, tools;
    args.Append(solid);
    for (const auto& k : cuts)
        tools.Append(BRepBuilderAPI_MakeFace(gp_Pln(k.first, k.second), -L, L, -L, L).Face());
    BRepAlgoAPI_Splitter sp;
    sp.SetArguments(args);
    sp.SetTools(tools);
    sp.Build();
    if (!sp.IsDone()) throw std::runtime_error(_u8L("cannot limit the dress-up to the picked edges"));

    std::vector<TopoDS_Shape> done;
    for (TopExp_Explorer ex(sp.Shape(), TopAbs_SOLID); ex.More(); ex.Next()) {
        const TopoDS_Shape piece = ex.Current();
        // the piece's edges that lie on a picked one (the splits cut them into several)
        std::vector<TopoDS_Edge> mine;
        NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> all;
        TopExp::MapShapes(piece, TopAbs_EDGE, all);
        for (int i = 1; i <= all.Extent(); ++i) {
            const TopoDS_Edge e = TopoDS::Edge(all(i));
            if (BRep_Tool::Degenerated(e)) continue;
            BRepAdaptor_Curve c(e);
            const TopoDS_Vertex mid = BRepBuilderAPI_MakeVertex(c.Value(0.5 * (c.FirstParameter() + c.LastParameter())));
            for (const TopoDS_Edge& s : edges) {
                BRepExtrema_DistShapeShape d(mid, s);
                if (d.IsDone() && d.Value() < 1e-4) { mine.push_back(e); break; }
            }
        }
        done.push_back(mine.empty() ? piece : op(piece, mine));
    }
    if (done.empty()) throw std::runtime_error(_u8L("cannot limit the dress-up to the picked edges"));
    TopoDS_Shape out = done.front();
    if (done.size() > 1) {
        TopTools_ListOfShape a, t;
        a.Append(done.front());
        for (size_t i = 1; i < done.size(); ++i) t.Append(done[i]);
        BRepAlgoAPI_Fuse fu;
        fu.SetArguments(a);
        fu.SetTools(t);
        fu.Build();
        if (!fu.IsDone()) throw std::runtime_error(_u8L("cannot limit the dress-up to the picked edges"));
        out = fu.Shape();
    }
    ShapeUpgrade_UnifySameDomain u(out, true, true, true);
    u.Build();
    out = u.Shape();
    if (!BRepCheck_Analyzer(out).IsValid()) throw std::runtime_error(_u8L("cannot limit the dress-up to the picked edges"));
    return out;
}

static TopoDS_Shape fillet_edges(const TopoDS_Shape& solid, double radius, const std::vector<TopoDS_Edge>& edges, const GeometryEngine::FilletLaw& law)
{
    if (radius <= 0.001) throw std::runtime_error("the fillet radius must be greater than 0");
    if (edges.empty()) throw std::runtime_error("apply_fillet: no edge picked");
    return dress_punctual(solid, edges, true, [&](const TopoDS_Shape& s, const std::vector<TopoDS_Edge>& es) {
        return fillet_edges_raw(s, radius, es, law);
    });
}

TopoDS_Shape GeometryEngine::apply_fillet(const TopoDS_Shape& solid, double radius, const std::vector<int>& edge_ids, const FilletLaw& law)
{
    std::vector<TopoDS_Edge> edges;
    for (int id : edge_ids) {
        TopoDS_Edge edge = edge_by_index(solid, id);
        if (edge.IsNull()) throw std::runtime_error("apply_fillet: invalid edge id");
        edges.push_back(edge);
    }
    try {
        return fillet_edges(solid, radius, edges, law);
    } catch (const std::exception&) {
        // An intersection curve that the surfaces' seams cut into several edges (two cylinders that cross) makes the builder
        // give up; merged back into one edge it often does not. Unify the body, follow the edges through the merge, retry.
        TopoDS_Shape merged;
        std::vector<TopoDS_Edge> now;
        try {
            ShapeUpgrade_UnifySameDomain u(solid, true, true, true);
            u.Build();
            merged = u.Shape();
            const Handle(BRepTools_History) h = u.History();
            for (const TopoDS_Edge& e : edges) {
                bool any = false;
                if (!h.IsNull())
                    for (const TopoDS_Shape& m : h->Modified(e)) { now.push_back(TopoDS::Edge(m)); any = true; }
                if (!any && !(!h.IsNull() && h->IsRemoved(e))) now.push_back(e);
            }
        } catch (...) { merged.Nullify(); }
        if (!merged.IsNull() && !now.empty()) {
            std::vector<TopoDS_Edge> uniq;   // several seam pieces collapse onto one edge
            for (const TopoDS_Edge& e : now) {
                bool dup = false;
                for (const TopoDS_Edge& q : uniq) if (q.IsSame(e)) { dup = true; break; }
                if (!dup) uniq.push_back(e);
            }
            try { return fillet_edges(merged, radius, uniq, law); } catch (const std::exception&) {}
        }
        throw;
    }
}

TopoDS_Shape GeometryEngine::apply_chamfer(const TopoDS_Shape& solid, double distance, const std::vector<int>& edge_ids)
{
    if (distance <= 0.001) throw std::runtime_error(_u8L("the chamfer distance must be greater than 0"));
    if (edge_ids.empty()) throw std::runtime_error("apply_chamfer: no edge picked");

    std::vector<TopoDS_Edge> edges;
    for (int id : edge_ids) {
        TopoDS_Edge edge = edge_by_index(solid, id);
        if (edge.IsNull()) throw std::runtime_error("apply_chamfer: invalid edge id");
        edges.push_back(edge);
    }
    return dress_punctual(solid, edges, false, [&](const TopoDS_Shape& sh, const std::vector<TopoDS_Edge>& es) {
        BRepFilletAPI_MakeChamfer mk(sh);
        for (const TopoDS_Edge& e : es) mk.Add(distance, e);
        mk.Build();
        if (!mk.IsDone()) throw std::runtime_error(_u8L("apply_chamfer: OCCT chamfer failed"));
        return mk.Shape();
    });
}

// ---- Tessellation ----

TriangleMesh GeometryEngine::tessellate(const TopoDS_Shape& shape,
                                        double linear_deflection,
                                        double angular_deflection)
{
    BRepMesh_IncrementalMesh mesh(shape, linear_deflection, false, angular_deflection, true);

    int nbNodes = 0, nbTri = 0;
    for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(TopoDS::Face(exp.Current()), loc);
        if (!tri.IsNull()) { nbNodes += tri->NbNodes(); nbTri += tri->NbTriangles(); }
    }
    if (nbTri == 0 || nbNodes == 0) return TriangleMesh{};

    stl_file stl;
    stl.stats.type = inmemory;
    stl.stats.number_of_facets = (uint32_t)nbTri;
    stl.stats.original_num_facets = stl.stats.number_of_facets;
    stl_allocate(&stl);

    std::vector<Vec3f> pts; pts.reserve(nbNodes);
    int ndOff = 0, trOff = 0;
    for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {
        const TopoDS_Shape& F = exp.Current();
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(TopoDS::Face(F), loc);
        if (tri.IsNull()) continue;
        gp_Trsf T = loc.Transformation();
        for (int i = 1; i <= tri->NbNodes(); ++i) {
            gp_Pnt p = tri->Node(i); p.Transform(T);
            pts.emplace_back(Vec3f(p.X(), p.Y(), p.Z()));
        }
        auto orient = exp.Current().Orientation();
        int ids[3];
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
            Poly_Triangle t = tri->Triangle(i); t.Get(ids[0], ids[1], ids[2]);
            if (orient == TopAbs_REVERSED) std::swap(ids[1], ids[2]);
            stl_facet f;
            f.vertex[0] = pts[ids[0]+ndOff-1].cast<float>();
            f.vertex[1] = pts[ids[1]+ndOff-1].cast<float>();
            f.vertex[2] = pts[ids[2]+ndOff-1].cast<float>();
            f.extra[0]=0; f.extra[1]=0;
            stl_normal n; stl_calculate_normal(n,&f); stl_normalize_vector(n);
            f.normal=n; stl.facet_start[trOff+i-1]=f;
        }
        ndOff += tri->NbNodes(); trOff += tri->NbTriangles();
    }
    TriangleMesh result; result.from_stl(stl); return result;
}

GeometryEngine::Deviation
GeometryEngine::surface_deviation(const TopoDS_Shape& candidate,
                                  const TopoDS_Shape& reference,
                                  double linear_deflection)
{
    Deviation d;
    if (candidate.IsNull() || reference.IsNull()) return d;
    TriangleMesh mesh = tessellate(candidate, linear_deflection, 0.5);
    const auto& verts = mesh.its.vertices;
    if (verts.empty()) return d;
    double sum = 0.0, sumsq = 0.0;
    int n = 0;
    for (const auto& v : verts) {
        gp_Pnt p(v.x(), v.y(), v.z());
        BRepExtrema_DistShapeShape dss(BRepBuilderAPI_MakeVertex(p).Vertex(), reference);
        if (!dss.IsDone() || dss.NbSolution() < 1) continue;
        double dist = dss.Value();
        d.max_mm = std::max(d.max_mm, dist);
        sum += dist; sumsq += dist * dist; ++n;
    }
    d.sample_count = n;
    if (n > 0) { d.mean_mm = sum / n; d.rms_mm = std::sqrt(sumsq / n); }
    return d;
}

GeometryEngine::MassProps GeometryEngine::mass_properties(const TopoDS_Shape& shape)
{
    MassProps p;
    if (shape.IsNull()) return p;
    try {
        // A sheet body (open shell, no solid) encloses nothing, and BRepGProp::VolumeProperties
        // integrates the divergence theorem over whatever faces exist — on an open shell that is
        // not a volume at all. It came back as 96000 with an inertia diagonal of
        // [-4.2e7, -4.2e7, -6.9e7] for a 60x60x40 four-walled box: negative principal moments,
        // which no real body can have. The old code then hid the only obvious tell by taking
        // std::abs() of the mass. Report the honest answer instead — surface area is still
        // meaningful, so this is not a failure, just not a solid.
        p.is_solid = TopExp_Explorer(shape, TopAbs_SOLID).More();
        if (!p.is_solid) {
            GProp_GProps sonly;
            BRepGProp::SurfaceProperties(shape, sonly);
            p.surface_area = sonly.Mass();
            p.valid = true;          // the area IS trustworthy; volume/inertia stay zero
            return p;
        }
        GProp_GProps vprops;
        BRepGProp::VolumeProperties(shape, vprops);
        double mass = vprops.Mass();
        if (std::abs(mass) < 1e-30) return p;
        p.volume        = std::abs(mass);
        p.center_of_mass = Vec3d(vprops.CentreOfMass().X(), vprops.CentreOfMass().Y(), vprops.CentreOfMass().Z());
        gp_Mat mat = vprops.MatrixOfInertia();
        p.inertia = {{
            mat(1,1), mat(1,2), mat(1,3),
            mat(2,1), mat(2,2), mat(2,3),
            mat(3,1), mat(3,2), mat(3,3),
        }};
        GProp_GProps sprops;
        BRepGProp::SurfaceProperties(shape, sprops);
        p.surface_area = sprops.Mass();
        p.valid = true;
    } catch (const Standard_Failure&) {
        // leave valid = false
    }
    return p;
}

// ---- Topology accessors ----

int GeometryEngine::face_count(const TopoDS_Shape& shape)
{
    int n = 0;
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next())
        ++n;
    return n;
}

TopoDS_Face GeometryEngine::face_by_index(const TopoDS_Shape& shape, int index)
{
    if (index < 0) return TopoDS_Face();
    int ordinal = 0;
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
        if (ordinal == index)
            return TopoDS::Face(e.Current());
        ++ordinal;
    }
    return TopoDS_Face();
}

std::vector<TopoDS_Face> GeometryEngine::faces_of(const TopoDS_Shape& shape)
{
    std::vector<TopoDS_Face> out;
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next())
        out.push_back(TopoDS::Face(e.Current()));   // same order as face_by_index
    return out;
}

std::vector<TopoDS_Edge> GeometryEngine::edges_of(const TopoDS_Shape& shape)
{
    ShapeIndexMap map;
    TopExp::MapShapes(shape, TopAbs_EDGE, map);     // same order as edge_by_index
    std::vector<TopoDS_Edge> out;
    out.reserve(map.Extent());
    for (int i = 1; i <= map.Extent(); ++i)
        out.push_back(TopoDS::Edge(map(i)));
    return out;
}

std::vector<TopoDS_Edge> GeometryEngine::edges_of_face(const TopoDS_Face& face)
{
    std::vector<TopoDS_Edge> result;
    ShapeIndexMap map;
    TopExp::MapShapes(face, TopAbs_EDGE, map);
    for (int i = 1; i <= map.Extent(); ++i)
        result.push_back(TopoDS::Edge(map(i)));
    return result;
}

std::vector<Vec3d> GeometryEngine::sample_edge_world(const TopoDS_Edge& edge, double chord_tol)
{
    if (BRep_Tool::Degenerated(edge))
        return {};

    BRepAdaptor_Curve curve(edge);
    GCPnts_TangentialDeflection disc(curve, 0.1, chord_tol);

    std::vector<Vec3d> pts;
    if (disc.NbPoints() >= 2) {
        for (int i = 1; i <= disc.NbPoints(); ++i) {
            gp_Pnt p = disc.Value(i);
            pts.emplace_back(p.X(), p.Y(), p.Z());
        }
    } else {
        gp_Pnt p0 = curve.Value(curve.FirstParameter());
        gp_Pnt p1 = curve.Value(curve.LastParameter());
        pts.emplace_back(p0.X(), p0.Y(), p0.Z());
        pts.emplace_back(p1.X(), p1.Y(), p1.Z());
    }
    return pts;
}

std::vector<std::vector<Vec3d>> GeometryEngine::display_edges(const TopoDS_Shape& shape, double chord_tol)
{
    NCollection_IndexedDataMap<TopoDS_Shape, NCollection_List<TopoDS_Shape>, TopTools_ShapeMapHasher> faces_of_edge;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, faces_of_edge);
    std::vector<std::vector<Vec3d>> out;
    for (int i = 1; i <= faces_of_edge.Extent(); ++i) {
        const TopoDS_Edge& edge = TopoDS::Edge(faces_of_edge.FindKey(i));
        if (BRep_Tool::Degenerated(edge))
            continue;
        bool seam = false;
        for (NCollection_List<TopoDS_Shape>::Iterator it(faces_of_edge.FindFromIndex(i)); it.More() && !seam; it.Next())
            seam = BRep_Tool::IsClosed(edge, TopoDS::Face(it.Value()));
        if (seam)
            continue;
        std::vector<Vec3d> pts = sample_edge_world(edge, chord_tol);
        if (pts.size() >= 2)
            out.push_back(std::move(pts));
    }
    return out;
}

Vec3d GeometryEngine::face_centroid_world(const TopoDS_Face& face)
{
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    gp_Pnt c = props.CentreOfMass();
    return Vec3d(c.X(), c.Y(), c.Z());
}

Vec3d GeometryEngine::face_normal_world(const TopoDS_Face& face)
{
    BRepAdaptor_Surface surf(face);
    const double u = 0.5 * (surf.FirstUParameter() + surf.LastUParameter());
    const double v = 0.5 * (surf.FirstVParameter() + surf.LastVParameter());
    BRepLProp_SLProps props(surf, u, v, 1, 1e-6);
    gp_Dir n(0.0, 0.0, 1.0);
    if (props.IsNormalDefined()) n = props.Normal();
    if (face.Orientation() == TopAbs_REVERSED) n.Reverse();   // outward (account for face winding)
    return Vec3d(n.X(), n.Y(), n.Z());
}

GeometryEngine::CylinderFace GeometryEngine::cylinder_of_face(const TopoDS_Face& face)
{
    CylinderFace cf;
    if (face.IsNull()) return cf;
    BRepAdaptor_Surface surf(face);
    if (surf.GetType() != GeomAbs_Cylinder) return cf;

    const gp_Cylinder cyl = surf.Cylinder();
    const gp_Ax1      ax  = cyl.Axis();
    const Vec3d axis(ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z());
    const Vec3d apt (ax.Location().X(),  ax.Location().Y(),  ax.Location().Z());
    cf.radius = cyl.Radius();

    // Axial extent: V is the axial parameter on a cylinder; bound the face's two ends and
    // order them so `axis` points base -> top.
    const double umid = 0.5 * (surf.FirstUParameter() + surf.LastUParameter());
    const gp_Pnt e0 = surf.Value(umid, surf.FirstVParameter());
    const gp_Pnt e1 = surf.Value(umid, surf.LastVParameter());
    double t0 = (Vec3d(e0.X(), e0.Y(), e0.Z()) - apt).dot(axis);
    double t1 = (Vec3d(e1.X(), e1.Y(), e1.Z()) - apt).dot(axis);
    if (t1 < t0) std::swap(t0, t1);
    cf.base   = apt + axis * t0;
    cf.axis   = axis;
    cf.height = t1 - t0;

    // Internal (bore) vs external: compare the face's outward normal at its centre to the
    // outward radial direction. A bore's normal points toward the axis (dot < 0).
    const gp_Pnt sp = surf.Value(umid, 0.5 * (surf.FirstVParameter() + surf.LastVParameter()));
    const Vec3d  S(sp.X(), sp.Y(), sp.Z());
    const Vec3d  axpt   = cf.base + axis * (S - cf.base).dot(axis);
    const Vec3d  radial = (S - axpt).normalized();
    cf.internal = face_normal_world(face).dot(radial) < 0.0;
    cf.ok = true;
    return cf;
}

GeometryEngine::CylinderFace GeometryEngine::circle_of_edge(const TopoDS_Edge& edge)
{
    CylinderFace cf;
    if (edge.IsNull()) return cf;
    BRepAdaptor_Curve curve(edge);
    if (curve.GetType() != GeomAbs_Circle) return cf;
    const gp_Circ c  = curve.Circle();
    const gp_Ax1  ax = c.Axis();
    cf.base     = Vec3d(c.Location().X(), c.Location().Y(), c.Location().Z());
    cf.axis     = Vec3d(ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z());
    cf.radius   = c.Radius();
    cf.height   = 0.0;       // an edge carries no axial extent; the card keeps the current length
    cf.internal = false;     // ambiguous from an edge alone — default external, user can toggle
    cf.ok       = true;
    return cf;
}

bool GeometryEngine::face_plane_bounds(const TopoDS_Face& face, const Vec3d& origin,
                                       const Vec3d& x_axis, const Vec3d& y_axis,
                                       double& umin, double& umax, double& vmin, double& vmax)
{
    umin = vmin = 1e30; umax = vmax = -1e30;
    bool any = false;
    for (TopExp_Explorer ex(face, TopAbs_VERTEX); ex.More(); ex.Next()) {
        const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(ex.Current()));
        const Vec3d P(p.X(), p.Y(), p.Z());
        const double u = (P - origin).dot(x_axis);
        const double v = (P - origin).dot(y_axis);
        umin = std::min(umin, u); umax = std::max(umax, u);
        vmin = std::min(vmin, v); vmax = std::max(vmax, v);
        any = true;
    }
    return any;
}

int GeometryEngine::edge_count(const TopoDS_Shape& shape)
{
    ShapeIndexMap map;
    TopExp::MapShapes(shape, TopAbs_EDGE, map);
    return map.Extent();
}

TopoDS_Edge GeometryEngine::edge_by_index(const TopoDS_Shape& shape, int index)
{
    ShapeIndexMap map;
    TopExp::MapShapes(shape, TopAbs_EDGE, map);
    if (index < 0 || index >= map.Extent())
        return TopoDS_Edge();
    return TopoDS::Edge(map(index + 1));
}

int GeometryEngine::edge_index_of(const TopoDS_Shape& shape, const TopoDS_Edge& edge)
{
    ShapeIndexMap map;
    TopExp::MapShapes(shape, TopAbs_EDGE, map);
    int idx = map.FindIndex(edge);
    return (idx > 0) ? (idx - 1) : -1;
}

// Unit tangent of `e` at its vertex `v` (the end of the curve that vertex sits on).
static bool edge_tangent_at(const TopoDS_Edge& e, const TopoDS_Vertex& v, gp_Vec& t)
{
    BRepAdaptor_Curve c(e);
    const gp_Pnt p = BRep_Tool::Pnt(v);
    const double u = c.Value(c.FirstParameter()).SquareDistance(p) <= c.Value(c.LastParameter()).SquareDistance(p)
                         ? c.FirstParameter() : c.LastParameter();
    gp_Pnt q; gp_Vec d;
    c.D1(u, q, d);
    if (d.Magnitude() < 1e-12) return false;
    t = d.Normalized();
    return true;
}

std::vector<int> GeometryEngine::tangent_edge_chain(const TopoDS_Shape& shape, int index, double deg)
{
    std::vector<int> out;
    ShapeIndexMap emap;
    TopExp::MapShapes(shape, TopAbs_EDGE, emap);
    if (index < 0 || index >= emap.Extent()) return out;
    TopTools_IndexedDataMapOfShapeListOfShape v2e;
    TopExp::MapShapesAndAncestors(shape, TopAbs_VERTEX, TopAbs_EDGE, v2e);
    const double cs = std::cos(deg * M_PI / 180.0);
    std::vector<char> seen(size_t(emap.Extent()), 0);
    std::vector<int> stack{index};
    seen[size_t(index)] = 1;
    while (!stack.empty()) {
        const int cur = stack.back(); stack.pop_back();
        out.push_back(cur);
        const TopoDS_Edge ce = TopoDS::Edge(emap(cur + 1));
        for (TopExp_Explorer vx(ce, TopAbs_VERTEX); vx.More(); vx.Next()) {
            const TopoDS_Vertex v = TopoDS::Vertex(vx.Current());
            gp_Vec t0;
            if (!edge_tangent_at(ce, v, t0)) continue;
            const int vi = v2e.FindIndex(v);
            if (vi <= 0) continue;
            for (const TopoDS_Shape& s : v2e(vi)) {
                const int ni = emap.FindIndex(s) - 1;
                if (ni < 0 || seen[size_t(ni)]) continue;
                gp_Vec t1;
                if (!edge_tangent_at(TopoDS::Edge(s), v, t1)) continue;
                if (std::abs(t0.Dot(t1)) >= cs) { seen[size_t(ni)] = 1; stack.push_back(ni); }
            }
        }
    }
    return out;
}

} // namespace Slic3r
