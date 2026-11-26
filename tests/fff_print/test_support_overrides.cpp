#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/SupportOverrides.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <math.h>
#include <utility>
#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

TEST_CASE("Disabling object support overrides preserves unrelated settings", "[SupportOverrides]")
{
    Model model;
    auto* object = model.add_object();
    object->config.set_key_value("enable_support", new ConfigOptionBool(true));
    object->config.set_key_value("enforce_support_layers", new ConfigOptionInt(5));
    object->config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stTree));
    object->config.set_key_value("raft_layers", new ConfigOptionInt(3));
    object->config.set_key_value("wall_loops", new ConfigOptionInt(4));
    const auto original_timestamp = static_cast<const ModelConfig&>(object->config).timestamp();
    auto* other                   = model.add_object();
    other->config.set_key_value("enable_support", new ConfigOptionBool(true));
    auto* disabled = model.add_object();
    disabled->config.set_key_value("enable_support", new ConfigOptionBool(false));
    model.add_object(); // An object with no local support setting inherits the global value.

    REQUIRE(GUI::has_enabled_support_overrides(model));
    REQUIRE(GUI::clear_enabled_support_overrides(model));
    CHECK_FALSE(object->config.has("enable_support"));
    CHECK_FALSE(other->config.has("enable_support"));
    CHECK(disabled->config.has("enable_support"));
    CHECK_FALSE(disabled->config.get().opt_bool("enable_support"));
    CHECK(object->config.opt_int("enforce_support_layers") == 5);
    CHECK(object->config.opt_int("raft_layers") == 3);
    CHECK(object->config.opt_int("wall_loops") == 4);
    CHECK(object->config.get().opt_enum<SupportType>("support_type") == stTree);
    CHECK(static_cast<const ModelConfig&>(object->config).timestamp() != original_timestamp);
    CHECK_FALSE(GUI::has_enabled_support_overrides(model));
    CHECK_FALSE(GUI::clear_enabled_support_overrides(model));
}

TEST_CASE("Disabled local support overrides remain intentional", "[SupportOverrides]")
{
    Model model;
    auto* object = model.add_object();
    object->config.set_key_value("enable_support", new ConfigOptionBool(false));
    object->config.set_key_value("enforce_support_layers", new ConfigOptionInt(0));
    const auto original_timestamp = static_cast<const ModelConfig&>(object->config).timestamp();
    CHECK_FALSE(GUI::has_enabled_support_overrides(model));
    CHECK_FALSE(GUI::clear_enabled_support_overrides(model));
    CHECK(object->config.has("enable_support"));
    CHECK(static_cast<const ModelConfig&>(object->config).timestamp() == original_timestamp);
}

TEST_CASE("Clearing imported object support overrides stops tree generation", "[SupportOverrides][SupportMaterial]")
{
    TriangleMesh mesh = Test::mesh(TestMesh::cube_with_hole);
    mesh.rotate_x(float(M_PI / 2));
    const bool manual = GENERATE(false, true);
    Model model;
    Print print;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"enable_support", false},
                                   {"enforce_support_layers", 0},
                                   {"raft_layers", 0},
                                   {"support_type", "tree(auto)"},
                                   {"support_style", "organic"},
                                   {"layer_height", 0.2}});
    init_print({mesh}, print, model, config);
    model.objects.front()->config.set_key_value("enable_support", new ConfigOptionBool(true));
    if (manual) {
        model.objects.front()->config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stTree));
        auto* part            = model.objects.front()->volumes.front();
        TriangleMesh enforcer = part->mesh().convex_hull_3d();
        auto* volume          = model.objects.front()->add_volume(std::move(enforcer), ModelVolumeType::SUPPORT_ENFORCER, false);
        volume->set_transformation(part->get_transformation());
    }
    const auto volume_count = model.objects.front()->volumes.size();
    print.apply(model, config);
    print.process();
    REQUIRE(print.objects().front()->has_support());
    REQUIRE_FALSE(print.objects().front()->support_layers().empty());

    REQUIRE(GUI::clear_enabled_support_overrides(model));
    print.apply(model, config);
    print.process();
    CHECK_FALSE(print.objects().front()->has_support());
    CHECK(print.objects().front()->support_layers().empty());
    CHECK(model.objects.front()->volumes.size() == volume_count);
    if (manual)
        CHECK(model.objects.front()->volumes.back()->is_support_enforcer());
}
