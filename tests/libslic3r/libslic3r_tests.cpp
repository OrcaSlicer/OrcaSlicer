#include <catch2/catch_all.hpp>

#include "libslic3r/Utils.hpp"
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
namespace {

TEST_CASE("sort_remove_duplicates", "[utils]") {
	std::vector<int> data_src = { 3, 0, 2, 1, 15, 3, 5, 6, 3, 1, 0 };
	std::vector<int> data_dst = { 0, 1, 2, 3, 5, 6, 15 };
	Slic3r::sort_remove_duplicates(data_src);
    REQUIRE(data_src == data_dst);
}

TEST_CASE("string_printf", "[utils]") {
    SECTION("Empty format with empty data should return empty string") {
        std::string outs = Slic3r::string_printf("");
        REQUIRE(outs.empty());
    }
    
    SECTION("String output length should be the same as input") {
        std::string outs = Slic3r::string_printf("1234");
        REQUIRE(outs.size() == 4);
    }
    
    SECTION("String format should be interpreted as with sprintf") {
        std::string outs = Slic3r::string_printf("%d %f %s", 10, 11.4, " This is a string");
        char buffer[1024];
        
        sprintf(buffer, "%d %f %s", 10, 11.4, " This is a string");
        
        REQUIRE(outs.compare(buffer) == 0);
    }
    
    SECTION("String format should survive large input data") {
        std::string input(2048, 'A');
        std::string outs = Slic3r::string_printf("%s", input.c_str());
        REQUIRE(outs.compare(input) == 0);
    }
}

}
