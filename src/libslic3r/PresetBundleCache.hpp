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
    int         technology = 0;
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

// ---- Per-preset cache entry ----

struct CachedPreset {
    int         type = 0;
    std::string name, alias, file, version;
    std::string vendor_id;
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
// Single blob at <data_dir>/system/system_presets_cache.cache
// Covers all vendor profiles and system presets.
// Invalidated when any vendor version string changes or config option count changes.

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
    static std::string bundled_cache_path();
    bool is_valid(const std::string& system_dir) const;
    void capture(const PresetBundle& bundle, const std::string& system_dir);
    void apply(PresetBundle& bundle) const;
    bool load(const std::string& path);
    void save(const std::string& path) const;
};

} // namespace PresetBundleCache
} // namespace Slic3r