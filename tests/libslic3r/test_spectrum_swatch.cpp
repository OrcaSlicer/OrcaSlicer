#include <catch2/catch_all.hpp>

#include "libslic3r/Color.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentMatch.hpp"
#include "libslic3r/MixedFilamentPicPrint.hpp"
#include "libslic3r/MixedFilamentSwatch.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/prusa_fdm_mixer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

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

std::vector<ColorRGB> first_n(const std::vector<ColorRGB> &src, size_t n)
{
    return std::vector<ColorRGB>(src.begin(), src.begin() + int(n));
}

bool ranking_byte_identical(const std::vector<MixMatchResult> &a, const std::vector<MixMatchResult> &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].valid != b[i].valid || a[i].kind != b[i].kind || a[i].distance != b[i].distance)
            return false;
        if (a[i].predicted != b[i].predicted)
            return false;
        if (a[i].kind == MixMatchResult::Kind::Physical) {
            if (a[i].physical_id != b[i].physical_id)
                return false;
        } else if (a[i].recipe_row != b[i].recipe_row) {
            return false;
        }
    }
    return true;
}

boost::filesystem::path unique_temp_dir()
{
    const auto dir = boost::filesystem::temp_directory_path() /
                     ("spectrum_swatch_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    boost::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("spectrum_swatch_lattice size is stable for n=2..4", "[spectrum_swatch]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const auto n2 = spectrum_swatch_lattice(first_n(phys, 2));
    const auto n3 = spectrum_swatch_lattice(first_n(phys, 3));
    const auto n4 = spectrum_swatch_lattice(phys);

    REQUIRE(n2.size() == 7);
    REQUIRE(n3.size() == 22);
    REQUIRE(n4.size() == 51);

    REQUIRE(spectrum_swatch_lattice(first_n(phys, 2)).size() == n2.size());
    REQUIRE(spectrum_swatch_lattice(phys).size() == n4.size());

    auto unique_mix_rows = [](const std::vector<MixMatchResult> &lat) {
        std::set<std::string> rows;
        size_t                mixes = 0;
        for (const MixMatchResult &r : lat) {
            if (r.kind != MixMatchResult::Kind::Mix)
                continue;
            ++mixes;
            REQUIRE_FALSE(r.recipe_row.empty());
            REQUIRE(rows.insert(r.recipe_row).second);
        }
        return mixes;
    };
    REQUIRE(unique_mix_rows(n2) == n2.size() - 2);
    REQUIRE(unique_mix_rows(n3) == n3.size() - 3);
    REQUIRE(unique_mix_rows(n4) == n4.size() - 4);
}

TEST_CASE("build_swatch_plate volumes match lattice and Mix IDs stay in mix-row cap", "[spectrum_swatch]")
{
    const std::vector<ColorRGB>       phys    = panchroma_physicals();
    const std::vector<MixMatchResult> lattice = spectrum_swatch_lattice(phys);
    REQUIRE(lattice.size() == 51);

    Model            model;
    SwatchPlateBuild built = build_swatch_plate(model, lattice, 4, "batch", 270.0);
    REQUIRE(built.valid);
    REQUIRE(built.object != nullptr);
    REQUIRE(built.object->volumes.size() == lattice.size());
    REQUIRE(built.manifest.pads.size() == lattice.size());
    REQUIRE(model.objects.size() == 1);

    size_t mix_count = 0;
    int    max_id    = 0;
    for (size_t i = 0; i < built.object->volumes.size(); ++i) {
        const ModelVolume *vol = built.object->volumes[i];
        REQUIRE(vol != nullptr);
        const int id = vol->extruder_id();
        REQUIRE(id >= 1);
        max_id = std::max(max_id, id);
        if (lattice[i].kind == MixMatchResult::Kind::Physical)
            REQUIRE(id == int(lattice[i].physical_id));
        else {
            ++mix_count;
            REQUIRE(id > 4);
            REQUIRE(id - 4 <= int(SPECTRUM_MIX_ENABLED_CAP));
        }
        REQUIRE(built.manifest.pads[i].recipe == spectrum_swatch_recipe_key(lattice[i]));
        REQUIRE(built.manifest.pads[i].extruder == id);
    }
    REQUIRE(mix_count + 4 == lattice.size());
    REQUIRE(max_id <= 4 + int(SPECTRUM_MIX_ENABLED_CAP));

    MixedFilamentManager mgr;
    mgr.load_definitions(built.mixed_filament_definitions);
    REQUIRE(mgr.enabled_count() == mix_count);

    const BoundingBoxf3 bb = built.object->raw_mesh_bounding_box();
    REQUIRE(bb.size().x() <= 270.0 + 1e-3);
    REQUIRE(bb.size().y() <= 270.0 + 1e-3);
}

TEST_CASE("swatch LUT JSON round-trip and refuse malformed / unknown recipe", "[spectrum_swatch]")
{
    const auto                    lattice = spectrum_swatch_lattice(panchroma_physicals());
    const std::vector<std::string> allowed = spectrum_swatch_recipe_keys(lattice);

    SwatchLUT lut;
    lut.version         = 1;
    lut.mode            = "translucent";
    lut.layer_height_mm = 0.08;
    lut.batch_key       = "abc123";
    lut.entries.push_back({"P1", 50.1, 2.2, -4.4});
    lut.entries.push_back({allowed.back(), 40.0, 1.0, 0.5});

    const std::string json = serialize_swatch_lut(lut);
    SwatchLUT         round;
    std::string       err;
    REQUIRE(parse_swatch_lut_json(json, allowed, round, err));
    REQUIRE(err.empty());
    REQUIRE(round.version == 1);
    REQUIRE(round.mode == "translucent");
    REQUIRE(round.layer_height_mm == Catch::Approx(0.08));
    REQUIRE(round.batch_key == "abc123");
    REQUIRE(round.entries.size() == 2);
    REQUIRE(round.entries[0].recipe == "P1");
    REQUIRE(round.find_recipe("P1") != nullptr);

    SECTION("malformed JSON refused") {
        SwatchLUT   bad;
        std::string e;
        REQUIRE_FALSE(parse_swatch_lut_json("{", allowed, bad, e));
        REQUIRE_FALSE(e.empty());
        REQUIRE_FALSE(parse_swatch_lut_json("{\"version\":2,\"entries\":[]}", allowed, bad, e));
        REQUIRE_FALSE(parse_swatch_lut_json("{\"version\":1}", allowed, bad, e));
    }

    SECTION("unknown recipe refused, no silent drop") {
        SwatchLUT   bad;
        std::string e;
        const std::string unknown =
            "{\"version\":1,\"mode\":\"translucent\",\"layer_height_mm\":0.08,\"batch_key\":\"x\","
            "\"entries\":[{\"recipe\":\"P9\",\"L\":1,\"a\":2,\"b\":3}]}";
        REQUIRE_FALSE(parse_swatch_lut_json(unknown, allowed, bad, e));
        REQUIRE(e.find("Unknown recipe") != std::string::npos);
        REQUIRE(bad.entries.empty());
    }

    SECTION("CSV import") {
        const std::string csv = "recipe,L,a,b\nP1,50.1,2.2,-4.4\n";
        SwatchLUT         from_csv;
        std::string       e;
        REQUIRE(parse_swatch_lut_csv(csv, allowed, from_csv, e));
        REQUIRE(from_csv.entries.size() == 1);
        REQUIRE(from_csv.entries[0].recipe == "P1");
        REQUIRE_FALSE(parse_swatch_lut_csv("recipe,L,a,b\nP9,1,2,3\n", allowed, from_csv, e));
    }
}

TEST_CASE("spectrum_compute_batch_key invalidates on null and partial TD", "[spectrum_swatch]")
{
    const std::vector<std::string> hexes{"#08ABFB", "#D93B90", "#F9ED3D", "#9199A4"};
    const std::vector<std::string> names{"C", "M", "Y", "K"};
    const std::string              k_null = spectrum_compute_batch_key(hexes, names, nullptr);
    const std::vector<float>       empty;
    REQUIRE(spectrum_compute_batch_key(hexes, names, &empty) == k_null);

    const std::vector<float> partial{8.f};
    const std::vector<float> more{8.f, 7.8f};
    const std::vector<float> full{8.f, 7.8f, 14.f, 11.5f};
    REQUIRE(spectrum_compute_batch_key(hexes, names, &partial) != k_null);
    REQUIRE(spectrum_compute_batch_key(hexes, names, &partial) !=
            spectrum_compute_batch_key(hexes, names, &more));
    REQUIRE(spectrum_compute_batch_key(hexes, names, &full) != k_null);
    REQUIRE(spectrum_compute_batch_key(hexes, names, nullptr) == k_null);

    const std::vector<std::string> renamed{"Cyan", "M", "Y", "K"};
    REQUIRE(spectrum_compute_batch_key(hexes, renamed, nullptr) != k_null);

    REQUIRE(spectrum_compute_batch_key(panchroma_physicals(), names, nullptr) ==
            spectrum_compute_batch_key(panchroma_physicals(), names, nullptr));
}

TEST_CASE("swatch LUT persist round-trip by batch_key", "[spectrum_swatch]")
{
    const auto                     lattice = spectrum_swatch_lattice(panchroma_physicals());
    const std::vector<std::string> allowed = spectrum_swatch_recipe_keys(lattice);
    const auto                     dir     = unique_temp_dir();

    SwatchLUT lut;
    lut.batch_key = "persistkey";
    lut.entries.push_back({"P1", 12.0, 3.0, -1.0});
    std::string err;
    REQUIRE(save_swatch_lut(lut, err, dir.string()));
    SwatchLUT loaded;
    REQUIRE(load_swatch_lut("persistkey", loaded, err, dir.string()));
    REQUIRE(loaded.entries.size() == 1);
    REQUIRE(loaded.entries[0].recipe == "P1");

    SwatchManifest man;
    man.batch_key = "persistkey";
    man.pads.push_back({0, 0, 0, "P1", 1});
    REQUIRE(save_swatch_manifest(man, err, dir.string()));
    SwatchManifest man2;
    REQUIRE(load_swatch_manifest("persistkey", man2, err, dir.string()));
    REQUIRE(man2.pads.size() == 1);
    boost::filesystem::remove_all(dir);
}

TEST_CASE("synthetic LUT changes rank vs nullptr", "[spectrum_swatch]")
{
    const std::vector<ColorRGB> phys   = panchroma_physicals();
    const ColorRGB              target = decode_hex_or_fail("#08ABFB");
    const auto                  none   = match_printable_candidates(target, phys, nullptr, 4, 25, 12);
    REQUIRE_FALSE(none.empty());
    REQUIRE(none[0].kind == MixMatchResult::Kind::Physical);
    REQUIRE(none[0].physical_id == 1u);

    SwatchLUT lut;
    lut.batch_key = "synth";
    // Make P1 a terrible measured match and P2 a perfect measured match of the cyan target.
    const prusa_fdm_mixer::LAB target_lab =
        prusa_fdm_mixer::rgb_to_lab(prusa_fdm_mixer::RGB{target.r_uchar(), target.g_uchar(), target.b_uchar()});
    lut.entries.push_back({"P1", 90.0, 40.0, 40.0});
    lut.entries.push_back({"P2", target_lab.L, target_lab.a, target_lab.b});

    const auto with = match_printable_candidates(target, phys, nullptr, 4, 25, 12, 100, &lut);
    REQUIRE_FALSE(with.empty());
    REQUIRE(with[0].kind == MixMatchResult::Kind::Physical);
    REQUIRE(with[0].physical_id == 2u);
    REQUIRE(with[0].measured);
    REQUIRE_FALSE(ranking_byte_identical(none, with));

    const MixMatchResult mix_none = match_printable_mix(target, phys, nullptr, 4, 70, nullptr);
    const MixMatchResult mix_lut  = match_printable_mix(target, phys, nullptr, 4, 70, &lut);
    REQUIRE(mix_none.physical_id == 1u);
    REQUIRE(mix_lut.physical_id == 2u);
}

TEST_CASE("nullptr LUT ranking is byte-identical to ranking without lut pointer", "[spectrum_swatch]")
{
    const std::vector<ColorRGB> phys   = panchroma_physicals();
    const ColorRGB              target = decode_hex_or_fail("#CE921A");
    const auto                  a      = match_printable_candidates(target, phys);
    const auto                  b      = match_printable_candidates(target, phys, nullptr, 4, 25, 12, 100, nullptr);
    REQUIRE(ranking_byte_identical(a, b));
    REQUIRE_FALSE(a.empty());
    REQUIRE_FALSE(a[0].measured);

    const MixMatchResult mix_a = match_printable_mix(target, phys);
    const MixMatchResult mix_b = match_printable_mix(target, phys, nullptr, 4, 100, nullptr);
    REQUIRE(mix_a.kind == mix_b.kind);
    REQUIRE(mix_a.recipe_row == mix_b.recipe_row);
    REQUIRE(mix_a.physical_id == mix_b.physical_id);
    REQUIRE(mix_a.distance == mix_b.distance);
    REQUIRE(mix_a.predicted == mix_b.predicted);

    std::vector<std::uint8_t> rgb(size_t(8) * 2 * 3, 0);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 8; ++x) {
            const size_t i = (size_t(y) * 8 + size_t(x)) * 3;
            if (x < 4) {
                rgb[i] = 0x08; rgb[i + 1] = 0xAB; rgb[i + 2] = 0xFB;
            } else {
                rgb[i] = 0xD9; rgb[i + 1] = 0x3B; rgb[i + 2] = 0x90;
            }
        }
    }
    const SpectrumPicPrintPlan p0 = plan_spectrum_picprint(rgb.data(), 8, 2, phys, 4);
    const SpectrumPicPrintPlan p1 = plan_spectrum_picprint(rgb.data(), 8, 2, phys, 4,
                                                           SPECTRUM_PAINT_ID_PERSIST_CAP, {}, nullptr);
    REQUIRE(p0.valid);
    REQUIRE(p1.valid);
    REQUIRE(p0.dest_id == p1.dest_id);
    REQUIRE(p0.mixed_filament_definitions == p1.mixed_filament_definitions);

    // Frozen HEAD (fdea2c2d10) ranking contract for Panchroma + #CE921A, max_results=12.
    // Keys + distances captured from nullptr LUT path after lattice extract.
    REQUIRE(a.size() == 12);
    REQUIRE(a[0].kind == MixMatchResult::Kind::Mix);
    // Frozen ranking for Panchroma + #CE921A @ max_results=12 (HEAD fdea2c2d10 nullptr path).
    const std::vector<std::string> head_keys = {
        "2,3,1,1,3",
        "2,3,1,1,2",
        "2,3,1,1,2,c4,rc1",
        "3,4,1,1,1",
        "3,4,1,2,1",
        "3,4,1,3,1",
        "3,4,1,1,2",
        "3,4,1,1,3",
        "P3",
        "1,2,1,1,1,c3,rc2",
        "2,3,1,1,1,c4,rc2",
        "2,3,1,1,1,c4,rc1",
    };
    const float head_dist[] = {
        7.23295546f, 16.3173809f, 16.673914f, 18.3103733f, 19.0504169f, 20.127943f,
        20.5951138f, 22.3597145f, 25.5842724f, 26.0279408f, 31.4628906f, 32.3313141f,
    };
    for (size_t i = 0; i < head_keys.size(); ++i) {
        REQUIRE(spectrum_swatch_recipe_key(a[i]) == head_keys[i]);
        REQUIRE_THAT(a[i].distance, Catch::Matchers::WithinAbs(head_dist[i], 1e-4));
        REQUIRE_FALSE(a[i].measured);
        REQUIRE(spectrum_swatch_recipe_key(mix_a) == head_keys[0]);
    }
    for (size_t i = 1; i < a.size(); ++i)
        REQUIRE(a[i - 1].distance <= a[i].distance);
}

TEST_CASE("LUT ranking keeps measured winner on dark-neutral targets", "[spectrum_swatch]")
{
    const std::vector<ColorRGB> phys   = panchroma_physicals();
    const ColorRGB              target = decode_hex_or_fail("#000000");
    const MixMatchResult        none   = match_printable_mix(target, phys);
    REQUIRE(none.kind == MixMatchResult::Kind::Physical);
    REQUIRE(none.physical_id == 4u); // Grey HEAD override

    SwatchLUT lut;
    lut.batch_key = "dark";
    lut.entries.push_back({"P4", 90.0, 40.0, 40.0}); // Grey terrible measured
    lut.entries.push_back({"1,2,1,1,1", 0.0, 0.0, 0.0}); // C+M 1:1 perfect Lab black
    const MixMatchResult with = match_printable_mix(target, phys, nullptr, 4, 100, &lut);
    REQUIRE(with.kind == MixMatchResult::Kind::Mix);
    REQUIRE(with.measured);
    REQUIRE(spectrum_swatch_recipe_key(with) == "1,2,1,1,1");
}

TEST_CASE("LUT near-tie groups use predicted-RGB order without pairwise comparator", "[spectrum_swatch]")
{
    const std::vector<ColorRGB> phys   = panchroma_physicals();
    const ColorRGB              target = decode_hex_or_fail("#CE921A");
    const auto                  none   = match_printable_candidates(target, phys, nullptr, 4, 25, 12);
    REQUIRE(none.size() >= 3);

    SwatchLUT lut;
    lut.batch_key = "neartie";
    for (const MixMatchResult &c : none) {
        const std::string key = spectrum_swatch_recipe_key(c);
        lut.entries.push_back({key, 50.0, 0.0, 0.0});
    }
    // Overlapping 0.4/0.4/0.8 measured gaps: pairwise |Δ|<0.5 is not transitive.
    lut.entries[0].L = 10.0;
    lut.entries[1].L = 10.4;
    lut.entries[2].L = 10.8;

    const auto with = match_printable_candidates(target, phys, nullptr, 4, 25, 12, 100, &lut);
    REQUIRE(with.size() >= 3);
    const auto again = match_printable_candidates(target, phys, nullptr, 4, 25, 12, 100, &lut);
    REQUIRE(ranking_byte_identical(with, again));
    for (size_t i = 1; i < with.size(); ++i) {
        if (with[i].distance - with[i - 1].distance >= 0.5f)
            continue;
        const float pa = mixer_delta_e00(with[i - 1].predicted, target);
        const float pb = mixer_delta_e00(with[i].predicted, target);
        REQUIRE(pa <= pb);
    }
}

TEST_CASE("persisted LUT is stale after restart when live batch changes", "[spectrum_swatch]")
{
    const auto dir = unique_temp_dir();
    spectrum_reset_lut_session();

    SwatchLUT lut;
    lut.batch_key = "batchA";
    lut.entries.push_back({"P1", 12.0, 3.0, -1.0});
    std::string err;
    REQUIRE(save_swatch_lut(lut, err, dir.string()));
    spectrum_store_loaded_lut(lut, dir.string());
    REQUIRE(spectrum_lut_for_batch("batchA", dir.string()) != nullptr);
    REQUIRE_FALSE(spectrum_lut_is_stale("batchA", dir.string()));
    REQUIRE(spectrum_lut_is_stale("batchB", dir.string()));

    spectrum_reset_lut_session();
    REQUIRE(spectrum_loaded_lut() == nullptr);
    REQUIRE(spectrum_lut_for_batch("batchB", dir.string()) == nullptr);
    REQUIRE(spectrum_lut_is_stale("batchB", dir.string()));
    REQUIRE_FALSE(spectrum_lut_is_stale("batchA", dir.string()));
    REQUIRE(spectrum_lut_for_batch("batchA", dir.string()) != nullptr);

    boost::filesystem::remove_all(dir);
    spectrum_reset_lut_session();
}
