#include "wxMediaCtrl3.h"
#include "AVVideoDecoder.hpp"
#include <algorithm>
#include <cmath>
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
#include <wx/utils.h>
#include <wx/region.h>
#include <wx/brush.h>
#include <wx/pen.h>
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
    // paintEvent covers every pixel; never erase first (that flickers on every frame).
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_MOUSEWHEEL, &wxMediaCtrl3::mouseWheelEvent, this);
    Bind(wxEVT_LEFT_DOWN, &wxMediaCtrl3::mouseLeftDown, this);
    Bind(wxEVT_LEFT_DCLICK, &wxMediaCtrl3::mouseLeftDown, this);
    Bind(wxEVT_LEFT_UP, &wxMediaCtrl3::mouseLeftUp, this);
    Bind(wxEVT_MOTION, &wxMediaCtrl3::mouseMotion, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &wxMediaCtrl3::mouseCaptureLost, this);
}

// Digital zoom of the live view, 1x (fit) up to 5x, towards the point under the cursor.
// While zoomed in, dragging with the left button pans the view.
void wxMediaCtrl3::mouseWheelEvent(wxMouseEvent &evt)
{
    if (evt.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL || evt.GetWheelRotation() == 0 || evt.GetWheelDelta() == 0) {
        evt.Skip();
        return;
    }

    // 1.1x per wheel notch; high-resolution wheels and touchpads send fractions of a notch.
    const double notches = double(evt.GetWheelRotation()) / evt.GetWheelDelta();
    const double zoom    = std::clamp(m_zoom * std::pow(1.1, notches), 1.0, 5.0);
    if (zoom == m_zoom)
        return;

    // Keep the frame point under the cursor in place.
    if (m_paint_fit_scale > 0. && m_paint_frame_size.x > 0 && m_paint_frame_size.y > 0) {
        const wxSize  size  = GetSize();
        const wxPoint pos   = evt.GetPosition();
        const double  s_old = m_paint_fit_scale * m_zoom;
        const double  s_new = m_paint_fit_scale * zoom;
        const double  dx    = pos.x - size.x / 2.0;
        const double  dy    = pos.y - size.y / 2.0;
        m_view_cx += (dx / s_old - dx / s_new) / m_paint_frame_size.x;
        m_view_cy += (dy / s_old - dy / s_new) / m_paint_frame_size.y;
    }

    m_zoom = zoom;
    if (m_zoom <= 1.0)
        end_pan();
    SetCursor(m_zoom > 1.0 ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
    Refresh();
}

void wxMediaCtrl3::mouseLeftDown(wxMouseEvent &evt)
{
    evt.Skip();
    if (m_zoom <= 1.0)
        return;
    m_panning  = true;
    m_pan_last = evt.GetPosition();
    if (!HasCapture())
        CaptureMouse();
}

void wxMediaCtrl3::mouseLeftUp(wxMouseEvent &evt)
{
    evt.Skip();
    end_pan();
}

void wxMediaCtrl3::mouseMotion(wxMouseEvent &evt)
{
    // Always let others see motion: the camera full screen frame shows its close button on it.
    evt.Skip();
    if (!m_panning)
        return;

#ifdef __APPLE__
    // macOS never sends wxEVT_MOUSE_CAPTURE_LOST; read the real button state.
    const bool left_down = wxGetMouseState().LeftIsDown();
#else
    const bool left_down = evt.LeftIsDown();
#endif
    if (!left_down) {
        end_pan();
        return;
    }

    if (m_paint_fit_scale <= 0. || m_paint_frame_size.x <= 0 || m_paint_frame_size.y <= 0)
        return;
    const double  scale = m_paint_fit_scale * m_zoom;
    const wxPoint pos   = evt.GetPosition();
    m_view_cx -= (pos.x - m_pan_last.x) / scale / m_paint_frame_size.x;
    m_view_cy -= (pos.y - m_pan_last.y) / scale / m_paint_frame_size.y;
    m_pan_last = pos;
    Refresh();
}

void wxMediaCtrl3::mouseCaptureLost(wxMouseCaptureLostEvent &)
{
    m_panning = false;
    Refresh();
}

void wxMediaCtrl3::end_pan()
{
    m_panning = false;
    if (HasCapture())
        ReleaseMouse();
}

// Keep the zoomed frame covering the view: clamp the frame point shown at the view centre.
void wxMediaCtrl3::clamp_view_center(wxSize const &window, wxSize const &frame, double scale)
{
    if (m_zoom <= 1.0 || frame.x <= 0 || frame.y <= 0 || scale <= 0.) {
        m_view_cx = m_view_cy = 0.5;
        return;
    }
    auto clamp_axis = [scale](double center, int window_len, int frame_len) {
        const double half = window_len / 2.0 / scale; // half the visible extent, in frame pixels
        if (frame_len <= 2 * half)
            return 0.5; // the frame is narrower than the view on this axis: keep it centred
        return std::clamp(center * frame_len, half, frame_len - half) / frame_len;
    };
    m_view_cx = clamp_axis(m_view_cx, window.x, frame.x);
    m_view_cy = clamp_axis(m_view_cy, window.y, frame.y);
}

wxMediaCtrl3::~wxMediaCtrl3()
{
    if (HasCapture())
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

    // The window uses wxBG_STYLE_PAINT, so this handler paints every pixel itself. It fills only
    // the area the frame does not cover: erasing under the frame first shows a black flash before
    // each frame is drawn, which makes the video flicker.
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(GetBackgroundColour()));

    std::unique_lock<std::mutex> lk(m_mutex);
    if (!m_frame.IsOk()) {
        dc.DrawRectangle(0, 0, size.x, size.y);
        return;
    }
    auto size2 = m_frame.GetSize();
    if (size2.x != m_frame_size.x && size2.y == m_frame_size.y)
        size2.x = m_frame_size.x;
    // Base "contain" fit scale, then apply the digital zoom factor.
    // At m_zoom == 1 this is identical to the normal fitted rendering.
    double scale = 1.0;
    if (size2.x != size.x || size2.y != size.y)
        scale = (size.x * size2.y > size.y * size2.x)
            ? double(size.y) / size2.y
            : double(size.x) / size2.x;

    m_paint_frame_size = size2;
    m_paint_fit_scale  = scale;

    const double effective_scale = scale * m_zoom;

    // Draw so that the frame point (m_view_cx, m_view_cy) lands on the view centre.
    clamp_view_center(size, size2, effective_scale);
    const int offset_x =
        wxRound(size.x / 2.0 / effective_scale - m_view_cx * size2.x);
    const int offset_y =
        wxRound(size.y / 2.0 / effective_scale - m_view_cy * size2.y);

    // Fill the letterbox bars around the frame. The frame rectangle is shrunk to whole pixels
    // so the bars overlap its edges by at most a pixel, which the frame then paints over.
    const int x0 = int(std::ceil(offset_x * effective_scale));
    const int y0 = int(std::ceil(offset_y * effective_scale));
    const int x1 = int(std::floor((offset_x + size2.x) * effective_scale));
    const int y1 = int(std::floor((offset_y + size2.y) * effective_scale));
    wxRegion background(0, 0, size.x, size.y);
    if (x1 > x0 && y1 > y0)
        background.Subtract(wxRect(x0, y0, x1 - x0, y1 - y0));
    for (wxRegionIterator it(background); it; ++it)
        dc.DrawRectangle(it.GetRect());

    dc.SetUserScale(effective_scale, effective_scale);
    dc.DrawBitmap(m_frame, offset_x, offset_y);
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
