#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/SettingsIndex.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Config.hpp"
#include "slic3r/GUI/Search.hpp"
#include "libslic3r/PrintConfig.hpp"

TEST_CASE("Rule setting search indexes custom triggers and refreshes its choices", "[ConditionalSettingsSearch]")
{
    Slic3r::Search::Option multi;
    multi.key = L"1;multi_color";
    multi.type = Slic3r::Preset::TYPE_PRINT;
    multi.label = multi.label_local = L"Multicolor";
    Slic3r::Search::Option wall;
    wall.key = L"1;wall_loops";
    wall.type = Slic3r::Preset::TYPE_PRINT;
    wall.label = wall.label_local = L"Wall count";
    Slic3r::Search::OptionsSearcher searcher;
    searcher.set_printer_technology(Slic3r::ptFFF);
    searcher.init_options({wall, multi});
    REQUIRE(searcher.index().options().size() == 2);
    CHECK(searcher.index().option_at(0).opt_key() == "multi_color");
    REQUIRE(searcher.search("Multicolor", true, Slic3r::Preset::TYPE_PRINT));
    REQUIRE(searcher.size() == 1);
    CHECK(searcher.get_option(0).opt_key() == "multi_color");
    searcher.init_options({wall});
    CHECK(searcher.size() == 0);
    REQUIRE(searcher.search("Wall", true, Slic3r::Preset::TYPE_PRINT));
    REQUIRE(searcher.size() == 1);
    CHECK(searcher.get_option(0).opt_key() == "wall_loops");
    CHECK(searcher.index().all_options().size() == 1);
}
