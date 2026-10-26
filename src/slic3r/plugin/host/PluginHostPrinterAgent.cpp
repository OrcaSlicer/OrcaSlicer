#include "PluginHostPrinterAgent.hpp"

#include "slic3r/Utils/PrinterAgentOperationConfig.hpp"
#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PluginJsonPy.hpp"

#include <nlohmann/json.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <stdexcept>
#include <string>

namespace Slic3r {
namespace {

std::string current_plugin_or_raise()
{
    const std::string plugin_key = PluginAuditManager::instance().current_plugin();
    if (plugin_key.empty())
        throw std::runtime_error("orca.host.printer_agent must be called from a plugin callback");
    return plugin_key;
}

nlohmann::json options_or_raise(pybind11::handle options)
{
    if (options.is_none())
        return nlohmann::json::object();
    nlohmann::json parsed = py_to_json(options);
    if (!parsed.is_object())
        throw pybind11::value_error("operation config options must be a dict");
    return parsed;
}

} // namespace

void register_printer_agent_host_api(pybind11::module_& host)
{
    auto printer_agent = host.def_submodule(
        "printer_agent",
        "Completion API for a plugin-owned pre-operation UI. A printer-agent plugin returns "
        "UiMode.Plugin from configure_operation(), keeps request.token, and completes it here from "
        "its HTML UI. The host runs the accepted operation through PrintJob/SendJob.");

    printer_agent.def(
        "submit_operation_config",
        [](const std::string& token, pybind11::object options) {
            const std::string plugin_key = current_plugin_or_raise();
            std::string       error;
            if (!PrinterAgentOperationConfigStore::instance().submit(token, plugin_key,
                                                                     options_or_raise(options), &error))
                throw std::runtime_error(error);
        },
        pybind11::arg("token"), pybind11::arg("options") = pybind11::none(),
        "Accept the pending operation and supply PrintParams overrides. options is a dict; recognized "
        "keys include target_id (or dev_id), project_name, preset_name, ams_mapping, nozzle_mapping, "
        "task_* flags, connection fields and extra_options. Every other key is forwarded to the printer "
        "agent in PrintParams.extra_options. A plugin-owned UI that did not fix a target in "
        "configure_operation() must supply target_id here. Raises if the token is unknown, already "
        "completed, belongs to another plugin, has no target, or would change a fixed target.");

    printer_agent.def(
        "cancel_operation_config",
        [](const std::string& token) {
            const std::string plugin_key = current_plugin_or_raise();
            std::string       error;
            if (!PrinterAgentOperationConfigStore::instance().cancel(token, plugin_key, &error))
                throw std::runtime_error(error);
        },
        pybind11::arg("token"),
        "Cancel the pending operation; no PrintJob/SendJob is started. Raises if the token is unknown, "
        "already completed, or belongs to another plugin.");

    printer_agent.def(
        "is_operation_config_pending", [](const std::string& token) {
            const std::string plugin_key = current_plugin_or_raise();
            return PrinterAgentOperationConfigStore::instance().is_pending_for(token, plugin_key);
        },
        pybind11::arg("token"),
        "Return True while the token still belongs to this plugin and has not been completed.");
}

} // namespace Slic3r
