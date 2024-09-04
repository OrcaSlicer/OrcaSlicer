#ifndef slic3r_GUI_PrinterFileSystem_h_
#define slic3r_GUI_PrinterFileSystem_h_

#define BAMBU_DYNAMIC
#include "BambuTunnel.h"

#include <wx/bitmap.h>
#include <wx/event.h>

#include <boost/thread.hpp>
#include <boost/enable_shared_from_this.hpp>

#include "nlohmann/json_fwd.hpp"
using nlohmann::json;

#include <functional>
#include <deque>

wxDECLARE_EVENT(EVT_STATUS_CHANGED, wxCommandEvent);
wxDECLARE_EVENT(EVT_MODE_CHANGED, wxCommandEvent);
wxDECLARE_EVENT(EVT_FILE_CHANGED, wxCommandEvent);
wxDECLARE_EVENT(EVT_SELECT_CHANGED, wxCommandEvent);
wxDECLARE_EVENT(EVT_THUMBNAIL, wxCommandEvent);
wxDECLARE_EVENT(EVT_DOWNLOAD, wxCommandEvent);
wxDECLARE_EVENT(EVT_RAMDOWNLOAD, wxCommandEvent);
wxDECLARE_EVENT(EVT_MEDIA_ABILITY_CHANGED, wxCommandEvent);
wxDECLARE_EVENT(EVT_UPLOADING, wxCommandEvent);
wxDECLARE_EVENT(EVT_UPLOAD_CHANGED, wxCommandEvent);

class PrinterFileSystem : public wxEvtHandler, public boost::enable_shared_from_this<PrinterFileSystem>, BambuLib
{
    static const int CTRL_TYPE     = 0x3001;

    enum {
        LIST_INFO             = 0x0001,
        SUB_FILE              = 0x0002,
        FILE_DEL              = 0x0003,
        FILE_DOWNLOAD         = 0x0004,
        FILE_UPLOAD           = 0x0005,
        REQUEST_MEDIA_ABILITY = 0x0007,
        NOTIFY_FIRST          = 0x0100,
        LIST_CHANGE_NOTIFY    = 0x0100,
        LIST_RESYNC_NOTIFY    = 0x0101,
        TASK_CANCEL           = 0x1000
    };

public:
    enum {
        SUCCESS                  = 0,
        CONTINUE                 = 1,
        ERROR_JSON               = 2,
        ERROR_PIPE               = 3,
        ERROR_CANCEL             = 4,
        ERROR_RES_BUSY           = 5,
        ERROR_TIME_OUT           = 6,
        FILE_NO_EXIST            = 10,
        FILE_NAME_INVALID        = 11,
        FILE_SIZE_ERR            = 12,
        FILE_OPEN_ERR            = 13,
        FILE_READ_WRITE_ERR      = 14,
        FILE_CHECK_ERR           = 15,
        FILE_TYPE_ERR            = 16,
        STORAGE_UNAVAILABLE      = 17,
        API_VERSION_UNSUPPORT    = 18,
        FILE_EXIST               = 19,
        STORAGE_SPACE_NOT_ENOUGH = 20,
        FILE_CREATE_ERR          = 21,
        FILE_WRITE_ERR           = 22,
        MD5_COMPARE_ERR          = 23,
        FILE_RENAME_ERR          = 24,
        SEND_ERR                 = 25,
    };


public:
    PrinterFileSystem();

    ~PrinterFileSystem();

public:
    enum FileType {
        F_TIMELAPSE,
        F_VIDEO,
        F_MODEL,
        F_INVALID_TYPE,
    };

    enum GroupMode {
        G_NONE,
        G_MONTH,
        G_YEAR,
    };

    void SetFileType(FileType type, std::string const & storage = {});

    void SetGroupMode(GroupMode mode);

    size_t EnterSubGroup(size_t index);

    FileType GetFileType() const { return m_file_type; }

    GroupMode GetGroupMode() const { return m_group_mode; }

    template<typename T> using Callback = std::function<void(int, T)>;

    enum Flags {
        FF_SELECT         = 1,
        FF_THUMNAIL       = 2,      // Thumbnail ready
        FF_DOWNLOAD       = 4,      // Request download
        FF_DELETED        = 8,      // Request delete
        FF_FETCH_MODEL    = 16,     // Request model
        FF_UPLOADING      = 1 << 5, // File uploading
        FF_UPLOADDONE     = 1 << 6, // File upload done
        FF_UPLOADCANCEL   = 1 << 7, // File upload cancel
        FF_THUMNAIL_RETRY = 0x100,  // Thumbnail need retry
    };

    enum UploadStatus
    {
        Uploading = 1 << 0,
        UploadDone = 1 << 1,
        UploadCancel = 1 << 2,
    };

    enum RequestMediaAbilityStatus
    {
        S_SUCCESS,
        S_FAILED
    };

    struct Progress
    {
        wxInt64 size  = 0;
        wxInt64 total = 0;
        int     progress = 0;
    };

    struct Download;
    struct Upload;

    struct File
    {
        std::string name;
        std::string path;
        time_t time = 0;
        boost::uint64_t size = 0;
        int         flags = 0;
        wxBitmap    thumbnail;
        std::shared_ptr<Download> download;
        std::string local_path;
        std::map<std::string, std::string> metadata;

        bool IsSelect() const { return flags & FF_SELECT; }
        bool IsDownload() const { return flags & FF_DOWNLOAD; }
        int DownloadProgress() const;
        std::string Title() const;
        std::string Metadata(std::string const &key, std::string const &dflt) const;

        friend bool operator<(File const & l, File const & r) { return l.time > r.time; }
    };

    struct UploadFile
    {
        std::string     name;
        std::string     path;
        std::string     select_storage;
        int             flags{0};
        boost::uint32_t size{0};
        boost::uint32_t chunk_size{0}; // KB
        std::unique_ptr<Upload> upload;

        bool IsUploading() const { return flags & FF_UPLOADING; }
    };

    struct Void {};

    typedef std::vector<File> FileList;
    typedef std::vector<std::string> MediaAbilityList;

    void ListAllFiles();

    void DeleteFiles(size_t index);

    void DownloadFiles(size_t index, std::string const &path);

    void GetPickImage(int id, const std::string &local_path, const std::string &path);

    void GetPickImages(const std::vector<std::string> &local_paths, const std::vector<std::string> &targetpaths);


    void DownloadRamFile(int index, const std::string &local_path, const std::string &param);

    void SendExistedFile();

    void SendConnectFail();

    void DownloadCheckFiles(std::string const &path);

    bool DownloadCheckFile(size_t index);

    void DownloadCancel(size_t index);

    void FetchModel(size_t index, std::function<void(int, std::string const &)> callback);

    void FetchModelCancel();

    size_t GetCount() const;

    size_t GetIndexAtTime(boost::uint32_t time);

    void ToggleSelect(size_t index);
    
    void SelectAll(bool select);

    size_t GetSelectCount() const;

    void SetFocusRange(size_t start, size_t count);

    File const &GetFile(size_t index);

    File const &GetFile(size_t index, bool &select);

    enum Status {
        Initializing,
        Connecting, 
        ListSyncing,
        ListReady,
        Failed,
        Reconnecting,
    };
    
    Status GetStatus() const { return m_status; }
    int GetLastError() const { return m_last_error; }

    void Attached();

    void Start();

    void Retry();

    void SetUrl(std::string const &url);

    void Stop(bool quit = false);

    boost::uint32_t RequestMediaAbility(int api_version);

    void RequestUploadFile();

    MediaAbilityList GetMediaAbilityList() const;

    void SetUploadFile(const std::string& path, const std::string& name, const std::string& select_storage);

    void CancelUploadTask(bool send_cancel_req = true);

private:
    void BuildGroups();

    void UpdateGroupSelect();

    void DeleteFilesContinue();

    void DownloadNextFile();

    void UpdateFocusThumbnail();

    static bool ParseThumbnail(File &file);

    static bool ParseThumbnail(File &file, std::istream &is);

    void UpdateFocusThumbnail2(std::shared_ptr<std::vector<File>> files, int type);

    void FileRemoved(std::pair<FileType, std::string> type, size_t index, std::string const &name, bool by_path);

    std::pair<FileList &, size_t> FindFile(std::pair<FileType, std::string> type, size_t index, std::string const &name, bool by_path);

    void SendChangedEvent(wxEventType type, size_t index = (size_t)-1, std::string const &str = {}, long extra = 0);

    static void DumpLog(void* context, int level, tchar const *msg);

private:
    template<typename T> using Translator = std::function<int(json const &, T &, unsigned char const *)>;

    typedef std::function<void(int, json const & resp)> callback_t;

    typedef std::function<int(int, json const &resp, unsigned char const *data)> callback_t2;

    typedef std::function<int(std::string &msg)> callback_t3;

    template<typename T> boost::uint32_t SendRequest(int type, json const &req, Translator<T> const &translator, Callback<T> const &callback, const std::string &param = "")
    {
        auto c = [translator, callback, this](int result, json const &resp, unsigned char const *data) -> int
        {
            T t;
            if (result == 0 || result == CONTINUE || result == FILE_EXIST) {
                try {
                    int n  = (translator != nullptr) ? translator(resp, t, data) : 0;
                    result = n == 0 ? result : n;
                }
                catch (...) {
                    result = ERROR_JSON;
                }
            }
            PostCallback<T>(callback, result, t);
            return result;
        };
        return SendRequest(type, req, c, param);
    }

    template<typename T> using Applier = std::function<void(T const &)>;

    template<typename T>
    void InstallNotify(int type, Translator<T> const& translator, Applier<T> const& applier)
    {
        auto c = [translator, applier, this](int result, json const &resp, unsigned char const *data) -> int
        {
            T t;
            if (result == 0 || result == CONTINUE) {
                try {
                    int n  = (translator != nullptr) ? translator(resp, t, data) : 0;
                    result = n == 0 ? result : n;
                }
                catch (...) {
                    result = ERROR_JSON;
                }
            }
            if (result == 0 && applier) {
                PostCallback<T>([applier](int, T const & t) {
                    applier(t);
                }, 0, t);
            }
            return result;
        };
        InstallNotify(type, c);
    }

    boost::uint32_t SendRequest(int type, json const &req, callback_t2 const &callback, const std::string &param = "");

    void InstallNotify(int type, callback_t2 const &callback);

    void CancelRequest(boost::uint32_t seq);

    void CancelRequests(std::vector<boost::uint32_t> const &seqs);

    void CancelRequests2(std::vector<boost::uint32_t> const & seqs);

    void RecvMessageThread();

    void HandleResponse(boost::unique_lock<boost::mutex> &l, Bambu_Sample const &sample);

    void Reconnect(boost::unique_lock<boost::mutex> & l, int result);

    template <typename T>
    void PostCallback(Callback<T> const& callback, int result, T const& resp)
    {
        PostCallback([=] { callback(result, resp); });
    }

    void PostCallback(std::function<void(void)> const & callback);

    int UploadFileTask(std::shared_ptr<UploadFile> upload_file, boost::uint64_t seq, std::string &msg);

protected:
    FileType m_file_type = F_INVALID_TYPE;
    std::string m_file_storage;
    GroupMode m_group_mode = G_NONE;
    FileList m_file_list;
    std::map<std::pair<FileType, std::string>, FileList> m_file_list_cache;
    std::vector<size_t> m_group_year;
    std::vector<size_t> m_group_month;
    std::vector<int> m_group_flags;
    std::shared_ptr<UploadFile> m_upload_file;

private:
    size_t m_select_count = 0;
    size_t m_lock_start = 0;
    size_t m_lock_end   = 0;
    int m_task_flags = 0;

    std::vector<bool> m_download_states;

private:
    struct Session
    {
        Bambu_Tunnel tunnel = nullptr;
        PrinterFileSystem * owner;
    };
    Session m_session;
    boost::uint32_t m_sequence = 0;
    boost::uint32_t m_download_seq = 0;
    boost::uint32_t m_fetch_model_seq = 0;
    boost::uint32_t m_upload_seq = 0;
    std::deque<std::string> m_messages;
    std::deque<callback_t2> m_callbacks;
    std::deque<callback_t2> m_notifies;
    bool m_stopped = true;
    boost::mutex m_mutex;
    boost::condition_variable m_cond;
    boost::thread m_recv_thread;
    Status m_status;
    int m_last_error = 0;

    MediaAbilityList m_media_ability_list;
    std::map<boost::uint32_t, callback_t3>  m_produce_message_cb_map;
};

#endif // !slic3r_GUI_PrinterFileSystem_h_
