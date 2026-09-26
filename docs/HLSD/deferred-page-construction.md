# Deferred Page Construction: High Level Design

## Why it exists

The main window is a notebook of tabs, and only one of them is on screen when the first
frame appears. Others are opened later in the session, some never, and some only exist
for certain printers. Building every tab before the first frame makes each startup pay for
tabs the user may never open.

This subsystem builds a tab the first time it is shown, and builds the rest in small units
while the user is idle after startup. Startup pays only for what the first frame shows,
the other tabs are usually ready before anyone opens them, and a click that lands in the
middle of the idle build waits for one unit at most. Work that is not a tab, such as a
dialog or the 3D view's GL resources, uses the same machinery.

## The parts

The parts are independent. A holder can hold anything a factory makes, a placeholder page
is a holder with a widget, a staged object can live outside a holder, and the scheduler
knows none of them; it runs tasks, which the main window makes from the holders.

### The holder: `Lazy<T>`

A holder keeps one object and the factory that makes it. The rest of the app reads the
object if it exists, makes sure it exists now when about to show or navigate to it, or runs
something once it exists. A type with one instance in the app gets these as statics
through `LazyInstance<T>`, so callers need no reference to the main window, and all of them
are harmless while no holder exists.

The holder guarantees that callers see the object only once it is completely built. A
build cannot re-enter itself, so a nested request finds nothing yet, and a factory that
returns null or a unit that throws leaves the holder and the scheduler able to carry on.
The holder does not own the object; its wx parent does, as for any window. The holder has
no wx dependency and is unit-tested.

### The placeholder page: `LazyPage<Panel>`

The notebook needs a page object for a tab to exist and for tabs to be inserted and
removed by pointer, and the placeholder is that object. It builds the real panel inside
itself the first time it is shown and forwards showing and hiding afterwards, so a panel's
own show handling stays its activation hook. Nothing builds while the main window is
hidden; the window's first show builds the start page. A page that is out of the book is
not prebuilt. A panel built while its page is hidden stays hidden, and gets the theming
the window applied before the panel existed.

### Staged construction: `StagedBuild`

A constructor too big to be one unit builds a skeleton and queues the rest as steps, which
run one per unit. A child panel's steps can be forwarded to its parent, and the parent is
complete only once the child is. Nothing may use what a step builds before the last step
has run, so staged panels follow these constraints:

- members created in steps start out null, so a partly built panel can be destroyed;
- timers, event handlers and destructors that touch step content check that the panel is
  complete first;
- nothing takes focus while off screen, since a unit may run while the user is typing
  elsewhere;
- a widget added by a step keeps its place in the sizer through an empty slot the skeleton
  creates.

### The scheduler: `IdleScheduler` and `PrebuildQueue`

The queue holds tasks in order, and a slice runs units of the first pending task until
the task finishes, the time budget is spent, or input arrives. It has no wx dependency and
is unit-tested with a fake clock. A task whose work goes away, such as a tab removed from
the book, is skipped, and becomes pending again if the work comes back.

A slice runs only once the user has been idle for a short quiet time, and each slice is its
own timer message, so paint, timers and input queued in between are handled before the
next slice. Posting slices as pending events would not do that, because wx drains every
pending event before the next native message. On GTK a timer that is always due starves
the lower-priority sources that repaint and deliver posted events, so slices are a few
milliseconds apart. On Windows a slice also waits while the native queue holds input,
not counting mouse moves, which Windows synthesizes whenever a window appears under the
cursor. A slice never runs inside a `wxYield()`, where it would build pages in the middle of
the code that yielded. A unit cannot be interrupted, so the largest unit bounds how long a
click can wait.
When nothing is pending the timer stops and the subsystem costs nothing.

The main window owns the scheduler because it owns what the tasks build, and clearing the
queue with the window keeps a task from outliving its object. Each owner provides its own
tasks, such as a tab, a dialog, the Prepare tab's settings page one option group at a
time, the Prepare page's layout at the size the book gives its pages, or the 3D view's GL
resources.

### The 3D view's GL resources

OpenGL is loaded on the Prepare tab's canvas. When the start page is not Prepare, loading
it is an idle task that makes the context current on the hidden canvas, so the start page
paints first and Prepare never appears. Loading it on a shown canvas under `Freeze()` holds
back the start page's paint, and on GTK `Freeze()` cannot hide the canvas, which is a
native child window or a Wayland subsurface drawn outside GTK. A hidden Windows child
window keeps its device context, macOS attaches the context to a hidden view, and GTK
creates the canvas's surface when the widget is realized, so on GTK the task realizes the
canvas first. If the context cannot be made current, the canvas's first render loads the
resources.

## Rules

**Before the first frame.** Only the start page and the Prepare tab's plater are built
before the first frame. Everything else goes through a holder.

**Reaching a lazy object.** Callers use the type's statics. Reading it if built is for
things the object can live without, such as a rescale, a color change or a status refresh.
Making sure it exists is for navigating to it or showing it. Running something once it is
built is for state it would not fetch for itself on construction. A panel that pulls its
state when constructed only ever needs to be read if built.

**Unit size.** A unit should fit in one slice on a fast machine. A constructor over that is
staged, and a single widget over it is accepted unless the widget itself can be split.

**Order.** Tasks run cheapest and most likely to be opened first. Each holder's order is
given where it is created, with gaps so a new task fits between its neighbors. A negative
order is never prebuilt, for something few sessions open that costs more to build unasked
than it saves.

## Adopting it

A lazy tab needs a panel type deriving from `LazyInstance`, a placeholder page the main
window creates once with its order and registers for the idle build, and every use of the
panel outside the main window going through the statics. Its constructor has to cope with
the main window already existing and the user being busy elsewhere, so it takes no focus
while off screen, and it does all of its own setup, since the main window does nothing to a
panel after creating it.

To stage a heavy constructor, keep the skeleton in the constructor, move the rest into
steps in its original order, and follow the staged-construction constraints. Measure the
units; a step that is still one big widget is split inside the widget or accepted.

## Verifying

The log lists the queue when it is registered, reports each completed task at info level
and each slice and unit at debug level, and reports every on-demand build with its units
and time. A task's completion line counts only the slice it finished in. A run from the
configured start page should show every registered task complete in order, with no unit
longer than intended. A click on a tab during the idle build should show an on-demand
build for what was left, with the slices resuming once the user is idle again.
