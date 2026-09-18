#pragma once

#include <wx/uri.h>
#include <string>

namespace Slic3r { namespace GUI {

// Use one canonical origin for API requests, navigation and credential injection.
// A bare hostname defaults to HTTPS; explicit HTTP supports local servers.
inline std::string cloud_server_url(const std::string& value)
{
    wxString text = wxString::FromUTF8(value);
    text.Trim(true).Trim(false);
    if (text.empty()) return {};
    if (!text.Contains("://")) text.Prepend("https://");
    wxURI uri(text);
    const wxString scheme = uri.GetScheme().Lower();
    wxString host = uri.GetServer().Lower();
    if ((scheme != "https" && scheme != "http") || host.empty() ||
        uri.HasUserInfo() || uri.HasQuery() || uri.HasFragment() ||
        (!uri.GetPath().empty() && uri.GetPath() != "/") ||
        text.find_first_of(" \t\r\n\\") != wxString::npos)
        return {};
    unsigned long port = scheme == "https" ? 443 : 80;
    if (uri.HasPort() && (!uri.GetPort().ToULong(&port) || port == 0 || port > 65535)) return {};
    if (host.Contains(":") && !host.StartsWith("[")) host = "[" + host + "]";
    wxString origin = scheme + "://" + host;
    if ((scheme == "https" && port != 443) || (scheme == "http" && port != 80))
        origin += wxString::Format(":%lu", port);
    return std::string(origin.utf8_str());
}

inline bool is_cloud_server_url(const wxString& url, const std::string& server)
{
    if (server.empty()) return false;
    const wxURI uri(url);
    wxString host = uri.GetServer();
    if (host.Contains(":") && !host.StartsWith("[")) host = "[" + host + "]";
    wxString origin = uri.GetScheme() + "://" + host;
    if (uri.HasPort()) origin += ":" + uri.GetPort();
    return cloud_server_url(std::string(origin.utf8_str())) == server;
}

}} // namespace Slic3r::GUI
