#include "python_test_support.hpp"
#include "libslic3r/CAD/CadShape.hpp"
#include "slic3r/GUI/CAD/CadFaceSelection.hpp"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <vector>
#include <pybind11/cast.h>
#include <pybind11/embed.h>
#include <pybind11/eval.h>
#include <pybind11/pytypes.h>

namespace py = pybind11;

TEST_CASE("Python CAD snapshots retain descendant geometry without exposing mutation", "[PluginCadGeometry][Python]")
{
    auto cad = import_orca_module().attr("host").attr("cad");
    py::dict scope;
    scope["cad"] = cad;
    Slic3r::GUI::CadFaceSelection selection;
    selection.shape = Slic3r::CadShape::copy_of(BRepPrimAPI_MakeBox(10, 20, 30).Shape())
                          ->subshapes(Slic3r::CadShapeType::Face).front();
    scope["selection"] = py::cast(selection);
    selection.shape.reset();
    REQUIRE_NOTHROW(py::exec(R"(
import gc
import math
assert selection.shape.type == cad.ShapeType.Face
assert selection.local_frame is not None
assert len(selection.boundary_loops) == 1
edge = selection.boundary_loops[0].edges[0].shape
points = edge.sample_curve(5)
del selection
gc.collect()
assert all(math.isclose(a, b, abs_tol=1e-12) for p, q in zip(edge.sample_curve(5), points) for a, b in zip(p, q))
try:
    edge.orientation = "reversed"
except AttributeError:
    pass
else:
    raise AssertionError("Shape property accepted mutation")
try:
    cad.Shape()
except TypeError:
    pass
else:
    raise AssertionError("Shape accepted construction")
try:
    edge.evaluate_surface(0, 0)
except ValueError:
    pass
else:
    raise AssertionError("Surface operation accepted an edge")
)", scope));
}

TEST_CASE("Python CAD surfaces expose oriented frames and seam occurrences", "[PluginCadGeometry][Python]")
{
    auto cad = import_orca_module().attr("host").attr("cad");
    py::dict scope;
    scope["cad"] = cad;
    scope["body"] = py::cast(Slic3r::CadShape::copy_of(BRepPrimAPI_MakeCylinder(5, 12).Shape()));
    REQUIRE_NOTHROW(py::exec(R"(
import math
face = next(f for f in body.subshapes(cad.ShapeType.Face) if f.surface_type == "cylinder")
assert face.periodic == [True, False]
u0, u1, v0, v1 = face.uv_bounds
sample = face.evaluate_surface((u0+u1)/2, (v0+v1)/2)
assert sample.inside_trim
assert sample.frame is not None
assert math.isclose(math.sqrt(sum(x*x for x in sample.frame.normal)), 1.0)
assert sum(e.is_seam for loop in face.boundary_loops() for e in loop.edges) == 2
assert all(loop.is_complete for loop in face.boundary_loops())
)", scope));
}
