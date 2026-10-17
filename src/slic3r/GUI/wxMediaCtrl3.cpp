#include "wxMediaCtrl3.h"
#include "AVVideoDecoder.hpp"
#include <algorithm>
#include <boost/log/trivial.hpp>
#include "slic3r/GUI/Printer/BambuTunnel.h"
#include <cassert>
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <wx/dcclient.h>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <wx/event.h>
#include <wx/uri.h>
#include <wx/gdicmn.h>
#include <wx/mediactrl.h>
#include <wx/string.h>
#include <wx/image.h>
#include <wx/cursor.h>
#include <wx/math.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>

class wxWindow;
}
#ifdef __WIN32__
#include <versionhelpers.h>
#include <wx/msw/registry.h>
#include <shellapi.h>
#endif

wxDEFINE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);

BEGIN_EVENT_TABLE(wxMediaCtrl3, wxWindow)

// catch paint events
EVT_PAINT(wxMediaCtrl3::paintEvent)
EVT_MOUSEWHEEL(wxMediaCtrl3::mouseWheelEvent)
EVT_LEFT_DOWN(wxMediaCtrl3::mouseLeftDown)
EVT_LEFT_DCLICK(wxMediaCtrl3::mouseDoubleClick)
EVT_LEFT_UP(wxMediaCtrl3::mouseLeftUp)
EVT_MOTION(wxMediaCtrl3::mouseMotion)
EVT_MOUSE_CAPTURE_LOST(wxMediaCtrl3::mouseCaptureLost)

END_EVENT_TABLE()

struct StaticBambuLib : BambuLib
{
    static StaticBambuLib &get(BambuLib *);
};

wxMediaCtrl3::wxMediaCtrl3(wxWindow *parent)
    : wxWindow(parent, wxID_ANY)
    , BambuLib(StaticBambuLib::get(this))
    , m_thread([this] { PlayThread(); })
{
    SetBackgroundColour("#000001ff");
}

wxMediaCtrl3::~wxMediaCtrl3()
{
    if (HasCapture()) // a drag may still be in progress
        ReleaseMouse();
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_url.reset(new wxURI);
        m_frame = wxImage(m_idle_image);
        m_cond.notify_all();
    }
    m_thread.join();
}

static void adjust_frame_size(wxSize& frame, wxSize const& video, wxSize const& window);

void wxMediaCtrl3::Load(wxURI url)
{
    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_external)
        return;
    m_video_size = wxDefaultSize;
    m_error = 0;
    reset_view();
    m_url.reset(new wxURI(url));
    m_cond.notify_all();
}

void wxMediaCtrl3::Play()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_external)
        return;
    if (m_state != wxMEDIASTATE_PLAYING) {
        m_state = wxMEDIASTATE_PLAYING;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
    }
}

void wxMediaCtrl3::Stop()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    m_url.reset();
    reset_view();
    m_frame = wxImage(m_idle_image);
    NotifyStopped();
    m_cond.notify_all();
    Refresh();
}

void wxMediaCtrl3::SetExternalFrame(const wxImage& frame, wxSize videoSize)
{
    if (!frame.IsOk())
        return;
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        if (!m_external)
            return;
        m_frame = frame;
        m_video_size = videoSize.IsFullySpecified() ? videoSize : frame.GetSize();
        adjust_frame_size(m_frame_size, m_video_size, GetSize());
    }
    CallAfter([this] { Refresh(); });
}

#ifdef _WIN32
void wxMediaCtrl3::SetExternalFrame(const wxBitmap& frame, wxSize videoSize)
{
    if (!frame.IsOk())
        return;
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        if (!m_external)
            return;
        m_frame = frame;
        m_video_size = videoSize.IsFullySpecified() ? videoSize : frame.GetSize();
        adjust_frame_size(m_frame_size, m_video_size, GetSize());
    }
    CallAfter([this] { Refresh(); });
}
#endif

void wxMediaCtrl3::BeginExternalStream()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    m_external = true;
    reset_view();
    m_url.reset();
    m_active_url.reset();
    m_video_size = wxDefaultSize;
    m_frame = wxImage(m_idle_image);
    m_cond.notify_all();
    Refresh();
}

void wxMediaCtrl3::EndExternalStream()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    m_external = false;
    reset_view();
    m_url.reset();
    m_active_url.reset();
    m_video_size = wxDefaultSize;
    m_frame = wxImage(m_idle_image);
    m_cond.notify_all();
    Refresh();
}

void wxMediaCtrl3::SetIdleImage(wxString const &image)
{
    if (m_idle_image == image)
        return;
    m_idle_image = image;
    if (m_url == nullptr) {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_frame = wxImage(m_idle_image);
        assert(m_frame.IsOk());
        Refresh();
    }
}

wxMediaState wxMediaCtrl3::GetState()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_state;
}

int wxMediaCtrl3::GetLastError() const
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_error;
}

wxSize wxMediaCtrl3::GetVideoSize() const
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_video_size;
}

wxSize wxMediaCtrl3::DoGetBestSize() const
{
    return {-1, -1};
}

static void adjust_frame_size(wxSize & frame, wxSize const & video, wxSize const & window)
{
    if (video.x * window.y < video.y * window.x)
        frame = { video.x * window.y / video.y, window.y };
    else
        frame = { window.x, video.y * window.x / video.x };
}

void wxMediaCtrl3::paintEvent(wxPaintEvent &evt)
{
    wxPaintDC dc(this);
    auto      size = GetSize();
    if (size.x <= 0 || size.y <= 0)
        return;
    std::unique_lock<std::mutex> lk(m_mutex);
    if (!m_frame.IsOk())
        return;
    auto size2 = m_frame.GetSize();
    if (size2.x != m_frame_size.x && size2.y == m_frame_size.y)
        size2.x = m_frame_size.x;
    // Base "contain" fit scale, then the digital zoom of the live view on top of it.
    // At m_zoom == 1 this draws exactly what the plain fitted, centred rendering does.
    const double fit = fit_scale(size, size2);
    m_zoom           = std::min(m_zoom, max_zoom(fit)); // the window may have grown since the zoom was set
    const double effective_scale = fit * m_zoom;
    if ((m_zoom > 1.0) != m_zoomed_cursor) { // a hand while the image can be dragged
        m_zoomed_cursor = m_zoom > 1.0;
        SetCursor(m_zoomed_cursor ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
    }
    dc.SetUserScale(effective_scale, effective_scale);
    // The image is centred in the window, moved by the pan; when zoomed in the overflow is cropped by the window.
    // The pan is clamped again here because the window may have been resized since it was set.
    double pan_x = m_pan_x, pan_y = m_pan_y;
    clamp_pan(pan_x, pan_y, size, size2, effective_scale);
    const int offset_x = wxRound((size.x / 2.0 + pan_x) / effective_scale - size2.x / 2.0);
    const int offset_y = wxRound((size.y / 2.0 + pan_y) / effective_scale - size2.y / 2.0);
    dc.DrawBitmap(m_frame, offset_x, offset_y);
}

// Scale that fits a frame into the window ("contain")
double wxMediaCtrl3::fit_scale(wxSize const &size, wxSize const &frame)
{
    if (frame.x == size.x || frame.y == size.y)
        return 1.;
    return (size.x * frame.y > size.y * frame.x) ? double(size.y) / frame.y : double(size.x) / frame.x;
}

// Keep the zoomed image covering the window: the pan can move it only by the part that overflows
void wxMediaCtrl3::clamp_pan(double &pan_x, double &pan_y, wxSize const &size, wxSize const &frame, double effective_scale)
{
    const double max_x = std::max(0.0, (frame.x * effective_scale - size.x) / 2.0);
    const double max_y = std::max(0.0, (frame.y * effective_scale - size.y) / 2.0);
    pan_x = std::clamp(pan_x, -max_x, max_x);
    pan_y = std::clamp(pan_y, -max_y, max_y);
}

// Highest zoom that is still worth it: beyond a few screen pixels per video pixel the image only gets blockier.
// It follows from the video resolution and the window size, so a large video in a small window can be zoomed further.
double wxMediaCtrl3::max_zoom(double fit)
{
    constexpr double max_screen_pixels_per_video_pixel = 3.0;
    constexpr double min_limit = 2.0;  // zoom stays available even when the window is already larger than the video
    constexpr double max_limit = 16.0;
    return std::clamp(max_screen_pixels_per_video_pixel / fit, min_limit, max_limit);
}

void wxMediaCtrl3::reset_view()
{
    m_zoom      = 1.0;
    m_pan_x     = 0.0;
    m_pan_y     = 0.0;
    m_dragging  = false;
}

// Size of the frame as paintEvent() sees it; false when no live frame is shown
bool wxMediaCtrl3::live_frame_size(wxSize &frame)
{
    std::unique_lock<std::mutex> lk(m_mutex);
    if (!(m_external || m_state == wxMEDIASTATE_PLAYING) || !m_frame.IsOk())
        return false;
    frame = m_frame.GetSize();
    if (frame.x != m_frame_size.x && frame.y == m_frame_size.y)
        frame.x = m_frame_size.x;
    return true;
}

// The mouse wheel zooms the live view in 10% steps from 1x (fit) up to max_zoom(), keeping the point under the cursor in place
void wxMediaCtrl3::mouseWheelEvent(wxMouseEvent &evt)
{
    wxSize frame;
    if (evt.GetWheelRotation() == 0 || !live_frame_size(frame)) {
        evt.Skip();
        return;
    }

    const wxSize size     = GetSize();
    const double old_zoom = m_zoom;
    const double fit      = fit_scale(size, frame);
    const double new_zoom = std::clamp(old_zoom * (evt.GetWheelRotation() > 0 ? 1.1 : 1.0 / 1.1), 1.0, max_zoom(fit));
    if (new_zoom == old_zoom)
        return;

    // Move the pan so that the image point under the cursor stays under it
    const double ratio = new_zoom / old_zoom;
    const double dx    = evt.GetX() - size.x / 2.0;
    const double dy    = evt.GetY() - size.y / 2.0;
    m_pan_x = dx - (dx - m_pan_x) * ratio;
    m_pan_y = dy - (dy - m_pan_y) * ratio;
    m_zoom  = new_zoom;
    clamp_pan(m_pan_x, m_pan_y, size, frame, fit * m_zoom);
    Refresh();
}

// Double click puts the view back to the fitted, centred image
void wxMediaCtrl3::mouseDoubleClick(wxMouseEvent &evt)
{
    if (m_zoom > 1.0) {
        reset_view();
        Refresh();
    }
    evt.Skip();
}

// Dragging with the left button moves the zoomed image
void wxMediaCtrl3::mouseLeftDown(wxMouseEvent &evt)
{
    wxSize frame;
    if (m_zoom > 1.0 && live_frame_size(frame)) {
        m_dragging  = true;
        m_drag_last = evt.GetPosition();
        if (!HasCapture())
            CaptureMouse();
    }
    evt.Skip();
}

void wxMediaCtrl3::mouseMotion(wxMouseEvent &evt)
{
    wxSize frame;
    if (m_dragging && evt.Dragging() && evt.LeftIsDown() && live_frame_size(frame)) {
        const wxSize  size = GetSize();
        const wxPoint pos  = evt.GetPosition();
        m_pan_x += pos.x - m_drag_last.x;
        m_pan_y += pos.y - m_drag_last.y;
        m_drag_last = pos;
        clamp_pan(m_pan_x, m_pan_y, size, frame, fit_scale(size, frame) * m_zoom);
        Refresh();
    }
    evt.Skip();
}

void wxMediaCtrl3::mouseLeftUp(wxMouseEvent &evt)
{
    m_dragging = false;
    if (HasCapture())
        ReleaseMouse();
    evt.Skip();
}

void wxMediaCtrl3::mouseCaptureLost(wxMouseCaptureLostEvent &)
{
    m_dragging = false;
}

void wxMediaCtrl3::DoSetSize(int x, int y, int width, int height, int sizeFlags)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
    if (sizeFlags == wxSIZE_USE_EXISTING) return;
    wxMediaCtrl_OnSize(this, m_video_size, width, height);
    std::unique_lock<std::mutex> lk(m_mutex);
    adjust_frame_size(m_frame_size, m_video_size, GetSize());
    Refresh();
}

void wxMediaCtrl3::bambu_log(void *ctx, int level, tchar const *msg2)
{
#ifdef _WIN32
    wxString msg(msg2);
#else
    wxString msg = wxString::FromUTF8(msg2);
#endif
    if (level == 1) {
        if (msg.EndsWith("]")) {
            int n = msg.find_last_of('[');
            if (n != wxString::npos) {
                long val = 0;
                wxMediaCtrl3 *ctrl = (wxMediaCtrl3 *) ctx;
                if (msg.SubString(n + 1, msg.Length() - 2).ToLong(&val)) {
                    std::unique_lock<std::mutex> lk(ctrl->m_mutex);
                    ctrl->m_error = (int) val;
                }
            }
        } else if (msg.Contains("stat_log")) {
            wxCommandEvent evt(EVT_MEDIA_CTRL_STAT);
            wxMediaCtrl3 *ctrl = (wxMediaCtrl3 *) ctx;
            evt.SetEventObject(ctrl);
            evt.SetString(msg.Mid(msg.Find(' ') + 1));
            wxPostEvent(ctrl, evt);
        }
    }
    BOOST_LOG_TRIVIAL(info) << msg.ToUTF8().data();
}

// FFmpeg's own diagnostics (HTTP status, "Invalid data found", demuxer choice,
// missing stream dimensions, ...) are otherwise swallowed: a failed camera open
// only surfaces as wxMediaCtrl3's generic error code, which MediaPlayCtrl maps to
// the misleading "Player is malfunctioning" string. Forward them to the Orca log
// instead. Verbosity defaults to AV_LOG_VERBOSE and can be raised at runtime with
// ORCA_FFMPEG_LOG_LEVEL=debug|trace|... (or lowered to warning/error/quiet).
static int ffmpeg_log_level_from_env()
{
    const char *env = std::getenv("ORCA_FFMPEG_LOG_LEVEL");
    if (env == nullptr || *env == '\0')
        return AV_LOG_VERBOSE;
    const wxString v = wxString(env).Lower();
    if (v == "quiet")   return AV_LOG_QUIET;
    if (v == "panic")   return AV_LOG_PANIC;
    if (v == "fatal")   return AV_LOG_FATAL;
    if (v == "error")   return AV_LOG_ERROR;
    if (v == "warning") return AV_LOG_WARNING;
    if (v == "info")    return AV_LOG_INFO;
    if (v == "verbose") return AV_LOG_VERBOSE;
    if (v == "debug")   return AV_LOG_DEBUG;
    if (v == "trace")   return AV_LOG_TRACE;
    return AV_LOG_VERBOSE;
}

static void ffmpeg_log_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    if (level > av_log_get_level())
        return;
    thread_local int print_prefix = 1;
    char line[1024];
    av_log_format_line2(avcl, level, fmt, vl, line, (int) sizeof(line), &print_prefix);
    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
        line[--len] = '\0';
    if (len == 0)
        return;
    if (level <= AV_LOG_ERROR)
        BOOST_LOG_TRIVIAL(error) << "ffmpeg: " << line;
    else if (level <= AV_LOG_WARNING)
        BOOST_LOG_TRIVIAL(warning) << "ffmpeg: " << line;
    else if (level <= AV_LOG_INFO)
        BOOST_LOG_TRIVIAL(info) << "ffmpeg: " << line;
    else
        BOOST_LOG_TRIVIAL(debug) << "ffmpeg: " << line;
}

static void install_ffmpeg_logger()
{
    av_log_set_level(ffmpeg_log_level_from_env());
    av_log_set_callback(&ffmpeg_log_callback);
}

int wxMediaCtrl3::ffmpeg_interrupt_callback(void *opaque)
{
    auto *ctrl = static_cast<wxMediaCtrl3 *>(opaque);
    std::lock_guard<std::mutex> lock(ctrl->m_mutex);
    return ctrl->m_url != ctrl->m_active_url;
}

int wxMediaCtrl3::PlayFfmpeg(std::shared_ptr<wxURI> const &url, std::unique_lock<std::mutex> &lock)
{
    static std::once_flag logger_once;
    std::call_once(logger_once, install_ffmpeg_logger);

    if (avformat_network_init() < 0)
        return 2;

    AVFormatContext *format_context = avformat_alloc_context();
    if (!format_context) {
        avformat_network_deinit();
        return 2;
    }

    format_context->interrupt_callback = {&wxMediaCtrl3::ffmpeg_interrupt_callback, this};
    format_context->flags |= AVFMT_FLAG_NOBUFFER;
    format_context->max_delay = 0;
    m_active_url = url;

    auto finish = [&](int error) {
        lock.unlock();
        avformat_close_input(&format_context);
        avformat_network_deinit();
        lock.lock();
        m_active_url.reset();
        return error;
    };

    const std::string uri = url->BuildURI().ToUTF8().data();
    const wxString scheme = url->GetScheme();
    const bool http_stream = scheme.CmpNoCase("http") == 0 || scheme.CmpNoCase("https") == 0;
    AVDictionary *options = nullptr;
    if (http_stream) {
        // Live multipart MJPEG. fflags=nobuffer / AVFMT_FLAG_NOBUFFER / max_delay=0
        // (set above) are the low-latency levers - they disable the demuxer
        // read-ahead queue. probesize / analyzeduration only bound the one-off
        // avformat_find_stream_info() at open; a 32-byte budget returned before a
        // whole JPEG frame was seen, so width/height came back unset and the open
        // was rejected. Give it room to identify one frame (a startup cost only).
        // rw_timeout / timeout bound a wedged connect or read so a stale stream
        // fails fast and is retried, instead of the reader thread hanging.
        // avioflags=direct is deliberately NOT set: unbuffered reads make the
        // mpjpeg demuxer emit "Packet corrupt" and bail on any short read across
        // a multipart boundary.
        av_dict_set(&options, "fflags", "nobuffer", 0);
        av_dict_set(&options, "probesize", "5000000", 0);
        av_dict_set(&options, "analyzeduration", "1000000", 0);
        av_dict_set(&options, "rw_timeout", "5000000", 0);
        av_dict_set(&options, "timeout", "5000000", 0);
    } else {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
    }
    lock.unlock();
    int error = avformat_open_input(&format_context, uri.c_str(), nullptr, &options);
    av_dict_free(&options);
    lock.lock();
    if (error < 0)
        return finish(2);

    lock.unlock();
    error = avformat_find_stream_info(format_context, nullptr);
    lock.lock();
    if (error < 0)
        return finish(2);

    const int video_stream = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream < 0)
        return finish(2);

    AVVideoDecoder decoder;
    if (decoder.open(*format_context->streams[video_stream]->codecpar) < 0)
        return finish(2);

    // Prefer the dimensions the container reported. A small probe budget, or a
    // camera that doesn't announce a size up front, can leave these unset - in
    // that case fill them in from the first frame that decodes (below) rather
    // than failing the open outright.
    auto apply_video_size = [&](wxSize size) {
        if (!size.IsFullySpecified() || size.x <= 0 || size.y <= 0)
            return false;
        m_video_size = size;
        adjust_frame_size(m_frame_size, m_video_size, GetSize());
        NotifyStopped();
        return true;
    };
    bool have_size = apply_video_size({format_context->streams[video_stream]->codecpar->width,
                                      format_context->streams[video_stream]->codecpar->height});
    int size_probe_frames = 0; // frames spent still waiting for a usable size

    AVPacket *packet = av_packet_alloc();
    if (!packet)
        return finish(2);

    while (m_url == url) {
        lock.unlock();
        error = av_read_frame(format_context, packet);
        lock.lock();
        if (m_url != url)
            break;
        if (error < 0)
            break;
        if (packet->stream_index == video_stream) {
            const int decode_error = decoder.decode(*packet);
            if (decode_error == 0) {
                if (!have_size) {
                    have_size = apply_video_size(decoder.decoded_frame_size());
                    if (!have_size) {
                        av_packet_unref(packet);
                        // MJPEG yields a sized frame on the first full packet; if
                        // several seconds of frames never do, treat it as a bad
                        // stream instead of sitting in "Loading..." forever.
                        if (++size_probe_frames > 120)
                            break;
                        continue;
                    }
                }
                auto frame_size = m_frame_size;
                lock.unlock();
#ifdef _WIN32
                wxBitmap frame;
                decoder.toWxBitmap(frame, frame_size);
#else
                wxImage frame;
                decoder.toWxImage(frame, frame_size);
#endif
                lock.lock();
                if (m_url != url)
                    break;
                if (frame.IsOk())
                    m_frame = frame;
                if (!m_refresh_pending.exchange(true)) {
                    CallAfter([this] {
                        m_refresh_pending.store(false);
                        Refresh();
                    });
                }
            }
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return finish(m_url == url ? 2 : 1);
}

void wxMediaCtrl3::PlayThread()
{
    using namespace std::chrono_literals;
    std::shared_ptr<wxURI> url;
    std::unique_lock<std::mutex> lk(m_mutex);
    while (true) {
        m_cond.wait(lk, [this, &url] { return m_url != url; });
        url = m_url;
        if (url == nullptr)
            continue;
        if (!url->HasScheme())
            break;
        const wxString scheme = url->GetScheme();
        const bool generic_ffmpeg = scheme.CmpNoCase("http") == 0 || scheme.CmpNoCase("https") == 0 ||
                                    scheme.CmpNoCase("rtsp") == 0 || scheme.CmpNoCase("rtsps") == 0;
        int error = 0;
        if (generic_ffmpeg) {
            error = PlayFfmpeg(url, lk);
        } else {
            lk.unlock();
            Bambu_Tunnel tunnel = nullptr;
            error = Bambu_Create(&tunnel, m_url->BuildURI().ToUTF8());
            if (error == 0) {
                Bambu_SetLogger(tunnel, &wxMediaCtrl3::bambu_log, this);
                error = Bambu_Open(tunnel);
                if (error == 0)
                    error = Bambu_would_block;
            }
            lk.lock();
            while (error == int(Bambu_would_block)) {
                m_cond.wait_for(lk, 100ms);
                if (m_url != url) {
                    error = 1;
                    break;
                }
                lk.unlock();
                error = Bambu_StartStream(tunnel, true);
                lk.lock();
            }
            Bambu_StreamInfo info;
            if (error == 0)
                error = Bambu_GetStreamInfo(tunnel, 0, &info);
            AVVideoDecoder decoder;
            int minFrameDuration = 0;
            if (error == 0) {
                decoder.open(info);
                m_video_size = { info.format.video.width, info.format.video.height };
                adjust_frame_size(m_frame_size, m_video_size, GetSize());
                minFrameDuration = 800 / info.format.video.frame_rate; // 80%
                NotifyStopped();
            }
            Bambu_Sample sample;
            while (error == 0) {
                lk.unlock();
                error = Bambu_ReadSample(tunnel, &sample);
                lk.lock();
                while (error == int(Bambu_would_block)) {
                    m_cond.wait_for(lk, 100ms);
                    if (m_url != url) {
                        error = 1;
                        break;
                    }
                    lk.unlock();
                    error = Bambu_ReadSample(tunnel, &sample);
                    lk.lock();
                }
                if (error == 0) {
                    auto frame_size = m_frame_size;
                    lk.unlock();
                    decoder.decode(sample);
#ifdef _WIN32
                    wxBitmap bm;
                    decoder.toWxBitmap(bm, frame_size);
#else
                    wxImage bm;
                    decoder.toWxImage(bm, frame_size);
#endif
                    lk.lock();
                    if (m_url != url) {
                        error = 1;
                        break;
                    }
                    if (bm.IsOk()) {
                        auto now = std::chrono::system_clock::now();
                        if (m_last_PTS && (sample.decode_time - m_last_PTS) < 30000000ULL) { // 3s
                            auto next_PTS_expected = m_last_PTS_expected + std::chrono::milliseconds((sample.decode_time - m_last_PTS) / 10000ULL);
                            // The frame is late, catch up a little
                            auto next_PTS_practical = m_last_PTS_practical + std::chrono::milliseconds(minFrameDuration);
                            auto next_PTS = std::max(next_PTS_expected, next_PTS_practical);
                            if(now < next_PTS)
                                std::this_thread::sleep_until(next_PTS);
                            else
                                next_PTS = now;
                            //auto text = wxString::Format(L"wxMediaCtrl3 pts diff %ld\n", std::chrono::duration_cast<std::chrono::milliseconds>(next_PTS - next_PTS_expected).count());
                            //OutputDebugString(text);
                            m_last_PTS = sample.decode_time;
                            m_last_PTS_expected = next_PTS_expected;
                            m_last_PTS_practical = next_PTS;
                        } else {
                            // Resync
                            m_last_PTS           = sample.decode_time;
                            m_last_PTS_expected  = now;
                            m_last_PTS_practical = now;
                        }
                        m_frame = bm;
                    }
                    CallAfter([this] { Refresh(); });
                }
            }
            if (tunnel) {
                lk.unlock();
                Bambu_Close(tunnel);
                Bambu_Destroy(tunnel);
                tunnel = nullptr;
                lk.lock();
            }
        }
        if (m_url == url)
            m_error = error;
        m_frame_size = wxDefaultSize;
        m_video_size = wxDefaultSize;
        NotifyStopped();
    }

}

void wxMediaCtrl3::NotifyStopped()
{
    m_state = wxMEDIASTATE_STOPPED;
    wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
    event.SetId(GetId());
    event.SetEventObject(this);
    wxPostEvent(this, event);
}
