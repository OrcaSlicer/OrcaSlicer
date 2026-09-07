#pragma once

#include <slic3r/GUI/IMediaController.hpp>

#include <string>

class wxWebView;

namespace Slic3r { namespace GUI {

class WebMediaController : public IMediaController
{
public:
    explicit WebMediaController(wxWebView *webview);

    void Load(wxURI url) override;

    void Load(wxURI url, CameraStreamMode mode) override;

    void Play() override;

    void Stop() override;

    // wxMediaState GetState() override;

    // int GetLastError() const override;

    // wxSize GetVideoSize() const override;

private:
    wxWebView * m_webview;
    std::string m_url;
    CameraStreamMode m_stream_mode = CameraStreamMode::http;
};

}} // namespace Slic3r::GUI
