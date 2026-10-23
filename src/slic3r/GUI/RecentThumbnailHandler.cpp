#include "RecentThumbnailHandler.hpp"

#include <utility>

#include <wx/mstream.h>
#include <wx/stream.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/Utils/Http.hpp"

namespace Slic3r { namespace GUI {

namespace {

const char* const thumbnail_scheme = "thumb";

class PngResponseData : public wxWebViewHandlerResponseData
{
public:
    explicit PngResponseData(std::string png) : m_png(std::move(png)), m_stream(m_png.data(), m_png.size()) {}
    wxInputStream* GetStream() override { return &m_stream; }

private:
    std::string         m_png;
    wxMemoryInputStream m_stream;
};

wxString project_path_from_url(const wxString& url)
{
    return from_u8(Http::url_decode(into_u8(url.AfterFirst(':').BeforeFirst('?'))));
}

} // namespace

RecentThumbnailHandler::RecentThumbnailHandler() : wxWebViewHandler(thumbnail_scheme) {}

void RecentThumbnailHandler::StartRequest(const wxWebViewHandlerRequest& request, wxSharedPtr<wxWebViewHandlerResponse> response)
{
    MainFrame* mainframe = wxGetApp().mainframe;
    std::string png = mainframe ? mainframe->get_recent_thumbnail(project_path_from_url(request.GetRawURI())) : std::string();
    if (png.empty()) {
        response->FinishWithError();
        return;
    }
    response->SetContentType("image/png");
    response->Finish(wxSharedPtr<wxWebViewHandlerResponseData>(new PngResponseData(std::move(png))));
}

std::wstring recent_thumbnail_url(const wxString& project_path, std::time_t modified)
{
    wxString url = wxString(thumbnail_scheme) + ":" + from_u8(Http::url_encode(into_u8(project_path))) + "?" + std::to_wstring(modified);
    return url.ToStdWstring();
}

}} // namespace Slic3r::GUI
