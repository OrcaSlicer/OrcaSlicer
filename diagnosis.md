# Windows-Only Silent Crash: OTA Preset Updater Race Condition

## Current status: startup vendor sync consolidated; Windows verification pending

## Summary

The silent crash reported on Windows is caused by two background threads inside
`PresetUpdater` independently downloading and extracting the **same vendor's**
OTA profile update at the same time, with no mutual exclusion between them.
One thread's cleanup routine (`prune_tmps()`) tries to delete the update
archive while the other thread still has it open for reading. On Windows this
fails with a sharing violation, which throws `boost::filesystem::filesystem_error`
uncaught inside a raw `std::thread`, which triggers `std::terminate()` and a
fail-fast process exit (`0xC0000409`) with no OrcaSlicer error dialog.

On Linux/POSIX this cannot happen — deleting/overwriting an open file is
always permitted there — which is why the crash was never reproducible on the
investigator's Linux (or, initially, Windows) machine.

## Original crash report

- GitHub Actions run: `30246354542`
- Build artifact: `OrcaSlicer_Windows_PR14900_x64_portable`
- Source commit: `b87b38bd7c003a03f27145ce3aabcf6c2c2122fe`
- Symptom: app closes silently on startup, no error dialog, on one specific
  Windows machine. Not reproducible on the investigator's Windows or Linux
  machine.

### ProcDump capture

```text
Exception: 406D1388
Exception: E06D7363.?AVfilesystem_error@filesystem@boost@@
Unhandled: C0000409
```

### Symbolized call stack (Visual Studio + matching PDB)

```text
OrcaSlicer.dll!boost::filesystem::emit_error(...)
OrcaSlicer.dll!boost::filesystem::detail::remove(...)
[Inline] OrcaSlicer.dll!boost::filesystem::remove(const boost::filesystem::path &) Line 639
OrcaSlicer.dll!Slic3r::PresetUpdater::priv::prune_tmps() Line 377
[Inline] OrcaSlicer.dll!PresetUpdater::sync::__l2::<lambda>() Line 1360
[Inline] std::invoke(...)
OrcaSlicer.dll!std::thread::_Invoke<...>(void *)
ucrtbase.dll / kernel32.dll / ntdll.dll (thread startup trampoline)
```

This pinpoints the throw to `fs::remove()` inside `prune_tmps()`, called from
the lambda passed to `PresetUpdater::sync()`'s background `std::thread`.

## Root cause chain

### 1. Two independent triggers download/extract the same vendor's profile

**Trigger A — automatic startup sync.**
`GUI_App.cpp:940`, inside the post-init `CallAfter`, runs on every launch
(unless stealth mode is enabled):

```cpp
this->preset_updater->sync(http_url, language, network_ver, sys_preset ? preset_bundle : nullptr);
```

`PresetUpdater::sync()` (`PresetUpdater.cpp:1341`) spawns a background
`std::thread` whose lambda body has **no try/catch anywhere**:

```cpp
p->thread = std::thread([this, vendors, active_vendor, http_url, language, plugin_version]() {
    this->p->prune_tmps();                       // deletes leftover *.data files in cache_path
    ...
    this->p->sync_vendor_config(active_vendor);   // downloads + extracts active vendor's update
    ...
});
```

**Trigger B — printer preset restore.**
Restoring the last-used printer preset at startup goes through
`Tab::select_preset()` → `Tab::on_presets_changed()`
(`Tab.cpp:2108-2125`):

```cpp
void Tab::on_presets_changed()
{
    ...
    if (m_type == Preset::TYPE_PRINTER) {
        const Preset& printer_preset = m_preset_bundle->printers.get_edited_preset();
        if (printer_preset.vendor) {
            wxGetApp().get_preset_updater()->check_vendor_update(printer_preset.vendor->id);
        }
    }
    ...
}
```

`PresetUpdater::check_vendor_update()` (`PresetUpdater.cpp:1395-1412`) spawns
its own thread that also calls `sync_vendor_config(vendor_id)` for the same
vendor:

```cpp
p->vendor_check_threads.emplace_back([this, vendor_id]() {
    try {
        this->p->sync_vendor_config(vendor_id);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[Orca Updater] vendor update failed for " << vendor_id << ": " << e.what();
    }
});
```

Both triggers fire on every normal startup. If the restored printer's vendor
is the same as the "active vendor" `sync()` checks (the common case — it's
whatever printer is currently selected), **both threads call
`sync_vendor_config()` for the same vendor concurrently.**

### 2. The de-duplication guard doesn't cover this window

`checked_vendors` (a `std::set` guarded by `vendor_check_mutex`) is meant to
prevent redundant checks for the same vendor. But the main `sync()` thread only
inserts into it **after** `sync_vendor_config(active_vendor)` returns
(`PresetUpdater.cpp:1379-1382`), not before it starts. So the entire
download+extract duration of the main thread's call is unguarded — a
concurrent `check_vendor_update()` call for the same vendor is not blocked.

### 3. Both threads touch the same file path

`sync_vendor_config()` (`PresetUpdater.cpp:663`) downloads to and extracts
from:

```cpp
fs::path download_file = cache_path / (vendor_id + TMP_EXTENSION);   // <data_dir>/ota/<vendor_id>.data
```

This path is identical regardless of which thread/caller invoked
`sync_vendor_config`, since it only depends on `vendor_id`.

### 4. The collision: `prune_tmps()` vs. an open archive reader

`prune_tmps()` (`PresetUpdater.cpp:375-382`, original code) runs at the very
start of the main thread's lambda and deletes **any** `*.data` file it finds
in `cache_path`, with no way to know whether another thread is actively using
one:

```cpp
for (auto &dir_entry : boost::filesystem::directory_iterator(cache_path))
    if (is_plain_file(dir_entry) && dir_entry.path().extension() == TMP_EXTENSION) {
        fs::remove(dir_entry.path());   // throwing overload, no try/catch
    }
```

Meanwhile, the other thread's `extract_file()` (`PresetUpdater.cpp:314-372`)
opens the same `.data` file via `open_zip_reader()`
(`miniz_extension.cpp:25`, `boost::nowide::fopen`). On Windows this maps to
`CreateFileW` with the CRT's default share mode, which does **not** include
`FILE_SHARE_DELETE`. While that file handle is open:

- `fs::remove()` on the same path fails with `ERROR_SHARING_VIOLATION`
  ("the process cannot access the file because it is being used by another
  process") → `boost::filesystem::remove()` throws `filesystem_error`.

This exception propagates out of the main thread's lambda — which has no
try/catch — so the C++ runtime calls `std::terminate()`, and Windows fail-fast
terminates the process with `0xC0000409`. This matches the captured dump
exactly.

On Linux/POSIX, `unlink()`/`rename()` on an open file always succeeds (the
existing file descriptor keeps working against the now-unlinked inode) — there
is no OS-level mechanism for this failure to occur at all. This is why the bug
is structurally Windows-only, not just something we got unlucky reproducing.

### 5. Secondary symptom: truncated/corrupted extraction

Even where the race doesn't hit `prune_tmps()`, the two threads can race on
the download step itself: one thread's download
(`fs::fstream file(download_file, std::ios::out | std::ios::trunc)`,
`PresetUpdater.cpp:732`) truncates and overwrites the very file the other
thread is still reading via `mz_zip_reader_extract_to_file`. This explains the
independently-reported symptom of profile extraction stopping partway through
(e.g. ~128 of 700+ files) — the reading thread hits corrupted/truncated
archive data mid-stream and `extract_file()` fails.

## Why it wasn't reproducible initially

The race requires the two threads' timing to overlap: the printer-preset
restore's `check_vendor_update()` call must still be mid-download/extract when
the main `sync()` thread's `prune_tmps()`/`sync_vendor_config()` runs. On a
fast machine (fast disk, no antivirus real-time-scan overhead, no network
throttling) the whole extraction can complete in well under a second, making
the overlap window narrow and unreliable to hit. On the affected user's
machine, something (slow disk, antivirus scanning each written file, network
latency on the initial HTTP GET, etc.) widened that window enough to hit it
consistently.

## Reproduction

The race was confirmed by artificially widening the window: a temporary
`std::this_thread::sleep_for(200ms)` was added after each extracted file in
`extract_file()` (`PresetUpdater.cpp`, retained temporarily as test-only
instrumentation, not a fix), stretching a 700+-file extraction to ~2.5
minutes. This reliably reproduced the collision on a real Windows machine and
was captured in `debug_Tue_Jul_28_16_02_24_29664.log.0`:

```text
[Thread 0x00002810] checking vendor update for Elegoo        (check_vendor_update thread starts)
[Thread 0x00002810] downloading update for Elegoo version 02.05.00.00
[Thread 0x00002810] extracting update for Elegoo
[Thread 0x00002810] successfully extract file 0..7 ...
[Thread 0x00007730] failed to remove ...\Elegoo.data: The process cannot access the file
                     because it is being used by another process     <-- prune_tmps() collision
[Thread 0x00007730] checking vendor update for Elegoo                (main sync() thread, same vendor)
[Thread 0x00007730] downloading update for Elegoo version 02.05.00.00  <-- truncates the open file
[Thread 0x00002810] extract file Elegoo/Elegoo Neptune 3 Max_cover.png ... failed
[Thread 0x00002810] extraction failed for Elegoo                      <-- reader thread's extraction dies
[Thread 0x00007730] extracting update for Elegoo                      (re-extracts from scratch)
... (678 files, ~2.5 minutes) ...
[Thread 0x00007730] vendor Elegoo update cached, notifying UI
```

With the crash-suppression fix (below) reverted and the same delay in place,
this reproduces the original crash: `boost::filesystem::filesystem_error`
uncaught in the main thread → fail-fast `0xC0000409`, matching the original
ProcDump signature.

## Historical crash-suppression fix

`PresetUpdater.cpp`, `prune_tmps()` — replaced the throwing
`directory_iterator`/`fs::remove()` calls with the non-throwing
`error_code`-overloads, logging failures instead of crashing:

```cpp
void PresetUpdater::priv::prune_tmps() const
{
    boost::system::error_code ec;
    auto dir_it = boost::filesystem::directory_iterator(cache_path, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(warning) << "[Orca Updater]failed to list cache dir " << cache_path.string() << ": " << ec.message();
        return;
    }
    for (auto &dir_entry : dir_it)
        if (is_plain_file(dir_entry) && dir_entry.path().extension() == TMP_EXTENSION) {
            BOOST_LOG_TRIVIAL(debug) << "[Orca Updater]remove old cached files: " << dir_entry.path().string();
            fs::remove(dir_entry.path(), ec);
            if (ec)
                BOOST_LOG_TRIVIAL(warning) << "[Orca Updater]failed to remove " << dir_entry.path().string() << ": " << ec.message();
        }
}
```

**This stops the fail-fast crash but does not fix the underlying race.** The
two threads can still corrupt each other's download/extraction of the same
vendor's profile (truncated files, failed extraction, wasted duplicate
network requests). This is confirmed by the log above: even without crashing,
one thread's extraction still fails outright and has to be silently redone by
the other.

## Historical same-vendor de-duplication fix

`check_vendor_update()` and the main `sync()` thread can no longer run
`sync_vendor_config()` for the same vendor concurrently. In the main thread's
lambda (`PresetUpdater.cpp:1374-1389`), the active vendor is now inserted into
`checked_vendors` (under `vendor_check_mutex`) **before** calling
`sync_vendor_config(active_vendor)`, not after — mirroring the guard
`check_vendor_update()` already applies to itself
(`PresetUpdater.cpp:1400-1403`). Whichever thread wins the insert races
proceeds with the download/extract; the other sees the vendor already claimed
and skips it entirely:

```cpp
if (!active_vendor.empty() && !vendors.empty()) {
    bool already_checked;
    {
        std::lock_guard<std::mutex> lock(this->p->vendor_check_mutex);
        already_checked = !this->p->checked_vendors.insert(active_vendor).second;
    }
    if (!already_checked)
        this->p->sync_vendor_config(active_vendor);
    if (p->cancel)
        return;
}
```

Additional defense-in-depth worth considering:

- Age-gate `prune_tmps()` so it only removes `*.data` files older than some
  threshold (e.g. last-write-time > a few minutes), so it can never touch a
  file that's plausibly still in active use regardless of any other guard.
- Wrap the entire main `sync()` thread lambda in a top-level
  `try { ... } catch (...)` as defense-in-depth against any other unforeseen
  throw site on that thread, matching the pattern `check_vendor_update()`
  already uses.

## Historical fix attempt #2: same-vendor de-duplication was insufficient

After applying the `checked_vendors`-before-`sync_vendor_config` fix above
(with the test-only extraction delay removed), **the crash still occurs.**
No fresh symbolized call stack/log has been captured yet for this run — that
is the next required step (see below) before proposing a third fix, per
systematic-debugging discipline (don't stack fixes without re-confirming the
throw site).

### Leading hypothesis: this is a different, still-unguarded throw site in the same code path

The two fixes so far only cover the specific mechanism confirmed in
`debug_Tue_Jul_28_16_02_24_29664.log.0`: `prune_tmps()`'s `fs::remove()` racing
the *same vendor's* concurrent extraction. They do **not** add any exception
handling around the main `sync()` thread's lambda itself
(`PresetUpdater.cpp:1367-1393`) — it still has zero try/catch, unlike
`check_vendor_update()`'s thread, which wraps its call in
`try { sync_vendor_config(...) } catch (const std::exception&)`.

Several other throwing (non-`error_code`) `boost::filesystem` calls remain
reachable from that same unguarded thread and were not touched by either fix:

- `fs::create_directories(cache_profile_path)` — `PresetUpdater.cpp:712`,
  in `sync_vendor_config()`, called unconditionally before the (already
  non-throwing) per-vendor cache cleanup.
- `fs::create_directories(dest_path)` — inside `extract_file()`'s
  directory-entry branch (~`PresetUpdater.cpp:339-340`). Note this call sits
  **outside** the local `try/catch` in `extract_file()` (that try/catch only
  wraps the `mz_zip_reader_extract_to_file` call for regular files,
  `PresetUpdater.cpp:347-363`), so an exception here is caught by nothing at
  any level.
- Other `sync_*` functions invoked from the same lambda
  (`sync_plugins`, `sync_printer_config`, `sync_resources`) have their own
  file/directory operations and were not audited for throwing calls in this
  pass.

Any of these throwing on a second app instance / second launch / different
vendor combination would reproduce the same failure class (uncaught
`filesystem_error` → `std::terminate` → fail-fast `0xC0000409`) with a
**different** call stack than the original `prune_tmps` one. This has not been
confirmed yet — it is the leading hypothesis, not an established fact.

### Superseded next steps from the earlier diagnosis

1. **Capture a fresh ProcDump + symbolized call stack** for this reproduction,
   the same way as the original (see "Original crash report" above). Compare
   the throw site against `prune_tmps()` — if it's a *different* function, that
   confirms the hypothesis above and tells us exactly which call to fix next.
2. Grep the fresh debug log (same format as
   `debug_Tue_Jul_28_16_02_24_29664.log.0`) for `"Orca Updater"` lines around
   the crash timestamp — specifically whether `"checking vendor update for"`
   appears more than once for *different* vendors close together (would
   indicate the race moved to a vendor pair the current fix doesn't cover, or
   to `prune_tmps()` colliding with a different vendor's in-flight extraction —
   which should now only log a warning, not crash, so check the log for that
   warning near the crash time to rule it out).
3. If the new stack confirms an unguarded throwing call elsewhere in the same
   thread, the most robust fix at that point is the "defense-in-depth" item
   already listed below — wrap the **entire** main `sync()` thread lambda body
   in a top-level `try/catch`, matching `check_vendor_update()`'s pattern —
   rather than continuing to patch individual call sites one at a time.

## Implemented fix

The current patch makes printer-preset restoration the sole startup trigger for
OTA vendor profile synchronization and moves stale-archive cleanup into that
selected-vendor path:

1. The general startup `PresetUpdater::sync()` path, called from
   `GUI_App.cpp`, no longer calls `sync_vendor_config()` and no longer tries to
   infer the active vendor from the preset bundle.
2. Startup printer-preset restoration calls `check_vendor_update()` after the
   actual selected printer/vendor is known. That is now the only startup path
   that launches `sync_vendor_config()`.
3. The general `sync()` path no longer calls the global `prune_tmps()` scan.
   The vendor-check worker calls targeted `prune_tmp(vendor_id)` immediately
   before `sync_vendor_config(vendor_id)`. It removes only
   `ota/<vendor>.data`, then the vendor sync truncates the same target before
   writing and removes it after extraction.

`checked_vendors` remains as a GUI-thread de-duplication set so repeated
printer-preset notifications do not launch another check for the same vendor.
`vendor_check_mutex` is no longer needed because the vendor check is claimed
from the GUI event path; the worker thread only performs the already-claimed
operation.

### Source-level changes

- `priv::prune_tmp(vendor_id)` replaces the global `prune_tmps()` scan and
  removes only the selected vendor's stale root-level archive immediately
  before its update.
- `PresetUpdater::sync()` handles version, plugin, and printer-config updates;
  it does not perform OTA vendor-profile synchronization.
- `PresetUpdater::check_vendor_update()` claims each vendor once in
  `checked_vendors` before creating its worker thread.
- Both raw updater thread entry points catch `std::exception` and unknown
  exceptions, preventing an unexpected updater failure from reaching
  `std::terminate()`.

The effective ordering is now:

```text
printer preset restored
  └─ check_vendor_update()
       └─ prune_tmp(vendor_id)
       └─ one sync_vendor_config() worker
```

The temporary 200 ms delay after each extracted file is test instrumentation
for validating the fix. It intentionally widens the original race window and
is not production behavior. In the current worktree, no active `sleep_for`
call is present; re-enable it in `extract_file()` before running the slowed
Windows reproduction, then remove it after successful verification.

## End-user log confirmation

`debug_Tue_Jul_28_17_30_29_1456.log.0` from the affected user's build
confirms the race mechanism described above. The relevant sequence is:

```text
17:30:35.989  Thread 0x00007fbc  checking vendor update for Elegoo
17:30:36.250  Thread 0x00007fbc  downloading update for Elegoo
17:30:36.365  Thread 0x00007fbc  extracting update for Elegoo
17:30:37.335  Thread 0x000071f8  failed to remove ...\\ota\\Elegoo.data:
                                 The process cannot access the file because it is being used
17:30:37.335  Thread 0x000071f8  checking vendor update for Elegoo
17:30:38.221  Thread 0x000071f8  downloading update for Elegoo
17:30:38.293  Thread 0x000071f8  extracting update for Elegoo
17:30:38.602  Thread 0x00007fbc  vendor Elegoo update cached
17:30:39.721  Thread 0x000071f8  vendor Elegoo update cached
```

This is direct evidence that two threads processed the same vendor and shared
the same `.data` path while the first extraction still had the archive open.
The thread `0x00007fbc` is the first vendor-check worker; `0x000071f8` is
consistent with the general startup updater thread because it continues into
the general plugin/printer-resource sync afterward. The thread-role mapping is
an inference from the surrounding log sequence, but the duplicate vendor
operation and Windows sharing violation are explicit.

The user did not crash because this artifact already logged the failed removal
instead of throwing it out of the raw thread. That suppresses the original
`filesystem_error` → `std::terminate()` failure, but it does not remove the
underlying race: both threads still downloaded and extracted the same archive.
Both extractions happened to complete in this run, so the log demonstrates the
race even without demonstrating truncated extraction.

The repeated `Unzip: invalid size for file Elegoo.changelog` warning appears in
both extraction attempts, but each update is still reported as cached. It is a
separate archive-content warning, not the cause of the Windows sharing
violation.

## Functional and side-effect audit

- `PresetUpdater::sync()` still performs version, plugin, and printer-resource
  synchronization. Its public call site in `GUI_App.cpp` is unchanged; only
  vendor-profile synchronization was removed from that worker.
- The only current `check_vendor_update()` call site is
  `Tab::on_presets_changed()`, a GUI event path. Removing `vendor_check_mutex`
  therefore preserves current behavior, but a future non-GUI caller must not
  be added without restoring synchronization around `checked_vendors`.
- `sync_resources()` downloads to unique files under the system temp directory
  and then extracts into its configured cache. It does not consume the root
  `ota/<vendor>.data` files used by `sync_vendor_config()`.
- `ota/plugins`, `ota/printers`, and `ota/profiles` remain unchanged. The
  targeted cleanup only removes the selected vendor's root archive before its
  own update.
- Root archives for vendors that are never selected are no longer globally
  swept. They can remain as harmless cache clutter; the next update for that
  vendor removes/truncates its own archive.
- Cross-process synchronization is not provided: a second OrcaSlicer instance
  using the same data directory could still hold the selected archive open.
  The targeted removal logs the failure and the worker catches exceptions, so
  this remains a recoverable update failure rather than the original silent
  termination.

## Verification pending

No build, automated test, Windows reproduction, or new ProcDump capture has
been run after this patch. Re-enable the slowed startup delay, then confirm
that extraction completes without sharing violations,
truncated archives, or duplicate vendor downloads. The startup log should show
one `checking vendor update for <vendor>` entry from the restored printer
preset path.

## Key files/functions for follow-up

| File | Function | Role |
|---|---|---|
| `src/slic3r/GUI/GUI_App.cpp:940` | startup `CallAfter` | Triggers `PresetUpdater::sync()` on every launch |
| `src/slic3r/GUI/Tab.cpp:2108-2125` | `Tab::on_presets_changed()` | Triggers `check_vendor_update()` on printer preset restore/change |
| `src/slic3r/Utils/PresetUpdater.cpp:1372-1399` | `PresetUpdater::sync()` | Main background sync; no vendor-profile sync |
| `src/slic3r/Utils/PresetUpdater.cpp:1401-1418` | `PresetUpdater::check_vendor_update()` | Sole startup vendor-profile sync trigger |
| `src/slic3r/Utils/PresetUpdater.cpp:374-383` | `PresetUpdater::priv::prune_tmp()` | Targeted cleanup of `cache_path/<vendor_id>.data` before sync |
| `src/slic3r/Utils/PresetUpdater.cpp:651-744` | `PresetUpdater::priv::sync_vendor_config()` | Sole owner of `cache_path/<vendor_id>.data` download/extraction |
| `src/libslic3r/miniz_extension.cpp:25` | `open_zip_reader()` | Opens the archive via `boost::nowide::fopen`; source of the Windows-only file-locking behavior |
