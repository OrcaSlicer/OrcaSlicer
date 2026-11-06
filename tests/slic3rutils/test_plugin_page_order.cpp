#include <catch2/catch_test_macros.hpp>

#include <slic3r/plugin/host/PluginPageOrder.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>

#include <optional>
#include <string>
#include <vector>

using Slic3r::PluginCapabilityId;
using Slic3r::PluginCapabilityType;
using Slic3r::move_plugin_page;
using Slic3r::order_plugin_pages;
using Slic3r::parse_plugin_page_order;
using Slic3r::serialize_plugin_page_order;

namespace {

PluginCapabilityId page(const std::string& plugin_key, const std::string& name = "Library")
{
    return {PluginCapabilityType::Pages, name, plugin_key};
}

} // namespace

TEST_CASE("saved plugin tab order survives asynchronous page registration", "[PluginPageOrder]")
{
    const auto first = page("first");
    const auto second = page("second");
    const auto third = page("third");
    const std::vector<PluginCapabilityId> preferred{first, second, third};
    std::vector<PluginCapabilityId> active{third};

    order_plugin_pages(active, preferred);
    CHECK(active == std::vector<PluginCapabilityId>{third});
    active.push_back(first);
    order_plugin_pages(active, preferred);
    CHECK(active == std::vector<PluginCapabilityId>{first, third});
    active.push_back(second);
    order_plugin_pages(active, preferred);
    CHECK(active == preferred);
}

TEST_CASE("new plugin tabs follow saved tabs in registration order", "[PluginPageOrder]")
{
    const auto first = page("first");
    const auto second = page("second");
    const auto new_first = page("new-first");
    const auto new_second = page("new-second");
    const std::vector<PluginCapabilityId> preferred{first, second};
    std::vector<PluginCapabilityId> active{new_first, second, new_second, first};

    order_plugin_pages(active, preferred);

    CHECK(active == std::vector<PluginCapabilityId>{first, second, new_first, new_second});
}

TEST_CASE("plugin tab configuration ignores malformed and duplicate entries", "[PluginPageOrder]")
{
    const auto order = parse_plugin_page_order(R"([
        {"plugin_key":"first","name":"Library"},
        {"plugin_key":"first","name":"Library"},
        null, false, [], "first",
        {"plugin_key":"","name":"Library"},
        {"plugin_key":"missing-name"},
        {"plugin_key":"wrong-name","name":5},
        {"plugin_key":5,"name":"Library"},
        {"plugin_key":"empty-name","name":""},
        {"plugin_key":"second","name":"Tools","unknown":true}
    ])");

    REQUIRE(order.has_value());
    CHECK(*order == std::vector<PluginCapabilityId>{page("first"), page("second", "Tools")});
}

TEST_CASE("plugin tab configuration distinguishes missing order from malformed JSON", "[PluginPageOrder]")
{
    const auto empty = parse_plugin_page_order("");
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
    CHECK(parse_plugin_page_order("[]") == empty);
    CHECK_FALSE(parse_plugin_page_order("{"));
    CHECK_FALSE(parse_plugin_page_order("{}"));
    CHECK_FALSE(parse_plugin_page_order("null"));
    CHECK_FALSE(parse_plugin_page_order("42"));
    CHECK_FALSE(parse_plugin_page_order("\"not an array\""));
}

TEST_CASE("plugin tab identities round trip without delimiter ambiguity", "[PluginPageOrder]")
{
    const std::vector<PluginCapabilityId> order{
        page("org.example", "page.tools"),
        page("org.example.page", "tools"),
        page("org.\"quoted\"\\plugin", "Library: [models]"),
        page(u8"例.プラグイン", u8"模型 📦"),
        page("same-plugin", "Other page")
    };

    const auto restored = parse_plugin_page_order(serialize_plugin_page_order(order));

    REQUIRE(restored.has_value());
    CHECK(*restored == order);
}

TEST_CASE("moving plugin tabs retains disabled and overflow page positions", "[PluginPageOrder]")
{
    const auto first = page("first");
    const auto disabled = page("disabled");
    const auto overflow = page("overflow");
    const auto last = page("last");
    std::vector<PluginCapabilityId> order{first, disabled, overflow, last};

    REQUIRE(move_plugin_page(order, last, first, true));
    CHECK(order == std::vector<PluginCapabilityId>{last, first, disabled, overflow});
    REQUIRE(move_plugin_page(order, first, overflow, false));
    CHECK(order == std::vector<PluginCapabilityId>{last, disabled, overflow, first});

    const auto restored = parse_plugin_page_order(serialize_plugin_page_order(order));
    REQUIRE(restored.has_value());
    std::vector<PluginCapabilityId> active{first, last};
    order_plugin_pages(active, *restored);
    CHECK(active == std::vector<PluginCapabilityId>{last, first});
    active.push_back(disabled);
    order_plugin_pages(active, *restored);
    CHECK(active == std::vector<PluginCapabilityId>{last, disabled, first});
}

TEST_CASE("moving plugin tabs reports unchanged order for adjacent and missing targets", "[PluginPageOrder]")
{
    const auto first = page("first");
    const auto second = page("second");
    const auto missing = page("missing");
    const std::vector<PluginCapabilityId> original{first, second};
    auto order = original;

    CHECK_FALSE(move_plugin_page(order, first, second, true));
    CHECK_FALSE(move_plugin_page(order, second, first, false));
    CHECK_FALSE(move_plugin_page(order, first, first, true));
    CHECK_FALSE(move_plugin_page(order, missing, first, true));
    CHECK_FALSE(move_plugin_page(order, first, missing, false));
    CHECK(order == original);
}

TEST_CASE("plugin tabs move in either direction before or after another tab", "[PluginPageOrder]")
{
    const auto first = page("first");
    const auto second = page("second");
    const auto third = page("third");
    const auto fourth = page("fourth");
    std::vector<PluginCapabilityId> order{first, second, third, fourth};

    REQUIRE(move_plugin_page(order, first, third, true));
    CHECK(order == std::vector<PluginCapabilityId>{second, first, third, fourth});
    REQUIRE(move_plugin_page(order, fourth, second, false));
    CHECK(order == std::vector<PluginCapabilityId>{second, fourth, first, third});
}
