#pragma once

#include <libslic3r/Config.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/PrinterAssetRef.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/plugin/ResourcePackKinds.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r {

// One loaded and enabled ResourcePack or ResourceTexture capability and its files, grouped by the kind each is declared for.
struct ResourcePackEntry
{
    PluginCapabilityId  id;
    std::string         plugin_key;
    std::string         pack_id;      // <plugin id> in a reference: the cloud uuid, else the plugin key
    std::string         display_name; // the capability name, e.g. "Voron Bed by sam"
    std::string         resource_dir;
    std::string         plugin_root;
    ResourceFilesByKind files;        // kind -> validated absolute paths
};

// Tracks every loaded and enabled ResourcePack and ResourceTexture capability by id and resolves printer asset
// references. ResourceTexture is kept as its own type for existing texture plugins and their manifests; its
// textures are served here as displacement_texture files. Holds strings only: a shared_ptr here would keep a Python
// object alive past Py_Finalize.
class PluginResourcePacks
{
public:
    static PluginResourcePacks &instance();

    PluginResourcePacks(const PluginResourcePacks &)            = delete;
    PluginResourcePacks &operator=(const PluginResourcePacks &) = delete;

    // Each returns true when the set of packs changed, so callers redraw the bed only then.
    bool on_plugin_register(const std::string &plugin_key);
    bool on_plugin_deregister(const std::string &plugin_key);
    bool on_cap_register(const PluginCapabilityId &id);
    bool on_cap_deregister(const PluginCapabilityId &id);
    void initial_sweep();

    std::vector<ResourcePackEntry> entries() const;

    // The installed file `ref` names when the pack declares it for `kind`, or empty.
    std::string resolve(const PrinterAssetRef &ref, std::string_view kind) const;
    // The capability name of the loaded pack `ref` names, or empty when it is not loaded.
    std::string capability_name(const PrinterAssetRef &ref) const;

private:
    PluginResourcePacks() = default;

    std::optional<ResourcePackEntry> build_entry(const PluginCapabilityId &id) const;

    mutable std::mutex                              m_mutex;
    std::map<PluginCapabilityId, ResourcePackEntry> m_entries;
};

// Resolves a printer asset reference in a bed_custom_model / bed_custom_texture value, as `kind`
// (RESOURCE_KIND_BED_MODEL / RESOURCE_KIND_BED_TEXTURE). A reference becomes the pack's installed file,
// or empty when the pack is not loaded here or does not declare that file for `kind`. Any other value
// (a path, or empty) is returned unchanged.
std::string resolve_printer_asset_value(const std::string &value, std::string_view kind);

// Points a printer config's printer_asset_plugins at the packs its bed_custom_model and
// bed_custom_texture come from, and rebuilds its manifest, so a machine without those packs is offered
// them. A pack that is not installed here keeps the name its manifest entry holds, in the config's own
// manifest or in `fallback_manifest` (e.g. the saved preset's). Does nothing for a config without
// printer_asset_plugins.
void update_printer_asset_plugins(DynamicPrintConfig &config, const ConfigOptionStrings *fallback_manifest = nullptr);

} // namespace Slic3r
