#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>
#include <vector>

#include "libslic3r/Color.hpp"
#include "libslic3r/TransmissionDistance.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

TEST_CASE("A transmission distance is looked up by colour, or not at all", "[ChromaStack]")
{
    // A stand-in for the shipped table, so the matching is tested rather than the resources path.
    // Three colours of one line, spread the way a real line is, plus another brand.
    const std::vector<FilamentTd> table = {
        { "polymaker", "Panchroma Matte PLA Cotton White", "PLA", ColorRGB(0.94f, 0.93f, 0.90f), 4.1f },
        { "polymaker", "Panchroma Matte PLA Army Blue", "PLA", ColorRGB(0.22f, 0.29f, 0.37f), 0.1f },
        { "polymaker", "Panchroma Matte PLA Lava Red", "PLA", ColorRGB(0.72f, 0.16f, 0.14f), 0.8f },
        { "prusament", "Prusament PLA Galaxy Black", "PLA", ColorRGB(0.11f, 0.11f, 0.13f), 0.4f },
    };

    SECTION("the vendor and the colour together pick one spool out of the line")
    {
        const FilamentTd *found = filament_td_lookup(table, "Polymaker", "Panchroma PLA Matte @base",
                                                 ColorRGB(0.93f, 0.92f, 0.89f));
        REQUIRE(found != nullptr);
        REQUIRE_THAT(found->td_mm, WithinAbs(4.1f, 1e-4f));

        found = filament_td_lookup(table, "Polymaker", "Panchroma PLA Matte @base", ColorRGB(0.23f, 0.30f, 0.36f));
        REQUIRE(found != nullptr);
        REQUIRE_THAT(found->td_mm, WithinAbs(0.1f, 1e-4f));
    }

    SECTION("the vendor is matched through the name it sells filament under")
    {
        // The presets say Prusa Polymers; the filament is Prusament.
        const FilamentTd *found = filament_td_lookup(table, "Prusa Polymers", "Prusament PLA @base",
                                                 ColorRGB(0.10f, 0.10f, 0.12f));
        REQUIRE(found != nullptr);
        REQUIRE_THAT(found->td_mm, WithinAbs(0.4f, 1e-4f));
    }

    SECTION("a colour the line is not made in gets nothing rather than the nearest thing")
    {
        // This is the failure that matters: answering with a number measured for another spool is
        // worse than answering with nothing, because it looks measured.
        REQUIRE(filament_td_lookup(table, "Polymaker", "Panchroma PLA Matte @base", ColorRGB(1.f, 0.f, 1.f)) == nullptr);
    }

    SECTION("a vendor the table has never heard of gets nothing")
    {
        REQUIRE(filament_td_lookup(table, "Nonesuch Filaments", "Nonesuch PLA Matte",
                                 ColorRGB(0.94f, 0.93f, 0.90f)) == nullptr);
    }

    SECTION("one word in common is a coincidence, not a line")
    {
        REQUIRE(filament_td_lookup(table, "Polymaker", "PLA", ColorRGB(0.94f, 0.93f, 0.90f)) == nullptr);
    }

    SECTION("the right line in a colour of another line of the same brand is still refused")
    {
        // Lava Red is in the table, but not within reach of a blue, and the line matches either way.
        const FilamentTd *found = filament_td_lookup(table, "Polymaker", "Panchroma PLA Matte @base",
                                                 ColorRGB(0.20f, 0.60f, 0.95f));
        REQUIRE(found == nullptr);
    }
}
