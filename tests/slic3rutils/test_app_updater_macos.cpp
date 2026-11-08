// The macOS half of the self-updater. These exercise the selection rules and, more importantly,
// the shell script launch_silent_update() generates -- the step that actually replaces the
// application, and the one a quoting slip would break silently.
#ifdef __APPLE__

#include <catch2/catch_test_macros.hpp>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <sstream>

#include "slic3r/Utils/AppUpdater.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// A path with a space in it, because install locations have them and shell quoting is the
// failure this file exists to catch.
fs::path make_temp_dir()
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("updater test %%%%");
    fs::create_directories(dir);
    return dir;
}

fs::path make_fake_bundle(const fs::path &parent, const std::string &name)
{
    const fs::path app = parent / name;
    fs::create_directories(app / "Contents" / "MacOS");
    boost::filesystem::ofstream(app / "Contents" / "Info.plist") << "<plist/>\n";
    return app;
}

std::string read_file(const fs::path &p)
{
    boost::filesystem::ifstream in(p);
    std::ostringstream          ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("the updater names this platform", "[AppUpdaterMac]")
{
    CHECK(std::string(updater_platform()) == "mac");
    const std::string arch = updater_arch();
    CHECK((arch == "arm64" || arch == "x64"));
}

TEST_CASE("asset selection takes the disk image and nothing else", "[AppUpdaterMac]")
{
    SECTION("a universal image is accepted whatever this architecture is") {
        const std::vector<AppUpdateAsset> assets{
            {"Unlayered_Slicer_Mac_universal_0.1.2.dmg", "https://example.invalid/a.dmg", "", 0}};
        const auto picked = select_platform_asset(assets);
        REQUIRE(picked.has_value());
        CHECK(picked->name == "Unlayered_Slicer_Mac_universal_0.1.2.dmg");
    }

    SECTION("the things published beside the installer are skipped") {
        const std::vector<AppUpdateAsset> assets{
            {"Unlayered_Slicer_profile_validator_Mac_universal.dmg", "https://example.invalid/v.dmg", "", 0},
            {"Unlayered_Slicer_Mac_universal_0.1.2.dmg", "https://example.invalid/a.dmg", "", 0}};
        const auto picked = select_platform_asset(assets);
        REQUIRE(picked.has_value());
        CHECK(picked->name == "Unlayered_Slicer_Mac_universal_0.1.2.dmg");
    }

    SECTION("a Windows installer is never selected here") {
        const std::vector<AppUpdateAsset> assets{
            {"Unlayered_Slicer_Windows_0.1.2.exe", "https://example.invalid/a.exe", "", 0}};
        CHECK_FALSE(select_platform_asset(assets).has_value());
    }

    SECTION("an image naming only the other architecture is refused") {
        const std::string other = std::string(updater_arch()) == "arm64" ? "x86_64" : "arm64";
        const std::vector<AppUpdateAsset> assets{
            {"Unlayered_Slicer_Mac_" + other + "_0.1.2.dmg", "https://example.invalid/a.dmg", "", 0}};
        CHECK_FALSE(select_platform_asset(assets).has_value());
    }

    SECTION("no assets is a normal, empty answer") {
        CHECK_FALSE(select_platform_asset({}).has_value());
    }
}

TEST_CASE("account downloads match platform, architecture and checksum", "[AppUpdaterMac]")
{
    const std::string sha(64, 'a');

    SECTION("a mac entry marked universal is accepted") {
        const std::vector<AccountDownload> d{{"mac", "universal", "a.dmg", "https://example.invalid/a.dmg", sha, 1}};
        CHECK(select_account_download(d).has_value());
    }

    SECTION("a windows entry is not") {
        const std::vector<AccountDownload> d{{"windows", "x64", "a.exe", "https://example.invalid/a.exe", sha, 1}};
        CHECK_FALSE(select_account_download(d).has_value());
    }

    SECTION("an entry without a usable sha256 is refused, so nothing unverified is ever run") {
        const std::vector<AccountDownload> d{{"mac", "universal", "a.dmg", "https://example.invalid/a.dmg", "abc", 1}};
        CHECK_FALSE(select_account_download(d).has_value());
    }
}

TEST_CASE("a build-tree bundle is not a self-updatable install", "[AppUpdaterMac]")
{
    // CMake builds this test as slic3rutils_tests.app, so the executable really is inside a
    // bundle -- it just is not an installed one. The updater must decline and leave the user on
    // the download page rather than overwrite anything in a build tree.
    CHECK_FALSE(self_updatable_install_dir().has_value());
}

TEST_CASE("launch_silent_update refuses a target that is not an app bundle", "[AppUpdaterMac]")
{
    const fs::path dir = make_temp_dir();
    std::string    error;
    CHECK_FALSE(launch_silent_update(dir / "update.dmg", dir / "not-a-bundle", false, error));
    CHECK_FALSE(error.empty());
    fs::remove_all(dir);
}

TEST_CASE("the generated update script is correct and correctly quoted", "[AppUpdaterMac]")
{
    const fs::path dir = make_temp_dir();          // contains a space
    const fs::path app = make_fake_bundle(dir, "Unlayered Slicer.app"); // and so does this
    const fs::path dl  = dir / "download dir";
    fs::create_directories(dl);
    const fs::path dmg = dl / "update.dmg";
    boost::filesystem::ofstream(dmg) << "not a real image";

    std::string error;
    REQUIRE(launch_silent_update(dmg, app, /*relaunch=*/true, error));
    CHECK(error.empty());

    const fs::path script = dl / "install-update.sh";
    REQUIRE(fs::exists(script));
    const std::string sh = read_file(script);

    // Paths carry spaces, so they must appear single-quoted, not bare.
    CHECK(sh.find("'" + app.string() + "'") != std::string::npos);
    CHECK(sh.find("'" + dmg.string() + "'") != std::string::npos);

    // The steps that make the swap safe.
    CHECK(sh.find("hdiutil attach") != std::string::npos);
    CHECK(sh.find("trap ") != std::string::npos);          // the image is always detached
    CHECK(sh.find("ditto ") != std::string::npos);         // signature and xattrs survive
    CHECK(sh.find(".update-staging") != std::string::npos); // staged, not written in place
    CHECK(sh.find("update-old") != std::string::npos);      // old bundle kept until the swap lands
    CHECK(sh.find("kill -0") != std::string::npos);        // waits for this process to exit

    // Rollback: a failed move must put the old bundle back.
    CHECK(sh.find("|| { mv") != std::string::npos);

    // relaunch=true asks for the app to be started again.
    CHECK(sh.find("\nopen ") != std::string::npos);

    // ...and relaunch=false does not.
    const fs::path dl2 = dir / "download dir 2";
    fs::create_directories(dl2);
    const fs::path dmg2 = dl2 / "update.dmg";
    boost::filesystem::ofstream(dmg2) << "not a real image";
    REQUIRE(launch_silent_update(dmg2, app, /*relaunch=*/false, error));
    const std::string sh2 = read_file(dl2 / "install-update.sh");
    CHECK(sh2.find("\nopen ") == std::string::npos);

    // The script is runnable.
    CHECK((fs::status(script).permissions() & fs::owner_exe) != 0);

    fs::remove_all(dir);
}

#endif // __APPLE__
