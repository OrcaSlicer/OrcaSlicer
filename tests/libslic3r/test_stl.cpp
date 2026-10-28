#include <catch2/catch_all.hpp>
#include <algorithm>
#include "libslic3r/TriangleMesh.hpp"
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
