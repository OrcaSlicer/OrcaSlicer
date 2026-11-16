#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_helpers.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

namespace {

// A taper exercises thin features and bead-count transitions on every layer.
TriangleMesh tapered_plate()
{
    TriangleMesh mesh = make_cube(8., 16., 0.6);
    for (Vec3f& v : mesh.its.vertices)
        v.x() *= 0.001f + 0.999f * v.y() / 16.f;
    return mesh;
}

TriangleMesh thin_wall_plate()
{
    TriangleMesh mesh = tapered_plate();
    double x          = 10.;
    for (double width : {0.04, 0.08, 0.12, 0.18, 0.26, 0.38, 0.7, 1.1}) {
        TriangleMesh strip = make_cube(width, 10., 0.6);
        strip.translate(x, 0., 0.);
        mesh.merge(strip);
        x += 2.;
    }
    return mesh;
}

DynamicPrintConfig nozzle_config(double unused, double outer, double inner)
{
    auto config = multifilament_config(3);
    config.set_num_extruders(3);
    config.option<ConfigOptionFloats>("nozzle_diameter")->values = {unused, outer, inner};
    size_flush_to_nozzles(config);
    config.set_deserialize_strict({
        {"wall_generator", "arachne"},
        {"outer_wall_filament_id", 2},
        {"inner_wall_filament_id", 3},
        {"sparse_infill_filament_id", 2},
        {"internal_solid_filament_id", 2},
        {"top_surface_filament_id", 2},
        {"bottom_surface_filament_id", 2},
        {"layer_height", 0.1},
        {"initial_layer_print_height", 0.1},
        {"wall_loops", 2},
        {"alternate_extra_wall", false},
        {"only_one_wall_first_layer", false},
        {"only_one_wall_top", false},
        {"sparse_infill_density", "0%"},
        {"top_shell_layers", 0},
        {"bottom_shell_layers", 0},
        {"top_shell_thickness", 0.0},
        {"bottom_shell_thickness", 0.0},
        {"initial_layer_line_width", "100%"},
        {"line_width", "100%"},
        {"outer_wall_line_width", "100%"},
        {"inner_wall_line_width", "100%"},
        {"min_feature_size", "25%"},
        {"min_bead_width", "85%"},
        {"initial_layer_min_bead_width", "75%"},
        {"wall_transition_length", "100%"},
        {"wall_transition_filter_deviation", "25%"},
        {"elefant_foot_compensation", 0.0},
        {"detect_overhang_wall", false},
        {"enable_support", false},
        {"enable_prime_tower", false},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
    });
    return config;
}

// Compare emitted XY segments and their actual extrusion cross sections, independent of
// traversal direction/order. This does not compare G-code comments or configuration text.
using Segment  = std::array<double, 8>;
using Geometry = std::vector<std::vector<Segment>>;

void append_path(const ExtrusionPath& path, std::vector<Segment>& out)
{
    const Polyline polyline = path.as_polyline();
    for (size_t i = 1; i < polyline.points.size(); ++i) {
        const Point& a = polyline.points[i - 1];
        const Point& b = polyline.points[i];
        std::array<double, 2> start{unscale<double>(a.x()), unscale<double>(a.y())};
        std::array<double, 2> end{unscale<double>(b.x()), unscale<double>(b.y())};
        if (end < start)
            std::swap(start, end);
        out.push_back({start[0], start[1], end[0], end[1], path.width, path.height, path.mm3_per_mm, double(path.role())});
    }
}

void append_geometry(const ExtrusionEntityCollection& collection, std::vector<Segment>& out)
{
    for (const ExtrusionEntity* entity : collection.entities) {
        if (const auto* nested = dynamic_cast<const ExtrusionEntityCollection*>(entity))
            append_geometry(*nested, out);
        else if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity))
            append_path(*path, out);
        else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(entity))
            for (const auto& path : loop->paths)
                append_path(path, out);
        else if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(entity))
            for (const auto& path : multi->paths)
                append_path(path, out);
        else
            FAIL("Unexpected extrusion entity");
    }
}

Geometry sliced_geometry(const DynamicPrintConfig& config, bool fill)
{
    Print print;
    Model model;
    init_print({fill ? tapered_plate() : thin_wall_plate()}, print, model, config);
    print.set_status_silent();
    print.process();
    Geometry result;
    for (const Layer* layer : print.objects().front()->layers()) {
        result.emplace_back();
        for (const LayerRegion* region : layer->regions()) {
            if (!fill) {
                REQUIRE(print.objects().front()->config().wall_generator.value == PerimeterGeneratorType::Arachne);
                const int outer              = config.option<ConfigOptionInt>("outer_wall_filament_id")->value;
                const double expected_nozzle = config.option<ConfigOptionFloats>("nozzle_diameter")->values[outer - 1];
                REQUIRE_THAT(region->flow(frExternalPerimeter).nozzle_diameter(), WithinAbs(expected_nozzle, 1e-6));
            }
            append_geometry(fill ? region->fills : region->perimeters, result.back());
        }
        std::sort(result.back().begin(), result.back().end());
    }
    REQUIRE(result.size() >= 3);
    return result;
}

void require_same_geometry(const Geometry& left, const Geometry& right)
{
    REQUIRE(left.size() == right.size());
    for (size_t layer = 0; layer < left.size(); ++layer) {
        CAPTURE(layer);
        REQUIRE_FALSE(left[layer].empty());
        REQUIRE(left[layer].size() == right[layer].size());
        for (size_t segment = 0; segment < left[layer].size(); ++segment)
            for (size_t field = 0; field < left[layer][segment].size(); ++field)
                REQUIRE_THAT(left[layer][segment][field], WithinAbs(right[layer][segment][field], 1e-6));
    }
}

} // namespace

TEST_CASE("Arachne walls ignore unused installed nozzles", "[ActiveNozzle][Perimeters][Regression]")
{
    const double active = GENERATE(0.2, 0.4, 0.6);
    const int walls     = GENERATE(1, 2);
    CAPTURE(active, walls);
    auto reference = nozzle_config(active, active, active);
    reference.set_deserialize_strict({{"wall_loops", walls}});
    auto changed                                                     = reference;
    changed.option<ConfigOptionFloats>("nozzle_diameter")->values[0] = 0.1;
    require_same_geometry(sliced_geometry(reference, false), sliced_geometry(changed, false));
}

TEST_CASE("Distinct wall tools share only participating nozzle limits", "[ActiveNozzle][Perimeters][Regression]")
{
    const bool small_outer = GENERATE(false, true);
    CAPTURE(small_outer);
    auto reference                                                   = nozzle_config(0.4, small_outer ? 0.2 : 0.6, small_outer ? 0.6 : 0.2);
    auto changed                                                     = reference;
    changed.option<ConfigOptionFloats>("nozzle_diameter")->values[0] = 0.1;
    require_same_geometry(sliced_geometry(reference, false), sliced_geometry(changed, false));
}

TEST_CASE("Single walls ignore the unprinted inner wall nozzle", "[ActiveNozzle][Perimeters][Regression]")
{
    const double active = GENERATE(0.2, 0.4, 0.6);
    auto reference      = nozzle_config(active, active, active);
    reference.set_deserialize_strict({{"wall_loops", 1}});
    auto changed                                                     = reference;
    changed.option<ConfigOptionFloats>("nozzle_diameter")->values[2] = 0.1;
    require_same_geometry(sliced_geometry(reference, false), sliced_geometry(changed, false));
}

TEST_CASE("Single wall layer overrides ignore the unprinted inner nozzle", "[ActiveNozzle][Perimeters][Regression]")
{
    const double active = GENERATE(0.2, 0.4, 0.6);
    const bool first    = GENERATE(false, true);
    CAPTURE(active, first);
    auto reference = nozzle_config(active, active, active);
    reference.set_deserialize_strict(
        {{"only_one_wall_first_layer", first}, {"only_one_wall_top", !first}, {"top_shell_layers", 1}, {"bottom_shell_layers", 1}});
    auto changed                                                     = reference;
    changed.option<ConfigOptionFloats>("nozzle_diameter")->values[2] = 0.1;
    const auto original                                              = sliced_geometry(reference, false);
    const auto modified                                              = sliced_geometry(changed, false);
    const size_t layer                                               = first ? 0 : original.size() - 1;
    require_same_geometry({original[layer]}, {modified[layer]});
}

TEST_CASE("Dense concentric and spiral fills ignore unused installed nozzles", "[ActiveNozzle][Fill][Regression]")
{
    const double active       = GENERATE(0.2, 0.4, 0.6);
    const std::string pattern = GENERATE(std::string("concentric"), std::string("spiralinset"));
    CAPTURE(active, pattern);
    auto reference = nozzle_config(active, 0.4, active);
    reference.set_deserialize_strict({
        {"wall_generator", "classic"},
        {"wall_loops", 1},
        {"inner_wall_filament_id", 2},
        {"internal_solid_filament_id", 3},
        {"top_surface_filament_id", 3},
        {"bottom_surface_filament_id", 3},
        {"top_shell_layers", 3},
        {"bottom_shell_layers", 3},
        {"top_surface_pattern", pattern},
        {"bottom_surface_pattern", pattern},
        {"internal_solid_infill_pattern", pattern},
        {"top_surface_line_width", "100%"},
        {"internal_solid_infill_line_width", "100%"},
        {"detect_narrow_internal_solid_infill", false},
    });
    auto changed                                                     = reference;
    changed.option<ConfigOptionFloats>("nozzle_diameter")->values[0] = 0.1;
    require_same_geometry(sliced_geometry(reference, true), sliced_geometry(changed, true));
}

TEST_CASE("Narrow internal solid fill ignores unused installed nozzles", "[ActiveNozzle][Fill][Regression]")
{
    const double active = GENERATE(0.2, 0.4, 0.6);
    CAPTURE(active);
    auto reference = nozzle_config(active, 0.4, active);
    reference.set_deserialize_strict({
        {"wall_generator", "classic"},
        {"wall_loops", 2},
        {"inner_wall_filament_id", 2},
        {"internal_solid_filament_id", 3},
        {"top_surface_filament_id", 3},
        {"bottom_surface_filament_id", 3},
        {"top_shell_layers", 4},
        {"bottom_shell_layers", 4},
        {"top_surface_pattern", "rectilinear"},
        {"bottom_surface_pattern", "rectilinear"},
        {"internal_solid_infill_pattern", "rectilinear"},
        {"internal_solid_infill_line_width", "100%"},
        {"detect_narrow_internal_solid_infill", true},
    });
    auto changed                                                     = reference;
    changed.option<ConfigOptionFloats>("nozzle_diameter")->values[0] = 0.1;
    require_same_geometry(sliced_geometry(reference, true), sliced_geometry(changed, true));
}

TEST_CASE("The lower region union retains its conservative installed nozzle limit", "[ActiveNozzle][Perimeters][Regression]")
{
    const bool arachne         = GENERATE(false, true);
    const bool spiral          = GENERATE(false, true);
    const double installed_min = GENERATE(0.2, 0.6);
    CAPTURE(arachne, spiral, installed_min);
    PrintConfig print_config;
    PrintObjectConfig object_config;
    PrintRegionConfig region_config;
    print_config.nozzle_diameter.values  = {installed_min, 0.6};
    print_config.spiral_mode.value       = spiral;
    object_config.wall_generator.value   = arachne ? PerimeterGeneratorType::Arachne : PerimeterGeneratorType::Classic;
    object_config.min_feature_size.value = 25.;
    const Flow flow = Flow::new_from_config_width(frExternalPerimeter, ConfigOptionFloatOrPercent(0.6, false), 0.6, 0.1);
    PerimeterGenerator generator(nullptr, nullptr, 0.1, 0.05, flow, &region_config, &object_config, &print_config, spiral, 0., nullptr,
                                 nullptr, nullptr, nullptr);
    generator.layer_id = 1;
    ExPolygons slices;
    for (double width : {0.04, 0.1, 0.3}) {
        const double x = double(slices.size()) * 2.;
        slices.emplace_back(
            Polygon{{Point(scaled<coord_t>(x), 0), Point(scaled<coord_t>(x + width), 0),
                     Point(scaled<coord_t>(x + width), scaled<coord_t>(4.)), Point(scaled<coord_t>(x), scaled<coord_t>(4.))}});
    }
    const double threshold = installed_min * (arachne ? 0.25 : 1. / 3.);
    const auto expected    = opening_ex(slices, float(scale_(threshold / 2.)));
    const auto actual      = generator.printable_slices(slices);
    REQUIRE_FALSE(actual.empty());
    REQUIRE(diff_ex(expected, actual).empty());
    REQUIRE(diff_ex(actual, expected).empty());
    // The middle sliver survives only with the smaller installed nozzle. This deliberately
    // preserves the lower-union exception instead of claiming universal nozzle independence.
    CHECK(actual.size() == (installed_min < 0.3 ? 2 : 1));
}

TEST_CASE("Wall geometry responds to the participating nozzle", "[ActiveNozzle][Perimeters][Regression]")
{
    const auto small = sliced_geometry(nozzle_config(0.2, 0.2, 0.2), false);
    const auto large = sliced_geometry(nozzle_config(0.6, 0.6, 0.6), false);
    REQUIRE_FALSE(small.front().empty());
    REQUIRE_FALSE(large.front().empty());
    double small_volume = 0., large_volume = 0.;
    for (const auto& segment : small.front())
        small_volume += std::hypot(segment[2] - segment[0], segment[3] - segment[1]) * segment[6];
    for (const auto& segment : large.front())
        large_volume += std::hypot(segment[2] - segment[0], segment[3] - segment[1]) * segment[6];
    CHECK(std::abs(small_volume - large_volume) > 0.01);
}
