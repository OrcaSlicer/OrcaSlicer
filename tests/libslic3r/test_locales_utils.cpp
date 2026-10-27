#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/LocalesUtils.hpp"
#include "test_utils.hpp"

#include <string>

using namespace Slic3r;

TEST_CASE("Floats print as printf prints them in the C locale", "[LocalesUtils]")
{
    const auto [value, precision, text] = GENERATE(table<double, int, std::string>({
        {0.5, -1, "0.5"},
        {25. / 3., -1, "8.33333"},
        {1500.5, -1, "1500.5"},
        {1e6, -1, "1e+06"},
        {-0.000123, -1, "-0.000123"},
        {25. / 3., 3, "8.333"},
        {2., 0, "2"},
        // Longer than the to_chars buffer.
        {1e21, 2, "1000000000000000000000.00"},
    }));
    CHECK(float_to_string_decimal_point(value, precision) == text);
}

TEST_CASE("Floats print with a decimal point in a locale whose decimal separator is a comma", "[LocalesUtils]")
{
    CommaNumericLocale comma;
    {
        CNumericLocalesSetter outer;
        if (! comma.apply())
            SKIP("no locale with a comma decimal separator is installed");
        CHECK(float_to_string_decimal_point(1500.5) == "1500.5");
        CHECK(float_to_string_decimal_point(25. / 3., 3) == "8.333");
    }
}

TEST_CASE("a setter nested in another leaves the C locale in place for the outer one", "[LocalesUtils]")
{
    CNumericLocalesSetter outer;
    {
        CNumericLocalesSetter inner;
        CHECK(is_decimal_separator_point());
    }
    CHECK(is_decimal_separator_point());
}

TEST_CASE("a setter nested in another sets C again when the locale changed between them", "[LocalesUtils]")
{
    CommaNumericLocale comma;
    {
        CNumericLocalesSetter outer;
        if (! comma.apply())
            SKIP("no locale with a comma decimal separator is installed");
        REQUIRE_FALSE(is_decimal_separator_point());
        {
            CNumericLocalesSetter inner;
            CHECK(is_decimal_separator_point());
        }
        CHECK_FALSE(is_decimal_separator_point());
    }
}

TEST_CASE("atof_decimal_point parses what atof parses in the C locale", "[LocalesUtils]")
{
    const auto [text, value] = GENERATE(table<const char*, double>({
        { "5", 5. },
        { "  12.5", 12.5 },
        { "\t+3", 3. },
        { "\r\n7", 7. },
        { "-1.25", -1.25 },
        { "1e2", 100. },
        { ".5", 0.5 },
        { "12.5;comment", 12.5 },
        { "+-5", 0. },
        { "", 0. },
        { "abc", 0. },
    }));
    INFO(text);
    CHECK_THAT(atof_decimal_point(text), Catch::Matchers::WithinAbs(value, 1e-12));
}
