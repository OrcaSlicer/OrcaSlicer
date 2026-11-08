#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/CrashLogScrub.hpp"
#include "slic3r/Utils/PrivacyConsent.hpp"
#include "slic3r/Utils/UsageTelemetry.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;
using json   = nlohmann::json;

// Unlayered 0.1.10: the end of a crashed session's log is sent with its crash report, scrubbed on
// the person's computer first (CrashLogScrub).

namespace {

ScrubContext jane()
{
    ScrubContext c;
    c.home_dirs  = {R"(C:\Users\Jane Smith)"};
    c.user_names = {"jsmith", "Jane Smith"};
    c.host_names = {"JANE-PC"};
    return c;
}

bool contains(const std::string &s, const std::string &part) { return s.find(part) != std::string::npos; }

void write_text(const fs::path &path, const std::string &text)
{
    std::ofstream out(path.string(), std::ios::binary);
    out << text;
}

} // namespace

TEST_CASE("Crash log scrub removes the home folder in every spelling", "[CrashLogScrub]")
{
    const ScrubContext c = jane();
    CHECK(scrub_log_for_upload(R"(load C:\Users\Jane Smith\Desktop\notes.txt)", c) == R"(load <home>\Desktop\notes.txt)");
    CHECK(scrub_log_for_upload(R"(dir c:/users/jane smith/AppData)", c) == "dir <home>/AppData");
    CHECK(scrub_log_for_upload(R"({"path":"C:\\Users\\Jane Smith\\AppData"})", c) == R"({"path":"<home>\\AppData"})");
    // Somebody else's profile folder, and macOS / Linux homes, without this computer's names.
    CHECK(scrub_log_for_upload(R"(C:\Users\Bob\Documents\x.txt)", c) == R"(<home>\Documents\x.txt)");
    CHECK(scrub_log_for_upload("/Users/kate/Library/x.txt and /home/tux/.config", c) == "<home>/Library/x.txt and <home>/.config");
    CHECK(scrub_log_for_upload(R"(%USERPROFILE%\AppData)", c) == R"(<home>\AppData)");
}

TEST_CASE("Crash log scrub removes names, e-mails, secrets, addresses and file names", "[CrashLogScrub]")
{
    const ScrubContext c = jane();
    SECTION("the login name and the computer name, anywhere")
    {
        CHECK(scrub_log_for_upload("user jsmith on JANE-PC", c) == "user <user> on <host>");
        CHECK(scrub_log_for_upload("device label Jane Smith's jane-pc", c) == "device label <user>'s <host>");
    }
    SECTION("e-mail addresses")
    {
        CHECK(scrub_log_for_upload("signed in as jane.smith+3d@example.co.uk", c) == "signed in as <email>");
        // A preset name with an @ is not an address.
        CHECK(scrub_log_for_upload("Generic PLA @System", c) == "Generic PLA @System");
    }
    SECTION("credentials")
    {
        CHECK(scrub_log_for_upload("Authorization: Bearer abc.def-ghi_123", c) == "Authorization: <redacted> <redacted>");
        CHECK(scrub_log_for_upload("GET /x?access_token=zzz&page=2", c) == "GET /x?access_token=<redacted>&page=2");
        CHECK(scrub_log_for_upload(R"({"api_key": "s3cr3t", "password":"hunter2"})", c) ==
              R"({"api_key": "<redacted>", "password":"<redacted>"})");
        CHECK(scrub_log_for_upload("token=abcd1234 key=AbC123", c) == "token=<redacted> key=<redacted>");
        CHECK(scrub_log_for_upload("url https://bob:pw@printer.example/x", c) == "url https://<redacted>@printer.example/x");
        // A setting's name is not a key; "metadata key = Title" names a field.
        CHECK(scrub_log_for_upload("Print::apply, i=0, key=wall_loops", c) == "Print::apply, i=0, key=wall_loops");
        CHECK(scrub_log_for_upload("metadata key = Title, value = x", c) == "metadata key = Title, value = x");
    }
    SECTION("long hex / base64 runs, but not long setting names")
    {
        CHECK(scrub_log_for_upload("sha 0123456789abcdef0123456789abcdef01234567", c) == "sha <redacted>");
        CHECK(scrub_log_for_upload("support_interface_bottom_layers_extra", c) == "support_interface_bottom_layers_extra");
    }
    SECTION("IP addresses, but not loopback, times, versions or C++ names")
    {
        CHECK(scrub_log_for_upload("upload to 192.0.2.10 ok", c) == "upload to <ip> ok");
        CHECK(scrub_log_for_upload("listen 127.0.0.1", c) == "listen 127.0.0.1");
        CHECK(scrub_log_for_upload("v6 fe80::1c2b:3a4d:5e6f:7a8b", c) == "v6 <ip>");
        CHECK(scrub_log_for_upload("at 13:45:22.123 in Slic3r::GUI::Plater ::1", c) == "at 13:45:22.123 in Slic3r::GUI::Plater ::1");
        CHECK(scrub_log_for_upload("plugin 01.09.07.52", c) == "plugin 01.09.07.52");
    }
    SECTION("model, project, G-code and image file names keep only their extension")
    {
        CHECK(scrub_log_for_upload(R"(load <x>\My Part v2.3mf ok)", c) == R"(load <x>\<file>.3mf ok)");
        CHECK(scrub_log_for_upload("export /tmp/benchy.gcode.3mf", c) == "export /tmp/<file>.gcode.3mf");
        CHECK(scrub_log_for_upload(R"(open "My Model.obj" and plate_1.PNG)", c) == R"(open "<file>.obj" and <file>.PNG)");
        CHECK(scrub_log_for_upload("bracket.STL, part.step, part.stp, a.amf, b.svg, c.jpg, d.gcode", c) ==
              "<file>.STL, <file>.step, <file>.stp, <file>.amf, <file>.svg, <file>.jpg, <file>.gcode");
        // Not a file: an identifier that merely starts like an extension.
        CHECK(scrub_log_for_upload("ModelObject.objects", c) == "ModelObject.objects");
    }
}

TEST_CASE("Crash log scrub keeps the shape of the log", "[CrashLogScrub]")
{
    const ScrubContext c = jane();
    // Line endings stay, replacements never collide with a name ("home" as a user name).
    ScrubContext home_user = c;
    home_user.user_names.push_back("home");
    CHECK(scrub_log_for_upload("a\r\nC:\\Users\\Jane Smith\\x\nlast", home_user) == "a\r\n<home>\\x\nlast");
    // A line too long to be worth the risk is dropped whole.
    CHECK(scrub_log_for_upload(std::string(9000, 'a') + "\nok", c) == "<long line removed>\nok");
    // The scrubber's own markers in the input do not survive.
    CHECK(scrub_log_for_upload(std::string("x\x01H\x02y"), c) == "xHy");
}

TEST_CASE("Crash log tail starts at a complete line", "[CrashLogScrub]")
{
    fs::path dir = fs::temp_directory_path() / fs::unique_path("crash-log-%%%%-%%%%");
    fs::create_directories(dir);
    write_text(dir / "small.log", "one\ntwo\n");
    write_text(dir / "big.log", "first line\nsecond\nthird\n");

    LogTail tail;
    REQUIRE(read_log_tail((dir / "small.log").string(), tail, 100));
    CHECK(tail.text == "one\ntwo\n");
    CHECK(tail.original_bytes == 8);
    CHECK_FALSE(tail.truncated);

    // The last 10 bytes are "ond\nthird\n": the partial "ond" line is dropped.
    REQUIRE(read_log_tail((dir / "big.log").string(), tail, 10));
    CHECK(tail.text == "third\n");
    CHECK(tail.original_bytes == 24);
    CHECK(tail.truncated);

    CHECK_FALSE(read_log_tail((dir / "missing.log").string(), tail, 10));

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Crash log finds the crashed session's log file", "[CrashLogScrub]")
{
    fs::path dir = fs::temp_directory_path() / fs::unique_path("crash-log-%%%%-%%%%");
    fs::create_directories(dir);
    write_text(dir / "debug_Mon_Nov_02_10_00_00_1234.log.0", "a");
    write_text(dir / "debug_Mon_Nov_02_10_00_00_1234.log.1", "b"); // rotated: the newer part
    write_text(dir / "debug_Mon_Nov_02_11_00_00_999.log.0", "c");
    const std::string d = dir.string();

    CHECK(fs::path(find_session_log(d, "debug_Mon_Nov_02_10_00_00_1234.log.0", 0, 0)).filename() ==
          "debug_Mon_Nov_02_10_00_00_1234.log.1");
    // A marker from before 0.1.10 names no file: found by its process id.
    CHECK(fs::path(find_session_log(d, "", 999, 0)).filename() == "debug_Mon_Nov_02_11_00_00_999.log.0");
    CHECK(find_session_log(d, "", 4242, 0).empty());
    CHECK(find_session_log(d, "debug_other.log.0", 0, 0).empty());
    // Never a path from the marker.
    CHECK(find_session_log(d, "../x.log.0", 0, 0).empty());
    CHECK(find_session_log((dir / "nope").string(), "", 999, 0).empty());

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Crash log goes to the same site as usage diagnostics", "[CrashLogScrub]")
{
    CHECK(crash_log_url("https://unlayered3d.com/api/slicer/telemetry") == "https://unlayered3d.com/api/slicer/crash-log");
    CHECK(crash_log_url("http://127.0.0.1:3000/api/slicer/telemetry?x=1") == "http://127.0.0.1:3000/api/slicer/crash-log");
    CHECK(crash_log_url("https://example.com") == "https://example.com/api/slicer/crash-log");
    CHECK(crash_log_url("not a url").empty());
    CHECK(crash_log_url("https://").empty());
}

// The crash event carries the report id its log is sent with.
TEST_CASE("Crash reports carry a report id", "[CrashLogScrub][UsageTelemetry]")
{
    const std::string old_data_dir = data_dir();
    fs::path dir = fs::temp_directory_path() / fs::unique_path("usage-crashlog-%%%%-%%%%");
    fs::create_directories(dir / "usage");
    set_data_dir(dir.string());

    AppConfig config;
    config.set("usage_diagnostics_url", "http://127.0.0.1:9/api/slicer/telemetry");
    // No privacy choice: the crash is offered to the person rather than sent.
    write_text(dir / "usage" / "session-dead.json",
               json{{"pid", 0x7ffffff0}, {"version", "0.1.10"}, {"started", 1000}, {"alive", 61000}, {"phase", "slicing"},
                    {"log", "debug_Mon_Nov_02_10_00_00_1234.log.0"}}
                   .dump());

    UsageTelemetry::start(config, false);
    if (UsageTelemetry::disabled_by_environment() || !PrivacyConsent::legal_approvals().crash_report_prompt) {
        UsageTelemetry::shutdown();
        set_data_dir(old_data_dir);
        SKIP("crash reports are not offered on this machine");
    }
    const std::vector<json> offers = UsageTelemetry::unreported_crashes();
    REQUIRE(!offers.empty());
    CHECK(offers.back().value("report_id", std::string()).size() == 36);
    CHECK(offers.back()["phase"] == "slicing");
    // Declined: forgotten, report and log alike.
    UsageTelemetry::answer_unreported_crashes(false);
    CHECK(UsageTelemetry::unreported_crashes().empty());
    UsageTelemetry::shutdown();

    set_data_dir(old_data_dir);
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}
