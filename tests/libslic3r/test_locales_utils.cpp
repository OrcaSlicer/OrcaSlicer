#include <catch2/catch_all.hpp>

#include <clocale>
#include <locale.h>

#include <catch2/catch_test_macros.hpp>
#include "libslic3r/LocalesUtils.hpp"

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
