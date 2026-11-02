#include <catch2/catch_all.hpp>

#include <libslic3r/Exception.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Fill/InfillPurpose.hpp>
#include <memory>
#include <optional>
#include <string>

#include "catch2/catch_test_macros.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Fill/InfillFactory.hpp"

using namespace Slic3r;

TEST_CASE("Infill factory registers built-in patterns with explicit metadata", "[InfillFactory]")
{
    InfillFactory::register_all_infills();

    CHECK(InfillFactory::is_infill_registered("rectilinear"));
    CHECK(InfillFactory::is_infill_registered("gyroid"));
    CHECK_FALSE(InfillFactory::is_infill_registered("not-a-built-in-pattern"));
    CHECK(InfillFactory::create_infill_by_id("not-a-built-in-pattern") == nullptr);

    for (const InfillInfo& info : InfillFactory::get_registered_infills()) {
        CHECK_FALSE(info.id.empty());
        CHECK_FALSE(info.is_plugin());
        std::unique_ptr<Fill> filler = InfillFactory::create_infill_by_id(info.id);
        CHECK(filler != nullptr);

        const auto pattern = infill_pattern_from_key(info.id);
        REQUIRE(pattern.has_value());
        std::unique_ptr<Fill> enum_filler(Fill::new_from_type(*pattern));
        CHECK(enum_filler != nullptr);
    }
}

TEST_CASE("An unknown pattern ID returns no fill, while an invalid enum value throws", "[InfillFactory]")
{
    // Fill::new_from_type(string) keeps create_infill_by_id's nullptr contract, so the caller can
    // report a missing plugin provider or an unknown key in its own context.
    std::unique_ptr<Fill> builtin(Fill::new_from_type(std::string("rectilinear")));
    CHECK(builtin != nullptr);
    CHECK(Fill::new_from_type(std::string("not-a-built-in-or-plugin-pattern")) == nullptr);
    CHECK_THROWS_AS(Fill::new_from_type(ipCount), Slic3r::InvalidArgument);
}

TEST_CASE("Internal-only fills are created from their enum value without being registered", "[InfillFactory]")
{
    // ipSupportBase and ipConcentricInternal have no config key and must stay out of the pattern
    // settings. The enum overload creates them directly for the support and narrow-region paths.
    for (const InfillPattern pattern : { ipSupportBase, ipConcentricInternal }) {
        CHECK(InfillFactory::builtin_infill_key(pattern).empty());
        std::unique_ptr<Fill> filler(Fill::new_from_type(pattern));
        CHECK(filler != nullptr);
    }
}

TEST_CASE("Plugin infill registration is owner-scoped and pinned while a selection lives", "[InfillFactory]")
{
    const std::string id = "test-plugin-pattern";
    const std::string plugin_key = "test-plugin";
    const std::string capability = "Test Infill";

    REQUIRE(InfillFactory::register_plugin_infill(id, "Test Pattern", plugin_key, capability,
        {InfillSurfacePurpose::Sparse}, {},
        []() -> std::unique_ptr<Fill> { return {}; }));

    // A second owner cannot claim an ID that is already registered.
    CHECK_FALSE(InfillFactory::register_plugin_infill(id, "Other", "other-plugin", capability,
        {InfillSurfacePurpose::Sparse}, {},
        []() -> std::unique_ptr<Fill> { return {}; }));

    const std::shared_ptr<const InfillInfo> info = InfillFactory::find_infill(id);
    REQUIRE(info != nullptr);
    CHECK(info->is_plugin());
    CHECK(info->plugin_key == plugin_key);
    CHECK(info->capability_name == capability);

    std::string error;
    CHECK_FALSE(static_cast<bool>(InfillFactory::select_infill(id, "other-plugin", capability,
                                                              {InfillSurfacePurpose::Sparse}, error)));
    CHECK_FALSE(error.empty());

    InfillFactory::Selection selection = InfillFactory::select_infill(id, plugin_key, capability,
                                                           {InfillSurfacePurpose::Sparse}, error);
    REQUIRE(static_cast<bool>(selection));
    CHECK(InfillFactory::has_pinned_infills(plugin_key, capability));

    // The selected job keeps the entry available while the owner goes away...
    InfillFactory::unregister_plugin_infills(plugin_key, capability);
    CHECK(InfillFactory::is_infill_registered(id));
    CHECK(InfillFactory::find_infill(id) == nullptr);
    CHECK(InfillFactory::create_infill_by_id(id) == nullptr);

    // ...and it cannot be reused by a new provider until the pin is released.
    CHECK_FALSE(InfillFactory::register_plugin_infill(id, "Other", "other-plugin", capability,
        {InfillSurfacePurpose::Sparse}, {},
        []() -> std::unique_ptr<Fill> { return {}; }));

    selection.reset();
    CHECK_FALSE(InfillFactory::has_pinned_infills(plugin_key, capability));
    CHECK_FALSE(InfillFactory::is_infill_registered(id));
    CHECK(InfillFactory::register_plugin_infill(id, "Replacement", plugin_key, capability,
        {InfillSurfacePurpose::Sparse}, {},
        []() -> std::unique_ptr<Fill> { return {}; }));
    InfillFactory::unregister_plugin_infills(plugin_key, capability);
}

TEST_CASE("A selection keeps the provider lease alive until the job releases it", "[InfillFactory]")
{
    const std::string id = "test-lease-pattern";
    const std::string plugin_key = "test-lease-plugin";
    const std::string capability = "Leased Infill";

    // Stands in for the plugin layer's opaque keep-alive around a provider.
    auto               lease          = std::make_shared<int>(0);
    std::weak_ptr<int> lease_observer = lease;

    REQUIRE(InfillFactory::register_plugin_infill(id, "Leased Pattern", plugin_key, capability,
        {InfillSurfacePurpose::Sparse}, lease,
        []() -> std::unique_ptr<Fill> { return {}; }));
    lease.reset();
    // Publishing the pattern already keeps the provider identity alive.
    CHECK_FALSE(lease_observer.expired());

    std::string error;
    InfillFactory::Selection selection = InfillFactory::select_infill(id, plugin_key, capability,
                                                           {InfillSurfacePurpose::Sparse}, error);
    REQUIRE(static_cast<bool>(selection));

    // An unload retires the entry for new selections but must not drop the provider a job still
    // holds; the selection outlives every Fill the job may still create.
    InfillFactory::unregister_plugin_infills(plugin_key, capability);
    CHECK(InfillFactory::has_pinned_infills(plugin_key, capability));
    CHECK_FALSE(lease_observer.expired());

    selection.reset();
    CHECK(lease_observer.expired());
    CHECK_FALSE(InfillFactory::has_pinned_infills(plugin_key, capability));
}

TEST_CASE("Re-enabling the same provider revives a pattern that is still pinned", "[InfillFactory]")
{
    const std::string id = "test-revive-pattern";
    const std::string plugin_key = "test-revive-plugin";
    const std::string capability = "Revived Infill";

    REQUIRE(InfillFactory::register_plugin_infill(id, "Before", plugin_key, capability,
        {InfillSurfacePurpose::Sparse}, {},
        []() -> std::unique_ptr<Fill> { return {}; }));

    std::string error;
    InfillFactory::Selection first_job = InfillFactory::select_infill(id, plugin_key, capability,
                                                           {InfillSurfacePurpose::Sparse}, error);
    REQUIRE(static_cast<bool>(first_job));

    // Disabling retires the entry while the job holds its pin.
    InfillFactory::unregister_plugin_infills(plugin_key, capability);
    CHECK(InfillFactory::find_infill(id) == nullptr);

    // Re-enabling is the same provider publishing again: the ID becomes selectable, the live pin is
    // preserved, and the already-running job keeps its entry.
    REQUIRE(InfillFactory::register_plugin_infill(id, "After", plugin_key, capability,
        {InfillSurfacePurpose::Sparse}, {},
        []() -> std::unique_ptr<Fill> { return {}; }));
    CHECK(InfillFactory::find_infill(id) != nullptr);

    InfillFactory::Selection second_job = InfillFactory::select_infill(id, plugin_key, capability,
                                                           {InfillSurfacePurpose::Sparse}, error);
    CHECK(static_cast<bool>(second_job));

    first_job.reset();
    CHECK(InfillFactory::is_infill_registered(id));
    second_job.reset();
    // Both pins are gone but the entry is active, so it stays published.
    CHECK(InfillFactory::is_infill_registered(id));

    InfillFactory::unregister_plugin_infills(plugin_key, capability);
    CHECK_FALSE(InfillFactory::is_infill_registered(id));
}

TEST_CASE("Built-in patterns carry the surface purposes their settings offer", "[InfillFactory]")
{
    InfillFactory::register_all_infills();

    // Rectilinear is offered by all four pattern settings, so a plugin-equivalent would be usable
    // everywhere; a pattern key that only the sparse setting offers stays sparse-only.
    const std::shared_ptr<const InfillInfo> rectilinear = InfillFactory::find_infill("rectilinear");
    REQUIRE(rectilinear != nullptr);
    CHECK(rectilinear->supports_surface(InfillSurfacePurpose::Sparse));
    CHECK(rectilinear->supports_surface(InfillSurfacePurpose::InternalSolid));
    CHECK(rectilinear->supports_surface(InfillSurfacePurpose::TopSurface));
    CHECK(rectilinear->supports_surface(InfillSurfacePurpose::BottomSurface));

    const std::shared_ptr<const InfillInfo> gyroid = InfillFactory::find_infill("gyroid");
    REQUIRE(gyroid != nullptr);
    CHECK(gyroid->supports_surface(InfillSurfacePurpose::Sparse));
    CHECK_FALSE(gyroid->supports_surface(InfillSurfacePurpose::TopSurface));

    // Monotonic is a surface-only pattern: it is not one of the sparse dropdown's keys.
    const std::shared_ptr<const InfillInfo> monotonic = InfillFactory::find_infill("monotonic");
    REQUIRE(monotonic != nullptr);
    CHECK_FALSE(monotonic->supports_surface(InfillSurfacePurpose::Sparse));
    CHECK(monotonic->supports_surface(InfillSurfacePurpose::InternalSolid));
    CHECK(monotonic->supports_surface(InfillSurfacePurpose::TopSurface));
    CHECK(monotonic->supports_surface(InfillSurfacePurpose::BottomSurface));
}

TEST_CASE("A plugin pattern can only be pinned for a purpose its descriptor declares", "[InfillFactory]")
{
    const std::string id         = "test-purpose-pattern";
    const std::string plugin_key = "test-purpose-plugin";
    const std::string capability = "Purpose Infill";

    // The same descriptor supports two surfaces; it must not be selectable for the other two.
    REQUIRE(InfillFactory::register_plugin_infill(
        id, "Purpose Pattern", plugin_key, capability,
        {InfillSurfacePurpose::TopSurface, InfillSurfacePurpose::BottomSurface}, {},
        []() -> std::unique_ptr<Fill> { return {}; }));

    const std::shared_ptr<const InfillInfo> info = InfillFactory::find_infill(id);
    REQUIRE(info != nullptr);
    CHECK(info->supports_surface(InfillSurfacePurpose::TopSurface));
    CHECK(info->supports_surface(InfillSurfacePurpose::BottomSurface));
    CHECK_FALSE(info->supports_surface(InfillSurfacePurpose::Sparse));

    std::string error;
    CHECK_FALSE(static_cast<bool>(
        InfillFactory::select_infill(id, plugin_key, capability, {InfillSurfacePurpose::Sparse}, error)));
    CHECK_FALSE(error.empty());

    // A job that selected the ID for several settings may pin it only when every one of them is
    // declared, so one undeclared purpose refuses the whole pin.
    CHECK_FALSE(static_cast<bool>(InfillFactory::select_infill(
        id, plugin_key, capability,
        {InfillSurfacePurpose::TopSurface, InfillSurfacePurpose::Sparse}, error)));
    CHECK_FALSE(error.empty());

    InfillFactory::Selection selection = InfillFactory::select_infill(
        id, plugin_key, capability,
        {InfillSurfacePurpose::TopSurface, InfillSurfacePurpose::BottomSurface}, error);
    CHECK(static_cast<bool>(selection));
    selection.reset();

    InfillFactory::unregister_plugin_infills(plugin_key, capability);
}

TEST_CASE("A plugin pattern without a supported surface cannot be registered", "[InfillFactory]")
{
    CHECK_FALSE(InfillFactory::register_plugin_infill(
        "test-no-surface-pattern", "No Surface", "test-no-surface-plugin", "No Surface Infill", {}, {},
        []() -> std::unique_ptr<Fill> { return {}; }));
    CHECK_FALSE(InfillFactory::is_infill_registered("test-no-surface-pattern"));
}
