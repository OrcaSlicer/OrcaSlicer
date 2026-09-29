#!/usr/bin/env python3
"""Headless, fully automated end-to-end test for the auto-reload-simple branch.

This is a from-scratch, much smaller counterpart to auto-reload-pr's
test_auto_reload_headless.py -- not a port of it. That script's phases exercise behavior this
branch deliberately doesn't have: the stability-gated poll loop, per-file retry/backoff, and the
notification-based non-interactive reload path. This branch has exactly one reload path
(Plater::priv::reload_from_disk()), used identically whether triggered manually or by the
watcher -- see docs/HLSD/auto-reload.md. What's worth covering here is narrower and different in
kind: the three Preferences options actually gate what they claim to, and the paint-loss dialog's
decline only drops the declined volume, not an unrelated sibling that happens to share its source
file (the scenario this script exists to catch: a painted clone and an unpainted one, same source,
one dialog).

Two categories of scenario are deliberately NOT covered here, not just under-instrumented:
  - A missing source file (the "Please select a file" wxFileDialog) or a load failure (the
    "Error during reload" MessageDialog) would show a real modal dialog this script has no hook to
    answer -- unlike auto-reload-pr's notification-based non-interactive path, there is no
    non-blocking equivalent in this design (see "The watcher's trigger is not a special case" in
    docs/HLSD/auto-reload.md). Triggering either headlessly would hang the run, not just leave a
    gap in coverage, so this script is careful never to write a missing/corrupt source.
  - Whether a *manual* "Reload from disk" re-arms the watch needs an actual menu click or
    keyboard shortcut; nothing here can drive that. Verified manually instead.

Uses a small checked-in template project at scripts/testdata/auto_reload_simple_test_template.3mf.
That template can't be authored by this script: it needs one volume with real paint on it, and
painting isn't scriptable (there's no CLI for it -- src/slic3r/Utils/PaintCLI.cpp only inspects
existing paint, it doesn't apply any). If that file is missing, see its own "how to (re)author
this" note for the one-time interactive setup.

Preferences can't be changed in a running instance (AppConfig loads OrcaSlicer.conf once at
startup and never re-reads it), so this script restarts the app whenever the phases ahead need a
different (reload, confirm, slice) combination.

    python3 scripts/test_auto_reload_simple_headless.py --binary <path to OrcaSlicer>

Use --only to run just one or more phases, e.g. --only D,E.
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
DEFAULT_TEMPLATE = os.path.join(SCRIPT_DIR, "testdata", "auto_reload_simple_test_template.3mf")

RELOAD_MARK          = "source file(s) changed on disk, reloading"
SLICE_START_MARK     = "will start print::process"
PAINT_DECLINED_MARK  = "skipping reload, declined in the paint-loss prompt"
WATCHING_RE          = re.compile(r"watching (\d+) source file\(s\) for changes")
# "reload_from_disk: reloaded /path/to/file.stl, bounding box size = 12 x 12 x 12 mm"
SIZE_RE              = re.compile(r"reloaded (\S.*?), bounding box size = ([\d.eE+-]+) x ([\d.eE+-]+) x ([\d.eE+-]+) mm")

PREF_RELOAD  = "auto_reload_on_source_change"
PREF_CONFIRM = "auto_reload_confirm_paint_loss"
PREF_SLICE   = "auto_slice_after_reload"

# The template must reference basic.stl once (an unpainted, unshared object) and clone.stl twice
# (two objects/volumes sharing one source file -- one painted, one not). Distinct watched files is
# 2 (basic.stl, clone.stl), not 3: the watcher tracks files, not volumes.
EXPECTED_WATCHED_FILES = 2
_SOURCE_FILE_RE = re.compile(r'key="source_file"\s+value="([^"]+)"')


def check_template(template_path):
    """Fails loudly, before any phase runs, if the template doesn't actually reference basic.stl
    once and clone.stl (at least) twice -- a template missing the clone relationship otherwise
    shows up much later as a confusing "wrong volume count" failure in phase D."""
    with zipfile.ZipFile(template_path) as zf:
        config = zf.read("Metadata/model_settings.config").decode("utf-8", errors="replace")
    names = [os.path.basename(v) for v in _SOURCE_FILE_RE.findall(config)]
    basic_count = names.count("basic.stl")
    clone_count = names.count("clone.stl")
    if basic_count != 1 or clone_count < 2:
        sys.exit("%s doesn't look right: basic.stl referenced %d time(s) (want 1), clone.stl "
                  "referenced %d time(s) (want >= 2, one painted + one not). Redo the one-time "
                  "interactive setup -- see this script's module docstring and the comment at the "
                  "top of main()." % (template_path, basic_count, clone_count))


def binary_path_suggestions():
    configs = ("Debug", "Release", "RelWithDebInfo")
    system = platform.system()
    if system == "Darwin":
        return [os.path.join(REPO_ROOT, "build/arm64-ninja/src", config, "OrcaSlicer.app/Contents/MacOS/OrcaSlicer")
                for config in configs]
    if system == "Windows":
        return [os.path.join(REPO_ROOT, "build", "src", config, "orca-slicer.exe") for config in configs]
    if system == "Linux":
        build_dirs = {"Release": "build", "Debug": "build-dbg", "RelWithDebInfo": "build-dbginfo"}
        return [os.path.join(REPO_ROOT, build_dirs[config], "src", config, "orca-slicer") for config in configs]
    return []


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


_MONTHS = {"Jan": 1, "Feb": 2, "Mar": 3, "Apr": 4, "May": 5, "Jun": 6,
           "Jul": 7, "Aug": 8, "Sep": 9, "Oct": 10, "Nov": 11, "Dec": 12}
_LOG_NAME_RE = re.compile(r"debug_\w{3}_(\w{3})_(\d{2})_(\d{2})_(\d{2})_(\d{2})_(\d+)\.log")


def _log_launch_key(path):
    """Sorts by the launch timestamp embedded in OrcaSlicer's own log filename, not the file's
    mtime, which a copy or an unrelated later write can disturb."""
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
    # JSON, so parse just the leading object and ignore whatever follows it.
    config, _ = json.JSONDecoder().raw_decode(text)
    return config, path


def write_prefs(data_dir, want_reload, want_confirm, want_slice):
    """Only safe to call while the app isn't running: AppConfig loads this file once at startup
    and never re-reads it. Must leave a trailing newline after the closing brace -- see
    AppConfig::load()'s Windows-only substr() past it, which throws on a file that doesn't have
    one, whether or not a checksum comment actually follows."""
    config, path = read_full_config(data_dir)
    app = config.setdefault("app", {})
    app[PREF_RELOAD] = want_reload
    app[PREF_CONFIRM] = want_confirm
    app[PREF_SLICE] = want_slice
    # Suppresses the "Customized Preset"/"Modified G-code" modal that a template authored on a
    # different machine would otherwise trip with nobody there to click it.
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
        # the exact instant this reads -- drain briefly before clearing so it can't be missed here
        # only to show up in a later caller's window and be mistaken for something new.
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

    def count_sizes(self, filename, timeout, want):
        """Waits until at least `want` 'reloaded <filename>, ...' lines have been seen (a shared
        source with several volumes logs one such line per volume reloaded), returning the list of
        (x, y, z) tuples seen, most recent last."""
        deadline = time.monotonic() + timeout
        base = os.path.basename(filename)
        hits = []
        while time.monotonic() < deadline:
            self._read()
            hits = [(float(x), float(y), float(z)) for path, x, y, z in SIZE_RE.findall(self.buf)
                    if os.path.basename(path) == base]
            if len(hits) >= want:
                return hits
            time.sleep(0.25)
        return hits

    def absent_after(self, marker, wait):
        time.sleep(wait)
        self._read()
        return marker not in self.buf

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

        # object_list_changed() (which logs this) fires more than once while a project loads --
        # once before the project folder is resolved, again after -- so wait for the specific
        # count this restart actually expects (both files, or zero when auto-reload is off)
        # instead of guessing when the log has gone quiet.
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


def write_cube_stl(path, size, atomic=False):
    s = float(size)
    v = [(0, 0, 0), (s, 0, 0), (s, s, 0), (0, s, 0), (0, 0, s), (s, 0, s), (s, s, s), (0, s, s)]
    lines = ["solid test"]
    for a, b, c in BOX_FACES:
        lines.append("  facet normal 0 0 0\n    outer loop")
        for i in (a, b, c):
            lines.append("      vertex %g %g %g" % v[i])
        lines.append("    endloop\n  endfacet")
    lines.append("endsolid test\n")
    data = "\n".join(lines)
    if atomic:
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            f.write(data)
        os.replace(tmp, path)
    else:
        with open(path, "w") as f:
            f.write(data)


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
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--data-dir", default=default_data_dir(), help="OrcaSlicer data directory")
    binary_help = "path to the OrcaSlicer executable (required)"
    suggestions = binary_path_suggestions()
    if suggestions:
        binary_help += ". e.g.:\n" + "\n".join(suggestions)
    parser.add_argument("--binary", required=True, help=binary_help)
    parser.add_argument("--timeout", type=float, default=15.0, help="seconds to wait for a reload/slice (default 15)")
    parser.add_argument("--quiet-window", type=float, default=6.0,
                        help="seconds to wait when asserting that nothing happens (default 6)")
    parser.add_argument("--startup-timeout", type=float, default=45.0,
                        help="seconds to wait for the app to launch and start watching files (default 45)")
    parser.add_argument("--work-dir", default=os.path.expanduser("~/orca_autoreload_simple_test"),
                        help="scratch dir for the generated STLs and the live project copy "
                             "(default ~/orca_autoreload_simple_test)")
    parser.add_argument("--template", default=DEFAULT_TEMPLATE,
                        help="the template project to reload from (default: the checked-in fixture "
                             "at scripts/testdata/auto_reload_simple_test_template.3mf)")
    parser.add_argument("--hooks-dir", default=None,
                        help="scratch dir for the paint-loss-answer test hook file "
                             "(default <work-dir>/hooks)")
    parser.add_argument("--only", metavar="LETTERS",
                        help="run only these phases, comma-separated (e.g. --only D,E)")
    args = parser.parse_args()

    if not os.path.exists(args.binary):
        sys.exit("No OrcaSlicer executable at %s -- build it first, or pass --binary." % args.binary)
    args.binary = os.path.abspath(args.binary)

    log_dir = os.path.join(args.data_dir, "log")
    if not os.path.isdir(log_dir):
        sys.exit("No log directory at %s -- pass --data-dir if OrcaSlicer stores its data elsewhere." % log_dir)

    work_dir = os.path.abspath(args.work_dir)
    os.makedirs(work_dir, exist_ok=True)
    template_path = os.path.abspath(args.template)
    project_path = os.path.join(work_dir, "auto_reload_simple_test.3mf")
    if not os.path.exists(template_path):
        sys.exit("No template project at %s -- see this script's module docstring for the "
                  "one-time interactive setup needed to author one (it needs real paint on one "
                  "volume, which nothing here can script)." % template_path)
    check_template(template_path)

    hooks_dir = os.path.abspath(args.hooks_dir) if args.hooks_dir else os.path.join(work_dir, "hooks")

    basic_stl = os.path.join(work_dir, "basic.stl")
    clone_stl = os.path.join(work_dir, "clone.stl")
    # SourceFileWatcher::resolve_source_file_path() only re-points a bare recorded filename (what
    # the template stores) at <project folder>/<filename> if that candidate already exists on disk
    # at the moment a project is first loaded -- so these need to exist before the very first
    # restart, not just whenever a phase gets to them.
    for path in (basic_stl, clone_stl):
        if not os.path.exists(path):
            write_cube_stl(path, 20)

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
        # A fresh copy every restart: every group starts from the template's original basic.stl
        # size and clone_a's original paint, regardless of what an earlier group's phases did.
        shutil.copy2(template_path, project_path)
        os.chmod(project_path, 0o644)
        print("\n--- restarting OrcaSlicer: reload=%s confirm=%s slice=%s ---" % (want_reload, want_confirm, want_slice))
        tail = app.start(project_path, args.startup_timeout, EXPECTED_WATCHED_FILES if want_reload else 0)

    # --- phase bodies ------------------------------------------------------------------------

    def phase_a():
        print("\n[A] In-place overwrite (basic.stl -> 30 mm)")
        tail.mark(); time.sleep(1.0)
        write_cube_stl(basic_stl, 30)
        ok = tail.wait_for(RELOAD_MARK, args.timeout)
        record("A1 reload after in-place overwrite", ok, "" if ok else "no reload line within %gs" % args.timeout)
        if ok:
            record("A2 no slice when auto-slice is off", tail.absent_after(SLICE_START_MARK, args.quiet_window))
            size = tail.wait_for_size(basic_stl, args.timeout)
            record("A3 model grew to 30 mm", size is not None and all(abs(v - 30) < 0.05 for v in size),
                   "" if size else "no matching bounding-box log line")

    def phase_b():
        print("\n[B] Rename-into-place (-> 40 mm) -- checks both watch kinds work")
        tail.mark(); time.sleep(1.0)
        write_cube_stl(basic_stl, 40, atomic=True)
        ok = tail.wait_for(RELOAD_MARK, args.timeout)
        record("B1 reload after rename-into-place", ok, "" if ok else "no reload line within %gs" % args.timeout)
        if ok:
            size = tail.wait_for_size(basic_stl, args.timeout)
            record("B2 model grew to 40 mm", size is not None and all(abs(v - 40) < 0.05 for v in size))

    def phase_c():
        print("\n[C] Two overwrites landing close together -- must settle on the final size, not the first")
        tail.mark(); time.sleep(1.0)
        write_cube_stl(basic_stl, 33)
        time.sleep(0.1)  # well under the 300ms debounce: both should coalesce into one reload
        write_cube_stl(basic_stl, 37)
        ok = tail.wait_for(RELOAD_MARK, args.timeout)
        record("C1 reload after the pair of writes", ok, "" if ok else "no reload line within %gs" % args.timeout)
        if ok:
            size = tail.wait_for_size(basic_stl, args.timeout)
            record("C2 final geometry is the second write (37 mm, not 33)",
                   size is not None and all(abs(v - 37) < 0.05 for v in size))

    def phase_d():
        print("\n[D] Clone + paint: overwrite clone.stl, decline the paint-loss prompt")
        app.set_paint_answer("no")
        tail.mark(); time.sleep(1.0)
        write_cube_stl(clone_stl, 25)
        seen = tail.wait_for(RELOAD_MARK, args.timeout)
        record("D1 watcher noticed the change", seen, "" if seen else "no reload line within %gs" % args.timeout)
        if seen:
            declined = tail.wait_for(PAINT_DECLINED_MARK, args.timeout)
            record("D2 the decline was auto-answered and logged", declined)
            sizes = tail.count_sizes(clone_stl, args.timeout, want=1)
            ok = len(sizes) == 1
            record("D3 exactly one clone.stl volume was reloaded (the unpainted one)", ok,
                   "" if ok else ("got %d" % len(sizes) if sizes else "no matching bounding-box log line"))
            if sizes:
                record("D4 the reloaded one grew to 25 mm", abs(sizes[0][0] - 25) < 0.05 and abs(sizes[0][1] - 25) < 0.05)

    def phase_e():
        print("\n[E] Clone + paint: overwrite clone.stl again, accept the paint-loss prompt")
        app.set_paint_answer("yes")
        tail.mark(); time.sleep(1.0)
        write_cube_stl(clone_stl, 15)
        seen = tail.wait_for(RELOAD_MARK, args.timeout)
        record("E1 watcher noticed the change", seen, "" if seen else "no reload line within %gs" % args.timeout)
        if seen:
            not_declined = tail.absent_after(PAINT_DECLINED_MARK, args.quiet_window)
            record("E2 no decline was logged this time", not_declined)
            sizes = tail.count_sizes(clone_stl, args.timeout, want=2)
            ok = len(sizes) == 2
            record("E3 both clone.stl volumes were reloaded", ok,
                   "" if ok else ("only %d" % len(sizes) if sizes else "no matching bounding-box log lines"))
            if len(sizes) == 2:
                record("E4 both grew to 15 mm", all(abs(v - 15) < 0.05 for size in sizes for v in size[:2]))

    def phase_f():
        print("\n[F] Background directory noise while overwriting -- reload must still fire")
        stop = threading.Event()
        noise_thread = threading.Thread(target=directory_noise, args=(work_dir, stop), daemon=True)
        noise_thread.start()
        try:
            tail.mark(); time.sleep(1.0)
            write_cube_stl(basic_stl, 44)
            ok = tail.wait_for(RELOAD_MARK, args.timeout)
            record("F1 reload still fires despite directory noise", ok,
                   "" if ok else "no reload line within %gs" % args.timeout)
            if ok:
                size = tail.wait_for_size(basic_stl, args.timeout)
                record("F2 model grew to 44 mm", size is not None and all(abs(v - 44) < 0.05 for v in size))
        finally:
            stop.set()
            noise_thread.join(timeout=2)

    def phase_g():
        print("\n[G] With auto-slice on: overwrite basic.stl -> slice starts automatically")
        tail.mark(); time.sleep(1.0)
        write_cube_stl(basic_stl, 22)
        ok = tail.wait_for(RELOAD_MARK, args.timeout)
        record("G1 reload", ok, "" if ok else "no reload line within %gs" % args.timeout)
        if ok:
            sliced = tail.wait_for(SLICE_START_MARK, args.timeout)
            record("G2 slice started automatically", sliced)

    def phase_h():
        print("\n[H] With the paint-loss confirmation off: clone.stl reloads silently, paint or not")
        tail.mark(); time.sleep(1.0)
        write_cube_stl(clone_stl, 18)
        seen = tail.wait_for(RELOAD_MARK, args.timeout)
        record("H1 watcher noticed the change", seen, "" if seen else "no reload line within %gs" % args.timeout)
        if seen:
            record("H2 no decline was logged (no dialog to decline)",
                   tail.absent_after(PAINT_DECLINED_MARK, args.quiet_window))
            sizes = tail.count_sizes(clone_stl, args.timeout, want=2)
            ok = len(sizes) == 2
            record("H3 both clone.stl volumes reloaded despite the paint", ok,
                   "" if ok else ("only %d" % len(sizes) if sizes else "no matching bounding-box log lines"))

    def phase_i():
        print("\n[I] With auto-reload off: no reload happens")
        tail.mark(); time.sleep(1.0)
        write_cube_stl(basic_stl, 50)
        quiet = tail.absent_after(RELOAD_MARK, args.quiet_window)
        record("I1 no reload when auto-reload is off", quiet, "" if quiet else "a reload happened anyway")

    STATE_GROUPS = [
        ((True, True, False), ["A", "B", "C", "D", "E", "F"]),
        ((True, True, True), ["G"]),
        ((True, False, False), ["H"]),
        ((False, True, False), ["I"]),
    ]
    PHASE_FUNCS = {
        "A": phase_a, "B": phase_b, "C": phase_c, "D": phase_d, "E": phase_e,
        "F": phase_f, "G": phase_g, "H": phase_h, "I": phase_i,
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
