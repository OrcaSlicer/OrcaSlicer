#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <fstream>
#include <set>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / fs::unique_path("orca-cache-test-%%%%-%%%%");
        fs::create_directories(path);
    }
    ~TempDir() { boost::system::error_code ec; fs::remove_all(path, ec); }
};

// Write a minimal vendor JSON with a version field so get_vendor_cache_key() returns
// a deterministic Semver string rather than an mtime-based key.
std::string write_vendor_json(const fs::path& dir, const std::string& vendor_id,
                               const std::string& version = "1.0.0")
{
    const fs::path p = dir / (vendor_id + ".json");
    std::ofstream f(p.string());
    f << R"({"version":")" << version << R"(","name":")" << vendor_id << R"("})";
    return p.string();
}

void add_vendor(PresetBundle& bundle, const std::string& vendor_id)
{
    VendorProfile vp(vendor_id);
    vp.name           = vendor_id + " Corp";
    vp.config_version = Semver(1, 0, 0);
    bundle.vendors.emplace(vendor_id, vp);
}

Preset& add_system_preset(PresetCollection& coll, const std::string& name,
                            const VendorProfile* vp)
{
    Preset& p = coll.load_preset("", name, DynamicPrintConfig(coll.default_preset().config), false);
    p.is_system = true;
    p.vendor    = vp;
    return p;
}

} // namespace

TEST_CASE("VendorCache: save and load via guide API", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid       = "Acme";
    const std::string json_path = write_vendor_json(tmp.path, vid);
    const fs::path    cache     = tmp.path / (vid + ".cache");

    PresetBundle src;
    add_vendor(src, vid);
    const VendorProfile* vp = &src.vendors.at(vid);

    Preset& fp = add_system_preset(src.filaments, vid + " PLA @0.4", vp);
    fp.alias       = "Acme PLA";
    fp.filament_id = "GFL_acme_pla";

    add_system_preset(src.printers, vid + " Printer 0.4", vp);

    const auto stats = src.save_bundled_vendor_cache(vid, json_path, false, cache.string());
    REQUIRE(stats.ok);
    CHECK(stats.filament_presets == 1);
    CHECK(stats.printer_presets  == 1);
    CHECK(stats.print_presets    == 0);

    VendorProfile       out_profile;
    std::vector<Preset> out_printers, out_filaments, out_prints;
    REQUIRE(PresetBundle::load_vendor_cache_for_guide(
        cache.string(), get_vendor_cache_key(json_path),
        out_profile, out_printers, out_filaments, out_prints));

    CHECK(out_profile.id == vid);
    REQUIRE(out_filaments.size() == 1);
    CHECK(out_filaments[0].name        == vid + " PLA @0.4");
    CHECK(out_filaments[0].alias       == "Acme PLA");
    CHECK(out_filaments[0].filament_id == "GFL_acme_pla");
    REQUIRE(out_printers.size() == 1);
    CHECK(out_printers[0].name == vid + " Printer 0.4");
}

TEST_CASE("VendorCache: stale json_ver is rejected", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid       = "Acme";
    const std::string json_path = write_vendor_json(tmp.path, vid, "1.0.0");
    const fs::path    cache     = tmp.path / (vid + ".cache");

    PresetBundle src;
    add_vendor(src, vid);
    add_system_preset(src.filaments, vid + " PLA", &src.vendors.at(vid));
    src.save_bundled_vendor_cache(vid, json_path, false, cache.string());

    VendorProfile       p; std::vector<Preset> pr, fi, pp;
    CHECK_FALSE(PresetBundle::load_vendor_cache_for_guide(cache.string(), "2.0.0", p, pr, fi, pp));
}

TEST_CASE("VendorCache: missing file returns false", "[VendorCache]")
{
    TempDir tmp;
    VendorProfile       p; std::vector<Preset> pr, fi, pp;
    CHECK_FALSE(PresetBundle::load_vendor_cache_for_guide(
        (tmp.path / "nonexistent.cache").string(), "1.0.0", p, pr, fi, pp));
}

TEST_CASE("VendorCache: corrupt data is rejected by CRC check", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid       = "Acme";
    const std::string json_path = write_vendor_json(tmp.path, vid);
    const fs::path    cache     = tmp.path / (vid + ".cache");

    PresetBundle src;
    add_vendor(src, vid);
    add_system_preset(src.filaments, vid + " PLA", &src.vendors.at(vid));
    src.save_bundled_vendor_cache(vid, json_path, false, cache.string());

    // Flip a byte in the data region (after the 20-byte CacheFileHeader).
    {
        std::fstream f(cache.string(), std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(30);
        char b; f.read(&b, 1);
        f.seekp(30);
        b ^= 0xFF;
        f.write(&b, 1);
    }

    VendorProfile       p; std::vector<Preset> pr, fi, pp;
    CHECK_FALSE(PresetBundle::load_vendor_cache_for_guide(
        cache.string(), get_vendor_cache_key(json_path), p, pr, fi, pp));
}

TEST_CASE("VendorCache: multiple vendors do not bleed into each other", "[VendorCache]")
{
    TempDir tmp;
    const std::string vid1 = "VendorA";
    const std::string vid2 = "VendorB";

    PresetBundle src;
    const std::string json1 = write_vendor_json(tmp.path, vid1);
    const std::string json2 = write_vendor_json(tmp.path, vid2);

    for (const auto& vid : {vid1, vid2}) {
        add_vendor(src, vid);
        const VendorProfile* vp = &src.vendors.at(vid);
        add_system_preset(src.filaments, vid + " PLA",     vp);
        add_system_preset(src.printers,  vid + " Printer", vp);
    }

    src.save_bundled_vendor_cache(vid1, json1, false, (tmp.path / (vid1 + ".cache")).string());
    src.save_bundled_vendor_cache(vid2, json2, false, (tmp.path / (vid2 + ".cache")).string());

    for (const auto& [vid, jpath] : std::vector<std::pair<std::string, std::string>>{{vid1, json1}, {vid2, json2}}) {
        VendorProfile       p; std::vector<Preset> pr, fi, pp;
        REQUIRE(PresetBundle::load_vendor_cache_for_guide(
            (tmp.path / (vid + ".cache")).string(), get_vendor_cache_key(jpath),
            p, pr, fi, pp));
        REQUIRE(fi.size() == 1);
        CHECK(fi[0].name == vid + " PLA");
        REQUIRE(pr.size() == 1);
        CHECK(pr[0].name == vid + " Printer");
    }
}

TEST_CASE("VendorCache: vendor profile fields are preserved", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid       = "Acme";
    const std::string json_path = write_vendor_json(tmp.path, vid, "2.5.1");
    const fs::path    cache     = tmp.path / (vid + ".cache");

    PresetBundle src;
    VendorProfile vp(vid);
    vp.name           = "Acme Corporation";
    vp.config_version = Semver(2, 5, 1);
    VendorProfile::PrinterModel model;
    model.id   = "AcmePro";
    model.name = "Acme Pro";
    VendorProfile::PrinterVariant v0_4; v0_4.name = "0.4";
    model.variants.push_back(v0_4);
    vp.models.push_back(model);
    src.vendors.emplace(vid, vp);

    src.save_bundled_vendor_cache(vid, json_path, false, cache.string());

    VendorProfile       out_p; std::vector<Preset> pr, fi, pp;
    REQUIRE(PresetBundle::load_vendor_cache_for_guide(
        cache.string(), get_vendor_cache_key(json_path), out_p, pr, fi, pp));

    CHECK(out_p.id   == vid);
    CHECK(out_p.name == "Acme Corporation");
    REQUIRE(out_p.models.size() == 1);
    CHECK(out_p.models[0].id   == "AcmePro");
    CHECK(out_p.models[0].name == "Acme Pro");
    REQUIRE(out_p.models[0].variants.size() == 1);
    CHECK(out_p.models[0].variants[0].name  == "0.4");
}

TEST_CASE("VendorCache: config option values are preserved", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid       = "Acme";
    const std::string json_path = write_vendor_json(tmp.path, vid);
    const fs::path    cache     = tmp.path / (vid + ".cache");

    PresetBundle src;
    add_vendor(src, vid);
    const VendorProfile* vp = &src.vendors.at(vid);

    Preset& fp = add_system_preset(src.filaments, vid + " PETG @0.4", vp);
    fp.config.set_key_value("filament_type", new ConfigOptionStrings({"PETG"}));

    src.save_bundled_vendor_cache(vid, json_path, false, cache.string());

    VendorProfile       p; std::vector<Preset> pr, fi, pp;
    REQUIRE(PresetBundle::load_vendor_cache_for_guide(
        cache.string(), get_vendor_cache_key(json_path), p, pr, fi, pp));

    REQUIRE(fi.size() == 1);
    const auto* ft = fi[0].config.option<ConfigOptionStrings>("filament_type");
    REQUIRE(ft != nullptr);
    REQUIRE(ft->values.size() >= 1);
    CHECK(ft->values[0] == "PETG");
}

TEST_CASE("VendorCache: multiple presets per collection round-trip", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid       = "Acme";
    const std::string json_path = write_vendor_json(tmp.path, vid);
    const fs::path    cache     = tmp.path / (vid + ".cache");

    PresetBundle src;
    add_vendor(src, vid);
    const VendorProfile* vp = &src.vendors.at(vid);

    const std::vector<std::string> fi_names = {vid + " PLA", vid + " PETG", vid + " ABS"};
    const std::vector<std::string> pr_names = {vid + " Printer 0.4", vid + " Printer 0.6"};

    for (const auto& n : fi_names) add_system_preset(src.filaments, n, vp);
    for (const auto& n : pr_names) add_system_preset(src.printers,  n, vp);

    const auto stats = src.save_bundled_vendor_cache(vid, json_path, false, cache.string());
    REQUIRE(stats.ok);
    CHECK(stats.filament_presets == 3);
    CHECK(stats.printer_presets  == 2);

    VendorProfile       p; std::vector<Preset> pr, fi, pp;
    REQUIRE(PresetBundle::load_vendor_cache_for_guide(
        cache.string(), get_vendor_cache_key(json_path), p, pr, fi, pp));

    REQUIRE(fi.size() == 3);
    REQUIRE(pr.size() == 2);
    std::set<std::string> fi_got, pr_got;
    for (const auto& x : fi) fi_got.insert(x.name);
    for (const auto& x : pr) pr_got.insert(x.name);
    for (const auto& n : fi_names) CHECK(fi_got.count(n) == 1);
    for (const auto& n : pr_names) CHECK(pr_got.count(n) == 1);
}

TEST_CASE("VendorCache: truncated file is rejected", "[VendorCache]")
{
    TempDir        tmp;
    const fs::path cache = tmp.path / "truncated.cache";

    // 3 bytes — shorter than the 20-byte CacheFileHeader
    {
        std::ofstream f(cache.string(), std::ios::binary);
        const char data[] = {0x4F, 0x52, 0x43};
        f.write(data, sizeof(data));
    }

    VendorProfile       p; std::vector<Preset> pr, fi, pp;
    CHECK_FALSE(PresetBundle::load_vendor_cache_for_guide(
        cache.string(), "1.0.0", p, pr, fi, pp));
}

TEST_CASE("VendorCache: wrong magic is rejected", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid       = "Acme";
    const std::string json_path = write_vendor_json(tmp.path, vid);
    const fs::path    cache     = tmp.path / (vid + ".cache");

    PresetBundle src;
    add_vendor(src, vid);
    add_system_preset(src.filaments, vid + " PLA", &src.vendors.at(vid));
    src.save_bundled_vendor_cache(vid, json_path, false, cache.string());

    // Overwrite first 4 bytes (magic field) with a wrong value; rest of file stays valid.
    {
        std::fstream f(cache.string(), std::ios::in | std::ios::out | std::ios::binary);
        const uint32_t bad = 0xDEADBEEFu;
        f.write(reinterpret_cast<const char*>(&bad), sizeof(bad));
    }

    VendorProfile       p; std::vector<Preset> pr, fi, pp;
    CHECK_FALSE(PresetBundle::load_vendor_cache_for_guide(
        cache.string(), get_vendor_cache_key(json_path), p, pr, fi, pp));
}

TEST_CASE("VendorCache: vendor with no presets saves and loads cleanly", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid       = "Acme";
    const std::string json_path = write_vendor_json(tmp.path, vid);
    const fs::path    cache     = tmp.path / (vid + ".cache");

    PresetBundle src;
    VendorProfile vp(vid);
    vp.name           = "Acme Corporation";
    vp.config_version = Semver(1, 0, 0);
    src.vendors.emplace(vid, vp);

    const auto stats = src.save_bundled_vendor_cache(vid, json_path, false, cache.string());
    REQUIRE(stats.ok);
    CHECK(stats.print_presets    == 0);
    CHECK(stats.filament_presets == 0);
    CHECK(stats.printer_presets  == 0);

    VendorProfile       p; std::vector<Preset> pr, fi, pp;
    REQUIRE(PresetBundle::load_vendor_cache_for_guide(
        cache.string(), get_vendor_cache_key(json_path), p, pr, fi, pp));
    CHECK(p.id   == vid);
    CHECK(p.name == "Acme Corporation");
    CHECK(fi.empty());
    CHECK(pr.empty());
    CHECK(pp.empty());
}

TEST_CASE("VendorCache: all Preset metadata fields are preserved", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid       = "Acme";
    const std::string json_path = write_vendor_json(tmp.path, vid);
    const fs::path    cache     = tmp.path / (vid + ".cache");

    PresetBundle src;
    add_vendor(src, vid);
    const VendorProfile* vp = &src.vendors.at(vid);

    Preset& fp          = add_system_preset(src.filaments, vid + " PLA @0.4", vp);
    fp.setting_id       = "sid-test-001";
    fp.description      = "A test filament preset";
    fp.bundle_id        = "bundle-xyz";
    fp.user_id          = "user-abc";
    fp.base_id          = "base-123";
    fp.sync_info        = "update";
    fp.updated_time     = 1700000000LL;
    fp.key_values       = {{"color", "red"}, {"diameter", "1.75"}};
    fp.ini_str          = "[filament]\nnozzle_temperature = 230\n";

    src.save_bundled_vendor_cache(vid, json_path, false, cache.string());

    VendorProfile       p; std::vector<Preset> pr, fi, pp;
    REQUIRE(PresetBundle::load_vendor_cache_for_guide(
        cache.string(), get_vendor_cache_key(json_path), p, pr, fi, pp));

    REQUIRE(fi.size() == 1);
    const Preset& out = fi[0];
    CHECK(out.setting_id   == "sid-test-001");
    CHECK(out.description  == "A test filament preset");
    CHECK(out.bundle_id    == "bundle-xyz");
    CHECK(out.user_id      == "user-abc");
    CHECK(out.base_id      == "base-123");
    CHECK(out.sync_info    == "update");
    CHECK(out.updated_time == 1700000000LL);
    REQUIRE(out.key_values.count("color") == 1);
    CHECK(out.key_values.at("color")      == "red");
    REQUIRE(out.key_values.count("diameter") == 1);
    CHECK(out.key_values.at("diameter")   == "1.75");
    CHECK(out.ini_str == "[filament]\nnozzle_temperature = 230\n");
}
