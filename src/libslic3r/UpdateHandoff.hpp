#ifndef slic3r_UpdateHandoff_hpp_
#define slic3r_UpdateHandoff_hpp_

// Handoff between the application and the updater helper, plus the small
// records they exchange through the updater cache directory:
//
//   handoff.json    app writes, helper reads once and deletes
//   installed.json  helper writes, app reads on its next start
//   pending.json    helper writes before swapping, app deletes after it starts
//   install.lock    app creates to claim the install, helper releases
//   downloads/      partial and verified artifacts
//   helper/         the helper, extracted so it can outlive the app
//   updater.log     helper log
//
// The cache lives in the OS cache directory, not in the data directory: it holds a
// download the user can lose. The helper never resolves it itself, because when it runs
// from a portable Windows install or from an AppImage it would resolve to a different
// place than the application does. `UpdateHandoff::cache_dir` is authoritative.

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

struct UpdateHandoff
{
    std::string channel;        // "stable" | "nightly"
    std::string version;        // "2.5.0", or the build label for nightlies
    std::string version_label;  // what the helper window shows
    std::string asset_name;
    std::string url;
    std::string digest;         // "sha256:<hex>"
    int64_t     size = 0;
    std::string flavour;        // install_flavour_key()
    std::string install_root;   // install dir, .app bundle, or AppImage file
    std::string exe_path;       // binary to launch when done
    std::string cache_dir;      // updater cache, resolved by the application
    int64_t     parent_pid = 0; // wait for this process to exit before installing
    std::string log_path;
    // Set when the application already took the install lock.
    std::string lock_path;
};

// Recorded by the helper once an install has been laid down, read by the
// application on its next start so that the next check knows what is running.
struct InstallRecord
{
    std::string digest;   // the installed artifact's digest
    std::string channel;  // channel it came from
    std::string version;
};

// Written before the swap, so that a crash between "old moved aside" and "new
// in place" can be recovered, and so the next start knows what to clean up.
struct PendingInstall
{
    std::string backup;   // the previous build, moved aside
    std::string target;   // where it should be
    std::string digest;
    std::string channel;
    std::string version;
    // Windows portable: the user's data_dir, moved aside while the trees swap.
    std::string data_aside;
};

std::string updater_cache_dir();  // the OS cache dir, <cache>/OrcaSlicer/updater
// Overrides the base directory above. The helper has no data directory of its own,
// so it is pointed at the cache the application resolved through the handoff.
void        set_updater_cache_dir(const std::string& dir);
void        clear_updater_cache_dir();
std::string updater_download_dir();
std::string updater_helper_dir();
std::string updater_handoff_path();
std::string updater_install_record_path();
std::string updater_pending_path();
std::string updater_lock_path();
std::string updater_log_path();

// The state of a partial download in `dir` for `asset_name`, after the directory has been
// reduced to it. The artifact's digest is recorded beside the .part, so a later run only
// resumes a download started for that same build: a nightly reuses one asset name and an
// appended-to .part could never verify.
struct DownloadPrep
{
    // The .part belongs to this build and can be resumed.
    bool resumable = false;
    // It is already as large as the release says, so it only needs verifying.
    bool complete = false;
    // Paths removed from the directory, so the caller can log them.
    std::vector<std::string> dropped;
};

DownloadPrep prepare_download_dir(const std::string& dir,
                                  const std::string& asset_name,
                                  const std::string& digest,
                                  int64_t            expected_size);

std::string handoff_to_json(const UpdateHandoff& handoff);
bool        handoff_from_json(const std::string& json, UpdateHandoff& out);
bool        write_handoff(const UpdateHandoff& handoff);
bool        read_handoff(UpdateHandoff& out);
void        remove_handoff();

std::string install_record_to_json(const InstallRecord& record);
bool        install_record_from_json(const std::string& json, InstallRecord& out);
bool        write_install_record(const InstallRecord& record);
// Reads and deletes the record. Returns false when there is none.
bool        take_install_record(InstallRecord& out);

std::string pending_install_to_json(const PendingInstall& pending);
bool        pending_install_from_json(const std::string& json, PendingInstall& out);
bool        write_pending_install(const PendingInstall& pending);
bool        read_pending_install(PendingInstall& out);
void        remove_pending_install();

// Creates the install lock holding this process id. False when another install
// already holds it and its owner is still alive (a stale lock is taken over).
bool acquire_install_lock(const std::string& path, int64_t owner_pid);
void release_install_lock(const std::string& path);

} // namespace Slic3r

#endif // slic3r_UpdateHandoff_hpp_
