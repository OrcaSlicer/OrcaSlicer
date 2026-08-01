#pragma once
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include "libslic3r/Print.hpp"      // SlicingPipelineStepPlugin, Print, PrintObject
#include <pybind11/pybind11.h>
#include <map>
#include <string>

namespace Slic3r {

// Workflow context handed to SlicingPipeline plugins. ctx.print / ctx.object
// are RAW references into the live slicing graph — the same objects the C++
// pipeline mutates. The data-model bindings and the mandatory lifetime rule
// (valid only during execute(ctx); mutators invalidate references into replaced
// containers, like std::vector iterators) live in
// src/slic3r/plugin/host/PluginHostSlicing.cpp.
struct SlicingPipelineContext {
    std::string          orca_version;
    SlicingPipelineStepPlugin  step { SlicingPipelineStepPlugin::posSlice };
    Print*               print  { nullptr };   // present for in-pipeline steps; null at psGCodePostProcess
    const PrintObject*   object { nullptr };   // null for print-wide steps and psGCodePostProcess
    // Populated ONLY at Step.psGCodePostProcess (the GUI G-code export/post-process seam,
    // PostProcessor.cpp). gcode_path is the working G-code file on disk that the plugin edits
    // in place; host is the target ("File", "OctoPrint", ...); output_name mirrors
    // SLIC3R_PP_OUTPUT_NAME. Empty at every other step.
    std::string          gcode_path;
    std::string          host;
    std::string          output_name;
    // C++-only config fallback for psGCodePostProcess (no live Print graph there): config_value()
    // reads it when `print` is null. Not exposed to Python directly. Never dereferenced elsewhere.
    const DynamicPrintConfig* full_config { nullptr };
    bool cancelled() const;                     // -> print->canceled() (false when print is null)
};

class SlicingPipelinePluginCapability : public PluginCapabilityInterface {
public:
    PluginCapabilityType get_type() const override { return PluginCapabilityType::SlicingPipeline; }
    // Runs on the slicing worker thread. Do not call orca.host.ui.* here: the UI thread can be
    // blocked waiting on the slicing worker, so a marshaled UI call from this thread can deadlock.
    virtual ExecutionResult execute(SlicingPipelineContext& ctx) = 0;
    static void RegisterBindings(pybind11::module_& module, pybind11::enum_<PluginCapabilityType>& pluginTypes);
};

} // namespace Slic3r
