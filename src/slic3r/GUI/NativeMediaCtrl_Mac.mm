#include "wx/wxprec.h"
#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#import "NativeMediaCtrl.h"
#include <boost/log/trivial.hpp>

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

namespace Slic3r { namespace GUI {

class NativeMediaCtrl::Impl
{
public:
    Impl(NativeMediaCtrl* owner, NSView* view);
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
    static GstFlowReturn OnNewSample(GstAppSink* sink, gpointer user_data);
    static gboolean BusCallback(GstBus* bus, GstMessage* message, gpointer user_data);
    void HandleBusMessage(GstMessage* message);
    void HandleNewSample(GstSample* sample);
    void NotifyStateChanged();
    void NotifySizeChanged();
    NativeMediaError MapGstErrorToError(GError* error);
    std::string BuildPipelineForRtsp(const std::string& url, const StreamCredentials& creds);
    std::string BuildPipelineForMjpeg(const std::string& url, const StreamCredentials& creds);

    void UpdateStatusText(const std::string& text);

    NativeMediaCtrl* m_owner;
    NSView* m_view;
    CALayer* m_image_layer;
    CATextLayer* m_status_layer;
    GstElement* m_pipeline;
    GstElement* m_appsink;
    gulong m_bus_watch_id;

    NativeMediaState m_state;
    NativeMediaError m_error;
    wxSize m_video_size;
    wxString m_url;
};

NativeMediaCtrl::Impl::Impl(NativeMediaCtrl* owner, NSView* view)
    : m_owner(owner)
    , m_view(view)
    , m_image_layer(nil)
    , m_status_layer(nil)
    , m_pipeline(nullptr)
    , m_appsink(nullptr)
    , m_bus_watch_id(0)
    , m_state(NativeMediaState::Stopped)
    , m_error(NativeMediaError::None)
    , m_video_size(1920, 1080)
{
    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }

    m_view.wantsLayer = YES;
    m_view.layer = [[CALayer alloc] init];
    m_view.layer.backgroundColor = [[NSColor blackColor] CGColor];
    m_view.layer.masksToBounds = YES;

    m_image_layer = [[CALayer alloc] init];
    m_image_layer.contentsGravity = kCAGravityResizeAspect;
    m_image_layer.backgroundColor = [[NSColor blackColor] CGColor];
    m_image_layer.frame = m_view.layer.bounds;
    m_image_layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    [m_view.layer addSublayer:m_image_layer];

    m_status_layer = [[CATextLayer alloc] init];
    m_status_layer.alignmentMode = kCAAlignmentCenter;
    m_status_layer.foregroundColor = [[NSColor whiteColor] CGColor];
    m_status_layer.backgroundColor = [[NSColor colorWithWhite:0.0 alpha:0.7] CGColor];
    m_status_layer.cornerRadius = 8.0;
    m_status_layer.fontSize = 14.0;
    m_status_layer.contentsScale = [[NSScreen mainScreen] backingScaleFactor];
    m_status_layer.wrapped = YES;
    m_status_layer.hidden = YES;
    [m_view.layer addSublayer:m_status_layer];

    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: GStreamer implementation initialized";
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
    m_appsink = nullptr;
}

bool NativeMediaCtrl::Impl::Load(const wxString& url)
{
    CleanupPipeline();
    m_url = url;
    m_error = NativeMediaError::None;
    m_state = NativeMediaState::Loading;
    UpdateStatusText("Connecting...");
    NotifyStateChanged();

    SetupPipeline(url);
    return m_pipeline != nullptr;
}

std::string NativeMediaCtrl::Impl::BuildPipelineForRtsp(const std::string& url, const StreamCredentials& creds)
{
    std::string pipeline;
    std::string clean_url = creds.HasCredentials() ? creds.BuildUrlWithoutCredentials() : url;

    pipeline = "rtspsrc location=\"" + clean_url + "\" latency=200 buffer-mode=auto ";
    pipeline += "protocols=tcp+udp-mcast+udp ";
    pipeline += "tcp-timeout=5000000 ";

    if (creds.HasCredentials()) {
        pipeline += "user-id=\"" + creds.username + "\" ";
        pipeline += "user-pw=\"" + creds.password + "\" ";
    }

    pipeline += "! decodebin ! videoconvert ! videoscale ! ";
    pipeline += "video/x-raw,format=BGRA ! ";
    pipeline += "appsink name=sink emit-signals=true sync=false max-buffers=2 drop=true";

    return pipeline;
}

std::string NativeMediaCtrl::Impl::BuildPipelineForMjpeg(const std::string& url, const StreamCredentials& creds)
{
    std::string pipeline;
    std::string clean_url = creds.HasCredentials() ? creds.BuildUrlWithoutCredentials() : url;

    pipeline = "souphttpsrc location=\"" + clean_url + "\" ";
    pipeline += "is-live=true do-timestamp=true timeout=10 ";

    if (creds.HasCredentials()) {
        pipeline += "user-id=\"" + creds.username + "\" ";
        pipeline += "user-pw=\"" + creds.password + "\" ";
    }

    pipeline += "! multipartdemux ! jpegdec ! videoconvert ! ";
    pipeline += "video/x-raw,format=BGRA ! ";
    pipeline += "appsink name=sink emit-signals=true sync=false max-buffers=2 drop=true";

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
        BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: Setting up RTSP pipeline";
    } else if (type == StreamType::MJPEG_HTTP || type == StreamType::MJPEG_HTTPS) {
        pipeline_desc = BuildPipelineForMjpeg(url_str, creds);
        BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: Setting up MJPEG pipeline";
    } else {
        pipeline_desc = "playbin uri=\"" + url_str + "\" ! videoconvert ! video/x-raw,format=BGRA ! appsink name=sink emit-signals=true sync=false";
        BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: Setting up generic pipeline";
    }

    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: Pipeline: " << pipeline_desc;

    GError* error = nullptr;
    m_pipeline = gst_parse_launch(pipeline_desc.c_str(), &error);

    if (error) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Mac: Pipeline creation failed: " << error->message;
        m_error = NativeMediaError::InternalError;
        g_error_free(error);
        m_state = NativeMediaState::Error;
        NotifyStateChanged();
        return;
    }

    if (!m_pipeline) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Mac: Pipeline is null";
        m_error = NativeMediaError::InternalError;
        m_state = NativeMediaState::Error;
        NotifyStateChanged();
        return;
    }

    m_appsink = gst_bin_get_by_name(GST_BIN(m_pipeline), "sink");
    if (m_appsink) {
        GstAppSinkCallbacks callbacks = {nullptr, nullptr, OnNewSample, nullptr};
        gst_app_sink_set_callbacks(GST_APP_SINK(m_appsink), &callbacks, this, nullptr);
        gst_object_unref(m_appsink);
    }

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
    m_bus_watch_id = gst_bus_add_watch(bus, BusCallback, this);
    gst_object_unref(bus);

    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: Pipeline created successfully";
}

GstFlowReturn NativeMediaCtrl::Impl::OnNewSample(GstAppSink* sink, gpointer user_data)
{
    Impl* impl = static_cast<Impl*>(user_data);

    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (sample) {
        impl->HandleNewSample(sample);
        gst_sample_unref(sample);
    }

    return GST_FLOW_OK;
}

void NativeMediaCtrl::Impl::HandleNewSample(GstSample* sample)
{
    GstCaps* caps = gst_sample_get_caps(sample);
    if (!caps) return;

    GstStructure* structure = gst_caps_get_structure(caps, 0);
    int width = 0, height = 0;
    gst_structure_get_int(structure, "width", &width);
    gst_structure_get_int(structure, "height", &height);

    if (width <= 0 || height <= 0) return;

    bool size_changed = (m_video_size.GetWidth() != width || m_video_size.GetHeight() != height);

    if (m_state == NativeMediaState::Loading) {
        m_state = NativeMediaState::Playing;
        m_video_size = wxSize(width, height);
        m_owner->ResetRetryState();
        UpdateStatusText("");
        NotifyStateChanged();
        NotifySizeChanged();
        BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: Now playing " << width << "x" << height;
    } else if (size_changed) {
        m_video_size = wxSize(width, height);
        NotifySizeChanged();
        BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: Video size changed to " << width << "x" << height;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) return;

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return;

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(
        (void*)map.data,
        width, height,
        8,
        width * 4,
        colorSpace,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little
    );

    if (context) {
        CGImageRef image = CGBitmapContextCreateImage(context);
        if (image) {
            CALayer* layer = [m_image_layer retain];
            dispatch_async(dispatch_get_main_queue(), ^{
                if (layer.superlayer != nil) {
                    [CATransaction begin];
                    [CATransaction setDisableActions:YES];
                    layer.contents = (__bridge id)image;
                    [CATransaction commit];
                }
                CGImageRelease(image);
                [layer release];
            });
        }
        CGContextRelease(context);
    }

    CGColorSpaceRelease(colorSpace);
    gst_buffer_unmap(buffer, &map);
}

gboolean NativeMediaCtrl::Impl::BusCallback(GstBus* bus, GstMessage* message, gpointer user_data)
{
    Impl* impl = static_cast<Impl*>(user_data);
    impl->HandleBusMessage(message);
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
            BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Mac: Error: " << err->message;
            if (debug) {
                BOOST_LOG_TRIVIAL(debug) << "NativeMediaCtrl_Mac: Debug: " << debug;
                g_free(debug);
            }

            m_error = MapGstErrorToError(err);
            g_error_free(err);

            std::string error_msg;
            switch (m_error) {
                case NativeMediaError::NetworkUnreachable: error_msg = "Network unreachable"; break;
                case NativeMediaError::AuthenticationFailed: error_msg = "Authentication failed"; break;
                case NativeMediaError::StreamNotFound: error_msg = "Stream not found"; break;
                case NativeMediaError::UnsupportedFormat: error_msg = "Unsupported format"; break;
                case NativeMediaError::DecoderError: error_msg = "Decoder error"; break;
                case NativeMediaError::ConnectionTimeout: error_msg = "Connection timeout"; break;
                case NativeMediaError::TLSError: error_msg = "TLS/SSL error"; break;
                default: error_msg = "Connection error"; break;
            }
            UpdateStatusText(error_msg + " - Retrying...");

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
            BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: End of stream";
            m_state = NativeMediaState::Stopped;
            NotifyStateChanged();
            break;

        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(m_pipeline)) {
                GstState old_state, new_state;
                gst_message_parse_state_changed(message, &old_state, &new_state, nullptr);
                BOOST_LOG_TRIVIAL(debug) << "NativeMediaCtrl_Mac: State changed from "
                                         << gst_element_state_get_name(old_state) << " to "
                                         << gst_element_state_get_name(new_state);
            }
            break;

        case GST_MESSAGE_STREAM_START:
            BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: Stream started";
            break;

        case GST_MESSAGE_BUFFERING: {
            gint percent = 0;
            gst_message_parse_buffering(message, &percent);
            BOOST_LOG_TRIVIAL(debug) << "NativeMediaCtrl_Mac: Buffering " << percent << "%";
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
            BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Mac: Failed to set pipeline to PLAYING";
            m_error = NativeMediaError::InternalError;
            m_state = NativeMediaState::Error;
            NotifyStateChanged();
        } else {
            BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: Play started";
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
    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: Stopped";
}

void NativeMediaCtrl::Impl::Pause()
{
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
    }
}

void NativeMediaCtrl::Impl::UpdateLayout(int width, int height)
{
    CALayer* imageLayer = [m_image_layer retain];
    CATextLayer* statusLayer = [m_status_layer retain];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (imageLayer.superlayer == nil || statusLayer.superlayer == nil) {
            [imageLayer release];
            [statusLayer release];
            return;
        }

        [CATransaction begin];
        [CATransaction setDisableActions:YES];

        CGRect bounds = CGRectMake(0, 0, width, height);
        imageLayer.frame = bounds;

        if (!statusLayer.hidden) {
            CGFloat labelWidth = fmin(width - 40, 300);
            CGFloat labelHeight = 36;
            CGFloat x = (width - labelWidth) / 2.0;
            CGFloat y = (height - labelHeight) / 2.0;
            statusLayer.frame = CGRectMake(x, y, labelWidth, labelHeight);
        }

        [CATransaction commit];
        [imageLayer release];
        [statusLayer release];
    });
}

void NativeMediaCtrl::Impl::NotifyStateChanged()
{
    wxCommandEvent event(EVT_NATIVE_MEDIA_STATE_CHANGED);
    event.SetEventObject(m_owner);
    event.SetInt(static_cast<int>(m_state));
    wxPostEvent(m_owner, event);
}

void NativeMediaCtrl::Impl::NotifySizeChanged()
{
    wxCommandEvent event(EVT_NATIVE_MEDIA_SIZE_CHANGED);
    event.SetEventObject(m_owner);
    wxPostEvent(m_owner, event);
}

void NativeMediaCtrl::Impl::UpdateStatusText(const std::string& text)
{
    std::string captured_text = text;
    CATextLayer* statusLayer = [m_status_layer retain];
    NSView* view = [m_view retain];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (statusLayer.superlayer == nil) {
            [statusLayer release];
            [view release];
            return;
        }

        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        if (captured_text.empty()) {
            statusLayer.string = @"";
            statusLayer.hidden = YES;
            BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: Status text cleared";
        } else {
            NSString* nsText = [NSString stringWithUTF8String:captured_text.c_str()];
            statusLayer.string = nsText;
            statusLayer.hidden = NO;

            CGRect bounds = view.layer.bounds;
            BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: View bounds: " << bounds.size.width << "x" << bounds.size.height;

            CGFloat labelWidth = 250;
            CGFloat labelHeight = 36;
            if (bounds.size.width > 40 && bounds.size.height > 40) {
                labelWidth = fmin(bounds.size.width - 40, 300);
                CGFloat x = (bounds.size.width - labelWidth) / 2.0;
                CGFloat y = (bounds.size.height - labelHeight) / 2.0;
                statusLayer.frame = CGRectMake(x, y, labelWidth, labelHeight);
            } else {
                statusLayer.frame = CGRectMake(20, 20, labelWidth, labelHeight);
            }

            BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Mac: Status text set to '" << captured_text << "'";
        }
        [CATransaction commit];
        [statusLayer release];
        [view release];
    });
}

NativeMediaCtrl::NativeMediaCtrl(wxWindow* parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_retry_enabled(true)
    , m_retry_count(0)
{
    SetBackgroundColour(*wxBLACK);
    m_retry_timer.SetOwner(this);
    Bind(wxEVT_TIMER, &NativeMediaCtrl::OnRetryTimer, this, m_retry_timer.GetId());

    NSView* view = (NSView*)GetHandle();
    m_impl = std::make_unique<Impl>(this, view);
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
