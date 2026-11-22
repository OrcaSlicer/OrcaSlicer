// The plugin commands: what the Plugins window lists and does (plugin_list, plugin_enable, plugin_uninstall,
// plugin_reload) on the operations of GUI/Operations/PluginOps.hpp, and the Plugins menu's items (plugin_install_local,
// plugin_refresh).

#include "McpArguments.hpp"
#include "McpTools.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NativeCommandMenuItems.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PluginOps.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"
#include "slic3r/plugin/PluginManager.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

json row_json(const Ops::PluginRow& row)
{
    json capabilities = json::array();
    for (const Ops::PluginCapabilityRow& capability : row.capabilities)
        capabilities.push_back({{"name", capability.name}, {"type", capability.type}, {"enabled", capability.enabled}});
    return {{"key", row.key},          {"name", row.name},     {"version", row.version}, {"installed", row.installed},
            {"enabled", row.enabled},  {"status", row.status}, {"source", row.source},   {"capabilities", std::move(capabilities)},
            {"error", row.error}};
}

json rows_json()
{
    json rows = json::array();
    for (const Ops::PluginRow& row : Ops::plugin_rows())
        rows.push_back(row_json(row));
    return rows;
}

// The row of plugin `key` as the window lists it now, or null where it lists none. UI thread.
json row_of(const std::string& key)
{
    for (const Ops::PluginRow& row : Ops::plugin_rows())
        if (row.key == key)
            return row_json(row);
    return nullptr;
}

// How a plugin stands, for an error after a step that changed it: "installed, disabled", or "no longer listed".
std::string state_of(const json& row)
{
    if (row.is_null())
        return "no longer listed";
    std::string state = std::string(row.value("installed", false) ? "installed" : "not installed") + ", " +
                        (row.value("enabled", false) ? "enabled" : "disabled");
    if (const std::string error = row.value("error", std::string()); !error.empty())
        state += ", with the error \"" + error + "\"";
    return state;
}

// `text`, then how a plugin stands (state_of(`row`)) as a sentence of its own: "<text>. It is installed, disabled.". A
// plugin's own error, which `text` may be, need not end in a full stop.
std::string with_state(std::string text, const json& row)
{
    text.erase(std::min(text.size(), text.find_last_not_of(" \t\r\n") + 1));
    if (!text.empty() && std::string(".!?").find(text.back()) == std::string::npos)
        text += '.';
    return (text.empty() ? text : text + " ") + "It is " + state_of(row) + ".";
}

// A refusal of a plugin command, with the names it carries under the key that says what they are.
ToolFailure plugin_refused(const Ops::Refusal& refusal)
{
    const std::string code = refusal.code;
    if (code == "no_such_plugin")
        return refused(refusal, "plugin_list lists the plugins by key.", "similar");
    if (code == "ambiguous_plugin")
        return refused(refusal, "", "plugins");
    if (code == "no_such_capability")
        return refused(refusal, "", "capabilities");
    if (code == "ambiguous_capability")
        return refused(refusal, "", "types");
    return refused(refusal);
}

// The plugin a call names, by its key or its name. UI thread.
PluginDescriptor named_plugin(const json& args)
{
    const Ops::Outcome<PluginDescriptor> found = Ops::find_plugin(arg_string(args, "plugin"));
    if (!found)
        throw plugin_refused(found.refusal());
    return found.value();
}

ToolResult enable_plugin(const json& args, ToolContext& ctx)
{
    Ops::SetPluginEnabledArgs op;
    op.capability = arg_string(args, "capability");
    op.enabled    = args.at("enabled").get<bool>();
    // The operation reads no capability as the plugin itself, which enabled false would unload.
    if (args.contains("capability") && op.capability.empty())
        throw ToolFailure("capability is empty: name one of the plugin's capabilities, or leave capability out to turn the plugin "
                          "itself on or off",
                          {{"code", "empty_capability"}});
    Ops::PluginChange change = Ops::PluginChange::None;
    ctx.run_ui([&] {
        ui_plater();
        op.plugin         = named_plugin(args).plugin_key;
        const json before = row_of(op.plugin);
        std::optional<Ops::Outcome<Ops::PluginEnabled>> done;
        std::string                                     thrown;
        try {
            done.emplace(Ops::set_plugin_enabled(op));
        } catch (const std::exception& ex) {
            thrown = ex.what();
        }
        // The Plugins window shows the plugin as it stands, whatever came of the call.
        wxGetApp().update_plugins_dialog();
        if (!done.has_value()) {
            const json after = row_of(op.plugin);
            throw ToolFailure(with_state(thrown, after), {{"code", op.enabled ? "not_loaded" : "not_unloaded"}, {"plugin", after}});
        }
        if (!*done)
            throw plugin_refusal_after(done->refusal(), before, row_of(op.plugin));
        change = done->value().change;
        return json();
    });
    bool still_loading = false;
    if (change == Ops::PluginChange::LoadStarted) {
        // The load runs in the background, and may ask for a permission in its dialog, which the caller answers; the
        // call waits for its end on its own thread, as the window's Activating... does.
        const std::string key  = op.plugin;
        const json        done = poll_until(
            ctx, [&key] { return PluginManager::instance().is_plugin_load_in_progress(key) ? json() : json(true); },
            std::chrono::minutes(5), std::chrono::milliseconds(200));
        still_loading = done.is_null();
        if (still_loading && ctx.cancelled())
            throw ToolFailure("Stopped waiting for " + key + " to load; its load goes on, and plugin_list shows how it ends");
    }
    const json row = ctx.run_ui([&] { return row_of(op.plugin); });
    return ToolResult::ok(
        plugin_enable_reply(op.plugin, change != Ops::PluginChange::None, change == Ops::PluginChange::LoadStarted, still_loading, row));
}

ToolResult uninstall(const json& args, ToolContext& ctx)
{
    std::optional<Ops::PluginUninstall> plan;
    ctx.run_ui([&] {
        ui_plater();
        const Ops::Outcome<Ops::PluginUninstall> planned = Ops::plugin_uninstall_plan(named_plugin(args).plugin_key);
        if (!planned)
            throw plugin_refused(planned.refusal());
        plan = planned.value();
        return json();
    });
    // The window runs this behind its progress dialog; the call runs it here, on its own thread.
    std::optional<Ops::Refusal> failed;
    try {
        if (const Ops::Outcome<Ops::Done> removed = Ops::uninstall_plugin(*plan); !removed)
            failed = removed.refusal();
    } catch (const std::exception& ex) {
        failed = Ops::Refusal{Ops::RefusalKind::Failed, "not_uninstalled", ex.what()};
    }
    // A removal that fails may have unloaded the plugin already, so the window shows it as it stands on every path.
    const json row = ctx.run_ui([&] {
        if (!failed)
            Ops::plugin_uninstalled();
        wxGetApp().update_plugins_dialog();
        return row_of(plan->plugin);
    });
    if (failed)
        throw ToolFailure(with_state(failed->english(), row), {{"code", failed->code}, {"plugin", row}});
    json reply{{"uninstalled", plan->plugin}, {"name", plan->name}, {"unsubscribed", plan->unsubscribe}};
    // A cloud plugin deleted stays listed, to be installed again.
    if (!row.is_null())
        reply["plugin"] = row;
    return ToolResult::ok(std::move(reply));
}

ToolResult reload(const json& args, ToolContext& ctx)
{
    std::optional<Ops::PluginReload> plan;
    ctx.run_ui([&] {
        ui_plater();
        const Ops::Outcome<Ops::PluginReload> planned =
            Ops::plugin_reload_plan(named_plugin(args).plugin_key, arg_bool(args, "clear_cache", false));
        if (!planned)
            throw plugin_refused(planned.refusal());
        plan = planned.value();
        return json();
    });
    // The window runs this behind its progress dialog; the call runs it here, on its own thread.
    std::optional<Ops::Refusal> failed;
    try {
        if (const Ops::Outcome<Ops::Done> reloaded = Ops::reload_plugin(*plan); !reloaded)
            failed = reloaded.refusal();
    } catch (const std::exception& ex) {
        failed = Ops::Refusal{Ops::RefusalKind::Failed, "not_loaded", ex.what()};
    }
    const json row = ctx.run_ui([&] {
        wxGetApp().update_plugins_dialog();
        return row_of(plan->plugin);
    });
    if (failed)
        throw ToolFailure(with_state(failed->english(), row), {{"code", failed->code}, {"plugin", row}});
    return ToolResult::ok({{"reloaded", plan->plugin}, {"reinstalled", plan->cloud}, {"plugin", row}});
}

ToolResult refresh(const json&, ToolContext& ctx)
{
    // The closure runs while the call waits for it, so it hands the ticket over in place.
    Ops::RefreshRun::Ticket ticket;
    ctx.run_ui([&ticket] {
        ui_plater();
        ticket = wxGetApp().refresh_plugins();
        return json();
    });
    // The refresh runs off the UI thread, after the ones asked for before it; the call waits on its own thread.
    const std::optional<Ops::RefreshRun::Status> status =
        Ops::wait_for_refresh(Ops::plugin_refresh_run(), ticket, [&ctx] { return ctx.cancelled(); }, std::chrono::milliseconds(200));
    if (!status)
        throw ToolFailure("Stopped waiting for the plugin refresh, which goes on; plugin_list shows the plugins as they stand");
    json plugins = ctx.run_ui([] { return rows_json(); });
    if (status->state == Ops::RefreshRun::State::Failed)
        throw ToolFailure("The plugin refresh failed: " + status->error, {{"code", "refresh_failed"}, {"plugins", std::move(plugins)}});
    return ToolResult::ok({{"plugins", std::move(plugins)}});
}

} // namespace

json plugin_enable_reply(const std::string& plugin, bool changed, bool load_started, bool still_loading, const json& row)
{
    if (load_started && !still_loading && (row.is_null() || !row.value("enabled", false)))
        throw ToolFailure(with_state(plugin + " did not load", row), {{"code", "not_loaded"}, {"plugin", row}});
    json reply{{"plugin", row}, {"changed", changed}};
    if (row.is_null())
        reply["note"] = plugin + " is no longer listed.";
    else if (still_loading)
        reply["note"] = "The plugin is still loading after five minutes; plugin_list shows how its load ends.";
    return reply;
}

ToolFailure plugin_refusal_after(const Ops::Refusal& refusal, const json& before, const json& after)
{
    ToolFailure bare      = plugin_refused(refusal);
    const auto  stands_as = [](const json& row) {
        return row.is_null() ? json() :
                               json{row.value("installed", false), row.value("enabled", false), row.value("error", std::string())};
    };
    if (stands_as(before) == stands_as(after))
        return bare;
    json details      = bare.details.is_object() ? bare.details : json::object();
    details["plugin"] = after;
    return ToolFailure(with_state(bare.what(), after), std::move(details));
}

void register_plugin_tools(CommandCatalog& catalog)
{
    using NativeCommands::action;
    const json plugin_prop = prop("string", "The plugin's key, as `plugin_list` gives it, or its name where one plugin has it.");

    NativeCommand list = inspect_tool(
        "plugin_list", L("List plugins"),
        "The plugins, as the Plugins window lists them: each with its key (which the other plugin commands take), name, "
        "version (the one installed, else the latest offered), installed (a package is on disk), enabled (loaded: the row's "
        "switch), status (Activated, Error, RuntimeError, Inactive or Loading), source (local, subscribed, mine or "
        "orphaned), capabilities (a loaded plugin's: name, type, enabled) and error.",
        object_schema({}), [](const json&, ToolContext& ctx) {
            json plugins = ctx.run_ui([] { return rows_json(); });
            return ToolResult::ok({{"total", plugins.size()}, {"plugins", std::move(plugins)}});
        });
    list.examples = json::parse(R"json([
        {"title": "List the plugins and how each stands", "calls": [{"tool": "plugin_list", "args": {}}]}])json");
    catalog.add(std::move(list));

    // The file its dialog asks for, which every call but the action's gives.
    const std::string replacing = "A plugin of that name installed already is replaced only with overwrite true; without it the "
                                  "call is refused with the question the dialog asks.";
    NativeCommand local = menu_command(
        "plugin_install_local", L("Install Local Plugin"),
        "Install a plugin from a .py or .whl file by absolute path, as the Plugins dialog's Install local plugin does, without its "
        "file dialog. " + replacing + " Called with no arguments, the file dialog asks for the file (`blocked_by_dialog`), and "
        "replacing an installed plugin asks first (`dialog_answer`).",
        object_schema({{"path", MenuItems::input_file("a .py or .whl plugin package")},
                       {"overwrite", prop("boolean", "Replace a plugin of that name installed already. Default false.")}},
                      {"path"}),
        json::array({action("install_local_plugin", L("Install Local Plugin"), L("Plugins"), "", json::object(),
                            "Install a plugin from the .py or .whl file a dialog asks for, as the Plugins dialog's Install local "
                            "plugin does, asking before it replaces one installed; plugin_install_local with a path installs that "
                            "file without a dialog. " + replacing)}),
        [](const json& args) { return MenuItems::install_local_plugin(args); });
    local.input_schema = with_silent(std::move(local.input_schema), person_forms(local));
    catalog.add(std::move(local));

    NativeCommand enable = reaching(
        {"set_plugin_enabled"},
        mutate_tool("plugin_enable", L("Enable or disable a plugin"),
                    "Turn a plugin, or with capability one of its capabilities, on or off, as the Plugins window's switches do. "
                    "A plugin turned on is loaded, and the call waits for its load to end; a cloud plugin not installed is "
                    "downloaded and installed first. A plugin that needs a permission while it loads asks in its permission "
                    "dialog (`blocked_by_dialog`), which the caller answers. A plugin turned off is unloaded and loses its error. "
                    "A capability is one of a loaded plugin's, by its name; an empty one is refused. Reply: plugin (its row, as "
                    "`plugin_list` gives it) and changed; a call that fails once it has changed the plugin, a load that fails "
                    "among them, is an error naming how the plugin is left.",
                    object_schema({{"plugin", plugin_prop},
                                   {"capability", {{"type", "string"},
                                                   {"minLength", 1},
                                                   {"description", "One of the plugin's capabilities, by name, in place of the "
                                                                   "plugin itself. The plugin must be enabled."}}},
                                   {"enabled", prop("boolean", "On (true) or off (false).")}},
                                  {"plugin", "enabled"}),
                    enable_plugin));
    // A plugin turned on runs its code, and a cloud plugin is downloaded.
    enable.open_world = true;
    enable.examples   = json::parse(R"json([
        {"title": "Turn a plugin on", "calls": [
            {"tool": "plugin_list", "args": {}},
            {"tool": "plugin_enable", "args": {"plugin": "my_plugin", "enabled": true}}]},
        {"title": "Turn one of a plugin's capabilities off", "calls": [
            {"tool": "plugin_enable", "args": {"plugin": "my_plugin", "capability": "Export report", "enabled": false}}]}])json");
    catalog.add(std::move(enable));

    NativeCommand reload_command = reaching(
        {"reload_plugin"},
        mutate_tool("plugin_reload", L("Reload a plugin"),
                    "Load a plugin again after its files changed, as its row's Reload does: it is unloaded and loaded, and a "
                    "plugin that was not loaded is unloaded again. `clear_cache` also removes its extracted wheel cache first, as "
                    "Delete cache and reload does. A cloud plugin is installed again from the cloud, as its Reinstall does, and "
                    "takes no `clear_cache`; one no longer available is refused. Reply: reloaded, reinstalled and plugin (its row); "
                    "a reload that fails is an error naming how the plugin is left.",
                    object_schema({{"plugin", plugin_prop},
                                   {"clear_cache", prop("boolean", "Remove the plugin's extracted wheel cache before loading it. "
                                                                   "Default false.")}},
                                  {"plugin"}),
                    reload));
    // A plugin loaded runs its code, and a cloud plugin is downloaded again.
    reload_command.open_world = true;
    reload_command.examples   = json::parse(R"json([
        {"title": "Reload a plugin after editing its files", "calls": [
            {"tool": "plugin_reload", "args": {"plugin": "my_plugin"}}]},
        {"title": "Reload a plugin with its wheel cache removed", "calls": [
            {"tool": "plugin_reload", "args": {"plugin": "my_plugin", "clear_cache": true}}]}])json");
    catalog.add(std::move(reload_command));

    NativeCommand uninstall_command = reaching(
        {"uninstall_plugin"},
        mutate_tool("plugin_uninstall", L("Uninstall a plugin"),
                    "Remove a plugin for good, without asking, as its row's Delete does once confirmed: it is unloaded where it "
                    "runs, and its files are removed. A cloud plugin subscribed to is unsubscribed, as its row's Unsubscribe "
                    "does, which also ends the subscription in the cloud account; the person's own cloud plugin loses its "
                    "package alone and stays listed, to be installed again. Reply: uninstalled (the key), name, unsubscribed, "
                    "and plugin where it stays listed; a removal that fails is an error naming how the plugin is left.",
                    object_schema({{"plugin", plugin_prop}}, {"plugin"}), uninstall, /*destructive=*/true));
    // Unsubscribe reaches the cloud account.
    uninstall_command.open_world = true;
    uninstall_command.examples   = json::parse(R"json([
        {"title": "Uninstall a plugin", "calls": [
            {"tool": "plugin_list", "args": {}},
            {"tool": "plugin_uninstall", "args": {"plugin": "my_plugin"}}]}])json");
    catalog.add(std::move(uninstall_command));

    NativeCommand refresh_command = reaching(
        {"start_plugin_refresh"},
        NativeCommands::long_command("plugin_refresh", L("Refresh Plugins"), {}, object_schema({}), refresh));
    refresh_command.description =
        "Reload the plugins, as Refresh Plugins does: the installed plugins are rescanned and the plugin hub's list fetched "
        "again, and a notification says how it went. The call waits for the refresh to end and replies with the plugins, as "
        "`plugin_list` gives them; a refresh already running, or waiting, runs before the call's own, so the reply comes from a "
        "rescan that began after the call. `job_cancel` stops the wait, not the refresh.";
    refresh_command.listed      = true;
    refresh_command.cancellable = true;
    // It fetches the plugin hub's list.
    refresh_command.open_world = true;
    refresh_command.actions =
        json::array({action("refresh_plugins", L("Refresh Plugins"), L("Plugins"), "", json::object(), "Reload plugins.")});
    // A person's pick starts the refresh and returns, its notification saying how it went; a call waits for it.
    catalog.add(NativeCommands::with_menu_item_forms(
        std::move(refresh_command), [](const json&) { return MenuItems::refresh_plugins(); }, /*person_form_behind_dialogs=*/false,
        std::vector<json>{json::object()}));
}

}}} // namespace Slic3r::GUI::Mcp
