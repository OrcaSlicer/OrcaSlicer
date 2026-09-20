#ifndef slic3r_GUI_SourceFileWatcher_hpp_
#define slic3r_GUI_SourceFileWatcher_hpp_

#include <wx/event.h>
#include <wx/fswatcher.h>
#include <wx/timer.h>

#include <boost/filesystem/path.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace Slic3r { namespace GUI {

// A tracked file's on-disk identity: mtime alone is whole-second resolution, so a second write
// landing in the same wall-clock second as a first would otherwise be invisible.
struct SourceStamp
{
    std::time_t    mtime{ 0 };
    std::uintmax_t size{ 0 };

    bool operator==(const SourceStamp& other) const { return mtime == other.mtime && size == other.size; }
    bool operator!=(const SourceStamp& other) const { return !(*this == other); }
};

// Watches the on-disk source files referenced by the loaded model and notifies its owner once a
// tracked file's content has actually changed. Self-contained and reusable: it knows nothing
// about Model/ModelVolume, Plater, or what "reload" means -- the caller supplies the set of
// resolved paths to watch and a callback to run once a change is confirmed.
//
// Two watches cover each other's blind spot: most exporters write a temp file and rename it into
// place (only visible as a directory-listing change), while an in-place overwrite produces no
// directory event at all and needs a watch on the file itself. The event handler doesn't try to
// match the reported path -- macOS's kqueue backend can report a rename with just the directory
// and no filename -- it only wakes a debounced stamp comparison against the baseline recorded in
// set_watched_files(). Per-file watches are skipped on Windows: wx's MSW backend rejects them
// with an error dialog, and its ReadDirectoryChangesW directory watch already reports in-place
// writes.
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

    // Invoked (after the debounce delay) with the set of tracked files confirmed changed. Must
    // return whether the caller's reload actually succeeded: the changed files' stamps are only
    // committed to the baseline on true, so a failed/partial reload (a file still being written,
    // locked, or one this build can't parse) is retried instead of silently accepted.
    void set_on_changed(std::function<bool(const std::set<std::string>&)> on_changed) { m_on_changed = std::move(on_changed); }

    // Replaces the set of watched files (already resolved to their on-disk paths) and rearms the
    // underlying OS-level watches. Always rearms (needed after forget_watched_files(), even when
    // the path set itself is unchanged); the stamp baseline is left untouched for files that stay
    // tracked, and seeded fresh only for newly-added ones, so a rearm never erases a pending,
    // not-yet-committed change. No-op if the path set is unchanged and nothing was forgotten.
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

    // Returns the tracked files whose stamp differs from the committed baseline, without
    // mutating anything -- a vanished file (mtime reads as missing) is skipped rather than
    // reported, and a file stuck at the exact stamp of its last failed attempt is skipped too,
    // so a permanently unloadable file doesn't retry on every subsequent event.
    std::map<std::string, SourceStamp> changed_source_files() const;
    // Advances the baseline to the given stamps (a successful reload) and clears any recorded
    // failure and retry count for them.
    void commit_source_stamps(const std::map<std::string, SourceStamp>& stamps);
    // Records a failed attempt at the given stamps and arms another retry, backing off per file
    // (1.5s, 3s, 6s, capped at 12s) for the transient case of a file still being written or
    // briefly locked -- e.g. a large STEP export that takes longer than one short retry to settle.
    void record_failed_attempt(const std::map<std::string, SourceStamp>& stamps);

    std::function<bool(const std::set<std::string>&)> m_on_changed;
    wxFileSystemWatcher*               m_watcher{ nullptr };
    wxTimer                            m_debounce_timer;
    // When the current debounce coalescing window opened (on_fs_event() only); caps how long a
    // burst of unrelated directory activity can keep pushing the check out.
    std::chrono::steady_clock::time_point m_debounce_started_at;
    std::set<std::string>              m_watched_files;
    std::map<std::string, SourceStamp> m_stamps;        // committed baseline
    std::map<std::string, SourceStamp> m_failed_stamps; // stamp of the last failed attempt, if any
    std::map<std::string, int>         m_retry_counts;  // consecutive failed attempts, per file
    // Guards m_on_changed() against re-entry from a nested event loop pumped during the reload
    // it triggers (a modal dialog, wxBusyInfo) while this timer is re-armed by another fs event.
    bool                                m_reload_in_progress{ false };
    // True while m_debounce_timer is running as a failed reload's backoff rather than a debounce.
    bool                                m_backoff_armed{ false };
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_SourceFileWatcher_hpp_
