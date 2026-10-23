#pragma once

#include <ctime>
#include <string>

#include <wx/sharedptr.h>
#include <wx/string.h>
#include <wx/webview.h>

namespace Slic3r { namespace GUI {

// Serves the Home page's recent project thumbnails as thumb: URLs. The page then fetches only the
// images it shows, instead of receiving every one of them base64-encoded inside one message.
class RecentThumbnailHandler : public wxWebViewHandler
{
public:
    RecentThumbnailHandler();
    void StartRequest(const wxWebViewHandlerRequest& request, wxSharedPtr<wxWebViewHandlerResponse> response) override;
};

// The modification time is part of the URL so a resaved project doesn't show the WebView's cached image.
std::wstring recent_thumbnail_url(const wxString& project_path, std::time_t modified);

}} // namespace Slic3r::GUI
