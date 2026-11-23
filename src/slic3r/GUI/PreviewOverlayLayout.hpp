#pragma once

#include "CanvasUi/Types.hpp"

namespace Slic3r {
namespace GUI {

struct PreviewOverlayInput
{
    float canvas_width{0.f};
    float canvas_height{0.f};
    float scale{1.f};
    float corner_right{0.f};
    float pill_height{0.f};
    float pill_width{0.f};
};

struct PreviewOverlayLayout
{
    float scale{1.f};
    float band{0.f};
    float handle_radius{0.f};
    float track_thickness{0.f};
    float gap{0.f};

    CanvasUi::Rect right_band;
    CanvasUi::Rect bottom_band;

    float       side_rail_x{0.f};
    float       side_rail_top{0.f};
    float       side_rail_bottom{0.f};
    CanvasUi::Rect top_pill;
    CanvasUi::Rect bottom_pill;
    // The squares at both ends of the right band, above and below the layer rail.
    CanvasUi::Rect top_toggle_square;
    CanvasUi::Rect bottom_toggle_square;

    float bottom_rail_y{0.f};
    float bottom_rail_left{0.f};
    float bottom_rail_right{0.f};

    float band_left{0.f};
    float band_right{0.f};
    float band_bottom{0.f};
    float band_center{0.f};

    float overlay_right{0.f};
    float column_top{0.f};
    float overlays_bottom{0.f};

    float corner_offset(float canvas_height) const { return canvas_height - band_bottom; }
    float panel_width(float wanted) const;
    float panel_left(float width) const;
};

PreviewOverlayLayout compute_preview_overlay_layout(const PreviewOverlayInput &input);

} // namespace GUI
} // namespace Slic3r
