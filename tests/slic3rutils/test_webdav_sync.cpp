#include <catch2/catch_all.hpp>

#include "slic3r/Utils/WebDAVSync.hpp"
#include "slic3r/Utils/ProfileSyncManager.hpp"

using namespace Slic3r;

// Helper: construct a WebDAVSync with dummy config for parser tests
static WebDAVSync make_dummy_sync()
{
    return WebDAVSync(WebDAVConfig{"http://localhost/dav", "user", "pass"});
}

// ============================================================
// parse_propfind_response
// ============================================================

TEST_CASE("PROPFIND parsing — Nextcloud (d: prefix)", "[WebDAV][ProfileSync]") {
    auto sync = make_dummy_sync();
    std::vector<RemoteFileInfo> results;

    std::string xml = R"(<?xml version="1.0"?>
<d:multistatus xmlns:d="DAV:">
  <d:response>
    <d:href>/remote.php/dav/files/user/sync/</d:href>
    <d:propstat>
      <d:prop>
        <d:resourcetype><d:collection/></d:resourcetype>
        <d:getlastmodified>Sat, 01 Jan 2024 12:00:00 GMT</d:getlastmodified>
        <d:getetag>"dir-etag"</d:getetag>
      </d:prop>
    </d:propstat>
  </d:response>
  <d:response>
    <d:href>/remote.php/dav/files/user/sync/preset.json</d:href>
    <d:propstat>
      <d:prop>
        <d:resourcetype/>
        <d:getlastmodified>Sun, 02 Jan 2024 15:30:00 GMT</d:getlastmodified>
        <d:getetag>"file-etag"</d:getetag>
        <d:getcontentlength>1234</d:getcontentlength>
      </d:prop>
    </d:propstat>
  </d:response>
</d:multistatus>)";

    REQUIRE(sync.parse_propfind_response(xml, "/sync", results));
    REQUIRE(results.size() == 2);

    // First entry — directory
    CHECK(results[0].is_directory);
    CHECK(results[0].etag == "dir-etag");
    CHECK(results[0].path.find("sync") != std::string::npos);

    // Second entry — file
    CHECK_FALSE(results[1].is_directory);
    CHECK(results[1].etag == "file-etag");
    CHECK(results[1].size == 1234);
    CHECK(results[1].path.find("preset.json") != std::string::npos);
    CHECK(results[1].modified_time > 0);
}

TEST_CASE("PROPFIND parsing — Apache mod_dav (lp1: prefix)", "[WebDAV][ProfileSync]") {
    auto sync = make_dummy_sync();
    std::vector<RemoteFileInfo> results;

    // Apache mod_dav uses lp1: as a namespace prefix for DAV: properties.
    // The old string-based parser could NOT handle this.
    std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<D:multistatus xmlns:D="DAV:" xmlns:lp1="DAV:">
  <D:response>
    <D:href>/webdav/config.json</D:href>
    <D:propstat>
      <D:prop>
        <lp1:resourcetype/>
        <lp1:getlastmodified>Mon, 15 Mar 2024 09:00:00 GMT</lp1:getlastmodified>
        <lp1:getetag>"apache-etag"</lp1:getetag>
        <lp1:getcontentlength>5678</lp1:getcontentlength>
      </D:prop>
    </D:propstat>
  </D:response>
</D:multistatus>)";

    REQUIRE(sync.parse_propfind_response(xml, "/webdav", results));
    REQUIRE(results.size() == 1);
    CHECK_FALSE(results[0].is_directory);
    CHECK(results[0].etag == "apache-etag");
    CHECK(results[0].size == 5678);
}

TEST_CASE("PROPFIND parsing — default namespace (no prefix)", "[WebDAV][ProfileSync]") {
    auto sync = make_dummy_sync();
    std::vector<RemoteFileInfo> results;

    std::string xml = R"(<?xml version="1.0"?>
<multistatus xmlns="DAV:">
  <response>
    <href>/dav/test.ini</href>
    <propstat>
      <prop>
        <resourcetype/>
        <getetag>"no-prefix-etag"</getetag>
        <getcontentlength>42</getcontentlength>
      </prop>
    </propstat>
  </response>
</multistatus>)";

    REQUIRE(sync.parse_propfind_response(xml, "/dav", results));
    REQUIRE(results.size() == 1);
    CHECK(results[0].etag == "no-prefix-etag");
    CHECK(results[0].size == 42);
}

TEST_CASE("PROPFIND parsing — XML entities in href", "[WebDAV][ProfileSync]") {
    auto sync = make_dummy_sync();
    std::vector<RemoteFileInfo> results;

    std::string xml = R"(<?xml version="1.0"?>
<d:multistatus xmlns:d="DAV:">
  <d:response>
    <d:href>/dav/file%20with%20spaces%26ampersand.json</d:href>
    <d:propstat>
      <d:prop>
        <d:resourcetype/>
        <d:getetag>"entity-etag"</d:getetag>
      </d:prop>
    </d:propstat>
  </d:response>
</d:multistatus>)";

    REQUIRE(sync.parse_propfind_response(xml, "/dav", results));
    REQUIRE(results.size() == 1);
    // URL-decoded: %20 → space, %26 → &
    CHECK(results[0].path.find("file with spaces&ampersand.json") != std::string::npos);
}

TEST_CASE("PROPFIND parsing — XML entity &amp; in href", "[WebDAV][ProfileSync]") {
    auto sync = make_dummy_sync();
    std::vector<RemoteFileInfo> results;

    // Some servers return href with XML entities instead of percent-encoding
    std::string xml = R"(<?xml version="1.0"?>
<d:multistatus xmlns:d="DAV:">
  <d:response>
    <d:href>/dav/A &amp; B.json</d:href>
    <d:propstat>
      <d:prop>
        <d:resourcetype/>
        <d:getetag>"amp-etag"</d:getetag>
      </d:prop>
    </d:propstat>
  </d:response>
</d:multistatus>)";

    REQUIRE(sync.parse_propfind_response(xml, "/dav", results));
    REQUIRE(results.size() == 1);
    // Expat decodes &amp; → &, then url_decode is a no-op (no percent-encoding)
    CHECK(results[0].path.find("A & B.json") != std::string::npos);
}

TEST_CASE("PROPFIND parsing — malformed XML", "[WebDAV][ProfileSync]") {
    auto sync = make_dummy_sync();
    std::vector<RemoteFileInfo> results;

    std::string xml = R"(<broken><xml>no closing tags)";

    CHECK_FALSE(sync.parse_propfind_response(xml, "/", results));
}

TEST_CASE("PROPFIND parsing — empty valid XML", "[WebDAV][ProfileSync]") {
    auto sync = make_dummy_sync();
    std::vector<RemoteFileInfo> results;

    std::string xml = R"(<?xml version="1.0"?>
<d:multistatus xmlns:d="DAV:">
</d:multistatus>)";

    REQUIRE(sync.parse_propfind_response(xml, "/", results));
    CHECK(results.empty());
}

// ============================================================
// parse_http_date
// ============================================================

TEST_CASE("HTTP date parsing — RFC 2822", "[WebDAV][ProfileSync]") {
    auto sync = make_dummy_sync();

    long long ts = sync.parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT");
    CHECK(ts > 0);
    // 1994-11-06 08:49:37 UTC = 784111777
    CHECK(ts == 784111777);
}

TEST_CASE("HTTP date parsing — ISO 8601", "[WebDAV][ProfileSync]") {
    auto sync = make_dummy_sync();

    long long ts = sync.parse_http_date("2024-01-01T00:00:00");
    CHECK(ts > 0);
    // 2024-01-01 00:00:00 UTC = 1704067200
    CHECK(ts == 1704067200);
}

TEST_CASE("HTTP date parsing — invalid string", "[WebDAV][ProfileSync]") {
    auto sync = make_dummy_sync();

    CHECK(sync.parse_http_date("not a date") == 0);
    CHECK(sync.parse_http_date("") == 0);
}

// ============================================================
// extract_etag
// ============================================================

TEST_CASE("ETag extraction — quoted", "[WebDAV][ProfileSync]") {
    auto sync = make_dummy_sync();

    std::string headers = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nETag: \"abc123\"\r\n";
    CHECK(sync.extract_etag(headers) == "abc123");
}

TEST_CASE("ETag extraction — unquoted", "[WebDAV][ProfileSync]") {
    auto sync = make_dummy_sync();

    std::string headers = "HTTP/1.1 200 OK\r\nETag: abc123\r\n";
    CHECK(sync.extract_etag(headers) == "abc123");
}

TEST_CASE("ETag extraction — case insensitive", "[WebDAV][ProfileSync]") {
    auto sync = make_dummy_sync();

    CHECK(sync.extract_etag("etag: \"lower\"\r\n") == "lower");
    CHECK(sync.extract_etag("ETAG: \"upper\"\r\n") == "upper");
    CHECK(sync.extract_etag("Etag: \"mixed\"\r\n") == "mixed");
}

TEST_CASE("ETag extraction — missing header", "[WebDAV][ProfileSync]") {
    auto sync = make_dummy_sync();

    CHECK(sync.extract_etag("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n").empty());
    CHECK(sync.extract_etag("").empty());
}

// ============================================================
// ProfileSyncManager validation
// ============================================================

TEST_CASE("ProfileSyncManager::start — disabled", "[ProfileSync]") {
    ProfileSyncManager mgr;
    SyncConfig cfg;
    cfg.enabled = false;
    mgr.set_config(cfg);

    std::string error;
    CHECK_FALSE(mgr.start(error));
    CHECK(error == "Sync is disabled");
}

TEST_CASE("ProfileSyncManager::start — WebDAV empty URL", "[ProfileSync]") {
    ProfileSyncManager mgr;
    SyncConfig cfg;
    cfg.enabled = true;
    cfg.backend_type = SyncBackendType::WebDAV;
    cfg.webdav_config.url = "";
    mgr.set_config(cfg);

    std::string error;
    CHECK_FALSE(mgr.start(error));
    CHECK(error == "WebDAV URL is not configured");
}

TEST_CASE("ProfileSyncManager::start — Git empty repo URL", "[ProfileSync]") {
    ProfileSyncManager mgr;
    SyncConfig cfg;
    cfg.enabled = true;
    cfg.backend_type = SyncBackendType::Git;
    cfg.git_config.repo_url = "";
    mgr.set_config(cfg);

    std::string error;
    CHECK_FALSE(mgr.start(error));
    CHECK(error == "Git repository URL is not configured");
}

TEST_CASE("ProfileSyncManager::start — read_only passes validation", "[ProfileSync]") {
    ProfileSyncManager mgr;
    SyncConfig cfg;
    cfg.enabled = true;
    cfg.read_only = true;
    cfg.backend_type = SyncBackendType::WebDAV;
    cfg.webdav_config.url = "http://example.com/dav";
    mgr.set_config(cfg);

    // start() should pass validation (read_only doesn't block it)
    // It will fail at connect() since there's no real server, but NOT at validation
    std::string error;
    CHECK_FALSE(mgr.start(error));
    CHECK(error.find("URL is not configured") == std::string::npos);
}
