#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <wx/app.h>
#include <wx/msgdlg.h>
#include <wx/string.h>

#include "libslic3r/UpdateHandoff.hpp"
#include "slic3r/UpdateHelper/UpdateHelperFrame.hpp"

namespace fs = std::filesystem;

namespace {

const char* kDialogTitle = "OrcaSlicer Update";

// Accepts "--handoff <path>" and "--handoff=<path>".
bool parse_handoff_path(int argc, wxChar** argv, wxString& out)
{
    for (int i = 1; i < argc; ++i) {
        const wxString arg(argv[i]);
        if (arg == "--handoff" && i + 1 < argc) {
            out = argv[++i];
            return true;
        }
        const wxString prefix("--handoff=");
        if (arg.StartsWith(prefix)) {
            out = arg.Mid(prefix.Length());
            return true;
        }
    }
    return false;
}

// Reads the handoff at `path`. The command-line path is authoritative, so this
// never falls back to the default file.
bool read_handoff_from_file(const std::string& path, Slic3r::UpdateHandoff& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    const std::string json{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    return !json.empty() && Slic3r::handoff_from_json(json, out);
}

} // namespace

class UpdateHelperApp : public wxApp
{
public:
    bool OnInit() override;
    int  OnRun() override;

private:
    void release_lock();

    Slic3r::Updater::UpdateHelperFrame* m_frame = nullptr;
    bool        m_startup_failed = false;
    std::string m_lock_path;
};

wxIMPLEMENT_APP(UpdateHelperApp);

void UpdateHelperApp::release_lock()
{
    if (m_lock_path.empty())
        return;
    Slic3r::release_install_lock(m_lock_path);
    m_lock_path.clear();
}

bool UpdateHelperApp::OnInit()
{
    if (!wxApp::OnInit())
        return false;
    SetAppName("OrcaSlicerUpdater");

    wxString handoff_arg;
    // A path given on the command line wins: for a portable Windows install the
    // default cache lives inside the tree being replaced, so it may name the
    // wrong file. The default is only used when nothing was passed.
    const bool from_command_line = parse_handoff_path(argc, argv, handoff_arg) && !handoff_arg.empty();
    const std::string path = from_command_line ? std::string(handoff_arg.ToUTF8().data())
                                               : Slic3r::updater_handoff_path();

    Slic3r::UpdateHandoff handoff;
    if (!read_handoff_from_file(path, handoff)) {
        if (!from_command_line)
            wxMessageBox("The updater was started without an update to install.", kDialogTitle,
                         wxOK | wxICON_ERROR);
        else
            wxMessageBox("The update information could not be read.", kDialogTitle, wxOK | wxICON_ERROR);
        Slic3r::remove_handoff();
        m_startup_failed = true;
        return true;
    }

    // The application resolved the cache directory for its own data dir. Every
    // record written from here on must land where the application reads it, so
    // this has to happen before any other file operation.
    if (handoff.cache_dir.empty())
        handoff.cache_dir = Slic3r::updater_cache_dir();
    Slic3r::set_updater_cache_dir(handoff.cache_dir);

    // Handed to the frame, which takes it only once the parent has exited: the
    // application still owns it while it is closing down. The single release
    // stays in the app, so every exit path frees it.
    m_lock_path = handoff.lock_path.empty() ? Slic3r::updater_lock_path() : handoff.lock_path;
    handoff.lock_path = m_lock_path;

    // A handoff is good for one run only: delete it before anything else so a
    // stale file can never replay this update.
    Slic3r::remove_handoff();
    {
        std::error_code ec;
        fs::remove(path, ec);
    }

    if (handoff.url.empty()) {
        release_lock();
        wxMessageBox("The update information is incomplete.", kDialogTitle, wxOK | wxICON_ERROR);
        m_startup_failed = true;
        return true;
    }

    m_frame = new Slic3r::Updater::UpdateHelperFrame(handoff);
    m_frame->Show();
    m_frame->start();
    return true;
}

int UpdateHelperApp::OnRun()
{
    if (m_startup_failed || !m_frame) {
        release_lock();
        return 1;
    }
    wxApp::OnRun();
    const int code = m_frame->exit_code();
    // The install lock lives until the helper is done; releasing it here covers
    // success, failure and a cancelled run alike.
    release_lock();
    return code;
}
