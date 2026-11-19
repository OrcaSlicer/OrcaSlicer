# Windows, views, printers and web pages

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

## `test_printer_extruders`
`test_printer_extruders.py` · Python, 29 checks

**Tests:** The test launches its own instances on fresh datadirs, with printers the sweep doesn't seed. On the Bambu
Lab H2D (two extruders) it checks four things. `settings_set` validates a whole call before any variant switch or
flow-type pick. A variant switch asks about unsaved printer changes unless `reset_all` is given. A plate's
`filament_map_mode` and `filament_map` round-trip through `plate_set_settings` and `plate_get_settings`. The second
extruder's `nozzle_volume_type` can be set. On the A1 mini, a flow type that its 0.2 nozzle preset doesn't offer is
refused. On the Eryone ER20, `extruders_count` 2 gives two extruders.

**Decides by:** Exits 1 when any check prints FAIL, or on an uncaught exception (an unexpected tool error). A
failed launch adds a FAIL "the app starts with the ...".
- Validation before change (H2D): three kinds of call must raise: an unparseable `printable_height`, another key
  beside a variant switch, and a flow type the 0.2 preset lacks. After each one, `app_state.presets.printer` and
  `nozzle_volume_type` must still equal the values read before. The "second call" and "extruder 2" refusals are
  matched by message substring.
- Variant switch: `app_state.presets.printer` must become `Bambu Lab H2D 0.6 nozzle` with extruder 2 at
  `High Flow`, then return exactly to the 0.4 state read before. With unsaved changes, the reply's `status` must be
  `blocked_by_dialog`. After Discard, or with `reset_all` (then no `status`), the 0.6 preset is selected, no dialog
  is open and `preset_list` marks nothing `modified`.
- Plate grouping:
  - `filament_map_mode` starts as `Auto For Flush`, with `Manual` among the options offered.
  - `applied`, the read-back `value` and `effective` all equal `Manual`, then `[2, 1]`.
  - `[1, 3]` is refused naming "extruder 3", and the map stays `[2, 1]`.
  - Setting the mode to null reads it back as `None`.
- Flow types and counts: `nozzle_volume_type` has the `variants` `extruder 1` and `extruder 2`. Setting index 1
  gives `<first>,High Flow` in the reply and on read-back. `TPU High Flow` on extruder 1 and `extruders_count` 3
  are refused, matched by message substring.
- A1 mini: the preset is `Bambu Lab A1 mini 0.4 nozzle`. A switch to 0.2 with a flow type is refused naming
  "extruder 1" and "no nozzle flow type to pick", and the state is unchanged.
- ER20: the preset name is checked. `nozzle_diameter` is `0.4` before and `0.4,0.4` after, with two `variants`.
  `applied` equals `{"extruders_count": 2}`.

**Not checked:**
- "a call with a value that does not parse changes nothing" passes on any error. The message isn't checked to name
  `printable_height`, and "nothing changed" is judged only by the preset name and the flow types.
- "a variant switch with a flow type its preset offers sets both" checks only extruder 2's flow type, not
  extruder 1's.
- The unsaved-changes dialog is answered with Discard without checking which dialog it is. The `printable_height`
  of 300 that made the change is never read back.
- On the ER20, only `nozzle_diameter` is read back; no other per-extruder key is. Nothing checks that `reset_all`
  takes it back to one extruder.
- Setup and clean-up calls have no check of their own: `project_new`, `filament_add`, the 0.4 restore at the end
  of `unsaved_changes`, and the `reset_all` calls.

**Strength:** Strong — preset names, values and refusal texts are checked exactly, and each refusal is checked to
leave the state as it was.

## `test_project_open_settings`
`test_project_open_settings.py` · Python, 6 checks

**Tests:** The test seeds its own datadir with the preference `project_load_behaviour` = `load_geometry_only` and
launches its own instance. It checks that opening a saved project still loads the process settings saved with it
(`layer_height` 0.28), and that the preference stays as it was.

**Decides by:** Exits 1 when a check prints FAIL, when its own launch fails, or on an uncaught exception.
- Preference: `preferences_get` reads `project_load_behaviour` = `load_geometry_only`, both before and after the
  open.
- Layer height: the process `layer_height` from `settings_get` is checked three times. It is 0.28 after
  `settings_set`, not 0.28 after `project_new` drops the presets' changes, and 0.28 again after `project_open`.
- Open: the `names` in `project_open`'s reply has exactly one entry.

**Not checked:**
- "a new project prints with the preset's layer height" checks only `!= 0.28`, not the preset's known value.
- Only `layer_height` is checked. No other process, filament or printer setting saved in the project is read, and
  nothing checks whether the process preset shows as modified after the open.
- The object's name isn't checked. Nor is whether the open asked anything: a job envelope would fail only because
  its `names` is missing.
- `project_save`'s reply isn't checked; the later open implies the file was written.

**Strength:** Strong — the round-tripped layer height and the unchanged preference are checked exactly, though
only one setting is covered.

## `test_sidebar_floating`
`test_sidebar_floating.py` · Python, 4 checks

**Tests:** While the sidebar floats in its own window, `view_set {sidebar_width}` is refused and the width stays unchanged. The test launches its own instance twice on one fresh datadir. The first run saves `app.window_layout`. The test then marks the sidebar's AUI entry in that layout as floating, so the second run starts with it floating. On Linux both runs start as X11 clients (`GDK_BACKEND=x11`, through XWayland on a Wayland desktop), as the runner's own launches do by default, since a native Wayland app drops the saved floating state.

**Decides by:** Exits 1 when a check prints FAIL or on an uncaught exception.
- "the app starts" and "the window layout the first run saved has the sidebar": the first launch succeeds, and the `window_layout` in `OrcaSlicer.conf` has a `name=sidebar;` entry.
- "view_set of sidebar_width while the sidebar floats is refused": the call must raise, with "The sidebar floats in a window of its own" in the message.
- "and the sidebar's width stays as it was": `app_state.view.sidebar_width` equals the value read before.

**Not checked:**
- A native Wayland app is not tested: its saved floating state is dropped, and nothing checks that it starts docked.
- Nothing reads the state to confirm the sidebar really floats; only the refusal text suggests it. No docked control shows that `sidebar_width` is accepted when the sidebar isn't floating.
- The first run is stopped right after it reports ready, but the app writes its layout "at its next idle moment". A layout not yet written fails the second check, so this can flake but can't pass falsely.

**To settle in review:**
- That an X11 client is what a sweep on a Wayland desktop should test here. A `--wayland` sweep means native Wayland launches; this test could instead check there that the sidebar starts docked and `view_set {sidebar_width}` is taken. The proposal keeps one behaviour, the floating one, on every platform.

**Strength:** Strong — the refusal text and the unchanged width are checked exactly on every platform.

## `test_set_window`
`test_set_window.py` · Python, 40 checks on Windows and macOS (1 SKIP), 41 on Linux X11 including `--headless`,
31 on Linux Wayland (4 SKIP)

**Tests:** `window_set` on three kinds of window, plus the read side in `window_list` and `app_state.displays`:
- The main window: size, position, maximize, minimize and restore, a size given while maximized or after a
  minimize from maximized, and a contradictory request.
- The modeless Plugins window, by its ref: size, position, and a stale ref once it has closed.
- The modal Preferences dialog: by `target: "dialog"` and by a `{label}` locator; it never minimizes; and it stays
  above the main window in the stacking order.

**Decides by:** Exits 1 when any check fails or on an uncaught exception. SKIPs never fail it.
- Main-window size:
  - Size 1400×900, then 1300×820: the reply's `size` and `state` and `window_list`'s width and height equal it.
  - A size of 1300×850 given while maximized replies `normal` with that size.
  - The flat `width`/`height` form replies 1500×950.
- Position, everywhere except Wayland:
  - `app_state.window.position` equals [40, 30].
  - A position plus a size equal to the work area in the first reply's `display` is echoed back.
- On Wayland instead:
  - `app_state.window` must have no `position`.
  - A `window_screenshot` of the main window (no image) must report a width and height equal to 1300×820 times the
    reply's `scale`. Only that pixel size is compared.
- Minimize and restore, everywhere except Wayland:
  - Minimize returns in under 5 s and replies `minimized`.
  - Restore replies `normal`, and a canvas `window_screenshot` reports a width > 0.
  - Maximized, minimized, then `normal`: the reply says `normal` with no `note`, and `window_list` still reads
    `normal` 1 s later.
  - Maximized, minimized, then a size: the reply says `normal` with that size.
- Contradiction: `state: maximized` together with a size raises, and the size in `window_list` is unchanged.
- Plugins window:
  - It is listed with a `ref`.
  - The reply's size is 900×700, or 900 by more than 700 with `size` listed in `adjusted`.
  - "Plugins: the reply has the position": off Wayland the reply's position is [100, 100]; on Wayland, where no
    position is given or known, the reply has no `position` (its fields are `window_list`'s entry, which has no
    `x` or `y` there).
  - `window_list`'s place and size equal the reply's.
  - Escape closes it, and its ref then raises naming `window_list`.
- Preferences:
  - It is listed.
  - "dialog: window_set sizes the topmost modal dialog": `window_set {target: "dialog", size: [1000, 760]}` replies
    `kind` `dialog` and the size 1000×760, or 1000 by more than 760 with `size` in `adjusted`; `window_list`'s
    Preferences entry then has that width and height.
  - "dialog: a {label} locator finds it": the `{label: "Preferences"}` call replies `title` `Preferences`. Off
    Wayland it is given the position [60, 60], and the reply's `position` and `window_list`'s Preferences `x` and `y`
    are 60 and 60, with the size from the call before. On Wayland it is given the size 1000×760 again, and
    `window_list`'s width and height equal the reply's.
  - `state: minimized` raises.
  - It closes through `dialog_answer`.
- Stacking, on Linux X11 only, where libX11 opens the display: the main window is minimized, restored and raised.
  In the X server's stacking order its title must then sit below `Preferences`.
- Read side:
  - Every window with a ref has a valid `state` and a boolean `resizable`.
  - `displays` is not empty and has exactly one `primary`.
  - Each display has the keys `index x y width height scale`.

**Not checked:**
- SKIP paths by platform:
  - Windows and macOS run everything except the stacking check, which is a SKIP because `x11_stacking` finds no
    libX11 or display.
  - Linux X11 runs all 41.
  - Linux Wayland (`--wayland`) skips position, both minimize sections (9 checks) and stacking.
- Maximize is checked only by the reply's `state`; neither `window_list` nor the size is read. The minimize and
  work-area checks also trust the reply alone.
- The work-area fill compares the reply with the work area the app reported in its own first reply. That makes it
  self-referential, and `window_list` isn't read.
- "restore: the 3D view captures" only requires a reported width > 0; no pixels are compared.
- "a contradiction is refused" accepts any error text.
- Timing:
  - "and window_list still reads it normal a second later" relies on a fixed 1 s sleep, so a later re-zoom would
    be missed.
  - "minimize: returns within 5 s" is a timing bound.
- Displays are checked for key presence only; their values aren't compared with the OS.
- The stacking check isn't gated by platform. On a Mac where `ctypes.util.find_library("X11")` succeeds and
  `DISPLAY` is set (XQuartz), it would read XQuartz's stacking order, which lists no Cocoa windows, and fail. I
  could not confirm whether any sweep machine hits this.
- The final restore to `WINDOW_SIZE` is unchecked.

**To settle in review:**
- That Preferences takes 1000×760: if its minimum height is above 760, the reply holds its minimum with `size` in
  `adjusted`, which the check accepts as it does for Plugins.

**Strength:** Partial — main-window and dialog sizes and places are read back from `window_list`, but maximize,
minimize and the work-area fill trust the reply, and Wayland skips about a quarter of the test.

## `test_set_view_preview`
`test_set_view_preview.py` · Python, 21 checks (1 conditional)

**Tests:** `view_set` on the Preview of a sliced cube:
- A legend row hidden by a click stays hidden across view types, until Line Type is picked.
- The view type picked is written to `preview_last_view_type`.
- Canvas captures taken right after a layer or view-type change show the new state.
- `layer_z`, a range given backwards and the one-layer mode map to the right layers.
- Six refusals: legend rows before a slice, a height above the top layer, `layer` with `layer_z`, an unknown row,
  a value that isn't a boolean, and silent mode when the print has no silent estimate.

**Decides by:** Exits 1 when a check prints FAIL or on an uncaught exception.
- Legend state: `plate_slice_stats.view.legend.features["Sparse infill"]` must be `False` after a `widget_click` on
  its row, still `False` after `view_type=speed`, and `True` after `feature_type`.
- Config: `app.preview_last_view_type` in `OrcaSlicer.conf` must become `speed`, then `feature_type`. The file is
  polled for up to 5 s.
- Captures: an inner `replay_run` makes 8 changes and saves two canvas captures to PNG after each one. The replay
  must pass. Then two byte comparisons decide:
  - "Settled": the two captures after each change must be byte-identical.
  - "Every change shows": from the second change on, the first capture after a change must differ byte-wise from
    the last capture before it.
- Layers:
  - `layer_z=1.05` replies `layers` [1, 6, 128] and `z` [0.2, 1.2].
  - The backwards range [60, 20] replies [60, 60, 128] and `z` [12.0, 12.0].
  - In one-layer mode (`L`), [2, 5] replies [2, 5, 128] and `z` [0.4, 0.4], and `app_state.preview.layers` is
    [2, 2].
  - Leaving one-layer mode reads [1, 128].
- Refusals, matched by message substring:
  - "slice first"
  - "above the top layer (25.6 mm)"
  - "not both"
  - the print's rows "Sparse infill" and "travel"
  - "true (show) or false (hide)"
  - "no separate silent-mode estimate"
- After the refusals, `view_type` is still `feature_type` after the unknown-row one, and `time_mode=normal` echoes
  `normal`.

**Not checked:**
- The capture comparison is only relative. It shows that a capture changed and then held still, not that it shows
  the right layers or legend. Nothing is compared with a baseline.
  - The first change (layers 1–40) is not checked for "shows".
  - Byte equality between two PNGs needs pixel-identical frames, with no animation and no caret. The test assumes
    this.
- "the legend reply gives each row's state" reads only the reply. `travel` shown and `Sparse infill` hidden are
  not read back from `plate_slice_stats`.
- "silent mode without its own estimate is refused" runs only when the slice has no `silent_time_s`. Otherwise it
  is dropped without a SKIP line.
- After the refusals, only `view_type` is checked as unchanged; layers and legend aren't, although the docstring
  says a refused call changes nothing.
- The cube's slice outcome is not checked. The exact layer values that follow would catch a failed slice.

**Strength:** Strong — legend, layer and z values are exact, the config write is read from disk, and the captures
are compared with each other.

## `test_set_view_camera`
`test_set_view_camera.py` · Python, 35 checks

**Tests:** `view_set`'s camera fields (`look_at`, `zoom`, `camera`, `orbit`) and the `camera_state` it returns,
checked against `app_state.camera`. It covers four views: Prepare (including straight after `project_new`'s queued
fit), Preview, the Design tab's own camera, and the assemble view's first showing. It also covers zoom clamping,
`click_px_per_mm`, a fit after a resize on the Home tab, and a zoom given on Home while a fit is queued.

**Decides by:** Exits 1 when a check prints FAIL or on an uncaught exception.
- Prepare after `project_new`:
  - The reply echoes `camera` `top`, and `camera_state` has zoom 3, `look_at` [50, 60, 0] and `elevation` 90.
  - "prepare: the 3D view captures": `window_screenshot {target: "canvas"}` replies `source` `canvas`, a width and
    height above 0, and no `approximate` key (a 3D view that comes back blank is marked `approximate`).
  - After that capture forces a frame, `app_state.camera` still has zoom 3 and that `look_at`.
  - "prepare: camera_state has a projection type": `type` is `perspective`, as the fresh data folder's
    `use_perspective_camera` (on by default, with `auto_perspective` off) gives it.
- `click_px_per_mm`: zoom divided by `click_px_per_mm` lies in [1, 3].
- Clamping: `zoom=1000` gives 250, and `adjusted` == ["zoom"].
- Orbit: `camera=front, orbit=[30, 20]` gives `elevation` 20 and `azimuth` -60. `look_at` equals the one before,
  and the reply echoes `orbit` and `camera`.
- Preview: the imported cube slices with `outcome` `ok`. Zoom 2 and `look_at` [0, 0, 0] appear in both the reply
  and `app_state.camera`.
- Design (the sweep launches with `--cad`):
  - The zoom is within 1e-3 of `250/1.4^15`.
  - `look_at` is within 1e-3 of `describe_design`'s `modeling_origin`.
  - The reply echoes the camera, and `app_state.camera` has the zoom.
  - An orbit is accepted and keeps that zoom.
  - Back on Prepare, the zoom is 2. That value was set on Preview, so this shows the two share a camera.
- Assemble: the call raises no error, and after a capture `app_state.camera` has zoom 4 and `look_at`
  [10, 20, 0].
- Home:
  - `app_state` has no `camera`.
  - A zoom's reply has a `note`.
  - After switching to Prepare and a capture, the zoom is not 3.

**Not checked:**
- The captures in assemble and on Home only force a frame; their replies are not read unless the call fails, and no
  capture's pixels are compared.
- After the Design orbit, the resulting elevation and azimuth are not checked; only the kept zoom is.
- "resized on home: a fit on prepare uses the new size" compares two fits in a row with each other, not with the
  zoom expected for a 1400×800 canvas. A fit that used a stale size both times would pass.
- "home: the fit replaces the zoom once the view draws" checks only `!= 3`, not the fit's zoom. The `note` is
  checked for presence, not for its text.
- `click_px_per_mm` is checked against a range, not the display's known scale. Both values come from one
  `app_state`.

**Strength:** Partial — Prepare, Preview, assemble and clamping values are exact, but the Design orbit, the resized
fit and the Home fit are checked loosely or only against each other.

## `test_speed_dial_open`
`test_speed_dial_open.py` · Python, 12 checks (plus 1 where KWin answers; the 3 focus checks SKIP where the pointer cannot be moved)

**Tests:** The app goes from Home to Prepare, Design and Preview by tab clicks. After each switch, the page's 3D
view (`OrcaSceneCanvas`) must hold the keyboard focus, Space sent with no target must open the Speed Dial, and
Escape must close it. Where KWin answers on D-Bus, it also checks that KWin draws no title bar on the dial. The
test runs exclusive in the sweep.

**Decides by:** Exits 1 when a check prints FAIL or on an uncaught exception, such as a tab missing from
`widget_list`.
- The pointer off the view: the 3D view also takes the focus when the pointer enters it, so before each tab click
  the test moves the system pointer to the primary display's top-left corner, (0, 0), and reads the pointer's
  position back from the system: (0, 0). The seeded main window opens at the work area's top left
  (`seed_datadir()`), so that corner is its top bar or lies outside it, never in the 3D view below the tab bar. It moves the
  pointer with `SetCursorPos` on Windows, `CGWarpMouseCursorPosition` on macOS and `XWarpPointer` on X11, each
  through `ctypes`, and reads it with `GetCursorPos`, `CGEventGetLocation` and `XQueryPointer`. A pointer that does
  not read back at (0, 0) fails the page's focus check.
- Per page:
  - `app_state.tab` reaches the tab within 10 s.
  - The first `[focused]` line in `widget_list` contains `name=OrcaSceneCanvas`.
  - Within 10 s of Space, `window_list` shows a web dialog with no title.
  - Within 10 s of Escape sent to the dial's ref, it is gone.
- On a native Wayland display, where an app can neither move the pointer nor read where it is, each focus check
  prints SKIP with that reason instead of passing.
- KWin, on the first page only: KWin's frame height for the app's window with no caption must be at most the dial's
  height in `window_list` + 2.

**Not checked:**
- The focus checks are not made on a native Wayland display.
- The wait for Home after the first click isn't checked. If that click did nothing, the page click is still judged
  only by the final tab.
- Any web dialog with no title counts as the dial, and its content isn't checked. Escape is sent to the dial's
  ref, so Escape through the keyboard focus isn't tested.
- The KWin title-bar check prints "skip", which isn't a counted check, in two cases:
  - No KWin answers: Windows, macOS and GNOME.
  - KWin's window runner doesn't list the dial. Per the code comment this covers X11 clients, so it includes the
    default Linux sweep and `--headless`.
  It runs only for a native Wayland app on KWin (`--wayland` on Plasma).

**To settle in review:**
- Moving the user's pointer during a sweep on a desktop: the test is exclusive in the sweep, so nothing else runs
  meanwhile, but a person using the machine sees the pointer jump.
- That (0, 0) lies outside the 3D view on every sweep machine: the seeded `window_mainframe` puts the window at the
  work area's top left, and the 3D view is below the tab bar in any case; with a monitor at negative coordinates,
  (0, 0) is still the primary's corner.

**Strength:** Partial — opening and closing the dial are checked on every page, and the focus check is made with the
pointer off the view, but the KWin check almost never runs and native Wayland skips the focus checks.

## `test_send_gcode`
`test_send_gcode.py` · Python

**Tests:** Printing and sending through a print host, run against an OctoPrint stub host inside the test process:
the `send_gcode` Speed Dial action (`orca_command:send_gcode:orca`), `printer_send` with `plates`, `print_start`,
and `printer_test_host`. The test makes the seeded X1 Carbon a print-host printer (`bbl_use_printhost` true) before
any of these calls, since on the Bambu network they would open dialogs that message a printer. It covers:
- Refusals for a printer with no print host, a plate not sliced, and all plates where the Print button offers no
  such action.
- `print_start` and `printer_send` opening the host's upload dialog, which the test cancels: nothing is uploaded.
- The host test, answered by the stub, refused with no address, and failing on a port nothing listens on.
- An upload through the action's dialog, with the file name set by `widget_set_value` and Upload pressed by
  `dialog_answer`, tracked in `app_state.uploads`.
- The exported replay of that send, which uploads again.
- HTTP 500 replies, delayed and immediate, which must end as errors, with the queue's error dialog.

**Decides by:** Exits 1 when a check prints FAIL or on an uncaught exception. Every refusal is asserted by its code
with `checks.py`'s `refused`, then `app_state.uploads` and `dialogs()` are read back. "Nothing sent" below means:
`app_state.uploads` is as before the call, the stub got no file, and `dialogs()` is empty. "Cancelled" means: the
call replies `blocked_by_dialog`, `dialogs()` lists one dialog titled "Send G-code to printer host",
`dialog_answer {button: "Cancel"}` closes it, and then nothing was sent.
- Discovery: `action_search "Send G-code"` lists the action's id.
- No address. `settings_set {type: "printer", values: {bbl_use_printhost: true, host_type: "octoprint", print_host:
  "", use_3mf: false}}`, with the imported cube not sliced (plain G-code, so the dialog proposes a `.gcode` name):
  - The action is refused `no_print_host`; nothing sent.
  - `printer_send {plates: 0}` is refused `no_print_host`; nothing sent.
  - `printer_test_host` is refused `no_print_host`; `dialogs()` is empty.
- The stub's address set (`print_host` `127.0.0.1:<port>`), the cube still not sliced:
  - `printer_test_host` replies `ok` true and `message` "Connection to OctoPrint is working correctly."; the stub
    answered one more version check (`GET /api/version`); `dialogs()` is empty.
  - `print_start` is refused `not_sliced`; nothing sent.
  - `print_start {plates: "all"}` and `printer_send {plates: "all"}` are each refused `all_plates_not_offered`: the
    Print button offers neither for a printer through a host other than SimplyPrint; nothing sent.
- Sliced (`plate_slice`, the plate's `outcome` `ok`):
  - `print_start` is cancelled.
  - `printer_send {plates: 0}` is cancelled.
- Upload answered 200:
  - The row ends `completed` at progress 100 and was seen at (`uploading`, 99) during the stub's 3 s pause.
  - The row has `filename` `mcp_send_ok.gcode` and the stub's port in `host`.
  - The stub received exactly `{"filename": "mcp_send_ok.gcode", "print": "false"}`.
- Replay:
  - The `replay_export` steps equal exactly `action_run, app_wait(dialog), widget_set_value, app_wait(dialog),
    dialog_answer, app_wait`.
  - `replay_run` passes, a new row completes, and the stub has 2 uploads.
- 500 after a pause:
  - The row ends `error` with "HTTP 500" in `error`, never read `completed`, and was seen at (`uploading`, 99).
  - An error dialog appears, and after `dialog_answer ok`, `app_state.dialogs` is empty.
- 500 at once: the row ends `error`.
- A port nothing listens on (`print_host` set to it): `printer_test_host` replies `ok` false and a `message` starting
  "Could not connect to OctoPrint"; `dialogs()` is empty.

**Not checked:**
- The uploaded file's content. The stub parses only the multipart `filename` and `print` fields, so an empty or
  wrong body would pass.
- The replayed upload's file name.
- The error dialog's text: only that some dialog opened and closed. For the immediate 500, neither the
  "HTTP 500" text nor the dialog is checked; `close_error_dialog`'s result is ignored.
- The Print and storage dialogs of a printer on the Bambu network, and `printer_send`'s `not_sliced` there: no
  sweep test opens them, since they message a printer. The A2L check (`tools/mcp/a2l_printer_check.py`), run by
  hand, covers them.
- A SimplyPrint host's Print all, and a cloud host's login: the stub is an OctoPrint host.
- The host test's message in another language: the sweep runs the app in English.
- Row ids are assumed to be `count + 1`. Different numbering makes `follow` time out, which fails rather than
  passes falsely.
- The (`uploading`, 99) checks rely on polling every 0.2 s during a 3 s pause.

**Strength:** Strong — row states, progress, the file name, the print flag and the exported steps are checked
exactly against an independent stub, and each refusal by its code with the uploads and dialogs read back.

## `test_printer_commands`
`test_printer_commands.py` · Python · in `sweep.json` as `{"path": "test_printer_commands.py"}`

**Tests:** `printer_status`, `print_pause`, `print_resume` and `print_stop` with no printer connected, on a scratch
datadir. A scratch datadir is not cut off from the account it may be signed in to, so the test reads
`printer_status` first and goes on only when it says no printer is connected; it never calls a job command while
one is.

**Decides by:** Exits 1 when a check prints FAIL or on an uncaught exception.
- The guard: `printer_status` replies exactly `{"connected": false}`. Otherwise the check "no printer is
  connected" fails with the printer's `name` and `model` from the reply, and the test ends there, having called
  nothing else.
- `print_pause`, `print_resume` and `print_stop` are each refused `no_printer_connected`, asserted with
  `checks.py`'s `refused`; after each, `printer_status` replies exactly `{"connected": false}` again and `dialogs()`
  is empty.

**Not checked:**
- A connected printer: what each job command publishes, refusals by the printer's state, resume where the printer
  allows it only at the printer, and the status fields are unit cases against the printer agent double
  (`tests/slic3rutils/test_device_manager_integration.cpp`). The A2L check, run by hand, reads the A2L's status and
  checks the refusals while it is idle. A print paused, resumed and stopped is the maintainer's to check.

**Strength:** Partial — the refusals and the status with no printer are checked exactly, but nothing reaches a
printer.

## `test_web_ui`
`test_web_ui.py` · Python, 230 checks (1 conditional)

**Tests:** The GUI tools on web pages, through the side-loaded `mcp_web_fixture` plugin. Before the launch, the
sweep's setup step copies `mcp_web_fixture.py` and an enabled `.install_state.json` into the datadir's
`orca_plugins`.
- What the plugin provides:
  - One page with every kind of control, opened as a modal dialog and as a modeless window.
  - A dock panel whose CSP forbids eval.
  - A page among the main window's tabs.
  - A thread that brings up the permission prompt.
  - A configuration UI in the Plugins dialog's sandboxed frame.
- What it exercises:
  - `widget_list` snapshots.
  - `widget_click`, `widget_set_value`, `keyboard_press` and `mouse_gesture`, with their effects on the page.
  - Locators into a shadow root, frames, by index, and as the journal records them.
  - Refusals, the timeout contract and stale refs.
  - Navigation and token redaction.
  - `dialog_answer` and `dialog_auto_answer` on web dialogs and on the permission prompt.
  - The web snapshot budget.
  - The setup wizard's navigation.

**Decides by:** Exits 1 when any check prints FAIL or on an uncaught exception. `call()` turns most tool errors
into `{"error": ...}`, and the check that follows judges it.
- Snapshot:
  - 19 needles must be listed, e.g. `spin "Count" = "3"` and `[options: Red | Green | Blue]`.
  - The table cell is named `1 / 2` with value "91".
  - The checked, unchecked and disabled markers are right, and the hidden element is absent.
- Actions:
  - Each reply's `value`, `checked`, `clicked` or `done` must equal the expected value.
  - The page's own `<pre id="log">` must hold 18 exact lines, e.g. `pad up 59,29 after 4 moves`,
    `cap samples 5` and `f2 submitted by go-value`.
  - The caret ends at positions [1, 3].
  - Text editing works without `execCommand` or `Selection.modify` ("acd", "abd").
  - Tab order is checked through `[focused]`.
  - An emoji is typed and deleted whole.
  - A form with an empty required field isn't submitted until the field is filled.
- Press bookkeeping: after each click, refusal, busy page, replay end and auto-answer, the count of `pad up`,
  `dock up` or `frame up` lines must be exactly right.
- Refusals and the timeout contract:
  - Error texts are matched by substring: "covered by (span#veil)", "outside the element", "file input",
    "That is a web view", "No page element matches", "2 page elements match", "outcome is unknown",
    "gone from the page", "page changed", "may not be the only match", and "page has no button" with
    `page_buttons`.
  - A click queued behind a busy page is never run: the count of `ok` lines stays the same.
  - "and the panel's own page never saw that click": the dock panel's log, read from `widget_list`, is exactly
    two pings (`label "ping ping"`, from the click "its page, whose CSP forbids eval, takes a click" and the click
    on Ping that ends the press held on the window's pad) before the refused click and still exactly that after
    it. A log that is not found reads as no line, which fails.
- Page changes and navigation:
  - `page.changed`, `added` and `partial` must match.
  - `navigated` URLs must end with `#bottom`, `#top`, `#evaluated` and `#orca-token=REDACTED&view=2`.
  - "and the snapshot shows no secret either": the dialog's `widget_list` snapshot answers, lists the page's
    `Token link` (the link just clicked, so the snapshot is of that page), and holds no `fixture-secret`.
- The dock panel's script: "webview_run_javascript there runs, or names the page's Content-Security-Policy" takes
  one outcome per platform. On Windows, whose WebView2 does not hold the injected script to the page's policy, the
  reply's `result` is 1. On macOS and Linux the call is refused with text holding `Content-Security-Policy`.
- Dialogs:
  - Closes report `closed`.
  - During a close, 15 `widget_list` calls from a second client must all answer.
  - The permission prompt's buttons are exactly ["Yes", "No"].
  - An auto-answer rule answers an already open prompt, with `uses` 1.
  - The submitted JSON shows up in the plugin's python log.
  - `dialog_answer` cancel returns in under 8 s.
- Persistence, tabs and the journal:
  - `orca_plugins/config.json` holds `{"speed": <random>, "enabled": true}`.
  - Go to tab shows `plugin.mcp_web_fixture.MCP web fixture page`, both in the reply and in `app_state.tab`.
  - The journal records the locators: `within`, `class: webview` and `index` 2.
- Setup wizard: Next navigates to a URL containing `/guide/21/`, the old ref goes stale, a reload replies
  `navigated`, and close replies `closed`.

**Not checked:**
- "the blocked note says to answer it with dialog_answer" runs only when the thread action replies
  `blocked_by_dialog`. Otherwise it is dropped without a word: whether the plugin's thread raises its prompt before
  the call replies is a race in the fixture.
- Loose matchers:
  - "the page keeps no more than the last round's unanswered request" accepts 0 or 1.
  - "a down the page answers too late has no outcome" accepts any error.
  - "a wheel turn on a scroller scrolls it" accepts any scrollTop above 0.
  - "the Home page is listed too" is a heuristic: `homepage` appears and there are more than 20 `[d` refs.
- The journal and the logs are not checked for the token.
- Fixed sleeps stand in for synchronisation: 2 s to outlast a stall before the close section, and 11.5 s before
  "a request with no deadline is given up after the tools' default".
- Timing: the `dialog_answer` checks use `elapsed < 8` bounds. "cancel ends a web dialog whose page is busy, after
  the 3 s" has no lower bound.
- The `window_screenshot` of a page element is used only for its journal entry; the image is never looked at.
- The auto-answer to the fixture dialog (Submit) is judged by the dialog closing and the rule's use count. Nothing
  checks that the plugin received the submit.

**To settle in review:**
- The dock panel's outcome on macOS and Linux. The code's comment says only that WebView2 is not held to the page's
  policy; `webview_run_javascript` runs its code through `eval`, which a policy without `unsafe-eval` forbids where
  the port holds injected scripts to it. The proposal, refused naming the policy on WKWebView and WebKitGTK, is to
  confirm on each.

**Strength:** Strong — nearly every action is checked by an exact reply value and by the effect the page logged,
with exact counts of press releases.

## `test_webview_modal`
`test_webview_modal.py` · Python, 16 checks

**Tests:** Which web view `webview_run_javascript` runs in while modal dialogs are open:
- With no target or by locator, it runs in the topmost modal's page: Preset Bundle, the Export bundle dialog above
  it, and Flushing volumes.
- It refuses the Home tab's web view behind a modal, and the bundle dialog's web view under the export dialog.
- A page that closes its own dialog replies `closed` at once.
- With no modal open, the Home web view takes scripts.

**Decides by:** Exits 1 when a check prints FAIL or on an uncaught exception.
- No modal: Home has a `webview` in `widget_list`, and `1 + 1` with no target returns 2.
- Preset Bundle:
  - One dialog opens.
  - The Home web view by ref is refused with "modal dialog is open".
  - `document.title` with no target is "Preset Bundle".
  - By `{class: webview, within: Preset Bundle}`, `close_btn` exists.
- Export dialog above it:
  - Two dialogs are open.
  - The bundle's web view by locator is refused with "is under".
  - A run with no target has a `url` containing `ExportPresetDialog`.
  - After `dialog_answer close`, one dialog is left.
- Close from the page: clicking `close_btn` replies `closed: true` in under 1.0 s, and no dialog is left.
- Flushing volumes:
  - After clicks on "Add one filament" and "Flushing volumes", "the Flushing volumes dialog opens": `app_state.dialogs`
    lists exactly one dialog, titled `Flushing volumes for filament change`.
  - Its web view leaves `about:blank`: the test waits up to 5 s for `widget_list {target: "dialog"}` to show it at
    `WipingDialog.html`, as it waits for the export dialog's.
  - "its page takes a script": `typeof document.title` with no target returns "string", and the reply's `url`
    holds `web/flush/WipingDialog.html`, the page the dialog loads.
  - After `dialog_answer close`, no dialog is left.

**Not checked:**
- "without a modal, the only web view takes a script" doesn't check which web view ran it; neither `url` nor title
  is read.
- "and does so within a second" is a 1.0 s wall-clock bound, tight for a loaded parallel sweep.
- The waits for the export and flushing dialogs' web views to leave `about:blank` have no check of their own; the
  URL checks after them cover them.
- The added filament isn't checked; only that the Flushing volumes button opened its dialog.

**Strength:** Partial — targeting and refusals for Preset Bundle, Export and Flushing volumes are checked by title,
URL and message, but the no-modal case is not tied to the Home page.
