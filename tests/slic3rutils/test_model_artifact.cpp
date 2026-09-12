#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp"
#include "libslic3r/Format/AssimpImport.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <boost/filesystem.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
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
    REQUIRE(model_artifact_sha256(source) == hash);
    REQUIRE_FALSE(write_model_artifact(output, mesh.its, colors.vertex_colors, error));
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
