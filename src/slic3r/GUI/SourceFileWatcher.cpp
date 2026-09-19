#include "SourceFileWatcher.hpp"

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

namespace {
    // Sentinel for "file does not currently exist" so a create is detected as a change too.
    constexpr std::time_t source_file_missing_mtime = 0;

    SourceStamp get_source_stamp(const std::string& path)
    {
        boost::system::error_code ec;
        std::time_t mtime = fs::last_write_time(path, ec);
        if (ec)
            return SourceStamp{source_file_missing_mtime, 0};
        std::uintmax_t size = fs::file_size(path, ec);
        return ec ? SourceStamp{source_file_missing_mtime, 0} : SourceStamp{mtime, size};
    }

    // fs::exists()/fs::is_directory() throw on an I/O error (e.g. an unreachable network share);
    // these treat that the same as "not found" instead, matching get_source_stamp() above.
    bool path_exists(const fs::path& path)
    {
        boost::system::error_code ec;
        bool result = fs::exists(path, ec);
        return !ec && result;
    }

    bool is_directory(const fs::path& path)
    {
        boost::system::error_code ec;
        bool result = fs::is_directory(path, ec);
        return !ec && result;
    }
}

std::string SourceFileWatcher::resolve_source_file_path(const std::string& recorded_path,
                                                          const fs::path& project_folder)
{
    if (recorded_path.empty() || path_exists(recorded_path))
        return recorded_path;
    if (!project_folder.empty()) {
        fs::path candidate = project_folder / fs::path(recorded_path).filename();
        if (path_exists(candidate))
            return candidate.string();
    }
    return recorded_path;
}

SourceFileWatcher::SourceFileWatcher()
{
    m_debounce_timer.SetOwner(this, 0);
    Bind(wxEVT_TIMER, &SourceFileWatcher::on_timer, this);
    Bind(wxEVT_FSWATCHER, &SourceFileWatcher::on_fs_event, this);
}

SourceFileWatcher::~SourceFileWatcher()
{
    if (m_watcher != nullptr) {
        m_watcher->RemoveAll();
        delete m_watcher;
    }
}

void SourceFileWatcher::set_watched_files(std::set<std::string> resolved_paths)
{
    if (resolved_paths == m_watched_files)
        return;

    if (m_watcher == nullptr) {
        m_watcher = new wxFileSystemWatcher();
        m_watcher->SetOwner(this);
    }

    m_watcher->RemoveAll();

    // Drop stamps (baseline and failed-attempt) for files no longer tracked; keep them for files
    // that stay tracked so a rearm (e.g. after forget_watched_files()) doesn't erase a pending,
    // not-yet-committed change.
    for (auto it = m_stamps.begin(); it != m_stamps.end(); )
        it = resolved_paths.count(it->first) ? std::next(it) : m_stamps.erase(it);
    for (auto it = m_failed_stamps.begin(); it != m_failed_stamps.end(); )
        it = resolved_paths.count(it->first) ? std::next(it) : m_failed_stamps.erase(it);
    for (auto it = m_retry_counts.begin(); it != m_retry_counts.end(); )
        it = resolved_paths.count(it->first) ? std::next(it) : m_retry_counts.erase(it);

    // Seed a baseline for newly tracked files only.
    std::set<std::string> watched_dirs;
    for (const std::string& file : resolved_paths) {
        if (m_stamps.find(file) == m_stamps.end())
            m_stamps[file] = get_source_stamp(file);
        watched_dirs.insert(fs::path(file).parent_path().string());
    }

    m_watched_files = std::move(resolved_paths);

    for (const std::string& dir : watched_dirs) {
        if (!dir.empty() && is_directory(dir))
            m_watcher->Add(wxFileName(dir, wxEmptyString));
    }

#ifndef _WIN32
    // The directory watch above only fires when the listing changes; an in-place overwrite of an
    // existing file needs a watch on the file itself. Not on Windows: wx's backend rejects
    // file-level watches with a wxLogError dialog, and ReadDirectoryChangesW already reports
    // in-place writes through the directory watch.
    for (const std::string& file : m_watched_files) {
        if (path_exists(file))
            m_watcher->Add(wxFileName(file));
    }
#endif
}

void SourceFileWatcher::clear()
{
    if (m_watcher != nullptr)
        m_watcher->RemoveAll();
    m_watched_files.clear();
    m_stamps.clear();
    m_failed_stamps.clear();
    m_retry_counts.clear();
}

void SourceFileWatcher::forget_watched_files()
{
    m_watched_files.clear();
}

void SourceFileWatcher::on_fs_event(wxFileSystemWatcherEvent&)
{
    // Any event in a watched directory just wakes the debounced check below; see the comment in
    // set_watched_files() for why we don't try to match the event's reported path. Coalesce a
    // burst of events into one 500ms quiet window, but cap the total delay: in a directory with
    // unrelated activity more frequent than that (a sync client, a build directory), restarting
    // the timer on every event would starve it forever and the reload would never fire.
    const auto now = std::chrono::steady_clock::now();
    if (!m_debounce_timer.IsRunning())
        m_debounce_started_at = now;
    else if (now - m_debounce_started_at >= std::chrono::milliseconds(2000))
        return; // cap reached: let the already-pending timer fire instead of pushing it out further
    m_debounce_timer.Start(500, wxTIMER_ONE_SHOT);
}

void SourceFileWatcher::on_timer(wxTimerEvent&)
{
    m_debounce_timer.Stop();
    if (m_reload_in_progress) {
        // The callback below can pump the event loop (a modal dialog, wxBusyInfo) and let this
        // timer fire again while the first reload is still on the stack. Postpone instead of
        // re-entering it: the caller's model/selection state isn't valid to touch twice at once.
        m_debounce_timer.Start(500, wxTIMER_ONE_SHOT);
        return;
    }

    std::map<std::string, SourceStamp> changed = changed_source_files();
    if (changed.empty() || !m_on_changed)
        return;

    std::set<std::string> changed_files;
    for (const auto& [file, stamp] : changed)
        changed_files.insert(file);

    m_reload_in_progress = true;
    struct ScopeGuard { bool& flag; ~ScopeGuard() { flag = false; } } guard{m_reload_in_progress};
    if (m_on_changed(changed_files))
        commit_source_stamps(changed);
    else
        record_failed_attempt(changed);
}

std::map<std::string, SourceStamp> SourceFileWatcher::changed_source_files() const
{
    std::map<std::string, SourceStamp> changed;
    for (const auto& [file, baseline] : m_stamps) {
        SourceStamp current = get_source_stamp(file);
        if (current.mtime == source_file_missing_mtime)
            // Vanished rather than changed -- e.g. a rename-into-place caught mid-flight, or a
            // volume unmounted. Wait for the file to come back instead of treating the
            // disappearance itself as a change to reload.
            continue;
        if (current == baseline)
            continue;
        auto failed_it = m_failed_stamps.find(file);
        if (failed_it != m_failed_stamps.end() && failed_it->second == current)
            // Already tried and failed at exactly this stamp; wait for it to change again rather
            // than retrying a permanently unloadable file on every subsequent event.
            continue;
        changed[file] = current;
    }
    return changed;
}

void SourceFileWatcher::commit_source_stamps(const std::map<std::string, SourceStamp>& stamps)
{
    for (const auto& [file, stamp] : stamps) {
        m_stamps[file] = stamp;
        m_failed_stamps.erase(file);
        m_retry_counts.erase(file);
    }
}

void SourceFileWatcher::record_failed_attempt(const std::map<std::string, SourceStamp>& stamps)
{
    constexpr int base_delay_ms = 1500;
    constexpr int max_delay_ms  = 12000;

    // One shared timer covers the whole batch; use the soonest-due file's delay so a file
    // failing for the first time isn't held up by another that's already backed off further.
    int next_delay_ms = max_delay_ms;
    for (const auto& [file, stamp] : stamps) {
        m_failed_stamps[file] = stamp;
        int& count = m_retry_counts[file];
        int delay_ms = base_delay_ms << std::min(count, 3); // 1.5s, 3s, 6s, 12s, then held at 12s
        next_delay_ms = std::min(next_delay_ms, delay_ms);
        ++count;
    }
    // The failed-stamp record above keeps this from retrying forever if the file never settles:
    // once its stamp stops advancing, changed_source_files() stops reporting it and no further
    // retry gets armed.
    m_debounce_timer.Start(next_delay_ms, wxTIMER_ONE_SHOT);
}

}} // namespace Slic3r::GUI
