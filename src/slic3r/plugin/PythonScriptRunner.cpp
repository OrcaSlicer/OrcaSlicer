#include "PythonScriptRunner.hpp"

#include "PluginAuditManager.hpp"
#include "PluginJsonPy.hpp"
#include "PluginLoader.hpp"
#include "PythonInterpreter.hpp"
#include "PythonRunScope.hpp"
#include "host/PluginHostTools.hpp"
#include "host/PluginHostUi.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/system/error_code.hpp>

#include <pybind11/attr.h>
#include <pybind11/embed.h>
#include <pybind11/eval.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/stl.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
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

class _Capture:
    __slots__ = ("parts", "size", "truncated")
    LIMIT = 256 * 1024
    def __init__(self):
        self.parts = []
        self.size = 0
        self.truncated = False

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
        if not capture.truncated:
            room = _Capture.LIMIT - capture.size
            if len(s) > room:
                capture.parts.append(s[:room])
                capture.size = _Capture.LIMIT
                capture.truncated = True
            else:
                capture.parts.append(s)
                capture.size += len(s)
        return len(s)
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

def begin(run_id):
    _captures[run_id] = _Capture()

def end(run_id):
    capture = _captures.pop(run_id, None)
    if capture is None:
        return ""
    text = "".join(capture.parts)
    if capture.truncated:
        text += "\n[output truncated at 256 KiB]\n"
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
};

// A work item's tag (item._orca_run, a capsule that owns it): the run it belongs to and its task's accounting there,
// in plain C++, so nothing between taking the count and recording it, or between testing the claim and setting it, can
// raise or run Python code.
struct ItemTag
{
    std::shared_ptr<PythonRunScope> run;
    bool                            counted = false;
    bool                            claimed = false;
};

py::capsule item_tag(std::shared_ptr<PythonRunScope> run)
{
    return py::capsule(new ItemTag{std::move(run)}, [](void* tag) { delete static_cast<ItemTag*>(tag); });
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
            if (run->stopped() || run->admission_closed()) {
                // A stopped or ended run's queued task doesn't run.
                py::object future = item.attr("future");
                if (future.attr("set_running_or_notify_cancel")().cast<bool>())
                    future.attr("set_exception")(py::module_::import("builtins").attr("RuntimeError")("The script's run has ended"));
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
    py::exec(kCaptureSource, module.attr("__dict__"));
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

// A file run names an existing file; any other name (<run_python>, a bare traceback name) is code sent as text.
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

// Interrupts the run when it is cancelled or out of time, until stopped. What it interrupts depends on the run's
// phase: in Body (the code runs) and Exit (atexit functions, the namespace's finalizers, the folder's teardown) the
// run's thread and its interrupt targets; in Threads (the run's thread waits in C++ for the work it started) the
// targets only; in Drain (host bookkeeping) nothing, though it still records a stop or time-out. The phase is set,
// and read before an interrupt, under the GIL, so no interrupt lands after the phase that allowed it.
class Watcher
{
public:
    enum class Reason { None, Cancelled, TimedOut };
    enum class Phase { Body, Threads, Exit, Drain };

    Watcher(const PythonRunRequest& request, unsigned long thread_ident, PyObject* interrupt, std::shared_ptr<PythonRunScope> scope)
        : m_request(request), m_ident(thread_ident), m_interrupt(interrupt), m_scope(std::move(scope)), m_thread([this] { run(); })
    {}
    ~Watcher() { stop(); }

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
    Reason reason() const { return m_reason.load(); }
    // With the GIL held.
    void set_phase(Phase phase) { m_phase = phase; }
    // Time since the run's first stop or time-out, or nullopt before it.
    std::optional<std::chrono::steady_clock::duration> since_stop() const
    {
        const long long at = m_stopped_at.load();
        if (at < 0)
            return std::nullopt;
        return std::chrono::steady_clock::now().time_since_epoch() - std::chrono::steady_clock::duration(at);
    }

private:
    void run()
    {
        const auto start = std::chrono::steady_clock::now();
        auto       last  = std::chrono::steady_clock::time_point{};
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!m_cv.wait_for(lock, std::chrono::milliseconds(50), [this] { return m_stop; })) {
            Reason why = Reason::None;
            if (m_request.cancelled && m_request.cancelled())
                why = Reason::Cancelled;
            else if (m_request.budget.count() > 0 && std::chrono::steady_clock::now() - start >= m_request.budget)
                why = Reason::TimedOut;
            if (why == Reason::None)
                continue;
            // The first reason sticks: a run cancelled after its budget ran out still timed out.
            Reason expected = Reason::None;
            if (m_reason.compare_exchange_strong(expected, why)) {
                m_stopped_at = std::chrono::steady_clock::now().time_since_epoch().count();
                m_scope->mark_stopped();
                lock.unlock();
                if (m_request.stop_calls)
                    m_request.stop_calls();
                // A stopped script opens nothing more, and what it has open closes now, which ends a modal loop it
                // waits in.
                if (m_scope->owns_ui()) {
                    m_scope->mark_ui_closed();
                    PluginHostUi::close_windows_for_plugin(m_scope->ui_owner());
                }
                lock.lock();
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(1))
                continue;
            last = now;
            lock.unlock();
            // The run holds an interpreter lease until this watcher is stopped, so the interpreter is up.
            const PyGILState_STATE gil   = PyGILState_Ensure();
            const Phase            phase = m_phase.load();
            if (phase == Phase::Body || phase == Phase::Exit)
                PyThreadState_SetAsyncExc(m_ident, m_interrupt);
            if (phase != Phase::Drain)
                m_scope->for_each_interrupt_target([this](unsigned long ident) { PyThreadState_SetAsyncExc(ident, m_interrupt); });
            PyGILState_Release(gil);
            lock.lock();
        }
    }

    const PythonRunRequest&         m_request;
    const unsigned long             m_ident;
    PyObject*                       m_interrupt;
    std::shared_ptr<PythonRunScope> m_scope;
    std::mutex                      m_mutex;
    std::condition_variable         m_cv;
    bool                            m_stop = false;
    std::atomic<Reason>             m_reason{Reason::None};
    std::atomic<Phase>              m_phase{Phase::Body}; // set and read under the GIL
    std::atomic<long long>          m_stopped_at{-1};
    std::thread                     m_thread;
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

} // namespace

PythonRunOutcome run_python_code(const PythonRunRequest& request)
{
    PythonRunOutcome outcome;
    std::string      result_note;
    if (!PythonInterpreter::instance().is_initialized()) {
        outcome.status  = "unavailable";
        outcome.message = "Python is not available in this OrcaSlicer";
        return outcome;
    }
    PythonRuntimeLease lease = PythonInterpreter::instance().acquire_runtime_lease();
    if (!lease) {
        outcome.status  = "unavailable";
        outcome.message = "Python is shutting down";
        return outcome;
    }

    const boost::filesystem::path file  = run_file(request.filename);
    auto                          scope = std::make_shared<PythonRunScope>(g_next_run_id++, request.identity, file, current_tool_bridge());
    ScopedPythonRun               bound(scope, PythonRunThread::Main);
    const std::uint64_t           run_id = scope->id();

    py::gil_scoped_acquire gil;
    const unsigned long ident = PyThread_get_thread_ident();
    const Capture*      capture = nullptr;
    try {
        capture = &capture_functions();
    } catch (py::error_already_set& err) {
        outcome.status  = "unavailable";
        outcome.message = std::string("Cannot prepare the Python run: ") + err.what();
        return outcome;
    }
    py::handle interrupt(capture->interrupt);

    // A file run's PEP 723 dependencies are installed before its code, as a plugin's are; a block that can't be read
    // or an install that fails stops the run there. The install runs without the GIL, bounded by uv's own limit.
    if (!file.empty()) {
        std::vector<std::string> packages;
        try {
            packages = py::handle(capture->dependencies)(request.code).cast<std::vector<std::string>>();
        } catch (py::error_already_set& err) {
            outcome.status  = "raised";
            outcome.message = "Cannot read the script's PEP 723 block: " + exception_line(err);
            return outcome;
        }
        if (!packages.empty()) {
            std::string error;
            bool        installed = false;
            {
                py::gil_scoped_release release;
                installed = install_dependencies(packages, error);
            }
            if (!installed) {
                outcome.status  = "raised";
                outcome.message = "Cannot install the script's dependencies: " + error;
                return outcome;
            }
        }
    }

    // Everything below that can run the script's Python (its body, a result's or exception's own methods,
    // finalizers of what its namespace and traceback hold) runs inside the script's audit scope and under the
    // watcher, so none of it escapes the identity, the budget or a cancel.
    Watcher::Reason reason = Watcher::Reason::None;
    {
        ScopedPluginAuditContext audit(request.identity);
        Watcher                  watcher(request, ident, interrupt.ptr(), scope);
        std::optional<py::error_already_set> failure;
        py::dict                             ns;
        bool                                 executed = false;
        bool                                 entered  = false;
        // The body is the run's main program, which `python file.py` runs on a non-daemon thread, so the threads it
        // starts are waited for unless they say otherwise. The app runs it on a thread Python didn't start, which
        // Python sees as a daemon dummy thread, so the body runs with that flag cleared. Thread.daemon refuses a
        // started thread, hence _daemonic.
        py::object main_thread;
        bool       was_daemon = false;
        // Host bookkeeping that an interrupt must not cut in half (a folder's use count, what its teardown takes) runs
        // in the Drain phase, with an interrupt already sent dropped. The caller sets the phase that follows.
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
        try {
            py::object taken;
            uninterrupted([&] {
                main_thread = py::module_::import("threading").attr("current_thread")();
                was_daemon  = main_thread.attr("daemon").cast<bool>();
                if (was_daemon)
                    main_thread.attr("_daemonic") = false;
                py::handle(capture->begin)(run_id);
                // The run's own __main__: a file run also gets __file__, sys.argv and imports from its folder.
                const py::tuple entry = py::handle(capture->enter)(run_id, file.empty() ? py::object(py::none()) : py::str(request.filename))
                                            .cast<py::tuple>();
                ns      = entry[0].cast<py::dict>();
                taken   = entry[1];
                entered = true;
            });
            watcher.set_phase(Watcher::Phase::Body);
            // A teardown an earlier run from this folder left pending runs before this run's code.
            if (taken && !taken.is_none())
                to_end([&] { py::handle(capture->teardown)(taken); });
            taken                = py::object();
            py::module_ builtins = py::module_::import("builtins");
            ns["__builtins__"]   = builtins;
            ns["args"]         = json_to_py(request.args);
            // Text: a BOM a file's text kept is dropped, as Python drops it from a file.
            const bool bom  = request.code.compare(0, 3, "\xEF\xBB\xBF") == 0;
            py::object code = builtins.attr("compile")(py::str(bom ? request.code.substr(3) : request.code), request.filename, "exec");
            builtins.attr("exec")(code, ns);
            executed = true;
        } catch (py::error_already_set& err) {
            failure.emplace(std::move(err));
        } catch (const std::exception& e) {
            // Never past the watcher while it may wait for the GIL this thread holds.
            outcome.status  = "raised";
            outcome.message = e.what();
        }
        if (was_daemon && PyObject_SetAttrString(main_thread.ptr(), "_daemonic", Py_True) != 0)
            PyErr_Clear();

        try {
            if (failure && failure->matches(PyExc_SystemExit)) {
                py::object exit_code = failure->value().attr("code");
                if (exit_code.is_none() || (py::isinstance<py::int_>(exit_code) && exit_code.cast<long long>() == 0)) {
                    failure.reset();
                    executed = true;
                }
            }
            if (failure && !failure->matches(interrupt)) {
                outcome.status    = "raised";
                outcome.message   = exception_line(*failure);
                outcome.traceback = format_traceback(*failure);
            } else if (!failure && executed && ns.contains("result")) {
                try {
                    outcome.result = strict_json(ns["result"]);
                    outcome.status = "finished";
                } catch (const std::invalid_argument& e) {
                    if (file.empty()) {
                        outcome.status  = "raised";
                        outcome.message = e.what();
                    } else {
                        // A standalone script may well have a variable named result: it is no result, not a failure.
                        outcome.status = "finished";
                        result_note    = std::string("\n[result is not JSON, so it was not returned: ") + e.what() + "]\n";
                    }
                }
            } else if (!failure && executed)
                outcome.status = "finished";
        } catch (py::error_already_set& err) {
            // The result's own code raised, or the run was interrupted while it ran.
            if (!err.matches(interrupt)) {
                outcome.status  = "raised";
                outcome.message = exception_line(err);
            } else
                failure.emplace(std::move(err));
        }
        if (outcome.status.empty())
            outcome.status = "interrupted"; // decided below from the watcher's reason

        // Like `python file.py`, the run ends once the work it started is done: its non-daemon threads, its window
        // callbacks and the pool tasks it submitted. It waits here, without the GIL, on what it counted, never through
        // Thread.join(), whose interrupted wait marks a live thread stopped. A stop interrupts that work and gives it
        // 5 s; then the run stops waiting and closes its admission anyway.
        watcher.set_phase(Watcher::Phase::Threads);
        {
            py::gil_scoped_release release;
            while (!scope->close_admission_if_quiet()) {
                if (const auto since = watcher.since_stop(); since && *since >= std::chrono::seconds(5)) {
                    scope->close_admission();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        // An interrupt the body was sent as it returned would land in the next Python this thread runs.
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
        failure.reset();
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
            outcome.output = py::handle(capture->end)(run_id).cast<std::string>();
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
    outcome.output += result_note;

    // A finalizer that ran on past the budget or a cancel while the namespace was cleared swallowed the
    // interrupt, as Python ignores what __del__ raises: the run still did not finish in time.
    if (outcome.status == "finished" && reason != Watcher::Reason::None)
        outcome.status = "interrupted";
    if (outcome.status == "interrupted" && reason == Watcher::Reason::None) {
        // The code raised the interrupt itself.
        outcome.status  = "raised";
        outcome.message = "RunInterrupted";
    } else if (outcome.status == "interrupted") {
        outcome.result  = nullptr;
        outcome.status  = reason == Watcher::Reason::TimedOut ? "timed_out" : "cancelled";
        outcome.message = reason == Watcher::Reason::TimedOut
                              ? "The script ran longer than its " + std::to_string(request.budget.count() / 1000) + " s budget"
                              : "The script was cancelled";
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
