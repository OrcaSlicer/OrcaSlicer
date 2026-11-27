#include "SlicingPipelinePluginCapability.hpp"
#include "SlicingPipelinePluginCapabilityTrampoline.hpp"
#include <pybind11/pybind11.h>
#include <pybind11/cast.h>
#include <pybind11/pytypes.h>
#include "slic3r/plugin/PluginBindingUtils.hpp" // config_value_or_none
#include "libslic3r/libslic3r.h"    // unscale<>, live SCALING_FACTOR
#include "libslic3r/Print.hpp"
#include <string>
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include <memory>

namespace py = pybind11;
namespace Slic3r {

bool SlicingPipelineContext::cancelled() const { return print && print->canceled(); }

void SlicingPipelinePluginCapability::RegisterBindings(py::module_& module) {
    auto slicing = module.def_submodule("slicing", "Slicing pipeline API (research/experimental).");

    py::enum_<SlicingPipelineStepPlugin>(
        slicing, "Step",
        "Point in slicing at which execute(ctx) runs, given as ctx.step. The pos* steps run for each\n"
        "PrintObject that recomputed that step in the current slice, with ctx.object set; an object whose\n"
        "step is still valid from an earlier slice is not passed again. The ps* steps run once for the whole\n"
        "print, with ctx.object None.")
        .value("posSlice", SlicingPipelineStepPlugin::posSlice, "After the object is sliced into layers.")
        .value("posPerimeters", SlicingPipelineStepPlugin::posPerimeters, "After the object's walls are generated.")
        .value("posEstimateCurledExtrusions", SlicingPipelineStepPlugin::posEstimateCurledExtrusions,
               "After curled extrusions are estimated.")
        .value("posPrepareInfill", SlicingPipelineStepPlugin::posPrepareInfill,
               "After the infill regions are prepared and before the fills are generated, so edits to\n"
               "fill_surfaces here change the fills generated next.")
        .value("posInfill", SlicingPipelineStepPlugin::posInfill,
               "After the fills are generated; edits to fill_surfaces here do not regenerate them.")
        .value("posIroning", SlicingPipelineStepPlugin::posIroning, "After ironing is generated.")
        .value("posContouring", SlicingPipelineStepPlugin::posContouring, "After Z contouring.")
        .value("posSupportMaterial", SlicingPipelineStepPlugin::posSupportMaterial, "After supports are generated.")
        .value("posDetectOverhangsForLift", SlicingPipelineStepPlugin::posDetectOverhangsForLift,
               "After overhangs are detected for Z lift.")
        .value("posSimplifyPath", SlicingPipelineStepPlugin::posSimplifyPath,
               "After the extrusion paths are simplified, covering all of its sub-steps.")
        .value("psWipeTower", SlicingPipelineStepPlugin::psWipeTower, "After the wipe tower is generated.")
        .value("psSkirtBrim", SlicingPipelineStepPlugin::psSkirtBrim, "After the skirt and brim are generated.")
        .value("psGCodePostProcess", SlicingPipelineStepPlugin::psGCodePostProcess,
               "After G-code export, following the post-processing scripts. ctx.print and ctx.object are None;\n"
               "ctx.gcode_path, ctx.host and ctx.output_name are set, and the plugin edits the file at\n"
               "ctx.gcode_path in place. It can run more than once per slice, once for each file export or upload,\n"
               "each on its own working copy, and the G-code preview does not show its edits.\n"
               "ctx.config_value() still works.")
        .export_values();

    // The read-graph data model (Surface / ExPolygon / the extrusion tree / LayerRegion /
    // Layer / PrintObject / Print) and the 2D-geometry mutators live in orca.host, registered
    // by PluginHostSlicing.cpp. orca.slicing is workflow-only: Step, unscale, the context, and
    // the capability base. See PluginHostSlicing.cpp for the mandatory reference-lifetime rule.

    // Scaled integer coordinate -> millimeters. Reads the live SCALING_FACTOR at call
    // time (1e-6 normal, 1e-5 for beds > 2147mm), so it is never cached.
    slicing.def("unscale", [](coord_t v) { return unscale<double>(v); }, py::arg("coord"),
        "Convert a scaled integer coordinate to millimeters (reads the live SCALING_FACTOR).");

    py::class_<SlicingPipelineContext>(
        slicing, "SlicingPipelineContext",
        "What execute(ctx) receives: the Step, and either the slicing graph (print, object) for the slicing\n"
        "steps or the exported G-code file for Step.psGCodePostProcess. Valid only during that execute() call.")
        .def_readonly("orca_version", &SlicingPipelineContext::orca_version, "The OrcaSlicer version string.")
        .def_readonly("step", &SlicingPipelineContext::step,
                      "The Step this call is for. execute() runs at every step, so return early for the steps the\n"
                      "plugin does not handle.")
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
           "Return the serialized value of a resolved (full) print config option for this slice, as a\n"
           "string, or None if absent. At the slicing steps it is shorthand for ctx.print.config_value(key);\n"
           "it also works at Step.psGCodePostProcess.")
        .def("cancelled", &SlicingPipelineContext::cancelled,
             "Return True once the user has cancelled this slice; a long step should check it and return early.\n"
             "Always False at Step.psGCodePostProcess.");

    py::class_<SlicingPipelinePluginCapability, PluginCapabilityInterface,
               PySlicingPipelinePluginCapabilityTrampoline,
               std::shared_ptr<SlicingPipelinePluginCapability>>(
        slicing, "SlicingPipelineCapabilityBase",
        "Base for a slicing pipeline capability: Python run at each Step of slicing and on the exported G-code,\n"
        "to read and change the intermediate slicing data. It runs only for slices whose print settings select\n"
        "it in the Slicing Pipeline Plugin option.")
        .def(py::init<>())
        .def("get_type", &SlicingPipelinePluginCapability::get_type, "Return PluginType.SlicingPipeline.")
        .def("execute", &SlicingPipelinePluginCapability::execute, py::arg("ctx"),
             "Required. Called at every Step with ctx, a SlicingPipelineContext, on the slicing thread; it must be\n"
             "a plain def, not async def, and must not call orca.host.ui. Return an ExecutionResult. At the\n"
             "slicing steps FatalError, like a raised exception, stops the slice with its message as the error,\n"
             "and the other statuses let slicing continue. At Step.psGCodePostProcess RecoverableError and\n"
             "FatalError, like a raised exception, fail the export. A non-empty message is logged.");
}

} // namespace Slic3r
