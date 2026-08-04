#include <catch2/catch_all.hpp>

#include <slic3r/GUI/PluginStatus.hpp>

using Slic3r::GUI::PluginStatus;
using Slic3r::GUI::resolve_plugin_status;

TEST_CASE("resolve_plugin_status precedence", "[plugin][status]") {
    // the new branch: loaded module + error -> runtime fault, not a load failure.
    REQUIRE(resolve_plugin_status(/*loading*/ false, /*has_error*/ true, /*is_loaded*/ true) == PluginStatus::RuntimeError);

    // error without a live module is a load-time Error.
    REQUIRE(resolve_plugin_status(false, true, false) == PluginStatus::Error);

    // loading wins over a pending error so a reload never flashes red.
    REQUIRE(resolve_plugin_status(true, true, true) == PluginStatus::Loading);

    // healthy loaded plugin.
    REQUIRE(resolve_plugin_status(false, false, true) == PluginStatus::Activated);

    // nothing loaded, no error.
    REQUIRE(resolve_plugin_status(false, false, false) == PluginStatus::Inactive);
}
