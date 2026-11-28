#include "PluginHostBindings.hpp"
#include <pybind11/pytypes.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/cast.h>
#include "slic3r/plugin/PluginBindingUtils.hpp"

#include <cstddef>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/ClipperUtils.hpp> // offset/offset_ex/union_ex/diff_ex/intersection_ex
#include <libslic3r/ExPolygon.hpp>

#include "libslic3r/Polygon.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Point.hpp"
#include <pybind11/stl.h>

#include <string>
#include <utility>
#include <vector>
#include <pybind11/attr.h>
#include <pybind11/detail/common.h>
#include <pybind11/detail/descr.h>

namespace py = pybind11;

namespace Slic3r {
namespace {
// --- Input path: Python geometry -> C++ Polygon/ExPolygon, with validation. ---------------
// The mutators take scaled integer coords (the same units the read views hand out). A Python
// raise here surfaces as ValueError (pybind translates) so malformed input is rejected up
// front rather than silently corrupting the slicing graph.

// One (N,2) int64 ndarray -> Polygon. Rejects wrong dtype/shape and degenerate (<3 pt) rings.
// Float / NaN / inf are rejected implicitly: only a signed-integer, 8-byte (coord_t==int64)
// dtype is accepted, and integer arrays cannot hold NaN/inf.
Polygon parse_polygon(py::handle h, const char* who)
{
    if (!py::isinstance<py::array>(h))
        throw py::value_error(std::string(who) + ": each contour/hole must be an (N,2) int64 ndarray");
    py::array a = py::reinterpret_borrow<py::array>(h);
    if (a.dtype().kind() != 'i' || a.itemsize() != (py::ssize_t) sizeof(coord_t))
        throw py::value_error(std::string(who) + ": polygon coordinates must be int64 (scaled coords)");
    if (a.ndim() != 2 || a.shape(1) != 2)
        throw py::value_error(std::string(who) + ": each polygon array must have shape (N,2)");
    if (a.shape(0) < 3)
        throw py::value_error(std::string(who) + ": a polygon needs at least 3 points");
    // dtype already validated as int64; forcecast here only guarantees a C-contiguous buffer.
    auto arr = py::array_t<coord_t, py::array::c_style | py::array::forcecast>::ensure(a);
    if (!arr)
        throw py::value_error(std::string(who) + ": could not read polygon as a contiguous int64 array");
    auto r = arr.unchecked<2>();
    Polygon poly;
    poly.points.reserve((size_t) arr.shape(0));
    for (py::ssize_t i = 0; i < arr.shape(0); ++i)
        poly.points.emplace_back((coord_t) r(i, 0), (coord_t) r(i, 1));
    return poly;
}

// Accept a bound orca.host.Polygon (copied) or an (N,2) int64 ndarray. Used by the ExPolygon
// binding, whose constructor/contour-setter/set_holes must accept the Polygon it itself hands
// out (e.g. `ExPolygon(some_polygon_ref)`) in addition to the ndarray-only parse_polygon() path.
Polygon as_polygon(py::handle h, const char* who)
{
    if (py::isinstance<Polygon>(h))
        return h.cast<Polygon>();
    return parse_polygon(h, who);
}
} // namespace

void host_bindings::register_geometry(py::module_& host)
{
    // ------------------------------------------------------------------
    // Geometry value types of the `orca.host` surface. All use pybind's
    // default holder, so plugins can construct and own instances. When
    // obtained from the live slicing graph they are non-owning references
    // instead — see the lifetime rule in PluginHostSlicing.cpp.
    // ------------------------------------------------------------------

    py::class_<BoundingBoxf3>(host, "BoundingBox",
        "Axis-aligned 3D bounding box in mm. It is a copy, independent of what it was computed from. Check defined "
        "before using the other fields.")
        .def_property_readonly("defined", [](const BoundingBoxf3& bb) { return bb.defined; },
            "False for an empty box, such as that of an object without model parts, or one with no extent along "
            "some axis, such as that of a flat mesh; min and max then mean nothing.")
        .def_property_readonly("min", [](const BoundingBoxf3& bb) { return vec3_to_tuple(bb.min); },
            "Minimum corner (x, y, z), in mm.")
        .def_property_readonly("max", [](const BoundingBoxf3& bb) { return vec3_to_tuple(bb.max); },
            "Maximum corner (x, y, z), in mm.")
        .def_property_readonly("size", [](const BoundingBoxf3& bb) { return vec3_to_tuple(bb.size()); },
            "Extent (dx, dy, dz), max minus min, in mm.")
        .def_property_readonly("center", [](const BoundingBoxf3& bb) { return vec3_to_tuple(bb.center()); },
            "Center (x, y, z), the midpoint of min and max, in mm.")
        .def_property_readonly("radius", [](const BoundingBoxf3& bb) { return bb.radius(); },
            "Half the length of the box's diagonal, in mm: the radius of the sphere around center that holds the box.");

    // Point: a constructible value type (default holder, so Python-owned instances
    // are freed). Returned-by-reference from Polygon.points, it aliases the buffer;
    // x()/y() are Eigen lvalues, so the properties are read/write. p+q / p-q go
    // through Eigen expression templates, wrapped back into a Point.
    py::class_<Point>(host, "Point",
        "2D point in scaled integer coordinates, the unit of all orca.host 2D geometry: mm = value * SCALING_FACTOR, "
        "which is 1e-6 (1 unit = 1 nm), or 1e-5 when a side of the printer's bed is longer than 2147 mm. "
        "orca.slicing.unscale(value) converts a coordinate to mm with the factor in use, and "
        "round(mm / orca.slicing.unscale(1)) converts back. A Point read from Polygon.points is a live reference into "
        "that polygon.")
        .def(py::init([](coord_t x, coord_t y) { return Point(x, y); }), py::arg("x"), py::arg("y"),
             "Point at (x, y), in scaled integer coordinates.")
        .def_property("x", [](const Point& p) { return p.x(); },
                           [](Point& p, coord_t v) { p.x() = v; },
                      "X coordinate in scaled units; assigning it edits the polygon this Point belongs to, if any.")
        .def_property("y", [](const Point& p) { return p.y(); },
                           [](Point& p, coord_t v) { p.y() = v; },
                      "Y coordinate in scaled units; assigning it edits the polygon this Point belongs to, if any.")
        .def("__add__", [](const Point& a, const Point& b) { return Point(a + b); }, py::is_operator(),
             "Component-wise sum, as a new Point.")
        .def("__sub__", [](const Point& a, const Point& b) { return Point(a - b); }, py::is_operator(),
             "Component-wise difference, as a new Point.")
        .def("__mul__", [](const Point& a, double s) { return Point(a.x() * s, a.y() * s); }, py::is_operator(),
             "Both coordinates multiplied by a float and rounded to integers, as a new Point.")
        .def("__repr__", [](const Point& p) {
            return "orca.host.Point(" + std::to_string(p.x()) + ", " + std::to_string(p.y()) + ")";
        }, "orca.host.Point(x, y) with the scaled coordinates.");

    py::class_<Polygon>(host, "Polygon",
        "Closed 2D polygon whose last vertex joins back to the first, in scaled integer coordinates (see Point). "
        "Outer contours run counter-clockwise and holes clockwise. Plugins can construct and own one; a Polygon "
        "reached from an ExPolygon or the slicing graph is a live reference into it.")
        .def(py::init<>(), "Empty polygon; add vertices with append() or set_points().")
        .def("size", [](const Polygon& p) { return p.points.size(); }, "Number of vertices.")
        .def("is_valid", [](const Polygon& p) { return p.is_valid(); },
             "True when the polygon has at least 3 vertices; self-intersections are not checked.")
        .def("is_counter_clockwise", [](const Polygon& p) { return p.is_counter_clockwise(); },
             "True when the signed area is 0 or more: counter-clockwise winding, as outer contours have.")
        .def("is_clockwise", [](const Polygon& p) { return p.is_clockwise(); },
             "True when the signed area is negative: clockwise winding, as holes have.")
        .def("make_counter_clockwise", [](Polygon& p) { return p.make_counter_clockwise(); },
             "Reorient to CCW in place. Returns True if it reversed the winding.")
        .def("make_clockwise", [](Polygon& p) { return p.make_clockwise(); },
             "Reorient to CW in place. Returns True if it reversed the winding.")
        .def("area", [](const Polygon& p) { return p.area(); },
             "Signed area in scaled units squared, positive when counter-clockwise and negative when clockwise; "
             "multiply by orca.slicing.unscale(1) ** 2 for mm^2.")
        .def("centroid", [](const Polygon& p) { return p.centroid(); },
             "Centre of mass of the enclosed area, as a Point in scaled coordinates. Meaningless for a polygon with "
             "fewer than 3 vertices or no area.")
        .def("contains", [](const Polygon& p, const Point& pt) { return p.contains(pt); }, py::arg("point"),
             "True when point lies inside the polygon or on its boundary, whichever its winding.")
        .def("translate", [](Polygon& p, double x, double y) { p.translate(x, y); }, py::arg("x"), py::arg("y"),
             "Move every vertex by (x, y) scaled units, in place; x and y are truncated to integers.")
        .def("rotate", [](Polygon& p, double angle) { p.rotate(angle); }, py::arg("angle"),
             "Rotate in place by angle radians, counter-clockwise, about the origin (0, 0); vertices are rounded to "
             "integers.")
        .def("rotate", [](Polygon& p, double angle, const Point& c) { p.rotate(angle, c); },
             py::arg("angle"), py::arg("center"),
             "Rotate in place by angle radians, counter-clockwise, about the Point center; vertices are rounded to "
             "integers.")
        .def("douglas_peucker", [](Polygon& p, double tol) { p.douglas_peucker(tol); }, py::arg("tolerance"),
             "Simplify in place with the Douglas-Peucker algorithm, dropping vertices within tolerance scaled units "
             "of the simplified outline. A small polygon can end up with fewer than 3 vertices, so check is_valid() "
             "afterwards. The polygon must not be empty.")
        .def("simplify", [](const Polygon& p, double tol) { return p.simplify(tol); }, py::arg("tolerance"),
             "Return simplified geometry as a list of Polygon (may split into several): Douglas-Peucker with "
             "tolerance in scaled units, then cleaned into simple polygons. Expects a non-empty counter-clockwise "
             "polygon.")
        .def("offset", [](const Polygon& p, coord_t delta) { return offset(p, (float) delta); }, py::arg("delta"),
             "Clipper offset by `delta` scaled units (negative shrinks). Returns [Polygon].")
        // --- Point-object idiom: references into the buffer (in-place element edit). ---
        .def_property_readonly("points", [](py::object self) {
            Polygon& p = self.cast<Polygon&>();
            py::list out;
            for (Point& pt : p.points)
                out.append(py::cast(&pt, py::return_value_policy::reference_internal, self));
            return out;
        }, "Vertices as [Point] references into this polygon. Editing a Point mutates the "
           "buffer in place. Structural changes (count) go through set_points/append, which "
           "invalidate previously returned Point refs and array views (C++ vector semantics).")
        .def("append", [](Polygon& p, const Point& pt) { p.points.push_back(pt); }, py::arg("point"),
             "Append a vertex. Structural change (count): invalidates previously returned "
             "Point refs and array views into this polygon (C++ vector semantics).")
        // --- numpy idiom: writable zero-copy (N,2) view (bulk affine edits). ---
        .def("as_array", [](py::object self) {
            Polygon& p = self.cast<Polygon&>();
            return with_numpy([&] {
                return py::object(make_writable_rows<coord_t, 2>(
                    self, p.points.empty() ? nullptr : p.points.front().data(),
                    (py::ssize_t) p.points.size()));
            });
        }, "Vertices as a WRITABLE int64 (N,2) numpy view in scaled coords, aliasing the "
           "buffer. Count-preserving in-place edits only; valid during execute(ctx). Requires numpy.")
        .def("set_points", [](Polygon& p, py::handle src) { p = parse_polygon(src, "Polygon.set_points"); },
             py::arg("points"),
             "Replace all vertices from an (N,2) int64 ndarray (scaled coords). Count-changing; "
             "invalidates prior Point refs and array views. Raises ValueError on malformed input.");

    // ExPolygon: default holder (Python-owned instances are freed) so plugins can construct
    // their own geometry, not just navigate the live slicing graph. contour/holes accessors
    // still use reference_internal, so refs into a graph-owned ExPolygon stay non-owning views
    // tied to that owner's lifetime, same as Polygon/Surface.
    py::class_<ExPolygon>(host, "ExPolygon",
        "Polygon with holes: a counter-clockwise outer contour and clockwise hole Polygons, in scaled integer "
        "coordinates (see Point). Plugins can construct and own one; an ExPolygon reached from the slicing graph is a "
        "live reference into it.")
        .def(py::init([](py::handle contour, py::handle holes) {
            // Accept bound Polygons or (N,2) ndarrays for both contour and each hole.
            ExPolygon ex;
            ex.contour = as_polygon(contour, "ExPolygon.contour");
            if (!holes.is_none()) {
                if (!py::isinstance<py::sequence>(holes) || py::isinstance<py::str>(holes))
                    throw py::value_error("ExPolygon: holes must be a list of Polygon or (N,2) ndarrays");
                for (py::handle h : py::reinterpret_borrow<py::sequence>(holes)) {
                    Polygon hole = as_polygon(h, "ExPolygon.hole");
                    hole.make_clockwise();
                    ex.holes.emplace_back(std::move(hole));
                }
            }
            ex.contour.make_counter_clockwise();
            return ex;
        }), py::arg("contour"), py::arg("holes") = py::none(),
            "Construct from a Polygon/ndarray contour and optional list of hole Polygons/ndarrays. "
            "Orientation is normalized (contour CCW, holes CW).")
        .def_property("contour",
            [](ExPolygon& e) -> Polygon& { return e.contour; },
            [](ExPolygon& e, py::handle v) { e.contour = as_polygon(v, "ExPolygon.contour"); },
            py::return_value_policy::reference_internal,
            "Outer contour (CCW). Read returns a live Polygon ref; assign a Polygon/ndarray to replace it. An "
            "assigned contour keeps its winding, so pass it counter-clockwise.")
        .def_property_readonly("holes", [](py::object self) {
            ExPolygon& e = self.cast<ExPolygon&>();
            py::list out;
            for (Polygon& h : e.holes)
                out.append(py::cast(&h, py::return_value_policy::reference_internal, self));
            return out;
        }, "Hole contours (CW) as [Polygon] references (in-place editable). set_holes replaces them.")
        .def("set_holes", [](ExPolygon& e, py::handle holes) {
            ExPolygon tmp;
            if (!py::isinstance<py::sequence>(holes) || py::isinstance<py::str>(holes))
                throw py::value_error("set_holes: expected a list of Polygon or (N,2) ndarrays");
            for (py::handle h : py::reinterpret_borrow<py::sequence>(holes)) {
                Polygon hole = as_polygon(h, "ExPolygon.set_holes");
                hole.make_clockwise();
                tmp.holes.emplace_back(std::move(hole));
            }
            e.holes = std::move(tmp.holes);
        }, py::arg("holes"), "Replace all holes. Invalidates prior hole refs (C++ vector semantics).")
        .def("translate", [](ExPolygon& e, double x, double y) { e.translate(x, y); }, py::arg("x"), py::arg("y"),
             "Move the contour and holes by (x, y) scaled units, in place; x and y are truncated to integers.")
        .def("rotate", [](ExPolygon& e, double a) { e.rotate(a); }, py::arg("angle"),
             "Rotate the contour and holes in place by angle radians, counter-clockwise, about the origin (0, 0); "
             "vertices are rounded to integers.")
        .def("rotate", [](ExPolygon& e, double a, const Point& c) { e.rotate(a, c); },
             py::arg("angle"), py::arg("center"),
             "Rotate the contour and holes in place by angle radians, counter-clockwise, about the Point center; "
             "vertices are rounded to integers.")
        .def("scale", [](ExPolygon& e, double f) { e.scale(f); }, py::arg("factor"),
             "Multiply every coordinate by factor in place, which scales about the origin (0, 0); results are "
             "truncated to integers.")
        .def("douglas_peucker", [](ExPolygon& e, double t) { e.douglas_peucker(t); }, py::arg("tolerance"),
             "Simplify the contour and each hole in place with the Douglas-Peucker algorithm, tolerance in scaled "
             "units. A small ring can end up with fewer than 3 vertices, so check is_valid() afterwards. The contour "
             "and holes must not be empty.")
        .def("area", [](const ExPolygon& e) { return e.area(); },
             "Area of the contour minus its holes, in scaled units squared (multiply by orca.slicing.unscale(1) ** 2 "
             "for mm^2). Assumes the normal winding: contour counter-clockwise, holes clockwise.")
        .def("is_valid", [](const ExPolygon& e) { return e.is_valid(); },
             "True when the contour has at least 3 vertices and runs counter-clockwise and every hole has at least 3 "
             "vertices and runs clockwise.")
        .def("contains", [](const ExPolygon& e, const Point& p) { return e.contains(p); }, py::arg("point"),
             "True when point lies inside the contour and outside every hole; a point on the contour or on a hole's "
             "boundary counts as inside.")
        .def("num_contours", [](const ExPolygon& e) { return e.num_contours(); },
             "Number of rings: 1 for the contour plus one per hole.")
        .def("simplify", [](const ExPolygon& e, double t) { return e.simplify(t); }, py::arg("tolerance"),
             "Return simplified geometry as [ExPolygon]: Douglas-Peucker on each ring with tolerance in scaled units, "
             "then merged back into simple shapes.")
        .def("offset", [](const ExPolygon& e, coord_t delta) { return offset_ex(e, (float) delta); },
             py::arg("delta"), "Clipper offset by `delta` scaled units (negative shrinks). Returns [ExPolygon].")
        .def("union_ex", [](const ExPolygon& a, const ExPolygon& b) {
            return union_ex(ExPolygons{ a, b });
        }, py::arg("other"), "Union with another ExPolygon. Returns [ExPolygon].")
        .def("diff_ex", [](const ExPolygon& a, const ExPolygon& b) {
            return diff_ex(ExPolygons{ a }, ExPolygons{ b });
        }, py::arg("other"), "This minus `other`. Returns [ExPolygon].")
        .def("intersection_ex", [](const ExPolygon& a, const ExPolygon& b) {
            return intersection_ex(ExPolygons{ a }, ExPolygons{ b });
        }, py::arg("other"), "Intersection with `other`. Returns [ExPolygon].");
}

} // namespace Slic3r
