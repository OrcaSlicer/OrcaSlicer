#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/ToolbarLayout.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r::GUI;

TEST_CASE("ToolbarLayout empty or bad value gives stock", "[ToolbarLayout]")
{
    CHECK(parse_toolbar_layout("") == stock_toolbar_layout());
    CHECK(parse_toolbar_layout("{not json") == stock_toolbar_layout());
    CHECK(parse_toolbar_layout("[1,2]") == stock_toolbar_layout());
}

TEST_CASE("ToolbarLayout merge drops unknown ids and duplicates", "[ToolbarLayout]")
{
    const ToolbarGroupLayout g = merge_toolbar_group({"a", "b", "c"}, {"c", "zz", "c", "a", "b"}, {"zz", "b"});
    CHECK(g.order == std::vector<std::string>{"c", "a", "b"});
    CHECK(g.hidden == std::set<std::string>{"b"});
}

TEST_CASE("ToolbarLayout merge inserts new stock ids after their stock predecessor", "[ToolbarLayout]")
{
    CHECK(merge_toolbar_group({"a", "b", "new", "c"}, {"c", "b", "a"}, {}).order == std::vector<std::string>{"c", "b", "new", "a"});
    CHECK(merge_toolbar_group({"new", "a", "b"}, {"b", "a"}, {}).order == std::vector<std::string>{"new", "b", "a"});
}

TEST_CASE("ToolbarLayout keeps the separator token", "[ToolbarLayout]")
{
    const ToolbarLayout l = parse_toolbar_layout(R"({"version":1,"main":["|","add"],"main_hidden":["|"]})");
    CHECK(l.main.order.front() == TOOLBAR_SEPARATOR_ID);
    CHECK(l.main.hidden.count(TOOLBAR_SEPARATOR_ID) == 1);
}

TEST_CASE("ToolbarLayout serialises stock as empty", "[ToolbarLayout]")
{
    CHECK(serialize_toolbar_layout(stock_toolbar_layout()).empty());
}

TEST_CASE("ToolbarLayout round-trips a change and writes only differing parts", "[ToolbarLayout]")
{
    ToolbarLayout l = stock_toolbar_layout();
    l.gizmos.hidden.insert("brim_ears");
    const nlohmann::json j = nlohmann::json::parse(serialize_toolbar_layout(l));
    CHECK(j["version"] == 1);
    CHECK(!j.contains("main"));
    CHECK(!j.contains("main_hidden"));
    CHECK(!j.contains("gizmos"));
    CHECK(j["gizmos_hidden"] == nlohmann::json::array({"brim_ears"}));
    CHECK(parse_toolbar_layout(j.dump()) == l);
}

TEST_CASE("ToolbarLayout keeps unknown fields and a newer version", "[ToolbarLayout]")
{
    ToolbarLayout l = parse_toolbar_layout(R"({"version":2,"future":[{"id":"x"}],"gizmos_hidden":["seam"]})");
    CHECK(l.version == 2);
    l.gizmos.hidden.insert("cut");
    const nlohmann::json j = nlohmann::json::parse(serialize_toolbar_layout(l));
    CHECK(j["version"] == 2);
    CHECK(j["future"][0]["id"] == "x");

    // Stock lists with unknown fields still serialise, so a newer build's data is never dropped silently.
    ToolbarLayout s = stock_toolbar_layout();
    s.extra   = l.extra;
    s.version = 2;
    CHECK(!serialize_toolbar_layout(s).empty());
}

TEST_CASE("ToolbarLayout allows every item hidden", "[ToolbarLayout]")
{
    ToolbarLayout l = stock_toolbar_layout();
    for (const std::string& id : stock_gizmo_bar_ids())
        l.gizmos.hidden.insert(id);
    CHECK(parse_toolbar_layout(serialize_toolbar_layout(l)).gizmos.hidden.size() == stock_gizmo_bar_ids().size());
}

TEST_CASE("ToolbarLayout gizmo ids cover the bar without svg and simplify", "[ToolbarLayout]")
{
    CHECK(gizmo_ids_in_etype_order().size() == 18);
    CHECK(stock_gizmo_bar_ids().size() == 16);
    CHECK(std::count(stock_gizmo_bar_ids().begin(), stock_gizmo_bar_ids().end(), "svg") == 0);
    CHECK(parse_toolbar_layout(R"({"gizmos":["simplify","move"]})").gizmos.order.front() == "move");
}

TEST_CASE("ToolbarLayout user groups round-trip", "[ToolbarLayout]")
{
    ToolbarLayout l = stock_toolbar_layout();
    l.groups.push_back({"Surface", {"fuzzy_skin", "texture_displacement"}});
    normalize_gizmo_groups(l);
    const ToolbarLayout back = parse_toolbar_layout(serialize_toolbar_layout(l));
    CHECK(back == l);
    REQUIRE(back.groups.size() == 1);
    CHECK(back.groups[0].name == "Surface");
}

TEST_CASE("ToolbarLayout user groups drop bad members", "[ToolbarLayout]")
{
    const ToolbarLayout l = parse_toolbar_layout(R"({"version":1,"groups":[
        {"name":"A","members":["seam","nope","fdm_supports"]},
        {"name":"B","members":["seam","fuzzy_skin"]},
        {"name":"C","members":["move","svg"]}]})");
    // B loses seam to A and keeps one member, so it is dropped; svg is not a bar id, so C is dropped too.
    REQUIRE(l.groups.size() == 1);
    CHECK(l.groups[0] == ToolbarGizmoGroup{"A", {"fdm_supports", "seam"}});
}

TEST_CASE("ToolbarLayout user group members are made contiguous", "[ToolbarLayout]")
{
    const ToolbarLayout l = parse_toolbar_layout(R"({"version":1,"groups":[{"name":"G","members":["move","brim_ears"]}]})");
    const std::vector<std::string>& o = l.gizmos.order;
    const auto at = std::find(o.begin(), o.end(), "move") - o.begin();
    CHECK(o[static_cast<size_t>(at) + 1] == "brim_ears");
}

TEST_CASE("ToolbarLayout keeps other group entries and writes no empty groups", "[ToolbarLayout]")
{
    const ToolbarLayout l = parse_toolbar_layout(R"({"version":1,"groups":[{"id":"paint"}]})");
    CHECK(l.groups.empty());
    CHECK(serialize_toolbar_layout(l).find("\"paint\"") != std::string::npos);
    CHECK(serialize_toolbar_layout(stock_toolbar_layout()).empty());
}

TEST_CASE("ToolbarLayout bad groups keep other settings", "[ToolbarLayout]")
{
    const ToolbarLayout l = parse_toolbar_layout(R"({"version":1,"main":["layersediting","add"],"gizmos_hidden":["move"],
        "groups":[{"name":"X","members":["nope","svg"]},{"name":"Y","members":"seam"}]})");
    CHECK(l.groups.empty());
    CHECK(l.gizmos.hidden.count("move") == 1);
    CHECK(l.main.order.front() == "layersediting");
}

namespace {

// A short gizmo order so the edit rules are easy to follow.
ToolbarLayout six_gizmos()
{
    ToolbarLayout l   = stock_toolbar_layout();
    l.gizmos.order    = {"move", "rotate", "scale", "seam", "fdm_supports", "fuzzy_skin"};
    return l;
}

using Order = std::vector<std::string>;

ToolbarGizmoRow plain(const std::string& id) { return {ToolbarRowKind::Plain, id, -1}; }
ToolbarGizmoRow member(const std::string& id, int group) { return {ToolbarRowKind::Member, id, group}; }
ToolbarGizmoRow header(int group) { return {ToolbarRowKind::Header, "", group}; }

} // namespace

TEST_CASE("Gizmo groups: grouping gathers members under a header", "[ToolbarLayout]")
{
    ToolbarLayout l = six_gizmos();
    REQUIRE(group_gizmos(l, {"scale", "move"}, "G"));
    CHECK(gizmo_rows(l) == std::vector<ToolbarGizmoRow>{header(0), member("move", 0), member("scale", 0), plain("rotate"), plain("seam"),
                                                         plain("fdm_supports"), plain("fuzzy_skin")});
    CHECK(l.groups[0] == ToolbarGizmoGroup{"G", {"move", "scale"}});
    CHECK_FALSE(group_gizmos(l, {"seam"}, "One"));
    CHECK_FALSE(group_gizmos(l, {"move", "rotate"}, "Taken"));
    CHECK_FALSE(group_gizmos(l, {"svg", "rotate"}, "Not on the bar"));
    CHECK(l.groups.size() == 1);
}

TEST_CASE("Gizmo groups: adding puts rows after the group's last member", "[ToolbarLayout]")
{
    ToolbarLayout l = six_gizmos();
    REQUIRE(group_gizmos(l, {"move", "scale"}, "G"));
    REQUIRE(add_to_group(l, 0, {"fuzzy_skin"}));
    CHECK(l.gizmos.order == Order{"move", "scale", "fuzzy_skin", "rotate", "seam", "fdm_supports"});
    CHECK(l.groups[0].members == Order{"move", "scale", "fuzzy_skin"});
}

TEST_CASE("Gizmo groups: adding rejects a row of another group", "[ToolbarLayout]")
{
    ToolbarLayout l = six_gizmos();
    REQUIRE(group_gizmos(l, {"move", "rotate"}, "A"));
    REQUIRE(group_gizmos(l, {"seam", "fdm_supports"}, "B"));
    CHECK_FALSE(add_to_group(l, 0, {"seam"}));
    CHECK(l.groups[1].members == Order{"seam", "fdm_supports"});
}

TEST_CASE("Gizmo groups: ungrouping keeps the order", "[ToolbarLayout]")
{
    ToolbarLayout l = six_gizmos();
    REQUIRE(group_gizmos(l, {"move", "scale"}, "G"));
    REQUIRE(ungroup_gizmos(l, 0));
    CHECK(l.groups.empty());
    CHECK(l.gizmos.order == Order{"move", "scale", "rotate", "seam", "fdm_supports", "fuzzy_skin"});
}

TEST_CASE("Gizmo groups: renaming trims, and a blank name falls back", "[ToolbarLayout]")
{
    ToolbarLayout l = six_gizmos();
    REQUIRE(group_gizmos(l, {"move", "scale"}, "G"));
    REQUIRE(rename_group(l, 0, "  X "));
    CHECK(l.groups[0].name == "X");
    CHECK_FALSE(rename_group(l, 0, "   "));
    CHECK(l.groups[0].name == "X");
    CHECK_FALSE(rename_group(l, 5, "Y"));
    CHECK_FALSE(ungroup_gizmos(l, 5));
    CHECK_FALSE(add_to_group(l, 5, {"seam"}));
}

TEST_CASE("Gizmo groups: the default name takes the lowest free number", "[ToolbarLayout]")
{
    ToolbarLayout l = six_gizmos();
    l.groups        = {{"Group 1", {"move", "rotate"}}, {"Group 3", {"seam", "fdm_supports"}}};
    CHECK(default_group_name(l, [](int n) { return "Group " + std::to_string(n); }) == "Group 2");
}

TEST_CASE("Gizmo groups: a header moves the whole group", "[ToolbarLayout]")
{
    ToolbarLayout l = six_gizmos();
    REQUIRE(group_gizmos(l, {"move", "rotate"}, "G"));
    CHECK_FALSE(move_gizmo_row(l, header(0), -1));
    REQUIRE(move_gizmo_row(l, header(0), 1));
    CHECK(l.gizmos.order == Order{"scale", "move", "rotate", "seam", "fdm_supports", "fuzzy_skin"});
}

TEST_CASE("Gizmo groups: a header jumps a whole neighbouring group", "[ToolbarLayout]")
{
    ToolbarLayout l = six_gizmos();
    REQUIRE(group_gizmos(l, {"move", "rotate"}, "A"));
    REQUIRE(group_gizmos(l, {"scale", "seam"}, "B"));
    REQUIRE(move_gizmo_row(l, header(0), 1));
    CHECK(l.gizmos.order == Order{"scale", "seam", "move", "rotate", "fdm_supports", "fuzzy_skin"});
    CHECK(l.groups[0].name == "B");
}

TEST_CASE("Gizmo groups: a plain row jumps a whole group", "[ToolbarLayout]")
{
    ToolbarLayout l = six_gizmos();
    REQUIRE(group_gizmos(l, {"rotate", "scale"}, "G"));
    REQUIRE(move_gizmo_row(l, plain("seam"), -1));
    CHECK(l.gizmos.order == Order{"move", "seam", "rotate", "scale", "fdm_supports", "fuzzy_skin"});
    CHECK(l.groups[0].members == Order{"rotate", "scale"});
}

TEST_CASE("Gizmo groups: a member swaps inside its group", "[ToolbarLayout]")
{
    ToolbarLayout l = six_gizmos();
    REQUIRE(group_gizmos(l, {"move", "rotate", "scale"}, "G"));
    REQUIRE(move_gizmo_row(l, member("rotate", 0), 1));
    CHECK(l.groups[0].members == Order{"move", "scale", "rotate"});
}

TEST_CASE("Gizmo groups: a member leaves at either edge, and a group of one dissolves", "[ToolbarLayout]")
{
    ToolbarLayout l = six_gizmos();
    REQUIRE(group_gizmos(l, {"rotate", "scale", "seam"}, "G"));
    REQUIRE(move_gizmo_row(l, member("rotate", 0), -1));
    CHECK(l.groups[0].members == Order{"scale", "seam"});
    CHECK(gizmo_rows(l)[1] == plain("rotate"));
    REQUIRE(move_gizmo_row(l, member("seam", 0), 1));
    CHECK(l.groups.empty());
    CHECK(l.gizmos.order == Order{"move", "rotate", "scale", "seam", "fdm_supports", "fuzzy_skin"});
}

TEST_CASE("Gizmo bar slots without groups are one per id", "[ToolbarLayout]")
{
    const auto slots = compute_gizmo_bar_slots({"move", "seam"}, {}, "", {});
    REQUIRE(slots.size() == 2);
    CHECK(slots[1] == ToolbarBarSlot{"seam", {}, -1});
}

TEST_CASE("Gizmo bar group slot sits at its first member and lists members in bar order", "[ToolbarLayout]")
{
    const auto slots = compute_gizmo_bar_slots({"move", "seam", "rotate", "fdm_supports", "fuzzy_skin"},
                                               {{"P", {"seam", "fdm_supports", "fuzzy_skin"}}}, "", {});
    REQUIRE(slots.size() == 3);
    CHECK(slots[0].shown == "move");
    CHECK(slots[1] == ToolbarBarSlot{"seam", {"seam", "fdm_supports", "fuzzy_skin"}, 0});
    CHECK(slots[2].shown == "rotate");
}

TEST_CASE("Gizmo bar adjacent groups stay separate slots", "[ToolbarLayout]")
{
    const auto slots = compute_gizmo_bar_slots({"move", "rotate", "scale", "seam"}, {{"A", {"move", "rotate"}}, {"B", {"scale", "seam"}}}, "", {});
    REQUIRE(slots.size() == 2);
    CHECK(slots[0] == ToolbarBarSlot{"move", {"move", "rotate"}, 0});
    CHECK(slots[1] == ToolbarBarSlot{"scale", {"scale", "seam"}, 1});
}

TEST_CASE("Gizmo bar hidden member between groups", "[ToolbarLayout]")
{
    const auto slots = compute_gizmo_bar_slots({"move", "scale", "seam", "fdm_supports"},
                                               {{"A", {"move", "rotate", "scale"}}, {"B", {"seam", "fdm_supports"}}}, "", {});
    REQUIRE(slots.size() == 2);
    CHECK(slots[0] == ToolbarBarSlot{"move", {"move", "scale"}, 0});
    CHECK(slots[1] == ToolbarBarSlot{"seam", {"seam", "fdm_supports"}, 1});
}

TEST_CASE("Gizmo bar group with one bar member is a plain icon", "[ToolbarLayout]")
{
    const auto slots = compute_gizmo_bar_slots({"move", "scale"}, {{"A", {"move", "rotate"}}}, "", {});
    REQUIRE(slots.size() == 2);
    CHECK(slots[0] == ToolbarBarSlot{"move", {}, -1});
}

TEST_CASE("Gizmo bar group slot shows the open, then the recent openable, then the first openable member", "[ToolbarLayout]")
{
    const std::vector<std::string>       ids{"seam", "fdm_supports", "fuzzy_skin"};
    const std::vector<ToolbarGizmoGroup> groups{{"P", ids}};
    CHECK(compute_gizmo_bar_slots(ids, groups, "fuzzy_skin", {"fdm_supports"}, {"fuzzy_skin"})[0].shown == "fuzzy_skin");
    CHECK(compute_gizmo_bar_slots(ids, groups, "move", {"fdm_supports"})[0].shown == "fdm_supports");
    CHECK(compute_gizmo_bar_slots(ids, groups, "", {"fuzzy_skin", "fdm_supports"}, {"fuzzy_skin"})[0].shown == "fdm_supports");
    CHECK(compute_gizmo_bar_slots(ids, groups, "", {}, {"seam"})[0].shown == "fdm_supports");
    CHECK(compute_gizmo_bar_slots(ids, groups, "", {"fdm_supports"}, {"seam", "fdm_supports", "fuzzy_skin"})[0].shown == "fdm_supports");
}

TEST_CASE("Gizmo bar group slot ignores a recent member that is not on the bar", "[ToolbarLayout]")
{
    const auto slots = compute_gizmo_bar_slots({"seam", "fdm_supports"}, {{"P", {"seam", "fdm_supports", "fuzzy_skin"}}}, "", {"fuzzy_skin"});
    CHECK(slots[0].shown == "seam");
}

TEST_CASE("Gizmo bar slot lookup maps a grouped member to the group slot", "[ToolbarLayout]")
{
    const auto slots = compute_gizmo_bar_slots({"move", "seam", "fdm_supports"}, {{"P", {"seam", "fdm_supports"}}}, "", {});
    CHECK(bar_slot_shown_for(slots, "fdm_supports") == "seam");
    CHECK(bar_slot_shown_for(slots, "move") == "move");
    CHECK(bar_slot_shown_for(slots, "cut").empty());
}

TEST_CASE("ToolbarLayout user groups survive a saved groups value that is not an array", "[ToolbarLayout]")
{
    ToolbarLayout l = parse_toolbar_layout(R"({"version":1,"groups":"weird"})");
    REQUIRE(group_gizmos(l, {"seam", "fuzzy_skin"}, "G"));
    const ToolbarLayout back = parse_toolbar_layout(serialize_toolbar_layout(l));
    REQUIRE(back.groups.size() == 1);
    CHECK(back.groups[0].name == "G");
}
