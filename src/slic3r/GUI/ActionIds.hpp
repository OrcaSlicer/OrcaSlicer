#pragma once

#include <string>

// The built-in action source's key and name, and the prefix of the id of a command's action. No wx here: this header
// is part of the library libslic3r_automation, which is built without wx.
namespace Slic3r { namespace GUI {

// Stable identity/display name of the built-in ("OrcaSlicer") action source. Shared by the native
// command catalog and the dynamically materialised setting/plate/recent actions so every built-in
// action re-keys together.
inline constexpr const char* kOrcaSourceKey  = "orca";
inline constexpr const char* kOrcaSourceName = "OrcaSlicer";
// The prefix of the id of a catalog command's action, "orca_command:<key>:orca" (NativeCommands::make_action()).
inline constexpr const char* kCommandPrefix = "orca_command";

// The key inside the id of a command's action, "orca_command:<key>:orca"; `id` itself when it has no colon, the name
// inside the id, which action_run takes in its place; empty for any other action's id.
inline std::string command_action_key(const std::string& id)
{
    if (id.find(':') == std::string::npos)
        return id;
    const std::string prefix = std::string(kCommandPrefix) + ':', suffix = std::string(":") + kOrcaSourceKey;
    if (id.size() > prefix.size() + suffix.size() && id.compare(0, prefix.size(), prefix) == 0 &&
        id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0)
        return id.substr(prefix.size(), id.size() - prefix.size() - suffix.size());
    return {};
}

}} // namespace Slic3r::GUI
