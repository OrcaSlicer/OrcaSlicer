#include <wx/dcmemory.h>
#include <wx/dcgraph.h>
#include <wx/graphics.h>
#include <wx/settings.h>
#include <wx/window.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <string>
#include <tuple>

#include "EncodedFilament.hpp"
#include "FilamentBitmapUtils.hpp"
#include "GUI_App.hpp"
#include "GuiColor.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r { namespace GUI {

// Barycentric utilities for a ternary (triangle) ratio picker.
double tri_signed_area2(TriPoint a, TriPoint b, TriPoint c)
{
    return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
}

bool tri_contains(TriPoint p, TriPoint v0, TriPoint v1, TriPoint v2)
{
    double total = tri_signed_area2(v0, v1, v2);
    if (std::abs(total) < 1e-9) return false;
    double s0 = tri_signed_area2(p, v1, v2) / total;
    double s1 = tri_signed_area2(v0, p, v2) / total;
    double s2 = 1.0 - s0 - s1;
    return s0 >= -0.001 && s1 >= -0.001 && s2 >= -0.001;
}

void tri_barycentric(TriPoint p, TriPoint v0, TriPoint v1, TriPoint v2,
                     double& w0, double& w1, double& w2)
{
    double total = std::abs(tri_signed_area2(v0, v1, v2));
    if (total < 1e-9) { w0 = w1 = w2 = 1.0 / 3.0; return; }
    w0 = std::abs(tri_signed_area2(p, v1, v2)) / total;
    w1 = std::abs(tri_signed_area2(v0, p, v2)) / total;
    w2 = 1.0 - w0 - w1;
    w0 = std::clamp(w0, 0.0, 1.0);
    w1 = std::clamp(w1, 0.0, 1.0);
    w2 = std::clamp(w2, 0.0, 1.0);
    double s = w0 + w1 + w2;
    if (s > 0) { w0 /= s; w1 /= s; w2 /= s; }
}

TriPoint tri_clamp(TriPoint p, TriPoint v0, TriPoint v1, TriPoint v2)
{
    double w0, w1, w2;
    tri_barycentric(p, v0, v1, v2, w0, w1, w2);
    return {w0 * v0.x + w1 * v1.x + w2 * v2.x,
            w0 * v0.y + w1 * v1.y + w2 * v2.y};
}

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
Slic3r::GradientCurve mixed_gradient_curve(const Slic3r::DynamicPrintConfig& cfg, size_t slot)
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

namespace {

// Layout ratios of the gradient plot rect, copied from GradientCurveEditor so the read-only
// preview and the interactive editor stay pixel-identical. Plot rect is square 1:1; the
// right/bottom margins host the axis arrows and labels.
constexpr double kPlotLeftRatio   = 0.0316;
constexpr double kPlotRightRatio  = 0.6766;
constexpr double kPlotTopRatio    = 0.1529;
constexpr double kPlotBottomRatio = 0.8474;
constexpr int    kGridDivisions   = 9;    // 10 grid lines including the outer borders.
constexpr int    kStrokeAxis      = 2;    // axis line width (px, no DPI scaling)
constexpr int    kAxisArrowHalf   = 5;    // half-base of the axis arrow triangle (DIP)
constexpr int    kAxisArrowLen    = 10;   // length of the axis arrow triangle (DIP)
constexpr int    kPointRadius     = 4;    // anchor outer radius (DIP)
constexpr float  kBgSimilarThreshold = 15.0f;
constexpr int    kOutlineExtraDip    = 2;
constexpr double kTriangleMarginDip  = 20.0;

// Quadratic blend that never goes out of gamut, matching MixedFilamentDialog::blend_colors.
wxColour lerp_blend(const wxColour& a, const wxColour& b, double ratio_a)
{
    unsigned char r, g, bl;
    Slic3r::filament_mixer_lerp(a.Red(), a.Green(), a.Blue(),
                                b.Red(), b.Green(), b.Blue(),
                                static_cast<float>(1.0 - ratio_a), &r, &g, &bl);
    return wxColour(r, g, bl);
}

// DIP conversion for these free functions: unlike the wxWindow member FromDIP, it needs the
// window parameter explicitly; nullptr picks the app's default DPI like the Publish dialog does.
int dip_px(int v) { return wxWindow::FromDIP(v, nullptr); }

} // namespace

wxRect mixed_gradient_plot_rect(const wxSize& sz)
{
    const int x  = static_cast<int>(std::lround(sz.x * kPlotLeftRatio));
    const int y  = static_cast<int>(std::lround(sz.y * kPlotTopRatio));
    const int x2 = static_cast<int>(std::lround(sz.x * kPlotRightRatio));
    const int y2 = static_cast<int>(std::lround(sz.y * kPlotBottomRatio));
    const int side = std::max(1, std::min(x2 - x, y2 - y));
    return wxRect(x, y, side, side);
}

void draw_mixed_gradient_plot(wxDC& raw_dc, const wxSize& canvas,
                              const std::vector<MixedGradientCurve>& curves,
                              const std::vector<wxPoint2DDouble>& anchors,
                              const MixedGradientTheme& theme)
{
    // Draw into an internal opaque buffer so wxGCDC text/curves anti-alias against a solid
    // background (never a transparent one), then blit the finished image onto the caller's
    // buffered paint DC. wxGCDC cannot wrap a generic wxDC&, so the buffer is always a
    // wxMemoryDC -- the one type wxGCDC accepts on every platform.
    if (canvas.x <= 0 || canvas.y <= 0)
        return;
    const wxRect rc = mixed_gradient_plot_rect(canvas);
    if (rc.width <= 0 || rc.height <= 0)
        return;

    wxBitmap buf(canvas);
    wxMemoryDC memdc(buf);
    memdc.SetBackground(wxBrush(theme.background));
    memdc.Clear();
    wxGCDC dc(memdc);
    wxGraphicsContext* gc = dc.GetGraphicsContext();

    // 10x10 light grid (10 lines including outer borders, 9 equal divisions).
    dc.SetPen(wxPen(theme.grid, 1));
    for (int i = 0; i <= kGridDivisions; ++i) {
        const int x = rc.x + rc.width  * i / kGridDivisions;
        const int y = rc.y + rc.height * i / kGridDivisions;
        dc.DrawLine(x, rc.y, x, rc.y + rc.height);
        dc.DrawLine(rc.x, y, rc.x + rc.width, y);
    }

    // Set the label font first so text width measurements drive arrow / label placement.
    wxFont label_font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    label_font.SetPointSize(std::max(7, label_font.GetPointSize() - 1));
    dc.SetFont(label_font);

    const wxString axis_y_title = _L("Material Ratio");
    const wxString axis_x_title = _L("Model Height");
    const wxString pct_text     = wxT("100%");
    const wxSize   x_title_sz   = dc.GetTextExtent(axis_x_title);
    const wxSize   y_title_sz   = dc.GetTextExtent(axis_y_title);

    wxFont strong_font = label_font;
    strong_font.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    dc.SetFont(strong_font);
    const wxSize pct_text_sz = dc.GetTextExtent(pct_text);
    dc.SetFont(label_font);

    // Axes (grey 700) with filled triangle arrows. Y-axis extends above the plot top to the
    // canvas top edge; X-axis extends past the plot right toward the canvas right edge.
    const int arrow_half = dip_px(kAxisArrowHalf);
    const int arrow_len  = dip_px(kAxisArrowLen);
    dc.SetPen(wxPen(theme.axis, kStrokeAxis));
    dc.SetBrush(wxBrush(theme.axis));

    const int y_axis_x           = rc.x;
    const int y_title_pct_gap    = dip_px(1);
    const int y_title_bottom_pad = dip_px(2);
    const int y_title_y = std::max(0, rc.y - y_title_sz.y - y_title_pct_gap - pct_text_sz.y - y_title_bottom_pad);
    const int y_arrow_tip_y = y_title_y;
    const int y_arrow_ty    = y_arrow_tip_y + arrow_len;
    dc.DrawLine(y_axis_x, y_arrow_ty, y_axis_x, rc.y + rc.height);
    {
        wxPoint tri[3] = {
            wxPoint(y_axis_x,              y_arrow_tip_y),
            wxPoint(y_axis_x - arrow_half, y_arrow_ty),
            wxPoint(y_axis_x + arrow_half, y_arrow_ty),
        };
        dc.DrawPolygon(3, tri);
    }

    const int x_axis_y      = rc.y + rc.height;
    const int x_label_gap   = dip_px(4);
    const int x_edge_pad    = dip_px(6);
    const int x_arrow_ideal = rc.x + rc.width + dip_px(10);
    const int x_arrow_max   = canvas.x - x_title_sz.x - x_label_gap - x_edge_pad - arrow_len;
    const int x_arrow_tx    = std::max(rc.x + rc.width + arrow_len, std::min(x_arrow_ideal, x_arrow_max));
    const int x_arrow_tip_x = x_arrow_tx + arrow_len;
    const int x_title_x     = x_arrow_tip_x + x_label_gap;
    dc.DrawLine(rc.x, x_axis_y, x_arrow_tx, x_axis_y);
    {
        wxPoint tri[3] = {
            wxPoint(x_arrow_tip_x,          x_axis_y),
            wxPoint(x_arrow_tx,             x_axis_y - arrow_half),
            wxPoint(x_arrow_tx,             x_axis_y + arrow_half),
        };
        dc.DrawPolygon(3, tri);
    }

    // Labels: "Material Ratio" and the leading "100%" share the same left x; the trailing
    // "Model Height" follows the X-axis arrow tip (already clamped to make room).
    const int label_left_x = y_axis_x + dip_px(10);
    dc.SetTextForeground(theme.label);
    dc.DrawText(axis_y_title, label_left_x, y_title_y);

    dc.SetFont(strong_font);
    dc.SetTextForeground(theme.label_strong);
    dc.DrawText(pct_text, label_left_x, y_title_y + y_title_sz.y + y_title_pct_gap);

    dc.DrawText(pct_text, rc.x + rc.width - pct_text_sz.x, x_axis_y);
    dc.SetFont(label_font);
    dc.SetTextForeground(theme.label);
    dc.DrawText(axis_x_title, x_title_x, x_axis_y - x_title_sz.y / 2);

    if (!gc)
        return;

    // Outline only when the curve colour is perceptually close to the background; otherwise the
    // plain filament colour reads fine and the extra stroke would look heavy.
    auto needs_outline = [&](const wxColour& c) {
        return calc_color_distance(c, theme.background) < kBgSimilarThreshold;
    };

    // Only the geometry goes through the graphics context: dc.DrawLines() takes integer wxPoint
    // and would quantize the curve back to whole pixels. The pen is still set on the dc, which
    // forwards it here while keeping its own cached state in sync for later dc drawing.
    auto draw_polyline = [&](const MixedGradientCurve& curve) {
        if (curve.points.size() < 2)
            return;
        dc.SetPen(wxPen(curve.colour, dip_px(curve.stroke_dip)));
        gc->StrokeLines(curve.points.size(), curve.points.data());
    };

    for (const MixedGradientCurve& curve : curves) {
        if (needs_outline(curve.colour))
            draw_polyline({curve.points, theme.outline, curve.stroke_dip + kOutlineExtraDip});
        draw_polyline(curve);
    }

    // Control points: hollow circle with axis-colour border, theme-aware fill, drawn with a
    // sub-pixel centre so the ring stays centred on the curve.
    if (!anchors.empty()) {
        const double r = dip_px(kPointRadius);
        dc.SetPen(wxPen(theme.axis, 1));
        dc.SetBrush(wxBrush(theme.point_fill));
        for (const wxPoint2DDouble& p : anchors)
            gc->DrawEllipse(p.m_x - r, p.m_y - r, r * 2, r * 2);
    }

    memdc.SelectObject(wxNullBitmap);
    raw_dc.DrawBitmap(buf, 0, 0);
}

void draw_mixed_ratio_blend_bar(wxDC& dc, const wxRect& rect, const wxColour& first,
                                const wxColour& second, double second_fraction)
{
    if (rect.width <= 0 || rect.height <= 0)
        return;
    for (int x = 0; x < rect.width; ++x) {
        const double t = rect.width > 1 ? double(x) / rect.width : 0.0;
        const wxColour c = lerp_blend(first, second, 1.0 - t);
        dc.SetPen(wxPen(c));
        dc.DrawLine(rect.x + x, rect.y, rect.x + x, rect.y + rect.height);
    }

    // Fixed in both themes, like the triangle picker's drag handle: the divider is drawn over
    // blended filament colour, so it has to keep its contrast against data rather than chrome.
    const int div_x = rect.x + static_cast<int>(second_fraction * rect.width);
    dc.SetPen(wxPen(wxColour(80, 80, 80), dip_px(4)));
    dc.DrawLine(div_x, rect.y, div_x, rect.y + rect.height);
    dc.SetPen(wxPen(*wxWHITE, dip_px(2)));
    dc.DrawLine(div_x, rect.y, div_x, rect.y + rect.height);
}

void draw_mixed_ratio_segments(wxDC& dc, const wxRect& rect, const std::vector<wxColour>& colours,
                               const std::vector<double>& shares)
{
    const size_t n = std::min(colours.size(), shares.size());
    if (n == 0 || rect.width <= 0 || rect.height <= 0)
        return;
    std::vector<double> norm = shares;
    double total = 0.0;
    for (double s : norm)
        total += s;
    if (total <= 0.0) {
        norm.assign(n, 1.0 / n);
        total = 1.0;
    }
    auto share_to_px = [&](double share_sum) { return rect.x + int(std::lround(share_sum / total * double(rect.width))); };
    int x0 = rect.x;
    std::vector<wxRect> segs(n);
    for (size_t i = 0; i < n; ++i) {
        int x1 = rect.x + rect.width;
        if (i + 1 < n)
            x1 = share_to_px(std::accumulate(norm.begin(), norm.begin() + i + 1, 0.0));
        segs[i] = wxRect(x0, rect.y, std::max(1, x1 - x0), rect.height);
        x0      = segs[i].GetRight() + 1;
    }
    for (size_t i = 0; i < n; ++i) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(colours[i]));
        dc.DrawRectangle(segs[i]);
    }
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#ACACAC")), 1));
    dc.DrawRectangle(rect);
}

namespace {

struct TriCacheKey
{
    int w, h;
    int c0r, c0g, c0b, c1r, c1g, c1b, c2r, c2g, c2b;
    int bg_r, bg_g, bg_b, ol_r, ol_g, ol_b;
    bool operator<(const TriCacheKey& o) const
    {
        return std::tie(w, h, c0r, c0g, c0b, c1r, c1g, c1b, c2r, c2g, c2b, bg_r, bg_g, bg_b, ol_r, ol_g, ol_b) <
               std::tie(o.w, o.h, o.c0r, o.c0g, o.c0b, o.c1r, o.c1g, o.c1b, o.c2r, o.c2g, o.c2b, o.bg_r, o.bg_g, o.bg_b, o.ol_r, o.ol_g, o.ol_b);
    }
};

std::map<TriCacheKey, wxBitmap>& tri_cache()
{
    static std::map<TriCacheKey, wxBitmap> cache;
    return cache;
}

} // namespace

std::array<TriPoint, 3> mixed_triangle_vertices(const wxSize& size, double margin_dip)
{
    const double pw = size.GetWidth(), ph = size.GetHeight();
    const double margin = dip_px(int(margin_dip));
    const double avail  = std::min(pw, ph) - 2.0 * margin;
    const double side   = avail;
    const double tri_h  = side * std::sqrt(3.0) / 2.0;
    const double cx     = pw / 2.0;
    const double top_y  = (ph - tri_h) / 2.0;
    return {{{cx, top_y}, {cx - side / 2.0, top_y + tri_h}, {cx + side / 2.0, top_y + tri_h}}};
}

void draw_mixed_triangle_picker(wxDC& dc, const wxSize& size, const std::array<wxColour, 3>& colours,
                                const std::array<double, 3>& weights, const MixedTriangleTheme& theme)
{
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
        return;
    const std::array<TriPoint, 3> v = mixed_triangle_vertices(size, kTriangleMarginDip);

    dc.SetBrush(wxBrush(theme.background));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

    const wxColour& c0 = colours[0];
    const wxColour& c1 = colours[1];
    const wxColour& c2 = colours[2];
    const TriCacheKey key{size.GetWidth(), size.GetHeight(),
                          c0.Red(), c0.Green(), c0.Blue(),
                          c1.Red(), c1.Green(), c1.Blue(),
                          c2.Red(), c2.Green(), c2.Blue(),
                          theme.background.Red(), theme.background.Green(), theme.background.Blue(),
                          theme.outline.Red(), theme.outline.Green(), theme.outline.Blue()};

    wxBitmap& bmp = tri_cache()[key];
    if (!bmp.IsOk()) {
        bmp = wxBitmap(size.GetWidth(), size.GetHeight(), 24);
        wxMemoryDC mdc(bmp);
        mdc.SetBrush(wxBrush(theme.background));
        mdc.SetPen(*wxTRANSPARENT_PEN);
        mdc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

        const int min_y = int(std::min({v[0].y, v[1].y, v[2].y}));
        const int max_y = int(std::max({v[0].y, v[1].y, v[2].y}));
        const int min_x = int(std::min({v[0].x, v[1].x, v[2].x}));
        const int max_x = int(std::max({v[0].x, v[1].x, v[2].x}));
        for (int py = min_y; py <= max_y; ++py) {
            for (int px = min_x; px <= max_x; ++px) {
                const TriPoint p = {double(px), double(py)};
                if (!tri_contains(p, v[0], v[1], v[2]))
                    continue;
                double w0, w1, w2;
                tri_barycentric(p, v[0], v[1], v[2], w0, w1, w2);
                unsigned char mr, mg, mb;
                if (w0 + w1 > 1e-6) {
                    float t01 = float(w1 / (w0 + w1));
                    Slic3r::filament_mixer_lerp(c0.Red(), c0.Green(), c0.Blue(), c1.Red(), c1.Green(), c1.Blue(), t01, &mr, &mg, &mb);
                    Slic3r::filament_mixer_lerp(mr, mg, mb, c2.Red(), c2.Green(), c2.Blue(), float(w2), &mr, &mg, &mb);
                } else {
                    mr = c2.Red(); mg = c2.Green(); mb = c2.Blue();
                }
                mdc.SetPen(wxPen(wxColour(mr, mg, mb)));
                mdc.DrawPoint(px, py);
            }
        }

        mdc.SetPen(wxPen(theme.outline, 1));
        mdc.SetBrush(*wxTRANSPARENT_BRUSH);
        const wxPoint pts[3] = {{int(v[0].x), int(v[0].y)}, {int(v[1].x), int(v[1].y)}, {int(v[2].x), int(v[2].y)}};
        mdc.DrawPolygon(3, pts);
        mdc.SelectObject(wxNullBitmap);

        // Keep the cache from growing without bound across DPI/size changes.
        if (tri_cache().size() > 6) {
            auto& cache = tri_cache();
            cache.erase(cache.begin());
        }
    }
    dc.DrawBitmap(bmp, 0, 0);

    // Published-ratio marker (read-only twin of the editor's drag handle).
    const double w0 = weights[0], w1 = weights[1], w2 = weights[2];
    const int hx = int(w0 * v[0].x + w1 * v[1].x + w2 * v[2].x);
    const int hy = int(w0 * v[0].y + w1 * v[1].y + w2 * v[2].y);
    dc.SetBrush(*wxWHITE_BRUSH);
    dc.SetPen(wxPen(theme.ring, dip_px(2)));
    dc.DrawCircle(hx, hy, dip_px(5));
}

void draw_mixed_triangle_labels(wxDC& dc, const wxSize& size, const std::array<double, 3>& weights,
                                const MixedTriangleTheme& theme)
{
    const std::array<TriPoint, 3> v = mixed_triangle_vertices(size, kTriangleMarginDip);
    dc.SetFont(::Label::Body_12);
    dc.SetTextForeground(theme.label);

    // "Ratio" title, sitting above the top vertex.
    const wxString title = _L("Ratio");
    dc.DrawText(title, dip_px(2), std::max(0, int(v[0].y - dc.GetTextExtent(title).GetHeight() - dip_px(4))));

    for (int i = 0; i < 3; ++i) {
        const wxString text = wxString::Format("%d%%", int(std::lround(weights[i] * 100.0)));
        const wxSize tsz    = dc.GetTextExtent(text);
        int lx = int(v[i].x - tsz.GetWidth() / 2.0);
        int ly = (i == 0) ? int(v[i].y - tsz.GetHeight() - dip_px(4)) : int(v[i].y + dip_px(3));
        ly = std::clamp(ly, 0, size.GetHeight() - tsz.GetHeight());
        lx = std::clamp(lx, 0, size.GetWidth() - tsz.GetWidth());
        dc.DrawText(text, lx, ly);
    }
}

}} // namespace Slic3r::GUI