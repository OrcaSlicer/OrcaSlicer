#include <catch2/catch_test_macros.hpp>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/PreviewPanelSlots.hpp"

#include <cstddef>
#include <optional>
#include <string>

using namespace Slic3r::GUI;

TEST_CASE("A panel slot survives a round trip through its config string", "[PreviewPanelSlots]")
{
    for (const PreviewPanelSlot slot : {PreviewPanelSlot{PreviewColumn::Left, 0}, PreviewPanelSlot{PreviewColumn::Right, 1}}) {
        const std::optional<PreviewPanelSlot> parsed = parse_preview_panel_slot(preview_panel_slot_to_string(slot));
        REQUIRE(parsed.has_value());
        CHECK(parsed->column == slot.column);
        CHECK(parsed->order == slot.order);
    }
}

TEST_CASE("A malformed slot string is rejected", "[PreviewPanelSlots]")
{
    CHECK_FALSE(parse_preview_panel_slot("").has_value());
    CHECK_FALSE(parse_preview_panel_slot("middle 0").has_value());
    CHECK_FALSE(parse_preview_panel_slot("left").has_value());
}

TEST_CASE("A panel moved across keeps its order, and the G-code panel goes first or second in its column", "[PreviewPanelSlots]")
{
    const PreviewPanelSlots moved = move_panel_across(DEFAULT_PREVIEW_PANEL_SLOTS, PreviewPanel::GCode);
    CHECK(moved[size_t(PreviewPanel::GCode)].column == PreviewColumn::Left);
    CHECK(moved[size_t(PreviewPanel::GCode)].order == 1);
    CHECK(moved[size_t(PreviewPanel::Legend)].column == PreviewColumn::Right);
    CHECK(move_panel_across(moved, PreviewPanel::GCode)[size_t(PreviewPanel::GCode)].column == PreviewColumn::Right);

    CHECK_FALSE(gcode_panel_first(DEFAULT_PREVIEW_PANEL_SLOTS));
    const PreviewPanelSlots first = put_gcode_panel_first(DEFAULT_PREVIEW_PANEL_SLOTS, true);
    CHECK(gcode_panel_first(first));
    CHECK(first[size_t(PreviewPanel::Legend)].column == PreviewColumn::Right);
    CHECK_FALSE(gcode_panel_first(put_gcode_panel_first(first, false)));
}

TEST_CASE("A settings file from before the print summary stacks the legend, the summary and the G-code panel", "[PreviewPanelSlots]")
{
    Slic3r::AppConfig config;
    config.set("legend_slot", "right 0");
    config.set("gcode_panel_slot", "right 1");
    const PreviewPanelSlots read = load_preview_panel_slots(config);
    CHECK(read[size_t(PreviewPanel::Summary)].column == PreviewColumn::Right);
    CHECK(read[size_t(PreviewPanel::Summary)].order == read[size_t(PreviewPanel::GCode)].order);
    CHECK(preview_panel_rank(PreviewPanel::Legend) < preview_panel_rank(PreviewPanel::Summary));
    CHECK(preview_panel_rank(PreviewPanel::Summary) < preview_panel_rank(PreviewPanel::GCode));

    const PreviewPanelSlots moved = move_panel_across(read, PreviewPanel::Summary);
    save_preview_panel_slots(config, moved);
    CHECK(load_preview_panel_slots(config)[size_t(PreviewPanel::Summary)].column == PreviewColumn::Left);
}

TEST_CASE("Panel slots saved in the settings read back, and a malformed one falls back to the default", "[PreviewPanelSlots]")
{
    Slic3r::AppConfig config;
    const PreviewPanelSlots left_first{{{PreviewColumn::Left, 1}, {PreviewColumn::Left, 0}, {PreviewColumn::Right, 1}}};
    save_preview_panel_slots(config, left_first);
    const PreviewPanelSlots read = load_preview_panel_slots(config);
    CHECK(read[size_t(PreviewPanel::GCode)].column == PreviewColumn::Left);
    CHECK(gcode_panel_first(read));

    Slic3r::AppConfig malformed;
    malformed.set("gcode_panel_slot", "middle 0");
    malformed.set("legend_slot", "left");
    const PreviewPanelSlots fallen_back = load_preview_panel_slots(malformed);
    CHECK(fallen_back[size_t(PreviewPanel::GCode)].column == DEFAULT_PREVIEW_PANEL_SLOTS[size_t(PreviewPanel::GCode)].column);
    CHECK(fallen_back[size_t(PreviewPanel::Legend)].column == DEFAULT_PREVIEW_PANEL_SLOTS[size_t(PreviewPanel::Legend)].column);
}

TEST_CASE("Each of the three panels keeps its slot under its own key", "[PreviewPanelSlots]")
{
    Slic3r::AppConfig       config;
    const PreviewPanelSlots slots{{{PreviewColumn::Left, 2}, {PreviewColumn::Right, 0}, {PreviewColumn::Left, 1}}};
    save_preview_panel_slots(config, slots);
    CHECK(config.get("legend_slot") == "left 2");
    CHECK(config.get("gcode_panel_slot") == "right 0");
    CHECK(config.get("print_summary_slot") == "left 1");

    const PreviewPanelSlots read = load_preview_panel_slots(config);
    for (size_t panel = 0; panel < PREVIEW_PANEL_COUNT; ++panel) {
        CHECK(read[panel].column == slots[panel].column);
        CHECK(read[panel].order == slots[panel].order);
    }
}

TEST_CASE("Each panel's collapsed state reads back as saved under its own key, and a value not understood reads as expanded", "[PreviewPanelSlots]")
{
    for (size_t i = 0; i < PREVIEW_PANEL_COUNT; ++i) {
        const PreviewPanel panel = PreviewPanel(i);
        Slic3r::AppConfig  config;
        save_preview_panel_collapsed(config, panel, true);
        CHECK(load_preview_panel_collapsed(config, panel));
        for (size_t other = 0; other < PREVIEW_PANEL_COUNT; ++other)
            if (other != i)
                CHECK_FALSE(load_preview_panel_collapsed(config, PreviewPanel(other)));
        save_preview_panel_collapsed(config, panel, false);
        CHECK_FALSE(load_preview_panel_collapsed(config, panel));
    }

    Slic3r::AppConfig named;
    save_preview_panel_collapsed(named, PreviewPanel::Legend, true);
    save_preview_panel_collapsed(named, PreviewPanel::Summary, false);
    save_preview_panel_collapsed(named, PreviewPanel::GCode, true);
    CHECK(named.get("legend_collapsed") == "1");
    CHECK(named.get("print_summary_collapsed") == "0");
    CHECK(named.get("gcode_panel_collapsed") == "1");

    Slic3r::AppConfig odd;
    odd.set("legend_collapsed", "true");
    odd.set("print_summary_collapsed", "yes");
    odd.set("gcode_panel_collapsed", "collapsed");
    for (size_t i = 0; i < PREVIEW_PANEL_COUNT; ++i)
        CHECK_FALSE(load_preview_panel_collapsed(odd, PreviewPanel(i)));
}
