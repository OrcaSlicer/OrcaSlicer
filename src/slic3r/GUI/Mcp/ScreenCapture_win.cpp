// Windows window capture for automation screenshots (see ScreenCapture.hpp).

#include "ScreenCapture.hpp"

#include <wx/toplevel.h>

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

bool capture_native_window(wxTopLevelWindow& window, wxImage& out, bool& approximate, std::string& note, std::string& err)
{
    HWND hwnd = static_cast<HWND>(window.GetHWND());
    RECT rc{};
    if (hwnd == nullptr || !GetClientRect(hwnd, &rc)) {
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

}}} // namespace Slic3r::GUI::Mcp
