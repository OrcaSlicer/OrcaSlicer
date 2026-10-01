#include "SourceFileWatcher.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>
#include <thread>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <Windows.h>
    #include <boost/nowide/convert.hpp>
#endif

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

namespace {
    // Sentinel for "file does not currently exist" so a create is detected as a change too.
    constexpr std::int64_t source_file_missing_mtime = std::numeric_limits<std::int64_t>::min();

    // How long a tracked file has to stay quiet (no fs event naming it) before it's reported.
    constexpr int debounce_ms = 500;

    // Backstop for a tracked file that never goes quiet: once this long has passed since the
    // first sign of activity, it's reported as it stands rather than held back any longer.
    constexpr auto max_settle = std::chrono::seconds(30);

    // TEST-ONLY hook for the headless verification script: never exposed via Preferences, and
    // inert unless ORCA_TEST_HOOKS_DIR is explicitly exported, so it can't fire in a real user's
    // session. If <ORCA_TEST_HOOKS_DIR>/stall_main_thread holds a number of seconds, consume the
    // file and block the calling (main) thread that long, so a script can hold the event loop
    // busy while a writer keeps going -- what a project backup or any long UI task does.
    void stall_main_thread_for_test_if_requested()
    {
        const char* hooks_dir = std::getenv("ORCA_TEST_HOOKS_DIR");
        if (hooks_dir == nullptr)
            return;
        const fs::path hook = fs::path(hooks_dir) / "stall_main_thread";
        double seconds = 0.;
        {
            std::ifstream ifs(hook.string());
            if (!(ifs >> seconds) || seconds <= 0.)
                return;
        }
        boost::system::error_code ec;
        fs::remove(hook, ec);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": stalling the main thread for " << seconds
                                << "s via ORCA_TEST_HOOKS_DIR";
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    }

    SourceStamp get_source_stamp(const std::string& path)
    {
        // std::filesystem rather than boost's for the mtime: boost only exposes whole seconds.
        // u8path because OrcaSlicer's std::string paths are UTF-8, which Windows would otherwise
        // read in the ANSI code page.
        std::error_code ec;
        const std::filesystem::path p = std::filesystem::u8path(path);
        const auto mtime = std::filesystem::last_write_time(p, ec);
        if (ec)
            return SourceStamp{source_file_missing_mtime, 0};
        const std::uintmax_t size = std::filesystem::file_size(p, ec);
        if (ec)
            return SourceStamp{source_file_missing_mtime, 0};
        // libc++'s file_time_type counts nanoseconds in a 128-bit rep; int64 nanoseconds last
        // until 2262.
        return SourceStamp{static_cast<std::int64_t>(mtime.time_since_epoch().count()), size};
    }

#ifdef _WIN32
    // ReadDirectoryChangesW only reports a size/mtime change once the write reaches the disk, and
    // NTFS updates the directory entry lazily while the file is open, so neither the events nor
    // the stamp reliably show a write still in progress. Whether another process still holds the
    // file open for writing does: a share mode that excludes writers fails against any open
    // handle with write access.
    bool is_open_for_writing(const std::string& path)
    {
        HANDLE handle = ::CreateFileW(boost::nowide::widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return ::GetLastError() == ERROR_SHARING_VIOLATION;
        ::CloseHandle(handle);
        return false;
    }
#endif

    // The form event paths and tracked paths are compared in: wxFileName::SameAs()'s own
    // normalization (case folding where the filesystem needs it, separators, dots), done once per
    // path so a lookup in a set replaces SameAs() against every tracked file.
    std::string normalized_path(const wxFileName& path)
    {
        wxFileName normalized(path);
        normalized.Normalize(wxPATH_NORM_ALL);
        return normalized.GetFullPath().utf8_string();
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
                                                          const std::string& object_input_file)
{
    if (recorded_path.empty() || path_exists(recorded_path))
        return recorded_path;
    // Same fallback as Plater::priv::reload_from_disk(): next to the object's input file.
    if (!object_input_file.empty()) {
        fs::path candidate = fs::path(object_input_file).remove_filename();
        if (!candidate.empty()) {
            candidate /= fs::path(recorded_path).filename();
            if (path_exists(candidate))
                return candidate.string();
        }
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

    const bool had_watches = !m_stamps.empty();
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

    m_tracked_normalized.clear();
    for (const auto& entry : m_stamps)
        m_tracked_normalized.insert(normalized_path(wxFileName(wxString::FromUTF8(entry.first))));

    // Paths are UTF-8; passing the std::string straight to wxFileName would read it in the ANSI
    // code page on Windows and fail to watch any folder with a non-ASCII name.
    for (const std::string& dir : watched_dirs) {
        if (!dir.empty() && is_directory(dir) &&
            !m_watcher->Add(wxFileName(wxString::FromUTF8(dir), wxEmptyString)))
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": could not watch directory " << dir;
    }

#ifndef _WIN32
    // An in-place overwrite of an existing file needs a watch on the file itself where the
    // directory watch doesn't report it. That is only macOS (kqueue, which sees listing changes
    // only): on Linux, inotify's directory watch already reports writes to its children, and a
    // second watch on the file just duplicates every event. A symlinked source is the exception
    // on Linux, because its content lives in another directory. Not on Windows: wx's backend
    // rejects file-level watches with a wxLogError dialog, and ReadDirectoryChangesW already
    // reports in-place writes through the directory watch.
    for (const std::string& file : m_watched_files) {
        if (!path_exists(file))
            continue;
#ifndef __APPLE__
        boost::system::error_code ec;
        if (!fs::is_symlink(fs::path(file), ec))
            continue;
#endif
        if (!m_watcher->Add(wxFileName(wxString::FromUTF8(file))))
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": could not watch file " << file;
    }
#endif

    // RemoveAll() above discards events the backend had queued but not yet delivered. A change
    // that landed just before this rebuild would then wait for some unrelated event to wake the
    // timer, so look once more after the rebuild; it's a no-op if nothing changed.
    if (had_watches)
        m_debounce_timer.Start(debounce_ms, wxTIMER_ONE_SHOT);
}

void SourceFileWatcher::clear()
{
    if (m_watcher != nullptr)
        m_watcher->RemoveAll();
    m_watched_files.clear();
    m_stamps.clear();
    m_tracked_normalized.clear();
}

void SourceFileWatcher::forget_watched_files()
{
    m_watched_files.clear();
}

bool SourceFileWatcher::is_tracked_file_event(const wxFileSystemWatcherEvent& evt) const
{
    // Reads (inotify's IN_ACCESS, including OrcaSlicer's own reload) and attribute changes don't
    // mean the file is still being written.
    const int type = evt.GetChangeType();
    if ((type & (wxFSW_EVENT_CREATE | wxFSW_EVENT_DELETE | wxFSW_EVENT_RENAME | wxFSW_EVENT_MODIFY)) == 0)
        return false;

    // Both sides are normalized, so case and separators don't matter on Windows.
    auto tracked = [this](const wxFileName& path) {
        return path.IsOk() && m_tracked_normalized.count(normalized_path(path)) != 0;
    };
    return tracked(evt.GetPath()) || (type == wxFSW_EVENT_RENAME && tracked(evt.GetNewPath()));
}

bool SourceFileWatcher::settle_expired() const
{
    return m_settle_start && std::chrono::steady_clock::now() - *m_settle_start >= max_settle;
}

void SourceFileWatcher::on_fs_event(wxFileSystemWatcherEvent& evt)
{
    if (!m_settle_start)
        m_settle_start = std::chrono::steady_clock::now();

    // An event naming a tracked file restarts the timer, so a file written in place over several
    // chunks is only reported once it goes quiet -- up to max_settle, after which the pending
    // tick is left to fire and report it as it stands.
    //
    // Any other event only starts the timer if one isn't already pending, never extends it. On
    // Windows there's no per-file watch, so every event in a watched directory reaches here
    // through the single directory watch; restarting on those too would starve the check
    // indefinitely under sustained unrelated activity in the same directory (a build process, a
    // sync client, anything else writing nearby). Events that don't name a tracked file still
    // start the timer, because some backends report a rename-into-place with just the directory
    // (macOS's kqueue) or drop the path entirely (a Windows buffer overflow warning).
    if (is_tracked_file_event(evt) && !settle_expired())
        m_debounce_timer.Start(debounce_ms, wxTIMER_ONE_SHOT);
    else if (!m_debounce_timer.IsRunning())
        m_debounce_timer.Start(debounce_ms, wxTIMER_ONE_SHOT);

    stall_main_thread_for_test_if_requested();
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

    if (!m_settle_start)
        m_settle_start = std::chrono::steady_clock::now();

    std::map<std::string, SourceStamp> changed = changed_source_files();

    bool deferred = false;
#ifdef _WIN32
    // Hold back a file another process still has open for writing (see is_open_for_writing());
    // the rest of the batch goes ahead. Bounded by the same backstop as the event restarts.
    if (!settle_expired()) {
        for (auto it = changed.begin(); it != changed.end();) {
            if (is_open_for_writing(it->first)) {
                it = changed.erase(it);
                deferred = true;
            } else
                ++it;
        }
    }
#endif
    if (deferred)
        m_debounce_timer.Start(debounce_ms, wxTIMER_ONE_SHOT);

    if (changed.empty() || !m_on_changed) {
        // Nothing to watch, or nobody to report to; go idle until the next fs event.
        if (!deferred)
            m_settle_start.reset();
        return;
    }

    // Advance the baseline for every changed file before calling out, not after: the file has
    // settled, so whether the reload below succeeds or not, this is the last attempt that stamp
    // will ever get -- a failure is reported through the callback's own usual path (the same
    // dialog a manual reload would show), not retried here.
    std::set<std::string> changed_files;
    for (const auto& [file, stamp] : changed) {
        m_stamps[file] = stamp;
        changed_files.insert(file);
    }
    if (!deferred)
        m_settle_start.reset();

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
