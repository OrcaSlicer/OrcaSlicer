#include <catch2/catch_all.hpp>

#include <fstream>
#include <iterator>

#include <boost/filesystem.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// What a bake leaves behind has to survive the slicer. The bake itself is covered in the libslic3r
// suite; these tests carry its result through Print::process and the G-code, which is where a mesh
// the slicer cannot use shows up - as a crash, or as a print with nothing in it.
namespace {

// A striped height map, so the relief has real steps in it rather than a flat offset.
std::shared_ptr<std::vector<unsigned char>> stripes_png(size_t w = 32, size_t h = 32)
{
    std::vector<uint8_t> pixels(w * h);
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x)
            pixels[y * w + x] = ((x / 4) % 2 == 0) ? 20 : 235;

    const boost::filesystem::path tmp_path = boost::filesystem::temp_directory_path() /
                                             boost::filesystem::unique_path("texdisp_slice_%%%%%%%%.png");
    REQUIRE(Slic3r::png::write_gray_to_file(tmp_path.string(), w, h, pixels));
    std::vector<unsigned char> bytes;
    {
        std::ifstream ifs(tmp_path.string(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    boost::system::error_code ec;
    boost::filesystem::remove(tmp_path, ec);
    REQUIRE_FALSE(bytes.empty());
    return std::make_shared<std::vector<unsigned char>>(std::move(bytes));
}

// Paints every facet of `volume` with the texture layer in slot 0, as painting over the whole model
// does, and gives the layer a relief to bake.
void paint_whole_volume(ModelVolume &volume, float depth_mm, float tile_mm)
{
    TextureDisplacementLayer layer;
    layer.slot         = 0;
    layer.image_data   = stripes_png();
    layer.depth_mm     = depth_mm;
    layer.tiling_scale = tile_mm;
    volume.texture_displacement_layers = { layer };

    TriangleSelector selector(volume.mesh());
    for (int i = 0; i < int(volume.mesh().its.indices.size()); ++i)
        selector.set_facet(i, EnforcerBlockerType::ENFORCER);
    volume.texture_displacement_facet(0).set(selector);
}

// The bake as the gizmo's job commits it: the mesh is replaced, the hull recomputed and the paint of
// the baked layer cleared. `budget_k` and `resolution_mm` stand in for the two panel controls.
size_t bake_into_volume(ModelVolume &volume, int budget_k, float resolution_mm)
{
    TextureDisplacementOptions options;
    options.pipeline_v2        = true;
    options.v2_refine_mm       = resolution_mm;
    options.v2_max_triangles_k = budget_k;

    TextureDisplacementFacetsData facets;
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        facets[size_t(i)] = volume.texture_displacement_facet(i).get_data();

    TriangleMesh baked(build_texture_displacement(volume.mesh().its, volume.texture_displacement_layers, facets, options));
    REQUIRE_FALSE(baked.empty());

    volume.set_mesh(std::move(baked));
    volume.set_new_unique_id();
    volume.calculate_convex_hull();
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        volume.texture_displacement_facet(i).reset();
    return volume.mesh().its.indices.size();
}

// Extruding moves in the G-code: what tells a print with toolpaths from one without.
size_t count_extrusions(const std::string &gcode_text)
{
    size_t count = 0;
    for (size_t pos = 0; (pos = gcode_text.find("\nG1 ", pos)) != std::string::npos; ++pos) {
        const size_t eol = gcode_text.find('\n', pos + 1);
        if (gcode_text.find(" E", pos) < eol)
            ++count;
    }
    return count;
}

} // namespace

TEST_CASE("A baked texture slices into a print with toolpaths", "[TextureDisplacement]")
{
    Model        model = Slic3r::Test::model("cube", Test::cube(20.));
    ModelVolume &volume = *model.objects.front()->volumes.front();
    paint_whole_volume(volume, /* depth */ 0.4f, /* tile */ 8.f);

    // Coarse on purpose: this test is about the result reaching the slicer, not about how fine it is.
    const size_t baked_triangles = bake_into_volume(volume, /* budget */ 60, /* resolution */ 0.8f);
    CHECK(baked_triangles > Test::cube(20.).its.indices.size()); // the relief added geometry

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "layer_height", "0.2" }, { "initial_layer_print_height", "0.2" } });

    Print print;
    Model sliced_model;
    init_print({ volume.mesh() }, print, sliced_model, config);
    REQUIRE(print.objects().size() == 1);

    print.process();
    const std::string gcode_text = Test::gcode(print);

    REQUIRE_FALSE(gcode_text.empty());
    CHECK(count_extrusions(gcode_text) > 1000);
    CHECK(print.objects().front()->layer_count() > 10);
}

TEST_CASE("A baked texture slices the same whether or not the budget capped it", "[TextureDisplacement]")
{
    // The budget decides how much of the relief survives, and a capped bake goes through the
    // simplification and the repair that follow it. Both have to leave a mesh the slicer can print.
    const int budget_k = GENERATE(10, 400);

    Model        model  = Slic3r::Test::model("cube", Test::cube(20.));
    ModelVolume &volume = *model.objects.front()->volumes.front();
    paint_whole_volume(volume, /* depth */ 0.4f, /* tile */ 8.f);
    bake_into_volume(volume, budget_k, /* resolution */ 0.5f);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "layer_height", "0.2" }, { "initial_layer_print_height", "0.2" } });

    Print print;
    Model sliced_model;
    init_print({ volume.mesh() }, print, sliced_model, config);
    print.process();

    const std::string gcode_text = Test::gcode(print);
    REQUIRE_FALSE(gcode_text.empty());
    CHECK(count_extrusions(gcode_text) > 1000);
}
