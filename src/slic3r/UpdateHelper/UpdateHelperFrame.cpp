#include "slic3r/UpdateHelper/UpdateHelperFrame.hpp"

#include "slic3r/UpdateHelper/HttpDownloader.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <openssl/evp.h>

#include <wx/app.h>
#include <wx/button.h>
#include <wx/gauge.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace Slic3r {
namespace Updater {

namespace {

constexpr int      kParentPollMs = 250;
constexpr int      kParentTimeoutSeconds = 300;
constexpr int      kLockRetryMs = 250;
constexpr int      kLockTimeoutMs = 10000;
constexpr int64_t  kShaBufferSize = 64 * 1024;

int64_t current_process_id()
{
#if defined(_WIN32)
    return static_cast<int64_t>(::_getpid());
#else
    return static_cast<int64_t>(::getpid());
#endif
}

std::string timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    char buffer[32] = { 0 };
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
    return buffer;
}

std::string lower_ascii(std::string value)
{
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

std::string human_bytes(int64_t bytes)
{
    static const char* units[] = { "B", "KB", "MB", "GB" };
    double value = static_cast<double>(bytes);
    int    unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[32] = { 0 };
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return buffer;
}

bool process_alive(int64_t pid)
{
    if (pid <= 0)
        return false;
#if defined(_WIN32)
    HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!handle)
        return false;
    DWORD code = 0;
    const bool queried = ::GetExitCodeProcess(handle, &code) != 0;
    ::CloseHandle(handle);
    return queried && code == STILL_ACTIVE;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
        return true;
    return errno == EPERM; // exists, just not ours to signal
#endif
}

bool sha256_hex(const fs::path& path, std::string& hex, std::string& error)
{
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (!file) {
        error = "Could not read the downloaded file.";
        return false;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        std::fclose(file);
        error = "Could not start the checksum calculation.";
        return false;
    }

    bool          ok = true;
    unsigned char buffer[kShaBufferSize];
    size_t        read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (EVP_DigestUpdate(ctx, buffer, read) != 1) {
            ok = false;
            error = "Could not calculate the checksum.";
            break;
        }
    }
    if (ok && std::ferror(file)) {
        ok = false;
        error = "Could not read the downloaded file.";
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  length = 0;
    if (ok && EVP_DigestFinal_ex(ctx, digest, &length) != 1) {
        ok = false;
        error = "Could not finish the checksum calculation.";
    }

    EVP_MD_CTX_free(ctx);
    std::fclose(file);
    if (!ok)
        return false;

    static const char* kDigits = "0123456789abcdef";
    hex.clear();
    hex.reserve(static_cast<size_t>(length) * 2);
    for (unsigned int i = 0; i < length; ++i) {
        hex.push_back(kDigits[digest[i] >> 4]);
        hex.push_back(kDigits[digest[i] & 0x0F]);
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// LogFile
// ---------------------------------------------------------------------------

LogFile::LogFile(std::string path)
    : m_path(std::move(path))
{
}

void LogFile::write(const std::string& message)
{
    if (m_path.empty())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    const fs::path            path(m_path);
    std::error_code           ec;
    const fs::path            parent = path.parent_path();
    if (!parent.empty())
        fs::create_directories(parent, ec);

    // Truncated on the first write of a run: the log describes the run in progress, so it
    // never accumulates one copy of itself per update.
    std::ofstream out(path, (m_started ? std::ios::app : std::ios::trunc) | std::ios::binary);
    if (!out)
        return;
    m_started = true;
    out << "[" << timestamp() << "] " << message << "\n";
}

// ---------------------------------------------------------------------------
// UpdateHelperFrame
// ---------------------------------------------------------------------------

UpdateHelperFrame::UpdateHelperFrame(const UpdateHandoff& handoff)
    : wxFrame(nullptr, wxID_ANY, "OrcaSlicer Update")
    , m_handoff(handoff)
    , m_log(handoff.log_path)
{
    SetClientSize(wxSize(480, 200));

    auto* root = new wxBoxSizer(wxVERTICAL);

    wxString label = "OrcaSlicer";
    if (!handoff.version_label.empty())
        label += " " + wxString::FromUTF8(handoff.version_label);
    if (!handoff.channel.empty())
        label += " (" + wxString::FromUTF8(handoff.channel) + " channel)";

    m_version_label = new wxStaticText(this, wxID_ANY, label);
    wxFont title_font = m_version_label->GetFont();
    title_font.MakeBold();
    m_version_label->SetFont(title_font);

    m_status = new wxStaticText(this, wxID_ANY, "Waiting for OrcaSlicer to close...");
    m_gauge  = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, 16));
    m_gauge->SetValue(0);

    m_cancel = new wxButton(this, wxID_CANCEL, "Cancel");
    m_cancel->Enable(false);

    root->Add(m_version_label, 0, wxLEFT | wxRIGHT | wxTOP, 16);
    root->Add(m_status, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 16);
    root->Add(m_gauge, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 16);
    root->AddStretchSpacer(1);
    root->Add(m_cancel, 0, wxALIGN_RIGHT | wxALL, 16);
    SetSizer(root);
    Layout();
    Centre();

    Bind(wxEVT_BUTTON, &UpdateHelperFrame::on_cancel, this, wxID_CANCEL);
    Bind(wxEVT_CLOSE_WINDOW, &UpdateHelperFrame::on_close, this);

    m_log.write("update helper started for version " + handoff.version + " on the " + handoff.channel + " channel");
}

UpdateHelperFrame::~UpdateHelperFrame()
{
    m_cancel_requested = true;
    if (m_worker.joinable())
        m_worker.join();
}

void UpdateHelperFrame::start()
{
    m_worker = std::thread([this]() { run(); });
}

void UpdateHelperFrame::on_cancel(wxCommandEvent& /*event*/)
{
    if (m_worker_done)
        return;
    m_cancel_requested = true;
    m_cancel->Disable();
    m_status->SetLabel("Cancelling...");
    Layout();
    m_log.write("cancel requested");
}

void UpdateHelperFrame::on_close(wxCloseEvent& event)
{
    if (m_worker_done) {
        event.Skip();
        return;
    }
    if (m_stage == Stage::Installing || m_stage == Stage::Launching) {
        m_status->SetLabel("Installing the update. Please wait...");
        Layout();
        event.Veto();
        return;
    }
    m_cancel_requested = true;
    m_cancel->Disable();
    m_status->SetLabel("Cancelling...");
    Layout();
    event.Veto();
}

void UpdateHelperFrame::post(Stage stage, int percent, const std::string& message)
{
    const wxString text = wxString::FromUTF8(message);
    wxTheApp->CallAfter([this, stage, percent, text]() { show_progress(stage, percent, text); });
}

void UpdateHelperFrame::show_progress(Stage stage, int percent, const wxString& message)
{
    m_stage = stage;
    if (!message.empty()) {
        m_status->SetLabel(message);
        m_status->Wrap(GetClientSize().x - 32);
    }
    if (percent < 0)
        m_gauge->Pulse();
    else
        m_gauge->SetValue(std::clamp(percent, 0, 100));

    m_cancel->Enable(stage == Stage::Downloading && !m_cancel_requested.load());
    Layout();
}

void UpdateHelperFrame::finish(int code, const wxString& message, bool alert)
{
    m_exit_code.store(code);
    m_worker_done.store(true);
    wxTheApp->CallAfter([this, message, alert]() {
        if (!message.empty()) {
            m_status->SetLabel(message);
            Layout();
        }
        if (alert) {
            wxString text = message;
            const wxString log_path = wxString::FromUTF8(m_handoff.log_path);
            if (!log_path.empty())
                text += "\n\nLog: " + log_path;
            wxMessageBox(text, "OrcaSlicer Update", wxOK | wxICON_ERROR, this);
        }
        wxTheApp->ExitMainLoop();
    });
}

UpdateHelperFrame::WaitResult UpdateHelperFrame::wait_for_parent()
{
    if (m_handoff.parent_pid <= 0)
        return WaitResult::Ready;

    post(Stage::Idle, -1, "Waiting for OrcaSlicer to close...");
    m_log.write("waiting for process " + std::to_string(m_handoff.parent_pid) + " to exit");

    const int rounds = (kParentTimeoutSeconds * 1000) / kParentPollMs;
    for (int i = 0; i < rounds; ++i) {
        if (!process_alive(m_handoff.parent_pid))
            return WaitResult::Ready;
        if (m_cancel_requested.load())
            return WaitResult::Cancelled;
        std::this_thread::sleep_for(std::chrono::milliseconds(kParentPollMs));
    }
    if (!process_alive(m_handoff.parent_pid))
        return WaitResult::Ready;
    return WaitResult::Timeout;
}

// The application holds this lock while it shuts down, so the first attempt can
// still fail after the parent process has gone: retry briefly before giving up.
UpdateHelperFrame::LockResult UpdateHelperFrame::take_install_lock(std::string& error)
{
    if (m_handoff.lock_path.empty()) {
        error = "The update does not say where the install lock is, so it was not started.";
        return LockResult::Failed;
    }

    post(Stage::Idle, -1, "Waiting for the previous update to finish...");
    m_log.write("taking the install lock at " + m_handoff.lock_path);

    const int rounds = kLockTimeoutMs / kLockRetryMs;
    for (int i = 0; i <= rounds; ++i) {
        if (Slic3r::acquire_install_lock(m_handoff.lock_path, current_process_id())) {
            m_log.write("install lock acquired");
            return LockResult::Acquired;
        }
        if (m_cancel_requested.load())
            return LockResult::Cancelled;
        if (i < rounds)
            std::this_thread::sleep_for(std::chrono::milliseconds(kLockRetryMs));
    }

    m_log.write("install lock still held by another process after " + std::to_string(kLockTimeoutMs) + " ms");
    error = "An update is already being installed. Close the other updater and try again.";
    return LockResult::Failed;
}

bool UpdateHelperFrame::verify_artifact(const fs::path& path, std::string& error) const
{
    std::error_code ec;
    const auto      size = fs::file_size(path, ec);
    if (ec) {
        error = "The downloaded file is missing.";
        return false;
    }
    if (m_handoff.size > 0 && static_cast<int64_t>(size) != m_handoff.size) {
        error = "The downloaded file is " + std::to_string(size) + " bytes, expected " +
                std::to_string(m_handoff.size) + ".";
        return false;
    }

    std::string expected = lower_ascii(m_handoff.digest);
    const std::string prefix = "sha256:";
    if (expected.compare(0, prefix.size(), prefix) == 0)
        expected = expected.substr(prefix.size());
    else if (expected.find(':') != std::string::npos) {
        error = "Unsupported checksum \"" + m_handoff.digest + "\".";
        return false;
    }
    if (expected.empty()) {
        error = "The update does not carry a checksum.";
        return false;
    }

    std::string actual;
    if (!sha256_hex(path, actual, error))
        return false;
    if (actual != expected) {
        error = "Checksum mismatch: the file is sha256:" + actual + ".";
        return false;
    }
    return true;
}

namespace {

void log_dropped(const Slic3r::DownloadPrep& prep, LogFile& log)
{
    for (const std::string& path : prep.dropped)
        log.write("dropped " + path);
}

} // namespace

void UpdateHelperFrame::run()
{
    try {
        const WaitResult waited = wait_for_parent();
        if (waited == WaitResult::Cancelled) {
            m_log.write("cancelled while waiting for the parent process");
            finish(2, "Update cancelled.", false);
            return;
        }
        if (waited == WaitResult::Timeout) {
            m_log.write("giving up: the parent process is still running");
            finish(1, "OrcaSlicer is still running.", true);
            return;
        }
        m_log.write("parent process is gone");

        // The lock is taken here, not at startup: the application still owned it
        // while it was closing, so acquiring it earlier always failed.
        std::string lock_error;
        const LockResult locked = take_install_lock(lock_error);
        if (locked == LockResult::Cancelled) {
            m_log.write("cancelled while waiting for the install lock");
            finish(2, "Update cancelled.", false);
            return;
        }
        if (locked == LockResult::Failed) {
            m_log.write("could not take the install lock: " + lock_error);
            finish(1, wxString::FromUTF8(lock_error), true);
            return;
        }

        // Every path comes from the handoff: the helper may resolve the cache
        // directory differently than the application did.
        std::string asset = fs::path(m_handoff.asset_name).filename().string();
        if (asset.empty())
            asset = "update.part";
        const fs::path     download_dir = fs::path(m_handoff.cache_dir) / "downloads";
        const fs::path     part         = download_dir / (asset + ".part");
        const DownloadPrep prep = prepare_download_dir(download_dir.string(), asset, m_handoff.digest, m_handoff.size);
        log_dropped(prep, m_log);

        if (prep.complete) {
            // A previous run downloaded and verified this build but did not install it:
            // verify it again rather than fetch it a second time.
            m_log.write("the download of " + asset + " is already complete");
            post(Stage::Downloading, 100, "Downloading " + m_handoff.asset_name + "... done");
        } else {
            m_log.write("starting the download" + std::string(prep.resumable ? " (resuming)" : ""));
            post(Stage::Downloading, 0, "Downloading " + m_handoff.asset_name + "...");
            m_log.write("downloading " + m_handoff.url + " to " + part.string());

            HttpDownloader downloader;
            const DownloadResult result = downloader.download(
                m_handoff.url, part.string(),
                [this](int64_t done, int64_t total) {
                    const int percent = total > 0 ? static_cast<int>(done * 100 / total) : -1;
                    std::string message = "Downloading " + m_handoff.asset_name + "... " + human_bytes(done);
                    if (total > 0)
                        message += " of " + human_bytes(total);
                    post(Stage::Downloading, percent, message);
                },
                m_cancel_requested);

            if (result.cancelled) {
                m_log.write("download cancelled at " + std::to_string(result.done) + " bytes");
                finish(2, "Update cancelled.", false);
                return;
            }
            if (!result.ok) {
                m_log.write("download failed: " + result.error);
                finish(1, wxString::FromUTF8("The download failed: " + result.error), true);
                return;
            }
            m_log.write("downloaded " + std::to_string(result.done) + " bytes");
        }

        post(Stage::Verifying, 100, "Verifying the download...");
        m_log.write("verifying " + part.string());
        std::string verify_error;
        if (!verify_artifact(part, verify_error)) {
            m_log.write("verification failed: " + verify_error);
            std::error_code ec;
            fs::remove(part, ec);
            finish(1, "The downloaded file is damaged.", true);
            return;
        }
        m_log.write("checksum and size match");

        ProgressSink sink;
        sink.report = [this](Stage stage, int percent, const std::string& message) {
            m_log.write(std::string(stage_key(stage)) + " " + std::to_string(percent) + "% " + message);
            post(stage, percent, message);
        };
        sink.cancelled = [this]() { return m_cancel_requested.load(); };

        std::string install_error;
        if (!apply_update(m_handoff, part.string(), sink, install_error)) {
            m_log.write("install failed: " + install_error);
            finish(1, wxString::FromUTF8("The update could not be installed: " + install_error), true);
            return;
        }

        m_log.write("update installed, helper done");
        finish(0, "Update installed. OrcaSlicer is starting...", false);
    } catch (const std::exception& ex) {
        m_log.write(std::string("unexpected error: ") + ex.what());
        finish(1, "The update helper stopped unexpectedly.", true);
    } catch (...) {
        m_log.write("unexpected error");
        finish(1, "The update helper stopped unexpectedly.", true);
    }
}

} // namespace Updater
} // namespace Slic3r
