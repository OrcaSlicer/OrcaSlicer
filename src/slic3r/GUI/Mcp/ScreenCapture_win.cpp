// Windows window capture for automation screenshots and recordings (see ScreenCapture.hpp).

#include "ScreenCapture.hpp"

#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/toplevel.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
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

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

namespace wgc = ABI::Windows::Graphics::Capture;
using ABI::Windows::Graphics::SizeInt32;
using Microsoft::WRL::ComPtr;

namespace {

constexpr auto kFormat = ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized;

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

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h; // top-down rows
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> bgra(size_t(w) * size_t(h) * 4);
    const int lines = GetDIBits(mem, bmp, 0, UINT(h), bgra.data(), &bi, DIB_RGB_COLORS);

    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, screen);
    if (lines != h) {
        err = "Reading the window bitmap failed";
        return false;
    }

    out.Create(w, h, false);
    unsigned char* rgb = out.GetData();
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i, rgb += 3) {
        rgb[0] = bgra[i * 4 + 2];
        rgb[1] = bgra[i * 4 + 1];
        rgb[2] = bgra[i * 4 + 0];
    }
    return true;
}

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

bool capture_native_window(wxTopLevelWindow& window, CaptureSessions* sessions, wxImage& out, bool& approximate, std::string& note,
                           std::vector<wxRect>& /*covered*/, std::string& err)
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
        if (sessions->capture(hwnd, out, wgc_err))
            return true;
        if (!wgc_err.empty())
            BOOST_LOG_TRIVIAL(warning) << "MCP capture: " << wgc_err << "; using PrintWindow";
    }
    return print_window(hwnd, out, approximate, note, err);
}

}}} // namespace Slic3r::GUI::Mcp
