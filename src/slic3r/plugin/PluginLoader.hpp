#pragma once

#include "PluginDescriptor.hpp"

#include <boost/filesystem/path.hpp>

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
struct Plugin;
} // namespace Slic3r

namespace Slic3r::plugin_loader {
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
