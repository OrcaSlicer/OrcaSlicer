#pragma once

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <deque>
#include <string>
#include <utility>

#include <wx/filehistory.h>
#include <wx/string.h>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "RecentProjectFinder.hpp"

namespace Slic3r { namespace GUI {

enum class RecentFileKind
{
    Project,
    Mesh,
    Step
};

RecentFileKind recent_file_kind(const wxString& path);

// What the history knows of a file beyond its path. Saved by path, apart from the history.
struct RecentFileInfo
{
    std::string hash;      // md5 of a 3MF, to find it again once moved
    std::time_t added = 0; // 0 when the file was in the history before dates were kept
    std::string tab;       // Home page tab the user moved the file to, empty to follow the path rule
};

// The recent files history behind the File menu and the Home page, with each file's thumbnail and
// what is known of it.
struct RecentFileHistory : wxFileHistory
{
    RecentFileHistory(int max) : wxFileHistory(max) {}
    const std::string&    GetThumbnail(int index) const;
    bool                  GetPublished(int index) const;
    bool                  GetHasDesigner(int index) const;
    const RecentFileInfo& GetFileInfo(size_t index) const { return m_file_info[index]; }
    void                  SetFileInfo(size_t index, RecentFileInfo info) { m_file_info[index] = std::move(info); }

    virtual void AddFileToHistory(const wxString& file);
    virtual void RemoveFileFromHistory(size_t i);
    size_t       FindFileInHistory(const wxString& file) const;
    // Adds a file found on disk at the bottom, out of the Recent list. False when the history is full.
    bool AppendFileToHistory(const FoundProject& file);
    // Points a missing file to where it is now, keeping its place in the history.
    void RelinkFile(size_t index, const FoundProject& file);
    // Drops the oldest files that no longer fit.
    void SetMaxFiles(size_t max);

    void LoadThumbnails();

    // How many files at the top of the history were opened since the Recent list was last cleared.
    size_t GetRecentCount() const { return m_recent_count; }
    void   SetRecentCount(size_t count) { m_recent_count = std::min(count, GetCount()); }
    void   ClearRecent() { m_recent_count = 0; }

private:
    std::deque<std::string>       m_thumbnails;
    std::deque<Bbs3mfListingInfo> m_listing_info; // parallel to m_thumbnails
    std::deque<RecentFileInfo>    m_file_info;    // parallel to m_thumbnails
    size_t                        m_recent_count = 0;
    bool                          m_load_called  = false;
};

}} // namespace Slic3r::GUI
