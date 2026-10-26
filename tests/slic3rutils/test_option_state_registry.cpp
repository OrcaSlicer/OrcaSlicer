#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include "slic3r/GUI/OptionStateRegistry.hpp"
#include "slic3r/GUI/ConfigManipulation.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Config.hpp"

#include <string>

#include <wx/string.h>

using Slic3r::GUI::OptionStateRegistry;

TEST_CASE("Showing an option preserves its independent editing restriction", "[OptionStateRegistry]")
{
    OptionStateRegistry registry;
    registry.set_visible("option", false, "Hidden reason");
    registry.set_enabled("option", false, "Disabled reason");
    registry.set_visible("option", true);

    CHECK(registry.hidden_reason("option") == nullptr);
    REQUIRE(registry.disabled_reason("option") != nullptr);
    CHECK(*registry.disabled_reason("option") == "Disabled reason");

    registry.set_enabled("option", true);
    CHECK(registry.disabled_reason("option") == nullptr);
}

TEST_CASE("A restriction keeps only the latest reason including an unspecified reason", "[OptionStateRegistry]")
{
    OptionStateRegistry registry;
    registry.set_visible("option", false, "First reason");
    registry.set_visible("option", false, "Latest reason");
    REQUIRE(registry.hidden_reason("option") != nullptr);
    CHECK(*registry.hidden_reason("option") == "Latest reason");

    registry.set_visible("option", false);
    REQUIRE(registry.hidden_reason("option") != nullptr);
    CHECK(registry.hidden_reason("option")->empty());
}

TEST_CASE("Option restrictions belong to one registry and preserve the supplied key", "[OptionStateRegistry]")
{
    OptionStateRegistry first;
    OptionStateRegistry second;
    first.set_visible("option#0", false, "Reason");
    CHECK(first.hidden_reason("option") == nullptr);
    CHECK(first.hidden_reason("option#1") == nullptr);
    CHECK(second.hidden_reason("option#0") == nullptr);

    first.set_enabled("option", false);
    first.clear();
    CHECK(first.hidden_reason("option#0") == nullptr);
    CHECK(first.disabled_reason("option") == nullptr);
}

TEST_CASE("Dependency toggles forward their reasons without requiring page controls", "[OptionStateRegistry]")
{
    OptionStateRegistry registry;
    Slic3r::GUI::ConfigManipulation manipulation(nullptr,
        [&registry](const std::string& key, bool enabled, int, const wxString& reason) {
            registry.set_enabled(key, enabled, reason);
        },
        [&registry](const std::string& key, bool visible, int, const wxString& reason) {
            registry.set_visible(key, visible, reason);
        }, nullptr);

    manipulation.toggle_field("option", false, 2, "Editing reason");
    manipulation.toggle_line("option", false, 2, "Visibility reason");
    REQUIRE(registry.disabled_reason("option") != nullptr);
    CHECK(*registry.disabled_reason("option") == "Editing reason");
    REQUIRE(registry.hidden_reason("option") != nullptr);
    CHECK(*registry.hidden_reason("option") == "Visibility reason");

    manipulation.toggle_field("option", true, 2);
    CHECK(registry.disabled_reason("option") == nullptr);
    CHECK(registry.hidden_reason("option") != nullptr);
    manipulation.toggle_line("option", true, 2);
    CHECK(registry.hidden_reason("option") == nullptr);
}

TEST_CASE("Availability descriptions update when either restriction is removed", "[OptionStateRegistry]")
{
    OptionStateRegistry registry;
    CHECK(registry.description("option").empty());
    registry.set_visible("option", false, "Enable sparse infill.");
    registry.set_enabled("option", false, "Change wall generator.");
    wxString description = registry.description("option");
    CHECK(description.Contains("Enable sparse infill."));
    CHECK(description.Contains("Change wall generator."));

    registry.set_visible("option", true);
    CHECK_FALSE(registry.description("option").Contains("Enable sparse infill."));
    CHECK(registry.description("option").Contains("Change wall generator."));
    registry.set_enabled("option", true);
    CHECK(registry.description("option").empty());

    registry.set_enabled("option", false);
    CHECK_FALSE(registry.description("option").empty());
}

TEST_CASE("SLA dependency rules explain restrictions and clear them when prerequisites change", "[OptionStateRegistry]")
{
    OptionStateRegistry registry;
    Slic3r::GUI::ConfigManipulation manipulation(nullptr,
        [&registry](const std::string& key, bool enabled, int, const wxString& reason) {
            INFO(key);
            if (!enabled)
                CHECK_FALSE(reason.empty());
            registry.set_enabled(key, enabled, reason);
        }, nullptr, nullptr);
    Slic3r::DynamicPrintConfig config;
    config.apply(Slic3r::SLAFullPrintConfig{});
    config.set_key_value("supports_enable", new Slic3r::ConfigOptionBool(false));
    config.set_key_value("pad_enable", new Slic3r::ConfigOptionBool(false));
    config.set_key_value("pad_around_object", new Slic3r::ConfigOptionBool(false));
    manipulation.toggle_print_sla_options(&config);
    REQUIRE(registry.disabled_reason("support_head_width") != nullptr);
    CHECK_FALSE(registry.disabled_reason("support_head_width")->empty());
    REQUIRE(registry.disabled_reason("pad_object_gap") != nullptr);

    config.set_key_value("supports_enable", new Slic3r::ConfigOptionBool(true));
    config.set_key_value("pad_enable", new Slic3r::ConfigOptionBool(true));
    config.set_key_value("pad_around_object", new Slic3r::ConfigOptionBool(true));
    manipulation.toggle_print_sla_options(&config);
    CHECK(registry.disabled_reason("support_head_width") == nullptr);
    CHECK(registry.disabled_reason("pad_object_gap") == nullptr);
    REQUIRE(registry.disabled_reason("support_object_elevation") != nullptr);
    CHECK_FALSE(registry.disabled_reason("support_object_elevation")->empty());

    config.set_key_value("pad_around_object", new Slic3r::ConfigOptionBool(false));
    manipulation.toggle_print_sla_options(&config);
    CHECK(registry.disabled_reason("support_object_elevation") == nullptr);
    REQUIRE(registry.disabled_reason("pad_object_gap") != nullptr);
    CHECK_FALSE(registry.disabled_reason("pad_object_gap")->empty());
}
