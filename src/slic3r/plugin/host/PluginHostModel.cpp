#include "PluginHostBindings.hpp"
#include "PluginHostMesh.hpp"
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/cast.h>
#include "slic3r/plugin/PluginBindingUtils.hpp"

#include <cstddef>
#include <libslic3r/Model.hpp>

#include <pybind11/stl.h>

#include <memory>
#include <string>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <pybind11/detail/common.h>
#include <pybind11/detail/descr.h>

namespace py = pybind11;

namespace Slic3r {

// The scene/document graph: Model -> ModelObject -> ModelInstance/ModelVolume.
// Everything is bound py::nodelete — non-owning references into a graph owned
// by the app (the live Plater model) or by a Print's model snapshot.
void host_bindings::register_model(py::module_& host)
{
    // Shared by id() on every model class.
    static constexpr const char* id_doc =
        "Positive integer ID, unique within this app session. A Print's model snapshot gives the same object, "
        "volume or instance the same ID, so it matches them to the live project.";

    py::enum_<ModelVolumeType>(host, "ModelVolumeType",
        "What a ModelVolume is: printed geometry, a settings or support modifier, or a precise seam helper. "
        "ModelVolume.type() returns it; prefer the is_*() checks on ModelVolume to comparing values.")
        .value("Invalid", ModelVolumeType::INVALID, "No valid type.")
        .value("ModelPart", ModelVolumeType::MODEL_PART, "Printed geometry.")
        .value("NegativeVolume", ModelVolumeType::NEGATIVE_VOLUME, "Subtracted from the object's model parts.")
        .value("ParameterModifier", ModelVolumeType::PARAMETER_MODIFIER,
               "Not printed; overrides settings where it overlaps the object's model parts.")
        .value("SupportBlocker", ModelVolumeType::SUPPORT_BLOCKER, "Not printed; keeps support out where it overlaps.")
        .value("SupportEnforcer", ModelVolumeType::SUPPORT_ENFORCER, "Not printed; forces support where it overlaps.")
        .value("PreciseSeamCenter", ModelVolumeType::PRECISE_SEAM_CENTER,
               "Strong precise seam helper: the seam goes at the middle of where it crosses the outer wall.")
        .value("PreciseSeamLeft", ModelVolumeType::PRECISE_SEAM_LEFT,
               "Strong precise seam helper: the seam goes at the first point where it crosses the outer wall.")
        .value("PreciseSeamRight", ModelVolumeType::PRECISE_SEAM_RIGHT,
               "Strong precise seam helper: the seam goes at the last point where it crosses the outer wall.")
        .value("PreciseSeamEnforced", ModelVolumeType::PRECISE_SEAM_ENFORCED,
               "Weak precise seam helper: marks the outer wall it crosses as a preferred seam area.")
        .value("PreciseSeamBlocked", ModelVolumeType::PRECISE_SEAM_BLOCKED,
               "Weak precise seam helper: keeps the seam off the outer wall it crosses.")
        .value("PreciseSeamNeutral", ModelVolumeType::PRECISE_SEAM_NEUTRAL,
               "Weak precise seam helper: resets the outer wall it crosses to neutral.")
        .value("PaintedModifier", ModelVolumeType::PAINTED_MODIFIER,
               "Not printed; overrides settings behind the area painted on one model part, to a set depth. It shares "
               "that part's mesh and placement, so its mesh is the whole part, not the painted area.");

    py::class_<ModelVolume, std::unique_ptr<ModelVolume, py::nodelete>>(host, "ModelVolume",
        "One volume of a ModelObject: a model part, a negative volume, a modifier, a support enforcer or blocker, or "
        "a precise seam helper (see type()). Its mesh is in the volume's local frame and matrix() places it in the "
        "object frame. A live reference into the project: fetch it again after the project changes.")
        .def("id", [](const ModelVolume& volume) { return volume.id().id; }, id_doc)
        .def_readonly("name", &ModelVolume::name, "Volume name, as the object list shows it.")
        .def("type", &ModelVolume::type, "The volume's ModelVolumeType.")
        .def("is_model_part", &ModelVolume::is_model_part, "True for a model part: printed geometry.")
        .def("is_modifier", &ModelVolume::is_modifier,
             "True for a settings modifier (ModelVolumeType.ParameterModifier) only, not for the other helper volumes.")
        .def("is_negative_volume", &ModelVolume::is_negative_volume,
             "True for a negative volume, which is subtracted from the object's model parts.")
        .def("is_support_enforcer", &ModelVolume::is_support_enforcer, "True for a support enforcer.")
        .def("is_support_blocker", &ModelVolume::is_support_blocker, "True for a support blocker.")
        .def("is_support_modifier", &ModelVolume::is_support_modifier, "True for a support enforcer or blocker.")
        .def("is_precise_seam", &ModelVolume::is_precise_seam,
             "True for any of the six precise seam helper types. Use these checks rather than ranges of "
             "ModelVolumeType values.")
        .def("is_precise_seam_strong", &ModelVolume::is_precise_seam_strong,
             "True for a strong precise seam helper (PreciseSeamCenter, PreciseSeamLeft or PreciseSeamRight), which "
             "puts the seam at one point.")
        .def("is_precise_seam_weak", &ModelVolume::is_precise_seam_weak,
             "True for a weak precise seam helper (PreciseSeamEnforced, PreciseSeamBlocked or PreciseSeamNeutral), "
             "which marks seam areas the way seam painting does.")
        .def("extruder_id", &ModelVolume::extruder_id,
             "1-based filament slot from the volume's own extruder setting, else the object's when the volume sets "
             "none or 0, else 1. Returned for every volume type, including those that print nothing; multi-material "
             "painting on the volume is not reflected.")
        .def("offset", [](const ModelVolume& volume) { return vec3_to_tuple(volume.get_offset()); },
             "Position (x, y, z) of the volume in its object's frame, in mm.")
        .def("rotation", [](const ModelVolume& volume) { return vec3_to_tuple(volume.get_rotation()); },
             "Rotation (rx, ry, rz) relative to the object frame, in radians, applied about X, then Y, then Z.")
        .def("scaling_factor", [](const ModelVolume& volume) { return vec3_to_tuple(volume.get_scaling_factor()); },
             "Scale (sx, sy, sz) relative to the object frame as positive factors, 1.0 meaning unscaled; "
             "mirror() reports mirroring.")
        .def("mirror", [](const ModelVolume& volume) { return vec3_to_tuple(volume.get_mirror()); },
             "Mirroring (mx, my, mz) relative to the object frame: -1.0 on each axis the volume is mirrored along, "
             "1.0 on the others.")
        .def("matrix", [](const ModelVolume& volume) { return mat4_to_numpy(volume.get_matrix()); },
            "Volume-to-object 4x4 float64 affine matrix (copy), in mm, applied to column vectors (x, y, z, 1). "
            "Requires numpy.")
        .def("facets_count", [](const ModelVolume& volume) { return volume.mesh().facets_count(); },
             "Number of triangles in the volume's mesh.")
        .def("volume", [](const ModelVolume& volume) { return volume.mesh().stats().volume; },
             "Volume in mm^3 that the volume's mesh encloses in its local frame, with no volume or instance scaling "
             "applied; -1 when it was never computed. Meaningful only for a closed mesh (see is_manifold()).")
        .def("bounding_box", [](const ModelVolume& volume) { return bbox_from_stats(volume.mesh().stats()); },
             "Bounding box of the volume's mesh in its local frame (volume transform not applied), in mm.")
        .def("is_manifold", [](const ModelVolume& volume) { return volume.mesh().stats().manifold(); },
             "True when the volume's mesh has no open edges, that is, it is closed.")
        .def("mesh", [](const ModelVolume& volume) {
            // The volume stores its mesh as shared_ptr<const TriangleMesh> (a
            // copy-on-write snapshot); the const_pointer_cast only serves the
            // binding's shared_ptr holder — the TriangleMesh binding exposes
            // read-only methods (see the immutability rule in PluginHostMesh.cpp).
            return std::const_pointer_cast<TriangleMesh>(volume.get_mesh_shared_ptr());
        }, "Return the volume's TriangleMesh (local coordinates) for vertex/triangle access.")
        .def("mesh_errors_count", [](const ModelVolume& volume) { return volume.get_repaired_errors_count(); },
             "Number of mesh errors recorded as repaired with the volume's mesh, for example in the 3MF it was "
             "loaded from: degenerate, removed and reversed facets and fixed and backwards edges, summed. 0 when "
             "none were recorded. Defects still present are not counted; see is_manifold().")
        .def("is_fdm_support_painted", &ModelVolume::is_fdm_support_painted,
             "True when the volume has support enforcer or blocker painting.")
        .def("is_seam_painted", &ModelVolume::is_seam_painted, "True when the volume has seam painting.")
        .def("is_mm_painted", &ModelVolume::is_mm_painted, "True when the volume has multi-material (colour) painting.")
        .def("is_fuzzy_skin_painted", &ModelVolume::is_fuzzy_skin_painted,
             "True when the volume has fuzzy skin painting.")
        .def("config_keys", [](const ModelVolume& volume) { return volume.config.keys(); },
             "Keys of the settings this volume overrides (its part or modifier settings); settings it inherits are "
             "not listed.")
        .def("config_value", [](const ModelVolume& volume, const std::string& key) {
            return config_value_or_none(volume.config.get(), key);
        }, py::arg("key"),
           "This volume's override of setting key, serialized as in a config file, or None when the volume does not "
           "override it.");

    py::class_<ModelInstance, std::unique_ptr<ModelInstance, py::nodelete>>(host, "ModelInstance",
        "One placement of a ModelObject: its object-to-world transform and its printable flag. Plates share one "
        "world space side by side, so world positions include the offset of the instance's plate. A live reference "
        "into the project: fetch it again after the project changes.")
        .def("id", [](const ModelInstance& instance) { return instance.id().id; }, id_doc)
        .def_readonly("printable", &ModelInstance::printable,
                      "Whether this instance is printable, as the object list's Printable toggle sets it; an instance "
                      "that is not printable is not sliced.")
        .def("is_printable", &ModelInstance::is_printable,
             "True when this instance gets sliced: the object and this instance are printable, and the app's last "
             "print-volume check, made against the current plate, found it fully inside the printable volume.")
        .def("offset", [](const ModelInstance& instance) { return vec3_to_tuple(instance.get_offset()); },
             "Position (x, y, z) of the object's origin in world coordinates, in mm.")
        .def("rotation", [](const ModelInstance& instance) { return vec3_to_tuple(instance.get_rotation()); },
             "Rotation (rx, ry, rz) in radians, applied about X, then Y, then Z.")
        .def("scaling_factor",
             [](const ModelInstance& instance) { return vec3_to_tuple(instance.get_scaling_factor()); },
             "Scale (sx, sy, sz) as positive factors, 1.0 meaning unscaled; mirror() reports mirroring.")
        .def("mirror", [](const ModelInstance& instance) { return vec3_to_tuple(instance.get_mirror()); },
             "Mirroring (mx, my, mz): -1.0 on each axis the instance is mirrored along, 1.0 on the others.")
        .def("matrix", [](const ModelInstance& instance) { return mat4_to_numpy(instance.get_matrix()); },
            "Object-to-world 4x4 float64 affine matrix (copy), in mm, applied to column vectors (x, y, z, 1). World "
            "vertices are instance.matrix() @ volume.matrix() applied to a volume's mesh vertices. Requires numpy.")
        .def("is_left_handed", &ModelInstance::is_left_handed,
             "True when the transform mirrors the object (negative determinant), which reverses triangle winding.")
        .def("is_assemble_initialized", [](ModelInstance& instance) { return instance.is_assemble_initialized(); },
             "True once the instance has its own Assemble-view transform, set from the project's assemble data or "
             "derived from the regular transform. Until then the assemble_*() values describe the identity transform.")
        .def("assemble_offset", [](const ModelInstance& instance) {
            return vec3_to_tuple(instance.get_assemble_transformation().get_offset());
        }, "Position (x, y, z) of the object in the Assemble view, in mm.")
        .def("assemble_rotation", [](const ModelInstance& instance) {
            return vec3_to_tuple(instance.get_assemble_transformation().get_rotation());
        }, "Rotation (rx, ry, rz) in the Assemble view, in radians, applied about X, then Y, then Z.")
        .def("assemble_matrix", [](const ModelInstance& instance) {
            return mat4_to_numpy(instance.get_assemble_transformation().get_matrix());
        }, "Assemble-view 4x4 float64 affine matrix (copy), in mm, applied to column vectors (x, y, z, 1). "
           "Requires numpy.")
        .def("offset_to_assembly", [](const ModelInstance& instance) {
            return vec3_to_tuple(instance.get_offset_to_assembly());
        }, "Offset (x, y, z) in mm of this object within the assembly it came from, which the Assemble view uses to "
           "explode the assembly. Set when a multi-part object is split into objects and kept in the project 3MF; "
           "(0, 0, 0) otherwise.")
        .def("bounding_box", [](ModelInstance& instance) {
            const ModelObject* object = instance.get_object();
            if (object == nullptr)
                return BoundingBoxf3();
            return object->instance_bounding_box(instance);
        }, "World-space bounding box of this instance's model parts, in mm, fitted to the transformed meshes. "
           "Modifiers and other helper volumes are left out.");

    py::class_<ModelObject, std::unique_ptr<ModelObject, py::nodelete>>(host, "ModelObject",
        "An object of the project: its volumes (parts, modifiers and other helpers) in the object frame, its "
        "instances placing it in the world, and its per-object settings. A live reference into the project: fetch it "
        "again after the project changes, since deleting the object frees it.")
        .def("id", [](const ModelObject& object) { return object.id().id; }, id_doc)
        .def_readonly("name", &ModelObject::name, "Object name, as the object list shows it.")
        .def_readonly("module_name", &ModelObject::module_name,
                      "Assembly module name from the project 3MF's object metadata; empty unless the file set one.")
        .def_readonly("input_file", &ModelObject::input_file,
                      "Path of the file the object was loaded from (a model file or the project 3MF); empty for an "
                      "object created in the app.")
        .def_readonly("printable", &ModelObject::printable,
                      "Object-level printable flag. The object list's Printable toggle sets ModelInstance.printable "
                      "instead and leaves this unchanged, so read the instances (printable, is_printable()) for the "
                      "effective state.")
        .def("instance_count", [](const ModelObject& object) {
            return object.instances.size();
        }, "Number of instances (copies) of the object; instance(index) takes 0 to instance_count() - 1.")
        .def("volume_count", [](const ModelObject& object) {
            return object.volumes.size();
        }, "Number of volumes of every type; volume(index) takes 0 to volume_count() - 1. parts_count() counts the "
           "model parts only.")
        .def("instances", [](ModelObject& object) {
            py::list instances;
            for (ModelInstance* instance : object.instances)
                instances.append(py::cast(instance, py::return_value_policy::reference));
            return instances;
        }, "All instances as a list of ModelInstance, in index order.")
        .def("instance", [](ModelObject& object, size_t index) -> ModelInstance* {
            if (index >= object.instances.size())
                throw py::index_error("instance index out of range");
            return object.instances[index];
        }, py::return_value_policy::reference_internal, py::arg("index"),
           "The instance at index, 0-based; raises IndexError when out of range.")
        .def("volumes", [](ModelObject& object) {
            py::list volumes;
            for (ModelVolume* volume : object.volumes)
                volumes.append(py::cast(volume, py::return_value_policy::reference));
            return volumes;
        }, "All volumes of every type as a list of ModelVolume, in index order.")
        .def("volume", [](ModelObject& object, size_t index) -> ModelVolume* {
            if (index >= object.volumes.size())
                throw py::index_error("volume index out of range");
            return object.volumes[index];
        }, py::return_value_policy::reference_internal, py::arg("index"),
           "The volume at index, 0-based, counting volumes of every type; raises IndexError when out of range.")
        .def("bounding_box", [](const ModelObject& object) { return object.bounding_box_exact(); },
             "World-space bounding box of the object's model parts over all its instances, in mm, fitted to the "
             "transformed meshes. Modifiers and other helper volumes are left out.")
        .def("raw_mesh_bounding_box", [](const ModelObject& object) { return object.raw_mesh_bounding_box(); },
             "Bounding box of the object's model parts in the object frame, in mm: volume transforms applied, "
             "instance transform not. Modifiers and other helper volumes are left out.")
        .def("min_z", &ModelObject::min_z,
             "Lowest world Z of the object's model parts, in mm, from the first instance's transform; all instances "
             "of an object share their Z range.")
        .def("max_z", &ModelObject::max_z,
             "Highest world Z of the object's model parts, in mm, from the first instance's transform; all instances "
             "of an object share their Z range.")
        .def("facets_count", [](const ModelObject& object) { return object.facets_count(); },
             "Total number of triangles in the object's model parts.")
        .def("parts_count", [](const ModelObject& object) { return object.parts_count(); },
             "Number of model parts (printed volumes); modifiers and other helper volumes are not counted.")
        .def("materials_count", [](const ModelObject& object) { return object.materials_count(); },
             "Number of distinct material IDs among the volumes, where volumes without one share the empty ID, so "
             "usually 1. These are materials an imported AMF file defines, not filaments; see "
             "ModelVolume.extruder_id().")
        .def("mesh_errors_count", [](const ModelObject& object) { return object.get_repaired_errors_count(); },
             "Sum of ModelVolume.mesh_errors_count() over all the object's volumes: mesh errors recorded as "
             "repaired, not defects still present.")
        .def("is_multiparts", &ModelObject::is_multiparts,
             "True when the object has more than one volume of any type, so a part with a modifier counts.")
        .def("is_cut", &ModelObject::is_cut,
             "True when the object is a piece of a Cut that is still linked to the other pieces of that cut.")
        .def("has_custom_layering", &ModelObject::has_custom_layering,
             "True when the object has layer height ranges or a variable layer height profile; the profile may still "
             "match the default layering.")
        .def("is_fdm_support_painted", &ModelObject::is_fdm_support_painted,
             "True when any of the object's volumes has support enforcer or blocker painting.")
        .def("is_seam_painted", &ModelObject::is_seam_painted,
             "True when any of the object's volumes has seam painting.")
        .def("is_mm_painted", &ModelObject::is_mm_painted,
             "True when any of the object's volumes has multi-material (colour) painting.")
        .def("is_fuzzy_skin_painted", &ModelObject::is_fuzzy_skin_painted,
             "True when any of the object's volumes has fuzzy skin painting.")
        .def("config_keys", [](const ModelObject& object) {
            return object.config.keys();
        }, "Keys of the settings this object overrides (its per-object settings); settings it inherits from the "
           "presets are not listed.")
        .def("config_value", [](const ModelObject& object, const std::string& key) {
            return config_value_or_none(object.config.get(), key);
        }, py::arg("key"),
           "This object's override of setting key, serialized as in a config file, or None when the object does not "
           "override it.");

    py::class_<Model, std::unique_ptr<Model, py::nodelete>>(host, "Model",
        "A project's scene: its objects, each with its volumes and instances, across all plates. orca.host.model() "
        "is the live project; a Print's model() is the snapshot that Print slices.")
        .def("id", [](const Model& model) { return model.id().id; }, id_doc)
        .def("object_count", [](const Model& model) {
            return model.objects.size();
        }, "Number of objects in the project; object(index) takes 0 to object_count() - 1.")
        .def("object", [](Model& model, size_t index) -> ModelObject* {
            if (index >= model.objects.size())
                throw py::index_error("model object index out of range");
            return model.objects[index];
        }, py::return_value_policy::reference_internal, py::arg("index"),
           "The object at index, 0-based in object-list order; raises IndexError when out of range.")
        .def("objects", [](Model& model) {
            py::list objects;
            for (ModelObject* object : model.objects)
                objects.append(py::cast(object, py::return_value_policy::reference));
            return objects;
        }, "All objects as a list of ModelObject, in index order.")
        .def("bounding_box", [](const Model& model) { return model.bounding_box_exact(); },
             "World-space bounding box of every instance's model parts on all plates, in mm, fitted to the "
             "transformed meshes.")
        .def("bounding_box_approx", [](const Model& model) { return model.bounding_box_approx(); },
             "Quicker, looser bounding_box(): it transforms each object's part box rather than its meshes, so it can "
             "be larger for rotated objects.")
        .def("max_z", &Model::max_z, "Highest world Z of any object's model parts, in mm; 0.0 for an empty project.")
        .def("material_count", [](const Model& model) { return model.materials.size(); },
             "Number of materials that imported AMF files defined in the model; 0 for most projects. Not the "
             "filament count.")
        .def("is_fdm_support_painted", &Model::is_fdm_support_painted,
             "True when any object has support enforcer or blocker painting.")
        .def("is_seam_painted", &Model::is_seam_painted, "True when any object has seam painting.")
        .def("is_mm_painted", &Model::is_mm_painted, "True when any object has multi-material (colour) painting.")
        .def("is_fuzzy_skin_painted", &Model::is_fuzzy_skin_painted, "True when any object has fuzzy skin painting.")
        .def("current_plate_index", [](const Model& model) { return model.curr_plate_index; },
             "0-based index of the selected plate.")
        .def("designer", [](const Model& model) {
            return model.design_info ? model.design_info->Designer : std::string();
        }, "Designer name from the metadata of the project 3MF the project was loaded from; empty when it has none.")
        .def("design_id", [](const Model& model) { return model.stl_design_id; },
             "Design ID of an online model the loaded files came from, read from 3MF metadata or a binary STL "
             "header; empty when they carry none.");
}

} // namespace Slic3r
