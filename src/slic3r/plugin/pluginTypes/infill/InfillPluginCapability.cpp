#include "InfillPluginCapability.hpp"

#include "InfillPluginCapabilityTrampoline.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/Fill/InfillFactory.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"
#include "slic3r/plugin/PluginManager.hpp"

#include <algorithm>
#include <boost/log/trivial.hpp>
#include <libslic3r/Polyline.hpp>
#include <libslic3r/Point.hpp>
#include <libslic3r/Flow.hpp>
#include <cstdint>
#include <libslic3r/Fill/FillBase.hpp>
#include <libslic3r/ExPolygon.hpp>
#include <libslic3r/Fill/InfillPurpose.hpp>
#include <libslic3r/ExtrusionEntity.hpp>
#include <pybind11/cast.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/stl.h>

#include <cstddef>
#include <exception>
#include <memory>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace Slic3r {
namespace {

// Opaque keep-alive handed to the infill registry for every pattern of one publication. libslic3r
// only holds and drops it; keeping the capability (and therefore its Python module namespace) alive
// is the whole point. The plugin manager retains its own capability reference until every job lease
// has been released, so this is never the reference that performs the final GIL-requiring decref.
struct InfillProviderLease
{
    explicit InfillProviderLease(std::shared_ptr<InfillPluginCapability> provider)
        : capability(std::move(provider)) {}

    std::shared_ptr<InfillPluginCapability> capability;
};

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

// Copy the resolved Flow scalars. Flow::mm3_per_mm() throws for an unset (zero) extrusion, so an
// unset flow is reported as zero area instead of making the snapshot fail.
InfillPluginFlow make_flow_snapshot(const Flow& flow)
{
    InfillPluginFlow snapshot;
    snapshot.width           = flow.width();
    snapshot.height          = flow.height();
    snapshot.spacing         = flow.spacing();
    snapshot.nozzle_diameter = flow.nozzle_diameter();
    snapshot.bridge          = flow.bridge();
    if (flow.width() > 0.f && flow.height() > 0.f)
        snapshot.mm3_per_mm = flow.mm3_per_mm();
    return snapshot;
}

// Fill uses size_t(-1) for "not set"; the Python contract uses -1 so a value can be compared
// without knowing the host word size.
int64_t request_identity(size_t value) { return value == size_t(-1) ? -1 : static_cast<int64_t>(value); }
int     request_region(size_t value)   { return value == size_t(-1) ? -1 : static_cast<int>(value); }

} // namespace

InfillPluginRequest make_infill_plugin_request(const Fill&                    fill,
                                               const FillParams&              params,
                                               unsigned int                   thickness_layers,
                                               const std::pair<float, Point>& direction,
                                               ExPolygon                      expolygon)
{
    InfillPluginRequest request;
    request.expolygon            = std::move(expolygon);
    request.layer_id             = fill.layer_id;
    request.object_id            = request_identity(fill.object_id);
    request.layer_index          = request_identity(fill.layer_index);
    request.region_id            = request_region(fill.region_id);
    request.region_ids.reserve(fill.region_ids.size());
    for (size_t region_id : fill.region_ids)
        request.region_ids.push_back(request_region(region_id));
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
    request.flow                 = make_flow_snapshot(params.flow);
    request.flow_width           = request.flow.width;
    request.flow_height          = request.flow.height;
    request.dont_adjust          = params.dont_adjust;
    request.monotonic            = params.monotonic;
    request.complete             = params.complete;
    request.use_arachne          = params.use_arachne;
    request.smooth_factor        = params.smooth_factor;

    request.gyroid_optimized                 = params.gyroid_optimized;
    request.adaptive_infill_mode             = params.adaptive_infill_mode;
    request.adaptive_infill_interior_density = params.adaptive_infill_interior_density;
    request.adaptive_infill_gradient         = params.adaptive_infill_gradient;
    request.lateral_lattice_angle_1          = params.lateral_lattice_angle_1;
    request.lateral_lattice_angle_2          = params.lateral_lattice_angle_2;
    request.pattern                          = params.pattern;
    request.infill_overhang_angle            = params.infill_overhang_angle;
    request.extrusion_role                   = params.extrusion_role;
    request.using_internal_flow              = params.using_internal_flow;
    request.no_extrusion_overlap             = params.no_extrusion_overlap;
    request.dont_sort                        = params.dont_sort;
    request.can_reverse                      = params.can_reverse;
    request.fill_order                       = params.fill_order;
    request.horiz_move                       = params.horiz_move;
    request.symmetric_infill_y_axis          = params.symmetric_infill_y_axis;
    request.infill_complete_top              = params.infill_complete_top;
    request.symmetric_y_axis                 = params.symmetric_y_axis;
    request.locked_zag                       = params.locked_zag;
    request.infill_lock_depth                = params.infill_lock_depth;
    request.skin_infill_depth                = params.skin_infill_depth;
    request.center_of_surface_pattern        = params.center_of_surface_pattern;
    request.purpose                          = params.purpose;
    return request;
}

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

std::vector<InfillPluginPattern> InfillPluginCapability::patterns() const
{
    // Compatibility default for plugins written before multi-pattern support: one sparse pattern
    // whose stable ID is the capability name.
    return { InfillPluginPattern{ name(), name(), { InfillSurfacePurpose::Sparse } } };
}

const InfillPluginPattern* InfillPluginCapability::find_resolved_pattern(const std::string& pattern_id) const
{
    const auto it = std::find_if(m_patterns.begin(), m_patterns.end(),
                                 [&pattern_id](const InfillPluginPattern& pattern) { return pattern.id == pattern_id; });
    return it == m_patterns.end() ? nullptr : &*it;
}

bool register_infill_patterns(const std::shared_ptr<InfillPluginCapability>& capability, std::string& error)
{
    error.clear();
    if (!capability) {
        error = "Infill capability is null";
        return false;
    }

    const PluginCapabilityId identity = capability->identity();
    if (identity.plugin_key.empty() || identity.name.empty()) {
        error = "Infill capability has no resolved owner identity";
        return false;
    }

    // One keep-alive for every pattern of this publication. The registry holds it while the
    // patterns are registered and, through a job's Selection, until that job stops creating Fills,
    // so an unload request cannot tear the Python module out from under a running slice.
    std::shared_ptr<void> lease = std::make_shared<InfillProviderLease>(capability);

    bool registered_any = false;
    for (const InfillPluginPattern& pattern : capability->resolved_patterns()) {
        // The loader rejects an empty purpose list before the capability is materialized; guard here
        // too so a native provider cannot publish a pattern that no dropdown could ever select.
        if (pattern.supported_surfaces.empty()) {
            error = "Infill pattern '" + pattern.id + "' declares no supported surfaces";
            if (registered_any)
                unregister_infill_patterns(identity.plugin_key, identity.name);
            return false;
        }

        const std::string pattern_id    = pattern.id;
        const std::string display_name  = pattern.display_name.empty() ? pattern.id : pattern.display_name;
        std::weak_ptr<InfillPluginCapability> weak = capability;
        InfillFactoryFn factory = [weak, pattern_id]() -> std::unique_ptr<Fill> {
            // The registry still holds a weak provider reference for creation. A Fill created while
            // the provider is alive holds its own strong reference; after unload this fails instead
            // of resolving to another plugin that reused the ID.
            std::shared_ptr<InfillPluginCapability> provider = weak.lock();
            if (!provider)
                return nullptr;
            return std::unique_ptr<Fill>(provider->create_fill(provider, pattern_id));
        };

        if (!InfillFactory::register_plugin_infill(pattern_id, display_name, identity.plugin_key, identity.name,
                                                   pattern.supported_surfaces, lease, std::move(factory))) {
            error = "Infill pattern '" + pattern_id + "' is already registered by another provider";
            if (registered_any)
                unregister_infill_patterns(identity.plugin_key, identity.name);
            return false;
        }
        registered_any = true;
    }

    return true;
}

void unregister_infill_patterns(const std::string& plugin_key, const std::string& capability_name)
{
    InfillFactory::unregister_plugin_infills(plugin_key, capability_name);
}

void InfillPluginCapability::RegisterBindings(pybind11::module_& module)
{
    BOOST_LOG_TRIVIAL(debug) << "Registering orca.infill bindings";

    auto infill = module.def_submodule("infill", "Plugin infill API (experimental)");

    py::enum_<InfillSurfacePurpose>(infill, "SurfacePurpose",
                                    "Surface category a pattern is selected for. A pattern is offered only in the "
                                    "settings whose purpose it declares.")
        .value("Sparse", InfillSurfacePurpose::Sparse)
        .value("InternalSolid", InfillSurfacePurpose::InternalSolid)
        .value("TopSurface", InfillSurfacePurpose::TopSurface)
        .value("BottomSurface", InfillSurfacePurpose::BottomSurface);

    // Host fill pattern. A plugin call runs as Plugin; the concrete plugin pattern is generate()'s
    // pattern_id. The built-in values are exposed so the field is semantic rather than a raw int.
    py::enum_<InfillPattern>(infill, "InfillPattern",
                             "Host infill pattern of the call. Plugin dispatch reports Plugin; the "
                             "selected plugin pattern is the pattern_id argument of generate().")
        .value("Monotonic", ipMonotonic)
        .value("MonotonicLine", ipMonotonicLine)
        .value("Rectilinear", ipRectilinear)
        .value("AlignedRectilinear", ipAlignedRectilinear)
        .value("ZigZag", ipZigZag)
        .value("CrossZag", ipCrossZag)
        .value("LockedZag", ipLockedZag)
        .value("Line", ipLine)
        .value("Grid", ipGrid)
        .value("Triangles", ipTriangles)
        .value("Stars", ipStars)
        .value("Cubic", ipCubic)
        .value("AdaptiveCubic", ipAdaptiveCubic)
        .value("QuarterCubic", ipQuarterCubic)
        .value("SupportCubic", ipSupportCubic)
        .value("Lightning", ipLightning)
        .value("Honeycomb", ipHoneycomb)
        .value("Honeycomb3D", ip3DHoneycomb)
        .value("LateralHoneycomb", ipLateralHoneycomb)
        .value("LateralLattice", ipLateralLattice)
        .value("CrossHatch", ipCrossHatch)
        .value("TpmsD", ipTpmsD)
        .value("TpmsFK", ipTpmsFK)
        .value("Gyroid", ipGyroid)
        .value("Concentric", ipConcentric)
        .value("SpiralInset", ipSpiralInset)
        .value("HilbertCurve", ipHilbertCurve)
        .value("ArchimedeanChords", ipArchimedeanChords)
        .value("OctagramSpiral", ipOctagramSpiral)
        .value("SupportBase", ipSupportBase)
        .value("ConcentricInternal", ipConcentricInternal)
        .value("Plugin", ipPlugin);

    py::enum_<ExtrusionRole>(infill, "ExtrusionRole",
                             "Extrusion role this fill will be emitted as.")
        .value("Undefined", erNone)
        .value("Perimeter", erPerimeter)
        .value("ExternalPerimeter", erExternalPerimeter)
        .value("OverhangPerimeter", erOverhangPerimeter)
        .value("InternalInfill", erInternalInfill)
        .value("SolidInfill", erSolidInfill)
        .value("TopSolidInfill", erTopSolidInfill)
        .value("BottomSurface", erBottomSurface)
        .value("Ironing", erIroning)
        .value("BridgeInfill", erBridgeInfill)
        .value("InternalBridgeInfill", erInternalBridgeInfill)
        .value("GapFill", erGapFill)
        .value("Skirt", erSkirt)
        .value("Brim", erBrim)
        .value("SupportMaterial", erSupportMaterial)
        .value("SupportMaterialInterface", erSupportMaterialInterface)
        .value("SupportTransition", erSupportTransition)
        .value("WipeTower", erWipeTower)
        .value("Custom", erCustom)
        .value("Mixed", erMixed);

    py::enum_<AdaptiveInfillMode>(infill, "AdaptiveInfillMode",
                                  "What graded adaptive infill density follows.")
        .value("Disabled", AdaptiveInfillMode::Disabled)
        .value("DistanceWarp", AdaptiveInfillMode::DistanceWarp)
        .value("SmoothBlend", AdaptiveInfillMode::SmoothBlend)
        .value("SteppedShells", AdaptiveInfillMode::SteppedShells)
        .value("Lobes", AdaptiveInfillMode::Lobes)
        .value("NormalZ", AdaptiveInfillMode::NormalZ)
        .value("NormalY", AdaptiveInfillMode::NormalY)
        .value("NormalX", AdaptiveInfillMode::NormalX);

    py::enum_<AdaptiveInfillGradient>(infill, "AdaptiveInfillGradient",
                                      "How adaptive density changes from surface to interior.")
        .value("Linear", AdaptiveInfillGradient::Linear)
        .value("Quadratic", AdaptiveInfillGradient::Quadratic)
        .value("Exponential", AdaptiveInfillGradient::Exponential);

    py::enum_<SurfaceFillOrder>(infill, "SurfaceFillOrder",
                                "Print order of center-based surface fill loops/fragments.")
        .value("Default", SurfaceFillOrder::Default)
        .value("Outward", SurfaceFillOrder::Outward)
        .value("Inward", SurfaceFillOrder::Inward);

    py::enum_<CenterOfSurfacePattern>(infill, "CenterOfSurfacePattern",
                                      "What a centered surface pattern centers each fill on.")
        .value("EachSurface", CenterOfSurfacePattern::Each_Surface)
        .value("EachModel", CenterOfSurfacePattern::Each_Model)
        .value("EachAssembly", CenterOfSurfacePattern::Each_Assembly);

    py::class_<InfillPluginFlow>(infill, "Flow",
                                 "Resolved extrusion flow of the fill. width, height and spacing "
                                 "are millimetres; mm3_per_mm is the extrusion cross section.")
        .def_readonly("width", &InfillPluginFlow::width, "Extrusion width, mm.")
        .def_readonly("height", &InfillPluginFlow::height, "Extrusion height, mm.")
        .def_readonly("spacing", &InfillPluginFlow::spacing,
                      "Spacing between extrusion centerlines, mm.")
        .def_readonly("nozzle_diameter", &InfillPluginFlow::nozzle_diameter, "Nozzle diameter, mm.")
        .def_readonly("bridge", &InfillPluginFlow::bridge, "True when this is a bridging flow.")
        .def_readonly("mm3_per_mm", &InfillPluginFlow::mm3_per_mm,
                      "Extrusion cross-section area, mm^3/mm; zero when the flow is unset.");

    py::class_<InfillPluginPattern>(infill, "PatternDescriptor",
                                    "One stable pattern ID advertised by an infill capability. "
                                    "display_name is presentation-only; supported_surfaces declares "
                                    "which surfaces the pattern can fill.")
        .def(py::init([](std::string id, std::string display_name,
                         std::vector<InfillSurfacePurpose> supported_surfaces) {
                 if (display_name.empty())
                     display_name = id;
                 return InfillPluginPattern{std::move(id), std::move(display_name), std::move(supported_surfaces)};
             }),
             py::arg("id"), py::arg("display_name") = "",
             py::arg("supported_surfaces") = std::vector<InfillSurfacePurpose>{ InfillSurfacePurpose::Sparse })
        .def_readwrite("id", &InfillPluginPattern::id)
        .def_readwrite("display_name", &InfillPluginPattern::display_name)
        .def_readwrite("supported_surfaces", &InfillPluginPattern::supported_surfaces);

    py::class_<InfillPluginRequest>(infill, "InfillPluginRequest",
                                    "Value snapshot of one infill generation call. Mirrors every "
                                    "resolved FillParams value except the live config pointer, so it "
                                    "contains no references into the print. Units: geometry, "
                                    "horiz_move, symmetric_y_axis, infill_lock_depth and "
                                    "skin_infill_depth are scaled integer coordinates; lengths are "
                                    "millimetres; angles are radians. Safe to keep after generate() "
                                    "returns.")
        .def_property_readonly(
            "expolygon",
            [](const InfillPluginRequest& request) -> const ExPolygon& { return request.expolygon; },
            py::return_value_policy::reference_internal,
            "Region to fill, already offset by Fill for the infill/perimeter overlap and translated "
            "into the pattern's working frame. Scaled coordinates.")
        .def_readonly("layer_id", &InfillPluginRequest::layer_id,
                      "Raw Fill layer id: Layer::id() in normal infill (starts at the raft-layer "
                      "count) or the object-relative id while anchoring. Use layer_index for a "
                      "stable object-relative index.")
        .def_readonly("object_id", &InfillPluginRequest::object_id,
                      "PrintObject identity (PrintObject::id().id) of the object owning the fill's "
                      "layer, normally the same value as ctx.object.id(), or -1 when the fill has "
                      "no owning object.")
        .def_readonly("layer_index", &InfillPluginRequest::layer_index,
                      "Zero-based index of the layer in PrintObject::layers() (the object layers, "
                      "excluding raft/support layers), or -1 when unavailable. Unlike layer_id, "
                      "this is object-relative.")
        .def_readonly("region_id", &InfillPluginRequest::region_id,
                      "Index of the representative LayerRegion in Layer::regions(), or -1 when the "
                      "fill has no owning region. For a grouped fill this is the first region whose "
                      "settings were used and the region that receives the generated extrusions; "
                      "see region_ids.")
        .def_readonly("region_ids", &InfillPluginRequest::region_ids,
                      "Every LayerRegion that contributed surfaces to this fill, representative "
                      "region first. A non-grouped fill has one entry. Empty when unavailable.")
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
        .def_readonly("flow", &InfillPluginRequest::flow,
                      "Resolved extrusion flow of the fill (orca.infill.Flow).")
        .def_readonly("flow_width", &InfillPluginRequest::flow_width,
                      "Shortcut for flow.width; extrusion width, mm.")
        .def_readonly("flow_height", &InfillPluginRequest::flow_height,
                      "Shortcut for flow.height; extrusion height, mm.")
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
        .def_readonly("gyroid_optimized", &InfillPluginRequest::gyroid_optimized,
                      "Use the parameterized Gyroid variant.")
        .def_readonly("adaptive_infill_mode", &InfillPluginRequest::adaptive_infill_mode,
                      "Graded adaptive infill mode (orca.infill.AdaptiveInfillMode).")
        .def_readonly("adaptive_infill_interior_density",
                      &InfillPluginRequest::adaptive_infill_interior_density,
                      "Adaptive infill density at the deepest point, fraction in <0,1>.")
        .def_readonly("adaptive_infill_gradient", &InfillPluginRequest::adaptive_infill_gradient,
                      "Adaptive density gradient (orca.infill.AdaptiveInfillGradient).")
        .def_readonly("lateral_lattice_angle_1", &InfillPluginRequest::lateral_lattice_angle_1,
                      "Lateral lattice tilt of the first wall, degrees.")
        .def_readonly("lateral_lattice_angle_2", &InfillPluginRequest::lateral_lattice_angle_2,
                      "Lateral lattice tilt of the second wall, degrees.")
        .def_readonly("pattern", &InfillPluginRequest::pattern,
                      "Host infill pattern of the call (orca.infill.InfillPattern). Plugin dispatch "
                      "reports InfillPattern.Plugin; the selected plugin pattern is the pattern_id "
                      "argument of generate().")
        .def_readonly("infill_overhang_angle", &InfillPluginRequest::infill_overhang_angle,
                      "Lateral honeycomb overhang angle, degrees.")
        .def_readonly("extrusion_role", &InfillPluginRequest::extrusion_role,
                      "Extrusion role this fill will be emitted as (orca.infill.ExtrusionRole).")
        .def_readonly("using_internal_flow", &InfillPluginRequest::using_internal_flow,
                      "True when the requested internal (sparse) flow is in use.")
        .def_readonly("no_extrusion_overlap", &InfillPluginRequest::no_extrusion_overlap,
                      "Overlap that keeps new top-surface extrusions off existing ones, mm.")
        .def_readonly("dont_sort", &InfillPluginRequest::dont_sort,
                      "Do not sort the lines; keep them connected as generated.")
        .def_readonly("can_reverse", &InfillPluginRequest::can_reverse,
                      "Paths may be reversed to join their ends.")
        .def_readonly("fill_order", &InfillPluginRequest::fill_order,
                      "Surface fill loop order (orca.infill.SurfaceFillOrder).")
        .def_readonly("horiz_move", &InfillPluginRequest::horiz_move,
                      "Per-layer pattern stagger offset, scaled coordinates.")
        .def_readonly("symmetric_infill_y_axis", &InfillPluginRequest::symmetric_infill_y_axis,
                      "The region was mirrored about symmetric_y_axis.")
        .def_readonly("infill_complete_top", &InfillPluginRequest::infill_complete_top,
                      "3D honeycomb: complete the top layer's cells.")
        .def_readonly("symmetric_y_axis", &InfillPluginRequest::symmetric_y_axis,
                      "Scaled x center the region was mirrored about when "
                      "symmetric_infill_y_axis is set.")
        .def_readonly("locked_zag", &InfillPluginRequest::locked_zag,
                      "Locked-zag contour locking requested.")
        .def_readonly("infill_lock_depth", &InfillPluginRequest::infill_lock_depth,
                      "Locked-zag lock depth, scaled coordinates.")
        .def_readonly("skin_infill_depth", &InfillPluginRequest::skin_infill_depth,
                      "Locked-zag skin depth, scaled coordinates.")
        .def_readonly("center_of_surface_pattern", &InfillPluginRequest::center_of_surface_pattern,
                      "Centered-surface grouping (orca.infill.CenterOfSurfacePattern).")
        .def_readonly("purpose", &InfillPluginRequest::purpose,
                      "Surface purpose that selected this pattern for the call.")
        .def_property_readonly("full_infill", &InfillPluginRequest::full_infill,
                               "True when density is effectively 100%.")
        .def_property_readonly("dont_connect", &InfillPluginRequest::dont_connect,
                               "True when infill paths must not be anchored to perimeters.");

    py::class_<InfillPluginCapability, PluginCapabilityInterface,
               PyInfillPluginCapabilityTrampoline,
               std::shared_ptr<InfillPluginCapability>>(infill, "InfillPluginCapabilityBase")
        .def(py::init<>())
        .def("get_type", &InfillPluginCapability::get_type)
        .def("patterns", [](const InfillPluginCapability& self) { return self.patterns(); },
             "Optional, default: one sparse pattern whose ID is get_name(). Return a sequence of "
             "orca.infill.PatternDescriptor values. Each ID is a stable, nonlocalized pattern key; "
             "IDs must be unique within the provider and across loaded infill providers, and must "
             "not collide with a built-in pattern key. Supported surfaces are declared per pattern.")
        .def("no_sort", &InfillPluginCapability::no_sort, py::arg("pattern_id"),
             "Optional, default False. Return True to keep Fill's extrusion collection unsorted, "
             "preserving the order of the paths generate() returns through the G-code path planner.")
        .def("aligned_to_origin", &InfillPluginCapability::aligned_to_origin, py::arg("pattern_id"),
             "Optional, default False. Return True when the pattern is laid out around the Fill "
             "bounding-box center rather than the island's own coordinates: Fill::fill_surface() "
             "shifts each island to the origin before generate() and shifts the returned paths "
             "back. The request expolygon is in that shifted frame.")
        .def(
            "generate",
            [](InfillPluginCapability& self, const std::string& pattern_id,
               const InfillPluginRequest& request) {
                return infill_paths_to_python(self.generate(pattern_id, request));
            },
            py::arg("pattern_id"), py::arg("request"),
            "Generate infill for one surface island of the named pattern. Runs synchronously on "
            "the slicing worker thread; do not call UI from here. Return a sequence of open paths, "
            "each an (N,2) int64 ndarray in scaled coordinates, an orca.host.Polygon, or a "
            "sequence of orca.host.Point. Paths are returned as-is: Fill converts them to "
            "extrusions, applies flow, and runs gap fill; it does not clip them again, so stay "
            "inside request.expolygon.");
}

Fill* InfillPluginCapability::create_fill(
    std::shared_ptr<InfillPluginCapability> self, std::string pattern_id) const
{
    return new InfillPluginFill(std::move(self), std::move(pattern_id));
}

InfillPluginFill::InfillPluginFill(std::shared_ptr<InfillPluginCapability> capability,
                                   std::string                            pattern_id)
    : m_capability(std::move(capability)), m_pattern_id(std::move(pattern_id))
{
    if (!m_capability)
        throw InvalidArgument("InfillPluginFill requires a capability");
    if (m_pattern_id.empty())
        throw InvalidArgument("InfillPluginFill requires a pattern ID");
}

bool InfillPluginFill::no_sort() const { return m_capability->no_sort(m_pattern_id); }

bool InfillPluginFill::aligned_to_origin() const { return m_capability->aligned_to_origin(m_pattern_id); }

Fill* InfillPluginFill::clone() const
{
    auto clone = std::make_unique<InfillPluginFill>(m_capability, m_pattern_id);
    clone->layer_id                      = layer_id;
    clone->object_id                     = object_id;
    clone->layer_index                   = layer_index;
    clone->region_id                     = region_id;
    clone->region_ids                    = region_ids;
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
        make_infill_plugin_request(*this, params, thickness_layers, direction, std::move(expolygon));

    Polylines generated;
    try {
        generated = m_capability->generate(m_pattern_id, request);
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
