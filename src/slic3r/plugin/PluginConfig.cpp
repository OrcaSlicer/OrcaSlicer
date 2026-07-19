#include "PluginConfig.hpp"

#include <algorithm>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <slic3r/GUI/GUI.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/I18N.hpp>
#include <slic3r/GUI/format.hpp>
#include <slic3r/plugin/PluginLoader.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <stdexcept>

#include <wx/app.h>
#include <wx/utils.h>

namespace Slic3r {

namespace {

constexpr const char* KEY_PLUGIN     = "plugin_key";
constexpr const char* KEY_CAPABILITY = "capability";
constexpr const char* KEY_TYPE       = "capability_type";
constexpr const char* KEY_VERSION    = "plugin_version";
constexpr const char* KEY_CAP_CONFIG = "cap_config";

std::string string_field(const nlohmann::json& entry, const char* key)
{
    const auto it = entry.find(key);
    return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool is_recognized_entry(const nlohmann::json& entry, PluginCapabilityId& id)
{
    if (!entry.is_object())
        return false;

    id.plugin_key = string_field(entry, KEY_PLUGIN);
    id.name       = string_field(entry, KEY_CAPABILITY);
    id.type       = plugin_capability_type_from_string(string_field(entry, KEY_TYPE));
    return !id.empty();
}

CapabilityConfigEntry decode_entry(const PluginCapabilityId& id, const nlohmann::json& entry)
{
    CapabilityConfigEntry result;
    result.id             = id;
    result.plugin_version = string_field(entry, KEY_VERSION);
    const auto cap_it     = entry.find(KEY_CAP_CONFIG);
    result.config         = cap_it != entry.end() ? *cap_it : nlohmann::json::object();
    return result;
}

// PluginDescriptor::version is overwritten with the latest cloud version on a cloud merge, so it can
// name a version that is not the one on disk; installed_version is what actually loaded.
std::string running_plugin_version(const std::string& plugin_key)
{
    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_valid_plugin_descriptor(plugin_key, descriptor))
        return {};
    return descriptor.installed_version.empty() ? descriptor.version : descriptor.installed_version;
}

// Null wherever the plugin host runs without the GUI app (the unit tests). wxGetApp() dereferences
// the app unconditionally, so ask wxWidgets instead.
const PresetBundle* active_preset_bundle()
{
    const auto* app = dynamic_cast<const GUI::GUI_App*>(wxApp::GetInstance());
    return app == nullptr ? nullptr : app->preset_bundle;
}

// PluginLoader stamps both halves onto the instance when it materializes the capability, so the
// caller never supplies them and cannot name another capability's entry. Empty means the instance
// was never materialized: refuse rather than read or clobber a wrong entry.
PluginCapabilityId capability_identity(const PluginCapabilityInterface& capability, const char* api_name)
{
    const PluginCapabilityId id = capability.identity();
    if (id.empty())
        throw std::runtime_error(std::string(api_name) + "() is only available on a capability loaded by the plugin host");

    return id;
}

} // namespace

CapabilityConfigDocument CapabilityConfigDocument::from_entries(const nlohmann::json& entries)
{
    CapabilityConfigDocument document;
    if (!entries.is_array())
        return document;

    for (const nlohmann::json& entry : entries) {
        PluginCapabilityId id;
        if (is_recognized_entry(entry, id) || (!id.plugin_key.empty() && !id.name.empty()))
            document.m_entries[id] = entry;
        else
            document.m_opaque_entries.push_back(entry);
    }

    return document;
}

CapabilityConfigDocument CapabilityConfigDocument::from_root_json(const nlohmann::json& root)
{
    const auto entries = root.find(KeyEntries);
    return entries != root.end() ? from_entries(*entries) : CapabilityConfigDocument();
}

std::optional<CapabilityConfigEntry> CapabilityConfigDocument::find(const PluginCapabilityId& id) const
{
    const auto it = m_entries.find(id);
    if (it != m_entries.end())
        return decode_entry(it->first, it->second);

    // Legacy config.json entries have no capability type. Keep them addressable by the new
    // typed API until that capability is saved again.
    if (id.type != PluginCapabilityType::Unknown) {
        const auto legacy = m_entries.find({PluginCapabilityType::Unknown, id.name, id.plugin_key});
        if (legacy != m_entries.end())
            return decode_entry(id, legacy->second);
    }
    return std::nullopt;
}

bool CapabilityConfigDocument::contains(const PluginCapabilityId& id) const
{
    return find(id).has_value();
}

bool CapabilityConfigDocument::upsert(CapabilityConfigEntry entry)
{
    if (entry.id.empty())
        return false;

    if (entry.id.type != PluginCapabilityType::Unknown)
        m_entries.erase({PluginCapabilityType::Unknown, entry.id.name, entry.id.plugin_key});

    nlohmann::json serialized = nlohmann::json::object();
    const auto existing       = m_entries.find(entry.id);
    if (existing != m_entries.end() && existing->second.is_object())
        serialized = existing->second;

    serialized[KEY_PLUGIN]     = entry.id.plugin_key;
    serialized[KEY_CAPABILITY] = entry.id.name;
    serialized[KEY_TYPE]       = plugin_capability_type_to_string(entry.id.type);
    serialized[KEY_VERSION]    = entry.plugin_version;
    serialized[KEY_CAP_CONFIG] = entry.config;

    m_entries[entry.id] = std::move(serialized);
    return true;
}

bool CapabilityConfigDocument::erase(const PluginCapabilityId& id)
{
    bool erased = m_entries.erase(id) != 0;
    if (id.type != PluginCapabilityType::Unknown)
        erased = m_entries.erase({PluginCapabilityType::Unknown, id.name, id.plugin_key}) != 0 || erased;
    return erased;
}

bool CapabilityConfigDocument::empty() const
{
    return m_entries.empty() && m_opaque_entries.empty();
}

nlohmann::json CapabilityConfigDocument::serialize_entries() const
{
    nlohmann::json result = nlohmann::json::array();
    for (const auto& item : m_entries)
        result.push_back(item.second);
    for (const nlohmann::json& entry : m_opaque_entries)
        result.push_back(entry);
    return result;
}

nlohmann::json CapabilityConfigDocument::root_json() const
{
    nlohmann::json root = nlohmann::json::object();
    root[KeyEntries] = serialize_entries();
    return root;
}

void PluginConfig::load()
{
    const std::string path = plugin_config_file();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_document = CapabilityConfigDocument();
    m_dirty = false;

    boost::system::error_code ec;
    if (!boost::filesystem::exists(path, ec))
        return;

    nlohmann::json root;
    try {
        boost::nowide::ifstream ifs(path.c_str());
        ifs >> root;
    } catch (const std::exception& err) {
        BOOST_LOG_TRIVIAL(error) << "PluginConfig: cannot read " << path << ": " << err.what() << "; starting with an empty config";
        return;
    }

    const auto entries = root.find(CapabilityConfigDocument::KeyEntries);
    if (entries == root.end() || !entries->is_array()) {
        BOOST_LOG_TRIVIAL(warning) << "PluginConfig: " << path << " has no \"" << CapabilityConfigDocument::KeyEntries
                                   << "\" array; starting with an empty config";
        return;
    }

    m_document = CapabilityConfigDocument::from_root_json(root);
}

bool PluginConfig::save()
{
    const std::string path = plugin_config_file();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_dirty)
        return true;

    const nlohmann::json root = m_document.root_json();

    boost::system::error_code ec;
    boost::filesystem::create_directories(boost::filesystem::path(path).parent_path(), ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "PluginConfig: cannot create the plugin directory: " << ec.message();
        return false;
    }

    // Write to a PID-suffixed file and rename it into place, so a crash mid-write cannot truncate an
    // existing config. Same approach as AppConfig::save().
    const std::string path_pid = (boost::format("%1%.%2%") % path % get_current_pid()).str();

    boost::nowide::ofstream file;
    file.open(path_pid, std::ios::out | std::ios::trunc);
    file << root.dump(1, '\t') << std::endl;
    file.close();
    if (file.fail()) {
        BOOST_LOG_TRIVIAL(error) << "PluginConfig: failed to write " << path_pid << "; keeping the existing config";
        return false;
    }

    if (const std::error_code rename_ec = rename_file(path_pid, path)) {
        BOOST_LOG_TRIVIAL(error) << "PluginConfig: failed to move " << path_pid << " onto " << path << ": " << rename_ec.message();
        return false;
    }

    m_dirty = false;
    return true;
}

bool PluginConfig::store_capability_config(const PluginCapabilityId& id, const nlohmann::json& config)
{
    if (id.empty())
        return false;

    save_config({id, running_plugin_version(id.plugin_key), config});
    return save();
}

void PluginConfig::save_config(const CapabilityConfigEntry& config)
{
    if (config.id.empty()) {
        BOOST_LOG_TRIVIAL(error) << "PluginConfig: refusing to store a config without a complete capability identity";
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_dirty = m_document.upsert(config) || m_dirty;
}

bool PluginConfig::erase_capability_config(const PluginCapabilityId& id)
{
    if (id.empty())
        return false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_document.erase(id))
            return true;
        m_dirty = true;
    }

    return save();
}

std::optional<CapabilityConfigEntry> PluginConfig::get_config(const PluginCapabilityId& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_document.find(id);
}

bool PluginConfig::has_config(const PluginCapabilityId& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_document.contains(id);
}

bool PluginConfig::dirty() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dirty;
}

std::string plugin_overrides_of(const Preset& preset)
{
    const auto* opt = dynamic_cast<const ConfigOptionString*>(preset.config.option(PLUGIN_OVERRIDES_OPTION_KEY));
    return opt == nullptr ? std::string() : opt->value;
}

bool parse_plugin_overrides(const std::string& raw, CapabilityConfigDocument& document, std::string& error)
{
    document = CapabilityConfigDocument();
    error.clear();

    if (raw.empty())
        return true;

    const nlohmann::json parsed = nlohmann::json::parse(raw, nullptr, /* allow_exceptions */ false);
    if (parsed.is_discarded()) {
        error = "The preset stores invalid plugin capability configuration JSON.";
        return false;
    }
    if (!parsed.is_array()) {
        error = "The preset's plugin capability configuration is not an array and cannot be edited.";
        return false;
    }

    document = CapabilityConfigDocument::from_entries(parsed);
    return true;
}

std::string serialize_plugin_overrides(const CapabilityConfigDocument& document)
{
    return document.empty() ? std::string() : document.serialize_entries().dump();
}

EffectiveCapabilityConfig PresetPluginConfigService::get_effective_config(const CapabilityConfigDocument&   overrides,
                                                                         const PluginCapabilityId& id) const
{
    EffectiveCapabilityConfig result;
    result.id                     = id;
    result.running_plugin_version = running_plugin_version(id.plugin_key);

    const auto base         = PluginManager::instance().get_config().get_config(id);
    result.has_base_config  = base.has_value();

    if (const auto entry = overrides.find(result.id)) {
        result.has_preset_override   = true;
        result.config                = entry->config;
        result.stored_plugin_version = entry->plugin_version;
        return result;
    }

    if (result.has_base_config) {
        result.config                = base->config;
        result.stored_plugin_version = base->plugin_version;
    }
    return result;
}

MutationResult PresetPluginConfigService::set_preset_override(CapabilityConfigDocument&         overrides,
                                                              const PluginCapabilityId& id,
                                                              const nlohmann::json&             value) const
{
    MutationResult    result;
    const std::string version = running_plugin_version(id.plugin_key);

    // A no-op is a successful unchanged result: re-saving the displayed value must not mark the
    // preset dirty.
    const auto existing = overrides.find(id);
    if (existing && existing->config == value && existing->plugin_version == version) {
        result.ok        = true;
        result.effective = get_effective_config(overrides, id);
        return result;
    }

    overrides.upsert({id, version, value});

    result.ok        = true;
    result.changed   = true;
    result.effective = get_effective_config(overrides, id);
    return result;
}

MutationResult PresetPluginConfigService::remove_preset_override(CapabilityConfigDocument&         overrides,
                                                                 const PluginCapabilityId& id) const
{
    MutationResult result;
    result.ok        = true;
    result.changed   = overrides.erase(id);
    result.effective = get_effective_config(overrides, id);
    return result;
}

EffectiveCapabilityConfig active_capability_config(const PluginCapabilityId& id)
{
    const PresetPluginConfigService service;

    CapabilityConfigDocument overrides;

    const PresetBundle* bundle = active_preset_bundle();
    const Preset* preset       = nullptr;

    if (bundle != nullptr) {
        const std::string type_key = plugin_capability_type_to_string(id.type);
        for (const auto& [key, def] : print_config_def.options) {
            if (def.plugin_type != type_key)
                continue;

            const auto& print_options = Preset::print_options();
            if (std::find(print_options.begin(), print_options.end(), key) != print_options.end()) {
                preset = &bundle->prints.get_edited_preset();
                break;
            }

            const auto& printer_options = Preset::printer_options();
            if (std::find(printer_options.begin(), printer_options.end(), key) != printer_options.end()) {
                preset = &bundle->printers.get_edited_preset();
                break;
            }
        }
    }

    if (preset != nullptr) {
        std::string error;
        if (!parse_plugin_overrides(plugin_overrides_of(*preset), overrides, error)) {
            // Text we cannot read is not an override: log it and resolve against the base config.
            BOOST_LOG_TRIVIAL(error) << "Preset \"" << preset->name << "\": " << error;
            overrides = CapabilityConfigDocument();
        }
    }

    return service.get_effective_config(overrides, id);
}

nlohmann::json capability_get_config(const PluginCapabilityInterface& capability)
{
    // Shares its resolution with the dialogs, so a capability reads back exactly the config its UI
    // showed as effective. Stored in neither layer: an empty object, indexable unconditionally.
    return active_capability_config(capability_identity(capability, "get_config")).config;
}

std::string capability_get_config_version(const PluginCapabilityInterface& capability)
{
    // Must resolve through the same layer as get_config(), or a plugin would migrate one layer's
    // config by another's version stamp.
    return active_capability_config(capability_identity(capability, "get_config_version")).stored_plugin_version;
}

bool capability_save_config(const PluginCapabilityInterface& capability, const nlohmann::json& config)
{
    return PluginManager::instance().get_config().store_capability_config(capability_identity(capability, "save_config"), config);
}

nlohmann::json PluginConfig::capabilities_payload(const std::vector<PluginCapabilityId>& caps)
{
    nlohmann::json payload = nlohmann::json::array();
    for (const PluginCapabilityId& id : caps) {
        // A capability unloaded since the list was built has nothing to configure.
        const auto capability = PluginManager::instance().get_plugin_capability(id, false);
        if (!capability)
            continue;

        nlohmann::json entry;
        entry["plugin_key"]    = id.plugin_key;
        entry["name"]          = id.name;
        entry["type"]          = plugin_capability_type_display_name(id.type);
        entry["type_key"]      = plugin_capability_type_to_string(id.type);
        entry["has_config_ui"] = capability->config_ui_available();
        payload.push_back(std::move(entry));
    }
    return payload;
}

// Config is sent as a JSON value, not text: the default editor pretty-prints it into its textarea,
// and a custom UI receives it as-is through window.orca.
nlohmann::json PluginConfig::get_config_response(const PluginCapabilityId& id)
{
    nlohmann::json response;
    response["command"]         = "capability_config";
    response["plugin_key"]      = id.plugin_key;
    response["capability_name"] = id.name;
    response["capability_type"] = plugin_capability_type_to_string(id.type);
    response["config"]          = nlohmann::json::object();
    response["custom_html"]     = "";
    response["error"]           = "";

    // Scoped to the full identity, so a stale request from a page that has not caught up with a
    // refresh misses rather than reading a different plugin's config.
    const auto cap = PluginManager::instance().get_plugin_capability(id, false);
    if (!cap) {
        BOOST_LOG_TRIVIAL(warning) << "Ignoring config request for a capability that is no longer loaded. plugin_key="
                                   << id.plugin_key << " capability_name=" << id.name;
        response["error"] = GUI::into_u8(_L("This capability is no longer available."));
        return response;
    }

    if (const auto stored = PluginManager::instance().get_config().get_config(id))
        response["config"] = stored->config;

    if (cap->config_ui_available()) {
        // A raising or empty get_config_ui() costs the capability only its custom UI: report the
        // failure and let the page fall back to the default JSON editor over the same stored config.
        std::string html;
        std::string error;
        {
            wxBusyCursor busy;
            try {
                PythonGILState gil;
                html = cap->get_config_ui();
            } catch (const std::exception& ex) {
                error = ex.what();
            } catch (...) {
                error = "Unknown error";
            }
        }

        if (!error.empty()) {
            BOOST_LOG_TRIVIAL(error) << "Plugin capability get_config_ui() failed. plugin_key=" << id.plugin_key
                                     << " capability_name=" << id.name << " error=" << error;
            response["error"] = GUI::into_u8(GUI::format_wxstr(_L("The plugin's configuration UI failed to load (%1%). Showing the default editor."),
                                                     GUI::from_u8(error)));
        } else if (html.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "Plugin capability reports a config UI but returned no HTML. plugin_key=" << id.plugin_key
                                       << " capability_name=" << id.name;
            response["error"] = GUI::into_u8(_L("The plugin's configuration UI was empty. Showing the default editor."));
        } else {
            response["custom_html"] = html;
        }
    }

    return response;
}

nlohmann::json PluginConfig::save_config_response(const PluginCapabilityId& id, const nlohmann::json& config)
{
    nlohmann::json response;
    response["command"]         = "capability_config_saved";
    response["plugin_key"]      = id.plugin_key;
    response["capability_name"] = id.name;
    response["capability_type"] = plugin_capability_type_to_string(id.type);
    response["ok"]              = false;
    response["error"]           = "";

    const auto cap = PluginManager::instance().get_plugin_capability(id, false);
    if (!cap) {
        BOOST_LOG_TRIVIAL(warning) << "Refusing to save config for a capability that is no longer loaded. plugin_key="
                                   << id.plugin_key << " capability_name=" << id.name;
        response["error"] = GUI::into_u8(_L("This capability is no longer available. Your changes were not saved."));
        return response;
    }

    nlohmann::json parsed = config;
    if (config.is_string()) {
        // The page validates as the user types, but it is not the authority: re-parse so a malformed
        // document is rejected before it can reach config.json.
        parsed = nlohmann::json::parse(config.get<std::string>(), nullptr, /* allow_exceptions */ false);
        if (parsed.is_discarded()) {
            response["error"] = GUI::into_u8(_L("The configuration is not valid JSON. Your changes were not saved."));
            return response;
        }
    }

    if (!PluginManager::instance().get_config().store_capability_config(id, parsed)) {
        BOOST_LOG_TRIVIAL(error) << "Failed to write the plugin config file. plugin_key=" << id.plugin_key
                                 << " capability_name=" << id.name;
        response["error"] = GUI::into_u8(_L("The configuration could not be written to disk. Your changes were not saved."));
        return response;
    }

    BOOST_LOG_TRIVIAL(info) << "Saved plugin capability config. plugin_key=" << id.plugin_key << " capability_name=" << id.name;

    // Echo back what was persisted, not what the user typed, so the editor reloads from the store.
    response["ok"]     = true;
    if (const auto stored = PluginManager::instance().get_config().get_config(id))
        response["config"] = stored->config;
    return response;
}

// The host never invents the default: a capability that does not override get_default_config()
// restores an empty config, which is right for one that applies its own defaults on read.
nlohmann::json PluginConfig::restore_config_response(const PluginCapabilityId& id)
{
    nlohmann::json response;
    response["command"]         = "capability_config_saved";
    response["plugin_key"]      = id.plugin_key;
    response["capability_name"] = id.name;
    response["capability_type"] = plugin_capability_type_to_string(id.type);
    response["ok"]              = false;
    response["error"]           = "";

    const auto cap = PluginManager::instance().get_plugin_capability(id, false);
    if (!cap) {
        BOOST_LOG_TRIVIAL(warning) << "Refusing to restore config for a capability that is no longer loaded. plugin_key="
                                   << id.plugin_key << " capability_name=" << id.name;
        response["error"] = GUI::into_u8(_L("This capability is no longer available."));
        return response;
    }

    nlohmann::json defaults;
    std::string    error;
    {
        wxBusyCursor busy;
        try {
            PythonGILState gil;
            defaults = cap->get_default_config();
        } catch (const std::exception& ex) {
            error = ex.what();
        } catch (...) {
            error = "Unknown error";
        }
    }

    // A raising hook leaves the stored config as it was: better to restore nothing than to wipe the
    // user's settings on the strength of a broken plugin.
    if (!error.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Plugin capability get_default_config() failed. plugin_key=" << id.plugin_key
                                 << " capability_name=" << id.name << " error=" << error;
        response["error"] = GUI::into_u8(GUI::format_wxstr(_L("The plugin could not supply a default configuration (%1%). "
                                                    "Nothing was changed."),
                                                 GUI::from_u8(error)));
        return response;
    }

    if (!PluginManager::instance().get_config().store_capability_config(id, defaults)) {
        BOOST_LOG_TRIVIAL(error) << "Failed to write the plugin config file while restoring defaults. plugin_key="
                                 << id.plugin_key << " capability_name=" << id.name;
        response["error"] = GUI::into_u8(_L("The configuration could not be written to disk. Nothing was changed."));
        return response;
    }

    BOOST_LOG_TRIVIAL(info) << "Restored default plugin capability config. plugin_key=" << id.plugin_key
                            << " capability_name=" << id.name;

    response["ok"]     = true;
    if (const auto stored = PluginManager::instance().get_config().get_config(id))
        response["config"] = stored->config;
    return response;
}

} // namespace Slic3r
