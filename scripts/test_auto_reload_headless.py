#!/usr/bin/env python3
"""Headless, fully automated variant of test_auto_reload.py.

Drives the same phases as the interactive script, but with no human involved: reads and writes
OrcaSlicer.conf directly instead of asking someone to click Preferences checkboxes, launches and
restarts the OrcaSlicer binary itself instead of asking someone to open a project, and checks the
debug log for evidence of what would otherwise have been eyeballed on screen (an object's new
size, which tab or plate is active, whether a notification call fired) instead of asking yes/no
questions.

Uses the checked-in template project at scripts/testdata/autoreload_test_template.3mf by default.
Pass --template to use a different one instead -- e.g. one freshly hand-authored via
test_auto_reload.py's one-time setup (import the six generated STLs onto three plates, paint one,
save), if you're testing a change to the template's own layout or paint. This script only reuses
a template -- it doesn't author one.

Preferences can't be changed in a running instance (AppConfig loads OrcaSlicer.conf once at
startup and never re-reads it), so this script restarts the app whenever the phases ahead need a
different (reload, confirm, slice) combination -- four restarts for a full run, grouped the same
way test_auto_reload.py's PHASES table is.

Two things a script can't do without a human are handled differently here rather than skipped
silently:
  - The paint-loss confirmation dialog (phases H/N) is answered through a file-based test hook
    (env var ORCA_TEST_HOOKS_DIR points at a directory; a "paint_loss_answer" file inside it holds
    "yes" or "no") instead of a real click. This hook only exists in the C++ for this script's
    benefit and is inert unless ORCA_TEST_HOOKS_DIR is exported -- a real user's Preferences never
    set it. A file, not the env var itself, carries the answer because H and N need different
    answers without restarting the process an env var would require.
  - Phase L's original setup (manually pre-slicing plate 1 so it could later be checked as
    "untouched") has no scriptable equivalent -- no CLI hook slices a single plate mid-session --
    so this version asserts the same invariant a different way: the plate id embedded in the
    slice-start/slice-done log lines matches the plate whose object changed, and no other plate
    starts a slice in the same window. That is "only the changed plate reslices", just not
    reproduced via the original manual repro.

Everything else that has no log-observable proxy is dropped rather than faked -- e.g. phase F's
"solid, with no voids" needs real mesh inspection a bounding-box size can't provide, and "the paint
is gone" needs ModelVolume::is_any_painted(), which nothing reload-related logs. Each such drop is
called out in a comment at its original phase.

    python3 scripts/test_auto_reload_headless.py [options]

Use --only to run just one or more phases, e.g. --only L. Preferences are still set correctly for
whichever phases are selected, and only the state groups those phases need are restarted into.
"""

import argparse
import glob
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import threading
import time
import zipfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
DEFAULT_TEMPLATE = os.path.join(SCRIPT_DIR, "testdata", "autoreload_test_template.3mf")

RELOAD_MARK          = "source file(s) changed on disk, reloading"
SLICE_START_MARK     = "will start print::process"
SLICE_DONE_MARK      = "on_process_completed:finished"
MISSING_SOURCE_MARK  = "source file missing, skipping reload"
LOAD_FAILED_MARK     = "failed to load"
PAINT_DECLINED_MARK  = "skipping reload, declined in the paint-loss prompt"
NOTIFICATION_MARK    = "pushed a PlaterError notification"
WATCHING_RE          = re.compile(r"watching (\d+) source file\(s\) for changes")
# "reload_from_disk: reloaded /path/to/file.stl, bounding box size = 12 x 12 x 12 mm"
SIZE_RE              = re.compile(r"reloaded (\S.*?), bounding box size = ([\d.eE+-]+) x ([\d.eE+-]+) x ([\d.eE+-]+) mm")
SLICE_START_PLATE_RE = re.compile(r"will start print::process, plate (\d+)")
PLATE_SWITCH_RE      = re.compile(r"current plate switched to (\d+)")
RELOADABLE_COUNT_RE  = re.compile(r"reloadable volumes number is: (\d+)")

PREF_RELOAD  = "auto_reload_on_source_change"
PREF_CONFIRM = "auto_reload_confirm_paint_loss"
PREF_SLICE   = "auto_slice_after_reload"


def default_data_dir():
    system = platform.system()
    if system == "Darwin":
        return os.path.expanduser("~/Library/Application Support/OrcaSlicer")
    if system == "Windows":
        return os.path.join(os.environ.get("APPDATA", ""), "OrcaSlicer")
    for candidate in ("~/.config/OrcaSlicer",
                      "~/.var/app/io.github.softfever.OrcaSlicer/config/OrcaSlicer"):
        if os.path.isdir(os.path.expanduser(candidate)):
            return os.path.expanduser(candidate)
    return os.path.expanduser("~/.config/OrcaSlicer")


EXPECTED_TEMPLATE_SOURCES = ("cube.stl", "second_a.stl", "second_b.stl", "painted.stl", "flaky.stl", "quick.stl")
_SOURCE_FILE_RE = re.compile(r'key="source_file"\s+value="([^"]+)"')


def check_template_sources(template_path):
    """Fails loudly, before any phase runs, if the template doesn't actually reference all six
    STLs this script writes to -- a template re-authored by hand (the one-time interactive setup)
    can easily end up missing one, which otherwise shows up much later as a confusing "no reload
    line" failure in whichever phase touches the missing one."""
    with zipfile.ZipFile(template_path) as zf:
        config = zf.read("Metadata/model_settings.config").decode("utf-8", errors="replace")
    found = {os.path.basename(v) for v in _SOURCE_FILE_RE.findall(config)}
    missing = [name for name in EXPECTED_TEMPLATE_SOURCES if name not in found]
    if missing:
        sys.exit("%s doesn't reference %s as a source file -- redo the one-time interactive setup "
                  "(test_auto_reload.py's docstring) so every object is imported from its matching "
                  "STL." % (template_path, ", ".join(missing)))


def binary_path_suggestions():
    """Example --binary values for this checkout's build layout, one per config, to put in
    --help -- not a default, since which config the caller actually built isn't this script's
    business to guess."""
    configs = ("Debug", "Release", "RelWithDebInfo")
    system = platform.system()
    if system == "Darwin":
        return [os.path.join(REPO_ROOT, "build/arm64-ninja/src", config, "OrcaSlicer.app/Contents/MacOS/OrcaSlicer")
                for config in configs]
    if system == "Windows":
        return [os.path.join(REPO_ROOT, "build", "src", config, "orca-slicer.exe") for config in configs]
    if system == "Linux":
        # build_linux.sh picks a config-specific build dir (build / build-dbg / build-dbginfo) and
        # configures with the Ninja Multi-Config generator, so each still has its own <Config>
        # subfolder underneath.
        build_dirs = {"Release": "build", "Debug": "build-dbg", "RelWithDebInfo": "build-dbginfo"}
        return [os.path.join(REPO_ROOT, build_dirs[config], "src", config, "orca-slicer") for config in configs]
    return []


_MONTHS = {"Jan": 1, "Feb": 2, "Mar": 3, "Apr": 4, "May": 5, "Jun": 6,
           "Jul": 7, "Aug": 8, "Sep": 9, "Oct": 10, "Nov": 11, "Dec": 12}
_LOG_NAME_RE = re.compile(r"debug_\w{3}_(\w{3})_(\d{2})_(\d{2})_(\d{2})_(\d{2})_(\d+)\.log")


def _log_launch_key(path):
    """Sorts by the launch timestamp embedded in OrcaSlicer's own log filename, not the file's
    mtime -- see test_auto_reload.py's identically-named helper for why that matters."""
    m = _LOG_NAME_RE.match(os.path.basename(path))
    if not m or m.group(1) not in _MONTHS:
        return (0, os.path.getmtime(path))
    month, day, hh, mm, ss, _pid = m.groups()
    return (1, _MONTHS[month], int(day), int(hh), int(mm), int(ss))


def read_full_config(data_dir):
    path = os.path.join(data_dir, "OrcaSlicer.conf")
    with open(path, encoding="utf-8") as f:
        text = f.read()
    # OrcaSlicer appends a trailing "# MD5 checksum ..." line after the closing brace -- not valid
    # JSON, so parse just the leading object and ignore whatever follows it (matches
    # test_auto_reload.py's read_prefs()).
    config, _ = json.JSONDecoder().raw_decode(text)
    return config, path


def write_prefs(data_dir, want_reload, want_confirm, want_slice):
    """Only safe to call while the app isn't running: AppConfig loads this file once at startup
    and never re-reads it, so a running instance would just overwrite these edits with whatever
    it already has in memory the next time it saves for an unrelated reason.

    Rewriting the file drops the trailing MD5 checksum comment AppConfig itself appends. Losing
    the checksum itself is harmless -- verification is compiled in only under Windows, and even
    there it's advisory (logs a warning, doesn't reject the file). But on Windows, AppConfig::load()
    always does total_string.substr(last_pos+2) right after the last '}' to skip that line's
    leading "\n", regardless of whether a checksum line actually follows -- so the file must still
    end with a newline after the closing brace, or that substr reads past the end of the string and
    throws std::out_of_range, which crashes the app at startup. See AppConfig::load()."""
    config, path = read_full_config(data_dir)
    app = config.setdefault("app", {})
    app[PREF_RELOAD] = want_reload
    app[PREF_CONFIRM] = want_confirm
    app[PREF_SLICE] = want_slice
    # The template's presets are whatever the machine that last authored it had installed, so
    # loading it on a different machine trips the "Customized Preset"/"Modified G-code" modal
    # (Plater.cpp, guarded by this exact key -- it's what that dialog's own "don't show again"
    # checkbox writes) with nobody there to click it. Suppressing it doesn't change what actually
    # gets sliced: the dialog only warns about the *preset name* not being found system-side, the
    # project's own embedded config values are used either way (PresetBundle::validate_presets()).
    app["no_warn_when_modified_gcodes"] = True
    with open(path, "w", encoding="utf-8") as f:
        json.dump(config, f, indent="\t")
        f.write("\n")


class LogTail:
    """Follows a log file from the moment it's opened; each test starts from a fresh mark."""

    def __init__(self, path):
        self.path = path
        self.pos = os.path.getsize(path)
        self.buf = ""

    def mark(self):
        # A line the app already emitted can still be mid-flight to the log file's OS buffers at
        # the exact instant this reads -- a single _read() can race the very last writes of
        # whatever just happened and miss them, only to have them show up in a *later* caller's
        # window instead, mistaken for new content there. Drain briefly before clearing.
        self._read()
        for _ in range(3):
            time.sleep(0.05)
            self._read()
        self.buf = ""

    def _read(self):
        with open(self.path, "r", encoding="utf-8", errors="replace") as f:
            f.seek(self.pos)
            chunk = f.read()
            self.pos = f.tell()
        self.buf += chunk

    def wait_for(self, marker, timeout):
        deadline = time.monotonic() + timeout
        while True:
            self._read()
            if marker in self.buf:
                return True
            if time.monotonic() > deadline:
                return False
            time.sleep(0.25)

    def wait_for_match(self, pattern, timeout):
        deadline = time.monotonic() + timeout
        while True:
            self._read()
            m = pattern.search(self.buf)
            if m:
                return m
            if time.monotonic() > deadline:
                return None
            time.sleep(0.25)

    def wait_for_size(self, filename, timeout):
        """Waits for a 'reloaded <path>, bounding box size = X x Y x Z mm' line whose path's
        basename matches `filename`'s, returning the most recent (x, y, z) floats seen within the
        window, or None."""
        deadline = time.monotonic() + timeout
        base = os.path.basename(filename)
        while time.monotonic() < deadline:
            self._read()
            for path, x, y, z in reversed(SIZE_RE.findall(self.buf)):
                if os.path.basename(path) == base:
                    return (float(x), float(y), float(z))
            time.sleep(0.25)
        return None

    def absent_after(self, marker, wait):
        time.sleep(wait)
        self._read()
        return marker not in self.buf

    def has_seen(self, marker):
        self._read()
        return marker in self.buf

    def last_match(self, pattern):
        self._read()
        matches = pattern.findall(self.buf)
        return matches[-1] if matches else None


class OrcaApp:
    """Launches/restarts the OrcaSlicer binary with a project preloaded via its CLI argument, and
    locates the fresh debug log it starts writing."""

    def __init__(self, binary_path, data_dir, log_dir, hooks_dir):
        self.binary_path = binary_path
        self.data_dir = data_dir
        self.log_dir = log_dir
        self.hooks_dir = hooks_dir
        self.proc = None

    def start(self, project_path, ready_timeout, expected_watch_count):
        os.makedirs(self.hooks_dir, exist_ok=True)
        self.set_paint_answer(None)
        env = os.environ.copy()
        env["ORCA_TEST_HOOKS_DIR"] = self.hooks_dir
        before = {p for p in glob.glob(os.path.join(self.log_dir, "debug_*.log*")) if not p.endswith(".enc")}
        args = [self.binary_path, project_path, "--datadir", self.data_dir]
        self.proc = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        deadline = time.monotonic() + ready_timeout
        log_path = None
        while time.monotonic() < deadline:
            candidates = {p for p in glob.glob(os.path.join(self.log_dir, "debug_*.log*"))
                          if not p.endswith(".enc")} - before
            if candidates:
                log_path = max(candidates, key=_log_launch_key)
                break
            if self.proc.poll() is not None:
                raise RuntimeError("OrcaSlicer exited during startup (exit code %s)" % self.proc.returncode)
            time.sleep(0.5)
        if log_path is None:
            raise RuntimeError("No new debug log appeared within %gs of launch" % ready_timeout)

        # update_source_file_watches() logs more than once while a project loads:
        # object_list_changed() fires once before the project folder is resolved (transiently
        # reporting a stale/incomplete count) and again after load_project() finishes with the
        # real one -- and how far apart those two lines land is a matter of local disk/GL-init
        # speed, not something a fixed quiet window can safely bound (seen anywhere from under a
        # second to a bit over two). So wait for the specific count this restart actually expects
        # (all six sources, or zero when auto-reload is off) instead of guessing when the log has
        # gone quiet -- that also makes a transient reading impossible to mistake for the real one,
        # since it's never equal to the true count by construction (the early call fails to resolve
        # every entry, precisely because the project folder isn't set yet).
        tail = LogTail(log_path)
        target_re = re.compile(r"watching %d source file\(s\) for changes" % expected_watch_count)
        if not tail.wait_for_match(target_re, ready_timeout):
            last_seen = tail.last_match(WATCHING_RE)
            raise RuntimeError(
                "App never reported watching %d source file(s) within %gs of launch (last seen: "
                "%s) -- probably blocked on a modal dialog the project load never got past (e.g. "
                "the \"Customized Preset\"/\"Modified G-code\" warning, if the template's presets "
                "aren't installed here). Check %s." % (expected_watch_count, ready_timeout, last_seen, log_path))
        return tail

    def set_paint_answer(self, answer):
        """answer: 'yes', 'no', or None to clear (dialog blocks if this hook were ever hit with
        no answer file present, same as a real user seeing the real dialog)."""
        path = os.path.join(self.hooks_dir, "paint_loss_answer")
        if answer is None:
            if os.path.exists(path):
                os.remove(path)
        else:
            with open(path, "w") as f:
                f.write(answer)

    def stop(self, timeout=8):
        if self.proc is None:
            return
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=timeout)
        self.proc = None


BOX_FACES = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
             (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]


def stl_data(boxes, voids=()):
    lines = ["solid test"]
    for x0, y0, z0, x1, y1, z1 in list(boxes) + list(voids):
        inward = (x0, y0, z0, x1, y1, z1) in voids
        v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
             (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
        for a, b, c in BOX_FACES:
            if inward:
                b, c = c, b
            lines.append("  facet normal 0 0 0\n    outer loop")
            for i in (a, b, c):
                lines.append("      vertex %g %g %g" % v[i])
            lines.append("    endloop\n  endfacet")
    lines.append("endsolid test\n")
    return "\n".join(lines)


def write_stl(path, boxes, atomic=False, voids=()):
    data = stl_data(boxes, voids)
    if atomic:
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            f.write(data)
        os.replace(tmp, path)
    else:
        with open(path, "w") as f:
            f.write(data)


def write_cube_stl(path, size, atomic=False):
    s = float(size)
    write_stl(path, [(0, 0, 0, s, s, s)], atomic)


def write_cube_stl_slow(path, size, num_chunks=6, chunk_delay=0.6):
    """Writes a cube STL like write_cube_stl(), but in num_chunks pieces with chunk_delay seconds
    between each, flushing and fsyncing after every piece -- see test_auto_reload.py's identically
    named helper for why (simulates a slow in-place export that keeps growing past the watcher's
    500ms stability window, for phase P's mid-write check)."""
    data = stl_data([(0, 0, 0, float(size), float(size), float(size))])
    step = max(1, len(data) // num_chunks)
    pieces = [data[i:i + step] for i in range(0, len(data), step)]
    with open(path, "w") as f:
        for i, piece in enumerate(pieces):
            f.write(piece)
            f.flush()
            os.fsync(f.fileno())
            if i < len(pieces) - 1:
                time.sleep(chunk_delay)


def write_buried_pillars_stl(path, height, n=32, pitch=1.5, width=1.0, with_voids=True):
    size = n * pitch + 2
    voids = [(1 + i * pitch, 1 + j * pitch, 1, 1 + i * pitch + width, 1 + j * pitch + width, height - 1.0)
             for i in range(n) for j in range(n)] if with_voids else []
    write_stl(path, [(0, 0, 0, size, size, float(height))], voids=voids)


def write_truncated_stl(path):
    with open(path, "w") as f:
        f.write("solid test\n  facet normal 0 0 0\n    outer loop\n      vertex 0 0 0\n")


def directory_noise(dir_path, stop_event, interval=0.2):
    i = 0
    while not stop_event.is_set():
        p = os.path.join(dir_path, "noise_%d.tmp" % i)
        with open(p, "w") as f:
            f.write("x")
        os.remove(p)
        i += 1
        stop_event.wait(interval)


def main():
    # RawText (not just RawDescription) so --binary's multi-line suggestion list isn't rewrapped.
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--data-dir", default=default_data_dir(), help="OrcaSlicer data directory")
    binary_help = "path to the OrcaSlicer executable (required -- which build config to test is your call, not this script's)"
    suggestions = binary_path_suggestions()
    if suggestions:
        binary_help += ". e.g.:\n" + "\n".join(suggestions)
    parser.add_argument("--binary", required=True, help=binary_help)
    parser.add_argument("--timeout", type=float, default=20.0, help="seconds to wait for a reload/slice (default 20)")
    parser.add_argument("--quiet-window", type=float, default=8.0,
                        help="seconds to wait when asserting that nothing happens (default 8)")
    parser.add_argument("--startup-timeout", type=float, default=45.0,
                        help="seconds to wait for the app to launch and start watching files (default 45)")
    parser.add_argument("--slow-height", type=float, default=60.0)
    parser.add_argument("--mid-slice-delay", type=float, default=3.0)
    parser.add_argument("--work-dir", default=os.path.expanduser("~/orca_autoreload_test"),
                        help="scratch dir for the generated STLs and the live project copy "
                             "(default ~/orca_autoreload_test)")
    parser.add_argument("--template", default=DEFAULT_TEMPLATE,
                        help="the template project to reload from (default: the checked-in fixture "
                             "at scripts/testdata/autoreload_test_template.3mf). Point this at a "
                             "hand-authored one instead if you're testing a change to the template's "
                             "own layout or paint -- see test_auto_reload.py's docstring.")
    parser.add_argument("--hooks-dir", default=None,
                        help="scratch dir for the paint-loss-answer test hook file "
                             "(default <work-dir>/hooks)")
    parser.add_argument("--only", metavar="LETTERS",
                        help="run only these phases, comma-separated (e.g. --only L or --only G,K,L)")
    args = parser.parse_args()

    if not args.binary:
        sys.exit("Don't know this platform's build layout -- pass --binary explicitly.")
    if not os.path.exists(args.binary):
        sys.exit("No OrcaSlicer executable at %s -- build it first, or pass --binary." % args.binary)
    # Windows' CreateProcess doesn't resolve a relative path with forward slashes (subprocess.Popen
    # then fails with WinError 2, even though the os.path.exists() check above just passed against
    # the current working directory) -- an absolute path works regardless of slash direction.
    args.binary = os.path.abspath(args.binary)

    log_dir = os.path.join(args.data_dir, "log")
    if not os.path.isdir(log_dir):
        sys.exit("No log directory at %s -- pass --data-dir if OrcaSlicer stores its data elsewhere." % log_dir)

    work_dir = os.path.abspath(args.work_dir)
    os.makedirs(work_dir, exist_ok=True)
    template_path = os.path.abspath(args.template)
    project_path = os.path.join(work_dir, "autoreload_test.3mf")
    if not os.path.exists(template_path):
        sys.exit("No template project at %s -- pass --template, or run test_auto_reload.py once to "
                  "author one (its docstring: import the six STLs onto three plates, paint one, "
                  "save)." % template_path)
    check_template_sources(template_path)

    hooks_dir = os.path.abspath(args.hooks_dir) if args.hooks_dir else os.path.join(work_dir, "hooks")

    stl           = os.path.join(work_dir, "cube.stl")
    stl_g_changed = os.path.join(work_dir, "second_a.stl")
    stl_g_missing = os.path.join(work_dir, "second_b.stl")
    paint_stl     = os.path.join(work_dir, "painted.stl")
    flaky_stl     = os.path.join(work_dir, "flaky.stl")
    quick_stl     = os.path.join(work_dir, "quick.stl")

    # SourceFileWatcher::resolve_source_file_path() only re-points a bare recorded filename (what
    # the template stores) at <project folder>/<filename> if that candidate already exists on disk
    # at the moment a project is first loaded -- otherwise it's left unresolved and nothing this
    # script writes afterward is ever seen as a change. A work-dir that already has these from an
    # earlier run is unaffected (each phase overwrites its own target anyway); a brand new one
    # needs them written before the very first restart, not just whenever a phase gets to them.
    for path in (stl, stl_g_changed, stl_g_missing, paint_stl, flaky_stl, quick_stl):
        if not os.path.exists(path):
            write_cube_stl(path, 20)

    h1, h2 = args.slow_height, args.slow_height / 2
    results = []

    def record(name, ok, detail=""):
        results.append((name, ok, detail))
        print("  %s  %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + detail) if detail else ""))

    app = OrcaApp(args.binary, args.data_dir, log_dir, hooks_dir)
    tail = None  # replaced by _restart() before every phase group

    def _restart(want_reload, want_confirm, want_slice):
        nonlocal tail
        app.stop()
        write_prefs(args.data_dir, want_reload, want_confirm, want_slice)
        # A fresh copy every restart, same as the interactive script's per-run copy -- whatever an
        # earlier group's phases left the project as doesn't matter, since it's about to be
        # overwritten. This also means every group starts from the template's ORIGINAL cube.stl
        # size and painted.stl paint, regardless of what an earlier group's phases did to them.
        shutil.copy2(template_path, project_path)
        os.chmod(project_path, 0o644)
        print("\n--- restarting OrcaSlicer: reload=%s confirm=%s slice=%s ---" % (want_reload, want_confirm, want_slice))
        tail = app.start(project_path, args.startup_timeout, len(EXPECTED_TEMPLATE_SOURCES) if want_reload else 0)

    # --- phase bodies ------------------------------------------------------------------------
    # Mirrors test_auto_reload.py's phases one for one; see that file for the narrative context
    # behind each one. Differences from the interactive version are called out inline.

    def phase_a():
        print("\n[A] In-place overwrite (plate 1, cube.stl -> 30 mm)")
        tail.mark(); time.sleep(1.5)
        write_cube_stl(stl, 30)
        ok = tail.wait_for(RELOAD_MARK, args.timeout)
        record("A1 reload after in-place overwrite", ok, "" if ok else "no reload line within %gs" % args.timeout)
        if ok:
            record("A2 no slice when auto-slice is off", tail.absent_after(SLICE_START_MARK, args.quiet_window))
            size = tail.wait_for_size(stl, args.timeout)
            record("A3 model grew to 30 mm", size is not None and all(abs(v - 30) < 0.05 for v in size),
                   "" if size else "no matching bounding-box log line")

    def phase_b():
        print("\n[B] Rename-into-place, the temp-file-then-rename pattern most exporters use (-> 40 mm)")
        tail.mark(); time.sleep(1.5)
        write_cube_stl(stl, 40, atomic=True)
        ok = tail.wait_for(RELOAD_MARK, args.timeout)
        record("B1 reload after rename-into-place", ok, "" if ok else "no reload line within %gs" % args.timeout)
        if ok:
            size = tail.wait_for_size(stl, args.timeout)
            record("B2 model grew to 40 mm", size is not None and all(abs(v - 40) < 0.05 for v in size))

    def phase_c():
        print("\n[C] In-place overwrite again after the rename (-> 50 mm) -- checks the watch was re-armed")
        tail.mark(); time.sleep(1.5)
        write_cube_stl(stl, 50)
        ok = tail.wait_for(RELOAD_MARK, args.timeout)
        record("C1 reload after overwrite following a rename", ok, "" if ok else "no reload line within %gs" % args.timeout)

    def phase_g():
        print("\n[G] Plate 2: one object's source changes, the other's vanishes")
        if not os.path.exists(stl_g_missing):
            write_cube_stl(stl_g_missing, 8)
            time.sleep(1.5)
        tail.mark(); time.sleep(1.5)
        write_cube_stl(stl_g_changed, 24)
        os.remove(stl_g_missing)
        ok = tail.wait_for(RELOAD_MARK, args.timeout)
        record("G1 reload after the change", ok, "" if ok else "no reload line within %gs" % args.timeout)
        if ok:
            count = tail.last_match(RELOADABLE_COUNT_RE)
            record("G2 only the changed volume was selected for reload", count == "1",
                   "reloadable volumes number is: %s (expected 1)" % count)
            record("G3 missing object's volume was never selected in the first place",
                   not tail.has_seen(MISSING_SOURCE_MARK))
            size = tail.wait_for_size(stl_g_changed, args.timeout)
            record("G4 only the changed object updated, to 24 mm",
                   size is not None and all(abs(v - 24) < 0.05 for v in size))

    def phase_h():
        print("\n[H] Plate 3: overwrite a painted object, decline the paint-loss prompt")
        app.set_paint_answer("no")
        tail.mark(); time.sleep(1.5)
        write_cube_stl(paint_stl, 19)
        seen = tail.wait_for(RELOAD_MARK, args.timeout)
        record("H1 watcher noticed the change", seen, "" if seen else "no reload line within %gs" % args.timeout)
        if seen:
            declined = tail.wait_for(PAINT_DECLINED_MARK, args.timeout)
            record("H2 the decline was auto-answered and logged", declined)
            record("H3 the object was not reloaded (still 18 mm)",
                   tail.absent_after("reloaded " + paint_stl, 5.0) if declined else False)

    def phase_n():
        print("\n[N] Plate 3: overwrite the same painted object, accept the paint-loss prompt")
        app.set_paint_answer("yes")
        tail.mark(); time.sleep(1.5)
        write_cube_stl(paint_stl, 21)
        seen = tail.wait_for(RELOAD_MARK, args.timeout)
        record("N1 watcher noticed the change", seen, "" if seen else "no reload line within %gs" % args.timeout)
        if seen:
            not_declined = tail.absent_after(PAINT_DECLINED_MARK, args.quiet_window)
            record("N2 no decline was logged this time", not_declined)
            size = tail.wait_for_size(paint_stl, args.timeout)
            record("N3 model grew to 21 mm", size is not None and all(abs(v - 21) < 0.05 for v in size))
            # N4 "the paint is gone" (interactive script) has no log-observable proxy --
            # ModelVolume::is_any_painted() isn't logged anywhere in the reload path. Dropped.

    def phase_o():
        print("\n[O] Plate 3: overwrite the painted object with the confirm preference off -- must reload silently")
        # This group's _restart() just gave paint_stl a fresh copy of the template, so it has its
        # original paint intact regardless of what phase N (a different group, different project
        # copy) did to it -- no manual "make sure it still has paint" step needed here.
        tail.mark(); time.sleep(1.5)
        write_cube_stl(paint_stl, 23)
        seen = tail.wait_for(RELOAD_MARK, args.timeout)
        record("O1 watcher noticed the change", seen, "" if seen else "no reload line within %gs" % args.timeout)
        if seen:
            size = tail.wait_for_size(paint_stl, args.timeout)
            record("O2 model grew to 23 mm", size is not None and all(abs(v - 23) < 0.05 for v in size))

    def phase_i():
        print("\n[I] Plate 3: overwrite with a truncated/corrupt file, then a valid one")
        # flaky.stl isn't supposed to be painted -- clear any answer phase H/N left in the hook
        # file so a template mistake that puts paint on it blocks (and times out) instead of
        # silently reusing whatever answer happened to be sitting there.
        app.set_paint_answer(None)
        tail.mark(); time.sleep(1.5)
        write_truncated_stl(flaky_stl)
        ok = tail.wait_for(RELOAD_MARK, args.timeout)
        record("I1 a reload was attempted for the corrupt write", ok,
               "" if ok else "no reload line within %gs" % args.timeout)
        if ok:
            failed = tail.wait_for(LOAD_FAILED_MARK, args.timeout)
            record("I2 the load failure was logged, not silently accepted", failed)
            pushed = tail.wait_for(NOTIFICATION_MARK, args.timeout)
            record("I3 the failure notification call fired", pushed)
            # I4/I5 in the interactive script (no dialog / toast actually visible) need eyes on
            # the screen; dropped here. A blocking dialog would instead show up as this whole
            # phase timing out, since nothing else in the reload path would proceed to log.
            tail.mark()
            write_cube_stl(flaky_stl, 12)
            ok2 = tail.wait_for(RELOAD_MARK, args.timeout)
            record("I4 a later valid write still reloads (the failed attempt didn't consume it)", ok2,
                   "" if ok2 else "no reload line within %gs" % args.timeout)
            if ok2:
                size = tail.wait_for_size(flaky_stl, args.timeout)
                record("I5 model shrank to 12 mm", size is not None and all(abs(v - 12) < 0.05 for v in size))

    def phase_j():
        print("\n[J] Plate 3: two overwrites landing close together, different sizes -- both must be picked up")
        # quick.stl isn't supposed to be painted either -- same reasoning as phase_i().
        app.set_paint_answer(None)
        tail.mark(); time.sleep(1.5)
        write_cube_stl(quick_stl, 18)
        ok = tail.wait_for(RELOAD_MARK, args.timeout)
        record("J1 reload after the first write", ok, "" if ok else "no reload line within %gs" % args.timeout)
        if ok:
            tail.mark()
            write_cube_stl(quick_stl, 14.5)
            ok2 = tail.wait_for(RELOAD_MARK, args.timeout)
            record("J2 reload after the second write", ok2,
                   "" if ok2 else "no reload line within %gs -- a same-second rewrite may have been missed" % args.timeout)
            if ok2:
                size = tail.wait_for_size(quick_stl, args.timeout)
                record("J3 final geometry is the second write (14.5 mm, not 18)",
                       size is not None and all(abs(v - 14.5) < 0.05 for v in size))

    def phase_k():
        print("\n[K] Plate 2: background directory noise while overwriting -- reload must still fire once the write settles")
        stop_noise = threading.Event()
        noise_thread = threading.Thread(target=directory_noise, args=(work_dir, stop_noise), daemon=True)
        tail.mark()
        noise_thread.start()
        time.sleep(0.5)
        write_cube_stl(stl_g_changed, 16)
        ok = tail.wait_for(RELOAD_MARK, 6.0)  # well under the noise's duration, comfortably above the ~700ms worst case
        stop_noise.set()
        noise_thread.join(timeout=2.0)
        record("K1 reload still fires despite directory noise", ok,
               "" if ok else "no reload line within 6s -- unrelated noise may be starving the stability check")
        if ok:
            size = tail.wait_for_size(stl_g_changed, args.timeout)
            record("K2 model shrank to 16 mm", size is not None and all(abs(v - 16) < 0.05 for v in size))

    def phase_p():
        print("\n[P] Plate 2: a slow multi-chunk write (like a large STEP/3MF export) must not be read until it stops growing")
        tail.mark(); time.sleep(1.5)
        write_cube_stl_slow(stl_g_changed, 11)  # reuses second_a.stl from phase K, shrinking it further
        quiet_during_write = not tail.has_seen(RELOAD_MARK) and not tail.has_seen(LOAD_FAILED_MARK)
        record("P1 no reload or load-failure logged while the file was still growing", quiet_during_write,
               "" if quiet_during_write else "a reload was attempted mid-write -- the stability check may not be holding")
        ok = tail.wait_for(RELOAD_MARK, args.timeout)
        record("P2 reload fires once the write settles", ok,
               "" if ok else "no reload line within %gs after the write finished" % args.timeout)
        if ok:
            record("P3 no load failure was logged for it (read only once it was complete)",
                   not tail.has_seen(LOAD_FAILED_MARK))
            size = tail.wait_for_size(stl_g_changed, args.timeout)
            record("P4 model shrank to 11 mm", size is not None and all(abs(v - 11) < 0.05 for v in size))

    def phase_d():
        print("\n[D] Plate 1: in-place overwrite with auto-slice on (-> 25 mm)")
        tail.mark(); time.sleep(1.5)
        write_cube_stl(stl, 25)
        ok = tail.wait_for(RELOAD_MARK, args.timeout)
        record("D1 reload", ok, "" if ok else "no reload line within %gs" % args.timeout)
        if ok:
            started = tail.wait_for(SLICE_START_MARK, args.timeout)
            record("D2 slice started automatically", started, "" if started else "no slice-start line within %gs" % args.timeout)
            if started:
                done = tail.wait_for(SLICE_DONE_MARK, args.timeout * 3)
                record("D3 slice completed", done, "" if done else "no completion line within %gs" % (args.timeout * 3))
            record("D4 view switched to Preview", tail.has_seen("select preview"))

    def phase_l():
        print("\n[L] Multi-plate: only the plate with the reloaded object should reslice")
        tail.mark(); time.sleep(1.5)
        write_cube_stl(stl_g_changed, 10)
        ok = tail.wait_for(RELOAD_MARK, args.timeout)
        record("L1 reload after the change", ok, "" if ok else "no reload line within %gs" % args.timeout)
        if ok:
            m = tail.wait_for_match(SLICE_START_PLATE_RE, args.timeout)
            record("L2 slice started automatically", m is not None,
                   "" if m else "no slice-start line within %gs" % args.timeout)
            if m:
                plate_idx = m.group(1)
                other_plate = re.search(r"will start print::process, plate (?!%s\b)\d+" % re.escape(plate_idx), tail.buf)
                record("L3 no other plate started slicing", other_plate is None,
                       "" if other_plate is None else other_plate.group(0))
                done = tail.wait_for(SLICE_DONE_MARK, args.timeout * 3)
                record("L4 slice completed", done, "" if done else "no completion line within %gs" % (args.timeout * 3))
                switched = tail.last_match(PLATE_SWITCH_RE)
                record("L5 view left on the plate that changed", switched == plate_idx,
                       "current plate is %s, expected %s" % (switched, plate_idx))
        # This drops the interactive script's manual pre-slice of plate 1 (so it could later be
        # checked as untouched) -- no CLI hook slices a single plate mid-session to reproduce that
        # setup headlessly. L3 above checks the same underlying invariant a different way: no
        # OTHER plate's slice starts alongside the changed one's.

    def phase_m():
        print("\n[M] Plate 3: a corrupt write with auto-slice on must not slice; the valid one after it must")
        tail.mark(); time.sleep(1.5)
        write_truncated_stl(flaky_stl)
        ok = tail.wait_for(RELOAD_MARK, args.timeout)
        record("M1 a reload was attempted for the corrupt write", ok,
               "" if ok else "no reload line within %gs" % args.timeout)
        if ok:
            failed = tail.wait_for(LOAD_FAILED_MARK, args.timeout)
            record("M2 the load failure was logged", failed)
            record("M3 no slice for the failed reload", tail.absent_after(SLICE_START_MARK, args.quiet_window),
                   "a slice started although nothing was reloaded")
            tail.mark()
            write_cube_stl(flaky_stl, 17)
            ok2 = tail.wait_for(RELOAD_MARK, args.timeout)
            record("M4 the valid write after it reloads", ok2, "" if ok2 else "no reload line within %gs" % args.timeout)
            if ok2:
                started = tail.wait_for(SLICE_START_MARK, args.timeout)
                record("M5 ...and slices", started, "" if started else "no slice-start line within %gs" % args.timeout)
                if started:
                    done = tail.wait_for(SLICE_DONE_MARK, args.timeout * 3)
                    record("M6 slice completed", done, "" if done else "no completion line within %gs" % (args.timeout * 3))

    def phase_f():
        print("\n[F] Plate 1: change arriving mid-slice: cube -> %g mm block with buried voids, then a plain %g mm block while that slices"
              % (h1, h2))
        tail.mark(); time.sleep(1.5)
        write_buried_pillars_stl(stl, h1)
        ok = tail.wait_for(RELOAD_MARK, args.timeout) and tail.wait_for(SLICE_START_MARK, args.timeout)
        record("F1 reload and slice start for the block", ok)
        if ok:
            time.sleep(args.mid_slice_delay)
            still_running = not tail.has_seen(SLICE_DONE_MARK)
            record("F2 first slice still running when the second change is written", still_running,
                   "" if still_running else "it already finished; raise --slow-height or lower --mid-slice-delay")
            tail.mark()
            write_buried_pillars_stl(stl, h2, with_voids=False)
            ok = tail.wait_for(RELOAD_MARK, args.timeout)
            record("F3 reload while slicing", ok, "" if ok else "no reload line within %gs" % args.timeout)
            if ok:
                restarted = tail.wait_for(SLICE_START_MARK, args.timeout)
                record("F4 slice restarted after the reload", restarted,
                       "" if restarted else "no second slice-start line within %gs" % args.timeout)
                if restarted:
                    done = tail.wait_for(SLICE_DONE_MARK, args.timeout * 6)
                    record("F5 restarted slice completed", done, "" if done else "no completion line within %gs" % (args.timeout * 6))
                size = tail.wait_for_size(stl, args.timeout)
                record("F6 final height is the second change (%g mm, not %g)" % (h2, h1),
                       size is not None and abs(size[2] - h2) < 0.05)
                # "...and solid, with no voids" (interactive script) can't be told from a bounding
                # box alone -- would need real mesh/volume inspection this log doesn't provide.

    def phase_e():
        print("\n[E] Plate 1: in-place overwrite with auto-reload off (-> 35 mm)")
        tail.mark(); time.sleep(1.5)
        write_cube_stl(stl, 35)
        quiet = tail.absent_after(RELOAD_MARK, args.quiet_window)
        record("E1 no reload when auto-reload is off", quiet, "" if quiet else "a reload happened anyway")
        # E2 "model unchanged" (interactive script) is implicit in E1: no reload means nothing
        # emits a bounding-box size line for it either.

    # (state, [phase letters]), in the order test_auto_reload.py's PHASES groups them.
    STATE_GROUPS = [
        ((True, True, False), ["A", "B", "C", "G", "H", "N", "I", "J", "K", "P"]),
        ((True, False, False), ["O"]),
        ((True, True, True), ["D", "L", "M", "F"]),
        ((False, True, True), ["E"]),
    ]
    PHASE_FUNCS = {
        "A": phase_a, "B": phase_b, "C": phase_c, "G": phase_g, "H": phase_h, "N": phase_n,
        "I": phase_i, "J": phase_j, "K": phase_k, "P": phase_p, "O": phase_o, "D": phase_d, "L": phase_l,
        "M": phase_m, "F": phase_f, "E": phase_e,
    }

    if args.only:
        wanted = {letter.strip().upper() for letter in args.only.split(",") if letter.strip()}
        unknown = wanted - set(PHASE_FUNCS)
        if unknown:
            sys.exit("Unknown phase(s): %s. Valid phases: %s" % (", ".join(sorted(unknown)), ", ".join(PHASE_FUNCS)))
    else:
        wanted = None

    last_state = None
    try:
        for state, letters in STATE_GROUPS:
            selected = [l for l in letters if wanted is None or l in wanted]
            if not selected:
                continue
            _restart(*state)
            last_state = state
            for letter in selected:
                PHASE_FUNCS[letter]()
    finally:
        app.stop()

    failed = [r for r in results if not r[1]]
    print("\n%d checks, %d failed" % (len(results), len(failed)))
    for name, _, detail in failed:
        print("  FAIL %s%s" % (name, (" -- " + detail) if detail else ""))
    if last_state is not None:
        want_reload, want_confirm, want_slice = last_state
        print("\nOrcaSlicer.conf at %s was left with %s=%s, %s=%s, %s=%s -- restore these to "
              "whatever you actually want for normal use."
              % (args.data_dir, PREF_RELOAD, want_reload, PREF_CONFIRM, want_confirm, PREF_SLICE, want_slice))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
