#include <catch2/catch_all.hpp>

#include <libslic3r/Model.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <slic3r/plugin/PythonPluginBridge.hpp>

#include "python_test_support.hpp"

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <string>

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

TEST_CASE("Plugin host API reports unavailable GUI objects before Orca app initialization", "[PluginHost][Python]")
{
    py::object host = import_orca_module().attr("host");

    for (const char* function_name : { "preset_bundle", "plater", "model" }) {
        CAPTURE(function_name);
        try {
            host.attr(function_name)();
            FAIL("host accessor unexpectedly succeeded without a wx application");
        } catch (const py::error_already_set& error) {
            CHECK(error.matches(PyExc_RuntimeError));
            CHECK(std::string(error.what()).find("OrcaSlicer application is not initialized") != std::string::npos);
        }
    }
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

    // With no wx application the UI calls marshal to a main thread that does not
    // exist here; they must fail cleanly with a clear error, not crash.
    try {
        ui.attr("message")("hello");
        FAIL("orca.host.ui.message unexpectedly succeeded without a wx application");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_RuntimeError));
        CHECK(std::string(error.what()).find("OrcaSlicer application is not initialized") != std::string::npos);
    }
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
