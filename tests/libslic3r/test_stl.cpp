#include <catch2/catch_all.hpp>
#include <algorithm>
#include <array>
#include <functional>
#include <vector>
#include "libslic3r/TexturePainting.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <cstddef>
#include <cstdint>
#include <ios>
#include <string>
#include "libslic3r/Point.hpp"

#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/STL.hpp"
#include "test_utils.hpp"
#include <boost/nowide/fstream.hpp>

using namespace Slic3r;

static inline std::string stl_path(const char* path)
{
	return std::string(TEST_DATA_DIR) + "/test_stl/" + path;
}

SCENARIO("Reading an STL file", "[stl]") {
	GIVEN("umlauts in the path of a binary STL file, Czech characters in the file name") {
        WHEN("STL file is read") {
			Slic3r::Model model;
			THEN("load should succeed") {
                REQUIRE(Slic3r::load_stl(stl_path("Geräte/20mmbox-čřšřěá.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
            }
        }
    }
	GIVEN("in ASCII format") {
		WHEN("line endings LF") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-LF.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}
		WHEN("line endings CRLF") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-CRLF.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}
#if 0
		// ASCII STLs ending with just carriage returns are not supported. These were used by the old Macs, while the Unix based MacOS uses LFs as any other Unix.
		WHEN("line endings CR") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-CR.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}

#endif
		WHEN("nonstandard STL file (text after ending tags, invalid normals, for example infinities)") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-nonstandard.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}
	}
}

TEST_CASE("A binary STL whose facet bytes never exceed 127 is read as binary", "[stl]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    std::string stl(80, '\0');
    const auto append = [&stl](const auto &value) { stl.append(reinterpret_cast<const char *>(&value), sizeof(value)); };
    append(uint32_t(cube.indices.size()));
    const stl_normal zero_normal = stl_normal::Zero();
    for (const stl_triangle_vertex_indices &facet : cube.indices) {
        append(zero_normal);
        for (int i = 0; i < 3; ++i)
            append(cube.vertices[facet[i]]);
        append(uint16_t(0));
    }
    REQUIRE(std::none_of(stl.begin() + 84, stl.begin() + 84 + 128, [](unsigned char c) { return c > 127; }));

    ScopedTemporaryFile file(".stl");
    boost::nowide::ofstream(file.string(), std::ios::binary) << stl;
    Model model;
    REQUIRE(load_stl(file.string().c_str(), &model));
    REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(10, 10, 10)));
}

// A 10 mm cube as binary STL whose facets take the attribute that `attribute` gives for their index.
static void write_cube_stl(const std::string& path, std::string header, const std::function<std::uint16_t(size_t)>& attribute)
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    header.resize(80, '\0');
    std::string stl = header;
    const auto append = [&stl](const auto &value) { stl.append(reinterpret_cast<const char *>(&value), sizeof(value)); };
    append(uint32_t(cube.indices.size()));
    const stl_normal zero_normal = stl_normal::Zero();
    for (size_t f = 0; f < cube.indices.size(); ++f) {
        append(zero_normal);
        for (int i = 0; i < 3; ++i)
            append(cube.vertices[cube.indices[f][i]]);
        append(attribute(f));
    }
    boost::nowide::ofstream(path, std::ios::binary) << stl;
}

static std::vector<std::array<std::size_t, 3>> stl_face_colors(const std::string& path, bool import_colors)
{
    Model model;
    REQUIRE(load_stl(path.c_str(), &model, nullptr, nullptr, 80, import_colors));
    if (!model.texture_mesh)
        return {};
    return model.texture_mesh->precomputed_face_colors;
}

TEST_CASE("A binary STL with VisCAM facet colors hands them to the texture-to-color importer", "[stl]")
{
    ScopedTemporaryFile file(".stl");
    // Bit 15 marks a valid colour, then 5 bits each of red, green and blue from the top.
    write_cube_stl(file.string(), "", [](size_t f) { return std::uint16_t(0x8000 | (f < 6 ? 31 << 10 : 31 << 5)); });

    using Color = std::array<std::size_t, 3>;
    const auto colors = stl_face_colors(file.string(), true);
    CHECK(std::count(colors.begin(), colors.end(), Color{255, 0, 0}) == 6);
    CHECK(std::count(colors.begin(), colors.end(), Color{0, 255, 0}) == 6);
    CHECK(stl_face_colors(file.string(), false).empty());
}

TEST_CASE("A binary STL with Magics facet colors falls back on the color of the part", "[stl]")
{
    ScopedTemporaryFile file(".stl");
    const std::string   header = std::string("COLOR=") + char(0) + char(0) + char(255) + char(255);
    // Bit 15 clear: the facet's own colour, red in the low bits. Set: the part colour from the header.
    write_cube_stl(file.string(), header, [](size_t f) { return std::uint16_t(f < 6 ? 31 : 0x8000); });

    using Color = std::array<std::size_t, 3>;
    const auto colors = stl_face_colors(file.string(), true);
    CHECK(std::count(colors.begin(), colors.end(), Color{255, 0, 0}) == 6);
    CHECK(std::count(colors.begin(), colors.end(), Color{0, 0, 255}) == 6);
}

TEST_CASE("A binary STL without facet colors has none to import", "[stl]")
{
    ScopedTemporaryFile file(".stl");
    write_cube_stl(file.string(), "", [](size_t) { return std::uint16_t(0); });
    CHECK(stl_face_colors(file.string(), true).empty());
}
