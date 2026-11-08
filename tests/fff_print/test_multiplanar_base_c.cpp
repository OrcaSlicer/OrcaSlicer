#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/MultiplanarBaseC.hpp"

#include "test_helpers.hpp"

#include <cmath>
#include <limits>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// UNLAYERED_MOD -- multiplanar base C (GCode/MultiplanarBaseC.hpp).
//
// Requirement: "for multiplanar, it should make the first layer have the same C rotation as the first angled
// chunk to keep it more aligned." The planar base of a cut-plane print used to go down at B-90 C0 and
// the table then swung to the first angled chunk's C at the first segment change. Now the table is at
// that C from the first layer of the base, and the first segment change only tilts B. The base's X/Y
// words do not move: the slicer writes bed-frame coordinates and RepRapFirmware's 5-axis compensation
// applies C, for the base exactly as for every angled chunk.

namespace {

constexpr double DEG = PI / 180.;

// Tool axis tilted `tilt_deg` from vertical toward azimuth `azimuth_deg` (tool_axis_to_bc() gives C =
// azimuth for it).
Vec3d tilted(double tilt_deg, double azimuth_deg)
{
    const double t = tilt_deg * DEG, a = azimuth_deg * DEG;
    return Vec3d(std::sin(t) * std::cos(a), std::sin(t) * std::sin(a), std::cos(t));
}

MultiplanarBaseC::Member upright(bool segment = true) { return { Vec3d::UnitZ(), segment }; }
MultiplanarBaseC::Member angled(double tilt_deg, double azimuth_deg, bool segment = true) { return { tilted(tilt_deg, azimuth_deg), segment }; }

Vec2d rot(const Vec2d &v, double a) { return Vec2d(std::cos(a) * v.x() - std::sin(a) * v.y(), std::sin(a) * v.x() + std::cos(a) * v.y()); }

std::vector<Vec2d> square(double x0, double y0, double x1, double y1) { return { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } }; }

} // namespace

TEST_CASE("Multiplanar base C: the first angled chunk is chosen in print order", "[cutplanes][gcode][basec]")
{
    using namespace MultiplanarBaseC;

    SECTION("a two-segment plan: the base takes the angled segment's C") {
        const Choice choice = choose({ { upright() }, { upright() }, { angled(30., 45.) } });
        REQUIRE(choice.c_rad);
        CHECK_THAT(*choice.c_rad / DEG, WithinAbs(45., 1e-9));
        CHECK(choice.layer == 2);
        CHECK(choice.member == 0);
        CHECK_FALSE(choice.ambiguous);
        // Exactly the C the segment's own B/C words will carry.
        double b = 0., c = 0.;
        tool_axis_to_bc(tilted(30., 45.), b, c);
        CHECK(*choice.c_rad == c);
    }
    SECTION("several first-level segments: the one printed first after the base") {
        const Choice choice = choose({ { upright() }, { angled(20., 120.) }, { angled(25., -60.) } });
        REQUIRE(choice.c_rad);
        CHECK_THAT(*choice.c_rad / DEG, WithinAbs(120., 1e-9));
        CHECK_FALSE(choice.ambiguous);
    }
    SECTION("an upright segment before the angled one is still base") {
        const Choice choice = choose({ { upright() }, { upright() }, { angled(10., -90.) } });
        REQUIRE(choice.c_rad);
        CHECK_THAT(*choice.c_rad / DEG, WithinAbs(-90., 1e-9));
    }
    SECTION("two angled chunks starting in the same layer with different C: the first in group order, flagged") {
        const Choice choice = choose({ { upright(), upright() }, { angled(20., 30.), angled(20., -150.) } });
        REQUIRE(choice.c_rad);
        CHECK_THAT(*choice.c_rad / DEG, WithinAbs(30., 1e-9));
        CHECK(choice.ambiguous);
    }
    SECTION("the first angled member is an ordinary object with its own B/C: not a multiplanar base") {
        const Choice choice = choose({ { upright() }, { angled(20., 30., false) }, { angled(20., 60.) } });
        CHECK_FALSE(choice.c_rad);
        CHECK_FALSE(choice.reason.empty());
    }
    SECTION("nothing prints before the first angled chunk") {
        CHECK_FALSE(choose({ { angled(20., 30.) }, { upright() } }).c_rad);
    }
    SECTION("no angled chunk at all") {
        CHECK_FALSE(choose({ { upright() }, { upright() } }).c_rad);
        CHECK_FALSE(choose({}).c_rad);
    }
    SECTION("only an upward vertical axis is base") {
        CHECK(is_vertical(Vec3d::UnitZ()));
        CHECK(is_vertical(Vec3d(0., 0., 3.)));
        CHECK_FALSE(is_vertical(-Vec3d::UnitZ()));
        CHECK_FALSE(is_vertical(tilted(0.01, 10.)));
    }
}

TEST_CASE("Multiplanar base C: the C-axis convention is the one the 5-axis writer already uses", "[cutplanes][gcode][basec]")
{
    using namespace MultiplanarBaseC;
    const Vec2d pivot(100., 100.);
    const Vec2d p(131.5, 62.25);

    SECTION("bed -> machine -> bed is the identity") {
        for (double c_deg : { -170., -45., 0., 30., 90., 179. }) {
            const Vec2d back = machine_to_bed_xy(bed_to_machine_xy(p, pivot, c_deg * DEG), pivot, c_deg * DEG);
            CHECK((back - p).norm() < 1e-9);
        }
    }
    SECTION("holding machine X/Y while C moves by dc turns the bed point by +dc about the pivot (chunk_transition_gcode move 2)") {
        const double c_old = 20. * DEG, c_new = 65. * DEG;
        const Vec2d  m     = bed_to_machine_xy(p, pivot, c_old);
        const Vec2d  after = machine_to_bed_xy(m, pivot, c_new);
        // GCode::chunk_transition_gcode(): rotated_xy = pivot + Rz(c_new - c_old) * (hopped - pivot).
        CHECK((after - (pivot + rot(p - pivot, c_new - c_old))).norm() < 1e-9);
    }
    SECTION("machine +Y in the bed frame is Rz(C) * Y, as tool_head_frame() has it") {
        const double c     = 37. * DEG;
        const Vec2d  y_bed = machine_to_bed_xy(pivot + Vec2d(0., 1.), pivot, c) - pivot;
        const Matrix3d head = tool_head_frame(tilted(30., 37.));
        CHECK((y_bed - Vec2d(head(1, 0), head(1, 1))).norm() < 1e-9);
    }
    SECTION("the base's unchanged G-code X/Y map back to the same part geometry at the new C") {
        // What the G-code says for the base (bed frame) is what the machine drives through the C axis,
        // and what lands on the part is that drive mapped back through the same C: the original points.
        const double c = 45. * DEG;
        for (const Vec2d &q : square(120., 60., 140., 80.)) {
            const Vec2d machine = bed_to_machine_xy(q, pivot, c);
            CHECK((machine - q).norm() > 1.);   // the head really goes somewhere else ...
            CHECK((machine_to_bed_xy(machine, pivot, c) - q).norm() < 1e-9);   // ... and prints q on the part
        }
        // Turning the toolpaths in the slicer as well would print them at the turned bed point: the
        // base would be rotated by C against the chunks that rest on it.
        const Vec2d pre_turned = bed_to_machine_xy(p, pivot, c);
        CHECK((pre_turned - p).norm() > 1.);
    }
}

TEST_CASE("Multiplanar base C: the turned base has to stay on the printable area", "[cutplanes][gcode][basec]")
{
    using namespace MultiplanarBaseC;
    const std::vector<Vec2d> bed   = square(0., 0., 200., 200.);
    const Vec2d              pivot = BoundingBoxf(bed).center();

    SECTION("a base near the bed centre fits at any C") {
        for (double c_deg : { 0., 45., 90., 135., -100. })
            CHECK(fits_at_c(square(120., 60., 140., 80.), bed, pivot, c_deg * DEG));
    }
    SECTION("a base in a corner of a square bed does not fit turned 45 degrees") {
        const std::vector<Vec2d> corner = square(165., 165., 185., 185.);
        CHECK(fits_at_c(corner, bed, pivot, 0.));
        CHECK(fits_at_c(corner, bed, pivot, 90. * DEG));   // a square bed maps onto itself
        Vec2d worst = Vec2d::Zero();
        CHECK_FALSE(fits_at_c(corner, bed, pivot, 45. * DEG, 0.1, &worst));
        CHECK(worst.x() > 200.);
    }
    SECTION("a round bed: the polygon's own corners do not fail a turned base") {
        std::vector<Vec2d> round;
        for (int i = 0; i < 72; ++ i)
            round.emplace_back(95. * std::cos(i * 5. * DEG), 95. * std::sin(i * 5. * DEG));
        const Vec2d centre = BoundingBoxf(round).center();
        // On a vertex, turned to the middle of an edge: 0.09 mm "outside" the 72-gon.
        CHECK(fits_at_c({ Vec2d(95., 0.) }, round, centre, 2.5 * DEG));
        CHECK_FALSE(fits_at_c({ Vec2d(96., 0.) }, round, centre, 2.5 * DEG));
    }
    SECTION("nothing to check") {
        CHECK(fits_at_c({}, bed, pivot, 1.));
        CHECK(fits_at_c({ Vec2d(1000., 1000.) }, {}, pivot, 1.));
    }
}

namespace {

struct BaseCSlice
{
    std::string   gcode;
    BoundingBoxf3 part;   // the cube on the bed, world frame
};

// A 20 mm cube cut through its centre by one plane tilted `tilt_deg` toward `azimuth_deg`: a root
// (segment 0, built straight up) and one angled segment (1) whose C is the azimuth.
BaseCSlice slice_two_segment_cube(const Vec2d &offset, double tilt_deg, double azimuth_deg, bool comments)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "base_c_cube";
    object->add_volume(Test::cube(20.), ModelVolumeType::MODEL_PART, false);
    ModelInstance *instance = object->add_instance();
    instance->set_offset(Vec3d(offset.x(), offset.y(), 0.));

    CutPlaneStack stack;
    stack.enabled = true;
    CutPlane cp;
    cp.point  = Vec3d(10., 10., 10.);
    cp.normal = tilted(tilt_deg, azimuth_deg);
    stack.planes.push_back(cp);
    object->cut_planes  = stack;
    object->auto_drop   = true;
    object->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_nonplanar",         "1" },
        { "axis_selection",           "bc" },
        { "print_sequence",           "by group" },
        { "layer_change_gcode",       "G92 E0" },
        { "use_relative_e_distances", "1" },
        { "enable_arc_fitting",       "0" },
        { "gcode_comments",           comments ? "1" : "0" },
    });
    Slic3r::Print print;
    print.apply(model, config);
    print.process();
    return { Test::gcode(print), object->instance_bounding_box(0) };
}

struct Extrusion { size_t line; int segment; double b, c, x, y; };
struct CWord     { size_t line; double c; };
struct Scan      { std::vector<Extrusion> extrusions; std::vector<CWord> c_words; };

// Modal B/C/X/Y through the file; every extruding move with the segment open at the time.
Scan scan(const std::string &gcode)
{
    Scan               r;
    std::istringstream in(gcode);
    std::string        line;
    size_t             n = 0;
    int                segment = -1;
    double             b = -90., c = 0., x = 0., y = 0.;
    const std::regex   word(R"(([XYEBC])(-?\d*\.?\d+))");
    while (std::getline(in, line)) {
        ++ n;
        if (line.rfind(";SEGMENT_ID:", 0) == 0) {
            segment = std::atoi(line.c_str() + 12);
            continue;
        }
        const std::string code = line.substr(0, line.find(';'));
        if (code.size() < 3 || code[0] != 'G' || code[1] < '0' || code[1] > '3' || code[2] != ' ')
            continue;   // G0..G3 only (not G10, G28, G92 ...)
        bool   has_xy = false;
        double e      = 0.;
        for (std::sregex_iterator it(code.begin(), code.end(), word), end; it != end; ++ it) {
            const char   axis = (*it)[1].str()[0];
            const double v    = std::stod((*it)[2].str());
            switch (axis) {
            case 'X': x = v; has_xy = true; break;
            case 'Y': y = v; has_xy = true; break;
            case 'E': e = v; break;
            case 'B': b = v; break;
            case 'C': c = v; r.c_words.push_back({ n, v }); break;
            default: break;
            }
        }
        if (e > 0. && has_xy)
            r.extrusions.push_back({ n, segment, b, c, x, y });
    }
    return r;
}

} // namespace

TEST_CASE("Multiplanar base C: the base prints at the first angled chunk's C", "[cutplanes][gcode][basec]")
{
    // Off the bed centre on purpose: turned toolpaths would show up as a displaced base.
    const BaseCSlice slice = slice_two_segment_cube(Vec2d(120., 60.), 20., 45., false);
    const Scan       s     = scan(slice.gcode);
    REQUIRE(! s.extrusions.empty());

    const auto first_angled = std::find_if(s.extrusions.begin(), s.extrusions.end(), [](const Extrusion &e) { return e.segment >= 1; });
    REQUIRE(first_angled != s.extrusions.end());
    REQUIRE(first_angled != s.extrusions.begin());   // a base was printed before it
    const double c_angled = first_angled->c;
    CHECK_THAT(c_angled, WithinAbs(45., 0.01));

    // The first layer -- and every other layer of the base, skirt and brim included -- has the angled
    // chunk's C, with the head upright.
    BoundingBoxf base_xy;
    for (auto it = s.extrusions.begin(); it != first_angled; ++ it) {
        INFO("G-code line " << it->line);
        CHECK_THAT(it->c, WithinAbs(c_angled, 0.002));
        CHECK_THAT(it->b, WithinAbs(-90., 0.002));
        base_xy.merge(Vec2d(it->x, it->y));
    }
    // The table was turned before the first base extrusion, not at the segment change.
    const size_t first_base_line = s.extrusions.front().line;
    CHECK(std::any_of(s.c_words.begin(), s.c_words.end(), [&](const CWord &w) {
        return w.line < first_base_line && std::abs(w.c - c_angled) < 0.002;
    }));
    // No C swing at the segment change: every C word between the base and the angled chunk is its C.
    const size_t last_base_line = std::prev(first_angled)->line;
    for (const CWord &w : s.c_words)
        if (w.line > last_base_line && w.line < first_angled->line) {
            INFO("C word on G-code line " << w.line);
            CHECK_THAT(w.c, WithinAbs(c_angled, 0.002));
        }
    // The base's X/Y are the part's own footprint on the bed: not turned with C.
    const Vec2d part_centre(0.5 * (slice.part.min.x() + slice.part.max.x()), 0.5 * (slice.part.min.y() + slice.part.max.y()));
    CHECK((base_xy.center() - part_centre).norm() < 1.5);
    CHECK(base_xy.min.x() > slice.part.min.x() - 12.);
    CHECK(base_xy.max.x() < slice.part.max.x() + 12.);
    CHECK(base_xy.min.y() > slice.part.min.y() - 12.);
    CHECK(base_xy.max.y() < slice.part.max.y() + 12.);
}

TEST_CASE("Multiplanar base C: a base that would leave the bed when turned keeps C0", "[cutplanes][gcode][basec]")
{
    // In the corner of the default 200 x 200 bed: at C45 its far corner would sit ~20 mm past the edge.
    const BaseCSlice slice = slice_two_segment_cube(Vec2d(165., 165.), 20., 45., true);
    const Scan       s     = scan(slice.gcode);
    REQUIRE(! s.extrusions.empty());

    const auto first_angled = std::find_if(s.extrusions.begin(), s.extrusions.end(), [](const Extrusion &e) { return e.segment >= 1; });
    REQUIRE(first_angled != s.extrusions.end());
    REQUIRE(first_angled != s.extrusions.begin());
    CHECK_THAT(first_angled->c, WithinAbs(45., 0.01));
    for (auto it = s.extrusions.begin(); it != first_angled; ++ it) {
        INFO("G-code line " << it->line);
        CHECK_THAT(it->c, WithinAbs(0., 0.002));
    }
    // Today's behaviour: the swing to C45 happens at the segment change.
    const size_t last_base_line = std::prev(first_angled)->line;
    CHECK(std::any_of(s.c_words.begin(), s.c_words.end(), [&](const CWord &w) {
        return w.line > last_base_line && w.line < first_angled->line && std::abs(w.c - 45.) < 0.01;
    }));
    // ... and the file says why.
    CHECK(slice.gcode.find("; multiplanar base: C kept at 0") != std::string::npos);
}

TEST_CASE("Multiplanar base C: a first angled chunk at C0 leaves the file as it was", "[cutplanes][gcode][basec]")
{
    const BaseCSlice slice = slice_two_segment_cube(Vec2d(120., 60.), 20., 0., false);
    const Scan       s     = scan(slice.gcode);
    REQUIRE(! s.extrusions.empty());
    // No rotary word at all before the first extrusion: no extra base rotation was written.
    for (const CWord &w : s.c_words)
        CHECK(w.line > s.extrusions.front().line);
    for (const Extrusion &e : s.extrusions)
        CHECK_THAT(e.c, WithinAbs(0., 0.002));
}
