#include "CadShape.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <NCollection_IndexedMap.hxx>
#include <Precision.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopAbs_State.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace {

CadPoint point(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }
CadPoint vector(const gp_Vec& v) { return {v.X(), v.Y(), v.Z()}; }

TopAbs_ShapeEnum occt_type(CadShapeType type)
{
    switch (type) {
    case CadShapeType::Compound: return TopAbs_COMPOUND;
    case CadShapeType::CompSolid: return TopAbs_COMPSOLID;
    case CadShapeType::Solid: return TopAbs_SOLID;
    case CadShapeType::Shell: return TopAbs_SHELL;
    case CadShapeType::Face: return TopAbs_FACE;
    case CadShapeType::Wire: return TopAbs_WIRE;
    case CadShapeType::Edge: return TopAbs_EDGE;
    case CadShapeType::Vertex: return TopAbs_VERTEX;
    }
    throw std::invalid_argument("Unknown CAD shape type");
}

double bound(double value)
{
    return Precision::IsInfinite(value) ? std::copysign(std::numeric_limits<double>::infinity(), value) : value;
}

void finite_parameter(double value)
{
    if (!std::isfinite(value) || Precision::IsInfinite(value))
        throw std::invalid_argument("CAD evaluation requires finite parameters");
}

} // namespace

struct CadShape::Context { std::recursive_mutex mutex; };

CadShape::CadShape(std::shared_ptr<Context> context, TopoDS_Shape shape)
    : m_context(std::move(context)), m_shape(std::move(shape)) {}

std::shared_ptr<CadShape> CadShape::copy_of(const TopoDS_Shape& shape)
{
    if (shape.IsNull())
        throw std::invalid_argument("Cannot snapshot a null CAD shape");
    BRepBuilderAPI_Copy copy(shape, true, false); // copy geometry, omit display triangulations
    return std::shared_ptr<CadShape>(new CadShape(std::make_shared<Context>(), copy.Shape()));
}

std::shared_ptr<CadShape> CadShape::view(const TopoDS_Shape& shape) const
{
    return std::shared_ptr<CadShape>(new CadShape(m_context, shape));
}

CadShapeType CadShape::type() const
{
    switch (m_shape.ShapeType()) {
    case TopAbs_COMPOUND: return CadShapeType::Compound;
    case TopAbs_COMPSOLID: return CadShapeType::CompSolid;
    case TopAbs_SOLID: return CadShapeType::Solid;
    case TopAbs_SHELL: return CadShapeType::Shell;
    case TopAbs_FACE: return CadShapeType::Face;
    case TopAbs_WIRE: return CadShapeType::Wire;
    case TopAbs_EDGE: return CadShapeType::Edge;
    case TopAbs_VERTEX: return CadShapeType::Vertex;
    default: throw std::runtime_error("Unsupported OCCT topology type");
    }
}

void CadShape::require(CadShapeType expected) const
{
    if (type() != expected)
        throw std::invalid_argument("Operation requires a different CAD shape type");
}

std::string CadShape::orientation() const
{
    switch (m_shape.Orientation()) {
    case TopAbs_FORWARD: return "forward";
    case TopAbs_REVERSED: return "reversed";
    case TopAbs_INTERNAL: return "internal";
    case TopAbs_EXTERNAL: return "external";
    }
    throw std::runtime_error("Unknown OCCT orientation");
}

std::vector<std::shared_ptr<CadShape>> CadShape::children() const
{
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    std::vector<std::shared_ptr<CadShape>> result;
    for (TopoDS_Iterator it(m_shape); it.More(); it.Next())
        result.push_back(view(it.Value()));
    return result;
}

std::vector<std::shared_ptr<CadShape>> CadShape::subshapes(CadShapeType type) const
{
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> shapes;
    TopExp::MapShapes(m_shape, occt_type(type), shapes);
    std::vector<std::shared_ptr<CadShape>> result;
    for (int i = 1; i <= shapes.Extent(); ++i)
        result.push_back(view(shapes(i)));
    return result;
}

CadPoint CadShape::vertex_point() const
{
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    require(CadShapeType::Vertex);
    return point(BRep_Tool::Pnt(TopoDS::Vertex(m_shape)));
}

bool CadShape::is_degenerate() const
{
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    require(CadShapeType::Edge);
    return BRep_Tool::Degenerated(TopoDS::Edge(m_shape));
}

std::string CadShape::curve_type() const
{
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    require(CadShapeType::Edge);
    if (is_degenerate()) return "degenerate";
    switch (BRepAdaptor_Curve(TopoDS::Edge(m_shape)).GetType()) {
    case GeomAbs_Line: return "line";
    case GeomAbs_Circle: return "circle";
    case GeomAbs_Ellipse: return "ellipse";
    case GeomAbs_Hyperbola: return "hyperbola";
    case GeomAbs_Parabola: return "parabola";
    case GeomAbs_BezierCurve: return "bezier";
    case GeomAbs_BSplineCurve: return "bspline";
    case GeomAbs_OffsetCurve: return "offset";
    case GeomAbs_OtherCurve: return "other";
    }
    return "other";
}

std::array<double, 2> CadShape::parameter_range() const
{
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    require(CadShapeType::Edge);
    if (is_degenerate()) throw std::invalid_argument("Degenerate edge has no regular 3D curve");
    BRepAdaptor_Curve curve(TopoDS::Edge(m_shape));
    return {bound(curve.FirstParameter()), bound(curve.LastParameter())};
}

CadCurveSample CadShape::evaluate_curve(double parameter) const
{
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    const auto range = parameter_range();
    finite_parameter(parameter);
    if (parameter < range[0] || parameter > range[1])
        throw std::invalid_argument("Parameter is outside the edge range");
    BRepAdaptor_Curve curve(TopoDS::Edge(m_shape));
    gp_Pnt p;
    gp_Vec derivative;
    curve.D1(parameter, p, derivative);
    CadCurveSample result{point(p), vector(derivative), std::nullopt};
    if (derivative.SquareMagnitude() > 1e-24) {
        derivative.Normalize();
        if (m_shape.Orientation() == TopAbs_REVERSED) derivative.Reverse();
        result.tangent = vector(derivative);
    }
    return result;
}

std::vector<CadPoint> CadShape::sample_curve(int count) const
{
    if (count < 2 || count > 100000)
        throw std::invalid_argument("Curve sample count must be between 2 and 100000");
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    auto range = parameter_range();
    finite_parameter(range[0]);
    finite_parameter(range[1]);
    if (m_shape.Orientation() == TopAbs_REVERSED) std::swap(range[0], range[1]);
    BRepAdaptor_Curve curve(TopoDS::Edge(m_shape));
    std::vector<CadPoint> result;
    result.reserve(count);
    for (int i = 0; i < count; ++i)
        result.push_back(point(curve.Value(range[0] + (range[1] - range[0]) * double(i) / double(count - 1))));
    return result;
}

std::string CadShape::surface_type() const
{
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    require(CadShapeType::Face);
    switch (BRepAdaptor_Surface(TopoDS::Face(m_shape)).GetType()) {
    case GeomAbs_Plane: return "plane";
    case GeomAbs_Cylinder: return "cylinder";
    case GeomAbs_Cone: return "cone";
    case GeomAbs_Sphere: return "sphere";
    case GeomAbs_Torus: return "torus";
    case GeomAbs_BezierSurface: return "bezier";
    case GeomAbs_BSplineSurface: return "bspline";
    case GeomAbs_SurfaceOfRevolution: return "revolution";
    case GeomAbs_SurfaceOfExtrusion: return "extrusion";
    case GeomAbs_OffsetSurface: return "offset";
    case GeomAbs_OtherSurface: return "other";
    }
    return "other";
}

std::array<double, 4> CadShape::uv_bounds() const
{
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    require(CadShapeType::Face);
    double u0, u1, v0, v1;
    BRepTools::UVBounds(TopoDS::Face(m_shape), u0, u1, v0, v1);
    return {bound(u0), bound(u1), bound(v0), bound(v1)};
}

std::array<bool, 2> CadShape::periodic() const
{
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    require(CadShapeType::Face);
    BRepAdaptor_Surface surface(TopoDS::Face(m_shape));
    return {surface.IsUPeriodic(), surface.IsVPeriodic()};
}

CadSurfaceSample CadShape::evaluate_surface(double u, double v) const
{
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    require(CadShapeType::Face);
    finite_parameter(u);
    finite_parameter(v);
    const auto bounds = uv_bounds();
    if (u < bounds[0] || u > bounds[1] || v < bounds[2] || v > bounds[3])
        throw std::invalid_argument("Parameters are outside the face's UV bounds");
    const TopoDS_Face face = TopoDS::Face(m_shape);
    BRepAdaptor_Surface surface(face);
    gp_Pnt p;
    gp_Vec du, dv;
    surface.D1(u, v, p, du, dv);
    BRepClass_FaceClassifier classifier(face, gp_Pnt2d(u, v), Precision::Confusion());
    CadSurfaceSample result{point(p), vector(du), vector(dv), std::nullopt,
                            classifier.State() == TopAbs_IN || classifier.State() == TopAbs_ON};
    gp_Vec normal = du.Crossed(dv);
    if (normal.SquareMagnitude() > 1e-24 && du.SquareMagnitude() > 1e-24) {
        normal.Normalize();
        if (face.Orientation() == TopAbs_REVERSED) normal.Reverse();
        du.Normalize();
        result.frame = CadFrame{point(p), vector(du), vector(normal.Crossed(du)), vector(normal)};
    }
    return result;
}

std::optional<CadFrame> CadShape::local_frame() const
{
    const auto bounds = uv_bounds();
    for (double b : bounds)
        if (!std::isfinite(b)) return std::nullopt;
    return evaluate_surface(0.5 * (bounds[0] + bounds[1]), 0.5 * (bounds[2] + bounds[3])).frame;
}

double CadShape::area() const
{
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    require(CadShapeType::Face);
    GProp_GProps props;
    BRepGProp::SurfaceProperties(m_shape, props);
    return props.Mass();
}

CadPoint CadShape::face_centroid() const
{
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    require(CadShapeType::Face);
    GProp_GProps props;
    BRepGProp::SurfaceProperties(m_shape, props);
    return point(props.CentreOfMass());
}

std::vector<CadBoundaryLoop> CadShape::boundary_loops() const
{
    std::lock_guard<std::recursive_mutex> lock(m_context->mutex);
    require(CadShapeType::Face);
    const TopoDS_Face face = TopoDS::Face(m_shape);
    const TopoDS_Wire outer = BRepTools::OuterWire(face);
    std::vector<CadBoundaryLoop> result;
    for (TopoDS_Iterator it(face); it.More(); it.Next()) {
        if (it.Value().ShapeType() != TopAbs_WIRE) continue;
        const TopoDS_Wire wire = TopoDS::Wire(it.Value());
        CadBoundaryLoop loop;
        loop.is_outer = !outer.IsNull() && wire.IsSame(outer);
        loop.wire = view(wire);
        std::size_t edge_count = 0;
        for (TopoDS_Iterator child(wire); child.More(); child.Next())
            if (child.Value().ShapeType() == TopAbs_EDGE) ++edge_count;
        for (BRepTools_WireExplorer edge(wire, face); edge.More(); edge.Next()) {
            const TopoDS_Edge& current = edge.Current();
            loop.edges.push_back({view(current), BRep_Tool::IsClosed(current, face), BRep_Tool::Degenerated(current)});
        }
        loop.is_complete = loop.edges.size() == edge_count;
        result.push_back(std::move(loop));
    }
    return result;
}

} // namespace Slic3r
