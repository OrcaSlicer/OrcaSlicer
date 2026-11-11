#pragma once

#include "PluginDescriptor.hpp"

#include <boost/filesystem/path.hpp>

#include <mutex>
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
    // Thread creation is a yes/no grant. Newer sidecars store a boolean; older ones stored an
    // array (empty when denied, nonempty when granted), and nonempty strings are accepted by the
    // same migration rule. The reader never rewrites; the next normal write emits a boolean.
    bool threading = false;
    // Grants for audit events that expose no target, keyed "<category>:<event>" (e.g.
    // "Socket:socket.__new__"). Matched verbatim; the filesystem wildcard does not apply.
    std::vector<std::string> events;
    // Turns auditing off for the whole plugin: every audited event is allowed without a grant or
    // prompt, except the unconditional denied paths (the install-state sidecar, the app config,
    // the cloud refresh token), which stay blocked. Stored as a JSON boolean; a missing or
    // non-boolean value reads as false. This is a Python audit-hook policy, not an OS sandbox.
    bool full_access = false;
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

// Find the single plugin entry file (.py or .whl) in a directory.
// Ignores __whl_extracted__ and hidden files/dirs.
// Returns the path to the entry file, or an empty path with error set if zero or multiple candidates.
boost::filesystem::path find_installed_plugin_entry(const boost::filesystem::path& plugin_dir, std::string& error);

// Validated non-recursive .png/.jpg/.jpeg/.bmp files directly inside `dir`, every one contained in
// `plugin_root`. Empty with `error` set on failure.
std::vector<std::string> collect_resource_texture_files(const std::string& dir,
                                                        const std::string& plugin_root,
                                                        std::string& error);

// Process-wide mutex serializing every in-process read and read-modify-write of a plugin's
// .install_state.json sidecar. Hold it across the read and the matching write so two writers
// cannot overwrite each other's grants, and around standalone reads so a reader cannot observe a
// state while another thread is replacing it. It is not a file lock: another OrcaSlicer process
// writing the same sidecar is not serialized.
//
// Lock order: install-state mutex -> PluginManager::m_mutex. Code that already holds the manager
// lock must not acquire this one.
std::mutex& install_state_mutex();

// Canonical writer: emits the .install_state.json schema for the given state.
// The file is replaced strictly atomically: the payload is written to a uniquely named temporary
// in the sidecar's own directory, flushed and closed with the stream state checked, then moved
// over the target with the platform replace primitive (POSIX rename(2), Windows MoveFileExW with
// MOVEFILE_REPLACE_EXISTING). There is no in-place fallback. On any failure the temporary is
// removed and the original sidecar is left untouched; the function returns false.
// A full read-modify-write caller must hold install_state_mutex() across its read and this write.
bool write_install_state(const boost::filesystem::path& plugin_dir, const PluginInstallState& state);

// Builds a PluginInstallState from the descriptor and delegates to the canonical writer. This
// overload reads the sidecar itself, so callers must hold install_state_mutex(). An existing
// sidecar that cannot be read is never replaced: the write fails and leaves it untouched. A
// missing sidecar is a new plugin, so the metadata is seeded and written.
bool write_install_state(const boost::filesystem::path& plugin_dir, const PluginDescriptor& entry, bool enabled,
                         const std::vector<std::pair<std::string, bool>>& capabilities);
// Convenience overload: write(dir, entry, /*enabled=*/true, /*capabilities=*/{}).
bool write_install_state(const boost::filesystem::path& plugin_dir, const PluginDescriptor& entry);

// Fill the descriptor-derived install metadata into a sidecar state that does not describe an
// installed plugin yet, and mark it enabled. Leaves permissions and capabilities untouched.
// Single source for the metadata mapping used by the install writer and by the audit prompt path
// that creates a sidecar on demand.
void seed_install_metadata(PluginInstallState& state, const PluginDescriptor& entry);

// Outcome of a sidecar read. Callers that may write a sidecar must distinguish Absent from
// Unreadable: a missing file is a new install whose metadata may be seeded, while an existing file
// that cannot be parsed -- or that cannot be examined because of a filesystem error -- holds state
// that must not be replaced. Only Absent and Readable are safe to overwrite.
enum class InstallStateReadStatus {
    Absent,    // no entry at the sidecar path
    Readable,  // parsed successfully into `out`
    Unreadable // the path exists but is not a regular readable file, or its contents are malformed
};

// Reads the sidecar and reports why it failed. `out` is only assigned on Readable. When `malformed`
// is given, it is set to whether the result is Unreadable because the sidecar is a regular file
// (not a link) that reads in full but does not parse as a JSON object, as opposed to a path that
// cannot be examined, opened or read in full. Reading such a file again cannot recover it, so a
// plugin load sets it aside (see plugin_loader::load); every other caller treats it as Unreadable.
// Caller must hold install_state_mutex(); see the note on read_install_state below.
InstallStateReadStatus read_install_state_status(const boost::filesystem::path& plugin_dir, PluginInstallState& out,
                                                 bool* malformed = nullptr);

// Reads install state back into the descriptor; plugin_key is always derived. Returns whether the
// sidecar was present and valid.
// Caller must hold install_state_mutex(); it is not recursive, so a caller already holding the
// lock must call this raw overload, not PluginManager::get_install_state().
bool read_install_state(const boost::filesystem::path& plugin_dir, PluginDescriptor& entry);
// Full read of the sidecar; returns false if there is no/invalid sidecar.
// Caller must hold install_state_mutex(); see the note above.
bool read_install_state(const boost::filesystem::path& plugin_dir, PluginInstallState& out);

} // namespace Slic3r
