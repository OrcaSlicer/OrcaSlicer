#include "slicer/Fixtures.hpp"

#include "core/Result.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Slic3r { namespace Bench {

namespace {

constexpr double bed_size = 350.;

constexpr std::string_view handy_prefix      = "handy:";
constexpr std::string_view procedural_prefix = "procedural:";

bool starts_with(const std::string& text, std::string_view prefix) { return text.compare(0, prefix.size(), prefix) == 0; }

// A plate of 64 short pegs, so every layer has 64 islands to wall and travel between.
TriangleMesh peg_grid()
{
    TriangleMesh mesh = make_cube(64., 64., 2.);
    for (int row = 0; row < 8; ++row)
        for (int column = 0; column < 8; ++column) {
            TriangleMesh peg = make_cylinder(2., 14., 2 * PI / 32);
            peg.translate(float(4 + 8 * column), float(4 + 8 * row), 0.f);
            mesh.merge(peg);
        }
    return mesh;
}

Model procedural(const std::string& shape)
{
    TriangleMesh mesh;
    if (shape == "smoke-cube")
        mesh = make_cube(20., 20., 20.);
    else if (shape == "peg-grid")
        mesh = peg_grid();
    else if (shape == "fine-sphere")
        // About 130,000 triangles, a dense mesh to slice.
        mesh = make_sphere(25., PI / 180);
    else
        throw std::invalid_argument("no procedural shape is called '" + shape + "'");
    Model model;
    model.add_object(shape.c_str(), "", std::move(mesh))->add_instance();
    return model;
}

// Whether a numeric option's value, or each comma-separated item of a list's, reads whole as its type,
// which the slicer's parser does not check, reading "2.5" as the integer 2 and "fast" in a list as 0.
bool reads_whole(const ConfigOptionDef& def, const std::string& value)
{
    const bool             list    = (def.type & coVectorType) != 0;
    const ConfigOptionType scalar  = ConfigOptionType(def.type & ~coVectorType);
    const bool             percent = scalar == coPercent || scalar == coFloatOrPercent;
    if (scalar != coFloat && scalar != coInt && !percent)
        return true;
    std::vector<std::string> items;
    if (list)
        boost::algorithm::split(items, value, boost::algorithm::is_any_of(","));
    else
        items.push_back(value);
    for (std::string& item : items) {
        boost::algorithm::trim(item);
        if (list && def.nullable && item == "nil")
            continue;
        std::istringstream text(item);
        text.imbue(std::locale::classic());
        if (scalar == coInt) {
            long long number = 0;
            text >> number;
        } else {
            double number = 0;
            text >> number;
        }
        if (percent && text.peek() == '%')
            text.get();
        if (text.fail() || text.peek() != std::istringstream::traits_type::eof())
            return false;
    }
    return true;
}

} // namespace

std::optional<std::string> fixture_path(const std::string& id)
{
    if (starts_with(id, handy_prefix))
        return (boost::filesystem::path(resources_dir()) / "handy_models" / id.substr(handy_prefix.size())).string();
    if (starts_with(id, procedural_prefix))
        return std::nullopt;
    throw std::invalid_argument("the fixture '" + id + "' is neither handy: nor procedural:");
}

ProjectParts::~ProjectParts()
{
    release_PlateData_list(plates);
    for (Preset* preset : presets)
        delete preset;
}

FixtureModel load_fixture(const std::string& id)
{
    FixtureModel fixture;
    Model        model;
    if (const std::optional<std::string> path = fixture_path(id)) {
        if (!boost::filesystem::exists(*path)) {
            fixture.unavailable = *path + " is missing";
            return fixture;
        }
        ProjectParts parts;
        // A 3MF read without LoadModel has no objects.
        model = Model::read_from_file(*path, nullptr, nullptr, LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances, &parts.plates,
                                      &parts.presets);
        // Deletes the backup folder a 3MF read makes, so the model's destructor does not start libslic3r's backup
        // thread, which can deadlock the process's exit.
        model.remove_backup_path_if_exist();
    } else {
        model = procedural(id.substr(procedural_prefix.size()));
    }
    model.center_instances_around_point(Vec2d(bed_size / 2, bed_size / 2));
    for (ModelObject* object : model.objects)
        object->ensure_on_bed();
    fixture.model = std::move(model);
    return fixture;
}

DynamicPrintConfig hermetic_config(const Properties& keys)
{
    // apply() creates each option from its definition, so the enum lists keep the keys map they read and write
    // names with, which the copies full_print_config() makes lack.
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    const std::string size = std::to_string(int(bed_size));
    config.set_deserialize_strict({{"printable_area", "0x0," + size + "x0," + size + "x" + size + ",0x" + size},
                                   {"printable_height", size},
                                   {"layer_change_gcode", "G92 E0"},
                                   // Object label comments print an id only the cancel-object feature sets, which
                                   // otherwise differs between passes.
                                   {"gcode_label_objects", "0"}});
    for (const auto& [key, value] : keys) {
        // A key the slicer does not know only lands in the context, which the strict setter ignores.
        ConfigSubstitutionContext context(ForwardCompatibilitySubstitutionRule::Disable);
        config.set_deserialize(key, value, context);
        if (!context.unrecogized_keys.empty())
            throw std::invalid_argument("the slicer has no setting called '" + key + "'");
        // The setter renames a legacy key and may rewrite its value, so the check reads what it set.
        t_config_option_key set_key   = key;
        std::string         set_value = value;
        PrintConfigDef::handle_legacy(set_key, set_value);
        const ConfigOptionDef* def = config.def()->get(set_key);
        if (def == nullptr || !reads_whole(*def, set_value))
            throw std::invalid_argument("the setting '" + key + "' cannot be '" + value + "'");
    }
    return config;
}

}} // namespace Slic3r::Bench
