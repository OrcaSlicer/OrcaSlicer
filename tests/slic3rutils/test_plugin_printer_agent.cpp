#include <catch2/catch_all.hpp>

#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonPluginBridge.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/Utils/IPrinterAgent.hpp>

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <memory>
#include <string>

namespace py = pybind11;
using namespace Slic3r;

namespace {

// Same idiom as ScopedPluginManager in test_plugin_lifecycle.cpp: the trampolines refuse to call
// into Python unless PythonInterpreter::instance() reports initialized.
struct ScopedPluginManager
{
    bool initialized = PluginManager::instance().initialize();

    ~ScopedPluginManager()
    {
        PluginManager::instance().shutdown();
        PythonInterpreter::instance().shutdown();
    }
};

// The host reaches a printer agent plugin through IPrinterAgent, so the tests do too. The Python
// instance carries the overrides, so it has to outlive every call, as PluginInstanceHandle ensures
// in production.
struct Agent
{
    py::object                     instance;
    std::shared_ptr<IPrinterAgent> agent;

    IPrinterAgent* operator->() const { return agent.get(); }
    IPrinterAgent& operator*() const { return *agent; }
};

Agent make_agent(const std::string& body)
{
    (void) PythonPluginBridge::instance(); // force the embedded module registration into the binary
    py::dict globals;
    globals["orca"] = py::module_::import("orca");

    py::exec("class Agent(orca.printer_agent.PrinterAgentBase):\n"
             "    def get_name(self): return 'agent'\n" + body, globals);
    py::object instance   = globals["Agent"]();
    auto       capability = instance.cast<std::shared_ptr<PluginCapabilityInterface>>();
    capability->set_audit_plugin_key("agent_plugin");
    return {instance, std::dynamic_pointer_cast<IPrinterAgent>(capability)};
}

// One operation per return type and dispatch shape; the rest share their macro.
const std::string OPERATIONS[] = {"get_agent_info", "disconnect_printer", "start_discovery", "get_user_selected_machine",
                                  "get_filament_sync_mode", "install_device_cert", "start_local_print", "request_bind_ticket"};

// What NetworkAgent answers when no printer agent is set.
void check_answers_like_no_agent(IPrinterAgent& agent)
{
    std::string ticket = "untouched";

    CHECK(agent.get_agent_info().id.empty());
    CHECK(agent.disconnect_printer() == -1);
    CHECK_FALSE(agent.start_discovery(true, false));
    CHECK(agent.get_user_selected_machine().empty());
    CHECK(agent.get_filament_sync_mode() == FilamentSyncMode::none);
    CHECK_NOTHROW(agent.install_device_cert("dev", true));
    CHECK(agent.start_local_print(PrintParams{}, nullptr, nullptr) == -1);
    CHECK(agent.request_bind_ticket(&ticket) == -1);
    CHECK(ticket == "untouched");
}

std::string define_all(const std::string& signature_tail, const std::string& statement)
{
    std::string body;
    for (const std::string& operation : OPERATIONS)
        body += "    def " + operation + "(self" + signature_tail + "): " + statement + "\n";
    return body;
}

} // namespace

TEST_CASE("A printer agent operation that raises answers like a missing agent", "[PluginPrinterAgent][Python]")
{
    ScopedPluginManager plugin_system; // declared first: destroyed last
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil; // released before plugin_system's destructor shuts Python down

    auto agent = make_agent(define_all(", *args", "raise RuntimeError('boom')"));
    REQUIRE(agent.agent);

    check_answers_like_no_agent(*agent);

    // The interpreter stays usable.
    CHECK(py::eval("1 + 1").cast<int>() == 2);
}

TEST_CASE("A printer agent that omits its operations answers like a missing agent", "[PluginPrinterAgent][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    auto agent = make_agent("");
    REQUIRE(agent.agent);

    check_answers_like_no_agent(*agent);
}

TEST_CASE("A printer agent operation returning the wrong type answers like a missing agent", "[PluginPrinterAgent][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    auto agent = make_agent(define_all(", *args", "return object()"));
    REQUIRE(agent.agent);

    check_answers_like_no_agent(*agent);
}

TEST_CASE("A working printer agent's answers reach the host unchanged", "[PluginPrinterAgent][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    auto agent = make_agent("    def get_agent_info(self): return orca.printer_agent.AgentInfo('id', 'name', '1', 'description')\n"
                            "    def disconnect_printer(self): return 7\n"
                            "    def start_discovery(self, start, sending): return start and not sending\n"
                            "    def get_user_selected_machine(self): return 'machine'\n"
                            "    def get_filament_sync_mode(self): return orca.printer_agent.FilamentSyncMode.Pull\n"
                            "    def request_bind_ticket(self): return (3, 'ticket')\n"
                            "    def bind_detect(self, dev_ip, sec_link, detect):\n"
                            "        detect.dev_id = dev_ip\n"
                            "        return 0\n");
    REQUIRE(agent.agent);

    std::string  ticket;
    detectResult detect;

    CHECK(agent->get_agent_info().id == "id");
    CHECK(agent->disconnect_printer() == 7);
    CHECK(agent->start_discovery(true, false));
    CHECK(agent->get_user_selected_machine() == "machine");
    CHECK(agent->get_filament_sync_mode() == FilamentSyncMode::pull);
    CHECK(agent->request_bind_ticket(&ticket) == 3);
    CHECK(ticket == "ticket");
    CHECK(agent->bind_detect("192.168.0.2", "secure", detect) == 0);
    CHECK(detect.dev_id == "192.168.0.2");
}
