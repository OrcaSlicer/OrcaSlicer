#include "PreviewOverlayLayout.hpp"

#include <algorithm>
#include <cmath>

#include "CanvasUi/Metrics.hpp"
#include "CanvasUi/Tokens.hpp"
#include "CanvasUi/Types.hpp"

namespace Slic3r {
namespace GUI {

static constexpr auto RAIL_BAND      = CanvasUi::space<28>();
static constexpr auto GAP            = CanvasUi::space<8>();
static constexpr auto RAIL_MAX_WIDTH = CanvasUi::space<1600>();
// The rails' handle and track, off the spacing scale.
static constexpr float HANDLE_RADIUS   = 9.f;
static constexpr float TRACK_THICKNESS = 5.f;

float PreviewOverlayLayout::panel_width(float wanted) const
{
    return std::max(0.f, std::min(wanted, band_right - band_left));
}

float PreviewOverlayLayout::panel_left(float width) const
{
    return std::round(std::clamp(band_center - 0.5f * width, band_left, std::max(band_left, band_right - width)));
}

PreviewOverlayLayout compute_preview_overlay_layout(const PreviewOverlayInput &input)
{
    const float             s = input.scale;
    const CanvasUi::Metrics metrics(s);

    PreviewOverlayLayout l;
    l.scale           = s;
    l.band            = std::max(std::round(metrics.px(RAIL_BAND)), std::ceil(input.pill_height));
    l.handle_radius   = HANDLE_RADIUS * s;
    l.track_thickness = TRACK_THICKNESS * s;
    l.gap             = metrics.px(GAP);

    const float W = input.canvas_width;
    const float H = input.canvas_height;

    l.right_band  = {W - l.band, 0.f, W, H};
    const float pill_w = std::ceil(input.pill_width);
    l.top_pill         = {W - l.band - pill_w, 0.f, W - l.band, l.band};
    l.bottom_pill      = {W - l.band - pill_w, H - l.band, W - l.band, H};

    const float rail_span = l.bottom_pill.left;
    const float rail_w    = std::min(rail_span, std::round(metrics.px(RAIL_MAX_WIDTH)));
    l.band_center         = 0.5f * rail_span;
    l.bottom_band         = {std::round(l.band_center - 0.5f * rail_w), H - l.band, 0.f, H};
    l.bottom_band.right   = l.bottom_band.left + rail_w;

    l.side_rail_x       = W - 0.5f * l.band;
    l.bottom_rail_y     = H - 0.5f * l.band;
    l.bottom_rail_left  = l.bottom_band.left + 0.5f * l.band;
    l.bottom_rail_right = l.bottom_band.right - 0.5f * l.band;

    l.top_toggle_square    = {W - l.band, 0.f, W, l.band};
    l.bottom_toggle_square = {W - l.band, H - l.band, W, H};
    l.side_rail_top        = l.top_toggle_square.bottom + 0.5f * l.band;
    l.side_rail_bottom     = l.bottom_toggle_square.top - 0.5f * l.band;

    l.band_bottom     = H - l.band;
    l.band_left       = input.corner_right + l.gap;
    l.band_right      = std::max(l.band_left, l.bottom_pill.left - l.gap);
    l.overlay_right   = l.right_band.left - l.gap;
    l.column_top      = l.top_pill.bottom + l.gap;
    l.overlays_bottom = l.bottom_pill.top - l.gap;
    return l;
}

} // namespace GUI
} // namespace Slic3r
