#include "PluginOperations.hpp"

#include "PluginAuditManager.hpp"
#include "PluginJsonPy.hpp"
#include "PythonInterpreter.hpp"
#include "host/PluginHostTools.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/gil.h>
#include <pybind11/pytypes.h>

#include <algorithm>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

namespace py = pybind11;
using nlohmann::json;

PluginOperations& PluginOperations::instance()
{
    static PluginOperations operations;
    return operations;
}

void PluginOperations::stage(const std::string& plugin_key, std::vector<PluginOperation> operations)
{
    discard_staged(plugin_key);
    std::lock_guard<std::mutex> lock(m_mutex);
    for (PluginOperation& op : operations)
        m_staged.push_back(std::move(op));
}

void PluginOperations::discard_staged(const std::string& plugin_key)
{
    // Released after the lock: a handler's deleter takes the GIL.
    std::vector<PluginOperation> dropped;
    std::lock_guard<std::mutex>  lock(m_mutex);
    auto it = std::stable_partition(m_staged.begin(), m_staged.end(), [&](const PluginOperation& o) { return o.plugin_key != plugin_key; });
    dropped.assign(std::make_move_iterator(it), std::make_move_iterator(m_staged.end()));
    m_staged.erase(it, m_staged.end());
}

void PluginOperations::activate(const std::string& plugin_key)
{
    // One step under the lock remove_plugin() takes, so an unload either drops the staged operations first or
    // withdraws the offered ones after; none is offered after it.
    std::vector<PluginOperation> replaced;
    std::lock_guard<std::mutex>  lock(m_mutex);
    auto it = std::stable_partition(m_staged.begin(), m_staged.end(), [&](const PluginOperation& o) { return o.plugin_key != plugin_key; });
    for (auto op = it; op != m_staged.end(); ++op) {
        auto same = std::find_if(m_operations.begin(), m_operations.end(),
                                 [&](const PluginOperation& o) { return o.plugin_key == op->plugin_key && o.name == op->name; });
        if (same != m_operations.end()) {
            replaced.push_back(std::move(*same));
            *same = std::move(*op);
        } else
            m_operations.push_back(std::move(*op));
    }
    m_staged.erase(it, m_staged.end());
}

void PluginOperations::add(PluginOperation op)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::find_if(m_operations.begin(), m_operations.end(),
                           [&](const PluginOperation& o) { return o.plugin_key == op.plugin_key && o.name == op.name; });
    if (it != m_operations.end())
        *it = std::move(op);
    else
        m_operations.push_back(std::move(op));
}

void PluginOperations::remove_plugin(const std::string& plugin_key)
{
    std::vector<PluginOperation> removed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = std::stable_partition(m_operations.begin(), m_operations.end(),
                                        [&](const PluginOperation& o) { return o.plugin_key != plugin_key; });
        removed.assign(std::make_move_iterator(it), std::make_move_iterator(m_operations.end()));
        m_operations.erase(it, m_operations.end());
        auto staged = std::stable_partition(m_staged.begin(), m_staged.end(), [&](const PluginOperation& o) { return o.plugin_key != plugin_key; });
        removed.insert(removed.end(), std::make_move_iterator(staged), std::make_move_iterator(m_staged.end()));
        m_staged.erase(staged, m_staged.end());
    }
    // The handlers' last references go here or with a call still running; their deleter takes the GIL.
}

std::optional<PluginOperation> PluginOperations::find(const std::string& full_name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const PluginOperation& op : m_operations)
        if (op.full_name() == full_name)
            return op;
    return std::nullopt;
}

std::vector<PluginOperation> PluginOperations::all() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_operations;
}

json PluginOperations::invoke(const PluginOperation& op, const json& args) const
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const bool offered = std::any_of(m_operations.begin(), m_operations.end(), [&](const PluginOperation& o) {
            return o.plugin_key == op.plugin_key && o.name == op.name && o.handler == op.handler;
        });
        if (!offered)
            throw std::runtime_error("the plugin that offered it has unloaded");
    }
    if (!op.handler || !PythonInterpreter::instance().is_initialized())
        throw std::runtime_error("Python is not available");
    PythonRuntimeLease lease = PythonInterpreter::instance().acquire_runtime_lease();
    if (!lease)
        throw std::runtime_error("Python is shutting down");
    py::gil_scoped_acquire   gil;
    ScopedPluginAuditContext audit(op.plugin_key);
    // An operation runs on an MCP job's thread, which nothing on the UI thread waits for: its code may call tools.
    ScopedToolCallerThread   tool_caller;
    try {
        py::object result = (*op.handler)(json_to_py(args));
        return py_to_json(result);
    } catch (py::error_already_set& err) {
        log_python_exception_keep(err);
        throw std::runtime_error(err.what());
    }
}

} // namespace Slic3r
