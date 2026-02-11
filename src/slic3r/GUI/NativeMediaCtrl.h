#ifndef NativeMediaCtrl_h
#define NativeMediaCtrl_h

#include <wx/window.h>
#include <wx/event.h>
#include <wx/timer.h>
#include <memory>
#include <string>
#include <functional>

wxDECLARE_EVENT(EVT_NATIVE_MEDIA_STATE_CHANGED, wxCommandEvent);
wxDECLARE_EVENT(EVT_NATIVE_MEDIA_ERROR, wxCommandEvent);
wxDECLARE_EVENT(EVT_NATIVE_MEDIA_SIZE_CHANGED, wxCommandEvent);

namespace Slic3r { namespace GUI {

enum class NativeMediaState {
    Stopped,
    Loading,
    Playing,
    Paused,
    Error
};

enum class StreamType {
    Unknown,
    RTSP,
    RTSPS,
    MJPEG_HTTP,
    MJPEG_HTTPS,
    HTTP_VIDEO,
    HTTPS_VIDEO
};

enum class NativeMediaError {
    None = 0,
    NetworkUnreachable = 1,
    AuthenticationFailed = 2,
    StreamNotFound = 3,
    UnsupportedFormat = 4,
    DecoderError = 5,
    ConnectionTimeout = 6,
    TLSError = 7,
    InternalError = 99
};

struct StreamCredentials {
    std::string username;
    std::string password;
    std::string host;
    int port{0};
    std::string path;
    std::string scheme;

    static StreamCredentials Parse(const std::string& url);
    std::string BuildUrlWithoutCredentials() const;
    bool HasCredentials() const { return !username.empty(); }
};

class NativeMediaCtrl : public wxWindow
{
public:
    NativeMediaCtrl(wxWindow* parent);
    ~NativeMediaCtrl() override;

    bool Load(const wxString& url);
    void Play();
    void Stop();
    void Pause();

    NativeMediaState GetState() const;
    wxSize GetVideoSize() const;

    NativeMediaError GetLastError() const;
    wxString GetLastErrorMessage() const;

    void SetRetryEnabled(bool enabled);
    bool IsRetryEnabled() const;

    static StreamType DetectStreamType(const wxString& url);
    static bool IsSupported(const wxString& url);
    static wxString StreamTypeToString(StreamType type);

protected:
    void DoSetSize(int x, int y, int width, int height, int sizeFlags) override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;

    void OnRetryTimer(wxTimerEvent& event);
    void ScheduleRetry();
    void ResetRetryState();

    wxTimer m_retry_timer;
    int m_retry_count{0};
    bool m_retry_enabled{true};
    wxString m_current_url;

    static constexpr int MAX_RETRIES = 5;
    static constexpr int BASE_RETRY_DELAY_MS = 2000;
};

}} // namespace Slic3r::GUI

#endif // NativeMediaCtrl_h
