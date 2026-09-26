# Auto-reload on source change — High Level Design

## Purpose and scope

Keeps OrcaSlicer in sync with an external CAD tool: when a model's source file is
re-exported, the affected objects are reloaded from disk automatically, and
optionally resliced, without the user switching back to OrcaSlicer. Both behaviors
are opt-in (`auto_reload_on_source_change`, `auto_slice_after_reload`, both off by
default).

An optional warning dialog prevents unintended overwrites when there are painted-on 
features (suppor, fuzzy skin, seam). By default the warning is on, and affects both 
manual and automatic reloads. See "Painted features do not survive a reload" below.

The feature has two parts: `SourceFileWatcher` (`src/slic3r/GUI/SourceFileWatcher.{hpp,cpp}`),
which only knows how to detect that a tracked file's content changed, and
`Plater::priv`'s integration of it, which knows what a "reload" and a "slice" are.
Neither half depends on the other's internals; the watcher's callback contract is
"here are the files that changed, tell me if you handled them."

## Watching for a change

A volume's source is a path recorded at import time (`ModelVolume::source.input_file`),
possibly a bare filename if the project was saved without "Store full source file
paths." `SourceFileWatcher::resolve_source_file_path()` falls back to looking next to
the project file in that case, mirroring the manual "Reload from disk" menu item's own
fallback. `Plater::priv::update_source_file_watches()` recomputes this set from the
model on every relevant change (`object_list_changed()`, project load) and hands it to
the watcher; the watcher itself never touches `Model`.

Detecting a change needs two OS-level watches, because neither alone covers both
common export patterns: a directory watch (added for each tracked file's parent
directory) catches a rename-into-place — the temp-file-then-rename pattern most
exporters use — which only shows up as a directory-listing change; a per-file watch
catches an in-place overwrite, which produces no directory event at all. Per-file
watches are skipped on Windows: wx's MSW backend rejects them outright, and
`ReadDirectoryChangesW`'s directory watch already reports in-place writes, so nothing
is lost by skipping them there.

Either watch firing only wakes a debounced comparison — the event handler never
trusts the reported path, because macOS's kqueue backend can report a rename with
just the directory and no filename. The comparison is against a *stamp*,
`(mtime, size)`: mtime alone is whole-second resolution, so a second write landing in
the same wall-clock second as the first would otherwise be invisible. A same-second,
same-size rewrite is still invisible; that's the accepted limit short of hashing
content.

A file whose current stamp reads as missing (deleted, unmounted, or caught between the
old name's removal and the new one's arrival) is not treated as a change: treating a
delete as if it were an edit would fire a reload against a path that isn't there. This
isn't because a rename-into-place is inherently non-atomic -- a single POSIX `rename(2)`
over an existing destination is atomic, and a lookup can never see it as absent because
of that call alone. What isn't guaranteed is that every exporter's "rename-into-place"
is actually one atomic rename: the classic C runtime `rename()` fails if the destination
already exists, so plenty of tools -- especially on Windows -- do `unlink()` then
`rename()` instead; a temp file on a different filesystem than the destination hits
`EXDEV` and falls back to copy-then-delete; and a network share or cloud-sync client can
show a transient gap to a remote watcher even when the writer's own operation was atomic.
Waiting for the file to come back handles all of these the same way, regardless of which
one actually applies.

The debounce window coalesces a burst of events into one 500ms quiet period, but caps
the total delay at 2s from the first event in a burst: a directory with unrelated
activity more frequent than that (a sync client, a build directory) would otherwise
reset the timer forever and the reload would never fire.

## Detection, commit and retry are separate

`changed_source_files()` only reports which tracked files differ from the committed
baseline; it never mutates anything. The caller's reload result decides what happens
next — `commit_source_stamps()` advances the baseline only for files that were
actually reloaded successfully. This split matters because a reload can fail for a
reason that has nothing to do with whether the file changed: an exception from a
still-writing or momentarily locked file, or a volume this build simply can't parse.
Committing the baseline unconditionally would make that failure permanent — the
change would be consumed with nothing left to retry it.

A failed attempt instead records the stamp that failed and backs off: 1.5s, 3s, 6s,
holding at 12s from there, per file. The failed-stamp record is what keeps this from
retrying forever — once a file's stamp stops advancing, `changed_source_files()` sees
the same stamp that already failed and stops reporting it, so a permanently unloadable
file gets exactly one failed attempt per distinct stamp it ever reaches, not one per
timer tick.

## What gets reloaded

The watcher's callback receives the exact set of files that changed.
`Plater::priv::reload_source_files()` selects only the `ModelVolume`s whose resolved
source is in that set, so with several objects loaded, editing one CAD file reimports
only that object, not everything else on the plate. This is narrower than
`reload_all_from_disk()` (still used by the "Reload all" menu item and the canvas
shortcut), which selects every object regardless of which one changed — appropriate
there because the user asked for it explicitly, not appropriate for something that
runs on every detected file change. One instance's `GLVolume` is enough to select a
volume for this: `reload_from_disk()` edits the shared
`ModelObject`/`ModelVolume` directly, so the change reaches every instance regardless
of which one's `GLVolume` triggered the selection. A cloned volume (the Clone tool
deep-copies a `ModelVolume`, source path included, into an independent object) matches
the changed-files set on its own and reloads independently, keeping its own transform.

## Suppressing dialogs on the automatic path

`reload_from_disk()` is written for the manual "Reload from disk" menu item and can
show two dialogs: a file picker for a missing source, and a "replace it?" confirmation.
The watcher doesn't skip these because nobody is at the keyboard — the user is
presumably still there, just not in the middle of clicking "Reload from disk"
themselves. It skips them because of the retry/backoff loop ("Detection, commit and
retry are separate" above): a source that's still missing, or still mid-write and
unparseable, gets retried automatically at 1.5s/3s/6s/12s until its stamp stops
changing. Running interactively would mean showing one of these dialogs again on every
retry that still fails — a file picker demanding attention every few seconds for an
export that simply isn't finished yet, not a one-off interruption the user asked for.

`reload_from_disk()`/`reload_all_from_disk()` take an `interactive` parameter (default
`true`, so the menu item and canvas shortcut are unaffected); the watcher always calls
with `false`. Non-interactively: a missing source's volume is left untouched rather than
prompted for, and the end-of-reload failure summary isn't shown as a dialog — both are
logged and raised as a single `NotificationManager` (`NotificationType::PlaterError`)
toast instead, one per batch rather than one per file, since repeat calls for the same
type update the existing notification in place rather than stacking. Unlike the
dialogs it replaces, a notification doesn't pump the event loop or wait for an answer,
so a retry that fails again just refreshes the same toast instead of reopening it. The
function returns whether anything was skipped, cancelled or failed to load, which is
what drives the commit/retry decision above.

`reload_from_disk()` also takes an `obj_color_fun` callback for a colour-import dialog
on `.obj` sources, but its `.obj` branch is dead code in the current tree — upstream
moved OBJ colour import to the texture importer, so the callback's `.obj` check never
matches and no such dialog appears for a reload, interactively or not.

A second hazard is re-entrancy: even with the three dialogs gone, `wxBusyInfo` and
`Model::read_from_file()` itself can pump the event loop, letting the debounce timer
fire again while an earlier reload is still on the stack. `SourceFileWatcher` guards
against this with an in-flight flag around the callback; a timer firing while it's set
re-arms itself instead of re-entering the callback.

## Painted features do not survive a reload

A reload replaces a volume's mesh outright, the same way the manual "Reload from disk"
menu item does. Painted supports, seam, color (MMU segmentation) and fuzzy skin are
triangle-indexed, so they only remap onto the new mesh when the `keep_painting` option
is on, and that option's own tooltip calls it experimental; with it off, painting is
simply lost. Before this feature, `reload_from_disk()` never warned about this at all —
manually or automatically, painted data was just silently gone. What the watcher adds
isn't only "now it can happen with nobody watching": the confirmation dialog below is
new too, and it now also covers a manually-triggered reload, which never asked before.

A source file changing on disk is the user's own doing — they re-exported it from CAD
— so `reload_from_disk()` treats it the same as the user clicking "Reload from disk"
themselves: honor it, without second-guessing whether they meant it. Paint is the one
exception, not because it's more "real" than other kinds of local work, but because
it's the one kind whose presence `reload_from_disk()` can always know for certain
(`ModelVolume::is_any_painted()`, derived straight from the facet data) — so a
confirmation here is genuinely informing the user of a consequence, not guessing at
their intent. If any volume a reload is about to touch is painted, one confirmation
dialog lists them and asks to continue, gated by `auto_reload_confirm_paint_loss` — **on
by default**, since losing paint with no warning is exactly the kind of surprise that
would make someone stop trusting the whole feature. The check that triggers the dialog
(`is_any_painted()`) runs before the `keep_painting` remap decision further down, so the
dialog can't tell whether that experimental option would actually end up preserving the
paint on the new mesh; its wording says the reload *might* discard the paint, not that it
will. The dialog and Preferences text deliberately don't name `keep_painting` as the
reason, though — an in-UI explanation would go stale the moment that option stops being
experimental, gets renamed, or is removed; the mechanism is documented here instead. This
one preference covers both a
manual reload (the "Reload from disk"/"Reload all from disk" menu items) and the
watcher: the risk of losing paint is identical regardless of what triggered the reload,
so it isn't scoped to auto-reload specifically, even though it's grouped in Preferences
next to the auto-reload options for now. The dialog itself also carries a "Reload
without warning" checkbox that turns the preference off right there, for whichever kind
of reload it was raised from and regardless of which button is then pressed — the
alternative to hunting down the Preferences checkbox after being surprised once.
- Declining a manual "Reload from disk"/"Reload all from disk" cancels the whole reload,
  the same as cancelling the missing-file picker does.
- Declining from the watcher only skips the painted volumes, not the whole batch — an
  auto-reload can cover several unrelated objects, and answering "no" to protect one
  object's paint shouldn't also hold back a change to another object that happened to
  arrive in the same debounce window. A skipped volume is retried the next time its file
  actually changes, via the same failed-stamp backoff as a missing source file.
- With the preference off, both a manual reload and the watcher proceed silently, same
  as any other file.

No other kind of local edit is tracked or asked about — a Cut, Simplify, Fix/Repair or
Smooth result is silently discarded by a reload exactly like an unpainted import would
be, with no way to opt back in per-volume. That was a deliberate choice, not an
oversight: any protection for those would only ever last for the current session (there
is nowhere in the `.3mf` format to record "this mesh no longer matches its source" —
implementing that would be a project-file format change, not a small addition), so
after closing and reopening the project, the protection would silently stop applying to
the exact same edit that was protected a moment earlier. That inconsistency — sometimes
protected, sometimes not, with no way to tell which without checking whether the
project was ever saved — was judged worse than no protection at all. **If you use Cut,
Simplify, Fix/Repair or Smooth on an object and care about the result surviving, treat
it as incompatible with `auto_reload_on_source_change`**: either don't enable
auto-reload for that project, or re-apply the tool after every reload.

Text/SVG embossing and SLA hollowing/support points are a different problem again: they
aren't removed by a reload (embossing is a separate `ModelVolume` that doesn't match the
reloaded file; hollowing and support points live on the `ModelObject`, which the REWORK
reload path never replaces, only the volumes inside it) — but nothing recomputes their
position either, so a reload that changes the host mesh's shape can leave them spatially
wrong instead of outright gone. Not covered by anything here.

## Auto-slice targets the affected plate, not the current one

With `auto_slice_after_reload` on, a successful reload queues a slice for whichever
plate(s) actually contain a reloaded object — found via
`PartPlateList::find_instance()` over each touched object's instances, not the plate
that happens to be on-screen (a reload can affect an off-screen plate) and not every
plate in the project (auto-arrange can spread one object's instances across plates,
but most reloads touch just one). Only a successful reload queues a slice: a failed one
is retried and comes back through the same path once it succeeds. If a slice is already
running, it's cancelled and the queue starts once cancellation completes; slicing directly
would have `MainFrame::get_enable_slice_status()` see a slice as still in progress and
silently skip the request. A running "Slice all" is the exception: cancelling it would abort
the whole multi-plate job, so the auto-slice is skipped instead. Plates queued by a later
reload are merged into the queue, and a plate that can't be sliced is skipped so it can't
stall the ones behind it. Each queued plate is selected, its slice result invalidated
directly (`reload_from_disk()`'s own `update()` only *schedules* that invalidation
via a debounce timer, which races a slice-enable check run right after it), and
sliced; `on_process_completed()` steps to the next queued plate once each one
finishes. Once the queue drains, the view is simply left on whichever plate was
sliced last, matching how "Slice all" already behaves. It deliberately does not
try to restore whatever plate was showing before the sequence started: a plate
switch immediately after a slice completes races that plate's own in-flight
preview refresh, and can leave stale toolpaths rendered over the wrong plate.

Each slice jumps to Preview once it starts, the same as a manually clicked "Slice" —
opting into both `auto_reload_on_source_change` and `auto_slice_after_reload` makes
the export itself the deliberate request to see a sliced result, no less than clicking
"Slice" would be.

## Implementation and verification

- [SourceFileWatcher.{hpp,cpp}](../../src/slic3r/GUI/SourceFileWatcher.hpp) — the
  watcher itself: path resolution, OS-level watches, debounce, stamp comparison,
  commit/retry.
- [Plater.cpp](../../src/slic3r/GUI/Plater.cpp) — `update_source_file_watches()`,
  `on_source_files_changed()`, `reload_source_files()`, `reload_from_disk()`'s
  `interactive` parameter, `maybe_auto_slice_after_reload()`, `slice_after_reload()`.
- [MainFrame.cpp](../../src/slic3r/GUI/MainFrame.cpp) — `slice_current_plate()`, shared
  by Cmd/Ctrl+R and the watcher's auto-slice.
- [Preferences.cpp](../../src/slic3r/GUI/Preferences.cpp) — the three checkboxes.
- [tests/slic3rutils/test_source_file_watcher.cpp](../../tests/slic3rutils/test_source_file_watcher.cpp) —
  the watcher's stamp comparison, commit/retry, baseline handling and re-entrancy guard, driven
  by delivering the debounce timer's event directly. The option defaults are in
  `tests/libslic3r/test_appconfig.cpp`.
- [scripts/test_auto_reload.py](../../scripts/test_auto_reload.py) — manual
  verification driving a running OrcaSlicer against real on-disk file changes; the
  GUI steps that can't be scripted (importing a model, toggling the preferences) are
  prompted for.
