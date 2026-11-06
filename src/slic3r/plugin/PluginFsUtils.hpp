#pragma once

#include "PluginDescriptor.hpp"

#include <boost/filesystem/path.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#define PLUGIN_SUBSCRIBED_DIR "_subscribed"
#define PLUGIN_DATA_DIR "plugin_data"

namespace Slic3r {

extern const char* const INSTALL_STATE_FILE;

struct PluginPermissions
{
    // Filesystem targets. A target ending in "/*" (e.g. "/path/to/folder/*") grants the folder
    // and its whole subtree, at any depth; "*" is the only wildcard and only as the final path
    // component. Reads and writes stay separate buckets: a wildcard in one never grants the other.
    std::vector<std::string> fs_read;
    std::vector<std::string> fs_readwrite;
    std::vector<std::string> network_http;
    std::vector<std::string> network_socket;
    std::vector<std::string> process;
    std::vector<std::string> threading;
    // Grants for audit events that expose no target, keyed "<category>:<event>" (e.g.
    // "Socket:socket.__new__"). Matched verbatim; the filesystem wildcard does not apply.
    std::vector<std::string> events;
};

struct PluginInstallState {
    std::string installed_from;      // "local" | "cloud"
    std::string installed_version;
    std::string plugin_name;
    std::string cloud_uuid;          // empty for local

    PluginPermissions permissions;

    bool enabled = true;
    std::vector<std::pair<std::string, bool>> capabilities; // name -> enabled, ordered
};

// Returns the cloud plugin install/scan directory for a given user_id.
// Path: {data_dir}/orca_plugins/_subscribed/{user_id}/
std::string get_cloud_plugin_dir(const std::string& user_id);

std::string get_orca_plugins_dir();

boost::filesystem::path resolve_plugin_root_from_descriptor(const PluginDescriptor& descriptor);

bool is_plugin_root_allowed(const boost::filesystem::path& candidate_root,
                            const std::vector<std::string>& allowed_dirs);

bool resolve_allowed_plugin_root(const PluginDescriptor& descriptor,
                                 const std::vector<std::string>& allowed_dirs,
                                 const std::string& out_of_scope_error,
                                 boost::filesystem::path& resolved_root,
                                 std::string& error);

bool delete_plugin_root(const boost::filesystem::path& resolved_root,
                        const std::string& plugin_id,
                        std::string& error);

// The directories plugins are discovered from: {data_dir}/orca_plugins, plus the per-user cloud
// directory when cloud_user_id is non-empty.
//
// NOTE: this CREATES the directories if they do not exist. Callers rely on that side effect — the
// install path writes into them without creating them itself.
std::vector<std::string> get_plugin_directories(const std::string& cloud_user_id);

// Scan the given directories for plugin packages (manifest-only; no Python is loaded, no state is
// kept). Pure: directories in, descriptors out.
//
// Returns every package found, valid and invalid alike: a package whose manifest could not be
// parsed comes back with metadata_valid == false and its error set (descriptor.is_invalid_package()).
// The package-level auto-load flag is read from each package's .install_state.json into
// descriptor.enabled. Capabilities are NOT discovered here — a package has none until it is loaded.
std::vector<PluginDescriptor> discover_plugin_packages(const std::vector<std::string>& dirs, std::string& error);

bool is_ignored_plugin_directory(const boost::filesystem::path& path);
bool is_safe_relative_path(const boost::filesystem::path& path);
bool is_valid_plugin_id(const std::string& id);
bool extract_zip_to_directory(const boost::filesystem::path& zip_path, const boost::filesystem::path& destination, std::string& error);

// Read PEP 723 inline script metadata from a .py plugin file.
// Populates metadata.dependencies and local identity fallbacks.
// Returns true on success (including when no PEP 723 block is found — deps will be empty).
bool read_python_plugin_metadata(const boost::filesystem::path& py_path, PluginDescriptor& descriptor, std::string& error);

// Read wheel metadata from a .whl plugin file (zip archive).
// Reads METADATA, WHEEL, RECORD, and top_level.txt from the .dist-info directory.
// Populates metadata.entry_package, metadata.dependencies, and local identity fallbacks.
// Returns true on success.
bool read_wheel_plugin_metadata(const boost::filesystem::path& whl_path, PluginDescriptor& descriptor, std::string& error);

// The subset of wheel metadata uv needs to accept a staged copy under a valid filename: the
// distribution name and version from METADATA and the declared WHEEL Tag lines.
struct WheelInstallMetadata
{
    std::string              name;
    std::string              version;
    std::vector<std::string> tags;
};
bool read_wheel_install_metadata(const boost::filesystem::path& whl_path, WheelInstallMetadata& out, std::string& error);

// Find the single plugin entry file (.py or .whl) in a directory.
// Ignores __whl_extracted__ and hidden files/dirs.
// Returns the path to the entry file, or an empty path with error set if zero or multiple candidates.
boost::filesystem::path find_installed_plugin_entry(const boost::filesystem::path& plugin_dir, std::string& error);

// Validated non-recursive .png/.jpg/.jpeg/.bmp files directly inside `dir`, every one contained in
// `plugin_root`. Empty with `error` set on failure.
std::vector<std::string> collect_resource_texture_files(const std::string& dir,
                                                        const std::string& plugin_root,
                                                        std::string& error);

// Canonical writer: emits the .install_state.json schema for the given state.
bool write_install_state(const boost::filesystem::path& plugin_dir, const PluginInstallState& state);

// Builds a PluginInstallState from the descriptor and delegates to the canonical writer.
bool write_install_state(const boost::filesystem::path& plugin_dir, const PluginDescriptor& entry, bool enabled,
                         const std::vector<std::pair<std::string, bool>>& capabilities);
// Convenience overload: write(dir, entry, /*enabled=*/true, /*capabilities=*/{}).
bool write_install_state(const boost::filesystem::path& plugin_dir, const PluginDescriptor& entry);

// Fill the descriptor-derived install metadata into a sidecar state that does not describe an
// installed plugin yet, and mark it enabled. Leaves permissions and capabilities untouched.
// Single source for the metadata mapping used by the install writer and by the audit prompt path
// that creates a sidecar on demand.
void seed_install_metadata(PluginInstallState& state, const PluginDescriptor& entry);

// Reads install state back into the descriptor; plugin_key is always derived. Returns whether the
// sidecar was present and valid.
bool read_install_state(const boost::filesystem::path& plugin_dir, PluginDescriptor& entry);
// Full read of the sidecar; returns false if there is no/invalid sidecar.
bool read_install_state(const boost::filesystem::path& plugin_dir, PluginInstallState& out);

} // namespace Slic3r

// uv-based installation of a wheel plugin's entry wheel.
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
// An existing markerless directory there is a pre-uv extraction: when it already contains the entry
// package it is used directly, without running uv. A markerless target without the entry package,
// and a target whose marker is stale or invalid, is replaced. Directories for dependency wheels
// elsewhere under __whl_extracted__/ are never touched; a dependency whose filename stem equals the
// entry package is reported as a collision instead of being replaced. The wheel is installed to a
// fresh staging target and promoted only after a successful, complete installation; the previous
// target is restored on failure. On failure `error` explains the cause and no partial target is
// importable.
bool ensure_installed(const PluginDescriptor& descriptor, std::string& error, const UvRunner& runner = {});

} // namespace Slic3r::plugin_wheel_install
