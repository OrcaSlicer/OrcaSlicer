# Curve editor regression checks

The editor is tested at three levels. The normal Catch2 suites cover its numeric model,
CSV parser, preset integration and G-code consumer. This standalone suite exercises the
panel's production interaction and geometry code with native wxWidgets events. The
checklist below covers full-dialog presentation and hardware behavior.

The standalone suite runs independently of the main build and does not open the user's
profiles or preferences. `prepare_panel.py` replaces only `paint_chart()` and its app/theme
includes: painting depends on full application startup. It does not copy or reimplement
the event handlers. Changes to the production panel regenerate the tested translation
unit. These checks cannot establish pixel appearance or how a real driver delivers gestures.

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
on a headless machine. GUI cases have the `RequiresDisplay` label. This optional suite
is not added to the application's default/headless build.
Linux builds also need GTK 3 development headers. macOS builds include the Objective-C++
adapter and link AppKit.

To exercise all cases in one process and detect fixture-order dependencies:

```text
build/curve-editor-gui-tests/curve_editor_gui_tests --order rand
```

For multi-config Windows builds the executable is in `Release/`.

The ordinary suites use the `SmallAreaInfillFlowCompensation` tag in
`tests/libslic3r/test_small_area_infill_flow_compensation.cpp`,
`tests/fff_print/test_small_area_infill_flow_compensation.cpp` and
`tests/slic3rutils/test_small_area_infill_flow_compensation.cpp`.
Build/run those suites through the normal test workflow in `tests/AGENTS.md`.

Localization has a separate read-only check:

```text
python tests/slic3rutils/curve_editor_gui/check_localization.py
```

It extracts dialog/model messages from the production source, checks template/catalog
coverage, fuzzy/empty translations, placeholder order, newlines and edge whitespace,
and runs `msgfmt --check-format` on every catalog. It uses bundled gettext tools on
Windows or tools on `PATH`; `--xgettext` and `--msgfmt` allow explicit paths.
When both tools are found at configuration time, CTest also runs this check with the
`Localization` label.
English intentionally falls back to the source. Correctness of wording still needs
human review, including legitimate borrowed labels that equal the English text.

| Behavior | Automated coverage |
| --- | --- |
| Validation, finite numbers, endpoints, strict ordering, decimal commas | Core model suite |
| Opening, editing, accepting, canceling, defaults, empty curves, precision | Core model suite; GUI value integration |
| Add/remove, exhausted float intervals, model-defined point limits | Core model suite |
| CSV separators, recognized headers, BOM, malformed rows, physical line errors, import limit | Core model suite |
| CSV precision, Unicode paths, embedded nulls, safe file replacement | Core model suite; standalone file checks |
| Pan/zoom bounds, minimum spans, cursor anchor, adaptive bed/model limits | Core model suite |
| Mouse modifiers, fractional wheel deltas, two-axis touchpad input, pinch-in/out, cumulative gestures | Standalone panel checks |
| Paired axis packets, equal pixel distances, closed circular motion, gesture/wheel overlap, discarded pending pan | Standalone panel checks with the native navigation timer |
| Stable plot geometry, point selection/dragging, capture cancellation, gestures during dragging | Standalone panel checks |
| Dirty/revert state, complete option transfer, unchanged inactive feature | Preset/GUI integration and FFF suites |
| Shared PCHIP, role eligibility, saved models above the editor limit, malformed inactive settings | FFF suite |
| Source/catalog coverage and formatting contracts | Localization script |

## Full-dialog and hardware checklist

Use a disposable print preset. Keep any fixture project separate from normal user projects.

1. Open the editor with default, empty and invalid point data. Check that the description,
   axis labels and single-line table headers are readable in light/dark themes and at
   different DPI/font sizes. Test a locale with longer labels.
2. Select a graph point and a table row in turn. Both selections must agree. Edit both
   columns, drag a point, add/remove points, restore defaults and use Reset View. Reset
   View and navigation must change only the viewport. Stationary/horizontal/vertical
   drags must not accidentally change the other coordinate or dirty unchanged values.
3. Reach the model's point limit. Add point must disable, then re-enable after removing
   a point. Import an oversized CSV: show an error and keep the current curve. Open a
   larger saved model: preserve its points and allow editing/export without extra additions.
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

These checks are explicit coverage of the dialog wiring and presentation; they are not
a claim of automated line coverage or evidence of a successful hardware test.
