#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Format/ImportNames.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using namespace Slic3r;

TEST_CASE("Imported names preserve Unicode and XML escaping characters", "[ImportNames]")
{
    // Ampersands need XML escaping at serialization, not destructive renaming.
    const std::vector<std::string> names{u8"Шов & тело", u8"部品2", "Same", "Same", ""};
    CHECK(sanitize_import_names(names, "_") == names);
    for (const auto& name : names)
        CHECK_FALSE(has_unsupported_name_characters(name));
}

TEST_CASE("Each unsupported character accepts a complete replacement string", "[ImportNames]")
{
    // Multi-character replacement applies to each character, including adjacent ones.
    CHECK(sanitize_import_names({"Part/A:B"}, "_slash_") ==
          std::vector<std::string>{"Part_slash_A_slash_B"});
    CHECK(sanitize_import_names({"A//B"}, "xyz") == std::vector<std::string>{"AxyzxyzB"});
    CHECK(sanitize_import_names({"A<>:/\\|?*\"B"}, "") == std::vector<std::string>{"AB"});
}

TEST_CASE("Cleaning names resolves only newly introduced sibling collisions", "[ImportNames]")
{
    // Existing duplicates remain equal; a cleaned name cannot take an existing valid name.
    const auto names = sanitize_import_names({"A/B", "AB", "AB", "A:B", "A/B", "AB (2)"}, "");
    REQUIRE(names.size() == 6);
    CHECK(names[0] == "AB (3)");
    CHECK(names[1] == "AB");
    CHECK(names[2] == "AB");
    CHECK(names[3] == "AB (4)");
    CHECK(names[4] == names[0]);
    CHECK(names[5] == "AB (2)");
}

TEST_CASE("Names emptied by cleaning receive a usable fallback", "[ImportNames]")
{
    // An unnamed input stays under the importer's existing fallback policy.
    CHECK(sanitize_import_names({"///", "Part", ""}, "") ==
          std::vector<std::string>{"Part (2)", "Part", ""});
    CHECK(sanitize_import_names({"///"}, "", "Assembly/part") ==
          std::vector<std::string>{"Assemblypart"});
}

TEST_CASE("Name validation rejects unsupported replacement and malformed Unicode", "[ImportNames]")
{
    CHECK(valid_import_name_replacement("_slash_"));
    CHECK(valid_import_name_replacement(u8"替换"));
    CHECK_FALSE(valid_import_name_replacement(""));
    CHECK_FALSE(valid_import_name_replacement("x/y"));
    CHECK_FALSE(valid_import_name_replacement(std::string("x\x01", 2)));
    CHECK_THROWS_AS(sanitize_import_names({"A/B"}, "/"), std::invalid_argument);
    CHECK_THROWS_AS(sanitize_import_names({std::string("\xc3\x28", 2)}, "_"), std::invalid_argument);
    CHECK(has_unsupported_name_characters(std::string("x\x01", 2)));
    CHECK(has_unsupported_name_characters(u8"x\ufffe"));
    CHECK_FALSE(has_unsupported_name_characters("\t\n\r"));
}
