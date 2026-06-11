#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <cereal/archives/binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/polymorphic.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include "Preset.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

class PresetBundle;

namespace PresetBundleCache {

// ---- Vendor profile structures ----

struct CachedPrinterVariant {
    std::string name;
    template<class Archive> void serialize(Archive& ar) { ar(name); }
};

struct CachedPrinterModel {
    std::string id, name, model_id, family;
    int         technology = 0; // PrinterTechnology enum
    std::vector<CachedPrinterVariant> variants;
    std::vector<std::string>          default_materials;
    std::vector<std::string>          not_support_bed_types;
    std::string bed_model, bed_texture, image_bed_type;
    std::string bottom_texture_end_name, use_double_extruder_default_texture;
    std::string bottom_texture_rect, middle_texture_rect, hotend_model;

    template<class Archive>
    void serialize(Archive& ar)
    {
        ar(id, name, model_id, family, technology, variants, default_materials,
           not_support_bed_types, bed_model, bed_texture, image_bed_type,
           bottom_texture_end_name, use_double_extruder_default_texture,
           bottom_texture_rect, middle_texture_rect, hotend_model);
    }
};

struct CachedVendorProfile {
    std::string id, name, config_version, config_update_url, changelog_url;
    std::vector<CachedPrinterModel> models;
    std::vector<std::string>        default_filaments;
    std::vector<std::string>        default_sla_materials;

    template<class Archive>
    void serialize(Archive& ar)
    {
        ar(id, name, config_version, config_update_url, changelog_url,
           models, default_filaments, default_sla_materials);
    }
};

// ---- Per-preset cache entry (shared by system and per-file caches) ----

struct CachedPreset {
    int         type = 0; // Preset::Type
    std::string name, alias, file, version;
    std::string vendor_id;   // reconstruct Preset::vendor pointer (system only)
    std::string filament_id, setting_id, description;
    std::string base_id, user_id, sync_info;
    long long   updated_time = 0;
    std::vector<std::string> renamed_from;
    bool        is_system               = true;
    bool        is_visible              = true;
    bool        m_from_orca_filament_lib = false;
    DynamicPrintConfig config;

    template<class Archive>
    void serialize(Archive& ar)
    {
        ar(type, name, alias, file, version, vendor_id, filament_id, setting_id,
           description, base_id, user_id, sync_info, updated_time, renamed_from,
           is_system, is_visible, m_from_orca_filament_lib, config);
    }
};

// ---- System preset cache ----
// Stored next to vendor JSONs: <data_dir>/system/system_presets_cache.bin

struct SystemPresetsCache {
    static constexpr uint32_t FORMAT_VERSION = 2;

    uint32_t format_version      = FORMAT_VERSION;
    size_t   config_options_count = 0;

    std::map<std::string, std::string> vendor_versions;
    std::vector<CachedVendorProfile>   vendor_profiles;

    std::vector<CachedPreset> print_presets;
    std::vector<CachedPreset> filament_presets;
    std::vector<CachedPreset> printer_presets;
    std::vector<CachedPreset> sla_print_presets;
    std::vector<CachedPreset> sla_material_presets;

    std::map<std::string, DynamicPrintConfig> config_maps;
    std::map<std::string, std::string>        filament_id_maps;

    template<class Archive>
    void serialize(Archive& ar)
    {
        ar(format_version, config_options_count, vendor_versions,
           vendor_profiles,
           print_presets, filament_presets, printer_presets,
           sla_print_presets, sla_material_presets,
           config_maps, filament_id_maps);
    }

    static std::string cache_path();
    bool is_valid(const std::string& system_dir) const;
    void capture(const PresetBundle& bundle, const std::string& system_dir);
    void apply(PresetBundle& bundle) const;
    bool load(const std::string& path);
    void save(const std::string& path) const;
};

// ---- Per-file preset cache ----
// Each <name>.json gets a sibling <name>.cache file.
// Invalidated when json or info mtime changes, or when the app binary changes.

struct PresetFileCache {
    static constexpr uint32_t FORMAT_VERSION = 1;

    uint32_t format_version      = FORMAT_VERSION;
    size_t   config_options_count = 0;
    int64_t  json_mtime           = 0; // last_write_time of .json file
    int64_t  info_mtime           = 0; // last_write_time of .info file (0 = doesn't exist)
    CachedPreset preset;

    template<class Archive>
    void serialize(Archive& ar)
    {
        ar(format_version, config_options_count, json_mtime, info_mtime, preset);
    }

    // Returns the path of the .cache file sibling to json_path.
    static std::string cache_path_for(const std::string& json_path);

    // Returns true when the stored mtimes still match the current filesystem state.
    bool is_valid(const std::string& json_path) const;
};

// Pre-load cached presets from all valid .cache files found in dir_path.
// Presets are inserted into coll via load_preset() so that a subsequent
// load_presets() call will skip them.  Returns the count of presets loaded.
int preload_file_caches(PresetCollection& coll, const std::string& dir_path,
                        const PresetOrigin& origin);

// Write (or overwrite) .cache files for presets in coll whose json .file is
// inside dir_path and whose existing .cache (if any) is stale or missing.
void update_file_caches(const PresetCollection& coll, const std::string& dir_path);

// Write (or overwrite) the .cache file for a single preset immediately after
// its .json has been written to disk.  Safe to call from Preset::save().
// Does nothing for system, default, or project-embedded presets.
void write_file_cache(const Preset& preset);

} // namespace PresetBundleCache
} // namespace Slic3r
