#pragma once

#include <pybind11/pybind11.h>

namespace Slic3r {

// Binds the `orca.host.printer_agent` submodule: completion callbacks for a plugin-owned
// pre-operation UI. A printer-agent plugin's configure_operation() returns UiMode.Plugin and
// keeps request.token; its HTML UI later calls submit_operation_config(token, options) or
// cancel_operation_config(token) from a plugin callback (the on_submit/on_message handler of
// orca.host.ui.create_window()). The host then starts the operation through the normal
// PrintJob/SendJob path, so the plugin never starts a print or send directly.
void register_printer_agent_host_api(pybind11::module_& host);

} // namespace Slic3r
