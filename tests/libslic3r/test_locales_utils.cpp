#include <catch2/catch_all.hpp>

#include <clocale>
#include <locale.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/LocalesUtils.hpp"

#include <string>

using namespace Slic3r;

namespace {

// Switches this thread's numeric locale to one whose decimal separator is a comma,
// when the system has one installed.
struct CommaNumericLocale
{
#ifdef _WIN32
    bool apply()
    {
        for (const char* name : { "de-DE", "German_Germany.1252" })
            if (std::setlocale(LC_NUMERIC, name) != nullptr)
                return true;
        return false;
    }
#else
    locale_t locale { (locale_t) 0 };
    bool apply()
    {
        for (const char* name : { "de_DE.UTF-8", "de_DE.utf8", "de_DE" })
            if ((locale = newlocale(LC_NUMERIC_MASK, name, (locale_t) 0)) != (locale_t) 0) {
                uselocale(locale);
                return true;
            }
        return false;
    }
    // Freed once the setters around apply() have put the thread's own locale back.
    ~CommaNumericLocale()
    {
        if (locale != (locale_t) 0)
            freelocale(locale);
    }
#endif
};

} // namespace

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
