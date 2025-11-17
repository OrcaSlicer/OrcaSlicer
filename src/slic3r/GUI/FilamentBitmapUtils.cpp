#include <wx/dcmemory.h>
#include <wx/graphics.h>
#include <algorithm>
#include <cmath>

#include "EncodedFilament.hpp"
#include "GUI_App.hpp"

namespace Slic3r { namespace GUI {

// Helper struct to hold bitmap and DC
struct BitmapDC {
    wxBitmap bitmap;
    wxMemoryDC dc;

    BitmapDC(const wxSize& size) : bitmap(size){
#ifdef __WXOSX__
        bitmap.UseAlpha();
#endif
        dc.SelectObject(bitmap);
        // Don't set white background - let the color patterns fill the entire area
        dc.SetPen(*wxTRANSPARENT_PEN);
    }
};

static BitmapDC init_bitmap_dc(const wxSize& size) {
    return BitmapDC(size);
}

// Check if a color is transparent (alpha == 0)
static bool is_transparent_color(const wxColour& color) {
    return color.Alpha() == 0;
}

// Create transparent bitmap
static wxBitmap create_transparent_bitmap(const wxSize& size) {
    BitmapDC bdc = init_bitmap_dc(size);
    if (!bdc.dc.IsOk()) return wxNullBitmap;

    // Create checkerboard pattern
    wxColour light_gray(217, 217, 217);  // #D9D9D9
    wxColour white(255, 255, 255);

    bool is_dark_mode = wxGetApp().dark_mode();

    // Calculate parameters based on mode
    int start_pos = is_dark_mode ? 0 : 1;
    int end_width = is_dark_mode ? size.GetWidth() : size.GetWidth() - 1;
    int end_height = is_dark_mode ? size.GetHeight() : size.GetHeight() - 1;
    int square_size = std::max(6, std::min(end_width - start_pos, end_height - start_pos) / 8);

    // Draw checkerboard
    for (int x = start_pos; x < end_width; x += square_size) {
        for (int y = start_pos; y < end_height; y += square_size) {
            bool is_light = ((x / square_size) + (y / square_size)) % 2 == 0;
            bdc.dc.SetBrush(wxBrush(is_light ? white : light_gray));

            int width = std::min(square_size, size.GetWidth() - x);
            int height = std::min(square_size, size.GetHeight() - y);
            bdc.dc.DrawRectangle(x, y, width, height);
        }
    }

    // Add border only in light mode
    if (!is_dark_mode) {
        bdc.dc.SetPen(wxPen(wxColour(130, 130, 128), 1, wxPENSTYLE_SOLID));
        bdc.dc.SetBrush(*wxTRANSPARENT_BRUSH);
        bdc.dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
    }

    bdc.dc.SelectObject(wxNullBitmap);
    return bdc.bitmap;
}

// Sort colors by HSV values (primarily by hue, then saturation, then value)
static void sort_colors_by_hsv(std::vector<wxColour>& colors) {
    if (colors.size() < 2) return;
    std::sort(colors.begin(), colors.end(),
        [](const wxColour& a, const wxColour& b) {
            ColourHSV ha = wxColourToHSV(a);
            ColourHSV hb = wxColourToHSV(b);
            if (ha.h != hb.h) return ha.h < hb.h;
            if (ha.s != hb.s) return ha.s < hb.s;
            return ha.v < hb.v;
        });
}

static wxBitmap create_single_filament_bitmap(const wxColour& color, const wxSize& size)
{
    // Check if color is transparent
    if (is_transparent_color(color)) {
        return create_transparent_bitmap(size);
    }

    BitmapDC bdc = init_bitmap_dc(size);
    if (!bdc.dc.IsOk()) return wxNullBitmap;

    bdc.dc.SetBackground(wxBrush(color));
    bdc.dc.Clear();
    bdc.dc.SetBrush(wxBrush(color));
    bdc.dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

    // Add gray border for light colors (similar to wxExtensions.cpp logic) - only in light mode
    if (!wxGetApp().dark_mode() && color.Red() > 224 && color.Blue() > 224 && color.Green() > 224) {
        bdc.dc.SetPen(wxPen(wxColour(130, 130, 128), 1, wxPENSTYLE_SOLID));
        bdc.dc.SetBrush(*wxTRANSPARENT_BRUSH);
        bdc.dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
    }

    // Add white border for dark colors - only in dark mode
    if(wxGetApp().dark_mode() && color.Red() < 45 && color.Blue() < 45 && color.Green() < 45) {
        bdc.dc.SetPen(wxPen(wxColour(207, 207, 207), 1, wxPENSTYLE_SOLID));
        bdc.dc.SetBrush(*wxTRANSPARENT_BRUSH);
        bdc.dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
    }

    bdc.dc.SelectObject(wxNullBitmap);
    return bdc.bitmap;
}

static wxBitmap create_dual_filament_bitmap(const wxColour& color1, const wxColour& color2, const wxSize& size)
{
    BitmapDC bdc = init_bitmap_dc(size);

    int half_width = size.GetWidth() / 2;

    bdc.dc.SetBrush(wxBrush(color1));
    bdc.dc.DrawRectangle(0, 0, half_width, size.GetHeight());

    bdc.dc.SetBrush(wxBrush(color2));
    bdc.dc.DrawRectangle(half_width, 0, size.GetWidth() - half_width, size.GetHeight());

    bdc.dc.SelectObject(wxNullBitmap);
    return bdc.bitmap;
}

static wxBitmap create_triple_filament_bitmap(const std::vector<wxColour>& colors, const wxSize& size)
{
    BitmapDC bdc = init_bitmap_dc(size);

    int third_width = size.GetWidth() / 3;
    int remaining_width = size.GetWidth() - (third_width * 2);

    // Draw three vertical sections
    bdc.dc.SetBrush(wxBrush(colors[0]));
    bdc.dc.DrawRectangle(0, 0, third_width, size.GetHeight());

    bdc.dc.SetBrush(wxBrush(colors[1]));
    bdc.dc.DrawRectangle(third_width, 0, third_width, size.GetHeight());

    bdc.dc.SetBrush(wxBrush(colors[2]));
    bdc.dc.DrawRectangle(third_width * 2, 0, remaining_width, size.GetHeight());

    bdc.dc.SelectObject(wxNullBitmap);
    return bdc.bitmap;
}

static wxBitmap create_quadruple_filament_bitmap(const std::vector<wxColour>& colors, const wxSize& size)
{
    BitmapDC bdc = init_bitmap_dc(size);

    int half_width = (size.GetWidth() + 1) / 2;
    int half_height = (size.GetHeight() + 1) / 2;

    const int rects[4][4] = {
        {0, 0, half_width, half_height},                    // Top left
        {half_width, 0, size.GetWidth() - half_width, half_height},           // Top right
        {0, half_height, half_width, size.GetHeight() - half_height},         // Bottom left
        {half_width, half_height, size.GetWidth() - half_width, size.GetHeight() - half_height}  // Bottom right
    };

    for (int i = 0; i < 4; i++) {
        bdc.dc.SetBrush(wxBrush(colors[i]));
        bdc.dc.DrawRectangle(rects[i][0], rects[i][1], rects[i][2], rects[i][3]);
    }

    bdc.dc.SelectObject(wxNullBitmap);
    return bdc.bitmap;
}

static wxBitmap create_gradient_filament_bitmap(const std::vector<wxColour>& colors, const wxSize& size)
{
    BitmapDC bdc = init_bitmap_dc(size);

    if (colors.size() == 1) {
        return create_single_filament_bitmap(colors[0], size);
    }

    // use segment gradient, make transition more natural
    wxDC& dc = bdc.dc;
    int total_width = size.GetWidth();
    int height = size.GetHeight();

    // calculate segment count
    int segment_count = colors.size() - 1;
    double segment_width = (double)total_width / segment_count;

    int left = 0;
    for (int i = 0; i < segment_count; i++) {
        int current_width = (int)segment_width;

        // handle last segment, ensure fully filled
        if (i == segment_count - 1) {
            current_width = total_width - left;
        }

        // avoid width exceed boundary
        if (left + current_width > total_width) {
            current_width = total_width - left;
        }

        if (current_width > 0) {
            auto rect = wxRect(left, 0, current_width, height);
            dc.GradientFillLinear(rect, colors[i], colors[i + 1], wxEAST);
            left += current_width;
        }
    }

    bdc.dc.SelectObject(wxNullBitmap);
    return bdc.bitmap;
}

wxBitmap create_filament_bitmap(const std::vector<wxColour>& colors, const wxSize& size, bool force_gradient)
{
    if (colors.empty()) return wxNullBitmap;

    // Make a copy to sort without modifying original
    std::vector<wxColour> sorted_colors = colors;

    // Sort colors by HSV when there are 2 or more colors
    if (sorted_colors.size() >= 2) {
        sort_colors_by_hsv(sorted_colors);
    }

    if (force_gradient && sorted_colors.size() >= 2) {
        return create_gradient_filament_bitmap(sorted_colors, size);
    }

    switch (sorted_colors.size()) {
        case 1: return create_single_filament_bitmap(sorted_colors[0], size);
        case 2: return create_dual_filament_bitmap(sorted_colors[0], sorted_colors[1], size);
        case 3: return create_triple_filament_bitmap(sorted_colors, size);
        case 4: return create_quadruple_filament_bitmap(sorted_colors, size);
        default: return create_gradient_filament_bitmap(sorted_colors, size);
    }
}

}} // namespace Slic3r::GUI