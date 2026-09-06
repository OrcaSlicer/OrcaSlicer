#include <catch2/catch_all.hpp>

#include <cmath>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Set up a single centred cube without going through Test::init_print(), whose arrange step
// throws "Objects could not fit on the bed" in this checkout for every [PrintObject] test.
static void process_cube(Slic3r::Print &print, Slic3r::Model &model,
                         std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict(config_items);

    ModelObject *object = model.add_object();
    object->name = "cube.stl";
    object->add_volume(Slic3r::Test::mesh(Slic3r::Test::TestMesh::cube_20x20x20));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);

    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
}

// Extruded length of sparse infill per layer index. This is what actually reaches the
// G-code, so it proves the density override is honoured by Fill.cpp and not merely recorded.
static std::vector<double> sparse_infill_length(const Print &print)
{
    const PrintObject *object = print.objects().front();
    std::vector<double> per_layer(object->layers().size(), 0.);
    for (size_t i = 0; i < object->layers().size(); ++ i)
        for (const LayerRegion *layerm : object->layers()[i]->regions())
            for (const ExtrusionEntity *entity : layerm->fills.flatten().entities)
                if (entity->role() == erInternalInfill)
                    per_layer[i] += unscale<double>(entity->length());
    return per_layer;
}

// Density overrides assigned by PrintObject::gradual_infill(), per layer index.
static std::vector<std::vector<float>> collect_overrides(const Print &print)
{
    const PrintObject *object = print.objects().front();
    std::vector<std::vector<float>> per_layer(object->layers().size());
    for (size_t i = 0; i < object->layers().size(); ++ i)
        for (const LayerRegion *layerm : object->layers()[i]->regions())
            for (const Surface &surface : layerm->fill_surfaces.surfaces)
                if (surface.density_override >= 0.f)
                    per_layer[i].emplace_back(surface.density_override);
    return per_layer;
}

} // namespace

SCENARIO("Gradual infill below top shells", "[GradualInfill]") {
    const double base = 0.10;

    GIVEN("A 20mm cube at 10% rectilinear infill") {
        WHEN("max_infill_bridge_length is 0 (feature off)") {
            Slic3r::Print print; Slic3r::Model model;
            process_cube(print, model, {
                { "sparse_infill_density",     10 },
                { "sparse_infill_pattern",     "rectilinear" },
                { "top_shell_layers",          3 },
                { "max_infill_bridge_length",  0 },
            });
            const auto overrides = collect_overrides(print);
            THEN("no fill surface carries a density override") {
                for (const auto &layer : overrides)
                    REQUIRE(layer.empty());
            }
        }

        WHEN("max_infill_bridge_length is 2mm") {
            Slic3r::Print print; Slic3r::Model model;
            process_cube(print, model, {
                { "sparse_infill_density",     10 },
                { "sparse_infill_pattern",     "rectilinear" },
                { "top_shell_layers",          3 },
                { "max_infill_bridge_length",  2 },
                { "sparse_infill_top_offset",  2 },
            });
            const auto overrides = collect_overrides(print);

            size_t densified_layers = 0, topmost = 0, bottommost = overrides.size();
            for (size_t i = 0; i < overrides.size(); ++ i)
                if (! overrides[i].empty()) {
                    ++ densified_layers;
                    topmost = std::max(topmost, i);
                    bottommost = std::min(bottommost, i);
                }

            THEN("some sparse infill is densified") {
                REQUIRE(densified_layers > 0);
            }
            AND_THEN("every density is a doubling of the configured one, so each added line "
                     "sits on the pattern the layer below already prints") {
                for (const auto &layer : overrides)
                    for (float density : layer) {
                        const double doublings = std::log2(double(density) / base);
                        REQUIRE(doublings >= 1.0 - EPSILON);
                        REQUIRE(std::abs(doublings - std::round(doublings)) < 1e-4);
                    }
            }
            AND_THEN("the densified layers sit under the top shell, not throughout the object") {
                REQUIRE(bottommost > overrides.size() / 2);
                REQUIRE(densified_layers < overrides.size() / 2);
            }
            AND_THEN("nothing is densified inside the top solid shell itself") {
                REQUIRE(topmost + 1 < overrides.size());
            }
            AND_THEN("the extra density reaches the G-code: the densified layers really do "
                     "extrude more sparse infill, and no other layer is touched") {
                Slic3r::Print baseline_print; Slic3r::Model baseline_model;
                process_cube(baseline_print, baseline_model, {
                    { "sparse_infill_density",     10 },
                    { "sparse_infill_pattern",     "rectilinear" },
                    { "top_shell_layers",          3 },
                    { "max_infill_bridge_length",  0 },
                });
                const std::vector<double> baseline = sparse_infill_length(baseline_print);
                const std::vector<double> densified = sparse_infill_length(print);
                REQUIRE(baseline.size() == densified.size());
                for (size_t i = 0; i < baseline.size(); ++ i) {
                    if (overrides[i].empty())
                        REQUIRE_THAT(densified[i], Catch::Matchers::WithinRel(baseline[i], 1e-9));
                    else
                        REQUIRE(densified[i] > baseline[i] * 1.5);
                }
            }
        }

        WHEN("the pattern is one whose lines do not nest when the density doubles") {
            Slic3r::Print print; Slic3r::Model model;
            process_cube(print, model, {
                { "sparse_infill_density",     10 },
                { "sparse_infill_pattern",     "concentric" },
                { "top_shell_layers",          3 },
                { "max_infill_bridge_length",  2 },
            });
            const auto overrides = collect_overrides(print);
            THEN("the feature leaves it alone") {
                for (const auto &layer : overrides)
                    REQUIRE(layer.empty());
            }
        }
    }
}
