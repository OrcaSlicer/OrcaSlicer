#include "wxMediaCtrl2.h"
#include "libslic3r/Time.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "LinuxDisplayBackend.hpp"
#include <boost/filesystem/operations.hpp>
#include <string>
#ifdef __WIN32__
#include <winuser.h>
#include <versionhelpers.h>
#include <wx/msw/registry.h>
#include <shellapi.h>
#endif

#ifdef __LINUX__
#include "Printer/gstbambusrc.h"
#include <gst/gst.h> // main gstreamer header
#endif

#if defined(__LINUX__) && defined(__WXGTK__)
#include <gtk/gtk.h>
#include <wx/nativewin.h>

namespace {
bool ensure_gstreamer_initialized_for_liveview()
{
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        BOOST_LOG_TRIVIAL(error) << "wxMediaCtrl2: gst_init_check failed before native Wayland liveview setup"
            << (error ? std::string(": ") + error->message : std::string());
        if (error)
            g_error_free(error);
        return false;
    }

    return true;
}

bool is_gstreamer_feature_available(const char* feature)
{
    if (!ensure_gstreamer_initialized_for_liveview())
        return false;

    GstElementFactory* factory = gst_element_factory_find(feature);
    if (!factory)
        return false;

    gst_object_unref(factory);
    return true;
}

void set_gstreamer_feature_rank(const char* feature, guint rank)
{
    GstElementFactory* factory = gst_element_factory_find(feature);
    if (!factory)
        return;

    gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), rank);
    gst_object_unref(factory);
}

void configure_wayland_gstreamer_liveview_path()
{
    static bool configured = false;
    if (configured)
        return;
    configured = true;

    if (!ensure_gstreamer_initialized_for_liveview())
        return;

    // Prefer software decode for Bambu liveview on Wayland/NVIDIA, where
    // zero-copy GL/DMABUF display paths can be fragile. Keep hardware
    // decoders available as lower-ranked fallbacks for VAAPI/NVDEC/V4L2-only
    // installations instead of passing preflight and then blocking autoplug.
    set_gstreamer_feature_rank("avdec_h264", GST_RANK_PRIMARY + 300);
    set_gstreamer_feature_rank("openh264dec", GST_RANK_PRIMARY + 100);
    set_gstreamer_feature_rank("nvh264dec", GST_RANK_MARGINAL);
    set_gstreamer_feature_rank("vaapih264dec", GST_RANK_MARGINAL);
    set_gstreamer_feature_rank("vah264dec", GST_RANK_MARGINAL);
    set_gstreamer_feature_rank("v4l2h264dec", GST_RANK_MARGINAL);
}
}

#endif // defined(__LINUX__) && defined(__WXGTK__)

#ifdef __LINUX__
extern "C" int gst_bambu_last_error;

class WXDLLIMPEXP_MEDIA
    wxGStreamerMediaBackend : public wxMediaBackendCommonBase
{
public:
    GstElement *m_playbin; // GStreamer media element
};
#endif

wxDEFINE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);

wxMediaCtrl2::wxMediaCtrl2(wxWindow *parent)
{
#if defined(__LINUX__) && defined(__WXGTK__)
    m_native_wayland = Slic3r::GUI::is_running_on_wayland();
    if (m_native_wayland && is_gstreamer_feature_available("gtksink"))
        configure_wayland_gstreamer_liveview_path();
    else if (m_native_wayland) {
        m_gtk_sink_error = _L("Native Wayland liveview requires the GStreamer GTK video sink. Please install the gtksink plugin for GStreamer, then restart OrcaSlicer.");
        BOOST_LOG_TRIVIAL(warning) << "wxMediaCtrl2: native Wayland liveview disabled because GStreamer gtksink is unavailable";
    }
#endif
#ifdef __WIN32__
    auto hModExe = GetModuleHandle(NULL);
    // BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl2: GetModuleHandle " << hModExe;
    auto NvOptimusEnablement = (DWORD *) GetProcAddress(hModExe, "NvOptimusEnablement");
    auto AmdPowerXpressRequestHighPerformance = (int *) GetProcAddress(hModExe, "AmdPowerXpressRequestHighPerformance");
    if (NvOptimusEnablement) {
        // BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl2: NvOptimusEnablement " << *NvOptimusEnablement;
        *NvOptimusEnablement = 0;
    }
    if (AmdPowerXpressRequestHighPerformance) {
        // BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl2: AmdPowerXpressRequestHighPerformance " << *AmdPowerXpressRequestHighPerformance;
        *AmdPowerXpressRequestHighPerformance = 0;
    }
#endif
#if defined(__LINUX__) && defined(__WXGTK__)
    if (m_native_wayland)
        wxControl::Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    else
#endif
        wxMediaCtrl::Create(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxMEDIACTRLPLAYERCONTROLS_NONE);
#ifdef __LINUX__
    gstbambusrc_register();
#ifdef __WXGTK__
    if (m_native_wayland && m_gtk_sink_error.empty())
        m_use_gtk_sink = CreateGtkSinkPlayer();
    if (m_native_wayland && !m_use_gtk_sink && m_gtk_sink_error.empty())
        m_gtk_sink_error = _L("Failed to initialize the native Wayland GStreamer video sink. Please check your GStreamer GTK plugin installation.");
#endif
    if (!m_use_gtk_sink && m_imp) {
        auto playbin = reinterpret_cast<wxGStreamerMediaBackend *>(m_imp)->m_playbin;
        g_object_set(G_OBJECT(playbin),
                     "audio-sink", nullptr,
                     nullptr);
    } else if (!m_use_gtk_sink) {
        BOOST_LOG_TRIVIAL(warning) << "wxMediaCtrl2: wxMediaCtrl backend is unavailable";
    }
    Bind(wxEVT_MEDIA_LOADED, [this](auto & e) {
        m_loaded = true;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(0);
        event.SetEventObject(this);
        wxPostEvent(this, event);
    });
#endif
}

wxMediaCtrl2::~wxMediaCtrl2()
{
#if defined(__LINUX__) && defined(__WXGTK__)
    DestroyGtkSinkPlayer();
#endif
}

#if defined(__LINUX__) && defined(__WXGTK__)
bool wxMediaCtrl2::CreateGtkSinkPlayer()
{
    GstElement *playbin = gst_element_factory_make("playbin", "orca-wayland-gtk-playbin");
    if (!playbin)
        return false;

    GError *error = nullptr;
    GstElement *video_sink = gst_parse_bin_from_description(
        "videoconvert ! videoscale ! video/x-raw,format=BGRx ! gtksink name=orca_wayland_gtksink sync=false",
        TRUE,
        &error);
    if (!video_sink) {
        BOOST_LOG_TRIVIAL(warning) << "wxMediaCtrl2: failed to create gtksink video bin"
            << (error ? std::string(": ") + error->message : std::string());
        if (error)
            g_error_free(error);
        gst_object_unref(playbin);
        return false;
    }

    GstElement *gtk_sink = gst_bin_get_by_name(GST_BIN(video_sink), "orca_wayland_gtksink");
    if (!gtk_sink) {
        BOOST_LOG_TRIVIAL(warning) << "wxMediaCtrl2: failed to find gtksink in video bin";
        gst_object_unref(video_sink);
        gst_object_unref(playbin);
        return false;
    }

    GtkWidget *gtk_widget = nullptr;
    g_object_get(G_OBJECT(gtk_sink), "widget", &gtk_widget, nullptr);
    if (!gtk_widget) {
        BOOST_LOG_TRIVIAL(warning) << "wxMediaCtrl2: gtksink did not expose a GtkWidget";
        gst_object_unref(gtk_sink);
        gst_object_unref(video_sink);
        gst_object_unref(playbin);
        return false;
    }

    gtk_widget_show(gtk_widget);
    m_gtk_video_window = new wxNativeWindow(this, wxID_ANY, gtk_widget);
    m_gtk_video_window->Show();
    g_object_unref(gtk_widget);

    g_object_set(G_OBJECT(playbin),
                 "video-sink", video_sink,
                 "audio-sink", nullptr,
                 nullptr);
    gst_object_unref(video_sink);

    m_gtk_playbin = playbin;
    m_gtk_sink = gtk_sink;

    GstBus *bus = gst_element_get_bus(playbin);
    m_gtk_bus_watch_id = gst_bus_add_watch(bus, [](GstBus *, GstMessage *message, gpointer data) -> gboolean {
        auto *self = static_cast<wxMediaCtrl2 *>(data);
        if (!self || !self->m_gtk_playbin)
            return G_SOURCE_REMOVE;

        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR:
        {
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            BOOST_LOG_TRIVIAL(warning) << "wxMediaCtrl2: gtksink pipeline error"
                << (error ? std::string(": ") + error->message : std::string())
                << (debug ? std::string(" debug: ") + debug : std::string());
            if (error)
                g_error_free(error);
            if (debug)
                g_free(debug);

            self->m_error = gst_bambu_last_error ? gst_bambu_last_error : 2;
            self->m_loaded = false;
            self->m_gtk_state = wxMEDIASTATE_STOPPED;
            self->PostGtkSinkStateEvent(self->GetId());
            break;
        }
        case GST_MESSAGE_EOS:
            self->m_loaded = false;
            self->m_gtk_state = wxMEDIASTATE_STOPPED;
            self->PostGtkSinkStateEvent(self->GetId());
            break;
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(self->m_gtk_playbin)) {
                GstState old_state;
                GstState new_state;
                GstState pending_state;
                gst_message_parse_state_changed(message, &old_state, &new_state, &pending_state);

                if (new_state == GST_STATE_PLAYING) {
                    self->m_loaded = true;
                    self->m_gtk_state = wxMEDIASTATE_PLAYING;
                    self->PostGtkSinkStateEvent();
                } else if (new_state == GST_STATE_PAUSED && old_state < GST_STATE_PAUSED) {
                    // Treat only upward READY/NULL -> PAUSED as load completion.
                    // PLAYING -> PAUSED is a normal teardown step before NULL.
                    self->m_loaded = true;
                    self->m_gtk_state = wxMEDIASTATE_PAUSED;
                    self->PostGtkSinkStateEvent();
                } else if (new_state <= GST_STATE_READY && old_state >= GST_STATE_PAUSED) {
                    self->m_loaded = false;
                    self->m_gtk_state = wxMEDIASTATE_STOPPED;
                    self->PostGtkSinkStateEvent();
                }
            }
            break;
        default:
            break;
        }

        return G_SOURCE_CONTINUE;
    }, this);
    gst_object_unref(bus);

    BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl2: using GTK native Wayland video sink";
    return true;
}

void wxMediaCtrl2::DestroyGtkSinkPlayer()
{
    if (m_gtk_bus_watch_id) {
        g_source_remove(m_gtk_bus_watch_id);
        m_gtk_bus_watch_id = 0;
    }

    if (m_gtk_playbin) {
        gst_element_set_state(m_gtk_playbin, GST_STATE_NULL);
    }

    if (m_gtk_video_window) {
        m_gtk_video_window->Destroy();
        m_gtk_video_window = nullptr;
    }

    if (m_gtk_playbin) {
        gst_object_unref(m_gtk_playbin);
        m_gtk_playbin = nullptr;
    }

    if (m_gtk_sink) {
        gst_object_unref(m_gtk_sink);
        m_gtk_sink = nullptr;
    }

}

void wxMediaCtrl2::PostGtkSinkStateEvent(int id)
{
    wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
    event.SetId(id);
    event.SetEventObject(this);
    wxPostEvent(this, event);
}
#endif // defined(__LINUX__) && defined(__WXGTK__)

#define CLSID_BAMBU_SOURCE L"{233E64FB-2041-4A6C-AFAB-FF9BCF83E7AA}"

void wxMediaCtrl2::Load(wxURI url)
{
#ifdef __WIN32__
    InvalidateBestSize();
    if (m_imp == nullptr) {
        static bool notified = false;
        if (!notified) CallAfter([] {
            auto res = wxMessageBox(_L("Windows Media Player is required for this task! Do you want to enable 'Windows Media Player' for your operation system?"), _L("Error"), wxOK | wxCANCEL);
            if (res == wxOK) {
                wxString url = IsWindows10OrGreater() 
                        ? "ms-settings:optionalfeatures?activationSource=SMC-Article-14209" 
                        : "https://support.microsoft.com/en-au/windows/get-windows-media-player-81718e0d-cfce-25b1-aee3-94596b658287";
                wxExecute("cmd /c start " + url, wxEXEC_HIDE_CONSOLE);
            }
        });
        m_error = 100;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
        return;
    }
    {
        wxRegKey key11(wxRegKey::HKCU, L"SOFTWARE\\Classes\\CLSID\\" CLSID_BAMBU_SOURCE L"\\InProcServer32");
        wxRegKey key12(wxRegKey::HKCR, L"CLSID\\" CLSID_BAMBU_SOURCE L"\\InProcServer32");
        wxString path = key11.Exists() ? key11.QueryDefaultValue() 
                                       : key12.Exists() ? key12.QueryDefaultValue() : wxString{};
        wxRegKey key2(wxRegKey::HKCR, "bambu");
        wxString clsid;
        if (key2.Exists())
            key2.QueryRawValue("Source Filter", clsid);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": clsid %1% path %2%") % clsid % path;

        std::string             data_dir_str = Slic3r::data_dir();
        boost::filesystem::path data_dir_path(data_dir_str);
        auto                    dll_path = data_dir_path / "plugins" / "BambuSource.dll";
        if (path.empty() || !wxFile::Exists(path) || clsid != CLSID_BAMBU_SOURCE) {
            if (boost::filesystem::exists(dll_path)) {
                CallAfter(
                    [dll_path] {
                    int res = wxMessageBox(_L("BambuSource has not correctly been registered for media playing! Press Yes to re-register it. You will be promoted twice"), _L("Error"), wxYES_NO);
                    if (res == wxYES) {
                        std::string regContent = R"(Windows Registry Editor Version 5.00
                                                    [HKEY_CLASSES_ROOT\bambu]
                                                    "Source Filter"="{233E64FB-2041-4A6C-AFAB-FF9BCF83E7AA}"
                                                    )";

                        auto reg_path = (fs::temp_directory_path() / fs::unique_path()).replace_extension(".reg");
                        std::ofstream temp_reg_file(reg_path.c_str());
                        if (!temp_reg_file) {
                            return false;
                        }
                        temp_reg_file << regContent;
                        temp_reg_file.close();
                        auto sei_params = L"/q /s " + reg_path.wstring();
                        SHELLEXECUTEINFO sei{sizeof(sei), SEE_MASK_NOCLOSEPROCESS, NULL,   L"open",
                                             L"regedit",  sei_params.c_str(),SW_HIDE,SW_HIDE};
                        ::ShellExecuteEx(&sei);

                        wstring quoted_dll_path = L"\"" + dll_path.wstring() + L"\"";
                        SHELLEXECUTEINFO info{sizeof(info), 0, NULL, L"runas", L"regsvr32", quoted_dll_path.c_str(), SW_HIDE };
                        ::ShellExecuteEx(&info);
                        fs::remove(reg_path);
                    }
                    return true;
                });
            } else {
                CallAfter([] {
                    wxMessageBox(_L("Missing BambuSource component registered for media playing! Please re-install OrcaSlicer or seek community help."), _L("Error"), wxOK);
                });
            }
            m_error = clsid != CLSID_BAMBU_SOURCE ? 101 : path.empty() ? 102 : 103;
            wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
            event.SetId(GetId());
            event.SetEventObject(this);
            wxPostEvent(this, event);
            return;
        }
        if (path != dll_path) {
            static bool notified = false;
            if (!notified) CallAfter([dll_path] {
                int res = wxMessageBox(_L("Using a BambuSource from a different install, video play may not work correctly! Press Yes to fix it."), _L("Warning"), wxYES_NO | wxICON_WARNING);
                if (res == wxYES) {
                    auto path = dll_path.wstring();
                    if (path.find(L' ') != std::wstring::npos)
                        path = L"\"" + path + L"\"";
                    SHELLEXECUTEINFO info{sizeof(info), 0, NULL, L"open", L"regsvr32", path.c_str(), SW_HIDE};
                    ::ShellExecuteEx(&info);
                }
            });
            notified = true;
        }
        wxRegKey keyWmp(wxRegKey::HKCU, "SOFTWARE\\Microsoft\\MediaPlayer\\Player\\Extensions\\.");
        keyWmp.Create();
        long permissions = 0;
        if (keyWmp.HasValue("Permissions"))
            keyWmp.QueryValue("Permissions", &permissions);
        if ((permissions & 32) == 0) {
            permissions |= 32;
            keyWmp.SetValue("Permissions", permissions);
        }
    }
    url = wxURI(url.BuildURI().append("&hwnd=").append(boost::lexical_cast<std::string>(GetHandle())).append("&tid=").append(
        boost::lexical_cast<std::string>(GetCurrentThreadId())));
#endif
#ifdef __WXGTK3__
    GstElementFactory *factory;
    int hasplugins = 1;
    
    factory = gst_element_factory_find("h264parse");
    if (!factory) {
        hasplugins = 0;
    } else {
        gst_object_unref(factory);
    }
    
    factory = gst_element_factory_find("openh264dec");
    if (!factory) {
        factory = gst_element_factory_find("avdec_h264");
    }
    if (!factory) {
        factory = gst_element_factory_find("vaapih264dec");
    }
    if (!factory) {
        factory = gst_element_factory_find("vah264dec");
    }
    if (!factory) {
        factory = gst_element_factory_find("nvh264dec");
    }
    if (!factory) {
        factory = gst_element_factory_find("v4l2h264dec");
    }
    if (!factory) {
        hasplugins = 0;
    } else {
        gst_object_unref(factory);
    }
    
    if (!hasplugins) {
        CallAfter([] {
            wxMessageBox(_L("Your system is missing H.264 codecs for GStreamer, which are required to play video. (Try installing the gstreamer1.0-plugins-bad or gstreamer1.0-libav packages, then restart Orca Slicer?)"), _L("Error"), wxOK);
        });
        m_error = 101;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
        return;
    }
    wxLog::EnableLogging(false);
#endif
    m_error = 0;
    m_loaded = false;
#if defined(__LINUX__) && defined(__WXGTK__)
    if (m_use_gtk_sink && m_gtk_playbin) {
        const std::string uri = std::string(url.BuildURI().ToUTF8().data());
        gst_element_set_state(m_gtk_playbin, GST_STATE_NULL);
        g_object_set(G_OBJECT(m_gtk_playbin), "uri", uri.c_str(), nullptr);
        m_gtk_state = wxMEDIASTATE_STOPPED;
        GstStateChangeReturn state = gst_element_set_state(m_gtk_playbin, GST_STATE_PAUSED);
        if (state == GST_STATE_CHANGE_FAILURE) {
            m_error = gst_bambu_last_error ? gst_bambu_last_error : 2;
            PostGtkSinkStateEvent(GetId());
        }
        return;
    }
    if (!m_imp) {
        m_error = m_native_wayland && !m_gtk_sink_error.empty() ? 104 : 100;
        m_loaded = false;
        if (m_native_wayland && !m_gtk_sink_error.empty() && !m_gtk_sink_error_notified) {
            m_gtk_sink_error_notified = true;
            const wxString message = m_gtk_sink_error;
            CallAfter([message] {
                wxMessageBox(message, _L("Error"), wxOK);
            });
        }
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
        return;
    }
#endif
    wxMediaCtrl::Load(url);
}

void wxMediaCtrl2::Play()
{
#if defined(__LINUX__) && defined(__WXGTK__)
    if (m_use_gtk_sink && m_gtk_playbin) {
        GstStateChangeReturn state = gst_element_set_state(m_gtk_playbin, GST_STATE_PLAYING);
        if (state == GST_STATE_CHANGE_FAILURE) {
            m_error = gst_bambu_last_error ? gst_bambu_last_error : 2;
            m_gtk_state = wxMEDIASTATE_STOPPED;
            PostGtkSinkStateEvent(GetId());
        }
        return;
    }
    if (!m_imp) {
        m_error = m_native_wayland && !m_gtk_sink_error.empty() ? 104 : 100;
        if (m_native_wayland && !m_gtk_sink_error.empty() && !m_gtk_sink_error_notified) {
            m_gtk_sink_error_notified = true;
            const wxString message = m_gtk_sink_error;
            CallAfter([message] {
                wxMessageBox(message, _L("Error"), wxOK);
            });
        }
        PostGtkSinkStateEvent(GetId());
        return;
    }
#endif
    wxMediaCtrl::Play();
}

void wxMediaCtrl2::Stop()
{
#if defined(__LINUX__) && defined(__WXGTK__)
    if (m_use_gtk_sink && m_gtk_playbin) {
        gst_element_set_state(m_gtk_playbin, GST_STATE_NULL);
        m_gtk_state = wxMEDIASTATE_STOPPED;
        m_loaded = false;
        PostGtkSinkStateEvent(0);
        return;
    }
    if (!m_imp)
        return;
#endif
    wxMediaCtrl::Stop();
}

wxMediaState wxMediaCtrl2::GetState()
{
#if defined(__LINUX__) && defined(__WXGTK__)
    if (m_use_gtk_sink && m_gtk_playbin)
        return m_gtk_state;
    if (!m_imp)
        return wxMEDIASTATE_STOPPED;
#endif
    return wxMediaCtrl::GetState();
}

int wxMediaCtrl2::GetLastError() const
{
#ifdef __LINUX__
#ifdef __WXGTK__
    if (m_use_gtk_sink && m_error)
        return m_error;
#endif
    if (m_error)
        return m_error;
    return gst_bambu_last_error;
#else
    return m_error;
#endif
}

wxSize wxMediaCtrl2::GetVideoSize() const
{
#ifdef __LINUX__
    // Gstreamer doesn't give us a VideoSize until we're playing, which
    // confuses the MediaPlayCtrl into claiming that it is stuck
    // "Loading...".  Fake it out for now.
    return m_loaded ? wxSize(1280, 720) : wxSize{};
#else
    wxSize size = m_imp ? m_imp->GetVideoSize() : wxSize(0, 0);
    if (size.GetWidth() > 0)
        const_cast<wxSize&>(m_video_size) = size;
    return size;
#endif
}

wxSize wxMediaCtrl2::DoGetBestSize() const
{
    return {-1, -1};
}

#ifdef __WIN32__

WXLRESULT wxMediaCtrl2::MSWWindowProc(WXUINT   nMsg,
                                   WXWPARAM wParam,
                                   WXLPARAM lParam)
{
    if (nMsg == WM_USER + 1000) {
        wxString msg((wchar_t const *) lParam);
        if (wParam == 1) {
            if (msg.EndsWith("]")) {
                int n = msg.find_last_of('[');
                if (n != wxString::npos) {
                    long val = 0;
                    if (msg.SubString(n + 1, msg.Length() - 2).ToLong(&val))
                        m_error = (int) val;
                }
            } else if (msg.Contains("stat_log")) {
                wxCommandEvent evt(EVT_MEDIA_CTRL_STAT);
                evt.SetEventObject(this);
                evt.SetString(msg.Mid(msg.Find(' ') + 1));
                wxPostEvent(this, evt);
            }
        }
        BOOST_LOG_TRIVIAL(trace) << msg.ToUTF8().data();
        return 0;
    }
    return wxMediaCtrl::MSWWindowProc(nMsg, wParam, lParam);
}

#endif
