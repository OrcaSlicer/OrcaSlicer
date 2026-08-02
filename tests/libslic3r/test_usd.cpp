#include <algorithm>

#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/USD.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;

static inline std::string usd_path(const char *path)
{
    return std::string(TEST_DATA_DIR) + "/test_usd/" + path;
}

// Restored on the way out: resources_dir() is process-global and this binary is
// shared with every other suite.
struct UsdResourcesFixture
{
    UsdResourcesFixture() : previous(resources_dir()) { set_resources_dir(USD_PLUGIN_PARENT_DIR); }
    ~UsdResourcesFixture() { set_resources_dir(previous); }

    std::string previous;
};

SCENARIO_METHOD(UsdResourcesFixture, "Reading a USD file", "[usd]")
{
    GIVEN("a stage declaring metersPerUnit and a Y up-axis") {
        THEN("both are applied") {
            Model       model;
            std::string message;
            REQUIRE(load_usd(usd_path("cube_metres.usda").c_str(), &model, message));
            REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(),
                              Vec3d(10000, 10000, 10000)));

            // Position, not size: the inverse rotation gives identical extents
            // while placing the model under the plate.
            TriangleMesh mesh;
            REQUIRE(load_usd(usd_path("cube_yup.usda").c_str(), &mesh, message));
            REQUIRE(mesh.bounding_box().min.z() == Catch::Approx(0.0).margin(1e-4));
            REQUIRE(mesh.bounding_box().max.z() == Catch::Approx(10.0).margin(1e-4));
        }
    }

    // Drops to .usda alone and nothing covers the plugInfo staging any more.
    GIVEN("the same cube as text, as a binary crate and as a usdz package") {
        THEN("all three import identically") {
            for (const char *f : {"cube_none.usda", "cube_none.usdc", "cube_none.usdz"}) {
                Model       model;
                std::string message;
                REQUIRE(load_usd(usd_path(f).c_str(), &model, message));
                REQUIRE(model.objects.front()->volumes.front()->mesh().facets_count() == 12);
            }
        }
    }
}
