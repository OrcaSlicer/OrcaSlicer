#include "PersistentPresetMutationTransaction.hpp"
#include "FilamentSlotMutationTransaction.hpp"
#include "PluginHostFeatureContracts.hpp"
#include "PluginHostBindings.hpp"

#include <pybind11/stl.h>

namespace py = pybind11;

namespace Slic3r::host_bindings {

void register_persistent_mutation(py::module_& host)
{
    py::enum_<PersistentPresetMutationState>(host, "PersistentPresetMutationState")
        .value("Ready", PersistentPresetMutationState::Ready)
        .value("Committed", PersistentPresetMutationState::Committed)
        .value("RolledBack", PersistentPresetMutationState::RolledBack)
        .value("FailedRecovery", PersistentPresetMutationState::FailedRecovery);

    py::class_<PersistentPresetMutationResult>(host, "PersistentPresetMutationResult")
        .def_readonly("state", &PersistentPresetMutationResult::state)
        .def_readonly("committed", &PersistentPresetMutationResult::committed)
        .def_readonly("rollback_attempted", &PersistentPresetMutationResult::rollback_attempted)
        .def_readonly("rollback_verified", &PersistentPresetMutationResult::rollback_verified)
        .def_readonly("selection_changed", &PersistentPresetMutationResult::selection_changed)
        .def_readonly("dirty_before", &PersistentPresetMutationResult::dirty_before)
        .def_readonly("dirty_after", &PersistentPresetMutationResult::dirty_after)
        .def_readonly("applied", &PersistentPresetMutationResult::applied)
        .def_readonly("persisted", &PersistentPresetMutationResult::persisted)
        .def_readonly("side_effects", &PersistentPresetMutationResult::side_effects)
        .def_readonly("error_code", &PersistentPresetMutationResult::error_code);

    py::class_<PersistentPresetMutationTransaction,
               std::shared_ptr<PersistentPresetMutationTransaction>>(
        host, "PersistentProcessPresetMutationTransaction")
        .def("execute", &PersistentPresetMutationTransaction::execute,
             py::arg("patch"), py::arg("protected_approved") = false)
        .def_property_readonly("state", &PersistentPresetMutationTransaction::state);

    host.def("create_process_preset_mutation_transaction",
             &PersistentPresetMutationTransaction::capture_live_process,
             "Create an opaque, UI-thread-only, one-shot persistent Process preset transaction.");
    host.def("create_printer_preset_mutation_transaction",
             &PersistentPresetMutationTransaction::capture_live_printer,
             "Create an opaque, UI-thread-only, one-shot persistent Printer preset transaction.");

    py::class_<FilamentSlotMutationResult>(host, "FilamentSlotMutationResult")
        .def_readonly("slot_index", &FilamentSlotMutationResult::slot_index)
        .def_readonly("target_preset_name", &FilamentSlotMutationResult::target_preset_name)
        .def_readonly("state", &FilamentSlotMutationResult::state)
        .def_readonly("committed", &FilamentSlotMutationResult::committed)
        .def_readonly("rollback_attempted", &FilamentSlotMutationResult::rollback_attempted)
        .def_readonly("rollback_verified", &FilamentSlotMutationResult::rollback_verified)
        .def_readonly("selection_changed", &FilamentSlotMutationResult::selection_changed)
        .def_readonly("dirty_before", &FilamentSlotMutationResult::dirty_before)
        .def_readonly("dirty_after", &FilamentSlotMutationResult::dirty_after)
        .def_readonly("mapping_changed", &FilamentSlotMutationResult::mapping_changed)
        .def_readonly("applied", &FilamentSlotMutationResult::applied)
        .def_readonly("persisted", &FilamentSlotMutationResult::persisted)
        .def_readonly("side_effects", &FilamentSlotMutationResult::side_effects)
        .def_readonly("error_code", &FilamentSlotMutationResult::error_code);

    py::class_<FilamentSlotMutationSnapshot>(host, "FilamentSlotMutationSnapshot")
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

    py::class_<FilamentSlotMutationTransaction,
               std::shared_ptr<FilamentSlotMutationTransaction>>(
        host, "FilamentSlotPresetMutationTransaction")
        .def("execute", &FilamentSlotMutationTransaction::execute,
             py::arg("patch"), py::arg("protected_approved") = false)
        .def("snapshot", &FilamentSlotMutationTransaction::snapshot)
        .def_property_readonly("state", &FilamentSlotMutationTransaction::state);

    host.def("create_filament_slot_preset_mutation_transaction",
             &FilamentSlotMutationTransaction::capture_live,
             py::arg("slot_index"),
             "Create an opaque, UI-thread-only, one-shot persistent Filament slot transaction.");

    advertise_plugin_host_feature_contract("process_preset_transaction", 1, 0);
    advertise_plugin_host_feature_contract("printer_preset_transaction", 1, 0);
    advertise_plugin_host_feature_contract("filament_slot_transaction", 1, 0);
}

} // namespace Slic3r::host_bindings
