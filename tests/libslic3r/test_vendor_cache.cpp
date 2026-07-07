#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <boost/crc.hpp>
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

std::string write_vendor_json(const fs::path& dir, const std::string& vendor_id,
                               const std::string& version = "1.0.0")
{
    const fs::path p = dir / (vendor_id + ".json");
    std::ofstream f(p.string());
    f << R"({"version":")" << version << R"(","name":")" << vendor_id << R"("})";
    return p.string();
}

std::string write_versionless_vendor_json(const fs::path& dir, const std::string& vendor_id)
{
    const fs::path p = dir / (vendor_id + ".json");
    std::ofstream f(p.string());
    f << R"({"name":")" << vendor_id << R"("})";
    return p.string();
}

void corrupt_blob_byte(const std::string& path)
{
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(30);
    char b = 0; f.read(&b, 1);
    f.seekp(30);
    b ^= 0xFF;
    f.write(&b, 1);
}

// Patch cache_version (blob[0..3]) and recompute CRC so the file passes
// the CRC check but fails the cache_version check in load_system_presets_cache_for_guide.
void patch_cache_version(const std::string& path, uint32_t wrong_version)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<char> data(std::istreambuf_iterator<char>(in), {});
    in.close();
    if (data.size() < 24) return;
    std::memcpy(&data[20], &wrong_version, 4);
    boost::crc_32_type crc;
    crc.process_bytes(&data[20], data.size() - 20);
    const uint32_t new_crc = crc.checksum();
    std::memcpy(&data[16], &new_crc, 4);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void add_vendor(PresetBundle& bundle, const std::string& vendor_id,
                const std::string& name = "", Semver ver = Semver(1, 0, 0))
{
    VendorProfile vp(vendor_id);
    vp.name           = name.empty() ? vendor_id + " Corp" : name;
    vp.config_version = ver;
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

PresetBundle::SaveCacheResult save_one_vendor(PresetBundle& src,
                                               const fs::path& profiles_dir,
                                               const fs::path& cache_path)
{
    return src.save_system_presets_cache(profiles_dir.string(), cache_path.string());
}

// Helper: filter a collection by vendor_id.
std::vector<const Preset*> presets_for(const PresetCollection& coll, const std::string& vendor_id)
{
    std::vector<const Preset*> out;
    for (const Preset& p : coll())
        if (p.is_system && p.vendor && p.vendor->id == vendor_id)
            out.push_back(&p);
    return out;
}

} // namespace

TEST_CASE("SystemPresetsCache: save and load via guide API", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid = "Acme";
    write_vendor_json(tmp.path, vid);
    const fs::path cache = tmp.path / "system_presets.cache";

    PresetBundle src;
    add_vendor(src, vid);
    const VendorProfile* vp = &src.vendors.at(vid);

    Preset& fp = add_system_preset(src.filaments, vid + " PLA @0.4", vp);
    fp.alias       = "Acme PLA";
    fp.filament_id = "GFL_acme_pla";
    add_system_preset(src.printers, vid + " Printer 0.4", vp);

    const auto stats = save_one_vendor(src, tmp.path, cache);
    REQUIRE(stats.ok);
    CHECK(stats.filament_presets == 1);
    CHECK(stats.printer_presets  == 1);
    CHECK(stats.print_presets    == 0);

    PresetBundle out;
    REQUIRE(PresetBundle::load_system_presets_cache_for_guide(cache.string(), out));
    REQUIRE(out.vendors.count(vid) == 1);

    auto fi = presets_for(out.filaments, vid);
    auto pr = presets_for(out.printers,  vid);
    REQUIRE(fi.size() == 1);
    CHECK(fi[0]->name        == vid + " PLA @0.4");
    CHECK(fi[0]->alias       == "Acme PLA");
    CHECK(fi[0]->filament_id == "GFL_acme_pla");
    REQUIRE(pr.size() == 1);
    CHECK(pr[0]->name == vid + " Printer 0.4");
}

TEST_CASE("SystemPresetsCache: missing file returns false", "[VendorCache]")
{
    TempDir tmp;
    PresetBundle out;
    CHECK_FALSE(PresetBundle::load_system_presets_cache_for_guide(
        (tmp.path / "nonexistent.cache").string(), out));
}

TEST_CASE("SystemPresetsCache: corrupt data rejected by CRC check", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid = "Acme";
    write_vendor_json(tmp.path, vid);
    const fs::path cache = tmp.path / "system_presets.cache";

    PresetBundle src;
    add_vendor(src, vid);
    add_system_preset(src.filaments, vid + " PLA", &src.vendors.at(vid));
    save_one_vendor(src, tmp.path, cache);
    corrupt_blob_byte(cache.string());

    PresetBundle out;
    CHECK_FALSE(PresetBundle::load_system_presets_cache_for_guide(cache.string(), out));
}

TEST_CASE("SystemPresetsCache: multiple vendors in one bundle are isolated", "[VendorCache]")
{
    TempDir tmp;
    const std::string vid1 = "VendorA";
    const std::string vid2 = "VendorB";
    write_vendor_json(tmp.path, vid1);
    write_vendor_json(tmp.path, vid2);
    const fs::path cache = tmp.path / "system_presets.cache";

    PresetBundle src;
    for (const auto& vid : {vid1, vid2}) {
        add_vendor(src, vid);
        const VendorProfile* vp = &src.vendors.at(vid);
        add_system_preset(src.filaments, vid + " PLA",     vp);
        add_system_preset(src.printers,  vid + " Printer", vp);
    }

    REQUIRE(save_one_vendor(src, tmp.path, cache).ok);

    PresetBundle out;
    REQUIRE(PresetBundle::load_system_presets_cache_for_guide(cache.string(), out));
    REQUIRE(out.vendors.count(vid1) == 1);
    REQUIRE(out.vendors.count(vid2) == 1);

    for (const auto& vid : {vid1, vid2}) {
        auto fi = presets_for(out.filaments, vid);
        auto pr = presets_for(out.printers,  vid);
        REQUIRE(fi.size() == 1);
        CHECK(fi[0]->name == vid + " PLA");
        REQUIRE(pr.size() == 1);
        CHECK(pr[0]->name == vid + " Printer");
    }
}

TEST_CASE("SystemPresetsCache: vendor profile fields are preserved", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid = "Acme";
    write_vendor_json(tmp.path, vid, "2.5.1");
    const fs::path cache = tmp.path / "system_presets.cache";

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
    save_one_vendor(src, tmp.path, cache);

    PresetBundle out;
    REQUIRE(PresetBundle::load_system_presets_cache_for_guide(cache.string(), out));
    REQUIRE(out.vendors.count(vid) == 1);
    const VendorProfile& gvp = out.vendors.at(vid);
    CHECK(gvp.id   == vid);
    CHECK(gvp.name == "Acme Corporation");
    REQUIRE(gvp.models.size() == 1);
    CHECK(gvp.models[0].id   == "AcmePro");
    CHECK(gvp.models[0].name == "Acme Pro");
    REQUIRE(gvp.models[0].variants.size() == 1);
    CHECK(gvp.models[0].variants[0].name  == "0.4");
}

TEST_CASE("SystemPresetsCache: config option values are preserved", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid = "Acme";
    write_vendor_json(tmp.path, vid);
    const fs::path cache = tmp.path / "system_presets.cache";

    PresetBundle src;
    add_vendor(src, vid);
    Preset& fp = add_system_preset(src.filaments, vid + " PETG @0.4", &src.vendors.at(vid));
    fp.config.set_key_value("filament_type", new ConfigOptionStrings({"PETG"}));
    save_one_vendor(src, tmp.path, cache);

    PresetBundle out;
    REQUIRE(PresetBundle::load_system_presets_cache_for_guide(cache.string(), out));

    auto fi = presets_for(out.filaments, vid);
    REQUIRE(fi.size() == 1);
    const auto* ft = fi[0]->config.option<ConfigOptionStrings>("filament_type");
    REQUIRE(ft != nullptr);
    REQUIRE(ft->values.size() >= 1);
    CHECK(ft->values[0] == "PETG");
}

TEST_CASE("SystemPresetsCache: multiple presets per collection round-trip", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid = "Acme";
    write_vendor_json(tmp.path, vid);
    const fs::path cache = tmp.path / "system_presets.cache";

    PresetBundle src;
    add_vendor(src, vid);
    const VendorProfile* vp = &src.vendors.at(vid);

    const std::vector<std::string> fi_names = {vid + " PLA", vid + " PETG", vid + " ABS"};
    const std::vector<std::string> pr_names = {vid + " Printer 0.4", vid + " Printer 0.6"};
    for (const auto& n : fi_names) add_system_preset(src.filaments, n, vp);
    for (const auto& n : pr_names) add_system_preset(src.printers,  n, vp);

    const auto stats = save_one_vendor(src, tmp.path, cache);
    REQUIRE(stats.ok);
    CHECK(stats.filament_presets == 3);
    CHECK(stats.printer_presets  == 2);

    PresetBundle out;
    REQUIRE(PresetBundle::load_system_presets_cache_for_guide(cache.string(), out));

    auto fi = presets_for(out.filaments, vid);
    auto pr = presets_for(out.printers,  vid);
    REQUIRE(fi.size() == 3);
    REQUIRE(pr.size() == 2);

    std::set<std::string> fi_got, pr_got;
    for (const auto* p : fi) fi_got.insert(p->name);
    for (const auto* p : pr) pr_got.insert(p->name);
    for (const auto& n : fi_names) CHECK(fi_got.count(n) == 1);
    for (const auto& n : pr_names) CHECK(pr_got.count(n) == 1);
}

TEST_CASE("SystemPresetsCache: truncated file is rejected", "[VendorCache]")
{
    TempDir        tmp;
    const fs::path cache = tmp.path / "truncated.cache";
    {
        std::ofstream f(cache.string(), std::ios::binary);
        const char data[] = {0x4F, 0x52, 0x43};
        f.write(data, sizeof(data));
    }
    PresetBundle out;
    CHECK_FALSE(PresetBundle::load_system_presets_cache_for_guide(cache.string(), out));
}

TEST_CASE("SystemPresetsCache: wrong magic is rejected", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid = "Acme";
    write_vendor_json(tmp.path, vid);
    const fs::path cache = tmp.path / "system_presets.cache";

    PresetBundle src;
    add_vendor(src, vid);
    add_system_preset(src.filaments, vid + " PLA", &src.vendors.at(vid));
    save_one_vendor(src, tmp.path, cache);

    {
        std::fstream f(cache.string(), std::ios::in | std::ios::out | std::ios::binary);
        const uint32_t bad = 0xDEADBEEFu;
        f.write(reinterpret_cast<const char*>(&bad), sizeof(bad));
    }

    PresetBundle out;
    CHECK_FALSE(PresetBundle::load_system_presets_cache_for_guide(cache.string(), out));
}

TEST_CASE("SystemPresetsCache: vendor with no presets saves and loads cleanly", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid = "Acme";
    write_vendor_json(tmp.path, vid);
    const fs::path cache = tmp.path / "system_presets.cache";

    PresetBundle src;
    VendorProfile vp(vid);
    vp.name           = "Acme Corporation";
    vp.config_version = Semver(1, 0, 0);
    src.vendors.emplace(vid, vp);

    const auto stats = save_one_vendor(src, tmp.path, cache);
    REQUIRE(stats.ok);
    CHECK(stats.print_presets    == 0);
    CHECK(stats.filament_presets == 0);
    CHECK(stats.printer_presets  == 0);

    PresetBundle out;
    REQUIRE(PresetBundle::load_system_presets_cache_for_guide(cache.string(), out));
    REQUIRE(out.vendors.count(vid) == 1);
    CHECK(out.vendors.at(vid).id   == vid);
    CHECK(out.vendors.at(vid).name == "Acme Corporation");
    CHECK(presets_for(out.filaments, vid).empty());
    CHECK(presets_for(out.printers,  vid).empty());
    CHECK(presets_for(out.prints,    vid).empty());
}

TEST_CASE("SystemPresetsCache: all Preset metadata fields are preserved", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid = "Acme";
    write_vendor_json(tmp.path, vid);
    const fs::path cache = tmp.path / "system_presets.cache";

    PresetBundle src;
    add_vendor(src, vid);
    Preset& fp     = add_system_preset(src.filaments, vid + " PLA @0.4", &src.vendors.at(vid));
    fp.setting_id  = "sid-test-001";
    fp.description = "A test filament preset";
    fp.bundle_id   = "bundle-xyz";
    fp.user_id     = "user-abc";
    fp.base_id     = "base-123";
    fp.sync_info   = "update";
    fp.updated_time = 1700000000LL;
    fp.key_values  = {{"color", "red"}, {"diameter", "1.75"}};
    fp.ini_str     = "[filament]\nnozzle_temperature = 230\n";
    save_one_vendor(src, tmp.path, cache);

    PresetBundle out;
    REQUIRE(PresetBundle::load_system_presets_cache_for_guide(cache.string(), out));

    auto fi = presets_for(out.filaments, vid);
    REQUIRE(fi.size() == 1);
    CHECK(fi[0]->setting_id   == "sid-test-001");
    CHECK(fi[0]->description  == "A test filament preset");
    CHECK(fi[0]->bundle_id    == "bundle-xyz");
    CHECK(fi[0]->user_id      == "user-abc");
    CHECK(fi[0]->base_id      == "base-123");
    CHECK(fi[0]->sync_info    == "update");
    CHECK(fi[0]->updated_time == 1700000000LL);
    REQUIRE(fi[0]->key_values.count("color") == 1);
    CHECK(fi[0]->key_values.at("color")      == "red");
    REQUIRE(fi[0]->key_values.count("diameter") == 1);
    CHECK(fi[0]->key_values.at("diameter")   == "1.75");
    CHECK(fi[0]->ini_str == "[filament]\nnozzle_temperature = 230\n");
}

TEST_CASE("SystemPresetsCache: wrong cache_version is rejected", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid = "Acme";
    write_vendor_json(tmp.path, vid);
    const fs::path cache = tmp.path / "system_presets.cache";

    PresetBundle src;
    add_vendor(src, vid);
    add_system_preset(src.filaments, vid + " PLA", &src.vendors.at(vid));
    save_one_vendor(src, tmp.path, cache);
    patch_cache_version(cache.string(), 0xFFFFFFFFu);

    PresetBundle out;
    CHECK_FALSE(PresetBundle::load_system_presets_cache_for_guide(cache.string(), out));
}

TEST_CASE("SystemPresetsCache: mid-blob truncation is rejected", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid = "Acme";
    write_vendor_json(tmp.path, vid);
    const fs::path cache = tmp.path / "system_presets.cache";

    PresetBundle src;
    add_vendor(src, vid);
    add_system_preset(src.filaments, vid + " PLA", &src.vendors.at(vid));
    save_one_vendor(src, tmp.path, cache);

    {
        std::ifstream in(cache.string(), std::ios::binary);
        std::vector<char> buf(30); // 20-byte header + 10 bytes of blob
        in.read(buf.data(), 30);
        in.close();
        std::ofstream out(cache.string(), std::ios::binary | std::ios::trunc);
        out.write(buf.data(), 30);
    }

    PresetBundle out;
    CHECK_FALSE(PresetBundle::load_system_presets_cache_for_guide(cache.string(), out));
}

TEST_CASE("SystemPresetsCache: versionless vendor uses mtime key in bundle", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid = "Acme";
    write_versionless_vendor_json(tmp.path, vid);
    const fs::path cache = tmp.path / "system_presets.cache";

    PresetBundle src;
    add_vendor(src, vid);
    add_system_preset(src.filaments, vid + " PLA", &src.vendors.at(vid));
    REQUIRE(save_one_vendor(src, tmp.path, cache).ok);

    PresetBundle out;
    REQUIRE(PresetBundle::load_system_presets_cache_for_guide(cache.string(), out));
    auto fi = presets_for(out.filaments, vid);
    REQUIRE(fi.size() == 1);
    CHECK(fi[0]->name == vid + " PLA");
}
