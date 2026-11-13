#ifndef slic3r_UpdateHandoff_hpp_
#define slic3r_UpdateHandoff_hpp_

// Handoff between the application and the updater helper, plus the small
// records they exchange through the updater cache directory:
//
//   handoff.json    app writes, helper reads once and deletes
//   installed.json  helper writes, app reads on its next start
//   pending.json    helper writes before swapping, app deletes after it starts
//   install.lock    app locks it to claim the install, helper takes it over once the app exits
//   downloads/      partial and verified artifacts
//   helper/         the helper, extracted so it can outlive the app
//   updater.log     helper log
//
// The cache lives in the OS cache directory, not in the data directory: it holds a
// download the user can lose. The helper never resolves it itself, because its environment
// can differ from the application's and resolve to a different place.
// `UpdateHandoff::cache_dir` is authoritative.

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
    std::string install_root;   // install dir or .app bundle
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
    std::string target;   // install dir or .app bundle the install wrote to
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
std::string updater_failure_path();

// Why an update did not install. Written by the helper, read once by the next start: an
// automatic update shows no dialog, so this is the only way the failure reaches the user.
bool write_update_failure(const std::string& message);
bool read_update_failure(std::string& message);
void remove_update_failure();

// Key of the app config entry recording one install's installed digest. The path is
// hashed because AppConfig keys cannot hold one: they reject whitespace.
std::string install_identity_key(const std::string& install_root);

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

// Only https downloads of `asset_name` from the OrcaSlicer release page, at
// releases/download/<tag>/<asset_name>, are fetched and installed.
bool is_release_download_url(const std::string& url, const std::string& asset_name);

std::string handoff_to_json(const UpdateHandoff& handoff);
bool        handoff_from_json(const std::string& json, UpdateHandoff& out);
bool        write_handoff(const UpdateHandoff& handoff);
bool        read_handoff(UpdateHandoff& out);
void        remove_handoff();

std::string install_record_to_json(const InstallRecord& record);
bool        install_record_from_json(const std::string& json, InstallRecord& out);
bool        write_install_record(const InstallRecord& record);
// Returns false when there is none. The record is left in place.
bool        read_install_record(InstallRecord& out);
void        remove_install_record();

std::string pending_install_to_json(const PendingInstall& pending);
bool        pending_install_from_json(const std::string& json, PendingInstall& out);
bool        write_pending_install(const PendingInstall& pending);
bool        read_pending_install(PendingInstall& out);
void        remove_pending_install();

// Takes an OS file lock on the install lock, which the holder keeps until it exits.
// False when another handle holds the lock.
bool acquire_install_lock(const std::string& path);
// Unlocks and deletes the file this process acquired. A no-op for any other path.
void release_install_lock(const std::string& path);

} // namespace Slic3r

#endif // slic3r_UpdateHandoff_hpp_
