#pragma once

#include "libslic3r/Preset.hpp"
#include <string>
#include <vector>

namespace Slic3r {

enum class SyncBackendType { WebDAV, Git };

enum class SyncError {
    None,
    NotFound,       // file doesn't exist on remote
    Conflict,       // ETag mismatch (concurrent update)
    AuthFailed,     // authentication error
    NetworkError,   // connectivity issue
    Other           // generic error
};

struct RemoteFileInfo {
    std::string path;
    std::string etag;
    long long   modified_time{0};
    size_t      size{0};
    bool        is_directory{false};
};

struct SyncConflict {
    std::string path;            // remote path (e.g. "presets/filament/Name.json")
    std::string local_filepath;  // absolute local file path
    std::string local_content;
    long long   local_time{0};
    std::string remote_content;
    long long   remote_time{0};
    std::string remote_etag;
    Preset::Type         preset_type{Preset::Type::TYPE_INVALID};
};

enum class ConflictResolution { KeepLocal, KeepRemote, Skip, Merge };

struct SyncConflictResult {
    ConflictResolution resolution{ConflictResolution::Skip};
    std::string        merged_content;  // populated only when resolution == Merge
};

class SyncBackend {
public:
    virtual ~SyncBackend() = default;

    virtual bool connect(std::string& error_out) = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
    virtual bool test_connection(std::string& error_out) = 0;

    virtual bool ensure_directory(const std::string& remote_path, std::string& error_out) = 0;

    virtual bool list_files(const std::string& remote_dir,
                            std::vector<RemoteFileInfo>& out,
                            std::string& error_out) = 0;

    virtual bool download_file(const std::string& remote_path,
                               std::string& content_out,
                               RemoteFileInfo& info_out,
                               std::string& error_out,
                               SyncError* error_code_out = nullptr) = 0;

    virtual bool upload_file(const std::string& remote_path,
                             const std::string& content,
                             const std::string& expected_etag,
                             std::string& new_etag_out,
                             std::string& error_out,
                             SyncError* error_code_out = nullptr) = 0;

    virtual bool delete_file(const std::string& remote_path,
                             std::string& error_out) = 0;

    virtual SyncBackendType type() const = 0;
    virtual std::string     display_name() const = 0;

    // Refresh remote state before a sync cycle (e.g. git pull).
    // Default is no-op (WebDAV fetches live data on each request).
    virtual bool refresh(std::string& error_out) { return true; }

    // Path prefix for sync files on this backend.
    virtual std::string     remote_prefix() const { return ""; }

    // Unique identifier for the sync target (used to invalidate state on config change).
    virtual std::string     fingerprint() const = 0;

    // Optional one-shot message produced after connect (e.g. "branch created").
    // Returns empty string when there is nothing to report.
    virtual std::string     info_message() const { return {}; }
};

} // namespace Slic3r
