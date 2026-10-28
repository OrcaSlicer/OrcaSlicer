#include "McpScript.hpp"
#include "McpTypes.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <exception>
#include <regex>
#include <string>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

bool is_job_envelope(const ToolResult& r)
{
    if (r.is_error || !r.data.is_object() || !r.data.contains("job") || !r.data.contains("status"))
        return false;
    const std::string status = r.data.value("status", "");
    return status == "running" || status == "blocked_by_dialog";
}

// Resolves one placeholder body ("vars.model", "env:HOME", ...) to a JSON value.
std::optional<json> resolve_placeholder(const std::string& key, const Script& script, const ScriptContext& ctx)
{
    if (key == "script_dir")
        return json(ctx.script_dir.string());
    if (key == "out_dir")
        return json(ctx.out_dir.string());
    if (key.rfind("env:", 0) == 0) {
        const char* v = std::getenv(key.substr(4).c_str());
        return json(v ? std::string(v) : std::string());
    }
    if (key.rfind("vars.", 0) == 0) {
        auto it = script.vars.find(key.substr(5));
        if (it != script.vars.end())
            return std::optional<json>(it.value());
        return std::nullopt;
    }
    if (key.rfind("jobs.", 0) == 0) {
        auto it = ctx.jobs.find(key.substr(5));
        if (it != ctx.jobs.end())
            return std::optional<json>(it.value());
        return std::nullopt;
    }
    return std::nullopt;
}

json substitute_value(const json& value, const Script& script, const ScriptContext& ctx, std::string& err, int depth)
{
    if (depth > 8) {
        err = "variable substitution nests too deeply";
        return value;
    }
    if (value.is_array() || value.is_object()) {
        json out = value;
        for (auto it = out.begin(); it != out.end(); ++it)
            *it = substitute_value(*it, script, ctx, err, depth);
        return out;
    }
    if (!value.is_string())
        return value;
    static const std::regex placeholder(R"(\$\{([^}]+)\})");
    const std::string       text = value.get<std::string>();
    std::smatch             whole;
    if (std::regex_match(text, whole, placeholder)) {
        auto v = resolve_placeholder(whole[1].str(), script, ctx);
        if (!v) {
            err = "unknown placeholder " + text;
            return value;
        }
        // A var may itself contain placeholders (e.g. "${script_dir}/part.stl").
        return substitute_value(*v, script, ctx, err, depth + 1);
    }
    std::string out;
    auto        begin = std::sregex_iterator(text.begin(), text.end(), placeholder);
    std::size_t last  = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        out.append(text, last, std::size_t(it->position()) - last);
        auto v = resolve_placeholder((*it)[1].str(), script, ctx);
        if (!v) {
            err = "unknown placeholder " + it->str();
            return value;
        }
        const json resolved = substitute_value(*v, script, ctx, err, depth + 1);
        out += resolved.is_string() ? resolved.get<std::string>() : resolved.dump();
        last = std::size_t(it->position() + it->length());
    }
    out.append(text, last, std::string::npos);
    return json(out);
}

bool numeric(const json& v) { return v.is_number(); }

std::optional<std::string> check_matcher(const std::string& pointer, const json& actual, bool exists, const std::string& op,
                                         const json& operand)
{
    auto fail = [&](const std::string& what) {
        return std::optional<std::string>(pointer + ": expected " + op + " " + operand.dump() + ", " + what);
    };
    if (op == "exists")
        return exists == operand.get<bool>() ? std::nullopt : fail(exists ? "but it exists" : "but it is missing");
    if (!exists)
        return fail("but it is missing");
    const std::string got = "got " + actual.dump();
    if (op == "eq")
        return actual == operand ? std::nullopt : fail(got);
    if (op == "ne")
        return actual != operand ? std::nullopt : fail(got);
    if (op == "gt" || op == "ge" || op == "lt" || op == "le") {
        if (!numeric(actual) || !numeric(operand))
            return fail(got + " (not a number)");
        const double a = actual.get<double>(), b = operand.get<double>();
        const bool   ok = op == "gt" ? a > b : op == "ge" ? a >= b : op == "lt" ? a < b : a <= b;
        return ok ? std::nullopt : fail(got);
    }
    if (op == "contains") {
        bool ok = false;
        if (actual.is_string() && operand.is_string())
            ok = actual.get<std::string>().find(operand.get<std::string>()) != std::string::npos;
        else if (actual.is_array())
            ok = std::find(actual.begin(), actual.end(), operand) != actual.end();
        else if (actual.is_object() && operand.is_string())
            ok = actual.contains(operand.get<std::string>());
        return ok ? std::nullopt : fail(got);
    }
    if (op == "regex") {
        if (!actual.is_string() || !operand.is_string())
            return fail(got + " (not a string)");
        try {
            return std::regex_search(actual.get<std::string>(), std::regex(operand.get<std::string>())) ? std::nullopt : fail(got);
        } catch (const std::regex_error&) {
            return fail("but the pattern is not a valid regular expression");
        }
    }
    if (op == "empty") {
        const bool is_empty = actual.is_null() || ((actual.is_array() || actual.is_object() || actual.is_string()) && actual.empty());
        return is_empty == operand.get<bool>() ? std::nullopt : fail(got);
    }
    return std::optional<std::string>(pointer + ": unknown matcher " + op);
}

} // namespace

std::optional<Script> parse_script(const std::string& text, std::string& err)
{
    const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    if (doc.is_discarded()) {
        err = "the script is not valid JSON";
        return std::nullopt;
    }
    if (!doc.is_object() || doc.value("orca_script", 0) != 1) {
        err = "the script must be an object with \"orca_script\": 1";
        return std::nullopt;
    }
    if (!doc.contains("steps") || !doc["steps"].is_array()) {
        err = "the script needs a \"steps\" array";
        return std::nullopt;
    }
    Script script;
    script.name = doc.value("name", "");
    if (doc.contains("vars")) {
        if (!doc["vars"].is_object()) {
            err = "\"vars\" must be an object";
            return std::nullopt;
        }
        script.vars = doc["vars"];
    }
    for (std::size_t i = 0; i < doc["steps"].size(); ++i) {
        const json&       s     = doc["steps"][i];
        const std::string where = "step " + std::to_string(i + 1);
        if (!s.is_object() || !s.contains("tool") || !s["tool"].is_string()) {
            err = where + " needs a \"tool\" name";
            return std::nullopt;
        }
        ScriptStep step;
        step.tool = s["tool"].get<std::string>();
        try {
            if (s.contains("args"))
                step.args = s["args"].is_object() ? s["args"] : throw std::invalid_argument("\"args\" must be an object");
            if (s.contains("expect"))
                step.expect = s["expect"].is_object() ? s["expect"] : throw std::invalid_argument("\"expect\" must be an object");
            step.optional  = s.value("optional", false);
            step.await     = s.value("await", true);
            step.save_as   = s.value("save_as", "");
            step.timeout_s = s.value("timeout_s", 120);
        } catch (const std::exception& e) {
            err = where + ": " + e.what();
            return std::nullopt;
        }
        if (!step.await && step.save_as.empty()) {
            err = where + ": \"await\": false needs \"save_as\" so a later step can wait for the job";
            return std::nullopt;
        }
        script.steps.push_back(std::move(step));
    }
    return script;
}

json substitute(const json& value, const Script& script, const ScriptContext& ctx, std::string& err)
{
    return substitute_value(value, script, ctx, err, 0);
}

std::optional<std::string> check_expect(const json& result, const json& expect)
{
    for (auto it = expect.begin(); it != expect.end(); ++it) {
        json::json_pointer ptr;
        try {
            ptr = json::json_pointer(it.key());
        } catch (const std::exception&) {
            return "invalid JSON pointer " + it.key();
        }
        const bool exists = result.contains(ptr);
        const json actual = exists ? result.at(ptr) : json();
        const json& matchers = it.value();
        if (!matchers.is_object())
            return it.key() + ": the matcher must be an object such as {\"eq\": 1}";
        for (auto m = matchers.begin(); m != matchers.end(); ++m)
            if (auto failure = check_matcher(it.key(), actual, exists, m.key(), m.value()))
                return failure;
    }
    return std::nullopt;
}

json run_script(const Script& script, ScriptContext& ctx, const ScriptHost& host)
{
    json steps  = json::array();
    bool passed = true;
    int  failed = -1;
    for (std::size_t i = 0; i < script.steps.size(); ++i) {
        const ScriptStep& step = script.steps[i];
        json              entry{{"index", i + 1}, {"tool", step.tool}};
        if (host.cancelled && host.cancelled()) {
            entry["ok"]    = false;
            entry["error"] = "cancelled";
            steps.push_back(std::move(entry));
            passed = false;
            failed = int(i + 1);
            break;
        }
        std::string err;
        const json  args = substitute(step.args, script, ctx, err);
        ToolResult  result;
        if (!err.empty())
            result = ToolResult::error(err);
        else
            result = host.call(step.tool, args);

        if (is_job_envelope(result)) {
            const std::string job = result.data["job"].get<std::string>();
            if (step.await) {
                if (auto done = host.wait_job(job, step.timeout_s))
                    result = std::move(*done);
                else
                    result = ToolResult::error("timed out after " + std::to_string(step.timeout_s) + " s waiting for job " + job,
                                               {{"job", job}});
            } else {
                ctx.jobs[step.save_as] = job;
            }
        }

        entry["args"] = args;
        bool ok       = !result.is_error;
        if (ok) {
            entry["result"] = result.data;
            if (!step.expect.empty())
                if (auto failure = check_expect(result.data, step.expect)) {
                    entry["expect_failure"] = *failure;
                    ok                      = false;
                }
        } else {
            entry["error"] = result.message;
            if (!result.data.is_null())
                entry["details"] = result.data;
        }
        entry["ok"] = ok;
        steps.push_back(std::move(entry));
        if (!ok && !step.optional) {
            passed = false;
            failed = int(i + 1);
            break;
        }
    }
    if (host.end_script)
        host.end_script();
    json out{{"passed", passed}, {"name", script.name}, {"steps", std::move(steps)}};
    if (failed > 0)
        out["failed_step"] = failed;
    return out;
}

}}} // namespace Slic3r::GUI::Mcp
