# Windows, views, printers and web pages

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

Two facts about the shared app and data folder decide several entries here, both measured on Windows:

- **The main window's place carries over within an app's run, and no further.** `start()` gives the main window the
  baseline's 1600×900, normal, and leaves it where the test before moved it, so a test that needs a place sets it.
  The app writes `window_mainframe` only when its main window closes, and the runner's stop ends the app without
  closing it, so a new launch on a job's folder opens the window where the seed put it: 1600×900 at the top left of
  the work area, trimmed to it. After `window_set` to 1300×820 at [100, 50] and a stop, the conf still read `0; 0;
  1600; 900; 0`. No test on a shared folder closes the main window. Neither the Plugins window nor Preferences stores
  a size or a place.
- **A capture without `save_path` stays in the folder.** `window_screenshot` with no `save_path` saves its PNG under
  `<datadir>/automation/out`. A `replay_run` with no `out_dir` writes outside it, to a `replay-<time>` folder in
  `orca_action_replay` in the temporary folder.

## `test_printers`
`test_printers.py` · Python, 46 checks · in `sweep.json` as `{"path": "test_printers.py"}`

**Tests:** Printers other than the default one, in four parts on the job's app, each from `start()` on its printer.
On the Bambu Lab X1 Carbon 0.4 nozzle, `printer_status`, `print_pause`, `print_resume` and `print_stop` with no
printer connected: the test reads `printer_status` first and calls a job command only when it says no printer is
connected. On the Bambu Lab H2D 0.4 nozzle (two extruders) it checks four things. `settings_set` validates a whole
call before any variant switch or flow-type pick. A variant switch asks about unsaved printer changes unless
`reset_all` is given. A plate's `filament_map_mode` and `filament_map` round-trip through `plate_set_settings` and
`plate_get_settings`. The second extruder's `nozzle_volume_type` can be set. On the A1 mini, a flow type that its 0.2
nozzle preset doesn't offer is refused. On the Eryone ER20, `extruders_count` 2 gives two extruders. The H2D, the A1
mini and the ER20 are the subject, not a default: their extruders, nozzle variants and nozzle cards are what the
checks read. The X1 Carbon is chosen for what the folder may keep, not for a behaviour of its own (see **Starts
from**).

The refusals of `settings_set` on a printer each stand for one check of `Tools/PresetTools.cpp`, all made before
anything changes:

| Case | Code path |
|---|---|
| `nozzle_volume_type` with a `printable_height` that does not parse (H2D) | the strict parse of every value, after the nozzle cards' checks (`parse_values()` in `settings_set`): "Invalid value for printable_height" |
| `nozzle_diameter` 0.6 with `printable_height` 300 (H2D) | `nozzle_picks()`'s variant switch with another key: "Set printable_height in a second call" |
| `nozzle_diameter` 0.2 with High Flow on extruder 2 (H2D) | `nozzle_picks()`'s flow type against the card of the preset the switch selects, which offers others (`Sidebar::nozzle_volume_types(target->config, e)`) |
| `nozzle_diameter` 0.2 with High Flow (A1 mini) | the same check where that card offers none: "no nozzle flow type to pick" |
| TPU High Flow on extruder 1, no switch (H2D) | the same check against the card the sidebar shows (`sidebar().nozzle_volume_types(e)`) |
| `extruders_count` 3 (H2D) | `extruders_count_value()`'s refusal while single extruder multi-material is on |

A `nozzle_diameter` 0.6 beside a `printable_height` that does not parse is not a case of its own: `nozzle_picks()`
refuses it as another key beside a variant switch before any value is parsed, so the `printable_height` 300 case
covers it.

**Starts from:** `start(client, printer=X1C)` for the job commands, which installs the X1 Carbon on the first test of
a job that needs it. The X1 Carbon is chosen for what the folder may keep: on the Klipper printer, a machine of the
Orca printer agent an earlier test stored is selected at the start when it is the agent's only one, and
`printer_status` then reads it connected (measured with one machine of those `test_send_gcode` leaves). The Bambu
printer agent selects only a Bambu Lab printer bound to the folder, and the folder has none: it is signed out
(`use_encrypted_token_file`, and no test signs in), the Bambu network plugin is not installed, and no test binds a
printer. Measured on the X1 Carbon with that Orca machine stored, and in the app `test_send_gcode` had just run in,
its two machines stored: `{"connected": false}`. The guard stays, since a person's own folder may hold a bound
printer.

The H2D part starts from `start()` on `Bambu Lab H2D 0.4 nozzle`, `0.20mm Standard @BBL H2D` and two slots of
`Bambu PLA Basic @BBL H2D`, two filaments for two extruders; the ER20 part from `start()` on `Eryone ER20 0.4
nozzle`, `0.20mm Standard @Eryone ER20` and `Generic PLA @System`. `checks.PRINTERS` does not list these two, so the
test installs each first, after `project_new`, with `printer_install` (`{vendor: "BBL", model: "Bambu Lab H2D",
nozzles: ["0.4"]}`, `{vendor: "Eryone", model: "Eryone ER20", nozzles: ["0.4"]}`) when `preset_list` does not list
it installed. The conf remembers each printer preset's nozzle flow types (`nozzle_volume_types`), which a variant
switch restores, and the test leaves High Flow on the H2D's second extruder, so before its checks it sets
`Standard,Standard` on the 0.6 nozzle preset and then on the 0.4 (`settings_set {nozzle_diameter,
nozzle_volume_type}`) and checks the printer and flow types are `Bambu Lab H2D 0.4 nozzle` and `Standard,Standard`.
Measured: a second run on one folder without this started from `Standard,High Flow`. The A1 mini is selected by
`preset_select`, discarding changes: the BBL vendor's presets are loaded, so it needs no install.

**Leaves:** The X1 Carbon, the H2D 0.4 and the ER20 0.4 installed: the BBL and Eryone vendors' profiles, the
H2D's default filaments that `printer_install` installs with it, and each printer's presets remembered in the conf.
The ER20 is selected, with one extruder. The conf's `nozzle_volume_types` holds `Standard,High Flow` for the H2D
0.4 and 0.6 nozzle presets. The A1 mini was selected once and is not installed: `preset_select` adds it to no model
list. An empty project. The next test's `start()` selects its own printer. Nothing is sent, and no printer is bound.

**Decides by:** Exits 1 when a check prints FAIL or on an uncaught exception, which fails its part's scenario and
lets the next part run.
- Job commands (X1 Carbon): `start()` returns `{printer: X1C, process: X1C_STD, filaments: [BAMBU_PLA]}`. The guard:
  `printer_status` replies exactly `{"connected": false}`. Otherwise the check "no printer is connected" fails with
  the printer's `name` and `model` from the reply, and the part ends there, having called no job command.
  `print_pause`, `print_resume` and `print_stop` are each refused `no_printer_connected`, asserted with `checks.py`'s
  `refused`; after each, `printer_status` replies exactly `{"connected": false}` again and `dialogs()` is empty.
- Start (H2D): `start()` returns the H2D's presets as written above, and the printer and flow types read
  `("Bambu Lab H2D 0.4 nozzle", "Standard,Standard")`.
- Validation before change (H2D): the three refusals of the table hold their words, the flow type's naming
  "extruder 2 with the printer preset Bambu Lab H2D 0.2 nozzle" and "not High Flow". After each, the printer and flow
  types are still `("Bambu Lab H2D 0.4 nozzle", "Standard,Standard")`.
- Variant switch (H2D): the switch with High Flow on extruder 2 replies `applied` `{"nozzle_diameter": "0.6,0.6",
  "nozzle_volume_type": "Standard,High Flow"}` and reads `("Bambu Lab H2D 0.6 nozzle", "Standard,High Flow")`; back
  on 0.4 with `Standard,Standard` it reads the start again. With unsaved changes, the reply's `status` must be
  `blocked_by_dialog`. After Discard, or with `reset_all` (then no `status`), the 0.6 preset is selected, no dialog
  is open and `preset_list` marks nothing `modified`.
- Plate grouping (H2D):
  - `filament_map_mode` starts as `Auto For Flush`, with `Manual` among the options offered.
  - `applied`, the read-back `value` and `effective` all equal `Manual`, then `[2, 1]`.
  - `[1, 3]` is refused naming "extruder 3", and the map stays `[2, 1]`.
  - Setting the mode to null reads it back as `None`.
- Flow types and counts (H2D): `nozzle_volume_type` has the `variants` `extruder 1` and `extruder 2` and the value
  `Standard,Standard`. Setting index 1 gives `Standard,High Flow` in the reply's `applied` and on read-back. `TPU
  High Flow` on extruder 1 is refused naming "extruder 1 offers" and "not TPU High Flow", and `extruders_count` 3
  naming "single extruder multimaterial".
- A1 mini: the printer and flow type read `("Bambu Lab A1 mini 0.4 nozzle", "Standard")` before and after a switch
  to 0.2 with a flow type, which is refused naming "extruder 1" and "no nozzle flow type to pick".
- ER20: `start()` returns its presets as written above. `nozzle_diameter` is `0.4` before, `0.4,0.4` with the two
  `variants` after, and `0.4` again after `reset_all`. `applied` equals `{"extruders_count": 2}`.

**Not checked:**
- A connected printer: what each job command publishes, refusals by the printer's state, resume where the printer
  allows it only at the printer, and the status fields are unit cases against the printer agent double
  (`tests/slic3rutils/test_device_manager_integration.cpp`). No test reaches a physical printer: a real printer's
  status and refusals, and a print paused, resumed and stopped, are the maintainer's to check.
- The machine a Klipper printer's print host makes, which `printer_status` reads as connected: a job command there
  would go to the host.
- "Nothing changed" after a refusal is judged by the printer preset's name and the flow types only.
- The unsaved-changes dialog is answered with Discard without checking which dialog it is. The `printable_height`
  of 300 that made the change is never read back.
- On the ER20, only `nozzle_diameter` is read back; no other per-extruder key is.
- Clean-up calls have no check of their own: the 0.4 restore at the end of the unsaved-changes steps, and the H2D's
  `reset_all`, which leaves the flow types: they are the sidebar's picks, remembered per printer preset, not changes
  of the preset.

**Strength:** Strong — preset names, values and refusal texts are checked exactly, and each refusal is checked to
leave the state as it was; the job commands are checked with no printer connected only, since no test reaches a
printer.

## `test_project_open_settings`
`test_project_open_settings.py` · Python, 7 checks

**Tests:** The test launches its own instance on a new data folder whose Preferences say to load a project's
geometry only (`project_load_behaviour` = `load_geometry_only`). It checks that opening a saved project still loads
the process settings saved with it (`layer_height` 0.28), and that the preference stays as it was. It runs on the
default printer, the Generic Klipper Printer 0.4 nozzle: nothing in it is a Bambu Lab printer's own.

**Starts from:** A new data folder of its own, seeded by `seed_datadir()` with the default printer (`MyKlipper 0.4
nozzle`, `0.20mm Standard @MyKlipper`, `Generic PLA @System`), with `project_load_behaviour` written into the seeded
conf before the launch, as Preferences > Load behavior writes it. The test reads both back before its first step:
`app_state.presets` and `preferences_get`. No other test's state reaches the folder.

**Leaves:** Nothing: the folder, with the saved `layer_height.3mf` in it, is deleted at the end unless `--datadir`
names it.

**Decides by:** Exits 1 when a check prints FAIL, when its own launch fails, or on an uncaught exception.
- Start: `app_state.presets` is `{printer: "MyKlipper 0.4 nozzle", process: "0.20mm Standard @MyKlipper", filaments:
  ["Generic PLA @System"]}`.
- Preference: `preferences_get` reads `project_load_behaviour` = `load_geometry_only`, both before and after the
  open.
- Layer height: the process `layer_height` from `settings_get` is checked three times. It is 0.28 after
  `settings_set`, 0.2 after `project_new` drops the presets' changes (the layer height of `0.20mm Standard
  @MyKlipper`), and 0.28 again after `project_open`.
- Open: the `names` in `project_open`'s reply has exactly one entry.

**Not checked:**
- Only `layer_height` is checked. No other process, filament or printer setting saved in the project is read, and
  nothing checks whether the process preset shows as modified after the open.
- The object's name isn't checked. Nor is whether the open asked anything: a job envelope would fail only because
  its `names` is missing.
- `project_save`'s reply isn't checked; the later open implies the file was written.

**Strength:** Strong — the round-tripped layer height, the preset's own layer height and the unchanged preference
are checked exactly, though only one setting is covered.

## `test_sidebar_floating`
`test_sidebar_floating.py` · Python, 4 checks

**Tests:** While the sidebar floats in its own window, `view_set {sidebar_width}` is refused and the width stays unchanged. The test launches its own instance twice on one new data folder. The first run saves `app.window_layout`. The test then marks the sidebar's AUI entry in that layout as floating, so the second run starts with it floating. On Linux both runs start as X11 clients (`GDK_BACKEND=x11`, through XWayland on a Wayland desktop), as the runner's own launches do by default, since a native Wayland app drops the saved floating state.

**Starts from:** A new data folder of its own, seeded by `orca_mcp.py launch` with the default printer, the Generic Klipper Printer 0.4 nozzle; no check reads a preset. The second run starts from the conf the first run wrote, with the sidebar's entry of `window_layout` marked floating by the test. A shared folder's window layout, with a sidebar another test collapsed or sized, never reaches it.

**Leaves:** Nothing: the folder is deleted at the end unless `--datadir` names its root.

**Decides by:** Exits 1 when a check prints FAIL or on an uncaught exception.
- "the app starts" and "the window layout the first run saved has the sidebar": the first launch succeeds, and the `window_layout` in `OrcaSlicer.conf` has a `name=sidebar;` entry. The app writes the conf at its next idle moment and the stop writes nothing (on Windows it kills the app), so the first run is stopped once the conf on disk holds that entry, read with `until()` for up to 30 s; when it never does, the check fails.
- "view_set of sidebar_width while the sidebar floats is refused": the call must raise, with "The sidebar floats in a window of its own" in the message.
- "and the sidebar's width stays as it was": `app_state.view.sidebar_width` equals the value read before.

**Not checked:**
- A native Wayland app is not tested: its saved floating state is dropped, and nothing checks that it starts docked.
- Nothing reads the state to confirm the sidebar really floats; only the refusal text suggests it. No docked control shows that `sidebar_width` is accepted when the sidebar isn't floating.

**To settle in review:**
- That an X11 client is what a sweep on a Wayland desktop should test here. A `--wayland` sweep means native Wayland launches; this test could instead check there that the sidebar starts docked and `view_set {sidebar_width}` is taken. The proposal keeps one behaviour, the floating one, on every platform.

**Strength:** Strong — the refusal text and the unchanged width are checked exactly on every platform.

## `test_set_window`
`test_set_window.py` · Python, 54 checks on Windows and macOS (1 SKIP), 55 on Linux X11 including `--headless`,
45 on Linux Wayland (4 SKIP)

**Tests:** `window_list` and `window_set` on four kinds of window, plus the read side in `app_state.displays`:
- The list a launch starts with: a page of windows, the main window first, with a ref, not modal.
- The main window: size, position, maximize, minimize and restore, a size given while maximized or after a
  minimize from maximized, and a contradictory request.
- The modeless Plugins window, by its ref: listed as not modal and holding a web page; size and position; a
  `window_screenshot` and a `widget_list` by its ref, each of that window alone; `keyboard_press` Escape by its ref
  closes it, and its ref is then stale.
- The modal Preferences dialog: listed last, as modal; by `target: "dialog"` and by a `{label}` locator; it never
  minimizes; and it stays above the main window in the stacking order.
- Action Replay, a window the tools reach: listed with a ref, and closed by Escape sent to that ref.

**Starts from:** `start(client)` on the Generic Klipper Printer 0.4 nozzle, which leaves the Prepare tab and the
main window 1600×900, normal. No check reads a preset, and nothing here is a Bambu Lab printer's own. The window list
is the one `start()` leaves: the main window alone, as it closes every other window, such as the Plugins dialog and
the fixture's window `test_web_ui` leaves open. The checks find Plugins, Preferences and Action Replay by title. The
main window is where the test before left it (see the top of this file), but no check rests on its place: each size,
place and state is given by a call before it is read, and the contradiction is compared with the size read just
before it. The Plugins window lists the plugins an earlier test left (`test_web_ui`'s fixture); its size and place are
set by the call that reads them.

**Leaves:** Nothing the checks change reaches the data folder: the main window's placement is not written, and the
Plugins window, Preferences and Action Replay store no size or place. Under `<datadir>/automation/out`: the
captures of the Plugins window and of the main window, and, off Wayland, the canvas capture after the restore; on
Wayland, a second capture of the main window instead. The project is `start()`'s, empty. The main window is left
normal at 1500×950, the last call's size, where the calls before placed it: the next `start()` gives it the
baseline's size and leaves its place.

**Decides by:** Exits 1 when any check fails or on an uncaught exception. SKIPs never fail it.
- Start: `start()` returns `{printer: KLIPPER, process: KLIPPER_STD, filaments: [GENERIC_PLA]}`.
- The list at the start: `total == len(items)` and `offset == 0`; the first item has `kind: "main"`, a `ref`,
  `modal: false`, and a width and height above 0.
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
  - Minimize replies `minimized` with no `note`. A wait for the 3D frame a minimized window never draws would run to
    its 3 s and reply with a note that the 3D view did not draw; the call has 30 s before the test reads it as hung.
  - Restore replies `normal`, and a canvas `window_screenshot` reports a width > 0.
  - Maximized, minimized, then `normal`: the reply says `normal` with no `note`, and `window_list`, read every 0.1 s
    through the second after the reply, reads `normal` each time.
  - Maximized, minimized, then a size: the reply says `normal` with that size.
- Contradiction: `state: maximized` together with a size raises, and the size in `window_list` is unchanged.
- Plugins window:
  - It is listed within 30 s with a `ref`, `modal: false` and `web: true`.
  - The reply's size is 900×700, or 900 by more than 700 with `size` listed in `adjusted`.
  - "Plugins: the reply has the position": off Wayland the reply's position is [100, 100]; on Wayland, where no
    position is given or known, the reply has no `position` (its fields are `window_list`'s entry, which has no
    `x` or `y` there).
  - `window_list`'s place and size equal the reply's.
  - A `window_screenshot` by its ref (no image) has a `path`, a width above 0, and a size other than that of a
    screenshot of `main` taken after it.
  - `widget_list` by its ref, without web pages, has `"Plugins"` on its first line and no `MainFrame`.
  - `keyboard_press Escape` by its ref removes it from the list within 30 s, and its ref then raises naming
    `window_list`.
- Preferences:
  - It is listed within 30 s with `modal: true` and a `ref`, last in the list.
  - "dialog: window_set sizes the topmost modal dialog": `window_set {target: "dialog", size: [1000, 760]}` replies
    `kind` `dialog` and the size 1000×760, or 1000 by more than 760 with `size` in `adjusted`; `window_list`'s
    Preferences entry then has that width and height. Measured on Windows at 100 % scaling: Preferences opens at
    640×640 with a minimum height of 640, and takes 1000×760 as asked (1000×600 is held to 1000×640 with `size` in
    `adjusted`); the allowance for a held minimum matters only where a platform's Preferences needs more than 760.
  - "dialog: a {label} locator finds it": the `{label: "Preferences"}` call replies `title` `Preferences`. Off
    Wayland it is given the position [60, 60], and the reply's `position` and `window_list`'s Preferences `x` and `y`
    are 60 and 60, with the size from the call before. On Wayland it is given the size 1000×760 again, and
    `window_list`'s width and height equal the reply's.
  - `state: minimized` raises.
  - It closes through `dialog_answer`, and the list then leaves it out.
- Stacking, on Linux X11 only, where libX11 opens the display: the main window is minimized, restored and raised.
  In the X server's stacking order its title must then sit below `Preferences`.
- Action Replay: listed within 30 s of `action_run open_action_replay`, with no `hidden` key, a `ref`, `modal:
  false` and `web: true`; `keyboard_press Escape` by its ref removes it from the list within 30 s.
- Each "within 30 s" is `until()` on `window_list`, which fails its check when the 30 s pass.
- Read side:
  - Every window with a ref has a valid `state` and a boolean `resizable`.
  - `displays` is not empty and has exactly one `primary`.
  - Each display has the keys `index x y width height scale`.

**Not checked:**
- SKIP paths by platform:
  - Windows and macOS run everything except the stacking check, which is a SKIP because `x11_stacking` finds no
    libX11 or display.
  - Linux X11 runs all 55.
  - Linux Wayland (`--wayland`) skips position, both minimize sections (9 checks) and stacking.
  - The Plugins window's checks after "listed with a ref" run only when it has a ref, and Action Replay's Escape
    only when it has one.
- Maximize is checked only by the reply's `state`; neither `window_list` nor the size is read. The minimize and
  work-area checks also trust the reply alone.
- The Plugins screenshot is checked only for a size unlike the main window's. It is not compared with the size
  `window_list` gives for Plugins, and its pixels are never compared.
- "the modal dialog comes last" runs after Plugins has closed. Measured on Windows: the list then holds the main
  window and Preferences alone, so "last" says only that the dialog follows the main window.
- Action Replay's size and position, and what its page shows.
- The work-area fill compares the reply with the work area the app reported in its own first reply. That makes it
  self-referential, and `window_list` isn't read.
- "restore: the 3D view captures" only requires a reported width > 0; no pixels are compared.
- "a contradiction is refused" accepts any error text.
- "and window_list reads it normal throughout the second after" samples a fixed second: no signal tells when the
  window manager is done with a window, and `app_wait {for: "idle"}` waits for jobs and dialogs, not for it. A
  re-zoom later than that second is missed.
- Displays are checked for key presence only; their values aren't compared with the OS.
- The stacking check isn't gated by platform. On a Mac where `ctypes.util.find_library("X11")` succeeds and
  `DISPLAY` is set (XQuartz), it would read XQuartz's stacking order, which lists no Cocoa windows, and fail. I
  could not confirm whether any sweep machine hits this.
- That a placement survives a restart: the app stores the main window's only when the window closes, which no test
  on a shared folder does.

**Strength:** Partial — main-window and dialog sizes and places are read back from `window_list`, and the list's
flags, refs, order and removals are checked exactly, but maximize, minimize and the work-area fill trust the reply,
and Wayland skips about a fifth of the test.

## `test_set_view_camera`
`test_set_view_camera.py` · Python, 36 checks

**Tests:** `view_set`'s camera fields (`look_at`, `zoom`, `camera`, `orbit`) and the `camera_state` it returns,
checked against `app_state.camera`. It covers four views: Prepare (including straight after `project_new`'s queued
fit), Preview, the Design tab's own camera, and the assemble view's first showing. It also covers zoom clamping,
`click_px_per_mm`, a fit after a resize on the Home tab, and a zoom given on Home while a fit is queued.

**Starts from:** `start(client)` on the Generic Klipper Printer 0.4 nozzle, which leaves the main window 1600×900 and
the perspective projection; no check reads a preset, and nothing here is a Bambu Lab printer's own. Then a
`project_new` of the test's own, which the first scenario starts straight after, while the fit it queues has yet to
run. `auto_perspective`, which would change the projection with the view's direction, is no preference a tool writes;
no test turns it on, and the app's default is off. The camera is the one the test before left, and each scenario
sets the camera it reads. The sweep launches the app with `--cad`.

**Leaves:** `use_perspective_camera` on; three canvas captures under `<datadir>/automation/out` (Prepare, assemble and
Home; measured); an empty new project, from its last `project_new`, on the Prepare tab. For the rest of the app's run,
which the next `start()` resets: the main window at 1400×800, from the resize on Home, and the auto-answer rule for
the unsaved changes question. The window's size and the cameras are not stored.

**Decides by:** Exits 1 when a check prints FAIL or on an uncaught exception.
- Start: `start()` returns `{printer: KLIPPER, process: KLIPPER_STD, filaments: [GENERIC_PLA]}`.
- Prepare after `project_new`:
  - The reply echoes `camera` `top`, and `camera_state` has zoom 3, `look_at` [50, 60, 0] and `elevation` 90.
  - "prepare: the 3D view captures": `window_screenshot {target: "canvas"}` replies `source` `canvas`, a width and
    height above 0, and no `approximate` key (a 3D view that comes back blank is marked `approximate`).
  - After that capture forces a frame, `app_state.camera` still has zoom 3 and that `look_at`.
  - "prepare: camera_state has a projection type": `type` is `perspective`, the projection `start()` sets.
- `click_px_per_mm`: zoom divided by `click_px_per_mm` lies in [1, 3].
- Clamping: `zoom=1000` gives 250, and `adjusted` == ["zoom"].
- Orbit: `camera=front, orbit=[30, 20]` gives `elevation` 20 and `azimuth` -60. `look_at` equals the one before,
  and the reply echoes `orbit` and `camera`.
- Preview: the imported cube slices with `outcome` `ok`. Zoom 2 and `look_at` [0, 0, 0] appear in both the reply
  and `app_state.camera`.
- Design (the sweep launches with `--cad`):
  - The zoom is within 1e-3 of `250/1.4^15`.
  - `look_at` is within 1e-3 of `design_describe`'s `modeling_origin`.
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
- An orthographic camera's `camera_state`, and `auto_perspective` turned on: `start()` sets perspective, and no tool
  sets `auto_perspective`.

**Strength:** Partial — Prepare, Preview, assemble and clamping values are exact, but the Design orbit, the resized
fit and the Home fit are checked loosely or only against each other.

## `test_speed_dial_open`
`test_speed_dial_open.py` · Python, 13 checks (plus 1 where KWin answers; the 3 focus checks SKIP where the pointer cannot be moved)

**Tests:** The app goes from Home to Prepare, Design and Preview by tab clicks. After each switch, the page's 3D
view (`OrcaSceneCanvas`) must hold the keyboard focus, Space sent with no target must open the Speed Dial, and
Escape must close it. Where KWin answers on D-Bus, it also checks that KWin draws no title bar on the dial. The
test runs exclusive in the sweep.

**Starts from:** `start(client)` on the Generic Klipper Printer 0.4 nozzle, which leaves the Prepare tab's 3D view and
the main window 1600×900, normal; no check reads a preset. Then `window_set` places the main window at the top left
of the primary display's work area (its `x` and `y` in `app_state.displays`), which the work area may trim; on
Wayland, which takes no position, the window keeps `start()`'s size alone. That placement keeps (0, 0) off the 3D
view, wherever the test before left the window (see the top of this file). The sweep launches the app with `--cad`.

**Leaves:** Nothing in the data folder beyond `start()`'s empty project: the dial is opened and closed without
running an action, so its recent actions are as they were. The system pointer is left at (0, 0).

**Decides by:** Exits 1 when a check prints FAIL or on an uncaught exception, such as a tab missing from
`widget_list`.
- Start: `start()` returns `{printer: KLIPPER, process: KLIPPER_STD, filaments: [GENERIC_PLA]}`.
- The pointer off the view: the 3D view also takes the focus when the pointer enters it, so before each tab click
  the test moves the system pointer to the primary display's top-left corner, (0, 0), and reads the pointer's
  position back from the system: (0, 0). The start placed the main window at the work area's top left, so that
  corner is its top bar or lies outside it, never in the 3D view below the tab bar. It moves the pointer with
  `SetCursorPos` on Windows, `CGWarpMouseCursorPosition` on macOS and `XWarpPointer` on X11, each through `ctypes`,
  and reads it with `GetCursorPos`, `CGEventGetLocation` and `XQueryPointer`. A pointer that does not read back at
  (0, 0) fails the page's focus check.
- Per page:
  - "the tab is shown": `app_state.tab` reads `home` within 30 s of the click on Home's tab, and then the page's tab
    within 30 s of the click on it.
  - The first `[focused]` line in `widget_list` contains `name=OrcaSceneCanvas`.
  - Within 30 s of Space, `window_list` shows a web dialog with no title.
  - Within 30 s of Escape sent to the dial's ref, it is gone.
  - Each "within 30 s" is `until()`, which fails its check when the 30 s pass.
- On a native Wayland display, where an app can neither move the pointer nor read where it is, each focus check
  prints SKIP with that reason instead of passing.
- KWin, on the first page only: KWin's frame height for the app's window with no caption must be at most the dial's
  height in `window_list` + 2.

**Not checked:**
- The focus checks are not made on a native Wayland display.
- Any web dialog with no title counts as the dial, and its content isn't checked, so what it lists, which a plugin
  an earlier test left adds to, plays no part. Escape is sent to the dial's ref, so Escape through the keyboard focus
  isn't tested.
- The KWin title-bar check prints "skip", which isn't a counted check, in two cases:
  - No KWin answers: Windows, macOS and GNOME.
  - KWin's window runner doesn't list the dial. Per the code comment this covers X11 clients, so it includes the
    default Linux sweep and `--headless`.
  It runs only for a native Wayland app on KWin (`--wayland` on Plasma).

**To settle in review:**
- Moving the user's pointer during a sweep on a desktop: the test is exclusive in the sweep, so nothing else runs
  meanwhile, but a person using the machine sees the pointer jump.
- That (0, 0) lies outside the 3D view on every sweep machine: the start puts the window at the work area's top
  left, and the 3D view is below the tab bar in any case; with a monitor at negative coordinates, (0, 0) is still
  the primary's corner.

**Strength:** Partial — opening and closing the dial are checked on every page, and the focus check is made with the
pointer off the view, but the KWin check almost never runs and native Wayland skips the focus checks.

## `test_send_gcode`
`test_send_gcode.py` · Python, 49 checks

**Tests:** Printing and sending through a print host, run against an OctoPrint stub host inside the test process,
on the Generic Klipper Printer 0.4 nozzle: the `send_gcode` Speed Dial action (`orca_command:send_gcode:orca`),
`printer_send` with `plates`, `print_start`, and `printer_test_host`. A printer of the Custom vendor sends through a
print host as its own route: its Print button offers Send G-code and the exports, and no call reaches the Bambu
network, so nothing is switched first. It covers:
- Refusals for a printer with no print host, a plate not sliced, and all plates, which the Print button offers no
  action on.
- `print_start` and `printer_send` opening the host's upload dialog, which the test cancels: nothing is uploaded.
- The host test, answered by the stub, refused with no address, and failing on a port nothing listens on.
- An upload through the action's dialog, with the file name set by `widget_set_value` and Upload pressed by
  `dialog_answer`, tracked in `app_state.uploads`.
- The exported replay of that send, which uploads again.
- HTTP 500 replies, delayed and immediate, which must end as errors, with the queue's error dialog.

**Starts from:** `start(client)` on the Klipper printer, which leaves `use_printer_agents` at a new folder's `false`,
read with `preferences_get`: with printer agents on, the Print button routes through an agent instead of the print
host and could reach a printer, so a value other than false stops the test before any print call. The print-host values
(`host_type`, `print_host`, `use_3mf`) are set on the printer by the scenarios as unsaved changes, so the preset's
saved ones play no part. The machines earlier runs stored (see **Leaves**) play none either: the route follows the
preset's print host, not the machine the app selected.

**Leaves:** In the conf, measured:
- Two machines of the Orca printer agent in `local_machines`, the stub's `127.0.0.1:<port>` and the closed port's,
  with `last_selected_machine` naming the closed port's. Setting a Klipper printer's `print_host` makes the app add
  and select a machine for that address, as the connection settings do for a person, and it keeps them; while the app
  runs, `printer_status` reads that address connected. A later launch with a Klipper printer whose `print_host` is
  empty selects a stored machine only when it is the agent's only one: with these two, the next launch read
  `{"connected": false}`; with one of them alone, the printer connected.
- The upload queue dialog's size, place and column widths (`print_host_queue_dialog_*`).

The project holds the imported cube on plate 0 and an empty plate 1, with the printer's print-host changes unsaved;
the auto-answer rule for the unsaved changes question is in force for the rest of the app's run. The next `start()`
discards the changes and clears the rule; the print-host changes are never saved.

**Decides by:** Exits 1 when a check prints FAIL or on an uncaught exception. Every refusal is asserted by its code
with `checks.py`'s `refused`, then `app_state.uploads` and `dialogs()` are read back. "Nothing sent" below means:
`app_state.uploads` is as before the call, the stub got no file, and `dialogs()` is empty. "Cancelled" means: the
call replies `blocked_by_dialog`, `dialogs()` lists one dialog titled "Send G-code to printer host",
`dialog_answer {button: "Cancel"}` closes it, and then nothing was sent. Measured on the Klipper printer: every
check below passes.
- Start: `start()` returns `{printer: KLIPPER, process: KLIPPER_STD, filaments: [GENERIC_PLA]}`, and
  `use_printer_agents` reads `false`.
- Discovery: `action_list {section: "Slice & Export"}` lists the id `orca_command:send_gcode:orca`.
- No address. `settings_set {type: "printer", values: {host_type: "octoprint", print_host: "", use_3mf: false}}`,
  with the imported cube not sliced (plain G-code, so the dialog proposes a `.gcode` name). The values are the Klipper
  printer's own, so the preset as edited is the preset as stored:
  - The action is refused `no_print_host`; nothing sent.
  - `printer_send {plates: 0}` is refused `no_print_host`; nothing sent.
  - `printer_test_host` is refused `no_print_host`; `dialogs()` is empty.
  - `settings_get {type: "printer", compare: ":saved"}` lists no key: the refused calls changed nothing in the printer
    preset.
  - A Send with no objects, or with the plate sliced, meets the same refusal, and no row makes one: `Ops::print_route()`
    refuses a host upload with no address before it selects a plate or reads the slice (`PrinterOps.cpp`).
- The stub's address set (`print_host` `127.0.0.1:<port>`), the cube still not sliced:
  - `printer_test_host` replies `ok` true and `message` "Connection to OctoPrint is working correctly."; the stub
    answered one more version check (`GET /api/version`); `dialogs()` is empty.
  - `print_start` is refused `not_sliced`; nothing sent.
  - `print_start {plates: "all"}` and `printer_send {plates: "all"}` are each refused `all_plates_not_offered`: the
    Print button of a printer not of Bambu Lab offers Send G-code and the exports, none of them on all plates;
    nothing sent.
  - `plate_add`, then `plate_select {plate: 1}`: plate 1, empty, is current. `print_start {plates: 0}` is refused
    `not_sliced`; nothing sent; `plate_list` reads plate 1 current again, and the call's undo steps are four
    `"select partplate!"`: a change of the current plate logs two (`Plater::select_plate()` one, and
    `PartPlateList::select_plate()` another, as `test_files`' `SELECTED` has it), once as plate 0 is made current
    for the Print button's check and once as the plate that was current is put back, as a refused export that names a
    plate does. Then `plate_select {plate: 0}`.
- Sliced (`plate_slice`, the plate's `outcome` `ok`):
  - `print_start` is cancelled: the Print button's action is Send G-code, the host's upload.
  - `printer_send {plates: 0}` is cancelled.
  - `plate_select {plate: 1}`, then `printer_send {plates: 0}` is cancelled; `plate_list` reads plate 0 current,
    and the call's undo steps are the two `"select partplate!"` of one change of plate: the plate it named stays
    current. Plate 0 stays current for the steps below.
- The stub's gate: for the two sends caught in flight, the stub, once it has read the whole file, holds its answer
  until the test opens its gate, which it does once the row reads (`uploading`, 99), read every 0.2 s for up to 30 s;
  then the row is read every 0.2 s until it ends, for up to 120 s. A read that runs out fails the checks after it.
- Upload answered 200:
  - The row ends `completed` at progress 100 and was seen at (`uploading`, 99) while the stub held its answer.
  - The row has `filename` `mcp_send_ok.gcode` and the stub's port in `host`.
  - The stub received exactly `{"filename": "mcp_send_ok.gcode", "print": "false"}`.
- Replay:
  - The `replay_export` steps equal exactly `action_run, app_wait(dialog), widget_set_value, app_wait(dialog),
    dialog_answer, app_wait`.
  - `replay_run` passes, a new row completes, and the stub has 2 uploads.
- 500 once caught in flight:
  - The row ends `error` with "HTTP 500" in `error`, never read `completed`, and was seen at (`uploading`, 99).
  - An error dialog appears within 30 s, and after `dialog_answer ok`, `app_state.dialogs` is empty.
- 500 at once: the row ends `error`.
- A port nothing listens on (`print_host` set to it): `printer_test_host` replies `ok` false and a `message` starting
  "Could not connect to OctoPrint"; `dialogs()` is empty.

**Not checked:**
- The uploaded file's content. The stub parses only the multipart `filename` and `print` fields, so an empty or
  wrong body would pass.
- The replayed upload's file name.
- The error dialog's text: only that some dialog opened and closed. For the immediate 500, neither the
  "HTTP 500" text nor the dialog is checked; `close_error_dialog`'s result is ignored.
- A Bambu Lab printer made a print host (`bbl_use_printhost`), whose Print button takes Print plate to the host's
  upload by a route of its own; the Klipper printer takes the route of other vendors' printers.
- The Print and storage dialogs of a printer on the Bambu network, and `printer_send`'s `not_sliced` there: no
  test opens them, since they message a physical printer. They are the maintainer's to check on a printer of his.
- A SimplyPrint host's Print all, which only a Bambu Lab printer made a print host offers, and a cloud host's login:
  the stub is an OctoPrint host.
- `printer_status` once a print host is set, which reads the host's address connected: `test_printers`
  covers `printer_status`.
- The host test's message in another language: the sweep runs the app in English.
- Row ids are assumed to be `count + 1`. Different numbering makes `follow` time out, which fails rather than
  passes falsely.

**Strength:** Strong — row states, progress, the file name, the print flag and the exported steps are checked
exactly against an independent stub, and each refusal by its code with the uploads and dialogs read back.

## `test_web_ui`
`test_web_ui.py` · Python, 227 checks (1 conditional)

**Tests:** The GUI tools on web pages, through the side-loaded `mcp_web_fixture` plugin, on the Generic Klipper
Printer 0.4 nozzle; nothing here is a Bambu Lab printer's own. Before the launch, the sweep's setup step
(`--install {datadir}`) copies `mcp_web_fixture.py` and an enabled `.install_state.json` into `orca_plugins` of the
job's data folder.
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

**Starts from:** The fixture plugin installed and enabled by the setup step, which writes it afresh on every run: it
copies the script over the one there and rewrites `.install_state.json` with the five capabilities on and no stored
permission, so a run after a failed one, such as the rerun alone on the folder it failed on, starts from the same
plugin. A test with a setup step gets a new launch of its job's app after it, so the plugin loads at the launch. Then
`start(client)` on the Klipper printer; no check reads a preset. The stored state the checks read is the
plugin's own, written during the run. The configuration saved through the Plugins dialog is a speed drawn for the run,
so a value an earlier run saved does not pass the check. The submitted JSON is looked for in this app's Python log,
`<datadir>/log/python_*_<pid>.log` with the pid the session names, not in the newest log: the app makes its log only
when the plugin first writes to it, so until then the newest is an earlier run's, which holds the same line.

**Leaves:** Measured after a run, and at the next launch on the folder:
- The fixture plugin, installed and enabled, with the threading permission it was refused stored in its
  `.install_state.json`. Every later launch on the job has the main-window tab "MCP web fixture page" after Project,
  and five more actions in the Speed Dial and `action_list` (the page and the four scripts). Nothing opens at
  startup and nothing asks: `window_list` lists the main window alone, and an untargeted `webview_run_javascript` on
  Home still reaches the Home page, as the plugin's page makes its web view only when shown. The test does not remove
  the plugin: a plugin a person installs stays, and the tests after it check what they own, as TESTS.md's rule says
  of the menu items a plugin adds.
- `orca_plugins/config.json` with the fixture dialog capability's entry; after the run it held the capability's
  defaults, speed 0 and enabled false.
- The plugin's Python log in `<datadir>/log`, and its pages' WebView2 data in `<datadir>/webview2` on Windows, where
  `orca_mcp.py` puts the profile.
- The project `start()` made. Its own dialogs are closed. For the rest of the app's run, measured: the Plugins dialog
  with the configuration frame muted and the fixture's modeless window are left open, and the next `start()` closes
  both, which `window_list` lists. The dock panel is closed at the end of its part by its page's Close panel button
  ("the dock panel's page closes the panel": its Ping button is no longer listed), since `start()` closes no dock
  panel and one left open would narrow the 3D view of every test after it.

**Decides by:** Exits 1 when any check prints FAIL or on an uncaught exception. `call()` turns most tool errors
into `{"error": ...}`, and the check that follows judges it. Measured on the Klipper printer: every check passes, the
conditional one not run.
- Start: `start()` returns `{printer: KLIPPER, process: KLIPPER_STD, filaments: [GENERIC_PLA]}`.
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
  - "the page answers again once its stall ends": after the stalled `dialog_answer`, a script with a 30 s
    `timeout_s` replies `result` 1, so the close section starts on a page that is free.
  - The calls whose deadline is what they test keep it: the `timeout_s` 1 of a locator that matches nothing, an up
    whose target is not found, a down and an up the page answers too late, a slow handler and a click queued behind a
    busy page. So do three that run to their deadline by design: the click refused beside the window's page, which stalls past whatever
    deadline the call has (5 s, the panel's page's time to answer); the Mute click (2 s), whose frame stops answering;
    and the Add note click (5 s), whose requests to the muted frame must be given up before the watch's 10 s for the
    relay check to find the watch the last one waiting.
  - "a request with no deadline is given up after the tools' default": a watch a script asks the page's agent for
    itself, with no deadline, is relayed to the muted configuration frame. The "Add note" check runs while it
    waits; then the page's `relaying` count, read again until it is 0 for up to 30 s, must read 0, and no sooner
    than 10 s after the request.
- Every wait for the page, the dialogs, the Python log or `config.json` is `until()`, asked every 0.2 s for up to
  30 s, and one that runs out fails its check.
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
  - The submitted JSON shows up in this app's Python log.
  - `dialog_answer` past its 3 s, judged by the reply, each call given 30 s before the test reads it as hung: a page
    button that shows half a second late is refused with "page has no button", and is not pressed once it shows, as a
    call that looked on would press it. Cancel on a page that is busy, and on one whose Cancel handler runs 10 s,
    replies `closed` true with no `web`: the call ended the dialog itself. A reply with `web` says the page pressed
    its button, which it does only once the call has waited on it: the busy page, free 2 s past the press's deadline,
    then presses its Cancel (the test moves that request's deadline on and takes the button's handler off), and the
    stalling handler replies once it ends; the dialog then stays open.
- Persistence, tabs and the journal:
  - `orca_plugins/config.json` holds `{"speed": <random>, "enabled": true}`.
  - Go to tab shows `plugin.mcp_web_fixture.MCP web fixture page`, both in the reply and in `app_state.tab`.
  - The journal records the locators: `within`, `class: webview` and `index` 2.

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
- "a request with no deadline is given up after the tools' default" accepts any time from 10 s to the 30 s the
  read waits, so a default anywhere in that span passes. Its lower bound shows the request was held only while
  "Add note"'s own requests are pruned before 10 s, about 6 s after the request when the click starts at once; a
  click delayed past that by load would let a request never held pass.
- How long `dialog_answer` looks for a page button: the button shown half a second past its 3 s shows the call had
  stopped by then, and nothing shows it looked that long.
- The `window_screenshot` of a page element is used only for its journal entry; the image is never looked at.
- The auto-answer to the fixture dialog (Submit) is judged by the dialog closing and the rule's use count. Nothing
  checks that the plugin received the submit.
- The speed is drawn from 89 values by the clock, so a saved configuration an earlier run left matches it once in 89
  runs; after a run the file held the defaults, which no draw gives.

**To settle in review:**
- The dock panel's outcome on macOS and Linux. The code's comment says only that WebView2 is not held to the page's
  policy; `webview_run_javascript` runs its code through `eval`, which a policy without `unsafe-eval` forbids where
  the port holds injected scripts to it. The proposal, refused naming the policy on WKWebView and WebKitGTK, is to
  confirm on each.
- The request with no deadline is a `watch`, which the page's agent relays outside its rounds (`relaySend()` in
  `web/mcp/page_agent.js`). A snapshot would sit in its frame's round slot, which the next listing's or click's round
  gives up (`relayRound()`), so the "Add note" check, run during the wait, would pass the check whether or not the
  10 s default held. The tools' default and the pruning are the same `relaySend()` and `prunePending()` code for
  either kind of request. "Add note"'s own requests to the frame have its 5 s and are pruned a second after, before
  the watch is.

**Strength:** Strong — nearly every action is checked by an exact reply value and by the effect the page logged,
with exact counts of press releases.
