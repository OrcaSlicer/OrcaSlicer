#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "libslic3r/Point.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <sstream>
#include <set>
#include <vector>
#include <limits>

using namespace Slic3r;
using namespace Slic3r::AI;
using Catch::Matchers::WithinAbs;
namespace {
struct Fixture {
    boost::filesystem::path directory = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca-finishing-%%%%-%%%%-%%%%");
    boost::filesystem::path source = directory / "source.obj", output = directory / "edited.obj";
    Fixture() { boost::filesystem::create_directory(directory); }
    ~Fixture() { boost::system::error_code ec; boost::filesystem::remove_all(directory, ec); }
    void write(const std::string& text) { boost::filesystem::ofstream file(source); file << text; }
};
std::string read(const boost::filesystem::path& path) {
    boost::filesystem::ifstream file(path); return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::vector<Vec3d> positions(const std::string& text) {
    std::vector<Vec3d> result; std::istringstream stream(text); std::string line;
    while (std::getline(stream, line)) {
        std::istringstream row(line); std::string tag; row >> tag;
        if (tag == "v") { Vec3d p; row >> p.x() >> p.y() >> p.z(); result.push_back(p); }
    }
    return result;
}
std::string noisy_grid(int width) {
    std::ostringstream mesh;
    for (int y = 0; y < width; ++y) for (int x = 0; x < width; ++x) {
        const double noise = x && y && x < width-1 && y < width-1 ? ((x+y)%2 ? 0.045 : -0.045) : 0.0;
        mesh << "v " << x << ' ' << y << ' ' << noise << " 0.127 0.513 0.947 1 # preserved color\n";
    }
    for (int y = 0; y < width-1; ++y) for (int x = 0; x < width-1; ++x) {
        const int a = y*width+x+1, b=a+1, c=a+width, d=c+1;
        mesh << "f " << a << ' ' << b << ' ' << d << "\nf " << a << ' ' << d << ' ' << c << '\n';
    }
    return mesh.str();
}
const char* tetrahedron =
    "mtllib source.mtl\no portrait\ng skin\nusemtl skin\n"
    "v 0 0 0 0.12 0.54 0.91\nv 10 0 0 0.22 0.64 0.81\nv 0 10 0 0.32 0.74 0.71\nv 0 0 10 0.42 0.84 0.61\n"
    "vt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 -1\n"
    "f 1/1/1 3/3/1 2/2/1\nf 1 2 4\nf 2 3 4\nf 3 1 4\n";

const char* colored_square =
    "v 0 0 0 1 0 0\nv 10 0 0 1 0 0\nv 0 10 0 1 0 0\nv 10 10 0 0.2 0.3 0.4\n"
    "f 1 2 3\nf 2 4 3\n";

ModelFinishingOptions recolor_options() {
    ModelFinishingOptions options;
    options.smooth_surface = false;
    options.repair_mesh = false;
    options.recolor_selected = true;
    options.selected_faces = {0};
    options.target_color = {0, 0, 1, 1};
    return options;
}

bool has_recolor_stage(const boost::filesystem::path& directory) {
    for (const auto& entry : boost::filesystem::directory_iterator(directory))
        if (entry.path().filename().string().find(".recolor-") == 0) return true;
    return false;
}
}

TEST_CASE("Local recoloring creates a comparable model version with exact isolated face colors", "[ModelFinishing][LocalRecolor]")
{
    const auto source_format = GENERATE(std::string("obj"), std::string("glb"));
    const auto destination_format = GENERATE(std::string("obj"), std::string("glb"));
    Fixture f;
    f.write(colored_square);
    std::string error;
    TriangleMesh original;
    ObjInfo colors;
    REQUIRE(load_model_artifact(f.source, original, colors, error));
    if (source_format == "glb") {
        const auto glb = f.directory / "source.glb";
        REQUIRE(write_model_artifact(glb, original.its, colors.vertex_colors, error));
        f.source = glb;
        REQUIRE(load_model_artifact(f.source, original, colors, error));
    }
    f.output = f.directory / ("edited." + destination_format);
    const auto before = read(f.source);
    auto options = recolor_options();
    options.selected_faces = {0, 0}; // Duplicate selection IDs are a single edited face.
    const auto result = finish_model_artifact(f.source, f.output, options);
    INFO(result.error);
    REQUIRE(result.success);
    CHECK(result.changed());
    CHECK(result.recolored_faces == 1);
    CHECK(result.faces_before == 2);
    CHECK(result.faces_after == 2);
    CHECK(result.moved_vertices == 0);
    CHECK(result.source_sha256 == model_artifact_sha256(f.source));
    CHECK(result.output_sha256 == model_artifact_sha256(f.output));
    CHECK(read(f.source) == before);
    CHECK_FALSE(has_recolor_stage(f.directory));
    CHECK_THAT(result.dimensions[0], WithinAbs(10, 1e-5));
    CHECK_THAT(result.dimensions[1], WithinAbs(10, 1e-5));
    CHECK_THAT(result.dimensions[2], WithinAbs(0, 1e-5));
    TriangleMesh edited;
    ObjInfo edited_colors;
    REQUIRE(load_model_artifact(f.output, edited, edited_colors, error));
    REQUIRE(edited.its.indices.size() == original.its.indices.size());
    CHECK(result.vertices == edited.its.vertices.size());
    for (size_t face = 0; face < 2; ++face) for (size_t corner = 0; corner < 3; ++corner) {
        const int original_vertex = original.its.indices[face][corner];
        const int edited_vertex = edited.its.indices[face][corner];
        const auto expected = face == 0 ? options.target_color : colors.vertex_colors[original_vertex];
        for (size_t axis = 0; axis < 3; ++axis)
            CHECK_THAT(edited.its.vertices[edited_vertex][axis], WithinAbs(original.its.vertices[original_vertex][axis], 1e-5));
        for (size_t channel = 0; channel < 4; ++channel)
            CHECK_THAT(edited_colors.vertex_colors[edited_vertex][channel], WithinAbs(expected[channel], 1e-6));
    }
}

TEST_CASE("Local recoloring reports an unchanged selected face without changing geometry", "[ModelFinishing][LocalRecolor]")
{
    Fixture f; f.write(colored_square);
    auto options = recolor_options();
    options.target_color = {1, 0, 0, 1};
    const auto result = finish_model_obj(f.source, f.output, options);
    INFO(result.error);
    REQUIRE(result.success);
    CHECK_FALSE(result.changed());
    CHECK(result.recolored_faces == 0);
    CHECK(result.vertices == 4);
}

TEST_CASE("Local recoloring rejects incomplete or conflicting requests before creating output", "[ModelFinishing][LocalRecolor]")
{
    const auto invalid = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    Fixture f; f.write(colored_square);
    const auto original = read(f.source);
    auto options = recolor_options();
    switch (invalid) {
    case 0: options.selected_faces.clear(); break;
    case 1: options.selected_faces = {0, 2}; break;
    case 2: options.smooth_surface = true; break;
    case 3: options.repair_mesh = true; break;
    case 4: options.clean_color_spots = true; break;
    case 5: options.target_color[0] = std::numeric_limits<float>::quiet_NaN(); break;
    case 6: options.target_color[1] = -0.1f; break;
    case 7: options.target_color[3] = 1.1f; break;
    }
    const auto result = finish_model_artifact(f.source, f.output, options);
    CHECK_FALSE(result.success);
    CHECK_FALSE(result.error.empty());
    CHECK_FALSE(boost::filesystem::exists(f.output));
    CHECK_FALSE(has_recolor_stage(f.directory));
    CHECK(read(f.source) == original);
}

TEST_CASE("Canceling a staged local recolor leaves the source and earlier versions intact", "[ModelFinishing][LocalRecolor]")
{
    const auto format = GENERATE(std::string("obj"), std::string("glb"));
    const bool after_write = GENERATE(false, true);
    Fixture f; f.write(colored_square);
    f.output = f.directory / ("edited." + format);
    const auto original = read(f.source);
    const auto earlier = f.directory / "earlier.obj";
    { boost::filesystem::ofstream stream(earlier); stream << "earlier version"; }
    const auto result = finish_model_artifact(f.source, f.output, recolor_options(), [&] {
        return !after_write || has_recolor_stage(f.directory);
    });
    CHECK(result.canceled);
    CHECK_FALSE(result.success);
    CHECK_FALSE(boost::filesystem::exists(f.output));
    CHECK_FALSE(has_recolor_stage(f.directory));
    CHECK(read(f.source) == original);
    CHECK(read(earlier) == "earlier version");
}

TEST_CASE("Local recoloring rejects a source changed during processing without publishing its candidate", "[ModelFinishing][LocalRecolor]")
{
    Fixture f; f.write(colored_square);
    bool changed_source = false;
    const auto result = finish_model_artifact(f.source, f.output, recolor_options(), [&] {
        if (!changed_source && has_recolor_stage(f.directory)) {
            boost::filesystem::ofstream stream(f.source, std::ios::app);
            stream << "# another edit\n";
            changed_source = true;
        }
        return false;
    });
    CHECK(changed_source);
    CHECK_FALSE(result.success);
    CHECK(result.error.find("source model changed") != std::string::npos);
    CHECK_FALSE(boost::filesystem::exists(f.output));
    CHECK_FALSE(has_recolor_stage(f.directory));
    CHECK(read(f.source).find("# another edit") != std::string::npos);
}

TEST_CASE("Surface finishing reduces noise while preserving open boundaries and vertex colors", "[ModelFinishing]")
{
    Fixture f; f.write(noisy_grid(13)); const auto original = read(f.source);
    const auto before = positions(original);
    const auto result = finish_model_obj(f.source, f.output, {true, false, 0.8});
    INFO(result.error); REQUIRE(result.success); REQUIRE(result.moved_vertices > 0);
    const auto after = positions(read(f.output)); REQUIRE(after.size() == before.size());
    double rough_before = 0, rough_after = 0;
    for (size_t i = 0; i < before.size(); ++i) {
        REQUIRE((after[i] - before[i]).norm() <= result.displacement_limit + 1e-10);
        if (i%13 == 0 || i%13 == 12 || i < 13 || i >= 156)
            REQUIRE_THAT((after[i]-before[i]).norm(), WithinAbs(0, 1e-12));
        rough_before += std::abs(before[i].z()); rough_after += std::abs(after[i].z());
    }
    REQUIRE(rough_after < rough_before);
    REQUIRE(read(f.source) == original);
    std::istringstream edited(read(f.output)); std::string line; size_t color_rows = 0;
    while (std::getline(edited, line)) if (line.rfind("v ", 0) == 0) {
        REQUIRE(line.find(" 0.127 0.513 0.947 1 # preserved color") != std::string::npos); ++color_rows;
    }
    REQUIRE(color_rows == before.size());
    REQUIRE(result.boundary_edges == 48);
    REQUIRE(result.source_sha256.size() == 64);
    REQUIRE(result.output_sha256.size() == 64);
    REQUIRE(result.source_sha256 != result.output_sha256);
    REQUIRE_THAT(result.dimensions[0], WithinAbs(12, 1e-9));
    REQUIRE_THAT(result.dimensions[1], WithinAbs(12, 1e-9));
}

TEST_CASE("Mesh repair removes duplicate and degenerate faces without rewriting material and UV data", "[ModelFinishing]")
{
    Fixture f; f.write(std::string(tetrahedron) + "f 1 3 2\nf 1 1 2\n");
    const auto original = read(f.source);
    const auto result = finish_model_obj(f.source, f.output, {false, true, 0});
    INFO(result.error); REQUIRE(result.success);
    REQUIRE(result.faces_after == 4); REQUIRE(result.removed_degenerate_faces == 1);
    REQUIRE(result.removed_duplicate_faces == 1); REQUIRE(result.moved_vertices == 0);
    REQUIRE(result.boundary_edges == 0); REQUIRE(result.nonmanifold_edges == 0);
    const auto output = read(f.output);
    REQUIRE(output.find("mtllib source.mtl\no portrait\ng skin\nusemtl skin") != std::string::npos);
    REQUIRE(output.find("vt 0 0\nvt 1 0\nvt 0 1") != std::string::npos);
    REQUIRE(output.find("f 1/1/1 3/3/1 2/2/1") != std::string::npos);
    REQUIRE(read(f.source) == original);
}

TEST_CASE("Surface finishing leaves irregular planar texture samples in place", "[ModelFinishing]")
{
    Fixture f;
    // An interior sample deliberately has an uneven one-ring. Tangential
    // Laplacian movement would slide its texture despite a perfectly flat surface.
    f.write("v 0 0 0\nv 4 0 0\nv 4 4 0\nv 0 4 0\nv 0.2 0.3 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvt 0.05 0.075\n"
        "f 1/1 2/2 5/5\nf 2/2 3/3 5/5\nf 3/3 4/4 5/5\nf 4/4 1/1 5/5\n");
    const auto source = read(f.source);
    const auto result = finish_model_obj(f.source, f.output, {true, false, 1});
    INFO(result.error); REQUIRE(result.success);
    REQUIRE(result.moved_vertices == 0);
    REQUIRE(read(f.output) == source);
}

TEST_CASE("Surface finishing preserves local fold geometry while completing other regions", "[ModelFinishing]")
{
    Fixture f;
    // Uneven sampling includes a narrow triangle susceptible to folding.
    // Keep sharp/folded edges fixed before smoothing; the orientation checks
    // below also cover the local fallback if any other triangle would fold.
    std::ostringstream mesh;
    mesh << "v 0 0 0.005007293\nv 1 0 -0.118409738\nv 2 0 0.299733844\nv 3 0 -0.498839113\nv 4 0 -0.245717933\n"
        "v 0 1 0.190129158\nv 0.955564992 1.173681191 0.046488245\nv 1.977206773 1.724371200 0.098844178\n"
        "v 3.185318295 0.456476483 -0.063756150\nv 4.223699949 0.407801817 0.391084243\n"
        "v 0.119223289 2.009077671 0.263756959\nv 0.652465207 1.560967222 -0.223520905\n"
        "v 2.836237072 2.173199399 0.134439863\nv 2.872491832 1.618618113 0.172842396\n"
        "v 5.203749977 1.716258830 0.203584990\nv 0.377945868 3.196679513 -0.116133856\n"
        "v 1.329622223 3.313735966 0.336259680\nv 2.617424879 3.090373233 0.043981557\n"
        "v 2.586007925 3.355399637 0.060499558\nv 4 3 -0.170122979\n"
        "v 0 4 0.219210829\nv 1 4 -0.073388398\nv 2 4 -0.013353635\nv 3 4 -0.029331671\nv 4 4 -0.095983811\n";
    std::vector<std::array<size_t, 3>> faces;
    for (size_t y = 0; y < 4; ++y) for (size_t x = 0; x < 4; ++x) {
        const size_t a = y * 5 + x;
        faces.push_back({a, a + 1, a + 6}); faces.push_back({a, a + 6, a + 5});
    }
    for (const auto& face : faces) mesh << "f " << face[0] + 1 << ' ' << face[1] + 1 << ' ' << face[2] + 1 << '\n';
    // An independent gently noisy patch must still improve when every vertex
    // of the folded component is conservatively pinned.
    std::istringstream gentle(noisy_grid(7)); std::string row;
    while (std::getline(gentle, row)) {
        std::istringstream values(row); std::string tag; values >> tag;
        if (tag == "v") {
            double x, y, z; values >> x >> y >> z;
            mesh << "v " << x + 20 << ' ' << y << ' ' << z << '\n';
        } else if (tag == "f") {
            std::array<size_t, 3> face; values >> face[0] >> face[1] >> face[2];
            for (auto& vertex : face) vertex += 24;
            faces.push_back(face);
            mesh << "f " << face[0] + 1 << ' ' << face[1] + 1 << ' ' << face[2] + 1 << '\n';
        }
    }
    f.write(mesh.str());
    const auto before = positions(mesh.str());
    const auto result = finish_model_obj(f.source, f.output, {true, false, 1});
    INFO(result.error); REQUIRE(result.success);
    REQUIRE(result.moved_vertices > 0);
    const auto after = positions(read(f.output)); REQUIRE(after.size() == before.size());
    size_t reversed_shared_edges = 0;
    for (size_t i = 0; i < faces.size(); ++i) for (size_t j = i + 1; j < faces.size(); ++j) {
        std::vector<size_t> shared;
        for (const size_t a : faces[i])
            if (std::find(faces[j].begin(), faces[j].end(), a) != faces[j].end()) shared.push_back(a);
        if (shared.size() != 2) continue;
        auto normal = [&](const auto& face) -> Vec3d {
            return (before[face[1]] - before[face[0]]).cross(before[face[2]] - before[face[0]]);
        };
        if (normal(faces[i]).dot(normal(faces[j])) >= 0) continue;
        ++reversed_shared_edges;
        for (const size_t vertex : shared)
            REQUIRE_THAT((after[vertex] - before[vertex]).norm(), WithinAbs(0, 1e-12));
    }
    REQUIRE(reversed_shared_edges > 0);
    for (const auto& face : faces) {
        const Vec3d n0 = (before[face[1]] - before[face[0]]).cross(before[face[2]] - before[face[0]]);
        const Vec3d n1 = (after[face[1]] - after[face[0]]).cross(after[face[2]] - after[face[0]]);
        REQUIRE(n0.dot(n1) > 0);
        for (int k = 0; k < 3; ++k) {
            const size_t a = face[k], b = face[(k + 1) % 3];
            const double bound = std::min(result.displacement_limit, 0.2 * (before[a] - before[b]).norm());
            REQUIRE((after[a] - before[a]).norm() <= bound + 1e-10);
            REQUIRE((after[b] - before[b]).norm() <= bound + 1e-10);
        }
    }
    REQUIRE(read(f.source) == mesh.str());
}

TEST_CASE("Mesh repair makes a closed component face consistently outward", "[ModelFinishing]")
{
    Fixture f;
    f.write("v 0 0 0\nv 10 0 0\nv 0 10 0\nv 0 0 10\nf 1 2 3\nf 1 2 4\nf 2 4 3\nf 3 4 1\n");
    const auto result = finish_model_obj(f.source, f.output, {false, true, 0});
    INFO(result.error); REQUIRE(result.success); REQUIRE(result.reversed_faces > 0);
    const auto vertices = positions(read(f.output)); double volume = 0;
    std::istringstream input(read(f.output)); std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line); std::string tag; row >> tag;
        if (tag == "f") { size_t a,b,c; row >> a >> b >> c; volume += vertices[a-1].dot(vertices[b-1].cross(vertices[c-1])); }
    }
    REQUIRE(volume > 0); REQUIRE(result.boundary_edges == 0);
}

TEST_CASE("Finishing preserves sharp features instead of rounding a tetrahedron", "[ModelFinishing]")
{
    Fixture f; f.write(tetrahedron);
    const auto result = finish_model_obj(f.source, f.output, {true, true, 1});
    INFO(result.error); REQUIRE(result.success); REQUIRE(result.moved_vertices == 0);
}

TEST_CASE("Finishing cancellation discards only its incomplete output", "[ModelFinishing]")
{
    Fixture f; f.write(noisy_grid(80)); const auto original = read(f.source);
    unsigned checks = 0;
    const auto result = finish_model_obj(f.source, f.output, {true, true, 1}, [&] { return ++checks > 5; });
    REQUIRE(result.canceled); REQUIRE_FALSE(result.success);
    REQUIRE_FALSE(boost::filesystem::exists(f.output));
    REQUIRE_FALSE(boost::filesystem::exists(f.output.string()+".partial"));
    REQUIRE(read(f.source) == original);
}

TEST_CASE("Finishing rejects invalid input and cannot overwrite its source", "[ModelFinishing]")
{
    Fixture f;
    for (const std::string input : {"v nan 0 0\nf 1 1 1\n", "v 0 0 0\nf 1 2 3\n", "v 0 0 0\nf 1 1 1 1\n"}) {
        f.write(input);
        const auto result = finish_model_obj(f.source, f.output, {});
        REQUIRE_FALSE(result.success); REQUIRE_FALSE(result.error.empty());
        REQUIRE_FALSE(boost::filesystem::exists(f.output)); REQUIRE(read(f.source) == input);
    }
    f.write(tetrahedron); const auto source = read(f.source);
    REQUIRE_FALSE(finish_model_obj(f.source, f.source, {}).success);
    REQUIRE(read(f.source) == source);
    REQUIRE_FALSE(finish_model_obj(f.source, f.output, {true, true, -0.1}).success);
    REQUIRE_FALSE(boost::filesystem::exists(f.output));
}

TEST_CASE("Local smoothing changes only the interior of selected source faces and preserves outside shading", "[ModelFinishing]")
{
    Fixture f;
    // OBJ face ordinals include both triangles in each cell. Interspersed
    // material/group records do not consume an ordinal. One explicit normal
    // is shared by selected and unselected faces and must remain untouched.
    std::istringstream grid(noisy_grid(13)); std::ostringstream mesh;
    mesh << "mtllib portrait.mtl\nvt 0.25 0.75\nvn 0 1 0\n";
    std::string line;
    while (std::getline(grid, line)) {
        if (line.rfind("f ", 0) != 0) { mesh << line << '\n'; continue; }
        std::istringstream row(line); std::string tag; size_t a, b, c;
        row >> tag >> a >> b >> c;
        mesh << "g cheek\nusemtl skin\nf " << a << "/1/1 " << b << "/1/1 " << c << "/1/1\n";
    }
    f.write(mesh.str());
    ModelFinishingOptions options {true, false, 0.8};
    for (size_t y = 2; y < 7; ++y) for (size_t x = 2; x < 7; ++x) {
        const size_t first = 2 * (y * 12 + x);
        options.selected_faces.push_back(first); options.selected_faces.push_back(first + 1);
    }
    const auto before = positions(mesh.str());
    const auto result = finish_model_obj(f.source, f.output, options);
    INFO(result.error); REQUIRE(result.success); REQUIRE(result.moved_vertices > 0);
    const auto output = read(f.output); const auto after = positions(output);
    REQUIRE(after.size() == before.size());
    double rough_before = 0, rough_after = 0;
    for (size_t i = 0; i < before.size(); ++i) {
        const size_t x = i % 13, y = i / 13;
        if (x <= 2 || x >= 7 || y <= 2 || y >= 7)
            REQUIRE_THAT((after[i] - before[i]).norm(), WithinAbs(0, 1e-12));
        else {
            rough_before += std::abs(before[i].z()); rough_after += std::abs(after[i].z());
        }
        REQUIRE((after[i] - before[i]).norm() <= result.displacement_limit + 1e-10);
    }
    REQUIRE(rough_after < rough_before);
    // Only coordinates of movable vertices may differ. All face records,
    // material switches, UVs, shared normals and outside vertices retain text.
    std::istringstream original_rows(mesh.str()), edited_rows(output); std::string edited;
    while (std::getline(original_rows, line)) {
        REQUIRE(static_cast<bool>(std::getline(edited_rows, edited)));
        if (line.rfind("v ", 0) != 0) REQUIRE(edited == line);
        else REQUIRE(edited.find(" 0.127 0.513 0.947 1 # preserved color") != std::string::npos);
    }
    REQUIRE_FALSE(static_cast<bool>(std::getline(edited_rows, edited)));
    REQUIRE(result.faces_after == result.faces_before);
    REQUIRE(result.removed_degenerate_faces == 0);
    REQUIRE(result.removed_duplicate_faces == 0);
    REQUIRE(result.reversed_faces == 0);
    REQUIRE(read(f.source) == mesh.str());
}

TEST_CASE("A selection without interior vertices leaves the model unchanged", "[ModelFinishing]")
{
    Fixture f; f.write(noisy_grid(7));
    const auto original = read(f.source);
    ModelFinishingOptions options {true, false, 1};
    options.selected_faces = {0, 28, 71};
    const auto result = finish_model_obj(f.source, f.output, options);
    INFO(result.error); REQUIRE(result.success);
    REQUIRE(result.moved_vertices == 0);
    REQUIRE_FALSE(result.changed());
    REQUIRE(read(f.output) == original);
}

TEST_CASE("Local finishing rejects stale face indices and whole mesh repair without producing an output", "[ModelFinishing]")
{
    Fixture f; f.write(noisy_grid(7)); const auto original = read(f.source);
    ModelFinishingOptions options {true, false, 0.5};
    options.selected_faces = {0, 72}; // 6 x 6 cells, two faces each: valid 0..71.
    auto result = finish_model_obj(f.source, f.output, options);
    REQUIRE_FALSE(result.success);
    REQUIRE(result.error.find("selected face index") != std::string::npos);
    REQUIRE_FALSE(boost::filesystem::exists(f.output));
    REQUIRE_FALSE(boost::filesystem::exists(f.output.string() + ".partial"));
    options.selected_faces = {0}; options.repair_mesh = true;
    result = finish_model_obj(f.source, f.output, options);
    REQUIRE_FALSE(result.success);
    REQUIRE(result.error.find("Disable mesh repair") != std::string::npos);
    REQUIRE_FALSE(boost::filesystem::exists(f.output));
    REQUIRE(read(f.source) == original);
}

TEST_CASE("Repeated face picks have the same effect as a unique selection", "[ModelFinishing]")
{
    Fixture f; f.write(noisy_grid(7));
    ModelFinishingOptions options {true, false, 0.8};
    // Every triangle incident to the center vertex (index 24).
    options.selected_faces = {28, 29, 31, 40, 42, 43};
    const auto first = finish_model_obj(f.source, f.output, options);
    INFO(first.error); REQUIRE(first.success); REQUIRE(first.moved_vertices > 0);
    const auto expected = read(f.output);
    options.selected_faces.insert(options.selected_faces.end(), {28, 43, 28, 40});
    const auto second_output = f.directory / "repeated.obj";
    const auto second = finish_model_obj(f.source, second_output, options);
    INFO(second.error); REQUIRE(second.success);
    REQUIRE(read(second_output) == expected);
}

TEST_CASE("Local smoothing fades gently into the fixed selection border", "[ModelFinishing]")
{
    Fixture f; f.write(noisy_grid(23));
    const auto before = positions(read(f.source));
    ModelFinishingOptions options {true, false, 0.8};
    for (size_t y = 3; y < 19; ++y) for (size_t x = 3; x < 19; ++x) {
        const size_t first = 2 * (y * 22 + x);
        options.selected_faces.push_back(first); options.selected_faces.push_back(first + 1);
    }
    const auto result = finish_model_obj(f.source, f.output, options);
    INFO(result.error); REQUIRE(result.success); REQUIRE(result.moved_vertices > 0);
    const auto after = positions(read(f.output));
    double border_movement = 0, interior_movement = 0;
    for (size_t y = 8; y < 15; ++y) {
        REQUIRE_THAT((after[y * 23 + 3] - before[y * 23 + 3]).norm(), WithinAbs(0, 1e-12));
        border_movement += (after[y * 23 + 4] - before[y * 23 + 4]).norm();
        interior_movement += (after[y * 23 + 11] - before[y * 23 + 11]).norm();
    }
    REQUIRE(border_movement > 0);
    REQUIRE(border_movement < interior_movement * 0.5);
    // Tapering may not invert even the small triangles at the selection edge.
    for (size_t y = 0; y < 22; ++y) for (size_t x = 0; x < 22; ++x) {
        const size_t a = y * 23 + x;
        for (const auto& face : {std::array<size_t, 3>{a, a + 1, a + 24}, {a, a + 24, a + 23}}) {
            const Vec3d n0 = (before[face[1]] - before[face[0]]).cross(before[face[2]] - before[face[0]]);
            const Vec3d n1 = (after[face[1]] - after[face[0]]).cross(after[face[2]] - after[face[0]]);
            REQUIRE(n0.dot(n1) > 0);
        }
    }
}

TEST_CASE("Canceling after output creation removes the partial model and preserves the source", "[ModelFinishing]")
{
    Fixture f; f.write(noisy_grid(25)); const auto original = read(f.source);
    const auto partial = boost::filesystem::path(f.output.string() + ".partial");
    const auto result = finish_model_obj(f.source, f.output, {true, false, 0.7}, [&] {
        return boost::filesystem::exists(partial);
    });
    REQUIRE(result.canceled); REQUIRE_FALSE(result.success);
    REQUIRE_FALSE(boost::filesystem::exists(partial));
    REQUIRE_FALSE(boost::filesystem::exists(f.output));
    REQUIRE(read(f.source) == original);
}

namespace {
std::string colored_grid(int width, const std::function<bool(int, int)>& dark, int offset = 0)
{
    std::ostringstream mesh;
    mesh << "# vertex RGB cleanup fixture\n\n";
    for (int y = 0; y < width; ++y) for (int x = 0; x < width; ++x)
        mesh << "\tv  " << x << ".000\t" << y << " 0.0  "
             << (dark(x,y) ? "0.02 0.02 0.02" : "0.12 0.32 0.16") << "\t0.73 # vertex " << y*width+x << '\n';
    mesh << "vt 0.5 0.5\nvn 0 0 1\nusemtl collar\n";
    for (int y = 0; y < width-1; ++y) for (int x = 0; x < width-1; ++x) {
        const int a = offset + y*width+x+1, b=a+1, c=a+width, d=c+1;
        mesh << "f " << a << "/1/1 " << b << "/1/1 " << d << "/1/1 # first\nf "
             << a << "/1/1 " << d << "/1/1 " << c << "/1/1\n";
    }
    return mesh.str();
}
ModelFinishingOptions cleanup_options(int width, int from, int to)
{
    ModelFinishingOptions options {false, false, 1};
    options.clean_color_spots = true;
    options.cleanup_palette = {{.02f,.02f,.02f}, {.12f,.32f,.16f}};
    for (int y = from; y < to; ++y) for (int x = from; x < to; ++x) {
        const size_t first = 2 * (y * (width - 1) + x);
        options.selected_faces.push_back(first); options.selected_faces.push_back(first + 1);
    }
    return options;
}
std::set<std::string> color_values(const std::string& text)
{
    std::istringstream input(text); std::string line; std::set<std::string> values;
    while (std::getline(input, line)) {
        std::istringstream row(line); std::string tag, x, y, z, r, g, b;
        if (row >> tag && tag == "v") { row >> x >> y >> z >> r >> g >> b; values.insert(r + " " + g + " " + b); }
    }
    return values;
}
}

TEST_CASE("Color cleanup removes enclosed speckles and preserves boundaries outside colors and large details", "[ModelFinishing][ColorCleanup]")
{
    Fixture f;
    f.write(colored_grid(31, [](int x, int y) {
        return (x == 8 && y == 8) || (x == 25 && y == 25) || (x == 2 && y == 8) ||
            (x >= 12 && x <= 18 && y >= 12 && y <= 18);
    }));
    const auto before = read(f.source);
    const auto result = finish_model_obj(f.source, f.output, cleanup_options(31, 2, 22));
    INFO(result.error); REQUIRE(result.success); REQUIRE(result.changed());
    REQUIRE(result.cleaned_color_regions == 1); REQUIRE(result.recolored_vertices == 1);
    REQUIRE(result.moved_vertices == 0); REQUIRE(result.faces_after == result.faces_before);
    const auto after = read(f.output);
    auto expected = before;
    const auto noise = expected.find("\tv  8.000\t8 0.0  0.02 0.02 0.02");
    REQUIRE(noise != std::string::npos);
    expected.replace(noise, std::string("\tv  8.000\t8 0.0  0.02 0.02 0.02").size(), "\tv  8.000\t8 0.0  0.12 0.32 0.16");
    // Exact equality protects normals, UV/material/face records, alpha, comments,
    // coordinate spelling, tabs, blank lines, and every unselected vertex.
    REQUIRE(after == expected); REQUIRE(read(f.source) == before);
    const auto source_colors = color_values(before), output_colors = color_values(after);
    for (const auto& color : output_colors) REQUIRE(source_colors.count(color) == 1);
}

TEST_CASE("Color cleanup budgets each disconnected selection separately", "[ModelFinishing][ColorCleanup]")
{
    Fixture f;
    // One vertex covers 1/4 of the small patch; a far larger selected component
    // must not inflate its threshold and erase this deliberate color region.
    f.write(colored_grid(31, [](int, int) { return false; }) +
        colored_grid(3, [](int x, int y) { return x == 1 && y == 1; }, 31*31));
    auto options = cleanup_options(31, 0, 30);
    for (size_t i = 1800; i < 1808; ++i) options.selected_faces.push_back(i);
    const auto result = finish_model_obj(f.source, f.output, options);
    INFO(result.error); REQUIRE(result.success); REQUIRE_FALSE(result.changed());
    REQUIRE(read(f.output) == read(f.source));
}

TEST_CASE("Color cleanup validates selected source colors before writing and never guesses texture colors", "[ModelFinishing][ColorCleanup]")
{
    Fixture f; auto options = cleanup_options(7, 0, 6);
    for (const std::string invalid : {"", "1 0", "0 0 2", "nan 0 0", "0 0 0 1 2", "0 0 0 nope"}) {
        std::string model = colored_grid(7, [](int,int) { return false; });
        const auto color = model.find("0.12 0.32 0.16\t0.73");
        model.replace(color, std::string("0.12 0.32 0.16\t0.73").size(), invalid);
        f.write(model);
        const auto result = finish_model_obj(f.source, f.output, options);
        REQUIRE_FALSE(result.success); REQUIRE_FALSE(result.error.empty());
        REQUIRE_FALSE(boost::filesystem::exists(f.output));
        REQUIRE_FALSE(boost::filesystem::exists(f.output.string() + ".partial"));
        REQUIRE(read(f.source) == model);
    }
    f.write(colored_grid(7, [](int,int) { return false; }));
    options.selected_faces.clear();
    REQUIRE_FALSE(finish_model_obj(f.source, f.output, options).success);
    REQUIRE_FALSE(boost::filesystem::exists(f.output));
    options = cleanup_options(7, 0, 6); options.cleanup_palette.clear();
    REQUIRE_FALSE(finish_model_obj(f.source, f.output, options).success);
    REQUIRE_FALSE(boost::filesystem::exists(f.output));
}

TEST_CASE("Color cleanup cancellation preserves the source and removes incomplete output", "[ModelFinishing][ColorCleanup]")
{
    Fixture f; f.write(colored_grid(31, [](int x,int y) { return x == 8 && y == 8; }));
    const auto before = read(f.source);
    const auto partial = boost::filesystem::path(f.output.string() + ".partial");
    const auto result = finish_model_obj(f.source, f.output, cleanup_options(31, 0, 30), [&] {
        return boost::filesystem::exists(partial);
    });
    REQUIRE(result.canceled); REQUIRE_FALSE(result.success);
    REQUIRE_FALSE(boost::filesystem::exists(f.output)); REQUIRE_FALSE(boost::filesystem::exists(partial));
    REQUIRE(read(f.source) == before);
}

TEST_CASE("Color cleanup preserves tiny colors on a geometric crease", "[ModelFinishing][ColorCleanup]")
{
    Fixture f;
    std::istringstream flat(colored_grid(21, [](int x, int y) { return x == 10 && y == 10; }));
    std::ostringstream folded; std::string line;
    while (std::getline(flat, line)) {
        std::istringstream row(line); std::string tag; row >> tag;
        if (tag == "v") {
            double x,y,z; row >> x >> y >> z;
            std::string tail; std::getline(row, tail);
            folded << "v " << x << ' ' << y << ' ' << std::abs(x-10) << tail << '\n';
        } else folded << line << '\n';
    }
    f.write(folded.str());
    const auto result = finish_model_obj(f.source, f.output, cleanup_options(21, 0, 20));
    INFO(result.error); REQUIRE(result.success); REQUIRE_FALSE(result.changed());
    REQUIRE(read(f.output) == folded.str());
}

TEST_CASE("Color cleanup preserves line endings and an absent final newline", "[ModelFinishing][ColorCleanup]")
{
    Fixture f; std::string source = colored_grid(21, [](int x, int y) { return x == 10 && y == 10; });
    source.pop_back();
    for (size_t pos = 0; (pos = source.find('\n', pos)) != std::string::npos; pos += 2) source.insert(pos, 1, '\r');
    { boost::filesystem::ofstream output(f.source, std::ios::binary); output << source; }
    const auto result = finish_model_obj(f.source, f.output, cleanup_options(21, 0, 20));
    INFO(result.error); REQUIRE(result.success); REQUIRE(result.recolored_vertices == 1);
    const std::string needle = "\tv  10.000\t10 0.0  0.02 0.02 0.02";
    auto expected = source;
    const auto where = expected.find(needle); REQUIRE(where != std::string::npos);
    expected.replace(where, needle.size(), "\tv  10.000\t10 0.0  0.12 0.32 0.16");
    boost::filesystem::ifstream output(f.output, std::ios::binary);
    const std::string actual {std::istreambuf_iterator<char>(output), std::istreambuf_iterator<char>()};
    REQUIRE(actual == expected);
}

TEST_CASE("Surface finishing pins a reversed patch while smoothing its surroundings", "[ModelFinishing]")
{
    Fixture f;
    auto source = noisy_grid(13);
    const auto face = source.find("f 85 86 99\n"); REQUIRE(face != std::string::npos);
    source.replace(face, 11, "f 85 99 86\n");
    f.write(source);
    const auto before = positions(source);
    const auto result = finish_model_obj(f.source, f.output, {true, false, 1});
    INFO(result.error); REQUIRE(result.success); REQUIRE(result.moved_vertices > 0);
    const auto after = positions(read(f.output)); REQUIRE(after.size() == before.size());
    for (size_t vertex : {84, 85, 98})
        REQUIRE_THAT((after[vertex] - before[vertex]).norm(), WithinAbs(0, 1e-12));
    REQUIRE(read(f.source) == source);
}

TEST_CASE("Finishing accepts signed scientific OBJ numbers without changing their spelling", "[ModelFinishing][ColorCleanup]")
{
    Fixture f;
    const std::string source = "v +0e0 .0 -0e0 +.1 2e-1 0.3 1\nv 1e1 0 0 .1 .2 .3 1\n"
        "v 0 1E1 0 .1 .2 .3 1\nvt 0 0\nvn 0 0 +1\nf +1/1/1 -2/1/1 -1/1/1 # unchanged\n";
    f.write(source);
    auto options = cleanup_options(2, 0, 1); options.selected_faces = {0};
    const auto result = finish_model_obj(f.source, f.output, options);
    INFO(result.error); REQUIRE(result.success); REQUIRE_FALSE(result.changed());
    REQUIRE(read(f.output) == source);
}

TEST_CASE("Finishing rejects malformed numeric tokens before writing a version", "[ModelFinishing][ColorCleanup]")
{
    for (const std::string token : {"+-1", "++1", "1oops", "1e999", "nan", "inf"}) {
        DYNAMIC_SECTION("invalid position " << token) {
            Fixture f;
            f.write("v " + token + " 0 0 .1 .2 .3\nv 10 0 0 .1 .2 .3\nv 0 10 0 .1 .2 .3\nf 1 2 3\n");
            const auto result = finish_model_obj(f.source, f.output, {true, false, .5});
            REQUIRE_FALSE(result.success); REQUIRE_FALSE(boost::filesystem::exists(f.output));
        }
        DYNAMIC_SECTION("invalid color " << token) {
            Fixture f;
            f.write("v 0 0 0 " + token + " .2 .3\nv 10 0 0 .1 .2 .3\nv 0 10 0 .1 .2 .3\nf 1 2 3\n");
            auto options = cleanup_options(2, 0, 1); options.selected_faces = {0};
            const auto result = finish_model_obj(f.source, f.output, options);
            REQUIRE_FALSE(result.success); REQUIRE_FALSE(boost::filesystem::exists(f.output));
        }
    }
}
