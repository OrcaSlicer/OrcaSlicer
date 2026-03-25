#pragma once

#include "SyncBackend.hpp"
#include <string>
#include <mutex>

struct git_repository;
struct git_credential;

namespace Slic3r {

struct GitSyncConfig {
    std::string repo_url;
    std::string branch{"main"};
    std::string local_clone_path;
    std::string username;
    std::string token;
    std::string author_name{"OrcaSlicer"};
    std::string author_email{"orcaslicer@localhost"};
};

class GitSync : public SyncBackend {
public:
    explicit GitSync(const GitSyncConfig& config);
    ~GitSync() override;

    // Non-copyable
    GitSync(const GitSync&) = delete;
    GitSync& operator=(const GitSync&) = delete;

    bool connect(std::string& error_out) override;
    void disconnect() override;
    bool is_connected() const override;
    bool test_connection(std::string& error_out) override;

    bool ensure_directory(const std::string& remote_path, std::string& error_out) override;
    bool list_files(const std::string& remote_dir,
                    std::vector<RemoteFileInfo>& out,
                    std::string& error_out) override;
    bool download_file(const std::string& remote_path,
                       std::string& content_out,
                       RemoteFileInfo& info_out,
                       std::string& error_out,
                       SyncError* error_code_out = nullptr) override;
    bool upload_file(const std::string& remote_path,
                     const std::string& content,
                     const std::string& expected_etag,
                     std::string& new_etag_out,
                     std::string& error_out,
                     SyncError* error_code_out = nullptr) override;
    bool delete_file(const std::string& remote_path,
                     std::string& error_out) override;

    SyncBackendType type() const override { return SyncBackendType::Git; }
    std::string     display_name() const override { return "Git"; }
    std::string     info_message() const override;

    bool commit_and_push(const std::string& message, std::string& error_out);
    bool pull(std::string& error_out);

private:
    GitSyncConfig    m_config;
    git_repository*  m_repo{nullptr};
    bool             m_branch_created{false};

    std::string local_file_path(const std::string& relative_path) const;
    std::string blob_hash_for_file(const std::string& relative_path);

    bool clone_repo(std::string& error_out);
    bool open_repo(std::string& error_out);
    bool fetch_remote(std::string& error_out);
    bool merge_fetched(std::string& error_out);

    // Credential callback for libgit2
    static int credential_cb(git_credential** out, const char* url,
                             const char* username_from_url,
                             unsigned int allowed_types, void* payload);
};

// RAII guard for git_libgit2_init / git_libgit2_shutdown
class LibGit2Init {
public:
    static LibGit2Init& instance();
    LibGit2Init(const LibGit2Init&) = delete;
    LibGit2Init& operator=(const LibGit2Init&) = delete;
private:
    LibGit2Init();
    ~LibGit2Init();
};

} // namespace Slic3r
