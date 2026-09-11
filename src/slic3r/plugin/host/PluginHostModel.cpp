#include "PluginHostBindings.hpp"
#include "PluginHostMesh.hpp"
#include "slic3r/plugin/PluginBindingUtils.hpp"

#include <libslic3r/Model.hpp>

#include <pybind11/stl.h>

#include <memory>
#include <string>

namespace py = pybind11;

namespace Slic3r {

// The scene/document graph: Model -> ModelObject -> ModelInstance/ModelVolume.
// Everything is bound py::nodelete — non-owning references into a graph owned
// by the app (the live Plater model) or by a Print's model snapshot.
void host_bindings::register_model(py::module_& host)
{
    py::enum_<ModelVolumeType>(host, "ModelVolumeType")
        .value("Invalid", ModelVolumeType::INVALID)
        .value("ModelPart", ModelVolumeType::MODEL_PART)
        .value("NegativeVolume", ModelVolumeType::NEGATIVE_VOLUME)
        .value("ParameterModifier", ModelVolumeType::PARAMETER_MODIFIER)
        .value("SupportBlocker", ModelVolumeType::SUPPORT_BLOCKER)
        .value("SupportEnforcer", ModelVolumeType::SUPPORT_ENFORCER);

    py::class_<ModelVolume, std::unique_ptr<ModelVolume, py::nodelete>>(host, "ModelVolume")
        .def("id", [](const ModelVolume& volume) { return volume.id().id; })
        .def_readonly("name", &ModelVolume::name)
        .def("type", &ModelVolume::type)
        .def("is_model_part", &ModelVolume::is_model_part)
        .def("is_modifier", &ModelVolume::is_modifier)
        .def("is_negative_volume", &ModelVolume::is_negative_volume)
        .def("is_support_enforcer", &ModelVolume::is_support_enforcer)
        .def("is_support_blocker", &ModelVolume::is_support_blocker)
        .def("is_support_modifier", &ModelVolume::is_support_modifier)
        // Extruder ID is 1-based for FFF, -1 for SLA or support volumes.
        .def("extruder_id", &ModelVolume::extruder_id)
        .def("offset", [](const ModelVolume& volume) { return vec3_to_tuple(volume.get_offset()); })
        .def("rotation", [](const ModelVolume& volume) { return vec3_to_tuple(volume.get_rotation()); })
        .def("scaling_factor", [](const ModelVolume& volume) { return vec3_to_tuple(volume.get_scaling_factor()); })
        .def("mirror", [](const ModelVolume& volume) { return vec3_to_tuple(volume.get_mirror()); })
        // 4x4 float64 affine matrix mapping this volume into its parent object frame. Requires numpy.
        .def("matrix", [](const ModelVolume& volume) { return mat4_to_numpy(volume.get_matrix()); },
            "Volume-to-object 4x4 float64 affine matrix (copy). Requires numpy.")
        .def("facets_count", [](const ModelVolume& volume) { return volume.mesh().facets_count(); })
        // Raw (untransformed) mesh volume in mm^3; -1 if it was never computed.
        .def("volume", [](const ModelVolume& volume) { return volume.mesh().stats().volume; })
        // Bounding box of the raw (untransformed) mesh, in the volume's local frame.
        .def("bounding_box", [](const ModelVolume& volume) { return bbox_from_stats(volume.mesh().stats()); })
        .def("is_manifold", [](const ModelVolume& volume) { return volume.mesh().stats().manifold(); })
        // Full mesh geometry (vertices/triangles) as an immutable snapshot.
        .def("mesh", [](const ModelVolume& volume) {
            // The volume stores its mesh as shared_ptr<const TriangleMesh> (a
            // copy-on-write snapshot); the const_pointer_cast only serves the
            // binding's shared_ptr holder — the TriangleMesh binding exposes
            // read-only methods (see the immutability rule in PluginHostMesh.cpp).
            return std::const_pointer_cast<TriangleMesh>(volume.get_mesh_shared_ptr());
        }, "Return the volume's TriangleMesh (local coordinates) for vertex/triangle access.")
        .def("mesh_errors_count", [](const ModelVolume& volume) { return volume.get_repaired_errors_count(); })
        .def("is_fdm_support_painted", &ModelVolume::is_fdm_support_painted)
        .def("is_seam_painted", &ModelVolume::is_seam_painted)
        .def("is_mm_painted", &ModelVolume::is_mm_painted)
        .def("is_fuzzy_skin_painted", &ModelVolume::is_fuzzy_skin_painted)
        .def("config_keys", [](const ModelVolume& volume) { return volume.config.keys(); })
        .def("config_value", [](const ModelVolume& volume, const std::string& key) {
            return config_value_or_none(volume.config.get(), key);
        });

    py::class_<ModelInstance, std::unique_ptr<ModelInstance, py::nodelete>>(host, "ModelInstance")
        .def("id", [](const ModelInstance& instance) { return instance.id().id; })
        .def_readonly("printable", &ModelInstance::printable)
        // True only if the object is printable, this instance is printable and it
        // currently sits fully inside the print volume (set during slicing).
        .def("is_printable", &ModelInstance::is_printable)
        .def("offset", [](const ModelInstance& instance) { return vec3_to_tuple(instance.get_offset()); })
        .def("rotation", [](const ModelInstance& instance) { return vec3_to_tuple(instance.get_rotation()); })
        .def("scaling_factor", [](const ModelInstance& instance) { return vec3_to_tuple(instance.get_scaling_factor()); })
        .def("mirror", [](const ModelInstance& instance) { return vec3_to_tuple(instance.get_mirror()); })
        // 4x4 float64 affine matrix mapping the object into world space. Requires numpy.
        // World vertices = instance.matrix() @ volume.matrix() applied to mesh vertices.
        .def("matrix", [](const ModelInstance& instance) { return mat4_to_numpy(instance.get_matrix()); },
            "Object-to-world 4x4 float64 affine matrix (copy). Requires numpy.")
        .def("is_left_handed", &ModelInstance::is_left_handed)
        // Assemble-view placement. Each instance carries a second transform used only by
        // the Assemble view, set from stored 3mf assemble data or derived from the regular
        // transform. Until then (is_assemble_initialized() false) it is identity.
        .def("is_assemble_initialized", [](ModelInstance& instance) { return instance.is_assemble_initialized(); })
        .def("assemble_offset", [](const ModelInstance& instance) {
            return vec3_to_tuple(instance.get_assemble_transformation().get_offset());
        })
        .def("assemble_rotation", [](const ModelInstance& instance) {
            return vec3_to_tuple(instance.get_assemble_transformation().get_rotation());
        })
        // 4x4 float64 affine matrix placing the object in the Assemble view. Requires numpy.
        .def("assemble_matrix", [](const ModelInstance& instance) {
            return mat4_to_numpy(instance.get_assemble_transformation().get_matrix());
        }, "Assemble-view 4x4 float64 affine matrix (copy). Requires numpy.")
        // Offset from the instance origin to its position within the source assembly,
        // recorded at import time (e.g. from a STEP assembly).
        .def("offset_to_assembly", [](const ModelInstance& instance) {
            return vec3_to_tuple(instance.get_offset_to_assembly());
        })
        // World-space bounding box of this instance.
        .def("bounding_box", [](ModelInstance& instance) {
            const ModelObject* object = instance.get_object();
            if (object == nullptr)
                return BoundingBoxf3();
            return object->instance_bounding_box(instance);
        });

    py::class_<ModelObject, std::unique_ptr<ModelObject, py::nodelete>>(host, "ModelObject")
        .def("id", [](const ModelObject& object) { return object.id().id; })
        .def_readonly("name", &ModelObject::name)
        .def_readonly("module_name", &ModelObject::module_name)
        .def_readonly("input_file", &ModelObject::input_file)
        // Import-time flag only: the GUI's printable toggle writes the per-instance
        // ModelInstance::printable and never updates this field, so derive an
        // object's effective state from its instances.
        .def_readonly("printable", &ModelObject::printable)
        .def("instance_count", [](const ModelObject& object) {
            return object.instances.size();
        })
        .def("volume_count", [](const ModelObject& object) {
            return object.volumes.size();
        })
        .def("instances", [](ModelObject& object) {
            py::list instances;
            for (ModelInstance* instance : object.instances)
                instances.append(py::cast(instance, py::return_value_policy::reference));
            return instances;
        })
        .def("instance", [](ModelObject& object, size_t index) -> ModelInstance* {
            if (index >= object.instances.size())
                throw py::index_error("instance index out of range");
            return object.instances[index];
        }, py::return_value_policy::reference_internal)
        .def("volumes", [](ModelObject& object) {
            py::list volumes;
            for (ModelVolume* volume : object.volumes)
                volumes.append(py::cast(volume, py::return_value_policy::reference));
            return volumes;
        })
        .def("volume", [](ModelObject& object, size_t index) -> ModelVolume* {
            if (index >= object.volumes.size())
                throw py::index_error("volume index out of range");
            return object.volumes[index];
        }, py::return_value_policy::reference_internal)
        // World-space bounding box over all instances of this object.
        .def("bounding_box", [](const ModelObject& object) { return object.bounding_box_exact(); })
        // Bounding box of the object's raw (untransformed) part meshes — its intrinsic size.
        .def("raw_mesh_bounding_box", [](const ModelObject& object) { return object.raw_mesh_bounding_box(); })
        .def("min_z", &ModelObject::min_z)
        .def("max_z", &ModelObject::max_z)
        .def("facets_count", [](const ModelObject& object) { return object.facets_count(); })
        .def("parts_count", [](const ModelObject& object) { return object.parts_count(); })
        .def("materials_count", [](const ModelObject& object) { return object.materials_count(); })
        .def("mesh_errors_count", [](const ModelObject& object) { return object.get_repaired_errors_count(); })
        .def("is_multiparts", &ModelObject::is_multiparts)
        .def("is_cut", &ModelObject::is_cut)
        .def("has_custom_layering", &ModelObject::has_custom_layering)
        .def("is_fdm_support_painted", &ModelObject::is_fdm_support_painted)
        .def("is_seam_painted", &ModelObject::is_seam_painted)
        .def("is_mm_painted", &ModelObject::is_mm_painted)
        .def("is_fuzzy_skin_painted", &ModelObject::is_fuzzy_skin_painted)
        .def("config_keys", [](const ModelObject& object) {
            return object.config.keys();
        })
        .def("config_value", [](const ModelObject& object, const std::string& key) {
            return config_value_or_none(object.config.get(), key);
        });

    py::class_<Model, std::unique_ptr<Model, py::nodelete>>(host, "Model")
        .def("id", [](const Model& model) { return model.id().id; })
        .def("object_count", [](const Model& model) {
            return model.objects.size();
        })
        .def("object", [](Model& model, size_t index) -> ModelObject* {
            if (index >= model.objects.size())
                throw py::index_error("model object index out of range");
            return model.objects[index];
        }, py::return_value_policy::reference_internal)
        .def("objects", [](Model& model) {
            py::list objects;
            for (ModelObject* object : model.objects)
                objects.append(py::cast(object, py::return_value_policy::reference));
            return objects;
        })
        // World-space bounding box of the whole model. bounding_box() is exact;
        // bounding_box_approx() is faster and cached.
        .def("bounding_box", [](const Model& model) { return model.bounding_box_exact(); })
        .def("bounding_box_approx", [](const Model& model) { return model.bounding_box_approx(); })
        .def("max_z", &Model::max_z)
        .def("material_count", [](const Model& model) { return model.materials.size(); })
        .def("is_fdm_support_painted", &Model::is_fdm_support_painted)
        .def("is_seam_painted", &Model::is_seam_painted)
        .def("is_mm_painted", &Model::is_mm_painted)
        .def("is_fuzzy_skin_painted", &Model::is_fuzzy_skin_painted)
        .def("current_plate_index", [](const Model& model) { return model.curr_plate_index; })
        .def("designer", [](const Model& model) {
            return model.design_info ? model.design_info->Designer : std::string();
        })
        .def("design_id", [](const Model& model) { return model.stl_design_id; });
}

} // namespace Slic3r
