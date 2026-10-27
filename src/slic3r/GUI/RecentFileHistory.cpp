#include "RecentFileHistory.hpp"

#include <algorithm>
#include <ctime>
#include <string>
#include <utility>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <wx/filename.h>

#include "slic3r/GUI/GUI.hpp"

namespace Slic3r { namespace GUI {

RecentFileKind recent_file_kind(const wxString &path)
{
    const wxString extension = wxFileName(path).GetExt().Lower();
    if (extension == "3mf")
        return RecentFileKind::Project;
    if (extension == "step" || extension == "stp")
        return RecentFileKind::Step;
    return RecentFileKind::Mesh;
}

const std::string& RecentFileHistory::GetThumbnail(int index) const
{
    static const std::string no_thumbnail;
    return index >= 0 && index < static_cast<int>(m_thumbnails.size()) ? m_thumbnails[index] : no_thumbnail;
}

bool RecentFileHistory::GetPublished(int index) const
{
    return index >= 0 && index < static_cast<int>(m_listing_info.size()) && m_listing_info[index].published;
}

bool RecentFileHistory::GetHasDesigner(int index) const
{
    return index >= 0 && index < static_cast<int>(m_listing_info.size()) && m_listing_info[index].has_designer;
}

void RecentFileHistory::AddFileToHistory(const wxString &file)
{
    if (this->m_fileMaxFiles == 0)
        return;
    // The base class removes a file already in the history before adding it on top: keep what is known of it.
    const size_t   old_index = FindFileInHistory(file);
    RecentFileInfo info      = old_index < m_file_info.size() ? m_file_info[old_index] : RecentFileInfo{{}, std::time(nullptr), {}};
    wxFileHistory::AddFileToHistory(file);
    if (m_load_called) {
        m_thumbnails.push_front(bbs_3mf_get_thumbnail(into_u8(file).c_str()));
        m_listing_info.push_front(bbs_3mf_get_listing_info(into_u8(file)));
        // Hashed again each time, as a project saved over has changed.
        if (recent_file_kind(file) == RecentFileKind::Project)
            info.hash = project_file_hash(file);
    } else {
        m_thumbnails.push_front("");
        m_listing_info.push_front({});
    }
    m_file_info.push_front(std::move(info));
    m_recent_count = std::min(m_recent_count + 1, GetCount());
}

void RecentFileHistory::RemoveFileFromHistory(size_t i)
{
    if (i >= m_thumbnails.size()) // FIX zero max
        return;
    wxFileHistory::RemoveFileFromHistory(i);
    m_thumbnails.erase(m_thumbnails.begin() + i);
    m_listing_info.erase(m_listing_info.begin() + i);
    m_file_info.erase(m_file_info.begin() + i);
    if (i < m_recent_count)
        --m_recent_count;
}

bool RecentFileHistory::AppendFileToHistory(const FoundProject &file)
{
    if (GetCount() >= m_fileMaxFiles)
        return false;
    m_fileHistory.Add(file.path);
    m_thumbnails.push_back(file.thumbnail);
    m_listing_info.push_back(file.listing);
    m_file_info.push_back({file.hash, std::time(nullptr), {}});
    return true;
}

void RecentFileHistory::RelinkFile(size_t index, const FoundProject &file)
{
    m_fileHistory[index]  = file.path;
    m_thumbnails[index]   = file.thumbnail;
    m_listing_info[index] = file.listing;
    m_file_info[index].hash = file.hash;
}

void RecentFileHistory::SetMaxFiles(size_t max)
{
    m_fileMaxFiles = max;
    while (GetCount() > max)
        RemoveFileFromHistory(GetCount() - 1);
}

size_t RecentFileHistory::FindFileInHistory(const wxString & file) const
{
    return m_fileHistory.Index(file);
}

void RecentFileHistory::LoadThumbnails()
{
    tbb::parallel_for(tbb::blocked_range<size_t>(0, GetCount()), [this](tbb::blocked_range<size_t> range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            auto thumbnail = bbs_3mf_get_thumbnail(into_u8(GetHistoryFile(i)).c_str());
            if (!thumbnail.empty()) {
                m_thumbnails[i] = thumbnail;
            }
            m_listing_info[i] = bbs_3mf_get_listing_info(into_u8(GetHistoryFile(i)));
        }
    });
    m_load_called = true;
}

}} // namespace Slic3r::GUI
