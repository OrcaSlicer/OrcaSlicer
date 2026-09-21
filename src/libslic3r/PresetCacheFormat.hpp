#ifndef slic3r_PresetCacheFormat_hpp_
#define slic3r_PresetCacheFormat_hpp_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"

namespace Slic3r {

// How the preset cache writes a DynamicPrintConfig.
//
// Not through the global cereal hooks in PrintConfig.hpp: those key an option by
// its serialization_key_ordinal, which ConfigDef::add assigns by declaration
// order at static-init time. Inserting one option into the middle of
// PrintConfig.cpp shifts every later ordinal, and the lookup on the way back in
// then SUCCEEDS on the wrong option — where the two share a type, and hundreds
// of coFloat/coBool/coInt options do, the bytes deserialize cleanly into the
// wrong key. Silently wrong print settings, no error. Those hooks are also the
// undo/redo wire format, where the process cannot change underneath them, so
// they stay as they are and the cache keys by name instead.
//
// Names are not repeated per preset. Each cache file carries one dictionary of
// the distinct opt_keys it uses, the type each was written as, and the distinct
// enum value names; an option on the wire is then a uint16 index into it plus
// its value. The dictionary is resolved to this build's option definitions once
// per file, after which reading an option is a vector index.
class CacheDictionary
{
public:
    CacheDictionary();

    // Index reserved in the enum table for an int the writing build could not
    // name — a nullable option's nil, or a definition carrying no
    // enum_keys_map. The raw int32 follows it on the wire and is loaded
    // verbatim, so those values survive too.
    static constexpr uint16_t ENUM_UNNAMED = 0;

    // ---- writing ----

    // Record every key and enum value `config` uses. Call for every config that
    // will be written, before writing the dictionary.
    void collect(const DynamicPrintConfig& config);

    uint16_t key_index(const t_config_option_key& key) const;
    // ENUM_UNNAMED for an empty name or one that was never collected.
    uint16_t enum_index(const std::string& name) const;

    // ---- reading ----

    // The definition an index resolves to in THIS build, or nullptr where the
    // key is unknown here or is now defined with a different type. A nullptr
    // entry's value is still read — using type_at(idx), the type the writer
    // recorded — and then dropped, which is what a JSON profile gets for an
    // option this build no longer has.
    const ConfigOptionDef* def_at(uint16_t idx) const { return m_defs[idx]; }
    ConfigOptionType       type_at(uint16_t idx) const { return ConfigOptionType(m_types[idx]); }
    const std::string&     enum_name_at(uint16_t idx) const { return m_enum_values[idx]; }
    // m_defs, not m_keys: only load() sizes it, so this is false for every index
    // on a dictionary that was collected rather than read.
    bool valid_key_index(uint16_t idx) const { return size_t(idx) < m_defs.size(); }
    bool valid_enum_index(uint16_t idx) const { return size_t(idx) < m_enum_values.size(); }

    // The layout these two agree on is covered by CACHE_VERSION (PresetCacheFormat.cpp);
    // bump it when they change.
    // Throws when either table outgrew the uint16 the wire format indexes it
    // with. Both are bounded by the option count (912 at the time of writing), so
    // that is a build-time failure in CI, not a runtime one.
    void save(cereal::BinaryOutputArchive& ar) const;
    // Throws on a dictionary that cannot be indexed as written.
    void load(cereal::BinaryInputArchive& ar);

private:
    // Indices are uint16, so a table may hold at most this many entries.
    static constexpr size_t MAX_ENTRIES = 0xFFFF;

    std::vector<std::string> m_keys;
    // ConfigOptionType, as written. Sixteen bits, not eight: coVectorType is
    // 0x4000, so every vector type — coFloats, coEnums, coStrings — is above
    // 255, and a byte would fold each one onto its scalar counterpart.
    std::vector<uint16_t>    m_types;
    std::vector<std::string> m_enum_values;  // [ENUM_UNNAMED] is always empty

    // Writing.
    std::unordered_map<std::string, uint16_t> m_key_index;
    std::unordered_map<std::string, uint16_t> m_enum_index;
    // Reading, resolved once by load().
    std::vector<const ConfigOptionDef*> m_defs;
};

// One config, keyed through `dict`. Options print_config_def does not know are
// not written: nothing could give them a type on the way back in.
void save_config(cereal::BinaryOutputArchive& ar, const DynamicPrintConfig& config, const CacheDictionary& dict);
// Throws only on a payload that cannot be indexed; an option this build cannot
// place is dropped, not fatal.
void load_config(cereal::BinaryInputArchive& ar, DynamicPrintConfig& config, const CacheDictionary& dict);
// Consume one config without building it, for a reader that only wants what
// comes after.
void skip_config(cereal::BinaryInputArchive& ar, const CacheDictionary& dict);

// One preset as its JSON subfile states it: the config diff, the name of the
// preset it inherits, and the parse metadata — everything the parse phase of
// load_vendor_configs_from_json extracts and nothing it derives. Inheritance
// is resolved when the entry is installed, against whatever filament library
// is loaded then, so a cache carries no other vendor's values and no other
// vendor's update can make it stale.
// Written and read by visit_entry in PresetCacheFormat.cpp, which lists every
// field below in this order — once, for the save, the load and the name peek alike.
struct CachedPreset
{
    std::string              name;
    std::string              sub_path;       // path under the vendor's directory
    DynamicPrintConfig       config_src;     // the preset's own diff, nothing inherited
    std::string              inherits;
    std::string              description;
    std::string              instantiation;  // "true"/"false" as stated; anything else was already counted as a parse error
    std::string              setting_id;
    std::string              filament_id;
    std::vector<std::string> renamed_from;
};

// What one per-vendor cache file carries besides its stamps: the vendor profile
// map, the presets in source form, and how many errors their parse counted.
struct VendorCacheData
{
    VendorMap                 vendors;
    std::vector<CachedPreset> process_entries;
    std::vector<CachedPreset> filament_entries;
    std::vector<CachedPreset> machine_entries;
    uint64_t                  parse_errors = 0;
};

// A per-vendor preset cache file (<vendor>.opc): a 20-byte header (magic, format
// version, body size, CRC) framing one cereal body — stamps (format version,
// vendor name, vendor profile version), the option dictionary, then the
// VendorCacheData. Everything about those bytes lives here; when a vendor is
// served from its cache, and how entries install into a bundle, is
// PresetBundle's business.
class VendorCacheFile
{
public:
    // Save one vendor (vendor_name at vendor_version). False when the file
    // could not be written whole.
    static bool save(const std::string& path, const std::string& vendor_name,
                     const std::string& vendor_version, const VendorCacheData& data);

    // Read a whole cache into `data`. False — with `data` in an unspecified
    // state — unless the file is a cache this build wrote, its CRC holds, it
    // names this vendor, it was built from a vendor profile at least as new as
    // `expected_vendor_version`, and it carries its own vendor profile. An
    // invalid expected version (a profile whose version
    // cannot be judged) is never served from cache; Semver::inf() (no profile
    // beside the cache at all) accepts whatever is cached.
    static bool load(const std::string& path, const std::string& expected_vendor_name,
                     const Semver& expected_vendor_version, VendorCacheData& data);

    // Read the profile version a cache was stamped with, without deserializing
    // its presets. Empty if the file is unreadable, not a cache this build
    // understands, or not this vendor's. This is how an installed vendor's
    // version is known when only its cache is installed.
    static std::string peek_version(const std::string& path, const std::string& expected_vendor_name);

    // The profile version an installed cache can actually be served at, or an
    // invalid Semver when the file is not a cache this build can read. Unlike
    // peek_version this verifies the body's CRC, at the cost of reading the
    // whole file: where the cache is the vendor's whole installation, "a file
    // is there" is not enough to call it installed, and a vendor wrongly
    // believed installed is never repaired.
    static Semver usable_version(const std::string& path, const std::string& expected_vendor_name);

    // Whether a cache carries a preset of `type` under `preset_name`, without
    // installing any of them. False when the file is not a cache this build can
    // read. The three kinds are written in one stream, so reaching the machines
    // means reading past the processes and filaments — their configs are consumed
    // and dropped rather than built. This is how a build that ships caches instead
    // of preset JSONs answers "which vendor carries this preset?".
    static bool carries_preset(const std::string& path, const std::string& vendor_name,
                               Preset::Type type, const std::string& preset_name);
};

} // namespace Slic3r

#endif // slic3r_PresetCacheFormat_hpp_
