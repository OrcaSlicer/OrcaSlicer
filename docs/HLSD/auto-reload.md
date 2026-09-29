# Auto-reload on source change — High Level Design

## Purpose and scope

Keeps OrcaSlicer in sync with an external CAD tool: when a model's source file is
re-exported, the affected objects are reloaded from disk automatically, and
optionally resliced, without the user switching back to OrcaSlicer. Both behaviors
are opt-in (`auto_reload_on_source_change`, `auto_slice_after_reload`, both off by
default).

An optional warning dialog prevents unintended overwrites when there are painted-on
features (support, fuzzy skin, seam). By default the warning is on, and affects both
manual and automatic reloads. See "Painted features do not survive a reload" below.

The feature has two parts: `SourceFileWatcher` (`src/slic3r/GUI/SourceFileWatcher.{hpp,cpp}`),
which only knows how to detect that a tracked file's content changed, and
`Plater::priv`'s integration of it, which knows what a "reload" and a "slice" are.
Neither half depends on the other's internals; the watcher's callback contract is
"here are the files that changed" — nothing more, see "Detecting and committing a
change" below for why there's nothing to report back.

A change is reported once the file has gone quiet, so both common ways of writing an
export are covered: a temp file written elsewhere and `rename()`'d into place (atomic
from the reader's point of view), and an in-place overwrite written in several chunks
over real time (OpenSCAD, Blender and most scripts write their exports this way). The
wait is bounded: a file that keeps being written for 30 seconds is reported as it
stands, so a runaway writer can't hold back reloads indefinitely. See "Waiting for a
write to finish" below.

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

## Waiting for a write to finish

Every filesystem event wakes a 300ms debounce timer (`on_fs_event()`), but what it does
to a timer that is already pending depends on which file the event names:

- **An event naming a tracked file** (its path, or a rename's new path; creates,
  deletes, renames and modifications only, not reads) restarts the timer. An in-place
  writer produces an event per write, so the check only runs once the file has gone
  300ms without one.
- **Any other event** starts the timer if it isn't already running, but never extends
  it. On Windows there is no per-file watch, so *every* event in a watched directory
  reaches `on_fs_event()` through the single directory watch. Restarting on those too
  would starve the check indefinitely under sustained unrelated activity in the same
  directory (a build process, a sync client). A headless test's directory-noise phase
  catches exactly this, deterministically on Windows. Such events still start the timer
  because some backends report a rename-into-place with just the directory (macOS's
  kqueue) or with no path at all (a Windows buffer-overflow warning).

Paths are compared with `wxFileName::SameAs()`, so case and separator differences on
Windows don't matter.

Windows needs one more signal. `ReadDirectoryChangesW` only reports a size or mtime
change once the data reaches the disk, and NTFS updates a directory entry lazily while
the file is open, so a long write can go quiet in the events (and in the stamp) while
still in progress. When the timer fires, each changed file is therefore opened with a
share mode that excludes writers; if that fails with a sharing violation, another
process still holds it open for writing, so that file is held back and the timer
re-armed while the rest of the batch goes ahead. POSIX has no equivalent, and needs
none: inotify and kqueue report each write as it happens.

Both ways of waiting share one backstop. The first event of a burst records when the
activity began (`m_settle_start`); after 30 seconds, events for tracked files stop
restarting the timer and the Windows open-for-writing check is skipped, so the pending
tick reports the file as it stands. The next event starts a fresh 30-second window, so a
file written continuously is reloaded at most every 30 seconds.

A writer that pauses for more than 300ms mid-file without holding the file open (closing
and reopening it between chunks) is still read while incomplete. The file's next write
is a new change, so the finished file is reloaded once it settles.

## Detecting and committing a change

When the debounce timer actually fires, `changed_source_files()` compares every
tracked file's current `(mtime, size)` stamp against the baseline recorded in
`set_watched_files()`/the last commit, and returns the ones that differ (a vanished
file — caught mid-rename between the old name's removal and the new one's arrival —
is skipped rather than reported; it's picked up once it reappears changed). Whatever
that set is, `on_timer()` advances the baseline for every one of those files
*immediately*, before calling `m_on_changed()` — not after, and not conditionally.
The mtime is kept at the filesystem's sub-second resolution: a binary STL's size
depends only on its triangle count, so a vertex-only edit re-exported within the same
second would be invisible to a whole-second mtime.

Since a file is only checked once it has settled, there's nothing worth retrying. Whatever
the callback's reload does with a file — succeeds, fails because the source is
genuinely corrupt, or the user declines a confirmation dialog it raises — that
attempt is final for this stamp. A file that keeps failing simply keeps not being
reported again until it changes to a new stamp, with no special-casing needed for
that: the baseline having already advanced to the failed stamp is what makes
`changed_source_files()` stop seeing it as different. There is no backoff, no retry
count, and no partial-batch bookkeeping — the callback (`m_on_changed`) takes the
changed set and returns nothing, because there's nothing for the watcher to do with a
result even if it had one.

## What gets reloaded

The watcher's callback receives the exact set of files that changed.
`Plater::priv::reload_source_files()` selects only the `ModelVolume`s whose resolved
source is in that set, so with several objects loaded, editing one CAD file reimports
only that object, not everything else on the plate. This is narrower than
`reload_all_from_disk()` (still used by the "Reload all" menu item and the canvas
shortcut), which selects every object regardless of which one changed — appropriate
there because the user asked for it explicitly, not appropriate for something that
runs on every detected file change. One instance's `GLVolume` is enough to select a
volume for this: `reload_from_disk()` edits the shared `ModelObject`/`ModelVolume`
directly, so the change reaches every instance regardless of which one's `GLVolume`
triggered the selection. A cloned volume (the Clone tool deep-copies a `ModelVolume`,
source path included, into an independent object) matches the changed-files set on
its own and reloads independently, keeping its own transform.

`reload_source_files()`'s `touched_objects` output (used below, for auto-slice) is
best-effort: it collects every object whose volume matched a changed file, not only
ones a reload actually managed to update. There's no per-volume success signal left
to filter on — `reload_from_disk()` returns nothing — so a plate can end up queued
for auto-slice even if, say, a missing source left one of its volumes untouched. That
inaccuracy was accepted deliberately: it's the same simplification that removed
retries everywhere else in this design, and its cost is at most one redundant slice of
an unchanged plate, not a correctness problem for the geometry itself.

## The watcher's trigger is not a special case

There is exactly one reload path — `Plater::priv::reload_from_disk()` — and the
watcher calls it the same way the manual "Reload from disk" menu item does: same
selection mechanics, same dialogs (missing-source file picker, paint-loss confirm,
end-of-batch failure summary), same everything. Nothing here is a design just for
"nobody's watching" — the retry/backoff loop that would motivate a quieter, dialog-free
path for an automatic trigger doesn't exist in this design (see "Detecting and
committing a change" above), so there's no repeated-dialog problem to avoid either.
If a re-export happens to be missing, corrupt, or touches a painted volume, the user
sees exactly the dialog they'd see for doing "Reload from disk" themselves at that
moment — because, from `reload_from_disk()`'s point of view, that's exactly what
happened.

The one thing the watcher's trigger does need that a menu click doesn't: re-entrancy
protection. `Model::read_from_file()` and any dialog `reload_from_disk()` shows
(`wxBusyInfo`, the paint-loss confirm, a missing-file picker) pump the event loop,
which can let the debounce timer fire again while an earlier reload from this same
watcher is still on the stack. `SourceFileWatcher` guards against this with an
in-flight flag (`m_reload_in_progress`) around the call to `m_on_changed()`; a timer
firing while it's set just re-arms itself instead of re-entering the callback.

## Painted features do not survive a reload

A reload replaces a volume's mesh outright, the same way the manual "Reload from disk"
menu item does. Painted supports, seam, color (MMU segmentation) and fuzzy skin are
triangle-indexed, so they only remap onto the new mesh when the `keep_painting` option
is on, and that option's own tooltip calls it experimental; with it off, painting is
simply lost.

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
one preference covers both a manual reload (the "Reload from disk"/"Reload all from disk"
menu items) and the watcher: the risk of losing paint is identical regardless of what
triggered the reload, so it isn't scoped to auto-reload specifically, even though it's
grouped in Preferences next to the auto-reload options for now. Declining the dialog
only drops the painted volumes from that reload, not the whole selection — a single
reload (manual or the watcher's own) can cover several unrelated volumes that happen to
share a source file, most commonly clones of the same object, and protecting one
painted clone shouldn't also hold back an unrelated change to the others just because
they were reloaded in the same batch. The dialog itself also carries a "Reload without
warning" checkbox that turns the preference off right there, for whichever kind of
reload it was raised from and regardless of which button is then pressed — the
alternative to hunting down the Preferences checkbox after being surprised once.

`ORCA_TEST_HOOKS_DIR` (an env var, checked just before `ShowModal()`) lets a headless
verification script answer this dialog from a file instead of a click, so it can drive
the watcher unattended without a person present to click through it. It's inert unless
that env var is exported — nothing a real user's session sets.

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
reloaded file; hollowing and support points live on the `ModelObject`, which the reload
path never replaces, only the volumes inside it) — but nothing recomputes their
position either, so a reload that changes the host mesh's shape can leave them spatially
wrong instead of outright gone. Not covered by anything here.

## Auto-slice targets the affected plate, not the current one

With `auto_slice_after_reload` on, `on_source_files_changed()` queues a slice for
whichever plate(s) actually contain a touched object (see "What gets reloaded" above
for what "touched" means here) — found via `PartPlateList::find_instance()` over each
touched object's instances, not the plate that happens to be on-screen (a reload can
affect an off-screen plate) and not every plate in the project (auto-arrange can spread
one object's instances across plates, but most reloads touch just one). If a slice is
already running, it's cancelled and the queue starts once cancellation completes;
slicing directly would have `MainFrame::get_enable_slice_status()` see a slice as still
in progress and silently skip the request. A running "Slice all" is the exception:
cancelling it would abort the whole multi-plate job, so the auto-slice is skipped
instead. Plates queued by a later reload are merged into the queue, and a plate that
can't be sliced is skipped so it can't stall the ones behind it. Each queued plate is
selected, its slice result invalidated directly (`reload_from_disk()`'s own `update()`
only *schedules* that invalidation via a debounce timer, which races a slice-enable
check run right after it), and sliced via `MainFrame::slice_current_plate()` (shared
with the Cmd/Ctrl+R shortcut); `on_process_completed()` steps to the next queued plate
once each one finishes. Once the queue drains, the view is simply left on whichever
plate was sliced last, matching how "Slice all" already behaves. It deliberately does
not try to restore whatever plate was showing before the sequence started: a plate
switch immediately after a slice completes races that plate's own in-flight preview
refresh, and can leave stale toolpaths rendered over the wrong plate.

Each slice jumps to Preview once it starts, the same as a manually clicked "Slice" —
opting into both `auto_reload_on_source_change` and `auto_slice_after_reload` makes
the export itself the deliberate request to see a sliced result, no less than clicking
"Slice" would be.

## Implementation and verification

- [SourceFileWatcher.{hpp,cpp}](../../src/slic3r/GUI/SourceFileWatcher.hpp) — the
  watcher itself: path resolution, OS-level watches, debounce, stamp comparison and
  unconditional commit.
- [Plater.cpp](../../src/slic3r/GUI/Plater.cpp) — `update_source_file_watches()`,
  `on_source_files_changed()`, `reload_source_files()`, `reload_from_disk()`'s
  paint-loss confirm block, `maybe_auto_slice_after_reload()`, `slice_after_reload()`.
- [MainFrame.cpp](../../src/slic3r/GUI/MainFrame.cpp) — `slice_current_plate()`, shared
  by Cmd/Ctrl+R and the watcher's auto-slice.
- [Preferences.cpp](../../src/slic3r/GUI/Preferences.cpp) — the three checkboxes.
- [tests/slic3rutils/test_source_file_watcher.cpp](../../tests/slic3rutils/test_source_file_watcher.cpp) —
  the watcher's stamp comparison (including a same-size rewrite within one second),
  unconditional baseline advance, re-entrancy guard and, on Windows, the
  open-for-writing hold-back, driven by delivering the debounce timer's event directly.
  The event-driven restart and the 30-second backstop depend on real events and elapsed
  time, and are covered by `scripts/test_auto_reload_simple_headless.py` instead.
