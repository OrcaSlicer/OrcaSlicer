#include "MixedFilamentBadge.hpp"

#include "Widgets/Label.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "BitmapCache.hpp"
#include "libslic3r/MixedFilament.hpp"

#include <wx/image.h>
#include <cstring>

namespace Slic3r { namespace GUI {

wxColour interpolate_color(const std::vector<wxColour>& colors, double pos)
{
    if (colors.empty()) return *wxWHITE;
    if (colors.size() == 1) return colors[0];

    pos = std::max(0.0, std::min(1.0, pos));

    double segment_size = 1.0 / (colors.size() - 1);
    int segment = int(pos / segment_size);
    segment = std::min(segment, int(colors.size()) - 2);

    double local_pos = (pos - segment * segment_size) / segment_size;

    const wxColour& c1 = colors[segment];
    const wxColour& c2 = colors[segment + 1];

    int r = int(c1.Red()   * (1.0 - local_pos) + c2.Red()   * local_pos);
    int g = int(c1.Green() * (1.0 - local_pos) + c2.Green() * local_pos);
    int b = int(c1.Blue()  * (1.0 - local_pos) + c2.Blue()  * local_pos);

    return wxColour(r, g, b);
}

// Check whether a pixel lies inside a rounded rectangle.
static inline bool pixelInRoundedRect(int x, int y, int w, int h, const CornerRadius& r)
{
    // --- Full-circle fast path --------------------------------------------
    const bool isCircle = (w == h)
        && (r.m_topLeft == r.m_topRight)
        && (r.m_topLeft == r.m_bottomRight)
        && (r.m_topLeft == r.m_bottomLeft)
        && (r.m_topLeft * 2 == w);

    if (isCircle) {
        // Pixel centre (x+½, y+½) against a circle centred at (w/2, h/2).
        // Multiply the inequality by 4 to stay in integer arithmetic:
        //   (2x+1 - 2r)² + (2y+1 - 2r)²  ≤  4r²
        const int R  = r.m_topLeft;
        const int dx = 2 * x + 1 - 2 * R;
        const int dy = 2 * y + 1 - 2 * R;
        return dx * dx + dy * dy <= 4 * R * R;
    }

    // --- Rounded-rectangle path -------------------------------------------
    // Top-left corner
    if (x < r.m_topLeft && y < r.m_topLeft) {
        int dx = x - r.m_topLeft;
        int dy = y - r.m_topLeft;
        return dx * dx + dy * dy <= r.m_topLeft * r.m_topLeft;
    }
    // Top-right corner
    if (x >= w - r.m_topRight && y < r.m_topRight) {
        int dx = (x + 1) - (w - r.m_topRight);
        int dy = y - r.m_topRight;
        return dx * dx + dy * dy <= r.m_topRight * r.m_topRight;
    }
    // Bottom-right corner
    if (x >= w - r.m_bottomRight && y >= h - r.m_bottomRight) {
        int dx = (x + 1) - (w - r.m_bottomRight);
        int dy = (y + 1) - (h - r.m_bottomRight);
        return dx * dx + dy * dy <= r.m_bottomRight * r.m_bottomRight;
    }
    // Bottom-left corner
    if (x < r.m_bottomLeft && y >= h - r.m_bottomLeft) {
        int dx = x - r.m_bottomLeft;
        int dy = (y + 1) - (h - r.m_bottomLeft);
        return dx * dx + dy * dy <= r.m_bottomLeft * r.m_bottomLeft;
    }
    return true;
}

MixedFilamentBadge::MixedFilamentBadge(wxWindow* parent, wxWindowID id, int virtual_id,
                                       const MixedFilament& mf,
                                       const MixedFilamentDisplayContext& display_context,
                                       bool show_number, int badge_size)
    : wxPanel(parent, id, wxDefaultPosition, wxSize(badge_size, badge_size), wxBORDER_NONE)
    , m_show_number(show_number)
    , m_label(wxString::Format("%d", virtual_id))
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetSize(parent->FromDIP(wxSize(badge_size, badge_size)));
    SetMinSize(parent->FromDIP(wxSize(badge_size, badge_size)));
    SetMaxSize(parent->FromDIP(wxSize(badge_size, badge_size)));
    Bind(wxEVT_PAINT, &MixedFilamentBadge::on_paint, this);
    Bind(wxEVT_LEFT_UP, &MixedFilamentBadge::on_left_up, this);
    SetCursor(wxCursor(wxCURSOR_ARROW));

    SetFont(badge_size >= 20 ? Label::Body_12 : Label::Body_8);

    m_solid_color = parse_mixed_color(mf.display_color);

    m_is_gradient = is_simple_gradient(mf);

    if (m_is_gradient) {
        auto get_color = [&](unsigned fid) -> wxColour {
            if (fid == 0 || fid > display_context.physical_colors.size()) return wxColour("#26A69A");
            return parse_mixed_color(display_context.physical_colors[fid - 1]);
        };
        const wxColour ca = get_color(mf.component_a);
        const wxColour cb = get_color(mf.component_b);
        const bool a_to_b = mf.gradient_start >= mf.gradient_end;
        m_gradient_colors.push_back(a_to_b ? ca : cb);
        m_gradient_colors.push_back(a_to_b ? cb : ca);
    }

    // Font color: for gradient use average luminance of the two endpoint colors
    if (m_is_gradient && m_gradient_colors.size() >= 2) {
        double lum0 = m_gradient_colors[0].GetLuminance();
        double lum1 = m_gradient_colors[1].GetLuminance();
        double avg_lum = (lum0 + lum1) * 0.5;
        SetForegroundColour(avg_lum < 0.51 ? *wxWHITE : *wxBLACK);
    } else {
        SetForegroundColour(m_solid_color.GetLuminance() < 0.51 ? *wxWHITE : *wxBLACK);
    }
}

void MixedFilamentBadge::on_paint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    wxRect rect = GetClientRect();

    if (m_is_gradient && m_gradient_colors.size() >= 2) {
        // Fill background with first gradient color to prevent black-edge artifacts.
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_gradient_colors[0]));
        dc.DrawRectangle(rect);

        // Draw gradient from bottom to top
        for (int y = rect.GetBottom(); y >= rect.GetTop(); --y) {
            double pos = double(rect.GetBottom() - y) / double(rect.GetHeight());
            wxColour color = interpolate_color(m_gradient_colors, pos);
            dc.SetPen(wxPen(color));
            dc.DrawLine(rect.GetLeft(), y, rect.GetLeft() + rect.GetWidth(), y);
        }

        // Draw grey border only if BOTH endpoint colors are very light (R/G/B all > 224)
        const auto& c0 = m_gradient_colors[0];
        const auto& c1 = m_gradient_colors[1];
        bool both_light = (c0.Red() > 224 && c0.Green() > 224 && c0.Blue() > 224)
                       && (c1.Red() > 224 && c1.Green() > 224 && c1.Blue() > 224);
        if (both_light) {
            dc.SetPen(*wxGREY_PEN);
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(rect);
        }
    } else {
        // Draw solid color
        dc.SetBrush(wxBrush(m_solid_color));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(rect);

        // Draw grey border for very light colors (R/G/B all > 224)
        if (m_solid_color.Red() > 224 && m_solid_color.Green() > 224 && m_solid_color.Blue() > 224) {
            dc.SetPen(*wxGREY_PEN);
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(rect);
        }
    }

    // Draw text — compute color using same luminance rule as constructor
    if (m_show_number) {
        wxString label = m_label;
        wxFont font = GetFont();
        dc.SetFont(font);

        double text_lum;
        if (m_is_gradient && m_gradient_colors.size() >= 2) {
            double lum0 = m_gradient_colors[0].GetLuminance();
            double lum1 = m_gradient_colors[1].GetLuminance();
            text_lum = (lum0 + lum1) * 0.5;
        } else {
            text_lum = m_solid_color.GetLuminance();
        }
        dc.SetTextForeground(text_lum < 0.51 ? *wxWHITE : *wxBLACK);

        dc.DrawLabel(label, rect, wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL);
    }
}

void MixedFilamentBadge::on_left_up(wxMouseEvent&)
{
    wxCommandEvent evt(wxEVT_BUTTON, GetId());
    evt.SetEventObject(this);
    ProcessWindowEvent(evt);
}

// ---------------------------------------------------------------------------
// get_color_block_bitmap_cached — unified cached bitmap factory
// ---------------------------------------------------------------------------

wxBitmap* get_color_block_bitmap_cached(const ColorBlockParams& params)
{
    wxASSERT(wxIsMainThread());
    static BitmapCache cache;

    // Build a deterministic cache key.
    std::string key;
    if (params.mode == ColorBlockParams::Gradient && params.gradient_colors.size() >= 2) {
        key = "grad:";
        key += params.gradient_colors[0].GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
        for (size_t i = 1; i < params.gradient_colors.size(); ++i) {
            key += ":";
            key += params.gradient_colors[i].GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
        }
        key += "BT:h" + std::to_string(params.height)
            + ":w" + std::to_string(params.width)
            + ":" + params.label.ToStdString();
    } else {
        key = "solid:";
        key += params.solid_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
        key += ":h" + std::to_string(params.height)
            + ":w" + std::to_string(params.width)
            + ":" + params.label.ToStdString();
    }

    wxBitmap* cached = cache.find(key);
    if (cached != nullptr)
        return cached;

    wxBitmap bmp(params.width, params.height);
    wxMemoryDC dc;
    dc.SelectObject(bmp);
    const bool use_small_font = std::min(params.width, params.height) < 20;
    dc.SetFont(use_small_font ? ::Label::Body_8 : ::Label::Body_12);

    bool very_light = false;

    if (params.mode == ColorBlockParams::Gradient && params.gradient_colors.size() >= 2) {
        const auto& stops = params.gradient_colors;
        // Fill background to prevent black-edge artifacts on partial paints.
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(stops[0]));
        dc.DrawRectangle(0, 0, params.width, params.height);

        for (int y = params.height - 1; y >= 0; --y) {
            double pos = double(params.height - 1 - y) / double(std::max(1, params.height - 1));
            wxColour col = interpolate_color(stops, pos);
            dc.SetPen(wxPen(col));
            dc.DrawLine(0, y, params.width, y);
        }

        very_light = true;
        for (const auto& c : stops) {
            if (c.Red() <= 224 || c.Green() <= 224 || c.Blue() <= 224) {
                very_light = false;
                break;
            }
        }

        double avg_lum = 0;
        for (const auto& c : stops) avg_lum += c.GetLuminance();
        avg_lum /= stops.size();
        dc.SetTextForeground(avg_lum < 0.51 ? *wxWHITE : *wxBLACK);
    } else {
        const wxColour& clr = params.solid_color;
        dc.SetBackground(wxBrush(clr));
        dc.Clear();
        dc.SetBrush(wxBrush(clr));
        very_light = (clr.Red() > 224 && clr.Green() > 224 && clr.Blue() > 224);
        dc.SetTextForeground(clr.GetLuminance() < 0.51 ? *wxWHITE : *wxBLACK);
    }

    if (very_light) {
        dc.SetPen(*wxGREY_PEN);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, params.width, params.height);
    }

    dc.DrawLabel(params.label, wxRect(0, 0, params.width, params.height), wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL);
    dc.SelectObject(wxNullBitmap);

    return cache.insert(key, bmp);
}

wxBitmap* get_color_block_bitmap_cached(const std::vector<wxColour>& colors, bool is_gradient,
                                        int width, int height, const wxString& label,
                                        const wxColour& lightBorderColor,
                                        const CornerRadius& radius)
{
    wxASSERT(wxIsMainThread());
    static BitmapCache cache;

    const bool useRadius = !radius.IsZero();

    width  = std::max(1, width);
    height = std::max(1, height);

    std::vector<wxColour> drawColors;
    drawColors.reserve(colors.empty() ? 1 : colors.size());
    if (colors.empty())
    {
        drawColors.emplace_back(wxColour("#26A69A"));
    }
    else
    {
        for (const wxColour& color : colors)
            drawColors.emplace_back(color.IsOk() ? color : wxColour("#26A69A"));
    }

    const bool useGradient = is_gradient && drawColors.size() > 1;
    std::string key = useGradient ? "official-grad:" : "official-seg:";
    key += "h" + std::to_string(height) + ":w" + std::to_string(width) + ":" + label.ToStdString();
    for (const wxColour& color : drawColors)
    {
        key += ":";
        key += color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    }
    if (lightBorderColor.IsOk())
    {
        key += ":border:";
        key += lightBorderColor.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    }

    if (useRadius) {
        key += ":r";
        key += std::to_string(radius.m_topLeft) + ",";
        key += std::to_string(radius.m_topRight) + ",";
        key += std::to_string(radius.m_bottomLeft) + ",";
        key += std::to_string(radius.m_bottomRight);
    }

    wxBitmap* cached = cache.find(key);
    if (cached != nullptr)
        return cached;

    const int bmpDepth = useRadius ? 32 : -1;
    wxBitmap bmp(width, height, bmpDepth);
    wxMemoryDC dc;
    dc.SelectObject(bmp);
    dc.SetPen(*wxTRANSPARENT_PEN);
    const bool useSmallFont = std::min(width, height) < 20;
    dc.SetFont(useSmallFont ? ::Label::Body_8 : ::Label::Body_12);

    if (drawColors.size() <= 1)
    {
        dc.SetBackground(wxBrush(drawColors.front()));
        dc.Clear();
        dc.SetBrush(wxBrush(drawColors.front()));
        dc.DrawRectangle(0, 0, width, height);
    }
    else if (useGradient)
    {
        dc.SetBrush(wxBrush(drawColors.front()));
        dc.DrawRectangle(0, 0, width, height);
        const int segmentCount = static_cast<int>(drawColors.size()) - 1;
        int left = 0;
        for (int index = 0; index < segmentCount; ++index)
        {
            const int right = index == segmentCount - 1 ? width : width * (index + 1) / segmentCount;
            const int segmentWidth = right - left;
            if (segmentWidth > 0)
                dc.GradientFillLinear(wxRect(left, 0, segmentWidth, height), drawColors[static_cast<size_t>(index)],
                                      drawColors[static_cast<size_t>(index + 1)], wxEAST);
            left = right;
        }
    }
    else
    {
        const int colorCount = static_cast<int>(drawColors.size());
        int left = 0;
        for (int index = 0; index < colorCount; ++index)
        {
            const int right = index == colorCount - 1 ? width : width * (index + 1) / colorCount;
            const int segmentWidth = right - left;
            dc.SetBrush(wxBrush(drawColors[static_cast<size_t>(index)]));
            if (segmentWidth > 0)
                dc.DrawRectangle(left, 0, segmentWidth, height);
            left = right;
        }
    }

    auto colorIsVeryLight = [](const wxColour& color) -> bool
    {
        return color.Red() > 224 && color.Green() > 224 && color.Blue() > 224;
    };

    bool veryLight = true;
    if (!useGradient && drawColors.size() > 1)
    {
        veryLight = false;
        for (const wxColour& color : drawColors)
        {
            if (colorIsVeryLight(color))
            {
                veryLight = true;
                break;
            }
        }
    }
    else
    {
        for (const wxColour& color : drawColors)
        {
            if (!colorIsVeryLight(color))
            {
                veryLight = false;
                break;
            }
        }
    }

    if (veryLight)
    {
        dc.SetPen(lightBorderColor.IsOk() ? wxPen(lightBorderColor, 1) : *wxGREY_PEN);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, width, height);
    }

    if (!label.empty())
    {
        double averageLuminance = 0.0;
        for (const wxColour& color : drawColors)
            averageLuminance += color.GetLuminance();
        averageLuminance /= static_cast<double>(drawColors.size());
        dc.SetTextForeground(averageLuminance < 0.51 ? *wxWHITE : *wxBLACK);
        dc.DrawLabel(label, wxRect(0, 0, width, height), wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL);
    }

    dc.SelectObject(wxNullBitmap);

    // Rounded corners: set alpha=0 for pixels outside the rounded rectangle
    if (useRadius) {
        wxImage img = bmp.ConvertToImage();
        img.InitAlpha();
        unsigned char* alpha = img.GetAlpha();
        // Start with alpha=255 everywhere, then clear corners
        memset(alpha, 255, static_cast<size_t>(width) * height);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if (!pixelInRoundedRect(x, y, width, height, radius))
                    alpha[y * width + x] = 0;
            }
        }
        bmp = wxBitmap(img, 32);
    }

    return cache.insert(key, bmp);
}

// ---------------------------------------------------------------------------
// Free function — shared between badge and merge menus
// ---------------------------------------------------------------------------

wxBitmap* create_mixed_filament_menu_bitmap(const MixedFilament&               mf,
                                           const MixedFilamentDisplayContext& ctx,
                                           int  width, int  height,
                                           const wxString& label)
{
    ColorBlockParams params;
    params.width  = width;
    params.height = height;
    params.label  = label;

    const bool is_gradient = is_simple_gradient(mf);

    if (is_gradient) {
        auto get_c = [&](unsigned fid) -> wxColour {
            if (fid == 0 || fid > ctx.physical_colors.size())
                return wxColour("#26A69A");
            return parse_mixed_color(ctx.physical_colors[fid - 1]);
        };
        const wxColour ca = get_c(mf.component_a);
        const wxColour cb = get_c(mf.component_b);
        const bool a_to_b = mf.gradient_start >= mf.gradient_end;

        params.mode = ColorBlockParams::Gradient;
        params.gradient_colors.push_back(a_to_b ? ca : cb);
        params.gradient_colors.push_back(a_to_b ? cb : ca);
    } else {
        params.mode = ColorBlockParams::Solid;
        params.solid_color = parse_mixed_color(mf.display_color.empty() ? "#808080" : mf.display_color);
    }

    return get_color_block_bitmap_cached(params);
}

}} // namespace Slic3r::GUI
