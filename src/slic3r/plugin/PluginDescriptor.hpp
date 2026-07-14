#pragma once

#include "PythonPluginInterface.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// Cloud overlay on a PluginDescriptor; presence means the descriptor is cloud-backed.
struct CloudPluginState
{
    std::string uuid;             // Cloud service UUID without the cloud: key prefix.
    bool installed        = false; // Cloud package exists locally and can be loaded.
    bool update_available = false; // Cloud version > the local package version.
    bool unauthorized     = false; // Cloud plugin is valid locally, but cannot receive cloud updates.
    bool is_mine          = false; // Plugin was created (and uploaded) by the current user.
};

enum class PluginUpdateStatus
{
    Normal,
    UpdateAvailable,
    Unauthorized,
};

struct PluginChangelog {
    std::string changelog_id;
    std::string plugin_uuid;
    std::string version;
    std::string changelog;
    long long created_time = 0;
};

inline void sort_plugin_changelog(std::vector<PluginChangelog>& changelog)
{
    std::sort(changelog.begin(), changelog.end(), [](const PluginChangelog& lhs, const PluginChangelog& rhs) {
        if (lhs.created_time != rhs.created_time)
            return lhs.created_time > rhs.created_time;
        return lhs.version > rhs.version;
    });
}

// Canonical plugin runtime/catalog representation used by Orca.
struct PluginDescriptor
{
    std::string plugin_key;                             // OrcaSlicer-generated operational identity
    std::string name;                                   // Display name
    std::string description;                            // Plugin description
    std::string author;                                 // Plugin author from manifest, if available
    std::string version;                                // Selected plugin version
    std::string latest_version;                         // Latest available cloud version fallback when changelog is unavailable.
    std::string installed_version;                       // Locally installed package version. Preserved across cloud merges, which overwrite `version` with the latest cloud version. Empty when not installed.
    std::vector<std::string> display_types;             // Display-only "compatibility" labels (cloud: raw service labels; local: from real capabilities). Never used for dispatch.
    std::string plugin_root;                            // Installed plugin directory, even when entry_path is invalid or ambiguous.
    std::string entry_path;                             // Full path to the installed plugin entry file
    std::string entry_package;                          // Import package/module used for package-based loading
    std::vector<std::string> dependencies;              // Python dependency requirements declared by plugin package metadata
    std::map<std::string, std::string> settings;        // [tool.orcaslicer.plugin.settings] table -> per-plugin params (ctx.params)
    std::vector<PluginChangelog> changelog;             // Cloud release changelog, sorted newest-first when available.

    std::string error;                     // Blocking error message. Non-empty means the plugin is in an error state.
    std::optional<CloudPluginState> cloud; // Extra cloud state layered on top of a normal plugin descriptor.
    bool metadata_valid = false;           // Manifest/package validity stays separate from the user-facing error field.
    // Package auto-load flag, read from .install_state.json. Defaults to FALSE: a package with no
    // sidecar has never been installed through Orca and carries no auto-load intent, so it must not
    // be loaded at startup. Installing a package writes the sidecar with enabled = true.
    bool enabled = false;
    std::string sharing_token;             // Use BASE_URL/p/SHARING_TOKEN to open relevant plugin in browser.
    std::string thumbnail_url;             // Cloud main_image pre-signed (access_url) thumbnail; empty for local plugins. Display-only.

    bool is_cloud_plugin() const { return cloud.has_value(); }
    std::string cloud_uuid() const { return cloud.has_value() ? cloud->uuid : std::string{}; }
    bool has_local_package() const { return !is_cloud_plugin() || cloud->installed || !plugin_root.empty() || !entry_path.empty(); }
    bool is_metadata_valid() const { return metadata_valid; }

    // A package that exists on disk but whose manifest could not be parsed.
    //
    // Deliberately not just !is_metadata_valid(): a cloud plugin that is merely available
    // (subscribed but not installed) also has metadata_valid == false — the cloud service never
    // sets it — yet it is a valid catalog row, not a broken package. What separates the two is
    // whether there is a local package behind the descriptor at all.
    bool is_invalid_package() const { return !metadata_valid && has_local_package(); }

    std::string normalized_error() const
    {
        auto begin = std::find_if_not(error.begin(), error.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
        if (begin == error.end())
            return {};

        auto end = std::find_if_not(error.rbegin(), error.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
        return std::string(begin, end);
    }

    bool has_error() const { return !normalized_error().empty(); }

    PluginUpdateStatus get_update_status() const
    {
        if (!cloud.has_value())
            return PluginUpdateStatus::Normal;
        if (cloud->unauthorized)
            return PluginUpdateStatus::Unauthorized;
        if (cloud->update_available)
            return PluginUpdateStatus::UpdateAvailable;
        return PluginUpdateStatus::Normal;
    }

    bool has_update_available() const { return get_update_status() == PluginUpdateStatus::UpdateAvailable; }
    bool is_unauthorized() const { return get_update_status() == PluginUpdateStatus::Unauthorized; }
    std::string latest_available_version() const
    {
        for (const PluginChangelog& entry : changelog) {
            if (!entry.version.empty())
                return entry.version;
        }
        if (!latest_version.empty())
            return latest_version;
        return version;
    }

    void set_metadata_valid(bool is_valid)
    {
        metadata_valid = is_valid;
        if (!is_valid && !has_error())
            error = "Plugin metadata is invalid.";
    }

    void set_unauthorized(bool unauthorized)
    {
        if (!cloud.has_value())
            return;
        cloud->unauthorized = unauthorized;
        if (unauthorized)
            cloud->update_available = false;
    }

    void clear_error() { error.clear(); }
    void set_error(std::string message) { error = std::move(message); }
};

inline void apply_plugin_metadata_fallbacks(PluginDescriptor& target, const PluginDescriptor& fallback)
{
    if (target.name.empty())
        target.name = fallback.name;
    if (target.description.empty())
        target.description = fallback.description;
    if (target.author.empty())
        target.author = fallback.author;
    if (target.version.empty())
        target.version = fallback.version;
    if (target.entry_package.empty())
        target.entry_package = fallback.entry_package;
    if (target.dependencies.empty())
        target.dependencies = fallback.dependencies;
}

// Sanitize a value for use as a filesystem name and as a local plugin_key:
// keeps [A-Za-z0-9_-.], collapses any other run into a single '_'.
inline std::string filesystem_safe_escape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.') {
            escaped += static_cast<char>(ch);
            continue;
        }
        if (escaped.empty() || escaped.back() != '_')
            escaped += '_';
    }
    return escaped.empty() ? "path" : escaped;
}

// Plugin display names are serialized into ';'-delimited config/preset strings
// (see escape_strings_cstyle in libslic3r/Config.cpp), so a ';' in a name would
// corrupt that encoding. Replace any ';' with '_' and otherwise leave the name
// untouched. Capability names are validated more strictly elsewhere — a ';' there
// is treated as an error because those names drive preset dispatch, not display.
inline std::string sanitize_plugin_name(std::string name)
{
    std::replace(name.begin(), name.end(), ';', '_');
    return name;
}

// True when s is a canonical 8-4-4-12 hex UUID, e.g. 550e8400-e29b-41d4-a716-446655440000.
inline bool is_uuid(const std::string& s)
{
    if (s.size() != 36)
        return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (ch != '-')
                return false;
        } else if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) {
            return false;
        }
    }
    return true;
}

// Key generation helpers.
// Local key = filesystem-safe plugin file stem (filename without extension), e.g. "Slow_Load_Plugin".
// Cloud key = the cloud UUID. Local vs cloud is determined from the descriptor's cloud
// state, never by parsing the key. Plugin keys are matched by plain equality.
inline std::string make_local_plugin_key(const std::string& stem)
{
    return filesystem_safe_escape(stem);
}

} // namespace Slic3r
