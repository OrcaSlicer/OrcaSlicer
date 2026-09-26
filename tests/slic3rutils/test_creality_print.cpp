#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/CrealityPrint.hpp"

using namespace Slic3r;

TEST_CASE("CrealityPrint builds native-API URLs against the bare host, without any web-UI port", "[CrealityPrint]")
{
    CHECK(creality_print_make_url("http://192.168.1.50:4408", "info") == "http://192.168.1.50/info");
    CHECK(creality_print_make_url("192.168.1.50:4408", "info") == "http://192.168.1.50/info");
    CHECK(creality_print_make_url("https://printer.local:8443", "upload/foo.gcode") == "https://printer.local/upload/foo.gcode");

    // No port at all: unaffected.
    CHECK(creality_print_make_url("http://192.168.1.50", "info") == "http://192.168.1.50/info");
    CHECK(creality_print_make_url("192.168.1.50", "info") == "http://192.168.1.50/info");

    // A trailing slash on the host must not double up when joined with path.
    CHECK(creality_print_make_url("http://192.168.1.50:4408/", "info") == "http://192.168.1.50/info");
}
