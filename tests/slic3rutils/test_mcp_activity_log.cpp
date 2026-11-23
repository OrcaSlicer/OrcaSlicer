#include <catch2/catch_test_macros.hpp>

#include "automation/automation_test_utils.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/Mcp/ActivityLog.hpp"
#include "slic3r/GUI/Mcp/JobRegistry.hpp"
#include "slic3r/GUI/Mcp/McpCore.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Mcp;
using nlohmann::json;
using Slic3r::GUI::Mcp::Test::Harness;

namespace {

const std::string kMask = "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";

NativeCommand command(const std::string& key, GuardClass guard = GuardClass::Mutate)
{
    NativeCommand def;
    def.key   = key;
    def.guard = guard;
    return def;
}

// A log whose changes are counted, over a job registry the test fills as the core would.
struct Log
{
    JobRegistry jobs;
    int         signals = 0;
    ActivityLog log{jobs, [this] { ++signals; }};
    uint64_t    next_call = 1;

    // The pane is open: the log records.
    Log() { log.set_recording(true); }

    // Starts a call as the core reports one, and returns its call id.
    uint64_t start(const NativeCommand& def, const json& args, const std::string& parent, const std::string& job, const json& origin = json())
    {
        const uint64_t call = next_call++;
        log.call_started(call, def, args, parent, job, origin);
        return call;
    }
    // Queues a job call as the core does, on the calling thread before its own starts, and returns its call id.
    uint64_t queue(const std::string& parent, const std::string& job, const json& origin = json())
    {
        const uint64_t call = next_call++;
        log.call_queued(call, job, parent, origin);
        return call;
    }
    // Starts a call queued before.
    void begin(uint64_t call, const NativeCommand& def, const json& args, const std::string& parent, const std::string& job,
               const json& origin = json())
    {
        log.call_started(call, def, args, parent, job, origin);
    }
    std::vector<json> entries()
    {
        const json changes = log.changes_since(0);
        return changes["entries"].get<std::vector<json>>();
    }
};

} // namespace

TEST_CASE("the activity log nests a replay's steps under its run and leaves out a person's pick", "[Mcp][ActivityLog]")
{
    Log               t;
    const std::string replay = t.jobs.create("replay_run", "");
    const uint64_t    run    = t.start(command("replay_run"), {{"path", "/tmp/demo.json"}}, "", replay, {{"source", "action_replay"}});
    const uint64_t    look   = t.start(command("object_list", GuardClass::Inspect), json::object(), replay, "");
    t.log.call_ended(look, ToolResult::ok({{"items", json::array()}}));
    const std::string slice_job = t.jobs.create("plate_slice", replay);
    const uint64_t    slice     = t.start(command("plate_slice"), {{"plate", 0}}, replay, slice_job);

    // A person's pick, and a call made under it, are the person's own doing.
    const std::string pick  = t.jobs.create("script_run", "", "", true);
    const uint64_t    mine  = t.start(command("script_run"), json::object(), "", pick, {{"source", "person"}});
    const std::string child = t.jobs.create("object_add_primitive", pick);
    t.log.call_ended(t.start(command("object_add_primitive"), json::object(), pick, child), ToolResult::ok());
    t.log.call_ended(mine, ToolResult::ok());

    t.log.call_ended(slice, ToolResult::ok({{"estimated_time_s", 1234}}));
    t.log.call_ended(run, ToolResult::ok({{"status", "passed"}}));

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 3);
    CHECK(e[0]["tool"] == "replay_run");
    CHECK(e[0]["parent"] == 0);
    CHECK(e[0]["number"] == 1);
    CHECK(e[0]["source"] == "action_replay");
    CHECK(e[0]["job"] == replay);
    CHECK(e[0]["request"] == json{{"tool", "replay_run"}, {"args", {{"path", "/tmp/demo.json"}}}});
    CHECK(e[0]["status"] == "done");
    CHECK(e[0]["response"] == json{{"status", "passed"}});

    CHECK(e[1]["tool"] == "object_list");
    CHECK(e[1]["parent"] == e[0]["id"]);
    CHECK(e[1]["number"] == 1);
    CHECK(e[1]["read"] == true);
    CHECK(e[1]["source"] == "action_replay"); // a step shares its run's source

    CHECK(e[2]["tool"] == "plate_slice");
    CHECK(e[2]["parent"] == e[0]["id"]);
    CHECK(e[2]["number"] == 2);
    CHECK(e[2]["read"] == false);
    CHECK(e[2]["response"] == json{{"estimated_time_s", 1234}});
    CHECK(e[2]["ended"].get<int64_t>() >= e[2]["started"].get<int64_t>());
}

TEST_CASE("the activity log shows an agent's call as the message it sent, and a refused one as failed", "[Mcp][ActivityLog]")
{
    Log        t;
    const json message{{"jsonrpc", "2.0"},
                       {"id", 12},
                       {"method", "tools/call"},
                       {"params", {{"name", "tool_call"}, {"arguments", {{"tool", "design_shell"}, {"args", {{"thickness", 2}}}}}}}};
    const uint64_t call = t.start(command("design_shell"), {{"thickness", 2}}, "", "j9",
                                  {{"source", "agent"}, {"client", "Claude Code"}, {"request", message}});
    std::vector<json> e = t.entries();
    REQUIRE(e.size() == 1);
    CHECK(e[0]["request"] == message);
    CHECK(e[0]["client"] == "Claude Code");
    CHECK(e[0]["status"] == "running");
    CHECK(e[0]["response"].is_null());
    t.log.call_ended(call, ToolResult::error("No body is selected", {{"code", "no_selection"}}));

    const json bad{{"jsonrpc", "2.0"}, {"id", 13}, {"method", "tools/call"}, {"params", {{"name", "plate_slice"}, {"arguments", {{"plat", 0}}}}}};
    t.log.refused("plate_slice", {{"source", "agent"}, {"client", ""}, {"request", bad}},
                  ToolResult::error("Invalid arguments for plate_slice: unknown argument plat", {{"code", "invalid_argument"}}));

    e = t.entries();
    REQUIRE(e.size() == 2);
    CHECK(e[0]["status"] == "failed");
    CHECK(e[0]["error"] == "No body is selected");
    CHECK(e[0]["response"] == json{{"isError", true}, {"message", "No body is selected"}, {"details", {{"code", "no_selection"}}}});
    CHECK(e[1]["tool"] == "plate_slice");
    CHECK(e[1]["number"] == 2);
    CHECK(e[1]["source"] == "agent");
    CHECK(e[1]["request"] == bad);
    CHECK(e[1]["status"] == "failed");
    CHECK(e[1]["error"] == "Invalid arguments for plate_slice: unknown argument plat");
    CHECK(e[1]["job"] == "");

    // A tool_call refused for its target's arguments is the target's call, with the target's arguments.
    const json wrapped{{"jsonrpc", "2.0"},
                       {"id", 14},
                       {"method", "tools/call"},
                       {"params", {{"name", "tool_call"}, {"arguments", {{"tool", "set_variable"}, {"args", {{"nme", "wall"}}}}}}}};
    t.log.refused("set_variable", {{"source", "agent"}, {"request", wrapped}},
                  ToolResult::error("Invalid arguments for set_variable: unknown argument nme"));
    e = t.entries();
    CHECK(e.back()["tool"] == "set_variable");
    CHECK(e.back()["args"] == json{{"nme", "wall"}});
    CHECK(e.back()["request"] == wrapped);
}

TEST_CASE("the activity log masks secrets and typed text in arguments, requests, responses and errors", "[Mcp][ActivityLog]")
{
    Log        t;
    const json args{{"values", {{"printhost_apikey", "s3cret-key"}, {"layer_height", 0.2}}}};
    const json message{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", "settings_set"}, {"arguments", args}}}};
    const uint64_t set = t.start(command("settings_set"), args, "", "j1", {{"source", "agent"}, {"client", ""}, {"request", message}});
    t.log.call_ended(set, ToolResult::error("Printer host refused key s3cret-key", {{"echo", {{"printhost_apikey", "s3cret-key"}}}}));

    const json typed{{"text", "hunter22"}, {"target", {{"label", "Password"}}}};
    const uint64_t type = t.start(command("keyboard_press", GuardClass::Recover), typed, "", "j2", {{"source", "plugin"}, {"plugin", "fill"}});
    t.log.call_ended(type, ToolResult::ok({{"typed", "hunter22"}}));

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 2);
    const std::string all = json(e).dump();
    CHECK(all.find("s3cret-key") == std::string::npos);
    CHECK(all.find("hunter22") == std::string::npos);
    CHECK(e[0]["args"]["values"]["printhost_apikey"] == kMask);
    CHECK(e[0]["args"]["values"]["layer_height"] == 0.2);
    CHECK(e[0]["request"]["params"]["arguments"]["values"]["printhost_apikey"] == kMask);
    CHECK(e[0]["error"] == "Printer host refused key " + kMask);
    CHECK(e[0]["response"]["details"]["echo"]["printhost_apikey"] == kMask);
    CHECK(e[1]["request"] == json{{"tool", "keyboard_press"}, {"args", {{"text", kMask}, {"target", {{"label", "Password"}}}}}});
    CHECK(e[1]["response"] == json{{"typed", kMask}});
    CHECK(e[1]["plugin"] == "fill");
}

TEST_CASE("the activity log caps a request and a response, and keeps its newest entries, dropping the oldest finished", "[Mcp][ActivityLog]")
{
    Log               t;
    const std::string big(ActivityLog::kMaxBytes, 'x');
    const uint64_t    run = t.start(command("script_run"), {{"code", big}}, "", "j1", {{"source", "agent"}});
    std::vector<json> e   = t.entries();
    REQUIRE(e.size() == 1);
    CHECK(e[0]["request"]["truncated"] == true);
    CHECK(e[0]["request"]["bytes"].get<std::size_t>() > ActivityLog::kMaxBytes);
    const uint64_t version = t.log.changes_since(0)["version"].get<uint64_t>();

    // The running call stays; the oldest finished calls go once the log is full.
    for (std::size_t i = 0; i < ActivityLog::kMaxEntries + 1; ++i)
        t.log.call_ended(t.start(command("object_list", GuardClass::Inspect), json::object(), "", ""), ToolResult::ok({{"n", i}}));
    e = t.entries();
    REQUIRE(e.size() == ActivityLog::kMaxEntries);
    CHECK(e[0]["tool"] == "script_run");
    CHECK(e[1]["response"] == json{{"n", 2}});
    CHECK(e.back()["response"] == json{{"n", ActivityLog::kMaxEntries}});
    const json changes = t.log.changes_since(version);
    CHECK_FALSE(changes["reset"].get<bool>());
    CHECK(changes["removed"].size() == 2);

    t.log.call_ended(run, ToolResult::ok({{"result", big}}));
    e = t.entries();
    CHECK(e[0]["response"]["truncated"] == true);
}

TEST_CASE("the activity log reports a change once until it is read, and Clear list keeps what still runs", "[Mcp][ActivityLog]")
{
    Log            t;
    const uint64_t first = t.start(command("plate_slice"), json::object(), "", "j1", {{"source", "agent"}});
    t.start(command("plate_arrange"), json::object(), "", "j2", {{"source", "agent"}});
    CHECK(t.signals == 1);
    json changes = t.log.changes_since(0);
    CHECK(changes["entries"].size() == 2);
    t.log.call_ended(first, ToolResult::ok());
    CHECK(t.signals == 2);
    changes = t.log.changes_since(changes["version"].get<uint64_t>());
    REQUIRE(changes["entries"].size() == 1);
    CHECK(changes["entries"][0]["status"] == "done");

    t.log.clear();
    changes = t.log.changes_since(changes["version"].get<uint64_t>());
    CHECK(changes["reset"] == true);
    REQUIRE(changes["entries"].size() == 1);
    CHECK(changes["entries"][0]["tool"] == "plate_arrange");
}

TEST_CASE("a Run Script session has no entry: each work it runs has one, with the calls made while it runs", "[Mcp][ActivityLog]")
{
    Log               t;
    const std::string session = t.jobs.create("script_run", "", "", true);
    const uint64_t    hosted  = t.start(command("script_run"), json::object(), "", session, {{"source", "run_script"}});
    LineageNode       node(t.jobs, "script_run", session);
    const auto        call_in_session = [&](const char* tool) {
        t.log.call_ended(t.start(command(tool), json::object(), node.id(), t.jobs.create(tool, node.id())), ToolResult::ok());
    };

    const json first{{"work", "cell"}, {"file", "/scripts/demo.py"}, {"cell", 2}, {"title", "Add parts"}, {"lines", {6, 10}},
                     {"code", "# %% Add parts\norca.host.tools.call('object_add_primitive', {})"}};
    t.log.work_started(first);
    call_in_session("object_add_primitive");
    call_in_session("object_arrange");
    t.log.work_ended("finished", "", json{{"added", 2}});
    call_in_session("plate_slice"); // a thread the work started, calling after it ended
    t.log.work_started({{"work", "console"}, {"file", "/scripts/demo.py"}, {"code", "1/0"}});
    call_in_session("app_state");
    t.log.work_ended("raised", "ZeroDivisionError: division by zero", json());
    t.log.call_ended(hosted, ToolResult::ok({{"status", "ended"}}));

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 6);
    CHECK(e[0]["kind"] == "work");
    CHECK(e[0]["number"] == 1);
    CHECK(e[0]["source"] == "run_script");
    CHECK(e[0]["request"] == first);
    CHECK(e[0]["status"] == "done");
    CHECK(e[0]["response"] == json{{"status", "finished"}, {"result", {{"added", 2}}}});
    CHECK(e[1]["parent"] == e[0]["id"]);
    CHECK(e[1]["number"] == 1);
    CHECK(e[2]["parent"] == e[0]["id"]);
    CHECK(e[2]["number"] == 2);
    CHECK(e[3]["tool"] == "plate_slice");
    CHECK(e[3]["parent"] == 0);
    CHECK(e[3]["source"] == "run_script");
    CHECK(e[3]["number"] == 2);
    CHECK(e[4]["kind"] == "work");
    CHECK(e[4]["status"] == "failed");
    CHECK(e[4]["error"] == "ZeroDivisionError: division by zero");
    CHECK(e[5]["parent"] == e[4]["id"]);
    for (const json& entry : e)
        CHECK((entry["tool"] != "script_run" || entry["kind"] == "work")); // the session itself is not listed
}

TEST_CASE("the activity log names the source of every call the core runs, and leaves out a person's pick", "[Mcp][ActivityLog]")
{
    Harness     h;
    ActivityLog log(h.jobs, {});
    log.set_recording(true);
    NativeCommand look = command("object_list", GuardClass::Inspect);
    look.inline_call   = true;
    look.run           = [](const json&, ToolContext&) { return ToolResult::ok(); };
    h.registry.add(look);
    NativeCommand act = command("plate_arrange");
    act.run           = [](const json&, ToolContext&) { return ToolResult::ok(); };
    h.registry.add(act);
    NativeCommand run = command("replay_run");
    run.run           = [](const json&, ToolContext& ctx) { return ctx.call_child("plate_arrange", json::object()); };
    h.registry.add(run);
    h.core = std::make_unique<McpCore>(h.registry, h.invoker, h.jobs, &h.journal, h.timing, WorkerHooks{}, std::vector<CallObserver*>{&log});

    const json message{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", "object_list"}}}};
    CHECK_FALSE(h.core->call(look, json::object(), {{"source", "agent"}, {"client", "Codex"}, {"request", message}}).is_error);
    CHECK_FALSE(h.core->call_person(run, json::object(), {{"source", "action_replay"}}).is_error);
    CHECK_FALSE(h.core->call_person(run, json::object(), {{"source", "command_line"}}).is_error);
    CHECK_FALSE(h.core->call_plugin(act, json::object(), "measure").is_error);
    std::promise<ToolResult> picked;
    REQUIRE_FALSE(h.core->submit(act, json::object(), [&picked](const ToolResult& r) { picked.set_value(r); }).empty());
    CHECK_FALSE(picked.get_future().get().is_error);

    std::vector<json> e = log.changes_since(0)["entries"].get<std::vector<json>>();
    REQUIRE(e.size() == 6);
    CHECK(e[0]["source"] == "agent");
    CHECK(e[0]["client"] == "Codex");
    CHECK(e[0]["request"] == message);
    CHECK(e[1]["source"] == "action_replay");
    CHECK(e[2]["parent"] == e[1]["id"]);
    CHECK(e[2]["source"] == "action_replay");
    CHECK(e[3]["source"] == "command_line");
    CHECK(e[4]["parent"] == e[3]["id"]);
    CHECK(e[5]["source"] == "plugin");
    CHECK(e[5]["plugin"] == "measure");
    for (const json& entry : e)
        CHECK(entry["status"] == "done");

    // A call another call started to outlive it (a recording's encode) is listed under that call.
    CHECK_FALSE(h.core->call(act, json::object(), {{"source", "agent"}, {"under", e[3]["job"]}}).is_error);
    e = log.changes_since(0)["entries"].get<std::vector<json>>();
    REQUIRE(e.size() == 7);
    CHECK(e[6]["parent"] == e[3]["id"]);
    CHECK(e[6]["source"] == "command_line");
}

TEST_CASE("a rule's answer to a dialog is listed with the run or client whose rule it was, linked to the call that set it", "[Mcp][ActivityLog]")
{
    Log        t;
    const json client_rules{{"rules", {{{"match", "Save changes"}, {"button", "no"}}}}};
    const uint64_t set = t.start(command("dialog_auto_answer", GuardClass::Recover), client_rules, "", "j1",
                                 {{"source", "agent"}, {"client", "Codex"}});
    t.log.rules_set("j1", 1);
    t.log.call_ended(set, ToolResult::ok());

    const std::string replay = t.jobs.create("replay_run", "");
    const uint64_t    run    = t.start(command("replay_run"), json::object(), "", replay, {{"source", "action_replay"}});
    const std::string step   = t.jobs.create("dialog_auto_answer", replay);
    const uint64_t    rules  = t.start(command("dialog_auto_answer", GuardClass::Recover),
                                       {{"rules", {{{"match", "Overwrite"}, {"button", "yes"}}}}}, replay, step);
    t.log.rules_set(step, 2);
    t.log.call_ended(rules, ToolResult::ok());

    t.log.dialog_answered(1, "", "", {{"title", "Save changes?"}, {"message", "The project has changed."}, {"match", "Save changes"}, {"button", "no"}});
    t.log.dialog_answered(2, replay, "", {{"title", "Overwrite file"}, {"message", ""}, {"match", "Overwrite"}, {"button", "yes"}});
    // A layer the log was never told of links nothing.
    t.log.dialog_answered(9, "", "", {{"title", "Other"}, {"message", ""}, {"match", "Other"}, {"button", "ok"}});
    t.log.call_ended(run, ToolResult::ok());

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 6);
    const json& client = e[3];
    CHECK(client["kind"] == "dialog");
    CHECK(client["parent"] == 0);
    CHECK(client["number"] == 0);
    CHECK(client["source"] == "agent");
    CHECK(client["client"] == "Codex"); // the client whose rule it was
    CHECK(client["status"] == "done");
    CHECK(client["rule"] == e[0]["id"]);
    CHECK(client["request"]["title"] == "Save changes?");
    const json& in_run = e[4];
    CHECK(in_run["kind"] == "dialog");
    CHECK(in_run["parent"] == e[1]["id"]);
    CHECK(in_run["source"] == "action_replay");
    CHECK(in_run["rule"] == e[2]["id"]);
    CHECK(e[5]["rule"] == 0);
    CHECK(e[1]["number"] == 2); // the dialogs took no number
}

TEST_CASE("the activity log masks a replay's steps in its request and result, and a waited job's repeated result", "[Mcp][ActivityLog]")
{
    Log        t;
    const json step{{"tool", "keyboard_press"}, {"args", {{"text", "hunter22"}}}};
    const json replay{{"replay", {{"orca_replay_version", "0.2.0"}, {"steps", json::array({step})}}}};
    const uint64_t run = t.start(command("replay_run"), replay, "", "j1", {{"source", "agent"}});
    t.log.call_ended(run, ToolResult::ok({{"passed", true}, {"steps", json::array({{{"tool", "keyboard_press"}, {"args", {{"text", "hunter22"}}}, {"ok", true}}})}}));

    // A replay given as its JSON text is read for its steps too.
    const json as_text{{"replay", json{{"steps", json::array({step})}}.dump()}};
    t.log.call_ended(t.start(command("replay_run"), as_text, "", "j2", {{"source", "agent"}}), ToolResult::ok({{"passed", true}}));

    // A keyboard job whose result repeats the text, then an app_wait that returns the same result.
    const uint64_t typed = t.start(command("keyboard_press", GuardClass::Recover), {{"text", "s3cret-pass"}}, "", "j3", {{"source", "agent"}});
    t.log.call_ended(typed, ToolResult::ok({{"typed", "s3cret-pass"}}));
    NativeCommand wait = command("app_wait", GuardClass::Inspect);
    t.log.call_ended(t.start(wait, {{"for", "job"}, {"job", "j3"}}, "", "", {{"source", "agent"}}), ToolResult::ok({{"typed", "s3cret-pass"}}));

    const std::string all = json(t.entries()).dump();
    CHECK(all.find("hunter22") == std::string::npos);
    CHECK(all.find("s3cret-pass") == std::string::npos);
}

TEST_CASE("a script that raised and a replay whose step failed are failed entries, with the reason", "[Mcp][ActivityLog]")
{
    Log t;
    t.log.call_ended(t.start(command("script_run"), {{"code", "1/0"}}, "", "j1", {{"source", "agent"}}),
                     ToolResult::ok({{"status", "raised"}, {"message", "ZeroDivisionError: division by zero"}}));
    t.log.call_ended(t.start(command("replay_run"), {{"path", "/tmp/r.json"}}, "", "j2", {{"source", "agent"}}),
                     ToolResult::ok({{"passed", false},
                                     {"failed_step", 2},
                                     {"steps", json::array({{{"index", 1}, {"ok", true}},
                                                            {{"index", 2}, {"ok", false}, {"expect_failure", "The step finished, but /plates/0/outcome was failed, not ok."}}})}}));
    t.log.call_ended(t.start(command("script_run"), {{"code", "x = 1"}}, "", "j3", {{"source", "agent"}}), ToolResult::ok({{"status", "finished"}}));

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 3);
    CHECK(e[0]["status"] == "failed");
    CHECK(e[0]["error"] == "ZeroDivisionError: division by zero");
    CHECK(e[1]["status"] == "failed");
    CHECK(e[1]["error"] == ""); // the pane words it
    CHECK(e[1]["failure"] == json{{"step", 2}, {"why", "The step finished, but /plates/0/outcome was failed, not ok."}});
    CHECK(e[2]["status"] == "done");
    CHECK(e[2]["error"] == "");
}

TEST_CASE("the activity log masks the variables a replay's steps use, given as an object or as JSON text", "[Mcp][ActivityLog]")
{
    Log        t;
    const json replay{{"orca_replay_version", "0.2.0"},
                      {"vars", {{"pw", "hunter22"}, {"quote", "say\"cheese"}, {"password", "s3cret-var"}}},
                      {"steps", json::array({{{"tool", "keyboard_press"}, {"args", {{"text", "${vars.pw}"}}}},
                                             {{"tool", "keyboard_press"}, {"args", {{"text", "${vars.quote}"}}}}})}};
    t.log.call_ended(t.start(command("replay_run"), {{"replay", replay}}, "", "j1", {{"source", "agent"}}), ToolResult::ok({{"passed", true}}));
    t.log.call_ended(t.start(command("replay_run"), {{"replay", replay.dump()}}, "", "j2", {{"source", "agent"}}),
                     ToolResult::ok({{"passed", true}}));
    // The caller's variables win over the replay's own.
    t.log.call_ended(t.start(command("replay_run"), {{"replay", replay}, {"vars", {{"pw", "other-pass"}}}}, "", "j3", {{"source", "agent"}}),
                     ToolResult::ok({{"passed", true}}));

    const std::string all = json(t.entries()).dump();
    CHECK(all.find("hunter22") == std::string::npos);
    CHECK(all.find("other-pass") == std::string::npos);
    CHECK(all.find("s3cret-var") == std::string::npos);
    CHECK(all.find("cheese") == std::string::npos); // in the text form, the quote is escaped
}

TEST_CASE("a replay run of one step reports that step's failure by its number", "[Mcp][ActivityLog]")
{
    Log t;
    t.log.call_ended(t.start(command("replay_run"), {{"path", "/tmp/r.json"}}, "", "j1", {{"source", "action_replay"}}),
                     ToolResult::ok({{"passed", false},
                                     {"failed_step", 7},
                                     {"steps", json::array({{{"index", 7}, {"ok", false}, {"error", "No objects on plate 1"}}})}}));
    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 1);
    CHECK(e[0]["failure"] == json{{"step", 7}, {"why", "No objects on plate 1"}});
}

TEST_CASE("an old Run Script session that ends late leaves the newer session's work running", "[Mcp][ActivityLog]")
{
    // Run all: the panel ends the session, starts a new one and its work, and only then does the old session's call end.
    Log               t;
    const std::string old_session = t.jobs.create("script_run", "", "", true);
    const uint64_t    old_hosted  = t.start(command("script_run"), json::object(), "", old_session, {{"source", "run_script"}});
    const std::string new_session = t.jobs.create("script_run", "", "", true);
    const uint64_t    new_hosted  = t.start(command("script_run"), json::object(), "", new_session, {{"source", "run_script"}});
    t.log.work_started({{"work", "file"}, {"file", "/scripts/demo.py"}, {"code", "orca.host.tools.call('app_state', {})"}});
    t.log.call_ended(old_hosted, ToolResult::ok({{"status", "ended"}}));

    LineageNode node(t.jobs, "script_run", new_session);
    t.log.call_ended(t.start(command("app_state", GuardClass::Inspect), json::object(), node.id(), ""), ToolResult::ok());
    std::vector<json> e = t.entries();
    REQUIRE(e.size() == 2);
    CHECK(e[0]["kind"] == "work");
    CHECK(e[0]["status"] == "running");
    CHECK(e[1]["parent"] == e[0]["id"]);

    t.log.work_ended("raised", "NameError: name 'x' is not defined", json());
    t.log.call_ended(new_hosted, ToolResult::ok({{"status", "ended"}}));
    e = t.entries();
    CHECK(e[0]["status"] == "failed");
    CHECK(e[0]["error"] == "NameError: name 'x' is not defined");
}

TEST_CASE("the activity log lists nothing until it records, and while off still ends what it lists", "[Mcp][ActivityLog]")
{
    // Off at first: no call, refusal, dialog answer or work is listed.
    JobRegistry jobs;
    ActivityLog off(jobs, {});
    off.call_started(1, command("plate_slice"), json::object(), "", "j1", {{"source", "agent"}});
    off.call_ended(1, ToolResult::ok());
    off.refused("plate_slice", {{"source", "agent"}, {"request", json::object()}}, ToolResult::error("Invalid arguments"));
    off.dialog_answered(1, "", "", {{"title", "Save changes?"}, {"match", "Save"}, {"button", "no"}});
    off.work_started({{"work", "console"}, {"code", "1"}});
    off.work_ended("finished", "", json());
    CHECK(off.changes_since(0)["entries"].empty());

    // A call and a work listed while recording end while off.
    Log               t;
    const uint64_t    slice   = t.start(command("plate_slice"), json::object(), "", "j2", {{"source", "agent"}});
    const std::string session = t.jobs.create("script_run", "", "", true);
    const uint64_t    hosted  = t.queue("", session, {{"source", "run_script"}});
    t.begin(hosted, command("script_run"), json::object(), "", session, {{"source", "run_script"}});
    t.log.work_started({{"work", "cell"}, {"code", "x = 1"}});
    t.log.set_recording(false);
    t.log.call_ended(slice, ToolResult::ok());
    t.log.call_ended(hosted, ToolResult::ok()); // the session ends under its work, which the panel then ends
    t.log.work_ended("cancelled", "", json());
    std::vector<json> e = t.entries();
    REQUIRE(e.size() == 2);
    CHECK(e[0]["status"] == "done");
    CHECK(e[1]["kind"] == "work");
    CHECK(e[1]["status"] == "failed");
    CHECK(e[1]["failure"] == json{{"script", "cancelled"}});

    // Nothing that comes while off is listed.
    t.log.call_ended(t.start(command("object_list", GuardClass::Inspect), json::object(), "", "", {{"source", "agent"}}), ToolResult::ok());
    t.log.work_started({{"work", "console"}, {"code", "2"}});
    CHECK(t.entries().size() == 2);
}

TEST_CASE("runs that started before the log records list their later calls under their own source", "[Mcp][ActivityLog]")
{
    Log t;
    t.log.set_recording(false);

    // A replay that started while off queues a nested replay that outlives it, then ends; many other calls end before
    // the nested one starts.
    const std::string replay = t.jobs.create("replay_run", "");
    const uint64_t    run    = t.queue("", replay, {{"source", "action_replay"}});
    t.begin(run, command("replay_run"), json::object(), "", replay, {{"source", "action_replay"}});
    const std::string nested = t.jobs.create("replay_run", replay);
    const uint64_t    later  = t.queue(replay, nested);
    // A person's pick, whose child is queued the same way.
    const std::string pick  = t.jobs.create("script_run", "", "", true);
    const uint64_t    picks = t.queue("", pick, {{"source", "person"}});
    t.begin(picks, command("script_run"), json::object(), "", pick, {{"source", "person"}});
    const std::string under_pick = t.jobs.create("object_add_primitive", pick);
    const uint64_t    child      = t.queue(pick, under_pick);
    // A recording_stop that queues its encode under itself, then returns.
    const std::string stop    = t.jobs.create("recording_stop", replay);
    const uint64_t    stops   = t.queue(replay, stop);
    t.begin(stops, command("recording_stop"), json::object(), replay, stop);
    const std::string encode  = t.jobs.create("recording_encode", "");
    const uint64_t    encodes = t.queue("", encode, {{"source", "agent"}, {"under", stop}});
    t.log.call_ended(stops, ToolResult::ok());
    t.log.call_ended(run, ToolResult::ok({{"passed", true}}));
    t.log.call_ended(picks, ToolResult::ok());
    for (int i = 0; i < 2100; ++i) {
        const std::string other = t.jobs.create("object_list", "");
        const uint64_t    call  = t.queue("", other, {{"source", "agent"}});
        t.begin(call, command("object_list", GuardClass::Inspect), json::object(), "", other, {{"source", "agent"}});
        t.log.call_ended(call, ToolResult::ok());
        t.jobs.finish(other, ToolResult::ok());
    }
    // A Run Script session that started while off.
    const std::string session = t.jobs.create("script_run", "", "", true);
    const uint64_t    hosted  = t.queue("", session, {{"source", "run_script"}});
    t.begin(hosted, command("script_run"), json::object(), "", session, {{"source", "run_script"}});

    t.log.set_recording(true);
    t.begin(later, command("replay_run"), json::object(), replay, nested);
    const std::string step = t.jobs.create("plate_slice", nested);
    t.log.call_ended(t.start(command("plate_slice"), json::object(), nested, step), ToolResult::ok());
    t.begin(child, command("object_add_primitive"), json::object(), pick, under_pick);
    t.begin(encodes, command("recording_encode"), json::object(), "", encode, {{"source", "agent"}, {"under", stop}});
    LineageNode node(t.jobs, "script_run", session);
    t.log.call_ended(t.start(command("object_list", GuardClass::Inspect), json::object(), node.id(), ""), ToolResult::ok());

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 4); // the nested replay, its step, the encode and the session's call; not the pick's child
    CHECK(e[0]["tool"] == "replay_run");
    CHECK(e[0]["parent"] == 0);
    CHECK(e[0]["source"] == "action_replay");
    CHECK(e[1]["tool"] == "plate_slice");
    CHECK(e[1]["parent"] == e[0]["id"]);
    CHECK(e[2]["tool"] == "recording_encode");
    CHECK(e[2]["source"] == "action_replay");
    CHECK(e[3]["tool"] == "object_list");
    CHECK(e[3]["source"] == "run_script");
    CHECK(e[3]["parent"] == 0);
}

TEST_CASE("a wait on a job started while off stays masked, and one on a job with no record shows nothing", "[Mcp][ActivityLog]")
{
    Log t;
    t.log.set_recording(false);
    const uint64_t typed = t.queue("", "j1", {{"source", "agent"}});
    t.begin(typed, command("keyboard_press", GuardClass::Recover), {{"text", "hunter22"}}, "", "j1", {{"source", "agent"}});
    t.log.call_ended(typed, ToolResult::ok({{"typed", "hunter22"}}));
    t.log.set_recording(true);

    NativeCommand wait = command("app_wait", GuardClass::Inspect);
    t.log.call_ended(t.start(wait, {{"for", "job"}, {"job", "j1"}}, "", "", {{"source", "agent"}}), ToolResult::ok({{"typed", "hunter22"}}));
    t.log.call_ended(t.start(wait, {{"for", "job"}, {"job", "j77"}}, "", "", {{"source", "agent"}}),
                     ToolResult::error("Job j77 failed: wrong password hunter33"));

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 2);
    CHECK(json(e).dump().find("hunter22") == std::string::npos);
    CHECK(e[0]["response"]["typed"] == kMask);
    CHECK(e[1]["withheld"] == true);
    CHECK(e[1]["response"].is_null());
    CHECK(e[1]["status"] == "failed");
    CHECK(e[1]["error"] == "");
    CHECK(json(e).dump().find("hunter33") == std::string::npos);
}

TEST_CASE("an answer links the call that set its rule layer, whoever made it, and none for a call not listed", "[Mcp][ActivityLog]")
{
    Log        t;
    const json client_rules{{"rules", {{{"match", "Save"}, {"button", "no"}}}}};
    const json plugin_rules{{"rules", {{{"match", "Overwrite"}, {"button", "yes"}}}}};
    const auto set_rules   = [&](const json& rules, const std::string& parent, const std::string& job, const json& origin, int layer) {
        const uint64_t call = t.start(command("dialog_auto_answer", GuardClass::Recover), rules, parent, job, origin);
        t.log.rules_set(job, layer);
        t.log.call_ended(call, ToolResult::ok());
    };
    set_rules(client_rules, "", "j1", {{"source", "agent"}, {"client", "Codex"}}, 1);
    set_rules(plugin_rules, "", "j2", {{"source", "plugin"}, {"plugin", "fill"}}, 2);
    // A plugin operation's child call, and an agent's through tool_call, whose layers the runtime gives the plugin and the
    // client.
    const std::string operation = t.jobs.create("plugin.fill.run", "", "fill");
    const uint64_t    operates  = t.start(command("plugin.fill.run"), json::object(), "", operation, {{"source", "plugin"}, {"plugin", "fill"}});
    set_rules(plugin_rules, operation, t.jobs.create("dialog_auto_answer", operation, "fill"), json(), 3);
    const std::string wrapper = t.jobs.create("tool_call", "");
    const uint64_t    wraps   = t.start(command("tool_call"), json::object(), "", wrapper, {{"source", "agent"}, {"client", "Codex"}});
    set_rules(client_rules, wrapper, t.jobs.create("dialog_auto_answer", wrapper), json(), 4);
    // While off, the client replaces its rules with one of the same match and button.
    t.log.set_recording(false);
    set_rules(client_rules, "", "j5", {{"source", "agent"}, {"client", "Claude Code"}}, 5);
    t.log.set_recording(true);

    t.log.dialog_answered(5, "", "", {{"title", "Save changes?"}, {"match", "Save"}, {"button", "no"}});
    t.log.dialog_answered(2, "", "fill", {{"title", "Overwrite?"}, {"match", "Overwrite"}, {"button", "yes"}});
    t.log.dialog_answered(3, "", "fill", {{"title", "Overwrite?"}, {"match", "Overwrite"}, {"button", "yes"}});
    t.log.dialog_answered(4, "", "", {{"title", "Save changes?"}, {"match", "Save"}, {"button", "no"}});
    t.log.call_ended(operates, ToolResult::ok());
    t.log.call_ended(wraps, ToolResult::ok());

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 10);
    const json& by_unlisted = e[6];
    CHECK(by_unlisted["kind"] == "dialog");
    CHECK(by_unlisted["rule"] == 0); // set while off: the earlier call with the same rule is not taken for it
    CHECK(by_unlisted["client"] == "Claude Code");
    CHECK(e[7]["rule"] == e[1]["id"]);
    CHECK(e[8]["rule"] == e[3]["id"]); // the plugin operation's child call
    CHECK(e[8]["plugin"] == "fill");
    CHECK(e[9]["rule"] == e[5]["id"]); // the call made through tool_call
    CHECK(e[9]["client"] == "Codex");
}

TEST_CASE("a Run Script session's rule links its answers in later works", "[Mcp][ActivityLog]")
{
    Log               t;
    const std::string session = t.jobs.create("script_run", "", "", true);
    const uint64_t    hosted  = t.start(command("script_run"), json::object(), "", session, {{"source", "run_script"}});
    t.log.work_started({{"work", "cell"}, {"code", "orca.host.tools.call('dialog_auto_answer', rules)"}});
    const std::string rule_job = t.jobs.create("dialog_auto_answer", session);
    const uint64_t    rules    = t.start(command("dialog_auto_answer", GuardClass::Recover), {{"rules", {{{"match", "Save"}, {"button", "no"}}}}},
                                         session, rule_job);
    t.log.rules_set(rule_job, 7);
    t.log.call_ended(rules, ToolResult::ok());
    t.log.work_ended("finished", "", json());
    t.log.work_started({{"work", "cell"}, {"code", "orca.host.tools.call('project_new', {})"}});
    t.log.dialog_answered(7, session, "", {{"title", "Save changes?"}, {"match", "Save"}, {"button", "no"}});
    t.log.work_ended("finished", "", json());
    t.log.call_ended(hosted, ToolResult::ok());

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 4);
    CHECK(e[3]["kind"] == "dialog");
    CHECK(e[3]["parent"] == e[2]["id"]); // under the work running as it was answered
    CHECK(e[3]["source"] == "run_script");
    CHECK(e[3]["rule"] == e[1]["id"]);
}

TEST_CASE("a dialog's text is masked with what the calls running as it is answered hide", "[Mcp][ActivityLog]")
{
    Log            t;
    const json     args{{"values", {{"printhost_apikey", "s3cret-key"}}}};
    const uint64_t set = t.start(command("printer_test_host"), args, "", "j1", {{"source", "agent"}});
    t.log.set_recording(false);
    const uint64_t typed = t.queue("", "j2", {{"source", "agent"}});
    t.begin(typed, command("keyboard_press", GuardClass::Recover), {{"text", "hunter22"}}, "", "j2", {{"source", "agent"}});
    t.log.set_recording(true);
    t.log.dialog_answered(1, "", "", {{"title", "Host refused key s3cret-key"}, {"message", "Wrong password hunter22."}, {"match", "Host"},
                                      {"button", "ok"}});
    t.log.call_ended(set, ToolResult::ok());
    t.log.call_ended(typed, ToolResult::ok());

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 2);
    CHECK(e[1]["request"]["title"] == "Host refused key " + kMask);
    CHECK(e[1]["request"]["message"] == "Wrong password " + kMask + ".");
}

TEST_CASE("a replay's variables are masked inside longer text, and in replay text that escapes them", "[Mcp][ActivityLog]")
{
    Log        t;
    const json replay{{"vars", {{"p", "hunter22"}}},
                      {"steps", json::array({{{"tool", "keyboard_press"}, {"args", {{"text", "password=${vars.p}"}}}}})}};
    t.log.call_ended(t.start(command("replay_run"), {{"replay", replay}}, "", "j1", {{"source", "agent"}}), ToolResult::ok({{"passed", true}}));
    // The same replay as JSON text that writes the secret's letters as \u escapes.
    const std::string escaped = R"({"steps":[{"tool":"keyboard_press","args":{"text":"s3cret-pass"}}]})";
    t.log.call_ended(t.start(command("replay_run"), {{"replay", escaped}}, "", "j2", {{"source", "agent"}}), ToolResult::ok({{"passed", true}}));

    // Replay text with nothing to mask is shown as written.
    const std::string plain = "{ \"steps\": [ { \"tool\": \"object_list\", \"args\": {} } ] }";
    t.log.call_ended(t.start(command("replay_run"), {{"replay", plain}}, "", "j3", {{"source", "agent"}}), ToolResult::ok({{"passed", true}}));

    const std::vector<json> e   = t.entries();
    const std::string       all = json(e).dump();
    CHECK(all.find("hunter22") == std::string::npos);
    CHECK(all.find("cret-pass") == std::string::npos);
    CHECK(e[0]["args"]["replay"]["vars"]["p"] == kMask);
    CHECK(e[2]["args"]["replay"] == plain);
}

TEST_CASE("a job whose secret is too long to keep, or whose record the budget dropped, has its repeated result withheld", "[Mcp][ActivityLog]")
{
    Log               t;
    NativeCommand     wait = command("app_wait", GuardClass::Inspect);
    const std::string long_secret(ActivityLog::kMaxBytes + 1, 's');
    const auto        wait_on = [&](const std::string& job, const ToolResult& result) {
        t.log.call_ended(t.start(wait, {{"for", "job"}, {"job", job}}, "", "", {{"source", "agent"}}), result);
        return t.entries().back();
    };

    // Listed, and not listed.
    t.log.call_ended(t.start(command("keyboard_press", GuardClass::Recover), {{"text", long_secret}}, "", "j1", {{"source", "agent"}}),
                     ToolResult::ok());
    t.log.set_recording(false);
    t.log.call_ended(t.start(command("keyboard_press", GuardClass::Recover), {{"text", long_secret}}, "", "j2", {{"source", "agent"}}),
                     ToolResult::ok());
    t.log.set_recording(true);
    for (const char* job : {"j1", "j2"}) {
        const json waited = wait_on(job, ToolResult::error("Field rejected " + long_secret));
        CHECK(waited["withheld"] == true);
        CHECK(waited["error"] == "");
        CHECK(json(waited).dump().find("sss") == std::string::npos);
    }

    // Records past the byte budget go, the oldest first, listed or not.
    const std::string part(30000, 'p');
    for (int i = 0; i < 160; ++i) {
        const std::string secret = std::to_string(i) + part;
        t.log.call_ended(t.start(command("keyboard_press", GuardClass::Recover), {{"text", secret}}, "", "k" + std::to_string(i),
                                 {{"source", "agent"}}),
                         ToolResult::ok());
    }
    CHECK(wait_on("k0", ToolResult::ok({{"typed", "0" + part}}))["withheld"] == true);
    const json newest = wait_on("k159", ToolResult::ok({{"typed", "159" + part}}));
    CHECK(newest["withheld"] == false);
    CHECK(newest["response"]["typed"] == kMask);
}

TEST_CASE("a job not listed keeps what its rewritten arguments reveal, for a later wait", "[Mcp][ActivityLog]")
{
    Log t;
    t.log.set_recording(false);
    const uint64_t set = t.queue("", "j1", {{"source", "agent"}});
    t.begin(set, command("widget_set_value", GuardClass::Recover), {{"target", "ref_7"}, {"value", "plain"}}, "", "j1", {{"source", "agent"}});
    // The ref resolves to a field found by its label, whose value is hidden.
    t.log.call_args(set, {{"target", {{"label", "Password"}}}, {"value", "hunter22"}});
    t.log.call_ended(set, ToolResult::ok());
    t.log.set_recording(true);

    NativeCommand wait = command("app_wait", GuardClass::Inspect);
    t.log.call_ended(t.start(wait, {{"for", "job"}, {"job", "j1"}}, "", "", {{"source", "agent"}}), ToolResult::ok({{"value", "hunter22"}}));
    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 1);
    CHECK(e[0]["response"]["value"] == kMask);
}

TEST_CASE("a call under a run started while off nests under the listed run above it", "[Mcp][ActivityLog]")
{
    Log               t;
    const std::string outer = t.jobs.create("replay_run", "");
    const uint64_t    run   = t.start(command("replay_run"), json::object(), "", outer, {{"source", "action_replay"}});
    t.log.set_recording(false);
    const std::string nested = t.jobs.create("replay_run", outer);
    const uint64_t    inner  = t.queue(outer, nested);
    t.begin(inner, command("replay_run"), json::object(), outer, nested);
    t.log.set_recording(true);
    const std::string step = t.jobs.create("plate_slice", nested);
    t.log.call_ended(t.start(command("plate_slice"), json::object(), nested, step), ToolResult::ok());
    t.log.call_ended(inner, ToolResult::ok());
    t.log.call_ended(run, ToolResult::ok());

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 2);
    CHECK(e[1]["tool"] == "plate_slice");
    CHECK(e[1]["parent"] == e[0]["id"]);
    CHECK(e[1]["source"] == "action_replay");
}

TEST_CASE("Clear list starts the numbering again and keeps what masks a cleared job's result", "[Mcp][ActivityLog]")
{
    Log t;
    t.log.call_ended(t.start(command("keyboard_press", GuardClass::Recover), {{"text", "hunter22"}}, "", "j1", {{"source", "agent"}}),
                     ToolResult::ok());
    t.log.call_ended(t.start(command("object_list", GuardClass::Inspect), json::object(), "", "", {{"source", "agent"}}), ToolResult::ok());
    t.log.clear();
    REQUIRE(t.entries().empty());

    NativeCommand wait = command("app_wait", GuardClass::Inspect);
    t.log.call_ended(t.start(wait, {{"for", "job"}, {"job", "j1"}}, "", "", {{"source", "agent"}}), ToolResult::ok({{"typed", "hunter22"}}));
    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 1);
    CHECK(e[0]["number"] == 1);
    CHECK(e[0]["response"]["typed"] == kMask);
}

TEST_CASE("a replay's variable that another variable names is masked too", "[Mcp][ActivityLog]")
{
    Log        t;
    const json replay{{"vars", {{"p", "${vars.q}"}, {"q", "hunter22"}}},
                      {"steps", json::array({{{"tool", "keyboard_press"}, {"args", {{"text", "password=${vars.p}"}}}}})}};
    t.log.call_ended(t.start(command("replay_run"), {{"replay", replay}}, "", "j1", {{"source", "agent"}}), ToolResult::ok({{"passed", true}}));
    t.log.call_ended(t.start(command("replay_run"), {{"replay", replay.dump()}}, "", "j2", {{"source", "agent"}}),
                     ToolResult::ok({{"passed", true}}));
    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 2);
    CHECK(json(e).dump().find("hunter22") == std::string::npos);
    CHECK(e[0]["args"]["replay"]["vars"]["q"] == kMask);
}

TEST_CASE("a wait masks what its job hid as the wait ends, and is withheld once the job's record goes", "[Mcp][ActivityLog]")
{
    Log           t;
    NativeCommand wait = command("app_wait", GuardClass::Inspect);
    // The wait starts before the job's arguments are rewritten into what hides the value.
    const uint64_t set = t.start(command("widget_set_value", GuardClass::Recover), {{"target", {{"option", "layer_height"}}}, {"value", "0.2"}},
                                 "", "j1", {{"source", "agent"}});
    const uint64_t waits = t.start(wait, {{"for", "job"}, {"job", "j1"}}, "", "", {{"source", "agent"}});
    t.log.call_args(set, {{"target", {{"label", "Password"}}}, {"value", "hunter22"}});
    t.log.call_ended(set, ToolResult::error("Field rejected hunter22"));
    t.log.call_ended(waits, ToolResult::error("Field rejected hunter22"));
    // The wait starts before the job's result drops its record (a value over kMaxBytes).
    const uint64_t replay = t.start(command("replay_run"), json::object(), "", "j2", {{"source", "agent"}});
    const uint64_t again  = t.start(wait, {{"for", "job"}, {"job", "j2"}}, "", "", {{"source", "agent"}});
    const json     long_step{{"tool", "keyboard_press"}, {"args", {{"text", std::string(ActivityLog::kMaxBytes + 1, 's')}}}};
    t.log.call_ended(replay, ToolResult::ok({{"passed", true}, {"steps", json::array({long_step})}}));
    t.log.call_ended(again, ToolResult::error("Step 1 typed " + std::string(100, 's')));

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 4);
    CHECK(e[1]["error"] == "Field rejected " + kMask);
    CHECK(e[1]["withheld"] == false);
    CHECK(e[3]["withheld"] == true);
    CHECK(e[3]["error"] == "");
}

TEST_CASE("a script that ended other than by raising is worded by the pane, a Run Script work too", "[Mcp][ActivityLog]")
{
    Log t;
    t.log.call_ended(t.start(command("script_run"), {{"code", "x"}}, "", "j1", {{"source", "agent"}}),
                     ToolResult::ok({{"status", "unavailable"}, {"message", "Python is not available in this OrcaSlicer"}}));
    t.log.call_ended(t.start(command("script_run"), {{"code", "x"}}, "", "j2", {{"source", "agent"}}),
                     ToolResult::ok({{"status", "timed_out"}, {"message", "The script ran longer than its 60 s budget"}}));
    t.log.work_started({{"work", "console"}, {"code", "while True: pass"}});
    t.log.work_ended("timed_out", "The script ran longer than its 60 s budget", json());
    // A replay whose result names no failed step.
    t.log.call_ended(t.start(command("replay_run"), json::object(), "", "j3", {{"source", "agent"}}), ToolResult::ok({{"passed", false}}));

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 4);
    CHECK(e[0]["failure"] == json{{"script", "unavailable"}});
    CHECK(e[0]["error"] == "");
    CHECK(e[1]["failure"] == json{{"script", "timed_out"}});
    CHECK(e[2]["kind"] == "work");
    CHECK(e[2]["status"] == "failed");
    CHECK(e[2]["failure"] == json{{"script", "timed_out"}});
    CHECK(e[2]["response"]["message"] == "The script ran longer than its 60 s budget");
    CHECK(e[3]["status"] == "failed");
    CHECK(e[3]["failure"] == json{{"why", ""}});
}

TEST_CASE("an answer links a listed rule call however many rule calls came after it", "[Mcp][ActivityLog]")
{
    Log            t;
    const uint64_t first = t.start(command("dialog_auto_answer", GuardClass::Recover), {{"rules", {{{"match", "Save"}, {"button", "no"}}}}}, "",
                                   "j0", {{"source", "agent"}, {"client", "Codex"}});
    t.log.rules_set("j0", 1);
    t.log.call_ended(first, ToolResult::ok());
    t.log.set_recording(false);
    const int others = int(2 * ActivityLog::kMaxEntries) + 10;
    for (int i = 0; i < others; ++i) {
        const std::string job  = "r" + std::to_string(i);
        const uint64_t    call = t.start(command("dialog_auto_answer", GuardClass::Recover), {{"rules", json::array()}}, "", job,
                                         {{"source", "agent"}, {"client", "Bot"}});
        t.log.rules_set(job, i + 2);
        t.log.call_ended(call, ToolResult::ok());
    }
    t.log.set_recording(true);
    t.log.dialog_answered(1, "", "", {{"title", "Save changes?"}, {"match", "Save"}, {"button", "no"}});
    // Of the layers whose call isn't listed, the newest keep their client, up to 2 × kMaxEntries layers in all.
    t.log.dialog_answered(others + 1, "", "", {{"title", "Newest"}, {"match", "Newest"}, {"button", "ok"}});
    t.log.dialog_answered(2, "", "", {{"title", "Oldest"}, {"match", "Oldest"}, {"button", "ok"}});
    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 4);
    CHECK(e[1]["rule"] == e[0]["id"]);
    CHECK(e[1]["client"] == "Codex");
    CHECK(e[2]["rule"] == 0);
    CHECK(e[2]["client"] == "Bot");
    CHECK(e[3]["client"] == "");
}

TEST_CASE("a dialog's cut message is masked where a hidden value straddles the cut", "[Mcp][ActivityLog]")
{
    Log            t;
    const uint64_t typed = t.start(command("keyboard_press", GuardClass::Recover), {{"text", "hunter22-long-pass"}}, "", "j1",
                                   {{"source", "agent"}});
    t.log.dialog_answered(1, "", "", {{"title", "Rejected"}, {"message", "The field rejected hunter22-lo..."}, {"match", "Rejected"},
                                      {"button", "ok"}});
    t.log.call_ended(typed, ToolResult::ok());
    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 2);
    CHECK(e[1]["request"]["message"] == "The field rejected " + kMask + "...");
}

TEST_CASE("JSON text or arguments nested too deep to read are not shown", "[Mcp][ActivityLog]")
{
    Log           t;
    NativeCommand wait = command("app_wait", GuardClass::Inspect);
    const auto    nested = [](int levels, const std::string& inside) {
        return std::string(levels, '[') + inside + std::string(levels, ']');
    };
    const std::string step = R"({"tool":"keyboard_press","args":{"text":"hunter22"}})";
    // A small replay text one level too deep, a huge one, and one whose comment hides brackets from a count of them.
    const std::string too_deep = R"({"vars":{"deep":)" + nested(ActivityLog::kMaxDepth, "1") + R"(},"steps":[)" + step + "]}";
    const std::string huge     = nested(100000, "");
    const std::string comment  = R"({"vars":{"c":"x"},/* )" + std::string(200, ']') + R"( */"deep":)" + nested(ActivityLog::kMaxDepth, "1") +
                                R"(,"steps":[)" + step + "]}";
    for (const std::string& text : {too_deep, huge, comment})
        t.log.call_ended(t.start(command("replay_run"), {{"replay", text}}, "", "", {{"source", "agent"}}), ToolResult::ok({{"passed", true}}));
    // Arguments too deep: not shown, and a wait on the job withheld.
    json deep_args = "hunter22";
    for (int i = 0; i < ActivityLog::kMaxDepth + 1; ++i)
        deep_args = json::array({deep_args});
    t.log.call_ended(t.start(command("keyboard_press", GuardClass::Recover), {{"text", deep_args}}, "", "j1", {{"source", "agent"}}),
                     ToolResult::ok());
    t.log.call_ended(t.start(wait, {{"for", "job"}, {"job", "j1"}}, "", "", {{"source", "agent"}}), ToolResult::ok({{"typed", "hunter22"}}));

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 5);
    CHECK(json(e).dump().find("hunter22") == std::string::npos);
    CHECK(e[0]["args"]["replay"] == kMask);
    CHECK(e[1]["args"]["truncated"] == true); // too long to show, never parsed
    CHECK(e[2]["args"]["replay"] == kMask);
    CHECK(e[3]["args"] == kMask);
    CHECK(e[4]["withheld"] == true);
}

TEST_CASE("a replay's variables are masked through long alias chains, as objects and arrays, and from the environment", "[Mcp][ActivityLog]")
{
    Log  t;
    json chain = json::object();
    for (int i = 0; i < 10000; ++i)
        chain["v" + std::to_string(i)] = "${vars.v" + std::to_string(i + 1) + "}";
    chain["v10000"] = "hunter22";
    const json typed = json::array({{{"tool", "keyboard_press"}, {"args", {{"text", "a=${vars.v0} b=${vars.o} c=${vars.a} d=${env:ORCA_ACTIVITY_TEST_PW}"}}}}});
    chain["o"]       = {{"x", "obj-secret"}};
    chain["a"]       = json::array({"arr-secret", 4242});
#ifdef _WIN32
    _putenv_s("ORCA_ACTIVITY_TEST_PW", "env-secret");
#else
    setenv("ORCA_ACTIVITY_TEST_PW", "env-secret", 1);
#endif
    const json replay{{"vars", chain}, {"steps", typed}};
    t.log.call_ended(t.start(command("replay_run"), {{"replay", replay}}, "", "j1", {{"source", "agent"}}),
                     ToolResult::error("Field rejected env-secret"));
    const std::string all = json(t.entries()).dump();
    CHECK(all.find("hunter22") == std::string::npos);
    CHECK(all.find("obj-secret") == std::string::npos);
    CHECK(all.find("arr-secret") == std::string::npos);
    CHECK(all.find("4242") == std::string::npos);
    CHECK(all.find("env-secret") == std::string::npos);
}

TEST_CASE("a script refused with a message is worded by the pane", "[Mcp][ActivityLog]")
{
    Log t;
    t.log.call_ended(t.start(command("script_run"), {{"code", "x"}}, "", "j1", {{"source", "agent"}}),
                     ToolResult::ok({{"status", "refused"}, {"message", "Python is shutting down"}}));
    t.log.work_started({{"work", "console"}, {"code", "x"}});
    t.log.work_ended("refused", "Scripts can't run while OrcaSlicer is starting or closing.", json());
    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 2);
    CHECK(e[0]["failure"] == json{{"script", "refused"}});
    CHECK(e[1]["failure"] == json{{"script", "refused"}});
}

TEST_CASE("a dialog masks escaped values, a value both whole and cut, and is not shown past the value limit", "[Mcp][ActivityLog]")
{
    Log            t;
    const uint64_t quoted = t.start(command("keyboard_press", GuardClass::Recover), {{"text", "say\"cheese"}}, "", "j1", {{"source", "agent"}});
    const uint64_t longer = t.start(command("keyboard_press", GuardClass::Recover), {{"text", "hunter22-long-pass"}}, "", "j2",
                                    {{"source", "agent"}});
    t.log.dialog_answered(1, "", "", {{"title", "Rejected"},
                                      {"message", "Sent {\"text\":\"say\\\"cheese\"}; hunter22-long-pass was rejected; again hunter22-lo..."},
                                      {"match", "Rejected"},
                                      {"button", "ok"}});
    t.log.call_ended(quoted, ToolResult::ok());
    t.log.call_ended(longer, ToolResult::ok());

    // A job whose record holds more values than a dialog checks.
    json many = json::array();
    for (std::size_t i = 0; i <= ActivityLog::kMaxDialogValues; ++i)
        many.push_back({{"tool", "keyboard_press"}, {"args", {{"text", "typed-" + std::to_string(i)}}}});
    const uint64_t heavy = t.start(command("replay_run"), {{"replay", {{"steps", many}}}}, "", "j3", {{"source", "agent"}});
    t.log.dialog_answered(1, "", "", {{"title", "Overwrite typed-7?"}, {"message", "typed-7 exists"}, {"match", "Overwrite"}, {"button", "yes"}});
    t.log.call_ended(heavy, ToolResult::ok({{"passed", true}}));

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 5);
    const std::string message = e[2]["request"]["message"].get<std::string>();
    CHECK(message.find("cheese") == std::string::npos);
    CHECK(message.find("hunter22") == std::string::npos);
    CHECK(e[4]["request"]["title"] == kMask);
    CHECK(e[4]["request"]["message"] == kMask);
}

TEST_CASE("a call whose arguments are too deep to read shows no result, and a dialog it runs under no text", "[Mcp][ActivityLog]")
{
    Log  t;
    json deep = 1;
    for (int i = 0; i < ActivityLog::kMaxDepth + 1; ++i)
        deep = json::array({deep});
    const uint64_t run = t.start(command("script_run"), {{"password", "hunter22"}, {"deep", deep}, {"code", "raise ValueError(args['password'])"}},
                                 "", "j1", {{"source", "agent"}});
    t.log.dialog_answered(1, "", "", {{"title", "Error"}, {"message", "ValueError: hunter22"}, {"match", "Error"}, {"button", "ok"}});
    t.log.call_ended(run, ToolResult::ok({{"status", "raised"}, {"message", "ValueError: hunter22"}}));
    // An agent's request, and a result, too deep to read.
    const json message{{"jsonrpc", "2.0"}, {"method", "tools/call"}, {"params", {{"name", "object_list"}, {"arguments", {{"x", deep}}}}}};
    t.log.call_ended(t.start(command("object_list", GuardClass::Inspect), json::object(), "", "", {{"source", "agent"}, {"request", message}}),
                     ToolResult::ok({{"items", deep}}));

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 3);
    CHECK(json(e).dump().find("hunter22") == std::string::npos);
    CHECK(e[0]["withheld"] == true);
    CHECK(e[0]["error"] == "");
    CHECK(e[1]["request"]["message"] == kMask);
    CHECK(e[2]["request"] == kMask);
    CHECK(e[2]["response"] == kMask);
}

TEST_CASE("an environment variable that names a replay variable has that variable masked", "[Mcp][ActivityLog]")
{
    Log t;
#ifdef _WIN32
    _putenv_s("ORCA_ACTIVITY_TEST_ALIAS", "${vars.p}");
#else
    setenv("ORCA_ACTIVITY_TEST_ALIAS", "${vars.p}", 1);
#endif
    const json replay{{"vars", {{"p", "hunter22"}}},
                      {"steps", json::array({{{"tool", "keyboard_press"}, {"args", {{"text", "password=${env:ORCA_ACTIVITY_TEST_ALIAS}"}}}}})}};
    t.log.call_ended(t.start(command("replay_run"), {{"replay", replay}}, "", "j1", {{"source", "agent"}}), ToolResult::ok({{"passed", true}}));
    CHECK(json(t.entries()).dump().find("hunter22") == std::string::npos);
}

TEST_CASE("a dialog is not shown past the byte limit, and masks a value cut in its escaped form", "[Mcp][ActivityLog]")
{
    Log            t;
    const uint64_t quoted = t.start(command("keyboard_press", GuardClass::Recover), {{"text", "say\"cheese-and-more"}}, "", "j1",
                                    {{"source", "agent"}});
    t.log.dialog_answered(1, "", "", {{"title", "Rejected"}, {"message", "Sent {\"text\":\"say\\\"chee..."}, {"match", "Rejected"},
                                      {"button", "ok"}});
    t.log.call_ended(quoted, ToolResult::ok());
    const uint64_t big = t.start(command("keyboard_press", GuardClass::Recover), {{"text", std::string(ActivityLog::kMaxDialogBytes + 1, 'b')}},
                                 "", "j2", {{"source", "agent"}});
    t.log.dialog_answered(1, "", "", {{"title", "Saved"}, {"message", "Done"}, {"match", "Saved"}, {"button", "ok"}});
    t.log.call_ended(big, ToolResult::ok());

    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 4);
    CHECK(e[1]["request"]["message"].get<std::string>().find("chee") == std::string::npos);
    CHECK(e[3]["request"]["title"] == kMask);
    CHECK(e[3]["request"]["message"] == kMask);
}

TEST_CASE("a payload too costly to check against what it must hide is not shown", "[Mcp][ActivityLog]")
{
    Log  t;
    json steps = json::array();
    for (int i = 0; i < 20000; ++i)
        steps.push_back({{"tool", "keyboard_press"}, {"args", {{"text", "typed-" + std::to_string(i)}}}});
    t.log.call_ended(t.start(command("replay_run"), {{"replay", {{"steps", steps}}}}, "", "j1", {{"source", "agent"}}),
                     ToolResult::ok({{"passed", true}}));
    NativeCommand wait = command("app_wait", GuardClass::Inspect);
    t.log.call_ended(t.start(wait, {{"for", "job"}, {"job", "j1"}}, "", "", {{"source", "agent"}}),
                     ToolResult::ok({{"note", std::string(20000, 'n') + " typed-7"}}));
    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 2);
    CHECK(e[0]["args"]["truncated"] == true);
    CHECK(e[1]["withheld"] == false);
    CHECK(e[1]["response"] == kMask);
}

TEST_CASE("a replay whose variables put each other in over and over is read in bounded time, its script code as written", "[Mcp][ActivityLog]")
{
    Log t;
    // Puts itself in ten times: nine levels deep, a billion copies.
    std::string tenfold;
    for (int i = 0; i < 10; ++i)
        tenfold += "${vars.a}";
    const json replay{{"vars", {{"a", tenfold}, {"p", "hunter22"}}},
                      {"steps", json::array({{{"tool", "script_run"}, {"args", {{"code", "x = '${vars.a}'"}}}},
                                             {{"tool", "tool_call"}, {"args", {{"tool", "script_run"}, {"args", {{"code", "y = '${vars.a}'"}}}}}},
                                             {{"tool", "keyboard_press"}, {"args", {{"text", "${vars.a} pw=${vars.p}"}}}}})}};
    t.log.call_ended(t.start(command("replay_run"), {{"replay", replay}}, "", "j1", {{"source", "agent"}}), ToolResult::ok({{"passed", true}}));
    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 1);
    CHECK(e[0]["status"] == "done");
    CHECK(json(e).dump().find("hunter22") == std::string::npos);
}

TEST_CASE("a run whose result is too deep to read is failed when it failed, without the reason", "[Mcp][ActivityLog]")
{
    Log  t;
    json deep = "hunter22";
    for (int i = 0; i < ActivityLog::kMaxDepth + 1; ++i)
        deep = json::array({deep});
    t.log.call_ended(t.start(command("replay_run"), {{"path", "/tmp/r.json"}}, "", "j1", {{"source", "agent"}}),
                     ToolResult::ok({{"passed", false},
                                     {"failed_step", 1},
                                     {"steps", json::array({{{"index", 1}, {"ok", false}, {"error", "Rejected hunter22"}, {"details", deep}}})}}));
    t.log.call_ended(t.start(command("script_run"), {{"code", "raise ValueError(secret)"}}, "", "j2", {{"source", "agent"}}),
                     ToolResult::ok({{"status", "raised"}, {"message", "ValueError: hunter22"}, {"result", deep}}));
    t.log.call_ended(t.start(command("replay_run"), {{"path", "/tmp/r.json"}}, "", "j3", {{"source", "agent"}}),
                     ToolResult::ok({{"passed", true}, {"steps", deep}}));
    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 3);
    CHECK(json(e).dump().find("hunter22") == std::string::npos);
    CHECK(e[0]["status"] == "failed");
    CHECK(e[0]["failure"] == json{{"step", 1}, {"why", ""}});
    CHECK(e[0]["response"] == kMask);
    CHECK(e[1]["status"] == "failed");
    CHECK(e[1]["failure"] == json{{"script", "raised"}});
    CHECK(e[1]["error"] == "");
    CHECK(e[2]["status"] == "done");
}

TEST_CASE("a replay's null variable is put in as the replay puts it in, and a nested step with a numeric tool is read", "[Mcp][ActivityLog]")
{
    Log t;
    // The replay types "x=null"; "null" alone is not a value to hide.
    const json replay{{"vars", {{"n", nullptr}}}, {"steps", json::array({{{"tool", "keyboard_press"}, {"args", {{"text", "x=${vars.n}"}}}}})}};
    t.log.call_ended(t.start(command("replay_run"), {{"replay", replay}}, "", "j1", {{"source", "agent"}}),
                     ToolResult::ok({{"passed", true}, {"note", "typed x=null, nullable"}}));
    // A nested inline replay whose step's tool isn't a string, beside a step that types a secret.
    const json inner{{"steps", json::array({{{"tool", 17}, {"args", json::object()}}})}};
    const json outer{{"steps", json::array({{{"tool", "replay_run"}, {"args", {{"replay", inner}}}},
                                            {{"tool", "keyboard_press"}, {"args", {{"text", "hunter22"}}}}})}};
    t.log.call_ended(t.start(command("replay_run"), {{"replay", outer}}, "", "j2", {{"source", "agent"}}),
                     ToolResult::error("Rejected hunter22"));
    const std::vector<json> e = t.entries();
    REQUIRE(e.size() == 2);
    CHECK(e[0]["response"]["note"] == "typed " + kMask + ", nullable");
    CHECK(e[1]["status"] == "failed");
    CHECK(json(e).dump().find("hunter22") == std::string::npos);
}
