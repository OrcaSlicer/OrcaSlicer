#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <vector>
#include <cstddef>

#include "libslic3r/Slicing.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

// Profiles are flat lists of (z, layer height) pairs.
static void check_profile(const std::vector<coordf_t> &profile, const std::vector<coordf_t> &expected)
{
    REQUIRE(profile.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        INFO("entry " << i);
        CHECK_THAT(profile[i], Catch::Matchers::WithinAbs(expected[i], 1e-9));
    }
}

TEST_CASE("A layer height profile fitted to a lower object is cut at its top", "[Slicing]")
{
    // 0.2 mm up to 5 mm, thinning to 0.1 mm at 10 mm: halfway through the ramp, 7.5 mm, is 0.15 mm.
    const std::vector<coordf_t> profile = { 0., 0.2, 5., 0.2, 10., 0.1, 20., 0.1 };
    check_profile(layer_height_profile_fit_to_height(profile, 7.5), { 0., 0.2, 5., 0.2, 7.5, 0.15 });
}

TEST_CASE("A layer height profile fitted to a taller object carries its last layer height up", "[Slicing]")
{
    const std::vector<coordf_t> profile = { 0., 0.2, 5., 0.2, 10., 0.1 };
    check_profile(layer_height_profile_fit_to_height(profile, 30.), { 0., 0.2, 5., 0.2, 10., 0.1, 30., 0.1 });
}

TEST_CASE("A layer height profile fitted to its own height is unchanged", "[Slicing]")
{
    const std::vector<coordf_t> profile = { 0., 0.2, 5., 0.2, 10., 0.1 };
    check_profile(layer_height_profile_fit_to_height(profile, 10.), profile);
}

TEST_CASE("Merged layer height profiles take the finest layer height up to the top of each object", "[Slicing]")
{
    // A 20 mm object at a constant 0.2 mm, and a 10 mm one thinning to 0.1 mm at 5 mm. The 0.3 mm
    // entry closing the lower profile is not a layer height the object asks for.
    const std::vector<coordf_t> tall   = { 0., 0.2, 10., 0.2, 20., 0.2 };
    const std::vector<coordf_t> lower  = { 0., 0.2, 5., 0.1, 10., 0.3 };
    const std::vector<coordf_t> merged = layer_height_profile_merge_finest({ tall, lower });

    REQUIRE(merged.size() >= 4);
    REQUIRE(merged.size() % 2 == 0);
    CHECK_THAT(merged[0], Catch::Matchers::WithinAbs(0., 1e-9));
    CHECK_THAT(merged[1], Catch::Matchers::WithinAbs(0.2, 1e-9));
    CHECK_THAT(merged[merged.size() - 2], Catch::Matchers::WithinAbs(20., 1e-9));
    for (size_t i = 2; i + 2 < merged.size(); i += 2) {
        INFO("entry at z = " << merged[i]);
        // One entry per layer: each starts where the layer before it ended.
        if (i + 4 < merged.size())
            CHECK_THAT(merged[i + 2], Catch::Matchers::WithinAbs(merged[i] + merged[i + 1], 1e-9));
        if (merged[i] < 5.)
            // Thinning with the lower object: 0.2 mm at the bed, 0.1 mm at 5 mm.
            CHECK_THAT(merged[i + 1], Catch::Matchers::WithinAbs(0.2 - 0.02 * merged[i], 1e-9));
        else if (merged[i] < 10.)
            CHECK_THAT(merged[i + 1], Catch::Matchers::WithinAbs(0.1, 1e-9));
        else {
            // Above the lower object the layers grow back to the 0.2 mm of the tall one, by no
            // more than the 0.04 mm the adaptive profile allows from one layer to the next.
            CHECK(merged[i + 1] <= 0.2 + 1e-9);
            CHECK(merged[i + 1] - merged[i - 1] <= 0.04 + 1e-9);
        }
        if (merged[i] > 11.)
            CHECK_THAT(merged[i + 1], Catch::Matchers::WithinAbs(0.2, 1e-9));
    }
}
