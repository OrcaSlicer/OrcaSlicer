#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/ObjColorUtils.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "test_utils.hpp"

#include <array>
#include <fstream>
#include <iomanip>
#include <set>

using namespace Slic3r;

namespace {

Model make_single_triangle_model()
{
    indexed_triangle_set its;
    its.vertices = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {0.0f, 10.0f, 0.0f}};
    its.indices = {{0, 1, 2}};

    Model model;
    ModelObject* object = model.add_object();
    object->add_volume(TriangleMesh(std::move(its)), ModelVolumeType::MODEL_PART, false);
    return model;
}

void write_colored_tetrahedra(const std::string& path, int color_count)
{
    std::ofstream out(path);
    out << std::setprecision(9);
    const std::array<std::array<int, 3>, 6> colors = {{{217, 107, 67}, {43, 36, 34}, {242, 215, 181},
                                                     {47, 107, 95}, {50, 103, 168}, {155, 63, 119}}};
    const std::array<std::array<int, 3>, 4> vertices = {{{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}}};
    for (int c = 0; c < color_count; ++c) {
        for (const auto& v : vertices)
            out << "v " << v[0] + 12 * c << ' ' << v[1] << ' ' << v[2] << ' '
                << colors[c][0] / 255.f << ' ' << colors[c][1] / 255.f << ' ' << colors[c][2] / 255.f << '\n';
        const int start = 4 * c + 1;
        for (const auto& f : std::array<std::array<int, 3>, 4>{{{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}}})
            out << "f " << start + f[0] << ' ' << start + f[1] << ' ' << start + f[2] << '\n';
    }
    REQUIRE(out.good());
}

Model read_with_colors(const std::string& path, ObjImportColorFn callback)
{
    return Model::read_from_file(path, nullptr, nullptr, LoadStrategy::LoadModel, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, 0, std::move(callback));
}

} // namespace

TEST_CASE("OBJ vertex colors retain non-base filament facets", "[Model][OBJ][MMU]")
{
    Model model = make_single_triangle_model();
    ModelVolume* volume = model.objects.front()->volumes.front();

    REQUIRE(Model::obj_import_vertex_color_deal({1, 1, 1}, 1, &model));
    CHECK(volume->mmu_segmentation_facets.empty());

    REQUIRE(Model::obj_import_vertex_color_deal({1, 1, 2}, 1, &model));
    CHECK_FALSE(volume->mmu_segmentation_facets.empty());
    CHECK_FALSE(volume->mmu_segmentation_facets.get_triangle_as_string(0).empty());
}

TEST_CASE("Explicit OBJ color callbacks preserve one through six material regions", "[ModelVertexColors][OBJ]")
{
    const int color_count = GENERATE(1, 2, 3, 4, 5, 6);
    ScopedTemporaryFile file(".obj");
    write_colored_tetrahedra(file.string(), color_count);
    int calls = 0;
    bool applied = false;
    Model model = read_with_colors(file.string(), [&](ObjDialogInOut& in_out) {
        ++calls;
        REQUIRE(in_out.deal_vertex_color);
        REQUIRE(in_out.model != nullptr);
        REQUIRE(in_out.input_colors.size() == size_t(4 * color_count));
        CHECK(std::set<RGBA>(in_out.input_colors.begin(), in_out.input_colors.end()).size() == size_t(color_count));
        for (int i = 0; i < 4 * color_count; ++i)
            in_out.filament_ids.push_back(static_cast<unsigned char>(i / 4 + 1));
        in_out.first_extruder_id = 1;
        applied = Model::obj_import_vertex_color_deal(in_out.filament_ids, in_out.first_extruder_id, in_out.model);
    });
    REQUIRE(calls == 1);
    REQUIRE(applied);
    REQUIRE_FALSE(model.texture_mesh);
    REQUIRE(model.objects.size() == 1);
    const auto* volume = model.objects.front()->volumes.front();
    CHECK(volume->config.extruder() == 1);
    CHECK(volume->mesh().facets_count() == size_t(4 * color_count));
    for (int c = 1; c < color_count; ++c)
        CHECK(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(c + 1)));
}

TEST_CASE("Default OBJ loading retains the native texture import path", "[ModelVertexColors][OBJ]")
{
    const int color_count = GENERATE(1, 2, 3, 4, 5, 6);
    ScopedTemporaryFile file(".obj");
    write_colored_tetrahedra(file.string(), color_count);
    Model model = read_with_colors(file.string(), nullptr);
    REQUIRE(model.texture_mesh);
    CHECK(model.texture_mesh->precomputed_vertex_colors.size() == size_t(4 * color_count));
    CHECK(model.texture_mesh->precomputed_face_colors.size() == size_t(4 * color_count));
    CHECK(model.objects.front()->volumes.front()->mmu_segmentation_facets.empty());
}

TEST_CASE("Explicit geometry-only OBJ loading does not invoke texture matching", "[ModelVertexColors][OBJ]")
{
    ScopedTemporaryFile file(".obj");
    write_colored_tetrahedra(file.string(), 6);
    int calls = 0;
    Model model = read_with_colors(file.string(), [&](ObjDialogInOut&) { ++calls; });
    REQUIRE(calls == 1);
    REQUIRE_FALSE(model.texture_mesh);
    REQUIRE(model.objects.size() == 1);
    CHECK(model.objects.front()->volumes.front()->mmu_segmentation_facets.empty());
}

TEST_CASE("Cancelling an explicit OBJ color callback leaves no imported objects", "[ModelVertexColors][OBJ]")
{
    ScopedTemporaryFile file(".obj");
    write_colored_tetrahedra(file.string(), 6);
    int calls = 0;
    Model model = read_with_colors(file.string(), [&](ObjDialogInOut& in_out) {
        ++calls;
        in_out.cancelled = true;
    });
    REQUIRE(calls == 1);
    CHECK(model.objects.empty());
    CHECK_FALSE(model.texture_mesh);
}

TEST_CASE("Explicit OBJ callbacks apply material face colors", "[ModelVertexColors][OBJ]")
{
    ScopedTemporaryDir dir;
    const auto obj = dir.path() / "face-colors.obj";
    {
        std::ofstream mtl((dir.path() / "face-colors.mtl").string());
        mtl << "newmtl red\nKa 0 0 0\nKd 1 0 0\nnewmtl blue\nKa 0 0 0\nKd 0 0 1\n";
        std::ofstream out(obj.string());
        out << "mtllib face-colors.mtl\nv 0 0 0\nv 10 0 0\nv 0 10 0\nv 0 0 10\n"
               "usemtl red\nf 1 3 2\nf 1 2 4\nusemtl blue\nf 1 4 3\nf 2 3 4\n";
    }
    int calls = 0;
    bool applied = false;
    Model model = read_with_colors(obj.string(), [&](ObjDialogInOut& in_out) {
        ++calls;
        REQUIRE_FALSE(in_out.deal_vertex_color);
        REQUIRE(in_out.input_colors.size() == 4);
        in_out.filament_ids = {1, 1, 6, 6};
        in_out.first_extruder_id = 1;
        applied = Model::obj_import_face_color_deal(in_out.filament_ids, in_out.first_extruder_id, in_out.model);
    });
    REQUIRE(calls == 1);
    REQUIRE(applied);
    REQUIRE_FALSE(model.texture_mesh);
    const auto* volume = model.objects.front()->volumes.front();
    CHECK(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType::Extruder6));
}

TEST_CASE("Preserved OBJ palettes keep nearby RGB colors and stable labels", "[ModelVertexColors][OBJ]")
{
    const int color_count = GENERATE(1, 2, 3, 4, 5, 6);
    const char requested = GENERATE(char(-1), char(6));
    std::vector<RGBA> input;
    for (int i = 0; i < 12; ++i)
        input.push_back({(90 + i % color_count) / 255.f, 100 / 255.f, 120 / 255.f, 1.f});
    std::vector<RGBA> palette;
    std::vector<int> labels;
    char count = requested;
    obj_color_deal_algo(input, palette, labels, count, 32, true);
    REQUIRE(palette.size() == size_t(color_count));
    REQUIRE(labels.size() == input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        REQUIRE(labels[i] == int(i % color_count));
        for (size_t c = 0; c < 4; ++c)
            CHECK_THAT(palette[labels[i]][c], Catch::Matchers::WithinAbs(input[i][c], 1e-7));
    }
}

TEST_CASE("An explicit smaller OBJ color count still permits quantization", "[ModelVertexColors][OBJ]")
{
    const bool preserve = GENERATE(false, true);
    std::vector<RGBA> input = {{1.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 1.f, 1.f}};
    std::vector<RGBA> palette;
    std::vector<int> labels;
    char count = 1;
    obj_color_deal_algo(input, palette, labels, count, 32, preserve);
    REQUIRE(palette.size() == 1);
    CHECK(labels == std::vector<int>{0, 0});
}
