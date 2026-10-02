#include "MaterialType.hpp"

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

// Both tables are data files shipped in <resources>/info so they can be refined (or hand-edited)
// without rebuilding. Each carries a "version" like the vendor profiles do.
constexpr const char* INFO_SUBDIR               = "info";
constexpr const char* MATERIAL_TYPES_FILE       = "material_types.json";
constexpr const char* BASE_COMPATIBILITIES_FILE = "base_compatibilities.json";

// Built-in tables. They exist because the print config defaults (the filament_type enum) are built
// during static initialisation, before the resource path is known, and because a broken installation
// must not leave the material database empty. The built-in material table is the full table; the
// shipped JSON mirrors it and is what users edit.
const std::vector<MaterialTypeInfo>& builtin_material_types()
{
    static const std::vector<MaterialTypeInfo> material_types = {
        // name  min  max  ch_min ch_max adhesion yield thermal base materials
        {"ABS", 190, 300, 50, 65, 1, 0.1, 100, {}},
        {"ABS-CF", 220, 300, 50, 65, 1, 0.1, 100, {"ABS"}},
        {"ABS-GF", 240, 280, 50, 65, 1, 0.1, 100, {"ABS"}},
        {"ASA", 220, 300, 50, 65, 1, 0.1, 100, {"ABS"}},
        {"ASA-CF", 230, 300, 50, 65, 1, 0.1, 100, {"ABS"}},
        {"ASA-GF", 240, 300, 50, 65, 1, 0.1, 100, {"ABS"}},
        {"ASA-AERO", 240, 280, 50, 65, 1, 0.1, 100, {"ABS"}},
        {"BVOH", 190, 240, 0, 70, 1, 0.02, 200, {}},
        {"CoPE", 190, 240, 0, 45, 1, 0.02, 200, {}},
        {"EVA", 175, 220, 0, 50, 1, 0.02, 200, {}},
        {"FLEX", 210, 230, 0, 50, 0.5, 0.02, 1000, {}},
        {"HIPS", 220, 270, 50, 60, 1, 0.02, 200, {}},
        {"PA", 235, 280, 50, 60, 1, 0.02, 100, {}},
        {"PA-CF", 240, 315, 50, 60, 1, 0.02, 100, {"PA"}},
        {"PA-GF", 240, 290, 50, 60, 1, 0.02, 100, {"PA"}},
        {"PA6", 260, 300, 50, 60, 1, 0.02, 100, {}},
        {"PA6-CF", 230, 300, 50, 60, 1, 0.02, 100, {"PA6"}},
        {"PA6-GF", 260, 300, 50, 60, 1, 0.02, 100, {"PA6"}},
        {"PA11", 275, 295, 50, 60, 1, 0.02, 100, {}},
        {"PA11-CF", 275, 295, 50, 60, 1, 0.02, 100, {"PA11"}},
        {"PA11-GF", 275, 295, 50, 60, 1, 0.02, 100, {"PA11"}},
        {"PA12", 250, 270, 50, 60, 1, 0.02, 100, {}},
        {"PA12-CF", 250, 300, 50, 60, 1, 0.02, 100, {"PA12"}},
        {"PA12-GF", 255, 270, 50, 60, 1, 0.02, 100, {"PA12"}},
        {"PAHT", 260, 310, 55, 65, 1, 0.02, 200, {}},
        {"PAHT-CF", 270, 310, 55, 65, 1, 0.02, 200, {"PAHT"}},
        {"PAHT-GF", 270, 310, 55, 65, 1, 0.02, 200, {"PAHT"}},
        {"PC", 240, 300, 60, 70, 1, 0.02, 40, {}},
        {"PC-ABS", 230, 270, 60, 70, 1, 0.02, 80, {"PC", "ABS"}},
        {"PC-CF", 270, 295, 60, 70, 1, 0.02, 80, {"PC"}},
        {"PC-PBT", 260, 300, 60, 70, 1, 0.02, 40, {"PC", "PBT"}},
        {"PCL", 130, 170, 0, 45, 1, 0.02, 200, {}},
        {"PCTG", 220, 300, 0, 55, 2, 0.02, 200, {}},
        {"PE", 175, 260, 45, 60, 1, 0.02, 200, {}},
        {"PE-CF", 175, 260, 45, 60, 1, 0.02, 200, {"PE"}},
        {"PE-GF", 230, 270, 45, 60, 1, 0.02, 200, {"PE"}},
        {"PEBA", 200, 270, 0, 50, 0.5, 0.02, 1000, {}},
        {"PEI-1010", 370, 430, 80, 100, 1, 0.02, 200, {}},
        {"PEI-1010-CF", 380, 430, 80, 100, 1, 0.02, 200, {"PEI-1010"}},
        {"PEI-1010-GF", 380, 430, 80, 100, 1, 0.02, 200, {"PEI-1010"}},
        {"PEI-9085", 350, 390, 80, 100, 1, 0.02, 200, {}},
        {"PEI-9085-CF", 365, 390, 80, 100, 1, 0.02, 200, {"PEI-9085"}},
        {"PEI-9085-GF", 370, 390, 80, 100, 1, 0.02, 200, {"PEI-9085"}},
        {"PEEK", 350, 460, 80, 100, 1, 0.02, 200, {}},
        {"PEEK-CF", 380, 410, 80, 100, 1, 0.02, 200, {"PEEK"}},
        {"PEEK-GF", 375, 410, 80, 100, 1, 0.02, 200, {"PEEK"}},
        {"PEKK", 325, 400, 80, 100, 1, 0.02, 200, {}},
        {"PEKK-CF", 360, 400, 80, 100, 1, 0.02, 200, {"PEKK"}},
        {"PES", 340, 390, 80, 100, 1, 0.02, 200, {}},
        {"PET", 200, 290, 0, 55, 2, 0.3, 100, {}},
        {"PET-CF", 240, 320, 0, 55, 2, 0.3, 100, {"PET"}},
        {"PET-GF", 280, 320, 0, 55, 2, 0.3, 100, {"PET"}},
        {"PETG", 190, 260, 0, 55, 2, 0.3, 100, {"PET"}},
        {"PETG-CF", 230, 290, 0, 55, 1, 0.3, 100, {"PET"}},
        {"PETG-GF", 210, 270, 0, 55, 1, 0.3, 100, {"PET"}},
        {"PHA", 190, 250, 0, 55, 1, 0.02, 200, {}},
        {"PI", 390, 410, 90, 100, 1, 0.02, 200, {}},
        {"PLA", 180, 240, 0, 45, 1, 0.02, 200, {}},
        {"PLA-AERO", 220, 270, 0, 55, 1, 0.02, 200, {"PLA"}},
        {"PLA-CF", 190, 250, 0, 50, 1, 0.02, 200, {"PLA"}},
        {"POM", 210, 250, 50, 65, 1, 0.02, 200, {}},
        {"PP", 200, 240, 45, 60, 1, 0.02, 200, {}},
        {"PP-CF", 210, 250, 45, 60, 1, 0.02, 200, {"PP"}},
        {"PP-GF", 220, 260, 45, 60, 1, 0.02, 200, {"PP"}},
        {"PPA-CF", 260, 300, 55, 70, 1, 0.02, 200, {"PPA"}},
        {"PPA-GF", 260, 290, 55, 70, 1, 0.02, 200, {"PPA"}},
        {"PPS", 300, 345, 90, 100, 1, 0.02, 200, {}},
        {"PPS-CF", 295, 350, 90, 100, 1, 0.02, 200, {"PPS"}},
        {"PPSU", 360, 420, 90, 100, 1, 0.02, 200, {}},
        {"PSU", 350, 380, 90, 100, 1, 0.02, 200, {}},
        {"PVA", 185, 250, 0, 60, 1, 0.02, 200, {}},
        {"PVOH", 185, 250, 0, 60, 1, 0.02, 200, {"PVA"}},
        {"PVB", 190, 250, 0, 55, 1, 0.02, 200, {}},
        {"PVDF", 245, 265, 40, 60, 1, 0.02, 200, {}},
        {"SBS", 195, 250, 0, 55, 1, 0.02, 200, {}},
        {"TPI", 420, 445, 90, 100, 1, 0.02, 200, {}},
        {"TPU", 175, 260, 0, 50, 0.5, 0.02, 1000, {}}
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

// Reads and parses one of the data files from <resources>/info. Returns none when it is missing or
// malformed, in which case the caller keeps the built-in table.
std::optional<json> read_data_file(const char* filename)
{
    if (resources_dir().empty())
        return std::nullopt;
    const fs::path path = fs::path(resources_dir()) / INFO_SUBDIR / filename;
    boost::system::error_code ec;
    if (!fs::exists(path, ec)) {
        BOOST_LOG_TRIVIAL(warning) << "MaterialType: " << path.string() << " not found, keeping the built-in table";
        return std::nullopt;
    }

    try {
        boost::nowide::ifstream ifs(path.string());
        json                    j;
        ifs >> j;
        BOOST_LOG_TRIVIAL(info) << "MaterialType: loaded " << path.string() << ", version "
                                << j.value("version", std::string("unknown"));
        return j;
    } catch (const std::exception& err) {
        BOOST_LOG_TRIVIAL(error) << "MaterialType: failed to parse " << path.string() << ": " << err.what()
                                 << ", keeping the built-in table";
        return std::nullopt;
    }
}

std::optional<std::vector<MaterialTypeInfo>> load_material_types()
{
    const std::optional<json> j = read_data_file(MATERIAL_TYPES_FILE);
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

std::optional<std::vector<BaseMaterialCompatibility>> load_base_compatibilities()
{
    const std::optional<json> j = read_data_file(BASE_COMPATIBILITIES_FILE);
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

    void load()
    {
        if (std::optional<std::vector<MaterialTypeInfo>> types = load_material_types()) {
            // Types only known to the built-in table (the filament_type enum was built from it) must
            // survive a JSON file that lags behind, so merge rather than replace.
            for (const MaterialTypeInfo& builtin : builtin_material_types())
                if (std::none_of(types->begin(), types->end(), [&builtin](const MaterialTypeInfo& t) { return t.name == builtin.name; }))
                    types->push_back(builtin);
            m_types = std::move(*types);
            reindex();
        }
        if (std::optional<std::vector<BaseMaterialCompatibility>> compatibilities = load_base_compatibilities())
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

void MaterialType::load() { MaterialDatabase::instance().load(); }

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