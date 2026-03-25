#pragma once

#include "SyncBackend.hpp"
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
    std::string     remote_prefix() const override { return "orcaslicer-sync/"; }
    std::string     fingerprint() const override { return "webdav:" + m_config.url; }
    std::string     info_message() const override;

    std::string extract_etag(const std::string& headers) const;
    bool        parse_propfind_response(const std::string& xml,
                                        const std::string& base_dir,
                                        std::vector<RemoteFileInfo>& out);
    long long   parse_http_date(const std::string& date_str) const;

private:
    WebDAVConfig m_config;
    bool         m_connected{false};
    bool         m_first_connect{false};

    std::string build_url(const std::string& relative_path) const;
};

} // namespace Slic3r
