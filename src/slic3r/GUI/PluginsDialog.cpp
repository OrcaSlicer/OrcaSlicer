#include "PluginsDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "OrcaCloudServiceAgent.hpp"
#include "slic3r/plugin/PluginConfig.hpp"
#include "slic3r/plugin/PluginFsUtils.hpp"
#include "slic3r/plugin/PluginManager.hpp"

#include <libslic3r/Utils.hpp>

#include <slic3r/GUI/NotificationManager.hpp>
#include <slic3r/GUI/Plater.hpp>
#include <slic3r/GUI/format.hpp>

#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/timer.h>
#include <wx/utils.h>

namespace Slic3r { namespace GUI {
namespace {

const wxString kDeletePluginTitle            = _L("Delete Plugin");
const wxString kUnsubscribeTitle             = _L("Unsubscribe");
const wxString kOverwritePluginTitle         = _L("Overwrite Plugin");
std::string s_selected_plugin_install_action = "explore";

struct PluginContextAction
{
    std::string id;
    std::string label;
    bool enabled = true;
    bool danger  = false;
};

struct PluginAvailableActions
{
    bool can_toggle                   = false;
    bool toggle_installs_cloud_plugin = false;
    std::vector<PluginContextAction> context_actions;
};

struct PluginCapabilityView
{
    PluginCapabilityId id;
    bool enabled    = false;
    bool can_toggle = false;
    bool can_run    = false;
    // Whether the capability supplies its own config UI; every capability is configurable, this only
    // picks the editor. False for descriptor-only rows: an unloaded plugin has no capabilities yet.
    bool has_config_ui = false;
};

struct PluginChangelogView
{
    std::string version;
    std::string changelog;
    long long created_time = 0;
};

// View-model for one plugin row in the dialog
struct PluginDialogItem
{
    std::string plugin_key;
    std::string plugin_id;
    std::string display_name;
    std::string description;
    std::string author;
    std::string version;
    std::string installed_version;
    std::string latest_version;
    std::string sort_version; // Version shown in the row (installed if installed, else latest); used by the Version sort.
    std::string type_label;
    std::string type_key;
    std::string sharing_token;
    std::string thumbnail_url;
    std::vector<std::string> type_labels;
    std::vector<PluginChangelogView> changelog;

    PluginSource source              = PluginSource::Local;
    PluginStatus status              = PluginStatus::Inactive;
    PluginUpdateStatus update_status = PluginUpdateStatus::Normal;
    std::string error_text;
    bool has_error = false;
    bool is_loaded = false;
    bool loading   = false;

    bool is_cloud_plugin       = false;
    bool has_local_package     = false;
    bool unauthorized          = false;
    bool has_script_capability = false;
    bool can_run_script        = false;

    // Runtime capabilities in registration order, or descriptor-only type rows when unloaded.
    std::vector<PluginCapabilityView> capabilities;

    PluginAvailableActions available_actions;
};

constexpr bool kFetchCloudMeta      = true;
constexpr bool kUseCurrentCloudMeta = false;

// The type of a package's first loaded capability. A package that is not loaded has no type.
PluginCapabilityType primary_capability_type_of(PluginManager& manager, const std::string& plugin_key)
{
    const auto capabilities = manager.get_plugin_capabilities(plugin_key, PluginCapabilityType::Unknown, /*only_enabled=*/false);
    return capabilities.empty() ? PluginCapabilityType::Unknown : capabilities.front()->type();
}

std::vector<PluginDescriptor> current_cloud_metadata_snapshot()
{
    std::vector<PluginDescriptor> cloud_entries;
    for (const PluginDescriptor& entry : PluginManager::instance().get_plugin_descriptors(/*include_invalid=*/true))
        if (entry.is_cloud_plugin())
            cloud_entries.push_back(entry);
    return cloud_entries;
}

PluginDescriptor as_cloud_only_descriptor(PluginDescriptor descriptor)
{
    descriptor.plugin_root.clear();
    descriptor.entry_path.clear();
    descriptor.installed_version.clear();
    if (descriptor.cloud.has_value()) {
        descriptor.cloud->installed        = false;
        descriptor.cloud->update_available = false;
    }
    return descriptor;
}

void refresh_plugin_metadata_blocking(bool fetch_cloud)
{
    PluginManager& manager = PluginManager::instance();

    std::vector<std::string> not_found, unauthorized;
    const std::vector<PluginDescriptor> current_cloud_metadata = fetch_cloud ? std::vector<PluginDescriptor>{} :
                                                                             current_cloud_metadata_snapshot();

    manager.rescan_plugins();

    if (!fetch_cloud) {
        manager.update_cloud_metadata(current_cloud_metadata);
        return;
    }

    manager.fetch_plugins_from_cloud(&not_found, &unauthorized);

    wxGetApp().CallAfter([not_found = std::move(not_found), unauthorized = std::move(unauthorized)]() {
        if (wxGetApp().is_closing())
            return;
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return;

        for (const auto& uuid : not_found)
            plater->get_notification_manager()->push_notification(NotificationType::CustomNotification,
                                                                  NotificationManager::NotificationLevel::RegularNotificationLevel,
                                                                  format(_L("Plugin %s is no longer available."), uuid));
        for (const auto& uuid : unauthorized)
            plater->get_notification_manager()->push_notification(NotificationType::CustomNotification,
                                                                  NotificationManager::NotificationLevel::RegularNotificationLevel,
                                                                  format(_L("Plugin %s access is unauthorized."), uuid));
    });
}

std::string to_string(PluginUpdateStatus status);
nlohmann::json build_context_actions_payload(const PluginAvailableActions& available_actions);

nlohmann::json build_plugin_payload_item(const PluginDialogItem& dialog_item)
{
    nlohmann::json payload_item;
    payload_item["plugin_key"]  = dialog_item.plugin_key;
    payload_item["plugin_id"]   = dialog_item.plugin_id;
    payload_item["name"]        = dialog_item.display_name;
    payload_item["description"] = dialog_item.description;
    payload_item["author"]      = dialog_item.author;
    payload_item["version"]     = dialog_item.version;
    payload_item["type"]        = dialog_item.type_label;
    payload_item["type_key"]    = dialog_item.type_key;
    payload_item["types"]       = dialog_item.type_labels;

    nlohmann::json caps = nlohmann::json::array();
    for (const PluginCapabilityView& capability : dialog_item.capabilities) {
        nlohmann::json c;
        c["name"]          = capability.id.name;
        c["type"]          = plugin_capability_type_display_name(capability.id.type);
        c["type_key"]      = plugin_capability_type_to_string(capability.id.type);
        c["enabled"]       = capability.enabled;
        c["can_toggle"]    = capability.can_toggle;
        c["can_run"]       = capability.can_run;
        c["has_config_ui"] = capability.has_config_ui;
        caps.push_back(std::move(c));
    }
    payload_item["capabilities"] = std::move(caps);

    // The Config tab's sidebar, built by the shared builder so it stays identical to PluginsConfigDialog's.
    // Not the `capabilities` array above: that is the list tab's, with enable/run state and descriptor-only
    // rows the config view has no use for.
    std::vector<PluginCapabilityId> config_ids;
    for (const PluginCapabilityView& capability : dialog_item.capabilities)
        if (!capability.id.name.empty())
            config_ids.push_back(capability.id);
    payload_item["config_capabilities"] = PluginConfig::capabilities_payload(config_ids);

    nlohmann::json changelog = nlohmann::json::array();
    for (const PluginChangelogView& entry : dialog_item.changelog) {
        nlohmann::json c;
        c["version"]      = entry.version;
        c["changelog"]    = entry.changelog;
        c["created_time"] = entry.created_time;
        changelog.push_back(std::move(c));
    }
    payload_item["changelog"] = std::move(changelog);

    payload_item["label"]                 = dialog_item.display_name;
    payload_item["source"]                = to_string(dialog_item.source);
    payload_item["status"]                = to_string(dialog_item.status);
    payload_item["error"]                 = dialog_item.error_text;
    payload_item["update_status"]         = to_string(dialog_item.update_status);
    payload_item["unauthorized"]          = dialog_item.unauthorized;
    payload_item["context_actions"]       = build_context_actions_payload(dialog_item.available_actions);
    payload_item["update_available"]      = dialog_item.update_status == PluginUpdateStatus::UpdateAvailable;
    payload_item["can_toggle"]            = dialog_item.available_actions.can_toggle;
    payload_item["has_script_capability"] = dialog_item.has_script_capability;
    payload_item["can_run_script"]        = dialog_item.can_run_script;
    payload_item["sharing_token"]         = dialog_item.sharing_token;
    payload_item["thumbnail_url"]         = dialog_item.thumbnail_url;
    payload_item["installed"]             = dialog_item.has_local_package;
    payload_item["installed_version"]     = dialog_item.installed_version;
    payload_item["latest_version"]        = dialog_item.latest_version;
    return payload_item;
}

std::string to_string(PluginUpdateStatus status)
{
    switch (status) {
    case PluginUpdateStatus::Normal: return "normal";
    case PluginUpdateStatus::UpdateAvailable: return "update_available";
    case PluginUpdateStatus::Unauthorized: return "unauthorized";
    }

    return "normal";
}

PluginSource derive_plugin_source(const PluginDescriptor& descriptor)
{
    const bool has_cloud_meta = descriptor.cloud.has_value();
    const bool is_cloud       = descriptor.is_cloud_plugin();
    const bool is_mine        = is_cloud && has_cloud_meta && descriptor.cloud->is_mine;

    // Source is ownership/locality only; issue states never replace this badge.
    if (is_mine)
        return PluginSource::Mine;
    if (is_cloud)
        return PluginSource::Subscribed;
    return PluginSource::Local;
}

PluginAvailableActions evaluate_action_policy(const PluginDialogItem& item)
{
    PluginAvailableActions available_actions;
    const bool is_loading             = item.status == PluginStatus::Loading;
    const bool is_cloud               = item.is_cloud_plugin;
    const bool is_mine                = item.source == PluginSource::Mine;
    const bool has_local              = item.has_local_package;
    const bool authorized_for_install = !item.unauthorized;

    available_actions.toggle_installs_cloud_plugin = is_cloud && !has_local && authorized_for_install;
    available_actions.can_toggle                   = !is_loading && (has_local || available_actions.toggle_installs_cloud_plugin);

    auto add_action = [&available_actions](const char* id, const char* label, bool enabled = true, bool danger = false) {
        available_actions.context_actions.push_back(PluginContextAction{id, label, enabled, danger});
    };

    if (is_cloud) {
        if (is_mine)
            add_action("delete_mine_plugin", "Delete", true, true);
        else
            add_action("unsubscribe_plugin", "Unsubscribe", true, true);
    } else if (has_local) {
        add_action("delete_plugin", "Delete", true, true);
    }

    add_action("open_folder", "Show in folder", has_local);

    if (is_cloud) {
        add_action("reinstall_plugin", "Reinstall");
    } else {
        add_action("reload_plugin", "Reload");
        add_action("clear_cache_reload_plugin", "Delete cache and reload");
    }

    return available_actions;
}

PluginDialogItem build_plugin_dialog_item(const PluginDescriptor& descriptor)
{
    PluginDialogItem item;
    PluginManager& manager = PluginManager::instance();

    item.plugin_key   = descriptor.plugin_key;
    item.display_name = descriptor.name;
    item.description  = !descriptor.is_metadata_valid() && descriptor.has_error() ?
                            descriptor.normalized_error() :
                            (descriptor.description.empty() ? "No description." : descriptor.description);
    item.author       = descriptor.author;
    item.version      = descriptor.version;
    // The cloud merge overwrites `version` with the latest cloud version, so prefer the preserved local
    // one, falling back to `version` for local-only / pre-merge descriptors.
    item.installed_version = descriptor.has_local_package() ?
                                 (descriptor.installed_version.empty() ? descriptor.version : descriptor.installed_version) :
                                 std::string{};
    item.latest_version    = descriptor.latest_available_version();
    item.sort_version      = item.installed_version.empty() ? item.latest_version : item.installed_version;
    // A package that is not loaded has no capabilities, so its row contributes none and does not
    // expand.
    const std::vector<std::shared_ptr<PluginCapabilityInterface>> capabilities =
        manager.get_plugin_capabilities(descriptor.plugin_key, PluginCapabilityType::Unknown, /*only_enabled=*/false);

    const PluginCapabilityType primary_type = capabilities.empty() ? PluginCapabilityType::Unknown : capabilities.front()->type();
    item.type_label                         = plugin_capability_type_to_string(primary_type);
    item.type_key                           = plugin_capability_type_to_string(primary_type);
    // "types" is the display-only compatibility list. Cloud plugins show the raw labels the
    // service returned (which may not map to real capability types); local plugins derive them
    // from the capabilities actually loaded.
    if (descriptor.is_cloud_plugin() && !descriptor.display_types.empty()) {
        for (const std::string& label : descriptor.display_types)
            if (std::find(item.type_labels.begin(), item.type_labels.end(), label) == item.type_labels.end())
                item.type_labels.push_back(label);
    } else {
        for (const auto& capability : capabilities) {
            const std::string label = plugin_capability_type_display_name(capability->type());
            if (std::find(item.type_labels.begin(), item.type_labels.end(), label) == item.type_labels.end())
                item.type_labels.push_back(label);
        }
    }
    for (const PluginChangelog& entry : descriptor.changelog)
        item.changelog.push_back({entry.version, entry.changelog, entry.created_time});
    item.source                = derive_plugin_source(descriptor);
    item.update_status         = descriptor.get_update_status();
    item.error_text            = descriptor.normalized_error();
    item.has_error             = descriptor.has_error();
    item.is_cloud_plugin       = descriptor.is_cloud_plugin();
    item.has_local_package     = descriptor.has_local_package();
    item.unauthorized          = descriptor.is_unauthorized();
    item.is_loaded             = manager.is_plugin_loaded(descriptor.plugin_key);
    item.loading               = manager.is_plugin_load_in_progress(descriptor.plugin_key);
    item.has_script_capability = false;
    for (const auto& capability : capabilities) {
        item.capabilities.push_back({capability->identity(), capability->is_enabled(), true, false, capability->config_ui_available()});
        if (capability->type() == PluginCapabilityType::Script)
            item.has_script_capability = true;
    }
    item.sharing_token = descriptor.sharing_token;
    item.thumbnail_url = descriptor.thumbnail_url;

    if (item.loading)
        item.status = PluginStatus::Loading;
    else if (item.has_error)
        item.status = PluginStatus::Error;
    else if (item.is_loaded)
        item.status = PluginStatus::Activated;
    else
        item.status = PluginStatus::Inactive;

    item.available_actions        = evaluate_action_policy(item);
    const bool has_enabled_script = std::any_of(item.capabilities.begin(), item.capabilities.end(),
                                                [](const PluginCapabilityView& capability) {
                                                    return capability.id.type == PluginCapabilityType::Script && capability.enabled;
                                                });
    item.can_run_script = descriptor.is_metadata_valid() && !descriptor.has_error() && item.has_script_capability && item.is_loaded &&
                          !item.loading && has_enabled_script;
    for (PluginCapabilityView& capability : item.capabilities) {
        capability.can_run = item.can_run_script && capability.id.type == PluginCapabilityType::Script && capability.enabled;
    }
    return item;
}

const PluginContextAction* find_context_action(const PluginAvailableActions& available_actions, const std::string& action_id)
{
    const auto it = std::find_if(available_actions.context_actions.begin(), available_actions.context_actions.end(),
                                 [&action_id](const PluginContextAction& action) { return action.id == action_id; });
    return it != available_actions.context_actions.end() ? &(*it) : nullptr;
}

nlohmann::json build_context_actions_payload(const PluginAvailableActions& available_actions)
{
    nlohmann::json payload = nlohmann::json::array();

    for (const PluginContextAction& action : available_actions.context_actions) {
        nlohmann::json item;
        item["id"]      = action.id;
        item["label"]   = action.label;
        item["enabled"] = action.enabled;
        item["danger"]  = action.danger;
        payload.push_back(std::move(item));
    }

    return payload;
}

struct PluginOperationState
{
    std::mutex mutex;
    bool succeeded = false;
    std::string error;
};

void store_plugin_operation_result(const std::shared_ptr<PluginOperationState>& state, bool succeeded, std::string error)
{
    std::lock_guard<std::mutex> lock(state->mutex);
    state->succeeded = succeeded;
    state->error     = std::move(error);
}

bool take_plugin_operation_result(const std::shared_ptr<PluginOperationState>& state, std::string& error)
{
    std::lock_guard<std::mutex> lock(state->mutex);
    error = std::move(state->error);
    return state->succeeded;
}
} // namespace

PluginsDialog::PluginsDialog(wxWindow* parent, wxWindowID id, const wxString&, const wxPoint& pos, const wxSize& size, long style)
    : WebViewHostDialog(parent, id, _L("Plugins"), pos, size, style)
{ create_webview("web/dialog/PluginsDialog/index.html", _L("Plugins"), wxSize(900, 820), wxSize(760, 715)); }

PluginsDialog::~PluginsDialog() { m_alive->store(false, std::memory_order_release); }

void PluginsDialog::set_open_terminal_dlg_fn()
{
    m_open_terminal_dlg_fn = [] { wxGetApp().open_terminal_dialog(); };
}

void PluginsDialog::update_plugin_dialog_ui()
{
    // Called after the any metadata is already updated, for example from the
    // cloud-plugin state callback. Do not fetch here or the callback can re-enter.
    send_plugins();
    resolve_pending_activation();
}

void PluginsDialog::resolve_pending_activation()
{
    if (m_activating_plugin_key.empty())
        return;

    PluginManager& manager = PluginManager::instance();
    if (manager.is_plugin_load_in_progress(m_activating_plugin_key))
        return; // Still loading: keep the "Activating..." message until the load resolves.

    const std::string plugin_key = m_activating_plugin_key;
    m_activating_plugin_key.clear();

    // Mirror the row's status precedence (Error before Activated) so the message matches the list.
    PluginDescriptor descriptor;
    if (get_descriptor(plugin_key, descriptor) && descriptor.has_error())
        show_status(wxString::Format(_L("Failed to activate \"%s\"."), plugin_display_name(plugin_key)), "error");
    else if (manager.is_plugin_loaded(plugin_key))
        show_status(wxString::Format(_L("Activated \"%s\"."), plugin_display_name(plugin_key)), "success");
    // Otherwise it ended up inactive (toggled off or cancelled mid-load): stay silent.
}

void PluginsDialog::on_script_message(const nlohmann::json& payload)
{
    if (handle_common_script_command(payload))
        return;

    // Defer command handling out of the webview script-message callback: GTK and macOS
    // deliver it synchronously inside the native webview callback (see ui_create_window
    // in PluginHostUi.cpp), and window work on that stack is the crash class fixed in
    // b779a7bfed/f2ccbfc8b5. Deferring here keeps every command handler in THIS dialog off
    // that stack; it is not a guarantee for other WebViewHostDialog subclasses, which each
    // have to defer for themselves (PluginsConfigDialog does; others still do not).
    wxGetApp().CallAfter([this, alive = m_alive, payload]() {
        if (alive->load(std::memory_order_acquire))
            handle_web_command(payload);
    });
}

void PluginsDialog::handle_web_command(const nlohmann::json& payload)
{
    const std::string command = payload.value("command", "");
    if (command == "request_plugins") {
        send_plugins();
    } else if (command == "refresh_plugins") {
        refresh_plugins();
    } else if (command == "toggle_plugin") {
        toggle_plugin(payload.value("plugin_key", ""), payload.value("enabled", false));
    } else if (command == "toggle_plugin_capability") {
        toggle_plugin_capability(payload.value("plugin_key", ""), plugin_capability_type_from_string(payload.value("capability_type", "")),
                                 payload.value("capability_name", ""), payload.value("enabled", false));
    } else if (command == "install_local_plugin") {
        install_plugin_from_file();
    } else if (command == "plugin_menu_action") {
        handle_plugin_menu_action(payload.value("plugin_key", ""), payload.value("action", ""));
    } else if (command == "run_script_plugin_capability") {
        run_script_plugin_capability(payload.value("plugin_key", ""), payload.value("capability_name", ""));
    } else if (command == "open_terminal") {
        m_open_terminal_dlg_fn();
    } else if (command == "update_plugin") {
        update_plugin(payload.value("plugin_key", ""));
    } else if (command == "open_plugin_on_cloud") {
        open_plugin_on_cloud(payload.value("sharing_token", ""));
    } else if (command == "open_plugin_hub") {
        open_plugin_hub();
    } else if (command == "set_plugin_sort") {
        set_plugin_sort(payload.value("sort_key", ""), payload.value("sort_order", ""));
    } else if (command == "get_capability_config" || command == "save_capability_config" || command == "restore_capability_config") {
        const PluginCapabilityId id{plugin_capability_type_from_string(payload.value("capability_type", "")),
                                    payload.value("capability_name", ""), payload.value("plugin_key", "")};
        if (command == "get_capability_config") {
            send_capability_config(id);
            return;
        }
        if (command == "restore_capability_config") {
            restore_capability_config(id);
            return;
        }

        // `config` is a JSON string from the default editor's textarea, or an already-structured
        // value from a capability's custom UI. Both land here; save_capability_config sorts it out.
        save_capability_config(id, payload.contains("config") ? payload.at("config") : nlohmann::json::object());
    } else if (command == "set_plugin_install_action") {
        const std::string action = payload.value("action", "");
        if (action == "explore" || action == "install-local")
            s_selected_plugin_install_action = action;
    }
}

void PluginsDialog::send_plugins() { call_web_handler(build_plugins_payload()); }

void PluginsDialog::set_plugin_sort(const std::string& sort_key, const std::string& sort_order)
{
    m_plugin_sort_key   = plugin_sort_key_from_string(sort_key, m_plugin_sort_key);
    m_plugin_sort_order = plugin_sort_order_from_string(sort_order, m_plugin_sort_order);
    send_plugins();
}

nlohmann::json PluginsDialog::build_plugins_payload() const
{
    nlohmann::json response;
    response["command"]        = "list_plugins";
    response["install_action"] = s_selected_plugin_install_action;
    response["sort_key"]       = to_string(m_plugin_sort_key);
    response["sort_order"]     = to_string(m_plugin_sort_order);
    response["data"]           = nlohmann::json::array();

    const auto rows = PluginManager::instance().get_plugin_descriptors(/*include_invalid=*/true);
    BOOST_LOG_TRIVIAL(info) << "Prepared " << rows.size() << " plugin rows for Plugins dialog";

    std::vector<PluginDialogItem> items;
    items.reserve(rows.size());

    for (const PluginDescriptor& row : rows)
        items.push_back(build_plugin_dialog_item(row));

    sort_plugin_items_for_dialog(items, m_plugin_sort_key, m_plugin_sort_order);

    for (const PluginDialogItem& item : items)
        response["data"].push_back(build_plugin_payload_item(item));

    return response;
}

bool PluginsDialog::get_descriptor(const std::string& plugin_key, PluginDescriptor& descriptor) const
{
    PluginManager& manager = PluginManager::instance();
    if (manager.try_get_valid_plugin_descriptor(plugin_key, descriptor))
        return true;
    return manager.try_get_plugin_descriptor(plugin_key, descriptor) && descriptor.is_invalid_package();
}

void PluginsDialog::refresh_plugin_metadata_async(const wxString& title, const wxString& message, bool fetch_cloud)
{
    run_with_dialog([fetch_cloud]() { refresh_plugin_metadata_blocking(fetch_cloud); }, [this]() { send_plugins(); }, title, message);
}

void PluginsDialog::refresh_plugins()
{
    BOOST_LOG_TRIVIAL(info) << "Refreshing plugins from Plugins dialog";

    refresh_plugin_metadata_async(_L("Refreshing"), _L("Refreshing plugins data"), kFetchCloudMeta);
}

void PluginsDialog::toggle_plugin(const std::string& plugin_key, bool enabled)
{
    if (plugin_key.empty())
        return;

    BOOST_LOG_TRIVIAL(info) << "Toggle plugin request. plugin_key=" << plugin_key << " enabled=" << enabled;

    PluginDescriptor row_data;
    if (!get_descriptor(plugin_key, row_data)) {
        send_plugins();
        return;
    }

    PluginDialogItem dialog_item             = build_plugin_dialog_item(row_data);
    PluginAvailableActions available_actions = dialog_item.available_actions;

    PluginManager& manager = PluginManager::instance();
    if (!enabled) {
        if (!manager.unload_plugin(plugin_key)) {
            BOOST_LOG_TRIVIAL(error) << "Failed to unload plugin from Plugins dialog: " << plugin_key;
            show_status(_L("Failed to unload plugin."), "warn");
            send_plugins();
            return;
        }

        BOOST_LOG_TRIVIAL(info) << "Plugin unloaded from Plugins dialog: " << plugin_key;
        // A prior activation of this plugin is moot now; drop it so no stale "Activated" arrives later.
        if (m_activating_plugin_key == plugin_key)
            m_activating_plugin_key.clear();
        send_plugins();
        show_status(wxString::Format(_L("Deactivated \"%s\"."), plugin_display_name(plugin_key)), "success");
        return;
    }

    if (!available_actions.can_toggle) {
        if (dialog_item.unauthorized && available_actions.toggle_installs_cloud_plugin == false && row_data.has_local_package() == false) {
            const std::string install_error = "Unauthorized cloud plugins cannot be installed.";
            manager.set_plugin_error(plugin_key, install_error);
            show_status(from_u8(install_error), "warn");
        }
        send_plugins();
        return;
    }

    if (available_actions.toggle_installs_cloud_plugin) {
        if (!install_cloud_plugin(row_data.plugin_key, row_data.version, from_u8(row_data.name))) {
            BOOST_LOG_TRIVIAL(warning) << "Cloud plugin install was not completed for " << plugin_key;
            send_plugins();
            return;
        }

        BOOST_LOG_TRIVIAL(info) << "Cloud plugin installed locally from Plugins dialog: " << plugin_key;
        // download_and_install_cloud_plugin already updated the descriptor, so no rescan is needed here.
        if (!get_descriptor(plugin_key, row_data)) {
            send_plugins();
            return;
        }
        dialog_item = build_plugin_dialog_item(row_data);
        if (dialog_item.has_error) {
            send_plugins();
            return;
        }
    }

    if (manager.is_plugin_loaded(plugin_key)) {
        send_plugins();
        return;
    }

    PluginDescriptor valid_descriptor;
    if (!manager.try_get_valid_plugin_descriptor(plugin_key, valid_descriptor)) {
        show_status(_L("Plugin manifest was not found."), "warn");
        send_plugins();
        return;
    }

    if (manager.is_plugin_load_in_progress(plugin_key)) {
        send_plugins();
        return;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Starting plugin load from web dialog: " << plugin_key;
    manager.load_plugin(plugin_key);
    send_plugins();
    // Loading is asynchronous: show a pending message now; resolve_pending_activation() turns it into
    // Activated/Failed once the loader's completion callback runs update_plugin_dialog_ui().
    m_activating_plugin_key = plugin_key;
    show_status(wxString::Format(_L("Activating \"%s\"..."), plugin_display_name(plugin_key)), "info");
}

void PluginsDialog::toggle_plugin_capability(const std::string& plugin_key,
                                             PluginCapabilityType type,
                                             const std::string& capability_name,
                                             bool enabled)
{
    if (plugin_key.empty() || capability_name.empty() || type == PluginCapabilityType::Unknown)
        return;

    PluginDescriptor row_data;
    if (!get_descriptor(plugin_key, row_data)) {
        send_plugins();
        return;
    }

    if (!PluginManager::instance().get_plugin_capability({type, capability_name, plugin_key}, /*only_enabled=*/false)) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot toggle missing plugin capability: " << plugin_key << " | " << capability_name;
        send_plugins();
        return;
    }

    PluginManager& manager = PluginManager::instance();
    if (enabled) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Enabling plugin capability: " << plugin_key << " | " << capability_name;
        manager.set_capability_enabled({type, capability_name, plugin_key}, true);
    } else {
        // check if the capability is currently in use here.
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Disabling plugin capability: " << plugin_key << " | " << capability_name;
        manager.set_capability_enabled({type, capability_name, plugin_key}, false);
    }

    send_plugins();
    show_status(wxString::Format(enabled ? _L("Enabled \"%s\".") : _L("Disabled \"%s\"."), from_u8(capability_name)), "success");
}

void PluginsDialog::handle_plugin_menu_action(const std::string& plugin_key, const std::string& action)
{
    PluginDescriptor row_data;
    if (!get_descriptor(plugin_key, row_data))
        return;

    const PluginDialogItem dialog_item = build_plugin_dialog_item(row_data);

    const PluginContextAction* context_action = find_context_action(dialog_item.available_actions, action);
    if (!context_action || !context_action->enabled)
        return;

    if (action == "open_folder") {
        open_plugin_folder(row_data);
    } else if (action == "delete_plugin") {
        delete_local_plugin(row_data);
    } else if (action == "unsubscribe_plugin") {
        unsubscribe_cloud_plugin(row_data);
    } else if (action == "delete_mine_plugin") {
        delete_mine_local_and_cloud_plugin(plugin_key);
    } else if (action == "reload_plugin") {
        reload_local_plugin(plugin_key, /*clear_cache=*/false);
    } else if (action == "clear_cache_reload_plugin") {
        reload_local_plugin(plugin_key, /*clear_cache=*/true);
    } else if (action == "reinstall_plugin") {
        if (row_data.is_cloud_plugin())
            reinstall_cloud_plugin(row_data);
    }
}

void PluginsDialog::restore_z_order()
{
    // Deferred so it runs on a clean stack after the modal has fully torn down; the
    // alive guard covers the dialog being destroyed while the CallAfter is queued.
    wxGetApp().CallAfter([this, alive = m_alive]() {
        if (alive->load(std::memory_order_acquire) && IsShown())
            Raise();
    });
}

void PluginsDialog::install_plugin_from_file()
{
    wxFileDialog dialog(this, _L("Select plugin package"), wxEmptyString, wxEmptyString, _L("Plugin files (*.py;*.whl)|*.py;*.whl"),
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    const int rc = dialog.ShowModal();
    restore_z_order();
    if (rc != wxID_OK)
        return;

    if (!install_plugin_package(dialog.GetPath().ToUTF8().data())) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Failed to install plugin package.";
    }
}

bool PluginsDialog::install_plugin_package(const std::string& package_path)
{
    if (package_path.empty())
        return false;
    BOOST_LOG_TRIVIAL(info) << "Installing local plugin package from path: " << package_path;
    std::string error;
    const boost::filesystem::path package_file(package_path);
    const wxString package_name = from_u8(package_file.filename().string());

    std::string extension = package_file.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (extension != ".py" && extension != ".whl") {
        show_status(_L("Select a .py or .whl plugin package."), "info");
        return false;
    }

    PluginDescriptor plugin_descriptor;
    bool existing_installation     = false;
    auto report_inspection_failure = [&]() {
        BOOST_LOG_TRIVIAL(error) << "Plugin package inspection failed for " << package_path << " error=" << error;
        show_status(_L("Failed to install plugin package. See the log for details."), "warn");
        send_plugins();
        return false;
    };

    try {
        if (!PluginManager::instance().inspect_local_plugin_package(package_file, plugin_descriptor, existing_installation, error))
            return report_inspection_failure();
    } catch (const std::exception& ex) {
        error = ex.what();
        return report_inspection_failure();
    } catch (...) {
        error = "Unknown error";
        return report_inspection_failure();
    }

    if (existing_installation) {
        const wxString plugin_name = from_u8(plugin_descriptor.name.empty() ? package_file.filename().string() : plugin_descriptor.name);
        wxMessageDialog dialog(
            this,
            wxString::Format(_L("Plugin \"%s\" is already installed.\n\nInstalling this package will overwrite the existing plugin."),
                             plugin_name),
            kOverwritePluginTitle, wxOK | wxCANCEL | wxCANCEL_DEFAULT | wxICON_WARNING);
        dialog.SetOKCancelLabels(_L("Overwrite"), _L("Cancel"));
        const int overwrite_rc = dialog.ShowModal();
        restore_z_order();
        if (overwrite_rc != wxID_OK) {
            BOOST_LOG_TRIVIAL(info) << "Plugin package installation cancelled before overwrite. package=" << package_path
                                    << " plugin=" << plugin_descriptor.name;
            return false;
        }
    }

    bool installed = false;
    try {
        installed = run_with_dialog_wait([package_file, &error]() { return PluginManager::instance().install_plugin(package_file, error); },
                                         _L("Installing plugin"), _L("Installing plugin") + ": " + package_name, 100,
                                         wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);
    } catch (const std::exception& ex) {
        error = ex.what();
    } catch (...) {
        error = "Unknown error";
    }

    if (!installed) {
        BOOST_LOG_TRIVIAL(error) << "Plugin package installation failed for " << package_path << " error=" << error;
        show_status(_L("Failed to install plugin package. See the log for details."), "warn");
        send_plugins();
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "Plugin package installed successfully from " << package_path;
    const wxString installed_name = from_u8(plugin_descriptor.name.empty() ? package_file.filename().string() : plugin_descriptor.name);
    show_status(wxString::Format(_L("Installed \"%s\"."), installed_name), "success");
    refresh_plugin_metadata_async(_L("Refreshing"), _L("Refreshing plugins data"), kUseCurrentCloudMeta);
    return true;
}

bool PluginsDialog::install_cloud_plugin(const std::string& plugin_key, const std::string& version, const wxString& name)
{
    if (plugin_key.empty())
        return false;

    BOOST_LOG_TRIVIAL(info) << "Downloading cloud plugin. plugin_key=" << plugin_key << " name=" << into_u8(name);

    std::string error;

    bool downloaded = false;
    try {
        downloaded = run_with_dialog_wait(
            [plugin_key, &error, version]() {
                return PluginManager::instance().download_and_install_cloud_plugin(plugin_key, version, error);
            },
            _L("Downloading plugin"), _L("Downloading") + ": " + name);
    } catch (const std::exception& ex) {
        error = ex.what();
    } catch (...) {
        error = "Unknown error";
    }

    if (!downloaded) {
        BOOST_LOG_TRIVIAL(error) << "Cloud plugin download failed. plugin_key=" << plugin_key << " error=" << error;
        PluginManager::instance().set_plugin_error(plugin_key, error);
        show_status(error.empty() ? _L("Plugin download failed.") : from_u8(error), "error");
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "Cloud plugin downloaded successfully. plugin_key=" << plugin_key;
    return true;
}

void PluginsDialog::show_status(const wxString& message, const char* level)
{
    nlohmann::json payload;
    payload["command"] = "status_message";
    payload["level"]   = level;
    payload["message"] = into_u8(message);
    call_web_handler(payload);
}

wxString PluginsDialog::plugin_display_name(const std::string& plugin_key) const
{
    PluginDescriptor descriptor;
    if (get_descriptor(plugin_key, descriptor) && !descriptor.name.empty())
        return from_u8(descriptor.name);
    return from_u8(plugin_key);
}

void PluginsDialog::send_capability_config(const PluginCapabilityId& id) { call_web_handler(PluginConfig::get_config_response(id)); }

void PluginsDialog::save_capability_config(const PluginCapabilityId& id, const nlohmann::json& config)
{
    const nlohmann::json response = PluginConfig::save_config_response(id, config);
    call_web_handler(response);

    if (response.value("ok", false))
        show_status(_L("Configuration saved."), "success");
}

void PluginsDialog::restore_capability_config(const PluginCapabilityId& id)
{
    // Destructive, so confirm first. The confirmation stays here rather than in PluginConfig: it needs
    // a parent window.
    const int rc = wxMessageBox(wxString::Format(_L("Restore the default configuration for \"%s\"?\n\n"
                                                    "This discards the settings currently saved for this capability."),
                                                 from_u8(id.name)),
                                _L("Restore defaults"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
    if (rc != wxYES)
        return;

    const nlohmann::json response = PluginConfig::restore_config_response(id);
    call_web_handler(response);

    if (response.value("ok", false))
        show_status(_L("Default configuration restored."), "success");
}

void PluginsDialog::run_script_plugin_capability(const std::string& plugin_key, const std::string& capability_name)
{
    PluginManager& manager = PluginManager::instance();

    std::string error;
    ExecutionResult result;

    auto complete_with_error = [this, &manager, &plugin_key](const std::string& plugin_error, const wxString& status_message) {
        const std::string normalized_error = plugin_error.empty() ? "Script plugin failed." : plugin_error;
        if (!manager.set_plugin_error(plugin_key, normalized_error))
            BOOST_LOG_TRIVIAL(warning) << "Failed to record plugin error. plugin_key=" << plugin_key;

        if (!manager.unload_plugin(plugin_key))
            BOOST_LOG_TRIVIAL(error) << "Failed to unload plugin after script error. plugin_key=" << plugin_key;

        send_plugins();

        // The row already shows "Error" and the Diagnostics tab holds the full text, so report in the
        // footer status bar instead of a modal box.
        const wxString message = status_message.empty() ? from_u8(normalized_error) : status_message;
        show_status(message, "error");
    };

    {
        wxBusyCursor busy;
        result = manager.run_script_capability(plugin_key, capability_name, error);
    }

    if (!error.empty()) {
        complete_with_error(error, wxString());
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "Script plugin execution completed. plugin_key=" << plugin_key
                            << " status=" << static_cast<int>(result.status) << " message=" << result.message << " data=" << result.data;

    const bool failed = result.status == PluginResult::RecoverableError || result.status == PluginResult::FatalError || !error.empty();
    if (failed) {
        complete_with_error(result.message, wxString());
        return;
    }

    manager.clear_plugin_error(plugin_key);
    send_plugins();

    const bool     skipped  = result.status == PluginResult::Skipped;
    const wxString fallback = skipped ? _L("Script plugin skipped.") : _L("Script plugin finished.");
    const wxString message  = result.message.empty() ? fallback : from_u8(result.message);
    show_status(message, skipped ? "info" : "success");
}

void PluginsDialog::update_plugin(const std::string& plugin_key)
{
    if (plugin_key.empty())
        return;

    BOOST_LOG_TRIVIAL(info) << "Updating cloud plugin. plugin_key=" << plugin_key;

    PluginDescriptor descriptor;
    const wxString name = get_descriptor(plugin_key, descriptor) ? from_u8(descriptor.name) : from_u8(plugin_key);

    // update_cloud_plugin unloads the old plugin, deletes its local package and reinstalls the latest
    // version; all of that is off-main-thread work, so run it on the worker behind a progress dialog.
    std::string error;
    bool updated = false;
    try {
        updated = run_with_dialog_wait([plugin_key, &error]() { return PluginManager::instance().update_cloud_plugin(plugin_key, error); },
                                       _L("Updating plugin"), _L("Updating") + ": " + name);
    } catch (const std::exception& ex) {
        error = ex.what();
    } catch (...) {
        error = "Unknown error";
    }

    if (!updated) {
        BOOST_LOG_TRIVIAL(error) << "Cloud plugin update failed. plugin_key=" << plugin_key << " error=" << error;
        show_status(error.empty() ? _L("Failed to update plugin.") : from_u8(error), "error");
        send_plugins();
        return;
    }

    // update_cloud_plugin already updated the in-memory descriptor, so a UI refresh is enough here.
    send_plugins();
    show_status(wxString::Format(_L("Updated \"%s\"."), name), "success");
}

void PluginsDialog::open_plugin_folder(const PluginDescriptor& plugin)
{
    const boost::filesystem::path plugin_folder = resolve_plugin_root_from_descriptor(plugin);

    if (plugin_folder.empty()) {
        show_status(_L("Plugin folder could not be determined."), "warn");
        return;
    }

    desktop_open_any_folder(plugin_folder.string());
}

void PluginsDialog::open_plugin_on_cloud(const std::string& sharing_token)
{
    if (sharing_token.empty())
        return;

    if (!wxGetApp().getAgent())
        return;

    auto orca_agent = std::dynamic_pointer_cast<OrcaCloudServiceAgent>(wxGetApp().getAgent()->get_cloud_agent());
    if (!orca_agent)
        return;

    wxLaunchDefaultBrowser(wxString::FromUTF8(orca_agent->get_cloud_base_url() + "/p/" + sharing_token));
}

void PluginsDialog::open_plugin_hub()
{
    std::string cloud_base_url = "https://cloud.orcaslicer.com";

    if (wxGetApp().getAgent()) {
        auto orca_agent = std::dynamic_pointer_cast<OrcaCloudServiceAgent>(wxGetApp().getAgent()->get_cloud_agent());
        if (orca_agent && !orca_agent->get_cloud_base_url().empty())
            cloud_base_url = orca_agent->get_cloud_base_url();
    }

    while (!cloud_base_url.empty() && cloud_base_url.back() == '/')
        cloud_base_url.pop_back();
    if (cloud_base_url.empty())
        cloud_base_url = "https://cloud.orcaslicer.com";

    wxLaunchDefaultBrowser(wxString::FromUTF8(cloud_base_url + "/app/plugins/plugin-hub"));
}

void PluginsDialog::delete_local_plugin(const PluginDescriptor& plugin)
{
    const wxString plugin_name = from_u8(plugin.name);
    const int rc = wxMessageBox(wxString::Format(_L("Delete plugin \"%s\"?\n\nThis permanently removes the plugin folder."), plugin_name),
                                kDeletePluginTitle, wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
    restore_z_order();
    if (rc != wxYES)
        return;

    auto state = std::make_shared<PluginOperationState>();
    run_with_dialog(
        [plugin_key = plugin.plugin_key, should_refresh = plugin.is_cloud_plugin(), state]() {
            std::string error;
            const bool succeeded = PluginManager::instance().delete_plugin(plugin_key, error);
            if (succeeded && should_refresh)
                refresh_plugin_metadata_blocking(kFetchCloudMeta);
            store_plugin_operation_result(state, succeeded, std::move(error));
        },
        [this, state, plugin_name]() {
            std::string error;
            if (!take_plugin_operation_result(state, error)) {
                show_status(error.empty() ? _L("Failed to delete plugin.") : from_u8(error), "error");
                return;
            }

            send_plugins();
            show_status(wxString::Format(_L("Deleted \"%s\"."), plugin_name), "success");
        },
        _L("Deleting plugin"), _L("Deleting plugin..."));
}

void PluginsDialog::unsubscribe_cloud_plugin(const PluginDescriptor& plugin)
{
    const wxString plugin_name = from_u8(plugin.name);
    const int rc               = wxMessageBox(
        wxString::Format(_L("Unsubscribe plugin \"%s\"?\n\nThis will stop tracking the plugin and delete any local plugin files."),
                         plugin_name),
        kUnsubscribeTitle, wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
    restore_z_order();
    if (rc != wxYES)
        return;

    auto state = std::make_shared<PluginOperationState>();
    run_with_dialog(
        [plugin_key = plugin.plugin_key, state]() {
            std::string error;
            const bool succeeded = PluginManager::instance().delete_and_unsubscribe_cloud_plugin(plugin_key, error);
            store_plugin_operation_result(state, succeeded, std::move(error));
        },
        [this, state, plugin_name]() {
            std::string error;
            if (!take_plugin_operation_result(state, error)) {
                show_status(error.empty() ? _L("Failed to unsubscribe plugin.") : from_u8(error), "error");
                return;
            }

            send_plugins();
            show_status(wxString::Format(_L("Unsubscribed \"%s\"."), plugin_name), "success");
        },
        _L("Unsubscribing plugin"), _L("Deleting local files and unsubscribing plugin..."));
}

void PluginsDialog::reload_local_plugin(const std::string& plugin_key, bool clear_cache)
{
    if (plugin_key.empty())
        return;

    const bool was_loaded = PluginManager::instance().is_plugin_loaded(plugin_key);
    std::pair<bool, std::string> reload_result{false, ""};
    try {
        reload_result = run_with_dialog_wait(
            [plugin_key, was_loaded, clear_cache]() -> std::pair<bool, std::string> {
                PluginManager& manager = PluginManager::instance();

                boost::filesystem::path cache_dir;
                if (clear_cache) {
                    PluginDescriptor descriptor;
                    if (!manager.try_get_plugin_descriptor(plugin_key, descriptor))
                        return {false, "Plugin not found."};

                    boost::filesystem::path resolved_root;
                    std::string                  resolve_error;
                    if (!resolve_allowed_plugin_root(descriptor, {get_orca_plugins_dir()},
                                                     "Refusing to clear a plugin cache outside the local plugin directory.",
                                                     resolved_root, resolve_error))
                        return {false, resolve_error};
                    cache_dir = resolved_root / "__whl_extracted__";
                }

                if (!manager.unload_plugin(plugin_key))
                    return {false, "Failed to unload plugin."};

                if (clear_cache) {
                    boost::system::error_code ec;
                    boost::filesystem::remove_all(cache_dir, ec);
                    if (ec)
                        return {false, "Failed to clear plugin cache: " + ec.message()};
                }

                manager.load_plugin(plugin_key, false);
                std::string error;
                if (!manager.wait_for_plugin_load(plugin_key, std::chrono::minutes(5), error) || !manager.is_plugin_loaded(plugin_key))
                    return {false, error.empty() ? "Plugin failed to load." : error};

                if (!was_loaded && !manager.unload_plugin(plugin_key))
                    return {false, "Plugin reloaded, but failed to restore the inactive state."};

                return {true, {}};
            },
            _L("Reloading plugin"), _L("Reloading plugin"));
    } catch (const std::exception& ex) {
        reload_result = {false, ex.what()};
    } catch (...) {
        reload_result = {false, "Unknown plugin reload error."};
    }

    if (!reload_result.first) {
        show_status(reload_result.second.empty() ? _L("Failed to reload plugin.") : from_u8(reload_result.second), "warn");
        send_plugins();
        return;
    }

    send_plugins();
    show_status(wxString::Format(_L("Reloaded \"%s\"."), plugin_display_name(plugin_key)), "success");
}

void PluginsDialog::reinstall_cloud_plugin(const PluginDescriptor& plugin)
{
    const std::string plugin_key = plugin.plugin_key;
    if (plugin_key.empty())
        return;

    PluginManager& manager = PluginManager::instance();
    const bool was_loaded  = PluginManager::instance().is_plugin_loaded(plugin_key);

    std::string error;
    if (plugin.has_local_package()) {
        if (!manager.delete_plugin(plugin_key, error)) {
            show_status(error.empty() ? _L("Failed to delete plugin.") : from_u8(error), "error");
            send_plugins();
            return;
        }

        manager.update_cloud_metadata(std::vector<PluginDescriptor>{as_cloud_only_descriptor(plugin)});
    }

    if (!install_cloud_plugin(plugin_key, plugin.version, from_u8(plugin.name))) {
        send_plugins();
        return;
    }

    if (was_loaded) {
        std::pair<bool, std::string> reload_result{false, ""};
        try {
            reload_result = run_with_dialog_wait(
                [plugin_key]() -> std::pair<bool, std::string> {
                    PluginManager& manager = PluginManager::instance();
                    manager.load_plugin(plugin_key);
                    std::string error;
                    if (!manager.wait_for_plugin_load(plugin_key, std::chrono::minutes(5), error) || !manager.is_plugin_loaded(plugin_key))
                        return {false, error.empty() ? "Plugin failed to load." : error};
                    return {true, {}};
                },
                _L("Reloading plugin"), _L("Reloading plugin"));
        } catch (const std::exception& ex) {
            reload_result = {false, ex.what()};
        } catch (...) {
            reload_result = {false, "Unknown plugin reload error."};
        }

        if (!reload_result.first) {
            show_status(reload_result.second.empty() ? _L("Failed to reload plugin.") : from_u8(reload_result.second), "warn");
            send_plugins();
            return;
        }
    }

    send_plugins();
    show_status(wxString::Format(_L("Reloaded \"%s\"."), plugin_display_name(plugin_key)), "success");
}

void PluginsDialog::delete_mine_local_and_cloud_plugin(const std::string& plugin_key)
{
    PluginDescriptor descriptor;
    const std::string display  = get_descriptor(plugin_key, descriptor) ? descriptor.name : std::string{};
    const wxString plugin_name = from_u8(display.empty() ? plugin_key : display);
    const int rc               = wxMessageBox(
        wxString::Format(_L("Delete plugin \"%s\" from local and cloud?\n\nThis permanently removes the local plugin files and "
                            "deletes the plugin from the cloud. This action cannot be undone."),
                         plugin_name),
        kDeletePluginTitle, wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
    restore_z_order();
    if (rc != wxYES)
        return;

    std::string error;
    if (!PluginManager::instance().delete_mine_local_and_cloud_plugin(plugin_key, error)) {
        show_status(error.empty() ? _L("Failed to delete plugin from local and cloud.") : from_u8(error), "error");
        return;
    }

    send_plugins();
    show_status(wxString::Format(_L("Deleted \"%s\"."), plugin_name), "success");
}

}} // namespace Slic3r::GUI
