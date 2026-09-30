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

    py::class_<FilamentSlotMutationTransaction,
               std::shared_ptr<FilamentSlotMutationTransaction>>(
        host, "FilamentSlotPresetMutationTransaction")
        .def("execute", &FilamentSlotMutationTransaction::execute,
             py::arg("patch"), py::arg("protected_approved") = false)
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
