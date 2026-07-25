#include "PythonInterpreter.hpp"
#include "GeneratedConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "PluginAuditManager.hpp"
#include <boost/filesystem/path.hpp>
#include <pytypedefs.h>
#include "PluginFsUtils.hpp"

#include <pybind11/embed.h>

#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/nowide/convert.hpp>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>

namespace Slic3r {

thread_local PythonInterpreter* PythonRuntimeLease::s_owner = nullptr;
thread_local unsigned int      PythonRuntimeLease::s_depth = 0;

PythonRuntimeLease::PythonRuntimeLease(PythonInterpreter& interpreter)
{
    if (s_owner == &interpreter) {
        m_interpreter = &interpreter;
        ++s_depth;
        return;
    }

    m_lock = std::shared_lock<std::shared_mutex>(interpreter.m_runtime_mutex);
    if (!interpreter.m_initialized.load(std::memory_order_acquire)) {
        m_lock.unlock();
        return;
    }

    m_interpreter = &interpreter;
    s_owner       = &interpreter;
    s_depth       = 1;
}

PythonRuntimeLease::PythonRuntimeLease(PythonRuntimeLease&& other) noexcept
    : m_interpreter(other.m_interpreter), m_lock(std::move(other.m_lock))
{
    other.m_interpreter = nullptr;
}

PythonRuntimeLease& PythonRuntimeLease::operator=(PythonRuntimeLease&& other) noexcept
{
    if (this != &other) {
        release();
        m_lock         = std::move(other.m_lock);
        m_interpreter  = other.m_interpreter;
        other.m_interpreter = nullptr;
    }
    return *this;
}

void PythonRuntimeLease::release()
{
    if (!m_interpreter)
        return;

    if (s_owner == m_interpreter && s_depth > 0) {
        --s_depth;
        if (s_depth == 0)
            s_owner = nullptr;
    }
    m_interpreter = nullptr;
}

PythonRuntimeLease::~PythonRuntimeLease()
{
    release();
}

void log_python_exception_keep(pybind11::error_already_set& err)
{
    // The GIL may already be released here: the macro's gil_scoped_acquire is a
    // local destroyed by stack unwinding before this catch runs. Touching Python
    // state below needs the GIL. PyGILState_Ensure is reentrant — harmless if held.
    PythonGILState gil;
    if (!gil)
        return;

    // Non-destructive: print the traceback to sys.stderr (tee'd to the session log)
    // WITHOUT consuming err, so the caller can rethrow it intact. For example, downstream C++
    // catchers can still read err.what() for the user-facing dialog. We must NOT use
    // restore()+PyErr_Print() here as those empty err.
    try {
        namespace py    = pybind11;
        py::module_ tb  = py::module_::import("traceback");
        py::module_ sys = py::module_::import("sys");
        tb.attr("print_exception")(err.type(), err.value(), err.trace(), py::none(), sys.attr("stderr"));
    } catch (...) {
        // Fallback: at least get the formatted message out. err.what() includes the
        // traceback in recent pybind11 and does not consume err.
        try {
            pybind11::module_::import("sys").attr("stderr").attr("write")(std::string(err.what()) + "\n");
        } catch (...) {
        }
    }
}

namespace {

std::string format_python_error(PyObject* ptype, PyObject* pvalue, PyObject* ptraceback)
{
    std::string result;

    if (ptype) {
        PyObject* type_name = PyObject_GetAttrString(ptype, "__name__");
        if (type_name) {
            const char* s = PyUnicode_AsUTF8(type_name);
            if (s) result += s;
            Py_DECREF(type_name);
        }
    }

    if (pvalue) {
        if (!result.empty()) result += ": ";
        PyObject* s = PyObject_Str(pvalue);
        if (s) {
            const char* cstr = PyUnicode_AsUTF8(s);
            if (cstr) result += cstr;
            Py_DECREF(s);
        }
    }

    if (ptraceback) {
        result += "\nTraceback (most recent call last):";
        PyTracebackObject* tb = reinterpret_cast<PyTracebackObject*>(ptraceback);
        while (tb) {
            PyFrameObject* frame = tb->tb_frame;
            int line = PyFrame_GetLineNumber(frame);
            PyCodeObject* code = PyFrame_GetCode(frame); // returns a NEW strong reference; must be released
            const char* filename = code ? PyUnicode_AsUTF8(code->co_filename) : nullptr;
            const char* funcname = code ? PyUnicode_AsUTF8(code->co_name) : nullptr;
            result += "\n  File \"" + std::string(filename ? filename : "?") +
                      "\", line " + std::to_string(line) +
                      ", in " + std::string(funcname ? funcname : "?");
            Py_XDECREF(code);
            tb = tb->tb_next;
        }
    }

    return result;
}

#ifdef _WIN32
constexpr const char* PYTHON_DLL       = "python312.dll";
constexpr const char* PYTHON_DEBUG_DLL = "python312_d.dll";
#else
constexpr const char* PYTHON_STDLIB_DIR = "python3.12";
constexpr const char* PYTHON_EXECUTABLE = "python3.12";
#endif

std::string executable_name(const char* base)
{
#ifdef _WIN32
    return std::string(base) + ".exe";
#else
    return base;
#endif
}

bool add_sys_path_entry(const boost::filesystem::path& path, std::string& error, bool* inserted = nullptr)
{
    if (inserted)
        *inserted = false;

    PyObject* sys_path = PySys_GetObject("path");
    if (!sys_path || !PyList_Check(sys_path)) {
        error = "Python sys.path is not available";
        return false;
    }

    const std::string path_str = path.string();
    PyObjectPtr py_path(PyUnicode_DecodeFSDefault(path_str.c_str()));
    if (!py_path) {
        error = "Failed to decode path for Python sys.path: " + path_str;
        PyErr_Clear();
        return false;
    }

    const int contains = PySequence_Contains(sys_path, py_path.get());
    if (contains == 1)
        return true;
    if (contains < 0)
        PyErr_Clear();

    if (PyList_Insert(sys_path, 0, py_path.get()) != 0) {
        error = "Failed to add path to Python sys.path: " + path_str;
        PyErr_Clear();
        return false;
    }

    if (inserted)
        *inserted = true;

    return true;
}

void log_python_stream(PyObject* sys, const char* name)
{
    PyObject* stream = PyObject_GetAttrString(sys, name);
    if (!stream) {
        PyErr_Clear();
        BOOST_LOG_TRIVIAL(info) << "Python shutdown: sys." << name << " is not set";
        return;
    }

    PyObject* original        = nullptr;
    std::string original_name = std::string("__") + name + "__";
    original                  = PyObject_GetAttrString(sys, original_name.c_str());
    if (!original)
        PyErr_Clear();

    BOOST_LOG_TRIVIAL(info) << "Python shutdown: sys." << name << " type=" << Py_TYPE(stream)->tp_name << " ptr=" << stream
                            << " original_ptr=" << original << " is_original=" << (stream == original);

    Py_XDECREF(original);
    Py_DECREF(stream);
}

void restore_python_stream(PyObject* sys, const char* name)
{
    std::string original_name = std::string("__") + name + "__";
    PyObject* original        = PyObject_GetAttrString(sys, original_name.c_str());
    if (!original) {
        PyErr_Clear();
        original = Py_NewRef(Py_None);
    }

    if (PyObject_SetAttrString(sys, name, original) < 0) {
        PyErr_Clear();
        BOOST_LOG_TRIVIAL(warning) << "Python shutdown: failed to restore sys." << name;
    } else {
        BOOST_LOG_TRIVIAL(info) << "Python shutdown: restored sys." << name << " to type=" << Py_TYPE(original)->tp_name
                                << " ptr=" << original;
    }

    Py_DECREF(original);
}

// Tee Python sys.stderr to <data_dir>/log/python_*.log so plugin errors are
// persisted. Uncaught exceptions in plugin-spawned threads never cross the
// pybind11 boundary back into C++ — CPython's default threading.excepthook
// prints them to sys.stderr and lets the thread die — so capturing stderr is
// the one place all of them can be observed.
void install_python_stderr_redirect()
{
    // Mirror the session filename convention from GUI_App: python_<weekday>_<mon>_<day>_<HH>_<MM>_<SS>_<pid>.log
    std::time_t t = std::time(nullptr);
    std::tm* now  = std::localtime(&t);
    std::ostringstream name;
    name << std::put_time(now, "python_%a_%b_%d_%H_%M_%S_") << get_current_pid() << ".log";
    const std::string log_path = (boost::filesystem::path(data_dir()) / "log" / name.str()).generic_string();

    const std::string redirect_script =
        "import io, os, sys, threading\n"
        "_ORCA_PYTHON_LOG = \"" + log_path + "\"\n"
        + std::string(R"REDIRECT_SCRIPT(
class _OrcaTeeStderr(io.TextIOBase):
    def __init__(self, original, path):
        self._original = original
        self._path = path
        self._lock = threading.Lock()
    def writable(self):
        return True
    def write(self, s):
        try:
            if self._original is not None:
                self._original.write(s)
        except Exception:
            pass
        try:
            with self._lock, open(self._path, "a", encoding="utf-8", errors="replace") as f:
                f.write(s)
        except Exception:
            pass
        return len(s)
    def flush(self):
        try:
            if self._original is not None:
                self._original.flush()
        except Exception:
            pass

os.makedirs(os.path.dirname(_ORCA_PYTHON_LOG), exist_ok=True)
sys.stderr = _OrcaTeeStderr(sys.__stderr__, _ORCA_PYTHON_LOG)
del _OrcaTeeStderr
)REDIRECT_SCRIPT");

    if (PyRun_SimpleString(redirect_script.c_str()) != 0) {
        PyErr_Clear();
        BOOST_LOG_TRIVIAL(warning) << "Failed to install Python stderr redirect to " << log_path;
    } else {
        BOOST_LOG_TRIVIAL(info) << "Python stderr redirected to " << log_path;
    }
}

void log_and_restore_python_stdio()
{
    PyObject* sys = PyImport_ImportModule("sys");
    if (!sys) {
        PyErr_Clear();
        BOOST_LOG_TRIVIAL(warning) << "Python shutdown: failed to import sys for stdio diagnostics";
        return;
    }

    log_python_stream(sys, "stdout");
    log_python_stream(sys, "stderr");
    restore_python_stream(sys, "stdout");
    restore_python_stream(sys, "stderr");

    Py_DECREF(sys);
}

bool valid_python_home(const boost::filesystem::path& candidate)
{
#ifdef _WIN32
    return boost::filesystem::exists(candidate / "Lib" / "encodings") &&
           (boost::filesystem::exists(candidate / PYTHON_DLL) || boost::filesystem::exists(candidate / PYTHON_DEBUG_DLL));
#else
    return boost::filesystem::exists(candidate / "lib" / PYTHON_STDLIB_DIR / "encodings");
#endif
}

boost::filesystem::path find_bundled_python_home()
{
    namespace fs = boost::filesystem;

#ifdef __APPLE__
    fs::path bundle_python = fs::path(resources_dir()).parent_path() / "MacOS" / "python";
    if (valid_python_home(bundle_python))
        return bundle_python;
#elif defined(_WIN32)
    fs::path exe_python = boost::dll::program_location().parent_path() / "python";
    if (valid_python_home(exe_python))
        return exe_python;
#else
    fs::path linux_python = fs::path(resources_dir()).parent_path() / "lib" / "python";
    if (valid_python_home(linux_python))
        return linux_python;
#endif

    fs::path configured_python = ORCA_BUNDLED_PYTHON_ROOT;
    if (!configured_python.empty() && valid_python_home(configured_python))
        return configured_python;

    const char* prefix_path = std::getenv("CMAKE_PREFIX_PATH");
    if (prefix_path && std::strlen(prefix_path) > 0) {
        fs::path libpython = fs::path(prefix_path) / "libpython";
        if (valid_python_home(libpython))
            return libpython;
    }

    fs::path res_python = fs::path(resources_dir()) / "python";
    if (valid_python_home(res_python))
        return res_python;

#ifndef _WIN32
    fs::path data_python = fs::path(data_dir()) / "python";
    if (valid_python_home(data_python))
        return data_python;
#endif

    return {};
}

boost::filesystem::path find_python_executable(const boost::filesystem::path& python_home)
{
    namespace fs = boost::filesystem;

#ifdef _WIN32
    const std::vector<fs::path> candidates = {
        python_home / "python.exe",
        python_home / "python_d.exe",
    };
#else
    const std::vector<fs::path> candidates = {
        python_home / "bin" / PYTHON_EXECUTABLE,
        python_home / "bin" / "python3",
        python_home / "bin" / "python",
    };
#endif

    for (const fs::path& candidate : candidates) {
        if (fs::exists(candidate) && fs::is_regular_file(candidate))
            return candidate;
    }

    return {};
}

} // namespace

std::string PythonInterpreter::python_abi_tag()
{
    return std::string("cp") + std::to_string(PY_MAJOR_VERSION) + std::to_string(PY_MINOR_VERSION);
}

PythonInterpreter& PythonInterpreter::instance()
{
    static PythonInterpreter inst;
    return inst;
}

std::string PythonInterpreter::shared_packages_dir()
{
    return (boost::filesystem::path(data_dir()) / "python" / "packages" / PythonInterpreter::python_abi_tag()).string();
}

std::string PythonInterpreter::bundled_python_executable()
{
    const boost::filesystem::path python_home = find_bundled_python_home();
    if (python_home.empty())
        return {};

    const boost::filesystem::path executable = find_python_executable(python_home);
    return executable.empty() ? std::string{} : executable.string();
}

std::string PythonInterpreter::bundled_uv_path()
{
    namespace fs = boost::filesystem;

    const fs::path configured_uv = ORCA_BUNDLED_UV_EXECUTABLE;
    if (!configured_uv.empty() && fs::exists(configured_uv) && fs::is_regular_file(configured_uv))
        return configured_uv.string();

    // <binary dir>/tools/uv covers the macOS bundle (Contents/MacOS/tools/uv)
    // and build-tree runs; <resources>/tools/uv covers the install() and
    // AppImage layouts.
    const std::string uv_exe               = executable_name("uv");
    const std::vector<fs::path> candidates = {
        fs::path(resources_dir()) / "tools" / "uv" / uv_exe,
        boost::dll::program_location().parent_path() / "tools" / "uv" / uv_exe,
    };

    for (const fs::path& candidate : candidates) {
        if (fs::exists(candidate) && fs::is_regular_file(candidate))
            return candidate.string();
    }

    return {};
}

bool PythonInterpreter::initialize()
{
    std::unique_lock<std::shared_mutex> runtime_lock(m_runtime_mutex);
    if (m_initialized.load(std::memory_order_acquire)) {
        return true;
    }

    m_last_error.clear();
    m_plugin_path_users.clear();
    m_plugin_path_owned.clear();
    m_plugin_module_users.clear();
    m_plugin_module_owned.clear();

    try {
        // Set Python home to the bundled Python installation
        // This is critical for finding the standard library (encodings module, etc.)

        namespace fs = boost::filesystem;
        const std::string python_home = find_bundled_python_home().string();

        if (python_home.empty()) {
            m_last_error = "Could not locate bundled Python installation";
            BOOST_LOG_TRIVIAL(error) << "Could not locate bundled Python installation";
            BOOST_LOG_TRIVIAL(error) << "Configured bundled Python root: " << ORCA_BUNDLED_PYTHON_ROOT;
#ifdef _WIN32
            BOOST_LOG_TRIVIAL(error) << "Searched next to executable: "
                                     << (boost::dll::program_location().parent_path() / "python").string();
#endif
            BOOST_LOG_TRIVIAL(error) << "Searched in resources_dir: " << resources_dir();
#ifndef _WIN32
            BOOST_LOG_TRIVIAL(error) << "Searched in data_dir: " << data_dir();
#endif
            BOOST_LOG_TRIVIAL(error) << "CMAKE_PREFIX_PATH: "
                                     << (std::getenv("CMAKE_PREFIX_PATH") ? std::getenv("CMAKE_PREFIX_PATH") : "not set");
            return false;
        }

        BOOST_LOG_TRIVIAL(info) << "Found bundled Python home: " << python_home;

// Verify Python standard library directory exists
#ifdef _WIN32
        fs::path python_lib = fs::path(python_home) / "Lib";
#else
        fs::path python_lib = fs::path(python_home) / "lib" / PYTHON_STDLIB_DIR;
#endif
        if (!fs::exists(python_lib)) {
            m_last_error = "Python standard library directory not found at: " + python_lib.string();
            BOOST_LOG_TRIVIAL(error) << "Python standard library directory not found at: " << python_lib.string();
            BOOST_LOG_TRIVIAL(error) << "Please build Python dependencies or check installation";
            return false;
        }

        BOOST_LOG_TRIVIAL(info) << "Python standard library found at: " << python_lib.string();

#ifdef _WIN32
        fs::path python_dll = fs::exists(fs::path(python_home) / PYTHON_DLL) ? fs::path(python_home) / PYTHON_DLL :
                                                                               fs::path(python_home) / PYTHON_DEBUG_DLL;
        if (!fs::exists(python_dll)) {
            m_last_error = "Python DLL not found in: " + python_home;
            BOOST_LOG_TRIVIAL(error) << "Python DLL not found in: " << python_home;
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << "Python DLL found at: " << python_dll.string();
        if (!fs::exists(fs::path(python_home) / "DLLs")) {
            BOOST_LOG_TRIVIAL(warning) << "Python DLLs directory not found at: " << (fs::path(python_home) / "DLLs").string();
        }
#endif

        // Log the exact paths being used for debugging
        BOOST_LOG_TRIVIAL(info) << "Setting Python home to: " << python_home;
        BOOST_LOG_TRIVIAL(info) << "Python 3.12 stdlib path: " << python_lib.string();

        // Verify encodings module exists
        fs::path encodings_path = python_lib / "encodings";
        if (fs::exists(encodings_path)) {
            BOOST_LOG_TRIVIAL(info) << "Encodings module found at: " << encodings_path.string();
        } else {
            BOOST_LOG_TRIVIAL(warning) << "Encodings module NOT found at: " << encodings_path.string();
        }

        BOOST_LOG_TRIVIAL(info) << "Using Python 3.12 PyConfig initialization API";

        // Set Python home - this is the prefix where Python libraries are located
        PyConfig config;
        PyConfig_InitPythonConfig(&config);
        // Do not let the host process's PYTHONPATH or user site-packages override the bundled
        // runtime used by plugins.
        config.use_environment    = 0;
        config.user_site_directory = 0;

        BOOST_LOG_TRIVIAL(debug) << "Calling PyConfig_SetBytesString with home=" << python_home;
        PyStatus status = PyConfig_SetBytesString(&config, &config.home, python_home.c_str());
        if (PyStatus_Exception(status)) {
            m_last_error = status.err_msg ? status.err_msg : "Failed to set Python home";
            BOOST_LOG_TRIVIAL(error) << "Failed to set Python home to: " << python_home << ": " << m_last_error;
            PyConfig_Clear(&config);
            return false;
        }
        BOOST_LOG_TRIVIAL(debug) << "Python home set successfully";

        // Set program name
        status = PyConfig_SetBytesString(&config, &config.program_name, "OrcaSlicer");
        if (PyStatus_Exception(status)) {
            m_last_error = status.err_msg ? status.err_msg : "Failed to set program name";
            BOOST_LOG_TRIVIAL(error) << "Failed to set program name: " << m_last_error;
            PyConfig_Clear(&config);
            return false;
        }
        BOOST_LOG_TRIVIAL(debug) << "Program name set successfully";

        // Use pybind11's scoped_interpreter which properly initializes
        // pybind11 internals for multi-threaded use — raw Py_InitializeFromConfig
        // does not set up thread state tracking that pybind11 requires.
        BOOST_LOG_TRIVIAL(debug) << "Creating py::scoped_interpreter with PyConfig...";
        try {
            m_interpreter = std::make_unique<pybind11::scoped_interpreter>(&config);
        } catch (const std::exception& ex) {
            m_last_error = std::string("Python initialization failed: ") + ex.what();
            BOOST_LOG_TRIVIAL(error) << m_last_error;
            PyConfig_Clear(&config);
            return false;
        }
        // PyConfig is cleared by pybind11's initialize_interpreter() internally.
        BOOST_LOG_TRIVIAL(debug) << "py::scoped_interpreter initialized successfully";

        if (!Py_IsInitialized()) {
            m_last_error = "Python interpreter not initialized";
            BOOST_LOG_TRIVIAL(error) << "Python interpreter not initialized";
            return false;
        }

        // Log Python paths for debugging
        PyObject* sys = PyImport_ImportModule("sys");
        if (sys) {
            PyObject* path = PyObject_GetAttrString(sys, "path");
            if (path) {
                PyObject* path_str = PyObject_Str(path);
                if (path_str) {
                    const char* path_cstr = PyUnicode_AsUTF8(path_str);
                    if (path_cstr) {
                        BOOST_LOG_TRIVIAL(debug) << "Python sys.path: " << path_cstr;
                    }
                    Py_DECREF(path_str);
                }
                Py_DECREF(path);
            }
            Py_DECREF(sys);
        }

        BOOST_LOG_TRIVIAL(info) << "Python " << Py_GetVersion() << " initialized successfully";

        const fs::path shared_packages = shared_packages_dir();
        boost::system::error_code ec;
        fs::create_directories(shared_packages, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(warning) << "Failed to create Python shared package directory: " << shared_packages.string() << ": "
                                       << ec.message();
        } else {
            std::string path_error;
            if (add_sys_path_entry(shared_packages, path_error))
                BOOST_LOG_TRIVIAL(info) << "Added Python shared package directory to sys.path: " << shared_packages.string();
            else
                BOOST_LOG_TRIVIAL(warning) << path_error;
        }

        const std::string uv_path = bundled_uv_path();
        if (!uv_path.empty())
            BOOST_LOG_TRIVIAL(info) << "Bundled uv executable found at: " << uv_path;
        else
            BOOST_LOG_TRIVIAL(info) << "Bundled uv executable not found";

        // Install the CPython audit hook for plugin policy enforcement.
        // This is defense-in-depth: today it only inspects the `open` audit event
        // and blocks writes outside the allowed roots; subprocess/socket/ctypes and
        // other events are not yet handled.  It is NOT a full security sandbox.
        PluginAuditManager::instance().install_hook();

        // Persist Python stderr (plugin tracebacks, including uncaught
        // background-thread exceptions) to <data_dir>/log/python_*.log.
        install_python_stderr_redirect();

        // Release the GIL so other threads can acquire it via PyGILState_Ensure.
        // Without this, calls from background threads will block trying to acquire the GIL.
        m_main_thread_state = PyEval_SaveThread();
        m_initialized.store(true, std::memory_order_release);
        BOOST_LOG_TRIVIAL(debug) << "Main thread released Python GIL after initialization";
        return true;

    } catch (const std::exception& ex) {
        m_last_error = std::string("Exception initializing Python: ") + ex.what();
        BOOST_LOG_TRIVIAL(error) << "Exception initializing Python: " << ex.what();
        return false;
    }
}

PythonInterpreter::~PythonInterpreter() { shutdown(); }

void PythonInterpreter::shutdown()
{
    std::unique_lock<std::shared_mutex> runtime_lock(m_runtime_mutex);
    if (!m_initialized.load(std::memory_order_acquire))
        return;

    BOOST_LOG_TRIVIAL(info) << "Python interpreter shutdown enter";

    // Reacquire the GIL using the saved thread state before finalizing.
    if (m_main_thread_state) {
        BOOST_LOG_TRIVIAL(debug) << "Restoring Python main thread state before shutdown";
        PyEval_RestoreThread(m_main_thread_state);
        m_main_thread_state = nullptr;
    }

    log_and_restore_python_stdio();

    BOOST_LOG_TRIVIAL(info) << "Finalizing Python interpreter";
    m_interpreter.reset();
    BOOST_LOG_TRIVIAL(info) << "Python interpreter finalized";

    m_plugin_path_users.clear();
    m_plugin_path_owned.clear();
    m_plugin_module_users.clear();
    m_plugin_module_owned.clear();
    m_initialized.store(false, std::memory_order_release);
}

bool PythonInterpreter::add_plugin_sys_path(const std::string& path, std::string& error)
{
    if (!m_initialized.load(std::memory_order_acquire)) {
        error = "Python interpreter not initialized";
        return false;
    }

    PythonGILState gil;
    if (!gil) {
        error = "Python interpreter is shutting down";
        return false;
    }

    return add_plugin_sys_path_locked(path, error);
}

bool PythonInterpreter::add_plugin_sys_path_locked(const std::string& path, std::string& error)
{
    bool inserted = false;
    if (!add_sys_path_entry(boost::filesystem::path(path), error, &inserted))
        return false;

    auto users = m_plugin_path_users.find(path);
    if (users == m_plugin_path_users.end()) {
        m_plugin_path_owned[path] = inserted;
        m_plugin_path_users.emplace(path, 1);
    } else {
        ++users->second;
    }

    return true;
}

void PythonInterpreter::remove_plugin_sys_paths_locked(const std::vector<std::string>& paths)
{
    PyObject* sys_path = PySys_GetObject("path");
    if (!sys_path || !PyList_Check(sys_path)) {
        PyErr_Clear();
        return;
    }

    for (const std::string& path : paths) {
        auto users = m_plugin_path_users.find(path);
        if (users == m_plugin_path_users.end())
            continue;

        if (--users->second != 0)
            continue;

        const bool owned = m_plugin_path_owned[path];
        m_plugin_path_users.erase(users);
        m_plugin_path_owned.erase(path);
        if (!owned)
            continue;

        PyObjectPtr py_path(PyUnicode_DecodeFSDefault(path.c_str()));
        if (!py_path) {
            PyErr_Clear();
            continue;
        }

        for (Py_ssize_t index = PyList_Size(sys_path) - 1; index >= 0; --index) {
            PyObject* entry = PyList_GetItem(sys_path, index); // borrowed reference
            int         equal = entry ? PyObject_RichCompareBool(entry, py_path.get(), Py_EQ) : 0;
            if (equal == 1 && PySequence_DelItem(sys_path, index) != 0)
                PyErr_Clear();
            else if (equal < 0)
                PyErr_Clear();
        }
    }
}

void PythonInterpreter::record_plugin_modules_locked(const std::string& module_name,
                                                      const std::vector<std::string>& plugin_paths,
                                                      std::vector<std::string>* plugin_modules)
{
    if (!plugin_modules)
        return;

    PyObject* modules = PyImport_GetModuleDict();
    if (!modules)
        return;

    PyObjectPtr names(PyDict_Keys(modules));
    if (!names) {
        PyErr_Clear();
        return;
    }

    const std::string prefix = module_name + ".";
    const auto        path_matches = [](const std::string& file, const std::string& root) {
        namespace fs = boost::filesystem;

        boost::system::error_code ec;
        fs::path                  file_path = fs::weakly_canonical(fs::path(file), ec);
        if (ec) {
            ec.clear();
            file_path = fs::absolute(fs::path(file), ec);
        }
        if (ec)
            return false;

        ec.clear();
        fs::path root_path = fs::weakly_canonical(fs::path(root), ec);
        if (ec) {
            ec.clear();
            root_path = fs::absolute(fs::path(root), ec);
        }
        if (ec)
            return false;

        const std::string file_string = file_path.generic_string();
        std::string       root_string = root_path.generic_string();
        if (root_string.empty())
            return false;
        if (root_string.back() != '/')
            root_string.push_back('/');
        return file_string.compare(0, root_string.size(), root_string) == 0;
    };

    const Py_ssize_t count = PyList_Size(names.get());
    for (Py_ssize_t index = 0; index < count; ++index) {
        PyObject* name_obj = PyList_GetItem(names.get(), index); // borrowed reference
        if (!name_obj || !PyUnicode_Check(name_obj))
            continue;

        const char* name_utf8 = PyUnicode_AsUTF8(name_obj);
        if (!name_utf8) {
            PyErr_Clear();
            continue;
        }

        const std::string name(name_utf8);
        const bool        is_plugin_namespace = name == module_name || name.compare(0, prefix.size(), prefix) == 0;
        bool              is_plugin_path_module = false;
        if (!is_plugin_namespace) {
            PyObject* module = PyDict_GetItem(modules, name_obj); // borrowed reference
            if (!module)
                continue;

            PyObjectPtr file(PyObject_GetAttrString(module, "__file__"));
            if (file && PyUnicode_Check(file.get())) {
                const char* file_utf8 = PyUnicode_AsUTF8(file.get());
                if (file_utf8) {
                    for (const std::string& path : plugin_paths) {
                        if (path_matches(file_utf8, path)) {
                            is_plugin_path_module = true;
                            break;
                        }
                    }
                } else {
                    PyErr_Clear();
                }
            } else {
                PyErr_Clear();
            }

            if (!is_plugin_path_module) {
                PyObjectPtr package_path(PyObject_GetAttrString(module, "__path__"));
                if (package_path) {
                    const Py_ssize_t path_count = PySequence_Size(package_path.get());
                    if (path_count < 0) {
                        PyErr_Clear();
                    } else {
                        for (Py_ssize_t path_index = 0; path_index < path_count && !is_plugin_path_module; ++path_index) {
                            PyObjectPtr path_entry(PySequence_GetItem(package_path.get(), path_index));
                            if (!path_entry) {
                                PyErr_Clear();
                                continue;
                            }
                            if (!PyUnicode_Check(path_entry.get()))
                                continue;
                            const char* path_utf8 = PyUnicode_AsUTF8(path_entry.get());
                            if (!path_utf8) {
                                PyErr_Clear();
                                continue;
                            }
                            for (const std::string& path : plugin_paths) {
                                if (path_matches(path_utf8, path)) {
                                    is_plugin_path_module = true;
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    PyErr_Clear();
                }
            }
        }

        if (!is_plugin_namespace && !is_plugin_path_module)
            continue;

        auto users = m_plugin_module_users.find(name);
        if (users == m_plugin_module_users.end()) {
            m_plugin_module_owned[name] = true;
            m_plugin_module_users.emplace(name, 1);
        } else {
            ++users->second;
        }
        plugin_modules->push_back(name);
    }
}

void PythonInterpreter::remove_plugin_modules_locked(const std::vector<std::string>& plugin_modules)
{
    PyObject* modules = PyImport_GetModuleDict();
    if (!modules)
        return;

    for (const std::string& name : plugin_modules) {
        auto users = m_plugin_module_users.find(name);
        if (users == m_plugin_module_users.end())
            continue;

        if (--users->second != 0)
            continue;

        const bool owned = m_plugin_module_owned[name];
        m_plugin_module_users.erase(users);
        m_plugin_module_owned.erase(name);
        if (owned && PyDict_DelItemString(modules, name.c_str()) != 0)
            PyErr_Clear();
    }
}

void PythonInterpreter::remove_module_tree_locked(const std::string& module_name)
{
    if (module_name.empty())
        return;

    PyObject* modules = PyImport_GetModuleDict();
    if (!modules)
        return;

    PyObjectPtr names(PyDict_Keys(modules));
    if (!names) {
        PyErr_Clear();
        return;
    }

    const std::string prefix = module_name + ".";
    const Py_ssize_t count  = PyList_Size(names.get());
    for (Py_ssize_t index = 0; index < count; ++index) {
        PyObject* name_obj = PyList_GetItem(names.get(), index); // borrowed reference
        if (!name_obj || !PyUnicode_Check(name_obj))
            continue;

        const char* name_utf8 = PyUnicode_AsUTF8(name_obj);
        if (!name_utf8) {
            PyErr_Clear();
            continue;
        }

        const std::string name(name_utf8);
        if (name != module_name && name.compare(0, prefix.size(), prefix) != 0)
            continue;

        if (PyDict_DelItem(modules, name_obj) != 0)
            PyErr_Clear();
    }
}

void PythonInterpreter::unload_module(PyObject*                      module,
                                      const std::string&              module_name,
                                      const std::vector<std::string>& plugin_paths,
                                      const std::vector<std::string>& plugin_modules)
{
    if (!module && plugin_paths.empty() && plugin_modules.empty())
        return;

    if (!m_initialized.load(std::memory_order_acquire))
        return;

    PythonGILState gil;
    if (!gil)
        return;

    remove_plugin_modules_locked(plugin_modules);
    if (plugin_modules.empty())
        remove_module_tree_locked(module_name);
    remove_plugin_sys_paths_locked(plugin_paths);
    Py_XDECREF(module);
}

PyObject* PythonInterpreter::load_module_from_file(const std::string&       file_path,
                                                   std::string&              error,
                                                   std::vector<std::string>* plugin_paths,
                                                   std::vector<std::string>* plugin_modules)
{
    if (!m_initialized.load(std::memory_order_acquire)) {
        error = "Python interpreter not initialized";
        return nullptr;
    }

    namespace fs = boost::filesystem;
    fs::path path(file_path);

    if (!fs::exists(path)) {
        error = "File does not exist: " + file_path;
        return nullptr;
    }

    PythonGILState gil;
    if (!gil) {
        error = "Python interpreter is shutting down";
        return nullptr;
    }

    // Add the directory to sys.path
    fs::path dir_path       = path.parent_path();
    std::string module_name = path.stem().string();

    const std::string dir = dir_path.string();
    if (!add_plugin_sys_path_locked(dir, error))
        return nullptr;
    if (plugin_paths)
        plugin_paths->push_back(dir);

    // Ensure module is re-imported fresh by removing any cached instance.
    remove_module_tree_locked(module_name);

    // Import the module
    PyObject* module = PyImport_ImportModule(module_name.c_str());
    record_plugin_modules_locked(module_name, plugin_paths ? *plugin_paths : std::vector<std::string>{}, plugin_modules);
    if (!module) {
        PyObject *ptype, *pvalue, *ptraceback;
        PyErr_Fetch(&ptype, &pvalue, &ptraceback);
        error = "Failed to import module: " + format_python_error(ptype, pvalue, ptraceback);
        Py_XDECREF(ptype);
        Py_XDECREF(pvalue);
        Py_XDECREF(ptraceback);
        return nullptr;
    }

    return module;
}

PyObject* PythonInterpreter::load_module_from_directory(const std::string& dir_path,
                                                        const std::string& pkg_name,
                                                        std::string&       error,
                                                        std::vector<std::string>* plugin_paths,
                                                        std::vector<std::string>* plugin_modules)
{
    if (!m_initialized.load(std::memory_order_acquire)) {
        error = "Python interpreter not initialized";
        return nullptr;
    }

    namespace fs = boost::filesystem;
    fs::path dir(dir_path);

    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        error = "Directory does not exist: " + dir_path;
        return nullptr;
    }

    PythonGILState gil;
    if (!gil) {
        error = "Python interpreter is shutting down";
        return nullptr;
    }

    const std::string directory = dir.string();
    if (!add_plugin_sys_path_locked(directory, error))
        return nullptr;
    if (plugin_paths)
        plugin_paths->push_back(directory);

    remove_module_tree_locked(pkg_name);

    PyObject* module = PyImport_ImportModule(pkg_name.c_str());
    record_plugin_modules_locked(pkg_name, plugin_paths ? *plugin_paths : std::vector<std::string>{}, plugin_modules);
    if (!module) {
        PyObject *ptype, *pvalue, *ptraceback;
        PyErr_Fetch(&ptype, &pvalue, &ptraceback);
        error = "Failed to import module: " + format_python_error(ptype, pvalue, ptraceback);
        Py_XDECREF(ptype);
        Py_XDECREF(pvalue);
        Py_XDECREF(ptraceback);
        return nullptr;
    }

    return module;
}

PyObject* PythonInterpreter::load_module_from_whl(const std::string& file_path,
                                                  const std::string& pkg_name,
                                                  std::string&       error,
                                                  std::vector<std::string>* plugin_paths,
                                                  std::vector<std::string>* plugin_modules)
{
    if (!m_initialized.load(std::memory_order_acquire)) {
        error = "Python interpreter not initialized";
        return nullptr;
    }

    namespace fs = boost::filesystem;

    fs::path whl_path(file_path);

    if (!fs::exists(whl_path)) {
        error = "File does not exist: " + file_path;
        return nullptr;
    }

    fs::path extract_dir = whl_path.parent_path() / "__whl_extracted__" / pkg_name;

    if (!fs::exists(extract_dir)) {
        if (!extract_zip_to_directory(whl_path, extract_dir, error))
            return nullptr;
    }

    return load_module_from_directory(extract_dir.string(), pkg_name, error, plugin_paths, plugin_modules);
}

} // namespace Slic3r
