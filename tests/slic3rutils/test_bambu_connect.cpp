#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/Utils/BambuConnect.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <wx/uri.h>

using namespace Slic3r;
namespace fs = boost::filesystem;

TEST_CASE("Bambu Connect query values encode reserved characters once", "[BambuConnect]")
{
    CHECK(BambuConnect::encode_query_value("abcXYZ019-._~") == "abcXYZ019-._~");
    CHECK(BambuConnect::encode_query_value("a b") == "a%20b");
    CHECK(BambuConnect::encode_query_value("#&+%/:?=") == "%23%26%2B%25%2F%3A%3F%3D");
    // UTF-8 bytes, upper-case hex
    CHECK(BambuConnect::encode_query_value("Jos\xC3\xA9") == "Jos%C3%A9");
    // an already encoded value is encoded again, never passed through
    CHECK(BambuConnect::encode_query_value("%20") == "%2520");
}

TEST_CASE("Bambu Connect import URI matches the plan's fixtures", "[BambuConnect]")
{
    CHECK(BambuConnect::build_import_uri(fs::path("/Users/test/Connect Jobs/part #1.gcode.3mf"), "Test part #1") ==
          "bambu-connect://import-file?path=%2FUsers%2Ftest%2FConnect%20Jobs%2Fpart%20%231.gcode.3mf&name=Test%20part%20%231&version=1.0.0");
    CHECK(BambuConnect::build_import_uri(fs::path("C:/Users/Test User/Unlayered Jobs/bracket #1.gcode.3mf"), "bracket #1") ==
          "bambu-connect://import-file?path=C%3A%2FUsers%2FTest%20User%2FUnlayered%20Jobs%2Fbracket%20%231.gcode.3mf&name=bracket%20%231&version=1.0.0");
#ifdef _WIN32
    // backslashes become forward slashes
    CHECK(BambuConnect::build_import_uri(fs::path("C:\\Jobs\\a.gcode.3mf"), "a") ==
          "bambu-connect://import-file?path=C%3A%2FJobs%2Fa.gcode.3mf&name=a&version=1.0.0");
#endif
}

TEST_CASE("Bambu Connect URI values round-trip through a URI parser", "[BambuConnect]")
{
    const std::string name = GENERATE(as<std::string>{}, "plain", "with space", "a&b=c", "50% + more", "#1 \xE8\xB5\x84\xE6\xBA\x90");
    const std::string uri  = BambuConnect::build_import_uri(fs::path("/tmp/x.gcode.3mf"), name);
    const wxURI       parsed(wxString::FromUTF8(uri));
    CHECK(parsed.GetScheme() == "bambu-connect");
    CHECK(parsed.GetServer() == "import-file");
    CHECK(!parsed.HasFragment());

    // exactly three fields: the value's own & and = never split it
    const std::string query = parsed.GetQuery().utf8_string();
    size_t amps = 0;
    for (char c : query)
        amps += c == '&';
    CHECK(amps == 2);
    const size_t start = query.find("&name=") + 6;
    const size_t end   = query.find("&version=");
    CHECK(wxURI::Unescape(wxString::FromUTF8(query.substr(start, end - start))).utf8_string() == name);
}

TEST_CASE("Bambu Connect hashes the embedded G-code of a package", "[BambuConnect]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("bambu-connect-test-%%%%-%%%%");
    fs::create_directories(dir);
    const fs::path package = dir / "job.gcode.3mf";
    {
        mz_zip_archive zip;
        mz_zip_zero_struct(&zip);
        REQUIRE(open_zip_writer(&zip, package.string()));
        const std::string gcode = "G28\n";
        const std::string meta  = "<config/>";
        REQUIRE(mz_zip_writer_add_mem(&zip, "Metadata/plate_2.gcode", gcode.data(), gcode.size(), MZ_DEFAULT_COMPRESSION));
        REQUIRE(mz_zip_writer_add_mem(&zip, "Metadata/model_settings.config", meta.data(), meta.size(), MZ_DEFAULT_COMPRESSION));
        REQUIRE(mz_zip_writer_finalize_archive(&zip));
        REQUIRE(close_zip_writer(&zip));
    }

    std::string error;
    const auto  entries = BambuConnect::gcode_entries(package, error);
    CHECK(error.empty());
    REQUIRE(entries.size() == 1);
    // not assumed to be plate_1
    CHECK(entries[0].name == "Metadata/plate_2.gcode");
    CHECK(entries[0].bytes == 4);
    // sha256("G28\n")
    CHECK(entries[0].sha256 == "8080cd1d636d28b227689516fdadfd2aaed94e57b113db13523a38b51d0d09e9");
    CHECK(BambuConnect::sha256_file(package).size() == 64);

    fs::remove_all(dir);
}

TEST_CASE("A package without G-code is refused", "[BambuConnect]")
{
    std::string error;
    CHECK(BambuConnect::gcode_entries(fs::path("does-not-exist.gcode.3mf"), error).empty());
    CHECK(!error.empty());
}

// 0.1.10: job.json said "ready" after a launch that the log called "hand-off requested".
TEST_CASE("Bambu Connect records each launch attempt in job.json", "[BambuConnect]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("bambu-connect-job-%%%%-%%%%");
    fs::create_directories(dir);
    auto slurp = [&dir]() {
        std::ifstream in((dir / "job.json").string(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    CHECK(!BambuConnect::record_launch_attempt(dir, "handoff_requested")); // no record yet

    BambuConnect::JobRecord record;
    record.job_id        = "20261103-010000-abcd";
    record.display_name  = "Cube";
    record.handoff_state = "ready";
    BambuConnect::write_job_record(dir, record);

    REQUIRE(BambuConnect::record_launch_attempt(dir, "handoff_requested"));
    std::string text = slurp();
    CHECK(text.find("\"handoff_state\": \"handoff_requested\"") != std::string::npos);
    CHECK(text.find("\"last_launch_attempt_at\": \"20") != std::string::npos);
    // The rest of the record is kept.
    CHECK(text.find("\"job_id\": \"20261103-010000-abcd\"") != std::string::npos);
    CHECK(text.find("\"display_name\": \"Cube\"") != std::string::npos);

    // A reopened job that fails to launch updates the same record.
    REQUIRE(BambuConnect::record_launch_attempt(dir, "launch_failed"));
    CHECK(slurp().find("\"handoff_state\": \"launch_failed\"") != std::string::npos);

    // A cut-off record is refused, not overwritten with a stub.
    {
        std::ofstream out((dir / "job.json").string(), std::ios::binary | std::ios::trunc);
        out << "{\"job_id\": \"x";
    }
    CHECK(!BambuConnect::record_launch_attempt(dir, "handoff_requested"));
    CHECK(slurp() == "{\"job_id\": \"x");

    fs::remove_all(dir);
}

TEST_CASE("Open in Bambu Connect is on by default, unless the person turned it off", "[BambuConnect]")
{
    using BambuConnect::is_enabled;
#if defined(_WIN32) || defined(__APPLE__)
    // Never set: on, installed or not (without Connect the item opens the guide).
    CHECK(is_enabled(""));
    // Set by the person: wins, in both saved spellings.
    CHECK_FALSE(is_enabled("0"));
    CHECK_FALSE(is_enabled("false"));
    CHECK(is_enabled("1"));
    CHECK(is_enabled("true"));
#else
    // No Bambu Connect app exists here.
    CHECK_FALSE(is_enabled(""));
    CHECK_FALSE(is_enabled("1"));
#endif
}

#ifdef __APPLE__
// Test D08 on a Mac without Bambu Connect: the launcher answers NoHandler and opens nothing. Hidden, since the
// answer depends on what the Mac has installed: run it with "[BambuConnectNoHandler]" where Connect is absent.
TEST_CASE("Bambu Connect on a Mac without Connect: launch() is NoHandler", "[.][BambuConnectNoHandler]")
{
    REQUIRE(BambuConnect::mac_handler_app_path().empty());
    const std::string uri = BambuConnect::build_import_uri(fs::path("/Users/test/Connect Jobs/part #1.gcode.3mf"), "Test part #1");
    CHECK(BambuConnect::launch(uri) == BambuConnect::LaunchResult::NoHandler);
}
#endif
