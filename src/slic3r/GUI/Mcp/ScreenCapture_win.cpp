// Windows window capture for automation screenshots and recordings (see ScreenCapture.hpp).

#include "ScreenCapture.hpp"
#include "UiInspector.hpp"
#include "slic3r/GUI/SyntheticInput.hpp"

#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/popupwin.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <eventtoken.h>
#include <inspectable.h>
#include <roapi.h>
#include <windows.foundation.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.h>
#include <windows.graphics.directx.direct3d11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <wx/msw/private.h> // wxFindWinFromHandle()

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

namespace wgc = ABI::Windows::Graphics::Capture;
using ABI::Windows::Graphics::SizeInt32;
using Microsoft::WRL::ComPtr;

namespace {

constexpr auto kFormat = ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized;

// The pixels of `bmp`, which must not be selected into a device context.
bool read_bitmap(HDC dc, HBITMAP bmp, int w, int h, wxImage& out)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h; // top-down rows
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> bgra(size_t(w) * size_t(h) * 4);
    if (GetDIBits(dc, bmp, 0, UINT(h), bgra.data(), &bi, DIB_RGB_COLORS) != h)
        return false;
    out.Create(w, h, false);
    unsigned char* rgb = out.GetData();
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i, rgb += 3) {
        rgb[0] = bgra[i * 4 + 2];
        rgb[1] = bgra[i * 4 + 1];
        rgb[2] = bgra[i * 4 + 0];
    }
    return true;
}

// What the screen shows in `r`, layered windows such as a fading menu or tooltip included.
bool read_screen(const RECT& r, wxImage& out)
{
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
        return false;
    HDC        screen = GetDC(nullptr);
    HDC        mem    = CreateCompatibleDC(screen);
    HBITMAP    bmp    = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ    old    = SelectObject(mem, bmp);
    const BOOL copied = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    const bool read = copied && read_bitmap(mem, bmp, w, h, out);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return read;
}

// PrintWindow makes the window and its children repaint into the bitmap, a repaint the screen can show as a
// flash of the half-drawn window; it is what is left without Windows Graphics Capture.
bool print_window(HWND hwnd, wxImage& out, bool& approximate, std::string& note, std::string& err)
{
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) {
        err = "The window has no native handle";
        return false;
    }
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        err = "The window has no client area";
        return false;
    }
    HDC     screen = GetDC(hwnd);
    HDC     mem    = CreateCompatibleDC(screen);
    HBITMAP bmp    = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old    = SelectObject(mem, bmp);
    // PW_RENDERFULLCONTENT asks DWM for the composed content, which includes most hardware-drawn
    // children; the 3D canvases are composited from their own frames afterwards anyway.
    const BOOL printed = PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
    if (!printed) {
        approximate = true;
        note        = "PrintWindow failed; copied from the screen";
        BitBlt(mem, 0, 0, w, h, screen, 0, 0, SRCCOPY);
    }
    SelectObject(mem, old);
    const bool read = read_bitmap(mem, bmp, w, h, out);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, screen);
    if (!read) {
        err = "Reading the window bitmap failed";
        return false;
    }
    return true;
}

bool has_class(HWND hwnd, const wchar_t* cls)
{
    wchar_t name[32] = {};
    return GetClassNameW(hwnd, name, int(std::size(name))) != 0 && wcscmp(name, cls) == 0;
}

// A menu or submenu, a tooltip, or the list of a native combo box or choice: the system shows each in a
// top-level window of the thread that asked for it.
bool is_system_popup(HWND hwnd)
{
    return has_class(hwnd, L"#32768") || has_class(hwnd, L"tooltips_class32") || has_class(hwnd, L"ComboLBox");
}

// A window another process shows as a popup over one of ours: WebView2 shows a page's drop-down lists and
// context menus from its browser process, in captionless popups owned by the window that hosts the web view.
// A dialog it opens, such as a file chooser, has a caption.
bool is_foreign_popup(HWND hwnd, DWORD process)
{
    if (process == GetCurrentProcessId())
        return false;
    const auto style = static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE));
    if ((style & WS_POPUP) == 0 || (style & WS_CAPTION) == WS_CAPTION)
        return false;
    const HWND root = GetAncestor(hwnd, GA_ROOTOWNER);
    return root != nullptr && root != hwnd && GetWindowThreadProcessId(root, nullptr) == GetCurrentThreadId();
}

// One of our popups: the system's for us, the shadow it draws under one, one of wx's, or another process's.
bool is_popup(HWND hwnd, DWORD process)
{
    if (process != GetCurrentProcessId())
        return is_foreign_popup(hwnd, process);
    return is_system_popup(hwnd) || has_class(hwnd, L"SysShadow") || dynamic_cast<wxPopupWindow*>(wxFindWinFromHandle(hwnd)) != nullptr;
}

// Hidden by DWM though IsWindowVisible() says shown, as a window on another virtual desktop is. A failed query
// counts as shown.
bool cloaked(HWND hwnd)
{
    DWORD value = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &value, sizeof(value))) && value != 0;
}

// A layered window at constant alpha 0: the one sign that a window draws nothing where it is.
bool draws_nothing(HWND hwnd)
{
    BYTE  alpha = 255;
    DWORD flags = 0;
    return (static_cast<DWORD>(GetWindowLongW(hwnd, GWL_EXSTYLE)) & WS_EX_LAYERED) != 0 &&
           GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) != 0 && alpha == 0;
}

// The window popup_menu() showed the wx menu in the menu window `hwnd` from, the menu itself or one of its
// submenus; null for a menu the system shows itself.
wxWindow* wx_menu_from(HWND hwnd)
{
    const auto menu_handle = reinterpret_cast<WXHMENU>(SendMessageW(hwnd, MN_GETHMENU, 0, 0));
    if (menu_handle == nullptr)
        return nullptr;
    for (const auto& [menu, from] : shown_menus())
        if (menu != nullptr && menu->MSWGetMenu(menu_handle) != nullptr)
            return from;
    return nullptr;
}

// The window of the UI thread's active menu: the nearest wx window at or above the one TrackPopupMenu was
// given, or null when that is not in a wx window (a native dialog's control) or no menu is active.
wxWindow* active_menu_from()
{
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(GetCurrentThreadId(), &info) || (info.flags & GUI_INMENUMODE) == 0)
        return nullptr;
    for (HWND h = info.hwndMenuOwner; h != nullptr; h = GetAncestor(h, GA_PARENT)) {
        if (wxWindow* w = wxFindWinFromHandle(h))
            return w;
        if (GetAncestor(h, GA_ROOT) == h)
            break;
    }
    return nullptr;
}

// The wx top-level window `w` belongs to, unless DWM hides it.
wxTopLevelWindow* shown_top_level(wxWindow* w)
{
    auto* top = dynamic_cast<wxTopLevelWindow*>(owning_window(w));
    return top != nullptr && !cloaked(static_cast<HWND>(top->GetHWND())) ? top : nullptr;
}

struct Shown
{
    HWND  hwnd = nullptr;
    RECT  rect{};
    DWORD process = 0;
    DWORD thread  = 0;

    bool is_menu() const { return thread == GetCurrentThreadId() && has_class(hwnd, L"#32768"); }
};

BOOL CALLBACK collect_shown(HWND hwnd, LPARAM windows)
{
    Shown s;
    s.hwnd = hwnd;
    if (IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetWindowRect(hwnd, &s.rect) && !IsRectEmpty(&s.rect)) {
        s.thread = GetWindowThreadProcessId(hwnd, &s.process);
        reinterpret_cast<std::vector<Shown>*>(windows)->push_back(s);
    }
    return TRUE;
}

// The screen's top-level windows, every process's, topmost first as EnumWindows lists them. Popups are read
// from the screen, so this says which window each shows over and whether the screen shows it unmixed.
class Screen
{
public:
    Screen()
    {
        EnumWindows(collect_shown, reinterpret_cast<LPARAM>(&m_windows));
        m_wx_menu_shown = std::any_of(m_windows.begin(), m_windows.end(),
                                      [](const Shown& w) { return w.is_menu() && wx_menu_from(w.hwnd) != nullptr; });
    }

    const std::vector<Shown>& windows() const { return m_windows; }

    // The wx top-level window the popup at `i` shows over, or null when it shows over a window captures do not
    // draw, such as a native file dialog or one DWM hides, and is left out with it. A wx popup shows over the
    // window it opened from, and so does a wx menu (wx shows them recursively, so several may be open). A menu
    // the system shows itself (an edit control's, a native dialog's) is the active one while no wx menu shows:
    // with one showing too, the active menu may be that one, and it is left out. Another popup shows over the
    // first of its owners that is neither a popup nor hidden (Windows makes only top-level windows owners); one
    // without (the list of a combo box, wx's tooltip control) over the topmost window of ours below it that it
    // overlaps and the screen shows.
    wxTopLevelWindow* shown_over(std::size_t i) const
    {
        const Shown& p = m_windows[i];
        if (auto* popup = dynamic_cast<wxPopupWindow*>(wxFindWinFromHandle(p.hwnd)))
            return dynamic_cast<wxTopLevelWindow*>(owning_window(popup));
        if (p.is_menu()) {
            wxWindow* from = wx_menu_from(p.hwnd);
            if (from == nullptr && !m_wx_menu_shown)
                from = active_menu_from();
            return from != nullptr ? shown_top_level(from) : nullptr;
        }
        for (HWND o = GetWindow(p.hwnd, GW_OWNER); o != nullptr; o = GetWindow(o, GW_OWNER)) {
            DWORD process = 0;
            GetWindowThreadProcessId(o, &process);
            if (IsWindowVisible(o) && !is_popup(o, process))
                return cloaked(o) ? nullptr : dynamic_cast<wxTopLevelWindow*>(wxFindWinFromHandle(o));
        }
        for (std::size_t j = i + 1; j < m_windows.size(); ++j) {
            const Shown& w = m_windows[j];
            RECT         both{};
            if (w.process == GetCurrentProcessId() && !is_popup(w.hwnd, w.process) && IntersectRect(&both, &p.rect, &w.rect) &&
                !cloaked(w.hwnd))
                return dynamic_cast<wxTopLevelWindow*>(wxFindWinFromHandle(w.hwnd));
        }
        return nullptr;
    }

    // Whether the screen mixes another window into the popup at `i`, which shows over `over`. A popup of ours over
    // the same window that is not covered itself is drawn with this one, after it, as captures draw popups by the
    // window they show over. Any other window above it that the screen shows there, another application's, a
    // window of ours the capture may not draw, a popup over another window or one left out, would put its pixels
    // in this one's.
    bool covered(std::size_t i, const wxTopLevelWindow* over) const
    {
        for (std::size_t j = 0; j < i; ++j) {
            const Shown& w = m_windows[j];
            RECT         both{};
            if (!IntersectRect(&both, &m_windows[i].rect, &w.rect) || cloaked(w.hwnd) || draws_nothing(w.hwnd))
                continue;
            if (!is_popup(w.hwnd, w.process) || shown_over(j) != over || covered(j, over))
                return true;
        }
        return false;
    }

private:
    std::vector<Shown> m_windows;
    bool               m_wx_menu_shown = false; // a menu window shows one of wx's menus
};

void check(HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        char code[16];
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
        throw std::runtime_error(std::string(what) + " failed (" + code + ")");
    }
}

template<class Factory> ComPtr<Factory> activation_factory(const wchar_t* runtime_class)
{
    ComPtr<Factory> factory;
    check(RoGetActivationFactory(Microsoft::WRL::Wrappers::HStringReference(runtime_class).Get(), IID_PPV_ARGS(&factory)),
          "RoGetActivationFactory");
    return factory;
}

// Ends a capture object's use of its buffers or of the window, as its IClosable says.
template<class T> void close_object(ComPtr<T>& object)
{
    ComPtr<ABI::Windows::Foundation::IClosable> closable;
    if (object && SUCCEEDED(object.As(&closable)))
        closable->Close();
    object.Reset();
}

// The client area within a capture of the window, which covers the window's visible frame.
std::optional<D3D11_BOX> client_box(HWND hwnd, UINT width, UINT height)
{
    RECT  client{}, frame{};
    POINT origin{};
    if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0 || !ClientToScreen(hwnd, &origin) ||
        FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
        return std::nullopt;
    const UINT left = origin.x > frame.left ? UINT(origin.x - frame.left) : 0;
    const UINT top  = origin.y > frame.top ? UINT(origin.y - frame.top) : 0;
    if (left >= width || top >= height)
        return std::nullopt;
    return D3D11_BOX{left, top, 0, std::min(width, left + UINT(client.right)), std::min(height, top + UINT(client.bottom)), 1};
}

// Handlers run on a thread pool thread, which the free-threaded frame pool calls directly when they are agile.
using FrameArrivedHandler = ABI::Windows::Foundation::ITypedEventHandler<wgc::Direct3D11CaptureFramePool*, IInspectable*>;
using ClosedHandler       = ABI::Windows::Foundation::ITypedEventHandler<wgc::GraphicsCaptureItem*, IInspectable*>;
template<class Handler>
using Agile = Microsoft::WRL::Implements<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Handler, Microsoft::WRL::FtmBase>;

} // namespace

class CaptureSessions
{
public:
    CaptureSessions()
    {
        const auto create = [this](D3D_DRIVER_TYPE driver) {
            return D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &m_device,
                                     nullptr, &m_context);
        };
        HRESULT hr = create(D3D_DRIVER_TYPE_HARDWARE);
        if (FAILED(hr))
            hr = create(D3D_DRIVER_TYPE_WARP);
        check(hr, "D3D11CreateDevice");
        ComPtr<IDXGIDevice> dxgi;
        check(m_device.As(&dxgi), "IDXGIDevice");
        ComPtr<IInspectable> device;
        check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), &device), "CreateDirect3D11DeviceFromDXGIDevice");
        check(device.As(&m_capture_device), "IDirect3DDevice");
        m_pools = activation_factory<wgc::IDirect3D11CaptureFramePoolStatics2>(
            RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool);
        m_interop = activation_factory<IGraphicsCaptureItemInterop>(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem);
    }
    CaptureSessions(const CaptureSessions&)            = delete;
    CaptureSessions& operator=(const CaptureSessions&) = delete;

    // The window's client area as the screen shows it. False when Windows Graphics Capture cannot read the
    // window, with `err` set the first time: the window is not tried again.
    bool capture(HWND hwnd, wxImage& out, std::string& err)
    {
        // A handle the system may hand to a new window.
        for (auto it = m_windows.begin(); it != m_windows.end();)
            it = IsWindow(it->first) ? std::next(it) : m_windows.erase(it);
        for (auto it = m_failed.begin(); it != m_failed.end();)
            it = IsWindow(*it) ? std::next(it) : m_failed.erase(it);
        if (m_failed.count(hwnd) != 0)
            return false;
        try {
            std::unique_ptr<Session>& s = m_windows[hwnd];
            if (s && s->closed())
                s.reset();
            if (!s)
                s = open(hwnd);
            // The first frame comes with the next composition; after it, only a change sends one.
            ComPtr<wgc::IDirect3D11CaptureFrame> frame = take(*s->latest, s->image.IsOk() ? std::chrono::milliseconds(0) : kFirstFrame);
            if (frame) {
                read(*s, hwnd, frame.Get());
                close_object(frame);
            }
            if (!s->image.IsOk())
                throw std::runtime_error("no frame of the window came");
            // A copy: the caller draws the 3D views into it, and the kept frame stays as the window sent it.
            out = s->image.Copy();
            return true;
        } catch (const std::exception& e) {
            err = std::string("Windows Graphics Capture: ") + e.what();
            m_windows.erase(hwnd);
            m_failed.insert(hwnd);
            return false;
        }
    }

private:
    static constexpr std::chrono::milliseconds kFirstFrame{1000};

    // The newest frame, which the frame pool's thread hands over as it arrives. Holding only one leaves the
    // pool's other buffer free, so a change is never dropped for want of one.
    struct Latest
    {
        std::mutex                           mutex;
        std::condition_variable              arrived;
        ComPtr<wgc::IDirect3D11CaptureFrame> frame;
        bool                                 closed = false; // the window is gone
    };

    struct Session
    {
        ComPtr<wgc::IGraphicsCaptureItem>        item;
        ComPtr<wgc::IDirect3D11CaptureFramePool> pool;
        ComPtr<wgc::IGraphicsCaptureSession>     session;
        std::shared_ptr<Latest>                  latest = std::make_shared<Latest>();
        EventRegistrationToken                   on_frame{}, on_closed{};
        SizeInt32                                size{};
        ComPtr<ID3D11Texture2D>                  staging;
        D3D11_TEXTURE2D_DESC                     staging_desc{};
        wxImage                                  image; // the newest frame read

        bool closed() const
        {
            std::lock_guard<std::mutex> lock(latest->mutex);
            return latest->closed;
        }

        ~Session()
        {
            if (pool)
                pool->remove_FrameArrived(on_frame);
            if (item)
                item->remove_Closed(on_closed);
            close_object(session);
            close_object(pool);
            std::lock_guard<std::mutex> lock(latest->mutex);
            close_object(latest->frame);
        }
    };

    std::unique_ptr<Session> open(HWND hwnd)
    {
        auto s = std::make_unique<Session>();
        check(m_interop->CreateForWindow(hwnd, IID_PPV_ARGS(&s->item)), "CreateForWindow");
        check(s->item->get_Size(&s->size), "GraphicsCaptureItem.Size");
        check(m_pools->CreateFreeThreaded(m_capture_device.Get(), kFormat, 2, s->size, &s->pool), "CreateFreeThreaded");
        auto on_frame = Microsoft::WRL::Callback<Agile<FrameArrivedHandler>>(
            [latest = s->latest](wgc::IDirect3D11CaptureFramePool* pool, IInspectable*) -> HRESULT {
                ComPtr<wgc::IDirect3D11CaptureFrame> frame;
                if (FAILED(pool->TryGetNextFrame(&frame)) || !frame)
                    return S_OK;
                std::lock_guard<std::mutex> lock(latest->mutex);
                close_object(latest->frame);
                latest->frame = std::move(frame);
                latest->arrived.notify_all();
                return S_OK;
            });
        auto on_closed = Microsoft::WRL::Callback<Agile<ClosedHandler>>(
            [latest = s->latest](wgc::IGraphicsCaptureItem*, IInspectable*) -> HRESULT {
                std::lock_guard<std::mutex> lock(latest->mutex);
                latest->closed = true;
                latest->arrived.notify_all();
                return S_OK;
            });
        if (!on_frame || !on_closed)
            throw std::runtime_error("out of memory");
        check(s->pool->add_FrameArrived(on_frame.Get(), &s->on_frame), "FrameArrived");
        check(s->item->add_Closed(on_closed.Get(), &s->on_closed), "Closed");
        check(s->pool->CreateCaptureSession(s->item.Get(), &s->session), "CreateCaptureSession");
        // The recording draws its own pointer. Windows 10 draws a yellow border around a captured window,
        // which only later versions let an application turn off.
        ComPtr<wgc::IGraphicsCaptureSession2> cursor;
        if (SUCCEEDED(s->session.As(&cursor)))
            cursor->put_IsCursorCaptureEnabled(false);
        ComPtr<wgc::IGraphicsCaptureSession3> border;
        if (SUCCEEDED(s->session.As(&border)))
            border->put_IsBorderRequired(false);
        check(s->session->StartCapture(), "StartCapture");
        return s;
    }

    static ComPtr<wgc::IDirect3D11CaptureFrame> take(Latest& latest, std::chrono::milliseconds wait)
    {
        std::unique_lock<std::mutex> lock(latest.mutex);
        latest.arrived.wait_for(lock, wait, [&] { return latest.frame || latest.closed; });
        return std::move(latest.frame);
    }

    // Copies the frame's client area into the session's image.
    void read(Session& s, HWND hwnd, wgc::IDirect3D11CaptureFrame* frame)
    {
        SizeInt32 content{};
        check(frame->get_ContentSize(&content), "ContentSize");
        if (content.Width != s.size.Width || content.Height != s.size.Height) {
            // A resized window: frames from now on come at its new size.
            s.size = content;
            check(s.pool->Recreate(m_capture_device.Get(), kFormat, 2, content), "Recreate");
        }
        ComPtr<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface> surface;
        check(frame->get_Surface(&surface), "Surface");
        ComPtr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
        check(surface.As(&access), "IDirect3DDxgiInterfaceAccess");
        ComPtr<ID3D11Texture2D> texture;
        check(access->GetInterface(IID_PPV_ARGS(&texture)), "ID3D11Texture2D");
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        const std::optional<D3D11_BOX> box = client_box(hwnd, std::min(desc.Width, UINT(std::max(0, content.Width))),
                                                         std::min(desc.Height, UINT(std::max(0, content.Height))));
        if (!box)
            return;
        const UINT width = box->right - box->left, height = box->bottom - box->top;
        if (!s.staging || s.staging_desc.Width != width || s.staging_desc.Height != height) {
            D3D11_TEXTURE2D_DESC staging{};
            staging.Width            = width;
            staging.Height           = height;
            staging.MipLevels        = 1;
            staging.ArraySize        = 1;
            staging.Format           = desc.Format;
            staging.SampleDesc.Count = 1;
            staging.Usage            = D3D11_USAGE_STAGING;
            staging.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
            s.staging.Reset();
            check(m_device->CreateTexture2D(&staging, nullptr, &s.staging), "CreateTexture2D");
            s.staging_desc = staging;
        }
        m_context->CopySubresourceRegion(s.staging.Get(), 0, 0, 0, 0, texture.Get(), 0, &*box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(m_context->Map(s.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map");
        wxImage        image(int(width), int(height), false);
        unsigned char* rgb = image.GetData();
        for (UINT y = 0; y < height; ++y) {
            const unsigned char* bgra = static_cast<const unsigned char*>(mapped.pData) + std::size_t(y) * mapped.RowPitch;
            for (UINT x = 0; x < width; ++x, bgra += 4, rgb += 3) {
                rgb[0] = bgra[2];
                rgb[1] = bgra[1];
                rgb[2] = bgra[0];
            }
        }
        m_context->Unmap(s.staging.Get(), 0);
        s.image = std::move(image);
    }

    ComPtr<ID3D11Device>                                                m_device;
    ComPtr<ID3D11DeviceContext>                                         m_context; // UI thread only
    ComPtr<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice> m_capture_device;
    ComPtr<wgc::IDirect3D11CaptureFramePoolStatics2>                    m_pools;
    ComPtr<IGraphicsCaptureItemInterop>                                 m_interop;
    std::map<HWND, std::unique_ptr<Session>>                            m_windows;
    std::set<HWND>                                                      m_failed;
};

std::shared_ptr<CaptureSessions> open_capture_sessions()
{
    static const bool supported = [] {
        try {
            const auto statics = activation_factory<wgc::IGraphicsCaptureSessionStatics>(
                RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession);
            boolean yes = false;
            return SUCCEEDED(statics->IsSupported(&yes)) && yes;
        } catch (const std::exception&) {
            return false;
        }
    }();
    if (!supported)
        return nullptr;
    try {
        return std::make_shared<CaptureSessions>();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Windows Graphics Capture is not available: " << e.what();
        return nullptr;
    }
}

bool capture_native_window(wxTopLevelWindow& window, CaptureSessions* sessions, Capture& out, std::string& err)
{
    HWND hwnd = static_cast<HWND>(window.GetHWND());
    if (hwnd == nullptr) {
        err = "The window has no native handle";
        return false;
    }
    std::shared_ptr<CaptureSessions> own;
    if (sessions == nullptr) {
        own      = open_capture_sessions();
        sessions = own.get();
    }
    std::string wgc_err;
    if (sessions != nullptr) {
        if (sessions->capture(hwnd, out.image, wgc_err))
            return true;
        if (!wgc_err.empty())
            BOOST_LOG_TRIVIAL(warning) << "MCP capture: " << wgc_err << "; using PrintWindow";
    }
    return print_window(hwnd, out.image, out.approximate, out.note, err);
}

bool capture_native_popup(wxWindow& popup, wxImage& out)
{
    const auto   hwnd    = static_cast<HWND>(popup.GetHWND());
    const Screen screen;
    const auto&  windows = screen.windows();
    const auto   it      = std::find_if(windows.begin(), windows.end(), [hwnd](const Shown& w) { return w.hwnd == hwnd; });
    if (it == windows.end() || cloaked(hwnd))
        return false;
    const auto i = std::size_t(it - windows.begin());
    return !screen.covered(i, screen.shown_over(i)) && read_screen(it->rect, out);
}

std::vector<PopupCapture> capture_native_popups()
{
    const Screen screen;
    const auto&  windows = screen.windows();
    std::vector<PopupCapture> out;
    for (std::size_t i = windows.size(); i-- > 0;) {
        const Shown& w = windows[i];
        // wx's popups come from popups_of() in capture_popups().
        const bool popup = (w.thread == GetCurrentThreadId() && is_system_popup(w.hwnd)) || is_foreign_popup(w.hwnd, w.process);
        if (!popup || cloaked(w.hwnd))
            continue;
        PopupCapture p;
        p.owner = screen.shown_over(i);
        if (p.owner == nullptr || screen.covered(i, p.owner) || !read_screen(w.rect, p.image))
            continue;
        p.offset = wxPoint(w.rect.left, w.rect.top) - p.owner->ClientToScreen(wxPoint(0, 0));
        out.push_back(std::move(p));
    }
    return out;
}

}}} // namespace Slic3r::GUI::Mcp
