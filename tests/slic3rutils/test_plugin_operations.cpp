#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <slic3r/plugin/PluginOperations.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;

namespace {

PluginOperation operation(const std::string& plugin, const std::string& name)
{
    PluginOperation op;
    op.plugin_key  = plugin;
    op.name        = name;
    op.description = "Measures " + name;
    return op;
}

} // namespace

TEST_CASE("a plugin's granted operations are offered once it loads and withdrawn when it unloads", "[PluginOperations]")
{
    PluginOperations& ops = PluginOperations::instance();
    ops.remove_plugin("measure");
    ops.remove_plugin("other");

    ops.stage("measure", {operation("measure", "area"), operation("measure", "volume")});
    // Staged while it loads, not offered yet.
    CHECK_FALSE(ops.find("plugin.measure.area").has_value());

    ops.activate("measure");
    REQUIRE(ops.find("plugin.measure.area").has_value());
    CHECK(ops.find("plugin.measure.area")->description == "Measures area");
    CHECK(ops.find("plugin.measure.volume").has_value());
    CHECK_FALSE(ops.find("plugin.other.area").has_value());

    // Another plugin's operation of the same name is its own.
    ops.add(operation("other", "area"));
    CHECK(ops.find("plugin.other.area").has_value());

    ops.remove_plugin("measure");
    CHECK_FALSE(ops.find("plugin.measure.area").has_value());
    CHECK(ops.find("plugin.other.area").has_value());
    ops.remove_plugin("other");
    CHECK(ops.all().empty());
}

TEST_CASE("a load that stages again replaces what an earlier attempt staged", "[PluginOperations]")
{
    PluginOperations& ops = PluginOperations::instance();
    ops.remove_plugin("measure");
    ops.stage("measure", {operation("measure", "area")});
    ops.stage("measure", {operation("measure", "volume")});
    ops.activate("measure");
    CHECK_FALSE(ops.find("plugin.measure.area").has_value());
    CHECK(ops.find("plugin.measure.volume").has_value());
    ops.remove_plugin("measure");
}

TEST_CASE("a load that fails drops what it staged and keeps what was offered", "[PluginOperations]")
{
    PluginOperations& ops = PluginOperations::instance();
    ops.remove_plugin("measure");
    ops.stage("measure", {operation("measure", "area")});
    ops.activate("measure");
    // A later load of the same plugin stages again, then fails.
    ops.stage("measure", {operation("measure", "volume")});
    ops.discard_staged("measure");
    ops.activate("measure");
    CHECK(ops.find("plugin.measure.area").has_value());
    CHECK_FALSE(ops.find("plugin.measure.volume").has_value());
    ops.remove_plugin("measure");
}

TEST_CASE("an operation found before its plugin unloaded is refused when it is invoked", "[PluginOperations]")
{
    PluginOperations& ops = PluginOperations::instance();
    ops.remove_plugin("measure");
    ops.add(operation("measure", "area"));
    const std::optional<PluginOperation> found = ops.find("plugin.measure.area");
    REQUIRE(found.has_value());
    // Still offered, it gets as far as Python, which this test never starts.
    CHECK_THROWS_WITH(ops.invoke(*found, nlohmann::json::object()), ContainsSubstring("Python"));

    ops.remove_plugin("measure");
    CHECK_THROWS_WITH(ops.invoke(*found, nlohmann::json::object()), ContainsSubstring("unloaded"));
}
