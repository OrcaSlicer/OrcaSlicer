#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/ObjColorUtils.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "test_utils.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>

using namespace Slic3r;

TEST_CASE("Fixed import palettes keep edited target colors and source regions", "[ModelVertexColors][FixedPalette]")
{
    const bool include_vertex_colors = GENERATE(false, true);
    TexturedMesh mesh;
    for (int part = 0; part < 2; ++part) {
        const int first = (int)mesh.vertices.size();
        for (const auto& v : std::array<std::array<float, 3>, 4>{{{0,0,0}, {10,0,0}, {0,10,0}, {0,0,10}}}) {
            mesh.vertices.push_back({v[0] + part * 20.f, v[1], v[2]});
            if (include_vertex_colors)
                mesh.precomputed_vertex_colors.push_back(part == 0 ? std::array<float,4>{1,0,0,1} : std::array<float,4>{0,0,1,1});
        }
        for (const auto& f : std::array<std::array<int, 3>, 4>{{{0,2,1}, {0,1,3}, {0,3,2}, {1,2,3}}}) {
            mesh.indices.push_back({first + f[0], first + f[1], first + f[2]});
            mesh.precomputed_face_colors.push_back(part == 0 ? std::array<size_t,3>{255,0,0} : std::array<size_t,3>{0,0,255});
        }
    }
    TexturePaintingSettings settings;
    settings.smooth_weight = 0;
    settings.fixed_mapping_palette = {{255,0,0}, {0,0,255}, {0,255,0}};
    settings.fixed_palette = {{0,0,255}, {255,0,0}, {255,255,0}};
    PaintedMesh painted;
    REQUIRE(face_colors_to_painting(mesh, painted, settings));
    REQUIRE(painted.face_colors.size() == mesh.indices.size());
    for (size_t i = 0; i < painted.face_colors.size(); ++i)
        CHECK(painted.face_colors[i] == settings.fixed_palette[i < 4 ? 0 : 1]);
    CHECK(painted.cluster_colors.size() == 2);
    CHECK(painted.vertices == mesh.vertices);
    CHECK(painted.indices == mesh.indices);
}

TEST_CASE("Fixed import palettes reject invalid colors and mismatched source centers", "[ModelVertexColors][FixedPalette]")
{
    TexturedMesh mesh;
    mesh.vertices = {{0,0,0}, {10,0,0}, {0,10,0}, {0,0,10}};
    mesh.indices = {{0,2,1}, {0,1,3}, {0,3,2}, {1,2,3}};
    mesh.precomputed_face_colors.assign(4, {255,0,0});
    TexturePaintingSettings settings;
    settings.smooth_weight = 0;
    settings.fixed_palette = {{0,0,255}};
    settings.fixed_mapping_palette = {{255,0,0}, {0,255,0}};
    PaintedMesh painted;
    CHECK_FALSE(face_colors_to_painting(mesh, painted, settings));
    settings.fixed_mapping_palette.clear();
    settings.fixed_palette = {{256,0,0}};
    CHECK_FALSE(face_colors_to_painting(mesh, painted, settings));
}

TEST_CASE("Explicit face colors survive old trial mapping centers without changing other regions", "[ModelVertexColors][FaceColorOverride]")
{
    const bool vertex_route = GENERATE(false, true);
    using Color = std::array<size_t, 3>;
    const Color red {255, 0, 0}, green {0, 255, 0}, blue {0, 0, 255};
    TexturedMesh mesh;
    mesh.vertices = {{0,0,0}, {10,0,0}, {0,10,0}, {0,0,10}};
    mesh.indices = {{0,2,1}, {0,1,3}, {0,3,2}, {1,2,3}};
    mesh.precomputed_face_colors.assign(4, blue);
    if (vertex_route)
        mesh.precomputed_vertex_colors.assign(4, {0,0,1,1});
    TexturePaintingSettings settings;
    settings.fixed_mapping_palette = {blue, red};
    settings.fixed_palette = {red, green};
    settings.smooth_weight = 1.;
    settings.face_color_overrides = {{0, blue}};
    PaintedMesh painted;
    REQUIRE(face_colors_to_painting(mesh, painted, settings));
    REQUIRE(painted.face_colors.size() == mesh.indices.size());
    CHECK(painted.face_colors[0] == blue);
    for (size_t face = 1; face < mesh.indices.size(); ++face)
        CHECK(painted.face_colors[face] == red);
    CHECK(painted.indices == mesh.indices);
    CHECK(painted.vertices == mesh.vertices);
    CHECK(std::set<Color>(painted.cluster_colors.begin(), painted.cluster_colors.end()) == std::set<Color>{red, blue});
    // Targets never join the source mapping centers or capture other regions.
    CHECK(settings.fixed_mapping_palette == std::vector<Color>{blue, red});
}

TEST_CASE("Explicit low-poly face colors retain conforming subdivision and untouched region colors", "[ModelVertexColors][FaceColorOverride]")
{
    using Color = std::array<size_t, 3>;
    TexturedMesh mesh;
    mesh.vertices = {{0,0,0}, {10,0,0}, {0,10,0}, {0,0,10}};
    mesh.indices = {{0,2,1}, {0,1,3}, {0,3,2}, {1,2,3}};
    mesh.precomputed_face_colors.assign(4, {128,128,128});
    mesh.precomputed_vertex_colors = {{1,0,0,1}, {0,1,0,1}, {0,0,1,1}, {1,1,1,1}};
    TexturePaintingSettings settings;
    settings.fixed_palette = {{255,0,0}, {0,255,0}, {0,0,255}, {255,255,255}};
    PaintedMesh baseline, edited;
    REQUIRE(face_colors_to_painting(mesh, baseline, settings));
    const Color target {102,140,182};
    settings.face_color_overrides = {{0, target}};
    REQUIRE(face_colors_to_painting(mesh, edited, settings));
    REQUIRE(edited.indices == baseline.indices);
    REQUIRE(edited.vertices == baseline.vertices);
    REQUIRE(edited.face_colors.size() == baseline.face_colors.size());
    size_t selected_children = 0;
    for (size_t face = 0; face < edited.indices.size(); ++face) {
        const auto& indices = edited.indices[face];
        const bool on_selected_plane = edited.vertices[indices[0]][2] == 0.f &&
            edited.vertices[indices[1]][2] == 0.f && edited.vertices[indices[2]][2] == 0.f;
        if (on_selected_plane) {
            ++selected_children;
            CHECK(edited.face_colors[face] == target);
        } else {
            CHECK(edited.face_colors[face] == baseline.face_colors[face]);
        }
    }
    CHECK(selected_children > 1);
    indexed_triangle_set output;
    for (const auto& vertex : edited.vertices) output.vertices.emplace_back(vertex[0], vertex[1], vertex[2]);
    for (const auto& face : edited.indices) output.indices.emplace_back(face[0], face[1], face[2]);
    CHECK(its_num_open_edges(output) == 0);
}

TEST_CASE("Face overrides preserve an open source surface without invoking repair or smoothing", "[ModelVertexColors][FaceColorOverride]")
{
    const bool fixed_palette = GENERATE(false, true);
    TexturedMesh mesh;
    mesh.vertices = {{0,0,0}, {10,0,0}, {0,10,0}};
    mesh.indices = {{0,1,2}};
    mesh.precomputed_face_colors = {{255,0,0}};
    TexturePaintingSettings settings;
    settings.target_colors_num = 1;
    settings.smooth_weight = 1.;
    if (fixed_palette) settings.fixed_palette = {{255,0,0}};
    settings.mesh_repair_decision = TexturePaintingSettings::MeshRepairDecision::RepairAndImport;
    bool repair_called = false;
    settings.mesh_repair_callback = [&](const indexed_triangle_set&, indexed_triangle_set&,
        std::function<void(const char*, unsigned)>, std::function<bool()>, std::string*) {
        repair_called = true;
        return false;
    };
    settings.face_color_overrides = {{0, {102,140,182}}};
    PaintedMesh painted;
    REQUIRE(face_colors_to_painting(mesh, painted, settings));
    CHECK_FALSE(repair_called);
    CHECK(painted.vertices == mesh.vertices);
    CHECK(painted.indices == mesh.indices);
    CHECK(painted.face_colors == std::vector<std::array<size_t,3>>{{102,140,182}});
}

TEST_CASE("Invalid explicit face colors fail before replacing a previous painted result", "[ModelVertexColors][FaceColorOverride]")
{
    const bool vertex_route = GENERATE(false, true);
    const bool invalid_index = GENERATE(false, true);
    TexturedMesh mesh;
    mesh.vertices = {{0,0,0}, {10,0,0}, {0,10,0}};
    mesh.indices = {{0,1,2}};
    mesh.precomputed_face_colors = {{255,0,0}};
    if (vertex_route) mesh.precomputed_vertex_colors.assign(3, {1,0,0,1});
    TexturePaintingSettings settings;
    settings.fixed_palette = {{255,0,0}};
    settings.face_color_overrides = invalid_index
        ? std::vector<std::pair<size_t,std::array<size_t,3>>>{{1,{0,0,255}}}
        : std::vector<std::pair<size_t,std::array<size_t,3>>>{{0,{0,0,256}}};
    PaintedMesh previous;
    previous.face_colors = {{1,2,3}};
    CHECK_FALSE(face_colors_to_painting(mesh, previous, settings));
    CHECK(previous.face_colors == std::vector<std::array<size_t,3>>{{1,2,3}});
}

TEST_CASE("Explicit face target colors resolve against current physical slot order", "[ModelVertexColors][FaceColorOverride]")
{
    const std::array<size_t, 3> blue {102,140,182};
    std::vector<RGBA> physical {{1,0,0,1}, {0,0,0,1}, {1,1,1,1}, {0,1,0,1},
        {102.f/255.f,140.f/255.f,182.f/255.f,1}, {0.5f,0.5f,0.5f,1}};
    const auto original = match_clusters_to_filaments({blue}, physical, {});
    REQUIRE(original.size() == 1);
    CHECK(original[0].filament_index == 4);
    std::swap(physical[1], physical[4]);
    const auto reordered = match_clusters_to_filaments({blue}, physical, {});
    REQUIRE(reordered.size() == 1);
    CHECK(reordered[0].filament_index == 1);
    CHECK(physical.size() == 6);
}

TEST_CASE("Vertex color boundary splits preserve closed surfaces and color regions", "[ModelVertexColors][ColorBoundary]")
{
    const int color_count = GENERATE(1, 2, 3, 4);
    const int face_rotation = GENERATE(0, 1, 2);
    CAPTURE(color_count, face_rotation);
    using Color = std::array<size_t, 3>;
    const std::array<Color, 4> source_colors = {{{255,0,0}, {0,255,0}, {0,0,255}, {255,255,255}}};
    const std::array<Color, 4> target_colors = {{{20,30,40}, {50,100,150}, {160,170,180}, {200,210,220}}};
    TexturedMesh textured;
    textured.vertices = {{0,0,0}, {60,0,0}, {0,0,100}, {0,40,0}};
    textured.indices = {{0,2,1}, {0,1,3}, {1,2,3}, {2,0,3}};
    for (auto& face : textured.indices) {
        const auto original = face;
        for (int corner = 0; corner < 3; ++corner)
            face[corner] = original[(corner + face_rotation) % 3];
    }
    for (size_t i = 0; i < textured.vertices.size(); ++i) {
        const auto& color = source_colors[i % color_count];
        textured.precomputed_vertex_colors.push_back({color[0] / 255.f, color[1] / 255.f, color[2] / 255.f, 1.f});
    }
    textured.precomputed_face_colors.assign(textured.indices.size(), {128,128,128});
    TexturePaintingSettings settings;
    settings.smooth_weight = 0;
    settings.fixed_mapping_palette.assign(source_colors.begin(), source_colors.begin() + color_count);
    settings.fixed_palette.assign(target_colors.begin(), target_colors.begin() + color_count);

    indexed_triangle_set original;
    for (const auto& v : textured.vertices)
        original.vertices.emplace_back(v[0], v[1], v[2]);
    for (const auto& f : textured.indices)
        original.indices.emplace_back(f[0], f[1], f[2]);
    REQUIRE(its_num_open_edges(original) == 0);
    PaintedMesh painted;
    REQUIRE(face_colors_to_painting(textured, painted, settings));
    REQUIRE(painted.face_colors.size() == painted.indices.size());
    indexed_triangle_set result;
    for (const auto& v : painted.vertices)
        result.vertices.emplace_back(v[0], v[1], v[2]);
    for (const auto& f : painted.indices) {
        for (int index : f) {
            REQUIRE(index >= 0);
            REQUIRE(size_t(index) < result.vertices.size());
        }
        result.indices.emplace_back(f[0], f[1], f[2]);
    }
    CHECK(its_num_open_edges(result) == 0);
    CHECK_THAT(its_volume(result), Catch::Matchers::WithinRel(its_volume(original), 1e-6f));
    std::array<bool, 4> retained_corners {};
    std::map<Color, double> base_color_areas;
    for (size_t fi = 0; fi < result.indices.size(); ++fi) {
        const auto& face = result.indices[fi];
        const Vec3f& a = result.vertices[face[0]];
        const Vec3f& b = result.vertices[face[1]];
        const Vec3f& c = result.vertices[face[2]];
        const double area = (b - a).cross(c - a).norm() / 2.;
        CHECK(area > 0.);
        for (size_t vi = 0; vi < original.vertices.size(); ++vi) {
            if (a.isApprox(original.vertices[vi]) || b.isApprox(original.vertices[vi]) || c.isApprox(original.vertices[vi])) {
                retained_corners[vi] = true;
                CHECK(painted.face_colors[fi] == target_colors[vi % color_count]);
            }
        }
        if (std::abs(a.y()) < 1e-6f && std::abs(b.y()) < 1e-6f && std::abs(c.y()) < 1e-6f)
            base_color_areas[painted.face_colors[fi]] += area;
    }
    for (bool retained : retained_corners)
        CHECK(retained);
    if (color_count == 4) {
        // The y=0 face has its widest angle at the origin. The centre cut
        // leaves that corner a quarter of the area and gives each other color
        // three eighths, regardless of the face's cyclic index order.
        const double base_area = 60. * 100. / 2.;
        CHECK_THAT(base_color_areas[target_colors[0]], Catch::Matchers::WithinRel(base_area / 4., 1e-6));
        CHECK_THAT(base_color_areas[target_colors[1]], Catch::Matchers::WithinRel(base_area * 3. / 8., 1e-6));
        CHECK_THAT(base_color_areas[target_colors[2]], Catch::Matchers::WithinRel(base_area * 3. / 8., 1e-6));
    }

    Model model;
    ModelVolume* volume = model.add_object()->add_volume(TriangleMesh(original), ModelVolumeType::MODEL_PART, false);
    std::vector<FilamentMatch> matches;
    for (size_t i = 0; i < painted.cluster_colors.size(); ++i) {
        FilamentMatch match;
        match.cluster_index = int(i);
        match.filament_index = int(i % 3);
        matches.push_back(match);
    }
    REQUIRE(apply_painted_mesh_to_volume(painted, matches, *volume));
    CHECK(its_num_open_edges(volume->mesh().its) == 0);
    CHECK_THAT(its_volume(volume->mesh().its), Catch::Matchers::WithinRel(its_volume(original), 1e-6f));
    for (const auto& match : matches) {
        const auto state = static_cast<EnforcerBlockerType>(static_cast<int>(EnforcerBlockerType::Extruder1) + match.filament_index);
        CHECK(volume->mmu_segmentation_facets.has_facets(*volume, state));
    }
}

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
