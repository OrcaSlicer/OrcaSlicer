#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"

#include "test_helpers.hpp"

#include <cmath>
#include <fstream>
#include <boost/filesystem.hpp>

using namespace Slic3r;

// UNLAYERED_MOD -- the virtual-5-axis tilt limit, pinned as pure geometry: no printer, no slicing,
// no GUI. Both enforcement points (the cut-plane gizmo's drag clamp and Print::validate()) call
// this one function, so if these numbers are right the two cannot disagree about what is safe.
//
// See virtual_tilt_limit_deg() and HotendEnvelope in PrintConfig.hpp for the derivation.

namespace {
// Generous enough that the gantry never binds, so the hotend term can be measured on its own.
constexpr double HUGE_GANTRY = 1.0e6;

// Which way a build normal leans. A chunk leaning toward +X climbs toward -X, so its -X side binds.
constexpr double LEAN_POS_X = 0.;
constexpr double LEAN_NEG_X = PI;
constexpr double LEAN_POS_Y = 0.5 * PI;
constexpr double LEAN_NEG_Y = -0.5 * PI;

HotendEnvelope envelope(double xpos, double xneg, double ypos, double yneg, double height = 40.)
{
    HotendEnvelope e;
    e.height = height;
    e.angle  = { xpos, xneg, ypos, yneg };
    return e;
}
HotendEnvelope square(double angle) { return envelope(angle, angle, angle, angle); }
// Every side flush with the nozzle: the hotend itself never limits anything.
HotendEnvelope needle() { return square(90.); }

} // namespace

SCENARIO("virtual 5-axis tilt limit: the hotend pyramid", "[VirtualTiltLimit]")
{
    GIVEN("a printer whose gantry is effectively out of reach") {
        THEN("tilting straight at one side, the limit is that side's angle from the bed") {
            // The old cone test's guard, restated: the angle that limits tilt is the COMPLEMENT of
            // the face's angle from the nozzle axis, which is its angle from the bed. At 45 the two
            // coincide, so only the unequal sides below tell a right implementation from a wrong one.
            const HotendEnvelope e = envelope(80., 30., 60., 45.);
            CHECK(virtual_tilt_limit_deg(e, HUGE_GANTRY, 0., LEAN_POS_X) == Catch::Approx(30.)); // climbs toward -X
            CHECK(virtual_tilt_limit_deg(e, HUGE_GANTRY, 0., LEAN_NEG_X) == Catch::Approx(80.)); // climbs toward +X
            CHECK(virtual_tilt_limit_deg(e, HUGE_GANTRY, 0., LEAN_POS_Y) == Catch::Approx(45.)); // climbs toward -Y
            CHECK(virtual_tilt_limit_deg(e, HUGE_GANTRY, 0., LEAN_NEG_Y) == Catch::Approx(60.)); // climbs toward +Y
        }

        THEN("the side that binds is the one the layer climbs toward") {
            const HotendEnvelope e = envelope(80., 30., 60., 45.);
            CHECK(virtual_tilt_binding_face(e, LEAN_POS_X) == HotendEnvelope::XNeg);
            CHECK(virtual_tilt_binding_face(e, LEAN_NEG_X) == HotendEnvelope::XPos);
            CHECK(virtual_tilt_binding_face(e, LEAN_POS_Y) == HotendEnvelope::YNeg);
            CHECK(virtual_tilt_binding_face(e, LEAN_NEG_Y) == HotendEnvelope::YPos);
        }

        THEN("a slimmer side allows MORE tilt toward it") {
            CHECK(virtual_tilt_limit_deg(envelope(45., 70., 45., 45.), HUGE_GANTRY, 0., LEAN_POS_X) >
                  virtual_tilt_limit_deg(envelope(45., 20., 45., 45.), HUGE_GANTRY, 0., LEAN_POS_X));
        }

        THEN("between two sides the pyramid's corner leads, so a diagonal is stricter than either side") {
            // Four 45-degree sides are the old 90-degree cone along each axis, but at 45 degrees of
            // azimuth the corner stands sqrt(2) further out: atan(1/sqrt(2)) = 35.26.
            const HotendEnvelope e = square(45.);
            CHECK(virtual_tilt_limit_deg(e, HUGE_GANTRY, 0., LEAN_POS_X) == Catch::Approx(45.));
            CHECK(virtual_tilt_limit_deg(e, HUGE_GANTRY, 0., 0.25 * PI) == Catch::Approx(35.264).margin(0.01));
        }

        THEN("an unknown direction yields the worst one, which no direction undercuts") {
            const HotendEnvelope e = envelope(80., 30., 60., 45.);
            const double worst = virtual_tilt_limit_deg(e, HUGE_GANTRY, 0.);
            double sampled_min = 90.;
            for (int i = 0; i < 3600; ++ i) {
                const double limit = virtual_tilt_limit_deg(e, HUGE_GANTRY, 0., 2. * PI * i / 3600.);
                CHECK(limit >= worst - 1e-9);
                sampled_min = std::min(sampled_min, limit);
            }
            CHECK(sampled_min == Catch::Approx(worst).margin(0.01));
        }

        THEN("a zero envelope height switches the hotend term off") {
            CHECK(virtual_tilt_hotend_term_deg(envelope(10., 10., 10., 10., 0.), LEAN_POS_X) == Catch::Approx(90.));
            CHECK(virtual_tilt_limit_deg(envelope(10., 10., 10., 10., 0.), HUGE_GANTRY, 0., LEAN_POS_X) == Catch::Approx(90.));
        }

        THEN("out-of-range side angles are clamped rather than going nonsense") {
            for (const double angle : { -5., 0., 90., 200. }) {
                const double limit = virtual_tilt_limit_deg(square(angle), HUGE_GANTRY, 0., LEAN_POS_X);
                CHECK(limit > 0.);
                CHECK(limit <= 90.);
            }
        }
    }
}

SCENARIO("virtual 5-axis tilt limit: the gantry", "[VirtualTiltLimit]")
{
    GIVEN("40 mm of headroom under the gantry and a hotend that never binds") {
        THEN("the limit is the angle whose climb across the chunk just fits") {
            // A chunk 80 mm across climbing at atan(40/80) rises exactly 40 mm end to end.
            CHECK(virtual_tilt_limit_deg(needle(), 40., 80., LEAN_POS_X) == Catch::Approx(26.565).margin(0.01));
            // Square case: as wide as it is tall -> 45 degrees.
            CHECK(virtual_tilt_limit_deg(needle(), 40., 40., LEAN_POS_X) == Catch::Approx(45.));
        }

        THEN("a wider chunk is allowed less tilt") {
            const double narrow = virtual_tilt_limit_deg(square(45.), 40., 40., LEAN_POS_X);
            const double wide   = virtual_tilt_limit_deg(square(45.), 40., 200., LEAN_POS_X);
            CHECK(wide < narrow);
        }

        THEN("an unknown reach falls back to the hotend term alone") {
            // 0 means "not derived yet" -- the looser answer, so a caller that cannot supply a
            // reach never over-restricts the user. Print::validate() always can.
            CHECK(virtual_tilt_limit_deg(square(45.), 40., 0., LEAN_POS_X) == Catch::Approx(45.));
            CHECK(virtual_tilt_limit_deg(square(45.), 40., -1., LEAN_POS_X) == Catch::Approx(45.));
        }
    }
}

SCENARIO("virtual 5-axis tilt limit: whichever constraint is stricter wins", "[VirtualTiltLimit]")
{
    GIVEN("four 45 degree sides and 40 mm of gantry clearance") {
        THEN("a small chunk is bounded by the hotend and a large one by the gantry") {
            // 20 mm across: the gantry would allow atan(40/20) = 63 degrees, so the hotend's 45 binds.
            CHECK(virtual_tilt_limit_deg(square(45.), 40., 20., LEAN_POS_X) == Catch::Approx(45.));
            // 160 mm across: the gantry allows atan(40/160) = 14, well inside the hotend's 45.
            CHECK(virtual_tilt_limit_deg(square(45.), 40., 160., LEAN_POS_X) == Catch::Approx(14.036).margin(0.01));
        }
    }
}

SCENARIO("virtual 5-axis tilt limit: each term has its own off switch", "[VirtualTiltLimit]")
{
    GIVEN("no gantry height") {
        THEN("the hotend still limits the tilt, whatever the chunk's size") {
            // 0 only means the gantry was never measured. The upright nozzle meets its own hotend
            // regardless, and that limit used to be skipped too, so a steep chunk printed straight
            // into it on a 3-axis machine.
            CHECK(virtual_tilt_limit_deg(square(45.), 0., 50., LEAN_POS_X) == Catch::Approx(45.));
            CHECK(virtual_tilt_limit_deg(square(45.), 0., 5000., LEAN_POS_X) == Catch::Approx(45.));
            CHECK(virtual_tilt_limit_deg(square(75.), 0., 50.) == Catch::Approx(virtual_tilt_hotend_term_deg(square(75.), std::nullopt)));
            CHECK(virtual_tilt_limit_deg(square(45.), -1., 50., LEAN_POS_X) == Catch::Approx(45.));
        }
        THEN("with the hotend envelope off too, there is no limit at all") {
            // 0 is the agreed "no limit" signal to every caller.
            CHECK(virtual_tilt_limit_deg(envelope(45., 45., 45., 45., 0.), 0., 50., LEAN_POS_X) == 0.);
            CHECK(virtual_tilt_limit_deg(envelope(45., 45., 45., 45., 0.), -1., 50.) == 0.);
        }
    }

    GIVEN("a geometry so hostile that the honest answer is ~0 degrees") {
        THEN("it still reports a limit rather than reading as 'no limit'") {
            // A 2 m chunk under 1 mm of clearance. The real answer rounds to zero, but returning 0
            // would mean "unlimited" to every caller and open the machine right up, so the floor
            // has to stay above it.
            const double limit = virtual_tilt_limit_deg(square(45.), 1., 2000., LEAN_POS_X);
            CHECK(limit > 0.);
            CHECK(limit < 1.);
        }
    }
}

SCENARIO("tool_head_frame: the head's real orientation for a tool axis", "[VirtualTiltLimit][hotend]")
{
    auto check_rotation = [](const Matrix3d &R) {
        CHECK((R * R.transpose()).isApprox(Matrix3d::Identity(), 1e-9));
        CHECK(R.determinant() == Catch::Approx(1.));
    };
    GIVEN("a vertical tool axis") {
        THEN("the head frame is the bed frame (C = 0, the home pose)") {
            CHECK(tool_head_frame(Vec3d::UnitZ()).isApprox(Matrix3d::Identity()));
        }
    }
    GIVEN("a tool axis leaning 30 degrees toward +X (B-60 C0)") {
        const Matrix3d R = tool_head_frame(Vec3d(0.5, 0., std::sqrt(0.75)));
        THEN("the head's +X side tips with it and +Y, the B axis, stays level") {
            check_rotation(R);
            CHECK(Vec3d(R.row(0).transpose()).isApprox(Vec3d(std::sqrt(0.75), 0., -0.5), 1e-9));
            CHECK(Vec3d(R.row(1).transpose()).isApprox(Vec3d(0., 1., 0.), 1e-9));
            CHECK(Vec3d(R.row(2).transpose()).isApprox(Vec3d(0.5, 0., std::sqrt(0.75)), 1e-9));
        }
    }
    GIVEN("a tool axis leaning 30 degrees toward +Y (the table turned to C90)") {
        const Matrix3d R = tool_head_frame(Vec3d(0., 0.5, std::sqrt(0.75)));
        THEN("the table has turned under the head, so the head's +X side faces bed +Y") {
            check_rotation(R);
            CHECK(Vec3d(R.row(0).transpose()).isApprox(Vec3d(0., std::sqrt(0.75), -0.5), 1e-9));
            CHECK(Vec3d(R.row(1).transpose()).isApprox(Vec3d(-1., 0., 0.), 1e-9));
        }
    }
}

SCENARIO("hotend envelope: the single cone angle migrates to four sides", "[VirtualTiltLimit][hotend]")
{
    auto migrated = [](double legacy_full_angle) {
        DynamicPrintConfig cfg;
        cfg.apply(FullPrintConfig::defaults());
        cfg.set_key_value("nonplanar_hotend_cone_angle", new ConfigOptionFloat(legacy_full_angle));
        migrate_legacy_hotend_cone(cfg);
        CHECK(! cfg.has("nonplanar_hotend_cone_angle"));
        return HotendEnvelope::from_config(cfg);
    };
    THEN("the default 90 degree cone becomes four 45 degree sides") {
        const HotendEnvelope e = migrated(90.);
        for (double a : e.angle) CHECK(a == Catch::Approx(45.));
        CHECK(e.height == Catch::Approx(40.));
    }
    THEN("a 60 degree cone becomes four 60 degree sides") {
        for (double a : migrated(60.).angle) CHECK(a == Catch::Approx(60.));
    }
    THEN("0, which used to disable the check, becomes a zero envelope height") {
        CHECK(! migrated(0.).enabled());
    }
    THEN("the defaults never carry the legacy key, so nothing is migrated that was not loaded") {
        DynamicPrintConfig cfg;
        cfg.apply(FullPrintConfig::defaults());
        CHECK(! cfg.has("nonplanar_hotend_cone_angle"));
        cfg.set_key_value("nonplanar_hotend_angle_xneg", new ConfigOptionFloats{ 20. });
        migrate_legacy_hotend_cone(cfg);
        CHECK(HotendEnvelope::from_config(cfg).angle[HotendEnvelope::XNeg] == Catch::Approx(20.));
    }
}

TEST_CASE("A virtual 5-axis slice steeper than the hotend allows is refused without a gantry height", "[VirtualTiltLimit]")
{
    // Every side of the hotend at 5 degrees from the bed, so tilting along X the hotend allows 5
    // degrees. The gantry height is left at 0, i.e. not measured, which used to switch the whole
    // limit off and let a steep chunk print straight into the hotend.
    const double allowed = 5.;
    auto validate = [allowed](double tilt) {
        Model        model;
        ModelObject *object = model.add_object();
        object->add_volume(make_cube(20., 20., 20.));
        object->add_instance()->set_offset(Vec3d(100., 100., 0.));
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_nonplanar",             "1" },
            { "axis_selection",               "virtual" },
            { "nonplanar_gantry_height",      0 },
            { "nonplanar_virtual_max_angle",  0 },
            { "nonplanar_hotend_cone_height", 40 },
            { "nonplanar_hotend_angle_xpos",  allowed },
            { "nonplanar_hotend_angle_xneg",  allowed },
            { "nonplanar_hotend_angle_ypos",  allowed },
            { "nonplanar_hotend_angle_yneg",  allowed },
            { "layer_rotation_b",             tilt },
            { "layer_rotation_c",             0 },
            { "layer_change_gcode",           "G92 E0" },
        });
        Print print;
        print.apply(model, config);
        REQUIRE(print.config().virtual_5axis.value);
        return print.validate();
    };

    const StringObjectException refused = validate(2. * allowed);
    INFO(refused.string);
    CHECK(refused.string.find("allows at most 5.0 degrees") != std::string::npos);
    // The hotend, not the gantry, is named as what binds.
    CHECK(refused.opt_key.rfind("nonplanar_hotend_angle_", 0) == 0);

    const StringObjectException accepted = validate(allowed - 1.);
    INFO(accepted.string);
    CHECK(accepted.string.empty());
}


// UNLAYERED_MOD 2026-10-11 -- "Max print height (from the bed)" checks the toolpath only.
//
// The check scanned the whole written G-code, so a printer's own end G-code that lifts and
// presents the print (Prusa MK3S: Z up, then X0 Y200) refused every virtual 5-axis slice. It was
// most of the slice errors 0.1.3 reported on its first day.
TEST_CASE("The virtual 5-axis height limit ignores the printer's own start and end G-code", "[VirtualTiltLimit]")
{
    // A 20 mm cube, no tilt: the toolpath tops out a little above 20 mm.
    auto slice = [](double max_z) {
        Model        model;
        ModelObject *object = model.add_object();
        object->add_volume(make_cube(20., 20., 20.));
        object->add_instance()->set_offset(Vec3d(100., 100., 0.));
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_nonplanar",            "1" },
            { "axis_selection",              "virtual" },
            { "nonplanar_virtual_max_z",     max_z },
            { "layer_change_gcode",          "G92 E0" },
            { "machine_start_gcode",         "G1 Z120 F600 ; start lift\n" },
            { "machine_end_gcode",           "G1 Z150 F600 ; lift\nG1 X0 Y200 F3000 ; present print\n" },
        });
        Print print;
        print.apply(model, config);
        REQUIRE(print.config().virtual_5axis.value);
        print.set_status_silent();
        print.process();
        // The height check runs on the export WITH a processor result, as the app's is (Test::gcode
        // exports without one and would skip it). Absolute temp path, as test_cut_plane_apply does.
        const auto out_path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("maxz-%%%%%%%%.gcode");
        struct Remove { boost::filesystem::path p; ~Remove() { boost::system::error_code ec; boost::filesystem::remove(p, ec); } } remove{out_path};
        GCodeProcessorResult result;
        print.export_gcode(out_path.string(), &result, nullptr);
        std::ifstream in(out_path.string());
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };

    // 25 mm clears the 20 mm part. The Z120 start and Z150 end moves are the printer's, not the
    // toolpath, and a virtual print no longer climbs 20 mm "before rotation" at the end.
    std::string gcode;
    REQUIRE_NOTHROW(gcode = slice(25.));
    CHECK(gcode.find("lift before rotation") == std::string::npos);
    CHECK(gcode.find("G1 Z150") != std::string::npos);

    // 10 mm is below the part itself: still refused, naming the setting and where it is.
    try {
        slice(10.);
        FAIL("a toolpath above the limit was not refused");
    } catch (const SlicingError &e) {
        const std::string msg = e.what();
        INFO(msg);
        CHECK(msg.find("Max print height (from the bed)") != std::string::npos);
        CHECK(msg.find("Motion ability") != std::string::npos);
        // Below 30 mm the message explains that the limit is a height, not a clearance.
        CHECK(msg.find("measured from the bed") != std::string::npos);
    }
}


// UNLAYERED_MOD 2026-10-31 (0.1.9) -- "Max print height (from the bed)" entered as a clearance.
//
// The field is an absolute Z, but it sits directly under "Gantry height above nozzle", which is a
// clearance, and it keeps being entered as a few millimetres of headroom above the part. Until this
// check the only enforcement was the post-export scan, so every such attempt cost a complete slice
// and G-code export before anything was reported. A limit below the top of the part cannot be met at
// all -- the nozzle has to reach the top to print it -- so it is refused from the plate alone.
TEST_CASE("A virtual 5-axis height limit below the part is refused before slicing", "[VirtualTiltLimit]")
{
    // 50 mm tall, upright, so nothing but the height limit can refuse it.
    auto validate = [](double max_z, const char *nonplanar = "1") {
        Model        model;
        ModelObject *object = model.add_object();
        object->name        = "tall part";
        object->add_volume(make_cube(20., 20., 50.));
        object->add_instance()->set_offset(Vec3d(100., 100., 0.));
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_nonplanar",        nonplanar },
            { "axis_selection",          "virtual" },
            { "nonplanar_virtual_max_z", max_z },
            { "layer_change_gcode",      "G92 E0" },
        });
        Print print;
        print.apply(model, config);
        return print.validate();
    };

    // 5 mm reads as headroom above the part, and is 45 mm below the top of it. Nothing is sliced:
    // validate() runs before the background process starts a slice at all.
    const StringObjectException headroom = validate(5.);
    INFO(headroom.string);
    CHECK(! headroom.string.empty());
    CHECK(headroom.opt_key == "nonplanar_virtual_max_z");
    // Names the setting and where it lives, says what the number means, and names the object.
    CHECK(headroom.string.find("Max print height (from the bed)") != std::string::npos);
    CHECK(headroom.string.find("Motion ability") != std::string::npos);
    CHECK(headroom.string.find("measured from the bed") != std::string::npos);
    CHECK(headroom.string.find("tall part") != std::string::npos);

    // Every size of clearance someone might type reads the same way, sub-millimetre ones included.
    for (const double clearance : { 0.3, 3., 8., 12. }) {
        const StringObjectException refused = validate(clearance);
        INFO(refused.string);
        CHECK(refused.opt_key == "nonplanar_virtual_max_z");
    }

    // 0 is "no limit", a limit above the part is the setting used as intended, and the top of the
    // part itself is not yet over it.
    CHECK(validate(0.).string.empty());
    CHECK(validate(300.).string.empty());
    CHECK(validate(50.).string.empty());

    // Non-planar printing off: the limit is a virtual 5-axis setting and does not bind here.
    CHECK(validate(5., "0").string.empty());
}


// UNLAYERED_MOD 2026-10-11 (0.1.4) -- 0.1.3 diagnostics reported two slicing crashes (access
// violation) on an FLSun V400: a delta whose round bed is centred on the origin, with cut planes,
// virtual 5-axis, by-group sequence and a prime tower. A cube does not reproduce it on that bed with
// any printer type, with or without the tower (the crash needs their geometry; 0.1.4's crash_at will
// say where), but the combination is covered from now on.
TEST_CASE("Virtual 5-axis cut planes slice on a round bed centred on the origin (FLSun V400)", "[VirtualTiltLimit][RoundBed]")
{
    const double deg = PI / 180.;
    for (const Vec3d offset : { Vec3d(0., 0., 0.), Vec3d(40., -30., 0.) }) {
        for (const char *tower : { "0", "1" })
        for (const char *axes : { "virtual", "ab", "ac", "bc" }) {
            Model        model;
            ModelObject *object = model.add_object();
            object->name        = "cube";
            object->add_volume(make_cube(20., 20., 30.));
            ModelInstance *instance = object->add_instance();
            instance->set_offset(offset);
            CutPlaneStack stack;
            stack.enabled = true;
            CutPlane cp;
            cp.point  = Vec3d(10., 10., 15.);
            cp.normal = Vec3d(std::sin(15. * deg), 0., std::cos(15. * deg)).normalized();
            stack.planes.push_back(cp);
            object->cut_planes  = stack;
            object->auto_drop   = true;
            object->ensure_on_bed();

            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_deserialize_strict({
                { "printable_area",       "149.429x13.0734,147.721x26.0472,144.889x38.8229,140.954x51.303,135.946x63.3927,129.904x75,122.873x86.0365,114.907x96.4181,106.066x106.066,96.4181x114.907,86.0365x122.873,75x129.904,63.3927x135.946,51.303x140.954,38.8229x144.889,26.0472x147.721,13.0734x149.429,0x150,-13.0734x149.429,-26.0472x147.721,-38.8229x144.889,-51.303x140.954,-63.3927x135.946,-75x129.904,-86.0365x122.873,-96.4181x114.907,-106.066x106.066,-114.907x96.4181,-122.873x86.0365,-129.904x75,-135.946x63.3927,-140.954x51.303,-144.889x38.8229,-147.721x26.0472,-149.429x13.0734,-150x0,-149.429x-13.0734,-147.721x-26.0472,-144.889x-38.8229,-140.954x-51.303,-135.946x-63.3927,-129.904x-75,-122.873x-86.0365,-114.907x-96.4181,-106.066x-106.066,-96.4181x-114.907,-86.0365x-122.873,-75x-129.904,-63.3927x-135.946,-51.303x-140.954,-38.8229x-144.889,-26.0472x-147.721,-13.0734x-149.429,-0x-150,13.0734x-149.429,26.0472x-147.721,38.8229x-144.889,51.303x-140.954,63.3927x-135.946,75x-129.904,86.0365x-122.873,96.4181x-114.907,106.066x-106.066,114.907x-96.4181,122.873x-86.0365,129.904x-75,135.946x-63.3927,140.954x-51.303,144.889x-38.8229,147.721x-26.0472,149.429x-13.0734,150x-0" },
                { "printable_height",     "410" },
                { "enable_nonplanar",     "1" },
                { "axis_selection",       axes },
                { "print_sequence",       "by group" },
                { "enable_prime_tower",   tower },
                { "wall_generator",       "arachne" },
                { "layer_change_gcode",   "G92 E0" },
            });
            INFO("offset " << offset.x() << "," << offset.y() << ", prime tower " << tower << ", axes " << axes);
            Print print;
            print.apply(model, config);
            print.set_status_silent();
            const auto out_path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("roundbed-%%%%%%%%.gcode");
            struct Remove { boost::filesystem::path p; ~Remove() { boost::system::error_code ec; boost::filesystem::remove(p, ec); } } remove{out_path};
            GCodeProcessorResult result;
            // A hardware exception here kills the test run: that is the reproduction.
            try {
                print.process();
                print.export_gcode(out_path.string(), &result, nullptr);
            } catch (const SlicingError &e) {
                // A refusal (tilt limit, collision) is fine; only a crash is the bug.
                WARN("refused: " << e.what());
            }
            SUCCEED();
        }
    }
}
// The scan Print.cpp refuses a slice with, as a helper: the lowest Z word of the toolpath (outside
// the printer's own ;TYPE:Custom blocks) and the number of extruding moves in a written file.
static std::pair<double, size_t> toolpath_low_z(const std::string &gcode_path)
{
    std::ifstream in(gcode_path);
    std::string   line;
    bool          custom = false;
    double        low_z  = 1e9;
    size_t        extrusions = 0;
    while (std::getline(in, line)) {
        if (line.rfind(";TYPE:", 0) == 0) {
            custom = line.find("Custom") != std::string::npos;
            continue;
        }
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
            continue;
        const std::string code = line.substr(0, line.find(';'));
        if (const size_t z = code.find(" Z"); z != std::string::npos && ! custom)
            low_z = std::min(low_z, std::atof(code.c_str() + z + 2));
        if (code.find(" E") != std::string::npos && (code.find(" X") != std::string::npos || code.find(" Y") != std::string::npos))
            ++ extrusions;
    }
    return std::make_pair(low_z, extrusions);
}

// UNLAYERED_MOD 2026-10-11 (0.1.4) -- a customer's A1 scraped its bed printing a tilted benchy with
// tilted layers. An object sunk into the plate with a whole-object layer rotation was sliced from its
// own minimum along the TILTED axis, which kept part of what is under the plate: walls went to
// Z -8.3 on a sunk 45 degree benchy. Now it is clipped at the plate, and the export refuses any
// toolpath below it as a last line of defence.
TEST_CASE("A sunk object with a layer rotation is clipped at the build plate", "[VirtualTiltLimit][BelowPlate]")
{
    auto slice = [](double sink) {
        Model        model;
        ModelObject *object = model.add_object();
        object->add_volume(make_cube(20., 20., 20.));
        object->add_instance()->set_offset(Vec3d(100., 100., - sink));
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_nonplanar",   "1" },
            { "axis_selection",     "virtual" },
            { "layer_rotation_b",   30 },
            { "layer_rotation_c",   0 },
            { "layer_change_gcode", "G92 E0" },
        });
        Print print;
        print.apply(model, config);
        REQUIRE(print.config().virtual_5axis.value);
        print.set_status_silent();
        print.process();
        const auto out_path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("sunk-%%%%%%%%.gcode");
        struct Remove { boost::filesystem::path p; ~Remove() { boost::system::error_code ec; boost::filesystem::remove(p, ec); } } remove{out_path};
        GCodeProcessorResult result;
        print.export_gcode(out_path.string(), &result, nullptr);
        return toolpath_low_z(out_path.string());
    };

    // Resting on the plate: the clip keeps everything but the half-bead sliver under its margin.
    const auto [rest_z, rest_n] = slice(0.);
    CHECK(rest_z >= 0.);
    CHECK(rest_n > 0);

    // Sunk 8 mm: slices without being refused, and nothing goes below the plate.
    std::pair<double, size_t> sunk;
    REQUIRE_NOTHROW(sunk = slice(8.));
    CHECK(sunk.first >= 0.);
    CHECK(sunk.second > 0);
    // Less material than the cube on the plate: the part under the plate is gone.
    CHECK(sunk.second < rest_n);
}

// UNLAYERED_MOD 2026-11-01 (0.1.9) -- the plate clip above ran only for an object whose box ALREADY
// dipped below the plate, and that box spanned every copy of the object. A tilted object resting on
// the plate was therefore sliced with no clip at all, and when its toolpath came out below the plate
// the export refused the slice -- after the whole slice and G-code export had run. The clip is now
// gated on the layer rotation alone, so the plate bounds every tilted object, at every tilt and for
// every copy.
TEST_CASE("A tilted object resting on the plate is clipped rather than refused", "[VirtualTiltLimit][BelowPlate]")
{
    // `copies` instances of one box, all resting on the plate (or sunk by `sink`), sliced in a frame
    // tilted by B/C. Returns the lowest toolpath Z and the number of extruding moves.
    auto slice = [](double tilt_b, double tilt_c, size_t copies, double sink = 0.) {
        Model        model;
        ModelObject *object = model.add_object();
        object->add_volume(make_cube(20., 20., 40.));
        for (size_t i = 0; i < copies; ++ i)
            object->add_instance()->set_offset(Vec3d(70. + 40. * double(i), 100., - sink));
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_nonplanar",   "1" },
            { "axis_selection",     "virtual" },
            { "layer_rotation_b",   tilt_b },
            { "layer_rotation_c",   tilt_c },
            { "layer_change_gcode", "G92 E0" },
        });
        Print print;
        print.apply(model, config);
        REQUIRE(print.config().virtual_5axis.value);
        print.set_status_silent();
        print.process();
        const auto out_path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("ontheplate-%%%%%%%%.gcode");
        struct Remove { boost::filesystem::path p; ~Remove() { boost::system::error_code ec; boost::filesystem::remove(p, ec); } } remove{out_path};
        GCodeProcessorResult result;
        // Throws SlicingError if the export finds a Z below the plate: that refusal is the bug.
        print.export_gcode(out_path.string(), &result, nullptr);
        return toolpath_low_z(out_path.string());
    };

    // Shallow tilts, where the dip is no more than a bead or two and the part plainly rests on the
    // plate, and a steep one and a tilt about both axes for good measure.
    for (const std::pair<double, double> &bc : { std::make_pair(5., 0.), std::make_pair(10., 0.),
                                                 std::make_pair(30., 0.), std::make_pair(10., 45.) }) {
        INFO("layer_rotation_b " << bc.first << ", layer_rotation_c " << bc.second);
        std::pair<double, size_t> one;
        REQUIRE_NOTHROW(one = slice(bc.first, bc.second, 1));
        CHECK(one.first >= 0.);
        CHECK(one.second > 0);
    }

    // Several copies: each is sliced in its own frame (one PrintObject per instance for a rotated
    // object), so the clip must be decided per copy and not from the box around all of them.
    std::pair<double, size_t> several;
    REQUIRE_NOTHROW(several = slice(10., 0., 3));
    CHECK(several.first >= 0.);
    CHECK(several.second > 0);

    // ... and with the copies sunk into the plate, which is the case that was clipped before.
    std::pair<double, size_t> several_sunk;
    REQUIRE_NOTHROW(several_sunk = slice(10., 0., 3, 6.));
    CHECK(several_sunk.first >= 0.);
    CHECK(several_sunk.second > 0);
    CHECK(several_sunk.second < several.second);
}
