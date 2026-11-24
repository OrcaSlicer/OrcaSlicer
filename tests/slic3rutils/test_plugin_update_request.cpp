#include <catch2/catch_test_macros.hpp>
#include <slic3r/plugin/PluginUpdateRequest.hpp>
#include <functional>
#include <stdexcept>
#include <vector>
#include <utility>

using Slic3r::PluginDescriptor;
using Slic3r::CloudPluginState;
using Slic3r::PluginUpdateRequests;
using Result = PluginUpdateRequests::Result;

namespace {
PluginDescriptor installed_plugin()
{
    PluginDescriptor plugin;
    plugin.plugin_key = "cloud-plugin";
    plugin.cloud = CloudPluginState{};
    plugin.cloud->installed = true;
    return plugin;
}
}

TEST_CASE("Plugin updates reject missing callers and other plugin identities", "[PluginUpdateRequest]")
{
    PluginUpdateRequests requests;
    auto plugin = installed_plugin();
    int scheduled = 0;
    auto defer = [&](PluginUpdateRequests::Task) { ++scheduled; };
    REQUIRE(requests.request("", plugin, true, defer, [] {}) == Result::NoCaller);
    REQUIRE(requests.request("another-plugin", plugin, true, defer, [] {}) == Result::WrongPlugin);
    REQUIRE(scheduled == 0);
}

TEST_CASE("Plugin updates require an installed authorized cloud package and sign-in", "[PluginUpdateRequest]")
{
    PluginUpdateRequests requests;
    auto plugin = installed_plugin();
    int scheduled = 0;
    auto defer = [&](PluginUpdateRequests::Task) { ++scheduled; };
    REQUIRE(requests.request(plugin.plugin_key, plugin, false, defer, [] {}) == Result::NotSignedIn);
    plugin.cloud->unauthorized = true;
    REQUIRE(requests.request(plugin.plugin_key, plugin, true, defer, [] {}) == Result::Unavailable);
    plugin.cloud->unauthorized = false;
    plugin.cloud->orphaned = true;
    REQUIRE(requests.request(plugin.plugin_key, plugin, true, defer, [] {}) == Result::Unavailable);
    plugin.cloud->orphaned = false;
    plugin.cloud->installed = false;
    REQUIRE(requests.request(plugin.plugin_key, plugin, true, defer, [] {}) == Result::NotInstalled);
    plugin.cloud.reset();
    REQUIRE(requests.request(plugin.plugin_key, plugin, true, defer, [] {}) == Result::LocalPlugin);
    REQUIRE(scheduled == 0);
}

TEST_CASE("Plugin update work is deferred and duplicate requests are refused until completion", "[PluginUpdateRequest]")
{
    PluginUpdateRequests requests;
    auto plugin = installed_plugin();
    std::vector<PluginUpdateRequests::Task> tasks;
    auto defer = [&](PluginUpdateRequests::Task task) { tasks.push_back(std::move(task)); };
    int executed = 0;
    REQUIRE(requests.request(plugin.plugin_key, plugin, true, defer, [&] { ++executed; }) == Result::Accepted);
    REQUIRE(executed == 0);
    REQUIRE(requests.request(plugin.plugin_key, plugin, true, defer, [] {}) == Result::Busy);
    auto other = plugin;
    other.plugin_key = "another-cloud-plugin";
    REQUIRE(requests.request(other.plugin_key, other, true, defer, [] {}) == Result::Busy);
    REQUIRE(tasks.size() == 1);
    tasks.front()();
    REQUIRE(executed == 1);
    REQUIRE(requests.request(plugin.plugin_key, plugin, true, defer, [] {}) == Result::Accepted);
    tasks.back()(); // Also releases the reservation after a user cancellation (no update work).
}

TEST_CASE("Failed deferred or scheduled updates allow another request", "[PluginUpdateRequest]")
{
    PluginUpdateRequests requests;
    auto plugin = installed_plugin();
    PluginUpdateRequests::Task pending;
    auto defer = [&](PluginUpdateRequests::Task task) { pending = std::move(task); };
    REQUIRE(requests.request(plugin.plugin_key, plugin, true, defer, [] { throw std::runtime_error("download failed"); }) == Result::Accepted);
    REQUIRE_THROWS(pending());
    REQUIRE_THROWS(requests.request(plugin.plugin_key, plugin, true, [](PluginUpdateRequests::Task) { throw std::runtime_error("closing"); }, [] {}));
    REQUIRE(requests.request(plugin.plugin_key, plugin, true, defer, [] {}) == Result::Accepted);
    pending();
}
