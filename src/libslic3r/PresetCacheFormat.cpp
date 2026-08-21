#include "libslic3r/PresetCacheFormat.hpp"

#include <algorithm>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/set.hpp>

#include "libslic3r/Utils.hpp"

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

// ---- The per-vendor cache file (<vendor>.opc) -----------------------------

namespace {

#pragma pack(push, 1)
struct CacheFileHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t data_size;
    uint32_t crc32;
};
#pragma pack(pop)
static_assert(sizeof(CacheFileHeader) == 20, "CacheFileHeader must be 20 bytes");

constexpr uint32_t CACHE_MAGIC   = 0x4F52435A; // "ORCZ"
// Bump when the wire format changes in a way the payload cannot describe
// itself out of: reordering, removing or retyping a field of a hand-written
// serialize() (VendorProfile and its nested types, CachedPreset via
// save_entries below), or a change to the cache's own layout or the
// meaning of its stamps. Option-schema drift is NOT such a change — the
// dictionary handles it, which is why this no longer moves every release.
constexpr uint32_t CACHE_VERSION = 1;

// A stamp-string read that refuses an absurd length before allocating anything.
// The stamps are read from files named from the outside (peek_version is
// pointed at whatever <vendor>.opc a directory holds), so the length word may
// be arbitrary bytes — and a resize to a garbage 64-bit length does not fail as
// a catchable bad_alloc here, it takes the app down through the out-of-memory
// handler. A vendor name or profile version is a short token; anything longer
// is not a cache this build wrote.
std::string read_bounded_string(cereal::BinaryInputArchive& ar)
{
    constexpr uint64_t MAX_STAMP_LEN = 1024;
    cereal::size_type len = 0;
    ar(cereal::make_size_tag(len));
    if (uint64_t(len) > MAX_STAMP_LEN)
        throw std::runtime_error("preset cache: string length out of bounds");
    std::string s(size_t(len), '\0');
    ar(cereal::binary_data(s.data(), size_t(len)));
    return s;
}

// The prologue every cache reader starts with: the format version, then the
// vendor's identity. Returns the vendor version stamped on a body this build can
// read, empty on anything else — which is the same answer as "not this vendor".
std::string read_cache_stamps(cereal::BinaryInputArchive& ar, const std::string& expected_vendor_name)
{
    // The version is judged before anything variable-length is read: on a body
    // that is not a per-vendor cache of this version, the bytes where a string
    // length would sit may be arbitrary framing.
    uint32_t cache_version = 0;
    ar(cache_version);
    if (cache_version != CACHE_VERSION)
        return {};
    const std::string vendor_name    = read_bounded_string(ar);
    const std::string vendor_version = read_bounded_string(ar);
    if (vendor_name != expected_vendor_name)
        return {};
    return vendor_version;
}

// A cache stays usable as long as it was built from a vendor profile at least
// as new as the one now on disk. Profiles whose version is invalid cannot be
// judged this way and are never served from cache; where no profile sits
// beside the cache at all, nothing can be newer than it — that state is passed
// as Semver::inf(), which no real profile can carry (an invalid version could
// not say it apart from "profile there but unjudgeable", and zero would
// collide with a genuine "0.0.0"). This is the serve rule; the install rule
// (cache_covers in PresetBundle.cpp) deliberately reads an unjudgeable profile
// the other way, so the two are not one function.
bool cache_covers_version(const std::string& cached, const Semver& on_disk)
{
    if (on_disk == Semver::inf())
        return true;    // before parsing `cached`: nothing exists that the stamp must cover
    if (! on_disk.valid())
        return false;
    const auto cached_ver = Semver::parse(cached);
    return cached_ver && *cached_ver >= on_disk;
}

// CachedPreset on the wire: all fields, declaration order, in one place.
// `config` writes, reads or skips the config sitting in the middle of that
// order — the three things a reader can want to do with it — so save, load and
// the name peek below cannot drift apart. Keep in sync with the struct in
// PresetCacheFormat.hpp and bump CACHE_VERSION on change. Written here rather
// than as a serialize() member because the config needs the file's dictionary,
// which cereal cannot thread through one.
template<class Archive, class Entry, class ConfigFn>
void visit_entry(Archive& ar, Entry& e, ConfigFn&& config)
{
    ar(e.name, e.sub_path);
    config();
    ar(e.inherits, e.description, e.instantiation, e.setting_id, e.filament_id, e.renamed_from);
}

// The count comes from a file that has already passed magic and CRC, but a
// reserve is a promise to allocate: cap it and let push_back grow the rest.
constexpr uint32_t MAX_RESERVED_ENTRIES = 4096;

void save_entries(cereal::BinaryOutputArchive& ar,
                  const std::vector<CachedPreset>& entries,
                  const CacheDictionary& dict)
{
    ar(uint32_t(entries.size()));
    for (const CachedPreset& e : entries)
        visit_entry(ar, e, [&] { save_config(ar, e.config_src, dict); });
}

void load_entries(cereal::BinaryInputArchive& ar,
                  std::vector<CachedPreset>& entries,
                  const CacheDictionary& dict)
{
    uint32_t cnt = 0;
    ar(cnt);
    entries.clear();
    entries.reserve(std::min(cnt, MAX_RESERVED_ENTRIES));
    for (uint32_t i = 0; i < cnt; ++ i) {
        CachedPreset e;
        visit_entry(ar, e, [&] { load_config(ar, e.config_src, dict); });
        entries.push_back(std::move(e));
    }
}

// Read a raw cache body: verify magic, size, CRC.
bool read_cache_blob(const std::string& path, std::string& out_blob)
{
    try {
        boost::nowide::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open())
            return false;
        CacheFileHeader fhdr;
        if (!ifs.read(reinterpret_cast<char*>(&fhdr), sizeof(fhdr)))
            return false;
        if (fhdr.magic != CACHE_MAGIC)
            return false;
        // data_size is 8 bytes from a file nothing has authenticated yet, and
        // it is about to size an allocation. The body is the whole of the file
        // behind the header — anything else is not a cache this build wrote.
        ifs.seekg(0, std::ios::end);
        const std::streamoff file_size = ifs.tellg();
        if (file_size < std::streamoff(sizeof(fhdr)) ||
            fhdr.data_size == 0 ||
            fhdr.data_size != uint64_t(file_size) - sizeof(fhdr))
            return false;
        ifs.seekg(sizeof(fhdr), std::ios::beg);
        out_blob.assign(fhdr.data_size, '\0');
        if (!ifs.read(&out_blob[0], static_cast<std::streamsize>(fhdr.data_size)))
            return false;
        boost::crc_32_type crc;
        crc.process_bytes(out_blob.data(), out_blob.size());
        if (crc.checksum() != fhdr.crc32) {
            BOOST_LOG_TRIVIAL(warning) << "VendorCacheFile: CRC mismatch: " << path;
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "VendorCacheFile: read failed (" << path << "): " << e.what();
        return false;
    }
}

// Write a cache body behind the standard 20-byte file header. False when the
// file could not be opened or written whole.
bool write_cache_blob(const std::string& path, const std::string& blob)
{
    boost::crc_32_type crc;
    crc.process_bytes(blob.data(), blob.size());
    // Written beside the target and moved into place, as AppConfig::save does:
    // a cache is truncated and rewritten in full, so a write that dies partway
    // would otherwise leave a header claiming more body than the file holds.
    // The PID suffix also keeps two instances writing the same vendor from
    // interleaving.
    const std::string tmp_path = path + "." + std::to_string(get_current_pid()) + ".tmp";
    try {
        boost::filesystem::create_directories(boost::filesystem::path(path).parent_path());
        {
            boost::nowide::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) {
                BOOST_LOG_TRIVIAL(warning) << "VendorCacheFile: cannot open for writing: " << tmp_path;
                return false;
            }
            CacheFileHeader fhdr;
            fhdr.magic     = CACHE_MAGIC;
            fhdr.version   = CACHE_VERSION;
            fhdr.data_size = static_cast<uint64_t>(blob.size());
            fhdr.crc32     = crc.checksum();
            ofs.write(reinterpret_cast<const char*>(&fhdr), sizeof(fhdr));
            ofs.write(blob.data(), static_cast<std::streamsize>(blob.size()));
            ofs.close();   // flush; close() raises failbit on error
            if (! ofs.good()) {
                BOOST_LOG_TRIVIAL(warning) << "VendorCacheFile: write failed (" << tmp_path << ")";
                boost::system::error_code ec;
                boost::filesystem::remove(tmp_path, ec);
                return false;
            }
        }
        if (const std::error_code ec = rename_file(tmp_path, path)) {
            BOOST_LOG_TRIVIAL(warning) << "VendorCacheFile: could not move " << tmp_path << " into place: " << ec.message();
            boost::system::error_code rm;
            boost::filesystem::remove(tmp_path, rm);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "VendorCacheFile: write failed (" << path << "): " << e.what();
        boost::system::error_code ec;
        boost::filesystem::remove(tmp_path, ec);
        return false;
    }
}

} // anonymous namespace

// static
bool VendorCacheFile::save(const std::string& path, const std::string& vendor_name,
                           const std::string& vendor_version, const VendorCacheData& data)
{
    try {
        // Collected before anything is written: the dictionary sits ahead of the
        // entries so a reader resolves it once and then indexes.
        CacheDictionary dict;
        for (const std::vector<CachedPreset>* entries : { &data.process_entries, &data.filament_entries, &data.machine_entries })
            for (const CachedPreset& e : *entries)
                dict.collect(e.config_src);

        std::ostringstream body(std::ios::binary);
        {
            cereal::BinaryOutputArchive ar(body);
            ar(CACHE_VERSION);
            ar(vendor_name, vendor_version);
            dict.save(ar);
            ar(data.vendors);
            save_entries(ar, data.process_entries, dict);
            save_entries(ar, data.filament_entries, dict);
            save_entries(ar, data.machine_entries, dict);
            ar(data.parse_errors);
        }
        return write_cache_blob(path, body.str());
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "VendorCacheFile: failed to save vendor cache " << path << ": " << e.what();
        return false;
    }
}

// static
bool VendorCacheFile::load(const std::string& path, const std::string& expected_vendor_name,
                           const Semver& expected_vendor_version, VendorCacheData& data)
{
    std::string blob;
    if (! read_cache_blob(path, blob))
        return false;
    try {
        // Read in place: an istringstream would copy the blob once more just to
        // stream over it.
        boost::iostreams::stream<boost::iostreams::array_source> body(blob.data(), blob.size());
        cereal::BinaryInputArchive ar(body);
        const std::string vendor_version = read_cache_stamps(ar, expected_vendor_name);
        if (vendor_version.empty() || ! cache_covers_version(vendor_version, expected_vendor_version))
            return false;
        CacheDictionary dict;
        dict.load(ar);
        ar(data.vendors);
        load_entries(ar, data.process_entries, dict);
        load_entries(ar, data.filament_entries, dict);
        load_entries(ar, data.machine_entries, dict);
        ar(data.parse_errors);
        if (data.vendors.find(expected_vendor_name) == data.vendors.end())
            throw std::runtime_error("vendor cache does not carry its own vendor profile");
        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "VendorCacheFile: rejecting vendor cache " << path << ": " << e.what();
        return false;
    }
}

// static
std::string VendorCacheFile::peek_version(const std::string& path, const std::string& expected_vendor_name)
{
    try {
        boost::nowide::ifstream ifs(path, std::ios::binary);
        CacheFileHeader fhdr;
        if (! ifs.read(reinterpret_cast<char*>(&fhdr), sizeof(fhdr)) || fhdr.magic != CACHE_MAGIC)
            return {};
        // Only the head of the body is read, and its CRC left unverified: the
        // stamps sit at the front, and this answers "what version is this?"
        // without paying for tens of megabytes. Callers that need to know the
        // file is whole use usable_version instead.
        std::string head(static_cast<size_t>(std::min<uint64_t>(fhdr.data_size, 1024)), '\0');
        if (! ifs.read(&head[0], static_cast<std::streamsize>(head.size())))
            return {};
        std::istringstream body(head, std::ios::binary);
        cereal::BinaryInputArchive ar(body);
        return read_cache_stamps(ar, expected_vendor_name);
    } catch (const std::exception&) {
        return {};
    }
}

// static
Semver VendorCacheFile::usable_version(const std::string& path, const std::string& expected_vendor_name)
{
    std::string blob;
    if (! read_cache_blob(path, blob))
        return Semver::invalid();
    try {
        boost::iostreams::stream<boost::iostreams::array_source> body(blob.data(), blob.size());
        cereal::BinaryInputArchive ar(body);
        const auto ver = Semver::parse(read_cache_stamps(ar, expected_vendor_name));
        return ver ? *ver : Semver::invalid();
    } catch (const std::exception&) {
        return Semver::invalid();
    }
}

// static
bool VendorCacheFile::carries_preset(const std::string& path, const std::string& vendor_name,
                                     Preset::Type type, const std::string& preset_name)
{
    std::string blob;
    if (! read_cache_blob(path, blob))
        return false;
    try {
        boost::iostreams::stream<boost::iostreams::array_source> body(blob.data(), blob.size());
        cereal::BinaryInputArchive ar(body);
        if (read_cache_stamps(ar, vendor_name).empty())
            return false;
        CacheDictionary dict;
        dict.load(ar);
        VendorMap vendors;
        ar(vendors);
        // Reused: every entry overwrites it, and only its name is ever looked at.
        CachedPreset entry;
        // Written in this order by save. The list that could carry the preset
        // is the last one worth reading.
        for (Preset::Type kind : { Preset::TYPE_PRINT, Preset::TYPE_FILAMENT, Preset::TYPE_PRINTER }) {
            uint32_t cnt = 0;
            ar(cnt);
            for (uint32_t i = 0; i < cnt; ++ i) {
                visit_entry(ar, entry, [&] { skip_config(ar, dict); });
                if (kind == type && entry.name == preset_name)
                    return true;
            }
            if (kind == type)
                return false;
        }
        return false;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "VendorCacheFile: could not read preset names from " << path << ": " << e.what();
        return false;
    }
}

} // namespace Slic3r
