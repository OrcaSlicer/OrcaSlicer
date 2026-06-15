#include "PresetBundleCache.hpp"

#include <sstream>

#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <cereal/archives/binary.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/polymorphic.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include "PresetBundle.hpp"
#include "PrintConfig.hpp"
#include "Semver.hpp"
#include "Utils.hpp"

namespace Slic3r {
namespace PresetBundleCache {

// -------------------------------------------------------------------------
// Binary cache file format: raw 20-byte header followed by cereal blob.
// -------------------------------------------------------------------------
static constexpr uint32_t CACHE_MAGIC        = 0x4F52435A; // "ORCZ"
static constexpr uint32_t CACHE_FILE_VERSION = 1;

#pragma pack(push, 1)
struct CacheFileHeader {
    uint32_t magic;
    uint32_t file_version;
    uint64_t data_size;
    uint32_t crc32;
};
#pragma pack(pop)
static_assert(sizeof(CacheFileHeader) == 20, "CacheFileHeader must be 20 bytes");

template<class T>
static void save_blob(const std::string& path, const T& obj)
{
    std::ostringstream oss(std::ios::out | std::ios::binary);
    {
        cereal::BinaryOutputArchive ar(oss);
        ar(obj);
    }
    const std::string blob = oss.str();
    boost::crc_32_type crc;
    crc.process_bytes(blob.data(), blob.size());
    try {
        boost::filesystem::create_directories(boost::filesystem::path(path).parent_path());
        boost::nowide::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            BOOST_LOG_TRIVIAL(warning) << "PresetBundleCache: cannot open for writing: " << path;
            return;
        }
        CacheFileHeader hdr;
        hdr.magic        = CACHE_MAGIC;
        hdr.file_version = CACHE_FILE_VERSION;
        hdr.data_size    = static_cast<uint64_t>(blob.size());
        hdr.crc32        = crc.checksum();
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        ofs.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "PresetBundleCache: write failed (" << path << "): " << e.what();
    }
}

template<class T>
static bool load_blob(const std::string& path, T& obj)
{
    try {
        boost::nowide::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open())
            return false;
        CacheFileHeader hdr;
        if (!ifs.read(reinterpret_cast<char*>(&hdr), sizeof(hdr)))
            return false;
        if (hdr.magic != CACHE_MAGIC || hdr.file_version != CACHE_FILE_VERSION)
            return false;
        if (hdr.data_size == 0 || hdr.data_size > 512u * 1024u * 1024u)
            return false;
        std::string blob(hdr.data_size, '\0');
        if (!ifs.read(&blob[0], static_cast<std::streamsize>(hdr.data_size)))
            return false;
        boost::crc_32_type crc;
        crc.process_bytes(blob.data(), blob.size());
        if (crc.checksum() != hdr.crc32) {
            BOOST_LOG_TRIVIAL(warning) << "PresetBundleCache: CRC32 mismatch: " << path;
            return false;
        }
        std::istringstream iss(blob, std::ios::in | std::ios::binary);
        cereal::BinaryInputArchive ar(iss);
        ar(obj);
        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "PresetBundleCache: load failed (" << path << "): " << e.what();
        return false;
    }
}

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------
static std::string vendor_root_json(const std::string& system_dir, const std::string& vendor_id)
{
    return (boost::filesystem::path(system_dir) / (vendor_id + ".json")).make_preferred().string();
}

// -------------------------------------------------------------------------
// SystemPresetsCache
// -------------------------------------------------------------------------
std::string SystemPresetsCache::cache_path()
{
    return (boost::filesystem::path(data_dir()) / PRESET_SYSTEM_DIR / "system_presets_cache.cache")
               .make_preferred().string();
}

std::string SystemPresetsCache::bundled_cache_path()
{
    return (boost::filesystem::path(resources_dir()) / "profiles" / "system_presets_cache.cache")
               .make_preferred().string();
}

bool SystemPresetsCache::is_valid(const std::string& system_dir) const
{
    if (format_version != FORMAT_VERSION)
        return false;
    if (config_options_count != print_config_def.options.size())
        return false;
    std::map<std::string, std::string> current;
    try {
        for (const auto& entry : boost::filesystem::directory_iterator(system_dir)) {
            const std::string path = entry.path().string();
            if (!Slic3r::is_json_file(path))
                continue;
            const std::string vendor_name = entry.path().stem().string();
            Semver ver = get_version_from_json(path);
            if (ver.valid())
                current[vendor_name] = ver.to_string();
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "PresetBundleCache: directory scan failed: " << e.what();
        return false;
    }
    if (current.size() != vendor_versions.size())
        return false;
    for (const auto& [name, ver] : current) {
        auto it = vendor_versions.find(name);
        if (it == vendor_versions.end() || it->second != ver)
            return false;
    }
    return true;
}

void SystemPresetsCache::capture(const PresetBundle& bundle, const std::string& system_dir)
{
    format_version       = FORMAT_VERSION;
    config_options_count = print_config_def.options.size();
    vendor_versions.clear();
    vendor_profiles.clear();
    print_presets.clear();
    filament_presets.clear();
    printer_presets.clear();
    sla_print_presets.clear();
    sla_material_presets.clear();
    config_maps      = bundle.m_config_maps;
    filament_id_maps = bundle.m_filament_id_maps;

    for (const auto& [id, vp] : bundle.vendors) {
        CachedVendorProfile cvp;
        cvp.id                = vp.id;
        cvp.name              = vp.name;
        cvp.config_version    = vp.config_version.valid() ? vp.config_version.to_string() : "";
        cvp.config_update_url = vp.config_update_url;
        cvp.changelog_url     = vp.changelog_url;
        for (const auto& model : vp.models) {
            CachedPrinterModel cm;
            cm.id         = model.id;
            cm.name       = model.name;
            cm.model_id   = model.model_id;
            cm.family     = model.family;
            cm.technology = static_cast<int>(model.technology);
            for (const auto& v : model.variants)
                cm.variants.push_back({v.name});
            cm.default_materials                   = model.default_materials;
            cm.not_support_bed_types               = model.not_support_bed_types;
            cm.bed_model                           = model.bed_model;
            cm.bed_texture                         = model.bed_texture;
            cm.image_bed_type                      = model.image_bed_type;
            cm.bottom_texture_end_name             = model.bottom_texture_end_name;
            cm.use_double_extruder_default_texture = model.use_double_extruder_default_texture;
            cm.bottom_texture_rect                 = model.bottom_texture_rect;
            cm.middle_texture_rect                 = model.middle_texture_rect;
            cm.hotend_model                        = model.hotend_model;
            cvp.models.push_back(std::move(cm));
        }
        for (const auto& f : vp.default_filaments)
            cvp.default_filaments.push_back(f);
        for (const auto& m : vp.default_sla_materials)
            cvp.default_sla_materials.push_back(m);
        vendor_profiles.push_back(std::move(cvp));
        Semver ver = get_version_from_json(vendor_root_json(system_dir, id));
        vendor_versions[id] = ver.valid() ? ver.to_string() : "";
    }

    auto capture_col = [](const PresetCollection& coll, std::vector<CachedPreset>& out) {
        for (const Preset& p : coll()) {
            if (!p.is_system)
                continue;
            CachedPreset cp;
            cp.type                     = static_cast<int>(p.type);
            cp.name                     = p.name;
            cp.alias                    = p.alias;
            cp.file                     = p.file;
            cp.version                  = p.version.valid() ? p.version.to_string() : "";
            cp.vendor_id                = (p.vendor != nullptr) ? p.vendor->id : "";
            cp.filament_id              = p.filament_id;
            cp.setting_id               = p.setting_id;
            cp.description              = p.description;
            cp.renamed_from             = p.renamed_from;
            cp.is_system                = p.is_system;
            cp.is_visible               = p.is_visible;
            cp.m_from_orca_filament_lib = p.m_from_orca_filament_lib;
            cp.config                   = p.config;
            out.push_back(std::move(cp));
        }
    };
    capture_col(bundle.prints,        print_presets);
    capture_col(bundle.filaments,     filament_presets);
    capture_col(bundle.printers,      printer_presets);
    capture_col(bundle.sla_prints,    sla_print_presets);
    capture_col(bundle.sla_materials, sla_material_presets);
}

void SystemPresetsCache::apply(PresetBundle& bundle) const
{
    bundle.reset(false);
    for (const auto& cvp : vendor_profiles) {
        VendorProfile vp(cvp.id);
        vp.name              = cvp.name;
        vp.config_update_url = cvp.config_update_url;
        vp.changelog_url     = cvp.changelog_url;
        if (!cvp.config_version.empty()) {
            auto v = Semver::parse(cvp.config_version);
            if (v) vp.config_version = *v;
        }
        for (const auto& cm : cvp.models) {
            VendorProfile::PrinterModel model;
            model.id         = cm.id;
            model.name       = cm.name;
            model.model_id   = cm.model_id;
            model.family     = cm.family;
            model.technology = static_cast<PrinterTechnology>(cm.technology);
            for (const auto& v : cm.variants)
                model.variants.emplace_back(v.name);
            model.default_materials                   = cm.default_materials;
            model.not_support_bed_types               = cm.not_support_bed_types;
            model.bed_model                           = cm.bed_model;
            model.bed_texture                         = cm.bed_texture;
            model.image_bed_type                      = cm.image_bed_type;
            model.bottom_texture_end_name             = cm.bottom_texture_end_name;
            model.use_double_extruder_default_texture = cm.use_double_extruder_default_texture;
            model.bottom_texture_rect                 = cm.bottom_texture_rect;
            model.middle_texture_rect                 = cm.middle_texture_rect;
            model.hotend_model                        = cm.hotend_model;
            vp.models.push_back(std::move(model));
        }
        for (const auto& f : cvp.default_filaments)
            vp.default_filaments.insert(f);
        for (const auto& m : cvp.default_sla_materials)
            vp.default_sla_materials.insert(m);
        bundle.vendors.emplace(cvp.id, std::move(vp));
    }

    auto apply_col = [&bundle](const std::vector<CachedPreset>& cached,
                               PresetCollection&                coll,
                               bool                             is_filaments) {
        for (const auto& cp : cached) {
            Semver version;
            if (!cp.version.empty()) {
                auto v = Semver::parse(cp.version);
                if (v) version = *v;
            }
            DynamicPrintConfig config = cp.config;
            Preset& p = coll.load_preset(cp.file, cp.name, std::move(config), /*select=*/false, version);
            p.is_system               = true;
            p.is_visible              = cp.is_visible;
            p.alias                   = cp.alias;
            p.renamed_from            = cp.renamed_from;
            p.filament_id             = cp.filament_id;
            p.setting_id              = cp.setting_id;
            p.description             = cp.description;
            p.m_from_orca_filament_lib = cp.m_from_orca_filament_lib;
            if (!cp.vendor_id.empty()) {
                auto it = bundle.vendors.find(cp.vendor_id);
                if (it != bundle.vendors.end())
                    p.vendor = &it->second;
            }
            if (is_filaments)
                coll.set_printer_hold_alias(p.alias, p);
        }
    };
    apply_col(print_presets,        bundle.prints,        false);
    apply_col(filament_presets,     bundle.filaments,     true);
    apply_col(printer_presets,      bundle.printers,      false);
    apply_col(sla_print_presets,    bundle.sla_prints,    false);
    apply_col(sla_material_presets, bundle.sla_materials, false);

    bundle.m_config_maps      = config_maps;
    bundle.m_filament_id_maps = filament_id_maps;
    // Caller must invoke bundle.update_system_maps() after this (it is private to PresetBundle).
}

bool SystemPresetsCache::load(const std::string& path)
{
    return load_blob(path, *this);
}

void SystemPresetsCache::save(const std::string& path) const
{
    save_blob(path, *this);
}

} // namespace PresetBundleCache
} // namespace Slic3r
