#include "FilamentSlotMutationTransaction.hpp"
#include "PluginHostTestSupport.hpp"

#include <pybind11/stl.h>

namespace py = pybind11;

namespace Slic3r::host_bindings {

void register_test_support(py::module_& host)
{
    py::class_<FilamentSlotMutationSnapshot>(host, "_TestFilamentSlotMappingSnapshot")
        .def_readonly("slot_index", &FilamentSlotMutationSnapshot::slot_index)
        .def_readonly("active_slot_name", &FilamentSlotMutationSnapshot::active_slot_name)
        .def_readonly("target_preset_name", &FilamentSlotMutationSnapshot::target_preset_name)
        .def_readonly("target_preset_file", &FilamentSlotMutationSnapshot::target_preset_file)
        .def_readonly("target_is_default", &FilamentSlotMutationSnapshot::target_is_default)
        .def_readonly("target_is_system", &FilamentSlotMutationSnapshot::target_is_system)
        .def_readonly("target_is_external", &FilamentSlotMutationSnapshot::target_is_external)
        .def_readonly("target_is_project_embedded", &FilamentSlotMutationSnapshot::target_is_project_embedded)
        .def_readonly("target_bundle_id", &FilamentSlotMutationSnapshot::target_bundle_id)
        .def_readonly("is_mixed", &FilamentSlotMutationSnapshot::is_mixed)
        .def_readonly("shared_slot_indices", &FilamentSlotMutationSnapshot::shared_slot_indices)
        .def_readonly("filament_presets", &FilamentSlotMutationSnapshot::filament_presets)
        .def_readonly("physical_filament_config_indices", &FilamentSlotMutationSnapshot::physical_filament_config_indices)
        .def_readonly("filament_is_mixed_present", &FilamentSlotMutationSnapshot::filament_is_mixed_present)
        .def_readonly("filament_is_mixed", &FilamentSlotMutationSnapshot::filament_is_mixed)
        .def_readonly("filament_map_present", &FilamentSlotMutationSnapshot::filament_map_present)
        .def_readonly("filament_map", &FilamentSlotMutationSnapshot::filament_map)
        .def_readonly("filament_nozzle_map_present", &FilamentSlotMutationSnapshot::filament_nozzle_map_present)
        .def_readonly("filament_nozzle_map", &FilamentSlotMutationSnapshot::filament_nozzle_map)
        .def_readonly("filament_volume_map_present", &FilamentSlotMutationSnapshot::filament_volume_map_present)
        .def_readonly("filament_volume_map", &FilamentSlotMutationSnapshot::filament_volume_map)
        .def_readonly("collection_selected_index", &FilamentSlotMutationSnapshot::collection_selected_index)
        .def_readonly("collection_selected_name", &FilamentSlotMutationSnapshot::collection_selected_name)
        .def_readonly("collection_edited_name", &FilamentSlotMutationSnapshot::collection_edited_name)
        .def_readonly("collection_selected_dirty", &FilamentSlotMutationSnapshot::collection_selected_dirty)
        .def_readonly("collection_edited_dirty", &FilamentSlotMutationSnapshot::collection_edited_dirty);

    host.def("_test_capture_filament_slot_mapping_snapshot", [](size_t slot_index) {
        const auto transaction = FilamentSlotMutationTransaction::capture_live(slot_index);
        return transaction->snapshot_for_test();
    }, "Test-build-only copied Filament mapping observation.");
}

} // namespace Slic3r::host_bindings
