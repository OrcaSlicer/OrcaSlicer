#include "slic3r/UpdateHelper/InstallPlatform.hpp"
#include <libslic3r/UpdateHandoff.hpp>
#include <slic3r/UpdateHelper/InstallApply.hpp>

#if defined(__APPLE__)

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
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

bool path_exists(const std::string& path)
{
    boost::system::error_code ec;
    return boost::filesystem::exists(boost::filesystem::path(path), ec);
}

// Move pairs of paths as the administrator, through one Finder-style prompt.
// Returns true only when the script exits 0; callers read the filesystem for how
// far a failed run got.
bool elevated_moves(const std::vector<std::pair<std::string, std::string>>& moves, std::string& error)
{
    std::string script;
    for (const auto& move : moves) {
        if (!script.empty())
            script += " && ";
        script += "/bin/mv -f " + shell_quoted(move.first) + " " + shell_quoted(move.second);
    }

    const std::vector<std::string> argv = {
        "/usr/bin/osascript",
        "-e",
        "do shell script \"" + apple_script_quoted(script) + "\" with administrator privileges",
    };
    int status = -1;
    if (!spawn_and_wait(argv, 600, status, error))
        return false;
    if (status != 0) {
        error = "The administrator step did not complete.";
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
            const bool  detached = spawn_and_wait({"/usr/bin/hdiutil", "detach", mount}, 120, status, ignored) && status == 0;
            // Still mounted: leave the mount point alone, the image is not ours to delete from.
            if (!detached)
                return;
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
    if (status != 0 || !path_exists(staged)) {
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
    bool elevated   = false;
    // Undoes the moves that happened. The new tree is moved aside to `staged`, not
    // deleted, so a failed restore still leaves it on disk. True only when the
    // previous bundle is demonstrably back at `bundle`.
    auto rollback = [&]() -> bool {
        std::vector<std::pair<std::string, std::string>> undo;
        if (placed_new)
            undo.push_back({bundle, staged});
        if (moved_old)
            undo.push_back({backup, bundle});

        bool restored = true;
        if (elevated) {
            std::string ignored;
            restored = elevated_moves(undo, ignored);
        } else {
            for (const auto& move : undo) {
                boost::system::error_code move_ec;
                boost::filesystem::rename(boost::filesystem::path(move.first), boost::filesystem::path(move.second), move_ec);
                if (move_ec) {
                    restored = false;
                    break;
                }
            }
        }
        if (restored) {
            boost::system::error_code dir_ec;
            restored = boost::filesystem::is_directory(boost::filesystem::path(bundle), dir_ec) && !path_exists(backup);
        }

        remove_path_quietly(updater_install_record_path());
        if (restored) {
            remove_path_quietly(staged);
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
            return message + " The previous version was put back.";
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
        // A system-wide install: one administrator prompt does both moves. After a
        // failed run, what happened is read from which paths still exist.
        elevated = true;
        if (!elevated_moves({{bundle, backup}, {staged, bundle}}, error)) {
            moved_old  = path_exists(backup);
            placed_new = !path_exists(staged);
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
            elevated = true;
            if (!elevated_moves({{staged, bundle}}, error)) {
                placed_new = !path_exists(staged);
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
    if (!write_install_record(record)) {
        error = rollback_failure("The update could not be recorded.");
        return false;
    }

    if (!spawn_unwaited({"/usr/bin/open", "-n", bundle}, error)) {
        error = rollback_failure(error);
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
