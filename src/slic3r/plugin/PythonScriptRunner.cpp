#include "PythonScriptRunner.hpp"

#include "PluginAuditManager.hpp"
#include "PluginJsonPy.hpp"
#include "PluginLoader.hpp"
#include "PythonInterpreter.hpp"
#include "PythonRunScope.hpp"
#include "PythonSession.hpp"
#include "host/PluginHostTools.hpp"
#include "host/PluginHostUi.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/system/error_code.hpp>

#include <pybind11/attr.h>
#include <pybind11/cast.h>
#include <pybind11/embed.h>
#include <pybind11/eval.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/stl.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r {

namespace py = pybind11;
using nlohmann::json;

namespace {

// The module that gives each run its own Python world inside the one interpreter, installed once. It captures a
// run's output, over whatever sys.stdout and sys.stderr are then (the stderr tee to the Python log): writes from a
// thread bound to a run that has a capture go to it, all others pass through. It routes the interpreter's own
// __main__ module and sys.argv to the bound run's, and serves a file run's imports from the script's folder. Runs
// overlap (an agent's beside a person's), so every piece is keyed by the run bound to the calling thread, read
// through _run_id(), which the runner injects before the source runs, with _plugin_uses() and _plugin_importing().
constexpr const char* kCaptureModule = "_orca_run_capture";
constexpr const char* kCaptureSource = R"CAPTURE(
import atexit, concurrent.futures.thread, importlib.machinery, os, re, sys, threading, tomllib, traceback, types, weakref

class RunInterrupted(BaseException):
    """Raised in a script's code when its run is cancelled or out of time."""

_TRUNCATED = "\n[output truncated at 256 KiB]\n"

# A run's output: kept until the run ends, or, for a session, handed to `sink` (as UTF-8 bytes) as it is written.
# Either way a stretch of output is capped at LIMIT characters and then cut with the note; a session renews the
# allowance as each work starts and ends. A session's `results` takes its results as JSON, within the same allowance.
class _Capture:
    __slots__ = ("parts", "size", "truncated", "sink", "results")
    LIMIT = 256 * 1024
    def __init__(self, sink, results):
        self.parts = []
        self.size = 0
        self.truncated = False
        self.sink = sink
        self.results = results

_captures = {}

class _RunStream:
    def __init__(self, original):
        self._original = original
    def write(self, s):
        capture = _captures.get(_run_id())
        if capture is None:
            if self._original is not None:
                return self._original.write(s)
            return len(s)
        written = len(s)
        if capture.truncated:
            return written
        room = _Capture.LIMIT - capture.size
        if written > room:
            s = s[:room]
            capture.size = _Capture.LIMIT
            capture.truncated = True
        else:
            capture.size += written
        if capture.sink is None:
            capture.parts.append(s)
        else:
            capture.sink((s + _TRUNCATED if capture.truncated else s).encode("utf-8", "replace"))
        return written
    def flush(self):
        if self._original is not None:
            try:
                self._original.flush()
            except Exception:
                pass
    def writable(self):
        return True
    def __getattr__(self, name):
        return getattr(self._original, name)

def begin(run_id, sink=None, results=None):
    _captures[run_id] = _Capture(sink, results)

def result(run_id, text):
    """Host bookkeeping: hands a result's JSON to the run's `results`. False when the run has none or the JSON doesn't
    fit what is left of the allowance, and the caller prints the result instead."""
    capture = _captures.get(run_id)
    if capture is None or capture.results is None or capture.truncated or len(text) > _Capture.LIMIT - capture.size:
        return False
    capture.size += len(text)
    capture.results(text.encode("utf-8", "replace"))
    return True

def renew(run_id):
    """Host bookkeeping: a session's next stretch of output gets the whole allowance."""
    capture = _captures.get(run_id)
    if capture is not None:
        capture.size = 0
        capture.truncated = False

def end(run_id):
    capture = _captures.pop(run_id, None)
    if capture is None:
        return ""
    text = "".join(capture.parts)
    if capture.truncated:
        text += _TRUNCATED
    return text

sys.stdout = _RunStream(sys.stdout)
sys.stderr = _RunStream(sys.stderr)

# Each run's __main__ module and sys.argv. The interpreter's own __main__ and the sys module get a ModuleType
# subclass whose attribute access goes to the bound run's, so typing.get_type_hints, dataclasses and pickle find a
# script's globals, and argparse its argv, while code outside runs sees the app's. C code that reads the module's
# dict directly (PyModule_GetDict, PySys_GetObject) sees the app's. The routing methods take what they use as
# defaults, so they keep working while the interpreter finalizes and this module's globals are cleared.
_app_main = sys.modules["__main__"]
_app_argv = sys.argv
_mains = {}
_argvs = {}

class _MainRouter(types.ModuleType):
    def __getattribute__(self, name, _get=types.ModuleType.__getattribute__, _mains=_mains, _rid=_run_id):
        module = _mains.get(_rid())
        return _get(self, name) if module is None else getattr(module, name)
    def __setattr__(self, name, value, _set=types.ModuleType.__setattr__, _mains=_mains, _rid=_run_id):
        module = _mains.get(_rid())
        if module is None:
            _set(self, name, value)
        else:
            setattr(module, name, value)
    def __delattr__(self, name, _del=types.ModuleType.__delattr__, _mains=_mains, _rid=_run_id):
        module = _mains.get(_rid())
        if module is None:
            _del(self, name)
        else:
            delattr(module, name)

class _SysRouter(types.ModuleType):
    # Only argv is routed: every other attribute read on sys costs one Python call more than before.
    def __getattribute__(self, name, _get=types.ModuleType.__getattribute__, _argvs=_argvs, _rid=_run_id):
        if name == "argv":
            argv = _argvs.get(_rid())
            if argv is not None:
                return argv
        return _get(self, name)
    def __setattr__(self, name, value, _set=types.ModuleType.__setattr__, _argvs=_argvs, _rid=_run_id):
        if name == "argv":
            run_id = _rid()
            if run_id in _argvs:
                _argvs[run_id] = value
                return
        _set(self, name, value)

def _install_routers():
    # Again at each run's start: a script may have put a plain class back.
    if type(_app_main) is not _MainRouter:
        types.ModuleType.__setattr__(_app_main, "__class__", _MainRouter)
    if type(sys) is not _SysRouter:
        types.ModuleType.__setattr__(sys, "__class__", _SysRouter)

# A file run's imports from its script's folder, as sys.path[0] serves them under `python file.py`, without changing
# sys.path, which plugins share. The finder serves only threads bound to an active file run, from the folder and from
# the packages it served there (namespace packages included), never a submodule of a package found elsewhere, even one
# installed below the folder. Source is compiled afresh each run, never from or to a cached .pyc, so an edit shows on
# the next run whatever the file's timestamp; a sourceless .pyc and extension modules load as Python loads them. What
# it found for a folder is evicted when the last run from that folder ends, each module only while sys.modules still
# holds the module it found (its spec), and never an extension module, which can't be loaded twice, or one a plugin's
# load recorded as its own.
class _SourceLoader(importlib.machinery.SourceFileLoader):
    def get_code(self, fullname):
        path = self.get_filename(fullname)
        return self.source_to_code(self.get_data(path), path)

_LOADERS = ((importlib.machinery.ExtensionFileLoader, importlib.machinery.EXTENSION_SUFFIXES),
            (_SourceLoader, importlib.machinery.SOURCE_SUFFIXES),
            (importlib.machinery.SourcelessFileLoader, importlib.machinery.BYTECODE_SUFFIXES))

_run_folders = {}   # run id -> (folder key, folder path)
_folder_runs = {}   # folder key -> active file runs from it
_folder_specs = {}  # folder key -> {module name: the spec the finder found}
_pending = set()    # folders whose teardown waits for a later run's entry

def _key(path):
    return os.path.normcase(os.path.abspath(path))

def _inside(path, key):
    entry = _key(path)
    return entry == key or entry.startswith(key.rstrip(os.sep) + os.sep)

class _FolderFinder:
    @staticmethod
    def find_spec(name, path=None, target=None):
        folder = _run_folders.get(_run_id())
        if folder is None:
            return None
        key, folder_path = folder
        if path is None:
            if "." in name:
                return None
            entries = [folder_path]
        else:
            parent = name.rpartition(".")[0]
            served = _folder_specs.get(key, {}).get(parent)
            if served is None or getattr(sys.modules.get(parent), "__spec__", None) is not served:
                return None
            entries = [entry for entry in path if isinstance(entry, str) and _inside(entry, key)]
        for entry in entries:
            spec = importlib.machinery.FileFinder(entry, *_LOADERS).find_spec(name, target)
            if spec is not None:
                _folder_specs.setdefault(key, {})[name] = spec
                return spec
        return None

    @staticmethod
    def invalidate_caches():
        pass

# After the built-in and frozen importers, as sys.path[0] can't shadow those either.
sys.meta_path.insert(next((i for i, finder in enumerate(sys.meta_path) if finder is importlib.machinery.PathFinder),
                          len(sys.meta_path)), _FolderFinder)

# Pools and atexit belong to the code that made them. _origin() names whose own code a frame is, skipping
# concurrent.futures and this module: a run's __main__ code ("run"), a module the finder served for a folder
# ("folder"; shared by the runs from that folder, so it ends with the folder's teardown), or anything else (None: a
# library's or a plugin's, left alone).
_this_globals = globals()

def _origin(frame):
    while frame is not None and (frame.f_globals is _this_globals
                                 or str(frame.f_globals.get("__name__", "")).startswith("concurrent.futures")):
        frame = frame.f_back
    if frame is None:
        return None
    run_id = _run_id()
    module = _mains.get(run_id)
    if module is not None and frame.f_globals is module.__dict__:
        return ("run", run_id)
    spec = frame.f_globals.get("__spec__")
    if spec is not None:
        for key, specs in _folder_specs.items():
            for name, found in specs.items():
                if found is spec:
                    return ("folder", key, name)
    return None

# A pool's work belongs to the run that submitted it: each work item created from a thread of a run (submit creates
# exactly one per task it accepts) gets a tag of that run, which counts it on the run, and the C++ wrapper of
# _WorkItem.run runs the item inside that run. A future completed without its item running (cancelled, or failed by a
# broken pool) is uncounted by its callback; the tag uncounts only what it counted, and once, whichever of the two
# comes first. A submit that raises after creating its item gave the caller no future, though the item may be queued
# with no worker to run it: it is cancelled, so it never runs and is uncounted. The callback and that hand-off are in
# place before the count, so no count is ever left without them.
_pools = weakref.WeakSet()
_original_pool_init = concurrent.futures.thread.ThreadPoolExecutor.__init__
_original_submit = concurrent.futures.thread.ThreadPoolExecutor.submit
_original_item_init = concurrent.futures.thread._WorkItem.__init__
_submitting = threading.local()

def _pool_init(self, *args, **kwargs):
    _original_pool_init(self, *args, **kwargs)
    self._orca_origin = _origin(sys._getframe(1))
    _pools.add(self)

def _submit(self, fn, /, *args, **kwargs):
    _submitting.item = None
    try:
        return _original_submit(self, fn, *args, **kwargs)
    except BaseException:
        item = getattr(_submitting, "item", None)
        if item is not None:
            item.future.cancel()
        raise
    finally:
        _submitting.item = None

def _item_init(self, future, fn, args, kwargs):
    _original_item_init(self, future, fn, args, kwargs)
    tag = _item_tag()
    if tag is None:
        return
    self._orca_run = tag
    future.add_done_callback(lambda done, tag=tag: _claim(tag) and _task_done(tag))
    _submitting.item = self
    if not _count(tag):
        raise RuntimeError("cannot schedule new futures: the script's run has ended")

concurrent.futures.thread.ThreadPoolExecutor.__init__ = _pool_init
concurrent.futures.thread.ThreadPoolExecutor.submit = _submit
concurrent.futures.thread._WorkItem.__init__ = _item_init

def _shut_down(pools):
    # Each pool stays listed until its shutdown returns, so an interrupt only delays the rest. A pool already shut down
    # is shut down again: an interrupt may have cut its shutdown after setting _shutdown and before waking its workers,
    # and another wake-up costs nothing.
    while pools:
        try:
            pools[-1].shutdown(wait=False)
        except Exception:
            traceback.print_exc()
        pools.pop()

def shutdown_own_pools(run_id):
    _shut_down([pool for pool in list(_pools) if getattr(pool, "_orca_origin", None) == ("run", run_id)])

_original_register = atexit.register
_original_unregister = atexit.unregister
_run_exits = {}     # run id -> [(func, args, kwargs)]
_folder_exits = {}  # folder key -> [(func, args, kwargs, name of the module that registered it)]

def _register(func, *args, **kwargs):
    origin = _origin(sys._getframe(1))
    if origin is None:
        return _original_register(func, *args, **kwargs)
    if origin[0] == "run":
        _run_exits.setdefault(origin[1], []).append((func, args, kwargs))
    else:
        _folder_exits.setdefault(origin[1], []).append((func, args, kwargs, origin[2]))
    return func

def _unregister(func):
    # Within the caller's own registrations, filed as register filed them: another run's, a folder's and the process's
    # stay.
    origin = _origin(sys._getframe(1))
    if origin is None:
        return _original_unregister(func)
    exits = (_run_exits if origin[0] == "run" else _folder_exits).get(origin[1], [])
    exits[:] = [entry for entry in exits if entry[0] != func]

atexit.register = _register
atexit.unregister = _unregister

def call_exits(exits):
    """Last registered first, each on its own, as Python's exit runs them. Each is taken off the list before it runs,
    so an interrupt cuts only the one it lands in and the runner resumes the rest."""
    while exits:
        func, args, kwargs = exits.pop()[:3]
        try:
            func(*args, **kwargs)
        except RunInterrupted:
            pass
        except BaseException:
            traceback.print_exc()

def take_exits(run_id):
    """Host bookkeeping: the run's atexit functions, for call_exits(). Taken once, as Python's exit takes them, so one
    registered while they run is not called."""
    return _run_exits.pop(run_id, [])

# A folder's teardown, in two steps. _take_folder is host bookkeeping, which the runner calls where no interrupt reaches
# and which runs none of the script's code: it takes everything the folder holds, evicts its modules and lists the
# pools they made, so a run that enters the folder while the second step runs (that step's code may let it in) imports
# its helpers afresh and keeps them, and their pools. A module that stays (a plugin's, an extension, one replaced in
# sys.modules) has its atexit functions handed to the process's exit there. teardown() then ends the evicted modules as
# Python ends a program's: their atexit functions, their globals cleared (so what they hold, cycles included,
# finalizes here, under the run and its watcher, while their pools still work), and their pools. Each piece stays in
# what was taken until it is done, so the runner resumes teardown() after an interrupt and only the piece the
# interrupt lands in is cut short.
class _Taken:
    __slots__ = ("exits", "modules", "pools")

def _take_folder(key):
    _pending.discard(key)
    specs = _folder_specs.pop(key, {})
    exits = _folder_exits.pop(key, [])
    taken = _Taken()
    taken.modules = {}
    for name, spec in specs.items():
        module = sys.modules.get(name)
        if (module is not None and getattr(module, "__spec__", None) is spec
                and not isinstance(spec.loader, importlib.machinery.ExtensionFileLoader) and not _plugin_uses(name)):
            del sys.modules[name]
            taken.modules[name] = module
    for func, args, kwargs, name in exits:
        if name not in taken.modules:
            _original_register(func, *args, **kwargs)
    taken.exits = [entry for entry in exits if entry[3] in taken.modules]
    taken.pools = [pool for pool in list(_pools) if (getattr(pool, "_orca_origin", None) or ("",))[0] == "folder"
                   and pool._orca_origin[1] == key and pool._orca_origin[2] in taken.modules]
    return taken

def teardown(taken):
    call_exits(taken.exits)
    while taken.modules:
        name = next(iter(taken.modules))
        taken.modules[name].__dict__.clear()
        del taken.modules[name]
    _shut_down(taken.pools)

def enter(run_id, path):
    """Host bookkeeping: the run's __main__ dict, and what a teardown an earlier run left pending took, for teardown()
    before this run's code, or None. `path` is the script's file, or None for code sent as text."""
    _install_routers()
    module = types.ModuleType("__main__")
    taken = None
    if path is None:
        argv = list(_app_argv)
    else:
        module.__file__ = path
        argv = [path]
        folder_path = os.path.dirname(os.path.abspath(path))
        key = _key(folder_path)
        _run_folders[run_id] = (key, folder_path)
        _folder_runs[key] = _folder_runs.get(key, 0) + 1
        if _folder_runs[key] == 1 and key in _pending and not _plugin_importing():
            taken = _take_folder(key)
    _mains[run_id] = module
    _argvs[run_id] = argv
    return module.__dict__, taken

def release(run_id):
    """Host bookkeeping: the run stops using its folder. When it was the folder's last run, what the folder's teardown
    took, for teardown(); None when another run still uses the folder, or when a plugin's import in progress leaves the
    teardown pending for a later run's entry."""
    folder = _run_folders.pop(run_id, None)
    if folder is None:
        return None
    key = folder[0]
    runs = _folder_runs.get(key, 1) - 1
    if runs > 0:
        _folder_runs[key] = runs
        return None
    _folder_runs.pop(key, None)
    _pending.add(key)
    return None if _plugin_importing() else _take_folder(key)

# A file run's PEP 723 dependencies, found with the PEP's reference expression and read with tomllib, so quoted commas,
# multi-line arrays and comments read as TOML says. Windows line ends would hide the block from the expression.
_PEP723 = re.compile(r"(?m)^# /// (?P<type>[a-zA-Z0-9-]+)$\s(?P<content>(^#(| .*)$\s)+)^# ///$")

def script_dependencies(source):
    """The `dependencies` of the script's PEP 723 block; none without one. ValueError for a malformed block."""
    source = source.lstrip("﻿").replace("\r\n", "\n").replace("\r", "\n")
    blocks = [match for match in _PEP723.finditer(source) if match.group("type") == "script"]
    if not blocks:
        return []
    if len(blocks) > 1:
        raise ValueError("the script has more than one `# /// script` block")
    content = "".join(line[2:] if line.startswith("# ") else line[1:]
                      for line in blocks[0].group("content").splitlines(keepends=True))
    dependencies = tomllib.loads(content).get("dependencies", [])
    if not isinstance(dependencies, list) or not all(isinstance(item, str) for item in dependencies):
        raise ValueError("`dependencies` is not a list of strings")
    return dependencies

def leave(run_id):
    """Host bookkeeping: the run's routes go, and the app's __main__ comes back if the run replaced it. Returns what
    they held (its __main__, its argv, what replaced the app's __main__, atexit functions registered while its own
    ran), for the runner to drop where a finalizer of it can still be stopped."""
    held = (_mains.pop(run_id, None), _argvs.pop(run_id, None), sys.modules.get("__main__"), _run_exits.pop(run_id, None))
    if held[2] is not _app_main:
        sys.modules["__main__"] = _app_main
    return held
)CAPTURE";

// A session's own helpers in the capture module, run after kCaptureSource in the same module.
constexpr const char* kSessionSource = R"SESSION(
import ast, bisect, io, json, linecache, math, sys, traceback

# A session's traceback, which quotes the editor's text (`source`) for the frames in the session's file and linecache
# for the others, in the exception and in every exception chained to it or grouped in it. linecache itself is left
# alone: every run, plugin and later traceback shares it, and theirs must quote the file on disk.
def format_quoted(exc_type, exc, tb, filename, source):
    lines = io.StringIO(source, newline=None).readlines()
    def quoted(stack):
        frames = []
        for frame in stack:
            line = ""
            if frame.lineno:
                if frame.filename == filename:
                    line = lines[frame.lineno - 1] if 0 < frame.lineno <= len(lines) else ""
                else:
                    line = linecache.getline(frame.filename, frame.lineno)
            if line and not line.endswith("\n"):
                line += "\n"
            frames.append(traceback.FrameSummary(frame.filename, frame.lineno, frame.name, lookup_line=False, line=line,
                                                 end_lineno=frame.end_lineno, colno=frame.colno, end_colno=frame.end_colno))
        return traceback.StackSummary.from_list(frames)
    top = traceback.TracebackException(exc_type, exc, tb, lookup_lines=False)
    seen = set()
    todo = [top]
    while todo:
        item = todo.pop()
        if id(item) in seen:
            continue
        seen.add(id(item))
        item.stack = quoted(item.stack)
        todo.extend(nested for nested in (item.__cause__, item.__context__) if nested is not None)
        todo.extend(item.exceptions or ())
    return "".join(top.format())

def error_line(exc, tb, filename):
    """The innermost line of `tb` in `filename`, or a SyntaxError's own line there; 0 for none."""
    if isinstance(exc, SyntaxError) and exc.filename == filename and isinstance(exc.lineno, int):
        return exc.lineno
    line = 0
    while tb is not None:
        if tb.tb_frame.f_code.co_filename == filename and tb.tb_lineno:
            line = tb.tb_lineno
        tb = tb.tb_next
    return line

# A File work's cells, reported as their lines start running: LINE events on its module code only, through
# sys.monitoring's tool id 4 while the work runs. False when another owner holds the id.
_CELLS_TOOL = 4

def claim_cells(code, starts, emit):
    monitoring = sys.monitoring
    try:
        monitoring.use_tool_id(_CELLS_TOOL, "OrcaSlicer Run Script")
    except ValueError:
        return False
    current = [-1]
    def line(where, number, _code=code, _starts=starts, _emit=emit, _current=current, _find=bisect.bisect_right):
        if where is _code:
            cell = max(_find(_starts, number) - 1, 0)
            if cell != _current[0]:
                _current[0] = cell
                _emit(cell)
    monitoring.register_callback(_CELLS_TOOL, monitoring.events.LINE, line)
    monitoring.set_local_events(_CELLS_TOOL, code, monitoring.events.LINE)
    return True

def release_cells(code):
    """Host bookkeeping: the events and the callback go before the id, since freeing it leaves both in place."""
    monitoring = sys.monitoring
    monitoring.set_local_events(_CELLS_TOOL, code, 0)
    monitoring.register_callback(_CELLS_TOOL, monitoring.events.LINE, None)
    monitoring.free_tool_id(_CELLS_TOOL)

# A File work's module with each cell's result shown, as a Cell work shows its own: the last top-level statement that
# starts in a cell, when it is a bare expression, becomes a call with its value to `show`, the name the host binds to
# show_result() in the work's namespace, so neither __import__ nor sys.modules stands between a cell and its value.
# Every statement keeps its lines, so cells, tracebacks and error lines are the file's. The module's docstring stays its
# docstring.
def mark_cell_results(tree, starts, show):
    body = tree.body
    last = {}
    for at, statement in enumerate(body):
        last[max(bisect.bisect_right(starts, statement.lineno) - 1, 0)] = at
    for at in last.values():
        statement = body[at]
        if not isinstance(statement, ast.Expr):
            continue
        if at == 0 and isinstance(statement.value, ast.Constant) and isinstance(statement.value.value, str):
            continue
        statement.value = ast.copy_location(ast.Call(ast.Name(show, ast.Load()), [statement.value], []), statement.value)
    ast.fix_missing_locations(tree)

# A result as compact JSON when it is a dict or a list that JSON holds as is: str keys, finite floats, strings UTF-8
# holds (no lone surrogate, which the trip to the page would replace), and no tuple, subclass or other value JSON would
# turn into something else. None otherwise, and for anything too deep or that can't be written; never raises an
# Exception.
def result_json(value):
    def plain(item, depth):
        kind = type(item)
        if kind is dict:
            return depth < 100 and all(type(key) is str and plain(member, depth + 1) for key, member in item.items())
        if kind is list:
            return depth < 100 and all(plain(member, depth + 1) for member in item)
        if kind is float:
            return math.isfinite(item)
        return item is None or kind is str or kind is int or kind is bool
    try:
        if type(value) in (dict, list) and plain(value, 0):
            text = json.dumps(value, ensure_ascii=False)
            text.encode("utf-8")
            return text
    except Exception:
        pass
    return None
)SESSION";

// The capture module's functions and interrupt, installed on first use and held from then on, so a script
// that rebinds the module's names or replaces it in sys.modules changes nothing the runner calls. The
// references are kept for the process: dropping them after the interpreter finalizes would crash. GIL held.
struct Capture
{
    PyObject* begin              = nullptr;
    PyObject* end                = nullptr;
    PyObject* interrupt          = nullptr;
    PyObject* enter              = nullptr;
    PyObject* release            = nullptr;
    PyObject* teardown           = nullptr;
    PyObject* leave              = nullptr;
    PyObject* shutdown_own_pools = nullptr;
    PyObject* take_exits         = nullptr;
    PyObject* call_exits         = nullptr;
    PyObject* dependencies       = nullptr;
    PyObject* renew              = nullptr;
    PyObject* format_quoted      = nullptr;
    PyObject* error_line         = nullptr;
    PyObject* claim_cells        = nullptr;
    PyObject* release_cells      = nullptr;
    PyObject* mark_cell_results  = nullptr;
    PyObject* result_json        = nullptr;
    PyObject* result             = nullptr;
    PyObject* show_result        = nullptr;
};

// The name a File work's namespace binds show_result() to for the calls mark_cell_results() puts in its cells. A
// leading underscore keeps it out of the session's variable count.
constexpr const char* kShowResultName = "_orca_show_result";

// The work a session's hosting thread runs now, for the show_result() calls mark_cell_results() puts in a File work.
// The function is one for every run and can outlive the work in a script's hands, so it finds the work here rather than
// holding it.
struct ResultTarget
{
    py::dict*      ns      = nullptr;
    std::uint64_t  run_id  = 0;
    const Capture* capture = nullptr;
};
thread_local const ResultTarget* t_result_target = nullptr;

// The thread's ResultTarget while a work's code runs.
class ScopedResultTarget
{
public:
    explicit ScopedResultTarget(const ResultTarget& target) : m_previous(t_result_target) { t_result_target = &target; }
    ~ScopedResultTarget() { t_result_target = m_previous; }
    ScopedResultTarget(const ScopedResultTarget&)            = delete;
    ScopedResultTarget& operator=(const ScopedResultTarget&) = delete;

private:
    const ResultTarget* m_previous;
};

// A work's or a cell's result, as the REPL shows one: `_` names it, and the session gets it as a Result event when
// result_json() writes it and it fits the output's allowance, else it is printed as its repr. Nothing for None. It is
// C++, so a repr that raises leaves no frame of the host's in the traceback. GIL held.
void show_result(const py::object& value, const ResultTarget& target)
{
    if (value.is_none())
        return;
    (*target.ns)["_"] = value;
    const py::object text = py::handle(target.capture->result_json)(value);
    if (!text.is_none() && py::handle(target.capture->result)(target.run_id, text).cast<bool>())
        return;
    const py::module_ builtins = py::module_::import("builtins");
    builtins.attr("print")(builtins.attr("repr")(value));
}

// A work item's tag (item._orca_run, a capsule that owns it): the run it belongs to, the run's stop generation when the
// task was submitted, and its task's accounting there, in plain C++, so nothing between taking the count and recording
// it, or between testing the claim and setting it, can raise or run Python code.
struct ItemTag
{
    std::shared_ptr<PythonRunScope> run;
    std::uint64_t                   generation = 0;
    bool                            counted    = false;
    bool                            claimed    = false;
};

py::capsule item_tag(std::shared_ptr<PythonRunScope> run)
{
    const std::uint64_t generation = run->stop_generation();
    return py::capsule(new ItemTag{std::move(run), generation}, [](void* tag) { delete static_cast<ItemTag*>(tag); });
}

ItemTag& tag_of(const py::handle& capsule) { return *static_cast<ItemTag*>(py::reinterpret_borrow<py::capsule>(capsule).get_pointer()); }

// Counts the item's task on its run, unless the run's admission has closed. GIL held.
bool count_item(ItemTag& tag)
{
    tag.counted = tag.run->task_submitted();
    return tag.counted;
}

// The first of the run's _WorkItem.run wrapper and its future's callback to take a counted item uncounts it. GIL held.
bool claim_item(ItemTag& tag)
{
    if (!tag.counted || tag.claimed)
        return false;
    tag.claimed = true;
    return true;
}

// concurrent.futures.thread._WorkItem.run, for an item a thread of a run created: the whole item (the task's
// callable, the future's completion and so the done callbacks the pool calls from there) runs inside that run, on a
// worker that is the run's interrupt target meanwhile. Every BaseException stops here, so neither Stop nor a
// SystemExit in a callback ends the worker, which pools share across runs.
py::object run_work_item(const py::object& original, const py::handle& interrupt, const py::object& item)
{
    const py::object capsule = py::getattr(item, "_orca_run", py::none());
    if (capsule.is_none())
        return original(item);
    ItemTag&                        tag = tag_of(capsule);
    std::shared_ptr<PythonRunScope> run = tag.run;
    if (!claim_item(tag)) {
        // Taken by its future's callback, so its future is done without it: the stock run marks a cancelled one
        // notified, which wait() and as_completed() need, and raises for any other (a future completed by hand),
        // which must not end the worker either.
        try {
            original(item);
        } catch (py::error_already_set&) {
        }
        return py::none();
    }
    {
        ScopedPythonRun          bound(run, PythonRunThread::Started);
        ScopedPluginAuditContext audit(run->identity());
        const unsigned long      ident = PyThread_get_thread_ident();
        run->task_running(ident);
        try {
            const bool ended = run->admission_closed();
            if (ended || run->queued_task_refused(tag.generation)) {
                // A queued task doesn't run once its run has ended, or once a stop came after it was submitted; a
                // session that goes on after the stop runs the tasks its later work submits.
                const char* why    = ended || run->ending() ? "The script's run has ended" : "The task was stopped before it ran";
                py::object  future = item.attr("future");
                if (future.attr("set_running_or_notify_cancel")().cast<bool>())
                    future.attr("set_exception")(py::module_::import("builtins").attr("RuntimeError")(why));
            } else
                original(item);
        } catch (py::error_already_set& err) {
            if (!err.matches(interrupt)) {
                try {
                    py::module_::import("traceback").attr("print_exception")(err.type(), err.value(), err.trace());
                } catch (py::error_already_set&) {
                }
            }
        }
        run->task_finished(ident);
        PyThreadState_SetAsyncExc(ident, nullptr);
        PyErr_Clear();
    }
    run->task_done();
    return py::none();
}

const Capture& capture_functions()
{
    static Capture capture;
    if (capture.begin != nullptr)
        return capture;
    // Installed once even when the first runs start together: the source's imports release the GIL, so without the
    // lock a second run would install another module, with routing tables of its own. The lock is taken without the
    // GIL, so the installing run can take the GIL back.
    static std::mutex install_mutex;
    std::unique_lock<std::mutex> install_lock(install_mutex, std::defer_lock);
    {
        py::gil_scoped_release release;
        install_lock.lock();
    }
    if (capture.begin != nullptr)
        return capture;
    py::module_ module = py::reinterpret_steal<py::module_>(PyModule_New(kCaptureModule));
    if (!module)
        throw py::error_already_set();
    module.attr("_run_id") = py::cpp_function([] {
        const PythonRunBinding& binding = current_python_run();
        return binding.run ? binding.run->id() : std::uint64_t(0);
    });
    module.attr("_plugin_uses")      = py::cpp_function([](const std::string& name) { return PythonInterpreter::instance().plugin_uses_module(name); });
    module.attr("_plugin_importing") = py::cpp_function([] { return PythonInterpreter::instance().plugin_import_in_progress(); });
    module.attr("_item_tag")         = py::cpp_function([]() -> py::object {
        const PythonRunBinding& binding = current_python_run();
        return binding.run ? py::object(item_tag(binding.run)) : py::object(py::none());
    });
    module.attr("_count")     = py::cpp_function([](const py::handle& capsule) { return count_item(tag_of(capsule)); });
    module.attr("_claim")     = py::cpp_function([](const py::handle& capsule) { return claim_item(tag_of(capsule)); });
    module.attr("_task_done") = py::cpp_function([](const py::handle& capsule) { tag_of(capsule).run->task_done(); });
    // Off the hosting thread of the File work that runs it, it shows nothing.
    module.attr("show_result") = py::cpp_function([](const py::object& value) {
        if (t_result_target != nullptr)
            show_result(value, *t_result_target);
    });
    py::exec(kCaptureSource, module.attr("__dict__"));
    py::exec(kSessionSource, module.attr("__dict__"));
    py::module_::import("sys").attr("modules")[kCaptureModule] = module;
    const auto hold = [&module](const char* name) { return py::object(module.attr(name)).release().ptr(); };
    capture.end         = hold("end");
    capture.interrupt   = hold("RunInterrupted");
    capture.enter       = hold("enter");
    capture.release     = hold("release");
    capture.teardown    = hold("teardown");
    capture.leave       = hold("leave");
    capture.shutdown_own_pools = hold("shutdown_own_pools");
    capture.take_exits         = hold("take_exits");
    capture.call_exits         = hold("call_exits");
    capture.dependencies       = hold("script_dependencies");
    capture.renew              = hold("renew");
    capture.format_quoted      = hold("format_quoted");
    capture.error_line         = hold("error_line");
    capture.claim_cells        = hold("claim_cells");
    capture.release_cells      = hold("release_cells");
    capture.mark_cell_results  = hold("mark_cell_results");
    capture.result_json        = hold("result_json");
    capture.result             = hold("result");
    capture.show_result        = hold("show_result");
    {
        py::object item_class = py::module_::import("concurrent.futures.thread").attr("_WorkItem");
        py::object original   = item_class.attr("run");
        py::handle interrupt  = capture.interrupt;
        item_class.attr("run") = py::cpp_function([original, interrupt](const py::object& item) { return run_work_item(original, interrupt, item); },
                                                  py::name("run"), py::is_method(item_class));
    }
    // Last: a set `begin` is what tells a later caller, under the GIL, that the rest is there.
    capture.begin       = hold("begin");
    return capture;
}

// A UTF-8 path as a native one: wide on Windows, where boost's narrow conversion uses the ANSI code page.
boost::filesystem::path utf8_path(const std::string& utf8)
{
#ifdef _WIN32
    return boost::filesystem::path(boost::nowide::widen(utf8));
#else
    return boost::filesystem::path(utf8);
#endif
}

// A file run names an existing file; any other name (<run_script>, a bare traceback name) is code sent as text.
boost::filesystem::path run_file(const std::string& filename)
{
    if (filename.empty() || filename.front() == '<')
        return {};
    boost::system::error_code ec;
    boost::filesystem::path   path = utf8_path(filename);
    return boost::filesystem::is_regular_file(path, ec) ? path : boost::filesystem::path();
}

std::atomic<std::uint64_t> g_next_run_id{1};

std::mutex                g_installer_mutex;
PythonDependencyInstaller g_installer;

bool install_dependencies(const std::vector<std::string>& packages, std::string& error)
{
    PythonDependencyInstaller installer;
    {
        std::lock_guard<std::mutex> lock(g_installer_mutex);
        installer = g_installer;
    }
    return installer ? installer(packages, error) : plugin_loader::install_packages(packages, error);
}

// A run's `result` as JSON, refusing what JSON can't hold rather than stringifying it. GIL held.
json strict_json(const py::handle& value, int depth = 0)
{
    if (depth > 200)
        throw std::invalid_argument("result is nested too deeply");
    if (value.is_none())
        return nullptr;
    if (py::isinstance<py::bool_>(value))
        return value.cast<bool>();
    if (py::isinstance<py::int_>(value)) {
        try {
            return value.cast<std::int64_t>();
        } catch (const py::cast_error&) {
            throw std::invalid_argument("result holds an integer too large for JSON");
        }
    }
    if (py::isinstance<py::float_>(value)) {
        const double d = value.cast<double>();
        if (!std::isfinite(d))
            throw std::invalid_argument("result holds a number that is not finite");
        return d;
    }
    if (py::isinstance<py::str>(value))
        return value.cast<std::string>();
    if (py::isinstance<py::dict>(value)) {
        json out = json::object();
        for (const auto item : py::reinterpret_borrow<py::dict>(value)) {
            if (!py::isinstance<py::str>(item.first))
                throw std::invalid_argument("result holds a dict whose keys are not all strings");
            out[item.first.cast<std::string>()] = strict_json(item.second, depth + 1);
        }
        return out;
    }
    if (py::isinstance<py::list>(value) || py::isinstance<py::tuple>(value)) {
        json out = json::array();
        for (const auto item : value)
            out.push_back(strict_json(item, depth + 1));
        return out;
    }
    throw std::invalid_argument("result holds a " + std::string(Py_TYPE(value.ptr())->tp_name) + ", which is not JSON");
}

// How long a stop reaches a session's threads and tasks while it is idle, and how long a run or session's end waits for
// the work it started once a stop came.
constexpr std::chrono::seconds kStopWindow{5};

// Interrupts what a run or session runs when it is stopped, cancelled or out of time. What it interrupts depends on the
// phase: in Body (a work's code runs) and Exit (atexit functions, the namespace's finalizers, the folder's teardown) the
// hosting thread and the interrupt targets; in Threads (the hosting thread waits in C++ for the work it started) and
// Idle (a session waits for work) the targets only; in Drain (host bookkeeping) nothing, though it still records a stop.
// The phase is set, and read before an interrupt, under the GIL, so no interrupt lands after the phase that allowed it.
//
// A session's stop() reaches what runs from then until the next work starts, and while the session is idle only for
// kStopWindow; a cancel or a time-out reaches everything until the end, and ends the session.
//
// Lock order: the GIL before this object's mutex, and this mutex before the scope's.
class Watcher
{
public:
    enum class Reason { None, Stopped, Cancelled, TimedOut };
    enum class Phase { Body, Threads, Exit, Drain, Idle };

    struct Config
    {
        std::function<bool()>     cancelled;
        std::chrono::milliseconds budget{0};
        std::function<void()>     stop_calls;
        bool                      stop_closes_ui = false;
    };

    Watcher(Config config, PythonSession& session, unsigned long thread_ident, PyObject* interrupt, std::shared_ptr<PythonRunScope> scope)
        : m_config(std::move(config))
        , m_session(session)
        , m_ident(thread_ident)
        , m_interrupt(interrupt)
        , m_scope(std::move(scope))
        , m_start(std::chrono::steady_clock::now())
        , m_thread([this] { run(); })
    {}
    ~Watcher() { stop(); }
    Watcher(const Watcher&)            = delete;
    Watcher& operator=(const Watcher&) = delete;

    // Ends the watcher. Call without the GIL: an interrupt it is raising needs it.
    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }
    // Takes in the stops asked for so far, as the watcher's own loop does, so a work's tasks are counted after them, and
    // waits until no stop is still being applied (its tool calls cancelled, its modal windows ended), this call's own or
    // one the loop took, so the next work's calls never meet a stop meant for the work before. Call without the GIL.
    void settle()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        take_stops(lock);
        m_cv.wait(lock, [this] { return m_applying == 0; });
    }
    // With the GIL held, as a work starts: only the stops asked for after the first `floor` reach it.
    void begin_work(std::uint64_t floor)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_floor = floor;
    }
    // With the GIL held, as the teardown starts: a stop older than kStopWindow no longer reaches it.
    void begin_teardown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stops_seen > m_floor && std::chrono::steady_clock::now() - m_last_stop >= kStopWindow)
            m_floor = m_stops_seen;
    }
    // What reaches what runs now: the end (a cancel or a time-out) when one came, else Stopped when a stop came since the
    // work started.
    Reason reason() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return reason_locked();
    }
    // With the GIL held.
    void set_phase(Phase phase) { m_phase = phase; }
    // Time since the stop, cancel or time-out that reaches what runs now, or nullopt when none does.
    std::optional<std::chrono::steady_clock::duration> since_stop() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Reason why = reason_locked();
        if (why == Reason::None)
            return std::nullopt;
        return std::chrono::steady_clock::now() - (why == Reason::Stopped ? m_last_stop : m_end_at);
    }

private:
    Reason reason_locked() const
    {
        if (m_end != Reason::None)
            return m_end;
        return m_stops_seen > m_floor ? Reason::Stopped : Reason::None;
    }

    // Records new stop requests, a cancel and a time-out, and does what each asks: the run's queued tasks are refused,
    // its tool calls cancelled, and its modal windows ended (every window of a run that closes its UI at a stop). A
    // cancel or a time-out also ends the session; the first of them sticks. `lock` is released while that is done.
    void take_stops(std::unique_lock<std::mutex>& lock)
    {
        const auto now     = std::chrono::steady_clock::now();
        bool       stopped = false;
        if (const std::uint64_t asked = m_session.stop_requests(); asked > m_stops_seen) {
            m_stops_seen = asked;
            m_last_stop  = now;
            stopped      = true;
        }
        Reason end = Reason::None;
        if (m_end == Reason::None) {
            if (m_config.cancelled && m_config.cancelled())
                end = Reason::Cancelled;
            else if (m_config.budget.count() > 0 && now - m_start >= m_config.budget)
                end = Reason::TimedOut;
        }
        if (end != Reason::None) {
            m_end    = end;
            m_end_at = now;
        }
        if (!stopped && end == Reason::None)
            return;
        m_last_interrupt = {};
        m_scope->mark_stopped();
        ++m_applying;
        lock.unlock();
        if (m_config.stop_calls)
            m_config.stop_calls();
        if (m_scope->owns_ui()) {
            // A stopped run opens nothing more, and what it has open closes now, which ends a modal loop it waits in. A
            // stopped session keeps its modeless windows and only leaves the modal loops.
            if (m_config.stop_closes_ui) {
                m_scope->mark_ui_closed();
                PluginHostUi::close_windows_for_plugin(m_scope->ui_owner());
            } else
                PluginHostUi::end_run_modals(m_scope->ui_owner());
        }
        if (end != Reason::None)
            m_session.end();
        lock.lock();
        --m_applying;
        m_cv.notify_all();
    }

    void run()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!m_cv.wait_for(lock, std::chrono::milliseconds(50), [this] { return m_stop; })) {
            m_session.set_has_threads(m_scope->has_live_work());
            take_stops(lock);
            const auto now = std::chrono::steady_clock::now();
            if (reason_locked() == Reason::None || now - m_last_interrupt < std::chrono::seconds(1))
                continue;
            lock.unlock();
            // The run holds an interpreter lease until this watcher is stopped, so the interpreter is up.
            const PyGILState_STATE gil   = PyGILState_Ensure();
            const Phase            phase = m_phase.load();
            bool                   reaches;
            {
                std::lock_guard<std::mutex> again(m_mutex);
                reaches = m_end != Reason::None ||
                          (m_stops_seen > m_floor && (phase != Phase::Idle || now - m_last_stop < kStopWindow));
            }
            bool sent = false;
            if (reaches && (phase == Phase::Body || phase == Phase::Exit)) {
                PyThreadState_SetAsyncExc(m_ident, m_interrupt);
                sent = true;
            }
            if (reaches && phase != Phase::Drain)
                m_scope->for_each_interrupt_target([this, &sent](unsigned long ident) {
                    PyThreadState_SetAsyncExc(ident, m_interrupt);
                    sent = true;
                });
            PyGILState_Release(gil);
            lock.lock();
            // Nothing sent (host bookkeeping, no target yet): tried again at the next poll.
            if (sent)
                m_last_interrupt = now;
        }
    }

    const Config                          m_config;
    PythonSession&                        m_session;
    const unsigned long                   m_ident;
    PyObject*                             m_interrupt;
    std::shared_ptr<PythonRunScope>       m_scope;
    const std::chrono::steady_clock::time_point m_start;
    mutable std::mutex                    m_mutex;
    std::condition_variable               m_cv;
    bool                                  m_stop       = false;
    // How many stops are having their tool calls and modal windows dealt with, outside the lock (take_stops()): the loop
    // and settle() can each be applying one.
    int                                   m_applying   = 0;
    std::uint64_t                         m_stops_seen = 0;
    std::uint64_t                         m_floor      = 0;
    std::chrono::steady_clock::time_point m_last_stop{};
    Reason                                m_end = Reason::None;
    std::chrono::steady_clock::time_point m_end_at{};
    std::chrono::steady_clock::time_point m_last_interrupt{};
    std::atomic<Phase>                    m_phase{Phase::Body}; // set and read under the GIL
    std::thread                           m_thread;
};

std::string format_traceback(py::error_already_set& err)
{
    try {
        py::object lines = py::module_::import("traceback").attr("format_exception")(err.type(), err.value(), err.trace());
        return py::str("").attr("join")(lines).cast<std::string>();
    } catch (const std::exception&) {
        return err.what();
    }
}

std::string exception_line(py::error_already_set& err)
{
    try {
        py::object lines = py::module_::import("traceback").attr("format_exception_only")(err.type(), err.value());
        std::string text = py::str("").attr("join")(lines).cast<std::string>();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        return text;
    } catch (const std::exception&) {
        return err.what();
    }
}

py::object trace_of(py::error_already_set& err) { return err.trace() ? py::object(err.trace()) : py::object(py::none()); }

// A session's traceback, quoting `source` for its file's frames; today's format when that can't be made.
std::string quoted_traceback(const Capture& capture, py::error_already_set& err, const std::string& filename, const std::string& source)
{
    try {
        return py::handle(capture.format_quoted)(err.type(), err.value(), trace_of(err), filename, source).cast<std::string>();
    } catch (const std::exception&) {
        return format_traceback(err);
    }
}

int error_line_of(const Capture& capture, py::error_already_set& err, const std::string& filename)
{
    try {
        return py::handle(capture.error_line)(err.value(), trace_of(err), filename).cast<int>();
    } catch (const std::exception&) {
        return 0;
    }
}

// The names a person calls the session's variables: not starting with '_', not `args`, not modules. Reads the dict
// without running Python code. GIL held.
int count_variables(const py::dict& ns)
{
    int        count = 0;
    PyObject*  key   = nullptr;
    PyObject*  value = nullptr;
    Py_ssize_t pos   = 0;
    while (PyDict_Next(ns.ptr(), &pos, &key, &value)) {
        if (!PyUnicode_Check(key))
            continue;
        Py_ssize_t  size = 0;
        const char* name = PyUnicode_AsUTF8AndSize(key, &size);
        if (name == nullptr) {
            PyErr_Clear();
            continue;
        }
        const std::string_view text(name, static_cast<std::size_t>(size));
        if (text.empty() || text.front() == '_' || text == "args" || PyModule_Check(value))
            continue;
        ++count;
    }
    return count;
}

PythonSessionEvent finished_event(std::string status, std::string message = {})
{
    PythonSessionEvent event;
    event.kind    = PythonSessionEvent::Kind::Finished;
    event.status  = std::move(status);
    event.message = std::move(message);
    return event;
}

// The status of a work an interrupt ended, from what reached it. With `ran_on`, a work that finished while a stop
// reached what it ran on into (its threads, or the session's teardown, whose finalizer swallowed the interrupt, as
// Python ignores what __del__ raises) did not finish in time either.
void resolve_interrupted(PythonSessionEvent& done, Watcher::Reason reason, bool ran_on, std::chrono::milliseconds budget)
{
    if (ran_on && done.status == "finished" && reason != Watcher::Reason::None)
        done.status = "interrupted";
    if (done.status != "interrupted")
        return;
    if (reason == Watcher::Reason::None) {
        // The code raised the interrupt itself.
        done.status  = "raised";
        done.message = "RunInterrupted";
        return;
    }
    done.result = nullptr;
    if (reason == Watcher::Reason::TimedOut) {
        done.status  = "timed_out";
        done.message = "The script ran longer than its " + std::to_string(budget.count() / 1000) + " s budget";
    } else {
        done.status  = "cancelled";
        done.message = reason == Watcher::Reason::Cancelled ? "The script was cancelled" : "The script was stopped";
    }
}

PythonSessionEvent output_event(std::string text)
{
    PythonSessionEvent event;
    event.kind = PythonSessionEvent::Kind::Output;
    event.text = std::move(text);
    return event;
}

} // namespace

void host_python_session(const PythonSessionRequest& request, PythonSession& session)
{
    struct Returned
    {
        PythonSession& session;
        ~Returned() { session.host_returned(); }
    } returned{session};

    // Nothing is set up before the first work: a session ended or cancelled before it leaves nothing behind.
    std::uint64_t             floor = 0;
    std::optional<PythonWork> work;
    while (!(work = session.take_work(std::chrono::milliseconds(50), floor)))
        if (session.end_requested() || (request.cancelled && request.cancelled()))
            return;
    // The first work ends without running, and the session with it.
    const auto refuse = [&session](const char* status, std::string message) {
        PythonSessionEvent done = finished_event(status, std::move(message));
        done.executed           = false;
        session.push_event(std::move(done));
    };
    if (!PythonInterpreter::instance().is_initialized()) {
        refuse("unavailable", "Python is not available in this OrcaSlicer");
        return;
    }
    PythonRuntimeLease lease = PythonInterpreter::instance().acquire_runtime_lease();
    if (!lease) {
        refuse("unavailable", "Python is shutting down");
        return;
    }

    const boost::filesystem::path file  = run_file(request.filename);
    auto                          scope = std::make_shared<PythonRunScope>(g_next_run_id++, request.identity, file, current_tool_bridge());
    ScopedPythonRun               bound(scope, PythonRunThread::Main);
    const std::uint64_t           run_id = scope->id();
    if (session.end_requested())
        scope->mark_ending();

    py::gil_scoped_acquire gil;
    const unsigned long    ident   = PyThread_get_thread_ident();
    const Capture*         capture = nullptr;
    try {
        capture = &capture_functions();
    } catch (py::error_already_set& err) {
        refuse("unavailable", std::string("Cannot prepare the Python run: ") + err.what());
        return;
    }
    py::handle interrupt(capture->interrupt);

    // A file session's PEP 723 dependencies are installed before its first work, as a plugin's are; a block that can't
    // be read or an install that fails ends the session there. The install runs without the GIL, bounded by uv's own
    // limit.
    if (!file.empty()) {
        std::vector<std::string> packages;
        try {
            packages = py::handle(capture->dependencies)(work->source).cast<std::vector<std::string>>();
        } catch (py::error_already_set& err) {
            refuse("raised", "Cannot read the script's PEP 723 block: " + exception_line(err));
            return;
        }
        if (!packages.empty()) {
            std::string error;
            bool        installed = false;
            {
                py::gil_scoped_release release;
                installed = install_dependencies(packages, error);
            }
            if (!installed) {
                refuse("raised", "Cannot install the script's dependencies: " + error);
                return;
            }
        }
    }

    // Everything below that can run the script's Python (its works, a result's or exception's own methods, finalizers of
    // what its namespace and tracebacks hold) runs inside the script's audit scope and under the watcher, so none of it
    // escapes the identity, the budget, a stop or a cancel.
    std::string                       notes;    // what follows the captured output when it is not streamed
    std::optional<PythonSessionEvent> deferred; // a work's Finished event when the session's end was asked before it
    Watcher::Reason                   deferred_reason = Watcher::Reason::None;
    Watcher::Reason                   reason          = Watcher::Reason::None;
    {
        ScopedPluginAuditContext audit(request.identity);
        Watcher watcher(Watcher::Config{request.cancelled, request.budget, request.stop_calls, request.stop_closes_ui}, session, ident,
                        interrupt.ptr(), scope);
        py::dict                             ns;
        bool                                 entered = false;
        py::object                           main_thread;
        py::object                           builtins = py::module_::import("builtins");
        // A deferred work's exception, which keeps its frames until the teardown drops it, as a run's does.
        std::optional<py::error_already_set> held_failure;
        // Host bookkeeping that an interrupt must not cut in half (a folder's use count, what its teardown takes, the
        // monitoring id) runs in the Drain phase, with an interrupt already sent dropped. The caller sets the phase that
        // follows.
        const auto uninterrupted = [&](const auto& step) {
            watcher.set_phase(Watcher::Phase::Drain);
            PyThreadState_SetAsyncExc(ident, nullptr);
            PyErr_Clear();
            step();
        };
        const auto quietly = [](const auto& step) {
            try {
                step();
            } catch (py::error_already_set&) {
                // Interrupted, or the script's own cleanup failed: what is left of it is dropped.
            }
        };
        // A step of the script's cleanup, resumed after the run's interrupt until it ends: it takes each piece off what
        // it works through as the piece starts (an atexit function) or once it is done (a pool, a module's globals), so
        // a resume repeats nothing that ran and an interrupt cuts at most the piece it lands in. Any other error ends
        // the step, as quietly() does.
        const auto to_end = [&](const auto& step) {
            for (;;) {
                try {
                    step();
                    return;
                } catch (py::error_already_set& err) {
                    if (!err.matches(interrupt))
                        return;
                }
            }
        };

        // The session's own __main__ and output; a file session also gets __file__, sys.argv and imports from its
        // folder. A teardown an earlier run from this folder left pending runs before the first work.
        bool ready = false;
        try {
            py::object taken;
            uninterrupted([&] {
                main_thread = py::module_::import("threading").attr("current_thread")();
                if (request.stream_output) {
                    PythonSession* target = &session;
                    py::handle(capture->begin)(run_id, py::cpp_function([target](const std::string& text) { target->push_event(output_event(text)); }),
                                               py::cpp_function([target](const std::string& text) {
                                                   PythonSessionEvent event = output_event(text);
                                                   event.kind               = PythonSessionEvent::Kind::Result;
                                                   target->push_event(std::move(event));
                                               }));
                } else
                    py::handle(capture->begin)(run_id);
                const py::tuple entry = py::handle(capture->enter)(run_id, file.empty() ? py::object(py::none()) : py::str(request.filename))
                                            .cast<py::tuple>();
                ns      = entry[0].cast<py::dict>();
                taken   = entry[1];
                entered = true;
            });
            watcher.set_phase(Watcher::Phase::Body);
            if (taken && !taken.is_none())
                to_end([&] { py::handle(capture->teardown)(taken); });
            taken              = py::object();
            ns["__builtins__"] = builtins;
            ns["args"]         = json_to_py(request.args);
            ready              = true;
        } catch (py::error_already_set& err) {
            deferred            = finished_event("raised", exception_line(err));
            deferred->traceback = format_traceback(err);
            deferred->executed  = false;
        } catch (const std::exception& e) {
            deferred           = finished_event("raised", e.what());
            deferred->executed = false;
        }

        // One work, up to its Finished event, whose status may still be "interrupted" for resolve_interrupted().
        const auto run_one = [&](const PythonWork& w) {
            PythonSessionEvent done    = finished_event("");
            const bool         is_file = w.kind == PythonWork::Kind::File;
            watcher.set_phase(Watcher::Phase::Drain);
            {
                py::gil_scoped_release release;
                watcher.settle();
                if (request.work_started)
                    request.work_started();
            }
            std::optional<py::error_already_set> failure;
            bool                                 started    = false;
            bool                                 completed  = false;
            bool                                 was_daemon = false;
            bool                                 claimed    = false;
            auto                                 cells_sent = std::make_shared<std::atomic<bool>>(false);
            py::object                           code;
            try {
                // A work is the session's main program, which `python file.py` runs on a non-daemon thread, so the
                // threads it starts are waited for unless they say otherwise. The app hosts it on a thread Python didn't
                // start, which Python sees as a daemon dummy thread, so the work runs with that flag cleared.
                // Thread.daemon refuses a started thread, hence _daemonic.
                uninterrupted([&] {
                    watcher.begin_work(floor);
                    if (session.end_requested())
                        scope->mark_ending();
                    if (request.stream_output)
                        py::handle(capture->renew)(run_id);
                    was_daemon = main_thread.attr("daemon").cast<bool>();
                    if (was_daemon)
                        main_thread.attr("_daemonic") = false;
                });
                watcher.set_phase(Watcher::Phase::Body);
                py::object expr;
                if (is_file) {
                    // Text: a BOM a file's text kept is dropped, as Python drops it from a file.
                    const bool    bom  = w.code.compare(0, 3, "\xEF\xBB\xBF") == 0;
                    const py::str text = py::str(bom ? w.code.substr(3) : w.code);
                    if (request.cell_results) {
                        const py::object tree = builtins.attr("compile")(text, request.filename, "exec",
                                                                         py::module_::import("ast").attr("PyCF_ONLY_AST"), true);
                        py::handle(capture->mark_cell_results)(tree, py::cast(w.cell_starts), kShowResultName);
                        code = builtins.attr("compile")(tree, request.filename, "exec", 0, true);
                        // Bound again for each work, so one that deleted it doesn't take it from the next.
                        ns[kShowResultName] = py::handle(capture->show_result);
                    } else
                        code = builtins.attr("compile")(text, request.filename, "exec");
                } else {
                    // A Cell compiles at its own lines of the file; a trailing bare expression is split off and
                    // evaluated, as the REPL shows one. Only builtins compile it, so a SyntaxError's traceback holds no
                    // frame of the host's.
                    const std::string filename = w.kind == PythonWork::Kind::Console ? std::string("<console>") : request.filename;
                    const std::string text     = w.kind == PythonWork::Kind::Cell ? std::string(std::max(w.first_line - 1, 0), '\n') + w.code
                                                                                 : w.code;
                    const py::module_ ast      = py::module_::import("ast");
                    const py::object  tree     = builtins.attr("compile")(py::str(text), filename, "exec", ast.attr("PyCF_ONLY_AST"), true);
                    py::list          body     = tree.attr("body");
                    if (py::len(body) > 0 && py::isinstance(body[py::len(body) - 1], ast.attr("Expr")))
                        expr = builtins.attr("compile")(ast.attr("Expression")(body.attr("pop")().attr("value")), filename, "eval", 0, true);
                    code = builtins.attr("compile")(tree, filename, "exec", 0, true);
                }
                started = true;
                if (is_file && request.report_cells) {
                    uninterrupted([&] {
                        PythonSession* target = &session;
                        claimed = py::handle(capture->claim_cells)(code, py::cast(w.cell_starts), py::cpp_function([target, cells_sent](int cell) {
                                      PythonSessionEvent event;
                                      event.kind = PythonSessionEvent::Kind::Cell;
                                      event.cell = cell;
                                      cells_sent->store(true);
                                      target->push_event(std::move(event));
                                  })).cast<bool>();
                    });
                    watcher.set_phase(Watcher::Phase::Body);
                }
                const ResultTarget       result_target{&ns, run_id, capture};
                const ScopedResultTarget showing(result_target);
                builtins.attr("exec")(code, ns);
                if (expr)
                    show_result(builtins.attr("eval")(expr, ns), result_target);
                completed = true;
            } catch (py::error_already_set& err) {
                failure.emplace(std::move(err));
            } catch (const std::exception& e) {
                // Never past the watcher while it may wait for the GIL this thread holds.
                done.status  = "raised";
                done.message = e.what();
            }
            if (claimed) {
                quietly([&] { uninterrupted([&] { py::handle(capture->release_cells)(code); }); });
                watcher.set_phase(Watcher::Phase::Body);
            }
            if (was_daemon && PyObject_SetAttrString(main_thread.ptr(), "_daemonic", Py_True) != 0)
                PyErr_Clear();

            try {
                if (failure && failure->matches(PyExc_SystemExit)) {
                    py::object exit_code = failure->value().attr("code");
                    if (exit_code.is_none() || (py::isinstance<py::int_>(exit_code) && exit_code.cast<long long>() == 0)) {
                        failure.reset();
                        completed = true;
                    }
                }
                if (failure && !failure->matches(interrupt)) {
                    done.status     = "raised";
                    done.message    = exception_line(*failure);
                    done.traceback  = request.quote_source ? quoted_traceback(*capture, *failure, request.filename, w.source) : format_traceback(*failure);
                    done.error_line = error_line_of(*capture, *failure, request.filename);
                } else if (!failure && completed && is_file && ns.contains("result")) {
                    try {
                        done.result = strict_json(ns["result"]);
                        done.status = "finished";
                    } catch (const std::invalid_argument& e) {
                        if (file.empty()) {
                            done.status  = "raised";
                            done.message = e.what();
                        } else {
                            // A standalone script may well have a variable named result: it is no result, not a failure.
                            done.status      = "finished";
                            std::string note = std::string("\n[result is not JSON, so it was not returned: ") + e.what() + "]\n";
                            if (request.stream_output)
                                session.push_event(output_event(std::move(note)));
                            else
                                notes += note;
                        }
                    }
                } else if (!failure && completed)
                    done.status = "finished";
            } catch (py::error_already_set& err) {
                // The result's own code raised, or the work was interrupted while it ran.
                if (!err.matches(interrupt)) {
                    done.status  = "raised";
                    done.message = exception_line(err);
                } else
                    failure.emplace(std::move(err));
            }
            if (done.status.empty())
                done.status = "interrupted"; // decided by resolve_interrupted() from what reached it
            held_failure.reset();
            if (failure && session.end_requested())
                held_failure.emplace(std::move(*failure));
            failure.reset();

            // A File work in a session that goes on reports once the waited work it started is done, as `python file.py`
            // ends; a stop interrupts that work and gives it kStopWindow. When the session ends, its teardown waits.
            if (is_file && !session.end_requested()) {
                watcher.set_phase(Watcher::Phase::Threads);
                py::gil_scoped_release release;
                while (!scope->quiet() && !session.end_requested()) {
                    if (const auto since = watcher.since_stop(); since && *since >= kStopWindow)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
            // An interrupt sent as the work returned would land in the next Python this thread runs. What the session's
            // threads print from here until the next work is a stretch of output of its own.
            uninterrupted([&] {
                if (request.stream_output)
                    py::handle(capture->renew)(run_id);
            });
            {
                py::gil_scoped_release release;
                if (request.work_ended)
                    request.work_ended();
            }
            // Before the Finished event, so a stop() right after it reaches the threads the work left.
            session.set_has_threads(scope->has_live_work());
            done.executed       = started;
            done.cells_reported = cells_sent->load();
            done.variables      = count_variables(ns);
            return done;
        };

        // Between works the hosting thread waits for the next one without the GIL; the session's threads run on.
        const auto next_work = [&]() -> std::optional<PythonWork> {
            uninterrupted([] {});
            watcher.set_phase(Watcher::Phase::Idle);
            py::gil_scoped_release release;
            for (;;) {
                if (session.end_requested())
                    return std::nullopt;
                if (std::optional<PythonWork> next = session.take_work(std::chrono::milliseconds(50), floor))
                    return next;
            }
        };

        while (ready && work) {
            PythonSessionEvent done = run_one(*work);
            if (session.end_requested()) {
                // Its Finished event comes after the teardown and reflects it.
                deferred_reason = watcher.reason();
                deferred        = std::move(done);
                break;
            }
            resolve_interrupted(done, watcher.reason(), work->kind == PythonWork::Kind::File, request.budget);
            session.work_done();
            session.push_event(std::move(done));
            work = next_work();
        }

        // Like `python file.py`, the session ends once the work it started is done: its non-daemon threads, its window
        // callbacks and the pool tasks it submitted. It waits here, without the GIL, on what it counted, never through
        // Thread.join(), whose interrupted wait marks a live thread stopped. A stop interrupts that work and gives it
        // kStopWindow; then the session stops waiting and closes its admission anyway.
        scope->mark_ending();
        uninterrupted([&] { watcher.begin_teardown(); });
        watcher.set_phase(Watcher::Phase::Threads);
        {
            py::gil_scoped_release release;
            while (!scope->close_admission_if_quiet()) {
                if (const auto since = watcher.since_stop(); since && *since >= kStopWindow) {
                    scope->close_admission();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        // An interrupt sent as the wait ended would land in the next Python this thread runs.
        PyThreadState_SetAsyncExc(ident, nullptr);
        PyErr_Clear();

        // What the script left behind goes here, under its identity and the watcher, with its prints captured: its
        // pools, its atexit functions, its namespace, the exception with the frames its traceback holds, its folder's
        // teardown when it is the folder's last run, and what it left in its argv or in place of __main__. What each
        // step works through is taken as host bookkeeping, where no interrupt reaches, and the step that runs it is
        // resumed after an interrupt.
        watcher.set_phase(Watcher::Phase::Exit);
        to_end([&] { py::handle(capture->shutdown_own_pools)(run_id); });
        py::object exits;
        quietly([&] { uninterrupted([&] { exits = py::handle(capture->take_exits)(run_id); }); });
        watcher.set_phase(Watcher::Phase::Exit);
        if (exits)
            to_end([&] { py::handle(capture->call_exits)(exits); });
        exits = py::object();
        // A script's windows close with it, after its atexit functions and before its globals go, waiting for the UI
        // thread, so none of its callbacks runs after.
        if (scope->owns_ui()) {
            scope->mark_ui_closed();
            PluginHostUi::close_run_windows(scope->ui_owner());
        }
        ns.clear();
        held_failure.reset();
        if (entered) {
            py::object taken;
            quietly([&] { uninterrupted([&] { taken = py::handle(capture->release)(run_id); }); });
            watcher.set_phase(Watcher::Phase::Exit);
            if (taken && !taken.is_none())
                to_end([&] { py::handle(capture->teardown)(taken); });
            taken = py::object();
            py::object left;
            quietly([&] { uninterrupted([&] { left = py::handle(capture->leave)(run_id); }); });
            watcher.set_phase(Watcher::Phase::Exit);
            // What the run's routes held finalizes here, where its finalizers can still be stopped.
            left = py::object();
        }

        // Host bookkeeping, which no interrupt reaches: the output and the tools.
        watcher.set_phase(Watcher::Phase::Drain);
        PyThreadState_SetAsyncExc(ident, nullptr);
        PyErr_Clear();
        try {
            std::string text = py::handle(capture->end)(run_id).cast<std::string>() + notes;
            if (!text.empty())
                session.push_event(output_event(std::move(text)));
        } catch (const std::exception&) {
        }
        {
            py::gil_scoped_release release;
            scope->close_tools();
        }
        {
            py::gil_scoped_release release;
            watcher.stop();
        }
        reason = watcher.reason();
        PyThreadState_SetAsyncExc(ident, nullptr);
        PyErr_Clear();
    }
    if (deferred) {
        resolve_interrupted(*deferred, reason != Watcher::Reason::None ? reason : deferred_reason, true, request.budget);
        session.push_event(std::move(*deferred));
    }
}

PythonRunOutcome run_python_code(const PythonRunRequest& request)
{
    // A session with one File work and its end asked up front, so its Finished event comes after the teardown.
    PythonSession session;
    PythonWork    work;
    work.kind        = PythonWork::Kind::File;
    work.code        = request.code;
    work.source      = request.code;
    work.cell_starts = {1};
    session.submit(std::move(work));
    session.end();

    PythonSessionRequest hosted;
    hosted.identity       = request.identity;
    hosted.filename       = request.filename;
    hosted.args           = request.args;
    hosted.budget         = request.budget;
    hosted.cancelled      = request.cancelled;
    hosted.stop_calls     = request.stop_calls;
    hosted.stop_closes_ui = true;
    hosted.stream_output  = false;
    hosted.quote_source   = false;
    hosted.report_cells   = false;
    hosted.cell_results   = false;
    host_python_session(hosted, session);

    PythonRunOutcome outcome;
    for (PythonSessionEvent& event : session.take_events()) {
        if (event.kind == PythonSessionEvent::Kind::Output)
            outcome.output += event.text;
        else if (event.kind == PythonSessionEvent::Kind::Finished) {
            outcome.status    = std::move(event.status);
            outcome.result    = std::move(event.result);
            outcome.traceback = std::move(event.traceback);
            outcome.message   = std::move(event.message);
        }
    }
    return outcome;
}

namespace {

// Whether `bytes` is valid UTF-8, the fallback check when Python is not there to decode. It is as strict as
// Python's codec (no overlong forms, surrogates or code points above U+10FFFF), because nlohmann::json's
// serializer validates every string it writes that way.
bool valid_utf8(const std::string& bytes)
{
    try {
        (void) nlohmann::json(bytes).dump();
        return true;
    } catch (const nlohmann::json::type_error&) {
        return false;
    }
}

} // namespace

namespace {

// The threading.Thread whose bootstrap `function`, given to _thread.start_new_thread, is; None for a raw thread.
py::object thread_of(const py::handle& function)
{
    py::object thread = py::getattr(function, "__self__", py::none());
    return py::isinstance(thread, py::module_::import("threading").attr("Thread")) ? thread : py::object(py::none());
}

bool is_pool_worker_thread(const py::object& thread)
{
    if (thread.is_none())
        return false;
    const py::object pools = py::module_::import("sys").attr("modules").attr("get")("concurrent.futures.thread");
    return !pools.is_none() && py::getattr(thread, "_target", py::none()).is(pools.attr("_worker"));
}

} // namespace

void set_python_dependency_installer_for_testing(PythonDependencyInstaller installer)
{
    std::lock_guard<std::mutex> lock(g_installer_mutex);
    g_installer = std::move(installer);
}

bool python_run_is_pool_worker(const py::handle& function) { return is_pool_worker_thread(thread_of(function)); }

bool python_run_waits_for(const py::handle& function)
{
    const py::object thread = thread_of(function);
    return !thread.is_none() && !thread.attr("daemon").cast<bool>() && !is_pool_worker_thread(thread);
}

PoolInitializerBinding python_run_bind_pool_initializer(const py::handle& function, const std::shared_ptr<PythonRunScope>& run)
{
    // The worker's target is _worker(executor_reference, work_queue, initializer, initargs).
    const py::object thread = thread_of(function);
    if (!is_pool_worker_thread(thread))
        return PoolInitializerBinding::None;
    const py::object args = py::getattr(thread, "_args", py::none());
    if (!py::isinstance<py::tuple>(args) || py::len(args) < 4)
        return PoolInitializerBinding::None;
    py::list         worker_args(args);
    const py::object initializer = worker_args[2];
    if (initializer.is_none())
        return PoolInitializerBinding::None;
    if (!run->task_submitted())
        return PoolInitializerBinding::Refused;
    try {
        worker_args[2] = py::cpp_function([initializer, run](const py::args& call_args) -> py::object {
            ScopedPythonRun          bound(run, PythonRunThread::Started);
            ScopedPluginAuditContext audit(run->identity());
            struct Target
            {
                PythonRunScope& run;
                unsigned long   ident;
                ~Target()
                {
                    run.task_finished(ident);
                    // An interrupt sent as the initializer returned would land in the worker's next task.
                    PyThreadState_SetAsyncExc(ident, nullptr);
                    run.task_done();
                }
            } target{*run, PyThread_get_thread_ident()};
            run->task_running(target.ident);
            PyObject* result = PyObject_Call(initializer.ptr(), call_args.ptr(), nullptr);
            if (result == nullptr)
                throw py::error_already_set();
            return py::reinterpret_steal<py::object>(result);
        });
        py::setattr(thread, "_args", py::tuple(worker_args));
    } catch (...) {
        run->task_done();
        throw;
    }
    return PoolInitializerBinding::Counted;
}

bool decode_python_source(const std::string& bytes, std::string& text, std::string& encoding, std::string& error)
{
    PythonRuntimeLease lease;
    if (PythonInterpreter::instance().is_initialized())
        lease = PythonInterpreter::instance().acquire_runtime_lease();
    if (!lease) {
        // Without Python only UTF-8 is read, as most files are.
        const bool bom = bytes.compare(0, 3, "\xEF\xBB\xBF") == 0;
        if (!valid_utf8(bom ? bytes.substr(3) : bytes)) {
            error = "the file is not UTF-8 text";
            return false;
        }
        text     = bom ? bytes.substr(3) : bytes;
        encoding = bom ? "utf-8-sig" : "utf-8";
        return true;
    }
    py::gil_scoped_acquire gil;
    try {
        py::bytes  raw(bytes);
        py::object readline = py::module_::import("io").attr("BytesIO")(raw).attr("readline");
        encoding            = py::module_::import("tokenize").attr("detect_encoding")(readline).cast<py::tuple>()[0].cast<std::string>();
        text                = raw.attr("decode")(encoding).cast<std::string>();
        return true;
    } catch (py::error_already_set& e) {
        error = e.what();
        return false;
    }
}

bool encode_python_source(const std::string& text, const std::string& encoding, std::string& bytes, std::string& error)
{
    PythonRuntimeLease lease;
    if (PythonInterpreter::instance().is_initialized())
        lease = PythonInterpreter::instance().acquire_runtime_lease();
    if (!lease) {
        if (encoding != "utf-8" && encoding != "utf-8-sig") {
            error = "Python is not available to encode the file as " + encoding;
            return false;
        }
        bytes = (encoding == "utf-8-sig" ? "\xEF\xBB\xBF" : "") + text;
        return true;
    }
    py::gil_scoped_acquire gil;
    try {
        bytes = py::str(text).attr("encode")(encoding).cast<std::string>();
        return true;
    } catch (py::error_already_set& e) {
        error = e.what();
        return false;
    }
}

} // namespace Slic3r
