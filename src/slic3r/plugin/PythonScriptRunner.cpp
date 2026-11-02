#include "PythonScriptRunner.hpp"

#include "PluginAuditManager.hpp"
#include "PluginJsonPy.hpp"
#include "PythonInterpreter.hpp"

#include <pybind11/embed.h>
#include <pybind11/eval.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace Slic3r {

namespace py = pybind11;
using nlohmann::json;

namespace {

// The module that captures a run's output and holds the exception that interrupts it. Installed once,
// over whatever sys.stdout and sys.stderr are then (the stderr tee to the Python log): writes from a
// thread that has a capture go to it, all others pass through.
constexpr const char* kCaptureModule = "_orca_run_capture";
constexpr const char* kCaptureSource = R"CAPTURE(
import sys, threading

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
        capture = _captures.get(threading.get_ident())
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

def begin(ident):
    _captures[ident] = _Capture()

def end(ident):
    capture = _captures.pop(ident, None)
    if capture is None:
        return ""
    text = "".join(capture.parts)
    if capture.truncated:
        text += "\n[output truncated at 256 KiB]\n"
    return text

sys.stdout = _RunStream(sys.stdout)
sys.stderr = _RunStream(sys.stderr)
)CAPTURE";

// The capture module's functions and interrupt, installed on first use and held from then on, so a script
// that rebinds the module's names or replaces it in sys.modules changes nothing the runner calls. The
// references are kept for the process: dropping them after the interpreter finalizes would crash. GIL held.
struct Capture
{
    PyObject* begin     = nullptr;
    PyObject* end       = nullptr;
    PyObject* interrupt = nullptr;
};

const Capture& capture_functions()
{
    static Capture capture;
    if (capture.begin != nullptr)
        return capture;
    py::module_ module = py::reinterpret_steal<py::module_>(PyModule_New(kCaptureModule));
    if (!module)
        throw py::error_already_set();
    py::exec(kCaptureSource, module.attr("__dict__"));
    py::module_::import("sys").attr("modules")[kCaptureModule] = module;
    capture.begin     = py::object(module.attr("begin")).release().ptr();
    capture.end       = py::object(module.attr("end")).release().ptr();
    capture.interrupt = py::object(module.attr("RunInterrupted")).release().ptr();
    return capture;
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

// Interrupts the run's thread when it is cancelled or out of time, until stopped.
class Watcher
{
public:
    enum class Reason { None, Cancelled, TimedOut };

    Watcher(const PythonRunRequest& request, unsigned long thread_ident, PyObject* interrupt)
        : m_request(request), m_ident(thread_ident), m_interrupt(interrupt), m_thread([this] { run(); })
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
            else if (std::chrono::steady_clock::now() - start >= m_request.budget)
                why = Reason::TimedOut;
            const auto now = std::chrono::steady_clock::now();
            if (why == Reason::None || now - last < std::chrono::seconds(1))
                continue;
            // The first reason sticks: a run cancelled after its budget ran out still timed out.
            Reason expected = Reason::None;
            m_reason.compare_exchange_strong(expected, why);
            last = now;
            lock.unlock();
            // The run holds an interpreter lease until this watcher is stopped, so the interpreter is up.
            const PyGILState_STATE gil = PyGILState_Ensure();
            PyThreadState_SetAsyncExc(m_ident, m_interrupt);
            PyGILState_Release(gil);
            lock.lock();
        }
    }

    const PythonRunRequest& m_request;
    const unsigned long     m_ident;
    PyObject*               m_interrupt;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    bool                    m_stop = false;
    std::atomic<Reason>     m_reason{Reason::None};
    std::thread             m_thread;
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

    // Everything below that can run the script's Python (its body, a result's or exception's own methods,
    // finalizers of what its namespace and traceback hold) runs inside the script's audit scope and under the
    // watcher, so none of it escapes the identity, the budget or a cancel.
    Watcher::Reason reason = Watcher::Reason::None;
    {
        ScopedPluginAuditContext audit(request.identity);
        Watcher                  watcher(request, ident, interrupt.ptr());
        std::optional<py::error_already_set> failure;
        py::dict                             ns;
        bool                                 executed = false;
        try {
            py::handle(capture->begin)(ident);
            py::module_ builtins = py::module_::import("builtins");
            ns["__name__"]       = "__orca_script__";
            ns["__builtins__"]   = builtins;
            ns["args"]           = json_to_py(request.args);
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
                    outcome.status  = "raised";
                    outcome.message = e.what();
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

        try {
            outcome.output = py::handle(capture->end)(ident).cast<std::string>();
        } catch (const std::exception&) {
        }
        // What the script left behind goes here, under its identity: its namespace, and the exception with
        // the frames its traceback holds.
        ns.clear();
        failure.reset();
        {
            py::gil_scoped_release release;
            watcher.stop();
        }
        reason = watcher.reason();
        // An interrupt raised as the run was ending would land in the next Python this thread runs.
        PyThreadState_SetAsyncExc(ident, nullptr);
        PyErr_Clear();
    }

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
