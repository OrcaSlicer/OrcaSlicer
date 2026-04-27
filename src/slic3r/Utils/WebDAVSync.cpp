#include "WebDAVSync.hpp"
#include "Http.hpp"

#include <boost/log/trivial.hpp>
#include <boost/algorithm/string.hpp>

#include <expat.h>
#include <cstring>
#include <sstream>
#include <ctime>

#ifdef _WIN32
#include <time.h>
// Windows doesn't have strptime/timegm, provide alternatives
static char* portable_strptime(const char* s, const char* format, struct tm* tm) {
    std::istringstream ss(s);
    ss >> std::get_time(tm, format);
    if (ss.fail()) return nullptr;
    return const_cast<char*>(s + static_cast<size_t>(ss.tellg()));
}
static time_t portable_timegm(struct tm* tm) {
    return _mkgmtime(tm);
}
#define strptime portable_strptime
#define timegm portable_timegm
#endif

namespace Slic3r {

WebDAVSync::WebDAVSync(const WebDAVConfig& config)
    : m_config(config)
{}

std::string WebDAVSync::build_url(const std::string& relative_path) const
{
    std::string base = m_config.url;
    if (!base.empty() && base.back() != '/')
        base += '/';
    std::string rel = relative_path;
    if (!rel.empty() && rel.front() == '/')
        rel = rel.substr(1);
    return base + rel;
}

std::string WebDAVSync::extract_etag(const std::string& headers) const
{
    // Look for ETag header in response headers
    std::istringstream stream(headers);
    std::string line;
    while (std::getline(stream, line)) {
        if (boost::istarts_with(line, "etag:")) {
            std::string val = line.substr(5);
            boost::trim(val);
            // Remove surrounding quotes if present
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
                val = val.substr(1, val.size() - 2);
            return val;
        }
    }
    return {};
}

static long long parse_http_date_impl(const std::string& date_str)
{
    // Parse RFC 2822 / HTTP date: "Sun, 06 Nov 1994 08:49:37 GMT"
    struct tm tm = {};
    // Try common HTTP date format
    if (strptime(date_str.c_str(), "%a, %d %b %Y %H:%M:%S", &tm) != nullptr) {
        return static_cast<long long>(timegm(&tm));
    }
    // Try ISO 8601 format often used by some WebDAV servers
    if (strptime(date_str.c_str(), "%Y-%m-%dT%H:%M:%S", &tm) != nullptr) {
        return static_cast<long long>(timegm(&tm));
    }
    return 0;
}

long long WebDAVSync::parse_http_date(const std::string& date_str) const
{
    return parse_http_date_impl(date_str);
}

// Returns true if the namespace-qualified element name matches DAV:|local.
// With XML_ParserCreateNS(nullptr, '|'), expat delivers names as "DAV:|href" etc.
static bool is_dav(const char* name, const char* local)
{
    return strncmp(name, "DAV:|", 5) == 0 && strcmp(name + 5, local) == 0;
}

struct PropfindParserContext {
    std::vector<RemoteFileInfo>& results;

    bool            in_response{false};
    bool            in_resourcetype{false};
    RemoteFileInfo  current;
    std::string     char_data;

    enum Tag { NONE, HREF, LASTMOD, ETAG, CONTENTLEN };
    Tag current_tag{NONE};

    static void XMLCALL start_element(void* ud, const char* name, const char** /*atts*/)
    {
        auto* ctx = static_cast<PropfindParserContext*>(ud);

        if (is_dav(name, "response")) {
            ctx->in_response = true;
            ctx->current = RemoteFileInfo{};
        } else if (ctx->in_response) {
            if (is_dav(name, "href")) {
                ctx->current_tag = HREF;
                ctx->char_data.clear();
            } else if (is_dav(name, "getlastmodified")) {
                ctx->current_tag = LASTMOD;
                ctx->char_data.clear();
            } else if (is_dav(name, "getetag")) {
                ctx->current_tag = ETAG;
                ctx->char_data.clear();
            } else if (is_dav(name, "getcontentlength")) {
                ctx->current_tag = CONTENTLEN;
                ctx->char_data.clear();
            } else if (is_dav(name, "resourcetype")) {
                ctx->in_resourcetype = true;
            } else if (ctx->in_resourcetype && is_dav(name, "collection")) {
                ctx->current.is_directory = true;
            }
        }
    }

    static void XMLCALL end_element(void* ud, const char* name)
    {
        auto* ctx = static_cast<PropfindParserContext*>(ud);

        if (is_dav(name, "response")) {
            if (ctx->in_response)
                ctx->results.push_back(std::move(ctx->current));
            ctx->in_response = false;
        } else if (ctx->in_response) {
            if (is_dav(name, "resourcetype")) {
                ctx->in_resourcetype = false;
            } else if (is_dav(name, "href") && ctx->current_tag == HREF) {
                ctx->current.path = Http::url_decode(ctx->char_data);
                ctx->current_tag = NONE;
            } else if (is_dav(name, "getlastmodified") && ctx->current_tag == LASTMOD) {
                ctx->current.modified_time = parse_http_date_impl(ctx->char_data);
                ctx->current_tag = NONE;
            } else if (is_dav(name, "getetag") && ctx->current_tag == ETAG) {
                std::string& etag = ctx->char_data;
                if (etag.size() >= 2 && etag.front() == '"' && etag.back() == '"')
                    etag = etag.substr(1, etag.size() - 2);
                ctx->current.etag = std::move(etag);
                ctx->current_tag = NONE;
            } else if (is_dav(name, "getcontentlength") && ctx->current_tag == CONTENTLEN) {
                try { ctx->current.size = std::stoull(ctx->char_data); }
                catch (...) {}
                ctx->current_tag = NONE;
            }
        }
    }

    static void XMLCALL char_handler(void* ud, const XML_Char* s, int len)
    {
        auto* ctx = static_cast<PropfindParserContext*>(ud);
        if (ctx->current_tag != NONE)
            ctx->char_data.append(s, static_cast<size_t>(len));
    }
};

bool WebDAVSync::parse_propfind_response(const std::string& xml,
                                          const std::string& base_dir,
                                          std::vector<RemoteFileInfo>& out)
{
    XML_Parser parser = XML_ParserCreateNS(nullptr, '|');
    if (!parser)
        return false;

    std::vector<RemoteFileInfo> temp_results;
    PropfindParserContext ctx{temp_results};
    XML_SetUserData(parser, &ctx);
    XML_SetElementHandler(parser, PropfindParserContext::start_element,
                                   PropfindParserContext::end_element);
    XML_SetCharacterDataHandler(parser, PropfindParserContext::char_handler);

    bool ok = XML_Parse(parser, xml.c_str(), static_cast<int>(xml.size()), 1) != XML_STATUS_ERROR;
    if (!ok) {
        BOOST_LOG_TRIVIAL(error) << "WebDAV PROPFIND XML parse error: "
                                 << XML_ErrorString(XML_GetErrorCode(parser));
        out.clear();
    } else {
        out = std::move(temp_results);
    }
    XML_ParserFree(parser);
    return ok;
}

bool WebDAVSync::connect(std::string& error_out)
{
    BOOST_LOG_TRIVIAL(info) << "WebDAVSync: connecting to " << m_config.url;
    bool was_connected = m_connected;
    bool ok = test_connection(error_out);
    if (ok) {
        BOOST_LOG_TRIVIAL(info) << "WebDAVSync: connected successfully";
        if (!was_connected)
            m_first_connect = true;
    } else {
        BOOST_LOG_TRIVIAL(error) << "WebDAVSync: connection failed: " << error_out;
    }
    return ok;
}

std::string WebDAVSync::info_message() const
{
    if (m_first_connect)
        return "Connected to WebDAV server: " + m_config.url;
    return {};
}

void WebDAVSync::disconnect()
{
    m_connected = false;
}

bool WebDAVSync::is_connected() const
{
    return m_connected;
}

bool WebDAVSync::test_connection(std::string& error_out)
{
    std::string url = build_url("");
    bool        success = false;
    std::string err_msg;
    unsigned    status_code = 0;

    // Send PROPFIND with Depth: 0 to just check the root
    auto http = Http::propfind(url);
    http.header("Depth", "0")
        .header("Content-Type", "application/xml")
        .set_post_body(std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                        "<d:propfind xmlns:d=\"DAV:\">"
                        "<d:prop><d:resourcetype/></d:prop>"
                        "</d:propfind>"))
        .timeout_connect(m_config.timeout_connect)
        .timeout_max(m_config.timeout_max)
        .on_complete([&](std::string body, unsigned status) {
            status_code = status;
            success     = true;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            status_code = status;
            if (!error.empty())
                err_msg = error;
            else
                err_msg = "HTTP " + std::to_string(status);
        });

    if (!m_config.username.empty()) {
        if (m_config.use_digest_auth)
            http.auth_digest(m_config.username, m_config.password);
        else
            http.auth_basic(m_config.username, m_config.password);
    }

    http.perform_sync();

    if (success || status_code == 207) {
        m_connected = true;
        return true;
    }

    error_out = err_msg.empty() ? "Connection failed" : err_msg;
    m_connected = false;
    return false;
}

bool WebDAVSync::ensure_directory(const std::string& remote_path, std::string& error_out)
{
    BOOST_LOG_TRIVIAL(info) << "WebDAVSync: ensuring directory " << remote_path;
    // Create directories recursively
    std::vector<std::string> parts;
    std::string path = remote_path;
    if (!path.empty() && path.front() == '/')
        path = path.substr(1);
    if (!path.empty() && path.back() == '/')
        path.pop_back();

    boost::split(parts, path, boost::is_any_of("/"));

    std::string current;
    for (const auto& part : parts) {
        if (part.empty()) continue;
        current += "/" + part;

        std::string url = build_url(current + "/");
        bool        ok  = false;
        std::string err;
        unsigned    status = 0;

        auto http = Http::mkcol(url);
        http.timeout_connect(m_config.timeout_connect)
            .timeout_max(m_config.timeout_max)
            .on_complete([&](std::string, unsigned s) {
                status = s;
                ok     = true;
            })
            .on_error([&](std::string, std::string error, unsigned s) {
                status = s;
                err    = error;
                // 405 = already exists, 301 = redirect (already exists)
                if (s == 405 || s == 301)
                    ok = true;
            });

        if (!m_config.username.empty()) {
            if (m_config.use_digest_auth)
                http.auth_digest(m_config.username, m_config.password);
            else
                http.auth_basic(m_config.username, m_config.password);
        }

        http.perform_sync();

        if (!ok) {
            error_out = "Failed to create directory '" + current + "': " + err;
            BOOST_LOG_TRIVIAL(error) << "WebDAVSync: " << error_out;
            return false;
        }
    }

    return true;
}

bool WebDAVSync::list_files(const std::string& remote_dir,
                             std::vector<RemoteFileInfo>& out,
                             std::string& error_out)
{
    BOOST_LOG_TRIVIAL(info) << "WebDAVSync: listing files in " << remote_dir;
    std::string url = build_url(remote_dir);
    if (!url.empty() && url.back() != '/')
        url += '/';

    bool        success = false;
    std::string response_body;
    std::string err_msg;

    auto http = Http::propfind(url);
    http.header("Depth", "1")
        .header("Content-Type", "application/xml")
        .set_post_body(std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                        "<d:propfind xmlns:d=\"DAV:\">"
                        "<d:prop>"
                        "<d:getlastmodified/>"
                        "<d:getetag/>"
                        "<d:getcontentlength/>"
                        "<d:resourcetype/>"
                        "</d:prop>"
                        "</d:propfind>"))
        .timeout_connect(m_config.timeout_connect)
        .timeout_max(m_config.timeout_max)
        .on_complete([&](std::string body, unsigned status) {
            response_body = std::move(body);
            success       = true;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            if (status == 207) {
                // HTTP 207 Multi-Status is the correct PROPFIND response, but
                // the Http wrapper routes non-2xx codes through on_error.
                // This is expected behavior — do not remove this handler.
                response_body = std::move(body);
                success       = true;
            } else if (status == 404) {
                // Directory doesn't exist yet - not an error, just empty
                success = true;
            } else {
                err_msg = !error.empty() ? error : "HTTP " + std::to_string(status);
            }
        });

    if (!m_config.username.empty()) {
        if (m_config.use_digest_auth)
            http.auth_digest(m_config.username, m_config.password);
        else
            http.auth_basic(m_config.username, m_config.password);
    }

    http.perform_sync();

    if (!success) {
        error_out = err_msg;
        BOOST_LOG_TRIVIAL(error) << "WebDAVSync: list_files failed for " << remote_dir << ": " << err_msg;
        return false;
    }

    if (response_body.empty())
        return true; // Empty directory or doesn't exist

    if (!parse_propfind_response(response_body, remote_dir, out)) {
        error_out = "Failed to parse PROPFIND response";
        BOOST_LOG_TRIVIAL(error) << "WebDAVSync: " << error_out;
        return false;
    }

    // Remove the directory entry itself (first entry is usually the directory)
    if (!out.empty() && out.front().is_directory) {
        // Check if it's the queried directory itself
        std::string dir_path = remote_dir;
        if (!dir_path.empty() && dir_path.back() != '/')
            dir_path += '/';
        // The first entry's href usually ends with the directory path
        if (out.front().path.find(dir_path) != std::string::npos ||
            out.front().path == dir_path) {
            out.erase(out.begin());
        }
    }

    return true;
}

bool WebDAVSync::download_file(const std::string& remote_path,
                                std::string& content_out,
                                RemoteFileInfo& info_out,
                                std::string& error_out,
                                SyncError* error_code_out)
{
    BOOST_LOG_TRIVIAL(info) << "WebDAVSync: downloading " << remote_path;
    std::string url = build_url(remote_path);
    bool        success = false;
    std::string err_msg;
    std::string response_headers;
    unsigned    err_status = 0;

    auto http = Http::get(url);
    http.timeout_connect(m_config.timeout_connect)
        .timeout_max(m_config.timeout_max)
        .on_header_callback([&](std::string headers) {
            response_headers += headers;
        })
        .on_complete([&](std::string body, unsigned status) {
            content_out = std::move(body);
            success     = true;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            err_status = status;
            err_msg = !error.empty() ? error : "HTTP " + std::to_string(status);
        });

    if (!m_config.username.empty()) {
        if (m_config.use_digest_auth)
            http.auth_digest(m_config.username, m_config.password);
        else
            http.auth_basic(m_config.username, m_config.password);
    }

    http.perform_sync();

    if (!success) {
        error_out = err_msg;
        if (error_code_out) {
            if (err_status == 404)
                *error_code_out = SyncError::NotFound;
            else if (err_status == 401 || err_status == 403)
                *error_code_out = SyncError::AuthFailed;
            else
                *error_code_out = SyncError::Other;
        }
        BOOST_LOG_TRIVIAL(error) << "WebDAVSync: download failed for " << remote_path << ": " << err_msg;
        return false;
    }

    if (error_code_out)
        *error_code_out = SyncError::None;

    info_out.path = remote_path;
    info_out.etag = extract_etag(response_headers);
    info_out.size = content_out.size();

    // Parse Last-Modified header for conflict resolution timestamps
    {
        std::istringstream stream(response_headers);
        std::string line;
        while (std::getline(stream, line)) {
            if (boost::istarts_with(line, "last-modified:")) {
                std::string val = line.substr(14);
                boost::trim(val);
                info_out.modified_time = parse_http_date_impl(val);
                break;
            }
        }
    }

    return true;
}

bool WebDAVSync::upload_file(const std::string& remote_path,
                              const std::string& content,
                              const std::string& expected_etag,
                              std::string& new_etag_out,
                              std::string& error_out,
                              SyncError* error_code_out)
{
    BOOST_LOG_TRIVIAL(info) << "WebDAVSync: uploading " << remote_path << " (" << content.size() << " bytes)";
    std::string url = build_url(remote_path);
    bool        success = false;
    std::string err_msg;
    std::string response_headers;
    unsigned    resp_status = 0;

    auto http = Http::put(url);
    http.set_post_body(content)
        .header("Content-Type", "application/octet-stream")
        .timeout_connect(m_config.timeout_connect)
        .timeout_max(m_config.timeout_max)
        .on_header_callback([&](std::string headers) {
            response_headers += headers;
        })
        .on_complete([&](std::string body, unsigned status) {
            resp_status = status;
            success     = true;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            resp_status = status;
            err_msg     = !error.empty() ? error : "HTTP " + std::to_string(status);
        });

    // Conflict detection via If-Match
    if (!expected_etag.empty())
        http.header("If-Match", "\"" + expected_etag + "\"");

    if (!m_config.username.empty()) {
        if (m_config.use_digest_auth)
            http.auth_digest(m_config.username, m_config.password);
        else
            http.auth_basic(m_config.username, m_config.password);
    }

    http.perform_sync();

    if (resp_status == 412) {
        BOOST_LOG_TRIVIAL(warning) << "WebDAVSync: upload conflict (412) for " << remote_path;
        error_out = "CONFLICT";
        if (error_code_out) *error_code_out = SyncError::Conflict;
        return false;
    }

    if (!success) {
        error_out = err_msg;
        if (error_code_out) {
            if (resp_status == 401 || resp_status == 403)
                *error_code_out = SyncError::AuthFailed;
            else
                *error_code_out = SyncError::Other;
        }
        BOOST_LOG_TRIVIAL(error) << "WebDAVSync: upload failed for " << remote_path << ": " << err_msg;
        return false;
    }

    if (error_code_out) *error_code_out = SyncError::None;
    new_etag_out = extract_etag(response_headers);
    return true;
}

bool WebDAVSync::delete_file(const std::string& remote_path, std::string& error_out)
{
    BOOST_LOG_TRIVIAL(info) << "WebDAVSync: deleting " << remote_path;
    std::string url = build_url(remote_path);
    bool        success = false;
    std::string err_msg;

    auto http = Http::del(url);
    http.timeout_connect(m_config.timeout_connect)
        .timeout_max(m_config.timeout_max)
        .on_complete([&](std::string, unsigned) {
            success = true;
        })
        .on_error([&](std::string, std::string error, unsigned status) {
            if (status == 404)
                success = true; // Already deleted
            else
                err_msg = !error.empty() ? error : "HTTP " + std::to_string(status);
        });

    if (!m_config.username.empty()) {
        if (m_config.use_digest_auth)
            http.auth_digest(m_config.username, m_config.password);
        else
            http.auth_basic(m_config.username, m_config.password);
    }

    http.perform_sync();

    if (!success) {
        error_out = err_msg;
        BOOST_LOG_TRIVIAL(error) << "WebDAVSync: delete failed for " << remote_path << ": " << err_msg;
        return false;
    }
    return true;
}

} // namespace Slic3r
