#include "slic3r/UpdateHelper/InstallPlatform.hpp"
#include <libslic3r/UpdateHandoff.hpp>
#include <slic3r/UpdateHelper/InstallApply.hpp>

#if defined(__APPLE__)

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <crt_externs.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <boost/filesystem.hpp>

#include "libslic3r/UpdateHandoff.hpp"

#endif

namespace Slic3r {
namespace Updater {

#if defined(__APPLE__)

namespace {

bool spawn_and_wait(const std::vector<std::string>& argv, int timeout_seconds, int& status, std::string& error)
{
    status = -1;
    error.clear();
    if (argv.empty()) {
        error = "There is no command to run.";
        return false;
    }

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& arg : argv)
        cargv.push_back(const_cast<char*>(arg.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, cargv.front(), nullptr, nullptr, cargv.data(), *_NSGetEnviron());
    if (rc != 0) {
        error = std::string("Could not run ") + argv.front() + ": " + std::strerror(rc) + ".";
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    for (;;) {
        int raw = 0;
        const pid_t waited = ::waitpid(pid, &raw, WNOHANG);
        if (waited == pid) {
            if (WIFEXITED(raw)) {
                status = WEXITSTATUS(raw);
                return true;
            }
            error = argv.front() + " was stopped before it finished.";
            return false;
        }
        if (waited < 0 && errno != EINTR) {
            error = std::string("Could not wait for ") + argv.front() + ": " + std::strerror(errno) + ".";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &raw, 0);
            error = argv.front() + " took too long and was stopped.";
            return false;
        }
        const struct timespec pause = {0, 20 * 1000 * 1000};
        ::nanosleep(&pause, nullptr);
    }
}

bool spawn_unwaited(const std::vector<std::string>& argv, std::string& error)
{
    if (argv.empty()) {
        error = "There is no program to start.";
        return false;
    }

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& arg : argv)
        cargv.push_back(const_cast<char*>(arg.c_str()));
    cargv.push_back(nullptr);

    posix_spawnattr_t attr;
    ::posix_spawnattr_init(&attr);
    ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);

    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, cargv.front(), nullptr, &attr, cargv.data(), *_NSGetEnviron());
    ::posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        error = std::string("The new version could not be started: ") + std::strerror(rc) + ".";
        return false;
    }
    return true;
}

std::string shell_quoted(const std::string& text)
{
    std::string out = "'";
    for (const char c : text)
        out += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    out += "'";
    return out;
}

std::string apple_script_quoted(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out;
}

// Reads the step counter the elevated script writes. Returns 0 when the file is
// missing or does not hold a number.
size_t read_step_count(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
        return 0;
    long long value = 0;
    if (!(in >> value) || value < 0)
        return 0;
    return static_cast<size_t>(value);
}

// Move pairs of paths as the administrator, through one Finder-style prompt.
// The script records on disk how many moves finished, so a partly applied swap
// is never mistaken for a swap that did nothing. `completed` receives that count.
bool elevated_moves(const std::vector<std::pair<std::string, std::string>>& moves,
                    const std::string& marker,
                    size_t&            completed,
                    std::string&       error)
{
    completed = 0;
    if (moves.empty())
        return true;

    std::string script = "/bin/echo 0 > " + shell_quoted(marker);
    size_t      step   = 0;
    for (const auto& move : moves) {
        ++step;
        script += " && /bin/mv -f " + shell_quoted(move.first) + " " + shell_quoted(move.second);
        // Written right after the move, so a later failure still says how far it got.
        script += " && /bin/echo " + std::to_string(step) + " > " + shell_quoted(marker);
    }

    const std::vector<std::string> argv = {
        "/usr/bin/osascript",
        "-e",
        "do shell script \"" + apple_script_quoted(script) + "\" with administrator privileges",
    };
    int        status = -1;
    const bool ran    = spawn_and_wait(argv, 600, status, error);

    completed = read_step_count(marker);
    if (completed > moves.size())
        completed = moves.size();
    remove_path_quietly(marker);

    if (!ran)
        return false;
    if (status != 0) {
        if (completed == 0)
            error = "Administrator approval was not given.";
        else
            error = "The administrator step stopped after " + std::to_string(completed) + " of " +
                    std::to_string(moves.size()) + " moves.";
        return false;
    }
    if (completed < moves.size()) {
        error = "The administrator step reported success, but only " + std::to_string(completed) + " of " +
                std::to_string(moves.size()) + " moves could be confirmed.";
        return false;
    }
    return true;
}

std::string find_mounted_app(const std::string& mount)
{
    std::string fallback;
    boost::system::error_code ec;
    for (boost::filesystem::directory_iterator it((boost::filesystem::path(mount)), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".app") != 0)
            continue;
        if (name == "OrcaSlicer.app")
            return it->path().string();
        if (fallback.empty())
            fallback = it->path().string();
    }
    return fallback;
}

// Unmounts on every exit path, including the early failures.
struct MountGuard
{
    std::string mount;
    bool        attached = false;

    void release()
    {
        if (attached) {
            std::string ignored;
            int         status = 0;
            spawn_and_wait({"/usr/bin/hdiutil", "detach", mount}, 120, status, ignored);
            attached = false;
        }
        remove_path_quietly(mount);
    }

    ~MountGuard() { release(); }
};

} // namespace

bool apply_macos_install(const UpdateHandoff& handoff, const std::string& artifact_path, ProgressSink& sink, std::string& error)
{
    if (sink.cancelled && sink.cancelled()) {
        error = "cancelled";
        return false;
    }

    const std::string label = handoff.version_label.empty() ? std::string("the new version") : handoff.version_label;
    sink.say(Stage::Installing, 0, "Installing " + label);

    std::string bundle = handoff.install_root;
    if (bundle.size() > 1 && bundle.back() == '/')
        bundle.pop_back();
    if (bundle.empty()) {
        error = "The application location is not known, so the update cannot be installed.";
        return false;
    }

    const std::string pid    = std::to_string(static_cast<long long>(::getpid()));
    const std::string staged = bundle + ".staged-" + pid;
    const std::string backup = bundle + ".backup-" + pid;
    // Where the elevated script records how many of its moves finished.
    const std::string move_log = "/tmp/orca-updater-" + pid + ".moves";
    const std::string download_folder = boost::filesystem::path(artifact_path).parent_path().string();

    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(boost::filesystem::path(bundle), ec)) {
        error = "The installed application was not found at " + bundle + ".";
        return false;
    }
    if (!boost::filesystem::is_regular_file(boost::filesystem::path(artifact_path), ec)) {
        error = "The downloaded disk image is missing, so the update cannot be installed.";
        return false;
    }

    remove_path_quietly(staged);

    MountGuard guard;
    guard.mount = "/tmp/orca-updater-" + pid;
    boost::system::error_code mkdir_ec;
    boost::filesystem::create_directories(boost::filesystem::path(guard.mount), mkdir_ec);
    if (!boost::filesystem::is_directory(boost::filesystem::path(guard.mount), mkdir_ec)) {
        error = "The update could not be prepared: " + guard.mount + " could not be created.";
        return false;
    }

    int status = 0;
    if (!spawn_and_wait({"/usr/bin/hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", guard.mount, artifact_path},
                        300, status, error))
        return false;
    guard.attached = true;
    if (status != 0) {
        error = "The downloaded disk image could not be opened (hdiutil exit code " + std::to_string(status) + ").";
        return false;
    }

    const std::string mounted_app = find_mounted_app(guard.mount);
    if (mounted_app.empty()) {
        error = "The disk image does not contain an application.";
        return false;
    }

    // Check the new build before anything in the install is touched.
    sink.say(Stage::Installing, 10, "Verifying the signature");
    if (!spawn_and_wait({"/usr/bin/codesign", "--verify", "--deep", "--strict", mounted_app}, 300, status, error))
        return false;
    if (status != 0) {
        error = "The new version's signature could not be verified, so nothing was changed.";
        return false;
    }
    if (!spawn_and_wait({"/usr/sbin/spctl", "-a", "-t", "exec", mounted_app}, 300, status, error))
        return false;
    if (status != 0) {
        error = "macOS refused the new version, so nothing was changed.";
        return false;
    }

    sink.say(Stage::Installing, 40, "Preparing " + label);
    if (!spawn_and_wait({"/usr/bin/ditto", mounted_app, staged}, 900, status, error)) {
        remove_path_quietly(staged);
        return false;
    }
    if (status != 0) {
        remove_path_quietly(staged);
        error = "The new version could not be copied out of the disk image.";
        return false;
    }

    // Written before anything moves: the application restores the backup on its
    // next start if we die between "old moved aside" and "new in place".
    PendingInstall pending;
    pending.backup  = backup;
    pending.target  = bundle;
    pending.digest  = handoff.digest;
    pending.channel = handoff.channel;
    pending.version = handoff.version;
    if (!write_pending_install(pending)) {
        remove_path_quietly(staged);
        error = "The update could not be recorded, so nothing was changed.";
        return false;
    }

    bool moved_old  = false;
    bool placed_new = false;
    // True only when the previous bundle is demonstrably back at `bundle`.
    auto rollback = [&]() -> bool {
        boost::system::error_code ignore;
        bool                     restored = true;
        if (placed_new) {
            boost::filesystem::remove_all(boost::filesystem::path(bundle), ignore);
            if (ignore)
                restored = false;
            placed_new = false;
        }
        if (moved_old) {
            boost::filesystem::rename(boost::filesystem::path(backup), boost::filesystem::path(bundle), ignore);
            if (ignore)
                restored = false;
            else
                moved_old = false;
        }
        remove_path_quietly(staged);
        remove_path_quietly(updater_install_record_path());
        // A rename that reported success is not enough: the bundle has to be at
        // the target now, or the pending record must stay for the next start.
        if (restored && !boost::filesystem::is_directory(boost::filesystem::path(bundle), ignore))
            restored = false;
        if (restored) {
            // Nothing is left to recover: the pending record would only make the
            // next start look for a backup that is no longer there.
            remove_pending_install();
        }
        return restored;
    };

    // Appends what actually happened, so a failed rollback never claims the old
    // build is back.
    auto rollback_failure = [&](const std::string& message) {
        if (rollback())
            return message;
        return message + " The previous version could not be restored; the previous bundle is still at " + backup + ".";
    };

    sink.say(Stage::Installing, 70, "Replacing the application");

    ec.clear();
    boost::filesystem::rename(boost::filesystem::path(bundle), boost::filesystem::path(backup), ec);
    if (ec) {
        if (ec.value() != EACCES && ec.value() != EPERM) {
            error = rollback_failure("The current application could not be moved aside: " + ec.message() + ".");
            return false;
        }
        // A system-wide install: one administrator prompt does both moves, and
        // its result says which of them actually happened.
        size_t completed = 0;
        if (!elevated_moves({{bundle, backup}, {staged, bundle}}, move_log, completed, error)) {
            moved_old  = completed >= 1;
            placed_new = completed >= 2;
            error = rollback_failure(error + " The downloaded disk image is still in " + download_folder + ".");
            return false;
        }
        moved_old  = true;
        placed_new = true;
    } else {
        moved_old = true;
        ec.clear();
        boost::filesystem::rename(boost::filesystem::path(staged), boost::filesystem::path(bundle), ec);
        if (!ec) {
            placed_new = true;
        } else if (ec.value() == EACCES || ec.value() == EPERM) {
            size_t completed = 0;
            if (!elevated_moves({{staged, bundle}}, move_log, completed, error)) {
                placed_new = completed >= 1;
                error = rollback_failure(error + " The downloaded disk image is still in " + download_folder + ".");
                return false;
            }
            placed_new = true;
        } else {
            error = rollback_failure("The new version could not be put in place: " + ec.message() + ".");
            return false;
        }
    }

    guard.release();

    sink.say(Stage::Launching, 90, "Starting " + label);

    // Recorded before the launch: a crash while the new build starts still
    // leaves the application knowing what it is running.
    InstallRecord record;
    record.digest  = handoff.digest;
    record.channel = handoff.channel;
    record.version = handoff.version;
    write_install_record(record);

    if (!spawn_unwaited({"/usr/bin/open", "-n", bundle}, error)) {
        const bool restored = rollback();
        if (restored)
            error += " The previous version was put back.";
        else
            error += " The previous version could not be restored; the previous bundle is still at " + backup + ".";
        return false;
    }

    // Success: the backup and the pending record stay behind on purpose; the
    // application cleans both up on its next start.
    return true;
}

#else

bool apply_macos_install(const UpdateHandoff&, const std::string&, ProgressSink&, std::string& error)
{
    error = "macOS installs are not supported on this platform.";
    return false;
}

#endif

} // namespace Updater
} // namespace Slic3r
