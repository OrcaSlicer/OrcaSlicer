#include "wx/wxprec.h"
#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#include "NativeMediaCtrl.h"
#include <boost/log/trivial.hpp>

#ifdef __linux__

#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

namespace Slic3r { namespace GUI {

class NativeMediaCtrl::Impl
{
public:
    Impl(NativeMediaCtrl* owner, GtkWidget* widget);
    ~Impl();

    bool Load(const wxString& url);
    void Play();
    void Stop();
    void Pause();

    NativeMediaState GetState() const { return m_state; }
    wxSize GetVideoSize() const { return m_video_size; }
    NativeMediaError GetLastError() const { return m_error; }

    void UpdateLayout(int width, int height);

private:
    void SetupPipeline(const wxString& url);
    void CleanupPipeline();
    static GstBusSyncReply BusSyncHandler(GstBus* bus, GstMessage* message, gpointer user_data);
    static gboolean BusCallback(GstBus* bus, GstMessage* message, gpointer user_data);
    void HandleBusMessage(GstMessage* message);
    void NotifyStateChanged();
    NativeMediaError MapGstErrorToError(GError* error);
    std::string BuildPipelineForRtsp(const std::string& url, const StreamCredentials& creds);
    std::string BuildPipelineForMjpeg(const std::string& url, const StreamCredentials& creds);

    NativeMediaCtrl* m_owner;
    GtkWidget* m_widget;
    GstElement* m_pipeline;
    gulong m_bus_watch_id;

    NativeMediaState m_state;
    NativeMediaError m_error;
    wxSize m_video_size;
    wxString m_url;
    guintptr m_window_handle;
};

NativeMediaCtrl::Impl::Impl(NativeMediaCtrl* owner, GtkWidget* widget)
    : m_owner(owner)
    , m_widget(widget)
    , m_pipeline(nullptr)
    , m_bus_watch_id(0)
    , m_state(NativeMediaState::Stopped)
    , m_error(NativeMediaError::None)
    , m_video_size(1920, 1080)
    , m_window_handle(0)
{
    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }

    GdkWindow* gdk_window = gtk_widget_get_window(m_widget);
    if (gdk_window) {
#ifdef GDK_WINDOWING_X11
        if (GDK_IS_X11_WINDOW(gdk_window)) {
            m_window_handle = GDK_WINDOW_XID(gdk_window);
            BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Linux: Using X11 window handle";
        }
#endif
#ifdef GDK_WINDOWING_WAYLAND
        if (GDK_IS_WAYLAND_WINDOW(gdk_window)) {
            struct wl_surface* wl_surface = gdk_wayland_window_get_wl_surface(gdk_window);
            m_window_handle = reinterpret_cast<guintptr>(wl_surface);
            BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Linux: Using Wayland surface handle";
        }
#endif
    }

    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Linux: GStreamer implementation initialized";
}

NativeMediaCtrl::Impl::~Impl()
{
    CleanupPipeline();
}

void NativeMediaCtrl::Impl::CleanupPipeline()
{
    if (m_bus_watch_id) {
        g_source_remove(m_bus_watch_id);
        m_bus_watch_id = 0;
    }

    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
}

bool NativeMediaCtrl::Impl::Load(const wxString& url)
{
    CleanupPipeline();
    m_url = url;
    m_error = NativeMediaError::None;
    m_state = NativeMediaState::Loading;
    NotifyStateChanged();

    SetupPipeline(url);
    return m_pipeline != nullptr;
}

std::string NativeMediaCtrl::Impl::BuildPipelineForRtsp(const std::string& url, const StreamCredentials& creds)
{
    std::string pipeline;
    std::string clean_url = creds.HasCredentials() ? creds.BuildUrlWithoutCredentials() : url;

    pipeline = "rtspsrc location=\"" + clean_url + "\" latency=100 buffer-mode=auto ";
    pipeline += "protocols=tcp+udp-mcast+udp ";
    pipeline += "tcp-timeout=5000000 ";

    if (creds.HasCredentials()) {
        pipeline += "user-id=\"" + creds.username + "\" ";
        pipeline += "user-pw=\"" + creds.password + "\" ";
    }

    pipeline += "! decodebin ! videoconvert ! videoscale ! ";
    pipeline += "video/x-raw,format=BGRx ! ";
    pipeline += "autovideosink name=sink sync=false";

    return pipeline;
}

std::string NativeMediaCtrl::Impl::BuildPipelineForMjpeg(const std::string& url, const StreamCredentials& creds)
{
    std::string pipeline;
    std::string clean_url = creds.HasCredentials() ? creds.BuildUrlWithoutCredentials() : url;

    pipeline = "souphttpsrc location=\"" + clean_url + "\" ";
    pipeline += "is-live=true do-timestamp=true ";

    if (creds.HasCredentials()) {
        pipeline += "user-id=\"" + creds.username + "\" ";
        pipeline += "user-pw=\"" + creds.password + "\" ";
    }

    pipeline += "! multipartdemux ! jpegdec ! videoconvert ! ";
    pipeline += "video/x-raw,format=BGRx ! ";
    pipeline += "autovideosink name=sink sync=false";

    return pipeline;
}

void NativeMediaCtrl::Impl::SetupPipeline(const wxString& url)
{
    StreamType type = NativeMediaCtrl::DetectStreamType(url);
    std::string url_str = url.ToStdString();
    StreamCredentials creds = StreamCredentials::Parse(url_str);

    std::string pipeline_desc;

    if (type == StreamType::RTSP || type == StreamType::RTSPS) {
        pipeline_desc = BuildPipelineForRtsp(url_str, creds);
    } else if (type == StreamType::MJPEG_HTTP || type == StreamType::MJPEG_HTTPS) {
        pipeline_desc = BuildPipelineForMjpeg(url_str, creds);
    } else {
        pipeline_desc = "playbin uri=\"" + url_str + "\"";
    }

    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Linux: Creating pipeline";

    GError* error = nullptr;
    m_pipeline = gst_parse_launch(pipeline_desc.c_str(), &error);

    if (error) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Linux: Pipeline creation failed: " << error->message;
        m_error = NativeMediaError::InternalError;
        g_error_free(error);
        m_state = NativeMediaState::Error;
        NotifyStateChanged();
        return;
    }

    if (!m_pipeline) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Linux: Pipeline is null";
        m_error = NativeMediaError::InternalError;
        m_state = NativeMediaState::Error;
        NotifyStateChanged();
        return;
    }

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
    gst_bus_set_sync_handler(bus, BusSyncHandler, this, nullptr);
    m_bus_watch_id = gst_bus_add_watch(bus, BusCallback, this);
    gst_object_unref(bus);

    GstElement* sink = gst_bin_get_by_name(GST_BIN(m_pipeline), "sink");
    if (sink) {
        if (GST_IS_VIDEO_OVERLAY(sink) && m_window_handle != 0) {
            gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), m_window_handle);
        }
        gst_object_unref(sink);
    }
}

GstBusSyncReply NativeMediaCtrl::Impl::BusSyncHandler(GstBus* bus, GstMessage* message, gpointer user_data)
{
    if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_ELEMENT)
        return GST_BUS_PASS;

    if (!gst_is_video_overlay_prepare_window_handle_message(message))
        return GST_BUS_PASS;

    Impl* self = static_cast<Impl*>(user_data);

    if (self->m_window_handle != 0) {
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(message)), self->m_window_handle);
    }

    gst_message_unref(message);
    return GST_BUS_DROP;
}

gboolean NativeMediaCtrl::Impl::BusCallback(GstBus* bus, GstMessage* message, gpointer user_data)
{
    Impl* self = static_cast<Impl*>(user_data);
    self->HandleBusMessage(message);
    return TRUE;
}

NativeMediaError NativeMediaCtrl::Impl::MapGstErrorToError(GError* error)
{
    if (g_error_matches(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_AUTHORIZED)) {
        return NativeMediaError::AuthenticationFailed;
    } else if (g_error_matches(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND)) {
        return NativeMediaError::StreamNotFound;
    } else if (g_error_matches(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_OPEN_READ) ||
               g_error_matches(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_OPEN_READ_WRITE)) {
        return NativeMediaError::NetworkUnreachable;
    } else if (g_error_matches(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_BUSY)) {
        return NativeMediaError::ConnectionTimeout;
    } else if (g_error_matches(error, GST_STREAM_ERROR, GST_STREAM_ERROR_CODEC_NOT_FOUND) ||
               g_error_matches(error, GST_STREAM_ERROR, GST_STREAM_ERROR_WRONG_TYPE)) {
        return NativeMediaError::UnsupportedFormat;
    } else if (g_error_matches(error, GST_STREAM_ERROR, GST_STREAM_ERROR_DECODE)) {
        return NativeMediaError::DecoderError;
    }
    return NativeMediaError::InternalError;
}

void NativeMediaCtrl::Impl::HandleBusMessage(GstMessage* message)
{
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &err, &debug);
            BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Linux: Error: " << err->message;
            if (debug) {
                BOOST_LOG_TRIVIAL(debug) << "NativeMediaCtrl_Linux: Debug: " << debug;
                g_free(debug);
            }

            m_error = MapGstErrorToError(err);
            g_error_free(err);

            m_state = NativeMediaState::Error;
            NotifyStateChanged();

            wxCommandEvent event(EVT_NATIVE_MEDIA_ERROR);
            event.SetEventObject(m_owner);
            event.SetInt(static_cast<int>(m_error));
            wxPostEvent(m_owner, event);

            m_owner->ScheduleRetry();
            break;
        }

        case GST_MESSAGE_EOS:
            BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Linux: End of stream";
            m_state = NativeMediaState::Stopped;
            NotifyStateChanged();
            break;

        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(m_pipeline)) {
                GstState old_state, new_state;
                gst_message_parse_state_changed(message, &old_state, &new_state, nullptr);

                if (new_state == GST_STATE_PLAYING && m_state != NativeMediaState::Playing) {
                    m_state = NativeMediaState::Playing;
                    m_owner->ResetRetryState();
                    NotifyStateChanged();
                    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Linux: Now playing";
                } else if (new_state == GST_STATE_PAUSED && m_state == NativeMediaState::Playing) {
                    m_state = NativeMediaState::Paused;
                    NotifyStateChanged();
                }
            }
            break;

        case GST_MESSAGE_STREAM_START:
            BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Linux: Stream started";
            break;

        case GST_MESSAGE_BUFFERING: {
            gint percent = 0;
            gst_message_parse_buffering(message, &percent);
            BOOST_LOG_TRIVIAL(debug) << "NativeMediaCtrl_Linux: Buffering " << percent << "%";
            break;
        }

        default:
            break;
    }
}

void NativeMediaCtrl::Impl::Play()
{
    if (m_pipeline) {
        GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Linux: Failed to set pipeline to PLAYING";
            m_error = NativeMediaError::InternalError;
            m_state = NativeMediaState::Error;
            NotifyStateChanged();
        } else {
            BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Linux: Play started";
        }
    }
}

void NativeMediaCtrl::Impl::Stop()
{
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
    }
    m_state = NativeMediaState::Stopped;
    NotifyStateChanged();
    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Linux: Stopped";
}

void NativeMediaCtrl::Impl::Pause()
{
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
    }
}

void NativeMediaCtrl::Impl::UpdateLayout(int width, int height)
{
    if (m_pipeline) {
        GstElement* sink = gst_bin_get_by_name(GST_BIN(m_pipeline), "sink");
        if (sink) {
            if (GST_IS_VIDEO_OVERLAY(sink)) {
                gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink));
            }
            gst_object_unref(sink);
        }
    }
}

void NativeMediaCtrl::Impl::NotifyStateChanged()
{
    wxCommandEvent event(EVT_NATIVE_MEDIA_STATE_CHANGED);
    event.SetEventObject(m_owner);
    event.SetInt(static_cast<int>(m_state));
    wxPostEvent(m_owner, event);
}

NativeMediaCtrl::NativeMediaCtrl(wxWindow* parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_retry_enabled(true)
    , m_retry_count(0)
{
    SetBackgroundColour(*wxBLACK);
    m_retry_timer.SetOwner(this);
    Bind(wxEVT_TIMER, &NativeMediaCtrl::OnRetryTimer, this, m_retry_timer.GetId());

    GtkWidget* widget = (GtkWidget*)GetHandle();
    m_impl = std::make_unique<Impl>(this, widget);
}

NativeMediaCtrl::~NativeMediaCtrl()
{
    m_retry_timer.Stop();
}

bool NativeMediaCtrl::Load(const wxString& url)
{
    if (!IsSupported(url)) {
        BOOST_LOG_TRIVIAL(warning) << "NativeMediaCtrl: Unsupported URL format: " << url.ToStdString();
        return false;
    }

    m_current_url = url;
    ResetRetryState();

    StreamType type = DetectStreamType(url);
    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl: Loading " << StreamTypeToString(type).ToStdString()
                            << " stream: " << url.ToStdString();

    return m_impl->Load(url);
}

void NativeMediaCtrl::Play()
{
    if (m_impl) {
        m_impl->Play();
    }
}

void NativeMediaCtrl::Stop()
{
    m_retry_timer.Stop();
    ResetRetryState();
    if (m_impl) {
        m_impl->Stop();
    }
}

void NativeMediaCtrl::Pause()
{
    if (m_impl) {
        m_impl->Pause();
    }
}

NativeMediaState NativeMediaCtrl::GetState() const
{
    return m_impl ? m_impl->GetState() : NativeMediaState::Stopped;
}

wxSize NativeMediaCtrl::GetVideoSize() const
{
    return m_impl ? m_impl->GetVideoSize() : wxSize(1920, 1080);
}

NativeMediaError NativeMediaCtrl::GetLastError() const
{
    return m_impl ? m_impl->GetLastError() : NativeMediaError::None;
}

void NativeMediaCtrl::DoSetSize(int x, int y, int width, int height, int sizeFlags)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
    if (m_impl && width > 0 && height > 0) {
        m_impl->UpdateLayout(width, height);
    }
}

}} // namespace Slic3r::GUI

#endif // __linux__
