#pragma once

#include <memory>
#include <string>

namespace Slic3r {

// A shared, process-local lease on one installation's browser profile. The lease owns the
// non-blocking cross-process lock on the profile's data directory; it lives as long as a webview
// that uses the profile. `std::shared_ptr<void>` keeps this type out of UI headers.
using BrowserProfileLease = std::shared_ptr<void>;

// One loaded instance's browser-profile identity. Refreshing this object binds UI and callbacks
// to a particular load: a reload creates a new object even when the profile id is unchanged, so
// callbacks captured from the old instance cannot run against the replacement.
struct PluginBrowserProfileIdentity
{
    std::string plugin_key;
    std::string profile_id;
    std::string plugin_root;          // package root this load instance was bound to
    std::string installation_revision; // sidecar revision of that package
};
using PluginBrowserProfileIdentityRef = std::shared_ptr<const PluginBrowserProfileIdentity>;

// Returns the installation-scoped browser profile id for `plugin_key`, creating and persisting a
// random id before any plugin code runs when the sidecar has none. A pre-identity directory found
// under the old plugin-key name is never reused, moved or deleted; fresh=true reports that a fresh
// profile was started because of it. Throws std::runtime_error when the sidecar cannot be read or
// written.
std::string ensure_plugin_browser_profile(const std::string& plugin_key, bool* fresh = nullptr,
                                          std::string* legacy_path = nullptr,
                                          std::string* installation_revision = nullptr,
                                          const std::string* expected_root = nullptr,
                                          const std::string* expected_revision = nullptr);

// Binds one load instance to its profile id. Call before plugin code runs; an existing binding
// for the key is replaced, so callbacks from the previous load become stale. Compares the
// descriptor's selected root/revision with the sidecar before binding: a stale load of a
// replaced installation throws instead of adopting the replacement's identity.
PluginBrowserProfileIdentityRef begin_plugin_browser_profile(const std::string& plugin_key,
                                                             const std::string& expected_plugin_root,
                                                             const std::string& expected_revision);

// Drops the binding for a key on unload or a failed load.
void end_plugin_browser_profile(const std::string& plugin_key);

// The binding for the loaded instance, or null when the plugin is not bound.
PluginBrowserProfileIdentityRef current_plugin_browser_profile_identity(const std::string& plugin_key);

// True when `identity` is still the current binding for its key. A reload, keep-local rekey or
// unload invalidates every previous identity, even when the underlying profile id is unchanged.
bool plugin_browser_profile_is_current(const PluginBrowserProfileIdentityRef& identity);

// Re-reads the identity's own package root and requires the same profile id and installation
// revision. Use at UI creation and before persisting audit state, not on every message callback.
bool plugin_browser_profile_installation_is_current(const PluginBrowserProfileIdentityRef& identity);

// Acquires a non-blocking lease on the identity's profile id. Returns null when another process
// owns the profile or the load instance was already replaced; callers must not create a webview
// then.
BrowserProfileLease acquire_plugin_browser_profile_lease(const PluginBrowserProfileIdentityRef& identity);

// Reports (once) that this process could not take the profile lease, so plugin UI was not opened.
void warn_plugin_browser_profile_in_use(const std::string& plugin_key);

// Reports (once) that a reinstall replaced the old profile with a fresh identity.
void warn_plugin_browser_profile_reset(const std::string& plugin_key);

// Reports (once) that uninstall could not identify a profile to delete, so old profile data may
// remain on disk.
void warn_plugin_browser_profile_cleanup_unknown(const std::string& plugin_key);

// Reports (once) that an old plugin-key-named profile exists and is not used by this installation.
// Used when the identity is created outside the normal load path (keep-local rekeying).
void warn_legacy_plugin_browser_profile(const std::string& plugin_key, const std::string& current_profile_id);

// Attempts to delete the profile after the plugin package has been removed. Non-blocking:
// another process's lease means the cleanup is skipped and a warning is shown. When this process
// still has a closing view, the attempt is retried on the GUI thread. A tombstone rename ensures
// the live directory is removed before recursive deletion. `installation_root` is checked under
// the cross-process install-state lock on every attempt: if a replacement package exists there,
// its profile is left untouched. Pass an empty root when the caller has just installed a new
// package at a known root (the local reinstall path).
void delete_plugin_browser_profile(const std::string& plugin_key, const std::string& profile_id,
                                   const std::string& installation_root = std::string());

} // namespace Slic3r
