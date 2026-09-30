#include <catch2/catch_all.hpp>

#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/host/PluginHostUi.hpp>

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace {

using Slic3r::PluginAuditManager;
using Slic3r::PluginHostUi;
using Slic3r::ScopedPluginAuditContext;

PluginHostUi::DeferredCallbackAuditContext capture_context(
    const std::string& plugin_key,
    const std::string& capability_name)
{
    ScopedPluginAuditContext registration(plugin_key, capability_name);
    return PluginHostUi::capture_deferred_callback_audit_context();
}

} // namespace

TEST_CASE("deferred UI message callbacks restore their host-captured audit identity",
          "[PluginHost][UiAuditContext]")
{
    const auto context = capture_context("plugin.alpha", "alpha-capability");
    REQUIRE(PluginAuditManager::instance().current_plugin().empty());

    std::string seen_plugin;
    std::string seen_capability;
    std::string observed_payload_identity;
    const nlohmann::json spoofed_payload = {
        { "plugin_key", "plugin.attacker" },
        { "capability", "attacker-capability" },
    };
    PluginHostUi::invoke_deferred_callback(context, [&] {
        seen_plugin = PluginAuditManager::instance().current_plugin();
        seen_capability = PluginAuditManager::instance().current_capability();
        observed_payload_identity = spoofed_payload.at("plugin_key").get<std::string>();
    });

    CHECK(seen_plugin == "plugin.alpha");
    CHECK(seen_capability == "alpha-capability");
    CHECK(observed_payload_identity == "plugin.attacker");
    CHECK(PluginAuditManager::instance().current_plugin().empty());
    CHECK(PluginAuditManager::instance().current_capability().empty());
}

TEST_CASE("deferred UI callback context restores prior context through exceptions and nesting",
          "[PluginHost][UiAuditContext]")
{
    const auto alpha = capture_context("plugin.alpha", "alpha-capability");
    const auto beta = capture_context("plugin.beta", "beta-capability");

    ScopedPluginAuditContext outer("plugin.outer", "outer-capability");
    CHECK_THROWS_AS(
        PluginHostUi::invoke_deferred_callback(alpha, [] {
            CHECK(PluginAuditManager::instance().current_plugin() == "plugin.alpha");
            CHECK(PluginAuditManager::instance().current_capability() == "alpha-capability");
            throw std::runtime_error("expected callback failure");
        }),
        std::runtime_error);
    CHECK(PluginAuditManager::instance().current_plugin() == "plugin.outer");
    CHECK(PluginAuditManager::instance().current_capability() == "outer-capability");

    PluginHostUi::invoke_deferred_callback(alpha, [&] {
        CHECK(PluginAuditManager::instance().current_plugin() == "plugin.alpha");
        PluginHostUi::invoke_deferred_callback(beta, [] {
            CHECK(PluginAuditManager::instance().current_plugin() == "plugin.beta");
            CHECK(PluginAuditManager::instance().current_capability() == "beta-capability");
        });
        CHECK(PluginAuditManager::instance().current_plugin() == "plugin.alpha");
        CHECK(PluginAuditManager::instance().current_capability() == "alpha-capability");
    });
    CHECK(PluginAuditManager::instance().current_plugin() == "plugin.outer");
    CHECK(PluginAuditManager::instance().current_capability() == "outer-capability");
}

TEST_CASE("unscoped deferred UI callbacks clear rather than inherit an outer identity",
          "[PluginHost][UiAuditContext]")
{
    const auto unscoped = PluginHostUi::capture_deferred_callback_audit_context();

    ScopedPluginAuditContext outer("plugin.outer", "outer-capability");
    PluginHostUi::invoke_deferred_callback(unscoped, [] {
        CHECK(PluginAuditManager::instance().current_plugin().empty());
        CHECK(PluginAuditManager::instance().current_capability().empty());
    });
    CHECK(PluginAuditManager::instance().current_plugin() == "plugin.outer");
    CHECK(PluginAuditManager::instance().current_capability() == "outer-capability");
}
