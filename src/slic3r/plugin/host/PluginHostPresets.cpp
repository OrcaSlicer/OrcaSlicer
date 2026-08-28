#include "PluginHostBindings.hpp"
#include "PluginHostUi.hpp"
#include "slic3r/plugin/PluginBindingUtils.hpp"

#include <libslic3r/AppConfig.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PresetBundle.hpp>

#include <slic3r/GUI/GUI.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/MainFrame.hpp>
#include <slic3r/GUI/Plater.hpp>
#include <slic3r/GUI/Tab.hpp>

#include <pybind11/stl.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace Slic3r {
namespace {

py::list current_filament_presets(PresetBundle& bundle)
{
    py::list presets;
    for (const std::string& preset_name : bundle.filament_presets) {
        Preset* preset = bundle.filaments.find_preset(preset_name);
        if (preset == nullptr)
            presets.append(py::none());
        else
            presets.append(py::cast(preset, py::return_value_policy::reference));
    }
    return presets;
}

PresetCollection& printer_presets(PresetBundle& bundle)
{
    return static_cast<PresetCollection&>(bundle.printers);
}

void reload_local_bundle(const std::string& bundle_id)
{
    bool        reloaded = false;
    std::string error;
    PluginHostUi::run_on_ui_thread([&] {
        GUI::GUI_App& app = GUI::wxGetApp();
        if (app.is_closing())
            throw std::runtime_error("OrcaSlicer is closing");
        if (app.preset_bundle == nullptr)
            throw std::runtime_error("Preset bundle is not available");
        if (app.app_config == nullptr)
            throw std::runtime_error("Application configuration is not available");
        if (app.mainframe == nullptr)
            throw std::runtime_error("OrcaSlicer main window is not available");

        reloaded = app.preset_bundle->reload_local_bundle(app.app_config->get("preset_folder"), bundle_id, &error);
        if (!reloaded)
            return;

        app.mainframe->update_side_preset_ui();
        for (GUI::Tab* tab : app.tabs_list) {
            tab->reload_config();
            tab->update_changed_ui();
        }
        if (GUI::Plater* plater = app.plater())
            plater->sidebar().update_all_preset_comboboxes();
    });
    if (!reloaded)
        throw std::runtime_error(error.empty() ? "Local bundle was not reloaded" : error);
}

} // namespace

void host_bindings::register_presets(py::module_& host)
{
    host.def("reload_local_bundle", &reload_local_bundle, py::arg("bundle_id"),
             "Reload one local preset bundle from the active user's _local directory.");

    py::enum_<Preset::Type>(host, "PresetType")
        .value("Invalid", Preset::TYPE_INVALID)
        .value("Print", Preset::TYPE_PRINT)
        .value("SlaPrint", Preset::TYPE_SLA_PRINT)
        .value("Filament", Preset::TYPE_FILAMENT)
        .value("SlaMaterial", Preset::TYPE_SLA_MATERIAL)
        .value("Printer", Preset::TYPE_PRINTER)
        .value("PhysicalPrinter", Preset::TYPE_PHYSICAL_PRINTER)
        .value("Plate", Preset::TYPE_PLATE)
        .value("Model", Preset::TYPE_MODEL);

    py::class_<Preset, std::unique_ptr<Preset, py::nodelete>>(host, "Preset")
        .def_readonly("type", &Preset::type)
        .def_readonly("name", &Preset::name)
        .def_readonly("alias", &Preset::alias)
        .def_readonly("file", &Preset::file)
        .def_readonly("is_default", &Preset::is_default)
        .def_readonly("is_external", &Preset::is_external)
        .def_readonly("is_system", &Preset::is_system)
        .def_readonly("is_visible", &Preset::is_visible)
        .def_readonly("is_dirty", &Preset::is_dirty)
        .def_readonly("is_compatible", &Preset::is_compatible)
        .def_readonly("is_project_embedded", &Preset::is_project_embedded)
        .def_readonly("bundle_id", &Preset::bundle_id)
        .def("is_user", &Preset::is_user)
        .def("is_from_bundle", &Preset::is_from_bundle)
        .def("label", &Preset::label, py::arg("no_alias") = false)
        .def("config_keys", [](const Preset& preset) { return preset.config.keys(); })
        .def("config_value", [](const Preset& preset, const std::string& key) {
            return config_value_or_none(preset.config, key);
        });

    py::class_<PresetCollection, std::unique_ptr<PresetCollection, py::nodelete>>(host, "PresetCollection")
        .def("size", &PresetCollection::size)
        .def("get_selected_preset", [](PresetCollection& collection) -> Preset& {
            return collection.get_selected_preset();
        }, py::return_value_policy::reference_internal)
        .def("selected_preset", [](PresetCollection& collection) -> Preset& {
            return collection.get_selected_preset();
        }, py::return_value_policy::reference_internal)
        .def("get_selected_preset_name", &PresetCollection::get_selected_preset_name)
        .def("selected_preset_name", &PresetCollection::get_selected_preset_name)
        .def("get_edited_preset", [](PresetCollection& collection) -> Preset& {
            return collection.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("edited_preset", [](PresetCollection& collection) -> Preset& {
            return collection.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("preset", [](PresetCollection& collection, size_t index) -> Preset& {
            if (index >= collection.size())
                throw py::index_error("preset index out of range");
            return collection.preset(index);
        }, py::return_value_policy::reference_internal)
        .def("find_preset", [](PresetCollection& collection, const std::string& name) -> Preset* {
            return collection.find_preset(name);
        }, py::return_value_policy::reference_internal)
        .def("preset_names", [](const PresetCollection& collection) {
            std::vector<std::string> names;
            names.reserve(collection.get_presets().size());
            for (const Preset& preset : collection.get_presets())
                names.push_back(preset.name);
            return names;
        });

    py::class_<PresetBundle, std::unique_ptr<PresetBundle, py::nodelete>>(host, "PresetBundle")
        .def_property_readonly("prints", [](PresetBundle& bundle) -> PresetCollection& {
            return bundle.prints;
        }, py::return_value_policy::reference_internal)
        .def_property_readonly("printers", &printer_presets, py::return_value_policy::reference_internal)
        .def_property_readonly("filaments", [](PresetBundle& bundle) -> PresetCollection& {
            return bundle.filaments;
        }, py::return_value_policy::reference_internal)
        .def_property_readonly("sla_prints", [](PresetBundle& bundle) -> PresetCollection& {
            return bundle.sla_prints;
        }, py::return_value_policy::reference_internal)
        .def_property_readonly("sla_materials", [](PresetBundle& bundle) -> PresetCollection& {
            return bundle.sla_materials;
        }, py::return_value_policy::reference_internal)
        .def("current_process_preset", [](PresetBundle& bundle) -> Preset& {
            return bundle.prints.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("current_print_preset", [](PresetBundle& bundle) -> Preset& {
            return bundle.prints.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("current_printer_preset", [](PresetBundle& bundle) -> Preset& {
            return bundle.printers.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("current_filament_preset_names", [](PresetBundle& bundle) {
            return bundle.filament_presets;
        })
        .def("current_filament_presets", &current_filament_presets)
        .def("full_config_keys", [](const PresetBundle& bundle) {
            return bundle.full_config().keys();
        })
        .def("full_config_value", [](const PresetBundle& bundle, const std::string& key) {
            return config_value_or_none(bundle.full_config(), key);
        });
}

} // namespace Slic3r
