# Curve editor regression checks

The editor is tested at three levels. The normal Catch2 suites cover its numeric model,
CSV parser, preset integration and G-code consumer. This standalone suite exercises the
panel's production interaction and geometry code with native wxWidgets events. The
checklist below covers full-dialog presentation and hardware behavior.

The standalone suite runs independently of the main build and does not open the user's
profiles or preferences. It compiles `CurveEditorPanel.cpp` directly and links an empty
`paint_chart()` implementation. The application links `CurveEditorPanelPaint.cpp`, which
uses its theme and application state. These checks cannot establish pixel appearance
or how a real driver delivers gestures.

Native input adapters preserve ordinary wheel modifiers and use native axes for gesture
scrolling. Ordinary mouse notches retain the existing 10% viewport step; gestures use
equal screen distances on both axes. Windows uses scoped thread registration and input-source metadata when it
exposes a Precision Touchpad. GTK uses the source device when a touchpad is exposed by
the seat. Cocoa uses AppKit's scroll/momentum phases (including gesture-capable mice)
and retains precise fractional screen deltas. The manual Touchpad fallback remains available
when reliable native identification is unavailable; it is not inferred from delta size
or event frequency. Hardware/backend recognition still needs platform testing.

## Automated checks

From the repository root, with the existing dependency install:

```text
cmake -S tests/slic3rutils/curve_editor_gui -B build/curve-editor-gui-tests -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<deps-install>
cmake --build build/curve-editor-gui-tests --config Release
ctest --test-dir build/curve-editor-gui-tests -C Release --output-on-failure
```

On Windows, use a Visual Studio developer shell and optionally `-G Ninja
-DCMAKE_CXX_COMPILER=clang-cl`. On Linux, the window tests need a display; use
`xvfb-run -a ctest --test-dir build/curve-editor-gui-tests -C Release --output-on-failure`
on a headless machine. GUI cases have the `RequiresDisplay` label. The normal test
build includes this target under `slic3rutils`, so packaged CI artifacts run the same
native cases. Headless Linux CI supplies Xvfb; self-hosted runners need a display or Xvfb.
Linux builds also need GTK 3 development headers. macOS builds include the Objective-C++
adapter and link AppKit.
Linux discovers wxWidgets through `wx-config`, including system installs without
`wxWidgetsConfig.cmake`; Windows/macOS use the dependency install's CONFIG package.
CTest disables the unused GTK accessibility bridge (`NO_AT_BRIDGE=1`) for these native
checks so a missing AT-SPI bus does not delay each process. The mapped test frames keep
their client area aligned with the panel, including after resizing.

To exercise all cases in one process and detect fixture-order dependencies:

```text
build/curve-editor-gui-tests/curve_editor_gui_tests --order rand
```

For multi-config Windows builds the executable is in `Release/`.
On headless Linux, run the same-process check with:

```text
NO_AT_BRIDGE=1 xvfb-run -a build/curve-editor-gui-tests/curve_editor_gui_tests --order rand
```

To rebuild/run only the native cases from the normal application build tree:

```text
cmake --build build --config Release --target curve_editor_gui_tests
xvfb-run -a ctest --test-dir build/tests -C Release -L CurveEditorGUI --output-on-failure
```

Use `-R "<case name>" -VV` to get a single case's full output. Flatpak's explicit test
target list includes `curve_editor_gui_tests`, so it is built before packaging the asset.

The ordinary suites use the `SmallAreaInfillFlowCompensation` tag in
`tests/libslic3r/test_small_area_infill_flow_compensation.cpp`,
`tests/fff_print/test_small_area_infill_flow_compensation.cpp` and
`tests/slic3rutils/test_small_area_infill_flow_compensation.cpp`.
Build/run those suites through the normal test workflow in `tests/AGENTS.md`.

The existing `Check translation` CI job validates catalog formats with
`msgfmt --check-format`. Catalog updates follow the normal localization workflow;
translation wording and the UI fit of longer labels need human review.

| Behavior | Automated coverage |
| --- | --- |
| Validation, finite numbers, endpoints, strict ordering, decimal commas | Core model suite |
| Opening, editing, accepting, canceling, defaults, empty curves, precision | Core model suite; GUI value integration |
| Add/remove, exhausted float intervals, model-defined point limits | Core model suite |
| Dragged-point clamping and rounding, untouched axis kept verbatim, axis locked by close neighbors | Core model suite; standalone panel checks |
| CSV separators, recognized headers, BOM, malformed rows, physical line errors, import limit | Core model suite |
| CSV precision, Unicode paths, embedded nulls, safe file replacement | Core model suite; standalone file checks |
| Pan/zoom bounds, minimum spans, cursor anchor, adaptive bed/model limits | Core model suite |
| Mouse modifiers, fractional wheel deltas, middle-button pan/capture/cancellation, two-axis touchpad input, pinch-in/out, cumulative gestures | Standalone panel checks |
| Paired axis packets, equal pixel distances, closed circular motion, gesture/wheel overlap, discarded pending pan | Standalone panel checks with the native navigation timer |
| Stable plot geometry, point selection/dragging, rapid repeated presses, capture cancellation/deactivation, existing capture, DPI interruption, gestures during dragging | Standalone panel checks |
| Dirty/revert state, complete option transfer, unchanged inactive feature | Preset/GUI integration and FFF suites |
| Shared PCHIP, role eligibility, saved models above the editor limit, malformed inactive settings | FFF suite |
| Translation catalog formats | Check translation CI job |

## Full-dialog and hardware checklist

Use a disposable print preset. Keep any fixture project separate from normal user projects.

1. Open the editor with default, empty and invalid point data. Check that the description,
   axis labels and single-line table headers are readable in light/dark themes and at
   different DPI/font sizes. Test a locale with longer labels.
   On macOS and Linux, where Orca follows the system appearance, switch it while the editor
   is open: the table (cells, headers, grid lines, selected row and frame), panel
   backgrounds, checkbox outlines, button icons and focus borders must follow the new
   theme without reopening the dialog.
   On Windows, move the open dialog between monitors at 100%, 150% and 200% scaling,
   then move it back, with table/range shown and hidden. Check fonts, icons, compact
   buttons, range fields/arrows, row heights, headers and footer alignment. Draft values,
   selection and view must survive, and size hints/scrollbars must follow the new scale.
   On Linux and macOS, repeat between displays with different scale/backing factors
   supported by the desktop and check that native font scaling is not applied twice.
2. Select a graph point and a table row in turn. Both selections must agree, also when
   a changed cell is still being edited and another row's cell or row number is clicked.
   Edit both columns, drag a point, add/remove points, restore defaults and use Reset
   View. Reset View and navigation must change only the viewport. Stationary/horizontal/vertical
   drags must not accidentally change the other coordinate or dirty unchanged values.
   Release and immediately grab the point again: dragging must still start when the
   second press is recognized as a double click. Repeat this for middle-button panning.
3. Reach the model's point limit. Add point must disable, then re-enable after removing
   a point. Make the table invalid (clear a cell, or leave a single row): Add point, Export
   CSV and OK must disable without adding a blank row, then re-enable once the values are
   valid. Remove points, then add them back: the table height must follow its rows, up to
   12 visible rows, and Remove point must disable once the table is empty. Import an
   oversized CSV: show an error and keep the current curve. Open a larger saved model:
   preserve its points and allow editing/export without extra additions.
4. Toggle Show range and Show table repeatedly at wide and narrow window sizes. Check
   stacking, scrollbars, retained draft values and selection. Hidden table controls must
   not hide graph actions or validation errors. Re-enable the table and continue editing.
5. Check compact button heights, icon/text padding, Help separation, footer alignment,
   Export-before-Import order and Tab navigation. With range visible, Minimum/Maximum
   fields must share the graph's available width without growing in height. Long numeric
   values and column headers must fit the table in the side-by-side layout.
6. Apply range bounds with Enter, Apply, spin arrows and keyboard Up/Down. Invalid input
   must leave the current viewport unchanged. Error text must identify a table row when
   relevant, remain readable below the graph, and not force the table to appear.
7. Pan and zoom with a mouse. Nearby controls must not blink or resize on each wheel
   event; negative/scientific tick labels must not move the plot. Reset View must recover
   a useful viewport. Change the printer/bed and reopen: limits/tooltips must update while
   keeping model points beyond the bed dimensions reachable.
   On Windows, Linux (X11 and Wayland) and macOS with a three-button mouse, hold the
   middle button inside the plot and drag horizontally, vertically and diagonally.
   The graph must follow the pointer with unchanged zoom, point values and selection.
   Release outside the panel, interrupt capture, resize and reset the view: movement
   must stop without delayed jumps. Left-button point dragging, wheel modifiers and
   touchpad gestures must still work after release. A trackpad without a middle-button
   event continues to use its existing two-finger and pinch gestures.
   During both point dragging and middle-button panning, switch to another window/app
   before releasing the button (Alt+Tab or Command+Tab), then return. The pointer must
   be released, pending movement discarded, and the dialog must remain clickable.
8. With supported native input detection, the Touchpad checkbox
   must be hidden: mouse controls retain their axis modifiers while touchpad input follows
   its native axes without switching modes. Test mouse and touchpad in turn, including
   after closing/reopening the dialog. Test both Wayland and X11 on Linux. On macOS,
   check precise/momentum scrolling and ordinary mouse wheel modifiers. If detection
   is unavailable (including legacy drivers or a backend that merges device sources),
   enable the manual Touchpad fallback. Test vertical, horizontal and diagonal
   two-finger motion, circular paths, small deltas,
   inertia and repeated pinch-in/out on actual Windows, macOS and Linux hardware where
   available. Check cursor anchoring, limits, no duplicate zoom and normal point dragging.
   Confirm original mouse controls in automatic mode; in manual mode, disable Touchpad
   to check them. Reopen the editor/app and verify the selected fallback mode is remembered.
   Driver-specific identification and behavior are not established by synthetic events.
9. Close with OK, Cancel, Escape and the title-bar button. Check saved size/position, then
   change resolution/monitor layout and reopen. The dialog must remain reachable and
   respect minimum size. Cancel/Escape/title close must leave the preset unchanged; OK
   must apply all rows and show the preset's dirty state. Revert must restore the complete model.
10. Import/export through the real file pickers with Unicode filenames, overwrite an existing
    CSV, and test read/write failure. Import/export must not save preset changes until OK.
    Check localized control tips and errors, including the point-limit and touchpad help.
11. Paste CSV and documented `x,y;` model text with Paste or Ctrl/Cmd+V on the graph.
    Invalid text must leave the model intact. While editing a cell, Escape must cancel
    only that edit and keep the dialog and previous edits open; a second Escape cancels
    the dialog. Search for the model setting and check that its Set button gets focus.
12. Revert a changed model and inspect Unsaved Changes: both complete curves must be
    displayed. Check printable area, compatibility and ramming widgets for unchanged
    undo behavior. Slice a saved model with blank/trailing semicolon entries; invalid
    points must identify both the point number and its values in the slicing error.

These checks are explicit coverage of the dialog wiring and presentation; they are not
a claim of automated line coverage or evidence of a successful hardware test.
