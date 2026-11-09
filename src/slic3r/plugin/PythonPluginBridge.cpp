#include "PythonPluginBridge.hpp"

#include <algorithm>
#include <optional>
#include <boost/log/trivial.hpp>
#include <exception>
#include "libslic3r/LifecycleEvents.hpp"
#include <memory>
#include <mutex>
#include <slic3r/plugin/PluginAuditManager.hpp>
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <vector>
#include <utility>

#include "PythonInterpreter.hpp"
#include "PythonRunScope.hpp"
#include "PythonScriptRunner.hpp"
#include "PluginJsonPy.hpp"
#include "PluginOperations.hpp"
#include "host/PluginHostTools.hpp"
#include "PluginFsUtils.hpp"
#include "PluginConfig.hpp"
#include "host/PluginHost.hpp"
#include "PyPluginPackage.hpp"
#include "PyPluginTrampoline.hpp"
#include "pluginTypes/printerAgent/PrinterAgentPluginCapability.hpp"
#include "pluginTypes/pages/PagesPluginCapability.hpp"
#include "pluginTypes/resourceTexture/ResourceTexturePluginCapability.hpp"
#include "pluginTypes/script/ScriptPluginCapability.hpp"
#include "pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp"
#include <pybind11/pytypes.h>
#include <pybind11/gil.h>
#include "slic3r/GUI/I18N.hpp"
#include <nlohmann/json.hpp>

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
std::unordered_map<std::string, PluginPermissions> g_pending_permissions;
// Operations a loading plugin declares with register_operation(); their handlers release under the GIL.
std::unordered_map<std::string, std::vector<PluginOperation>> g_pending_operations;
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

    g_pending_permissions.erase(plugin_key);

    g_pending_operations.erase(plugin_key);
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
        g_pending_permissions.erase(plugin_key);
        g_pending_operations.erase(plugin_key);
    }
    // From now until finalize/cancel, @orca.plugin and register_capability() calls made by
    // Python code on this thread are attributed to this plugin.
    g_active_plugin_key = plugin_key;
}

std::vector<CapturedCapability> PythonPluginBridge::finalize_plugin_capture(const std::string& capture_key,
                                                                              const std::string& plugin_key,
                                                                              std::string&       error)
{
    PythonGILState gil;
    if (!gil) {
        error = "Python interpreter is shutting down";
        return {};
    }
    BOOST_LOG_TRIVIAL(info) << "Finalizing Python plugin capture for key " << capture_key;

    // Phase 1: run the package class's register_capabilities() while the active key is
    // still set. That method is expected to call orca.register_capability() once per
    // capability class, and register_capability() needs g_active_plugin_key to know which
    // pending bucket to append to.
    {
        auto clear_active_key = [&capture_key]() {
            if (g_active_plugin_key == capture_key)
                g_active_plugin_key.clear();
        };
        auto discard_pending_for_key = [&capture_key]() {
            std::lock_guard<std::mutex> lock(g_registry_mutex);
            g_pending_capabilities.erase(capture_key);
            g_pending_package.erase(capture_key);
            g_pending_permissions.erase(capture_key);
            g_pending_operations.erase(capture_key);
        };

        try {
            // The @orca.plugin decorator records the package class during module import.
            // Move it into a local py::object and remove it from the pending map so the
            // registry no longer owns it once finalization starts.
            py::object package_cls;
            {
                std::lock_guard<std::mutex> lock(g_registry_mutex);
                auto it = g_pending_package.find(capture_key);
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

            // Permission declarations are collected while register_capabilities() runs so the
            // plugin can describe its needs without touching wx from the Python load worker. Ask
            // only after registration returns, before capability instances are materialized.
            PluginPermissions requested_permissions;
            {
                std::lock_guard<std::mutex> lock(g_registry_mutex);
                auto it = g_pending_permissions.find(capture_key);
                if (it != g_pending_permissions.end()) {
                    requested_permissions = std::move(it->second);
                    g_pending_permissions.erase(it);
                }
            }
            const bool has_permission_request =
                !requested_permissions.fs_read.empty() || !requested_permissions.fs_readwrite.empty() ||
                !requested_permissions.network_http.empty() || !requested_permissions.network_socket.empty() ||
                !requested_permissions.process.empty() || requested_permissions.threading;
            if (has_permission_request) {
                bool granted = false;
                {
                    py::gil_scoped_release release;
                    granted = PluginAuditManager::instance().request_permissions(plugin_key, requested_permissions);
                }
                if (!granted) {
                    error = _u8L("Plugin permission request denied");
                    BOOST_LOG_TRIVIAL(warning) << error << " for key " << plugin_key;
                    discard_pending_for_key();
                    clear_active_key();
                    return {};
                }
            }
            // Calling tools is asked on its own, after the rest: a refusal leaves the plugin loading without it.
            if (requested_permissions.tools) {
                bool granted = false;
                {
                    py::gil_scoped_release release;
                    granted = PluginAuditManager::instance().request_tools_permission(plugin_key);
                }
                if (!granted)
                    BOOST_LOG_TRIVIAL(info) << "Plugin " << plugin_key << " loads without the tools permission";
            }
            std::vector<PluginOperation> declared;
            {
                std::lock_guard<std::mutex> lock(g_registry_mutex);
                auto it = g_pending_operations.find(capture_key);
                if (it != g_pending_operations.end()) {
                    declared = std::move(it->second);
                    g_pending_operations.erase(it);
                }
            }
            std::vector<PluginOperation> granted_operations;
            for (PluginOperation& op : declared) {
                bool granted = false;
                {
                    py::gil_scoped_release release;
                    granted = PluginAuditManager::instance().request_operation_permission(plugin_key, op.name, op.description);
                }
                if (granted) {
                    op.plugin_key = plugin_key;
                    granted_operations.push_back(std::move(op));
                } else
                    BOOST_LOG_TRIVIAL(info) << "Plugin " << plugin_key << " does not offer its operation " << op.name;
            }
            PluginOperations::instance().stage(plugin_key, std::move(granted_operations));
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
        auto it = g_pending_capabilities.find(capture_key);
        if (it != g_pending_capabilities.end()) {
            classes = std::move(it->second);
            g_pending_capabilities.erase(it);
        }
    }

    // Registration is complete. Later register_capability() calls should fail instead of
    // accidentally attaching themselves to this plugin.
    if (g_active_plugin_key == capture_key)
        g_active_plugin_key.clear();

    BOOST_LOG_TRIVIAL(info) << "Collected " << classes.size() << " registered capability class(es) for key " << capture_key;

    std::vector<CapturedCapability> capabilities;
    capabilities.reserve(classes.size());

    // Phase 3: instantiate each registered capability class and convert it to the common
    // C++ interface used by the rest of OrcaSlicer.
    for (auto& cls : classes) {
        try {
            py::object instance = cls();
            if (!py::isinstance<PluginCapabilityInterface>(instance)) {
                error = "Registered capability must inherit from a PluginCapability base";
                BOOST_LOG_TRIVIAL(error) << "Python plugin capture failed type check for key " << capture_key
                                         << " error=" << error;
                return {};
            }

            auto capability_iface = instance.cast<std::shared_ptr<PluginCapabilityInterface>>();
            if (!capability_iface) {
                error = "Failed to cast Python capability to PluginCapabilityInterface";
                BOOST_LOG_TRIVIAL(error) << "Python plugin capture failed cast for key " << capture_key
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
                BOOST_LOG_TRIVIAL(error) << "Python plugin capture rejected capability for key " << capture_key
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
            BOOST_LOG_TRIVIAL(error) << "Python plugin capture raised Python exception for key " << capture_key
                                     << " error=" << error;
            return {};
        } catch (const std::exception& ex) {
            error = ex.what();
            BOOST_LOG_TRIVIAL(error) << "Python plugin capture raised exception for key " << capture_key
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
        g_pending_permissions.erase(plugin_key);
        g_pending_operations.erase(plugin_key);
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
        g_pending_permissions.clear();
        g_pending_operations.clear();
        g_active_plugin_key.clear();
        return;
    }

    std::lock_guard<std::mutex> lock(g_registry_mutex);
    BOOST_LOG_TRIVIAL(info) << "Clearing " << g_pending_capabilities.size() << " pending Python plugin capture(s)";
    g_pending_capabilities.clear();
    g_pending_package.clear();
    g_pending_permissions.clear();
    g_active_plugin_key.clear();
}

void bind_python_api(pybind11::module_& m)
{
    m.doc() = "OrcaSlicer plugin API";

    py::enum_<PluginCapabilityType>(m, "PluginType", "Available plugin capability groups")
        .value("PrinterConnection", PluginCapabilityType::PrinterConnection)
        .value("Pages", PluginCapabilityType::Pages)
        .value("Analysis", PluginCapabilityType::Analysis)
        .value("Importer", PluginCapabilityType::Importer)
        .value("Exporter", PluginCapabilityType::Exporter)
        .value("Visualization", PluginCapabilityType::Visualization)
        .value("Script", PluginCapabilityType::Script)
        .value("SlicingPipeline", PluginCapabilityType::SlicingPipeline)
        .value("ResourceTexture", PluginCapabilityType::ResourceTexture)
        .value("Unknown", PluginCapabilityType::Unknown)
        .export_values();

    py::enum_<PluginResult>(m, "PluginResult",
                            "Status of an ExecutionResult. What the host does with each value depends on the\n"
                            "method that returns it; see the execute() method of each capability base.")
        .value("Success", PluginResult::Success)
        .value("Skipped", PluginResult::Skipped)
        .value("RecoverableError", PluginResult::RecoverableError)
        .value("FatalError", PluginResult::FatalError)
        .export_values();

    py::enum_<LifecycleEvent>(m, "LifecycleEvent",
                              "Application lifecycle moment passed to on_lifecycle_event(). The LifecycleEventContext\n"
                              "fields describe which events fill them.")
        .value("NewProject", LifecycleEvent::NewProject,
               "A new project is being started. It fires before the current project is reset, so a\n"
               "ProjectClosed event for that project, when it had a file or objects, follows it.")
        .value("ProjectOpened", LifecycleEvent::ProjectOpened, "A project was opened; name is its file path.")
        .value("ProjectBeforeSave", LifecycleEvent::ProjectBeforeSave,
               "The project is about to be saved; name is the target file path.")
        .value("ProjectAfterSave", LifecycleEvent::ProjectAfterSave, "The project was saved; name is its file path.")
        .value("ProjectClosed", LifecycleEvent::ProjectClosed,
               "The current project, when it had a file or objects, was closed or reset; name is its file path\n"
               "without the extension, or empty if it was never saved.")
        .value("ProjectDirtyChanged", LifecycleEvent::ProjectDirtyChanged,
               "The project gained or lost unsaved changes; see dirty and source.")
        .value("SliceStarted", LifecycleEvent::SliceStarted,
               "Slicing started, on the slicing thread; id and name identify the model.")
        .value("SliceGeometryFinished", LifecycleEvent::SliceGeometryFinished,
               "The slicing steps finished, before G-code export, on the slicing thread; id and name identify the\n"
               "model.")
        .value("GCodeExportStarted", LifecycleEvent::GCodeExportStarted,
               "G-code export started, on the slicing thread; msg is the output path.")
        .value("GCodeExportFinished", LifecycleEvent::GCodeExportFinished,
               "G-code export ended; code is Ok or Error, and msg is the output path, followed by the error on\n"
               "failure.")
        .value("SlicingJobComplete", LifecycleEvent::SlicingJobComplete,
               "The background slicing job ended, on the UI thread; code is Ok, Warn when cancelled, or Error with\n"
               "the error in msg.")
        .value("ObjectAdded", LifecycleEvent::ObjectAdded, "An object was added; name is its name.")
        .value("ObjectDeleted", LifecycleEvent::ObjectDeleted, "An object was deleted; name is its name.")
        .value("ObjectTransformed", LifecycleEvent::ObjectTransformed,
               "Objects were moved, rotated, scaled, arranged or auto-oriented; msg says which, and name is the\n"
               "object's name when exactly one object was moved, rotated or scaled.")
        .value("ObjectChanged", LifecycleEvent::ObjectChanged,
               "An object's geometry changed; name, id and, when known, index identify it.")
        .value("ObjectRenamed", LifecycleEvent::ObjectRenamed,
               "An object or one of its parts was renamed; name, previous_name, id, index and source (\"object\" or\n"
               "\"volume\") describe it.")
        .value("PlateCreated", LifecycleEvent::PlateCreated, "A plate was added; name and index identify it.")
        .value("PlateDeleted", LifecycleEvent::PlateDeleted, "A plate was deleted; name and index identify it.")
        .value("PlateSelected", LifecycleEvent::PlateSelected,
               "Another plate became the current plate; name and index identify it.")
        .value("PlateRenamed", LifecycleEvent::PlateRenamed,
               "A plate was renamed; name, previous_name and index describe it.")
        .value("PresetSelected", LifecycleEvent::PresetSelected,
               "A preset was selected; name is the preset name and msg its type: \"print\", \"filament\",\n"
               "\"printer\", \"sla_print\" or \"sla_material\".")
        .value("PresetSaved", LifecycleEvent::PresetSaved,
               "A preset was saved; name is the preset name and msg is \"new\" or \"overwrite\".")
        .value("PrintStateChanged", LifecycleEvent::PrintStateChanged,
               "A printer's print status changed; name is the device id and msg the new status.")
        .value("DeviceOnline", LifecycleEvent::DeviceOnline, "A printer came online; name is its device id.")
        .value("DeviceOffline", LifecycleEvent::DeviceOffline, "A printer went offline; name is its device id.")
        .value("DeviceDiscovered", LifecycleEvent::DeviceDiscovered,
               "A printer not known before was discovered on the network; name is its device id.")
        .value("DeviceSelected", LifecycleEvent::DeviceSelected,
               "The user selected a printer; name is its device id, or empty when the selection was cleared.")
        .value("UploadStarted", LifecycleEvent::UploadStarted,
               "An upload to a print host such as OctoPrint started; name is the uploaded file's name.")
        .value("UploadFinished", LifecycleEvent::UploadFinished,
               "A print host upload ended; code is Ok or Error, with the error in msg.")
        .value("PrintJobStarted", LifecycleEvent::PrintJobStarted,
               "Sending a print job to a printer started; name is the project name, device_id the printer and\n"
               "source the sender.")
        .value("PrintJobFinished", LifecycleEvent::PrintJobFinished,
               "Sending a print job ended; code is Ok, Warn when cancelled, or Error, and msg is \"\", \"cancelled\",\n"
               "\"failed\" or \"exception\".")
        .value("SendJobStarted", LifecycleEvent::SendJobStarted,
               "Sending a file to a printer's storage started; name is the project name and device_id the printer.")
        .value("SendJobFinished", LifecycleEvent::SendJobFinished,
               "Sending a file to a printer's storage ended; code and msg as for PrintJobFinished.")
        .export_values();

    py::enum_<LifecycleEvtCode>(m, "LifecycleEvtCode", "Outcome code accompanying a LifecycleEventContext")
        .value("Ok", LifecycleEvtCode::Ok, "The event happened, or the operation it reports succeeded.")
        .value("Error", LifecycleEvtCode::Error, "The operation failed; msg holds the detail.")
        .value("Warn", LifecycleEvtCode::Warn, "The operation was cancelled.")
        .export_values();

    py::class_<LifecycleEventContext>(m, "LifecycleEventContext",
                                      "Payload accompanying a LifecycleEvent. Each field says which events fill it; a\n"
                                      "field the event does not use is empty, -1 or False.")
        .def(py::init<>())
        .def_readonly("name", &LifecycleEventContext::name,
                      "The event's subject: a file path for Project* events (see ProjectClosed), the model name for\n"
                      "SliceStarted, SliceGeometryFinished, GCodeExport* and SlicingJobComplete, the object, plate or\n"
                      "preset name for Object*, Plate* and Preset* events, the device id for PrintStateChanged and\n"
                      "Device* events, the uploaded file name for Upload* events, and the project name for PrintJob*\n"
                      "and SendJob* events. Empty for NewProject and ProjectDirtyChanged.")
        .def_readonly("code", &LifecycleEventContext::code,
                      "The LifecycleEvtCode. Only GCodeExportFinished, SlicingJobComplete, UploadFinished,\n"
                      "PrintJobFinished and SendJobFinished report anything but Ok: Error on failure, Warn when\n"
                      "cancelled.")
        .def_readonly("msg", &LifecycleEventContext::msg,
                      "Detail text: the output path for GCodeExport* events, followed by the error on failure;\n"
                      "the error or \"cancelled\" for SlicingJobComplete; the error for UploadFinished; \"\",\n"
                      "\"cancelled\", \"failed\" or \"exception\" for PrintJobFinished and SendJobFinished; \"moved\",\n"
                      "\"rotated\", \"scaled\", \"arranged\" or \"auto_oriented\" for ObjectTransformed; the preset\n"
                      "type for PresetSelected; \"new\" or \"overwrite\" for PresetSaved; the new status for\n"
                      "PrintStateChanged; \"online\" or \"offline\" for DeviceOnline and DeviceOffline. Error text\n"
                      "is for people, not for parsing.")
        .def_readonly("id", &LifecycleEventContext::id,
                      "A numeric id as a decimal string: the model's id for SliceStarted, SliceGeometryFinished,\n"
                      "GCodeExport* and SlicingJobComplete, the object's or part's id for ObjectChanged and\n"
                      "ObjectRenamed.")
        .def_readonly("previous_name", &LifecycleEventContext::previous_name,
                      "The old name for ObjectRenamed and PlateRenamed.")
        .def_readonly("device_id", &LifecycleEventContext::device_id,
                      "The target printer's device id for PrintJob* and SendJob* events. PrintStateChanged and\n"
                      "Device* events carry the device id in name instead.")
        .def_readonly("job_id", &LifecycleEventContext::job_id,
                      "The task id for PrintJob* events sent through the multi-printer task queue (source\n"
                      "\"task_manager\").")
        .def_readonly("source", &LifecycleEventContext::source,
                      "Where the event came from: \"object\" or \"volume\" for ObjectRenamed, \"geometry\" for\n"
                      "ObjectChanged, \"print_job\" or \"task_manager\" for PrintJob* events, \"send_job\" for\n"
                      "SendJob* events, and for ProjectDirtyChanged the change that flipped the state: \"plater\",\n"
                      "\"presets\", \"undo_redo\", \"save\" or \"batch\".")
        .def_readonly("index", &LifecycleEventContext::index,
                      "A 0-based index: the plate for Plate* events, the object for ObjectChanged (when known) and\n"
                      "object renames, and the part within its object for part renames.")
        .def_readonly("dirty", &LifecycleEventContext::dirty,
                      "For ProjectDirtyChanged, True when the project now has unsaved changes.");

    py::class_<PluginContext>(m, "PluginContext",
                              "Holds the OrcaSlicer version. No OrcaSlicer call passes one to a plugin; slicing steps\n"
                              "read the version from SlicingPipelineContext.orca_version.")
        .def(py::init<>())
        .def_readwrite("orca_version", &PluginContext::orca_version, "The OrcaSlicer version string.");

    py::class_<ExecutionResult>(m, "ExecutionResult",
                                "Outcome a capability's execute() returns: a PluginResult status, a message for the\n"
                                "user or the log, and optional data. Build one with success(), skipped() or failure().")
        .def(py::init<>(), "A Success result with no message or data.")
        .def(py::init<PluginResult, std::string, std::string>(), py::arg("status"), py::arg("message"),
             py::arg("data"), "A result with the given status, message and data.")
        .def_readwrite("status", &ExecutionResult::status, "The PluginResult status.")
        .def_readwrite("message", &ExecutionResult::message,
                       "Text shown to the user or logged, depending on the capability; may be empty.")
        .def_readwrite("data", &ExecutionResult::data,
                       "Extra result text for the caller, such as a JSON string. The Plugins dialog logs it; the host\n"
                       "does not otherwise read it.")
        .def_static("success", &ExecutionResult::success, py::arg("message") = std::string(),
                    py::arg("data") = std::string(), "Return a Success result with an optional message and data.")
        .def_static("skipped", &ExecutionResult::skipped, py::arg("message") = std::string(),
                    "Return a Skipped result, for a run that had nothing to do, with an optional message.")
        .def_static("failure", &ExecutionResult::failure, py::arg("status"), py::arg("message"),
                    py::arg("data") = std::string(),
                    "Return a failed result: status is PluginResult.RecoverableError or PluginResult.FatalError,\n"
                    "message says what went wrong, and data is optional.");

    // Config lives at the capability level, not as a global orca.* function: the host reads the
    // owning (plugin_key, capability) straight off the instance the call arrived on, so a
    // capability can only ever address its own config and never has to name itself.
    // Registered on the base, so every capability type (script/gcode/printer-agent) inherits it.
    py::class_<PluginCapabilityInterface, PyPluginInterfaceTrampoline, std::shared_ptr<PluginCapabilityInterface>>(
        m, "PythonPluginBase",
        "Base of every capability class, holding what all capabilities share: the required get_name(), the\n"
        "on_load(), on_unload() and on_lifecycle_event() hooks, and the capability's config. Subclass one of\n"
        "the typed bases rather than this class: orca.script.ScriptPluginCapabilityBase,\n"
        "orca.pages.PagesPluginCapabilityBase, orca.printer_agent.PrinterAgentBase,\n"
        "orca.slicing.SlicingPipelineCapabilityBase or orca.resource_texture.ResourceTexturePluginCapabilityBase.")
        .def(py::init<>())
        .def("get_name", &PluginCapabilityInterface::get_name,
             "Required. Return this capability's name. The host reads it once when the plugin loads, shows it\n"
             "in the UI and uses it to refer to the capability in presets and configs, so it must stay the same.\n"
             "It must not contain ';' and must be unique among loaded capabilities of the same type.")
        .def("get_type", &PluginCapabilityInterface::get_type,
             "Return this capability's PluginType. Each typed capability base already returns its own type, and\n"
             "the host reads it once when the plugin loads.")
        .def("on_load", &PluginCapabilityInterface::on_load,
             "Called once when the plugin loads, after all of its capabilities have been created. Raising an\n"
             "exception fails the plugin load. The default does nothing.")
        .def("on_unload", &PluginCapabilityInterface::on_unload,
             "Called once when the plugin unloads (disabled, removed, reloaded or at exit), and after its own\n"
             "on_load() raised. Exceptions are logged and ignored. The default does nothing.")
        .def("on_lifecycle_event", &PluginCapabilityInterface::on_lifecycle_event, py::arg("event"), py::arg("ctx"),
             "Called for every application lifecycle moment, on every loaded and enabled capability whatever\n"
             "its type. event is a LifecycleEvent and ctx the LifecycleEventContext describing it. It runs on\n"
             "the thread that raised the event, a background thread for slicing, export, upload and print-job\n"
             "events. The return value is ignored, and exceptions are logged and ignored. The default does\n"
             "nothing.")
        .def("has_config_ui", &PluginCapabilityInterface::has_config_ui,
             "Override to return True to replace the host's default JSON editor with your own HTML\n"
             "UI, returned by get_config_ui(). Every capability is configurable and appears in the\n"
             "Plugins dialog's Config tab regardless; this only chooses how its config is edited.")
        .def("get_config_ui", &PluginCapabilityInterface::get_config_ui,
             "Override to return the custom configuration UI as an HTML string. Only called when\n"
             "has_config_ui() is True; an empty result falls back to the default JSON editor.\n"
             "Inside the page, use window.orca.getConfig()/saveConfig() to reach this same config.")
        .def(
            "get_default_config",
            [](const PluginCapabilityInterface& self) {
                nlohmann::json config = self.get_default_config();
                return config.dump();
            },
            "Override to return the config that the Config tab's \"Restore defaults\" action writes\n"
            "back, as a dict. Optional: without it the action stores an empty config, which already\n"
            "restores the defaults of a capability that keeps its stored config sparse and applies\n"
            "its own defaults on read. Override it to write an explicit starting config instead.\n"
            "Calling it returns that config as a JSON string.")
        .def(
            "get_config",
            [](const PluginCapabilityInterface& self) {
                nlohmann::json config = capability_get_config(self);
                return config.dump();
            },
            "Return this capability's stored config as a JSON string — json.loads() it to use.\n"
            "\"{}\" if it has never been saved, so the parsed result is always indexable.")
        .def(
            "get_config_version", [](const PluginCapabilityInterface& self) { return capability_get_config_version(self); },
            "Return the plugin version that last wrote this capability's config, so a newer\n"
            "release can spot a stale config and migrate it. Empty string if never saved.")
        .def(
            "save_config",
            [](const PluginCapabilityInterface& self, const std::string& config_str) {
                nlohmann::json config = nlohmann::json::parse(config_str, nullptr, /* allow_exceptions */ false);
                if (config.is_discarded()) {
                    // Refused rather than stored: the caller gets False, and the previously stored
                    // config is left alone. Logged because False alone does not say why.
                    BOOST_LOG_TRIVIAL(error) << "save_config: capability '" << self.get_name() << "' passed a config that is not valid JSON";
                    return false;
                }
                return capability_save_config(self, config);
            },
            py::arg("config"),
            "Persist this capability's config, given as a JSON string (e.g. json.dumps(cfg)).\n"
            "The plugin key, capability name and version are supplied by the host. Returns False if\n"
            "the string is not valid JSON, or if the config file could not be written.");

    // @orca.plugin verifies that the decorated class derives from this exact pybind-registered C++ type.
    py::class_<PyPluginPackage, PyPluginPackageTrampoline>(
        m, "base",
        "Base of a plugin's package class. Subclass it once per plugin, decorate the subclass with\n"
        "@orca.plugin, and override register_capabilities().")
        .def(py::init<>())
        .def("register_capabilities", &PyPluginPackage::register_capabilities,
             "Called once while OrcaSlicer loads the plugin, after its module is imported. Call\n"
             "orca.register_capability() for each capability class, and orca.request_permissions() and\n"
             "orca.register_operation() as needed. Raising an exception fails the plugin load. The default\n"
             "registers nothing.");

    BOOST_LOG_TRIVIAL(debug) << "Registering embedded Python plugin type bindings";

    // Make sure you register your bindings here
    PrinterAgentPluginCapability::RegisterBindings(m);
    PagesPluginCapability::RegisterBindings(m);
    ScriptPluginCapability::RegisterBindings(m);
    SlicingPipelinePluginCapability::RegisterBindings(m);
    ResourceTexturePluginCapability::RegisterBindings(m);
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
        py::arg("capability_class"),
        R"pbdoc(Register a capability class while OrcaSlicer loads your module, normally from register_capabilities().

Pass the class, not an instance: OrcaSlicer creates one instance with no arguments after
register_capabilities() returns. The class must derive from a capability base such as
orca.script.ScriptPluginCapabilityBase. Raises ValueError outside plugin loading or for a class that is
not a capability.)pbdoc");

    m.def(
        "register_operation",
        [](const std::string& name, const std::string& description, const py::object& schema, const py::object& handler, bool changes) {
            if (g_active_plugin_key.empty())
                throw py::value_error("register_operation() called outside plugin discovery context");
            if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
                throw py::value_error("register_operation() takes a name of letters, digits and underscores");
            if (!PyCallable_Check(handler.ptr()))
                throw py::type_error("register_operation() takes a callable handler");
            PluginOperation op;
            op.name        = name;
            op.description = description;
            op.changes     = changes;
            if (!schema.is_none()) {
                op.schema = py_to_json(schema);
                if (!op.schema.is_object() || op.schema.value("type", "") != "object")
                    throw py::value_error("register_operation() takes an object schema: {\"type\": \"object\", \"properties\": ...}");
            }
            // The last reference may go on any thread: released under the GIL, or abandoned once Python is gone.
            op.handler = std::shared_ptr<py::object>(new py::object(handler), [](py::object* held) {
                PythonGILState gil;
                if (!gil)
                    (void) held->release();
                delete held;
            });
            std::lock_guard<std::mutex> lock(g_registry_mutex);
            auto& pending = g_pending_operations[g_active_plugin_key];
            pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const PluginOperation& o) { return o.name == name; }),
                          pending.end());
            pending.push_back(std::move(op));
        },
        // handler is required, and the callable check above refuses a missing one. It defaults to None only so that
        // the signature stays one Python can declare while schema keeps its default, which lets a caller pass the
        // handler by keyword alone.
        py::arg("name"), py::arg("description"), py::arg("schema") = py::none(), py::arg("handler") = py::none(),
        py::arg("changes") = true,
        R"pbdoc(Offer an operation to AI agents and scripts while OrcaSlicer loads your module.

The person is asked whether to allow each operation; an allowed one is reached as
"plugin.<plugin>.<name>" through call_tool and orca.host.tools.call. handler, which is required, is
called as handler(args) with the arguments as a dict, validated against schema (a JSON Schema object,
or None for an operation that takes no arguments); it runs under the plugin's identity and permissions
and returns a JSON value. Pass handler by position after schema, or as handler= on its own.
changes=False marks an operation that only reads.)pbdoc");

    m.def(
        "request_permissions",
        [](const std::vector<std::string>& fs_read,
           const std::vector<std::string>& fs_readwrite,
           const std::vector<std::string>& network_http,
           const std::vector<std::string>& network_socket,
           const std::vector<std::string>& process,
           bool                            threading,
           const std::vector<std::string>& network,
           bool                            tools) {
            if (g_active_plugin_key.empty())
                throw py::value_error("request_permissions() called outside plugin discovery context");

            // Validate and canonicalize everything before touching the pending registry, so a
            // partially invalid call cannot leave a half-recorded request behind.
            const auto reject_empty = [](const char* name, const std::vector<std::string>& targets) {
                for (const std::string& target : targets)
                    if (target.empty())
                        throw py::value_error(std::string("request_permissions(") + name +
                                              "=...) does not accept empty values");
            };
            reject_empty("fs_read", fs_read);
            reject_empty("fs_readwrite", fs_readwrite);
            reject_empty("network_http", network_http);
            reject_empty("network_socket", network_socket);
            reject_empty("process", process);

            // A network URL is reduced to scheme://host, so one grant covers every path, query
            // and port on that host. It is recorded as an HTTP grant; network_permission_matches
            // applies that grant to raw socket events for the same host too.
            std::vector<std::string> normalized_network;
            normalized_network.reserve(network.size());
            for (const std::string& url : network) {
                const std::string normalized = PluginAuditManager::normalize_network_url(url);
                if (normalized.empty())
                    throw py::value_error(
                        "request_permissions(network=...) requires an http:// or https:// URL with a host");
                if (std::find(normalized_network.begin(), normalized_network.end(), normalized) ==
                    normalized_network.end())
                    normalized_network.push_back(normalized);
            }

            std::lock_guard<std::mutex> lock(g_registry_mutex);
            PluginPermissions& requested = g_pending_permissions[g_active_plugin_key];

            const auto add_targets = [](std::vector<std::string>& bucket, const std::vector<std::string>& targets) {
                for (const std::string& target : targets)
                    if (std::find(bucket.begin(), bucket.end(), target) == bucket.end())
                        bucket.push_back(target);
            };

            add_targets(requested.fs_read, fs_read);
            add_targets(requested.fs_readwrite, fs_readwrite);
            add_targets(requested.network_http, network_http);
            add_targets(requested.network_http, normalized_network);
            add_targets(requested.network_socket, network_socket);
            add_targets(requested.process, process);

            if (threading && !requested.threading)
                requested.threading = true;
            requested.tools = requested.tools || tools;
        },
        py::arg("fs_read") = std::vector<std::string>{},
        py::arg("fs_readwrite") = std::vector<std::string>{},
        py::arg("network_http") = std::vector<std::string>{},
        py::arg("network_socket") = std::vector<std::string>{},
        py::arg("process") = std::vector<std::string>{},
        py::arg("threading") = false,
        py::arg("network") = std::vector<std::string>{},
        py::arg("tools") = false,
        R"pbdoc(Request permissions while OrcaSlicer loads your module.

Each bucket takes the target strings the plugin expects the corresponding audit event to
report: filesystem paths for fs_read and fs_readwrite, URLs for network_http, socket addresses
for network_socket, and commands for process. threading=True requests permission to start
threads. Targets already granted to the plugin are not asked for again.

tools=True asks, on its own, for permission to call OrcaSlicer's tools through orca.host.tools,
with what an AI agent can do; when it is refused the plugin still loads and its tool calls fail.

A filesystem target may end in "/*" to grant a whole directory tree, for example
"/path/to/folder/*". That covers the folder itself and every path beneath it, at any depth.
"*" is the only wildcard and is recognized only as the final path component; a "*" anywhere
else in a target is an ordinary character. Wildcards apply to fs_read and fs_readwrite only,
and a grant in one of those buckets never authorizes the other direction. Tree grants are
matched on canonical paths, so a symlink below the granted folder does not extend the grant
outside it. Paths the host denies by name or keyword stay denied even when a wildcard covers
them.

`network` also lists http(s) URLs, but each is reduced to its scheme and host, so one grant covers
every path, query and port on that host and also authorizes raw socket events for it. The host
presents one request to the user after register_capabilities() returns and before any capability
instance is created. Denying the request aborts the plugin load.)pbdoc");

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
    }, py::arg("cls"),
    R"pbdoc(Decorator marking the plugin's package class, its orca.base subclass, and returning it unchanged.

Use it on exactly one class per plugin. Raises ValueError outside plugin loading, on something that is
not a class, on a class that does not derive from orca.base, or on a second package class.)pbdoc");

}

void install_python_thread_audit_shim()
{
    // Runs with the GIL held from PythonInterpreter::initialize(). CPython's own thread bootstrap
    // calls the target with no knowledge of the audit context that created it: the calling
    // thread is the only place the plugin key and capability are still available. Capture them
    // here, then re-establish the same ScopedPluginAuditContext inside the spawned thread before
    // the plugin body runs. The identity lives only in this C++ closure -- no Python-visible
    // setter is added.
    try {
        py::object thread_module = py::module_::import("_thread");

        const int installed = PyObject_HasAttrString(thread_module.ptr(), "_orca_thread_audit_shim_installed");
        if (installed == 1) {
            BOOST_LOG_TRIVIAL(debug) << "[AUDIT] Python thread attribution shim already installed";
            return;
        }
        if (installed < 0)
            PyErr_Clear();

        py::object original = py::reinterpret_steal<py::object>(
            PyObject_GetAttrString(thread_module.ptr(), "start_new_thread"));
        if (!original || !PyCallable_Check(original.ptr())) {
            PyErr_Clear();
            BOOST_LOG_TRIVIAL(error) << "[AUDIT] _thread.start_new_thread is missing or not callable; "
                                        "plugin threads will run without audit attribution";
            return;
        }

        py::object shim = py::cpp_function(
            [original](py::object function, py::object args, py::object kwargs) -> py::object {
                const auto start = [&](const py::object& target) {
                    return kwargs.is_none() ? original(target, args) : original(target, args, kwargs);
                };
                // A script's run reaches the threads it starts. A pool's worker is the exception: pools are reused
                // across runs, so a worker carries neither run nor identity, and each task brings its own, as the
                // pool's initializer does from the run that started the worker.
                std::shared_ptr<PythonRunScope> run = current_python_run().run;
                if (run && python_run_is_pool_worker(function)) {
                    const PoolInitializerBinding initializer = python_run_bind_pool_initializer(function, run);
                    if (initializer == PoolInitializerBinding::Refused)
                        throw std::runtime_error("can't start a new thread: the script's run has ended");
                    try {
                        return start(function);
                    } catch (...) {
                        if (initializer == PoolInitializerBinding::Counted)
                            run->task_done();
                        throw;
                    }
                }
                const std::string plugin_key  = PluginAuditManager::instance().current_plugin();
                if (plugin_key.empty() && !run) {
                    // Not called from a plugin: preserve the stock implementation untouched.
                    return start(function);
                }
                const std::string capability  = PluginAuditManager::instance().current_capability();
                // A thread a Script worker starts may call tools as the worker may.
                const bool        tool_caller = tool_caller_thread();
                // Counted before the OS thread exists, so the run never misses a thread started late.
                const bool waited = run && python_run_waits_for(function);
                if (run && !run->thread_starting(waited))
                    throw std::runtime_error("can't start a new thread: the script's run has ended");

                py::object guarded = py::cpp_function(
                    [function, plugin_key, capability, tool_caller, run, waited](py::args call_args, py::kwargs call_kwargs) -> py::object {
                        std::optional<ScopedPluginAuditContext> audit_scope;
                        if (!plugin_key.empty())
                            audit_scope.emplace(plugin_key, capability);
                        std::optional<ScopedToolCallerThread> caller;
                        if (tool_caller)
                            caller.emplace();
                        std::optional<ScopedPythonRun> bound;
                        struct Ended
                        {
                            PythonRunScope* scope;
                            unsigned long   ident;
                            ~Ended()
                            {
                                if (scope != nullptr)
                                    scope->thread_ended(ident);
                            }
                        } ended{nullptr, PyThread_get_thread_ident()};
                        if (run) {
                            bound.emplace(run, PythonRunThread::Started);
                            run->thread_started(ended.ident, waited);
                            ended.scope = run.get();
                        }
                        PyObject* result = PyObject_Call(function.ptr(), call_args.ptr(), call_kwargs.ptr());
                        if (result == nullptr)
                            throw py::error_already_set();
                        return py::reinterpret_steal<py::object>(result);
                    });
                try {
                    return start(guarded);
                } catch (...) {
                    if (run)
                        run->thread_start_failed(waited);
                    throw;
                }
            },
            py::arg("function"), py::arg("args"), py::arg("kwargs") = py::none());

        if (PyObject_SetAttrString(thread_module.ptr(), "start_new_thread", shim.ptr()) < 0) {
            PyErr_Clear();
            BOOST_LOG_TRIVIAL(error)
                << "[AUDIT] Failed to install Python thread attribution shim on _thread.start_new_thread";
            return;
        }
        if (PyObject_SetAttrString(thread_module.ptr(), "_orca_thread_audit_shim_installed", Py_True) < 0)
            PyErr_Clear();

        // threading.py caches the function at import time (as `_start_new_thread` in 3.12, and as
        // `start_new_thread` in older versions). Patch the cached reference too when the module is
        // already loaded; otherwise its Thread.start() would keep calling the unwrapped original.
        py::object sys_modules = py::module_::import("sys").attr("modules");
        PyObject*  threading_module = PyDict_GetItemString(sys_modules.ptr(), "threading");
        if (threading_module && threading_module != Py_None) {
            const char* cached_names[] = {"_start_new_thread", "start_new_thread"};
            for (const char* cached_name : cached_names) {
                const int has_cached_name = PyObject_HasAttrString(threading_module, cached_name);
                if (has_cached_name < 0) {
                    PyErr_Clear();
                    continue;
                }
                if (has_cached_name != 1)
                    continue;
                if (PyObject_SetAttrString(threading_module, cached_name, shim.ptr()) < 0) {
                    PyErr_Clear();
                    BOOST_LOG_TRIVIAL(warning) << "[AUDIT] Failed to update threading." << cached_name
                                               << " with the Python thread attribution shim";
                }
            }
        }

        BOOST_LOG_TRIVIAL(info) << "[AUDIT] Python thread attribution shim installed";
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT] Failed to install Python thread attribution shim: " << ex.what();
    }
}

} // namespace Slic3r

#ifdef ORCA_PYTHON_STUBGEN_MODULE
PYBIND11_MODULE(orca, m) { Slic3r::bind_python_api(m); }
#else
PYBIND11_EMBEDDED_MODULE(orca, m) { Slic3r::bind_python_api(m); }
#endif
