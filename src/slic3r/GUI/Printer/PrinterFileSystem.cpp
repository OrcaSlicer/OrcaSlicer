#include "PrinterFileSystem.h"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/I18N.hpp"

#include "../../Utils/NetworkAgent.hpp"
#include "../BitmapCache.hpp"

#include <boost/algorithm/hex.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/detail/md5.hpp>
#include <boost/regex.hpp>

#include <wx/mstream.h>

#include "nlohmann/json.hpp"

#include <cstring>

#ifndef NDEBUG
//#define PRINTER_FILE_SYSTEM_TEST
#endif

std::string last_system_error() {
    return Slic3r::decode_path(std::error_code(
#ifdef _WIN32
        GetLastError(), 
#else
        errno, 
#endif
        std::system_category()).message().c_str());
}

wxDEFINE_EVENT(EVT_STATUS_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_MODE_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_FILE_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_SELECT_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_THUMBNAIL, wxCommandEvent);
wxDEFINE_EVENT(EVT_DOWNLOAD, wxCommandEvent);

wxDEFINE_EVENT(EVT_FILE_CALLBACK, wxCommandEvent);

static wxBitmap default_thumbnail;

static std::map<int, std::string> error_messages = {
    {PrinterFileSystem::ERROR_PIPE, L("Reconnecting the printer, the operation cannot be completed immediately, please try again later.")},
    {PrinterFileSystem::ERROR_RES_BUSY, L("The device cannot handle more conversations. Please retry later.")},
    {PrinterFileSystem::FILE_NO_EXIST, L("File does not exist.")},
    {PrinterFileSystem::FILE_CHECK_ERR, L("File checksum error. Please retry.")},
    {PrinterFileSystem::FILE_TYPE_ERR, L("Not supported on the current printer version.")},
    {PrinterFileSystem::STORAGE_UNAVAILABLE, L("Storage unavailable, insert SD card.")}
};

struct StaticBambuLib : BambuLib {
    static StaticBambuLib & get();
    static int Fake_Bambu_Create(Bambu_Tunnel*, char const*) { return -2; }
};

PrinterFileSystem::PrinterFileSystem()
    : BambuLib(StaticBambuLib::get())
{
    if (!default_thumbnail.IsOk()) {
        default_thumbnail = *Slic3r::GUI::BitmapCache().load_svg("printer_file", 0, 0);
#ifdef __APPLE__
        default_thumbnail = wxBitmap(default_thumbnail.ConvertToImage(), -1, 1);
#endif
    }
    m_session.owner = this;
#ifdef PRINTER_FILE_SYSTEM_TEST
    auto time = wxDateTime::Now();
    wxString path = "D:\\work\\pic\\";
    for (int i = 0; i < 10; ++i) {
        auto name = wxString::Format(L"gcode-%02d.3mf", i + 1);
        m_file_list.push_back({name.ToUTF8().data(), "", time.GetTicks(), 26937, i < 5 ? FF_DOWNLOAD : 0, default_thumbnail});
        std::ifstream ifs((path + name).ToUTF8().data(), std::ios::binary);
        if (ifs)
            ParseThumbnail(m_file_list.back(), ifs);
        time.Add(wxDateSpan::Days(-1));
    }
    m_file_list.swap(m_file_list_cache[{F_MODEL, ""}]);
    time = wxDateTime::Now();
    for (int i = 0; i < 100; ++i) {
        auto name = wxString::Format(L"img-%03d.jpg", i + 1);
        wxImage im(path + name);
        m_file_list.push_back({name.ToUTF8().data(), "", time.GetTicks(), 26937, i < 20 ? FF_DOWNLOAD : 0, i > 3 ? im : default_thumbnail});
        time.Add(wxDateSpan::Days(-1));
    }
    m_file_list[0].thumbnail = default_thumbnail;
    m_file_list.swap(m_file_list_cache[{F_TIMELAPSE, ""}]);
#endif
}

PrinterFileSystem::~PrinterFileSystem()
{
    m_recv_thread.detach();
}

void PrinterFileSystem::SetFileType(FileType type, std::string const &storage)
{
    if (m_file_type == type && m_file_storage == storage)
        return;
    SelectAll(false);
    assert(m_file_list_cache[std::make_pair(m_file_type, m_file_storage)].empty());
    m_file_list.swap(m_file_list_cache[{m_file_type, m_file_storage}]);
    std::swap(m_file_type, type);
    m_file_storage = storage;
    m_file_list.swap(m_file_list_cache[{m_file_type, m_file_storage}]);
    m_lock_start = m_lock_end = 0;
    BuildGroups();
    UpdateGroupSelect();
    SendChangedEvent(EVT_FILE_CHANGED);
    if (type == F_INVALID_TYPE)
        return;
    if (m_session.tunnel == nullptr)
        return;
    m_status = Status::ListSyncing;
    SendChangedEvent(EVT_STATUS_CHANGED, m_status);
    ListAllFiles();
}

void PrinterFileSystem::SetGroupMode(GroupMode mode)
{
    if (this->m_group_mode == mode)
        return;
    this->m_group_mode = mode;
    m_lock_start = m_lock_end = 0;
    UpdateGroupSelect();
    SendChangedEvent(EVT_MODE_CHANGED);
}

size_t PrinterFileSystem::EnterSubGroup(size_t index)
{
    if (m_group_mode == G_NONE)
        return index;
    index = m_group_mode == G_YEAR ? m_group_year[index] : m_group_month[index];
    SetGroupMode((GroupMode)(m_group_mode - 1));
    return index;
}

void PrinterFileSystem::ListAllFiles()
{
    json req;
    char const * types[] {"timelapse","video", "model" };
    req["type"] = types[m_file_type];
    if (!m_file_storage.empty())
        req["storage"] = m_file_storage;
    req["api_version"] = 2;
    req["notify"] = "DETAIL";
    SendRequest<FileList>(LIST_INFO, req, [this, type = m_file_type](json const& resp, FileList & list, auto) -> int {
        json files = resp["file_lists"];
        for (auto& f : files) {
            std::string     name = f["name"];
            std::string     path = f.value("path", "");
            time_t          time = f.value("time", 0);
            boost::uint64_t size = f["size"];
            if (type > F_TIMELAPSE && path.empty()) // Fix old printer that always return timelapses
                return FILE_TYPE_ERR;
            File            ff   = {name, path, time, size, 0};
            list.push_back(ff);
        }
        return 0;
    }, [this, type = m_file_type](int result, FileList list) {
        if (result != 0) {
            m_last_error = result;
            m_status = Status::Failed;
            m_file_list.clear();
            BuildGroups();
            UpdateGroupSelect();
            SendChangedEvent(EVT_STATUS_CHANGED, m_status, "", result);
            SendChangedEvent(EVT_FILE_CHANGED);
            return 0;
        }
        if (type != m_file_type)
            return 0;
        m_file_list.swap(list);
        for (auto & file : m_file_list)
            file.thumbnail = default_thumbnail;
        std::sort(m_file_list.begin(), m_file_list.end());
        auto iter1 = m_file_list.begin();
        auto end1  = m_file_list.end();
        auto iter2 = list.begin();
        auto end2  = list.end();
        while (iter1 != end1 && iter2 != end2) {
            if (*iter1 < *iter2) {
                ++iter1;
            } else if (*iter2 < *iter1) {
                ++iter2;
            } else {
                if (iter1->path == iter2->path && iter1->name == iter2->name) {
                    iter1->thumbnail = iter2->thumbnail;
                    iter1->flags     = iter2->flags;
                    if (!iter1->thumbnail.IsOk())
                        iter1->flags &= ~FF_THUMNAIL;
                    iter1->download   = iter2->download;
                    iter1->local_path = iter2->local_path;
                    iter1->metadata   = iter2->metadata;
                }
                ++iter1;
                ++iter2;
            }
        }
        BuildGroups();
        UpdateGroupSelect();
        m_last_error = 0;
        m_status = Status::ListReady;
        SendChangedEvent(EVT_STATUS_CHANGED, m_status);
        SendChangedEvent(EVT_FILE_CHANGED);
        if ((m_task_flags & FF_DOWNLOAD) == 0)
            DownloadNextFile();
        return 0;
    });
}

void PrinterFileSystem::DeleteFiles(size_t index)
{
    if (index == size_t(-1)) {
        size_t n = 0;
        for (size_t i = 0; i < m_file_list.size(); ++i) {
            auto &file = m_file_list[i];
            if ((file.flags & FF_SELECT) != 0 && (file.flags & FF_DELETED) == 0) {
                file.flags |= FF_DELETED;
                ++n;
            }
        }
        if (n == 0) return;
    } else {
        if (index >= m_file_list.size())
            return;
        auto &file = m_file_list[index];
        if ((file.flags & FF_DELETED) != 0)
            return;
        file.flags |= FF_DELETED;
    }
    if ((m_task_flags & FF_DELETED) == 0)
        DeleteFilesContinue();
}

struct PrinterFileSystem::Download : Progress
{
    size_t                      index;
    std::string                 name;
    std::string                 path;
    std::string                 local_path;
    std::string                 error;
    boost::filesystem::ofstream ofs;
    boost::uuids::detail::md5   boost_md5;
};

void PrinterFileSystem::DownloadFiles(size_t index, std::string const &path)
{
    if (index == (size_t) -1) {
        size_t n = 0;
        for (size_t i = 0; i < m_file_list.size(); ++i) {
            auto &file = m_file_list[i];
            if ((file.flags & FF_SELECT) == 0) continue;
            if ((file.flags & FF_DOWNLOAD) != 0 && file.DownloadProgress() >= -1) continue;
            file.flags |= FF_DOWNLOAD;
            std::shared_ptr<Download> download(new Download);
            download->progress = -1;
            download->local_path = (boost::filesystem::path(path) / file.name).string();
            file.download = download;
            ++n;
        }
        if (n == 0) return;
    } else {
        if (index >= m_file_list.size())
            return;
        auto &file = m_file_list[index];
        if ((file.flags & FF_DOWNLOAD) != 0 && file.DownloadProgress() >= -1)
            return;
        file.flags |= FF_DOWNLOAD;
        std::shared_ptr<Download> download(new Download);
        download->progress   = -1;
        download->local_path = (boost::filesystem::path(path) / file.name).string();
        file.download        = download;
    }
    boost::filesystem::create_directories(path);
    if ((m_task_flags & FF_DOWNLOAD) == 0)
        DownloadNextFile();
}

void PrinterFileSystem::DownloadCheckFiles(std::string const &path)
{
    for (size_t i = 0; i < m_file_list.size(); ++i) {
        auto &file = m_file_list[i];
        if ((file.flags & FF_DOWNLOAD) != 0 && file.download) continue;
        auto path2 = boost::filesystem::path(path) / file.name;
        boost::system::error_code ec;
        if (boost::filesystem::file_size(path2, ec) == file.size) {
            file.flags |= FF_DOWNLOAD;
            file.local_path = path2.string();
        }
    }
}

bool PrinterFileSystem::DownloadCheckFile(size_t index)
{
    if (index >= m_file_list.size()) return false;
    auto &file = m_file_list[index];
    if ((file.flags & FF_DOWNLOAD) == 0 || file.local_path.empty())
        return false;
    if (!boost::filesystem::exists(file.local_path)) {
        file.flags &= ~FF_DOWNLOAD;
        file.local_path.clear();
        SendChangedEvent(EVT_DOWNLOAD, index, file.local_path);
        return false;
    }
    return true;
}

void PrinterFileSystem::DownloadCancel(size_t index)
{
    if (index == (size_t) -1) return;
    if (index >= m_file_list.size()) return;
    auto &file = m_file_list[index];
    if ((file.flags & FF_DOWNLOAD) == 0 || !file.download) return;
    if (file.DownloadProgress() >= 0)
        CancelRequest(m_download_seq);
    else
        file.flags &= ~FF_DOWNLOAD, file.download.reset();
}

void PrinterFileSystem::FetchModel(size_t index, std::function<void(int, std::string const &)> callback)
{
    if (m_task_flags & FF_FETCH_MODEL)
        return;
    json req;
    json arr;
    if (index == (size_t) -1) return;
    if (index >= m_file_list.size()) return;
    auto &file = m_file_list[index];
    arr.push_back(file.path + "#_rels/.rels");
    arr.push_back(file.path + "#3D/3dmodel.model");
    arr.push_back(file.path + "#Metadata/model_settings.config");
    arr.push_back(file.path + "#Metadata/slice_info.config");
    arr.push_back(file.path + "#Metadata/project_settings.config");
    for (auto & meta : file.metadata) {
        if (boost::algorithm::starts_with(meta.first, "plate_thumbnail_"))
            arr.push_back(file.path + "#" + meta.second);
    }
    req["paths"] = arr;
    req["zip"] = true;
    m_task_flags |= FF_FETCH_MODEL;
    std::shared_ptr<std::string> file_data(new std::string());
    m_fetch_model_seq = SendRequest<Void>(
        SUB_FILE, req,
        [file_data](json const &resp, Void &, unsigned char const *data) -> int {
            // in work thread, continue recv
            // receive data
            boost::uint32_t size      = resp["size"];
            if (size > 0) {
                *file_data += std::string((char *) data, size);
            }
            return 0;
        },
        [this, file_data, callback](int result, Void const &) {
            if (result == CONTINUE) return;
            m_task_flags &= ~FF_FETCH_MODEL;
            if (result != 0) {
                auto iter = error_messages.find(result);
                if (iter != error_messages.end())     
                    *file_data = _u8L(iter->second.c_str());
                else
                    file_data->clear();
            }
            callback(result, *file_data);
        });
}

void PrinterFileSystem::FetchModelCancel()
{
    if ((m_task_flags & FF_FETCH_MODEL) == 0) return;
    CancelRequests2({m_fetch_model_seq});
}

size_t PrinterFileSystem::GetCount() const
{
    if (m_group_mode == G_NONE)
        return m_file_list.size();
    return m_group_mode == G_YEAR ? m_group_year.size() : m_group_month.size();
}

int PrinterFileSystem::File::DownloadProgress() const { return download ? download->progress : !local_path.empty() ? 100 : -2; }

std::string PrinterFileSystem::File::Title() const { return Metadata("Title", ""); }

std::string PrinterFileSystem::File::Metadata(std::string const &key, std::string const &dflt) const
{
    auto iter = metadata.find(key);
    return iter == metadata.end() || iter->second.empty() ? dflt : iter->second;
}

size_t PrinterFileSystem::GetIndexAtTime(boost::uint32_t time)
{
    auto   iter = std::upper_bound(m_file_list.begin(), m_file_list.end(), File{"", "", time});
    size_t n = std::distance(m_file_list.begin(), iter) - 1;
    if (m_group_mode == G_NONE) {
        return n;
    }
    auto & group = m_group_mode == G_YEAR ? m_group_year : m_group_month;
    auto iter2 = std::upper_bound(group.begin(), group.end(), n);
    return std::distance(group.begin(), iter2) - 1;
}

void PrinterFileSystem::ToggleSelect(size_t index)
{
    if (m_group_mode != G_NONE) {
        size_t beg = m_group_mode == G_YEAR ? m_group_month[m_group_year[index]] : m_group_month[index];
        size_t end_month = m_group_mode == G_YEAR ? ((index + 1) < m_group_year.size() ? m_group_year[index + 1] : m_group_month.size()) : index + 1;
        size_t end       = end_month < m_group_month.size() ? m_group_month[end_month] : m_file_list.size();
        if ((m_group_flags[index] & FF_SELECT) == 0) {
            for (int i = beg; i < end; ++i) {
                if ((m_file_list[i].flags & FF_SELECT) == 0) {
                    m_file_list[i].flags |= FF_SELECT;
                    ++m_select_count;
                }
            }
            m_group_flags[index] |= FF_SELECT;
        } else {
            for (int i = beg; i < end; ++i) {
                if (m_file_list[i].flags & FF_SELECT) {
                    m_file_list[i].flags &= ~FF_SELECT;
                    --m_select_count;
                }
            }
            m_group_flags[index] &= ~FF_SELECT;
        }
    } else if (index < m_file_list.size()) {
        m_file_list[index].flags ^= FF_SELECT;
        if (m_file_list[index].flags & FF_SELECT)
            ++m_select_count;
        else
            --m_select_count;
    }
    SendChangedEvent(EVT_SELECT_CHANGED, m_select_count);
}

void PrinterFileSystem::SelectAll(bool select)
{
    if (select) {
        for (auto &f : m_file_list) f.flags |= FF_SELECT;
        m_select_count = m_file_list.size();
        for (auto &s : m_group_flags) s |= FF_SELECT;
    } else {
        for (auto &f : m_file_list) f.flags &= ~FF_SELECT;
        m_select_count = 0;
        for (auto &s : m_group_flags) s &= ~FF_SELECT;
    }
    SendChangedEvent(EVT_SELECT_CHANGED, m_select_count);
}

size_t PrinterFileSystem::GetSelectCount() const { return m_select_count; }

void PrinterFileSystem::SetFocusRange(size_t start, size_t count)
{
    m_lock_start = start;
    m_lock_end = start + count;
    if (!m_stopped && (m_task_flags & FF_THUMNAIL) == 0)
        UpdateFocusThumbnail();
}

PrinterFileSystem::File const &PrinterFileSystem::GetFile(size_t index)
{
    if (m_group_mode == G_NONE)
        return m_file_list[index];
    if (m_group_mode == G_YEAR) index = m_group_year[index];
    return m_file_list[m_group_month[index]];
}

PrinterFileSystem::File const &PrinterFileSystem::GetFile(size_t index, bool &select)
{
    if (m_group_mode == G_NONE) {
        select = m_file_list[index].IsSelect();
        return m_file_list[index];
    }
    select = m_group_flags[index] & FF_SELECT;
    if (m_group_mode == G_YEAR)
        index = m_group_year[index];
    return m_file_list[m_group_month[index]];
}

void PrinterFileSystem::Attached()
{
    boost::unique_lock lock(m_mutex);
    m_recv_thread = std::move(boost::thread([w = weak_from_this()] {
        boost::shared_ptr<PrinterFileSystem> s = w.lock();
        if (s) s->RecvMessageThread();
    }));
}

void PrinterFileSystem::Start()
{
    boost::unique_lock l(m_mutex);
    if (!m_stopped) return;
    m_stopped = false;
    m_cond.notify_all();
}

void PrinterFileSystem::Retry()
{
    boost::unique_lock l(m_mutex);
    m_stopped = false;
    m_cond.notify_all();
}

void PrinterFileSystem::SetUrl(std::string const &url)
{
    boost::unique_lock l(m_mutex);
    m_messages.push_back(url);
    m_cond.notify_all();
}

void PrinterFileSystem::Stop(bool quit)
{
    boost::unique_lock l(m_mutex);
    if (quit) {
        m_session.owner = nullptr;
    } else if (m_stopped) {
        return;
    }
    m_stopped = true;
    m_cond.notify_all();
}

void PrinterFileSystem::BuildGroups()
{
    m_group_year.clear();
    m_group_month.clear();
    if (m_file_list.empty())
        return;
    wxDateTime t = wxDateTime((time_t) m_file_list.front().time);
    m_group_year.push_back(0);
    m_group_month.push_back(0);
    for (size_t i = 0; i < m_file_list.size(); ++i) {
        wxDateTime s = wxDateTime((time_t) m_file_list[i].time);
        if (s.GetYear() != t.GetYear()) {
            m_group_year.push_back(m_group_month.size());
            m_group_month.push_back(i);
        } else if (s.GetMonth() != t.GetMonth()) {
            m_group_month.push_back(i);
        }
        t = s;
    }
}

void PrinterFileSystem::UpdateGroupSelect()
{
    m_group_flags.clear();
    int beg = 0;
    if (m_group_mode != G_NONE) {
        auto group = m_group_mode == G_YEAR ? m_group_year : m_group_month;
        if (m_group_mode == G_YEAR)
            for (auto &g : group) g = m_group_month[g];
        m_group_flags.resize(group.size(), FF_SELECT);
        for (int i = 0; i < m_file_list.size(); ++i) {
            if ((m_file_list[i].flags & FF_SELECT) == 0) {
                auto iter = std::upper_bound(group.begin(), group.end(), i);
                m_group_flags[iter - group.begin() - 1] &= ~FF_SELECT;
                if (iter == group.end()) break;
                i = *iter - 1; // start from next group
            }
        }
    }
}

void PrinterFileSystem::DeleteFilesContinue()
{
    std::vector<size_t> indexes;
    std::vector<std::string> names;
    std::vector<std::string> paths;
    for (size_t i = 0; i < m_file_list.size(); ++i)
        if ((m_file_list[i].flags & FF_DELETED) && !m_file_list[i].name.empty()) {
            indexes.push_back(i);
            auto &file = m_file_list[i];
            if (file.path.empty())
                names.push_back(file.name);
            else
                paths.push_back(file.path);
            if (names.size() >= 64 || paths.size() >= 64)
                break;
        }
    m_task_flags &= ~FF_DELETED;
    if (names.empty() && paths.empty())
        return;
    json req;
    json arr;
    if (paths.empty()) {
        for (auto &name : names) arr.push_back(name);
        req["delete"] = arr;
    } else {
        for (auto &path : paths) arr.push_back(path);
        req["paths"] = arr;
    }
    m_task_flags |= FF_DELETED;
    auto type = std::make_pair(m_file_type, m_file_storage);
    SendRequest<Void>(
        FILE_DEL, req, nullptr,
        [indexes, type, names = paths.empty() ? names : paths, bypath = !paths.empty(), this](int, Void const &) {
            // TODO:
            for (size_t i = indexes.size() - 1; i != size_t(-1); --i)
                FileRemoved(type, indexes[i], names[i], bypath);
            SendChangedEvent(EVT_FILE_CHANGED, indexes.size());
            DeleteFilesContinue();
        });
}

void PrinterFileSystem::DownloadNextFile()
{
    size_t index = size_t(-1);
    for (size_t i = 0; i < m_file_list.size(); ++i) {
        if (m_file_list[i].IsDownload() && m_file_list[i].DownloadProgress() == -1) {
            index = i;
            break;
        }
    }
    m_task_flags &= ~FF_DOWNLOAD;
    if (index >= m_file_list.size())
        return;
    auto &file = m_file_list[index];
    json req;
    if (file.path.empty())
        req["file"] = file.name;
    else
        req["path"] = file.path;
    SendChangedEvent(EVT_DOWNLOAD, index, m_file_list[index].name);
    std::shared_ptr<Download> download(m_file_list[index].download);
    download->index = index;
    download->name = file.name;
    download->path = file.path;
    m_task_flags |= FF_DOWNLOAD;
    m_download_seq = SendRequest<Progress>(
        FILE_DOWNLOAD, req,
        [download](json const &resp, Progress &prog, unsigned char const *data) -> int {
            // in work thread, continue recv
            size_t size = resp.value("size", 0);
            prog.size   = resp["offset"];
            prog.total  = resp["total"];
            if (prog.size == 0) {
                download->ofs.open(download->local_path, std::ios::binary);
                if (!download->ofs) {
                    download->error = last_system_error();
                    wxLogWarning("PrinterFileSystem::DownloadNextFile open error: %s\n", wxString::FromUTF8(download->error));
                    return FILE_OPEN_ERR;
                }
            }
            if (download->total && (download->size != prog.size || download->total != prog.total)) {
                wxLogWarning("PrinterFileSystem::DownloadNextFile data error: %d != %d\n", download->size, prog.size);
            }
            // receive data
            download->ofs.write((char const *) data, size);
            if (!download->ofs) {
                download->error = last_system_error();
                wxLogWarning("PrinterFileSystem::DownloadNextFile write error: %s\n", wxString::FromUTF8(download->error));
                return FILE_READ_WRITE_ERR;
            }
            download->boost_md5.process_bytes(data, size);
            prog.size += size;
            download->total = prog.total;
            download->size = prog.size;
            if (prog.size < prog.total) { return 0; }
            download->ofs.close();
            int         result = 0;
            std::string md5    = resp["file_md5"];
            // check size and md5
            if (prog.size == prog.total) {
                boost::uuids::detail::md5::digest_type digest;
                download->boost_md5.get_digest(digest);
                for (int i = 0; i < 4; ++i) digest[i] = boost::endian::endian_reverse(digest[i]);
                std::string str_md5;
                const auto  char_digest = reinterpret_cast<const char *>(&digest[0]);
                boost::algorithm::hex(char_digest, char_digest + sizeof(digest), std::back_inserter(str_md5));
                if (!boost::iequals(str_md5, md5)) {
                    wxLogWarning("PrinterFileSystem::DownloadNextFile checksum error: %s != %s\n", str_md5, md5);
                    boost::system::error_code ec;
                    boost::filesystem::rename(download->local_path, download->local_path + ".tmp", ec);
                    result = FILE_CHECK_ERR;
                }
            } else {
                result = FILE_SIZE_ERR;
            }
            if (result != 0) {
                boost::system::error_code ec;
                boost::filesystem::remove(download->local_path, ec);
            }
            return result;
        },
        [this, download, type = std::make_pair(m_file_type, m_file_storage)](int result, Progress const &data) {
            int progress = data.total ? data.size * 100 / data.total : 0;
            if (result == CONTINUE) {
                if (download->progress == progress)
                    return;
            }
            download->progress = progress;
            if (download->index != size_t(-1)) {
                auto file_index = FindFile(type, download->index, download->path.empty() ? download->name : download->path, !download->path.empty());
                download->index = file_index.second;
                if (download->index != size_t(-1)) {
                    auto &file = file_index.first[download->index];
                    if (result == CONTINUE)
                        ;
                    else if (result == SUCCESS)
                        file.download.reset(), file.local_path = download->local_path;
                    else if (result == ERROR_CANCEL)
                        file.download.reset(), file.flags &= ~FF_DOWNLOAD;
                    else // FAILED
                        file.download.reset();
                    if (&file_index.first == &m_file_list)
                        SendChangedEvent(EVT_DOWNLOAD, download->index, result ? download->error : file.local_path, result);
                }
            }
            if (result != CONTINUE) DownloadNextFile();
        });
}

enum ThumbnailType
{
    OldThumbnail = 0,
    VideoThumbnail = 1,
    ModelMetadata = 2,
    ModelThumbnail = 3,
    FinishThumbnail
};

void PrinterFileSystem::UpdateFocusThumbnail()
{
    m_task_flags &= ~FF_THUMNAIL;
    if (m_lock_start >= m_file_list.size() || m_lock_start >= m_lock_end)
        return;
    size_t start = m_lock_start;
    size_t end   = std::min(m_lock_end, GetCount());
    std::vector<File> names;
    std::vector<File> paths;
    for (; start < end; ++start) {
        auto &file = GetFile(start);
        if ((file.flags & FF_THUMNAIL) == 0) {
            if (m_file_type == F_MODEL) {
                const_cast<File &>(file).metadata.emplace("Time", "...");
                const_cast<File &>(file).metadata.emplace("Weight", "...");
            }
            if (file.path.empty())
                names.push_back({file.name, ""});
            else
                paths.push_back({file.name, file.path});
            if (names.size() >= 5 || paths.size() >= 5)
                break;
            if ((file.flags & FF_THUMNAIL_RETRY) != 0) {
                const_cast<File&>(file).flags &= ~FF_THUMNAIL_RETRY;
                break;
            }
        }
    }
    if (names.empty() && paths.empty())
        return;
    m_task_flags |= FF_THUMNAIL;
    UpdateFocusThumbnail2(std::make_shared<std::vector<File>>(paths.empty() ? names : paths), 
        paths.empty() ? OldThumbnail : m_file_type == F_MODEL ? ModelMetadata : VideoThumbnail);
}

bool PrinterFileSystem::ParseThumbnail(File &file)
{
    std::istringstream iss(file.local_path, std::ios::binary);
    return ParseThumbnail(file, iss);
}

static std::string durationString(long duration)
{
    static boost::regex rx("^0d(0h)?");
    auto time = boost::format("%1%d%2%h%3%m") % (duration / 86400) % ((duration % 86400) / 3600) % ((duration % 3600) / 60);
    return boost::regex_replace(time.str(), rx, "");
}

bool PrinterFileSystem::ParseThumbnail(File &file, std::istream &is)
{
    Slic3r::DynamicPrintConfig config;
    Slic3r::Model              model;
    Slic3r::PlateDataPtrs      plate_data_list;
    Slic3r::Semver file_version;
    if (!Slic3r::load_gcode_3mf_from_stream(is, &config, &model, &plate_data_list, &file_version))
        return false;
    float time      = 0.f;
    float weight    = 0.f;
    for (auto &plate : plate_data_list) {
        time += atof(plate->gcode_prediction.c_str());
        weight += atof(plate->gcode_weight.c_str());
        if (!plate->gcode_file.empty() && !plate->thumbnail_file.empty())
            file.metadata.emplace("plate_thumbnail_" + std::to_string(plate->plate_index), plate->thumbnail_file);
    }
    file.metadata.emplace("Title", model.model_info->model_name);
    file.metadata.emplace("Time", durationString(round(time)));
    file.metadata.emplace("Weight", std::to_string(int(round(weight))) + 'g');
    auto thumbnail = model.model_info->metadata_items["Thumbnail"];
    if (thumbnail.empty() && !plate_data_list.empty()) {
        thumbnail = plate_data_list.front()->thumbnail_file;
    }
    file.metadata.emplace("Thumbnail", thumbnail);
    return true;
}

void PrinterFileSystem::UpdateFocusThumbnail2(std::shared_ptr<std::vector<File>> files, int type)
{
    json req;
    json arr;
    if (type == OldThumbnail) {
        for (auto &file : *files) arr.push_back(file.name);
        req["files"] = arr;
    } else {
        if (type == VideoThumbnail) {
            for (auto &file : *files) arr.push_back(file.path + "#thumbnail");
        } else if (type == ModelMetadata) {
            for (auto &file : *files) {
                arr.push_back(file.path + "#_rels/.rels");
                arr.push_back(file.path + "#3D/3dmodel.model");
                arr.push_back(file.path + "#Metadata/model_settings.config");
                arr.push_back(file.path + "#Metadata/slice_info.config");
                arr.push_back(file.path + "#Metadata/project_settings.config");
            }
            req["zip"] = true;
        } else { // ModelThumbnail, FinishThumbnail
            std::vector<std::string> fails;
            for (auto &file : *files) {
                if ((file.flags & FF_THUMNAIL) == 0) {
                    fails.push_back(file.path);
                    file.flags |= FF_THUMNAIL;
                }
            }
            for (auto &path : fails) {
                auto iter = std::find_if(m_file_list.begin(), m_file_list.end(), [&path](auto &f) { return f.path == path; });
                if (iter != m_file_list.end()) {
                    if (type == ModelThumbnail) iter->metadata.clear();
                    iter->flags |= fails.size() == 1 ? FF_THUMNAIL : FF_THUMNAIL_RETRY;
                }
            }
            if (type == ModelThumbnail) {
                for (auto &file : *files) {
                    auto thumbnail = file.metadata["Thumbnail"];
                    if (!thumbnail.empty()) {
                        arr.push_back(file.path + "#" + thumbnail);
                        file.flags &= ~FF_THUMNAIL;
                        file.local_path.clear();    
                    }
                }
            }
            if (arr.empty()) {
                UpdateFocusThumbnail();
                return;
            }
        }
        req["paths"] = arr;
    }
    SendRequest<File>(
        SUB_FILE, req, [type, files](json const &resp, File &file, unsigned char const *data) -> int {
            // in work thread, continue recv
            // receive data
            wxString        mimetype  = resp.value("mimetype", "");
            std::string     thumbnail = resp.value("thumbnail", "");
            std::string     path      = resp.value("path", "");
            boost::uint32_t size      = resp.value("size", 0);
            bool            cont      = resp.value("continue", false);
            if (size == 0) {
                file.name = thumbnail;
                file.path = path;
                return FILE_SIZE_ERR;
            }
            auto n = type == ModelMetadata ? std::string::npos : path.find_last_of('#'); // ModelMetadata is zipped without subpath
            auto path2 = n == std::string::npos ? path : path.substr(0, n);
            auto subpath = n == std::string::npos ? path : path.substr(n + 1);
            auto iter = std::find_if(files->begin(), files->end(), [&path2](auto &f) { return f.path == path2; });
            if (cont) {
                if (iter != files->end())
                    iter->local_path += std::string((char *) data, size);
                return 0;
            }
            if (type == ModelMetadata) {
                if (iter != files->end())
                    file.local_path = iter->local_path + std::string((char *) data, size);
                else
                    file.local_path = std::string((char *) data, size);
                ParseThumbnail(file);
            } else {
                if (mimetype.empty()) {
                    if (subpath.empty()) subpath = thumbnail;
                    auto n = subpath.find_last_of('.');
                    if (n != std::string::npos)
                        mimetype = "image/" + subpath.substr(n + 1);
                    else if (subpath == "thumbnail")
                        mimetype = "image/jpeg"; // default jpg
                }
                if (iter != files->end() && !iter->local_path.empty()) {
                    iter->local_path += std::string((char *) data, size);
                    data = reinterpret_cast<unsigned char const *>(iter->local_path.c_str());
                    size = iter->local_path.size();
                }
                wxMemoryInputStream mis(data, size);
                mimetype.Replace("jpg", "jpeg");
                file.thumbnail = wxImage(mis, mimetype);
                if (!file.thumbnail.IsOk()) {
                    BOOST_LOG_TRIVIAL(info) << "PrinterFileSystem: parse thumbnail failed" << size << mimetype;
                }
            }
            file.name = thumbnail;
            file.path = path;
            return 0;
        },
        [this, files, type](int result, File const &file) {
            auto n    = file.name.find_last_of('.');
            auto name  = n == std::string::npos ? file.name : file.name.substr(0, n) + ".mp4";
            n          = (type == ModelMetadata) ? std::string::npos : file.path.find_last_of('#');
            auto path = n == std::string::npos ? file.path : file.path.substr(0, n);
            auto iter = path.empty() ? std::find_if(m_file_list.begin(), m_file_list.end(), [&name](auto &f) { return f.name == name; }) :
                                       std::find_if(m_file_list.begin(), m_file_list.end(), [&path](auto &f) { return f.path == path; });
            auto iter2 = path.empty() ? std::find_if(files->begin(), files->end(), [&name](auto &f) { return f.name == name; }) :
                                        std::find_if(files->begin(), files->end(), [&path](auto &f) { return f.path == path; });
            if (iter != m_file_list.end()) {
                if (type == ModelMetadata) {
                    iter->metadata = file.metadata;
                    auto thumbnail = iter->metadata["Thumbnail"];
                    if (thumbnail.empty())
                        iter->flags |= FF_THUMNAIL; // DOTO: retry on fail
                    int index       = iter - m_file_list.begin();
                    SendChangedEvent(EVT_THUMBNAIL, index, file.name);
                    if (iter2 != files->end())
                        iter2->metadata = file.metadata;
                } else {
                    iter->flags |= FF_THUMNAIL; // DOTO: retry on fail
                    if (file.thumbnail.IsOk()) {
                        iter->thumbnail = file.thumbnail;
                        int index       = iter - m_file_list.begin();
                        SendChangedEvent(EVT_THUMBNAIL, index, file.name);
                    }
                }
            }
            if (iter2 != files->end())
                iter2->flags |= FF_THUMNAIL; // have received response
            if (result != CONTINUE)
                UpdateFocusThumbnail2(files, type == ModelMetadata ? ModelThumbnail : FinishThumbnail);
        });
}

std::pair<PrinterFileSystem::FileList &, size_t> PrinterFileSystem::FindFile(std::pair<FileType, std::string> type, size_t index, std::string const &name, bool by_path)
{
    FileList & file_list = type == std::make_pair(m_file_type, m_file_storage) ?
                               m_file_list :
                               m_file_list_cache[type];
    if (index >= file_list.size() || (by_path ? file_list[index].path : file_list[index].name) != name) {
        auto iter = std::find_if(m_file_list.begin(), file_list.end(), 
                [name, by_path](File &f) { return (by_path ? f.path : f.name) == name; });
        if (iter == m_file_list.end()) return {file_list, -1};
        index = std::distance(m_file_list.begin(), iter);
    }
    return {file_list, index};
}

void PrinterFileSystem::FileRemoved(std::pair<FileType, std::string> type, size_t index, std::string const &name, bool by_path)
{
    auto file_index = FindFile(type, index, name, by_path);
    if (file_index.second == size_t(-1))
        return;
    if (&file_index.first == &m_file_list) {
        auto removeFromGroup = [](std::vector<size_t> &group, size_t index, size_t total) {
            for (auto iter = group.begin(); iter != group.end(); ++iter) {
                size_t index2 = -1;
                if (*iter < index) continue;
                if (*iter == index) {
                    auto iter2 = iter + 1;
                    if (index + 1 == (iter2 == group.end() ? total : *iter2)) {
                        index2 = std::distance(group.begin(), iter);
                    }
                    ++iter;
                }
                for (; iter != group.end(); ++iter) {
                    --*iter;
                }
                return index2;
            }
            return size_t(-1);
        };
        size_t index2 = removeFromGroup(m_group_month, index, m_file_list.size());
        if (index2 < m_group_month.size()) {
            int index3 = removeFromGroup(m_group_year, index2, m_group_month.size());
            if (index3 < m_group_year.size()) {
                m_group_year.erase(m_group_year.begin() + index3);
                if (m_group_mode == G_YEAR)
                    m_group_flags.erase(m_group_flags.begin() + index3);
            }
            m_group_month.erase(m_group_month.begin() + index2);
            if (m_group_mode == G_MONTH)
                m_group_flags.erase(m_group_flags.begin() + index2);
        }
    }
    file_index.first.erase(file_index.first.begin() + index);
}

struct CallbackEvent : wxCommandEvent
{
    CallbackEvent(std::function<void(void)> const &callback, boost::weak_ptr<PrinterFileSystem> owner) : wxCommandEvent(EVT_FILE_CALLBACK), callback(callback), owner(owner) {}
    ~CallbackEvent(){ if (!owner.expired()) callback(); }
    std::function<void(void)> const callback;
    boost::weak_ptr<PrinterFileSystem> owner;
};

void PrinterFileSystem::PostCallback(std::function<void(void)> const& callback)
{
    wxCommandEvent *e = new CallbackEvent(callback, boost::weak_ptr(shared_from_this()));
    wxQueueEvent(this, e);
}

void PrinterFileSystem::SendChangedEvent(wxEventType type, size_t index, std::string const &str, long extra)
{
    wxCommandEvent event(type);
    event.SetEventObject(this);
    event.SetInt(index);
    if (!str.empty())
        event.SetString(wxString::FromUTF8(str.c_str()));
    else if (auto iter = error_messages.find(extra); iter != error_messages.end())     
        event.SetString(_L(iter->second.c_str()));
    else if (extra > CONTINUE && extra != ERROR_CANCEL)
        event.SetString(wxString::Format(_L("Error code: %d"), int(extra)));
    event.SetExtraLong(extra);
    if (wxThread::IsMain())
        ProcessEventLocally(event);
    else
        wxPostEvent(this, event);
}

void PrinterFileSystem::DumpLog(void * thiz, int, tchar const *msg)
{
    BOOST_LOG_TRIVIAL(info) << "PrinterFileSystem: " << wxString(msg).ToUTF8().data();
    static_cast<PrinterFileSystem*>(thiz)->Bambu_FreeLogMsg(msg);
}

boost::uint32_t PrinterFileSystem::SendRequest(int type, json const &req, callback_t2 const &callback)
{
    if (m_session.tunnel == nullptr) {
        Retry();
        callback(ERROR_PIPE, json(), nullptr);
        return 0;
    }
    boost::uint32_t seq = m_sequence + m_callbacks.size();
    json root;
    root["cmdtype"] = type;
    root["sequence"] = seq;
    root["req"] = req;
    std::ostringstream oss;
    oss << root;
    auto msg = oss.str();
    boost::unique_lock l(m_mutex);
    m_messages.push_back(msg);
    m_callbacks.push_back(callback);
    m_cond.notify_all();
    return seq;
}

void PrinterFileSystem::InstallNotify(int type, callback_t2 const &callback)
{
    type -= NOTIFY_FIRST;
    if (m_notifies.size() <= size_t(type)) m_notifies.resize(type + 1);
    m_notifies[type] = callback;
}

void PrinterFileSystem::CancelRequest(boost::uint32_t seq) { CancelRequests({seq}); }

void PrinterFileSystem::CancelRequests(std::vector<boost::uint32_t> const &seqs)
{
    json req;
    json arr;
    for (auto seq : seqs)
        arr.push_back(seq);
    req["tasks"] = arr;
    SendRequest(TASK_CANCEL, req, [this](int result, json const &resp, unsigned char const *) -> int {
        if (result != 0) return result;
        json tasks = resp["tasks"];
        std::vector<boost::uint32_t> seqs;
        for (auto &f : tasks) seqs.push_back(f);
        CancelRequests2(seqs);
        return 0;
    });
}

void PrinterFileSystem::CancelRequests2(std::vector<boost::uint32_t> const &seqs)
{
    std::vector<std::pair<boost::uint32_t, callback_t2>> callbacks;
    boost::unique_lock      l(m_mutex);
    for (auto &f : seqs) {
        boost::uint32_t seq = f;
        seq -= m_sequence;
        if (size_t(seq) >= m_callbacks.size())
            continue;
        auto &c = m_callbacks[seq];
        if (c == nullptr)
            continue;
        callbacks.emplace_back(f, c);
        c = nullptr;
    }
    while (!m_callbacks.empty() && m_callbacks.front() == nullptr) {
        m_callbacks.pop_front();
        ++m_sequence;
    }
    l.unlock();
    for (auto &c : callbacks) {
        wxLogInfo("PrinterFileSystem::CancelRequests2: %u\n", c.first);
        c.second(ERROR_CANCEL, json(), nullptr);
    }
}

void PrinterFileSystem::RecvMessageThread()
{
    Bambu_Sample sample;
    boost::unique_lock l(m_mutex);
    Reconnect(l, 0);
    while (true) {
        if (m_stopped && (m_session.owner == nullptr || (m_messages.empty() && m_callbacks.empty()))) {
            Reconnect(l, 0); // Close and wait start again
            if (m_session.owner == nullptr) {
                // clear callbacks first
                auto callbacks(std::move(m_callbacks));
                break;
            }
        }
        if (!m_messages.empty()) {
            auto & msg = m_messages.front();
            // OutputDebugStringA(msg.c_str());
            // OutputDebugStringA("\n");
            wxLogInfo("PrinterFileSystem::SendRequest >>>: \n%s\n", wxString::FromUTF8(msg));
            l.unlock();
            int n = Bambu_SendMessage(m_session.tunnel, CTRL_TYPE, msg.c_str(), msg.length());
            l.lock();
            if (n == 0)
                m_messages.pop_front();
            else if (n != Bambu_would_block) {
                Reconnect(l, n);
                continue;
            }
        }
        l.unlock();
        int n = Bambu_ReadSample(m_session.tunnel, &sample);
        l.lock();
        if (n == 0) {
            HandleResponse(l, sample);
        } else if (n == Bambu_stream_end) {
            m_stopped = true;
            Reconnect(l, m_status == ListSyncing ? ERROR_RES_BUSY : ERROR_PIPE);
        } else if (n == Bambu_would_block) {
            m_cond.timed_wait(l, boost::posix_time::milliseconds(m_messages.empty() && m_callbacks.empty() ? 1000 : 20));
        } else {
            Reconnect(l, n);
        }
    } // while
}

void PrinterFileSystem::HandleResponse(boost::unique_lock<boost::mutex> &l, Bambu_Sample const &sample)
{
    unsigned char const *end      = sample.buffer + sample.size;
    unsigned char const *json_end = (unsigned char const *) memchr(sample.buffer, '\n', sample.size);
    while (json_end && json_end + 3 < end && json_end[1] != '\n') json_end = (unsigned char const *) memchr(json_end + 2, '\n', end - json_end - 2);
    if (json_end)
        json_end += 2;
    else
        json_end = end;
    std::string msg((char const *) sample.buffer, json_end - sample.buffer);
    json        root;
    //OutputDebugStringA(msg.c_str());
    //OutputDebugStringA("\n");
    wxLogInfo("PrinterFileSystem::HandleResponse <<<: \n%s\n", wxString::FromUTF8(msg));
    std::istringstream iss(msg);
    int                cmd    = 0;
    int                seq    = -1;
    int                result = 0;
    json               resp;
    try {
        iss >> root;
        if (!root["result"].is_null()) {
            result = root["result"];
            seq    = root["sequence"];
            resp   = root["reply"];
        } else {
            // maybe notify
            cmd  = root["cmdtype"];
            seq  = root["sequence"];
            resp = root["notify"];
        }
    } catch (...) {
        result = ERROR_JSON;
        return;
    }
    if (cmd > 0) {
        if (cmd < NOTIFY_FIRST) return;
        cmd -= NOTIFY_FIRST;
        if (size_t(cmd) >= m_notifies.size()) return;
        auto n = m_notifies[cmd];
        l.unlock();
        n(result, resp, json_end);
        l.lock();
    } else {
        int seq2 = seq - m_sequence;
        if (size_t(seq2) >= m_callbacks.size()) return;
        auto c = m_callbacks[seq2];
        if (c == nullptr) return;
        l.unlock();
        int result2 = c(result, resp, json_end);
        l.lock();
        if (result2 != CONTINUE) {
            int seq2 = seq - m_sequence;
            m_callbacks[seq2] = callback_t2();
            if (seq2 == 0) {
                while (!m_callbacks.empty() && m_callbacks.front() == nullptr) {
                    m_callbacks.pop_front();
                    ++m_sequence;
                }
            }
            if (result == CONTINUE) {
                l.unlock();
                CancelRequest(seq);
                l.lock();
            }
        }
    }
}

void PrinterFileSystem::Reconnect(boost::unique_lock<boost::mutex> &l, int result)
{
    if (m_session.tunnel) {
        auto tunnel = m_session.tunnel;
        m_session.tunnel = nullptr;
        wxLogMessage("PrinterFileSystem::Reconnect close %d", result);
        l.unlock();
        Bambu_Close(tunnel);
        Bambu_Destroy(tunnel);
        l.lock();
    }
    if (m_session.owner == nullptr)
        return;
    json r;
    while(!m_callbacks.empty()) {
        auto c = m_callbacks.front();
        m_callbacks.pop_front();
        ++m_sequence;
        if (c) c(result, r, nullptr);
    }
    m_messages.clear();
    if (result)
        m_cond.timed_wait(l, boost::posix_time::seconds(10));
    while (true) {
        while (m_stopped) {
            if (m_session.owner == nullptr)
                return;
            m_status = Status::Stopped;
            SendChangedEvent(EVT_STATUS_CHANGED, m_status);
            m_cond.wait(l);
        }
        wxLogMessage("PrinterFileSystem::Reconnect Initializing");
        m_status = Status::Initializing;
        m_last_error = 0;
        SendChangedEvent(EVT_STATUS_CHANGED, m_status);
        // wait for url
        while (!m_stopped && m_messages.empty())
            m_cond.wait(l);
        if (m_stopped || m_messages.empty()) continue;
        std::string url = m_messages.front();
        m_messages.clear();
        if (url.size() < 2) {
            wxLogMessage("PrinterFileSystem::Reconnect Initialize failed: %s", wxString::FromUTF8(url));
            m_last_error = atoi(url.c_str());
            if (m_last_error == 0)
                m_stopped = true;
        } else {
            wxLogInfo("PrinterFileSystem::Reconnect Initialized: %s", wxString::FromUTF8(url));
            l.unlock();
            m_status = Status::Connecting;
            wxLogMessage("PrinterFileSystem::Reconnect Connecting");
            SendChangedEvent(EVT_STATUS_CHANGED, m_status);
            Bambu_Tunnel tunnel = nullptr;
            int ret = Bambu_Create(&tunnel, url.c_str());
            if (ret == 0) {

                Bambu_SetLogger(tunnel, DumpLog, this);
                ret = Bambu_Open(tunnel);
            }
            if (ret == 0)
                do {
                    ret = Bambu_StartStreamEx 
                        ? Bambu_StartStreamEx(tunnel, CTRL_TYPE)
                        : Bambu_StartStream(tunnel, false);
                } while (ret == Bambu_would_block);
            l.lock();
            if (ret == 0) {
                m_session.tunnel = tunnel;
                wxLogMessage("PrinterFileSystem::Reconnect Connected");
                break;
            } else if (ret == 1) {
                m_stopped = true;
                ret = ERROR_RES_BUSY;
            }
            if (tunnel) {
                Bambu_Close(tunnel);
                Bambu_Destroy(tunnel);
            }
            m_last_error = ret;
        }
        wxLogMessage("PrinterFileSystem::Reconnect Failed");
        m_status = Status::Failed;

        SendChangedEvent(EVT_STATUS_CHANGED, m_status, "", url.size() < 2 ? 1 : m_last_error);
        m_cond.timed_wait(l, boost::posix_time::seconds(10));
    }
    m_status = Status::ListSyncing;
    SendChangedEvent(EVT_STATUS_CHANGED, m_status);
#ifdef PRINTER_FILE_SYSTEM_TEST
    PostCallback([this] { SendChangedEvent(EVT_FILE_CHANGED); });
#else
    PostCallback([this] { m_task_flags = 0; ListAllFiles(); });
#endif
}


#include <stdlib.h>
#if defined(_MSC_VER) || defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#if defined(_MSC_VER) || defined(_WIN32)
static HMODULE module = NULL;
#else
static void* module = NULL;
#endif

static void* get_function(const char* name)
{
    void* function = nullptr;

    if (!module)
        return function;

#if defined(_MSC_VER) || defined(_WIN32)
    function = GetProcAddress(module, name);
#else
    function = dlsym(module, name);
#endif

    if (!function) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", can not find function %1%") % name;
    }
    return function;
}

#define GET_FUNC(x) lib.x = reinterpret_cast<decltype(lib.x)>(get_function(#x))

StaticBambuLib &StaticBambuLib::get()
{
    static StaticBambuLib lib;
    // first load the library

    if (lib.Bambu_Open)
        return lib;

    if (!module) {
        module = Slic3r::NetworkAgent::get_bambu_source_entry();
    }

    if (!module) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", can not Load Library";
    }

    GET_FUNC(Bambu_Create);
    GET_FUNC(Bambu_Open);
    GET_FUNC(Bambu_StartStream);
    GET_FUNC(Bambu_StartStreamEx);
    GET_FUNC(Bambu_GetStreamCount);
    GET_FUNC(Bambu_GetStreamInfo);
    GET_FUNC(Bambu_SendMessage);
    GET_FUNC(Bambu_ReadSample);
    GET_FUNC(Bambu_Close);
    GET_FUNC(Bambu_Destroy);
    GET_FUNC(Bambu_SetLogger);
    GET_FUNC(Bambu_FreeLogMsg);

    if (!lib.Bambu_Open)
        lib.Bambu_Create = Fake_Bambu_Create;
    return lib;
}

extern "C" BambuLib *bambulib_get() {
    return &StaticBambuLib::get();
}
