#include <wx/dcmemory.h>
#include <wx/graphics.h>
#include <algorithm>
#include <cmath>

#include "EncodedFilament.hpp"
#include "FilamentBitmapUtils.hpp"
#include "GUI_App.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r { namespace GUI {

void fill_gradient_rect_east(wxDC& dc, const wxRect& rect, const wxColour& from, const wxColour& to)
{
    if (rect.width <= 0 || rect.height <= 0) return;

    auto mix_channel = [](unsigned char a, unsigned char b, double t) {
        return static_cast<unsigned char>(a + (b - a) * t + 0.5);
    };

    dc.SetPen(*wxTRANSPARENT_PEN);
    for (int x = 0; x < rect.width; ++x) {
        const double t = rect.width > 1 ? static_cast<double>(x) / (rect.width - 1) : 0.0;
        const wxColour col(mix_channel(from.Red(), to.Red(), t),
                           mix_channel(from.Green(), to.Green(), t),
                           mix_channel(from.Blue(), to.Blue(), t),
                           mix_channel(from.Alpha(), to.Alpha(), t));
        dc.SetBrush(wxBrush(col));
        dc.DrawRectangle(rect.x + x, rect.y, 1, rect.height);
    }
}

static std::string to_hex(const wxColour& c)
{
    return wxString::Format("#%02X%02X%02X", c.Red(), c.Green(), c.Blue()).ToStdString();
}

wxColour blend_n_colors(const std::vector<wxColour>& cols, const std::vector<double>& weights)
{
    const size_t n = std::min(cols.size(), weights.size());
    std::vector<std::string> hex_colors;
    std::vector<int>         int_weights;
    hex_colors.reserve(n);
    int_weights.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        hex_colors.push_back(to_hex(cols[i]));
        // Scale double weights (e.g. 0.5) to int (5000) for blend_color_multi;
        // only relative magnitude matters.
        int_weights.push_back(static_cast<int>(std::lround(weights[i] * 10000.0)));
    }
    wxColour blended(Slic3r::blend_color_multi(hex_colors, int_weights));
    return blended.IsOk() ? blended : wxColour(128, 128, 128);
}

std::vector<wxColour> sample_gradient_ramp(const wxColour& first,
                                           const wxColour& second,
                                           const Slic3r::GradientCurve& curve,
                                           int steps)
{
    std::vector<wxColour> ramp;
    if (steps <= 0 || curve.points.size() < 2) return ramp;

    ramp.reserve(steps);
    for (int i = 0; i < steps; ++i) {
        const double t  = (steps > 1) ? (i + 0.5) / steps : 0.5;
        const double r1 = Slic3r::sample_gradient_curve(curve, t);
        ramp.push_back(blend_n_colors({first, second}, {r1, 1.0 - r1}));
    }
    return ramp;
}

// Resolve the curve a gradient slot is sampled with, mirroring the slicer's fallback in
// ToolOrdering: a custom curve wins, otherwise a straight line between gradient_range's
// endpoints, otherwise the 0.10 -> 0.90 default.
static Slic3r::GradientCurve mixed_gradient_curve(const Slic3r::DynamicPrintConfig& cfg, size_t slot)
{
    const auto* curve_opt = cfg.option<ConfigOptionStrings>("filament_mixed_gradient_curve");
    if (curve_opt && slot < curve_opt->values.size() && !curve_opt->values[slot].empty()) {
        Slic3r::GradientCurve custom = Slic3r::parse_gradient_curve(curve_opt->values[slot]);
        if (custom.points.size() >= 2) return custom;
    }

    double start = kGradientMinRatio, end = kGradientMaxRatio;
    const auto* range_opt = cfg.option<ConfigOptionStrings>("filament_mixed_gradient_range");
    if (range_opt && slot < range_opt->values.size() && !range_opt->values[slot].empty()) {
        CNumericLocalesSetter c_locale_setter;
        float v0 = 0, v1 = 0;
        if (std::sscanf(range_opt->values[slot].c_str(), "%f,%f", &v0, &v1) == 2 &&
            v0 > 0 && v0 < 1.0 && v1 > 0 && v1 < 1.0) {
            start = v0;
            end   = v1;
        }
    }

    Slic3r::GradientCurve curve;
    curve.points = {{0.0, start, NAN, NAN}, {1.0, end, NAN, NAN}};
    return curve;
}

std::vector<wxColour> mixed_gradient_ramp(const Slic3r::DynamicPrintConfig& cfg, size_t slot, int steps)
{
    const auto* is_mixed_opt = cfg.option<ConfigOptionBools>("filament_is_mixed");
    const auto* grad_opt     = cfg.option<ConfigOptionBools>("filament_mixed_gradient");
    const auto* comp_opt     = cfg.option<ConfigOptionStrings>("filament_mixed_components");
    const auto* colour_opt   = cfg.option<ConfigOptionStrings>("filament_colour");
    if (!is_mixed_opt || !grad_opt || !comp_opt || !colour_opt) return {};
    if (slot >= is_mixed_opt->values.size() || !is_mixed_opt->values[slot]) return {};
    if (slot >= grad_opt->values.size() || !grad_opt->values[slot]) return {};
    if (slot >= comp_opt->values.size()) return {};

    // Only two-component slots fade; anything else stays on the plain blended swatch.
    const auto comp_ids = Slic3r::parse_mixed_components(comp_opt->values[slot]);
    if (comp_ids.size() != 2) return {};

    auto component_colour = [&](unsigned int id) {
        wxColour c = (id >= 1 && id <= colour_opt->values.size()) ? wxColour(colour_opt->values[id - 1]) : wxColour();
        return c.IsOk() ? c : wxColour("#D9D9D9");
    };

    // Both gradient_range and the curve express the *first* component's ratio over Z, so
    // the components stay in config order and the curve alone decides which end is which.
    return sample_gradient_ramp(component_colour(comp_ids[0]), component_colour(comp_ids[1]),
                                mixed_gradient_curve(cfg, slot), steps);
}

void fill_gradient_ramp_rect(wxDC& dc, const wxRect& rect, const std::vector<wxColour>& ramp)
{
    if (rect.width <= 0 || rect.height <= 0 || ramp.empty()) return;

    dc.SetPen(*wxTRANSPARENT_PEN);
    for (int y = 0; y < rect.height; ++y) {
        // Row 0 is the top of the rect and so takes the ramp's last entry, the model's top.
        // Mapping over height - 1 puts both ends of the ramp on screen even in a short swatch.
        const double t = (rect.height > 1) ? (double) (rect.height - 1 - y) / (rect.height - 1) : 0.5;
        dc.SetBrush(wxBrush(ramp[static_cast<size_t>(t * (ramp.size() - 1) + 0.5)]));
        dc.DrawRectangle(rect.x, rect.y + y, rect.width, 1);
    }
}

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

wxBitmap create_gradient_ramp_bitmap(const std::vector<wxColour>& ramp, const wxSize& size)
{
    if (ramp.empty()) return wxNullBitmap;

    BitmapDC bdc = init_bitmap_dc(size);
    if (!bdc.dc.IsOk()) return wxNullBitmap;

    fill_gradient_ramp_rect(bdc.dc, wxRect(0, 0, size.GetWidth(), size.GetHeight()), ramp);

    bdc.dc.SelectObject(wxNullBitmap);
    return bdc.bitmap;
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

void recompute_mixed_slot_colors(std::vector<wxColour>& colors,
                                 const Slic3r::DynamicPrintConfig& cfg)
{
    const auto* is_mixed_opt = cfg.option<ConfigOptionBools>("filament_is_mixed");
    const auto* comp_opt     = cfg.option<ConfigOptionStrings>("filament_mixed_components");
    const auto* ratio_opt    = cfg.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios");
    const auto* grad_opt     = cfg.option<ConfigOptionBools>("filament_mixed_gradient");
    if (!is_mixed_opt || !comp_opt) return;

    const size_t n = is_mixed_opt->values.size();
    if (colors.size() < n) colors.resize(n);

    const auto* colour_opt = cfg.option<ConfigOptionStrings>("filament_colour");
    const auto  kFallback  = wxColour(128, 128, 128, 255);

    for (size_t i = 0; i < n; ++i) {
        if (!is_mixed_opt->values[i]) continue;

        if (i >= comp_opt->values.size()) { colors[i] = kFallback; continue; }
        auto comp_ids = Slic3r::parse_mixed_components(comp_opt->values[i]);
        if (comp_ids.empty()) { colors[i] = kFallback; continue; }

        bool is_gradient = grad_opt && i < grad_opt->values.size() && grad_opt->values[i];
        std::vector<unsigned int> use_ids = comp_ids;
        std::vector<int>          weights;

        if (is_gradient && comp_ids.size() >= 2) {
            use_ids = { comp_ids.front(), comp_ids.back() };
            weights = { 5000, 5000 };
        } else {
            auto ratios_d = Slic3r::parse_mixed_ratios(
                (ratio_opt && i < ratio_opt->values.size()) ? ratio_opt->values[i] : std::string{},
                comp_ids.size());
            weights.reserve(comp_ids.size());
            for (double r : ratios_d)
                weights.push_back(static_cast<int>(std::lround(r * 10000.0)));
        }

        std::vector<std::string> hex_colors;
        hex_colors.reserve(use_ids.size());
        bool any_invalid = false;
        for (unsigned int id : use_ids) {
            if (id == 0 || id > colors.size()) { any_invalid = true; break; }
            wxColour c = colors[id - 1];
            if (c.IsOk() && (c.Red() > 0 || c.Green() > 0 || c.Blue() > 0)) {
                hex_colors.push_back(to_hex(c));
            } else if (colour_opt && (id - 1) < colour_opt->values.size()) {
                hex_colors.push_back(colour_opt->values[id - 1]);
            } else {
                any_invalid = true; break;
            }
        }
        if (any_invalid) { colors[i] = kFallback; continue; }

        std::string hex = Slic3r::blend_color_multi(hex_colors, weights);
        wxColour blended(hex);
        if (!blended.IsOk()) blended = kFallback;
        colors[i] = wxColour(blended.Red(), blended.Green(), blended.Blue(), 255);
    }
}

}} // namespace Slic3r::GUI