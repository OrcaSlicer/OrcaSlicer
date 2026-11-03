#include "PythonRunScope.hpp"

#include "PluginAuditManager.hpp"
#include "host/PluginHostTools.hpp"

#include <boost/filesystem/path.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace Slic3r {

namespace {
thread_local PythonRunBinding tl_binding;
} // namespace

PythonRunScope::PythonRunScope(std::uint64_t id, std::string identity, boost::filesystem::path file, ToolBridge* bridge)
    : m_id(id), m_identity(std::move(identity)), m_file(std::move(file)), m_bridge(bridge)
{}

bool PythonRunScope::owns_ui() const { return is_script_identity(m_identity); }

std::string PythonRunScope::ui_owner() const { return owns_ui() ? "script/" + std::to_string(m_id) : m_identity; }

nlohmann::json PythonRunScope::call_tool(const std::string& tool, const nlohmann::json& args)
{
    ToolBridge* bridge = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_tools_closed)
            throw std::runtime_error("The script's run has ended; a thread it started calls no tools after that");
        if (m_bridge == nullptr)
            throw std::runtime_error("This run has no tools to call");
        bridge = m_bridge;
        ++m_calls_in_flight;
    }
    struct Leave
    {
        PythonRunScope& scope;
        ~Leave()
        {
            {
                std::lock_guard<std::mutex> lock(scope.m_mutex);
                --scope.m_calls_in_flight;
            }
            scope.m_calls_cv.notify_all();
        }
    } leave{*this};
    return bridge->call(tool, args);
}

bool PythonRunScope::has_tools() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bridge != nullptr && !m_tools_closed;
}

bool PythonRunScope::tools_closed() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tools_closed;
}

void PythonRunScope::close_tools()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_tools_closed = true;
    m_calls_cv.wait(lock, [this] { return m_calls_in_flight == 0; });
    m_bridge = nullptr;
}

bool PythonRunScope::thread_starting(bool waited)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_admission_closed)
        return false;
    if (waited)
        ++m_waited_pending;
    return true;
}

void PythonRunScope::thread_start_failed(bool waited)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (waited)
        --m_waited_pending;
}

void PythonRunScope::thread_started(unsigned long ident, bool waited)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (waited)
        --m_waited_pending;
    m_live_threads[ident] = waited;
}

void PythonRunScope::thread_ended(unsigned long ident)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_live_threads.erase(ident);
}

bool PythonRunScope::callback_entering()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_admission_closed)
        return false;
    ++m_callbacks_active;
    return true;
}

void PythonRunScope::callback_left()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    --m_callbacks_active;
}

bool PythonRunScope::task_submitted()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_admission_closed)
        return false;
    ++m_tasks_outstanding;
    return true;
}

void PythonRunScope::task_done()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    --m_tasks_outstanding;
}

void PythonRunScope::task_running(unsigned long ident)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_task_targets.insert(ident);
}

void PythonRunScope::task_finished(unsigned long ident)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_task_targets.find(ident); it != m_task_targets.end())
        m_task_targets.erase(it);
}

bool PythonRunScope::close_admission_if_quiet()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_admission_closed)
        return true;
    if (m_waited_pending > 0 || m_callbacks_active > 0 || m_tasks_outstanding > 0)
        return false;
    for (const auto& [ident, waited] : m_live_threads)
        if (waited)
            return false;
    m_admission_closed = true;
    return true;
}

void PythonRunScope::close_admission()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_admission_closed = true;
}

bool PythonRunScope::admission_closed() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_admission_closed;
}

void PythonRunScope::for_each_interrupt_target(const std::function<void(unsigned long)>& fn) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [ident, waited] : m_live_threads)
        fn(ident);
    for (const unsigned long ident : m_task_targets)
        fn(ident);
}

const PythonRunBinding& current_python_run() { return tl_binding; }

ScopedPythonRun::ScopedPythonRun(std::shared_ptr<PythonRunScope> run, PythonRunThread kind) : m_previous(std::move(tl_binding))
{
    tl_binding = PythonRunBinding{std::move(run), kind};
}

ScopedPythonRun::~ScopedPythonRun() { tl_binding = std::move(m_previous); }

} // namespace Slic3r
