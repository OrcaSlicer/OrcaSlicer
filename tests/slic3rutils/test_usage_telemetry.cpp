#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/PrivacyConsent.hpp"
#include "slic3r/Utils/UsageTelemetry.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

// 0.1.6: diagnostics run only on a saved privacy choice, never on the old "usage_diagnostics" key.
void choose(AppConfig &config, bool share)
{
    PrivacyConsent::save(config, PrivacyConsent::make_choice("north_america", "CA", share, false, "initial_setup",
                                                             "2026-10-13T12:00:00Z", PrivacyConsent::legal_approvals()));
}
void grant(AppConfig &config) { choose(config, true); }
void decline(AppConfig &config) { choose(config, false); }

std::string slurp(const fs::path &path)
{
    std::ifstream in(path.string(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Every event name in every batch saved for the next launch.
std::vector<std::string> pending_event_names(const fs::path &dir)
{
    std::vector<std::string> names;
    json batches = json::parse(slurp(dir / "usage" / "pending.json"), nullptr, false);
    REQUIRE(batches.is_array());
    for (const auto &body : batches) {
        json batch = json::parse(body.get<std::string>());
        for (const auto &e : batch["events"])
            names.push_back(e["name"].get<std::string>());
    }
    return names;
}

} // namespace

TEST_CASE("Usage diagnostics redact slicing errors", "[UsageTelemetry]")
{
    using UsageTelemetry::redact_message;

    CHECK(redact_message("Nothing to be sliced.", {}) == "Nothing to be sliced.");
    // First line only.
    CHECK(redact_message("Line one\nObject Secret_Bracket has no volume", {}) == "Line one");
    // The model's own names, longest first, wherever they appear.
    CHECK(redact_message("Bracket_v2 and Bracket are too small", {"Bracket", "Bracket_v2"}) == "<name> and <name> are too small");
    // Quoted text and paths.
    CHECK(redact_message("Object \"F-35 mount\" failed", {}) == "Object <...> failed");
    CHECK(redact_message("Cannot write C:\\Users\\someone\\part.gcode now", {}) == "Cannot write <path> now");
    CHECK(redact_message("Cannot write /home/someone/part.gcode now", {}) == "Cannot write <path> now");
    // Numbers and units survive: they are what makes the message useful.
    CHECK(redact_message("Speed 300 mm/s exceeds 250", {}) == "Speed 300 mm/s exceeds 250");
    // Cut to 160 bytes on a character boundary.
    std::string long_utf8;
    for (int i = 0; i < 100; ++i)
        long_utf8 += "\xC3\xA9"; // e acute, 2 bytes
    std::string cut = redact_message(long_utf8, {});
    CHECK(cut.size() == 160);
    CHECK((static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80);
}

TEST_CASE("Usage diagnostics keep batches and find crashes offline", "[UsageTelemetry]")
{
    const std::string old_data_dir = data_dir();
    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-telemetry-%%%%-%%%%");
    fs::create_directories(dir);
    set_data_dir(dir.string());

    AppConfig config;
    // Nothing listens on the discard port, so every send fails fast and is kept for later.
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    grant(config);

    UsageTelemetry::start(config, true);
    if (UsageTelemetry::disabled_by_environment()) {
        UsageTelemetry::shutdown();
        set_data_dir(old_data_dir);
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");
    }
    REQUIRE(UsageTelemetry::enabled());
    const std::string install = config.get("usage_diagnostics_id");
    CHECK(install.size() == 36);

    UsageTelemetry::record("slice", {{"result", "ok"}});
    UsageTelemetry::count("tab:preview", 2);
    UsageTelemetry::shutdown();

    CHECK(pending_event_names(dir) == std::vector<std::string>{"slice", "session_end"});
    {
        json batch = json::parse(json::parse(slurp(dir / "usage" / "pending.json"))[0].get<std::string>());
        CHECK(batch["install"] == install);
        CHECK(batch["events"][1]["counts"]["tab:preview"] == 2);
    }
    // A clean exit leaves no crash marker.
    for (fs::directory_iterator it(dir / "usage"), end; it != end; ++it)
        CHECK(it->path().filename().string().rfind("session-", 0) != 0);

    // A marker from a process that is gone: that session crashed.
    {
        std::ofstream marker((dir / "usage" / "session-dead.json").string());
        marker << json{{"pid", 0x7ffffff0}, {"version", "0.1.1"}, {"started", 1000}, {"alive", 61000}, {"consented", true}}.dump();
    }
    UsageTelemetry::start(config, false);
    UsageTelemetry::shutdown();

    std::vector<std::string> names = pending_event_names(dir);
    CHECK(std::count(names.begin(), names.end(), "crash") == 1);
    CHECK(!fs::exists(dir / "usage" / "session-dead.json"));
    // The first launch's batch is still waiting too.
    CHECK(std::count(names.begin(), names.end(), "slice") == 1);
    // Same install id across launches.
    CHECK(config.get("usage_diagnostics_id") == install);

    // Switching off forgets everything queued.
    UsageTelemetry::start(config, false);
    decline(config);
    UsageTelemetry::refresh(config);
    CHECK(!fs::exists(dir / "usage" / "pending.json"));
    UsageTelemetry::shutdown();

    set_data_dir(old_data_dir);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}


// UNLAYERED_MOD 2026-10-11 (0.1.4) -- a crash says where it happened. Day one of 0.1.3 reported 17
// "crashes" with nothing to go on: no phase, no exception, no location.
TEST_CASE("Usage diagnostics report a crash's phase, exception and location, and skip OS session ends", "[UsageTelemetry]")
{
    const std::string old_data_dir = data_dir();
    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-telemetry-%%%%-%%%%");
    fs::create_directories(dir / "usage");
    set_data_dir(dir.string());

    AppConfig config;
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    grant(config);

    // Two dead sessions: one crashed while slicing, one the operating system ended.
    auto write_marker = [&](const std::string &name, const json &body) {
        std::ofstream out((dir / "usage" / name).string());
        out << body.dump();
    };
    write_marker("session-crashed.json", {{"pid", 0x7ffffff0}, {"version", "0.1.4"}, {"started", 1000}, {"alive", 31000},
                                          {"phase", "slicing"}, {"exception", "0xC0000005"}, {"at", "UnlayeredSlicer.dll+0x1a2b3c"},
                                          {"consented", true}});
    write_marker("session-logoff.json", {{"pid", 0x7ffffff1}, {"version", "0.1.4"}, {"started", 1000}, {"alive", 31000},
                                         {"phase", "os_session_end"}, {"consented", true}});

    UsageTelemetry::start(config, false);
    if (UsageTelemetry::disabled_by_environment()) {
        UsageTelemetry::shutdown();
        set_data_dir(old_data_dir);
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");
    }

    // The live session's marker carries the phase the app says it is in.
    UsageTelemetry::set_phase("slicing");
    fs::path live;
    for (fs::directory_iterator it(dir / "usage"), end; it != end; ++it)
        if (it->path().filename().string().rfind("session-", 0) == 0)
            live = it->path();
    REQUIRE(!live.empty());
    CHECK(json::parse(slurp(live))["phase"] == "slicing");

#ifdef _WIN32
    // A code address is a module file name and an offset, never a path.
    const std::string here = UsageTelemetry::code_location(reinterpret_cast<const void *>(&slurp));
    INFO(here);
    CHECK(here.find("slic3rutils_tests") != std::string::npos);
    CHECK(here.find("+0x") != std::string::npos);
    CHECK(here.find('\\') == std::string::npos);
    CHECK(here.find('/') == std::string::npos);
#endif

    UsageTelemetry::shutdown();

    json crash;
    int  crashes = 0;
    json batches = json::parse(slurp(dir / "usage" / "pending.json"));
    for (const auto &body : batches) {
        // Named, not iterated as a temporary: operator[] on a temporary would dangle.
        const json batch = json::parse(body.get<std::string>());
        for (const auto &e : batch["events"])
            if (e["name"] == "crash") {
                ++crashes;
                crash = e["props"];
            }
    }
    // Only the real crash: the session the operating system ended is not one.
    INFO(slurp(dir / "usage" / "pending.json"));
    CHECK(crashes == 1);
    CHECK(crash["phase"] == "slicing");
    CHECK(crash["exception"] == "0xC0000005");
    CHECK(crash["at"] == "UnlayeredSlicer.dll+0x1a2b3c");
    CHECK(crash["uptime_s"] == 30);
    CHECK(!fs::exists(dir / "usage" / "session-logoff.json"));

    set_data_dir(old_data_dir);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}


// UNLAYERED_MOD 2026-11-02 (0.1.10) -- a customer's 0.1.8 closed 4 s into every launch with
// "[json.exception.type_error.306] cannot use value() with discarded": an unreadable marker threw
// in start() before it was removed, so the next launch met it again.
TEST_CASE("Usage diagnostics survive an unreadable or malformed crash marker", "[UsageTelemetry]")
{
    const std::string old_data_dir = data_dir();
    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-telemetry-%%%%-%%%%");
    fs::create_directories(dir / "usage");
    set_data_dir(dir.string());

    AppConfig config;
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    grant(config);

    auto write_marker = [&](const std::string &name, const std::string &body) {
        std::ofstream out((dir / "usage" / name).string(), std::ios::binary);
        out << body;
    };
    write_marker("session-empty.json", "");
    write_marker("session-zeroed.json", std::string(64, '\0'));
    write_marker("session-cut.json", "{\"pid\":123,\"version\":\"0.1.8\",\"sta");
    write_marker("session-array.json", "[1,2,3]");
    // Parses, but a field has the wrong type: value() throws type_error.302 on it.
    write_marker("session-badtype.json", json{{"pid", 0x7ffffff2}, {"version", 18}, {"started", "soon"}, {"consented", true}}.dump());
    // A good one beside them is still reported.
    write_marker("session-crashed.json", json{{"pid", 0x7ffffff0}, {"version", "0.1.8"}, {"started", 1000}, {"alive", 31000},
                                              {"phase", "slicing"}, {"exception", "0xC0000005"}, {"consented", true}}.dump());

    REQUIRE_NOTHROW(UsageTelemetry::start(config, false));
    if (UsageTelemetry::disabled_by_environment()) {
        UsageTelemetry::shutdown();
        set_data_dir(old_data_dir);
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");
    }
    for (const char *name : {"session-empty.json", "session-zeroed.json", "session-cut.json", "session-array.json",
                             "session-badtype.json", "session-crashed.json"}) {
        INFO(name);
        CHECK(!fs::exists(dir / "usage" / name));
    }
    UsageTelemetry::shutdown();

    std::vector<std::string> names = pending_event_names(dir);
    CHECK(std::count(names.begin(), names.end(), "crash") == 1);

    // And the launch after is clean too.
    REQUIRE_NOTHROW(UsageTelemetry::start(config, false));
    UsageTelemetry::shutdown();

    set_data_dir(old_data_dir);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}


// UNLAYERED_MOD 2026-10-11 (0.1.4) -- the update check sends the install id, and only while
// diagnostics are on.
TEST_CASE("Usage diagnostics give the install id out only while on", "[UsageTelemetry]")
{
    const std::string old_data_dir = data_dir();
    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-telemetry-%%%%-%%%%");
    fs::create_directories(dir);
    set_data_dir(dir.string());

    AppConfig config;
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    grant(config);

    CHECK(UsageTelemetry::install_id().empty()); // not started yet
    UsageTelemetry::start(config, true);
    if (UsageTelemetry::disabled_by_environment()) {
        CHECK(UsageTelemetry::install_id().empty());
        UsageTelemetry::shutdown();
        set_data_dir(old_data_dir);
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");
    }
    CHECK(UsageTelemetry::install_id() == config.get("usage_diagnostics_id"));
    CHECK(UsageTelemetry::install_id().size() == 36);

    decline(config);
    UsageTelemetry::refresh(config);
    CHECK(UsageTelemetry::install_id().empty());
    UsageTelemetry::shutdown();
    CHECK(UsageTelemetry::install_id().empty());

    set_data_dir(old_data_dir);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

// Consent given while the app runs (setup, the update question, Preferences) creates the id then.
TEST_CASE("Usage diagnostics create the install id only when permitted", "[UsageTelemetry]")
{
    const std::string old_data_dir = data_dir();
    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-telemetry-%%%%-%%%%");
    fs::create_directories(dir);
    set_data_dir(dir.string());

    AppConfig config;
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    UsageTelemetry::start(config, true);
    if (UsageTelemetry::disabled_by_environment()) {
        UsageTelemetry::shutdown();
        set_data_dir(old_data_dir);
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");
    }
    CHECK(config.get("usage_diagnostics_id").empty());

    grant(config);
    UsageTelemetry::refresh(config);
    CHECK(UsageTelemetry::install_id().size() == 36);
    CHECK(config.get("usage_diagnostics_id") == UsageTelemetry::install_id());
    UsageTelemetry::shutdown();

    set_data_dir(old_data_dir);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

// Only Slicing improvement program members' batches say program:true (the site trains on nothing
// else), and nothing collected under the other answer leaves after a change.
TEST_CASE("Usage diagnostics stamp program membership and drop what was queued under the other answer", "[UsageTelemetry]")
{
    const std::string old_data_dir = data_dir();
    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-telemetry-%%%%-%%%%");
    fs::create_directories(dir);
    set_data_dir(dir.string());

    AppConfig config;
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    grant(config);
    UsageTelemetry::start(config, false);
    if (UsageTelemetry::disabled_by_environment()) {
        UsageTelemetry::shutdown();
        set_data_dir(old_data_dir);
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");
    }
    UsageTelemetry::record("slice", {{"result", "ok"}});
    UsageTelemetry::shutdown();
    {
        json batch = json::parse(json::parse(slurp(dir / "usage" / "pending.json"))[0].get<std::string>());
        CHECK(batch["program"] == false);
    }

    // Joined: the batch waiting from before (program:false) and anything queued are dropped.
    UsageTelemetry::start(config, false);
    UsageTelemetry::record("slice", {{"result", "queued before joining"}});
    PrivacyConsent::save(config, PrivacyConsent::make_choice("north_america", "CA", true, false, "preferences", "2026-10-14T12:00:00Z",
                                                             PrivacyConsent::legal_approvals(), true));
    UsageTelemetry::refresh(config);
    CHECK(!fs::exists(dir / "usage" / "pending.json"));
    UsageTelemetry::record("slice", {{"result", "ok"}});
    UsageTelemetry::shutdown();
    {
        const json pending = json::parse(slurp(dir / "usage" / "pending.json"));
        REQUIRE(pending.size() == 1);
        json batch = json::parse(pending[0].get<std::string>());
        CHECK(batch["program"] == true);
        for (const auto &e : batch["events"])
            CHECK(e["props"].value("result", "") != "queued before joining");
    }

    // Left again: what was stamped program:true never goes out as anything else.
    UsageTelemetry::start(config, false);
    grant(config);
    UsageTelemetry::refresh(config);
    CHECK(!fs::exists(dir / "usage" / "pending.json"));
    UsageTelemetry::shutdown();

    set_data_dir(old_data_dir);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Usage diagnostics report Unlayered's own copies as channel dev", "[UsageTelemetry]")
{
    // The site counts only "release" towards customers.
    CHECK(UsageTelemetry::channel_for("0.1.4", false) == "release");
    CHECK(UsageTelemetry::channel_for("0.1.5-beta1", false) == "beta");
    CHECK(UsageTelemetry::channel_for("0.1.5-RC2", false) == "beta");
    CHECK(UsageTelemetry::channel_for("0.2.0-dev", false) == "dev");
    // An internal copy is dev whatever its version says.
    CHECK(UsageTelemetry::channel_for("0.1.4", true) == "dev");
    CHECK(UsageTelemetry::channel_for("0.1.5-beta1", true) == "dev");
    // This test runs from the build tree, which no installer put there.
    CHECK(UsageTelemetry::is_internal_copy());
}

#ifdef _WIN32
// A crash names its callers, not just the faulting address. The child process (hidden test case below)
// starts diagnostics in a scratch data dir and dereferences null three calls deep; the marker it leaves
// behind is what the next launch would report.
namespace {
__declspec(noinline) void crash_level_three(volatile int *target) { *target = 1; }
__declspec(noinline) void crash_level_two(volatile int *target) { crash_level_three(target); *target = 2; }
__declspec(noinline) void crash_level_one(volatile int *target) { crash_level_two(target); *target = 3; }
} // namespace

TEST_CASE("Usage diagnostics crash child", "[.CrashChild]")
{
    char dir[MAX_PATH] = {0};
    REQUIRE(GetEnvironmentVariableA("UNLAYERED_CRASH_TEST_DIR", dir, MAX_PATH) > 0);
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS); // no "has stopped working" window
    set_data_dir(dir);
    AppConfig config;
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    grant(config);
    UsageTelemetry::start(config, false);
    UsageTelemetry::set_phase("slicing");
    crash_level_one(nullptr);
}

TEST_CASE("Usage diagnostics crash marker names the callers", "[UsageTelemetry]")
{
    if (UsageTelemetry::disabled_by_environment())
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");

    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-crash-%%%%-%%%%");
    fs::create_directories(dir / "usage");
    SetEnvironmentVariableA("UNLAYERED_CRASH_TEST_DIR", dir.string().c_str());

    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" \"[CrashChild]\"";
    STARTUPINFOW        startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
    WaitForSingleObject(process.hProcess, 60000);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    SetEnvironmentVariableA("UNLAYERED_CRASH_TEST_DIR", nullptr);
    CHECK(exit_code == 0xC0000005);

    json marker;
    for (fs::directory_iterator it(dir / "usage"), end; it != end; ++it)
        if (it->path().filename().string().rfind("session-", 0) == 0)
            marker = json::parse(slurp(it->path()), nullptr, false);
    REQUIRE(marker.is_object());
    INFO(marker.dump());
    CHECK(marker["exception"] == "0xC0000005");
    CHECK(marker["phase"] == "slicing");
    CHECK(marker["consented"] == true);
    const std::string at = marker["at"].get<std::string>();
    // The faulting frame, then its callers: all inside this test program, 3+ frames deep.
    size_t frames = 0;
    for (size_t pos = 0; (pos = at.find("slic3rutils_tests.exe+0x", pos)) != std::string::npos; ++pos)
        ++frames;
    CHECK(frames >= 3);
    CHECK(at.find('>') != std::string::npos);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

#if defined(_M_X64)
// A hang says where the main thread is stuck: its top frame (a wait inside Windows), then the
// callers from the first frame outside Windows' own modules -- here, this test program.
__declspec(noinline) void hang_here_for(DWORD ms) { ::Sleep(ms); }

TEST_CASE("Usage diagnostics hang marker names where the main thread waits", "[UsageTelemetry]")
{
    const std::string old_data_dir = data_dir();
    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-hang-%%%%-%%%%");
    fs::create_directories(dir / "usage");
    set_data_dir(dir.string());
    AppConfig config;
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    config.set("usage_diagnostics_hang_s", "1");
    grant(config);
    UsageTelemetry::start(config, false);
    if (UsageTelemetry::disabled_by_environment()) {
        UsageTelemetry::shutdown();
        set_data_dir(old_data_dir);
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");
    }
    auto marker = [&]() {
        for (fs::directory_iterator it(dir / "usage"), end; it != end; ++it)
            if (it->path().filename().string().rfind("session-", 0) == 0)
                return json::parse(slurp(it->path()), nullptr, false);
        return json();
    };
    UsageTelemetry::heartbeat(); // this thread is the one that beats
    hang_here_for(8000);         // the watchdog looks every 5 s; 1 s without a beat is a hang here

    json hung = marker();
    INFO(hung.dump());
    REQUIRE(hung.is_object());
    CHECK(hung.value("exception", std::string()) == "hang");
    const std::string at = hung.value("at", std::string());
    CHECK(at.find("slic3rutils_tests.exe+0x") != std::string::npos);
    CHECK(at.rfind("slic3rutils_tests.exe", 0) != 0); // the top frame is the wait, inside Windows
    CHECK(at.find('\\') == std::string::npos);

    // Beating again clears both.
    UsageTelemetry::heartbeat();
    json again = marker();
    CHECK(!again.contains("exception"));
    CHECK(!again.contains("at"));

    UsageTelemetry::shutdown();
    set_data_dir(old_data_dir);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}
#endif
#endif

// Real Application-log events (EvtRenderEventXml) from a Windows 11 machine, paths shortened.
TEST_CASE("Usage diagnostics read how a process ended from the Windows event log", "[UsageTelemetry]")
{
    const std::string crash_xml =
        "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='Application Error'/>"
        "<EventID>1000</EventID></System><EventData><Data Name='AppName'>unlayered-slicer.exe</Data>"
        "<Data Name='AppVersion'>2.0.0.0</Data><Data Name='AppTimeStamp'>6ab0bc35</Data>"
        "<Data Name='ModuleName'>MSVCP140.dll</Data><Data Name='ModuleVersion'>14.50.35719.0</Data>"
        "<Data Name='ModuleTimeStamp'>a3a3015d</Data><Data Name='ExceptionCode'>c0000005</Data>"
        "<Data Name='FaultingOffset'>000000000004bce1</Data><Data Name='ProcessId'>0x670c</Data>"
        "<Data Name='ProcessCreationTime'>0x1dd59ef17b4e6e1</Data><Data Name='AppPath'>C:\\app\\unlayered-slicer.exe</Data>"
        "<Data Name='ModulePath'>C:\\WINDOWS\\SYSTEM32\\MSVCP140.dll</Data><Data Name='IntegratorReportId'>x</Data>"
        "<Data Name='PackageFullName'></Data><Data Name='PackageRelativeAppId'></Data></EventData></Event>";
    const std::string hang_xml =
        "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='Application Hang'/>"
        "<EventID Qualifiers='0'>1002</EventID></System><EventData><Data Name='AppName'>unlayered-slicer.exe</Data>"
        "<Data Name='AppVersion'>2.0.0.0</Data><Data Name='ProcessId'>0x48e0</Data><Data Name='StartTime'>0x1dd4d247101f42c</Data>"
        "<Data Name='TerminationTime'>309</Data><Data Name='ExeFileName'>C:\\app\\unlayered-slicer.exe</Data>"
        "<Data Name='ReportId'>y</Data><Data Name='PackageFullName'></Data><Data Name='PackageRelativeAppId'></Data>"
        "<Data Name='HangType'>Unknown</Data></EventData></Event>";
    const int64_t crash_started = 1791771132077LL; // 0x1dd59ef17b4e6e1 as Unix ms
    const int64_t hang_started  = 1790364680109LL; // 0x1dd4d247101f42c

    std::string exception, at;
    SECTION("a crash: code and module+offset, for the same process")
    {
        REQUIRE(UsageTelemetry::match_crash_event(crash_xml, 0x670c, crash_started + 800, "unlayered-slicer.exe", exception, at));
        CHECK(exception == "0xC0000005");
        CHECK(at == "MSVCP140.dll+0x4bce1");
        // The app name is compared without case; no start time known = id only.
        CHECK(UsageTelemetry::match_crash_event(crash_xml, 0x670c, 0, "Unlayered-Slicer.EXE", exception, at));
    }
    SECTION("a hang the person closed")
    {
        REQUIRE(UsageTelemetry::match_crash_event(hang_xml, 0x48e0, hang_started, "unlayered-slicer.exe", exception, at));
        CHECK(exception == "hang_closed");
        CHECK(at.empty());
    }
    SECTION("never another process")
    {
        // Same id, but a process started an hour later reused it.
        CHECK_FALSE(UsageTelemetry::match_crash_event(crash_xml, 0x670c, crash_started + 3600 * 1000, "unlayered-slicer.exe", exception, at));
        CHECK_FALSE(UsageTelemetry::match_crash_event(crash_xml, 0x670d, crash_started, "unlayered-slicer.exe", exception, at));
        CHECK_FALSE(UsageTelemetry::match_crash_event(crash_xml, 0x670c, crash_started, "other.exe", exception, at));
        CHECK_FALSE(UsageTelemetry::match_crash_event("<Event><System><EventID>1001</EventID></System></Event>", 1, 0,
                                                      "unlayered-slicer.exe", exception, at));
    }
}
// 0.1.6: nothing is collected, queued or kept before a privacy choice, and a withdrawal discards
// whatever was waiting. (Replaces 0.1.6's earlier "held until answered", which collected first.)
TEST_CASE("Usage diagnostics collect nothing before a choice and drop everything on withdrawal", "[UsageTelemetry]")
{
    const std::string old_data_dir = data_dir();
    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-telemetry-%%%%-%%%%");
    fs::create_directories(dir);
    set_data_dir(dir.string());

    AppConfig config;
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    // An old config's default "on" is not a choice.
    config.set_bool("usage_diagnostics", true);

    UsageTelemetry::start(config, false);
    if (UsageTelemetry::disabled_by_environment()) {
        UsageTelemetry::shutdown();
        set_data_dir(old_data_dir);
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");
    }
    CHECK(!UsageTelemetry::enabled());
    CHECK(UsageTelemetry::install_id().empty());
    CHECK(!config.get_bool("usage_diagnostics")); // the mirror follows the (missing) choice
    // No installation id exists before diagnostics are permitted.
    CHECK(config.get("usage_diagnostics_id").empty());
    UsageTelemetry::record("slice", {{"result", "ok"}});
    UsageTelemetry::count("tab:preview");
    UsageTelemetry::flush();
    UsageTelemetry::shutdown();
    CHECK(!fs::exists(dir / "usage" / "pending.json"));
    CHECK(config.get("usage_diagnostics_id").empty());

    // A crash in a session that never had consent is not reported once consent is given.
    {
        std::ofstream marker((dir / "usage" / "session-before.json").string());
        marker << json{{"pid", 0x7ffffff0}, {"version", "0.1.5"}, {"started", 1000}, {"alive", 61000}, {"phase", "slicing"}}.dump();
    }
    grant(config);
    UsageTelemetry::start(config, false);
    CHECK(UsageTelemetry::enabled());
    CHECK(UsageTelemetry::install_id().size() == 36);
    const std::string install = config.get("usage_diagnostics_id"); // created by the consent
    CHECK(install == UsageTelemetry::install_id());
    UsageTelemetry::record("slice", {{"result", "ok"}});
    UsageTelemetry::shutdown(); // nothing listens: kept for the next launch
    CHECK(pending_event_names(dir) == std::vector<std::string>{"slice", "session_end"});

    // Withdrawn on the next launch: everything waiting is gone and nothing new is queued.
    UsageTelemetry::start(config, false);
    decline(config);
    UsageTelemetry::refresh(config);
    CHECK(!UsageTelemetry::enabled());
    CHECK(!fs::exists(dir / "usage" / "pending.json"));
    UsageTelemetry::record("slice", {{"result", "ok"}});
    UsageTelemetry::shutdown();
    CHECK(!fs::exists(dir / "usage" / "pending.json"));
    CHECK(!config.get_bool("usage_diagnostics"));
    // The id is kept after a withdrawal (for a privacy request) but no longer given out.
    CHECK(config.get("usage_diagnostics_id") == install);
    CHECK(UsageTelemetry::install_id().empty());

    // A decline survives a restart (and an update: the record does not depend on the version).
    UsageTelemetry::start(config, false);
    CHECK(!UsageTelemetry::enabled());
    UsageTelemetry::shutdown();

    set_data_dir(old_data_dir);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

// The organisation-wide switch wins over any choice.
TEST_CASE("Usage diagnostics obey UNLAYERED_NO_TELEMETRY over a granted choice", "[UsageTelemetry]")
{
    AppConfig config;
    grant(config);
    CHECK(!PrivacyConsent::diagnostics_permitted(PrivacyConsent::load(config), true, PrivacyConsent::legal_approvals()));
    CHECK(PrivacyConsent::diagnostics_permitted(config) == !PrivacyConsent::organization_opt_out());
}

// Events and properties not on the allowlist never reach a batch.
TEST_CASE("Usage diagnostics send only allowlisted events and properties", "[UsageTelemetry]")
{
    const std::string old_data_dir = data_dir();
    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-telemetry-%%%%-%%%%");
    fs::create_directories(dir);
    set_data_dir(dir.string());

    AppConfig config;
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    grant(config);
    UsageTelemetry::start(config, false);
    if (UsageTelemetry::disabled_by_environment()) {
        UsageTelemetry::shutdown();
        set_data_dir(old_data_dir);
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");
    }
    UsageTelemetry::record("model_upload", {{"file", "bracket.stl"}});
    UsageTelemetry::record("slice", {{"result", "ok"},
                                     {"project", "Secret bracket"},
                                     {"printer", "C:/Users/alex/my.3mf"},
                                     {"settings", std::vector<std::string>{"layer_height=0.2", "sparse_infill_svg_path=logo.svg"}}});
    UsageTelemetry::count("file:bracket.stl");
    UsageTelemetry::shutdown();

    std::vector<json> events;
    const json batches = json::parse(slurp(dir / "usage" / "pending.json"));
    for (const auto &body : batches) {
        // Named, not iterated as a temporary: operator[] on a temporary would dangle.
        const json batch = json::parse(body.get<std::string>());
        for (const auto &e : batch["events"])
            events.push_back(e);
    }
    REQUIRE(events.size() == 2); // slice, session_end
    const json &slice = events[0];
    CHECK(slice["name"] == "slice");
    CHECK(slice["props"]["result"] == "ok");
    CHECK(!slice["props"].contains("project"));
    CHECK(!slice["props"].contains("printer"));
    CHECK(slice["props"]["settings"] == json::array({"layer_height=0.2"}));
    CHECK(events[1]["counts"].empty());

    set_data_dir(old_data_dir);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

// 0.1.6: slice events carry setting values and part measurements, so batches are cut by size too:
// the site rejects a body over 64 KB (and the whole batch with it).
TEST_CASE("Usage diagnostics keep every batch under the site's size limit", "[UsageTelemetry]")
{
    const std::string old_data_dir = data_dir();
    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-telemetry-%%%%-%%%%");
    fs::create_directories(dir);
    set_data_dir(dir.string());

    AppConfig config;
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    grant(config);
    UsageTelemetry::start(config, false);
    if (UsageTelemetry::disabled_by_environment()) {
        UsageTelemetry::shutdown();
        set_data_dir(old_data_dir);
        SKIP("UNLAYERED_NO_TELEMETRY is set on this machine");
    }
    std::vector<std::string> settings, parts;
    for (int i = 0; i < 60; ++i)
        settings.push_back("setting_with_a_long_name_" + std::to_string(i) + "=" + std::string(40, 'v'));
    for (int i = 0; i < 20; ++i)
        parts.push_back("x=123.4;y=123.4;z=123.4;vol=1234567;area=123456;ovh=12.5;base=34.0;inst=20");
    for (int i = 0; i < 30; ++i)
        UsageTelemetry::record("slice", {{"result", "ok"}, {"settings", settings}, {"parts", parts}});
    // Nothing listens: every batch is kept on disk, so the test can read them back.
    UsageTelemetry::shutdown();

    json batches = json::parse(slurp(dir / "usage" / "pending.json"), nullptr, false);
    REQUIRE(batches.is_array());
    CHECK(batches.size() > 1);
    size_t events = 0;
    for (const auto &body : batches) {
        CHECK(body.get<std::string>().size() < 64 * 1024);
        events += json::parse(body.get<std::string>())["events"].size();
    }
    CHECK(events == 31); // and session_end

    set_data_dir(old_data_dir);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}
