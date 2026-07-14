#pragma once

#include "PluginDescriptor.hpp"

#include <boost/filesystem/path.hpp>

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
// Defined in PluginManager.hpp, which owns the registry these are materialized into. Only
// referenced here, never dereferenced, so the declaration is enough — and it keeps the leaf
// service free of any dependency on the manager that drives it.
struct Plugin;
} // namespace Slic3r

namespace Slic3r::plugin_loader {

// Stateless plugin engine: descriptor in, materialized Plugin out. It owns no registry, no locks
// and no async machinery — PluginManager owns all of that and calls in from its worker thread.

// Import a plugin package, instantiate its capabilities and run on_load() on each.
//
// On success `out` holds the module and the capability instances. Each capability's name and type
// are read exactly once here — under the GIL this function already holds — and cached ON the
// capability (PluginCapabilityInterface::name()/type()); no code outside this function may call
// get_name(), which is a Python dispatch.
//
// A capability comes up enabled unless `capabilities_to_enable` is non-empty and omits it, and in
// either case the flag persisted for that name in the package's .install_state.json wins.
//
// `registry_precheck`, when set, is called with the fully materialized plugin AFTER its
// capabilities exist but BEFORE on_load() runs. Returning a non-empty string aborts the load with
// that error and no on_load()/on_unload() cycle is ever paid — this is how the manager rejects a
// duplicate package, a capability-name collision, or a load that was cancelled while it ran.
//
// On failure everything materialized is torn down (on_unload() is called only on the capabilities
// that actually got on_load()) and `out` is left empty.
bool load(const PluginDescriptor&                          descriptor,
          bool                                             skip_deps,
          const std::vector<std::string>&                  capabilities_to_enable,
          const std::function<std::string(const Plugin&)>& registry_precheck,
          Plugin&                                          out,
          std::string&                                     error);

// Run on_unload() on every capability, drop them, and release the module. Safe to call on a
// not-loaded Plugin, and safe after the interpreter has been finalized (in which case the module
// reference is deliberately leaked rather than DECREF'd).
void unload(Plugin& plugin);

// Install Python dependencies into the shared packages directory via the bundled uv. Blocks, with
// a 120 s cap.
bool install_packages(const std::vector<std::string>& pkgs, std::string& error);

// Read a local .py/.whl package's metadata without installing it, and report whether a package is
// already installed under the same directory.
bool inspect_local_plugin_package(const boost::filesystem::path& filepath,
                                  PluginDescriptor&              plugin_descriptor,
                                  bool&                          existing_installation,
                                  std::string&                   error);

// Copy a .py/.whl package into the plugin directory (the per-user cloud directory when the
// descriptor carries a cloud UUID and cloud_user_id is non-empty) and write its
// .install_state.json sidecar, backing up and restoring any existing installation on failure.
bool install_plugin(const boost::filesystem::path& filepath,
                    const std::string&             cloud_user_id,
                    PluginDescriptor&              plugin_descriptor,
                    std::string&                   error);
bool install_plugin(const boost::filesystem::path& filepath, const std::string& cloud_user_id, std::string& error);

} // namespace Slic3r::plugin_loader
