#include <catch2/catch_all.hpp>

#include <libslic3r/Utils.hpp>
#include <slic3r/plugin/PluginConfig.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>

#include "fff_print/test_helpers.hpp"
#include "plugin_test_utils.hpp"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <pybind11/embed.h>

#include <fstream>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;
namespace fs = boost::filesystem;
using json   = nlohmann::json;

// End-to-end coverage of a slicing-pipeline capability reading its own config: the loader seeds the
// store from the capability's get_default_config() hook, and the real dispatch
// (execute_capabilities_from_refs -> hook -> GIL -> trampoline) lets the plugin read back whatever
// the host has stored, through self.get_config(). A break anywhere in that chain makes plugins
// silently run on their built-in defaults, which is invisible to the plugin author (Twistify
// incident, 2026-07-17).
//
// Note this is deliberately NOT ctx.config_value(): that reads the slicer's print config, not the
// plugin's own config.

namespace {

struct ScopedPluginManager
{
    bool initialized = false;

    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager()
    {
        PluginManager::instance().shutdown();
        PythonInterpreter::instance().shutdown();
    }
};

const char* const CONFIG_PROBE_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Config Probe"
# description = "Echoes its own config back to the test"
# author = "OrcaSlicer"
# version = "1.0"
# type = "slicing-pipeline"
# ///
import json

import orca

class ConfigEcho(orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self):
        return "ConfigEcho"

    def get_default_config(self):
        return {"alpha": "1.25", "beta": "hello"}

    def execute(self, ctx):
        if ctx.step != orca.slicing.Step.posSlice or ctx.object is None:
            return orca.ExecutionResult.success()
        try:
            text = repr(sorted(json.loads(self.get_config()).items()))
        except Exception as e:  # what plugins' defaults-fallback code swallows silently
            text = "config-error: " + repr(e)
        orca._probe_config = text  # read back by the test through pybind
        return orca.ExecutionResult.success("config probed")

@orca.plugin
class ConfigProbePackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(ConfigEcho)
)PY";

fs::path write_plugin(const std::string& stem, const std::string& source)
{
    const fs::path plugin_dir = fs::path(get_orca_plugins_dir()) / stem;
    fs::create_directories(plugin_dir);

    std::ofstream out((plugin_dir / (stem + ".py")).string(), std::ios::binary);
    out << source;
    out.close();

    return plugin_dir;
}

} // namespace

TEST_CASE("slicing-pipeline dispatch delivers the stored config to self.get_config()", "[slicing_pipeline][PluginConfig][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("pipeline-config");
    write_plugin("ConfigProbe", CONFIG_PROBE_SOURCE);

    PluginManager& manager = PluginManager::instance();
    manager.get_config().load(); // reset the singleton's store against the empty temp data dir
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    manager.load_plugin("ConfigProbe", /*skip_deps=*/true, {});
    REQUIRE(manager.wait_for_plugin_load("ConfigProbe", std::chrono::seconds(120), error));
    INFO("load error: " << error);
    REQUIRE(manager.is_plugin_loaded("ConfigProbe"));

    const PluginCapabilityId id{PluginCapabilityType::SlicingPipeline, "ConfigEcho", "ConfigProbe"};

    // Loading seeds the store from the capability's get_default_config() hook, so a plugin has a
    // config before anyone has opened the Config tab.
    const auto seeded = manager.get_config().get_config(id);
    REQUIRE(seeded);
    CHECK(seeded->config == json({{"alpha", "1.25"}, {"beta", "hello"}}));

    // What editing the config in the Config tab does: the value the plugin must actually run on.
    REQUIRE(manager.get_config().store_capability_config(id, json({{"alpha", "9.5"}, {"beta", "hello"}})));

    // Slice with the capability selected, exactly as a preset would reference it.
    Print print;
    Model model;
    auto  config = DynamicPrintConfig::full_print_config();
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"ConfigEcho"}));
    config.set_key_value("plugins", new ConfigOptionStrings({"ConfigProbe;;ConfigEcho"}));
    init_print({cube(20)}, print, model, config);
    print.process();

    std::string observed = "<capability never executed>";
    {
        PythonGILState gil;
        REQUIRE(static_cast<bool>(gil));
        pybind11::module_ orca = pybind11::module_::import("orca");
        if (pybind11::hasattr(orca, "_probe_config"))
            observed = orca.attr("_probe_config").cast<std::string>();
    }
    INFO("config observed by Python: " << observed);
    // The edited value arrived, not the seeded default: the host's store is what reaches the plugin.
    CHECK(observed.find("'alpha', '9.5'") != std::string::npos);
    CHECK(observed.find("'beta', 'hello'") != std::string::npos);
    CHECK(observed.find("1.25") == std::string::npos);

    manager.unload_plugin("ConfigProbe");
}
