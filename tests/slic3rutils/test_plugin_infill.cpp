// Infill plugin registration and request contract: a capability advertises stable pattern IDs, the
// host registers every advertised pattern in InfillFactory on load/enable together with the surface
// purposes its descriptor declares, and removes them on disable/unload. The project manifest still
// records the owning capability. Tests here cover the registry side of that identity, the owned
// request snapshot handed to generate(), and the legacy/current Python override signatures, not
// slicing.
//
// As in test_plugin_lifecycle.cpp, each Catch2 test case runs in its own process
// (catch_discover_tests), so the PluginManager and interpreter come up at most once per test.
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <catch2/catch_all.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pybind11/cast.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <chrono>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Fill/InfillFactory.hpp"
#include "libslic3r/Fill/InfillPurpose.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include "slic3r/plugin/pluginTypes/infill/InfillPluginCapability.hpp"

#include "plugin_test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

using Catch::Matchers::WithinAbs;

// Minimal concrete Fill so make_infill_plugin_request() can be exercised without a slicing pipeline.
class RequestSnapshotFill : public Fill
{
public:
    Fill*    clone() const override { return nullptr; }
    bool     is_self_crossing() override { return false; }
};

// Brings the plugin system up for one test and tears it down at the end, as in
// test_plugin_lifecycle.cpp. The interpreter is process-wide and finalized once by
// PythonInterpreterRunEnd in slic3rutils_tests_main.cpp. Declare this FIRST in a test so it is
// destroyed last.
struct ScopedPluginManager
{
    ScopedDataDir python_data_dir{"infill-python"};
    bool          initialized = false;

    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

// A minimal infill plugin. `stem` distinguishes package, module and class names so two fixtures can
// coexist in one discovery pass; `patterns_method` is the Python body of patterns(), or empty for a
// legacy plugin that relies on the host's one-sparse-pattern default. `methods` replaces the default
// modern generate() block when a test needs legacy or flexible override signatures.
std::string make_infill_plugin_source(const std::string& stem,
                                      const std::string& capability_name,
                                      const std::string& patterns_method = {},
                                      const std::string& methods = {})
{
    return std::string{}
        + "# /// script\n"
        + "# requires-python = \">=3.12\"\n"
        + "# dependencies = []\n"
        + "#\n"
        + "# [tool.orcaslicer.plugin]\n"
        + "# name = \"" + capability_name + " Package\"\n"
        + "# description = \"Infill dispatch test fixture.\"\n"
        + "# author = \"tests\"\n"
        + "# version = \"1.0.0\"\n"
        + "# type = \"infill\"\n"
        + "# ///\n"
        + "import orca\n"
        + "\n\n"
        + "class Cap" + stem + "(orca.infill.InfillPluginCapabilityBase):\n"
        + "    def get_name(self):\n"
        + "        return \"" + capability_name + "\"\n"
        + "\n"
        + (patterns_method.empty()
               ? std::string{}
               : "    def patterns(self):\n" + patterns_method + "\n")
        + (methods.empty()
               ? std::string("    def generate(self, pattern_id, request):\n"
                             "        return []\n")
               : methods)
        + "\n\n"
        + "@orca.plugin\n"
        + "class Plugin" + stem + "(orca.base):\n"
        + "    def register_capabilities(self):\n"
        + "        orca.register_capability(Cap" + stem + ")\n";
}

// Loads a plugin and blocks until the detached load worker is done with it.
bool load_and_wait(PluginManager&     manager,
                   const std::string& plugin_key,
                   std::string&       error)
{
    manager.load_plugin(plugin_key, /*skip_deps=*/true);
    return manager.wait_for_plugin_load(plugin_key, std::chrono::seconds(120), error);
}

void write_plugin(const ScopedDataDir& data_dir_guard,
                  const std::string&   stem,
                  const std::string&   capability_name,
                  const std::string&   patterns_method = {},
                  const std::string&   methods = {})
{
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / stem;
    fs::create_directories(plugin_dir);

    std::ofstream out((plugin_dir / (stem + ".py")).string(), std::ios::binary);
    out << make_infill_plugin_source(stem, capability_name, patterns_method, methods);
    out.close();
}

} // namespace

TEST_CASE("The infill request mirrors every FillParams value as an owned snapshot", "[PluginInfill]")
{
    RequestSnapshotFill fill;
    fill.layer_id = 7;
    fill.object_id = 42;
    fill.layer_index = 5;
    fill.region_id = 1;
    fill.region_ids = { 1, 3 };
    fill.z        = 1.25;
    fill.spacing  = 0.45;
    fill.overlap  = 0.1;
    fill.angle    = 0.5f;

    FillParams params;
    params.density                          = 0.2f;
    params.multiline                        = 3;
    params.anchor_length                    = 11.f;
    params.anchor_length_max                = 22.f;
    params.resolution                       = 0.05;
    params.layer_height                     = 0.3;
    params.flow                             = Flow(0.42f, 0.2f, 0.4f);
    params.flow.set_spacing(0.43f);
    params.dont_adjust                      = true;
    params.monotonic                        = true;
    params.complete                         = true;
    params.use_arachne                      = false;
    params.smooth_factor                    = 0.25;
    params.gyroid_optimized                 = true;
    params.adaptive_infill_mode             = AdaptiveInfillMode::NormalZ;
    params.adaptive_infill_interior_density = 0.35f;
    params.adaptive_infill_gradient         = AdaptiveInfillGradient::Quadratic;
    params.lateral_lattice_angle_1          = 15.;
    params.lateral_lattice_angle_2          = 30.;
    params.pattern                          = ipGyroid;
    params.infill_overhang_angle            = 55.f;
    params.extrusion_role                   = erTopSolidInfill;
    params.using_internal_flow              = true;
    params.no_extrusion_overlap             = 0.15f;
    params.dont_sort                        = true;
    params.can_reverse                      = false;
    params.fill_order                       = SurfaceFillOrder::Inward;
    params.horiz_move                       = 1234.f;
    params.symmetric_infill_y_axis          = true;
    params.infill_complete_top              = true;
    params.symmetric_y_axis                 = 9876;
    params.locked_zag                       = true;
    params.infill_lock_depth                = 4321.f;
    params.skin_infill_depth                = 210.f;
    params.center_of_surface_pattern        = CenterOfSurfacePattern::Each_Model;
    params.purpose                          = InfillSurfacePurpose::TopSurface;

    const std::pair<float, Point> direction{ 1.5f, Point(11, 22) };
    const InfillPluginRequest request =
        make_infill_plugin_request(fill, params, 3, direction, ExPolygon{});

    // Per-call context from the Fill and the generation hook.
    CHECK(request.layer_id == 7);
    CHECK(request.object_id == 42);
    CHECK(request.layer_index == 5);
    CHECK(request.region_id == 1);
    CHECK(request.region_ids == std::vector<int>{ 1, 3 });
    CHECK(request.thickness_layers == 3);
    CHECK_THAT(request.z, WithinAbs(1.25, 1e-12));
    CHECK_THAT(request.spacing, WithinAbs(0.45, 1e-12));
    CHECK_THAT(request.overlap, WithinAbs(0.1, 1e-12));
    CHECK_THAT(request.angle, WithinAbs(0.5, 1e-6));
    CHECK_THAT(request.direction_angle, WithinAbs(1.5, 1e-6));
    CHECK(request.direction_origin == Point(11, 22));

    // Every FillParams value except the live config pointer.
    CHECK_THAT(request.density, WithinAbs(0.2, 1e-6));
    CHECK(request.multiline == 3);
    CHECK_THAT(request.anchor_length, WithinAbs(11.0, 1e-6));
    CHECK_THAT(request.anchor_length_max, WithinAbs(22.0, 1e-6));
    CHECK_THAT(request.resolution, WithinAbs(0.05, 1e-12));
    CHECK_THAT(request.layer_height, WithinAbs(0.3, 1e-12));
    CHECK_THAT(request.flow.width, WithinAbs(0.42, 1e-6));
    CHECK_THAT(request.flow.height, WithinAbs(0.2, 1e-6));
    CHECK_THAT(request.flow.spacing, WithinAbs(0.43, 1e-6));
    CHECK_THAT(request.flow.nozzle_diameter, WithinAbs(0.4, 1e-6));
    CHECK_FALSE(request.flow.bridge);
    CHECK(request.flow.mm3_per_mm > 0.0);
    // flow_width/flow_height are float-derived shortcuts, so allow float precision.
    CHECK_THAT(request.flow_width, WithinAbs(0.42, 1e-6));
    CHECK_THAT(request.flow_height, WithinAbs(0.2, 1e-6));
    CHECK(request.dont_adjust == true);
    CHECK(request.monotonic == true);
    CHECK(request.complete == true);
    CHECK(request.use_arachne == false);
    CHECK_THAT(request.smooth_factor, WithinAbs(0.25, 1e-12));
    CHECK(request.gyroid_optimized == true);
    CHECK(request.adaptive_infill_mode == AdaptiveInfillMode::NormalZ);
    CHECK_THAT(request.adaptive_infill_interior_density, WithinAbs(0.35, 1e-6));
    CHECK(request.adaptive_infill_gradient == AdaptiveInfillGradient::Quadratic);
    CHECK_THAT(request.lateral_lattice_angle_1, WithinAbs(15.0, 1e-12));
    CHECK_THAT(request.lateral_lattice_angle_2, WithinAbs(30.0, 1e-12));
    CHECK(request.pattern == ipGyroid);
    CHECK_THAT(request.infill_overhang_angle, WithinAbs(55.0, 1e-6));
    CHECK(request.extrusion_role == erTopSolidInfill);
    CHECK(request.using_internal_flow == true);
    CHECK_THAT(request.no_extrusion_overlap, WithinAbs(0.15, 1e-6));
    CHECK(request.dont_sort == true);
    CHECK(request.can_reverse == false);
    CHECK(request.fill_order == SurfaceFillOrder::Inward);
    CHECK_THAT(request.horiz_move, WithinAbs(1234.0, 1e-6));
    CHECK(request.symmetric_infill_y_axis == true);
    CHECK(request.infill_complete_top == true);
    CHECK(request.symmetric_y_axis == 9876);
    CHECK(request.locked_zag == true);
    CHECK_THAT(request.infill_lock_depth, WithinAbs(4321.0, 1e-6));
    CHECK_THAT(request.skin_infill_depth, WithinAbs(210.0, 1e-6));
    CHECK(request.center_of_surface_pattern == CenterOfSurfacePattern::Each_Model);
    // The setting that selected the pattern is explicit in the request, never inferred.
    CHECK(request.purpose == InfillSurfacePurpose::TopSurface);
}

TEST_CASE("An unset flow in the infill request reports zero cross-section", "[PluginInfill]")
{
    RequestSnapshotFill fill;
    const InfillPluginRequest request =
        make_infill_plugin_request(fill, FillParams{}, 1,
                                   std::pair<float, Point>{ 0.f, Point(0, 0) }, ExPolygon{});

    // Flow::mm3_per_mm() rejects a non-positive extrusion; the snapshot must not fail a call for the
    // host leaving the flow unset.
    CHECK_THAT(request.flow.mm3_per_mm, WithinAbs(0.0, 0.0));
    CHECK_THAT(request.flow.spacing, WithinAbs(0.0, 0.0));
    CHECK_THAT(request.flow_width, WithinAbs(0.0, 0.0));
    CHECK_THAT(request.flow_height, WithinAbs(0.0, 0.0));
    // A Fill outside an object layer reports the identity as unavailable, never as object 0.
    CHECK(request.object_id == -1);
    CHECK(request.layer_index == -1);
    CHECK(request.region_id == -1);
    CHECK(request.region_ids.empty());
}

TEST_CASE("An infill capability advertises multiple patterns with per-pattern surface support", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-multi");
    write_plugin(data_dir_guard, "MultiInfill", "Multi Infill",
                 "        return [\n"
                 "            orca.infill.PatternDescriptor('plugin-grid', 'Plugin Grid', [orca.infill.SurfacePurpose.Sparse]),\n"
                 "            orca.infill.PatternDescriptor('plugin-solid', 'Plugin Solid', [orca.infill.SurfacePurpose.InternalSolid]),\n"
                 "        ]\n");

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "MultiInfill", error));
    INFO("load error: " << error);

    const auto capability = manager.get_plugin_capability("Multi Infill", PluginCapabilityType::Infill,
                                                          /*only_enabled=*/true);
    REQUIRE(capability != nullptr);
    auto infill = std::dynamic_pointer_cast<InfillPluginCapability>(capability);
    REQUIRE(infill != nullptr);
    REQUIRE(infill->resolved_patterns().size() == 2);

    // A capability name is the provider identity, not the pattern key.
    CHECK(manager.get_plugin_capability("plugin-grid", PluginCapabilityType::Infill, true) == nullptr);

    // Every advertised pattern is registered, and its descriptor's purposes become its registry
    // metadata so the four dropdowns and the project preflight can filter it.
    CHECK(InfillFactory::is_infill_registered("plugin-grid"));
    CHECK(InfillFactory::is_infill_registered("plugin-solid"));
    const std::shared_ptr<const InfillInfo> grid = InfillFactory::find_infill("plugin-grid");
    REQUIRE(grid != nullptr);
    CHECK(grid->plugin_key == "MultiInfill");
    CHECK(grid->capability_name == "Multi Infill");
    CHECK(grid->display_name == "Plugin Grid");
    CHECK(grid->supports_surface(InfillSurfacePurpose::Sparse));
    CHECK_FALSE(grid->supports_surface(InfillSurfacePurpose::InternalSolid));

    const std::shared_ptr<const InfillInfo> solid = InfillFactory::find_infill("plugin-solid");
    REQUIRE(solid != nullptr);
    CHECK_FALSE(solid->supports_surface(InfillSurfacePurpose::Sparse));
    CHECK(solid->supports_surface(InfillSurfacePurpose::InternalSolid));

    manager.unload_plugin("MultiInfill");
    CHECK_FALSE(InfillFactory::is_infill_registered("plugin-grid"));
    CHECK_FALSE(InfillFactory::is_infill_registered("plugin-solid"));
}

TEST_CASE("Legacy infill overrides without a pattern ID still dispatch", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-legacy-signatures");
    const std::string methods =
        "    def generate(self, request):\n"
        "        assert isinstance(request, orca.infill.InfillPluginRequest)\n"
        "        return [[orca.host.Point(0, 0), orca.host.Point(10, 0)]]\n"
        "    def no_sort(self):\n"
        "        return True\n"
        "    def aligned_to_origin(self):\n"
        "        return True\n";
    write_plugin(data_dir_guard, "LegacySignaturesInfill", "Legacy Signatures Infill", {}, methods);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "LegacySignaturesInfill", error));
    INFO("load error: " << error);

    const auto capability = manager.get_plugin_capability("Legacy Signatures Infill",
                                                          PluginCapabilityType::Infill, /*only_enabled=*/true);
    REQUIRE(capability != nullptr);
    auto infill = std::dynamic_pointer_cast<InfillPluginCapability>(capability);
    REQUIRE(infill != nullptr);

    const InfillPluginRequest request;
    Polylines                 paths;
    REQUIRE_NOTHROW(paths = infill->generate("legacy-pattern", request));
    REQUIRE(paths.size() == 1);
    CHECK(paths.front().points.size() == 2);
    CHECK(infill->no_sort("legacy-pattern"));
    CHECK(infill->aligned_to_origin("legacy-pattern"));

    manager.unload_plugin("LegacySignaturesInfill");
}

TEST_CASE("Modern infill overrides receive the pattern ID", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-modern-signatures");
    const std::string methods =
        "    def generate(self, pattern_id, request):\n"
        "        assert pattern_id == 'modern-pattern'\n"
        "        assert isinstance(request, orca.infill.InfillPluginRequest)\n"
        "        assert request.object_id == -1\n"
        "        assert request.layer_index == -1\n"
        "        assert request.region_id == -1\n"
        "        assert list(request.region_ids) == []\n"
        "        return [[orca.host.Point(0, 0), orca.host.Point(10, 0)]]\n"
        "    def no_sort(self, pattern_id):\n"
        "        return pattern_id == 'modern-pattern'\n"
        "    def aligned_to_origin(self, pattern_id):\n"
        "        return pattern_id == 'modern-pattern'\n";
    write_plugin(data_dir_guard, "ModernSignaturesInfill", "Modern Signatures Infill", {}, methods);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "ModernSignaturesInfill", error));
    INFO("load error: " << error);

    const auto capability = manager.get_plugin_capability("Modern Signatures Infill",
                                                          PluginCapabilityType::Infill, /*only_enabled=*/true);
    REQUIRE(capability != nullptr);
    auto infill = std::dynamic_pointer_cast<InfillPluginCapability>(capability);
    REQUIRE(infill != nullptr);

    const InfillPluginRequest request;
    Polylines                 paths;
    REQUIRE_NOTHROW(paths = infill->generate("modern-pattern", request));
    REQUIRE(paths.size() == 1);
    CHECK(infill->no_sort("modern-pattern"));
    CHECK(infill->aligned_to_origin("modern-pattern"));

    manager.unload_plugin("ModernSignaturesInfill");
}

TEST_CASE("Flexible *args infill overrides receive the current signature", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-args-signatures");
    const std::string methods =
        "    def generate(self, *args):\n"
        "        assert len(args) == 2\n"
        "        assert isinstance(args[0], str)\n"
        "        assert isinstance(args[1], orca.infill.InfillPluginRequest)\n"
        "        return [[orca.host.Point(0, 0), orca.host.Point(10, 0)]]\n"
        "    def no_sort(self, *args):\n"
        "        return len(args) == 1\n"
        "    def aligned_to_origin(self, *args):\n"
        "        return len(args) == 1\n";
    write_plugin(data_dir_guard, "ArgsSignaturesInfill", "Args Signatures Infill", {}, methods);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "ArgsSignaturesInfill", error));
    INFO("load error: " << error);

    const auto capability = manager.get_plugin_capability("Args Signatures Infill",
                                                          PluginCapabilityType::Infill, /*only_enabled=*/true);
    REQUIRE(capability != nullptr);
    auto infill = std::dynamic_pointer_cast<InfillPluginCapability>(capability);
    REQUIRE(infill != nullptr);

    const InfillPluginRequest request;
    Polylines                 paths;
    REQUIRE_NOTHROW(paths = infill->generate("args-pattern", request));
    REQUIRE(paths.size() == 1);
    CHECK(infill->no_sort("args-pattern"));
    CHECK(infill->aligned_to_origin("args-pattern"));

    manager.unload_plugin("ArgsSignaturesInfill");
}

TEST_CASE("A legacy generate() that raises TypeError is called only once", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-legacy-typeerror");
    const std::string methods =
        "    def generate(self, request):\n"
        "        self.calls = getattr(self, 'calls', 0) + 1\n"
        "        raise TypeError('plugin logic, not a signature mismatch')\n";
    write_plugin(data_dir_guard, "LegacyTypeErrorInfill", "Legacy TypeError Infill", {}, methods);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "LegacyTypeErrorInfill", error));
    INFO("load error: " << error);

    const auto capability = manager.get_plugin_capability("Legacy TypeError Infill",
                                                          PluginCapabilityType::Infill, /*only_enabled=*/true);
    REQUIRE(capability != nullptr);
    auto infill = std::dynamic_pointer_cast<InfillPluginCapability>(capability);
    REQUIRE(infill != nullptr);

    // Hold the GIL while the rethrown Python error and the instance attribute are alive.
    {
        pybind11::gil_scoped_acquire gil;
        const InfillPluginRequest   request;
        CHECK_THROWS(infill->generate("legacy-pattern", request));

        const pybind11::object py_infill = pybind11::cast(infill);
        REQUIRE(pybind11::hasattr(py_infill, "calls"));
        CHECK(py_infill.attr("calls").cast<int>() == 1);
    }

    manager.unload_plugin("LegacyTypeErrorInfill");
}

TEST_CASE("An infill capability without patterns() defaults to one sparse pattern named after itself", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-legacy");
    write_plugin(data_dir_guard, "LegacyInfill", "Legacy Infill");

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "LegacyInfill", error));
    INFO("load error: " << error);

    const auto capability = manager.get_plugin_capability("Legacy Infill", PluginCapabilityType::Infill);
    REQUIRE(capability != nullptr);
    auto infill = std::dynamic_pointer_cast<InfillPluginCapability>(capability);
    REQUIRE(infill != nullptr);
    REQUIRE(infill->resolved_patterns().size() == 1);
    CHECK(infill->resolved_patterns().front().id == "Legacy Infill");
    CHECK(infill_pattern_supports_surface(infill->resolved_patterns().front(), InfillSurfacePurpose::Sparse));

    CHECK(InfillFactory::is_infill_registered("Legacy Infill"));
    const std::shared_ptr<const InfillInfo> info = InfillFactory::find_infill("Legacy Infill");
    REQUIRE(info != nullptr);
    CHECK(info->plugin_key == "LegacyInfill");
    CHECK(info->capability_name == "Legacy Infill");
    // The descriptor default stays sparse-only, so plugins written before surface purposes existed
    // keep exactly the surface they used to have.
    CHECK(info->supports_surface(InfillSurfacePurpose::Sparse));
    CHECK_FALSE(info->supports_surface(InfillSurfacePurpose::TopSurface));

    // A plugin that defines neither optional method keeps the C++ Fill defaults.
    CHECK_FALSE(infill->no_sort("Legacy Infill"));
    CHECK_FALSE(infill->aligned_to_origin("Legacy Infill"));

    manager.unload_plugin("LegacyInfill");
}

TEST_CASE("Disabling an infill capability unregisters its patterns and re-enabling restores them", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-toggle");
    write_plugin(data_dir_guard, "ToggleInfill", "Toggle Infill",
                 "        return [orca.infill.PatternDescriptor('toggle-pattern', 'Toggle Pattern')]\n");

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "ToggleInfill", error));
    INFO("load error: " << error);
    REQUIRE(InfillFactory::is_infill_registered("toggle-pattern"));

    const PluginCapabilityId id{PluginCapabilityType::Infill, "Toggle Infill", "ToggleInfill"};
    manager.set_capability_enabled(id, false);
    CHECK_FALSE(InfillFactory::is_infill_registered("toggle-pattern"));

    manager.set_capability_enabled(id, true);
    CHECK(InfillFactory::is_infill_registered("toggle-pattern"));

    manager.unload_plugin("ToggleInfill");
    CHECK_FALSE(InfillFactory::is_infill_registered("toggle-pattern"));
}

TEST_CASE("An infill capability must have a non-empty name", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-empty");
    write_plugin(data_dir_guard, "EmptyInfill", "");

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    CHECK_FALSE(load_and_wait(manager, "EmptyInfill", error));
    CHECK(error.find("empty name") != std::string::npos);
    CHECK_FALSE(manager.is_plugin_loaded("EmptyInfill"));
}

TEST_CASE("An infill pattern ID cannot claim a built-in pattern key", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-builtin");
    write_plugin(data_dir_guard, "BuiltinInfill", "Builtin Infill",
                 "        return [orca.infill.PatternDescriptor('rectilinear', 'Rectilinear Clone')]\n");

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    CHECK_FALSE(load_and_wait(manager, "BuiltinInfill", error));
    CHECK(error.find("built-in sparse infill pattern") != std::string::npos);
    CHECK_FALSE(manager.is_plugin_loaded("BuiltinInfill"));
}

TEST_CASE("An infill capability cannot declare duplicate pattern IDs", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-duplicate");
    write_plugin(data_dir_guard, "DuplicateInfill", "Duplicate Infill",
                 "        return [\n"
                 "            orca.infill.PatternDescriptor('same-pattern', 'One'),\n"
                 "            orca.infill.PatternDescriptor('same-pattern', 'Two'),\n"
                 "        ]\n");

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    CHECK_FALSE(load_and_wait(manager, "DuplicateInfill", error));
    CHECK(error.find("duplicate pattern") != std::string::npos);
    CHECK_FALSE(manager.is_plugin_loaded("DuplicateInfill"));
}

TEST_CASE("Two plugins cannot register the same infill pattern ID", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-pattern-collision");
    write_plugin(data_dir_guard, "FirstInfill", "First Infill",
                 "        return [orca.infill.PatternDescriptor('shared-pattern', 'Shared Pattern')]\n");
    write_plugin(data_dir_guard, "SecondInfill", "Second Infill",
                 "        return [orca.infill.PatternDescriptor('shared-pattern', 'Shared Pattern')]\n");

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "FirstInfill", error));
    INFO("first load error: " << error);

    CHECK_FALSE(load_and_wait(manager, "SecondInfill", error));
    CHECK(error.find("already provided by plugin") != std::string::npos);
    CHECK_FALSE(manager.is_plugin_loaded("SecondInfill"));

    manager.unload_plugin("FirstInfill");
    CHECK_FALSE(InfillFactory::is_infill_registered("shared-pattern"));
}

TEST_CASE("Two plugins cannot register the same infill capability name", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-collision");
    write_plugin(data_dir_guard, "FirstCapInfill", "Shared Infill");
    write_plugin(data_dir_guard, "SecondCapInfill", "Shared Infill");

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "FirstCapInfill", error));
    INFO("first load error: " << error);

    CHECK_FALSE(load_and_wait(manager, "SecondCapInfill", error));
    CHECK(error.find("Capability collision for type") != std::string::npos);
    CHECK_FALSE(manager.is_plugin_loaded("SecondCapInfill"));

    // The first owner keeps the name.
    REQUIRE(manager.get_plugin_capability("Shared Infill", PluginCapabilityType::Infill) != nullptr);
    CHECK(manager.get_plugin_capability("Shared Infill", PluginCapabilityType::Infill)->name() == "Shared Infill");

    manager.unload_plugin("FirstCapInfill");
}
