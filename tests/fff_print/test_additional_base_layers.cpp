#include <catch2/catch_all.hpp>

#include "libslic3r/Layer.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Exception.hpp"

#include "test_data.hpp"

using namespace Slic3r::Test;
using namespace Slic3r;

// -----------------------------------------------------------------------
// Helper: init and process a print without arrange (avoids bed-fit issues)
// The upstream init_print calls arrange_objects with InfiniteBed which
// throws "Objects could not fit on the bed" in the current codebase.
// -----------------------------------------------------------------------
static void init_and_process_print_no_arrange(
    std::initializer_list<TestMesh> test_meshes,
    Slic3r::Print &print,
    std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    // Fix default config: relative E needs G92 E0 in layer_gcode to pass validation
    config.set_deserialize_strict({ { "use_relative_e_distances", "0" } });
    config.set_deserialize_strict(config_items);

    Slic3r::Model model;
    for (const TestMesh tm : test_meshes) {
        TriangleMesh m = mesh(tm);
        ModelObject *object = model.add_object();
        object->name += "object.stl";
        object->add_volume(std::move(m));
        ModelInstance *inst = object->add_instance();
        // Place at origin, on bed
        inst->set_offset(Vec3d(100., 100., 0.));
    }
    for (ModelObject *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }

    print.apply(model, config);
    // Validate before process to catch config errors early
    {
        StringObjectException warning;
        auto result = print.validate(&warning);
        if (! result.string.empty())
            throw Slic3r::RuntimeError("validate: " + result.string);
    }
    print.set_status_silent();
    try {
        print.process();
    } catch (const Slic3r::SlicingErrors &e) {
        std::string msg = "SlicingErrors:";
        for (const auto &err : e.errors_)
            msg += "\n  - " + std::string(err.what());
        throw Slic3r::RuntimeError(msg);
    }
}

// -----------------------------------------------------------------------
// SlicingParameters-level tests  (fast, no geometry)
// -----------------------------------------------------------------------

static SlicingParameters make_slicing_params(int raft_layers, int additional_base_layers)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({
        { "layer_height",             "0.2"  },
        { "initial_layer_print_height", "0.2" },
        { "raft_layers",              std::to_string(raft_layers) },
        { "additional_base_layers",   std::to_string(additional_base_layers) },
        { "enable_support",           "1" },
    });

    PrintConfig        print_config;
    PrintObjectConfig  object_config;
    print_config      .apply(cfg, true);
    object_config     .apply(cfg, true);

    std::vector<unsigned int> extruders = { 0 };
    return SlicingParameters::create_from_config(
        print_config, object_config,
        /* object_height */ 20.,
        extruders,
        /* shrinkage_compensation */ Vec3d(1., 1., 1.));
}

TEST_CASE("SlicingParameters: additional_base_layers=0 is legacy behaviour", "[AdditionalBaseLayers]")
{
    auto sp = make_slicing_params(/* raft_layers */ 3, /* additional */ 0);
    size_t total = sp.raft_layers();
    REQUIRE(total == 3);
    CHECK(sp.interface_raft_layers == 2);
    CHECK(sp.base_raft_layers == 1);
}

TEST_CASE("SlicingParameters: additional_base_layers adds to base_raft_layers (raft>1)", "[AdditionalBaseLayers]")
{
    SECTION("raft_layers=3, additional=1") {
        auto sp = make_slicing_params(3, 1);
        CHECK(sp.base_raft_layers == 2);
        CHECK(sp.interface_raft_layers == 2);
        CHECK(sp.raft_layers() == 4);
    }
    SECTION("raft_layers=3, additional=2") {
        auto sp = make_slicing_params(3, 2);
        CHECK(sp.base_raft_layers == 3);
        CHECK(sp.interface_raft_layers == 2);
        CHECK(sp.raft_layers() == 5);
    }
    SECTION("raft_layers=3, additional=3") {
        auto sp = make_slicing_params(3, 3);
        CHECK(sp.base_raft_layers == 4);
        CHECK(sp.interface_raft_layers == 2);
        CHECK(sp.raft_layers() == 6);
    }
    SECTION("raft_layers=5, additional=2") {
        auto sp = make_slicing_params(5, 2);
        CHECK(sp.base_raft_layers == 4);
        CHECK(sp.interface_raft_layers == 3);
        CHECK(sp.raft_layers() == 7);
    }
}

TEST_CASE("SlicingParameters: additional_base_layers with raft_layers<=1", "[AdditionalBaseLayers]")
{
    SECTION("raft_layers=0, additional=1 — no raft, layers handled in SupportCommon") {
        auto sp = make_slicing_params(0, 1);
        CHECK(sp.base_raft_layers == 0);
        CHECK(sp.raft_layers() == 0);
        CHECK_FALSE(sp.has_raft());
    }
    SECTION("raft_layers=1, additional=1 — single-layer raft, guard skips") {
        auto sp = make_slicing_params(1, 1);
        CHECK(sp.base_raft_layers == 0);
        CHECK(sp.interface_raft_layers == 1);
        CHECK(sp.raft_layers() == 1);
    }
}

// -----------------------------------------------------------------------
// Full-pipeline tests  (slice a mesh with support + additional base layers)
// -----------------------------------------------------------------------

TEST_CASE("Additional base layers: no-crash with raft_layers=0", "[AdditionalBaseLayers]")
{
    SECTION("additional=0 — baseline") {
        Slic3r::Print print;
        REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::overhang }, print, {
            { "enable_support",           "1" },
            { "raft_layers",              "0" },
            { "additional_base_layers",   "0" },
        }));
        auto sl = print.objects().front()->support_layers();
        REQUIRE(sl.size() > 0);
    }
    SECTION("additional=1") {
        Slic3r::Print print;
        REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::overhang }, print, {
            { "enable_support",           "1" },
            { "raft_layers",              "0" },
            { "additional_base_layers",   "1" },
        }));
        auto sl = print.objects().front()->support_layers();
        REQUIRE(sl.size() > 0);
    }
    SECTION("additional=2") {
        Slic3r::Print print;
        REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::overhang }, print, {
            { "enable_support",           "1" },
            { "raft_layers",              "0" },
            { "additional_base_layers",   "2" },
        }));
        auto sl = print.objects().front()->support_layers();
        REQUIRE(sl.size() > 0);
    }
    SECTION("additional=3") {
        Slic3r::Print print;
        REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::overhang }, print, {
            { "enable_support",           "1" },
            { "raft_layers",              "0" },
            { "additional_base_layers",   "3" },
        }));
        auto sl = print.objects().front()->support_layers();
        REQUIRE(sl.size() > 0);
    }
}

TEST_CASE("Additional base layers: no-crash with raft_layers=3", "[AdditionalBaseLayers]")
{
    SECTION("additional=0 — baseline") {
        Slic3r::Print print;
        REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::cube_20x20x20 }, print, {
            { "enable_support",           "1" },
            { "raft_layers",              "3" },
            { "additional_base_layers",   "0" },
        }));
        auto sl = print.objects().front()->support_layers();
        REQUIRE(sl.size() >= 3);
    }
    SECTION("additional=1") {
        Slic3r::Print print;
        REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::cube_20x20x20 }, print, {
            { "enable_support",           "1" },
            { "raft_layers",              "3" },
            { "additional_base_layers",   "1" },
        }));
        auto sl = print.objects().front()->support_layers();
        REQUIRE(sl.size() >= 3);
    }
    SECTION("additional=2") {
        Slic3r::Print print;
        REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::cube_20x20x20 }, print, {
            { "enable_support",           "1" },
            { "raft_layers",              "3" },
            { "additional_base_layers",   "2" },
        }));
        auto sl = print.objects().front()->support_layers();
        REQUIRE(sl.size() >= 3);
    }
    SECTION("additional=3") {
        Slic3r::Print print;
        REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::cube_20x20x20 }, print, {
            { "enable_support",           "1" },
            { "raft_layers",              "3" },
            { "additional_base_layers",   "3" },
        }));
        auto sl = print.objects().front()->support_layers();
        REQUIRE(sl.size() >= 3);
    }
}

TEST_CASE("Additional base layers: no-crash with raft_layers=1", "[AdditionalBaseLayers]")
{
    SECTION("additional=0") {
        Slic3r::Print print;
        REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::cube_20x20x20 }, print, {
            { "enable_support",           "1" },
            { "raft_layers",              "1" },
            { "additional_base_layers",   "0" },
        }));
    }
    SECTION("additional=1") {
        Slic3r::Print print;
        REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::cube_20x20x20 }, print, {
            { "enable_support",           "1" },
            { "raft_layers",              "1" },
            { "additional_base_layers",   "1" },
        }));
    }
    SECTION("additional=2") {
        Slic3r::Print print;
        REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::cube_20x20x20 }, print, {
            { "enable_support",           "1" },
            { "raft_layers",              "1" },
            { "additional_base_layers",   "2" },
        }));
    }
}

TEST_CASE("Additional base layers: extra support layers are created (no raft)", "[AdditionalBaseLayers]")
{
    Slic3r::Print print_base;
    init_and_process_print_no_arrange({ TestMesh::overhang }, print_base, {
        { "enable_support",           "1" },
        { "raft_layers",              "0" },
        { "additional_base_layers",   "0" },
    });
    size_t n_base = print_base.objects().front()->support_layers().size();

    SECTION("additional=1 produces more support layers than additional=0") {
        Slic3r::Print print;
        init_and_process_print_no_arrange({ TestMesh::overhang }, print, {
            { "enable_support",           "1" },
            { "raft_layers",              "0" },
            { "additional_base_layers",   "1" },
        });
        size_t n = print.objects().front()->support_layers().size();
        CHECK(n > n_base);
    }
    SECTION("additional=2 produces more support layers than additional=1") {
        Slic3r::Print print1;
        init_and_process_print_no_arrange({ TestMesh::overhang }, print1, {
            { "enable_support",           "1" },
            { "raft_layers",              "0" },
            { "additional_base_layers",   "1" },
        });
        Slic3r::Print print2;
        init_and_process_print_no_arrange({ TestMesh::overhang }, print2, {
            { "enable_support",           "1" },
            { "raft_layers",              "0" },
            { "additional_base_layers",   "2" },
        });
        size_t n1 = print1.objects().front()->support_layers().size();
        size_t n2 = print2.objects().front()->support_layers().size();
        CHECK(n2 > n1);
    }
}

TEST_CASE("Additional base layers: extra raft layers are created (raft>1)", "[AdditionalBaseLayers]")
{
    Slic3r::Print print_base;
    init_and_process_print_no_arrange({ TestMesh::cube_20x20x20 }, print_base, {
        { "enable_support",           "1" },
        { "raft_layers",              "3" },
        { "additional_base_layers",   "0" },
    });
    size_t n_base = print_base.objects().front()->support_layers().size();

    SECTION("additional=1 produces more support layers than additional=0") {
        Slic3r::Print print;
        init_and_process_print_no_arrange({ TestMesh::cube_20x20x20 }, print, {
            { "enable_support",           "1" },
            { "raft_layers",              "3" },
            { "additional_base_layers",   "1" },
        });
        size_t n = print.objects().front()->support_layers().size();
        CHECK(n > n_base);
    }
    SECTION("additional=2 produces more support layers than additional=0") {
        Slic3r::Print print;
        init_and_process_print_no_arrange({ TestMesh::cube_20x20x20 }, print, {
            { "enable_support",           "1" },
            { "raft_layers",              "3" },
            { "additional_base_layers",   "2" },
        });
        size_t n = print.objects().front()->support_layers().size();
        CHECK(n > n_base);
    }
}

TEST_CASE("Additional base layers: raft_layers=5 various additional counts", "[AdditionalBaseLayers]")
{
    for (int additional = 0; additional <= 3; ++ additional) {
        DYNAMIC_SECTION("raft_layers=5, additional=" << additional) {
            Slic3r::Print print;
            REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::cube_20x20x20 }, print, {
                { "enable_support",           "1" },
                { "raft_layers",              "5" },
                { "additional_base_layers",   std::to_string(additional) },
            }));
            auto sl = print.objects().front()->support_layers();
            REQUIRE(sl.size() > 0);
        }
    }
}

// -----------------------------------------------------------------------
// Tree support variants
// -----------------------------------------------------------------------

TEST_CASE("Additional base layers: tree support, no raft", "[AdditionalBaseLayers][TreeSupport]")
{
    for (int additional = 0; additional <= 3; ++ additional) {
        DYNAMIC_SECTION("tree(auto), raft=0, additional=" << additional) {
            Slic3r::Print print;
            REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::overhang }, print, {
                { "enable_support",           "1" },
                { "support_type",             "tree(auto)" },
                { "raft_layers",              "0" },
                { "additional_base_layers",   std::to_string(additional) },
            }));
            auto sl = print.objects().front()->support_layers();
            REQUIRE(sl.size() > 0);
        }
    }
}

TEST_CASE("Additional base layers: tree support, raft_layers=3 diagnostics", "[AdditionalBaseLayers][TreeSupport]")
{
    // Diagnostic test: tree support + raft=3 throws "empty initial layer".
    // Normal support + raft=3 works fine with the same mesh.
    // This test captures what the layers look like to identify the root cause.
    Slic3r::Print print;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "use_relative_e_distances", "0" } });
    config.set_deserialize_strict({
        { "enable_support",           "1" },
        { "support_type",             "tree(auto)" },
        { "raft_layers",              "3" },
        { "additional_base_layers",   "0" },
    });

    Slic3r::Model model;
    TriangleMesh m = mesh(TestMesh::cube_20x20x20);
    ModelObject *object = model.add_object();
    object->name += "object.stl";
    object->add_volume(std::move(m));
    ModelInstance *inst = object->add_instance();
    inst->set_offset(Vec3d(100., 100., 0.));
    for (ModelObject *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }

    print.apply(model, config);
    print.set_status_silent();

    // Process slicing (this generates layers but doesn't do GCode export)
    try {
        print.process();
    } catch (const Slic3r::SlicingErrors &e) {
        std::string msg = "SlicingErrors:";
        for (const auto &err : e.errors_)
            msg += "\n  - " + std::string(err.what());
        FAIL(msg);
    } catch (const std::exception &e) {
        FAIL("process() exception: " << e.what());
    }

    const auto *po = print.objects().front();
    auto obj_layers = po->layers();
    auto sup_layers = po->support_layers();

    INFO("Object layers: " << obj_layers.size());
    INFO("Support layers: " << sup_layers.size());

    REQUIRE(sup_layers.size() > 0);

    // Dump first few support layers
    for (size_t i = 0; i < std::min(sup_layers.size(), size_t(6)); ++i) {
        const auto *sl = sup_layers[i];
        INFO("  support_layer[" << i << "]: print_z=" << sl->print_z
             << " height=" << sl->height
             << " has_extrusions=" << sl->has_extrusions());
    }

    // Dump first few object layers
    for (size_t i = 0; i < std::min(obj_layers.size(), size_t(3)); ++i) {
        const auto *ol = obj_layers[i];
        INFO("  object_layer[" << i << "]: print_z=" << ol->print_z
             << " height=" << ol->height
             << " has_extrusions=" << ol->has_extrusions());
    }

    // The actual check: does the first support layer have extrusions?
    CHECK(sup_layers.front()->has_extrusions());

    // Check that every raft layer (below object) has extrusions
    double first_object_z = obj_layers.empty() ? 9999. : obj_layers.front()->print_z;
    for (size_t i = 0; i < sup_layers.size(); ++i) {
        if (sup_layers[i]->print_z < first_object_z - EPSILON) {
            INFO("Raft support_layer[" << i << "] at z=" << sup_layers[i]->print_z);
            CHECK(sup_layers[i]->has_extrusions());
        }
    }
}

TEST_CASE("Additional base layers: tree support, raft_layers=3", "[AdditionalBaseLayers][TreeSupport]")
{
    for (int additional = 0; additional <= 3; ++ additional) {
        DYNAMIC_SECTION("tree(auto), raft=3, additional=" << additional) {
            Slic3r::Print print;
            REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::cube_20x20x20 }, print, {
                { "enable_support",           "1" },
                { "support_type",             "tree(auto)" },
                { "raft_layers",              "3" },
                { "additional_base_layers",   std::to_string(additional) },
            }));
            auto sl = print.objects().front()->support_layers();
            REQUIRE(sl.size() >= 3);
        }
    }
}

TEST_CASE("Additional base layers: tree support, raft_layers=1", "[AdditionalBaseLayers][TreeSupport]")
{
    for (int additional = 0; additional <= 2; ++ additional) {
        DYNAMIC_SECTION("tree(auto), raft=1, additional=" << additional) {
            Slic3r::Print print;
            REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::cube_20x20x20 }, print, {
                { "enable_support",           "1" },
                { "support_type",             "tree(auto)" },
                { "raft_layers",              "1" },
                { "additional_base_layers",   std::to_string(additional) },
            }));
        }
    }
}

TEST_CASE("Additional base layers: tree support styles", "[AdditionalBaseLayers][TreeSupport]")
{
    auto styles = { "tree_slim", "tree_strong", "tree_hybrid" };
    for (const char *style : styles) {
        DYNAMIC_SECTION("style=" << style << ", raft=3, additional=2") {
            Slic3r::Print print;
            REQUIRE_NOTHROW(init_and_process_print_no_arrange({ TestMesh::cube_20x20x20 }, print, {
                { "enable_support",           "1" },
                { "support_type",             "tree(auto)" },
                { "support_style",            style },
                { "raft_layers",              "3" },
                { "additional_base_layers",   "2" },
            }));
            auto sl = print.objects().front()->support_layers();
            REQUIRE(sl.size() > 0);
        }
    }
}
