#include <catch2/catch_all.hpp>

// MultiMaterialSegmentation.hpp declares boost::polygon traits for ColoredLine, so its
// geometry/boost dependencies must be included first.
#include <boost/polygon/polygon.hpp>
#include "libslic3r/Line.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

TEST_CASE("Multi-material segmentation resolves the outer-wall line width", "[MultiMaterialSegmentation][Regression]")
{
    struct Case
    {
        std::string         description;
        double              outer_value;
        bool                outer_percent;
        double              line_value;
        bool                line_percent;
        std::vector<double> nozzle_diameters;
        int                 outer_wall_filament_id;
        double              expected;
    };

    auto c = GENERATE(values<Case>({
        {"absolute outer-wall width is used as-is",      0.6, false, 0.42, false, {0.4},      1, 0.6},
        {"percent outer-wall width uses the nozzle",     120, true,  0.42, false, {0.5},      1, 0.6},
        {"zero outer-wall width uses the line width",    0,   false, 0.5,  false, {0.4},      1, 0.5},
        {"zero outer-wall width uses a percent line",    0,   false, 100,  true,  {0.5},      1, 0.5},
        {"zero width falls back to auto",                0,   false, 0,    false, {0.4},      1, Flow::auto_extrusion_width(frExternalPerimeter, 0.4)},
        {"the auto fallback scales with the nozzle",     0,   false, 0,    false, {0.6},      1, Flow::auto_extrusion_width(frExternalPerimeter, 0.6)},
        {"a percent width uses the outer wall's nozzle", 120, true,  0.42, false, {0.4, 0.8}, 2, 0.96},
        {"the auto width uses the outer wall's nozzle",  0,   false, 0,    false, {0.4, 0.8}, 2, Flow::auto_extrusion_width(frExternalPerimeter, 0.8)},
        {"an absolute width ignores the nozzle",         0.6, false, 0.42, false, {0.4, 0.8}, 2, 0.6},
        {"a zero percent width uses the line width",     0,   true,  0.5,  false, {0.4},      1, 0.5},
        {"an unset filament id uses the first nozzle",   0,   false, 0,    false, {0.4, 0.8}, 0, Flow::auto_extrusion_width(frExternalPerimeter, 0.4)},
        {"an out-of-range filament id uses nozzle 1",    0,   false, 0,    false, {0.4, 0.8}, 5, Flow::auto_extrusion_width(frExternalPerimeter, 0.4)},
    }));

    DYNAMIC_SECTION(c.description)
    {
        PrintConfig print_config;
        print_config.nozzle_diameter.values = c.nozzle_diameters;

        PrintObjectConfig object_config;
        object_config.line_width = ConfigOptionFloatOrPercent(c.line_value, c.line_percent);

        PrintRegionConfig region_config;
        region_config.outer_wall_line_width        = ConfigOptionFloatOrPercent(c.outer_value, c.outer_percent);
        region_config.outer_wall_filament_id.value = c.outer_wall_filament_id;

        REQUIRE_THAT(resolve_outer_wall_line_width(region_config, object_config, print_config),
                     Catch::Matchers::WithinAbs(c.expected, 1e-9));
    }
}
