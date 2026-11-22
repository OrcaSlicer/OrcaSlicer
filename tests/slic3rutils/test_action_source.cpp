#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_message.hpp>
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/GUI_Factories.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/OptionsGroup.hpp"
#include "slic3r/GUI/SettingsIndex.hpp"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <wx/string.h>

#include "libslic3r/Config.hpp"
#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using Slic3r::GUI::AppAction;
using Slic3r::GUI::AppActionRunResult;
using Slic3r::GUI::ActionRegistry;

namespace {

// AppAction is abstract; this minimal concrete action lets the tests exercise its
// constructor-composed identity without involving a plugin runner.
class TestAppAction final : public AppAction
{
public:
    explicit TestAppAction(std::string title = "Action title", std::string source_name = "Action source")
        : AppAction("test", std::move(title), "src-key", std::move(source_name))
    {}

    AppActionRunResult run(const nlohmann::json& /*args*/) const override { return {}; }
};

// The AppAction the registry makes of the Speed Dial action with key `key`, a command's own or an item of its actions;
// null when no command of the process catalog has that action.
std::unique_ptr<AppAction> speed_dial_action(const std::string& key)
{
    const auto [command, action] = Slic3r::GUI::find_command_action(Slic3r::GUI::NativeCommands::instance(), key);
    if (command == nullptr)
        return nullptr;
    return action != nullptr ? Slic3r::GUI::NativeCommands::make_action(*command, *action) :
                               Slic3r::GUI::NativeCommands::make_action(*command);
}

} // namespace

TEST_CASE("AppAction composes a stable id from prefix:title:source_key", "[ActionSource][SpeedDial]")
{
    CHECK(AppAction::compose_id("test", "Action title", "src-key") == "test:Action title:src-key");
    // source_key (not the display name) carries identity, so it is the third field.
    CHECK(AppAction::compose_id("script", "Do Thing", "pack.py") == "script:Do Thing:pack.py");
}

TEST_CASE("AppAction definitions are immutable after construction", "[ActionSource][SpeedDial]")
{
    using StringAccessor = const std::string& (AppAction::*) () const;

    STATIC_CHECK(std::is_same_v<decltype(&AppAction::id), StringAccessor>);
    STATIC_CHECK(std::is_same_v<decltype(&AppAction::title), StringAccessor>);
    STATIC_CHECK(std::is_same_v<decltype(&AppAction::source_key), StringAccessor>);
    STATIC_CHECK(std::is_same_v<decltype(&AppAction::source_name), StringAccessor>);

    const TestAppAction action;
    CHECK(action.id() == "test:Action title:src-key");
    CHECK(action.title() == "Action title");
    CHECK(action.source_key() == "src-key");
    CHECK(action.source_name() == "Action source");
}

TEST_CASE("ActionRegistry takes exclusive ownership of published actions", "[ActionSource][SpeedDial]")
{
    using ExpectedUpsert = void (ActionRegistry::*)(std::unique_ptr<AppAction>);

    STATIC_CHECK(std::is_same_v<decltype(&ActionRegistry::upsert), ExpectedUpsert>);
}

// A dynamic "Go to Plate N" action is keyed by plate index (not the display title), so renaming
// a plate never re-keys it - the same contract as a setting action.
TEST_CASE("Go-to-plate actions are keyed by index, not title", "[ActionSource][SpeedDial]")
{
    CHECK(AppAction::compose_id("orca_plate_goto", "0", "orca") == "orca_plate_goto:0:orca");
    CHECK(AppAction::compose_id("orca_plate_goto", "2", "orca") == "orca_plate_goto:2:orca");
}

// A dynamic "Open recent project" action is keyed by file path (not the display name), so renaming
// a project or reordering the recents list never re-keys it - the same contract as a setting action.
TEST_CASE("Recent-project actions are keyed by path, not title", "[ActionSource][SpeedDial]")
{
    CHECK(AppAction::compose_id("orca_recent_project", "/a/b/project.3mf", "orca") ==
          "orca_recent_project:/a/b/project.3mf:orca");
    CHECK(AppAction::compose_id("orca_recent_project", "C:/Data/cube.3mf", "orca") ==
          "orca_recent_project:C:/Data/cube.3mf:orca");
}

// A built-in command is keyed by its stable catalog key (not the localized display title), so a
// rename or a UI-language switch never re-keys the action and its persisted favourite/stats survive.
TEST_CASE("Command actions are keyed by catalog key, not display title", "[ActionSource][SpeedDial]")
{
    CHECK(AppAction::compose_id("orca_command", "save_project", "orca") == "orca_command:save_project:orca");
    // The second field is the stable key, so distinct commands never collide.
    CHECK(AppAction::compose_id("orca_command", "save_project", "orca") !=
          AppAction::compose_id("orca_command", "load_project", "orca"));
}

// The real catalog -> action mapping keys by the stable catalog key and copies presentation from the
// catalog, so a rename or a UI-language switch never re-keys the action. The catalog's English title
// and group become the action's English search aliases; no translation catalog is loaded in the tests,
// so the displayed title and group equal them.
TEST_CASE("Command action construction keys by catalog key", "[ActionSource][SpeedDial]")
{
    const std::vector<Slic3r::GUI::NativeCommand>& commands = Slic3r::GUI::NativeCommands::instance().commands();
    REQUIRE_FALSE(commands.empty());
    const Slic3r::GUI::NativeCommand& c = commands.front();

    std::unique_ptr<AppAction> action = Slic3r::GUI::NativeCommands::make_action(c);
    REQUIRE(action != nullptr);
    CHECK(action->id() == AppAction::compose_id("orca_command", c.key, "orca"));
    CHECK(action->id() != AppAction::compose_id("orca_command", c.title, "orca"));
    CHECK(action->title() == c.title);
    CHECK(action->group == c.group);
    CHECK(action->title_en == c.title);
    CHECK(action->group_en == c.group);
    CHECK(action->input == c.input);
    CHECK(action->icon == c.icon);
}

// The footer description/wiki link is settings-only: built-in commands leave both fields empty, so
// the Speed Dial's detail strip depends on list-level visibility for them.
TEST_CASE("Actions default to no description or wiki link", "[ActionSource][SpeedDial]")
{
    const TestAppAction action;
    CHECK(action.tooltip.empty());
    CHECK(action.help_url.empty());

    const std::vector<Slic3r::GUI::NativeCommand>& commands = Slic3r::GUI::NativeCommands::instance().commands();
    REQUIRE_FALSE(commands.empty());
    std::unique_ptr<AppAction> command = Slic3r::GUI::NativeCommands::make_action(commands.front());
    REQUIRE(command != nullptr);
    CHECK(command->tooltip.empty());
    CHECK(command->help_url.empty());
}

// Two-phase actions declare the input the Speed Dial must collect before they can run.
TEST_CASE("Two-phase commands declare their input phase", "[ActionSource][SpeedDial]")
{
    const std::unique_ptr<AppAction> layer = speed_dial_action("go_to_layer");
    const std::unique_ptr<AppAction> tab   = speed_dial_action("go_to_tab");
    REQUIRE(layer != nullptr);
    REQUIRE(tab != nullptr);
    CHECK(layer->input == "percent");
    CHECK(tab->input == "tab");
}

// The input token vocabulary is a JS<->C++ contract (speeddial.js dispatches "percent"/"tab").
// A typo here would leave an action that never enters its second phase, so pin the allowed set.
TEST_CASE("Command input tokens stay in the known vocabulary", "[ActionSource][SpeedDial]")
{
    for (const Slic3r::GUI::NativeCommands::SpeedDialAction& t :
         Slic3r::GUI::NativeCommands::speed_dial_actions(Slic3r::GUI::NativeCommands::instance())) {
        const std::string key   = t.action != nullptr ? t.action->at("key").get<std::string>() : t.command->key;
        const std::string input = t.action != nullptr ? t.action->value("input", std::string()) : t.command->input;
        INFO(key << " input=" << input);
        CHECK((input.empty() || input == "percent" || input == "tab"));
    }
}

// The quick-launch cap must stay 10 to match the numbered Alt/Option+1..9,0 keys. The Speed Dial's
// page mirrors it as K_FAV_LIMIT (asserted in speeddial.test.js); the C++ side pins it here.
static_assert(Slic3r::GUI::ActionRegistry::kFavLimit == 10, "kFavLimit must stay 10");

TEST_CASE("Favourite lists are capped and deduped preserving order", "[ActionSource][SpeedDial]")
{
    using Slic3r::GUI::cap_favourites;

    CHECK(cap_favourites({}, 10) == std::vector<std::string>{});
    CHECK(cap_favourites({"a", "b", "a"}, 10) == std::vector<std::string>{"a", "b"});
    CHECK(cap_favourites({"c", "a", "b", "c"}, 3) == std::vector<std::string>{"c", "a", "b"});
    CHECK(cap_favourites({"a", "b"}, 0) == std::vector<std::string>{});
}

TEST_CASE("A pin with no live action is dropped, the others keep their order", "[ActionSource][SpeedDial]")
{
    std::vector<std::string> stored, shown;
    const auto live = [](const std::string& id) { return id != "gone"; };
    Slic3r::GUI::prune_favourites({"b", "gone", "a"}, live, stored, shown);
    CHECK(stored == std::vector<std::string>{"b", "a"});
    CHECK(shown == std::vector<std::string>{"b", "a"});
#ifndef SLIC3R_CAD
    // Without the Design table, a Design tab action's pin cannot be told from a removed command's.
    CHECK_FALSE(Slic3r::GUI::is_design_action_id("orca_command:shell:orca"));
#endif
}

TEST_CASE("A pin under a menu item's id is kept, as its action's", "[ActionSource][SpeedDial]")
{
    // The ids the registry holds of the Speed Dial's actions, which it makes as the Speed Dial starts.
    std::set<std::string> held;
    for (const Slic3r::GUI::NativeCommands::SpeedDialAction& t :
         Slic3r::GUI::NativeCommands::speed_dial_actions(Slic3r::GUI::NativeCommands::instance()))
        held.insert((t.action != nullptr ? Slic3r::GUI::NativeCommands::make_action(*t.command, *t.action) :
                                           Slic3r::GUI::NativeCommands::make_action(*t.command))
                        ->id());
    const auto live = [&held](const std::string& id) { return held.count(id) != 0; };
    // Delete Selected and New Project, whose menu items are actions of object_delete and project_new.
    const std::vector<std::string> pinned{"orca_command:obj_delete:orca", "orca_command:new_project:orca"};
    std::vector<std::string>       stored, shown;
    Slic3r::GUI::prune_favourites(pinned, live, stored, shown);
    CHECK(stored == pinned);
    CHECK(shown == pinned);
}

namespace {

// `n` pins, pin1 to pin<n>.
std::vector<std::string> pins(int n)
{
    std::vector<std::string> out;
    for (int i = 1; i <= n; ++i)
        out.push_back("pin" + std::to_string(i));
    return out;
}

} // namespace

TEST_CASE("The favourites bar shows the first ten live pins and keeps the rest", "[ActionSource][SpeedDial]")
{
    const auto                     live   = [](const std::string&) { return true; };
    const std::vector<std::string> twelve = pins(12);
    std::vector<std::string>       stored, shown;
    Slic3r::GUI::prune_favourites(twelve, live, stored, shown);
    CHECK(stored == twelve);
    CHECK(shown == pins(10));
    // A pin past the tenth comes onto the bar once one before it is unpinned.
    std::vector<std::string> unpinned = twelve;
    unpinned.erase(unpinned.begin());
    Slic3r::GUI::prune_favourites(unpinned, live, stored, shown);
    CHECK(shown.back() == "pin11");
}

TEST_CASE("A pin is refused while the bar shows ten", "[ActionSource][SpeedDial]")
{
    const auto               live = [](const std::string&) { return true; };
    std::vector<std::string> favs = pins(10);
    CHECK_FALSE(Slic3r::GUI::add_favourite(favs, "pin11", live));
    CHECK(favs == pins(10));
    // A pin already on the bar stays where it is.
    CHECK(Slic3r::GUI::add_favourite(favs, "pin3", live));
    CHECK(favs == pins(10));
    // A stored pin past the tenth is not on the bar, which is full, so it is refused as a new one is.
    favs = pins(12);
    CHECK_FALSE(Slic3r::GUI::add_favourite(favs, "pin12", live));
    CHECK(favs == pins(12));
}

// Ordinary use stores at most thirty pins: ten for each set of live actions the bar is shown with.
static_assert(ActionRegistry::kFavStoredLimit > 3 * ActionRegistry::kFavLimit, "kFavStoredLimit must stay out of ordinary use's reach");

TEST_CASE("The stored pins stay within their bound however few the bar shows", "[ActionSource][SpeedDial]")
{
    const auto               none_live = [](const std::string&) { return false; };
    std::vector<std::string> favs      = pins(int(ActionRegistry::kFavStoredLimit));
    CHECK_FALSE(Slic3r::GUI::add_favourite(favs, "one_more", none_live));
    CHECK(favs.size() == ActionRegistry::kFavStoredLimit);
}

TEST_CASE("Reordering the bar moves only the pins it shows, each other pin keeping its place", "[ActionSource][SpeedDial]")
{
    // The bar shows a, b and c; hidden1 and hidden2 are pins it does not show. The page moves b to the front and
    // sends the pins it shows.
    CHECK(Slic3r::GUI::reordered_favourites({"a", "hidden1", "b", "c", "hidden2"}, {"b", "a", "c"}) ==
          std::vector<std::string>{"b", "hidden1", "a", "c", "hidden2"});
    // An id that is no pin, and one sent twice, move nothing else.
    CHECK(Slic3r::GUI::reordered_favourites({"a", "hidden1", "b"}, {"gone", "b", "b", "a"}) ==
          std::vector<std::string>{"b", "hidden1", "a"});
}

TEST_CASE("Native command catalog has unique keys and present titles", "[ActionSource][SpeedDial]")
{
    const std::vector<Slic3r::GUI::NativeCommand>& commands = Slic3r::GUI::NativeCommands::instance().commands();
    CHECK_FALSE(commands.empty());

    std::set<std::string> seen;
    for (const auto& c : commands) {
        INFO(c.key);
        CHECK_FALSE(c.key.empty());
        CHECK_FALSE(c.title.empty());
        // A command's group is what tool_list filters by: its place's heading, or Design.
        CHECK_FALSE(c.group.empty());
        // A duplicated key would silently shadow the earlier command in the Speed Dial.
        CHECK(seen.insert(c.key).second);
    }
}

// Every command's tile pictogram is a theme-neutral SVG (the matching GUI control's icon, or the
// equivalent settings-group icon); an absent icon gets the page's generic placeholder. Guard
// representative names and that every non-empty value resolves to a shipped file, so a rename/typo
// cannot leave broken images in the Speed Dial.
TEST_CASE("Native command icons resolve to shipped SVGs", "[ActionSource][SpeedDial]")
{
    struct Expected
    {
        const char* key;
        const char* icon;
    };
    for (const Expected& e : {Expected{"load_project", "menu_open"},
                              Expected{"save_project", "menu_save"},
                              Expected{"sync_ams", "ams_fila_sync"},
                              Expected{"mode_simple", "advanced"},
                              Expected{"calib_temperature", "param_temperature"},
                              Expected{"calib_cornering", "param_precision"},
                              Expected{"plate_add", "toolbar_add_plate"},
                              Expected{"add_primitive_cube", "menu_obj_cube"},
                              // These previously pointed at blank placeholder SVGs or theme-broken ones.
                              Expected{"obj_delete", "delete"},
                              Expected{"export_gcode", "custom-gcode_gcode"},
                              Expected{"import_file", "menu_open"},
                              Expected{"help_open_config_folder", "open_project"},
                              Expected{"help_check_updates", "refresh"},
                              Expected{"help_about", "OrcaSlicer_gradient_circle"},
                              Expected{"go_to_tab", ""}}) {
        INFO(e.key);
        const std::unique_ptr<AppAction> action = speed_dial_action(e.key);
        REQUIRE(action != nullptr);
        CHECK(action->icon == e.icon);
    }

    // Every action's icon, a command's own or an item's.
    const boost::filesystem::path images = boost::filesystem::path(PROFILES_DIR).parent_path() / "images";
    for (const Slic3r::GUI::NativeCommands::SpeedDialAction& t :
         Slic3r::GUI::NativeCommands::speed_dial_actions(Slic3r::GUI::NativeCommands::instance())) {
        const std::string key  = t.action != nullptr ? t.action->at("key").get<std::string>() : t.command->key;
        const std::string icon = t.action != nullptr ? t.action->at("icon").get<std::string>() : t.command->icon;
        if (icon.empty())
            continue;
        INFO(key << " -> " << icon);
        CHECK(boost::filesystem::exists(images / (icon + ".svg")));
    }
}

// The Design tab's actions are the registry's to show while the tab is shown (materialize_design_actions()),
// so the Speed Dial's own actions, which it holds whatever tab is shown, leave the Design group out. A command of any other
// group, listed as one that runs from its arguments, has its actions there.
TEST_CASE("the Speed Dial's own actions leave the Design group out", "[ActionSource][SpeedDial]")
{
    Slic3r::GUI::CommandCatalog catalog;
    for (const char* group : {"Design", "Objects"}) {
        Slic3r::GUI::NativeCommand c;
        c.key            = std::string(group) + "_action";
        c.title          = group;
        c.group          = group;
        c.listed         = c.group != Slic3r::GUI::kDesignGroup;
        c.has_own_action = true;
        c.actions        = nlohmann::json::array({nlohmann::json{{"key", std::string(group) + "_item"}, {"title", group}, {"icon", ""},
                                                                 {"description", ""}, {"args", {{"item", true}}}}});
        catalog.add(std::move(c));
    }
    catalog.seal();
    const std::vector<Slic3r::GUI::NativeCommands::SpeedDialAction> speed_dial = Slic3r::GUI::NativeCommands::speed_dial_actions(catalog);
    REQUIRE(speed_dial.size() == 2);
    CHECK(speed_dial[0].command->key == "Objects_action");
    CHECK(speed_dial[0].action == nullptr);
    REQUIRE(speed_dial[1].action != nullptr);
    CHECK(speed_dial[1].action->at("key") == "Objects_item");
}

// The Help-menu actions, wiki/YouTube links and the developer-mode toggle are part of the Speed Dial.
// Guard their presence and that they stay grouped with their peers, so a catalog edit cannot drop
// or scatter them. Groups are compared to the peer's own group to stay independent of translation.
TEST_CASE("Native command catalog includes the Help and developer-mode commands", "[ActionSource][SpeedDial]")
{
    const std::unique_ptr<AppAction> first = speed_dial_action("help_keyboard_shortcuts");
    REQUIRE(first != nullptr);
    for (const char* key : {"help_setup_wizard", "help_open_config_folder", "help_migrate_filament_presets", "help_troubleshoot", "help_network_test",
                            "help_benchmark_3d_scene", "help_tip_of_the_day", "help_check_updates", "help_about", "open_wiki",
                            "open_youtube"}) {
        INFO(key);
        const std::unique_ptr<AppAction> action = speed_dial_action(key);
        REQUIRE(action != nullptr);
        CHECK(action->group_en == first->group_en);
    }

    const std::unique_ptr<AppAction> mode_simple = speed_dial_action("mode_simple");
    const std::unique_ptr<AppAction> dev_mode    = speed_dial_action("toggle_developer_mode");
    REQUIRE(mode_simple != nullptr);
    REQUIRE(dev_mode != nullptr);
    CHECK(dev_mode->group_en == mode_simple->group_en);
}

// A person takes screenshots and records from the Speed Dial, beside Run Script, with no agent involved. Start and
// Stop Recording are actions of the typed tools, whose arguments skip what the action does; Take Screenshot's command has
// no argument form.
TEST_CASE("Native command catalog has the screenshot and recording commands", "[ActionSource][SpeedDial]")
{
    const std::unique_ptr<AppAction> runner = speed_dial_action("open_action_replay");
    REQUIRE(runner != nullptr);
    for (const auto& [key, name] : {std::make_pair("take_screenshot", "window_save_screenshot"),
                                    std::make_pair("start_recording", "recording_start"), std::make_pair("stop_recording", "recording_stop")}) {
        INFO(key);
        const std::unique_ptr<AppAction> action = speed_dial_action(key);
        REQUIRE(action != nullptr);
        CHECK(action->group_en == runner->group_en);
        CHECK(action->input.empty());
        CHECK(action->command == name);
        const Slic3r::GUI::NativeCommand* c = Slic3r::GUI::NativeCommands::find(name);
        REQUIRE(c != nullptr);
        CHECK(c->input_schema.at("properties").empty() == (std::string(key) == "take_screenshot"));
        CHECK_FALSE(c->description.empty());
    }
}

// The Run Script panel's action sits beside Action Replay's, which keeps its id so existing replays and agent sessions
// that open it still do.
TEST_CASE("Native command catalog has the Run Script panel and Action Replay commands", "[ActionSource][SpeedDial]")
{
    const std::unique_ptr<AppAction> replay = speed_dial_action("open_action_replay");
    REQUIRE(replay != nullptr);
    CHECK(replay->title_en == "Action Replay");
    const std::unique_ptr<AppAction> panel = speed_dial_action("toggle_run_script_panel");
    REQUIRE(panel != nullptr);
    CHECK(panel->group_en == replay->group_en);
    CHECK(panel->input.empty());
    const Slic3r::GUI::NativeCommand* command = Slic3r::GUI::NativeCommands::find(panel->command);
    REQUIRE(command != nullptr);
    CHECK(command->key == "script_toggle_panel");
    CHECK(command->input_schema.at("properties").empty());
    CHECK_FALSE(command->description.empty());
}

// Every "Add Primitive" item and shipped handy model has an action in the Speed Dial, grouped as in the
// Add menu. Groups are compared to a peer's own group to stay independent of translation.
TEST_CASE("Native command catalog covers the Add menus", "[ActionSource][SpeedDial]")
{
    const std::unique_ptr<AppAction> primitive = speed_dial_action("add_primitive_cube");
    REQUIRE(primitive != nullptr);
    for (const char* key : {"add_primitive_cylinder", "add_primitive_sphere", "add_primitive_cone", "add_primitive_disc",
                            "add_primitive_torus", "add_primitive_text", "add_primitive_svg"}) {
        INFO(key);
        const std::unique_ptr<AppAction> action = speed_dial_action(key);
        REQUIRE(action != nullptr);
        CHECK(action->group_en == primitive->group_en);
    }

    const std::unique_ptr<AppAction> handy = speed_dial_action("add_handy_orca_cube");
    REQUIRE(handy != nullptr);
    for (const char* key : {"add_handy_orcasliced_combo", "add_handy_orca_badge", "add_handy_orca_tolerance_test",
                            "add_handy_3dbenchy", "add_handy_cali_cat", "add_handy_autodesk_fdm_test", "add_handy_voron_cube",
                            "add_handy_stanford_bunny", "add_handy_orca_string_hell"}) {
        INFO(key);
        const std::unique_ptr<AppAction> action = speed_dial_action(key);
        REQUIRE(action != nullptr);
        CHECK(action->group_en == handy->group_en);
    }

    // A handy model's action is titled by the English label its Add-menu entry is built from, so it is
    // searchable in English in any UI language.
    for (const auto& model : Slic3r::GUI::MenuFactory::handy_models()) {
        const std::string key = "add_handy_" + std::string(model.key);
        INFO(key);
        const std::unique_ptr<AppAction> action = speed_dial_action(key);
        REQUIRE(action != nullptr);
        CHECK(action->title_en == model.label);
    }
}

// A setting action is named like its settings row, not the ConfigOptionDef label: the row's
// Line::label, plus the field leaf when the row packs several options.
TEST_CASE("Setting display labels mirror the settings row", "[ActionSource][SpeedDial]")
{
    using Slic3r::Search::compose_display_label;

    // Single-option row: the row label is the whole title.
    CHECK(compose_display_label(L"Reverse on even", L"Reverse on even", false) == L"Reverse on even");
    // No recorded row label falls back to the field leaf.
    CHECK(compose_display_label(L"", L"Outer wall", false) == L"Outer wall");
    // Multi-option row: qualify with the leaf so the plate-temperature fields are distinct.
    CHECK(compose_display_label(L"Cool Plate", L"First layer", true) == wxString(L"Cool Plate \u2013 First layer"));
    CHECK(compose_display_label(L"Cool Plate", L"Other layers", true) == wxString(L"Cool Plate \u2013 Other layers"));
    // A leaf equal to the row label is not repeated.
    CHECK(compose_display_label(L"Skirt loops", L"Skirt loops", true) == L"Skirt loops");

    using Slic3r::Search::resolve_setting_title;

    // A single-option row's live label wins, so a runtime rename is reflected.
    CHECK(resolve_setting_title(L"Brim width", L"Brim ear radius", false) == L"Brim ear radius");
    // Multi-option rows keep their precomposed "row – field" label (the leaf disambiguates them).
    CHECK(resolve_setting_title(L"Cool Plate \u2013 First layer", L"Cool Plate", true) ==
          wxString(L"Cool Plate \u2013 First layer"));
    // No live row label (option not on a built page) keeps the precomposed label.
    CHECK(resolve_setting_title(L"Reverse on even", L"", false) == L"Reverse on even");
    // Neither present: empty, so the caller falls back to the descriptive label.
    CHECK(resolve_setting_title(L"", L"", false).IsEmpty());
}

// The page receives an English alias only where it differs from the displayed text, so an English UI
// (where every alias equals its text) receives none and searches exactly the displayed text.
TEST_CASE("Action JSON sends English aliases only when they differ from the displayed text", "[ActionSource][SpeedDial]")
{
    const char* aliases[] = {"title_en", "group_en", "source_en", "full_label_en"};

    TestAppAction english("Reverse on even", "Process : Quality : Overhangs");
    english.full_label    = "Overhang reversal";
    english.title_en      = english.title();
    english.source_en     = english.source_name();
    english.full_label_en = english.full_label;
    const nlohmann::json same = Slic3r::GUI::action_json(english);
    for (const char* key : aliases) {
        INFO(key);
        CHECK_FALSE(same.contains(key));
    }
    CHECK(same.at("title") == "Reverse on even");

    // A translated action (German stands in for any UI language) carries each differing English name.
    TestAppAction localized("Bei geraden Schichten umkehren", "Prozess : Qualitaet : Ueberhaenge");
    localized.group         = "Befehle";
    localized.full_label    = "Ueberhangumkehr";
    localized.title_en      = "Reverse on even";
    localized.group_en      = "Commands";
    localized.source_en     = "Process : Quality : Overhangs";
    localized.full_label_en = "Overhang reversal";
    const nlohmann::json differs = Slic3r::GUI::action_json(localized);
    CHECK(differs.at("title_en") == "Reverse on even");
    CHECK(differs.at("group_en") == "Commands");
    CHECK(differs.at("source_en") == "Process : Quality : Overhangs");
    CHECK(differs.at("full_label_en") == "Overhang reversal");
    CHECK(differs.at("title") == "Bei geraden Schichten umkehren");

    // No English counterpart (a plugin, a file name) sends nothing.
    const nlohmann::json none = Slic3r::GUI::action_json(TestAppAction("Optimize", "Gcode Optimizer"));
    for (const char* key : aliases) {
        INFO(key);
        CHECK_FALSE(none.contains(key));
    }
}

// A settings row keeps the untranslated English of its label, from the ctor and from a runtime
// rename, so the Speed Dial can offer the row's English name in any UI language. No catalog is loaded
// in the tests, so the displayed label equals the English.
TEST_CASE("Settings rows keep the English of their label", "[ActionSource][SpeedDial]")
{
    Slic3r::GUI::Line line{L("Cool Plate"), ""};
    CHECK(line.label_en == "Cool Plate");
    CHECK(line.label == "Cool Plate");

    line.set_label(L("Brim ear radius"));
    CHECK(line.label_en == "Brim ear radius");
    CHECK(line.label == "Brim ear radius");
}

// A setting whose mode is above the user's current mode must be prompted before it can be edited.
// Developer settings (comDevelop) are above every non-developer mode, so they always prompt then.
TEST_CASE("Settings above the current mode require a switch", "[ActionSource][SpeedDial]")
{
    using Slic3r::GUI::requires_mode_switch;
    using Slic3r::comAdvanced;
    using Slic3r::comDevelop;
    using Slic3r::comExpert;
    using Slic3r::comSimple;

    CHECK(requires_mode_switch(comAdvanced, comSimple));
    CHECK(requires_mode_switch(comExpert, comSimple));
    CHECK(requires_mode_switch(comExpert, comAdvanced));
    CHECK(requires_mode_switch(comDevelop, comSimple));
    CHECK(requires_mode_switch(comDevelop, comAdvanced));
    CHECK(requires_mode_switch(comDevelop, comExpert));

    CHECK_FALSE(requires_mode_switch(comSimple, comSimple));
    CHECK_FALSE(requires_mode_switch(comSimple, comAdvanced));
    CHECK_FALSE(requires_mode_switch(comAdvanced, comAdvanced));
    CHECK_FALSE(requires_mode_switch(comAdvanced, comExpert));
    CHECK_FALSE(requires_mode_switch(comExpert, comExpert));
    CHECK_FALSE(requires_mode_switch(comDevelop, comDevelop));
}
