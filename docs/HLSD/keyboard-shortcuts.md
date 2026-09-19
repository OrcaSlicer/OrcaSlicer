# Keyboard Shortcuts

## Why it exists

Key events arrive in several windows (the main frame's char hook, the 3D canvases, the
gizmo manager and the object list), and the same keys are shown again in menu labels,
toolbar tooltips, gizmo names and the shortcuts dialog. The registry is the one table all
of them read. Each binding is defined once; dispatchers look key events up there, labels
are derived from it, and a change the user makes updates all of them.

## Data model

`KeyChord` (`src/slic3r/GUI/KeyChord.hpp`) is one key press: the key code as
`wxEVT_KEY_DOWN` reports it, plus the `wxMOD_*` modifiers held with it. It has two
text forms. The canonical one (`Ctrl+Shift+S`) is platform-neutral and doubles as the wx
accelerator string and the config format. The display one uses translated modifier
names and the command and option glyphs on macOS. `KeyChord::from_event()` turns any wx
key event into the same key code and modifiers, so a chord recorded in the dialog is
equal to the chord a dispatcher builds from the key press.

`Shortcut` is the enum of every user-facing binding. `shortcut_table` in
`src/slic3r/GUI/Shortcuts.cpp` gives each one a config key, a description, a context
mask, a default chord, a `repeatable` flag and a `modifier_variants` flag, in the order
the dialog lists them; a `static_assert` keeps the table and the enum in step.

`ShortcutRegistry` overlays the user's overrides on the defaults and keeps a
chord-to-shortcut index for lookups. It reads and writes the `shortcuts` section of
`AppConfig`. Only overrides are stored, so a default can change between releases
without touching anyone's config; `none` records a shortcut the user unbound.

## Contexts

A key press is looked up in the context of the window that received it.

| Context      | Dispatcher                                                | Examples                      |
|--------------|-----------------------------------------------------------|-------------------------------|
| `Global`     | `MainFrame`'s `wxEVT_CHAR_HOOK`, before any child sees it | New project, camera views     |
| `Plater`     | `GLCanvas3D` of the 3D and assembly views                 | Arrange, gizmo activation     |
| `Preview`    | `GLCanvas3D` of the G-code preview                        | One-layer mode, jump to layer |
| `ObjectList` | the object list                                           | Copy, delete, auto drop       |
| `Painting`   | `GLGizmosManager` while a painting gizmo is open          | Circle, sphere, fill tools    |

A shortcut can belong to several contexts, which is how copy and paste are a single
binding for the canvas and the object list. Two shortcuts can share a chord when their
contexts do not overlap; `C` is the cut gizmo in the 3D view, the G-code window in the
preview and the circle tool while painting. A Global chord is dispatched before every
other context, so the dialog treats it as conflicting with all of them.

A Global shortcut has to include Ctrl or Alt or use a key that types nothing, since a
bare printable key in the frame hook would swallow that character in every text field.
The dialog refuses such chords and `ShortcutRegistry::load()` drops them from the config.
Space counts as typing. The speed dial's default is the one bare Space, and
`MainFrame` leaves it to a focused control that uses Space itself (text fields, buttons,
combo boxes), so it opens the dial from the canvases and the tab strip only.

## Which event a chord matches

Letters, digits and special keys match on `wxEVT_KEY_DOWN`. Its key codes do not depend
on the keyboard layout: the key labelled `Q` on an AZERTY keyboard and the key in the
same position under a Cyrillic layout both report `Q`. Numpad keys fold onto their main
keyboard equivalents, so `Ctrl+1` and `Ctrl+Numpad 1` are one binding.

Punctuation matches on `wxEVT_CHAR`, because only the char event knows which character
a key produced under the active layout. `+` is Shift and `=` on a US keyboard and a key
of its own on a German one, and the binding means the character in both cases. The
canvas looks a key up on key-down first and, when nothing matched, once more on the char
event, for punctuation chords only. The dialog records chords the same way: a printable
non-alphanumeric key pressed with nothing but Shift is taken from the char event that
follows.

wxGTK does not report key auto-repeat, so the canvases share one record of the keys
seen going down and swallow the repeats of every shortcut not marked `repeatable`. Zoom
and undo repeat, for example; a toggle such as Tab does not. The record is shared because
a shortcut can move the focus to another canvas while its key is still held; a key
released while no canvas had the focus is dropped on the next press.

A few shortcuts have `modifier_variants`: Shift or Ctrl added to their binding selects a
step of the same action (1 mm and camera-space moves of the selection, five-step slider
moves). Only a binding without Shift or Ctrl of its own has steps, so no two bindings
share one. `ShortcutRegistry::match()` looks the exact chord up first and only then, when
nothing is bound to it, looks for such a shortcut whose binding is the chord minus those
modifiers, reporting which were added; a binding on Ctrl+Shift+key therefore wins over
the combined step. The Shift and Ctrl steps themselves are reserved. `step_owner()` names
the shortcut they belong to, the capture dialog refuses to assign them, and
`conflicts()` reports exact chords only. A binding made before its key became a stepping
key keeps its chord and shadows that one step. A move or rotation of the selection
started from the keyboard runs until the key that started it is released, or the
canvas loses focus, so a held key is one undo step.

## Labels

Menu labels, toolbar tooltips, gizmo names, the context menu and the shortcuts dialog
read the registry, so a rebinding shows up in all of them. Each tracked menu item keeps
its base label; `MainFrame::update_shortcut_labels()` appends the current binding
again after an edit, which also installs the new wx accelerator.

A chord that is unsafe as a menu accelerator, meaning a bare printable key, is appended
to the label as plain text so the menu cannot take it away from text fields. The macOS
edit menu shows its clipboard and undo entries that way, because a system-menu key
equivalent for Cmd+C would run instead of the text field's own copy.

On macOS the object list receives no key events at all, so its bindings are installed as
a `wxAcceleratorTable`, regenerated from the registry after each edit.

## Editing

The shortcuts dialog has a page per context, each opening with a line that says when its
keys apply. A page lists the shortcuts under the headings of `section_table`, with the
fixed keys that cannot change (mouse buttons, the step modifiers, Esc, the digit keys
that pick a filament) sorted into the same sections. The mouse drag rows describe the
camera actions chosen in Preferences; their button opens Preferences > Control with that
option scrolled into view and focused, instead of editing a key.
Editing a row opens a capture dialog that records the next chord, names the shortcuts it
would take the chord from, and on confirmation unbinds those and binds this one.
Resetting a row asks the same question when its default is now held by another
shortcut, so a reset cannot leave two shortcuts on one chord. Each change is written to
the config at once and pushed to the menus, tooltips and accelerator tables through
`GUI_App::on_shortcuts_changed()`. The dialog opens from the Help menu and Preferences >
Control on the Global page, and from the `?` key on the page of the view that received it.

## Adding a shortcut

1. Add the enum value to `Shortcut` and its row to `shortcut_table`, in the position
   the dialog should list it; the row's section heading is the `section_table` entry
   above it, so a new section needs an entry there too. Pick a default that does not
   collide inside its contexts; the `[Shortcuts]` tests check every default against the
   others.
2. Handle it in the dispatcher of its context: `MainFrame::handle_global_shortcut`,
   `GLCanvas3D::handle_shortcut`, `ObjectList::dispatch_shortcut`, or a gizmo's
   `on_tool_shortcut`. A gizmo that opens on a key sets `m_shortcut` in its constructor.
3. Where the UI shows the key, ask the registry (`display()` for tooltips,
   `accelerator()` for menu labels); no label holds a literal key name.
