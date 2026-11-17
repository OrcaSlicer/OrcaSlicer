# Curve editor — High Level Design

## Purpose and scope

The curve editor is a dialog for a setting whose value is a list of XY points with a curve drawn
through them. It shows the points on a graph and in a table, and both can be edited: points are
dragged on the graph or typed in the table, added, removed, pasted, and imported from or exported
to CSV.

Its first user is the Flow Compensation Model of small area flow compensation
(`small_area_infill_flow_compensation_model`), which maps an extrusion length in mm to a flow
correction factor. The editor itself knows nothing about that setting: the rules of a particular
curve are supplied by a model class, so another point-list setting can reuse the dialog.

The editor changes only the edited preset value, and only when it is closed with OK. Cancel, Escape
and the title-bar button leave the preset as it was. CSV export and import do not save anything to
the preset either.

## Parts

| Part | Where | Role |
| --- | --- | --- |
| `CurveModel` | `src/libslic3r/CurveModel.*` | The numeric rules and the text of the points. No wxWidgets. |
| `SmallAreaInfillFlowCompensationModel` | `src/libslic3r/GCode/` | The rules of the flow compensation curve. |
| `CurveEditorPanel` | `src/slic3r/GUI/CurveEditorPanel.cpp` | The graph: geometry, selection, dragging, pan and zoom input. |
| `CurveEditorPanelPaint.cpp`, `CurveEditorPanelMac.mm` | `src/slic3r/GUI/` | Drawing of the graph, and reading of the macOS scroll event. |
| `CurveEditorDialog` | `src/slic3r/GUI/CurveEditorDialog.*` | The table, the buttons, validation messages, layout, file dialogs. |
| `CurveEditorUtils` | `src/slic3r/GUI/CurveEditorUtils.hpp` | Steps of the dialog that need no application: cancelling a cell edit, reading and writing a CSV file. |
| `SmallAreaInfillFlowCompensationDialog` | `src/slic3r/GUI/` | Titles, axis labels and the model for the flow compensation setting. |

The split follows one rule: everything that decides whether a number or a view is acceptable lives
in `CurveModel`, which is in libslic3r. The slicer and the editor then check the points with the same
rules and draw and apply the same interpolation, and the rules can be tested without a window. The panel only
turns input into proposals (`on_move`, `on_pan`, `on_zoom`) and reports when a point drag starts and
ends; the dialog passes the proposals to the model and shows what the model accepted.

The panel's drawing is in a separate source file because it needs the application (theme colours),
while the interaction and geometry code does not. The GUI tests compile `CurveEditorPanel.cpp`
directly and link an empty `paint_chart()` in place of the application's one.

## The points as text

The setting is a list of strings, one `x,y` pair per string. `CurveModel` keeps the pairs as text,
not as numbers:

- A row the user did not change is written back exactly as it was read, including its spacing and
  digits. Only an edited cell is normalized (trimmed, decimal comma replaced by a point). Opening
  the editor and pressing OK therefore does not mark the preset as modified.
- A typed value keeps all its digits, also when they exceed what a `double` holds. Numbers are
  parsed and written independently of the locale; a decimal comma is accepted on input.
- Entries that are blank in the saved value are not shown as rows. They stay in the value as long as
  the rows are not changed.
- A row shows the first two comma-separated fields of its saved entry, which are the fields slicing
  reads. A decimal comma is therefore never read from a saved entry: `0,0,5` is the point (0, 0),
  not (0, 0.5). Only a typed or imported cell can hold a decimal comma, and it is saved with a point.

On OK the dialog hands the list of strings itself to the config: `change_opt_value` accepts a
`std::vector<std::string>` for an option kept as one value. The text form `x,y;x,y;` that a text field
of such an option produces is not used, because joining and splitting it again would not keep blank
entries and the unchanged preset would then count as modified.

A model limits the number of points (50 for the flow compensation curve). The limit applies to
points that are added or imported; a saved value with more points opens, can be edited and exported,
and is not cut.

## Validation

`CurveModel::read_points()` parses the rows and calls the model's `validate_points()`. The result is
a message and the row it refers to. The dialog shows it below the graph as "Table row N: ...", and
while the table is invalid the graph draws no curve and OK and Export CSV are disabled. The table
keeps the invalid text so it can be corrected.

For the flow compensation curve, lengths and factors must be finite and strictly increasing, the
first length must be 0 and the last factor must be 1.

An empty table is valid and means that no compensation is applied; the dialog says so in the same
place where it shows errors. A saved value with only blank entries opens as an empty table and means
the same.

The editor is strict: each of the two cells of a row must hold a number and nothing else. Slicing
reads the same two fields of a saved entry leniently, see [Use in slicing](#use-in-slicing), so a
value written by hand that sliced before the editor existed still slices. Opened in the editor, such
a value shows its errors and can be corrected, and OK stays disabled until it is. A value the editor
shows as valid has the points that slicing uses.

## Editing

**Dragging.** The panel proposes a new position for the dragged point. `CurveModel::move_point()`
clamps it to the model's `drag_bounds()`, rounds each moved coordinate with `format_point_number()`
(four decimals for the flow compensation curve) and accepts it only if the result is still valid.
A coordinate along which the pointer did not move keeps its value and its text, even if that value
is closer to a neighbour than the drag bounds allow: a horizontal drag never rewrites the other
column. The drag bounds keep a point between its neighbours and lock what the model fixes (the
first length and the last factor).

**Adding.** Add point puts a point in the middle of the interval after the selected row. If the
rounded midpoint would coincide with a neighbour or break the model's bounds, the nearest interval
with room is used; if there is none, or the point limit is reached, nothing is added and the button
is disabled. In an empty table Add point inserts the model's seed rows, a minimal valid curve.

Beside a single row there is no interval to split, so the model adds a neighbour: the last seed row
to the right of the point if the model accepts that pair, otherwise the first seed row to its left,
each moved into the model's bounds for an end point. The new row is selected. If no valid two-point
curve contains the lone point (for the flow compensation curve, a point at length 0 with factor 1),
the button is disabled.

The button's state is computed from the rows whenever the table changes and when a point drag ends:
dragging updates only the graph and the dragged row, and a drag can open or close the room for a
new point.

**CSV and paste.** Export writes a header with the model's column names and the values as typed,
with a decimal point. Import accepts comma or semicolon as the separator (a semicolon file may use
decimal commas), quoted cells, an optional header (the model's column names, or `x,y`) and a UTF-8
byte order mark. It also accepts the setting's own text form, `x,y;x,y;`, with or without the last
semicolon. Two pairs on the only line, `x,y;x,y`, could also be one semicolon row of two decimal
comma numbers; they are read as two pairs unless that one row is a valid curve of the model by
itself. A file that does not
parse, breaks validation or has more points than the limit is rejected as a whole with the number
of the offending line, and the current points stay. Paste (the button, or Ctrl/Cmd+V with the graph
focused) reads the clipboard through the same parser. A click anywhere on the graph gives it the
focus.

Export writes a complete temporary file and replaces the target with it, so a failed write does not
damage an existing file.

## The view

The visible range is separate from the model values: panning, zooming, Reset View and the range
fields never change a point. The model supplies the limits of the view (`view_limits()`), the
smallest span that can be zoomed to, and the view that shows the whole curve (`fitted_view()`).
`panned_view()` moves the range as a whole and stops at the limits without changing the scale;
`zoomed_view()` scales one axis around the pointer.

For the flow compensation curve the horizontal limit follows the printer: it starts from the bed
diagonal and grows so that every point of the model stays reachable. The bed size is a reference for
navigation, not a limit on the values.

The plot rectangle depends only on the size, DPI and font of the panel and on the view limits, not
on the current range, so the plot does not shift while its tick labels change during navigation.

## Mouse and touchpad input

A mouse and a touchpad need different mappings of the same wheel event. With a mouse, the wheel
pans horizontally, Shift+wheel pans vertically, Ctrl+wheel zooms horizontally and Ctrl+Shift+wheel
zooms vertically; one notch moves 10% of the visible range. Dragging with the middle button pans
both axes. With a touchpad, a two-finger scroll pans along its own axes by equal screen distances
on both, so a circular motion of the fingers returns the view to where it started, and a pinch
zooms both axes around the pointer.

wxWidgets reports the axis of a wheel event but not the device it came from, so the panel asks the
platform:

- **Windows:** the thread is registered as touchpad-capable and the source of the current input
  message is read. This is used when Windows reports a Precision Touchpad among the pointer devices.
  The registration is a single switch for the whole GUI thread, not for a window, so the panels
  count their use of it and the last one to be destroyed switches it off.
- **macOS:** the scroll and momentum phases of the current AppKit event mark gesture scrolling
  (a gesture-capable mouse included), and its precise deltas are used so fractions are not lost.
- **Linux (GTK 3.20 or newer):** the source device of the current GDK scroll event is checked. This
  is used when the seat exposes a touchpad.

The device is never guessed from the size or the frequency of the deltas. When the platform cannot
identify it (a legacy driver that presents the touchpad as a mouse, a backend that merges devices),
the dialog shows a Touchpad checkbox that switches the mapping by hand; the choice is kept in the
app config as `curve_editor_touchpad_controls`. With automatic detection the checkbox is hidden.

Pan input is collected and applied once per frame, so the horizontal and vertical packets of one
gesture move the view together. While a native pan or zoom gesture is active, the wheel events the
system sends for compatibility are ignored, otherwise the gesture would be applied twice.

Native gestures are enabled per platform. Windows and GTK use the pan and the zoom gesture. GTK's
pan recognizer also fires for a drag with the left mouse button, which must not pan the view from an
empty part of the plot, so on GTK a pan gesture is accepted only when the GDK event being dispatched
at its start comes from a touchscreen; two-finger scrolling on a touchpad reaches the panel as
scroll events. macOS needs no gestures, it delivers scrolling and magnification directly.

A gesture counts from its start event to its end event. A gesture that starts while a point is
dragged or the middle button pans is ignored as a whole, also after the drag has ended, so it never
takes over halfway. A gesture that was already running is suspended for the time of the drag.

A point drag or a middle-button pan captures the mouse. The capture is released, and movement
still pending is dropped, when the button is released, the capture is lost, the window is
deactivated, the DPI changes or the dialog closes. macOS sends no capture-lost event when another
window becomes active, which is why the panel watches the activation of its top-level window.

## The dialog

The graph and the table show the same selection. "Show table" and "Show range" hide or show the
table and the fields for exact view bounds; the graph, its buttons and the validation message stay
visible either way. When the window is too narrow for the graph and the table side by side, they
are stacked. The table is as tall as its rows up to 12 rows and scrolls beyond that.

Escape while a cell is being edited cancels only that edit; the next Escape cancels the dialog.

The dialog is a `DPIDialog`, like the other dialogs of the application. It is therefore on the
application's stack of modal dialogs, which is what ends the open dialogs when the system session
ends, and it takes DPI and theme changes from the base class (`on_dpi_changed()`,
`on_sys_color_changed()`) without binding these events on itself a second time. On a DPI change on
Windows it applies the rectangle Windows suggests, no smaller than its minimum size, because
wxWidgets leaves the size and position to a window that handles the event; keeping the old corner
could leave it mostly on the previous monitor and change the DPI back. When the user goes on
resizing the window after the DPI change, or one drag crosses two DPI changes, the suggested
rectangle no longer fits, and the dialog keeps its corner and scales its current size. Windows
gives a window the DPI of the monitor that has the largest part of it, so the final rectangle
must stay on the monitor that owns the suggested rectangle, or the window when no rectangle is
suggested. Where the scaled size or the minimum size would leave its largest part on another
monitor, the dialog moves into the work area of the owning one; otherwise it keeps its corner. On
macOS the base class does not handle the DPI event, and logical sizes do not change there; the
widgets with icons rescale from their own handlers on every platform. Escape is decided by the
dialog's key hook, which runs before the one of the base class.

Its size and position are stored under a key given by the dialog that derives from it, not under
the window name, and are brought back onto a display when restored.

## Use in slicing

`SmallAreaInfillFlowCompensator` checks the points with the model's `validate_points()` and builds
its interpolator with the model's `make_interpolator()` (PCHIP by default), so the curve the editor
draws is the curve the slicer applies.

It reads the saved entries more leniently than the editor, the way they were read before the editor
existed, so that every value that sliced then gives the same points now. An entry is split at its
commas and checked in this order: a blank first field skips the entry, a first field that does not
start with a number is an error, nothing after the first comma skips the entry, and a second field
that does not start with a number is an error. Text after a number or after the second field is
ignored. A number may have leading whitespace and a sign and be decimal or hexadecimal (`0x1p-1`).
Numbers are read independently of the locale and must be finite. What remains must pass the same
validation as in the editor, otherwise slicing stops with an error that names the entry.

A value with no entries, or with only blank ones, applies no compensation.

## Tests

- `tests/libslic3r/test_small_area_infill_flow_compensation.cpp`: the model. Parsing, validation,
  adding and moving points, CSV, view limits, pan and zoom.
- `tests/libslic3r/test_locales_utils.cpp`: the shortest number text the points are written with.
- `tests/fff_print/test_small_area_infill_flow_compensation.cpp`: the slicer's use of the curve.
- `tests/slic3rutils/test_small_area_infill_flow_compensation.cpp`: the setting in presets and in
  the GUI value conversion.
- `tests/slic3rutils/curve_editor_gui/`: the `curve_editor_gui_tests` executable, which drives the
  panel with native wxWidgets events (mouse, wheel, gestures, capture) and checks the dialog's CSV
  file reading and writing through `CurveEditorUtils.hpp`. The dialog as a whole needs the
  application and is not built there, and its header is not included either.

What the tests do not show: how the graph looks, and how a real driver delivers gestures. Synthetic
events cannot establish which device the platform reports.

### Running the GUI tests

`curve_editor_gui_tests` is built with the other tests and runs under `ctest`:

```text
cmake --build build --config Release --target curve_editor_gui_tests
ctest --test-dir build/tests -C Release -L CurveEditorGUI --output-on-failure
```

The cases that create windows have the `RequiresDisplay` tag, which is also their ctest label. They
need a display. On Windows and macOS there always is one. On Linux without `DISPLAY` or
`WAYLAND_DISPLAY`, run them under Xvfb:

```text
xvfb-run -a ctest --test-dir build/tests -C Release -L CurveEditorGUI --output-on-failure
```

`scripts/run_unit_tests.sh` does this by itself. It counts `WAYLAND_DISPLAY` as a display only when
`GDK_BACKEND` is unset or its list contains `wayland` or `*`. If the machine has neither a display nor
`xvfb-run`, the script leaves the `RequiresDisplay` tests out and prints a warning; `ctest` run
directly fails them.

These tests take the `curve_editor_display` resource lock, so `ctest -j` does not run two of them at
once: mouse capture belongs to the display. On Linux they run with `NO_AT_BRIDGE=1`, because a
missing accessibility bus would delay the start of each process.

The directory can also be configured on its own, which builds only this executable against an
existing dependency install:

```text
cmake -S tests/slic3rutils/curve_editor_gui -B build/curve-editor-gui-tests -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<deps-install>
cmake --build build/curve-editor-gui-tests --config Release
ctest --test-dir build/curve-editor-gui-tests -C Release --output-on-failure
```

To run all cases in one process in random order, which shows a dependence on the order of the
fixtures, start the executable with `--order rand`.
