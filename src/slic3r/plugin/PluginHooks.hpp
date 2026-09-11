#pragma once

// The plugin layer's installers for the hooks libslic3r exposes. libslic3r
// stays free of any plugin/Python dependency: it exposes static setter seams
// (ConfigBase::set_resolve_capability_fn, Print::set_slicing_pipeline_hook_fn,
// ...) and this unit injects the dispatchers -- one file-local installer per
// hook, aggregated by install(). Capabilities dispatched from the GUI layer
// (e.g. PostProcessor.cpp) call execute_capabilities_from_refs at their own
// call site and need no hook here.

namespace Slic3r::plugin_hooks {

// Install every hook. Called once from PluginManager::initialize().
void install();

// Reset every hook to null so none can enter Python after the interpreter
// finalizes. Called from PluginManager::shutdown(); callers must have stopped
// background slicing first (resetting a hook while process() runs is a race).
void uninstall();

} // namespace Slic3r::plugin_hooks
