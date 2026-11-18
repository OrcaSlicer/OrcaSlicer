#pragma once

#include "Outcome.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The app's preferences as the Preferences dialog shows them: one table of the rows whose control is named after the
// key it stores in the app configuration's `app` section. The dialog builds those rows from it, preferences_get lists
// it and preferences_set writes through it, so the three cannot differ.
namespace Slic3r { namespace GUI { namespace Ops {

enum class PreferenceKind { Checkbox, Choice, Number, Text };

struct Preference
{
    std::string    key;
    std::string    tab;     // general, files, behaviour, control, graphics, online or developer
    std::string    section; // the title of the row's group, an English msgid
    std::string    label;   // the row's title, an English msgid
    PreferenceKind kind = PreferenceKind::Checkbox;
    // A choice's entries in the dialog's order: the value stored for it and its label, an English msgid, a symbol such as
    // "4x" that is shown as it is, or for a row of its own the English of what its function shows. Empty for a choice
    // whose entries the running app lists.
    std::vector<std::pair<std::string, std::string>> choices;
    std::optional<double> min, max; // a number's range; a bound the dialog does not set is left out
    std::string           fallback; // what the dialog shows where nothing is stored
    bool folder = false; // a text that is the absolute path of an existing folder, which the dialog picks in a folder dialog
    const char*           refused = nullptr; // why a call may not write it, an English msgid whose %1% is the key; null: it may
    // The value is read at the next start, so a write is stored at once and changes nothing before then: the rows the
    // dialog marks "(Requires restart)", in their note or their tooltip.
    bool restart_needed = false;

    // How the dialog builds the row.
    std::vector<std::string> tooltip; // English msgids, shown one per line; none: the label
    std::string              note;    // a msgid shown after the control, "(Requires restart)"
    std::string unit; // a number's unit: a msgid after a spin control, or a symbol in a decimal field, shown as it is
    std::string wiki; // the label's link into the wiki, under https://www.orcaslicer.com/wiki/
    int         decimals = 0; // a number's decimals; 0: a whole number, in a spin control unless the row is its own
    // The dialog builds the row with a function of its own, by its key, as its control has handlers or labels the
    // plain rows do not; a row that function builds with another key's (a checkbox and its field) has one too.
    bool own_row = false;
    // Whether the dialog shows the row in this run, where that depends on the app (the CAD feature, the Microsoft Store
    // package); null: always. Called on the UI thread.
    bool (*shown)() = nullptr;
};

// Every preference the Preferences dialog can show, in its order, tab by tab.
const std::vector<Preference>& preferences();

// The preferences the dialog shows in this run: preferences() less the rows whose `shown` says no. UI thread.
std::vector<Preference> shown_preferences();

// `key`'s entry in `table`, or null.
const Preference* find_preference(const std::vector<Preference>& table, const std::string& key);

// What `p` stores for `value` as its control stores it: "true" or "false" for a checkbox ("1" or "0" where its fallback
// is "0"), a choice's value, a whole number, or a number with the row's decimals. `value` is one check_preferences()
// takes.
std::string preference_text(const Preference& p, const nlohmann::json& value);

// The value `stored` stands for in `p`'s kind: true or false for a checkbox, a number for a number field (the text as it
// is where it is no number), the text for the others.
nlohmann::json preference_value(const Preference& p, const std::string& stored);

// Why `values` ({key: value}) cannot be written to `table`'s preferences, checked key by key in the keys' order (the
// map's), the first refusal winning, or none:
// not_a_preference (a key `table` lacks, with the closest keys), preference_refused (the reason the table gives,
// with the key), not_a_boolean, no_such_choice (with the choice's values), not_a_number, not_a_whole_number,
// out_of_range (with the range), not_a_text and not_a_folder (a folder's text that is no existing folder's absolute path).
std::optional<Refusal> check_preferences(const std::vector<Preference>& table, const std::map<std::string, nlohmann::json>& values);

// A preference a call wrote: its text before and after, as stored. Where nothing was stored before, `before` is the
// fallback the dialog showed and `stored_before` is false.
struct PreferenceChange
{
    std::string key, before, after;
    bool        stored_before = true;
};
struct PreferenceChanges
{
    std::vector<PreferenceChange> changed;   // in the keys' order
    std::vector<std::string>      unchanged; // keys whose stored value was already the one asked for
};

// Writes `values` ({key: value}) to the preferences the dialog shows, as their controls do: every key and value is
// checked first (check_preferences() over shown_preferences()), so a refused call changes nothing; then the changed
// ones are written together, the app configuration is saved, and each changed key's effect runs through
// apply_preference(), in the keys' order, followed by one redraw of the 3D view where a key it draws with changed. A
// key whose stored value is already the one asked for is left as it is and runs nothing. Not in the undo history. UI
// thread.
// Refuses: what check_preferences() refuses.
Outcome<PreferenceChanges> set_preferences(const std::map<std::string, nlohmann::json>& values);

// What changing `key` does in the app beyond storing it, as its control in the Preferences dialog does it, with the new
// value read from the app configuration: the preview's look, the sidebar's preset lists and height, the mode the
// settings show, the backup timer, the log level, the theme, the preset sync started or stopped, stealth mode's sign-out
// of the cloud accounts, the downloads folder an open Preferences dialog shows. The
// dialog's controls and set_preferences() both call it once the value is stored; the dialog's own controls that follow a
// value (a field enabled with its checkbox) stay the dialog's. A key with no effect does nothing. UI thread.
void apply_preference(const std::string& key);

// The preferences the 3D view reads every frame: a change takes one redraw to show, which the Preferences dialog's
// closing and set_preferences() ask for.
const std::vector<std::string>& canvas_preferences();

// The MCP server's switch as the Preferences dialog shows it: on while the server runs, also where --mcp started it for
// this session only, else as stored. UI thread.
bool mcp_switch_shown();

// The MCP server's port as the dialog shows it: the running server's, otherwise the one turning it on opens
// (GUI_App::open_mcp_endpoint()): --mcp-port until a port is set in Preferences, then the saved one. UI thread.
int mcp_port_shown();

// What the dialog shows, as preference_text() writes it, for each preference whose control does not show the stored
// value: mcp_enabled (mcp_switch_shown()) and mcp_port (mcp_port_shown()). preferences_get reads these. UI thread.
std::map<std::string, std::string> shown_texts();

}}} // namespace Slic3r::GUI::Ops
