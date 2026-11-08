// Automation action tests for the printer family (WEB_APP_BRIDGE.md 4.6). Tag: [actions].
//
// The pure rules behind slicer.printerControl / slicer.print (Actions/PrinterRules.hpp), each
// mirroring a Device tab or print dialog check, and the printer descriptors. No running
// application or printer is needed.

#include <catch2/catch_all.hpp>

#include "slic3r/GUI/Actions/ActionCatalog.hpp"
#include "slic3r/GUI/Actions/ActionSchema.hpp"
#include "slic3r/GUI/Actions/PrinterRules.hpp"
#include "slic3r/GUI/Actions/Register.hpp"

#include <set>
#include <string>

using namespace Slic3r::GUI::Actions;
namespace Rules = Slic3r::GUI::Actions::PrinterRules;
using nlohmann::json;

TEST_CASE("Load temperatures are the tray range midpoint", "[actions]")
{
    CHECK(Rules::tray_mid_temp("190", "230") == 210);
    CHECK(Rules::tray_mid_temp("", "230") == -1);
    CHECK(Rules::tray_mid_temp("190", "") == -1);
}

TEST_CASE("Fan percent moves in tenths, per protocol", "[actions]")
{
    CHECK(Rules::fan_step(0) == 0);
    CHECK(Rules::fan_step(44) == 4);
    CHECK(Rules::fan_step(46) == 5);
    CHECK(Rules::fan_step(100) == 10);
    CHECK(Rules::fan_value_old_protocol(10) == 255);
    CHECK(Rules::fan_value_old_protocol(5) == 127);
    CHECK(Rules::fan_value_new_protocol(5) == 50);
}

TEST_CASE("Filament load and unload refuse where the Device tab does", "[actions]")
{
    Rules::FilamentFacts idle;
    CHECK(Rules::filament_refusal(idle, true).empty());
    CHECK(Rules::filament_refusal(idle, false).empty());

    Rules::FilamentFacts printing = idle;
    printing.printing             = true;
    CHECK_FALSE(Rules::filament_refusal(printing, true).empty());

    Rules::FilamentFacts paused = printing;
    paused.can_resume           = true;
    CHECK_FALSE(Rules::filament_refusal(paused, true).empty()); // AMS slot while paused
    paused.virtual_slot = true;
    CHECK(Rules::filament_refusal(paused, true).empty()); // the external spool is fine

    Rules::FilamentFacts busy = idle;
    busy.changing_filament    = true;
    CHECK_FALSE(Rules::filament_refusal(busy, false).empty());

    Rules::FilamentFacts track = idle;
    track.track_switch_installed = true;
    CHECK_FALSE(Rules::filament_refusal(track, true).empty()); // not set up
    track.track_switch_ready = true;
    CHECK(Rules::filament_refusal(track, true).empty());
    track.virtual_slot = true;
    CHECK_FALSE(Rules::filament_refusal(track, true).empty()); // no external spool with the switch

    Rules::FilamentFacts loaded = idle;
    loaded.slot_loaded          = true;
    CHECK_FALSE(Rules::filament_refusal(loaded, true).empty());
    CHECK(Rules::filament_refusal(loaded, false).empty());

    Rules::FilamentFacts empty = idle;
    empty.slot_has_filament    = false;
    CHECK_FALSE(Rules::filament_refusal(empty, true).empty());
}

TEST_CASE("Chamber and extruder rules follow StatusPanel", "[actions]")
{
    CHECK_FALSE(Rules::chamber_refusal(true, true, true).empty());
    CHECK(Rules::chamber_refusal(false, true, true).empty());
    CHECK(Rules::chamber_refusal(true, false, true).empty());
    CHECK(Rules::chamber_switches_to_heating(false, 45, 40));
    CHECK_FALSE(Rules::chamber_switches_to_heating(true, 45, 40));
    CHECK_FALSE(Rules::chamber_switches_to_heating(false, 35, 40));

    CHECK_FALSE(Rules::select_extruder_refusal(true, false, false).empty());
    CHECK(Rules::select_extruder_refusal(true, true, false).empty());
    CHECK_FALSE(Rules::select_extruder_refusal(false, false, true).empty());
}

TEST_CASE("AMS drying limits per AMS type", "[actions]")
{
    const Rules::DryLimits pro = Rules::dry_limits(true, false), ht = Rules::dry_limits(false, true), none = Rules::dry_limits(false, false);
    CHECK(Rules::dry_refusal(pro, 55, 8).empty());
    CHECK_FALSE(Rules::dry_refusal(pro, 70, 8).empty());
    CHECK_FALSE(Rules::dry_refusal(pro, 40, 8).empty());
    CHECK(Rules::dry_refusal(ht, 80, 8).empty());
    CHECK_FALSE(Rules::dry_refusal(ht, 55, 25).empty());
    CHECK_FALSE(Rules::dry_refusal(ht, 55, 0).empty());
    CHECK_FALSE(Rules::dry_refusal(none, 55, 8).empty());
}

TEST_CASE("Job names follow the print dialog's rename rules", "[actions]")
{
    std::string out;
    CHECK(Rules::job_name_refusal("Bracket  v2", out).empty());
    CHECK(out == "Bracket v2");
    CHECK_FALSE(Rules::job_name_refusal("a/b", out).empty());
    CHECK_FALSE(Rules::job_name_refusal("part (modified)", out).empty());
    CHECK_FALSE(Rules::job_name_refusal(" lead", out).empty());
    CHECK_FALSE(Rules::job_name_refusal("", out).empty());
    CHECK_FALSE(Rules::job_name_refusal(std::string(100, 'x'), out).empty());
    CHECK(Rules::job_name_refusal(std::string(99, 'x'), out).empty());
}

TEST_CASE("Printer error buttons have stable names", "[actions]")
{
    CHECK(std::string(Rules::error_action_name(2)) == "resumePrinting");
    CHECK(std::string(Rules::error_action_name(5)) == "stopPrinting");
    CHECK(std::string(Rules::error_action_name(10002)) == "retry");
    CHECK(Rules::error_action_name(6) == nullptr);  // Check Assistant: slicer navigation only
    CHECK(Rules::error_action_name(41) == nullptr); // Proceed: carries the printer's own request
    CHECK(Rules::rrf_speed_percent(1) == 50);
    CHECK(Rules::rrf_speed_percent(4) == 166);
}

TEST_CASE("Printer descriptors: valid schemas, every control action listed", "[actions]")
{
    Catalog c;
    register_existing_methods(c);
    for (const char* id : {"printer.list", "printer.status", "printer.print", "printer.send", "printer.select", "printer.jobs",
                           "printer.cancelJob", "printer.control"}) {
        const ActionEntry* e = c.find(id);
        REQUIRE(e != nullptr);
        CHECK(e->descriptor.gate == Gate::Printer);
        SchemaError err;
        CHECK(check_schema(e->descriptor.params, err));
    }
    const json& actions = c.find("printer.control")->descriptor.params["properties"]["action"]["enum"];
    const std::set<std::string> listed(actions.begin(), actions.end());
    for (const char* a : {"pause", "stop", "resume", "setBedTemp", "setNozzleTemp", "setChamberTemp", "setFan", "setAirductMode", "setSpeed",
                          "setLight", "home", "jog", "selectExtruder", "loadFilament", "unloadFilament", "retryFilamentChange",
                          "abortFilamentChange", "refreshRfid", "dryAms", "stopDryingAms", "errorAction", "gcode"})
        CHECK(listed.count(a) == 1);
    SchemaError err;
    CHECK(validate(c.find("printer.print")->descriptor.params,
                   json{{"printer", "pr-1"}, {"plate", "all"}, {"options", {{"timelapse", "off"}, {"jobName", "Bracket"}}}}, err));
    CHECK_FALSE(validate(c.find("printer.print")->descriptor.params, json{{"printer", "pr-1"}, {"options", {{"vibration", "on"}}}}, err));
}
