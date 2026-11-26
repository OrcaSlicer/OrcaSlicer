# G-code preview overlays

The sliced preview draws its controls as ImGui overlays on top of the 3D scene: the layer rail on
the right edge, the move rail along the bottom, the panel describing the current move, the legend,
the G-code window, the 3D navigator and the section view button in the bottom-left corner, and the
notifications. They are drawn by different classes (`IMSlider`, `GCodeViewer`, `GCodePanel`,
`Navigator3D`, `GLCanvas3D`, `NotificationManager`) but must not overlap, so where each one goes is
decided in one place.

## One layout per frame

`compute_preview_overlay_layout()` (`src/slic3r/GUI/PreviewOverlayLayout.cpp`) turns a
`PreviewOverlayInput` — the canvas size, the UI scale, the right edge and the height of the
bottom-left corner, the size of a layer pill, the right edge of the plate list and the room the
sidebar collapse button takes at the top left — into a `PreviewOverlayLayout`: every rectangle and
edge the overlays are placed by. `GLCanvas3D::_update_preview_layout()` computes it right after the
ImGui frame starts, before anything is drawn, and the canvas keeps it for the rest of the
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
 |  |cube|        +-------[ move panel ]-------+                | o |  <- side_rail_bottom
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
- `overlay_right` is where the right column and the notifications end: a fixed distance from the
  side rail.
- The legend, the print summary and the G-code window are the panels of the layout. Each stands in one of two columns,
  chosen in View → Preview Layout, and is kept in the app config as a `PreviewPanelSlot`: a column
  and an order inside it, under `legend_slot`, `print_summary_slot` and `gcode_panel_slot`; panels of
  equal order stand as `preview_panel_rank()` ranks them, the legend, the summary, then the G-code window,
  so a settings file without `print_summary_slot` stacks the three without a migration. The submenu
  (`append_preview_layout_menu()` in `PreviewLayoutMenu.cpp`) only calls the slot operations of
  `PreviewPanelSlots.hpp`: `move_panel_across()`, `gcode_panel_first()` and `put_gcode_panel_first()`.
  `load_preview_panel_slots()` and `save_preview_panel_slots()` read and write a slot for every panel up to
  `PreviewPanel::Count`. Beside its slot, whether each panel is collapsed is kept under `legend_collapsed`,
  `print_summary_collapsed` and `gcode_panel_collapsed`, as `1` or `0`; a value missing or not understood
  reads as expanded. The right
  column ends at `overlay_right` and above the bottom pill. The left column starts past the plate
  list and ends above the bottom-left corner. The right column starts at `column_top`, under the
  top pill; the left one at the same height, or under the sidebar collapse button when the
  sidebar is docked on the left and the button stands in that corner.
- `PreviewOverlayLayout::place_panels()` stacks the panels of each column in their order, each one a
  gap under the one before, by the heights the panels had when last drawn; a hidden panel takes no
  room. `GCodeViewerPanels.cpp` holds the viewer's side of the panels — their slots, where each was
  drawn last, the G-code window's line colours and jump to a line — and `PreviewPanels` the legend, the
  print summary, the statistics of all plates and what they point at, so `GCodeViewer.cpp` only calls them.
  `PreviewPanels` reads the viewer's result and sliders as its friend rather than through accessors that
  would widen the upstream class. `GCodeViewer::render_overlay()` places and draws the legend, then the print summary, and places
  the panels again with their fresh heights before it draws the G-code window, so the default layout
  (the G-code window under both) follows panels that resize within the same frame. Any other
  order follows one frame later, and a change of height asks for that frame.
- The G-code window is as tall as the number of lines its settings ask for, whole lines only; with
  Auto it fills its column down to the bottom and cuts its last line there, so its bottom does not
  move when the settings above the code change height. When another shown panel stands under it, the window gets half of the
  room left, so the panel under it stays visible. Whether a panel stands under it goes by the panel being
  shown, not by its last height, which a panel just shown does not have yet.
- A panel ends above the move panel as soon as the move panel comes within the column gap of it
  (`PreviewOverlayLayout::bottom_clear_of`): a move panel that only grazes the column would otherwise
  shorten the G-code window by whole lines as its width changes. The move panel's rectangle and the panel's own from the previous frame decide that. Notifications always stand above the move
  panel and are drawn over every other overlay, the G-code window included.
- The mouse wheel moves a slider over its band and over everything docked to it: the layer
  slider over its pills, the move slider over the move panel.

The corner (the navigator and the section view button, see [3d-navigator.md](3d-navigator.md)) is raised by `GLCanvas3D::preview_corner_offset()`. The
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

When the band is too narrow, `fit_move_panel()` (`MovePanelLayout.cpp`) decides what is shown. The speed profile goes
first, together with its button. After that the header row and the body shrink independently: the
body moves the groups that do not fit behind a `[N more]` button, the header row drops the elapsed
time and then the X/Y/Z position. The move number, the speed, the filament of the move (only when
the print uses more than one) and the value of the current color scheme are always shown.

## The legend

The legend (`LegendPanel`) is the key to the colours of the toolpaths in the current view, and the
place to hide and show what they stand for. It is a card 300 logical pixels wide, as the G-code
window and the print summary, in every mode and language; its height is bounded by the room in its
column, its body scrolls, and its header stays in place. Collapsed, it is its header alone.
`PreviewPanels::render_legend_panel()` draws it with a fresh `LegendModel`, which
`PreviewPanels::legend_model()` builds each frame from the viewer, the print statistics and the plater —
in millimetres, grams and seconds, so the panel alone formats them, once, in metric or imperial units
and in the interface language (`LegendText`). `PreviewPanels` only reads the viewer and feeds plain samples
to the functions of `LegendModel.cpp`, which gather a scale's values and histogram, the share at a limit, the
range filter's share and the focus range, so they are tested without the viewer. The scale is drawn by
`LegendScaleView`: the bar, the histogram, the marks, the brush, the filter row, the limits and their note,
and the exact-range popover. The legend's settings are fields of the toolkit's settings form.

- Twelve modes in five groups stand for libvgcode's view types. Speed, flow and layer
  time each stand for two `libvgcode::EViewType`s, picked by a toggle group inside the mode; the
  preference keys keep the view type names, so older versions read them. The summary view is not
  offered: a saved `summary` reads as the filament view wherever a view is read from the preferences, and
  stays as it is in the config, so going back to an older version keeps the user's choice. The tool view
  has no entry but is still drawn. Speed, acceleration and jerk have an Include travel switch, at the right of
  the variants' row, or on a row of its own under the variants when the two do not fit or the values are not
  a scale.
- A row is the switch for what it stands for: a click hides or shows it, Alt+click shows it alone, and
  a hidden row is muted with a hollow swatch. The moves are toggles in a flow under the line types, as a
  chart's legend lists its series, their numbers in the tooltip. Hiding does not depend on the mode,
  and choosing a mode does not reset it. The count in the header counts the line types hidden and the
  moves hidden that are shown by default (only seams); a click brings back exactly that default. Being
  in the header, it moves nothing when something is hidden. Rows have no fill for their share: the
  share column tells it, and a row's background is for hover and selection.
- Each colour keeps what it adds up to: a line type its time, share of the print and filament mass,
  a move its time and count, a filament its mass. Everything else the print takes is in the print
  summary.
- A scale mode lays out its values one of three ways. One value in the whole result is a line; up to
  four are categories with the layers they occur on, their time and share; more are a histogram over a
  horizontal bar in libvgcode's palette. Values are told apart as the preview colours them, at two decimals
  more than the mode shows (`legend_value_key()`); categories are labelled with the fewest decimals, from the
  mode's up to those two more, that keep their labels apart, and values whose labels cannot be told apart
  are shown as a scale. The unit stands with the values: after the one value, in the heading
  of the categories' values, among the labels of the scale. Telling them apart and counting the histogram need a pass over
  every vertex, with the same selection libvgcode's colour ranges use (`LegendValueScan`); `update_legend_values()` keeps its
  result until the result, the view, the time mode, the range of the scale, the layers it spans or what
  the ranges count changes.
- The histogram counts print time, or layers on a scale of layer times, in fine bins on round edges
  (`legend_bin_edges`); the panel merges them into the narrowest round bins that fit its width
  (`legend_merge_bins`). Prints sit at a few values (a speed per line type), so the axis is piecewise:
  bins with print time share the width, bins under 2 % take a quarter, and a run of empty or nearly empty
  bins shrinks to a sliver; a bar is at most 24 logical pixels wide, and grows with the square root of
  its share so that small ones stay visible. Labels are the round ticks the stretched axis leaves room
  for, the scale's ends and its unit; the amounts above the bars from 5 % that fit, and every bar's in
  its tooltip. No mean is shown: on a print at a few levels it falls between them.
- While the sliders show less than the whole print, a mark on the scale gives the value of the last
  move shown. On a scale of flow the filament's maximum volumetric speed, and on one of layer times its
  slow-down time, are dashed lines, one per distinct value among the filaments used, read from the
  settings the plate was sliced with, or a G-code file's own. Their labels stand at the top of the
  histogram beside the line, or above the bar when no histogram is drawn. A note under the scale tells the
  share of print time at the flow limit as a warning, or says in a neutral note that no part of the print
  reaches it; on a scale of layer times it tells the layers slowed down.
- A brush on the histogram filters the scale: hovering a bar shows its values in the preview, a click keeps
  them, Shift+click adds them, a drag keeps the bars it spans; the edges of the range then drag on their own,
  and a double click on one opens fields for exact values. The paths outside the range are hidden (see Path
  focus below), the bars outside it go grey, and a row tells the share of print time inside, or of the
  layers on a scale of layer times, with a reset. The range drops with another view, however it comes, and
  with a new result, whose scale is another, and whenever the legend does not show it — hidden, collapsed, or
  its values laid out as a line or categories (`legend_range_filter_shown()`) — which would leave it unseen. A
  first click of a double click on an edge keeps the range, and only what the pointer is seen over counts: a
  click on an open menu over the histogram picks nothing on it. The axis and each step of the brush, worked
  out from plain input (`legend_brush_step()`), are `LegendAxis`, apart from the drawing.
- With more than one filament, hovering a filament's row shows its paths and a click keeps it alone in
  colour; the summary's row is the same row (`filament_row()` in `PreviewPanelParts`, with the translated
  units and the isolate hint) and both show the same state, the viewer's. The filament isolated drops with a
  new result and when both the legend and the summary are hidden.
- The settings open under the header as the G-code window's do: the line types' columns (time, share,
  weight, length, at least one), the histogram, highlighting on hover, whether a scale spans the whole print or the layers the slider shows — recoloured once the
  slider rests for 150 ms, since every change recolours all vertices —, and the palette, Classic or
  a perceptual one (viridis) for every scale. They are kept under `legend_*` keys; a value not understood reads
  as the default. A new result takes its own layers at once, and a scale over layers without values shows none.

## Path focus

Hovering a row of the legend or the summary or a bar of the histogram, brushing a range on the histogram and isolating a
filament keep some paths in colour and hide the others while the focus lasts. Focus is a line type or move, an extruder and
an interval of the scale, combined (`libvgcode::Focus`); `PreviewPanels::apply_path_focus()` builds it each frame from what
the panels report and sets it only when it changes. A range that misses the scale keeps nothing in colour.

libvgcode keeps, next to each vertex's colour, a 32-bit key: its kind (line type or move), its extruder and the position of
its value on the current scale, written in the same pass as the colours (`Focus.hpp`) from the first focus set on; until
then no key is kept or uploaded, and the shaders, whose focus uniforms then hold no focus, read none. The vertex shaders compare the key
with uniforms in one focus test shared by the GLSL and GLSL ES shaders, which take the layout of the key from `Focus.hpp`
as defines. A segment out of focus is hidden: its fragments are discarded and write no depth, so a line type inside the model
shows, and the model's translucent shell keeps it in place. One rule decides whether the value of a vertex counts on a scale:
it gives the vertex its colour and the position in its key, enters the colour range, and is what `Viewer::get_range_value()`
returns, which the legend's histogram reads too. Once the keys are kept, a change of focus uploads nothing. Shadows ignore the focus: the shadow map is
cached without it, and redrawing it on every hover would cost too much. The solid model takes a body's colour from one of
its paths, so a body goes grey when that path is out of focus; under a filter by value that is an approximation of the body
as a whole.

## The print summary

The print summary (`PrintSummaryPanel`) tells what the plate shown takes and what happens in it: the
totals (time, preparing, the normal or stealth estimate, weight, length, cost, the filament on the
model alone), each filament's mass on the legend's filament row, its length and its use split into model, support, flushing and the
prime tower in the tooltip, with the parts of all of them under the total, filament and tool
changes, the events (pauses, colour changes, tool changes, templates and custom G-code, each with its
layer and the time up to it, a click moving the layer slider there), on a two-nozzle Bambu printer the
nozzles' groups and the grouping block with its actions, and a G-code file's profiles. Collapsed, its
header holds the line of totals. `PreviewPanels::print_summary_model()` builds its `PrintSummaryModel`
each frame; the filaments are built once for both panels by `PreviewPanels::legend_filaments()`. The totals,
the events and colour changes, a filament's parts and the nozzle grouping comparison are worked out from
plain inputs by the functions of `PrintSummaryModel.cpp`, which also gather a filament's part volumes for the
statistics of all plates. When a G-code file is opened on its own, the plater has no statistics, and the
totals come from the filaments.

Statistics of All Plates (`AllPlatesStatsPanel`) is drawn from the same parts, summed over the
plates, in a card at the middle of the canvas.

The legend, the print summary, their text, models, scale view and shared parts, and the statistics of all
plates are panels of the toolkit and go through the panel check. The View menu shows and hides the legend, the print
summary and the G-code window, each with a shortcut of its own in the registry (`toggle_legend`,
`toggle_print_summary`, `toggle_gcode_window`); no panel switches another. The legend, the summary and the G-code
window stand on the toolkit's `PanelCard` and collapse through its `PanelCollapse`; the G-code window takes the card and the header from it and draws its rows
itself, since they scroll by line rather than in a scroll area.

## The G-code window

The G-code window (`GCodePanel`) shows the lines of the G-code file around the current move.
`GCodeViewer::SequentialView::render_overlay()` decides whether it is shown: when the preview has a
render path and View → Show G-code Window is on it draws the panel, otherwise it calls
`GCodePanel::hide()`, so the panel takes no room in its column. It is 300 logical
pixels wide whatever the lines hold, so its edge never moves while the move slider runs through
lines of different lengths; long lines are clipped with a fade at the edge, or wrapped by words onto
rows without a number or a stripe.

- The code is drawn in JetBrains Mono, bundled in `resources/fonts` and loaded by `ImGuiWrapper`
  into the ImGui atlas in regular, bold and italic at the sizes of `CanvasUi::MONO_FONT_SIZES` (11, 12
  and 14 px against an interface font of 15, scaled with it), with Latin and Cyrillic only. That
  table is the one list of monospaced sizes: the Font size setting offers it and the toolkit
  context hands out its faces. A span with a glyph
  the monospaced font lacks, such as a Japanese object name in a comment, is drawn with the default
  font, which holds the scripts of the interface language.
- `GCodePanelText` turns a line into spans placed in characters: the command, the letter words of a
  move in fixed columns when "Align parameters in columns" is on (command 4, X 9, Y 9, Z 8, E 8, F 7 characters; X and Y always keep theirs, Z, E
  and F take room only on a line that has them, so a line lays out the same whatever lines are around it; a move with other letters, such as an arc, keeps its words in file order), the keys and values of a macro, and the comment. Tag comments, the ones Orca
  writes to mark the print, are told apart by their fixed prefixes (`TYPE:`, `WIPE_START`,
  `LAYER_CHANGE`, block bounds and the like) and drawn as badges; the Comments setting shows all
  comments, only the tags (`GCodeCommentMode::Tags`) or none. Because
  the font is monospaced, the window places a span by multiplying its column by the width of a
  character.
- The stripe left of the code is the line type color of the move the line makes, found by the
  line's `gcode_id` among the viewer's vertices; a line without a move has none. A stripe too close
  to the panel color gets a ring.
- The rows are rebuilt only when the current line, the row count or a setting changes. The current
  line sits in the middle, or in the upper third, and moves toward an end of the file near it. Its
  tint takes contrast from the syntax colours, so on it every colour of the code moves 30 % toward
  the foreground: axis letters keep 3:1 and secondary text 4.5:1.
- The header holds the collapse button, the title, the current line out of all lines, which turns
  into a field to go to a line, and the settings button. Collapsed,
  the window shows no lines under its header, which shows the current line of code in place of the
  count. The settings open inside the window, under the header, so the lines below show each change at
  once; they are fields of the toolkit's settings form, loaded from the app config the viewer passes and
  saved through a callback it sets, under `gcode_panel_*` keys, not in the project. Going to a line puts the layer slider on its layer and the move
  slider on its move, or on the first move after a line that is not one.
- The settings open over the code: a divider and a shadow on the lines under them, which keep
  their place and colors. The window grows by the settings' height, but when expanded always keeps
  three lines of code under them; settings taller than the room left scroll. Their first row holds the View and
  Highlighting tabs, the count of settings changed from their defaults while some are, and a Reset button,
  which stays in place, disabled, when nothing is changed. Only the settings of the tab shown are drawn;
  View is shown each time the settings open, and the tab is not kept.
- A position indicator at the right edge of the lines tells where they stand in the file. Its thumb
  stays inside the track and reaches its bottom on the last lines; it is not drawn when the whole file is
  shown.

The window, in `src/slic3r/GUI/GCodePanel.cpp`, with the parsing and laying out of lines in
`GCodePanelText.cpp`, is built from the toolkit only
([canvas-ui.md](canvas-ui.md)): a panel card with its collapse and settings buttons, its settings a settings form of folder
tabs and fields, a small ghost button with an icon and a label to reset them, a toolkit menu to copy lines
and the position field to go to one. Its rows are painted with the card's painter in the monospaced faces the toolkit context hands
out, and it reads the mouse and Esc through the toolkit, calling no ImGui itself.
Its code colors are its own, in a light and a dark set; everything around them takes the theme
tokens.

## Marks on the layer rail

Pauses, color changes, filament changes and custom G-code are drawn on the rail by type. Marks
closer than a line of text on screen are merged by `group_ticks_by_position()` (`PreviewMarks.cpp`) into a badge with
their count; the grouping is recomputed every frame, so a taller window can split a badge again.
Hovering a mark or a badge opens a list of its marks. The list stays open while the pointer is over
the mark, over the list, or in the gap between them, and its actions reuse the code of the handle's
context menus.
