#include "PluginHostBindings.hpp"

#include <libslic3r/Model.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <slic3r/GUI/GUI.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/MainFrame.hpp>
#include <slic3r/GUI/Plater.hpp>

#include <memory>
#include <stdexcept>

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
        .def("inside_snapshot_capture", &GUI::Plater::inside_snapshot_capture);

    host.def("plater", &current_plater, py::return_value_policy::reference);
    host.def("model", []() -> Model& {
        return current_plater()->model();
    }, py::return_value_policy::reference);
    host.def("preset_bundle", &current_preset_bundle, py::return_value_policy::reference);
    // UI language of the running app ("en_US", "ru_RU", ...), so plugins can
    // localize their own dialogs. The app config file that stores this value
    // is deny-listed by the audit hook (it sits next to cloud secrets), so a
    // read-only accessor is the supported way to get just the language.
    host.def("app_language", []() -> std::string {
        if (wxTheApp == nullptr)
            throw std::runtime_error("OrcaSlicer application is not initialized");
        return GUI::into_u8(GUI::wxGetApp().current_language_code_safe());
    });
    // Re-read user preset files from disk and refresh the filament dropdown,
    // without restarting the app. Plugins that write preset/spool files (see
    // preset_bundle().filaments) otherwise require a restart to make them
    // visible. This mirrors exactly what OrcaCloud sync does after pulling
    // presets (GUI_App: load_user_presets + update_side_preset_ui); the loader
    // preserves the currently selected presets.
    host.def("reload_presets", []() {
        if (wxTheApp == nullptr)
            throw std::runtime_error("OrcaSlicer application is not initialized");
        auto& app = GUI::wxGetApp();
        if (app.preset_bundle == nullptr)
            throw std::runtime_error("Preset bundle is not available");
        app.preset_bundle->load_user_presets(DEFAULT_USER_FOLDER_NAME,
                                             ForwardCompatibilitySubstitutionRule::Enable);
        if (app.mainframe != nullptr)
            app.mainframe->update_side_preset_ui();
    });
}

} // namespace Slic3r
