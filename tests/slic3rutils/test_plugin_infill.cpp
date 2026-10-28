// Infill plugin registration and dispatch: sparse_infill_pattern stores the infill capability's
// canonical name (PluginCapabilityInterface::name()), the same key convention
// slicing_pipeline_plugin uses. Covers the loader's name validation (empty, built-in collision)
// and the generic per-type registry collision check.
//
// As in test_plugin_lifecycle.cpp, each Catch2 test case runs in its own process
// (catch_discover_tests), so the PluginManager and interpreter come up at most once per test.
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <string>

#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include "slic3r/plugin/pluginTypes/infill/InfillPluginCapability.hpp"

#include "plugin_test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

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
// coexist in one discovery pass; `capability_name` is what get_name() reports and is also the
// sparse_infill_pattern key the plugin serves.
std::string make_infill_plugin_source(const std::string& stem,
                                      const std::string& capability_name)
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
        + "    def generate(self, request):\n"
        + "        return []\n"
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
                  const std::string&   capability_name)
{
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / stem;
    fs::create_directories(plugin_dir);

    std::ofstream out((plugin_dir / (stem + ".py")).string(), std::ios::binary);
    out << make_infill_plugin_source(stem, capability_name);
    out.close();
}

} // namespace

TEST_CASE("Infill dispatch resolves sparse_infill_pattern by canonical capability name", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-dispatch");
    write_plugin(data_dir_guard, "PluginGrid", "Plugin Grid");

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "PluginGrid", error));
    INFO("load error: " << error);

    // sparse_infill_pattern stores this canonical name verbatim; no separate pattern identifier.
    const auto capability = manager.get_plugin_capability("Plugin Grid", PluginCapabilityType::Infill,
                                                          /*only_enabled=*/true);
    REQUIRE(capability != nullptr);
    CHECK(capability->name() == "Plugin Grid");
    CHECK(capability->type() == PluginCapabilityType::Infill);
    CHECK(dynamic_cast<const InfillPluginCapability*>(capability.get()) != nullptr);

    // Only the canonical capability name is a pattern key; an arbitrary identifier must not resolve.
    CHECK(manager.get_plugin_capability("plugin-grid", PluginCapabilityType::Infill,
                                        /*only_enabled=*/true) == nullptr);

    manager.unload_plugin("PluginGrid");
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

TEST_CASE("An infill capability cannot claim a built-in pattern key", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-builtin");
    write_plugin(data_dir_guard, "BuiltinInfill", "rectilinear");

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    CHECK_FALSE(load_and_wait(manager, "BuiltinInfill", error));
    CHECK(error.find("built-in sparse infill pattern") != std::string::npos);
    CHECK_FALSE(manager.is_plugin_loaded("BuiltinInfill"));
}

TEST_CASE("Two plugins cannot register the same infill capability name", "[PluginInfill][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("infill-collision");
    write_plugin(data_dir_guard, "FirstInfill", "Shared Infill");
    write_plugin(data_dir_guard, "SecondInfill", "Shared Infill");

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "FirstInfill", error));
    INFO("first load error: " << error);

    CHECK_FALSE(load_and_wait(manager, "SecondInfill", error));
    CHECK(error.find("Capability collision for type") != std::string::npos);
    CHECK_FALSE(manager.is_plugin_loaded("SecondInfill"));

    // The first owner keeps the name.
    REQUIRE(manager.get_plugin_capability("Shared Infill", PluginCapabilityType::Infill) != nullptr);
    CHECK(manager.get_plugin_capability("Shared Infill", PluginCapabilityType::Infill)->name() == "Shared Infill");

    manager.unload_plugin("FirstInfill");
}
