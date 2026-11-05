#pragma once

#include "PluginDescriptor.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r::plugin_wheel_install {

// Shared wheel extraction root inside the plugin root. Dependency wheels are extracted into it,
// and the entry wheel is installed into <extraction root>/<entry package>. This is the same path
// the pre-uv loader extracted the entry wheel to, so there is one cache location. The "__" prefix
// keeps discovery from treating it as a plugin package (see is_ignored_plugin_directory()).
inline constexpr const char* EXTRACTED_DIR_NAME = "__whl_extracted__";

// Obsolete private uv target from before the entry wheel moved into EXTRACTED_DIR_NAME. Only the
// cache-clear action refers to it, to remove leftovers from older builds.
inline constexpr const char* LEGACY_INSTALL_DIR_NAME = "__wheel_install__";

// Staging and retirement directories, also inside the plugin root and ignored by discovery.
inline constexpr const char* STAGING_DIR_PREFIX  = "__wheel_staging__-";
inline constexpr const char* PREVIOUS_DIR_PREFIX = "__wheel_prev__-";

// Marker written into the target before it is promoted. Bump INSTALL_FORMAT when the layout or
// the uv invocation changes, so an existing target is rebuilt instead of reused.
inline constexpr const char* MARKER_FILE_NAME = "__wheel_install__.json";
inline constexpr int         INSTALL_FORMAT   = 2;

inline constexpr std::chrono::seconds INSTALL_TIMEOUT{120};

// Runs `uv <args>` and reports success. Injectable so tests can exercise staging, promotion and
// reuse without a real uv binary.
using UvRunner = std::function<bool(const std::vector<std::string>& args, std::chrono::seconds timeout, std::string& error)>;

// True for a filename shaped like a PEP 427 wheel: name-version(-build)?-python-abi-platform.whl,
// where the three trailing components look like real interpreter/ABI/platform tags. uv performs
// the full metadata and compatibility validation; this only decides whether the stored name can be
// handed to uv as-is or a metadata-derived copy has to be staged.
bool is_valid_wheel_filename(const std::string& filename);

// Build a PEP 427 wheel filename from metadata. Returns an empty string when any component is
// empty or cannot be escaped into the allowed character set.
std::string make_wheel_filename(const std::string& name, const std::string& version, const std::string& tag);

// Return a python-abi-platform tag for a staged wheel filename from the tags the wheel declares.
// A declared tag compatible with the bundled interpreter is preferred (so a wheel declaring both
// py2 and py3 uses py3); this never invents a tag. Among the chosen tags, simple platform
// alternatives are combined into one PEP 425 compressed tag set. uv performs the final
// platform/architecture compatibility check. Empty when no well-formed tag exists.
std::string wheel_filename_tag(const std::vector<std::string>& tags);

// True for a safe dotted package path: non-empty components separated by '.', with no ASCII path
// separators, control characters, or characters Windows forbids in filenames. Bytes >= 0x80 are
// allowed, so non-ASCII (Unicode) import names are not rejected. This is path-component safety,
// not a Python identifier check; an unimportable name is reported when the module is imported.
bool is_valid_entry_package(const std::string& entry_package);

// Path of the installed target: <plugin_root>/__whl_extracted__/<entry package>. Does not touch
// the disk. Empty when entry_package is not a safe import name.
std::string install_dir(const std::string& plugin_root, const std::string& entry_package);

// True when the target exists and the marker proves it was installed from the current entry wheel
// for this interpreter and entry package. An empty reason means "reusable".
bool is_installed(const PluginDescriptor& descriptor, std::string* reason = nullptr);

// Install the plugin's entry wheel with uv into the legacy extraction path
// <plugin_root>/__whl_extracted__/<entry package> unless a matching installation already exists.
// An existing markerless directory there is a pre-uv extraction and is replaced: it is cache, not
// plugin data. Directories for dependency wheels elsewhere under __whl_extracted__/ are never
// touched; a dependency whose filename stem equals the entry package is reported as a collision
// instead of being replaced. The wheel is installed to a fresh staging target and promoted only
// after a successful, complete installation; the previous target is restored on failure. On
// failure `error` explains the cause and no partial target is importable.
bool ensure_installed(const PluginDescriptor& descriptor, std::string& error, const UvRunner& runner = {});

} // namespace Slic3r::plugin_wheel_install
