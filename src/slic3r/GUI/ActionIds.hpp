#pragma once

// The built-in action source's key and name, and the prefix of a command's tile id. No wx here: this header is part
// of the library libslic3r_automation, which is built without wx.
namespace Slic3r { namespace GUI {

// Stable identity/display name of the built-in ("OrcaSlicer") action source. Shared by the native
// command catalog and the dynamically materialised setting/plate/recent actions so every built-in
// action re-keys together.
inline constexpr const char* kOrcaSourceKey  = "orca";
inline constexpr const char* kOrcaSourceName = "OrcaSlicer";
// The prefix of a catalog command's tile id, "orca_command:<key>:orca" (NativeCommands::make_action()).
inline constexpr const char* kCommandPrefix = "orca_command";

}} // namespace Slic3r::GUI
