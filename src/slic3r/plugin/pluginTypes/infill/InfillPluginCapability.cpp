#include "InfillPluginCapability.hpp"

#include "InfillPluginCapabilityTrampoline.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/libslic3r.h"
#include "slic3r/plugin/PluginManager.hpp"

#include <boost/log/trivial.hpp>
#include <pybind11/cast.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace py = pybind11;

namespace Slic3r {
namespace {

// One Python path element -> Polyline. Points must already be in scaled integer coordinates; the
// host never rescales plugin output, exactly as a built-in filler's _fill_surface_single() output.
Polyline infill_path_from_python(py::handle value, size_t index)
{
    const auto malformed = [index](const std::string& why) {
        return py::value_error("generate() path " + std::to_string(index) + ": " + why);
    };

    // The common dependency-free contract is a list/tuple of orca.host.Point values. Handle these
    // before py::array: pybind11's NumPy type check imports numpy, even when the plugin returned an
    // ordinary Python list and does not declare NumPy as a dependency.
    if (py::isinstance<py::list>(value) || py::isinstance<py::tuple>(value)) {
        const py::sequence points = py::reinterpret_borrow<py::sequence>(value);
        Polyline path;
        path.points.reserve((size_t) py::len(points));
        for (py::handle point : points) {
            if (!py::isinstance<Point>(point))
                throw malformed("a point sequence may contain only orca.host.Point values");
            path.points.push_back(point.cast<Point>());
        }
        if (path.points.size() < 2)
            throw malformed("a path needs at least 2 points");
        return path;
    }

    // Only ask pybind11 about ndarray when NumPy is already loaded. Its array type check imports
    // NumPy, while buffer-protocol values such as memoryview do not require that dependency.
    if (PyObject_CheckBuffer(value.ptr()) &&
        PyDict_GetItemString(PyImport_GetModuleDict(), "numpy") != nullptr &&
        py::isinstance<py::array>(value)) {
        const py::array array = py::reinterpret_borrow<py::array>(value);
        if (array.dtype().kind() != 'i' || array.itemsize() != (py::ssize_t) sizeof(coord_t))
            throw malformed("expected an (N,2) int64 ndarray in scaled coordinates");
        if (array.ndim() != 2 || array.shape(1) != 2)
            throw malformed("expected an (N,2) int64 ndarray in scaled coordinates");
        if (array.shape(0) < 2)
            throw malformed("a path needs at least 2 points");

        // dtype was validated above; forcecast only guarantees a C-contiguous buffer.
        auto buffer = py::array_t<coord_t, py::array::c_style | py::array::forcecast>::ensure(array);
        if (!buffer)
            throw malformed("could not read the path as a contiguous int64 array");
        auto rows = buffer.unchecked<2>();

        Polyline path;
        path.points.reserve((size_t) rows.shape(0));
        for (py::ssize_t i = 0; i < rows.shape(0); ++i)
            path.points.emplace_back((coord_t) rows(i, 0), (coord_t) rows(i, 1));
        return path;
    }

    if (py::isinstance<Polygon>(value)) {
        const Polygon polygon = value.cast<Polygon>();
        if (polygon.points.size() < 2)
            throw malformed("a path needs at least 2 points");
        Polyline path;
        path.points = polygon.points;
        return path;
    }

    if (!py::isinstance<py::sequence>(value) || py::isinstance<py::str>(value) ||
        py::isinstance<py::bytes>(value))
        throw malformed("expected an (N,2) int64 ndarray, an orca.host.Polygon, or a sequence of "
                        "orca.host.Point");

    Polyline         path;
    const py::sequence points = py::reinterpret_borrow<py::sequence>(value);
    path.points.reserve((size_t) py::len(points));
    for (py::handle point : points) {
        if (!py::isinstance<Point>(point))
            throw malformed("a point sequence may contain only orca.host.Point values");
        path.points.push_back(point.cast<Point>());
    }
    if (path.points.size() < 2)
        throw malformed("a path needs at least 2 points");
    return path;
}

py::list infill_paths_to_python(const Polylines& paths)
{
    py::list out;
    for (const Polyline& path : paths) {
        py::list points;
        for (const Point& point : path.points)
            points.append(py::cast(point));
        out.append(std::move(points));
    }
    return out;
}

InfillPluginRequest make_request(const Fill&                              fill,
                                 const FillParams&                        params,
                                 unsigned int                             thickness_layers,
                                 const std::pair<float, Point>&           direction,
                                 ExPolygon                                expolygon)
{
    InfillPluginRequest request;
    request.expolygon            = std::move(expolygon);
    request.layer_id             = fill.layer_id;
    request.thickness_layers     = thickness_layers;
    request.z                    = fill.z;
    request.spacing              = fill.spacing;
    request.overlap              = fill.overlap;
    request.angle                = fill.angle;
    request.direction_angle      = direction.first;
    request.direction_origin     = direction.second;
    request.density              = params.density;
    request.multiline            = params.multiline;
    request.anchor_length        = params.anchor_length;
    request.anchor_length_max    = params.anchor_length_max;
    request.resolution           = params.resolution;
    request.layer_height         = params.layer_height;
    request.flow_width           = params.flow.width();
    request.flow_height          = params.flow.height();
    request.dont_adjust          = params.dont_adjust;
    request.monotonic            = params.monotonic;
    request.complete             = params.complete;
    request.use_arachne          = params.use_arachne;
    request.smooth_factor        = params.smooth_factor;
    return request;
}

} // namespace

Polylines infill_paths_from_python(const pybind11::object& paths)
{
    if (!paths || !py::isinstance<py::sequence>(paths) || py::isinstance<py::str>(paths) ||
        py::isinstance<py::bytes>(paths))
        throw py::value_error("generate() must return a sequence of paths; each path is an (N,2) "
                              "int64 ndarray in scaled coordinates, an orca.host.Polygon, or a "
                              "sequence of orca.host.Point");

    Polylines out;
    size_t    index = 0;
    for (py::handle path : py::reinterpret_borrow<py::sequence>(paths)) {
        out.emplace_back(infill_path_from_python(path, index));
        ++index;
    }
    return out;
}

void InfillPluginCapability::RegisterBindings(pybind11::module_& module)
{
    BOOST_LOG_TRIVIAL(debug) << "Registering orca.infill bindings";

    auto infill = module.def_submodule("infill", "Plugin infill API (experimental)");

    py::class_<InfillPluginRequest>(infill, "InfillPluginRequest",
                                    "Value snapshot of one infill generation call. Units: geometry and "
                                    "direction_origin are scaled integer coordinates; lengths are "
                                    "millimetres; angles are radians. Safe to keep after generate() "
                                    "returns.")
        .def_property_readonly(
            "expolygon",
            [](const InfillPluginRequest& request) -> const ExPolygon& { return request.expolygon; },
            py::return_value_policy::reference_internal,
            "Region to fill, already offset by Fill for the infill/perimeter overlap and translated "
            "into the pattern's working frame. Scaled coordinates.")
        .def_readonly("layer_id", &InfillPluginRequest::layer_id, "Layer id passed to the Fill.")
        .def_readonly("thickness_layers", &InfillPluginRequest::thickness_layers,
                      "Surface thickness in layers.")
        .def_readonly("z", &InfillPluginRequest::z, "Top print-surface Z in millimetres.")
        .def_readonly("spacing", &InfillPluginRequest::spacing, "Infill centreline spacing, mm.")
        .def_readonly("overlap", &InfillPluginRequest::overlap, "Infill/perimeter overlap, mm.")
        .def_readonly("angle", &InfillPluginRequest::angle, "Current fill angle, radians CCW from +X.")
        .def_readonly("direction_angle", &InfillPluginRequest::direction_angle,
                      "Direction chosen for this surface, radians CCW from +X.")
        .def_readonly("direction_origin", &InfillPluginRequest::direction_origin,
                      "Origin of the fill direction, scaled coordinates.")
        .def_readonly("density", &InfillPluginRequest::density, "Fill density as a fraction in <0,1>.")
        .def_readonly("multiline", &InfillPluginRequest::multiline, "Requested multiline count.")
        .def_readonly("anchor_length", &InfillPluginRequest::anchor_length, "Infill anchor length, mm.")
        .def_readonly("anchor_length_max", &InfillPluginRequest::anchor_length_max,
                      "Maximum infill anchor length, mm.")
        .def_readonly("resolution", &InfillPluginRequest::resolution, "G-code resolution, mm.")
        .def_readonly("layer_height", &InfillPluginRequest::layer_height, "Layer height, mm.")
        .def_readonly("flow_width", &InfillPluginRequest::flow_width,
                      "Extrusion width this fill will be converted to, mm.")
        .def_readonly("flow_height", &InfillPluginRequest::flow_height,
                      "Extrusion height this fill will be converted to, mm.")
        .def_readonly("dont_adjust", &InfillPluginRequest::dont_adjust,
                      "Do not adjust spacing to fill the region evenly.")
        .def_readonly("monotonic", &InfillPluginRequest::monotonic, "Monotonic ordering requested.")
        .def_readonly("complete", &InfillPluginRequest::complete,
                      "Pattern-specific: complete each loop.")
        .def_readonly("use_arachne", &InfillPluginRequest::use_arachne,
                      "Arachne/variable-width generation requested. Plugin infill cannot honour it: "
                      "the adapter rejects the thick-path hook.")
        .def_readonly("smooth_factor", &InfillPluginRequest::smooth_factor,
                      "Corner smoothing factor in [0,1].")
        .def_property_readonly("full_infill", &InfillPluginRequest::full_infill,
                               "True when density is effectively 100%.")
        .def_property_readonly("dont_connect", &InfillPluginRequest::dont_connect,
                               "True when infill paths must not be anchored to perimeters.");

    py::class_<InfillPluginCapability, PluginCapabilityInterface,
               PyInfillPluginCapabilityTrampoline,
               std::shared_ptr<InfillPluginCapability>>(infill, "InfillPluginCapabilityBase")
        .def(py::init<>())
        .def("get_type", &InfillPluginCapability::get_type)
        .def("no_sort", &InfillPluginCapability::no_sort,
             "Optional, default False. Return True to keep Fill's extrusion collection unsorted, "
             "preserving the order of the paths generate() returns through the G-code path planner.")
        .def("aligned_to_origin", &InfillPluginCapability::aligned_to_origin,
             "Optional, default False. Return True when the pattern is laid out around the Fill "
             "bounding-box center rather than the island's own coordinates: Fill::fill_surface() "
             "shifts each island to the origin before generate() and shifts the returned paths "
             "back. The request expolygon is in that shifted frame.")
        .def(
            "generate",
            [](InfillPluginCapability& self, const InfillPluginRequest& request) {
                return infill_paths_to_python(self.generate(request));
            },
            py::arg("request"),
            "Generate infill for one surface island. Runs synchronously on the slicing worker "
            "thread; do not call UI from here. Return a sequence of open paths, each an (N,2) "
            "int64 ndarray in scaled coordinates, an orca.host.Polygon, or a sequence of "
            "orca.host.Point. Paths are returned as-is: Fill converts them to extrusions, applies "
            "flow, and runs gap fill; it does not clip them again, so stay inside "
            "request.expolygon.");
}

Fill* InfillPluginCapability::create_fill(
    std::shared_ptr<InfillPluginCapability> self) const
{
    return new InfillPluginFill(std::move(self));
}

InfillPluginFill::InfillPluginFill(std::shared_ptr<InfillPluginCapability> capability)
    : m_capability(std::move(capability))
{
    if (!m_capability)
        throw InvalidArgument("InfillPluginFill requires a capability");
}

bool InfillPluginFill::no_sort() const { return m_capability->no_sort(); }

bool InfillPluginFill::aligned_to_origin() const { return m_capability->aligned_to_origin(); }

Fill* InfillPluginFill::clone() const
{
    auto clone = std::make_unique<InfillPluginFill>(m_capability);
    clone->layer_id                      = layer_id;
    clone->z                             = z;
    clone->spacing                       = spacing;
    clone->overlap                       = overlap;
    clone->angle                         = angle;
    clone->fixed_angle                   = fixed_angle;
    clone->link_max_length               = link_max_length;
    clone->loop_clipping                 = loop_clipping;
    clone->bounding_box                  = bounding_box;
    clone->adapt_fill_octree             = adapt_fill_octree;
    clone->adaptive_infill_field         = adaptive_infill_field;
    clone->print_config                  = print_config;
    clone->print_object_config           = print_object_config;
    clone->no_overlap_expolygons         = no_overlap_expolygons;
    clone->dont_alternate_fill_direction = dont_alternate_fill_direction;
    return clone.release();
}

void InfillPluginFill::_fill_surface_single(const FillParams&                params,
                                            unsigned int                     thickness_layers,
                                            const std::pair<float, Point>&   direction,
                                            ExPolygon                        expolygon,
                                            Polylines&                       polylines_out)
{
    InfillPluginRequest request =
        make_request(*this, params, thickness_layers, direction, std::move(expolygon));

    Polylines generated;
    try {
        generated = m_capability->generate(request);
    } catch (const CanceledException&) {
        throw; // cancellation must reach the slicing job untouched
    } catch (const std::exception& ex) {
        // A plugin raise is a non-critical slicing failure. Keep the detailed cause in the plugin's
        // diagnostic store and give the user the same generic message for every plugin infill fault.
        const std::string &plugin_key = m_capability->audit_plugin_key();
        if (!plugin_key.empty())
            PluginManager::instance().report_plugin_diagnostic(
                plugin_key, "Infill capability '" + m_capability->name() + "' failed: " + ex.what());
        throw SlicingError(PluginManager::plugin_infill_failure_message());
    }

    polylines_out.reserve(polylines_out.size() + generated.size());
    for (Polyline& path : generated) {
        // A one-point path cannot become an extrusion. Do not reject the whole surface for it.
        if (path.points.size() >= 2)
            polylines_out.emplace_back(std::move(path));
    }
}

void InfillPluginFill::_fill_surface_single(const FillParams&                params,
                                            unsigned int                     thickness_layers,
                                            const std::pair<float, Point>&   direction,
                                            ExPolygon                        expolygon,
                                            ThickPolylines&                  thick_polylines_out)
{
    (void) params;
    (void) thickness_layers;
    (void) direction;
    (void) expolygon;
    (void) thick_polylines_out;
    throw InvalidArgument("plugin infill does not support Arachne variable-width generation");
}

} // namespace Slic3r
