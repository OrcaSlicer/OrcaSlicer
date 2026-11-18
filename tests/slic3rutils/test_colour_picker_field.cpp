#include "slic3r/GUI/Field.hpp"
#include "libslic3r/Config.hpp"
#include <boost/any.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <wx/colour.h>
#include <wx/string.h>

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {
class TestColourPicker : public ColourPicker {
public:
    TestColourPicker() : ColourPicker(ConfigOptionDef{}, "filament_colour") {}
    std::string value() { return boost::any_cast<std::string>(get_value()); }
};
}

TEST_CASE("Undefined color fields remain distinct from opaque black", "[ColourPickerField]")
{
    TestColourPicker field;
    REQUIRE(field.value().empty());
    field.set_value(std::string("#000000"));
    REQUIRE(field.value() == "#000000");
    field.set_value(std::string("invalid color"));
    REQUIRE(field.value().empty());
    field.set_value(boost::any(wxString("#123456")));
    REQUIRE(field.value() == "#123456");
    field.set_value(std::string());
    REQUIRE(field.value().empty());
}

TEST_CASE("Programmatic color field assignment stays silent", "[ColourPickerField]")
{
    TestColourPicker field;
    int changes = 0;
    field.m_on_change = [&](const std::string&, const boost::any&) { ++changes; };
    field.set_value(std::string("#123456"), false);
    field.set_value(boost::any(wxString("#654321")), true);
    REQUIRE(changes == 0);
    REQUIRE(field.value() == "#654321");
    field.m_disable_change_event = true;
    field.set_value(std::string("#FF0000"));
    REQUIRE(field.value() == "#FF0000");
    REQUIRE(changes == 0);
}
