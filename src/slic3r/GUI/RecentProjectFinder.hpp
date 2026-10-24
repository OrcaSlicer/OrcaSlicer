#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <boost/thread.hpp>
#include <wx/string.h>

#include "libslic3r/Format/bbs_3mf.hpp"

namespace Slic3r { namespace GUI {

// A 3MF of the recent files history, as the finder sees it.
struct RecentProjectEntry
{
    wxString    path;
    std::string hash;
    bool        missing = false;
};

// A 3MF found on disk, with what the Home page shows about it.
struct FoundProject
{
    wxString          path;
    std::string       hash;
    std::string       thumbnail;
    Bbs3mfListingInfo listing;
};

struct RecentProjectMoves
{
    std::vector<std::pair<wxString, FoundProject>> relinks;   // missing history path -> where the file is now
    std::vector<FoundProject>                      additions; // new files, not in the history yet
    std::vector<std::pair<wxString, std::string>>  hashes;    // history files that had no hash yet
};

// Empty when the file can't be read.
std::string project_file_hash(const wxString& path);

// Matches the found files to the missing history files: by hash, or by file name when the history
// file was never hashed. A found file that is already in the history, or a copy of one, is skipped.
RecentProjectMoves plan_recent_project_moves(const std::vector<RecentProjectEntry>& history,
                                             const std::vector<FoundProject>&       found,
                                             bool                                   add_new);

// Scans a folder and its subfolders for 3MFs on a worker thread, one scan at a time.
class RecentProjectFinder
{
public:
    using Done = std::function<void(RecentProjectMoves)>;

    ~RecentProjectFinder();

    bool is_running() const { return m_running; }
    // Does nothing while a scan is running. done is called on the worker thread.
    void start(const wxString& folder, std::vector<RecentProjectEntry> history, bool add_new, Done done);

private:
    boost::thread     m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancel{false};
};

}} // namespace Slic3r::GUI
