#include "AgentCli.hpp"

#include "PrintConfig.hpp"
#include "Utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/nowide/fstream.hpp>

#include <cmath>
#include <cstring>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>

namespace Slic3r {
namespace {

constexpr int kSchemaVersion = 1;
constexpr int kDefaultLimit = 12;
constexpr int kMaxLimit = 30;
constexpr size_t kMaxRecipeBytes = 1u << 20;

enum class AgentKind { Legacy, File, LiveGui };

struct AgentOperation
{
    const char *id;
    const char *group;
    const char *summary;
    const char *config_key;
    AgentKind kind;
    bool output_action;
    const char *effect;
    const char *owner;
    const char *inputs;
};

const AgentOperation kOperations[] = {
    {"model.add", "model", "Add mesh, 3MF, or other model files to this run.", nullptr, AgentKind::File, false, "project_mutation",
     "CLI positional input files. The GUI owner is Plater::add_model.",
     "files: paths. Combine with an output step such as slice.export_3mf or slice.run. A run with no output action opens the window."},
    {"model.edit.scale", "model", "Scale the loaded models by a factor.", "scale", AgentKind::Legacy, false, "project_mutation", "CLI transform scale", nullptr},
    {"model.edit.rotate", "model", "Rotate the loaded models around Z, in degrees.", "rotate", AgentKind::Legacy, false, "project_mutation", "CLI transform rotate", nullptr},
    {"model.edit.rotate_x", "model", "Rotate the loaded models around X, in degrees.", "rotate_x", AgentKind::Legacy, false, "project_mutation", "CLI transform rotate_x", nullptr},
    {"model.edit.rotate_y", "model", "Rotate the loaded models around Y, in degrees.", "rotate_y", AgentKind::Legacy, false, "project_mutation", "CLI transform rotate_y", nullptr},
    {"model.edit.arrange", "model", "Arrange models on the plate. 0 disables, 1 enables, any other value means auto.", "arrange", AgentKind::Legacy, false, "project_mutation", "CLI transform arrange", nullptr},
    {"model.edit.orient", "model", "Orient models. 0 disables, 1 enables, any other value means auto.", "orient", AgentKind::Legacy, false, "project_mutation", "CLI transform orient", nullptr},
    {"model.edit.ground_largest_face", "model", "Lay each object on the largest convex-hull face and drop it onto the bed.", "ground_largest_face", AgentKind::Legacy, false, "project_mutation", "CLI transform ground_largest_face", nullptr},
    {"model.edit.ground_face_normal", "model", "Lay each object on the convex-hull face whose normal is closest to NX,NY,NZ.", "ground_face_normal", AgentKind::Legacy, false, "project_mutation", "CLI transform ground_face_normal", nullptr},
    {"model.edit.ground_face_point", "model", "Lay each object on the convex-hull face that contains the point X,Y,Z.", "ground_face_point", AgentKind::Legacy, false, "project_mutation", "CLI transform ground_face_point", nullptr},
    {"model.edit.convert_unit", "model", "Convert the units of the loaded models.", "convert_unit", AgentKind::Legacy, false, "project_mutation", "CLI transform convert_unit", nullptr},
    {"model.edit.ensure_on_bed", "model", "Lift objects that sit partly below the bed.", "ensure_on_bed", AgentKind::Legacy, false, "project_mutation", "CLI transform ensure_on_bed", nullptr},
    {"model.split.objects", "model", "Split unconnected parts into separate objects.", nullptr, AgentKind::LiveGui, false, "project_mutation", "ModelObject::split",
     "Plater::split_object(int obj_idx, bool auto_drop) calls ModelObject::split, then updates plates and undo. No legacy CLI flag is registered."},
    {"model.split.parts", "model", "Split the volumes of one object into parts.", nullptr, AgentKind::LiveGui, false, "project_mutation", "Plater::split_volume",
     "The GUI selection decides the object. There is no legacy CLI flag."},
    {"model.merge", "model", "Merge volumes of one object into a single volume.", nullptr, AgentKind::LiveGui, false, "project_mutation", "ModelObject::merge_volumes",
     "Plater::merge(size_t obj_idx, std::vector<int>& vol_indeces) is the GUI wrapper. ObjectList::merge combines whole objects. There is no legacy CLI flag."},
    {"model.insert.clone", "model", "Clone objects from the load list by index counts.", "clone_objects", AgentKind::Legacy, false, "project_mutation", "CLI option clone_objects", nullptr},
    {"model.insert.repetitions", "model", "Repeat the whole model.", "repetitions", AgentKind::Legacy, false, "project_mutation", "CLI transform repetitions", nullptr},
    {"model.insert.assemble", "model", "Arrange the supplied models and merge them into one.", "assemble", AgentKind::Legacy, false, "project_mutation", "CLI transform assemble", nullptr},
    {"model.insert.shape", "model", "Insert a primitive into the open project.", nullptr, AgentKind::LiveGui, false, "project_mutation", "ObjectList::load_shape_object",
     "type_name is Cube, Cylinder, Sphere, Slab, Cone, Disc, or Torus. The call returns without adding a shape when something is already selected. Live GUI only."},
    {"model.insert.text", "model", "Emboss text as a volume on the open project.", nullptr, AgentKind::LiveGui, false, "project_mutation", "GLGizmoEmboss::create_volume",
     "Live GUI text emboss. Loading an SVG file is svg.add, which is a different operation."},
    {"svg.add", "svg", "Load an SVG file as its own model.", nullptr, AgentKind::File, false, "project_mutation", "Model::read_from_file, which calls load_svg",
     "Put the .svg path in files. This loads a model. It does not emboss the SVG onto an existing volume."},
    {"svg.emboss", "svg", "Emboss an SVG onto a volume in the open project.", nullptr, AgentKind::LiveGui, false, "project_mutation", "emboss_svg",
     "emboss_svg(Plater&, svg path, drop position) is the GUI drop path. Live GUI only."},
    {"slice.run", "slice", "Slice plates. 0 slices all plates. N slices plate N.", "slice", AgentKind::Legacy, true, "slice", "CLI action slice", nullptr},
    {"slice.export_3mf", "slice", "Export the project as a 3MF file.", "export_3mf", AgentKind::Legacy, true, "export", "CLI action export_3mf", nullptr},
    {"slice.export_stl", "slice", "Export the objects as one STL.", "export_stl", AgentKind::Legacy, true, "export", "CLI action export_stl", nullptr},
    {"slice.export_stls", "slice", "Export each object as an STL into a directory.", "export_stls", AgentKind::Legacy, true, "export", "CLI action export_stls", nullptr},
    {"slice.inspect_mesh", "slice", "Print a JSON summary of each loaded object, then exit.", "inspect_mesh", AgentKind::Legacy, true, "read", "CLI action inspect_mesh", nullptr},
    {"slice.inspect_paint", "slice", "Print a JSON summary of painted facets, then exit.", "inspect_paint", AgentKind::Legacy, true, "read", "CLI action inspect_paint", nullptr},
    {"slice.info", "slice", "Print model information, then exit.", "info", AgentKind::Legacy, true, "read", "CLI action info", nullptr},
    {"configuration.load_settings", "configuration", "Load process and machine settings from JSON files.", "load_settings", AgentKind::Legacy, false, "cli_override", "CLI option load_settings", nullptr},
    {"configuration.load_filaments", "configuration", "Load filament settings from JSON files.", "load_filaments", AgentKind::Legacy, false, "cli_override", "CLI option load_filaments", nullptr},
    {"configuration.export", "configuration", "Export the resolved settings to a JSON file. Use - for stdout.", "export_settings", AgentKind::Legacy, true, "read", "CLI action export_settings", nullptr},
};

const char *kGroups[] = {"model", "svg", "slice", "configuration"};

const char *kind_name(AgentKind kind)
{
    switch (kind) {
    case AgentKind::Legacy: return "legacy_cli";
    case AgentKind::File: return "legacy_cli";
    case AgentKind::LiveGui: return "live_gui";
    }
    return "live_gui";
}

const char *type_name(ConfigOptionType type)
{
    switch (type) {
    case coFloat: return "float";
    case coFloats: return "floats";
    case coInt: return "int";
    case coInts: return "ints";
    case coString: return "string";
    case coStrings: return "strings";
    case coPercent: return "percent";
    case coPercents: return "percents";
    case coFloatOrPercent: return "float_or_percent";
    case coFloatsOrPercents: return "floats_or_percents";
    case coPoint: return "point";
    case coPoints: return "points";
    case coPoint3: return "point3";
    case coBool: return "bool";
    case coBools: return "bools";
    case coEnum: return "enum";
    case coEnums: return "enums";
    case coPointsGroups: return "point_groups";
    case coIntsGroups: return "int_groups";
    default: return "unknown";
    }
}

const char *mode_name(ConfigOptionMode mode)
{
    switch (mode) {
    case comSimple: return "simple";
    case comAdvanced: return "advanced";
    case comExpert: return "expert";
    case comDevelop: return "develop";
    }
    return "simple";
}

bool icontains(std::string haystack, const std::string &needle)
{
    if (needle.empty())
        return true;
    boost::to_lower(haystack);
    std::string lowered = needle;
    boost::to_lower(lowered);
    return haystack.find(lowered) != std::string::npos;
}

bool same_text(std::string left, std::string right)
{
    boost::to_lower(left);
    boost::to_lower(right);
    return left == right;
}

nlohmann::json ok_envelope(const std::string &operation, nlohmann::json data, nlohmann::json warnings = nlohmann::json::array())
{
    return {
        {"schema_version", kSchemaVersion},
        {"operation", operation},
        {"status", "ok"},
        {"data", std::move(data)},
        {"warnings", std::move(warnings)},
        {"errors", nlohmann::json::array()},
    };
}

nlohmann::json error_envelope(const std::string &operation, const std::string &code, const std::string &message, const std::string &next)
{
    nlohmann::json err = {
        {"code", code},
        {"message", message},
        {"retryable", false},
        {"next", next},
    };
    return {
        {"schema_version", kSchemaVersion},
        {"operation", operation},
        {"status", "error"},
        {"data", nlohmann::json::object()},
        {"warnings", nlohmann::json::array()},
        {"errors", nlohmann::json::array({std::move(err)})},
    };
}

int exit_for_code(const std::string &code)
{
    if (code == "live_gui_required")
        return CLI_UNSUPPORTED_OPERATION;
    return CLI_INVALID_PARAMS;
}

AgentCliResult error_result(const nlohmann::json &envelope)
{
    AgentCliResult result;
    result.exit_code = exit_for_code(envelope["errors"][0]["code"].get<std::string>());
    result.stdout_text = envelope.dump();
    return result;
}

const AgentOperation *find_operation(const std::string &id, std::string &ambiguous_next)
{
    for (const AgentOperation &op : kOperations) {
        if (id == op.id)
            return &op;
    }
    const AgentOperation *found = nullptr;
    int matches = 0;
    std::string candidates;
    for (const AgentOperation &op : kOperations) {
        if (op.config_key != nullptr && id == op.config_key) {
            if (!candidates.empty())
                candidates += ", ";
            candidates += op.id;
            found = &op;
            ++matches;
        }
    }
    if (matches == 1)
        return found;
    if (matches > 1)
        ambiguous_next = "describe one of: " + candidates;
    return nullptr;
}

const ConfigDef *cli_defs[] = {&cli_actions_config_def, &cli_transform_config_def, &cli_misc_config_def};

const ConfigOptionDef *find_cli_def(const std::string &key)
{
    for (const ConfigDef *def : cli_defs) {
        if (const ConfigOptionDef *opt = def->get(key))
            return opt;
    }
    return nullptr;
}

const ConfigOptionDef *find_setting_def(const std::string &key)
{
    return print_config_def.get(key);
}

const ConfigOptionDef *find_any_def(const std::string &key)
{
    if (const ConfigOptionDef *opt = find_cli_def(key))
        return opt;
    return find_setting_def(key);
}

std::string legacy_flag(const ConfigOptionDef &def, const std::string &key)
{
    const std::vector<std::string> args = def.cli_args(key);
    if (args.empty())
        return {};
    return "--" + args.front();
}

std::string one_line(const std::string &text)
{
    std::string out = text;
    for (char &ch : out) {
        if (ch == '\n' || ch == '\r' || ch == '\t')
            ch = ' ';
    }
    return out;
}

std::string truncate_text(const std::string &text, bool &truncated)
{
    constexpr size_t kMax = 160;
    std::string flat = one_line(text);
    if (flat.size() <= kMax) {
        truncated = false;
        return flat;
    }
    truncated = true;
    return flat.substr(0, kMax - 1) + "…";
}

bool finite_bound(float value)
{
    return std::isfinite(value) && std::fabs(value) < 1.0e10f;
}

nlohmann::json value_schema(const ConfigOptionDef &def, const std::string &key)
{
    nlohmann::json value = {{"type", type_name(def.type)}, {"mode", mode_name(def.mode)}};
    if (def.default_value)
        value["default"] = def.default_value->serialize();
    if (finite_bound(def.min))
        value["min"] = def.min;
    if (finite_bound(def.max))
        value["max"] = def.max;
    if (!def.sidetext.empty())
        value["unit"] = one_line(def.sidetext);
    if (!def.cli_params.empty())
        value["cli_params"] = def.cli_params;
    if (!def.category.empty())
        value["category"] = def.category;
    if (!def.enum_values.empty())
        value["enum"] = def.enum_values;
    const std::string flag = legacy_flag(def, key);
    if (!flag.empty())
        value["legacy_flag"] = flag;
    bool truncated = false;
    value["tooltip"] = truncate_text(def.tooltip, truncated);
    if (truncated)
        value["tooltip_truncated"] = true;
    return value;
}

nlohmann::json hit_operation(const AgentOperation &op)
{
    return {
        {"id", op.id},
        {"group", op.group},
        {"summary", op.summary},
        {"availability", kind_name(op.kind)},
        {"next", std::string("describe ") + op.id},
    };
}

nlohmann::json hit_setting(const std::string &key, const ConfigOptionDef &def)
{
    bool truncated = false;
    std::string summary = def.label.empty() ? key : one_line(def.label);
    if (!def.tooltip.empty()) {
        summary += ": ";
        summary += truncate_text(def.tooltip, truncated);
    }
    nlohmann::json hit = {
        {"id", key},
        {"group", "configuration"},
        {"summary", summary},
        {"availability", def.cli == ConfigOptionDef::nocli ? "not_on_cli" : "legacy_cli"},
        {"next", "describe " + key},
    };
    if (truncated)
        hit["summary_truncated"] = true;
    return hit;
}

bool operation_matches(const AgentOperation &op, const std::string &group, const std::string &query)
{
    if (!group.empty() && !same_text(group, op.group))
        return false;
    if (query.empty())
        return true;
    return icontains(op.id, query) || icontains(op.summary, query) || (op.config_key && icontains(op.config_key, query)) || icontains(op.owner, query);
}

bool setting_matches(const std::string &key, const ConfigOptionDef &def, const std::string &query, const std::string &category)
{
    if (!category.empty()) {
        const std::string cat = def.category.empty() ? "uncategorized" : def.category;
        if (!same_text(cat, category))
            return false;
    }
    if (query.empty())
        return true;
    return icontains(key, query) || icontains(def.label, query) || icontains(def.category, query) || icontains(def.tooltip, query);
}

bool catalog_owns_key(const std::string &key)
{
    for (const AgentOperation &op : kOperations) {
        if (op.config_key != nullptr && key == op.config_key)
            return true;
    }
    return false;
}

int clamp_limit(int limit, nlohmann::json &warnings)
{
    if (limit <= 0)
        return kDefaultLimit;
    if (limit > kMaxLimit) {
        warnings.push_back({{"code", "limit_capped"}, {"message", "limit is capped at 30"}});
        return kMaxLimit;
    }
    return limit;
}

int clamp_offset(int offset)
{
    return offset < 0 ? 0 : offset;
}

nlohmann::json page_matches(nlohmann::json matches, int offset, int limit)
{
    const int total = static_cast<int>(matches.size());
    if (offset > total)
        offset = total;
    int end = offset + limit;
    if (end > total)
        end = total;
    nlohmann::json page = nlohmann::json::array();
    for (int i = offset; i < end; ++i)
        page.push_back(std::move(matches[i]));
    nlohmann::json data = {
        {"matches", std::move(page)},
        {"total", total},
        {"offset", offset},
        {"limit", limit},
        {"truncated", end < total},
    };
    if (end < total)
        data["next"] = "search --offset " + std::to_string(end) + " --limit " + std::to_string(limit);
    return data;
}

nlohmann::json configuration_categories()
{
    std::map<std::string, std::pair<int, std::string>> counts;
    for (const auto &entry : print_config_def.options) {
        const std::string category = entry.second.category.empty() ? "uncategorized" : entry.second.category;
        auto &slot = counts[category];
        slot.first += 1;
        if (slot.second.empty())
            slot.second = entry.first;
    }
    nlohmann::json categories = nlohmann::json::array();
    for (const auto &entry : counts) {
        categories.push_back({
            {"id", entry.first},
            {"count", entry.second.first},
            {"example", entry.second.second},
            {"next", "search --group configuration --category " + entry.first},
        });
    }
    return categories;
}

bool known_group(const std::string &group)
{
    if (group.empty())
        return true;
    for (const char *name : kGroups) {
        if (same_text(group, name))
            return true;
    }
    return false;
}

nlohmann::json describe_operation(const AgentOperation &op)
{
    nlohmann::json data = {
        {"id", op.id},
        {"group", op.group},
        {"summary", op.summary},
        {"availability", kind_name(op.kind)},
        {"effect", op.effect},
        {"owner", op.owner},
    };
    if (op.inputs != nullptr)
        data["inputs"] = op.inputs;
    if (op.kind == AgentKind::LiveGui) {
        data["unavailable"] = {
            {"code", "live_gui_required"},
            {"message", "This operation updates the open project, its undo stack, or a GUI tool. This command lists it and does not run it."},
            {"next", "describe " + std::string(op.id)},
        };
        return data;
    }
    if (op.config_key != nullptr) {
        if (const ConfigOptionDef *def = find_any_def(op.config_key)) {
            data["config_key"] = op.config_key;
            data["value"] = value_schema(*def, op.config_key);
            const std::string flag = legacy_flag(*def, op.config_key);
            if (!flag.empty())
                data["legacy_flag"] = flag;
        }
    }
    data["invoke"] = "Add this id to steps in an invoke recipe. stdout of invoke is the existing CLI output, not this discovery envelope.";
    if (std::strcmp(op.group, "configuration") == 0)
        data["effect_note"] = "A legacy CLI override for this run. It is not Plater::on_config_change, and it is stored only when an export writes a project.";
    return data;
}

nlohmann::json describe_setting(const std::string &key, const ConfigOptionDef &def)
{
    const std::string flag = legacy_flag(def, key);
    nlohmann::json data = {
        {"id", key},
        {"group", "configuration"},
        {"summary", def.label.empty() ? key : one_line(def.label)},
        {"availability", flag.empty() ? "not_on_cli" : "legacy_cli"},
        {"effect", "cli_override"},
        {"owner", "PrintConfigDef"},
        {"config_key", key},
        {"value", value_schema(def, key)},
        {"effect_note", "A legacy CLI override for this run. It is not Plater::on_config_change, and it is stored only when an export writes a project."},
    };
    if (!def.tooltip.empty())
        data["tooltip"] = one_line(def.tooltip);
    if (!flag.empty()) {
        data["legacy_flag"] = flag;
        data["invoke"] = "Use this key as a step id. Search before describe when you do not know the key yet.";
    } else {
        data["unavailable"] = {
            {"code", "not_on_cli"},
            {"message", "This setting has no legacy CLI flag."},
            {"next", "search --group configuration --query " + key},
        };
    }
    return data;
}

const char *output_keys[] = {
    "slice", "export_3mf", "export_stl", "export_stls", "export_slicedata",
    "inspect_mesh", "inspect_paint", "info", "export_settings",
};

bool counts_as_output(const std::string &key, const nlohmann::json &value)
{
    bool known = false;
    for (const char *output : output_keys) {
        if (key == output)
            known = true;
    }
    if (!known)
        return false;
    if (value.is_boolean() && !value.get<bool>())
        return false;
    if (value.is_string() && value.get<std::string>().empty())
        return false;
    return true;
}

bool step_needs_files(const AgentOperation *op, const std::string &key)
{
    if (op != nullptr && (std::strcmp(op->group, "model") == 0 || std::strcmp(op->group, "svg") == 0))
        return true;
    return key == "slice" || key == "export_3mf" || key == "export_stl" || key == "export_stls"
        || key == "inspect_mesh" || key == "inspect_paint" || key == "info";
}

bool single_line(const std::string &text)
{
    return text.find('\n') == std::string::npos && text.find('\r') == std::string::npos;
}

bool json_to_cli_value(const ConfigOptionDef &def, const nlohmann::json &value, std::string &out, std::string &error)
{
    if (value.is_null()) {
        error = "a value is required";
        return false;
    }
    if (value.is_string()) {
        out = value.get<std::string>();
        if (!single_line(out)) {
            error = "value must be a single line";
            return false;
        }
        if (def.type == coBool || def.type == coBools)
            out = boost::iequals(out, "true") || out == "1" ? "1" : (boost::iequals(out, "false") || out == "0" ? "0" : out);
        return true;
    }
    if (def.type == coBool || def.type == coBools) {
        if (value.is_boolean()) {
            out = value.get<bool>() ? "1" : "0";
            return true;
        }
        if (value.is_number_integer()) {
            out = value.get<int>() == 0 ? "0" : "1";
            return true;
        }
        error = "expected a boolean";
        return false;
    }
    if (value.is_number()) {
        out = value.dump();
        return true;
    }
    if (!value.is_array()) {
        error = "pass a string, number, boolean, or array";
        return false;
    }
    if (value.empty()) {
        error = "value array is empty";
        return false;
    }
    if (def.type == coStrings) {
        std::vector<std::string> items;
        for (const nlohmann::json &item : value) {
            if (!item.is_string()) {
                error = "string arrays must contain strings";
                return false;
            }
            const std::string text = item.get<std::string>();
            if (!single_line(text)) {
                error = "value must be a single line";
                return false;
            }
            items.push_back(text);
        }
        out = escape_strings_cstyle(items);
        return true;
    }
    std::ostringstream joined;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i != 0)
            joined << ",";
        if (value[i].is_string())
            joined << value[i].get<std::string>();
        else if (value[i].is_number() || value[i].is_boolean())
            joined << value[i].dump();
        else {
            error = "array items must be strings, numbers, or booleans";
            return false;
        }
    }
    out = joined.str();
    if (!single_line(out)) {
        error = "value must be a single line";
        return false;
    }
    return true;
}

bool extension_is(const std::string &path, const char *ext)
{
    const std::string dotted = std::string(".") + ext;
    return path.size() >= dotted.size() && same_text(path.substr(path.size() - dotted.size()), dotted);
}

std::string read_bounded(std::istream &in, std::string &error)
{
    std::string data;
    char buffer[4096];
    while (in) {
        in.read(buffer, sizeof(buffer));
        data.append(buffer, static_cast<size_t>(in.gcount()));
        if (data.size() > kMaxRecipeBytes) {
            error = "input JSON exceeds 1 MiB";
            return {};
        }
    }
    return data;
}

bool parse_json_text(const std::string &text, nlohmann::json &out, std::string &error)
{
    try {
        out = nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error &ex) {
        error = ex.what();
        return false;
    }
    if (!out.is_object()) {
        error = "recipe must be a JSON object";
        return false;
    }
    return true;
}

bool take_option_value(const std::vector<std::string> &args, size_t &index, std::string &out, std::string &error)
{
    const std::string &token = args[index];
    const size_t equals = token.find('=');
    if (equals != std::string::npos) {
        out = token.substr(equals + 1);
        return true;
    }
    if (index + 1 >= args.size()) {
        error = "missing value for " + token;
        return false;
    }
    out = args[++index];
    return true;
}

} // namespace

bool agent_invocation(int argc, char **argv)
{
    if (argc < 2 || argv[1] == nullptr || std::strcmp(argv[1], "agent") != 0)
        return false;
    if (argc == 2)
        return true;
    if (argv[2] == nullptr)
        return false;
    const std::string sub = argv[2];
    return sub == "help" || sub == "--help" || sub == "-h" || sub == "search" || sub == "describe" || sub == "invoke";
}

nlohmann::json agent_help_json()
{
    nlohmann::json groups = nlohmann::json::array();
    const char *summaries[] = {
        "Add, edit, split, merge, and insert models.",
        "Load an SVG as a model, or emboss one in the live GUI.",
        "Slice, export, and inspect with the existing CLI actions.",
        "Change one print setting at a time. Search a category or a query before opening individual keys.",
    };
    const char *examples[][3] = {
        {"model.add", "model.edit.scale", "model.split.objects"},
        {"svg.add", "svg.emboss", nullptr},
        {"slice.run", "slice.export_3mf", "slice.inspect_mesh"},
        {"configuration.load_settings", "configuration.export", nullptr},
    };
    for (size_t i = 0; i < std::size(kGroups); ++i) {
        nlohmann::json ids = nlohmann::json::array();
        for (const char *example : examples[i]) {
            if (example != nullptr)
                ids.push_back(example);
        }
        groups.push_back({
            {"id", kGroups[i]},
            {"summary", summaries[i]},
            {"examples", std::move(ids)},
            {"next", std::string("search --group ") + kGroups[i]},
        });
    }
    nlohmann::json data = nlohmann::json::object();
    data["how"] = "Pick a group, search inside it, then describe one id before invoke.";
    data["groups"] = std::move(groups);
    data["examples"] = nlohmann::json::array();
    data["examples"].push_back("orca-slicer agent search --group model --query split");
    data["examples"].push_back("orca-slicer agent describe model.edit.scale");
    data["examples"].push_back("orca-slicer agent search --group configuration --category Quality");
    data["examples"].push_back("orca-slicer agent describe layer_height");
    data["examples"].push_back("orca-slicer agent invoke --input-json recipe.json");
    nlohmann::json files = nlohmann::json::array();
    files.push_back("part.stl");
    nlohmann::json steps = nlohmann::json::array();
    steps.push_back({{"id", "model.edit.scale"}, {"value", 2}});
    steps.push_back({{"id", "model.edit.rotate"}, {"value", 90}});
    steps.push_back({{"id", "slice.export_3mf"}, {"value", "out.3mf"}});
    nlohmann::json recipe = nlohmann::json::object();
    recipe["files"] = std::move(files);
    recipe["steps"] = std::move(steps);
    data["recipe"] = std::move(recipe);
    return ok_envelope("help", std::move(data));
}

nlohmann::json agent_search_json(const std::string &group, const std::string &query, const std::string &category, int limit, int offset)
{
    if (!known_group(group))
        return error_envelope("search", "invalid_input", "Unknown group.", "search with no --group lists the groups");
    nlohmann::json warnings = nlohmann::json::array();
    const int used_limit = clamp_limit(limit, warnings);
    const int used_offset = clamp_offset(offset);
    const bool configuration = group.empty() || same_text(group, "configuration");
    if (configuration && query.empty() && !category.empty()) {
        nlohmann::json matches = nlohmann::json::array();
        for (const auto &entry : print_config_def.options) {
            if (setting_matches(entry.first, entry.second, "", category) && !catalog_owns_key(entry.first))
                matches.push_back(hit_setting(entry.first, entry.second));
        }
        nlohmann::json data = page_matches(std::move(matches), used_offset, used_limit);
        data["kind"] = "settings";
        data["category"] = category;
        return ok_envelope("search", std::move(data), std::move(warnings));
    }
    if (configuration && query.empty() && category.empty() && (group.empty() ? false : true)) {
        nlohmann::json operations = nlohmann::json::array();
        for (const AgentOperation &op : kOperations) {
            if (std::strcmp(op.group, "configuration") == 0)
                operations.push_back(hit_operation(op));
        }
        nlohmann::json data = nlohmann::json::object();
        data["kind"] = "categories";
        data["operations"] = std::move(operations);
        data["categories"] = configuration_categories();
        data["next"] = "search --group configuration --category Quality";
        return ok_envelope("search", std::move(data), std::move(warnings));
    }
    if (group.empty() && query.empty() && category.empty())
        return agent_help_json();

    nlohmann::json matches = nlohmann::json::array();
    for (const AgentOperation &op : kOperations) {
        if (operation_matches(op, group, query))
            matches.push_back(hit_operation(op));
    }
    if (configuration && !query.empty()) {
        for (const auto &entry : print_config_def.options) {
            if (!catalog_owns_key(entry.first) && setting_matches(entry.first, entry.second, query, category))
                matches.push_back(hit_setting(entry.first, entry.second));
        }
    }
    if (!query.empty()) {
        for (const ConfigDef *catalog : cli_defs) {
            for (const auto &entry : catalog->options) {
                if (catalog_owns_key(entry.first))
                    continue;
                if (!setting_matches(entry.first, entry.second, query, ""))
                    continue;
                const bool action = cli_actions_config_def.has(entry.first);
                const bool transform = cli_transform_config_def.has(entry.first);
                const char *extra_group = action || transform ? (action ? "slice" : "model") : "configuration";
                if (!group.empty() && !same_text(group, extra_group))
                    continue;
                nlohmann::json hit = hit_setting(entry.first, entry.second);
                hit["group"] = extra_group;
                matches.push_back(std::move(hit));
            }
        }
    }
    nlohmann::json data = page_matches(std::move(matches), used_offset, used_limit);
    data["kind"] = "operations";
    return ok_envelope("search", std::move(data), std::move(warnings));
}

nlohmann::json agent_describe_json(const std::string &id)
{
    if (id.empty())
        return error_envelope("describe", "invalid_input", "describe needs one id.", "search --group model");
    std::string ambiguous;
    if (const AgentOperation *op = find_operation(id, ambiguous))
        return ok_envelope("describe", describe_operation(*op));
    if (!ambiguous.empty())
        return error_envelope("describe", "ambiguous", "That key matches more than one operation.", ambiguous);
    if (const ConfigOptionDef *def = find_setting_def(id))
        return ok_envelope("describe", describe_setting(id, *def));
    if (const ConfigOptionDef *def = find_cli_def(id))
        return ok_envelope("describe", describe_setting(id, *def));
    return error_envelope("describe", "not_found", "No operation or setting is named " + id + ".", "search --query " + id);
}

AgentCliResult agent_invoke_json(const nlohmann::json &recipe)
{
    if (!recipe.is_object())
        return error_result(error_envelope("invoke", "invalid_input", "recipe must be a JSON object.", "describe slice.export_3mf"));
    if (!recipe.contains("steps") || !recipe["steps"].is_array() || recipe["steps"].empty())
        return error_result(error_envelope("invoke", "invalid_input", "steps must be a non-empty array.", "describe model.edit.scale"));

    std::vector<std::string> files;
    if (recipe.contains("files")) {
        if (!recipe["files"].is_array())
            return error_result(error_envelope("invoke", "invalid_input", "files must be an array of paths.", "describe model.add"));
        for (const nlohmann::json &file : recipe["files"]) {
            if (!file.is_string())
                return error_result(error_envelope("invoke", "invalid_input", "each file must be a string path.", "describe model.add"));
            const std::string path = file.get<std::string>();
            if (path.empty() || !single_line(path) || path[0] == '-')
                return error_result(error_envelope("invoke", "invalid_input", "file paths must be non-empty and must not start with a dash.", "describe model.add"));
            files.push_back(path);
        }
    }

    std::vector<std::string> args = files;
    bool need_files = false;
    bool need_svg = false;
    bool has_output = false;
    for (const nlohmann::json &step : recipe["steps"]) {
        if (!step.is_object() || !step.contains("id") || !step["id"].is_string())
            return error_result(error_envelope("invoke", "invalid_input", "each step needs a string id.", "describe model.edit.scale"));
        const std::string id = step["id"].get<std::string>();
        if (id == "help" || id == "help_fff" || id == "help_sla")
            return error_result(error_envelope("invoke", "invalid_input", "help is the agent index, not a slice action.", "orca-slicer agent"));
        std::string ambiguous;
        const AgentOperation *op = find_operation(id, ambiguous);
        if (!ambiguous.empty())
            return error_result(error_envelope("invoke", "ambiguous", "That key matches more than one operation.", ambiguous));
        if (op != nullptr && op->kind == AgentKind::LiveGui)
            return error_result(error_envelope("invoke", "live_gui_required", std::string(op->summary) + " Owner: " + op->owner + ". " + (op->inputs ? op->inputs : ""), std::string("describe ") + op->id));
        if (op != nullptr && op->kind == AgentKind::File) {
            need_files = true;
            if (std::strcmp(op->id, "svg.add") == 0)
                need_svg = true;
            continue;
        }
        const std::string key = op != nullptr && op->config_key != nullptr ? op->config_key : id;
        const ConfigOptionDef *def = find_any_def(key);
        if (def == nullptr)
            return error_result(error_envelope("invoke", "not_found", "No operation or setting is named " + id + ".", "search --query " + id));
        if (!step.contains("value"))
            return error_result(error_envelope("invoke", "invalid_input", id + " needs a value.", "describe " + id));
        std::string rendered;
        std::string value_error;
        if (!json_to_cli_value(*def, step["value"], rendered, value_error))
            return error_result(error_envelope("invoke", "invalid_input", id + ": " + value_error, "describe " + id));
        const std::string flag = legacy_flag(*def, key);
        if (flag.empty())
            return error_result(error_envelope("invoke", "not_on_cli", id + " has no legacy CLI flag.", "describe " + id));
        args.push_back(flag + "=" + rendered);
        if (step_needs_files(op, key))
            need_files = true;
        if (counts_as_output(key, step["value"]))
            has_output = true;
    }
    if (need_files && files.empty())
        return error_result(error_envelope("invoke", "invalid_input", "this recipe needs at least one file.", "describe model.add"));
    if (need_svg) {
        bool found_svg = false;
        for (const std::string &file : files) {
            if (extension_is(file, "svg"))
                found_svg = true;
        }
        if (!found_svg)
            return error_result(error_envelope("invoke", "invalid_input", "svg.add needs a .svg path in files.", "describe svg.add"));
    }
    if (!has_output)
        return error_result(error_envelope("invoke", "needs_output_action", "Add an output step such as slice.export_stl, slice.export_3mf, slice.run, or slice.inspect_mesh. A recipe with no output action opens the window.", "describe slice.export_3mf"));

    AgentCliResult result;
    result.forward_to_legacy = true;
    result.legacy_args = std::move(args);
    return result;
}

AgentCliResult agent_cli_run(int argc, char **argv)
{
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i)
        args.emplace_back(argv[i] ? argv[i] : "");
    if (args.size() < 2 || args[1] != "agent")
        return error_result(error_envelope("agent", "invalid_input", "expected the agent command.", "orca-slicer agent"));

    std::string sub = "help";
    size_t index = 2;
    if (args.size() >= 3 && args[2] != "--help" && args[2] != "-h") {
        sub = args[2];
        index = 3;
    }
    if (sub != "help" && sub != "search" && sub != "describe" && sub != "invoke")
        return error_result(error_envelope("agent", "invalid_input", "Unknown agent command.", "orca-slicer agent"));

    std::string group;
    std::string query;
    std::string category;
    std::string input_json;
    std::string positional;
    int limit = 0;
    int offset = 0;
    bool saw_positional = false;
    for (; index < args.size(); ++index) {
        const std::string &token = args[index];
        std::string error;
        if (token == "--group" || token == "--domain" || boost::starts_with(token, "--group=") || boost::starts_with(token, "--domain=")) {
            if (!take_option_value(args, index, group, error))
                return error_result(error_envelope(sub, "invalid_input", error, "orca-slicer agent"));
        } else if (token == "--query" || boost::starts_with(token, "--query=")) {
            if (!take_option_value(args, index, query, error))
                return error_result(error_envelope(sub, "invalid_input", error, "orca-slicer agent"));
        } else if (token == "--category" || boost::starts_with(token, "--category=")) {
            if (!take_option_value(args, index, category, error))
                return error_result(error_envelope(sub, "invalid_input", error, "orca-slicer agent"));
        } else if (token == "--limit" || boost::starts_with(token, "--limit=")) {
            std::string text;
            if (!take_option_value(args, index, text, error))
                return error_result(error_envelope(sub, "invalid_input", error, "orca-slicer agent"));
            try {
                limit = std::stoi(text);
            } catch (const std::exception &) {
                return error_result(error_envelope(sub, "invalid_input", "limit must be an integer.", "orca-slicer agent search --group model"));
            }
        } else if (token == "--offset" || boost::starts_with(token, "--offset=")) {
            std::string text;
            if (!take_option_value(args, index, text, error))
                return error_result(error_envelope(sub, "invalid_input", error, "orca-slicer agent"));
            try {
                offset = std::stoi(text);
            } catch (const std::exception &) {
                return error_result(error_envelope(sub, "invalid_input", "offset must be an integer.", "orca-slicer agent search --group model"));
            }
        } else if (token == "--input-json" || boost::starts_with(token, "--input-json=")) {
            if (!take_option_value(args, index, input_json, error))
                return error_result(error_envelope(sub, "invalid_input", error, "orca-slicer agent invoke --input-json recipe.json"));
        } else if (boost::starts_with(token, "-")) {
            return error_result(error_envelope(sub, "invalid_input", "Unknown option " + token + ".", "orca-slicer agent"));
        } else if (saw_positional) {
            return error_result(error_envelope(sub, "invalid_input", "Only one positional argument is accepted.", "orca-slicer agent"));
        } else {
            positional = token;
            saw_positional = true;
        }
    }

    nlohmann::json body;
    if (sub == "help")
        body = agent_help_json();
    else if (sub == "search")
        body = agent_search_json(group, query.empty() ? positional : query, category, limit, offset);
    else if (sub == "describe")
        body = positional.empty() ? error_envelope("describe", "invalid_input", "describe needs one id.", "search --group model") : agent_describe_json(positional);
    else {
        if (input_json.empty())
            return error_result(error_envelope("invoke", "invalid_input", "invoke needs --input-json.", "orca-slicer agent invoke --input-json recipe.json"));
        std::string text;
        std::string read_error;
        if (input_json == "-") {
            text = read_bounded(std::cin, read_error);
        } else {
            boost::nowide::ifstream in(input_json);
            if (!in)
                return error_result(error_envelope("invoke", "invalid_input", "could not open " + input_json + ".", "describe model.add"));
            text = read_bounded(in, read_error);
        }
        if (!read_error.empty())
            return error_result(error_envelope("invoke", "invalid_input", read_error, "describe model.add"));
        nlohmann::json recipe;
        if (!parse_json_text(text, recipe, read_error))
            return error_result(error_envelope("invoke", "invalid_input", read_error, "describe slice.export_3mf"));
        return agent_invoke_json(recipe);
    }

    AgentCliResult result;
    if (body["status"] != "ok")
        result.exit_code = exit_for_code(body["errors"][0]["code"].get<std::string>());
    result.stdout_text = body.dump();
    return result;
}

}
