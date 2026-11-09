#include "LanMonitorManager.hpp"
#include "format.hpp"
#include "ProcessRunner.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/BBLNetworkPlugin.hpp"
#include "slic3r/Utils/LanMonitorSession.hpp"
#include "slic3r/Utils/LanMonitorSnapshot.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/timer.h>

namespace Slic3r::GUI {
using json = nlohmann::json;
using Clock = LanMonitorStatus::Clock;

struct LanMonitorManager::Session : LanMonitorSession
{
    std::string id;
    std::string host;
    std::string code;
    std::string module;
    std::string configuration;
    std::unique_ptr<ProcessRunner> runner;
    Clock::time_point launched_at{};
    bool configured = false;
};

LanMonitorManager::LanMonitorManager(GUI_App& app) : m_app(app), m_timer(this)
{
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { sync(); });
    start();
}

LanMonitorManager::~LanMonitorManager() { stop(); }

void LanMonitorManager::start() { m_timer.Start(1000); }

void LanMonitorManager::stop()
{
    m_timer.Stop();
    for (auto& [id, session] : m_sessions) {
        session->active = false;
        ++session->generation;
        if (session->runner) session->runner->terminate();
    }
    m_sessions.clear();
}

const LanMonitorStatus* LanMonitorManager::status(const std::string& id) const
{
    auto it = m_sessions.find(id);
    return it == m_sessions.end() ? nullptr : &it->second->status;
}

void LanMonitorManager::retry(const std::string& id)
{
    auto it = m_sessions.find(id);
    if (it == m_sessions.end()) return;
    auto& session = it->second;
    ++session->generation;
    if (session->runner) session->runner->terminate();
    session->status = {};
    session->retry_at = {};
    session->backoff = 1;
}

void LanMonitorManager::sync()
{
    if (m_app.is_closing() || !m_app.app_config || !m_app.is_enable_multi_machine()) {
        // Keep the timer so enabling the existing preference takes effect without creating a page.
        for (auto& [id, session] : m_sessions) {
            session->active = false;
            if (session->runner) session->runner->terminate();
        }
        m_sessions.clear();
        return;
    }
    auto* devices = m_app.getDeviceManager();
    if (!devices) return;
    auto machines = devices->get_multi_machine_list();
    std::set<std::string> wanted;
    const auto now = Clock::now();
    for (unsigned i = 0; i < 6; ++i) {
        const auto id = m_app.app_config->get("multi_devices", std::to_string(i));
        auto machine = machines.find(id);
        if (machine == machines.end() || !machine->second->supports_lan_monitoring()) continue;
        wanted.insert(id);
        const auto host = machine->second->get_dev_ip();
        const auto code = machine->second->get_access_code();
        const auto module = BBLNetworkPlugin::get_versioned_library_path(m_app.app_config->get_network_plugin_version());
        auto& entry = m_sessions[id];
        if (entry && (entry->host != host || entry->code != code || entry->module != module)) {
            entry->active = false;
            if (entry->runner) entry->runner->terminate();
            entry.reset();
        }
        if (!entry) {
            entry = std::make_shared<Session>();
            entry->id = id;
            entry->host = host;
            entry->code = code;
            entry->module = module;
        }
        if (entry->runner && entry->runner->is_running()) {
            if (!entry->configured && entry->runner->can_write_stdin()) {
                entry->runner->write_stdin(entry->configuration + "\n");
                entry->configuration.clear();
                entry->configured = true;
            }
            // A stuck worker must not stay "Connecting" or show old telemetry indefinitely.
            if (entry->status.state == LanMonitorStatus::State::AuthenticationError ||
                entry->status.state == LanMonitorStatus::State::Unavailable ||
                entry->status.state == LanMonitorStatus::State::Offline ||
                (entry->status.received && now - entry->status.last_report > std::chrono::seconds(30)) ||
                (!entry->status.received && now - entry->launched_at > std::chrono::seconds(40)))
                entry->runner->terminate();
        } else if (entry->status.state != LanMonitorStatus::State::AuthenticationError &&
                   entry->status.state != LanMonitorStatus::State::Unavailable && now >= entry->retry_at) {
            launch(entry);
        }
    }
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (wanted.count(it->first)) { ++it; continue; }
        it->second->active = false;
        if (it->second->runner) it->second->runner->terminate();
        it = m_sessions.erase(it);
    }
}

void LanMonitorManager::launch(const std::shared_ptr<Session>& session)
{
    namespace fs = boost::filesystem;
    wxFileName executable(wxStandardPaths::Get().GetExecutablePath());
#ifdef _WIN32
    executable.SetFullName("orca-lan-monitor-worker.exe");
#else
    executable.SetFullName("orca-lan-monitor-worker");
#endif
    if (!executable.FileExists() || !fs::exists(session->module)) {
        session->status.state = LanMonitorStatus::State::Unavailable;
        return;
    }
    try {
        const auto directory = fs::path(data_dir()) / "cache" / "lan-monitor" /
                               std::to_string(std::hash<std::string>{}(session->id));
        fs::create_directories(directory);
        // The public device certificate cache is the only SDK state reused by a monitor.
        const auto certificates = fs::path(data_dir()) / "printers" / session->id;
        if (fs::is_directory(certificates)) {
            for (fs::recursive_directory_iterator it(certificates), end; it != end; ++it) {
                if (!fs::is_regular_file(it->path()) || it->path().extension() != ".cer") continue;
                const auto target = directory / "printers" / session->id / fs::relative(it->path(), certificates);
                fs::create_directories(target.parent_path());
                fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing);
            }
        }
        session->configuration = json({{"module", session->module}, {"directory", directory.string()},
            {"certificate", resources_dir() + "/cert"}, {"country", m_app.app_config->get_country_code()},
            {"seconds", 0}, {"printers", json::array({{{"id", session->id}, {"host", session->host}, {"code", session->code}}})}}).dump();
        session->status = {};
        session->configured = false;
        session->launched_at = Clock::now();
        const auto generation = ++session->generation;
        std::weak_ptr<Session> weak = session;
        session->runner = std::make_unique<ProcessRunner>();
        session->runner->run_command_line_async("\"" + into_u8(executable.GetFullPath()) + "\"",
            [weak, generation](const std::vector<ProcessRunner::OutputLine>& lines) {
                auto entry = weak.lock();
                if (!entry || !entry->active || entry->generation != generation) return;
                for (const auto& line : lines) {
                    const std::string prefix = "ORCA_LAN_MONITOR ";
                    if (line.is_stderr || line.text.compare(0, prefix.size(), prefix) != 0) continue;
                    const auto message = json::parse(line.text.substr(prefix.size()), nullptr, false);
                    entry->consume(message, generation);
                }
            }, [weak, generation](int) {
                auto entry = weak.lock();
                if (!entry || !entry->active || entry->generation != generation) return;
                entry->finished(generation);
            });
    } catch (const std::exception&) {
        session->configuration.clear();
        session->status.state = LanMonitorStatus::State::Unavailable;
    }
}

wxString lan_monitor_status_text(const LanMonitorStatus* status)
{
    if (!status) return _L("Connecting");
    if (status->state == LanMonitorStatus::State::AuthenticationError) return _L("Incorrect access code");
    if (status->state == LanMonitorStatus::State::Unavailable) return _L("LAN monitoring unavailable");
    if (status->state == LanMonitorStatus::State::Connecting ||
        (status->state == LanMonitorStatus::State::Online && !status->received)) return _L("Connecting");
    if (!status->live()) return _L("Offline");
    const auto& state = status->job.print_state;
    if (state == "RUNNING") return _L("Printing");
    if (state == "PAUSE") return _L("Paused");
    if (state == "FINISH") return _L("Finished");
    if (state == "FAILED") return _L("Failed");
    if (state == "PREPARE" || state == "SLICING") return _L("Preparing");
    if (state == "IDLE") return _L("Idle");
    return _L("Unknown");
}

wxString lan_monitor_progress_text(const LanMonitorStatus* status)
{
    if (!status || !status->live()) return _L("N/A");
    const auto progress = status->job.progress ? wxString::Format("%d%%", *status->job.progress) : _L("N/A");
    const auto remaining = status->job.remaining_seconds ? from_u8(get_bbl_monitor_time_dhm(*status->job.remaining_seconds)) : _L("N/A");
    // TRN %1% is the progress percentage and %2% is the remaining duration.
    return format_wxstr(_L("%1% | %2% remaining"), progress, remaining);
}

} // namespace Slic3r::GUI
