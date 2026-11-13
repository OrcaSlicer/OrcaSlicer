#include "PluginResourcePacks.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PrinterAssetRef.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"
#include "slic3r/plugin/PluginFsUtils.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include "slic3r/plugin/ResourcePackKinds.hpp"
#include "slic3r/plugin/pluginTypes/resourcePrinterAsset/ResourcePrinterAssetPluginCapability.hpp"
#include "slic3r/plugin/pluginTypes/resourceTexture/ResourceTexturePluginCapability.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Slic3r {

PluginResourcePacks &PluginResourcePacks::instance()
{
    static PluginResourcePacks registry;
    return registry;
}

std::optional<ResourcePackEntry> PluginResourcePacks::build_entry(const PluginCapabilityId &id) const
{
    // Query by id and re-check state; the manager copies the shared_ptr and releases its lock.
    auto capability = PluginManager::instance().get_plugin_capability(id, /*only_enabled=*/false);
    if (!capability || !capability->is_enabled())
        return std::nullopt;
    const PluginCapabilityType type = capability->type();

    // An empty directory means resolution failed at load, so the pack contributes nothing.
    std::string resource_dir;
    if (type == PluginCapabilityType::ResourcePrinterAsset) {
        auto asset_capability = std::dynamic_pointer_cast<ResourcePrinterAssetPluginCapability>(capability);
        if (!asset_capability)
            return std::nullopt;
        resource_dir = asset_capability->asset_dir();
        if (resource_dir.empty())
            return std::nullopt;
    } else if (type == PluginCapabilityType::ResourceTexture) {
        // Kept as its own type so existing texture plugins and their resource-texture manifests keep working.
        auto texture_capability = std::dynamic_pointer_cast<ResourceTexturePluginCapability>(capability);
        if (!texture_capability)
            return std::nullopt;
        resource_dir = texture_capability->texture_dir();
        if (resource_dir.empty())
            return std::nullopt;
    } else {
        return std::nullopt;
    }

    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor_for_capability(capability->name(), type, descriptor))
        return std::nullopt;

    ResourcePackEntry entry;
    entry.id           = capability->identity();
    entry.plugin_key   = entry.id.plugin_key;
    entry.display_name = capability->name();
    entry.resource_dir = resource_dir;
    entry.plugin_root  = descriptor.plugin_root;
    // References name cloud packs by their uuid, the same on every machine. Packs installed by hand
    // fall back to their plugin key, which only this machine knows.
    entry.pack_id = descriptor.cloud_uuid().empty() ? descriptor.plugin_key : boost::algorithm::to_lower_copy(descriptor.cloud_uuid());

    std::string error;
    if (type == PluginCapabilityType::ResourceTexture) {
        entry.files[std::string(RESOURCE_KIND_DISPLACEMENT_TEXTURE)] = collect_resource_texture_files(resource_dir, entry.plugin_root, error);
    } else {
        entry.files = collect_printer_asset_files(resource_dir, entry.plugin_root, error);
    }
    if (!error.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Resource pack '" << entry.display_name << "' was skipped: " << error;
        return std::nullopt;
    }
    return entry;
}

bool PluginResourcePacks::on_plugin_register(const std::string &plugin_key)
{
    bool changed = false;
    for (const PluginCapabilityType type : {PluginCapabilityType::ResourcePrinterAsset, PluginCapabilityType::ResourceTexture}) {
        for (const auto &capability : PluginManager::instance().get_plugin_capabilities(plugin_key, type)) {
            if (capability && on_cap_register(capability->identity()))
                changed = true;
        }
    }
    return changed;
}

bool PluginResourcePacks::on_plugin_deregister(const std::string &plugin_key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    bool changed = false;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->first.plugin_key == plugin_key) {
            it      = m_entries.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

bool PluginResourcePacks::on_cap_register(const PluginCapabilityId &id)
{
    std::optional<ResourcePackEntry> entry = build_entry(id);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!entry)
        return m_entries.erase(id) > 0;
    m_entries[entry->id] = std::move(*entry);
    return true;
}

bool PluginResourcePacks::on_cap_deregister(const PluginCapabilityId &id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.erase(id) > 0;
}

void PluginResourcePacks::initial_sweep()
{
    std::map<PluginCapabilityId, ResourcePackEntry> entries;
    for (const PluginCapabilityType type : {PluginCapabilityType::ResourcePrinterAsset, PluginCapabilityType::ResourceTexture}) {
        for (const auto &capability : PluginManager::instance().get_plugin_capabilities("", type)) {
            if (!capability)
                continue;
            std::optional<ResourcePackEntry> entry = build_entry(capability->identity());
            if (entry)
                entries[entry->id] = std::move(*entry);
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries = std::move(entries);
}

std::vector<ResourcePackEntry> PluginResourcePacks::entries() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<ResourcePackEntry> result;
    result.reserve(m_entries.size());
    for (const auto &entry : m_entries)
        result.push_back(entry.second);
    return result;
}

std::string PluginResourcePacks::resolve(const PrinterAssetRef &ref, std::string_view kind) const
{
    const std::string pack_id = boost::algorithm::to_lower_copy(ref.plugin_id);

    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &[id, entry] : m_entries) {
        if (id.type != PluginCapabilityType::ResourcePrinterAsset || (entry.pack_id != pack_id && entry.plugin_key != ref.plugin_id))
            continue;
        const auto kind_files = entry.files.find(std::string(kind));
        if (kind_files == entry.files.end())
            continue;
        // Matched against the validated listing by name, so a reference can never leave the pack.
        for (const std::string &file : kind_files->second)
            if (boost::filesystem::path(file).filename().string() == ref.filename)
                return file;
    }
    return {};
}

std::string PluginResourcePacks::capability_name(const PrinterAssetRef &ref) const
{
    const std::string pack_id = boost::algorithm::to_lower_copy(ref.plugin_id);

    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &[id, entry] : m_entries)
        if (id.type == PluginCapabilityType::ResourcePrinterAsset && (entry.pack_id == pack_id || entry.plugin_key == ref.plugin_id))
            return entry.display_name;
    return {};
}

std::string resolve_printer_asset_value(const std::string &value, std::string_view kind)
{
    const std::optional<PrinterAssetRef> ref = parse_printer_asset_ref(value);
    return ref ? PluginResourcePacks::instance().resolve(*ref, kind) : value;
}

// The name of the ResourcePrinterAsset capability an installed plugin provides for `ref`, enabled or
// not, or empty when the plugin is not installed here or provides none (or several).
static std::string installed_capability_name(const PrinterAssetRef &ref)
{
    PluginManager   &manager = PluginManager::instance();
    PluginDescriptor descriptor;
    if (!manager.try_get_plugin_descriptor(ref.plugin_id, descriptor))
        return {};
    const auto capabilities = manager.get_plugin_capabilities(descriptor.plugin_key, PluginCapabilityType::ResourcePrinterAsset,
                                                              /*only_enabled=*/false);
    return capabilities.size() == 1 ? capabilities.front()->name() : std::string();
}

// The capability name a manifest entry holds for the pack `ref` names, matched by uuid for a cloud
// pack and by plugin key (the first field) for a local one. Entries carry no type, so a name the
// config already lists as an asset pack wins over another capability of the same plugin.
static std::string manifest_capability_name(const std::vector<const ConfigOptionStrings *> &manifests, const PrinterAssetRef &ref,
                                            const std::vector<std::string> &known_names)
{
    std::string first_match;
    for (const ConfigOptionStrings *manifest : manifests) {
        if (manifest == nullptr)
            continue;
        for (const std::string &entry : manifest->values) {
            const std::optional<PluginCapabilityRef> parsed = parse_capability_ref(entry);
            if (!parsed || !boost::algorithm::iequals(parsed->uuid.empty() ? parsed->name : parsed->uuid, ref.plugin_id))
                continue;
            if (std::find(known_names.begin(), known_names.end(), parsed->capability_name) != known_names.end())
                return parsed->capability_name;
            if (first_match.empty())
                first_match = parsed->capability_name;
        }
    }
    return first_match;
}

void update_printer_asset_plugins(DynamicPrintConfig &config, const ConfigOptionStrings *fallback_manifest)
{
    auto *names_option = config.option<ConfigOptionStrings>("printer_asset_plugins");
    if (names_option == nullptr)
        return;

    const std::vector<const ConfigOptionStrings *> manifests{config.option<ConfigOptionStrings>("printer_plugins"), fallback_manifest};
    std::vector<std::string>                       names;
    std::vector<std::string>                       pack_ids;
    for (const char *key : {"bed_custom_texture", "bed_custom_model"}) {
        const auto                          *value = config.option<ConfigOptionString>(key);
        const std::optional<PrinterAssetRef> ref   = value != nullptr ? parse_printer_asset_ref(value->value) : std::nullopt;
        if (!ref)
            continue;
        pack_ids.push_back(ref->plugin_id);
        std::string name = PluginResourcePacks::instance().capability_name(*ref);
        if (name.empty())
            name = installed_capability_name(*ref);
        if (name.empty())
            name = manifest_capability_name(manifests, *ref, names_option->values);
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(std::move(name));
    }

    // Take over the fallback's entries for these bed files' packs only, so the rebuild below can reuse
    // them for packs that are not installed here (and nothing else leaks in without a resolver).
    auto *manifest = config.option<ConfigOptionStrings>("printer_plugins", true);
    if (fallback_manifest != nullptr)
        for (const std::string &entry : fallback_manifest->values) {
            const std::optional<PluginCapabilityRef> parsed = parse_capability_ref(entry);
            const bool for_these_packs = parsed && std::any_of(pack_ids.begin(), pack_ids.end(), [&parsed](const std::string &id) {
                return boost::algorithm::iequals(parsed->uuid.empty() ? parsed->name : parsed->uuid, id);
            });
            if (for_these_packs && std::find(manifest->values.begin(), manifest->values.end(), entry) == manifest->values.end())
                manifest->values.push_back(entry);
        }

    names_option->values = std::move(names);
    config.update_plugin_manifest();
}

} // namespace Slic3r
