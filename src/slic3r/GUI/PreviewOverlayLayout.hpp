#pragma once

#include <array>
#include <cstddef>

#include "CanvasUi/Types.hpp"
#include "PreviewPanelSlots.hpp"

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
    float plates_right{0.f};
    float corner_height{0.f};
    float top_left_reserved{0.f};
};

struct PreviewOverlayLayout
{
    float scale{1.f};
    float band{0.f};
    float handle_radius{0.f};
    float track_thickness{0.f};
    float gap{0.f};
    // The header of a panel of a column, which a panel above it leaves room for.
    float panel_header{0.f};

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
    float left_column_left{0.f};
    float left_column_top{0.f};
    float left_column_bottom{0.f};

    float corner_offset(float canvas_height) const { return canvas_height - band_bottom; }
    float panel_width(float wanted) const;
    float panel_left(float width) const;
    // The bottom of a panel over left..right, kept a gap above the move panel when that comes within a gap of it.
    float bottom_clear_of(const CanvasUi::Rect &move_panel, float left, float right, float bottom) const;
    // The heights are those of the last frame; whether the G-code panel gives way to a panel under it
    // goes by that panel being shown, as a panel just shown has no height yet.
    std::array<PreviewPanelPlacement, PREVIEW_PANEL_COUNT> place_panels(const PreviewPanelSlots &slots, const PreviewPanelHeights &heights,
                                                                        const PreviewPanelShown &shown) const;
};

PreviewOverlayLayout compute_preview_overlay_layout(const PreviewOverlayInput &input);

} // namespace GUI
} // namespace Slic3r
