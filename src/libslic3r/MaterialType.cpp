#include "MaterialType.hpp"

#include "Preset.hpp"
#include "Utils.hpp"

#include <algorithm>
#include <optional>
#include <string_view>
#include <unordered_map>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace {
namespace fs = boost::filesystem;
using json   = nlohmann::json;

constexpr int    DEFAULT_MIN_TEMP             = 190;
constexpr int    DEFAULT_MAX_TEMP             = 300;
constexpr int    DEFAULT_CHAMBER_MIN_TEMP     = 0;
constexpr int    DEFAULT_CHAMBER_MAX_TEMP     = 100;
constexpr double DEFAULT_ADHESION_COEFFICIENT = 1.0;
constexpr double DEFAULT_YIELD_STRENGTH       = 0.02;
constexpr double DEFAULT_THERMAL_LENGTH       = 200.0;

// Both tables are data files shipped in <resources>/info and mirrored into <data_dir>/info, so they can
// be updated (or hand-edited) without rebuilding. Each carries a "version" like the vendor profiles do.
constexpr const char* INFO_SUBDIR               = "info";
constexpr const char* MATERIAL_TYPES_FILE       = "material_types.json";
constexpr const char* BASE_COMPATIBILITIES_FILE = "base_compatibilities.json";

// Built-in fallback tables. They exist because the print config defaults are built during static
// initialisation, before the resource paths are known, and because a broken installation must not leave
// the material database empty. Only the most common materials are listed - the shipped JSON is the full
// table - and the unit tests check every entry here against it, so the two cannot drift apart.
const std::vector<MaterialTypeInfo>& builtin_material_types()
{
    static const std::vector<MaterialTypeInfo> material_types = {
        // name  min  max  ch_min ch_max adhesion yield thermal base materials
        {"ABS",  190, 300, 50, 65, 1,   0.1 , 100,  {}},
        {"ASA",  220, 300, 50, 65, 1,   0.1 , 100,  {"ABS"}},
        {"PA",   235, 280, 50, 60, 1,   0.02, 100,  {}},
        {"PC",   240, 300, 60, 70, 1,   0.02, 40,   {}},
        {"PET",  200, 290, 0,  55, 2,   0.3 , 100,  {}},
        {"PETG", 190, 260, 0,  55, 2,   0.3 , 100,  {"PET"}},
        {"PLA",  180, 240, 0,  45, 1,   0.02, 200,  {}},
        {"PVA",  185, 250, 0,  60, 1,   0.02, 200,  {}},
        {"TPU",  175, 260, 0,  50, 0.5, 0.02, 1000, {}}
    };

    return material_types;
}

const std::vector<BaseMaterialCompatibility>& builtin_base_compatibilities()
{
    // Adhesion rules between base materials.
    // A shared base material already implies compatibility, so only cross-family rules need listing here.
    // Lookups are symmetric, so each pair is listed once.
    static const std::vector<BaseMaterialCompatibility> base_compatibilities = {
        // base    compatible      incompatible
        {"ABS",    {"PC", "HIPS"}, {"PLA", "PET"}},
        {"PC",     {"ABS"},        {}},
        {"PLA",    {},             {"PET"}},
        {"PP",     {},             {"*"}},
        {"PET",    {},             {}},
        // Soluble support materials: bond with nothing ("*" wildcard).
        {"BVOH",   {},             {"*"}},
        {"PVA",    {},             {"*"}},
    };

    return base_compatibilities;
}

// Resolves which copy of a data file to read, keeping <data_dir>/info in sync with <resources>/info: the
// shipped file replaces the user copy whenever its "version" is newer, the rule the vendor profiles are
// refreshed with. Without the mirror the shipped copy is read directly. Returns an empty path when no
// copy exists.
fs::path resolve_data_file(const char* filename, bool mirror_to_data_dir)
{
    const fs::path rsrc_file = resources_dir().empty() ? fs::path() : fs::path(resources_dir()) / INFO_SUBDIR / filename;
    const fs::path user_file = mirror_to_data_dir && !data_dir().empty() ? fs::path(data_dir()) / INFO_SUBDIR / filename :
                                                                          fs::path();

    boost::system::error_code ec;
    const bool has_rsrc = !rsrc_file.empty() && fs::exists(rsrc_file, ec);
    const bool has_user = !user_file.empty() && fs::exists(user_file, ec);

    if (!has_rsrc)
        return has_user ? user_file : fs::path();
    if (user_file.empty())
        return rsrc_file;
    // A missing version reads as 0.0.0, so a corrupt user copy is refreshed as well.
    if (has_user && !(get_version_from_json(user_file.string()) < get_version_from_json(rsrc_file.string())))
        return user_file;

    fs::create_directories(user_file.parent_path(), ec);
    std::string error_message;
    if (copy_file(rsrc_file.string(), user_file.string(), error_message) != CopyFileResult::SUCCESS) {
        BOOST_LOG_TRIVIAL(warning) << "MaterialType: failed to update " << user_file.string() << " from resources: " << error_message;
        return rsrc_file;
    }
    BOOST_LOG_TRIVIAL(info) << "MaterialType: updated " << user_file.string() << " from resources";
    return user_file;
}

// Reads and parses one of the data files. Returns none when it is missing or malformed, in which case
// the caller keeps the built-in table.
std::optional<json> read_data_file(const char* filename, bool mirror_to_data_dir)
{
    const fs::path path = resolve_data_file(filename, mirror_to_data_dir);
    if (path.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "MaterialType: " << filename << " not found, keeping the built-in table";
        return std::nullopt;
    }

    try {
        boost::nowide::ifstream ifs(path.string());
        json                    j;
        ifs >> j;
        BOOST_LOG_TRIVIAL(info) << "MaterialType: loaded " << path.string() << ", version "
                                << j.value(BBL_JSON_KEY_VERSION, std::string("unknown"));
        return j;
    } catch (const std::exception& err) {
        BOOST_LOG_TRIVIAL(error) << "MaterialType: failed to parse " << path.string() << ": " << err.what()
                                 << ", keeping the built-in table";
        return std::nullopt;
    }
}

std::optional<std::vector<MaterialTypeInfo>> load_material_types(bool mirror_to_data_dir)
{
    const std::optional<json> j = read_data_file(MATERIAL_TYPES_FILE, mirror_to_data_dir);
    if (!j)
        return std::nullopt;

    try {
        std::vector<MaterialTypeInfo> types;
        for (const json& item : j->at("materials")) {
            MaterialTypeInfo info;
            info.name                 = item.at("name").get<std::string>();
            info.min_temp             = item.at("min_temp").get<int>();
            info.max_temp             = item.at("max_temp").get<int>();
            info.chamber_min_temp     = item.at("chamber_min_temp").get<int>();
            info.chamber_max_temp     = item.at("chamber_max_temp").get<int>();
            info.adhesion_coefficient = item.at("adhesion_coefficient").get<double>();
            info.yield_strength       = item.at("yield_strength").get<double>();
            info.thermal_length       = item.at("thermal_length").get<double>();
            info.base_materials       = item.value("base_materials", std::vector<std::string>{});
            types.emplace_back(std::move(info));
        }
        // An empty table would leave the filament type list empty as well, so treat it as invalid data.
        if (types.empty())
            throw std::runtime_error("no materials listed");
        return types;
    } catch (const std::exception& err) {
        BOOST_LOG_TRIVIAL(error) << "MaterialType: invalid " << MATERIAL_TYPES_FILE << ": " << err.what()
                                 << ", keeping the built-in table";
        return std::nullopt;
    }
}

std::optional<std::vector<BaseMaterialCompatibility>> load_base_compatibilities(bool mirror_to_data_dir)
{
    const std::optional<json> j = read_data_file(BASE_COMPATIBILITIES_FILE, mirror_to_data_dir);
    if (!j)
        return std::nullopt;

    try {
        std::vector<BaseMaterialCompatibility> compatibilities;
        for (const json& item : j->at("base_compatibilities")) {
            BaseMaterialCompatibility bc;
            bc.base_material = item.at("base_material").get<std::string>();
            bc.compatible    = item.value("compatible", std::vector<std::string>{});
            bc.incompatible  = item.value("incompatible", std::vector<std::string>{});
            compatibilities.emplace_back(std::move(bc));
        }
        return compatibilities;
    } catch (const std::exception& err) {
        BOOST_LOG_TRIVIAL(error) << "MaterialType: invalid " << BASE_COMPATIBILITIES_FILE << ": " << err.what()
                                 << ", keeping the built-in table";
        return std::nullopt;
    }
}

// Both tables plus the name index used by find(). Filled from the built-in tables at construction and
// replaced by load() at startup; read-only afterwards, so the lookups stay usable from the slicing
// threads without locking.
class MaterialDatabase
{
public:
    static MaterialDatabase& instance()
    {
        static MaterialDatabase database;
        return database;
    }

    void load(bool mirror_to_data_dir)
    {
        if (std::optional<std::vector<MaterialTypeInfo>> types = load_material_types(mirror_to_data_dir)) {
            m_types = std::move(*types);
            reindex();
        }
        if (std::optional<std::vector<BaseMaterialCompatibility>> compatibilities = load_base_compatibilities(mirror_to_data_dir))
            m_base_compatibilities = std::move(*compatibilities);
    }

    const std::vector<MaterialTypeInfo>&          types() const { return m_types; }
    const std::vector<BaseMaterialCompatibility>& base_compatibilities() const { return m_base_compatibilities; }

    // Indexed rather than scanned: find() sits under compatibility(), which runs per layer and per
    // filament pair while slicing, and a linear scan there costs a string compare per entry on every call.
    const MaterialTypeInfo* find(const std::string& name) const
    {
        const auto it = m_index.find(name);
        return it != m_index.end() ? it->second : nullptr;
    }

private:
    MaterialDatabase() { reindex(); }

    void reindex()
    {
        // The keys are views into the names of m_types, so the index only survives as long as the table
        // it was built from.
        m_index.clear();
        m_index.reserve(m_types.size());
        for (const MaterialTypeInfo& info : m_types)
            m_index.emplace(info.name, &info);
    }

    std::vector<MaterialTypeInfo>                                 m_types                = builtin_material_types();
    std::vector<BaseMaterialCompatibility>                        m_base_compatibilities = builtin_base_compatibilities();
    std::unordered_map<std::string_view, const MaterialTypeInfo*> m_index;
};
} // namespace

void MaterialType::load(bool mirror_to_data_dir) { MaterialDatabase::instance().load(mirror_to_data_dir); }

const std::vector<MaterialTypeInfo>& MaterialType::all() { return MaterialDatabase::instance().types(); }

const std::vector<BaseMaterialCompatibility>& MaterialType::base_compatibilities()
{
    return MaterialDatabase::instance().base_compatibilities();
}

const MaterialTypeInfo* MaterialType::find(const std::string& name) { return MaterialDatabase::instance().find(name); }

bool MaterialType::get_temperature_range(const std::string& type, int& min_temp, int& max_temp)
{
    min_temp = DEFAULT_MIN_TEMP;
    max_temp = DEFAULT_MAX_TEMP;

    if (const auto* info = find(type)) {
        min_temp = info->min_temp;
        max_temp = info->max_temp;
        return true;
    }

    return false;
}

bool MaterialType::get_chamber_temperature_range(const std::string& type, int& chamber_min_temp, int& chamber_max_temp)
{
    chamber_min_temp = DEFAULT_CHAMBER_MIN_TEMP;
    chamber_max_temp = DEFAULT_CHAMBER_MAX_TEMP;

    if (const auto* info = find(type)) {
        chamber_min_temp = info->chamber_min_temp;
        chamber_max_temp = info->chamber_max_temp;
        return true;
    }

    return false;
}

bool MaterialType::get_adhesion_coefficient(const std::string& type, double& adhesion_coefficient)
{
    adhesion_coefficient = DEFAULT_ADHESION_COEFFICIENT;

    if (const auto* info = find(type)) {
        adhesion_coefficient = info->adhesion_coefficient;
        return true;
    }

    return false;
}

bool MaterialType::get_yield_strength(const std::string& type, double& yield_strength)
{
    yield_strength = DEFAULT_YIELD_STRENGTH;

    if (const auto* info = find(type)) {
        yield_strength = info->yield_strength;
        return true;
    }

    return false;
}

bool MaterialType::get_thermal_length(const std::string& type, double& thermal_length)
{
    thermal_length = DEFAULT_THERMAL_LENGTH;

    if (const auto* info = find(type)) {
        thermal_length = info->thermal_length;
        return true;
    }

    return false;
}

std::vector<std::string> MaterialType::base_materials(const std::string& type)
{
    if (const auto* info = find(type); info && !info->base_materials.empty())
        return info->base_materials;
    return {type};
}

namespace {
// The families of `type` without copying them: the table's own vector when the type declares families,
// otherwise `fallback` holding just the type itself. compatibility() runs in slicing loops, so it must not
// allocate for the common case of a known material.
const std::vector<std::string>& base_materials_ref(const std::string& type, std::vector<std::string>& fallback)
{
    if (const auto* info = MaterialType::find(type); info && !info->base_materials.empty())
        return info->base_materials;
    fallback.assign(1, type);
    return fallback;
}
} // namespace

namespace {
// Adhesion rule (compatible/incompatible) between two base materials, looked up symmetrically.
bool base_in_list(const std::string& base, const std::string& other, bool incompatible)
{
    const auto& table = MaterialType::base_compatibilities();
    const auto  it    = std::find_if(table.begin(), table.end(),
                                     [&base](const BaseMaterialCompatibility& bc) { return bc.base_material == base; });
    if (it == table.end())
        return false;
    const auto& list = incompatible ? it->incompatible : it->compatible;
    // "*" is a wildcard matching every other base material (e.g. soluble materials bond with nothing).
    return std::find(list.begin(), list.end(), "*") != list.end() ||
           std::find(list.begin(), list.end(), other) != list.end();
}

bool bases_listed(const std::string& base_a, const std::string& base_b, bool incompatible)
{
    return base_in_list(base_a, base_b, incompatible) || base_in_list(base_b, base_a, incompatible);
}
} // namespace

MaterialCompatibility MaterialType::compatibility(const std::string& type_a, const std::string& type_b)
{
    std::vector<std::string>        fallback_a, fallback_b;
    const std::vector<std::string>& bases_a = base_materials_ref(type_a, fallback_a);
    const std::vector<std::string>& bases_b = base_materials_ref(type_b, fallback_b);

    // Compare every base-material pairing. A material always bonds with itself, so a shared base is
    // compatible even when a material is flagged incompatible with all ("*"). Otherwise an explicit
    // incompatibility anywhere wins; failing that, a listed compatibility means the materials adhere.
    bool compatible = false;
    for (const std::string& ba : bases_a) {
        for (const std::string& bb : bases_b) {
            if (ba == bb) {
                compatible = true;
                continue;
            }
            if (bases_listed(ba, bb, /*incompatible=*/true))
                return MaterialCompatibility::Incompatible;
            if (bases_listed(ba, bb, /*incompatible=*/false))
                compatible = true;
        }
    }

    return compatible ? MaterialCompatibility::Compatible : MaterialCompatibility::Unknown;
}

bool MaterialType::bonds(const std::string& type_a, const std::string& type_b)
{
    return compatibility(type_a, type_b) == MaterialCompatibility::Compatible;
}

} // namespace Slic3r
