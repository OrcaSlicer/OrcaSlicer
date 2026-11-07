#pragma once

#include <libslic3r/Config.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/PrinterAssetRef.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// One loaded and enabled ResourcePrinterAsset capability (a printer asset pack) and its files.
struct PrinterAssetPackEntry
{
    PluginCapabilityId       id;
    std::string              plugin_key;
    std::string              pack_id;      // <plugin id> in a reference: the cloud uuid, else the plugin key
    std::string              display_name; // the capability name, e.g. "Voron Bed by sam"
    std::string              asset_dir;
    std::string              plugin_root;
    std::vector<std::string> files;        // validated absolute paths, at most 16
};

// Tracks every loaded and enabled ResourcePrinterAsset capability by id and resolves printer asset references.
// Holds strings only: a shared_ptr here would keep a Python object alive past Py_Finalize.
class PluginPrinterAssets
{
public:
    static PluginPrinterAssets &instance();

    PluginPrinterAssets(const PluginPrinterAssets &)            = delete;
    PluginPrinterAssets &operator=(const PluginPrinterAssets &) = delete;

    // Each returns true when the set of packs changed, so callers redraw the bed only then.
    bool on_plugin_register(const std::string &plugin_key);
    bool on_plugin_deregister(const std::string &plugin_key);
    bool on_cap_register(const PluginCapabilityId &id);
    bool on_cap_deregister(const PluginCapabilityId &id);
    void initial_sweep();

    std::vector<PrinterAssetPackEntry> entries() const;

    // The installed file `ref` names, or empty when its pack is not loaded or lacks the file.
    std::string resolve(const PrinterAssetRef &ref) const;
    // The capability name of the loaded pack `ref` names, or empty when it is not loaded.
    std::string capability_name(const PrinterAssetRef &ref) const;

private:
    PluginPrinterAssets() = default;

    std::optional<PrinterAssetPackEntry> build_entry(const PluginCapabilityId &id) const;

    mutable std::mutex                                m_mutex;
    std::map<PluginCapabilityId, PrinterAssetPackEntry> m_entries;
};

// Resolves a bed_custom_model / bed_custom_texture value. A printer asset reference becomes the
// pack's installed file, or empty when the pack is not loaded here. Any other value (a path, or
// empty) is returned unchanged.
std::string resolve_printer_asset_value(const std::string &value);

// Points a printer config's printer_asset_plugins at the packs its bed_custom_model and
// bed_custom_texture come from, and rebuilds its manifest, so a machine without those packs is offered
// them. A pack that is not installed here keeps the name its manifest entry holds, in the config's own
// manifest or in `fallback_manifest` (e.g. the saved preset's). Does nothing for a config without
// printer_asset_plugins.
void update_printer_asset_plugins(DynamicPrintConfig &config, const ConfigOptionStrings *fallback_manifest = nullptr);

} // namespace Slic3r
