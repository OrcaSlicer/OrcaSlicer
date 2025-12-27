#include "libslic3r/Technologies.hpp"
#include "GUI_App.hpp"
#include "GUI_Init.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_Factories.hpp"
#include "slic3r/GUI/UserManager.hpp"
#include "slic3r/GUI/TaskManager.hpp"
#include "format.hpp"
#include "libslic3r_version.h"
#include "Downloader.hpp"

// Localization headers: include libslic3r version first so everything in this file
// uses the slic3r/GUI version (the macros will take precedence over the functions).
// Also, there is a check that the former is not included from slic3r module.
// This is the only place where we want to allow that, so define an override macro.
#define SLIC3R_ALLOW_LIBSLIC3R_I18N_IN_SLIC3R
#include "libslic3r/I18N.hpp"
#undef SLIC3R_ALLOW_LIBSLIC3R_I18N_IN_SLIC3R
#include "slic3r/GUI/I18N.hpp"

#include <algorithm>
#include <iterator>
#include <exception>
#include <cstdlib>
#include <regex>
#include <thread>
#include <string_view>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/beast/core/detail/base64.hpp>

#include <wx/stdpaths.h>
#include <wx/imagpng.h>
#include <wx/display.h>
#include <wx/menu.h>
#include <wx/menuitem.h>
#include <wx/filedlg.h>
#include <wx/progdlg.h>
#include <wx/dir.h>
#include <wx/wupdlock.h>
#include <wx/filefn.h>
#include <wx/sysopt.h>
#include <wx/richmsgdlg.h>
#include <wx/log.h>
#include <wx/intl.h>

#include <wx/dialog.h>
#include <wx/textctrl.h>
#include <wx/splash.h>
#include <wx/fontutil.h>
#include <wx/glcanvas.h>
#include <wx/utils.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#include "libslic3r/Utils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Color.hpp"

#include "GUI.hpp"
#include "GUI_Utils.hpp"
#include "3DScene.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "GLCanvas3D.hpp"
#include "EncodedFilament.hpp"
#include "GeneratedConfig.hpp"

#include "DeviceCore/DevManager.h"

#include "../Utils/PresetUpdater.hpp"
#include "../Utils/PrintHost.hpp"
#include "../Utils/Process.hpp"
#include "../Utils/MacDarkMode.hpp"
#include "../Utils/Http.hpp"
#include "../Utils/InstanceID.hpp"
#include "../Utils/UndoRedo.hpp"
#include "slic3r/Config/Snapshot.hpp"
#include "Preferences.hpp"
#include "Tab.hpp"
#include "SysInfoDialog.hpp"
#include "UpdateDialogs.hpp"
#include "Mouse3DController.hpp"
#include "RemovableDriveManager.hpp"
#include "InstanceCheck.hpp"
#include "NotificationManager.hpp"
#include "UnsavedChangesDialog.hpp"
#include "SavePresetDialog.hpp"
#include "PrintHostDialogs.hpp"
#include "DesktopIntegrationDialog.hpp"
#include "SendSystemInfoDialog.hpp"
#include "ParamsDialog.hpp"
#include "KBShortcutsDialog.hpp"
#include "DownloadProgressDialog.hpp"

#include "BitmapCache.hpp"
#include "Notebook.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/ProgressDialog.hpp"

//BBS: DailyTip and UserGuide Dialog
#include "WebDownPluginDlg.hpp"
#include "WebGuideDialog.hpp"
#include "ReleaseNote.hpp"
#include "PrivacyUpdateDialog.hpp"
#include "ModelMall.hpp"
#include "HintNotification.hpp"

//#ifdef WIN32
//#include "BaseException.h"
//#endif


#ifdef __WXMSW__
#include <dbt.h>
#include <shlobj.h>

#ifdef __WINDOWS__
#ifdef _MSW_DARK_MODE
#include "dark_mode.hpp"
#include "wx/headerctrl.h"
#include "wx/msw/headerctrl.h"

typedef BOOL (WINAPI *LPFN_ISWOW64PROCESS2)(
    HANDLE hProcess,
    USHORT *pProcessMachine,
    USHORT *pNativeMachine
);

#endif // _MSW_DARK_MODE
#endif // __WINDOWS__

#endif
#ifdef _WIN32
#include <boost/dll/runtime_symbol_info.hpp>
#endif

#ifdef WIN32
#include "dev-utils/BaseException.h"
#endif

#if ENABLE_THUMBNAIL_GENERATOR_DEBUG
#include <boost/beast/core/detail/base64.hpp>
#include <boost/nowide/fstream.hpp>
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG

// Needed for forcing menu icons back under gtk2 and gtk3
#if defined(__WXGTK20__) || defined(__WXGTK3__)
    #include <gtk/gtk.h>
#endif

using namespace std::literals;
namespace pt = boost::property_tree;

struct StaticBambuLib
{
    static void reset();
    static void release();
};

namespace Slic3r {
namespace GUI {

class MainFrame;

void start_ping_test()
{
    return;
    wxArrayString output;
    wxExecute("ping www.amazon.com", output, wxEXEC_NODISABLE);

    wxString output_i;
    std::string output_temp;

    for (int i = 0; i < output.size(); i++) {
        output_i = output[i].To8BitData();
        output_temp = output_i.ToStdString(wxConvUTF8);
        BOOST_LOG_TRIVIAL(info) << "ping amazon:" << output_temp;

    }
    wxExecute("ping www.apple.com", output, wxEXEC_NODISABLE);
    for (int i = 0; i < output.size(); i++) {
        output_i = output[i].To8BitData();
        output_temp = output_i.ToStdString(wxConvUTF8);
        BOOST_LOG_TRIVIAL(info) << "ping www.apple.com:" << output_temp;
    }
    wxExecute("ping www.bambulab.com", output, wxEXEC_NODISABLE);
    for (int i = 0; i < output.size(); i++) {
        output_i = output[i].To8BitData();
        output_temp = output_i.ToStdString(wxConvUTF8);
        BOOST_LOG_TRIVIAL(info) << "ping bambulab:" << output_temp;
    }
    //Get GateWay IP
    wxExecute("ping 192.168.0.1", output, wxEXEC_NODISABLE);
    for (int i = 0; i < output.size(); i++) {
        output_i = output[i].To8BitData();
        output_temp = output_i.ToStdString(wxConvUTF8);
        BOOST_LOG_TRIVIAL(info) << "ping 192.168.0.1:" << output_temp;
    }
}

std::string VersionInfo::convert_full_version(std::string short_version)
{
    std::string result = "";
    std::vector<std::string> items;
    boost::split(items, short_version, boost::is_any_of("."));
    if (items.size() == VERSION_LEN) {
        for (int i = 0; i < VERSION_LEN; i++) {
            std::stringstream ss;
            ss << std::setw(2) << std::setfill('0') << items[i];
            result += ss.str();
            if (i != VERSION_LEN - 1)
                result += ".";
        }
        return result;
    }
    return result;
}

std::string VersionInfo::convert_short_version(std::string full_version)
{
    full_version.erase(std::remove(full_version.begin(), full_version.end(), '0'), full_version.end());
    return full_version;
}

static std::string convert_studio_language_to_api(std::string lang_code)
{
    boost::replace_all(lang_code, "_", "-");
    return lang_code;

    /*if (lang_code == "zh_CN")
        return "zh-hans";
    else if (lang_code == "zh_TW")
        return "zh-hant";
    else
        return "en";*/
}

#ifdef _WIN32
bool is_associate_files(std::wstring extend)
{
    wchar_t app_path[MAX_PATH];
    ::GetModuleFileNameW(nullptr, app_path, sizeof(app_path));

    std::wstring prog_id             = L" Orca.Slicer.1";
    std::wstring reg_base            = L"Software\\Classes";
    std::wstring reg_extension       = reg_base + L"\\." + extend;

    wchar_t szValueCurrent[1000];
    DWORD   dwType;
    DWORD   dwSize = sizeof(szValueCurrent);

    int iRC = ::RegGetValueW(HKEY_CURRENT_USER, reg_extension.c_str(), nullptr, RRF_RT_ANY, &dwType, szValueCurrent, &dwSize);

    bool bDidntExist = iRC == ERROR_FILE_NOT_FOUND;

    if (!bDidntExist && ::wcscmp(szValueCurrent, prog_id.c_str()) == 0)
        return true;

    return false;
}
#endif

class SplashScreen : public wxSplashScreen
{
public:
    SplashScreen(const wxBitmap& bitmap, long splashStyle, int milliseconds, wxPoint pos = wxDefaultPosition)
        : wxSplashScreen(bitmap, splashStyle, milliseconds, static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, wxDefaultPosition, wxDefaultSize,
#ifdef __APPLE__
            wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxSTAY_ON_TOP
#else
            wxBORDER_NONE | wxFRAME_NO_TASKBAR
#endif // !__APPLE__
        )
    {
        int init_dpi = get_dpi_for_window(this);
        this->SetPosition(pos);
        this->CenterOnScreen();
        int new_dpi = get_dpi_for_window(this);

        m_scale = (float)(new_dpi) / (float)(init_dpi);

        m_main_bitmap = bitmap;

        scale_bitmap(m_main_bitmap, m_scale);

        // init constant texts and scale fonts
        m_constant_text.init(Label::Body_16);

		// ORCA scale all fonts with monitor scale
        scale_font(m_constant_text.version_font,	m_scale * 2);
        scale_font(m_constant_text.based_on_font,	m_scale * 1.5f);
        scale_font(m_constant_text.credits_font,	m_scale * 2);

        // this font will be used for the action string
        m_action_font = m_constant_text.credits_font;

        // draw logo and constant info text
        Decorate(m_main_bitmap);
        wxGetApp().UpdateFrameDarkUI(this);
    }

    void SetText(const wxString& text)
    {
        set_bitmap(m_main_bitmap);
        if (!text.empty()) {
            wxBitmap bitmap(m_main_bitmap);

            wxMemoryDC memDC;
            memDC.SelectObject(bitmap);
            memDC.SetFont(m_action_font);
            memDC.SetTextForeground(StateColor::darkModeColorFor(wxColour(144, 144, 144)));
            int width = bitmap.GetWidth();
            int text_height = memDC.GetTextExtent(text).GetHeight();
            int text_width = memDC.GetTextExtent(text).GetWidth();
            wxRect text_rect(wxPoint(0, m_action_line_y_position), wxPoint(width, m_action_line_y_position + text_height));
            memDC.DrawLabel(text, text_rect, wxALIGN_CENTER);

            memDC.SelectObject(wxNullBitmap);
            set_bitmap(bitmap);
#ifdef __WXOSX__
            // without this code splash screen wouldn't be updated under OSX
            wxYield();
#endif
        }
    }

    void Decorate(wxBitmap& bmp)
    {
        if (!bmp.IsOk())
            return;

		bool is_dark = wxGetApp().app_config->get("dark_color_mode") == "1";

        // use a memory DC to draw directly onto the bitmap
        wxMemoryDC memDc(bmp);
        
        int width = bmp.GetWidth();
		int height = bmp.GetHeight();

		// Logo
        BitmapCache bmp_cache;
        wxBitmap logo_bmp = *bmp_cache.load_svg(is_dark ? "splash_logo_dark" : "splash_logo", width, height);  // use with full width & height
        memDc.DrawBitmap(logo_bmp, 0, 0, true);

        // Version
        memDc.SetFont(m_constant_text.version_font);
        memDc.SetTextForeground(StateColor::darkModeColorFor(wxColor(134, 134, 134)));
        wxSize version_ext = memDc.GetTextExtent(m_constant_text.version);
        wxRect version_rect(
			wxPoint(0, int(height * 0.70)),
			wxPoint(width, int(height * 0.70) + version_ext.GetHeight())
		);
        memDc.DrawLabel(m_constant_text.version, version_rect, wxALIGN_CENTER);

        // Dynamic Text
        m_action_line_y_position = int(height * 0.83);

		// Based on Text
        memDc.SetFont(m_constant_text.based_on_font);
        auto bs_version = wxString::Format(_L("Based on PrusaSlicer and BambuStudio")).ToStdString();
        wxSize based_on_ext = memDc.GetTextExtent(bs_version);
        wxRect based_on_rect(
			wxPoint(0, height - based_on_ext.GetHeight() * 2),
            wxPoint(width, height - based_on_ext.GetHeight())
		);
        memDc.DrawLabel(bs_version, based_on_rect, wxALIGN_CENTER);
    }

    static wxBitmap MakeBitmap()
    {
        int width = FromDIP(480, nullptr);
        int height = FromDIP(480, nullptr);

        wxImage image(width, height);
        wxBitmap new_bmp(image);

        wxMemoryDC memDC;
        memDC.SelectObject(new_bmp);
        memDC.SetBrush(StateColor::darkModeColorFor(*wxWHITE));
        memDC.DrawRectangle(-1, -1, width + 2, height + 2);
        memDC.DrawBitmap(new_bmp, 0, 0, true);
        return new_bmp;
    }

    void set_bitmap(wxBitmap& bmp)
    {
        m_window->SetBitmap(bmp);
        m_window->Refresh();
        m_window->Update();
    }

    void scale_bitmap(wxBitmap& bmp, float scale)
    {
        if (scale == 1.0)
            return;

        wxImage image = bmp.ConvertToImage();
        if (!image.IsOk() || image.GetWidth() == 0 || image.GetHeight() == 0)
            return;

        int width   = int(scale * image.GetWidth());
        int height  = int(scale * image.GetHeight());
        image.Rescale(width, height, wxIMAGE_QUALITY_BILINEAR);

        bmp = wxBitmap(std::move(image));
    }

    void scale_font(wxFont& font, float scale)
    {
#ifdef __WXMSW__
        // Workaround for the font scaling in respect to the current active display,
        // not for the primary display, as it's implemented in Font.cpp
        // See https://github.com/wxWidgets/wxWidgets/blob/master/src/msw/font.cpp
        // void wxNativeFontInfo::SetFractionalPointSize(float pointSizeNew)
        wxNativeFontInfo nfi= *font.GetNativeFontInfo();
        float pointSizeNew  = scale * font.GetPointSize();
        nfi.lf.lfHeight     = nfi.GetLogFontHeightAtPPI(pointSizeNew, get_dpi_for_window(this));
        nfi.pointSize       = pointSizeNew;
        font = wxFont(nfi);
#else
        font.Scale(scale);
#endif //__WXMSW__
    }


private:
    wxStaticText* m_staticText_slicer_name;
    wxStaticText* m_staticText_slicer_version;
    wxStaticBitmap* m_bitmap;
    wxStaticText* m_staticText_loading;

    wxBitmap    m_main_bitmap;
    wxFont      m_action_font;
    int         m_action_line_y_position;
    float       m_scale {1.0};

    struct ConstantText
    {
        wxString title;
        wxString version;
        wxString credits;

        wxFont   title_font;
        wxFont   version_font;
        wxFont   credits_font;
        wxFont   based_on_font;

        void init(wxFont init_font)
        {
            // title
            //title = wxGetApp().is_editor() ? SLIC3R_APP_FULL_NAME : GCODEVIEWER_APP_NAME;

            // dynamically get the version to display
            version = GUI_App::format_display_version();

            // credits infornation
            credits = "";

            //title_font    = Label::Head_16;
            version_font  = Label::Body_13;
            based_on_font = Label::Body_8;
            credits_font  = Label::Body_8;
        }
    }
    m_constant_text;
};

#ifdef __linux__
bool static check_old_linux_datadir(const wxString& app_name) {
    // If we are on Linux and the datadir does not exist yet, look into the old
    // location where the datadir was before version 2.3. If we find it there,
    // tell the user that he might wanna migrate to the new location.
    // (https://github.com/prusa3d/PrusaSlicer/issues/2911)
    // To be precise, the datadir should exist, it is created when single instance
    // lock happens. Instead of checking for existence, check the contents.

    namespace fs = boost::filesystem;

    std::string new_path = Slic3r::data_dir();

    wxString dir;
    if (! wxGetEnv(wxS("XDG_CONFIG_HOME"), &dir) || dir.empty() )
        dir = wxFileName::GetHomeDir() + wxS("/.config");
    std::string default_path = (dir + "/" + app_name).ToUTF8().data();

    if (new_path != default_path) {
        // This happens when the user specifies a custom --datadir.
        // Do not show anything in that case.
        return true;
    }

    fs::path data_dir = fs::path(new_path);
    if (! fs::is_directory(data_dir))
        return true; // This should not happen.

    int file_count = std::distance(fs::directory_iterator(data_dir), fs::directory_iterator());

    if (file_count <= 1) { // just cache dir with an instance lock
        // BBS
    } else {
        // If the new directory exists, be silent. The user likely already saw the message.
    }
    return true;
}
#endif

struct FileWildcards {
    std::string_view              title;
    std::vector<std::string_view> file_extensions;
};

static const FileWildcards file_wildcards_by_type[FT_SIZE] = {
    /* FT_STEP */    { "STEP files"sv,      { ".stp"sv, ".step"sv } },
    /* FT_STL */     { "STL files"sv,       { ".stl"sv } },
    /* FT_OBJ */     { "OBJ files"sv,       { ".obj"sv } },
    /* FT_AMF */     { "AMF files"sv,       { ".amf"sv, ".zip.amf"sv, ".xml"sv } },
    /* FT_3MF */     { "3MF files"sv,       { ".3mf"sv } },
    /* FT_GCODE_3MF */ {"Gcode 3MF files"sv, {".gcode.3mf"sv}},
    /* FT_GCODE */   { "G-code files"sv,    { ".gcode"sv} },
#ifdef __APPLE__
    /* FT_MODEL */
    {"Supported files"sv, {".3mf"sv, ".stl"sv, ".oltp"sv, ".stp"sv, ".step"sv, ".svg"sv, ".amf"sv, ".obj"sv, ".usd"sv, ".usda"sv, ".usdc"sv, ".usdz"sv, ".abc"sv, ".ply"sv}},
#else
    /* FT_MODEL */
    {"Supported files"sv, {".3mf"sv, ".stl"sv, ".oltp"sv, ".stp"sv, ".step"sv, ".svg"sv, ".amf"sv, ".obj"sv}},
#endif
    /* FT_ZIP */     { "ZIP files"sv,       { ".zip"sv } },
    /* FT_PROJECT */ { "Project files"sv,   { ".3mf"sv} },
    /* FT_GALLERY */ { "Known files"sv,     { ".stl"sv, ".obj"sv } },

    /* FT_INI */     { "INI files"sv,       { ".ini"sv } },
    /* FT_SVG */     { "SVG files"sv,       { ".svg"sv } },
    /* FT_TEX */     { "Texture"sv,         { ".png"sv, ".svg"sv } },
    /* FT_SL1 */     { "Masked SLA files"sv, { ".sl1"sv, ".sl1s"sv } },
};

// This function produces a Win32 file dialog file template mask to be consumed by wxWidgets on all platforms.
// The function accepts a custom extension parameter. If the parameter is provided, the custom extension
// will be added as a fist to the list. This is important for a "file save" dialog on OSX, which strips
// an extension from the provided initial file name and substitutes it with the default extension (the first one in the template).
wxString file_wildcards(FileType file_type, const std::string &custom_extension)
{
    const FileWildcards& data = file_wildcards_by_type[file_type];
    std::string title;
    std::string mask;
    std::string custom_ext_lower;

    if (! custom_extension.empty()) {
        // Generate an extension into the title mask and into the list of extensions.
        custom_ext_lower = boost::to_lower_copy(custom_extension);
        const std::string custom_ext_upper = boost::to_upper_copy(custom_extension);
        if (custom_ext_lower == custom_extension) {
            // Add a lower case version.
            title = std::string("*") + custom_ext_lower;
            mask = title;
            // Add an upper case version.
            mask  += ";*";
            mask  += custom_ext_upper;
        } else if (custom_ext_upper == custom_extension) {
            // Add an upper case version.
            title = std::string("*") + custom_ext_upper;
            mask = title;
            // Add a lower case version.
            mask += ";*";
            mask += custom_ext_lower;
        } else {
            // Add the mixed case version only.
            title = std::string("*") + custom_extension;
            mask = title;
        }
    }

    for (const std::string_view &ext : data.file_extensions)
        // Only add an extension if it was not added first as the custom extension.
        if (ext != custom_ext_lower) {
            if (title.empty()) {
                title = "*";
                title += ext;
                mask  = title;
            } else {
                title += ", *";
                title += ext;
                mask  += ";*";
                mask  += ext;
            }
            mask += ";*";
            mask += boost::to_upper_copy(std::string(ext));
        }
    return GUI::format_wxstr("%s (%s)|%s", data.title, title, mask);
}

static std::string libslic3r_translate_callback(const char *s) { return wxGetTranslation(wxString(s, wxConvUTF8)).utf8_str().data(); }

#ifdef WIN32
#if !wxVERSION_EQUAL_OR_GREATER_THAN(3,1,3)
static void register_win32_dpi_event()
{
    enum { WM_DPICHANGED_ = 0x02e0 };

    wxWindow::MSWRegisterMessageHandler(WM_DPICHANGED_, [](wxWindow *win, WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) {
        const int dpi = wParam & 0xffff;
        const auto rect = reinterpret_cast<PRECT>(lParam);
        const wxRect wxrect(wxPoint(rect->top, rect->left), wxPoint(rect->bottom, rect->right));

        DpiChangedEvent evt(EVT_DPI_CHANGED_SLICER, dpi, wxrect);
        win->GetEventHandler()->AddPendingEvent(evt);

        return true;
    });
}
#endif // !wxVERSION_EQUAL_OR_GREATER_THAN

static GUID GUID_DEVINTERFACE_HID = { 0x4D1E55B2, 0xF16F, 0x11CF, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 };

static void register_win32_device_notification_event()
{
    wxWindow::MSWRegisterMessageHandler(WM_DEVICECHANGE, [](wxWindow *win, WXUINT /* nMsg */, WXWPARAM wParam, WXLPARAM lParam) {
        // Some messages are sent to top level windows by default, some messages are sent to only registered windows, and we explictely register on MainFrame only.
        auto main_frame = dynamic_cast<MainFrame*>(win);
        auto plater = (main_frame == nullptr) ? nullptr : main_frame->plater();
        if (plater == nullptr)
            // Maybe some other top level window like a dialog or maybe a pop-up menu?
            return true;
		PDEV_BROADCAST_HDR lpdb = (PDEV_BROADCAST_HDR)lParam;
        switch (wParam) {
        case DBT_DEVICEARRIVAL:
			if (lpdb->dbch_devicetype == DBT_DEVTYP_VOLUME)
		        plater->GetEventHandler()->AddPendingEvent(VolumeAttachedEvent(EVT_VOLUME_ATTACHED));
			else if (lpdb->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
				PDEV_BROADCAST_DEVICEINTERFACE lpdbi = (PDEV_BROADCAST_DEVICEINTERFACE)lpdb;
//				if (lpdbi->dbcc_classguid == GUID_DEVINTERFACE_VOLUME) {
//					printf("DBT_DEVICEARRIVAL %d - Media has arrived: %ws\n", msg_count, lpdbi->dbcc_name);
				if (lpdbi->dbcc_classguid == GUID_DEVINTERFACE_HID)
			        plater->GetEventHandler()->AddPendingEvent(HIDDeviceAttachedEvent(EVT_HID_DEVICE_ATTACHED, boost::nowide::narrow(lpdbi->dbcc_name)));
			}
            break;
		case DBT_DEVICEREMOVECOMPLETE:
			if (lpdb->dbch_devicetype == DBT_DEVTYP_VOLUME)
                plater->GetEventHandler()->AddPendingEvent(VolumeDetachedEvent(EVT_VOLUME_DETACHED));
			else if (lpdb->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
				PDEV_BROADCAST_DEVICEINTERFACE lpdbi = (PDEV_BROADCAST_DEVICEINTERFACE)lpdb;
//				if (lpdbi->dbcc_classguid == GUID_DEVINTERFACE_VOLUME)
//					printf("DBT_DEVICEARRIVAL %d - Media was removed: %ws\n", msg_count, lpdbi->dbcc_name);
				if (lpdbi->dbcc_classguid == GUID_DEVINTERFACE_HID)
        			plater->GetEventHandler()->AddPendingEvent(HIDDeviceDetachedEvent(EVT_HID_DEVICE_DETACHED, boost::nowide::narrow(lpdbi->dbcc_name)));
			}
			break;
        default:
            break;
        }
        return true;
    });

    wxWindow::MSWRegisterMessageHandler(MainFrame::WM_USER_MEDIACHANGED, [](wxWindow *win, WXUINT /* nMsg */, WXWPARAM wParam, WXLPARAM lParam) {
        // Some messages are sent to top level windows by default, some messages are sent to only registered windows, and we explictely register on MainFrame only.
        auto main_frame = dynamic_cast<MainFrame*>(win);
        auto plater = (main_frame == nullptr) ? nullptr : main_frame->plater();
        if (plater == nullptr)
            // Maybe some other top level window like a dialog or maybe a pop-up menu?
            return true;
        wchar_t sPath[MAX_PATH];
        if (lParam == SHCNE_MEDIAINSERTED || lParam == SHCNE_MEDIAREMOVED) {
            struct _ITEMIDLIST* pidl = *reinterpret_cast<struct _ITEMIDLIST**>(wParam);
            if (! SHGetPathFromIDList(pidl, sPath)) {
                BOOST_LOG_TRIVIAL(error) << "MediaInserted: SHGetPathFromIDList failed";
                return false;
            }
        }
        switch (lParam) {
        case SHCNE_MEDIAINSERTED:
        {
            //printf("SHCNE_MEDIAINSERTED %S\n", sPath);
            plater->GetEventHandler()->AddPendingEvent(VolumeAttachedEvent(EVT_VOLUME_ATTACHED));
            break;
        }
        case SHCNE_MEDIAREMOVED:
        {
            //printf("SHCNE_MEDIAREMOVED %S\n", sPath);
            plater->GetEventHandler()->AddPendingEvent(VolumeDetachedEvent(EVT_VOLUME_DETACHED));
            break;
        }
	    default:
//          printf("Unknown\n");
            break;
	    }
        return true;
    });

    wxWindow::MSWRegisterMessageHandler(WM_INPUT, [](wxWindow *win, WXUINT /* nMsg */, WXWPARAM wParam, WXLPARAM lParam) {
        auto main_frame = dynamic_cast<MainFrame*>(Slic3r::GUI::find_toplevel_parent(win));
        auto plater = (main_frame == nullptr) ? nullptr : main_frame->plater();
//        if (wParam == RIM_INPUTSINK && plater != nullptr && main_frame->IsActive()) {
        if (wParam == RIM_INPUT && plater != nullptr && main_frame->IsActive()) {
        RAWINPUT raw;
			UINT rawSize = sizeof(RAWINPUT);
			::GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &raw, &rawSize, sizeof(RAWINPUTHEADER));
			if (raw.header.dwType == RIM_TYPEHID && plater->get_mouse3d_controller().handle_raw_input_win32(raw.data.hid.bRawData, raw.data.hid.dwSizeHid))
				return true;
		}
        return false;
    });

	wxWindow::MSWRegisterMessageHandler(WM_COPYDATA, [](wxWindow* win, WXUINT /* nMsg */, WXWPARAM wParam, WXLPARAM lParam) {
		COPYDATASTRUCT* copy_data_structure = { 0 };
		copy_data_structure = (COPYDATASTRUCT*)lParam;
		if (copy_data_structure->dwData == 1) {
			LPCWSTR arguments = (LPCWSTR)copy_data_structure->lpData;
			Slic3r::GUI::wxGetApp().other_instance_message_handler()->handle_message(boost::nowide::narrow(arguments));
		}
		return true;
		});
}
#endif // WIN32

static void generic_exception_handle()
{
    // Note: Some wxWidgets APIs use wxLogError() to report errors, eg. wxImage
    // - see https://docs.wxwidgets.org/3.1/classwx_image.html#aa249e657259fe6518d68a5208b9043d0
    //
    // wxLogError typically goes around exception handling and display an error dialog some time
    // after an error is logged even if exception handling and OnExceptionInMainLoop() take place.
    // This is why we use wxLogError() here as well instead of a custom dialog, because it accumulates
    // errors if multiple have been collected and displays just one error message for all of them.
    // Otherwise we would get multiple error messages for one missing png, for example.
    //
    // If a custom error message window (or some other solution) were to be used, it would be necessary
    // to turn off wxLogError() usage in wx APIs, most notably in wxImage
    // - see https://docs.wxwidgets.org/trunk/classwx_image.html#aa32e5d3507cc0f8c3330135bc0befc6a
/*#ifdef WIN32
    //LPEXCEPTION_POINTERS exception_pointers = nullptr;
    __try {
        throw;
    }
    __except (CBaseException::UnhandledExceptionFilter2(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) {
    //__except (exception_pointers = GetExceptionInformation(), EXCEPTION_EXECUTE_HANDLER) {
    //    if (exception_pointers) {
    //        CBaseException::UnhandledExceptionFilter(exception_pointers);
    //    }
    //    else
            throw;
    }
#else*/
    try {
        throw;
    } catch (const std::bad_alloc& ex) {
        // bad_alloc in main thread is most likely fatal. Report immediately to the user (wxLogError would be delayed)
        // and terminate the app so it is at least certain to happen now.
        BOOST_LOG_TRIVIAL(error) << boost::format("std::bad_alloc exception: %1%") % ex.what();
        flush_logs();
        wxString errmsg = wxString::Format(_L("OrcaSlicer will terminate because of running out of memory. "
                                              "It may be a bug. It will be appreciated if you report the issue to our team."));
        wxMessageBox(errmsg + "\n\n" + wxString(ex.what()), _L("Fatal error"), wxOK | wxICON_ERROR);

        std::terminate();
        //throw;
     } catch (const boost::io::bad_format_string& ex) {
     	BOOST_LOG_TRIVIAL(error) << boost::format("Uncaught exception: %1%") % ex.what();
        	flush_logs();
        wxString errmsg = _L("OrcaSlicer will terminate because of a localization error. "
                             "It will be appreciated if you report the specific scenario this issue happened.");
        wxMessageBox(errmsg + "\n\n" + wxString(ex.what()), _L("Critical error"), wxOK | wxICON_ERROR);
        std::terminate();
        //throw;
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << boost::format("Uncaught exception: %1%") % ex.what();
        flush_logs();
        wxLogError(format_wxstr(_L("OrcaSlicer got an unhandled exception: %1%"), ex.what()));
        throw;
    }
//#endif
}

void GUI_App::toggle_show_gcode_window()
{
    m_show_gcode_window = !m_show_gcode_window;
    app_config->set_bool("show_gcode_window", m_show_gcode_window);
}

std::vector<std::string> GUI_App::split_str(std::string src, std::string separator)
{
    std::string::size_type pos;
    std::vector<std::string> result;
    src += separator;
    int size = src.size();

    for (int i = 0; i < size; i++)
    {
        pos = src.find(separator, i);
        if (pos < size)
        {
            std::string s = src.substr(i, pos - i);
            result.push_back(s);
            i = pos + separator.size() - 1;
        }
    }
    return result;
}

void GUI_App::post_init()
{
    assert(initialized());
    if (! this->initialized())
        throw Slic3r::RuntimeError("Calling post_init() while not yet initialized");

    m_open_method = "double_click";
    bool switch_to_3d = false;

    if (!this->init_params->input_files.empty()) {

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", init with input files, size %1%, input_gcode %2%")
            %this->init_params->input_files.size() %this->init_params->input_gcode;

        switch_to_3d = true;

        const auto first_url = this->init_params->input_files.front();
        if (this->init_params->input_files.size() == 1 && is_supported_open_protocol(first_url)) {
            start_download(first_url);
            m_open_method = "url";
        } else {
            if (this->init_params->input_gcode) {
                mainframe->select_tab(size_t(MainFrame::tp3DEditor));
                plater_->select_view_3D("3D");
                this->plater()->load_gcode(from_u8(this->init_params->input_files.front()));
                m_open_method = "gcode";
            } else {
                mainframe->select_tab(size_t(MainFrame::tp3DEditor));
                plater_->select_view_3D("3D");
                wxArrayString input_files;
                for (auto& file : this->init_params->input_files) {
                    input_files.push_back(wxString::FromUTF8(file));
                }
                this->plater()->set_project_filename(_L("Untitled"));
                this->plater()->load_files(input_files);
                try {
                    if (!input_files.empty()) {
                        std::string           file_path = input_files.front().ToStdString();
                        std::filesystem::path path(file_path);
                        m_open_method = "file_" + path.extension().string();
                    }
                } catch (...) {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ", file path exception!";
                    m_open_method = "file";
                }
            }
        }
    }

//#if BBL_HAS_FIRST_PAGE
    bool slow_bootup = false;
    if (app_config->get("slow_bootup") == "true") {
        slow_bootup = true;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", slow bootup, won't render gl here.";
    }
    if (!switch_to_3d) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", begin load_gl_resources";
        mainframe->Freeze();
        plater_->canvas3D()->enable_render(false);
        mainframe->select_tab(size_t(MainFrame::tp3DEditor));
        plater_->select_view_3D("3D");
        //BBS init the opengl resource here
//#ifdef __linux__
        if (plater_->canvas3D()->get_wxglcanvas()->IsShownOnScreen()&&plater_->canvas3D()->make_current_for_postinit()) {
//#endif
            Size canvas_size = plater_->canvas3D()->get_canvas_size();
            wxGetApp().imgui()->set_display_size(static_cast<float>(canvas_size.get_width()), static_cast<float>(canvas_size.get_height()));
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", start to init opengl";
            wxGetApp().init_opengl();

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", finished init opengl";
            plater_->canvas3D()->init();

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", finished init canvas3D";
            wxGetApp().imgui()->new_frame();

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", finished init imgui frame";
            plater_->canvas3D()->enable_render(true);

            if (!slow_bootup) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", start to render a first frame for test";
                plater_->canvas3D()->render(false);
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", finished rendering a first frame for test";
            }
//#ifdef __linux__
        }
        else {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << "Found glcontext not ready, postpone the init";
        }
//#endif
        if (is_editor())
            mainframe->select_tab(size_t(0));
        if (app_config->get("default_page") == "1")
            mainframe->select_tab(size_t(1));
        mainframe->Thaw();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", end load_gl_resources";
    }

    plater_->trigger_restore_project(1);
    //#endif

    //BBS: remove GCodeViewer as seperate APP logic
    /*if (this->init_params->start_as_gcodeviewer) {
        if (! this->init_params->input_files.empty())
            this->plater()->load_gcode(wxString::FromUTF8(this->init_params->input_files[0].c_str()));
    }
    else
    {
        if (! this->init_params->preset_substitutions.empty())
            show_substitutions_info(this->init_params->preset_substitutions);

#if 0
        // Load the cummulative config over the currently active profiles.
        //FIXME if multiple configs are loaded, only the last one will have an effect.
        // We need to decide what to do about loading of separate presets (just print preset, just filament preset etc).
        // As of now only the full configs are supported here.
        if (!m_print_config.empty())
            this->gui->mainframe->load_config(m_print_config);
#endif
        if (! this->init_params->load_configs.empty())
            // Load the last config to give it a name at the UI. The name of the preset may be later
            // changed by loading an AMF or 3MF.
            //FIXME this is not strictly correct, as one may pass a print/filament/printer profile here instead of a full config.
            this->mainframe->load_config_file(this->init_params->load_configs.back());
        // If loading a 3MF file, the config is loaded from the last one.
        if (!this->init_params->input_files.empty()) {
            const std::vector<size_t> res = this->plater()->load_files(this->init_params->input_files);
            if (!res.empty() && this->init_params->input_files.size() == 1) {
                // Update application titlebar when opening a project file
                const std::string& filename = this->init_params->input_files.front();
                //BBS: remove amf logic as project
                if (boost::algorithm::iends_with(filename, ".3mf"))
                    this->plater()->set_project_filename(filename);
            }
        }
        if (! this->init_params->extra_config.empty())
            this->mainframe->load_config(this->init_params->extra_config);
    }*/

    // BBS: to be checked
#if 1
    // show "Did you know" notification
    if (app_config->get("show_hints") == "true" && !is_gcode_viewer()) {
        plater_->get_notification_manager()->push_hint_notification(false);
    }
#endif

    hms_query = new HMSQuery();

    m_show_gcode_window = app_config->get_bool("show_gcode_window");
    if (m_networking_need_update) {
        //updating networking
        int ret = updating_bambu_networking();
        if (!ret) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<<":networking plugin updated successfully";
            //restart_networking();
        }
        else {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<<":networking plugin updated failed";
        }
    }

    // Start preset sync after project opened, otherwise we could have preset change during project opening which could cause crash 
    if (app_config->get("sync_user_preset") == "true") {
        // BBS loading user preset
        // Always async, not such startup step
        // BOOST_LOG_TRIVIAL(info) << "Loading user presets...";
        // scrn->SetText(_L("Loading user presets..."));
        if (m_agent) {
            start_sync_user_preset();
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " sync_user_preset: true";
    } else {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " sync_user_preset: false";
    }

    // The extra CallAfter() is needed because of Mac, where this is the only way
    // to popup a modal dialog on start without screwing combo boxes.
    // This is ugly but I honestly found no better way to do it.
    // Neither wxShowEvent nor wxWindowCreateEvent work reliably.
    if (this->preset_updater) { // G-Code Viewer does not initialize preset_updater.
        CallAfter([this] {
            bool cw_showed = this->config_wizard_startup();

            std::string http_url = get_http_url(app_config->get_country_code());
            std::string language = GUI::into_u8(current_language_code());
            std::string network_ver = Slic3r::NetworkAgent::get_version();
            bool        sys_preset  = app_config->get("sync_system_preset") == "true";
            this->preset_updater->sync(http_url, language, network_ver, sys_preset ? preset_bundle : nullptr);

            this->check_new_version_sf();
            if (is_user_login() && !app_config->get_stealth_mode()) {
              // this->check_privacy_version(0);
              request_user_handle(0);
            }
        });
    }

    if(!m_networking_need_update && m_agent) {
        m_agent->set_on_ssdp_msg_fn(
            [this](std::string json_str) {
                if (is_closing()) {
                    return;
                }
                GUI::wxGetApp().CallAfter([this, json_str] {
                    if (m_device_manager) {
                        m_device_manager->on_machine_alive(json_str);
                    }
                    });
            }
        );
        m_agent->set_on_http_error_fn([this](unsigned int status, std::string body) {
            this->handle_http_error(status, body);
        });
        m_agent->start_discovery(true, false);
    }

    //update the plugin tips
    CallAfter([this] {
            mainframe->refresh_plugin_tips();
        });

    // remove old log files over LOG_FILES_MAX_NUM
    std::string log_addr = data_dir();
    if (!log_addr.empty()) {
        auto log_folder = boost::filesystem::path(log_addr) / "log";
        if (boost::filesystem::exists(log_folder)) {
           std::vector<std::pair<time_t, std::string>> files_vec;
           for (auto& it : boost::filesystem::directory_iterator(log_folder)) {
               auto temp_path = it.path();
               try {
                   if (it.status().type() == boost::filesystem::regular_file) {
                       std::time_t lw_t = boost::filesystem::last_write_time(temp_path) ;
                       files_vec.push_back({ lw_t, temp_path.filename().string() });
                   }
               } catch (const std::exception &) {
               }
           }
           std::sort(files_vec.begin(), files_vec.end(), [](
               std::pair<time_t, std::string> &a, std::pair<time_t, std::string> &b) {
               return a.first > b.first;
           });

           while (files_vec.size() > LOG_FILES_MAX_NUM) {
               auto full_path = log_folder / boost::filesystem::path(files_vec[files_vec.size() - 1].second);
               BOOST_LOG_TRIVIAL(info) << "delete log file over " << LOG_FILES_MAX_NUM << ", filename: "<< files_vec[files_vec.size() - 1].second;
               try {
                   boost::filesystem::remove(full_path);
               }
               catch (const std::exception& ex) {
                   BOOST_LOG_TRIVIAL(error) << "failed to delete log file: "<< files_vec[files_vec.size() - 1].second << ". Error: " << ex.what();
               }
               files_vec.pop_back();
           }
        }
    }
    BOOST_LOG_TRIVIAL(info) << "finished post_init";
//BBS: remove the single instance currently
#ifdef _WIN32
    // Sets window property to mainframe so other instances can indentify it.
    OtherInstanceMessageHandler::init_windows_properties(mainframe, m_instance_hash_int);
#endif //WIN32
}

wxDEFINE_EVENT(EVT_ENTER_FORCE_UPGRADE, wxCommandEvent);
wxDEFINE_EVENT(EVT_SHOW_NO_NEW_VERSION, wxCommandEvent);
wxDEFINE_EVENT(EVT_SHOW_DIALOG, wxCommandEvent);
wxDEFINE_EVENT(EVT_CONNECT_LAN_MODE_PRINT, wxCommandEvent);
IMPLEMENT_APP(GUI_App)

//BBS: remove GCodeViewer as seperate APP logic
//GUI_App::GUI_App(EAppMode mode)
GUI_App::GUI_App()
    : wxApp()
    //, m_app_mode(mode)
    , m_app_mode(EAppMode::Editor)
    , m_em_unit(10)
    , m_imgui(new ImGuiWrapper())
	, m_removable_drive_manager(std::make_unique<RemovableDriveManager>())
    , m_downloader(std::make_unique<Downloader>())
	, m_other_instance_message_handler(std::make_unique<OtherInstanceMessageHandler>())
{
	//app config initializes early becasuse it is used in instance checking in OrcaSlicer.cpp
    this->init_app_config();
    this->init_download_path();
#if wxUSE_WEBVIEW_EDGE
    this->init_webview_runtime();
#endif

    reset_to_active();
}

void GUI_App::shutdown()
{
    BOOST_LOG_TRIVIAL(info) << "GUI_App::shutdown enter";

	if (m_removable_drive_manager) {
		removable_drive_manager()->shutdown();
	}

    // destroy login dialog
    if (login_dlg != nullptr) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": destroy login dialog");
        delete login_dlg;
        login_dlg = nullptr;
    }

    if (m_is_recreating_gui) return;
    set_closing(true);
    BOOST_LOG_TRIVIAL(info) << "GUI_App::shutdown exit";
}


std::string GUI_App::get_http_url(std::string country_code, std::string path)
{
    std::string url;
    if (country_code == "US") {
        url = "https://api.bambulab.com/";
    }
    else if (country_code == "CN") {
        url = "https://api.bambulab.cn/";
    }
    else if (country_code == "ENV_CN_DEV") {
        url = "https://api-dev.bambu-lab.com/";
    }
    else if (country_code == "ENV_CN_QA") {
        url = "https://api-qa.bambu-lab.com/";
    }
    else if (country_code == "ENV_CN_PRE") {
        url = "https://api-pre.bambu-lab.com/";
    }
    else {
        url = "https://api.bambulab.com/";
    }

    url += path.empty() ? "v1/iot-service/api/slicer/resource" : path;
    return url;
}

std::string GUI_App::get_model_http_url(std::string country_code)
{
    std::string url;
    if (country_code == "US") {
        url = "https://makerworld.com/";
    }
    else if (country_code == "CN") {
        url = "https://makerworld.com/";
    }
    else if (country_code == "ENV_CN_DEV") {
        url = "https://makerhub-dev.bambu-lab.com/";
    }
    else if (country_code == "ENV_CN_QA") {
        url = "https://makerhub-qa.bambu-lab.com/";
    }
    else if (country_code == "ENV_CN_PRE") {
        url = "https://makerhub-pre.bambu-lab.com/";
    }
    else {
        url = "https://makerworld.com/";
    }

    return url;
}


std::string GUI_App::get_plugin_url(std::string name, std::string country_code)
{
    std::string url = get_http_url(country_code);

    std::string curr_version = NetworkAgent::use_legacy_network ? BAMBU_NETWORK_AGENT_VERSION_LEGACY : BAMBU_NETWORK_AGENT_VERSION;
    std::string using_version = curr_version.substr(0, 9) + "00";
    if (name == "cameratools")
        using_version = curr_version.substr(0, 6) + "00.00";
    url += (boost::format("?slicer/%1%/cloud=%2%") % name % using_version).str();
    //url += (boost::format("?slicer/plugins/cloud=%1%") % "01.01.00.00").str();
    return url;
}

static std::string decode(std::string const& extra, std::string const& path = {}) {
    char const* p = extra.data();
    char const* e = p + extra.length();
    while (p + 4 < e) {
        boost::uint16_t len = ((boost::uint16_t)p[2]) | ((boost::uint16_t)p[3] << 8);
        if (p[0] == '\x75' && p[1] == '\x70' && len >= 5 && p + 4 + len < e && p[4] == '\x01') {
            return std::string(p + 9, p + 4 + len);
        }
        else {
            p += 4 + len;
        }
    }
    return Slic3r::decode_path(path.c_str());
}

int GUI_App::download_plugin(std::string name, std::string package_name, InstallProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    int result = 0;
    json j;
    std::string err_msg;

    // get country_code
    AppConfig* app_config = wxGetApp().app_config;
    if (!app_config) {
        j["result"] = "failed";
        j["error_msg"] = "app_config is nullptr";
        return -1;
    }

    BOOST_LOG_TRIVIAL(info) << "[download_plugin]: enter";
    m_networking_cancel_update = false;
    // get temp path
    fs::path target_file_path = (fs::temp_directory_path() / package_name);
    fs::path tmp_path = target_file_path;
    tmp_path += format(".%1%%2%", get_current_pid(), ".tmp");

#if defined(__WINDOWS__)
    if (is_running_on_arm64() && !NetworkAgent::use_legacy_network) {
        //set to arm64 for plugins
        std::map<std::string, std::string> current_headers = Slic3r::Http::get_extra_headers();
        current_headers["X-BBL-OS-Type"] = "windows_arm";

        Slic3r::Http::set_extra_headers(current_headers);
        BOOST_LOG_TRIVIAL(info) << boost::format("download_plugin: set X-BBL-OS-Type to windows_arm");
    }
#endif

    // get_url
    std::string  url = get_plugin_url(name, app_config->get_country_code());
    std::string download_url;
    Slic3r::Http http_url = Slic3r::Http::get(url);
    BOOST_LOG_TRIVIAL(info) << "[download_plugin]: check the plugin from " << url;
    http_url.timeout_connect(TIMEOUT_CONNECT)
        .timeout_max(TIMEOUT_RESPONSE)
        .on_complete(
        [&download_url](std::string body, unsigned status) {
            try {
                json j = json::parse(body);
                std::string message = j["message"].get<std::string>();

                if (message == "success") {
                    json resource = j.at("resources");
                    if (resource.is_array()) {
                        for (auto iter = resource.begin(); iter != resource.end(); iter++) {
                            Semver version;
                            std::string url;
                            std::string type;
                            std::string vendor;
                            std::string description;
                            for (auto sub_iter = iter.value().begin(); sub_iter != iter.value().end(); sub_iter++) {
                                if (boost::iequals(sub_iter.key(), "type")) {
                                    type = sub_iter.value();
                                    BOOST_LOG_TRIVIAL(info) << "[download_plugin]: get version of settings's type, " << sub_iter.value();
                                }
                                else if (boost::iequals(sub_iter.key(), "version")) {
                                    version = *(Semver::parse(sub_iter.value()));
                                }
                                else if (boost::iequals(sub_iter.key(), "description")) {
                                    description = sub_iter.value();
                                }
                                else if (boost::iequals(sub_iter.key(), "url")) {
                                    url = sub_iter.value();
                                }
                            }
                            BOOST_LOG_TRIVIAL(info) << "[download_plugin 1]: get type " << type << ", version " << version.to_string() << ", url " << url;
                            download_url = url;
                        }
                    }
                }
                else {
                    BOOST_LOG_TRIVIAL(info) << "[download_plugin 1]: get version of plugin failed, body=" << body;
                }
            }
            catch (...) {
                BOOST_LOG_TRIVIAL(error) << "[download_plugin 1]: catch unknown exception";
                ;
            }
        }).on_error(
            [&result, &err_msg](std::string body, std::string error, unsigned int status) {
                BOOST_LOG_TRIVIAL(error) << "[download_plugin 1] on_error: " << error<<", body = " << body;
                err_msg += "[download_plugin 1] on_error: " + error + ", body = " + body;
                result = -1;
        }).perform_sync();

#if defined(__WINDOWS__)
    if (is_running_on_arm64() && !NetworkAgent::use_legacy_network) {
        //set back
        std::map<std::string, std::string> current_headers = Slic3r::Http::get_extra_headers();
        current_headers["X-BBL-OS-Type"] = "windows";

        Slic3r::Http::set_extra_headers(current_headers);
        BOOST_LOG_TRIVIAL(info) << boost::format("download_plugin: set X-BBL-OS-Type back to windows");
    }
#endif

    bool cancel = false;
    if (result < 0) {
        j["result"] = "failed";
        j["error_msg"] = err_msg;
        if (pro_fn) pro_fn(InstallStatusDownloadFailed, 0, cancel);
        return result;
    }


    if (download_url.empty()) {
        BOOST_LOG_TRIVIAL(info) << "[download_plugin 1]: no available plugin found for this app version: " << SLIC3R_VERSION;
        if (pro_fn) pro_fn(InstallStatusDownloadFailed, 0, cancel);
        j["result"] = "failed";
        j["error_msg"] = "[download_plugin 1]: no available plugin found for this app version: " + std::string(SLIC3R_VERSION);
        return -1;
    }
    else if (pro_fn) {
        pro_fn(InstallStatusNormal, 5, cancel);
    }

    if (m_networking_cancel_update || cancel) {
        BOOST_LOG_TRIVIAL(info) << boost::format("[download_plugin 1]: %1%, cancelled by user") % __LINE__;
        j["result"] = "failed";
        j["error_msg"] = (boost::format("[download_plugin 1]: %1%, cancelled by user") % __LINE__).str();
        return -1;
    }
    BOOST_LOG_TRIVIAL(info) << "[download_plugin] get_url = " << download_url;

    // download
    Slic3r::Http http = Slic3r::Http::get(download_url);
    int reported_percent = 0;
    http.on_progress(
        [this, &pro_fn, cancel_fn, &result, &reported_percent, &err_msg](Slic3r::Http::Progress progress, bool& cancel) {
            int percent = 0;
            if (progress.dltotal != 0)
                percent = progress.dlnow * 50 / progress.dltotal;
            bool was_cancel = false;
            if (pro_fn && ((percent - reported_percent) >= 10)) {
                pro_fn(InstallStatusNormal, percent, was_cancel);
                reported_percent = percent;
                BOOST_LOG_TRIVIAL(info) << "[download_plugin 2] progress: " << reported_percent;
            }
            cancel = m_networking_cancel_update || was_cancel;
            if (cancel_fn)
                if (cancel_fn())
                    cancel = true;

            if (cancel) {
                err_msg += "[download_plugin] cancel";
                result = -1;
            }
        })
        .on_complete([&pro_fn, tmp_path, target_file_path](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << "[download_plugin 2] completed";
            bool cancel = false;
            int percent = 0;
            fs::fstream file(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
            file.write(body.c_str(), body.size());
            file.close();
            fs::rename(tmp_path, target_file_path);
            if (pro_fn) pro_fn(InstallStatusDownloadCompleted, 80, cancel);
            })
        .on_error([&pro_fn, &result, &err_msg](std::string body, std::string error, unsigned int status) {
            bool cancel = false;
            if (pro_fn) pro_fn(InstallStatusDownloadFailed, 0, cancel);
            BOOST_LOG_TRIVIAL(error) << "[download_plugin 2] on_error: " << error<<", body = " << body;
            err_msg += "[download_plugin 2] on_error: " + error + ", body = " + body;
            result = -1;
        });
    http.perform_sync();
    j["result"] = result < 0 ? "failed" : "success";
    j["error_msg"] = err_msg;
    return result;
}

int GUI_App::install_plugin(std::string name, std::string package_name, InstallProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    bool cancel = false;
    std::string target_file_path = (fs::temp_directory_path() / package_name).string();

    BOOST_LOG_TRIVIAL(info) << "[install_plugin] enter";
    // get plugin folder
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / name;
    //auto plugin_folder = boost::filesystem::path(wxStandardPaths::Get().GetUserDataDir().ToUTF8().data()) / "plugins";
    auto backup_folder = plugin_folder/"backup";
    if (!boost::filesystem::exists(plugin_folder)) {
        BOOST_LOG_TRIVIAL(info) << "[install_plugin] will create directory "<<plugin_folder.string();
        boost::filesystem::create_directory(plugin_folder);
    }
    if (!boost::filesystem::exists(backup_folder)) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", will create directory %1%")%backup_folder.string();
        boost::filesystem::create_directory(backup_folder);
    }

    if (m_networking_cancel_update) {
        BOOST_LOG_TRIVIAL(info) << boost::format("[install_plugin]: %1%, cancelled by user")%__LINE__;
        return -1;
    }
    if (pro_fn) {
        pro_fn(InstallStatusNormal, 50, cancel);
    }
    // unzip
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, target_file_path)) {
        BOOST_LOG_TRIVIAL(error) << boost::format("[install_plugin]: %1%, open zip file failed")%__LINE__;
        if (pro_fn) pro_fn(InstallStatusDownloadFailed, 0, cancel);
        return InstallStatusUnzipFailed;
    }

    mz_uint num_entries = mz_zip_reader_get_num_files(&archive);
    mz_zip_archive_file_stat stat;
    BOOST_LOG_TRIVIAL(error) << boost::format("[install_plugin]: %1%, got %2% files")%__LINE__ %num_entries;
    for (mz_uint i = 0; i < num_entries; i++) {
        if (m_networking_cancel_update || cancel) {
            BOOST_LOG_TRIVIAL(info) << boost::format("[install_plugin]: %1%, cancelled by user")%__LINE__;
            return -1;
        }
        if (mz_zip_reader_file_stat(&archive, i, &stat)) {
            if (stat.m_uncomp_size > 0) {
                std::string dest_file;
                if (stat.m_is_utf8) {
                    dest_file = stat.m_filename;
                }
                else {
                    std::string extra(1024, 0);
                    size_t n = mz_zip_reader_get_extra(&archive, stat.m_file_index, extra.data(), extra.size());
                    dest_file = decode(extra.substr(0, n), stat.m_filename);
                }
                auto dest_path = plugin_folder / dest_file;
                boost::filesystem::create_directories(dest_path.parent_path());
                std::string dest_zip_file = encode_path(dest_path.string().c_str());
                try {
                    if (fs::exists(dest_path))
                        fs::remove(dest_path);
                    mz_bool res = 0;
#ifndef WIN32
                    if (S_ISLNK(stat.m_external_attr >> 16)) {
                        std::string link(stat.m_uncomp_size + 1, 0);
                        res = mz_zip_reader_extract_to_mem(&archive, stat.m_file_index, link.data(), stat.m_uncomp_size, 0);
                        try {
                            boost::filesystem::create_symlink(link, dest_path);
                        } catch (const std::exception &e) {
                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " create_symlink:" << e.what();
                        }
                    } else {
#endif
                        res = mz_zip_reader_extract_to_file(&archive, stat.m_file_index, dest_zip_file.c_str(), 0);
#ifndef WIN32
                    }
#endif
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", extract  %1% from plugin zip %2%\n") % dest_file % stat.m_filename;
                    if (res == 0) {
#ifdef WIN32
                        std::wstring new_dest_zip_file = boost::locale::conv::utf_to_utf<wchar_t>(dest_path.generic_string());
                        res                            = mz_zip_reader_extract_to_file_w(&archive, stat.m_file_index, new_dest_zip_file.c_str(), 0);
#endif
                        if (res == 0) {
                            mz_zip_error zip_error = mz_zip_get_last_error(&archive);
                            BOOST_LOG_TRIVIAL(error) << "[install_plugin]Archive read error:" << mz_zip_get_error_string(zip_error) << std::endl;
                            close_zip_reader(&archive);
                            if (pro_fn) { pro_fn(InstallStatusUnzipFailed, 0, cancel); }
                            return InstallStatusUnzipFailed;
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    // ensure the zip archive is closed and rethrow the exception
                    close_zip_reader(&archive);
                    BOOST_LOG_TRIVIAL(error) << "[install_plugin]Archive read exception:"<<e.what();
                    if (pro_fn) {
                        pro_fn(InstallStatusUnzipFailed, 0, cancel);
                    }
                    return InstallStatusUnzipFailed;
                }
            }
        }
        else {
            BOOST_LOG_TRIVIAL(error) << boost::format("[install_plugin]: %1%, mz_zip_reader_file_stat for file %2% failed")%__LINE__%i;
        }
    }

    close_zip_reader(&archive);
    {
        fs::path dir_path(plugin_folder);
        if (fs::exists(dir_path) && fs::is_directory(dir_path)) {
            int file_count = 0, file_index = 0;
            for (fs::directory_iterator it(dir_path); it != fs::directory_iterator(); ++it) {
                if (fs::is_regular_file(it->status())) { ++file_count; }
            }
            for (fs::directory_iterator it(dir_path); it != fs::directory_iterator(); ++it) {
                BOOST_LOG_TRIVIAL(info) << " current path:" << it->path().string();
                if (it->path().string() == backup_folder) {
                    continue;
                }
                auto dest_path = backup_folder.string() + "/" + it->path().filename().string();
                if (fs::is_regular_file(it->status())) {
                    BOOST_LOG_TRIVIAL(info) << " copy file:" << it->path().string() << "," << it->path().filename();
                    try {
                        if (pro_fn) { pro_fn(InstallStatusNormal, 50 + file_index / file_count, cancel); }
                        file_index++;
                        if (fs::exists(dest_path)) { fs::remove(dest_path); }
                        std::string    error_message;
                        CopyFileResult cfr = copy_file(it->path().string(), dest_path, error_message, false);
                        if (cfr != CopyFileResult::SUCCESS) { BOOST_LOG_TRIVIAL(error) << "Copying to backup failed(" << cfr << "): " << error_message; }
                    } catch (const std::exception &e) {
                        BOOST_LOG_TRIVIAL(error) << "Copying to backup failed: " << e.what();
                    }
                } else {
                    BOOST_LOG_TRIVIAL(info) << " copy framework:" << it->path().string() << "," << it->path().filename();
                    copy_framework(it->path().string(), dest_path);
                }
            }
        }
    }
    if (pro_fn)
        pro_fn(InstallStatusInstallCompleted, 100, cancel);
    if (name == "plugins")
        app_config->set_bool("installed_networking", true);
    BOOST_LOG_TRIVIAL(info) << "[install_plugin] success";
    return 0;
}

void GUI_App::restart_networking()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(" enter, mainframe %1%")%mainframe;
    on_init_network(true);
    StaticBambuLib::reset();
    if(m_agent) {
        init_networking_callbacks();
        m_agent->set_on_ssdp_msg_fn(
            [this](std::string json_str) {
                if (is_closing()) {
                    return;
                }
                GUI::wxGetApp().CallAfter([this, json_str] {
                    if (m_device_manager) {
                        m_device_manager->on_machine_alive(json_str);
                    }
                    });
            }
        );
        m_agent->set_on_http_error_fn([this](unsigned int status, std::string body) {
            this->handle_http_error(status, body);
        });
        m_agent->start_discovery(true, false);
        if (mainframe)
            mainframe->refresh_plugin_tips();
        if (plater_)
            plater_->get_notification_manager()->bbl_close_plugin_install_notification();

        if (m_agent->is_user_login()) {
            remove_user_presets();
            enable_user_preset_folder(true);
            preset_bundle->load_user_presets(m_agent->get_user_id(), ForwardCompatibilitySubstitutionRule::Enable);
            mainframe->update_side_preset_ui();
        }

        if (app_config->get("sync_user_preset") == "true") {
            start_sync_user_preset();
        }
        // if (mainframe && this->app_config->get("staff_pick_switch") == "true") {
        //     if (mainframe->m_webview) { mainframe->m_webview->SendDesignStaffpick(has_model_mall()); }
        // }
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(" exit, m_agent=%1%")%m_agent;
}

void GUI_App::remove_old_networking_plugins()
{
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / "plugins";
    //auto plugin_folder = boost::filesystem::path(wxStandardPaths::Get().GetUserDataDir().ToUTF8().data()) / "plugins";
    if (boost::filesystem::exists(plugin_folder)) {
        BOOST_LOG_TRIVIAL(info) << "[remove_old_networking_plugins] remove the directory "<<plugin_folder.string();
        try {
            fs::remove_all(plugin_folder);
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Failed  removing the plugins directory " << plugin_folder.string();
        }
    }
}

int GUI_App::updating_bambu_networking()
{
    DownloadProgressDialog dlg(_L("Downloading Bambu Network Plug-in"));
    dlg.ShowModal();
    return 0;
}

bool GUI_App::check_networking_version()
{
    std::string network_ver = Slic3r::NetworkAgent::get_version();
    if (!network_ver.empty()) {
        BOOST_LOG_TRIVIAL(info) << "get_network_agent_version=" << network_ver;
    }
    std::string studio_ver = NetworkAgent::use_legacy_network ? BAMBU_NETWORK_AGENT_VERSION_LEGACY : BAMBU_NETWORK_AGENT_VERSION;
    if (network_ver.length() >= 8) {
        if (network_ver.substr(0,8) == studio_ver.substr(0,8)) {
            m_networking_compatible = true;
            return true;
        }
    }

    m_networking_compatible = false;
    return false;
}

bool GUI_App::is_compatibility_version()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": m_networking_compatible=%1%")%m_networking_compatible;
    return m_networking_compatible;
}

void GUI_App::cancel_networking_install()
{
    m_networking_cancel_update = true;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": plugin install cancelled!");
}

void GUI_App::init_networking_callbacks()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": enter, m_agent=%1%")%m_agent;
    if (m_agent) {
        //set callbacks
        //m_agent->set_on_user_login_fn([this](int online_login, bool login) {
        //    GUI::wxGetApp().request_user_handle(online_login);
        //    });

        m_agent->set_server_callback([](std::string url, int status) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": server_callback, url=%1%, status=%2%") % url % status;
            //CallAfter([this]() {
            //    if (!m_server_error_dialog) {
            //        /*m_server_error_dialog->EndModal(wxCLOSE);
            //        m_server_error_dialog->Destroy();
            //        m_server_error_dialog = nullptr;*/
            //        m_server_error_dialog = new NetworkErrorDialog(mainframe);
            //    }
            //
            //    if(plater()->get_select_machine_dialog() && plater()->get_select_machine_dialog()->IsShown()){
            //        return;
            //    }
            //
            //    if (m_server_error_dialog->m_show_again) {
            //        return;
            //    }
            //
            //    if (m_server_error_dialog->IsShown()) {
            //        return;
            //    }
            //
            //    m_server_error_dialog->ShowModal();
            //});
        });


        m_agent->set_on_server_connected_fn([this](int return_code, int reason_code) {
            if (is_closing()) {
            return;
            }
            if (return_code == 5) {
                GUI::wxGetApp().CallAfter([this] {
                    this->request_user_logout();
                    MessageDialog msg_dlg(nullptr, _L("Login information expired. Please login again."), "", wxAPPLY | wxOK);
                    if (msg_dlg.ShowModal() == wxOK) {
                        return;
                    }
                });
                return;
            }
            GUI::wxGetApp().CallAfter([this] {
                if (is_closing())
                    return;
                BOOST_LOG_TRIVIAL(trace) << "static: server connected";
                m_agent->set_user_selected_machine(m_agent->get_user_selected_machine());
                    if (this->is_enable_multi_machine()) {
                        auto evt = new wxCommandEvent(EVT_UPDATE_MACHINE_LIST);
                        wxQueueEvent(this, evt);
                    }
                    m_agent->set_user_selected_machine(m_agent->get_user_selected_machine());
                    //subscribe device
                    if (m_agent->is_user_login()) {

                        /*disconnect lan*/
                        DeviceManager* dev = this->getDeviceManager();
                        if (!dev) return;

                        MachineObject *obj = dev->get_selected_machine();
                        if (!obj) return;

                        /* resubscribe the cache dev list */
                        if (this->is_enable_multi_machine()) {

                            if (!dev->subscribe_list_cache.empty()) {
                                dev->subscribe_device_list(dev->subscribe_list_cache);
                            }
                        }
                    }
                });
            });

        m_agent->set_on_printer_connected_fn([this](std::string dev_id) {
            if (is_closing()) {
                return;
            }
            GUI::wxGetApp().CallAfter([this, dev_id] {
                if (is_closing())
                    return;
                bool tunnel = boost::algorithm::starts_with(dev_id, "tunnel/");
                /* request_pushing */
                MachineObject* obj = m_device_manager->get_my_machine(tunnel ? dev_id.substr(7) : dev_id);
                if (obj) {
                    obj->is_tunnel_mqtt = tunnel;
                    obj->command_request_push_all(true);
                    obj->command_get_version();
                    obj->erase_user_access_code();
                    obj->command_get_access_code();
                    if (m_agent)
                        m_agent->install_device_cert(obj->get_dev_id(), obj->is_lan_mode_printer());
                }
                });
            });

        m_agent->set_get_country_code_fn([this]() {
            if (app_config)
                return app_config->get_country_code();
            return std::string();
            }
        );

        m_agent->set_on_subscribe_failure_fn([this](std::string dev_id) {
            CallAfter([this, dev_id] {
                on_start_subscribe_again(dev_id);
            });
        });

        m_agent->set_on_local_connect_fn(
            [this](int state, std::string dev_id, std::string msg) {
                if (is_closing()) {
                    return;
                }
                CallAfter([this, state, dev_id, msg] {
                    if (is_closing()) {
                        return;
                    }
                    /* request_pushing */
                    MachineObject* obj = m_device_manager->get_my_machine(dev_id);
                    wxCommandEvent event(EVT_CONNECT_LAN_MODE_PRINT);

                    if (obj) {

                        if (obj->is_lan_mode_printer()) {
                            if (state == ConnectStatus::ConnectStatusOk) {
                                obj->command_request_push_all(true);
                                obj->command_get_version();
                                event.SetInt(0);
                                event.SetString(obj->get_dev_id());
                            } else if (state == ConnectStatus::ConnectStatusFailed) {
                                // Orca: only update status if same device id
                                if (m_device_manager->selected_machine != dev_id) return;

                                m_device_manager->set_selected_machine("");
                                wxString text;
                                if (msg == "5") {
                                    obj->set_access_code("");
                                    obj->erase_user_access_code();
                                    text = wxString::Format(_L("Incorrect password"));
                                    wxGetApp().show_dialog(text);
                                } else {
                                text = wxString::Format(_L("Connect %s failed! [SN:%s, code=%s]"), from_u8(obj->get_dev_name()), obj->get_dev_id(), msg);
                                    wxGetApp().show_dialog(text);
                                }
                                event.SetInt(-1);
                            } else if (state == ConnectStatus::ConnectStatusLost) {
                                m_device_manager->set_selected_machine("");
                                event.SetInt(-1);
                                BOOST_LOG_TRIVIAL(info) << "set_on_local_connect_fn: state = lost";
                            } else {
                                event.SetInt(-1);
                                BOOST_LOG_TRIVIAL(info) << "set_on_local_connect_fn: state = " << state;
                            }

                            obj->set_lan_mode_connection_state(false);
                        }
                        else {
                            if (state == ConnectStatus::ConnectStatusOk) {
                                event.SetInt(1);
                                event.SetString(obj->get_dev_id());
                            }
                            else if(msg == "5") {
                                event.SetInt(5);
                                event.SetString(obj->get_dev_id());
                            }
                            else {
                                event.SetInt(-2);
                                event.SetString(obj->get_dev_id());
                            }
                        }
                    }
                    if (wxGetApp().plater()->get_select_machine_dialog()) {
                        wxPostEvent(wxGetApp().plater()->get_select_machine_dialog(), event);
                    }
                });
            }
        );

        auto message_arrive_fn = [this](std::string dev_id, std::string msg) {
            if (is_closing()) {
                return;
            }
            CallAfter([this, dev_id, msg] {
                if (is_closing())
                    return;

                if (process_network_msg(dev_id, msg)) {
                    return;
                }

                if (MachineObject* obj = this->m_device_manager->get_user_machine(dev_id)) {
                    auto sel = this->m_device_manager->get_selected_machine();
                    if (sel && sel->get_dev_id() == dev_id) {
                        obj->parse_json("cloud", msg);
                        GUI::wxGetApp().sidebar().load_ams_list(obj);
                    } else {
                        obj->parse_json("cloud", msg, true);
                    }
                }

                if (GUI::wxGetApp().plater())
                    GUI::wxGetApp().plater()->update_machine_sync_status();
            });
        };

        m_agent->set_on_message_fn(message_arrive_fn);

        auto user_message_arrive_fn = [this](std::string user_id, std::string msg) {
            if (is_closing()) {
                return;
            }
            CallAfter([this, user_id, msg] {
                if (is_closing())
                    return;

                //check user
                if (user_id == m_agent->get_user_id()) {
                    this->m_user_manager->parse_json(msg);
                }

            });
        };

        m_agent->set_on_user_message_fn(user_message_arrive_fn);


        auto lan_message_arrive_fn = [this](std::string dev_id, std::string msg) {
            if (is_closing()) {
                return;
            }
            CallAfter([this, dev_id, msg] {
                if (is_closing())
                    return;

                if (this->process_network_msg(dev_id, msg)) {
                    return;
                }

                if (MachineObject* obj = m_device_manager->get_my_machine(dev_id)) {
                    obj->parse_json("lan", msg);
                    if (this->m_device_manager->get_selected_machine() == obj) {
                        GUI::wxGetApp().sidebar().load_ams_list(obj);
                    }
                }

                if (GUI::wxGetApp().plater())
                    GUI::wxGetApp().plater()->update_machine_sync_status();
                });
        };
        m_agent->set_on_local_message_fn(lan_message_arrive_fn);
        m_agent->set_queue_on_main_fn([this](std::function<void()> callback) {
            CallAfter(callback);
        });
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": exit, m_agent=%1%")%m_agent;
}

GUI_App::~GUI_App()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": enter");
    if (app_config != nullptr) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": destroy app_config");
        delete app_config;
    }

    if (preset_bundle != nullptr) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": destroy preset_bundle");
        delete preset_bundle;
    }

    if (preset_updater != nullptr) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": destroy preset updater");
        delete preset_updater;
    }

    StaticBambuLib::release();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": exit");
}

bool GUI_App::is_blocking_printing(MachineObject *obj_)
{
    DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return true;
    std::string target_model;
    if (obj_ == nullptr) {
        obj_ = dev->get_selected_machine();
        if (obj_) {
            target_model = obj_->printer_type;
        }
    } else {
        target_model = obj_->printer_type;
    }

    if (!obj_)
    {
        return false;
    }

    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    std::string    source_model  = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);

    if (source_model != target_model) {
        std::vector<std::string>      compatible_machine = obj_->get_compatible_machine();
        vector<std::string>::iterator it                 = find(compatible_machine.begin(), compatible_machine.end(), source_model);
        if (it == compatible_machine.end()) {
            return true;
        }
    }
    return false;
}

// If formatted for github, plaintext with OpenGL extensions enclosed into <details>.
// Otherwise HTML formatted for the system info dialog.
std::string GUI_App::get_gl_info(bool for_github)
{
    return OpenGLManager::get_gl_info().to_string(for_github);
}

wxGLContext* GUI_App::init_glcontext(wxGLCanvas& canvas)
{
    return m_opengl_mgr.init_glcontext(canvas);
}

bool GUI_App::init_opengl()
{
#ifdef __linux__
    bool status = m_opengl_mgr.init_gl();
    m_opengl_initialized = true;
    return status;
#else
    return m_opengl_mgr.init_gl();
#endif
}

// gets path to PrusaSlicer.ini, returns semver from first line comment
static boost::optional<Semver> parse_semver_from_ini(std::string path)
{
    std::ifstream stream(path);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    std::string body = buffer.str();
    size_t start = body.find("OrcaSlicer ");
    if (start == std::string::npos) {
        start = body.find("OrcaSlicer ");
        if (start == std::string::npos)
            return boost::none;
    }
    body = body.substr(start + 12);
    size_t end = body.find_first_of(" \n");
    if (end < body.size())
        body.resize(end);
    return Semver::parse(body);
}

void GUI_App::init_download_path()
{
    std::string down_path = app_config->get("download_path");

    if (down_path.empty()) {
        std::string user_down_path = wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Downloads).ToUTF8().data();
        app_config->set("download_path", user_down_path);
    }
    else {
        fs::path dp(down_path);
        if (!fs::exists(dp)) {

            std::string user_down_path = wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Downloads).ToUTF8().data();
            app_config->set("download_path", user_down_path);
        }
    }
}

#if wxUSE_WEBVIEW_EDGE
void GUI_App::init_webview_runtime()
{
    // Check WebView Runtime
    if (!WebView::CheckWebViewRuntime()) {
        int nRet = wxMessageBox(_L("Orca Slicer requires the Microsoft WebView2 Runtime to operate certain features.\nClick Yes to install it now."),
                                _L("WebView2 Runtime"), wxYES_NO);
        if (nRet == wxYES) {
            WebView::DownloadAndInstallWebViewRuntime();
        }
    }
}
#endif

void GUI_App::init_app_config()
{
	// Profiles for the alpha are stored into the PrusaSlicer-alpha directory to not mix with the current release.
    SetAppName(SLIC3R_APP_KEY);
//	SetAppName(SLIC3R_APP_KEY "-alpha");
//  SetAppName(SLIC3R_APP_KEY "-beta");
//	SetAppDisplayName(SLIC3R_APP_NAME);

	// Set the Slic3r data directory at the Slic3r XS module.
	// Unix: ~/ .Slic3r
	// Windows : "C:\Users\username\AppData\Roaming\Slic3r" or "C:\Documents and Settings\username\Application Data\Slic3r"
	// Mac : "~/Library/Application Support/Slic3r"

    if (data_dir().empty()) {
        // Orca: check if data_dir folder exists in application folder use it if it exists
        // Note:wxStandardPaths::Get().GetExecutablePath() return following paths
        // Unix: /usr/local/bin/exename
        // Windows: "C:\Programs\AppFolder\exename.exe"
        // Mac: /Applications/exename.app/Contents/MacOS/exename
        // TODO: have no idea what to do with Linux bundles
        auto _app_folder = boost::filesystem::path(wxStandardPaths::Get().GetExecutablePath().ToUTF8().data()).parent_path();
#ifdef __APPLE__
        // On macOS, the executable is inside the .app bundle.
        _app_folder = _app_folder.parent_path().parent_path().parent_path();
#endif
        boost::filesystem::path app_data_dir_path = _app_folder / "data_dir";
        if (boost::filesystem::exists(app_data_dir_path)) {
            set_data_dir(app_data_dir_path.string());
        }
        else{
            boost::filesystem::path data_dir_path;
            #ifndef __linux__
                std::string data_dir = wxStandardPaths::Get().GetUserDataDir().ToUTF8().data();
                //BBS create folder if not exists
                data_dir_path = boost::filesystem::path(data_dir);
                set_data_dir(data_dir);
            #else
                // Since version 2.3, config dir on Linux is in ${XDG_CONFIG_HOME}.
                // https://github.com/prusa3d/PrusaSlicer/issues/2911
                wxString dir;
                if (! wxGetEnv(wxS("XDG_CONFIG_HOME"), &dir) || dir.empty() )
                    dir = wxFileName::GetHomeDir() + wxS("/.config");
                set_data_dir((dir + "/" + GetAppName()).ToUTF8().data());
                data_dir_path = boost::filesystem::path(data_dir());
            #endif
            if (!boost::filesystem::exists(data_dir_path)){
                boost::filesystem::create_directory(data_dir_path);
            }
        }

        // Change current dirtory of application
        [[maybe_unused]] auto unused_result = chdir(encode_path((Slic3r::data_dir() + "/log").c_str()).c_str());
    } else {
        m_datadir_redefined = true;
    }

    // start log here
    std::time_t       t        = std::time(0);
    std::tm *         now_time = std::localtime(&t);
    std::stringstream buf;
    buf << std::put_time(now_time, "debug_%a_%b_%d_%H_%M_%S_");
    buf << get_current_pid() << ".log";
    std::string log_filename = buf.str();
#if !BBL_RELEASE_TO_PUBLIC
    set_log_path_and_level(log_filename, 5);
#else
    set_log_path_and_level(log_filename, 3);
#endif

    BOOST_LOG_TRIVIAL(info) << boost::format("gui mode, Current OrcaSlicer Version %1% build %2%") % SoftFever_VERSION % GIT_COMMIT_HASH;

    //BBS: remove GCodeViewer as seperate APP logic
	if (!app_config)
        app_config = new AppConfig();
        //app_config = new AppConfig(is_editor() ? AppConfig::EAppMode::Editor : AppConfig::EAppMode::GCodeViewer);

    m_config_corrupted = false;
	// load settings
	m_app_conf_exists = app_config->exists();
	if (m_app_conf_exists) {
        std::string error = app_config->load();
        if (!error.empty()) {
            // Orca: if the config file is corrupted, we will show a error dialog and create a default config file.
            m_config_corrupted = true;

        }
        // Save orig_version here, so its empty if no app_config existed before this run.
        m_last_config_version = app_config->orig_version();//parse_semver_from_ini(app_config->config_path());
    }
    else {
#ifdef _WIN32
        // update associate files from registry information
        if (is_associate_files(L"3mf")) {
            app_config->set("associate_3mf", "true");
        }
        if (is_associate_files(L"stl")) {
            app_config->set("associate_stl", "true");
        }
        if (is_associate_files(L"step") && is_associate_files(L"stp")) {
            app_config->set("associate_step", "true");
        }
#endif // _WIN32
    }
    set_logging_level(Slic3r::level_string_to_boost(app_config->get("log_severity_level")));

}

// returns true if found newer version and user agreed to use it
bool GUI_App::check_older_app_config(Semver current_version, bool backup)
{
    //BBS: current no need these logic
    return false;
}

void GUI_App::copy_older_config()
{
    preset_bundle->copy_files(m_older_data_dir_path);
}

std::map<std::string, std::string> GUI_App::get_extra_header()
{
    std::map<std::string, std::string> extra_headers;
    extra_headers.insert(std::make_pair("X-BBL-Client-Type", "slicer"));
    extra_headers.insert(std::make_pair("X-BBL-Client-Name", SLIC3R_APP_NAME));
    extra_headers.insert(std::make_pair("X-BBL-Client-Version", VersionInfo::convert_full_version(SLIC3R_VERSION)));
#if defined(__WINDOWS__)
#ifdef _M_X64
    extra_headers.insert(std::make_pair("X-BBL-OS-Type", "windows"));
#else
    extra_headers.insert(std::make_pair("X-BBL-OS-Type", "windows_arm"));
#endif
#elif defined(__APPLE__)
    extra_headers.insert(std::make_pair("X-BBL-OS-Type", "macos"));
#elif defined(__LINUX__)
    extra_headers.insert(std::make_pair("X-BBL-OS-Type", "linux"));
#endif
    int major = 0, minor = 0, micro = 0;
    wxGetOsVersion(&major, &minor, &micro);
    std::string os_version = (boost::format("%1%.%2%.%3%") % major % minor % micro).str();
    extra_headers.insert(std::make_pair("X-BBL-OS-Version", os_version));
    if (app_config)
        extra_headers.insert(std::make_pair("X-BBL-Device-ID", app_config->get("slicer_uuid")));
    extra_headers.insert(std::make_pair("X-BBL-Language", convert_studio_language_to_api(into_u8(current_language_code_safe()))));
    return extra_headers;
}

//BBS
void GUI_App::init_http_extra_header()
{
    std::map<std::string, std::string> extra_headers = get_extra_header();

    if (m_agent)
        m_agent->set_extra_http_header(extra_headers);
}

void GUI_App::update_http_extra_header()
{
    std::map<std::string, std::string> extra_headers = get_extra_header();
    Slic3r::Http::set_extra_headers(extra_headers);
    if (m_agent)
        m_agent->set_extra_http_header(extra_headers);
}

void GUI_App::on_start_subscribe_again(std::string dev_id)
{
    auto start_subscribe_timer = new wxTimer(this, wxID_ANY);
    Bind(wxEVT_TIMER, [this, start_subscribe_timer, dev_id](auto& e) {
        start_subscribe_timer->Stop();
        Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
        if (!dev) return;
        MachineObject* obj = dev->get_selected_machine();
        if (!obj) return;

        if ( (dev_id == obj->get_dev_id()) && obj->is_connecting() && obj->subscribe_counter > 0) {
            obj->subscribe_counter--;
            if(wxGetApp().getAgent()) wxGetApp().getAgent()->set_user_selected_machine(dev_id);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": dev_id=" << obj->get_dev_id();
        }
    });
    start_subscribe_timer->Start(5000, wxTIMER_ONE_SHOT);
}

std::string GUI_App::get_local_models_path()
{
    std::string local_path = "";
    if (data_dir().empty()) {
        return local_path;
    }

    auto models_folder = (boost::filesystem::path(data_dir()) / "models");
    local_path = models_folder.string();

    if (!fs::exists(models_folder)) {
        if (!fs::create_directory(models_folder)) {
            local_path = "";
        }
        BOOST_LOG_TRIVIAL(info) << "create models folder:" << models_folder.string();
    }
    return local_path;
}

void GUI_App::init_single_instance_checker(const std::string &name, const std::string &path)
{
    BOOST_LOG_TRIVIAL(debug) << "init wx instance checker " << name << " "<< path;
    m_single_instance_checker = std::make_unique<wxSingleInstanceChecker>(boost::nowide::widen(name), boost::nowide::widen(path));
}

bool GUI_App::OnInit()
{
    try {
        return on_init_inner();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(fatal) << "OnInit Got Fatal error: " << e.what();
        generic_exception_handle();
        return false;
    }
}

int GUI_App::OnExit()
{
    stop_sync_user_preset();

    if (m_device_manager) {
        delete m_device_manager;
        m_device_manager = nullptr;
    }

    if (m_user_manager) {
        delete m_user_manager;
        m_user_manager = nullptr;
    }

    if (m_agent) {
        // BBS avoid a crash on mac platform
#ifdef __WINDOWS__
        m_agent->start_discovery(false, false);
#endif
        delete m_agent;
        m_agent = nullptr;
    }

    // Orca: clean up encrypted bbl network log file if plugin is used
    // No point to keep them as they are encrypted and can't be used for debugging
    try {
        auto              log_folder  = boost::filesystem::path(data_dir()) / "log";
        const std::string filePattern = R"(debug_network_.*\.log\.enc)";
        std::regex        pattern(filePattern);
        if (boost::filesystem::exists(log_folder)) {
            std::vector<boost::filesystem::path> network_logs;
            for (auto& it : boost::filesystem::directory_iterator(log_folder)) {
                auto temp_path = it.path();
                if (boost::filesystem::is_regular_file(temp_path) && std::regex_match(temp_path.filename().string(), pattern)) {
                    network_logs.push_back(temp_path.filename());
                }
            }
            for (auto f : network_logs) {
                boost::filesystem::remove(f);
            }
        }
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "Failed to clean up encrypt bbl network log file";
    }

    return wxApp::OnExit();
}

class wxBoostLog : public wxLog
{
    void DoLogText(const wxString &msg) override {

        BOOST_LOG_TRIVIAL(warning) << msg.ToUTF8().data();
    }
    ~wxBoostLog() override
    {
        // This is a hack. Prevent thread logs from going to wxGuiLog on app quit.
        auto t = wxLog::SetActiveTarget(this);
        wxLog::FlushActive();
        wxLog::SetActiveTarget(t);
    }
};

std::string get_system_info()
{
    std::stringstream out;

    std::string b_start  = "";
    std::string b_end    = "";
    std::string line_end = "\n";

    out << b_start << "Operating System:    " << b_end << wxPlatformInfo::Get().GetOperatingSystemFamilyName() << line_end;
    out << b_start << "System Architecture: " << b_end << wxPlatformInfo::Get().GetBitnessName() << line_end;
    out << b_start <<
#if defined _WIN32
        "Windows Version:     "
#else
        // Hopefully some kind of unix / linux.
        "System Version:      "
#endif
        << b_end << wxPlatformInfo::Get().GetOperatingSystemDescription() << line_end;
    out << b_start << "Total RAM size [MB]: " << b_end << Slic3r::format_memsize_MB(Slic3r::total_physical_memory());

    return out.str();
}

bool GUI_App::on_init_inner()
{
    wxLog::SetActiveTarget(new wxBoostLog());
#if BBL_RELEASE_TO_PUBLIC
    wxLog::SetLogLevel(wxLOG_Message);
#endif

    ::Label::initSysFont();

    // Set initialization of image handlers before any UI actions - See GH issue #7469
    wxInitAllImageHandlers();
#ifdef NDEBUG
    wxImage::SetDefaultLoadFlags(0); // ignore waring in release build
#endif

#if defined(_WIN32) && ! defined(_WIN64)
    // BBS: remove 32bit build prompt
    // Win32 32bit build.
#endif // _WIN64

    // Forcing back menu icons under gtk2 and gtk3. Solution is based on:
    // https://docs.gtk.org/gtk3/class.Settings.html
    // see also https://docs.wxwidgets.org/3.0/classwx_menu_item.html#a2b5d6bcb820b992b1e4709facbf6d4fb
    // TODO: Find workaround for GTK4
#if defined(__WXGTK20__) || defined(__WXGTK3__)
    g_object_set (gtk_settings_get_default (), "gtk-menu-images", TRUE, NULL);
#endif

#ifdef WIN32
    //BBS set crash log folder
    CBaseException::set_log_folder(data_dir());
#endif

    wxGetApp().Bind(wxEVT_QUERY_END_SESSION, [this](auto & e) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< "received wxEVT_QUERY_END_SESSION";
        if (mainframe) {
            wxCloseEvent e2(wxEVT_CLOSE_WINDOW);
            e2.SetCanVeto(true);
            mainframe->GetEventHandler()->ProcessEvent(e2);
            if (e2.GetVeto()) {
                e.Veto();
                return;
            }
        }
        for (auto d : dialogStack)
            d->EndModal(wxID_ABORT);
    });

    // Verify resources path
    const wxString resources_dir = from_u8(Slic3r::resources_dir());
    wxCHECK_MSG(wxDirExists(resources_dir), false,
        wxString::Format(_L("Resources path does not exist or is not a directory: %s"), resources_dir));

#ifdef __linux__
    if (! check_old_linux_datadir(GetAppName())) {
        std::cerr << "Quitting, user chose to move their data to new location." << std::endl;
        return false;
    }
#endif

    BOOST_LOG_TRIVIAL(info) << get_system_info();

// initialize label colors and fonts
    init_label_colours();
    init_fonts();
    wxGetApp().Update_dark_mode_flag();
    
#if defined(__WINDOWS__)
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    m_is_arm64 = false;
    if (hKernel32) {
        auto fnIsWow64Process2 = (LPFN_ISWOW64PROCESS2)GetProcAddress(hKernel32, "IsWow64Process2");
        if (fnIsWow64Process2) {
            USHORT processMachine = 0;
            USHORT nativeMachine = 0;
            if (fnIsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine)) {
                if (nativeMachine == IMAGE_FILE_MACHINE_ARM64) {//IMAGE_FILE_MACHINE_ARM64
                    m_is_arm64 = true;
                }
                BOOST_LOG_TRIVIAL(info) << boost::format("processMachine architecture %1%, nativeMachine %2% m_is_arm64 %3%")%(int)(processMachine) %(int) nativeMachine %m_is_arm64;
            }
            else {
                BOOST_LOG_TRIVIAL(info) << boost::format("IsWow64Process2 failed, set m_is_arm64 to %1%") %m_is_arm64;
            }
        }
        else {
            BOOST_LOG_TRIVIAL(info) << boost::format("can not find IsWow64Process2, set m_is_arm64 to %1%") %m_is_arm64;
        }
    }
    else {
        BOOST_LOG_TRIVIAL(info) << boost::format("can not find kernel32, set m_is_arm64 to %1%") %m_is_arm64;
    }
#endif
    // Enable this to get the default Win32 COMCTRL32 behavior of static boxes.
//    wxSystemOptions::SetOption("msw.staticbox.optimized-paint", 0);
    // Enable this to disable Windows Vista themes for all wxNotebooks. The themes seem to lead to terrible
    // performance when working on high resolution multi-display setups.
//    wxSystemOptions::SetOption("msw.notebook.themed-background", 0);

//     Slic3r::debugf "wxWidgets version %s, Wx version %s\n", wxVERSION_STRING, wxVERSION;
    if (is_editor()) {
        std::string msg = Slic3r::Http::tls_global_init();
        std::string ssl_cert_store = app_config->get("tls_accepted_cert_store_location");
        bool ssl_accept = app_config->get("tls_cert_store_accepted") == "yes" && ssl_cert_store == Slic3r::Http::tls_system_cert_store();

        if (!msg.empty() && !ssl_accept) {
            RichMessageDialog
                dlg(nullptr,
                    wxString::Format(_L("%s\nDo you want to continue?"), msg),
                    "OrcaSlicer", wxICON_QUESTION | wxYES_NO);
            dlg.ShowCheckBox(_L("Remember my choice"));
            if (dlg.ShowModal() != wxID_YES) return false;

            app_config->set("tls_cert_store_accepted",
                dlg.IsCheckBoxChecked() ? "yes" : "no");
            app_config->set("tls_accepted_cert_store_location",
                dlg.IsCheckBoxChecked() ? Slic3r::Http::tls_system_cert_store() : "");
        }
    }

    // !!! Initialization of UI settings as a language, application color mode, fonts... have to be done before first UI action.
    // Like here, before the show InfoDialog in check_older_app_config()

    // If load_language() fails, the application closes.
    load_language(wxString(), true);
#ifdef _MSW_DARK_MODE

#ifndef __WINDOWS__
    wxSystemAppearance app = wxSystemSettings::GetAppearance();
    GUI::wxGetApp().app_config->set("dark_color_mode", app.IsDark() ? "1" : "0");
    GUI::wxGetApp().app_config->save();
#endif // __APPLE__


    bool init_dark_color_mode = dark_mode();
    bool init_sys_menu_enabled = app_config->get("sys_menu_enabled") == "1";
#ifdef __WINDOWS__
     NppDarkMode::InitDarkMode(init_dark_color_mode, init_sys_menu_enabled);
#endif // __WINDOWS__

#endif


#ifdef _MSW_DARK_MODE
    // app_config can be updated in check_older_app_config(), so check if dark_color_mode and sys_menu_enabled was changed
    if (bool new_dark_color_mode = dark_mode();
        init_dark_color_mode != new_dark_color_mode) {

#ifdef __WINDOWS__
        NppDarkMode::SetDarkMode(new_dark_color_mode);
#endif // __WINDOWS__

        init_label_colours();
        //update_label_colours_from_appconfig();
    }
    if (bool new_sys_menu_enabled = app_config->get("sys_menu_enabled") == "1";
        init_sys_menu_enabled != new_sys_menu_enabled)
#ifdef __WINDOWS__
        NppDarkMode::SetSystemMenuForApp(new_sys_menu_enabled);
#endif
#endif

    if (m_last_config_version) {
        int last_major = m_last_config_version->maj();
        int last_minor = m_last_config_version->min();
        int last_patch = m_last_config_version->patch()/100;
        std::string studio_ver = SLIC3R_VERSION;
        int cur_major = atoi(studio_ver.substr(0,2).c_str());
        int cur_minor = atoi(studio_ver.substr(3,2).c_str());
        int cur_patch = atoi(studio_ver.substr(6,2).c_str());
        BOOST_LOG_TRIVIAL(info) << boost::format("last app version {%1%.%2%.%3%}, current version {%4%.%5%.%6%}")
            %last_major%last_minor%last_patch%cur_major%cur_minor%cur_patch;
        if ((last_major != cur_major)
            ||(last_minor != cur_minor)
            ||(last_patch != cur_patch)) {
            remove_old_networking_plugins();
        }
    }

    if(app_config->get("version") != SLIC3R_VERSION) {
        app_config->set("version", SLIC3R_VERSION);
    }

    SplashScreen * scrn = nullptr;
    if (app_config->get("show_splash_screen") == "true") {
        // make a bitmap with dark grey banner on the left side
        //BBS make BBL splash screen bitmap
        wxBitmap bmp = SplashScreen::MakeBitmap();
        // Detect position (display) to show the splash screen
        // Now this position is equal to the mainframe position
        wxPoint splashscreen_pos = wxDefaultPosition;
        if (app_config->has("window_mainframe")) {
            auto metrics = WindowMetrics::deserialize(app_config->get("window_mainframe"));
            if (metrics)
                splashscreen_pos = metrics->get_rect().GetPosition();
        }

        BOOST_LOG_TRIVIAL(info) << "begin to show the splash screen...";
        //BBS use BBL splashScreen
        scrn = new SplashScreen(bmp, wxSPLASH_CENTRE_ON_SCREEN | wxSPLASH_TIMEOUT, 1500, splashscreen_pos);
        wxYield();
        scrn->SetText(_L("Loading configuration")+ dots);
    }

    BOOST_LOG_TRIVIAL(info) << "loading systen presets...";
    preset_bundle = new PresetBundle();

    // just checking for existence of Slic3r::data_dir is not enough : it may be an empty directory
    // supplied as argument to --datadir; in that case we should still run the wizard
    preset_bundle->setup_directories();


    if (m_init_app_config_from_older)
        copy_older_config();

    if (is_editor()) {
#ifdef __WXMSW__
        if (app_config->get("associate_3mf") == "true")
            associate_files(L"3mf");
        if (app_config->get("associate_stl") == "true")
            associate_files(L"stl");
        if (app_config->get("associate_step") == "true") {
            associate_files(L"step");
            associate_files(L"stp");
        }
        associate_url(L"orcaslicer");

        if (app_config->get("associate_gcode") == "true")
            associate_files(L"gcode");
#endif // __WXMSW__

        preset_updater = new PresetUpdater();
        Bind(EVT_SLIC3R_VERSION_ONLINE, [this](const wxCommandEvent& evt) {
            if (this->plater_ != nullptr) {
                // this->plater_->get_notification_manager()->push_notification(NotificationType::NewAppAvailable);
                //BBS show msg box to download new version
               /* wxString tips = wxString::Format(_L("Click to download new version in default browser: %s"), version_info.version_str);
                DownloadDialog dialog(this->mainframe,
                    tips,
                    _L("New version of Orca Slicer"),
                    false,
                    wxCENTER | wxICON_INFORMATION);


                dialog.SetExtendedMessage(extmsg);*/
                std::string skip_version_str = this->app_config->get("app", "skip_version");
                bool skip_this_version = false;
                if (!skip_version_str.empty()) {
                    BOOST_LOG_TRIVIAL(info) << "new version = " << version_info.version_str << ", skip version = " << skip_version_str;
                    if (version_info.version_str <= skip_version_str) {
                        skip_this_version = true;
                    } else {
                        app_config->set("skip_version", "");
                        skip_this_version = false;
                    }
                }
                if (!skip_this_version
                    || evt.GetInt() != 0) {
                    UpdateVersionDialog dialog(this->mainframe);
                    wxString            extmsg = wxString::FromUTF8(version_info.description);
                    dialog.update_version_info(extmsg, version_info.version_str);
                    //dialog.update_version_info(version_info.description);
                    if (evt.GetInt() != 0) {
                        dialog.m_button_skip_version->Hide();
                    }
                    switch (dialog.ShowModal())
                    {
                    case wxID_YES:
                        wxLaunchDefaultBrowser(version_info.url);
                        break;
                    case wxID_NO:
                        break;
                    default:
                        ;
                    }
                }
            }
            });

        Bind(EVT_ENTER_FORCE_UPGRADE, [this](const wxCommandEvent& evt) {
                wxString      version_str = wxString::FromUTF8(this->app_config->get("upgrade", "version"));
                wxString      description_text = wxString::FromUTF8(this->app_config->get("upgrade", "description"));
                std::string   download_url = this->app_config->get("upgrade", "url");
                wxString tips = wxString::Format(_L("Click to download new version in default browser: %s"), version_str);
                DownloadDialog dialog(this->mainframe,
                    tips,
                    _L("The Orca Slicer needs an upgrade"),
                    false,
                    wxCENTER | wxICON_INFORMATION);
                dialog.SetExtendedMessage(description_text);

                int result = dialog.ShowModal();
                switch (result)
                {
                 case wxID_YES:
                     wxLaunchDefaultBrowser(download_url);
                     break;
                 case wxID_NO:
                     wxGetApp().mainframe->Close(true);
                     break;
                 default:
                     wxGetApp().mainframe->Close(true);
                }
            });

        Bind(EVT_SHOW_NO_NEW_VERSION, [this](const wxCommandEvent& evt) {
            wxString msg = _L("This is the newest version.");
            InfoDialog dlg(nullptr, _L("Info"), msg);
            dlg.ShowModal();
        });

        Bind(EVT_SHOW_DIALOG, [this](const wxCommandEvent& evt) {
            wxString msg = evt.GetString();
            InfoDialog dlg(this->mainframe, _L("Info"), msg);
            dlg.Bind(wxEVT_DESTROY, [this](auto& e) {
                m_info_dialog_content = wxEmptyString;
            });
            dlg.ShowModal();
        });
    }
    else {
#ifdef __WXMSW__
        if (app_config->get("associate_gcode") == "true")
            associate_files(L"gcode");
#endif // __WXMSW__
    }

    // Suppress the '- default -' presets.
    preset_bundle->set_default_suppressed(true);

    preset_bundle->backup_user_folder();

    Bind(EVT_UPDATE_MACHINE_LIST, &GUI_App::on_update_machine_list, this);
    Bind(EVT_USER_LOGIN, &GUI_App::on_user_login, this);
    Bind(EVT_USER_LOGIN_HANDLE, &GUI_App::on_user_login_handle, this);
    Bind(EVT_CHECK_PRIVACY_VER, &GUI_App::on_check_privacy_update, this);
    Bind(EVT_CHECK_PRIVACY_SHOW, &GUI_App::show_check_privacy_dlg, this);

    Bind(EVT_SHOW_IP_DIALOG, &GUI_App::show_ip_address_enter_dialog_handler, this);


    std::map<std::string, std::string> extra_headers = get_extra_header();
    Slic3r::Http::set_extra_headers(extra_headers);

    // Orca: select network plugin version
    NetworkAgent::use_legacy_network = app_config->get_bool("legacy_networking");
    // Force legacy network plugin if debugger attached
    // See https://github.com/bambulab/BambuStudio/issues/6726
    /* if (!NetworkAgent::use_legacy_network) {
        bool debugger_attached = false;
#if defined(__WINDOWS__)
        debugger_attached = IsDebuggerPresent();
#elif defined(__WXOSX__) || defined(__linux__)
        debugger_attached = is_debugger_present();
#endif
        if (debugger_attached) {
            NetworkAgent::use_legacy_network = true;
            wxMessageBox("Force using legacy bambu networking plugin because debugger is attached! If the app terminates itself immediately, please delete installed plugin and try again!");
        }
    } */
    copy_network_if_available();
    on_init_network();

    if (m_agent && m_agent->is_user_login()) {
        enable_user_preset_folder(true);
    } else {
        enable_user_preset_folder(false);
    }

    // BBS if load user preset failed
    //if (loaded_preset_result != 0) {
        try {
            // Enable all substitutions (in both user and system profiles), but log the substitutions in user profiles only.
            // If there are substitutions in system profiles, then a "reconfigure" event shall be triggered, which will force
            // installation of a compatible system preset, thus nullifying the system preset substitutions.
            init_params->preset_substitutions = preset_bundle->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
        }
        catch (const std::exception& ex) {
            show_error(nullptr, ex.what());
        }
    //}

#ifdef WIN32
#if !wxVERSION_EQUAL_OR_GREATER_THAN(3,1,3)
    register_win32_dpi_event();
#endif // !wxVERSION_EQUAL_OR_GREATER_THAN
    register_win32_device_notification_event();
#endif // WIN32

    // Let the libslic3r know the callback, which will translate messages on demand.
    Slic3r::I18N::set_translate_callback(libslic3r_translate_callback);

    BOOST_LOG_TRIVIAL(info) << "create the main window";
    mainframe = new MainFrame();
    // hide settings tabs after first Layout
    if (is_editor()) {
        mainframe->select_tab(size_t(0));
    }

    sidebar().obj_list()->init();
    //sidebar().aux_list()->init_auxiliary();
    mainframe->m_project->init_auxiliary();

//     update_mode(); // !!! do that later
    SetTopWindow(mainframe);

    plater_->init_notification_manager();

    m_printhost_job_queue.reset(new PrintHostJobQueue(mainframe->printhost_queue_dlg()));

    if (is_gcode_viewer()) {
        mainframe->update_layout();
        if (plater_ != nullptr)
            // ensure the selected technology is ptFFF
            plater_->set_printer_technology(ptFFF);
    }
    else
        load_current_presets();

    if (plater_ != nullptr) {
        plater_->reset_project_dirty_initial_presets();
        plater_->update_project_dirty_from_presets();
        plater_->get_partplate_list().set_filament_count(preset_bundle->filament_presets.size());
    }

    // BBS:
#ifdef __WINDOWS__
    mainframe->topbar()->SaveNormalRect();
#endif
    mainframe->Show(true);
    BOOST_LOG_TRIVIAL(info) << "main frame firstly shown";

//#if BBL_HAS_FIRST_PAGE
    //BBS: set tp3DEditor firstly
    /*plater_->canvas3D()->enable_render(false);
    mainframe->select_tab(size_t(MainFrame::tp3DEditor));
    scrn->SetText(_L("Loading Opengl resourses..."));
    plater_->select_view_3D("3D");
    //BBS init the opengl resource here
    Size canvas_size = plater_->canvas3D()->get_canvas_size();
    wxGetApp().imgui()->set_display_size(static_cast<float>(canvas_size.get_width()), static_cast<float>(canvas_size.get_height()));
    wxGetApp().init_opengl();
    plater_->canvas3D()->init();
    wxGetApp().imgui()->new_frame();
    plater_->canvas3D()->enable_render(true);
    plater_->canvas3D()->render();
    if (is_editor())
        mainframe->select_tab(size_t(0));*/
//#else
    //plater_->trigger_restore_project(1);
//#endif

    obj_list()->set_min_height();

    update_mode(); // update view mode after fix of the object_list size

#ifdef __APPLE__
   other_instance_message_handler()->bring_instance_forward();
#endif //__APPLE__

    Bind(EVT_HTTP_ERROR, &GUI_App::on_http_error, this);


    Bind(wxEVT_IDLE, [this](wxIdleEvent& event)
    {
        bool curr_studio_active = this->is_studio_active();
        if (m_studio_active != curr_studio_active) {
            if (curr_studio_active) {
                BOOST_LOG_TRIVIAL(info) << "studio is active, start to subscribe";
                if (m_agent) {
                    json j = json::object();
                    m_agent->start_subscribe("app");
                }
            } else {
                BOOST_LOG_TRIVIAL(info) << "studio is inactive, stop to subscribe";
                if (m_agent) {
                    json j = json::object();
                    m_agent->stop_subscribe("app");
                }
            }
            m_studio_active = curr_studio_active;
        }


        if (! plater_)
            return;

        // BBS
        //this->obj_manipul()->update_if_dirty();

        //use m_post_initialized instead
        //static bool update_gui_after_init = true;

        // An ugly solution to GH #5537 in which GUI_App::init_opengl (normally called from events wxEVT_PAINT
        // and wxEVT_SET_FOCUS before GUI_App::post_init is called) wasn't called before GUI_App::post_init and OpenGL wasn't initialized.
//#ifdef __linux__
//        if (!m_post_initialized && m_opengl_initialized) {
//#else
        if (!m_post_initialized && !m_adding_script_handler) {
//#endif
            m_post_initialized = true;
#ifdef WIN32
            this->mainframe->register_win32_callbacks();
#endif
            this->post_init();

            update_publish_status();
        }

        if (m_post_initialized && app_config->dirty())
            app_config->save();

    });

    m_initialized = true;

    flush_logs();

    BOOST_LOG_TRIVIAL(info) << "finished the gui app init";
    if (m_config_corrupted) {
        m_config_corrupted = false;
        show_error(nullptr,
                   _u8L(
                       "The OrcaSlicer configuration file may be corrupted and cannot be parsed.\nOrcaSlicer has attempted to recreate the "
                       "configuration file.\nPlease note, application settings will be lost, but printer profiles will not be affected."));
    }
    return true;
}

void GUI_App::copy_network_if_available()
{
    if (app_config->get("update_network_plugin") != "true")
        return;
    std::string network_library, player_library, live555_library, network_library_dst, player_library_dst, live555_library_dst;
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / "plugins";
    auto cache_folder = data_dir_path / "ota";
    std::string changelog_file = cache_folder.string() + "/network_plugins.json";
#if defined(_MSC_VER) || defined(_WIN32)
    network_library = cache_folder.string() + "/bambu_networking.dll";
    player_library      = cache_folder.string() + "/BambuSource.dll";
    live555_library     = cache_folder.string() + "/live555.dll";
    network_library_dst = plugin_folder.string() + "/bambu_networking.dll";
    player_library_dst  = plugin_folder.string() + "/BambuSource.dll";
    live555_library_dst = plugin_folder.string() + "/live555.dll";
#elif defined(__WXMAC__)
    network_library = cache_folder.string() + "/libbambu_networking.dylib";
    player_library = cache_folder.string() + "/libBambuSource.dylib";
    live555_library = cache_folder.string() + "/liblive555.dylib";
    network_library_dst = plugin_folder.string() + "/libbambu_networking.dylib";
    player_library_dst = plugin_folder.string() + "/libBambuSource.dylib";
    live555_library_dst = plugin_folder.string() + "/liblive555.dylib";
#else
    network_library = cache_folder.string() + "/libbambu_networking.so";
    player_library      = cache_folder.string() + "/libBambuSource.so";
    live555_library     = cache_folder.string() + "/liblive555.so";
    network_library_dst = plugin_folder.string() + "/libbambu_networking.so";
    player_library_dst  = plugin_folder.string() + "/libBambuSource.so";
    live555_library_dst = plugin_folder.string() + "/liblive555.so";
#endif

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ": checking network_library " << network_library << ", player_library " << player_library;
    if (!boost::filesystem::exists(plugin_folder)) {
        BOOST_LOG_TRIVIAL(info)<< __FUNCTION__ << ": create directory "<<plugin_folder.string();
        boost::filesystem::create_directory(plugin_folder);
    }
    std::string error_message;
    if (boost::filesystem::exists(network_library)) {
        CopyFileResult cfr = copy_file(network_library, network_library_dst, error_message, false);
        if (cfr != CopyFileResult::SUCCESS) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": Copying failed(" << cfr << "): " << error_message;
            return;
        }

        static constexpr const auto perms = fs::owner_read | fs::owner_write | fs::group_read | fs::others_read;
        fs::permissions(network_library_dst, perms);
        fs::remove(network_library);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ": Copying network library from" << network_library << " to " << network_library_dst<<" successfully.";
    }

    if (boost::filesystem::exists(player_library)) {
        CopyFileResult cfr = copy_file(player_library, player_library_dst, error_message, false);
        if (cfr != CopyFileResult::SUCCESS) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": Copying failed(" << cfr << "): " << error_message;
            return;
        }

        static constexpr const auto perms = fs::owner_read | fs::owner_write | fs::group_read | fs::others_read;
        fs::permissions(player_library_dst, perms);
        fs::remove(player_library);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ": Copying player library from" << player_library << " to " << player_library_dst<<" successfully.";
    }

    if (boost::filesystem::exists(live555_library)) {
        CopyFileResult cfr = copy_file(live555_library, live555_library_dst, error_message, false);
        if (cfr != CopyFileResult::SUCCESS) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": Copying failed(" << cfr << "): " << error_message;
            return;
        }

        static constexpr const auto perms = fs::owner_read | fs::owner_write | fs::group_read | fs::others_read;
        fs::permissions(live555_library_dst, perms);
        fs::remove(live555_library);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ": Copying live555 library from" << live555_library << " to " << live555_library_dst<<" successfully.";
    }
    if (boost::filesystem::exists(changelog_file))
        fs::remove(changelog_file);
    app_config->set("update_network_plugin", "false");
}

bool GUI_App::on_init_network(bool try_backup)
{
    bool create_network_agent = false;
    auto should_load_networking_plugin = app_config->get_bool("installed_networking");
    if(!should_load_networking_plugin) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "Don't load plugin as installed_networking is false";
    } else {
    int load_agent_dll = Slic3r::NetworkAgent::initialize_network_module();
__retry:
    if (!load_agent_dll) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": on_init_network, load dll ok";
        if (check_networking_version()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": on_init_network, compatibility version";
            auto bambu_source = Slic3r::NetworkAgent::get_bambu_source_entry();
            if (!bambu_source) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": can not get bambu source module!";
                m_networking_compatible = false;
                if (should_load_networking_plugin) {
                    m_networking_need_update = true;
                }
            }
            else
                create_network_agent = true;
        } else {
            if (try_backup) {
                int result = Slic3r::NetworkAgent::unload_network_module();
                BOOST_LOG_TRIVIAL(info) << "on_init_network, version mismatch, unload_network_module, result = " << result;
                load_agent_dll = Slic3r::NetworkAgent::initialize_network_module(true);
                try_backup = false;
                goto __retry;
            }
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": on_init_network, version dismatch, need upload network module";
            if (should_load_networking_plugin) {
                m_networking_need_update = true;
            }
        }
    } else {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": on_init_network, load dll failed";
        if (should_load_networking_plugin) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": on_init_network, need upload network module";
            m_networking_need_update = true;
        }
    }
    }

    if (create_network_agent) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", create network agent...");
        //std::string data_dir = wxStandardPaths::Get().GetUserDataDir().ToUTF8().data();
        std::string data_directory = data_dir();

        m_agent = new Slic3r::NetworkAgent(data_directory);

        if (!m_device_manager)
            m_device_manager = new Slic3r::DeviceManager(m_agent);
        else
            m_device_manager->set_agent(m_agent);

        if (!m_user_manager)
            m_user_manager = new Slic3r::UserManager(m_agent);
        else
            m_user_manager->set_agent(m_agent);

        if (this->is_enable_multi_machine()) {
            if (!m_task_manager) {
                m_task_manager = new Slic3r::TaskManager(m_agent);
                m_task_manager->start();
            }

            m_device_manager->EnableMultiMachine(true);
        } else {
            m_device_manager->EnableMultiMachine(false);
        }

        //BBS set config dir
        if (m_agent) {
            m_agent->set_config_dir(data_directory);
        }
        //BBS start http log
        if (m_agent) {
            m_agent->init_log();
        }

        //BBS set cert dir
        if (m_agent)
            m_agent->set_cert_file(resources_dir() + "/cert", "slicer_base64.cer");

        init_http_extra_header();

        if (m_agent) {
            init_networking_callbacks();
            std::string country_code = app_config->get_country_code();
            m_agent->set_country_code(country_code);
            m_agent->start();
        }
    }
    else {
        int result = Slic3r::NetworkAgent::unload_network_module();
        BOOST_LOG_TRIVIAL(info) << "on_init_network, unload_network_module, result = " << result;

        if (!m_device_manager)
            m_device_manager = new Slic3r::DeviceManager();

        if (!m_user_manager)
            m_user_manager = new Slic3r::UserManager();
    }

    return true;
}

unsigned GUI_App::get_colour_approx_luma(const wxColour &colour)
{
    double r = colour.Red();
    double g = colour.Green();
    double b = colour.Blue();

    return std::round(std::sqrt(
        r * r * .241 +
        g * g * .691 +
        b * b * .068
        ));
}

bool GUI_App::dark_mode()
{
#ifdef SUPPORT_DARK_MODE
#if __APPLE__
    // The check for dark mode returns false positive on 10.12 and 10.13,
    // which allowed setting dark menu bar and dock area, which is
    // is detected as dark mode. We must run on at least 10.14 where the
    // proper dark mode was first introduced.
    return wxPlatformInfo::Get().CheckOSVersion(10, 14) && mac_dark_mode();
#else
    return wxGetApp().app_config->get("dark_color_mode") == "1" ? true : check_dark_mode();
    //const unsigned luma = get_colour_approx_luma(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    //return luma < 128;
#endif
#else
    //BBS disable DarkUI mode
    return false;
#endif
}

const wxColour GUI_App::get_label_default_clr_system()
{
    return dark_mode() ? wxColour(115, 220, 103) : wxColour(26, 132, 57);
}

const wxColour GUI_App::get_label_default_clr_modified()
{
    return dark_mode() ? wxColour(253, 111, 40) : wxColour(252, 77, 1);
}

void GUI_App::init_label_colours()
{
    bool is_dark_mode = dark_mode();
    m_color_label_modified = is_dark_mode ? wxColour("#F1754E") : wxColour("#F1754E");
    m_color_label_sys      = is_dark_mode ? wxColour("#B2B3B5") : wxColour("#363636");

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    m_color_label_default           = is_dark_mode ? wxColour(250, 250, 250) : m_color_label_sys; // wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    m_color_highlight_label_default = is_dark_mode ? wxColour(230, 230, 230): wxSystemSettings::GetColour(/*wxSYS_COLOUR_HIGHLIGHTTEXT*/wxSYS_COLOUR_WINDOWTEXT);
    m_color_highlight_default       = is_dark_mode ? wxColour("#36363B") : wxColour("#F1F1F1"); // ORCA row highlighting
    m_color_hovered_btn_label       = is_dark_mode ? wxColour(255, 255, 254) : wxColour(0,0,0);
    m_color_default_btn_label       = is_dark_mode ? wxColour(255, 255, 254): wxColour(0,0,0);
    m_color_selected_btn_bg         = is_dark_mode ? wxColour(84, 84, 91)   : wxColour(206, 206, 206);
#else
    m_color_label_default = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
#endif
    m_color_window_default          = is_dark_mode ? wxColour(43, 43, 43)   : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    StateColor::SetDarkMode(is_dark_mode);
}

void GUI_App::update_label_colours_from_appconfig()
{
    if (app_config->has("label_clr_sys")) {
        auto str = app_config->get("label_clr_sys");
        if (str != "")
            m_color_label_sys = wxColour(str);
    }

    if (app_config->has("label_clr_modified")) {
        auto str = app_config->get("label_clr_modified");
        if (str != "")
            m_color_label_modified = wxColour(str);
    }
}

void GUI_App::update_publish_status()
{
    // mainframe->show_publish_button(has_model_mall());
    // if (app_config->get("staff_pick_switch") == "true") {
    //     mainframe->m_webview->SendDesignStaffpick(has_model_mall());
    // }
}

bool GUI_App::has_model_mall()
{
    if (auto cc = app_config->get_region(); cc == "CNH" || cc == "China" || cc == "")
        return false;
    return true;
}

void GUI_App::update_label_colours()
{
    for (Tab* tab : tabs_list)
        tab->update_label_colours();
}

#ifdef _WIN32
static bool is_focused(HWND hWnd)
{
    HWND hFocusedWnd = ::GetFocus();
    return hFocusedWnd && hWnd == hFocusedWnd;
}

static bool is_default(wxWindow* win)
{
    wxTopLevelWindow* tlw = find_toplevel_parent(win);
    if (!tlw)
        return false;

    return win == tlw->GetDefaultItem();
}
#endif

void GUI_App::UpdateDarkUI(wxWindow* window, bool highlited/* = false*/, bool just_font/* = false*/)
{
    if (wxButton *btn = dynamic_cast<wxButton*>(window)) {
        if (btn->GetWindowStyleFlag() & wxBU_AUTODRAW)
            return;
        else {
#ifdef _WIN32
            if (btn->GetId() == wxID_OK || btn->GetId() == wxID_CANCEL) {
                bool is_focused_button = false;
                bool is_default_button = false;

                if (!(btn->GetWindowStyle() & wxNO_BORDER)) {
                    btn->SetWindowStyle(btn->GetWindowStyle() | wxNO_BORDER);
                    highlited = true;
                }

                auto mark_button = [this, btn, highlited](const bool mark) {
                    btn->SetBackgroundColour(mark ? m_color_selected_btn_bg : highlited ? m_color_highlight_default : m_color_window_default);
                    btn->SetForegroundColour(mark ? m_color_hovered_btn_label :m_color_default_btn_label);
                    btn->Refresh();
                    btn->Update();
                };

                // hovering
                btn->Bind(wxEVT_ENTER_WINDOW, [mark_button](wxMouseEvent& event) { mark_button(true); event.Skip(); });
                btn->Bind(wxEVT_LEAVE_WINDOW, [mark_button, btn](wxMouseEvent& event) { mark_button(is_focused(btn->GetHWND())); event.Skip(); });
                // focusing
                btn->Bind(wxEVT_SET_FOCUS, [mark_button](wxFocusEvent& event) { mark_button(true); event.Skip(); });
                btn->Bind(wxEVT_KILL_FOCUS, [mark_button](wxFocusEvent& event) { mark_button(false); event.Skip(); });

                is_focused_button = is_focused(btn->GetHWND());
                is_default_button = is_default(btn);
                mark_button(is_focused_button);
            }
#endif
        }
    }

    if (Button* btn = dynamic_cast<Button*>(window)) {
        if (btn->GetWindowStyleFlag() & wxBU_AUTODRAW)
            return;
    }


    /*if (m_is_dark_mode != dark_mode() )
        m_is_dark_mode = dark_mode();*/

    if (m_is_dark_mode) {

        auto orig_col = window->GetBackgroundColour();
        auto bg_col = StateColor::darkModeColorFor(orig_col);
        // there are cases where the background color of an item is bright, specifically:
        // * the background color of a button: #009688  -- 73
        if (bg_col != orig_col) {
            window->SetBackgroundColour(bg_col);
        }

        orig_col = window->GetForegroundColour();
        auto fg_col = StateColor::darkModeColorFor(orig_col);
        auto fg_l = StateColor::GetLightness(fg_col);

        auto color_difference = StateColor::GetColorDifference(bg_col, fg_col);

        // fallback and sanity check with LAB
        // color difference of less than 2 or 3 is not normally visible, and even less than 30-40 doesn't stand out
        if (color_difference < 10) {
            fg_col = StateColor::SetLightness(fg_col, 90);
        }
        // some of the stock colors have a lightness of ~49
        if (fg_l < 45) {
            fg_col = StateColor::SetLightness(fg_col, 70);
        }
        // at this point it shouldn't be possible that fg_col is the same as bg_col, but let's be safe
        if (fg_col == bg_col) {
            fg_col = StateColor::SetLightness(fg_col, 70);
        }

        window->SetForegroundColour(fg_col);
    }
    else {
        auto original_col = window->GetBackgroundColour();
        auto bg_col = StateColor::lightModeColorFor(original_col);

        if (bg_col != original_col) {
            window->SetBackgroundColour(bg_col);
        }

        original_col = window->GetForegroundColour();
        auto fg_col = StateColor::lightModeColorFor(original_col);

        if (fg_col != original_col) {
            window->SetForegroundColour(fg_col);
        }
    }
}

// recursive function for scaling fonts for all controls in Window
static void update_dark_children_ui(wxWindow* window, bool just_buttons_update = false)
{
    /*bool is_btn = dynamic_cast<wxButton*>(window) != nullptr;
    is_btn = false;*/
    if (!window) return;

    if (ScalableButton* btn = dynamic_cast<ScalableButton*>(window)) {
        btn->UpdateDarkUI();
    } else {
        wxGetApp().UpdateDarkUI(window);
    }

    auto children = window->GetChildren();
    for (auto child : children) {
        update_dark_children_ui(child);
    }
}

// Note: Don't use this function for Dialog contains ScalableButtons
void GUI_App::UpdateDarkUIWin(wxWindow* win)
{
    update_dark_children_ui(win);
}

void GUI_App::Update_dark_mode_flag()
{
    m_is_dark_mode = dark_mode();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": switch the current dark mode status to %1% ")%m_is_dark_mode;
}

void GUI_App::UpdateDlgDarkUI(wxDialog* dlg)
{
#ifdef __WINDOWS__
    NppDarkMode::SetDarkExplorerTheme(dlg->GetHWND());
    NppDarkMode::SetDarkTitleBar(dlg->GetHWND());
#endif
    update_dark_children_ui(dlg);
}

void GUI_App::UpdateFrameDarkUI(wxFrame* dlg)
{
#ifdef __WINDOWS__
    NppDarkMode::SetDarkExplorerTheme(dlg->GetHWND());
    NppDarkMode::SetDarkTitleBar(dlg->GetHWND());
#endif
    update_dark_children_ui(dlg);
}

void GUI_App::UpdateDVCDarkUI(wxDataViewCtrl* dvc, bool highlited/* = false*/)
{
#ifdef __WINDOWS__
    UpdateDarkUI(dvc, highlited ? dark_mode() : false);
#ifdef _MSW_DARK_MODE
    //dvc->RefreshHeaderDarkMode(&m_normal_font);
    HWND hwnd;
    if (!dvc->HasFlag(wxDV_NO_HEADER)) {
        hwnd = (HWND) dvc->GenericGetHeader()->GetHandle();
        hwnd = GetWindow(hwnd, GW_CHILD);
        if (hwnd != NULL)
            NppDarkMode::SetDarkListViewHeader(hwnd);
        wxItemAttr attr;
        attr.SetTextColour(NppDarkMode::GetTextColor());
        attr.SetFont(m_normal_font);
        dvc->SetHeaderAttr(attr);
    }
#endif //_MSW_DARK_MODE
    if (dvc->HasFlag(wxDV_ROW_LINES))
        dvc->SetAlternateRowColour(m_color_highlight_default);
    if (dvc->GetBorder() != wxBORDER_SIMPLE)
        dvc->SetWindowStyle(dvc->GetWindowStyle() | wxBORDER_SIMPLE);
#endif
}

void GUI_App::UpdateAllStaticTextDarkUI(wxWindow* parent)
{
#ifdef __WINDOWS__
    wxGetApp().UpdateDarkUI(parent);

    auto children = parent->GetChildren();
    for (auto child : children) {
        if (dynamic_cast<wxStaticText*>(child))
            child->SetForegroundColour(m_color_label_default);
    }
#endif
}

void GUI_App::init_fonts()
{
    // BBS: modify font
    m_small_font = Label::Body_10;
    m_bold_font = Label::Body_10.Bold();
    m_normal_font = Label::Body_10;

#ifdef __WXMAC__
    m_small_font.SetPointSize(11);
    m_bold_font.SetPointSize(13);
#endif /*__WXMAC__*/

    // wxSYS_OEM_FIXED_FONT and wxSYS_ANSI_FIXED_FONT use the same as
    // DEFAULT in wxGtk. Use the TELETYPE family as a work-around
    m_code_font = wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE));
    m_code_font.SetPointSize(m_small_font.GetPointSize());
}

void GUI_App::update_fonts(const MainFrame *main_frame)
{
    /* Only normal and bold fonts are used for an application rescale,
     * because of under MSW small and normal fonts are the same.
     * To avoid same rescaling twice, just fill this values
     * from rescaled MainFrame
     */
	if (main_frame == nullptr)
		main_frame = this->mainframe;
    m_normal_font   = Label::Body_14; // BBS: larger font size
    m_small_font    = m_normal_font;
    m_bold_font     = m_normal_font.Bold();
    m_link_font     = m_bold_font.Underlined();
    m_em_unit       = main_frame->em_unit();
    m_code_font.SetPointSize(m_small_font.GetPointSize());
}

void GUI_App::set_label_clr_modified(const wxColour& clr)
{
    return;
    //BBS
    /*
    if (m_color_label_modified == clr)
        return;
    m_color_label_modified = clr;
    const std::string str = encode_color(ColorRGB(clr.Red(), clr.Green(), clr.Blue()));
    app_config->save();
    */
}

void GUI_App::set_label_clr_sys(const wxColour& clr)
{
    return;
    //BBS
    /*
    if (m_color_label_sys == clr)
        return;
    m_color_label_sys = clr;
    const std::string str = encode_color(ColorRGB(clr.Red(), clr.Green(), clr.Blue()));
    app_config->save();
    */
}

bool GUI_App::get_side_menu_popup_status()
{
    return m_side_popup_status;
}

void GUI_App::set_side_menu_popup_status(bool status)
{
    m_side_popup_status = status;
}

void GUI_App::link_to_network_check()
{
    std::string url;
    std::string country_code = app_config->get_country_code();


    if (country_code == "US") {
        url = "https://status.bambulab.com";
    }
    else if (country_code == "CN") {
        url = "https://status.bambulab.cn";
    }
    else {
        url = "https://status.bambulab.com";
    }
    wxLaunchDefaultBrowser(url);
}

void GUI_App::link_to_lan_only_wiki()
{
    std::string url;
    std::string country_code = app_config->get_country_code();

    if (country_code == "US") {
        url = "https://wiki.bambulab.com/en/knowledge-sharing/enable-lan-mode";
    }
    else if (country_code == "CN") {
        url = "https://wiki.bambulab.com/zh/knowledge-sharing/enable-lan-mode";
    }
    else {
        url = "https://wiki.bambulab.com/en/knowledge-sharing/enable-lan-mode";
    }
    wxLaunchDefaultBrowser(url);
}

bool GUI_App::tabs_as_menu() const
{
    return false;
}

wxSize GUI_App::get_min_size() const
{
    return wxSize(76*m_em_unit, 49 * m_em_unit);
}

float GUI_App::toolbar_icon_scale(const bool is_limited/* = false*/) const
{
#ifdef __APPLE__
    const float icon_sc = 1.0f; // for Retina display will be used its own scale
#else
    const float icon_sc = m_em_unit * 0.1f;
#endif // __APPLE__

    //return icon_sc;

    const std::string& auto_val = app_config->get("toolkit_size");

    if (auto_val.empty())
        return icon_sc;

    int int_val =  100;
    // correct value in respect to toolkit_size
    int_val = std::min(atoi(auto_val.c_str()), int_val);

    if (is_limited && int_val < 50)
        int_val = 50;

    return 0.01f * int_val * icon_sc;
}

void GUI_App::set_auto_toolbar_icon_scale(float scale) const
{
#ifdef __APPLE__
    const float icon_sc = 1.0f; // for Retina display will be used its own scale
#else
    const float icon_sc = m_em_unit * 0.1f;
#endif // __APPLE__

    long int_val = std::min(int(std::lround(scale / icon_sc * 100)), 100);
    std::string val = std::to_string(int_val);

    app_config->set("toolkit_size", val);
}

// check user printer_presets for the containing information about "Print Host upload"
void GUI_App::check_printer_presets()
{
//BBS
#if 0
    std::vector<std::string> preset_names = PhysicalPrinter::presets_with_print_host_information(preset_bundle->printers);
    if (preset_names.empty())
        return;

    // BBS: remove "print host upload" message dialog
    preset_bundle->physical_printers.load_printers_from_presets(preset_bundle->printers);
#endif
}

void switch_window_pools();
void release_window_pools();

void GUI_App::recreate_GUI(const wxString &msg_name)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "recreate_GUI enter";
    m_is_recreating_gui = true;

    update_http_extra_header();

    mainframe->shutdown();
    ProgressDialog dlg(msg_name, msg_name, 100, nullptr, wxPD_AUTO_HIDE);
    dlg.Pulse();
    dlg.Update(10, _L("Rebuild") + dots);

    MainFrame *old_main_frame = mainframe;
    struct ClientData : wxClientData
    {
        ~ClientData() { release_window_pools(); }
    };
    old_main_frame->SetClientObject(new ClientData);

    switch_window_pools();
    mainframe = new MainFrame();
    if (is_editor())
        // hide settings tabs after first Layout
        mainframe->select_tab(size_t(MainFrame::tp3DEditor));
    // Propagate model objects to object list.
    sidebar().obj_list()->init();
    //sidebar().aux_list()->init_auxiliary();
    //mainframe->m_auxiliary->init_auxiliary();
    SetTopWindow(mainframe);

    dlg.Update(30, _L("Rebuild") + dots);
    old_main_frame->Destroy();

    dlg.Update(80, _L("Loading current presets") + dots);
    m_printhost_job_queue.reset(new PrintHostJobQueue(mainframe->printhost_queue_dlg()));
    load_current_presets();
    mainframe->Show(true);
    //mainframe->refresh_plugin_tips();

    dlg.Update(90, _L("Loading a mode view") + dots);

    obj_list()->set_min_height();
    update_mode();

    // clear previous hms query, so that the hms info can use different language
    if (hms_query) hms_query->clear_hms_info();

    //BBS: trigger restore project logic here, and skip confirm
    plater_->trigger_restore_project(1);

    // #ys_FIXME_delete_after_testing  Do we still need this  ?
//     CallAfter([]() {
//         // Run the config wizard, don't offer the "reset user profile" checkbox.
//         config_wizard_startup(true);
//     });


    update_publish_status();

    m_is_recreating_gui = false;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "recreate_GUI exit";
}

void GUI_App::system_info()
{
    //SysInfoDialog dlg;
    //dlg.ShowModal();
}

void GUI_App::keyboard_shortcuts()
{
    KBShortcutsDialog dlg;
    dlg.ShowModal();
}


void GUI_App::ShowUserGuide() {
    // BBS:Show NewUser Guide
    try {
        bool res = false;
        GuideFrame GuideDlg(this);
                //if (GuideDlg.IsFirstUse())
        res = GuideDlg.run();
if (res) {
            load_current_presets();
            update_publish_status();
            mainframe->refresh_plugin_tips();
            // BBS: remove SLA related message
        }
    } catch (std::exception &) {
        // wxMessageBox(e.what(), "", MB_OK);
    }
}

void GUI_App::ShowDownNetPluginDlg() {
    try {
        auto iter = std::find_if(dialogStack.begin(), dialogStack.end(), [](auto dialog) {
            return dynamic_cast<DownloadProgressDialog *>(dialog) != nullptr;
        });
        if (iter != dialogStack.end())
            return;
        DownloadProgressDialog dlg(_L("Downloading Bambu Network Plug-in"));
        dlg.ShowModal();
    } catch (std::exception &) {
        ;
    }
}

void GUI_App::ShowUserLogin(bool show)
{
    // BBS: User Login Dialog
    if (show) {
        try {
            if (!login_dlg)
                login_dlg = new ZUserLogin();
            else {
                delete login_dlg;
                login_dlg = new ZUserLogin();
            }
            login_dlg->ShowModal();
        } catch (std::exception &) {
            ;
        }
    } else {
        if (login_dlg)
            login_dlg->EndModal(wxID_OK);
    }
}


void GUI_App::ShowOnlyFilament() {
    // BBS:Show NewUser Guide
    try {
        bool       res = false;
        GuideFrame GuideDlg(this);
        GuideDlg.SetStartPage(GuideFrame::GuidePage::BBL_FILAMENT_ONLY);
        res = GuideDlg.run();
        if (res) {
            load_current_presets();

            // BBS: remove SLA related message
        }
    } catch (std::exception &) {
        // wxMessageBox(e.what(), "", MB_OK);
    }
}



// static method accepting a wxWindow object as first parameter
bool GUI_App::catch_error(std::function<void()> cb,
    //                       wxMessageDialog* message_dialog,
    const std::string& err /*= ""*/)
{
    if (!err.empty()) {
        if (cb)
            cb();
        //         if (message_dialog)
        //             message_dialog->(err, "Error", wxOK | wxICON_ERROR);
        show_error(/*this*/nullptr, err);
        return true;
    }
    return false;
}

// static method accepting a wxWindow object as first parameter
void fatal_error(wxWindow* parent)
{
    show_error(parent, "");
    //     exit 1; // #ys_FIXME
}

#ifdef __WINDOWS__
#ifdef _MSW_DARK_MODE
static void update_scrolls(wxWindow* window)
{
    wxWindowList::compatibility_iterator node = window->GetChildren().GetFirst();
    while (node)
    {
        wxWindow* win = node->GetData();
        if (dynamic_cast<wxScrollHelper*>(win) ||
            dynamic_cast<wxTreeCtrl*>(win) ||
            dynamic_cast<wxTextCtrl*>(win))
            NppDarkMode::SetDarkExplorerTheme(win->GetHWND());

        update_scrolls(win);
        node = node->GetNext();
    }
}
#endif //_MSW_DARK_MODE


#ifdef _MSW_DARK_MODE
void GUI_App::force_menu_update()
{
    NppDarkMode::SetSystemMenuForApp(app_config->get("sys_menu_enabled") == "1");
}
#endif //_MSW_DARK_MODE
#endif //__WINDOWS__

void GUI_App::force_colors_update()
{
#ifdef _MSW_DARK_MODE
#ifdef __WINDOWS__
    NppDarkMode::SetDarkMode(dark_mode());
    if (WXHWND wxHWND = wxToolTip::GetToolTipCtrl())
        NppDarkMode::SetDarkExplorerTheme((HWND)wxHWND);
    NppDarkMode::SetDarkTitleBar(mainframe->GetHWND());


    //NppDarkMode::SetDarkExplorerTheme((HWND)mainframe->m_settings_dialog.GetHWND());
    //NppDarkMode::SetDarkTitleBar(mainframe->m_settings_dialog.GetHWND());

#endif // __WINDOWS__
#endif //_MSW_DARK_MODE
    m_force_colors_update = true;
}

// Called after the Preferences dialog is closed and the program settings are saved.
// Update the UI based on the current preferences.
void GUI_App::update_ui_from_settings()
{
    update_label_colours();
    // Upadte UI colors before Update UI from settings
    if (m_force_colors_update) {
        m_force_colors_update = false;
        //UpdateDlgDarkUI(&mainframe->m_settings_dialog);
        //mainframe->m_settings_dialog.Refresh();
        //mainframe->m_settings_dialog.Update();

        if (mainframe) {
#ifdef __WINDOWS__
            mainframe->force_color_changed();
            update_scrolls(mainframe);
            update_scrolls(&mainframe->m_settings_dialog);
#endif //_MSW_DARK_MODE
            update_dark_children_ui(mainframe);
        }
    }

    if (mainframe) {mainframe->update_ui_from_settings();}
}

void GUI_App::persist_window_geometry(wxTopLevelWindow *window, bool default_maximized)
{
    const std::string name = into_u8(window->GetName());

    window->Bind(wxEVT_CLOSE_WINDOW, [=](wxCloseEvent &event) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ": received wxEVT_CLOSE_WINDOW, trigger save for window_mainframe";
        window_pos_save(window, "mainframe");
        event.Skip();
    });

    if (window_pos_restore(window, "mainframe", default_maximized)) {
        on_window_geometry(window, [=]() {
            window_pos_sanitize(window);
        });
    } else {
        on_window_geometry(window, [=]() {
            window_pos_center(window);
        });
    }
}

void GUI_App::load_project(wxWindow *parent, wxString& input_file) const
{
    input_file.Clear();
    wxFileDialog dialog(parent ? parent : GetTopWindow(),
        _L("Choose one file (3MF):"),
        app_config->get_last_dir(), "",
        file_wildcards(FT_PROJECT), wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dialog.ShowModal() == wxID_OK)
        input_file = dialog.GetPath();
}

void GUI_App::import_model(wxWindow *parent, wxArrayString& input_files) const
{
    input_files.Clear();
    wxFileDialog dialog(parent ? parent : GetTopWindow(),
#ifdef __APPLE__
        _L("Choose one or more files (3MF/STEP/STL/SVG/OBJ/AMF/USD*/ABC/PLY):"),
#else
        _L("Choose one or more files (3MF/STEP/STL/SVG/OBJ/AMF):"),
#endif
        from_u8(app_config->get_last_dir()), "",
        file_wildcards(FT_MODEL), wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);

    if (dialog.ShowModal() == wxID_OK)
        dialog.GetPaths(input_files);
}

void GUI_App::import_zip(wxWindow* parent, wxString& input_file) const
{
    wxFileDialog dialog(parent ? parent : GetTopWindow(),
                        _L("Choose ZIP file") + ":",
                        from_u8(app_config->get_last_dir()), "",
                        file_wildcards(FT_ZIP), wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dialog.ShowModal() == wxID_OK)
        input_file = dialog.GetPath();
}

void GUI_App::load_gcode(wxWindow* parent, wxString& input_file) const
{
    input_file.Clear();
    wxFileDialog dialog(parent ? parent : GetTopWindow(),
        _L("Choose one file (GCODE/3MF):"),
        app_config->get_last_dir(), "",
        file_wildcards(FT_GCODE), wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dialog.ShowModal() == wxID_OK)
        input_file = dialog.GetPath();
}

wxString GUI_App::transition_tridid(int trid_id) const
{
    if (trid_id == VIRTUAL_TRAY_MAIN_ID || trid_id == VIRTUAL_TRAY_DEPUTY_ID)
    {
        assert(0);
        return wxString("Ext");
    }

    wxString maping_dict[] = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" };

    if (trid_id >= 128 * 4) {
        trid_id -= 128 * 4;
        int id_index = trid_id / 4;
        return wxString::Format("%s", maping_dict[id_index]);
    }
    else if (trid_id >= 0x80 && trid_id <= 0x87) { // n3s
        const char base = 'A' + (trid_id - 128);
        wxString prefix("HT-");
        prefix.append(base);
        return prefix;
    }
    else {
        int id_index = std::clamp((int)ceil(trid_id / 4), 0, 25);
        int id_suffix = trid_id % 4 + 1;
        return wxString::Format("%s%d", maping_dict[id_index], id_suffix);
    }
}

//BBS
void GUI_App::request_login(bool show_user_info)
{
    ShowUserLogin();

    if (show_user_info) {
        get_login_info();
    }
}

void GUI_App::get_login_info()
{
    if (m_agent) {
        if (m_agent->is_user_login()) {
            std::string login_cmd = m_agent->build_login_cmd();
            wxString strJS = wxString::Format("window.postMessage(%s)", login_cmd);
            GUI::wxGetApp().run_script(strJS);
        }
        else {
            m_agent->user_logout();
            std::string logout_cmd = m_agent->build_logout_cmd();
            wxString strJS = wxString::Format("window.postMessage(%s)", logout_cmd);
            GUI::wxGetApp().run_script(strJS);
        }
        mainframe->m_webview->SetLoginPanelVisibility(true);
    }
}

bool GUI_App::is_user_login()
{
    if (m_agent) {
        return m_agent->is_user_login();
    }
    return false;
}


bool GUI_App::check_login()
{
    bool result = false;
    if (m_agent) {
        result = m_agent->is_user_login();
    }

    if (!result) {
        ShowUserLogin();
    }
    return result;
}

void GUI_App::request_user_handle(int online_login)
{
    auto evt = new wxCommandEvent(EVT_USER_LOGIN_HANDLE);
    evt->SetInt(online_login);
    wxQueueEvent(this, evt);
}

void GUI_App::request_user_login(int online_login)
{
    auto evt = new wxCommandEvent(EVT_USER_LOGIN);
    evt->SetInt(online_login);
    wxQueueEvent(this, evt);
}

void GUI_App::request_user_logout()
{
    if (m_agent && m_agent->is_user_login()) {
        // Update data first before showing dialogs
        m_agent->user_logout(true);
        m_agent->set_user_selected_machine("");
        /* delete old user settings */
        bool     transfer_preset_changes = false;
        wxString header = _L("Some presets are modified.") + "\n" +
            _L("You can keep the modified presets to the new project, discard or save changes as new presets.");
        wxGetApp().check_and_keep_current_preset_changes(_L("User logged out"), header, ActionButtons::KEEP | ActionButtons::SAVE, &transfer_preset_changes);

        m_device_manager->clean_user_info();
        remove_user_presets();
        enable_user_preset_folder(false);
        preset_bundle->load_user_presets(DEFAULT_USER_FOLDER_NAME, ForwardCompatibilitySubstitutionRule::Enable);
        mainframe->update_side_preset_ui();

        GUI::wxGetApp().stop_sync_user_preset();
    }
}

int GUI_App::request_user_unbind(std::string dev_id)
{
    int result = -1;
    if (m_agent) {
        result = m_agent->unbind(dev_id);
        BOOST_LOG_TRIVIAL(info) << "request_user_unbind, dev_id = " << dev_id << ", result = " << result;
        return result;
    }
    return result;
}

std::string GUI_App::handle_web_request(std::string cmd)
{
    try {
        //BBS use nlohmann json format
        std::stringstream ss(cmd), oss;
        pt::ptree root, response;
        pt::read_json(ss, root);
        if (root.empty())
            return "";

        boost::optional<std::string> sequence_id = root.get_optional<std::string>("sequence_id");
        boost::optional<std::string> command = root.get_optional<std::string>("command");
        if (command.has_value()) {
            std::string command_str = command.value();
            if (command_str.compare("request_project_download") == 0) {
                if (root.get_child_optional("data") != boost::none) {
                    pt::ptree data_node = root.get_child("data");
                    boost::optional<std::string> project_id = data_node.get_optional<std::string>("project_id");
                    if (project_id.has_value()) {
                        this->request_project_download(project_id.value());
                    }
                }
            }
            else if (command_str.compare("open_project") == 0) {
                if (root.get_child_optional("data") != boost::none) {
                    pt::ptree data_node = root.get_child("data");
                    boost::optional<std::string> project_id = data_node.get_optional<std::string>("project_id");
                    if (project_id.has_value()) {
                        this->request_open_project(project_id.value());
                    }
                }
            }
            else if (command_str.compare("get_login_info") == 0) {
                CallAfter([this] {
                        get_login_info();
                    });
            }
            else if (command_str.compare("homepage_login_or_register") == 0) {
                CallAfter([this] {
                    this->request_login(true);
                });
            }
            else if (command_str.compare("homepage_logout") == 0) {
                CallAfter([this] {
                    wxGetApp().request_user_logout();
                });
            }
            else if (command_str.compare("homepage_modeldepot") == 0) {
                CallAfter([this] {
                    wxGetApp().open_mall_page_dialog();
                });
            }
            else if (command_str.compare("homepage_newproject") == 0) {
                this->request_open_project("<new>");
            }
            else if (command_str.compare("homepage_openproject") == 0) {
                this->request_open_project({});
            }
            else if (command_str.compare("get_recent_projects") == 0) {
                if (mainframe) {
                    if (mainframe->m_webview) {
                        mainframe->m_webview->SendRecentList(INT_MAX);
                    }
                }
            }
            // else if (command_str.compare("modelmall_model_advise_get") == 0) {
            //     if (mainframe && this->app_config->get("staff_pick_switch") == "true") {
            //         if (mainframe->m_webview) {
            //             mainframe->m_webview->SendDesignStaffpick(has_model_mall());
            //         }
            //     }
            // }
            // else if (command_str.compare("modelmall_model_open") == 0) {
            //     if (root.get_child_optional("data") != boost::none) {
            //         pt::ptree data_node = root.get_child("data");
            //         boost::optional<std::string> id = data_node.get_optional<std::string>("id");
            //         if (id.has_value() && mainframe->m_webview) {
            //             mainframe->m_webview->OpenModelDetail(id.value(), m_agent);
            //         }
            //     }
            // }
            else if (command_str.compare("homepage_open_recentfile") == 0) {
                if (root.get_child_optional("data") != boost::none) {
                    pt::ptree data_node = root.get_child("data");
                    boost::optional<std::string> path = data_node.get_optional<std::string>("path");
                    if (path.has_value()) {
                        this->request_open_project(path.value());
                    }
                }
            }
            else if (command_str.compare("homepage_delete_recentfile") == 0) {
                if (root.get_child_optional("data") != boost::none) {
                    pt::ptree                    data_node = root.get_child("data");
                    boost::optional<std::string> path      = data_node.get_optional<std::string>("path");
                    if (path.has_value()) {
                        this->request_remove_project(path.value());
                    }
                }
            }
            else if (command_str.compare("homepage_delete_all_recentfile") == 0) {
                this->request_remove_project("");
            }
            else if (command_str.compare("homepage_explore_recentfile") == 0) {
                if (root.get_child_optional("data") != boost::none) {
                    pt::ptree                    data_node = root.get_child("data");
                    boost::optional<std::string> path      = data_node.get_optional<std::string>("path");
                    if (path.has_value())
                    {
                        boost::filesystem::path NowFile(path.value());

                        std::string FilePath = NowFile.make_preferred().string();
                        desktop_open_any_folder(FilePath);
                    }
                }
            }
            else if (command_str.compare("homepage_open_hotspot") == 0) {
                if (root.get_child_optional("data") != boost::none) {
                    pt::ptree data_node = root.get_child("data");
                    boost::optional<std::string> url = data_node.get_optional<std::string>("url");
                    if (url.has_value()) {
                        this->request_open_project(url.value());
                    }
                }
            }
            else if (command_str.compare("begin_network_plugin_download") == 0) {
                CallAfter([this] { wxGetApp().ShowDownNetPluginDlg(); });
            }
            else if (command_str.compare("get_web_shortcut") == 0) {
                if (root.get_child_optional("key_event") != boost::none) {
                    pt::ptree key_event_node = root.get_child("key_event");
                    auto keyCode = key_event_node.get<int>("key");
                    auto ctrlKey = key_event_node.get<bool>("ctrl");
                    auto shiftKey = key_event_node.get<bool>("shift");
                    auto cmdKey = key_event_node.get<bool>("cmd");

                    wxKeyEvent e(wxEVT_CHAR_HOOK);
#ifdef __APPLE__
                    e.SetControlDown(cmdKey);
                    e.SetRawControlDown(ctrlKey);
#else
                    e.SetControlDown(ctrlKey);
#endif
                    e.SetShiftDown(shiftKey);
                    keyCode     = keyCode == 188 ? ',' : keyCode;
                    e.m_keyCode = keyCode;
                    e.SetEventObject(mainframe);
                    wxPostEvent(mainframe, e);
                }
            }
            else if (command_str.compare("userguide_wiki_open") == 0) {
                if (root.get_child_optional("data") != boost::none) {
                    pt::ptree                    data_node = root.get_child("data");
                    boost::optional<std::string> path      = data_node.get_optional<std::string>("url");
                    if (path.has_value()) {
                        wxLaunchDefaultBrowser(path.value());
                    }
                }
            }
            else if (command_str.compare("homepage_open_ccabin") == 0) {
                if (root.get_child_optional("data") != boost::none) {
                    pt::ptree                    data_node = root.get_child("data");
                    boost::optional<std::string> path      = data_node.get_optional<std::string>("file");
                    if (path.has_value()) {
                        std::string Fullpath = resources_dir() + "/web/homepage/model/" + path.value();

                        this->request_open_project(Fullpath);
                    }
                }
            }
            else if (command_str.compare("common_openurl") == 0) {
                boost::optional<std::string> path      = root.get_optional<std::string>("url");
                if (path.has_value()) {
                    wxLaunchDefaultBrowser(path.value());
                }
            } 
            else if (command_str.compare("homepage_makerlab_get") == 0) {
                //if (mainframe->m_webview) { mainframe->m_webview->SendMakerlabList(); }
            }
            else if (command_str.compare("makerworld_model_open") == 0) 
            {
                if (root.get_child_optional("model") != boost::none) {
                    pt::ptree                    data_node = root.get_child("model");
                    boost::optional<std::string> path      = data_node.get_optional<std::string>("url");
                    if (path.has_value()) 
                    { 
                        wxString realurl = from_u8(url_decode(path.value()));
                        wxGetApp().request_model_download(realurl);
                    }
                }
            }
        }
    }
    catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "parse json cmd failed " << cmd;
        return "";
    }
    return "";
}

void GUI_App::handle_script_message(std::string msg)
{
    try {
        json j = json::parse(msg);
        if (j.contains("command")) {
            wxString cmd = j["command"];
            if (cmd == "user_login") {
                if (m_agent) {
                    m_agent->change_user(j.dump());
                    if (m_agent->is_user_login()) {
                        request_user_login(1);
                    }
                }
            }
        }
    }
    catch (...) {
        ;
    }
}

void GUI_App::request_model_download(wxString url)
{
    if (plater_) {
        plater_->request_model_download(url);
    }
}

//BBS download project by project id
void GUI_App::download_project(std::string project_id)
{
    if (plater_) {
        plater_->request_download_project(project_id);
    }
}

void GUI_App::request_project_download(std::string project_id)
{
    if (!check_login()) return;

    download_project(project_id);
}

void GUI_App::request_open_project(std::string project_id)
{
    if (plater()->is_background_process_slicing()) {
        Slic3r::GUI::show_info(nullptr, _L("new or open project file is not allowed during the slicing process!"), _L("Open Project"));
        return;
    }

    if (project_id == "<new>")
        plater()->new_project();
    else if (project_id.empty())
        plater()->load_project();
    else if (std::find_if_not(project_id.begin(), project_id.end(),
        [](char c) { return std::isdigit(c); }) == project_id.end())
        ;
    else if (boost::algorithm::starts_with(project_id, "http"))
        ;
    else
        CallAfter([this, project_id] { mainframe->open_recent_project(-1, wxString::FromUTF8(project_id)); });
}

void GUI_App::request_remove_project(std::string project_id)
{
    mainframe->remove_recent_project(-1, wxString::FromUTF8(project_id));
}

void GUI_App::handle_http_error(unsigned int status, std::string body)
{
    // tips body size must less than 1024
    auto evt = new wxCommandEvent(EVT_HTTP_ERROR);
    evt->SetInt(status);
    evt->SetString(wxString(body));
    wxQueueEvent(this, evt);
}

void GUI_App::on_http_error(wxCommandEvent &evt)
{
    int status = evt.GetInt();

    int code = 0;
    std::string error;
    wxString result;
    if (status >= 400 && status < 500) {
        try {
        auto evt_str = evt.GetString();
        if (!evt_str.empty()) {
            json j = json::parse(evt_str.utf8_string());
            if (j.contains("code")) {
                if (!j["code"].is_null())
                    code = j["code"].get<int>();
            }
            if (j.contains("error"))
                if (!j["error"].is_null())
                    error = j["error"].get<std::string>();
        }
        } catch (...) {}
    }

    // Version limit
    if (code == HttpErrorVersionLimited) {
        MessageDialog msg_dlg(nullptr, _L("The version of Orca Slicer is too low and needs to be updated to the latest version before it can be used normally."), "", wxAPPLY | wxOK);
        if (msg_dlg.ShowModal() == wxOK) {
        }

    }

    // request login
    if (status == 401) {
        if (m_agent) {
            if (m_agent->is_user_login()) {
                this->request_user_logout();

                if (!m_show_http_errpr_msgdlg) {
                    MessageDialog msg_dlg(nullptr, _L("Login information expired. Please login again."), "", wxAPPLY | wxOK);
                    m_show_http_errpr_msgdlg = true;
                    auto modal_result = msg_dlg.ShowModal();
                    if (modal_result == wxOK || modal_result == wxCLOSE) {
                        m_show_http_errpr_msgdlg = false;
                        return;
                    }
                }
            }
        }
        return;
    }
}

void GUI_App::enable_user_preset_folder(bool enable)
{
    if (enable) {
        std::string user_id = m_agent->get_user_id();
        app_config->set("preset_folder", user_id);
        GUI::wxGetApp().preset_bundle->update_user_presets_directory(user_id);
    } else {
        BOOST_LOG_TRIVIAL(info) << "preset_folder: set to empty";
        app_config->set("preset_folder", "");
        GUI::wxGetApp().preset_bundle->update_user_presets_directory(DEFAULT_USER_FOLDER_NAME);
    }
}

void GUI_App::on_update_machine_list(wxCommandEvent &evt)
{
    /* DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
     if (dev) {
         dev->add_user_subscribe();
     }*/
}

void GUI_App::on_user_login_handle(wxCommandEvent &evt)
{
    if (!m_agent) { return; }

    int online_login = evt.GetInt();
    m_agent->connect_server();
    // get machine list
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;

    boost::thread update_thread = boost::thread([this, dev] {
        dev->update_user_machine_list_info();
    });

    if (online_login) {
        remove_user_presets();
        enable_user_preset_folder(true);
        preset_bundle->load_user_presets(m_agent->get_user_id(), ForwardCompatibilitySubstitutionRule::Enable);
        mainframe->update_side_preset_ui();

        GUI::wxGetApp().mainframe->show_sync_dialog();
    }
}


void GUI_App::check_track_enable()
{
    // Orca: alaways disable track event
    if (m_agent) {
        m_agent->track_enable(false);
        m_agent->track_remove_files();
    }
}

void GUI_App::on_user_login(wxCommandEvent &evt)
{
    if (!m_agent) { return; }
    int online_login = evt.GetInt();
    // check privacy before handle
    check_privacy_version(online_login);
    check_track_enable();
}

bool GUI_App::is_studio_active()
{
    auto curr_time = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time - last_active_point);
    if (diff.count() < STUDIO_INACTIVE_TIMEOUT) {
        return true;
    }
    return false;
}

void GUI_App::reset_to_active()
{
    last_active_point = std::chrono::system_clock::now();
}

void GUI_App::check_update(bool show_tips, int by_user)
{
    if (version_info.version_str.empty()) return;
    if (version_info.url.empty()) return;

    auto curr_version = Semver::parse(SLIC3R_VERSION);
    auto remote_version = Semver::parse(version_info.version_str);
    if (curr_version && remote_version && (*remote_version > *curr_version)) {
        if (version_info.force_upgrade) {
            wxGetApp().app_config->set_bool("force_upgrade", version_info.force_upgrade);
            wxGetApp().app_config->set("upgrade", "force_upgrade", true);
            wxGetApp().app_config->set("upgrade", "description", version_info.description);
            wxGetApp().app_config->set("upgrade", "version", version_info.version_str);
            wxGetApp().app_config->set("upgrade", "url", version_info.url);
            GUI::wxGetApp().enter_force_upgrade();
        }
        else {
            GUI::wxGetApp().request_new_version(by_user);
        }
    } else {
        wxGetApp().app_config->set("upgrade", "force_upgrade", false);
        if (show_tips)
            this->no_new_version();
    }
}

void GUI_App::check_new_version(bool show_tips, int by_user)
{
    return; // orca: not used, see check_new_version_sf
    std::string platform = "windows";

#ifdef __WINDOWS__
    platform = "windows";
#endif
#ifdef __APPLE__
    platform = "macos";
#endif
#ifdef __LINUX__
    platform = "linux";
#endif
    std::string query_params = (boost::format("?name=slicer&version=%1%&guide_version=%2%")
        % VersionInfo::convert_full_version(SLIC3R_VERSION)
        % VersionInfo::convert_full_version("0.0.0.1")
        ).str();

    std::string url = get_http_url(app_config->get_country_code()) + query_params;
    Slic3r::Http http = Slic3r::Http::get(url);

    http.header("accept", "application/json")
        .timeout_connect(TIMEOUT_CONNECT)
        .timeout_max(TIMEOUT_RESPONSE)
        .on_complete([this, show_tips, by_user](std::string body, unsigned) {
        try {
            json j = json::parse(body);
            if (j.contains("message")) {
                if (j["message"].get<std::string>() == "success") {
                    if (j.contains("software")) {
                        if (j["software"].empty() && show_tips) {
                            this->no_new_version();
                        }
                        else {
                            if (j["software"].contains("url")
                                && j["software"].contains("version")
                                && j["software"].contains("description")) {
                                version_info.url = j["software"]["url"].get<std::string>();
                                version_info.version_str = j["software"]["version"].get<std::string>();
                                version_info.description = j["software"]["description"].get<std::string>();
                            }
                            if (j["software"].contains("force_update")) {
                                version_info.force_upgrade = j["software"]["force_update"].get<bool>();
                            }
                            CallAfter([this, show_tips, by_user](){
                                this->check_update(show_tips, by_user);
                            });
                        }
                    }
                }
            }
        }
        catch (...) {
            ;
        }
            })
        .on_error([this](std::string body, std::string error, unsigned int status) {
            handle_http_error(status, body);
            BOOST_LOG_TRIVIAL(error) << "check new version error" << body;
    }).perform();
}

//parse the string, if it doesn't contain a valid version string, return invalid version.
Semver get_version(const std::string& str, const std::regex& regexp) {
    std::smatch match;
    if (std::regex_match(str, match, regexp)) {
        std::string version_cleaned = match[0];
        const boost::optional<Semver> version = Semver::parse(version_cleaned);
        if (version.has_value()) {
            return *version;
        }
    }
    return Semver::invalid();
}

namespace
{

struct UpdaterQuery
{
    std::string iid;
    std::string version;
    std::string os;
    std::string arch;
    std::string os_info;
};

std::string detect_updater_os()
{
#if defined(_WIN32)
    return "win";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__) || defined(__LINUX__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string detect_updater_arch()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "i386";
#else
    std::string arch = wxPlatformInfo::Get().GetArchName().ToStdString();
    boost::algorithm::to_lower(arch);
    if (arch.find("aarch64") != std::string::npos || arch.find("arm64") != std::string::npos)
        return "arm64";
    if (arch.find("x86_64") != std::string::npos || arch.find("amd64") != std::string::npos)
        return "x86_64";
    if (arch.find("i686") != std::string::npos || arch.find("i386") != std::string::npos || arch.find("x86") != std::string::npos)
        return "i386";
    return "unknown";
#endif
}

std::string detect_updater_os_info()
{
    wxString description = wxPlatformInfo::Get().GetOperatingSystemDescription();
#if defined(__LINUX__) || defined(__linux__)
    wxLinuxDistributionInfo distro = wxGetLinuxDistributionInfo();
    if (!distro.Id.empty()) {
        wxString normalized = distro.Id;
        if (!distro.Release.empty())
            normalized << " " << distro.Release;
        normalized.Trim(true);
        normalized.Trim(false);
        if (!normalized.empty())
            description = normalized;
    }
#endif
    if (description.empty())
        description = wxGetOsDescription();

    //Orca: workaround: wxGetOsVersion can't recognize Windows 11
    // For Windows, use actual version numbers to properly detect Windows 11
    // Windows 11 starts at build 22000
#if defined(_WIN32)
    int major = 0, minor = 0, micro = 0;
    wxGetOsVersion(&major, &minor, &micro);
    if (micro >= 22000) {
        // replace Windows 10 with Windows 11
        description.Replace("Windows 10", "Windows 11");
    }
#endif
    std::string os_info = description.ToStdString();
    boost::replace_all(os_info, "\r", " ");
    boost::replace_all(os_info, "\n", " ");
    boost::algorithm::trim(os_info);
    if (os_info.size() > 120)
        os_info.resize(120);
    boost::algorithm::to_lower(os_info);
    return os_info;
}

std::string detect_updater_version()
{
    return SoftFever_VERSION;
}

std::string detect_updater_iid(AppConfig* config)
{
    if (config == nullptr)
        return {};
    return instance_id::ensure(*config);
}

std::string encode_uri_component(const std::string& value)
{
    static constexpr const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~' ||
            ch == '!' || ch == '*' || ch == '(' || ch == ')' || ch == '\'') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[(ch >> 4) & 0xF]);
            out.push_back(hex[ch & 0xF]);
        }
    }
    return out;
}

std::string build_updater_query(const UpdaterQuery& query)
{
    std::vector<std::pair<std::string, std::string>> params;

    auto add_param = [&params](const char* key, const std::string& value) {
        if (!value.empty())
            params.emplace_back(key, encode_uri_component(value));
    };

    add_param("iid", query.iid);
    add_param("v", query.version);
    add_param("os", query.os);
    add_param("arch", query.arch);
    add_param("os_info", query.os_info);

    std::sort(params.begin(), params.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });

    if (params.empty())
        return {};

    std::string encoded;
    for (size_t idx = 0; idx < params.size(); ++idx) {
        if (idx > 0)
            encoded.push_back('&');
        encoded += params[idx].first;
        encoded.push_back('=');
        encoded += params[idx].second;
    }
    return encoded;
}

std::string base64url_encode(const unsigned char* data, std::size_t length)
{
    std::string encoded;
    encoded.resize(boost::beast::detail::base64::encoded_size(length));
    encoded.resize(boost::beast::detail::base64::encode(encoded.data(), data, length));
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    while (!encoded.empty() && encoded.back() == '=')
        encoded.pop_back();
    return encoded;
}

std::optional<std::vector<unsigned char>> load_signature_key()
{
#if ORCA_UPDATER_SIG_KEY_AVAILABLE
    std::string key = ORCA_UPDATER_SIG_KEY_B64;
    boost::algorithm::trim(key);
    if (key.empty())
        return std::nullopt;

    key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char ch) { return std::isspace(ch); }), key.end());
    std::replace(key.begin(), key.end(), '-', '+');
    std::replace(key.begin(), key.end(), '_', '/');
    while (key.size() % 4 != 0)
        key.push_back('=');

    std::string decoded;
    decoded.resize(boost::beast::detail::base64::decoded_size(key.size()));
    auto decode_result = boost::beast::detail::base64::decode(decoded.data(), key.data(), key.size());
    if (!decode_result.second)
        return std::nullopt;
    decoded.resize(decode_result.first);

    return std::vector<unsigned char>(decoded.begin(), decoded.end());
#else
    return std::nullopt;
#endif
}

const std::optional<std::vector<unsigned char>>& get_signature_key()
{
    static std::optional<std::vector<unsigned char>> cached;
    static bool loaded = false;
    if (!loaded) {
        cached = load_signature_key();
        loaded = true;
    }
    return cached;
}

std::string extract_path_from_url(const std::string& url)
{
    if (url.empty())
        return "/latest";

    std::string path;
    const auto scheme_pos = url.find("://");
    if (scheme_pos != std::string::npos) {
        const auto path_pos = url.find('/', scheme_pos + 3);
        if (path_pos != std::string::npos)
            path = url.substr(path_pos);
        else
            path = "/";
    } else {
        path = url;
    }

    const auto fragment_pos = path.find('#');
    if (fragment_pos != std::string::npos)
        path = path.substr(0, fragment_pos);

    const auto query_pos = path.find('?');
    if (query_pos != std::string::npos)
        path = path.substr(0, query_pos);

    if (path.empty())
        path = "/";
    return path;
}

void maybe_attach_updater_signature(Http& http, const std::string& canonical_query, const std::string& request_url)
{
    if (canonical_query.empty())
        return;

    const auto& key = get_signature_key();
    if (!key || key->empty())
        return;

    const auto now   = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    const std::string timestamp = std::to_string(now.time_since_epoch().count());
    const std::string path      = extract_path_from_url(request_url);

    std::string string_to_sign = "GET\n";
    string_to_sign += path;
    string_to_sign += "\n";
    string_to_sign += canonical_query;
    string_to_sign += "\n";
    string_to_sign += timestamp;

    unsigned int digest_length = 0;
    unsigned char digest[EVP_MAX_MD_SIZE] = {};
    if (HMAC(EVP_sha256(), key->data(), static_cast<int>(key->size()),
             reinterpret_cast<const unsigned char*>(string_to_sign.data()),
             string_to_sign.size(), digest, &digest_length) == nullptr || digest_length == 0)
        return;

    const std::string signature = base64url_encode(digest, digest_length);
    http.header("X-Orca-Ts", timestamp);
    http.header("X-Orca-Sig", "v1:" + signature);
}

} // namespace

void GUI_App::check_new_version_sf(bool show_tips, int by_user)
{
    AppConfig* app_config = wxGetApp().app_config;
    bool       check_stable_only = app_config->get_bool("check_stable_update_only");
    auto version_check_url = app_config->version_check_url();

    UpdaterQuery query{
        detect_updater_iid(app_config),
        detect_updater_version(),
        detect_updater_os(),
        detect_updater_arch(),
        detect_updater_os_info()
    };

    const std::string query_string = build_updater_query(query);
    if (!query_string.empty()) {
        const bool has_query = version_check_url.find('?') != std::string::npos;
        if (!has_query)
            version_check_url.push_back('?');
        else if (!version_check_url.empty() && version_check_url.back() != '&' && version_check_url.back() != '?')
            version_check_url.push_back('&');
        version_check_url += query_string;
    }

    auto http = Http::get(version_check_url);
    maybe_attach_updater_signature(http, query_string, version_check_url);

    http.header("accept", "application/vnd.github.v3+json")
        .timeout_connect(5)
        .timeout_max(10)
        .on_error([&](std::string body, std::string error, unsigned http_status) {
          (void)body;
          BOOST_LOG_TRIVIAL(error) << format("Error getting: `%1%`: HTTP %2%, %3%", "check_new_version_sf", http_status,
                                             error);
        })
        .on_complete([this, by_user, check_stable_only](std::string body, unsigned http_status) {
          if (http_status != 200)
            return;
          try {
            boost::trim(body);
            if (body.empty()) {
                if (by_user != 0)
                    this->no_new_version();
                return;
            }

            boost::property_tree::ptree root;
            std::stringstream           json_stream(body);
            boost::property_tree::read_json(json_stream, root);

            std::regex matcher("[0-9]+\\.[0-9]+(\\.[0-9]+)*(-[A-Za-z0-9]+)?(\\+[A-Za-z0-9]+)?");
            Semver    current_version = get_version(SoftFever_VERSION, matcher);
            Semver    best_pre(0, 0, 0);
            Semver    best_release(0, 0, 0);
            bool      best_pre_valid = false;
            bool      best_release_valid = false;
            std::string best_pre_url;
            std::string best_release_url;
            std::string best_release_content;
            std::string best_pre_content;

            auto consider_release = [&](const boost::property_tree::ptree& node) {
                auto tag_opt = node.get_optional<std::string>("tag_name");
                if (!tag_opt)
                    return;

                std::string tag = *tag_opt;
                if (!tag.empty() && tag.front() == 'v')
                    tag.erase(0, 1);

                Semver tag_version = get_version(tag, matcher);
                if (!tag_version.valid())
                    return;

                const bool is_prerelease = node.get_optional<bool>("prerelease").get_value_or(false);
                const std::string html_url = node.get_optional<std::string>("html_url").get_value_or(std::string());
                const std::string body_copy = node.get_optional<std::string>("body").get_value_or(std::string());

                if (is_prerelease) {
                    if (!best_pre_valid || best_pre < tag_version) {
                        best_pre        = tag_version;
                        best_pre_url    = html_url;
                        best_pre_content = body_copy;
                        best_pre_valid  = true;
                    }
                } else {
                    if (!best_release_valid || best_release < tag_version) {
                        best_release         = tag_version;
                        best_release_url     = html_url;
                        best_release_content = body_copy;
                        best_release_valid   = true;
                    }
                }
            };

            if (root.get_optional<std::string>("tag_name")) {
                consider_release(root);
            } else {
                for (const auto& child : root)
                    consider_release(child.second);
            }

            if (!best_release_valid && !best_pre_valid) {
                if (by_user != 0)
                    this->no_new_version();
                return;
            }

            if (best_pre_valid && best_release_valid && best_pre < best_release) {
                best_pre        = best_release;
                best_pre_url    = best_release_url;
                best_pre_content = best_release_content;
                best_pre_valid  = true;
            }

            const bool        prefer_release = check_stable_only || !best_pre_valid;
            const Semver&     chosen_version = prefer_release ? best_release : best_pre;
            const bool        chosen_valid   = prefer_release ? best_release_valid : best_pre_valid;

            if (!chosen_valid) {
                if (by_user != 0)
                    this->no_new_version();
                return;
            }

            if (current_version.valid() && chosen_version <= current_version) {
                if (by_user != 0)
                    this->no_new_version();
                return;
            }

            version_info.url           = prefer_release ? best_release_url : best_pre_url;
            version_info.version_str   = prefer_release ? best_release.to_string_sf() : best_pre.to_string();
            version_info.description   = prefer_release ? best_release_content : best_pre_content;
            version_info.force_upgrade = false;

            wxCommandEvent* evt = new wxCommandEvent(EVT_SLIC3R_VERSION_ONLINE);
            evt->SetString((prefer_release ? best_release : best_pre).to_string());
            GUI::wxGetApp().QueueEvent(evt);
          } catch (...) {}
        });

    http.perform();
}

// return true if handled
bool GUI_App::process_network_msg(std::string dev_id, std::string msg)
{
    if (dev_id.empty()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << msg;
    }
    else if (msg == "device_cert_installed") {
        BOOST_LOG_TRIVIAL(info) << "process_network_msg, device_cert_installed";
        if (Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager()) {
            if (MachineObject* obj = dev->get_my_machine(dev_id)) {
                obj->update_device_cert_state(true);
            }
        }

        return true;
    }
    else if (msg == "device_cert_uninstalled") {
        BOOST_LOG_TRIVIAL(info) << "process_network_msg, device_cert_uninstalled";
        if (Slic3r::DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager()) {
            if (MachineObject* obj = dev->get_my_machine(dev_id)){
                obj->update_device_cert_state(false);
            }
        }

        return true;
    }

    return false;
}

//BBS pop up a dialog and download files
void GUI_App::request_new_version(int by_user)
{
    wxCommandEvent* evt = new wxCommandEvent(EVT_SLIC3R_VERSION_ONLINE);
    evt->SetString(GUI::from_u8(version_info.version_str));
    evt->SetInt(by_user);
    GUI::wxGetApp().QueueEvent(evt);
}

void GUI_App::enter_force_upgrade()
{
    wxCommandEvent *evt = new wxCommandEvent(EVT_ENTER_FORCE_UPGRADE);
    GUI::wxGetApp().QueueEvent(evt);
}

void GUI_App::set_skip_version(bool skip)
{
    BOOST_LOG_TRIVIAL(info) << "set_skip_version, skip = " << skip << ", version = " <<version_info.version_str;
    if (skip) {
        app_config->set("skip_version", version_info.version_str);
    }else {
        app_config->set("skip_version", "");
    }
}

void GUI_App::show_check_privacy_dlg(wxCommandEvent& evt)
{
    int online_login = evt.GetInt();
    PrivacyUpdateDialog privacy_dlg(this->mainframe, wxID_ANY, _L("Privacy Policy Update"));
    privacy_dlg.Bind(EVT_PRIVACY_UPDATE_CONFIRM, [this, online_login](wxCommandEvent &e) {
        app_config->set("privacy_version", privacy_version_info.version_str);
        app_config->set_bool("privacy_update_checked", true);
        request_user_handle(online_login);
        });
    privacy_dlg.Bind(EVT_PRIVACY_UPDATE_CANCEL, [this](wxCommandEvent &e) {
            app_config->set_bool("privacy_update_checked", false);
            if (m_agent) {
                m_agent->user_logout();
            }
        });

    privacy_dlg.set_text(privacy_version_info.description);
    privacy_dlg.on_show();
}

void GUI_App::on_show_check_privacy_dlg(int online_login)
{
    auto evt = new wxCommandEvent(EVT_CHECK_PRIVACY_SHOW);
    evt->SetInt(online_login);
    wxQueueEvent(this, evt);
}

bool GUI_App::check_privacy_update()
{
    if (privacy_version_info.version_str.empty() || privacy_version_info.description.empty()
        || privacy_version_info.url.empty()) {
        return false;
    }

    std::string local_privacy_ver = app_config->get("privacy_version");
    auto curr_version = Semver::parse(local_privacy_ver);
    auto remote_version = Semver::parse(privacy_version_info.version_str);
    if (curr_version && remote_version) {
        if (*remote_version > *curr_version || app_config->get("privacy_update_checked") != "true") {
            return true;
        }
    }
    return false;
}

void GUI_App::on_check_privacy_update(wxCommandEvent& evt)
{
    int online_login = evt.GetInt();
    bool result = check_privacy_update();
    if (result)
        on_show_check_privacy_dlg(online_login);
    else
        request_user_handle(online_login);
}

void GUI_App::check_privacy_version(int online_login)
{
    update_http_extra_header();
    std::string query_params = "?policy/privacy=00.00.00.00";
    std::string url = get_http_url(app_config->get_country_code()) + query_params;
    Slic3r::Http http = Slic3r::Http::get(url);

    http.header("accept", "application/json")
        .timeout_connect(TIMEOUT_CONNECT)
        .timeout_max(TIMEOUT_RESPONSE)
        .on_complete([this, online_login](std::string body, unsigned) {
            try {
                json j = json::parse(body);
                if (j.contains("message")) {
                    if (j["message"].get<std::string>() == "success") {
                        if (j.contains("resources")) {
                            for (auto it = j["resources"].begin(); it != j["resources"].end(); it++) {
                                if (it->contains("type")) {
                                    if ((*it)["type"] == std::string("policy/privacy")
                                        && it->contains("version")
                                        && it->contains("description")
                                        && it->contains("url")
                                        && it->contains("force_update")) {
                                        privacy_version_info.version_str = (*it)["version"].get<std::string>();
                                        privacy_version_info.description = (*it)["description"].get<std::string>();
                                        privacy_version_info.url = (*it)["url"].get<std::string>();
                                        privacy_version_info.force_upgrade = (*it)["force_update"].get<bool>();
                                        break;
                                    }
                                }
                            }
                            CallAfter([this, online_login]() {
                                auto evt = new wxCommandEvent(EVT_CHECK_PRIVACY_VER);
                                evt->SetInt(online_login);
                                wxQueueEvent(this, evt);
                            });
                        }
                    }
                }
            }
            catch (...) {
                request_user_handle(online_login);
            }
        })
        .on_error([this, online_login](std::string body, std::string error, unsigned int status) {
            request_user_handle(online_login);
            BOOST_LOG_TRIVIAL(error) << "check privacy version error" << body;
    }).perform();
}

void GUI_App::no_new_version()
{
    wxCommandEvent* evt = new wxCommandEvent(EVT_SHOW_NO_NEW_VERSION);
    GUI::wxGetApp().QueueEvent(evt);
}

std::string GUI_App::version_display = "";
std::string GUI_App::format_display_version()
{
    if (!version_display.empty()) return version_display;

    version_display = SoftFever_VERSION;
    return version_display;
}

std::string GUI_App::format_IP(const std::string& ip)
{
    std::string format_ip = ip;
    size_t pos_st = 0;
    size_t pos_en = 0;

    for (int i = 0; i < 2; i++) {
        pos_en = format_ip.find('.', pos_st + 1);
        if (pos_en == std::string::npos) {
            return ip;
        }
        format_ip.replace(pos_st, pos_en - pos_st, "***");
        pos_st = pos_en + 1;
    }

    return format_ip;
}

void GUI_App::show_dialog(wxString msg)
{
    if (m_info_dialog_content.empty()) {
        wxCommandEvent* evt = new wxCommandEvent(EVT_SHOW_DIALOG);
        evt->SetString(msg);
        GUI::wxGetApp().QueueEvent(evt);
        m_info_dialog_content = msg;
    }
}

void  GUI_App::push_notification(const MachineObject* obj, wxString msg, wxString title, UserNotificationStyle style)
{
    if (this->is_enable_multi_machine())
    {
        if (m_device_manager && (obj != m_device_manager->get_selected_machine()))
        {
            return;
        }
    }

    if (style == UserNotificationStyle::UNS_NORMAL)
    {
        if (m_info_dialog_content.empty())
        {
            wxCommandEvent* evt = new wxCommandEvent(EVT_SHOW_DIALOG);
            evt->SetString(msg);
            GUI::wxGetApp().QueueEvent(evt);
            m_info_dialog_content = msg;
        }
    }
    else if (style == UserNotificationStyle::UNS_WARNING_CONFIRM)
    {
        GUI::wxGetApp().CallAfter([msg, title]
            {
                GUI::MessageDialog msg_dlg(nullptr, msg, title, wxICON_WARNING | wxOK);
                msg_dlg.ShowModal();
            });
    }
}

void GUI_App::reload_settings()
{
    if (preset_bundle && m_agent) {
        std::map<std::string, std::map<std::string, std::string>> user_presets;
        m_agent->get_user_presets(&user_presets);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " cloud user preset number is: " << user_presets.size();
        preset_bundle->load_user_presets(*app_config, user_presets, ForwardCompatibilitySubstitutionRule::Enable);
        preset_bundle->save_user_presets(*app_config, get_delete_cache_presets());
        mainframe->update_side_preset_ui();
    }
}

//BBS reload when logout
void GUI_App::remove_user_presets()
{
    if (preset_bundle && m_agent) {
        preset_bundle->remove_users_preset(*app_config);

        // Not remove user preset cache
        //std::string user_id = m_agent->get_user_id();
        //preset_bundle->remove_user_presets_directory(user_id);

        //update ui
        mainframe->update_side_preset_ui();
    }
}

void GUI_App::sync_preset(Preset* preset)
{
    int result = -1;
    unsigned int http_code = 200;
    std::string updated_info;
    long long update_time = 0;
    // only sync user's preset
    if (!preset->is_user()) return;

    auto setting_id = preset->setting_id;
    std::map<std::string, std::string> values_map;
    if (setting_id.empty() && preset->sync_info.empty()) {
        if (m_create_preset_blocked[preset->type])
            return;
        int ret = preset_bundle->get_differed_values_to_update(*preset, values_map);
        if (!ret) {
            std::string new_setting_id = m_agent->request_setting_id(preset->name, &values_map, &http_code);
            if (!new_setting_id.empty()) {
                setting_id = new_setting_id;
                result = 0;
                auto update_time_str = values_map[BBL_JSON_KEY_UPDATE_TIME];
                if (!update_time_str.empty())
                    update_time = std::atoll(update_time_str.c_str());
            }
            else {
                BOOST_LOG_TRIVIAL(trace) << "[sync_preset]init: request_setting_id failed, http code "<<http_code;
                // do not post new preset this time if http code >= 400
                if (http_code >= 400) {
                    result = 0;
                    updated_info = "hold";
                } else
                    result = -1;
            }
        }
        else {
            BOOST_LOG_TRIVIAL(trace) << "[sync_preset]init: can not generate differed key-values";
            result = 0;
            updated_info = "hold";
        }
    }
    else if (preset->sync_info.compare("create") == 0) {
        if (m_create_preset_blocked[preset->type])
            return;
        int ret = preset_bundle->get_differed_values_to_update(*preset, values_map);
        if (!ret) {
            std::string new_setting_id = m_agent->request_setting_id(preset->name, &values_map, &http_code);
            if (!new_setting_id.empty()) {
                setting_id = new_setting_id;
                result = 0;
                auto update_time_str = values_map[BBL_JSON_KEY_UPDATE_TIME];
                if (!update_time_str.empty())
                    update_time = std::atoll(update_time_str.c_str());
            } else {
                BOOST_LOG_TRIVIAL(trace) << "[sync_preset]create: request_setting_id failed, http code "<<http_code;
                // do not post new preset this time if http code >= 400
                if (http_code >= 400) {
                    result = 0;
                    updated_info = "hold";
                }
                else
                    result = -1;
            }
        } else {
            BOOST_LOG_TRIVIAL(trace) << "[sync_preset]create: can not generate differed preset";
        }
    } else if (preset->sync_info.compare("update") == 0) {
        if (!setting_id.empty()) {
            int ret = preset_bundle->get_differed_values_to_update(*preset, values_map);
            if (!ret) {
                if (auto iter = values_map.find(BBL_JSON_KEY_BASE_ID); iter != values_map.end() && iter->second == setting_id) {
                    //clear the setting_id in this case ???
                    setting_id.clear();
                    result = 0;
                }
                else {
                    result = m_agent->put_setting(setting_id, preset->name, &values_map, &http_code);
                    if (http_code >= 400) {
                        result = 0;
                        updated_info = "hold";
                        BOOST_LOG_TRIVIAL(error) << "[sync_preset] put setting_id = " << setting_id << " failed, http_code = " << http_code;
                    } else {
                        auto update_time_str = values_map[BBL_JSON_KEY_UPDATE_TIME];
                        if (!update_time_str.empty())
                            update_time = std::atoll(update_time_str.c_str());
                    }
                }

            }
            else {
                BOOST_LOG_TRIVIAL(trace) << "[sync_preset]update: can not generate differed key-values, we need to skip this preset "<< preset->name;
                result = 0;
            }
        }
        else {
            //clear the sync_info
            result = 0;
        }
    }

    if (http_code >= 400 && values_map["code"] == "14") { // Limit
        m_create_preset_blocked[preset->type] = true;
        CallAfter([this] {
            plater()->get_notification_manager()->push_notification(NotificationType::BBLUserPresetExceedLimit);
            static bool dialog_notified = false;
            if (dialog_notified)
                return;
            dialog_notified = true;
            if (mainframe == nullptr)
                return;
            auto msg = _L("The number of user presets cached in the cloud has exceeded the upper limit, newly created user presets can only be used locally.");
            MessageDialog(mainframe, msg, _L("Sync user presets"), wxICON_WARNING | wxOK).ShowModal();
        });
        return; // this error not need hold, and should not hold
    }

    // update sync_info preset info in file
    if (result == 0) {
        //PresetBundle* preset_bundle = wxGetApp().preset_bundle;
        if (!this->preset_bundle) return;

        BOOST_LOG_TRIVIAL(trace) << "sync_preset: sync operation: " << preset->sync_info << " success! preset = " << preset->name;
        if (preset->type == Preset::Type::TYPE_FILAMENT) {
            preset_bundle->filaments.set_sync_info_and_save(preset->name, setting_id, updated_info, update_time);
        } else if (preset->type == Preset::Type::TYPE_PRINT) {
            preset_bundle->prints.set_sync_info_and_save(preset->name, setting_id, updated_info, update_time);
        } else if (preset->type == Preset::Type::TYPE_PRINTER) {
            preset_bundle->printers.set_sync_info_and_save(preset->name, setting_id, updated_info, update_time);
        }
    }
}

void GUI_App::start_sync_user_preset(bool with_progress_dlg)
{
    if (app_config->get_stealth_mode())
        return;

    if (!m_agent || !m_agent->is_user_login()) return;

    // has already start sync
    if (m_user_sync_token) return;

    ProgressFn progressFn;
    WasCancelledFn cancelFn;
    std::function<void(bool)> finishFn;

    BOOST_LOG_TRIVIAL(info) << "start_sync_service...";
    // BBS
    m_user_sync_token.reset(new int(0));
    if (with_progress_dlg) {
        auto dlg = new ProgressDialog(_L("Loading"), "", 100, this->mainframe, wxPD_AUTO_HIDE | wxPD_APP_MODAL | wxPD_CAN_ABORT);
        dlg->Update(0, _L("Loading user preset"));
        progressFn = [this, dlg](int percent) {
            CallAfter([=]{
                dlg->Update(percent, _L("Loading user preset"));
            });
        };
        cancelFn = [this, dlg]() {
            return is_closing() || dlg->WasCanceled();
        };
        finishFn = [this, userid = m_agent->get_user_id(), dlg, t = std::weak_ptr<int>(m_user_sync_token)](bool ok) {
            CallAfter([=]{
                dlg->Destroy();
                if (ok && m_agent && t.lock() == m_user_sync_token && userid == m_agent->get_user_id()) reload_settings();
            });
        };
    }
    else {
        finishFn = [this, userid = m_agent->get_user_id(), t = std::weak_ptr<int>(m_user_sync_token)](bool ok) {
            CallAfter([=] {
                if (ok && m_agent && t.lock() == m_user_sync_token && userid == m_agent->get_user_id()) reload_settings();
            });
        };
        cancelFn = [this]() {
            return is_closing();
        };
    }

    m_sync_update_thread = Slic3r::create_thread(
        [this, progressFn, cancelFn, finishFn, t = std::weak_ptr<int>(m_user_sync_token)] {
            // get setting list, update setting list
            std::string version = preset_bundle->get_vendor_profile_version(PresetBundle::ORCA_DEFAULT_BUNDLE).to_string();
            if(!m_agent) return;
            int ret = m_agent->get_setting_list2(version, [this](auto info) {
                auto type = info[BBL_JSON_KEY_TYPE];
                auto name = info[BBL_JSON_KEY_NAME];
                auto setting_id = info[BBL_JSON_KEY_SETTING_ID];
                auto update_time_str = info[BBL_JSON_KEY_UPDATE_TIME];
                long long update_time = 0;
                if (!update_time_str.empty())
                    update_time = std::atoll(update_time_str.c_str());
                if (type == "filament") {
                    return preset_bundle->filaments.need_sync(name, setting_id, update_time);
                } else if (type == "print") {
                    return preset_bundle->prints.need_sync(name, setting_id, update_time);
                } else if (type == "printer") {
                    return preset_bundle->printers.need_sync(name, setting_id, update_time);
                } else {
                    return true;
                }
            }, progressFn, cancelFn);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " get_setting_list2 ret = " << ret << " m_is_closing = " << m_is_closing;
            finishFn(ret == 0);

            int count = 0, sync_count = 0;
            std::vector<Preset> presets_to_sync;
            while (!t.expired()) {
                count++;
                if (count % 20 == 0) {
                    if (m_agent) {
                        if (!m_agent->is_user_login()) {
                            continue;
                        }
                        //sync preset
                        if (!preset_bundle) continue;

                        int total_count = 0;
                        sync_count = preset_bundle->prints.get_user_presets(preset_bundle, presets_to_sync);
                        if (sync_count > 0) {
                            for (Preset& preset : presets_to_sync) {
                                sync_preset(&preset);
                                boost::this_thread::sleep_for(boost::chrono::milliseconds(100));
                            }
                        }
                        total_count += sync_count;

                        sync_count = preset_bundle->filaments.get_user_presets(preset_bundle, presets_to_sync);
                        if (sync_count > 0) {
                            for (Preset& preset : presets_to_sync) {
                                sync_preset(&preset);
                                boost::this_thread::sleep_for(boost::chrono::milliseconds(100));
                            }
                        }
                        total_count += sync_count;

                        sync_count = preset_bundle->printers.get_user_presets(preset_bundle, presets_to_sync);
                        if (sync_count > 0) {
                            for (Preset& preset : presets_to_sync) {
                                sync_preset(&preset);
                                boost::this_thread::sleep_for(boost::chrono::milliseconds(100));
                            }
                        }
                        total_count += sync_count;

                        if (total_count == 0) {
                            CallAfter([this] {
                                if (!is_closing())
                                    plater()->get_notification_manager()->close_notification_of_type(NotificationType::BBLUserPresetExceedLimit);
                            });
                        }

                        unsigned int http_code = 200;

                        /* get list witch need to be deleted*/
                        std::vector<string> delete_cache_presets = get_delete_cache_presets_lock();
                        for (auto it = delete_cache_presets.begin(); it != delete_cache_presets.end();) {
                            if ((*it).empty()) continue;
                            std::string del_setting_id = *it;
                            int result = m_agent->delete_setting(del_setting_id);
                            if (result == 0) {
                                preset_deleted_from_cloud(del_setting_id);
                                it = delete_cache_presets.erase(it);
                                m_create_preset_blocked = { false, false, false, false, false, false };
                                BOOST_LOG_TRIVIAL(trace) << "sync_preset: sync operation: delete success! setting id = " << del_setting_id;
                            }
                            else {
                                BOOST_LOG_TRIVIAL(info) << "delete setting = " <<del_setting_id << " failed";
                                it++;
                            }
                        }
                    }
                } else {
                    boost::this_thread::sleep_for(boost::chrono::milliseconds(100));
                }
            }
        });
}

void GUI_App::stop_sync_user_preset()
{
    if (!m_user_sync_token)
        return;

    m_user_sync_token.reset();
    if (m_sync_update_thread.joinable()) {
        if (is_closing())
            m_sync_update_thread.join();
        else
            m_sync_update_thread.detach();
    }
}

void GUI_App::start_http_server()
{
    if (!m_http_server.is_started())
        m_http_server.start();
}
void GUI_App::stop_http_server()
{
    m_http_server.stop();
}

void GUI_App::switch_staff_pick(bool on)
{
    mainframe->m_webview->SendDesignStaffpick(on);
}

bool GUI_App::switch_language()
{
    if (select_language()) {
        recreate_GUI(_L("Switching application language") + dots);
        return true;
    } else {
        return false;
    }
}

#ifdef __linux__
static const wxLanguageInfo* linux_get_existing_locale_language(const wxLanguageInfo* language,
                                                                const wxLanguageInfo* system_language)
{
    constexpr size_t max_len = 50;
    char path[max_len] = "";
    std::vector<std::string> locales;
    const std::string lang_prefix = into_u8(language->CanonicalName.BeforeFirst('_'));

    // Call locale -a so we can parse the output to get the list of available locales
    // We expect lines such as "en_US.utf8". Pick ones starting with the language code
    // we are switching to. Lines with different formatting will be removed later.
    FILE* fp = popen("locale -a", "r");
    if (fp != NULL) {
        while (fgets(path, max_len, fp) != NULL) {
            std::string line(path);
            line = line.substr(0, line.find('\n'));
            if (boost::starts_with(line, lang_prefix))
                locales.push_back(line);
        }
        pclose(fp);
    }

    // locales now contain all candidates for this language.
    // Sort them so ones containing anything about UTF-8 are at the end.
    std::sort(locales.begin(), locales.end(), [](const std::string& a, const std::string& b)
    {
        auto has_utf8 = [](const std::string & s) {
            auto S = boost::to_upper_copy(s);
            return S.find("UTF8") != std::string::npos || S.find("UTF-8") != std::string::npos;
        };
        return ! has_utf8(a) && has_utf8(b);
    });

    // Remove the suffix behind a dot, if there is one.
    for (std::string& s : locales)
        s = s.substr(0, s.find("."));

    // We just hope that dear Linux "locale -a" returns country codes
    // in ISO 3166-1 alpha-2 code (two letter) format.
    // https://en.wikipedia.org/wiki/List_of_ISO_3166_country_codes
    // To be sure, remove anything not looking as expected
    // (any number of lowercase letters, underscore, two uppercase letters).
    locales.erase(std::remove_if(locales.begin(),
                                 locales.end(),
                                 [](const std::string& s) {
                                     return ! std::regex_match(s,
                                         std::regex("^[a-z]+_[A-Z]{2}$"));
                                 }),
                   locales.end());

    // Is there a candidate matching a country code of a system language? Move it to the end,
    // while maintaining the order of matches, so that the best match ends up at the very end.
    std::string temp_local = into_u8(system_language->CanonicalName.AfterFirst('_'));
    if (temp_local.size() >= 2) {
        temp_local = temp_local.substr(0, 2);
    }
    std::string system_country = "_" + temp_local;
    int cnt = locales.size();
    for (int i=0; i<cnt; ++i)
        if (locales[i].find(system_country) != std::string::npos) {
            locales.emplace_back(std::move(locales[i]));
            locales[i].clear();
        }

    // Now try them one by one.
    for (auto it = locales.rbegin(); it != locales.rend(); ++ it)
        if (! it->empty()) {
            const std::string &locale = *it;
            const wxLanguageInfo* lang = wxLocale::FindLanguageInfo(from_u8(locale));
            if (wxLocale::IsAvailable(lang->Language))
                return lang;
        }
    return language;
}
#endif

int GUI_App::GetSingleChoiceIndex(const wxString& message,
                                const wxString& caption,
                                const wxArrayString& choices,
                                int initialSelection)
{
#ifdef _WIN32
    wxSingleChoiceDialog dialog(nullptr, message, caption, choices);
    dialog.SetBackgroundColour(*wxWHITE);
    wxGetApp().UpdateDlgDarkUI(&dialog);

    dialog.SetSelection(initialSelection);
    return dialog.ShowModal() == wxID_OK ? dialog.GetSelection() : -1;
#else
    return wxGetSingleChoiceIndex(message, caption, choices, initialSelection);
#endif
}

// select language from the list of installed languages
bool GUI_App::select_language()
{
	wxArrayString translations = wxTranslations::Get()->GetAvailableTranslations(SLIC3R_APP_KEY);
    std::vector<const wxLanguageInfo*> language_infos;
    language_infos.emplace_back(wxLocale::GetLanguageInfo(wxLANGUAGE_ENGLISH));
    for (size_t i = 0; i < translations.GetCount(); ++ i) {
	    const wxLanguageInfo *langinfo = wxLocale::FindLanguageInfo(translations[i]);
        if (langinfo != nullptr)
            language_infos.emplace_back(langinfo);
    }
    sort_remove_duplicates(language_infos);
	std::sort(language_infos.begin(), language_infos.end(), [](const wxLanguageInfo* l, const wxLanguageInfo* r) { return l->Description < r->Description; });

    wxArrayString names;
    names.Alloc(language_infos.size());

    // Some valid language should be selected since the application start up.
    const wxLanguage current_language = wxLanguage(m_wxLocale->GetLanguage());
    int 		     init_selection   		= -1;
    int 			 init_selection_alt     = -1;
    int 			 init_selection_default = -1;
    for (size_t i = 0; i < language_infos.size(); ++ i) {
        if (wxLanguage(language_infos[i]->Language) == current_language)
        	// The dictionary matches the active language and country.
            init_selection = i;
        else if ((language_infos[i]->CanonicalName.BeforeFirst('_') == m_wxLocale->GetCanonicalName().BeforeFirst('_')) ||
        		 // if the active language is Slovak, mark the Czech language as active.
        	     (language_infos[i]->CanonicalName.BeforeFirst('_') == "cs" && m_wxLocale->GetCanonicalName().BeforeFirst('_') == "sk"))
        	// The dictionary matches the active language, it does not necessarily match the country.
        	init_selection_alt = i;
        if (language_infos[i]->CanonicalName.BeforeFirst('_') == "en")
        	// This will be the default selection if the active language does not match any dictionary.
        	init_selection_default = i;
        names.Add(language_infos[i]->Description);
    }
    if (init_selection == -1)
    	// This is the dictionary matching the active language.
    	init_selection = init_selection_alt;
    if (init_selection != -1)
    	// This is the language to highlight in the choice dialog initially.
    	init_selection_default = init_selection;

    const long index = GetSingleChoiceIndex(_L("Select the language"), _L("Language"), names, init_selection_default);
	// Try to load a new language.
    if (index != -1 && (init_selection == -1 || init_selection != index)) {
    	const wxLanguageInfo *new_language_info = language_infos[index];
    	if (this->load_language(new_language_info->CanonicalName, false)) {
			// Save language at application config.
            // Which language to save as the selected dictionary language?
            // 1) Hopefully the language set to wxTranslations by this->load_language(), but that API is weird and we don't want to rely on its
            //    stability in the future:
            //    wxTranslations::Get()->GetBestTranslation(SLIC3R_APP_KEY, wxLANGUAGE_ENGLISH);
            // 2) Current locale language may not match the dictionary name, see GH issue #3901
            //    m_wxLocale->GetCanonicalName()
            // 3) new_language_info->CanonicalName is a safe bet. It points to a valid dictionary name.
			app_config->set("language", new_language_info->CanonicalName.ToUTF8().data());
    		return true;
        }
    }

    return false;
}

// Load gettext translation files and activate them at the start of the application,
// based on the "language" key stored in the application config.
bool GUI_App::load_language(wxString language, bool initial)
{
    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: language %2%, initial: %3%") %__FUNCTION__ %language %initial;
    if (initial) {
    	// There is a static list of lookup path prefixes in wxWidgets. Add ours.
	    wxFileTranslationsLoader::AddCatalogLookupPathPrefix(from_u8(localization_dir()));
    	// Get the active language from PrusaSlicer.ini, or empty string if the key does not exist.
        language = app_config->get("language");
        if (! language.empty())
        	BOOST_LOG_TRIVIAL(info) << boost::format("language provided by OrcaSlicer.conf: %1%") % language;
        else {
            // Get the system language.
            const wxLanguage lang_system = wxLanguage(wxLocale::GetSystemLanguage());
            if (lang_system != wxLANGUAGE_UNKNOWN) {
                m_language_info_system = wxLocale::GetLanguageInfo(lang_system);
#ifdef __WXMSW__
                WCHAR wszLanguagesBuffer[LOCALE_NAME_MAX_LENGTH];
                ::LCIDToLocaleName(LOCALE_USER_DEFAULT, wszLanguagesBuffer, LOCALE_NAME_MAX_LENGTH, 0);
                wxString lang(wszLanguagesBuffer);
                lang.Replace('-', '_');
                if (auto info = wxLocale::FindLanguageInfo(lang))
                    m_language_info_system = info;
#endif
                BOOST_LOG_TRIVIAL(info) << boost::format("System language detected (user locales and such): %1%") % m_language_info_system->CanonicalName.ToUTF8().data();
                // BBS set language to app config
                app_config->set("language", m_language_info_system->CanonicalName.ToUTF8().data());
            } else {
                {
                    // Allocating a temporary locale will switch the default wxTranslations to its internal wxTranslations instance.
                    wxLocale temp_locale;
                    temp_locale.Init();
                    // Set the current translation's language to default, otherwise GetBestTranslation() may not work (see the wxWidgets source code).
                    wxTranslations::Get()->SetLanguage(wxLANGUAGE_DEFAULT);
                    // Let the wxFileTranslationsLoader enumerate all translation dictionaries for PrusaSlicer
                    // and try to match them with the system specific "preferred languages".
                    // There seems to be a support for that on Windows and OSX, while on Linuxes the code just returns wxLocale::GetSystemLanguage().
                    // The last parameter gets added to the list of detected dictionaries. This is a workaround
                    // for not having the English dictionary. Let's hope wxWidgets of various versions process this call the same way.
                    wxString best_language = wxTranslations::Get()->GetBestTranslation(SLIC3R_APP_KEY, wxLANGUAGE_ENGLISH);
                    if (!best_language.IsEmpty()) {
                        m_language_info_best = wxLocale::FindLanguageInfo(best_language);
                        BOOST_LOG_TRIVIAL(info) << boost::format("Best translation language detected (may be different from user locales): %1%") %
                                                        m_language_info_best->CanonicalName.ToUTF8().data();
                        app_config->set("language", m_language_info_best->CanonicalName.ToUTF8().data());
                    }
#ifdef __linux__
                    wxString lc_all;
                    if (wxGetEnv("LC_ALL", &lc_all) && !lc_all.IsEmpty()) {
                        // Best language returned by wxWidgets on Linux apparently does not respect LC_ALL.
                        // Disregard the "best" suggestion in case LC_ALL is provided.
                        m_language_info_best = nullptr;
                    }
#endif
                }
            }
        }
    }

	const wxLanguageInfo *language_info = language.empty() ? nullptr : wxLocale::FindLanguageInfo(language);
	if (! language.empty() && (language_info == nullptr || language_info->CanonicalName.empty())) {
		// Fix for wxWidgets issue, where the FindLanguageInfo() returns locales with undefined ANSII code (wxLANGUAGE_KONKANI or wxLANGUAGE_MANIPURI).
		language_info = nullptr;
    	BOOST_LOG_TRIVIAL(error) << boost::format("Language code \"%1%\" is not supported") % language.ToUTF8().data();
	}

	if (language_info != nullptr && language_info->LayoutDirection == wxLayout_RightToLeft) {
    	BOOST_LOG_TRIVIAL(trace) << boost::format("The following language code requires right to left layout, which is not supported by OrcaSlicer: %1%") % language_info->CanonicalName.ToUTF8().data();
		language_info = nullptr;
	}

    if (language_info == nullptr) {
        // PrusaSlicer does not support the Right to Left languages yet.
        if (m_language_info_system != nullptr && m_language_info_system->LayoutDirection != wxLayout_RightToLeft)
            language_info = m_language_info_system;
        if (m_language_info_best != nullptr && m_language_info_best->LayoutDirection != wxLayout_RightToLeft)
        	language_info = m_language_info_best;
	    if (language_info == nullptr)
			language_info = wxLocale::GetLanguageInfo(wxLANGUAGE_ENGLISH_US);
    }

	BOOST_LOG_TRIVIAL(trace) << boost::format("Switching wxLocales to %1%") % language_info->CanonicalName.ToUTF8().data();

    // Select language for locales. This language may be different from the language of the dictionary.
    //if (language_info == m_language_info_best || language_info == m_language_info_system) {
    //    // The current language matches user's default profile exactly. That's great.
    //} else if (m_language_info_best != nullptr && language_info->CanonicalName.BeforeFirst('_') == m_language_info_best->CanonicalName.BeforeFirst('_')) {
    //    // Use whatever the operating system recommends, if it the language code of the dictionary matches the recommended language.
    //    // This allows a Swiss guy to use a German dictionary without forcing him to German locales.
    //    language_info = m_language_info_best;
    //} else if (m_language_info_system != nullptr && language_info->CanonicalName.BeforeFirst('_') == m_language_info_system->CanonicalName.BeforeFirst('_'))
    //    language_info = m_language_info_system;

    // Alternate language code.
    wxLanguage language_dict = wxLanguage(language_info->Language);
    if (language_info->CanonicalName.BeforeFirst('_') == "sk") {
    	// Slovaks understand Czech well. Give them the Czech translation.
    	language_dict = wxLANGUAGE_CZECH;
		BOOST_LOG_TRIVIAL(info) << "Using Czech dictionaries for Slovak language";
    }

#ifdef __linux__
    // If we can't find this locale , try to use different one for the language
    // instead of just reporting that it is impossible to switch.
    if (! wxLocale::IsAvailable(language_info->Language) && m_language_info_system) {
        std::string original_lang = into_u8(language_info->CanonicalName);
        language_info = linux_get_existing_locale_language(language_info, m_language_info_system);
        BOOST_LOG_TRIVIAL(info) << boost::format("Can't switch language to %1% (missing locales). Using %2% instead.")
                                    % original_lang % language_info->CanonicalName.ToUTF8().data();
    }
#endif

    if (! wxLocale::IsAvailable(language_info->Language)&&initial) {
        language_info = wxLocale::GetLanguageInfo(wxLANGUAGE_ENGLISH_UK);
        app_config->set("language", language_info->CanonicalName.ToUTF8().data());
    }
    else if (initial) {
        // bbs supported languages
        //TODO: use a global one with Preference
        //wxLanguage supported_languages[]{
        //    wxLANGUAGE_ENGLISH,
        //    wxLANGUAGE_CHINESE_SIMPLIFIED,
        //    wxLANGUAGE_GERMAN,
        //    wxLANGUAGE_FRENCH,
        //    wxLANGUAGE_SPANISH,
        //    wxLANGUAGE_SWEDISH,
        //    wxLANGUAGE_DUTCH,
        //    wxLANGUAGE_HUNGARIAN,
        //    wxLANGUAGE_JAPANESE,
        //    wxLANGUAGE_ITALIAN
        //};
        //std::string cur_language = app_config->get("language");
        //if (cur_language != "") {
        //    //cleanup the language wrongly set before
        //    const wxLanguageInfo *langinfo = nullptr;
        //    bool embedded_language = false;
        //    int language_num = sizeof(supported_languages) / sizeof(supported_languages[0]);
        //    for (auto index = 0; index < language_num; index++) {
        //        langinfo = wxLocale::GetLanguageInfo(supported_languages[index]);
        //        std::string temp_lan = langinfo->CanonicalName.ToUTF8().data();
        //        if (cur_language == temp_lan) {
        //            embedded_language = true;
        //            break;
        //        }
        //    }
        //    if (!embedded_language)
        //        app_config->erase("app", "language");
        //}
    }

    if (! wxLocale::IsAvailable(language_info->Language)) {
    	// Loading the language dictionary failed.
    	wxString message = "Switching Orca Slicer to language " + language_info->CanonicalName + " failed.";
#if !defined(_WIN32) && !defined(__APPLE__)
        // likely some linux system
        message += "\nYou may need to reconfigure the missing locales, likely by running the \"locale-gen\" and \"dpkg-reconfigure locales\" commands.\n";
#endif
        if (initial)
        	message + "\n\nApplication will close.";
        wxMessageBox(message, "Orca Slicer - Switching language failed", wxOK | wxICON_ERROR);
        if (initial)
			std::exit(EXIT_FAILURE);
		else
			return false;
    }

    // Release the old locales, create new locales.
    //FIXME wxWidgets cause havoc if the current locale is deleted. We just forget it causing memory leaks for now.
    m_wxLocale.release();
    m_wxLocale = Slic3r::make_unique<wxLocale>();
    m_wxLocale->Init(language_info->Language);
    // Override language at the active wxTranslations class (which is stored in the active m_wxLocale)
    // to load possibly different dictionary, for example, load Czech dictionary for Slovak language.
    wxTranslations::Get()->SetLanguage(language_dict);
    m_wxLocale->AddCatalog(SLIC3R_APP_KEY);
    m_imgui->set_language(into_u8(language_info->CanonicalName));

    //FIXME This is a temporary workaround, the correct solution is to switch to "C" locale during file import / export only.
    //wxSetlocale(LC_NUMERIC, "C");
    Preset::update_suffix_modified((_L("*") + " ").ToUTF8().data());
    HintDatabase::get_instance().reinit();
	return true;
}

Tab* GUI_App::get_tab(Preset::Type type)
{
    for (Tab* tab: tabs_list)
        if (tab->type() == type)
            return tab->completed() ? tab : nullptr; // To avoid actions with no-completed Tab
    return nullptr;
}

Tab* GUI_App::get_plate_tab()
{
    return plate_tab;
}

Tab* GUI_App::get_model_tab(bool part)
{
    return model_tabs_list[part ? 1 : 0];
}

Tab* GUI_App::get_layer_tab()
{
    return model_tabs_list[2];
}

ConfigOptionMode GUI_App::get_mode()
{
    if (!app_config->has("user_mode"))
        return comSimple;
    //BBS
    const auto mode = app_config->get("user_mode");
    return mode == "advanced" ? comAdvanced :
           mode == "simple" ? comSimple :
           mode == "develop" ? comDevelop : comSimple;
}

std::string GUI_App::get_mode_str()
{
    if (!app_config->has("user_mode"))
        return "simple";
    return app_config->get("user_mode");
}

void GUI_App::save_mode(const /*ConfigOptionMode*/int mode)
{
    //BBS
    const std::string mode_str = mode == comAdvanced ? "advanced" :
                                 mode == comSimple ? "simple" :
                                 mode == comDevelop ? "develop" : "simple";
    app_config->set("user_mode", mode_str);
    update_mode();
}

// Update view mode according to selected menu
void GUI_App::update_mode()
{
    sidebar().update_mode();

    //BBS: GUI refactor
    if (mainframe->m_param_panel)
        mainframe->m_param_panel->update_mode();
    if (mainframe->m_param_dialog)
        mainframe->m_param_dialog->panel()->update_mode();
    if (mainframe->m_printer_view)
        mainframe->m_printer_view->update_mode();
    mainframe->m_webview->update_mode();

#ifdef _MSW_DARK_MODE
    if (!wxGetApp().tabs_as_menu())
        dynamic_cast<Notebook*>(mainframe->m_tabpanel)->UpdateMode();
#endif

    for (auto tab : tabs_list)
        tab->update_mode();
    for (auto tab : model_tabs_list)
        tab->update_mode();

    //BBS plater()->update_menus();

    plater()->canvas3D()->update_gizmos_on_off_state();
}

void GUI_App::update_internal_development() {
    mainframe->m_webview->update_mode();
    if (mainframe->m_printer_view)
        mainframe->m_printer_view->update_mode();
}

void GUI_App::show_ip_address_enter_dialog(wxString title)
{
    auto evt = new wxCommandEvent(EVT_SHOW_IP_DIALOG);
    evt->SetString(title);
    evt->SetInt(-1);
    wxQueueEvent(this, evt);
}

bool GUI_App::show_modal_ip_address_enter_dialog(bool input_sn, wxString title)
{
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return false;
    if (!dev->get_selected_machine()) return false;
    auto obj = dev->get_selected_machine();

    InputIpAddressDialog dlg(nullptr);
    dlg.m_need_input_sn = input_sn;
    dlg.set_machine_obj(obj);
    if (!title.empty()) dlg.update_title(title);

    dlg.Bind(EVT_ENTER_IP_ADDRESS, [this, obj](wxCommandEvent& e) {
        auto selection_data_arr = wxSplit(e.GetString().ToStdString(), '|');

        if (selection_data_arr.size() == 2) {
            auto ip_address = selection_data_arr[0];
            auto access_code = selection_data_arr[1];

            BOOST_LOG_TRIVIAL(info) << "User enter IP address is " << ip_address;
            if (!ip_address.empty()) {
                wxGetApp().app_config->set_str("ip_address", obj->get_dev_id(), ip_address.ToStdString());
                wxGetApp().app_config->save();

                obj->set_dev_ip(ip_address.ToStdString());
                obj->set_user_access_code(access_code.ToStdString());
            }
        }
    });

    if (dlg.ShowModal() == wxID_YES) {
        return true;
    }
    return false;
}

void  GUI_App::show_ip_address_enter_dialog_handler(wxCommandEvent& evt)
{
    wxString title = evt.GetString();
    int mode = evt.GetInt();
    show_modal_ip_address_enter_dialog(mode == -1?false:true, title);
}

//void GUI_App::add_config_menu(wxMenuBar *menu)
//void GUI_App::add_config_menu(wxMenu *menu)
//{
//    auto local_menu = new wxMenu();
//    wxWindowID config_id_base = wxWindow::NewControlId(int(ConfigMenuCnt));
//
//    const auto config_wizard_name = _(ConfigWizard::name(true));
//    const auto config_wizard_tooltip = from_u8((boost::format(_utf8(L("Open %s"))) % config_wizard_name).str());
//    // Cmd+, is standard on OS X - what about other operating systems?
//    if (is_editor()) {
//        local_menu->Append(config_id_base + ConfigMenuWizard, config_wizard_name + dots, config_wizard_tooltip);
//        local_menu->Append(config_id_base + ConfigMenuUpdate, _L("Check for Configuration Updates"), _L("Check for configuration updates"));
//        local_menu->AppendSeparator();
//    }
//    local_menu->Append(config_id_base + ConfigMenuPreferences, _L("Preferences") + dots +
//#ifdef __APPLE__
//        "\tCtrl+,",
//#else
//        "\tCtrl+P",
//#endif
//        _L("Application preferences"));
//    wxMenu* mode_menu = nullptr;
//    if (is_editor()) {
//        local_menu->AppendSeparator();
//        mode_menu = new wxMenu();
//        mode_menu->AppendRadioItem(config_id_base + ConfigMenuModeSimple, _L("Simple"), _L("Simple Mode"));
//        mode_menu->AppendRadioItem(config_id_base + ConfigMenuModeAdvanced, _L("Advanced"), _L("Advanced Mode"));
//        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { if (get_mode() == comSimple) evt.Check(true); }, config_id_base + ConfigMenuModeSimple);
//        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { if (get_mode() == comAdvanced) evt.Check(true); }, config_id_base + ConfigMenuModeAdvanced);
//
//        local_menu->AppendSubMenu(mode_menu, _L("Mode"), wxString::Format(_L("%s Mode"), SLIC3R_APP_NAME));
//    }
//    local_menu->AppendSeparator();
//    local_menu->Append(config_id_base + ConfigMenuLanguage, _L("Language"));
//    if (is_editor()) {
//        local_menu->AppendSeparator();
//    }
//
//    local_menu->Bind(wxEVT_MENU, [this, config_id_base](wxEvent &event) {
//        switch (event.GetId() - config_id_base) {
//        case ConfigMenuWizard:
//            run_wizard(ConfigWizard::RR_USER);
//            break;
//		case ConfigMenuUpdate:
//			check_updates(true);
//			break;
//#ifdef __linux__
//        case ConfigMenuDesktopIntegration:
//            show_desktop_integration_dialog();
//            break;
//#endif
//        case ConfigMenuSnapshots:
//            //BBS do not support task snapshot
//            break;
//        case ConfigMenuPreferences:
//        {
//            //BBS GUI refactor: remove unuse layout logic
//            //bool app_layout_changed = false;
//            {
//                // the dialog needs to be destroyed before the call to recreate_GUI()
//                // or sometimes the application crashes into wxDialogBase() destructor
//                // so we put it into an inner scope
//                PreferencesDialog dlg(mainframe);
//                dlg.ShowModal();
//                //BBS GUI refactor: remove unuse layout logic
//                //app_layout_changed = dlg.settings_layout_changed();
//                if (dlg.seq_top_layer_only_changed())
//                    this->plater_->refresh_print();
//
//                if (dlg.recreate_GUI()) {
//                    recreate_GUI(_L("Restart application") + dots);
//                    return;
//                }
//#ifdef _WIN32
//                if (is_editor()) {
//                    if (app_config->get("associate_3mf") == "true")
//                        associate_3mf_files();
//                    if (app_config->get("associate_stl") == "true")
//                        associate_stl_files();
//                }
//                else {
//                    if (app_config->get("associate_gcode") == "true")
//                        associate_gcode_files();
//                }
//#endif // _WIN32
//            }
//            //BBS GUI refactor: remove unuse layout logic
//            /*if (app_layout_changed) {
//                // hide full main_sizer for mainFrame
//                mainframe->GetSizer()->Show(false);
//                mainframe->update_layout();
//                mainframe->select_tab(size_t(0));
//            }*/
//            break;
//        }
//        case ConfigMenuLanguage:
//        {
//            /* Before change application language, let's check unsaved changes on 3D-Scene
//             * and draw user's attention to the application restarting after a language change
//             */
//            {
//                // the dialog needs to be destroyed before the call to switch_language()
//                // or sometimes the application crashes into wxDialogBase() destructor
//                // so we put it into an inner scope
//                wxString title = is_editor() ? wxString(SLIC3R_APP_NAME) : wxString(GCODEVIEWER_APP_NAME);
//                title += " - " + _L("Choose language");
//                //wxMessageDialog dialog(nullptr,
//                MessageDialog dialog(nullptr,
//                    _L("Switching the language requires application restart.\n") + "\n\n" +
//                    _L("Do you want to continue?"),
//                    title,
//                    wxICON_QUESTION | wxOK | wxCANCEL);
//                if (dialog.ShowModal() == wxID_CANCEL)
//                    return;
//            }
//
//            switch_language();
//            break;
//        }
//        case ConfigMenuFlashFirmware:
//            //BBS FirmwareDialog::run(mainframe);
//            break;
//        default:
//            break;
//        }
//    });
//
//    using std::placeholders::_1;
//
//    if (mode_menu != nullptr) {
//        auto modfn = [this](int mode, wxCommandEvent&) { if (get_mode() != mode) save_mode(mode); };
//        mode_menu->Bind(wxEVT_MENU, std::bind(modfn, comSimple, _1), config_id_base + ConfigMenuModeSimple);
//        mode_menu->Bind(wxEVT_MENU, std::bind(modfn, comAdvanced, _1), config_id_base + ConfigMenuModeAdvanced);
//    }
//
//    // BBS
//    //menu->Append(local_menu, _L("Configuration"));
//    menu->AppendSubMenu(local_menu, _L("Configuration"));
//}

void GUI_App::open_preferences(size_t open_on_tab, const std::string& highlight_option)
{
    bool app_layout_changed = false;
    {
        // the dialog needs to be destroyed before the call to recreate_GUI()
        // or sometimes the application crashes into wxDialogBase() destructor
        // so we put it into an inner scope
        PreferencesDialog dlg(mainframe, open_on_tab, highlight_option);
        dlg.ShowModal();
        this->plater_->get_current_canvas3D()->force_set_focus();
        // BBS
        //app_layout_changed = dlg.settings_layout_changed();
#if ENABLE_GCODE_LINES_ID_IN_H_SLIDER
        if (dlg.seq_top_layer_only_changed() || dlg.seq_seq_top_gcode_indices_changed())
#else
        if (dlg.seq_top_layer_only_changed())
#endif // ENABLE_GCODE_LINES_ID_IN_H_SLIDER
            this->plater_->refresh_print();
#ifdef _WIN32
        if (is_editor()) {
            if (app_config->get("associate_3mf") == "true")
                associate_files(L"3mf");
            if (app_config->get("associate_stl") == "true")
                associate_files(L"stl");
            if (app_config->get("associate_step") == "true") {
                associate_files(L"step");
                associate_files(L"stp");
            }
            associate_url(L"orcaslicer");
        }
        else {
            if (app_config->get("associate_gcode") == "true")
                associate_files(L"gcode");
        }
#endif // _WIN32
    }

    // BBS
    /*
    if (app_layout_changed) {
        // hide full main_sizer for mainFrame
        mainframe->GetSizer()->Show(false);
        mainframe->update_layout();
        mainframe->select_tab(size_t(0));
    }*/
}

bool GUI_App::has_unsaved_preset_changes() const
{
    PrinterTechnology printer_technology = preset_bundle->printers.get_edited_preset().printer_technology();
    for (const Tab* const tab : tabs_list) {
        if (tab->supports_printer_technology(printer_technology) && tab->saved_preset_is_dirty())
            return true;
    }
    return false;
}

bool GUI_App::has_current_preset_changes() const
{
    PrinterTechnology printer_technology = preset_bundle->printers.get_edited_preset().printer_technology();
    for (const Tab* const tab : tabs_list) {
        if (tab->supports_printer_technology(printer_technology) && tab->current_preset_is_dirty())
            return true;
    }
    return false;
}

void GUI_App::update_saved_preset_from_current_preset()
{
    PrinterTechnology printer_technology = preset_bundle->printers.get_edited_preset().printer_technology();
    for (Tab* tab : tabs_list) {
        if (tab->supports_printer_technology(printer_technology))
            tab->update_saved_preset_from_current_preset();
    }
}

std::vector<std::pair<unsigned int, std::string>> GUI_App::get_selected_presets() const
{
    std::vector<std::pair<unsigned int, std::string>> ret;
    PrinterTechnology printer_technology = preset_bundle->printers.get_edited_preset().printer_technology();
    for (Tab* tab : tabs_list) {
        if (tab->supports_printer_technology(printer_technology)) {
            const PresetCollection* presets = tab->get_presets();
            ret.push_back({ static_cast<unsigned int>(presets->type()), presets->get_selected_preset_name() });
        }
    }
    return ret;
}

// To notify the user whether he is aware that some preset changes will be lost,
// UnsavedChangesDialog: "Discard / Save / Cancel"
// This is called when:
// - Close Application & Current project isn't saved
// - Load Project      & Current project isn't saved
// - Undo / Redo with change of print technologie
// - Loading snapshot
// - Loading config_file/bundle
// UnsavedChangesDialog: "Don't save / Save / Cancel"
// This is called when:
// - Exporting config_bundle
// - Taking snapshot
bool GUI_App::check_and_save_current_preset_changes(const wxString& caption, const wxString& header, bool remember_choice/* = true*/, bool dont_save_insted_of_discard/* = false*/)
{
    if (has_current_preset_changes()) {
        int act_buttons = ActionButtons::SAVE;
        if (dont_save_insted_of_discard)
            act_buttons |= ActionButtons::DONT_SAVE;
        if (remember_choice)
            act_buttons |= ActionButtons::REMEMBER_CHOISE;
        UnsavedChangesDialog dlg(caption, header, "", act_buttons);
        bool no_need_change = dlg.getUpdateItemCount() == 0 ? true : false;
        if (!no_need_change && dlg.ShowModal() == wxID_CANCEL)
            return false;

        if (dlg.save_preset())  // save selected changes
        {
            //BBS: add project embedded preset relate logic
            for (const UnsavedChangesDialog::PresetData& nt : dlg.get_names_and_types())
                preset_bundle->save_changes_for_preset(nt.name, nt.type, dlg.get_unselected_options(nt.type), nt.save_to_project);
            //for (const std::pair<std::string, Preset::Type>& nt : dlg.get_names_and_types())
            //    preset_bundle->save_changes_for_preset(nt.first, nt.second, dlg.get_unselected_options(nt.second));

            load_current_presets(false);

            // if we saved changes to the new presets, we should to
            // synchronize config.ini with the current selections.
            preset_bundle->export_selections(*app_config);

            //MessageDialog(nullptr, _L_PLURAL("Modifications to the preset have been saved",
            //                                 "Modifications to the presets have been saved", dlg.get_names_and_types().size())).ShowModal();
        }
    }

    return true;
}

void GUI_App::apply_keeped_preset_modifications()
{
    PrinterTechnology printer_technology = preset_bundle->printers.get_edited_preset().printer_technology();
    for (Tab* tab : tabs_list) {
        if (tab->supports_printer_technology(printer_technology))
            tab->apply_config_from_cache();
    }
    load_current_presets(false);
}

// This is called when creating new project or load another project
// OR close ConfigWizard
// to ask the user what should we do with unsaved changes for presets.
// New Project          => Current project is saved    => UnsavedChangesDialog: "Keep / Discard / Cancel"
//                      => Current project isn't saved => UnsavedChangesDialog: "Keep / Discard / Save / Cancel"
// Close ConfigWizard   => Current project is saved    => UnsavedChangesDialog: "Keep / Discard / Save / Cancel"
// Note: no_nullptr postponed_apply_of_keeped_changes indicates that thie function is called after ConfigWizard is closed
bool GUI_App::check_and_keep_current_preset_changes(const wxString& caption, const wxString& header, int action_buttons, bool* postponed_apply_of_keeped_changes/* = nullptr*/)
{
    if (has_current_preset_changes()) {
        bool is_called_from_configwizard = postponed_apply_of_keeped_changes != nullptr;

        UnsavedChangesDialog dlg(caption, header, "", action_buttons);
        bool no_need_change = dlg.getUpdateItemCount() == 0 ? true : false;
        if (!no_need_change && dlg.ShowModal() == wxID_CANCEL)
            return false;

        auto reset_modifications = [this, is_called_from_configwizard]() {
            //if (is_called_from_configwizard)
            //    return; // no need to discared changes. It will be done fromConfigWizard closing

            PrinterTechnology printer_technology = preset_bundle->printers.get_edited_preset().printer_technology();
            for (const Tab* const tab : tabs_list) {
                if (tab->supports_printer_technology(printer_technology) && tab->current_preset_is_dirty())
                    tab->m_presets->discard_current_changes();
            }
            load_current_presets(false);
        };

        if (dlg.discard() || no_need_change)
            reset_modifications();
        else  // save selected changes
        {
            //BBS: add project embedded preset relate logic
            const auto& preset_names_and_types = dlg.get_names_and_types();
            if (dlg.save_preset()) {
                for (const UnsavedChangesDialog::PresetData& nt : preset_names_and_types)
                    preset_bundle->save_changes_for_preset(nt.name, nt.type, dlg.get_unselected_options(nt.type), nt.save_to_project);

                // if we saved changes to the new presets, we should to
                // synchronize config.ini with the current selections.
                preset_bundle->export_selections(*app_config);

                //wxString text = _L_PLURAL("Modifications to the preset have been saved",
                //    "Modifications to the presets have been saved", preset_names_and_types.size());
                //if (!is_called_from_configwizard)
                //    text += "\n\n" + _L("All modifications will be discarded for new project.");

                //MessageDialog(nullptr, text).ShowModal();
                reset_modifications();
            }
            else if (dlg.transfer_changes() && (dlg.has_unselected_options() || is_called_from_configwizard)) {
                // execute this part of code only if not all modifications are keeping to the new project
                // OR this function is called when ConfigWizard is closed and "Keep modifications" is selected
                for (const UnsavedChangesDialog::PresetData& nt : preset_names_and_types) {
                    Preset::Type type = nt.type;
                    Tab* tab = get_tab(type);
                    std::vector<std::string> selected_options = dlg.get_selected_options(type);
                    if (type == Preset::TYPE_PRINTER) {
                        auto it = std::find(selected_options.begin(), selected_options.end(), "extruders_count");
                        if (it != selected_options.end()) {
                            // erase "extruders_count" option from the list
                            selected_options.erase(it);
                            // cache the extruders count
                            static_cast<TabPrinter*>(tab)->cache_extruder_cnt();
                        }
                    }
                    std::vector<std::string> selected_options2;
                    std::transform(selected_options.begin(), selected_options.end(), std::back_inserter(selected_options2), [](auto & o) {
                        auto i = o.find('#');
                        return i != std::string::npos ? o.substr(0, i) : o;
                    });
                    tab->cache_config_diff(selected_options2);
                    if (!is_called_from_configwizard)
                        tab->m_presets->discard_current_changes();
                }
                if (is_called_from_configwizard)
                    *postponed_apply_of_keeped_changes = true;
                else
                    apply_keeped_preset_modifications();
            }
        }
    }

    return true;
}

bool GUI_App::can_load_project()
{
    return true;
}

bool GUI_App::check_print_host_queue()
{
    wxString dirty;
    std::vector<std::pair<std::string, std::string>> jobs;
    // Get ongoing jobs from dialog
    mainframe->m_printhost_queue_dlg->get_active_jobs(jobs);
    if (jobs.empty())
        return true;
    // Show dialog
    wxString job_string = wxString();
    for (const auto& job : jobs) {
        job_string += format_wxstr("   %1% : %2% \n", job.first, job.second);
    }
    wxString message;
    message += _(L("The uploads are still ongoing")) + ":\n\n" + job_string +"\n" + _(L("Stop them and continue anyway?"));
    //wxMessageDialog dialog(mainframe,
    MessageDialog dialog(mainframe,
        message,
        wxString(SLIC3R_APP_NAME) + " - " + _(L("Ongoing uploads")),
        wxICON_QUESTION | wxYES_NO | wxNO_DEFAULT);
    if (dialog.ShowModal() == wxID_YES)
        return true;

    // TODO: If already shown, bring forward
    mainframe->m_printhost_queue_dlg->Show();
    return false;
}

bool GUI_App::checked_tab(Tab* tab)
{
    bool ret = true;
    if (find(tabs_list.begin(), tabs_list.end(), tab) == tabs_list.end() &&
        find(model_tabs_list.begin(), model_tabs_list.end(), tab) == model_tabs_list.end())
        ret = false;
    return ret;
}

// Update UI / Tabs to reflect changes in the currently loaded presets
//BBS: add preset combo box re-activate logic
void GUI_App::load_current_presets(bool active_preset_combox/*= false*/, bool check_printer_presets_ /*= true*/)
{
    // check printer_presets for the containing information about "Print Host upload"
    // and create physical printer from it, if any exists
    if (check_printer_presets_)
        check_printer_presets();

    PrinterTechnology printer_technology = preset_bundle->printers.get_edited_preset().printer_technology();
	this->plater()->set_printer_technology(printer_technology);
    for (Tab *tab : tabs_list)
		if (tab->supports_printer_technology(printer_technology)) {
			if (tab->type() == Preset::TYPE_PRINTER) {
				static_cast<TabPrinter*>(tab)->update_pages();
				// Mark the plater to update print bed by tab->load_current_preset() from Plater::on_config_change().
				this->plater()->force_print_bed_update();
			}
			tab->load_current_preset();
			//BBS: add preset combox re-active logic
			if (active_preset_combox)
				tab->reactive_preset_combo_box();
		}
    // BBS: model config
    for (Tab *tab : model_tabs_list)
		if (tab->supports_printer_technology(printer_technology)) {
            tab->rebuild_page_tree();
        }
}

static std::mutex mutex_delete_cache_presets;

std::vector<std::string> & GUI_App::get_delete_cache_presets()
{
    return need_delete_presets;
}

std::vector<std::string> GUI_App::get_delete_cache_presets_lock()
{
    std::scoped_lock l(mutex_delete_cache_presets);
    return need_delete_presets;
}

void GUI_App::delete_preset_from_cloud(std::string setting_id)
{
    std::scoped_lock l(mutex_delete_cache_presets);
    need_delete_presets.push_back(setting_id);
}

void GUI_App::preset_deleted_from_cloud(std::string setting_id)
{
    std::scoped_lock l(mutex_delete_cache_presets);
    need_delete_presets.erase(std::remove(need_delete_presets.begin(), need_delete_presets.end(), setting_id), need_delete_presets.end());
}

wxString GUI_App::filter_string(wxString str)
{
    std::string result = str.utf8_string();
    std::string input = str.utf8_string();


    std::regex domainRegex(R"(([a-zA-Z0-9.-]+\.[a-zA-Z]{2,}(?:\.[a-zA-Z]{2,})?))");
    std::sregex_iterator it(input.begin(), input.end(), domainRegex);
    std::sregex_iterator end;

    while (it != end) {
        std::smatch match = *it;
        std::string domain = match.str();
        result.replace(match.position(), domain.length(), "[***]");
        ++it;
    }

    return wxString::FromUTF8(result);
}

bool GUI_App::OnExceptionInMainLoop()
{
    generic_exception_handle();
    return false;
}

#ifdef __APPLE__
// This callback is called from wxEntry()->wxApp::CallOnInit()->NSApplication run
// that is, before GUI_App::OnInit(), so we have a chance to switch GUI_App
// to a G-code viewer.
void GUI_App::OSXStoreOpenFiles(const wxArrayString &fileNames)
{
    //BBS: remove GCodeViewer as seperate APP logic
    /*size_t num_gcodes = 0;
    for (const wxString &filename : fileNames)
        if (is_gcode_file(into_u8(filename)))
            ++ num_gcodes;
    if (fileNames.size() == num_gcodes) {
        // Opening PrusaSlicer by drag & dropping a G-Code onto OrcaSlicer icon in Finder,
        // just G-codes were passed. Switch to G-code viewer mode.
        m_app_mode = EAppMode::GCodeViewer;
        unlock_lockfile(get_instance_hash_string() + ".lock", data_dir() + "/cache/");
        if(app_config != nullptr)
            delete app_config;
        app_config = nullptr;
        init_app_config();
    }*/
    wxApp::OSXStoreOpenFiles(fileNames);
}

void GUI_App::MacOpenURL(const wxString& url)
{
    if (url.empty())
        return;
    start_download(into_u8(url));
}

// wxWidgets override to get an event on open files.
void GUI_App::MacOpenFiles(const wxArrayString &fileNames)
{
    bool single_instance = app_config->get("app", "single_instance") == "true";
    if (m_post_initialized && !single_instance) {
        bool has3mf = false;
        std::vector<wxString> names;
        for (auto & n : fileNames) {
            has3mf |= n.EndsWith(".3mf");
            names.push_back(n);
        }
        if (has3mf) {
            start_new_slicer(names);
            return;
        }
    }
    std::vector<std::string> files;
    std::vector<wxString>    gcode_files;
    std::vector<wxString>    non_gcode_files;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", open files, size " << fileNames.size();
    for (const auto& filename : fileNames) {
        if (is_gcode_file(into_u8(filename)))
            gcode_files.emplace_back(filename);
        else {
            files.emplace_back(into_u8(filename));
            non_gcode_files.emplace_back(filename);
        }
    }
    //BBS: remove GCodeViewer as seperate APP logic
    /*if (m_app_mode == EAppMode::GCodeViewer) {
        // Running in G-code viewer.
        // Load the first G-code into the G-code viewer.
        // Or if no G-codes, send other files to slicer.
        if (! gcode_files.empty())
            this->plater()->load_gcode(gcode_files.front());
        if (!non_gcode_files.empty())
            start_new_slicer(non_gcode_files, true);
    } else*/
    {
        if (! files.empty()) {
            if (m_post_initialized) {
                wxArrayString input_files;
                for (size_t i = 0; i < non_gcode_files.size(); ++i) {
                    input_files.push_back(non_gcode_files[i]);
                }
                this->plater()->load_files(input_files);
            } else {
                for (size_t i = 0; i < files.size(); ++i) {
                    this->init_params->input_files.emplace_back(files[i]);
                }
            }
        } else {
            if (m_post_initialized) {
                this->plater()->load_gcode(gcode_files.front());
            } else {
                this->init_params->input_gcode = true;
                this->init_params->input_files = { into_u8(gcode_files.front()) };
            }
        }
        /*for (const wxString &filename : gcode_files)
            start_new_gcodeviewer(&filename);*/
    }
}

#endif /* __APPLE */

Sidebar& GUI_App::sidebar()
{
    return plater_->sidebar();
}

GizmoObjectManipulation *GUI_App::obj_manipul()
{
    // If this method is called before plater_ has been initialized, return nullptr (to avoid a crash)
    return (plater_ != nullptr) ? &plater_->get_view3D_canvas3D()->get_gizmos_manager().get_object_manipulation() : nullptr;
}

ObjectSettings* GUI_App::obj_settings()
{
    return sidebar().obj_settings();
}

ObjectList* GUI_App::obj_list()
{
    return sidebar().obj_list();
}

ObjectLayers* GUI_App::obj_layers()
{
    return sidebar().obj_layers();
}

Plater* GUI_App::plater()
{
    return plater_;
}

const Plater* GUI_App::plater() const
{
    return plater_;
}

ParamsPanel* GUI_App::params_panel()
{
    if (mainframe)
        return mainframe->m_param_panel;
    return nullptr;
}

ParamsDialog* GUI_App::params_dialog()
{
    if (mainframe)
        return mainframe->m_param_dialog;
    return nullptr;
}

Model& GUI_App::model()
{
    return plater_->model();
}

Downloader* GUI_App::downloader()
{
    return m_downloader.get();
}

void GUI_App::load_url(wxString url)
{
    if (mainframe)
        return mainframe->load_url(url);
}

void GUI_App::open_mall_page_dialog()
{
    std::string host_url;
    std::string model_url;
    std::string link_url;

    int result = -1;

    //model api url
    host_url = get_model_http_url(app_config->get_country_code());

    //model url

    wxString language_code = this->current_language_code().BeforeFirst('_');
    model_url = language_code.ToStdString();

    if (getAgent() && mainframe) {

        //login already
        if (getAgent()->is_user_login()) {
            std::string ticket;
            result = getAgent()->request_bind_ticket(&ticket);

            if(result == 0){
                link_url = host_url + "api/sign-in/ticket?to=" + host_url + url_encode(model_url) + "&ticket=" + ticket;
            }
        }
    }

    if (result < 0) {
       link_url = host_url + model_url;
    }

    if (link_url.find("?") != std::string::npos) {
        link_url += "&from=orcaslicer";
    } else {
        link_url += "?from=orcaslicer";
    }

    wxLaunchDefaultBrowser(link_url);
}

void GUI_App::open_publish_page_dialog()
{
    std::string host_url;
    std::string model_url;
    std::string link_url;

    int result = -1;

    //model api url
    host_url = get_model_http_url(app_config->get_country_code());

    //publish url
    wxString language_code = this->current_language_code().BeforeFirst('_');
    model_url += (language_code.ToStdString() + "/my/models/publish");

    if (getAgent() && mainframe) {

        //login already
        if (getAgent()->is_user_login()) {
            std::string ticket;
            result = getAgent()->request_bind_ticket(&ticket);

            if (result == 0) {
                link_url = host_url + "api/sign-in/ticket?to=" + host_url + url_encode(model_url) + "&ticket=" + ticket;
            }
        }
    }

    if (result < 0) {
        link_url = host_url + model_url;
    }

    wxLaunchDefaultBrowser(link_url);
}

char GUI_App::from_hex(char ch) {
    return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

std::string GUI_App::url_decode(std::string value) {
    return Http::url_decode(value);
}

std::string GUI_App::url_encode(std::string value) {
    return Http::url_encode(value);
}

void GUI_App::popup_ping_bind_dialog()
{
    if (m_ping_code_binding_dialog == nullptr) {
        m_ping_code_binding_dialog = new PingCodeBindDialog();
        m_ping_code_binding_dialog->ShowModal();
        remove_ping_bind_dialog();
    }
}

void GUI_App::remove_ping_bind_dialog()
{
    if (m_ping_code_binding_dialog != nullptr) {
        m_ping_code_binding_dialog->Destroy();
        delete m_mall_publish_dialog;
        m_ping_code_binding_dialog = nullptr;
    }
}


void GUI_App::remove_mall_system_dialog()
{
    if (m_mall_publish_dialog != nullptr) {
        m_mall_publish_dialog->Destroy();
        delete m_mall_publish_dialog;
    }
}

void GUI_App::run_script(wxString js)
{
    if (mainframe)
        return mainframe->RunScript(js);
}

Notebook* GUI_App::tab_panel() const
{
    if (mainframe)
        return mainframe->m_tabpanel;
    return nullptr;
}

NotificationManager * GUI_App::notification_manager()
{
    if (plater_)
        return plater_->get_notification_manager();
    return nullptr;
}

// extruders count from selected printer preset
int GUI_App::extruders_cnt() const
{
    const Preset& preset = preset_bundle->printers.get_selected_preset();
    return preset.printer_technology() == ptSLA ? 1 :
           preset.config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
}

// extruders count from edited printer preset
int GUI_App::extruders_edited_cnt() const
{
    const Preset& preset = preset_bundle->printers.get_edited_preset();
    return preset.printer_technology() == ptSLA ? 1 :
           preset.config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
}

// BBS
int GUI_App::filaments_cnt() const
{
    return preset_bundle->filament_presets.size();
}

PrintSequence GUI_App::global_print_sequence() const
{
    PrintSequence global_print_seq = PrintSequence::ByDefault;
    auto curr_preset_config = preset_bundle->prints.get_edited_preset().config;
    if (curr_preset_config.has("print_sequence"))
        global_print_seq = curr_preset_config.option<ConfigOptionEnum<PrintSequence>>("print_sequence")->value;
    return global_print_seq;
}

wxString GUI_App::current_language_code_safe() const
{
	// Translate the language code to a code, for which Prusa Research maintains translations.
	const std::map<wxString, wxString> mapping {
		{ "cs", 	"cs_CZ", },
		{ "sk", 	"cs_CZ", },
		{ "de", 	"de_DE", },
		{ "nl", 	"nl_NL", },
		{ "sv", 	"sv_SE", },
		{ "es", 	"es_ES", },
		{ "fr", 	"fr_FR", },
		{ "it", 	"it_IT", },
		{ "ja", 	"ja_JP", },
		{ "ko", 	"ko_KR", },
		{ "pl", 	"pl_PL", },
		{ "uk", 	"uk_UA", },
		{ "zh", 	"zh_CN", },
		{ "ru", 	"ru_RU", },
        { "tr", 	"tr_TR", },
        { "pt", 	"pt_BR", },
        { "lt", 	"lt_LT", },
        { "vi", 	"vi_VN", },
	};
	wxString language_code = this->current_language_code().BeforeFirst('_');
	auto it = mapping.find(language_code);
	if (it != mapping.end())
		language_code = it->second;
	else
		language_code = "en_US";
	return language_code;
}

void GUI_App::open_web_page_localized(const std::string &http_address)
{
    open_browser_with_warning_dialog(http_address + "&lng=" + this->current_language_code_safe());
}

// If we are switching from the FFF-preset to the SLA, we should to control the printed objects if they have a part(s).
// Because of we can't to print the multi-part objects with SLA technology.
bool GUI_App::may_switch_to_SLA_preset(const wxString& caption)
{
    if (model_has_multi_part_objects(model())) {
        // BBS: remove SLA related message
        return false;
    }
    return true;
}

bool GUI_App::run_wizard(ConfigWizard::RunReason reason, ConfigWizard::StartPage start_page)
{
    wxCHECK_MSG(mainframe != nullptr, false, "Internal error: Main frame not created / null");

#ifdef __APPLE__
     if (is_adding_script_handler()) {
        BOOST_LOG_TRIVIAL(info) << "run_wizard: Script handler is being added, delaying wizard creation";
        auto timer = new wxTimer();
        timer->Bind(wxEVT_TIMER, [this, reason, start_page, timer](wxTimerEvent &) {
            timer->Stop();
            run_wizard(reason, start_page);
            delete timer;
        });
        timer->StartOnce(200);

        return true;
    }
#endif

    //if (reason == ConfigWizard::RR_USER) {
    //    //TODO: turn off it currently, maybe need to turn on in the future
    //    if (preset_updater->config_update(app_config->orig_version(), PresetUpdater::UpdateParams::FORCED_BEFORE_WIZARD) == PresetUpdater::R_ALL_CANCELED)
    //        return false;
    //}

    //auto wizard_t = new ConfigWizard(mainframe);
    //const bool res = wizard_t->run(reason, start_page);

    std::string strFinish = wxGetApp().app_config->get("firstguide", "finish");
    long        pStyle    = wxCAPTION | wxCLOSE_BOX | wxSYSTEM_MENU;
    if (strFinish == "false" || strFinish.empty())
        pStyle = wxCAPTION | wxTAB_TRAVERSAL;

    GuideFrame wizard(this, pStyle);
    auto page = start_page == ConfigWizard::SP_WELCOME ? GuideFrame::BBL_WELCOME :
                start_page == ConfigWizard::SP_FILAMENTS ? GuideFrame::BBL_FILAMENT_ONLY :
                start_page == ConfigWizard::SP_PRINTERS ? GuideFrame::BBL_MODELS_ONLY :
                GuideFrame::BBL_MODELS;
    wizard.SetStartPage(page);
    bool       res = wizard.run();

    if (res) {
        load_current_presets();
        update_publish_status();
        mainframe->refresh_plugin_tips();
        // BBS: remove SLA related message
    }

    return res;
}

void GUI_App::show_desktop_integration_dialog()
{
#ifdef __linux__
    //wxCHECK_MSG(mainframe != nullptr, false, "Internal error: Main frame not created / null");
    DesktopIntegrationDialog dialog(mainframe);
    dialog.ShowModal();
#endif //__linux__
}

#if ENABLE_THUMBNAIL_GENERATOR_DEBUG
void GUI_App::gcode_thumbnails_debug()
{
    const std::string BEGIN_MASK = "; thumbnail begin";
    const std::string END_MASK = "; thumbnail end";
    std::string gcode_line;
    bool reading_image = false;
    unsigned int width = 0;
    unsigned int height = 0;

    wxFileDialog dialog(GetTopWindow(), _L("Select a G-code file:"), "", "", "G-code files (*.gcode)|*.gcode;*.GCODE;", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;

    std::string in_filename = into_u8(dialog.GetPath());
    std::string out_path = boost::filesystem::path(in_filename).remove_filename().append(L"thumbnail").string();

    boost::nowide::ifstream in_file(in_filename.c_str());
    std::vector<std::string> rows;
    std::string row;
    if (in_file.good())
    {
        while (std::getline(in_file, gcode_line))
        {
            if (in_file.good())
            {
                if (boost::starts_with(gcode_line, BEGIN_MASK))
                {
                    reading_image = true;
                    gcode_line = gcode_line.substr(BEGIN_MASK.length() + 1);
                    std::string::size_type x_pos = gcode_line.find('x');
                    std::string width_str = gcode_line.substr(0, x_pos);
                    width = (unsigned int)::atoi(width_str.c_str());
                    std::string height_str = gcode_line.substr(x_pos + 1);
                    height = (unsigned int)::atoi(height_str.c_str());
                    row.clear();
                }
                else if (reading_image && boost::starts_with(gcode_line, END_MASK))
                {
                    std::string out_filename = out_path + std::to_string(width) + "x" + std::to_string(height) + ".png";
                    boost::nowide::ofstream out_file(out_filename.c_str(), std::ios::binary);
                    if (out_file.good())
                    {
                        std::string decoded;
                        decoded.resize(boost::beast::detail::base64::decoded_size(row.size()));
                        decoded.resize(boost::beast::detail::base64::decode((void*)&decoded[0], row.data(), row.size()).first);

                        out_file.write(decoded.c_str(), decoded.size());
                        out_file.close();
                    }

                    reading_image = false;
                    width = 0;
                    height = 0;
                    rows.clear();
                } else if (reading_image)
                    row += gcode_line.substr(2);
            }
        }

        in_file.close();
    }
}
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG

void GUI_App::window_pos_save(wxTopLevelWindow* window, const std::string &name)
{
    if (name.empty()) { return; }
    const auto config_key = (boost::format("window_%1%") % name).str();

    WindowMetrics metrics = WindowMetrics::from_window(window);
    app_config->set(config_key, metrics.serialize());
    app_config->save();
}

bool GUI_App::window_pos_restore(wxTopLevelWindow* window, const std::string &name, bool default_maximized)
{
    if (name.empty()) { return false; }
    const auto config_key = (boost::format("window_%1%") % name).str();

    if (! app_config->has(config_key)) {
        //window->Maximize(default_maximized);
        return false;
    }

    auto metrics = WindowMetrics::deserialize(app_config->get(config_key));
    if (! metrics) {
        window->Maximize(default_maximized);
        return true;
    }

    const wxRect& rect = metrics->get_rect();
    window->SetPosition(rect.GetPosition());
    window->SetSize(rect.GetSize());
    window->Maximize(metrics->get_maximized());
    return true;
}

void GUI_App::window_pos_sanitize(wxTopLevelWindow* window)
{
    /*unsigned*/int display_idx = wxDisplay::GetFromWindow(window);
    wxRect display;
    if (display_idx == wxNOT_FOUND) {
        display = wxDisplay(0u).GetClientArea();
        window->Move(display.GetTopLeft());
    } else {
        display = wxDisplay(display_idx).GetClientArea();
    }

    auto metrics = WindowMetrics::from_window(window);
    metrics.sanitize_for_display(display);
    if (window->GetScreenRect() != metrics.get_rect()) {
        window->SetSize(metrics.get_rect());
    }
}

void GUI_App::window_pos_center(wxTopLevelWindow *window)
{
    /*unsigned*/int display_idx = wxDisplay::GetFromWindow(window);
    wxRect display;
    if (display_idx == wxNOT_FOUND) {
        display = wxDisplay(0u).GetClientArea();
        window->Move(display.GetTopLeft());
    } else {
        display = wxDisplay(display_idx).GetClientArea();
    }

    auto metrics = WindowMetrics::from_window(window);
    metrics.center_for_display(display);
    if (window->GetScreenRect() != metrics.get_rect()) {
        window->SetSize(metrics.get_rect());
    }
}

bool GUI_App::config_wizard_startup()
{
    if (!m_app_conf_exists || preset_bundle->printers.only_default_printers()) {
        BOOST_LOG_TRIVIAL(info) << "run wizard...";
        run_wizard(ConfigWizard::RR_DATA_EMPTY);
        BOOST_LOG_TRIVIAL(info) << "finished run wizard";
        return true;
    } /*else if (get_app_config()->legacy_datadir()) {
        // Looks like user has legacy pre-vendorbundle data directory,
        // explain what this is and run the wizard

        MsgDataLegacy dlg;
        dlg.ShowModal();

        run_wizard(ConfigWizard::RR_DATA_LEGACY);
        return true;
    }*/
    return false;
}

void GUI_App::check_updates(const bool verbose)
{
	PresetUpdater::UpdateResult updater_result;
	try {
		updater_result = preset_updater->config_update(app_config->orig_version(), verbose ? PresetUpdater::UpdateParams::SHOW_TEXT_BOX : PresetUpdater::UpdateParams::SHOW_NOTIFICATION);
		if (updater_result == PresetUpdater::R_INCOMPAT_EXIT) {
			mainframe->Close();
		}
		else if (updater_result == PresetUpdater::R_INCOMPAT_CONFIGURED) {
            m_app_conf_exists = true;
		}
		else if (verbose && updater_result == PresetUpdater::R_NOOP) {
			MsgNoUpdates dlg;
			dlg.ShowModal();
		}
	}
	catch (const std::exception & ex) {
		show_error(nullptr, ex.what());
	}
}


FilamentColorCodeQuery* GUI_App::get_filament_color_code_query()
{
    if (!m_filament_color_code_query)
    {
        m_filament_color_code_query = new FilamentColorCodeQuery();
    }

    return m_filament_color_code_query;
}

bool GUI_App::open_browser_with_warning_dialog(const wxString& url, int flags/* = 0*/)
{
    return wxLaunchDefaultBrowser(url, flags);
}

// static method accepting a wxWindow object as first parameter
// void warning_catcher{
//     my($self, $message_dialog) = @_;
//     return sub{
//         my $message = shift;
//         return if $message = ~/ GLUquadricObjPtr | Attempt to free unreferenced scalar / ;
//         my @params = ($message, 'Warning', wxOK | wxICON_WARNING);
//         $message_dialog
//             ? $message_dialog->(@params)
//             : Wx::MessageDialog->new($self, @params)->ShowModal;
//     };
// }

// Do we need this function???
// void GUI_App::notify(message) {
//     auto frame = GetTopWindow();
//     // try harder to attract user attention on OS X
//     if (!frame->IsActive())
//         frame->RequestUserAttention(defined(__WXOSX__/*&Wx::wxMAC */)? wxUSER_ATTENTION_ERROR : wxUSER_ATTENTION_INFO);
//
//     // There used to be notifier using a Growl application for OSX, but Growl is dead.
//     // The notifier also supported the Linux X D - bus notifications, but that support was broken.
//     //TODO use wxNotificationMessage ?
// }


#ifdef __WXMSW__
static bool set_into_win_registry(HKEY hkeyHive, const wchar_t* pszVar, const wchar_t* pszValue)
{
    // see as reference: https://stackoverflow.com/questions/20245262/c-program-needs-an-file-association
    wchar_t szValueCurrent[1000];
    DWORD dwType;
    DWORD dwSize = sizeof(szValueCurrent);

    int iRC = ::RegGetValueW(hkeyHive, pszVar, nullptr, RRF_RT_ANY, &dwType, szValueCurrent, &dwSize);

    bool bDidntExist = iRC == ERROR_FILE_NOT_FOUND;

    if ((iRC != ERROR_SUCCESS) && !bDidntExist)
        // an error occurred
        return false;

    if (!bDidntExist) {
        if (dwType != REG_SZ)
            // invalid type
            return false;

        if (::wcscmp(szValueCurrent, pszValue) == 0)
            // value already set
            return false;
    }

    DWORD dwDisposition;
    HKEY hkey;
    iRC = ::RegCreateKeyExW(hkeyHive, pszVar, 0, 0, 0, KEY_ALL_ACCESS, nullptr, &hkey, &dwDisposition);
    bool ret = false;
    if (iRC == ERROR_SUCCESS) {
        iRC = ::RegSetValueExW(hkey, L"", 0, REG_SZ, (BYTE*)pszValue, (::wcslen(pszValue) + 1) * sizeof(wchar_t));
        if (iRC == ERROR_SUCCESS)
            ret = true;
    }

    RegCloseKey(hkey);
    return ret;
}

static bool del_win_registry(HKEY hkeyHive, const wchar_t *pszVar, const wchar_t *pszValue)
{
    wchar_t szValueCurrent[1000];
    DWORD   dwType;
    DWORD   dwSize = sizeof(szValueCurrent);

    int iRC = ::RegGetValueW(hkeyHive, pszVar, nullptr, RRF_RT_ANY, &dwType, szValueCurrent, &dwSize);

    bool bDidntExist = iRC == ERROR_FILE_NOT_FOUND;

    if ((iRC != ERROR_SUCCESS) && !bDidntExist)
        return false;

    if (!bDidntExist) {
        iRC      = ::RegDeleteKeyExW(hkeyHive, pszVar, KEY_ALL_ACCESS, 0);
        if (iRC == ERROR_SUCCESS) {
            return true;
        }
    }

    return false;
}

#endif // __WXMSW__

void GUI_App::associate_files(std::wstring extend)
{
#ifdef WIN32
    wchar_t app_path[MAX_PATH];
    ::GetModuleFileNameW(nullptr, app_path, sizeof(app_path));

    std::wstring prog_path = L"\"" + std::wstring(app_path) + L"\"";
    std::wstring prog_id = L" Orca.Slicer.1";
    std::wstring prog_desc = L"OrcaSlicer";
    std::wstring prog_command = prog_path + L" \"%1\"";
    std::wstring reg_base = L"Software\\Classes";
    std::wstring reg_extension = reg_base + L"\\." + extend;
    std::wstring reg_prog_id = reg_base + L"\\" + prog_id;
    std::wstring reg_prog_id_command = reg_prog_id + L"\\Shell\\Open\\Command";

    bool is_new = false;
    is_new |= set_into_win_registry(HKEY_CURRENT_USER, reg_extension.c_str(), prog_id.c_str());
    is_new |= set_into_win_registry(HKEY_CURRENT_USER, reg_prog_id.c_str(), prog_desc.c_str());
    is_new |= set_into_win_registry(HKEY_CURRENT_USER, reg_prog_id_command.c_str(), prog_command.c_str());
    if (is_new)
        // notify Windows only when any of the values gets changed
        ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif // WIN32
}

void GUI_App::disassociate_files(std::wstring extend)
{
#ifdef WIN32
    wchar_t app_path[MAX_PATH];
    ::GetModuleFileNameW(nullptr, app_path, sizeof(app_path));

    std::wstring prog_path = L"\"" + std::wstring(app_path) + L"\"";
    std::wstring prog_id = L" Orca.Slicer.1";
    std::wstring prog_desc = L"OrcaSlicer";
    std::wstring prog_command = prog_path + L" \"%1\"";
    std::wstring reg_base = L"Software\\Classes";
    std::wstring reg_extension = reg_base + L"\\." + extend;
    std::wstring reg_prog_id = reg_base + L"\\" + prog_id;
    std::wstring reg_prog_id_command = reg_prog_id + L"\\Shell\\Open\\Command";

    bool is_new = false;
    is_new |= del_win_registry(HKEY_CURRENT_USER, reg_extension.c_str(), prog_id.c_str());

    bool is_associate_3mf  = app_config->get("associate_3mf") == "true";
    bool is_associate_stl  = app_config->get("associate_stl") == "true";
    bool is_associate_step = app_config->get("associate_step") == "true";
    if (!is_associate_3mf && !is_associate_stl && !is_associate_step)
    {
        is_new |= del_win_registry(HKEY_CURRENT_USER, reg_prog_id.c_str(), prog_desc.c_str());
        is_new |= del_win_registry(HKEY_CURRENT_USER, reg_prog_id_command.c_str(), prog_command.c_str());
    }

    if (is_new)
       ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif // WIN32
}

bool GUI_App::check_url_association(std::wstring url_prefix, std::wstring& reg_bin)
{
    reg_bin = L"";
#ifdef WIN32
    wxRegKey key_full(wxRegKey::HKCU, "Software\\Classes\\" + url_prefix + "\\shell\\open\\command");
    if (!key_full.Exists()) {
        return false;
    }
    reg_bin = key_full.QueryDefaultValue().ToStdWstring();

    boost::filesystem::path binary_path(boost::filesystem::canonical(boost::dll::program_location()));
    std::wstring key_string = L"\"" + binary_path.wstring() + L"\" \"%1\"";
    return key_string == reg_bin;
#else
    return false;
#endif // WIN32
}

void GUI_App::associate_url(std::wstring url_prefix)
{
#ifdef WIN32
    boost::filesystem::path binary_path(boost::filesystem::canonical(boost::dll::program_location()));
    // the path to binary needs to be correctly saved in string with respect to localized characters
    wxString wbinary = wxString::FromUTF8(binary_path.string());
    std::string binary_string = (boost::format("%1%") % wbinary).str();
    BOOST_LOG_TRIVIAL(info) << "Downloader registration: Path of binary: " << binary_string;

    std::string key_string = "\"" + binary_string + "\" \"%1\"";

    wxRegKey key_first(wxRegKey::HKCU, "Software\\Classes\\" + url_prefix);
    wxRegKey key_full(wxRegKey::HKCU, "Software\\Classes\\" + url_prefix + "\\shell\\open\\command");
    if (!key_first.Exists()) {
        key_first.Create(false);
    }
    key_first.SetValue("URL Protocol", "");

    if (!key_full.Exists()) {
        key_full.Create(false);
    }
    key_full = key_string;
#elif defined(__linux__) && defined(SLIC3R_DESKTOP_INTEGRATION)
    DesktopIntegrationDialog::perform_downloader_desktop_integration(into_u8(url_prefix));
#endif // WIN32
}

void GUI_App::disassociate_url(std::wstring url_prefix)
{
#ifdef WIN32
    wxRegKey key_full(wxRegKey::HKCU, "Software\\Classes\\" + url_prefix + "\\shell\\open\\command");
    if (!key_full.Exists()) {
        return;
    }
    key_full = "";
#endif // WIN32
}


void GUI_App::start_download(std::string url)
{
    if (!plater_) {
        BOOST_LOG_TRIVIAL(error) << "Could not start URL download: plater is nullptr.";
        return;
    }
    //lets always init so if the download dest folder was changed, new dest is used
    boost::filesystem::path dest_folder(app_config->get("download_path"));
    if (dest_folder.empty() || !boost::filesystem::is_directory(dest_folder)) {
        std::string msg = _u8L("Could not start URL download. Destination folder is not set. Please choose destination folder in Configuration Wizard.");
        BOOST_LOG_TRIVIAL(error) << msg;
        show_error(nullptr, msg);
        return;
    }
    m_downloader->init(dest_folder);
    m_downloader->start_download(url);

}

bool is_soluble_filament(int extruder_id)
{
    auto &filament_presets = Slic3r::GUI::wxGetApp().preset_bundle->filament_presets;
    auto &filaments        = Slic3r::GUI::wxGetApp().preset_bundle->filaments;

    if (extruder_id >= filament_presets.size()) return false;

    Slic3r::Preset *filament = filaments.find_preset(filament_presets[extruder_id]);
    if (filament == nullptr) return false;

    Slic3r::ConfigOptionBools *support_option = dynamic_cast<Slic3r::ConfigOptionBools *>(filament->config.option("filament_soluble"));
    if (support_option == nullptr) return false;

    return support_option->get_at(0);
};

bool has_filaments(const std::vector<string>& model_filaments) {
    auto &filament_presets = Slic3r::GUI::wxGetApp().preset_bundle->filament_presets;
    if (!Slic3r::GUI::wxGetApp().plater()) return false;
    auto model_objects = Slic3r::GUI::wxGetApp().plater()->model().objects;
    const Slic3r::DynamicPrintConfig &config = wxGetApp().preset_bundle->full_config();
    Model::setExtruderParams(config, filament_presets.size());

    auto get_filament_name = [](int id) { return Model::extruderParamsMap.find(id) != Model::extruderParamsMap.end() ? Model::extruderParamsMap.at(id).materialName : "PLA"; };
    for (const ModelObject *mo : model_objects) {
        for (auto vol : mo->volumes) {
            auto ve = vol->get_extruders();
            for (auto id : ve) {
                auto name = get_filament_name(id);
                if (find(model_filaments.begin(), model_filaments.end(), name) != model_filaments.end()) return true;
            }
        }
    }
    return false;
}

bool is_support_filament(int extruder_id, bool strict_check)
{
    auto &filament_presets = Slic3r::GUI::wxGetApp().preset_bundle->filament_presets;
    auto &filaments        = Slic3r::GUI::wxGetApp().preset_bundle->filaments;

    if (extruder_id >= filament_presets.size()) return false;

    Slic3r::Preset *filament = filaments.find_preset(filament_presets[extruder_id]);
    if (filament == nullptr) return false;

    std::string filament_type = filament->config.option<ConfigOptionStrings>("filament_type")->values[0];

    Slic3r::ConfigOptionBools *support_option = dynamic_cast<Slic3r::ConfigOptionBools *>(filament->config.option("filament_is_support"));

    if(!strict_check &&(filament_type == "PETG" || filament_type == "PLA")) {
        std::vector<string> model_filaments;
        if (filament_type == "PETG")
            model_filaments.emplace_back("PLA");
        else {
            model_filaments = {"PETG", "TPU", "TPU-AMS"};
        }
        if (has_filaments(model_filaments)) return true;
    }
    if (support_option == nullptr) return false;
    return support_option->get_at(0);
};

} // GUI
} //Slic3r
