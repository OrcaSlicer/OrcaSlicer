#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/ToolbarLayout.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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
    ToolbarLayout l = parse_toolbar_layout(R"({"version":2,"groups":[{"id":"paint"}],"gizmos_hidden":["seam"]})");
    CHECK(l.version == 2);
    l.gizmos.hidden.insert("cut");
    const nlohmann::json j = nlohmann::json::parse(serialize_toolbar_layout(l));
    CHECK(j["version"] == 2);
    CHECK(j["groups"][0]["id"] == "paint");

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
