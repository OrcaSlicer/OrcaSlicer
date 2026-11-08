// UNLAYERED_MOD — the multiplanar (5-axis) kinematics conventions.
//
// Everything downstream of slicing keys off two things: the pre-slice rotation that turns a chunk's
// print direction into +Z, and the tool axis / machine B-C pair derived from it. The G-code viewer
// reconstructs the tool axis from the file, so the forward and inverse maps have to agree exactly.

#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

static Vec3d axis_of(const Transform3d &trafo) { return (trafo.rotation() * Vec3d::UnitZ()).normalized(); }

SCENARIO("Multiplanar layer rotation", "[NonPlanar]") {
    GIVEN("a PrintObjectConfig with layer_rotation_b/c") {
        PrintObjectConfig cfg;
        cfg.layer_rotation_b.value = 30.0;
        cfg.layer_rotation_c.value = 45.0;

        THEN("the rotation is the identity while non-planar printing is disabled") {
            const Transform3d trafo = layer_rotation_trafo(cfg, false);
            REQUIRE(trafo.isApprox(Transform3d::Identity()));
        }

        THEN("it is Rb(Y) * Rc(Z) when enabled, and a pure rotation") {
            const Transform3d trafo = layer_rotation_trafo(cfg, true);
            const double b = 30.0 * PI / 180.0;
            const double c = 45.0 * PI / 180.0;
            Eigen::Matrix3d Rb;
            Rb << std::cos(b), 0.0, std::sin(b), 0.0, 1.0, 0.0, -std::sin(b), 0.0, std::cos(b);
            Eigen::Matrix3d Rc;
            Rc << std::cos(c), -std::sin(c), 0.0, std::sin(c), std::cos(c), 0.0, 0.0, 0.0, 1.0;
            REQUIRE(trafo.linear().isApprox(Rb * Rc));
            REQUIRE(trafo.translation().isApprox(Vec3d::Zero()));
        }

        THEN("zero angles give the identity even when enabled") {
            PrintObjectConfig flat;
            REQUIRE(layer_rotation_trafo(flat, true).isApprox(Transform3d::Identity()));
        }
    }
}

SCENARIO("Tool axis and machine B/C round trip", "[NonPlanar]") {
    GIVEN("the identity orientation") {
        THEN("the tool points straight up, which is the machine's B = -90 datum") {
            double b = 0.0, c = 0.0;
            tool_axis_to_bc(Vec3d::UnitZ(), b, c);
            CHECK_THAT(b * 180.0 / PI, Catch::Matchers::WithinAbs(-90.0, 1e-9));
            CHECK_THAT(c, Catch::Matchers::WithinAbs(0.0, 1e-9));
            REQUIRE(bc_to_tool_axis(b, c).isApprox(Vec3d::UnitZ()));
        }
    }

    GIVEN("a tool axis pointing straight down") {
        THEN("B is +90 and the inverse recovers it") {
            double b = 0.0, c = 0.0;
            tool_axis_to_bc(-Vec3d::UnitZ(), b, c);
            CHECK_THAT(b * 180.0 / PI, Catch::Matchers::WithinAbs(90.0, 1e-9));
            REQUIRE(bc_to_tool_axis(b, c).isApprox(-Vec3d::UnitZ()));
        }
    }

    GIVEN("a horizontal tool axis") {
        THEN("B is 0 and C is the azimuth") {
            double b = 0.0, c = 0.0;
            tool_axis_to_bc(Vec3d(0.0, 1.0, 0.0), b, c);
            CHECK_THAT(b * 180.0 / PI, Catch::Matchers::WithinAbs(0.0, 1e-9));
            CHECK_THAT(c * 180.0 / PI, Catch::Matchers::WithinAbs(90.0, 1e-9));
            REQUIRE(bc_to_tool_axis(b, c).isApprox(Vec3d(0.0, 1.0, 0.0)));
        }
    }

    GIVEN("a sweep of layer rotations") {
        THEN("axis -> B/C -> axis is exact for every one of them") {
            for (double b_deg = -180.0; b_deg <= 180.0; b_deg += 15.0) {
                for (double c_deg = -180.0; c_deg <= 180.0; c_deg += 15.0) {
                    PrintObjectConfig cfg;
                    cfg.layer_rotation_b.value = b_deg;
                    cfg.layer_rotation_c.value = c_deg;
                    // The emitter derives B/C from post_trafo_inverse(), i.e. from the inverse of the
                    // pre-slice rotation, and that is the axis the tool physically points along.
                    const Vec3d axis = axis_of(layer_rotation_trafo(cfg, true).inverse());
                    double b = 0.0, c = 0.0;
                    tool_axis_to_bc(axis, b, c);
                    INFO("b_deg = " << b_deg << ", c_deg = " << c_deg);
                    REQUIRE(bc_to_tool_axis(b, c).isApprox(axis, 1e-9));
                }
            }
        }
    }

    GIVEN("an unnormalized tool axis") {
        THEN("it is treated as a direction") {
            double b1 = 0.0, c1 = 0.0, b2 = 0.0, c2 = 0.0;
            tool_axis_to_bc(Vec3d(1.0, 2.0, 3.0), b1, c1);
            tool_axis_to_bc(Vec3d(10.0, 20.0, 30.0), b2, c2);
            CHECK_THAT(b1, Catch::Matchers::WithinAbs(b2, 1e-12));
            CHECK_THAT(c1, Catch::Matchers::WithinAbs(c2, 1e-12));
        }
    }
}
