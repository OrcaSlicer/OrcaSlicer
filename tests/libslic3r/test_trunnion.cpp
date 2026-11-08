#include <catch2/catch_all.hpp>

#include <cmath>
#include <random>

#include "libslic3r/GCode/TrunnionAC.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
using namespace Slic3r::TrunnionAC;

// UNLAYERED_MOD 2026-10-11 (0.1.4) -- kinematics of the table-table output: the A/C trunnion and
// the rough A/B guess. Both must invert exactly (G-code -> machine -> G-code), and the pose solved
// for a tool axis must turn that axis straight up.

namespace {

Machine machine(Kind kind)
{
    Machine m;
    m.kind         = kind;
    m.pivot_height = 57.;
    m.c_offset_x   = 3.;
    m.c_offset_y   = -4.;
    m.origin_x     = 110.;
    m.origin_y     = 105.;
    m.a_min        = -100.;
    m.a_max        = 100.;
    if (kind == Kind::AB) {
        m.c_continuous = false;
        m.c_min        = -60.;
        m.c_max        = 60.;
    }
    return m;
}

Vec3 rotate_by(const std::array<double, 9> &r, const Vec3 &v)
{
    return { r[0] * v.x + r[1] * v.y + r[2] * v.z, r[3] * v.x + r[4] * v.y + r[5] * v.z, r[6] * v.x + r[7] * v.y + r[8] * v.z };
}

double dist(const Vec3 &a, const Vec3 &b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

} // namespace

TEST_CASE("Table-table output converts G-code to machine coordinates and back exactly", "[TrunnionAC]")
{
    std::mt19937                           rng(7);
    std::uniform_real_distribution<double> angle(-80., 80.), coord(-60., 160.);
    for (Kind kind : { Kind::AC, Kind::AB }) {
        const Machine m = machine(kind);
        for (int i = 0; i < 200; ++ i) {
            const Orientation o{ angle(rng), angle(rng) * (kind == Kind::AB ? 0.7 : 4.) };
            const Vec3        p(coord(rng), coord(rng), coord(rng) * 0.5 + 40.);
            const Vec3        back = to_gcode(m, o, to_machine(m, o, p));
            INFO("kind " << int(kind) << " A " << o.a << " second " << o.c);
            CHECK(dist(back, p) < 1e-9);
        }
    }
}

TEST_CASE("Table-table output moves points as a rigid rotation of the part", "[TrunnionAC]")
{
    // to_machine is the machine's rotation about its axes: distances between points are kept, and
    // the difference of two points turns by rotation(m, o).
    for (Kind kind : { Kind::AC, Kind::AB }) {
        const Machine     m = machine(kind);
        const Orientation o{ 35., kind == Kind::AB ? -25. : 120. };
        const Vec3        p(130., 90., 12.), q(95., 140., 31.);
        const Vec3        mp = to_machine(m, o, p), mq = to_machine(m, o, q);
        const Vec3        d(q.x - p.x, q.y - p.y, q.z - p.z);
        const Vec3        rd = rotate_by(rotation(m, o), d);
        INFO("kind " << int(kind));
        CHECK(std::abs(dist(mp, mq) - dist(p, q)) < 1e-9);
        CHECK(dist(Vec3(mq.x - mp.x, mq.y - mp.y, mq.z - mp.z), rd) < 1e-9);
    }
}

TEST_CASE("The solved table pose turns the tool axis straight up", "[TrunnionAC]")
{
    std::mt19937                           rng(11);
    std::uniform_real_distribution<double> tilt(0., 55.), azimuth(-180., 180.);
    for (Kind kind : { Kind::AC, Kind::AB }) {
        const Machine m = machine(kind);
        int           solved = 0;
        for (int i = 0; i < 300; ++ i) {
            const double t = tilt(rng) * M_PI / 180., az = azimuth(rng) * M_PI / 180.;
            const Vec3   n(std::sin(t) * std::cos(az), std::sin(t) * std::sin(az), std::cos(t));
            Orientation  o;
            if (! solve(m, n, Orientation{}, o))
                continue; // outside the B limits (A/B): refused, checked below
            ++ solved;
            const Vec3 up = rotate_by(rotation(m, o), n);
            INFO("kind " << int(kind) << " n " << n.x << "," << n.y << "," << n.z << " -> A " << o.a << " second " << o.c);
            CHECK(dist(up, Vec3(0., 0., 1.)) < 1e-9);
            CHECK(o.a >= m.a_min - 1e-9);
            CHECK(o.a <= m.a_max + 1e-9);
        }
        CHECK(solved > 100);
    }
}

TEST_CASE("The rough A/B guess refuses a tool axis beyond the B limits", "[TrunnionAC]")
{
    const Machine m = machine(Kind::AB); // B within +-60
    Orientation   o;
    // Tilted 70 degrees about Y: needs |B| = 70.
    const double t = 70. * M_PI / 180.;
    CHECK_FALSE(solve(m, Vec3(std::sin(t), 0., std::cos(t)), Orientation{}, o));
    // 50 degrees fits.
    const double u = 50. * M_PI / 180.;
    REQUIRE(solve(m, Vec3(std::sin(u), 0., std::cos(u)), Orientation{}, o));
    CHECK(std::abs(std::abs(o.c) - 50.) < 1e-9);
    CHECK(std::abs(o.a) < 1e-9);
}


TEST_CASE("A/B is a tri-Z bed: a pivot in the bed plane and small default tilt limits", "[TrunnionAC]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_nonplanar", "1" },
        { "axis_selection",   "ab" },
    });
    REQUIRE(trunnion_ac_enabled(config));
    const Machine m = trunnion_ac_machine(config);
    CHECK(m.kind == Kind::AB);
    CHECK(m.pivot_height == 0.);         // the pivot sits in the bed plane
    CHECK_FALSE(m.c_continuous);         // B is a tilt, not a turntable
    // Limits left at the +-360 defaults mean "not set": a leadscrew bed tilts a few degrees.
    CHECK(m.a_min == -10.);
    CHECK(m.a_max == 10.);
    CHECK(m.c_min == -10.);
    CHECK(m.c_max == 10.);

    // Limits that were set are used as they are.
    config.set_deserialize_strict({ { "axis_a_rotation_min", "-6" }, { "axis_a_rotation_max", "4" },
                                    { "axis_b_rotation_min", "-3" }, { "axis_b_rotation_max", "3" } });
    const Machine set = trunnion_ac_machine(config);
    CHECK(set.a_min == -6.);
    CHECK(set.a_max == 4.);
    CHECK(set.c_min == -3.);
    CHECK(set.c_max == 3.);

    // A tool axis 12 degrees off vertical does not fit those limits; 2 degrees does.
    Orientation o;
    const double big = 12. * M_PI / 180., small = 2. * M_PI / 180.;
    CHECK_FALSE(solve(set, Vec3(std::sin(big), 0., std::cos(big)), Orientation{}, o));
    CHECK(solve(set, Vec3(std::sin(small), 0., std::cos(small)), Orientation{}, o));
}
