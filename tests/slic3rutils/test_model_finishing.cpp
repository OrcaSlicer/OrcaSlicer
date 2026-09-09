#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "libslic3r/Point.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <sstream>
#include <vector>

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
    // This deterministic geometry exercises the local fallback, not merely
    // smoothing a regular grid that cannot trigger it.
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
    f.write(mesh.str());
    const auto before = positions(mesh.str());
    const auto result = finish_model_obj(f.source, f.output, {true, false, 1});
    INFO(result.error); REQUIRE(result.success);
    REQUIRE(result.protected_vertices > 0);
    REQUIRE(result.moved_vertices > 0);
    const auto after = positions(read(f.output)); REQUIRE(after.size() == before.size());
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
