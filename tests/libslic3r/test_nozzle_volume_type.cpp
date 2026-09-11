#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

TEST_CASE("convert_to_nvt_type maps extruder variant strings to nozzle volume types", "[Config]")
{
    SECTION("Direct Drive variants") {
        REQUIRE(convert_to_nvt_type("Direct Drive Standard") == nvtStandard);
        REQUIRE(convert_to_nvt_type("Direct Drive High Flow") == nvtHighFlow);
        REQUIRE(convert_to_nvt_type("Direct Drive TPU High Flow") == nvtTPUHighFlow);
    }

    SECTION("Bowden variants") {
        REQUIRE(convert_to_nvt_type("Bowden Standard") == nvtStandard);
        REQUIRE(convert_to_nvt_type("Bowden High Flow") == nvtHighFlow);
    }

    SECTION("Unparsable strings fall back to hybrid") {
        REQUIRE(convert_to_nvt_type("Unknown Extruder") == nvtHybrid);
        REQUIRE(convert_to_nvt_type("") == nvtHybrid);
        REQUIRE(convert_to_nvt_type("High Flow") == nvtHybrid);
        REQUIRE(convert_to_nvt_type("Direct Drive") == nvtHybrid);
    }

    SECTION("Whitespace around the volume-type remainder is trimmed") {
        REQUIRE(convert_to_nvt_type("Direct Drive  High Flow ") == nvtHighFlow);
        REQUIRE(convert_to_nvt_type(" Bowden Standard") == nvtStandard);
    }
}
