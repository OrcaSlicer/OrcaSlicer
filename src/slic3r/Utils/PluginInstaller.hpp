// Phase 4.3.1 — PluginInstaller.
//
// Accepts a .orcaplugin (zip) file, validates that it contains a
// manifest.json with the required identity fields + a permission
// bitfield, and copies its contents into data_dir/plugins/<id>/. After
// extraction the caller is expected to ask the user for permission
// (PluginPermissionDialog) and then drive PluginManager::load_plugin /
// reload_plugin.
//
// Lives in slic3r/Utils — pulls miniz from libslic3r but never
// wxWidgets, so this file is reusable from non-GUI contexts (e.g. the
// future `orca install <path>` CLI).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace Slic3r {

class PluginInstaller {
public:
    /// Manifest identity extracted from the .orcaplugin's manifest.json.
    struct Identity {
        std::string   id;
        std::string   name;
        std::string   version;
        std::string   author;
        std::string   description;
        std::uint64_t permissions = 0;
        std::string   kind;    // "" / "wasm" / "webview" — drives PluginManager dispatch
    };

    enum class Status {
        Ok,
        InvalidArchive,      // zip open failed
        ManifestMissing,     // archive has no manifest.json at root
        ManifestMalformed,   // manifest parses but lacks required fields
        IdMissing,
        DestinationExists,   // <data_dir>/plugins/<id> already populated
        DestinationCreateFailed,
        ExtractFailed,
    };

    struct Result {
        Status                status = Status::Ok;
        Identity              identity;
        std::filesystem::path install_dir;   // where files landed
        std::string           error_message; // empty when status == Ok
    };

    /// Open `archive_path` and parse its manifest.json without extracting
    /// anything else. Use this to drive the permission prompt: the
    /// returned Identity carries the permission bitfield to display.
    static Result peek_identity(const std::filesystem::path& archive_path);

    /// Extract the archive into `<plugins_root>/<identity.id>/`,
    /// overwriting any existing files in that directory. Returns
    /// DestinationExists if the install dir is non-empty and
    /// `replace_existing` is false.
    static Result install(const std::filesystem::path& archive_path,
                          const std::filesystem::path& plugins_root,
                          bool replace_existing = false);
};

} // namespace Slic3r
