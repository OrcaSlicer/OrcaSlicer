# Auto-reload on source change — High Level Design

## Purpose and scope

Keeps OrcaSlicer in sync with an external CAD tool: when a model's source file is
re-exported, the affected objects are reloaded from disk automatically, and
optionally resliced, without the user switching back to OrcaSlicer. Both behaviors
are opt-in (`auto_reload_on_source_change`, `auto_slice_after_reload`, both off by
default) so nothing changes for anyone who hasn't turned them on.

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

A file whose current stamp reads as missing (deleted, unmounted, or caught mid-rename
between the old name's removal and the new one's arrival) is not treated as a change:
treating a delete as if it were an edit would fire a reload against a path that isn't
there. Waiting for the file to come back is also just the correct behavior for a
rename-into-place, which necessarily vanishes the old name for an instant.

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

## Reloading without anyone at the keyboard

`reload_from_disk()` is written for the manual "Reload from disk" menu item and can
show three dialogs: a file picker for a missing source, a "replace it?" confirmation,
and a colour-import dialog for `.obj` sources. All three assume someone is present to
answer them. `reload_from_disk()`/`reload_all_from_disk()` take an `interactive`
parameter (default `true`, so the menu item and canvas shortcut are unaffected); the
watcher always calls with `false`. Non-interactively: a missing source is logged and
its volume left untouched rather than prompted for, the `.obj` colour dialog is
skipped in favor of its own cancel behavior (colours left as they are), and the
end-of-reload failure summary is logged instead of shown in a dialog. The function
returns whether anything was skipped, cancelled or failed to load, which is what
drives the commit/retry decision above.

A second hazard is re-entrancy: even with the three dialogs gone, `wxBusyInfo` and
`Model::read_from_file()` itself can pump the event loop, letting the debounce timer
fire again while an earlier reload is still on the stack. `SourceFileWatcher` guards
against this with an in-flight flag around the callback; a timer firing while it's set
re-arms itself instead of re-entering the callback.

## Auto-slice targets the affected plate, not the current one

With `auto_slice_after_reload` on, a successful reload queues a slice for whichever
plate(s) actually contain a reloaded object — found via
`PartPlateList::find_instance()` over each touched object's instances, not the plate
that happens to be on-screen (a reload can affect an off-screen plate) and not every
plate in the project (auto-arrange can spread one object's instances across plates,
but most reloads touch just one). If a slice is already running, it's cancelled and
the queue starts once cancellation completes; slicing directly would have
`MainFrame::get_enable_slice_status()` see a slice as still in progress and silently
skip the request. Each queued plate is selected, its slice result invalidated
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
- [Preferences.cpp](../../src/slic3r/GUI/Preferences.cpp) — the two checkboxes.
- [scripts/test_auto_reload.py](../../scripts/test_auto_reload.py) — manual
  verification driving a running OrcaSlicer against real on-disk file changes; the
  GUI steps that can't be scripted (importing a model, toggling the preferences) are
  prompted for.
