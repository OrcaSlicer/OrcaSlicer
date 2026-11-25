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

// Writes timestamped lines to handoff.log_path, appended across runs and rotated once
// it grows past a few hundred kilobytes. Safe to call from any thread.
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
    // How this run was started. The silent mode never shows the window: it only
    // writes the log, and leaves any failure for the next start to report.
    enum class Mode {
        Interactive,  // started from the application: wait, download, install
        Prefetch,     // --silent: fetch and verify the artifact only; the install waits for the user
    };

    explicit UpdateHelperFrame(const UpdateHandoff& handoff, Mode mode = Mode::Interactive);
    ~UpdateHelperFrame() override;

    // Starts this mode's sequence on a worker thread.
    void start();

    // Valid once the worker has finished; the app returns this from OnRun().
    int exit_code() const { return m_exit_code.load(); }

private:
    enum class WaitResult { Ready, Timeout, Cancelled };
    enum class LockResult { Acquired, Cancelled, Failed };
    // Why verification failed. Only SizeMismatch and DigestMismatch say the bytes are wrong;
    // a read error or an unsupported digest says nothing about the file.
    enum class VerifyFailure { None, Missing, SizeMismatch, DigestMismatch, BadDigest, ReadError };

    void on_cancel(wxCommandEvent& event);
    void on_close(wxCloseEvent& event);

    bool silent() const { return m_mode == Mode::Prefetch; }

    void run();                                  // worker thread
    // Fetches the artifact into `artifact` unless an earlier run already did. Does not verify it.
    bool fetch_artifact(const boost::filesystem::path& download_dir,
                        const boost::filesystem::path& part, const boost::filesystem::path& artifact);
    // Verifies the file this run or an earlier one fetched. Any failure is this run's failure.
    bool check_artifact(const boost::filesystem::path& artifact);
    // Ends a failed run: logged, and recorded for the next start when there is no window.
    void fail(int code, const std::string& message);
    WaitResult wait_for_parent();
    // Taken only after the parent is gone: until then it still holds the lock.
    LockResult take_install_lock(std::string& error);
    bool verify_artifact(const boost::filesystem::path& path, std::string& error,
                         VerifyFailure* failure = nullptr) const;

    void post(Stage stage, int percent, const std::string& message);
    void show_progress(Stage stage, int percent, const wxString& message);
    void finish(int code, const wxString& message, bool alert);

    UpdateHandoff      m_handoff;
    Mode               m_mode = Mode::Interactive;
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
    // Worker thread only. Set by an install that stopped without failing: the Windows
    // installer outlived its timeout, or the new build is in place but did not start.
    std::string        m_install_notice;
    std::thread        m_worker;
};

} // namespace Updater
} // namespace Slic3r

#endif // slic3r_Updater_UpdateHelperFrame_hpp_
