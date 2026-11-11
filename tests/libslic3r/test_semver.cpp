#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/Semver.hpp"

#include <string>
#include <tuple>

using namespace Slic3r;

TEST_CASE("A dev build ranks above its alpha and beta and below its release", "[Semver]") {
    const auto [lower, higher] = GENERATE(table<std::string, std::string>({
        { "2.5.0-alpha", "2.5.0-dev" },
        { "2.5.0-beta",  "2.5.0-dev" },
        { "2.5.0-rc",    "2.5.0-dev" },
        { "2.5.0-alpha", "2.5.0-beta" },
        { "2.5.0-dev",   "2.5.0" },
        { "2.4.2",       "2.5.0-dev" },
        { "2.5.0",       "2.6.0-dev" },
    }));
    INFO(lower << " < " << higher);
    const Semver lo(lower);
    const Semver hi(higher);
    CHECK(lo < hi);
    CHECK(hi > lo);
    CHECK_FALSE(hi < lo);
}

TEST_CASE("Two dev builds of the same version are equal", "[Semver]") {
    CHECK(Semver("2.5.0-dev") == Semver("2.5.0-dev"));
}

TEST_CASE("Versions print with the patch split in two, or as three parts", "[Semver]") {
    // to_string() prints the patch as patch / 100 and patch % 100; to_string_sf() prints it whole.
    const Semver release(2, 5, 103);
    CHECK(release.to_string() == "2.5.1.3");
    CHECK(release.to_string_sf() == "2.5.103");

    const Semver dev(2, 6, 0, nullptr, "dev");
    CHECK(dev.to_string() == "2.6.0.0-dev");
    CHECK(dev.to_string_sf() == "2.6.0-dev");

    const Semver tagged(1, 2, 3, "build7");
    CHECK(tagged.to_string() == "1.2.0.3+build7");
    CHECK(tagged.to_string_sf() == "1.2.3+build7");
}
