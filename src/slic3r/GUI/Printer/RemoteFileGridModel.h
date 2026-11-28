#pragma once

#include "IFileGridModel.h"

#include "slic3r/Utils/IPrinterAgent.hpp"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Slic3r {
namespace GUI {

class RemoteFileGridModel : public IFileGridModel
{
public:
    enum class Status { Loading, Ready, Empty, Failed };

    explicit RemoteFileGridModel(std::string device_id);
    ~RemoteFileGridModel() override;

    void Refresh();
    Status GetStatus() const { return m_status; }
    void SetStatusHandler(std::function<void(Status)> handler);
    void SetGroupModeHandler(std::function<void(FileGridGroup)> handler);

    size_t GetCount() const override;
    const FileGridCard& GetFile(size_t index, bool& selected) const override;
    const FileGridCard& GetFile(size_t index) const override;

    FileGridType GetFileType() const override;
    void SetFileType(FileGridType type, const std::string& storage) override;
    FileGridGroup GetGroupMode() const override;
    void SetGroupMode(FileGridGroup mode) override;

    size_t GetIndexAtTime(time_t time) const override;
    size_t EnterSubGroup(size_t index) override;

    void ToggleSelect(size_t index) override;
    void SelectAll(bool select) override;
    void SetFocusRange(size_t start, size_t count) override;

    void Retry() override;
    int GetLastError() const override;
    void DownloadCheckFiles(const std::string& path) override;
    bool preserve_thumbnail_aspect() const override { return true; }

    // Delete `path` from the printer; `done` reports success on the UI thread.
    void DeleteFile(const std::string& path, std::function<void(bool ok)> done);

    void SetChangeHandler(std::function<void(FileGridChange)> handler) override;

private:
    void setStatus(Status status);
    void onFilesLoaded(std::uint64_t request, const std::string& device_id, int result,
                       std::vector<PrinterFileEntry> files);
    void buildGroups();
    size_t cardIndexForGroup(size_t index) const;
    void requestThumbnail(const std::string& path);
    void pumpThumbnailQueue();
    void onThumbnailLoaded(std::uint64_t request, const std::string& device_id,
                           const std::string& path, int result, std::string image);
    void requestMetadata(const std::string& path);
    void pumpMetadataQueue();
    void onMetadataLoaded(std::uint64_t request, const std::string& device_id,
                          const std::string& path, int result, PrinterFileMetadata meta);

    std::string m_device_id;
    mutable std::vector<FileGridCard> m_cards;
    std::vector<size_t> m_month;
    std::vector<size_t> m_year;
    std::function<void(FileGridChange)> m_change_handler;
    std::function<void(Status)> m_status_handler;
    std::function<void(FileGridGroup)> m_group_mode_handler;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
    std::unordered_map<std::string, wxBitmap> m_thumbnail_cache;
    std::unordered_set<std::string> m_thumbnail_requested;
    std::deque<std::string> m_thumbnail_queue;
    std::unordered_map<std::string, PrinterFileMetadata> m_metadata_cache;
    std::unordered_set<std::string> m_metadata_requested;
    std::deque<std::string> m_metadata_queue;
    std::uint64_t m_request = 0;
    size_t m_thumbnail_requests_in_flight = 0;
    bool m_pumping_thumbnail_queue = false;
    size_t m_metadata_requests_in_flight = 0;
    bool m_pumping_metadata_queue = false;
    int m_last_error = 0;
    FileGridType m_file_type = FileGridType::Timelapse;
    FileGridGroup m_group_mode = FileGridGroup::All;
    Status m_status = Status::Loading;
};

}}
