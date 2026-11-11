#include "slic3r/UpdateHelper/InstallPlatform.hpp"
#include <slic3r/UpdateHelper/InstallApply.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#if defined(__linux__)

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

#include <boost/filesystem.hpp>

#include "libslic3r/UpdateHandoff.hpp"

namespace Slic3r {
namespace Updater {

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

    // Written before anything moves: if we die between "old moved aside" and
    // "new in place", the application restores the backup on its next start.
    PendingInstall pending;
    pending.backup  = backup;
    pending.target  = target;
    pending.digest  = handoff.digest;
    pending.channel = handoff.channel;
    pending.version = handoff.version;
    if (!write_pending_install(pending)) {
        error = "The update could not be recorded, so nothing was changed.";
        return false;
    }

    sink.say(Stage::Installing, 25, "Replacing " + boost::filesystem::path(target).filename().string());

    boost::filesystem::rename(boost::filesystem::path(target), boost::filesystem::path(backup), ec);
    if (ec) {
        remove_pending_install();
        error = "The current AppImage could not be moved aside: " + ec.message() + ".";
        return false;
    }

    // Same filesystem in the common case, so the artifact moves cheaply; a
    // cache on another mount falls back to a copy.
    boost::filesystem::rename(boost::filesystem::path(artifact_path), boost::filesystem::path(target), ec);
    if (ec) {
        boost::system::error_code copy_ec;
        boost::filesystem::copy_file(boost::filesystem::path(artifact_path), boost::filesystem::path(target),
                                     boost::filesystem::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            boost::filesystem::rename(boost::filesystem::path(backup), boost::filesystem::path(target), ec);
            remove_pending_install();
            error = "The new AppImage could not be written to " + target + ": " + copy_ec.message() + ".";
            return false;
        }
    }

    if (::chmod(target.c_str(), 0755) != 0) {
        boost::system::error_code ignore;
        boost::filesystem::remove(boost::filesystem::path(target), ignore);
        boost::filesystem::rename(boost::filesystem::path(backup), boost::filesystem::path(target), ignore);
        remove_pending_install();
        error = "The new AppImage could not be made executable: " + std::string(std::strerror(errno)) + ".";
        return false;
    }

    sink.say(Stage::Launching, 90, "Starting " + label);

    // Recorded before the launch: if this helper dies while the new build
    // starts, the application still knows what it is running.
    InstallRecord record;
    record.digest  = handoff.digest;
    record.channel = handoff.channel;
    record.version = handoff.version;
    write_install_record(record);

    // Always the installed AppImage: handoff.exe_path points inside the running
    // mount and is gone as soon as the application exits.
    const std::string exe_path = target;
    std::string launch_error;
    if (!launch_detached(exe_path, launch_error)) {
        boost::system::error_code ignore;
        remove_path_quietly(updater_install_record_path());
        boost::filesystem::remove(boost::filesystem::path(target), ignore);
        boost::filesystem::rename(boost::filesystem::path(backup), boost::filesystem::path(target), ignore);
        remove_pending_install();
        error = launch_error + " The previous version was put back.";
        return false;
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
