#include "libslic3r/CAD/GeometryEngine.hpp"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepLProp_SLProps.hxx>
#include <gp_Cylinder.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <stdexcept>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <TopExp.hxx>
#include <TopTools.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <Poly_Triangulation.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <GeomLProp_SLProps.hxx>
#include <BRepAdaptor_Curve.hxx>
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
#include <TopoDS_Vertex.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <array>
#include <map>
#include <cmath>

namespace Slic3r {

// ---- STEP import (B-rep, not mesh) ----
std::vector<TopoDS_Shape> GeometryEngine::read_step_solids(const std::string& path, std::string& err)
{
    err.clear();
    std::vector<TopoDS_Shape> out;
    try {
        STEPControl_Reader reader;
        if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) {
            err = "cannot read STEP file";
            return out;
        }
        reader.TransferRoots();
        const TopoDS_Shape shape = reader.OneShape();
        if (shape.IsNull()) { err = "STEP file has no geometry"; return out; }
        // One body per top-level solid; fall back to the whole shape (shells/faces) if none.
        for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next())
            out.push_back(ex.Current());
        if (out.empty())
            out.push_back(shape);
    } catch (const Standard_Failure& e) {
        err = e.GetMessageString() ? e.GetMessageString() : "OCCT failed to read STEP";
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
        throw std::runtime_error("mesh_to_brep: mesh has no triangles");

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
        throw std::runtime_error("mesh_to_brep: every triangle was rejected as degenerate "
                                 "(try a smaller tolerance)");

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

// ---- Primitive creation ----

TopoDS_Solid GeometryEngine::make_primitive(const PrimitiveParams& params)
{
    switch (params.type) {
    case PrimitiveType::Box:
        return BRepPrimAPI_MakeBox(gp_Pnt(-params.box_w/2, -params.box_d/2, 0),
                                   params.box_w, params.box_d, params.box_h).Solid();
    case PrimitiveType::Cylinder:
        return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0,0,0), gp_Dir(0,0,1)),
                                        params.cyl_radius, params.cyl_height).Solid();
    case PrimitiveType::Sphere:
        return BRepPrimAPI_MakeSphere(gp_Pnt(0,0,params.sph_radius), params.sph_radius).Solid();
    case PrimitiveType::Cone:
        return BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(0,0,0), gp_Dir(0,0,1)),
                                    params.cone_r1, params.cone_r2, params.cone_height).Solid();
    case PrimitiveType::Torus:
        return BRepPrimAPI_MakeTorus(gp_Ax2(gp_Pnt(0,0,params.torus_r2), gp_Dir(0,0,1)),
                                     params.torus_r1, params.torus_r2).Solid();
    default:
        return BRepPrimAPI_MakeBox(gp_Pnt(-10,-10,0), 20,20,20).Solid();
    }
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
    std::vector<TopoDS_Edge> result;
    if (target == FaceGroup::All) {
        for (TopExp_Explorer exp(solid, TopAbs_EDGE); exp.More(); exp.Next())
            result.push_back(TopoDS::Edge(exp.Current()));
        return result;
    }

    // Build edge-to-face map once
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;
    TopExp::MapShapesAndAncestors(solid, TopAbs_EDGE, TopAbs_FACE, edgeFaceMap);

    for (TopExp_Explorer edgeExp(solid, TopAbs_EDGE); edgeExp.More(); edgeExp.Next()) {
        const TopoDS_Edge& edge = TopoDS::Edge(edgeExp.Current());
        if (!edgeFaceMap.Contains(edge)) continue;
        const TopTools_ListOfShape& faces = edgeFaceMap.FindFromKey(edge);

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

TopoDS_Shape GeometryEngine::apply_fillet(const TopoDS_Shape& solid, double radius, FaceGroup faces)
{
    if (radius <= 0.001) return solid;

    std::vector<TopoDS_Edge> edges = collect_edges(solid, faces);
    if (edges.empty()) return solid;

    BRepFilletAPI_MakeFillet fillet(solid);
    for (const auto& edge : edges)
        fillet.Add(radius, edge);
    fillet.Build();

    // A too-large radius (e.g. >= half the smallest spanned dimension) makes the
    // operation degenerate; OCCT leaves IsDone() false. Report it instead of
    // silently returning the unfilleted solid (which reads as a false success).
    if (!fillet.IsDone()) throw std::runtime_error("fillet radius too large for this geometry");
    return fillet.Shape();
}

TopoDS_Shape GeometryEngine::apply_chamfer(const TopoDS_Shape& solid, double distance, FaceGroup faces)
{
    if (distance <= 0.001) return solid;

    std::vector<TopoDS_Edge> edges = collect_edges(solid, faces);
    if (edges.empty()) return solid;

    BRepFilletAPI_MakeChamfer chamfer(solid);
    for (const auto& edge : edges)
        chamfer.Add(distance, edge); // symmetric chamfer
    chamfer.Build();

    if (!chamfer.IsDone()) throw std::runtime_error("chamfer distance too large for this geometry");
    return chamfer.Shape();
}

TopoDS_Shape GeometryEngine::apply_fillet(const TopoDS_Shape& solid, double radius, int edge_id)
{
    if (radius <= 0.001) return solid;

    TopoDS_Edge edge = edge_by_index(solid, edge_id);
    if (edge.IsNull()) throw std::runtime_error("apply_fillet: invalid edge id");

    BRepFilletAPI_MakeFillet mk(solid);
    mk.Add(radius, edge);
    mk.Build();

    if (!mk.IsDone()) throw std::runtime_error("apply_fillet: OCCT fillet failed");
    return mk.Shape();
}

TopoDS_Shape GeometryEngine::apply_chamfer(const TopoDS_Shape& solid, double distance, int edge_id)
{
    if (distance <= 0.001) return solid;

    TopoDS_Edge edge = edge_by_index(solid, edge_id);
    if (edge.IsNull()) throw std::runtime_error("apply_chamfer: invalid edge id");

    BRepFilletAPI_MakeChamfer mk(solid);
    mk.Add(distance, edge);
    mk.Build();

    if (!mk.IsDone()) throw std::runtime_error("apply_chamfer: OCCT chamfer failed");
    return mk.Shape();
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

std::string GeometryEngine::primitive_name(PrimitiveType type)
{
    switch (type) {
    case PrimitiveType::Box:      return "Box";
    case PrimitiveType::Cylinder: return "Cylinder";
    case PrimitiveType::Sphere:   return "Sphere";
    case PrimitiveType::Cone:     return "Cone";
    case PrimitiveType::Torus:    return "Torus";
    default:                      return "Unknown";
    }
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
    TopTools_IndexedMapOfShape map;
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
    TopTools_IndexedMapOfShape map;
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
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(shape, TopAbs_EDGE, map);
    return map.Extent();
}

TopoDS_Edge GeometryEngine::edge_by_index(const TopoDS_Shape& shape, int index)
{
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(shape, TopAbs_EDGE, map);
    if (index < 0 || index >= map.Extent())
        return TopoDS_Edge();
    return TopoDS::Edge(map(index + 1));
}

int GeometryEngine::edge_index_of(const TopoDS_Shape& shape, const TopoDS_Edge& edge)
{
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(shape, TopAbs_EDGE, map);
    int idx = map.FindIndex(edge);
    return (idx > 0) ? (idx - 1) : -1;
}

} // namespace Slic3r
