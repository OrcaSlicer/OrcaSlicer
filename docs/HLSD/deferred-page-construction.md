# Deferred Page Construction: High Level Design

## Why it exists

The main window is a notebook of tabs. When the first frame appears, one tab is on
screen and the others are not; some of them are opened later in the session, some
never, and some only exist for certain printers. Building a tab before the first frame
adds its cost to every startup, whether or not the tab is used.

This subsystem builds a tab's panel the first time the tab is shown. It also builds the
remaining tabs while the user is idle after startup, in units of tens of milliseconds,
so a click that lands in the middle of one waits for that unit and no longer. Startup
pays only for what the first frame shows, and the other tabs are usually built before
anyone opens them.

## The parts

`src/slic3r/GUI/Lazy.hpp`, `LazyPage.hpp`, `StagedBuild.hpp` and `IdleScheduler.hpp/.cpp`
(with its wx-free `PrebuildQueue.hpp`) are independent. A holder can hold anything a
factory makes, a page is a holder with a placeholder widget, a staged object can live
outside a holder, and the scheduler knows none of them; it runs tasks, which `MainFrame`
makes from the holders.

### Lazy<T>: the holder

Holds an object that a factory makes on first use, under a name for the log and a place
in the idle queue, both given by the owner. The header has no wx dependency; the busy
cursor for an on-demand build lives in `Lazy.cpp` and is skipped when there is no app,
so the holder is unit-tested.

- `get()` is null until the object is completely built; `built()` says the same.
- `ensure()` builds whatever is left now, under a busy cursor, and returns the object, or
  null in the two cases below. A click on an unbuilt tab or a first open of a dialog goes
  through this, and it logs the units and time it took.
- `build_step()` runs one unit of construction and returns true while more remain. The
  first unit is the factory call, and each later unit is one `StagedBuild` step if the
  type has them.
- `when_built(fn)` runs `fn(object)` now if it exists, otherwise once it is built.
- `pending()` says whether the idle prebuild has work here: not built, and the factory
  has not returned null. A null factory result is logged and the holder stays unbuilt.
- A unit that pumps the event loop cannot re-enter the holder; a nested `build_step()`
  does nothing and a nested `ensure()` returns null.
- After a unit throws, the holder and the scheduler still run the next one.

The holder does not own the object; its wx parent does, as for any window. A type with
one instance in the app derives from `LazyInstance<T>`, which points at that instance's
holder (the holder registers itself, and a recreated `MainFrame`'s holder replaces the
old frame's) and gives the type the static entry points the rest of the app uses,
`T::if_built()`, `T::ensure()` and `T::when_built(fn)`. They return null, or do
nothing, while no holder exists, so a caller needs no `mainframe` check.

`built()` is an atomic flag, since a job worker reads it through the statics
(`MainFrame::get_calibration_curr_tab()`); it is set after the object is complete.

`LazyBase` is the holder's type-free interface (`name()`, `built()`, `pending()`,
`build_step()`, `prebuild_order()`) and is what the scheduler side sees.

### LazyPage<Panel>: the placeholder

A `wxPanel` placed in the parent in place of the real panel, and a `Lazy<Panel>` whose
factory makes the panel inside it (by default `new Panel(parent)`). The notebook needs a
page object for the tab to exist and for `show_device()` to insert and remove tabs by
pointer, and the placeholder is that object. `MainFrame` creates every page once, named
after its `TAB_ID_*`, and keeps it for the frame's life; `show_device()` only moves
pages in and out of the book.

- `Show()` is forwarded to the panel, so a panel's own `Show()` override stays its
  activation hook (refresh timers, machine sync) and `SelectPageByName()` works
  unchanged. A show builds the panel unless the frame itself is still hidden, because
  the book selects its first page as it is inserted; `MainFrame::Show()` shows the
  current page again when the frame becomes visible, which builds it.
- `in_book()` says whether the parent notebook currently lists the page, and
  `pending()` is that and not built, so a tab `show_device()` has taken out of the book
  is not prebuilt.
- A panel built while its page is hidden stays hidden, and a completed panel gets the
  dark-mode pass the frame ran before it existed.

The Compare presets dialog is a holder without a page. `MainFrame` keeps a
`Lazy<DiffPresetDialog>` whose factory constructs the dialog and binds its events, the
dialog derives from `LazyInstance`, and its callers use
`DiffPresetDialog::ensure()->show()` and `DiffPresetDialog::if_built()` like a tab's
callers do. Saving a preset refreshes the dialog only while it is shown, since `show()`
reloads the presets.

### StagedBuild: construction in units

A mixin for a panel whose constructor is too big to be one unit. The constructor builds
the skeleton (sizers, and the parts other code may touch) and queues the rest with
`add_build_step()`. `build_step()` runs one step, and `add_build_steps_of(child)`
forwards a child's steps so a nested panel is spread the same way; the parent is built
only once the child is, including steps the child queues later. Steps run in order, on
the main thread. A `Lazy<T>` recognises a staged type at compile time and runs its steps
one per unit.

Nothing may touch what a step builds before the last step has run. In practice:

- nothing paints the panel before it is complete, since a panel built at idle is hidden
  with its page and one built on demand finishes inside `ensure()` before the event loop
  runs again;
- members created in steps are initialised to null in the header, so a partially built
  panel can be destroyed;
- timers and event handlers that use step content check `built()` first
  (`MonitorPanel::update_all()`, `CalibrationPanel::update_all()`), and a child's steps
  are queued before anything in the constructor can fire such a handler;
- a destructor that disconnects from step content checks `built()` first;
- a constructor or step does not take focus while the panel is off screen
  (`IsShownOnScreen()` before `SetFocus()`), since it may run while the user is typing
  elsewhere;
- where a step's widgets must keep their place in a sizer that later steps also fill, the
  constructor adds an empty slot sizer in that position and the step fills the slot
  (`StatusBasePanel`).

### IdleScheduler: when to build

A task is any `LazyBase`: a name for the log, `pending()`, `build_step()` and
`prebuild_order()`. `PrebuildQueue` holds the tasks by order (equal order in the order
added) and runs a slice, the units of the first pending task until it completes, the
budget is spent on the clock it is given, or the interrupt predicate says input arrived.
It has no wx dependency and is unit-tested with a fake clock. A task whose work is gone
is passed over and stays in the queue, so a tab that `show_device()` removes and later
re-inserts is pending again.

`IdleScheduler` drives the queue with the real clock, the app's input timestamp and the
Windows queue check, and logs each unit and slice. Each slice is a timer message: a
period of 250 ms while waiting for the user to go quiet, and a one-shot of zero after a
slice that left work, so the event loop dispatches whatever it has queued (paint,
timers, input) before the next slice runs. Chaining slices with `CallAfter` would not do
this: wx drains pending events fully before the next native message, on every platform.
On GTK the one-shot is 5 ms, because a due GLib timeout runs ahead of the redraw and
idle sources that paint and deliver posted events.
A slice runs only once the user has been idle for the quiet time, and the timer stops
itself once no task is pending. The tick period, quiet time and slice length are
constants in `IdleScheduler.cpp`. A unit cannot be interrupted once started, so the
largest unit bounds click latency on Windows; on the other platforms a click also waits
for the rest of the slice. A unit that pumps the event loop lets the timer fire inside
its own slice, and that tick does nothing.

The tasks are made by their owners. `MainFrame::prebuild_pages_when_idle()` registers
every `LazyPage` the frame created, in or out of the book (`pending()` is false for a
page out of the book), the Compare presets holder, and the Prepare sidebar's settings
page from `ParamsPanel::settings_page_prebuild()`, whose first unit selects the default
tab if none is selected yet and whose later units build one option group each.
`show_device()` only restarts the timer. `MainFrame` owns the scheduler because it owns
everything the tasks build, and clearing the queue with the frame is what keeps a task
from outliving its object.

Idle time comes from `GUI_App::FilterEvent`, which timestamps mouse and keyboard events
(`wxEVT_CATEGORY_USER_INPUT` minus command events, which all claim that category), and
`GUI_App::input_idle_ms()` reports it. On Windows a slice additionally refuses to start
when the message queue holds keyboard, button, touch or pen input. Mouse moves are
excluded because Windows synthesises one whenever a window appears under the cursor,
which every unit does. Other platforms use the timestamp alone.

Once every registered page is built the timer is stopped and the subsystem costs
nothing.

## Rules

**What builds before the first frame.** The Prepare tab's plater, because `post_init()`
needs its GL canvas on screen to initialise OpenGL in every startup state, and the start
page the user configured. Home is built by the first `MainFrame::Show()`, Prepare's
settings page by selecting the tab. `post_init()` passes through the Prepare tab for GL
init under `Freeze()` with `MainFrame::select_prepare_for_gl_init()`, which changes the
selection without the page-changed event, so nothing else is built for that pass.

**Reaching a lazy object.** A caller uses the type's own statics. `T::if_built()` may
return null and is for telling the object something it can live without (a rescale, a
colour change, a status update). `T::ensure()` builds the object and is for navigating
to it or for a caller that is about to show it. A caller that tells the object something
it would not fetch for itself on construction uses `T::when_built()`, which keeps the
message until the object exists. A panel that pulls its state when constructed (the Home
page requests the recent list on load, the Device tab reads the device manager on show)
is reached with `if_built()`; one that cannot pull gets `when_built()`.

**Unit size.** A unit cannot be interrupted, so it should stay within the slice length on
a fast machine. A constructor above that is staged. A single widget above it is the
floor unless the widget itself is split.

**Order.** Cheapest and most likely to be opened first, given where `MainFrame` creates
each holder. The settings page is 0 (the Prepare tab's own content), Home 10, Device 20
(a Bambu user's usual second stop), Calibration 30, Multi-device 40, the web Device
view 50, Project 60 (a second WebView2 instance) and the Compare presets dialog 100;
steps of ten so a new tab takes a value between its neighbours without renumbering
them. `MainFrame::prebuild_pages_when_idle()` registers the tasks once, from
`post_init()`.

**Never prebuilt.** A holder with a negative order, for a tab that few sessions open
and that costs more to build unasked than it saves (the Design tab), and plugin-provided
tabs, which are Python-side and not lazy pages.

## Adopting it

A lazy tab needs:

1. The panel derived from `LazyInstance<Panel>`, since a tab's panel has one instance.
2. A `LazyPage<Panel>*` member in `MainFrame`, created once in `init_tabpanel()` with
   its `TAB_ID_*` as the name, its place by the order rule (and a factory if
   `new Panel(parent)` is not enough), added to `m_lazy_pages`, and used wherever the
   tab is added to or looked up in the notebook.
3. Every use of the panel outside `MainFrame` going through one of the panel's statics,
   chosen by the rule above. When converting an existing member, `grep` for it; the
   compiler finds the rest.
4. A constructor that copes with the frame already existing and the user being busy
   elsewhere, since `wxGetApp().mainframe` is set, the frame may be shown, and the user
   may be typing when a lazy panel is built: no `SetFocus()` while off screen, and
   whatever the constructor did through a `MainFrame` accessor before (a mode update, a
   deferred URL) done in the constructor itself.

To stage a heavy constructor, inherit `StagedBuild`, keep the skeleton in the constructor,
move the rest into `add_build_step()` lambdas in the original order, and follow the
staged-panel rules above. Measure the units. A step that is still one big widget has to
be split inside that widget or accepted as the floor.

Verification is by log. `MainFrame::prebuild_pages_when_idle` lists the queue it
registered, `IdleScheduler::tick` reports each completed task by name at info level and
each slice and unit at debug level, and `Lazy::ensure` reports an object a user built
on demand with the units and time it took. A run from the configured start
page shows every registered page complete, in order, with no unit longer than intended.
A click on a tab mid-prebuild shows the finished panel with an `ensure` line for what was
left, and the slices resume for the remaining tasks once the user is idle again.
