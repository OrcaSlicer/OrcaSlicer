#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libvgcode/include/PathVertex.hpp"
#include "libvgcode/include/Types.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

// Guard values are part of the preview conversion contract. Walls convert their whole contour offset, which may pass
// a full bead, and read horizontal only at the cap; other lines are measured up to a full bead, which is a bridge.
TEST_CASE("Overhang angles convert walls continuously and saturate bridges", "[GCodeViewer][Overhang]")
{
    using libvgcode::EGCodeExtrusionRole;
    struct Case {
        const char         *name;
        EGCodeExtrusionRole role;
        float               percentage;
        float               width;
        float               height;
        float               z_distance;
        float               expected_degree;
    };
    constexpr float degrees = 57.29577951308232f;
    const std::array<Case, 10> cases{{
        {"fully unsupported infill without dimensions", EGCodeExtrusionRole::SolidInfill, 100.0f, 0.0f, 0.0f, 0.0f, 90.0f},
        {"infill above a full bead", EGCodeExtrusionRole::BridgeInfill, 120.0f, 0.4f, 0.2f, 0.2f, 90.0f},
        {"fully supported", EGCodeExtrusionRole::ExternalPerimeter, 0.0f, 0.4f, 0.2f, 0.2f, 0.0f},
        {"percentage below the supported range", EGCodeExtrusionRole::Perimeter, -10.0f, 0.4f, 0.2f, 0.2f, 0.0f},
        {"missing width", EGCodeExtrusionRole::ExternalPerimeter, 50.0f, 0.0f, 0.2f, 0.2f, 0.0f},
        {"missing vertical separation", EGCodeExtrusionRole::ExternalPerimeter, 50.0f, 0.4f, 0.0f, 0.0f, 0.0f},
        {"wall at a full bead", EGCodeExtrusionRole::ExternalPerimeter, 100.0f, 0.4f, 0.2f, 0.2f, std::atan(2.0f) * degrees},
        {"wall past a full bead", EGCodeExtrusionRole::OverhangPerimeter, 150.0f, 0.4f, 0.2f, 0.2f, std::atan(3.0f) * degrees},
        {"wall at the cap", EGCodeExtrusionRole::Perimeter, libvgcode::PathVertex::MAX_OVERHANG_PERCENTAGE, 0.4f, 0.2f, 0.2f, 90.0f},
        {"wall height used without slice spacing", EGCodeExtrusionRole::ExternalPerimeter, 150.0f, 0.4f, 0.3f, 0.0f, std::atan(2.0f) * degrees},
    }};

    for (const Case &test : cases) {
        DYNAMIC_SECTION(test.name) {
            libvgcode::PathVertex vertex;
            vertex.role                = test.role;
            vertex.overhang_percentage = test.percentage;
            vertex.width               = test.width;
            vertex.height              = test.height;
            vertex.overhang_z_distance = test.z_distance;
            CHECK_THAT(vertex.overhang_degree(), Catch::Matchers::WithinAbs(test.expected_degree, 1e-4));
            // The percentage view shows the unsupported share of the bead.
            CHECK_THAT(vertex.overhang_share(), Catch::Matchers::WithinAbs(std::clamp(test.percentage, 0.0f, 100.0f), 1e-6));
        }
    }
}

// Lines without overhang data, such as supports, are not fully supported lines.
TEST_CASE("Overhang preview tells lines without data from supported ones", "[GCodeViewer][Overhang]")
{
    libvgcode::PathVertex vertex;
    vertex.role  = libvgcode::EGCodeExtrusionRole::SupportMaterial;
    vertex.width = 0.4f;
    vertex.height = 0.2f;
    vertex.overhang_percentage = -1.0f;
    CHECK_FALSE(vertex.has_overhang());
    vertex.role = libvgcode::EGCodeExtrusionRole::ExternalPerimeter;
    vertex.overhang_percentage = 0.0f;
    CHECK(vertex.has_overhang());
    CHECK_THAT(vertex.overhang_degree(), Catch::Matchers::WithinAbs(0.0, 1e-6));
}
