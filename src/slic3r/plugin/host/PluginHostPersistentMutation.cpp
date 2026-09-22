#include "PersistentPresetMutationTransaction.hpp"
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
}

} // namespace Slic3r::host_bindings
