# OrcaSlicer GUI architecture

The map of OrcaSlicer's GUI: which object owns what, how the app starts, rebuilds and shuts down,
how pages are built lazily, and where a new dialog, panel, sidebar control, setting, notification,
menu item or source file belongs. Read it before adding a component or when code has to reach
another part of the GUI; the API detail of each area lives in the file the section points to.

Contents: [The stack](#the-stack-orcas-gui-is-built-on) ·
[Component map](#component-map) · [GUI_App](#gui_app) ·
[Close and shutdown](#close-and-shutdown-sequence) · [MainFrame](#mainframe) ·
[Deferred construction](#deferred-construction-lazy-lazypage-stagedbuild-idlescheduler) ·
[Plater and Sidebar](#plater-and-sidebar) · [Settings placement](#settings-placement-paramspanel-paramsdialog-tabs) ·
[ObjectList](#objectlist) · [3D canvas and ImGui](#3d-canvas-imgui-layer-and-notificationmanager) ·
[Background work](#background-work) · [Device pages](#device-and-monitor-pages) ·
[Web UI](#web-based-ui) · [Preferences](#preferences) · [AppConfig](#appconfig) ·
[Where new code goes](#where-new-code-goes) · [Build registration](#build-registration) ·
[Design docs](#design-docs-docshlsd)

## Rules

1. Reach app-wide objects through `wxGetApp()`. `app_config` is non-null for the whole GUI
   lifetime; `plater()` and `mainframe` can be null, and `sidebar()`, `obj_list()`, `model()`
   dereference the plater unchecked — test `plater()` first on any path that can run before the
   main frame exists or after it closes. → [Accessors](#accessors)
2. Deferred code (CallAfter bodies, agent callbacks, timers) that can run during shutdown checks
   `!wxTheApp || wxGetApp().is_closing()` before touching the GUI. → [Close and shutdown](#close-and-shutdown-sequence)
3. Never keep a raw pointer to a `MainFrame` child, a `Tab`, a lazy panel or a cached dialog across
   `GUI_App::recreate_GUI` (language switch); `is_closing()` stays false during it. → [recreate_GUI](#recreate_gui-language-switch)
4. A new top-level tab is a `LazyPage<Panel>` with a `LazyInstance<Panel>` panel; a heavy dialog
   owned by the main frame is a `Lazy<Dlg>` member. Only the start page and the Prepare plater are
   built before the first frame. → [Deferred construction](#deferred-construction-lazy-lazypage-stagedbuild-idlescheduler)
5. Outside `MainFrame`, reach a lazy object only through its statics: `if_built()` for work it can
   live without, `ensure()` only to show or navigate to it, `when_built()` for state it would not
   pull for itself (and for rescale/recolour of staged panels). → [Reaching a lazy object](#reaching-a-lazy-object)
6. In a `StagedBuild` panel, step-built members start null; timers, handlers and the destructor
   check `built()` first; nothing takes focus while off screen. → [Staged construction](#staged-construction-stagedbuild)
7. Background UI construction is a prebuild task run by `IdleScheduler`, never a `wxEVT_IDLE` +
   `RequestMore()` loop or a chain of posted events. → [The idle scheduler](#the-idle-scheduler-idlescheduler-prebuildqueue)
8. A component with Orca rescale / recolour hooks must be reached by the explicit fan-out
   (`MainFrame::on_dpi_changed` / `on_sys_color_changed`, `Plater::msw_rescale` /
   `sys_color_changed`, `Sidebar::msw_rescale` / `sys_color_changed`); nothing calls it otherwise. → [DPI and colour fan-out](#dpi-and-colour-fan-out)
9. Process and model-scope settings are in `ParamsPanel` inside the sidebar; filament and printer
   settings are in the modeless `ParamsDialog`. `get_tab()` returns null until the tab is complete. → [Settings placement](#settings-placement-paramspanel-paramsdialog-tabs)
10. `Plater` and `Sidebar` are pimpl'd: new state goes into `Plater::priv` / `Sidebar::priv`. New
    events are declared next to their emitter with the `Event.hpp` types; a short-lived listener on
    the plater binds through `EventGuard`. → [Plater and Sidebar](#plater-and-sidebar)
11. Docked panes go through `Plater::add_dock_pane` with a stable, untranslated, delimiter-free name;
    the window must be a child of the plater. → [Docking](#docking)
12. UI drawn over the 3D view is ImGui inside `GLCanvas3D`, never a wx child window over the GL
    canvas; ask for a redraw with `set_as_dirty()` / `request_extra_frame()`. → [3D canvas](#3d-canvas-imgui-layer-and-notificationmanager)
13. `NotificationManager` is called on the UI thread only, and its notifications are visible only
    while the plater is shown. → [NotificationManager](#notificationmanager)
14. UI-initiated background work is a `Job` on a `Worker`; slicing is `BackgroundSlicingProcess`;
    network agents call back through `wxGetApp().CallAfter`. UI is touched only on the main thread. → [Background work](#background-work)
15. Device UI pulls state from `DeviceManager` on its own timer and mutates `MachineObject` only on
    the UI thread. → [Device pages](#device-and-monitor-pages)
16. Web UI goes through Orca's hosts (`WebView::CreateWebView`, `WebViewHostDialog`, `WebPanel`,
    `DockPanel`); window operations requested from a script message are deferred and liveness-checked. → [Web UI](#web-based-ui)
17. A preference is a `create_item_*` row in `PreferencesDialog::create_items` that writes
    `app_config` and saves at once; its default goes in `AppConfig::set_defaults`; effects needed
    after the dialog closes go in `GUI_App::open_preferences`. → [Preferences](#preferences)
18. `AppConfig` values are strings: match the key's own convention (`"true"/"false"` or `"1"/"0"`);
    `save()` and every write run on the main thread. → [AppConfig](#appconfig)
19. Every new source file is registered in `src/slic3r/CMakeLists.txt`: `SLIC3R_GUI_SOURCES`, or the
    `if (WIN32)` / `if (APPLE)` / `if (SLIC3R_CAD)` blocks, or the `GUI/DeviceCore` / `GUI/DeviceTab`
    lists. → [Build registration](#build-registration)
20. A new subsystem whose design is not evident from the code gets `docs/HLSD/<subsystem>.md`; a
    change that invalidates an existing HLSD doc updates it in the same PR. → [Design docs](#design-docs-docshlsd)

## The stack Orca's GUI is built on

- **wxWidgets 3.3.2, SoftFever fork.** `deps/wxWidgets/wxWidgets.cmake` fetches
  `https://github.com/SoftFever/Orca-deps-wxWidgets` at tag `v3.3.2` and builds it static
  (`-DwxBUILD_SHARED=OFF`); Flatpak builds build it shared. Linux builds against **GTK3** by default
  (`option(DEP_WX_GTK3 "Build wxWidgets against GTK3" ON)` in `deps/CMakeLists.txt`, `SLIC3R_GTK`
  default `"3"`, Flatpak uses gtk3). GTK2 exists only as an opt-out (`-DDEP_WX_GTK3=OFF`) and loses
  EGL, WebKit2 and DIP pixels. Code guarded for GTK should still compile on GTK2, but GTK3 under X11
  and Wayland is the target. Toolkit and build-option detail: `references/platforms.md`.
- **Asserts are compiled out.** wx is built with `-DwxBUILD_DEBUG_LEVEL=0` and `libslic3r_gui` adds
  `wxDEBUG_LEVEL=0` (under `SLIC3R_STATIC`, `src/slic3r/CMakeLists.txt`). `wxASSERT`/`wxFAIL` vanish
  and `wxCHECK*` return silently, so API misuse shows up as wrong pixels, dropped calls or corrupted
  state, never as an assert dialog.
- **No wx SVG.** `-DwxUSE_NANOSVG=OFF`: `wxBitmapBundle::FromSVG*` does not exist; Orca rasterises
  SVG itself (`BitmapCache`, `create_scaled_bitmap`) — `references/dpi-bitmaps-fonts.md`.
- **Orca's own widget library.** New UI code largely does not use raw wx controls: the owner-drawn
  widgets in `src/slic3r/GUI/Widgets/` (`Button`, `CheckBox`, `ComboBox`, `TextInput`, `SpinInput`,
  `SwitchButton`, `RadioGroup`, `Label`, `DialogButtons`, …) replace them. Reasons: native controls
  cannot follow Orca's look or its app-level dark-mode toggle; on Windows wx's native dark mode does
  not reach anything built on `TaskDialog()` (`wxMessageBox`, `wxMessageDialog`, `wxRichMessageDialog`,
  `wxProgressDialog`) nor the wrapped common dialogs (`wxColourDialog`, `wxFontDialog`, …)
  (`interface/wx/app.h:1434-1443`), so Orca shows the `MsgDialog` family and its own generic
  `Widgets/ProgressDialog` instead; and on GTK the theme's borders bleed through wrapped native
  controls (the widgets call `RemoveButtonBorder` / `RemoveInputBorder` under `__WXGTK__`).
  Plain containers stay raw (`wxPanel`, `wxBoxSizer`, `wxScrolledWindow`).
- **Namespaces.** Most widgets are in the global namespace; a few (`DialogButtons`, `HyperLink`,
  `ProgressDialog`, `RadioBox`, `WebViewHostDialog`, the AMS/device composites) are in
  `Slic3r::GUI`. GUI code inside `Slic3r::GUI` writes `::CheckBox` because `Field.hpp` declares the
  settings-field classes `Slic3r::GUI::CheckBox`, `TextCtrl`, `SpinCtrl`, `Choice`, `StaticText`,
  which an unqualified name finds first once `Field.hpp` is reachable; `::TextInput` and
  `::ComboBox` are qualified the same way by convention (no `Slic3r::GUI` class shadows them).
  Catalog and quirks: `references/orca-widgets.md`.

## Component map

| Component | Type, file | Owns / does | Reach it with |
|---|---|---|---|
| `GUI_App` | `wxApp`; `GUI/GUI_App.hpp/.cpp` | process singletons, startup, `post_init`, app idle handler, dark-mode entry points, `recreate_GUI` | `wxGetApp()` |
| `MainFrame` | `DPIFrame`; `GUI/MainFrame.hpp/.cpp` | borderless main window, top bar / menu bar, tab book, preset tabs, idle prebuild, DPI/colour fan-out | `wxGetApp().mainframe` |
| `Plater` | `wxPanel`, pimpl `Plater::priv`; `GUI/Plater.hpp/.cpp` | the Prepare and Preview page: model, three canvases, AUI docking, slicing, job worker, notifications, context menus | `wxGetApp().plater()` |
| `Sidebar` | `wxPanel`, pimpl `Sidebar::priv`; `GUI/Plater.hpp/.cpp` | printer and filament blocks, `ParamsPanel`, object search + `ObjectList`, settings index | `wxGetApp().sidebar()` (unchecked) / `plater()->sidebar()` |
| `ParamsPanel` | `wxPanel`; `GUI/ParamsPanel.hpp` | process and model-scope `Tab`s, reparented into the sidebar | `wxGetApp().params_panel()` (null-safe) |
| `ParamsDialog` | `DPIDialog`; `GUI/ParamsDialog.hpp` | its own `ParamsPanel` with the filament and printer `Tab`s; modeless | `wxGetApp().params_dialog()` (null-safe) |
| `Tab` family | `GUI/Tab.hpp/.cpp` | preset editors (`TabPrint`, `TabPrintPlate/Object/Part/Layer`, `TabFilament`, `TabPrinter`) | `get_tab(Preset::Type)`, `get_plate_tab()`, `get_model_tab(part)`, `get_layer_tab()` |
| `ObjectList` | `wxDataViewCtrl`; `GUI/GUI_ObjectList.hpp` | plate/object/part tree | `wxGetApp().obj_list()` (unchecked) |
| `GLCanvas3D` | wraps a `wxGLCanvas`; `GUI/GLCanvas3D.hpp` | 3D, preview and assemble rendering, gizmos, ImGui overlays | `plater()->canvas3D()`, `get_current_canvas3D()` |
| `NotificationManager` | `GUI/NotificationManager.hpp` | ImGui notifications drawn in the canvas | `wxGetApp().notification_manager()` (null-safe) |
| Jobs | `GUI/Jobs/` | UI-initiated background tasks | `plater()->get_ui_job_worker()` |
| `MonitorPanel` / `StatusPanel` | `GUI/Monitor.hpp`, `GUI/StatusPanel.hpp` | Device tab | `MonitorPanel::if_built()` / `ensure()` |
| `DeviceManager` / `MachineObject` | `GUI/DeviceCore/DevManager.h` (`DeviceManager`), `GUI/DeviceManager.hpp` (`MachineObject`), parts in `GUI/DeviceCore/Dev*` | device state | `wxGetApp().getDeviceManager()` |
| `NetworkAgent` | `Utils/NetworkAgent.hpp` | printer agent + cloud agents façade | `wxGetApp().getAgent()` |
| Web hosts | `Widgets/WebView`, `Widgets/WebViewHostDialog`, `WebViewDialog.hpp` (`WebViewPanel`), `PrinterWebView`, `WebPanel`, `DockPanel`, `WebDialog` | HTML UI | per class |
| `PreferencesDialog` | `DPIDialog`; `GUI/Preferences.hpp` | app settings | `wxGetApp().open_preferences(tab, highlight)` |
| `AppConfig` | `libslic3r/AppConfig.hpp` | persisted app settings | `wxGetApp().app_config` |
| `PresetBundle` | `libslic3r/PresetBundle.hpp` | presets | `wxGetApp().preset_bundle` |
| `ShortcutRegistry` | `GUI/Shortcuts.hpp` | key bindings | `wxGetApp().shortcuts()` |
| `ActionRegistry` | `GUI/ActionRegistry.hpp` | Speed Dial actions | `wxGetApp().action_registry()` |
| `ImGuiWrapper` | `GUI/ImGuiWrapper.hpp` | the app's ImGui context | `wxGetApp().imgui()` |

## GUI_App

### Entry and construction

`GUI_Run` (`GUI/GUI_Init.cpp`) creates the app by hand: `new GUI_App()`, then
`Slic3r::instance_check(argc, argv, single_instance)` using `app_config`, then
`GUI_App::SetInstance(gui)`, `gui->init_params = &params`, and `wxEntry`. When there are command-line
arguments it passes **only `argv[0]`** to `wxEntry`, because wx reports errors for some file names; the real
arguments travel in `GUI_App::init_params` (`GUI_InitParams`). `IMPLEMENT_APP(GUI_App)` is in
`GUI_App.cpp` and `DECLARE_APP(GUI_App)` in `GUI_App.hpp` inside `Slic3r::GUI`, so `wxGetApp()` is
`Slic3r::GUI::wxGetApp()` — write `GUI::wxGetApp()` from `Slic3r` scope outside `GUI`. It expands to
`*static_cast<GUI_App*>(wxApp::GetInstance())` (`include/wx/app.h:941` **[source]**), so it is valid from
`SetInstance` until wx cleanup nulls the instance.

The constructor (`GUI_App::GUI_App`) runs before `wxEntry`, i.e. before wx is initialised. It creates
the `ImGuiWrapper`, `RemovableDriveManager`, `Downloader`, `OtherInstanceMessageHandler`, then calls
`init_app_config()` early (instance checking needs it) and loads the `ShortcutRegistry` from it.
Nothing that needs a running wx (timers, windows, modal prompts, WebView runtime checks) may go in
the constructor; those belong in `on_init_inner` or `post_init`.

### Startup sequence

`GUI_App::OnInit` wraps `on_init_inner()` in a try/catch (`generic_exception_handle`, then a `return false`
that is never reached because the handler terminates or rethrows — `references/threads-timers-app.md` §Startup).
The order inside `on_init_inner` that contributors depend on:

1. Log target, `::Label::initSysFont()` (the `Label::Head_*/Body_*` font table every widget uses),
   wxInspector plugin registration, `wxInitAllImageHandlers()`, GTK menu-image and log-filter tweaks.
2. `wxEVT_QUERY_END_SESSION` bound on the app: it sends the main frame a vetoable `wxCloseEvent`
   (so the save prompts run), vetoes the session end if that close was vetoed, then calls
   `EndModal(wxID_ABORT)` on every dialog in the global `dialogStack`.
3. `init_label_colours()`, `init_fonts()`, `Update_dark_mode_flag()`; the editor's TLS
   certificate prompt.
4. `load_language()` — language, colour mode and fonts must be initialised before the first UI
   action; the app exits if loading the language fails.
5. Dark-mode initialisation (non-Windows writes `dark_color_mode` from the system appearance;
   Windows calls `MSWEnableDarkMode(DarkMode_Auto)` before `NppDarkMode::InitDarkMode`) —
   `references/colours-dark-mode.md`.
6. `SplashScreen` (if `show_splash_screen`), held in a `wxWeakRef` and advanced with
   `SetText(text, progress)` + `wxYield()` — `references/threads-timers-app.md`.
7. `new PresetBundle`, `new PresetUpdater` and their event bindings; plugin GUI wiring
   (`init_plugin_gui_wiring`); networking (`on_init_network`).
8. GTK with EGL: `wxGLCanvas::PreferGLX()` on X11, before any GL canvas exists —
   `references/webview-gl-aui-media.md`.
9. `mainframe = new MainFrame()` (creates the plater, the tab book, the preset tabs and the lazy
   pages), then `select_tab(TAB_ID_PREPARE or TAB_ID_HOME)` per `starts_on_prepare()`
   (`default_page == "1"`).
10. `obj_list()->init()`, `SetTopWindow(mainframe)`, `plater_->init_notification_manager()`,
    `load_current_presets()`, `mainframe->Show(true)`; the splash is destroyed; `update_mode()`.
11. The app-level `wxEVT_IDLE` handler is bound.

### post_init and the app idle handler

The idle handler bound at the end of `on_init_inner` runs `post_init()` exactly once (guarded by
`m_post_initialized`, and postponed while a WebView script handler is being added), then on every
idle **saves `app_config` if it is `dirty()`**. `post_init` initialises the WebView2 runtime on
Windows (`init_webview_runtime`, before the first WebView), opens command-line files, loads the GL
resources on the Prepare canvas when the app starts on Prepare (when it starts on Home they load
later as an idle task, so Home paints first), starts the idle prebuild
(`MainFrame::prebuild_pages_when_idle`), and `CallAfter`s the config wizard and update checks — the
code comment: on Mac this is "the only way to popup a modal dialog on start without screwing combo
boxes". If the GL context cannot be made current yet, Linux resets `m_post_initialized` so the next
idle retries (a Wayland surface commits late).

### Accessors

| Accessor | Null? |
|---|---|
| `app_config`, `preset_bundle`, `imgui()`, `shortcuts()`, `action_registry()` | created in the constructor or before `MainFrame`; non-null for the GUI lifetime (`preset_updater` is null in the G-code viewer) |
| `mainframe`, `plater()` | null before `MainFrame` exists |
| `sidebar()`, `obj_list()`, `model()` | dereference `plater_` **without a check** |
| `params_panel()`, `params_dialog()`, `notification_manager()` | null-safe (return null without a main frame / plater) |
| `get_tab(Preset::Type)` | null for a tab not found **or not yet `completed()`**; `tabs_list` / `model_tabs_list` are cleared by `MainFrame::shutdown` |
| `get_model_tab(part)`, `get_layer_tab()` | index `model_tabs_list` **without a bounds check** — undefined once `MainFrame::shutdown` has cleared it |
| `getDeviceManager()`, `getAgent()` | may be null; check before use |
| `em_unit()` | app-wide; per-window value via the free `em_unit(wxWindow*)` — `references/dpi-bitmaps-fonts.md` |
| `dark_mode()` | static, recomputed per call — `references/colours-dark-mode.md` |
| `is_closing()`, `is_recreating_gui()`, `input_idle_ms()` | state flags; `input_idle_ms` is fed by `GUI_App::FilterEvent` |

### recreate_GUI (language switch)

`GUI_App::recreate_GUI` sets `m_is_recreating_gui`, destroys the cached Speed Dial dialog (its
translated strings are injected once), calls `mainframe->shutdown()`, swaps the `Field` control pools
(`switch_window_pools()`; the old pools are released only when the old frame is destroyed), creates a
**new `MainFrame`**, `Destroy()`s the old one, reloads presets, shows the new frame and calls
`prebuild_pages_when_idle()` again. `GUI_App::shutdown` returns early while recreating, so
`is_closing()` never becomes true during a language switch.

Consequences: every `MainFrame` child, `Tab`, lazy panel and cached dialog is a new object afterwards.
The `LazyInstance` statics follow automatically (the new frame's holders replace the old ones); raw
pointers do not. Settings fields are recycled through pools: `references/orca-settings-ui.md`.

### Pitfalls

- **Rule:** On startup and shutdown paths, test `plater()` before `sidebar()` / `obj_list()` / `model()`.
  **Why:** those accessors dereference `plater_` unchecked; before `MainFrame` exists they crash.
  ```cpp
  // Wrong: reachable before the main frame exists
  wxGetApp().sidebar().update_presets(Preset::TYPE_PRINTER);
  // Right
  if (Plater* plater = wxGetApp().plater())
      plater->sidebar().update_presets(Preset::TYPE_PRINTER);
  ```
  Cite: `GUI_App::sidebar`, `GUI_App::obj_list`, `GUI_App::model`.
- **Rule:** Do not null-check `app_config` inside the GUI; do keep it on the main thread.
  **Why:** it is created in the `GUI_App` constructor, before `wxEntry`, so it exists for the whole GUI
  lifetime; the hazard is threading ([AppConfig](#appconfig)), not null. Cite: `GUI_App::GUI_App`.
- **Rule:** Do not cache a pointer to a `MainFrame` child or lazy panel in a static or a long-lived
  object. **Why:** `recreate_GUI` destroys the old frame; the cached pointer dangles and `is_closing()`
  does not warn you.
  ```cpp
  // Wrong
  static MonitorPanel* s_monitor = MonitorPanel::ensure();
  // Right: ask each time; the statics follow the new frame's holder
  if (MonitorPanel* monitor = MonitorPanel::if_built()) monitor->jump_to_HMS();
  ```
  Cite: `GUI_App::recreate_GUI`, `LazyInstance` (`Lazy.hpp`).

## Close and shutdown sequence

Orca's side of shutdown, in order:

1. `MainFrame`'s `wxEVT_CLOSE_WINDOW` handler (bound in the constructor) vetoes, when the event can
   be vetoed, if a gizmo is in editing mode, if `Plater::close_with_confirm` (project and preset save
   prompts) is cancelled, or if `GUI_App::check_print_host_queue` refuses.
2. Otherwise: `MarkdownTip::ExitTip()`, `wxGetApp().set_closing(true)` (so queued work is inert during
   the reset), `m_plater->reset()` (which also saves the AUI perspective to the `window_layout` key),
   `MainFrame::shutdown()`, `event.Skip()` (wx's default handler then `Destroy()`s the frame, or — for a
   vetoable close while a modal dialog is open — vetoes it after this teardown, `references/windows-dialogs.md` §2).
3. `MainFrame::shutdown()`: stops the idle scheduler (`m_idle.stop()`), shuts down the built Project
   panel and plugin pages, removes dock panes, clears the backup callback, cancels all UI jobs
   (`get_ui_job_worker().cancel_all()`), unbinds the canvases' handlers (on macOS Cmd+Q delivers a mouse
   event after the close handler), resets canvas volumes, **hides the frame** (paint messages into
   dying windows crashed), stops the 3D-mouse controller and saves its config, shuts down the
   other-instance listener, saves `app_config` if dirty, clears `tabs_list` / `model_tabs_list`, and
   calls `GUI_App::shutdown()`.
4. `GUI_App::shutdown()`: removable-drive manager shutdown, login dialog deleted, then (unless
   recreating the GUI) stop the HTTP server, `set_closing(true)`, plugin manager shutting down,
   printer agent detached and the agent cache cleared.
5. wx deletes all remaining top-level windows, then calls `GUI_App::OnExit`, which stops the HTTP server
   and preset sync, deletes `DeviceManager`, `UserManager` and the network agent.

`m_is_closing` is a `std::atomic<bool>`. There is no drain of queued `CallAfter`s at shutdown: queued
app calls are discarded with the app object, and those that still run see `is_closing()`. (The bounded
`drain_pending_events` belongs to `GUI_App::hot_reload_network_plugin`.) The wx side — windows deleted
before `OnExit` (`interface/wx/app.h:358-371`), `wxTheApp` null in `~GUI_App`, exception policy — is in
`references/threads-timers-app.md`.

- **Rule:** Guard deferred GUI work with `!wxTheApp || wxGetApp().is_closing()`.
  **Why:** after wx cleanup `wxGetApp()` dereferences a null instance (`wxEntryCleanup` resets the
  instance before deleting the app, `src/common/init.cpp:472-487` **[source]**); between the close
  handler and `OnExit` the plater has been reset and windows are dying.
  ```cpp
  // Wrong
  wxGetApp().CallAfter([this, msg] { handle(msg); });
  // Right (as ActionRegistry::init)
  if (!wxTheApp || wxGetApp().is_closing()) return;
  wxGetApp().CallAfter([this, msg] { if (wxGetApp().is_closing()) return; handle(msg); });
  ```
  Cite: `ActionRegistry::init` (plugin source callbacks), `NetworkAgentFactory.cpp`
  (`reject_conflicting_capability`); `GUI_App::init_networking_callbacks` (`message_arrive_fn`) runs
  inside `GUI_App`, so it tests its own `is_closing()` before and inside the `CallAfter`.
- **Rule:** A component that owns a thread, timer, socket or dock pane stops it from
  `MainFrame::shutdown()` (or its own `shutdown()` called from there), not from its destructor alone.
  **Why:** by the time destructors run, the frame is hidden and the plater reset; a timer or thread that
  fires in between touches half-destroyed state. `MainFrame::shutdown` is the one place that runs before
  any window is deleted, both on exit and on a language switch.

## MainFrame

### Frame, top bar and menu bar

`MainFrame : DPIFrame` uses `BORDERLESS_FRAME_STYLE` (no `wxCAPTION`; no `wxRESIZE_BORDER` on macOS)
and draws its own title bar. Each platform restores the missing decoration differently (MSW strips
`WS_CAPTION` and handles non-client messages in `MainFrame::MSWWindowProc`; GTK adds
`ResizeEdgePanel`s that start a resize drag; macOS `set_miniaturizable` in `Utils/MacDarkMode.mm`) —
`references/platforms.md`.

Off macOS the title bar is `BBLTopbar` (a `wxAuiToolBar` in the frame's sizer, not an AUI pane) that
hosts the File menu, the Edit/View/Help drop-down submenus, the Calibration menu and undo/redo. On
macOS the same menus are attached to a native `wxMenuBar` (`m_menubar`), with Preferences under
`OSXGetAppleMenu()`. `MainFrame::init_menubar_as_editor` builds the `wxMenu`s once and branches only
where they are attached; `generate_help_menu` builds Help. Menu mechanics, `append_menu_item`,
`MenuFactory` and `BBLTopbar` events: `references/popups-menus.md`.

### The tab book

`m_tabpanel` is Orca's `Notebook` (`GUI/Notebook.hpp`, a `wxBookCtrlBase` with a `ButtonsListCtrl`
header that sends `wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED`). Pages are addressed by **string ids**, the
`TAB_ID_*` macros in `MainFrame.hpp` (`TAB_ID_HOME`, `TAB_ID_DESIGN`, `TAB_ID_PREPARE`,
`TAB_ID_PREVIEW`, `TAB_ID_MONITOR`, `TAB_ID_MONITOR_WEB`, `TAB_ID_MULTI_DEVICE`, `TAB_ID_PROJECT`,
`TAB_ID_CALIBRATION`): `AddPage(id, page, text, bmp_name)`, `InsertPage(n, id, …)`,
`FindPageByName`, `SelectPageByName`, `GetSelectedPageName`, `PositionAfter({ids})`. Use the ids, not
indices: pages come and go per printer and per feature flag.

- The **same `Plater` window is inserted twice**, as Prepare and Preview (`MainFrame::update_layout`).
  The page-changed handler posts `EVT_GLVIEWTOOLBAR_3D` / `EVT_GLVIEWTOOLBAR_PREVIEW` to the plater, so
  "which page" is resolved by id, never by `GetName()` of the window (`MainFrame::select_tab(wxPanel*)`).
- Every other page is a `LazyPage<…>` created in `MainFrame::init_tabpanel`: Home
  (`WebViewPanel`), Device (`MonitorPanel`), web Device (`PrinterWebView`), Multi-device
  (`MultiMachinePage`), Project (`ProjectPanel`), Calibration (`CalibrationPanel`), and Design
  (`DesignPanel`, only under `SLIC3R_CAD` with the feature enabled, order −1 so it is never prebuilt).
- `MainFrame::show_device` inserts and removes the Device, web Device, Multi-device and Calibration
  pages depending on the printer and on `use_printer_agents`; a removed page stays registered but is
  not prebuilt (its `LazyPage::in_book()` is false).
- Plugin pages are appended by `PluginPages::initialize` (`plugin/host/PluginPages.hpp`) with
  namespaced ids (`plugin.<plugin_key>.<name>`) that cannot collide with `TAB_ID_*`. Each is a
  `LazyPage<PluginPage>` with order −1, destroyed when its capability goes away.
  → [Deferred construction](#deferred-construction-lazy-lazypage-stagedbuild-idlescheduler)

### Preset tabs

`MainFrame::create_preset_tabs` creates `TabPrint`, `TabPrintPlate`, `TabPrintObject`, `TabPrintPart`,
`TabPrintLayer` on `m_param_panel`, and `TabFilament`, `TabPrinter` on `m_param_dialog->panel()`.
`add_created_tab` moves the plate tab out of `tabs_list` into `plate_tab`, and the model tabs into
`model_tabs_list`, so `tabs_list` holds print, filament and printer. Placement and the settings
pipeline: [Settings placement](#settings-placement-paramspanel-paramsdialog-tabs),
`references/orca-settings-ui.md`.

### DPI and colour fan-out

`MainFrame::on_dpi_changed` and `MainFrame::on_sys_color_changed` call each component they own
explicitly: the tab book and top bar `Rescale()`, the action buttons, `plater()->msw_rescale()` /
`sys_color_changed()` (which go on to the preview, canvas, sidebar, `MenuFactory` and the cached
select-machine dialog), `m_param_panel->msw_rescale()`, every tab's `sys_color_changed()`,
`MenuFactory::sys_color_changed(m_menubar)`, `WebView::RecreateAll()`; lazy panels only through
`X::when_built(...)` and built dialogs through `X::if_built()` (`DiffPresetDialog`). A panel or cached
dialog that is not reached from this chain never runs its `msw_rescale` / `sys_color_changed`. The
DPI mechanics are in `references/dpi-bitmaps-fonts.md`; the colour path (and why Windows reaches it
through `force_color_changed`) is in `references/colours-dark-mode.md`.

- **Rule:** When you add a panel with `msw_rescale()` / `on_sys_color_changed()` hooks, add it to the
  fan-out of its owner in the same change.
  **Why:** child panels are not top-level windows and get no DPI handling of their own from
  `DPIAware`; on Windows the dark-mode toggle reaches components only through this chain.
  ```cpp
  // Right (MainFrame::on_dpi_changed): lazy panels through the statics
  CalibrationPanel::when_built([](CalibrationPanel& calibration) { calibration.msw_rescale(); });
  // Right (MainFrame::on_sys_color_changed): a lazily built dialog
  if (DiffPresetDialog* dialog = DiffPresetDialog::if_built())
      dialog->on_sys_color_changed();
  ```
  Cite: `MainFrame::on_dpi_changed`, `MainFrame::on_sys_color_changed`, `Plater::msw_rescale`.

## Deferred construction (Lazy, LazyPage, StagedBuild, IdleScheduler)

Design doc: `docs/HLSD/deferred-page-construction.md`. Startup pays only for what the first frame
shows (the start page and the Prepare plater); every other tab, and heavy dialogs and GL resources,
build on first show or in small units while the user is idle. A click during the idle build waits for
one unit at most. The parts are independent and wx-free where possible (`Lazy`, `StagedBuild`,
`PrebuildQueue` are unit-tested in `tests/slic3rutils`: `test_lazy.cpp`, `test_staged_build.cpp`,
`test_prebuild_queue.cpp`).

### The holder: `Lazy<T>` and `LazyInstance<T>`

`Lazy<T>` (`GUI/Lazy.hpp`) holds a factory and the object it makes: `Lazy(name, order, factory)`.

| Member | Contract |
|---|---|
| `get()` | the object, **null until completely built** (a staged object mid-build is null) |
| `ensure()` | builds whatever is left now (busy cursor + log line) and returns the object; null if the factory returned null or a nested call finds it mid-build |
| `when_built(fn)` | runs `fn` now if built, otherwise once the build completes |
| `build_step()` | one unit: the factory first, then one `StagedBuild` step per call; a nested call (a unit that pumps the loop) does nothing |
| `prebuild_order()` | position in the idle queue; lower first; **negative = never prebuilt** |

The holder does not own the object — its wx parent does. A factory that returns null or a unit that
throws leaves the holder and scheduler able to carry on. `LazyInstance<Self>` is a mixin that gives a
type with one instance app-wide the statics `Self::if_built()`, `Self::ensure()`,
`Self::when_built(fn)`; the `Lazy<Self>` constructor registers itself, and a recreated `MainFrame`'s
holder replaces the old one. All statics are harmless (null / no-op) while no holder exists —
including `when_built`, which then drops `fn`.

### The placeholder page: `LazyPage<Panel>`

`LazyPage<Panel> : wxPanel, Lazy<Panel>` (`GUI/LazyPage.hpp`) is the notebook page (the book needs a
page object to insert and remove by pointer). `LazyPage(parent, name, order, factory)`; the default
factory is `new Panel(parent)`. Its `Show(true)` builds the panel the first time (only once the
top-level frame is shown — `MainFrame::Show` completes the start page on the frame's first show) and
forwards later shows/hides to the panel, so the panel's own `Show()` override stays its activation
hook. A panel built while its page is hidden stays hidden, and `when_built` gives it the dark-UI pass
the frame ran before it existed (`apply_dark_ui_to_lazy_panel`). `pending()` is true only while the page
is in the book.

### Staged construction: StagedBuild

`StagedBuild` (`GUI/StagedBuild.hpp`) splits a constructor too big for one unit: the constructor builds
a skeleton and queues the rest with `add_build_step(fn)`; `add_build_steps_of(child)` forwards a child
panel's steps, and the parent is `built()` only once every child is. Constraints, all from the design:

- members created in steps start null, so a partly built panel can be destroyed;
- timers, event handlers and the destructor that touch step content check `built()` first;
- nothing takes focus while off screen (a unit may run while the user types elsewhere);
- a widget added by a step keeps its place through an empty sizer slot the skeleton creates.

### The idle scheduler: IdleScheduler, PrebuildQueue

`PrebuildQueue` (`GUI/PrebuildQueue.hpp`) orders `LazyBase` tasks by `prebuild_order()` (equal order:
insertion order) and runs one slice of units of the first pending task. `IdleScheduler`
(`GUI/IdleScheduler.hpp/.cpp`, `MainFrame::m_idle`) drives it from a self-owned `wxTimer`:

- it ticks every 250 ms and runs a slice only after 500 ms without user input (`GUI_App::input_idle_ms`,
  stamped by `GUI_App::FilterEvent` for non-command user-input events and main-frame resizes);
- a slice spends at most 40 ms, then the next slice is `StartOnce(5)` — a separate timer message, so
  paint, timers and input queued meanwhile run first. Posting slices as pending events would not do
  that, because wx drains every pending event, including ones posted meanwhile, before the next
  native message (`src/common/appbase.cpp` `wxAppConsoleBase::ProcessPendingEvents` loops until the
  list is empty **[source]**);
- it skips while `wxEventLoopBase::GetActive()->IsYielding()` (a slice inside a `wxYield()` would build
  pages in the middle of the code that yielded) and guards re-entry with `m_in_slice`;
- it stops its timer when nothing is pending, so it costs nothing afterwards.

`MainFrame::prebuild_pages_when_idle` (called from `post_init` and `recreate_GUI`) clears the queue
and registers the GL resources (`GLResourcesPrebuild`), the Prepare settings page one option group at
a time (`ParamsPanel::settings_page_prebuild`), the Prepare layout at the book's page size
(`m_prepare_layout_prebuild`), every lazy page with a non-negative order, and the lazily built
dialogs (`m_diff_dialog`); the queue then runs them by `prebuild_order()`. `MainFrame::shutdown`
stops it. Units should fit in one slice on a fast machine; a constructor over that is staged.

**Platforms.** GTK: a timer that is always due (`g_timeout_add`, default priority,
`src/gtk/timer.cpp`) runs ahead of the lower-priority GLib sources that repaint and that deliver
posted events and idle (wx's single `G_PRIORITY_LOW` idle source, `src/gtk/app.cpp`
`wxApp::WakeUpIdle` **[source]**) — hence the 5 ms gap rather than 0. macOS: wxOSX rejects a 0 ms
timer (`src/osx/core/timer.cpp:74` `wxCHECK_MSG(m_milli > 0, …)` **[source]**; with asserts compiled
out, `StartOnce(0)` silently never fires). Windows: a slice also waits while the native queue holds input
(`GetQueueStatus`), not counting mouse moves, which Windows synthesises when a window appears under the
cursor. GTK GL resources: the prebuild task `gtk_widget_realize`s the hidden canvas before making the
context current, since GTK creates the surface only on realize.

### Reaching a lazy object

| Need | Use |
|---|---|
| work the object can live without (refresh, status update) | `if (X* x = X::if_built()) x->…;` |
| navigating to it or showing it | `X::ensure()->…` (as `MainFrame::jump_to_monitor`) |
| state it would not fetch for itself when constructed; rescale/recolour of a staged panel (null from `if_built()` while mid-build) | `X::when_built([](X& x) { … });` |

A panel that pulls its own state in its constructor only ever needs `if_built()`.

### Usage

The shape to copy for a new tab (`MainFrame::init_tabpanel`):

```cpp
// Panel: one instance app-wide; heavy constructors also derive StagedBuild
class CalibrationPanel : public wxPanel, public StagedBuild, public LazyInstance<CalibrationPanel> { … };

// MainFrame::init_tabpanel: id, order (gaps leave room between neighbours; <0 = never prebuilt)
m_calibration_page = new LazyPage<CalibrationPanel>(m_tabpanel, TAB_ID_CALIBRATION, 30);
m_lazy_pages.push_back(m_calibration_page);
m_tabpanel->AddPage(TAB_ID_CALIBRATION, m_calibration_page, _L("Calibration"), "tab_calibration_active");

// MainFrame::on_dpi_changed / on_sys_color_changed
CalibrationPanel::when_built([](CalibrationPanel& calibration) { calibration.msw_rescale(); });
```

A lazily built dialog is a `Lazy<Dlg>` member of `MainFrame` with `Dlg : DPIDialog,
LazyInstance<Dlg>`, e.g. `m_diff_dialog("compare_presets", 100, [this] { return make_diff_dialog(); })`,
added to the queue in `prebuild_pages_when_idle` if it should prebuild. The panel's constructor must
cope with the main frame already existing and the user being busy elsewhere, and do all its own setup:
the main frame does nothing to a panel after creating it.

### Pitfalls

- **Rule:** Do not `ensure()` a lazy object for optional work.
  **Why:** `ensure()` builds the whole object now under a busy cursor, defeating the deferral for a
  page the user may never open.
  ```cpp
  // Wrong: a DPI change builds the Device tab
  MonitorPanel::ensure()->msw_rescale();
  // Right
  MonitorPanel::when_built([](MonitorPanel& monitor) { monitor.msw_rescale(); });
  ```
  Cite: `MainFrame::on_dpi_changed`.
- **Rule:** In a staged panel, timer and event handlers return early until `built()`.
  **Why:** a step-built member is null until its step runs; the timer can fire, or the book can select
  the page, in between.
  ```cpp
  // Right (MonitorPanel::update_all)
  if (!built())
      return;
  ```
  Cite: `MonitorPanel::update_all`, `MonitorPanel::init_tabpanel` (steps queued before the page is
  added, "where built() must already be false").
- **Rule:** Never take focus while built off screen.
  **Why:** a unit can run while the user is typing in another control; `SetFocus` steals the
  keystrokes.
  ```cpp
  // Wrong
  page->SetFocus();
  // Right (MonitorPanel page-changed handler)
  if (page->IsShownOnScreen())
      page->SetFocus();
  ```
- **Rule:** Put background UI construction into the prebuild queue, not into idle events.
  **Why:** an `wxEVT_IDLE` + `RequestMore()` loop busy-loops the CPU (wxGTK keeps its idle source
  installed while more is requested, `src/gtk/app.cpp` `wxApp::DoIdle` **[source]**), runs inside
  every `wxYield()` (a full yield calls `ProcessIdle()`, `src/common/evtloopcmn.cpp:182-191`
  **[source]**), and builds even while the user is clicking or typing, so the input waits behind it.
  ```cpp
  // Wrong
  Bind(wxEVT_IDLE, [this](wxIdleEvent& e) { /* build the next part */ e.RequestMore(); });
  // Right: a Lazy<…> holder (or a LazyBase task) registered in MainFrame::prebuild_pages_when_idle
  m_idle.add(m_diff_dialog);
  ```
  Cite: `IdleScheduler::tick`, `docs/HLSD/deferred-page-construction.md`.
- **Rule:** A lazy page that can be destroyed while the main frame lives takes a negative order and
  stays out of `m_lazy_pages`.
  **Why:** `m_lazy_pages` and `PrebuildQueue` hold raw `LazyBase*` and nothing removes one
  (`PrebuildQueue` has only `add` and `clear`). The queue calls `pending()` on every task each slice,
  and `prebuild_pages_when_idle` reads every entry of `m_lazy_pages`, so a page destroyed while still
  listed can be read after it is freed. A page only taken out of the book is fine: it stays registered
  and its `pending()` is false (`MainFrame::show_device`).
  ```cpp
  // Right (PluginPages::create_page): order -1, and no m_lazy_pages.push_back
  auto* page = new GUI::LazyPage<PluginPage>(m_parent, name, -1, [capability](wxWindow* parent) {
      return new PluginPage(parent, capability);
  });
  ```
  Cite: `PluginPages::create_page`, `PluginPages::remove_page`.
- **Rule:** Remove several lazy pages from a book left to right.
  **Why:** removing the selected page selects and shows the page before it
  (`references/controls-dataview.md` §Book controls), and showing an unbuilt `LazyPage` while the frame
  is shown builds it. In any other order the page before the selected one can be one removed next,
  built only to be destroyed; left to right it is one that stays (unless the selected page is the
  book's first).
  ```cpp
  // Right (PluginPages::shutdown): m_order is the tabs' left-to-right order
  for (const PluginCapabilityId& id : std::vector<PluginCapabilityId>(m_order))
      remove_page(id);
  ```
  Cite: `PluginPages::shutdown`, `PluginPages::relayout`, `PluginPages::on_plugin_deregister`.

## Plater and Sidebar

### Structure

`Plater` and `Sidebar` (`GUI/Plater.hpp/.cpp`) are pimpl'd (`std::unique_ptr<priv> p`); public methods
forward to `p->`. `Plater::priv` owns the model, `PartPlateList`, the three canvases (`view3D`,
`preview`, `assemble_view` in one sizer inside `panel_3d`), `BackgroundSlicingProcess
background_process`, `PlaterWorker<BoostThreadWorker> m_worker` (`Plater::get_ui_job_worker()`), the
`NotificationManager`, `Mouse3DController`, `MenuFactory menus` and the AUI manager. New private state
and helpers go into `priv` in `Plater.cpp`; the header changes only for a public entry point.

### Event hub and custom events

`Plater::priv::priv` is the hub: it binds Orca events on the canvases (`EVT_GLCANVAS_OBJECT_SELECT`,
`EVT_GLCANVAS_RIGHT_CLICK`, `EVT_GLCANVAS_ARRANGE`, …, posted by `GLCanvas3D::post_event`, which does
`wxPostEvent(m_canvas, …)`) and on the plater itself (`EVT_SLICING_UPDATE`, `EVT_SLICING_COMPLETED`,
`EVT_PROCESS_COMPLETED`, `EVT_EXPORT_BEGAN`, `EVT_GLCANVAS_COLOR_MODE_CHANGED`, …). Events are declared
in the header of the class that emits them (`GLCanvas3D.hpp`, `Plater.hpp`, `NotificationManager.hpp`,
`ParamsDialog.hpp`); `BackgroundSlicingProcess` is handed the ids to post (`set_finished_event`,
`set_export_began_event`).

Payload types are in `GUI/Event.hpp`: `SimpleEvent`, `IntEvent`, `Event<T>`, `ArrayEvent<T,N>`. They
derive from `wxEvent` but set `m_propagationLevel = wxEVENT_PROPAGATE_MAX` (a plain `wxEvent` does not
propagate, a command event does — `interface/wx/event.h:270-273`) and implement `Clone()`, so they can
be posted or queued and travel up to the plater.

```cpp
wxDECLARE_EVENT(EVT_GLCANVAS_ARRANGE, SimpleEvent);   // GLCanvas3D.hpp, next to the emitter
wxDEFINE_EVENT(EVT_GLCANVAS_ARRANGE, SimpleEvent);    // GLCanvas3D.cpp
post_event(SimpleEvent(EVT_GLCANVAS_ARRANGE));        // GLCanvas3D: wxPostEvent on the wxGLCanvas
view3D_canvas->Bind(EVT_GLCANVAS_ARRANGE, [this](SimpleEvent& evt) { … });   // Plater::priv::priv
wxQueueEvent(wxGetApp().plater(), new SimpleEvent(EVT_MODIFY_FILAMENT, filament_info)); // heap, owned (ParamsDialog)
```

Binding, `Skip`, `CallAfter` and cross-thread rules are in `references/events.md` and
`references/threads-timers-app.md`.

- **Rule:** A short-lived object that listens to plater (or canvas) events binds through `EventGuard`
  (`GUI_Utils.hpp`) or unbinds in its destructor, and `Skip()`s.
  **Why:** the plater outlives the listener; a handler left bound runs on a freed object. Dynamic
  handlers run most recently bound first, so a handler that does not `Skip()` hides the event from the
  plater's own handler (dynamically bound handlers are searched in reverse order of registration,
  `docs/doxygen/overviews/eventhandling.h:480`). `EventGuard` stores the functor at a stable
  address, which is what functor `Unbind` matches on (`interface/wx/event.h:967-970`).
  ```cpp
  // Wrong
  wxGetApp().plater()->Bind(EVT_SLICING_UPDATE, [this](SlicingStatusEvent& e) { refresh(); });
  // Right: member EventGuard unbinds when the dialog dies
  m_slicing_guard = EventGuard(wxGetApp().plater(), EVT_SLICING_UPDATE,
                               [this](SlicingStatusEvent& e) { refresh(); e.Skip(); });
  ```
  Cite: `EventGuard` (`GUI_Utils.hpp`), `PlaterWorker` (binds the plater's idle/paint through it).

### Sidebar content

`Sidebar::Sidebar` builds, inside `p->scrolled` (a `wxPanel`; the sidebar is itself the AUI pane
`"sidebar"`):

1. the printer block — title bar, `PlaterPresetComboBox* combo_printer`, bed type
   (`combo_printer_bed`), nozzle/extruder cards (`ExtruderGroup`), sync and connect buttons;
2. the filament block ("Project Filaments") — `combos_filament`, add / delete / edit, purge mode,
   flushing volumes, AMS sync;
3. the **`ParamsPanel` top bar reparented in** (`params_panel->get_top_panel()->Reparent(p->scrolled)`:
   "Process" title, global/object switch, mode view);
4. `p->sizer_params` (proportion 2): the object search box, `ObjectList` and the `ObjectLayers`
   sizer (`ObjectSettings` is created on `p->scrolled`, but its sizer is added only in the
   `#if !NEW_OBJECT_SETTING` branch);
5. the **`ParamsPanel` itself reparented in** with proportion 3.

So the process settings are the full `ParamsPanel` in the sidebar, not a summary group; the process
preset combo is the `TabPrint` page's own `TabPresetComboBox`. `Sidebar::update_presets(type)` refreshes
the combos after a preset change; `Sidebar::jump_to_option(...)` activates a tab row and blinks it;
`Sidebar::settings_index()` (`Search::SettingsIndex`) and `Sidebar::get_searcher()`
(`Search::OptionsSearcher`) are the settings search — `references/orca-settings-ui.md`.
`Sidebar::load_ams_list(obj)` is how device data reaches the filament block.

Spacing constants come from `SidebarProps` (`Plater.hpp`): `TitlebarMargin()`, `ContentMargin()`,
`ContentMarginV()`, `IconSpacing()`, `WideSpacing()`, `ElementSpacing()`, used as
`FromDIP(SidebarProps::ContentMargin())`. A new sidebar control uses them and is added to
`Sidebar::msw_rescale`, `Sidebar::sys_color_changed` and, if mode-dependent, `Sidebar::update_mode`.

### Docking

`Plater::priv` owns `AuiMgr m_aui_mgr` (a `wxAuiManager` subclass whose `CreateFloatingFrame` returns a
themed `FloatFrame : wxAuiFloatingFrame`), managing the plater. Panes: `"sidebar"` (left, no close
button, not top/bottom dockable), `"main"` (`CenterPane()`, the `panel_3d`), `"uv_editor"` (right,
hidden until the texture-displacement gizmo shows it), plus dynamic dock panes. The default perspective
is saved right after `AddPane`; the app-config `window_layout` is applied with
`LoadPerspective(layout, false)` and falls back to the default on failure; `Plater::priv::reset` saves
it back. On Wayland floating is disabled (`wxAUI_MGR_ALLOW_FLOATING` cleared,
`sanitize_window_layout_for_wayland` strips floating state). wx AUI contracts (`Update()` batching,
perspective semantics, floating-frame lifetime): `references/webview-gl-aui-media.md`.

`Plater::add_dock_pane(window, name, caption, dock, size, on_close)` adds a pane: `window` must be a
child of the plater; `dock` is `"left"`, `"right"`, `"bottom"` or `"float"`; `size` is in DIPs; the
name is made unique with `#2`, `#3`…; a saved per-pane layout entry restores its last place. A pane
closed by its own close button is destroyed after `on_close` runs; `remove_dock_pane(window)` destroys
it **without** calling `on_close`; `remove_dock_panes()` runs from `MainFrame::shutdown`.
`show_dock_pane(window, show)` toggles it. `DockPanel : WebPanel` is the plugin pane, named with
`plugin_pane_name(plugin_key, title)` ("stable across sessions … free of wxAuiManager layout
delimiters").

- **Rule:** Name a dock pane with a stable, untranslated identifier free of `|`, `;`, `=` and `\`.
  **Why:** `LoadPerspective` restores only panes whose names match, and wx's own parser hides every
  pane it does not find (`src/aui/framemanager.cpp:1906-1912` **[source]**, contrary to `interface/wx/aui/framemanager.h:562-565`).
  A translated or reused name loses its layout after a language switch or collides.
  ```cpp
  // Wrong
  plater->add_dock_pane(panel, into_u8(caption), caption, "right", size, on_close);
  // Right
  plater->add_dock_pane(panel, plugin_pane_name(plugin_key, title), caption, "right", size, on_close);
  ```
  Cite: `Plater::priv::add_dock_pane`, `DockPanel.hpp`.

### Context menus

Right-click menus are built and cached by `MenuFactory` (`GUI/GUI_Factories.hpp`, `Plater::priv::menus`)
and shown with `Plater::PopupMenu`, which suppresses background-processing updates while the menu tracks
and defers slicing error dialogs (`m_tracking_popup_menu`) to a `CallAfter` after the menu closes. Detail:
`references/popups-menus.md`.

## Settings placement: ParamsPanel, ParamsDialog, Tabs

- `m_param_panel` (a `ParamsPanel`) is created as a `m_tabpanel` child in `MainFrame::init_tabpanel`
  and reparented into the sidebar by `Sidebar::Sidebar` (top bar and body separately). It hosts the
  process tab and the model-scope tabs; `ParamsPanel::switch_to_object` / `switch_to_global` flip the
  sidebar between object and global settings.
- `m_param_dialog` (a `ParamsDialog : DPIDialog`, parented to the plater) owns a second `ParamsPanel`
  with the filament and printer tabs. It is **modeless with emulated modality** (a `wxWindowDisabler`
  while shown); `Popup()`, the close/validation path and where post-edit work goes:
  `references/orca-settings-ui.md` §Where the tabs live; the modality mechanics:
  `references/windows-dialogs.md` §6.

The pipeline from `PrintConfigDef` to `Field`, adding a setting, toggles, search and per-object
overrides: `references/orca-settings-ui.md`.

- **Rule:** Null-check `get_tab()`.
  **Why:** tabs complete after construction, and `MainFrame::shutdown` clears `tabs_list`.
  ```cpp
  // Wrong
  wxGetApp().get_tab(Preset::TYPE_PRINTER)->reload_config();
  // Right
  if (Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER)) tab->reload_config();
  ```

## ObjectList

`ObjectList : wxDataViewCtrl` (`GUI/GUI_ObjectList.hpp`) over `ObjectDataViewModel : wxDataViewModel`
(`GUI/ObjectDataViewModel.hpp`), whose nodes are typed by the `ItemType` bitmask (`itPlate`,
`itObject`, `itVolume`, `itInstanceRoot`, `itInstance`, `itSettings`, `itLayerRoot`, `itLayer`,
`itInfo`). It lives in the sidebar, is initialised by `obj_list()->init()` after the main frame is
created, gets keys through the shortcut registry (`ObjectList::dispatch_shortcut`; on macOS a
`wxAcceleratorTable` regenerated by `update_shortcut_accelerators`, because the native control
delivers no key events), and shows context menus through `MenuFactory` + `Plater::PopupMenu`. Per-object
overrides appear as `itSettings` children that open the model-scope tabs. Model ownership, renderers,
drag and drop, native-vs-generic data view: `references/controls-dataview.md`.

## 3D canvas, ImGui layer and NotificationManager

### GLCanvas3D

`GLCanvas3D` is **not a window**: it wraps a `wxGLCanvas* m_canvas` (`get_wxglcanvas()`) created by
`OpenGLManager`, binds its size/idle/key/mouse/paint/focus/timer handlers in `bind_event_handlers`, and
must be unbound before teardown (`Plater::unbind_canvas_event_handlers`, from `MainFrame::shutdown`).
Rendering is idle-driven: handlers mark `set_as_dirty()`, `request_extra_frame()` or
`schedule_extra_frame(ms)`, and `on_idle` renders. Outgoing events go through `GLCanvas3D::post_event`.
The view, preview and assemble canvases and the UV editor share one `wxGLContext`. Paint/idle/swap
details, the shared-context attribute rule and EGL/GLX: `references/webview-gl-aui-media.md`.

### What is ImGui and what is wx

| Drawn with ImGui inside the canvas | wx windows |
|---|---|
| gizmo panels (`GLGizmoBase::on_render_input_window`), `NotificationManager` and its hint / slicing-progress notifications, the preview layer slider (`IMSlider`), `IMToolbar`, the G-code legend (`GCodeViewer`), plate labels (`PartPlate`), and the overlays in `GLCanvas3D::_render_overlays` (plate-select toolbar, variable-layer-height dialog, 3D navigator, toolbar item windows) | everything outside the canvas: sidebar, tabs, dialogs, top bar, Home/Device/Project pages |

`GLToolbar` is not ImGui: its icons are OpenGL-textured quads; only its item option windows are
ImGui callbacks. ImGui input arrives only through the canvas's own handlers (`ImGuiWrapper::update_mouse_data` /
`update_key_data`), so text entry needs canvas focus; ImGui sizes are physical pixels (scale by
`GLCanvas3D::get_scale()`); ImGui strings are UTF-8 (`_u8L`). wx theming, `DPIDialog`, sizers and
`Widgets/` do not apply there.

- **Rule:** Never place a wx child window over the GL canvas; draw the overlay in ImGui or put a wx
  window beside the canvas.
  **Why:** on GTK the GL canvas is a native child window or, on Wayland, a subsurface drawn outside
  GTK, and a wx child over it does not reliably stack above the GL content.
  ```cpp
  // Wrong
  auto* banner = new wxPanel(canvas->get_wxglcanvas());
  // Right: ImGui from the gizmo / overlay pass, or a sibling of the canvas in the plater layout
  void on_render_input_window(float x, float y, float bottom_limit) override;   // GLGizmoBase
  ```
  Cite: `docs/HLSD/design-tab.md` (sketch banner "a sibling of the canvas, not a child over it").

### NotificationManager

`NotificationManager` (`GUI/NotificationManager.hpp`) is owned by `Plater::priv` and initialised after
the canvas exists (`Plater::init_notification_manager`; notifications pushed before `init()` are
neither shown nor updated). Push with
`push_notification(NotificationType, NotificationLevel, text, hypertext, callback)`;
`NotificationType::CustomNotification` covers one-offs, and a new `NotificationType` is needed only
when the notification must be closed or updated by type (`close_notification_of_type`). Levels order
importance and fading (`RegularNotificationLevel` fades, `ErrorNotificationLevel` never does). It has
no locking, and it draws only while the plater's canvas renders.

- **Rule:** Push notifications from the UI thread, and use a dialog for messages that must be seen
  while Home or Device is shown.
  **Why:** the manager's containers are unsynchronised; a notification pushed while the plater is
  hidden is not drawn until the user returns to Prepare/Preview.
  ```cpp
  // Wrong: inside Job::process or an agent callback
  wxGetApp().notification_manager()->push_notification(text);
  // Right
  wxGetApp().CallAfter([text] {
      if (wxGetApp().is_closing()) return;
      if (NotificationManager* nm = wxGetApp().notification_manager())
          nm->push_notification(NotificationType::CustomNotification,
                                NotificationManager::NotificationLevel::RegularNotificationLevel, text);
  });
  ```

## Background work

| Kind | Mechanism | Back to the UI |
|---|---|---|
| UI-initiated task (arrange, orient, fill bed, send) | `Job` subclass in `GUI/Jobs/`; `replace_job(plater->get_ui_job_worker(), std::make_unique<OrientJob>())`, or a dialog-owned `PlaterWorker<BoostThreadWorker>` | `Job::finalize` and `Ctl::call_on_main_thread`, delivered from the owner window's idle/paint |
| slicing and export | `BackgroundSlicingProcess` (`Plater::priv::background_process`) | `wxQueueEvent(plater, evt.Clone())`; `execute_ui_task` for a synchronous UI call |
| network agents, HTTP, preset sync | agent / io threads | `wxGetApp().CallAfter` + `is_closing()`; agents get `set_queue_on_main_fn` |
| geometry | TBB | no wx calls inside |

The contracts (which side runs what, cancellation, the `eptr` rethrow, deadlock rules, platform stalls)
are in `references/threads-timers-app.md`.

## Device and Monitor pages

Design doc: `docs/HLSD/printer-agent.md`; implementing a printer agent: the `orca-printer-communication`
skill. Data flow:

1. `NetworkAgent` (façade over the active `IPrinterAgent` and the cloud agents) calls the callbacks
   installed by `GUI_App::init_networking_callbacks` (`set_on_message_fn`, `set_on_local_message_fn`,
   `set_on_printer_connected_fn`, `set_queue_on_main_fn`, …) and by `GUI_App::post_init` /
   `restart_networking` (`set_on_ssdp_msg_fn`) on its own threads.
2. Each callback returns if `is_closing()`, then `CallAfter`s a by-value lambda that re-checks
   `is_closing()` and, on the UI thread, updates the `MachineObject` (`parse_json`), refreshes
   `Sidebar::load_ams_list` and `Plater::update_machine_sync_status`. `MachineObject` and
   `DeviceManager` state is main-thread-only.
3. The Device UI is **pull-based**: `MonitorPanel : wxPanel, StagedBuild, LazyInstance<MonitorPanel>`
   (`GUI/Monitor.hpp`) starts its refresh `wxTimer` in its `Show(true)` override, stops it on hide, and
   `on_timer` → `update_all()` reads `DeviceManager::get_selected_machine()` and pushes it into the
   `StatusPanel` (`StatusBasePanel : wxScrolledWindow, StagedBuild`), HMS and media pages inside a
   `Tabbook`. `DeviceManager::start_refresher` / `stop_refresher` follow the main frame's `wxEVT_SHOW`.
4. Camera playback: `MediaPlayCtrl` selects and tears down the stream backend; the wx parent owns the
   rendering window.

New device UI goes inside `MonitorPanel` / `StatusPanel`, reads state on the timer, makes no network
call on the UI path, and stops its timers on hide.

## Web-based UI

| Host | Use |
|---|---|
| `WebView::CreateWebView(parent, url)` (`Widgets/WebView.hpp`) | the sanctioned way to make a browser: backend choice, handlers, user agent, `"wx"` script handler once per view, registration for `WebView::RecreateAll()` theming, a `FakeWebView` stub instead of null on failure. A raw `wxWebView::New` view gets none of these (no theming on colour change, no null safety) |
| `WebViewPanel` (`WebViewDialog.hpp`, `LazyInstance`) | the Home tab |
| `PrinterWebView` (`LazyInstance`) | the web Device tab (Fluidd/Mainsail/printer UIs) |
| `WebViewHostDialog : DPIDialog` (`Widgets/WebViewHostDialog.hpp`) | local-HTML dialogs: `create_webview(resource_path, …)`, pure-virtual `on_script_message(json)`, `handle_common_script_command`, theme user scripts registered once, `apply_theme_live`, `call_web_handler` (C++ → JS). Subclasses include `WebDialog`, `PluginsDialog`, `PluginsConfigDialog`, `SpeedDialWebDialog`, `TerminalDialog`, `PresetBundleDialog`, `ExportPresetBundleDialog` |
| `WebPanel`, `DockPanel : WebPanel` | plugin pages and docked plugin panes |
| `GuideFrame` (`WebGuideDialog.hpp`) | setup wizard |

Script messages arrive synchronously inside the native WebKit delegate / GTK signal on macOS and Linux
(**[source]**; Edge queues them), so a subclass defers every window operation (show, close, create, `EndModal`) with `CallAfter` and
re-checks liveness inside; `handle_common_script_command`'s `close_page` ends the dialog directly and
`call_web_handler` captures `this` in an app `CallAfter`, so a subclass whose lifetime can end first
adds its own guard. Backend rules, creation order, `RunScript` re-entrancy: `references/webview-gl-aui-media.md`.

## Preferences

`PreferencesDialog : DPIDialog` (`GUI/Preferences.hpp`) is a `TabCtrl m_pref_tabs` over the
`PreferencesTab` pages (`General`, `Control`, `Graphics`, `Online`) plus the Associate and Developer pages,
each a `wxFlexGridSizer` of rows built in `PreferencesDialog::create_items` with the
`create_item_title / label / checkbox / combobox / input / spinctrl / decimal_input / button / …`
helpers (title, tooltip, app-config key, …, `wiki_url`). Window focus follows creation order, so rows are
created in display order; an empty tooltip is filled from the title.

- Rows **write `app_config` and `save()` immediately** in their handler; side effects are `param == "…"`
  branches inside the row's handler (`create_item_checkbox`).
- The dialog is opened only through `GUI_App::open_preferences(tab, highlight_option)`, which shows it
  modally in an inner scope (it must be destroyed before `recreate_GUI`), then handles what must happen
  after it closes: canvas focus, reloading the print when sequence options changed, file associations
  on Windows, redraw when a render setting changed, a pending language switch (`load_language`,
  `ActionRegistry::relocalize_builtins`, `recreate_GUI`).
- The Windows-only dark-mode row (`create_item_darkmode`) is described in
  `references/colours-dark-mode.md`.

```cpp
// PreferencesDialog::create_items — a checkbox row bound to an app_config key
auto item_show_splash_scr = create_item_checkbox(_L("Show splash screen"),
                                                 _L("Show the splash screen during startup."), "show_splash_screen");
g_sizer->Add(item_show_splash_scr);
// AppConfig::set_defaults — the default for a fresh config
if (get("show_splash_screen").empty())
    set_bool("show_splash_screen", true);
```

- **Rule:** Put a preference's runtime effect where it belongs: immediate effects in the row handler,
  effects that need the dialog gone (rebuilding the GUI, reloading the print) in
  `GUI_App::open_preferences`.
  **Why:** `recreate_GUI` while the dialog is alive crashed in `~wxDialogBase` (the inner-scope comment
  in `open_preferences`); work done from the row handler runs under the modal loop.

## AppConfig

`AppConfig` (`libslic3r/AppConfig.hpp`) is Orca's own string store, saved as JSON, not `wxConfig`
(comparison with `wxConfig`: `references/strings-i18n-files.md`). Keys live in sections (`"app"` by
default).

| Call | Behaviour |
|---|---|
| `get(key)` / `get(section, key)` | the string, `""` if missing |
| `get_bool(key)` | `get("app", key) == "true" \|\| get("app", key) == "1"` |
| `get_bool(section, key)` | `get(section, key) == "true" \|\| get("app", key) == "1"` — the `"1"` is read from **`"app"`** |
| `set(key, value)`, `set(section, key, value)`, `set_str(section, key, value)`, `set(section, key, bool)` | marks dirty only when the value changes; the `bool` overload writes `"true"`/`"false"`, and a bare `const char*` value selects it — pass a `std::string` or use `set_str` (`references/strings-i18n-files.md` §AppConfig) |
| `set_bool(key, value)` | `"true"`/`"false"` in `"app"` |
| `has(section, key)`, `dirty()`, `save()` | `save()` throws `CriticalException` off the main thread |
| `set_defaults()` | fills missing keys at load (`if (get("k").empty()) set…`) |

Persistence: the app idle handler saves whenever `dirty()` after `post_init`, and `MainFrame::shutdown`
saves if dirty, so a `set` persists on its own; an explicit `save()` is for immediate persistence
(Preferences rows, dark-mode init). Keys use both conventions — `set_bool` keys hold `"true"/"false"`,
others hold `"1"/"0"` (`dark_color_mode`, `default_page`, `sys_menu_enabled`) — so compare with the
key's own convention.

- **Rule:** Read a non-`"app"` boolean with `get(section, key)` and an explicit comparison.
  **Why:** `get_bool(section, key)` accepts `"1"` only from the `"app"` section.
  ```cpp
  // Wrong: false when section/key holds "1"
  bool on = app_config->get_bool("section", "key");
  // Right
  bool on = app_config->get("section", "key") == "1";
  ```
- **Rule:** Write `app_config` on the main thread only.
  **Why:** the storage map is unsynchronised and `save()` throws off the main thread.
  ```cpp
  // Wrong: on a worker or agent thread
  wxGetApp().app_config->set("key", value);
  // Right
  wxGetApp().CallAfter([value] { if (!wxGetApp().is_closing()) wxGetApp().app_config->set("key", value); });
  ```
  Cite: `AppConfig::save`.

## Where new code goes

| You add | Put it | Must also |
|---|---|---|
| Modal dialog | `GUI/<Name>Dialog.hpp/.cpp`, `class X : public DPIDialog` | follow the dialog recipe in `references/windows-dialogs.md` (parent fallback `wxGetApp().mainframe`, `on_dpi_changed`, `SetSizerAndFit`, `UpdateDlgDarkUI` last) |
| Message / confirm box | `MessageDialog`, `RichMessageDialog`, `WarningDialog`, `ErrorDialog`, `InfoDialog` (`MsgDialog.hpp`), or `show_error` / `show_info` (`GUI.hpp`) | never `wxMessageBox`; `show_error` is asynchronous (an app `CallAfter` around an `ErrorDialog`), so pass a parent that outlives the call or none; `show_info` is a synchronous modal `MessageDialog` — `references/windows-dialogs.md` |
| Local-HTML dialog | subclass `WebViewHostDialog` | implement `on_script_message`; reuse `handle_common_script_command`; defer window operations; register user scripts once ([Web UI](#web-based-ui)) |
| Top-level tab | `LazyPage<Panel>` + `TAB_ID_*` in `MainFrame::init_tabpanel` | panel derives `LazyInstance<Panel>` (+ `StagedBuild` if heavy); fan-out hooks with `when_built`; statics outside `MainFrame`; no focus off screen |
| Heavy dialog owned by the frame | `Lazy<Dlg>` member of `MainFrame`, `Dlg : LazyInstance<Dlg>` | register in `prebuild_pages_when_idle` to prebuild; `if_built()` in the colour fan-out |
| Sidebar control | `Sidebar::Sidebar`, state in `Sidebar::priv` | `SidebarProps` spacing; add to `Sidebar::msw_rescale`, `sys_color_changed`, `update_mode` |
| Docked pane | `Plater::add_dock_pane` | window is a plater child; stable name; know that `remove_dock_pane` skips `on_close` |
| Print / filament / printer setting | def in `PrintConfig.cpp`, row in `Tab*::build` | the full checklist in `references/orca-settings-ui.md` |
| Per-object setting | `SettingsFactory::OBJECT_CATEGORY_SETTINGS` / `PART_CATEGORY_SETTINGS` | `references/orca-settings-ui.md` |
| Overlay or tool UI in the 3D view | gizmo `on_render_input_window` or `GLCanvas3D::_render_overlays` | ImGui + `_u8L`; redraw via `set_as_dirty()` / `request_extra_frame()`; GL only in the canvas's current context |
| Transient message about the 3D view | `NotificationManager::push_notification` | UI thread; new `NotificationType` only to close/update by type |
| Main-menu item | the shared `wxMenu` in `MainFrame::init_menubar_as_editor` / `generate_help_menu` | `append_menu_item`, or `append_shortcut_item` when it has a shortcut — `references/popups-menus.md` |
| Context-menu item | `MenuFactory` (`GUI_Factories.cpp`) | show with `Plater::PopupMenu` |
| Speed Dial command | the `NativeCommands` catalog (`NativeCommands.cpp`, `NativeCommand{key, title, group, input, icon, runner}`) | `ActionRegistry` stores and dispatches it |
| Keyboard shortcut | `Shortcut` enum + `shortcut_table` (`Shortcuts.cpp`) | handle in the context's dispatcher; labels from the registry — `references/mouse-keyboard-focus.md`, `docs/HLSD/keyboard-shortcuts.md` |
| Preference | a `create_item_*` row in `PreferencesDialog::create_items`; default in `AppConfig::set_defaults` | effects in the row handler; post-close effects in `GUI_App::open_preferences` |
| Background task | `Job` subclass in `GUI/Jobs/` | UI only in `finalize` / `call_on_main_thread`; poll `was_canceled()` — `references/threads-timers-app.md` |
| Device UI | inside `MonitorPanel` / `StatusPanel` | pull from `DeviceManager::get_selected_machine()` on the timer; no network calls on the UI path |
| Reusable control | `GUI/Widgets/` | `references/orca-widgets.md`, `references/painting-custom-widgets.md` |
| Source files | `src/slic3r/CMakeLists.txt` | [Build registration](#build-registration) |
| Tests for wx-free GUI logic | `tests/slic3rutils/test_<subsystem>.cpp` (as `test_lazy.cpp`, `test_shortcuts.cpp`) | list the file in that suite's `CMakeLists.txt` (`tests/AGENTS.md`) |

## Build registration

`src/slic3r/CMakeLists.txt` defines `SLIC3R_GUI_SOURCES`, the list compiled into `libslic3r_gui`. It
covers everything under `src/slic3r` (`GUI/`, `GUI/Widgets/`, `GUI/Jobs/`, `Utils/`, `Config/`,
`plugin/`), with paths relative to `src/slic3r`. Add a new `.cpp`/`.hpp` pair on consecutive lines
(the list is only roughly alphabetical). Additional places:

| File kind | Where |
|---|---|
| Windows-only sources | `if (WIN32) list(APPEND SLIC3R_GUI_SOURCES …)` (the vendored `GUI/dark_mode/` code lives there) |
| macOS Objective-C++ (`.mm`) and their headers | `if (APPLE) list(APPEND SLIC3R_GUI_SOURCES …)` |
| Design/CAD UI | the `if (SLIC3R_CAD) list(APPEND …)` block; shared code that references it is guarded with `#ifdef SLIC3R_CAD` (the root `CMakeLists.txt` adds the definition) |
| `GUI/DeviceCore/`, `GUI/DeviceTab/` | their own `CMakeLists.txt`, included with `add_subdirectory`, which `list(APPEND SLIC3R_GUI_SOURCES …)` and re-export it with `PARENT_SCOPE` |

- **Rule:** Keep platform-only sources out of the shared list.
  **Why:** an `.mm` file or a Win32-only header in the shared list breaks the other platforms' builds.
  ```cmake
  # Wrong: in the shared set(SLIC3R_GUI_SOURCES …) list
      GUI/GUI_UtilsMac.mm
  # Right
  if (APPLE)
      list(APPEND SLIC3R_GUI_SOURCES
              GUI/GUI_UtilsMac.mm
          )
  endif ()
  ```

## Design docs (docs/HLSD)

Per `AGENTS.md`, the high-level design of a subsystem goes in `docs/HLSD/<subsystem>.md`, describes
the design as it stands (no phases or before/after framing), and is updated in the same PR when a change
invalidates it. Planning output stays in the gitignored `docs/superpowers/`. GUI-relevant documents:

| Doc | Covers |
|---|---|
| `docs/HLSD/deferred-page-construction.md` | `Lazy`, `LazyPage`, `StagedBuild`, `IdleScheduler`, `PrebuildQueue`, GL-resource prebuild; rules for reaching lazy objects, unit size, order |
| `docs/HLSD/keyboard-shortcuts.md` | `KeyChord`, `Shortcut` / `shortcut_table`, `ShortcutRegistry`, contexts and dispatchers, labels, the shortcuts dialog, "Adding a shortcut" |
| `docs/HLSD/design-tab.md` | the Design (CAD) tab: `SLIC3R_CAD` gate, null-guarded hooks in `GLCanvas3D`, Esc-level contract, generated offer table, project persistence |
| `docs/HLSD/printer-agent.md` | `NetworkAgent`, `IPrinterAgent`, `ICloudServiceAgent`, `DeviceManager` / `MachineObject` ownership, camera playback boundary |

The other HLSD documents cover slicing features and profile data (for example `preset-cache.md`, which
explains how system presets load at startup).
