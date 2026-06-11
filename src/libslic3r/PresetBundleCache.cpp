#include "PresetBundleCache.hpp"

#include <chrono>
#include <sstream>

#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <cereal/archives/binary.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/polymorphic.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include "Preset.hpp"
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
        cvp.id               = vp.id;
        cvp.name             = vp.name;
        cvp.config_version   = vp.config_version.valid() ? vp.config_version.to_string() : "";
        cvp.config_update_url = vp.config_update_url;
        cvp.changelog_url    = vp.changelog_url;

        for (const auto& model : vp.models) {
            CachedPrinterModel cm;
            cm.id         = model.id;
            cm.name       = model.name;
            cm.model_id   = model.model_id;
            cm.family     = model.family;
            cm.technology = static_cast<int>(model.technology);
            for (const auto& v : model.variants)
                cm.variants.push_back({v.name});
            cm.default_materials           = model.default_materials;
            cm.not_support_bed_types       = model.not_support_bed_types;
            cm.bed_model                   = model.bed_model;
            cm.bed_texture                 = model.bed_texture;
            cm.image_bed_type              = model.image_bed_type;
            cm.bottom_texture_end_name     = model.bottom_texture_end_name;
            cm.use_double_extruder_default_texture = model.use_double_extruder_default_texture;
            cm.bottom_texture_rect         = model.bottom_texture_rect;
            cm.middle_texture_rect         = model.middle_texture_rect;
            cm.hotend_model                = model.hotend_model;
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
            cp.type                    = static_cast<int>(p.type);
            cp.name                    = p.name;
            cp.alias                   = p.alias;
            cp.file                    = p.file;
            cp.version                 = p.version.valid() ? p.version.to_string() : "";
            cp.vendor_id               = (p.vendor != nullptr) ? p.vendor->id : "";
            cp.filament_id             = p.filament_id;
            cp.setting_id              = p.setting_id;
            cp.description             = p.description;
            cp.renamed_from            = p.renamed_from;
            cp.is_system               = p.is_system;
            cp.is_visible              = p.is_visible;
            cp.m_from_orca_filament_lib = p.m_from_orca_filament_lib;
            cp.config                  = p.config;
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
            model.default_materials           = cm.default_materials;
            model.not_support_bed_types       = cm.not_support_bed_types;
            model.bed_model                   = cm.bed_model;
            model.bed_texture                 = cm.bed_texture;
            model.image_bed_type              = cm.image_bed_type;
            model.bottom_texture_end_name     = cm.bottom_texture_end_name;
            model.use_double_extruder_default_texture = cm.use_double_extruder_default_texture;
            model.bottom_texture_rect         = cm.bottom_texture_rect;
            model.middle_texture_rect         = cm.middle_texture_rect;
            model.hotend_model                = cm.hotend_model;
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
            p.is_system              = true;
            p.is_visible             = cp.is_visible;
            p.alias                  = cp.alias;
            p.renamed_from           = cp.renamed_from;
            p.filament_id            = cp.filament_id;
            p.setting_id             = cp.setting_id;
            p.description            = cp.description;
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

// -------------------------------------------------------------------------
// PresetFileCache
// -------------------------------------------------------------------------

std::string PresetFileCache::cache_path_for(const std::string& json_path)
{
    boost::filesystem::path p(json_path);
    p.replace_extension(".cache");
    return p.string();
}

bool PresetFileCache::is_valid(const std::string& json_path) const
{
    if (format_version != FORMAT_VERSION)
        return false;
    if (config_options_count != print_config_def.options.size())
        return false;

    namespace fs = boost::filesystem;
    const fs::path json(json_path);
    if (!fs::exists(json))
        return false;

    int64_t cur_json_mtime = 0;
    try {
        cur_json_mtime = static_cast<int64_t>(fs::last_write_time(json));
    } catch (...) { return false; }
    if (json_mtime != cur_json_mtime)
        return false;

    fs::path info = json;
    info.replace_extension(".info");
    int64_t cur_info_mtime = 0;
    if (fs::exists(info)) {
        try {
            cur_info_mtime = static_cast<int64_t>(fs::last_write_time(info));
        } catch (...) { return false; }
    }
    return info_mtime == cur_info_mtime;
}

// -------------------------------------------------------------------------
// preload_file_caches / update_file_caches
// -------------------------------------------------------------------------

int preload_file_caches(PresetCollection& coll, const std::string& dir_path,
                        const PresetOrigin& origin)
{
    namespace fs = boost::filesystem;
    const fs::path dir(dir_path);
    if (!fs::exists(dir))
        return 0;

    // Step 1: collect .json paths (fast, sequential).
    std::vector<std::string> json_paths;
    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (fs::is_regular_file(entry.path()) &&
                    entry.path().extension().string() == ".json")
                json_paths.push_back(entry.path().string());
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "PresetBundleCache: dir scan failed ("
                                   << dir_path << "): " << e.what();
        return 0;
    }

    // Step 2: load + deserialize each .cache file in parallel.
    // Each slot is independent - no shared state touched here.
    struct LoadResult {
        std::string      json_path;
        std::string      canonical_name;
        PresetFileCache  pfc;
        bool             valid = false;
    };
    std::vector<LoadResult> results(json_paths.size());

    tbb::parallel_for(tbb::blocked_range<size_t>(0, json_paths.size()),
        [&](const tbb::blocked_range<size_t>& range) {
            for (size_t i = range.begin(); i < range.end(); ++i) {
                const std::string& json_path  = json_paths[i];
                const std::string  cache_path = PresetFileCache::cache_path_for(json_path);

                PresetFileCache pfc;
                if (!load_blob(cache_path, pfc) || !pfc.is_valid(json_path))
                    continue;

                const std::string stem = fs::path(json_path).stem().string();
                results[i].json_path      = json_path;
                results[i].canonical_name = get_preset_canonical_name(stem, origin);
                results[i].pfc            = std::move(pfc);
                results[i].valid          = true;
            }
        });

    // Step 3: sequential merge into the collection (load_preset uses a lock).
    const bool is_filaments = (coll.type() == Preset::TYPE_FILAMENT);
    int loaded = 0;
    for (auto& r : results) {
        if (!r.valid)
            continue;

        Semver version;
        if (!r.pfc.preset.version.empty()) {
            auto v = Semver::parse(r.pfc.preset.version);
            if (v) version = *v;
        }

        DynamicPrintConfig config = r.pfc.preset.config;
        Preset& p = coll.load_preset(r.json_path, r.canonical_name,
                                     std::move(config), /*select=*/false, version);
        p.is_system    = false;
        p.is_visible   = r.pfc.preset.is_visible;
        p.alias        = r.pfc.preset.alias;
        p.filament_id  = r.pfc.preset.filament_id;
        p.setting_id   = r.pfc.preset.setting_id;
        p.description  = r.pfc.preset.description;
        p.base_id      = r.pfc.preset.base_id;
        p.user_id      = r.pfc.preset.user_id;
        p.sync_info    = r.pfc.preset.sync_info;
        p.updated_time = r.pfc.preset.updated_time;
        p.renamed_from = r.pfc.preset.renamed_from;
        p.bundle_id    = origin.bundle_id;

        if (is_filaments)
            coll.set_printer_hold_alias(p.alias, p);

        ++loaded;
    }
    return loaded;
}

void update_file_caches(const PresetCollection& coll, const std::string& dir_path)
{
    namespace fs = boost::filesystem;
    const std::string norm_dir = fs::path(dir_path).make_preferred().string();

    // Step 1: collect candidate presets (sequential, fast).
    std::vector<const Preset*> candidates;
    for (const Preset& p : coll()) {
        if (p.is_system || p.is_default || p.file.empty())
            continue;
        const std::string norm_file = fs::path(p.file).make_preferred().string();
        if (norm_file.rfind(norm_dir, 0) != 0)
            continue;
        candidates.push_back(&p);
    }

    // Step 2: check + write each .cache file in parallel.
    // Each write targets an independent file path - no shared state.
    tbb::parallel_for(tbb::blocked_range<size_t>(0, candidates.size()),
        [&](const tbb::blocked_range<size_t>& range) {
            for (size_t i = range.begin(); i < range.end(); ++i) {
                const Preset& p          = *candidates[i];
                const std::string cache_path = PresetFileCache::cache_path_for(p.file);

                PresetFileCache existing;
                if (load_blob(cache_path, existing) && existing.is_valid(p.file))
                    continue;

                PresetFileCache pfc;
                pfc.format_version       = PresetFileCache::FORMAT_VERSION;
                pfc.config_options_count = print_config_def.options.size();

                try {
                    pfc.json_mtime = static_cast<int64_t>(
                        fs::last_write_time(fs::path(p.file)));
                } catch (...) {}

                fs::path info_path = fs::path(p.file);
                info_path.replace_extension(".info");
                if (fs::exists(info_path)) {
                    try {
                        pfc.info_mtime = static_cast<int64_t>(
                            fs::last_write_time(info_path));
                    } catch (...) {}
                }

                CachedPreset& cp    = pfc.preset;
                cp.type             = static_cast<int>(p.type);
                cp.name             = p.name;
                cp.alias            = p.alias;
                cp.file             = p.file;
                cp.version          = p.version.valid() ? p.version.to_string() : "";
                cp.filament_id      = p.filament_id;
                cp.setting_id       = p.setting_id;
                cp.description      = p.description;
                cp.base_id          = p.base_id;
                cp.user_id          = p.user_id;
                cp.sync_info        = p.sync_info;
                cp.updated_time     = p.updated_time;
                cp.renamed_from     = p.renamed_from;
                cp.is_system        = false;
                cp.is_visible       = p.is_visible;
                cp.config           = p.config;

                save_blob(cache_path, pfc);
            }
        });
}

void write_file_cache(const Preset& preset)
{
    if (preset.is_system || preset.is_default || preset.is_project_embedded || preset.file.empty())
        return;

    namespace fs = boost::filesystem;
    const std::string cache_path = PresetFileCache::cache_path_for(preset.file);

    PresetFileCache pfc;
    pfc.format_version       = PresetFileCache::FORMAT_VERSION;
    pfc.config_options_count = print_config_def.options.size();

    try {
        pfc.json_mtime = static_cast<int64_t>(fs::last_write_time(fs::path(preset.file)));
    } catch (...) {}

    fs::path info_path = fs::path(preset.file);
    info_path.replace_extension(".info");
    if (fs::exists(info_path)) {
        try {
            pfc.info_mtime = static_cast<int64_t>(fs::last_write_time(info_path));
        } catch (...) {}
    }

    CachedPreset& cp    = pfc.preset;
    cp.type             = static_cast<int>(preset.type);
    cp.name             = preset.name;
    cp.alias            = preset.alias;
    cp.file             = preset.file;
    cp.version          = preset.version.valid() ? preset.version.to_string() : "";
    cp.filament_id      = preset.filament_id;
    cp.setting_id       = preset.setting_id;
    cp.description      = preset.description;
    cp.base_id          = preset.base_id;
    cp.user_id          = preset.user_id;
    cp.sync_info        = preset.sync_info;
    cp.updated_time     = preset.updated_time;
    cp.renamed_from     = preset.renamed_from;
    cp.is_system        = false;
    cp.is_visible       = preset.is_visible;
    cp.config           = preset.config;

    save_blob(cache_path, pfc);
}

} // namespace PresetBundleCache
} // namespace Slic3r
