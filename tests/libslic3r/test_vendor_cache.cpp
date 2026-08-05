#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <boost/crc.hpp>
#include <fstream>
#include <set>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
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

// One vendor profile with a single process preset beside it, as an install or an
// update lays it down: <dir>/<vendor>.json plus <dir>/<vendor>/process/standard.json.
void write_vendor_tree(const fs::path& dir, const std::string& vendor, const std::string& version)
{
    fs::create_directories(dir / vendor / "process");
    std::ofstream((dir / (vendor + ".json")).string())
        << R"({"version":")" << version << R"(","name":")" << vendor
        << R"(","process_list":[{"name":"0.20mm Standard @)" << vendor << R"(","sub_path":"process/standard.json"}]})";
    std::ofstream((dir / vendor / "process" / "standard.json").string())
        << R"({"type":"process","name":"0.20mm Standard @)" << vendor
        << R"(","from":"system","instantiation":"true","layer_height":"0.2"})";
}

// A small but complete vendor: one machine model, one process, a non-instantiated
// base filament with an instantiated child inheriting it, a second standalone
// filament carrying explicit metadata, and one machine preset with a rename — so
// the equivalence test below sees every CachedPreset field populated.
void write_full_vendor_tree(const fs::path& dir, const std::string& vendor, const std::string& version)
{
    fs::create_directories(dir / vendor / "process");
    fs::create_directories(dir / vendor / "filament");
    fs::create_directories(dir / vendor / "machine");
    std::ofstream((dir / (vendor + ".json")).string())
        << R"({"version":")" << version << R"(","name":")" << vendor << R"(",)"
        << R"("machine_model_list":[{"name":"Test Model","sub_path":"machine/model.json"}],)"
        << R"("process_list":[{"name":"0.20mm Standard @)" << vendor << R"(","sub_path":"process/standard.json"}],)"
        << R"("filament_list":[)"
        << R"({"name":")" << vendor << R"( Base PLA","sub_path":"filament/base.json"},)"
        << R"({"name":")" << vendor << R"( PLA @0.4","sub_path":"filament/pla.json"},)"
        << R"({"name":")" << vendor << R"( Silk PLA @0.4","sub_path":"filament/silk.json"}],)"
        << R"("machine_list":[{"name":")" << vendor << R"( 0.4 nozzle","sub_path":"machine/printer.json"}]})";
    std::ofstream((dir / vendor / "machine" / "model.json").string())
        << R"({"type":"machine_model","name":"Test Model","nozzle_diameter":"0.4"})";
    std::ofstream((dir / vendor / "process" / "standard.json").string())
        << R"({"type":"process","name":"0.20mm Standard @)" << vendor
        << R"(","from":"system","instantiation":"true","layer_height":"0.2"})";
    std::ofstream((dir / vendor / "filament" / "base.json").string())
        << R"({"type":"filament","name":")" << vendor
        << R"( Base PLA","from":"system","instantiation":"false","filament_id":"GFA_base","filament_cost":"42"})";
    std::ofstream((dir / vendor / "filament" / "pla.json").string())
        << R"({"type":"filament","name":")" << vendor
        << R"( PLA @0.4","from":"system","instantiation":"true","filament_id":"GFA00","filament_cost":"20",)"
        << R"("setting_id":"GFSA04","description":"Test PLA description"})";
    std::ofstream((dir / vendor / "filament" / "silk.json").string())
        << R"({"type":"filament","name":")" << vendor
        << R"( Silk PLA @0.4","from":"system","instantiation":"true","inherits":")" << vendor << R"( Base PLA"})";
    std::ofstream((dir / vendor / "machine" / "printer.json").string())
        << R"({"type":"machine","name":")" << vendor
        << R"( 0.4 nozzle","from":"system","instantiation":"true","printer_model":"Test Model","printer_variant":"0.4",)"
        << R"("renamed_from":")" << vendor << R"( old 0.4 nozzle"})";
}

// The filament library: one non-instantiated base filament other vendors inherit
// from. `cost` lets a test bump the library and watch the change flow through.
void write_lib_tree(const fs::path& dir, const std::string& version, const std::string& cost)
{
    const std::string lib(PresetBundle::ORCA_FILAMENT_LIBRARY);
    fs::create_directories(dir / lib / "filament");
    std::ofstream((dir / (lib + ".json")).string())
        << R"({"version":")" << version << R"(","name":")" << lib << R"(",)"
        << R"("filament_list":[{"name":"Generic PLA","sub_path":"filament/generic_pla.json"}]})";
    std::ofstream((dir / lib / "filament" / "generic_pla.json").string())
        << R"({"type":"filament","name":"Generic PLA","from":"system","instantiation":"false",)"
        << R"("filament_id":"GFL99","filament_cost":")" << cost << R"("})";
}

// A vendor whose one filament inherits the library's base and states nothing of
// its own — everything it shows comes from the library it is resolved against.
void write_vendor_with_lib_filament(const fs::path& dir, const std::string& vendor, const std::string& version)
{
    fs::create_directories(dir / vendor / "filament");
    std::ofstream((dir / (vendor + ".json")).string())
        << R"({"version":")" << version << R"(","name":")" << vendor << R"(",)"
        << R"("filament_list":[{"name":")" << vendor << R"( PLA @0.4","sub_path":"filament/pla.json"}]})";
    std::ofstream((dir / vendor / "filament" / "pla.json").string())
        << R"({"type":"filament","name":")" << vendor
        << R"( PLA @0.4","from":"system","instantiation":"true","inherits":"Generic PLA"})";
}

std::string write_versionless_vendor_json(const fs::path& dir, const std::string& vendor_id)
{
    const fs::path p = dir / (vendor_id + ".json");
    std::ofstream f(p.string());
    f << R"({"name":")" << vendor_id << R"("})";
    return p.string();
}

// Whole file as bytes, for the byte-identity comparisons below.
std::string slurp(const fs::path& p)
{
    std::string s;
    load_string_file(p, s);
    return s;
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

// Patch cache_version (blob[0..3], i.e. file offset 20) and recompute CRC so the
// file passes the CRC check but fails the cache_version check in load_vendor_cache.
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

// Truncates the cache's PAYLOAD (everything after the 20-byte header) by
// `truncate_by` bytes and recomputes data_size/crc32 in the header, exactly
// as write_cache_blob computes them, so read_cache_blob's size and CRC checks
// still pass but cereal runs out of bytes partway through deserializing the
// body — exercising load_vendor_cache's catch block instead of its early
// (pre-body) rejection paths.
void truncate_payload_and_fix_header(const std::string& path, size_t truncate_by)
{
    constexpr size_t header_size = 20; // magic(4) + version(4) + data_size(8) + crc32(4)
    std::ifstream in(path, std::ios::binary);
    std::vector<char> data(std::istreambuf_iterator<char>(in), {});
    in.close();
    REQUIRE(data.size() > header_size + truncate_by);
    const size_t new_payload_size = data.size() - header_size - truncate_by;
    const uint64_t data_size_field = static_cast<uint64_t>(new_payload_size);
    boost::crc_32_type crc;
    crc.process_bytes(&data[header_size], new_payload_size);
    const uint32_t crc_field = crc.checksum();
    std::memcpy(&data[8],  &data_size_field, sizeof(data_size_field)); // data_size offset
    std::memcpy(&data[16], &crc_field,       sizeof(crc_field));       // crc32 offset
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(header_size + new_payload_size));
}

// One vendor as a cache's VendorMap. It carries one printer model ("Test Model",
// variant "0.4") so machine entries can pass install's model/variant validation.
VendorMap one_vendor(const std::string& vendor_id, const std::string& name = "",
                     Semver ver = Semver(1, 0, 0))
{
    VendorMap vendors;
    VendorProfile vp(vendor_id);
    vp.name           = name.empty() ? vendor_id + " Corp" : name;
    vp.config_version = ver;
    VendorProfile::PrinterModel model;
    model.id = "Test Model";
    model.variants.emplace_back(VendorProfile::PrinterVariant("0.4"));
    vp.models.push_back(model);
    vendors.emplace(vendor_id, vp);
    return vendors;
}

// Source-form entries as parse_subfile would emit them. The alias is derived by
// install from the '@' in the name, exactly as it is for the JSON parse.
PresetBundle::CachedPreset filament_entry(const std::string& name, const std::string& filament_id = "GFA00",
                                          const std::string& inherits = "")
{
    PresetBundle::CachedPreset e;
    e.name          = name;
    e.sub_path      = "filament/" + name + ".json";
    e.instantiation = "true";
    e.filament_id   = filament_id;
    e.inherits      = inherits;
    return e;
}

PresetBundle::CachedPreset printer_entry(const std::string& name)
{
    PresetBundle::CachedPreset e;
    e.name          = name;
    e.sub_path      = "machine/" + name + ".json";
    e.instantiation = "true";
    e.config_src.set_key_value("printer_model",   new ConfigOptionString("Test Model"));
    e.config_src.set_key_value("printer_variant", new ConfigOptionString("0.4"));
    return e;
}

static bool save_one_vendor(const std::string& path, const VendorMap& vendors,
                            const std::string& vendor, const std::string& vendor_version,
                            const std::vector<PresetBundle::CachedPreset>& filament_entries = {},
                            const std::vector<PresetBundle::CachedPreset>& machine_entries = {},
                            const std::vector<PresetBundle::CachedPreset>& process_entries = {})
{
    return PresetBundle::save_vendor_cache(path, vendor, vendor_version, vendors,
                                           process_entries, filament_entries, machine_entries, 0);
}

// resources_dir()/data_dir() are process-wide, so restore them however the test
// leaves — including through a failed REQUIRE — to stay green under --order rand.
struct ScopedDirs {
    std::string prev_data{data_dir()}, prev_rsrc{resources_dir()};
    ScopedDirs(const fs::path& data, const fs::path& rsrc)
    {
        set_data_dir(data.string());
        set_resources_dir(rsrc.string());
    }
    ~ScopedDirs() { set_data_dir(prev_data); set_resources_dir(prev_rsrc); }
};

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

namespace Slic3r {
inline bool operator==(const VendorProfile::PrinterVariant& a, const VendorProfile::PrinterVariant& b) { return a.name == b.name; }
inline bool operator==(const VendorProfile::PrinterModel& a, const VendorProfile::PrinterModel& b)
{
    return a.id == b.id && a.name == b.name && a.model_id == b.model_id && a.technology == b.technology
        && a.family == b.family && a.variants == b.variants && a.default_materials == b.default_materials
        && a.not_support_bed_types == b.not_support_bed_types && a.bed_model == b.bed_model
        && a.bed_texture == b.bed_texture && a.image_bed_type == b.image_bed_type
        && a.bottom_texture_end_name == b.bottom_texture_end_name
        && a.use_double_extruder_default_texture == b.use_double_extruder_default_texture
        && a.bottom_texture_rect == b.bottom_texture_rect
        && a.bottom_texture_rect_longer == b.bottom_texture_rect_longer
        && a.middle_texture_rect == b.middle_texture_rect && a.hotend_model == b.hotend_model;
}
} // namespace Slic3r

static bool vendor_deep_equal(const VendorProfile& a, const VendorProfile& b)
{
    return a.name == b.name && a.id == b.id && a.config_version == b.config_version
        && a.config_update_url == b.config_update_url && a.changelog_url == b.changelog_url
        && a.models == b.models && a.default_filaments == b.default_filaments
        && a.default_sla_materials == b.default_sla_materials;
}

static bool preset_deep_equal(const Preset& a, const Preset& b)
{
    return a.type == b.type && a.is_default == b.is_default && a.is_external == b.is_external
        && a.is_system == b.is_system && a.is_visible == b.is_visible && a.is_dirty == b.is_dirty
        && a.is_compatible == b.is_compatible && a.is_project_embedded == b.is_project_embedded
        && a.name == b.name && a.file == b.file && a.loaded == b.loaded
        && a.config.equals(b.config)
        && a.alias == b.alias && a.renamed_from == b.renamed_from
        && a.m_excluded_from == b.m_excluded_from && a.m_from_orca_filament_lib == b.m_from_orca_filament_lib
        && a.bundle_id == b.bundle_id && a.version == b.version && a.ini_str == b.ini_str
        && a.setting_id == b.setting_id && a.filament_id == b.filament_id && a.user_id == b.user_id
        && a.base_id == b.base_id && a.sync_info == b.sync_info && a.description == b.description
        && a.updated_time == b.updated_time && a.key_values == b.key_values;
}

TEST_CASE("a saved cache loads back with names, aliases and filament ids intact", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    REQUIRE(save_one_vendor(cache.string(), one_vendor(vid), vid, "1.0.0",
                            {filament_entry(vid + " PLA @0.4", "GFL_acme_pla")},
                            {printer_entry(vid + " Printer 0.4")}));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), vid, Semver("1.0.0")));
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

TEST_CASE("loading a missing cache file returns false", "[VendorCache]")
{
    TempDir tmp;
    PresetBundle out;
    REQUIRE(!out.load_vendor_cache((tmp.path / "nonexistent.opc").string(), "Acme", Semver("1.0.0")));
}

TEST_CASE("a cache with a corrupted byte is rejected by the CRC check", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    REQUIRE(save_one_vendor(cache.string(), one_vendor(vid), vid, "1.0.0",
                            {filament_entry(vid + " PLA")}));
    corrupt_blob_byte(cache.string());

    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), vid, Semver("1.0.0")));
}

TEST_CASE("two vendors produce two independent cache files", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cacheA = tmp.path / "vendorA.opc";
    const fs::path cacheB = tmp.path / "vendorB.opc";

    REQUIRE(save_one_vendor(cacheA.string(), one_vendor("VendorA"), "VendorA", "1.0.0",
                            {filament_entry("VendorA PLA")}));
    REQUIRE(save_one_vendor(cacheB.string(), one_vendor("VendorB"), "VendorB", "1.0.0",
                            {filament_entry("VendorB PLA")}));

    // Corrupt only vendor B's file; vendor A's must be unaffected.
    corrupt_blob_byte(cacheB.string());

    PresetBundle outA;
    REQUIRE(outA.load_vendor_cache(cacheA.string(), "VendorA", Semver("1.0.0")));
    REQUIRE(outA.vendors.count("VendorA") == 1);
    REQUIRE(presets_for(outA.filaments, "VendorA").size() == 1);

    PresetBundle outB;
    REQUIRE(!outB.load_vendor_cache(cacheB.string(), "VendorB", Semver("1.0.0")));
    REQUIRE(outB.vendors.empty());
}

TEST_CASE("vendor profile fields survive a cache round-trip", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    VendorMap vendors;
    VendorProfile vp(vid);
    vp.name           = "Acme Corporation";
    vp.config_version = Semver(2, 5, 1);
    VendorProfile::PrinterModel model;
    model.id   = "AcmePro";
    model.name = "Acme Pro";
    VendorProfile::PrinterVariant v0_4; v0_4.name = "0.4";
    model.variants.push_back(v0_4);
    vp.models.push_back(model);
    vendors.emplace(vid, vp);
    REQUIRE(save_one_vendor(cache.string(), vendors, vid, "2.5.1"));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), vid, Semver("2.5.1")));
    REQUIRE(out.vendors.count(vid) == 1);
    const VendorProfile& gvp = out.vendors.at(vid);
    REQUIRE(vendor_deep_equal(gvp, vendors.at(vid)));
    // Spot-check the fields the old test asserted directly, so a
    // vendor_deep_equal regression still points at what actually broke.
    CHECK(gvp.id   == vid);
    CHECK(gvp.name == "Acme Corporation");
    REQUIRE(gvp.models.size() == 1);
    CHECK(gvp.models[0].id   == "AcmePro");
    CHECK(gvp.models[0].name == "Acme Pro");
    REQUIRE(gvp.models[0].variants.size() == 1);
    CHECK(gvp.models[0].variants[0].name  == "0.4");
}

TEST_CASE("config option values survive a cache round-trip", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    auto entry = filament_entry(vid + " PETG @0.4");
    entry.config_src.set_key_value("filament_type", new ConfigOptionStrings({"PETG"}));
    REQUIRE(save_one_vendor(cache.string(), one_vendor(vid), vid, "1.0.0", {entry}));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), vid, Semver("1.0.0")));

    auto fi = presets_for(out.filaments, vid);
    REQUIRE(fi.size() == 1);
    const auto* ft = fi[0]->config.option<ConfigOptionStrings>("filament_type");
    REQUIRE(ft != nullptr);
    REQUIRE(ft->values.size() >= 1);
    CHECK(ft->values[0] == "PETG");
}

TEST_CASE("multiple presets in one collection all round-trip", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    const std::vector<std::string> fi_names = {vid + " PLA", vid + " PETG", vid + " ABS"};
    const std::vector<std::string> pr_names = {vid + " Printer 0.4", vid + " Printer 0.6"};
    std::vector<PresetBundle::CachedPreset> filament_entries, machine_entries;
    for (const auto& n : fi_names) filament_entries.push_back(filament_entry(n));
    for (const auto& n : pr_names) machine_entries.push_back(printer_entry(n));

    REQUIRE(save_one_vendor(cache.string(), one_vendor(vid), vid, "1.0.0",
                            filament_entries, machine_entries));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), vid, Semver("1.0.0")));

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

TEST_CASE("a truncated cache file is rejected", "[VendorCache]")
{
    TempDir        tmp;
    const fs::path cache = tmp.path / "truncated.opc";
    {
        std::ofstream f(cache.string(), std::ios::binary);
        const char data[] = {0x4F, 0x52, 0x43};
        f.write(data, sizeof(data));
    }
    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), "Acme", Semver("1.0.0")));
}

TEST_CASE("a cache with the wrong magic number is rejected", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    REQUIRE(save_one_vendor(cache.string(), one_vendor(vid), vid, "1.0.0",
                            {filament_entry(vid + " PLA")}));

    {
        std::fstream f(cache.string(), std::ios::in | std::ios::out | std::ios::binary);
        const uint32_t bad = 0xDEADBEEFu;
        f.write(reinterpret_cast<const char*>(&bad), sizeof(bad));
    }

    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), vid, Semver("1.0.0")));
}

TEST_CASE("a vendor with no presets saves and loads cleanly", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    REQUIRE(save_one_vendor(cache.string(), one_vendor(vid, "Acme Corporation"), vid, "1.0.0"));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), vid, Semver("1.0.0")));
    REQUIRE(out.vendors.count(vid) == 1);
    CHECK(out.vendors.at(vid).id   == vid);
    CHECK(out.vendors.at(vid).name == "Acme Corporation");
    CHECK(presets_for(out.filaments, vid).empty());
    CHECK(presets_for(out.printers,  vid).empty());
    CHECK(presets_for(out.prints,    vid).empty());
}

TEST_CASE("a cache-loaded vendor is indistinguishable from a JSON-loaded one", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);
    write_full_vendor_tree(user, "Acme", "1.0.0");

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");

    PresetBundle from_json;
    from_json.set_generate_vendor_caches(true);
    from_json.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                            ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(fs::exists(user / "Acme.opc"));

    // Take the preset JSONs away: were the cache rejected, the load below would
    // have nothing to parse — so its success proves the cache answered.
    fs::remove_all(user / "Acme");
    PresetBundle from_cache;
    from_cache.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                             ForwardCompatibilitySubstitutionRule::EnableSilent);

    // Both paths run the same install code over the same entries, so everything
    // observable must come out identical — the vendor profile and every preset,
    // field by field.
    REQUIRE(from_cache.vendors.count("Acme") == 1);
    REQUIRE(vendor_deep_equal(from_cache.vendors.at("Acme"), from_json.vendors.at("Acme")));
    const std::pair<const PresetCollection*, const PresetCollection*> colls[] = {
        {&from_json.prints,    &from_cache.prints},
        {&from_json.filaments, &from_cache.filaments},
        {&from_json.printers,  &from_cache.printers},
    };
    for (const auto& [jc, cc] : colls) {
        auto a = presets_for(*jc, "Acme");
        auto b = presets_for(*cc, "Acme");
        REQUIRE(a.size() == b.size());
        REQUIRE(!a.empty());
        for (size_t i = 0; i < a.size(); ++i) {
            CHECK(a[i]->name == b[i]->name);
            CHECK(preset_deep_equal(*a[i], *b[i]));
        }
    }

    // Pin the explicit metadata against symmetric loss: dropping a field from
    // CachedPreset::serialize keeps the two bundles equal to each other, but not
    // to the fixture.
    const Preset* pla = from_cache.filaments.find_preset("Acme PLA @0.4", false);
    REQUIRE(pla != nullptr);
    CHECK(pla->setting_id == "GFSA04");
    CHECK(pla->description == "Test PLA description");
    const Preset* silk = from_cache.filaments.find_preset("Acme Silk PLA @0.4", false);
    REQUIRE(silk != nullptr);
    CHECK(silk->filament_id == "GFA_base");   // inherited from the non-instantiated base
    const auto* cost = silk->config.option<ConfigOptionFloats>("filament_cost");
    REQUIRE(cost != nullptr);
    CHECK_THAT(cost->values.front(), WithinAbs(42., 1e-9));
    const Preset* pr = from_cache.printers.find_preset("Acme 0.4 nozzle", false);
    REQUIRE(pr != nullptr);
    CHECK(pr->renamed_from == std::vector<std::string>{"Acme old 0.4 nozzle"});
}

TEST_CASE("a cache-served vendor reports the errors its parse counted", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);
    // One process preset without the required "instantiation" key — a parse-phase
    // error the load survives, so it must reach the cache's parse_errors stamp.
    fs::create_directories(user / "Acme" / "process");
    std::ofstream((user / "Acme.json").string())
        << R"({"version":"1.0.0","name":"Acme","process_list":[{"name":"0.20mm Standard @Acme","sub_path":"process/standard.json"}]})";
    std::ofstream((user / "Acme" / "process" / "standard.json").string())
        << R"({"type":"process","name":"0.20mm Standard @Acme","from":"system","layer_height":"0.2"})";

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");

    PresetBundle from_json;
    from_json.set_generate_vendor_caches(true);
    from_json.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                            ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(fs::exists(user / "Acme.opc"));
    CHECK(from_json.error_count() > 0);

    fs::remove_all(user / "Acme");
    PresetBundle from_cache;
    from_cache.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                             ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(from_cache.error_count() == from_json.error_count());
    CHECK(presets_for(from_cache.prints, "Acme").size() == 1);
}

TEST_CASE("a non-instantiated base in a regular vendor's cache resolves its children and stays out of the library maps", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cache = tmp.path / "vendor.opc";

    // Entry order is the resolution order: the base must install (into the local
    // config maps) before the child that inherits it.
    auto base = filament_entry("Acme Base PLA", "GFA_base");
    base.instantiation = "false";
    base.config_src.set_key_value("filament_cost", new ConfigOptionFloats({42.}));
    auto child = filament_entry("Acme Silk PLA @0.4", "", "Acme Base PLA");
    REQUIRE(save_one_vendor(cache.string(), one_vendor("Acme"), "Acme", "1.0.0", {base, child}));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), "Acme", Semver("1.0.0")));
    auto fi = presets_for(out.filaments, "Acme");
    REQUIRE(fi.size() == 1);   // the base never becomes a preset
    CHECK(fi[0]->name == "Acme Silk PLA @0.4");
    CHECK(fi[0]->filament_id == "GFA_base");
    const auto* cost = fi[0]->config.option<ConfigOptionFloats>("filament_cost");
    REQUIRE(cost != nullptr);
    CHECK_THAT(cost->values.front(), WithinAbs(42., 1e-9));
    // Only the filament library's bases persist as the cross-vendor inheritance
    // maps; a regular vendor's stay local to its own load.
    CHECK(out.m_config_maps.empty());
    CHECK(out.m_filament_id_maps.empty());
}

TEST_CASE("a cache with the wrong cache version is rejected", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    REQUIRE(save_one_vendor(cache.string(), one_vendor(vid), vid, "1.0.0",
                            {filament_entry(vid + " PLA")}));
    patch_cache_version(cache.string(), 0xFFFFFFFFu);

    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), vid, Semver("1.0.0")));
}

TEST_CASE("a cache truncated mid-blob is rejected", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    REQUIRE(save_one_vendor(cache.string(), one_vendor(vid), vid, "1.0.0",
                            {filament_entry(vid + " PLA")}));

    {
        std::ifstream in(cache.string(), std::ios::binary);
        std::vector<char> buf(30); // 20-byte header + 10 bytes of blob
        in.read(buf.data(), 30);
        in.close();
        std::ofstream out(cache.string(), std::ios::binary | std::ios::trunc);
        out.write(buf.data(), 30);
    }

    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), vid, Semver("1.0.0")));
}

TEST_CASE("printer model bed texture fields survive a cache round-trip", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid   = "Acme";
    const fs::path     cache = tmp.path / "vendor.opc";

    VendorMap vendors = one_vendor(vid);
    VendorProfile::PrinterModel model;
    model.id                         = "N1";
    model.name                       = "Neat One";
    model.bottom_texture_rect_longer = "5,5,50,10";
    vendors.at(vid).models.push_back(model);
    REQUIRE(save_one_vendor(cache.string(), vendors, vid, "1.0.0"));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), vid, Semver("1.0.0")));
    REQUIRE(out.vendors.at(vid).models.size() == 2);
    REQUIRE(vendor_deep_equal(out.vendors.at(vid), vendors.at(vid)));
    CHECK(out.vendors.at(vid).models[1].bottom_texture_rect_longer == "5,5,50,10");
}

TEST_CASE("a cache older than the vendor profile on disk is rejected", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cache = tmp.path / "vendor.opc";
    REQUIRE(save_one_vendor(cache.string(), one_vendor("Acme"), "Acme", "1.0.0"));

    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), "Acme", Semver("1.0.1")));
}

TEST_CASE("a cache newer than the vendor profile on disk is used", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cache = tmp.path / "vendor.opc";
    REQUIRE(save_one_vendor(cache.string(), one_vendor("Acme"), "Acme", "1.2.0",
                            {filament_entry("Acme PLA")}));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), "Acme", Semver("1.0.0")));
    CHECK(presets_for(out.filaments, "Acme").size() == 1);
}

TEST_CASE("a vendor cache outlives a filament library update and resolves against the new library", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);
    const std::string lib(PresetBundle::ORCA_FILAMENT_LIBRARY);
    write_lib_tree(user, "1.0.0", "20");
    write_vendor_with_lib_filament(user, "Acme", "1.0.0");

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");

    // First launch: the library parses first, then the vendor against it, and
    // both caches are written.
    PresetBundle base1;
    base1.set_generate_vendor_caches(true);
    base1.load_vendor_configs_from_json(user.string(), lib, PresetBundle::LoadSystem,
                                        ForwardCompatibilitySubstitutionRule::EnableSilent);
    PresetBundle acme1;
    acme1.set_generate_vendor_caches(true);
    acme1.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                        ForwardCompatibilitySubstitutionRule::EnableSilent, &base1);
    REQUIRE(fs::exists(user / "Acme.opc"));
    {
        auto fi = presets_for(acme1.filaments, "Acme");
        REQUIRE(fi.size() == 1);
        const auto* cost = fi[0]->config.option<ConfigOptionFloats>("filament_cost");
        REQUIRE(cost != nullptr);
        CHECK_THAT(cost->values.front(), WithinAbs(20., 1e-9));
    }

    // An update delivers a new library only; the vendor stays as it was.
    write_lib_tree(user, "2.0.0", "30");
    PresetBundle base2;
    base2.load_vendor_configs_from_json(user.string(), lib, PresetBundle::LoadSystem,
                                        ForwardCompatibilitySubstitutionRule::EnableSilent);

    // Take the vendor's preset JSONs away: were its cache rejected, the load
    // below would have nothing to parse — so its success proves the cache
    // survived the library bump.
    fs::remove_all(user / "Acme");
    PresetBundle acme2;
    auto [substitutions, presets_loaded] = acme2.load_vendor_configs_from_json(
        user.string(), "Acme", PresetBundle::LoadSystem,
        ForwardCompatibilitySubstitutionRule::EnableSilent, &base2);
    CHECK(presets_loaded == 1);
    auto fi = presets_for(acme2.filaments, "Acme");
    REQUIRE(fi.size() == 1);
    // The cache holds only the vendor's own diff; the library values come from
    // the library loaded now, not the one in effect when the cache was written.
    const auto* cost = fi[0]->config.option<ConfigOptionFloats>("filament_cost");
    REQUIRE(cost != nullptr);
    CHECK_THAT(cost->values.front(), WithinAbs(30., 1e-9));
    CHECK(fi[0]->filament_id == "GFL99");
}

TEST_CASE("a vendor installed as its cache alone still loads after a library update", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);
    const std::string lib(PresetBundle::ORCA_FILAMENT_LIBRARY);
    write_lib_tree(user, "1.0.0", "20");
    write_vendor_with_lib_filament(user, "Acme", "1.0.0");

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");

    // Generate the vendor's cache, then strip the vendor to the cache alone —
    // the shape of a packaged install, which ships each vendor as its .opc and
    // nothing else.
    PresetBundle base1;
    base1.set_generate_vendor_caches(true);
    base1.load_vendor_configs_from_json(user.string(), lib, PresetBundle::LoadSystem,
                                        ForwardCompatibilitySubstitutionRule::EnableSilent);
    PresetBundle acme1;
    acme1.set_generate_vendor_caches(true);
    acme1.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                        ForwardCompatibilitySubstitutionRule::EnableSilent, &base1);
    fs::remove(user / "Acme.json");
    fs::remove_all(user / "Acme");

    // An OTA update then delivers a new library only. With no JSONs anywhere to
    // fall back on, the vendor must keep loading from its cache.
    write_lib_tree(user, "2.0.0", "30");
    PresetBundle base2;
    base2.load_vendor_configs_from_json(user.string(), lib, PresetBundle::LoadSystem,
                                        ForwardCompatibilitySubstitutionRule::EnableSilent);
    PresetBundle acme2;
    auto [substitutions, presets_loaded] = acme2.load_vendor_configs_from_json(
        user.string(), "Acme", PresetBundle::LoadSystem,
        ForwardCompatibilitySubstitutionRule::EnableSilent, &base2);
    CHECK(presets_loaded == 1);
    REQUIRE(acme2.vendors.count("Acme") == 1);
    auto fi = presets_for(acme2.filaments, "Acme");
    REQUIRE(fi.size() == 1);
    const auto* cost = fi[0]->config.option<ConfigOptionFloats>("filament_cost");
    REQUIRE(cost != nullptr);
    CHECK_THAT(cost->values.front(), WithinAbs(30., 1e-9));
}

TEST_CASE("a cache entry whose parent is missing falls back to the vendor's JSONs", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);
    write_vendor_tree(user, "Acme", "1.0.0");

    // A cache claiming the installed version, but whose entry inherits a preset
    // no loaded library provides.
    REQUIRE(save_one_vendor((user / "Acme.opc").string(), one_vendor("Acme", "Cached Acme"), "Acme", "1.0.0",
                            {filament_entry("Acme PLA @0.4", "GFA00", "No Such Base")}));

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");

    // Directly: the load fails and leaves the bundle clean.
    PresetBundle direct;
    REQUIRE(!direct.load_vendor_cache((user / "Acme.opc").string(), "Acme", Semver("1.0.0")));
    CHECK(direct.vendors.empty());

    // Through the vendor load: the JSONs answer instead, as if no cache existed.
    PresetBundle out;
    auto [substitutions, presets_loaded] = out.load_vendor_configs_from_json(
        user.string(), "Acme", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(presets_loaded == 1);
    CHECK(out.vendors.at("Acme").name == "Acme");   // the profile's name, not the cache's
}

TEST_CASE("a profile with no usable version is never served from cache", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cache = tmp.path / "vendor.opc";
    REQUIRE(save_one_vendor(cache.string(), one_vendor("Acme"), "Acme", "1.0.0"));

    PresetBundle out;
    // An unversioned vendor profile has no version to compare against.
    REQUIRE(!out.load_vendor_cache(cache.string(), "Acme", Semver::invalid()));
    // And a cache carrying no version of its own cannot cover a profile that has one.
    REQUIRE(save_one_vendor(cache.string(), one_vendor("Acme"), "Acme", ""));
    REQUIRE(!out.load_vendor_cache(cache.string(), "Acme", Semver("1.0.0")));
    REQUIRE(out.vendors.empty());
}

TEST_CASE("a versionless profile beside a cache keeps the cache from being served", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);

    REQUIRE(save_one_vendor((user / "Acme.opc").string(), one_vendor("Acme", "Cached Acme"), "Acme", "1.0.0",
                            {filament_entry("Acme PLA @0.4")}));
    // The profile beside the cache parses to no usable version, which can no
    // more judge the cache's staleness than it could be cached itself.
    write_versionless_vendor_json(user, "Acme");

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");

    PresetBundle out;
    auto [substitutions, presets_loaded] = out.load_vendor_configs_from_json(
        user.string(), "Acme", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    // Nothing came from the cache: the versionless profile was parsed instead,
    // and it carries no presets.
    CHECK(presets_loaded == 0);
}

TEST_CASE("a vendor's cache is its whole installation", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources";
    const fs::path data = tmp.path / "data";
    fs::create_directories(rsrc / "profiles" / "Acme" / "machine");
    write_vendor_json(rsrc / "profiles", "Acme");
    std::ofstream((rsrc / "profiles" / "Acme" / "machine" / "printer.json").string()) << "{}";

    REQUIRE(save_one_vendor((rsrc / "profiles" / "Acme.opc").string(), one_vendor("Acme"), "Acme", "1.0.0"));

    ScopedDirs dirs(data, rsrc);
    REQUIRE(install_vendor_bundles_from_resources({"Acme"}));
    // The cache carries the presets, the vendor profile and the version they were
    // built at, so it is installed on its own.
    CHECK(fs::exists(data / "system" / "Acme.opc"));
    CHECK(!fs::exists(data / "system" / "Acme.json"));
    CHECK(!fs::exists(data / "system" / "Acme"));
    CHECK(is_vendor_installed("Acme"));
    CHECK(installed_vendor_version("Acme") == Semver(1, 0, 0));

    // A vendor with no cache is installed as its profile and preset JSONs instead,
    // parsing them being the only way left to load it — and the cache the previous
    // install left behind has to go, or it would shadow the profile just installed.
    fs::remove(rsrc / "profiles" / "Acme.opc");
    REQUIRE(install_vendor_bundles_from_resources({"Acme"}));
    CHECK(!fs::exists(data / "system" / "Acme.opc"));
    CHECK(fs::exists(data / "system" / "Acme" / "machine" / "printer.json"));
    CHECK(installed_vendor_version("Acme") == Semver(1, 0, 0));

    // Installing the cache again takes the profile and its preset JSONs back out.
    REQUIRE(save_one_vendor((rsrc / "profiles" / "Acme.opc").string(), one_vendor("Acme"), "Acme", "1.0.0"));
    REQUIRE(install_vendor_bundles_from_resources({"Acme"}));
    CHECK(fs::exists(data / "system" / "Acme.opc"));
    CHECK(!fs::exists(data / "system" / "Acme.json"));
    CHECK(!fs::exists(data / "system" / "Acme"));
}

TEST_CASE("a vendor shipped as a cache alone is installed and loaded from it", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);

    // A packaged build: every vendor is its cache, with no profile of any kind
    // beside it — not even the filament library's.
    const std::string lib(PresetBundle::ORCA_FILAMENT_LIBRARY);
    REQUIRE(save_one_vendor((rsrc / (lib + ".opc")).string(), one_vendor(lib, "Shipped Library"), lib, "1.0.0"));
    REQUIRE(save_one_vendor((rsrc / "Acme.opc").string(), one_vendor("Acme", "Shipped Acme"), "Acme", "1.0.0"));

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");
    // The version the build ships the vendor at comes from the cache, there being
    // no profile to read it from.
    CHECK(resource_vendor_version("Acme") == Semver(1, 0, 0));

    // Nothing installed yet, so the shipped cache answers.
    PresetBundle before;
    before.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                         ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(before.vendors.at("Acme").name == "Shipped Acme");

    REQUIRE(install_vendor_bundles_from_resources({lib, "Acme"}));
    CHECK(fs::exists(user / "Acme.opc"));
    CHECK(!fs::exists(user / "Acme.json"));
    CHECK(installed_vendor_version("Acme") == Semver(1, 0, 0));

    PresetBundle after;
    after.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                        ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(after.vendors.at("Acme").name == "Shipped Acme");
}

TEST_CASE("a vendor cache installed in the data dir shadows the shipped one", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);

    write_vendor_tree(rsrc, "Acme", "1.0.0");
    REQUIRE(save_one_vendor((rsrc / "Acme.opc").string(), one_vendor("Acme", "Shipped Acme"), "Acme", "1.0.0"));

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");
    // The vendor profiles name the vendor "Acme"; the caches name it after where
    // they came from, so the loaded name says which source answered.
    auto loaded_name = [&user](PresetBundle& bundle) {
        bundle.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                             ForwardCompatibilitySubstitutionRule::EnableSilent);
        return bundle.vendors.at("Acme").name;
    };

    // Nothing installed yet: the shipped cache answers.
    PresetBundle from_rsrc;
    CHECK(loaded_name(from_rsrc) == "Shipped Acme");

    // Installing a newer vendor profile makes the shipped cache too old for it, even
    // though that cache still matches the profile sitting beside it in resources.
    write_vendor_tree(user, "Acme", "2.0.0");
    PresetBundle stale;
    CHECK(loaded_name(stale) == "Acme");

    // The cache installed alongside it does answer, and wins over the shipped one.
    REQUIRE(save_one_vendor((user / "Acme.opc").string(), one_vendor("Acme", "Installed Acme"), "Acme", "2.0.0"));
    PresetBundle from_user;
    CHECK(loaded_name(from_user) == "Installed Acme");
}

TEST_CASE("a cache installed with no profile beside it is used whatever its version", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);
    write_vendor_json(rsrc, "Acme");

    // Installed at an older version than the one now shipped in resources. Nothing
    // sits beside it claiming to be newer, so the cache is what the vendor is.
    REQUIRE(save_one_vendor((user / "Acme.opc").string(), one_vendor("Acme", "Installed Acme"), "Acme", "0.9.0"));

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");
    CHECK(PresetBundle::peek_vendor_cache_version((user / "Acme.opc").string(), "Acme") == "0.9.0");
    CHECK(PresetBundle::peek_vendor_cache_version((user / "Acme.opc").string(), "Other").empty());
    CHECK(installed_vendor_version("Acme") == Semver(0, 9, 0));

    // Loading the vendor takes the installed cache, not the newer shipped profile.
    PresetBundle out;
    out.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                      ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(out.vendors.at("Acme").name == "Installed Acme");
}

TEST_CASE("a vendor whose cache covers it is loaded without parsing any JSON", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);

    REQUIRE(save_one_vendor((user / "Acme.opc").string(), one_vendor("Acme", "Cached Acme"), "Acme", "1.0.0",
                            {filament_entry("Acme PLA @0.4")},
                            {printer_entry("Acme Printer 0.4")}));

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");

    // The cache is the whole installation — no profile, no preset JSONs — and the
    // caller asks for the vendor exactly as it would for a JSON install.
    PresetBundle out;
    auto [substitutions, presets_loaded] = out.load_vendor_configs_from_json(
        user.string(), "Acme", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::Disable);
    CHECK(substitutions.empty());
    CHECK(presets_loaded == 2);
    CHECK(out.vendors.at("Acme").name == "Cached Acme");

    // Nothing was written back: the presets never came from a parse.
    CHECK(!fs::exists(user / "Acme.json"));
}

TEST_CASE("a vendor whose cache is stale falls back to parsing its JSONs", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);

    // An update installed the vendor at 2.0.0; the cache next to it was built from
    // the profile before that, so it no longer covers what is on disk.
    write_vendor_tree(user, "Acme", "2.0.0");
    REQUIRE(save_one_vendor((user / "Acme.opc").string(), one_vendor("Acme", "Cached Acme"), "Acme", "1.0.0"));

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");

    PresetBundle out;
    auto [substitutions, presets_loaded] = out.load_vendor_configs_from_json(
        user.string(), "Acme", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(presets_loaded == 1);
    CHECK(out.vendors.at("Acme").config_version == Semver(2, 0, 0));

    // A one-off parse like this one leaves the stale cache alone: only a bundle
    // told its parses are complete writes one.
    CHECK(PresetBundle::peek_vendor_cache_version((user / "Acme.opc").string(), "Acme") == "1.0.0");

    PresetBundle caching;
    caching.set_generate_vendor_caches(true);
    caching.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(PresetBundle::peek_vendor_cache_version((user / "Acme.opc").string(), "Acme")
          == get_version_from_json((user / "Acme.json").string()).to_string());
}

TEST_CASE("a vendor with nothing installed is parsed from the shipped profiles", "[VendorCache]")
{
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(user);
    fs::create_directories(rsrc);
    write_vendor_tree(rsrc, "Acme", "1.0.0");

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");

    // Neither a cache nor a profile in the directory asked for, so the vendor comes
    // out of resources — which is where a build that ships caches keeps the JSONs a
    // rejected cache has to be re-parsed from.
    PresetBundle out;
    auto [substitutions, presets_loaded] = out.load_vendor_configs_from_json(
        user.string(), "Acme", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(presets_loaded == 1);
    CHECK(out.vendors.at("Acme").config_version == Semver(1, 0, 0));

    // The cache such a parse writes lands in the directory asked for, stamped with
    // the version of the profile it was actually built from.
    PresetBundle caching;
    caching.set_generate_vendor_caches(true);
    caching.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(PresetBundle::peek_vendor_cache_version((user / "Acme.opc").string(), "Acme")
          == get_version_from_json((rsrc / "Acme.json").string()).to_string());
}

TEST_CASE("a cache with a mismatched vendor name is rejected", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cache = tmp.path / "vendor.opc";
    REQUIRE(save_one_vendor(cache.string(), one_vendor("VendorA"), "VendorA", "1.0.0"));

    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(cache.string(), "VendorB", Semver("1.0.0")));
}

TEST_CASE("a cache is rejected against an unparsable version", "[VendorCache]")
{
    TempDir tmp;
    const fs::path cache = tmp.path / "vendor.opc";
    REQUIRE(save_one_vendor(cache.string(), one_vendor("Acme"), "Acme", "1.0.0"));
    PresetBundle out;
    // A profile version that does not parse comes out of get_version_from_json
    // as zero, which cannot be judged any more than Semver::invalid() can.
    REQUIRE(!out.load_vendor_cache(cache.string(), "Acme", Semver()));
    REQUIRE(out.vendors.empty());   // rejection happens before the body is touched
}

TEST_CASE("the filament library's inheritance maps are rebuilt on cache load", "[VendorCache]")
{
    // m_config_maps/m_filament_id_maps are the inheritance base other vendors
    // resolve against. The cache no longer stores them: they are rebuilt by
    // installing the library's entries — including the non-instantiated bases,
    // which exist for exactly this and never become presets.
    TempDir tmp;
    const fs::path    cache = tmp.path / "lib.opc";
    const std::string lib(PresetBundle::ORCA_FILAMENT_LIBRARY);

    auto base = filament_entry("Generic PLA", "GFL99");
    base.instantiation = "false";
    base.config_src.set_key_value("filament_cost", new ConfigOptionFloats({20.}));
    REQUIRE(save_one_vendor(cache.string(), one_vendor(lib), lib, "1.0.0", {base}));

    PresetBundle out;
    REQUIRE(out.load_vendor_cache(cache.string(), lib, Semver("1.0.0")));
    REQUIRE(out.m_config_maps.count("Generic PLA") == 1);
    const auto* cost = out.m_config_maps.at("Generic PLA").option<ConfigOptionFloats>("filament_cost");
    REQUIRE(cost != nullptr);
    CHECK_THAT(cost->values.front(), WithinAbs(20., 1e-9));
    CHECK(out.m_filament_id_maps.at("Generic PLA") == "GFL99");
    CHECK(presets_for(out.filaments, lib).empty());   // not instantiated, not a preset
}

TEST_CASE("the same fixture parsed twice serializes byte-identically", "[VendorCache]")
{
    // Shipped caches must be reproducible: the same profiles must produce the
    // same bytes on every machine that generates them.
    TempDir tmp;
    const fs::path rsrc = tmp.path / "resources" / "profiles";
    const fs::path user = tmp.path / "data" / PRESET_SYSTEM_DIR;
    fs::create_directories(rsrc);
    fs::create_directories(user);
    write_full_vendor_tree(user, "Acme", "1.0.0");

    ScopedDirs dirs(tmp.path / "data", tmp.path / "resources");

    PresetBundle first;
    first.set_generate_vendor_caches(true);
    first.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                        ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(fs::exists(user / "Acme.opc"));
    const std::string bytes1 = slurp(user / "Acme.opc");
    fs::remove(user / "Acme.opc");

    PresetBundle second;
    second.set_generate_vendor_caches(true);
    second.load_vendor_configs_from_json(user.string(), "Acme", PresetBundle::LoadSystem,
                                         ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(slurp(user / "Acme.opc") == bytes1);
}

TEST_CASE("a cache that fails mid-body deserialization is rejected and leaves the bundle clean", "[VendorCache]")
{
    TempDir           tmp;
    const std::string vid           = "Acme";
    const fs::path    valid_cache   = tmp.path / "valid.opc";
    const fs::path    corrupt_cache = tmp.path / "corrupt.opc";

    REQUIRE(save_one_vendor(valid_cache.string(), one_vendor(vid), vid, "1.0.0",
                            {filament_entry(vid + " PLA @0.4")},
                            {printer_entry(vid + " Printer 0.4")}));
    // Truncate the tail (machine entries + parse_errors, per save_vendor_cache's
    // field order) so the header's size/CRC still validate but cereal runs out of
    // bytes partway through the body. Grow the cut if a given size ever stops
    // throwing (e.g. after an unrelated field-order change to the cache format).
    size_t truncate_by = 40;
    bool   throws      = false;
    for (; truncate_by <= 200; truncate_by += 8) {
        fs::copy_file(valid_cache, corrupt_cache, fs::copy_option::overwrite_if_exists);
        truncate_payload_and_fix_header(corrupt_cache.string(), truncate_by);
        PresetBundle probe_bundle;
        if (!probe_bundle.load_vendor_cache(corrupt_cache.string(), vid, Semver("1.0.0"))) {
            throws = true;
            break;
        }
    }
    REQUIRE(throws);

    PresetBundle out;
    REQUIRE(!out.load_vendor_cache(corrupt_cache.string(), vid, Semver("1.0.0")));
    // The catch block put the bundle back the way a failed parse would leave it.
    CHECK(out.vendors.empty());
    CHECK(out.m_config_maps.empty());
    CHECK(presets_for(out.filaments, vid).empty());

    // The recovery must leave a bundle a caller can still load a good cache into.
    REQUIRE(out.load_vendor_cache(valid_cache.string(), vid, Semver("1.0.0")));
    CHECK(out.vendors.count(vid) == 1);
    CHECK(presets_for(out.filaments, vid).size() == 1);
}
