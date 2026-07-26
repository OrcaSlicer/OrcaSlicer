#include <catch2/catch_test_macros.hpp>
#include "slic3r/plugin/PythonPluginInterface.hpp"
using namespace Slic3r;

TEST_CASE("SlicingPipeline capability-type string maps round-trip", "[slicing_pipeline]") {
    CHECK(plugin_capability_type_to_string(PluginCapabilityType::SlicingPipeline) == "slicing-pipeline");
    CHECK(plugin_capability_type_display_name(PluginCapabilityType::SlicingPipeline) == "Slicing Pipeline");
    CHECK(plugin_capability_type_from_string("slicing-pipeline") == PluginCapabilityType::SlicingPipeline);
    CHECK(plugin_capability_type_from_string("SLICING-PIPELINE") == PluginCapabilityType::SlicingPipeline);
    CHECK(plugin_capability_type_from_string("nope") == PluginCapabilityType::Unknown);
}

#include "python_test_support.hpp"
#include "slic3r/plugin/PluginBindingUtils.hpp"
#include "slic3r/plugin/pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pybind11/embed.h>
#include <pybind11/numpy.h>
namespace py = pybind11;

TEST_CASE("make_readonly_rows builds a read-only (N,2) int64 view", "[slicing_pipeline]") {
    ensure_python_initialized(); // helper already used by test_plugin_host_api.cpp
    py::gil_scoped_acquire gil;

    // make_readonly_rows() constructs a py::array_t, which requires numpy to be
    // importable in the embedded interpreter. The unit-test interpreter ships no
    // site-packages (same condition test_plugin_host_api.cpp's TriangleMesh numpy
    // test guards against), so skip the array-backed assertions when numpy is
    // unavailable there rather than fail on an environment quirk.
    bool have_numpy = false;
    try {
        py::module_::import("numpy");
        have_numpy = true;
    } catch (const py::error_already_set&) {
        have_numpy = false;
    }
    if (!have_numpy) {
        SKIP("numpy unavailable in unit-test interpreter");
    }

    static Slic3r::Points pts = { Slic3r::Point(10, 20), Slic3r::Point(30, 40) };
    py::capsule keepalive(&pts, [](void*){});
    py::array a = Slic3r::make_readonly_rows<coord_t, 2>(keepalive, pts.front().data(), (py::ssize_t)pts.size());
    CHECK(a.dtype().kind() == 'i');
    CHECK(a.itemsize() == 8);           // int64
    CHECK(a.shape(0) == 2);
    CHECK(a.shape(1) == 2);
    CHECK_FALSE(a.writeable());
    auto r = a.unchecked<coord_t, 2>();
    CHECK(r(0,0) == 10); CHECK(r(1,1) == 40);
}

TEST_CASE("make_writable_rows builds a writable (N,2) int64 view that aliases the buffer", "[slicing_pipeline]") {
    ensure_python_initialized();
    py::gil_scoped_acquire gil;
    bool have_numpy = false;
    try { py::module_::import("numpy"); have_numpy = true; }
    catch (const py::error_already_set&) { have_numpy = false; }
    if (!have_numpy) SKIP("numpy unavailable in unit-test interpreter");

    static Slic3r::Points pts = { Slic3r::Point(10, 20), Slic3r::Point(30, 40) };
    py::capsule keepalive(&pts, [](void*){});
    py::array a = Slic3r::make_writable_rows<coord_t, 2>(keepalive, pts.front().data(), (py::ssize_t)pts.size());
    CHECK(a.writeable());
    // Writing through the view mutates the C++ buffer (zero-copy alias).
    a.attr("__setitem__")(py::make_tuple(0, 0), py::int_(99));
    CHECK(pts.front().x() == 99);
}

TEST_CASE("orca.slicing module: Step enum, context, and a Python capability can execute", "[slicing_pipeline]") {
    ensure_python_initialized();
    import_orca_module(); // forces PythonPluginBridge::instance() (see import_orca_module in python_test_support.hpp)
    py::gil_scoped_acquire gil;
    py::module_ orca = py::module_::import("orca");
    REQUIRE(py::hasattr(orca, "slicing"));
    py::object slicing = orca.attr("slicing");
    CHECK(py::hasattr(slicing, "Step"));
    CHECK(py::hasattr(slicing.attr("Step"), "posSlice"));
    CHECK(py::hasattr(slicing.attr("Step"), "psGCodePostProcess"));
    CHECK(py::hasattr(slicing, "SlicingPipelineContext"));
    CHECK(py::hasattr(slicing, "SlicingPipelineCapabilityBase"));

    // A trivial Python subclass whose execute() reports success, invoked via the C++ trampoline.
    py::exec(R"(
import orca
class Probe(orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self): return "probe"
    def execute(self, ctx): return orca.ExecutionResult.success("ok")
_probe = Probe()
    )");
    // (Full C++ trampoline invocation with a real context is exercised elsewhere.)
}

TEST_CASE("orca.slicing is workflow-only: context exposes raw print/object; view classes are gone", "[slicing_pipeline]") {
    using Catch::Matchers::WithinRel;
    ensure_python_initialized();
    import_orca_module();
    py::gil_scoped_acquire gil;
    py::module_ orca = py::module_::import("orca");
    py::object slicing = orca.attr("slicing");

    // Context surface: raw graph entry points + workflow accessors.
    for (const char* name : { "print", "object", "config_value", "cancelled",
                              "orca_version", "step" })
        CHECK(py::hasattr(slicing.attr("SlicingPipelineContext"), name));

    // The wrapper layer is gone.
    for (const char* legacy : { "ExPolygonView", "SurfaceView", "LayerRegionView",
                                "LayerView", "PrintObjectView", "PathData", "SurfaceType" })
        CHECK_FALSE(py::hasattr(slicing, legacy));

    // unscale() stays in orca.slicing and reads the live SCALING_FACTOR.
    const coord_t scaled10 = (coord_t) scale_(10.0);
    double mm = slicing.attr("unscale")(scaled10).cast<double>();
    CHECK_THAT(mm, WithinRel(10.0, 1e-9));

    // A default context casts print/object to None (no dangling wrapper).
    Slic3r::SlicingPipelineContext ctx;
    py::object pyctx = py::cast(&ctx, py::return_value_policy::reference);
    CHECK(pyctx.attr("print").is_none());
    CHECK(pyctx.attr("object").is_none());
}

#include "libslic3r/PrintConfig.hpp"   // DynamicPrintConfig for the psGCodePostProcess context
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <sstream>

// psGCodePostProcess is the merged post-processing seam: no live Print (print/object are None), the
// plugin edits the file at ctx.gcode_path in place, and ctx.config_value() falls back to the config
// the export path handed in. Exercising the real bindings by calling the Python execute() directly
// (not the C++ audit trampoline) keeps this a pure binding-surface test.
TEST_CASE("orca.slicing psGCodePostProcess context: file edit in place + config fallback", "[slicing_pipeline]") {
    namespace fs = boost::filesystem;
    ensure_python_initialized();
    import_orca_module();
    py::gil_scoped_acquire gil;

    const fs::path gpath = fs::temp_directory_path() / fs::unique_path("orca_pp_%%%%-%%%%.gcode");
    {
        boost::nowide::ofstream ofs(gpath.string());
        ofs << "; header\nG1 X0 Y0\n";
    }

    // Config the plugin reads back through ctx.config_value() (there is no live Print at this step).
    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_key_value("layer_height", new Slic3r::ConfigOptionFloat(0.2));

    Slic3r::SlicingPipelineContext ctx;
    ctx.orca_version = "test";
    ctx.step         = Slic3r::SlicingPipelineStepPlugin::psGCodePostProcess;
    ctx.gcode_path   = gpath.string();
    ctx.host         = "File";
    ctx.output_name  = "final.gcode";
    ctx.full_config  = &config;   // print stays null

    py::object pyctx = py::cast(&ctx, py::return_value_policy::reference);
    CHECK(pyctx.attr("gcode_path").cast<std::string>() == gpath.string());
    CHECK(pyctx.attr("host").cast<std::string>() == "File");
    CHECK(pyctx.attr("output_name").cast<std::string>() == "final.gcode");
    CHECK(pyctx.attr("print").is_none());
    CHECK(pyctx.attr("object").is_none());
    CHECK(pyctx.attr("step").cast<Slic3r::SlicingPipelineStepPlugin>()
          == Slic3r::SlicingPipelineStepPlugin::psGCodePostProcess);
    CHECK_FALSE(pyctx.attr("cancelled")().cast<bool>());   // null print -> not cancelled
    // config_value() resolves from full_config when print is null; unknown keys are None.
    CHECK_FALSE(pyctx.attr("config_value")("layer_height").is_none());
    CHECK(pyctx.attr("config_value")("this_key_does_not_exist").is_none());

    // A Python capability edits the file in place through ctx.gcode_path. Calling execute() directly
    // in Python dispatches to the Python method (no C++ trampoline), so this needs no audit context.
    py::module_ main = py::module_::import("__main__");
    main.attr("_pp_ctx") = pyctx;
    py::exec(R"(
import orca
class Stamp(orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self): return "stamp"
    def execute(self, ctx):
        assert ctx.step == orca.slicing.Step.psGCodePostProcess
        assert ctx.print is None and ctx.object is None
        with open(ctx.gcode_path, "a") as f:
            f.write("; stamped by " + ctx.host + "\n")
        return orca.ExecutionResult.success("ok")
_pp_result = Stamp().execute(_pp_ctx)
    )");
    CHECK(main.attr("_pp_result").attr("message").cast<std::string>() == std::string("ok"));

    std::string contents;
    {
        boost::nowide::ifstream ifs(gpath.string());
        std::stringstream ss; ss << ifs.rdbuf(); contents = ss.str();
    }
    CHECK(contents.find("; stamped by File") != std::string::npos);
    fs::remove(gpath);
}

// ---------------------------------------------------------------------------
// Toolpath helpers for the raw-graph tests.
//
// LayerRegion's ctor is protected (constructed only by Layer/PrintObject). A
// trivial derived struct lets a unit test build one with null layer/region
// pointers — the extrusion accessors only read the public `perimeters`/`fills`
// collections, never the layer/region back-pointers.
// ---------------------------------------------------------------------------
namespace {
struct TestLayerRegion : Slic3r::LayerRegion {
    TestLayerRegion() : Slic3r::LayerRegion(nullptr, nullptr) {}
};

// Build a realistic nested perimeters collection into `region.perimeters`:
//   perimeters (outer) -> inner collection -> [ ExtrusionLoop(pathA), ExtrusionPath(pathB) ]
// This exercises both the recursive descent through nested collections and the
// decomposition of an ExtrusionLoop into its contained ExtrusionPath (flatten()
// does NOT decompose loops, hence the hand-rolled recursive walk).
static void build_nested_perimeters(TestLayerRegion& region) {
    using namespace Slic3r;
    ExtrusionPath pathA(erExternalPerimeter);        // -> "Outer wall"
    pathA.mm3_per_mm = 0.05; pathA.width = 0.45f; pathA.height = 0.20f;
    pathA.polyline.points = { Point3(0, 0, 0), Point3(10, 0, 0), Point3(10, 10, 0) };

    ExtrusionPath pathB(erInternalInfill);           // -> "Sparse infill"
    pathB.mm3_per_mm = 0.03; pathB.width = 0.40f; pathB.height = 0.20f;
    pathB.polyline.points = { Point3(1, 1, 0), Point3(2, 1, 0), Point3(2, 2, 0) };

    ExtrusionEntityCollection inner;
    inner.append(ExtrusionLoop(pathA));              // clone_move
    inner.append(pathB);                             // clone
    region.perimeters.append(inner);                 // nested (deep clone)
}
} // namespace

// ---------------------------------------------------------------------------
// Raw Print-graph data model (orca.host) — replaces the *View wrapper API.
// LIFETIME: raw bindings follow C++ semantics — references into the slicing
// graph are valid during execute(ctx) and invalidated by container-replacing
// mutators, exactly like std::vector iterators.
// ---------------------------------------------------------------------------
TEST_CASE("orca.host leaf geometry: Surface/ExPolygon/Polygon raw bindings", "[slicing_pipeline]") {
    using Catch::Matchers::WithinRel;
    using Catch::Matchers::WithinAbs;
    ensure_python_initialized();
    import_orca_module();
    py::gil_scoped_acquire gil;
    py::object host = py::module_::import("orca").attr("host");

    for (const char* name : { "SurfaceType", "Polygon", "ExPolygon", "Surface", "SurfaceCollection" })
        CHECK(py::hasattr(host, name));

    // SurfaceType enum values round-trip to the C++ enumerators (moved from orca.slicing).
    py::object ST = host.attr("SurfaceType");
    CHECK(ST.attr("stTop").cast<Slic3r::SurfaceType>()           == Slic3r::stTop);
    CHECK(ST.attr("stInternalSolid").cast<Slic3r::SurfaceType>() == Slic3r::stInternalSolid);
    CHECK(ST.attr("stPerimeter").cast<Slic3r::SurfaceType>()     == Slic3r::stPerimeter);

    // Raw Surface: scalar reads + WRITABLE surface_type (replaces SurfaceView.set_type).
    Slic3r::Surface surf(Slic3r::stInternalSolid);
    surf.thickness        = 0.4;
    surf.bridge_angle     = -1.0;
    surf.extra_perimeters = 2;
    py::object sv = py::cast(&surf, py::return_value_policy::reference);
    CHECK(sv.attr("surface_type").cast<Slic3r::SurfaceType>() == Slic3r::stInternalSolid);
    CHECK_THAT(sv.attr("thickness").cast<double>(), WithinRel(0.4, 1e-9));
    CHECK_THAT(sv.attr("bridge_angle").cast<double>(), WithinAbs(-1.0, 1e-12));
    CHECK(sv.attr("extra_perimeters").cast<int>() == 2);
    sv.attr("surface_type") = host.attr("SurfaceType").attr("stTop");
    CHECK(surf.surface_type == Slic3r::stTop);   // C++ side reflects the assignment

    // ExPolygon navigation without numpy: contour is a Polygon, holes an empty list.
    py::object exv = sv.attr("expolygon");
    CHECK(py::hasattr(exv, "contour"));
    CHECK(exv.attr("holes").cast<py::list>().size() == 0);
    CHECK(exv.attr("contour").attr("size")().cast<size_t>() == 0);
}

TEST_CASE("orca.host Surface/SurfaceCollection: construct, writable members, set()", "[slicing_pipeline]") {
    using Catch::Matchers::WithinRel;
    ensure_python_initialized();
    import_orca_module();
    py::gil_scoped_acquire gil;
    py::object host = py::module_::import("orca").attr("host");
    py::object ST = host.attr("SurfaceType");
    const coord_t s = (coord_t) scale_(10.0);

    // Build an ExPolygon (Point idiom) and a Surface from it.
    py::object P = host.attr("Polygon")();
    P.attr("append")(host.attr("Point")(0, 0));
    P.attr("append")(host.attr("Point")(s, 0));
    P.attr("append")(host.attr("Point")(s, s));
    P.attr("append")(host.attr("Point")(0, s));
    py::object ex = host.attr("ExPolygon")(P);
    py::object surf = host.attr("Surface")(ST.attr("stTop"), ex);
    CHECK(surf.attr("surface_type").cast<Slic3r::SurfaceType>() == Slic3r::stTop);
    CHECK(surf.attr("is_top")().cast<bool>());
    CHECK_THAT(surf.attr("area")().cast<double>(), WithinRel((double) s * (double) s, 1e-9));
    surf.attr("thickness") = py::float_(0.3);
    CHECK_THAT(surf.attr("thickness").cast<double>(), WithinRel(0.3, 1e-9));

    // SurfaceCollection.set(expolys, type): replace all surfaces from a list of ExPolygon tagged with one SurfaceType.
    Slic3r::SurfaceCollection coll;
    py::object cv = py::cast(&coll, py::return_value_policy::reference);
    py::list expolys; expolys.append(ex);
    cv.attr("set")(expolys, ST.attr("stInternalSolid"));
    REQUIRE(coll.surfaces.size() == 1);
    CHECK(coll.surfaces.front().surface_type == Slic3r::stInternalSolid);
    CHECK(cv.attr("has")(ST.attr("stInternalSolid")).cast<bool>());
    cv.attr("clear")();
    CHECK(coll.surfaces.empty());
}

TEST_CASE("orca.host Point: construct, read/write coords, arithmetic", "[slicing_pipeline]") {
    ensure_python_initialized();
    import_orca_module();
    py::gil_scoped_acquire gil;
    py::object host = py::module_::import("orca").attr("host");
    REQUIRE(py::hasattr(host, "Point"));
    py::object p = host.attr("Point")(3, 4);
    CHECK(p.attr("x").cast<coord_t>() == 3);
    CHECK(p.attr("y").cast<coord_t>() == 4);
    p.attr("x") = py::int_(7);
    CHECK(p.attr("x").cast<coord_t>() == 7);
    py::object q = host.attr("Point")(1, 2);
    py::object sum = p.attr("__add__")(q);
    CHECK(sum.attr("x").cast<coord_t>() == 8);
    CHECK(sum.attr("y").cast<coord_t>() == 6);

    // __mul__ must scale as a double, not truncate to int64 before multiplying.
    py::object h = host.attr("Point")(10, 20).attr("__mul__")(py::float_(0.5));
    CHECK(h.attr("x").cast<coord_t>() == 5);
    CHECK(h.attr("y").cast<coord_t>() == 10);
}

TEST_CASE("orca.host Polygon: writable as_array aliases buffer; Point refs; set_points; offset", "[slicing_pipeline]") {
    using Catch::Matchers::WithinRel;
    ensure_python_initialized();
    import_orca_module();
    py::gil_scoped_acquire gil;
    py::object host = py::module_::import("orca").attr("host");

    const coord_t s = (coord_t) scale_(10.0);
    Slic3r::Polygon poly;
    poly.points = { Slic3r::Point(0, 0), Slic3r::Point(s, 0), Slic3r::Point(s, s), Slic3r::Point(0, s) };
    py::object pv = py::cast(&poly, py::return_value_policy::reference);

    // Non-array surface works without numpy.
    CHECK(pv.attr("size")().cast<size_t>() == 4);
    CHECK(pv.attr("is_counter_clockwise")().cast<bool>());
    CHECK_THAT(pv.attr("area")().cast<double>(), WithinRel((double) s * (double) s, 1e-9));
    // Point-object idiom: editing a returned Point ref mutates the buffer in place.
    py::list pts = pv.attr("points").cast<py::list>();
    REQUIRE(pts.size() == 4);
    pts[0].attr("x") = py::int_(5);
    CHECK(poly.points[0].x() == 5);
    poly.points[0].x() = 0; // restore

    // offset() returns new geometry (ClipperUtils bound as a method).
    py::list shrunk = pv.attr("offset")(py::int_(-(coord_t)scale_(1.0))).cast<py::list>();
    CHECK(shrunk.size() >= 1);

    bool have_numpy = false;
    try { py::module_::import("numpy"); have_numpy = true; }
    catch (const py::error_already_set&) { have_numpy = false; }
    if (!have_numpy) SKIP("numpy unavailable: array-backed assertions skipped");

    py::module_ np = py::module_::import("numpy");
    py::array a = pv.attr("as_array")().cast<py::array>();
    CHECK(a.dtype().kind() == 'i');
    CHECK(a.itemsize() == 8);
    CHECK(a.shape(0) == 4);
    CHECK(a.shape(1) == 2);
    CHECK(a.writeable());                              // writable now
    a.attr("__setitem__")(py::make_tuple(0, 0), py::int_(123));
    CHECK(poly.points[0].x() == 123);                 // in-place bulk edit
    // set_points replaces contents (count-changing).
    py::object i64 = np.attr("int64");
    py::list rows;
    rows.append(py::make_tuple(0, 0)); rows.append(py::make_tuple(s, 0)); rows.append(py::make_tuple(s, s));
    pv.attr("set_points")(np.attr("array")(rows, py::arg("dtype") = i64));
    CHECK(poly.points.size() == 3);
}

TEST_CASE("orca.host ExPolygon: construct, writable contour/holes, transforms, boolean ops", "[slicing_pipeline]") {
    using Catch::Matchers::WithinRel;
    ensure_python_initialized();
    import_orca_module();
    py::gil_scoped_acquire gil;
    py::object host = py::module_::import("orca").attr("host");
    const coord_t s = (coord_t) scale_(10.0);

    // Construct from Polygon objects (Point idiom, no numpy).
    py::object P = host.attr("Polygon")();
    P.attr("append")(host.attr("Point")(0, 0));
    P.attr("append")(host.attr("Point")(s, 0));
    P.attr("append")(host.attr("Point")(s, s));
    P.attr("append")(host.attr("Point")(0, s));
    py::object ex = host.attr("ExPolygon")(P);
    CHECK_THAT(ex.attr("area")().cast<double>(), WithinRel((double) s * (double) s, 1e-9));
    CHECK(ex.attr("num_contours")().cast<size_t>() == 1);
    CHECK(ex.attr("contour").attr("size")().cast<size_t>() == 4);

    // In-place transform mutates the geometry.
    ex.attr("translate")(py::float_(1000.0), py::float_(0.0));
    // Boolean op returns new geometry: A minus a smaller inset of A is a non-empty ring set.
    py::list inset = ex.attr("offset")(py::int_(-(coord_t)scale_(1.0))).cast<py::list>();
    REQUIRE(inset.size() >= 1);
    py::list ring = ex.attr("diff_ex")(inset[0]).cast<py::list>();
    CHECK(ring.size() >= 1);
}

namespace {
// Nested collection: outer -> inner -> [ ExtrusionLoop(pathA), ExtrusionPath(pathB) ].
// Exercises polymorphic downcast of .entities and loop decomposition in flatten_paths().
static Slic3r::ExtrusionEntityCollection build_nested_collection() {
    using namespace Slic3r;
    ExtrusionPath pathA(erExternalPerimeter);        // -> "Outer wall"
    pathA.mm3_per_mm = 0.05; pathA.width = 0.45f; pathA.height = 0.20f;
    pathA.polyline.points = { Point3(0, 0, 0), Point3(10, 0, 0), Point3(10, 10, 0) };

    ExtrusionPath pathB(erInternalInfill);           // -> "Sparse infill"
    pathB.mm3_per_mm = 0.03; pathB.width = 0.40f; pathB.height = 0.20f;
    pathB.polyline.points = { Point3(1, 1, 0), Point3(2, 1, 0), Point3(2, 2, 0) };

    ExtrusionEntityCollection inner;
    inner.append(ExtrusionLoop(pathA));
    inner.append(pathB);
    ExtrusionEntityCollection outer;
    outer.append(inner);
    return outer;
}
} // namespace

TEST_CASE("orca.host extrusion tree: polymorphic entities + flatten_paths", "[slicing_pipeline]") {
    using Catch::Matchers::WithinRel;
    ensure_python_initialized();
    import_orca_module();
    py::gil_scoped_acquire gil;
    py::object host = py::module_::import("orca").attr("host");
    for (const char* name : { "ExtrusionEntity", "ExtrusionPath", "ExtrusionLoop",
                              "ExtrusionMultiPath", "ExtrusionEntityCollection", "PrintRegion" })
        CHECK(py::hasattr(host, name));

    Slic3r::ExtrusionEntityCollection outer = build_nested_collection();
    py::object coll = py::cast(&outer, py::return_value_policy::reference);

    // .entities downcasts: the single child is a collection; ITS children are a loop + a path.
    py::list kids = coll.attr("entities").cast<py::list>();
    REQUIRE(kids.size() == 1);
    py::list inner_kids = kids[0].attr("entities").cast<py::list>();
    REQUIRE(inner_kids.size() == 2);
    CHECK(py::hasattr(inner_kids[0], "paths"));      // ExtrusionLoop binding
    CHECK(py::hasattr(inner_kids[1], "width"));      // ExtrusionPath binding

    // flatten_paths: loop decomposed, scalars readable.
    py::list ps = coll.attr("flatten_paths")().cast<py::list>();
    REQUIRE(ps.size() == 2);
    CHECK(ps[0].attr("role").cast<std::string>() == "Outer wall");
    CHECK_THAT(ps[0].attr("width").cast<double>(),      WithinRel(0.45, 1e-6));
    CHECK_THAT(ps[0].attr("mm3_per_mm").cast<double>(), WithinRel(0.05, 1e-9));
    CHECK(ps[1].attr("role").cast<std::string>() == "Sparse infill");
}

TEST_CASE("orca.host ExtrusionPath.points() is a read-only (N,3) int64 view", "[slicing_pipeline]") {
    ensure_python_initialized();
    import_orca_module();
    py::gil_scoped_acquire gil;
    bool have_numpy = false;
    try { py::module_::import("numpy"); have_numpy = true; }
    catch (const py::error_already_set&) { have_numpy = false; }
    if (!have_numpy) SKIP("numpy unavailable in unit-test interpreter");

    Slic3r::ExtrusionEntityCollection outer = build_nested_collection();
    py::object coll = py::cast(&outer, py::return_value_policy::reference);
    py::list ps = coll.attr("flatten_paths")().cast<py::list>();
    REQUIRE(ps.size() == 2);
    py::array pts = ps[1].attr("points")().cast<py::array>();  // pathB: (1,1,0),(2,1,0),(2,2,0)
    CHECK(pts.dtype().kind() == 'i');
    CHECK(pts.itemsize() == 8);
    CHECK(pts.shape(0) == 3);
    CHECK(pts.shape(1) == 3);
    CHECK_FALSE(pts.writeable());
    auto r = pts.cast<py::array_t<coord_t>>().unchecked<2>();
    CHECK(r(0, 0) == 1); CHECK(r(1, 0) == 2); CHECK(r(2, 1) == 2);
}

// ---------------------------------------------------------------------------
// Raw Print-graph spine (orca.host): LayerRegion / Layer / PrintObject / Print,
// read side. LayerRegion/Layer ctors are protected (friend class PrintObject),
// so the tests use tiny derived structs -- the pattern TestLayerRegion above
// already establishes; TestLayer is its Layer counterpart.
// ---------------------------------------------------------------------------
namespace {
struct TestLayer : Slic3r::Layer {
    // id=0, no owning PrintObject, height/print_z/slice_z suitable for assertions.
    TestLayer() : Slic3r::Layer(0, nullptr, 0.2, 0.45, 0.35) {}
};
} // namespace

TEST_CASE("orca.host graph classes: LayerRegion/Layer raw traversal; Print/PrintObject registered", "[slicing_pipeline]") {
    using Catch::Matchers::WithinRel;
    ensure_python_initialized();
    import_orca_module();
    py::gil_scoped_acquire gil;
    py::object host = py::module_::import("orca").attr("host");

    for (const char* name : { "LayerRegion", "Layer", "PrintObject", "Print" })
        CHECK(py::hasattr(host, name));
    // Members needing a live Print are verified by registration only (slic3rutils
    // cannot build a Print; the fff_print C++ suite covers live-graph behavior).
    for (const char* name : { "layers", "support_layers", "model_object", "id",
                              "bounding_box", "trafo", "config_value", "config_keys" })
        CHECK(py::hasattr(host.attr("PrintObject"), name));
    for (const char* name : { "objects", "model", "config_value", "config_keys", "canceled" })
        CHECK(py::hasattr(host.attr("Print"), name));

    // Raw LayerRegion traversal over a hand-built region.
    TestLayerRegion region;
    region.slices.surfaces.emplace_back(Slic3r::Surface(Slic3r::stInternal));
    build_nested_perimeters(region);   // helper defined earlier in this file
    py::object lr = py::cast(static_cast<Slic3r::LayerRegion*>(&region),
                             py::return_value_policy::reference);
    CHECK(lr.attr("slices").attr("size")().cast<size_t>() == 1);
    CHECK(lr.attr("slices").attr("surfaces").cast<py::list>().size() == 1);
    CHECK(lr.attr("perimeters").attr("flatten_paths")().cast<py::list>().size() == 2);
    CHECK(lr.attr("fills").attr("size")().cast<size_t>() == 0);
    CHECK(lr.attr("layer")().is_none());               // hand-built region has no owning layer

    // Raw Layer scalars + empty traversals on a hand-built layer.
    TestLayer layer;
    py::object ly = py::cast(static_cast<Slic3r::Layer*>(&layer),
                             py::return_value_policy::reference);
    CHECK_THAT(ly.attr("print_z").cast<double>(),  WithinRel(0.45, 1e-9));
    CHECK_THAT(ly.attr("slice_z").cast<double>(),  WithinRel(0.35, 1e-9));
    CHECK_THAT(ly.attr("height").cast<double>(),   WithinRel(0.2, 1e-9));
    CHECK(ly.attr("regions")().cast<py::list>().size() == 0);
    CHECK(ly.attr("lslices")().cast<py::list>().size() == 0);
    CHECK(ly.attr("upper_layer").is_none());
    CHECK(ly.attr("lower_layer").is_none());
}

TEST_CASE("orca.host: plugin-only mutators are gone; class-API editing works", "[slicing_pipeline]") {
    ensure_python_initialized();
    import_orca_module();
    py::gil_scoped_acquire gil;
    py::object host = py::module_::import("orca").attr("host");

    // The three plugin-only mutators were removed in the raw-API realignment.
    CHECK_FALSE(py::hasattr(host.attr("LayerRegion"), "set_slices"));
    CHECK_FALSE(py::hasattr(host.attr("LayerRegion"), "set_fill_surfaces"));
    CHECK_FALSE(py::hasattr(host.attr("Layer"), "set_lslices"));
    // The faithful surface is present.
    CHECK(py::hasattr(host.attr("SurfaceCollection"), "set"));
    CHECK(py::hasattr(host.attr("Layer"), "make_slices"));

    // clear() via the collection on a hand-built region (null owning layer is null-safe).
    TestLayerRegion region;
    region.slices.surfaces.emplace_back(Slic3r::Surface(Slic3r::stInternal));
    py::object lr = py::cast(static_cast<Slic3r::LayerRegion*>(&region), py::return_value_policy::reference);
    lr.attr("slices").attr("clear")();
    CHECK(region.slices.surfaces.empty());
}

TEST_CASE("orca.host: SurfaceCollection.set mutates geometry; lslices via make_slices", "[slicing_pipeline]") {
    using Catch::Matchers::WithinRel;
    ensure_python_initialized();
    import_orca_module();
    py::gil_scoped_acquire gil;
    bool have_numpy = false;
    try { py::module_::import("numpy"); have_numpy = true; }
    catch (const py::error_already_set&) { have_numpy = false; }
    if (!have_numpy) SKIP("numpy unavailable in unit-test interpreter");

    py::object host = py::module_::import("orca").attr("host");
    py::module_ np = py::module_::import("numpy");
    py::object  i64 = np.attr("int64");
    py::object  ST  = host.attr("SurfaceType");
    const coord_t s = (coord_t) scale_(10.0);
    auto arr = [&](std::initializer_list<std::pair<coord_t,coord_t>> pts) {
        py::list rows; for (auto& p : pts) rows.append(py::make_tuple(p.first, p.second));
        return np.attr("array")(rows, py::arg("dtype") = i64);
    };

    // Build an ExPolygon from a CW ndarray; the ctor normalizes to CCW.
    py::object ex = host.attr("ExPolygon")(arr({ {0,0}, {0,s}, {s,s}, {s,0} }));
    CHECK(ex.attr("contour").attr("is_counter_clockwise")().cast<bool>());

    TestLayerRegion region;
    py::object lr = py::cast(static_cast<Slic3r::LayerRegion*>(&region), py::return_value_policy::reference);
    py::list expolys; expolys.append(ex);
    lr.attr("slices").attr("set")(expolys, ST.attr("stInternalSolid"));
    REQUIRE(region.slices.surfaces.size() == 1);
    const Slic3r::Surface& out = region.slices.surfaces.front();
    CHECK(out.surface_type == Slic3r::stInternalSolid);
    CHECK_THAT(out.expolygon.area(), WithinRel((double) s * (double) s, 1e-9));
    // Read geometry back through the class API.
    py::array c = lr.attr("slices").attr("surfaces").cast<py::list>()[0]
                    .attr("expolygon").attr("contour").attr("as_array")().cast<py::array>();
    CHECK(c.shape(0) == 4);

    // lslices are derived: make_slices() re-derives them + refreshes the bbox cache.
    TestLayer layer;
    py::object ly = py::cast(static_cast<Slic3r::Layer*>(&layer), py::return_value_policy::reference);
    // (A hand-built layer has no regions, so make_slices() yields empty lslices — still null-safe.)
    ly.attr("make_slices")();
    CHECK(layer.lslices_bboxes.size() == layer.lslices.size());
}

TEST_CASE("orca.host ExPolygon in-place transforms + SurfaceCollection.append (sample ops)", "[slicing_pipeline]") {
    using Catch::Matchers::WithinRel;
    ensure_python_initialized();
    import_orca_module();
    py::gil_scoped_acquire gil;
    py::object host = py::module_::import("orca").attr("host");
    const coord_t s = (coord_t) scale_(10.0);
    auto make_square = [&]() {
        py::object P = host.attr("Polygon")();
        P.attr("append")(host.attr("Point")(0, 0));
        P.attr("append")(host.attr("Point")(s, 0));
        P.attr("append")(host.attr("Point")(s, s));
        P.attr("append")(host.attr("Point")(0, s));
        return host.attr("ExPolygon")(P);
    };
    const double area0 = (double) s * (double) s;

    // rotate about the square's center preserves area
    py::object ex = make_square();
    py::object center = host.attr("Point")(s / 2, s / 2);
    ex.attr("rotate")(py::float_(1.5707963267948966), center);   // pi/2
    CHECK_THAT(ex.attr("area")().cast<double>(), WithinRel(area0, 1e-6));

    // uniform scale by 2 quadruples area (scale is about the origin)
    py::object ex2 = make_square();
    ex2.attr("scale")(py::float_(2.0));
    CHECK_THAT(ex2.attr("area")().cast<double>(), WithinRel(4.0 * area0, 1e-6));

    // translate preserves area
    py::object ex3 = make_square();
    ex3.attr("translate")(py::float_(1000.0), py::float_(-500.0));
    CHECK_THAT(ex3.attr("area")().cast<double>(), WithinRel(area0, 1e-6));

    // SurfaceCollection.append accumulates surfaces of a second type (the sample write-back path)
    Slic3r::SurfaceCollection coll;
    py::object cv = py::cast(&coll, py::return_value_policy::reference);
    py::list g1; g1.append(make_square());
    cv.attr("set")(g1, host.attr("SurfaceType").attr("stInternalSolid"));
    py::list g2; g2.append(make_square());
    cv.attr("append")(g2, host.attr("SurfaceType").attr("stTop"));
    REQUIRE(coll.surfaces.size() == 2);
    CHECK(coll.surfaces[0].surface_type == Slic3r::stInternalSolid);
    CHECK(coll.surfaces[1].surface_type == Slic3r::stTop);
}

TEST_CASE("orca.host: in-place edit of surface.expolygon through a live collection persists to C++", "[slicing_pipeline]") {
    using Catch::Matchers::WithinRel;
    ensure_python_initialized();
    import_orca_module();
    py::gil_scoped_acquire gil;

    const coord_t s = (coord_t) scale_(10.0);
    // Live LayerRegion holding one surface (a 10mm square at the origin).
    TestLayerRegion region;
    Slic3r::ExPolygon sq;
    sq.contour.points = { Slic3r::Point(0, 0), Slic3r::Point(s, 0),
                          Slic3r::Point(s, s), Slic3r::Point(0, s) };
    region.slices.surfaces.emplace_back(Slic3r::Surface(Slic3r::stInternal, sq));
    py::object lr = py::cast(static_cast<Slic3r::LayerRegion*>(&region),
                             py::return_value_policy::reference);

    // Twistify's path: get the Surface through the live collection, mutate its expolygon in place.
    py::object surf = lr.attr("slices").attr("surfaces").cast<py::list>()[0];
    surf.attr("expolygon").attr("translate")(py::float_(1000.0), py::float_(0.0));

    // The C++-side surface geometry reflects the Python in-place edit (proves the live ref).
    const Slic3r::Surface& out = region.slices.surfaces.front();
    CHECK(out.expolygon.contour.points[0].x() == 1000);   // was 0
    CHECK(out.expolygon.contour.points[0].y() == 0);
    CHECK_THAT(out.expolygon.area(), WithinRel((double) s * (double) s, 1e-9));  // translate preserves area
}
