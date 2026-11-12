#ifndef slic3r_Updater_UpdateHelperFrame_hpp_
#define slic3r_Updater_UpdateHelperFrame_hpp_

// The updater helper's only window: version label, progress gauge, status line
// and a Cancel button. Everything slow happens on a worker thread; the frame
// only ever touches wxWidgets on the main thread.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include <boost/filesystem/path.hpp>

#include <wx/event.h>
#include <wx/frame.h>
#include <wx/string.h>

#include "libslic3r/UpdateHandoff.hpp"
#include "slic3r/UpdateHelper/InstallApply.hpp"

class wxButton;
class wxGauge;
class wxStaticText;

namespace Slic3r {
namespace Updater {

// Starts the application again after an update that did not install. False when it could not be started.
bool relaunch_app(const UpdateHandoff& handoff);

// Writes timestamped lines to handoff.log_path, starting a new log for each run.
// Safe to call from any thread.
class LogFile
{
public:
    explicit LogFile(std::string path);

    void write(const std::string& message);

private:
    std::mutex  m_mutex;
    std::string m_path;
    bool        m_started = false;
};

class UpdateHelperFrame : public wxFrame
{
public:
    explicit UpdateHelperFrame(const UpdateHandoff& handoff);
    ~UpdateHelperFrame() override;

    // Starts the wait/download/verify/install sequence.
    void start();

    // Valid once the worker has finished; the app returns this from OnRun().
    int exit_code() const { return m_exit_code.load(); }

private:
    enum class WaitResult { Ready, Timeout, Cancelled };
    enum class LockResult { Acquired, Cancelled, Failed };

    void on_cancel(wxCommandEvent& event);
    void on_close(wxCloseEvent& event);

    void run();                                  // worker thread
    WaitResult wait_for_parent();
    // Taken only after the parent is gone: until then it still holds the lock.
    LockResult take_install_lock(std::string& error);
    bool verify_artifact(const boost::filesystem::path& path, std::string& error) const;

    void post(Stage stage, int percent, const std::string& message);
    void show_progress(Stage stage, int percent, const wxString& message);
    void finish(int code, const wxString& message, bool alert);

    UpdateHandoff      m_handoff;
    LogFile            m_log;
    wxStaticText*      m_version_label = nullptr;
    wxStaticText*      m_status = nullptr;
    wxGauge*           m_gauge = nullptr;
    wxButton*          m_cancel = nullptr;

    Stage              m_stage = Stage::Idle;
    std::atomic<bool>  m_cancel_requested{ false };
    std::atomic<bool>  m_worker_done{ false };
    std::atomic<int>   m_exit_code{ 1 };
    // Worker thread only. Set once the parent is gone, so the application can be started again.
    bool               m_parent_gone = false;
    std::thread        m_worker;
};

} // namespace Updater
} // namespace Slic3r

#endif // slic3r_Updater_UpdateHelperFrame_hpp_
