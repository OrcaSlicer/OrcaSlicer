#ifndef slic3r_Downloader_hpp_
#define slic3r_Downloader_hpp_

#include "DownloaderFileGet.hpp"
#include <boost/filesystem/path.hpp>
#include <wx/wx.h>

namespace Slic3r {
namespace GUI {

class NotificationManager;

enum DownloadState
{
    DownloadPending = 0,
    DownloadOngoing,
    DownloadStopped,
    DownloadDone,
    DownloadError,
    DownloadPaused,
    DownloadStateUnknown
};

enum DownloaderUserAction
{
    DownloadUserCanceled,
    DownloadUserPaused,
    DownloadUserContinued,
    DownloadUserOpenedFolder
};

class Download { 
public:
    Download(int ID, std::string url, wxEvtHandler* evt_handler, const boost::filesystem::path& dest_folder);
    void start();
    void cancel();
    void pause();
    void resume();

    int get_id() const { return m_id; }
    boost::filesystem::path get_final_path() const { return m_final_path; }
    std::string get_filename() const { return m_filename; }
    DownloadState get_state() const { return m_state; }
    void set_state(DownloadState state) { m_state = state; }
    std::string get_dest_folder() { return m_dest_folder.string(); }
private: 
    const int m_id;
    std::string m_filename;
    boost::filesystem::path m_final_path;
    boost::filesystem::path m_dest_folder;
    std::shared_ptr<FileGet> m_file_get;
    DownloadState m_state { DownloadState::DownloadPending };
};

class Downloader : public wxEvtHandler {
public:
    Downloader();
    
    bool get_initialized() { return m_initialized; }
    void init(const boost::filesystem::path& dest_folder) 
    { 
        m_dest_folder = dest_folder;
        m_initialized = true; 
    }
    void start_download(const std::string& full_url);
    // cancel = false -> just pause
    bool user_action_callback(DownloaderUserAction action, int id);
private:
    bool m_initialized { false };

    std::vector<std::unique_ptr<Download>> m_downloads;
    boost::filesystem::path m_dest_folder;

    size_t m_next_id { 0 };
    size_t get_next_id() { return ++m_next_id; }

    void on_progress(wxCommandEvent& event);
    void on_error(wxCommandEvent& event);
    void on_complete(wxCommandEvent& event);
    void on_name_change(wxCommandEvent& event);
    void on_paused(wxCommandEvent& event);
    void on_canceled(wxCommandEvent& event);

    void set_download_state(int id, DownloadState state);
    /*
    bool is_in_state(int id, DownloadState state) const;
    DownloadState get_download_state(int id) const;
    bool cancel_download(int id);
    bool pause_download(int id);
    bool resume_download(int id);
    bool delete_download(int id);
    wxString get_path_of(int id) const;
    wxString get_folder_path_of(int id) const;
    */
};

}
}
#endif