#ifndef slic3r_PythonInterpreter_hpp_
#define slic3r_PythonInterpreter_hpp_

#include <Python.h>
#include <pytypedefs.h>
#include <atomic>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "libslic3r/libslic3r.h"

namespace pybind11 {
class scoped_interpreter;
class error_already_set;
}

namespace Slic3r {

class PythonRuntimeLease;

// Print a Python exception's full traceback to sys.stderr (tee'd to the session
// log) WITHOUT consuming err.
//
// Used by the ORCA_PY_OVERRIDE_AUDITED trampoline macro: the
// traceback is logged centrally at the C++<->Python boundary, then the original
// error_already_set is rethrown intact so downstream C++ catchers can still build
// the user-facing dialog from err.what(). Because it is non-destructive it does
// NOT call restore()/PyErr_Print() (those will empty err); it prints via Python's
// traceback module instead.
void log_python_exception_keep(pybind11::error_already_set& err);


// RAII wrapper for Python interpreter initialization/finalization
class PythonInterpreter
{
public:
    static PythonInterpreter& instance();

    // Initialize the Python interpreter
    bool initialize();

    // Check if interpreter is initialized
    bool is_initialized() const { return m_initialized.load(std::memory_order_acquire); }

    // Acquire a shared lease on the interpreter. Shutdown takes the exclusive side of this
    // lock, so a caller that owns a lease can safely acquire the GIL and touch Python objects.
    PythonRuntimeLease acquire_runtime_lease();

    const std::string& last_error() const { return m_last_error; }

    // Shared user-writable package directory added to sys.path for plugins.
    static std::string shared_packages_dir();

    // Bundled Python executable path, or empty when no executable is found.
    static std::string bundled_python_executable();

    // Bundled uv executable path, or empty when uv is not bundled/found.
    static std::string bundled_uv_path();

    // Python ABI tag for the bundled interpreter, e.g. "cp312".
    static std::string python_abi_tag();

    // Finalize the Python interpreter.
    void shutdown();

    // Add a path owned by one plugin load. The path is reference-counted across plugins and is
    // removed when the final plugin using it is unloaded.
    bool add_plugin_sys_path(const std::string& path, std::string& error);

    // Load a Python module from file path
    PyObject* load_module_from_file(const std::string& file_path,
                                    std::string&       error,
                                    std::vector<std::string>* plugin_paths = nullptr,
                                    std::vector<std::string>* plugin_modules = nullptr);
    PyObject* load_module_from_whl(const std::string& whl_path,
                                   const std::string& pkg_name,
                                   std::string&       error,
                                   std::vector<std::string>* plugin_paths = nullptr,
                                   std::vector<std::string>* plugin_modules = nullptr);
    PyObject* load_module_from_directory(const std::string& dir_path,
                                         const std::string& pkg_name,
                                         std::string&       error,
                                         std::vector<std::string>* plugin_paths = nullptr,
                                         std::vector<std::string>* plugin_modules = nullptr);

    // Remove the complete module namespace and plugin-owned paths, then release the root module
    // reference. Safe to call with a null module when a load failed after adding paths.
    void unload_module(PyObject* module,
                       const std::string& module_name,
                       const std::vector<std::string>& plugin_paths,
                       const std::vector<std::string>& plugin_modules);

    // Destructor finalizes Python if shutdown() was not called explicitly.
    ~PythonInterpreter();

private:
    friend class PythonRuntimeLease;

    PythonInterpreter() = default;
    PythonInterpreter(const PythonInterpreter&) = delete;
    PythonInterpreter& operator=(const PythonInterpreter&) = delete;

    bool add_plugin_sys_path_locked(const std::string& path, std::string& error);
    void remove_plugin_sys_paths_locked(const std::vector<std::string>& paths);
    void record_plugin_modules_locked(const std::string& module_name,
                                      const std::vector<std::string>& plugin_paths,
                                      std::vector<std::string>* plugin_modules);
    void remove_plugin_modules_locked(const std::vector<std::string>& plugin_modules);
    void remove_module_tree_locked(const std::string& module_name);

    std::atomic<bool> m_initialized{false};
    mutable std::shared_mutex m_runtime_mutex;
    PyThreadState* m_main_thread_state = nullptr; // thread state saved after releasing GIL post-initialize
    std::unique_ptr<pybind11::scoped_interpreter> m_interpreter;
    std::string m_last_error;
    std::unordered_map<std::string, std::size_t> m_plugin_path_users;
    std::unordered_map<std::string, bool>        m_plugin_path_owned;
    std::unordered_map<std::string, std::size_t> m_plugin_module_users;
    std::unordered_map<std::string, bool>        m_plugin_module_owned;
};

class PythonRuntimeLease
{
public:
    PythonRuntimeLease() = default;
    PythonRuntimeLease(PythonRuntimeLease&& other) noexcept;
    PythonRuntimeLease& operator=(PythonRuntimeLease&& other) noexcept;
    ~PythonRuntimeLease();

    PythonRuntimeLease(const PythonRuntimeLease&)            = delete;
    PythonRuntimeLease& operator=(const PythonRuntimeLease&) = delete;

    explicit operator bool() const { return m_interpreter != nullptr; }

private:
    friend class PythonInterpreter;

    explicit PythonRuntimeLease(PythonInterpreter& interpreter);
    void release();

    static thread_local PythonInterpreter* s_owner;
    static thread_local unsigned int      s_depth;

    PythonInterpreter*                  m_interpreter = nullptr;
    std::shared_lock<std::shared_mutex> m_lock;
};

inline PythonRuntimeLease PythonInterpreter::acquire_runtime_lease()
{
    return PythonRuntimeLease(*this);
}

// RAII helper for Python GIL (Global Interpreter Lock)
class PythonGILState
{
public:
    PythonGILState() {
        m_runtime_lease = PythonInterpreter::instance().acquire_runtime_lease();
        if (m_runtime_lease) {
            m_state    = PyGILState_Ensure();
            m_acquired = true;
        }
    }

    ~PythonGILState() {
        if (m_acquired)
            PyGILState_Release(m_state);
    }

    explicit operator bool() const { return m_acquired; }

private:
    PythonRuntimeLease m_runtime_lease;
    PyGILState_STATE   m_state{};
    bool               m_acquired = false;
};

// RAII helper for Python object references
class PyObjectPtr
{
public:
    explicit PyObjectPtr(PyObject* obj = nullptr) : m_obj(obj) {}

    ~PyObjectPtr() {
        if (m_obj) {
            Py_DECREF(m_obj);
        }
    }

    PyObject* get() const { return m_obj; }
    PyObject* release() {
        PyObject* temp = m_obj;
        m_obj = nullptr;
        return temp;
    }

    void reset(PyObject* obj = nullptr) {
        if (m_obj) {
            Py_DECREF(m_obj);
        }
        m_obj = obj;
    }

    operator bool() const { return m_obj != nullptr; }

private:
    PyObject* m_obj;

    PyObjectPtr(const PyObjectPtr&) = delete;
    PyObjectPtr& operator=(const PyObjectPtr&) = delete;
};

} // namespace Slic3r

#endif /* slic3r_PythonInterpreter_hpp_ */
