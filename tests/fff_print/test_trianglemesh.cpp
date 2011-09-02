#include <catch2/catch_all.hpp>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <future>
#include <chrono>

//#include "test_options.hpp"
#include "test_data.hpp"

using namespace Slic3r;
using namespace std;

static inline TriangleMesh make_cube() { return make_cube(20., 20, 20); }

SCENARIO( "TriangleMesh: Basic mesh statistics") {
    GIVEN( "A 20mm cube, built from constexpr std::array" ) {
        std::vector<Vec3f> vertices { {20,20,0}, {20,0,0}, {0,0,0}, {0,20,0}, {20,20,20}, {0,20,20}, {0,0,20}, {20,0,20} };
        std::vector<Vec3i32> facets { {0,1,2}, {0,2,3}, {4,5,6}, {4,6,7}, {0,4,7}, {0,7,1}, {1,7,6}, {1,6,2}, {2,6,5}, {2,5,3}, {4,0,3}, {4,3,5} };
        TriangleMesh cube(vertices, facets);
        
        THEN( "Volume is appropriate for 20mm square cube.") {
            REQUIRE(abs(cube.volume() - 20.0*20.0*20.0) < 1e-2);
        }

        THEN( "Vertices array matches input.") {
            for (size_t i = 0U; i < cube.its.vertices.size(); i++) {
                REQUIRE(cube.its.vertices.at(i) == vertices.at(i).cast<float>());
            }
            for (size_t i = 0U; i < vertices.size(); i++) {
                REQUIRE(vertices.at(i).cast<float>() == cube.its.vertices.at(i));
            }
        }
        THEN( "Vertex count matches vertex array size.") {
            REQUIRE(cube.facets_count() == facets.size());
        }

        THEN( "Facet array matches input.") {
            for (size_t i = 0U; i < cube.its.indices.size(); i++) {
                REQUIRE(cube.its.indices.at(i) == facets.at(i));
            }

            for (size_t i = 0U; i < facets.size(); i++) {
                REQUIRE(facets.at(i) == cube.its.indices.at(i));
            }
        }
        THEN( "Facet count matches facet array size.") {
            REQUIRE(cube.facets_count() == facets.size());
        }

#if 0
        THEN( "Number of normals is equal to the number of facets.") {
            REQUIRE(cube.normals().size() == facets.size());
        }
#endif

        THEN( "center() returns the center of the object.") {
            REQUIRE(cube.center() == Vec3d(10.0,10.0,10.0));
        }

        THEN( "Size of cube is (20,20,20)") {
            REQUIRE(cube.size() == Vec3d(20,20,20));
        }

    }
}

SCENARIO( "TriangleMesh: Transformation functions affect mesh as expected.") {
    GIVEN( "A 20mm cube with one corner on the origin") {
        auto cube = make_cube();

        WHEN( "The cube is scaled 200% uniformly") {
            cube.scale(2.0);
            THEN( "The volume is equivalent to 40x40x40 (all dimensions increased by 200%") {
                REQUIRE(abs(cube.volume() - 40.0*40.0*40.0) < 1e-2);
            }
        }
        WHEN( "The resulting cube is scaled 200% in the X direction") {
            cube.scale(Vec3f(2.0, 1, 1));
            THEN( "The volume is doubled.") {
                REQUIRE(abs(cube.volume() - 2*20.0*20.0*20.0) < 1e-2);
            }
            THEN( "The X coordinate size is 200%.") {
                REQUIRE(cube.its.vertices.at(0).x() == 40.0);
            }
        }

        WHEN( "The cube is scaled 25% in the X direction") {
            cube.scale(Vec3f(0.25, 1, 1));
            THEN( "The volume is 25% of the previous volume.") {
                REQUIRE(abs(cube.volume() - 0.25*20.0*20.0*20.0) < 1e-2);
            }
            THEN( "The X coordinate size is 25% from previous.") {
                REQUIRE(cube.its.vertices.at(0).x() == 5.0);
            }
        }

        WHEN( "The cube is rotated 45 degrees.") {
            cube.rotate_z(float(M_PI / 4.));
            THEN( "The X component of the size is sqrt(2)*20") {
                REQUIRE(abs(cube.size().x() - sqrt(2.0)*20) < 1e-2);
            }
        }

        WHEN( "The cube is translated (5, 10, 0) units with a Vec3f ") {
            cube.translate(Vec3f(5.0, 10.0, 0.0));
            THEN( "The first vertex is located at 25, 30, 0") {
                REQUIRE(cube.its.vertices.at(0) == Vec3f(25.0, 30.0, 0.0));
            }
        }

        WHEN( "The cube is translated (5, 10, 0) units with 3 doubles") {
            cube.translate(5.0, 10.0, 0.0);
            THEN( "The first vertex is located at 25, 30, 0") {
                REQUIRE(cube.its.vertices.at(0) == Vec3f(25.0, 30.0, 0.0));
            }
        }
        WHEN( "The cube is translated (5, 10, 0) units and then aligned to origin") {
            cube.translate(5.0, 10.0, 0.0);
            cube.align_to_origin();
            THEN( "The third vertex is located at 0,0,0") {
                REQUIRE(cube.its.vertices.at(2) == Vec3f::Zero());
            }
            THEN( "Size is OK") {
                REQUIRE(cube.stats().size == Vec3f(20.f, 20.f, 20.f));
            }
        }
    }
}

SCENARIO( "TriangleMesh: slice behavior.") {
    GIVEN( "A 20mm cube with one corner on the origin") {
        auto cube = make_cube();
        
        WHEN("Cube is sliced with z = [0+EPSILON,2,4,8,6,8,10,12,14,16,18,20]") {
            std::vector<double> z { 0+EPSILON,2,4,8,6,8,10,12,14,16,18,20 };
			std::vector<ExPolygons> result = cube.slice(z);
            THEN( "The correct number of polygons are returned per layer.") {
                for (size_t i = 0U; i < z.size(); i++) {
                    REQUIRE(result.at(i).size() == 1);
                }
            }
            THEN( "The area of the returned polygons is correct.") {
                for (size_t i = 0U; i < z.size(); i++) {
                    REQUIRE(result.at(i).at(0).area() == 20.0*20/(std::pow(SCALING_FACTOR,2)));
                }
            }
        }
    }
    GIVEN( "A STL with an irregular shape.") {
        const std::vector<Vec3f> vertices {{0,0,0},{0,0,20},{0,5,0},{0,5,20},{50,0,0},{50,0,20},{15,5,0},{35,5,0},{15,20,0},{50,5,0},{35,20,0},{15,5,10},{50,5,20},{35,5,10},{35,20,10},{15,20,10}};
        const std::vector<Vec3i32> facets {{0,1,2},{2,1,3},{1,0,4},{5,1,4},{0,2,4},{4,2,6},{7,6,8},{4,6,7},{9,4,7},{7,8,10},{2,3,6},{11,3,12},{7,12,9},{13,12,7},{6,3,11},{11,12,13},{3,1,5},{12,3,5},{5,4,9},{12,5,9},{13,7,10},{14,13,10},{8,15,10},{10,15,14},{6,11,8},{8,11,15},{15,11,13},{14,15,13}};

		auto cube = make_cube();
        WHEN(" a top tangent plane is sliced") {
            // At Z = 10 we have a top horizontal surface.
			std::vector<ExPolygons> slices = cube.slice({5.0, 10.0});
            THEN( "its area is included") {
                REQUIRE(slices.at(0).at(0).area() > 0);
                REQUIRE(slices.at(1).at(0).area() > 0);
            }
        }
        WHEN(" a model that has been transformed is sliced") {
            cube.mirror_z();
			std::vector<ExPolygons> slices = cube.slice({-5.0, -10.0});
            THEN( "it is sliced properly (mirrored bottom plane area is included)") {
                REQUIRE(slices.at(0).at(0).area() > 0);
                REQUIRE(slices.at(1).at(0).area() > 0);
            }
        }
    }
}

SCENARIO( "make_xxx functions produce meshes.") {
    GIVEN("make_cube() function") {
        WHEN("make_cube() is called with arguments 20,20,20") {
			TriangleMesh cube = make_cube(20,20,20);
            THEN("The resulting mesh has one and only one vertex at 0,0,0") {
                const std::vector<Vec3f> &verts = cube.its.vertices;
                REQUIRE(std::count_if(verts.begin(), verts.end(), [](const Vec3f& t) { return t.x() == 0 && t.y() == 0 && t.z() == 0; } ) == 1);
            }
            THEN("The mesh volume is 20*20*20") {
                REQUIRE(abs(cube.volume() - 20.0*20.0*20.0) < 1e-2);
            }
            THEN("There are 12 facets.") {
                REQUIRE(cube.its.indices.size() == 12);
            }
        }
    }
    GIVEN("make_cylinder() function") {
        WHEN("make_cylinder() is called with arguments 10,10, PI / 3") {
            TriangleMesh cyl = make_cylinder(10, 10, PI / 243.0);
            double angle = (2*PI / floor(2*PI / (PI / 243.0)));
            THEN("The resulting mesh has one and only one vertex at 0,0,0") {
                const std::vector<Vec3f> &verts = cyl.its.vertices;
                REQUIRE(std::count_if(verts.begin(), verts.end(), [](const Vec3f& t) { return t.x() == 0 && t.y() == 0 && t.z() == 0; } ) == 1);
            }
            THEN("The resulting mesh has one and only one vertex at 0,0,10") {
                const std::vector<Vec3f> &verts = cyl.its.vertices;
                REQUIRE(std::count_if(verts.begin(), verts.end(), [](const Vec3f& t) { return t.x() == 0 && t.y() == 0 && t.z() == 10; } ) == 1);
            }
            THEN("Resulting mesh has 2 + (2*PI/angle * 2) vertices.") { 
                REQUIRE(cyl.its.vertices.size() == (2 + ((2*PI/angle)*2)));
            }
            THEN("Resulting mesh has 2*PI/angle * 4 facets") {
                REQUIRE(cyl.its.indices.size() == (2*PI/angle)*4);
            }
            THEN( "The mesh volume is approximately 10pi * 10^2") {
                REQUIRE(abs(cyl.volume() - (10.0 * M_PI * std::pow(10,2))) < 1);
            }
        }
    }

    GIVEN("make_sphere() function") {
        WHEN("make_sphere() is called with arguments 10, PI / 3") {
            TriangleMesh sph = make_sphere(10, PI / 243.0);
            THEN("Resulting mesh has one point at 0,0,-10 and one at 0,0,10") {
				const std::vector<stl_vertex> &verts = sph.its.vertices;
                REQUIRE(std::count_if(verts.begin(), verts.end(), [](const Vec3f& t) { return is_approx(t, Vec3f(0.f, 0.f, 10.f)); } ) == 1);
				REQUIRE(std::count_if(verts.begin(), verts.end(), [](const Vec3f& t) { return is_approx(t, Vec3f(0.f, 0.f, -10.f)); } ) == 1);
            }
            THEN( "The mesh volume is approximately 4/3 * pi * 10^3") {
                REQUIRE(abs(sph.volume() - (4.0/3.0 * M_PI * std::pow(10,3))) < 1); // 1% tolerance?
            }
        }
    }
}

SCENARIO( "TriangleMesh: split functionality.") {
    GIVEN( "A 20mm cube with one corner on the origin") {
		auto cube = make_cube();
        WHEN( "The mesh is split into its component parts.") {
            std::vector<TriangleMesh> meshes = cube.split();
            THEN(" The bounding box statistics are propagated to the split copies") {
                REQUIRE(meshes.size() == 1);
                REQUIRE((meshes.front().bounding_box() == cube.bounding_box()));
            }
        }
    }
    GIVEN( "Two 20mm cubes, each with one corner on the origin, merged into a single TriangleMesh") {
		auto cube = make_cube();
		TriangleMesh cube2(cube);

        cube.merge(cube2);
        WHEN( "The combined mesh is split") {
            THEN( "Number of faces is 2x the source.") {
                REQUIRE(cube.facets_count() == 2 * cube2.facets_count());
            }
            std::vector<TriangleMesh> meshes = cube.split();
            THEN( "Two meshes are in the output vector.") {
                REQUIRE(meshes.size() == 2);
            }
        }
    }
}

SCENARIO( "TriangleMesh: Mesh merge functions") {
    GIVEN( "Two 20mm cubes, each with one corner on the origin") {
		auto cube = make_cube();
		TriangleMesh cube2(cube);

        WHEN( "The two meshes are merged") {
            cube.merge(cube2);
            THEN( "There are twice as many facets in the merged mesh as the original.") {
                REQUIRE(cube.facets_count() == 2 * cube2.facets_count());
            }
        }
    }
}

SCENARIO( "TriangleMeshSlicer: Cut behavior.") {
    GIVEN( "A 20mm cube with one corner on the origin") {
		auto cube = make_cube();
        WHEN( "Object is cut at the bottom") {
            indexed_triangle_set upper {};
            indexed_triangle_set lower {};
            cut_mesh(cube.its, 0, &upper, &lower);
            THEN("Upper mesh has all facets except those belonging to the slicing plane.") {
                REQUIRE(upper.indices.size() == 12);
            }
            THEN("Lower mesh has no facets.") {
                REQUIRE(lower.indices.size() == 0);
            }
        }
        WHEN( "Object is cut at the center") {
            indexed_triangle_set upper {};
            indexed_triangle_set lower {};
            cut_mesh(cube.its, 10, &upper, &lower);
            THEN("Upper mesh has 2 external horizontal facets, 3 facets on each side, and 6 facets on the triangulated side (2 + 12 + 6).") {
                REQUIRE(upper.indices.size() == 2+12+6);
            }
            THEN("Lower mesh has 2 external horizontal facets, 3 facets on each side, and 6 facets on the triangulated side (2 + 12 + 6).") {
                REQUIRE(lower.indices.size() == 2+12+6);
            }
        }
    }
}
#ifdef TEST_PERFORMANCE
TEST_CASE("Regression test for issue #4486 - files take forever to slice") {
    TriangleMesh mesh;
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    mesh.ReadSTLFile(std::string(testfile_dir) + "test_trianglemesh/4486/100_000.stl");

    config.set("layer_height", 500);
    config.set("first_layer_height", 250);
    config.set("nozzle_diameter", 500);

    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({mesh}, print, model, config);

    print.status_cb = [] (int ln, const std::string& msg) { Slic3r::Log::info("Print") << ln << " " << msg << "\n";};

    std::future<void> fut = std::async([&print] () { print.process(); });
    std::chrono::milliseconds span {120000};
    bool timedout {false};
    if(fut.wait_for(span) == std::future_status::timeout) {
        timedout = true;
    }
    REQUIRE(timedout == false);

}
#endif // TEST_PERFORMANCE

#ifdef BUILD_PROFILE
TEST_CASE("Profile test for issue #4486 - files take forever to slice") {
    TriangleMesh mesh;
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    mesh.ReadSTLFile(std::string(testfile_dir) + "test_trianglemesh/4486/10_000.stl");

    config.set("layer_height", 500);
    config.set("first_layer_height", 250);
    config.set("nozzle_diameter", 500);
    config.set("fill_density", "5%");

    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({mesh}, print, model, config);

    print.status_cb = [] (int ln, const std::string& msg) { Slic3r::Log::info("Print") << ln << " " << msg << "\n";};

    print.process();

    REQUIRE(true);

}
#endif //BUILD_PROFILE
