// preferences_get and preferences_set: the app's preferences as the Preferences dialog shows them, read from and
// written through the table of GUI/Operations/PreferenceOps.hpp, which the dialog builds its rows from too.

#include "McpTools.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PreferenceOps.hpp"

#include "libslic3r/AppConfig.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

const char* kind_name(Ops::PreferenceKind kind)
{
    switch (kind) {
    case Ops::PreferenceKind::Checkbox: return "checkbox";
    case Ops::PreferenceKind::Choice: return "choice";
    case Ops::PreferenceKind::Number: return "number";
    case Ops::PreferenceKind::Text: return "text";
    }
    return "text";
}

// A bound of a number's range, whole where the row takes whole numbers.
json bound(const Ops::Preference& p, double value)
{
    if (p.decimals == 0)
        return std::int64_t(value);
    return value;
}

// What a call learns of `p`'s refusal to be written: its reason, in English.
std::string refused_reason(const Ops::Preference& p)
{
    return Ops::Refusal{Ops::RefusalKind::Precondition, "preference_refused", p.refused, {p.key}}.english();
}

// preferences_get's item for `p`: its row, and the value the dialog shows: `shown` where the dialog shows a value of its
// own (Ops::shown_texts()), else the stored one, else the fallback.
json preference_item(const AppConfig& config, const Ops::Preference& p, const std::map<std::string, std::string>& shown)
{
    std::string       stored;
    const bool        is_stored = config.get("app", p.key, stored);
    const auto        own       = shown.find(p.key);
    const std::string text      = own != shown.end() ? own->second : is_stored ? stored : p.fallback;
    json item{{"key", p.key},     {"tab", p.tab},  {"section", p.section},
              {"label", p.label}, {"kind", kind_name(p.kind)},
              {"value", Ops::preference_value(p, text)}};
    if (!p.choices.empty()) {
        json choices = json::array();
        for (const auto& [value, label] : p.choices)
            choices.push_back({{"value", value}, {"label", label}});
        item["choices"] = std::move(choices);
    }
    if (p.min)
        item["min"] = bound(p, *p.min);
    if (p.max)
        item["max"] = bound(p, *p.max);
    if (!is_stored)
        item["stored"] = false;
    if (p.restart_needed)
        item["restart_needed"] = true;
    if (p.refused != nullptr)
        item["refused"] = refused_reason(p);
    return item;
}

// What preferences_set replies when the operation refuses: the names the refusal carries under the key that says what
// they are, and where a refused preference changes instead.
ToolFailure preferences_refused(const Ops::Refusal& refusal, const std::vector<Ops::Preference>& table)
{
    const std::string code = refusal.code;
    if (code == "not_a_preference")
        return refused(refusal, "preferences_get lists the Preferences dialog's preferences.", "similar");
    if (code == "no_such_choice")
        return refused(refusal, "", "choices");
    if (code == "out_of_range")
        return refused(refusal, "", "range");
    if (code == "preference_refused" && !refusal.args.empty())
        if (const Ops::Preference* p = Ops::find_preference(table, refusal.args.front()))
            return refused(refusal, "It changes in the Preferences dialog: window_open {\"window\": \"preferences\", \"tab\": \"" + p->tab +
                                        "\", \"key\": \"" + p->key + "\"}, then widget_set_value on the control named " + p->key +
                                        " within Preferences.",
                           "keys");
    return refused(refusal);
}

// preferences_set's reply: each preference changed, with its value before and after, typed as preferences_get gives it,
// stored_before: false where `before` is the fallback, restart_needed where the value is read at the next start; and
// those already so.
json changes_json(const Ops::PreferenceChanges& changes, const std::vector<Ops::Preference>& table)
{
    json changed = json::array();
    for (const Ops::PreferenceChange& c : changes.changed) {
        const Ops::Preference& p = *Ops::find_preference(table, c.key);
        json item{{"key", c.key}, {"before", Ops::preference_value(p, c.before)}, {"after", Ops::preference_value(p, c.after)}};
        if (!c.stored_before)
            item["stored_before"] = false;
        if (p.restart_needed)
            item["restart_needed"] = true;
        changed.push_back(std::move(item));
    }
    return {{"changed", std::move(changed)}, {"unchanged", changes.unchanged}};
}

} // namespace

json preferences_settings(const AppConfig& config, const std::vector<Ops::Preference>& shown, const json& args,
                          const std::map<std::string, std::string>& shown_texts)
{
    const std::string        tab = arg_string(args, "tab");
    std::vector<std::string> asked;
    if (args.contains("keys"))
        for (const json& key : args["keys"])
            asked.push_back(key.get<std::string>());
    const auto named = [&asked](const std::string& key) { return std::find(asked.begin(), asked.end(), key) != asked.end(); };
    const auto matches = [&](const Ops::Preference& p) {
        return (tab.empty() || p.tab == tab) && (!args.contains("keys") || named(p.key));
    };

    json items = json::array(), unknown = json::array();
    for (const Ops::Preference& p : shown)
        if (matches(p))
            items.push_back(preference_item(config, p, shown_texts));
    // A named key the dialog does not show is read as the app configuration stores it.
    for (const std::string& key : asked) {
        if (Ops::find_preference(shown, key) != nullptr)
            continue;
        std::string value;
        if (config.get("app", key, value))
            items.push_back({{"key", key}, {"value", value}});
        else
            unknown.push_back(key);
    }
    json out{{"total", items.size()}, {"items", std::move(items)}};
    if (!unknown.empty()) {
        out["unknown_keys"] = std::move(unknown);
        // Some preferences are stored only once they are changed.
        out["note"] = "unknown_keys are not stored and are no preference the Preferences dialog shows now.";
    }
    return out;
}

void register_preference_tools(CommandCatalog& catalog)
{
    catalog.add(inspect_tool(
        "preferences_get", L("Read preferences"),
        "Read the app's preferences as the Preferences dialog shows them. With no argument, every preference it shows, "
        "tab by tab in its order, each with key (its control is named after it), tab, section and label (the row's title, "
        "in English), kind (checkbox, choice, number or text), value (what the dialog shows: true or false for a checkbox, "
        "a number for a number field), a choice's choices ({value, label}; preferences_set takes the value), a number's "
        "min and max, stored: false where nothing is stored and the value is the one the dialog shows then, "
        "restart_needed: true for a preference read at the next start, which a write changes nothing before, and "
        "refused, the reason, for a preference preferences_set does not write. The MCP server's switch and port read as "
        "the dialog shows them: on while the server runs, and the port it is on, also where this session's launch "
        "arguments set them without storing them; their stored says whether a value is stored. keys and tab narrow "
        "the list together; a "
        "named key the dialog does not show is read as the app configuration stores it ({key, value}), and one that is "
        "neither is listed under unknown_keys. The app configuration also holds account and device state, which is read "
        "only by name.",
        object_schema({{"keys", array_prop({{"type", "string"}}, "Preference keys, e.g. [\"project_load_behaviour\"].")},
                       {"tab", enum_prop({"general", "files", "behaviour", "control", "graphics", "online", "developer"},
                                         "The dialog's tab, as window_open names it.")}}),
        [](const json& args, ToolContext& ctx) {
            return ToolResult::ok(ctx.run_ui([&] {
                ui_plater();
                return preferences_settings(*wxGetApp().app_config, Ops::shown_preferences(), args, Ops::shown_texts());
            }));
        }));

    NativeCommand write = reaching(
        {"set_preferences"},
        mutate_tool(
            "preferences_set", L("Change preferences"),
            "Change preferences as their controls in the Preferences dialog do: values {key: value}, a checkbox as true or "
            "false, a choice as one of its values, a number within its range, as preferences_get lists them, and the "
            "downloads folder (download_path) as the absolute path of an existing folder. All are checked first and written "
            "together, so nothing changes if one is refused; then each does what its control does in the app, in the keys' "
            "order (the preview redrawn, the sidebar's lists, the settings' mode, the theme, preset sync started or "
            "stopped, stealth mode's sign-out of the cloud accounts), and those preferences_get marks restart_needed are "
            "stored and take effect at the next start. A key whose value is already the one given is left as it is. "
            "Refused, by name and with the reason: a key the dialog does not show, and those preferences_get marks "
            "refused, whose control opens a dialog, restarts OrcaSlicer, signs out, or reaches outside it (the language, "
            "the login region, the network plug-in, file associations, the mixed-temperature warning, the MCP server's "
            "switch and port); those change in the dialog: window_open {\"window\": \"preferences\", \"tab\", "
            "\"key\"}, then widget_set_value. Not in the undo history. Reply: changed, [{key, before, after}] typed as "
            "preferences_get gives them, with stored_before: false where nothing was stored and before is the value the "
            "dialog showed, and restart_needed: true where preferences_get gives it; and unchanged, the keys already so.",
            object_schema({{"values", {{"type", "object"},
                                       {"description", "{key: value}, e.g. {\"auto_arrange\": false, \"auto_calculate_flush\": "
                                                       "\"color change\"}."}}}},
                          {"values"}),
            [](const json& args, ToolContext& ctx) {
                const json values = args.value("values", json::object());
                if (values.empty())
                    throw ToolFailure("values names no preference: give {key: value}, e.g. {\"auto_arrange\": false}");
                std::map<std::string, json> asked;
                for (const auto& [key, value] : values.items())
                    asked[key] = value;
                json result = ctx.run_ui([&]() -> json {
                    ui_plater();
                    const std::vector<Ops::Preference>            table = Ops::shown_preferences();
                    const Ops::Outcome<Ops::PreferenceChanges> done  = Ops::set_preferences(asked);
                    if (!done)
                        throw preferences_refused(done.refusal(), table);
                    return changes_json(done.value(), table);
                });
                settle(ctx);
                return ToolResult::ok(result);
            }));
    write.examples = json::parse(R"json([
        {"title": "Read the Behaviour tab's preferences and turn one off", "calls": [
            {"tool": "preferences_get", "args": {"tab": "behaviour"}},
            {"tool": "preferences_set", "args": {"values": {"auto_arrange": false}}}]},
        {"title": "Set a choice and a number", "calls": [
            {"tool": "preferences_set", "args": {"values": {"auto_calculate_flush": "color change", "opengl_fps_cap": 60}}}]}])json");
    catalog.add(std::move(write));
}

}}} // namespace Slic3r::GUI::Mcp
