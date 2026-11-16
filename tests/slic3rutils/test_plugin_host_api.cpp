// why: keep the Windows headers ahead of libslic3r. Model.hpp pulls in Format/STEP.hpp, and
// OCCT's Standard_Macro.hxx defines NOUSER and NOGDI on Windows so that windows.h does not
// collide with OCCT's identifiers. Defined that early they also hide the USER and GDI
// declarations, and the wx MSW headers Field.hpp reaches (wx/wxprec.h -> wx/msw/private.h)
// need WNDPROC, RGB and the ::GetWindowRect family. Including windows.h first, as
// libslic3r_gui does through pchheader.hpp or win_platform.hpp, keeps them.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    #include <CommCtrl.h>
#endif

#include <catch2/catch_all.hpp>

#include <cstddef>
#include <libslic3r/Model.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <slic3r/GUI/DockPanel.hpp>
#include <slic3r/GUI/AuiPaneLayout.hpp>
#include <slic3r/GUI/OptionsGroup.hpp>
#include <slic3r/GUI/Widgets/WebHosting.hpp>

#include <map>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include "plugin_test_utils.hpp"
#include <pybind11/pytypes.h>
#include <pybind11/cast.h>
#include "python_test_support.hpp"

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <string>

#include <wx/string.h>
#include <wx/uri.h>
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <pybind11/detail/common.h>

namespace py = pybind11;

namespace {

// import_orca_module() lives in python_test_support.hpp (shared with
// test_slicing_pipeline_bindings.cpp).

bool has_attr(const py::handle& object, const char* name)
{
    return py::hasattr(object, name);
}

} // namespace

TEST_CASE("Plugin host API exposes host-owned bundle and preset surface to Python", "[PluginHost][Python]")
{
    py::module_ orca = import_orca_module();
    REQUIRE(has_attr(orca, "host"));

    py::object host = orca.attr("host");
    REQUIRE(has_attr(host, "PresetBundle"));
    REQUIRE(has_attr(host, "Preset"));
    REQUIRE(has_attr(host, "PresetCollection"));
    REQUIRE(has_attr(host, "Model"));
    REQUIRE(has_attr(host, "ModelObject"));
    REQUIRE(has_attr(host, "Plater"));
    REQUIRE(has_attr(host, "preset"));
    py::object preset = host.attr("preset");
    REQUIRE(has_attr(preset, "bundles"));
    py::object bundles = preset.attr("bundles");
    CHECK(has_attr(bundles, "list_ids"));
    CHECK(has_attr(bundles, "reload_local"));
    CHECK_FALSE(has_attr(host, "reload_local_bundle"));
    CHECK_FALSE(has_attr(host, "list_bundle_ids"));

    py::object preset_bundle_type = host.attr("PresetBundle");
    CHECK(has_attr(preset_bundle_type, "prints"));
    CHECK(has_attr(preset_bundle_type, "printers"));
    CHECK(has_attr(preset_bundle_type, "filaments"));
    CHECK(has_attr(preset_bundle_type, "current_process_preset"));
    CHECK(has_attr(preset_bundle_type, "current_printer_preset"));
    CHECK(has_attr(preset_bundle_type, "current_filament_preset_names"));
    CHECK(has_attr(preset_bundle_type, "current_filament_presets"));
    CHECK(has_attr(preset_bundle_type, "full_config_value"));

    py::object preset_collection_type = host.attr("PresetCollection");
    CHECK(has_attr(preset_collection_type, "get_edited_preset"));
    CHECK(has_attr(preset_collection_type, "get_selected_preset"));
    CHECK(has_attr(preset_collection_type, "get_selected_preset_name"));
    CHECK(has_attr(preset_collection_type, "edited_preset"));
    CHECK(has_attr(preset_collection_type, "selected_preset"));
    CHECK(has_attr(preset_collection_type, "selected_preset_name"));

    py::object preset_type = host.attr("Preset");
    CHECK(has_attr(preset_type, "name"));
    CHECK(has_attr(preset_type, "type"));
    CHECK(has_attr(preset_type, "is_default"));
    CHECK(has_attr(preset_type, "is_system"));
    CHECK(has_attr(preset_type, "is_user"));
    CHECK(has_attr(preset_type, "is_from_bundle"));
    CHECK(has_attr(preset_type, "config_value"));

    Slic3r::PresetBundle bundle;
    Slic3r::Preset&      printer_preset = bundle.printers.get_edited_preset();
    Slic3r::Preset&      process_preset = bundle.prints.get_edited_preset();
    Slic3r::Preset&      filament_preset = bundle.filaments.get_edited_preset();

    printer_preset.config.set("printer_model", "Plugin Host Test Printer", true);
    bundle.filament_presets = { filament_preset.name, filament_preset.name, "missing filament preset" };

    py::object py_bundle = py::cast(&bundle, py::return_value_policy::reference);

    CHECK(py_bundle.attr("current_printer_preset")().attr("name").cast<std::string>() == printer_preset.name);
    CHECK(py_bundle.attr("current_print_preset")().attr("name").cast<std::string>() == process_preset.name);
    CHECK(py_bundle.attr("current_process_preset")().attr("name").cast<std::string>() == process_preset.name);
    CHECK(py_bundle.attr("current_printer_preset")().attr("is_default").cast<bool>() == printer_preset.is_default);
    CHECK(py_bundle.attr("current_printer_preset")().attr("is_user")().cast<bool>() == printer_preset.is_user());
    CHECK(py_bundle.attr("current_printer_preset")().attr("config_value")("printer_model").cast<std::string>() == "Plugin Host Test Printer");
    CHECK(py_bundle.attr("current_printer_preset")().attr("config_value")("missing_test_key").is_none());

    py::list filament_names = py_bundle.attr("current_filament_preset_names")();
    REQUIRE(py::len(filament_names) == 3);
    CHECK(filament_names[0].cast<std::string>() == filament_preset.name);
    CHECK(filament_names[1].cast<std::string>() == filament_preset.name);
    CHECK(filament_names[2].cast<std::string>() == "missing filament preset");

    py::list filament_presets = py_bundle.attr("current_filament_presets")();
    REQUIRE(py::len(filament_presets) == 3);
    CHECK_FALSE(filament_presets[0].is_none());
    CHECK(filament_presets[0].attr("name").cast<std::string>() == filament_preset.name);
    CHECK_FALSE(filament_presets[1].is_none());
    CHECK(filament_presets[1].attr("name").cast<std::string>() == filament_preset.name);
    CHECK(filament_presets[2].is_none());

    py::object printers = py_bundle.attr("printers");
    py::object prints = py_bundle.attr("prints");
    py::object filaments = py_bundle.attr("filaments");
    CHECK(printers.attr("get_edited_preset")().attr("name").cast<std::string>() == printer_preset.name);
    CHECK(prints.attr("get_edited_preset")().attr("name").cast<std::string>() == process_preset.name);
    CHECK(filaments.attr("get_edited_preset")().attr("name").cast<std::string>() == filament_preset.name);
    CHECK(printers.attr("get_selected_preset_name")().cast<std::string>() == bundle.printers.get_selected_preset_name());
    CHECK(printers.attr("get_selected_preset")().attr("name").cast<std::string>() == bundle.printers.get_selected_preset().name);
    CHECK(printers.attr("selected_preset_name")().cast<std::string>() == bundle.printers.get_selected_preset_name());
    CHECK(printers.attr("edited_preset")().attr("name").cast<std::string>() == printer_preset.name);
    CHECK(printers.attr("find_preset")(printer_preset.name).attr("name").cast<std::string>() == printer_preset.name);
}

TEST_CASE("Plugin host API exposes the editable preset category handles and guards them before Orca app initialization",
          "[PluginHost][Python]")
{
    py::object host   = import_orca_module().attr("host");
    py::object preset = host.attr("preset");

    REQUIRE(has_attr(preset, "PresetEditor"));
    REQUIRE(has_attr(preset, "process"));
    REQUIRE(has_attr(preset, "printer"));
    REQUIRE(has_attr(preset, "filament"));

    py::object process  = preset.attr("process");
    py::object printer  = preset.attr("printer");
    py::object filament = preset.attr("filament");

    CHECK(process.attr("type").cast<Slic3r::Preset::Type>() == Slic3r::Preset::TYPE_PRINT);
    CHECK(printer.attr("type").cast<Slic3r::Preset::Type>() == Slic3r::Preset::TYPE_PRINTER);
    CHECK(filament.attr("type").cast<Slic3r::Preset::Type>() == Slic3r::Preset::TYPE_FILAMENT);
    for (const py::object& editor : { process, printer, filament })
        CHECK(has_attr(editor, "update"));

    // With no wx application the call must fail cleanly, like the rest of host.preset, and must not
    // be mistaken for a missing attribute or an argument error.
    for (const py::object& editor : { process, printer, filament }) {
        try {
            editor.attr("update")("layer_height", "0.25");
            FAIL("preset editor update unexpectedly succeeded without a wx application");
        } catch (const py::error_already_set& error) {
            CHECK(error.matches(PyExc_RuntimeError));
            CHECK(std::string(error.what()).find("OrcaSlicer application is not initialized") != std::string::npos);
        }
    }
}

TEST_CASE("Plugin host API exposes the filament slot form of preset update and guards it", "[PluginHost][Python]")
{
    py::object host   = import_orca_module().attr("host");
    py::object preset = host.attr("preset");

    // The filament editor accepts update(slot_number, key, value) as well as update(key, value); with
    // no wx application it reaches the same clean failure as the rest of host.preset. The slot
    // validation and the switch/cancel path need a running app, so they are not exercised here.
    try {
        preset.attr("filament").attr("update")(1, "layer_height", "0.25");
        FAIL("slot-addressed filament update unexpectedly succeeded without a wx application");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_RuntimeError));
        CHECK(std::string(error.what()).find("OrcaSlicer application is not initialized") != std::string::npos);
    }

    // The process and printer editors accept only update(key, value); the slot form is rejected
    // before any application access, naming the editor that does accept it.
    for (const char* name : { "process", "printer" }) {
        CAPTURE(name);
        try {
            preset.attr(name).attr("update")(1, "layer_height", "0.25");
            FAIL("slot-addressed update on a non-filament editor unexpectedly succeeded");
        } catch (const py::error_already_set& error) {
            CHECK(error.matches(PyExc_ValueError));
            CHECK(std::string(error.what()).find("only available on") != std::string::npos);
        }
    }
}

TEST_CASE("Plugin host API exposes staged preset update transactions", "[PluginHost][Python]")
{
    py::object preset = import_orca_module().attr("host").attr("preset");
    py::object transaction_type = preset.attr("PresetTransaction");
    py::object result_type = preset.attr("PresetUpdateResult");

    CHECK(has_attr(transaction_type, "set"));
    CHECK(has_attr(transaction_type, "commit"));
    CHECK(has_attr(result_type, "target_name"));
    CHECK(has_attr(result_type, "selection_changed"));
    CHECK(has_attr(result_type, "slot_assignment_changed"));
    CHECK(has_attr(result_type, "shared_slots"));

    for (const char* name : { "process", "printer" }) {
        CAPTURE(name);
        py::object editor = preset.attr(name);
        CHECK(has_attr(editor, "begin_update"));
        try {
            editor.attr("begin_update")();
            FAIL("begin_update unexpectedly succeeded without a wx application");
        } catch (const py::error_already_set& error) {
            CHECK(error.matches(PyExc_RuntimeError));
            CHECK(std::string(error.what()).find("OrcaSlicer application is not initialized") != std::string::npos);
        }
    }

    py::object filament = preset.attr("filament");
    CHECK(has_attr(filament, "begin_update"));
    try {
        filament.attr("begin_update")(1);
        FAIL("filament begin_update unexpectedly succeeded without a wx application");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_RuntimeError));
        CHECK(std::string(error.what()).find("OrcaSlicer application is not initialized") != std::string::npos);
    }
}

TEST_CASE("A settings group resolves an option's id from its option map", "[PluginHost]")
{
    using Slic3r::GUI::option_id_in_map;
    using Slic3r::GUI::t_opt_map;

    // The map ConfigOptionsGroup::get_option leaves: a scalar option, and a per-extruder option
    // whose entries carry their index.
    t_opt_map options;
    options.emplace("layer_height", std::make_pair(std::string("layer_height"), -1));
    options.emplace("nozzle_diameter#0", std::make_pair(std::string("nozzle_diameter"), 0));
    options.emplace("nozzle_diameter#1", std::make_pair(std::string("nozzle_diameter"), 1));

    // A negative index resolves a scalar option and yields its own (negative) index.
    int index = -1;
    CHECK(option_id_in_map(options, "layer_height", index) == "layer_height");
    CHECK(index == -1);

    // A negative index on a vector option matches several entries, so it resolves to nothing.
    index = -1;
    CHECK(option_id_in_map(options, "nozzle_diameter", index).empty());

    // An explicit index selects that entry.
    index = 1;
    CHECK(option_id_in_map(options, "nozzle_diameter", index) == "nozzle_diameter#1");
    CHECK(index == 1);

    // An index no entry carries, and a key the map does not own, resolve to nothing.
    index = 7;
    CHECK(option_id_in_map(options, "nozzle_diameter", index).empty());
    index = -1;
    CHECK(option_id_in_map(options, "wall_loops", index).empty());

    const t_opt_map empty;
    CHECK(option_id_in_map(empty, "layer_height", index).empty());
}

TEST_CASE("Plugin host API reports unavailable GUI objects before Orca app initialization", "[PluginHost][Python]")
{
    py::object host = import_orca_module().attr("host");
    py::object bundles = host.attr("preset").attr("bundles");

    for (const char* function_name : { "preset_bundle", "plater", "model", "app_info", "selected_printer" }) {
        CAPTURE(function_name);
        try {
            host.attr(function_name)();
            FAIL("host accessor unexpectedly succeeded without a wx application");
        } catch (const py::error_already_set& error) {
            CHECK(error.matches(PyExc_RuntimeError));
            CHECK(std::string(error.what()).find("OrcaSlicer application is not initialized") != std::string::npos);
        }
    }

    try {
        bundles.attr("reload_local")("test-bundle");
        FAIL("host.preset.bundles.reload_local unexpectedly succeeded without a wx application");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_RuntimeError));
        CHECK(std::string(error.what()).find("OrcaSlicer application is not initialized") != std::string::npos);
    }

    try {
        bundles.attr("list_ids")();
        FAIL("host.preset.bundles.list_ids unexpectedly succeeded without a wx application");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_RuntimeError));
        CHECK(std::string(error.what()).find("OrcaSlicer application is not initialized") != std::string::npos);
    }
}

TEST_CASE("Plugin host API reports no GL details before the 3D view exists", "[PluginHost][Python]")
{
    py::object host = import_orca_module().attr("host");

    CHECK(host.attr("gl_info")().is_none());

    py::object plater_type = host.attr("Plater");
    CHECK(has_attr(plater_type, "project_path"));
    CHECK(has_attr(plater_type, "export_3mf_copy"));
}

TEST_CASE("Plugin host API exposes the UI module and guards it before Orca app initialization", "[PluginHost][Python]")
{
    py::object host = import_orca_module().attr("host");
    REQUIRE(has_attr(host, "ui"));

    py::object ui = host.attr("ui");
    CHECK(has_attr(ui, "message"));
    CHECK_FALSE(has_attr(ui, "show_dialog"));
    CHECK(has_attr(ui, "create_window"));
    CHECK(has_attr(ui, "WINDOW_MODELESS"));
    CHECK(has_attr(ui, "WINDOW_MODAL"));
    CHECK(ui.attr("WINDOW_MODELESS").cast<long>() == 0);
    CHECK(ui.attr("WINDOW_MODAL").cast<long>() == 1);
    CHECK(has_attr(ui, "UiWindow"));
    CHECK(has_attr(ui, "create_dock_panel"));
    CHECK(has_attr(ui, "UiDockPanel"));

    // With no wx application the UI calls marshal to a main thread that does not
    // exist here; they must fail cleanly with a clear error, not crash.
    try {
        ui.attr("message")("hello");
        FAIL("orca.host.ui.message unexpectedly succeeded without a wx application");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_RuntimeError));
        CHECK(std::string(error.what()).find("OrcaSlicer application is not initialized") != std::string::npos);
    }

    try {
        ui.attr("create_dock_panel")("<p>panel</p>");
        FAIL("orca.host.ui.create_dock_panel unexpectedly succeeded without a wx application");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_RuntimeError));
        CHECK(std::string(error.what()).find("OrcaSlicer application is not initialized") != std::string::npos);
    }

    // Positional arguments follow create_window(): width and height come straight after the title.
    try {
        ui.attr("create_dock_panel")("<p>panel</p>", "Panel", 400, 300);
        FAIL("orca.host.ui.create_dock_panel unexpectedly succeeded without a wx application");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_RuntimeError));
    }

    // An unknown dock position is rejected before the application is needed.
    try {
        ui.attr("create_dock_panel")("<p>panel</p>", py::arg("dock") = "top");
        FAIL("orca.host.ui.create_dock_panel accepted an unknown dock position");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_ValueError));
    }
}

TEST_CASE("Plugin pane names identify the plugin and title without layout delimiters", "[PluginHost]")
{
    using Slic3r::GUI::plugin_pane_name;

    CHECK(plugin_pane_name("dock_demo", "Scene") == "plugin:dock_demo:Scene");
    CHECK(plugin_pane_name("key", "a|b;c=d\\e").find_first_of("|;=\\") == std::string::npos);
}

TEST_CASE("A pane's saved layout entry is found by pane name", "[PluginHost]")
{
    using Slic3r::GUI::aui_pane_layout_entry;

    const std::string sidebar = "name=sidebar;caption=;state=2099196;dir=4;layer=0;row=0;pos=0;bestw=390;besth=900";
    // The caption holds an escaped '|', which must not end the entry.
    const std::string plugin  = "name=plugin:demo:Scene;caption=Scene \\| stats;state=2099198;dir=2;layer=0;row=1;pos=0;bestw=320;besth=480";
    const std::string layout  = "layout3|" + sidebar + "|" + plugin + "|dock_size(4,0,0)=392|";

    CHECK(aui_pane_layout_entry(layout, "plugin:demo:Scene") == plugin);
    CHECK(aui_pane_layout_entry(layout, "sidebar") == sidebar);
    CHECK(aui_pane_layout_entry(layout, "plugin:demo").empty());
    CHECK(aui_pane_layout_entry("", "plugin:demo:Scene").empty());
}

TEST_CASE("A reloaded plugin page is recognised by its base URL, fragment aside", "[PluginHost]")
{
    using namespace Slic3r::GUI::web_hosting;

    // A resources path holding a space, which the web view may report escaped differently.
    const Slic3r::ScopedResourcesDir resources("web content check");

    // The swapped-in page, then after an in-page anchor and a reload.
    CHECK(is_content_url(content_base_url()));
    CHECK(is_content_url(content_base_url() + "#tab2"));
    const wxString unescaped = wxURI::Unescape(content_base_url());
    REQUIRE(unescaped != content_base_url());
    CHECK(is_content_url(unescaped));
    CHECK(is_content_url(unescaped + "#tab2"));
    // A page the plugin linked to keeps its own URL and must be left alone.
    CHECK_FALSE(is_content_url(content_base_url() + "guide.html"));
    CHECK_FALSE(is_content_url("https://example.com/"));
    CHECK_FALSE(is_content_url(""));
}

TEST_CASE("A reloaded plugin page is recognised when the resources path holds a '#'", "[PluginHost]")
{
    using namespace Slic3r::GUI::web_hosting;

    const Slic3r::ScopedResourcesDir resources("web#content check");

    CHECK(is_content_url(content_base_url()));
    CHECK(is_content_url(content_base_url() + "#tab2"));
    CHECK_FALSE(is_content_url(content_base_url() + "guide.html"));
}

TEST_CASE("Plugin host API exposes model geometry and structure to Python", "[PluginHost][Python]")
{
    using Catch::Matchers::WithinAbs;
    using Catch::Matchers::WithinRel;

    py::object host = import_orca_module().attr("host");
    REQUIRE(has_attr(host, "BoundingBox"));
    REQUIRE(has_attr(host, "Model"));
    REQUIRE(has_attr(host, "ModelInstance"));
    REQUIRE(has_attr(host, "ModelVolume"));
    REQUIRE(has_attr(host, "ModelVolumeType"));

    py::object volume_type_enum = host.attr("ModelVolumeType");
    CHECK(has_attr(volume_type_enum, "ModelPart"));
    CHECK(has_attr(volume_type_enum, "ParameterModifier"));
    CHECK(has_attr(volume_type_enum, "SupportEnforcer"));

    // Build a model in C++: one object with a 10x20x30 mm printable part, a small
    // modifier volume, and a single instance shifted on the bed.
    Slic3r::Model        model;
    Slic3r::ModelObject* object = model.add_object();
    object->name                = "Plugin Host Test Cube";

    Slic3r::ModelVolume* part = object->add_volume(Slic3r::make_cube(10.0, 20.0, 30.0));
    part->name                = "cube part";
    Slic3r::ModelVolume* modifier = object->add_volume(Slic3r::make_cube(2.0, 2.0, 2.0),
                                                       Slic3r::ModelVolumeType::PARAMETER_MODIFIER);
    modifier->name = "fit modifier";

    Slic3r::ModelInstance* instance = object->add_instance();
    instance->set_offset(Slic3r::Vec3d(5.0, 6.0, 0.0));

    py::object py_model = py::cast(&model, py::return_value_policy::reference);

    // Model surface.
    CHECK(py_model.attr("object_count")().cast<size_t>() == 1);
    CHECK(py_model.attr("id")().cast<size_t>() == model.id().id);
    CHECK(py_model.attr("bounding_box")().attr("defined").cast<bool>());

    // Object surface.
    py::object py_object = py_model.attr("object")(0);
    CHECK(py_object.attr("name").cast<std::string>() == "Plugin Host Test Cube");
    CHECK(py_object.attr("id")().cast<size_t>() == object->id().id);
    CHECK(py_object.attr("instance_count")().cast<size_t>() == 1);
    CHECK(py_object.attr("volume_count")().cast<size_t>() == 2);
    CHECK(py::len(py_object.attr("instances")()) == 1);
    CHECK(py::len(py_object.attr("volumes")()) == 2);
    CHECK(py_object.attr("is_multiparts")().cast<bool>());

    // Intrinsic (untransformed) object size must match the printable part's dimensions.
    py::object obj_size = py_object.attr("raw_mesh_bounding_box")().attr("size");
    REQUIRE_THAT(obj_size[py::int_(0)].cast<double>(), WithinAbs(10.0, 1e-3));
    REQUIRE_THAT(obj_size[py::int_(1)].cast<double>(), WithinAbs(20.0, 1e-3));
    REQUIRE_THAT(obj_size[py::int_(2)].cast<double>(), WithinAbs(30.0, 1e-3));

    // Instance surface.
    py::object py_instance = py_object.attr("instance")(0);
    py::object inst_offset = py_instance.attr("offset")();
    REQUIRE_THAT(inst_offset[py::int_(0)].cast<double>(), WithinAbs(5.0, 1e-6));
    REQUIRE_THAT(inst_offset[py::int_(1)].cast<double>(), WithinAbs(6.0, 1e-6));
    REQUIRE_THAT(inst_offset[py::int_(2)].cast<double>(), WithinAbs(0.0, 1e-6));
    CHECK(py_instance.attr("id")().cast<size_t>() == instance->id().id);

    // Volume surface — part.
    py::object py_part = py_object.attr("volume")(0);
    CHECK(py_part.attr("name").cast<std::string>() == "cube part");
    CHECK(py_part.attr("is_model_part")().cast<bool>());
    CHECK_FALSE(py_part.attr("is_modifier")().cast<bool>());
    CHECK(py_part.attr("type")().cast<Slic3r::ModelVolumeType>() == Slic3r::ModelVolumeType::MODEL_PART);
    CHECK(py_part.attr("facets_count")().cast<size_t>() == 12);
    REQUIRE_THAT(py_part.attr("volume")().cast<double>(), WithinRel(6000.0, 1e-2));

    // Volume surface — modifier.
    py::object py_modifier = py_object.attr("volume")(1);
    CHECK(py_modifier.attr("is_modifier")().cast<bool>());
    CHECK_FALSE(py_modifier.attr("is_model_part")().cast<bool>());
    CHECK(py_modifier.attr("type")().cast<Slic3r::ModelVolumeType>() == Slic3r::ModelVolumeType::PARAMETER_MODIFIER);
}

TEST_CASE("Plugin host API exposes TriangleMesh geometry to Python", "[PluginHost][Python]")
{
    using Catch::Matchers::WithinAbs;
    using Catch::Matchers::WithinRel;

    py::object host = import_orca_module().attr("host");
    REQUIRE(has_attr(host, "TriangleMesh"));

    py::object mesh_type = host.attr("TriangleMesh");
    for (const char* member : { "vertex_count", "triangle_count", "facets_count", "is_empty",
                                "vertices", "triangles", "face_normals", "vertex", "triangle",
                                "volume", "bounding_box", "is_manifold" }) {
        CAPTURE(member);
        CHECK(has_attr(mesh_type, member));
    }

    // A 10 x 20 x 30 mm box: 8 vertices, 12 triangles.
    Slic3r::Model        model;
    Slic3r::ModelObject* object = model.add_object();
    object->add_volume(Slic3r::make_cube(10.0, 20.0, 30.0));
    Slic3r::ModelInstance* instance = object->add_instance();
    instance->set_offset(Slic3r::Vec3d(5.0, 6.0, 0.0));

    py::object py_object = py::cast(object, py::return_value_policy::reference);
    py::object py_volume = py_object.attr("volume")(0);
    py::object mesh      = py_volume.attr("mesh")();

    // Deterministic, numpy-free surface.
    CHECK(mesh.attr("vertex_count")().cast<size_t>() == 8);
    CHECK(mesh.attr("triangle_count")().cast<size_t>() == 12);
    CHECK(mesh.attr("facets_count")().cast<size_t>() == 12);
    CHECK_FALSE(mesh.attr("is_empty")().cast<bool>());
    CHECK(mesh.attr("is_manifold")().cast<bool>());
    REQUIRE_THAT(mesh.attr("volume")().cast<double>(), WithinRel(6000.0, 1e-2));

    py::object bbox_size = mesh.attr("bounding_box")().attr("size");
    REQUIRE_THAT(bbox_size[py::int_(0)].cast<double>(), WithinAbs(10.0, 1e-3));
    REQUIRE_THAT(bbox_size[py::int_(1)].cast<double>(), WithinAbs(20.0, 1e-3));
    REQUIRE_THAT(bbox_size[py::int_(2)].cast<double>(), WithinAbs(30.0, 1e-3));

    py::object vertex0 = mesh.attr("vertex")(0);
    REQUIRE(py::len(vertex0) == 3);
    py::object triangle0 = mesh.attr("triangle")(0);
    REQUIRE(py::len(triangle0) == 3);
    for (int k = 0; k < 3; ++k) {
        int idx = triangle0[py::int_(k)].cast<int>();
        CHECK(idx >= 0);
        CHECK(idx < 8);
    }
    CHECK_THROWS_AS(mesh.attr("vertex")(8), py::error_already_set);
    CHECK_THROWS_AS(mesh.attr("triangle")(12), py::error_already_set);

    // numpy path: exercised when numpy is importable, otherwise assert the clear
    // "numpy required" error so the absent path is itself covered.
    bool have_numpy = false;
    try {
        py::module_::import("numpy");
        have_numpy = true;
    } catch (const py::error_already_set&) {
        have_numpy = false;
    }

    if (!have_numpy) {
        WARN("numpy unavailable in unit-test interpreter; asserting the numpy-absent error path");
        try {
            mesh.attr("vertices")();
            FAIL("vertices() must raise ImportError when numpy is unavailable");
        } catch (const py::error_already_set& error) {
            CHECK(error.matches(PyExc_ImportError));
            CHECK(std::string(error.what()).find("numpy is required") != std::string::npos);
        }
        return;
    }

    py::object vertices = mesh.attr("vertices")();
    CHECK(vertices.attr("shape").cast<py::tuple>()[py::int_(0)].cast<size_t>() == 8);
    CHECK(vertices.attr("shape").cast<py::tuple>()[py::int_(1)].cast<size_t>() == 3);
    CHECK(vertices.attr("dtype").attr("name").cast<std::string>() == "float32");
    CHECK_FALSE(vertices.attr("flags").attr("writeable").cast<bool>());
    CHECK_FALSE(vertices.attr("base").is_none()); // zero-copy view keeps an owner alive
    CHECK_THROWS_AS(vertices.attr("__setitem__")(py::make_tuple(0, 0), py::float_(1.0)), py::error_already_set);

    py::object triangles = mesh.attr("triangles")();
    CHECK(triangles.attr("shape").cast<py::tuple>()[py::int_(0)].cast<size_t>() == 12);
    CHECK(triangles.attr("shape").cast<py::tuple>()[py::int_(1)].cast<size_t>() == 3);
    CHECK(triangles.attr("dtype").attr("name").cast<std::string>() == "int32");
    CHECK_FALSE(triangles.attr("flags").attr("writeable").cast<bool>());

    py::object face_normals = mesh.attr("face_normals")();
    CHECK(face_normals.attr("shape").cast<py::tuple>()[py::int_(0)].cast<size_t>() == 12);
    CHECK(face_normals.attr("dtype").attr("name").cast<std::string>() == "float32");

    // World-space transform matrices.
    py::object volume_matrix = py_volume.attr("matrix")();
    CHECK(volume_matrix.attr("shape").cast<py::tuple>()[py::int_(0)].cast<size_t>() == 4);
    CHECK(volume_matrix.attr("shape").cast<py::tuple>()[py::int_(1)].cast<size_t>() == 4);
    CHECK(volume_matrix.attr("dtype").attr("name").cast<std::string>() == "float64");

    py::object instance_matrix = py_object.attr("instance")(0).attr("matrix")();
    CHECK(instance_matrix.attr("shape").cast<py::tuple>()[py::int_(0)].cast<size_t>() == 4);
    // Instance offset (5, 6, 0) must land in the matrix translation column.
    REQUIRE_THAT(instance_matrix.attr("__getitem__")(py::make_tuple(0, 3)).cast<double>(), WithinAbs(5.0, 1e-6));
    REQUIRE_THAT(instance_matrix.attr("__getitem__")(py::make_tuple(1, 3)).cast<double>(), WithinAbs(6.0, 1e-6));
}
