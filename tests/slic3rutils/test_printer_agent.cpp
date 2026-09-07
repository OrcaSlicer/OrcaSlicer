#include <catch2/catch_all.hpp>

#include <slic3r/Utils/BBLPrinterAgent.hpp>
#include <slic3r/Utils/MoonrakerPrinterAgent.hpp>
#include <slic3r/Utils/NetworkAgentFactory.hpp>
#include <slic3r/plugin/PythonPluginBridge.hpp>

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace Slic3r;
namespace py = pybind11;

class MoonrakerParserProbe : public MoonrakerPrinterAgent
{
public:
    using MoonrakerPrinterAgent::parse_nozzle_diameter;

    explicit MoonrakerParserProbe(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}
};

TEST_CASE("Moonraker parses nozzle diameter from configfile settings", "[unit][moonraker]")
{
    const auto response = nlohmann::json::parse(R"({
        "result": {
            "status": {
                "configfile": {
                    "settings": {
                        "extruder": {
                            "nozzle_diameter": 0.6
                        }
                    }
                }
            }
        }
    })");

    CHECK(MoonrakerParserProbe::parse_nozzle_diameter(response) == Catch::Approx(0.6f));
}

TEST_CASE("Moonraker parses nozzle diameter from raw config and tolerates missing data", "[unit][moonraker]")
{
    const auto raw_config_response = nlohmann::json::parse(R"({
        "result": {
            "status": {
                "configfile": {
                    "config": {
                        "extruder": {
                            "nozzle_diameter": "0.8"
                        }
                    }
                }
            }
        }
    })");
    const auto missing_response = nlohmann::json::object();

    CHECK(MoonrakerParserProbe::parse_nozzle_diameter(raw_config_response) == Catch::Approx(0.8f));
    CHECK(MoonrakerParserProbe::parse_nozzle_diameter(missing_response) == 0.0f);
}

// why: these builders preserve the Bambu firmware dialect byte-for-byte, including its trailing space.
TEST_CASE("unit: BBL AMS gcode builders preserve command bytes", "[unit][bbl]")
{
    CHECK(BBLPrinterAgent::ams_refresh_rfid_gcode("123") == "M620 R123 \n");
    CHECK(BBLPrinterAgent::ams_calibrate_gcode(123) == "M620 C123 \n");
    CHECK(BBLPrinterAgent::ams_select_tray_gcode("123") == "M620 P123 \n");
}

// why: an agent without a Bambu-dialect translation must refuse these commands before any network or wx path.
TEST_CASE("unit: default AMS commands report not supported", "[unit][moonraker]")
{
    MoonrakerPrinterAgent agent("");

    CHECK(agent.command_ams_refresh_rfid("dev", "123", 1, false) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.command_ams_calibrate("dev", 1, 2, false) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.command_ams_select_tray("dev", "123", 3, false) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
}

TEST_CASE("unit: Moonraker light name matching", "[unit][moonraker]")
{
    CHECK(moonraker_is_light_name("caselight"));
    CHECK(moonraker_is_light_name("LED_STRIP"));
    CHECK_FALSE(moonraker_is_light_name("beeper"));
    CHECK(moonraker_is_light_name("FLASHLIGHT_SWITCH"));
    CHECK(moonraker_is_light_name("MODLELIGHT_SWITCH"));
}

// ===========================================================================
// UNIT - handle_request's not-supported default.
// The agent is the only thing that knows what it can translate, so an untranslated
// command has to say so instead of returning success and letting the UI believe the
// control worked. Guards the inverse too: the pushing namespace is genuinely
// satisfied by the websocket status stream, and it re-fires from the keepalive timer
// roughly once a second, so it must stay a success or it would raise a dialog on a
// timer. Only branches that touch neither the network nor wx are exercised.
// ===========================================================================
TEST_CASE("unit: Moonraker reports untranslated commands as not supported", "[unit][moonraker]")
{
    MoonrakerPrinterAgent agent("");

    CHECK(agent.send_message("dev", R"({"print":{"command":"ams_change_filament"}})", 0, 0) ==
          ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.send_message("dev", R"({"system":{"command":"set_door_stat"}})", 0, 0) ==
          ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.send_message("dev", R"({"xcam":{"command":"xcam_control_set"}})", 0, 0) ==
          ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);

    CHECK(agent.send_message("dev", R"({"pushing":{"command":"pushall"}})", 0, 0) == BAMBU_NETWORK_SUCCESS);
    CHECK(agent.send_message("dev", R"({"pushing":{"command":"start"}})", 0, 0) == BAMBU_NETWORK_SUCCESS);

    // why: malformed input is a different failure than an untranslated command, and the
    // default must not swallow it into a misleading not-supported verdict.
    CHECK(agent.send_message("dev", "{not json", 0, 0) == BAMBU_NETWORK_ERR_INVALID_RESULT);
}

// why: IPrinterAgent::fetch_filament_info is the single virtual hook derived agents override
// (MoonrakerPrinterAgent's own override is synchronous, but QidiPrinterAgent's override is
// fire-and-forget: it spawns a detached thread and returns immediately). QidiPrinterAgent is
// `final`, so this probes the same contract with a controllable double instead.
TEST_CASE("unit: a fire-and-forget override of fetch_filament_info is not waited on by the caller",
          "[unit][moonraker]")
{
    class RecordingAgent : public Slic3r::MoonrakerPrinterAgent
    {
    public:
        explicit RecordingAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

        std::atomic<bool>  invoked{false};
        std::promise<void> release_gate;
        std::promise<void> done_promise;

        bool fetch_filament_info(std::string /*dev_id*/, FilamentSyncMode /*sync_mode*/ = FilamentSyncMode::pull) override
        {
            std::thread([this]() {
                invoked.store(true);
                // Block here until the test explicitly releases us, proving the caller
                // (fetch_filament_info) does not wait for this to run.
                release_gate.get_future().wait();
                done_promise.set_value();
            }).detach();
            return true;
        }
    };

    auto agent = std::make_shared<RecordingAgent>(std::string{});
    auto done_future = agent->done_promise.get_future();

    bool immediate_result = agent->fetch_filament_info("test-dev");

    // fetch_filament_info must return before its background work completes — prove
    // it by confirming the background call is still blocked on the gate right now.
    REQUIRE(immediate_result == true);
    REQUIRE(done_future.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);

    // Now let the background call finish and confirm it actually ran (polymorphic dispatch).
    agent->release_gate.set_value();
    REQUIRE(done_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    REQUIRE(agent->invoked.load() == true);
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
