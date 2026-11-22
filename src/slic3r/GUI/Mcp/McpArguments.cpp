// Argument checking and name matching for the command catalog: the schema validator, the normalizer that
// reads what agents send in place of a schema's form, what tells a command's person's form from its silent
// one, the words and scores of the searches, and the edit distance every "did you mean" ranks with. It
// includes no wx header: it is a unit of the library libslic3r_automation, which is built without wx.

#include "McpArguments.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

std::string json_type_name(const json& v)
{
    if (v.is_boolean()) return "boolean";
    if (v.is_number_integer() || v.is_number_unsigned()) return "integer";
    if (v.is_number()) return "number";
    if (v.is_string()) return "string";
    if (v.is_array()) return "array";
    if (v.is_object()) return "object";
    return "null";
}

bool type_matches(const json& value, const json& type)
{
    if (type.is_array())
        return std::any_of(type.begin(), type.end(), [&](const json& t) { return type_matches(value, t); });
    if (!type.is_string())
        return true;
    const std::string t = type.get<std::string>();
    if (t == "number")
        return value.is_number();
    if (t == "integer")
        return is_whole_number(value);
    return json_type_name(value) == t;
}

// Whether `value`, a call's arguments, meets `cond`, the "if" of with_silent(): it is no person's form, so what the
// silent form requires applies. {"minProperties": 1} holds for any argument, {"not": {"enum": [...]}} for arguments
// other than those listed; any other "if" is not with_silent()'s, and never holds.
bool silent_form(const json& cond, const json& value)
{
    if (cond == json{{"minProperties", 1}})
        return !value.empty();
    if (const auto no = cond.find("not"); cond.size() == 1 && no != cond.end() && no->is_object() && no->size() == 1)
        if (const auto forms = no->find("enum"); forms != no->end() && forms->is_array())
            return std::find(forms->begin(), forms->end(), value) == forms->end();
    return false;
}

std::string validate_value(const json& schema, const json& value, const std::string& path)
{
    if (!schema.is_object())
        return {};
    if (auto it = schema.find("type"); it != schema.end() && !type_matches(value, *it))
        return path + " must be of type " + (it->is_string() ? it->get<std::string>() : it->dump()) + ", got " + json_type_name(value);
    if (auto it = schema.find("enum"); it != schema.end() && it->is_array() &&
        std::find(it->begin(), it->end(), value) == it->end())
        return path + " must be one of " + it->dump();
    // A bound is read only when it is a number: a plugin operation's schema may be draft-04, whose
    // exclusiveMinimum is a boolean that qualifies minimum.
    if (value.is_number()) {
        if (auto it = schema.find("minimum"); it != schema.end() && it->is_number() && value.get<double>() < it->get<double>())
            return path + " must be >= " + it->dump();
        if (auto it = schema.find("exclusiveMinimum"); it != schema.end() && it->is_number() && value.get<double>() <= it->get<double>())
            return path + " must be > " + it->dump();
        if (auto it = schema.find("maximum"); it != schema.end() && it->is_number() && value.get<double>() > it->get<double>())
            return path + " must be <= " + it->dump();
    }
    if (value.is_array()) {
        if (auto it = schema.find("minItems");
            it != schema.end() && it->is_number_integer() && it->get<long long>() > 0 && value.size() < it->get<std::size_t>())
            return path + " must hold at least " + it->dump() + " item" + (it->get<long long>() == 1 ? "" : "s");
        if (auto it = schema.find("items"); it != schema.end())
            for (std::size_t i = 0; i < value.size(); ++i)
                if (std::string err = validate_value(*it, value[i], path + "[" + std::to_string(i) + "]"); !err.empty())
                    return err;
    }
    if (value.is_object() && schema.contains("properties")) {
        const json& props   = schema["properties"];
        const auto  missing = [&](const json& required) -> std::string {
            if (required.is_array())
                for (const json& key : required)
                    if (key.is_string() && !value.contains(key.get<std::string>()))
                        return (path.empty() ? std::string() : path + ".") + key.get<std::string>() + " is required";
            return {};
        };
        const bool closed = schema.value("additionalProperties", true) == false;
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string child = path.empty() ? it.key() : path + "." + it.key();
            if (auto p = props.find(it.key()); p != props.end()) {
                if (std::string err = validate_value(*p, it.value(), child); !err.empty())
                    return err;
            } else if (closed) {
                std::vector<std::string> names;
                for (auto k = props.begin(); k != props.end(); ++k)
                    names.push_back(k.key());
                return "unknown argument " + child + " (accepted: " + json(names).dump() + ")";
            }
        }
        // What is required, after the properties, so that a wrong value or an argument the tool does not take (an
        // old name for a required one) is named before what is missing.
        if (auto it = schema.find("required"); it != schema.end())
            if (std::string err = missing(*it); !err.empty())
                return err;
        // with_silent()'s shapes, and no other use of if: what the silent form requires, once the call is not one of
        // the person's forms.
        const auto cond = schema.find("if"), then = schema.find("then");
        if (cond != schema.end() && then != schema.end() && then->is_object() && silent_form(*cond, value))
            if (auto it = then->find("required"); it != then->end())
                if (std::string err = missing(*it); !err.empty())
                    return err;
    }
    return {};
}

// Argument names agents reach for in place of the ones the tools take, each with the names it may
// stand for, tried in order.
const std::vector<std::pair<std::string, std::vector<std::string>>>& argument_aliases()
{
    static const std::vector<std::pair<std::string, std::vector<std::string>>> aliases{
        {"ref", {"target"}},          {"key", {"keys"}},        {"text", {"value"}},   {"action", {"id", "button"}},
        {"object", {"objects"}},      {"plate", {"plates"}},    {"filter", {"query"}}, {"settings", {"values"}},
        // A capture's output file, where the tool has no path of its own.
        {"path", {"save_path"}},
        // The Design commands' feature index and a feature's card fields, which their functions also read
        // under these names.
        {"index", {"feature"}},       {"params", {"fields"}},   {"values", {"fields"}},
        // The Design commands' two variant arguments, what the result does to the bodies (operation) and which kind
        // of thing a command makes (type): a command that has one of them reads the other as it.
        {"type", {"operation"}},      {"operation", {"type"}},
    };
    return aliases;
}

// The arguments that pick a command's variant where it is not a type, named for what is picked (project_export's
// format, object_add_primitive's shape). A guessed type stands for the one of them a command takes, where it takes
// exactly one and no type of its own; with two, either could be meant.
constexpr const char* kVariantArguments[] = {"format", "shape", "model", "test", "into", "by", "axis", "mode", "window", "page"};

// The one argument of kVariantArguments that `props` has, or nullptr where it has none or several.
const char* only_variant_argument(const json& props)
{
    const char* found = nullptr;
    for (const char* name : kVariantArguments)
        if (props.contains(name)) {
            if (found != nullptr)
                return nullptr;
            found = name;
        }
    return found;
}

bool type_includes(const json& type, const char* name)
{
    return type.is_array() ? std::find(type.begin(), type.end(), name) != type.end() : type == name;
}

// A value in another JSON form than its schema's: a number, boolean, array or object written as a
// string ("2", "true", "[1, 2]"), a plate index as a string where a name may also go ("0" for
// "current" | index), or a single item where an array is wanted.
void coerce_value(const json& schema, json& value)
{
    if (!schema.is_object() || !schema.contains("type"))
        return;
    const json& type = schema["type"];
    if (value.is_string()) {
        json parsed = json::parse(value.get<std::string>(), nullptr, /*allow_exceptions=*/false);
        if (!parsed.is_discarded() && ((!type_matches(value, type) && type_matches(parsed, type)) ||
                                       (type_includes(type, "integer") && parsed.is_number_integer()))) {
            value = std::move(parsed);
            return;
        }
    }
    // Also where null may stand for the array, and is left as it is there (a CAD param whose default is null).
    const bool or_null = type == json::array({"array", "null"});
    if ((type == "array" || or_null) && !value.is_array() && !(or_null && value.is_null())) {
        json item = value;
        if (schema.contains("items"))
            coerce_value(schema["items"], item);
        json wrapped = json::array({std::move(item)});
        if (validate_value(schema, wrapped, "").empty())
            value = std::move(wrapped);
    }
}

// The words of `text` in lower case, split at anything but a letter or a digit. With `camel`, a word
// with a capital after a lower-case letter also counts as its parts: TriangleMesh as triangle and mesh.
std::vector<std::string> text_words(const std::string& text, bool camel)
{
    std::vector<std::string> words;
    std::string              word;
    std::vector<std::size_t> parts; // where each CamelCase part after the first starts in `word`
    const auto               flush = [&] {
        if (word.empty())
            return;
        std::size_t from = 0;
        for (const std::size_t at : parts) {
            words.push_back(word.substr(from, at - from));
            from = at;
        }
        if (!parts.empty())
            words.push_back(word.substr(from));
        words.push_back(std::exchange(word, {}));
        parts.clear();
    };
    unsigned char previous = 0;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (!std::isalnum(u)) {
            flush();
            previous = 0;
            continue;
        }
        if (camel && std::isupper(u) && std::islower(previous))
            parts.push_back(word.size());
        word += char(std::tolower(u));
        previous = u;
    }
    flush();
    return words;
}

// Whether a word of `words` starts with `w`.
bool has_word(const std::vector<std::string>& words, const std::string& w)
{
    return std::any_of(words.begin(), words.end(), [&w](const std::string& word) { return word.compare(0, w.size(), w) == 0; });
}

} // namespace

std::size_t edit_distance(const std::string& a, const std::string& b)
{
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j)
        row[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diag = row[0];
        row[0]           = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t up = row[j];
            row[j]               = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diag                 = up;
        }
    }
    return row[b.size()];
}

void normalize_arguments(const json& schema, json& args)
{
    if (!args.is_object() || !schema.is_object() || !schema.contains("properties"))
        return;
    const json& props = schema["properties"];
    // x and y, and width and height, as window_list reports them, read as the position and size arrays
    // of a tool that takes those as window geometry, arrays of integers, and neither part by its own
    // name. The mm arrays of the 3D tools (object_transform, part_add) hold numbers and never take a pair. Only
    // both parts of a pair fold, and never over an array the call gives.
    struct Pair
    {
        const char* name;
        const char* first;
        const char* second;
    };
    const auto integer_array = [](const json& prop) {
        return prop.is_object() && prop.value("type", json()) == "array" && prop.contains("items") && prop["items"].is_object() &&
               prop["items"].value("type", json()) == "integer";
    };
    for (const Pair& pair : {Pair{"position", "x", "y"}, Pair{"size", "width", "height"}}) {
        if (!props.contains(pair.name) || !integer_array(props[pair.name]) || props.contains(pair.first) || props.contains(pair.second) ||
            args.contains(pair.name) || !args.contains(pair.first) || !args.contains(pair.second))
            continue;
        args[pair.name] = json::array({args[pair.first], args[pair.second]});
        args.erase(pair.first);
        args.erase(pair.second);
    }
    // Moves `alias`'s value to `name` where the schema takes name, the call leaves it out and the value fits: the
    // widget_click tool's action "click" is no button, and its error should name action.
    const auto read_as = [&props, &args](const std::string& alias, const std::string& name) {
        if (!props.contains(name) || args.contains(name))
            return false;
        json value = args[alias];
        coerce_value(props[name], value);
        if (!validate_value(props[name], value, name).empty())
            return false;
        args[name] = std::move(value);
        args.erase(alias);
        return true;
    };
    for (const auto& [alias, names] : argument_aliases()) {
        if (!args.contains(alias) || props.contains(alias))
            continue;
        for (const std::string& name : names)
            if (read_as(alias, name))
                break;
    }
    if (args.contains("type") && !props.contains("type"))
        if (const char* variant = only_variant_argument(props))
            read_as("type", variant);
    for (auto it = args.begin(); it != args.end(); ++it)
        if (auto p = props.find(it.key()); p != props.end())
            coerce_value(*p, it.value());
    // A value outside its own enum that the enum of one other argument, which the call leaves out,
    // takes: view_set's canvas "front" is its camera. The schema keeps each enum to its own values,
    // as listing them under both names would have agents give a value to the wrong one.
    std::vector<std::pair<std::string, std::string>> moves;
    for (const auto& [key, value] : args.items()) {
        const auto own = props.find(key);
        if (own == props.end() || !own->contains("enum") || validate_value(*own, value, key).empty())
            continue;
        std::vector<std::string> fits;
        for (const auto& [name, prop] : props.items())
            if (!args.contains(name) && prop.contains("enum") && validate_value(prop, value, name).empty())
                fits.push_back(name);
        if (fits.size() == 1)
            moves.emplace_back(key, fits.front());
    }
    for (const auto& [from, to] : moves)
        if (!args.contains(to)) {
            args[to] = std::move(args[from]);
            args.erase(from);
        }
}

std::string validate_arguments(const json& schema, const json& args) { return validate_value(schema, args, ""); }

bool is_whole_number(const json& v)
{
    if (v.is_number_integer())
        return true;
    if (!v.is_number_float())
        return false;
    const double d = v.get<double>();
    return std::isfinite(d) && d == std::floor(d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0;
}

bool person_form(const json& args) { return args.is_null() || (args.is_object() && args.empty()); }

std::vector<json> person_forms(const NativeCommand& command)
{
    std::vector<json> forms;
    if (command.has_own_action)
        forms.push_back(json::object());
    if (command.actions.is_array())
        for (const json& action : command.actions)
            if (action.value("input", std::string()).empty())
                forms.push_back(action.at("args"));
    return forms;
}

bool person_form(const NativeCommand& command, const json& args)
{
    const json              given = person_form(args) ? json::object() : args;
    const std::vector<json> forms = person_forms(command);
    return std::find(forms.begin(), forms.end(), given) != forms.end();
}

json silent_args(json args)
{
    if (args.is_object())
        args.erase("silent");
    return args;
}

json with_silent(json schema, const std::vector<json>& person_args)
{
    json required = json::array();
    if (auto it = schema.find("required"); it != schema.end()) {
        required = std::move(*it);
        schema.erase(it);
    }
    const bool only_none = person_args.size() == 1 && person_form(person_args.front());
    const bool has_none  = std::any_of(person_args.begin(), person_args.end(), [](const json& a) { return person_form(a); });
    // What silent says is the command's own: what its silent form still needs, and which calls are its person's forms.
    std::string needs;
    for (std::size_t i = 0; i < required.size(); ++i)
        needs += (i == 0 ? "" : i + 1 == required.size() ? " and " : ", ") + required[i].get<std::string>();
    std::string text = needs.empty() ? "true runs the command without asking; what the call leaves out takes its default." :
                                       "true runs the command without asking; it then needs " + needs +
                                           ", and what else the call leaves out takes its default.";
    text += only_none ? " A call with no arguments does what a person's pick does." :
            has_none  ? " A call with no arguments, or with exactly a Speed Dial action's arguments, does what a person's pick of "
                        "that action does." :
                        " A call with exactly a Speed Dial action's arguments does what a person's pick of that action does.";
    schema["properties"]["silent"] = {{"type", "boolean"}, {"enum", {true}}, {"description", std::move(text)}};
    if (!required.empty()) {
        schema["if"]   = only_none ? json{{"minProperties", 1}} : json{{"not", {{"enum", person_args}}}};
        schema["then"] = {{"required", std::move(required)}};
    }
    return schema;
}

std::string argument_error(const NativeCommand& command, json& args)
{
    normalize_arguments(command.input_schema, args);
    return validate_arguments(command.input_schema, args);
}

std::vector<std::string> query_words(const std::string& query)
{
    std::vector<std::string> words = text_words(query, false);
    std::vector<std::string> longer;
    std::copy_if(words.begin(), words.end(), std::back_inserter(longer), [](const std::string& w) { return w.size() >= 3; });
    return longer.empty() ? words : longer;
}

std::pair<int, int> match_score(const std::vector<std::string>& words, const std::string& name, const std::string& text)
{
    const std::vector<std::string> in_name = text_words(name, true);
    const std::vector<std::string> in_text = text_words(text, true);
    std::pair<int, int>            score;
    for (const std::string& w : words)
        if (has_word(in_name, w)) {
            ++score.first;
            ++score.second;
        } else if (has_word(in_text, w))
            ++score.first;
    return score;
}

std::string first_sentence(const std::string& text)
{
    std::string line = text;
    std::replace(line.begin(), line.end(), '\n', ' ');
    for (std::size_t dot = line.find('.'); dot != std::string::npos; dot = line.find('.', dot + 1)) {
        const bool abbreviation = dot >= 3 && (line.compare(dot - 3, 3, "e.g") == 0 || line.compare(dot - 3, 3, "i.e") == 0);
        if (!abbreviation && (dot + 1 == line.size() || line[dot + 1] == ' '))
            return line.substr(0, dot + 1);
    }
    return line;
}

}}} // namespace Slic3r::GUI::Mcp
