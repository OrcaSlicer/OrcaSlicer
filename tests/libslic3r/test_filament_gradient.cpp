#include <catch2/catch_all.hpp>
#include "libslic3r/FilamentGradient.hpp"

using namespace Slic3r;
using Catch::Approx;   // Catch2 v3 moved Approx into the Catch namespace

static FilamentGradient make_grad(std::vector<GradientColorStop> stops,
                                  float cycle = 100.f, float offset = 0.f)
{
    FilamentGradient g;
    g.stops = std::move(stops);
    g.cycle_length_mm = cycle;
    g.start_offset_mm = offset;
    return g;
}

static const ColorRGBA RED{ 1.f, 0.f, 0.f, 1.f };
static const ColorRGBA GRN{ 0.f, 1.f, 0.f, 1.f };
static const ColorRGBA BLU{ 0.f, 0.f, 1.f, 1.f };

TEST_CASE("FilamentGradient sample - two stops", "[FilamentGradient]")
{
    auto g = make_grad({ {0.f, RED}, {1.f, BLU} }, /*cycle*/100.f);

    SECTION("endpoints and blends") {              // SMP-01..03
        REQUIRE(g.sample(0.f).r()  == Approx(1.f));
        REQUIRE(g.sample(0.f).b()  == Approx(0.f));
        const ColorRGBA mid = g.sample(50.f);
        REQUIRE(mid.r() == Approx(0.5f));
        REQUIRE(mid.b() == Approx(0.5f));
        const ColorRGBA q = g.sample(25.f);
        REQUIRE(q.r() == Approx(0.75f));
        REQUIRE(q.b() == Approx(0.25f));
    }
    SECTION("cycling wraps") {                      // SMP-04..06
        REQUIRE(g.sample(100.f).r() == Approx(1.f));   // fmod -> start
        REQUIRE(g.sample(150.f).r() == Approx(0.5f));  // == sample(50)
        REQUIRE(g.sample(250.f).b() == Approx(0.5f));
    }
    SECTION("start offset shifts") {                // SMP-07..08
        auto go = make_grad({ {0.f, RED}, {1.f, BLU} }, 100.f, /*offset*/50.f);
        REQUIRE(go.sample(0.f).b()  == Approx(0.5f));
        REQUIRE(go.sample(75.f).r() == Approx(0.75f)); // fmod(125,100)=25
    }
    SECTION("negative position guard") {            // SMP-09
        REQUIRE(g.sample(-25.f).b() == Approx(0.75f));
    }
    SECTION("zero cycle length does not divide by zero") { // SMP-10
        auto gz = make_grad({ {0.f, RED}, {1.f, BLU} }, /*cycle*/0.f);
        const ColorRGBA c = gz.sample(50.f);
        REQUIRE(std::isfinite(c.r()));
    }
}

TEST_CASE("FilamentGradient sample - three stops", "[FilamentGradient]")
{
    auto g = make_grad({ {0.f, RED}, {0.5f, GRN}, {1.f, BLU} }, 100.f);
    SECTION("first segment") {                      // SMP-11
        const ColorRGBA c = g.sample(25.f);
        REQUIRE(c.r() == Approx(0.5f));
        REQUIRE(c.g() == Approx(0.5f));
    }
    SECTION("exactly on middle stop") {             // SMP-12
        const ColorRGBA c = g.sample(50.f);
        REQUIRE(c.g() == Approx(1.f));
        REQUIRE(c.r() == Approx(0.f));
    }
    SECTION("second segment") {                     // SMP-13
        const ColorRGBA c = g.sample(75.f);
        REQUIRE(c.g() == Approx(0.5f));
        REQUIRE(c.b() == Approx(0.5f));
    }
}

TEST_CASE("FilamentGradient sample - degenerate", "[FilamentGradient]")
{
    REQUIRE(make_grad({}).sample(10.f).r() == Approx(1.f));          // SMP-14 white
    REQUIRE(make_grad({ {0.f, RED} }).sample(10.f).g() == Approx(1.f)); // SMP-15 white
    REQUIRE(make_grad({}).empty());                                  // EMP-01
    REQUIRE(make_grad({ {0.f, RED} }).empty());                      // EMP-02
    REQUIRE_FALSE(make_grad({ {0.f, RED}, {1.f, BLU} }).empty());    // EMP-03
}

TEST_CASE("FilamentGradient parse and serialize", "[FilamentGradient]")
{
    SECTION("round trip preserves RGB") {           // SER-01
        auto g = make_grad({ {0.f, RED}, {1.f, BLU} });
        std::vector<GradientColorStop> out;
        REQUIRE(FilamentGradient::parse_stops(g.serialize_stops(), out));
        REQUIRE(out.size() == 2);
        REQUIRE(out[0].position == Approx(0.f));
        REQUIRE(out[0].color.r() == Approx(1.f));
        REQUIRE(out[1].position == Approx(1.f));
        REQUIRE(out[1].color.b() == Approx(1.f));
    }
    SECTION("parse sorts by position") {            // PAR-02
        std::vector<GradientColorStop> out;
        REQUIRE(FilamentGradient::parse_stops("1,#0000FF;0,#FF0000", out));
        REQUIRE(out.size() == 2);
        REQUIRE(out.front().position == Approx(0.f));
        REQUIRE(out.front().color.r() == Approx(1.f));
    }
    SECTION("validation") {                         // PAR-03..07
        std::vector<GradientColorStop> out;
        REQUIRE(FilamentGradient::parse_stops("", out));            // empty -> true, 0 stops
        REQUIRE(out.empty());
        REQUIRE_FALSE(FilamentGradient::parse_stops("0,#FF0000", out));     // single stop
        REQUIRE_FALSE(FilamentGradient::parse_stops("abc", out));           // no comma
        REQUIRE_FALSE(FilamentGradient::parse_stops("0,notacolor", out));   // bad color
        REQUIRE_FALSE(FilamentGradient::parse_stops("x,#FF0000;1,#0000FF", out)); // bad pos
    }
    SECTION("accepts 8-digit RRGGBBAA") {           // PAR-08
        std::vector<GradientColorStop> out;
        REQUIRE(FilamentGradient::parse_stops("0,#FF0000FF;1,#0000FFFF", out));
        REQUIRE(out.size() == 2);
    }
}

TEST_CASE("FilamentGradient stops_from_color_list", "[FilamentGradient]")
{
    std::vector<GradientColorStop> out;
    SECTION("even spacing from 3 colors") {         // CLR-01
        REQUIRE(FilamentGradient::stops_from_color_list("#FF0000 #00FF00 #0000FF", out));
        REQUIRE(out.size() == 3);
        REQUIRE(out[0].position == Approx(0.f));
        REQUIRE(out[1].position == Approx(0.5f));
        REQUIRE(out[2].position == Approx(1.f));
    }
    SECTION("comma separator") {                    // CLR-02
        REQUIRE(FilamentGradient::stops_from_color_list("#FF0000,#0000FF", out));
        REQUIRE(out.size() == 2);
    }
    SECTION("rejects single / empty") {             // CLR-03..04
        REQUIRE_FALSE(FilamentGradient::stops_from_color_list("#FF0000", out));
        REQUIRE_FALSE(FilamentGradient::stops_from_color_list("", out));
    }
    SECTION("skips invalid tokens") {               // CLR-05
        REQUIRE(FilamentGradient::stops_from_color_list("#FF0000 garbage #0000FF", out));
        REQUIRE(out.size() == 2);
    }
}
