#pragma once

#include "SyncBackend.hpp"
#include <atomic>
#include <string>

namespace Slic3r {

struct WebDAVConfig {
    std::string url;
    std::string username;
    std::string password;
    bool        use_digest_auth{false};
    long        timeout_connect{10};
    long        timeout_max{60};
};

class WebDAVSync : public SyncBackend {
public:
    explicit WebDAVSync(const WebDAVConfig& config);

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

    SyncBackendType type() const override { return SyncBackendType::WebDAV; }
    std::string     display_name() const override { return "WebDAV"; }
    std::string     info_message() const override;

    std::string extract_etag(const std::string& headers) const;
    bool        parse_propfind_response(const std::string& xml,
                                        const std::string& base_dir,
                                        std::vector<RemoteFileInfo>& out);
    long long   parse_http_date(const std::string& date_str) const;

private:
    WebDAVConfig      m_config;
    // Read from is_connected() which the GUI may call while a background sync
    // thread updates it in connect()/test_connection() -- keep it atomic.
    std::atomic<bool> m_connected{false};
    bool              m_first_connect{false};

    std::string build_url(const std::string& relative_path) const;

    // Best-effort PROPFIND Depth:0 for getetag, used to recover the ETag when a
    // server omits it from the PUT response (see upload_file). Returns "" if the
    // server does not expose one.
    std::string fetch_remote_etag(const std::string& remote_path);
};

} // namespace Slic3r
