#include <catch2/catch_all.hpp>

#include "libslic3r/SlicingAdmission.hpp"
#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/host/IsolatedSlicingJob.hpp"

#include "../fff_print/test_helpers.hpp"
#include "plugin_test_utils.hpp"
#include "python_test_support.hpp"
#include "test_utils.hpp"

#include <boost/filesystem.hpp>
#include <pybind11/embed.h>
#include <pybind11/eval.h>

#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace py = pybind11;

namespace {

Slic3r::DynamicPrintConfig small_fff_config()
{
    Slic3r::DynamicPrintConfig config;
    config.apply(Slic3r::FullPrintConfig::defaults());
    config.set_deserialize_strict({
        { "printer_technology", "FFF" },
        { "layer_height", "0.30" },
        { "initial_layer_print_height", "0.30" },
        { "sparse_infill_density", "0%" },
        { "gcode_comments", "1" },
        { "use_relative_e_distances", "0" },
    });
    return config;
}

Slic3r::Model small_fff_model(const Slic3r::DynamicPrintConfig& config)
{
    Slic3r::Print seed_print;
    Slic3r::Model model;
    Slic3r::Test::init_print(
        { Slic3r::make_cube(5.0, 5.0, 2.0) }, seed_print, model, config);
    return model;
}

Slic3r::IsolatedSlicingBaseline owned_baseline(
    const Slic3r::Model& model, const Slic3r::DynamicPrintConfig& config)
{
    Slic3r::IsolatedSlicingBaseline baseline;
    baseline.model        = model;
    baseline.config       = config;
    baseline.plate_index  = 0;
    baseline.plate_origin = Slic3r::Vec3d::Zero();
    baseline.filament_maps =
        config.option<Slic3r::ConfigOptionInts>("filament_map")->values;
    baseline.filament_volume_maps =
        config.option<Slic3r::ConfigOptionInts>("filament_volume_map")->values;
    baseline.bbl_vendor = false;
    baseline.check_mixed_filament_compatibility = true;
    return baseline;
}

std::shared_ptr<Slic3r::IsolatedSlicingJob> make_job(
    const boost::filesystem::path& storage_root, const std::string& plugin_key = {})
{
    const Slic3r::DynamicPrintConfig config = small_fff_config();
    const Slic3r::Model model = small_fff_model(config);
    return Slic3r::IsolatedSlicingJob::create_owned(
        owned_baseline(model, config), storage_root.string(), plugin_key);
}

bool tree_has_regular_file(const boost::filesystem::path& root)
{
    if (!boost::filesystem::exists(root))
        return false;
    for (boost::filesystem::recursive_directory_iterator it(root), end; it != end; ++it)
        if (boost::filesystem::is_regular_file(it->path()))
            return true;
    return false;
}

void set_printer_technology(
    Slic3r::DynamicPrintConfig& config, Slic3r::PrinterTechnology technology)
{
    Slic3r::ConfigOption* option = config.option("printer_technology");
    REQUIRE(option != nullptr);
    if (auto* typed = dynamic_cast<Slic3r::ConfigOptionEnum<Slic3r::PrinterTechnology>*>(option))
        typed->value = technology;
    else if (auto* generic = dynamic_cast<Slic3r::ConfigOptionEnumGeneric*>(option))
        generic->value = technology;
    else
        FAIL("printer_technology has an unexpected option type");
}

struct ScopedIsolatedPluginManager
{
    bool initialized = false;

    ScopedIsolatedPluginManager()
    {
        release_test_python_interpreter();
        initialized = Slic3r::PluginManager::instance().initialize();
    }

    ~ScopedIsolatedPluginManager()
    {
        Slic3r::PluginManager::instance().shutdown();
        Slic3r::PythonInterpreter::instance().shutdown();
    }
};

const char* const ISOLATED_LIFECYCLE_PLUGIN_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Isolated Lifecycle Plugin"
# description = "Isolated job lifecycle fixture"
# author = "OrcaSlicer"
# version = "1.0"
# type = "script"
# ///
import orca

class LifecycleCapability(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "LifecycleCapability"

    def execute(self, ctx):
        return orca.ExecutionResult.success()

@orca.plugin
class LifecyclePackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(LifecycleCapability)
)PY";

void write_isolated_lifecycle_plugin(const Slic3r::ScopedDataDir& data_dir)
{
    const boost::filesystem::path plugin_dir =
        data_dir.plugins_dir() / "Isolated_Lifecycle_Plugin";
    boost::filesystem::create_directories(plugin_dir);
    std::ofstream out(
        (plugin_dir / "Isolated_Lifecycle_Plugin.py").string(), std::ios::binary);
    out << ISOLATED_LIFECYCLE_PLUGIN_SOURCE;
}

} // namespace

TEST_CASE("isolated slicing candidate staging is strict, atomic, and baseline-relative",
          "[PluginHost][IsolatedSlicing]")
{
    ScopedTemporaryDir storage("orca-isolated-candidate");
    auto job = make_job(storage.path());

    const auto first = job->apply_candidate({ { "layer_height", "0.25" } });
    REQUIRE(first.at("layer_height") == "0.25");
    CHECK(job->candidate() == first);

    CHECK_THROWS(job->apply_candidate({
        { "layer_height", "0.20" },
        { "sparse_infill_density", "not-a-percent" },
    }));
    CHECK(job->candidate() == first);

    const auto second = job->apply_candidate({ { "sparse_infill_density", "10%" } });
    CHECK(second.size() == 1);
    CHECK(second.count("layer_height") == 0);
    CHECK(second.at("sparse_infill_density") == "10%");
    CHECK(job->candidate() == second);
}

TEST_CASE("isolated slicing owns its baseline and exports only inside plugin storage",
          "[PluginHost][IsolatedSlicing]")
{
    ScopedTemporaryDir storage("orca-isolated-success");
    Slic3r::DynamicPrintConfig live_config = small_fff_config();
    Slic3r::Model live_model = small_fff_model(live_config);

    auto job = Slic3r::IsolatedSlicingJob::create_owned(
        owned_baseline(live_model, live_config), storage.string());
    REQUIRE(job->snapshot().object_count == 1);

    // Mutating the source after capture cannot affect the owned snapshot.
    live_model.clear_objects();
    live_config.set_deserialize_strict("layer_height", "0.10");
    CHECK(job->snapshot().object_count == 1);

    job->apply_candidate({ { "layer_height", "0.10" } });
    const auto candidate = job->apply_candidate({ { "sparse_infill_density", "0%" } });
    job->run();
    const Slic3r::IsolatedSlicingState terminal = job->wait();
    INFO(job->result().error);
    REQUIRE(terminal == Slic3r::IsolatedSlicingState::Succeeded);

    const Slic3r::IsolatedSlicingResult result = job->result();
    CHECK(result.state == Slic3r::IsolatedSlicingState::Succeeded);
    CHECK(result.error.empty());
    CHECK(result.candidate == candidate);
    CHECK(result.gcode_size > 0);
    CHECK(result.gcode_statistics.move_count > 0);
    // The earlier 0.10 mm candidate and the later source edit must not have
    // accumulated: the immutable 0.30 mm baseline yields far fewer than 12 layers.
    CHECK(result.gcode_statistics.layer_count < 12);
    CHECK(boost::filesystem::is_regular_file(result.gcode_path));
    CHECK(Slic3r::is_inside_allowed_root(result.gcode_path, storage.path()));

    const boost::filesystem::path owned_dir =
        boost::filesystem::path(result.gcode_path).parent_path();
    job->close();
    job->close();
    CHECK(job->state() == Slic3r::IsolatedSlicingState::Closed);
    CHECK_FALSE(boost::filesystem::exists(owned_dir));
    CHECK(boost::filesystem::exists(storage.path()));
}

TEST_CASE("isolated slicing never mutates its source model or config",
          "[PluginHost][IsolatedSlicing]")
{
    ScopedTemporaryDir storage("orca-isolated-source");
    Slic3r::DynamicPrintConfig source_config = small_fff_config();
    Slic3r::Model source_model = small_fff_model(source_config);
    const size_t source_objects = source_model.objects.size();
    const std::string source_layer_height = source_config.opt_serialize("layer_height");
    const Slic3r::ObjectID source_id = source_model.objects.front()->id();

    auto job = Slic3r::IsolatedSlicingJob::create_owned(
        owned_baseline(source_model, source_config), storage.string());
    job->apply_candidate({ { "layer_height", "0.25" } });
    job->run();
    const Slic3r::IsolatedSlicingState terminal = job->wait();
    INFO(job->result().error);
    REQUIRE(terminal == Slic3r::IsolatedSlicingState::Succeeded);

    CHECK(source_model.objects.size() == source_objects);
    CHECK(source_model.objects.front()->id() == source_id);
    CHECK(source_config.opt_serialize("layer_height") == source_layer_height);
    job->close();
}

TEST_CASE("isolated slicing admission rejects live and second isolated jobs immediately",
          "[PluginHost][IsolatedSlicing]")
{
    ScopedTemporaryDir storage("orca-isolated-admission");

    SECTION("live slicing overlap") {
        Slic3r::SlicingAdmissionToken live = Slic3r::try_acquire_slicing_admission(
            Slic3r::SlicingAdmissionMode::LiveSlicing);
        REQUIRE(live);
        auto job = make_job(storage.path());
        CHECK_THROWS_WITH(job->run(), Catch::Matchers::ContainsSubstring("Slicing is busy"));
        CHECK(job->state() == Slic3r::IsolatedSlicingState::Ready);
    }

    SECTION("second isolated job") {
        auto first = make_job(storage.path());
        auto second = make_job(storage.path());
        first->run();
        CHECK_THROWS_WITH(second->run(), Catch::Matchers::ContainsSubstring("Slicing is busy"));
        first->cancel();
        CHECK(first->wait() == Slic3r::IsolatedSlicingState::Cancelled);
        first->close();
        second->close();
    }
}

TEST_CASE("isolated slicing cancellation and close join and clean owned artifacts",
          "[PluginHost][IsolatedSlicing]")
{
    ScopedTemporaryDir storage("orca-isolated-cancel");

    SECTION("explicit cancellation") {
        auto job = make_job(storage.path());
        job->run();
        job->cancel();
        REQUIRE(job->wait() == Slic3r::IsolatedSlicingState::Cancelled);
        CHECK(job->result().state == Slic3r::IsolatedSlicingState::Cancelled);
        CHECK_FALSE(tree_has_regular_file(storage.path() / "isolated_slicing"));

        job->close();
        job->close();
        CHECK(job->state() == Slic3r::IsolatedSlicingState::Closed);
    }

    SECTION("close while running") {
        auto job = make_job(storage.path());
        job->run();
        job->close();
        CHECK(job->state() == Slic3r::IsolatedSlicingState::Closed);
        CHECK_FALSE(tree_has_regular_file(storage.path() / "isolated_slicing"));
    }
}

TEST_CASE("isolated slicing v1 rejects unsupported baselines and candidate seams",
          "[PluginHost][IsolatedSlicing]")
{
    ScopedTemporaryDir storage("orca-isolated-reject");
    const Slic3r::DynamicPrintConfig base_config = small_fff_config();
    const Slic3r::Model model = small_fff_model(base_config);

    for (const Slic3r::PrinterTechnology technology :
         { Slic3r::ptSLA, Slic3r::ptUnknown }) {
        auto baseline = owned_baseline(model, base_config);
        set_printer_technology(baseline.config, technology);
        CHECK_THROWS(Slic3r::IsolatedSlicingJob::create_owned(
            std::move(baseline), storage.string()));
    }

    {
        auto baseline = owned_baseline(model, base_config);
        baseline.config.option<Slic3r::ConfigOptionStrings>(
            "slicing_pipeline_plugin")->values = { "unsafe" };
        CHECK_THROWS_WITH(Slic3r::IsolatedSlicingJob::create_owned(
            std::move(baseline), storage.string()),
            Catch::Matchers::ContainsSubstring("slicing pipeline"));
    }
    {
        auto baseline = owned_baseline(model, base_config);
        baseline.config.option<Slic3r::ConfigOptionStrings>("post_process")->values = {
            "external-command" };
        CHECK_THROWS_WITH(Slic3r::IsolatedSlicingJob::create_owned(
            std::move(baseline), storage.string()),
            Catch::Matchers::ContainsSubstring("post_process"));
    }

    auto job = make_job(storage.path());
    for (const char* key : {
             "slicing_pipeline_plugin", "post_process", "printer_technology",
             "printable_area", "bed_exclude_area", "wrapping_exclude_area",
             "printable_height", "extruder_printable_area",
             "extruder_printable_height", "bed_custom_texture", "bed_custom_model" }) {
        CAPTURE(key);
        CHECK_THROWS(job->apply_candidate({ { key, "0" } }));
    }
    CHECK_THROWS(job->apply_candidate({ { "not_a_real_orca_setting", "1" } }));
}

TEST_CASE("isolated slicing Python surface is opaque and wait releases the GIL",
          "[PluginHost][IsolatedSlicing][Python]")
{
    ScopedTemporaryDir storage("orca-isolated-python");
    py::object host = import_orca_module().attr("host");
    REQUIRE(py::hasattr(host, "create_isolated_fff_slicing_job"));
    REQUIRE(py::hasattr(host, "IsolatedFFFSlicingJob"));

    auto job = make_job(storage.path());
    py::dict globals;
    globals["__builtins__"] = py::module_::import("builtins");
    globals["job"] = py::cast(job);

    py::exec(R"PY(
import sys
import threading

assert not hasattr(job, "model")
assert not hasattr(job, "config")
assert not hasattr(job, "preset")
assert not hasattr(job, "upload")
assert not hasattr(job, "print")
assert not hasattr(job, "post_process")

snapshot = job.snapshot()
try:
    snapshot.object_count = 99
    raise AssertionError("snapshot DTO is writable")
except AttributeError:
    pass

go = threading.Event()
progressed = threading.Event()
def marker():
    go.wait()
    progressed.set()

old_interval = sys.getswitchinterval()
sys.setswitchinterval(1000.0)
try:
    worker = threading.Thread(target=marker)
    worker.start()
    job.run()
    go.set()
    terminal = job.wait()
    worker.join()
finally:
    sys.setswitchinterval(old_interval)

gil_progressed = progressed.is_set()
job.close()
)PY", globals);

    CHECK(globals["gil_progressed"].cast<bool>());
}

TEST_CASE("live isolated job capture fails closed without an initialized Orca GUI",
          "[PluginHost][IsolatedSlicing][Python]")
{
    py::object host = import_orca_module().attr("host");
    try {
        host.attr("create_isolated_fff_slicing_job")();
        FAIL("live isolated capture unexpectedly succeeded without Orca GUI state");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_RuntimeError));
        CHECK(std::string(error.what()).find("application is not initialized") != std::string::npos);
    }
}

TEST_CASE("plugin unload and shutdown synchronously drain isolated slicing jobs",
          "[PluginHost][IsolatedJobLifecycle]")
{
    Slic3r::ScopedDataDir plugin_data("isolated-job-lifecycle");
    write_isolated_lifecycle_plugin(plugin_data);
    ScopedIsolatedPluginManager plugin_system;
    REQUIRE(plugin_system.initialized);

    Slic3r::PluginManager& manager = Slic3r::PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    manager.load_plugin("Isolated_Lifecycle_Plugin", /*skip_deps=*/true);
    std::string load_error;
    REQUIRE(manager.wait_for_plugin_load(
        "Isolated_Lifecycle_Plugin", std::chrono::seconds(120), load_error));
    INFO(load_error);
    REQUIRE(manager.is_plugin_loaded("Isolated_Lifecycle_Plugin"));

    ScopedTemporaryDir storage_a("orca-isolated-lifecycle-a");
    ScopedTemporaryDir storage_b("orca-isolated-lifecycle-b");
    ScopedTemporaryDir storage_c("orca-isolated-lifecycle-c");
    ScopedTemporaryDir storage_d("orca-isolated-lifecycle-d");

    auto plugin_a_job = make_job(storage_a.path(), "Isolated_Lifecycle_Plugin");
    auto plugin_b_job = make_job(storage_b.path(), "isolated-plugin-b");

    bool teardown_observed_after_drain = false;
    bool registration_rejected_during_teardown = false;
    manager.subscribe_on_unload_callback(
        [&](const std::string& plugin_key) {
            if (plugin_key == "Isolated_Lifecycle_Plugin") {
                teardown_observed_after_drain =
                    plugin_a_job->state() == Slic3r::IsolatedSlicingState::Closed &&
                    !tree_has_regular_file(storage_a.path() / "isolated_slicing");
                try {
                    (void) make_job(storage_a.path(), "Isolated_Lifecycle_Plugin");
                } catch (const std::runtime_error& error) {
                    registration_rejected_during_teardown =
                        std::string(error.what()).find("Plugin is unloading") != std::string::npos;
                }
            }
        });

    plugin_a_job->run();
    REQUIRE(manager.unload_plugin("Isolated_Lifecycle_Plugin"));
    CHECK(plugin_a_job->state() == Slic3r::IsolatedSlicingState::Closed);
    CHECK(plugin_a_job->wait() == Slic3r::IsolatedSlicingState::Closed);
    CHECK(teardown_observed_after_drain);
    CHECK(registration_rejected_during_teardown);
    CHECK_FALSE(tree_has_regular_file(storage_a.path() / "isolated_slicing"));

    // The per-plugin gate remains closed through teardown, is idempotent, and does not affect B.
    CHECK_THROWS_WITH(make_job(storage_a.path(), "Isolated_Lifecycle_Plugin"),
                      Catch::Matchers::ContainsSubstring("Plugin is unloading"));
    CHECK(manager.unload_plugin("Isolated_Lifecycle_Plugin"));
    CHECK(plugin_b_job->state() == Slic3r::IsolatedSlicingState::Ready);
    plugin_b_job->run();
    REQUIRE(plugin_b_job->wait() == Slic3r::IsolatedSlicingState::Succeeded);
    REQUIRE(tree_has_regular_file(storage_b.path() / "isolated_slicing"));

    auto plugin_c_job = make_job(storage_c.path(), "isolated-plugin-c");
    auto plugin_d_job = make_job(storage_d.path(), "isolated-plugin-d");
    plugin_c_job->run();

    // Global shutdown drains running, completed and not-yet-run jobs before normal plugin teardown.
    manager.shutdown();
    CHECK(plugin_b_job->state() == Slic3r::IsolatedSlicingState::Closed);
    CHECK(plugin_c_job->state() == Slic3r::IsolatedSlicingState::Closed);
    CHECK(plugin_c_job->wait() == Slic3r::IsolatedSlicingState::Closed);
    CHECK(plugin_d_job->state() == Slic3r::IsolatedSlicingState::Closed);
    CHECK_FALSE(tree_has_regular_file(storage_b.path() / "isolated_slicing"));
    CHECK_FALSE(tree_has_regular_file(storage_c.path() / "isolated_slicing"));
    CHECK_FALSE(tree_has_regular_file(storage_d.path() / "isolated_slicing"));
    CHECK_THROWS_WITH(make_job(storage_d.path(), "isolated-plugin-new"),
                      Catch::Matchers::ContainsSubstring("Plugin is unloading"));

    // Repeated shutdown has no workers to drain and remains safe.
    manager.shutdown();
}
