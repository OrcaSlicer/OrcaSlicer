#include "wx/wxprec.h"
#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#include "NativeMediaCtrl.h"
#include <boost/log/trivial.hpp>
#include <regex>
#include <algorithm>

wxDEFINE_EVENT(EVT_NATIVE_MEDIA_STATE_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_NATIVE_MEDIA_ERROR, wxCommandEvent);
wxDEFINE_EVENT(EVT_NATIVE_MEDIA_SIZE_CHANGED, wxCommandEvent);

namespace Slic3r { namespace GUI {

StreamCredentials StreamCredentials::Parse(const std::string& url)
{
    StreamCredentials creds;

    std::regex url_regex(R"(^(\w+)://(?:([^:@]+)(?::([^@]*))?@)?([^/:]+)(?::(\d+))?(/.*)?$)");
    std::smatch match;

    if (std::regex_match(url, match, url_regex)) {
        creds.scheme = match[1].str();
        creds.username = match[2].str();
        creds.password = match[3].str();
        creds.host = match[4].str();
        if (match[5].matched) {
            creds.port = std::stoi(match[5].str());
        }
        creds.path = match[6].matched ? match[6].str() : "/";
    }

    return creds;
}

std::string StreamCredentials::BuildUrlWithoutCredentials() const
{
    std::string result = scheme + "://" + host;
    if (port > 0) {
        result += ":" + std::to_string(port);
    }
    result += path;
    return result;
}

StreamType NativeMediaCtrl::DetectStreamType(const wxString& url)
{
    std::string url_lower = url.Lower().ToStdString();

    if (url_lower.find("rtsp://") == 0) {
        return StreamType::RTSP;
    }
    if (url_lower.find("rtsps://") == 0) {
        return StreamType::RTSPS;
    }

    bool is_https = url_lower.find("https://") == 0;
    bool is_http = url_lower.find("http://") == 0;

    if (!is_http && !is_https) {
        return StreamType::Unknown;
    }

    size_t path_start = url_lower.find('/', is_https ? 8 : 7);
    std::string path = (path_start != std::string::npos) ? url_lower.substr(path_start) : "/";

    size_t query_pos = path.find('?');
    if (query_pos != std::string::npos) {
        path = path.substr(0, query_pos);
    }

    if (path.length() >= 5) {
        std::string ext = path.substr(path.length() - 5);
        if (ext == ".mjpg" || (path.length() >= 6 && path.substr(path.length() - 6) == ".mjpeg")) {
            return is_https ? StreamType::MJPEG_HTTPS : StreamType::MJPEG_HTTP;
        }
    }

    if (path.find("/axis-cgi/mjpg") != std::string::npos ||
        path.find("/cgi-bin/mjpg") != std::string::npos ||
        path.find("/mjpg/") != std::string::npos ||
        path.find("/mjpeg/") != std::string::npos) {
        return is_https ? StreamType::MJPEG_HTTPS : StreamType::MJPEG_HTTP;
    }

    return is_https ? StreamType::HTTPS_VIDEO : StreamType::HTTP_VIDEO;
}

bool NativeMediaCtrl::IsSupported(const wxString& url)
{
    StreamType type = DetectStreamType(url);
    return type != StreamType::Unknown;
}

wxString NativeMediaCtrl::StreamTypeToString(StreamType type)
{
    switch (type) {
        case StreamType::RTSP:        return "RTSP";
        case StreamType::RTSPS:       return "RTSPS (TLS)";
        case StreamType::MJPEG_HTTP:  return "MJPEG (HTTP)";
        case StreamType::MJPEG_HTTPS: return "MJPEG (HTTPS)";
        case StreamType::HTTP_VIDEO:  return "HTTP Video";
        case StreamType::HTTPS_VIDEO: return "HTTPS Video";
        default:                      return "Unknown";
    }
}

wxString NativeMediaCtrl::GetLastErrorMessage() const
{
    NativeMediaError err = GetLastError();
    switch (err) {
        case NativeMediaError::None:               return "";
        case NativeMediaError::NetworkUnreachable: return "Network unreachable";
        case NativeMediaError::AuthenticationFailed: return "Authentication failed";
        case NativeMediaError::StreamNotFound:     return "Stream not found";
        case NativeMediaError::UnsupportedFormat:  return "Unsupported format";
        case NativeMediaError::DecoderError:       return "Decoder error";
        case NativeMediaError::ConnectionTimeout:  return "Connection timeout";
        case NativeMediaError::TLSError:           return "TLS/SSL error";
        case NativeMediaError::InternalError:      return "Internal error";
        default:                                   return "Unknown error";
    }
}

void NativeMediaCtrl::SetRetryEnabled(bool enabled)
{
    m_retry_enabled = enabled;
    if (!enabled) {
        m_retry_timer.Stop();
    }
}

bool NativeMediaCtrl::IsRetryEnabled() const
{
    return m_retry_enabled;
}

void NativeMediaCtrl::OnRetryTimer(wxTimerEvent& event)
{
    if (m_retry_count >= MAX_RETRIES) {
        BOOST_LOG_TRIVIAL(warning) << "NativeMediaCtrl: Max retries reached, giving up";
        wxCommandEvent error_event(EVT_NATIVE_MEDIA_ERROR);
        error_event.SetEventObject(this);
        error_event.SetInt(static_cast<int>(NativeMediaError::ConnectionTimeout));
        wxPostEvent(this, error_event);
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl: Retry attempt " << (m_retry_count + 1) << " of " << MAX_RETRIES;
    m_retry_count++;

    if (!m_current_url.empty()) {
        Load(m_current_url);
        Play();
    }
}

void NativeMediaCtrl::ScheduleRetry()
{
    if (!m_retry_enabled || m_retry_count >= MAX_RETRIES) {
        return;
    }

    int delay = BASE_RETRY_DELAY_MS * (1 << std::min(m_retry_count, 4));
    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl: Scheduling retry in " << delay << "ms";
    m_retry_timer.StartOnce(delay);
}

void NativeMediaCtrl::ResetRetryState()
{
    m_retry_count = 0;
    m_retry_timer.Stop();
}

}} // namespace Slic3r::GUI
