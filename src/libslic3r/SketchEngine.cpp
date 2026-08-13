#include "SketchEngine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <TopoDS_Vertex.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GC_MakeArcOfEllipse.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <gp_Ax2.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <BRep_Builder.hxx>
#include <ShapeFix_Face.hxx>
#include <GeomAbs_Shape.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <GeomAPI_IntCS.hxx>
#include <map>
#include <tuple>
#include <stdexcept>

namespace Slic3r {

// ---- SketchPlane ----

gp_Pln SketchPlane::to_occt() const
{
    gp_Pnt o(origin.x(), origin.y(), origin.z());
    gp_Dir n(normal.x(), normal.y(), normal.z());
    gp_Dir x(x_axis.x(), x_axis.y(), x_axis.z());
    return gp_Pln(gp_Ax3(o, n, x));
}

SketchPlane SketchPlane::from_face(const TopoDS_Face& face)
{
    SketchPlane sp;
    // Use the first triangulation vertex + face normal
    TopLoc_Location loc;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (!tri.IsNull() && tri->NbNodes() > 0) {
        gp_Pnt p = tri->Node(1).Transformed(loc.Transformation());
        sp.origin = Vec3d(p.X(), p.Y(), p.Z());
    }
    // Compute normal from BRep face
    BRepAdaptor_Surface surf(face);
    if (surf.GetType() != GeomAbs_Plane) {
        // Fallback: try to use the first uv point
        double u1 = surf.FirstUParameter(), v1 = surf.FirstVParameter();
        gp_Pnt pt; gp_Vec du, dv;
        surf.D1(u1, v1, pt, du, dv);
        gp_Dir n = gp_Dir(du.Crossed(dv));
        sp.normal = Vec3d(n.X(), n.Y(), n.Z());
        sp.x_axis = Vec3d(du.X(), du.Y(), du.Z()).normalized();
        sp.y_axis = sp.normal.cross(sp.x_axis).normalized();
        return sp;
    }
    // Plane face — use the plane directly
    gp_Pln pln = surf.Plane();
    sp.origin = Vec3d(pln.Location().X(), pln.Location().Y(), pln.Location().Z());
    gp_Dir n = pln.Axis().Direction();
    sp.normal = Vec3d(n.X(), n.Y(), n.Z());
    gp_Dir xd = pln.XAxis().Direction();
    sp.x_axis = Vec3d(xd.X(), xd.Y(), xd.Z());
    sp.y_axis = sp.normal.cross(sp.x_axis).normalized();
    return sp;
}

Vec2d SketchPlane::project(const Vec3d& ray_origin, const Vec3d& ray_dir) const
{
    double denom = ray_dir.dot(normal);
    if (std::abs(denom) < 1e-12)
        return {0, 0}; // ray parallel to plane
    double t = (origin - ray_origin).dot(normal) / denom;
    if (t < 0)
        return {0, 0}; // behind camera
    Vec3d hit = ray_origin + t * ray_dir;
    Vec3d local = hit - origin;
    return {local.dot(x_axis), local.dot(y_axis)};
}

Vec3d SketchPlane::to_world(const Vec2d& pt) const
{
    return origin + x_axis * pt.x() + y_axis * pt.y();
}

// ---- SketchProfile ----

bool SketchProfile::is_closed(double tolerance) const
{
    if (points.size() < 3) return false;
    return (points.front() - points.back()).norm() < tolerance;
}

bool SketchProfile::try_close(double tolerance)
{
    if (is_closed(tolerance)) {
        closed = true;
        return true;
    }
    if (points.size() < 2) return false;
    if ((points.front() - points.back()).norm() < tolerance) {
        closed = true;
        return true;
    }
    return false;
}

TopoDS_Wire SketchProfile::to_occt_wire(const SketchPlane& plane) const
{
    if (points.size() < 2)
        throw std::runtime_error("Profile has fewer than 2 points");

    BRepBuilderAPI_MakeWire builder;
    for (size_t i = 0; i < points.size(); ++i) {
        Vec3d a3 = plane.to_world(points[i]);
        Vec3d b3 = plane.to_world(points[(i + 1) % points.size()]);
        gp_Pnt pa(a3.x(), a3.y(), a3.z());
        gp_Pnt pb(b3.x(), b3.y(), b3.z());
        builder.Add(BRepBuilderAPI_MakeEdge(pa, pb).Edge());
    }
    builder.Build();
    if (!builder.IsDone())
        throw std::runtime_error("Failed to build wire from profile");
    return builder.Wire();
}

// ---- SketchEngine ----

static TopoDS_Shape extrude_face_internal(const TopoDS_Face& face, const gp_Dir& dir, double length, bool symmetric)
{
    gp_Vec vec = gp_Vec(dir) * length;
    if (symmetric) {
        gp_Vec halfVec = gp_Vec(dir) * (length / 2.0);
        BRepPrimAPI_MakePrism pos(face, halfVec);
        BRepPrimAPI_MakePrism neg(face, -halfVec);
        if (!pos.IsDone() || !neg.IsDone()) throw std::runtime_error("Symmetric extrude failed");
        BRepAlgoAPI_Fuse fuse(pos.Shape(), neg.Shape());
        if (!fuse.IsDone()) throw std::runtime_error("Fuse failed");
        return fuse.Shape();
    }
    BRepPrimAPI_MakePrism prism(face, vec);
    if (!prism.IsDone()) throw std::runtime_error("Extrude failed");
    return prism.Shape();
}

TopoDS_Shape SketchEngine::make_extrude(const TopoDS_Wire& wire, const SketchPlane& plane,
                                        double length, bool symmetric, double taper_deg)
{
    BRepBuilderAPI_MakeFace fm(wire);
    if (!fm.IsDone()) throw std::runtime_error("Failed to make face from wire");
    return make_extrude(fm.Face(), plane, length, symmetric, taper_deg);
}

TopoDS_Shape SketchEngine::make_extrude(const TopoDS_Face& face, const SketchPlane& plane,
                                        double length, bool symmetric, double /*taper_deg*/)
{
    gp_Dir dir(plane.normal.x(), plane.normal.y(), plane.normal.z());
    return extrude_face_internal(face, dir, length, symmetric);
}

TopoDS_Shape SketchEngine::make_extrude_two_sided(const TopoDS_Wire& wire, const SketchPlane& plane,
                                                  double up, double down)
{
    BRepBuilderAPI_MakeFace fm(wire);
    if (!fm.IsDone()) throw std::runtime_error("Failed to make face from wire");
    return make_extrude_two_sided(fm.Face(), plane, up, down);
}

TopoDS_Shape SketchEngine::make_extrude_two_sided(const TopoDS_Face& face, const SketchPlane& plane,
                                                  double up, double down)
{
    gp_Dir dir(plane.normal.x(), plane.normal.y(), plane.normal.z());
    const double u = std::abs(up), d = std::abs(down);
    if (u < 1e-9 && d < 1e-9) return TopoDS_Shape();
    if (d < 1e-9) { BRepPrimAPI_MakePrism p(face, gp_Vec(dir) *  u); return p.Shape(); }
    if (u < 1e-9) { BRepPrimAPI_MakePrism p(face, gp_Vec(dir) * -d); return p.Shape(); }
    BRepPrimAPI_MakePrism pos(face, gp_Vec(dir) *  u);
    BRepPrimAPI_MakePrism neg(face, gp_Vec(dir) * -d);
    BRepAlgoAPI_Fuse fuse(pos.Shape(), neg.Shape());
    if (!fuse.IsDone()) throw std::runtime_error("two-sided extrude fuse failed");
    return fuse.Shape();
}

TopoDS_Shape SketchEngine::make_extrude_taper(const TopoDS_Wire& wire, const SketchPlane& plane,
                                              double length, double taper_deg)
{
    gp_Dir dir(plane.normal.x(), plane.normal.y(), plane.normal.z());
    auto straight = [&]() -> TopoDS_Shape {
        BRepBuilderAPI_MakeFace fm(wire);
        BRepPrimAPI_MakePrism prism(fm.Face(), gp_Vec(dir) * length);
        return prism.Shape();
    };
    if (std::abs(taper_deg) >= 89.0 || std::abs(length) < 1e-9) return straight();
    const double off = length * std::tan(taper_deg * M_PI / 180.0);
    if (std::abs(off) < 1e-7) return straight();
    try {
        // 1) offset the planar base wire in its own plane by `off`
        BRepOffsetAPI_MakeOffset mko(wire, GeomAbs_Arc);
        mko.Perform(off);
        if (!mko.IsDone()) return straight();
        TopoDS_Shape offShape = mko.Shape();
        TopoDS_Wire topFlat;
        if (offShape.ShapeType() == TopAbs_WIRE) topFlat = TopoDS::Wire(offShape);
        else { for (TopExp_Explorer ex(offShape, TopAbs_WIRE); ex.More(); ex.Next()) { topFlat = TopoDS::Wire(ex.Current()); break; } }
        if (topFlat.IsNull()) return straight();
        // 2) lift it along the normal by `length`
        gp_Trsf tr; tr.SetTranslation(gp_Vec(dir) * length);
        BRepBuilderAPI_Transform xf(topFlat, tr, Standard_True);
        TopoDS_Wire topWire = TopoDS::Wire(xf.Shape());
        // 3) loft base -> top into a solid
        BRepOffsetAPI_ThruSections loft(Standard_True /*solid*/, Standard_False /*ruled*/);
        loft.AddWire(wire);
        loft.AddWire(topWire);
        loft.Build();
        if (!loft.IsDone()) return straight();
        TopoDS_Shape s = loft.Shape();
        if (s.IsNull()) return straight();
        return s;
    } catch (const Standard_Failure&) {
        return straight();
    }
}

TopoDS_Shape SketchEngine::make_extrude_face(const TopoDS_Face& face, const SketchPlane& plane,
                                             double length, bool symmetric, double /*taper_deg*/)
{
    gp_Dir dir(plane.normal.x(), plane.normal.y(), plane.normal.z());
    return extrude_face_internal(face, dir, length, symmetric);
}

TopoDS_Shape SketchEngine::make_extrude_regions(
    const std::vector<std::vector<std::vector<Vec2d>>>& regions,
    const SketchPlane& plane, double length, bool symmetric)
{
    // Drop consecutive coincident points and the closing duplicate. FreeType /
    // SVG flattening routinely emits repeated points which would build a
    // degenerate OCCT edge and make the wire builder throw — sanitising keeps a
    // single bad glyph from killing the whole extrude.
    auto clean = [](const std::vector<Vec2d>& pts) {
        const double eps2 = 1e-12;   // ~1e-6 mm
        std::vector<Vec2d> out;
        out.reserve(pts.size());
        for (const Vec2d& p : pts)
            if (out.empty() || (p - out.back()).squaredNorm() > eps2)
                out.push_back(p);
        while (out.size() >= 2 && (out.front() - out.back()).squaredNorm() <= eps2)
            out.pop_back();
        return out;
    };

    // Build a closed planar wire from a contour. Never throws — returns a null
    // wire on any failure so the caller can skip just that contour. Winding is
    // NOT normalised here: ShapeFix_Face::FixOrientation() below classifies outer
    // vs hole by geometry and fixes orientations, which is robust to the
    // inconsistent winding Emboss/NSVG glyph contours arrive with (manual
    // winding guesses extrude holed glyphs (P, e, o, 8) inverted or solid).
    auto contour_wire = [&](const std::vector<Vec2d>& raw) -> TopoDS_Wire {
        std::vector<Vec2d> pts = clean(raw);
        if (pts.size() < 3) return TopoDS_Wire{};
        SketchProfile prof;
        prof.points = std::move(pts);
        prof.closed = true;
        try { return prof.to_occt_wire(plane); }
        catch (...) { return TopoDS_Wire{}; }
    };

    gp_Dir dir(plane.normal.x(), plane.normal.y(), plane.normal.z());

    // Accumulate each region's solid into a compound rather than boolean-fusing:
    // glyphs are independent profiles, so a compound avoids every boolean-failure
    // mode (and is faster). Holes are still handled per region by MakeFace.
    BRep_Builder bb;
    TopoDS_Compound comp;
    bb.MakeCompound(comp);
    int count = 0;
    TopoDS_Shape last;

    for (const auto& region : regions) {
        if (region.empty()) continue;
        try {
            TopoDS_Wire outer = contour_wire(region[0]);
            if (outer.IsNull()) continue;

            // Add the outer loop + every hole loop as-is, then let ShapeFix_Face
            // classify outer vs holes by area/containment and set correct wire
            // orientations. This is winding-independent, so holed glyphs extrude
            // with a solid body and empty counters regardless of source winding.
            BRepBuilderAPI_MakeFace fm(outer);
            if (!fm.IsDone()) continue;
            for (size_t h = 1; h < region.size(); ++h) {
                TopoDS_Wire hole = contour_wire(region[h]);
                if (hole.IsNull()) continue;
                fm.Add(hole);
            }
            if (!fm.IsDone()) continue;

            ShapeFix_Face sff(fm.Face());
            sff.FixOrientation();
            const TopoDS_Face face = sff.Face();

            TopoDS_Shape solid = extrude_face_internal(face, dir, length, symmetric);
            bb.Add(comp, solid);
            last = solid;
            ++count;
        } catch (...) {
            continue;   // skip one bad glyph rather than fail the whole insert
        }
    }

    if (count == 0) throw std::runtime_error("imported regions produced no extrudable geometry");
    return count == 1 ? last : TopoDS_Shape(comp);   // avoid a compound-of-one
}

TopoDS_Shape SketchEngine::make_revolve(const TopoDS_Wire& wire, const SketchPlane& plane,
                                        double angle_deg, int axis_sel)
{
    BRepBuilderAPI_MakeFace faceMaker(wire);
    if (!faceMaker.IsDone())
        throw std::runtime_error("Failed to make face from wire");
    TopoDS_Face face = faceMaker.Face();

    // Revolution axis lies in the sketch plane through its origin: X (0) or Y (1).
    const Vec3d& adir = (axis_sel == 1) ? plane.y_axis : plane.x_axis;
    gp_Pnt o(plane.origin.x(), plane.origin.y(), plane.origin.z());
    gp_Dir xd(adir.x(), adir.y(), adir.z());
    gp_Ax1 axis(o, xd);

    double angle_rad = angle_deg * M_PI / 180.0;
    // A negative angle is expressed as a positive sweep about the reversed axis,
    // since BRepPrimAPI_MakeRevol expects an angle in (0, 2*pi].
    if (angle_rad < 0) { axis.Reverse(); angle_rad = -angle_rad; }
    BRepPrimAPI_MakeRevol rev(face, axis, angle_rad);
    if (!rev.IsDone())
        throw std::runtime_error("Failed to revolve");
    return rev.Shape();
}

TopoDS_Shape SketchEngine::make_sweep(const TopoDS_Wire& profile, const TopoDS_Wire& path)
{
    BRepBuilderAPI_MakeFace faceMaker(profile);
    if (!faceMaker.IsDone())
        throw std::runtime_error("Failed to make face from sweep profile");
    TopoDS_Face face = faceMaker.Face();

    BRepOffsetAPI_MakePipe pipe(path, face);
    pipe.Build();
    if (!pipe.IsDone())
        throw std::runtime_error("Failed to sweep profile along path");
    return pipe.Shape();
}

TopoDS_Shape SketchEngine::make_loft(const std::vector<TopoDS_Wire>& profiles, bool ruled)
{
    if (profiles.size() < 2)
        throw std::runtime_error("loft needs at least 2 profiles");
    BRepOffsetAPI_ThruSections loft(Standard_True /*solid*/,
                                    ruled ? Standard_True : Standard_False);
    for (const TopoDS_Wire& w : profiles) {
        if (w.IsNull()) throw std::runtime_error("loft: null profile wire");
        loft.AddWire(w);
    }
    loft.Build();
    if (!loft.IsDone()) throw std::runtime_error("loft failed");
    TopoDS_Shape s = loft.Shape();
    if (s.IsNull()) throw std::runtime_error("loft produced no solid");
    return s;
}

// ponytail: sibling of make_loft that builds an open shell (sheet) instead of a solid.
TopoDS_Shape SketchEngine::make_loft_surface(const std::vector<TopoDS_Wire>& profiles, bool ruled)
{
    if (profiles.size() < 2)
        throw std::runtime_error("loft needs at least 2 profiles");
    BRepOffsetAPI_ThruSections loft(Standard_False /*shell, no end caps*/,
                                    ruled ? Standard_True : Standard_False);
    for (const TopoDS_Wire& w : profiles) {
        if (w.IsNull()) throw std::runtime_error("loft: null profile wire");
        loft.AddWire(w);
    }
    loft.Build();
    if (!loft.IsDone()) throw std::runtime_error("loft failed");
    TopoDS_Shape s = loft.Shape();
    if (s.IsNull()) throw std::runtime_error("loft produced no shape");
    return s;
}

TopoDS_Shape SketchEngine::make_pocket(const TopoDS_Wire& wire, const SketchPlane& plane,
                                        const TopoDS_Shape& target, double depth)
{
    BRepBuilderAPI_MakeFace fm(wire);
    if (!fm.IsDone()) throw std::runtime_error("Pocket face failed");
    TopoDS_Shape tool = extrude_face_internal(fm.Face(),
        gp_Dir(plane.normal.x(), plane.normal.y(), plane.normal.z()), depth + 1.0, false);
    BRepAlgoAPI_Cut cut(target, tool);
    if (!cut.IsDone()) throw std::runtime_error("Pocket cut failed");
    return cut.Shape();
}

TriangleMesh SketchEngine::tessellate(const TopoDS_Shape& shape,
                                      double linear_deflection,
                                      double angular_deflection)
{
    std::vector<int> dummy;
    return tessellate(shape, dummy, linear_deflection, angular_deflection);
}

TriangleMesh SketchEngine::tessellate(const TopoDS_Shape& shape,
                                      std::vector<int>& tri_face,
                                      double linear_deflection,
                                      double angular_deflection)
{
    tri_face.clear();
    BRepMesh_IncrementalMesh mesh(shape, linear_deflection, false, angular_deflection, true);

    int nbNodes = 0, nbTriangles = 0;
    for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(TopoDS::Face(exp.Current()), loc);
        if (!tri.IsNull()) {
            nbNodes += tri->NbNodes();
            nbTriangles += tri->NbTriangles();
        }
    }

    if (nbTriangles == 0 || nbNodes == 0)
        return TriangleMesh{};

    indexed_triangle_set raw;
    raw.vertices.reserve(nbNodes);
    raw.indices.reserve(nbTriangles);
    tri_face.reserve(nbTriangles);

    int faceIdx = -1;
    int nodeOff = 0;
    for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {
        ++faceIdx;

        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(TopoDS::Face(exp.Current()), loc);
        if (tri.IsNull()) continue;

        gp_Trsf trsf = loc.Transformation();
        for (int i = 1; i <= tri->NbNodes(); ++i) {
            gp_Pnt p = tri->Node(i);
            p.Transform(trsf);
            raw.vertices.emplace_back(Vec3f(p.X(), p.Y(), p.Z()));
        }

        TopAbs_Orientation orient = exp.Current().Orientation();
        int ids[3];
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
            Poly_Triangle t = tri->Triangle(i);
            t.Get(ids[0], ids[1], ids[2]);
            if (orient == TopAbs_REVERSED)
                std::swap(ids[1], ids[2]);

            raw.indices.emplace_back(nodeOff + ids[0] - 1,
                                     nodeOff + ids[1] - 1,
                                     nodeOff + ids[2] - 1);
            tri_face.push_back(faceIdx);
        }
        nodeOff += tri->NbNodes();
    }

    std::map<std::tuple<float, float, float>, int> vmap;
    indexed_triangle_set its;
    its.indices.reserve(raw.indices.size());
    its.vertices.reserve(raw.vertices.size() / 2);

    std::vector<int> kept_face;
    kept_face.reserve(tri_face.size());

    for (size_t ti = 0; ti < raw.indices.size(); ++ti) {
        const auto& tri = raw.indices[ti];
        stl_triangle_vertex_indices new_tri;
        for (int j = 0; j < 3; ++j) {
            const stl_vertex& v = raw.vertices[tri[j]];
            auto key = std::make_tuple(v.x(), v.y(), v.z());
            auto it = vmap.find(key);
            if (it == vmap.end()) {
                int new_id = static_cast<int>(its.vertices.size());
                vmap[key] = new_id;
                its.vertices.push_back(v);
                new_tri[j] = new_id;
            } else {
                new_tri[j] = it->second;
            }
        }
        // Drop triangles that welding collapsed to a repeated vertex. OCCT emits one at the
        // pole of every degenerate surface parameterization — a sphere patch at a filleted
        // corner has exactly one — and its v->v edge can never pair with a neighbour, so the
        // mesh reports an open edge per corner and the slicer declares the model non-manifold
        // and tells the user to repair it elsewhere. The triangle has zero area: removing it
        // changes no geometry, only the mesh's bookkeeping.
        if (new_tri[0] == new_tri[1] || new_tri[1] == new_tri[2] || new_tri[0] == new_tri[2])
            continue;
        its.indices.push_back(new_tri);
        kept_face.push_back(tri_face[ti]);
    }
    tri_face.swap(kept_face);   // tri_face stays index-aligned with its.indices

    return TriangleMesh(std::move(its));
}

std::vector<TopoDS_Wire> SketchEngine::entities_to_wires(const std::vector<SketchEntity>& entities,
                                                         const SketchPlane& plane)
{
    struct Item { const SketchEntity* e; size_t idx; };
    std::vector<Item> valid;
    valid.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        const SketchEntity& e = entities[i];
        if (e.construction) continue;
        if (e.type == SketchEntity::Type::Point) continue;
        valid.push_back({&e, i});
    }
    if (valid.empty()) return {};

    // Build an OCCT ellipse (gp_Elips) in the sketch plane from an Ellipse(Arc)
    // entity. Major-axis direction = plane-rotated (cos phi, sin phi). Enforces
    // a >= b (OCCT requirement); the GUI builder already guarantees this.
    auto make_elips = [&](const SketchEntity& c) -> gp_Elips {
        Vec3d  c3 = plane.to_world(c.center);
        gp_Pnt center(c3.x(), c3.y(), c3.z());
        gp_Dir n(plane.normal.x(), plane.normal.y(), plane.normal.z());
        Vec2d  maj2(std::cos(c.rotation), std::sin(c.rotation));
        Vec3d  x3 = plane.to_world(c.center + maj2) - c3;
        gp_Dir xdir(x3.x(), x3.y(), x3.z());
        double a = c.radius, b = c.rminor;
        if (a < b) std::swap(a, b);
        return gp_Elips(gp_Ax2(center, n, xdir), a, b);
    };

    // Clamped uniform B-spline (degree min(3, n-1)) through the control poles. The
    // knot construction is mirrored in DesignSketchTool's GUI sampler so the on-screen
    // curve matches the extruded geometry exactly.
    auto make_bspline = [&](const SketchEntity& c) -> Handle(Geom_BSplineCurve) {
        const int n = int(c.ctrl.size());
        const int p = n >= 4 ? 3 : (n >= 2 ? n - 1 : 0);
        if (p < 1) return Handle(Geom_BSplineCurve)();
        TColgp_Array1OfPnt poles(1, n);
        for (int i = 0; i < n; ++i) {
            Vec3d w = plane.to_world(c.ctrl[i]);
            poles.SetValue(i + 1, gp_Pnt(w.x(), w.y(), w.z()));
        }
        const int interior = n - p - 1;        // count of single interior knots
        const int nknots   = interior + 2;
        TColStd_Array1OfReal    knots(1, nknots);
        TColStd_Array1OfInteger mults(1, nknots);
        knots.SetValue(1, 0.0);                mults.SetValue(1, p + 1);
        for (int i = 1; i <= interior; ++i) { knots.SetValue(i + 1, double(i)); mults.SetValue(i + 1, 1); }
        knots.SetValue(nknots, double(interior + 1)); mults.SetValue(nknots, p + 1);
        return new Geom_BSplineCurve(poles, knots, mults, p);
    };

    auto is_chain = [](const SketchEntity& e) {
        return e.type == SketchEntity::Type::Line || e.type == SketchEntity::Type::Arc ||
               e.type == SketchEntity::Type::EllipseArc || e.type == SketchEntity::Type::BSpline;
    };

    // Endpoints of a chain entity in SKETCH coordinates (before to_world). False on a
    // degenerate (fewer than two control points) BSpline, which can never close a loop.
    auto endpoints = [&](const SketchEntity& e, Vec2d& a, Vec2d& b) -> bool {
        if (e.type == SketchEntity::Type::BSpline) {
            if (e.ctrl.size() < 2) return false;
            a = e.ctrl.front(); b = e.ctrl.back();
            return true;
        }
        a = e.p0; b = e.p1;
        return true;
    };

    const double EPS = 1e-6;
    auto same = [&](const Vec2d& p, const Vec2d& q) { return (p - q).norm() < EPS; };

    // Union-find over the valid index list: chain entities sharing an endpoint belong to one loop.
    std::vector<int> parent(valid.size());
    for (size_t i = 0; i < valid.size(); ++i) parent[i] = int(i);
    auto find = [&](int x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](int x, int y) {
        int rx = find(x), ry = find(y);
        if (rx != ry) parent[rx] = ry;
    };

    std::vector<size_t> chain_idx;
    chain_idx.reserve(valid.size());
    for (size_t i = 0; i < valid.size(); ++i)
        if (is_chain(*valid[i].e)) chain_idx.push_back(i);

    for (size_t a = 0; a < chain_idx.size(); ++a) {
        const size_t i = chain_idx[a];
        Vec2d i0, i1;
        if (!endpoints(*valid[i].e, i0, i1)) continue;
        for (size_t b = a + 1; b < chain_idx.size(); ++b) {
            const size_t j = chain_idx[b];
            Vec2d j0, j1;
            if (!endpoints(*valid[j].e, j0, j1)) continue;
            if (same(i0, j0) || same(i0, j1) || same(i1, j0) || same(i1, j1))
                unite(int(i), int(j));
        }
    }

    // Group chain entities by connected-component root.
    std::map<int, std::vector<size_t>> comps;
    for (size_t i = 0; i < valid.size(); ++i) {
        if (!is_chain(*valid[i].e)) continue;
        comps[find(int(i))].push_back(i);
    }

    // Loop descriptors: each Circle/Ellipse is its own loop; each chain component is a loop.
    struct Loop { size_t min_idx{0}; bool closed_single{false}; size_t member{0}; std::vector<size_t> members; };
    std::vector<Loop> loops;
    for (size_t i = 0; i < valid.size(); ++i) {
        const SketchEntity& e = *valid[i].e;
        if (e.type == SketchEntity::Type::Circle || e.type == SketchEntity::Type::Ellipse) {
            Loop l; l.min_idx = valid[i].idx; l.closed_single = true; l.member = i;
            loops.push_back(l);
        }
    }
    for (const auto& kv : comps) {
        Loop l; l.closed_single = false; l.members = kv.second;
        size_t mn = std::numeric_limits<size_t>::max();
        for (size_t m : kv.second) mn = std::min(mn, valid[m].idx);
        l.min_idx = mn;
        loops.push_back(l);
    }
    // Deterministic order: by the index of each loop's first entity.
    std::sort(loops.begin(), loops.end(), [](const Loop& x, const Loop& y) { return x.min_idx < y.min_idx; });

    // Build each loop. All-or-nothing: one failed loop poisons the whole result.
    std::vector<TopoDS_Wire> out;
    out.reserve(loops.size());
    for (const Loop& loop : loops) {
        BRepBuilderAPI_MakeWire wm;
        if (loop.closed_single) {
            const SketchEntity& c = *valid[loop.member].e;
            TopoDS_Edge e;
            if (c.type == SketchEntity::Type::Ellipse) {
                if (c.radius <= 1e-9 || c.rminor <= 1e-9) return {};
                e = BRepBuilderAPI_MakeEdge(make_elips(c)).Edge();
            } else {
                Vec3d  c3 = plane.to_world(c.center);
                gp_Pnt center(c3.x(), c3.y(), c3.z());
                gp_Dir n(plane.normal.x(), plane.normal.y(), plane.normal.z());
                gp_Circ circ(gp_Ax2(center, n), c.radius);
                e = BRepBuilderAPI_MakeEdge(circ).Edge();
            }
            wm.Add(e);
        } else {
            for (size_t m : loop.members) {
                const SketchEntity* e = valid[m].e;
                if (e->type == SketchEntity::Type::Line) {
                    Vec3d  p0 = plane.to_world(e->p0);
                    Vec3d  p1 = plane.to_world(e->p1);
                    gp_Pnt pa(p0.x(), p0.y(), p0.z());
                    gp_Pnt pb(p1.x(), p1.y(), p1.z());
                    wm.Add(BRepBuilderAPI_MakeEdge(pa, pb).Edge());
                } else if (e->type == SketchEntity::Type::EllipseArc) {
                    if (e->radius <= 1e-9 || e->rminor <= 1e-9) return {};
                    GC_MakeArcOfEllipse arc_maker(make_elips(*e), e->start_angle, e->end_angle, Standard_True);
                    if (!arc_maker.IsDone()) return {};
                    wm.Add(BRepBuilderAPI_MakeEdge(arc_maker.Value()).Edge());
                } else if (e->type == SketchEntity::Type::Arc) {
                    Vec3d  p0 = plane.to_world(e->p0);
                    Vec3d  p1 = plane.to_world(e->p1);
                    double mid_angle = (e->start_angle + e->end_angle) * 0.5;
                    Vec2d  mid_2d(e->center.x() + e->radius * std::cos(mid_angle),
                                  e->center.y() + e->radius * std::sin(mid_angle));
                    Vec3d  mid_3d = plane.to_world(mid_2d);
                    gp_Pnt pa(p0.x(), p0.y(), p0.z());
                    gp_Pnt pm(mid_3d.x(), mid_3d.y(), mid_3d.z());
                    gp_Pnt pb(p1.x(), p1.y(), p1.z());
                    GC_MakeArcOfCircle arc_maker(pa, pm, pb);
                    if (!arc_maker.IsDone()) return {};
                    Handle(Geom_TrimmedCurve) curve = arc_maker.Value();
                    wm.Add(BRepBuilderAPI_MakeEdge(curve).Edge());
                } else if (e->type == SketchEntity::Type::BSpline) {
                    Handle(Geom_BSplineCurve) crv = make_bspline(*e);
                    if (crv.IsNull()) return {};
                    wm.Add(BRepBuilderAPI_MakeEdge(crv).Edge());
                }
            }
        }
        wm.Build();
        if (!wm.IsDone()) return {};
        out.push_back(wm.Wire());
    }
    return out;
}

TopoDS_Wire SketchEngine::entities_to_wire(const std::vector<SketchEntity>& entities,
                                           const SketchPlane& plane)
{
    const std::vector<TopoDS_Wire> w = entities_to_wires(entities, plane);
    return w.size() == 1 ? w[0] : TopoDS_Wire{};
}

TopoDS_Face SketchEngine::wires_to_face(const std::vector<TopoDS_Wire>& wires,
                                        const SketchPlane& /*plane*/)
{
    if (wires.empty()) throw std::runtime_error("sketch has no closed loop");

    if (wires.size() == 1) {
        BRepBuilderAPI_MakeFace fm(wires[0]);
        if (!fm.IsDone()) throw std::runtime_error("sketch loop does not bound a face");
        return fm.Face();
    }

    // Two or more loops: build a face per wire and let the largest area be the outer
    // boundary; every other loop is a candidate hole inside it.
    std::vector<TopoDS_Face> faces;
    faces.reserve(wires.size());
    std::vector<double> areas;
    areas.reserve(wires.size());
    for (const TopoDS_Wire& w : wires) {
        BRepBuilderAPI_MakeFace fm(w);
        if (!fm.IsDone()) throw std::runtime_error("sketch loop does not bound a face");
        faces.push_back(fm.Face());
        GProp_GProps props;
        BRepGProp::SurfaceProperties(faces.back(), props);
        areas.push_back(props.Mass());
    }

    size_t outer = 0;
    for (size_t i = 1; i < areas.size(); ++i)
        if (areas[i] > areas[outer]) outer = i;

    // Note: NOT MakeFace(faces[outer], wires[outer]) — that constructor copies the outer face
    // (including its existing boundary wire) and then adds the wire again, doubling the outer
    // boundary. The wire-only constructor starts clean and the reversed holes follow.
    BRepBuilderAPI_MakeFace fm(wires[outer]);
    for (size_t i = 0; i < wires.size(); ++i) {
        if (i == outer) continue;
        // Containment is checked, not assumed: a vertex of the inner wire must lie strictly
        // inside the outer face. A loop outside the largest one is a second island, not a hole.
        gp_Pnt p;
        bool got = false;
        for (TopExp_Explorer ex(wires[i], TopAbs_VERTEX); ex.More(); ex.Next()) {
            p = BRep_Tool::Pnt(TopoDS::Vertex(ex.Current()));
            got = true;
            break;
        }
        if (!got) throw std::runtime_error("sketch loop does not bound a face");
        BRepClass_FaceClassifier fc(faces[outer], p, 1e-7);
        if (fc.State() != TopAbs_IN)
            throw std::runtime_error("sketch has two disjoint regions; put each in its own sketch");
        // A reversed wire tells OCCT this loop is a hole, not a second boundary.
        fm.Add(TopoDS::Wire(wires[i].Reversed()));
    }
    if (!fm.IsDone()) throw std::runtime_error("sketch loop does not bound a face");
    return fm.Face();
}

std::vector<SketchEntity> SketchEngine::mirror_entities(
    const std::vector<SketchEntity>& src, const Vec2d& a, const Vec2d& b)
{
    Vec2d dir = b - a;
    if (dir.norm() < 1e-12)
        return src;

    dir.normalize();

    auto reflect = [&](const Vec2d& p) -> Vec2d {
        Vec2d v = p - a;
        return a + (2.0 * v.dot(dir)) * dir - v;
    };

    std::vector<SketchEntity> out;
    out.reserve(src.size());

    for (const auto& e : src) {
        SketchEntity m = e;
        switch (e.type) {
        case SketchEntity::Type::Line:
            m.p0 = reflect(e.p0);
            m.p1 = reflect(e.p1);
            break;
        case SketchEntity::Type::Point:
            m.p0 = reflect(e.p0);
            break;
        case SketchEntity::Type::Circle:
            m.center = reflect(e.center);
            m.p0 = m.center;
            break;
        case SketchEntity::Type::Arc: {
            m.p0     = reflect(e.p0);
            m.p1     = reflect(e.p1);
            m.center = reflect(e.center);

            const Vec2d& c = m.center;
            m.start_angle = std::atan2(m.p0.y() - c.y(), m.p0.x() - c.x());
            double raw_end = std::atan2(m.p1.y() - c.y(), m.p1.x() - c.x());

            double s = e.end_angle - e.start_angle;

            double sweep = raw_end - m.start_angle;
            while (sweep <= -2.0 * M_PI) sweep += 2.0 * M_PI;
            while (sweep >=  2.0 * M_PI) sweep -= 2.0 * M_PI;

            if (s != 0.0 && sweep * s > 0.0) {
                if (sweep > 0.0)
                    sweep -= 2.0 * M_PI;
                else
                    sweep += 2.0 * M_PI;
            }

            m.end_angle = m.start_angle + sweep;
            m.radius    = e.radius;
            break;
        }
        case SketchEntity::Type::Ellipse:
        case SketchEntity::Type::EllipseArc: {
            m.center = reflect(e.center);
            // Reflect the major-axis direction; a/b unchanged.
            const Vec2d majdir(std::cos(e.rotation), std::sin(e.rotation));
            const Vec2d rdir = reflect(e.center + majdir) - m.center;
            m.rotation = std::atan2(rdir.y(), rdir.x());
            if (e.type == SketchEntity::Type::Ellipse) {
                m.p0 = m.center;
            } else {
                m.p0 = reflect(e.p0);
                m.p1 = reflect(e.p1);
                // Reflection reverses orientation: recompute parametric angles in
                // the reflected frame, original end -> new start (CCW sense kept).
                auto param = [&](const Vec2d& P) {
                    const Vec2d d  = P - m.center;
                    const double cu = std::cos(m.rotation), su = std::sin(m.rotation);
                    const double u =  d.x() * cu + d.y() * su;
                    const double v = -d.x() * su + d.y() * cu;
                    return std::atan2(v / std::max(e.rminor, 1e-9), u / std::max(e.radius, 1e-9));
                };
                m.start_angle = param(m.p1);
                m.end_angle   = param(m.p0);
            }
            break;
        }
        case SketchEntity::Type::BSpline:
            for (auto& cp : m.ctrl) cp = reflect(cp);
            m.p0 = reflect(e.p0);
            m.p1 = reflect(e.p1);
            break;
        }
        out.push_back(m);
    }

    return out;
}

std::vector<SketchEntity> SketchEngine::offset_entities(
    const std::vector<SketchEntity>& src, double d)
{
    std::vector<SketchEntity> out;

    for (const auto& e : src) {
        switch (e.type) {
        case SketchEntity::Type::Line: {
            Vec2d t = e.p1 - e.p0;
            if (t.norm() < 1e-12) continue;
            t.normalize();
            Vec2d n(-t.y(), t.x());
            SketchEntity o = e;
            o.p0 = e.p0 + d * n;
            o.p1 = e.p1 + d * n;
            out.push_back(o);
            break;
        }
        case SketchEntity::Type::Circle: {
            double r = e.radius + d;
            if (r <= 1e-9) continue;
            SketchEntity o = e;
            o.radius = r;
            o.p0     = o.center;
            out.push_back(o);
            break;
        }
        case SketchEntity::Type::Arc: {
            double r = e.radius + d;
            if (r <= 1e-9) continue;
            SketchEntity o = e;
            o.radius = r;
            o.p0     = e.center + r * Vec2d(std::cos(e.start_angle), std::sin(e.start_angle));
            o.p1     = e.center + r * Vec2d(std::cos(e.end_angle),   std::sin(e.end_angle));
            out.push_back(o);
            break;
        }
        case SketchEntity::Type::Point:
            continue;
        case SketchEntity::Type::Ellipse:
        case SketchEntity::Type::EllipseArc:
            // A true parallel offset of an ellipse is not an ellipse; skip in v1.
            continue;
        case SketchEntity::Type::BSpline:
            // Offset of a spline is not a same-degree spline; skip in v1.
            continue;
        }
    }

    return out;
}

std::vector<SketchEntity> SketchEngine::array_entities(
    const std::vector<SketchEntity>& src, int count,
    const Vec2d& step, double angle_step, const Vec2d& pivot)
{
    std::vector<SketchEntity> out;
    if (count < 2) return out;

    for (int i = 1; i < count; ++i) {
        const double ang = i * angle_step;
        const double ca = std::cos(ang), sa = std::sin(ang);
        const Vec2d  tr  = double(i) * step;
        // Rigid map: rotate about pivot by `ang`, then translate by `tr`.
        auto xf = [&](const Vec2d& p) -> Vec2d {
            const Vec2d d = p - pivot;
            return Vec2d(pivot.x() + ca * d.x() - sa * d.y(),
                         pivot.y() + sa * d.x() + ca * d.y()) + tr;
        };

        for (const auto& e : src) {
            SketchEntity m = e;   // carry construction flag, radii, etc.
            switch (e.type) {
            case SketchEntity::Type::Line:
                m.p0 = xf(e.p0); m.p1 = xf(e.p1);
                break;
            case SketchEntity::Type::Point:
                m.p0 = xf(e.p0);
                break;
            case SketchEntity::Type::Circle:
                m.center = xf(e.center); m.p0 = m.center;   // radius unchanged
                break;
            case SketchEntity::Type::Arc: {
                m.center      = xf(e.center);
                m.start_angle = e.start_angle + ang;
                m.end_angle   = e.end_angle   + ang;        // rigid: sweep preserved
                m.p0 = m.center + e.radius * Vec2d(std::cos(m.start_angle), std::sin(m.start_angle));
                m.p1 = m.center + e.radius * Vec2d(std::cos(m.end_angle),   std::sin(m.end_angle));
                break;
            }
            case SketchEntity::Type::Ellipse:
            case SketchEntity::Type::EllipseArc:
                m.center   = xf(e.center);
                m.rotation = e.rotation + ang;              // major axis rotates with the body
                if (e.type == SketchEntity::Type::Ellipse) {
                    m.p0 = m.center;
                } else {
                    m.p0 = xf(e.p0); m.p1 = xf(e.p1);
                    // Parametric angles are in the (rotated) body frame -> unchanged.
                }
                break;
            case SketchEntity::Type::BSpline:
                for (auto& cp : m.ctrl) cp = xf(cp);
                m.p0 = xf(e.p0); m.p1 = xf(e.p1);
                break;
            }
            out.push_back(m);
        }
    }
    return out;
}

std::vector<SketchEntity> SketchEngine::transform_entities(
    const std::vector<SketchEntity>& src,
    const Vec2d& move, double angle, double scale, const Vec2d& pivot)
{
    std::vector<SketchEntity> out;
    out.reserve(src.size());
    const double ca = std::cos(angle), sa = std::sin(angle);
    const double rs = std::abs(scale);   // radii are unsigned magnitudes
    // Affine map: translate pivot to origin, scale, rotate, then translate by `move`.
    auto xf = [&](const Vec2d& p) -> Vec2d {
        const Vec2d d = scale * (p - pivot);
        return Vec2d(pivot.x() + ca * d.x() - sa * d.y(),
                     pivot.y() + sa * d.x() + ca * d.y()) + move;
    };

    for (const auto& e : src) {
        SketchEntity m = e;   // carry construction flag, etc.
        switch (e.type) {
        case SketchEntity::Type::Line:
            m.p0 = xf(e.p0); m.p1 = xf(e.p1);
            break;
        case SketchEntity::Type::Point:
            m.p0 = xf(e.p0);
            break;
        case SketchEntity::Type::Circle:
            m.center = xf(e.center); m.radius = e.radius * rs; m.p0 = m.center;
            break;
        case SketchEntity::Type::Arc: {
            m.center      = xf(e.center);
            m.radius      = e.radius * rs;
            m.start_angle = e.start_angle + angle;
            m.end_angle   = e.end_angle   + angle;   // rigid sweep, shifted by rotation
            m.p0 = m.center + m.radius * Vec2d(std::cos(m.start_angle), std::sin(m.start_angle));
            m.p1 = m.center + m.radius * Vec2d(std::cos(m.end_angle),   std::sin(m.end_angle));
            break;
        }
        case SketchEntity::Type::Ellipse:
        case SketchEntity::Type::EllipseArc:
            m.center   = xf(e.center);
            m.radius   = e.radius * rs;
            m.rminor   = e.rminor * rs;
            m.rotation = e.rotation + angle;          // major axis rotates with the body
            if (e.type == SketchEntity::Type::Ellipse) {
                m.p0 = m.center;
            } else {
                m.p0 = xf(e.p0); m.p1 = xf(e.p1);
                // Parametric angles live in the (rotated) body frame -> unchanged.
            }
            break;
        case SketchEntity::Type::BSpline:
            for (auto& cp : m.ctrl) cp = xf(cp);
            m.p0 = xf(e.p0); m.p1 = xf(e.p1);
            break;
        }
        out.push_back(m);
    }
    return out;
}

bool SketchEngine::fillet_lines(const SketchEntity& a, const SketchEntity& b, double r,
                                SketchEntity& a_out, SketchEntity& b_out, SketchEntity& arc_out)
{
    if (a.type != SketchEntity::Type::Line || b.type != SketchEntity::Type::Line || r <= 1e-9)
        return false;

    Vec2d da = a.p1 - a.p0;
    Vec2d db = b.p1 - b.p0;
    double denom = da.x() * db.y() - da.y() * db.x();
    if (std::abs(denom) < 1e-12)
        return false;

    Vec2d diff = b.p0 - a.p0;
    double s = (diff.x() * db.y() - diff.y() * db.x()) / denom;
    Vec2d C = a.p0 + s * da;

    Vec2d ua;
    int   a_near_idx;
    {
        double d0 = (a.p0 - C).norm();
        double d1 = (a.p1 - C).norm();
        if (d0 <= d1) {
            a_near_idx = 0;
            ua = a.p1 - C;
        } else {
            a_near_idx = 1;
            ua = a.p0 - C;
        }
    }
    if (ua.norm() < 1e-12) return false;
    ua.normalize();

    Vec2d ub;
    int   b_near_idx;
    {
        double d0 = (b.p0 - C).norm();
        double d1 = (b.p1 - C).norm();
        if (d0 <= d1) {
            b_near_idx = 0;
            ub = b.p1 - C;
        } else {
            b_near_idx = 1;
            ub = b.p0 - C;
        }
    }
    if (ub.norm() < 1e-12) return false;
    ub.normalize();

    double cosT = ua.dot(ub);
    cosT = std::max(-1.0, std::min(1.0, cosT));
    double theta = std::acos(cosT);
    if (theta < 1e-6 || theta > M_PI - 1e-6)
        return false;

    double t = r / std::tan(theta / 2.0);
    {
        Vec2d a_far = (a_near_idx == 0) ? a.p1 : a.p0;
        Vec2d b_far = (b_near_idx == 0) ? b.p1 : b.p0;
        if (t > (a_far - C).norm() || t > (b_far - C).norm())
            return false;
    }

    Vec2d Ta = C + t * ua;
    Vec2d Tb = C + t * ub;

    Vec2d bis = ua + ub;
    if (bis.norm() < 1e-12) return false;
    bis.normalize();

    double dCO = r / std::sin(theta / 2.0);
    Vec2d O = C + dCO * bis;

    a_out = a;
    b_out = b;
    if (a_near_idx == 0)
        a_out.p0 = Ta;
    else
        a_out.p1 = Ta;

    if (b_near_idx == 0)
        b_out.p0 = Tb;
    else
        b_out.p1 = Tb;

    arc_out = SketchEntity{};
    arc_out.type        = SketchEntity::Type::Arc;
    arc_out.center      = O;
    arc_out.radius      = r;
    arc_out.p0          = Ta;
    arc_out.p1          = Tb;
    arc_out.start_angle = std::atan2(Ta.y() - O.y(), Ta.x() - O.x());

    double sb = std::atan2(Tb.y() - O.y(), Tb.x() - O.x());
    double sweep = sb - arc_out.start_angle;
    while (sweep <= -M_PI) sweep += 2.0 * M_PI;
    while (sweep >   M_PI) sweep -= 2.0 * M_PI;
    arc_out.end_angle = arc_out.start_angle + sweep;

    return true;
}

bool SketchEngine::chamfer_lines(const SketchEntity& a, const SketchEntity& b, double d,
                                 SketchEntity& a_out, SketchEntity& b_out, SketchEntity& seg_out)
{
    if (a.type != SketchEntity::Type::Line || b.type != SketchEntity::Type::Line || d <= 1e-9)
        return false;

    Vec2d da = a.p1 - a.p0;
    Vec2d db = b.p1 - b.p0;
    double denom = da.x() * db.y() - da.y() * db.x();
    if (std::abs(denom) < 1e-12)
        return false;   // parallel: no corner to chamfer

    // Shared corner = line/line intersection.
    Vec2d diff = b.p0 - a.p0;
    double s = (diff.x() * db.y() - diff.y() * db.x()) / denom;
    Vec2d C = a.p0 + s * da;

    // For each line, unit vector pointing from the corner toward its far endpoint
    // (the endpoint that is kept); the near endpoint is the one trimmed back.
    auto pick = [&](const SketchEntity& ln, int& near_idx, Vec2d& u) -> bool {
        double d0 = (ln.p0 - C).norm();
        double d1 = (ln.p1 - C).norm();
        if (d0 <= d1) { near_idx = 0; u = ln.p1 - C; }
        else          { near_idx = 1; u = ln.p0 - C; }
        if (u.norm() < 1e-12) return false;
        u.normalize();
        return true;
    };
    int a_near_idx, b_near_idx;
    Vec2d ua, ub;
    if (!pick(a, a_near_idx, ua)) return false;
    if (!pick(b, b_near_idx, ub)) return false;

    // Reject collinear (no real corner) and ensure the setback fits both lines.
    double cosT = std::max(-1.0, std::min(1.0, ua.dot(ub)));
    if (cosT > 1.0 - 1e-9 || cosT < -1.0 + 1e-9)
        return false;
    {
        Vec2d a_far = (a_near_idx == 0) ? a.p1 : a.p0;
        Vec2d b_far = (b_near_idx == 0) ? b.p1 : b.p0;
        if (d > (a_far - C).norm() || d > (b_far - C).norm())
            return false;
    }

    Vec2d Ta = C + d * ua;
    Vec2d Tb = C + d * ub;

    a_out = a;
    b_out = b;
    if (a_near_idx == 0) a_out.p0 = Ta; else a_out.p1 = Ta;
    if (b_near_idx == 0) b_out.p0 = Tb; else b_out.p1 = Tb;

    seg_out = SketchEntity{};
    seg_out.type = SketchEntity::Type::Line;
    seg_out.p0   = Ta;
    seg_out.p1   = Tb;
    return true;
}

static std::vector<double> line_entity_hits(const Vec2d& a0, const Vec2d& adir,
                                            const SketchEntity& other)
{
    std::vector<double> hits;
    double La2 = adir.dot(adir);
    if (La2 < 1e-18) return hits;

    switch (other.type) {
    case SketchEntity::Type::Line: {
        Vec2d bdir = other.p1 - other.p0;
        double bxa = bdir.x() * adir.y() - bdir.y() * adir.x();
        if (std::abs(bxa) < 1e-12) return hits;

        Vec2d w  = a0 - other.p0;
        double u = (w.x() * adir.y() - w.y() * adir.x()) / bxa;
        if (u < -1e-9 || u > 1.0 + 1e-9) return hits;

        double axb = adir.x() * bdir.y() - adir.y() * bdir.x();
        Vec2d  w2  = other.p0 - a0;
        double t   = (w2.x() * bdir.y() - w2.y() * bdir.x()) / axb;
        hits.push_back(t);
        break;
    }
    case SketchEntity::Type::Circle:
    case SketchEntity::Type::Arc: {
        double R = other.radius;
        Vec2d f = a0 - other.center;
        double A  = La2;
        double B  = 2.0 * adir.dot(f);
        double Cc = f.dot(f) - R * R;
        double disc = B * B - 4.0 * A * Cc;
        if (disc < -1e-12) return hits;
        if (disc < 0.0) disc = 0.0;
        double sq = std::sqrt(disc);
        double t1 = (-B - sq) / (2.0 * A);
        double t2 = (-B + sq) / (2.0 * A);

        auto angle_in_sweep = [&](const Vec2d& P) -> bool {
            double phi   = std::atan2(P.y() - other.center.y(), P.x() - other.center.x());
            double sweep = other.end_angle - other.start_angle;
            double delta = phi - other.start_angle;
            if (sweep >= 0.0) {
                while (delta < -1e-9) delta += 2.0 * M_PI;
                while (delta > 2.0 * M_PI) delta -= 2.0 * M_PI;
                return delta <= sweep + 1e-9;
            } else {
                while (delta > 1e-9) delta -= 2.0 * M_PI;
                while (delta < -2.0 * M_PI) delta += 2.0 * M_PI;
                return delta >= sweep - 1e-9;
            }
        };

        auto check = [&](double t) {
            Vec2d P = a0 + t * adir;
            if (other.type == SketchEntity::Type::Circle || angle_in_sweep(P))
                hits.push_back(t);
        };

        check(t1);
        if (disc > 1e-12)
            check(t2);
        break;
    }
    default:
        break;
    }

    return hits;
}

// Angles (atan2, radians) where `other` crosses the circle of radius R about C.
// For Arc/Circle cutters the crossing point must lie within the cutter's own
// sweep (a full circle always qualifies). Powers arc/circle-subject trim/extend,
// where the subject is parametrized by angle rather than by a line ray param.
static std::vector<double> circle_cross_angles(const Vec2d& C, double R,
                                               const SketchEntity& other)
{
    std::vector<double> out;
    if (R < 1e-12) return out;

    auto on_other = [&](const Vec2d& P) -> bool {
        switch (other.type) {
        case SketchEntity::Type::Line: {
            Vec2d d = other.p1 - other.p0;
            double L2 = d.dot(d);
            if (L2 < 1e-18) return false;
            double u = (P - other.p0).dot(d) / L2;
            return u > -1e-9 && u < 1.0 + 1e-9;
        }
        case SketchEntity::Type::Circle:
            return true;
        case SketchEntity::Type::Arc: {
            double phi   = std::atan2(P.y() - other.center.y(), P.x() - other.center.x());
            double sweep = other.end_angle - other.start_angle;
            double delta = phi - other.start_angle;
            if (sweep >= 0.0) {
                while (delta < -1e-9) delta += 2.0 * M_PI;
                while (delta > 2.0 * M_PI) delta -= 2.0 * M_PI;
                return delta <= sweep + 1e-9;
            } else {
                while (delta > 1e-9) delta -= 2.0 * M_PI;
                while (delta < -2.0 * M_PI) delta += 2.0 * M_PI;
                return delta >= sweep - 1e-9;
            }
        }
        default:
            return false;
        }
    };
    auto add = [&](const Vec2d& P) {
        out.push_back(std::atan2(P.y() - C.y(), P.x() - C.x()));
    };

    switch (other.type) {
    case SketchEntity::Type::Line: {
        Vec2d a0 = other.p0, adir = other.p1 - other.p0;
        double A = adir.dot(adir);
        if (A < 1e-18) break;
        Vec2d f = a0 - C;
        double B  = 2.0 * adir.dot(f);
        double Cc = f.dot(f) - R * R;
        double disc = B * B - 4.0 * A * Cc;
        if (disc < 0.0) break;
        double sq = std::sqrt(disc);
        Vec2d P1 = a0 + ((-B - sq) / (2.0 * A)) * adir;
        if (on_other(P1)) add(P1);
        if (disc > 1e-12) {
            Vec2d P2 = a0 + ((-B + sq) / (2.0 * A)) * adir;
            if (on_other(P2)) add(P2);
        }
        break;
    }
    case SketchEntity::Type::Circle:
    case SketchEntity::Type::Arc: {
        Vec2d  C2 = other.center;
        double R2 = other.radius;
        Vec2d  d  = C2 - C;
        double dd = d.norm();
        if (dd < 1e-12) break;                          // concentric
        if (dd > R + R2 + 1e-9) break;                  // too far apart
        if (dd < std::abs(R - R2) - 1e-9) break;        // one circle inside the other
        double a  = (R * R - R2 * R2 + dd * dd) / (2.0 * dd);
        double h2 = R * R - a * a;
        if (h2 < 0.0) h2 = 0.0;
        double h   = std::sqrt(h2);
        Vec2d  mid = C + (a / dd) * d;
        Vec2d  perp(-d.y() / dd, d.x() / dd);
        Vec2d  P1 = mid + h * perp;
        if (on_other(P1)) add(P1);
        if (h > 1e-12) {
            Vec2d P2 = mid - h * perp;
            if (on_other(P2)) add(P2);
        }
        break;
    }
    default:
        break;
    }
    return out;
}

// Wrap x into [0, 2pi).
static double wrap_2pi(double x)
{
    while (x < 0.0)         x += 2.0 * M_PI;
    while (x >= 2.0 * M_PI) x -= 2.0 * M_PI;
    return x;
}

bool SketchEngine::trim_entity(SketchEntity& e, const std::vector<SketchEntity>& others,
                               const Vec2d& pick)
{
    // Arc subject: parametrize by sweep fraction u in [0,1]; cut on the picked side.
    if (e.type == SketchEntity::Type::Arc) {
        double sweep = e.end_angle - e.start_angle;
        if (std::abs(sweep) < 1e-12) return false;
        double phi_pick = std::atan2(pick.y() - e.center.y(), pick.x() - e.center.x());
        double u_pick = (phi_pick - e.start_angle) / sweep;
        // Bring the pick onto the arc's [0,1] domain.
        while (u_pick < -1e-9) u_pick += (2.0 * M_PI) / std::abs(sweep);
        u_pick = std::max(0.0, std::min(1.0, u_pick));

        std::vector<double> cuts;
        for (const auto& other : others) {
            for (double phi : circle_cross_angles(e.center, e.radius, other)) {
                double u = (phi - e.start_angle) / sweep;
                while (u < -1e-9) u += (2.0 * M_PI) / std::abs(sweep);
                if (u > 1e-9 && u < 1.0 - 1e-9) cuts.push_back(u);
            }
        }
        if (cuts.empty()) return false;

        if (u_pick <= 0.5) {
            double uc = std::numeric_limits<double>::max();
            for (double u : cuts) if (u > u_pick + 1e-9 && u < uc) uc = u;
            if (uc == std::numeric_limits<double>::max()) return false;
            e.start_angle = e.start_angle + uc * sweep;   // drop [0, uc)
        } else {
            double uc = -std::numeric_limits<double>::max();
            for (double u : cuts) if (u < u_pick - 1e-9 && u > uc) uc = u;
            if (uc == -std::numeric_limits<double>::max()) return false;
            e.end_angle = e.start_angle + uc * sweep;      // drop (uc, 1]
        }
        return true;
    }

    // Circle subject: trimming opens it into an Arc that excludes the picked gap.
    if (e.type == SketchEntity::Type::Circle) {
        std::vector<double> ang;
        for (const auto& other : others)
            for (double phi : circle_cross_angles(e.center, e.radius, other))
                ang.push_back(wrap_2pi(phi));
        std::sort(ang.begin(), ang.end());
        ang.erase(std::unique(ang.begin(), ang.end(),
                              [](double a, double b){ return std::abs(a - b) < 1e-7; }),
                  ang.end());
        if (ang.size() < 2) return false;

        double pk = wrap_2pi(std::atan2(pick.y() - e.center.y(), pick.x() - e.center.x()));
        const int n = int(ang.size());
        int idx = -1;
        for (int i = 0; i < n; ++i) {
            double lo = ang[i];
            double hi = (i + 1 < n) ? ang[i + 1] : ang[0] + 2.0 * M_PI;
            double p  = (pk < lo - 1e-12) ? pk + 2.0 * M_PI : pk;
            if (p >= lo - 1e-12 && p < hi + 1e-12) { idx = i; break; }
        }
        if (idx < 0) return false;
        double lo = ang[idx];
        double hi = (idx + 1 < n) ? ang[idx + 1] : ang[0] + 2.0 * M_PI;
        // Keep the complement of the (lo,hi) gap: sweep ccw from hi back round to lo.
        e.type        = SketchEntity::Type::Arc;
        e.start_angle = hi;
        e.end_angle   = lo + 2.0 * M_PI;
        return true;
    }

    if (e.type != SketchEntity::Type::Line) return false;

    Vec2d adir = e.p1 - e.p0;
    double La2 = adir.dot(adir);
    if (La2 < 1e-18) return false;

    double t_pick = (pick - e.p0).dot(adir) / La2;
    t_pick = std::max(0.0, std::min(1.0, t_pick));

    std::vector<double> cuts;
    for (const auto& other : others) {
        auto h = line_entity_hits(e.p0, adir, other);
        for (double t : h) {
            if (t > 1e-9 && t < 1.0 - 1e-9)
                cuts.push_back(t);
        }
    }
    if (cuts.empty()) return false;

    if (t_pick <= 0.5) {
        double tc = std::numeric_limits<double>::max();
        for (double t : cuts) {
            if (t > t_pick + 1e-9 && t < tc)
                tc = t;
        }
        if (tc == std::numeric_limits<double>::max()) return false;
        e.p0 = e.p0 + tc * adir;
    } else {
        double tc = -std::numeric_limits<double>::max();
        for (double t : cuts) {
            if (t < t_pick - 1e-9 && t > tc)
                tc = t;
        }
        if (tc == -std::numeric_limits<double>::max()) return false;
        e.p1 = e.p0 + tc * adir;
    }

    return true;
}

bool SketchEngine::extend_entity(SketchEntity& e, const std::vector<SketchEntity>& others,
                                 const Vec2d& pick)
{
    // Arc subject: grow the sweep toward the picked end up to the nearest crossing,
    // capped at a full turn so the arc never self-overlaps. (A Circle is already
    // closed — nothing to extend.)
    if (e.type == SketchEntity::Type::Arc) {
        double sweep = e.end_angle - e.start_angle;
        double mag   = std::abs(sweep);
        if (mag < 1e-12) return false;
        double sgn   = (sweep >= 0.0) ? 1.0 : -1.0;
        double room  = 2.0 * M_PI - mag;                 // max extra sweep before a full turn
        if (room <= 1e-9) return false;

        double phi_pick = std::atan2(pick.y() - e.center.y(), pick.x() - e.center.x());
        double up = wrap_2pi(sgn * (phi_pick - e.start_angle)) / mag;  // pick fraction on arc
        const bool extend_end = (up > 0.5);

        double best = std::numeric_limits<double>::max();
        for (const auto& other : others) {
            for (double phi : circle_cross_angles(e.center, e.radius, other)) {
                double adv = extend_end ? wrap_2pi(sgn * (phi - e.end_angle))
                                        : wrap_2pi(sgn * (e.start_angle - phi));
                if (adv > 1e-9 && adv <= room + 1e-9 && adv < best) best = adv;
            }
        }
        if (best == std::numeric_limits<double>::max()) return false;
        if (extend_end) e.end_angle   += sgn * best;
        else            e.start_angle -= sgn * best;
        return true;
    }

    if (e.type != SketchEntity::Type::Line) return false;

    Vec2d adir = e.p1 - e.p0;
    double La2 = adir.dot(adir);
    if (La2 < 1e-18) return false;

    double t_pick = (pick - e.p0).dot(adir) / La2;

    std::vector<double> hits;
    for (const auto& other : others) {
        auto h = line_entity_hits(e.p0, adir, other);
        hits.insert(hits.end(), h.begin(), h.end());
    }
    if (hits.empty()) return false;

    if (t_pick > 0.5) {
        double tc = std::numeric_limits<double>::max();
        for (double t : hits) {
            if (t > 1.0 + 1e-9 && t < tc)
                tc = t;
        }
        if (tc == std::numeric_limits<double>::max()) return false;
        e.p1 = e.p0 + tc * adir;
    } else {
        double tc = -std::numeric_limits<double>::max();
        for (double t : hits) {
            if (t < -1e-9 && t > tc)
                tc = t;
        }
        if (tc == -std::numeric_limits<double>::max()) return false;
        e.p0 = e.p0 + tc * adir;
    }

    return true;
}

// Bridge: cubic Bézier with G1 continuity at both ends.
// Poles = {Pa, Pa + Ta*d/3, Pb - Tb*d/3, Pb}, where d = |Pb - Pa|.
SketchEntity SketchEngine::make_bridge(const SketchEntity& a, int a_end,
                                       const SketchEntity& b, int b_end)
{
    auto endpoint = [](const SketchEntity& e, int end) -> Vec2d {
        return end == 0 ? e.p0 : e.p1;
    };

    auto tangent = [](const SketchEntity& e, int end, const Vec2d& fallback_dir) -> Vec2d {
        switch (e.type) {
        case SketchEntity::Type::Line: {
            Vec2d dir = end == 1 ? e.p1 - e.p0 : e.p0 - e.p1;
            double len = dir.norm();
            if (len < 1e-12) return fallback_dir;
            return dir / len;
        }
        case SketchEntity::Type::Arc: {
            double theta = end == 1 ? e.end_angle : e.start_angle;
            // Tangent to the circle at angle theta, CCW: (-sin θ, cos θ).
            // At end=1 (end_angle), the outward direction is along the sweep direction.
            // At end=0 (start_angle), outward is opposite the sweep direction.
            double sweep = e.end_angle - e.start_angle;
            int sign = end == 1 ? (sweep >= 0 ? 1 : -1) : (sweep >= 0 ? -1 : 1);
            Vec2d t(-std::sin(theta), std::cos(theta));
            return t * double(sign);
        }
        default:
            // ponytail: straight-ish bridge for unsupported entity types.
            return fallback_dir;
        }
    };

    const Vec2d Pa = endpoint(a, a_end);
    const Vec2d Pb = endpoint(b, b_end);
    const double d = (Pb - Pa).norm();

    // Fallback tangent direction: point toward the other endpoint.
    Vec2d fallback = d < 1e-9 ? Vec2d(1, 0) : (Pb - Pa) / d;
    Vec2d Ta = tangent(a, a_end, fallback);
    Vec2d Tb = tangent(b, b_end, fallback * -1.0);

    const double k = std::max(d, 1e-9) / 3.0;

    SketchEntity e;
    e.type         = SketchEntity::Type::BSpline;
    e.construction = false;
    e.ctrl = { Pa, Pa + Ta * k, Pb - Tb * k, Pb };
    e.p0 = e.ctrl.front();
    e.p1 = e.ctrl.back();
    return e;
}

} // namespace Slic3r
