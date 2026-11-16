#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/CAD/CadShape.hpp"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom_BezierSurface.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_Surface.hxx>
#include <GeomConvert.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <NCollection_Array2.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Handle.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <TopLoc_Location.hxx>
#include <gp_Ax1.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

TopoDS_Wire square(double lo, double hi)
{
    BRepBuilderAPI_MakePolygon wire;
    wire.Add(gp_Pnt(lo, lo, 0)); wire.Add(gp_Pnt(hi, lo, 0));
    wire.Add(gp_Pnt(hi, hi, 0)); wire.Add(gp_Pnt(lo, hi, 0)); wire.Close();
    return wire.Wire();
}

TopoDS_Face face_with_hole()
{
    BRepBuilderAPI_MakeFace face(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), square(0, 20));
    face.Add(TopoDS::Wire(square(8, 12).Reversed()));
    return face.Face();
}

std::shared_ptr<CadShape> surface(const std::shared_ptr<CadShape>& shape, const std::string& type)
{
    for (const auto& face : shape->subshapes(CadShapeType::Face))
        if (face->surface_type() == type) return face;
    return {};
}

double distance(const CadPoint& a, const CadPoint& b)
{
    return std::sqrt(std::pow(a[0]-b[0], 2) + std::pow(a[1]-b[1], 2) + std::pow(a[2]-b[2], 2));
}

} // namespace

TEST_CASE("CAD topology traversal exposes unique faces edges and vertices", "[CadShape]")
{
    auto body = CadShape::copy_of(BRepPrimAPI_MakeBox(10, 20, 30).Shape());
    CHECK(body->type() == CadShapeType::Solid);
    CHECK(body->children().size() == 1);
    CHECK(body->subshapes(CadShapeType::Shell).size() == 1);
    CHECK(body->subshapes(CadShapeType::Face).size() == 6);
    CHECK(body->subshapes(CadShapeType::Edge).size() == 12);
    CHECK(body->subshapes(CadShapeType::Vertex).size() == 8);
    CHECK_THROWS_AS(body->vertex_point(), std::invalid_argument);
    CHECK_THROWS_AS(body->uv_bounds(), std::invalid_argument);
}

TEST_CASE("Face boundaries preserve holes and connected edge order", "[CadShape]")
{
    auto source = face_with_hole();
    if (GENERATE(false, true)) source.Reverse();
    auto face = CadShape::copy_of(source);
    CHECK_THAT(face->area(), WithinAbs(20.*20. - 4.*4., 1e-8));
    const auto loops = face->boundary_loops();
    REQUIRE(loops.size() == 2);
    CHECK(std::count_if(loops.begin(), loops.end(), [](const auto& loop) { return loop.is_outer; }) == 1);
    for (const auto& loop : loops) {
        CHECK(loop.is_complete);
        CHECK(loop.wire->type() == CadShapeType::Wire);
        REQUIRE(loop.edges.size() == 4);
        for (std::size_t i = 0; i < loop.edges.size(); ++i) {
            const auto points = loop.edges[i].shape->sample_curve(2);
            const auto next = loop.edges[(i+1)%loop.edges.size()].shape->sample_curve(2);
            CHECK_THAT(distance(points.back(), next.front()), WithinAbs(0., 1e-8));
        }
    }
    CHECK(face->evaluate_surface(2, 2).inside_trim);
    CHECK_FALSE(face->evaluate_surface(10, 10).inside_trim);
    CHECK(face->evaluate_surface(8, 10).inside_trim); // boundary counts as inside
}

TEST_CASE("Face frames follow orientation and identify tangent coordinates", "[CadShape]")
{
    const auto source = face_with_hole();
    auto forward = CadShape::copy_of(source);
    auto reverse = CadShape::copy_of(source.Reversed());
    REQUIRE(forward->local_frame());
    REQUIRE(reverse->local_frame());
    CHECK_THAT(forward->local_frame()->normal[2], WithinAbs(1., 1e-12));
    CHECK_THAT(reverse->local_frame()->normal[2], WithinAbs(-1., 1e-12));
    const auto frame = *reverse->local_frame();
    const double cross_z = frame.x_axis[0]*frame.y_axis[1] - frame.x_axis[1]*frame.y_axis[0];
    CHECK_THAT(cross_z, WithinAbs(frame.normal[2], 1e-12));
    CHECK_THAT(frame.origin[0], WithinAbs(10., 1e-12));
}

TEST_CASE("Cylindrical faces expose periodic surfaces and repeated seam occurrences", "[CadShape]")
{
    auto cylinder = CadShape::copy_of(BRepPrimAPI_MakeCylinder(5., 12.).Shape());
    auto face = surface(cylinder, "cylinder");
    REQUIRE(face);
    CHECK(face->periodic()[0]);
    const auto bounds = face->uv_bounds();
    const auto a = face->evaluate_surface(bounds[0], 6.);
    const auto b = face->evaluate_surface(bounds[0] + 0.25*(bounds[1]-bounds[0]), 6.);
    REQUIRE(a.frame);
    REQUIRE(b.frame);
    CHECK_THAT(a.frame->normal[0]*b.frame->normal[0] + a.frame->normal[1]*b.frame->normal[1], WithinAbs(0., 1e-9));
    int seam_occurrences = 0;
    for (const auto& loop : face->boundary_loops()) {
        CHECK(loop.is_complete);
        for (const auto& edge : loop.edges) seam_occurrences += edge.is_seam;
    }
    CHECK(seam_occurrences == 2);
}

TEST_CASE("Spherical poles retain degenerate boundary topology", "[CadShape]")
{
    auto sphere = CadShape::copy_of(BRepPrimAPI_MakeSphere(5.).Shape());
    auto face = surface(sphere, "sphere");
    REQUIRE(face);
    int degenerate = 0;
    for (const auto& loop : face->boundary_loops())
        for (const auto& edge : loop.edges)
            if (edge.is_degenerate) {
                ++degenerate;
                CHECK(edge.shape->curve_type() == "degenerate");
                CHECK_THROWS_AS(edge.shape->parameter_range(), std::invalid_argument);
                CHECK_FALSE(edge.shape->subshapes(CadShapeType::Vertex).empty());
            }
    CHECK(degenerate > 0);
    const auto bounds = face->uv_bounds();
    CHECK_FALSE(face->evaluate_surface(0.5*(bounds[0]+bounds[1]), bounds[3]).frame);
}

TEST_CASE("Freeform spline faces support parameter evaluation without a planar assumption", "[CadShape]")
{
    NCollection_Array2<gp_Pnt> poles(1, 2, 1, 2);
    poles(1, 1) = gp_Pnt(0, 0, 0); poles(2, 1) = gp_Pnt(10, 0, 0);
    poles(1, 2) = gp_Pnt(0, 10, 0); poles(2, 2) = gp_Pnt(10, 10, 4);
    auto spline = GeomConvert::SurfaceToBSplineSurface(new Geom_BezierSurface(poles));
    auto face = CadShape::copy_of(BRepBuilderAPI_MakeFace(spline, 1e-6).Face());
    CHECK(face->surface_type() == "bspline");
    const auto sample = face->evaluate_surface(0.5, 0.5);
    CHECK_THAT(sample.position[2], WithinAbs(1., 1e-8));
    CHECK(sample.inside_trim);
    REQUIRE(sample.frame);
    CHECK(sample.frame->normal[2] > 0.);
    CHECK_THROWS_AS(face->evaluate_surface(std::numeric_limits<double>::quiet_NaN(), 0.), std::invalid_argument);
    CHECK_THROWS_AS(face->evaluate_surface(2., 0.), std::invalid_argument);
}

TEST_CASE("Edge sampling respects traversal orientation and validates parameters", "[CadShape]")
{
    auto edge = BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)).Edge();
    auto reverse = CadShape::copy_of(edge.Reversed());
    CHECK(reverse->curve_type() == "line");
    const auto points = reverse->sample_curve(3);
    CHECK_THAT(points.front()[0], WithinAbs(10., 1e-8));
    CHECK_THAT(points.back()[0], WithinAbs(0., 1e-8));
    const auto sample = reverse->evaluate_curve(5.);
    REQUIRE(sample.tangent);
    CHECK_THAT((*sample.tangent)[0], WithinAbs(-1., 1e-8));
    CHECK_THAT(sample.derivative[0], WithinAbs(1., 1e-8));
    CHECK_THROWS_AS(reverse->sample_curve(1), std::invalid_argument);
    CHECK_THROWS_AS(reverse->evaluate_curve(-1.), std::invalid_argument);
}

TEST_CASE("Snapshots detach geometry and descendants outlive their parents", "[CadShape]")
{
    TopoDS_Vertex vertex = BRepBuilderAPI_MakeVertex(gp_Pnt(1, 2, 3)).Vertex();
    auto copy = CadShape::copy_of(vertex);
    BRep_Builder builder;
    builder.UpdateVertex(vertex, gp_Pnt(90, 90, 90), 1e-6);
    CHECK_THAT(copy->vertex_point()[0], WithinAbs(1., 1e-12));
    auto source_face = face_with_hole();
    auto face = CadShape::copy_of(source_face);
    BRep_Tool::Surface(source_face)->Translate(gp_Vec(0, 0, 10));
    CHECK_THAT(face->evaluate_surface(2., 2.).position[2], WithinAbs(0., 1e-12));
    auto edge = face->boundary_loops()[0].edges[0].shape;
    face.reset();
    CHECK(edge->sample_curve(2).size() == 2);
}

TEST_CASE("Detached surface snapshots allow concurrent reads", "[CadShape]")
{
    auto face = CadShape::copy_of(face_with_hole());
    std::array<double, 4> areas{};
    std::array<std::exception_ptr, 4> errors{};
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < areas.size(); ++i)
        workers.emplace_back([&, i] {
            try {
                for (int n = 0; n < 20; ++n) {
                    areas[i] = face->area();
                    (void)face->evaluate_surface(2., 2.);
                    (void)face->boundary_loops();
                }
            } catch (...) { errors[i] = std::current_exception(); }
        });
    for (auto& worker : workers) worker.join();
    for (std::size_t i = 0; i < areas.size(); ++i) {
        CHECK_FALSE(errors[i]);
        CHECK_THAT(areas[i], WithinAbs(384., 1e-8));
    }
}

TEST_CASE("Located CAD faces and their descendants keep OCCT coordinates", "[CadShape]")
{
    gp_Trsf transform;
    transform.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), std::acos(-1.) / 2.);
    transform.SetTranslationPart(gp_Vec(5, 6, 7));
    auto face = CadShape::copy_of(face_with_hole().Moved(TopLoc_Location(transform)));
    const auto sample = face->evaluate_surface(2., 3.);
    CHECK_THAT(sample.position[0], WithinAbs(7., 1e-9));
    CHECK_THAT(sample.position[1], WithinAbs(6., 1e-9));
    CHECK_THAT(sample.position[2], WithinAbs(10., 1e-9));
    REQUIRE(sample.frame);
    CHECK_THAT(sample.frame->normal[1], WithinAbs(-1., 1e-9));
    CHECK_THAT(face->area(), WithinAbs(384., 1e-8));
    for (const auto& vertex : face->subshapes(CadShapeType::Vertex))
        CHECK_THAT(vertex->vertex_point()[1], WithinAbs(6., 1e-9));
}

TEST_CASE("Unbounded CAD geometry requires explicit finite evaluation parameters", "[CadShape]")
{
    auto face = CadShape::copy_of(BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1))).Face());
    CHECK_FALSE(face->local_frame());
    CHECK(std::isinf(face->uv_bounds()[0]));
    CHECK_THAT(face->evaluate_surface(2., 3.).position[0], WithinAbs(2., 1e-9));
    auto edge = CadShape::copy_of(BRepBuilderAPI_MakeEdge(gp_Lin(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0))).Edge());
    CHECK_THAT(edge->evaluate_curve(20.).position[0], WithinAbs(20., 1e-9));
    CHECK_THROWS_AS(edge->sample_curve(3), std::invalid_argument);
    CHECK_THROWS_AS(edge->evaluate_curve(std::numeric_limits<double>::infinity()), std::invalid_argument);
}

TEST_CASE("STEP imported curved geometry retains queryable CAD surfaces", "[CadShape]")
{
    STEPControl_Writer writer;
    REQUIRE(writer.Transfer(BRepPrimAPI_MakeCylinder(5., 12.).Shape(), STEPControl_AsIs) == IFSelect_RetDone);
    std::stringstream step;
    REQUIRE(writer.WriteStream(step) == IFSelect_RetDone);
    STEPControl_Reader reader;
    REQUIRE(reader.ReadStream("cad-shape-test.step", step) == IFSelect_RetDone);
    REQUIRE(reader.TransferRoots() > 0);
    auto face = surface(CadShape::copy_of(reader.OneShape()), "cylinder");
    REQUIRE(face);
    CHECK_THAT(face->area(), WithinAbs(2.*std::acos(-1.)*5.*12., 1e-6));
    CHECK_FALSE(face->boundary_loops().empty());
}
