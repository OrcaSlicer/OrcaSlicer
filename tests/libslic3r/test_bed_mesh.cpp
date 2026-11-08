#include <catch2/catch_all.hpp>

#include "libslic3r/BedMesh.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// A KAMP adaptive mesh as Moonraker reports it: 5 x 4 probe points over the print area only, on
// a 173 x 190 mm bed. mesh_matrix trimmed to the probed grid for brevity.
const char* MICRON_ADAPTIVE_REPLY = R"({"result":{"eventtime":533595.96,"status":{"bed_mesh":{
    "profile_name":"default",
    "mesh_min":[28.28,53.43],"mesh_max":[141.72,126.54],
    "probed_matrix":[[0.019767,0.011017,0.012892,0.021642,0.018517],
                     [0.037267,0.004142,0.020392,0.027892,0.027892],
                     [0.004142,-0.001483,0.010392,0.024767,0.024142],
                     [-0.013358,-0.012733,0.009142,0.026642,0.022892]],
    "mesh_matrix":[[0.019767,0.011017,0.012892,0.021642,0.018517],
                   [0.037267,0.004142,0.020392,0.027892,0.027892],
                   [0.004142,-0.001483,0.010392,0.024767,0.024142],
                   [-0.013358,-0.012733,0.009142,0.026642,0.022892]],
    "profiles":{"default":{"points":[[0.0]],"mesh_params":{"x_count":5,"y_count":4}}}}}}})";

const BoundingBoxf MICRON_BED(Vec2d(0.0, 0.0), Vec2d(173.0, 190.0));

} // namespace

TEST_CASE("Moonraker bed_mesh reply parses into a grid in printer coordinates", "[BedMesh]")
{
    BedMesh     mesh;
    std::string error;
    REQUIRE(BedMesh::from_moonraker_json(MICRON_ADAPTIVE_REPLY, mesh, error));
    CHECK(error.empty());
    CHECK_FALSE(mesh.empty());
    CHECK(mesh.profile_name == "default");
    CHECK(mesh.probed_rows() == 4);
    CHECK(mesh.probed_cols() == 5);
    CHECK_THAT(mesh.min.x(), WithinAbs(28.28, 1e-6));
    CHECK_THAT(mesh.max.y(), WithinAbs(126.54, 1e-6));
    CHECK_THAT(mesh.z_min(), WithinAbs(-0.013358, 1e-6));
    CHECK_THAT(mesh.z_max(), WithinAbs(0.037267, 1e-6));

    // Row 0 sits at mesh_min.y, the last column at mesh_max.x.
    const Vec2d first = mesh.node_position(0, 0, 4, 5);
    const Vec2d last  = mesh.node_position(3, 4, 4, 5);
    CHECK_THAT(first.x(), WithinAbs(28.28, 1e-6));
    CHECK_THAT(first.y(), WithinAbs(53.43, 1e-6));
    CHECK_THAT(last.x(), WithinAbs(141.72, 1e-6));
    CHECK_THAT(last.y(), WithinAbs(126.54, 1e-6));
}

TEST_CASE("The bare bed_mesh object parses the same as the full envelope", "[BedMesh]")
{
    BedMesh     mesh;
    std::string error;
    const char* bare = R"({"profile_name":"","mesh_min":[10,10],"mesh_max":[20,30],"probed_matrix":[[0.1,0.2],[0.3,0.4],[0.5,0.6]]})";
    REQUIRE(BedMesh::from_moonraker_json(bare, mesh, error));
    CHECK(mesh.probed_rows() == 3);
    CHECK(mesh.probed_cols() == 2);
    // No interpolated matrix: the probed one is what gets drawn.
    CHECK(mesh.mesh.empty());
    CHECK(&mesh.surface() == &mesh.probed);
}

TEST_CASE("An adaptive mesh is reported as partial, a full-bed mesh is not", "[BedMesh]")
{
    BedMesh     adaptive;
    std::string error;
    REQUIRE(BedMesh::from_moonraker_json(MICRON_ADAPTIVE_REPLY, adaptive, error));
    CHECK(adaptive.is_partial(MICRON_BED));
    const Vec2d cov = adaptive.coverage(MICRON_BED);
    CHECK_THAT(cov.x(), WithinAbs((141.72 - 28.28) / 173.0, 1e-6));
    CHECK_THAT(cov.y(), WithinAbs((126.54 - 53.43) / 190.0, 1e-6));

    // A full mesh loses only the probe offset at the edges (here 20 mm on a 180 mm bed).
    BedMesh full;
    full.min    = { 20.0, 20.0 };
    full.max    = { 160.0, 160.0 };
    full.probed = { { 0.f, 0.f }, { 0.f, 0.f } };
    CHECK_FALSE(full.is_partial(BoundingBoxf(Vec2d(0.0, 0.0), Vec2d(180.0, 180.0))));
}

TEST_CASE("A printer with no mesh loaded yields an empty mesh, not an error", "[BedMesh]")
{
    BedMesh     mesh;
    std::string error;
    const char* none = R"({"result":{"status":{"bed_mesh":{"profile_name":"","mesh_min":[0,0],"mesh_max":[0,0],"probed_matrix":[[]],"mesh_matrix":[[]],"profiles":{}}}}})";
    REQUIRE(BedMesh::from_moonraker_json(none, mesh, error));
    CHECK(mesh.empty());
    CHECK_THAT(mesh.z_min(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("With no active mesh a saved profile is used and flagged as not loaded", "[BedMesh]")
{
    // What Klipper reports right after SAVE_CONFIG restarted it: the active mesh is gone, the
    // profile survives in the config.
    const char* after_restart = R"({"result":{"status":{"bed_mesh":{
        "profile_name":"","mesh_min":[0,0],"mesh_max":[0,0],"probed_matrix":[[]],"mesh_matrix":[[]],
        "profiles":{"default":{"points":[[0.01,0.02,0.03],[0.04,0.05,0.06]],
                               "mesh_params":{"min_x":20.0,"max_x":160.0,"min_y":25.0,"max_y":165.0,"x_count":3,"y_count":2}}}}}}})";
    BedMesh     mesh;
    std::string error;
    REQUIRE(BedMesh::from_moonraker_json(after_restart, mesh, error));
    CHECK_FALSE(mesh.empty());
    CHECK_FALSE(mesh.loaded);
    CHECK(mesh.profile_name == "default");
    CHECK(mesh.probed_rows() == 2);
    CHECK(mesh.probed_cols() == 3);
    CHECK_THAT(mesh.min.x(), WithinAbs(20.0, 1e-9));
    CHECK_THAT(mesh.max.y(), WithinAbs(165.0, 1e-9));
    CHECK(mesh.mesh.empty());
}

TEST_CASE("An active mesh is preferred over saved profiles and is flagged as loaded", "[BedMesh]")
{
    BedMesh     mesh;
    std::string error;
    REQUIRE(BedMesh::from_moonraker_json(MICRON_ADAPTIVE_REPLY, mesh, error));
    CHECK(mesh.loaded);
    CHECK(mesh.probed_cols() == 5);
}

TEST_CASE("A RepRapFirmware v2 height map parses with rows from min Y upwards", "[BedMesh]")
{
    // The file an Unlayered (Duet) printer writes after G29: bed centred on the origin.
    const char* csv =
        "RepRapFirmware height map file v2, min error -0.531, max error 0.029, mean -0.198, deviation 0.148\n"
        "axis0,axis1,min0,max0,min1,max1,radius,spacing0,spacing1,num0,num1\n"
        "X,Y,-60.00,60.00,-60.00,60.00,-1.00,24.00,24.00,6,6\n"
        " -0.168, -0.127, -0.057,  0.014,  0.029, -0.016\n"
        " -0.275, -0.150, -0.049, -0.009,  0.006, -0.078\n"
        " -0.328, -0.182, -0.082, -0.002, -0.046, -0.111\n"
        " -0.360, -0.275, -0.165, -0.124, -0.139, -0.203\n"
        " -0.428, -0.358, -0.277, -0.257, -0.276, -0.251\n"
        " -0.531, -0.465, -0.375, -0.344, -0.344, -0.318\n";
    BedMesh     mesh;
    std::string error;
    REQUIRE(BedMesh::from_rrf_heightmap_csv(csv, mesh, error));
    CHECK(mesh.probed_rows() == 6);
    CHECK(mesh.probed_cols() == 6);
    CHECK_THAT(mesh.min.x(), WithinAbs(-60.0, 1e-9));
    CHECK_THAT(mesh.max.y(), WithinAbs(60.0, 1e-9));
    CHECK_THAT(mesh.probed[0][0], WithinAbs(-0.168, 1e-6));
    CHECK_THAT(mesh.probed[5][0], WithinAbs(-0.531, 1e-6));
    CHECK_THAT(mesh.z_range(), WithinAbs(0.029 + 0.531, 1e-5));
    CHECK(mesh.loaded);
}

TEST_CASE("A RepRapFirmware v1 height map header is understood too", "[BedMesh]")
{
    const char* csv =
        "RepRapFirmware height map file v1\n"
        "xmin,xmax,ymin,ymax,radius,xspacing,yspacing,xnum,ynum\n"
        "10.00,190.00,10.00,190.00,-1.00,90.00,90.00,3,3\n"
        "0.1,0.2,0.3\n0.4,0.5,0.6\n0.7,0.8,0.9\n";
    BedMesh     mesh;
    std::string error;
    REQUIRE(BedMesh::from_rrf_heightmap_csv(csv, mesh, error));
    CHECK(mesh.probed_rows() == 3);
    CHECK_THAT(mesh.min.x(), WithinAbs(10.0, 1e-9));
    CHECK_THAT(mesh.max.x(), WithinAbs(190.0, 1e-9));
    CHECK_THAT(mesh.probed[2][2], WithinAbs(0.9, 1e-6));
}

TEST_CASE("A truncated or foreign height map file is rejected", "[BedMesh]")
{
    BedMesh     mesh;
    std::string error;
    CHECK_FALSE(BedMesh::from_rrf_heightmap_csv("hello\nworld\n", mesh, error));
    CHECK_FALSE(error.empty());
    const char* short_file =
        "RepRapFirmware height map file v2\n"
        "axis0,axis1,min0,max0,min1,max1,radius,spacing0,spacing1,num0,num1\n"
        "X,Y,0,100,0,100,-1,50,50,3,3\n"
        "0.1,0.2,0.3\n";
    CHECK_FALSE(BedMesh::from_rrf_heightmap_csv(short_file, mesh, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Marlin's bilinear M420 V grid parses and is stretched over the assumed bed", "[BedMesh]")
{
    // As OctoPrint's terminal shows it, "Recv: " prefixes already stripped; row 0 is the front.
    const std::vector<std::string> lines = {
        "echo:Bed Leveling ON",
        "echo:Fade Height 10.00",
        "Bilinear Leveling Grid:",
        "      0      1      2",
        " 0 -0.113 -0.076 -0.017",
        " 1 +0.015 +0.060 +0.100",
        " 2 -0.020 +0.011 +0.049",
        "Subdivided with CATMULL ROM Leveling Grid:",
        "      0      1      2      3      4      5      6",
        " 0 -0.113 -0.100 -0.090 -0.076 -0.050 -0.030 -0.017",
        " 1 -0.070 -0.060 -0.050 -0.040 -0.020 -0.010  0.000",
        " 2 -0.030 -0.020 -0.010  0.000  0.010  0.020  0.030",
        " 3 +0.015 +0.030 +0.045 +0.060 +0.070 +0.090 +0.100",
        " 4 +0.000 +0.010 +0.020 +0.030 +0.040 +0.050 +0.060",
        " 5 -0.010 +0.000 +0.010 +0.020 +0.030 +0.040 +0.050",
        " 6 -0.020 -0.010  0.000 +0.011 +0.020 +0.035 +0.049",
        "echo:  M420 S1 Z10.00",
        "ok",
    };
    BedMesh     mesh;
    std::string error;
    REQUIRE(BedMesh::from_marlin_report(lines, BoundingBoxf(Vec2d(0.0, 0.0), Vec2d(220.0, 220.0)), mesh, error));
    CHECK(mesh.probed_rows() == 3);
    CHECK(mesh.probed_cols() == 3);
    CHECK_THAT(mesh.probed[0][0], WithinAbs(-0.113, 1e-6));
    CHECK_THAT(mesh.probed[1][2], WithinAbs(0.100, 1e-6));
    CHECK(mesh.mesh.size() == 7);
    CHECK(mesh.mesh.front().size() == 7);
    CHECK_THAT(mesh.min.x(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(mesh.max.y(), WithinAbs(220.0, 1e-9));
    CHECK_FALSE(mesh.note.empty());
}

TEST_CASE("A UBL topography report supplies its own corner positions and is flipped to min Y first", "[BedMesh]")
{
    const std::vector<std::string> lines = {
        "Bed Topography Report:",
        "",
        "    (  0,200)                      (200,200)",
        "        0       1       2",
        " 2  |  +0.300  +0.310  +0.320",
        " 1  |  +0.200  +0.210  +0.220",
        " 0  |  +0.100  +0.110  +0.120",
        "    (  0,  0)                      (200,  0)",
        "ok",
    };
    BedMesh     mesh;
    std::string error;
    REQUIRE(BedMesh::from_marlin_report(lines, BoundingBoxf(), mesh, error));
    CHECK(mesh.probed_rows() == 3);
    // Row 0 is min Y: the +0.1 row printed last.
    CHECK_THAT(mesh.probed[0][0], WithinAbs(0.100, 1e-6));
    CHECK_THAT(mesh.probed[2][2], WithinAbs(0.320, 1e-6));
    CHECK_THAT(mesh.min.x(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(mesh.max.x(), WithinAbs(200.0, 1e-9));
    CHECK_THAT(mesh.max.y(), WithinAbs(200.0, 1e-9));
}

TEST_CASE("Marlin's mesh bed leveling report survives the lines between its header and the grid", "[BedMesh]")
{
    const std::vector<std::string> lines = {
        "Mesh Bed Level data:",
        "3x3 mesh. Z offset: 0.00000",
        "Measured points:",
        "        0        1        2",
        " 0 +0.10000 +0.20000 +0.30000",
        " 1 +0.40000 +0.50000 +0.60000",
        " 2 +0.70000 +0.80000 +0.90000",
        "ok",
    };
    BedMesh     mesh;
    std::string error;
    REQUIRE(BedMesh::from_marlin_report(lines, BoundingBoxf(Vec2d(0.0, 0.0), Vec2d(200.0, 200.0)), mesh, error));
    CHECK(mesh.probed_rows() == 3);
    CHECK_THAT(mesh.probed[2][2], WithinAbs(0.9, 1e-6));
}

TEST_CASE("UBL's bracketed nozzle marker and unprobed cells do not break a row", "[BedMesh]")
{
    const std::vector<std::string> lines = {
        "Bed Topography Report:",
        "    (  0,100)          (100,100)",
        "        0       1       2",
        " 1  |  +0.300 [+0.310]  +0.320",
        " 0  |  +0.100  +0.110     .",
        "    (  0,  0)          (100,  0)",
    };
    BedMesh     mesh;
    std::string error;
    REQUIRE(BedMesh::from_marlin_report(lines, BoundingBoxf(), mesh, error));
    REQUIRE(mesh.probed_rows() == 2);
    CHECK_THAT(mesh.probed[1][1], WithinAbs(0.31, 1e-6));
    CHECK_FALSE(BedMesh::is_probed(mesh.probed[0][2]));
    CHECK(mesh.probed_count() == 5);
    // The unprobed cell is left out of the statistics.
    CHECK_THAT(mesh.z_min(), WithinAbs(0.1, 1e-6));
    CHECK_THAT(mesh.z_max(), WithinAbs(0.32, 1e-6));
}

TEST_CASE("RRF nodes follow the spacing and a delta's corners outside the radius are unprobed", "[BedMesh]")
{
    // M557 X10:220 Y10:220 S50: five points at 10,60,...,210 although the header says 220.
    const char* rect =
        "RepRapFirmware height map file v2\n"
        "axis0,axis1,min0,max0,min1,max1,radius,spacing0,spacing1,num0,num1\n"
        "X,Y,10.00,220.00,10.00,220.00,-1.00,50.00,50.00,5,5\n"
        "0,0,0,0,0\n0,0,0,0,0\n0,0,0.5,0,0\n0,0,0,0,0\n0,0,0,0,0\n";
    BedMesh     mesh;
    std::string error;
    REQUIRE(BedMesh::from_rrf_heightmap_csv(rect, mesh, error));
    CHECK_THAT(mesh.max.x(), WithinAbs(210.0, 1e-9));
    CHECK_THAT(mesh.node_position(0, 1, 5, 5).x(), WithinAbs(60.0, 1e-9));

    // A delta: 3x3 over -100..100 with radius 100 leaves the four corners unprobed.
    const char* delta =
        "RepRapFirmware height map file v2\n"
        "axis0,axis1,min0,max0,min1,max1,radius,spacing0,spacing1,num0,num1\n"
        "X,Y,-100.00,100.00,-100.00,100.00,100.00,100.00,100.00,3,3\n"
        "0,-0.3,0\n-0.3,-0.3,-0.3\n0,-0.3,0\n";
    REQUIRE(BedMesh::from_rrf_heightmap_csv(delta, mesh, error));
    CHECK(mesh.probed_count() == 5);
    CHECK_FALSE(BedMesh::is_probed(mesh.probed[0][0]));
    CHECK(BedMesh::is_probed(mesh.probed[1][1]));
    CHECK_THAT(mesh.z_range(), WithinAbs(0.0, 1e-6));
    CHECK_FALSE(mesh.out_of_flat());
}

TEST_CASE("Terminal output without a leveling grid is reported as such", "[BedMesh]")
{
    BedMesh     mesh;
    std::string error;
    CHECK_FALSE(BedMesh::from_marlin_report({ "echo:Bed Leveling OFF", "ok" }, BoundingBoxf(Vec2d(0.0, 0.0), Vec2d(1.0, 1.0)), mesh, error));
    CHECK_FALSE(error.empty());
    // The 3x3 planar correction matrix is not a height map.
    CHECK_FALSE(BedMesh::from_marlin_report({ "Bed Level Correction Matrix:", "1.000 0.000 0.000", "0.000 1.000 0.000", "0.000 0.000 1.000" },
                                            BoundingBoxf(Vec2d(0.0, 0.0), Vec2d(1.0, 1.0)), mesh, error));
}

TEST_CASE("The Bed Level Visualizer socket message parses with the plugin's bed bounds", "[BedMesh]")
{
    const char* msg = R"({"plugin":"bedlevelvisualizer","data":{"mesh":[["0.1","0.2"],["0.3","0.4"]],
        "bed":{"type":"rectangular","x_min":5,"x_max":215,"y_min":10,"y_max":210,"z_min":-2,"z_max":2}}})";
    BedMesh     mesh;
    std::string error;
    REQUIRE(BedMesh::from_bedlevelvisualizer_message(msg, mesh, error));
    CHECK(mesh.loaded);
    CHECK(mesh.probed_rows() == 2);
    CHECK_THAT(mesh.probed[1][1], WithinAbs(0.4, 1e-6));
    CHECK_THAT(mesh.min.x(), WithinAbs(5.0, 1e-9));
    CHECK_THAT(mesh.max.y(), WithinAbs(210.0, 1e-9));
}

TEST_CASE("The Bed Level Visualizer stored copy parses from OctoPrint settings and is flagged as not loaded", "[BedMesh]")
{
    // stored_mesh_y descending: the plugin stored the rows top-down, so they get flipped.
    const char* settings = R"({"api":{"key":"x"},"plugins":{"bedlevelvisualizer":{
        "stored_mesh":[[0.3,0.31],[0.1,0.11]],"stored_mesh_x":[10,200],"stored_mesh_y":[200,10],
        "stored_mesh_z_height":2,"mesh_timestamp":"9/20/2026, 10:15:00 AM"}}})";
    BedMesh     mesh;
    std::string error;
    REQUIRE(BedMesh::from_bedlevelvisualizer_json(settings, mesh, error));
    CHECK_FALSE(mesh.loaded);
    CHECK_THAT(mesh.probed[0][0], WithinAbs(0.1, 1e-6));
    CHECK_THAT(mesh.min.y(), WithinAbs(10.0, 1e-9));
    CHECK_THAT(mesh.max.y(), WithinAbs(200.0, 1e-9));
    CHECK(mesh.note.find("9/20/2026") != std::string::npos);

    BedMesh none;
    CHECK_FALSE(BedMesh::from_bedlevelvisualizer_json(R"({"plugins":{"tracking":{}}})", none, error));
    CHECK(error.find("not installed") != std::string::npos);
    REQUIRE(BedMesh::from_bedlevelvisualizer_json(R"({"plugins":{"bedlevelvisualizer":{"stored_mesh":[]}}})", none, error));
    CHECK(none.empty());
}

TEST_CASE("A bed more than the threshold out of flat is flagged, a flatter one is not", "[BedMesh]")
{
    BedMesh flat;
    flat.min    = { 0.0, 0.0 };
    flat.max    = { 100.0, 100.0 };
    flat.probed = { { -0.05f, 0.02f }, { 0.04f, 0.10f } };
    CHECK_THAT(flat.z_range(), WithinAbs(0.15, 1e-6));
    CHECK_FALSE(flat.out_of_flat());

    BedMesh warped = flat;
    warped.probed[1][1] = 0.20f;
    CHECK_THAT(warped.z_range(), WithinAbs(0.25, 1e-6));
    CHECK(warped.out_of_flat());

    BedMesh none;
    CHECK_FALSE(none.out_of_flat());
}

TEST_CASE("Malformed replies are rejected with a reason", "[BedMesh]")
{
    BedMesh     mesh;
    std::string error;

    SECTION("API error envelope")
    {
        CHECK_FALSE(BedMesh::from_moonraker_json(R"({"error":{"code":404,"message":"Object not found"}})", mesh, error));
        CHECK(error == "Object not found");
    }
    SECTION("ragged rows")
    {
        CHECK_FALSE(BedMesh::from_moonraker_json(R"({"mesh_min":[0,0],"mesh_max":[1,1],"probed_matrix":[[0.1,0.2],[0.3]]})", mesh, error));
        CHECK_FALSE(error.empty());
    }
    SECTION("degenerate rectangle")
    {
        CHECK_FALSE(BedMesh::from_moonraker_json(R"({"mesh_min":[5,5],"mesh_max":[5,9],"probed_matrix":[[0.1,0.2]]})", mesh, error));
        CHECK_FALSE(error.empty());
    }
    SECTION("not JSON")
    {
        CHECK_FALSE(BedMesh::from_moonraker_json("<html>", mesh, error));
        CHECK_FALSE(error.empty());
    }
    SECTION("no bed_mesh object at all")
    {
        CHECK_FALSE(BedMesh::from_moonraker_json(R"({"result":{"status":{}}})", mesh, error));
        CHECK_FALSE(error.empty());
    }
}
