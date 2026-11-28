#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Point.hpp"
#include <string>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"

#include <boost/any.hpp>

using namespace Slic3r;

TEST_CASE("Pasted volume syntax leaves the saved bed area intact pending confirmation", "[ExclusionVolume][GUI][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    const std::string area = "0x0,20x0,20x10,10x10,10x20,0x20";
    config.set_deserialize_strict("bed_exclude_area", area);
    const std::string pasted = "5..10;40x40,50x40,50x50,40x50";

    // OptionsGroup updates the config before Tab asks whether to copy the text.
    GUI::change_opt_value(config, "bed_exclude_area", boost::any(pasted));
    CHECK(config.option<ConfigOptionPoints>("bed_exclude_area")->serialize() == area);
    CHECK_FALSE(has_bed_exclude_volumes(config));

    append_bed_exclude_volumes(config, pasted);
    CHECK(config.opt_string("bed_exclude_volumes") == pasted);
    CHECK(config.option<ConfigOptionPoints>("bed_exclude_area")->serialize() == area);
}

TEST_CASE("The normal bed-area field still accepts arbitrary polygons and clearing", "[ExclusionVolume][GUI][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    const Pointfs polygon{Vec2d(0, 0), Vec2d(20, 0), Vec2d(20, 10), Vec2d(10, 10), Vec2d(10, 20), Vec2d(0, 20)};
    GUI::change_opt_value(config, "bed_exclude_area", boost::any(polygon));
    CHECK(config.option<ConfigOptionPoints>("bed_exclude_area")->values == polygon);
    CHECK_FALSE(has_bed_exclude_volumes(config));

    GUI::change_opt_value(config, "bed_exclude_area", boost::any(Pointfs{}));
    CHECK(config.option<ConfigOptionPoints>("bed_exclude_area")->values.empty());
}
