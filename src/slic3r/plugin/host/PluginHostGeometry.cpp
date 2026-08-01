#include "PluginHostBindings.hpp"
#include "slic3r/plugin/PluginBindingUtils.hpp"

#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/ClipperUtils.hpp> // offset/offset_ex/union_ex/diff_ex/intersection_ex
#include <libslic3r/ExPolygon.hpp>

#include <pybind11/stl.h>

#include <string>
#include <utility>
#include <vector>

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

    // Axis-aligned bounding box, returned by value (a copy) so its lifetime is
    // independent of the model object it was computed from. Coordinates are in mm.
    py::class_<BoundingBoxf3>(host, "BoundingBox", "Axis-aligned bounding box in millimetres")
        .def_property_readonly("defined", [](const BoundingBoxf3& bb) { return bb.defined; })
        .def_property_readonly("min", [](const BoundingBoxf3& bb) { return vec3_to_tuple(bb.min); })
        .def_property_readonly("max", [](const BoundingBoxf3& bb) { return vec3_to_tuple(bb.max); })
        .def_property_readonly("size", [](const BoundingBoxf3& bb) { return vec3_to_tuple(bb.size()); })
        .def_property_readonly("center", [](const BoundingBoxf3& bb) { return vec3_to_tuple(bb.center()); })
        .def_property_readonly("radius", [](const BoundingBoxf3& bb) { return bb.radius(); });

    // Point: a constructible value type (default holder, so Python-owned instances
    // are freed). Returned-by-reference from Polygon.points, it aliases the buffer;
    // x()/y() are Eigen lvalues, so the properties are read/write. p+q / p-q go
    // through Eigen expression templates, wrapped back into a Point.
    py::class_<Point>(host, "Point")
        .def(py::init([](coord_t x, coord_t y) { return Point(x, y); }), py::arg("x"), py::arg("y"))
        .def_property("x", [](const Point& p) { return p.x(); },
                           [](Point& p, coord_t v) { p.x() = v; })
        .def_property("y", [](const Point& p) { return p.y(); },
                           [](Point& p, coord_t v) { p.y() = v; })
        .def("__add__", [](const Point& a, const Point& b) { return Point(a + b); }, py::is_operator())
        .def("__sub__", [](const Point& a, const Point& b) { return Point(a - b); }, py::is_operator())
        .def("__mul__", [](const Point& a, double s) { return Point(a.x() * s, a.y() * s); }, py::is_operator())
        .def("__repr__", [](const Point& p) {
            return "orca.host.Point(" + std::to_string(p.x()) + ", " + std::to_string(p.y()) + ")";
        });

    py::class_<Polygon>(host, "Polygon")
        .def(py::init<>())
        .def("size", [](const Polygon& p) { return p.points.size(); })
        .def("is_valid", [](const Polygon& p) { return p.is_valid(); })
        .def("is_counter_clockwise", [](const Polygon& p) { return p.is_counter_clockwise(); })
        .def("is_clockwise", [](const Polygon& p) { return p.is_clockwise(); })
        .def("make_counter_clockwise", [](Polygon& p) { return p.make_counter_clockwise(); },
             "Reorient to CCW in place. Returns True if it reversed the winding.")
        .def("make_clockwise", [](Polygon& p) { return p.make_clockwise(); })
        .def("area", [](const Polygon& p) { return p.area(); })
        .def("centroid", [](const Polygon& p) { return p.centroid(); })
        .def("contains", [](const Polygon& p, const Point& pt) { return p.contains(pt); }, py::arg("point"))
        .def("translate", [](Polygon& p, double x, double y) { p.translate(x, y); }, py::arg("x"), py::arg("y"))
        .def("rotate", [](Polygon& p, double angle) { p.rotate(angle); }, py::arg("angle"))
        .def("rotate", [](Polygon& p, double angle, const Point& c) { p.rotate(angle, c); },
             py::arg("angle"), py::arg("center"))
        .def("douglas_peucker", [](Polygon& p, double tol) { p.douglas_peucker(tol); }, py::arg("tolerance"))
        .def("simplify", [](const Polygon& p, double tol) { return p.simplify(tol); }, py::arg("tolerance"),
             "Return simplified geometry as a list of Polygon (may split into several).")
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
    py::class_<ExPolygon>(host, "ExPolygon")
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
            "Outer contour (CCW). Read returns a live Polygon ref; assign a Polygon/ndarray to replace it.")
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
        .def("translate", [](ExPolygon& e, double x, double y) { e.translate(x, y); }, py::arg("x"), py::arg("y"))
        .def("rotate", [](ExPolygon& e, double a) { e.rotate(a); }, py::arg("angle"))
        .def("rotate", [](ExPolygon& e, double a, const Point& c) { e.rotate(a, c); },
             py::arg("angle"), py::arg("center"))
        .def("scale", [](ExPolygon& e, double f) { e.scale(f); }, py::arg("factor"))
        .def("douglas_peucker", [](ExPolygon& e, double t) { e.douglas_peucker(t); }, py::arg("tolerance"))
        .def("area", [](const ExPolygon& e) { return e.area(); })
        .def("is_valid", [](const ExPolygon& e) { return e.is_valid(); })
        .def("contains", [](const ExPolygon& e, const Point& p) { return e.contains(p); }, py::arg("point"))
        .def("num_contours", [](const ExPolygon& e) { return e.num_contours(); })
        .def("simplify", [](const ExPolygon& e, double t) { return e.simplify(t); }, py::arg("tolerance"),
             "Return simplified geometry as [ExPolygon].")
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
