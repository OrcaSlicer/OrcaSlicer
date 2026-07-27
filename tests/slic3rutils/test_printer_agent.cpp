#include <catch2/catch_all.hpp>

#include <slic3r/Utils/MoonrakerPrinterAgent.hpp>
#include <slic3r/Utils/NetworkAgentFactory.hpp>
#include <slic3r/plugin/PythonPluginBridge.hpp>

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <memory>
#include <string>

using namespace Slic3r;
namespace py = pybind11;

TEST_CASE("unit: Moonraker light name matching", "[unit][moonraker]")
{
    CHECK(moonraker_is_light_name("caselight"));
    CHECK(moonraker_is_light_name("LED_STRIP"));
    CHECK_FALSE(moonraker_is_light_name("beeper"));
    CHECK(moonraker_is_light_name("FLASHLIGHT_SWITCH"));
    CHECK(moonraker_is_light_name("MODLELIGHT_SWITCH"));
}

// ===========================================================================
// UNIT - printer-agent registry duplicate handling.
// Confirms a duplicate agent id is rejected so a plugin cannot shadow a built-in
// or previously registered agent.
// ===========================================================================
TEST_CASE("unit: printer-agent registry register / lookup / duplicate-reject", "[registry][unit]")
{
    // why: the registry is process-global state shared by the test binary, and
    // Catch2 may run cases in any order. Use an id that cannot collide with
    // built-ins or other cases. Avoid SECTIONs because each section re-runs the
    // body and would register the same id twice.
    const std::string id = "orca-test::registry-probe-7f3a";
    auto stub_factory = [](std::shared_ptr<ICloudServiceAgent>, const std::string&)
        -> std::shared_ptr<IPrinterAgent> { return nullptr; };

    REQUIRE_FALSE(NetworkAgentFactory::is_printer_agent_registered(id));

    REQUIRE(NetworkAgentFactory::register_printer_agent(id, "Registry Probe", stub_factory));
    REQUIRE(NetworkAgentFactory::is_printer_agent_registered(id));

    // Re-registering the same id is rejected and does not replace the entry.
    REQUIRE_FALSE(NetworkAgentFactory::register_printer_agent(id, "Impostor", stub_factory));

    // The first registration's display name survives the rejected duplicate.
    const PrinterAgentInfo* info = NetworkAgentFactory::get_printer_agent_info(id);
    REQUIRE(info != nullptr);
    CHECK(info->display_name == "Registry Probe");

    // It appears exactly once in the UI-population list.
    auto agents = NetworkAgentFactory::get_registered_printer_agents();
    int  count  = 0;
    for (const auto& a : agents)
        if (a.id == id)
            ++count;
    CHECK(count == 1);
}

// ===========================================================================
// INTEGRATION - the orca.printer_agent Python binding surface.
// Boots the embedded interpreter and asserts the C++ to Python contract that
// every printer-agent plugin subclasses. If a binding is renamed or removed,
// plugins fail at runtime even though C++ still compiles.
// ===========================================================================
namespace {

void ensure_python_initialized()
{
    // why: the `orca` module is embedded in this binary, so a bare interpreter
    // can import it without a bundled Python home. The app interpreter expects
    // that deployed layout, which is not present beside this test binary.
    if (!Py_IsInitialized()) {
        static py::scoped_interpreter interpreter;
        (void) interpreter;
    }
}

py::module_ import_orca_module()
{
    ensure_python_initialized();
    // Force PythonPluginBridge.cpp into the binary so the embedded
    // PYBIND11_EMBEDDED_MODULE(orca, ...) registration (incl. printer_agent) exists.
    (void) Slic3r::PythonPluginBridge::instance();
    return py::module_::import("orca");
}

} // namespace

TEST_CASE("integration: orca.printer_agent binding surface", "[integration][Python]")
{
    py::module_ orca = import_orca_module();

    REQUIRE(py::hasattr(orca, "printer_agent"));
    py::object pa = orca.attr("printer_agent");

    // The base class every printer-agent plugin subclasses.
    REQUIRE(py::hasattr(pa, "PrinterAgentBase"));
    py::object base = pa.attr("PrinterAgentBase");
    for (const char* method : { "get_agent_info", "connect_printer", "disconnect_printer",
                                "send_message", "start_discovery", "bind_detect",
                                "start_print", "get_filament_sync_mode" }) {
        CAPTURE(method);
        CHECK(py::hasattr(base, method));
    }

    // AgentInfo value type - the registry identity the host reads (id is the key).
    REQUIRE(py::hasattr(pa, "AgentInfo"));
    py::object info = pa.attr("AgentInfo")("moonraker", "Moonraker", "1.0", "test agent");
    CHECK(info.attr("id").cast<std::string>() == "moonraker");
    CHECK(info.attr("name").cast<std::string>() == "Moonraker");

    // FilamentSyncMode enum the host queries to pick pull vs subscription.
    REQUIRE(py::hasattr(pa, "FilamentSyncMode"));
    py::object mode = pa.attr("FilamentSyncMode");
    CHECK(py::hasattr(mode, "Pull"));
    CHECK(py::hasattr(mode, "Subscription"));
    CHECK(py::hasattr(mode, "None_"));

    // Plugin-type enum exposed at module root (host reads it without the GIL).
    CHECK(py::hasattr(orca, "PluginType"));
}

// ===========================================================================
// INTEGRATION - plugin-registration API and discovery-context guards.
// These are the symbols every plugin package uses: the @orca.plugin decorator,
// orca.base, orca.register_capability, and the capability base modules. Checking
// them in the lightweight embedded-interpreter test catches binding breakage
// before the plugin-loader test needs to run.
// ===========================================================================
TEST_CASE("integration: orca plugin-registration API surface + discovery-context guards", "[integration][Python]")
{
    py::module_ orca = import_orca_module();

    // Module-level surface every plugin package relies on.
    // note: no "gcode" module here - this branch has no G-code capability module;
    // PostProcessing exists only as a PluginType value.
    for (const char* name : { "plugin", "register_capability", "base", "PythonPluginBase",
                              "PluginType", "PluginResult", "script", "printer_agent", "host" }) {
        CAPTURE(name);
        CHECK(py::hasattr(orca, name));
    }

    // Plugin package base and capability base contract.
    CHECK(py::hasattr(orca.attr("base"), "register_capabilities"));
    py::object cap_base = orca.attr("PythonPluginBase");
    for (const char* method : { "get_name", "get_type", "on_load", "on_unload" }) {
        CAPTURE(method);
        CHECK(py::hasattr(cap_base, method));
    }

    // The script capability module exposes its own base class.
    CHECK(py::hasattr(orca.attr("script"), "ScriptPluginCapabilityBase"));

    // PluginType enum carries the values that route a capability, including PrinterConnection.
    // note: no PostProcessing value on this branch's binding.
    py::object types = orca.attr("PluginType");
    for (const char* value : { "PrinterConnection", "Script" }) {
        CAPTURE(value);
        CHECK(py::hasattr(types, value));
    }

    // note: this is testing behavior, not normal plugin loading.
    // These APIs should only work while Orca is actively loading a plugin.
    try {
        // Calls Python's orca.register_capability(0) from C++.
        // 0 is intentionally bogus. The important part is that there is no active
        // plugin load context, so the function should reject the call immediately.
        orca.attr("register_capability")(py::int_(0));

        // If the call above does NOT throw, the test fails here.
        FAIL("register_capability outside discovery context must raise");
    } catch (const py::error_already_set& error) {
        // pybind11 wraps Python exceptions as py::error_already_set.
        // This checks the Python exception type is ValueError.
        CHECK(error.matches(PyExc_ValueError));
    }

    try {
        // This is the function behind @orca.plugin.
        // Same logic as above.
        orca.attr("plugin")(py::int_(0));

        FAIL("@orca.plugin outside discovery context must raise");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_ValueError));
    }
}
