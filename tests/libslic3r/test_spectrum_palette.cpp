#include <catch2/catch_all.hpp>

#include "libslic3r/Color.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentCookbook.hpp"
#include "libslic3r/MixedFilamentMatch.hpp"

#include <string>
#include <vector>

using namespace Slic3r;

namespace {

ColorRGB decode_hex_or_fail(const std::string &hex)
{
    ColorRGB c;
    REQUIRE(decode_color(hex, c));
    return c;
}

std::vector<ColorRGB> panchroma_physicals()
{
    return {
        decode_hex_or_fail("#08ABFB"),
        decode_hex_or_fail("#D93B90"),
        decode_hex_or_fail("#F9ED3D"),
        decode_hex_or_fail("#9199A4"),
    };
}

MixedFilament pair_11_ab()
{
    MixedFilament mf;
    mf.component_a = 1;
    mf.component_b = 2;
    mf.component_c = 0;
    mf.ratio_a     = 1;
    mf.ratio_b     = 1;
    mf.ratio_c     = 0;
    mf.enabled     = true;
    return mf;
}

MixedFilament pattern_1234_mix()
{
    MixedFilament mf = pair_11_ab();
    mf.manual_pattern = "1234";
    return mf;
}

} // namespace

TEST_CASE("spectrum_palette_try_add appends on empty", "[spectrum][spectrum_palette]")
{
    std::vector<MixedFilament> rows;
    const SpectrumPaletteAddResult r = spectrum_palette_try_add(rows, pair_11_ab());
    REQUIRE(r.outcome == SpectrumPaletteAddOutcome::Append);
    REQUIRE(r.index == 0);
    REQUIRE(rows.size() == 1);
}

TEST_CASE("spectrum_palette_try_add selects serialize duplicate", "[spectrum][spectrum_palette]")
{
    std::vector<MixedFilament> rows;
    const MixedFilament        pair = pair_11_ab();
    REQUIRE(spectrum_palette_try_add(rows, pair).outcome == SpectrumPaletteAddOutcome::Append);

    const SpectrumPaletteAddResult again = spectrum_palette_try_add(rows, pair);
    REQUIRE(again.outcome == SpectrumPaletteAddOutcome::SelectExisting);
    REQUIRE(again.index == 0);
    REQUIRE(rows.size() == 1);
}

TEST_CASE("spectrum_palette_try_add treats 1234 cycle as distinct from 1:1 pair", "[spectrum][spectrum_palette]")
{
    const MixedFilament pair  = pair_11_ab();
    const MixedFilament cycle = pattern_1234_mix();
    REQUIRE(serialize_mix_recipe(pair) != serialize_mix_recipe(cycle));
    REQUIRE(spectrum_cookbook_same_recipe(pair, cycle));

    std::vector<MixedFilament> rows;
    const SpectrumPaletteAddResult r_pair = spectrum_palette_try_add(rows, pair);
    REQUIRE(r_pair.outcome == SpectrumPaletteAddOutcome::Append);
    REQUIRE(r_pair.index == 0);

    const SpectrumPaletteAddResult r_cycle = spectrum_palette_try_add(rows, cycle);
    REQUIRE(r_cycle.outcome == SpectrumPaletteAddOutcome::Append);
    REQUIRE(r_cycle.index == 1);
    REQUIRE(rows.size() == 2);
}

TEST_CASE("spectrum_palette_try_add refuses at enabled cap", "[spectrum][spectrum_palette]")
{
    std::vector<MixedFilament> rows;
    rows.reserve(SPECTRUM_MIX_ENABLED_CAP);
    for (size_t i = 0; i < SPECTRUM_MIX_ENABLED_CAP; ++i) {
        MixedFilament mf = pair_11_ab();
        mf.ratio_b       = int(i) + 1;
        const SpectrumPaletteAddResult r = spectrum_palette_try_add(rows, mf);
        REQUIRE(r.outcome == SpectrumPaletteAddOutcome::Append);
        REQUIRE(r.index == i);
    }
    REQUIRE(rows.size() == SPECTRUM_MIX_ENABLED_CAP);

    MixedFilament extra = pair_11_ab();
    extra.component_b   = 3;
    const size_t                   size_before = rows.size();
    const SpectrumPaletteAddResult refused     = spectrum_palette_try_add(rows, extra);
    REQUIRE(refused.outcome == SpectrumPaletteAddOutcome::CapRefuse);
    REQUIRE(rows.size() == size_before);
}

TEST_CASE("spectrum_swatch_lattice n=4 is still 51", "[spectrum][spectrum_palette]")
{
    REQUIRE(spectrum_swatch_lattice(panchroma_physicals()).size() == 51);
}
