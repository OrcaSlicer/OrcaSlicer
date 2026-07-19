#include <catch2/catch_all.hpp>

#include <libslic3r/Utils.hpp>
#include <slic3r/plugin/PluginConfig.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>

#include "plugin_test_utils.hpp"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

using namespace Slic3r;
namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

// Brings the plugin manager up and shuts both singletons down while boost::log is still alive;
// left to their static destructors, shutdown()'s logging runs after boost::log tears down its
// thread-local storage and crashes the process on exit (same reason ScopedPluginManager exists in
// test_plugin_lifecycle.cpp). initialize() IS needed here: the plugin under test is a
// slicing-pipeline script, so discover_plugins() brings up the Python interpreter to parse it,
// same as any other plugin.
struct ScopedManagerShutdown
{
    bool initialized = PluginManager::instance().initialize();

    ~ScopedManagerShutdown()
    {
        PluginManager::instance().shutdown();
        PythonInterpreter::instance().shutdown();
    }
};

const char* const CLOUD_PLUGIN_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Config Cloud Plugin"
# type = "slicing-pipeline"
# version = "1.0"
# ///
print('ok')
)PY";

} // namespace

// Regression: update_cloud_metadata() replaces a matched entry's descriptor wholesale with the
// cloud catalog record (`entry = cloud_entry`). Configuration used to ride on the descriptor, so
// that overwrite silently wiped it and plugins fell back to their built-in defaults (found via
// Twistify running with its demo defaults instead of the configured values, 2026-07-17).
// Configuration now lives in PluginConfig, keyed by the capability identity and kept off the
// descriptor entirely, so the merge cannot reach it. This asserts that end to end: a stored
// config survives the same refresh path, while the descriptor fields the refresh owns do update.
//
// The capability need not exist for this to be meaningful: what is pinned is the architectural
// invariant that config never rides on the descriptor again. Anyone reintroducing it there, or
// adding a cloud-refresh path that prunes config, fails here.
TEST_CASE("cloud metadata refresh preserves a plugin's stored config", "[PluginCloudMetadata]")
{
    ScopedManagerShutdown manager_shutdown_guard; // declared first: destroyed last
    if (!manager_shutdown_guard.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    ScopedDataDir data_dir_guard("cloud-meta-config");

    // A locally-installed cloud plugin: package .py plus an install-state sidecar carrying the
    // cloud identity. Discovery derives plugin_key from the cloud UUID.
    const std::string uuid       = "11111111-2222-3333-4444-555555555555";
    const fs::path    plugin_dir = fs::path(get_orca_plugins_dir()) / uuid;
    fs::create_directories(plugin_dir);
    {
        std::ofstream out((plugin_dir / "cloud_plugin-test.py").string(), std::ios::binary);
        out << CLOUD_PLUGIN_SOURCE;
    }
    PluginDescriptor sidecar;
    sidecar.name              = "Config Cloud Plugin";
    sidecar.installed_version = "1.0";
    sidecar.cloud             = CloudPluginState{uuid, true, false, false, false};
    REQUIRE(write_install_state(plugin_dir, sidecar));

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    const auto find_by_uuid = [&manager, &uuid]() -> PluginDescriptor {
        for (const PluginDescriptor& d : manager.get_plugin_descriptors(/*include_invalid=*/true))
            if (d.cloud_uuid() == uuid)
                return d;
        return {};
    };

    const PluginDescriptor discovered = find_by_uuid();
    REQUIRE(discovered.plugin_key == uuid);
    REQUIRE(discovered.version == "1.0");

    // The user has configured the plugin's capability (premise).
    const PluginCapabilityId id{PluginCapabilityType::SlicingPipeline, "Twist", uuid};
    const json               configured{{"twist_deg_per_mm", 1.0}, {"taper_per_mm", 0.0}};
    REQUIRE(manager.get_config().store_capability_config(id, configured));

    // A cloud catalog refresh for the same plugin: the record knows name/version/uuid and knows
    // nothing about local config or local paths.
    PluginDescriptor cloud_record;
    cloud_record.name       = "Config Cloud Plugin";
    cloud_record.plugin_key = uuid;
    cloud_record.version    = "1.1";
    cloud_record.cloud      = CloudPluginState{uuid, false, false, false, false};
    manager.update_cloud_metadata({cloud_record});

    // Cloud metadata landed on the descriptor...
    const PluginDescriptor refreshed = find_by_uuid();
    CHECK(refreshed.version == "1.1");
    CHECK(refreshed.plugin_key == uuid);
    CHECK(refreshed.installed_version == "1.0");

    // ...and the stored config is untouched, both in the live store...
    const auto stored = manager.get_config().get_config(id);
    REQUIRE(stored);
    CHECK(stored->config == configured);

    // ...and on disk, which is what the next run reads back.
    PluginConfig reloaded;
    reloaded.load();
    REQUIRE(reloaded.has_config(id));
    CHECK(reloaded.get_config(id)->config == configured);
}
