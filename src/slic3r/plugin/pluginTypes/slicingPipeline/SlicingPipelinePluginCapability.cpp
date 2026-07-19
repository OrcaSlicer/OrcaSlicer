#include "SlicingPipelinePluginCapability.hpp"
#include "SlicingPipelinePluginCapabilityTrampoline.hpp"
#include "slic3r/plugin/PluginBindingUtils.hpp" // config_value_or_none
#include "libslic3r/libslic3r.h"    // unscale<>, live SCALING_FACTOR

namespace py = pybind11;
namespace Slic3r {

bool SlicingPipelineContext::cancelled() const { return print && print->canceled(); }

void SlicingPipelinePluginCapability::RegisterBindings(py::module_& module, py::enum_<PluginCapabilityType>& pluginTypes) {
    (void) pluginTypes; // unused: this capability defines its own Step enum (below) rather than extending the shared PluginCapabilityType enum.
    auto slicing = module.def_submodule("slicing", "Slicing pipeline API (research/experimental).");

    py::enum_<SlicingPipelineStepPlugin>(slicing, "Step")
        .value("posSlice", SlicingPipelineStepPlugin::posSlice)
        .value("posPerimeters", SlicingPipelineStepPlugin::posPerimeters)
        .value("posEstimateCurledExtrusions", SlicingPipelineStepPlugin::posEstimateCurledExtrusions)
        .value("posPrepareInfill", SlicingPipelineStepPlugin::posPrepareInfill) // after prepare_infill, before make_fills: editing fill_surfaces here CASCADES
        .value("posInfill", SlicingPipelineStepPlugin::posInfill)          // after make_fills: editing fill_surfaces here does NOT regenerate the fills
        .value("posIroning", SlicingPipelineStepPlugin::posIroning)
        .value("posContouring", SlicingPipelineStepPlugin::posContouring)
        .value("posSupportMaterial", SlicingPipelineStepPlugin::posSupportMaterial)
        .value("posDetectOverhangsForLift", SlicingPipelineStepPlugin::posDetectOverhangsForLift)
        .value("posSimplifyPath", SlicingPipelineStepPlugin::posSimplifyPath) // covers all simplify sub-steps
        .value("psWipeTower", SlicingPipelineStepPlugin::psWipeTower)
        .value("psSkirtBrim", SlicingPipelineStepPlugin::psSkirtBrim)
        // Post-process seam: fires in the GUI export path AFTER the classic post_process scripts, on the
        // exported G-code file. Unlike every step above it is NOT fired by Print::process(): ctx.print and
        // ctx.object are None; instead ctx.gcode_path / ctx.host / ctx.output_name are set and the plugin
        // edits the file at ctx.gcode_path IN PLACE. May fire more than once per slice (file export and/or
        // upload each fire once, on separate working copies) and its output is not reflected in the G-code
        // preview (the viewer maps the pre-post-process file). ctx.config_value() still works.
        .value("psGCodePostProcess", SlicingPipelineStepPlugin::psGCodePostProcess)
        .export_values();

    // The read-graph data model (Surface / ExPolygon / the extrusion tree / LayerRegion /
    // Layer / PrintObject / Print) and the 2D-geometry mutators live in orca.host, registered
    // by PluginHostSlicing.cpp. orca.slicing is workflow-only: Step, unscale, the context, and
    // the capability base. See PluginHostSlicing.cpp for the mandatory reference-lifetime rule.

    // Scaled integer coordinate -> millimeters. Reads the live SCALING_FACTOR at call
    // time (1e-6 normal, 1e-5 for beds > 2147mm), so it is never cached.
    slicing.def("unscale", [](coord_t v) { return unscale<double>(v); }, py::arg("coord"),
        "Convert a scaled integer coordinate to millimeters (reads the live SCALING_FACTOR).");

    py::class_<SlicingPipelineContext>(slicing, "SlicingPipelineContext")
        .def_readonly("orca_version", &SlicingPipelineContext::orca_version)
        .def_readonly("step", &SlicingPipelineContext::step)
        .def_readonly("gcode_path", &SlicingPipelineContext::gcode_path,
            "Path to the working G-code file, set ONLY at Step.psGCodePostProcess. Edit it in "
            "place; empty at every other step.")
        .def_readonly("host", &SlicingPipelineContext::host,
            "Target host at Step.psGCodePostProcess (\"File\", \"OctoPrint\", ...); empty otherwise.")
        .def_readonly("output_name", &SlicingPipelineContext::output_name,
            "Final output G-code name at Step.psGCodePostProcess (mirrors SLIC3R_PP_OUTPUT_NAME); "
            "empty otherwise.")
        .def_property_readonly("print", [](const SlicingPipelineContext& ctx) -> py::object {
            if (ctx.print == nullptr)
                return py::none();
            return py::cast(ctx.print, py::return_value_policy::reference);
        }, "The orca.host.Print being sliced — the raw slicing graph, exactly what the "
           "C++ pipeline walks. Valid only during the execute(ctx) call. For mesh access "
           "use ctx.print.model() (the Print's snapshot), never orca.host.model().")
        .def_property_readonly("object", [](const SlicingPipelineContext& ctx) -> py::object {
            if (ctx.object == nullptr)
                return py::none();
            // The hook signature hands objects out as const; they are genuinely mutable
            // (owned by the Print), so the const_cast is safe — done once here at the
            // graph entry point so Python steps receive a mutable PrintObject.
            return py::cast(const_cast<PrintObject*>(ctx.object), py::return_value_policy::reference);
        }, "orca.host.PrintObject for object-scoped steps, or None for print-wide steps. "
           "Valid only during the execute(ctx) call.")
        .def("config_value", [](const SlicingPipelineContext& ctx, const std::string& key) -> py::object {
            // In-pipeline steps read the live Print's full config; at psGCodePostProcess (print == null)
            // fall back to the config the export path handed in.
            if (ctx.print != nullptr)
                return config_value_or_none(ctx.print->full_print_config(), key);
            if (ctx.full_config != nullptr)
                return config_value_or_none(*ctx.full_config, key);
            return py::none();
        }, py::arg("key"),
           "serialized value of a resolved (full) print config option for this slice, or "
           "None if absent. Shorthand for ctx.print.config_value(key).")
        .def("cancelled", &SlicingPipelineContext::cancelled);

    py::class_<SlicingPipelinePluginCapability, PluginCapabilityInterface,
               PySlicingPipelinePluginCapabilityTrampoline,
               std::shared_ptr<SlicingPipelinePluginCapability>>(slicing, "SlicingPipelineCapabilityBase")
        .def(py::init<>())
        .def("get_type", &SlicingPipelinePluginCapability::get_type)
        .def("execute", &SlicingPipelinePluginCapability::execute);
}

} // namespace Slic3r
