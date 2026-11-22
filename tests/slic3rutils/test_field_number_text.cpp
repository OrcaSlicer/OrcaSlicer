#include "slic3r/GUI/Field.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "test_utils.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <wx/numformatter.h>
#include <wx/string.h>

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

TEST_CASE("Numbers drop trailing zeros when the C locale writes a comma and the UI locale a point", "[Field]")
{
    CommaNumericLocale comma;
    CNumericLocalesSetter outer;
    if (!comma.apply())
        SKIP("no locale with a comma decimal separator is installed");
    if (wxNumberFormatter::GetDecimalSeparator() != '.')
        SKIP("the UI locale does not write a decimal point");

    CHECK(double_to_string(0.2) == wxString("0.2"));
    CHECK(double_to_string(0.42) == wxString("0.42"));
    CHECK(double_to_string(100.) == wxString("100"));
}

TEST_CASE("Field numbers parse back whichever decimal separator they show", "[Field]")
{
    CommaNumericLocale comma;
    CNumericLocalesSetter outer;
    if (!comma.apply())
        SKIP("no locale with a comma decimal separator is installed");

    double value = 0.;
    // As point fields write it, in the UI locale.
    REQUIRE(string_to_double_any_separator(wxNumberFormatter::ToString(0.5, 2, wxNumberFormatter::Style_None), &value));
    CHECK_THAT(value, WithinAbs(0.5, 1e-12));
    REQUIRE(string_to_double_any_separator("0.25", &value));
    CHECK_THAT(value, WithinAbs(0.25, 1e-12));
    REQUIRE(string_to_double_any_separator("0,25", &value));
    CHECK_THAT(value, WithinAbs(0.25, 1e-12));
    REQUIRE(string_to_double_any_separator("-1,5", &value));
    CHECK_THAT(value, WithinAbs(-1.5, 1e-12));
    // Accepted by ToDouble before, so typing it must keep working.
    REQUIRE(string_to_double_any_separator("0.", &value));
    CHECK_THAT(value, WithinAbs(0., 1e-12));
    CHECK_FALSE(string_to_double_any_separator("abc", &value));
}
