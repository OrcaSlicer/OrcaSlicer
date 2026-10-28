#include "PluginHooks.hpp"

#include "PluginManager.hpp"
#include "PythonInterpreter.hpp"
#include "PythonPluginInterface.hpp"
#include "pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/LifecycleEvents.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <exception>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <utility>
#include <vector>
#include "slic3r/plugin/PluginDescriptor.hpp"
#include <slic3r/plugin/pluginTypes/infill/InfillPluginCapability.hpp>
#include <string>
#include <stdexcept>
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"

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

// Global libslic3r-side seam (Slic3r::fire_lifecycle_event, in libslic3r/LifecycleEvents.hpp):
// broadcasts to every loaded, enabled capability regardless of type, unlike the SlicingPipeline
// hook below which only targets picker-selected SlicingPipeline capabilities.
void install_lifecycle_event_hook()
{
    set_lifecycle_hook_fn([](LifecycleEvent event, const LifecycleEventContext& ctx) {
        PluginManager::instance().dispatch_lifecycle_event(event, ctx);
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
                        // Slicing hooks are synchronous by design and never await: the context
                        // borrows Print/PrintObject that are only valid during this call, and the
                        // UI thread may itself be blocked waiting on this slicing worker. An async
                        // override is rejected here rather than scheduled -- cap->execute() casts
                        // its Python result to ExecutionResult and a coroutine object cannot be
                        // cast, so it surfaces as the SlicingError below.
                        r = cap->execute(ctx);
                    } catch (const CanceledException&) {
                        throw; // cancellation must reach process(), never become a slicing error
                    } catch (const std::exception& ex) {
                        // A Python raise reaches here as pybind11::error_already_set; surface it as a
                        // (non-critical) slicing error instead of a crash.
                        throw SlicingError(GUI::format(_u8L("Slicing pipeline plugin '%1%' error: %2%"),
                                                       ref.capability_name, ex.what()));
                    }
                    if (r.status == PluginResult::FatalError)
                        throw SlicingError(GUI::format(_u8L("Slicing pipeline plugin '%1%' error: %2%"),
                                                       ref.capability_name, r.message));
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

void install_infill_hook()
{
    const auto plugin_key_of = [](const PluginCapabilityRef& ref) {
        return ref.uuid.empty() ? ref.name : ref.uuid;
    };

    // Resolve one project reference as an Infill capability of exactly that identity. A reference to
    // another capability type that shares the name simply does not resolve here.
    const auto resolve_infill = [](const PluginCapabilityRef& ref, std::string& error)
        -> std::shared_ptr<InfillPluginCapability> {
        const std::string plugin_key = ref.uuid.empty() ? ref.name : ref.uuid;
        if (plugin_key.empty() || ref.capability_name.empty()) {
            error = "Plugin infill capability reference is incomplete";
            return nullptr;
        }

        PluginManager& mgr = PluginManager::instance();
        const PluginCapabilityId id{PluginCapabilityType::Infill, ref.capability_name, plugin_key};
        std::shared_ptr<PluginCapabilityInterface> capability =
            mgr.get_plugin_capability(id, /*only_enabled=*/false);
        if (!capability) {
            error = "Plugin infill capability '" + ref.capability_name + "' is not loaded from plugin '" +
                    plugin_key + "'";
            return nullptr;
        }
        if (capability->type() != PluginCapabilityType::Infill ||
            capability->name() != ref.capability_name) {
            error = "Plugin capability '" + ref.capability_name + "' from plugin '" + plugin_key +
                    "' is not the requested infill capability";
            return nullptr;
        }
        if (!capability->is_enabled()) {
            error = "Plugin infill capability '" + ref.capability_name + "' from plugin '" + plugin_key +
                    "' is disabled";
            return nullptr;
        }

        auto infill = std::dynamic_pointer_cast<InfillPluginCapability>(capability);
        if (!infill)
            error = "Plugin capability '" + ref.capability_name + "' from plugin '" + plugin_key +
                    "' is not an infill provider";
        return infill;
    };

    const auto record_failure = [plugin_key_of](const PluginCapabilityRef& ref, const std::string& error) {
        const std::string plugin_key = plugin_key_of(ref);
        if (!plugin_key.empty())
            PluginManager::instance().report_plugin_diagnostic(plugin_key, error);
    };

    const auto record_capability_failure = [](const std::shared_ptr<InfillPluginCapability>& capability,
                                              const std::string&                                     error) {
        if (!capability)
            return;
        const std::string& plugin_key = capability->audit_plugin_key();
        if (!plugin_key.empty())
            PluginManager::instance().report_plugin_diagnostic(plugin_key, error);
    };

    Fill::set_plugin_infill_hooks(
        // Prepare: one bounded wait, then select exactly one unambiguous Infill provider per selected
        // key from that key's candidate manifest references. Runs once per PrintObject, before fill
        // work enters any worker pool.
        [plugin_key_of, record_capability_failure](
            const std::vector<std::string>&         capability_names,
            const std::vector<PluginCapabilityRef>& manifest_refs)
            -> std::map<std::string, PluginCapabilityRef> {
            if (capability_names.empty())
                return {};

            PluginManager& mgr = PluginManager::instance();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            std::vector<std::string> waited_plugin_keys;
            for (const std::string& capability_name : capability_names) {
                for (const PluginCapabilityRef& ref : manifest_refs) {
                    if (ref.capability_name != capability_name)
                        continue;
                    const std::string plugin_key = plugin_key_of(ref);
                    if (plugin_key.empty() ||
                        std::find(waited_plugin_keys.begin(), waited_plugin_keys.end(), plugin_key) != waited_plugin_keys.end())
                        continue;
                    waited_plugin_keys.emplace_back(plugin_key);

                    const auto now = std::chrono::steady_clock::now();
                    const auto remaining = now < deadline
                        ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                        : std::chrono::milliseconds::zero();
                    std::string load_error;
                    if (!mgr.wait_for_plugin_load(plugin_key, remaining, load_error)) {
                        auto capability = std::dynamic_pointer_cast<InfillPluginCapability>(
                            mgr.get_plugin_capability(
                                {PluginCapabilityType::Infill, capability_name, plugin_key},
                                /*only_enabled=*/false));
                        if (capability)
                            record_capability_failure(
                                capability, "Plugin infill capability '" + capability_name +
                                                "' failed while waiting for its plugin load: " + load_error);
                        else
                            BOOST_LOG_TRIVIAL(error) << "Plugin infill capability '" << capability_name
                                                     << "' failed while waiting for plugin '" << plugin_key
                                                     << "': " << load_error;
                        throw SlicingError(PluginManager::plugin_infill_failure_message());
                    }
                }
            }

            std::map<std::string, PluginCapabilityRef> selected;

            struct MatchedProvider
            {
                PluginCapabilityRef                     ref;
                std::shared_ptr<InfillPluginCapability> capability;
            };

            for (const std::string& capability_name : capability_names) {
                // Candidate refs that resolve as the explicit Infill capability with exact identity.
                // A same-named ref of another capability type does not resolve here and is ignored.
                std::vector<MatchedProvider> matched;
                for (const PluginCapabilityRef& ref : manifest_refs) {
                    if (ref.capability_name != capability_name)
                        continue;
                    const std::string plugin_key = plugin_key_of(ref);
                    if (plugin_key.empty())
                        continue;

                    auto capability = std::dynamic_pointer_cast<InfillPluginCapability>(
                        mgr.get_plugin_capability(
                            {PluginCapabilityType::Infill, capability_name, plugin_key},
                            /*only_enabled=*/false));
                    if (!capability) {
                        BOOST_LOG_TRIVIAL(warning)
                            << "Plugin infill candidate '" << capability_name << "' from plugin '" << plugin_key
                            << "' does not resolve as an Infill capability; ignoring it";
                        continue;
                    }
                    if (capability->type() != PluginCapabilityType::Infill ||
                        capability->name() != capability_name) {
                        BOOST_LOG_TRIVIAL(warning)
                            << "Plugin infill candidate '" << capability_name << "' from plugin '" << plugin_key
                            << "' resolved to a different capability; ignoring it";
                        continue;
                    }
                    if (std::find_if(matched.begin(), matched.end(), [&](const MatchedProvider& existing) {
                            return existing.capability->identity() == capability->identity();
                        }) == matched.end())
                        matched.push_back({ref, std::move(capability)});
                }

                if (matched.size() > 1) {
                    for (const MatchedProvider& provider : matched)
                        record_capability_failure(
                            provider.capability, "Project references more than one infill provider for pattern '" +
                                                     capability_name + "'");
                    throw SlicingError(PluginManager::plugin_infill_failure_message());
                }

                if (matched.empty()) {
                    // Loaded Infill providers with this name, deduplicated by identity. They are used
                    // only to explain the failure: a project must be sliced with the provider it was
                    // saved with, so there is never a fallback to a provider it does not reference.
                    std::vector<std::shared_ptr<InfillPluginCapability>> loaded;
                    for (const auto& capability : mgr.get_plugin_capabilities(
                             /*plugin_key=*/"", PluginCapabilityType::Infill, /*only_enabled=*/false)) {
                        if (!capability || capability->type() != PluginCapabilityType::Infill ||
                            capability->name() != capability_name)
                            continue;
                        auto infill = std::dynamic_pointer_cast<InfillPluginCapability>(capability);
                        if (!infill)
                            continue;
                        if (std::find_if(loaded.begin(), loaded.end(), [&](const auto& existing) {
                                return existing->identity() == infill->identity();
                            }) == loaded.end())
                            loaded.emplace_back(std::move(infill));
                    }

                    if (loaded.empty()) {
                        // No loaded Infill provider has this name. Log the candidate refs, but do not
                        // mark a non-infill plugin as failed: a ref without an Infill provider is not
                        // evidence that its plugin is broken.
                        size_t candidate_count = 0;
                        for (const PluginCapabilityRef& ref : manifest_refs)
                            if (ref.capability_name == capability_name)
                                ++candidate_count;
                        BOOST_LOG_TRIVIAL(error) << "No loaded Infill capability named '" << capability_name
                                                 << "' for the project reference(s); candidates="
                                                 << candidate_count;
                        throw SlicingError(PluginManager::plugin_infill_failure_message());
                    }
                    if (loaded.size() > 1) {
                        for (const auto& provider : loaded)
                            record_capability_failure(
                                provider, "More than one loaded infill provider is named '" + capability_name + "'");
                        throw SlicingError(PluginManager::plugin_infill_failure_message());
                    }
                    record_capability_failure(
                        loaded.front(), "Loaded infill provider '" + capability_name +
                                            "' does not match any plugin reference in the project manifest");
                    throw SlicingError(PluginManager::plugin_infill_failure_message());
                }

                if (!matched.front().capability->is_enabled()) {
                    record_capability_failure(
                        matched.front().capability, "Plugin infill capability '" + capability_name +
                                                        "' from plugin '" + plugin_key_of(matched.front().ref) +
                                                        "' is disabled");
                    throw SlicingError(PluginManager::plugin_infill_failure_message());
                }

                selected.emplace(capability_name, matched.front().ref);
            }

            return selected;
        },
        // Factory: create the Fill for one already-selected exact reference. Never waits; the prepare
        // pass resolved it, and a late unload/disable is still reported as a slicing failure.
        [resolve_infill, record_failure](const PluginCapabilityRef& ref) -> Fill* {
            std::string error;
            std::shared_ptr<InfillPluginCapability> infill = resolve_infill(ref, error);
            if (!infill) {
                if (!error.empty())
                    record_failure(ref, std::move(error));
                throw SlicingError(PluginManager::plugin_infill_failure_message());
            }

            try {
                return infill->create_fill(infill);
            } catch (const CanceledException&) {
                throw;
            } catch (const std::exception& ex) {
                record_failure(ref, "Plugin infill capability '" + ref.capability_name +
                                        "' could not create a filler: " + ex.what());
                throw SlicingError(PluginManager::plugin_infill_failure_message());
            }
        });
}

} // namespace

void install()
{
    install_capability_resolver();
    install_slicing_pipeline_hook();
    install_infill_hook();
    install_lifecycle_event_hook();
}

void uninstall()
{
    ConfigBase::set_resolve_capability_fn(nullptr);
    Print::set_slicing_pipeline_hook_fn(nullptr);
    Fill::set_plugin_infill_hooks(nullptr, nullptr);
    set_lifecycle_hook_fn(nullptr);
}

} // namespace Slic3r::plugin_hooks
