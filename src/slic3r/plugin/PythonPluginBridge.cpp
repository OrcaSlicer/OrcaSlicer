#include "PythonPluginBridge.hpp"

#include <boost/log/trivial.hpp>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "PythonInterpreter.hpp"
#include "host/PluginHost.hpp"
#include "PyPluginPackage.hpp"
#include "PyPluginTrampoline.hpp"
#include "pluginTypes/printerAgent/PrinterAgentPluginCapability.hpp"
#include "pluginTypes/script/ScriptPluginCapability.hpp"
#include "pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp"

namespace py = pybind11;

namespace Slic3r {
namespace {

// Python plugin discovery is a two-step capture:
// 1) PluginLoader sets an active plugin key and imports the Python module.
// 2) Python decorators/API calls enter these pybind callbacks without receiving the
//    C++ PluginDescriptor, so the callbacks use the active key to attach Python classes
//    to the plugin currently being loaded.
//
// The pending maps hold Python class objects, not plugin instances. Instances are created
// only after the package class has had a chance to register every capability.
thread_local std::string g_active_plugin_key;
std::mutex g_registry_mutex;
std::unordered_map<std::string, std::vector<py::object>> g_pending_capabilities;
std::unordered_map<std::string, py::object> g_pending_package;
struct PluginInstanceHandle
{
    // The C++ plugin interface points into a Python object. Keep both alive through one
    // shared control block; CapturedCapability later exposes an aliasing shared_ptr to plugin.
    std::shared_ptr<PluginCapabilityInterface> plugin;
    py::object keep_alive;

    ~PluginInstanceHandle()
    {
        if (keep_alive) {
            // Dropping a py::object decrefs the Python object, so acquire a runtime lease and
            // the GIL. If shutdown has already won the lease, intentionally abandon the wrapper.
            PythonGILState gil;
            if (gil) {
                keep_alive = py::object();
            } else {
                (void) keep_alive.release();
            }
        }
    }
};

void discard_pending_capture_without_python(const std::string& plugin_key)
{
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    auto                        capabilities = g_pending_capabilities.find(plugin_key);
    if (capabilities != g_pending_capabilities.end()) {
        for (py::object& capability : capabilities->second)
            (void) capability.release();
        g_pending_capabilities.erase(capabilities);
    }

    auto package = g_pending_package.find(plugin_key);
    if (package != g_pending_package.end()) {
        (void) package->second.release();
        g_pending_package.erase(package);
    }
}

} // namespace

PythonPluginBridge& PythonPluginBridge::instance()
{
    static PythonPluginBridge bridge;
    return bridge;
}

void PythonPluginBridge::begin_plugin_capture(const std::string& plugin_key)
{
    PythonGILState gil;
    if (!gil) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot begin Python plugin capture while the interpreter is shutting down";
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "Beginning Python plugin capture for key " << plugin_key;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        // Start from a clean slot in case a previous failed load left pending Python classes
        // for this same entry path.
        g_pending_capabilities.erase(plugin_key);
        g_pending_package.erase(plugin_key);
    }
    // From now until finalize/cancel, @orca.plugin and register_capability() calls made by
    // Python code on this thread are attributed to this plugin.
    g_active_plugin_key = plugin_key;
}

std::vector<CapturedCapability> PythonPluginBridge::finalize_plugin_capture(const std::string& plugin_key, std::string& error)
{
    PythonGILState gil;
    if (!gil) {
        error = "Python interpreter is shutting down";
        return {};
    }
    BOOST_LOG_TRIVIAL(info) << "Finalizing Python plugin capture for key " << plugin_key;

    // Phase 1: run the package class's register_capabilities() while the active key is
    // still set. That method is expected to call orca.register_capability() once per
    // capability class, and register_capability() needs g_active_plugin_key to know which
    // pending bucket to append to.
    {
        auto clear_active_key = [&plugin_key]() {
            if (g_active_plugin_key == plugin_key)
                g_active_plugin_key.clear();
        };
        auto discard_pending_for_key = [&plugin_key]() {
            std::lock_guard<std::mutex> lock(g_registry_mutex);
            g_pending_capabilities.erase(plugin_key);
            g_pending_package.erase(plugin_key);
        };

        try {
            // The @orca.plugin decorator records the package class during module import.
            // Move it into a local py::object and remove it from the pending map so the
            // registry no longer owns it once finalization starts.
            py::object package_cls;
            {
                std::lock_guard<std::mutex> lock(g_registry_mutex);
                auto it = g_pending_package.find(plugin_key);
                if (it != g_pending_package.end()) {
                    package_cls = it->second;
                    g_pending_package.erase(it);
                }
            }
            if (!package_cls) {
                error = "Plugin did not register a package class; decorate it with @orca.plugin";
                BOOST_LOG_TRIVIAL(error) << error << " for key " << plugin_key;
                discard_pending_for_key();
                clear_active_key();
                return {};
            }

            // The package instance is only a registration coordinator. It is not returned
            // to the rest of the plugin system; only the capability classes it registers
            // are kept.
            py::object package = package_cls();
            package.attr("register_capabilities")();
        } catch (py::error_already_set& err) {
            log_python_exception_keep(err);
            error = err.what();
            BOOST_LOG_TRIVIAL(error) << "Plugin register_capabilities raised Python exception for key " << plugin_key
                                     << " error=" << error;
            discard_pending_for_key();
            clear_active_key();
            return {};
        } catch (const std::exception& ex) {
            error = ex.what();
            BOOST_LOG_TRIVIAL(error) << "Plugin register_capabilities raised exception for key " << plugin_key
                                     << " error=" << error;
            discard_pending_for_key();
            clear_active_key();
            return {};
        }
    }

    // Phase 2: move the capability classes that register_capabilities() appended into a
    // local vector. From this point the pending registry no longer owns these py::objects.
    std::vector<py::object> classes;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        auto it = g_pending_capabilities.find(plugin_key);
        if (it != g_pending_capabilities.end()) {
            classes = std::move(it->second);
            g_pending_capabilities.erase(it);
        }
    }

    // Registration is complete. Later register_capability() calls should fail instead of
    // accidentally attaching themselves to this plugin.
    if (g_active_plugin_key == plugin_key)
        g_active_plugin_key.clear();

    BOOST_LOG_TRIVIAL(info) << "Collected " << classes.size() << " registered capability class(es) for key " << plugin_key;

    std::vector<CapturedCapability> capabilities;
    capabilities.reserve(classes.size());

    // Phase 3: instantiate each registered capability class and convert it to the common
    // C++ interface used by the rest of OrcaSlicer.
    for (auto& cls : classes) {
        try {
            py::object instance = cls();
            if (!py::isinstance<PluginCapabilityInterface>(instance)) {
                error = "Registered capability must inherit from a PluginCapability base";
                BOOST_LOG_TRIVIAL(error) << "Python plugin capture failed type check for key " << plugin_key
                                         << " error=" << error;
                return {};
            }

            auto capability_iface = instance.cast<std::shared_ptr<PluginCapabilityInterface>>();
            if (!capability_iface) {
                error = "Failed to cast Python capability to PluginCapabilityInterface";
                BOOST_LOG_TRIVIAL(error) << "Python plugin capture failed cast for key " << plugin_key
                                         << " error=" << error;
                return {};
            }

            // This is a registered capability, not the transient orca.base package.
            // get_name() is required on capabilities and is cached for preset lookup.
            std::string name = capability_iface->get_name();

            // Capability names feed ';'-delimited config/preset serialization and drive
            // dispatch, so unlike display names they cannot be silently rewritten — a ';'
            // here is a hard error that rejects the whole plugin capture.
            if (name.find(';') != std::string::npos) {
                error = "Capability name must not contain ';': " + name;
                BOOST_LOG_TRIVIAL(error) << "Python plugin capture rejected capability for key " << plugin_key
                                         << " error=" << error;
                return {};
            }

            auto handle        = std::make_shared<PluginInstanceHandle>();
            handle->keep_alive = instance;
            handle->plugin     = std::move(capability_iface);

            CapturedCapability captured;
            // Return a shared_ptr<PluginCapabilityInterface> while keeping PluginInstanceHandle
            // as the owner, so the Python instance stays alive as long as the C++ interface does.
            captured.instance = std::shared_ptr<PluginCapabilityInterface>(handle, handle->plugin.get());
            captured.name     = std::move(name);
            capabilities.emplace_back(std::move(captured));
        } catch (py::error_already_set& err) {
            // Direct Python call (cls() / get_name() above), not a trampoline override —
            // log the traceback here. GIL is held for the duration of finalize_plugin_capture.
            log_python_exception_keep(err);
            error = err.what();
            BOOST_LOG_TRIVIAL(error) << "Python plugin capture raised Python exception for key " << plugin_key
                                     << " error=" << error;
            return {};
        } catch (const std::exception& ex) {
            error = ex.what();
            BOOST_LOG_TRIVIAL(error) << "Python plugin capture raised exception for key " << plugin_key
                                     << " error=" << error;
            return {};
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Instantiated " << capabilities.size() << " Python capability instance(s) for key " << plugin_key;
    return capabilities;
}

void PythonPluginBridge::cancel_plugin_capture(const std::string& plugin_key)
{
    PythonGILState gil;
    BOOST_LOG_TRIVIAL(warning) << "Cancelling Python plugin capture for key " << plugin_key;

    if (!gil) {
        discard_pending_capture_without_python(plugin_key);
        if (g_active_plugin_key == plugin_key)
            g_active_plugin_key.clear();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        // Import or dependency setup failed before finalization. Drop anything the module
        // may already have registered under this key.
        g_pending_capabilities.erase(plugin_key);
        g_pending_package.erase(plugin_key);
    }

    if (g_active_plugin_key == plugin_key)
        g_active_plugin_key.clear();
}

void PythonPluginBridge::clear_pending_captures()
{
    PythonGILState gil;
    if (!gil) {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        BOOST_LOG_TRIVIAL(info) << "Clearing " << g_pending_capabilities.size()
                                << " pending Python plugin capture(s) without Python interpreter";
        // py::object destruction would decref Python objects. If the interpreter is already
        // gone, release the wrappers instead and intentionally skip decref.
        for (auto& [plugin_key, plugins] : g_pending_capabilities) {
            (void) plugin_key;
            for (py::object& plugin : plugins)
                (void) plugin.release();
        }
        g_pending_capabilities.clear();
        for (auto& [plugin_key, pkg] : g_pending_package) {
            (void) plugin_key;
            (void) pkg.release();
        }
        g_pending_package.clear();
        g_active_plugin_key.clear();
        return;
    }

    std::lock_guard<std::mutex> lock(g_registry_mutex);
    BOOST_LOG_TRIVIAL(info) << "Clearing " << g_pending_capabilities.size() << " pending Python plugin capture(s)";
    g_pending_capabilities.clear();
    g_pending_package.clear();
    g_active_plugin_key.clear();
}

void bind_python_api(pybind11::module_& m)
{
    m.doc() = "OrcaSlicer plugin API";

    auto pluginTypes = py::enum_<PluginCapabilityType>(m, "PluginType", "Available plugin capability groups")
                           .value("PrinterConnection", PluginCapabilityType::PrinterConnection)
                           .value("Automation", PluginCapabilityType::Automation)
                           .value("Analysis", PluginCapabilityType::Analysis)
                           .value("Importer", PluginCapabilityType::Importer)
                           .value("Exporter", PluginCapabilityType::Exporter)
                           .value("Visualization", PluginCapabilityType::Visualization)
                           .value("Script", PluginCapabilityType::Script)
                           .value("SlicingPipeline", PluginCapabilityType::SlicingPipeline)
                           .value("Unknown", PluginCapabilityType::Unknown)
                           .export_values();

    py::enum_<PluginResult>(m, "PluginResult", "Execution summary code")
        .value("Success", PluginResult::Success)
        .value("Skipped", PluginResult::Skipped)
        .value("RecoverableError", PluginResult::RecoverableError)
        .value("FatalError", PluginResult::FatalError)
        .export_values();

    py::class_<PluginContext>(m, "PluginContext", "Context shared with plugin entry points")
        .def(py::init<>())
        .def_readwrite("orca_version", &PluginContext::orca_version);

    py::class_<ExecutionResult>(m, "ExecutionResult", "Structured execution outcome")
        .def(py::init<>())
        .def(py::init<PluginResult, std::string, std::string>())
        .def_readwrite("status", &ExecutionResult::status)
        .def_readwrite("message", &ExecutionResult::message)
        .def_readwrite("data", &ExecutionResult::data)
        .def_static("success", &ExecutionResult::success, py::arg("message") = std::string(), py::arg("data") = std::string())
        .def_static("skipped", &ExecutionResult::skipped, py::arg("message") = std::string())
        .def_static("failure", &ExecutionResult::failure, py::arg("status"), py::arg("message"), py::arg("data") = std::string());

    py::class_<PluginCapabilityInterface, PyPluginInterfaceTrampoline, std::shared_ptr<PluginCapabilityInterface>>(m, "PythonPluginBase")
        .def(py::init<>())
        .def("get_name", &PluginCapabilityInterface::get_name)
        .def("get_type", &PluginCapabilityInterface::get_type)
        .def("on_load", &PluginCapabilityInterface::on_load)
        .def("on_unload", &PluginCapabilityInterface::on_unload);

    // Expose the package marker base as orca.base. @orca.plugin later verifies that the
    // decorated class derives from this exact pybind-registered C++ type.
    py::class_<PyPluginPackage, PyPluginPackageTrampoline>(m, "base")
        .def(py::init<>())
        .def("register_capabilities", &PyPluginPackage::register_capabilities);

    BOOST_LOG_TRIVIAL(debug) << "Registering embedded Python plugin type bindings";

    // Make sure you register your bindings here
    PrinterAgentPluginCapability::RegisterBindings(m, pluginTypes);
    ScriptPluginCapability::RegisterBindings(m, pluginTypes);
    SlicingPipelinePluginCapability::RegisterBindings(m, pluginTypes);
    PluginHost::RegisterBindings(m);
    BOOST_LOG_TRIVIAL(debug) << "Registered ScriptPluginCapability Python bindings";

    m.def(
        "register_capability",
        [](py::object plugin_cls) {
            if (g_active_plugin_key.empty()) {
                throw py::value_error("register_capability() called outside plugin discovery context");
            }

            // Store capability classes only, not instances. Finalization instantiates them
            // after the package has registered the full set for this plugin.
            py::handle base = py::type::of<PluginCapabilityInterface>();
            const int is_subclass = PyObject_IsSubclass(plugin_cls.ptr(), base.ptr());
            if (is_subclass != 1) {
                if (is_subclass < 0)
                    PyErr_Clear();
                throw py::value_error("Registered class must inherit from a PluginCapability base");
            }

            std::lock_guard<std::mutex> lock(g_registry_mutex);
            g_pending_capabilities[g_active_plugin_key].push_back(std::move(plugin_cls));
            BOOST_LOG_TRIVIAL(debug) << "Registered Python plugin capability class for key " << g_active_plugin_key;
        },
        R"pbdoc(Register a PluginCapability subclass while OrcaSlicer loads your module.)pbdoc");

    m.def("plugin", [](py::object cls) {
        if (g_active_plugin_key.empty())
            throw py::value_error("@orca.plugin used outside plugin discovery context");
        if (!PyType_Check(cls.ptr()))
            throw py::value_error("@orca.plugin must decorate a class");
        // The decorator is only a marker/capture hook. It records the package class now;
        // finalize_plugin_capture() instantiates it later and calls register_capabilities().
        py::handle base = py::type::of<PyPluginPackage>();
        const int is_subclass = PyObject_IsSubclass(cls.ptr(), base.ptr());
        if (is_subclass != 1) {
            if (is_subclass < 0)
                PyErr_Clear();
            throw py::value_error("@orca.plugin must decorate a subclass of orca.base");
        }
        {
            std::lock_guard<std::mutex> lock(g_registry_mutex);
            auto& slot = g_pending_package[g_active_plugin_key];
            if (slot)
                throw py::value_error("multiple @orca.plugin classes registered; exactly one is allowed per plugin");
            slot = cls;
        }
        return cls; // decorator returns the class unchanged
    }, R"pbdoc(Mark the single plugin package class (the orca.base subclass) for this file.)pbdoc");

}

} // namespace Slic3r

#ifdef ORCA_PYTHON_STUBGEN_MODULE
PYBIND11_MODULE(orca, m) { Slic3r::bind_python_api(m); }
#else
PYBIND11_EMBEDDED_MODULE(orca, m) { Slic3r::bind_python_api(m); }
#endif
