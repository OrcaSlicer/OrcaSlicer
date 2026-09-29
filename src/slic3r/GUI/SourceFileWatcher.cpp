#include "SourceFileWatcher.hpp"

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

namespace {
    // Sentinel for "file does not currently exist" so a create is detected as a change too.
    constexpr std::time_t source_file_missing_mtime = 0;

    // Coalesces the handful of fs events one atomic write can produce (e.g. the several events a
    // single rename-into-place triggers) into one check. Under the atomic-write assumption
    // there's nothing further to wait out beyond this -- no resampling, no cap -- so this is the
    // only delay in the whole path.
    constexpr int debounce_ms = 300;

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

    // Drop the baseline for files no longer tracked; keep it for files that stay tracked so a
    // rearm (e.g. after forget_watched_files()) doesn't erase a pending, not-yet-reported change.
    for (auto it = m_stamps.begin(); it != m_stamps.end(); )
        it = resolved_paths.count(it->first) ? std::next(it) : m_stamps.erase(it);

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
}

void SourceFileWatcher::forget_watched_files()
{
    m_watched_files.clear();
}

void SourceFileWatcher::on_fs_event(wxFileSystemWatcherEvent&)
{
    // Restarting an already-running one-shot timer just extends it -- exactly the coalescing a
    // burst of events from one atomic write needs. No anti-starvation cap: under the atomic-write
    // assumption a burst from one change is inherently brief, so unlike a design that has to keep
    // resampling until a file settles, there's nothing pathological here to guard against.
    m_debounce_timer.Start(debounce_ms, wxTIMER_ONE_SHOT);
}

void SourceFileWatcher::on_timer(wxTimerEvent&)
{
    if (m_reload_in_progress) {
        // The callback below can pump the event loop (a modal dialog, wxBusyInfo) and let this
        // timer fire again while the first reload is still on the stack. Postpone instead of
        // re-entering it: the caller's model/selection state isn't valid to touch twice at once.
        m_debounce_timer.Start(debounce_ms, wxTIMER_ONE_SHOT);
        return;
    }

    std::map<std::string, SourceStamp> changed = changed_source_files();
    if (changed.empty() || !m_on_changed)
        return; // nothing to watch, or nobody to report to; go idle until the next fs event

    // Advance the baseline for every changed file before calling out, not after: an assumed-
    // atomic write means there's nothing left to retry, so whether the reload below succeeds or
    // not, this is the last attempt that stamp will ever get -- a failure is reported through the
    // callback's own usual path (the same dialog a manual reload would show), not retried here.
    std::set<std::string> changed_files;
    for (const auto& [file, stamp] : changed) {
        m_stamps[file] = stamp;
        changed_files.insert(file);
    }

    m_reload_in_progress = true;
    struct ScopeGuard { bool& flag; ~ScopeGuard() { flag = false; } } guard{m_reload_in_progress};
    m_on_changed(changed_files);

    // Plater's forget_watched_files() + update_source_file_watches() runs unconditionally after
    // every call, tearing down and rebuilding the underlying OS watch regardless of outcome. A
    // write landing in that window can be missed: the backend's own teardown/rebuild bookkeeping
    // (e.g. inotify's IN_IGNORED for the removed watch descriptors) is asynchronous, so nothing
    // guarantees an event during it is still delivered. This tick catches that; it's a no-op if
    // nothing changed since (changed_source_files() finds nothing and this returns immediately).
    m_debounce_timer.Start(debounce_ms, wxTIMER_ONE_SHOT);
}

std::map<std::string, SourceStamp> SourceFileWatcher::changed_source_files() const
{
    std::map<std::string, SourceStamp> changed;
    for (const auto& [file, baseline] : m_stamps) {
        SourceStamp current = get_source_stamp(file);
        if (current.mtime == source_file_missing_mtime)
            // Vanished rather than changed -- e.g. a source replaced via unlink()-then-rename()
            // (common on Windows, since the classic rename() fails over an existing destination),
            // a cross-device move falling back to copy-then-delete, or a volume unmounted. A
            // single atomic rename(2) over an existing destination can't cause this by itself --
            // see docs/HLSD/auto-reload.md. Wait for the file to come back instead of treating
            // the disappearance itself as a change to reload.
            continue;
        if (current != baseline)
            changed[file] = current;
    }
    return changed;
}

}} // namespace Slic3r::GUI
