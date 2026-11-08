// End-to-end slicing of the mesh and SVG sparse infill patterns through Print::apply/process:
// the pattern enum is accepted by the config, prepare_mesh_infill_data runs, and the layers
// receive sparse infill extrusions (see docs/HLSD/mesh-infill.md).
#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>

#include <cstdlib>
#include <fstream>
#include <functional>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;

namespace {

double sparse_infill_length(const Print &print)
{
    double len = 0.;
    for (const PrintObject *object : print.objects())
        for (const Layer *layer : object->layers())
            for (const LayerRegion *region : layer->regions())
                for (const ExtrusionEntity *entity : region->fills.flatten().entities)
                    if (entity->role() == erInternalInfill)
                        len += unscaled<double>(entity->length());
    return len;
}

DynamicPrintConfig base_config(std::initializer_list<ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "sparse_infill_density", "15%" },
        { "wall_loops",            "2" },
        { "top_shell_layers",      "2" },
        { "bottom_shell_layers",   "2" },
        // Unrelated validate() requirement of the stock test config (relative E addressing).
        { "layer_change_gcode",    "G92 E0" },
    });
    config.set_deserialize_strict(extra);
    return config;
}

// A 30 mm cube whose Infill Mesh cell is a cross of two 1 mm bars in a 6 x 4 x 1 mm box: tiled
// on that pitch it becomes a rectangular grid of walls with open cells (a solid box would union
// into one solid block).
void make_cube_with_infill_mesh(Model &model, const Vec3d &cell_rotation = Vec3d::Zero())
{
    ModelObject *object = model.add_object();
    object->name = "cube";
    object->add_volume(Test::cube(30.));
    indexed_triangle_set cross = its_make_cube(6., 1., 1.);
    its_transform(cross, Transform3d(Eigen::Translation3d(0., 1.5, 0.)), false);
    indexed_triangle_set bar = its_make_cube(1., 4., 1.);
    its_transform(bar, Transform3d(Eigen::Translation3d(2.5, 0., 0.)), false);
    its_merge(cross, bar);
    ModelVolume *cell = object->add_volume(TriangleMesh(std::move(cross)), ModelVolumeType::INFILL_MESH);
    cell->set_rotation(cell_rotation);
    object->add_instance();
    object->ensure_on_bed();
}

// Every layer strictly between the shells must carry sparse infill.
void require_sparse_infill_on_interior_layers(const Print &print)
{
    const PrintObject *object = print.objects().front();
    REQUIRE(object->layers().size() > 6);
    for (size_t i = 3; i + 3 < object->layers().size(); ++i) {
        double len = 0.;
        for (const LayerRegion *region : object->layers()[i]->regions())
            for (const ExtrusionEntity *entity : region->fills.flatten().entities)
                if (entity->role() == erInternalInfill)
                    len += unscaled<double>(entity->length());
        INFO("layer " << i << " z=" << object->layers()[i]->print_z);
        REQUIRE(len > 10.);
    }
}

struct ResourcesDirGuard
{
    std::string previous;
    ResourcesDirGuard() : previous(resources_dir()) {
        set_resources_dir((boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources").string());
    }
    ~ResourcesDirGuard() { set_resources_dir(previous); }
};

} // namespace

TEST_CASE("Catfill slices a cube with sparse infill", "[MeshInfill][FillSvg]") {
    ResourcesDirGuard resources;
    Print print;
    Model model;
    Test::init_print({ Test::cube(30.) }, print, model, base_config({ { "sparse_infill_pattern", "catfill" } }));
    print.process();
    require_sparse_infill_on_interior_layers(print);
    // The automatic scale realizes roughly the requested density: the interior square is
    // about 27 x 27 mm per layer, so lines * 0.45 mm / area should be near 15%.
    const PrintObject *object = print.objects().front();
    const size_t mid = object->layers().size() / 2;
    double len = 0.;
    for (const LayerRegion *region : object->layers()[mid]->regions())
        for (const ExtrusionEntity *entity : region->fills.flatten().entities)
            if (entity->role() == erInternalInfill)
                len += unscaled<double>(entity->length());
    const double fraction = len * 0.45 / (27. * 27.);
    REQUIRE(fraction > 0.08);
    REQUIRE(fraction < 0.25);
}

TEST_CASE("Custom (SVG) slices with a user file and honours the scale", "[MeshInfill][FillSvg]") {
    ResourcesDirGuard resources;
    // generic_string(): the path goes through set_deserialize_strict, which C-unescapes string values,
    // so a native Windows path would lose its backslashes.
    const std::string svg = (boost::filesystem::path(TEST_DATA_DIR) / "catsfill2.svg").generic_string();
    auto slice = [&](const char *scale) {
        Print print;
        Model model;
        Test::init_print({ Test::cube(30.) }, print, model,
                         base_config({ { "sparse_infill_pattern", "svg" },
                                       { "sparse_infill_svg_path", svg },
                                       { "sparse_infill_svg_scale", scale } }));
        print.process();
        // The setting must reach the region config the Filler reads.
        REQUIRE(print.objects().front()->printing_region(0).config().sparse_infill_svg_scale.value == Catch::Approx(std::atof(scale)));
        require_sparse_infill_on_interior_layers(print);
        return sparse_infill_length(print);
    };
    const double automatic = slice("0");
    const double small     = slice("5");   // the 193 mm pattern cell at 5% = 9.6 mm tiles
    const double large     = slice("20");  // 38.6 mm tiles
    REQUIRE(automatic > 0.);
    // Larger tiles put fewer lines into the 30 mm cube (only part of the drawing lands in it).
    REQUIRE(small > 1.5 * large);
    REQUIRE(large > 0.);
}

TEST_CASE("Custom (infill mesh) tiles the Infill Mesh volume through the cube", "[MeshInfill]") {
    auto slice = [](const Vec3d &cell_rotation, const char *global, const char *pattern = "custommesh") {
        Print print;
        Model model;
        make_cube_with_infill_mesh(model, cell_rotation);
        print.apply(model, base_config({ { "sparse_infill_pattern", pattern },
                                         { "infill_mesh_global_orientation", global } }));
        print.process();
        REQUIRE(print.objects().size() == 1);
        require_sparse_infill_on_interior_layers(print);
        return sparse_infill_length(print);
    };

    SECTION("axis-aligned cell") {
        const double lattice = slice(Vec3d::Zero(), "0");
        REQUIRE(lattice > 100.);
        // The lattice is what got printed, not the analytic fallback.
        const double gyroid = slice(Vec3d::Zero(), "0", "gyroid");
        REQUIRE(lattice != Catch::Approx(gyroid).epsilon(0.01));
    }
    SECTION("global and local orientation agree without a layer rotation") {
        const double local  = slice(Vec3d::Zero(), "0");
        const double global = slice(Vec3d::Zero(), "1");
        REQUIRE(global == Catch::Approx(local).epsilon(1e-6));
    }
    SECTION("a rotated cell rotates the lattice") {
        const double upright = slice(Vec3d::Zero(), "0");
        const double tilted  = slice(Vec3d(0., 0., M_PI / 6.), "0");
        REQUIRE(tilted > 100.);
        REQUIRE(tilted != Catch::Approx(upright).epsilon(0.01));
    }
}

TEST_CASE("Custom (infill mesh) prints an open-sheet cell as single lines", "[MeshInfill]") {
    Print print;
    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(Test::cube(30.));
    // Two crossing vertical 6 x 6 mm sheets: an open surface, tiled on a 6 x 6 x 6 mm pitch.
    indexed_triangle_set cross;
    cross.vertices = { {0.f, 3.f, 0.f}, {6.f, 3.f, 0.f}, {6.f, 3.f, 6.f}, {0.f, 3.f, 6.f},
                       {3.f, 0.f, 0.f}, {3.f, 6.f, 0.f}, {3.f, 6.f, 6.f}, {3.f, 0.f, 6.f} };
    cross.indices  = { {0, 1, 2}, {0, 2, 3}, {4, 5, 6}, {4, 6, 7} };
    object->add_volume(TriangleMesh(std::move(cross)), ModelVolumeType::INFILL_MESH);
    object->add_instance();
    object->ensure_on_bed();
    print.apply(model, base_config({ { "sparse_infill_pattern", "custommesh" } }));
    print.process();
    require_sparse_infill_on_interior_layers(print);
    // A 6 mm grid of single lines over a ~27 mm square: two families of ~4-5 lines of ~27 mm.
    const PrintObject *po = print.objects().front();
    const Layer *mid = po->layers()[po->layers().size() / 2];
    double len = 0.;
    for (const LayerRegion *region : mid->regions())
        for (const ExtrusionEntity *entity : region->fills.flatten().entities)
            if (entity->role() == erInternalInfill)
                len += unscaled<double>(entity->length());
    REQUIRE(len > 150.);
    REQUIRE(len < 400.);
}

TEST_CASE("Custom (infill mesh) without an Infill Mesh volume falls back to gyroid", "[MeshInfill]") {
    Print print;
    Model model;
    Test::init_print({ Test::cube(30.) }, print, model, base_config({ { "sparse_infill_pattern", "custommesh" } }));
    print.process();
    require_sparse_infill_on_interior_layers(print);
}

// Hidden helper (run explicitly with the [.dump] tag): slice a 60 x 40 x 6 mm box with the
// pattern given by the environment and write one mid layer's sparse infill as JSON polylines
// for a quick picture of what the pattern prints, without driving the GUI.
//   UNLAYERED_INFILL_DUMP=<out.json> [UNLAYERED_INFILL_PATTERN=catfill|svg|custommesh]
//   [UNLAYERED_INFILL_SVG=<file>] [UNLAYERED_INFILL_MESH=<file>] [UNLAYERED_INFILL_SINGLE_WALL=1]
//   [UNLAYERED_INFILL_SCALE=<pct>] [UNLAYERED_INFILL_GLOBAL=1]
TEST_CASE("Dump one layer of sparse infill", "[.dump]") {
    const char *out = std::getenv("UNLAYERED_INFILL_DUMP");
    if (out == nullptr)
        return;
    ResourcesDirGuard resources;
    auto env = [](const char *name, const char *fallback) { const char *v = std::getenv(name); return std::string(v != nullptr ? v : fallback); };
    Print print;
    Model model;
    Test::init_print({ make_cube(60., 40., 6.) }, print, model,
                     base_config({ { "sparse_infill_pattern",     env("UNLAYERED_INFILL_PATTERN", "catfill") },
                                   { "sparse_infill_svg_path",    env("UNLAYERED_INFILL_SVG", "") },
                                   { "sparse_infill_svg_scale",   env("UNLAYERED_INFILL_SCALE", "0") },
                                   { "sparse_infill_single_wall", env("UNLAYERED_INFILL_SINGLE_WALL", "0") },
                                   { "infill_mesh_path",          env("UNLAYERED_INFILL_MESH", "") },
                                   { "infill_mesh_global_orientation", env("UNLAYERED_INFILL_GLOBAL", "0") },
                                   { "infill_direction",          "0" } }));
    print.process();
    const PrintObject *object = print.objects().front();
    REQUIRE(object->printing_region(0).config().sparse_infill_single_wall.value == (env("UNLAYERED_INFILL_SINGLE_WALL", "0") == "1"));
    const Layer *layer = object->layers()[object->layers().size() / 2];
    std::ofstream f(out);
    f << "[";
    bool first = true;
    std::function<void(const ExtrusionEntity *)> dump = [&](const ExtrusionEntity *entity) {
        if (auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
            for (const ExtrusionEntity *child : collection->entities)
                dump(child);
            return;
        }
        if (entity->role() != erInternalInfill)
            return;
        const Polyline pl = entity->as_polyline();
        f << (first ? "" : ",") << "[";
        for (size_t i = 0; i < pl.points.size(); ++ i)
            f << (i ? "," : "") << "[" << unscaled<double>(pl.points[i].x()) << "," << unscaled<double>(pl.points[i].y()) << "]";
        f << "]";
        first = false;
    };
    for (const LayerRegion *region : layer->regions())
        for (const ExtrusionEntity *entity : region->fills.entities)
            dump(entity);
    f << "]\n";
}
