#include "libslic3r/PresetCacheFormat.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace Slic3r {

CacheDictionary::CacheDictionary()
{
    // ENUM_UNNAMED is index 0 and always the empty name.
    m_enum_values.emplace_back();
}

// The ints an enum option holds — one for a coEnum, the whole vector for coEnums.
static std::vector<int> enum_ints(const ConfigOptionDef& def, const ConfigOption* opt)
{
    if (def.type == coEnum)
        return { opt->getInt() };
    return static_cast<const ConfigOptionInts*>(opt)->values;
}

// The name this build gives one of those ints, empty where it has none — a
// nullable option's nil, or a definition carrying no enum_keys_map. Enums are
// written by name so a build that reorders an enum's values still reads it right.
static std::string enum_name_of(const ConfigOptionDef& def, int value)
{
    if (def.enum_keys_map != nullptr)
        for (const auto& kvp : *def.enum_keys_map)
            if (kvp.second == value)
                return kvp.first;
    return {};
}

void CacheDictionary::collect(const DynamicPrintConfig& config)
{
    for (auto it = config.cbegin(); it != config.cend(); ++ it) {
        const ConfigOptionDef* def = print_config_def.get(it->first);
        if (def == nullptr)
            continue;   // save_config does not write it either
        if (m_key_index.try_emplace(it->first, uint16_t(m_keys.size())).second) {
            m_keys.push_back(it->first);
            m_types.push_back(uint16_t(def->type));
        }
        if (def->type != coEnum && def->type != coEnums)
            continue;
        for (int value : enum_ints(*def, it->second.get())) {
            std::string name = enum_name_of(*def, value);
            if (! name.empty() && m_enum_index.try_emplace(name, uint16_t(m_enum_values.size())).second)
                m_enum_values.push_back(std::move(name));
        }
    }
}

uint16_t CacheDictionary::key_index(const t_config_option_key& key) const
{
    auto it = m_key_index.find(key);
    if (it == m_key_index.end())
        throw std::runtime_error("preset cache: option " + key + " was never collected into the dictionary");
    return it->second;
}

uint16_t CacheDictionary::enum_index(const std::string& name) const
{
    if (name.empty())
        return ENUM_UNNAMED;
    auto it = m_enum_index.find(name);
    return it == m_enum_index.end() ? ENUM_UNNAMED : it->second;
}

void CacheDictionary::save(cereal::BinaryOutputArchive& ar) const
{
    // Checked here rather than left to the caller: an index that wrapped would
    // be written silently, and nothing downstream could tell.
    if (m_keys.size() > MAX_ENTRIES || m_enum_values.size() > MAX_ENTRIES)
        throw std::runtime_error("preset cache: the option dictionary outgrew the uint16 it is indexed with");
    ar(m_keys, m_types, m_enum_values);
}

void CacheDictionary::load(cereal::BinaryInputArchive& ar)
{
    ar(m_keys, m_types, m_enum_values);
    if (m_keys.size() != m_types.size())
        throw std::runtime_error("preset cache: dictionary key and type tables differ in length");
    if (m_keys.size() > MAX_ENTRIES || m_enum_values.size() > MAX_ENTRIES)
        throw std::runtime_error("preset cache: dictionary is larger than the uint16 it is indexed with");
    if (m_enum_values.empty() || ! m_enum_values.front().empty())
        throw std::runtime_error("preset cache: dictionary is missing its unnamed-enum slot");
    // Resolved once per file: every option read after this is a vector index.
    m_defs.resize(m_keys.size());
    for (size_t i = 0; i < m_keys.size(); ++ i) {
        const ConfigOptionDef* def = print_config_def.get(m_keys[i]);
        m_defs[i] = (def != nullptr && uint16_t(def->type) == m_types[i]) ? def : nullptr;
    }
}

// ---- one config -----------------------------------------------------------

static void save_enum_option(cereal::BinaryOutputArchive& ar, const ConfigOptionDef& def,
                             const ConfigOption* opt, const CacheDictionary& dict)
{
    const std::vector<int> values = enum_ints(def, opt);
    ar(uint32_t(values.size()));
    for (int value : values) {
        const uint16_t idx = dict.enum_index(enum_name_of(def, value));
        ar(idx);
        if (idx == CacheDictionary::ENUM_UNNAMED)
            ar(int32_t(value));
    }
}

// `config` may be null, in which case the option is read and dropped.
static void load_enum_option(cereal::BinaryInputArchive& ar, ConfigOptionType type,
                             const ConfigOptionDef* def, DynamicPrintConfig* config,
                             const CacheDictionary& dict)
{
    uint32_t cnt = 0;
    ar(cnt);
    if (type == coEnum && cnt != 1)
        throw std::runtime_error("preset cache: a scalar enum carrying more than one value");
    // Every element is read whatever happens, so the stream stays in sync and
    // whatever follows this option still loads.
    bool usable = def != nullptr && config != nullptr;
    std::vector<int> values;
    values.reserve(cnt);
    for (uint32_t i = 0; i < cnt; ++ i) {
        uint16_t idx = 0;
        ar(idx);
        if (! dict.valid_enum_index(idx))
            throw std::runtime_error("preset cache: enum value index past the end of the dictionary");
        if (idx == CacheDictionary::ENUM_UNNAMED) {
            // An int the writer could not name — a nil, or an option whose
            // definition carried no enum_keys_map. It travels verbatim.
            int32_t raw = 0;
            ar(raw);
            values.push_back(int(raw));
            continue;
        }
        if (! usable)
            continue;             // the index above was this element's whole payload
        if (def->enum_keys_map == nullptr) {
            usable = false;       // this build no longer maps this option's names
            continue;
        }
        const auto it = def->enum_keys_map->find(dict.enum_name_at(idx));
        if (it == def->enum_keys_map->end()) {
            usable = false;       // a value this build dropped: the option goes with it
            continue;
        }
        values.push_back(it->second);
    }
    if (! usable)
        return;
    if (type == coEnum) {
        config->set_key_value(def->opt_key, new ConfigOptionEnumGeneric(def->enum_keys_map, values.front()));
    } else {
        auto* opt = def->nullable ? static_cast<ConfigOptionInts*>(new ConfigOptionEnumsGenericNullable(def->enum_keys_map))
                                  : static_cast<ConfigOptionInts*>(new ConfigOptionEnumsGeneric(def->enum_keys_map));
        opt->values = std::move(values);
        config->set_key_value(def->opt_key, opt);
    }
}

void save_config(cereal::BinaryOutputArchive& ar, const DynamicPrintConfig& config, const CacheDictionary& dict)
{
    struct Written { uint16_t idx; const ConfigOptionDef* def; const ConfigOption* opt; };
    std::vector<Written> written;
    written.reserve(config.size());
    for (auto it = config.cbegin(); it != config.cend(); ++ it)
        if (const ConfigOptionDef* def = print_config_def.get(it->first))
            written.push_back({ dict.key_index(it->first), def, it->second.get() });

    ar(uint32_t(written.size()));
    for (const Written& w : written) {
        ar(w.idx);
        if (w.def->type == coEnum || w.def->type == coEnums)
            save_enum_option(ar, *w.def, w.opt, dict);
        else
            w.def->save_option_to_archive(ar, w.opt);
    }
}

// `config` null means: read everything, keep nothing.
static void read_config(cereal::BinaryInputArchive& ar, DynamicPrintConfig* config, const CacheDictionary& dict)
{
    uint32_t cnt = 0;
    ar(cnt);
    if (config != nullptr)
        config->clear();
    // Reused across the loop: constructing a ConfigOptionDef per dropped option
    // would allocate its strings and vectors for nothing.
    ConfigOptionDef scratch;
    for (uint32_t i = 0; i < cnt; ++ i) {
        uint16_t idx = 0;
        ar(idx);
        if (! dict.valid_key_index(idx))
            throw std::runtime_error("preset cache: option index past the end of the dictionary");
        const ConfigOptionType type = dict.type_at(idx);
        const ConfigOptionDef* def  = dict.def_at(idx);
        if (type == coEnum || type == coEnums) {
            load_enum_option(ar, type, def, config, dict);
        } else if (def != nullptr && config != nullptr) {
            config->set_key_value(def->opt_key, def->load_option_from_archive(ar));
        } else {
            // Read by the type the writer recorded, then drop: the same outcome
            // a JSON profile gets for an option this build no longer has.
            scratch.type = type;
            std::unique_ptr<ConfigOption> discard(scratch.load_option_from_archive(ar));
        }
    }
}

void load_config(cereal::BinaryInputArchive& ar, DynamicPrintConfig& config, const CacheDictionary& dict)
{
    read_config(ar, &config, dict);
}

void skip_config(cereal::BinaryInputArchive& ar, const CacheDictionary& dict)
{
    read_config(ar, nullptr, dict);
}

} // namespace Slic3r
