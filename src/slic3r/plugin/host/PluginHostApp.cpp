#include "PluginHostBindings.hpp"
#include <pybind11/cast.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <libslic3r/GCode/GCodeProcessor.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/AppConfig.hpp>
#include <libslic3r/Semver.hpp>
#include <libslic3r_version.h>
#include <slic3r/GUI/BuildCommit.hpp>
#include <slic3r/GUI/DeviceCore/DevManager.h>
#include <slic3r/GUI/DeviceManager.hpp>
#include <slic3r/GUI/GUI.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/OpenGLManager.hpp>
#include <slic3r/GUI/PartPlate.hpp>
#include <slic3r/GUI/Plater.hpp>
#include <slic3r/Utils/CloudProvider.hpp>
#include <slic3r/Utils/NetworkAgent.hpp>
#include <slic3r/Utils/NetworkAgentFactory.hpp>
#include <slic3r/Utils/bambu_networking.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <wx/app.h>
#include <wx/string.h>
#include <wx/thread.h>
#include <string>
#include <utility>

namespace py = pybind11;

namespace Slic3r {
namespace {

GUI::Plater* current_plater()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    GUI::Plater* plater = GUI::wxGetApp().plater();
    if (plater == nullptr)
        throw std::runtime_error("Plater is not available");

    return plater;
}

PresetBundle* current_preset_bundle()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    PresetBundle* preset_bundle = GUI::wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        throw std::runtime_error("Preset bundle is not available");

    return preset_bundle;
}

GUI::GUI_App& current_app()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");
    return GUI::wxGetApp();
}

// The plater and the device list are only safe to touch from the UI thread, and the 3MF export
// renders thumbnails, which needs the UI thread's GL context.
void require_main_thread(const char* function_name)
{
    if (!wxIsMainThread())
        throw std::runtime_error(std::string(function_name) + "() must be called from the UI thread");
}

void export_3mf_copy(GUI::Plater& plater, const std::string& path)
{
    require_main_thread("export_3mf_copy");
    if (!boost::iends_with(path, ".3mf"))
        throw py::value_error("export_3mf_copy() needs a path ending in .3mf");
    // The thumbnails are rendered by the 3D view, which may not exist yet while plugins load.
    if (!GUI::OpenGLManager::get_gl_info().is_detected())
        throw std::runtime_error("export_3mf_copy() needs the 3D view, which is not ready yet");
    // The file is written from C++, so raise the audit event Python's open(path, "w") raises: the
    // plugin gets the same deny list, permissions and prompt as for writing the file itself.
    if (PySys_Audit("open", "ssi", path.c_str(), "w", 0) < 0)
        throw py::error_already_set();
    if (!plater.export_3mf_copy(GUI::into_path(GUI::from_u8(path))))
        throw std::runtime_error("Failed to write " + path);
}

py::object gl_info()
{
    const GUI::OpenGLManager::GLInfo& info = GUI::OpenGLManager::get_gl_info();
    if (!info.is_detected())
        return py::none();
    py::dict out;
    out["vendor"]       = info.get_vendor();
    out["renderer"]     = info.get_renderer();
    out["version"]      = info.get_version();
    out["glsl_version"] = info.get_glsl_version();
    out["core_profile"] = info.is_core_profile();
    return std::move(out);
}

py::dict app_info()
{
    GUI::GUI_App& app = current_app();
    // The app config has no lock of its own; the UI thread is where it is written.
    require_main_thread("app_info");
    py::dict out;
    out["version"]  = SoftFever_VERSION;
    out["build"]    = GUI::build_commit_label;
    out["mode"]     = app.is_editor() ? "editor" : "gcode viewer";
    out["language"] = GUI::into_u8(app.current_language_code_safe());
    // The app config file is deny-listed for plugins; these are the parts a bug report needs.
    const boost::optional<Semver>& config_version = app.last_config_version();
    out["app_config_version"] = config_version && config_version->valid() ? config_version->to_string_sf() : std::string();
    out["stealth_mode"]       = app.app_config != nullptr && app.app_config->get_stealth_mode();
    out["is_signed_in"]       = app.is_user_login(ORCA_CLOUD_PROVIDER);
    out["is_bambu_signed_in"] = app.is_user_login(BBL_CLOUD_PROVIDER);
    out["network_plugin_version"] = NetworkAgent::is_network_module_loaded() ? NetworkAgent::get_version() : std::string();
    const NetworkLibraryLoadError load_error = NetworkAgent::get_load_error();
    out["network_plugin_error"] = load_error.has_error ? load_error.message : std::string();
    return out;
}

py::object selected_printer()
{
    GUI::GUI_App& app = current_app();
    require_main_thread("selected_printer");
    DeviceManager* devices = app.getDeviceManager();
    MachineObject* machine = devices != nullptr ? devices->get_selected_machine() : nullptr;
    if (machine == nullptr)
        return py::none();
    // Agent, model, link and firmware only: no name, serial, address or access code. Other agents
    // fill the model and firmware with placeholders, so those come from Bambu printers only.
    const bool bambu = machine->printer_agent_id == BBL_PRINTER_AGENT_ID;
    py::dict   out;
    out["agent"]      = machine->printer_agent_id;
    out["model_id"]   = bambu ? machine->printer_type : std::string();
    out["connection"] = machine->connection_type();
    out["online"]     = machine->is_online();
    out["connected"]  = machine->is_connected();
    out["firmware"]   = bambu ? machine->get_ota_version() : std::string();
    return std::move(out);
}

py::object slice_statistics(int plate_index)
{
    GUI::Plater* plater = current_plater();
    require_main_thread("slice_statistics");
    GUI::PartPlateList& plates = plater->get_partplate_list();
    GUI::PartPlate* plate = nullptr;
    if (plate_index < 0)
        plate = plates.get_curr_plate();
    else if (plate_index < plates.get_plate_count())
        plate = plates.get_plate(plate_index);
    if (plate == nullptr || !plate->is_slice_result_valid())
        return py::none();

    const GCodeProcessorResult* result = plate->get_slice_result();
    if (result == nullptr)
        return py::none();

    // The result is shared with the background slicing process.
    std::lock_guard<std::mutex> lock(result->result_mutex);

    const PrintEstimatedStatistics& ps = result->print_statistics;

    // Filament changes in order: (from, to) -> count, from -1 for the first filament of the print.
    std::map<std::pair<int, int>, unsigned int> transitions;
    int active = -1;
    for (const GCodeProcessorResult::MoveVertex& move : result->moves) {
        if (move.type != EMoveType::Extrude || static_cast<int>(move.extruder_id) == active)
            continue;
        ++transitions[{active, static_cast<int>(move.extruder_id)}];
        active = move.extruder_id;
    }

    std::set<size_t> ids;
    for (const auto* volumes : {&ps.model_volumes_per_extruder, &ps.support_volumes_per_extruder,
                                &ps.wipe_tower_volumes_per_extruder, &ps.flush_per_filament,
                                &ps.total_volumes_per_extruder})
        for (const auto& item : *volumes)
            ids.insert(item.first);

    const auto volume_of = [](const std::map<size_t, double>& volumes, size_t id) {
        const auto it = volumes.find(id);
        return it == volumes.end() ? 0.0 : it->second;
    };

    py::list filaments;
    for (size_t id : ids) {
        py::dict f;
        f["index"]       = id;
        f["model_mm3"]   = volume_of(ps.model_volumes_per_extruder, id);
        f["support_mm3"] = volume_of(ps.support_volumes_per_extruder, id);
        f["tower_mm3"]   = volume_of(ps.wipe_tower_volumes_per_extruder, id);
        f["flush_mm3"]   = volume_of(ps.flush_per_filament, id);
        f["total_mm3"]   = volume_of(ps.total_volumes_per_extruder, id);
        f["diameter"]    = id < result->filament_diameters.size() ? py::object(py::float_(result->filament_diameters[id])) : py::object(py::none());
        f["density"]     = id < result->filament_densities.size() ? py::object(py::float_(result->filament_densities[id])) : py::object(py::none());
        filaments.append(std::move(f));
    }

    py::dict out;
    out["plate_index"]                  = plate->get_index();
    out["filaments"]                    = std::move(filaments);
    out["total_filament_changes"]       = ps.total_filament_changes;
    out["total_flush_filament_changes"] = ps.total_flush_filament_changes;

    py::list changes;
    for (const auto& [pair, count] : transitions) {
        py::dict c;
        c["from"]  = pair.first < 0 ? py::object(py::none()) : py::object(py::int_(pair.first));
        c["to"]    = pair.second;
        c["count"] = count;
        changes.append(std::move(c));
    }
    out["transitions"] = std::move(changes);
    return out;
}

} // namespace

// Access to the live GUI application: the Plater and the module-level
// plater()/model()/preset_bundle() accessors. Everything here is owned by the
// app and only reachable once the GUI is up (the accessors throw before that).
void host_bindings::register_app(py::module_& host)
{
    py::class_<GUI::Plater, std::unique_ptr<GUI::Plater, py::nodelete>>(host, "Plater")
        .def("model", static_cast<Model& (GUI::Plater::*)()>(&GUI::Plater::model), py::return_value_policy::reference_internal)
        .def("is_project_dirty", &GUI::Plater::is_project_dirty)
        .def("is_presets_dirty", &GUI::Plater::is_presets_dirty)
        .def("inside_snapshot_capture", &GUI::Plater::inside_snapshot_capture)
        // Path the project was last opened from or saved to; empty while it has never been saved.
        .def("project_path", [](GUI::Plater& plater) {
            require_main_thread("project_path");
            return GUI::into_u8(plater.get_project_filename(".3mf"));
        })
        .def("export_3mf_copy", &export_3mf_copy, py::arg("path"),
             "Write the current project, unsaved changes included, to path as a 3MF copy. The project's "
             "file name and saved state are not changed, and the signed-in account is not written as the designer.");

    host.def("plater", &current_plater, py::return_value_policy::reference);
    host.def("model", []() -> Model& {
        return current_plater()->model();
    }, py::return_value_policy::reference);
    host.def("preset_bundle", &current_preset_bundle, py::return_value_policy::reference);
    host.def("app_info", &app_info,
             "The running app: version, build, mode, language, app_config_version (the version that wrote the app config "
             "before this run, empty when there was none), stealth_mode, is_signed_in (Orca Cloud), is_bambu_signed_in (Bambu Cloud), network_plugin_version (empty "
             "when the Bambu network plugin is not loaded) and network_plugin_error (empty unless it failed to load).");
    host.def("gl_info", &gl_info,
             "OpenGL vendor, renderer, version, glsl_version and core_profile of the 3D view; None before it has been created.");
    host.def("selected_printer", &selected_printer,
             "Agent, model_id, connection ('lan' or 'cloud'), online, connected and firmware of the printer selected in the "
             "device list; model_id and firmware are empty for non-Bambu agents. None when none is selected. Print hosts "
             "configured in the printer preset (OctoPrint and the like) are not in the device list.");
    host.def("slice_statistics", &slice_statistics, py::arg("plate_index") = -1,
             "Material usage of the last valid slice of a plate (plate_index -1 is the current plate), as the preview "
             "legend shows it: per filament (0-based index) the model, support, prime tower, flush and total volume in "
             "mm^3 plus diameter and density; total_filament_changes, total_flush_filament_changes and the filament "
             "changes in order as transitions (from, to, count; from is None for the first filament). None when the "
             "plate has no valid slice result.");
}

} // namespace Slic3r
