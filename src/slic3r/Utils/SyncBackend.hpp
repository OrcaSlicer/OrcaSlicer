#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

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
    std::string path;
    std::string local_content;
    long long   local_time{0};
    std::string remote_content;
    long long   remote_time{0};
    std::string remote_etag;
    int         preset_type{0};     // Preset::Type for merge dialog context
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

    // Optional one-shot message produced after connect (e.g. "branch created").
    // Returns empty string when there is nothing to report.
    virtual std::string     info_message() const { return {}; }
};

} // namespace Slic3r
