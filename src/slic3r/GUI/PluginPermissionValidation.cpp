#include "PluginPermissionValidation.hpp"

#include "I18N.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>
#include <wx/string.h>

// PluginAuditManager pulls in the Python headers, so it is included after the wx and standard
// headers above.
#include "slic3r/plugin/PluginAuditManager.hpp"

namespace Slic3r { namespace GUI {
namespace {

PermissionPathStyle resolved_path_style(PermissionPathStyle style)
{
#ifdef _WIN32
    return style == PermissionPathStyle::Native ? PermissionPathStyle::Windows : style;
#else
    return style == PermissionPathStyle::Native ? PermissionPathStyle::Posix : style;
#endif
}

bool is_path_separator(char c, PermissionPathStyle style)
{
    if (style == PermissionPathStyle::Windows)
        return c == '/' || c == '\\';
    return c == '/';
}

// Absolute on the platform that will match the grant. A POSIX path must start at the root; a
// Windows path must name a drive and a root directory or be a UNC path. A Windows-style path is
// deliberately not accepted on POSIX: it is relative there, so the grant would follow the
// plugin's working directory instead of naming the folder the user selected.
bool is_absolute_permission_path(const std::string& value, PermissionPathStyle style)
{
    style = resolved_path_style(style);
    if (style == PermissionPathStyle::Windows) {
        if (value.size() >= 3 && std::isalpha(static_cast<unsigned char>(value[0])) && value[1] == ':' &&
            is_path_separator(value[2], style))
            return true;

        // A UNC path needs both a server and a share. "\\server" and "\\server\" are not absolute.
        if (value.size() > 2 && is_path_separator(value[0], style) && is_path_separator(value[1], style)) {
            size_t server_end = 2;
            while (server_end < value.size() && !is_path_separator(value[server_end], style))
                ++server_end;
            const size_t share_start = server_end + 1;
            if (server_end == value.size() || share_start == value.size())
                return false;
            size_t share_end = share_start;
            while (share_end < value.size() && !is_path_separator(value[share_end], style))
                ++share_end;
            return share_end > share_start;
        }
        return false;
    }
    return !value.empty() && value.front() == '/';
}

bool has_parent_path_component(const std::string& value, PermissionPathStyle style)
{
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = start;
        while (end < value.size() && !is_path_separator(value[end], style))
            ++end;
        if (end - start == 2 && value[start] == '.' && value[start + 1] == '.')
            return true;
        if (end == value.size())
            return false;
        start = end + 1;
    }
    return false;
}

// Ports are decimal only and must fit in 16 bits. Checking the accumulated value after every digit
// rejects oversized input before it can overflow, so an arbitrarily long digit run is refused
// rather than wrapping to an in-range value.
bool is_decimal_port(const std::string& port)
{
    if (port.empty())
        return false;
    unsigned value = 0;
    for (const unsigned char c : port) {
        if (std::isdigit(c) == 0)
            return false;
        value = value * 10 + static_cast<unsigned>(c - '0');
        if (value > 65535)
            return false;
    }
    return true;
}

// A bare IPv6 literal used as a socket host. The audit matcher accepts one, so the editor has to as
// well; the standard parser keeps a mistyped literal from being saved as if it were a host name. A
// port needs brackets around the literal and is handled by the caller.
bool is_ipv6_literal(const std::string& host)
{
    boost::system::error_code parse_error;
    const boost::asio::ip::address address = boost::asio::ip::make_address(host, parse_error);
    return !parse_error && address.is_v6();
}

// A host component of a socket target: a host name, an IPv4 address or a bare IPv6 literal. Only
// characters that would trail the host in an address, plus the tuple syntax, are refused, because
// the audit matcher reads the host up to exactly those characters.
bool is_socket_host(const std::string& host)
{
    if (host.empty())
        return false;
    if (host.find_first_of(" \t\r\n/?#@\\[]()'\",;*") != std::string::npos)
        return false;
    if (host.find(':') != std::string::npos)
        return is_ipv6_literal(host);
    return true;
}

// The address tuple older releases persisted for socket events: "('host', 443)" or
// "('host', 443, 0, 0)". This mirrors how the matcher reads the host (the first quoted string, or
// the text before the first comma when unquoted) and additionally checks the trailing address
// fields, so a malformed tuple cannot be saved as if it were a host grant.
bool parse_legacy_socket_tuple(const std::string& value, std::string& host)
{
    const size_t quote = value.find_first_of("'\"");
    const size_t comma = value.find(',');
    std::string rest;
    if (quote != std::string::npos && (comma == std::string::npos || quote < comma)) {
        const char delimiter = value[quote];
        const size_t close = value.find(delimiter, quote + 1);
        if (close == std::string::npos)
            return false;
        host = value.substr(quote + 1, close - quote - 1);
        rest = trim_permission_target(value.substr(close + 1));
    } else {
        if (comma == std::string::npos)
            return false;
        host = trim_permission_target(value.substr(1, comma - 1));
        rest = trim_permission_target(value.substr(comma));
    }

    if (!is_socket_host(host))
        return false;
    if (rest.empty() || rest.front() != ',')
        return false;
    rest.erase(0, 1);
    rest = trim_permission_target(rest);
    if (rest.empty() || rest.back() != ')')
        return false;
    rest.erase(rest.size() - 1);

    bool first_field = true;
    size_t start = 0;
    while (start <= rest.size()) {
        const size_t end = rest.find(',', start);
        const std::string field = trim_permission_target(rest.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (field.empty() || field.find_first_not_of("0123456789") != std::string::npos)
            return false;
        if (first_field) {
            if (!is_decimal_port(field))
                return false;
            first_field = false;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

// A socket target without a scheme. The stored form is kept as entered: the matcher compares socket
// grants by host, so canonicalizing a bare address into an http(s) URL would add an HTTP grant the
// plugin never had.
bool parse_socket_address(const std::string& value, std::string& host)
{
    if (value.front() == '(')
        return parse_legacy_socket_tuple(value, host);

    if (value.front() == '[') {
        const size_t close = value.find(']');
        if (close == std::string::npos || close == 1)
            return false;
        host = value.substr(1, close - 1);
        if (!is_ipv6_literal(host))
            return false;
        const std::string rest = value.substr(close + 1);
        return rest.empty() || (rest.front() == ':' && is_decimal_port(rest.substr(1)));
    }
    if (value.find_first_of("[]") != std::string::npos)
        return false;

    const size_t colon = value.find(':');
    if (colon == std::string::npos) {
        host = value;
        return is_socket_host(host);
    }
    if (value.find(':', colon + 1) != std::string::npos) {
        // More than one colon: only a bare IPv6 literal is left; a port needs brackets.
        host = value;
        return is_ipv6_literal(host);
    }
    host = value.substr(0, colon);
    return is_socket_host(host) && is_decimal_port(value.substr(colon + 1));
}

} // namespace

std::string trim_permission_target(const std::string& value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool normalize_filesystem_permission(const std::string& raw, std::string& normalized, wxString& error,
                                     PermissionPathStyle style)
{
    const PermissionPathStyle resolved = resolved_path_style(style);

    normalized = trim_permission_target(raw);
    if (normalized.empty()) {
        error = _L("A file system permission cannot be empty.");
        return false;
    }
    if (normalized.find('\0') != std::string::npos) {
        error = _L("A file system permission cannot contain a null character.");
        return false;
    }

    const size_t wildcard = normalized.find('*');
    if (wildcard != std::string::npos) {
        if (wildcard == 0 || wildcard != normalized.size() - 1 || !is_path_separator(normalized[wildcard - 1], resolved)) {
            error = _L("A file system permission may use \"*\" only as the final \"/*\" of a folder tree.");
            return false;
        }
    }
    if (has_parent_path_component(normalized, resolved)) {
        error = _L("A file system permission cannot contain a \"..\" path component.");
        return false;
    }
    if (!is_absolute_permission_path(normalized, resolved)) {
        error = _L("A file system permission must be an absolute path on this computer.");
        return false;
    }
    return true;
}

bool normalize_network_permission(const std::string& raw, std::string& normalized, wxString& error)
{
    const std::string value = trim_permission_target(raw);
    if (value.empty()) {
        error = _L("A network permission cannot be empty.");
        return false;
    }
    for (const unsigned char c : value) {
        if (c < 0x20 || c == 0x7f) {
            error = _L("A network permission cannot contain control characters.");
            return false;
        }
    }
    if (value.find('*') != std::string::npos) {
        error = _L("Network permissions do not support wildcards. Enter one full http:// or https:// URL.");
        return false;
    }

    const size_t scheme_end = value.find("://");
    if (scheme_end == std::string::npos || scheme_end == 0) {
        error = _L("Enter a full http:// or https:// URL with a host name or IP address. Bare domain names are not accepted.");
        return false;
    }

    std::string scheme = value.substr(0, scheme_end);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (scheme != "http" && scheme != "https") {
        error = _L("Only http:// and https:// URLs are supported for network permissions.");
        return false;
    }

    const size_t      authority_start = scheme_end + 3;
    const size_t      authority_end   = value.find_first_of("/?#", authority_start);
    const std::string authority       = value.substr(authority_start,
                                                     authority_end == std::string::npos ? std::string::npos : authority_end - authority_start);
    if (authority.empty()) {
        error = _L("Enter a full http:// or https:// URL with a host name or IP address.");
        return false;
    }

    if (authority.find('@') != std::string::npos) {
        error = _L("Network permissions cannot include a username or password. Enter a URL with only a scheme and host.");
        return false;
    }
    if (authority.find('\\') != std::string::npos) {
        error = _L("Enter a full http:// or https:// URL with a host name or IP address.");
        return false;
    }

    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos || close == 1) {
            error = _L("Enter a valid bracketed IPv6 address.");
            return false;
        }
        const std::string rest = authority.substr(close + 1);
        if (!rest.empty() && (rest.front() != ':' || !is_decimal_port(rest.substr(1)))) {
            error = _L("Enter a full http:// or https:// URL with a host name or IP address.");
            return false;
        }
    } else {
        if (authority.find_first_of("[]") != std::string::npos) {
            error = _L("Enter a full http:// or https:// URL with a host name or IP address.");
            return false;
        }
        const size_t colon = authority.find(':');
        if (colon != std::string::npos) {
            if (colon == 0 || authority.find(':', colon + 1) != std::string::npos || !is_decimal_port(authority.substr(colon + 1))) {
                error = _L("Enter a full http:// or https:// URL with a host name or IP address.");
                return false;
            }
        }
    }

    normalized = PluginAuditManager::normalize_network_url(value);
    if (normalized.empty()) {
        error = _L("Enter a full http:// or https:// URL with a host name or IP address.");
        return false;
    }
    return true;
}

bool normalize_socket_permission(const std::string& raw, std::string& normalized, wxString& error)
{
    const std::string value = trim_permission_target(raw);
    if (value.empty()) {
        error = _L("A network permission cannot be empty.");
        return false;
    }
    for (const unsigned char c : value) {
        if (c < 0x20 || c == 0x7f) {
            error = _L("A network permission cannot contain control characters.");
            return false;
        }
    }
    if (value.find('*') != std::string::npos) {
        error = _L("Network permissions do not support wildcards.");
        return false;
    }

    // A full URL keeps the existing HTTP(S) rules: its authority is checked the same way and the
    // saved grant is reduced to scheme://host.
    if (value.find("://") != std::string::npos)
        return normalize_network_permission(value, normalized, error);

    std::string host;
    if (!parse_socket_address(value, host)) {
        error = _L("Enter a host name or IP address, optionally with a port, or a full http:// or https:// URL.");
        return false;
    }
    // Keep the saved shape: a bare host, address with port or legacy tuple stays as it is, so the
    // edit cannot turn a socket-only grant into an HTTP one.
    normalized = value;
    return true;
}

}} // namespace Slic3r::GUI
