#include "slic3r/UpdateHelper/InstallPlatform.hpp"
#include <slic3r/UpdateHelper/InstallApply.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#if defined(__linux__)

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/filesystem.hpp>

#include "libslic3r/UpdateHandoff.hpp"

#endif

namespace Slic3r {
namespace Updater {

#if defined(__linux__)

namespace {

std::string parent_dir_of(const std::string& path)
{
    std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return ".";
    if (slash == 0)
        return "/";
    return path.substr(0, slash);
}

bool is_writable_dir(const std::string& dir)
{
    return ::access(dir.c_str(), W_OK | X_OK) == 0;
}

bool sync_file(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    return synced;
}

} // namespace

// An AppImage is a single file: the install root is the file itself, so the
// swap is "move the running file aside, put the verified one in its place".
bool apply_linux_install(const UpdateHandoff& handoff, const std::string& artifact_path, ProgressSink& sink, std::string& error)
{
    if (sink.cancelled && sink.cancelled()) {
        error = "cancelled";
        return false;
    }

    const std::string label   = handoff.version_label.empty() ? std::string("the new version") : handoff.version_label;
    const std::string target  = handoff.install_root;
    const std::string pid     = std::to_string(static_cast<long long>(::getpid()));
    const std::string backup  = target + ".backup-" + pid;

    sink.say(Stage::Installing, 0, "Installing " + label);

    if (target.empty()) {
        error = "The AppImage location is not known, so the update cannot be installed.";
        return false;
    }

    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(boost::filesystem::path(target), ec)) {
        error = "The installed AppImage was not found at " + target + ".";
        return false;
    }
    if (!boost::filesystem::is_regular_file(boost::filesystem::path(artifact_path), ec)) {
        error = "The downloaded file is missing, so the update cannot be installed.";
        return false;
    }

    const std::string install_dir = parent_dir_of(target);
    if (!is_writable_dir(install_dir)) {
        error = "No permission to write to " + install_dir + ". Move the AppImage somewhere you can write to, or update it manually.";
        return false;
    }

    // Staged beside the target, so the final rename stays on one filesystem. The
    // artifact moves cheaply in the common case; a cache on another mount is copied
    // and synced first.
    const std::string staged = target + ".new-" + pid;
    boost::filesystem::rename(boost::filesystem::path(artifact_path), boost::filesystem::path(staged), ec);
    if (ec) {
        boost::system::error_code copy_ec;
        boost::filesystem::copy_file(boost::filesystem::path(artifact_path), boost::filesystem::path(staged),
                                     boost::filesystem::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            remove_path_quietly(staged);
            error = "The new AppImage could not be copied to " + install_dir + ": " + copy_ec.message() + ".";
            return false;
        }
        if (!sync_file(staged)) {
            remove_path_quietly(staged);
            error = "The new AppImage could not be written to " + install_dir + ".";
            return false;
        }
    }

    // vfat, exFAT and NTFS mounts refuse chmod but already report the file as executable.
    if (::chmod(staged.c_str(), 0755) != 0) {
        const int chmod_errno = errno;
        if (::access(staged.c_str(), X_OK) != 0) {
            remove_path_quietly(staged);
            error = "The new AppImage could not be made executable: " + std::string(std::strerror(chmod_errno)) + ".";
            return false;
        }
    }

    // Written before anything moves: if we die between "old moved aside" and
    // "new in place", the application restores the backup on its next start.
    PendingInstall pending;
    pending.backup  = backup;
    pending.target  = target;
    pending.digest  = handoff.digest;
    pending.channel = handoff.channel;
    pending.version = handoff.version;
    if (!write_pending_install(pending)) {
        remove_path_quietly(staged);
        error = "The update could not be recorded, so nothing was changed.";
        return false;
    }

    sink.say(Stage::Installing, 25, "Replacing " + boost::filesystem::path(target).filename().string());

    boost::filesystem::rename(boost::filesystem::path(target), boost::filesystem::path(backup), ec);
    if (ec) {
        remove_pending_install();
        remove_path_quietly(staged);
        error = "The current AppImage could not be moved aside: " + ec.message() + ".";
        return false;
    }

    // True only when the previous AppImage is back at target. The pending record
    // is kept otherwise, so the next start restores it.
    auto restore_previous = [&]() -> bool {
        boost::system::error_code restore_ec;
        boost::filesystem::rename(boost::filesystem::path(backup), boost::filesystem::path(target), restore_ec);
        if (restore_ec)
            return false;
        remove_pending_install();
        return true;
    };
    auto fail = [&](const std::string& message) -> bool {
        const bool restored = restore_previous();
        remove_path_quietly(staged);
        error = message + (restored ? " The previous version was put back."
                                    : " The previous version could not be restored; it is still at " + backup + ".");
        return false;
    };

    boost::filesystem::rename(boost::filesystem::path(staged), boost::filesystem::path(target), ec);
    if (ec)
        return fail("The new AppImage could not be put in place: " + ec.message() + ".");

    sink.say(Stage::Launching, 90, "Starting " + label);

    // Recorded before the launch: if this helper dies while the new build
    // starts, the application still knows what it is running.
    InstallRecord record;
    record.digest  = handoff.digest;
    record.channel = handoff.channel;
    record.version = handoff.version;
    if (!write_install_record(record))
        return fail("The update could not be recorded.");

    // Always the installed AppImage: handoff.exe_path points inside the running
    // mount and is gone as soon as the application exits.
    const std::string exe_path = target;
    std::string launch_error;
    if (!launch_detached(exe_path, launch_error)) {
        remove_path_quietly(updater_install_record_path());
        return fail(launch_error);
    }

    // Success: the backup and the pending record stay behind on purpose. Only
    // the application knows the new build actually started; it cleans both up
    // on its next start.
    return true;
}

#else

bool apply_linux_install(const UpdateHandoff&, const std::string&, ProgressSink&, std::string& error)
{
    error = "Linux installs are not supported on this platform.";
    return false;
}

#endif

} // namespace Updater
} // namespace Slic3r
