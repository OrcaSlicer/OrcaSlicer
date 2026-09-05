#include <catch2/catch_all.hpp>

#include "libslic3r/Color.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentMatch.hpp"
#include "libslic3r/MixedFilamentPicPrint.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

MixedFilament pair_mix(unsigned a, unsigned b, int ra = 1, int rb = 1)
{
    MixedFilament mf;
    mf.component_a = a;
    mf.component_b = b;
    mf.ratio_a     = ra;
    mf.ratio_b     = rb;
    return mf;
}

MixedFilament triple_mix(unsigned a, unsigned b, unsigned c, int ra = 1, int rb = 1, int rc = 1)
{
    MixedFilament mf;
    mf.component_a = a;
    mf.component_b = b;
    mf.component_c = c;
    mf.ratio_a     = ra;
    mf.ratio_b     = rb;
    mf.ratio_c     = rc;
    return mf;
}

MixedFilament pattern_mix(unsigned a, unsigned b, const std::string &pattern)
{
    MixedFilament mf;
    mf.component_a     = a;
    mf.component_b     = b;
    mf.manual_pattern  = pattern;
    return mf;
}

ColorRGB decode_hex_or_fail(const std::string &hex)
{
    ColorRGB c;
    REQUIRE(decode_color(hex, c));
    return c;
}

} // namespace

TEST_CASE("filament_opaque default false and round-trip", "[spectrum][opaque]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    const auto *opt = cfg.option<ConfigOptionBools>("filament_opaque");
    REQUIRE(opt != nullptr);
    REQUIRE_FALSE(opt->values.empty());
    REQUIRE(opt->get_at(0) == false);
    // Default emission matches soluble (both coBools false): all-false golden vs sibling key.
    REQUIRE(opt->serialize() == cfg.option("filament_soluble")->serialize());

    cfg.set_deserialize_strict("filament_opaque", "1");
    REQUIRE(cfg.option<ConfigOptionBools>("filament_opaque")->get_at(0) == true);
    const std::string ser = cfg.option("filament_opaque")->serialize();
    DynamicPrintConfig round = DynamicPrintConfig::full_print_config();
    round.set_deserialize_strict("filament_opaque", ser);
    REQUIRE(round.option<ConfigOptionBools>("filament_opaque")->get_at(0) == true);

    // Absent key (old 3mf / profile) behaves as false. Existing config tests cover 3mf
    // emission of defined keys; default false means no new warn-spam on load.
    DynamicPrintConfig empty;
    REQUIRE(empty.option("filament_opaque") == nullptr);
    const std::vector<bool> no_flags;
    REQUIRE_FALSE(spectrum_has_opaque_blend(no_flags, pair_mix(1, 2)));
}

TEST_CASE("spectrum_has_opaque_blend truth table", "[spectrum][opaque]")
{
    const std::vector<bool> none{false, false, false, false};
    const std::vector<bool> k_opaque{false, false, false, true};
    const std::vector<bool> a_opaque{true, false, false, false};
    const std::vector<bool> four = k_opaque;

    SECTION("all translucent pair is false")
    {
        REQUIRE_FALSE(spectrum_has_opaque_blend(none, pair_mix(1, 2)));
        REQUIRE_FALSE(spectrum_has_opaque_blend(none, "1,2,1,1,1"));
    }

    SECTION("period-2 pair with one opaque is true")
    {
        REQUIRE(spectrum_has_opaque_blend(k_opaque, pair_mix(1, 4)));
        REQUIRE(spectrum_has_opaque_blend(k_opaque, "1,4,1,1,1"));
        REQUIRE(spectrum_has_opaque_blend(a_opaque, pair_mix(1, 2, 1, 1)));
    }

    SECTION("period-3 2:1 with one opaque is true")
    {
        REQUIRE(spectrum_has_opaque_blend(a_opaque, pair_mix(1, 2, 2, 1)));
        REQUIRE(spectrum_has_opaque_blend(k_opaque, triple_mix(1, 2, 4)));
    }

    SECTION("period-1 same-slot 1:1 is false")
    {
        REQUIRE_FALSE(spectrum_has_opaque_blend(a_opaque, pair_mix(1, 1)));
        REQUIRE_FALSE(spectrum_has_opaque_blend(a_opaque, "1,1,1,1,1"));
    }

    SECTION("pattern 1111 is false")
    {
        REQUIRE_FALSE(spectrum_has_opaque_blend(a_opaque, pattern_mix(1, 2, "1111")));
        REQUIRE_FALSE(spectrum_has_opaque_blend(a_opaque, "1111"));
        REQUIRE_FALSE(spectrum_has_opaque_blend(a_opaque, "1,2,1,1,1,1111"));
    }

    SECTION("OOB component_a=5 with flags size 4 does not crash and is false")
    {
        MixedFilament oob = pair_mix(5, 1);
        REQUIRE_FALSE(spectrum_has_opaque_blend(four, oob));
        MixedFilament both_oob = pair_mix(5, 6);
        REQUIRE_FALSE(spectrum_has_opaque_blend(four, both_oob));
    }

    SECTION("OOB second slot ignored; remaining one slot is false")
    {
        REQUIRE_FALSE(spectrum_has_opaque_blend(a_opaque, pair_mix(1, 5)));
    }

    SECTION("empty flags is false")
    {
        REQUIRE_FALSE(spectrum_has_opaque_blend(std::vector<bool>{}, pair_mix(1, 2)));
    }

    SECTION("only-translucent triple is false")
    {
        REQUIRE_FALSE(spectrum_has_opaque_blend(none, triple_mix(1, 2, 3)));
    }
}

TEST_CASE("opaque blend marker presence by period", "[spectrum][opaque]")
{
    const std::vector<bool> flags{true, false, false, false};
    const std::vector<bool> none{false, false, false, false};

    SECTION("period 1 pattern and same-slot have no marker")
    {
        REQUIRE(spectrum_opaque_blend_marker(flags, pattern_mix(1, 2, "1")).empty());
        REQUIRE(spectrum_opaque_blend_marker(flags, pair_mix(1, 1)).empty());
        REQUIRE(spectrum_with_opaque_blend_marker("1+1 1:1", flags, pair_mix(1, 1)) == "1+1 1:1");
    }

    SECTION("period 2 appends ASCII stripes - no blend")
    {
        const MixedFilament mf = pair_mix(1, 2);
        REQUIRE(spectrum_opaque_blend_marker(flags, mf) == "stripes - no blend");
        const std::string labeled = spectrum_with_opaque_blend_marker("1+2 1:1", flags, mf);
        REQUIRE(labeled.find("stripes - no blend") != std::string::npos);
        REQUIRE(labeled.find("1+2 1:1") != std::string::npos);
    }

    SECTION("period 3 appends ASCII stripes - no blend")
    {
        const MixedFilament p3 = pair_mix(1, 2, 2, 1);
        REQUIRE(spectrum_opaque_blend_marker(flags, p3) == "stripes - no blend");
        REQUIRE(spectrum_with_opaque_blend_marker("1+2 2:1", flags, p3).find("stripes - no blend") !=
                std::string::npos);
        const MixedFilament triple = triple_mix(1, 2, 3);
        REQUIRE(spectrum_opaque_blend_marker(flags, triple) == "stripes - no blend");
    }

    SECTION("no flags never marks")
    {
        REQUIRE(spectrum_opaque_blend_marker(none, pair_mix(1, 2)).empty());
        REQUIRE(spectrum_with_opaque_blend_marker("1+2 1:1", none, pair_mix(1, 2)) == "1+2 1:1");
    }

    SECTION("lut loaded may say measured stripes")
    {
        const std::string marker = spectrum_opaque_blend_marker(flags, pair_mix(1, 2), true);
        REQUIRE(marker.find("stripes - no blend") != std::string::npos);
        REQUIRE(marker.find("measured") != std::string::npos);
    }
}

TEST_CASE("PicPrint cluster mix rows append stripe marker", "[spectrum][opaque]")
{
    // Simulated PicPrint cluster list: C+K 1:1 plus a translucent pair.
    MixedFilamentManager mgr;
    mgr.load_definitions("1,4,1,1,1;1,2,1,1,1");
    REQUIRE(mgr.mixed_filaments().size() == 2);
    const std::vector<bool> flags{false, false, false, true};
    const MixedFilament    &ck = mgr.mixed_filaments()[0];
    const MixedFilament    &cm = mgr.mixed_filaments()[1];
    REQUIRE(spectrum_has_opaque_blend(flags, ck));
    REQUIRE_FALSE(spectrum_has_opaque_blend(flags, cm));
    REQUIRE(spectrum_with_opaque_blend_marker("1+4 1:1", flags, ck).find("stripes - no blend") !=
            std::string::npos);
    REQUIRE(spectrum_with_opaque_blend_marker("1+2 1:1", flags, cm) == "1+2 1:1");
    REQUIRE(spectrum_has_opaque_blend(flags, mgr.serialize_definitions()));
}

TEST_CASE("ColorMix predicted swatch is independent of filament_opaque", "[spectrum][opaque]")
{
    const std::vector<ColorRGB> physicals{
        decode_hex_or_fail("#08ABFB"),
        decode_hex_or_fail("#D93B90"),
        decode_hex_or_fail("#F9ED3D"),
        decode_hex_or_fail("#9199A4"),
    };
    const MixedFilament mf = pair_mix(1, 2);
    const ColorRGB      predicted = predicted_swatch_for_mix(mf, physicals);
    REQUIRE((predicted.r_uchar() != 0 || predicted.g_uchar() != 0 || predicted.b_uchar() != 0));
    // Opaque flag is not an argument to predicted_swatch_for_mix; ranking helpers stay uncoupled.
}

TEST_CASE("PicPrint planner does not consume filament_opaque", "[spectrum][opaque]")
{
    std::vector<std::uint8_t> rgb(size_t(8) * 2 * 3, 0);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 8; ++x) {
            const size_t i = (size_t(y) * 8 + size_t(x)) * 3;
            if (x < 4) {
                rgb[i]     = 0x08;
                rgb[i + 1] = 0xAB;
                rgb[i + 2] = 0xFB;
            } else {
                rgb[i]     = 0xD9;
                rgb[i + 1] = 0x3B;
                rgb[i + 2] = 0x90;
            }
        }
    }
    const std::vector<ColorRGB> phys{
        decode_hex_or_fail("#08ABFB"),
        decode_hex_or_fail("#D93B90"),
        decode_hex_or_fail("#F9ED3D"),
        decode_hex_or_fail("#9199A4"),
    };
    const SpectrumPicPrintPlan plan = plan_spectrum_picprint(rgb.data(), 8, 2, phys, 4);
    REQUIRE(plan.valid);
    // Planner API has no opaque_flags argument; dest IDs are independent of the guidance flag.
    REQUIRE(plan.dest_id.size() == size_t(16));
}
