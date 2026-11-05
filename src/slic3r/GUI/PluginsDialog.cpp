#include "PluginsDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "OrcaCloudServiceAgent.hpp"
#include "slic3r/plugin/PluginConfig.hpp"
#include "slic3r/plugin/PluginFsUtils.hpp"
#include "slic3r/plugin/PluginManager.hpp"

#include <cctype>
#include <cstddef>
#include <exception>
#include <atomic>
#include <boost/filesystem/operations.hpp>
#include <chrono>
#include <libslic3r/Utils.hpp>

#include <slic3r/GUI/NotificationManager.hpp>
#include <slic3r/GUI/Plater.hpp>
#include "slic3r/GUI/PluginSource.hpp"
#include "slic3r/GUI/PluginStatus.hpp"
#include "slic3r/GUI/Widgets/WebViewHostDialog.hpp"
#include "slic3r/GUI/PluginSort.hpp"
#include <slic3r/GUI/format.hpp>

#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <wx/busycursor.h>
#include <wx/chartype.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/gdicmn.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <nlohmann/json.hpp>

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
    bool orphaned              = false;
    bool has_local_package     = false;
    bool unauthorized          = false;
    bool has_script_capability = false;
    bool can_run_script        = false;

    // Runtime capabilities in registration order, or descriptor-only type rows when unloaded.
    std::vector<PluginCapabilityView> capabilities;

    // Grants stored in the plugin's install sidecar. False when the sidecar is missing or unreadable
    // (e.g. an uninstalled cloud plugin), so the payload reports null instead of empty permissions.
    PluginPermissions permissions;
    bool has_permissions = false;

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

PluginDescriptor as_cloud_only_descriptor(PluginDescriptor descriptor)
{
    descriptor.plugin_root.clear();
    descriptor.entry_path.clear();
    descriptor.installed_version.clear();
    // No local package is left behind, so the package verdict and any load error it produced go with it.
    descriptor.metadata_valid = false;
    descriptor.clear_error();
    if (descriptor.cloud.has_value()) {
        descriptor.cloud->installed        = false;
        descriptor.cloud->update_available = false;
    }
    return descriptor;
}

std::string to_string(PluginUpdateStatus status);
nlohmann::json build_context_actions_payload(const PluginAvailableActions& available_actions);

nlohmann::json build_permissions_payload(const PluginPermissions& permissions)
{
    nlohmann::json payload;
    payload["fs_read"]        = permissions.fs_read;
    payload["fs_readwrite"]   = permissions.fs_readwrite;
    payload["network_http"]   = permissions.network_http;
    payload["network_socket"] = permissions.network_socket;
    payload["process"]        = permissions.process;
    payload["threading"]      = permissions.threading;
    payload["events"]         = permissions.events;
    payload["full_access"]    = permissions.full_access;
    return payload;
}

// Basic shape check for a save_plugin_permissions payload: every editable category present and of
// the expected type. No target normalization or content rules here; the writer stores what the
// webview sent. full_access is not part of the payload and is preserved by the writer.
bool read_permission_list(const nlohmann::json& permissions, const char* key, std::vector<std::string>& out)
{
    if (!permissions.contains(key) || !permissions.at(key).is_array())
        return false;
    for (const auto& entry : permissions.at(key)) {
        if (!entry.is_string())
            return false;
        out.push_back(entry.get<std::string>());
    }
    return true;
}

bool parse_permissions_payload(const nlohmann::json& payload, PluginPermissions& out)
{
    if (!payload.is_object())
        return false;
    if (!read_permission_list(payload, "fs_read", out.fs_read) ||
        !read_permission_list(payload, "fs_readwrite", out.fs_readwrite) ||
        !read_permission_list(payload, "network_http", out.network_http) ||
        !read_permission_list(payload, "network_socket", out.network_socket) ||
        !read_permission_list(payload, "process", out.process) ||
        !read_permission_list(payload, "events", out.events))
        return false;
    if (!payload.contains("threading") || !payload.at("threading").is_boolean())
        return false;
    out.threading = payload.at("threading").get<bool>();
    return true;
}

struct PermissionAddition
{
    wxString label;
    std::vector<std::string> entries;
    bool boolean_grant = false;
};

// The exact entries the draft adds, grouped for the confirmation message. Comparison is exact
// string addition, which is enough here: a target re-added in a different form is a new grant,
// and removals never need confirmation. full_access is preserved separately and ignored.
std::vector<PermissionAddition> collect_permission_additions(const PluginPermissions& stored,
                                                             const PluginPermissions& requested)
{
    std::vector<PermissionAddition> additions;

    auto collect = [&additions](const wxString& label,
                                const std::vector<std::string>& stored_entries,
                                const std::vector<std::string>& requested_entries) {
        PermissionAddition addition;
        addition.label = label;
        for (const std::string& entry : requested_entries)
            if (std::find(stored_entries.begin(), stored_entries.end(), entry) == stored_entries.end())
                addition.entries.push_back(entry);
        if (!addition.entries.empty())
            additions.push_back(std::move(addition));
    };

    collect(_L("File system read"), stored.fs_read, requested.fs_read);
    collect(_L("File system read and write"), stored.fs_readwrite, requested.fs_readwrite);
    collect(_L("HTTP network"), stored.network_http, requested.network_http);
    collect(_L("Network sockets"), stored.network_socket, requested.network_socket);
    collect(_L("Processes"), stored.process, requested.process);
    collect(_L("Events"), stored.events, requested.events);

    if (requested.threading && !stored.threading) {
        PermissionAddition addition;
        addition.label         = _L("Threading");
        addition.boolean_grant = true;
        additions.push_back(std::move(addition));
    }

    return additions;
}

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
        c["type"]          = PluginConfig::capability_type_label(capability.id.type);
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
    payload_item["is_loaded"]             = dialog_item.is_loaded;
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
    payload_item["orphaned"]              = dialog_item.orphaned;
    payload_item["installed_version"]     = dialog_item.installed_version;
    payload_item["latest_version"]        = dialog_item.latest_version;
    // Null when the sidecar could not be read: an uninstalled/unreadable plugin has unknown grants,
    // not no grants.
    if (dialog_item.has_permissions)
        payload_item["permissions"] = build_permissions_payload(dialog_item.permissions);
    else
        payload_item["permissions"] = nullptr;
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

    if (is_cloud && has_cloud_meta && descriptor.cloud->orphaned)
        return PluginSource::Orphaned;
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
    const bool is_orphaned            = item.orphaned;
    const bool is_mine                = item.source == PluginSource::Mine;
    const bool has_local              = item.has_local_package;
    const bool authorized_for_install = !item.unauthorized;

    available_actions.toggle_installs_cloud_plugin = is_cloud && !is_orphaned && !has_local && authorized_for_install;
    available_actions.can_toggle                   = !is_loading && (has_local || available_actions.toggle_installs_cloud_plugin);

    auto add_action = [&available_actions](const char* id, const char* label, bool enabled = true, bool danger = false) {
        available_actions.context_actions.push_back(PluginContextAction{id, _u8L(label), enabled, danger});
    };

    // Owned cloud plugins fall through to the local delete: it removes the installed package only.
    // Deleting a plugin from the cloud is a plugin hub operation and is never offered here.
    if (is_cloud && !is_orphaned && !is_mine) {
        add_action("unsubscribe_plugin", L("Unsubscribe"), true, true);
    } else if (has_local) {
        add_action("delete_plugin", L("Delete"), true, true);
    }

    add_action("open_folder", L("Show in folder"), has_local);

    if (!is_orphaned) {
        if (is_cloud) {
            add_action("reinstall_plugin", L("Reinstall"));
        } else {
            add_action("reload_plugin", L("Reload"));
            add_action("clear_cache_reload_plugin", L("Delete cache and reload"));
        }
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
                            (descriptor.description.empty() ? _u8L("No description.") : descriptor.description);
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
            const std::string label = PluginConfig::capability_type_label(capability->type());
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
    item.orphaned              = descriptor.cloud.has_value() && descriptor.cloud->orphaned;
    item.has_local_package     = descriptor.has_local_package();
    item.unauthorized          = descriptor.is_unauthorized();
    item.is_loaded             = manager.is_plugin_loaded(descriptor.plugin_key);
    item.loading               = manager.is_plugin_load_in_progress(descriptor.plugin_key);

    PluginInstallState install_state;
    item.has_permissions = manager.get_install_state(descriptor.plugin_key, install_state);
    if (item.has_permissions)
        item.permissions = std::move(install_state.permissions);

    item.has_script_capability = false;
    for (const auto& capability : capabilities) {
        item.capabilities.push_back({capability->identity(), capability->is_enabled(), true, false, capability->config_ui_available()});
        if (capability->type() == PluginCapabilityType::Script)
            item.has_script_capability = true;
    }
    item.sharing_token = descriptor.sharing_token;
    item.thumbnail_url = descriptor.thumbnail_url;

    item.status = resolve_plugin_status(item.loading, item.has_error, item.is_loaded);

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

// A full-access or permission-save request carries an opaque correlation token that the webview
// ties to its pending operation. Bound the shape so a malformed or oversized value cannot be
// echoed back: non-empty, at most 128 bytes, printable ASCII only.
bool is_valid_bridge_request_id(const std::string& request_id)
{
    constexpr size_t kMaxRequestIdLength = 128;
    if (request_id.empty() || request_id.size() > kMaxRequestIdLength)
        return false;
    return std::all_of(request_id.begin(), request_id.end(), [](unsigned char c) {
        return c >= 0x21 && c <= 0x7e;
    });
}

// Reports the outcome of a set_plugin_full_access request to the webview, so it clears its
// pending switch only on the matching plugin's result. Sent after every completion: success,
// failure, cancelled confirmation, and invalid payload.
void send_full_access_result(WebViewHostDialog& dialog, const std::string& plugin_key, const std::string& request_id)
{
    nlohmann::json payload;
    payload["command"]    = "plugin_full_access_result";
    payload["plugin_key"] = plugin_key;
    payload["request_id"] = request_id;
    dialog.call_web_handler(payload);
}

// Reports the outcome of a save_plugin_permissions request. Sent after every completion: success,
// failure, cancelled confirmation and invalid payload. The webview keeps its draft unless
// success is true.
void send_permissions_result(WebViewHostDialog& dialog,
                             const std::string& plugin_key,
                             const std::string& request_id,
                             bool success)
{
    nlohmann::json payload;
    payload["command"]    = "plugin_permissions_result";
    payload["plugin_key"] = plugin_key;
    payload["request_id"] = request_id;
    payload["success"]    = success;
    dialog.call_web_handler(payload);
}
} // namespace

// ── Dialog-independent plugin actions (also used by the speed dial) ───────────────────────────

namespace {

// Snapshot of the currently-known cloud plugin descriptors, used to refresh metadata without a
// network round-trip (kUseCurrentCloudMeta).
std::vector<PluginDescriptor> current_cloud_metadata_snapshot()
{
    std::vector<PluginDescriptor> cloud_entries;
    for (const PluginDescriptor& entry : PluginManager::instance().get_plugin_descriptors(/*include_invalid=*/true))
        if (entry.is_cloud_plugin())
            cloud_entries.push_back(entry);
    return cloud_entries;
}

} // namespace

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

void open_plugin_hub()
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

bool install_local_plugin_package(const boost::filesystem::path& package_file, wxWindow* parent, wxString& message)
{
    message.clear();
    if (package_file.empty())
        return false;

    // ---- pre-flight (main thread): validate + inspect + overwrite prompt ----
    const wxString package_name = from_u8(package_file.filename().string());

    std::string extension = package_file.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (extension != ".py" && extension != ".whl") {
        message = _L("Select a .py or .whl plugin package.");
        return false;
    }

    PluginDescriptor plugin_descriptor;
    bool             existing_installation = false;
    std::string      error;
    try {
        if (!PluginManager::instance().inspect_local_plugin_package(package_file, plugin_descriptor, existing_installation, error)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Plugin package inspection failed for " << package_file << " error=" << error;
            message = _L("Failed to install plugin package. See the log for details.");
            return false;
        }
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Plugin package inspection failed for " << package_file << " error=" << ex.what();
        message = _L("Failed to install plugin package. See the log for details.");
        return false;
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Plugin package inspection failed for " << package_file;
        message = _L("Failed to install plugin package. See the log for details.");
        return false;
    }

    if (existing_installation) {
        const wxString plugin_name = from_u8(plugin_descriptor.name.empty() ? package_file.filename().string() : plugin_descriptor.name);
        wxMessageDialog dialog(parent,
                               wxString::Format(_L("Plugin \"%s\" is already installed.\n\nInstalling this package will overwrite the existing plugin."),
                                                plugin_name),
                               kOverwritePluginTitle, wxOK | wxCANCEL | wxCANCEL_DEFAULT | wxICON_WARNING);
        dialog.SetOKCancelLabels(_L("Overwrite"), _L("Cancel"));
        if (dialog.ShowModal() != wxID_OK) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Plugin package installation cancelled before overwrite. package=" << package_file
                                    << " plugin=" << plugin_descriptor.name;
            return false; // cancelled: message stays empty so callers stay silent
        }
    }

    // ---- install + refresh on a worker behind a modal progress dialog (keeps the UI live) ----
    bool installed = false;
    {
        struct Result
        {
            std::mutex  mutex;
            bool        ok = false;
            std::string error;
        };
        auto state = std::make_shared<Result>();

        detail::run_wait_with_progress(
            [state, package_file]() {
                std::string error;
                bool        ok = false;
                try {
                    ok = PluginManager::instance().install_plugin(package_file, error);
                } catch (const std::exception& ex) {
                    error = ex.what();
                } catch (...) {
                    error = "Unknown error";
                }
                if (ok) {
                    // Reflect the new package in discovery/cloud metadata without blocking the caller.
                    try { refresh_plugin_metadata_blocking(kUseCurrentCloudMeta); } catch (...) {}
                }
                std::lock_guard<std::mutex> lock(state->mutex);
                state->ok    = ok;
                state->error = std::move(error);
            },
            parent, _L("Installing plugin"), _L("Installing plugin") + ": " + package_name, 100,
            wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME, /*alive=*/nullptr, /*restore=*/{});

        std::lock_guard<std::mutex> lock(state->mutex);
        installed = state->ok;
        error     = std::move(state->error);
    }

    if (!installed) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Plugin package installation failed for " << package_file << " error=" << error;
        message = _L("Failed to install plugin package. See the log for details.");
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Plugin package installed successfully from " << package_file;
    const wxString installed_name = from_u8(plugin_descriptor.name.empty() ? package_file.filename().string() : plugin_descriptor.name);
    message = wxString::Format(_L("Installed \"%s\"."), installed_name);
    return true;
}

PluginsDialog::PluginsDialog(wxWindow* parent, wxWindowID id, const wxString&, const wxPoint& pos, const wxSize& size, long style)
    : WebViewHostDialog(parent, id, _L("Plugins"), pos, size, style)
{
    Bind(wxEVT_CLOSE_WINDOW, &PluginsDialog::on_window_close, this);
    Bind(wxEVT_CHAR_HOOK, &PluginsDialog::on_char_hook, this);
    create_webview("web/dialog/PluginsDialog/index.html", _L("Plugins"), wxSize(900, 820), wxSize(760, 715));
}

void PluginsDialog::on_window_close(wxCloseEvent& event)
{
    if (m_allow_close || wxGetApp().is_closing()) {
        event.Skip();
        return;
    }

    // A forced close (session end) cannot be delayed for the page's draft prompt.
    if (!event.CanVeto()) {
        event.Skip();
        return;
    }

    event.Veto();
    // The ESC char hook already asked the page; do not ask twice when wx's default ESC handling
    // turns into a close event for this same key press.
    if (!m_esc_close_request_sent && !m_close_request_in_flight)
        request_permission_dialog_close();
}

void PluginsDialog::on_char_hook(wxKeyEvent& event)
{
    if (event.GetKeyCode() != WXK_ESCAPE || m_allow_close) {
        event.Skip();
        return;
    }

    // Ask the page exactly once per ESC. The page's guard is a no-op while its confirmation
    // overlay is visible; Skip lets that overlay receive ESC and dismiss itself. Either way the
    // page decides whether close_plugins_dialog follows.
    if (!m_esc_close_request_sent) {
        m_esc_close_request_sent = true;
        request_permission_dialog_close();
        wxGetApp().CallAfter([this, alive = m_alive]() {
            if (alive->load(std::memory_order_acquire))
                m_esc_close_request_sent = false;
        });
    }
    event.Skip();
}

void PluginsDialog::request_permission_dialog_close()
{
    if (m_close_request_in_flight)
        return;

    // The page owns the unsaved-draft prompt and answers with close_plugins_dialog.
    m_close_request_in_flight = true;
    const bool asked = run_script(wxT("RequestPermissionDialogClose();"));
    m_close_request_in_flight = false;
    if (asked)
        return;

    // No loaded page to ask: close instead of trapping the user.
    m_allow_close = true;
    wxGetApp().CallAfter([this, alive = m_alive]() {
        if (alive->load(std::memory_order_acquire) && !IsBeingDeleted())
            Close();
    });
}

nlohmann::json PluginsDialog::localized_strings() const
{
    return {
        {"search_plugins", _u8L("Search plugins")},
        {"clear", _u8L("Clear")},
        {"clear_search", _u8L("Clear search")},
        {"match_case", _u8L("Match case")},
        {"match_whole_word", _u8L("Match whole word")},
        {"refresh", _u8L("Refresh")},
        {"install_options", _u8L("More plugin install options")},
        {"install_plugin", _u8L("Install plugin")},
        {"install_local_plugin", _u8L("Install local plugin")},
        {"activate", _u8L("Activate")},
        {"sort_by_name", _u8L("Sort by name")},
        {"name", _u8L("Name")},
        {"sort_by_version", _u8L("Sort by version")},
        {"plugin_version", _u8L("Plugin Version")},
        {"sort_by_source", _u8L("Sort by source")},
        {"source", _u8L("Source")},
        {"sort_by_status", _u8L("Sort by status")},
        {"status", _u8L_CONTEXT("Status", "Plugin")},
        {"resize_hint", _u8L("Drag to resize; double-click to reset")},
        {"resize_list", _u8L("Resize the plugin list")},
        {"details_label", _u8L("Plugin details")},
        {"tab_info", _u8L("Plugin Info")},
        {"description", _u8L("Description")},
        {"tab_config", _u8L("Config")},
        {"tab_changelog", _u8L("Changelog")},
        {"tab_diagnostics", _u8L("Diagnostics")},
        {"types", _u8L("Types")},
        {"author", _u8L("Author")},
        {"installed_version", _u8L("Installed Version")},
        {"latest_version", _u8L("Latest Version")},
        {"update_available", _u8L("Update available")},
        {"update_title", _u8L("Update to the latest version")},
        {"update", _u8L("Update")},
        {"no_description", _u8L("No description available")},
        {"config_select_plugin", _u8L("Select a plugin to configure its capabilities")},
        {"configurable_capabilities", _u8L("Configurable capabilities")},
        {"config_json_label", _u8L("Capability configuration (JSON)")},
        {"config_frame_title", _u8L("Plugin configuration")},
        {"restore_title", _u8L("Discard the settings saved for this capability and restore the plugin's defaults")},
        {"restore_defaults", _u8L("Restore defaults")},
        {"save", _u8L("Save")},
        {"changelog_label", _u8L("Plugin changelog")},
        {"version", _u8L("Version")},
        {"date", _u8L("Date")},
        {"changes", _u8L("Changes")},
        {"no_changelog", _u8L("No changelog available")},
        {"no_issues", _u8L("No issues detected")},
        {"no_plugins", _u8L("No plugins found")},
        {"no_match", _u8L("No plugins match \"%s\"")},
        {"source_mine", _u8L("Mine")},
        {"source_subscribed", _u8L_CONTEXT("Subscribed", "Plugin")},
        {"source_orphaned", _u8L("Orphaned")},
        {"source_local", _u8L_CONTEXT("Local", "Plugin")},
        {"mixed_capabilities", _u8L("Some plugin capabilities are enabled")},
        {"collapse_named", _u8L("Collapse %s capabilities")},
        {"expand_named", _u8L("Expand %s capabilities")},
        {"collapse", _u8L("Collapse capabilities")},
        {"expand", _u8L("Expand capabilities")},
        {"open_in_browser", _u8L("Open this plugin in your browser")},
        {"capability_count", _u8L("Capabilities: %s")},
        {"capabilities", _u8L_CONTEXT("Capabilities", "Plugin")},
        {"permissions_tab", _u8L("Permissions")},
        {"permissions_granted", _u8L("Granted permissions")},
        {"permissions_unavailable", _u8L("Permissions are unavailable.")},
        {"permissions_empty", _u8L("No permissions granted.")},
        {"permissions_fs_read", _u8L("File system read")},
        {"permissions_fs_readwrite", _u8L("File system read and write")},
        {"permissions_network_http", _u8L("HTTP network")},
        {"permissions_network_socket", _u8L("Network sockets")},
        {"permissions_process", _u8L("Processes")},
        {"permissions_threading", _u8L("Threading")},
        {"permissions_events", _u8L("Events")},
        {"permissions_filesystem_help", _u8L("Enter an absolute path. Add /* at the end to include its subtree.")},
        {"permissions_filesystem_placeholder", _u8L("/path/to/directory/*")},
        {"permissions_network_access_guidance", _u8L("Enter an HTTP or HTTPS URL using a domain name or IP address. Access applies to the entire host; ports, paths, and queries are not used to limit it. Grants in either list are checked for HTTP and socket access.")},
        {"permissions_network_placeholder", _u8L("https://example.com")},
        {"permissions_process_help", _u8L("Enter the exact value recorded by the audit. It may be a Python representation of the command.")},
        {"permissions_process_placeholder", _u8L("Exact audited value")},
        {"permissions_threading_help_short", _u8L("Allow this plugin to create threads.")},
        {"permissions_events_help", _u8L("Enter a category and event separated by a colon, such as Socket:socket.__new__.")},
        {"permissions_events_placeholder", _u8L("Socket:socket.__new__")},
        {"permissions_full_access_label", _u8L("Full access")},
        {"permissions_group_filesystem", _u8L("File system")},
        {"permissions_network_access_title", _u8L("Network access")},
        {"permissions_group_execution", _u8L("Execution")},
        {"permissions_group_events", _u8L("Audit events")},
        {"permissions_files_folders", _u8L("Files and folders")},
        {"permissions_access_read", _u8L("Read")},
        {"permissions_access_readwrite", _u8L("Read and write")},
        {"permissions_read_unavailable", _u8L("Read permissions are unavailable.")},
        {"permissions_readwrite_unavailable", _u8L("Read and write permissions are unavailable.")},
        {"permissions_filesystem_access_label", _u8L("Access for new path")},
        {"permissions_filesystem_access_name", _u8L("File system: %s")},
        {"permissions_full_access_help_live_v2", _u8L("Full access bypasses permission checks. Direct Python access to install-state files remains blocked, but subprocesses are not restricted.")},
        {"permissions_full_access_editor_disabled", _u8L("Permission list editing is disabled while full access is on.")},
        {"permissions_full_access_unavailable", _u8L("Full access status is unavailable.")},
        {"permissions_full_access_updating", _u8L("Updating full access…")},
        {"permissions_saving_unavailable_notice", _u8L("Permission list changes do not affect access and cannot be saved yet. Full access is saved separately.")},
        {"permissions_changes_outdated", _u8L("Stored permissions have changed. Reset changes to see the latest values.")},
        {"permissions_save_changes_unavailable", _u8L("Save changes (not yet available)")},
        {"permissions_save_changes", _u8L("Save changes")},
        {"permissions_save_notice", _u8L("Permission changes take effect after saving. Full access is saved separately.")},
        {"permissions_unapplied_notice", _u8L("Apply or remove the text you entered before saving.")},
        {"permissions_unsaved_title", _u8L("Unsaved permission changes")},
        {"permissions_unsaved_unapplied", _u8L("Apply or remove the text you entered before saving.")},
        {"permissions_unsaved_description", _u8L("Save or discard your changes before leaving.")},
        {"permissions_save_close", _u8L("Save and close")},
        {"permissions_save_switch", _u8L("Save and switch")},
        {"permissions_discard_close", _u8L("Discard and close")},
        {"permissions_discard_switch", _u8L("Discard and switch")},
        {"permissions_continue_editing", _u8L("Continue editing")},
        {"permissions_reset_changes", _u8L("Reset changes")},
        {"permissions_values_empty", _u8L("No values added.")},
        {"permissions_apply", _u8L("Apply")},
        {"permissions_apply_value_label", _u8L("Apply value for %s")},
        {"permissions_install_to_edit", _u8L("Install this plugin to edit its permissions.")},
        {"permissions_add_value_label", _u8L("New value for %s")},
        {"permissions_edit_value_label", _u8L("Edit value for %s")},
        {"permissions_add", _u8L("Add")},
        {"permissions_save", _u8L("Save")},
        {"permissions_add_label", _u8L("Add value to %s")},
        {"permissions_save_label", _u8L("Save value for %s")},
        {"permissions_remove", _u8L("Remove")},
        {"permissions_remove_label", _u8L("Remove %s")},
        {"run_named", _u8L("Run \"%s\"")},
        {"enable_named", _u8L("Enable %s")},
        {"not_available", _u8L("N/A")},
        {"unauthorized", _u8L("Unauthorized for updates")},
        {"not_installed", _u8L("Not installed")},
        {"config_loading", _u8L("Loading the plugin…")},
        {"config_no_capabilities", _u8L("This plugin exposes no capabilities")},
        {"config_activate", _u8L("Activate this plugin to configure its capabilities")},
        {"invalid_json", _u8L("Invalid JSON")},
        {"view_on_cloud", _u8L("View on OrcaCloud <a>here</a>")},
        {"status_activated", _u8L("Activated")},
        {"status_loading", _u8L("Loading")},
        {"status_error", _u8L("Error")},
        {"status_runtime_error", _u8L("Runtime error")},
        {"status_inactive", _u8L("Inactive")},
        {"status_activated_desc", _u8L("This plugin is active and ready to be used.")},
        {"status_loading_desc", _u8L("This plugin is still loading.")},
        {"status_error_desc", _u8L("This plugin is blocked until its error is fixed.")},
        {"status_runtime_error_desc", _u8L("This plugin is loaded but a capability reported an error.")},
        {"status_inactive_desc", _u8L("This plugin is inactive. Activate it to install or load it.")},
        {"select_plugin", _u8L("Select a plugin to view its status and details.")},
        {"orphaned_note", _u8L("Orphaned: This plugin is no longer subscribed or available in OrcaCloud. The local copy remains installed and can still be used.")},
        {"newer_version_note", _u8L("A newer plugin version is available.")},
        {"unauthorized_note", _u8L("Cloud updates are unavailable because this plugin is unauthorized for updates.")},
    };
}

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
    } else if (command == "set_plugin_full_access") {
        // The webview is shared with plugin-provided UI, so validate the shape before acting:
        // a non-string key, a non-boolean flag or a malformed correlation token is refused rather
        // than coerced. The result still echoes the token so the sender can clear its pending state.
        const std::string plugin_key = payload.contains("plugin_key") && payload.at("plugin_key").is_string()
                                           ? payload.at("plugin_key").get<std::string>()
                                           : std::string{};
        const std::string request_id = payload.contains("request_id") && payload.at("request_id").is_string()
                                           ? payload.at("request_id").get<std::string>()
                                           : std::string{};
        const bool request_id_valid = is_valid_bridge_request_id(request_id);
        if (!request_id_valid || !payload.contains("plugin_key") || !payload.at("plugin_key").is_string() ||
            !payload.contains("enabled") || !payload.at("enabled").is_boolean()) {
            show_status(_L("Invalid full access request."), "error");
            send_plugins();
            send_full_access_result(*this, plugin_key, request_id_valid ? request_id : std::string{});
            return;
        }
        set_plugin_full_access(plugin_key, payload.at("enabled").get<bool>(), request_id);
    } else if (command == "save_plugin_permissions") {
        // Same shape guard as full access: the webview is shared with plugin-authored UI, so a
        // non-string key, malformed token or wrongly typed category list is refused, not coerced.
        const std::string plugin_key = payload.contains("plugin_key") && payload.at("plugin_key").is_string()
                                           ? payload.at("plugin_key").get<std::string>()
                                           : std::string{};
        const std::string request_id = payload.contains("request_id") && payload.at("request_id").is_string()
                                           ? payload.at("request_id").get<std::string>()
                                           : std::string{};
        const bool request_id_valid = is_valid_bridge_request_id(request_id);
        PluginPermissions permissions;
        const bool valid = request_id_valid && !plugin_key.empty() &&
                           payload.contains("permissions") && parse_permissions_payload(payload.at("permissions"), permissions);
        if (!valid) {
            show_status(_L("Invalid permission save request."), "error");
            send_plugins();
            send_permissions_result(*this, plugin_key, request_id_valid ? request_id : std::string{}, /*success=*/false);
            return;
        }
        save_plugin_permissions(plugin_key, permissions, request_id);
    } else if (command == "close_plugins_dialog") {
        // The page handled any unsaved draft. Ignore a duplicate command and authorize the close
        // first so the close event it raises cannot be intercepted again.
        if (m_allow_close)
            return;
        m_allow_close = true;
        m_esc_close_request_sent = false;
        wxGetApp().CallAfter([this, alive = m_alive]() {
            if (alive->load(std::memory_order_acquire) && !IsBeingDeleted())
                Close();
        });
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
    run_with_dialog([fetch_cloud]() { refresh_plugin_metadata_blocking(fetch_cloud); }, [this]() {
        prompt_for_missing_plugins();
        send_plugins();
    }, title, message);
}

void PluginsDialog::prompt_for_missing_plugins()
{
    PluginManager& manager = PluginManager::instance();
    const std::vector<PluginDescriptor> missing = manager.get_missing_plugin_descriptors();
    if (missing.empty())
        return;

    wxString names;
    std::vector<std::string> keys;
    keys.reserve(missing.size());
    for (const PluginDescriptor& plugin : missing) {
        keys.push_back(plugin.plugin_key);
        names += "\n- ";
        names += plugin_display_name(plugin.plugin_key);
    }

    const int result = wxMessageBox(
        wxString::Format(_L("The following installed plugins were not found on disk:\n%s\n\nRemove them from OrcaSlicer?"), names),
        _L("Missing Plugins"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
    restore_z_order();

    if (result == wxYES)
        manager.remove_missing_plugins(keys);
}

void PluginsDialog::refresh_plugins()
{
    BOOST_LOG_TRIVIAL(info) << "Refreshing plugins from Plugins dialog";

    refresh_plugin_metadata_async(_L("Refreshing"), _L("Refreshing plugins data"), kFetchCloudMeta);
}

void PluginsDialog::set_plugin_full_access(const std::string& plugin_key, bool enabled, const std::string& request_id)
{
    if (plugin_key.empty()) {
        show_status(_L("Invalid full access request."), "error");
        send_plugins();
        send_full_access_result(*this, plugin_key, request_id);
        return;
    }

    const wxString plugin_name = plugin_display_name(plugin_key);

    // Enabling is the dangerous direction, and this dialog can be driven by a sandboxed,
    // plugin-authored iframe that sends native commands directly. Require an explicit Yes from
    // the host UI; any other outcome cancels and refreshes, so a scripted dismissal grants nothing.
    if (enabled) {
        MessageDialog dialog(
            this,
            wxString::Format(
                _L("Enable full access for \"%s\"?\n\n"
                   "Full access turns off permission auditing for this plugin: its file system, network, "
                   "process, thread and event permissions are no longer checked. Direct Python access to "
                   "the install-state files remains blocked, but subprocesses are not restricted.\n\n"
                   "Only enable this for a plugin you trust."),
                plugin_name),
            _L("Full access"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
        // MsgDialog builds its own buttons in a fixed order and always gives Yes the initial focus;
        // wxNO_DEFAULT does not change that. Focus No explicitly so Enter cannot grant full access.
        dialog.SetButtonLabel(wxID_NO, _L("No"), /*set_focus=*/true);

        const int rc = dialog.ShowModal();
        restore_z_order();
        if (rc != wxID_YES) {
            send_plugins();
            send_full_access_result(*this, plugin_key, request_id);
            return;
        }
    }

    if (!PluginManager::instance().set_plugin_full_access(plugin_key, enabled)) {
        show_status(_L("Failed to update full access."), "error");
        send_plugins();
        send_full_access_result(*this, plugin_key, request_id);
        return;
    }

    send_plugins();
    send_full_access_result(*this, plugin_key, request_id);
    show_status(wxString::Format(enabled ? _L("Full access enabled for \"%s\".") : _L("Full access disabled for \"%s\"."), plugin_name),
                "success");
}

void PluginsDialog::save_plugin_permissions(const std::string& plugin_key, const PluginPermissions& permissions, const std::string& request_id)
{
    if (plugin_key.empty()) {
        show_status(_L("Invalid permission save request."), "error");
        send_plugins();
        send_permissions_result(*this, plugin_key, request_id, /*success=*/false);
        return;
    }

    // An uninstalled or unreadable plugin has no sidecar to replace. The webview should not have
    // offered Save for it, but the command can arrive from a plugin-authored iframe, so refuse.
    PluginInstallState stored;
    if (!PluginManager::instance().get_install_state(plugin_key, stored)) {
        show_status(_L("Install this plugin to edit its permissions."), "warn");
        send_plugins();
        send_permissions_result(*this, plugin_key, request_id, /*success=*/false);
        return;
    }

    const wxString plugin_name = plugin_display_name(plugin_key);

    // Reject before any confirmation: while full access is on the permission list is inert, so a
    // save would silently store grants that take effect only after full access is turned off.
    if (stored.permissions.full_access) {
        show_status(_L("Permission list editing is disabled while full access is on."), "warn");
        send_plugins();
        send_permissions_result(*this, plugin_key, request_id, /*success=*/false);
        return;
    }

    // A saved list is a replacement, so removals need no confirmation. Added or broadened grants
    // do: this dialog can be driven by a sandboxed, plugin-authored iframe that sends native
    // commands directly. Any outcome other than an explicit Yes cancels, and the draft survives.
    const std::vector<PermissionAddition> additions = collect_permission_additions(stored.permissions, permissions);
    if (!additions.empty()) {
        wxString details;
        for (const PermissionAddition& addition : additions) {
            if (!details.empty())
                details += "\n\n";
            if (addition.boolean_grant) {
                details += format_wxstr(_L("%1%: enabled"), addition.label);
            } else {
                wxString entries;
                for (const std::string& entry : addition.entries) {
                    if (!entries.empty())
                        entries += "\n";
                    entries += from_u8(entry);
                }
                details += format_wxstr(_L("%1%:\n%2%"), addition.label, entries);
            }
        }

        MessageDialog dialog(
            this,
            wxString::Format(
                _L("Save these permissions for \"%s\"?\n\n"
                   "The following permissions will be added and take effect immediately:\n\n%s\n\n"
                   "Only save this for a plugin you trust."),
                plugin_name, details),
            _L("Save Permissions"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
        // MsgDialog always gives Yes the initial focus; focus No so Enter cannot grant access.
        dialog.SetButtonLabel(wxID_NO, _L("No"), /*set_focus=*/true);

        const int rc = dialog.ShowModal();
        restore_z_order();
        if (rc != wxID_YES) {
            send_plugins();
            send_permissions_result(*this, plugin_key, request_id, /*success=*/false);
            return;
        }
    }

    if (!PluginManager::instance().set_plugin_permissions(plugin_key, permissions)) {
        show_status(_L("Failed to save permissions."), "error");
        send_plugins();
        send_permissions_result(*this, plugin_key, request_id, /*success=*/false);
        return;
    }

    send_plugins();
    send_permissions_result(*this, plugin_key, request_id, /*success=*/true);
    show_status(wxString::Format(_L("Permissions saved for \"%s\"."), plugin_name), "success");
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
        // A user-disabled plugin has no meaningful error state.
        if (!manager.clear_plugin_error(plugin_key))
            BOOST_LOG_TRIVIAL(warning) << "Failed to clear plugin error for " << plugin_key << " (failed to find)";
        // A prior activation of this plugin is moot now; drop it so no stale "Activated" arrives later.
        if (m_activating_plugin_key == plugin_key)
            m_activating_plugin_key.clear();
        send_plugins();
        show_status(wxString::Format(_L("Deactivated \"%s\"."), plugin_display_name(plugin_key)), "success");
        return;
    }

    if (!available_actions.can_toggle) {
        if (dialog_item.unauthorized && available_actions.toggle_installs_cloud_plugin == false && row_data.has_local_package() == false) {
            const std::string install_error = _u8L("Unauthorized cloud plugins cannot be installed.");
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
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Installing local plugin package from path: " << package_path;

    const boost::filesystem::path package_file(package_path);
    wxString message;
    const bool installed = install_local_plugin_package(package_file, this, message);
    // The helper's overwrite prompt and progress dialog can push this webview behind; re-raise it
    // once, after both have closed (the speed-dial path parents to the mainframe instead).
    restore_z_order();

    // The shared helper reports a user-cancelled overwrite with an empty message: stay silent.
    if (message.IsEmpty()) {
        send_plugins();
        return false;
    }

    if (!installed) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Failed to install plugin package.";
        show_status(message, "warn");
        send_plugins();
        return false;
    }

    show_status(message, "success");
    prompt_for_missing_plugins();
    send_plugins();
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
        error = _u8L("Unknown error");
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

    auto complete_with_error = [this, &manager, plugin_key](const std::string& plugin_error, const wxString& status_message) {
        const std::string normalized_error = plugin_error.empty() ? _u8L("Script plugin failed.") : plugin_error;
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

    // Always runs on the GUI thread. For a started invocation (sync `def execute` or `async def`)
    // the runtime delivers it later; it is called inline here only when the launch itself failed.
    auto finish = [this, alive = m_alive, plugin_key, &manager, complete_with_error](const ScriptCompletion& completion) {
        if (!alive->load())
            return;

        // Cancelled by PluginManager (unload/shutdown): expected termination. Report neither an
        // error nor a normal success, and do not touch the plugin's error/state.
        if (completion.cancelled)
            return;

        if (!completion.error.empty()) {
            complete_with_error(completion.error, wxString());
            return;
        }

        const ExecutionResult& result = completion.result;

        BOOST_LOG_TRIVIAL(info) << "Script plugin execution completed. plugin_key=" << plugin_key
                                << " status=" << static_cast<int>(result.status) << " message=" << result.message
                                << " data=" << result.data;

        const bool failed = result.status == PluginResult::RecoverableError || result.status == PluginResult::FatalError;
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
    };

    PluginManager::ScriptLaunchResult launch;
    {
        wxBusyCursor busy;
        launch = manager.launch_script_capability(plugin_key, capability_name, finish);
    }

    // Every started invocation — a synchronous `def execute` included — completes later through
    // `finish`; only a launch failure is reported inline. Return without blocking.
    if (!launch.launched_async)
        finish(launch.outcome);
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
        error = _u8L("Unknown error");
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

void PluginsDialog::open_plugin_hub() { Slic3r::GUI::open_plugin_hub(); }

void PluginsDialog::delete_local_plugin(const PluginDescriptor& plugin)
{
    const wxString plugin_name = from_u8(plugin.name);
    const int rc               = wxMessageBox(
        wxString::Format(plugin.is_cloud_plugin() ?
                             _L("Delete plugin \"%s\"?\n\nThis removes the local plugin files. The plugin stays in the cloud "
                                "and can be reinstalled.") :
                             _L("Delete plugin \"%s\"?\n\nThis permanently removes the plugin folder."),
                         plugin_name),
        kDeletePluginTitle, wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
    restore_z_order();
    if (rc != wxYES)
        return;

    auto state = std::make_shared<PluginOperationState>();
    run_with_dialog(
        [plugin_key = plugin.plugin_key, cloud_row = as_cloud_only_descriptor(plugin), state]() {
            std::string error;
            const bool succeeded = PluginManager::instance().delete_plugin(plugin_key, error);
            // Keep the cloud row listed so the plugin stays reinstallable without waiting on a cloud fetch.
            if (succeeded && cloud_row.is_cloud_plugin())
                PluginManager::instance().update_cloud_metadata(std::vector<PluginDescriptor>{cloud_row});
            store_plugin_operation_result(state, succeeded, std::move(error));
        },
        [this, state, plugin_name]() {
            std::string error;
            if (!take_plugin_operation_result(state, error)) {
                show_status(error.empty() ? _L("Failed to delete plugin.") : from_u8(error), "error");
                return;
            }

            send_plugins();
            // The package is gone only now; the unload callback rechecked the notices too early.
            if (Plater* plater = wxGetApp().plater())
                plater->refresh_missing_plugin_notifications();
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
            // The package is gone only now; the unload callback rechecked the notices too early.
            if (Plater* plater = wxGetApp().plater())
                plater->refresh_missing_plugin_notifications();
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
                        return {false, _u8L("Plugin not found.")};

                    boost::filesystem::path resolved_root;
                    std::string                  resolve_error;
                    if (!resolve_allowed_plugin_root(descriptor, {get_orca_plugins_dir()},
                                                     _u8L("Refusing to clear a plugin cache outside the local plugin directory."),
                                                     resolved_root, resolve_error))
                        return {false, resolve_error};
                    cache_dir = resolved_root / "__whl_extracted__";
                }

                if (!manager.unload_plugin(plugin_key))
                    return {false, _u8L("Failed to unload plugin.")};

                if (clear_cache) {
                    boost::system::error_code ec;
                    boost::filesystem::remove_all(cache_dir, ec);
                    if (ec)
                        return {false, GUI::format(_u8L("Failed to clear plugin cache: %1%"), ec.message())};
                }

                manager.load_plugin(plugin_key, false);
                std::string error;
                if (!manager.wait_for_plugin_load(plugin_key, std::chrono::minutes(5), error) || !manager.is_plugin_loaded(plugin_key))
                    return {false, error.empty() ? _u8L("Plugin failed to load.") : error};

                if (!was_loaded && !manager.unload_plugin(plugin_key))
                    return {false, _u8L("Plugin reloaded, but failed to restore the inactive state.")};

                return {true, {}};
            },
            _L("Reloading plugin"), _L("Reloading plugin"));
    } catch (const std::exception& ex) {
        reload_result = {false, ex.what()};
    } catch (...) {
        reload_result = {false, _u8L("Unknown plugin reload error.")};
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
                        return {false, error.empty() ? _u8L("Plugin failed to load.") : error};
                    return {true, {}};
                },
                _L("Reloading plugin"), _L("Reloading plugin"));
        } catch (const std::exception& ex) {
            reload_result = {false, ex.what()};
        } catch (...) {
            reload_result = {false, _u8L("Unknown plugin reload error.")};
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

}} // namespace Slic3r::GUI
