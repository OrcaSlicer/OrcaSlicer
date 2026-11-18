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
