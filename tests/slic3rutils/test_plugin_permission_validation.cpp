#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>

#include <slic3r/GUI/PluginPermissionValidation.hpp>

#include <string>

#include <wx/string.h>

using Slic3r::GUI::normalize_filesystem_permission;
using Slic3r::GUI::normalize_network_permission;
using Slic3r::GUI::PermissionPathStyle;

namespace {

struct ValidationResult
{
    bool        valid;
    std::string normalized;
};

ValidationResult validate_filesystem(const std::string& raw, PermissionPathStyle style)
{
    std::string normalized;
    wxString    error;
    const bool  valid = normalize_filesystem_permission(raw, normalized, error, style);
    return {valid, normalized};
}

ValidationResult validate_network(const std::string& raw)
{
    std::string normalized;
    wxString    error;
    const bool  valid = normalize_network_permission(raw, normalized, error);
    return {valid, normalized};
}

} // namespace

TEST_CASE("filesystem permission accepts absolute roots and the final tree wildcard", "[PluginPermissions]")
{
    SECTION("POSIX rules") {
        CHECK(validate_filesystem("/", PermissionPathStyle::Posix).valid);
        CHECK(validate_filesystem("/tmp", PermissionPathStyle::Posix).valid);
        CHECK(validate_filesystem("  /tmp/dir/*  ", PermissionPathStyle::Posix).normalized == "/tmp/dir/*");
        CHECK(validate_filesystem("/*", PermissionPathStyle::Posix).valid);
    }

    SECTION("Windows rules") {
        CHECK(validate_filesystem(R"(C:\tmp)", PermissionPathStyle::Windows).valid);
        CHECK(validate_filesystem("c:/tmp/dir", PermissionPathStyle::Windows).valid);
        CHECK(validate_filesystem(R"(\\server\share)", PermissionPathStyle::Windows).valid);
        CHECK(validate_filesystem(R"(\\server\share\*)", PermissionPathStyle::Windows).valid);
        CHECK(validate_filesystem(R"(C:\tmp\*)", PermissionPathStyle::Windows).valid);
        CHECK(validate_filesystem(R"(C:\tmp/*)", PermissionPathStyle::Windows).valid);
    }
}

TEST_CASE("filesystem permission refuses a UNC path without a share name", "[PluginPermissions]")
{
    CHECK_FALSE(validate_filesystem(R"(\\server)", PermissionPathStyle::Windows).valid);
    CHECK_FALSE(validate_filesystem(R"(\\server\)", PermissionPathStyle::Windows).valid);
    CHECK_FALSE(validate_filesystem("  \\\\server  ", PermissionPathStyle::Windows).valid);
    CHECK_FALSE(validate_filesystem(R"(\server\share)", PermissionPathStyle::Windows).valid);

    CHECK(validate_filesystem(R"(\\server\share)", PermissionPathStyle::Windows).valid);
}

TEST_CASE("filesystem permission refuses a wildcard that is not the final tree marker", "[PluginPermissions]")
{
    const std::string posix_wildcard = GENERATE(
        std::string("*"),
        std::string("/tmp/*/logs"),
        std::string("/tmp/*logs"),
        std::string("/tmp/log*"),
        std::string("/tmp/a*b"),
        std::string("/tmp/*/"));
    CHECK_FALSE(validate_filesystem(posix_wildcard, PermissionPathStyle::Posix).valid);

    const std::string windows_wildcard = GENERATE(
        std::string(R"(C:\tmp\*\)"),
        std::string(R"(C:\tmp\*.txt)"),
        std::string(R"(C:\tmp*)"),
        std::string(R"(C:\tmp\a*b)"));
    CHECK_FALSE(validate_filesystem(windows_wildcard, PermissionPathStyle::Windows).valid);
}

TEST_CASE("filesystem permission refuses a parent directory component", "[PluginPermissions]")
{
    const std::string posix_parent = GENERATE(
        std::string("/.."),
        std::string("/tmp/.."),
        std::string("/tmp/../etc"),
        std::string("/tmp/dir/../file"));
    CHECK_FALSE(validate_filesystem(posix_parent, PermissionPathStyle::Posix).valid);

    const std::string windows_parent = GENERATE(
        std::string(R"(C:\..)"),
        std::string(R"(C:\tmp\..)"),
        std::string(R"(C:\tmp\..\etc)"));
    CHECK_FALSE(validate_filesystem(windows_parent, PermissionPathStyle::Windows).valid);

    CHECK(validate_filesystem("/tmp/...", PermissionPathStyle::Posix).valid);
    CHECK(validate_filesystem("/tmp/a..b", PermissionPathStyle::Posix).valid);
    CHECK(validate_filesystem(R"(C:\tmp\...)", PermissionPathStyle::Windows).valid);
    CHECK(validate_filesystem(R"(C:\tmp\a..b)", PermissionPathStyle::Windows).valid);
}

TEST_CASE("filesystem permission refuses empty, relative and foreign-style paths", "[PluginPermissions]")
{
    CHECK_FALSE(validate_filesystem("", PermissionPathStyle::Posix).valid);
    CHECK_FALSE(validate_filesystem("   ", PermissionPathStyle::Posix).valid);
    CHECK_FALSE(validate_filesystem("tmp/dir", PermissionPathStyle::Posix).valid);
    CHECK_FALSE(validate_filesystem(R"(C:\tmp)", PermissionPathStyle::Posix).valid);

    CHECK_FALSE(validate_filesystem("tmp\\dir", PermissionPathStyle::Windows).valid);
    CHECK_FALSE(validate_filesystem(R"(C:tmp)", PermissionPathStyle::Windows).valid);

    std::string with_null = "/tmp/";
    with_null.push_back('\0');
    CHECK_FALSE(validate_filesystem(with_null, PermissionPathStyle::Posix).valid);
}

TEST_CASE("network permission stores the canonical host of a bracketed IPv6 URL", "[PluginPermissions]")
{
    ValidationResult result = validate_network("http://[2001:DB8::1]:8080/path?q=1");
    CHECK(result.valid);
    CHECK(result.normalized == "http://[2001:db8::1]");

    result = validate_network("HTTPS://[::1]:443");
    CHECK(result.valid);
    CHECK(result.normalized == "https://[::1]");

    result = validate_network("http://[::1]");
    CHECK(result.valid);
    CHECK(result.normalized == "http://[::1]");
}

TEST_CASE("network permission stores only the scheme and canonical host", "[PluginPermissions]")
{
    ValidationResult result = validate_network("https://Example.COM:8443/a/b?q=1#frag");
    CHECK(result.valid);
    CHECK(result.normalized == "https://example.com");

    result = validate_network("  http://example.com/path  ");
    CHECK(result.valid);
    CHECK(result.normalized == "http://example.com");

    result = validate_network("http://example.com");
    CHECK(result.valid);
    CHECK(result.normalized == "http://example.com");
}

TEST_CASE("network permission accepts ports up to 65535", "[PluginPermissions]")
{
    ValidationResult result = validate_network("http://example.com:65535");
    CHECK(result.valid);
    CHECK(result.normalized == "http://example.com");

    result = validate_network("http://[::1]:65535");
    CHECK(result.valid);
    CHECK(result.normalized == "http://[::1]");

    result = validate_network("http://example.com:0");
    CHECK(result.valid);
    CHECK(result.normalized == "http://example.com");

    result = validate_network("http://[::1]:0");
    CHECK(result.valid);
    CHECK(result.normalized == "http://[::1]");
}

TEST_CASE("network permission refuses userinfo, bad ports, malformed brackets and other schemes", "[PluginPermissions]")
{
    const std::string invalid = GENERATE(
        std::string("http://user@example.com"),
        std::string("http://user:secret@example.com"),
        std::string("https://@example.com"),
        std::string("http://example.com:"),
        std::string("http://example.com:abc"),
        std::string("http://example.com:80x"),
        std::string("http://example.com:80:90"),
        std::string("http://example.com:65536"),
        std::string("http://example.com:999999999999999999999999999999"),
        std::string("http://[::1]:65536"),
        std::string("http://[::1]:999999999999999999999999999999"),
        std::string("http://:80"),
        std::string("http://[::1"),
        std::string("http://[::1]x"),
        std::string("http://[::1]:"),
        std::string("http://[::1]:abc"),
        std::string("http://[]"),
        std::string("http://::1"),
        std::string("http://[::1]]"),
        std::string("http://example.com\\path"),
        std::string("ftp://example.com"),
        std::string("file:///tmp/plugin.py"),
        std::string("example.com"),
        std::string("http://"),
        std::string("http:///path"),
        std::string("http://*.example.com"),
        std::string("http://exa\nmple.com"),
        std::string(""));
    CHECK_FALSE(validate_network(invalid).valid);
}

TEST_CASE("permission rejections report a reason", "[PluginPermissions]")
{
    std::string normalized;
    wxString    error;

    CHECK_FALSE(normalize_filesystem_permission("relative/path", normalized, error, PermissionPathStyle::Posix));
    CHECK_FALSE(error.empty());

    error.clear();
    CHECK_FALSE(normalize_network_permission("ftp://example.com", normalized, error));
    CHECK_FALSE(error.empty());
}
