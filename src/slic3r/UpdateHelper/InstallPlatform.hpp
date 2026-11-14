#ifndef slic3r_Updater_InstallPlatform_hpp_
#define slic3r_Updater_InstallPlatform_hpp_

// Per-platform halves of apply_update(). Exactly one is compiled for real in a
// given binary; the others compile to nothing so the target builds everywhere.

#include <string>

#include "libslic3r/UpdateHandoff.hpp"
#include "slic3r/UpdateHelper/InstallApply.hpp"

namespace Slic3r {
namespace Updater {

// Each returns false with `error` filled when the install did not happen, or
// when a change already made was rolled back. `launch` starts the new build once
// it is in place; the install after a quit passes false.
bool apply_windows_install(const UpdateHandoff& handoff, const std::string& artifact_path, ProgressSink& sink, std::string& error, bool launch = true);
bool apply_macos_install(const UpdateHandoff& handoff, const std::string& artifact_path, ProgressSink& sink, std::string& error, bool launch = true);

// Shared: delete a path (file or folder) without failing the caller, and launch
// the new build detached.
void remove_path_quietly(const std::string& path);
bool launch_detached(const std::string& exe_path, std::string& error);

} // namespace Updater
} // namespace Slic3r

#endif // slic3r_Updater_InstallPlatform_hpp_
