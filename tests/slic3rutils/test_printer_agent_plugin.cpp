#include <catch2/catch_all.hpp>

#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/Utils/NetworkAgentFactory.hpp>
#include <libslic3r/Utils.hpp> // for set_data_dir

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <catch2/catch_session.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// why: embedding the fake plugin keeps this test self-contained. The PEP 723
// block declares a printer-connection plugin, and the decorated plugin package
// registers one PrinterAgentBase capability whose AgentInfo.id is the registry
// key asserted below.
constexpr const char* kFakePluginSource = R"PY(# /// script
# requires-python = ">=3.12"
# dependencies = []
#
# [tool.orcaslicer.plugin]
# name = "Lifecycle Test Agent"
# description = "Minimal printer-agent plugin for the lifecycle test."
# author = "tests"
# version = "1.0.0"
# type = "printer-connection"
# ///
import orca


class LifecycleTestAgentCapability(orca.printer_agent.PrinterAgentBase):
    def get_name(self):
        return "Lifecycle Test Agent"

    def get_agent_info(self):
        return orca.printer_agent.AgentInfo(
            id="lifecycle-test-agent",
            name="Lifecycle Test Agent",
            version="1.0.0",
            description="Lifecycle test printer agent",
        )


@orca.plugin
class LifecycleTestPlugin(orca.base):
    def register_capabilities(self):
        orca.register_capability(LifecycleTestAgentCapability)
)PY";

// why: in production GUI_App::init_plugin_gui_wiring subscribes the agent-registry
// callbacks; the test binary has no GUI, so install the same UNLOAD-side wiring
// once so the tests exercise the production deregister-on-unload path.
// note: the load-side (register) wiring is deliberately NOT installed - the two
// concurrent load_plugin calls in the duplicate-id test would race for the id;
// the tests register manually, in a deterministic order, instead.
void install_agent_registry_wiring()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    PluginManager& mgr = PluginManager::instance();
    mgr.subscribe_on_unload_callback(NetworkAgentFactory::deregister_python_plugin);
    mgr.subscribe_on_capability_unload_callback([](const PluginCapabilityId& capability) {
        if (capability.type == PluginCapabilityType::PrinterConnection)
            NetworkAgentFactory::deregister_python_printer_agent(capability.plugin_key, capability.name);
    });
}

} // namespace

// ===========================================================================
// PRINTER-AGENT PLUGIN LIFECYCLE: load, register, unload, deregister.
//
// This test uses its own executable because load_plugin runs on a detached worker
// thread that needs the GIL released on the main thread (the PythonInterpreter
// model). slic3rutils_tests' other Python tests hold the GIL on the main thread
// via a bare scoped_interpreter; the two models can't share one process.
//
// When bundled Python is unavailable in the test environment, the test is
// skipped so source-only or partially staged builds can still run the rest of
// the suite.
// ===========================================================================
TEST_CASE("plugin lifecycle: printer-agent load registers and unload deregisters", "[plugin][lifecycle][Python]")
{
    const std::string plugin_key = "LifecycleTestAgent";   // entry-file stem
    const std::string agent_id   = "lifecycle-test-agent"; // AgentInfo.id from the plugin

    // Stage a throwaway data directory. Plugins are discovered under
    // <data_dir>/orca_plugins, so this controls which plugin is loaded.
    const fs::path data_dir   = fs::temp_directory_path() / "orca-plugin-lifecycle-test";
    const fs::path plugin_dir = data_dir / "orca_plugins" / plugin_key;
    {
        boost::system::error_code ec;
        fs::remove_all(data_dir, ec); // clear any stale run
    }
    fs::create_directories(plugin_dir);
    {
        std::ofstream out((plugin_dir / (plugin_key + ".py")).string(), std::ios::binary);
        out << kFakePluginSource;
    }
    // why: best-effort cleanup even if an assertion throws.
    struct DirGuard
    {
        fs::path p;
        ~DirGuard()
        {
            boost::system::error_code ec;
            fs::remove_all(p, ec);
        }
    } guard{data_dir};

    Slic3r::set_data_dir(data_dir.string());

    // Initialize the plugin system on this thread. If the bundled Python home
    // is not reachable, skip gracefully.
    PluginManager& mgr = PluginManager::instance();
    if (!mgr.initialize())
        SKIP("PythonInterpreter could not initialize (bundled Python home not found in this environment)");
    install_agent_registry_wiring();

    // Discover synchronously so the catalog holds the descriptor before loading.
    mgr.discover_plugins(/*async=*/false, /*clear=*/true);
    INFO("expected plugin_key (entry-file stem): " << plugin_key);
    PluginDescriptor descriptor;
    REQUIRE(mgr.try_get_valid_plugin_descriptor(plugin_key, descriptor));

    // Load on the worker thread and block until it finishes.
    std::string error;
    mgr.load_plugin(plugin_key, /*skip_deps=*/true);
    const bool loaded = mgr.wait_for_plugin_load(plugin_key, std::chrono::seconds(60), error);
    INFO("plugin load error: " << error);
    REQUIRE(loaded);
    REQUIRE(mgr.is_plugin_loaded(plugin_key));

    // Resolve the one PrinterConnection capability and register it as an agent.
    auto caps = mgr.get_plugin_capabilities(plugin_key, PluginCapabilityType::PrinterConnection);
    REQUIRE(caps.size() == 1);
    REQUIRE(caps[0] != nullptr);
    NetworkAgentFactory::register_python_printer_agent(plugin_key, caps[0]->name());

    // The AgentInfo.id returned by the plugin is now in the registry.
    CHECK(NetworkAgentFactory::is_printer_agent_registered(agent_id));

    // Unloading the plugin deregisters its Python-backed agent.
    REQUIRE(mgr.unload_plugin(plugin_key));

    // The registry no longer contains the agent id after unload.
    CHECK_FALSE(NetworkAgentFactory::is_printer_agent_registered(agent_id));
}

namespace {

// why: duplicate-id coverage needs two distinct plugins with different package
// keys and classes while both return the same AgentInfo.id.
std::string make_agent_plugin_source(const std::string& suffix, const std::string& display_name, const std::string& agent_id)
{
    return std::string{}
        + "# /// script\n"
        + "# requires-python = \">=3.12\"\n"
        + "# dependencies = []\n"
        + "#\n"
        + "# [tool.orcaslicer.plugin]\n"
        + "# name = \"" + display_name + "\"\n"
        + "# description = \"Duplicate-id fake printer-agent plugin.\"\n"
        + "# author = \"tests\"\n"
        + "# version = \"1.0.0\"\n"
        + "# type = \"printer-connection\"\n"
        + "# ///\n"
        + "import orca\n"
        + "\n\n"
        + "class Cap" + suffix + "(orca.printer_agent.PrinterAgentBase):\n"
        + "    def get_name(self):\n"
        + "        return \"" + display_name + "\"\n"
        + "\n"
        + "    def get_agent_info(self):\n"
        + "        return orca.printer_agent.AgentInfo(\n"
        + "            id=\"" + agent_id + "\", name=\"" + display_name + "\",\n"
        + "            version=\"1.0.0\", description=\"duplicate id test\")\n"
        + "\n\n"
        + "@orca.plugin\n"
        + "class Plugin" + suffix + "(orca.base):\n"
        + "    def register_capabilities(self):\n"
        + "        orca.register_capability(Cap" + suffix + ")\n";
}

void stage_plugin(const fs::path& data_dir, const std::string& plugin_key, const std::string& source)
{
    const fs::path dir = data_dir / "orca_plugins" / plugin_key;
    fs::create_directories(dir);
    std::ofstream out((dir / (plugin_key + ".py")).string(), std::ios::binary);
    out << source;
}

} // namespace

// ===========================================================================
// DUPLICATE PRINTER-AGENT IDS
// When two loaded plugins return the same AgentInfo.id, the first registration
// keeps ownership. Unloading it removes the id, and the rejected plugin is not
// promoted automatically. Manual re-registration is required.
// ===========================================================================
TEST_CASE("duplicate agent id is rejected and the winner is not clobbered", "[plugin][lifecycle][Python]")
{
    const std::string key_a  = "DuplicateIdAgentA"; // winner, registered first
    const std::string key_b  = "DuplicateIdAgentB"; // duplicate, rejected
    const std::string dup_id = "duplicate-id-agent"; // both plugins return this AgentInfo.id

    const fs::path data_dir = fs::temp_directory_path() / "orca-duplicate-agent-test";
    {
        boost::system::error_code ec;
        fs::remove_all(data_dir, ec);
    }
    stage_plugin(data_dir, key_a, make_agent_plugin_source("A", "Duplicate Id Agent A", dup_id));
    stage_plugin(data_dir, key_b, make_agent_plugin_source("B", "Duplicate Id Agent B", dup_id));
    struct DirGuard
    {
        fs::path p;
        ~DirGuard()
        {
            boost::system::error_code ec;
            fs::remove_all(p, ec);
        }
    } guard{data_dir};

    Slic3r::set_data_dir(data_dir.string());

    PluginManager& mgr = PluginManager::instance();
    if (!mgr.initialize())
        SKIP("PythonInterpreter could not initialize (bundled Python home not found in this environment)");
    install_agent_registry_wiring();

    mgr.discover_plugins(/*async=*/false, /*clear=*/true);
    PluginDescriptor descriptor_a;
    PluginDescriptor descriptor_b;
    REQUIRE(mgr.try_get_valid_plugin_descriptor(key_a, descriptor_a));
    REQUIRE(mgr.try_get_valid_plugin_descriptor(key_b, descriptor_b));

    std::string error;
    mgr.load_plugin(key_a, /*skip_deps=*/true);
    mgr.load_plugin(key_b, /*skip_deps=*/true);
    const bool loaded_a = mgr.wait_for_plugin_load(key_a, std::chrono::seconds(60), error);
    INFO("plugin A load error: " << error);
    REQUIRE(loaded_a);
    const bool loaded_b = mgr.wait_for_plugin_load(key_b, std::chrono::seconds(60), error);
    INFO("plugin B load error: " << error);
    REQUIRE(loaded_b);

    auto caps_a = mgr.get_plugin_capabilities(key_a, PluginCapabilityType::PrinterConnection);
    auto caps_b = mgr.get_plugin_capabilities(key_b, PluginCapabilityType::PrinterConnection);
    REQUIRE(caps_a.size() == 1);
    REQUIRE(caps_b.size() == 1);

    // Register A first as the owner, then B with the duplicate id.
    NetworkAgentFactory::register_python_printer_agent(key_a, caps_a[0]->name());
    NetworkAgentFactory::register_python_printer_agent(key_b, caps_b[0]->name());

    // The shared id is registered, and still owned by A; B did not replace it.
    CHECK(NetworkAgentFactory::is_printer_agent_registered(dup_id));
    const PrinterAgentInfo* info = NetworkAgentFactory::get_printer_agent_info(dup_id);
    REQUIRE(info != nullptr);
    CHECK(info->plugin_identifier.find(key_a) != std::string::npos); // owned by A
    CHECK(info->plugin_identifier.find(key_b) == std::string::npos); // B never took ownership

    // Unload the owner. The id is not promoted to the still loaded duplicate.
    REQUIRE(mgr.unload_plugin(key_a));
    CHECK_FALSE(NetworkAgentFactory::is_printer_agent_registered(dup_id));

    mgr.unload_plugin(key_b); // the duplicate was loaded but never registered
}

// ===========================================================================
// NATIVE (BUILT-IN) AGENT ID COLLISION
// A plugin may not hijack a built-in agent id (e.g. "bbl"). The built-in keeps
// ownership and the plugin's agent is rejected.
// ===========================================================================
TEST_CASE("printer-agent plugin cannot claim a built-in agent id", "[plugin][lifecycle][Python]")
{
    // Register the native built-ins so "bbl"/"orca" occupy the registry.
    NetworkAgentFactory::register_all_agents();
    REQUIRE(NetworkAgentFactory::is_printer_agent_registered(BBL_PRINTER_AGENT_ID));

    const std::string plugin_key = "BuiltinClashAgent";

    const fs::path data_dir = fs::temp_directory_path() / "orca-builtin-clash-test";
    {
        boost::system::error_code ec;
        fs::remove_all(data_dir, ec);
    }
    stage_plugin(data_dir, plugin_key, make_agent_plugin_source("Clash", "Builtin Clash", BBL_PRINTER_AGENT_ID));
    struct DirGuard
    {
        fs::path p;
        ~DirGuard()
        {
            boost::system::error_code ec;
            fs::remove_all(p, ec);
        }
    } guard{data_dir};

    Slic3r::set_data_dir(data_dir.string());

    PluginManager& mgr = PluginManager::instance();
    if (!mgr.initialize())
        SKIP("PythonInterpreter could not initialize (bundled Python home not found in this environment)");
    install_agent_registry_wiring();

    mgr.discover_plugins(/*async=*/false, /*clear=*/true);
    PluginDescriptor descriptor;
    REQUIRE(mgr.try_get_valid_plugin_descriptor(plugin_key, descriptor));

    std::string error;
    mgr.load_plugin(plugin_key, /*skip_deps=*/true);
    REQUIRE(mgr.wait_for_plugin_load(plugin_key, std::chrono::seconds(60), error));

    auto caps = mgr.get_plugin_capabilities(plugin_key, PluginCapabilityType::PrinterConnection);
    REQUIRE(caps.size() == 1);
    NetworkAgentFactory::register_python_printer_agent(plugin_key, caps[0]->name());

    // "bbl" is still the native built-in, not the plugin.
    const PrinterAgentInfo* info = NetworkAgentFactory::get_printer_agent_info(BBL_PRINTER_AGENT_ID);
    REQUIRE(info != nullptr);
    CHECK_FALSE(info->is_plugin());
    CHECK(info->plugin_identifier.find(plugin_key) == std::string::npos);

    mgr.unload_plugin(plugin_key);
}

// ===========================================================================
// RE-REGISTERING THE SAME CAPABILITY IS A REFRESH, NOT A DUPLICATE
// The guard rejects only a DIFFERENT owner. The same capability registering its
// own id again must stay registered (insert_or_assign refreshes it in place).
// ===========================================================================
TEST_CASE("re-registering the same capability keeps its agent registered", "[plugin][lifecycle][Python]")
{
    const std::string plugin_key = "ReRegisterAgent";
    const std::string agent_id   = "re-register-agent";

    const fs::path data_dir = fs::temp_directory_path() / "orca-re-register-test";
    {
        boost::system::error_code ec;
        fs::remove_all(data_dir, ec);
    }
    stage_plugin(data_dir, plugin_key, make_agent_plugin_source("Re", "Re Register", agent_id));
    struct DirGuard
    {
        fs::path p;
        ~DirGuard()
        {
            boost::system::error_code ec;
            fs::remove_all(p, ec);
        }
    } guard{data_dir};

    Slic3r::set_data_dir(data_dir.string());

    PluginManager& mgr = PluginManager::instance();
    if (!mgr.initialize())
        SKIP("PythonInterpreter could not initialize (bundled Python home not found in this environment)");
    install_agent_registry_wiring();

    mgr.discover_plugins(/*async=*/false, /*clear=*/true);
    PluginDescriptor descriptor;
    REQUIRE(mgr.try_get_valid_plugin_descriptor(plugin_key, descriptor));

    std::string error;
    mgr.load_plugin(plugin_key, /*skip_deps=*/true);
    REQUIRE(mgr.wait_for_plugin_load(plugin_key, std::chrono::seconds(60), error));

    auto caps = mgr.get_plugin_capabilities(plugin_key, PluginCapabilityType::PrinterConnection);
    REQUIRE(caps.size() == 1);

    NetworkAgentFactory::register_python_printer_agent(plugin_key, caps[0]->name());
    REQUIRE(NetworkAgentFactory::is_printer_agent_registered(agent_id));
    const PrinterAgentInfo* first = NetworkAgentFactory::get_printer_agent_info(agent_id);
    REQUIRE(first != nullptr);
    const std::string owner = first->plugin_identifier;

    // The same capability registering again is a refresh: still registered, same owner, not rejected.
    NetworkAgentFactory::register_python_printer_agent(plugin_key, caps[0]->name());
    CHECK(NetworkAgentFactory::is_printer_agent_registered(agent_id));
    const PrinterAgentInfo* second = NetworkAgentFactory::get_printer_agent_info(agent_id);
    REQUIRE(second != nullptr);
    CHECK(second->plugin_identifier == owner);

    mgr.unload_plugin(plugin_key);
}

// why: this binary embeds CPython through the PythonInterpreter singleton, which
// lives for the full process. Normal static destruction can tear Python down
// while C++ objects still hold Python handles, producing a Windows heap
// corruption after the assertions have finished. The app has an ordered shutdown
// path; this test harness does not, so it returns the Catch2 result through
// _Exit after flushing output.
int main(int argc, char* argv[])
{
    const int result = Catch::Session().run(argc, argv);
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    std::_Exit(result);
}
