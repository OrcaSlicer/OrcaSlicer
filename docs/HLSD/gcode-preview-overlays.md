# G-code preview overlays

The sliced preview draws its controls as ImGui overlays on top of the 3D scene: the layer rail on
the right edge, the move rail along the bottom, the panel describing the current move, the legend,
the G-code window, the 3D navigator and round canvas buttons in the bottom-left corner, and the
notifications. They are drawn by different classes (`IMSlider`, `GCodeViewer`, `GLCanvas3D`,
`NotificationManager`) but must not overlap, so where each one goes is decided in one place.

## One layout per frame

`compute_preview_overlay_layout()` (`src/slic3r/GUI/PreviewOverlayLayout.cpp`) turns the canvas
size, the UI scale, the right edge of the bottom-left corner and the size of a layer pill into a
`PreviewOverlayLayout`: every rectangle and edge the overlays are placed by. `GLCanvas3D` computes
it right after the ImGui frame starts, before anything is drawn, and keeps it for the rest of the
frame, so a reader never depends on where it is drawn in the frame. It is passed down to `GCodeViewer::render_overlay()` and from there to both sliders, and
read back by the corner, the plate list and the notifications. When the preview shows no toolpaths
there is no layout, and everything falls back to where it is in the Prepare view.

```
 +-------------------------------------------------[ top pill ]+[S]+  <- single layer toggle
 |                         +--------------------+               | o |  <- side_rail_top
 |                         | legend             |               |   |
 |                         +--------------------+               |   |
 |                         | G-code window      |               |   |
 |                         +--------------------+               |   |
 |  +----+                                                      |   |
 |  |cube|        +---[ move strip / panel ]---+                | o |  <- side_rail_bottom
 +--+----+--------+----------------------------+-[bottom pill ]-+[S]+  <- single layer toggle
    o===================(o)=====================|                       <- bottom band
```

- The rails sit in a dark L-shaped frame: a band along the full height of the right edge and a band
  along the bottom edge. The bottom pill is the frame's corner piece: it always sits in the
  bottom-right corner, against the canvas edge and the right band, and the bottom band runs from the
  left edge up to it, capped at 1600 logical pixels and centered on that stretch. Because the band
  never runs under the pill, the frame stays whole at any canvas width. Where the band meets the
  pill, a thin vertical rule marks the pill as part of the layer rail rather than the move rail.
- The top pill hangs from the top edge next to the right band; the move panel and the corner stand
  on the bottom band. Only free corners are rounded, and where a tab meets a band the inner corner is
  filled with a concave curve. Where such a curve faces a rounded window across the overlay gap
  (the legend, the G-code window) its radius is that window's radius plus the gap, so the gap
  stays even around the bend.
- The squares at both ends of the right band, next to the pills, hold the single layer toggle; it is
  the same button twice, so it is at hand from either end of the rail. The side rail runs between
  them, half a band clear of each square, and the slider does not take clicks inside them.
  Expanding the move panel never shortens it.
- `overlay_right` is where the legend, the G-code window and the notifications end: a fixed
  distance from the side rail.
- The legend starts under the top pill. The legend and the G-code window under it end above the
  bottom pill, or above the move panel when the panel is expanded under them; the panel's
  rectangle and the windows' own from the previous frame decide that. Notifications always
  stand above the move panel and are drawn over every other overlay, the G-code window included.
- The mouse wheel moves a slider over its band and over everything docked to it: the layer
  slider over its pills, the move slider over the move panel.

The corner (navigator and canvas buttons) is raised by `GLCanvas3D::preview_corner_offset()`. The
offset does not depend on whether the move panel is expanded, so the navigator never jumps.

## The move panel

The panel stands on the bottom band in the gap between the corner and the bottom pill. It is
centered on the bottom rail's axis and pushed back into the gap when it would reach the corner or
the pill. Collapsed, it shows the move number, position, speed and the value of the color scheme;
expanded, it adds the elapsed time to that row and opens the property groups above it, so the row
always stays next to the rail. The expanded panel is only as wide as its content.

Every number sits in a slot as wide as the largest value of the current print, measured with
`CalcTextSize`, and is right-aligned in it: the move count of the layer, the total print time, the
print's extent on each axis relative to the plate, its top speed, its last layer and height in the
pills. Nothing shifts or flickers while a handle is dragged, and a small print keeps short labels.

When the band is too narrow, `fit_move_panel()` decides what is shown. The speed profile goes
first, together with its button. After that the header row and the body shrink independently: the
body moves the groups that do not fit behind a `[N more]` button, the header row drops the elapsed
time and then the X/Y/Z position. The move number, the speed, the filament of the move (only when
the print uses more than one) and the value of the current color scheme are always shown.

## Marks on the layer rail

Pauses, color changes, filament changes and custom G-code are drawn on the rail by type. Marks
closer than a line of text on screen are merged by `group_ticks_by_position()` into a badge with
their count; the grouping is recomputed every frame, so a taller window can split a badge again.
Hovering a mark or a badge opens a list of its marks. The list stays open while the pointer is over
the mark, over the list, or in the gap between them, and its actions reuse the code of the handle's
context menus.
