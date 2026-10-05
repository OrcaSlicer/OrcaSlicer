#include "RemoteFileGridModel.h"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <wx/datetime.h>
#include <wx/image.h>
#include <wx/mstream.h>

#include <boost/format.hpp>
#include <boost/regex.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace Slic3r {
namespace GUI {

namespace {

const FileGridCard& emptyCard()
{
    static const FileGridCard card;
    return card;
}

// Same display format as PrinterFileSystem: "1d2h3m", dropping a leading "0d"/"0h".
std::string durationString(long duration)
{
    static boost::regex rx("^0d(0h)?");
    auto time = boost::format("%1%d%2%h%3%m") % (duration / 86400) % ((duration % 86400) / 3600) % ((duration % 3600) / 60);
    return boost::regex_replace(time.str(), rx, "");
}

// Case-insensitive suffix test shared by the file filters below.
bool ends_with_ci(const std::string& name, const std::string& suffix)
{
    return name.size() >= suffix.size() &&
           std::equal(suffix.rbegin(), suffix.rend(), name.rbegin(),
                      [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

// .gcode / .3mf only, case-insensitive; a .gcode.3mf name matches via the .3mf suffix.
bool is_displayable_file(const std::string& name)
{
    return ends_with_ci(name, ".gcode") || ends_with_ci(name, ".3mf");
}

// Only .gcode is printable today; .gcode.3mf support to be added later.
bool is_printable_gcode(const std::string& name)
{
    return ends_with_ci(name, ".gcode");
}

}

RemoteFileGridModel::RemoteFileGridModel(std::string device_id)
    : m_device_id(std::move(device_id))
{
}

RemoteFileGridModel::~RemoteFileGridModel()
{
    m_lifetime.reset();
}

void RemoteFileGridModel::Refresh()
{
    const std::uint64_t request = ++m_request;
    const std::string device_id = m_device_id;
    m_thumbnail_cache.clear();
    m_thumbnail_requested.clear();
    m_thumbnail_queue.clear();
    m_metadata_cache.clear();
    m_metadata_requested.clear();
    m_metadata_queue.clear();
    for (FileGridCard& card : m_cards)
        card.thumbnail = wxBitmap();
    m_last_error = 0;
    setStatus(Status::Loading);

    NetworkAgent* agent = wxGetApp().getAgent();
    if (!agent) {
        m_last_error = -1;
        m_cards.clear();
        m_month.clear();
        m_year.clear();
        setStatus(Status::Failed);
        if (m_change_handler)
            m_change_handler(FileGridChange::Files);
        return;
    }

    std::weak_ptr<int> lifetime = m_lifetime;
    int result = agent->list_printer_files(device_id,
        [this, lifetime, request, device_id](int result, std::vector<PrinterFileEntry> files) {
            if (lifetime.expired())
                return;
            onFilesLoaded(request, device_id, result, std::move(files));
        });

    if (result != 0 && request == m_request && m_status == Status::Loading) {
        m_last_error = result;
        m_cards.clear();
        m_month.clear();
        m_year.clear();
        setStatus(Status::Failed);
        if (m_change_handler)
            m_change_handler(FileGridChange::Files);
    }
}

void RemoteFileGridModel::onFilesLoaded(std::uint64_t request, const std::string& device_id, int result,
                                        std::vector<PrinterFileEntry> files)
{
    if (request != m_request || device_id != m_device_id)
        return;

    m_last_error = result;
    m_cards.clear();
    if (result == 0) {
        files.erase(std::remove_if(files.begin(), files.end(),
                                   [](const PrinterFileEntry& file) { return !is_displayable_file(file.name); }),
                    files.end());
        std::sort(files.begin(), files.end(), [](const PrinterFileEntry& lhs, const PrinterFileEntry& rhs) {
            return lhs.modified > rhs.modified;
        });
        m_cards.reserve(files.size());
        for (const PrinterFileEntry& file : files) {
            FileGridCard card;
            card.id = file.path;
            card.name = file.name;
            card.time = static_cast<time_t>(file.modified);
            card.printable = is_printable_gcode(file.name);
            m_cards.emplace_back(std::move(card));
        }
        buildGroups();
        setStatus(m_cards.empty() ? Status::Empty : Status::Ready);
    } else {
        m_month.clear();
        m_year.clear();
        setStatus(Status::Failed);
    }

    if (m_change_handler)
        m_change_handler(FileGridChange::Files);
}

void RemoteFileGridModel::setStatus(Status status)
{
    m_status = status;
    if (m_status_handler)
        m_status_handler(status);
}

void RemoteFileGridModel::SetStatusHandler(std::function<void(Status)> handler)
{
    m_status_handler = std::move(handler);
}

size_t RemoteFileGridModel::GetCount() const
{
    if (m_group_mode == FileGridGroup::Year)
        return m_year.size();
    if (m_group_mode == FileGridGroup::Month)
        return m_month.size();
    return m_cards.size();
}

const FileGridCard& RemoteFileGridModel::GetFile(size_t index, bool& selected) const
{
    selected = false;
    const size_t card_index = cardIndexForGroup(index);
    if (card_index >= m_cards.size())
        return emptyCard();
    FileGridCard& card = m_cards[card_index];
    const auto thumbnail = m_thumbnail_cache.find(card.id);
    card.thumbnail = thumbnail == m_thumbnail_cache.end() ? wxBitmap() : thumbnail->second;
    return card;
}

const FileGridCard& RemoteFileGridModel::GetFile(size_t index) const
{
    bool selected = false;
    return GetFile(index, selected);
}

size_t RemoteFileGridModel::cardIndexForGroup(size_t index) const
{
    if (m_group_mode == FileGridGroup::All)
        return index;
    if (m_group_mode == FileGridGroup::Month)
        return index < m_month.size() ? m_month[index] : m_cards.size();
    return index < m_year.size() && m_year[index] < m_month.size() ? m_month[m_year[index]] : m_cards.size();
}

FileGridType RemoteFileGridModel::GetFileType() const
{
    return m_file_type;
}

void RemoteFileGridModel::SetFileType(FileGridType type, const std::string&)
{
    m_file_type = type;
}

FileGridGroup RemoteFileGridModel::GetGroupMode() const
{
    return m_group_mode;
}

void RemoteFileGridModel::SetGroupMode(FileGridGroup mode)
{
    if (mode != FileGridGroup::All && mode != FileGridGroup::Month && mode != FileGridGroup::Year)
        return;
    if (m_group_mode == mode)
        return;
    m_group_mode = mode;
    if (m_group_mode_handler)
        m_group_mode_handler(mode);
    if (m_change_handler)
        m_change_handler(FileGridChange::Mode);
}

size_t RemoteFileGridModel::GetIndexAtTime(time_t time) const
{
    if (m_cards.empty())
        return 0;

    size_t low = 0;
    size_t high = m_cards.size();
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (m_cards[middle].time >= time)
            low = middle + 1;
        else
            high = middle;
    }
    const size_t card_index = low == 0 ? 0 : low - 1;
    if (m_group_mode == FileGridGroup::All)
        return card_index;

    const auto month = std::upper_bound(m_month.begin(), m_month.end(), card_index);
    const size_t month_index = month == m_month.begin() ? 0 : static_cast<size_t>(month - m_month.begin() - 1);
    if (m_group_mode == FileGridGroup::Month)
        return month_index;

    const auto year = std::upper_bound(m_year.begin(), m_year.end(), card_index,
        [this](size_t file_index, size_t month_index) {
            return file_index < m_month[month_index];
        });
    return year == m_year.begin() ? 0 : static_cast<size_t>(year - m_year.begin() - 1);
}

size_t RemoteFileGridModel::EnterSubGroup(size_t index)
{
    if (m_group_mode == FileGridGroup::All)
        return index;
    if (m_group_mode == FileGridGroup::Year) {
        if (index >= m_year.size())
            return 0;
        index = m_year[index];
    } else {
        if (index >= m_month.size())
            return 0;
        index = m_month[index];
    }
    SetGroupMode(static_cast<FileGridGroup>(static_cast<int>(m_group_mode) - 1));
    return index;
}

void RemoteFileGridModel::ToggleSelect(size_t)
{
}

void RemoteFileGridModel::SelectAll(bool)
{
}

void RemoteFileGridModel::SetFocusRange(size_t start, size_t count)
{
    if (m_status == Status::Loading || start >= GetCount())
        return;
    const size_t end = start + std::min(count, GetCount() - start);
    for (size_t i = start; i < end; ++i) {
        const FileGridCard& card = GetFile(i);
        if (!card.id.empty()) {
            requestThumbnail(card.id);
            requestMetadata(card.id);
        }
    }
    pumpThumbnailQueue();
    pumpMetadataQueue();
}

void RemoteFileGridModel::Retry()
{
    Refresh();
}

int RemoteFileGridModel::GetLastError() const
{
    return m_last_error;
}

void RemoteFileGridModel::DownloadCheckFiles(const std::string&)
{
}

void RemoteFileGridModel::DeleteFile(const std::string& path, std::function<void(bool ok)> done)
{
    NetworkAgent* agent = wxGetApp().getAgent();
    if (!agent || m_device_id.empty() || path.empty()) {
        if (done)
            done(false);
        return;
    }

    const std::uint64_t request = m_request;
    const std::string device_id = m_device_id;
    const std::weak_ptr<int> lifetime = m_lifetime;
    const auto completed = std::make_shared<bool>(false);
    auto finish = [this, lifetime, completed, request, device_id, path, done = std::move(done)](int result) {
        if (*completed)
            return;
        *completed = true;
        if (lifetime.expired())
            return;

        const bool ok = result == 0;
        if (ok && request == m_request && device_id == m_device_id) {
            m_thumbnail_cache.erase(path);
            m_thumbnail_requested.erase(path);
            m_cards.erase(std::remove_if(m_cards.begin(), m_cards.end(),
                                         [&path](const FileGridCard& card) { return card.id == path; }),
                          m_cards.end());
            buildGroups();
            setStatus(m_cards.empty() ? Status::Empty : Status::Ready);
            if (m_change_handler)
                m_change_handler(FileGridChange::Files);
        }
        if (done)
            done(ok);
    };

    const int result = agent->delete_printer_file(device_id, path, finish);
    if (result != 0)
        finish(result);
}

void RemoteFileGridModel::SetChangeHandler(std::function<void(FileGridChange)> handler)
{
    m_change_handler = std::move(handler);
}

void RemoteFileGridModel::SetGroupModeHandler(std::function<void(FileGridGroup)> handler)
{
    m_group_mode_handler = std::move(handler);
}

void RemoteFileGridModel::buildGroups()
{
    m_month.clear();
    m_year.clear();
    if (m_cards.empty())
        return;

    m_month.push_back(0);
    m_year.push_back(0);
    wxDateTime previous(static_cast<time_t>(m_cards.front().time));
    for (size_t i = 1; i < m_cards.size(); ++i) {
        const wxDateTime current(static_cast<time_t>(m_cards[i].time));
        if (current.GetYear() != previous.GetYear()) {
            m_year.push_back(m_month.size());
            m_month.push_back(i);
        } else if (current.GetMonth() != previous.GetMonth()) {
            m_month.push_back(i);
        }
        previous = current;
    }
}

void RemoteFileGridModel::requestThumbnail(const std::string& path)
{
    if (m_thumbnail_cache.find(path) != m_thumbnail_cache.end() || !m_thumbnail_requested.insert(path).second)
        return;
    m_thumbnail_queue.push_back(path);
}

void RemoteFileGridModel::pumpThumbnailQueue()
{
    constexpr size_t MAX_THUMBNAIL_REQUESTS = 4;
    if (m_pumping_thumbnail_queue)
        return;
    m_pumping_thumbnail_queue = true;

    NetworkAgent* agent = wxGetApp().getAgent();
    if (!agent) {
        while (!m_thumbnail_queue.empty()) {
            m_thumbnail_cache.emplace(m_thumbnail_queue.front(), wxBitmap());
            m_thumbnail_queue.pop_front();
        }
        m_pumping_thumbnail_queue = false;
        return;
    }

    while (!m_thumbnail_queue.empty() && m_thumbnail_requests_in_flight < MAX_THUMBNAIL_REQUESTS) {
        std::string path = std::move(m_thumbnail_queue.front());
        m_thumbnail_queue.pop_front();
        ++m_thumbnail_requests_in_flight;

        const std::uint64_t request = m_request;
        const std::string device_id = m_device_id;
        const std::weak_ptr<int> lifetime = m_lifetime;
        const auto completed = std::make_shared<bool>(false);
        auto finish = [this, lifetime, completed, request, device_id, path](int result, std::string image) {
            if (*completed)
                return;
            *completed = true;
            if (lifetime.expired())
                return;
            onThumbnailLoaded(request, device_id, path, result, std::move(image));
        };

        const int result = agent->get_printer_file_thumbnail(device_id, path, finish);
        if (result != 0)
            finish(result, {});
    }
    m_pumping_thumbnail_queue = false;
}

void RemoteFileGridModel::onThumbnailLoaded(std::uint64_t request, const std::string& device_id,
                                            const std::string& path, int result, std::string image)
{
    if (m_thumbnail_requests_in_flight > 0)
        --m_thumbnail_requests_in_flight;

    if (request == m_request && device_id == m_device_id) {
        wxBitmap bitmap;
        if (result == 0 && !image.empty()) {
            wxMemoryInputStream stream(image.data(), image.size());
            wxImage decoded(stream, wxBITMAP_TYPE_PNG);
            if (decoded.IsOk())
                bitmap = wxBitmap(decoded);
        }
        m_thumbnail_cache[path] = std::move(bitmap);
        if (m_change_handler)
            m_change_handler(FileGridChange::Thumbnail);
    }

    pumpThumbnailQueue();
}

void RemoteFileGridModel::requestMetadata(const std::string& path)
{
    if (m_metadata_cache.find(path) != m_metadata_cache.end() || !m_metadata_requested.insert(path).second)
        return;
    m_metadata_queue.push_back(path);
}

void RemoteFileGridModel::pumpMetadataQueue()
{
    constexpr size_t MAX_METADATA_REQUESTS = 4;
    if (m_pumping_metadata_queue)
        return;
    m_pumping_metadata_queue = true;

    NetworkAgent* agent = wxGetApp().getAgent();
    if (!agent) {
        while (!m_metadata_queue.empty()) {
            m_metadata_cache.emplace(m_metadata_queue.front(), PrinterFileMetadata{});
            m_metadata_queue.pop_front();
        }
        m_pumping_metadata_queue = false;
        return;
    }

    while (!m_metadata_queue.empty() && m_metadata_requests_in_flight < MAX_METADATA_REQUESTS) {
        std::string path = std::move(m_metadata_queue.front());
        m_metadata_queue.pop_front();
        ++m_metadata_requests_in_flight;

        const std::uint64_t request = m_request;
        const std::string device_id = m_device_id;
        const std::weak_ptr<int> lifetime = m_lifetime;
        const auto completed = std::make_shared<bool>(false);
        auto finish = [this, lifetime, completed, request, device_id, path](int result, PrinterFileMetadata meta) {
            if (*completed)
                return;
            *completed = true;
            if (lifetime.expired())
                return;
            onMetadataLoaded(request, device_id, path, result, meta);
        };

        const int result = agent->get_printer_file_metadata(device_id, path, finish);
        if (result != 0)
            finish(result, {});
    }
    m_pumping_metadata_queue = false;
}

void RemoteFileGridModel::onMetadataLoaded(std::uint64_t request, const std::string& device_id,
                                           const std::string& path, int result, PrinterFileMetadata meta)
{
    if (m_metadata_requests_in_flight > 0)
        --m_metadata_requests_in_flight;

    if (request == m_request && device_id == m_device_id) {
        m_metadata_cache[path] = meta;
        if (result == 0) {
            for (FileGridCard& card : m_cards) {
                if (card.id != path)
                    continue;
                card.time_text   = meta.estimated_time > 0 ? durationString(meta.estimated_time) : std::string();
                card.weight_text = meta.filament_weight > 0
                                       ? std::to_string(int(std::round(meta.filament_weight))) + 'g'
                                       : std::string();
                break;
            }
            if (m_change_handler)
                m_change_handler(FileGridChange::Thumbnail);
        }
    }

    pumpMetadataQueue();
}

}}
