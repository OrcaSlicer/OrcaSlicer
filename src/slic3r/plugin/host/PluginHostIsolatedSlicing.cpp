#include "IsolatedSlicingJob.hpp"
#include "PluginHostBindings.hpp"

#include <pybind11/stl.h>

namespace py = pybind11;

namespace Slic3r::host_bindings {

void register_isolated_slicing(py::module_& host)
{
    py::enum_<IsolatedSlicingState>(host, "IsolatedSlicingState")
        .value("Ready", IsolatedSlicingState::Ready)
        .value("Running", IsolatedSlicingState::Running)
        .value("Succeeded", IsolatedSlicingState::Succeeded)
        .value("Cancelled", IsolatedSlicingState::Cancelled)
        .value("Failed", IsolatedSlicingState::Failed)
        .value("Closed", IsolatedSlicingState::Closed);

    py::class_<IsolatedSlicingSnapshotInfo>(host, "IsolatedSlicingSnapshot")
        .def_readonly("object_count", &IsolatedSlicingSnapshotInfo::object_count)
        .def_readonly("plate_index", &IsolatedSlicingSnapshotInfo::plate_index)
        .def_readonly("filament_maps", &IsolatedSlicingSnapshotInfo::filament_maps)
        .def_readonly("filament_volume_maps", &IsolatedSlicingSnapshotInfo::filament_volume_maps)
        .def_readonly("bbl_vendor", &IsolatedSlicingSnapshotInfo::bbl_vendor)
        .def_readonly("check_mixed_filament_compatibility",
                      &IsolatedSlicingSnapshotInfo::check_mixed_filament_compatibility)
        .def_readonly("extruder_count", &IsolatedSlicingSnapshotInfo::extruder_count)
        .def_readonly("extruder_filament_config_count",
                      &IsolatedSlicingSnapshotInfo::extruder_filament_config_count);

    py::class_<IsolatedPrintStatistics>(host, "IsolatedPrintStatistics")
        .def_readonly("estimated_normal_print_time", &IsolatedPrintStatistics::estimated_normal_print_time)
        .def_readonly("estimated_silent_print_time", &IsolatedPrintStatistics::estimated_silent_print_time)
        .def_readonly("total_used_filament", &IsolatedPrintStatistics::total_used_filament)
        .def_readonly("total_extruded_volume", &IsolatedPrintStatistics::total_extruded_volume)
        .def_readonly("total_cost", &IsolatedPrintStatistics::total_cost)
        .def_readonly("total_toolchanges", &IsolatedPrintStatistics::total_toolchanges)
        .def_readonly("total_weight", &IsolatedPrintStatistics::total_weight)
        .def_readonly("total_wipe_tower_cost", &IsolatedPrintStatistics::total_wipe_tower_cost)
        .def_readonly("total_wipe_tower_filament", &IsolatedPrintStatistics::total_wipe_tower_filament)
        .def_readonly("initial_tool", &IsolatedPrintStatistics::initial_tool)
        .def_readonly("filament_stats", &IsolatedPrintStatistics::filament_stats);

    py::class_<IsolatedGCodeStatistics>(host, "IsolatedGCodeStatistics")
        .def_readonly("result_id", &IsolatedGCodeStatistics::result_id)
        .def_readonly("move_count", &IsolatedGCodeStatistics::move_count)
        .def_readonly("line_count", &IsolatedGCodeStatistics::line_count)
        .def_readonly("layer_count", &IsolatedGCodeStatistics::layer_count)
        .def_readonly("filament_count", &IsolatedGCodeStatistics::filament_count)
        .def_readonly("normal_time_seconds", &IsolatedGCodeStatistics::normal_time_seconds)
        .def_readonly("silent_time_seconds", &IsolatedGCodeStatistics::silent_time_seconds)
        .def_readonly("total_filament_changes", &IsolatedGCodeStatistics::total_filament_changes)
        .def_readonly("total_extruder_changes", &IsolatedGCodeStatistics::total_extruder_changes)
        .def_readonly("total_travel_distance", &IsolatedGCodeStatistics::total_travel_distance)
        .def_readonly("total_travel_moves", &IsolatedGCodeStatistics::total_travel_moves);

    py::class_<IsolatedSlicingResult>(host, "IsolatedSlicingResult")
        .def_readonly("state", &IsolatedSlicingResult::state)
        .def_readonly("error", &IsolatedSlicingResult::error)
        .def_readonly("candidate", &IsolatedSlicingResult::candidate)
        .def_readonly("print_statistics", &IsolatedSlicingResult::print_statistics)
        .def_readonly("gcode_statistics", &IsolatedSlicingResult::gcode_statistics)
        .def_readonly("warnings", &IsolatedSlicingResult::warnings)
        .def_readonly("gcode_path", &IsolatedSlicingResult::gcode_path)
        .def_readonly("gcode_size", &IsolatedSlicingResult::gcode_size);

    py::class_<IsolatedSlicingJob, std::shared_ptr<IsolatedSlicingJob>>(
        host, "IsolatedFFFSlicingJob")
        .def("snapshot", &IsolatedSlicingJob::snapshot,
             "Return copied metadata describing the immutable owned baseline.")
        .def("apply_candidate", &IsolatedSlicingJob::apply_candidate, py::arg("patch"),
             "Atomically replace the staged candidate with a strict patch over the baseline.")
        .def_property_readonly("candidate", &IsolatedSlicingJob::candidate,
             "Canonical values for the current staged patch.")
        .def("fork_from_baseline", &IsolatedSlicingJob::fork_from_baseline,
             "Create an independent single-use job over this job's immutable owned baseline.")
        .def("run", &IsolatedSlicingJob::run,
             "Start the dedicated isolated slicing worker without waiting.")
        .def("cancel", &IsolatedSlicingJob::cancel,
             "Request cooperative cancellation of the owned Print.")
        .def("wait", &IsolatedSlicingJob::wait, py::call_guard<py::gil_scoped_release>(),
             "Join the worker and return its terminal state.")
        .def("result", &IsolatedSlicingJob::result,
             "Return a copied terminal result; never returns Print or model wrappers.")
        .def_property_readonly("state", &IsolatedSlicingJob::state)
        .def("close", &IsolatedSlicingJob::close, py::call_guard<py::gil_scoped_release>(),
             "Idempotently cancel, join and remove this job's temporary artifacts.");

    host.def("create_isolated_fff_slicing_job", &IsolatedSlicingJob::capture_live,
             "Capture the current FFF plate into an opaque, immutable isolated slicing job.");
}

} // namespace Slic3r::host_bindings
