#include "PluginPermissionValidation.hpp"

#include "I18N.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

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

    // Ports are decimal only and must fit in 16 bits. Checking the accumulated value after every
    // digit rejects oversized input before it can overflow, so an arbitrarily long digit run is
    // refused rather than wrapping to an in-range value.
    const auto is_port = [](const std::string& port) {
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
    };
    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos || close == 1) {
            error = _L("Enter a valid bracketed IPv6 address.");
            return false;
        }
        const std::string rest = authority.substr(close + 1);
        if (!rest.empty() && (rest.front() != ':' || !is_port(rest.substr(1)))) {
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
            if (colon == 0 || authority.find(':', colon + 1) != std::string::npos || !is_port(authority.substr(colon + 1))) {
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

}} // namespace Slic3r::GUI
