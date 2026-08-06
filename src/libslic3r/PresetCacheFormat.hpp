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
#include "libslic3r/PrintConfig.hpp"

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

    // The layout these two agree on is covered by CACHE_VERSION (PresetBundle.cpp);
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

} // namespace Slic3r

#endif // slic3r_PresetCacheFormat_hpp_
