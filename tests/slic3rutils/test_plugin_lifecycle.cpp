#include <catch2/catch_all.hpp>

#include <libslic3r/Utils.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>

#include <boost/filesystem.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;

// Plugin load/unload lifecycle: discovery -> load -> capability materialization -> enable/disable
// -> unload.
//
// Each Catch2 test case runs in its own process (catch_discover_tests), so the PluginManager and
// interpreter singletons are brought up at most once per test.

namespace {

// Point data_dir() at a throwaway directory for the lifetime of a test and restore the previous
// value afterwards, so discovery scans a disposable {data_dir}/orca_plugins tree and tests don't
// leak state into each other.
struct ScopedDataDir
{
    std::string previous;
    fs::path    dir;

    explicit ScopedDataDir(const std::string& tag)
    {
        previous = data_dir();
        dir      = fs::temp_directory_path() / fs::unique_path("orca-" + tag + "-%%%%-%%%%");
        fs::create_directories(dir);
        set_data_dir(dir.string());
    }

    ~ScopedDataDir()
    {
        set_data_dir(previous);
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }

    fs::path plugins_dir() const { return dir / "orca_plugins"; }
};

// Brings the plugin system up, and tears it down explicitly at the end of the test.
//
// Shutting the interpreter down here, rather than leaving it to PythonInterpreter's static
// destructor, mirrors what the app does (GUI_App finalizes it before exit). Left to static
// destruction, shutdown()'s logging runs after boost::log has torn down its thread-local storage
// and throws, aborting the process after the tests have already passed.
//
// Declare this FIRST in a test so it is destroyed last.
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

// A minimal script plugin exposing exactly one capability, "Echo".
const char* const ECHO_PLUGIN_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Echo Plugin"
# description = "Plugin lifecycle characterization fixture"
# author = "OrcaSlicer"
# version = "1.0"
# type = "script"
# ///
import orca

class Echo(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Echo"

    def execute(self, ctx):
        return orca.ExecutionResult.success()

@orca.plugin
class EchoPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(Echo)
)PY";

// Writes {data_dir}/orca_plugins/<stem>/<stem>.py and returns the plugin directory.
fs::path write_plugin(const ScopedDataDir& data_dir_guard, const std::string& stem, const std::string& source)
{
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / stem;
    fs::create_directories(plugin_dir);

    std::ofstream out((plugin_dir / (stem + ".py")).string(), std::ios::binary);
    out << source;
    out.close();

    return plugin_dir;
}

// Loads a plugin and blocks until the detached worker thread is done with it.
bool load_and_wait(PluginManager&           manager,
                   const std::string&       plugin_key,
                   std::string&             error,
                   std::vector<std::string> capabilities_to_enable = {})
{
    manager.load_plugin(plugin_key, /*skip_deps=*/true, std::move(capabilities_to_enable));
    return manager.wait_for_plugin_load(plugin_key, std::chrono::seconds(120), error);
}

std::shared_ptr<PluginCapabilityInterface> find_capability(PluginManager& manager, const std::string& plugin_key,
                                                           const std::string& name)
{ return manager.get_plugin_capability({PluginCapabilityType::Unknown, name, plugin_key}, /*only_enabled=*/false); }

std::vector<std::shared_ptr<PluginCapabilityInterface>> capabilities_of(PluginManager& manager, const std::string& plugin_key)
{
    return manager.get_plugin_capabilities(plugin_key, PluginCapabilityType::Unknown, /*only_enabled=*/false);
}

PluginDescriptor descriptor_of(PluginManager& manager, const std::string& plugin_key)
{
    PluginDescriptor descriptor;
    manager.try_get_plugin_descriptor(plugin_key, descriptor);
    return descriptor;
}

} // namespace

TEST_CASE("A discovered script plugin loads and materializes its capability", "[PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("lifecycle-load");
    write_plugin(data_dir_guard, "Echo_Plugin", ECHO_PLUGIN_SOURCE);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    PluginDescriptor descriptor;
    REQUIRE(manager.try_get_valid_plugin_descriptor("Echo_Plugin", descriptor));
    CHECK(descriptor.name == "Echo Plugin");

    std::string error;
    REQUIRE(load_and_wait(manager, "Echo_Plugin", error));
    INFO("load error: " << error);
    CHECK(error.empty());

    CHECK(manager.is_plugin_loaded("Echo_Plugin"));
    CHECK(manager.get_plugin_load_error("Echo_Plugin").empty());

    const auto capabilities = capabilities_of(manager, "Echo_Plugin");
    REQUIRE(capabilities.size() == 1);

    const auto& echo = capabilities.front();
    CHECK(echo->name() == "Echo");
    CHECK(echo->type() == PluginCapabilityType::Script);
    CHECK(echo->is_enabled());
    CHECK(echo->audit_plugin_key() == "Echo_Plugin");

    CHECK(manager.get_plugin_capability({PluginCapabilityType::Script, "Echo", "Echo_Plugin"}) == echo);

    manager.unload_plugin("Echo_Plugin");
}

TEST_CASE("Plugin manager can initialize again after shutdown", "[PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("lifecycle-reinitialize");
    write_plugin(data_dir_guard, "Echo_Plugin", ECHO_PLUGIN_SOURCE);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    manager.shutdown();

    REQUIRE(manager.initialize());
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "Echo_Plugin", error));
    CHECK(manager.is_plugin_loaded("Echo_Plugin"));

    manager.unload_plugin("Echo_Plugin");
}

TEST_CASE("Duplicate discovered plugin keys are reported", "[PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("lifecycle-duplicate-key");
    for (const char* directory_name : {"first", "second"}) {
        const fs::path plugin_dir = data_dir_guard.plugins_dir() / directory_name;
        fs::create_directories(plugin_dir);
        std::ofstream out((plugin_dir / "Shared.py").string(), std::ios::binary);
        out << ECHO_PLUGIN_SOURCE;
    }

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    PluginDescriptor descriptor;
    REQUIRE(manager.try_get_plugin_descriptor("Shared", descriptor));
    CHECK(descriptor.has_error());
    CHECK(descriptor.normalized_error().find("Duplicate plugin key") != std::string::npos);
}

TEST_CASE("Unloading a plugin drops the package and its capabilities", "[PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("lifecycle-unload");
    write_plugin(data_dir_guard, "Echo_Plugin", ECHO_PLUGIN_SOURCE);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "Echo_Plugin", error));
    REQUIRE(manager.is_plugin_loaded("Echo_Plugin"));

    CHECK(manager.unload_plugin("Echo_Plugin"));

    CHECK_FALSE(manager.is_plugin_loaded("Echo_Plugin"));
    CHECK(manager.get_plugin_capabilities("Echo_Plugin").empty());
    CHECK(manager.get_plugin_capability({PluginCapabilityType::Script, "Echo", "Echo_Plugin"}) == nullptr);

    // The package stays discovered, but nothing capability-shaped survives the unload.
    const PluginDescriptor descriptor = descriptor_of(manager, "Echo_Plugin");
    CHECK(descriptor.plugin_key == "Echo_Plugin");
    CHECK(capabilities_of(manager, "Echo_Plugin").empty());
}

TEST_CASE("Python module release removes package submodules and owned sys.path", "[PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("module-release");
    const fs::path package_root = data_dir_guard.dir / "reload_package";
    fs::create_directories(package_root);

    auto write_package = [&](const std::string& value) {
        std::ofstream init((package_root / "__init__.py").string());
        init << "import reload_helper\nfrom . import sub\nVALUE = sub.VALUE\n";
        std::ofstream sub((package_root / "sub.py").string());
        sub << "VALUE = " << value << "\n";
        std::ofstream helper((package_root.parent_path() / "reload_helper.py").string());
        helper << "VALUE = 'helper'\n";
    };

    write_package("'old'");

    PythonInterpreter& interpreter = PythonInterpreter::instance();
    std::vector<std::string> paths;
    std::vector<std::string> modules;
    std::string              error;
    PyObject*                module = interpreter.load_module_from_directory(
        package_root.parent_path().string(), "reload_package", error, &paths, &modules);
    REQUIRE(module != nullptr);
    INFO("module load error: " << error);
    REQUIRE(error.empty());
    REQUIRE(paths.size() == 1);

    {
        PythonGILState gil;
        REQUIRE(gil);
        PyObject* modules = PyImport_GetModuleDict();
        REQUIRE(modules != nullptr);
        CHECK(PyDict_GetItemString(modules, "reload_package") != nullptr);
        CHECK(PyDict_GetItemString(modules, "reload_package.sub") != nullptr);
        CHECK(PyDict_GetItemString(modules, "reload_helper") != nullptr);
    }

    Plugin loaded;
    loaded.module           = module;
    loaded.module_name      = "reload_package";
    loaded.plugin_sys_paths = paths;
    loaded.plugin_modules   = modules;
    loaded.release_module();

    {
        PythonGILState gil;
        REQUIRE(gil);
        PyObject* modules = PyImport_GetModuleDict();
        REQUIRE(modules != nullptr);
        CHECK(PyDict_GetItemString(modules, "reload_package") == nullptr);
        CHECK(PyDict_GetItemString(modules, "reload_package.sub") == nullptr);
        CHECK(PyDict_GetItemString(modules, "reload_helper") == nullptr);

        PyObject* sys_path = PySys_GetObject("path");
        REQUIRE(sys_path != nullptr);
        PyObjectPtr path(PyUnicode_DecodeFSDefault(paths.front().c_str()));
        REQUIRE(path);
        CHECK(PySequence_Contains(sys_path, path.get()) == 0);
    }

    // Ensure the next import executes the new submodule rather than reusing a stale package child.
    write_package("'new'");
    boost::system::error_code ec;
    fs::remove_all(package_root / "__pycache__", ec);

    paths.clear();
    modules.clear();
    module = interpreter.load_module_from_directory(
        package_root.parent_path().string(), "reload_package", error, &paths, &modules);
    REQUIRE(module != nullptr);
    REQUIRE(error.empty());

    {
        PythonGILState gil;
        REQUIRE(gil);
        PyObjectPtr value(PyObject_GetAttrString(module, "VALUE"));
        REQUIRE(value);
        CHECK(std::string(PyUnicode_AsUTF8(value.get())) == "new");
    }

    loaded.module           = module;
    loaded.module_name      = "reload_package";
    loaded.plugin_sys_paths = paths;
    loaded.plugin_modules   = modules;
    loaded.release_module();
}

TEST_CASE("A capability disabled in the sidecar loads disabled", "[PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data_dir_guard("lifecycle-disabled");
    const fs::path plugin_dir = write_plugin(data_dir_guard, "Echo_Plugin", ECHO_PLUGIN_SOURCE);

    // Pre-seed the sidecar with the capability disabled, as a previous session would have.
    PluginInstallState state;
    state.installed_from    = "local";
    state.installed_version = "1.0";
    state.plugin_name       = "Echo Plugin";
    state.enabled           = true;
    state.capabilities      = {{"Echo", false}};
    REQUIRE(write_install_state(plugin_dir, state));

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "Echo_Plugin", error));
    REQUIRE(manager.is_plugin_loaded("Echo_Plugin"));

    // The capability still materializes — it is loaded, but logically disabled, so consumers skip it.
    const auto echo = find_capability(manager, "Echo_Plugin", "Echo");
    REQUIRE(echo != nullptr);
    CHECK_FALSE(echo->is_enabled());

    CHECK(manager.get_plugin_capabilities("Echo_Plugin", PluginCapabilityType::Unknown, /*only_enabled=*/true).empty());
    CHECK(manager.get_plugin_capabilities("Echo_Plugin", PluginCapabilityType::Unknown, /*only_enabled=*/false).size() == 1);

    // An empty load request must preserve the persisted disabled state even when the package is
    // already loaded.
    std::string no_request_error;
    REQUIRE(load_and_wait(manager, "Echo_Plugin", no_request_error));
    CHECK_FALSE(find_capability(manager, "Echo_Plugin", "Echo")->is_enabled());

    manager.unload_plugin("Echo_Plugin");
}

TEST_CASE("Disabling a capability round-trips through the sidecar and survives a reload", "[PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data_dir_guard("lifecycle-roundtrip");
    const fs::path plugin_dir = write_plugin(data_dir_guard, "Echo_Plugin", ECHO_PLUGIN_SOURCE);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "Echo_Plugin", error));
    REQUIRE(find_capability(manager, "Echo_Plugin", "Echo")->is_enabled());

    // Disabling writes the choice through to .install_state.json.
    manager.set_capability_enabled({PluginCapabilityType::Unknown, "Echo", "Echo_Plugin"}, false);
    CHECK_FALSE(find_capability(manager, "Echo_Plugin", "Echo")->is_enabled());

    PluginInstallState persisted;
    REQUIRE(read_install_state(plugin_dir, persisted));
    REQUIRE(persisted.capabilities.size() == 1);
    CHECK(persisted.capabilities.front().first == "Echo");
    CHECK_FALSE(persisted.capabilities.front().second);

    // Unload and reload: the user's choice must survive.
    REQUIRE(manager.unload_plugin("Echo_Plugin"));

    std::string reload_error;
    REQUIRE(load_and_wait(manager, "Echo_Plugin", reload_error));

    const auto echo = find_capability(manager, "Echo_Plugin", "Echo");
    REQUIRE(echo != nullptr);
    CHECK_FALSE(echo->is_enabled());

    manager.unload_plugin("Echo_Plugin");
}

TEST_CASE("A capability disabled after load stays disabled when rediscovered and reloaded", "[PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("lifecycle-reload-live");
    write_plugin(data_dir_guard, "Echo_Plugin", ECHO_PLUGIN_SOURCE);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "Echo_Plugin", error));

    manager.set_capability_enabled({PluginCapabilityType::Unknown, "Echo", "Echo_Plugin"}, false);
    REQUIRE(manager.unload_plugin("Echo_Plugin"));

    // Rediscover, as the app does when a plugin is toggled off and back on. The enable flags the
    // loader seeds from must come from the sidecar just written, not from a stale cache.
    manager.discover_plugins(/*async=*/false, /*clear=*/false);

    std::string reload_error;
    REQUIRE(load_and_wait(manager, "Echo_Plugin", reload_error));

    const auto echo = find_capability(manager, "Echo_Plugin", "Echo");
    REQUIRE(echo != nullptr);
    CHECK_FALSE(echo->is_enabled());

    manager.unload_plugin("Echo_Plugin");
}

TEST_CASE("Re-enabling a disabled capability writes the sidecar back", "[PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data_dir_guard("lifecycle-reenable");
    const fs::path plugin_dir = write_plugin(data_dir_guard, "Echo_Plugin", ECHO_PLUGIN_SOURCE);

    PluginInstallState state;
    state.installed_from = "local";
    state.plugin_name    = "Echo Plugin";
    state.enabled        = true;
    state.capabilities   = {{"Echo", false}};
    REQUIRE(write_install_state(plugin_dir, state));

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "Echo_Plugin", error));

    // An explicit request overrides the persisted disabled state, including on a fresh load.
    REQUIRE(manager.unload_plugin("Echo_Plugin"));
    std::string enable_error;
    REQUIRE(load_and_wait(manager, "Echo_Plugin", enable_error, {"Echo"}));

    const auto echo = find_capability(manager, "Echo_Plugin", "Echo");
    REQUIRE(echo != nullptr);
    CHECK(echo->is_enabled());

    PluginInstallState persisted;
    REQUIRE(read_install_state(plugin_dir, persisted));
    REQUIRE(persisted.capabilities.size() == 1);
    CHECK(persisted.capabilities.front().first == "Echo");
    CHECK(persisted.capabilities.front().second);

    manager.unload_plugin("Echo_Plugin");
}

TEST_CASE("Overwriting a local plugin unloads its live module", "[PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("lifecycle-overwrite");
    const fs::path package_dir = data_dir_guard.dir / "packages";
    fs::create_directories(package_dir);
    const fs::path package = package_dir / "Echo_Plugin.py";
    {
        std::ofstream out(package.string(), std::ios::binary);
        out << ECHO_PLUGIN_SOURCE;
    }

    PluginManager& manager = PluginManager::instance();
    std::string error;
    REQUIRE(manager.install_plugin(package, error));
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    REQUIRE(load_and_wait(manager, "Echo_Plugin", error));
    REQUIRE(manager.is_plugin_loaded("Echo_Plugin"));

    REQUIRE(manager.install_plugin(package, error));
    CHECK_FALSE(manager.is_plugin_loaded("Echo_Plugin"));
}

TEST_CASE("capabilities_to_enable selects which capabilities come up enabled", "[PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // Two capabilities in one package; only the second is requested.
    const char* const two_cap_source = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Duo Plugin"
# version = "1.0"
# type = "script"
# ///
import orca

class Alpha(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Alpha"

    def execute(self, ctx):
        return orca.ExecutionResult.success()

class Beta(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Beta"

    def execute(self, ctx):
        return orca.ExecutionResult.success()

@orca.plugin
class DuoPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(Alpha)
        orca.register_capability(Beta)
)PY";

    ScopedDataDir data_dir_guard("lifecycle-select");
    write_plugin(data_dir_guard, "Duo_Plugin", two_cap_source);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "Duo_Plugin", error, {"Beta"}));

    REQUIRE(capabilities_of(manager, "Duo_Plugin").size() == 2);

    const auto alpha = find_capability(manager, "Duo_Plugin", "Alpha");
    const auto beta  = find_capability(manager, "Duo_Plugin", "Beta");
    REQUIRE(alpha != nullptr);
    REQUIRE(beta != nullptr);

    CHECK_FALSE(alpha->is_enabled());
    CHECK(beta->is_enabled());

    manager.unload_plugin("Duo_Plugin");
}

TEST_CASE("A cancelled load keeps blocking wait_for_all_plugin_loads until the worker exits", "[PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // Stalls inside the module import, so the detached load worker is still executing Python while
    // the test cancels it.
    const char* const slow_source = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Slow Load Plugin"
# version = "1.0"
# type = "script"
# ///
import time

import orca

time.sleep(2)

class Slow(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Slow"

    def execute(self, ctx):
        return orca.ExecutionResult.success()

@orca.plugin
class SlowPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(Slow)
)PY";

    ScopedDataDir data_dir_guard("lifecycle-cancel");
    write_plugin(data_dir_guard, "Slow_Load_Plugin", slow_source);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    manager.load_plugin("Slow_Load_Plugin", /*skip_deps=*/true);

    // The key is registered before the worker is spawned, so this is not a race.
    REQUIRE(manager.is_plugin_load_in_progress("Slow_Load_Plugin"));
    REQUIRE(manager.cancel_plugin_load("Slow_Load_Plugin"));

    // Cancelling must not release the worker's slot. shutdown() unloads everything and GUI_App
    // finalizes the interpreter as soon as this wait returns, so reporting "no loads in progress"
    // while the worker is still inside Python is how the app crashes on exit.
    CHECK(manager.is_plugin_load_in_progress("Slow_Load_Plugin"));
    CHECK_FALSE(manager.wait_for_all_plugin_loads(std::chrono::milliseconds(0)));

    // The worker releases the slot itself, once it has unwound.
    CHECK(manager.wait_for_all_plugin_loads(std::chrono::seconds(60)));
    CHECK_FALSE(manager.is_plugin_load_in_progress("Slow_Load_Plugin"));
    CHECK_FALSE(manager.is_plugin_loaded("Slow_Load_Plugin"));
}

TEST_CASE("Loading an unknown plugin key records an error instead of crashing", "[PluginLifecycle][Python]")
{
    // discover_plugins() initializes the plugin system (and with it the interpreter), so this
    // needs the same explicit teardown as the load tests.
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("lifecycle-missing");

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    // Rejected synchronously: no worker thread is spawned for an unknown key.
    manager.load_plugin("No_Such_Plugin", /*skip_deps=*/true);

    CHECK_FALSE(manager.is_plugin_loaded("No_Such_Plugin"));
    CHECK(manager.get_plugin_load_error("No_Such_Plugin") == "Plugin not found: No_Such_Plugin");
    CHECK(manager.get_plugin_capabilities("No_Such_Plugin").empty());
}

TEST_CASE("The startup auto-load list only contains packages whose sidecar enables them", "[PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("lifecycle-autoload");

    // No sidecar at all: never installed through Orca, so it carries no auto-load intent.
    write_plugin(data_dir_guard, "Bare_Plugin", ECHO_PLUGIN_SOURCE);

    // Sidecar with enabled = true: auto-loads.
    const fs::path on_dir = write_plugin(data_dir_guard, "Enabled_Plugin", ECHO_PLUGIN_SOURCE);
    PluginInstallState on_state;
    on_state.installed_from = "local";
    on_state.enabled        = true;
    REQUIRE(write_install_state(on_dir, on_state));

    // Sidecar with enabled = false: the user turned it off.
    const fs::path off_dir = write_plugin(data_dir_guard, "Disabled_Plugin", ECHO_PLUGIN_SOURCE);
    PluginInstallState off_state;
    off_state.installed_from = "local";
    off_state.enabled        = false;
    REQUIRE(write_install_state(off_dir, off_state));

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    const std::vector<std::string> keys = manager.get_enabled_plugin_keys();

    CHECK(std::find(keys.begin(), keys.end(), "Enabled_Plugin") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "Disabled_Plugin") == keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "Bare_Plugin") == keys.end());
}

TEST_CASE("Signing out drops every cloud plugin row, installed or not", "[PluginLifecycle][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("lifecycle-signout");

    // A local package, which must survive sign-out.
    write_plugin(data_dir_guard, "Echo_Plugin", ECHO_PLUGIN_SOURCE);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    // Two cloud rows: one merely available (nothing installed), one with a local package behind it —
    // the case that used to linger, unloaded but still listed, until the user hit refresh.
    PluginDescriptor available;
    available.plugin_key = "11111111-1111-1111-1111-111111111111";
    available.name       = "Available Cloud Plugin";
    available.cloud      = CloudPluginState{available.plugin_key, /*installed=*/false, false, false, false};

    PluginDescriptor installed;
    installed.plugin_key  = "22222222-2222-2222-2222-222222222222";
    installed.name        = "Installed Cloud Plugin";
    installed.plugin_root = (data_dir_guard.plugins_dir() / "_subscribed" / "user" / installed.plugin_key).string();
    installed.cloud       = CloudPluginState{installed.plugin_key, /*installed=*/true, false, false, false};

    manager.update_cloud_metadata({available, installed});

    const auto has_key = [&manager](const std::string& key) {
        PluginDescriptor descriptor;
        return manager.try_get_plugin_descriptor(key, descriptor);
    };

    REQUIRE(has_key(available.plugin_key));
    REQUIRE(has_key(installed.plugin_key));
    REQUIRE(has_key("Echo_Plugin"));

    // Sign out. The per-user _subscribed directory stops being scanned, so both cloud rows are now
    // stale and must go — not just the one with nothing installed behind it.
    manager.unload_cloud_plugins();
    manager.clear_cloud_plugin_metadata();
    manager.set_cloud_user("");

    CHECK_FALSE(has_key(available.plugin_key));
    CHECK_FALSE(has_key(installed.plugin_key));
    CHECK(has_key("Echo_Plugin"));
}

TEST_CASE("Unloading a plugin that is not loaded is a no-op", "[PluginLifecycle]")
{
    ScopedDataDir data_dir_guard("lifecycle-noop-unload");

    PluginManager& manager = PluginManager::instance();

    // Current behavior: unloading an unknown key succeeds (it fires the unload callbacks and
    // reports success) rather than reporting "nothing to unload".
    CHECK(manager.unload_plugin("No_Such_Plugin"));
    CHECK_FALSE(manager.is_plugin_loaded("No_Such_Plugin"));
}
