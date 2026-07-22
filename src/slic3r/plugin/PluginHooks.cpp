#include "PluginHooks.hpp"

#include "PluginManager.hpp"
#include "PythonInterpreter.hpp"
#include "PythonPluginInterface.hpp"
#include "pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r_version.h"

#include <boost/log/trivial.hpp>

#include <memory>
#include <string>
#include <stdexcept>

namespace Slic3r::plugin_hooks {
namespace {

// Manifest resolver: turns the bare capability name a preset stores into the full
// "name;uuid;capability" reference the dispatchers consume (see
// ConfigBase::collect_plugin_manifest / update_plugin_manifest).
void install_capability_resolver()
{
    ConfigBase::set_resolve_capability_fn([](const std::string& cap_name, const std::string& cap_type) {
        PluginManager& plugin_mgr = PluginManager::instance();
        const PluginCapabilityType type = plugin_capability_type_from_string(cap_type);
        // only_enabled = false: this resolves the reference a preset STORES, which must stay
        // resolvable whether or not the user currently has the capability enabled. Filtering on
        // the enable flag here would quietly drop the reference out of the preset's manifest.
        auto plugin_cap = plugin_mgr.get_plugin_capability(cap_name, type, /*only_enabled=*/false);
        if (!plugin_cap)
            return std::string();

        PluginDescriptor descriptor;
        if (!plugin_mgr.try_get_plugin_descriptor_for_capability(cap_name, type, descriptor))
            return std::string();

        // Cloud plugins are resolved at runtime via the UUID in the middle field, so the first
        // field keeps the friendly display name. Local plugins are looked up by plugin_key (the
        // first field, with an empty UUID), so emit the plugin_key to keep them resolvable.
        const std::string identity = descriptor.is_cloud_plugin() ? descriptor.name : descriptor.plugin_key;
        return identity + ';' + descriptor.cloud_uuid() + ';' + cap_name;
    });
}

// Print::process() fires this hook at each pipeline seam on the slicing worker
// thread; here we run the picker-selected SlicingPipeline capabilities. Per
// capability we acquire the GIL, honor cancellation, and convert a plugin
// failure into a (non-critical) SlicingError so it surfaces as a slicing-error
// notification rather than the fatal-crash dialog.
void install_slicing_pipeline_hook()
{
    Print::set_slicing_pipeline_hook_fn(
        [](Print& print, const PrintObject* object, SlicingPipelineStepPlugin step) {
            const auto* caps  = print.config().option<ConfigOptionStrings>("slicing_pipeline_plugin");
            // `plugins` is a dynamic-only manifest key (not a static PrintConfig member), so it
            // must be read from the full/dynamic config -- reading it off print.config() (the
            // static PrintConfig) always yields nullptr and skips every capability. Mirrors the
            // post-process path (PostProcessor.cpp, via BackgroundSlicingProcess::full_print_config()).
            const auto* plugs = print.full_print_config().option<ConfigOptionStrings>("plugins");
            if (caps == nullptr || caps->values.empty())
                return;

            execute_capabilities_from_refs<SlicingPipelinePluginCapability>(
                *caps, plugs, PluginCapabilityType::SlicingPipeline,
                [&](std::shared_ptr<SlicingPipelinePluginCapability> cap, const PluginCapabilityRef& ref) {
                    const std::string plugin_key = ref.uuid.empty() ? ref.name : ref.uuid;
                    ExecutionResult r;
                    try {
                        // GIL is acquired per capability (not once for the whole dispatch) so it
                        // is released between capabilities.
                        PythonGILState gil;
                        if (!gil)
                            throw std::runtime_error("Python interpreter is shutting down");
                        // throw_if_canceled() is protected on PrintBase; canceled() is the public
                        // equivalent check (same cancel flag), so honor cancellation via it.
                        if (print.canceled())
                            throw CanceledException();
                        SlicingPipelineContext ctx;
                        ctx.orca_version = SoftFever_VERSION;
                        ctx.step   = step;
                        ctx.print  = &print;
                        ctx.object = object;
                        r = cap->execute(ctx);
                    } catch (const CanceledException&) {
                        throw; // cancellation must reach process(), never become a slicing error
                    } catch (const std::exception& ex) {
                        // A Python raise reaches here as pybind11::error_already_set; surface it as a
                        // (non-critical) slicing error instead of a crash.
                        throw SlicingError(std::string("Slicing pipeline plugin '") +
                                           ref.capability_name + "' error: " + ex.what());
                    }
                    if (r.status == PluginResult::FatalError)
                        throw SlicingError(std::string("Slicing pipeline plugin '") +
                                           ref.capability_name + "' error: " + r.message);
                    // log a non-empty success/skipped message instead of dropping it. This is
                    // log-only by design: every pipeline hook fires AFTER set_done() (see Print.cpp),
                    // so the Print-level m_step_active is -1 here. Calling active_step_add_warning()
                    // would then index m_state[-1] (out-of-bounds; the guarding assert is compiled
                    // out in Release), so it must NOT be called from a pipeline hook.
                    if (!r.message.empty()) {
                        static const char* const kStepNames[] = {
                            "posSlice", "posPerimeters", "posEstimateCurledExtrusions", "posPrepareInfill", "posInfill",
                            "posIroning", "posContouring", "posSupportMaterial", "posDetectOverhangsForLift",
                            "posSimplifyPath", "psWipeTower", "psSkirtBrim", "psGCodePostProcess"
                        }; // order must match SlicingPipelineStepPlugin
                        const char* step_name = static_cast<size_t>(step) < sizeof(kStepNames) / sizeof(kStepNames[0])
                                                    ? kStepNames[static_cast<int>(step)] : "Unknown";
                        BOOST_LOG_TRIVIAL(info) << "Slicing pipeline plugin '" << ref.capability_name
                                                << "' [" << step_name << "]: " << r.message;
                    }
                });
        });
}

} // namespace

void install()
{
    install_capability_resolver();
    install_slicing_pipeline_hook();
}

void uninstall()
{
    ConfigBase::set_resolve_capability_fn(nullptr);
    Print::set_slicing_pipeline_hook_fn(nullptr);
}

} // namespace Slic3r::plugin_hooks
