#include <catch2/catch_all.hpp>

// MultiMaterialSegmentation.hpp declares boost::polygon traits for ColoredLine, so its
// geometry/boost dependencies must be included first.
#include <boost/polygon/polygon.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/Flow.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <cstddef>
#include <map>
#include <utility>

using namespace Slic3r;

namespace {

// Colors 2 and 3 are the two ends of a color gradient and 4 is the color between them. A texture or vertex color
// import gives each triangle the color nearest to its own, so the triangles in the middle of the gradient become a band of 4.
constexpr EnforcerBlockerType gradient_start = EnforcerBlockerType::Extruder2;
constexpr EnforcerBlockerType gradient_end   = EnforcerBlockerType::Extruder3;
constexpr EnforcerBlockerType between        = EnforcerBlockerType::Extruder4;
constexpr float               cell           = 0.45f; // mm

// A flat grid of 0.45 mm squares, each split into two triangles, painted by column:
//
//   4 4 2 2 2 4
//   4 4 2 2 2 4
//   4 4 2 2 2 4 3 3 3 3
//   4 4 2 2 2 4 3 3 3 3
//
// Column 5 is the band, two triangles wide and 0.81 mm^2. It shares 1.8 mm of border with color 2 and 0.9 mm with
// color 3. Columns 0 and 1 are a real region of color 4 (1.62 mm^2), color 2 covers 2.43 mm^2 and color 3 1.62 mm^2.
struct BandGrid
{
    Model                            model;
    ModelVolume                     *volume = nullptr;
    std::vector<EnforcerBlockerType> colors;
};

void make_band_grid(BandGrid &grid)
{
    indexed_triangle_set               its;
    std::map<std::pair<int, int>, int> vertex_ids;
    auto vertex = [&](int i, int j) {
        const auto [it, added] = vertex_ids.emplace(std::make_pair(i, j), int(its.vertices.size()));
        if (added)
            its.vertices.emplace_back(i * cell, j * cell, 0.f);
        return it->second;
    };
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 10; ++i) {
            if (i > 5 && j > 1)
                continue;
            const EnforcerBlockerType color = i < 2 ? between : i < 5 ? gradient_start : i == 5 ? between : gradient_end;
            its.indices.emplace_back(vertex(i, j), vertex(i + 1, j), vertex(i + 1, j + 1));
            its.indices.emplace_back(vertex(i, j), vertex(i + 1, j + 1), vertex(i, j + 1));
            grid.colors.insert(grid.colors.end(), 2, color);
        }
    grid.volume = grid.model.add_object()->add_volume(TriangleMesh(std::move(its)));
    TriangleSelector selector(grid.volume->mesh());
    for (size_t facet = 0; facet < grid.colors.size(); ++facet)
        selector.set_facet(int(facet), grid.colors[facet]);
    REQUIRE(grid.volume->mmu_segmentation_facets.set(selector));
}

// The color of each mesh triangle in `paint`. None of the test paint splits a triangle.
std::vector<EnforcerBlockerType> facet_colors(const ModelVolume &volume, const TriangleSelector::TriangleSplittingData &paint)
{
    TriangleSelector selector(volume.mesh());
    selector.deserialize(paint, false);
    std::vector<EnforcerBlockerType> colors(volume.mesh().its.indices.size(), EnforcerBlockerType::NONE);
    std::vector<int>                 sources;
    for (int state = 0; state <= int(EnforcerBlockerType::ExtruderMax); ++state) {
        selector.get_facets_strict(EnforcerBlockerType(state), &sources);
        for (const int source : sources)
            colors[source] = EnforcerBlockerType(state);
    }
    return colors;
}

// The band grid's colors with the band (column 5) repainted.
std::vector<EnforcerBlockerType> band_grid_colors_with_band(const BandGrid &grid, EnforcerBlockerType band_color)
{
    std::vector<EnforcerBlockerType> colors = grid.colors;
    const indexed_triangle_set      &its    = grid.volume->mesh().its;
    const float                      min_x  = grid.volume->mesh().bounding_box().min.x();
    for (size_t facet = 0; facet < colors.size(); ++facet) {
        const auto &t = its.indices[facet];
        const float x = (its.vertices[t[0]].x() + its.vertices[t[1]].x() + its.vertices[t[2]].x()) / 3.f - min_x;
        if (x > 5 * cell && x < 6 * cell)
            colors[facet] = band_color;
    }
    return colors;
}

} // namespace

TEST_CASE("A painted region smaller than the minimum area takes the color it shares the longest border with", "[MultiMaterialSegmentation]")
{
    BandGrid grid;
    make_band_grid(grid);
    // 1.2 mm^2 is above the band (0.81 mm^2) and below every other region (1.62 mm^2 and up).
    const auto merged = merge_small_painted_regions(*grid.volume, Transform3d::Identity(), 1.2);
    REQUIRE(facet_colors(*grid.volume, merged) == band_grid_colors_with_band(grid, gradient_start));
}

TEST_CASE("A painted region larger than the minimum area keeps its color", "[MultiMaterialSegmentation]")
{
    BandGrid grid;
    make_band_grid(grid);
    const auto merged = merge_small_painted_regions(*grid.volume, Transform3d::Identity(), 0.6);
    REQUIRE(facet_colors(*grid.volume, merged) == grid.colors);
}

TEST_CASE("The minimum painted region area is measured after the object's transformation", "[MultiMaterialSegmentation]")
{
    BandGrid grid;
    make_band_grid(grid);
    // Scaled 2x, the band covers 3.24 mm^2, above 1.2 mm^2.
    Transform3d trafo = Transform3d::Identity();
    trafo.scale(2.);
    const auto merged = merge_small_painted_regions(*grid.volume, trafo, 1.2);
    REQUIRE(facet_colors(*grid.volume, merged) == grid.colors);
}

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
