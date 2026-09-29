#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/CrealityPrint.hpp"

using namespace Slic3r;

TEST_CASE("CrealityPrint recognizes a web-UI page in place of the native API's JSON", "[CrealityPrint]")
{
    CHECK(creality_print_looks_like_html_response("<!DOCTYPE html><html><head></head><body></body></html>"));
    CHECK(creality_print_looks_like_html_response("<HTML><BODY>Mainsail</BODY></HTML>"));

    CHECK_FALSE(creality_print_looks_like_html_response(R"({"model":"K2 Plus","mac":"AA:BB:CC:DD:EE:FF"})"));
    CHECK_FALSE(creality_print_looks_like_html_response(""));
    CHECK_FALSE(creality_print_looks_like_html_response("not json and not html either"));
}
