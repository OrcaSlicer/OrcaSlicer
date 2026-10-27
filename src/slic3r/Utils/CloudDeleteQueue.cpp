#include "CloudDeleteQueue.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/system/error_code.hpp>

#include "libslic3r/InstanceLock.hpp"
#include "libslic3r/Preset.hpp"
#include "OrcaCloudServiceAgent.hpp"
#include "bambu_networking.hpp"

namespace Slic3r {

namespace {

CloudDeleteQueue::Clock::duration retry_wait(int failures)
{
    using namespace std::chrono_literals;
    constexpr auto first   = 1min;
    constexpr auto longest = 60min;
    // Capped before shifting, so a long run of failures can't overflow the shift.
    const int doublings = std::min(failures - 1, 6);
    return std::min<CloudDeleteQueue::Clock::duration>(first * (1 << doublings), longest);
}

} // namespace

void CloudDeleteQueue::start_session(const std::string& user_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (user_id == m_user_id)
        return;
    m_user_id = user_id;
    ++m_generation;
    m_pending.clear();
}

uint64_t CloudDeleteQueue::generation() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_generation;
}

bool CloudDeleteQueue::is_current(uint64_t generation) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return generation == m_generation;
}

bool CloudDeleteQueue::add(uint64_t generation, const std::string& setting_id, const std::string& info_path, const std::string& owner)
{
    if (setting_id.empty())
        return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (generation != m_generation)
        return false;
    m_pending.try_emplace(setting_id, Pending{Entry{setting_id, info_path, owner.empty() ? m_user_id : owner}});
    return true;
}

CloudDeleteQueue::Batch CloudDeleteQueue::due(Clock::time_point now) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Batch batch{m_generation, {}};
    if (m_user_id.empty())
        return batch;
    for (const auto& [setting_id, pending] : m_pending)
        if (pending.entry.owner == m_user_id && pending.not_before <= now)
            batch.entries.push_back(pending.entry);
    return batch;
}

std::optional<std::string> CloudDeleteQueue::finish(uint64_t generation, const std::string& setting_id, bool gone, Clock::time_point now)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (generation != m_generation)
        return std::nullopt;
    auto it = m_pending.find(setting_id);
    if (it == m_pending.end())
        return std::nullopt;
    if (gone) {
        std::string info_path = std::move(it->second.entry.info_path);
        m_pending.erase(it);
        return info_path;
    }
    Pending& pending  = it->second;
    pending.not_before = now + retry_wait(++pending.failures);
    return std::nullopt;
}

void CloudDeleteQueue::send_due(const std::function<Clock::time_point()>& now,
                                const std::function<std::string()>&       current_user,
                                const std::function<bool(const Entry&)>&  send,
                                const std::function<void(const Entry&)>&  on_gone)
{
    const Batch batch = due(now());
    for (const Entry& entry : batch.entries) {
        if (!is_current(batch.generation) || current_user() != entry.owner)
            return;
        const bool gone = send(entry);
        if (current_user() != entry.owner)
            return;
        if (finish(batch.generation, entry.setting_id, gone, now()))
            on_gone(entry);
    }
}

size_t CloudDeleteQueue::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending.size();
}

bool queue_orphaned_info_files(CloudDeleteQueue& queue, uint64_t generation, const std::string& user_dir)
{
    namespace fs = boost::filesystem;
    std::vector<fs::path> preset_dirs;
    for (const char* type : {PRESET_PRINT_NAME, PRESET_FILAMENT_NAME, PRESET_PRINTER_NAME}) {
        preset_dirs.push_back(fs::path(user_dir) / type);
        // Presets with no parent, custom printers and the presets made for them among them, are kept in base/.
        preset_dirs.push_back(fs::path(user_dir) / type / "base");
    }
    for (const fs::path& type_dir : preset_dirs) {
        if (!fs::exists(type_dir))
            continue;

        // The error_code iterator logs and skips a transient directory-read failure (e.g. macOS readdir
        // returning ENOTSUP) instead of throwing on the background sync thread this runs on.
        boost::system::error_code ec;
        for (fs::directory_iterator it(type_dir, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path info_file = it->path();
            if (info_file.extension() != ".info")
                continue;
            fs::path preset_file = info_file;
            preset_file.replace_extension(".json");
            // Most .info files sit beside their preset; only the rest are read, and only those need the lock.
            if (fs::exists(preset_file))
                continue;

            // Read under the lock, so a remove_files() in another instance is seen whole or not at all;
            // the queue's own mutex is taken after the lock is released.
            Preset info(Preset::TYPE_INVALID, "");
            {
                InstanceLock instance_lock(user_presets_lock_path());
                if (!fs::exists(preset_file))
                    info.load_info(info_file.string());
            }
            if (info.setting_id.empty())
                continue;
            if (!queue.add(generation, info.setting_id, info_file.string(), info.user_id)) {
                BOOST_LOG_TRIVIAL(info) << "queue_orphaned_info_files: account changed, scan of " << user_dir << " abandoned";
                return false;
            }
            BOOST_LOG_TRIVIAL(info) << "Found orphaned .info file on startup: " << info_file.string();
        }
        if (ec)
            BOOST_LOG_TRIVIAL(warning) << "queue_orphaned_info_files: failed to scan " << type_dir.string() << ": " << ec.message();
    }
    return true;
}

void remove_info_file(const std::string& info_path)
{
    if (info_path.empty() || !boost::filesystem::exists(boost::filesystem::path(info_path)))
        return;
    InstanceLock instance_lock(user_presets_lock_path());
    boost::nowide::remove(info_path.c_str());
    BOOST_LOG_TRIVIAL(info) << "Deleted .info file after cloud confirmation: " << info_path;
}

void send_cloud_deletes(CloudDeleteQueue&                                          queue,
                        OrcaCloudServiceAgent&                                     agent,
                        const std::function<void(const CloudDeleteQueue::Entry&)>& on_gone)
{
    queue.send_due(
        [] { return CloudDeleteQueue::Clock::now(); }, [&agent] { return agent.get_user_id(); },
        [&agent](const CloudDeleteQueue::Entry& entry) {
            const bool gone = agent.delete_setting_as(entry.owner, entry.setting_id) == BAMBU_NETWORK_SUCCESS;
            if (!gone)
                BOOST_LOG_TRIVIAL(info) << "delete setting = " << entry.setting_id << " failed, will retry";
            return gone;
        },
        [&on_gone](const CloudDeleteQueue::Entry& entry) {
            // The cloud has no such profile any more, so the .info that kept the delete pending can go.
            remove_info_file(entry.info_path);
            if (on_gone)
                on_gone(entry);
        });
}

} // namespace Slic3r
