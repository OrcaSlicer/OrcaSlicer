#pragma once

#include <string>

#include <wx/string.h>

namespace Slic3r { namespace GUI {

// Selects the path rules a filesystem permission is validated against. Native follows the
// platform that will match the grant; Posix and Windows exist so tests can pin both rule sets
// on any host.
enum class PermissionPathStyle
{
    Native,
    Posix,
    Windows
};

std::string trim_permission_target(const std::string& value);

// A tree wildcard may only be the final "/*"; "*" anywhere else is a literal character to the
// matcher, so accepting it here would promise a scope the grant does not have. Relative roots and
// ".." are refused for the same reason: they move the effective scope to the process working
// directory at match time.
bool normalize_filesystem_permission(const std::string& raw, std::string& normalized, wxString& error,
                                     PermissionPathStyle style = PermissionPathStyle::Native);

// Validates the authority before handing the URL to the shared matcher normalizer. This catches
// malformed ports, unbracketed IPv6, userinfo and backslashes, which host_from_authority would
// otherwise silently turn into a different host. The canonical result drops port, path, query and
// fragment and lowercases scheme and host, exactly as the audit matcher stores and compares
// network grants.
bool normalize_network_permission(const std::string& raw, std::string& normalized, wxString& error);

}} // namespace Slic3r::GUI
