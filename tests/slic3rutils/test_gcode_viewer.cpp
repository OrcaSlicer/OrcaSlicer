#include <catch2/catch_test_macros.hpp>

#include "libvgcode/include/Types.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"

#include <string>

TEST_CASE("Overhang preview selection round trips through saved preferences", "[GCodeViewer][Overhang][Regression]")
{
    using Slic3r::GUI::GCodeViewer;
    const std::string name = GCodeViewer::view_type_to_config_name(libvgcode::EViewType::Overhang);
    REQUIRE(name == "overhang");

    libvgcode::EViewType restored = libvgcode::EViewType::FeatureType;
    REQUIRE(GCodeViewer::view_type_from_config_name(name, restored));
    CHECK(restored == libvgcode::EViewType::Overhang);
}
