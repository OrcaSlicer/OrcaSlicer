#ifdef WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
	#include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include "test_data.hpp"

#include <boost/filesystem.hpp>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

// Always-on test: verifies the "Relative to Part" option is registered correctly, round-trips
// through (de)serialization, has the expected defaults, and is carried by the Print preset.
// This exercises the whole config surface of the feature without slicing (see the [.]-hidden
// test below for the actual seam-placement behavior).
TEST_CASE("Relative-to-Part seam option is registered and round-trips", "[Seams]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    // Defaults: the mode is not "custom", and the coordinates default to the part center (0,0).
    REQUIRE(config.opt_enum<SeamPosition>("seam_position") != spCustom);
    REQUIRE(config.opt_float("seam_position_x") == 0.0);
    REQUIRE(config.opt_float("seam_position_y") == 0.0);

    // "custom" deserializes to spCustom and serializes back (this also proves the enum value is
    // registered in the name table — otherwise serialize() would read out of bounds).
    config.set_deserialize_strict({
        { "seam_position",   "custom" },
        { "seam_position_x", "40"     },
        { "seam_position_y", "-15"    },
    });
    REQUIRE(config.opt_enum<SeamPosition>("seam_position") == spCustom);
    REQUIRE(config.opt_serialize("seam_position") == "custom");
    REQUIRE(config.opt_float("seam_position_x") == 40.0);
    REQUIRE(config.opt_float("seam_position_y") == -15.0);

    // The closest/farthest reference defaults to "closest" and round-trips to "farthest".
    REQUIRE(config.opt_enum<SeamRelativeReference>("seam_position_ref") == srrClosest);
    config.set_deserialize_strict({ { "seam_position_ref", "farthest" } });
    REQUIRE(config.opt_enum<SeamRelativeReference>("seam_position_ref") == srrFarthest);
    REQUIRE(config.opt_serialize("seam_position_ref") == "farthest");

    // The new keys must be part of the Print preset, otherwise the Process tab cannot bind them
    // ("No <key> in ConfigOptionsGroup config." at runtime).
    const std::vector<std::string> &opts = Preset::print_options();
    REQUIRE(std::find(opts.begin(), opts.end(), "seam_position")     != opts.end());
    REQUIRE(std::find(opts.begin(), opts.end(), "seam_position_x")   != opts.end());
    REQUIRE(std::find(opts.begin(), opts.end(), "seam_position_y")   != opts.end());
    REQUIRE(std::find(opts.begin(), opts.end(), "seam_position_ref") != opts.end());
}

// ---------------------------------------------------------------------------------------------
// Seam-placement behavior test. Marked [.] (hidden) like every other fff_print slicing test:
// G-code export currently SIGSEGVs in this test harness inside append_full_config() while
// serializing a coEnums option with a null keys_map — a pre-existing issue unrelated to this
// feature (all existing [.] slicing tests fail the same way). Run explicitly with:
//     ./fff_print_tests "[Seams][.]"
// in an environment where the export harness works.
//
// Slices a 20 mm-tall, 20 mm-diameter cylinder with seam_position = "custom" aimed at
// (target_x, target_y) and returns the mean XY of the outer-wall seam (start point of each
// outer perimeter loop) across layers. A cylinder is used because it is the rotationally-
// symmetric case this feature targets, its smooth wall gives a single unambiguous nearest
// point (unlike a cube's corner ties), and its vertical wall is free of overhang interference.
// init_print() is bypassed because its arrange_objects() also fails in this harness; the object
// is placed manually. Seam placement is computed in object coordinates, so this is equivalent.
static Vec2d outer_wall_seam_mean(const std::string &target_x, const std::string &target_y, size_t &n_seams,
                                  const std::string &reference = "closest")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "seam_position",      "custom"   },
        { "seam_position_x",    target_x   },
        { "seam_position_y",    target_y   },
        { "seam_position_ref",  reference  },
        { "wall_loops",         "2"        },
        { "layer_height",       "0.3"      },
        { "first_layer_height", "0.3"      },
        { "gcode_comments",     "1"        },
    });

    Model        model;
    ModelObject *object = model.add_object();
    object->name = "cylinder";
    object->add_volume(Slic3r::make_cylinder(10.0, 20.0)); // r=10mm, h=20mm, vertical walls
    ModelInstance *instance = object->add_instance();
    instance->set_offset(Vec3d(100.0, 100.0, 0.0));
    object->ensure_on_bed();

    Print print;
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    namespace fs = boost::filesystem;
    const fs::path out = fs::temp_directory_path() / fs::unique_path("orca_seam_%%%%%%%%.gcode");
    print.export_gcode(out.string(), nullptr, nullptr);
    std::ifstream in(out.string());
    const std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    fs::remove(out);

    std::vector<Vec2d> seams;
    bool   seam_pending = false;
    double prev_x = 0.0, prev_y = 0.0;

    GCodeReader reader;
    reader.parse_buffer(gcode, [&seams, &seam_pending, &prev_x, &prev_y]
                        (Slic3r::GCodeReader &self, const Slic3r::GCodeReader::GCodeLine &line) {
        if (line.raw().find("Outer wall") != std::string::npos) {
            seam_pending = true;
        } else if (seam_pending && line.extruding(self) && line.dist_XY(self) > 0) {
            seams.emplace_back(prev_x, prev_y);
            seam_pending = false;
        }
        prev_x = self.x();
        prev_y = self.y();
    });

    n_seams = seams.size();
    if (seams.empty())
        return Vec2d(0.0, 0.0);
    Vec2d sum(0.0, 0.0);
    for (const Vec2d &p : seams)
        sum += p;
    return sum / double(seams.size());
}

TEST_CASE("Relative-to-Part seam follows the configured X/Y point", "[Seams][.]")
{
    size_t n_xp = 0, n_xn = 0, n_yp = 0, n_yn = 0;
    const Vec2d seam_x_plus  = outer_wall_seam_mean( "40",  "0", n_xp);
    const Vec2d seam_x_minus = outer_wall_seam_mean("-40",  "0", n_xn);
    const Vec2d seam_y_plus  = outer_wall_seam_mean(  "0", "40", n_yp);
    const Vec2d seam_y_minus = outer_wall_seam_mean(  "0","-40", n_yn);

    REQUIRE(n_xp > 5);
    REQUIRE(n_xn > 5);
    REQUIRE(n_yp > 5);
    REQUIRE(n_yn > 5);

    // Aiming at +X vs -X moves the seam to opposite sides of the cylinder: X separates by
    // roughly the diameter (~20 mm) while the orthogonal (Y) coordinate stays near the centre.
    REQUIRE(seam_x_plus.x() - seam_x_minus.x() > 8.0);
    REQUIRE_THAT(seam_x_plus.y() - seam_x_minus.y(), Catch::Matchers::WithinAbs(0.0, 4.0));

    // Symmetric check on the Y axis.
    REQUIRE(seam_y_plus.y() - seam_y_minus.y() > 8.0);
    REQUIRE_THAT(seam_y_plus.x() - seam_y_minus.x(), Catch::Matchers::WithinAbs(0.0, 4.0));

    // "Farthest from point" flips the side: aiming at +X with reference=farthest puts the seam
    // on the -X side, i.e. opposite the closest-to-+X result.
    size_t n_far = 0;
    const Vec2d seam_x_plus_far = outer_wall_seam_mean("40", "0", n_far, "farthest");
    REQUIRE(n_far > 5);
    REQUIRE(seam_x_plus.x() - seam_x_plus_far.x() > 8.0);
}
