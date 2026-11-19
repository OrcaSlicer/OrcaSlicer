#include "slic3r/GUI/Field.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "test_utils.hpp"

#include <catch2/catch_test_macros.hpp>
#include <wx/numformatter.h>
#include <wx/string.h>

using namespace Slic3r;
using namespace Slic3r::GUI;

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
