#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include "test_helpers.hpp"

#include <cmath>
#include <regex>
#include <sstream>
#include <string>

using namespace Slic3r;

// UNLAYERED_MOD 2026-10-09 -- {max_layer_z} must be the true top of the printed part.
//
// On a Bambu Lab H2S a virtual 5-axis Benchy (By group, one ~5 degree cut plane, three chunks) ended
// with the stock Bambu end G-code "G1 Z{max_layer_z + 0.5} F900 ; lower z a little". The template
// got max_layer_z = 37.4 -- the first chunk's top -- while the part ended at ~67.5, because a later
// chunk's print_z is measured along its own build normal from its own base, not above the bed. The
// nozzle drove 30 mm down into the finished part. Every Bambu profile carries that line.

namespace {

struct EndOfPrint
{
    double max_extrusion_z = 0.; // highest Z any extrusion was laid at
    double end_lower_z     = -1.; // the Z the "lower z a little" line moved to
};

EndOfPrint scan(const std::string &gcode)
{
    EndOfPrint         r;
    std::istringstream in(gcode);
    std::string        line;
    double             z = 0.;
    bool               in_end = false;
    const std::regex   num(R"(([XYZE])(-?\d*\.?\d+))");
    while (std::getline(in, line)) {
        if (line.find("; TEST_END_MARK") != std::string::npos) {
            in_end = true;
            continue;
        }
        if (line.rfind("G1", 0) != 0 && line.rfind("G0", 0) != 0)
            continue;
        const std::string code = line.substr(0, line.find(';'));
        bool   has_xy = false, has_z = false;
        double e = 0., nz = z;
        for (std::sregex_iterator it(code.begin(), code.end(), num), end; it != end; ++it) {
            const char   axis = (*it)[1].str()[0];
            const double v    = std::stod((*it)[2].str());
            if (axis == 'X' || axis == 'Y') has_xy = true;
            if (axis == 'Z') { has_z = true; nz = v; }
            if (axis == 'E') e = v;
        }
        if (in_end && has_z && r.end_lower_z < 0.) {
            r.end_lower_z = nz;
            break;
        }
        if (e > 0. && has_xy)
            r.max_extrusion_z = std::max(r.max_extrusion_z, nz);
        z = nz;
    }
    return r;
}

std::string slice_segmented_cube(const Vec3d &plane_normal, const std::string &axis_selection)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "tall_cube";
    object->add_volume(Test::cube(20.), ModelVolumeType::MODEL_PART, false);
    ModelInstance *instance = object->add_instance();
    instance->set_offset(Vec3d(120., 120., 0.));

    CutPlaneStack stack;
    stack.enabled = true;
    CutPlane cp;
    cp.point  = Vec3d(10., 10., 10.);
    cp.normal = plane_normal.normalized();
    stack.planes.push_back(cp);
    object->cut_planes  = stack;
    object->auto_drop = true;
    object->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_nonplanar",   "1" },
        { "axis_selection",     axis_selection },
        { "print_sequence",     "by group" },
        { "layer_change_gcode", "G92 E0" },
        // Bambu's stock end G-code line, exactly as the H2S profile has it.
        { "machine_end_gcode",  "; TEST_END_MARK\nG1 Z{max_layer_z + 0.5} F900 ; lower z a little\n" },
    });
    Slic3r::Print print;
    print.apply(model, config);
    print.process();
    return Test::gcode(print);
}

} // namespace

TEST_CASE("{max_layer_z} in the end G-code clears the whole part on a multiplanar print", "[cutplanes][gcode][placeholders]")
{
    // ~5 degrees, the H2S Benchy's cut; and a steeper one for margin.
    const double deg = PI / 180.;
    for (double tilt_deg : { 5.5, 15. }) {
        for (const char *axes : { "virtual", "bc" }) {
            const Vec3d normal(std::sin(tilt_deg * deg), 0., std::cos(tilt_deg * deg));
            const std::string gcode = slice_segmented_cube(normal, axes);
            const EndOfPrint  r     = scan(gcode);
            INFO("tilt " << tilt_deg << " deg, axes " << axes << ": highest extrusion Z " << r.max_extrusion_z
                 << ", end G-code lowered to Z " << r.end_lower_z);
            REQUIRE(r.max_extrusion_z > 15.);   // the cube really printed to the top
            REQUIRE(r.end_lower_z >= 0.);       // the end G-code line was emitted and found
            // "lower z a little" must never go below the material already printed.
            CHECK(r.end_lower_z >= r.max_extrusion_z);
        }
    }
}
