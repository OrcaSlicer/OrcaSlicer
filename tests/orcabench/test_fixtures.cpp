#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Catalog.hpp"
#include "core/Workload.hpp"
#include "slicer/Fixtures.hpp"

#include "orcabench_slicer_test_utils.hpp"
#include "test_utils.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/operations.hpp>

#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Bench;
using namespace Slic3r::Bench::Test;
using Catch::Matchers::EndsWith;
using Catch::Matchers::WithinAbs;

TEST_CASE("a procedural fixture is one object with one instance, centered on the bed", "[OrcaBench][Fixtures]")
{
    const std::string shape = GENERATE(as<std::string> {}, "smoke-cube", "peg-grid", "fine-sphere");
    CAPTURE(shape);
    const FixtureModel fixture = load_fixture("procedural:" + shape);
    REQUIRE(fixture.model);
    REQUIRE(fixture.model->objects.size() == 1);
    CHECK(fixture.model->objects.front()->instances.size() == 1);
    const BoundingBoxf3 box = fixture.model->bounding_box_exact();
    CHECK_THAT(box.center().x(), WithinAbs(175., 1e-6));
    CHECK_THAT(box.center().y(), WithinAbs(175., 1e-6));
    CHECK_THAT(box.min.z(), WithinAbs(0., 1e-6));
}

TEST_CASE("the peg grid is a plate of pegs and the fine sphere a sphere of many triangles", "[OrcaBench][Fixtures]")
{
    const FixtureModel pegs = load_fixture("procedural:peg-grid");
    REQUIRE(pegs.model);
    const Vec3d plate = pegs.model->bounding_box_exact().size();
    CHECK_THAT(plate.x(), WithinAbs(64., 1e-6));
    CHECK_THAT(plate.y(), WithinAbs(64., 1e-6));
    CHECK_THAT(plate.z(), WithinAbs(14., 1e-6));

    const FixtureModel sphere = load_fixture("procedural:fine-sphere");
    REQUIRE(sphere.model);
    CHECK_THAT(sphere.model->bounding_box_exact().size().z(), WithinAbs(50., 1e-3));
    CHECK(sphere.model->objects.front()->volumes.front()->mesh().facets_count() > 100000);
}

TEST_CASE("a handy fixture reads the model the app offers, and a missing one names the path it looked for", "[OrcaBench][Fixtures]")
{
    const TreeResources resources;
    const ScopedSlic3rTemporaryDir temporary("orcabench");
    const FixtureModel  voron = load_fixture("handy:Voron_Design_Cube_v7.drc");
    REQUIRE(voron.model);
    CHECK_FALSE(voron.model->objects.empty());
    CHECK(voron.unavailable.empty());

    const FixtureModel badge = load_fixture("handy:OrcaBadge.3mf");
    REQUIRE(badge.model);
    CHECK(badge.model->objects.size() == 3);

    const FixtureModel missing = load_fixture("handy:missing.drc");
    CHECK_FALSE(missing.model);
    CHECK_THAT(missing.unavailable, EndsWith("missing.drc is missing"));
}

TEST_CASE("a 3MF fixture keeps no backup folder", "[OrcaBench][Fixtures]")
{
    const TreeResources            resources;
    const ScopedSlic3rTemporaryDir temporary("orcabench");
    const FixtureModel             badge = load_fixture("handy:OrcaBadge.3mf");
    REQUIRE(badge.model);
    std::vector<std::string> files;
    for (const auto& entry : boost::filesystem::recursive_directory_iterator(temporary.path()))
        if (boost::filesystem::is_regular_file(entry.status()))
            files.push_back(entry.path().string());
    CHECK(files.empty());
}

TEST_CASE("a handy fixture's file is the model under handy_models, and a procedural fixture has none", "[OrcaBench][Fixtures]")
{
    const TreeResources              resources;
    const std::optional<std::string> benchy = fixture_path("handy:3DBenchy.drc");
    REQUIRE(benchy);
    CHECK(boost::filesystem::exists(*benchy));
    CHECK(boost::filesystem::path(*benchy).filename() == "3DBenchy.drc");
    CHECK(boost::filesystem::path(*benchy).parent_path().filename() == "handy_models");
    CHECK_FALSE(fixture_path("procedural:smoke-cube"));
    CHECK_THROWS_AS(fixture_path("smoke-cube"), std::invalid_argument);
}

TEST_CASE("a fixture id that names no fixture is refused", "[OrcaBench][Fixtures]")
{
    const std::string id = GENERATE(as<std::string> {}, "smoke-cube", "procedural:sphere", "handy");
    CAPTURE(id);
    CHECK_THROWS_AS(load_fixture(id), std::invalid_argument);
}

TEST_CASE("the hermetic config sets the bed, the layer change G-code and no object labels, then the entry's keys", "[OrcaBench][Fixtures]")
{
    const DynamicPrintConfig config = hermetic_config({{"layer_height", "0.3"}, {"z_hop_types", "Spiral Lift"}, {"nozzle_type", "brass"}});
    CHECK(config.opt_serialize("printable_area") == "0x0,350x0,350x350,0x350");
    CHECK(config.opt_string("layer_change_gcode") == "G92 E0");
    CHECK_FALSE(config.opt_bool("gcode_label_objects"));
    CHECK_THAT(config.opt_float("printable_height"), WithinAbs(350., 1e-9));
    CHECK_THAT(config.opt_float("layer_height"), WithinAbs(0.3, 1e-9));
    CHECK(config.opt_serialize("z_hop_types") == "Spiral Lift");
    CHECK(config.opt_serialize("nozzle_type") == "brass");
    CHECK(config.opt_serialize("retract_lift_enforce") == "All Surfaces");
    CHECK_THROWS(hermetic_config({{"no_such_key", "1"}}));
}

TEST_CASE("the hermetic config refuses a number that does not read whole, and takes one that does", "[OrcaBench][Fixtures]")
{
    const auto [key, value, read] = GENERATE(table<std::string, std::string, bool>({
        {"wall_loops", "2", true},
        {"wall_loops", "2.5", false},
        {"layer_height", "0.2", true},
        {"layer_height", "0.2mm", false},
        {"sparse_infill_density", "15%", true},
        {"sparse_infill_density", "15,5%", false},
        {"inner_wall_speed", "200,180", true},
        {"inner_wall_speed", "200mm/s", false},
        {"inner_wall_speed", "fast", false},
        {"inner_wall_speed", "200,", false},
    }));
    CAPTURE(key, value);
    if (read)
        CHECK_NOTHROW(hermetic_config({{key, value}}));
    else
        CHECK_THROWS_WITH(hermetic_config({{key, value}}), "the setting '" + key + "' cannot be '" + value + "'");
}

TEST_CASE("every fixture and config a slice in the catalog names loads", "[OrcaBench][Fixtures]")
{
    const TreeResources   resources;
    std::set<std::string> loaded;
    for (const CatalogEntry& entry : shipped_catalog("slicing.json")) {
        CAPTURE(entry.name);
        if (loaded.insert(entry.fixture).second) {
            const FixtureModel fixture = load_fixture(entry.fixture);
            CHECK(fixture.model);
            CHECK(fixture.unavailable.empty());
        }
        CHECK_NOTHROW(hermetic_config(entry.config));
    }
    CHECK(loaded.size() == 8);
}
