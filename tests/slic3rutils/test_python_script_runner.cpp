#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonScriptRunner.hpp>
#include <slic3r/plugin/host/PluginHostTools.hpp>
#include <slic3r/plugin/PluginOperations.hpp>

#include <pybind11/eval.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include "plugin_test_utils.hpp"
#include "test_utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
namespace py = pybind11;

namespace {

// The production interpreter, as the app brings it up, with a data folder of its own. No permission
// prompt may show: one would mean the identity was judged as a plugin's.
struct RunnerHarness
{
    ScopedDataDir            data{"python-script-runner"};
    bool                     initialized = PluginManager::instance().initialize();
    std::vector<std::string> prompts;

    RunnerHarness()
    {
        PluginAuditManager::set_permission_prompt_for_testing([this](const std::string& event, const std::vector<std::string>&) {
            prompts.push_back(event);
            return false;
        });
    }
    ~RunnerHarness()
    {
        PluginAuditManager::set_permission_prompt_for_testing(nullptr);
        PluginManager::instance().shutdown();
    }
};

PythonRunOutcome run(const std::string& code, json args = json::object(), std::chrono::milliseconds budget = std::chrono::minutes(1),
                     std::function<bool()> cancelled = {})
{
    PythonRunRequest request;
    request.code      = code;
    request.args      = std::move(args);
    request.identity  = kScriptIdentityAgent;
    request.budget    = budget;
    request.cancelled = std::move(cancelled);
    return run_python_code(request);
}

} // namespace

TEST_CASE("a script returns the result it sets, computed from its args", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const PythonRunOutcome out = run("result = {'sum': args['a'] + 1, 'names': ['x', 'y']}", json{{"a", 1}});
    CHECK(out.status == "finished");
    CHECK(out.result == json{{"sum", 2}, {"names", {"x", "y"}}});
    CHECK(out.output.empty());

    // A script that sets no result finishes with null.
    const PythonRunOutcome none = run("x = 1");
    CHECK(none.status == "finished");
    CHECK(none.result.is_null());
}

TEST_CASE("a script's printed output comes back with its result", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const PythonRunOutcome out = run("import sys\nprint('hi')\nsys.stderr.write('warn\\n')\nresult = 3");
    CHECK(out.status == "finished");
    CHECK(out.output == "hi\nwarn\n");
    CHECK(out.result == 3);
}

TEST_CASE("a script that raises reports the exception and its traceback", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const PythonRunOutcome out = run("print('before')\nraise ValueError('x')");
    CHECK(out.status == "raised");
    CHECK(out.message == "ValueError: x");
    CHECK_THAT(out.traceback, ContainsSubstring("<run_python>"));
    CHECK_THAT(out.traceback, ContainsSubstring("ValueError: x"));
    CHECK(out.output == "before\n");

    // sys.exit(0) ends a script as finished; another code is a failure.
    CHECK(run("import sys\nresult = 1\nsys.exit(0)").status == "finished");
    CHECK(run("import sys\nsys.exit(2)").status == "raised");
}

TEST_CASE("a script saved with a BOM runs as its file would", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    const PythonRunOutcome out = run("\xEF\xBB\xBF" "result = 'with bom'");
    CHECK(out.status == "finished");
    CHECK(out.result == "with bom");
}

TEST_CASE("a result that is not JSON makes the run raised", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const PythonRunOutcome out = run("result = {'when': object()}");
    CHECK(out.status == "raised");
    CHECK_THAT(out.message, ContainsSubstring("not JSON"));
    CHECK(run("result = {1: 'a'}").status == "raised");
    CHECK(run("result = float('nan')").status == "raised");
}

TEST_CASE("a script that never returns is cancelled, or stopped at its budget", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const auto        start = std::chrono::steady_clock::now();
    const PythonRunOutcome cancelled = run("while True:\n    pass", json::object(), std::chrono::minutes(1), [&] {
        return std::chrono::steady_clock::now() - start > std::chrono::milliseconds(200);
    });
    CHECK(cancelled.status == "cancelled");

    const PythonRunOutcome timed_out = run("while True:\n    pass", json::object(), std::chrono::milliseconds(300));
    CHECK(timed_out.status == "timed_out");

    // The thread runs Python normally afterwards: no interrupt is left pending on it.
    CHECK(run("result = 5").result == 5);
}

TEST_CASE("an agent's script may touch any file but the categorically denied ones, without prompts", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedTemporaryDir     outside("orca-python-script-outside");
    const std::string      file = (outside.path() / "note.txt").generic_string();
    const PythonRunOutcome wrote = run("open(args['file'], 'w').write('x')\nresult = open(args['file']).read()", json{{"file", file}});
    CHECK(wrote.status == "finished");
    CHECK(wrote.result == "x");

    const std::string      denied = (outside.path() / "OrcaSlicer.conf").generic_string();
    const PythonRunOutcome refused = run("open(args['file'], 'w')", json{{"file", denied}});
    CHECK(refused.status == "raised");
    CHECK_THAT(refused.message, ContainsSubstring("PermissionError"));

    CHECK(harness.prompts.empty());
    // The identity ends with the run.
    CHECK(PluginAuditManager::instance().current_plugin().empty());
}

namespace {

// Answers tool calls as the MCP core would for a run: get_state and run_script succeed, anything else is
// an error.
struct FakeBridge : ToolBridge
{
    std::vector<std::string> calls;
    json                     last_args;
    json call(const std::string& tool, const json& args) override
    {
        calls.push_back(tool);
        last_args = args;
        if (tool == "get_state")
            return json{{"ready", true}, {"args", args}};
        if (tool == "run_script")
            return json{{"passed", true}};
        throw ToolCallError("Unknown tool: " + tool, json{{"did_you_mean", {"get_state"}}});
    }
};

} // namespace

TEST_CASE("a script calls tools through its run's bridge, and an error result raises ToolError", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    FakeBridge             bridge;
    ScopedToolBridge       bound(bridge);
    const PythonRunOutcome out = run(R"PY(
import orca
state = orca.host.tools.call("get_state", {"verbose": True})
try:
    orca.host.tools.call("no_such_tool")
    error = None
except orca.host.tools.ToolError as e:
    error = {"message": str(e), "details": e.details}
result = {"state": state, "error": error}
)PY");
    REQUIRE(out.status == "finished");
    CHECK(out.result["state"] == json{{"ready", true}, {"args", {{"verbose", true}}}});
    CHECK(out.result["error"]["message"] == "Unknown tool: no_such_tool");
    CHECK(out.result["error"]["details"] == json{{"did_you_mean", {"get_state"}}});
    CHECK(bridge.calls == std::vector<std::string>{"get_state", "no_such_tool"});
}

TEST_CASE("a script hands run_script a script given as JSON text as it is, with its vars", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    FakeBridge             bridge;
    ScopedToolBridge       bound(bridge);
    const PythonRunOutcome out = run(R"PY(
import orca
text = '{"orca_script": 1, "steps": []}  // replayed'
result = orca.host.tools.run_script(text, {"part": "lid"})
)PY");
    REQUIRE(out.status == "finished");
    CHECK(out.result == json{{"passed", true}});
    CHECK(bridge.calls == std::vector<std::string>{"run_script"});
    CHECK(bridge.last_args == json{{"script", R"({"orca_script": 1, "steps": []}  // replayed)"}, {"vars", {{"part", "lid"}}}});
}

TEST_CASE("a thread a script starts reads but can't call tools", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    FakeBridge             bridge;
    ScopedToolBridge       bound(bridge);
    const PythonRunOutcome out = run(R"PY(
import orca, threading
seen = {}
def work():
    try:
        orca.host.tools.call("get_state")
        seen["called"] = True
    except RuntimeError as e:
        seen["error"] = str(e)
worker = threading.Thread(target=work)
worker.start()
worker.join()
result = seen
)PY");
    REQUIRE(out.status == "finished");
    CHECK_THAT(out.result.value("error", ""), ContainsSubstring("run's own worker"));
    CHECK(bridge.calls.empty());
}

TEST_CASE("a script is refused the orca.host functions only plugins may call", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const PythonRunOutcome out = run(R"PY(
import orca
refusals = []
for attempt in (lambda: orca.host.preset.process.begin_update(),
                lambda: orca.host.preset.process.update("layer_height", "0.2"),
                lambda: orca.host.ui.message("hello"),
                lambda: orca.host.plugin.storage()):
    try:
        attempt()
        refusals.append("allowed")
    except RuntimeError as e:
        refusals.append(str(e))
result = refusals
)PY");
    REQUIRE(out.status == "finished");
    REQUIRE(out.result.size() == 4);
    for (const json& refusal : out.result)
        CHECK_THAT(refusal.get<std::string>(), ContainsSubstring("is for plugins"));
}

TEST_CASE("orca.host.tools refuses code that is neither a plugin nor a script", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // Code with no audit identity: a plugin window's callback today.
    PythonRunRequest request;
    request.code     = "import orca\ntry:\n    orca.host.tools.call('get_state')\n    result = 'called'\nexcept RuntimeError as e:\n    result = str(e)";
    request.identity = "";
    const PythonRunOutcome out = run_python_code(request);
    REQUIRE(out.status == "finished");
    CHECK_THAT(out.result.get<std::string>(), ContainsSubstring("needs a plugin or a script"));
}

TEST_CASE("code a script leaves for its result, exception or namespace runs under its identity", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedTemporaryDir outside("orca-python-script-leftovers");
    const std::string  denied = (outside.path() / "OrcaSlicer.conf").generic_string();

    // An exception's own __str__ runs when the run reports it.
    const PythonRunOutcome raised = run(R"PY(
import orca
class Sneaky(Exception):
    def __str__(self):
        seen = []
        try:
            open(args["denied"], "w")
            seen.append("wrote")
        except PermissionError:
            seen.append("write refused")
        try:
            orca.host.preset.process.begin_update()
            seen.append("updated")
        except RuntimeError:
            seen.append("update refused")
        return ", ".join(seen)
raise Sneaky()
)PY", json{{"denied", denied}});
    CHECK(raised.status == "raised");
    CHECK_THAT(raised.message, ContainsSubstring("write refused, update refused"));

    // A finalizer of an object the namespace holds runs when the run clears it.
    const PythonRunOutcome held = run(R"PY(
class Held:
    def __init__(self, path):
        self.path = path
    # Bound now: by the time it runs, the cleared namespace has no names left.
    def __del__(self, open=open, refused=PermissionError):
        try:
            open(self.path, "w")
        except refused:
            pass
keep = Held(args["denied"])
result = 1
)PY", json{{"denied", denied}});
    CHECK(held.status == "finished");
    CHECK_FALSE(boost::filesystem::exists(denied));
}

TEST_CASE("a finalizer that runs past the budget while the run ends makes it timed out", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // Python ignores what __del__ raises, so the interrupt that stops this loop is swallowed.
    const PythonRunOutcome out = run(R"PY(
class Slow:
    def __del__(self):
        while True:
            pass
keep = Slow()
result = 1
)PY",
                                     json::object(), std::chrono::milliseconds(500));
    CHECK(out.status == "timed_out");
    CHECK(out.result.is_null());
}

TEST_CASE("a result whose own iteration never ends is stopped at the budget", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const PythonRunOutcome out = run("class Forever(list):\n    def __iter__(self):\n        while True:\n            pass\nresult = Forever([1])",
                                     json::object(), std::chrono::milliseconds(500));
    CHECK(out.status == "timed_out");
    CHECK(run("result = 2").result == 2);
}

TEST_CASE("a plugin calls tools only from a thread that may wait for the UI thread", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    std::vector<std::string> calls;
    set_plugin_tool_caller([&calls](const std::string& plugin, const std::string& tool, const json&) {
        calls.push_back(plugin + ":" + tool);
        return json{{"plugin", plugin}};
    });
    PythonRunRequest request;
    request.code     = "import orca\ntry:\n    result = orca.host.tools.call('get_state')\nexcept RuntimeError as e:\n    result = str(e)";
    request.identity = "measure";

    // A thread that is no Script worker (a slicing hook's, a callback's) is refused before anything waits.
    const PythonRunOutcome refused = run_python_code(request);
    REQUIRE(refused.status == "finished");
    CHECK_THAT(refused.result.get<std::string>(), ContainsSubstring("Script worker"));
    CHECK(calls.empty());

    {
        ScopedToolCallerThread worker;
        const PythonRunOutcome called = run_python_code(request);
        REQUIRE(called.status == "finished");
        CHECK(called.result == json{{"plugin", "measure"}});
    }
    CHECK(calls == std::vector<std::string>{"measure:get_state"});
    set_plugin_tool_caller({});
}

TEST_CASE("a plugin operation runs under its plugin's identity and grants", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedTemporaryDir outside("orca-plugin-operation-outside");
    PluginOperation    op;
    op.plugin_key = "measure";
    op.name       = "area";
    {
        PythonGILState gil;
        REQUIRE(gil);
        py::dict scope;
        py::exec(R"PY(
def handler(args):
    try:
        open(args["outside"], "w")
        return {"wrote": True}
    except PermissionError:
        return {"wrote": False, "sum": args["a"] + 1}
)PY", scope);
        op.handler = std::shared_ptr<py::object>(new py::object(scope["handler"]), [](py::object* held) {
            PythonGILState release;
            if (!release)
                (void) held->release();
            delete held;
        });
    }
    PluginOperations& ops = PluginOperations::instance();
    ops.add(op);
    const json out = ops.invoke(op, json{{"outside", (outside.path() / "note.txt").generic_string()}, {"a", 1}});
    ops.remove_plugin("measure");
    CHECK(out == json{{"wrote", false}, {"sum", 2}});
    // Judged as the plugin's code: its write needed a grant, which was asked for and refused.
    CHECK_FALSE(harness.prompts.empty());
}

TEST_CASE("an agent's code is compiled as the text it sent, whatever coding line it holds", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    // The UTF-8 for e acute, which a Latin-1 decode would turn into two characters.
    const PythonRunOutcome out = run("# -*- coding: latin-1 -*-\nresult = '\xC3\xA9'");
    REQUIRE(out.status == "finished");
    CHECK(out.result == "\xC3\xA9");
}

TEST_CASE("a .py file is decoded by its BOM or coding line and saved back the same", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // A Latin-1 file with its declaration: e acute is one byte, 0xE9.
    const std::string latin1 = "# -*- coding: latin-1 -*-\nname = '\xE9'\n";
    std::string       text, encoding, error;
    REQUIRE(decode_python_source(latin1, text, encoding, error));
    CHECK(text == "# -*- coding: latin-1 -*-\nname = '\xC3\xA9'\n");
    std::string saved;
    REQUIRE(encode_python_source(text, encoding, saved, error));
    CHECK(saved == latin1);

    // A BOM file reads without it and saves with it.
    REQUIRE(decode_python_source("\xEF\xBB\xBFx = 1\n", text, encoding, error));
    CHECK(text == "x = 1\n");
    REQUIRE(encode_python_source(text, encoding, saved, error));
    CHECK(saved == "\xEF\xBB\xBFx = 1\n");

    // Bytes that are not UTF-8, with no declaration, are not text; a character Latin-1 lacks can't be saved in it.
    CHECK_FALSE(decode_python_source("x = '\xFF\xFE'\n", text, encoding, error));
    CHECK_FALSE(encode_python_source("x = '\xE2\x82\xAC'\n", "iso-8859-1", saved, error));
}

TEST_CASE("without Python, a .py file is read only as strict UTF-8", "[PythonScript]")
{
    // The interpreter is never started here; a run of every case in one process may have started it.
    if (PythonInterpreter::instance().is_initialized())
        SKIP("The interpreter was started by an earlier test in this process");
    std::string text, encoding, error;
    REQUIRE(decode_python_source("name = '\xC3\xA9'\n", text, encoding, error));
    CHECK(text == "name = '\xC3\xA9'\n");
    CHECK(encoding == "utf-8");
    REQUIRE(decode_python_source("\xEF\xBB\xBFx = 1\n", text, encoding, error));
    CHECK(text == "x = 1\n");
    CHECK(encoding == "utf-8-sig");

    // What Python's codec refuses: an overlong form, a surrogate, a code point above U+10FFFF, a cut sequence.
    for (const std::string bytes : {"x = '\xC0\xAF'\n", "x = '\xED\xA0\x80'\n", "x = '\xF4\x90\x80\x80'\n", "x = '\xE2\x82"}) {
        CAPTURE(bytes);
        CHECK_FALSE(decode_python_source(bytes, text, encoding, error));
    }
    std::string saved;
    CHECK_FALSE(encode_python_source("x = 1\n", "iso-8859-1", saved, error));
}
