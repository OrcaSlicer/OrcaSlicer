#include "BambuFileGridModel.h"

#include "slic3r/GUI/GUI_App.hpp"

#include <utility>

namespace Slic3r {
namespace GUI {

static const FileGridCard& emptyCard()
{
    static const FileGridCard card;
    return card;
}

BambuFileGridModel::BambuFileGridModel(boost::shared_ptr<PrinterFileSystem> fs)
    : m_fs(std::move(fs))
{
    if (m_fs) {
        m_fs->Bind(EVT_FILE_CHANGED, &BambuFileGridModel::onFileChanged, this);
        m_fs->Bind(EVT_MODE_CHANGED, &BambuFileGridModel::onModeChanged, this);
        m_fs->Bind(EVT_THUMBNAIL, &BambuFileGridModel::onThumbnail, this);
        m_fs->Bind(EVT_DOWNLOAD, &BambuFileGridModel::onDownload, this);
    }
}

BambuFileGridModel::~BambuFileGridModel()
{
    if (m_fs) {
        m_fs->Unbind(EVT_FILE_CHANGED, &BambuFileGridModel::onFileChanged, this);
        m_fs->Unbind(EVT_MODE_CHANGED, &BambuFileGridModel::onModeChanged, this);
        m_fs->Unbind(EVT_THUMBNAIL, &BambuFileGridModel::onThumbnail, this);
        m_fs->Unbind(EVT_DOWNLOAD, &BambuFileGridModel::onDownload, this);
    }
}

void BambuFileGridModel::EnsureCards() const
{
    if (!m_dirty && (!m_fs || m_cards.size() == m_fs->GetCount()))
        return;
    m_dirty = false;
    m_cards.clear();
    if (!m_fs)
        return;
    size_t count = m_fs->GetCount();
    m_cards.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        bool selected = false;
        PrinterFileSystem::File const &file = m_fs->GetFile(i, selected);
        FileGridCard card;
        card.id                = file.path;
        card.name              = file.name;
        card.title             = file.Title();
        card.time_text         = file.Metadata("Time", "");
        card.weight_text       = file.Metadata("Weight", "");
        card.time              = file.time;
        card.downloading       = file.IsDownload();
        card.download_progress = file.DownloadProgress();
        card.selected          = selected;
        card.thumbnail         = file.thumbnail;
        m_cards.push_back(std::move(card));
    }
}

size_t BambuFileGridModel::GetCount() const
{
    return m_fs ? m_fs->GetCount() : 0;
}

const FileGridCard& BambuFileGridModel::GetFile(size_t index, bool& selected) const
{
    EnsureCards();
    if (!m_fs || index >= m_cards.size()) {
        selected = false;
        return emptyCard();
    }
    m_fs->GetFile(index, selected);
    return m_cards[index];
}

const FileGridCard& BambuFileGridModel::GetFile(size_t index) const
{
    EnsureCards();
    if (index >= m_cards.size())
        return emptyCard();
    return m_cards[index];
}

FileGridType BambuFileGridModel::GetFileType() const
{
    return m_fs ? static_cast<FileGridType>(m_fs->GetFileType()) : FileGridType::Timelapse;
}

void BambuFileGridModel::SetFileType(FileGridType type, const std::string& storage)
{
    if (m_fs)
        m_fs->SetFileType(static_cast<PrinterFileSystem::FileType>(type), storage);
}

FileGridGroup BambuFileGridModel::GetGroupMode() const
{
    return m_fs ? static_cast<FileGridGroup>(m_fs->GetGroupMode()) : FileGridGroup::All;
}

void BambuFileGridModel::SetGroupMode(FileGridGroup mode)
{
    if (m_fs)
        m_fs->SetGroupMode(static_cast<PrinterFileSystem::GroupMode>(mode));
}

size_t BambuFileGridModel::GetIndexAtTime(time_t time) const
{
    return m_fs ? m_fs->GetIndexAtTime(static_cast<boost::uint32_t>(time)) : 0;
}

size_t BambuFileGridModel::EnterSubGroup(size_t index)
{
    return m_fs ? m_fs->EnterSubGroup(index) : index;
}

void BambuFileGridModel::ToggleSelect(size_t index)
{
    if (m_fs)
        m_fs->ToggleSelect(index);
}

void BambuFileGridModel::SelectAll(bool select)
{
    if (m_fs)
        m_fs->SelectAll(select);
}

void BambuFileGridModel::SetFocusRange(size_t start, size_t count)
{
    if (m_fs)
        m_fs->SetFocusRange(start, count);
}

void BambuFileGridModel::Retry()
{
    if (m_fs)
        m_fs->Retry();
}

int BambuFileGridModel::GetLastError() const
{
    return m_fs ? m_fs->GetLastError() : 1;
}

void BambuFileGridModel::DownloadCheckFiles(const std::string& path)
{
    if (m_fs)
        m_fs->DownloadCheckFiles(path);
}

void BambuFileGridModel::SetChangeHandler(std::function<void(FileGridChange)> handler)
{
    m_handler = std::move(handler);
}

void BambuFileGridModel::notify(FileGridChange change)
{
    if (m_handler)
        m_handler(change);
}

void BambuFileGridModel::onFileChanged(wxCommandEvent& evt)
{
    evt.Skip();
    if (evt.GetInt() == -1 && m_fs)
        m_fs->DownloadCheckFiles(wxGetApp().app_config->get("download_path"));
    m_dirty = true;
    notify(FileGridChange::Files);
}

void BambuFileGridModel::onModeChanged(wxCommandEvent& evt)
{
    evt.Skip();
    m_dirty = true;
    notify(FileGridChange::Mode);
}

void BambuFileGridModel::onThumbnail(wxCommandEvent& evt)
{
    evt.Skip();
    m_dirty = true;
    notify(FileGridChange::Thumbnail);
}

void BambuFileGridModel::onDownload(wxCommandEvent& evt)
{
    evt.Skip();
    m_dirty = true;
    notify(FileGridChange::Download);
}

}}
