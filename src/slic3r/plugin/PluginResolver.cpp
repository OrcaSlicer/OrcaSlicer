#include "PluginResolver.hpp"

#include "PluginManager.hpp"
#include "../Utils/Http.hpp"
#include "../Utils/OrcaCloudServiceAgent.hpp"
#include "../GUI/GUI.hpp"
#include "../GUI/GUI_App.hpp"
#include "../GUI/I18N.hpp"
#include "../GUI/Plater.hpp"
#include "../GUI/NotificationManager.hpp"

#include <boost/log/trivial.hpp>
#include <libslic3r/Config.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <slic3r/plugin/PluginLoader.hpp>
#include <vector>
#include <wx/utils.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace Slic3r {

namespace {

// The tracked option in `preset` whose value references `ref`'s capability, or "" when none does.
// Doubles as the "Jump to" target and as the signal that the plugin is still required: a missing
// plugin with no referencing option is dropped from the set.
std::string find_option_for_capability(Preset::Type type,
                                       const Preset& preset,
                                       const PluginCapabilityRef& ref,
                                       PluginCapabilityType capability_type = PluginCapabilityType::Unknown)
{
    if (type != Preset::TYPE_PRINT && type != Preset::TYPE_PRINTER && type != Preset::TYPE_FILAMENT)
        return {};

    // Options opt in via ConfigOptionDef::is_plugin_backed, so scan the definition rather than keep a
    // hardcoded per-type field list. A typed preset's config only holds keys for its own type.
    const ConfigDef* def = preset.config.def();
    if (def == nullptr)
        return {};

    const std::string expected_type = plugin_capability_type_to_string(capability_type);
    const auto matches_ref = [&ref](const std::string& value) {
        return value == ref.capability_name;
    };

    for (const std::string& field : preset.config.keys()) {
        const ConfigOptionDef* opt_def = def->get(field);
        if (opt_def == nullptr || !opt_def->is_plugin_backed() ||
            (capability_type != PluginCapabilityType::Unknown && opt_def->plugin_type != expected_type))
            continue;

        const ConfigOption* option = preset.config.option(field);
        if (option == nullptr)
            continue;

        if (const auto* string_option = dynamic_cast<const ConfigOptionString*>(option)) {
            if (string_option->value == ref.capability_name)
                return field;
            continue;
        }

        if (const auto* vector_option = dynamic_cast<const ConfigOptionVectorBase*>(option)) {
            const std::vector<std::string> values = vector_option->vserialize();
            if (std::any_of(values.begin(), values.end(), matches_ref))
                return field;
        }
    }

    // printer_agent stores AgentInfo::id, so a missing plugin cannot be reverse-mapped through
    // the runtime registry. If no regular printer plugin field matched, assume printer_agent.
    if (type == Preset::Type::TYPE_PRINTER && preset.config.has("printer_agent")) {
        const ConfigOptionDef* agent_def = def->get("printer_agent");
        if (agent_def != nullptr && agent_def->is_plugin_backed() &&
            (capability_type == PluginCapabilityType::Unknown || agent_def->plugin_type == expected_type))
        return "printer_agent";
    }

    return {};
}

} // namespace

// One set per tracked preset type, keyed by the full "name;uuid;capability" ref.
static std::map<Preset::Type, std::unordered_map<std::string, MissingPlugin>> s_missing;
static std::mutex s_missing_mutex;
// Installed-but-inactive capabilities (not loaded, or loaded-but-disabled); resolvable locally.
static std::map<Preset::Type, std::unordered_map<std::string, MissingPlugin>> s_inactive;
// Installed+loaded but capability absent; not resolvable by activation. Both share s_missing_mutex.
static std::map<Preset::Type, std::unordered_map<std::string, MissingPlugin>> s_broken;

static bool is_tracked_type(Preset::Type type)
{
    return type == Preset::TYPE_PRINT || type == Preset::TYPE_PRINTER || type == Preset::TYPE_FILAMENT;
}

std::vector<PluginCapabilityRef> referenced_capabilities(Preset::Type type, const Preset& preset)
{
    if (!is_tracked_type(type))
        return {};

    const auto* manifest = dynamic_cast<const ConfigOptionStrings*>(preset.config.option("plugins"));
    if (manifest == nullptr)
        return {};

    std::vector<PluginCapabilityRef> refs;
    for (const std::string& entry : manifest->values) {
        const auto ref = parse_capability_ref(entry);
        if (!ref)
            continue;
        if (find_option_for_capability(type, preset, *ref).empty())
            continue;
        refs.push_back(*ref);
    }
    return refs;
}

namespace {

// Resolve one preset's referenced capabilities to loaded capability identifiers, appending to `out`.
// A ref not in the catalog, or whose capability is not loaded, is dropped: no instance means nothing
// to configure.
void collect_capabilities_in_use(Preset::Type type, const Preset& preset, std::vector<PluginCapabilityId>& out)
{
    for (const PluginCapabilityRef& ref : referenced_capabilities(type, preset)) {
        // Cloud plugins resolve by UUID, local plugins by plugin_key.
        const std::string key = ref.uuid.empty() ? ref.name : ref.uuid;
        if (key.empty())
            continue;

        PluginDescriptor descriptor;
        if (!PluginManager::instance().try_get_plugin_descriptor(key, descriptor))
            continue;

        // The manifest ref does not carry a type, so require the live capability type to match the
        // type declared by the option that references it.
        for (const auto& capability : PluginManager::instance().get_plugin_capabilities(descriptor.plugin_key, PluginCapabilityType::Unknown, false))
            if (capability && capability->type() != PluginCapabilityType::Unknown &&
                capability->name() == ref.capability_name &&
                !find_option_for_capability(type, preset, ref, capability->type()).empty())
                out.push_back(capability->identity());
    }
}

} // namespace

std::vector<PluginCapabilityId> capabilities_in_use(const PresetBundle& preset_bundle, Preset::Type type)
{
    if (!is_tracked_type(type))
        return {};

    std::vector<PluginCapabilityId> result;
    if (type == Preset::TYPE_PRINT) {
        collect_capabilities_in_use(type, preset_bundle.prints.get_edited_preset(), result);
    } else if (type == Preset::TYPE_PRINTER) {
        collect_capabilities_in_use(type, preset_bundle.printers.get_edited_preset(), result);
    } else {
        // Each filament preset is tested against its own config; refresh_missing_plugins cannot do
        // that, as it unions the manifests and loses the preset.
        for (const std::string& filament_name : preset_bundle.filament_presets)
            if (const Preset* filament = preset_bundle.filaments.find_preset(filament_name))
                collect_capabilities_in_use(type, *filament, result);
    }

    std::sort(result.begin(), result.end(), [](const PluginCapabilityId& a, const PluginCapabilityId& b) {
        return std::tie(a.plugin_key, a.name, a.type) < std::tie(b.plugin_key, b.name, b.type);
    });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<PluginCapabilityId> capabilities_in_use(Preset::Type type, const Preset& preset)
{
    std::vector<PluginCapabilityId> result;
    if (!is_tracked_type(type))
        return result;

    collect_capabilities_in_use(type, preset, result);
    std::sort(result.begin(), result.end(), [](const PluginCapabilityId& a, const PluginCapabilityId& b) {
        return std::tie(a.plugin_key, a.name, a.type) < std::tie(b.plugin_key, b.name, b.type);
    });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

static std::string resolve_cloud_base_url()
{
    std::string cloud_base_url = "https://cloud.orcaslicer.com";
    if (auto agent = GUI::wxGetApp().getAgent()) {
        if (auto orca_agent = std::dynamic_pointer_cast<OrcaCloudServiceAgent>(agent->get_cloud_agent())) {
            if (!orca_agent->get_cloud_base_url().empty())
                cloud_base_url = orca_agent->get_cloud_base_url();
        }
    }
    return cloud_base_url;
}

std::string create_full_ref(const PluginCapabilityRef& ref) { return ref.name + ';' + ref.uuid + ';' + ref.capability_name; }

std::string resolve_recovery_url(const PluginCapabilityRef& ref)
{
    return resolve_cloud_base_url() + "/app/plugins/plugin-hub?search=" + Http::url_encode(ref.name);
}

// {present, enabled}; {false, false} when the plugin is not loaded or does not provide the capability.
static std::pair<bool, bool> loaded_capability_state(const std::string& plugin_key, const PluginCapabilityRef& ref)
{
    PluginManager& mgr = PluginManager::instance();

    // Only a loaded package exposes capabilities; a discovered one carries their names in its
    // descriptor but has materialized nothing.
    if (!mgr.is_plugin_loaded(plugin_key))
        return {false, false};

    const auto capability = mgr.get_plugin_capability({PluginCapabilityType::Unknown, ref.capability_name, plugin_key},
                                                      /*only_enabled=*/false);
    if (!capability)
        return {false, false};

    return {true, capability->is_enabled()};
}

void refresh_missing_plugins(const PresetBundle& preset_bundle)
{
    const auto manifest_of = [](const Preset& preset) { return dynamic_cast<const ConfigOptionStrings*>(preset.config.option("plugins")); };
    const Preset& print_preset = preset_bundle.prints.get_edited_preset();
    refresh_missing_plugins(Preset::TYPE_PRINT, manifest_of(print_preset), &print_preset);
    const Preset& printer_preset = preset_bundle.printers.get_edited_preset();
    refresh_missing_plugins(Preset::TYPE_PRINTER, manifest_of(printer_preset), &printer_preset);

    // Filament plugins (if any) are the union over all selected filament presets.
    ConfigOptionStrings filament_manifest;
    for (const std::string& filament_name : preset_bundle.filament_presets) {
        const Preset* filament = preset_bundle.filaments.find_preset(filament_name);
        if (!filament)
            continue;
        if (const auto* opt = dynamic_cast<const ConfigOptionStrings*>(filament->config.option("plugins")))
            filament_manifest.values.insert(filament_manifest.values.end(), opt->values.begin(), opt->values.end());
    }
    refresh_missing_plugins(Preset::TYPE_FILAMENT, &filament_manifest);
}

void refresh_missing_plugins(Preset::Type type, const ConfigOptionStrings* manifest, const Preset* preset)
{
    if (!is_tracked_type(type))
        return;

    std::lock_guard<std::mutex> lock(s_missing_mutex);
    auto& missing_set  = s_missing[type];
    auto& inactive_set = s_inactive[type];
    auto& broken_set   = s_broken[type];
    missing_set.clear();
    inactive_set.clear();
    broken_set.clear();
    if (manifest == nullptr)
        return;

    PluginManager& mgr = PluginManager::instance();
    for (const std::string& entry : manifest->values) {
        const auto ref = parse_capability_ref(entry);
        if (!ref)
            continue;

        // Cloud plugins resolve by UUID, local plugins by plugin_key (the first field).
        const std::string key = ref->uuid.empty() ? ref->name : ref->uuid;
        if (key.empty())
            continue;

        PluginDescriptor descriptor;
        const bool in_catalog = mgr.try_get_plugin_descriptor(key, descriptor);
        const bool installed  = in_catalog && descriptor.has_local_package();
        const bool loaded     = installed && mgr.is_plugin_loaded(descriptor.plugin_key);
        const auto cap_state  = installed ? loaded_capability_state(descriptor.plugin_key, *ref)
                                          : std::pair<bool, bool>{false, false};
        const bool cap_present = cap_state.first;
        const bool cap_enabled = cap_state.second;
        if (cap_enabled)
            continue; // active and enabled — nothing to resolve

        std::string opt = preset != nullptr ? find_option_for_capability(type, *preset, *ref) : std::string();
        if (opt.empty())
            continue;

        if (!installed) {
            // Not on disk — needs download/install.
            std::string recovery_url = ref->uuid.empty() ? resolve_recovery_url(*ref) : std::string();
            missing_set.emplace(entry, MissingPlugin{*ref, std::move(recovery_url), std::move(opt), type, PluginCapabilityType::Unknown});
        } else if (!loaded || cap_present) {
            // Installed but not loaded yet (optimistic), or loaded but capability disabled — activatable.
            inactive_set.emplace(entry, MissingPlugin{*ref, std::string(), std::move(opt), type, PluginCapabilityType::Unknown});
        } else {
            // Loaded but the capability is absent — activation cannot fix it; offer a browse/update link.
            broken_set.emplace(entry, MissingPlugin{*ref, resolve_recovery_url(*ref), std::move(opt), type, PluginCapabilityType::Unknown});
        }
    }
}

std::vector<MissingPlugin> get_missing_cloud_plugins()
{
    std::vector<MissingPlugin> out;
    std::lock_guard<std::mutex> lock(s_missing_mutex);
    for (const auto& [type, set] : s_missing)
        for (const auto& [ref_str, missing] : set)
            if (!missing.ref.uuid.empty())
                out.push_back(missing);
    return out;
}

std::vector<MissingPlugin> get_missing_local_plugins()
{
    std::vector<MissingPlugin> out;
    std::lock_guard<std::mutex> lock(s_missing_mutex);
    for (const auto& [type, set] : s_missing)
        for (const auto& [ref_str, missing] : set)
            if (missing.ref.uuid.empty())
                out.push_back(missing);
    return out;
}

bool has_missing_plugins()
{
    std::lock_guard<std::mutex> lock(s_missing_mutex);
    for (const auto& [type, set] : s_missing)
        if (!set.empty())
            return true;
    return false;
}

std::vector<MissingPlugin> get_inactive_plugins()
{
    std::vector<MissingPlugin> out;
    std::lock_guard<std::mutex> lock(s_missing_mutex);
    for (const auto& [type, set] : s_inactive)
        for (const auto& [ref_str, plugin] : set)
            out.push_back(plugin);
    return out;
}

bool has_inactive_plugins()
{
    std::lock_guard<std::mutex> lock(s_missing_mutex);
    for (const auto& [type, set] : s_inactive)
        if (!set.empty())
            return true;
    return false;
}

std::vector<MissingPlugin> get_broken_plugins()
{
    std::vector<MissingPlugin> out;
    std::lock_guard<std::mutex> lock(s_missing_mutex);
    for (const auto& [type, set] : s_broken)
        for (const auto& [ref_str, plugin] : set)
            out.push_back(plugin);
    return out;
}

bool has_broken_plugins()
{
    std::lock_guard<std::mutex> lock(s_missing_mutex);
    for (const auto& [type, set] : s_broken)
        if (!set.empty())
            return true;
    return false;
}

static void report_install_failure(const std::string& message)
{
    GUI::wxGetApp().CallAfter([message]() {
        if (GUI::Plater* plater = GUI::wxGetApp().plater())
            plater->get_notification_manager()->push_notification(GUI::NotificationType::OrcaCloudAPIError,
                                                                  GUI::NotificationManager::NotificationLevel::ErrorNotificationLevel,
                                                                  _u8L("Plugin installation failed") + ": " + message);
    });
}

void resolve_missing_plugins(const std::vector<std::string>& refs, PluginInstallProgress progress)
{
    std::vector<std::string> uuids;
    for (const std::string& r : refs) {
        const auto ref = parse_capability_ref(r);
        if (ref && !ref->uuid.empty() && std::find(uuids.begin(), uuids.end(), ref->uuid) == uuids.end())
            uuids.push_back(ref->uuid);
    }
    if (uuids.empty()) {
        if (progress.on_finished)
            progress.on_finished();
        return;
    }

    // subscribe_and_install_cloud_plugin blocks on network + load, so run off the UI thread. On
    // success the capability-load callback re-validates the plate and clears the notification.
    std::thread worker([uuids, progress = std::move(progress)]() {
        PluginManager& mgr      = PluginManager::instance();
        const std::size_t total = uuids.size();
        for (std::size_t i = 0; i < total; ++i) {
            if (progress.is_cancelled && progress.is_cancelled())
                break;

            const std::string& uuid = uuids[i];

            std::string display_name = uuid;
            PluginDescriptor known;
            if (mgr.try_get_plugin_descriptor(uuid, known) && !known.name.empty())
                display_name = known.name;
            if (progress.on_plugin_begin)
                progress.on_plugin_begin(display_name, i, total);

            std::string error;
            if (!mgr.subscribe_and_install_cloud_plugin(uuid, error)) {
                report_install_failure(error.empty() ? uuid : (uuid + ": " + error));
                continue;
            }
            PluginDescriptor descriptor;
            if (!mgr.try_get_plugin_descriptor(uuid, descriptor)) {
                report_install_failure(uuid + ": installed plugin was not found in the catalog.");
                continue;
            }
            mgr.load_plugin(descriptor.plugin_key, false);
            if (!mgr.wait_for_plugin_load(descriptor.plugin_key, std::chrono::minutes(5), error) ||
                !mgr.is_plugin_loaded(descriptor.plugin_key)) {
                report_install_failure(descriptor.name + ": " + (error.empty() ? "plugin failed to load." : error));
            }
        }
        if (progress.on_finished)
            progress.on_finished();
    });
    worker.detach();
}

void resolve_inactive_plugins(const std::vector<std::string>& refs)
{
    PluginManager& mgr = PluginManager::instance();

    // Group by owning plugin so each plugin is loaded once with the full set to enable.
    std::map<std::string, std::vector<std::string>> by_plugin;
    for (const std::string& r : refs) {
        const auto ref = parse_capability_ref(r);
        if (!ref)
            continue;
        const std::string key = ref->uuid.empty() ? ref->name : ref->uuid;
        PluginDescriptor descriptor;
        if (!mgr.try_get_plugin_descriptor(key, descriptor) || !descriptor.has_local_package())
            continue;
        by_plugin[descriptor.plugin_key].push_back(ref->capability_name);
    }
    if (by_plugin.empty())
        return;

    // The fresh-load path does NOT fire the capability-load callback the GUI uses to clear the
    // notification, so wait for each load off the UI thread and re-validate once. That clears the
    // inactive notification, or flips it to broken if the plugin does not provide the capability.
    std::vector<std::pair<std::string, std::vector<std::string>>> work(by_plugin.begin(), by_plugin.end());
    std::thread([work = std::move(work)]() {
        PluginManager& mgr = PluginManager::instance();
        for (auto& [plugin_key, capabilities] : work) {
            mgr.load_plugin(plugin_key, /*skip_deps=*/false, capabilities);
            std::string error;
            mgr.wait_for_plugin_load(plugin_key, std::chrono::minutes(5), error);
        }
        GUI::wxGetApp().CallAfter([]() {
            if (GUI::Plater* plater = GUI::wxGetApp().plater())
                plater->revalidate_current_plate_if_plugins_missing();
        });
    }).detach();
}

void open_missing_plugins_on_cloud(const std::vector<std::string>& local_refs)
{
    if (local_refs.size() == 1) {
        if (const auto ref = parse_capability_ref(local_refs.front())) {
            wxLaunchDefaultBrowser(GUI::from_u8(resolve_recovery_url(*ref)), wxBROWSER_NEW_WINDOW);
            return;
        }
    }
    wxLaunchDefaultBrowser(GUI::from_u8(resolve_cloud_base_url() + "/app/plugins/plugin-hub"), wxBROWSER_NEW_WINDOW);
}

} // namespace Slic3r
