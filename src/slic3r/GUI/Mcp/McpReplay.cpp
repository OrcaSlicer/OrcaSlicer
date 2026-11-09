#include "McpReplay.hpp"
#include "McpPaths.hpp"
#include "McpTypes.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
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
std::optional<json> resolve_placeholder(const std::string& key, const Replay& replay, const ReplayContext& ctx)
{
    if (key == "replay_dir")
        return json(path_to_utf8(ctx.replay_dir));
    if (key == "out_dir")
        return json(path_to_utf8(ctx.out_dir));
    if (key.rfind("env:", 0) == 0) {
        const char* v = std::getenv(key.substr(4).c_str());
        return json(v ? std::string(v) : std::string());
    }
    if (key.rfind("vars.", 0) == 0) {
        auto it = replay.vars.find(key.substr(5));
        if (it != replay.vars.end())
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

// How a substitution went: in lenient mode an unresolvable placeholder is kept as written; otherwise
// the first one fails the substitution and is recorded here, so run_replay can say why.
struct Substitution
{
    bool        lenient = false;
    std::string err;
    std::string failed; // the placeholder's body, e.g. "jobs.prefs"
};

// The 1-based index of the step that saves its job as `name`, or 0.
std::size_t step_saving(const Replay& replay, const std::string& name)
{
    for (std::size_t i = 0; i < replay.steps.size(); ++i)
        if (replay.steps[i].save_as == name)
            return i + 1;
    return 0;
}

void fail_placeholder(Substitution& sub, const std::string& key, const std::string& text, const Replay& replay)
{
    sub.failed = key;
    sub.err    = "unknown placeholder " + text;
    if (key.rfind("jobs.", 0) == 0)
        if (const std::size_t saver = step_saving(replay, key.substr(5)); saver != 0)
            sub.err += ": no job is saved as " + key.substr(5) + " yet; step " + std::to_string(saver) + " saves it";
}

json substitute_value(const json& value, const Replay& replay, const ReplayContext& ctx, Substitution& sub, int depth)
{
    if (depth > 8) {
        sub.err = "variable substitution nests too deeply";
        return value;
    }
    if (value.is_array() || value.is_object()) {
        json out = value;
        for (auto it = out.begin(); it != out.end(); ++it)
            *it = substitute_value(*it, replay, ctx, sub, depth);
        return out;
    }
    if (!value.is_string())
        return value;
    static const std::regex placeholder(R"(\$\{([^}]+)\})");
    const std::string       text = value.get<std::string>();
    std::smatch             whole;
    if (std::regex_match(text, whole, placeholder)) {
        auto v = resolve_placeholder(whole[1].str(), replay, ctx);
        if (!v) {
            if (!sub.lenient)
                fail_placeholder(sub, whole[1].str(), text, replay);
            return value;
        }
        // A var may itself contain placeholders (e.g. "${replay_dir}/part.stl").
        return substitute_value(*v, replay, ctx, sub, depth + 1);
    }
    std::string out;
    auto        begin = std::sregex_iterator(text.begin(), text.end(), placeholder);
    std::size_t last  = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        out.append(text, last, std::size_t(it->position()) - last);
        last   = std::size_t(it->position() + it->length());
        auto v = resolve_placeholder((*it)[1].str(), replay, ctx);
        if (!v) {
            if (!sub.lenient) {
                fail_placeholder(sub, (*it)[1].str(), it->str(), replay);
                return value;
            }
            out += it->str();
            continue;
        }
        const json resolved = substitute_value(*v, replay, ctx, sub, depth + 1);
        out += resolved.is_string() ? resolved.get<std::string>() : resolved.dump();
    }
    out.append(text, last, std::string::npos);
    return json(out);
}

// "line L, column C" of a 1-based byte offset into `text`, as nlohmann's parse errors give it.
std::string text_position(const std::string& text, std::size_t byte)
{
    const std::size_t end  = std::min(byte, text.size());
    std::size_t       line = 1, column = 1;
    for (std::size_t i = 0; i + 1 < end; ++i) {
        if (text[i] == '\n') {
            ++line;
            column = 1;
        } else
            ++column;
    }
    return "line " + std::to_string(line) + ", column " + std::to_string(column);
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

std::string inline_replay_text(const json& replay) { return replay.is_string() ? replay.get<std::string>() : replay.dump(); }

std::optional<Replay> parse_replay(const std::string& text, std::string& err)
{
    json doc;
    try {
        doc = json::parse(text, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
    } catch (const json::parse_error& e) {
        // nlohmann's text reads "[json.exception.parse_error.101] parse error at line 3, column 25:
        // syntax error ..."; keep the reason after the first ": " and give the position in one form.
        const std::string what = e.what();
        const auto        sep  = what.find(": ");
        err = "the replay is not valid JSON: " + text_position(text, e.byte) + ": " + (sep == std::string::npos ? what : what.substr(sep + 2));
        return std::nullopt;
    }
    if (!doc.is_object() || !doc.contains("orca_replay") || doc["orca_replay"] != 1) {
        err = "the replay must be an object with \"orca_replay\": 1";
        return std::nullopt;
    }
    if (!doc.contains("steps") || !doc["steps"].is_array()) {
        err = "the replay needs a \"steps\" array";
        return std::nullopt;
    }
    Replay replay;
    if (doc.contains("name")) {
        if (!doc["name"].is_string()) {
            err = "\"name\" must be a string";
            return std::nullopt;
        }
        replay.name = doc["name"].get<std::string>();
    }
    if (doc.contains("vars")) {
        if (!doc["vars"].is_object()) {
            err = "\"vars\" must be an object";
            return std::nullopt;
        }
        replay.vars = doc["vars"];
    }
    for (std::size_t i = 0; i < doc["steps"].size(); ++i) {
        const json&       s     = doc["steps"][i];
        const std::string where = "step " + std::to_string(i + 1);
        if (!s.is_object() || !s.contains("tool")) {
            err = where + " needs a \"tool\" name";
            return std::nullopt;
        }
        // Each field is checked for its type, so a wrong one is an error naming it, never an exception.
        auto bad = [&](const char* field, const char* type) {
            err = where + ": \"" + field + "\" must be " + type;
            return std::nullopt;
        };
        if (!s["tool"].is_string())
            return bad("tool", "a string");
        ReplayStep step;
        step.tool = s["tool"].get<std::string>();
        if (s.contains("args")) {
            if (!s["args"].is_object())
                return bad("args", "an object");
            step.args = s["args"];
        }
        if (s.contains("expect")) {
            if (!s["expect"].is_object())
                return bad("expect", "an object");
            step.expect = s["expect"];
        }
        if (s.contains("optional")) {
            if (!s["optional"].is_boolean())
                return bad("optional", "true or false");
            step.optional = s["optional"].get<bool>();
        }
        if (s.contains("await")) {
            if (!s["await"].is_boolean())
                return bad("await", "true or false");
            step.await = s["await"].get<bool>();
        }
        if (s.contains("save_as")) {
            if (!s["save_as"].is_string())
                return bad("save_as", "a string");
            step.save_as = s["save_as"].get<std::string>();
        }
        if (s.contains("timeout_s")) {
            if (!s["timeout_s"].is_number_integer() || s["timeout_s"].get<long long>() <= 0)
                return bad("timeout_s", "a positive whole number");
            step.timeout_s = int(std::min<long long>(s["timeout_s"].get<long long>(), 86400));
        }
        if (!step.await && step.save_as.empty()) {
            err = where + ": \"await\": false needs \"save_as\" so a later step can wait for the job";
            return std::nullopt;
        }
        replay.steps.push_back(std::move(step));
    }
    return replay;
}

// A step's arguments with their placeholders filled in, except Python source, which runs as written (D7):
// the code of a run_script call, of one a call_tool makes, and of the Python steps of an inline replay.
json substitute_args(const std::string& tool, const json& args, const Replay& replay, const ReplayContext& ctx, Substitution& sub)
{
    if (!args.is_object())
        return substitute_value(args, replay, ctx, sub, 0);
    json out = json::object();
    for (auto it = args.begin(); it != args.end(); ++it) {
        const std::string& key = it.key();
        if (tool == "run_script" && key == "code") {
            out[key] = it.value();
        } else if (tool == "call_tool" && key == "args" && it.value().is_object()) {
            const json target = substitute_value(args.value("tool", json()), replay, ctx, sub, 0);
            out[key]          = substitute_args(target.is_string() ? target.get<std::string>() : std::string(), it.value(), replay, ctx, sub);
        } else if (tool == "run_replay" && key == "replay" && it.value().is_object() && it.value().contains("steps") &&
                   it.value()["steps"].is_array()) {
            json nested = json::object();
            for (auto field = it.value().begin(); field != it.value().end(); ++field) {
                if (field.key() != "steps") {
                    nested[field.key()] = substitute_value(field.value(), replay, ctx, sub, 0);
                    continue;
                }
                json steps = json::array();
                for (const json& step : field.value()) {
                    if (!step.is_object()) {
                        steps.push_back(substitute_value(step, replay, ctx, sub, 0));
                        continue;
                    }
                    json done = json::object();
                    for (auto part = step.begin(); part != step.end(); ++part)
                        done[part.key()] = part.key() == "args" ? substitute_args(step.value("tool", std::string()), part.value(), replay, ctx, sub) :
                                                                  substitute_value(part.value(), replay, ctx, sub, 0);
                    steps.push_back(std::move(done));
                }
                nested["steps"] = std::move(steps);
            }
            out[key] = std::move(nested);
        } else {
            out[key] = substitute_value(it.value(), replay, ctx, sub, 0);
        }
    }
    return out;
}

json substitute(const json& value, const Replay& replay, const ReplayContext& ctx, std::string& err)
{
    Substitution sub;
    json         out = substitute_value(value, replay, ctx, sub, 0);
    if (!sub.err.empty())
        err = sub.err;
    return out;
}

json substitute_lenient(const json& value, const Replay& replay, const ReplayContext& ctx)
{
    Substitution sub;
    sub.lenient = true;
    return substitute_value(value, replay, ctx, sub, 0);
}

std::optional<ExpectFailure> check_expect_detail(const json& result, const json& expect)
{
    for (auto it = expect.begin(); it != expect.end(); ++it) {
        ExpectFailure f;
        f.pointer = it.key();
        json::json_pointer ptr;
        try {
            ptr = json::json_pointer(it.key());
        } catch (const std::exception&) {
            f.message = "invalid JSON pointer " + it.key();
            return f;
        }
        f.exists = result.contains(ptr);
        f.actual = f.exists ? result.at(ptr) : json();
        const json& matchers = it.value();
        if (!matchers.is_object()) {
            f.message = it.key() + ": the matcher must be an object such as {\"eq\": 1}";
            return f;
        }
        for (auto m = matchers.begin(); m != matchers.end(); ++m)
            if (auto failure = check_matcher(it.key(), f.actual, f.exists, m.key(), m.value())) {
                f.message  = *failure;
                f.op       = m.key();
                f.expected = m.value();
                return f;
            }
    }
    return std::nullopt;
}

std::optional<std::string> check_expect(const json& result, const json& expect)
{
    if (auto failure = check_expect_detail(result, expect))
        return failure->message;
    return std::nullopt;
}

json run_replay(const Replay& replay, ReplayContext& ctx, const ReplayHost& host, std::optional<std::size_t> only_step)
{
    std::size_t first = 0, last = replay.steps.size();
    if (only_step) {
        if (*only_step < 1 || *only_step > replay.steps.size())
            throw std::out_of_range("the replay has no step " + std::to_string(*only_step));
        first = *only_step - 1;
        last  = *only_step;
    }
    const auto stopping = [&host] { return host.cancelled && host.cancelled(); };
    json       steps    = json::array();
    bool       passed   = true;
    int        failed   = -1;
    const auto report   = [&](json entry) {
        if (host.step_finished)
            host.step_finished(entry);
        steps.push_back(std::move(entry));
    };
    for (std::size_t i = first; i < last; ++i) {
        const ReplayStep& step = replay.steps[i];
        json              entry{{"index", i + 1}, {"tool", step.tool}};
        if (stopping()) {
            entry.update({{"ok", false}, {"error", "cancelled"}, {"cancelled", true}, {"reason", {{"kind", "cancelled"}}}, {"duration_ms", 0}});
            report(std::move(entry));
            passed = false;
            failed = int(i + 1);
            break;
        }
        if (host.step_started)
            host.step_started(i + 1);
        const auto   start = std::chrono::steady_clock::now();
        Substitution sub;
        const json   args = substitute_args(step.tool, step.args, replay, ctx, sub);
        ToolResult   result;
        json         reason;
        if (!sub.err.empty()) {
            result = ToolResult::error(sub.err);
            if (sub.failed.rfind("jobs.", 0) == 0) {
                const std::string name  = sub.failed.substr(5);
                const std::size_t saver = step_saving(replay, name);
                reason = {{"kind", "missing_job"}, {"name", name}, {"saved_by", saver != 0 ? json(saver) : json()}};
            } else if (!sub.failed.empty())
                reason = {{"kind", "placeholder"}, {"text", "${" + sub.failed + "}"}};
        } else
            result = host.call(step.tool, args);

        if (!result.is_error && is_job_envelope(result)) {
            const std::string job = result.data["job"].get<std::string>();
            if (step.await) {
                if (auto done = host.wait_job(job, step.timeout_s))
                    result = std::move(*done);
                else if (stopping())
                    result = ToolResult::error("cancelled", {{"job", job}});
                else {
                    std::string message = "timed out after " + std::to_string(step.timeout_s) + " s waiting for job " + job;
                    json details{{"job", job}};
                    // A job a dialog holds up waits for someone to answer it, which the step itself can't
                    // while it waits; the dialog is named, as nothing else in the result says so.
                    if (const json dialogs = host.open_dialogs ? host.open_dialogs() : json::array();
                        dialogs.is_array() && !dialogs.empty()) {
                        message += " with the dialog \"" + dialogs.back().value("title", std::string()) +
                                   "\" open: a step can't answer a dialog it opens, so register an auto_answer_dialogs rule for it "
                                   "before the step, or give the step \"await\": false and answer the dialog in the steps after it";
                        details["dialogs"] = dialogs;
                    }
                    result = ToolResult::error(message, std::move(details));
                    reason = {{"kind", "timeout"}, {"seconds", step.timeout_s}};
                }
            } else {
                ctx.jobs[step.save_as] = job;
            }
        }

        entry["args"] = args;
        bool ok       = !result.is_error;
        if (ok) {
            entry["result"] = result.data;
            // run_script reports a script that raised, was cancelled or ran out of time in a successful
            // result, as run_replay reports a failed step; inside a replay that fails the step, also when an
            // call_tool made the call. A job left running (await: false) has no outcome yet.
            const std::string target = step.tool == "call_tool" ? args.value("tool", std::string()) : step.tool;
            if (target == "run_script" && !is_job_envelope(result) && result.data.value("status", "") != "finished") {
                entry["error"] = result.data.value("message", "the Python script did not finish");
                reason         = {{"kind", "python"}, {"status", result.data.value("status", "")}};
                ok             = false;
            } else if (!step.expect.empty())
                if (auto failure = check_expect_detail(result.data, step.expect)) {
                    entry["expect_failure"] = failure->message;
                    reason = failure->op.empty() ? json{{"kind", "expect_invalid"}} :
                                                   json{{"kind", "expect"},          {"pointer", failure->pointer},
                                                        {"op", failure->op},         {"expected", failure->expected},
                                                        {"actual", failure->actual}, {"exists", failure->exists}};
                    ok = false;
                }
        } else {
            entry["error"] = result.message;
            if (!result.data.is_null())
                entry["details"] = result.data;
            if (reason.is_null())
                reason = {{"kind", "tool"}};
        }
        entry["duration_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        // A stop that came while the step ran fails it whatever it returned: a cancelled slice, for
        // one, still reports its plates.
        const bool cancelled = stopping();
        if (cancelled) {
            ok                 = false;
            entry["error"]     = "cancelled";
            entry["cancelled"] = true;
            reason             = {{"kind", "cancelled"}};
        }
        entry["ok"] = ok;
        if (!ok)
            entry["reason"] = reason;
        report(std::move(entry));
        if (!ok && (!step.optional || cancelled)) {
            passed = false;
            failed = int(i + 1);
            break;
        }
    }
    if (host.end_replay)
        host.end_replay();
    json out{{"passed", passed}, {"name", replay.name}, {"steps", std::move(steps)}};
    if (failed > 0)
        out["failed_step"] = failed;
    return out;
}

}}} // namespace Slic3r::GUI::Mcp
