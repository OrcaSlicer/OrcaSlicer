#pragma once

#include <wx/event.h>
#include <wx/fswatcher.h>
#include <wx/timer.h>

#include <boost/filesystem/path.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace Slic3r { namespace GUI {

// A tracked file's on-disk identity. mtime is kept at the filesystem's native sub-second
// resolution (ticks of std::filesystem::file_time_type): a binary STL's size depends only on its
// triangle count, so a vertex-only edit re-exported within the same wall-clock second would be
// invisible to a whole-second mtime.
struct SourceStamp
{
    std::int64_t   mtime{ 0 };
    std::uintmax_t size{ 0 };

    bool operator==(const SourceStamp& other) const { return mtime == other.mtime && size == other.size; }
    bool operator!=(const SourceStamp& other) const { return !(*this == other); }
};

// Watches the on-disk source files referenced by the loaded model and notifies its owner once a
// tracked file's content has actually changed. Self-contained and reusable: it knows nothing
// about Model/ModelVolume, Plater, or what "reload" means -- the caller supplies the set of
// resolved paths to watch and a callback to run once a change is confirmed.
//
// A change is reported once its file goes quiet: every fs event naming a tracked file restarts
// the debounce timer, so a file written in place over several chunks is reported after its last
// write, not in the middle. Any other event -- unrelated activity in a watched directory, or a
// backend that only names the directory -- starts the timer if it isn't running but never extends
// it, so unrelated noise can't starve the check. On Windows, where change notifications can lag
// behind cached writes, a file still held open for writing by another process is also held back
// when the timer fires. A write that never goes quiet is reported anyway once max_settle (30s)
// has passed since the first sign of activity. See docs/HLSD/auto-reload.md.
//
// Two watches cover each other's blind spot: an exporter that writes a temp file and renames it
// into place is only visible as a directory-listing change, while an in-place overwrite produces no
// directory event at all and needs a watch on the file itself. Whatever wakes it, the timer
// resamples every tracked file's own stamp against the baseline recorded in set_watched_files()
// -- the event's path only decides whether to extend the wait. Per-file watches are skipped on
// Windows: wx's MSW backend rejects them with an error dialog, and its ReadDirectoryChangesW
// directory watch already reports in-place writes.
class SourceFileWatcher : public wxEvtHandler
{
public:
    // Resolves a volume's recorded source path against a project folder fallback: a volume's
    // recorded source can be a bare filename rather than a full path (a 3MF saved without "Store
    // full source file paths in projects" only keeps the filename). Falls back to the recorded
    // path unchanged if it already exists or nothing is found next to the project.
    static std::string resolve_source_file_path(const std::string& recorded_path,
                                                 const boost::filesystem::path& project_folder);

    SourceFileWatcher();
    ~SourceFileWatcher() override;

    SourceFileWatcher(const SourceFileWatcher&) = delete;
    SourceFileWatcher& operator=(const SourceFileWatcher&) = delete;

    // Invoked (once the changed files have gone quiet) with the set of tracked files confirmed
    // changed. The baseline for every one of them advances unconditionally right before this call
    // -- there is no retry: a settled write either reloads now or it doesn't, and if it doesn't,
    // that's surfaced through the caller's own usual "reload failed" dialog (see
    // Plater::priv::reload_from_disk()), not retried later against the same bytes.
    void set_on_changed(std::function<void(const std::set<std::string>&)> on_changed) { m_on_changed = std::move(on_changed); }

    // Replaces the set of watched files (already resolved to their on-disk paths) and rearms the
    // underlying OS-level watches. Always rearms (needed after forget_watched_files(), even when
    // the path set itself is unchanged); the stamp baseline is left untouched for files that stay
    // tracked, and seeded fresh only for newly-added ones, so a rearm never erases a pending,
    // not-yet-reported change. No-op if the path set is unchanged and nothing was forgotten.
    void set_watched_files(std::set<std::string> resolved_paths);

    // Drops all watches and the tracked baseline, e.g. when the feature is turned off.
    void clear();

    // Forgets which files are currently watched (without touching the stamp baseline or the
    // on/off state) so the next call to set_watched_files() re-arms the OS-level watch from
    // scratch, even if the resolved path set itself is unchanged. A rename-into-place leaves a
    // file-level watch bound to the old inode, so the set of paths looking the same does not mean
    // the watch is still live.
    void forget_watched_files();

private:
    void on_fs_event(wxFileSystemWatcherEvent& evt);
    void on_timer(wxTimerEvent& evt);

    // Whether the event names a tracked file (as its path, or as a rename's new path).
    bool is_tracked_file_event(const wxFileSystemWatcherEvent& evt) const;
    // Whether max_settle has passed since the first sign of the current burst of activity.
    bool settle_expired() const;

    // Returns the tracked files whose stamp differs from the committed baseline, without
    // mutating anything -- a vanished file (mtime reads as missing) is skipped rather than
    // reported, since a single atomic rename(2) over an existing destination can't cause a file
    // to vanish (see docs/HLSD/auto-reload.md); wait for it to come back instead.
    std::map<std::string, SourceStamp> changed_source_files() const;

    std::function<void(const std::set<std::string>&)> m_on_changed;
    wxFileSystemWatcher*               m_watcher{ nullptr };
    wxTimer                            m_debounce_timer;
    std::set<std::string>              m_watched_files;
    std::map<std::string, SourceStamp> m_stamps; // committed baseline
    // When the current burst of activity began; unset while idle. Bounds how long a tracked file
    // that keeps being written can hold back its own report.
    std::optional<std::chrono::steady_clock::time_point> m_settle_start;
    // Guards m_on_changed() against re-entry from a nested event loop pumped during the reload it
    // triggers (a modal dialog, wxBusyInfo) while this timer is re-armed by another fs event.
    bool                                m_reload_in_progress{ false };
};

}} // namespace Slic3r::GUI
