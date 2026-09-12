#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/NativeCommands.hpp"

#include <boost/filesystem.hpp>

#include <memory>
#include <set>
#include <string>
#include <type_traits>
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
    TestAppAction() : AppAction("test", "Action title", "src-key", "Action source") {}

    AppActionRunResult run(const std::string& param = {}) const override { return {}; }
};

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
// catalog, so a rename or a UI-language switch never re-keys the action.
TEST_CASE("Command action construction keys by catalog key", "[ActionSource][SpeedDial]")
{
    const std::vector<Slic3r::GUI::NativeCommand>& commands = Slic3r::GUI::NativeCommands::catalog();
    REQUIRE_FALSE(commands.empty());
    const Slic3r::GUI::NativeCommand& c = commands.front();

    std::unique_ptr<AppAction> action = Slic3r::GUI::NativeCommands::make_action(c);
    REQUIRE(action != nullptr);
    CHECK(action->id() == AppAction::compose_id("orca_command", c.key, "orca"));
    CHECK(action->id() != AppAction::compose_id("orca_command", c.title, "orca"));
    CHECK(action->title() == c.title);
    CHECK(action->group == c.group);
    CHECK(action->input == c.input);
    CHECK(action->icon == c.icon);
}

// The footer description/wiki link is settings-only: built-in commands leave both fields empty, so
// the palette's detail strip depends on list-level visibility for them.
TEST_CASE("Actions default to no description or wiki link", "[ActionSource][SpeedDial]")
{
    const TestAppAction action;
    CHECK(action.tooltip.empty());
    CHECK(action.help_url.empty());

    REQUIRE_FALSE(Slic3r::GUI::NativeCommands::catalog().empty());
    std::unique_ptr<AppAction> command = Slic3r::GUI::NativeCommands::make_action(Slic3r::GUI::NativeCommands::catalog().front());
    REQUIRE(command != nullptr);
    CHECK(command->tooltip.empty());
    CHECK(command->help_url.empty());
}

// Two-phase commands declare the input the palette must collect before they can run.
TEST_CASE("Two-phase commands declare their input phase", "[ActionSource][SpeedDial]")
{
    auto input_of = [](const std::string& key) -> std::string {
        for (const auto& c : Slic3r::GUI::NativeCommands::catalog())
            if (c.key == key)
                return c.input;
        return {};
    };
    CHECK(input_of("go_to_layer") == "percent");
    CHECK(input_of("go_to_tab") == "tab");
}

// The input token vocabulary is a JS<->C++ contract (speeddial.js dispatches "percent"/"tab").
// A typo here would leave a command that never enters its second phase, so pin the allowed set.
TEST_CASE("Command input tokens stay in the known vocabulary", "[ActionSource][SpeedDial]")
{
    for (const auto& c : Slic3r::GUI::NativeCommands::catalog()) {
        INFO(c.key << " input=" << c.input);
        CHECK((c.input.empty() || c.input == "percent" || c.input == "tab"));
    }
}

// The quick-launch cap must stay 10 to match the numbered Alt/Option+1..9,0 keys. The web palette
// mirrors it as K_FAV_LIMIT (asserted in speeddial.test.js); the C++ side pins it here.
static_assert(Slic3r::GUI::ActionRegistry::kFavLimit == 10, "kFavLimit must stay 10");

TEST_CASE("Favourite lists are capped and deduped preserving order", "[ActionSource][SpeedDial]")
{
    using Slic3r::GUI::cap_favourites;

    CHECK(cap_favourites({}, 10) == std::vector<std::string>{});
    CHECK(cap_favourites({"a", "b", "a"}, 10) == std::vector<std::string>{"a", "b"});
    CHECK(cap_favourites({"c", "a", "b", "c"}, 3) == std::vector<std::string>{"c", "a", "b"});
    CHECK(cap_favourites({"a", "b"}, 0) == std::vector<std::string>{});
}

TEST_CASE("Native command catalog has unique keys and present titles", "[ActionSource][SpeedDial]")
{
    const std::vector<Slic3r::GUI::NativeCommand>& commands = Slic3r::GUI::NativeCommands::catalog();
    CHECK_FALSE(commands.empty());

    std::set<std::string> seen;
    for (const auto& c : commands) {
        CHECK_FALSE(c.key.empty());
        CHECK_FALSE(c.title.empty());
        // A duplicated key would silently shadow the earlier command in the palette.
        CHECK(seen.insert(c.key).second);
    }
}

// Every command's tile pictogram is the SVG the matching GUI control already uses; an absent icon
// means a blank tile (like the tab picker). Guard representative names and that every non-empty
// value resolves to a shipped file, so a rename/typo cannot leave broken images in the palette.
TEST_CASE("Native command icons resolve to shipped SVGs", "[ActionSource][SpeedDial]")
{
    const std::vector<Slic3r::GUI::NativeCommand>& commands = Slic3r::GUI::NativeCommands::catalog();
    auto icon_of = [&commands](const std::string& key) -> const std::string* {
        for (const auto& c : commands)
            if (c.key == key)
                return &c.icon;
        return nullptr;
    };

    struct Expected
    {
        const char* key;
        const char* icon;
    };
    for (const Expected& e : {Expected{"load_project", "menu_open"},
                              Expected{"save_project", "menu_save"},
                              Expected{"sync_ams", "ams_fila_sync"},
                              Expected{"mode_simple", "advanced"},
                              Expected{"calib_temperature", "calib_sf"},
                              Expected{"plate_add", "toolbar_add_plate"},
                              Expected{"add_primitive_cube", "menu_obj_cube"},
                              Expected{"go_to_tab", ""}}) {
        const std::string* icon = icon_of(e.key);
        INFO(e.key);
        REQUIRE(icon != nullptr);
        CHECK(*icon == e.icon);
    }

    const boost::filesystem::path images = boost::filesystem::path(PROFILES_DIR).parent_path() / "images";
    for (const auto& c : commands) {
        if (c.icon.empty())
            continue;
        INFO(c.key << " -> " << c.icon);
        CHECK(boost::filesystem::exists(images / (c.icon + ".svg")));
    }
}

// The Help-menu commands, wiki/YouTube links and the developer-mode toggle are part of the palette.
// Guard their presence and that they stay grouped with their peers, so a catalog edit cannot drop
// or scatter them. Groups are compared to the peer's own group to stay independent of translation.
TEST_CASE("Native command catalog includes the Help and developer-mode commands", "[ActionSource][SpeedDial]")
{
    const std::vector<Slic3r::GUI::NativeCommand>& commands = Slic3r::GUI::NativeCommands::catalog();
    auto find = [&commands](const std::string& key) -> const Slic3r::GUI::NativeCommand* {
        for (const auto& c : commands)
            if (c.key == key)
                return &c;
        return nullptr;
    };

    const Slic3r::GUI::NativeCommand* first = find("help_keyboard_shortcuts");
    REQUIRE(first != nullptr);
    for (const char* key : {"help_setup_wizard", "help_open_config_folder", "help_troubleshoot", "help_network_test",
                            "help_tip_of_the_day", "help_check_updates", "help_about", "open_wiki", "open_youtube"}) {
        const Slic3r::GUI::NativeCommand* c = find(key);
        REQUIRE(c != nullptr);
        CHECK(c->group == first->group);
    }

    const Slic3r::GUI::NativeCommand* mode_simple = find("mode_simple");
    const Slic3r::GUI::NativeCommand* dev_mode    = find("toggle_developer_mode");
    REQUIRE(mode_simple != nullptr);
    REQUIRE(dev_mode != nullptr);
    CHECK(dev_mode->group == mode_simple->group);
}

// Every "Add Primitive" item and shipped handy model has a palette command, grouped as in the Add
// menu. Groups are compared to a peer's own group to stay independent of translation.
TEST_CASE("Native command catalog covers the Add menus", "[ActionSource][SpeedDial]")
{
    const std::vector<Slic3r::GUI::NativeCommand>& commands = Slic3r::GUI::NativeCommands::catalog();
    auto group_of = [&commands](const std::string& key) -> const std::string* {
        for (const auto& c : commands)
            if (c.key == key)
                return &c.group;
        return nullptr;
    };

    const std::string* primitive_group = group_of("add_primitive_cube");
    REQUIRE(primitive_group != nullptr);
    for (const char* key : {"add_primitive_cylinder", "add_primitive_sphere", "add_primitive_cone", "add_primitive_disc",
                            "add_primitive_torus", "add_primitive_text", "add_primitive_svg"}) {
        const std::string* group = group_of(key);
        INFO(key);
        REQUIRE(group != nullptr);
        CHECK(*group == *primitive_group);
    }

    const std::string* handy_group = group_of("add_handy_orca_cube");
    REQUIRE(handy_group != nullptr);
    for (const char* key : {"add_handy_orcasliced_combo", "add_handy_orca_badge", "add_handy_orca_tolerance_test",
                            "add_handy_3dbenchy", "add_handy_cali_cat", "add_handy_autodesk_fdm_test", "add_handy_voron_cube",
                            "add_handy_stanford_bunny", "add_handy_orca_string_hell"}) {
        const std::string* group = group_of(key);
        INFO(key);
        REQUIRE(group != nullptr);
        CHECK(*group == *handy_group);
    }
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
