#include "JobRegistry.hpp"
#include "McpTypes.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

bool JobRegistry::unloaded(const std::string& parent, const std::string& owner) const
{
    std::string plugin = owner;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (plugin.empty() && !parent.empty())
            if (const auto it = m_jobs.find(parent); it != m_jobs.end())
                plugin = it->second.owner;
        if (plugin.empty())
            return false;
        if (m_blocked_owners.count(plugin) != 0)
            return true;
    }
    // The hook takes the plugin manager's lock, so it is asked without this registry's.
    return m_owner_loaded && !m_owner_loaded(plugin);
}

std::string JobRegistry::create(const std::string& tool, const std::string& parent, const std::string& owner)
{
    const std::string id = register_job(tool, parent, owner);
    if (unloaded(id, {})) {
        // Never started: nothing waits for it, and a stop the unload asked for it meanwhile goes with it.
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.erase(id);
        m_stop_requests.erase(id);
        throw ToolFailure(kPluginUnloaded);
    }
    return id;
}

std::string JobRegistry::register_job(const std::string& tool, const std::string& parent, const std::string& owner)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t running = std::count_if(m_jobs.begin(), m_jobs.end(), [](const auto& kv) { return !kv.second.finished; });
    if (running >= kMaxRunning)
        throw ToolFailure("Too many running jobs; wait for some to finish (see get_app_state.jobs)");
    std::string id = "j" + std::to_string(m_next++);
    Job job;
    job.tool    = tool;
    job.parent  = parent;
    job.owner   = owner;
    job.created = std::chrono::steady_clock::now();
    if (!parent.empty()) {
        job.ancestors.push_back(parent);
        if (auto it = m_jobs.find(parent); it != m_jobs.end()) {
            job.ancestors.insert(job.ancestors.end(), it->second.ancestors.begin(), it->second.ancestors.end());
            if (job.owner.empty())
                job.owner = it->second.owner;
        }
    }
    if (!job.owner.empty() && m_blocked_owners.count(job.owner) != 0)
        throw ToolFailure(kPluginUnloaded);
    m_jobs.emplace(id, std::move(job));
    return id;
}

void JobRegistry::block_owner(const std::string& owner)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_blocked_owners.insert(owner);
}

void JobRegistry::unblock_owner(const std::string& owner)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_blocked_owners.erase(owner);
}

std::vector<std::string> JobRegistry::running_owned_by(const std::string& owner) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string>    out;
    for (const auto& [id, job] : m_jobs)
        if (!job.finished && !owner.empty() && job.owner == owner)
            out.push_back(id);
    return out;
}

LineageNode::LineageNode(JobRegistry& jobs, std::string tool, std::string parent)
    : m_jobs(jobs), m_tool(std::move(tool)), m_parent(std::move(parent)), m_id(make())
{}

LineageNode::~LineageNode()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_jobs.finish(m_id, ToolResult::ok());
}

std::string LineageNode::make() const
{
    const std::string id = m_jobs.create(m_tool, m_parent);
    m_jobs.set_idle(id);
    return id;
}

std::string LineageNode::id() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_id;
}

void LineageNode::renew()
{
    std::string fresh = make();
    std::string old;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        old = std::exchange(m_id, std::move(fresh));
    }
    m_jobs.finish(old, ToolResult::ok());
}

void require_own_job(const ToolContext& ctx, const JobRegistry& jobs, const std::string& job, const char* what)
{
    if (!ctx.caller_plugin().empty() && jobs.owner(job) != ctx.caller_plugin())
        throw ToolFailure(std::string("A plugin ") + what + " only the jobs of its own calls");
}

std::string JobRegistry::owner(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto                  it = m_jobs.find(id);
    return it == m_jobs.end() ? std::string() : it->second.owner;
}

void JobRegistry::finish(const std::string& id, ToolResult result)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_jobs.find(id);
        if (it == m_jobs.end() || it->second.finished)
            return;
        it->second.finished = true;
        it->second.result   = std::move(result);
        m_finished_order.push_back(id);
        // Forget the oldest finished jobs beyond the limit, except those a running parent may
        // still wait for (a replay's await:false steps) while fewer than kKeepFinishedMax are kept.
        auto old = m_finished_order.begin();
        while (m_finished_order.size() > kKeepFinished && old != m_finished_order.end()) {
            const auto job    = m_jobs.find(*old);
            const auto parent = job == m_jobs.end() ? m_jobs.end() : m_jobs.find(job->second.parent);
            if (parent != m_jobs.end() && !parent->second.finished && m_finished_order.size() <= kKeepFinishedMax) {
                ++old;
                continue;
            }
            if (job != m_jobs.end())
                m_jobs.erase(job);
            old = m_finished_order.erase(old);
        }
    }
    m_cv.notify_all();
}

std::optional<ToolResult> JobRegistry::wait(const std::string& id, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_jobs.find(id) == m_jobs.end())
        throw ToolFailure("Unknown job " + id + " (finished jobs are kept only for the last " + std::to_string(kKeepFinished) + ")");
    m_cv.wait_for(lock, timeout, [&] {
        auto it = m_jobs.find(id);
        return m_woken || it == m_jobs.end() || it->second.finished;
    });
    auto it = m_jobs.find(id);
    if (it == m_jobs.end() || !it->second.finished)
        return std::nullopt;
    return it->second.result;
}

std::optional<ToolResult> JobRegistry::wait(const std::string& id, std::chrono::milliseconds timeout, const std::function<bool()>& stop)
{
    constexpr std::chrono::milliseconds slice{500};
    const auto                          deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (auto done = wait(id, std::clamp(left, std::chrono::milliseconds(0), slice)))
            return done;
        if (left <= slice || stop())
            return std::nullopt;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_woken)
            return std::nullopt;
    }
}

void JobRegistry::request_stop(const std::string& id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stop_requests.insert(id);
}

bool JobRegistry::stop_requested(const std::string& id) const
{
    const std::vector<std::string> chain = ancestry(id);
    std::lock_guard<std::mutex>    lock(m_mutex);
    return std::any_of(chain.begin(), chain.end(), [this](const std::string& job) { return m_stop_requests.count(job) != 0; });
}

std::set<std::string> JobRegistry::lineage(const std::string& id) const
{
    const std::vector<std::string> chain = ancestry(id);
    return {chain.begin(), chain.end()};
}

std::vector<std::string> JobRegistry::ancestry(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> out;
    if (id.empty())
        return out;
    out.push_back(id);
    if (auto it = m_jobs.find(id); it != m_jobs.end())
        out.insert(out.end(), it->second.ancestors.begin(), it->second.ancestors.end());
    return out;
}

bool JobRegistry::any_running_except(const std::set<std::string>& excluded) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::any_of(m_jobs.begin(), m_jobs.end(),
                       [&](const auto& kv) { return !kv.second.finished && !kv.second.idle && excluded.count(kv.first) == 0; });
}

void JobRegistry::set_idle(const std::string& id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_jobs.find(id); it != m_jobs.end())
        it->second.idle = true;
}

std::string JobRegistry::begin_operation(const std::string& id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [other, job] : m_jobs)
        if (other != id && !job.finished && job.operation)
            return other;
    if (auto it = m_jobs.find(id); it != m_jobs.end())
        it->second.operation = true;
    return {};
}

std::vector<std::string> JobRegistry::running_operations() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> out;
    for (const auto& [id, job] : m_jobs)
        if (!job.finished && job.operation)
            out.push_back(id);
    return out;
}

std::string JobRegistry::running_tool(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto                  it = m_jobs.find(id);
    return it == m_jobs.end() || it->second.finished ? std::string() : it->second.tool;
}

nlohmann::json JobRegistry::list() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto     now = std::chrono::steady_clock::now();
    // Newest first.
    std::vector<std::pair<std::string, const Job*>> jobs;
    for (const auto& [id, job] : m_jobs)
        jobs.emplace_back(id, &job);
    std::sort(jobs.begin(), jobs.end(), [](const auto& a, const auto& b) { return a.second->created > b.second->created; });
    nlohmann::json out = nlohmann::json::array();
    for (const auto& [id, job_ptr] : jobs) {
        const Job&     job = *job_ptr;
        nlohmann::json j{{"id", id},
                         {"tool", job.tool},
                         {"state", job.finished ? (job.result.is_error ? "failed" : "done") : "running"},
                         {"age_ms", std::chrono::duration_cast<std::chrono::milliseconds>(now - job.created).count()}};
        if (!job.parent.empty())
            j["parent"] = job.parent;
        if (job.idle && !job.finished)
            j["idle"] = true;
        out.push_back(std::move(j));
    }
    return out;
}

void JobRegistry::wake_all()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_woken = true;
    }
    m_cv.notify_all();
}

}}} // namespace Slic3r::GUI::Mcp
