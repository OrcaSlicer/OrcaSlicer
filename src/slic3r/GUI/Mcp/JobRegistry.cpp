#include "JobRegistry.hpp"

#include <algorithm>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

std::string JobRegistry::create(const std::string& tool, const std::string& parent)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t running = std::count_if(m_jobs.begin(), m_jobs.end(), [](const auto& kv) { return !kv.second.finished; });
    if (running >= kMaxRunning)
        throw ToolFailure("Too many running jobs; wait for some to finish (see get_state.jobs)");
    std::string id = "j" + std::to_string(m_next++);
    Job job;
    job.tool    = tool;
    job.parent  = parent;
    job.created = std::chrono::steady_clock::now();
    m_jobs.emplace(id, std::move(job));
    return id;
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
        while (m_finished_order.size() > kKeepFinished) {
            m_jobs.erase(m_finished_order.front());
            m_finished_order.pop_front();
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

std::set<std::string> JobRegistry::lineage(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::set<std::string> out;
    std::string           cur = id;
    while (!cur.empty() && out.insert(cur).second) {
        auto it = m_jobs.find(cur);
        cur     = it == m_jobs.end() ? std::string() : it->second.parent;
    }
    return out;
}

bool JobRegistry::any_running_except(const std::set<std::string>& excluded) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::any_of(m_jobs.begin(), m_jobs.end(),
                       [&](const auto& kv) { return !kv.second.finished && excluded.count(kv.first) == 0; });
}

void JobRegistry::set_operation(const std::string& id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_jobs.find(id); it != m_jobs.end())
        it->second.operation = true;
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

nlohmann::json JobRegistry::list() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto     now = std::chrono::steady_clock::now();
    nlohmann::json out = nlohmann::json::array();
    for (const auto& [id, job] : m_jobs) {
        nlohmann::json j{{"id", id},
                         {"tool", job.tool},
                         {"state", job.finished ? (job.result.is_error ? "failed" : "done") : "running"},
                         {"age_ms", std::chrono::duration_cast<std::chrono::milliseconds>(now - job.created).count()}};
        if (!job.parent.empty())
            j["parent"] = job.parent;
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
