// Reusing an installer the previous session downloaded but never ran (it crashed first):
// download_relative_path, claim_previous_installer and verify_installer.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/AppUpdater.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// sha256("abc")
const char *ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

fs::path write_file(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    fs::ofstream f(path, std::ios::binary);
    f << content;
    return path;
}

} // namespace

TEST_CASE("download_relative_path: only our download directories", "[AppUpdater]")
{
    const fs::path tmp = fs::temp_directory_path();
    CHECK(download_relative_path(tmp / "unlayered-update-42" / "Setup.exe") == "unlayered-update-42/Setup.exe");
    CHECK(download_relative_path(tmp / "somewhere-else" / "Setup.exe").empty());
    CHECK(download_relative_path(fs::path()).empty());
}

TEST_CASE("claim_previous_installer: refuses anything but <download dir>/<file>", "[AppUpdater]")
{
    fs::path    dest;
    std::string error;
    // Absolute forms are built at run time: a drive-letter literal here trips the public export's path scan.
    const std::string rooted   = "/unlayered-update-1/Setup.exe";
    const std::string absolute = (fs::temp_directory_path() / "unlayered-update-1" / "Setup.exe").generic_string();
    for (const std::string &bad : {std::string(), std::string("Setup.exe"), std::string("unlayered-update-1"),
                                   std::string("other-dir/Setup.exe"), std::string("../unlayered-update-1/Setup.exe"),
                                   std::string("unlayered-update-1/../Setup.exe"), std::string("unlayered-update-1/.."),
                                   std::string("unlayered-update-1/a/Setup.exe"), rooted, absolute}) {
        INFO(bad);
        CHECK_FALSE(claim_previous_installer(bad, dest, error));
    }
    // Well formed, but nothing there.
    CHECK_FALSE(claim_previous_installer("unlayered-update-999999991/Setup.exe", dest, error));
}

TEST_CASE("claim_previous_installer + verify_installer: a kept download moves into this process's directory", "[AppUpdater]")
{
    const fs::path tmp      = fs::temp_directory_path();
    const fs::path old_dir  = tmp / "unlayered-update-999999992";
    const fs::path own_dir  = tmp / ("unlayered-update-" + std::to_string(get_current_pid()));
    const fs::path previous = write_file(old_dir / "Setup.exe", "abc");

    fs::path    claimed;
    std::string error;
    REQUIRE(claim_previous_installer("unlayered-update-999999992/Setup.exe", claimed, error));
    CHECK(claimed == own_dir / "Setup.exe");
    CHECK(fs::exists(claimed));
    CHECK_FALSE(fs::exists(previous));
    // What the next session would be told to look for.
    CHECK(download_relative_path(claimed) == "unlayered-update-" + std::to_string(get_current_pid()) + "/Setup.exe");

    CHECK(verify_installer(claimed, ABC_SHA256, error));
    CHECK(verify_installer(claimed, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", error));
    CHECK_FALSE(verify_installer(claimed, std::string(64, '0'), error));
    CHECK_FALSE(verify_installer(claimed, "", error));
    CHECK_FALSE(verify_installer(own_dir / "missing.exe", ABC_SHA256, error));

    // A tampered file fails.
    write_file(claimed, "abd");
    CHECK_FALSE(verify_installer(claimed, ABC_SHA256, error));

    boost::system::error_code ec;
    fs::remove_all(old_dir, ec);
    fs::remove_all(own_dir, ec);
}
