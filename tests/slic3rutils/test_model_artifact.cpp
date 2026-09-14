#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp"
#include "libslic3r/Format/AssimpImport.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <boost/filesystem.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

using namespace Slic3r;
using namespace Slic3r::AI;
using Catch::Matchers::WithinAbs;
namespace {
struct Fixture {
    boost::filesystem::path directory = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca-glb-%%%%-%%%%-%%%%");
    Fixture() { boost::filesystem::create_directory(directory); }
    ~Fixture() { boost::system::error_code ec; boost::filesystem::remove_all(directory, ec); }
};
const boost::filesystem::path samples = boost::filesystem::path(std::string(TEST_DATA_DIR)) / "model_artifact";

std::array<int, 3> canonical_face(std::array<int, 3> face)
{
    const auto minimum = std::min_element(face.begin(), face.end());
    std::rotate(face.begin(), minimum, face.end());
    return face;
}

void require_closed_painted_mesh(const PaintedMesh& painted)
{
    // Count indexed edges, so coincident but unshared vertices and T-junctions
    // cannot be hidden by a later mesh repair or position-based edge lookup.
    std::map<std::array<int, 2>, std::pair<int, int>> edges;
    REQUIRE(painted.face_colors.size() == painted.indices.size());
    for (const auto& face : painted.indices) {
        for (int corner = 0; corner < 3; ++corner) {
            const int a = face[corner], b = face[(corner + 1) % 3];
            REQUIRE(a >= 0);
            REQUIRE(size_t(a) < painted.vertices.size());
            REQUIRE(a != b);
            auto& edge = edges[{std::min(a, b), std::max(a, b)}];
            ++edge.first;
            edge.second += a < b ? 1 : -1;
        }
    }
    for (const auto& edge : edges) {
        REQUIRE(edge.second.first == 2);
        REQUIRE(edge.second.second == 0);
    }
}
}

TEST_CASE("GLB textures and transformed scenes match the local analysis colors and print coordinates", "[ModelArtifact]") {
    for (const std::string name : {"textured", "transformed", "baseline", "uv-rotation", "uv-repeat",
                                  "uv-mirrored-repeat", "uv-clamp", "multi-material", "vertex-material-color",
                                  "ushort-vertex-colors", "nested-negative-nodes", "nested-negative-nodes-prepared"}) {
        DYNAMIC_SECTION(name) {
            TriangleMesh glb, expected; ObjInfo colors, expected_colors; std::string error;
            REQUIRE(load_model_artifact(samples / (name + ".glb"), glb, colors, error));
            REQUIRE(load_model_artifact(samples / (name + ".obj"), expected, expected_colors, error));
            REQUIRE(glb.its.vertices.size() == expected.its.vertices.size());
            REQUIRE(glb.its.indices.size() == expected.its.indices.size());
            // Assimp may reorder vertices while generating normals. Compare
            // the surface and its colors by position, not incidental indices.
            std::vector<bool> used(expected.its.vertices.size(), false);
            std::vector<int> remap(glb.its.vertices.size(), -1);
            for (size_t i = 0; i < glb.its.vertices.size(); ++i) {
                INFO("vertex " << i);
                size_t match = expected.its.vertices.size();
                for (size_t j = 0; j < expected.its.vertices.size(); ++j) {
                    bool same_color = true;
                    for (int c = 0; c < 3; ++c)
                        same_color = same_color && std::abs(colors.vertex_colors[i][c] - expected_colors.vertex_colors[j][c]) < .00001f;
                    if (!used[j] && same_color && (glb.its.vertices[i] - expected.its.vertices[j]).norm() < .001f) {
                        match = j; break;
                    }
                }
                REQUIRE(match < expected.its.vertices.size());
                used[match] = true;
                remap[i] = int(match);
                for (int c = 0; c < 3; ++c) {
                    REQUIRE_THAT(glb.its.vertices[i][c], WithinAbs(expected.its.vertices[match][c], .001));
                    REQUIRE_THAT(colors.vertex_colors[i][c], WithinAbs(expected_colors.vertex_colors[match][c], .00001));
                }
            }
            std::multiset<std::array<int, 3>> actual_faces, expected_faces;
            for (const auto& f : glb.its.indices)
                actual_faces.insert(canonical_face({remap[f[0]], remap[f[1]], remap[f[2]]}));
            for (const auto& f : expected.its.indices)
                expected_faces.insert(canonical_face({f[0], f[1], f[2]}));
            REQUIRE(actual_faces == expected_faces);
            if (name == "textured" || name == "multi-material") {
                Model native = Model::read_from_file((samples / (name + ".glb")).string());
                REQUIRE(native.texture_mesh);
                REQUIRE_FALSE(native.texture_mesh->textures.empty());
                CHECK(native.texture_mesh->precomputed_vertex_colors.empty());
                CHECK(native.texture_mesh->precomputed_face_colors.empty());
            }
        }
    }
}

TEST_CASE("Embedded JPEG textures retain their dimensions and projected colors", "[ModelArtifact][JPEG]") {
    const auto source = samples / "jpeg-textured.glb";
    TriangleMesh mesh; ObjInfo colors; std::string error;
    const bool loaded = load_model_artifact(source, mesh, colors, error);
    INFO(error);
    REQUIRE(loaded);
    REQUIRE(mesh.its.vertices.size() == 4);
    REQUIRE(mesh.its.indices.size() == 4);
    REQUIRE(colors.vertex_colors.size() == mesh.its.vertices.size());

    // Four quadrants in a 32 by 16 baseline JPEG, sampled at their centers.
    // Allow two code values for JPEG rounding, but catch swapped RGB/UV axes.
    const std::array<Vec3f, 4> positions { Vec3f(0, 0, 0), Vec3f(60, 0, 0), Vec3f(0, 0, 100), Vec3f(0, 40, 0) };
    const std::array<RGBA, 4> expected { RGBA{1, 0, 0, 1}, RGBA{0, 1, 0, 1}, RGBA{0, 0, 1, 1}, RGBA{1, 1, 1, 1} };
    for (size_t sample = 0; sample < positions.size(); ++sample) {
        const auto found = std::find_if(mesh.its.vertices.begin(), mesh.its.vertices.end(),
            [&](const Vec3f& v) { return (v - positions[sample]).norm() < .001f; });
        REQUIRE(found != mesh.its.vertices.end());
        const size_t index = size_t(std::distance(mesh.its.vertices.begin(), found));
        for (size_t channel = 0; channel < 3; ++channel)
            REQUIRE_THAT(colors.vertex_colors[index][channel], WithinAbs(expected[sample][channel], 2.0 / 255.0));
    }

    // Exercise the same image adapter headlessly, without wx image handlers.
    TexturedMesh textured;
    REQUIRE(load_assimp_textured_model(source.string(), textured, &error));
    REQUIRE(textured.textures.size() == 1);
    CHECK(textured.precomputed_vertex_colors.empty());
    CHECK(textured.precomputed_face_colors.empty());
    std::vector<unsigned char> pixels;
    int width = 0, height = 0;
    REQUIRE(decode_texture_to_pixels(textured.textures.front(), pixels, width, height));
    REQUIRE(width == 32);
    REQUIRE(height == 16);
    REQUIRE(pixels.size() == size_t(width) * height * 3);
}

TEST_CASE("Malformed or truncated embedded JPEG textures fail without publishing a model", "[ModelArtifact][JPEG]") {
    for (const std::string name : {"jpeg-malformed", "jpeg-truncated"}) {
        DYNAMIC_SECTION(name) {
            const auto source = samples / (name + ".glb");
            const auto hash = model_artifact_sha256(source);
            TriangleMesh mesh; ObjInfo colors; std::string error;
            bool loaded = true;
            REQUIRE_NOTHROW(loaded = load_model_artifact(source, mesh, colors, error));
            INFO(error);
            REQUIRE_FALSE(loaded);
            REQUIRE(error.find("texture") != std::string::npos);
            REQUIRE(mesh.empty());
            REQUIRE(colors.vertex_colors.empty());
            REQUIRE(model_artifact_sha256(source) == hash);
        }
    }
}

// Hidden because the input is an explicitly selected local asset, not a suite fixture.
// This probe only reads through the production loader; it does not edit or generate models.
TEST_CASE("An explicitly supplied local GLB loads without changing its source", "[ModelArtifact][.LocalArtifactProbe]") {
    const char* fixture = std::getenv("ORCASLICER_MODEL_ARTIFACT_FIXTURE");
    if (!fixture || !*fixture) SKIP("Set ORCASLICER_MODEL_ARTIFACT_FIXTURE to an existing local GLB.");
    const boost::filesystem::path source(fixture);
    REQUIRE(model_artifact_format(source) == "glb");
    const auto hash = model_artifact_sha256(source);
    REQUIRE_FALSE(hash.empty());
    TriangleMesh mesh; ObjInfo colors; std::string error;
    const auto started = std::chrono::steady_clock::now();
    const bool loaded = load_model_artifact(source, mesh, colors, error);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    INFO("Artifact: " << source.string() << "; SHA256: " << hash << "; load seconds: " << seconds << "; error: " << error);
    REQUIRE(loaded);
    REQUIRE_FALSE(mesh.empty());
    REQUIRE(colors.vertex_colors.size() == mesh.its.vertices.size());
    const bool finite_vertices = std::all_of(mesh.its.vertices.begin(), mesh.its.vertices.end(),
        [](const Vec3f& v) { return v.allFinite(); });
    const bool valid_colors = std::all_of(colors.vertex_colors.begin(), colors.vertex_colors.end(), [](const RGBA& color) {
        return std::all_of(color.begin(), color.end(), [](float value) { return std::isfinite(value) && value >= 0.f && value <= 1.f; });
    });
    INFO("Vertices: " << mesh.its.vertices.size() << "; triangles: " << mesh.its.indices.size());
    REQUIRE(finite_vertices);
    REQUIRE(valid_colors);
    REQUIRE(model_artifact_sha256(source) == hash);
}

TEST_CASE("Local GLB versions preserve geometry colors and the source file", "[ModelArtifact]") {
    Fixture f;
    const auto source = samples / "textured.glb";
    const auto hash = model_artifact_sha256(source);
    TriangleMesh mesh, loaded; ObjInfo colors, read_colors; std::string error;
    REQUIRE(load_model_artifact(source, mesh, colors, error));
    const auto output = f.directory / "edited.glb";
    colors.vertex_colors[0] = { .15f, .43f, .71f, 1.f };
    REQUIRE(write_model_artifact(output, mesh.its, colors.vertex_colors, error));
    REQUIRE(load_model_artifact(output, loaded, read_colors, error));
    REQUIRE(loaded.its.vertices.size() == mesh.its.vertices.size());
    for (size_t i = 0; i < mesh.its.vertices.size(); ++i) {
        size_t match = loaded.its.vertices.size();
        for (size_t j = 0; j < loaded.its.vertices.size(); ++j)
            if ((loaded.its.vertices[j] - mesh.its.vertices[i]).norm() < .001f) { match = j; break; }
        REQUIRE(match < loaded.its.vertices.size());
        for (int c = 0; c < 3; ++c) {
            REQUIRE_THAT(loaded.its.vertices[match][c], WithinAbs(mesh.its.vertices[i][c], .001));
            REQUIRE_THAT(read_colors.vertex_colors[match][c], WithinAbs(colors.vertex_colors[i][c], .00001));
        }
    }
    // File > Import and drag-and-drop use the default loader, without requesting
    // the optional raw Assimp colors used by the local artifact preview.
    Model native = Model::read_from_file(output.string());
    REQUIRE(native.texture_mesh);
    const auto& textured = *native.texture_mesh;
    REQUIRE(textured.textures.empty());
    REQUIRE(textured.precomputed_vertex_colors.size() == textured.vertices.size());
    REQUIRE(textured.precomputed_face_colors.size() == textured.indices.size());
    for (size_t i = 0; i < textured.vertices.size(); ++i) {
        const auto& p = textured.vertices[i];
        const Vec3f position = Vec3f(p[0], p[1], p[2]) * 1000.f;
        const auto found = std::find_if(mesh.its.vertices.begin(), mesh.its.vertices.end(),
            [&](const Vec3f& vertex) { return (vertex - position).norm() < .001f; });
        REQUIRE(found != mesh.its.vertices.end());
        const size_t original = size_t(std::distance(mesh.its.vertices.begin(), found));
        for (size_t channel = 0; channel < 3; ++channel)
            CHECK_THAT(textured.precomputed_vertex_colors[i][channel], WithinAbs(colors.vertex_colors[original][channel], .00001));
    }
    REQUIRE(model_artifact_sha256(source) == hash);
    REQUIRE_FALSE(write_model_artifact(output, mesh.its, colors.vertex_colors, error));
}

TEST_CASE("Ordinary GLB imports apply material factors to linear vertex colors", "[ModelArtifact]") {
    for (const std::string name : {"vertex-material-color", "ushort-vertex-colors"}) {
        DYNAMIC_SECTION(name) {
            Model native = Model::read_from_file((samples / (name + ".glb")).string());
            TriangleMesh expected; ObjInfo colors; std::string error;
            REQUIRE(load_model_artifact(samples / (name + ".obj"), expected, colors, error));
            REQUIRE(native.texture_mesh);
            const auto& textured = *native.texture_mesh;
            REQUIRE(textured.precomputed_vertex_colors.size() == textured.vertices.size());
            REQUIRE(textured.precomputed_face_colors.size() == textured.indices.size());
            REQUIRE_FALSE(colors.vertex_colors.empty());
            // These fixtures have a uniform COLOR_0 multiplied by a nonwhite factor.
            for (const auto& color : textured.precomputed_vertex_colors)
                for (size_t channel = 0; channel < 3; ++channel)
                    CHECK_THAT(color[channel], WithinAbs(colors.vertex_colors.front()[channel], .00001));
            for (const auto& color : textured.precomputed_face_colors)
                for (size_t channel = 0; channel < 3; ++channel)
                    CHECK_THAT(double(color[channel]), WithinAbs(colors.vertex_colors.front()[channel] * 255.f, 1.));
        }
    }
}

TEST_CASE("Locally recolored OBJ and GLB artifacts stay closed through native painting", "[ModelArtifact]") {
    for (const std::string extension : {"obj", "glb"}) {
        for (int split_edges : {0, 1, 2, 3}) {
            DYNAMIC_SECTION(extension << " with " << split_edges << " split edges on the recolored face") {
                Fixture fixture;
                indexed_triangle_set mesh;
                mesh.vertices = {Vec3f(0, 0, 0), Vec3f(20, 0, 0), Vec3f(0, 20, 0), Vec3f(0, 0, 20)};
                mesh.indices = {Vec3i(0, 2, 1), Vec3i(0, 1, 3), Vec3i(1, 2, 3), Vec3i(2, 0, 3)};
                const RGBA red{1, 0, 0, 1}, green{0, 1, 0, 1}, white{1, 1, 1, 1}, blue{0, 0, 1, 1};
                std::vector<RGBA> colors{red, red, red, green};
                if (split_edges >= 2) colors[1] = green;
                if (split_edges == 3) colors[2] = white;
                if (split_edges == 1) {
                    // A preexisting seam also makes a two-color neighbor split
                    // its equal-color edge to conform to the opposite side.
                    for (int corner = 0; corner < 3; ++corner) {
                        const Vec3f position = mesh.vertices[mesh.indices[1][corner]];
                        mesh.indices[1][corner] = int(mesh.vertices.size());
                        mesh.vertices.push_back(position);
                        colors.push_back(corner == 0 ? red : green);
                    }
                }
                std::string error;
                const auto source = fixture.directory / ("source." + extension);
                const auto output = fixture.directory / ("recolored." + extension);
                REQUIRE(write_model_artifact(source, mesh, colors, error));
                const auto source_hash = model_artifact_sha256(source);
                TriangleMesh loaded; ObjInfo loaded_colors;
                REQUIRE(load_model_artifact(source, loaded, loaded_colors, error));
                VertexColorRegionEditor editor;
                REQUIRE(editor.initialize(loaded.its, loaded_colors.vertex_colors, error));
                REQUIRE(editor.select_faces({0}) == 1);
                REQUIRE(editor.apply_color_to_obj_copy(blue, source, output, error));

                Model native = Model::read_from_file(output.string());
                REQUIRE(native.objects.size() == 1);
                REQUIRE(native.objects.front()->volumes.size() == 1);
                REQUIRE(native.texture_mesh);
                const auto& textured = *native.texture_mesh;
                REQUIRE(textured.precomputed_vertex_colors.size() == textured.vertices.size());
                REQUIRE(textured.precomputed_face_colors.size() == textured.indices.size());
                const auto& selected = textured.indices.front();
                const auto point = [&](int index) {
                    const auto& p = textured.vertices[index];
                    return Vec3f(p[0], p[1], p[2]);
                };
                const Vec3f origin = point(selected[0]);
                const Vec3f cross = (point(selected[1]) - origin).cross(point(selected[2]) - origin);
                const Vec3f normal = cross.normalized();
                TexturePaintingSettings settings;
                settings.fixed_palette = {{255, 0, 0}, {0, 255, 0}, {255, 255, 255}, {0, 0, 255}};
                settings.smooth_weight = 0;
                std::array<size_t, 3> selected_color{0, 0, 255};
                SECTION("saved face color") {}
                SECTION("explicit face color override") {
                    selected_color = {255, 255, 0};
                    settings.face_color_overrides.push_back({0, selected_color});
                }
                PaintedMesh painted;
                REQUIRE(face_colors_to_painting(textured, painted, settings));
                require_closed_painted_mesh(painted);
                size_t selected_children = 0;
                double selected_area = 0;
                for (size_t i = 0; i < painted.indices.size(); ++i) {
                    const auto& face = painted.indices[i];
                    const auto vertex = [&](int corner) {
                        const auto& p = painted.vertices[face[corner]];
                        return Vec3f(p[0], p[1], p[2]);
                    };
                    const Vec3f center = (vertex(0) + vertex(1) + vertex(2)) / 3.f;
                    if (std::abs((center - origin).dot(normal)) < 1e-6f) {
                        ++selected_children;
                        CHECK(painted.face_colors[i] == selected_color);
                        selected_area += (vertex(1) - vertex(0)).cross(vertex(2) - vertex(0)).norm();
                    } else {
                        CHECK(painted.face_colors[i] != selected_color);
                    }
                }
                CHECK(selected_children == size_t(split_edges + 1));
                CHECK_THAT(selected_area, WithinAbs(cross.norm(), cross.norm() * .00001));
                std::vector<FilamentMatch> matches(painted.cluster_colors.size());
                for (size_t i = 0; i < matches.size(); ++i) {
                    matches[i].cluster_index = int(i);
                    matches[i].filament_index = int(i);
                }
                auto& volume = *native.objects.front()->volumes.front();
                const double original_volume = its_volume(volume.mesh().its);
                REQUIRE(apply_painted_mesh_to_volume(painted, matches, volume));
                CHECK(volume.mesh().stats().manifold());
                CHECK_THAT(its_volume(volume.mesh().its), WithinAbs(original_volume, std::abs(original_volume) * .00001));
                CHECK_FALSE(volume.mmu_segmentation_facets.empty());
                CHECK(model_artifact_sha256(source) == source_hash);
            }
        }
    }
}

TEST_CASE("GLB finishing creates a separate version and supports cancellation", "[ModelArtifact]") {
    Fixture f;
    const auto source = samples / "textured.glb";
    const auto hash = model_artifact_sha256(source);
    ModelFinishingOptions options;
    const auto output = f.directory / "finished.glb";
    auto result = finish_model_artifact(source, output, options, [] { return true; });
    REQUIRE(result.canceled);
    REQUIRE_FALSE(boost::filesystem::exists(output));
    result = finish_model_artifact(source, output, options);
    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.source_sha256 == hash);
    REQUIRE(result.output_sha256 == model_artifact_sha256(output));
    REQUIRE(model_artifact_sha256(source) == hash);
    REQUIRE_FALSE(boost::filesystem::exists(output.string() + ".source.obj"));
}

TEST_CASE("GLB regional recoloring preserves unselected colors and the source editor", "[ModelArtifact]") {
    Fixture f;
    const auto source = samples / "textured.glb";
    const auto hash = model_artifact_sha256(source);
    TriangleMesh mesh, edited; ObjInfo colors, edited_colors; std::string error;
    REQUIRE(load_model_artifact(source, mesh, colors, error));
    VertexColorRegionEditor editor;
    REQUIRE(editor.initialize(mesh.its, colors.vertex_colors, error));
    REQUIRE(editor.select_faces({0}) == 1);
    const RGBA target { .12f, .34f, .56f, 1.f };
    const auto output = f.directory / "recolored.glb";
    REQUIRE(editor.apply_color_to_obj_copy(target, source, output, error));
    REQUIRE(load_model_artifact(output, edited, edited_colors, error));
    REQUIRE(editor.vertex_colors() == colors.vertex_colors);
    REQUIRE(edited.its.indices.size() == mesh.its.indices.size());
    REQUIRE(edited_colors.vertex_colors.size() == edited.its.vertices.size());
    // Boundary vertices may split to keep the unselected side's color. Check
    // each face corner rather than assuming exported vertex indices are stable.
    for (size_t face = 0; face < mesh.its.indices.size(); ++face) {
        for (int corner = 0; corner < 3; ++corner) {
            const int original = mesh.its.indices[face][corner];
            const int derived = edited.its.indices[face][corner];
            for (int c = 0; c < 3; ++c) {
                REQUIRE_THAT(edited.its.vertices[derived][c], WithinAbs(mesh.its.vertices[original][c], .001));
                REQUIRE_THAT(edited_colors.vertex_colors[derived][c],
                             WithinAbs(face == 0 ? target[c] : colors.vertex_colors[original][c], .00001));
            }
        }
    }
    REQUIRE(model_artifact_sha256(source) == hash);
}
