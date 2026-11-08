#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonRunScope.hpp>
#include <slic3r/plugin/PythonScriptRunner.hpp>
#include <slic3r/plugin/PythonSession.hpp>
#include <slic3r/plugin/host/PluginHostTools.hpp>
#include <slic3r/plugin/host/PluginHostUi.hpp>
#include <slic3r/plugin/PluginOperations.hpp>

#include <pybind11/eval.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include "plugin_test_utils.hpp"
#include "test_utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <memory>
#include <algorithm>
#include <atomic>
#include <functional>
#include <initializer_list>
#include <ios>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
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

// UTF-8 text to a native path and back: wide on Windows, where boost's narrow conversion uses the ANSI code page.
boost::filesystem::path utf8_path(const std::string& utf8)
{
#ifdef _WIN32
    return boost::filesystem::path(boost::nowide::widen(utf8));
#else
    return boost::filesystem::path(utf8);
#endif
}

std::string path_utf8(const boost::filesystem::path& path)
{
#ifdef _WIN32
    return boost::nowide::narrow(path.wstring());
#else
    return path.string();
#endif
}

// A folder of script files whose name holds a non-ASCII character, removed at the end.
struct MacroDir
{
    boost::filesystem::path dir;

    MacroDir() : dir(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca-macro-%%%%-%%%%"))
    {
        dir += utf8_path("-\xC3\xBC");
        boost::filesystem::create_directories(dir);
    }
    ~MacroDir()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(dir, ec);
    }
    MacroDir(const MacroDir&)            = delete;
    MacroDir& operator=(const MacroDir&) = delete;

    // Writes `text` as `name` ('/'-separated) and returns the file's path as UTF-8.
    std::string write(const std::string& name, const std::string& text) const
    {
        const boost::filesystem::path path = dir / utf8_path(name);
        boost::filesystem::create_directories(path.parent_path());
        boost::nowide::ofstream out(path_utf8(path).c_str(), std::ios::binary);
        out << text;
        return path_utf8(path);
    }
    std::string folder() const { return path_utf8(dir); }
};

// Runs `code` as the person's script read from the file `path`, which holds the same text.
PythonRunOutcome run_file(const std::string& path, const std::string& code, std::chrono::milliseconds budget = std::chrono::minutes(1),
                          std::function<bool()> cancelled = {})
{
    PythonRunRequest request;
    request.code      = code;
    request.identity  = kScriptIdentityPerson;
    request.filename  = path;
    request.budget    = budget;
    request.cancelled = std::move(cancelled);
    return run_python_code(request);
}

// Evaluates `expr` outside any run and returns its value as JSON.
json py_json(const std::string& expr)
{
    PythonGILState gil;
    py::dict       scope;
    py::exec("import json, sys", scope);
    return json::parse(py::eval("json.dumps(" + expr + ")", scope).cast<std::string>());
}

// A module the test shares with its scripts, holding threading.Events and the values they leave; removed at the end.
struct SharedModule
{
    std::string name;
    SharedModule(std::string module_name, std::initializer_list<const char*> events);
    ~SharedModule();
    SharedModule(const SharedModule&)            = delete;
    SharedModule& operator=(const SharedModule&) = delete;
    void set(const char* event) const;
    bool wait(const char* event, double seconds) const;
    // A value a script left at `path` below the module ("pool._shutdown"), as JSON; null when there is none.
    json get(const std::string& path) const;
};

// Runs `code` outside any run, for the state a test sets up or clears.
void py_run(const std::string& code)
{
    PythonGILState gil;
    py::dict       scope;
    py::exec(code, scope);
}

SharedModule::SharedModule(std::string module_name, std::initializer_list<const char*> events) : name(std::move(module_name))
{
    std::string code = "import sys, types, threading\nm = types.ModuleType('" + name + "')\n";
    for (const char* event : events)
        code += std::string("m.") + event + " = threading.Event()\n";
    py_run(code + "sys.modules['" + name + "'] = m\n");
}

SharedModule::~SharedModule() { py_run("import sys\nsys.modules.pop('" + name + "', None)\n"); }

void SharedModule::set(const char* event) const { py_run("import " + name + "\n" + name + "." + event + ".set()\n"); }

bool SharedModule::wait(const char* event, double seconds) const
{
    return py_json("__import__('" + name + "')." + event + ".wait(" + std::to_string(seconds) + ")").get<bool>();
}

json SharedModule::get(const std::string& path) const
{
    return py_json("(lambda m: eval('m.' + '" + path + "', {'m': m}) if hasattr(m, '" + path.substr(0, path.find('.')) +
                   "') else None)(__import__('" + name + "'))");
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

TEST_CASE("code run from a file sees the file's path as __file__, and code under a <name> sees none", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    MacroDir          macro;
    const std::string code = "result = {'file': globals().get('__file__'), 'name': __name__}";
    const std::string path = macro.write("report.py", code);
    const PythonRunOutcome from_file = run_file(path, code);
    REQUIRE(from_file.status == "finished");
    CHECK(from_file.result == json{{"file", path}, {"name", "__main__"}});

    const PythonRunOutcome from_name = run_file("<run_python>", code);
    REQUIRE(from_name.status == "finished");
    CHECK(from_name.result["file"].is_null());
}

TEST_CASE("a script file runs as the main program, with its folder importable", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    MacroDir macro;
    macro.write("helper.py", "VALUE = 1\n");
    const std::string code = "import sys, helper\n"
                             "result = {'name': __name__, 'argv': list(sys.argv), 'file': __file__, 'value': helper.VALUE,\n"
                             "          'main': sys.modules['__main__'].__dict__ is globals()}\n";
    const std::string path = macro.write("main.py", code);
    const PythonRunOutcome out = run_file(path, code);
    REQUIRE(out.status == "finished");
    CHECK(out.result["name"] == "__main__");
    CHECK(out.result["argv"] == json::array({path}));
    CHECK(out.result["file"] == path);
    CHECK(out.result["value"] == 1);
    CHECK(out.result["main"] == true);
}

TEST_CASE("a file run's sys.argv is a list of its own", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const json        app_argv = py_json("list(sys.argv)");
    MacroDir          macro;
    const std::string code = "import sys\n"
                             "sys.argv[0] += '!'\n"
                             "sys.argv.append('x')\n"
                             "ne = sys.argv != [__file__]\n"
                             "sys.argv = sys.argv + ['y']\n"
                             "result = [list(sys.argv), ne]\n";
    const std::string path = macro.write("argv.py", code);
    const PythonRunOutcome out = run_file(path, code);
    REQUIRE(out.status == "finished");
    CHECK(out.result == json::array({json::array({path + "!", "x", "y"}), true}));
    CHECK(py_json("list(sys.argv)") == app_argv);
}

TEST_CASE("a run leaves sys.path, sys.argv and the app's __main__ as it found them", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::string state = "[list(sys.path), list(sys.argv), id(sys.modules['__main__'].__dict__)]";
    const json        before = py_json(state);
    MacroDir          macro;
    macro.write("helper.py", "VALUE = 1\n");
    const std::string code = "import helper\nresult = helper.VALUE\n";
    REQUIRE(run_file(macro.write("main.py", code), code).status == "finished");
    CHECK(py_json(state) == before);
}

TEST_CASE("a helper module edited between runs is imported afresh", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    MacroDir          macro;
    const std::string helper = macro.write("helper.py", "VALUE = 1\n");
    const std::time_t written = boost::filesystem::last_write_time(utf8_path(helper));
    const std::string code    = "import helper\nresult = helper.VALUE\n";
    const std::string path    = macro.write("main.py", code);
    REQUIRE(run_file(path, code).result == 1);

    // Same size and the first write's time, so only reading the source again shows the edit.
    macro.write("helper.py", "VALUE = 2\n");
    boost::filesystem::last_write_time(utf8_path(helper), written);
    CHECK(run_file(path, code).result == 2);
    CHECK(py_json("'helper' in sys.modules") == false);
}

TEST_CASE("a namespace package beside the script imports as Python would", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    MacroDir macro;
    macro.write("nspkg/mod.py", "X = 3\n");
    const std::string code = "import nspkg.mod\nresult = nspkg.mod.X\n";
    const PythonRunOutcome out = run_file(macro.write("main.py", code), code);
    REQUIRE(out.status == "finished");
    CHECK(out.result == 3);
    CHECK(py_json("'nspkg.mod' in sys.modules") == false);
}

TEST_CASE("a module the script reaches through another path entry stays imported", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    MacroDir macro;
    macro.write("lib/libmod_kept.py", "Y = 4\n");
    const std::string code = "import os, sys\n"
                             "sys.path.append(os.path.join(os.path.dirname(__file__), 'lib'))\n"
                             "import libmod_kept\nresult = libmod_kept.Y\n";
    REQUIRE(run_file(macro.write("main.py", code), code).result == 4);
    CHECK(py_json("'libmod_kept' in sys.modules") == true);
    py_run("import sys\nsys.modules.pop('libmod_kept', None)\n"
           "sys.path[:] = [p for p in sys.path if not p.endswith('lib') or 'orca-macro-' not in p]\n");
}

TEST_CASE("a package the script reaches through another path entry keeps its submodules", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // The package lies below the script's folder, as one installed anywhere under a broad folder does.
    MacroDir macro;
    macro.write("lib/libpkg_kept/__init__.py", "");
    macro.write("lib/libpkg_kept/part.py", "Z = 5\n");
    const std::string code = "import os, sys\n"
                             "sys.path.append(os.path.join(os.path.dirname(__file__), 'lib'))\n"
                             "import libpkg_kept.part\nresult = libpkg_kept.part.Z\n";
    REQUIRE(run_file(macro.write("main.py", code), code).result == 5);
    CHECK(py_json("'libpkg_kept.part' in sys.modules") == true);
    py_run("import sys\nfor name in ('libpkg_kept.part', 'libpkg_kept'):\n    sys.modules.pop(name, None)\n"
           "sys.path[:] = [p for p in sys.path if 'orca-macro-' not in p]\n");
}

TEST_CASE("a script's folder shadows a standard module not imported yet, but not a built-in one", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    REQUIRE(py_json("'colorsys' in sys.modules") == false);
    const json builtin = py_json("next((n for n in sorted(sys.builtin_module_names) if n not in sys.modules and n.isidentifier() "
                                 "and not n.startswith('_')), None)");
    if (builtin.is_null())
        SKIP("Every built-in module is imported already");
    MacroDir macro;
    macro.write("colorsys.py", "SHADOW = True\n");
    macro.write(builtin.get<std::string>() + ".py", "SHADOW = True\n");
    const std::string code = "import importlib\nresult = [getattr(importlib.import_module(name), 'SHADOW', False) "
                             "for name in ('colorsys', '" + builtin.get<std::string>() + "')]\n";
    const PythonRunOutcome out = run_file(macro.write("main.py", code), code);
    REQUIRE(out.status == "finished");
    CHECK(out.result == json::array({true, false}));
    CHECK(py_json("'colorsys' in sys.modules") == false);
}

TEST_CASE("a helper a plugin also loaded stays imported after the run", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    py_run("import sys, types, threading\nm = types.ModuleType('_overlap')\nm.started = threading.Event()\nm.go = threading.Event()\n"
           "sys.modules['_overlap'] = m\n");
    MacroDir macro;
    macro.write("helper_shared.py", "VALUE = 5\n");
    const std::string plugmod = macro.write("plugmod_shared.py", "import helper_shared\n");
    const std::string code    = "import _overlap, helper_shared\n_overlap.started.set()\n_overlap.go.wait(10)\nresult = helper_shared.VALUE\n";
    const std::string path    = macro.write("main.py", code);
    PythonRunOutcome  out;
    std::thread       a([&] { out = run_file(path, code); });
    py_run("import _overlap\n_overlap.started.wait(10)\n");

    std::string              error;
    std::vector<std::string> paths, modules;
    PyObject*                module = PythonInterpreter::instance().load_module_from_file(plugmod, error, &paths, &modules);
    py_run("import _overlap\n_overlap.go.set()\n");
    a.join();
    CHECK(out.status == "finished");
    CHECK(module != nullptr);
    CHECK(py_json("'helper_shared' in sys.modules") == true);
    PythonInterpreter::instance().unload_module(module, "plugmod_shared", paths, modules);
    py_run("import sys\nsys.modules.pop('_overlap', None)\n");
}

TEST_CASE("a run that replaces the routers or __main__ leaves the next run its own", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const json        app_main = py_json("id(sys.modules['__main__'])");
    MacroDir          macro;
    const std::string first  = "import sys, types\nsys.__class__ = types.ModuleType\nsys.modules['__main__'] = types.ModuleType('x')\n";
    REQUIRE(run_file(macro.write("first.py", first), first).status == "finished");
    CHECK(py_json("id(sys.modules['__main__'])") == app_main);

    const std::string second = "import sys\nresult = [list(sys.argv), sys.modules['__main__'].__dict__ is globals()]\n";
    const std::string path   = macro.write("second.py", second);
    const PythonRunOutcome out = run_file(path, second);
    REQUIRE(out.status == "finished");
    CHECK(out.result == json::array({json::array({path}), true}));
}

TEST_CASE("overlapping runs each see their own __main__ and argv while both run", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    py_run("import sys, types, threading\nm = types.ModuleType('_overlap')\nm.b_started = threading.Event()\n"
           "m.a_read = threading.Event()\nsys.modules['_overlap'] = m\n");
    MacroDir          macro;
    const std::string a_code = "import sys, _overlap\n_overlap.b_started.wait(10)\nsys.argv.append('a')\n"
                               "result = [list(sys.argv), sys.modules['__main__'].__dict__ is globals()]\n_overlap.a_read.set()\n";
    const std::string b_code = "import sys, _overlap\n_overlap.b_started.set()\n_overlap.a_read.wait(10)\n"
                               "result = [list(sys.argv), sys.modules['__main__'].__dict__ is globals()]\n";
    const std::string a_path = macro.write("a.py", a_code);
    const std::string b_path = macro.write("b.py", b_code);
    PythonRunOutcome  a_out;
    std::thread       a([&] { a_out = run_file(a_path, a_code); });
    const PythonRunOutcome b_out = run_file(b_path, b_code);
    a.join();
    CHECK(a_out.result == json::array({json::array({a_path, "a"}), true}));
    CHECK(b_out.result == json::array({json::array({b_path}), true}));
    py_run("import sys\nsys.modules.pop('_overlap', None)\n");
}

TEST_CASE("runs from one folder evict its helpers only when the last of them ends", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const bool a_ends_first = GENERATE(true, false);
    py_run("import sys, types, threading\nm = types.ModuleType('_overlap')\n"
           "m.a_started, m.b_started, m.go_a, m.go_b = (threading.Event() for _ in range(4))\nsys.modules['_overlap'] = m\n");
    MacroDir          macro;
    macro.write("helper_two.py", "VALUE = 6\n");
    const std::string a_code = "import _overlap, helper_two\n_overlap.a_started.set()\n_overlap.go_a.wait(10)\n";
    const std::string b_code = "import _overlap\n_overlap.b_started.set()\n_overlap.go_b.wait(10)\n";
    const std::string a_path = macro.write("a.py", a_code);
    const std::string b_path = macro.write("b.py", b_code);
    std::thread       a([&] { run_file(a_path, a_code); });
    std::thread       b([&] { run_file(b_path, b_code); });
    py_run("import _overlap\n_overlap.a_started.wait(10)\n_overlap.b_started.wait(10)\n");
    CHECK(py_json("'helper_two' in sys.modules") == true);

    py_run(std::string("import _overlap\n_overlap.") + (a_ends_first ? "go_a" : "go_b") + ".set()\n");
    (a_ends_first ? a : b).join();
    CHECK(py_json("'helper_two' in sys.modules") == true);
    py_run(std::string("import _overlap\n_overlap.") + (a_ends_first ? "go_b" : "go_a") + ".set()\n");
    (a_ends_first ? b : a).join();
    CHECK(py_json("'helper_two' in sys.modules") == false);
    py_run("import sys\nsys.modules.pop('_overlap', None)\n");
}

TEST_CASE("a run entering a folder while its last run's teardown runs keeps the helpers it imports", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // A's teardown waits in the helper's atexit function, without the GIL, while B enters the folder and imports.
    SharedModule shared("_td", {"in_exit", "go_exit", "b_imported", "go_b"});
    MacroDir     macro;
    macro.write("helper_td.py", "import atexit, _td\nfrom concurrent.futures import ThreadPoolExecutor\nVALUE = _td.version\n"
                                "POOL = ThreadPoolExecutor(1)\ndef _bye():\n    _td.in_exit.set()\n    _td.go_exit.wait(10)\n"
                                "atexit.register(_bye)\n");
    const std::string a_code = "import _td\n_td.version = 1\nimport helper_td\n";
    const std::string b_code = "import _td\n_td.version = 2\nimport helper_td\n_td.b_value = helper_td.VALUE\n_td.b_imported.set()\n"
                               "_td.go_b.wait(10)\nresult = helper_td.POOL.submit(int, '9').result(5)\n";
    const std::string a_path = macro.write("a.py", a_code);
    const std::string b_path = macro.write("b.py", b_code);
    PythonRunOutcome  b_out;
    std::thread       a([&] { run_file(a_path, a_code); });
    REQUIRE(shared.wait("in_exit", 10));
    std::thread b([&] { b_out = run_file(b_path, b_code); });
    REQUIRE(shared.wait("b_imported", 10));
    shared.set("go_exit");
    a.join();
    CHECK(shared.get("b_value") == 2);
    CHECK(py_json("getattr(sys.modules.get('helper_td'), 'VALUE', None)") == 2);
    shared.set("go_b");
    b.join();
    CHECK(b_out.status == "finished");
    CHECK(b_out.result == 9);
    CHECK(py_json("'helper_td' in sys.modules") == false);
}

TEST_CASE("a helper's finalizers run when its folder's last run ends, inside that run", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // The helper's functions hold its globals, which hold KEEP: only clearing them frees it before a later collection.
    MacroDir macro;
    macro.write("helper_cycle.py", "class Bye:\n    def __del__(self):\n        print('helper finalized')\nKEEP = Bye()\n");
    const std::string      code = "import helper_cycle\n";
    const PythonRunOutcome out  = run_file(macro.write("main.py", code), code);
    CHECK(out.status == "finished");
    CHECK_THAT(out.output, ContainsSubstring("helper finalized"));
}

TEST_CASE("a filename that is not an existing file runs as code sent as text", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const json             app_argv = py_json("list(sys.argv)");
    const std::string      code     = "import sys\nresult = {'name': __name__, 'file': globals().get('__file__'), 'argv': list(sys.argv)}\n";
    const PythonRunOutcome out      = run_file("report", code);
    REQUIRE(out.status == "finished");
    CHECK(out.result["name"] == "__main__");
    CHECK(out.result["file"].is_null());
    CHECK(out.result["argv"] == app_argv);
}

TEST_CASE("a thread a script starts prints into the run's output", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const PythonRunOutcome out = run("import threading\nt = threading.Thread(target=lambda: print('from thread'))\nt.start()\nt.join()\n");
    REQUIRE(out.status == "finished");
    CHECK_THAT(out.output, ContainsSubstring("from thread"));
}

TEST_CASE("a non-JSON result in a file run comes back as no result, with a note", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    MacroDir          macro;
    const std::string code = "result = object()\n";
    const PythonRunOutcome out = run_file(macro.write("main.py", code), code);
    CHECK(out.status == "finished");
    CHECK(out.result.is_null());
    CHECK_THAT(out.output, ContainsSubstring("not JSON"));
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
    // Answers a call in place of the canned results when it returns a value; `after` sees each call once answered.
    std::function<std::optional<json>(const std::string&, const json&)> hook;
    std::function<void(const std::string&)>                             after;
    std::mutex                                                          mutex;
    json call(const std::string& tool, const json& args) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            calls.push_back(tool);
            last_args = args;
        }
        struct After
        {
            FakeBridge&        bridge;
            const std::string& tool;
            ~After()
            {
                if (bridge.after)
                    bridge.after(tool);
            }
        } after_call{*this, tool};
        if (hook)
            if (std::optional<json> answer = hook(tool, args))
                return *answer;
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

TEST_CASE("a run waits for the non-daemon threads it starts, which call tools through it", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    FakeBridge             bridge;
    ScopedToolBridge       bound(bridge);
    const PythonRunOutcome out = run("import orca, threading, time\n"
                                     "def work():\n    time.sleep(0.2)\n    orca.host.tools.call('get_state')\n"
                                     "threading.Thread(target=work).start()\n");
    CHECK(out.status == "finished");
    CHECK(bridge.calls == std::vector<std::string>{"get_state"});
}

TEST_CASE("a run on a thread Python didn't start waits for the threads it starts", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // The app runs a script on an MCP job thread, which Python sees as a daemon dummy thread.
    FakeBridge       bridge;
    PythonRunOutcome out;
    std::thread      job([&] {
        ScopedToolBridge bound(bridge);
        out = run("import orca, threading, time\n"
                  "def work(orca=orca, time=time):\n    time.sleep(0.2)\n    orca.host.tools.call('get_state')\n"
                  "threading.Thread(target=work).start()\n");
    });
    job.join();
    CHECK(out.status == "finished");
    CHECK(bridge.calls == std::vector<std::string>{"get_state"});
}

TEST_CASE("a thread started just before the code returns is waited for", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    FakeBridge       bridge;
    ScopedToolBridge bound(bridge);
    CHECK(run("import orca, threading\nthreading.Thread(target=lambda: orca.host.tools.call('get_state')).start()\n").status == "finished");
    CHECK(bridge.calls == std::vector<std::string>{"get_state"});
}

TEST_CASE("a thread started by another of the run's threads after the code returned is waited for", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    FakeBridge       bridge;
    ScopedToolBridge bound(bridge);
    CHECK(run("import orca, threading, time\n"
              "def second():\n    time.sleep(0.3)\n    orca.host.tools.call('get_state')\n"
              "def first():\n    time.sleep(0.3)\n    threading.Thread(target=second).start()\n"
              "threading.Thread(target=first).start()\n")
              .status == "finished");
    CHECK(bridge.calls == std::vector<std::string>{"get_state"});
}

TEST_CASE("a callback already running when the code returns is waited for, and the thread it starts too", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // The script's code hands its run to the test through the bridge, the way a window callback captures it.
    SharedModule                    shared("_cb", {"entered"});
    std::shared_ptr<PythonRunScope> run_scope;
    FakeBridge                      bridge;
    bridge.hook = [&](const std::string& tool, const json&) -> std::optional<json> {
        if (tool != "capture_run")
            return std::nullopt;
        run_scope = current_python_run().run;
        return json::object();
    };
    ScopedToolBridge bound(bridge);
    std::thread      callback;
    bridge.after = [&](const std::string& tool) {
        if (tool != "capture_run")
            return;
        callback = std::thread([&] {
            ScopedPythonRun in_run(run_scope, PythonRunThread::Callback);
            if (!run_scope->callback_entering())
                return;
            py_run("import orca, threading, time, _cb\n_cb.entered.set()\ntime.sleep(0.3)\n"
                   "threading.Thread(target=lambda: orca.host.tools.call('get_state')).start()\n");
            run_scope->callback_left();
        });
    };
    const PythonRunOutcome out = run("import orca, _cb\norca.host.tools.call('capture_run')\n_cb.entered.wait(10)\n");
    callback.join();
    CHECK(out.status == "finished");
    CHECK(bridge.calls == std::vector<std::string>{"capture_run", "get_state"});
}

TEST_CASE("a raw thread can't start a thread after its run has ended", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule     shared("_late", {"done"});
    FakeBridge       bridge;
    ScopedToolBridge bound(bridge);
    REQUIRE(run("import _thread, threading, time, _late\n"
                "def raw(time=time, threading=threading, late=_late):\n    time.sleep(0.3)\n    try:\n"
                "        threading.Thread(target=print).start()\n    except RuntimeError as e:\n        late.error = str(e)\n"
                "    late.done.set()\n"
                "_thread.start_new_thread(raw, ())\n")
                .status == "finished");
    REQUIRE(shared.wait("done", 5));
    CHECK_THAT(shared.get("error").get<std::string>(), ContainsSubstring("run has ended"));
}

TEST_CASE("a script that exits with sys.exit(0) still waits for its threads", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    FakeBridge       bridge;
    ScopedToolBridge bound(bridge);
    CHECK(run("import orca, sys, threading, time\n"
              "def work():\n    time.sleep(0.2)\n    orca.host.tools.call('get_state')\n"
              "threading.Thread(target=work).start()\nsys.exit(0)\n")
              .status == "finished");
    CHECK(bridge.calls == std::vector<std::string>{"get_state"});
}

TEST_CASE("a daemon thread still running after its run is refused tool calls", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule     shared("_late", {"go", "done"});
    FakeBridge       bridge;
    ScopedToolBridge bound(bridge);
    REQUIRE(run("import orca, threading, _late\n"
                "def work(orca=orca, late=_late):\n    late.go.wait(10)\n    try:\n        orca.host.tools.call('get_state')\n"
                "    except RuntimeError as e:\n        late.error = str(e)\n    late.done.set()\n"
                "threading.Thread(target=work, daemon=True).start()\n")
                .status == "finished");
    shared.set("go");
    REQUIRE(shared.wait("done", 5));
    CHECK_THAT(shared.get("error").get<std::string>(), ContainsSubstring("run has ended"));
    CHECK(bridge.calls.empty());
}

TEST_CASE("a tool call in flight when the run ends finishes before the run does", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule shared("_late", {"in_call"});
    FakeBridge   bridge;
    bridge.hook = [&](const std::string&, const json&) -> std::optional<json> {
        shared.set("in_call");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return json{{"ready", true}};
    };
    ScopedToolBridge bound(bridge);
    const auto       start = std::chrono::steady_clock::now();
    REQUIRE(run("import orca, threading, _late\n"
                "def work(orca=orca, late=_late):\n    late.result = orca.host.tools.call('get_state')\n"
                "threading.Thread(target=work, daemon=True).start()\n_late.in_call.wait(10)\n")
                .status == "finished");
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::seconds(1));
    CHECK(shared.get("result") == json{{"ready", true}});
}

TEST_CASE("a stop during the in-flight wait still ends the run as cancelled", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const bool        by_budget = GENERATE(false, true);
    SharedModule      shared("_late", {"in_call"});
    std::atomic<bool> stop{false};
    FakeBridge        bridge;
    bridge.hook = [&](const std::string&, const json&) -> std::optional<json> {
        shared.set("in_call");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop = true;
        std::this_thread::sleep_for(std::chrono::seconds(2)); // ignores the cancel
        return json::object();
    };
    ScopedToolBridge bound(bridge);
    int              stops = 0;
    PythonRunRequest request;
    request.code       = "import orca, threading, _late\n"
                         "threading.Thread(target=lambda: orca.host.tools.call('get_state'), daemon=True).start()\n"
                         "_late.in_call.wait(10)\n";
    request.identity   = kScriptIdentityAgent;
    request.budget     = by_budget ? std::chrono::milliseconds(1000) : std::chrono::minutes(1);
    request.cancelled  = [&] { return !by_budget && stop.load(); };
    request.stop_calls = [&] { ++stops; };
    const PythonRunOutcome out = run_python_code(request);
    CHECK(out.status == (by_budget ? "timed_out" : "cancelled"));
    CHECK(stops == 1);
}

TEST_CASE("a task the run submits is waited for and runs inside the run", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::string where = GENERATE("module", "list", "function", "helper", "library");
    SharedModule      shared("_kept", {});
    MacroDir          macro;
    const std::string pool_source = "from concurrent.futures import ThreadPoolExecutor\nPOOL = ThreadPoolExecutor(1)\n";
    macro.write("helper_pool.py", pool_source);
    macro.write("lib/libpool_task.py", pool_source);
    std::string make;
    if (where == "module")
        make = "from concurrent.futures import ThreadPoolExecutor\npool = ThreadPoolExecutor(1)\n";
    else if (where == "list")
        make = "from concurrent.futures import ThreadPoolExecutor\npools = [ThreadPoolExecutor(1)]\npool = pools[0]\n";
    else if (where == "function")
        make = "from concurrent.futures import ThreadPoolExecutor\ndef make():\n    _kept.pool = ThreadPoolExecutor(1)\n"
               "    return _kept.pool\npool = make()\n";
    else if (where == "helper")
        make = "import helper_pool\npool = helper_pool.POOL\n";
    else
        make = "import os, sys\nsys.path.append(os.path.join(os.path.dirname(__file__), 'lib'))\nimport libpool_task\n"
               "pool = libpool_task.POOL\n";
    const std::string code = "import orca, time, _kept\n" + make +
                             "_kept.pool = pool\n"
                             "def task():\n    time.sleep(0.3)\n    orca.host.tools.call('get_state')\n"
                             "pool.submit(task)\n";
    FakeBridge             bridge;
    ScopedToolBridge       bound(bridge);
    const PythonRunOutcome out = run_file(macro.write("main.py", code), code, std::chrono::seconds(20));
    CHECK(out.status == "finished");
    CHECK(bridge.calls == std::vector<std::string>{"get_state"});
    CHECK(shared.get("pool._shutdown") == (where != "library"));
    if (where == "library") {
        CHECK(py_json("__import__('_kept').pool.submit(int, '3').result()") == 3);
        py_run("import sys, _kept\n_kept.pool.shutdown()\nsys.modules.pop('libpool_task', None)\n"
               "sys.path[:] = [p for p in sys.path if 'orca-macro-' not in p]\n");
    }
}

TEST_CASE("a pool worker another run started runs this run's task inside this run", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule shared("_kept", {});
    py_run("import _kept\nfrom concurrent.futures import ThreadPoolExecutor\n_kept.pool = ThreadPoolExecutor(1)\n");
    FakeBridge bridge_a, bridge_b;
    {
        ScopedToolBridge bound(bridge_a);
        REQUIRE(run("import _kept\n_kept.pool.submit(int).result()\n").status == "finished");
    }
    {
        ScopedToolBridge bound(bridge_b);
        REQUIRE(run("import orca, _kept\n_kept.pool.submit(lambda: orca.host.tools.call('get_state')).result()\n").status ==
                "finished");
    }
    CHECK(bridge_a.calls.empty());
    CHECK(bridge_b.calls == std::vector<std::string>{"get_state"});
    py_run("import _kept\n_kept.pool.shutdown()\n");
}

TEST_CASE("a submit to a shut-down pool does not hang the run", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const bool caught = GENERATE(true, false);
    const auto start  = std::chrono::steady_clock::now();
    const PythonRunOutcome out = run(std::string("from concurrent.futures import ThreadPoolExecutor\npool = ThreadPoolExecutor(1)\n"
                                                 "pool.shutdown()\n") +
                                     (caught ? "try:\n    pool.submit(int)\nexcept RuntimeError:\n    pass\n" : "pool.submit(int)\n"));
    CHECK(out.status == (caught ? "finished" : "raised"));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

TEST_CASE("Stop interrupts a task the run submitted", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule shared("_stop", {"returned", "ended"});
    py_run("import _stop\nfrom concurrent.futures import ThreadPoolExecutor\n_stop.pool = ThreadPoolExecutor(1)\n");
    std::atomic<bool> stop{false};
    std::thread       stopper([&] {
        shared.wait("returned", 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop = true;
    });
    const PythonRunOutcome out = run("import _stop\n"
                                     "def task():\n    try:\n        while True:\n            pass\n    finally:\n        _stop.ended.set()\n"
                                     "_stop.pool.submit(task)\n_stop.returned.set()\n",
                                     json::object(), std::chrono::minutes(1), [&] { return stop.load(); });
    stopper.join();
    CHECK(out.status == "cancelled");
    CHECK(shared.wait("ended", 5));
    CHECK(py_json("__import__('_stop').pool.submit(int, '5').result()") == 5);
    py_run("import _stop\n_stop.pool.shutdown()\n");
}

TEST_CASE("a task queued when the run is stopped never runs", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule      shared("_stop", {"ran"});
    std::atomic<bool> stop{false};
    std::thread       stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop = true;
    });
    const PythonRunOutcome out = run("import time, _stop\nfrom concurrent.futures import ThreadPoolExecutor\n"
                                     "pool = ThreadPoolExecutor(1)\n"
                                     "def spin():\n    while True:\n        time.sleep(0.01)\n"
                                     "pool.submit(spin)\n_stop.second = pool.submit(_stop.ran.set)\n",
                                     json::object(), std::chrono::minutes(1), [&] { return stop.load(); });
    stopper.join();
    CHECK(out.status == "cancelled");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK(py_json("__import__('_stop').ran.is_set()") == false);
    CHECK_THAT(py_json("str(__import__('_stop').second.exception(5))").get<std::string>(), ContainsSubstring("run has ended"));
}

TEST_CASE("a task's done callbacks run inside the run that submitted it, and it waits for them", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const bool   warmed = GENERATE(false, true);
    SharedModule shared("_cb", {"release"});
    py_run("import _cb\nfrom concurrent.futures import ThreadPoolExecutor\n_cb.pool = ThreadPoolExecutor(1)\n");
    if (warmed) {
        FakeBridge       other;
        ScopedToolBridge bound(other);
        REQUIRE(run("import _cb\n_cb.pool.submit(int).result()\n").status == "finished");
    }
    FakeBridge       bridge;
    ScopedToolBridge bound(bridge);
    std::thread      releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        shared.set("release");
    });
    const PythonRunOutcome out = run("import orca, time, _cb\n"
                                     "def cb(future):\n    time.sleep(0.3)\n    print('callback ran')\n    orca.host.tools.call('get_state')\n"
                                     "_cb.pool.submit(_cb.release.wait, 10).add_done_callback(cb)\n");
    releaser.join();
    CHECK(out.status == "finished");
    CHECK(bridge.calls == std::vector<std::string>{"get_state"});
    CHECK_THAT(out.output, ContainsSubstring("callback ran"));
    py_run("import _cb\n_cb.pool.shutdown()\n");
}

TEST_CASE("Stop reaches a task's done callback and the pool keeps working", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule shared("_cb", {"release", "first"});
    py_run("import _cb\nfrom concurrent.futures import ThreadPoolExecutor\n_cb.pool = ThreadPoolExecutor(1)\n");
    std::atomic<bool> stop{false};
    std::thread       driver([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        shared.set("release");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop = true;
    });
    const PythonRunOutcome out = run("import _cb\n"
                                     "def first(future):\n    try:\n        while True:\n            pass\n    finally:\n        _cb.first.set()\n"
                                     "f = _cb.pool.submit(_cb.release.wait, 10)\nf.add_done_callback(first)\n",
                                     json::object(), std::chrono::minutes(1), [&] { return stop.load(); });
    driver.join();
    CHECK(out.status == "cancelled");
    CHECK(shared.wait("first", 5));
    CHECK(py_json("__import__('_cb').pool.submit(int, '6').result(5)") == 6);
    py_run("import _cb\n_cb.pool.shutdown()\n");
}

TEST_CASE("a SystemExit in a task's done callback leaves a shared pool working", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // One worker: had the SystemExit ended it, the pool would never run another task.
    SharedModule shared("_cb", {});
    py_run("import _cb\nfrom concurrent.futures import ThreadPoolExecutor\n_cb.pool = ThreadPoolExecutor(1)\n");
    const PythonRunOutcome out = run("import threading, _cb\nrelease = threading.Event()\n"
                                     "def bye(future):\n    raise SystemExit(3)\n"
                                     "f = _cb.pool.submit(release.wait, 10)\nf.add_done_callback(bye)\nrelease.set()\n");
    CHECK(out.status == "finished");
    CHECK_THAT(out.output, ContainsSubstring("SystemExit"));
    CHECK(py_json("__import__('_cb').pool.submit(int, '6').result(5)") == 6);
    py_run("import _cb\n_cb.pool.shutdown()\n");
}

TEST_CASE("a task's future completed by hand while queued leaves a shared pool working", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule shared("_cb", {});
    py_run("import _cb\nfrom concurrent.futures import ThreadPoolExecutor\n_cb.pool = ThreadPoolExecutor(1)\n");
    const PythonRunOutcome out = run("import threading, _cb\nhold = threading.Event()\n_cb.pool.submit(hold.wait, 10)\n"
                                     "f = _cb.pool.submit(int)\nf.set_result(7)\nhold.set()\nresult = f.result()\n");
    CHECK(out.status == "finished");
    CHECK(out.result == 7);
    CHECK(py_json("__import__('_cb').pool.submit(int, '6').result(5)") == 6);
    py_run("import _cb\n_cb.pool.shutdown()\n");
}

TEST_CASE("a submit whose accounting callback can't be added does not hang the run", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const auto             start = std::chrono::steady_clock::now();
    const PythonRunOutcome out   = run("import concurrent.futures\npool = concurrent.futures.ThreadPoolExecutor(1)\n"
                                       "original = concurrent.futures.Future.add_done_callback\n"
                                       "def refuse(self, fn):\n    raise MemoryError('no room')\n"
                                       "concurrent.futures.Future.add_done_callback = refuse\n"
                                       "try:\n    pool.submit(int)\nexcept MemoryError as e:\n    result = str(e)\n"
                                       "finally:\n    concurrent.futures.Future.add_done_callback = original\n",
                                       json::object(), std::chrono::seconds(20));
    CHECK(out.status == "finished");
    CHECK(out.result == "no room");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
}

TEST_CASE("a pool's initializer runs inside the run that started its worker", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    FakeBridge             bridge;
    ScopedToolBridge       bound(bridge);
    const PythonRunOutcome out = run("import orca\nfrom concurrent.futures import ThreadPoolExecutor\n"
                                     "def init():\n    print('init ran')\n    orca.host.tools.call('get_state')\n"
                                     "pool = ThreadPoolExecutor(1, initializer=init)\nresult = pool.submit(int, '5').result(10)\n");
    CHECK(out.status == "finished");
    CHECK(out.result == 5);
    CHECK_THAT(out.output, ContainsSubstring("init ran"));
    CHECK(bridge.calls == std::vector<std::string>{"get_state"});
}

TEST_CASE("a pool's initializer in a plugin's run runs under the plugin's identity", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // The plugin may start its pool's thread: the test grants what it asks for.
    PluginAuditManager::set_permission_prompt_for_testing([](const std::string&, const std::vector<std::string>&) { return true; });
    PythonRunRequest request;
    request.code     = "import orca\nfrom concurrent.futures import ThreadPoolExecutor\n"
                       "def init():\n    try:\n        orca.host.plugin.storage()\n    except RuntimeError as e:\n        print('storage:', e)\n"
                       "pool = ThreadPoolExecutor(1, initializer=init)\npool.submit(int).result(10)\n";
    request.identity = "test_plugin_init";
    const PythonRunOutcome out = run_python_code(request);
    INFO(out.message << "\n" << out.traceback);
    CHECK(out.status == "finished");
    CHECK_THAT(out.output, ContainsSubstring("not registered"));
}

TEST_CASE("a run waits for a pool's initializer whose task it cancelled", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule     shared("_ini", {"entered", "go"});
    FakeBridge       bridge;
    ScopedToolBridge bound(bridge);
    std::thread      releaser([&] {
        shared.wait("entered", 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        shared.set("go");
    });
    const PythonRunOutcome out = run("import orca, _ini\nfrom concurrent.futures import ThreadPoolExecutor\n"
                                     "def init():\n    _ini.entered.set()\n    _ini.go.wait(10)\n    print('init done')\n"
                                     "    orca.host.tools.call('get_state')\n"
                                     "pool = ThreadPoolExecutor(1, initializer=init)\nf = pool.submit(int)\n_ini.entered.wait(10)\n"
                                     "cancelled = f.cancel()\n");
    releaser.join();
    CHECK(out.status == "finished");
    CHECK_THAT(out.output, ContainsSubstring("init done"));
    CHECK(bridge.calls == std::vector<std::string>{"get_state"});
}

TEST_CASE("a thread of an ended run can't start a pool worker whose initializer would run outside it", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // The daemon thread starts a second worker after the run ended, as a submit whose task the first worker already
    // ran does when it then spins up another worker.
    SharedModule shared("_k", {"go", "done"});
    py_run("import _k\n_k.inits = []\ndef init(k=_k):\n    k.inits.append(1)\n"
           "from concurrent.futures import ThreadPoolExecutor\n_k.pool = ThreadPoolExecutor(2, initializer=init)\n");
    REQUIRE(run("import threading, _k\n_k.pool.submit(int).result(5)\n"
                "def late(pool=_k.pool, k=_k):\n    k.go.wait(10)\n"
                "    while pool._idle_semaphore.acquire(timeout=0):\n        pass\n"
                "    try:\n        pool._adjust_thread_count()\n    except RuntimeError as e:\n        k.error = str(e)\n"
                "    k.done.set()\n"
                "threading.Thread(target=late, daemon=True).start()\n")
                .status == "finished");
    shared.set("go");
    REQUIRE(shared.wait("done", 10));
    CHECK_THAT(shared.get("error").get<std::string>(), ContainsSubstring("run has ended"));
    CHECK(shared.get("inits") == json::array({1}));
    py_run("import _k\n_k.pool.shutdown()\n");
}

TEST_CASE("a pool shutdown an interrupt cut short is done again", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // The first shutdown is cut as an interrupt would cut CPython's: after _shutdown is set, before the workers' wake-up.
    SharedModule           shared("_k", {});
    const PythonRunOutcome out = run("import sys, _k\nfrom concurrent.futures import ThreadPoolExecutor\n"
                                     "Interrupted = sys.modules['_orca_run_capture'].RunInterrupted\n"
                                     "class Pool(ThreadPoolExecutor):\n    cut = True\n"
                                     "    def shutdown(self, wait=True, *, cancel_futures=False):\n"
                                     "        if Pool.cut:\n            Pool.cut = False\n            self._shutdown = True\n"
                                     "            raise Interrupted()\n"
                                     "        super().shutdown(wait=wait, cancel_futures=cancel_futures)\n"
                                     "_k.pool = Pool(1)\n_k.pool.submit(int).result(5)\n");
    CHECK(out.status == "finished");
    CHECK(py_json("(lambda p: [t.join(5) for t in p._threads] and [t.is_alive() for t in p._threads])(__import__('_k').pool)") ==
          json::array({false}));
}

TEST_CASE("Stop reaches a pool's initializer", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule      shared("_stop", {"ended"});
    std::atomic<bool> stop{false};
    std::thread       stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop = true;
    });
    const PythonRunOutcome out = run("import time, _stop\nfrom concurrent.futures import ThreadPoolExecutor\n"
                                     "def init():\n    try:\n        while True:\n            time.sleep(0.05)\n"
                                     "    finally:\n        _stop.ended.set()\n"
                                     "pool = ThreadPoolExecutor(1, initializer=init)\npool.submit(int).result()\n",
                                     json::object(), std::chrono::minutes(1), [&] { return stop.load(); });
    stopper.join();
    CHECK(out.status == "cancelled");
    CHECK(shared.wait("ended", 5));
}

TEST_CASE("a task cancelled while queued is done to wait()", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const PythonRunOutcome out = run("import threading\nfrom concurrent.futures import ThreadPoolExecutor, wait\n"
                                     "pool = ThreadPoolExecutor(1)\nrelease = threading.Event()\n"
                                     "pool.submit(release.wait, 10)\nsecond = pool.submit(int)\nsecond.cancel()\nrelease.set()\n"
                                     "done, _ = wait([second], timeout=5)\nresult = second in done\n",
                                     json::object(), std::chrono::seconds(30));
    CHECK(out.status == "finished");
    CHECK(out.result == true);
}

TEST_CASE("a task a broken pool fails does not hang the run", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const auto             start = std::chrono::steady_clock::now();
    const PythonRunOutcome out   = run("from concurrent.futures import ThreadPoolExecutor\n"
                                       "def boom():\n    raise ValueError('no start')\n"
                                       "pool = ThreadPoolExecutor(1, initializer=boom)\nf = pool.submit(int)\n"
                                       "result = type(f.exception(5)).__name__\n",
                                       json::object(), std::chrono::seconds(20));
    CHECK(out.status == "finished");
    CHECK(out.result == "BrokenThreadPool");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
}

TEST_CASE("a submit that raises after queueing its task does not hang the run", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // The task is queued, but the pool can't start the worker that would run it.
    const auto             start = std::chrono::steady_clock::now();
    const PythonRunOutcome out   = run("from concurrent.futures import ThreadPoolExecutor\npool = ThreadPoolExecutor(1)\n"
                                       "def no_thread():\n    raise RuntimeError('no thread')\npool._adjust_thread_count = no_thread\n"
                                       "try:\n    pool.submit(int)\nexcept RuntimeError as e:\n    result = str(e)\n",
                                       json::object(), std::chrono::seconds(20));
    CHECK(out.status == "finished");
    CHECK(out.result == "no thread");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
}

TEST_CASE("a cancelled task is uncounted once", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const bool   by_shutdown = GENERATE(false, true);
    SharedModule shared("_cb", {"release"});
    std::thread  releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        shared.set("release");
    });
    const auto start = std::chrono::steady_clock::now();
    const PythonRunOutcome out = run(std::string("import _cb\nfrom concurrent.futures import ThreadPoolExecutor\n"
                                                 "pool = ThreadPoolExecutor(1)\npool.submit(_cb.release.wait, 10)\n"
                                                 "second = pool.submit(int)\n") +
                                     (by_shutdown ? "pool.shutdown(wait=False, cancel_futures=True)\n" : "second.cancel()\n"));
    releaser.join();
    CHECK(out.status == "finished");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

TEST_CASE("an untagged pool is still a pool to a run", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule shared("_kept", {});
    py_run("import _kept\nfrom concurrent.futures import ThreadPoolExecutor\n_kept.pool = ThreadPoolExecutor(1)\n"
           "_kept.pool.__dict__.pop('_orca_origin', None)\n");
    FakeBridge       bridge;
    ScopedToolBridge bound(bridge);
    const auto       start = std::chrono::steady_clock::now();
    CHECK(run("import orca, _kept\n_kept.pool.submit(lambda: orca.host.tools.call('get_state'))\n").status == "finished");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK(bridge.calls == std::vector<std::string>{"get_state"});
    py_run("import _kept\n_kept.pool.shutdown()\n");
}

TEST_CASE("a submit from a thread of an ended run is refused", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule shared("_late", {"go", "done"});
    py_run("import _late\nfrom concurrent.futures import ThreadPoolExecutor\n_late.pool = ThreadPoolExecutor(1)\n");
    REQUIRE(run("import threading, _late\n"
                "def work(late=_late):\n    late.go.wait(10)\n    try:\n        late.pool.submit(int)\n"
                "    except RuntimeError as e:\n        late.error = str(e)\n    late.done.set()\n"
                "threading.Thread(target=work, daemon=True).start()\n")
                .status == "finished");
    shared.set("go");
    REQUIRE(shared.wait("done", 5));
    CHECK_THAT(shared.get("error").get<std::string>(), ContainsSubstring("run has ended"));
    py_run("import _late\n_late.pool.shutdown()\n");
}

TEST_CASE("runs from one folder share a helper's pool and atexit until the last of them ends", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule shared("_overlap", {"a_started", "b_started", "go_a", "go_b"});
    MacroDir     macro;
    macro.write("helper_atexit.py", "import atexit\nfrom concurrent.futures import ThreadPoolExecutor\nPOOL = ThreadPoolExecutor(1)\n"
                                    "atexit.register(print, 'helper done')\n");
    const std::string a_code = "import _overlap, helper_atexit\n_overlap.pool = helper_atexit.POOL\n_overlap.a_started.set()\n"
                               "_overlap.go_a.wait(10)\n";
    const std::string b_code = "import _overlap, helper_atexit\n_overlap.b_started.set()\n_overlap.go_b.wait(10)\n"
                               "result = helper_atexit.POOL.submit(int, '4').result(5)\n";
    const std::string a_path = macro.write("a.py", a_code);
    const std::string b_path = macro.write("b.py", b_code);
    PythonRunOutcome  a_out, b_out;
    std::thread       a([&] { a_out = run_file(a_path, a_code); });
    REQUIRE(shared.wait("a_started", 10));
    std::thread b([&] { b_out = run_file(b_path, b_code); });
    REQUIRE(shared.wait("b_started", 10));
    shared.set("go_a");
    a.join();
    shared.set("go_b");
    b.join();
    CHECK_THAT(a_out.output, !ContainsSubstring("helper done"));
    CHECK(b_out.result == 4);
    CHECK_THAT(b_out.output, ContainsSubstring("helper done"));
    CHECK(shared.get("pool._shutdown") == true);
}

TEST_CASE("a folder's teardown waits for a plugin import in progress, at the end and at the next entry", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule shared("_overlap", {"a_imported", "go_a", "imported", "release"});
    MacroDir     macro;
    macro.write("helper_import.py", "from concurrent.futures import ThreadPoolExecutor\nPOOL = ThreadPoolExecutor(1)\n");
    const std::string plugmod = macro.write("plugmod_import.py", "import _overlap, helper_import\n_overlap.imported.set()\n"
                                                                 "_overlap.release.wait(10)\n"
                                                                 "_overlap.value = helper_import.POOL.submit(int, '7').result(5)\n");
    const std::string a_code = "import _overlap, helper_import\n_overlap.a_imported.set()\n_overlap.go_a.wait(10)\n";
    const std::string a_path = macro.write("a.py", a_code);
    const std::string b_code = "x = 1\n";
    const std::string b_path = macro.write("b.py", b_code);

    // Run A imports the helper; a plugin's import takes it and pauses; A ends, then B enters and ends.
    PythonRunOutcome a_out;
    std::thread      a([&] { a_out = run_file(a_path, a_code); });
    REQUIRE(shared.wait("a_imported", 10));
    std::string              error;
    std::vector<std::string> paths, modules;
    PyObject*                module = nullptr;
    std::thread              plugin([&] { module = PythonInterpreter::instance().load_module_from_file(plugmod, error, &paths, &modules); });
    REQUIRE(shared.wait("imported", 10));
    shared.set("go_a");
    a.join();
    const PythonRunOutcome b_out = run_file(b_path, b_code);
    shared.set("release");
    plugin.join();
    CHECK(a_out.status == "finished");
    CHECK(b_out.status == "finished");
    REQUIRE(module != nullptr);
    CHECK(shared.get("value") == 7);
    CHECK(py_json("'helper_import' in sys.modules") == true);
    CHECK(std::find(modules.begin(), modules.end(), "helper_import") != modules.end());

    // A later run tears the folder down: the plugin's helper and its pool stay.
    REQUIRE(run_file(b_path, b_code).status == "finished");
    CHECK(py_json("'helper_import' in sys.modules") == true);
    CHECK(py_json("__import__('helper_import').POOL.submit(int, '8').result(5)") == 8);
    PythonInterpreter::instance().unload_module(module, "plugmod_import", paths, modules);
}

TEST_CASE("atexit functions a script registers run when its run ends, last first", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // A first run installs the run helper, whose imports register the standard library's own atexit hooks.
    REQUIRE(run("x = 1").status == "finished");
    const json             before = py_json("__import__('atexit')._ncallbacks()");
    const PythonRunOutcome out    = run("import atexit, functools\natexit.register(print, 'one')\n"
                                        "atexit.register(functools.partial(print, 'two'))\nprint('body')\n");
    REQUIRE(out.status == "finished");
    CHECK(out.output == "body\ntwo\none\n");
    CHECK(py_json("__import__('atexit')._ncallbacks()") == before);
}

TEST_CASE("a library's atexit registration made during a run stays for the process", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    REQUIRE(run("x = 1").status == "finished");
    const json        before = py_json("__import__('atexit')._ncallbacks()");
    MacroDir          macro;
    macro.write("lib/libexit.py", "import atexit\nCALLED = []\ndef _bye():\n    CALLED.append(1)\natexit.register(_bye)\n");
    const std::string code = "import os, sys\nsys.path.append(os.path.join(os.path.dirname(__file__), 'lib'))\nimport libexit\n";
    REQUIRE(run_file(macro.write("main.py", code), code).status == "finished");
    CHECK(py_json("__import__('libexit').CALLED") == json::array());
    CHECK(py_json("__import__('atexit')._ncallbacks()") == before.get<int>() + 1);
    py_run("import atexit, sys, libexit\natexit.unregister(libexit._bye)\nsys.modules.pop('libexit', None)\n"
           "sys.path[:] = [p for p in sys.path if 'orca-macro-' not in p]\n");
}

TEST_CASE("atexit.unregister in one run leaves another run's registration", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule shared("_ae", {"a_registered", "go_a"});
    py_run("import _ae\n_ae.calls = []\ndef mark(tag, calls=_ae.calls):\n    calls.append(tag)\n_ae.mark = mark\n");
    PythonRunOutcome a_out;
    std::thread      a([&] { a_out = run("import atexit, _ae\natexit.register(_ae.mark, 'a')\n_ae.a_registered.set()\n_ae.go_a.wait(10)\n"); });
    REQUIRE(shared.wait("a_registered", 10));
    const PythonRunOutcome b_out = run("import atexit, _ae\natexit.register(_ae.mark, 'b')\natexit.unregister(_ae.mark)\n");
    shared.set("go_a");
    a.join();
    CHECK(b_out.status == "finished");
    CHECK(a_out.status == "finished");
    CHECK(shared.get("calls") == json::array({"a"}));
}

TEST_CASE("a helper a plugin also loaded keeps its atexit functions for the process", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    REQUIRE(run("x = 1").status == "finished");
    const json   before = py_json("__import__('atexit')._ncallbacks()");
    SharedModule shared("_overlap", {"started", "go"});
    MacroDir     macro;
    macro.write("helper_ae.py", "import atexit\nCLOSED = []\ndef _close():\n    CLOSED.append(1)\n    print('helper closed')\n"
                                "atexit.register(_close)\n");
    const std::string plugmod = macro.write("plugmod_ae.py", "import helper_ae\n");
    const std::string code    = "import _overlap, helper_ae\n_overlap.started.set()\n_overlap.go.wait(10)\n";
    const std::string path    = macro.write("main.py", code);
    PythonRunOutcome  out;
    std::thread       a([&] { out = run_file(path, code); });
    REQUIRE(shared.wait("started", 10));
    std::string              error;
    std::vector<std::string> paths, modules;
    PyObject*                module = PythonInterpreter::instance().load_module_from_file(plugmod, error, &paths, &modules);
    shared.set("go");
    a.join();
    CHECK(out.status == "finished");
    REQUIRE(module != nullptr);
    CHECK(py_json("__import__('helper_ae').CLOSED") == json::array());
    CHECK_THAT(out.output, !ContainsSubstring("helper closed"));
    CHECK(py_json("__import__('atexit')._ncallbacks()") == before.get<int>() + 1);
    py_run("import atexit, helper_ae\natexit.unregister(helper_ae._close)\n");
    PythonInterpreter::instance().unload_module(module, "plugmod_ae", paths, modules);
}

TEST_CASE("Stop interrupts the threads a run started", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule      shared("_stop", {"ended"});
    std::atomic<bool> stop{false};
    std::thread       stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop = true;
    });
    const PythonRunOutcome out = run("import threading, time, _stop\n"
                                     "def spin():\n    try:\n        while True:\n            time.sleep(0.05)\n"
                                     "    finally:\n        _stop.ended.set()\n"
                                     "threading.Thread(target=spin).start()\n",
                                     json::object(), std::chrono::minutes(1), [&] { return stop.load(); });
    stopper.join();
    CHECK(out.status == "cancelled");
    CHECK(shared.wait("ended", 5));
}

TEST_CASE("Stop does not hang on a thread that ignores it", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    std::atomic<bool> stop{false};
    std::thread       stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop = true;
    });
    const auto             start = std::chrono::steady_clock::now();
    const PythonRunOutcome out   = run("import threading, time\nthreading.Thread(target=time.sleep, args=(30,)).start()\n",
                                       json::object(), std::chrono::minutes(1), [&] { return stop.load(); });
    stopper.join();
    CHECK(out.status == "cancelled");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(15));
}

TEST_CASE("a stopped run's atexit functions still run and the app's state is restored", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const json        state = py_json("[list(sys.argv), id(sys.modules['__main__'].__dict__)]");
    MacroDir          macro;
    macro.write("helper_stop.py", "VALUE = 1\n");
    const std::string code = "import atexit, helper_stop\natexit.register(print, 'goodbye')\nwhile True:\n    pass\n";
    std::atomic<bool> stop{false};
    std::thread       stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop = true;
    });
    const PythonRunOutcome out = run_file(macro.write("main.py", code), code, std::chrono::minutes(1), [&] { return stop.load(); });
    stopper.join();
    CHECK(out.status == "cancelled");
    CHECK_THAT(out.output, ContainsSubstring("goodbye"));
    CHECK(py_json("[list(sys.argv), id(sys.modules['__main__'].__dict__)]") == state);
    const std::string next = "x = 1\n";
    REQUIRE(run_file(macro.write("next.py", next), next).status == "finished");
    CHECK(py_json("'helper_stop' in sys.modules") == false);
}

TEST_CASE("a finalizer of what a script left in sys.argv is still stopped by the run's time limit", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const auto             start = std::chrono::steady_clock::now();
    const PythonRunOutcome out   = run("import sys\nclass Loop:\n    def __del__(self):\n        while True:\n            pass\n"
                                       "sys.argv.append(Loop())\n",
                                       json::object(), std::chrono::seconds(1));
    CHECK(out.status == "timed_out");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
}

TEST_CASE("a run with no time limit runs past what a limit would allow", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::string code = "import time\ntime.sleep(1.5)\nresult = 1\n";
    CHECK(run(code, json::object(), std::chrono::milliseconds(0)).status == "finished");
    CHECK(run(code, json::object(), std::chrono::milliseconds(1000)).status == "timed_out");
}

TEST_CASE("a window callback of a run is refused tool calls", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    FakeBridge      bridge;
    auto            scope = std::make_shared<PythonRunScope>(0, kScriptIdentityPerson, boost::filesystem::path(), &bridge);
    ScopedPythonRun in_run(scope, PythonRunThread::Callback);
    PythonGILState  gil;
    py::dict        ns;
    py::exec("import orca\ntry:\n    orca.host.tools.call('get_state')\n    error = None\nexcept RuntimeError as e:\n    error = str(e)\n", ns);
    CHECK_THAT(ns["error"].cast<std::string>(), ContainsSubstring("window callbacks"));
    CHECK(bridge.calls.empty());
}

TEST_CASE("a script passes every plugin gate it can reach without an app", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // Without an app every one of these fails further on; none may fail at a plugins-only gate. A gate left anywhere
    // would not compile: require_plugin_caller() is gone.
    const PythonRunOutcome out = run(R"PY(
import orca
messages = []
for attempt in (lambda: orca.host.preset.process.begin_update(),
                lambda: orca.host.preset.process.update("layer_height", "0.2"),
                lambda: orca.host.preset.filament.begin_update(1),
                lambda: orca.host.preset.filament.update(1, "filament_flow_ratio", "0.98"),
                lambda: orca.host.preset.bundles.reload_local("x"),
                lambda: orca.host.plater().export_3mf_copy("x.3mf"),
                lambda: orca.host.ui.message("hello"),
                lambda: orca.host.ui.create_window("<p>x</p>"),
                lambda: orca.host.ui.create_dock_panel("<p>x</p>"),
                lambda: orca.host.ui.create_progress_dialog("t", "m"),
                lambda: orca.host.ui.push_notification("n")):
    try:
        attempt()
        messages.append("allowed")
    except Exception as e:
        messages.append(str(e))
result = messages
)PY");
    REQUIRE(out.status == "finished");
    REQUIRE(out.result.size() == 11);
    for (const json& message : out.result)
        CHECK_THAT(message.get<std::string>(), !ContainsSubstring("is for plugins"));
}

TEST_CASE("storage() is the folder of a script's file", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    MacroDir               macro;
    const std::string      code = "import orca\nresult = orca.host.plugin.storage()\n";
    const PythonRunOutcome out  = run_file(macro.write("main.py", code), code);
    REQUIRE(out.status == "finished");
    CHECK(utf8_path(out.result.get<std::string>()) == macro.dir);

    const PythonRunOutcome text = run("import orca\ntry:\n    orca.host.plugin.storage()\nexcept RuntimeError as e:\n    result = str(e)\n");
    REQUIRE(text.status == "finished");
    CHECK_THAT(text.result.get<std::string>(), ContainsSubstring("no folder"));
}

TEST_CASE("a window callback runs inside the script's run", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // The script hands its callback to the host through the bridge, as create_window does; the test then delivers a
    // page message to it from another thread while the run lasts.
    SharedModule                              shared("_cb", {"done"});
    std::function<void(const nlohmann::json&)> handler;
    FakeBridge                                bridge;
    bridge.hook = [&](const std::string& tool, const json&) -> std::optional<json> {
        if (tool != "take_callback")
            return std::nullopt;
        PythonGILState gil;
        handler = PluginHostUi::message_callback(py::module_::import("_cb").attr("on_message"));
        return json::object();
    };
    std::thread delivery;
    bridge.after = [&](const std::string& tool) {
        if (tool == "take_callback")
            delivery = std::thread([&] { handler(json{{"x", 1}}); });
    };
    ScopedToolBridge  bound(bridge);
    MacroDir          macro;
    const std::string code = R"PY(
import orca, threading, _cb
def on_message(data):
    _cb.storage = orca.host.plugin.storage()
    try:
        orca.host.tools.call("get_state")
    except RuntimeError as e:
        _cb.refused = str(e)
    worker = threading.Thread(target=lambda: orca.host.tools.call("get_state"))
    worker.start()
    worker.join()
    _cb.done.set()
_cb.on_message = on_message
orca.host.tools.call("take_callback")
_cb.done.wait(10)
)PY";
    REQUIRE(run_file(macro.write("main.py", code), code).status == "finished");
    delivery.join();
    CHECK(utf8_path(shared.get("storage").get<std::string>()) == macro.dir);
    CHECK_THAT(shared.get("refused").get<std::string>(), ContainsSubstring("window callbacks"));
    CHECK(bridge.calls == std::vector<std::string>{"take_callback", "get_state"});
}

TEST_CASE("a run's callbacks do nothing and its threads open no UI once its UI is closed", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule                               shared("_cb", {"called"});
    auto                                       scope = std::make_shared<PythonRunScope>(1, kScriptIdentityPerson, boost::filesystem::path(), nullptr);
    std::function<void(const nlohmann::json&)> handler;
    {
        ScopedPythonRun in_run(scope, PythonRunThread::Main);
        PythonGILState  gil;
        handler = PluginHostUi::message_callback(py::eval("lambda data: __import__('_cb').called.set()"));
    }
    scope->mark_ui_closed();
    handler(json::object());
    CHECK(shared.wait("called", 0.2) == false);

    ScopedPythonRun in_run(scope, PythonRunThread::Started);
    PythonGILState  gil;
    py::dict        ns;
    py::exec("import orca\ntry:\n    orca.host.ui.message('x')\n    error = None\nexcept RuntimeError as e:\n    error = str(e)\n", ns);
    CHECK_THAT(ns["error"].cast<std::string>(), ContainsSubstring("run has ended"));
}

TEST_CASE("a plugin's run leaves its window callbacks and its storage to the plugin", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule                               shared("_cb", {"entered", "go"});
    MacroDir                                   macro;
    const std::string                          path  = macro.write("main.py", "x = 1\n");
    auto                                       scope = std::make_shared<PythonRunScope>(2, "test_plugin_ui", utf8_path(path), nullptr);
    std::function<void(const nlohmann::json&)> handler;
    {
        ScopedPythonRun in_run(scope, PythonRunThread::Main);
        PythonGILState  gil;
        handler = PluginHostUi::message_callback(py::eval("lambda data: (__import__('_cb').entered.set(), __import__('_cb').go.wait(10))"));
    }
    // A script's run counts a running callback and drops one that comes after it ended; a plugin's does neither.
    std::thread delivery([&] { handler(json::object()); });
    REQUIRE(shared.wait("entered", 10));
    CHECK(scope->close_admission_if_quiet());
    shared.set("go");
    delivery.join();
    py_run("import _cb\n_cb.entered.clear()\n");
    std::thread later([&] { handler(json::object()); });
    CHECK(shared.wait("entered", 10));
    later.join();

    ScopedPythonRun          in_run(scope, PythonRunThread::Main);
    ScopedPluginAuditContext audit("test_plugin_ui");
    PythonGILState           gil;
    py::dict                 ns;
    py::exec("import orca\ntry:\n    orca.host.plugin.storage()\n    error = None\nexcept RuntimeError as e:\n    error = str(e)\n", ns);
    CHECK_THAT(ns["error"].cast<std::string>(), ContainsSubstring("not registered"));
}

TEST_CASE("a stopped run opens no more UI", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    std::atomic<bool> stop{false};
    std::thread       stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop = true;
    });
    PythonRunRequest request;
    request.code      = "import atexit, orca\n"
                        "def later():\n    try:\n        orca.host.ui.message('x')\n    except RuntimeError as e:\n        print(e)\n"
                        "atexit.register(later)\nwhile True:\n    pass\n";
    request.identity  = kScriptIdentityPerson;
    request.cancelled = [&] { return stop.load(); };
    const PythonRunOutcome out = run_python_code(request);
    stopper.join();
    CHECK(out.status == "cancelled");
    CHECK_THAT(out.output, ContainsSubstring("run has ended"));
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

namespace {

// Records what a run asks to install, in place of uv, for the test's lifetime.
struct InstallerHook
{
    std::vector<std::vector<std::string>> installs;
    bool                                  succeed = true;
    std::string                           error   = "no network";
    InstallerHook()
    {
        set_python_dependency_installer_for_testing([this](const std::vector<std::string>& packages, std::string& message) {
            installs.push_back(packages);
            if (!succeed)
                message = error;
            return succeed;
        });
    }
    ~InstallerHook() { set_python_dependency_installer_for_testing(nullptr); }
    InstallerHook(const InstallerHook&)            = delete;
    InstallerHook& operator=(const InstallerHook&) = delete;
};

// `text` with its line ends made CRLF.
std::string crlf(const std::string& text)
{
    std::string out;
    for (const char c : text) {
        if (c == '\n')
            out += '\r';
        out += c;
    }
    return out;
}

} // namespace

TEST_CASE("a script file's dependencies are installed before its code runs", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const bool        windows_lines = GENERATE(false, true);
    InstallerHook     hook;
    MacroDir          macro;
    std::string       code = "# /// script\n"
                             "# dependencies = [\n"
                             "#   \"requests>=2,<3\",  # pinned\n"
                             "#   \"rich\",\n"
                             "# ]\n"
                             "# ///\n"
                             "result = 1\n";
    if (windows_lines)
        code = crlf(code);
    const std::string path = macro.write("deps.py", code);
    // The text as the dialog hands it over: read back and decoded as Python reads a source file.
    std::string text, encoding, error;
    REQUIRE(decode_python_source(code, text, encoding, error));
    const PythonRunOutcome out = run_file(path, text);
    CHECK(out.status == "finished");
    REQUIRE(hook.installs.size() == 1);
    CHECK(hook.installs[0] == std::vector<std::string>{"requests>=2,<3", "rich"});
}

TEST_CASE("a failed install stops the run before its code", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    InstallerHook hook;
    hook.succeed = false;
    MacroDir          macro;
    const std::string code = "# /// script\n# dependencies = [\"rich\"]\n# ///\n"
                             "import os\nopen(os.path.join(os.path.dirname(__file__), 'ran.txt'), 'w').close()\n";
    const PythonRunOutcome out = run_file(macro.write("deps.py", code), code);
    CHECK(out.status == "raised");
    CHECK_THAT(out.message, ContainsSubstring("no network"));
    CHECK_FALSE(boost::filesystem::exists(macro.dir / "ran.txt"));
}

TEST_CASE("a malformed PEP 723 block stops the run before its code", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::string block = GENERATE(std::string("# /// script\n# dependencies = [\"x\",\n# ///\n"),
                                       std::string("# /// script\n# dependencies = []\n# ///\n# /// script\n# dependencies = []\n# ///\n"),
                                       std::string("# /// script\n# dependencies = \"x\"\n# ///\n"));
    InstallerHook     hook;
    MacroDir          macro;
    const std::string code = block + "import os\nopen(os.path.join(os.path.dirname(__file__), 'ran.txt'), 'w').close()\n";
    const PythonRunOutcome out = run_file(macro.write("deps.py", code), code);
    CHECK(out.status == "raised");
    CHECK_THAT(out.message, ContainsSubstring("PEP 723"));
    CHECK(hook.installs.empty());
    CHECK_FALSE(boost::filesystem::exists(macro.dir / "ran.txt"));
}

TEST_CASE("code sent as text installs nothing", "[PythonScript][Python]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    InstallerHook          hook;
    const PythonRunOutcome out = run("# /// script\n# dependencies = [\"rich\"]\n# ///\nresult = 2\n");
    CHECK(out.status == "finished");
    CHECK(hook.installs.empty());
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

namespace {

// A session hosted on a thread of its own, as the app hosts one on a job thread Python didn't start. The test's thread
// submits work, reads the events and asserts; the host thread only hosts.
struct HostedSession
{
    PythonSession     session;
    std::thread       host;
    std::atomic<bool> returned{false};

    static PythonSessionRequest person_session(std::string filename = "<run_python>")
    {
        PythonSessionRequest request;
        request.identity = kScriptIdentityPerson;
        request.filename = std::move(filename);
        return request;
    }

    explicit HostedSession(PythonSessionRequest request = person_session())
    {
        host = std::thread([this, request = std::move(request)] {
            host_python_session(request, session);
            returned = true;
        });
    }
    ~HostedSession()
    {
        session.stop();
        session.end();
        if (host.joinable())
            host.join();
    }
    HostedSession(const HostedSession&)            = delete;
    HostedSession& operator=(const HostedSession&) = delete;
};

// The events of one piece of work, up to and including its Finished event.
struct WorkEvents
{
    std::vector<PythonSessionEvent>   events;
    std::optional<PythonSessionEvent> finished;

    std::string output() const
    {
        std::string text;
        for (const PythonSessionEvent& event : events)
            if (event.kind == PythonSessionEvent::Kind::Output)
                text += event.text;
        return text;
    }
    // The Result events' JSON, in order.
    std::vector<std::string> results() const
    {
        std::vector<std::string> texts;
        for (const PythonSessionEvent& event : events)
            if (event.kind == PythonSessionEvent::Kind::Result)
                texts.push_back(event.text);
        return texts;
    }
    // "cell N", "out <text>" (consecutive outputs merged), "result <json>" and "finished <status>", in order.
    std::vector<std::string> sequence() const
    {
        std::vector<std::string> out;
        for (const PythonSessionEvent& event : events) {
            if (event.kind == PythonSessionEvent::Kind::Output) {
                if (!out.empty() && out.back().rfind("out ", 0) == 0)
                    out.back() += event.text;
                else
                    out.push_back("out " + event.text);
            } else if (event.kind == PythonSessionEvent::Kind::Cell)
                out.push_back("cell " + std::to_string(event.cell));
            else if (event.kind == PythonSessionEvent::Kind::Result)
                out.push_back("result " + event.text);
            else
                out.push_back("finished " + event.status);
        }
        return out;
    }
};

// Polls the session's events until a Finished event or `seconds`.
WorkEvents await_work(PythonSession& session, double seconds = 10)
{
    WorkEvents result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!result.finished && std::chrono::steady_clock::now() < deadline) {
        for (PythonSessionEvent& event : session.take_events()) {
            if (!result.finished && event.kind == PythonSessionEvent::Kind::Finished)
                result.finished = event;
            result.events.push_back(std::move(event));
        }
        if (!result.finished)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return result;
}

// What the session prints within `seconds`.
std::string output_within(PythonSession& session, double seconds)
{
    std::string text;
    const auto  deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const PythonSessionEvent& event : session.take_events())
            if (event.kind == PythonSessionEvent::Kind::Output)
                text += event.text;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return text;
}

bool eventually(const std::function<bool()>& condition, double seconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

PythonWork cell(std::string code, int first_line = 1, std::string source = {})
{
    PythonWork work;
    work.kind       = PythonWork::Kind::Cell;
    work.first_line = first_line;
    work.source     = source.empty() ? code : std::move(source);
    work.code       = std::move(code);
    return work;
}

PythonWork console(std::string code)
{
    PythonWork work;
    work.kind = PythonWork::Kind::Console;
    work.code = std::move(code);
    return work;
}

PythonWork file_work(std::string code, std::vector<int> cell_starts = {1})
{
    PythonWork work;
    work.kind        = PythonWork::Kind::File;
    work.source      = code;
    work.code        = std::move(code);
    work.cell_starts = std::move(cell_starts);
    return work;
}

WorkEvents run_work(PythonSession& session, PythonWork work)
{
    REQUIRE(session.submit(std::move(work)));
    WorkEvents result = await_work(session);
    REQUIRE(result.finished);
    return result;
}

constexpr const char* kTruncatedNote = "\n[output truncated at 256 KiB]\n";

} // namespace

TEST_CASE("a session keeps what each cell defined for the next one", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    HostedSession hosted;
    CHECK(run_work(hosted.session, cell("x = 2")).finished->status == "finished");
    CHECK(run_work(hosted.session, cell("y = x * 3")).finished->status == "finished");
    const WorkEvents shown = run_work(hosted.session, console("y"));
    CHECK(shown.output() == "6\n");
    CHECK(shown.finished->status == "finished");
    CHECK(shown.finished->variables == 2);
}

TEST_CASE("a cell ending in an expression shows its value, and so does a file run's cell, but not an agent's run", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    HostedSession hosted;
    CHECK(run_work(hosted.session, cell("1 + 1")).output() == "2\n");
    CHECK(run_work(hosted.session, cell("None")).output().empty());
    const WorkEvents file = run_work(hosted.session, file_work("1 + 1"));
    CHECK(file.finished->status == "finished");
    CHECK(file.output() == "2\n");

    const PythonRunOutcome agent = run_file("<run_python>", "1 + 1\n[1, 2]\n");
    CHECK(agent.status == "finished");
    CHECK(agent.output.empty());
}

TEST_CASE("a file run shows the result of each cell whose last statement is an expression", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    HostedSession     hosted;
    const std::string code = "x = 1\nx + 1\n"                                  // cell 0, lines 1-2
                             "# %% two\nprint('b')\n{'a': [1, 2.5, True, None]}\n" // cell 1, lines 3-5
                             "# %% three\nobjects = [1]\n"                       // cell 2, lines 6-7
                             "# %% four\n'done'\nNone\n";                        // cell 3, lines 8-10
    const WorkEvents  file = run_work(hosted.session, file_work(code, {1, 3, 6, 8}));
    CHECK(file.sequence() == std::vector<std::string>{"cell 0", "out 2\n", "cell 1", "out b\n", "result {\"a\": [1, 2.5, true, null]}",
                                                      "cell 2", "cell 3", "finished finished"});
    // `_` is the last result shown, as after a cell.
    CHECK(run_work(hosted.session, console("_ == {'a': [1, 2.5, True, None]}")).output() == "True\n");
}

TEST_CASE("a result comes as JSON only when JSON holds it as is", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    HostedSession hosted;
    const WorkEvents json_result = run_work(hosted.session, cell("{'name': 'Benchy \\u00e9', 'sizes': [[1, 2], {}]}"));
    CHECK(json_result.results() == std::vector<std::string>{"{\"name\": \"Benchy \xC3\xA9\", \"sizes\": [[1, 2], {}]}"});
    CHECK(json_result.output().empty());
    // A str key JSON would write for an int, a tuple it would write as a list, a float it can't write, a string UTF-8
    // can't hold (a lone surrogate, as a key or a value), and anything that is not a dict or a list show as their repr.
    for (const char* code : {"{1: 'a'}", "[(1, 2)]", "[float('nan')]", "{'path': '\\udcff'}", "{'\\udcff': 1}", "'text'", "3"}) {
        const WorkEvents shown = run_work(hosted.session, cell(code));
        CHECK(shown.results().empty());
        CHECK(shown.output() == py_json(std::string("repr(") + code + ")").get<std::string>() + "\n");
    }
    // JSON past the output's allowance is printed instead, and cut as long output is.
    const WorkEvents long_result = run_work(hosted.session, cell("list(range(100000))"));
    CHECK(long_result.results().empty());
    CHECK_THAT(long_result.output(), EndsWith(kTruncatedNote));
}

TEST_CASE("a file run shows its cells' results whatever the script does to __import__ and sys.modules", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    HostedSession     hosted;
    const std::string code = "import sys\ncapture = sys.modules.pop('_orca_run_capture')\n__import__ = None\n1 + 1\n";
    const WorkEvents  file = run_work(hosted.session, file_work(code));
    // The capture module goes back before anything checks, whatever the run did.
    run_work(hosted.session, console("sys.modules['_orca_run_capture'] = capture"));
    CHECK(file.finished->status == "finished");
    CHECK(file.output() == "2\n");
}

TEST_CASE("a file run keeps its docstring and its tracebacks as the file's", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    HostedSession hosted;
    // The docstring is the last statement of its cell.
    const WorkEvents documented = run_work(hosted.session, file_work("'''The plate report.'''\n# %% two\nx = 1\n", {1, 2}));
    CHECK(documented.output().empty());
    CHECK(run_work(hosted.session, console("__doc__")).output() == "'The plate report.'\n");

    const WorkEvents raised = run_work(hosted.session, file_work("x = 1\n# %% two\n1 / x\n1 / 0\n", {1, 3}));
    CHECK(raised.finished->status == "raised");
    CHECK(raised.finished->error_line == 4);
    CHECK_THAT(raised.finished->traceback, ContainsSubstring("1 / 0"));
    CHECK_THAT(raised.finished->traceback, !ContainsSubstring("show_result"));
    CHECK_THAT(raised.finished->traceback, !ContainsSubstring("_orca_run_capture"));
}

TEST_CASE("a session's traceback quotes the editor's text and leaves other runs' tracebacks alone", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    MacroDir    macro;
    std::string disk;
    for (int line = 1; line <= 10; ++line)
        disk += "print('disk line " + std::to_string(line) + "')\n";
    const std::string path = macro.write("script.py", disk);
    // What another run of the same file raises at line 5: its traceback quotes the file on disk.
    const auto disk_run_quotes_disk = [&] {
        const PythonRunOutcome other = run_file(path, "x = 1\n\n\n\nraise ValueError('y')\n");
        CHECK(other.status == "raised");
        CHECK_THAT(other.traceback, ContainsSubstring("disk line 5"));
        CHECK_THAT(other.traceback, !ContainsSubstring("raise ValueError('x')"));
    };

    auto hosted = std::make_unique<HostedSession>(HostedSession::person_session(path));
    const std::string source = "# editor 1\n# 2\n# %% cell\na = 1\nraise ValueError('x')\n";
    const WorkEvents  raised = run_work(hosted->session, cell("a = 1\nraise ValueError('x')", 4, source));
    CHECK(raised.finished->status == "raised");
    CHECK(raised.finished->error_line == 5);
    CHECK(raised.finished->executed);
    CHECK_THAT(raised.finished->traceback, ContainsSubstring("line 5"));
    CHECK_THAT(raised.finished->traceback, ContainsSubstring("raise ValueError('x')"));
    CHECK_THAT(raised.finished->traceback, !ContainsSubstring("disk line"));

    const WorkEvents syntax = run_work(hosted->session, cell("def f(:", 9, source));
    CHECK(syntax.finished->status == "raised");
    CHECK(syntax.finished->error_line == 9);
    CHECK_FALSE(syntax.finished->executed);

    const std::string chained_source = "def g():\n    raise KeyError('inner')\ntry:\n    g()\n"
                                       "except KeyError as e:\n    raise ValueError('outer') from e\n";
    const WorkEvents chained = run_work(hosted->session, cell(chained_source, 1, chained_source));
    CHECK(chained.finished->status == "raised");
    CHECK(chained.finished->error_line == 6);
    CHECK_THAT(chained.finished->traceback, ContainsSubstring("raise KeyError('inner')"));
    CHECK_THAT(chained.finished->traceback, ContainsSubstring("raise ValueError('outer') from e"));
    CHECK_THAT(chained.finished->traceback, !ContainsSubstring("disk line"));

    disk_run_quotes_disk();
    hosted.reset();
    disk_run_quotes_disk();
}

TEST_CASE("a file run reports each cell as it starts and stops where it fails", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    HostedSession    hosted;
    const WorkEvents file = run_work(hosted.session, file_work("print('a')\n# %% two\nprint('b')\n# %% three\nraise RuntimeError('c')\n", {1, 3, 5}));
    CHECK(file.sequence() == std::vector<std::string>{"cell 0", "out a\n", "cell 1", "out b\n", "cell 2", "finished raised"});
    CHECK(file.finished->error_line == 5);
    CHECK(file.finished->cells_reported);
    CHECK(py_json("sys.monitoring.get_tool(4)").is_null());
}

TEST_CASE("a file run sends no cell events while another owner holds the monitoring id", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    py_run("import sys\nsys.monitoring.use_tool_id(4, 'test')\n");
    HostedSession    hosted;
    const WorkEvents file = run_work(hosted.session, file_work("print('a')\n# %% two\nprint('b')\n# %% three\nraise RuntimeError('c')\n", {1, 3, 5}));
    CHECK(file.sequence() == std::vector<std::string>{"out a\nb\n", "finished raised"});
    CHECK(file.finished->error_line == 5);
    CHECK_FALSE(file.finished->cells_reported);
    CHECK(py_json("sys.monitoring.get_tool(4)") == "test");
    py_run("import sys\nsys.monitoring.free_tool_id(4)\n");
}

TEST_CASE("a file run leaves the session able to start threads and tasks", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    HostedSession hosted;
    CHECK(run_work(hosted.session, file_work("import threading\nt = threading.Thread(target=print, args=('x',))\nt.start()\nt.join()\n"))
              .finished->status == "finished");
    const WorkEvents later = run_work(hosted.session, cell("import threading\nfrom concurrent.futures import ThreadPoolExecutor\n"
                                                           "t = threading.Thread(target=print, args=('t',))\nt.start()\nt.join()\n"
                                                           "ThreadPoolExecutor(1).submit(lambda: 7).result()\n"));
    CHECK(later.finished->status == "finished");
    CHECK_THAT(later.output(), ContainsSubstring("7"));
    CHECK_THAT(later.output(), ContainsSubstring("t"));
}

TEST_CASE("output arrives while the work is still running", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule  shared("_stream", {"go"});
    HostedSession hosted;
    REQUIRE(hosted.session.submit(cell("import _stream\nprint('a')\nok = _stream.go.wait(10)\n")));
    std::string early;
    CHECK(eventually(
        [&] {
            for (const PythonSessionEvent& event : hosted.session.take_events())
                if (event.kind == PythonSessionEvent::Kind::Output)
                    early += event.text;
            return early == "a\n";
        },
        5));
    CHECK(hosted.session.working());
    shared.set("go");
    const WorkEvents rest = await_work(hosted.session);
    REQUIRE(rest.finished);
    CHECK(rest.finished->status == "finished");
}

TEST_CASE("Stop interrupts the running cell and keeps the session", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    HostedSession hosted;
    REQUIRE(run_work(hosted.session, cell("x = 1")).finished->status == "finished");
    REQUIRE(hosted.session.submit(cell("while True:\n    pass\n")));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(hosted.session.working());
    hosted.session.stop();
    const WorkEvents stopped = await_work(hosted.session);
    REQUIRE(stopped.finished);
    CHECK(stopped.finished->status == "cancelled");
    CHECK(run_work(hosted.session, cell("x + 1")).output() == "2\n");
}

TEST_CASE("Stop reaches a thread an earlier cell left running, and later cells start threads and tasks again",
          "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    HostedSession hosted;
    REQUIRE(run_work(hosted.session, cell("import threading, time\nfrom concurrent.futures import ThreadPoolExecutor\n"
                                          "def spin():\n    while True:\n        time.sleep(0.01)\n"
                                          "threading.Thread(target=spin).start()\nex = ThreadPoolExecutor(1)\n"))
                .finished->status == "finished");
    CHECK(eventually([&] { return hosted.session.has_threads(); }, 2));
    hosted.session.stop();
    CHECK(eventually([&] { return !hosted.session.has_threads(); }, 5));
    // What the stopped thread printed as it ended comes before the next cell.
    output_within(hosted.session, 0.2);
    CHECK(run_work(hosted.session, cell("ex.submit(lambda: 7).result()")).output() == "7\n");
}

TEST_CASE("a task queued when a cell is stopped never runs, and the next cell's tasks do", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule  shared("_queued", {"ran"});
    HostedSession hosted;
    REQUIRE(hosted.session.submit(cell("import time, _queued\nfrom concurrent.futures import ThreadPoolExecutor\n"
                                       "ex = ThreadPoolExecutor(1)\n"
                                       "def spin():\n    while True:\n        time.sleep(0.01)\n"
                                       "ex.submit(spin)\nsecond = ex.submit(_queued.ran.set)\n"
                                       "while True:\n    time.sleep(0.01)\n")));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    hosted.session.stop();
    const WorkEvents stopped = await_work(hosted.session);
    REQUIRE(stopped.finished);
    CHECK(stopped.finished->status == "cancelled");
    CHECK(run_work(hosted.session, cell("ex.submit(lambda: 3).result()")).output() == "3\n");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK(py_json("__import__('_queued').ran.is_set()") == false);
    CHECK_THAT(run_work(hosted.session, console("str(second.exception(5))")).output(), ContainsSubstring("The task was stopped before it ran"));
}

TEST_CASE("ending a session whose thread ignores Stop returns within its five seconds", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule shared("_stubborn", {"quit"});
    {
        HostedSession hosted;
        // The thread waits in C, where no interrupt lands, until the test lets it go; the atexit function loops until
        // an interrupt ends it.
        REQUIRE(run_work(hosted.session, cell("import atexit, threading, _stubborn\n"
                                              "threading.Thread(target=_stubborn.quit.wait, args=(30,)).start()\n"
                                              "def forever():\n    while True:\n        pass\n"
                                              "atexit.register(forever)\n"))
                    .finished->status == "finished");
        hosted.session.stop();
        const auto start = std::chrono::steady_clock::now();
        hosted.session.end();
        CHECK(eventually([&] { return hosted.returned.load(); }, 8));
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(8));
        CHECK(hosted.session.ended());
        shared.set("quit");
    }
}

TEST_CASE("stopping an idle session as it ends interrupts an atexit function that never returns", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule shared("_spin", {"release"});
    {
        HostedSession hosted;
        // No thread is left: the session is idle with nothing live when it ends, and only its teardown runs the loop.
        REQUIRE(run_work(hosted.session, cell("import atexit, _spin\n"
                                              "def spin():\n    while not _spin.release.is_set():\n        pass\n"
                                              "atexit.register(spin)\n"))
                    .finished->status == "finished");
        REQUIRE_FALSE(hosted.session.has_threads());
        const auto start = std::chrono::steady_clock::now();
        hosted.session.end();
        hosted.session.stop();
        CHECK(eventually([&] { return hosted.returned.load(); }, 8));
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(8));
        // Lets a teardown the stop never reached finish, so the session's thread can be joined.
        shared.set("release");
    }
}

TEST_CASE("a work starts only once the stop before it has finished cancelling the session's tool calls", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // The watcher cancels a stop's tool calls outside its lock; this stop holds that step open until the test lets it go.
    std::atomic<bool>    cancelling{false}, release{false}, cancelled{false}, second_saw_cancelled{false};
    std::atomic<int>     works{0};
    PythonSessionRequest request = HostedSession::person_session();
    request.stop_calls           = [&] {
        cancelling = true;
        for (int i = 0; i < 1000 && !release; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        cancelled = true;
    };
    request.work_started = [&] {
        if (++works == 2)
            second_saw_cancelled = cancelled.load();
    };
    HostedSession hosted(request);
    REQUIRE(hosted.session.submit(cell("import time\ntime.sleep(0.5)\n")));
    REQUIRE(eventually([&] { return works.load() == 1; }, 5));
    hosted.session.stop();
    REQUIRE(eventually([&] { return cancelling.load(); }, 5));
    REQUIRE(await_work(hosted.session).finished); // the sleep ends by itself; the stop's interrupt waits behind the cancel

    REQUIRE(hosted.session.submit(cell("1")));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    release = true;
    REQUIRE(await_work(hosted.session).finished);
    CHECK(second_saw_cancelled);
}

TEST_CASE("a work starts only once every stop being applied has finished, its own and the watcher's", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    // The watcher applies the first stop and is held there; a second stop, asked while the session is idle with a thread
    // left, is taken by the next work's own start, overlapping the first.
    SharedModule         shared("_overlap", {"go"});
    std::atomic<bool>    release{false}, first_done{false}, second_saw_first_done{false};
    std::atomic<int>     calls{0}, works{0};
    PythonSessionRequest request = HostedSession::person_session();
    request.stop_calls           = [&] {
        if (++calls != 1)
            return;
        for (int i = 0; i < 1000 && !release; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        first_done = true;
    };
    request.work_started = [&] {
        if (++works == 2)
            second_saw_first_done = first_done.load();
    };
    {
        HostedSession hosted(request);
        REQUIRE(hosted.session.submit(cell("import threading, time, _overlap\n"
                                           "threading.Thread(target=_overlap.go.wait, args=(30,)).start()\n"
                                           "time.sleep(0.5)\n")));
        REQUIRE(eventually([&] { return works.load() == 1; }, 5));
        hosted.session.stop();
        REQUIRE(eventually([&] { return calls.load() == 1; }, 5));
        REQUIRE(await_work(hosted.session).finished);
        REQUIRE(hosted.session.has_threads());
        hosted.session.stop(); // the watcher is held in the first stop: the second work's start takes this one
        REQUIRE(hosted.session.submit(cell("1")));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        release = true;
        REQUIRE(await_work(hosted.session).finished);
        CHECK(calls.load() == 2);
        CHECK(second_saw_first_done);
        shared.set("go");
    }
}

TEST_CASE("ending a session runs its atexit functions once, inside the session", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule  shared("_bye", {"done"});
    HostedSession hosted;
    REQUIRE(run_work(hosted.session, cell("import atexit, _bye\n_bye.count = 0\n"
                                          "def bye():\n    _bye.count += 1\n    _bye.done.set()\n    print('bye')\n"
                                          "atexit.register(bye)\n"))
                .finished->status == "finished");
    hosted.session.end();
    REQUIRE(eventually([&] { return hosted.session.ended(); }, 10));
    CHECK(shared.wait("done", 1));
    CHECK(shared.get("count") == 1);
    CHECK_THAT(output_within(hosted.session, 0.1), ContainsSubstring("bye"));
}

TEST_CASE("work hooks bracket each piece of work", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule         shared("_hooks", {"go"});
    std::atomic<int>     started{0}, ended{0};
    PythonSessionRequest request = HostedSession::person_session();
    request.work_started         = [&] { ++started; };
    request.work_ended           = [&] { ++ended; };
    HostedSession hosted(request);
    REQUIRE(hosted.session.submit(cell("import _hooks\nok = _hooks.go.wait(10)\n")));
    CHECK(eventually([&] { return started.load() == 1; }, 5));
    CHECK(ended.load() == 0);
    shared.set("go");
    REQUIRE(await_work(hosted.session).finished);
    run_work(hosted.session, cell("1"));
    CHECK(started.load() == 2);
    CHECK(ended.load() == 2);
}

TEST_CASE("submit refuses work while work runs and after end", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule  shared("_busy", {"go"});
    HostedSession hosted;
    REQUIRE(hosted.session.submit(cell("import _busy\nok = _busy.go.wait(10)\n")));
    CHECK_FALSE(hosted.session.submit(cell("1")));
    shared.set("go");
    REQUIRE(await_work(hosted.session).finished);
    hosted.session.end();
    CHECK_FALSE(hosted.session.submit(cell("1")));
    REQUIRE(eventually([&] { return hosted.session.ended(); }, 10));
    CHECK_FALSE(hosted.session.submit(cell("1")));
}

TEST_CASE("a file session imports from its folder in cells and evicts its helpers when it ends", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    MacroDir macro;
    macro.write("session_helper.py", "v = 41\n");
    HostedSession hosted(HostedSession::person_session(macro.write("main.py", "")));
    CHECK(run_work(hosted.session, cell("import session_helper\nsession_helper.v")).output() == "41\n");
    hosted.session.end();
    REQUIRE(eventually([&] { return hosted.session.ended(); }, 10));
    CHECK(py_json("'session_helper' in sys.modules") == false);
}

TEST_CASE("SystemExit(0) in a cell finishes it and keeps the session", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    HostedSession hosted;
    CHECK(run_work(hosted.session, cell("raise SystemExit(0)")).finished->status == "finished");
    CHECK(run_work(hosted.session, cell("1")).output() == "1\n");
}

TEST_CASE("a session's output past 256 KiB is cut with a note, in a work and between works, and the next work streams again",
          "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::size_t limit = 256 * 1024 + std::string(kTruncatedNote).size();
    SharedModule      shared("_chatter", {"go", "done"});
    HostedSession     hosted;
    const std::string in_work = run_work(hosted.session, cell("print('x' * (300 * 1024))")).output();
    CHECK(in_work.size() <= limit);
    CHECK_THAT(in_work, ContainsSubstring("[output truncated at 256 KiB]"));

    REQUIRE(run_work(hosted.session, cell("import threading, _chatter\n"
                                          "def chatter():\n    _chatter.go.wait(10)\n    print('y' * (300 * 1024))\n    _chatter.done.set()\n"
                                          "threading.Thread(target=chatter).start()\n"))
                .finished->status == "finished");
    shared.set("go");
    REQUIRE(shared.wait("done", 10));
    const std::string between = output_within(hosted.session, 0.5);
    CHECK(between.size() <= limit);
    CHECK_THAT(between, ContainsSubstring("[output truncated at 256 KiB]"));

    CHECK(run_work(hosted.session, cell("print('ok')")).output() == "ok\n");
}

TEST_CASE("what a session's threads print after a work that used up its output still arrives", "[PythonScript][Python][PythonSession]")
{
    RunnerHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    SharedModule  shared("_after", {"go", "done"});
    HostedSession hosted;
    REQUIRE(run_work(hosted.session, cell("import threading, _after\n"
                                          "def later():\n    _after.go.wait(10)\n    print('after the work')\n    _after.done.set()\n"
                                          "threading.Thread(target=later).start()\n"
                                          "print('x' * (300 * 1024))\n"))
                .finished->status == "finished");
    shared.set("go");
    REQUIRE(shared.wait("done", 10));
    CHECK_THAT(output_within(hosted.session, 0.5), ContainsSubstring("after the work"));
}
